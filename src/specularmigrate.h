#pragma once

// PBRM v5 -> v6 texture repacks (docs/PBRM-v6-Specular.md section 4).
//
// v5 stored dielectric F0 in RMAOS.A and a tint weight in the specular colour
// map's alpha. v6 reads RMAOS.A as OpenPBR specular_weight and the specular
// colour alpha as the specular IOR map. Constants migrate exactly inside
// MaterialDocument; texels cannot, so the loader marks such slots
// (legacyF0Alpha / legacyTintAlpha) and these routines rewrite the textures.
// The pixel routines are hermetic (QImage in, QImage out) so the tests pin
// them; the slot routines resolve paths, keep a .v5.bak beside every file
// they rewrite, and clear the markers on the document.

#include <QImage>
#include <QString>
#include <QStringList>

namespace Pbr {
class MaterialDocument;
}

namespace SpecularMigrate {

struct F0Repack {
    QImage rmaos;               // RGBA8888: RGB untouched, A = specular weight
    QImage iorAlpha;            // Grayscale8 IOR map (texel = ior / iorMax * 255); null when no texel exceeded F0 0.04
    int texelsAboveDefault = 0; // texels whose F0 exceeded 0.04 and moved into the IOR map
    int texelsAboveCeiling = 0; // of those, texels whose IOR exceeded iorMax (clamped)
};

// ior = (1 + sqrt f0) / (1 - sqrt f0); f0 clamped below 1.
double specularIorForF0(double f0);

// RMAOS alpha authored as dielectric F0 -> alpha as weight. f0 <= 0.04 becomes
// weight f0 / 0.04 at the material IOR; f0 > 0.04 becomes weight 1 with the
// IOR that has that F0 written into the IOR map, so weight * F0(ior) is the
// authored F0 either way (up to texel quantisation).
F0Repack repackF0Alpha(const QImage& rmaos, double materialIor, double iorMax);

// Specular colour map with a v5 tint-weight alpha -> the weight baked into
// RGB (lerp(1, rgb, a) in linear, re-encoded sRGB) and alpha = ior / iorMax.
QImage bakeTintAlpha(const QImage& specular, double ior, double iorMax);

// rgb with its alpha replaced by alpha8 (resampled to rgb's size if needed).
QImage withAlpha(const QImage& rgb, const QImage& alpha8);

// "path/foo_rmaos.dds" -> "path/foo_spec.dds"; any other stem gets "_spec".
QString specularPathFor(const QString& rmaosPath);

// Rewrites the slot's RMAOS texture (and writes or extends the matching
// specular colour map when an IOR map is needed), then clears the marker,
// declares the alpha (overrideSpecularWeight = false, alphaCarries = Specular
// Weight) and, if an IOR map was written, clears overrideIor on the colour slot.
bool repackRmaosSlot(Pbr::MaterialDocument* document, const QString& setName, const QString& slotKey,
    const QString& textureRoot, QString* error);

// Rewrites the slot's specular colour map with bakeTintAlpha, clears the
// marker and declares the alpha as the IOR map.
bool bakeTintSlot(Pbr::MaterialDocument* document, const QString& setName, const QString& slotKey,
    const QString& textureRoot, QString* error);

// Every marked slot in every texture set. Returns the number of slots
// rewritten, or -1 with *error set. Report lines name what was written.
int repackDocument(Pbr::MaterialDocument* document, const QString& textureRoot, QString* error,
    QStringList* report = nullptr);

} // namespace SpecularMigrate
