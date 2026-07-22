#include "textureimage.h"
#include "texturearchive.h"
#include "detex.h"
#define TINYEXR_USE_MINIZ 0
#include "tinyexr.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QColorSpace>
#include <QSettings>
#include <QSet>
#include <QVector3D>
#include <QVector4D>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace TextureImage {
namespace {

quint32 u32(const QByteArray& data, int offset)
{
    if (offset < 0 || offset + 4 > data.size()) return 0;
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(data.constData() + offset));
}

QColor color565(quint16 value)
{
    const int r = ((value >> 11) & 31) * 255 / 31;
    const int g = ((value >> 5) & 63) * 255 / 63;
    const int b = (value & 31) * 255 / 31;
    return QColor(r, g, b, 255);
}

void decodeColorBlock(const uchar* block, QColor colors[16], bool allowTransparent)
{
    const quint16 c0 = qFromLittleEndian<quint16>(block);
    const quint16 c1 = qFromLittleEndian<quint16>(block + 2);
    QColor palette[4]{color565(c0), color565(c1)};
    if (c0 > c1 || !allowTransparent) {
        palette[2] = QColor((2 * palette[0].red() + palette[1].red()) / 3,
                            (2 * palette[0].green() + palette[1].green()) / 3,
                            (2 * palette[0].blue() + palette[1].blue()) / 3, 255);
        palette[3] = QColor((palette[0].red() + 2 * palette[1].red()) / 3,
                            (palette[0].green() + 2 * palette[1].green()) / 3,
                            (palette[0].blue() + 2 * palette[1].blue()) / 3, 255);
    } else {
        palette[2] = QColor((palette[0].red() + palette[1].red()) / 2,
                            (palette[0].green() + palette[1].green()) / 2,
                            (palette[0].blue() + palette[1].blue()) / 2, 255);
        palette[3] = QColor(0, 0, 0, 0);
    }
    const quint32 indices = qFromLittleEndian<quint32>(block + 4);
    for (int i = 0; i < 16; ++i) colors[i] = palette[(indices >> (2 * i)) & 3u];
}

