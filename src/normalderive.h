#pragma once

#include <QImage>

// Normal-map derivations for the FO4/FO76 converter (docs/FO4-Conversion.md):
// one reconstruction feeds three physical channels. The shipped normal is
// always the caller's original — the (optionally denoised) field here is a
// derivation-side input only.
//
//   height    <- Poisson-integrated from the normal's gradient field (FFT,
//                mirrored boundary, solved at a capped derivation size)
//   detail AO <- horizon-scan over the reconstructed height field
//   curvature <- divergence of the normal XY field (full resolution),
//                encoded 0.5-neutral: below = convex edge, above = concave
namespace NormalDerive {

struct Options {
    // Height/AO solve resolution cap (power-of-two friendly; curvature always
    // runs at full resolution).
    int derivationSize = 512;
    // Signed-curvature encoding gain around the 0.5 neutral.
    float curvatureScale = 2.0f;
    // Height amplitude in derivation texels for the horizon scan.
    float heightScale = 12.0f;
    int aoDirections = 8;
    int aoRadius = 24;
    float aoStrength = 1.0f;
    // Derivation-side denoising (the Laplacian amplifies exactly the
    // frequencies compression noise lives in). Auto-gated by the metric.
    bool autoDenoise = true;
    float denoiseThreshold = 0.035f;
    float bilateralRangeSigma = 0.10f;
    bool forceMedian = false;
};

struct Results {
    QImage curvature;  // full resolution, grayscale, 0.5 neutral
    QImage height;     // full resolution (upsampled), grayscale 0-1
    QImage ao;         // full resolution (upsampled), grayscale
    float noiseMetric = 0.0f;
    bool denoised = false;
};

// The 1-2 px high-frequency energy of the XY field versus its structural
// scale; compression garbage and dither score high.
float noiseMetric(const QImage& normalMap);

// Median + bilateral cleanup of the XY vector field (edge-preserving).
QImage denoiseNormal(const QImage& normalMap, const Options& options);

Results derive(const QImage& normalMap, const Options& options);

} // namespace NormalDerive
