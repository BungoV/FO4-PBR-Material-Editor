#pragma once

#include <QImage>
#include <QString>
#include <QVector>

namespace TextureImage {

struct Diagnostics {
    bool valid = false;
    QString resolvedPath;
    int width = 0;
    int height = 0;
    int mipLevels = 1;
    qint64 fileSize = 0;
    QString format;
    QString colorSpace;
    QStringList channels;
    QString error;

    QString summary() const;
    // Compression written the way an artist says it: "BC7 sRGB",
    // "BC5 Linear Signed". BC4/BC5 are data formats and are always linear, so
    // their label carries signedness where the colour formats carry the space.
    QString compressionLabel() const;
};

struct Resource {
    QString path;
    QString source;
    QString absolutePath;
    bool archived = false;
};

QString resolvePath(const QString& authoredPath, const QString& textureRoot = {});
QString authoredPathForFile(const QString& absolutePath, const QString& textureRoot = {});
QStringList searchRoots(const QString& textureRoot = {});
QStringList archivePaths();
QVector<Resource> enumerate(const QString& textureRoot = {}, QStringList* errors = nullptr);
QImage load(const QString& authoredPath, const QString& textureRoot = {}, QString* error = nullptr);
QImage loadArraySlice(const QString& authoredPath, int slice, const QString& textureRoot = {}, int* sliceCount = nullptr, QString* error = nullptr);
QVector<QImage> loadCubemapFaces(const QString& authoredPath, const QString& textureRoot = {}, QString* error = nullptr);
QImage isolateChannel(const QImage& source, const QString& channel);
Diagnostics inspect(const QString& authoredPath, const QString& textureRoot = {});

} // namespace TextureImage
