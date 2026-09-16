#include "specularmigrate.h"

#include "materialdocument.h"
#include "textureimage.h"
#include "texturewriter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace SpecularMigrate {

namespace {

int texel(double unit)
{
    return std::clamp(static_cast<int>(std::lround(unit * 255.0)), 0, 255);
}

int iorTexel(double ior, double iorMax)
{
    return texel(ior / std::max(iorMax, 1e-4));
}

bool fail(QString* error, const QString& text)
{
    if (error) *error = text;
    return false;
}

// Keeps the first .v5.bak ever taken; a second migration of the same file must
// not clobber the original.
bool backup(const QString& filePath, QString* error)
{
    const QString backupPath = filePath + QStringLiteral(".v5.bak");
    if (QFile::exists(backupPath)) return true;
    if (QFile::copy(filePath, backupPath)) return true;
    return fail(error, QStringLiteral("cannot back up %1 to %2").arg(filePath, backupPath));
}

bool isArraySlot(const QString& slotKey)
{
    return slotKey.endsWith(QStringLiteral("Array"));
}

} // namespace

double specularIorForF0(double f0)
{
    const double s = std::sqrt(std::clamp(f0, 0.0, 0.999999));
    return (1.0 + s) / (1.0 - s);
}

F0Repack repackF0Alpha(const QImage& source, double materialIor, double iorMax)
{
    F0Repack result;
    if (source.isNull()) return result;
    QImage rmaos = source.convertToFormat(QImage::Format_RGBA8888);
    QImage ior(rmaos.size(), QImage::Format_Grayscale8);
    const int materialTexel = iorTexel(materialIor, iorMax);
    bool needsMap = false;
    for (int y = 0; y < rmaos.height(); ++y) {
        uchar* line = rmaos.scanLine(y);
        uchar* iorLine = ior.scanLine(y);
        for (int x = 0; x < rmaos.width(); ++x) {
            const double f0 = line[x * 4 + 3] / 255.0;
            int weight = 255;
            int mapTexel = materialTexel;
            if (f0 > 0.04 + 1e-9) {
                needsMap = true;
                ++result.texelsAboveDefault;
                const double n = specularIorForF0(f0);
                if (n > iorMax) ++result.texelsAboveCeiling;
                mapTexel = iorTexel(n, iorMax);
            } else {
                weight = texel(f0 / 0.04);
            }
            line[x * 4 + 3] = static_cast<uchar>(weight);
            iorLine[x] = static_cast<uchar>(mapTexel);
        }
    }
    result.rmaos = rmaos;
    if (needsMap) result.iorAlpha = ior;
    return result;
}

QImage bakeTintAlpha(const QImage& source, double ior, double iorMax)
{
    if (source.isNull()) return {};
    QImage spec = source.convertToFormat(QImage::Format_RGBA8888);
    const uchar alpha = static_cast<uchar>(iorTexel(ior, iorMax));
    for (int y = 0; y < spec.height(); ++y) {
        uchar* line = spec.scanLine(y);
        for (int x = 0; x < spec.width(); ++x) {
            const double a = line[x * 4 + 3] / 255.0;
            for (int c = 0; c < 3; ++c) {
                const double linear = std::pow(line[x * 4 + c] / 255.0, 2.2);
                const double baked = 1.0 + (linear - 1.0) * a;
                line[x * 4 + c] = static_cast<uchar>(texel(std::pow(std::clamp(baked, 0.0, 1.0), 1.0 / 2.2)));
            }
            line[x * 4 + 3] = alpha;
        }
    }
    return spec;
}

