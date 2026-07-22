#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace TextureArchive {

// Fallout 4 / Fallout 76 BA2 support (versions 1, 7, and 8 share the same
// layout) used by the editor's texture resolver, the browser, and the
// converter's archive-direct reads.
// Texture paths are returned in material form (the leading "textures/" is
// omitted); other entries keep their full archive paths.
QStringList texturePaths(const QString& archivePath, QString* error = nullptr);
// Full display paths of entries whose name ends with `suffix`
// (case-insensitive), e.g. ".bgsm" over a materials archive.
QStringList entryPaths(const QString& archivePath, const QString& suffix, QString* error = nullptr);
bool contains(const QString& archivePath, const QString& texturePath);
QByteArray extract(const QString& archivePath, const QString& texturePath, QString* error = nullptr);
void clearCache();

} // namespace TextureArchive
