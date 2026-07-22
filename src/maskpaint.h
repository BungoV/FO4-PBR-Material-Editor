#pragma once

#include "previewmesh.h"

#include <QImage>
#include <QVector>

// Substance-style correction-mask fills for the converter
// (docs/FO4-Conversion.md): pick a triangle on the reference mesh, expand it
// to a connected element or a submesh, and rasterize the fill into a
// 0.5-neutral mask in UV0 space. applyMetallicCorrection() consumes the
// result — below 0.5 forces dielectric, above forces metal.
namespace MaskPaint {

// Per-triangle element ids: connected components over position-welded
// vertices (meshes split vertices at UV/normal seams; welding restores the
// "separate part" the artist sees). Compute once per mesh and reuse.
QVector<int> triangleElements(const PreviewMesh& mesh);

QVector<quint32> trianglesInElement(const PreviewMesh& mesh, const QVector<int>& elements, int triangle);
QVector<quint32> trianglesInSubmesh(const PreviewMesh& mesh, int triangle);

// Rasterizes the triangles' UV0 footprints into mask.R (0..1 value written
// as 0..255). The mask starts — and stays elsewhere — at the 0.5 neutral.
void rasterizeFill(QImage& mask, const PreviewMesh& mesh, const QVector<quint32>& triangles, float value);

QImage neutralMask(int size = 1024);

} // namespace MaskPaint
