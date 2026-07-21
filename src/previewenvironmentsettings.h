#pragma once

#include <QString>

class QSettings;

namespace PreviewEnvironmentSettings {

QString filePath();
void migrateLegacy(QSettings& portableSettings);

} // namespace PreviewEnvironmentSettings
