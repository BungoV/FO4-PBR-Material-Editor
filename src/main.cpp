#include "mainwindow.h"
#include "materialpreviewwidget.h"
#include "texturecache.h"
#include "textureimage.h"

#include <QApplication>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QIcon>
#include <QSettings>
#include <QSurfaceFormat>
#include <QTextStream>
#include <QTimer>

namespace {

// Headless validation gate for scripting mod releases: document-level checks
// that need no UI. Page-level spec checks (per-shader slot requirements) stay
// in the editor's Validation tab.
int runHeadlessValidation(const QStringList& targets)
{
    QTextStream out(stdout);
    QSettings settings;
    QString textureRoot = settings.value(QStringLiteral("textureRoot")).toString();
    if (textureRoot.isEmpty())
        textureRoot = settings.value(QStringLiteral("TextureSources/modFolders")).toStringList().value(0);

    QStringList files;
    for (const QString& target : targets) {
        const QFileInfo info(target);
        if (info.isDir()) {
            QDirIterator it(info.absoluteFilePath(), {QStringLiteral("*.pbrm")}, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) files.append(it.next());
        } else if (info.isFile()) {
            files.append(info.absoluteFilePath());
        } else {
            out << target << ": error: path does not exist\n";
        }
    }
    if (files.isEmpty()) {
        out << "no .pbrm files found\n";
        return 1;
    }

    int errorCount = 0, warningCount = 0;
    for (const QString& file : files) {
        Pbr::MaterialDocument document;
        QString loadError;
        if (!document.load(file, &loadError)) {
            out << file << ": error: " << loadError << '\n';
            ++errorCount;
            continue;
        }
        // v5: legacy per-material surface state data blocks export in the GUI
        // validation; surface the same errors here so scripted pipelines and
        // batch checks see them too.
        bool legacyGroups = false;
        for (const QString& legacyName : {QStringLiteral("Wetness"), QStringLiteral("Dust"), QStringLiteral("Frost"), QStringLiteral("Heat"), QStringLiteral("Blood")})
            legacyGroups = legacyGroups || !document.settingGroup(QStringLiteral("SurfaceStates/") + legacyName).isEmpty();
        for (int slot = 0; slot < Pbr::MaxSurfaceStates; ++slot)
            legacyGroups = legacyGroups || !document.settingGroup(QStringLiteral("SurfaceStates/State%1").arg(slot)).isEmpty();
        if (legacyGroups) {
            out << file << ": error: legacy per-material surface state groups; extract to a definitions JSON and strip them via Batch Operations\n";
            ++errorCount;
        }
        for (const QString& setName : {QStringLiteral("UV0"), QStringLiteral("UV1"), QStringLiteral("UV2")}) {
            const QJsonObject set = document.textureSet(setName);
            for (auto it = set.constBegin(); it != set.constEnd(); ++it) {
                if (it.key().endsWith(QStringLiteral("SurfaceStateResponse"))) {
                    out << file << ": error: " << setName << ": retired Surface State Response input; repack R into RMAOS alpha and B into Normal alpha, then strip legacy surface data\n";
                    ++errorCount;
                }
                const QJsonObject slotValues = it.value().toObject().value(QStringLiteral("values")).toObject();
                if (it.key().endsWith(QStringLiteral("Normal")) && slotValues.value(QStringLiteral("layerMaskInAlpha")).toBool(false)) {
                    out << file << ": warning: " << setName << '/' << it.key()
                        << ": retired layer-modulation-in-alpha flag; repack the mask to a Layer Coverage texture or use Height Blend coverage\n";
                    ++warningCount;
                }
            }
        }
        const auto checkSet = [&](const QString& setName, const QJsonObject& set) {
            for (auto it = set.constBegin(); it != set.constEnd(); ++it) {
                const auto state = Pbr::TextureSlotState::fromJson(it.value().toObject());
                if (!state.enabled) continue;
                if (TextureCache::hasChannelSwaps(state.values)) {
                    out << file << ": warning: " << setName << '/' << it.key()
                        << ": unbaked channel sources; export a packed texture before runtime use\n";
                    ++warningCount;
                }
                if (state.path.trimmed().isEmpty()) continue;
                if (TextureImage::resolvePath(state.path, textureRoot).isEmpty()) {
                    out << file << ": error: " << setName << '/' << it.key()
                        << ": texture cannot be resolved: " << state.path << '\n';
                    ++errorCount;
                    continue;
                }
                const TextureImage::Diagnostics info = TextureImage::inspect(state.path, textureRoot);
                if (!info.valid) {
                    out << file << ": error: " << setName << '/' << it.key() << ": " << info.error << '\n';
                    ++errorCount;
                } else if (qMax(info.width, info.height) > 1 && info.mipLevels == 1) {
                    out << file << ": warning: " << setName << '/' << it.key() << ": no mip chain\n";
                    ++warningCount;
                }
            }
        };
        for (const QString& set : {QStringLiteral("UV0"), QStringLiteral("UV1"), QStringLiteral("UV2")})
            checkSet(set, document.textureSet(set));
        const QJsonObject resources = document.toJson().value(QStringLiteral("resources")).toObject();
        for (auto it = resources.constBegin(); it != resources.constEnd(); ++it)
            checkSet(it.key(), it.value().toObject());
    }
    out << files.size() << " file(s), " << errorCount << " error(s), " << warningCount << " warning(s)\n";
    return errorCount > 0 ? 1 : 0;
}

} // namespace

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

    const int validateIndex = application.arguments().indexOf(QStringLiteral("--validate"));
    if (validateIndex >= 0)
        return runHeadlessValidation(application.arguments().mid(validateIndex + 1));

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
