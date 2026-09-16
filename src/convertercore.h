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
    // FO4 metal window over _s.R. This is a STENCIL, not a classifier, and the
    // distinction sets the numbers. As a classifier _s.R is worthless -- 53%
    // precision, measured (see docs) -- but the artist put the panel seams,
    // rivets and wear into that channel, so its SHAPE is right and what it gets
    // wrong is whole regions: the brick body, the leather grip, the wood stock.
    // Those are a couple of fills to mask out, where painting metal in from
    // nothing means rebuilding all that structure by hand.
    //
    // Because the workflow subtracts, UNDER-detecting is the fatal failure: a
    // metal object stencilled at 0% gives the user nothing to paint out.
    // Over-detecting is the cheap error -- a brick wall reading metal is one
    // fill to remove. So the window is placed low and kept narrow: low so real
    // metal survives, narrow so what it does emit is decisive rather than
    // smeared across the ambiguous band a PBR validator flags.
    //
    // Measured over the name-labelled vanilla sets, share of METAL materials
    // left with under 2% metal ("dead", no usable starting point):
    //     0.35/0.45   21.0% dead,  6.9% ambiguous   <- optimising ambiguity
    //     0.25/0.35   12.5% dead, 12.5% ambiguous      alone picks this, and
    //     0.20/0.30    7.8% dead, 14.2% ambiguous   <- it is the wrong target
    //     0.15/0.25    5.5% dead, 15.7% ambiguous
    // 0.35/0.45 killed MetalVerTrimFuturistic01 and SerratedBlade outright.
    //
    // The user drags these per material against the histogram on the Levels
    // node; setting metalSpecStart to 1 converts fully dielectric.
    float metalSpecStart = 0.20f, metalSpecFull = 0.30f;
    // Gloss/smoothness -> roughness response curve (1 = linear invert).
    float glossCurve = 1.0f;
    // Dielectric F0 window (the physical dielectric range). Since PBRM v6 the
    // window is expressed as a specular WEIGHT against a material IOR whose
    // F0 is dielectricF0Max: FO76 weight = clamp(luma / max, min / max, 1),
    // so weight * F0(ior) reproduces the v5 clamp [min, max] exactly.
    float dielectricF0Min = 0.02f, dielectricF0Max = 0.08f;
    // _s.R is a Blinn-Phong specular intensity with no physical anchor, so the
    // F0 mapping is anchored on the corpus instead: the measured median
    // (54/255 over 6155 materials) maps to the physical dielectric default
    // 0.04, and the window above clamps the tails.
    float specMedianReference = 0.212f;
    // Spec percentile normalization. Default OFF since 2026-07-26: it existed
    // to stop blown-out spec maps converting to mirror chrome, and chrome now
    // requires a cubemap declaration instead of a bright texel. What is left is
    // a per-material stretch of a continuous physical quantity, which is the
    // thing the roughness rule in docs/FO4-Conversion.md warns against.
    bool normalizeSpec = false;
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

// Does this cubemap path name an explicit metal? Not a gate on the conversion
// -- nothing in a .bgsm is reliable enough for that -- but the strongest hint
// the format carries (55-100% metal by asset naming across 522 vanilla
// materials), so it orders the review queue: materials naming a metal cubemap
// are the ones worth masking first. Matches chrome/brushed/bronze/copper/gold/
// silver/steel cube names; deliberately NOT the general-purpose sky cubes,
// which carry the volume but are used on brick. See docs/FO4-Conversion.md.
bool cubemapDeclaresMetal(const QString& envmapTexture);

// FO4 metallic stencil from _s.R. One signal, no second opinion: the diffuse
// darkness that makes FO76's gate reliable runs backwards in FO4 (measured
// ratio 0.84, agreeing in 26% of materials against FO76's 92.5%), so including
// it removed metal from exactly the texels that carry it. The result is a
// starting point for masking, not an answer -- see Options::metalSpecStart.
float metallicConfidenceFo4(float specStrength, const Options& options);

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

