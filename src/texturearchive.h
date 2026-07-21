#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace TextureArchive {

// Fallout 4 BA2 support used by the editor's texture resolver and browser.
// Paths are returned in material form (the leading "textures/" is omitted).
QStringList texturePaths(const QString& archivePath, QString* error = nullptr);
bool contains(const QString& archivePath, const QString& texturePath);
QByteArray extract(const QString& archivePath, const QString& texturePath, QString* error = nullptr);
void clearCache();

} // namespace TextureArchive
