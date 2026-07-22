#include "converter.h"

#include "bgsmreader.h"
#include "textureimage.h"
#include "texturewriter.h"

#include <QColor>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>

namespace Converter {

namespace {

// Locates the "materials" component so siblings keep their Data-relative
// paths in the output tree; falls back to the bare file name.
QString materialsRelativePath(const QString& materialPath)
{
    const QString normalized = QDir::fromNativeSeparators(materialPath);
    const QStringList parts = normalized.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (int i = parts.size() - 1; i >= 0; --i) {
        if (parts[i].compare(QLatin1String("materials"), Qt::CaseInsensitive) == 0)
            return parts.mid(i + 1).join(QLatin1Char('/'));
    }
    return QFileInfo(materialPath).fileName();
}

// The Data root above the materials tree, for resolving root-material chains.
QString dataRootFor(const QString& materialPath)
{
    const QString normalized = QDir::fromNativeSeparators(materialPath);
    const int index = normalized.toLower().lastIndexOf(QLatin1String("/materials/"));
    return index >= 0 ? normalized.left(index) : QFileInfo(materialPath).absolutePath();
}

// Portable texture output location: mirrors the source texture's
// Textures-relative directory; falls back beside the material's rel path.
QString portableOutputDirectory(const QString& sourceTexturePath, const QString& materialRelative)
{
    const QString normalized = QDir::fromNativeSeparators(sourceTexturePath);
    const QStringList parts = normalized.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (int i = 0; i < parts.size(); ++i) {
        if (parts[i].compare(QLatin1String("textures"), Qt::CaseInsensitive) == 0)
            return parts.mid(i + 1, parts.size() - i - 2).join(QLatin1Char('/'));
    }
    const QStringList relParts = normalized.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (relParts.size() > 1) return relParts.mid(0, relParts.size() - 1).join(QLatin1Char('/'));
    return QFileInfo(materialRelative).path();
}

QString baseNameForOutputs(const QString& sourceTexturePath)
{
    QString base = QFileInfo(sourceTexturePath).completeBaseName();
    for (const QString& suffix : {QStringLiteral("_d"), QStringLiteral("_diffuse")}) {
        if (base.endsWith(suffix, Qt::CaseInsensitive)) {
            base.chop(suffix.size());
            break;
        }
    }
    return base;
}

QString shortHash(const QString& signature)
{
    return QString::fromLatin1(QCryptographicHash::hash(signature.toUtf8(), QCryptographicHash::Sha1).toHex().left(8));
}

QImage loadSource(const QString& path, const QString& textureRoot, QStringList* notes, const QString& label,
    bool* missing = nullptr)
{
    if (path.trimmed().isEmpty()) return {};
    QString error;
    const QImage image = TextureImage::load(path, textureRoot, &error);
    if (image.isNull()) {
        if (notes) notes->append(label + QStringLiteral(" missing: ") + path);
        if (missing) *missing = true;
    }
    return image;
}

// Mean color of the referenced cubemap (mid mip, luminance-weighted), for the
// black-metal hue donor. Cached per cubemap path by the caller.
bool cubemapMeanColor(const QString& path, const QString& textureRoot, QVector3D* tint)
{
    if (path.trimmed().isEmpty()) return false;
    QString error;
    const QVector<QImage> faces = TextureImage::loadCubemapFaces(path, textureRoot, &error);
    if (faces.size() != 6) return false;
    double sumR = 0, sumG = 0, sumB = 0, weightSum = 0;
    for (const QImage& faceImage : faces) {
        const QImage face = faceImage.convertToFormat(QImage::Format_RGBA8888)
            .scaled(16, 16, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        for (int y = 0; y < face.height(); ++y) {
            const uchar* line = face.constScanLine(y);
            for (int x = 0; x < face.width(); ++x) {
                const float r = srgbToLinear(line[x * 4] / 255.0f);
                const float g = srgbToLinear(line[x * 4 + 1] / 255.0f);
                const float b = srgbToLinear(line[x * 4 + 2] / 255.0f);
                const double weight = 0.05 + luminance({r, g, b});
                sumR += r * weight;
                sumG += g * weight;
                sumB += b * weight;
                weightSum += weight;
            }
        }
    }
    if (weightSum <= 0) return false;
    *tint = QVector3D(float(sumR / weightSum), float(sumG / weightSum), float(sumB / weightSum));
    return true;
}

// Writes one converted texture, honoring the dedup cache and hash-suffix
// collision policy. Returns the portable path ("dir/name_BC.dds").
QString writeConverted(const QImage& image, const QString& signature, const QString& portableDirectory,
    const QString& baseName, const QString& suffix, TextureWriter::DdsFormat format,
    const QImage& toksvigNormal, const JobOptions& options, TextureCache* cache,
    QHash<QString, QString>& signatureMap, QStringList* notes, QStringList* errors)
{
    if (image.isNull()) return {};
    if (signatureMap.contains(signature)) return signatureMap.value(signature);
    QString portable = portableDirectory.isEmpty()
        ? baseName + suffix
        : portableDirectory + QLatin1Char('/') + baseName + suffix;
    QString absolute = options.outputRoot + QStringLiteral("/Textures/") + portable;
    if (cache->claimedOutputs.contains(QDir::cleanPath(absolute).toLower())
        && cache->claimedOutputs.value(QDir::cleanPath(absolute).toLower()) != signature) {
        // Same source texture, different pairing: disambiguate, never overwrite.
        const QString hashed = baseName + QStringLiteral("_") + shortHash(signature) + suffix;
        portable = portableDirectory.isEmpty() ? hashed : portableDirectory + QLatin1Char('/') + hashed;
        absolute = options.outputRoot + QStringLiteral("/Textures/") + portable;
        if (notes) notes->append(QStringLiteral("pairing divergence: wrote ") + portable);
    }
    if (!options.dryRun) {
        QDir().mkpath(QFileInfo(absolute).absolutePath());
        QString error;
        if (!TextureWriter::saveDds(image, absolute, format, true, &error, toksvigNormal)) {
            if (errors) errors->append(portable + QStringLiteral(": ") + error);
            return {};
        }
    }
    cache->claimedOutputs.insert(QDir::cleanPath(absolute).toLower(), signature);
    signatureMap.insert(signature, portable);
    return portable;
}

void wireSlot(Pbr::MaterialDocument* document, const QString& key, const QString& path,
    const QJsonObject& values = {})
{
    Pbr::TextureSlotState state;
    state.enabled = true;
    state.expanded = false;
    state.path = QDir::toNativeSeparators(path);
    state.values = values;
    document->setTextureSlot(QStringLiteral("UV0"), key, state);
}

} // namespace

JobResult convertMaterial(const QString& materialPath, const JobOptions& options, TextureCache* cache)
{
    JobResult result;
    result.materialPath = materialPath;
    TextureCache localCache;
    if (!cache) cache = &localCache;

    Bgsm::Material material;
    QString error;
    if (!Bgsm::readFile(materialPath, &material, &error)) {
        result.errors.append(error);
        return result;
    }
    // Root template chains: children are frequently texture-sparse.
    const QString dataRoot = dataRootFor(materialPath);
    const auto lookup = [&dataRoot, &options](const QString& path, QByteArray* data) {
        const QString normalized = QDir::fromNativeSeparators(path);
        for (const QString& candidate : {dataRoot + QLatin1Char('/') + normalized,
                 options.textureRoot + QLatin1Char('/') + normalized,
                 dataRoot + QStringLiteral("/materials/") + normalized}) {
            QFile file(candidate);
            if (file.open(QIODevice::ReadOnly)) {
                *data = file.readAll();
                return true;
            }
        }
        return false;
    };
    QString chainError;
    if (!material.rootMaterialPath.trimmed().isEmpty() && !Bgsm::resolveRootChain(&material, lookup, &chainError)) {
        result.notes.append(QStringLiteral("root chain: ") + chainError);
        result.flagged = true;
    }

    const QString materialRelative = materialsRelativePath(materialPath);
    result.pbrmPath = options.outputRoot + QStringLiteral("/Materials/") + materialRelative;
    result.pbrmPath.replace(QRegularExpression(QStringLiteral("\\.(bgsm|bgem)$"), QRegularExpression::CaseInsensitiveOption),
        QStringLiteral(".pbrm"));

    // ---- Source textures ----
    const bool bgem = material.kind == Bgsm::Kind::Bgem;
    const QString diffusePath = bgem ? material.baseTexture : material.diffuseTexture;
    bool missingCritical = false;
    const QImage diffuse = loadSource(diffusePath, options.textureRoot, &result.notes, QStringLiteral("diffuse"), &missingCritical);
    if (diffuse.isNull()) {
        if (bgem && diffusePath.trimmed().isEmpty()) {
            // Texture-free effect material (editor markers, colored glass):
            // the whole appearance is the constant color — a constants-only
            // sibling is the correct conversion, not a failure.
            Pbr::MaterialDocument document;
            mapParameters(material, &document, &result.notes);
            Pbr::TextureSlotState base;
            base.enabled = true;
            base.expanded = false;
            base.values = QJsonObject{
                {QStringLiteral("overrideColor"), true},
                {QStringLiteral("color"), QColor::fromRgbF(material.baseColor.x(), material.baseColor.y(), material.baseColor.z()).name()},
                {QStringLiteral("overrideOpacity"), true},
                {QStringLiteral("opacity"), double(material.alpha)}};
            document.setTextureSlot(QStringLiteral("UV0"), QStringLiteral("primaryBaseColor"), base);
            result.notes.append(QStringLiteral("texture-free effect material: constants-only sibling"));
            if (!options.dryRun) {
                QDir().mkpath(QFileInfo(result.pbrmPath).absolutePath());
                if (!document.save(result.pbrmPath, &error)) {
                    result.errors.append(QStringLiteral("sibling save failed: ") + error);
                    return result;
                }
            }
            result.converted = true;
            return result;
        }
        result.errors.append(QStringLiteral("no usable diffuse/base texture"));
        result.flagged = true;
        return result;
    }
    const QImage normal = loadSource(material.normalTexture, options.textureRoot, &result.notes, QStringLiteral("normal"));

    // ---- Profile conversion ----
    ConvertedImages converted;
    const bool fallout76 = material.isFallout76();
    if (fallout76) {
        Fo76Sources sources;
        sources.diffuse = diffuse;
        sources.reflectance = loadSource(material.specularTexture, options.textureRoot, &result.notes, QStringLiteral("reflectance (_r)"));
        sources.lighting = loadSource(material.lightingTexture, options.textureRoot, &result.notes, QStringLiteral("lighting (_l)"));
        sources.glow = loadSource(material.glowTexture, options.textureRoot, &result.notes, QStringLiteral("glow"));
        converted = convertFo76(sources, material, options.math);
    } else {
        Fo4Sources sources;
        sources.diffuse = diffuse;
        sources.specular = loadSource(material.smoothSpecTexture, options.textureRoot, &result.notes, QStringLiteral("specular (_s)"));
        sources.glow = loadSource(material.glowTexture, options.textureRoot, &result.notes, QStringLiteral("glow"));
        sources.envMask = loadSource(material.envmapMaskTexture, options.textureRoot, &result.notes, QStringLiteral("env mask"));
        sources.hasCubemapTint = cubemapMeanColor(material.envmapTexture, options.textureRoot, &sources.cubemapTint);
        converted = convertFo4(sources, material, options.math);
        if (material.environmentMapping && !material.envmapTexture.trimmed().isEmpty() && !sources.hasCubemapTint)
            result.notes.append(QStringLiteral("cubemap unavailable for tint: ") + material.envmapTexture);
    }
    if (converted.baseColor.isNull()) {
        result.errors.append(QStringLiteral("conversion produced no base color"));
        return result;
    }
    result.metalCoverage = converted.metalCoverage;

    // ---- Derivations (curvature always; height/AO per options) ----
    NormalDerive::Results derived;
    if (!normal.isNull())
        derived = NormalDerive::derive(normal, options.derive);
    if (derived.denoised)
        result.notes.append(QStringLiteral("derivation-side denoise engaged (noise metric %1)").arg(derived.noiseMetric, 0, 'f', 3));

    // Detail AO joins RMAOS.B. FO76 ships baked AO in _l.G already; the
    // derived detail pass is FO4's (and an option elsewhere).
    QImage rmaos = converted.rmaos;
    if (options.detailAo && !fallout76 && !derived.ao.isNull() && !rmaos.isNull()) {
        const QImage ao = derived.ao.scaled(rmaos.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        for (int y = 0; y < rmaos.height(); ++y) {
            uchar* rmaosLine = rmaos.scanLine(y);
            const uchar* aoLine = ao.constScanLine(y);
            for (int x = 0; x < rmaos.width(); ++x)
                rmaosLine[x * 4 + 2] = uchar(int(rmaosLine[x * 4 + 2]) * int(aoLine[x * 4]) / 255);
        }
    }

    // Shipped normal = original RG + derived curvature in A (+ height in B
    // when requested). Without a normal map the slots stay texture-free.
    QImage packedNormal;
    if (!normal.isNull()) {
        packedNormal = normal.convertToFormat(QImage::Format_RGBA8888);
        const QImage curvature = derived.curvature.isNull() ? QImage()
            : derived.curvature.scaled(packedNormal.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        const QImage heightImage = (options.heightInBlue && !derived.height.isNull())
            ? derived.height.scaled(packedNormal.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation) : QImage();
        for (int y = 0; y < packedNormal.height(); ++y) {
            uchar* line = packedNormal.scanLine(y);
            const uchar* curvatureLine = curvature.isNull() ? nullptr : curvature.constScanLine(y);
            const uchar* heightLine = heightImage.isNull() ? nullptr : heightImage.constScanLine(y);
            for (int x = 0; x < packedNormal.width(); ++x) {
                line[x * 4 + 2] = heightLine ? heightLine[x * 4] : line[x * 4 + 2];
                line[x * 4 + 3] = curvatureLine ? curvatureLine[x * 4] : 128;
            }
        }
    }

    // ---- Outputs (dedup'd, collision-safe) ----
    const QString portableDirectory = portableOutputDirectory(diffusePath, materialRelative);
    const QString baseName = baseNameForOutputs(diffusePath);
    const QString optionsSignature = QStringLiteral("|rs%1|f%2|gc%3|em%4|")
        .arg(options.math.recoveryStrength).arg(fallout76).arg(options.math.glossCurve)
        .arg(material.environmentMappingMaskScale);
    const QString pairSignature = diffusePath.toLower() + QLatin1Char('+')
        + (bgem ? QString() : material.smoothSpecTexture.toLower()) + QLatin1Char('+')
        + material.specularTexture.toLower() + QLatin1Char('+') + material.lightingTexture.toLower()
        + QLatin1Char('+') + material.envmapTexture.toLower() + optionsSignature;

    const auto recordTexture = [&result](const QString& portable) {
        if (!portable.isEmpty() && !result.writtenTextures.contains(portable))
            result.writtenTextures.append(portable);
    };
    const QString basePortable = writeConverted(converted.baseColor, pairSignature, portableDirectory, baseName,
        QStringLiteral("_BC.dds"), TextureWriter::DdsFormat::Bc7Srgb, QImage(), options, cache,
        cache->baseColorBySignature, &result.notes, &result.errors);
    recordTexture(basePortable);
    const QString rmaosPortable = writeConverted(rmaos, pairSignature + QStringLiteral("#rmaos"), portableDirectory,
        baseName, QStringLiteral("_RMAOS.dds"), TextureWriter::DdsFormat::Bc7, normal, options, cache,
        cache->rmaosBySignature, &result.notes, &result.errors);
    recordTexture(rmaosPortable);
    QString normalPortable;
    if (!packedNormal.isNull()) {
        const QString normalSignature = material.normalTexture.toLower() + QStringLiteral("#n")
            + QString::number(options.heightInBlue);
        normalPortable = writeConverted(packedNormal, normalSignature, portableDirectory, baseName,
            QStringLiteral("_N.dds"), TextureWriter::DdsFormat::Bc7, QImage(), options, cache,
            cache->normalBySignature, &result.notes, &result.errors);
        recordTexture(normalPortable);
    }
    QString emissivePortable;
    if (!converted.emissive.isNull()) {
        emissivePortable = writeConverted(converted.emissive, pairSignature + QStringLiteral("#e"), portableDirectory,
            baseName, QStringLiteral("_E.dds"), TextureWriter::DdsFormat::Bc7Srgb, QImage(), options, cache,
            cache->emissiveBySignature, &result.notes, &result.errors);
        recordTexture(emissivePortable);
    }
    if (options.writeMetallicDiagnostic && !converted.metallic.isNull())
        recordTexture(writeConverted(converted.metallic, pairSignature + QStringLiteral("#m"), portableDirectory, baseName,
            QStringLiteral("_MetalMask.dds"), TextureWriter::DdsFormat::Bc4, QImage(), options, cache,
            cache->emissiveBySignature, &result.notes, &result.errors));

    // ---- The sibling .pbrm ----
    Pbr::MaterialDocument document;
    mapParameters(material, &document, &result.notes);
    if (!basePortable.isEmpty())
        wireSlot(&document, QStringLiteral("primaryBaseColor"), basePortable,
            bgem ? QJsonObject{{QStringLiteral("tint"),
                QColor::fromRgbF(material.baseColor.x(), material.baseColor.y(), material.baseColor.z()).name()}} : QJsonObject{});
    if (!normalPortable.isEmpty()) {
        QJsonObject values{{QStringLiteral("curvatureInAlpha"), true}};
        if (options.heightInBlue) values.insert(QStringLiteral("heightInBlue"), true);
        wireSlot(&document, QStringLiteral("primaryNormal"), normalPortable, values);
    }
    if (!rmaosPortable.isEmpty()) {
        Pbr::TextureSlotState rmaosSlot = document.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryRmaos"));
        rmaosSlot.enabled = true;
        rmaosSlot.expanded = false;
        rmaosSlot.path = QDir::toNativeSeparators(rmaosPortable);
        document.setTextureSlot(QStringLiteral("UV0"), QStringLiteral("primaryRmaos"), rmaosSlot);
    }
    if (!emissivePortable.isEmpty())
        wireSlot(&document, QStringLiteral("primaryEmissive"), emissivePortable,
            QJsonObject{{QStringLiteral("intensity"), double(std::max(material.emittanceMult, 0.0f))}});

    // Review flags for the workspace queue.
    if (!fallout76 && converted.metalCoverage > 0.02f && converted.metalCoverage < 0.98f
        && channelPercentile(converted.metallic, 0, 0.5f) > 0.35f && channelPercentile(converted.metallic, 0, 0.5f) < 0.65f) {
        result.notes.append(QStringLiteral("ambiguous metallic inference; review with the correction tools"));
        result.flagged = true;
    }

    if (!options.dryRun) {
        QDir().mkpath(QFileInfo(result.pbrmPath).absolutePath());
        if (!document.save(result.pbrmPath, &error)) {
            result.errors.append(QStringLiteral("sibling save failed: ") + error);
            return result;
        }
    }
    result.converted = result.errors.isEmpty();
    return result;
}

} // namespace Converter
