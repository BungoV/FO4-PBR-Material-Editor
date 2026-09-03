#include "mainwindow.h"

#include "colorpicker.h"
#include "convertworkspace.h"
#include "disclosureicon.h"
#include "dragnumericedit.h"
#include "mainwindow_detail.h"
#include "materialpreviewwidget.h"
#include "previewenvironmentsettings.h"
#include "texturearchive.h"
#include "texturebrowserdialog.h"
#include "textureimage.h"
#include "upscaler.h"
#include "wheelfocusguard.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QClipboard>
#include <QDateTime>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QPlainTextEdit>
#include <QUrl>
#include <QComboBox>
#include <QCompleter>
#include <QCoreApplication>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QDir>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QJsonDocument>
#include <QMenuBar>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QStatusBar>
#include <QStandardPaths>
#include <QStringListModel>
#include <QHeaderView>
#include <QToolButton>
#include <QTreeWidget>
#include <QTimer>
#include <QUndoCommand>
#include <QUndoStack>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace {

class DocumentCommand final : public QUndoCommand {
public:
    DocumentCommand(const QString& text, const Pbr::MaterialDocument& before, const Pbr::MaterialDocument& after,
        std::function<void(const Pbr::MaterialDocument&)> apply)
        : QUndoCommand(text), m_before(before), m_after(after), m_apply(std::move(apply))
        , m_timestamp(QDateTime::currentMSecsSinceEpoch()) {}

    void undo() override { m_apply(m_before); }
    void redo() override
    {
        if (m_firstRedo) { m_firstRedo = false; return; }
        m_apply(m_after);
    }

    // Same-description edits arriving in quick succession (a value drag emits
    // one command per mouse move) collapse into a single undo step.
    int id() const override { return 0xD0C; }
    bool mergeWith(const QUndoCommand* other) override
    {
        const auto* command = static_cast<const DocumentCommand*>(other);
        if (command->text() != text()) return false;
        if (command->m_timestamp - m_timestamp > 800) return false;
        m_after = command->m_after;
        m_timestamp = command->m_timestamp;
        return true;
    }

private:
    Pbr::MaterialDocument m_before;
    Pbr::MaterialDocument m_after;
    std::function<void(const Pbr::MaterialDocument&)> m_apply;
    bool m_firstRedo = true;
    qint64 m_timestamp = 0;
};

QString uvLayerPageName(const QString& uvSet)
{
    if (uvSet == QStringLiteral("UV1")) return QStringLiteral("Secondary UV Textures");
    if (uvSet == QStringLiteral("UV2")) return QStringLiteral("Tertiary UV Textures");
    return QStringLiteral("Primary UV Textures");
}

QString autosaveFileName(const QString& fileName)
{
    return fileName.isEmpty() ? QString() : fileName + QStringLiteral(".autosave");
}

} // namespace


MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    QSettings settings;
    m_recentDirectory = settings.value(QStringLiteral("recentDirectory")).toString();
    m_textureRoot = settings.value(QStringLiteral("textureRoot")).toString();
    if (m_textureRoot.isEmpty()) {
        const QStringList mods = settings.value(QStringLiteral("TextureSources/modFolders")).toStringList();
        m_textureRoot = mods.value(0, settings.value(QStringLiteral("TextureSources/fallout4Textures")).toString());
    }
    m_advancedInterface = settings.value(QStringLiteral("Interface/advanced"), false).toBool();
    setAcceptDrops(true);
    m_undoStack = new QUndoStack(this);
    m_textureWatcher = new QFileSystemWatcher(this);
    m_textureReloadTimer = new QTimer(this);
    // Exporters write several times per save and often replace-by-rename, so
    // reloads are debounced and paths are re-armed after every change.
    m_textureReloadTimer->setSingleShot(true);
    m_textureReloadTimer->setInterval(400);
    connect(m_textureWatcher, &QFileSystemWatcher::fileChanged, this, &MainWindow::handleWatchedTextureChanged);
    m_autosaveTimer = new QTimer(this);
    connect(m_autosaveTimer, &QTimer::timeout, this, &MainWindow::performAutosave);
    applyAutosaveSettings();
    connect(m_textureReloadTimer, &QTimer::timeout, this, [this] {
        const QSet<QString> changed = m_pendingTextureReloads;
        m_pendingTextureReloads.clear();
        int stillPresent = 0;
        for (const QString& path : changed)
            if (QFileInfo::exists(path)) ++stillPresent;
        rebuildWorkspace(m_currentPage);
        statusBar()->showMessage(tr("Reloaded %1 changed texture(s) from disk").arg(stillPresent > 0 ? stillPresent : changed.size()), 4000);
    });
    buildWindow();
    buildMenus();
    rebuildWorkspace();
    updateWindowTitle();
    statusBar()->showMessage(tr("Ready"));
    connect(m_undoStack, &QUndoStack::cleanChanged, this, [this](bool clean) {
        m_document.setModified(!clean);
        updateWindowTitle();
    });
}

void MainWindow::buildWindow()
{
    resize(1500, 900);
    setMinimumSize(1050, 650);

    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(5, 5, 5, 5);
    root->setSpacing(5);

    auto* topBar = new QFrame(central);
    topBar->setObjectName(QStringLiteral("TopBar"));
    auto* top = new QHBoxLayout(topBar);
    top->setContentsMargins(9, 5, 9, 5);
    // Workspace switcher: the Material Editor edits one .pbrm; the Material
    // Converter runs the FO4/FO76 batch pipeline (docs/FO4-Conversion.md).
    // The converter row is added by setConverterAvailable() only while the
    // experimental option is on, so a gated-off converter has no entry here.
    m_workspaceModeCombo = new QComboBox(topBar);
    m_workspaceModeCombo->setObjectName(QStringLiteral("WorkspaceModeCombo"));
    m_workspaceModeCombo->addItem(tr("Material Editor"));
    m_workspaceModeCombo->setItemData(0, tr("Edit the open .pbrm material."), Qt::ToolTipRole);
    m_workspaceModeCombo->setToolTip(tr("Switch between authoring a material and the batch converter."));
    connect(m_workspaceModeCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        setWorkspaceMode(index == 1);
    });
    top->addWidget(m_workspaceModeCombo);
    auto* modeSeparator = new QFrame(topBar);
    modeSeparator->setFrameShape(QFrame::VLine);
    modeSeparator->setFrameShadow(QFrame::Sunken);
    top->addWidget(modeSeparator);
    m_workspaceModeSeparator = modeSeparator;
    top->addWidget(new QLabel(tr("Shader"), topBar));
    m_shaderCombo = new QComboBox(topBar);
    m_shaderCombo->addItems(Pbr::shaderModeNames());
    for (int index = 0; index < m_shaderCombo->count(); ++index) {
        const auto mode = Pbr::shaderModeFromName(m_shaderCombo->itemText(index));
        m_shaderCombo->setItemData(index, Pbr::shaderModeDescription(mode), Qt::ToolTipRole);
    }
    m_shaderCombo->setMinimumWidth(230);
    top->addWidget(m_shaderCombo);
    auto* templates = new QPushButton(topBar);
    templates->setObjectName(QStringLiteral("TemplatesButton"));
    templates->setText(tr("Templates"));
    templates->setToolTip(tr("Start a new material from practical shader and fallback defaults."));
    auto* templateMenu = new QMenu(templates);
    for (const QString& name : materialTemplateNames())
        templateMenu->addAction(name, this, [this, name] { applyMaterialTemplate(name); });
    templates->setMenu(templateMenu);
    top->addWidget(templates);
    top->addStretch();
    m_search = new QLineEdit(topBar);
    m_search->setPlaceholderText(tr("Search all material settings…"));
    m_search->setMinimumWidth(280);
    m_searchCompleter = new QCompleter(this);
    m_searchCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    m_searchCompleter->setFilterMode(Qt::MatchContains);
    m_searchCompleter->setCompletionMode(QCompleter::PopupCompletion);
    m_search->setCompleter(m_searchCompleter);
    top->addWidget(m_search);
    root->addWidget(topBar);

    m_splitter = new QSplitter(Qt::Horizontal, central);
    auto* side = new QFrame(m_splitter);
    side->setObjectName(QStringLiteral("SidePanel"));
    auto* sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(5, 7, 5, 7);
    m_navigation = new QListWidget(side);
    sideLayout->addWidget(m_navigation, 1);

    m_pages = new QStackedWidget(m_splitter);
    m_preview = new MaterialPreviewWidget(m_splitter);
    m_preview->setTextureRoot(m_textureRoot);
    m_splitter->addWidget(side);
    m_splitter->addWidget(m_pages);
    m_splitter->addWidget(m_preview);
    // The 3D viewport opens at a third of the window width by default; the
    // pages pane and the preview share resize growth so the ratio holds.
    m_splitter->setSizes({210, 795, 495});
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 2);
    m_splitter->setStretchFactor(2, 1);
    m_workspaceStack = new QStackedWidget(central);
    m_workspaceStack->addWidget(m_splitter);
    m_convertWorkspace = new ConvertWorkspace(m_workspaceStack);
    m_workspaceStack->addWidget(m_convertWorkspace);
    connect(m_convertWorkspace, &ConvertWorkspace::openInAuthorRequested, this, [this](const QString& pbrmPath) {
        if (maybeSave() && openFile(pbrmPath)) setWorkspaceMode(false);
    });
    root->addWidget(m_workspaceStack, 1);
    setCentralWidget(central);

    connect(m_shaderCombo, &QComboBox::currentTextChanged, this, [this](const QString& name) {
        const auto mode = Pbr::shaderModeFromName(name);
        applyDocumentChange(tr("Change shader"), [this, mode] { m_document.setShaderMode(mode); });
        m_shaderCombo->setToolTip(Pbr::shaderModeDescription(m_document.shaderMode()));
        rebuildWorkspace(mode == Pbr::ShaderMode::LandscapeSurface ? QStringLiteral("Landscape") : QStringLiteral("Primary UV Textures"));
        updateWindowTitle();
    });
    connect(m_navigation, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (item->data(Qt::UserRole + 3).toBool()) { toggleNavigationSection(item); return; }
        if (!item->data(Qt::UserRole).toBool()) return;
        const QString pageName = item->data(Qt::UserRole + 2).toString().isEmpty()
            ? item->text() : item->data(Qt::UserRole + 2).toString();
        showPage(pageName);
    });
    connect(m_search, &QLineEdit::textChanged, this, &MainWindow::filterCurrentPage);
    connect(m_searchCompleter, qOverload<const QString&>(&QCompleter::activated), this, &MainWindow::openSearchResult);
    connect(m_search, &QLineEdit::returnPressed, this, [this] {
        const QString query = m_search->text().trimmed();
        if (query.isEmpty()) return;
        for (auto it = m_searchTargets.cbegin(); it != m_searchTargets.cend(); ++it) {
            if (it.key().contains(query, Qt::CaseInsensitive)) { openSearchResult(it.key()); return; }
        }
    });

    // Read last so the stack the fallback switches to already exists.
    setConverterAvailable(QSettings().value(QStringLiteral("Interface/experimentalConverter"), false).toBool());
}

