#include "maskpaint.h"

#include <QHash>

#include <algorithm>
#include <cmath>

namespace MaskPaint {
namespace {

struct UnionFind {
    QVector<int> parent;
    explicit UnionFind(int size) : parent(size) { for (int i = 0; i < size; ++i) parent[i] = i; }
    int find(int x) { while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; } return x; }
    void unite(int a, int b) { a = find(a); b = find(b); if (a != b) parent[b] = a; }
};

quint64 quantizedPosition(const QVector3D& position)
{
    // 1/1024-unit weld tolerance: generous enough to rejoin seam-split
    // vertices, far below any real geometric feature.
    const auto quantize = [](float value) {
        return quint64(qint32(std::lround(double(value) * 1024.0))) & 0x1FFFFFu;
    };
    return quantize(position.x()) | (quantize(position.y()) << 21) | (quantize(position.z()) << 42);
}

} // namespace

QVector<int> triangleElements(const PreviewMesh& mesh)
{
    const int vertexCount = int(mesh.vertices.size());
    UnionFind sets(vertexCount);
    // Weld coincident positions.
    QHash<quint64, int> firstAt;
    firstAt.reserve(vertexCount);
    for (int i = 0; i < vertexCount; ++i) {
        const quint64 key = quantizedPosition(mesh.vertices[i].position);
        const auto found = firstAt.constFind(key);
        if (found == firstAt.constEnd()) firstAt.insert(key, i);
        else sets.unite(*found, i);
    }
    // Triangles join their vertices.
    for (qsizetype i = 0; i + 2 < mesh.indices.size(); i += 3) {
        sets.unite(int(mesh.indices[i]), int(mesh.indices[i + 1]));
        sets.unite(int(mesh.indices[i]), int(mesh.indices[i + 2]));
    }
    QVector<int> elements(int(mesh.indices.size() / 3));
    for (int triangle = 0; triangle < elements.size(); ++triangle)
        elements[triangle] = sets.find(int(mesh.indices[qsizetype(triangle) * 3]));
    return elements;
}

QVector<quint32> trianglesInElement(const PreviewMesh& mesh, const QVector<int>& elements, int triangle)
{
    QVector<quint32> result;
    if (triangle < 0 || triangle >= elements.size()) return result;
    const int element = elements[triangle];
    for (int i = 0; i < elements.size(); ++i)
        if (elements[i] == element) result.append(quint32(i));
    Q_UNUSED(mesh);
    return result;
}

QVector<quint32> trianglesInSubmesh(const PreviewMesh& mesh, int triangle)
{
    QVector<quint32> result;
    const qsizetype firstIndex = qsizetype(triangle) * 3;
    if (triangle < 0 || firstIndex + 2 >= mesh.indices.size()) return result;
    if (mesh.submeshes.isEmpty()) {
        for (int i = 0; i < int(mesh.indices.size() / 3); ++i) result.append(quint32(i));
        return result;
    }
    for (const PreviewSubmesh& submesh : mesh.submeshes) {
        if (firstIndex < submesh.firstIndex || firstIndex >= qsizetype(submesh.firstIndex) + submesh.indexCount) continue;
        for (int i = submesh.firstIndex / 3; i < (submesh.firstIndex + submesh.indexCount) / 3; ++i)
            result.append(quint32(i));
        return result;
    }
    return result;
}

void rasterizeFill(QImage& mask, const PreviewMesh& mesh, const QVector<quint32>& triangles, float value)
{
    if (mask.isNull() || !mesh.isValid()) return;
    const int width = mask.width(), height = mask.height();
    const uchar shade = uchar(std::clamp(int(std::lround(value * 255.0f)), 0, 255));
    for (quint32 triangle : triangles) {
        const qsizetype base = qsizetype(triangle) * 3;
        if (base + 2 >= mesh.indices.size()) continue;
        QPointF uv[3];
        for (int corner = 0; corner < 3; ++corner) {
            const QVector2D& raw = mesh.vertices[int(mesh.indices[base + corner])].uv0;
            uv[corner] = QPointF(double(raw.x()) * width, double(raw.y()) * height);
        }
        const double minX = std::floor(std::min({uv[0].x(), uv[1].x(), uv[2].x()}));
        const double maxX = std::ceil(std::max({uv[0].x(), uv[1].x(), uv[2].x()}));
        const double minY = std::floor(std::min({uv[0].y(), uv[1].y(), uv[2].y()}));
        const double maxY = std::ceil(std::max({uv[0].y(), uv[1].y(), uv[2].y()}));
        const double area = (uv[1].x() - uv[0].x()) * (uv[2].y() - uv[0].y())
            - (uv[2].x() - uv[0].x()) * (uv[1].y() - uv[0].y());
        if (std::abs(area) < 1e-9) continue;
        for (int y = std::max(0, int(minY)); y <= std::min(height - 1, int(maxY)); ++y) {
            uchar* line = mask.scanLine(y);
            for (int x = std::max(0, int(minX)); x <= std::min(width - 1, int(maxX)); ++x) {
                const QPointF p(x + 0.5, y + 0.5);
                const double w0 = ((uv[1].x() - p.x()) * (uv[2].y() - p.y())
                    - (uv[2].x() - p.x()) * (uv[1].y() - p.y())) / area;
                const double w1 = ((uv[2].x() - p.x()) * (uv[0].y() - p.y())
                    - (uv[0].x() - p.x()) * (uv[2].y() - p.y())) / area;
                const double w2 = 1.0 - w0 - w1;
                // Half-pixel tolerance keeps seams sealed against bilinear
                // sampling in the converter's ScaledReader.
                const double tolerance = -0.75 / std::sqrt(std::abs(area));
                if (w0 < tolerance || w1 < tolerance || w2 < tolerance) continue;
                line[x * 4] = shade;
                line[x * 4 + 1] = shade;
                line[x * 4 + 2] = shade;
                line[x * 4 + 3] = 255;
            }
        }
    }
}

QImage neutralMask(int size)
{
    QImage mask(size, size, QImage::Format_RGBA8888);
    mask.fill(QColor(128, 128, 128, 255));
    return mask;
}

} // namespace MaskPaint
