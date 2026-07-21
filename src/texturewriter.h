#pragma once

#include <QImage>
#include <QString>

// Game-ready DDS export for baked packed textures. Writes DX10-header DDS
// with a generated mip chain, compressing through the vendored rgbcx (BC1/4/5)
// and bc7enc (BC7) encoders, or storing uncompressed RGBA8.
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
    bool generateMips, QString* error = nullptr);

} // namespace TextureWriter
