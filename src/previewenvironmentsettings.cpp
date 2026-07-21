#include "previewenvironmentsettings.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>

namespace PreviewEnvironmentSettings {

QString filePath()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("preview-environments.ini"));
}

void migrateLegacy(QSettings& portableSettings)
{
    constexpr auto migrationKey = "Catalog/migratedFromRegistry";
    if (portableSettings.value(QString::fromLatin1(migrationKey), false).toBool()) return;

    QSettings legacy(QSettings::NativeFormat, QSettings::UserScope,
        QCoreApplication::organizationName(), QCoreApplication::applicationName());
    const QStringList keys {
        QStringLiteral("PreviewCubemaps/defaultPath"),
        QStringLiteral("PreviewCubemaps/activePath"),
        QStringLiteral("PreviewCubemaps/names"),
        QStringLiteral("PreviewCubemaps/paths")
    };
    for (const QString& key : keys) {
        if (!portableSettings.contains(key) && legacy.contains(key))
            portableSettings.setValue(key, legacy.value(key));
    }
    portableSettings.setValue(QString::fromLatin1(migrationKey), true);
    portableSettings.sync();
}

} // namespace PreviewEnvironmentSettings