QImage withAlpha(const QImage& rgb, const QImage& alpha8)
{
    if (rgb.isNull() || alpha8.isNull()) return rgb;
    QImage out = rgb.convertToFormat(QImage::Format_RGBA8888);
    QImage alpha = alpha8.convertToFormat(QImage::Format_Grayscale8);
    if (alpha.size() != out.size())
        alpha = alpha.scaled(out.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    for (int y = 0; y < out.height(); ++y) {
        uchar* line = out.scanLine(y);
        const uchar* alphaLine = alpha.constScanLine(y);
        for (int x = 0; x < out.width(); ++x)
            line[x * 4 + 3] = alphaLine[x];
    }
    return out;
}

QString specularPathFor(const QString& rmaosPath)
{
    const QFileInfo info(rmaosPath);
    const QString fileName = info.fileName();
    const QString directory = rmaosPath.left(rmaosPath.size() - fileName.size());
    QString stem = info.completeBaseName();
    static const QRegularExpression rmaosSuffix(QStringLiteral("_rmaos$"), QRegularExpression::CaseInsensitiveOption);
    if (rmaosSuffix.match(stem).hasMatch())
        stem.replace(rmaosSuffix, QStringLiteral("_spec"));
    else
        stem += QStringLiteral("_spec");
    return directory + stem + QStringLiteral(".dds");
}

bool repackRmaosSlot(Pbr::MaterialDocument* document, const QString& setName, const QString& slotKey,
    const QString& textureRoot, QString* error)
{
    if (!document) return fail(error, QStringLiteral("no document"));
    if (isArraySlot(slotKey))
        return fail(error, QStringLiteral("%1/%2 is a texture array; repack its slices by hand and clear the marker").arg(setName, slotKey));
    Pbr::TextureSlotState rmaosSlot = document->textureSlot(setName, slotKey);
    const QString rmaosFile = TextureImage::resolvePath(rmaosSlot.path, textureRoot);
    if (rmaosFile.isEmpty())
        return fail(error, QStringLiteral("%1/%2 cannot be resolved: %3").arg(setName, slotKey, rmaosSlot.path));
    QString loadError;
    const QImage rmaos = TextureImage::load(rmaosSlot.path, textureRoot, &loadError);
    if (rmaos.isNull())
        return fail(error, QStringLiteral("%1/%2 cannot be loaded: %3").arg(setName, slotKey, loadError));

    const double materialIor = document->setting(QStringLiteral("Specular"), QStringLiteral("ior"), 1.5).toDouble();
    const double iorMax = document->setting(QStringLiteral("Specular"), QStringLiteral("iorMax"), 4.25).toDouble();

    if (rmaos.hasAlphaChannel()) {
        const F0Repack repack = repackF0Alpha(rmaos, materialIor, iorMax);
        if (!backup(rmaosFile, error)) return false;
        if (!TextureWriter::saveDds(repack.rmaos, rmaosFile, TextureWriter::DdsFormat::Bc7, true, error)) return false;

        if (!repack.iorAlpha.isNull()) {
            const QString specKey = slotKey.left(slotKey.size() - QStringLiteral("Rmaos").size()) + QStringLiteral("SpecularColor");
            Pbr::TextureSlotState specSlot = document->textureSlot(setName, specKey);
            QImage spec;
            QString specFile;
            if (specSlot.enabled && !specSlot.path.trimmed().isEmpty()) {
                spec = TextureImage::load(specSlot.path, textureRoot, &loadError);
                if (spec.isNull())
                    return fail(error, QStringLiteral("%1/%2 cannot be loaded: %3").arg(setName, specKey, loadError));
                if (specSlot.values.value(QStringLiteral("legacyTintAlpha")).toBool(false))
                    spec = bakeTintAlpha(spec, materialIor, iorMax);
                specFile = TextureImage::resolvePath(specSlot.path, textureRoot);
                if (specFile.isEmpty())
                    return fail(error, QStringLiteral("%1/%2 cannot be resolved: %3").arg(setName, specKey, specSlot.path));
                if (!backup(specFile, error)) return false;
            } else {
                spec = QImage(rmaos.size(), QImage::Format_RGBA8888);
                spec.fill(Qt::white);
                specSlot.path = specularPathFor(rmaosSlot.path);
                specSlot.enabled = true;
                specFile = QFileInfo(rmaosFile).dir().filePath(QFileInfo(specSlot.path).fileName());
            }
            spec = withAlpha(spec, repack.iorAlpha);
            if (!TextureWriter::saveDds(spec, specFile, TextureWriter::DdsFormat::Bc7Srgb, true, error)) return false;
            specSlot.values.insert(QStringLiteral("iorInAlpha"), true);
            specSlot.values.remove(QStringLiteral("legacyTintAlpha"));
            document->setTextureSlot(setName, specKey, specSlot);
        }
        rmaosSlot.values.insert(QStringLiteral("overrideSpecularWeight"), false);
        rmaosSlot.values.insert(QStringLiteral("alphaCarries"), QStringLiteral("Specular Weight"));
    }
    // An opaque texture never carried an F0: nothing to rewrite, the constant
    // override the loader forced stays in charge.
    rmaosSlot.values.remove(QStringLiteral("legacyF0Alpha"));
    document->setTextureSlot(setName, slotKey, rmaosSlot);
    return true;
}

bool bakeTintSlot(Pbr::MaterialDocument* document, const QString& setName, const QString& slotKey,
    const QString& textureRoot, QString* error)
{
    if (!document) return fail(error, QStringLiteral("no document"));
    Pbr::TextureSlotState specSlot = document->textureSlot(setName, slotKey);
    const QString specFile = TextureImage::resolvePath(specSlot.path, textureRoot);
    if (specFile.isEmpty())
        return fail(error, QStringLiteral("%1/%2 cannot be resolved: %3").arg(setName, slotKey, specSlot.path));
    QString loadError;
    const QImage spec = TextureImage::load(specSlot.path, textureRoot, &loadError);
    if (spec.isNull())
        return fail(error, QStringLiteral("%1/%2 cannot be loaded: %3").arg(setName, slotKey, loadError));
    const double materialIor = document->setting(QStringLiteral("Specular"), QStringLiteral("ior"), 1.5).toDouble();
    const double iorMax = document->setting(QStringLiteral("Specular"), QStringLiteral("iorMax"), 4.25).toDouble();
    if (spec.hasAlphaChannel()) {
        if (!backup(specFile, error)) return false;
        if (!TextureWriter::saveDds(bakeTintAlpha(spec, materialIor, iorMax), specFile, TextureWriter::DdsFormat::Bc7Srgb, true, error))
            return false;
        specSlot.values.insert(QStringLiteral("iorInAlpha"), true);
    }
    specSlot.values.remove(QStringLiteral("legacyTintAlpha"));
    document->setTextureSlot(setName, slotKey, specSlot);
    return true;
}

int repackDocument(Pbr::MaterialDocument* document, const QString& textureRoot, QString* error, QStringList* report)
{
    if (!document) {
        fail(error, QStringLiteral("no document"));
        return -1;
    }
    int written = 0;
    const QStringList sets{QStringLiteral("UV0"), QStringLiteral("UV1"), QStringLiteral("UV2"), QStringLiteral("Landscape")};
    for (const QString& setName : sets) {
        const QJsonObject set = document->textureSet(setName);
        // RMAOS first: its IOR map may also consume the colour slot's tint marker.
        for (auto it = set.begin(); it != set.end(); ++it) {
            const QJsonObject slot = it.value().toObject();
            const QJsonObject values = slot.value(QStringLiteral("values")).toObject();
            if (!values.value(QStringLiteral("legacyF0Alpha")).toBool(false)) continue;
            if (!repackRmaosSlot(document, setName, it.key(), textureRoot, error)) return -1;
            ++written;
            if (report) *report << QStringLiteral("%1/%2: alpha repacked as specular weight").arg(setName, it.key());
        }
        const QJsonObject after = document->textureSet(setName);
        for (auto it = after.begin(); it != after.end(); ++it) {
            const QJsonObject values = it.value().toObject().value(QStringLiteral("values")).toObject();
            if (!values.value(QStringLiteral("legacyTintAlpha")).toBool(false)) continue;
            if (!bakeTintSlot(document, setName, it.key(), textureRoot, error)) return -1;
            ++written;
            if (report) *report << QStringLiteral("%1/%2: tint weight baked into RGB, alpha is the IOR map").arg(setName, it.key());
        }
    }
    return written;
}

} // namespace SpecularMigrate
