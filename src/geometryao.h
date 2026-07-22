#pragma once

#include "previewmesh.h"

#include <QImage>
#include <QStringList>

// Geometry ambient occlusion baked from a reference mesh
// (docs/FO4-Conversion.md): rasterize a UV-space G-buffer (world position +
// normal), then shadow-test every texel against whole-mesh ortho depth maps
// from a sphere of directions — the depth-render approach from the spec, run
// on the CPU so the bake also works headless (`pbrm bake-ao`).
//
// Duplicate UV islands (vanilla stacks copies in 0-1 for reuse): islands
// whose footprints overlap form a group; every instance occludes, but the
// group's texels come from its brightest instance — a canonical bake instead
// of last-rasterized-wins garbage.
namespace GeometryAO {

struct Options {
    int mapSize = 512;        // baked AO resolution
    int depthSize = 512;      // per-direction depth map resolution
    int directions = 64;      // sphere directions (cosine-weighted by the surface normal)
    float bias = 0.005f;      // depth acceptance bias, in mesh-radius units
    float strength = 1.0f;    // final AO = ao^strength
    bool canonicalIslands = true;
    int dilate = 4;           // edge padding passes so bilinear sampling stays clean
};

QImage bake(const PreviewMesh& mesh, const Options& options, QStringList* notes = nullptr);

} // namespace GeometryAO