void decodeAlphaBlock(const uchar* block, uchar values[16])
{
    uchar palette[8]{block[0], block[1]};
    if (palette[0] > palette[1]) {
        for (int i = 1; i <= 6; ++i)
            palette[i + 1] = static_cast<uchar>(((7 - i) * palette[0] + i * palette[1]) / 7);
    } else {
        for (int i = 1; i <= 4; ++i)
            palette[i + 1] = static_cast<uchar>(((5 - i) * palette[0] + i * palette[1]) / 5);
        palette[6] = 0;
        palette[7] = 255;
    }
    quint64 bits = 0;
    for (int i = 0; i < 6; ++i) bits |= static_cast<quint64>(block[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i) values[i] = palette[(bits >> (3 * i)) & 7u];
}

// Signed (SNORM) BC4/BC5 channel block, decoded into unsigned 0-255 space
// ((v/127)*0.5+0.5). FO76 normal maps ship as BC5_SNORM — legacy fourCC
// "BC5S" or DX10 DXGI 84 — and reading them through the unsigned path folds
// negative components into garbage.
void decodeSignedChannelBlock(const uchar* block, uchar values[16])
{
    const auto toUnsigned = [](int signedValue) {
        const float normalized = qMax(signedValue, -127) / 127.0f;
        return static_cast<uchar>(qBound(0, static_cast<int>((normalized * 0.5f + 0.5f) * 255.0f + 0.5f), 255));
    };
    const int e0 = static_cast<qint8>(block[0]);
    const int e1 = static_cast<qint8>(block[1]);
    uchar palette[8]{toUnsigned(e0), toUnsigned(e1)};
    if (e0 > e1) {
        for (int i = 1; i <= 6; ++i)
            palette[i + 1] = toUnsigned(((7 - i) * e0 + i * e1) / 7);
    } else {
        for (int i = 1; i <= 4; ++i)
            palette[i + 1] = toUnsigned(((5 - i) * e0 + i * e1) / 5);
        palette[6] = 0;    // -1.0
        palette[7] = 255;  // +1.0
    }
    quint64 bits = 0;
    for (int i = 0; i < 6; ++i) bits |= static_cast<quint64>(block[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i) values[i] = palette[(bits >> (3 * i)) & 7u];
}

void writeBlock(QImage& image, int bx, int by, const QColor pixels[16])
{
    for (int y = 0; y < 4; ++y) {
        const int py = by * 4 + y;
        if (py >= image.height()) continue;
        for (int x = 0; x < 4; ++x) {
            const int px = bx * 4 + x;
            if (px < image.width()) image.setPixelColor(px, py, pixels[y * 4 + x]);
        }
    }
}

QImage decodeDds(const QByteArray& data, QString* error)
{
    if (data.size() < 128 || data.first(4) != QByteArrayLiteral("DDS ") || u32(data, 4) != 124) {
        if (error) *error = QStringLiteral("Invalid DDS header.");
        return {};
    }
    const int height = static_cast<int>(u32(data, 12));
    const int width = static_cast<int>(u32(data, 16));
    if (width <= 0 || height <= 0 || width > 32768 || height > 32768) {
        if (error) *error = QStringLiteral("Invalid DDS dimensions.");
        return {};
    }

    QByteArray fourCc = data.mid(84, 4);
    quint32 dxgi = 0;
    int offset = 128;
    if (fourCc == QByteArrayLiteral("DX10")) {
        if (data.size() < 148) return {};
        dxgi = u32(data, 128);
        offset = 148;
        if (dxgi == 71 || dxgi == 72) fourCc = QByteArrayLiteral("DXT1");
        else if (dxgi == 74 || dxgi == 75) fourCc = QByteArrayLiteral("DXT3");
        else if (dxgi == 77 || dxgi == 78) fourCc = QByteArrayLiteral("DXT5");
        else if (dxgi == 80) fourCc = QByteArrayLiteral("ATI1");
        else if (dxgi == 81) fourCc = QByteArrayLiteral("BC4S");
        else if (dxgi == 83) fourCc = QByteArrayLiteral("ATI2");
        else if (dxgi == 84) fourCc = QByteArrayLiteral("BC5S");
        else if (dxgi == 98 || dxgi == 99) fourCc = QByteArrayLiteral("BC7 ");
    }

    QImage image(width, height, QImage::Format_RGBA8888);
    image.fill(Qt::transparent);
    const int blocksX = (width + 3) / 4;
    const int blocksY = (height + 3) / 4;
    int blockBytes = 0;
    if (fourCc == QByteArrayLiteral("DXT1") || fourCc == QByteArrayLiteral("ATI1") || fourCc == QByteArrayLiteral("BC4U") || fourCc == QByteArrayLiteral("BC4S")) blockBytes = 8;
    else if (fourCc == QByteArrayLiteral("DXT3") || fourCc == QByteArrayLiteral("DXT5") || fourCc == QByteArrayLiteral("ATI2") || fourCc == QByteArrayLiteral("BC5U") || fourCc == QByteArrayLiteral("BC5S") || fourCc == QByteArrayLiteral("BC7 ")) blockBytes = 16;

    if (blockBytes) {
        if (offset + blocksX * blocksY * blockBytes > data.size()) {
            if (error) *error = QStringLiteral("Truncated DDS image data.");
            return {};
        }
        for (int by = 0; by < blocksY; ++by) for (int bx = 0; bx < blocksX; ++bx) {
            const uchar* block = reinterpret_cast<const uchar*>(data.constData() + offset + (by * blocksX + bx) * blockBytes);
            QColor pixels[16];
            if (fourCc == QByteArrayLiteral("DXT1")) {
                decodeColorBlock(block, pixels, true);
            } else if (fourCc == QByteArrayLiteral("DXT3")) {
                decodeColorBlock(block + 8, pixels, false);
                for (int i = 0; i < 16; ++i) {
                    const int nibble = (block[i / 2] >> ((i & 1) * 4)) & 15;
                    pixels[i].setAlpha(nibble * 17);
                }
            } else if (fourCc == QByteArrayLiteral("DXT5")) {
                uchar alpha[16]; decodeAlphaBlock(block, alpha);
                decodeColorBlock(block + 8, pixels, false);
                for (int i = 0; i < 16; ++i) pixels[i].setAlpha(alpha[i]);
            } else if (fourCc == QByteArrayLiteral("ATI1") || fourCc == QByteArrayLiteral("BC4U") || fourCc == QByteArrayLiteral("BC4S")) {
                uchar red[16];
                if (fourCc == QByteArrayLiteral("BC4S")) decodeSignedChannelBlock(block, red);
                else decodeAlphaBlock(block, red);
                for (int i = 0; i < 16; ++i) pixels[i] = QColor(red[i], red[i], red[i], 255);
            } else if (fourCc == QByteArrayLiteral("ATI2") || fourCc == QByteArrayLiteral("BC5U") || fourCc == QByteArrayLiteral("BC5S")) {
                uchar red[16], green[16];
                if (fourCc == QByteArrayLiteral("BC5S")) {
                    decodeSignedChannelBlock(block, red);
                    decodeSignedChannelBlock(block + 8, green);
                } else {
                    decodeAlphaBlock(block, red);
                    decodeAlphaBlock(block + 8, green);
                }
                for (int i = 0; i < 16; ++i) {
                    const double nx = red[i] / 127.5 - 1.0;
                    const double ny = green[i] / 127.5 - 1.0;
                    const int blue = qBound(0, static_cast<int>((std::sqrt(qMax(0.0, 1.0 - nx * nx - ny * ny)) * 0.5 + 0.5) * 255.0), 255);
                    pixels[i] = QColor(red[i], green[i], blue, 255);
                }
            } else {
                uchar rgba[64]{};
                if (!detexDecompressBlockBPTC(block, DETEX_MODE_MASK_ALL_MODES_BPTC, 0, rgba)) {
                    if (error) *error = QStringLiteral("Invalid BC7 texture block.");
                    return {};
                }
                for (int i = 0; i < 16; ++i)
                    pixels[i] = QColor(rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2], rgba[i * 4 + 3]);
            }
            writeBlock(image, bx, by, pixels);
        }
        return image;
    }

    const quint32 bits = u32(data, 88);
    const quint32 rMask = u32(data, 92), gMask = u32(data, 96), bMask = u32(data, 100), aMask = u32(data, 104);
    if (bits != 32 || offset + width * height * 4 > data.size()) {
        if (error) *error = QStringLiteral("Unsupported DDS format.");
        return {};
    }
    const auto component = [](quint32 value, quint32 mask, int fallback) {
        if (!mask) return fallback;
        const int shift = qCountTrailingZeroBits(mask);
        const quint32 maximum = mask >> shift;
        return static_cast<int>(((value & mask) >> shift) * 255u / maximum);
    };
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const quint32 pixel = u32(data, offset + (y * width + x) * 4);
        image.setPixelColor(x, y, QColor(component(pixel, rMask, 0), component(pixel, gMask, 0), component(pixel, bMask, 0), component(pixel, aMask, 255)));
    }
    return image;
}

} // namespace

