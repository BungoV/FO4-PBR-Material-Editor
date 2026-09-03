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

bool cubemapDeclaresMetal(const QString& envmapTexture)
{
    QString name = envmapTexture.trimmed().toLower();
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    name = name.section(QLatin1Char('/'), -1);
    if (name.isEmpty()) return false;
    // Explicitly metal-named cubes only. mipblur_defaultoutside1 carries by far
    // the most volume (1309 materials) and reads 68% metal by asset naming, but
    // it is the generic sky cube: the other 32% are brick walls and hair. The
    // names below cover 522 vanilla materials scoring 55-100% metal.
    static const char* const declarations[] = {
        "metalchrome", "metalbrushed", "metalshine", "chrome_e", "bronze_e",
        "copper_e", "gold_e", "silver_e", "steel_e",
    };
    for (const char* declaration : declarations)
        if (name.contains(QLatin1String(declaration))) return true;
    return false;
}

float metallicConfidenceFo4(float specStrength, const Options& options)
{
    // Closing the window converts fully dielectric, and it has to hold for a
    // saturated spec map too: specStrength is clamped to [0,1], so a start at
    // or above 1 can admit nothing. Without this the degenerate smoothstep
    // (edge0 == edge1) returns 1 for a texel sitting exactly on the edge, and
    // vanilla ships plenty of _s.R at 255 against a specularMult above 1.
    if (options.metalSpecStart >= 1.0f) return 0.0f;
    // One signal, not a joint one. The second opinion FO76 gets from diffuse
    // darkness does not exist here: measured over 5350 vanilla materials the
    // diffuse under high spec is BRIGHTER (median 95 against 76), so the FO76
    // gate transplanted into FO4 removed metal from the texels carrying it.
    // Metal that should not be here is subtracted by the painted mask, which is
    // the cheap direction -- see applyMetallicCorrection.
    return std::clamp(smoothstepf(options.metalSpecStart, options.metalSpecFull, specStrength),
        0.0f, 1.0f);
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

bool channelIsFlat(const QImage& image, int channel, int tolerance)
{
    const QImage source = toRgba(image);
    if (source.isNull() || channel < 0 || channel > 3) return true;
    int lowest = 255, highest = 0;
    for (int y = 0; y < source.height(); ++y) {
        const uchar* line = source.constScanLine(y);
        for (int x = 0; x < source.width(); ++x) {
            const int value = line[x * 4 + channel];
            lowest = std::min(lowest, value);
            highest = std::max(highest, value);
        }
    }
    return highest - lowest < tolerance;
}

QImage constantSpecularTint(const QVector3D& specularColor)
{
    const auto byte = [](float value) {
        return uchar(std::clamp<long>(std::lround(value * 255.0f), 0L, 255L));
    };
    const uchar r = byte(specularColor.x()), g = byte(specularColor.y()), b = byte(specularColor.z());
    // White is the identity. Emitting it would cost a texture to multiply by 1.
    if (r == 255 && g == 255 && b == 255) return {};
    // Four texels rather than one: the value is constant, the size is only
    // about surviving a block-compressed writer and its mip chain.
    QImage tint(4, 4, QImage::Format_RGBA8888);
    tint.fill(QColor(r, g, b, 255));
    return tint;
}

QImage reflectanceSpecularTint(const QImage& reflectance)
{
    const QImage source = toRgba(reflectance);
    if (source.isNull()) return {};
    QImage tint(source.width(), source.height(), QImage::Format_RGBA8888);
    qint64 chromaSum = 0, stronglyColored = 0;
    for (int y = 0; y < source.height(); ++y) {
        const uchar* line = source.constScanLine(y);
        uchar* tintLine = tint.scanLine(y);
        for (int x = 0; x < source.width(); ++x) {
            // FO76 authors _r as BC1_UNORM_SRGB and the load path does not act
            // on the declared color space, so decode it here exactly as the F0
            // luminance path a few lines below does.
            const QVector3D linear = srgbToLinear(QVector3D(line[x * 4] / 255.0f,
                line[x * 4 + 1] / 255.0f, line[x * 4 + 2] / 255.0f));
            const float luma = luminance(linear);
            // The LEVEL belongs to RMAOS.A. Dividing it out leaves the hue, and
            // a texel with no light in it has no hue worth keeping.
            const QVector3D hue = luma > 1e-4f
                ? QVector3D(std::clamp(linear.x() / luma, 0.0f, 1.0f),
                      std::clamp(linear.y() / luma, 0.0f, 1.0f),
                      std::clamp(linear.z() / luma, 0.0f, 1.0f))
                : QVector3D(1.0f, 1.0f, 1.0f);
            const QVector3D encoded = linearToSrgb(hue);
            const uchar r = uchar(std::lround(std::clamp(encoded.x(), 0.0f, 1.0f) * 255.0f));
            const uchar g = uchar(std::lround(std::clamp(encoded.y(), 0.0f, 1.0f) * 255.0f));
            const uchar b = uchar(std::lround(std::clamp(encoded.z(), 0.0f, 1.0f) * 255.0f));
            const int chroma = int(std::max({ r, g, b })) - int(std::min({ r, g, b }));
            chromaSum += chroma;
            if (chroma > 24) ++stronglyColored;
            tintLine[x * 4] = r;
            tintLine[x * 4 + 1] = g;
            tintLine[x * 4 + 2] = b;
            tintLine[x * 4 + 3] = 255;
        }
    }
    // An achromatic reflectance map holds nothing the F0 scalar did not already
    // carry, so it earns no texture. Same discipline as the _l alpha check:
    // measure the data before consuming it — and the threshold is measured too.
    // Vanilla FO76 ships _r as BC1, whose block ends leave a few stray
    // chromatic texels on a map that is grey by intent: ProtectronArmor_r
    // (1024², channel means 0.265/0.265/0.268) has some, and letting one texel
    // decide shipped a megabyte of white. Gold on the same model
    // (ProtectronArmorGold_r, 0.725/0.655/0.483) is what a real tint looks
    // like. So: the tint ships when the surface is tinted ON AVERAGE, or when a
    // real share of it is strongly colored (an emblem on a grey body).
    const double texels = std::max<double>(1.0, double(source.width()) * source.height());
    const bool tinted = double(chromaSum) / texels > 4.0
        || double(stronglyColored) / texels > 0.02;
    return tinted ? tint : QImage();
}

ConvertedImages convertFo4(const Fo4Sources& sources, const Bgsm::Material& material, const Options& options)
{
    ConvertedImages result;
    const QImage diffuse = toRgba(sources.diffuse);
    if (diffuse.isNull()) return result;
    // FO4 has no specular COLOR map to take: _s is BC5, R specular strength and
    // G gloss. The color it does author is the material's own cSpecularColor,
    // which vanilla's deferred path drops entirely.
    result.specularColor = constantSpecularTint(material.specularColor);
    const int width = diffuse.width(), height = diffuse.height();
    const ScaledReader specular(sources.specular, width, height);
    const ScaledReader correction(sources.correctionMask, width, height);

    // Off by default now; see Options::normalizeSpec for why the chrome it was
    // added to prevent no longer comes from here.
    float specScale = 1.0f;
    if (options.normalizeSpec && specular.valid()) {
        const float percentile = channelPercentile(sources.specular, 0, options.specNormalizePercentile);
        specScale = 1.0f / std::max(percentile, options.specNormalizeFloor);
    }
    const float specMult = std::clamp(material.specularMult, 0.0f, 4.0f);

    // The .bgsm smoothness scalar is a FALLBACK for a gloss channel that says
    // nothing, not a multiplier on one that does -- measured non-default in
    // 80.9% of flat-G maps against 20.8% of textured ones. Multiplying instead
    // would have made 1055 vanilla materials with real gloss maps uniformly
    // rougher for no reason.
    const bool glossFromScalar = !specular.valid() || channelIsFlat(sources.specular, 1);
    const float scalarGloss = std::clamp(material.smoothness, 0.0f, 1.0f);

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
            float storedSpec = 0.0f, gloss = scalarGloss;
            if (specular.valid()) {
                const uchar* specTexel = specular.texel(x, y);
                storedSpec = std::clamp(specTexel[0] / 255.0f * specScale, 0.0f, 1.0f);
                if (!glossFromScalar) gloss = specTexel[1] / 255.0f;
            }
            // specularMult scales the SPECULAR RESPONSE, not the material class.
            // It belongs on F0, where it genuinely dims a reflection, and NOT on
            // the metal stencil: MetalVerTrimFuturistic01 is metal trim whose
            // spec peaks at 84/255 against a mult of 0.400, and folding the
            // mult in dropped it to 0.13 and stencilled the material at 0%.
            const float specStrength = std::clamp(storedSpec * specMult, 0.0f, 1.0f);
            float metallic = metallicConfidenceFo4(storedSpec, options);
            if (correction.valid())
                metallic = applyMetallicCorrection(metallic, correction.texel(x, y)[0] / 255.0f);
            if (metallic > 0.5f) ++metalTexels;

            // Raw spec, not specStrength: recovery produces an ALBEDO, which is
            // a material property, and specularMult is a highlight dial. Same
            // argument as the stencil above, and the same failure if ignored —
            // a metal trim with mult 0.400 recovered into the bottom of the F0
            // window and came out near-black. It also keeps this in step with
            // the graph's Metal Albedo node, which reads the same raw channel.
            const QVector3D metalAlbedo = recoverMetalAlbedoFo4(diffuseLinear, storedSpec,
                sources.cubemapTint, sources.hasCubemapTint, options);
            const float blend = metallic * std::clamp(options.recoveryStrength, 0.0f, 1.0f);
            const QVector3D baseLinear = diffuseLinear * (1.0f - blend) + metalAlbedo * blend;
            const QVector3D baseSrgb = linearToSrgb(baseLinear);
            baseLine[x * 4] = uchar(std::lround(baseSrgb.x() * 255.0f));
            baseLine[x * 4 + 1] = uchar(std::lround(baseSrgb.y() * 255.0f));
            baseLine[x * 4 + 2] = uchar(std::lround(baseSrgb.z() * 255.0f));
            baseLine[x * 4 + 3] = diffuseLine[x * 4 + 3];

            const float roughness = glossToRoughness(gloss, options);
            // _s.R is a Blinn-Phong intensity, not an F0, so it is anchored on
            // the corpus rather than on a physical table: the measured median
            // maps to the dielectric default 0.04 and the window clamps the
            // tails. The old specStrength * 0.08 put the median texel of the
            // median material at 0.017, under the floor -- so the whole F0 axis
            // pinned flat at 0.02 and carried no information at all.
            const float dielectricF0 = std::clamp(
                specStrength * (0.04f / std::max(options.specMedianReference, 1e-4f)),
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
                // FO76 authors _r as BC1_UNORM_SRGB and nothing in the load path
                // acts on the declared colour space, so these texels arrive
                // encoded. Every consumer below is linear maths -- the metal
                // smoothstep edges bracket the linear dielectric/metal gap, the
                // mix runs against diffuseLinear, and the F0 clamp is the linear
                // 0.02-0.08 band -- and the 0.04 fallback above is itself linear.
                // Read as-is, a 0.04 dielectric measured ~0.22: inside the metal
                // ramp, and over the F0 ceiling.
                reflectanceLinear = srgbToLinear(QVector3D(reflTexel[0] / 255.0f,
                    reflTexel[1] / 255.0f, reflTexel[2] / 255.0f));
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

    // FO76 packs two more masks into _l that FO4 has no equivalent for: B is
    // the subsurface mask and A is the glow mask. A dedicated _g map still
    // wins where one is authored, so this only fills in what was being lost.
    const QImage lightingRgba = toRgba(sources.lighting);

    // _l.A only carries a glow mask when the file actually has an alpha
    // channel. Plenty of vanilla _l maps are BC1, which decodes to a constant
    // opaque alpha — reading that as a mask would make the whole surface emit.
    const auto alphaCarriesData = [](const QImage& image) {
        if (image.isNull()) return false;
        uchar low = 255, high = 0;
        for (int y = 0; y < image.height(); ++y) {
            const uchar* line = image.constScanLine(y);
            for (int x = 0; x < image.width(); ++x) {
                const uchar value = line[x * 4 + 3];
                low = std::min(low, value);
                high = std::max(high, value);
                if (high - low > 2) return true;
            }
        }
        return false;
    };

    QImage glowMask = toRgba(sources.glow);
    if (glowMask.isNull() && !lightingRgba.isNull() && (material.emitEnabled || material.glowmap)
        && alphaCarriesData(lightingRgba)) {
        glowMask = QImage(lightingRgba.width(), lightingRgba.height(), QImage::Format_RGBA8888);
        for (int y = 0; y < lightingRgba.height(); ++y) {
            const uchar* lightLine = lightingRgba.constScanLine(y);
            uchar* maskLine = glowMask.scanLine(y);
            for (int x = 0; x < lightingRgba.width(); ++x) {
                const uchar value = lightLine[x * 4 + 3]; // _l.A
                maskLine[x * 4] = maskLine[x * 4 + 1] = maskLine[x * 4 + 2] = value;
                maskLine[x * 4 + 3] = 255;
            }
        }
    }

    if ((material.emitEnabled || material.glowmap) && !glowMask.isNull()) {
        const QImage& glow = glowMask;
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
    // The F0 above is luminance(_r), which discards the hue of a map that
    // genuinely has one — FO76 authors true RGB reflectance. The level stays
    // where it was; the hue becomes the specular tint.
    result.specularColor = reflectanceSpecularTint(sources.reflectance);

    // Only materials that actually scatter get a subsurface map; every other
    // FO76 material would otherwise gain a meaningless extra texture.
    if ((material.translucency || material.subsurfaceLighting) && !lightingRgba.isNull()) {
        result.subsurface = QImage(lightingRgba.width(), lightingRgba.height(), QImage::Format_RGBA8888);
        for (int y = 0; y < lightingRgba.height(); ++y) {
            const uchar* lightLine = lightingRgba.constScanLine(y);
            uchar* sssLine = result.subsurface.scanLine(y);
            for (int x = 0; x < lightingRgba.width(); ++x) {
                const uchar value = lightLine[x * 4 + 2]; // _l.B
                sssLine[x * 4] = sssLine[x * 4 + 1] = sssLine[x * 4 + 2] = value;
                sssLine[x * 4 + 3] = 255;
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

    // Subsurface. FO76 authors a translucency block (v>=8), FO4 the simpler
    // subsurface-lighting pair. Both used to only pick the shader family above,
    // which selected a subsurface material and then shipped it with default
    // scattering — every authored value was dropped.
    if (material.translucency || material.subsurfaceLighting) {
        const QString subsurface = QStringLiteral("Shading/Subsurface");
        if (material.translucency) {
            // Thick objects scatter through a volume, thin ones transmit
            // through a sheet; the transmissive scale is authored either way.
            document->setSetting(subsurface, QStringLiteral("mode"),
                material.translucencyThickObject ? QStringLiteral("Combined")
                                                 : QStringLiteral("Thin Transmission"));
            document->setSetting(subsurface, QStringLiteral("transmission"),
                double(std::clamp(material.translucencyTransmissiveScale, 0.0f, 4.0f)));
            // The authored subsurface colour is a wavelength tint, which is
            // exactly what a Custom diffusion profile expresses. A default
            // white one carries no information, so the stock profile stays.
            const QVector3D scatter = material.translucencySubsurfaceColor;
            if (!qFuzzyCompare(scatter, QVector3D(1, 1, 1))) {
                document->setSetting(subsurface, QStringLiteral("profile"), QStringLiteral("Custom"));
                document->setSetting(subsurface, QStringLiteral("customScatterColor"),
                    QColor::fromRgbF(std::clamp(scatter.x(), 0.0f, 1.0f),
                        std::clamp(scatter.y(), 0.0f, 1.0f),
                        std::clamp(scatter.z(), 0.0f, 1.0f)).name());
            }
            if (material.translucencyMixAlbedo)
                flag(QStringLiteral("bTranslucencyMixAlbedo: runtime blends the scatter colour with albedo; no editor equivalent"));
            if (material.translucencyTurbulence > 0.0f)
                flag(QStringLiteral("fTranslucencyTurbulence %1: no editor equivalent")
                        .arg(material.translucencyTurbulence, 0, 'f', 3));
        } else {
            // FO4's rolloff is normalised against its own default, so a
            // material that left it alone lands on the editor's neutral weight
            // and one that raised it scatters further.
            document->setSetting(subsurface, QStringLiteral("mode"), QStringLiteral("Diffusion Profile"));
            document->setSetting(subsurface, QStringLiteral("weight"),
                double(std::clamp(material.subsurfaceLightingRolloff / 0.3f, 0.0f, 4.0f)));
        }
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
