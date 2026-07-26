#pragma once

#include "bgsmreader.h"
#include "materialdocument.h"

#include <QImage>
#include <QStringList>
#include <QVector3D>

// Pure conversion math for the FO4/FO76 -> PBR converter
// (docs/FO4-Conversion.md). Everything here is host-testable: images in,
// images out, no file I/O. The orchestrator resolves textures, calls these,
// and writes DDS + sibling .pbrm files.
namespace Converter {

struct Options {
    // FO4 metal albedo recovery: vanilla metals are authored dark because the
    // engine composites the cubemap on top. Recovered texels take hue from
    // the diffuse (or the cubemap mean when the diffuse is too dark to trust)
    // and brightness from spec strength.
    float recoveryStrength = 1.0f;
    float metalF0Min = 0.35f;             // linear brightness window for recovered metal albedo
    float metalF0Max = 0.85f;
    float diffuseReliabilityThreshold = 0.09f; // linear luma below which the cubemap tint owns the hue
    float cubemapTintSaturationClamp = 0.35f;  // scene-neutral cubes must not tint metals
    // FO4 metallic inference thresholds (joint signal, smoothsteps).
    float metalSpecStart = 0.25f, metalSpecFull = 0.65f;
    float metalDarkStart = 0.35f, metalDarkFull = 0.12f;  // linear diffuse luma, darker = more metal
    // Gloss/smoothness -> roughness response curve (1 = linear invert).
    float glossCurve = 1.0f;
    // Dielectric F0 window (vanilla spec maps are blown out; clamp into the
    // physical dielectric range).
    float dielectricF0Min = 0.02f, dielectricF0Max = 0.08f;
    // Spec percentile normalization: batch-mode taming of blown-out spec maps.
    bool normalizeSpec = true;
    float specNormalizePercentile = 0.98f;
    float specNormalizeFloor = 0.5f;      // never divide by less than this
    // FO76 reconstruction thresholds: reflectance intensity joined with
    // diffuse near-blackness.
    float fo76ReflectanceStart = 0.12f, fo76ReflectanceFull = 0.4f;
    float fo76DarkStart = 0.3f, fo76DarkFull = 0.08f;
};

// ---- Pure per-texel math (test-pinned) ----

float srgbToLinear(float value);
float linearToSrgb(float value);
QVector3D srgbToLinear(const QVector3D& color);
QVector3D linearToSrgb(const QVector3D& color);
float luminance(const QVector3D& linearColor);

// Roughness from FO4 glossiness / FO76 smoothness.
float glossToRoughness(float gloss, const Options& options);

// FO4 metallic confidence from the joint signal: env mapping enabled x env
// mask scale x spec strength x diffuse darkness.
float metallicConfidenceFo4(float specStrength, float diffuseLinearLuma,
    bool environmentMapping, float environmentMaskScale, float environmentMask,
    const Options& options);

// FO76 metallic confidence: reflectance intensity joined with diffuse
// near-blackness (true specular workflow - metals have black diffuse).
float metallicConfidenceFo76(const QVector3D& reflectanceLinear,
    float diffuseLinearLuma, const Options& options);

// Applies a painted 0.5-neutral correction: below forces dielectric, above
// forces metal, strength = distance from neutral.
float applyMetallicCorrection(float inferred, float maskValue);

// FO4 recovered metal albedo (linear): hue from diffuse or cubemap tint by
// reliability, brightness from spec strength.
QVector3D recoverMetalAlbedoFo4(const QVector3D& diffuseLinear, float specStrength,
    const QVector3D& cubemapTintLinear, bool hasCubemapTint, const Options& options);

// Clamps a cubemap mean color's chroma so scene-neutral cubes cannot tint.
QVector3D clampTintSaturation(const QVector3D& linearColor, float maxSaturation);

// Percentile of a channel across an image (0-1 domain), for spec normalization.
float channelPercentile(const QImage& image, int channel, float percentile);

// ---- Image-level conversion ----

struct Fo4Sources {
    QImage diffuse;        // sRGB, alpha = opacity
    QImage specular;       // linear, R = spec strength, G = gloss
    QImage glow;           // grayscale mask (color lives in the material)
    QImage envMask;        // optional _m; white when absent
    QVector3D cubemapTint{0.5f, 0.5f, 0.5f}; // linear mean of the referenced _e
    bool hasCubemapTint = false;
    QImage correctionMask; // optional painted metallic correction (R, 0.5 neutral)
};

// FO76 channel semantics — verified against real extracted assets
// (2026-07-23, alien/PipBoy-chrome/Protectron-gold sample set):
//   _r (specularTexture slot)  = reflectance color / F0. RGB on metals (gold
//       reads warm 0.73/0.66/0.48 over a near-black diffuse), BC4 grayscale
//       ~0.04 on dielectrics.
//   _l (lightingTexture slot)  = R smoothness (chrome 0.61 > skin 0.44),
//       G baked AO (mean ~0.9 with cavity dips), B subsurface mask,
//       A glow mask. B and A were read as unused until 2026-07-24; FO76
//       materials that scatter or emit without a dedicated _g map carry
//       those masks here and nowhere else.
struct Fo76Sources {
    QImage diffuse;        // sRGB albedo (metals near-black), alpha = opacity
    QImage reflectance;    // _r: F0 color (treated linear; see options)
    QImage lighting;       // _l: R smoothness, G baked AO, B subsurface mask, A glow mask
    QImage glow;
    QImage correctionMask;
};

struct ConvertedImages {
    QImage baseColor;      // sRGB + opacity alpha
    QImage rmaos;          // linear: R rough, G metal, B AO (1.0 until derivations), A dielectric F0
    QImage emissive;       // sRGB tinted emission + A mask; null when the material does not emit
    QImage subsurface;     // FO76 _l.B scatter mask; null unless the material scatters
    QImage metallic;       // grayscale metallic confidence (diagnostics / masking UI)
    float metalCoverage = 0; // fraction of texels with metallic > 0.5 (report)
};

ConvertedImages convertFo4(const Fo4Sources& sources, const Bgsm::Material& material, const Options& options);
ConvertedImages convertFo76(const Fo76Sources& sources, const Bgsm::Material& material, const Options& options);

// ---- Parameter mapping (the four-fate audit) ----

// Fills a fresh document's shader family, transparency, depth, UV transform,
// emissive constants, refraction -> screen distortion, model-space normals,
// FO76 porosity constant, and family-specific settings. Texture paths are the
// orchestrator's job. Unhonored fields are reported, never silently dropped.
void mapParameters(const Bgsm::Material& material, Pbr::MaterialDocument* document, QStringList* unhonored);

} // namespace Converter
