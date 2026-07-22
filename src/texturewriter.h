#pragma once

#include <QImage>
#include <QString>

// Game-ready DDS export for baked packed textures. Writes DX10-header DDS
// with a generated mip chain, compressing through the vendored rgbcx (BC1/4/5)
// and bc7enc (BC7) encoders, or storing uncompressed RGBA8.
//
// Mip discipline: every level is a plain per-channel box average of the level
// above — mean-preserving, with no premultiplied-alpha weighting — so packed
// data channels (signed curvature in Normal.A, porosity in RMAOS.A, masks)
// converge to their correct area averages at distance instead of being
// contrast-boosted or bled into by RGB.
//
// Set `normalMapMips` for normal-map exports so each mip averages decoded
// tangent-space vectors and renormalizes the result instead of averaging the
// encoded RG bytes independently. This preserves the correct mean direction.
//
// When `toksvigNormal` is supplied (an RG tangent normal map paired with a
// roughness-in-R export), the normal's per-mip variance is folded into the
// roughness chain (Toksvig): distant minified speculars stop aliasing because
// the roughness mips carry the normal detail the mip chain averaged away.
namespace TextureWriter {

enum class DdsFormat {
    Bc1,        // RGB masks / low-cost color, linear
    Bc4,        // single channel, linear
    Bc5,        // two channel (normals), linear
    Bc7,        // packed data, linear
    Bc7Srgb,    // color, sRGB
    Rgba8,      // uncompressed, linear
    Rgba8Srgb   // uncompressed, sRGB
};

bool saveDds(const QImage& image, const QString& filePath, DdsFormat format,
    bool generateMips, QString* error = nullptr,
    const QImage& toksvigNormal = QImage(), bool normalMapMips = false);

} // namespace TextureWriter
