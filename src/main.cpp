#include "clicommands.h"
#include "mainwindow.h"
#include "materialpreviewwidget.h"

#include <QApplication>
#include <QFile>
#include <QFont>
#include <QIcon>
#include <QSurfaceFormat>
#include <QTextStream>
#include <QTimer>

int main(int argc, char* argv[])
{
    QSurfaceFormat previewFormat;
    previewFormat.setRenderableType(QSurfaceFormat::OpenGL);
    previewFormat.setVersion(3, 3);
    previewFormat.setProfile(QSurfaceFormat::CoreProfile);
    previewFormat.setDepthBufferSize(24);
    previewFormat.setSamples(4);
    QSurfaceFormat::setDefaultFormat(previewFormat);

    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("FO4CommunityShaders"));
    QCoreApplication::setApplicationName(QStringLiteral("PBRMaterialEditor"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    application.setWindowIcon(QIcon(QStringLiteral(":/app-icon.png")));

    // Compatibility alias; the pbrm.exe console companion is the primary CLI.
    const int validateIndex = application.arguments().indexOf(QStringLiteral("--validate"));
    if (validateIndex >= 0)
        return PbrmCli::runValidate(application.arguments().mid(validateIndex + 1));

    QFile style(QStringLiteral(":/style.qss"));
    if (style.open(QIODevice::ReadOnly))
        application.setStyleSheet(QString::fromUtf8(style.readAll()));

    MainWindow window;
    const int smokeIndex = application.arguments().indexOf(QStringLiteral("--ui-smoke"));
    if (application.arguments().size() > 1 && smokeIndex != 1)
        window.openFile(application.arguments().at(1));
    else
        window.restoreLastSession();
    window.showMaximized();
    if (smokeIndex >= 0 && smokeIndex + 1 < application.arguments().size()) {
        const QString output = application.arguments().at(smokeIndex + 1);
        QTimer::singleShot(1800, &window, [&application, &window, output] {
            window.grab().save(output);
            const auto* preview = window.findChild<PbrPreviewGL*>();
            application.exit(preview && preview->rendererReady() ? 0 : 2);
        });
    }
    return application.exec();
}