QStringList searchRoots(const QString& textureRoot)
{
    QSettings settings;
    QStringList candidates = settings.value(QStringLiteral("TextureSources/modFolders")).toStringList();
    if (!textureRoot.trimmed().isEmpty()) candidates.prepend(textureRoot);
    const QString legacy = settings.value(QStringLiteral("textureRoot")).toString();
    if (!legacy.trimmed().isEmpty()) candidates.append(legacy);
    const QString game = settings.value(QStringLiteral("TextureSources/fallout4Textures")).toString();
    if (!game.trimmed().isEmpty()) candidates.append(game);
    QStringList roots;
    QSet<QString> seen;
    for (const QString& candidate : candidates) {
        const QString absolute = QDir::cleanPath(QFileInfo(candidate).absoluteFilePath());
        const QString key = absolute.toLower();
        if (!candidate.trimmed().isEmpty() && !seen.contains(key)) {
            roots.append(absolute);
            seen.insert(key);
        }
    }
    return roots;
}

QStringList archivePaths()
{
    QStringList result;
    QSet<QString> seen;
    for (const QString& archive : QSettings().value(QStringLiteral("TextureSources/archives")).toStringList()) {
        const QString absolute = QFileInfo(archive).absoluteFilePath();
        const QString key = absolute.toLower();
        if (QFileInfo::exists(absolute) && !seen.contains(key)) {
            result.append(absolute);
            seen.insert(key);
        }
    }
    return result;
}

