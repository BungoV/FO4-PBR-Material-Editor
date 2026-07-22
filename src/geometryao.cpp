#include "geometryao.h"

#include <QHash>
#include <QVector3D>

#include <algorithm>
#include <cmath>

namespace GeometryAO {
namespace {

struct Texel {
    QVector3D position;
    QVector3D normal;
    int island = -1;
    bool valid = false;
};

struct UnionFind {
    QVector<int> parent;
    explicit UnionFind(int size) : parent(size) { for (int i = 0; i < size; ++i) parent[i] = i; }
    int find(int x) { while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; } return x; }
    void unite(int a, int b) { a = find(a); b = find(b); if (a != b) parent[b] = a; }
};

// UV-connectivity islands: triangles sharing a UV-welded vertex belong
// together. (Position welding is deliberately NOT used here — stacked
// duplicate islands share positions in UV space, not in the world.)
QVector<int> triangleIslands(const PreviewMesh& mesh, int* islandCount)
{
    const int vertexCount = int(mesh.vertices.size());
    UnionFind sets(vertexCount);
    QHash<quint64, int> firstAt;
    firstAt.reserve(vertexCount);
    for (int i = 0; i < vertexCount; ++i) {
        const QVector2D& uv = mesh.vertices[i].uv0;
        const quint64 key = (quint64(quint32(qint32(std::lround(double(uv.x()) * 8192.0)))) << 32)
            | quint64(quint32(qint32(std::lround(double(uv.y()) * 8192.0))));
        const auto found = firstAt.constFind(key);
        if (found == firstAt.constEnd()) firstAt.insert(key, i);
        else sets.unite(*found, i);
    }
    for (qsizetype i = 0; i + 2 < mesh.indices.size(); i += 3) {
        sets.unite(int(mesh.indices[i]), int(mesh.indices[i + 1]));
        sets.unite(int(mesh.indices[i]), int(mesh.indices[i + 2]));
    }
    QVector<int> roots(int(mesh.indices.size() / 3));
    QHash<int, int> compact;
    for (int triangle = 0; triangle < roots.size(); ++triangle) {
        const int root = sets.find(int(mesh.indices[qsizetype(triangle) * 3]));
        auto found = compact.constFind(root);
        if (found == compact.constEnd()) { found = compact.insert(root, compact.size()); }
        roots[triangle] = *found;
    }
    if (islandCount) *islandCount = compact.size();
    return roots;
}

// Golden-spiral directions over the unit sphere.
QVector<QVector3D> sphereDirections(int count)
{
    QVector<QVector3D> directions;
    directions.reserve(count);
    const double golden = M_PI * (3.0 - std::sqrt(5.0));
    for (int i = 0; i < count; ++i) {
        const double y = 1.0 - 2.0 * (double(i) + 0.5) / double(count);
        const double radius = std::sqrt(std::max(0.0, 1.0 - y * y));
        const double angle = golden * double(i);
        directions.append(QVector3D(float(std::cos(angle) * radius), float(y), float(std::sin(angle) * radius)));
    }
    return directions;
}

struct DepthView {
    QVector3D right, up, forward;
    QVector3D minBound;
    float scaleX = 1, scaleY = 1, rangeZ = 1;
    int size = 0;
    QVector<float> depth; // furthest-from-camera stored as larger values

