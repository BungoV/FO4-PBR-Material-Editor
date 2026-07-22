#include "texturewriter.h"

#define RGBCX_IMPLEMENTATION
#include "rgbcx.h"
#include "bc7enc.h"

#include <QDataStream>
#include <QSaveFile>

#include <cmath>
#include <cstring>

namespace TextureWriter {

namespace {

// Plain per-channel 2x2 box average. Qt's smooth scaling premultiplies alpha,
// which would weight RGB by whatever data channel rides A (curvature, masks)
// and contrast-shift the chain; packed data needs independent, mean-preserving
// channels.
QImage halveImage(const QImage& source)
{
    const int width = qMax(1, source.width() / 2);
    const int height = qMax(1, source.height() / 2);
    QImage result(width, height, QImage::Format_RGBA8888);
    for (int y = 0; y < height; ++y) {
        const uchar* row0 = source.constScanLine(qMin(y * 2, source.height() - 1));
        const uchar* row1 = source.constScanLine(qMin(y * 2 + 1, source.height() - 1));
        uchar* out = result.scanLine(y);
        for (int x = 0; x < width; ++x) {
            const int x0 = qMin(x * 2, source.width() - 1) * 4;
            const int x1 = qMin(x * 2 + 1, source.width() - 1) * 4;
            for (int c = 0; c < 4; ++c)
                out[x * 4 + c] = uchar((int(row0[x0 + c]) + int(row0[x1 + c]) + int(row1[x0 + c]) + int(row1[x1 + c]) + 2) / 4);
        }
    }
    return result;
}

// Normal maps cannot use the packed-data box filter above: averaging encoded
// RG values shortens or biases the represented direction. Decode each 2x2
// footprint to unit tangent-space vectors, average in vector space, then
// renormalize and encode RG again. B/A remain independent mean-preserving data
// channels for BC7 normal layouts that also carry height or curvature.
QImage halveNormalImage(const QImage& source)
{
    const int width = qMax(1, source.width() / 2);
    const int height = qMax(1, source.height() / 2);
    QImage result(width, height, QImage::Format_RGBA8888);
    for (int y = 0; y < height; ++y) {
        const uchar* rows[2] = {
            source.constScanLine(qMin(y * 2, source.height() - 1)),
            source.constScanLine(qMin(y * 2 + 1, source.height() - 1))
        };
        uchar* out = result.scanLine(y);
        for (int x = 0; x < width; ++x) {
            const int xs[2] = {
                qMin(x * 2, source.width() - 1) * 4,
                qMin(x * 2 + 1, source.width() - 1) * 4
            };
            float averageX = 0.0f, averageY = 0.0f, averageZ = 0.0f;
            int dataB = 0, dataA = 0;
            for (int row = 0; row < 2; ++row) {
                for (int column = 0; column < 2; ++column) {
                    const uchar* texel = rows[row] + xs[column];
                    float nx = texel[0] / 255.0f * 2.0f - 1.0f;
                    float ny = texel[1] / 255.0f * 2.0f - 1.0f;
                    float nz = std::sqrt(qMax(0.0f, 1.0f - nx * nx - ny * ny));
                    const float inputLength = std::sqrt(nx * nx + ny * ny + nz * nz);
                    if (inputLength > 1.0e-6f) {
                        nx /= inputLength;
                        ny /= inputLength;
                        nz /= inputLength;
                    }
                    averageX += nx;
                    averageY += ny;
                    averageZ += nz;
                    dataB += texel[2];
                    dataA += texel[3];
                }
            }
            const float averageLength = std::sqrt(
                averageX * averageX + averageY * averageY + averageZ * averageZ);
            if (averageLength > 1.0e-6f) {
                averageX /= averageLength;
                averageY /= averageLength;
            } else {
                averageX = averageY = 0.0f;
            }
            out[x * 4] = uchar(std::lround(qBound(0.0f, averageX * 0.5f + 0.5f, 1.0f) * 255.0f));
            out[x * 4 + 1] = uchar(std::lround(qBound(0.0f, averageY * 0.5f + 0.5f, 1.0f) * 255.0f));
            out[x * 4 + 2] = uchar((dataB + 2) / 4);
            out[x * 4 + 3] = uchar((dataA + 2) / 4);
        }
    }
    return result;
}

// Folds normal-map variance into the roughness (R) mip chain. Each normal mip
// is the plain average of decoded unit vectors; the shortening of that average
// measures how much normal detail the level lost, and that lost detail becomes
// roughness: alpha'^2 = alpha^2 + (1 - |avg N|), r = sqrt(alpha). Offline and
// physically motivated — the runtime then needs only its screen-space
// specular AA.
void applyToksvigToMips(QVector<QImage>& mips, const QImage& normalMap)
{
    if (mips.size() < 2 || normalMap.isNull()) return;
    const QImage baseNormal = normalMap.convertToFormat(QImage::Format_RGBA8888)
        .scaled(mips.first().width(), mips.first().height(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    // Decode to float vectors once; Z is reconstructed from XY so a packed
    // blue channel (height) cannot poison the variance estimate.
    int width = baseNormal.width(), height = baseNormal.height();
    QVector<float> vectors(width * height * 3);
    for (int y = 0; y < height; ++y) {
        const uchar* line = baseNormal.constScanLine(y);
        for (int x = 0; x < width; ++x) {
            const float nx = line[x * 4] / 255.0f * 2.0f - 1.0f;
            const float ny = line[x * 4 + 1] / 255.0f * 2.0f - 1.0f;
            const float nz = std::sqrt(qMax(0.0f, 1.0f - nx * nx - ny * ny));
            vectors[(y * width + x) * 3] = nx;
            vectors[(y * width + x) * 3 + 1] = ny;
            vectors[(y * width + x) * 3 + 2] = nz;
        }
    }
    for (int level = 1; level < mips.size(); ++level) {
        // Halve the float vector field with a plain average (mean-preserving).
        const int nextWidth = qMax(1, width / 2), nextHeight = qMax(1, height / 2);
        QVector<float> next(nextWidth * nextHeight * 3);
        for (int y = 0; y < nextHeight; ++y) {
            const int y0 = qMin(y * 2, height - 1), y1 = qMin(y * 2 + 1, height - 1);
            for (int x = 0; x < nextWidth; ++x) {
                const int x0 = qMin(x * 2, width - 1), x1 = qMin(x * 2 + 1, width - 1);
                for (int c = 0; c < 3; ++c)
                    next[(y * nextWidth + x) * 3 + c] = (vectors[(y0 * width + x0) * 3 + c] + vectors[(y0 * width + x1) * 3 + c]
                        + vectors[(y1 * width + x0) * 3 + c] + vectors[(y1 * width + x1) * 3 + c]) * .25f;
            }
        }
        vectors = next; width = nextWidth; height = nextHeight;
        QImage& mip = mips[level];
        for (int y = 0; y < mip.height(); ++y) {
            uchar* line = mip.scanLine(y);
            const int vy = qMin(y, height - 1);
            for (int x = 0; x < mip.width(); ++x) {
                const int vx = qMin(x, width - 1);
                const float* v = vectors.constData() + (vy * width + vx) * 3;
                const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                const float variance = qMax(0.0f, 1.0f - length);
                if (variance <= 0.0f) continue;
                const float rough = line[x * 4] / 255.0f;
                const float alpha = rough * rough;
                const float combined = std::sqrt(std::sqrt(qMin(1.0f, alpha * alpha + variance)));
                line[x * 4] = uchar(std::lround(qBound(0.0f, combined, 1.0f) * 255.0f));
            }
        }
    }
}

bool isBlockCompressed(DdsFormat format)
{
    return format != DdsFormat::Rgba8 && format != DdsFormat::Rgba8Srgb;
}

quint32 dxgiFormat(DdsFormat format)
{
    switch (format) {
    case DdsFormat::Bc1: return 71;       // DXGI_FORMAT_BC1_UNORM
    case DdsFormat::Bc4: return 80;       // DXGI_FORMAT_BC4_UNORM
    case DdsFormat::Bc5: return 83;       // DXGI_FORMAT_BC5_UNORM
    case DdsFormat::Bc7: return 98;       // DXGI_FORMAT_BC7_UNORM
    case DdsFormat::Bc7Srgb: return 99;   // DXGI_FORMAT_BC7_UNORM_SRGB
    case DdsFormat::Rgba8: return 28;     // DXGI_FORMAT_R8G8B8A8_UNORM
    case DdsFormat::Rgba8Srgb: return 29; // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
    }
    return 28;
}

int bytesPerBlock(DdsFormat format)
{
    switch (format) {
    case DdsFormat::Bc1:
    case DdsFormat::Bc4: return 8;
    default: return 16;
    }
}

void encodersInitialized()
{
    static bool initialized = false;
    if (initialized) return;
    rgbcx::init();
    bc7enc_compress_block_init();
    initialized = true;
}

QByteArray encodeMipLevel(const QImage& mip, DdsFormat format)
{
    const QImage source = mip.convertToFormat(QImage::Format_RGBA8888);
    if (!isBlockCompressed(format)) {
        QByteArray data;
        data.reserve(source.width() * source.height() * 4);
        for (int y = 0; y < source.height(); ++y)
            data.append(reinterpret_cast<const char*>(source.constScanLine(y)), source.width() * 4);
        return data;
    }

    bc7enc_compress_block_params bc7Params;
    bc7enc_compress_block_params_init(&bc7Params);

    const int blocksX = (source.width() + 3) / 4;
    const int blocksY = (source.height() + 3) / 4;
    QByteArray data(blocksX * blocksY * bytesPerBlock(format), Qt::Uninitialized);
    quint8 blockPixels[16 * 4];
    for (int by = 0; by < blocksY; ++by) {
        for (int bx = 0; bx < blocksX; ++bx) {
            // Gather a 4x4 block, clamping at the edges.
            for (int py = 0; py < 4; ++py) {
                const int y = qMin(by * 4 + py, source.height() - 1);
                const uchar* line = source.constScanLine(y);
                for (int px = 0; px < 4; ++px) {
                    const int x = qMin(bx * 4 + px, source.width() - 1);
                    std::memcpy(blockPixels + (py * 4 + px) * 4, line + x * 4, 4);
                }
            }
            char* block = data.data() + (by * blocksX + bx) * bytesPerBlock(format);
            switch (format) {
            case DdsFormat::Bc1:
                rgbcx::encode_bc1(rgbcx::MAX_LEVEL, block, blockPixels, false, false);
                break;
            case DdsFormat::Bc4:
                rgbcx::encode_bc4(block, blockPixels, 4);
                break;
            case DdsFormat::Bc5:
                rgbcx::encode_bc5(block, blockPixels, 0, 1, 4);
                break;
            case DdsFormat::Bc7:
            case DdsFormat::Bc7Srgb:
                bc7enc_compress_block(block, blockPixels, &bc7Params);
                break;
            default:
                break;
            }
        }
    }
    return data;
}

} // namespace

bool saveDds(const QImage& image, const QString& filePath, DdsFormat format,
    bool generateMips, QString* error, const QImage& toksvigNormal,
    bool normalMapMips)
{
    if (image.isNull()) {
        if (error) *error = QStringLiteral("There is no composed image to export.");
        return false;
    }
    encodersInitialized();

    QVector<QImage> mips;
    mips.append(image.convertToFormat(QImage::Format_RGBA8888));
    if (generateMips) {
        while (mips.last().width() > 1 || mips.last().height() > 1)
            mips.append(normalMapMips
                ? halveNormalImage(mips.last())
                : halveImage(mips.last()));
        applyToksvigToMips(mips, toksvigNormal);
    }

    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);

    const quint32 width = quint32(image.width()), height = quint32(image.height());
    const bool compressed = isBlockCompressed(format);
    const quint32 pitchOrLinear = compressed
        ? quint32(((width + 3) / 4) * ((height + 3) / 4) * bytesPerBlock(format))
        : width * 4;
    // DDSD_CAPS|HEIGHT|WIDTH|PIXELFORMAT|MIPMAPCOUNT + pitch kind. Compressed
    // formats use a DX10 extension header; uncompressed RGBA uses the legacy
    // masked pixel format for the broadest reader compatibility (the sRGB
    // distinction is a metadata-only loss there).
    quint32 flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | (compressed ? 0x80000 : 0x8);
    stream << quint32(0x20534444);                       // 'DDS '
    stream << quint32(124) << flags << height << width << pitchOrLinear;
    stream << quint32(0) << quint32(mips.size());        // depth, mipMapCount
    for (int i = 0; i < 11; ++i) stream << quint32(0);   // reserved
    if (compressed) {
        stream << quint32(32) << quint32(0x4);           // ddspf size, DDPF_FOURCC
        stream << quint32(0x30315844);                   // 'DX10'
        for (int i = 0; i < 5; ++i) stream << quint32(0);
    } else {
        stream << quint32(32) << quint32(0x40 | 0x1);    // DDPF_RGB|DDPF_ALPHAPIXELS
        stream << quint32(0) << quint32(32);             // no fourCC, 32bpp
        stream << quint32(0x000000FF) << quint32(0x0000FF00) << quint32(0x00FF0000) << quint32(0xFF000000);
    }
    stream << quint32(0x1000 | 0x400000 | 0x8);          // caps: TEXTURE|MIPMAP|COMPLEX
    stream << quint32(0) << quint32(0) << quint32(0) << quint32(0);
    if (compressed)
        stream << dxgiFormat(format) << quint32(3) << quint32(0) << quint32(1) << quint32(0); // DX10 block

    for (const QImage& mip : mips) {
        const QByteArray data = encodeMipLevel(mip, format);
        if (file.write(data) != data.size()) {
            if (error) *error = file.errorString();
            return false;
        }
    }
    if (!file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

} // namespace TextureWriter