namespace {

QString canonicalTexturePath(QString path)
{
    path = QDir::cleanPath(QDir::fromNativeSeparators(path.trimmed()));
    if (path.startsWith(QStringLiteral("textures/"), Qt::CaseInsensitive)) path.remove(0, 9);
    return path;
}

QString loosePath(const QString& authoredPath, const QString& textureRoot)
{
    const QString native = canonicalTexturePath(authoredPath);
    if (native.isEmpty()) return {};
    const QFileInfo direct(QDir::fromNativeSeparators(authoredPath.trimmed()));
    if (direct.isAbsolute() && direct.exists()) return direct.absoluteFilePath();
    for (const QString& textureSource : searchRoots(textureRoot)) {
        const QDir root(textureSource);
        const QString candidate = root.filePath(native);
        if (QFileInfo::exists(candidate)) return QFileInfo(candidate).absoluteFilePath();
        const QString underTextures = root.filePath(QStringLiteral("Textures/") + native);
        if (QFileInfo::exists(underTextures)) return QFileInfo(underTextures).absoluteFilePath();
    }
    return {};
}

QByteArray textureBytes(const QString& authoredPath, const QString& textureRoot, QString* resolved, QString* error)
{
    const QString loose = loosePath(authoredPath, textureRoot);
    if (!loose.isEmpty()) {
        QFile file(loose);
        if (!file.open(QIODevice::ReadOnly)) {
            if (error) *error = file.errorString();
            return {};
        }
        if (resolved) *resolved = loose;
        return file.readAll();
    }
    const QString virtualPath = canonicalTexturePath(authoredPath);
    for (const QString& archive : archivePaths()) {
        if (!TextureArchive::contains(archive, virtualPath)) continue;
        QByteArray bytes = TextureArchive::extract(archive, virtualPath, error);
        if (!bytes.isEmpty() && resolved) *resolved = archive + QStringLiteral(" :: ") + virtualPath;
        return bytes;
    }
    if (error) *error = QStringLiteral("Texture could not be found in configured loose folders or BA2 archives.");
    return {};
}

QVector3D cubemapDirection(int face, float u, float v)
{
    switch (face) {
    case 0: return QVector3D(1, -v, -u).normalized();
    case 1: return QVector3D(-1, -v, u).normalized();
    case 2: return QVector3D(u, 1, v).normalized();
    case 3: return QVector3D(u, -1, -v).normalized();
    case 4: return QVector3D(u, -v, 1).normalized();
    default: return QVector3D(-u, -v, -1).normalized();
    }
}

QVector<QImage> decodeExrEnvironment(const QByteArray& data, QString* error)
{
    float* source = nullptr;
    int width = 0, height = 0;
    const char* exrError = nullptr;
    const int result = LoadEXRFromMemory(&source, &width, &height,
        reinterpret_cast<const unsigned char*>(data.constData()), size_t(data.size()), &exrError);
    if (result != TINYEXR_SUCCESS || !source) {
        if (error) *error = exrError ? QString::fromUtf8(exrError) : QStringLiteral("Invalid OpenEXR image.");
        if (exrError) FreeEXRErrorMessage(exrError);
        std::free(source);
        return {};
    }
    if (exrError) FreeEXRErrorMessage(exrError);
    if (width < 2 || height < 2 || width < int(height * 1.8f) || width > int(height * 2.2f)) {
        if (error) *error = QStringLiteral("EXR preview environments must use a 2:1 latitude-longitude layout.");
        std::free(source);
        return {};
    }

    const auto sample = [source, width, height](float u, float v) {
        u -= std::floor(u);
        v = std::clamp(v, 0.0f, 1.0f);
        const float px = u * float(width) - .5f, py = v * float(height) - .5f;
        const int x0 = int(std::floor(px)), y0 = std::clamp(int(std::floor(py)), 0, height - 1);
        const int x1 = x0 + 1, y1 = std::min(y0 + 1, height - 1);
        const float tx = px - std::floor(px), ty = py - std::floor(py);
        const auto texel = [source, width, height](int x, int y) {
            x = (x % width + width) % width; y = std::clamp(y, 0, height - 1);
            const float* value = source + (y * width + x) * 4;
            return QVector4D(value[0], value[1], value[2], value[3]);
        };
        const QVector4D top=texel(x0,y0)*(1.0f-tx)+texel(x1,y0)*tx;
        const QVector4D bottom=texel(x0,y1)*(1.0f-tx)+texel(x1,y1)*tx;
        return top*(1.0f-ty)+bottom*ty;
    };

    const int faceSize = std::clamp(height, 16, 1024);
    QVector<QImage> faces;
    faces.reserve(6);
    constexpr float pi = 3.14159265358979323846f;
    for (int face = 0; face < 6; ++face) {
        QImage image(faceSize, faceSize, QImage::Format_RGBA32FPx4);
        for (int y = 0; y < faceSize; ++y) {
            float* row = reinterpret_cast<float*>(image.scanLine(y));
            for (int x = 0; x < faceSize; ++x) {
                const QVector3D cubeDirection = cubemapDirection(face,
                    2.0f * (float(x) + .5f) / float(faceSize) - 1.0f,
                    2.0f * (float(y) + .5f) / float(faceSize) - 1.0f);
                // Preview shaders remap Y-up world directions to the Fallout/DDS
                // cubemap convention as (x, z, -y). Bake the inverse transform
                // into equirectangular environments so EXRs remain world-Y-up.
                const QVector3D environmentDirection(cubeDirection.x(), -cubeDirection.z(), cubeDirection.y());
                const float longitude = std::atan2(environmentDirection.z(), environmentDirection.x());
                const float latitude = std::asin(std::clamp(environmentDirection.y(), -1.0f, 1.0f));
                const QVector4D value = sample(longitude / (2.0f * pi) + .5f, .5f - latitude / pi);
                row[x * 4] = std::max(value.x(), 0.0f);
                row[x * 4 + 1] = std::max(value.y(), 0.0f);
                row[x * 4 + 2] = std::max(value.z(), 0.0f);
                row[x * 4 + 3] = std::max(value.w(), 0.0f);
            }
        }
        faces.append(std::move(image));
    }
    std::free(source);
    return faces;
}

} // namespace