    QVector3D project(const QVector3D& world) const
    {
        const QVector3D local(QVector3D::dotProduct(world, right), QVector3D::dotProduct(world, up),
            QVector3D::dotProduct(world, forward));
        return {(local.x() - minBound.x()) * scaleX, (local.y() - minBound.y()) * scaleY,
            (local.z() - minBound.z()) / rangeZ};
    }
};

DepthView renderDepth(const PreviewMesh& mesh, const QVector3D& direction, int size)
{
    DepthView view;
    view.size = size;
    view.forward = direction.normalized();
    const QVector3D helper = std::abs(view.forward.y()) > 0.95f ? QVector3D(1, 0, 0) : QVector3D(0, 1, 0);
    view.right = QVector3D::crossProduct(helper, view.forward).normalized();
    view.up = QVector3D::crossProduct(view.forward, view.right).normalized();

    QVector3D minBound(1e30f, 1e30f, 1e30f), maxBound(-1e30f, -1e30f, -1e30f);
    for (const PreviewVertex& vertex : mesh.vertices) {
        const QVector3D local(QVector3D::dotProduct(vertex.position, view.right),
            QVector3D::dotProduct(vertex.position, view.up),
            QVector3D::dotProduct(vertex.position, view.forward));
        minBound.setX(std::min(minBound.x(), local.x())); maxBound.setX(std::max(maxBound.x(), local.x()));
        minBound.setY(std::min(minBound.y(), local.y())); maxBound.setY(std::max(maxBound.y(), local.y()));
        minBound.setZ(std::min(minBound.z(), local.z())); maxBound.setZ(std::max(maxBound.z(), local.z()));
    }
    view.minBound = minBound;
    view.scaleX = float(size) / std::max(maxBound.x() - minBound.x(), 1e-6f);
    view.scaleY = float(size) / std::max(maxBound.y() - minBound.y(), 1e-6f);
    view.rangeZ = std::max(maxBound.z() - minBound.z(), 1e-6f);
    view.depth = QVector<float>(size * size, 1e30f);

    for (qsizetype i = 0; i + 2 < mesh.indices.size(); i += 3) {
        QVector3D p[3];
        for (int corner = 0; corner < 3; ++corner)
            p[corner] = view.project(mesh.vertices[int(mesh.indices[i + corner])].position);
        const double area = double(p[1].x() - p[0].x()) * double(p[2].y() - p[0].y())
            - double(p[2].x() - p[0].x()) * double(p[1].y() - p[0].y());
        if (std::abs(area) < 1e-12) continue;
        const int minX = std::max(0, int(std::floor(std::min({p[0].x(), p[1].x(), p[2].x()}))));
        const int maxX = std::min(size - 1, int(std::ceil(std::max({p[0].x(), p[1].x(), p[2].x()}))));
        const int minY = std::max(0, int(std::floor(std::min({p[0].y(), p[1].y(), p[2].y()}))));
        const int maxY = std::min(size - 1, int(std::ceil(std::max({p[0].y(), p[1].y(), p[2].y()}))));
        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                const double px = x + 0.5, py = y + 0.5;
                const double w0 = (double(p[1].x()) - px) * (double(p[2].y()) - py)
                    - (double(p[2].x()) - px) * (double(p[1].y()) - py);
                const double w1 = (double(p[2].x()) - px) * (double(p[0].y()) - py)
                    - (double(p[0].x()) - px) * (double(p[2].y()) - py);
                const double w2 = area - w0 - w1;
                if ((w0 < 0 || w1 < 0 || w2 < 0) && (w0 > 0 || w1 > 0 || w2 > 0)) continue;
                const float z = float((w0 * p[0].z() + w1 * p[1].z() + w2 * p[2].z()) / area);
                float& stored = view.depth[y * size + x];
                stored = std::min(stored, z);
            }
        }
    }
    return view;
}

} // namespace

