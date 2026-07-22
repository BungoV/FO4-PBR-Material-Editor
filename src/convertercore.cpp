#include "convertercore.h"

#include <QColor>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace Converter {

namespace {

float smoothstepf(float edge0, float edge1, float value)
{
    if (qFuzzyCompare(edge0, edge1)) return value < edge0 ? 0.0f : 1.0f;
    const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

QImage toRgba(const QImage& image)
{
    return image.isNull() ? image : image.convertToFormat(QImage::Format_RGBA8888);
}

// Samples an auxiliary map at a texel of the base resolution, nearest with
// coordinate scaling so mismatched resolutions still pair up.
struct ScaledReader {
    QImage image;
    int width = 0, height = 0;
    ScaledReader(const QImage& source, int baseWidth, int baseHeight)
        : image(toRgba(source)), width(baseWidth), height(baseHeight) {}
    bool valid() const { return !image.isNull(); }
    const uchar* texel(int x, int y) const {
        const int sx = image.width() == width ? x : x * image.width() / qMax(1, width);
        const int sy = image.height() == height ? y : y * image.height() / qMax(1, height);
        return image.constScanLine(qBound(0, sy, image.height() - 1)) + qBound(0, sx, image.width() - 1) * 4;
    }
};

} // namespace

float srgbToLinear(float value)
{
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

float linearToSrgb(float value)
{
    value = std::clamp(value, 0.0f, 1.0f);
    return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

QVector3D srgbToLinear(const QVector3D& color)
{
    return {srgbToLinear(color.x()), srgbToLinear(color.y()), srgbToLinear(color.z())};
}

QVector3D linearToSrgb(const QVector3D& color)
{
    return {linearToSrgb(color.x()), linearToSrgb(color.y()), linearToSrgb(color.z())};
}

float luminance(const QVector3D& linearColor)
{
    return 0.2126f * linearColor.x() + 0.7152f * linearColor.y() + 0.0722f * linearColor.z();
}

float glossToRoughness(float gloss, const Options& options)
{
    const float inverted = 1.0f - std::clamp(gloss, 0.0f, 1.0f);
    return std::clamp(std::pow(inverted, options.glossCurve), 0.0f, 1.0f);
}

float metallicConfidenceFo4(float specStrength, float diffuseLinearLuma,
    bool environmentMapping, float environmentMaskScale, float environmentMask,
    const Options& options)
{
    if (!environmentMapping) return 0.0f;
    const float envSignal = std::clamp(environmentMaskScale, 0.0f, 1.0f) * std::clamp(environmentMask, 0.0f, 1.0f);
    const float specSignal = smoothstepf(options.metalSpecStart, options.metalSpecFull, specStrength);
    // Darker diffuse means the cubemap was meant to carry the appearance.
    const float darkSignal = smoothstepf(options.metalDarkStart, options.metalDarkFull, diffuseLinearLuma);
    return std::clamp(envSignal * specSignal * darkSignal, 0.0f, 1.0f);
}

float metallicConfidenceFo76(const QVector3D& reflectanceLinear,
    float diffuseLinearLuma, const Options& options)
{
    // True specular workflow: metals carry bright reflectance AND near-black
    // diffuse; the joint signal is nearly foolproof.
    const float reflectanceSignal = smoothstepf(options.fo76ReflectanceStart, options.fo76ReflectanceFull,
        luminance(reflectanceLinear));
    const float darkSignal = smoothstepf(options.fo76DarkStart, options.fo76DarkFull, diffuseLinearLuma);
    return std::clamp(reflectanceSignal * darkSignal, 0.0f, 1.0f);
}

float applyMetallicCorrection(float inferred, float maskValue)
{
    const float strength = std::clamp(std::abs(maskValue - 0.5f) * 2.0f, 0.0f, 1.0f);
    const float painted = maskValue >= 0.5f ? 1.0f : 0.0f;
    return inferred + (painted - inferred) * strength;
}

QVector3D clampTintSaturation(const QVector3D& linearColor, float maxSaturation)
{
    const float luma = luminance(linearColor);
    const QVector3D gray(luma, luma, luma);
    const QVector3D chroma = linearColor - gray;
    const float saturation = std::max({std::abs(chroma.x()), std::abs(chroma.y()), std::abs(chroma.z())})
        / std::max(luma, 1e-4f);
    if (saturation <= maxSaturation || saturation <= 0.0f) return linearColor;
    return gray + chroma * (maxSaturation / saturation);
}

QVector3D recoverMetalAlbedoFo4(const QVector3D& diffuseLinear, float specStrength,
    const QVector3D& cubemapTintLinear, bool hasCubemapTint, const Options& options)
{
    const float diffuseLuma = luminance(diffuseLinear);
    // Hue source: the diffuse when it carries any usable color, the cubemap
    // mean when the metal was authored fully black (the cubemap choice IS the
    // color authoring in that case).
    const float reliability = std::clamp(diffuseLuma / std::max(options.diffuseReliabilityThreshold, 1e-4f), 0.0f, 1.0f);
    QVector3D tint = hasCubemapTint
        ? clampTintSaturation(cubemapTintLinear, options.cubemapTintSaturationClamp)
        : QVector3D(0.5f, 0.5f, 0.5f);
    QVector3D hueSource = tint * (1.0f - reliability) + diffuseLinear * reliability;
    const float hueLuma = luminance(hueSource);
    if (hueLuma < 1e-4f) hueSource = QVector3D(0.5f, 0.5f, 0.5f);
    // Normalize the hue to unit luminance, then place brightness on the metal
    // F0 window from spec strength.
    const QVector3D normalizedHue = hueSource / std::max(luminance(hueSource), 1e-4f);
    const float brightness = options.metalF0Min + (options.metalF0Max - options.metalF0Min)
        * std::clamp(specStrength, 0.0f, 1.0f);
    QVector3D recovered = normalizedHue * brightness;
    // Strongly hued colors can push their dominant channel past 1 after
    // luminance normalization; rescale by the maximum instead of clamping
    // per channel, so the hue survives.
    const float maxChannel = std::max({recovered.x(), recovered.y(), recovered.z()});
    if (maxChannel > 1.0f) recovered /= maxChannel;
    return {std::max(recovered.x(), 0.0f), std::max(recovered.y(), 0.0f), std::max(recovered.z(), 0.0f)};
}

float channelPercentile(const QImage& image, int channel, float percentile)
{
    const QImage source = toRgba(image);
    if (source.isNull() || channel < 0 || channel > 3) return 1.0f;
    int histogram[256] = {};
    for (int y = 0; y < source.height(); ++y) {
        const uchar* line = source.constScanLine(y);
        for (int x = 0; x < source.width(); ++x)
            ++histogram[line[x * 4 + channel]];
    }
    const qint64 total = qint64(source.width()) * source.height();
    const qint64 target = qint64(std::clamp(percentile, 0.0f, 1.0f) * total);
    qint64 running = 0;
    for (int value = 0; value < 256; ++value) {
        running += histogram[value];
        if (running >= target) return value / 255.0f;
    }
    return 1.0f;
}

ConvertedImages convertFo4(const Fo4Sources& sources, const Bgsm::Material& material, const Options& options)
{
    ConvertedImages result;
    const QImage diffuse = toRgba(sources.diffuse);
    if (diffuse.isNull()) return result;
    const int width = diffuse.width(), height = diffuse.height();
    const ScaledReader specular(sources.specular, width, height);
    const ScaledReader envMask(sources.envMask, width, height);
    const ScaledReader correction(sources.correctionMask, width, height);

    // Batch-mode spec normalization: blown-out vanilla spec maps otherwise
    // convert to mirror chrome.
    float specScale = 1.0f;
    if (options.normalizeSpec && specular.valid()) {
        const float percentile = channelPercentile(sources.specular, 0, options.specNormalizePercentile);
        specScale = 1.0f / std::max(percentile, options.specNormalizeFloor);
    }
    const float specMult = std::clamp(material.specularMult, 0.0f, 4.0f);

    result.baseColor = QImage(width, height, QImage::Format_RGBA8888);
    result.rmaos = QImage(width, height, QImage::Format_RGBA8888);
    result.metallic = QImage(width, height, QImage::Format_RGBA8888);
    qint64 metalTexels = 0;

    for (int y = 0; y < height; ++y) {
        const uchar* diffuseLine = diffuse.constScanLine(y);
        uchar* baseLine = result.baseColor.scanLine(y);
        uchar* rmaosLine = result.rmaos.scanLine(y);
        uchar* metallicLine = result.metallic.scanLine(y);
        for (int x = 0; x < width; ++x) {
            const QVector3D diffuseSrgb(diffuseLine[x * 4] / 255.0f, diffuseLine[x * 4 + 1] / 255.0f, diffuseLine[x * 4 + 2] / 255.0f);
            const QVector3D diffuseLinear = srgbToLinear(diffuseSrgb);
            float specStrength = 0.0f, gloss = material.smoothness;
            if (specular.valid()) {
                const uchar* specTexel = specular.texel(x, y);
                specStrength = std::clamp(specTexel[0] / 255.0f * specScale, 0.0f, 1.0f);
                gloss = specTexel[1] / 255.0f;
            }
            specStrength = std::clamp(specStrength * specMult, 0.0f, 1.0f);
            const float mask = envMask.valid() ? envMask.texel(x, y)[0] / 255.0f : 1.0f;
            float metallic = metallicConfidenceFo4(specStrength, luminance(diffuseLinear),
                material.environmentMapping, material.environmentMappingMaskScale, mask, options);
            if (correction.valid())
                metallic = applyMetallicCorrection(metallic, correction.texel(x, y)[0] / 255.0f);
            if (metallic > 0.5f) ++metalTexels;

            const QVector3D metalAlbedo = recoverMetalAlbedoFo4(diffuseLinear, specStrength,
                sources.cubemapTint, sources.hasCubemapTint, options);
            const float blend = metallic * std::clamp(options.recoveryStrength, 0.0f, 1.0f);
            const QVector3D baseLinear = diffuseLinear * (1.0f - blend) + metalAlbedo * blend;
            const QVector3D baseSrgb = linearToSrgb(baseLinear);
            baseLine[x * 4] = uchar(std::lround(baseSrgb.x() * 255.0f));
            baseLine[x * 4 + 1] = uchar(std::lround(baseSrgb.y() * 255.0f));
            baseLine[x * 4 + 2] = uchar(std::lround(baseSrgb.z() * 255.0f));
            baseLine[x * 4 + 3] = diffuseLine[x * 4 + 3];

            const float roughness = glossToRoughness(gloss, options);
            const float dielectricF0 = std::clamp(specStrength * 0.08f,
                options.dielectricF0Min, options.dielectricF0Max);
            rmaosLine[x * 4] = uchar(std::lround(roughness * 255.0f));
            rmaosLine[x * 4 + 1] = uchar(std::lround(metallic * 255.0f));
            rmaosLine[x * 4 + 2] = 255; // AO joins from the derivation pass
            rmaosLine[x * 4 + 3] = uchar(std::lround(dielectricF0 * 255.0f));
            const uchar metallicByte = uchar(std::lround(metallic * 255.0f));
            metallicLine[x * 4] = metallicByte;
            metallicLine[x * 4 + 1] = metallicByte;
            metallicLine[x * 4 + 2] = metallicByte;
            metallicLine[x * 4 + 3] = 255;
        }
    }
    result.metalCoverage = float(double(metalTexels) / (double(width) * height));

    // Emissive: the FO4 glow map is a grayscale mask; the color lives in the
    // material. Bake RGB = mask x emittance color (sRGB), A = raw mask.
    if (material.emitEnabled || !sources.glow.isNull()) {
        const QImage glow = toRgba(sources.glow);
        if (!glow.isNull()) {
            result.emissive = QImage(glow.width(), glow.height(), QImage::Format_RGBA8888);
            const QVector3D tint = material.emittanceColor;
            for (int y = 0; y < glow.height(); ++y) {
                const uchar* glowLine = glow.constScanLine(y);
                uchar* emissiveLine = result.emissive.scanLine(y);
                for (int x = 0; x < glow.width(); ++x) {
                    const float mask = glowLine[x * 4] / 255.0f;
                    emissiveLine[x * 4] = uchar(std::lround(std::clamp(mask * tint.x(), 0.0f, 1.0f) * 255.0f));
                    emissiveLine[x * 4 + 1] = uchar(std::lround(std::clamp(mask * tint.y(), 0.0f, 1.0f) * 255.0f));
                    emissiveLine[x * 4 + 2] = uchar(std::lround(std::clamp(mask * tint.z(), 0.0f, 1.0f) * 255.0f));
                    emissiveLine[x * 4 + 3] = uchar(std::lround(mask * 255.0f));
                }
            }
        }
    }
    return result;
}

ConvertedImages convertFo76(const Fo76Sources& sources, const Bgsm::Material& material, const Options& options)
{
    ConvertedImages result;
    const QImage diffuse = toRgba(sources.diffuse);
    if (diffuse.isNull()) return result;
    const int width = diffuse.width(), height = diffuse.height();
    const ScaledReader reflectance(sources.reflectance, width, height);
    const ScaledReader lighting(sources.lighting, width, height);
    const ScaledReader correction(sources.correctionMask, width, height);

    result.baseColor = QImage(width, height, QImage::Format_RGBA8888);
    result.rmaos = QImage(width, height, QImage::Format_RGBA8888);
    result.metallic = QImage(width, height, QImage::Format_RGBA8888);
    qint64 metalTexels = 0;

    for (int y = 0; y < height; ++y) {
        const uchar* diffuseLine = diffuse.constScanLine(y);
        uchar* baseLine = result.baseColor.scanLine(y);
        uchar* rmaosLine = result.rmaos.scanLine(y);
        uchar* metallicLine = result.metallic.scanLine(y);
        for (int x = 0; x < width; ++x) {
            const QVector3D diffuseSrgb(diffuseLine[x * 4] / 255.0f, diffuseLine[x * 4 + 1] / 255.0f, diffuseLine[x * 4 + 2] / 255.0f);
            const QVector3D diffuseLinear = srgbToLinear(diffuseSrgb);
            QVector3D reflectanceLinear(0.04f, 0.04f, 0.04f);
            if (reflectance.valid()) {
                const uchar* reflTexel = reflectance.texel(x, y);
                reflectanceLinear = QVector3D(reflTexel[0] / 255.0f, reflTexel[1] / 255.0f, reflTexel[2] / 255.0f);
            }
            float smoothness = material.smoothness, bakedAo = 1.0f;
            if (lighting.valid()) {
                const uchar* lightTexel = lighting.texel(x, y);
                smoothness = lightTexel[0] / 255.0f;
                bakedAo = lightTexel[1] / 255.0f;
            }
            float metallic = metallicConfidenceFo76(reflectanceLinear, luminance(diffuseLinear), options);
            if (correction.valid())
                metallic = applyMetallicCorrection(metallic, correction.texel(x, y)[0] / 255.0f);
            if (metallic > 0.5f) ++metalTexels;

            // Textbook specular -> metalness reconstruction: metals take hue
            // AND brightness from the reflectance map over black diffuse.
            const QVector3D baseLinear = diffuseLinear * (1.0f - metallic) + reflectanceLinear * metallic;
            const QVector3D baseSrgb = linearToSrgb(baseLinear);
            baseLine[x * 4] = uchar(std::lround(baseSrgb.x() * 255.0f));
            baseLine[x * 4 + 1] = uchar(std::lround(baseSrgb.y() * 255.0f));
            baseLine[x * 4 + 2] = uchar(std::lround(baseSrgb.z() * 255.0f));
            baseLine[x * 4 + 3] = diffuseLine[x * 4 + 3];

            const float roughness = glossToRoughness(smoothness, options);
            const float dielectricF0 = std::clamp(luminance(reflectanceLinear),
                options.dielectricF0Min, options.dielectricF0Max);
            rmaosLine[x * 4] = uchar(std::lround(roughness * 255.0f));
            rmaosLine[x * 4 + 1] = uchar(std::lround(metallic * 255.0f));
            rmaosLine[x * 4 + 2] = uchar(std::lround(bakedAo * 255.0f)); // FO76 ships AO in _l.G
            rmaosLine[x * 4 + 3] = uchar(std::lround(dielectricF0 * 255.0f));
            const uchar metallicByte = uchar(std::lround(metallic * 255.0f));
            metallicLine[x * 4] = metallicByte;
            metallicLine[x * 4 + 1] = metallicByte;
            metallicLine[x * 4 + 2] = metallicByte;
            metallicLine[x * 4 + 3] = 255;
        }
    }
    result.metalCoverage = float(double(metalTexels) / (double(width) * height));

    if ((material.emitEnabled || material.glowmap) && !sources.glow.isNull()) {
        const QImage glow = toRgba(sources.glow);
        result.emissive = QImage(glow.width(), glow.height(), QImage::Format_RGBA8888);
        const QVector3D tint = material.emittanceColor;
        for (int y = 0; y < glow.height(); ++y) {
            const uchar* glowLine = glow.constScanLine(y);
            uchar* emissiveLine = result.emissive.scanLine(y);
            for (int x = 0; x < glow.width(); ++x) {
                const float mask = glowLine[x * 4] / 255.0f;
                emissiveLine[x * 4] = uchar(std::lround(std::clamp(mask * tint.x(), 0.0f, 1.0f) * 255.0f));
                emissiveLine[x * 4 + 1] = uchar(std::lround(std::clamp(mask * tint.y(), 0.0f, 1.0f) * 255.0f));
                emissiveLine[x * 4 + 2] = uchar(std::lround(std::clamp(mask * tint.z(), 0.0f, 1.0f) * 255.0f));
                emissiveLine[x * 4 + 3] = uchar(std::lround(mask * 255.0f));
            }
        }
    }
    return result;
}

void mapParameters(const Bgsm::Material& material, Pbr::MaterialDocument* document, QStringList* unhonored)
{
    if (!document) return;
    const auto flag = [unhonored](const QString& text) { if (unhonored) unhonored->append(text); };

    // Shader family (BGSM) / effect family (BGEM).
    if (material.kind == Bgsm::Kind::Bgem) {
        document->setShaderMode(material.effectLightingEnabled
            ? Pbr::ShaderMode::SimpleLitEffect : Pbr::ShaderMode::UnlitEffect);
    } else if (material.hair) {
        document->setShaderMode(Pbr::ShaderMode::HairSurface);
        document->setSetting(QStringLiteral("Shading/Hair"), QStringLiteral("tint"),
            QColor::fromRgbF(material.hairTintColor.x(), material.hairTintColor.y(), material.hairTintColor.z()).name());
    } else if (material.skinTint || material.facegen || material.translucency || material.subsurfaceLighting) {
        document->setShaderMode(Pbr::ShaderMode::AdvancedSubsurface);
    } else {
        document->setShaderMode(Pbr::ShaderMode::StandardSurface);
    }

    // Transparency composition.
    QString composition = QStringLiteral("Opaque");
    if (material.alphaTest) composition = QStringLiteral("Alpha Test");
    else if (material.blendMode == Bgsm::AlphaBlendMode::Standard) composition = QStringLiteral("Alpha Blend");
    else if (material.blendMode == Bgsm::AlphaBlendMode::Additive) composition = QStringLiteral("Additive");
    else if (material.blendMode == Bgsm::AlphaBlendMode::Multiplicative) composition = QStringLiteral("Multiply");
    document->setSetting(QStringLiteral("Transparency/Composition"), QStringLiteral("mode"), composition);
    if (material.alphaTest)
        document->setSetting(QStringLiteral("Transparency/Composition"), QStringLiteral("threshold"), material.alphaTestRef / 255.0);
    if (!qFuzzyCompare(material.alpha, 1.0f))
        document->setSetting(QStringLiteral("Transparency/Composition"), QStringLiteral("alpha"), double(material.alpha));
    document->setSetting(QStringLiteral("Transparency/Composition"), QStringLiteral("depthWrite"), material.zBufferWrite);
    document->setSetting(QStringLiteral("Transparency/Composition"), QStringLiteral("depthTest"), material.zBufferTest);

    // Core shading flags.
    if (material.twoSided)
        document->setSetting(QStringLiteral("Shading/Core"), QStringLiteral("twoSided"), true);
    if (material.modelSpaceNormals)
        document->setSetting(QStringLiteral("Shading/Core"), QStringLiteral("normalSpace"), QStringLiteral("Model"));

    // UV transforms.
    if (!qFuzzyCompare(material.uScale, 1.0f)) document->setSetting(QStringLiteral("UV0"), QStringLiteral("scaleU"), double(material.uScale));
    if (!qFuzzyCompare(material.vScale, 1.0f)) document->setSetting(QStringLiteral("UV0"), QStringLiteral("scaleV"), double(material.vScale));
    if (!qFuzzyIsNull(material.uOffset)) document->setSetting(QStringLiteral("UV0"), QStringLiteral("offsetU"), double(material.uOffset));
    if (!qFuzzyIsNull(material.vOffset)) document->setSetting(QStringLiteral("UV0"), QStringLiteral("offsetV"), double(material.vOffset));
    if (!material.tileU || !material.tileV)
        document->setSetting(QStringLiteral("UV0"), QStringLiteral("address"),
            material.tileU || material.tileV ? QStringLiteral("Wrap / Clamp") : QStringLiteral("Clamp / Clamp"));

    // Refraction -> screen distortion.
    if (material.refraction) {
        Pbr::TextureSlotState distortion;
        distortion.enabled = true;
        distortion.values.insert(QStringLiteral("strength"), double(std::clamp(material.refractionPower, 0.0f, 1.0f)));
        distortion.values.insert(QStringLiteral("overrideStrength"), true);
        document->setTextureSlot(QStringLiteral("UV0"), QStringLiteral("primaryScreenDistortion"), distortion);
    }

    // Decals.
    if (material.decal) {
        document->setSetting(QStringLiteral("Decal"), QStringLiteral("enabled"), true);
        if (material.decalNoFade)
            document->setSetting(QStringLiteral("Decal"), QStringLiteral("distanceFade"), 0.0);
    }

    // FO76 porosity: vanilla data mapping straight onto the v5 constant.
    if (material.customPorosity) {
        Pbr::TextureSlotState rmaos = document->textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryRmaos"));
        rmaos.values.insert(QStringLiteral("overridePorosity"), true);
        rmaos.values.insert(QStringLiteral("porosity"), double(std::clamp(material.porosityValue, 0.0f, 1.0f)));
        document->setTextureSlot(QStringLiteral("UV0"), QStringLiteral("primaryRmaos"), rmaos);
    }

    // BGEM constants: diffuse-as-emission handled by the orchestrator's slot
    // wiring; the scale and tint land here.
    if (material.kind == Bgsm::Kind::Bgem) {
        document->setSetting(QStringLiteral("Shading/Unlit"), QStringLiteral("intensity"),
            double(std::clamp(material.baseColorScale, 0.0f, 1000.0f)));
    }

    // Superseded by the PBR runtime: wetness control block (v5 surface
    // states), screen-space reflection flags, per-material cubemaps.
    // Flagged for review: the palette systems and external emittance.
    if (material.grayscaleToPaletteColor || material.grayscaleToPaletteAlpha)
        flag(QStringLiteral("grayscale-to-palette is not representable; review manually"));
    if (material.externalEmittance)
        flag(QStringLiteral("external emittance is not representable; review manually"));
    if (material.kind == Bgsm::Kind::Bgsm && material.tree)
        flag(QStringLiteral("tree flag noted; consider the Vegetation family manually"));
}

} // namespace Converter