QString resolvePath(const QString& authoredPath, const QString& textureRoot)
{
    const QString loose = loosePath(authoredPath, textureRoot);
    if (!loose.isEmpty()) return loose;
    const QString virtualPath = canonicalTexturePath(authoredPath);
    for (const QString& archive : archivePaths())
        if (TextureArchive::contains(archive, virtualPath)) return archive + QStringLiteral(" :: ") + virtualPath;
    return {};
}

QString authoredPathForFile(const QString& absolutePath, const QString& textureRoot)
{
    const QString absolute = QFileInfo(absolutePath).absoluteFilePath();
    QString best;
    for (const QString& source : searchRoots(textureRoot)) {
        const QStringList roots{source, QDir(source).filePath(QStringLiteral("Textures"))};
        for (const QString& rootPath : roots) {
            const QString relative = QDir::cleanPath(QDir(rootPath).relativeFilePath(absolute));
            if (relative != QStringLiteral("..") && !relative.startsWith(QStringLiteral("../"))
                && (best.isEmpty() || relative.size() < best.size())) best = relative;
        }
    }
    if (best.isEmpty()) {
        const QString normalized = QDir::fromNativeSeparators(absolute);
        const int marker = normalized.lastIndexOf(QStringLiteral("/Textures/"), -1, Qt::CaseInsensitive);
        if (marker >= 0) best = normalized.mid(marker + 10);
    }
    return QDir::toNativeSeparators(best.isEmpty() ? absolute : canonicalTexturePath(best));
}