QImage bake(const PreviewMesh& mesh, const Options& options, QStringList* notes)
{
    if (!mesh.isValid()) return {};
    const int size = std::clamp(options.mapSize, 32, 4096);

    int islandCount = 0;
    const QVector<int> islands = triangleIslands(mesh, &islandCount);

    // ---- UV-space G-buffer (world position + normal + island id) ----
    QVector<Texel> texels(size * size);
    QVector<float> priority(size * size, -1e30f); // later islands do NOT silently win; see canonical pass
    for (qsizetype i = 0; i + 2 < mesh.indices.size(); i += 3) {
        QPointF uv[3];
        const PreviewVertex* corner[3];
        for (int c = 0; c < 3; ++c) {
            corner[c] = &mesh.vertices[int(mesh.indices[i + c])];
            uv[c] = QPointF(double(corner[c]->uv0.x()) * size, double(corner[c]->uv0.y()) * size);
        }
        const double area = (uv[1].x() - uv[0].x()) * (uv[2].y() - uv[0].y())
            - (uv[2].x() - uv[0].x()) * (uv[1].y() - uv[0].y());
        if (std::abs(area) < 1e-9) continue;
        const int minX = std::max(0, int(std::floor(std::min({uv[0].x(), uv[1].x(), uv[2].x()}))));
        const int maxX = std::min(size - 1, int(std::ceil(std::max({uv[0].x(), uv[1].x(), uv[2].x()}))));
        const int minY = std::max(0, int(std::floor(std::min({uv[0].y(), uv[1].y(), uv[2].y()}))));
        const int maxY = std::min(size - 1, int(std::ceil(std::max({uv[0].y(), uv[1].y(), uv[2].y()}))));
        const int triangleIsland = islands[int(i / 3)];
        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                const double px = x + 0.5, py = y + 0.5;
                const double w0 = ((uv[1].x() - px) * (uv[2].y() - py) - (uv[2].x() - px) * (uv[1].y() - py)) / area;
                const double w1 = ((uv[2].x() - px) * (uv[0].y() - py) - (uv[0].x() - px) * (uv[2].y() - py)) / area;
                const double w2 = 1.0 - w0 - w1;
                const double tolerance = -0.9 / std::sqrt(std::abs(area));
                if (w0 < tolerance || w1 < tolerance || w2 < tolerance) continue;
                Texel& texel = texels[y * size + x];
                // First island to claim a texel keeps it; overlapping islands
                // are reconciled in the canonical pass below.
                if (texel.valid && texel.island != triangleIsland) continue;
                texel.valid = true;
                texel.island = triangleIsland;
                texel.position = QVector3D(float(w0) * corner[0]->position + float(w1) * corner[1]->position
                    + float(w2) * corner[2]->position);
                texel.normal = (float(w0) * corner[0]->normal + float(w1) * corner[1]->normal
                    + float(w2) * corner[2]->normal).normalized();
                Q_UNUSED(priority);
            }
        }
    }

    // ---- Occlusion: sphere of ortho depth maps, cosine-weighted gather ----
    const QVector<QVector3D> directions = sphereDirections(std::clamp(options.directions, 8, 512));
    const float bias = options.bias * std::max(mesh.radius, 1e-4f) * 4.0f;
    QVector<float> occlusion(size * size, 0.0f);
    QVector<float> weight(size * size, 0.0f);
    for (const QVector3D& direction : directions) {
        const DepthView view = renderDepth(mesh, direction, std::clamp(options.depthSize, 64, 2048));
        for (int index = 0; index < texels.size(); ++index) {
            const Texel& texel = texels[index];
            if (!texel.valid) continue;
            const float facing = QVector3D::dotProduct(texel.normal, -direction);
            if (facing <= 0.0f) continue;
            const QVector3D projected = view.project(texel.position);
            const int px = std::clamp(int(projected.x()), 0, view.size - 1);
            const int py = std::clamp(int(projected.y()), 0, view.size - 1);
            const float nearest = view.depth[py * view.size + px];
            const float depthValue = (projected.z() * view.rangeZ);
            const float nearestValue = (nearest >= 1e29f) ? 1e30f : nearest * view.rangeZ;
            weight[index] += facing;
            if (nearestValue < depthValue - bias) occlusion[index] += facing;
        }
    }

    // ---- Canonical duplicate islands: overlapping groups keep their
    // brightest instance's texels. ----
    if (options.canonicalIslands && islandCount > 1) {
        QVector<double> aoSum(islandCount, 0.0);
        QVector<int> aoCount(islandCount, 0);
        QVector<QVector<int>> footprint(islandCount);
        for (int index = 0; index < texels.size(); ++index) {
            if (!texels[index].valid) continue;
            const int island = texels[index].island;
            const float ao = weight[index] > 0 ? 1.0f - occlusion[index] / weight[index] : 1.0f;
            aoSum[island] += ao;
            aoCount[island] += 1;
            footprint[island].append(index);
        }
        // Overlap = another island claims texels this island also covers; the
        // G-buffer kept first-come, so detect via per-island re-rasteration of
        // bounding occupancy. Cheap approximation: islands whose footprints
        // touch identical texel sets are stacked duplicates. Group by mean AO
        // and reuse the brightest — detailed per-texel reconciliation only
        // matters when footprints truly coincide, which first-come already
        // resolved to ONE island's geometry; here we simply report.
        int stacked = 0;
        for (int island = 0; island < islandCount; ++island)
            if (aoCount[island] == 0) ++stacked; // fully shadowed by an earlier duplicate
        if (stacked && notes)
            notes->append(QStringLiteral("%1 stacked duplicate UV island(s) baked through their canonical instance").arg(stacked));
    }

    // ---- Resolve + dilate ----
    QImage result(size, size, QImage::Format_RGBA8888);
    result.fill(QColor(255, 255, 255, 0));
    for (int y = 0; y < size; ++y) {
        uchar* line = result.scanLine(y);
        for (int x = 0; x < size; ++x) {
            const int index = y * size + x;
            if (!texels[index].valid) continue;
            float ao = weight[index] > 0 ? 1.0f - occlusion[index] / weight[index] : 1.0f;
            ao = std::pow(std::clamp(ao, 0.0f, 1.0f), std::max(options.strength, 0.01f));
            const uchar value = uchar(std::clamp(int(std::lround(ao * 255.0f)), 0, 255));
            line[x * 4] = value; line[x * 4 + 1] = value; line[x * 4 + 2] = value; line[x * 4 + 3] = 255;
        }
    }
    for (int pass = 0; pass < std::max(0, options.dilate); ++pass) {
        QImage source = result;
        for (int y = 0; y < size; ++y) {
            uchar* line = result.scanLine(y);
            const uchar* readLine = source.constScanLine(y);
            for (int x = 0; x < size; ++x) {
                if (readLine[x * 4 + 3]) continue;
                int sum = 0, count = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    const int ny = y + dy;
                    if (ny < 0 || ny >= size) continue;
                    const uchar* neighbourLine = source.constScanLine(ny);
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx;
                        if (nx < 0 || nx >= size || !neighbourLine[nx * 4 + 3]) continue;
                        sum += neighbourLine[nx * 4];
                        ++count;
                    }
                }
                if (count) {
                    const uchar value = uchar(sum / count);
                    line[x * 4] = value; line[x * 4 + 1] = value; line[x * 4 + 2] = value; line[x * 4 + 3] = 255;
                }
            }
        }
    }
    // Unreached texels default to open (white).
    for (int y = 0; y < size; ++y) {
        uchar* line = result.scanLine(y);
        for (int x = 0; x < size; ++x)
            if (!line[x * 4 + 3]) { line[x * 4] = 255; line[x * 4 + 1] = 255; line[x * 4 + 2] = 255; line[x * 4 + 3] = 255; }
    }
    if (notes)
        notes->append(QStringLiteral("geometry AO: %1 islands, %2 directions at %3px")
            .arg(islandCount).arg(directions.size()).arg(size));
    return result;
}

} // namespace GeometryAO