void MainWindow::setConverterAvailable(bool available)
{
    m_converterAvailable = available;
    const bool listed = m_workspaceModeCombo->count() > 1;
    if (available && !listed) {
        const QSignalBlocker blocker(m_workspaceModeCombo);
        m_workspaceModeCombo->addItem(tr("Material Converter"));
        m_workspaceModeCombo->setItemData(1, tr("Batch-convert FO4/FO76 .bgsm/.bgem materials and textures to PBR siblings."), Qt::ToolTipRole);
    } else if (!available && listed) {
        // Leave the converter BEFORE the row disappears: removing the current
        // item would otherwise leave the stack and the window title behind on
        // a workspace the switcher can no longer name.
        if (m_workspaceModeCombo->currentIndex() == 1) setWorkspaceMode(false);
        const QSignalBlocker blocker(m_workspaceModeCombo);
        m_workspaceModeCombo->removeItem(1);
    }
    // A switcher with one destination is dead UI, so the whole control goes
    // away with it and the converter has no visible entry point at all.
    m_workspaceModeCombo->setVisible(available);
    if (m_workspaceModeSeparator) m_workspaceModeSeparator->setVisible(available);
}

void MainWindow::showConverter()
{
    setConverterAvailable(true);
    setWorkspaceMode(true);
}

void MainWindow::setWorkspaceMode(bool convert)
{
    if (convert && !m_converterAvailable) convert = false;
    if (m_workspaceModeCombo->currentIndex() != (convert ? 1 : 0)) {
        const QSignalBlocker blocker(m_workspaceModeCombo);
        m_workspaceModeCombo->setCurrentIndex(convert ? 1 : 0);
    }
    m_workspaceStack->setCurrentIndex(convert ? 1 : 0);
    // The shader combo and settings search act on the editor document; keep
    // them visibly inert while the converter queue owns the window.
    m_shaderCombo->setEnabled(!convert);
    m_search->setEnabled(!convert);
    updateWindowTitle();
}

void MainWindow::buildMenus()
{
    auto* file = menuBar()->addMenu(tr("&File"));
    auto* newAction = file->addAction(tr("&New"), QKeySequence::New, this, &MainWindow::newDocument);
    newAction->setStatusTip(tr("Create a new PBRM material."));
    auto* openAction = file->addAction(tr("&Open…"), QKeySequence::Open, this, &MainWindow::openDocument);
    openAction->setStatusTip(tr("Open a PBRM version 4 material."));
    m_recentMenu = file->addMenu(tr("Open &Recent"));
    rebuildRecentMenu();
    file->addSeparator();
    file->addAction(tr("&Save"), QKeySequence::Save, this, &MainWindow::saveDocument);
    file->addAction(tr("Save &As…"), QKeySequence::SaveAs, this, &MainWindow::saveDocumentAs);
    m_templateMenu = file->addMenu(tr("New from &Template"));
    rebuildTemplateMenu();
    file->addAction(tr("Export Material as &JSON…"), this, [this] {
        const QString suggested = m_document.fileName().isEmpty()
            ? m_recentDirectory + QStringLiteral("/material.json") : m_document.fileName() + QStringLiteral(".json");
        const QString fileName = QFileDialog::getSaveFileName(this, tr("Export material JSON"), suggested, tr("JSON (*.json)"));
        if (fileName.isEmpty()) return;
        QFile out(fileName);
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            out.write(QJsonDocument(m_document.toJson()).toJson(QJsonDocument::Indented));
            statusBar()->showMessage(tr("Exported readable JSON to %1").arg(QFileInfo(fileName).fileName()), 4000);
        }
    });
    file->addSeparator();
    file->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);

    auto* edit = menuBar()->addMenu(tr("&Edit"));
    edit->addAction(m_undoStack->createUndoAction(this, tr("&Undo")));
    edit->addAction(m_undoStack->createRedoAction(this, tr("&Redo")));

    auto* settings = menuBar()->addMenu(tr("&Settings"));
    settings->addAction(tr("Texture Sources…"), this, &MainWindow::showTextureSourceSettings);
    settings->addAction(tr("Repair Texture Paths…"), this, &MainWindow::showPathRepairTool);
    settings->addAction(tr("Batch Operations…"), this, &MainWindow::showBatchOperations);
    settings->addAction(tr("Preview Cubemaps…"), this, &MainWindow::showEditorSettings);
    settings->addAction(tr("Texture Browser…"), this, [this] { TextureBrowserDialog(m_textureRoot, this).exec(); });
    // Upscaling: one policy for a whole conversion run, plus where the neural
    // upscaler lives. The policy is expressed against each material's OWN
    // highest-resolution map, so a single choice covers a batch of mixed sizes.
    auto* upscaling = settings->addMenu(tr("Upscaling"));
    struct PolicyEntry { const char* label; int value; };
    static const PolicyEntry policies[] = {{QT_TR_NOOP("Off"), 0},
        {QT_TR_NOOP("Match the highest map"), 1}, {QT_TR_NOOP("2x the highest map"), 2},
        {QT_TR_NOOP("4x the highest map"), 3}};
    auto* policyGroup = new QActionGroup(this);
    const int activePolicy = QSettings().value(QStringLiteral("Convert/upscalePolicy"), 0).toInt();
    for (const PolicyEntry& entry : policies) {
        QAction* action = upscaling->addAction(tr(entry.label));
        action->setCheckable(true);
        action->setChecked(entry.value == activePolicy);
        policyGroup->addAction(action);
        const int value = entry.value;
        connect(action, &QAction::triggered, this, [this, value] {
            QSettings().setValue(QStringLiteral("Convert/upscalePolicy"), value);
            statusBar()->showMessage(value == 0
                ? tr("Upscaling off — maps convert at the size they ship")
                : tr("Under-sized maps will be raised on the next conversion"), 5000);
        });
    }
    upscaling->addSeparator();
    // One model per map role, chosen once. Without this the model was a
    // substring guess against whatever happened to be installed, which is not
    // something anyone could see, predict, or change.
    upscaling->addAction(tr("Standard Models…"), this, [this] { showUpscaleModelsDialog(); });
    upscaling->addAction(tr("Choose Upscaler…"), this, [this] {
        const QString fileName = QFileDialog::getOpenFileName(this, tr("Choose the upscaler executable"),
            Upscaler::binaryPath(), tr("Executable (*.exe)"));
        if (fileName.isEmpty()) return;
        Upscaler::setBinaryPath(fileName);
        const QStringList found = Upscaler::models();
        // A binary with no models beside it is inert, and that is worth saying
        // now rather than after a batch has quietly fallen back to Lanczos.
        if (found.isEmpty())
            QMessageBox::warning(this, tr("No models found"),
                tr("No .param models sit beside %1, so upscaling will fall back to Lanczos.").arg(fileName));
        else
            statusBar()->showMessage(tr("Upscaler ready — %1 model(s) available").arg(found.size()), 8000);
    });

    // Runtime-owned surface state definitions: export the built-in defaults
    // as the starter JSON asset, or preview a modded definitions file.
    auto* stateDefinitions = settings->addMenu(tr("Surface State Definitions"));
    stateDefinitions->addAction(tr("Export Starter Definitions…"), this, [this] {
        const QString fileName = QFileDialog::getSaveFileName(this, tr("Export Surface State Definitions"),
            QStringLiteral("SurfaceStates.json"), tr("Surface State Definitions (*.json)"));
        if (fileName.isEmpty()) return;
        QString error;
        if (!Pbr::saveSurfaceStateDefinitions(fileName, Pbr::surfaceStatePresets(), &error))
            QMessageBox::warning(this, tr("Export failed"), error);
        else statusBar()->showMessage(tr("Exported built-in state definitions to %1").arg(fileName), 5000);
    });
    stateDefinitions->addAction(tr("Load Definitions…"), this, [this] {
        const QString fileName = QFileDialog::getOpenFileName(this, tr("Load Surface State Definitions"),
            QString(), tr("Surface State Definitions (*.json)"));
        if (fileName.isEmpty()) return;
        QVector<Pbr::SurfaceStateSpec> states;
        QString error;
        if (!Pbr::loadSurfaceStateDefinitions(fileName, &states, &error)) {
            QMessageBox::warning(this, tr("Load failed"), error);
            return;
        }
        QSettings().setValue(QStringLiteral("surfaceStates/definitionsPath"), fileName);
        m_preview->setMaterial(m_document);
        statusBar()->showMessage(tr("Previewing %1 state definition(s) from %2").arg(states.size()).arg(fileName), 5000);
    });
    stateDefinitions->addAction(tr("Use Built-in Defaults"), this, [this] {
        QSettings().remove(QStringLiteral("surfaceStates/definitionsPath"));
        m_preview->setMaterial(m_document);
        statusBar()->showMessage(tr("Previewing the built-in surface state definitions"), 5000);
    });
    settings->addSeparator();
    auto* interfaceMode = settings->addMenu(tr("Interface Mode"));
    auto* modeGroup = new QActionGroup(interfaceMode);
    modeGroup->setExclusive(true);
    auto* basicMode = interfaceMode->addAction(tr("Basic"));
    auto* advancedMode = interfaceMode->addAction(tr("Advanced"));
    basicMode->setCheckable(true); advancedMode->setCheckable(true);
    basicMode->setChecked(!m_advancedInterface); advancedMode->setChecked(m_advancedInterface);
    basicMode->setToolTip(tr("Show the common material-authoring controls."));
    advancedMode->setToolTip(tr("Show uncommon controls and expert pages."));
    modeGroup->addAction(basicMode); modeGroup->addAction(advancedMode);
    connect(basicMode, &QAction::triggered, this, [this] { setAdvancedInterface(false); });
    connect(advancedMode, &QAction::triggered, this, [this] { setAdvancedInterface(true); });

    auto* addFeature = settings->addMenu(tr("Add Feature…"));
    const auto exposeFeature = [this](const QString& page) {
        if (!m_pageByName.contains(page)) {
            QMessageBox::information(this, tr("Feature unavailable"),
                tr("%1 is not available for the selected shader type.").arg(page));
            return;
        }
        if (!m_advancedInterface) setAdvancedInterface(true);
        showPage(page);
    };
    addFeature->addAction(tr("Dynamic Normals"), this, [exposeFeature] { exposeFeature(QStringLiteral("Dynamic Normals")); });
    addFeature->addAction(tr("Advanced Rendering"), this, [exposeFeature] { exposeFeature(QStringLiteral("Advanced")); });

    auto* view = menuBar()->addMenu(tr("&View"));
    m_previewAction = view->addAction(tr("Material Preview"));
    m_previewAction->setCheckable(true); m_previewAction->setChecked(QSettings().value(QStringLiteral("Interface/showPreview"), true).toBool());
    m_previewAction->setShortcut(QKeySequence(Qt::Key_F9));
    connect(m_previewAction, &QAction::toggled, this, [this](bool visible) { m_preview->setVisible(visible); QSettings().setValue(QStringLiteral("Interface/showPreview"), visible); });
    m_preview->setVisible(m_previewAction->isChecked());
    view->addSeparator();
    view->addAction(tr("Expand Texture Cards"), this, [this] { setCurrentTextureCardsExpanded(true); });
    view->addAction(tr("Collapse Texture Cards"), this, [this] { setCurrentTextureCardsExpanded(false); });

    auto* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("About PBR Material Editor"), this, [this] {
        QMessageBox::about(this, tr("About PBR Material Editor"),
            tr("FO4 PBR Material Editor\n\nIndependent C++20/Qt 6 implementation for authoring experimental PBRM materials and future NifSkope integration.\n\nDeveloped by Bungo + Claude"));
    });

    // Build stamp so "which exe am I actually running?" is answerable at a
    // glance. Read from the executable's own timestamp at RUNTIME: __TIME__
    // froze at mainwindow.o's last compile and lied whenever later builds only
    // touched other translation units.
    const QDateTime linkTime = QFileInfo(QCoreApplication::applicationFilePath()).lastModified();
    auto* version = new QLabel(tr("C++ / Qt • PBRM V5 • build %1").arg(linkTime.toString(QStringLiteral("MMM d yyyy HH:mm:ss"))), this);
    version->setObjectName(QStringLiteral("VersionBadge"));
    version->setToolTip(tr("Independent C++/Qt PBRM version 5 editor. The build stamp identifies the running binary."));
    statusBar()->addPermanentWidget(version);
}