QVector<Resource> enumerate(const QString& textureRoot, QStringList* errors)
{
    QVector<Resource> result;
    QSet<QString> claimed;
    const QStringList filters{QStringLiteral("*.dds"), QStringLiteral("*.tga"), QStringLiteral("*.png"),
        QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"), QStringLiteral("*.bmp")};
    for (const QString& source : searchRoots(textureRoot)) {
        QString scanRoot = source;
        if (QFileInfo(QDir(source).filePath(QStringLiteral("Textures"))).isDir()) scanRoot = QDir(source).filePath(QStringLiteral("Textures"));
        if (!QFileInfo(scanRoot).isDir()) continue;
        QDirIterator it(scanRoot, filters, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString absolute = it.next();
            const QString path = canonicalTexturePath(QDir(scanRoot).relativeFilePath(absolute));
            const QString key = path.toLower();
            if (claimed.contains(key)) continue;
            claimed.insert(key);
            result.append({QDir::toNativeSeparators(path), QFileInfo(source).fileName(), absolute, false});
        }
    }
    for (const QString& archive : archivePaths()) {
        QString error;
        const QStringList paths = TextureArchive::texturePaths(archive, &error);
        if (!error.isEmpty() && errors) errors->append(QStringLiteral("%1: %2").arg(QFileInfo(archive).fileName(), error));
        for (const QString& path : paths) {
            const QString key = canonicalTexturePath(path).toLower();
            if (claimed.contains(key)) continue;
            claimed.insert(key);
            result.append({QDir::toNativeSeparators(canonicalTexturePath(path)), QFileInfo(archive).fileName(), {}, true});
        }
    }
    std::sort(result.begin(), result.end(), [](const Resource& a, const Resource& b) {
        return a.path.compare(b.path, Qt::CaseInsensitive) < 0;
    });
    return result;
}

QImage load(const QString& authoredPath, const QString& textureRoot, QString* error)
{
    if (error) error->clear();
    QString resolved;
    const QByteArray data = textureBytes(authoredPath, textureRoot, &resolved, error);
    if (data.isEmpty()) return {};
    if (authoredPath.endsWith(QStringLiteral(".dds"), Qt::CaseInsensitive)) return decodeDds(data, error);
    QImage image = QImage::fromData(data);
    if (image.isNull() && error) *error = QStringLiteral("Unsupported or invalid texture image.");
    return image;
}

QImage loadArraySlice(const QString& authoredPath, int slice, const QString& textureRoot, int* sliceCount, QString* error)
{
    if (sliceCount) *sliceCount = 1;
    if (error) error->clear();
    if (!authoredPath.endsWith(QStringLiteral(".dds"), Qt::CaseInsensitive)) return load(authoredPath, textureRoot, error);
    QString resolved;
    const QByteArray data = textureBytes(authoredPath, textureRoot, &resolved, error);
    if (data.isEmpty()) return {};
    if (data.size() < 148 || data.first(4) != QByteArrayLiteral("DDS ") || data.mid(84, 4) != QByteArrayLiteral("DX10"))
        return decodeDds(data, error);

    const int arraySize = qMax(1, int(u32(data, 140)));
    if (sliceCount) *sliceCount = arraySize;
    if (arraySize == 1) return decodeDds(data, error);
    const int width = int(u32(data, 16)), height = int(u32(data, 12)), mipLevels = qMax(1, int(u32(data, 28)));
    QByteArray fourCc;
    const quint32 dxgi = u32(data, 128);
    if (dxgi == 71 || dxgi == 72) fourCc = QByteArrayLiteral("DXT1");
    else if (dxgi == 74 || dxgi == 75) fourCc = QByteArrayLiteral("DXT3");
    else if (dxgi == 77 || dxgi == 78) fourCc = QByteArrayLiteral("DXT5");
    else if (dxgi == 80 || dxgi == 81) fourCc = QByteArrayLiteral("ATI1");
    else if (dxgi == 83 || dxgi == 84) fourCc = QByteArrayLiteral("ATI2");
    else if (dxgi == 98 || dxgi == 99) fourCc = QByteArrayLiteral("BC7 ");
    int blockBytes = 0;
    if (fourCc == QByteArrayLiteral("DXT1") || fourCc == QByteArrayLiteral("ATI1")) blockBytes = 8;
    else if (!fourCc.isEmpty()) blockBytes = 16;
    const auto levelBytes = [blockBytes](int w, int h) {
        return blockBytes ? qMax(1, (w + 3) / 4) * qMax(1, (h + 3) / 4) * blockBytes : w * h * 4;
    };
    const qint64 baseBytes = levelBytes(width, height);
    qint64 sliceStride = 0;
    for (int level = 0; level < mipLevels; ++level)
        sliceStride += levelBytes(qMax(1, width >> level), qMax(1, height >> level));
    const int selected = std::clamp(slice, 0, arraySize - 1);
    if (sliceStride <= 0 || 148 + sliceStride * arraySize > data.size()) {
        if (error) *error = QStringLiteral("DDS texture array is truncated or uses an unsupported format.");
        return {};
    }
    QByteArray selectedData = data.left(148);
    selectedData.append(data.mid(148 + selected * sliceStride, baseBytes));
    return decodeDds(selectedData, error);
}

QVector<QImage> loadCubemapFaces(const QString& authoredPath, const QString& textureRoot, QString* error)
{
    if (error) error->clear();
    if (authoredPath.endsWith(QStringLiteral(".exr"), Qt::CaseInsensitive)) {
        QString resolved;
        const QByteArray data = textureBytes(authoredPath, textureRoot, &resolved, error);
        return data.isEmpty() ? QVector<QImage>() : decodeExrEnvironment(data, error);
    }
    if (!authoredPath.endsWith(QStringLiteral(".dds"), Qt::CaseInsensitive)) {
        const QImage image = load(authoredPath, textureRoot, error);
        if (image.isNull()) return {};
        return QVector<QImage>(6, image);
    }
    QString resolved;
    const QByteArray data = textureBytes(authoredPath, textureRoot, &resolved, error);
    if (data.isEmpty()) return {};
    if (data.size() < 128 || data.first(4) != QByteArrayLiteral("DDS ")) {
        if (error) *error = QStringLiteral("Invalid DDS cubemap header.");
        return {};
    }
    const int width = int(u32(data, 16)), height = int(u32(data, 12));
    const int mipLevels = qMax(1, int(u32(data, 28)));
    QByteArray fourCc = data.mid(84, 4);
    int headerBytes = 128;
    quint32 dxgi = 0;
    if (fourCc == QByteArrayLiteral("DX10")) {
        if (data.size() < 148) return {};
        dxgi = u32(data, 128); headerBytes = 148;
        if (dxgi == 71 || dxgi == 72) fourCc = QByteArrayLiteral("DXT1");
        else if (dxgi == 74 || dxgi == 75) fourCc = QByteArrayLiteral("DXT3");
        else if (dxgi == 77 || dxgi == 78) fourCc = QByteArrayLiteral("DXT5");
        else if (dxgi == 80 || dxgi == 81) fourCc = QByteArrayLiteral("ATI1");
        else if (dxgi == 83 || dxgi == 84) fourCc = QByteArrayLiteral("ATI2");
        else if (dxgi == 98 || dxgi == 99) fourCc = QByteArrayLiteral("BC7 ");
    }
    int blockBytes = 0;
    if (fourCc == QByteArrayLiteral("DXT1") || fourCc == QByteArrayLiteral("ATI1") || fourCc == QByteArrayLiteral("BC4U") || fourCc == QByteArrayLiteral("BC4S")) blockBytes = 8;
    else if (fourCc == QByteArrayLiteral("DXT3") || fourCc == QByteArrayLiteral("DXT5") || fourCc == QByteArrayLiteral("ATI2") || fourCc == QByteArrayLiteral("BC5U") || fourCc == QByteArrayLiteral("BC5S") || fourCc == QByteArrayLiteral("BC7 ")) blockBytes = 16;
    const auto levelBytes = [blockBytes](int w, int h) {
        return blockBytes ? qMax(1, (w + 3) / 4) * qMax(1, (h + 3) / 4) * blockBytes : w * h * 4;
    };
    const int baseBytes = levelBytes(width, height);
    qint64 faceStride = 0;
    for (int level = 0; level < mipLevels; ++level)
        faceStride += levelBytes(qMax(1, width >> level), qMax(1, height >> level));
    if (faceStride <= 0 || headerBytes + faceStride * 6 > data.size()) {
        if (error) *error = QStringLiteral("DDS does not contain six complete cubemap faces in a supported format.");
        return {};
    }
    QVector<QImage> faces;
    for (int face = 0; face < 6; ++face) {
        QByteArray faceData = data.left(headerBytes);
        faceData.append(data.mid(headerBytes + face * faceStride, baseBytes));
        QImage decoded = decodeDds(faceData, error);
        if (decoded.isNull()) return {};
        faces.append(decoded);
    }
    return faces;
}

QImage isolateChannel(const QImage& source, const QString& channel)
{
    if (source.isNull() || channel == QStringLiteral("RGBA")) return source;
    QImage result = source.convertToFormat(QImage::Format_RGBA8888);
    for (int y = 0; y < result.height(); ++y) {
        for (int x = 0; x < result.width(); ++x) {
            const QColor color = result.pixelColor(x, y);
            int value = 0;
            if (channel == QStringLiteral("R")) value = color.red();
            else if (channel == QStringLiteral("G")) value = color.green();
            else if (channel == QStringLiteral("B")) value = color.blue();
            else if (channel == QStringLiteral("A")) value = color.alpha();
            result.setPixelColor(x, y, QColor(value, value, value, 255));
        }
    }
    return result;
}

QString Diagnostics::summary() const
{
    if (!valid) return error;
    const QString size = fileSize >= 1024 * 1024
        ? QStringLiteral("%1 MiB").arg(fileSize / (1024.0 * 1024.0), 0, 'f', 1)
        : QStringLiteral("%1 KiB").arg(fileSize / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1×%2 • %3 • %4 • %5 • %6 mip%7 • %8")
        .arg(width)
        .arg(height)
        .arg(format)
        .arg(colorSpace)
        .arg(channels.join(QString()))
        .arg(mipLevels)
        .arg(mipLevels == 1 ? QString() : QStringLiteral("s"))
        .arg(size);
}

Diagnostics inspect(const QString& authoredPath, const QString& textureRoot)
{
    Diagnostics result;
    result.resolvedPath = resolvePath(authoredPath, textureRoot);
    if (result.resolvedPath.isEmpty()) {
        result.error = QStringLiteral("Texture path is unresolved");
        return result;
    }
    QString resolved;
    QString readError;
    const QByteArray bytes = textureBytes(authoredPath, textureRoot, &resolved, &readError);
    if (bytes.isEmpty()) { result.error = readError; return result; }
    result.fileSize = bytes.size();
    if (authoredPath.endsWith(QStringLiteral(".dds"), Qt::CaseInsensitive)) {
        const QByteArray header = bytes.left(148);
        if (header.size() < 128 || header.first(4) != QByteArrayLiteral("DDS ")) {
            result.error = QStringLiteral("Invalid DDS header"); return result;
        }
        result.height = static_cast<int>(u32(header, 12));
        result.width = static_cast<int>(u32(header, 16));
        result.mipLevels = qMax(1, static_cast<int>(u32(header, 28)));
        QByteArray fourCc = header.mid(84, 4);
        quint32 dxgi = 0;
        if (fourCc == QByteArrayLiteral("DX10") && header.size() >= 148) dxgi = u32(header, 128);
        if (fourCc == QByteArrayLiteral("DXT1") || dxgi == 71 || dxgi == 72) {
            result.format = QStringLiteral("BC1"); result.channels = {"R","G","B","A"};
        } else if (fourCc == QByteArrayLiteral("DXT3") || dxgi == 74 || dxgi == 75) {
            result.format = QStringLiteral("BC2"); result.channels = {"R","G","B","A"};
        } else if (fourCc == QByteArrayLiteral("DXT5") || dxgi == 77 || dxgi == 78) {
            result.format = QStringLiteral("BC3"); result.channels = {"R","G","B","A"};
        } else if (fourCc == QByteArrayLiteral("BC4S") || dxgi == 81) {
            result.format = QStringLiteral("BC4 SNORM"); result.channels = {"R"};
        } else if (fourCc == QByteArrayLiteral("ATI1") || fourCc == QByteArrayLiteral("BC4U") || dxgi == 80) {
            result.format = QStringLiteral("BC4"); result.channels = {"R"};
        } else if (fourCc == QByteArrayLiteral("BC5S") || dxgi == 84) {
            result.format = QStringLiteral("BC5 SNORM"); result.channels = {"R","G"};
        } else if (fourCc == QByteArrayLiteral("ATI2") || fourCc == QByteArrayLiteral("BC5U") || dxgi == 83) {
            result.format = QStringLiteral("BC5"); result.channels = {"R","G"};
        } else if (dxgi == 98 || dxgi == 99) {
            result.format = QStringLiteral("BC7"); result.channels = {"R","G","B","A"};
        } else {
            result.format = fourCc.trimmed().isEmpty() ? QStringLiteral("RGBA32") : QString::fromLatin1(fourCc);
            result.channels = {"R","G","B","A"};
        }
        const bool srgb = dxgi == 72 || dxgi == 75 || dxgi == 78 || dxgi == 99;
        result.colorSpace = srgb ? QStringLiteral("sRGB") : QStringLiteral("Linear/unspecified");
        result.valid = result.width > 0 && result.height > 0;
        if (!result.valid) result.error = QStringLiteral("Invalid DDS dimensions");
        return result;
    }

    const QImage image = QImage::fromData(bytes);
    if (image.isNull()) { result.error = QStringLiteral("Unsupported or invalid texture image"); return result; }
    result.width = image.width();
    result.height = image.height();
    result.format = QFileInfo(authoredPath).suffix().toUpper();
    result.colorSpace = image.colorSpace().isValid() ? image.colorSpace().description() : QStringLiteral("Unspecified");
    result.channels = image.hasAlphaChannel() ? QStringList{"R","G","B","A"} : QStringList{"R","G","B"};
    result.valid = true;
    return result;
}

} // namespace TextureImage