// Whether a channel carries no variation worth reading. The FO4 gloss fallback
// turns on this: _s.G is the glossiness where it varies, and the .bgsm
// smoothness scalar is the glossiness where it does not. Measured over 6155
// vanilla materials -- the scalar is non-default in 80.9% of flat-G maps and
// only 20.8% of textured ones, so it is a fallback, not a multiplier.
bool channelIsFlat(const QImage& image, int channel, int tolerance = 8);

// ---- Image-level conversion ----

struct Fo4Sources {
    QImage diffuse;        // sRGB, alpha = opacity
    QImage specular;       // linear, R = spec strength, G = gloss
    QImage glow;           // grayscale mask (color lives in the material)
    // No env mask here on purpose. BGSM v2 has no env-mask texture slot at all
    // -- the field the orchestrator was reading is BGEM's -- so it was always
    // an empty image standing in for a signal Fallout 4 never stored.
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

// IOR whose normal-incidence Fresnel is f0: ior = (1 + sqrt f0) / (1 - sqrt f0).
float specularIorForF0(float f0);
// The FO4 anchor: F0(ior) = 0.04 / specMedianReference, so the corpus median
// _s.R texel keeps landing on the dielectric default while the weight is the
// raw Blinn-Phong intensity.
float fo4SpecularIor(const Options& options);
// The FO76 anchor: F0(ior) = dielectricF0Max (see Options).
float fo76SpecularIor(const Options& options);

struct ConvertedImages {
    QImage baseColor;      // sRGB + opacity alpha
    QImage rmaos;          // linear: R rough, G metal, B AO (1.0 until derivations), A specular weight (v6)
    QImage emissive;       // sRGB tinted emission + A mask; null when the material does not emit
    QImage subsurface;     // FO76 _l.B scatter mask; null unless the material scatters
    // Specular TINT (sRGB RGB; A unused, the converter writes no IOR map):
    // multiplies the reflectance the material IOR and the weight set. FO76 fills it with the HUE of _r, which
    // the F0 luminance throws away; FO4 fills it from the material's
    // cSpecularColor constant, because an FO4 _s map is BC5 (R spec strength,
    // G gloss) and carries no color at all. Null wherever the tint is white,
    // which is most materials — a white tint is the identity and would only
    // cost a texture.
    QImage specularColor;
    QImage metallic;       // grayscale metallic confidence (diagnostics / masking UI)
    float metalCoverage = 0; // fraction of texels with metallic > 0.5 (report)
    // v6: the specular IOR scalar (primarySpecularColor.ior). The converter
    // anchors the reflectance LEVEL here and writes only the weight per texel.
    float specularIor = 1.5f;
};

// The specular tint an FO4/FO76 material's own constant asks for, as a small
// flat sRGB image with weight 1 — or a null image when the constant is white
// (the identity). Vanilla FO4's deferred path drops cSpecularColor entirely,
// so this is authored intent being recovered, not vanilla being reproduced.
QImage constantSpecularTint(const QVector3D& specularColor);

// The hue of an FO76 reflectance map, normalized so its luminance stays with
// RMAOS.A: tint = saturate(reflectanceLinear / luminance). Null when the map
// is achromatic, in which case the F0 scalar already carries everything it had.
QImage reflectanceSpecularTint(const QImage& reflectance);

ConvertedImages convertFo4(const Fo4Sources& sources, const Bgsm::Material& material, const Options& options);
ConvertedImages convertFo76(const Fo76Sources& sources, const Bgsm::Material& material, const Options& options);

// ---- Parameter mapping (the four-fate audit) ----

// Fills a fresh document's shader family, transparency, depth, UV transform,
// emissive constants, refraction -> screen distortion, model-space normals,
// FO76 porosity constant, and family-specific settings. Texture paths are the
// orchestrator's job. Unhonored fields are reported, never silently dropped.
void mapParameters(const Bgsm::Material& material, Pbr::MaterialDocument* document, QStringList* unhonored);

} // namespace Converter