void MainWindow::showTextureSourceSettings()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Texture Sources"));
    dialog.resize(820, 650);
    auto* outer = new QVBoxLayout(&dialog);
    auto* intro = new QLabel(tr("Texture paths are saved relative to these sources. Loose mod folders have priority in list order, followed by the Fallout 4 base directory. Loose files override matching BA2 entries; archives use their own list order."), &dialog);
    intro->setWordWrap(true);
    outer->addWidget(intro);

    QSettings settings;
    auto* gameRow = new QWidget(&dialog);
    auto* gameRowLayout = new QHBoxLayout(gameRow);
    gameRowLayout->setContentsMargins(0, 0, 0, 0);
    auto* gamePath = new QLineEdit(settings.value(QStringLiteral("TextureSources/fallout4Textures")).toString(), gameRow);
    gamePath->setPlaceholderText(tr("Fallout 4 Data or Data\\Textures directory"));
    auto* browseGame = new QPushButton(tr("Browse…"), gameRow);
    gameRowLayout->addWidget(gamePath, 1);
    gameRowLayout->addWidget(browseGame);
    auto* form = new QFormLayout();
    form->addRow(tr("Fallout 4 textures"), gameRow);
    outer->addLayout(form);

    const auto addPathList = [&](const QString& title, const QStringList& initial, bool archives) {
        auto* heading = new QLabel(title, &dialog);
        heading->setObjectName(QStringLiteral("SectionTitle"));
        outer->addWidget(heading);
        auto* list = new QListWidget(&dialog);
        for (const QString& path : initial) list->addItem(QDir::toNativeSeparators(path));
        outer->addWidget(list, 1);
        auto* controls = new QHBoxLayout();
        auto* add = new QPushButton(archives ? tr("Add BA2…") : tr("Add Mod Folder…"), &dialog);
        auto* remove = new QPushButton(tr("Remove"), &dialog);
        auto* up = new QPushButton(tr("Move Up"), &dialog);
        auto* down = new QPushButton(tr("Move Down"), &dialog);
        controls->addWidget(add); controls->addWidget(remove); controls->addWidget(up); controls->addWidget(down); controls->addStretch();
        outer->addLayout(controls);
        connect(add, &QPushButton::clicked, &dialog, [&, list, archives] {
            if (archives) {
                const QStringList paths = QFileDialog::getOpenFileNames(&dialog, tr("Add Fallout 4 texture archives"),
                    gamePath->text(), tr("Fallout 4 BA2 archives (*.ba2)"));
                for (const QString& path : paths) {
                    const QString native = QDir::toNativeSeparators(path);
                    if (list->findItems(native, Qt::MatchFixedString).isEmpty()) list->addItem(native);
                }
            } else {
                const QString path = QFileDialog::getExistingDirectory(&dialog, tr("Add mod or Textures folder"), m_textureRoot);
                if (!path.isEmpty()) {
                    const QString native = QDir::toNativeSeparators(path);
                    if (list->findItems(native, Qt::MatchFixedString).isEmpty()) list->addItem(native);
                }
            }
        });
        connect(remove, &QPushButton::clicked, &dialog, [list] { delete list->takeItem(list->currentRow()); });
        connect(up, &QPushButton::clicked, &dialog, [list] {
            const int row = list->currentRow();
            if (row <= 0) return;
            auto* item = list->takeItem(row); list->insertItem(row - 1, item); list->setCurrentRow(row - 1);
        });
        connect(down, &QPushButton::clicked, &dialog, [list] {
            const int row = list->currentRow();
            if (row < 0 || row + 1 >= list->count()) return;
            auto* item = list->takeItem(row); list->insertItem(row + 1, item); list->setCurrentRow(row + 1);
        });
        return list;
    };

    QStringList mods = settings.value(QStringLiteral("TextureSources/modFolders")).toStringList();
    const QString legacy = settings.value(QStringLiteral("textureRoot")).toString();
    if (mods.isEmpty() && !legacy.isEmpty()) mods.append(legacy);
    auto* modList = addPathList(tr("Loose mod texture folders — highest priority first"), mods, false);
    auto* archiveList = addPathList(tr("Fallout 4 BA2 texture archives — highest priority first"),
        settings.value(QStringLiteral("TextureSources/archives")).toStringList(), true);

    auto* note = new QLabel(tr("The texture browser merges these sources by virtual path. A loose file overrides the same path in an archive."), &dialog);
    note->setWordWrap(true);
    outer->addWidget(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    outer->addWidget(buttons);

    connect(browseGame, &QPushButton::clicked, &dialog, [&] {
        const QString path = QFileDialog::getExistingDirectory(&dialog, tr("Select Fallout 4 Data or Textures directory"), gamePath->text());
        if (!path.isEmpty()) gamePath->setText(QDir::toNativeSeparators(path));
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;

    const auto values = [](QListWidget* list) {
        QStringList result;
        for (int i = 0; i < list->count(); ++i) result.append(list->item(i)->text());
        return result;
    };
    const QStringList modPaths = values(modList);
    settings.setValue(QStringLiteral("TextureSources/fallout4Textures"), gamePath->text().trimmed());
    settings.setValue(QStringLiteral("TextureSources/modFolders"), modPaths);
    settings.setValue(QStringLiteral("TextureSources/archives"), values(archiveList));
    settings.remove(QStringLiteral("textureRoot"));
    TextureArchive::clearCache();
    m_textureRoot = modPaths.value(0, gamePath->text().trimmed());
    m_preview->setTextureRoot(m_textureRoot);
    rebuildWorkspace(m_currentPage);
    statusBar()->showMessage(tr("Texture sources updated"), 2500);
}

void MainWindow::showEditorSettings()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Editor Settings"));
    dialog.resize(680, 420);
    auto* outer = new QVBoxLayout(&dialog);
    auto* intro = new QLabel(tr("Preview cubemaps are editor preferences only. They are never serialized into a material."), &dialog);
    intro->setWordWrap(true);
    outer->addWidget(intro);

    QSettings settings(PreviewEnvironmentSettings::filePath(), QSettings::IniFormat);
    PreviewEnvironmentSettings::migrateLegacy(settings);
    auto* form = new QFormLayout();
    auto* defaultRow = new QWidget(&dialog);
    auto* defaultLayout = new QHBoxLayout(defaultRow);
    defaultLayout->setContentsMargins(0, 0, 0, 0);
    auto* defaultPath = new QLineEdit(settings.value(QStringLiteral("PreviewCubemaps/defaultPath"),
        QStringLiteral("Shared/Cubemaps/mipblur_DefaultOutside1.dds")).toString(), defaultRow);
    auto* browseDefault = new QPushButton(tr("Browse…"), defaultRow);
    defaultLayout->addWidget(defaultPath, 1); defaultLayout->addWidget(browseDefault);
    form->addRow(tr("Default preview cubemap"), defaultRow);
    QSettings appSettings;
    auto* autosaveRow = new QWidget(&dialog);
    auto* autosaveLayout = new QHBoxLayout(autosaveRow);
    autosaveLayout->setContentsMargins(0, 0, 0, 0);
    auto* autosaveEnabled = new QCheckBox(tr("Write a recovery snapshot every"), autosaveRow);
    autosaveEnabled->setChecked(appSettings.value(QStringLiteral("Interface/autosaveEnabled"), false).toBool());
    autosaveEnabled->setToolTip(tr("Periodically snapshots a modified material to a .pbrm.autosave sidecar. The real file is only written when you save."));
    auto* autosaveMinutes = new QSpinBox(autosaveRow);
    autosaveMinutes->setRange(1, 60);
    autosaveMinutes->setSuffix(tr(" min"));
    autosaveMinutes->setValue(qBound(1, appSettings.value(QStringLiteral("Interface/autosaveMinutes"), 3).toInt(), 60));
    autosaveMinutes->setEnabled(autosaveEnabled->isChecked());
    connect(autosaveEnabled, &QCheckBox::toggled, autosaveMinutes, &QWidget::setEnabled);
    autosaveLayout->addWidget(autosaveEnabled);
    autosaveLayout->addWidget(autosaveMinutes);
    autosaveLayout->addStretch();
    form->addRow(tr("Autosave"), autosaveRow);
    auto* jsonSidecar = new QCheckBox(tr("Write a .pbrm.json sidecar on save"), &dialog);
    jsonSidecar->setChecked(appSettings.value(QStringLiteral("Interface/jsonSidecar"), false).toBool());
    jsonSidecar->setToolTip(tr("Writes a readable JSON mirror next to the material so version-control diffs are reviewable. The .pbrm stays authoritative; the sidecar is never read back."));
    form->addRow(tr("Version control"), jsonSidecar);
    // Opt-in gate for the FO4/FO76 batch converter. Off by default: the
    // workspace is unfinished, so it stays out of the interface entirely
    // until someone asks for it here.
    auto* experimentalConverter = new QCheckBox(tr("Experimental: Material Converter"), &dialog);
    experimentalConverter->setChecked(appSettings.value(QStringLiteral("Interface/experimentalConverter"), false).toBool());
    experimentalConverter->setToolTip(tr("EXPERIMENTAL and unfinished. Adds the Material Converter — the FO4/FO76 .bgsm/.bgem batch pipeline and its node graph — to the workspace dropdown. While this is off the converter is not offered anywhere in the interface, and turning it off returns the window to the Material Editor."));
    form->addRow(tr("Experimental"), experimentalConverter);
    outer->addLayout(form);

    auto* customLabel = new QLabel(tr("Custom preview cubemaps"), &dialog);
    customLabel->setObjectName(QStringLiteral("SectionTitle"));
    outer->addWidget(customLabel);
    auto* customList = new QListWidget(&dialog);
    const QStringList names = settings.value(QStringLiteral("PreviewCubemaps/names")).toStringList();
    const QStringList paths = settings.value(QStringLiteral("PreviewCubemaps/paths")).toStringList();
    for (int i = 0; i < qMin(names.size(), paths.size()); ++i) {
        auto* item = new QListWidgetItem(QStringLiteral("%1 — %2").arg(names[i], paths[i]), customList);
        item->setData(Qt::UserRole, names[i]); item->setData(Qt::UserRole + 1, paths[i]);
    }
    outer->addWidget(customList, 1);
    auto* customButtons = new QHBoxLayout();
    auto* add = new QPushButton(tr("Add…"), &dialog);
    auto* remove = new QPushButton(tr("Remove"), &dialog);
    customButtons->addWidget(add); customButtons->addWidget(remove); customButtons->addStretch();
    outer->addLayout(customButtons);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    outer->addWidget(buttons);

    connect(browseDefault, &QPushButton::clicked, &dialog, [&] {
        const QString path = QFileDialog::getOpenFileName(&dialog, tr("Select default preview cubemap"), defaultPath->text(), tr("Preview environments (*.dds *.exr *.hdr *.png *.jpg);;OpenEXR environment (*.exr);;DDS cubemap (*.dds);;All files (*.*)"));
        if (!path.isEmpty()) defaultPath->setText(QDir::toNativeSeparators(path));
    });
    connect(add, &QPushButton::clicked, &dialog, [&] {
        const QString path = QFileDialog::getOpenFileName(&dialog, tr("Add preview cubemap"), {}, tr("Preview environments (*.dds *.exr *.hdr *.png *.jpg);;OpenEXR environment (*.exr);;DDS cubemap (*.dds);;All files (*.*)"));
        if (path.isEmpty()) return;
        bool ok = false;
        const QString suggested = QFileInfo(path).completeBaseName();
        const QString name = QInputDialog::getText(&dialog, tr("Cubemap name"), tr("Name"), QLineEdit::Normal, suggested, &ok).trimmed();
        if (!ok || name.isEmpty()) return;
        auto* item = new QListWidgetItem(QStringLiteral("%1 — %2").arg(name, QDir::toNativeSeparators(path)), customList);
        item->setData(Qt::UserRole, name); item->setData(Qt::UserRole + 1, QDir::toNativeSeparators(path));
    });
    connect(remove, &QPushButton::clicked, &dialog, [customList] { delete customList->takeItem(customList->currentRow()); });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;

    QStringList savedNames, savedPaths;
    for (int i = 0; i < customList->count(); ++i) {
        savedNames.append(customList->item(i)->data(Qt::UserRole).toString());
        savedPaths.append(customList->item(i)->data(Qt::UserRole + 1).toString());
    }
    settings.setValue(QStringLiteral("PreviewCubemaps/defaultPath"), defaultPath->text().trimmed());
    settings.setValue(QStringLiteral("PreviewCubemaps/names"), savedNames);
    settings.setValue(QStringLiteral("PreviewCubemaps/paths"), savedPaths);
    appSettings.setValue(QStringLiteral("Interface/autosaveEnabled"), autosaveEnabled->isChecked());
    appSettings.setValue(QStringLiteral("Interface/autosaveMinutes"), autosaveMinutes->value());
    appSettings.setValue(QStringLiteral("Interface/jsonSidecar"), jsonSidecar->isChecked());
    appSettings.setValue(QStringLiteral("Interface/experimentalConverter"), experimentalConverter->isChecked());
    setConverterAvailable(experimentalConverter->isChecked());
    applyAutosaveSettings();
    m_preview->reloadPreviewCubemaps();
}

void MainWindow::showPathRepairTool()
{
    struct BrokenEntry { QString uvSet; QString key; QString path; };
    const auto collectBroken = [this] {
        QVector<BrokenEntry> broken;
        const auto scan = [this, &broken](const QString& setName, const QJsonObject& set) {
            for (auto it = set.constBegin(); it != set.constEnd(); ++it) {
                const auto state = Pbr::TextureSlotState::fromJson(it.value().toObject());
                if (state.path.trimmed().isEmpty()) continue;
                if (TextureImage::resolvePath(state.path, m_textureRoot).isEmpty())
                    broken.append({setName, it.key(), state.path});
            }
        };
        for (const QString& set : {QStringLiteral("UV0"), QStringLiteral("UV1"), QStringLiteral("UV2")})
            scan(set, m_document.textureSet(set));
        const QJsonObject resources = m_document.toJson().value(QStringLiteral("resources")).toObject();
        for (auto it = resources.constBegin(); it != resources.constEnd(); ++it)
            scan(it.key(), it.value().toObject());
        return broken;
    };

    const QVector<BrokenEntry> broken = collectBroken();
    if (broken.isEmpty()) {
        QMessageBox::information(this, tr("Repair Texture Paths"),
            tr("Every authored texture path resolves against the configured sources."));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Repair Texture Paths"));
    dialog.resize(860, 420);
    auto* outer = new QVBoxLayout(&dialog);
    auto* intro = new QLabel(tr("%1 texture path(s) no longer resolve. Propose fixes by filename search or a prefix rewrite, then apply them as one undoable change.").arg(broken.size()), &dialog);
    intro->setWordWrap(true);
    outer->addWidget(intro);

    auto* table = new QTreeWidget(&dialog);
    table->setHeaderLabels({tr("Input"), tr("Authored path"), tr("Proposed fix")});
    table->setRootIsDecorated(false);
    table->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    for (const BrokenEntry& entry : broken) {
        auto* item = new QTreeWidgetItem(table);
        item->setText(0, entry.uvSet + QStringLiteral(" · ") + entry.key);
        item->setText(1, entry.path);
        item->setData(0, Qt::UserRole, entry.uvSet);
        item->setData(0, Qt::UserRole + 1, entry.key);
    }
    outer->addWidget(table, 1);

    auto* actions = new QHBoxLayout();
    auto* findByName = new QPushButton(tr("Find by Filename"), &dialog);
    findByName->setToolTip(tr("Searches every configured loose folder and BA2 archive for files with the same name."));
    auto* replacePrefix = new QPushButton(tr("Replace Prefix…"), &dialog);
    replacePrefix->setToolTip(tr("Rewrites the beginning of each broken path, e.g. after renaming a mod folder."));
    actions->addWidget(findByName);
    actions->addWidget(replacePrefix);
    actions->addStretch();
    outer->addLayout(actions);
    auto* buttons = new QDialogButtonBox(&dialog);
    auto* applyButton = buttons->addButton(tr("Apply Fixes"), QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Cancel);
    applyButton->setEnabled(false);
    outer->addWidget(buttons);

    const auto refreshApply = [table, applyButton] {
        bool any = false;
        for (int i = 0; i < table->topLevelItemCount(); ++i)
            if (!table->topLevelItem(i)->text(2).isEmpty()) { any = true; break; }
        applyButton->setEnabled(any);
    };
    connect(findByName, &QPushButton::clicked, &dialog, [this, table, refreshApply] {
        const QVector<TextureImage::Resource> resources = TextureImage::enumerate(m_textureRoot);
        for (int i = 0; i < table->topLevelItemCount(); ++i) {
            QTreeWidgetItem* item = table->topLevelItem(i);
            const QString fileName = QFileInfo(item->text(1)).fileName();
            for (const TextureImage::Resource& resource : resources) {
                if (QFileInfo(resource.path).fileName().compare(fileName, Qt::CaseInsensitive) == 0) {
                    item->setText(2, resource.path);
                    break;
                }
            }
        }
        refreshApply();
    });
    connect(replacePrefix, &QPushButton::clicked, &dialog, [this, table, refreshApply, &dialog] {
        bool ok = false;
        const QString from = QInputDialog::getText(&dialog, tr("Replace Prefix"), tr("Path prefix to replace"), QLineEdit::Normal, QString(), &ok).trimmed();
        if (!ok || from.isEmpty()) return;
        const QString to = QInputDialog::getText(&dialog, tr("Replace Prefix"), tr("Replacement prefix"), QLineEdit::Normal, QString(), &ok).trimmed();
        if (!ok) return;
        for (int i = 0; i < table->topLevelItemCount(); ++i) {
            QTreeWidgetItem* item = table->topLevelItem(i);
            const QString path = item->text(1);
            if (!path.startsWith(from, Qt::CaseInsensitive)) continue;
            const QString candidate = to + path.mid(from.size());
            if (!TextureImage::resolvePath(candidate, m_textureRoot).isEmpty())
                item->setText(2, candidate);
        }
        refreshApply();
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;

    QVector<std::tuple<QString, QString, QString>> fixes;
    for (int i = 0; i < table->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = table->topLevelItem(i);
        if (item->text(2).isEmpty()) continue;
        fixes.append({item->data(0, Qt::UserRole).toString(), item->data(0, Qt::UserRole + 1).toString(), item->text(2)});
    }
    if (fixes.isEmpty()) return;
    applyDocumentChange(tr("Repair %1 texture path(s)").arg(fixes.size()), [this, fixes] {
        for (const auto& [uvSet, key, newPath] : fixes) {
            Pbr::TextureSlotState state = m_document.textureSlot(uvSet, key);
            state.path = newPath;
            m_document.setTextureSlot(uvSet, key, state);
        }
    });
    rebuildWorkspace(m_currentPage);
    statusBar()->showMessage(tr("Repaired %1 texture path(s)").arg(fixes.size()), 4000);
}

void MainWindow::showBatchOperations()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Batch Operations"));
    dialog.resize(820, 520);
    auto* outer = new QVBoxLayout(&dialog);
    auto* intro = new QLabel(tr("Applies one operation to every .pbrm under a folder. Dry Run reports what would change; Apply saves atomically. Autosave sidecars are never touched."), &dialog);
    intro->setWordWrap(true);
    outer->addWidget(intro);

    auto* form = new QFormLayout();
    auto* folderRow = new QWidget(&dialog);
    auto* folderLayout = new QHBoxLayout(folderRow);
    folderLayout->setContentsMargins(0, 0, 0, 0);
    auto* folderEdit = new QLineEdit(m_recentDirectory, folderRow);
    auto* folderBrowse = new QPushButton(tr("Browse…"), folderRow);
    folderLayout->addWidget(folderEdit, 1);
    folderLayout->addWidget(folderBrowse);
    form->addRow(tr("Folder"), folderRow);
    auto* operation = new QComboBox(&dialog);
    operation->addItem(tr("Repair broken texture paths (by filename)"));
    operation->addItem(tr("Set a setting key"));
    operation->addItem(tr("Re-save (normalize legacy fields)"));
    operation->addItem(tr("Extract legacy surface states to definitions JSON"));
    operation->addItem(tr("Strip legacy surface state data"));
    form->addRow(tr("Operation"), operation);
    auto* sectionEdit = new QLineEdit(&dialog);
    sectionEdit->setPlaceholderText(tr("e.g. Shading/Core"));
    auto* keyEdit = new QLineEdit(&dialog);
    keyEdit->setPlaceholderText(tr("e.g. specularAa"));
    auto* valueEdit = new QLineEdit(&dialog);
    valueEdit->setPlaceholderText(tr("true, false, a number, or text"));
    form->addRow(tr("Section"), sectionEdit);
    form->addRow(tr("Key"), keyEdit);
    form->addRow(tr("Value"), valueEdit);
    outer->addLayout(form);
    auto* report = new QPlainTextEdit(&dialog);
    report->setReadOnly(true);
    outer->addWidget(report, 1);
    auto* buttons = new QDialogButtonBox(&dialog);
    auto* dryRunButton = buttons->addButton(tr("Dry Run"), QDialogButtonBox::ActionRole);
    auto* applyButton = buttons->addButton(tr("Apply"), QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Close);
    outer->addWidget(buttons);

    const auto updateFieldState = [operation, sectionEdit, keyEdit, valueEdit] {
        const bool settingOp = operation->currentIndex() == 1;
        sectionEdit->setEnabled(settingOp);
        keyEdit->setEnabled(settingOp);
        valueEdit->setEnabled(settingOp);
    };
    updateFieldState();
    connect(operation, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, [updateFieldState](int) { updateFieldState(); });
    connect(folderBrowse, &QPushButton::clicked, &dialog, [&dialog, folderEdit] {
        const QString folder = QFileDialog::getExistingDirectory(&dialog, tr("Choose material folder"), folderEdit->text());
        if (!folder.isEmpty()) folderEdit->setText(folder);
    });

    const auto runBatch = [this, folderEdit, operation, sectionEdit, keyEdit, valueEdit, report](bool apply) {
        report->clear();
        QStringList files;
        QDirIterator it(folderEdit->text().trimmed(), {QStringLiteral("*.pbrm")}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) files.append(it.next());
        if (files.isEmpty()) { report->appendPlainText(tr("No .pbrm files found.")); return; }

        QVector<TextureImage::Resource> resources;
        if (operation->currentIndex() == 0) resources = TextureImage::enumerate(m_textureRoot);
        QJsonValue settingValue;
        if (operation->currentIndex() == 1) {
            const QString raw = valueEdit->text().trimmed();
            bool isNumber = false;
            const double number = raw.toDouble(&isNumber);
            if (raw.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0) settingValue = true;
            else if (raw.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0) settingValue = false;
            else if (isNumber) settingValue = number;
            else settingValue = raw;
            if (sectionEdit->text().trimmed().isEmpty() || keyEdit->text().trimmed().isEmpty()) {
                report->appendPlainText(tr("Section and key are required for the setting operation."));
                return;
            }
        }

        int changed = 0, failed = 0;
        for (const QString& file : files) {
            Pbr::MaterialDocument document;
            QString error;
            if (!document.load(file, &error)) {
                report->appendPlainText(tr("%1: load failed: %2").arg(file, error));
                ++failed;
                continue;
            }
            const QJsonObject before = document.toJson();
            QStringList notes;
            if (operation->currentIndex() == 0) {
                for (const QString& setName : {QStringLiteral("UV0"), QStringLiteral("UV1"), QStringLiteral("UV2")}) {
                    const QJsonObject set = document.textureSet(setName);
                    for (auto slotIt = set.constBegin(); slotIt != set.constEnd(); ++slotIt) {
                        auto state = Pbr::TextureSlotState::fromJson(slotIt.value().toObject());
                        if (state.path.trimmed().isEmpty()) continue;
                        if (!TextureImage::resolvePath(state.path, m_textureRoot).isEmpty()) continue;
                        const QString fileName = QFileInfo(state.path).fileName();
                        for (const TextureImage::Resource& resource : resources) {
                            if (QFileInfo(resource.path).fileName().compare(fileName, Qt::CaseInsensitive) != 0) continue;
                            notes << tr("%1/%2: %3 -> %4").arg(setName, slotIt.key(), state.path, resource.path);
                            state.path = resource.path;
                            document.setTextureSlot(setName, slotIt.key(), state);
                            break;
                        }
                    }
                }
            } else if (operation->currentIndex() == 1) {
                document.setSetting(sectionEdit->text().trimmed(), keyEdit->text().trimmed(), settingValue);
            } else if (operation->currentIndex() == 3) {
                // Extract: materialize this material's legacy per-material
                // states as a standalone definitions JSON beside the file, so
                // the authored tuning survives the strip.
                if (Pbr::hasLegacySurfaceStateGroups(document)) {
                    const QFileInfo info(file);
                    const QString target = info.absolutePath() + QLatin1Char('/') + info.completeBaseName() + QStringLiteral(".SurfaceStates.json");
                    const int stateCount = Pbr::resolveSurfaceStates(document).size();
                    if (apply) {
                        if (Pbr::extractLegacyStates(document, target, &error))
                            notes << tr("extracted %1 state(s) to %2").arg(stateCount).arg(target);
                        else
                            notes << tr("extraction FAILED: %1").arg(error);
                    } else {
                        notes << tr("would extract %1 state(s) to %2").arg(stateCount).arg(target);
                    }
                }
            } else if (operation->currentIndex() == 4) {
                // Strip: remove legacy state groups, retired response slots,
                // and retired layer-mask keys. Constants were already folded
                // into their v5 homes by the load-time migration.
                const int removed = Pbr::stripLegacySurfaceData(document);
                if (removed > 0) notes << tr("stripped %1 legacy surface item(s)").arg(removed);
            }
            const bool wouldChange = document.toJson() != before;
            // Extraction writes a sidecar JSON, never the material itself.
            const bool reportOnly = operation->currentIndex() == 3;
            if (reportOnly && notes.isEmpty()) continue;
            if (!reportOnly && !wouldChange && operation->currentIndex() != 2) continue;
            ++changed;
            if (apply && !reportOnly) {
                if (!document.save(file, &error)) {
                    report->appendPlainText(tr("%1: save failed: %2").arg(file, error));
                    ++failed; --changed;
                    continue;
                }
            }
            report->appendPlainText((apply ? tr("%1: updated") : tr("%1: would update")).arg(file)
                + (notes.isEmpty() ? QString() : QStringLiteral("\n  ") + notes.join(QStringLiteral("\n  "))));
        }
        report->appendPlainText(tr("— %1 file(s) scanned, %2 %3, %4 failed")
            .arg(files.size()).arg(changed).arg(apply ? tr("updated") : tr("would update")).arg(failed));
        if (apply && m_document.fileName().isEmpty() == false && files.contains(QDir::toNativeSeparators(m_document.fileName())))
            openFile(m_document.fileName());
    };
    connect(dryRunButton, &QPushButton::clicked, &dialog, [runBatch] { runBatch(false); });
    connect(applyButton, &QPushButton::clicked, &dialog, [runBatch] { runBatch(true); });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}

void MainWindow::applyAutosaveSettings()
{
    QSettings settings;
    const bool enabled = settings.value(QStringLiteral("Interface/autosaveEnabled"), false).toBool();
    const int minutes = qBound(1, settings.value(QStringLiteral("Interface/autosaveMinutes"), 3).toInt(), 60);
    if (enabled) {
        m_autosaveTimer->start(minutes * 60 * 1000);
    } else {
        m_autosaveTimer->stop();
    }
}

void MainWindow::performAutosave()
{
    // Recovery snapshot beside the material; never touches the real file.
    if (!m_document.modified() || m_document.fileName().isEmpty()) return;
    const QString sidecar = autosaveFileName(m_document.fileName());
    QString error;
    if (m_document.save(sidecar, &error))
        statusBar()->showMessage(tr("Autosaved recovery snapshot"), 2000);
}

void MainWindow::setAdvancedInterface(bool enabled)
{
    if (m_advancedInterface == enabled) return;
    m_advancedInterface = enabled;
    QSettings().setValue(QStringLiteral("Interface/advanced"), enabled);
    const QString page = (!enabled && (m_currentPage == QStringLiteral("Advanced") || m_currentPage == QStringLiteral("Dynamic Normals")))
        ? QStringLiteral("Overview") : m_currentPage;
    rebuildWorkspace(page);
}

void MainWindow::setCurrentTextureCardsExpanded(bool expanded)
{
    QWidget* page = m_pages->currentWidget();
    if (!page) return;
    const auto cards = page->findChildren<TextureSlotWidget*>();
    for (TextureSlotWidget* card : cards) card->setExpanded(expanded);
    statusBar()->showMessage(expanded ? tr("Texture cards expanded") : tr("Texture cards collapsed"), 1800);
}

void MainWindow::rebuildWorkspace(const QString& preferredPage)
{
    QSignalBlocker shaderBlock(m_shaderCombo);
    m_shaderCombo->setCurrentText(Pbr::shaderModeName(m_document.shaderMode()));
    m_shaderCombo->setToolTip(Pbr::shaderModeDescription(m_document.shaderMode()));

    // Rebuilding tears every page down; remember each page's scroll offset so
    // toggles and undo steps do not bounce the user back to the top.
    QHash<QString, int> scrollOffsets;
    for (auto it = m_pageByName.cbegin(); it != m_pageByName.cend(); ++it)
        if (auto* scroll = it.value()->findChild<QScrollArea*>())
            scrollOffsets.insert(it.key(), scroll->verticalScrollBar()->value());

    while (m_pages->count() > 0) {
        QWidget* page = m_pages->widget(0);
        m_pages->removeWidget(page);
        page->deleteLater();
    }
    m_pageByName.clear();

    const auto addPage = [this](const QString& name, QWidget* page) {
        m_pageByName.insert(name, page);
        m_pages->addWidget(page);
    };
    addPage(QStringLiteral("Overview"), createOverviewPage());
    const Pbr::ShaderMode mode = m_document.shaderMode();
    const bool landscape = mode == Pbr::ShaderMode::LandscapeSurface;
    if (!landscape) {
        addPage(QStringLiteral("Primary UV Textures"), createUvPage(QStringLiteral("UV0")));
        addPage(QStringLiteral("Secondary UV Textures"), createUvPage(QStringLiteral("UV1")));
        addPage(QStringLiteral("Tertiary UV Textures"), createUvPage(QStringLiteral("UV2")));
        addPage(QStringLiteral("UV Mapping"), createUvMappingPage());
    }
    addPage(QStringLiteral("Shading"), createShadingPage());

    const bool all = mode == Pbr::ShaderMode::AllFeatures;
    if (mode == Pbr::ShaderMode::VegetationSurface || all) addPage(QStringLiteral("Vegetation"), createVegetationPage());
    if (mode == Pbr::ShaderMode::StandardSurface || mode == Pbr::ShaderMode::LandscapeSurface || all)
        addPage(QStringLiteral("Height & Geometry"), createGeometryPage());
    if (mode == Pbr::ShaderMode::UnlitEffect || mode == Pbr::ShaderMode::SimpleLitEffect || mode == Pbr::ShaderMode::PhysicallyLitEffect || all)
        addPage(QStringLiteral("Particles"), createParticlesPage());
    if (mode == Pbr::ShaderMode::StandardSurface || mode == Pbr::ShaderMode::AdvancedSubsurface || mode == Pbr::ShaderMode::VegetationSurface || all)
        addPage(QStringLiteral("Surface Response"), createSurfaceResponsePage());
    if (mode == Pbr::ShaderMode::StandardSurface || mode == Pbr::ShaderMode::AdvancedSubsurface || all) addPage(QStringLiteral("Dynamic Normals"), createDynamicNormalsPage());
    if (mode == Pbr::ShaderMode::StandardSurface || all) addPage(QStringLiteral("Decals"), createDecalsPage());
    if (landscape || all) addPage(QStringLiteral("Landscape"), createLandscapePage());
    addPage(QStringLiteral("Transparency"), createTransparencyPage());
    if (mode != Pbr::ShaderMode::UnlitEffect) addPage(QStringLiteral("Environment"), createEnvironmentPage());
    addPage(QStringLiteral("Advanced"), createAdvancedPage());
    rebuildNavigation();
    refreshSearchIndex();
    const QString fallback = landscape ? QStringLiteral("Landscape") : QStringLiteral("Overview");
    showPage(m_pageByName.contains(preferredPage) ? preferredPage : fallback);
    filterCurrentPage(m_search->text());
    m_preview->setMaterial(m_document);
    updateTextureWatcher();
    for (auto it = scrollOffsets.cbegin(); it != scrollOffsets.cend(); ++it) {
        QWidget* page = m_pageByName.value(it.key(), nullptr);
        if (!page) continue;
        if (auto* scroll = page->findChild<QScrollArea*>()) {
            // Restore after the layout settles so ranges are final.
            const int offset = it.value();
            QTimer::singleShot(0, scroll, [scroll, offset] { scroll->verticalScrollBar()->setValue(offset); });
        }
    }
}

void MainWindow::updateTextureWatcher()
{
    QSet<QString> wanted;
    const auto collect = [this, &wanted](const QJsonObject& set) {
        for (auto it = set.constBegin(); it != set.constEnd(); ++it) {
            const auto state = Pbr::TextureSlotState::fromJson(it.value().toObject());
            if (state.path.trimmed().isEmpty()) continue;
            const QString resolved = TextureImage::resolvePath(state.path, m_textureRoot);
            // Only loose files can change under us; BA2 entries resolve elsewhere.
            if (!resolved.isEmpty() && QFileInfo::exists(resolved))
                wanted.insert(QDir::cleanPath(resolved));
        }
    };
    for (const QString& set : {QStringLiteral("UV0"), QStringLiteral("UV1"), QStringLiteral("UV2")})
        collect(m_document.textureSet(set));
    const QJsonObject resources = m_document.toJson().value(QStringLiteral("resources")).toObject();
    for (auto it = resources.constBegin(); it != resources.constEnd(); ++it)
        collect(it.value().toObject());

    const QStringList watched = m_textureWatcher->files();
    if (!watched.isEmpty()) m_textureWatcher->removePaths(watched);
    if (!wanted.isEmpty()) m_textureWatcher->addPaths(QStringList(wanted.cbegin(), wanted.cend()));
}

void MainWindow::handleWatchedTextureChanged(const QString& absolutePath)
{
    m_pendingTextureReloads.insert(absolutePath);
    // Replace-by-rename drops the watch; re-arm once the new file exists.
    if (QFileInfo::exists(absolutePath) && !m_textureWatcher->files().contains(absolutePath))
        m_textureWatcher->addPath(absolutePath);
    m_textureReloadTimer->start();
}

void MainWindow::rebuildNavigation()
{
    QSignalBlocker navigationBlock(m_navigation);
    m_navigation->clear();
    addNavigationSection(tr("WORKSPACE"));
    addNavigationPage(QStringLiteral("Overview"));
    if (m_pageByName.contains(QStringLiteral("Primary UV Textures"))) {
        addNavigationSection(tr("TEXTURES"));
        addNavigationPage(QStringLiteral("Primary UV Textures"));
        addUvLayerToggleItem(QStringLiteral("Secondary"), m_document.secondaryUvEnabled(),
            [this](bool checked) { m_document.setSecondaryUvEnabled(checked); });
        addUvLayerToggleItem(QStringLiteral("Tertiary"), m_document.tertiaryUvEnabled(),
            [this](bool checked) { m_document.setTertiaryUvEnabled(checked); });
        addNavigationPage(QStringLiteral("UV Mapping"));
    }
    addNavigationSection(tr("MATERIAL"));
    addNavigationPage(QStringLiteral("Shading"));
    for (const QString& name : {QStringLiteral("Particles"), QStringLiteral("Surface Response"), QStringLiteral("Vegetation"), QStringLiteral("Landscape")})
        if (m_pageByName.contains(name)) addNavigationPage(name);
    addNavigationPage(QStringLiteral("Transparency"));
    if (m_pageByName.contains(QStringLiteral("Environment"))) addNavigationPage(QStringLiteral("Environment"));
    if (m_pageByName.contains(QStringLiteral("Height & Geometry")) || m_pageByName.contains(QStringLiteral("Decals"))) {
        addNavigationSection(tr("GEOMETRY & LAYERING"));
        if (m_pageByName.contains(QStringLiteral("Height & Geometry"))) addNavigationPage(QStringLiteral("Height & Geometry"));
        if (m_pageByName.contains(QStringLiteral("Decals"))) addNavigationPage(QStringLiteral("Decals"));
    }
    if (m_advancedInterface) {
        addNavigationSection(tr("ADVANCED"));
        if (m_pageByName.contains(QStringLiteral("Dynamic Normals"))) addNavigationPage(QStringLiteral("Dynamic Normals"));
        addNavigationPage(QStringLiteral("Advanced"));
    }
}

void MainWindow::addUvLayerToggleItem(const QString& layerName, bool layerEnabled, const std::function<void(bool)>& setLayerEnabled)
{
    const QString pageName = layerName + QStringLiteral(" UV Textures");
    auto* item = new QListWidgetItem(pageName, m_navigation);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    item->setData(Qt::UserRole, true);
    item->setData(Qt::UserRole + 2, pageName);
    item->setData(Qt::UserRole + 4, m_navigationSection);
    item->setHidden(QSettings().value(QStringLiteral("Interface/collapsedNavigationSections")).toStringList().contains(m_navigationSection));
    item->setToolTip(layerEnabled
        ? tr("%1 UV is enabled and will be stored as an active material layer.").arg(layerName)
        : tr("%1 UV is disabled. Check this box to activate it; authored settings remain accessible.").arg(layerName));

    auto* row = new QWidget(m_navigation);
    row->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(7, 0, 5, 0);
    rowLayout->setSpacing(7);
    auto* enabled = new QCheckBox(row);
    enabled->setChecked(layerEnabled);
    enabled->setToolTip(item->toolTip());
    auto* label = new QLabel(pageName, row);
    label->setStyleSheet(layerEnabled
        ? QStringLiteral("color: #e6e8eb; background: transparent;")
        : QStringLiteral("color: #777c84; background: transparent;"));
    rowLayout->addWidget(enabled);
    rowLayout->addWidget(label, 1);
    item->setSizeHint(QSize(0, 28));
    m_navigation->setItemWidget(item, row);
    item->setText(QString());
    connect(enabled, &QCheckBox::toggled, this, [this, item, label, layerName, pageName, setLayerEnabled](bool checked) {
        applyDocumentChange(tr("Toggle %1 UV").arg(layerName), [setLayerEnabled, checked] { setLayerEnabled(checked); });
        label->setStyleSheet(checked
            ? QStringLiteral("color: #e6e8eb; background: transparent;")
            : QStringLiteral("color: #777c84; background: transparent;"));
        item->setToolTip(checked
            ? tr("%1 UV is enabled and will be stored as an active material layer.").arg(layerName)
            : tr("%1 UV is disabled. Its authored settings are preserved but the layer is inactive.").arg(layerName));
        updateWindowTitle();
        QTimer::singleShot(0, this, [this, pageName] { rebuildWorkspace(pageName); });
    });
}

void MainWindow::addNavigationSection(const QString& title)
{
    m_navigationSection = title;
    const QStringList collapsed = QSettings().value(QStringLiteral("Interface/collapsedNavigationSections")).toStringList();
    const bool isCollapsed = collapsed.contains(title);
    auto* item = new QListWidgetItem(title, m_navigation);
    item->setIcon(disclosureArrowIcon(!isCollapsed, 12, QColor(168, 174, 181)));
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    item->setData(Qt::UserRole + 3, true);
    item->setData(Qt::UserRole + 4, title);
    item->setToolTip(tr("Click to expand or collapse this sidebar group."));
    item->setForeground(QColor(168, 174, 181));
    QFont font = item->font(); font.setBold(true); font.setPointSizeF(8.0); item->setFont(font);
}

void MainWindow::addNavigationPage(const QString& title)
{
    QString display = title;
    auto* item = new QListWidgetItem(display, m_navigation);
    item->setData(Qt::UserRole, true);
    item->setData(Qt::UserRole + 2, title);
    item->setData(Qt::UserRole + 4, m_navigationSection);
    item->setToolTip(tr("Open the %1 settings page.").arg(title));
    const QStringList collapsed = QSettings().value(QStringLiteral("Interface/collapsedNavigationSections")).toStringList();
    item->setHidden(collapsed.contains(m_navigationSection));
    if (title == QStringLiteral("Overview")) {
        const int severity = validationSeverity();
        if (severity > 0) {
            item->setText(title + (severity > 1 ? QStringLiteral("   ●") : QStringLiteral("   ●")));
            item->setForeground(severity > 1 ? QColor(255, 132, 132) : QColor(255, 181, 74));
            item->setToolTip(severity > 1 ? tr("Open Overview — validation has blocking errors.") : tr("Open Overview — validation has warnings."));
        }
    }
}

void MainWindow::toggleNavigationSection(QListWidgetItem* item)
{
    const QString section = item->data(Qt::UserRole + 4).toString();
    QStringList collapsed = QSettings().value(QStringLiteral("Interface/collapsedNavigationSections")).toStringList();
    if (collapsed.contains(section)) collapsed.removeAll(section); else collapsed.append(section);
    QSettings().setValue(QStringLiteral("Interface/collapsedNavigationSections"), collapsed);
    rebuildNavigation();
    showPage(m_currentPage);
}

int MainWindow::validationSeverity() const
{
    int severity = (TextureImage::searchRoots(m_textureRoot).isEmpty() && TextureImage::archivePaths().isEmpty()) ? 1 : 0;
    const auto inspectSet = [this, &severity](const QJsonObject& set) {
        for (auto it = set.constBegin(); it != set.constEnd(); ++it) {
            const auto state = Pbr::TextureSlotState::fromJson(it.value().toObject());
            if (state.enabled && !state.path.trimmed().isEmpty() && TextureImage::resolvePath(state.path, m_textureRoot).isEmpty()) severity = 2;
        }
    };
    inspectSet(m_document.textureSet(QStringLiteral("UV0")));
    if (m_document.secondaryUvEnabled()) inspectSet(m_document.textureSet(QStringLiteral("UV1")));
    if (m_document.tertiaryUvEnabled()) inspectSet(m_document.textureSet(QStringLiteral("UV2")));
    const QJsonObject resources = m_document.toJson().value(QStringLiteral("resources")).toObject();
    for (auto it = resources.constBegin(); it != resources.constEnd(); ++it) inspectSet(it.value().toObject());
    return severity;
}

void MainWindow::showPage(const QString& title)
{
    QWidget* page = m_pageByName.value(title, nullptr);
    if (!page) return;
    m_currentPage = title;
    m_pages->setCurrentWidget(page);
    for (int i = 0; i < m_navigation->count(); ++i) {
        QListWidgetItem* item = m_navigation->item(i);
        const QString pageName = item->data(Qt::UserRole + 2).toString().isEmpty()
            ? item->text() : item->data(Qt::UserRole + 2).toString();
        if (item->data(Qt::UserRole).toBool() && pageName == title) {
            m_navigation->setCurrentItem(item);
            break;
        }
    }
    filterCurrentPage(m_search->text());
}

void MainWindow::filterCurrentPage(const QString& text)
{
    QWidget* page = m_pages->currentWidget();
    if (!page) return;
    const QString query = text.trimmed();
    for (QFrame* card : page->findChildren<QFrame*>(QStringLiteral("Card"), Qt::FindChildrenRecursively)) {
        const QString searchable = card->property("searchText").toString() + QStringLiteral(" ") + card->toolTip();
        card->setVisible(query.isEmpty() || searchable.contains(query, Qt::CaseInsensitive));
    }
}

void MainWindow::refreshSearchIndex()
{
    m_searchTargets.clear();
    QStringList entries;
    for (auto pageIt = m_pageByName.cbegin(); pageIt != m_pageByName.cend(); ++pageIt) {
        const QString pageName = pageIt.key();
        const auto cards = pageIt.value()->findChildren<QFrame*>(QStringLiteral("Card"), Qt::FindChildrenRecursively);
        for (QFrame* card : cards) {
            QString title;
            const auto headings = card->findChildren<QLabel*>(QStringLiteral("SectionTitle"), Qt::FindChildrenRecursively);
            if (!headings.isEmpty()) title = headings.first()->text();
            if (title.isEmpty()) title = pageName;
            QString display = pageName + QStringLiteral(" › ") + title;
            int suffix = 2;
            while (m_searchTargets.contains(display))
                display = pageName + QStringLiteral(" › ") + title + QStringLiteral(" (%1)").arg(suffix++);
            const QString query = title + QStringLiteral(" ") + card->property("searchText").toString();
            m_searchTargets.insert(display, qMakePair(pageName, query));
            entries.append(display);
        }
    }
    entries.sort(Qt::CaseInsensitive);
    m_searchCompleter->setModel(new QStringListModel(entries, m_searchCompleter));
}

void MainWindow::openSearchResult(const QString& displayText)
{
    const auto target = m_searchTargets.value(displayText);
    if (target.first.isEmpty()) return;
    showPage(target.first);
    const QString focused = displayText.section(QStringLiteral(" › "), 1, 1).section(QStringLiteral(" ("), 0, 0);
    m_search->setText(focused);
    m_search->selectAll();
}

void MainWindow::applyDocumentChange(const QString& description, const std::function<void()>& mutation)
{
    const Pbr::MaterialDocument before = m_document;
    mutation();
    const Pbr::MaterialDocument after = m_document;
    if (before.toJson() == after.toJson()) return;
    m_undoStack->push(new DocumentCommand(description, before, after,
        [this](const Pbr::MaterialDocument& document) { restoreDocument(document); }));
    m_preview->setMaterial(m_document);
    updateWindowTitle();
}

void MainWindow::restoreDocument(const Pbr::MaterialDocument& document)
{
    const QString page = m_currentPage;
    m_document = document;
    rebuildWorkspace(page);
    updateWindowTitle();
}

void MainWindow::copyUvLayer(const QString& uvSet)
{
    QJsonObject settings;
    settings.insert(QStringLiteral("coordinates"), m_document.settingGroup(uvSet));
    settings.insert(QStringLiteral("animation"), m_document.settingGroup(uvSet + QStringLiteral("/Animation")));
    if (uvSet == QStringLiteral("UV1") || uvSet == QStringLiteral("UV2"))
        settings.insert(QStringLiteral("composition"), m_document.settingGroup(uvSet + QStringLiteral("/Composition")));
    QJsonObject payload{
        {QStringLiteral("schema"), QStringLiteral("FO4.PBRM.UVLayer")},
        {QStringLiteral("source"), uvSet},
        {QStringLiteral("textures"), m_document.textureSet(uvSet)},
        {QStringLiteral("settings"), settings}
    };
    const QByteArray json = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    auto* mime = new QMimeData();
    mime->setData(QStringLiteral("application/x-fo4-pbrm-uv-layer"), json);
    mime->setText(QString::fromUtf8(json));
    QApplication::clipboard()->setMimeData(mime);
    statusBar()->showMessage(tr("%1 copied").arg(uvSet), 2500);
}

void MainWindow::pasteUvLayer(const QString& uvSet)
{
    const QMimeData* mime = QApplication::clipboard()->mimeData();
    const QByteArray json = mime->hasFormat(QStringLiteral("application/x-fo4-pbrm-uv-layer"))
        ? mime->data(QStringLiteral("application/x-fo4-pbrm-uv-layer")) : mime->text().toUtf8();
    const QJsonDocument parsed = QJsonDocument::fromJson(json);
    const QJsonObject payload = parsed.object();
    if (!parsed.isObject() || payload.value(QStringLiteral("schema")).toString() != QStringLiteral("FO4.PBRM.UVLayer")) {
        QMessageBox::warning(this, tr("Paste UV Layer"), tr("The clipboard does not contain a PBRM UV layer."));
        return;
    }
    const QString source = payload.value(QStringLiteral("source")).toString();
    const auto layerPrefix = [](const QString& set) {
        if (set == QStringLiteral("UV1")) return QStringLiteral("secondary");
        if (set == QStringLiteral("UV2")) return QStringLiteral("tertiary");
        return QStringLiteral("primary");
    };
    const QString targetPrefix = layerPrefix(uvSet);
    QJsonObject remapped;
    const QJsonObject textures = payload.value(QStringLiteral("textures")).toObject();
    for (auto it = textures.begin(); it != textures.end(); ++it) {
        QString key = it.key();
        if (source != uvSet) {
            for (const QString& fromPrefix : {QStringLiteral("primary"), QStringLiteral("secondary"), QStringLiteral("tertiary")}) {
                if (fromPrefix != targetPrefix && key.startsWith(fromPrefix)) {
                    key.replace(0, fromPrefix.size(), targetPrefix);
                    break;
                }
            }
        }
        remapped.insert(key, it.value());
    }
    const QJsonObject settings = payload.value(QStringLiteral("settings")).toObject();
    applyDocumentChange(tr("Paste %1 layer").arg(uvSet), [this, uvSet, remapped, settings] {
        m_document.setTextureSet(uvSet, remapped);
        m_document.setSettingGroup(uvSet, settings.value(QStringLiteral("coordinates")).toObject());
        m_document.setSettingGroup(uvSet + QStringLiteral("/Animation"), settings.value(QStringLiteral("animation")).toObject());
        if (uvSet == QStringLiteral("UV1") || uvSet == QStringLiteral("UV2"))
            m_document.setSettingGroup(uvSet + QStringLiteral("/Composition"), settings.value(QStringLiteral("composition")).toObject());
    });
    rebuildWorkspace(uvLayerPageName(uvSet));
}

void MainWindow::resetUvLayer(const QString& uvSet)
{
    if (QMessageBox::question(this, tr("Reset UV Layer"), tr("Reset every texture and setting in %1?").arg(uvSet),
        QMessageBox::Reset | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Reset) return;
    applyDocumentChange(tr("Reset %1 layer").arg(uvSet), [this, uvSet] {
        m_document.clearTextureSet(uvSet);
        m_document.clearSettingGroup(uvSet);
        m_document.clearSettingGroup(uvSet + QStringLiteral("/Animation"));
        if (uvSet == QStringLiteral("UV1") || uvSet == QStringLiteral("UV2"))
            m_document.clearSettingGroup(uvSet + QStringLiteral("/Composition"));
    });
    rebuildWorkspace(uvLayerPageName(uvSet));
}

void MainWindow::newDocument()
{
    if (!maybeSave()) return;
    m_document.reset();
    m_undoStack->clear();
    m_undoStack->setClean();
    rebuildWorkspace();
    m_preview->showFullMaterial();
    updateWindowTitle();
    QSettings().remove(QStringLiteral("lastFile"));
}

void MainWindow::openDocument()
{
    if (!maybeSave()) return;
    const QString fileName = QFileDialog::getOpenFileName(this, tr("Open PBR material"), m_recentDirectory, tr("PBR materials (*.pbrm);;All files (*.*)"));
    if (!fileName.isEmpty()) openFile(fileName);
}

bool MainWindow::openFile(const QString& fileName)
{
    const QString sidecar = autosaveFileName(fileName);
    bool restoredFromAutosave = false;
    if (!sidecar.isEmpty() && QFileInfo::exists(sidecar)
        && QFileInfo(sidecar).lastModified() > QFileInfo(fileName).lastModified()) {
        const auto answer = QMessageBox::question(this, tr("Recover material"),
            tr("A newer autosave snapshot exists for %1.\nRestore the recovered changes?").arg(QFileInfo(fileName).fileName()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (answer == QMessageBox::Yes) restoredFromAutosave = true;
        else QFile::remove(sidecar);
    }
    QString error;
    if (!m_document.load(restoredFromAutosave ? sidecar : fileName, &error)) {
        QMessageBox::critical(this, tr("Unable to open material"), error);
        if (restoredFromAutosave) return openFile(fileName);
        return false;
    }
    if (restoredFromAutosave) {
        m_document.setFileName(fileName);
        m_document.setModified(true);
        statusBar()->showMessage(tr("Restored autosaved changes — save to keep them"), 6000);
    }
    updateRecentDirectory(fileName);
    addRecentFile(fileName);
    m_undoStack->clear();
    m_undoStack->setClean();
    rebuildWorkspace();
    m_preview->showFullMaterial();
    updateWindowTitle();
    statusBar()->showMessage(tr("Opened %1").arg(QFileInfo(fileName).fileName()), 4000);
    return true;
}

bool MainWindow::saveDocument()
{
    if (m_document.fileName().isEmpty()) return saveDocumentAs();
    QString error;
    if (!m_document.save(m_document.fileName(), &error)) {
        QMessageBox::critical(this, tr("Unable to save material"), error);
        return false;
    }
    m_document.setModified(false);
    QFile::remove(autosaveFileName(m_document.fileName()));
    if (QSettings().value(QStringLiteral("Interface/jsonSidecar"), false).toBool()) {
        // Version-control aid: a readable mirror of the payload. Never read back.
        QFile sidecar(m_document.fileName() + QStringLiteral(".json"));
        if (sidecar.open(QIODevice::WriteOnly | QIODevice::Truncate))
            sidecar.write(QJsonDocument(m_document.toJson()).toJson(QJsonDocument::Indented));
    }
    addRecentFile(m_document.fileName());
    m_undoStack->setClean();
    updateWindowTitle();
    statusBar()->showMessage(tr("Saved %1").arg(QFileInfo(m_document.fileName()).fileName()), 4000);
    return true;
}

bool MainWindow::saveDocumentAs()
{
    QString baseDirectory = m_recentDirectory;
    if (baseDirectory.isEmpty()) baseDirectory = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QString suggested = m_document.fileName().isEmpty() ? baseDirectory + QStringLiteral("/material.pbrm") : m_document.fileName();
    QString fileName = QFileDialog::getSaveFileName(this, tr("Save PBR material"), suggested, tr("PBR materials (*.pbrm)"));
    if (fileName.isEmpty()) return false;
    if (!fileName.endsWith(QStringLiteral(".pbrm"), Qt::CaseInsensitive)) fileName += QStringLiteral(".pbrm");
    m_document.setFileName(fileName);
    updateRecentDirectory(fileName);
    return saveDocument();
}

bool MainWindow::maybeSave()
{
    if (!m_document.modified()) return true;
    const auto answer = QMessageBox::warning(this, tr("Unsaved material"), tr("Save changes to the current material?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Cancel) return false;
    if (answer == QMessageBox::Save) return saveDocument();
    return true;
}

void MainWindow::showUpscaleModelsDialog()
{
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("UpscaleModelsDialog"));
    dialog.setWindowTitle(tr("Standard Upscale Models"));
    auto* layout = new QVBoxLayout(&dialog);

    auto* intro = new QLabel(tr("Which trained model runs for each kind of map. A graph's "
                                "Upscale nodes use these unless a node names a model itself."), &dialog);
    intro->setWordWrap(true);
    intro->setObjectName(QStringLiteral("Muted"));
    layout->addWidget(intro);

    // Where the weights live. Defaulting to the folder beside the binary is
    // only a convention — the official Real-ESRGAN release ships no models at
    // all, so a working install is one that was assembled by hand and the
    // weights can be anywhere. Hardcoding it made a good install report "no
    // models found", which reads exactly like a broken one.
    auto* folderRow = new QHBoxLayout();
    folderRow->addWidget(new QLabel(tr("Models folder"), &dialog));
    auto* folderEdit = new QLineEdit(Upscaler::modelsPath(), &dialog);
    folderEdit->setReadOnly(true);
    folderRow->addWidget(folderEdit, 1);
    auto* browse = new QPushButton(tr("Browse…"), &dialog);
    folderRow->addWidget(browse);
    auto* resetFolder = new QPushButton(tr("Default"), &dialog);
    resetFolder->setToolTip(tr("Back to the models folder beside the upscaler binary."));
    folderRow->addWidget(resetFolder);
    layout->addLayout(folderRow);

    const QStringList installed = Upscaler::models();
    if (installed.isEmpty()) {
        auto* warning = new QLabel(tr("No models are installed beside the configured upscaler, "
                                      "so every map resamples with Lanczos. Set one under "
                                      "<b>Choose Upscaler…</b> first."), &dialog);
        warning->setWordWrap(true);
        layout->addWidget(warning);
    }

    auto* table = new QTreeWidget(&dialog);
    table->setObjectName(QStringLiteral("UpscaleModelTable"));
    table->setHeaderLabels({tr("Map"), tr("Model"), tr("Scale")});
    table->setRootIsDecorated(false);
    table->setUniformRowHeights(true);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    layout->addWidget(table, 1);

    // Kept so Restore Suggested can reset every row without rebuilding the
    // dialog, and so OK can write them all in one pass.
    struct Row { Upscaler::MapSlot slot; QComboBox* combo; QTreeWidgetItem* item; };
    QVector<Row> rows;
    for (const Upscaler::MapSlot slot : Upscaler::allSlots()) {
        auto* item = new QTreeWidgetItem(table);
        item->setText(0, Upscaler::slotLabel(slot));
        auto* combo = new QComboBox(table);
        // Lanczos first and always available: it is a legitimate choice, not
        // the absence of one, and for a normal with no normal-trained model
        // installed it is the only correct answer.
        combo->addItem(tr("Lanczos (no model)"), QString());
        for (const QString& model : installed) combo->addItem(model, model);
        const QString active = Upscaler::modelForSlot(slot);
        const int found = combo->findData(active);
        combo->setCurrentIndex(found >= 0 ? found : 0);
        WheelFocusGuard::install(combo);
        table->setItemWidget(item, 1, combo);
        const auto describe = [item, combo] {
            const QString model = combo->currentData().toString();
            const int scale = Upscaler::modelNativeScale(model);
            // The scale is worth showing: a model only runs at the size it was
            // trained for, and anything else is resampled from that.
            item->setText(2, model.isEmpty() ? tr("any")
                : scale ? QStringLiteral("%1×").arg(scale) : tr("unstated"));
        };
        describe();
        QObject::connect(combo, &QComboBox::currentIndexChanged, &dialog, describe);
        if (!Upscaler::slotModelIsPinned(slot))
            item->setToolTip(0, tr("Suggested, not chosen — picking anything here pins it."));
        rows.append({slot, combo, item});
    }

    // Restoration is a separate axis: a 1:1 de-block that runs BEFORE any
    // enlargement. One setting rather than nine, because BC artifacts are BC
    // artifacts whatever the map means.
    auto* restoreRow = new QHBoxLayout();
    restoreRow->addWidget(new QLabel(tr("Restore (1:1)"), &dialog));
    auto* restoreCombo = new QComboBox(&dialog);
    restoreCombo->setToolTip(tr("Removes block-compression artifacts without resizing. "
                                "Only 1× models are listed — anything else would change the size."));
    restoreCombo->addItem(tr("Off"), QString());
    for (const QString& model : installed)
        if (Upscaler::modelNativeScale(model) == 1) restoreCombo->addItem(model, model);
    const int restoreFound = restoreCombo->findData(Upscaler::restorationModel());
    restoreCombo->setCurrentIndex(restoreFound >= 0 ? restoreFound : 0);
    WheelFocusGuard::install(restoreCombo);
    restoreRow->addWidget(restoreCombo, 1);
    if (restoreCombo->count() == 1)
        restoreRow->addWidget(new QLabel(tr("no 1× models installed"), &dialog));
    layout->addLayout(restoreRow);

    auto* hint = new QLabel(tr("Reflectance feeds the metal inference: a model that invents "
                               "detail there changes what converts as metal, not only how it looks."), &dialog);
    hint->setWordWrap(true);
    hint->setObjectName(QStringLiteral("Muted"));
    layout->addWidget(hint);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel
        | QDialogButtonBox::RestoreDefaults, &dialog);
    buttons->button(QDialogButtonBox::RestoreDefaults)->setText(tr("Restore Suggested"));
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, &dialog, [&rows] {
        for (const Row& row : rows) {
            const int found = row.combo->findData(Upscaler::suggestedModelForSlot(row.slot));
            row.combo->setCurrentIndex(found >= 0 ? found : 0);
        }
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.resize(560, 420);

    // The folder is applied immediately rather than on OK: changing it changes
    // which models exist, and every combo above was filled from the old list.
    // Re-opening the dialog is the honest way to show the new one.
    connect(browse, &QPushButton::clicked, &dialog, [this, &dialog, folderEdit] {
        const QString chosen = QFileDialog::getExistingDirectory(&dialog,
            tr("Choose the folder holding the .param/.bin models"), folderEdit->text());
        if (chosen.isEmpty()) return;
        Upscaler::setModelsPath(chosen);
        folderEdit->setText(chosen);
        dialog.accept();
        QTimer::singleShot(0, this, [this] { showUpscaleModelsDialog(); });
    });
    connect(resetFolder, &QPushButton::clicked, &dialog, [this, &dialog] {
        Upscaler::clearModelsPath();
        dialog.accept();
        QTimer::singleShot(0, this, [this] { showUpscaleModelsDialog(); });
    });

    if (dialog.exec() != QDialog::Accepted) return;
    for (const Row& row : rows) Upscaler::setModelForSlot(row.slot, row.combo->currentData().toString());
    Upscaler::setRestorationModel(restoreCombo->currentData().toString());
    statusBar()->showMessage(tr("Standard upscale models saved"), 5000);
}

void MainWindow::scanConverterSources()
{
    if (m_convertWorkspace) m_convertWorkspace->scanNow();
}

void MainWindow::selectFirstConvertMaterial()
{
    // A scanned-but-unselected converter shows a placeholder where the graph
    // goes, so every capture of the canvas has to pick a row first.
    if (m_convertWorkspace) m_convertWorkspace->selectFirstMaterial();
}

void MainWindow::updateWindowTitle()
{
    // In the converter the authored document is not what is on screen, and
    // showing its name there is actively misleading.
    if (m_workspaceStack && m_workspaceStack->currentIndex() == 1) {
        const QString source = m_convertWorkspace ? m_convertWorkspace->sourceLabel() : QString();
        setWindowTitle(source.isEmpty()
                ? tr("Material Converter — FO4 PBR Material Editor")
                : tr("%1 — Material Converter — FO4 PBR Material Editor").arg(source));
        return;
    }
    const QString name = m_document.fileName().isEmpty() ? tr("Untitled") : QFileInfo(m_document.fileName()).fileName();
    setWindowTitle(tr("%1%2 — FO4 PBR Material Editor").arg(name, m_document.modified() ? QStringLiteral(" *") : QString()));
}

void MainWindow::updateRecentDirectory(const QString& fileName)
{
    m_recentDirectory = QFileInfo(fileName).absolutePath();
    QSettings settings;
    settings.setValue(QStringLiteral("recentDirectory"), m_recentDirectory);
}

void MainWindow::addRecentFile(const QString& fileName)
{
    if (fileName.trimmed().isEmpty()) return;
    const QString canonical = QDir::toNativeSeparators(QFileInfo(fileName).absoluteFilePath());
    QSettings settings;
    QStringList recent = settings.value(QStringLiteral("recentFiles")).toStringList();
    recent.removeAll(canonical);
    recent.prepend(canonical);
    while (recent.size() > 10) recent.removeLast();
    settings.setValue(QStringLiteral("recentFiles"), recent);
    settings.setValue(QStringLiteral("lastFile"), canonical);
    rebuildRecentMenu();
}

void MainWindow::rebuildTemplateMenu()
{
    if (!m_templateMenu) return;
    m_templateMenu->clear();
    for (const QString& name : materialTemplateNames())
        m_templateMenu->addAction(name, this, [this, name] { applyMaterialTemplate(name); });
    QSettings settings;
    const QStringList userNames = settings.value(QStringLiteral("Templates/names")).toStringList();
    const QStringList userPayloads = settings.value(QStringLiteral("Templates/payloads")).toStringList();
    if (!userNames.isEmpty()) m_templateMenu->addSeparator();
    for (int i = 0; i < qMin(userNames.size(), userPayloads.size()); ++i) {
        const QString name = userNames[i];
        const QString payload = userPayloads[i];
        m_templateMenu->addAction(name, this, [this, name, payload] {
            if (!maybeSave()) return;
            const QJsonDocument json = QJsonDocument::fromJson(payload.toUtf8());
            QString error;
            Pbr::MaterialDocument templateDocument;
            if (!json.isObject() || !templateDocument.fromJson(json.object(), &error)) {
                QMessageBox::warning(this, tr("User template"), tr("The stored template could not be loaded: %1").arg(error));
                return;
            }
            m_document = templateDocument;
            m_document.setFileName(QString());
            m_document.setModified(true);
            m_undoStack->clear();
            rebuildWorkspace();
            updateWindowTitle();
            statusBar()->showMessage(tr("Created material from template %1").arg(name), 3000);
        });
    }
    m_templateMenu->addSeparator();
    m_templateMenu->addAction(tr("Save Current as Template…"), this, &MainWindow::saveCurrentAsTemplate);
    if (!userNames.isEmpty()) {
        auto* removeMenu = m_templateMenu->addMenu(tr("Remove User Template"));
        for (const QString& name : userNames) {
            removeMenu->addAction(name, this, [this, name] {
                QSettings settings;
                QStringList names = settings.value(QStringLiteral("Templates/names")).toStringList();
                QStringList payloads = settings.value(QStringLiteral("Templates/payloads")).toStringList();
                const int index = names.indexOf(name);
                if (index >= 0 && index < payloads.size()) { names.removeAt(index); payloads.removeAt(index); }
                settings.setValue(QStringLiteral("Templates/names"), names);
                settings.setValue(QStringLiteral("Templates/payloads"), payloads);
                rebuildTemplateMenu();
            });
        }
    }
}

void MainWindow::saveCurrentAsTemplate()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Save as Template"), tr("Template name"),
        QLineEdit::Normal, tr("My Material"), &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    QSettings settings;
    QStringList names = settings.value(QStringLiteral("Templates/names")).toStringList();
    QStringList payloads = settings.value(QStringLiteral("Templates/payloads")).toStringList();
    const QString payload = QString::fromUtf8(QJsonDocument(m_document.toJson()).toJson(QJsonDocument::Compact));
    const int existing = names.indexOf(name);
    if (existing >= 0 && existing < payloads.size()) payloads[existing] = payload;
    else { names.append(name); payloads.append(payload); }
    settings.setValue(QStringLiteral("Templates/names"), names);
    settings.setValue(QStringLiteral("Templates/payloads"), payloads);
    rebuildTemplateMenu();
    statusBar()->showMessage(tr("Saved template %1").arg(name), 3000);
}

void MainWindow::rebuildRecentMenu()
{
    if (!m_recentMenu) return;
    m_recentMenu->clear();
    QSettings settings;
    QStringList recent = settings.value(QStringLiteral("recentFiles")).toStringList();
    recent.erase(std::remove_if(recent.begin(), recent.end(), [](const QString& file) { return !QFileInfo::exists(file); }), recent.end());
    settings.setValue(QStringLiteral("recentFiles"), recent);
    if (recent.isEmpty()) {
        auto* empty = m_recentMenu->addAction(tr("No recent materials"));
        empty->setEnabled(false);
        return;
    }
    for (const QString& fileName : recent) {
        auto* action = m_recentMenu->addAction(QFileInfo(fileName).fileName());
        action->setToolTip(fileName);
        connect(action, &QAction::triggered, this, [this, fileName] {
            if (maybeSave()) openFile(fileName);
        });
    }
    m_recentMenu->addSeparator();
    m_recentMenu->addAction(tr("Clear Recent"), this, [this] {
        QSettings().remove(QStringLiteral("recentFiles"));
        rebuildRecentMenu();
    });
}

void MainWindow::restoreLastSession()
{
    const QString lastFile = QSettings().value(QStringLiteral("lastFile")).toString();
    if (!lastFile.isEmpty() && QFileInfo::exists(lastFile)) openFile(lastFile);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    // Texture drops onto slot widgets are handled by the widgets themselves;
    // this window-level handler only claims .pbrm material files.
    const QList<QUrl> urls = event->mimeData()->urls();
    if (urls.size() == 1 && urls.first().isLocalFile()
        && urls.first().toLocalFile().endsWith(QStringLiteral(".pbrm"), Qt::CaseInsensitive))
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event)
{
    const QList<QUrl> urls = event->mimeData()->urls();
    if (urls.isEmpty() || !urls.first().isLocalFile()) return;
    const QString fileName = urls.first().toLocalFile();
    if (!fileName.endsWith(QStringLiteral(".pbrm"), Qt::CaseInsensitive)) return;
    event->acceptProposedAction();
    if (maybeSave()) openFile(fileName);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (maybeSave()) {
        // Saved or deliberately discarded — either way the snapshot is stale.
        QFile::remove(autosaveFileName(m_document.fileName()));
        event->accept();
    } else {
        event->ignore();
    }
}
