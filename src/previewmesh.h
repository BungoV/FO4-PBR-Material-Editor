#pragma once

#include <QMatrix4x4>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>

struct PreviewVertex {
    QVector3D position;
    QVector3D normal;
    QVector4D tangent;
    QVector2D uv0;
    QVector2D uv1;
    QVector4D color{1, 1, 1, 1};
};

struct PreviewSubmesh {
    QString name;
    int firstIndex = 0;
    int indexCount = 0;
};

struct PreviewMesh {
    QVector<PreviewVertex> vertices;
    QVector<quint32> indices;
    QVector<PreviewSubmesh> submeshes;
    QString sourcePath;
    QStringList diagnostics;
    bool hasUv1 = false;
    bool generatedNormals = false;
    bool generatedTangents = false;
    float radius = 1.0f;

    bool isValid() const { return !vertices.isEmpty() && indices.size() >= 3; }
};

namespace PreviewMeshes {

enum class PlaneOrientation { XY, XZ, YZ };

PreviewMesh plane(PlaneOrientation orientation = PlaneOrientation::XY, bool twoSided = true);
PreviewMesh planeGrid(PlaneOrientation orientation = PlaneOrientation::XY, int subdivisions = 64);
PreviewMesh cube();
PreviewMesh sphere(int segments = 128, int rings = 64);
void normalizeAndCenter(PreviewMesh& mesh);
void generateMissingNormals(PreviewMesh& mesh);
void generateTangents(PreviewMesh& mesh, int uvSet = 0);
PreviewMesh loadStaticFbx(const QString& path, QString* error = nullptr);
PreviewMesh loadStaticNif(const QString& path, QString* error = nullptr);

} // namespace PreviewMeshes
