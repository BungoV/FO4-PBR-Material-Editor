#include "mainwindow.h"
#include "specularmigrate.h"

#include "colorpicker.h"
#include "disclosureicon.h"
#include "dragnumericedit.h"
#include "materialpreviewwidget.h"
#include "previewenvironmentsettings.h"
#include "texturearchive.h"
#include "texturebrowserdialog.h"
#include "texturecache.h"
#include "textureimage.h"
#include "wheelfocusguard.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QClipboard>
#include <QComboBox>
#include <QCompleter>
#include <QCoreApplication>
#include <QFileDialog>
#include <QFileInfo>
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
#include <QSet>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QStatusBar>
#include <QStandardPaths>
#include <QStringListModel>
#include <QToolButton>
#include <QTimer>
#include <QUndoCommand>
#include <QUndoStack>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>
#include "mainwindow_detail.h"

MainWindow::PageLayout MainWindow::createPage(const QString& title, const QString& description)
{
    auto* scroll = new QScrollArea(m_pages);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    auto* host = new QWidget(scroll);
    auto* content = new QVBoxLayout(host);
    content->setContentsMargins(5, 5, 5, 9);
    content->setSpacing(4);
    auto* intro = new QFrame(host);
    intro->setObjectName(QStringLiteral("Card"));
    intro->setProperty("searchText", title + QStringLiteral(" ") + description);
    auto* introLayout = new QVBoxLayout(intro);
    introLayout->setContentsMargins(8, 4, 8, 4);
    auto* heading = new QLabel(title, intro);
    heading->setObjectName(QStringLiteral("SectionTitle"));
    auto* copy = new QLabel(description, intro);
    copy->setObjectName(QStringLiteral("Muted"));
    copy->setWordWrap(true);
    introLayout->addWidget(heading);
    if (m_advancedInterface) introLayout->addWidget(copy);
    else { intro->setToolTip(description); copy->deleteLater(); }
    content->addWidget(intro);
    scroll->setWidget(host);
    return {scroll, content, intro, introLayout};
}

QWidget* MainWindow::createOverviewPage()
{
    auto* tabs = new QTabWidget(m_pages);
    tabs->setObjectName(QStringLiteral("OverviewTabs"));
    tabs->addTab(createValidationPage(), tr("Validation"));
    tabs->addTab(createMaterialInfoPage(), tr("Material"));
    tabs->addTab(createOverridesPage(), tr("Authored Overrides"));
    return tabs;
}

QWidget* MainWindow::createMaterialInfoPage()
{
    auto page = createPage(tr("Overview — %1").arg(Pbr::shaderModeName(m_document.shaderMode())), Pbr::shaderModeDescription(m_document.shaderMode()));
    if (m_document.shaderMode() == Pbr::ShaderMode::LandscapeSurface) {
        addInfoCard(page.content, tr("Landscape workflow"),
            tr("Author terrain resources entirely in Landscape. LAND weights choose LTEX/TXST layers; texture arrays supply base color, normal/height, and RMAOS. Primary and Secondary UV are intentionally hidden."));
    } else {
        addInfoCard(page.content, tr("Authoring workflow"),
            tr("1. Author the base material in Primary UV.\n2. Optionally enable Secondary UV for overlays, detail, clearcoat, or independent animation.\n3. Tune the selected shader behavior.\n4. Open Validation before saving PBRM V5."));
    }
    if (m_document.shaderMode() == Pbr::ShaderMode::AllFeatures) {
        addInfoCard(page.content, tr("Advanced warning"),
            tr("This shader is for prototype and combination validation. Final assets should use the narrowest applicable shader to avoid expensive or invalid combinations."), true);
    }
    {
        qint64 totalFileBytes = 0;
        double vramMegabytes = 0;
        int textureCount = 0;
        QStringList outliers;
        const auto tally = [this, &totalFileBytes, &vramMegabytes, &textureCount, &outliers](const QString& setName, const QJsonObject& set) {
            for (auto it = set.constBegin(); it != set.constEnd(); ++it) {
                const auto state = Pbr::TextureSlotState::fromJson(it.value().toObject());
                if (!state.enabled || state.path.trimmed().isEmpty()) continue;
                const TextureImage::Diagnostics info = TextureImage::inspect(state.path, m_textureRoot);
                if (!info.valid) continue;
                ++textureCount;
                totalFileBytes += info.fileSize;
                const bool halfByte = info.format == QStringLiteral("BC1") || info.format == QStringLiteral("DXT1")
                    || info.format == QStringLiteral("BC4") || info.format == QStringLiteral("ATI1");
                const bool compressed = info.format.startsWith(QStringLiteral("BC"))
                    || info.format.startsWith(QStringLiteral("DXT")) || info.format.startsWith(QStringLiteral("ATI"));
                const double bytesPerTexel = halfByte ? .5 : (compressed ? 1.0 : 4.0);
                vramMegabytes += double(info.width) * info.height * bytesPerTexel * (info.mipLevels > 1 ? 4.0 / 3.0 : 1.0) / (1024.0 * 1024.0);
                if (qMax(info.width, info.height) > 2048)
                    outliers << tr("%1/%2 is %3×%4").arg(setName, it.key()).arg(info.width).arg(info.height);
                if (!compressed)
                    outliers << tr("%1/%2 is uncompressed").arg(setName, it.key());
            }
        };
        for (const QString& set : {QStringLiteral("UV0"), QStringLiteral("UV1"), QStringLiteral("UV2")})
            tally(set, m_document.textureSet(set));
        const QJsonObject resources = m_document.toJson().value(QStringLiteral("resources")).toObject();
        for (auto it = resources.constBegin(); it != resources.constEnd(); ++it)
            tally(it.key(), it.value().toObject());
        if (textureCount > 0) {
            QStringList lines;
            lines << tr("%1 resolved texture(s) · %2 MiB on disk · ~%3 MiB VRAM with mips")
                .arg(textureCount).arg(double(totalFileBytes) / (1024.0 * 1024.0), 0, 'f', 1).arg(vramMegabytes, 0, 'f', 1);
            if (!outliers.isEmpty()) lines << outliers.join(QStringLiteral("\n"));
            addInfoCard(page.content, tr("Texture budget"), lines.join(QStringLiteral("\n")), !outliers.isEmpty());
        }
    }
    page.content->addStretch();
    return page.page;
}

QWidget* MainWindow::createOverridesPage()
{
    auto page = createPage(tr("Authored Overrides"),
        tr("Shows only values stored by this material that differ from the implicit new-material defaults."));
    QStringList general;
    if (m_document.shaderMode() != Pbr::ShaderMode::StandardSurface)
        general.append(tr("Shader: %1").arg(Pbr::shaderModeName(m_document.shaderMode())));
    if (m_document.secondaryUvEnabled()) general.append(tr("Secondary UV: enabled"));
    if (m_document.tertiaryUvEnabled()) general.append(tr("Tertiary UV: enabled"));
    if (!general.isEmpty()) addInfoCard(page.content, tr("Material"), general.join(QStringLiteral("\n")));

    const auto addTextureOverrides = [this, &page](const QString& title, const QJsonObject& set) {
        QStringList lines;
        for (auto it = set.constBegin(); it != set.constEnd(); ++it) {
            const Pbr::TextureSlotState state = Pbr::TextureSlotState::fromJson(it.value().toObject());
            QString detail = state.enabled ? tr("enabled") : tr("disabled");
            detail += state.path.trimmed().isEmpty() ? tr(", constants") : tr(", %1").arg(state.path);
            if (!state.values.isEmpty()) detail += tr(", %1 authored value(s)").arg(state.values.size());
            lines.append(QStringLiteral("%1 — %2").arg(it.key(), detail));
        }
        if (!lines.isEmpty()) addInfoCard(page.content, title, lines.join(QStringLiteral("\n")));
    };
    addTextureOverrides(tr("Primary UV"), m_document.textureSet(QStringLiteral("UV0")));
    addTextureOverrides(tr("Secondary UV"), m_document.textureSet(QStringLiteral("UV1")));
    addTextureOverrides(tr("Tertiary UV"), m_document.textureSet(QStringLiteral("UV2")));
    const QJsonObject resources = m_document.toJson().value(QStringLiteral("resources")).toObject();
    for (auto it = resources.constBegin(); it != resources.constEnd(); ++it)
        addTextureOverrides(tr("Resource: %1").arg(it.key()), it.value().toObject());

    QStringList settings;
    const QJsonObject groups = m_document.toJson().value(QStringLiteral("settings")).toObject();
    for (auto group = groups.constBegin(); group != groups.constEnd(); ++group) {
        const QJsonObject values = group.value().toObject();
        for (auto value = values.constBegin(); value != values.constEnd(); ++value)
            settings.append(QStringLiteral("%1 / %2 = %3").arg(group.key(), value.key(), value.value().toVariant().toString()));
    }
    if (!settings.isEmpty()) addInfoCard(page.content, tr("Settings"), settings.join(QStringLiteral("\n")));

    if (general.isEmpty() && m_document.textureSet(QStringLiteral("UV0")).isEmpty()
        && m_document.textureSet(QStringLiteral("UV1")).isEmpty() && resources.isEmpty() && settings.isEmpty())
        addInfoCard(page.content, tr("No authored overrides"), tr("This material currently matches the implicit new-material defaults."));
    page.content->addStretch();
    return page.page;
}

void MainWindow::addInfoCard(QVBoxLayout* layout, const QString& title, const QString& text, bool warning)
{
    auto* card = new QFrame(layout->parentWidget());
    card->setObjectName(QStringLiteral("Card"));
    card->setProperty("searchText", title + QStringLiteral(" ") + text);
    if (warning) card->setStyleSheet(QStringLiteral("QFrame#Card { border-color:#9a6d2c; background:#392f24; }"));
    auto* outer = new QVBoxLayout(card);
    outer->setContentsMargins(8, 5, 8, 5);
    auto* heading = new QLabel(title, card);
    heading->setObjectName(QStringLiteral("SectionTitle"));
    auto* copy = new QLabel(text, card);
    copy->setObjectName(QStringLiteral("Muted"));
    copy->setWordWrap(true);
    outer->addWidget(heading);
    outer->addWidget(copy);
    layout->addWidget(card);
}

QWidget* MainWindow::createUvPage(const QString& uvSet)
{
    const bool tertiary = uvSet == QStringLiteral("UV2");
    const bool secondary = uvSet == QStringLiteral("UV1") || tertiary;
    const QString layerName = tertiary ? tr("Tertiary") : tr("Secondary");
    const QString pageTitle = tertiary ? tr("Tertiary UV Textures")
        : (secondary ? tr("Secondary UV Textures") : tr("Primary UV Textures"));
    auto page = createPage(pageTitle,
        secondary ? tr("Optional independently transformed material layer. Every texture input can be enabled, collapsed, inspected, and assigned a constant fallback.")
                  : tr("Base material layer. Texture sockets live here; feature pages contain only algorithms and behavior."));
    const bool layerEnabled = tertiary ? m_document.tertiaryUvEnabled() : m_document.secondaryUvEnabled();
    if (secondary && !layerEnabled)
        addInfoCard(page.content, tr("%1 UV is inactive").arg(layerName), tr("You can author this layer while it is disabled. Check %1 UV in the navigation to include it in the material.").arg(layerName), true);
    page.content->removeWidget(page.header);
    delete page.header;
    auto* bulkActions = new QFrame(page.page);
    bulkActions->setObjectName(QStringLiteral("Card"));
    bulkActions->setProperty("searchText", tr("copy paste reset UV layer texture overrides expand collapse all"));
    auto* bulkRow = new QHBoxLayout(bulkActions);
    bulkRow->setContentsMargins(8, 5, 8, 5);
    auto* bulkLabel = new QLabel(pageTitle, bulkActions);
    bulkLabel->setObjectName(QStringLiteral("SectionTitle"));
    auto* uvBadge = new QLabel(tertiary ? tr("UV2") : (secondary ? tr("UV1") : tr("UV0")), bulkActions);
    uvBadge->setObjectName(QStringLiteral("PathBadge"));
    uvBadge->setToolTip(tertiary ? tr("Independent third layer; its mesh coordinate source is chosen in UV Mapping.")
        : (secondary ? tr("TEXCOORD1") : tr("TEXCOORD0")));
    auto* enabledOnly = new QCheckBox(tr("Enabled only"), bulkActions);
    enabledOnly->setToolTip(tr("Hide disabled texture inputs without deleting their settings."));
    auto* bulkMenuButton = new QToolButton(bulkActions);
    bulkMenuButton->setText(QStringLiteral("⋯"));
    bulkMenuButton->setPopupMode(QToolButton::InstantPopup);
    bulkMenuButton->setToolTip(tr("Bulk texture-input actions."));
    auto* bulkMenu = new QMenu(bulkMenuButton);
    auto* useTextureChannels = bulkMenu->addAction(tr("Use Available Texture Channels"));
    auto* useConstantChannels = bulkMenu->addAction(tr("Enable All Constant Overrides"));
    bulkMenu->addSeparator();
    auto* expandAll = bulkMenu->addAction(tr("Expand All"));
    auto* collapseAll = bulkMenu->addAction(tr("Collapse All"));
    bulkMenu->addSeparator();
    bulkMenu->addAction(tr("Copy Layer"), this, [this, uvSet] { copyUvLayer(uvSet); });
    bulkMenu->addAction(tr("Paste Layer"), this, [this, uvSet] { pasteUvLayer(uvSet); });
    bulkMenu->addAction(tr("Reset Layer"), this, [this, uvSet] { resetUvLayer(uvSet); });
    bulkMenuButton->setMenu(bulkMenu);
    bulkRow->addWidget(bulkLabel);
    bulkRow->addWidget(uvBadge);
    bulkRow->addStretch();
    bulkRow->addWidget(enabledOnly);
    bulkRow->addWidget(bulkMenuButton);
    page.content->insertWidget(0, bulkActions);
    bulkActions->setContextMenuPolicy(Qt::CustomContextMenu);
    bulkActions->setToolTip(tr("Right-click for Copy Layer, Paste Layer, and Reset Layer."));
    connect(bulkActions, &QWidget::customContextMenuRequested, this, [this, bulkActions, uvSet](const QPoint& position) {
        QMenu menu(bulkActions);
        menu.addAction(tr("Copy Layer"), this, [this, uvSet] { copyUvLayer(uvSet); });
        menu.addAction(tr("Paste Layer"), this, [this, uvSet] { pasteUvLayer(uvSet); });
        menu.addSeparator();
        menu.addAction(tr("Reset Layer"), this, [this, uvSet] { resetUvLayer(uvSet); });
        menu.exec(bulkActions->mapToGlobal(position));
    });

    // Coordinate transforms and controller binding live on the dedicated UV Mapping page.
    QVector<TextureSlotWidget*> inputs;
    for (const TextureSlotSpec& spec : textureSlotsFor(m_document.shaderMode(), uvSet))
        inputs.append(addTextureSlot(page.content, uvSet, spec));
    const auto updateFilter = [enabledOnly, inputs] {
        for (TextureSlotWidget* input : inputs) input->setVisible(!enabledOnly->isChecked() || input->isSlotEnabled());
    };
    connect(enabledOnly, &QCheckBox::toggled, this, [updateFilter](bool) { updateFilter(); });
    for (TextureSlotWidget* input : inputs)
        connect(input, &TextureSlotWidget::stateChanged, this, [updateFilter](const QString&, const QString&, const Pbr::TextureSlotState&) { updateFilter(); });
    const auto applyOverrideMode = [this, inputs, uvSet](bool overrideChannels) {
        applyDocumentChange(overrideChannels ? tr("Enable constant overrides in %1").arg(uvSet) : tr("Use available texture channels in %1").arg(uvSet),
            [this, inputs, overrideChannels] {
                for (TextureSlotWidget* input : inputs)
                    if (input->hasChannelSources())
                        m_document.setTextureSlot(input->uvSet(), input->key(), input->stateWithAllChannelSources(overrideChannels));
            });
        rebuildWorkspace(m_currentPage);
    };
    connect(useTextureChannels, &QAction::triggered, this, [applyOverrideMode] { applyOverrideMode(false); });
    connect(useConstantChannels, &QAction::triggered, this, [applyOverrideMode] { applyOverrideMode(true); });
    const auto applyExpanded = [this, inputs](bool expanded) {
        applyDocumentChange(expanded ? tr("Expand all texture inputs") : tr("Collapse all texture inputs"),
            [this, inputs, expanded] {
                for (TextureSlotWidget* input : inputs) {
                    Pbr::TextureSlotState state = input->state();
                    state.expanded = expanded;
                    m_document.setTextureSlot(input->uvSet(), input->key(), state);
                }
            });
        rebuildWorkspace(m_currentPage);
    };
    connect(expandAll, &QAction::triggered, this, [applyExpanded] { applyExpanded(true); });
    connect(collapseAll, &QAction::triggered, this, [applyExpanded] { applyExpanded(false); });
    page.content->addStretch();
    return page.page;
}

QWidget* MainWindow::createUvMappingPage()
{
    auto page = createPage(tr("UV Mapping"), tr("Coordinate sources, transforms, address modes, animation, and NIF controller bindings for every texture layer."));
    const auto addLayer = [this, &page](const QString& uvSet, const QString& title, int layer) {
        addGroupTitle(page.content, title);
        addSettingsCard(page.content, uvSet, tr("Coordinate transform"), coordinateFields(layer));
        addSettingsCard(page.content, uvSet + QStringLiteral("/Animation"), tr("UV animation and controller binding"), {
            numberField("scrollU", "Scroll speed U", 0.0, -1000.0, 1000.0, 0.001, "Horizontal coordinate motion per second."),
            numberField("scrollV", "Scroll speed V", 0.0, -1000.0, 1000.0, 0.001, "Vertical coordinate motion per second."),
            numberField("rotationSpeed", "Rotation speed", 0.0, -1000.0, 1000.0, 0.01, "Coordinate rotation per second.", 2, "°/s"),
            numberField("phase", "Animation phase", 0.0, -1000.0, 1000.0, 0.001, "Initial time or phase offset."),
            textField("controller", "NIF controller binding", "", "Optional controller or animation-parameter binding for this coordinate set.")
        });
        addSettingsCard(page.content, uvSet + QStringLiteral("/Oscillation"), tr("Oscillation (breathing)"), {
            choiceField("waveform", "Waveform", "Sine", {"Sine", "Triangle", "Pulse", "Breathing"}, "Periodic waveform driving every oscillation target on this layer. Breathing is an eased sine with organic, fuller crests."),
            numberField("frequency", "Frequency", 0.0, 0.0, 20.0, 0.01, "Oscillation cycles per second; zero disables the oscillator.", 4, " Hz"),
            numberField("phase", "Phase", 0.0, 0.0, 1.0, 0.005, "Cycle offset so layers can breathe out of sync."),
            numberField("duty", "Pulse duty", 0.5, 0.05, 0.95, 0.005, "Pulse waveform only: fraction of the cycle spent high."),
            numberField("scaleAmplitude", "Scale amplitude", 0.0, 0.0, 1.0, 0.005, "UV scale swell around the authored scale — the classic breathing effect."),
            numberField("offsetAmplitudeU", "Offset amplitude U", 0.0, 0.0, 4.0, 0.005, "Horizontal sway distance in UV units."),
            numberField("offsetAmplitudeV", "Offset amplitude V", 0.0, 0.0, 4.0, 0.005, "Vertical sway distance in UV units."),
            numberField("rotationAmplitude", "Rotation amplitude", 0.0, 0.0, 180.0, 0.1, "Rotational wobble around the authored rotation.", 2, "°"),
            numberField("emissiveAmplitude", "Emissive amplitude", 0.0, 0.0, 4.0, 0.005, "Pulses this layer's emissive intensity (glow breathing)."),
            numberField("coverageAmplitude", "Coverage amplitude", 0.0, 0.0, 1.0, 0.005, "Pulses layer coverage; on the primary layer this pulses opacity."),
            numberField("normalAmplitude", "Normal amplitude", 0.0, 0.0, 1.0, 0.005, "Pulses normal-map strength so surface detail swells and relaxes.")
        });
    };
    const auto addCompositionCard = [this, &page](const QString& uvSet, const QString& layerName, const QString& baseAlphaSource) {
        addSettingsCard(page.content, uvSet + QStringLiteral("/Composition"), tr("%1 layer composition & culling").arg(layerName), {
            choiceField("coverageMode", "Coverage mode", "Multiply", {"Vertex Only", "Multiply", "Pattern Reveal", "Height Blend"}, "Combines broad vertex-painted coverage with texture-authored breakup. Height Blend compares the layers' Normal+Height blue channels so the overlay settles into recesses first; enable \"B contains height\" on the participating Normal inputs. Without height data it behaves like Multiply."),
            choiceField("vertexChannel", "Vertex paint channel", "Red", {"Red", "Green", "Blue", "Alpha", "None"}, QStringLiteral("Vertex-color channel carrying broad %1 UV coverage.").arg(layerName)),
            choiceField("pattern", "Pattern source", "Constant", {"Layer Coverage R", baseAlphaSource, "Constant"}, "High-frequency pattern used to reveal or break up the layer. Height Blend coverage is the physically-motivated default breakup; use a dedicated Layer Coverage texture for art-directed patterns height cannot express."),
            numberField("constantCoverage", "Constant pattern", 1.0, 0.0, 1.0, 0.005, "Pattern coverage used when Pattern source is Constant."),
            numberField("bias", "Pattern bias", 0.0, -1.0, 1.0, 0.002, "Moves the reveal threshold."),
            numberField("contrast", "Pattern contrast", 1.0, 0.0, 16.0, 0.01, "Sharpens or softens the pattern."),
            numberField("edgeSoftness", "Edge softness", 0.1, 0.0, 1.0, 0.005, "Controls the transition width between layers."),
            toggleField("sparseSampling", "Skip empty regions", true, QStringLiteral("Allows coherent zero-coverage areas to avoid %1 UV texture samples.").arg(layerName))
        });
        const QString patternSource = m_document.setting(uvSet + QStringLiteral("/Composition"), QStringLiteral("pattern"), QStringLiteral("Constant")).toString();
        setSettingEditorsEnabled(page.page, uvSet + QStringLiteral("/Composition"),
            {QStringLiteral("constantCoverage")}, patternSource == QStringLiteral("Constant"));
    };
    addLayer(QStringLiteral("UV0"), tr("PRIMARY UV TEXTURES"), 0);
    addLayer(QStringLiteral("UV1"), tr("SECONDARY UV TEXTURES"), 1);
    addCompositionCard(QStringLiteral("UV1"), tr("Secondary"), QStringLiteral("Secondary Base Alpha"));
    addLayer(QStringLiteral("UV2"), tr("TERTIARY UV TEXTURES"), 2);
    addCompositionCard(QStringLiteral("UV2"), tr("Tertiary"), QStringLiteral("Tertiary Base Alpha"));
    for (const QString& set : {QStringLiteral("UV0"), QStringLiteral("UV1"), QStringLiteral("UV2")}) {
        const QString source = m_document.setting(set, QStringLiteral("source"),
            set == QStringLiteral("UV0") ? QStringLiteral("Mesh UV0") : QStringLiteral("Mesh UV1")).toString();
        const bool planar = source.contains(QStringLiteral("Planar"));
        setSettingEditorsEnabled(page.page, set, {QStringLiteral("projectionAxis"), QStringLiteral("triplanarBlend")}, planar);
    }
    page.content->addStretch();
    return page.page;
}

TextureSlotWidget* MainWindow::addTextureSlot(QVBoxLayout* layout, const QString& uvSet, const TextureSlotSpec& spec)
{
    auto state = m_document.textureSlot(uvSet, spec.key);
    auto* widget = new TextureSlotWidget(uvSet, spec, state, m_textureRoot, layout->parentWidget());
    if (m_document.textureSet(uvSet).contains(spec.key) && widget->state().toJson() != state.toJson()) {
        const bool wasModified = m_document.modified();
        m_document.setTextureSlot(uvSet, spec.key, widget->state());
        m_document.setModified(wasModified);
    }
    widget->setAdvancedVisible(m_advancedInterface);
    widget->setProperty("searchText", spec.name + QStringLiteral(" ") + spec.format);
    connect(widget, &TextureSlotWidget::stateChanged, this, [this](const QString& set, const QString& key, const Pbr::TextureSlotState& value) {
        // Per-slot description: better undo labels, and drag merging never
        // collapses edits from different inputs.
        applyDocumentChange(tr("Edit %1/%2").arg(set, key), [this, set, key, value] { m_document.setTextureSlot(set, key, value); });
    });
    connect(widget, &TextureSlotWidget::inspectRequested, m_preview, &MaterialPreviewWidget::inspectTexture);
    connect(widget, &TextureSlotWidget::resetRequested, this, [this](const QString& set, const QString& key) {
        applyDocumentChange(tr("Reset texture input"), [this, set, key] { m_document.removeTextureSlot(set, key); });
        rebuildWorkspace(m_currentPage);
    });
    connect(widget, &TextureSlotWidget::pasteRequested, this, [this](const QString& set, const QString& key, const Pbr::TextureSlotState& state) {
        applyDocumentChange(tr("Paste texture input"), [this, set, key, state] { m_document.setTextureSlot(set, key, state); });
        rebuildWorkspace(m_currentPage);
    });
    connect(widget, &TextureSlotWidget::companionTexturesRequested, this,
        &MainWindow::assignCompanionTextures);
    layout->addWidget(widget);
    return widget;
}

void MainWindow::assignCompanionTextures(const QString& uvSet, const QString& sourceKey, const QString& absolutePath)
{
    if (uvSet != QStringLiteral("UV0") && uvSet != QStringLiteral("UV1") && uvSet != QStringLiteral("UV2")) return;
    const QFileInfo sourceInfo(absolutePath);
    const QString base = companionBaseName(sourceInfo.fileName());
    if (base.isEmpty()) return;
    const QDir directory = sourceInfo.absoluteDir();
    const QFileInfoList files = directory.entryInfoList(
        {QStringLiteral("*.dds"), QStringLiteral("*.tga"), QStringLiteral("*.png"), QStringLiteral("*.jpg"),
         QStringLiteral("*.jpeg"), QStringLiteral("*.bmp")}, QDir::Files | QDir::Readable);

    QMap<QString, QString> candidates;
    for (const TextureSlotSpec& spec : textureSlotsFor(m_document.shaderMode(), uvSet)) {
        if (spec.key == sourceKey) continue;
        const QStringList suffixes = companionSuffixes(spec.key);
        if (suffixes.isEmpty()) continue;
        const Pbr::TextureSlotState existing = m_document.textureSlot(uvSet, spec.key);
        if (!existing.path.trimmed().isEmpty()) continue;
        for (const QString& suffix : suffixes) {
            const QString wanted = base + suffix;
            for (const QFileInfo& file : files) {
                if (file.completeBaseName().compare(wanted, Qt::CaseInsensitive) == 0) {
                    candidates.insert(spec.key, file.absoluteFilePath());
                    break;
                }
            }
            if (candidates.contains(spec.key)) break;
        }
    }
    if (candidates.isEmpty()) {
        statusBar()->showMessage(tr("No unfilled companion textures were found in %1.").arg(directory.dirName()), 5000);
        return;
    }

    QStringList found;
    for (auto it = candidates.cbegin(); it != candidates.cend(); ++it)
        found.append(QStringLiteral("%1 → %2").arg(it.key(), QFileInfo(it.value()).fileName()));
    applyDocumentChange(tr("Assign companion textures"), [this, uvSet, candidates] {
        for (auto it = candidates.cbegin(); it != candidates.cend(); ++it) {
            const QString authoredPath = TextureImage::authoredPathForFile(it.value(), m_textureRoot);
            Pbr::TextureSlotState state = m_document.textureSlot(uvSet, it.key());
            state.enabled = true;
            state.expanded = true;
            state.path = authoredPath;
            m_document.setTextureSlot(uvSet, it.key(), state);
        }
    });
    rebuildWorkspace(m_currentPage);
    statusBar()->showMessage(tr("Assigned %1 companion texture(s): %2")
        .arg(candidates.size()).arg(found.join(QStringLiteral(", "))), 7000);
}

void MainWindow::addGroupTitle(QVBoxLayout* layout, const QString& title)
{
    auto* label = new QLabel(title, layout->parentWidget());
    label->setObjectName(QStringLiteral("GroupTitle"));
    layout->addWidget(label);
}

QFrame* MainWindow::addSettingsCard(QVBoxLayout* layout, const QString& section, const QString& title, const QVector<FieldSpec>& fields)
{
    auto* card = new QFrame(layout->parentWidget());
    card->setObjectName(QStringLiteral("Card"));
    card->setProperty("settingSection", section);
    QString searchText = title;
    auto* outer = new QVBoxLayout(card);
    outer->setContentsMargins(8, 5, 8, 5);
    outer->setSpacing(5);
    auto* header = new QHBoxLayout();
    auto* heading = new QLabel(title, card);
    heading->setObjectName(QStringLiteral("SectionTitle"));
    header->addWidget(heading, 1);
    const QJsonObject authored = m_document.settingGroup(section);
    bool runtimeDriven = false;
    for (const FieldSpec& field : fields)
        runtimeDriven = runtimeDriven || field.tooltip.contains(QStringLiteral("runtime"), Qt::CaseInsensitive)
            || field.tooltip.contains(QStringLiteral("controller"), Qt::CaseInsensitive);
    const QString stateText = runtimeDriven ? tr("RUNTIME") : (authored.isEmpty() ? tr("DEFAULT") : tr("OVERRIDE"));
    auto* stateBadge = new QLabel(QStringLiteral("●"), card);
    stateBadge->setObjectName(QStringLiteral("StateBadge"));
    stateBadge->setToolTip(stateText);
    stateBadge->setProperty("stateKind", runtimeDriven ? QStringLiteral("runtime")
        : (authored.isEmpty() ? QStringLiteral("default") : QStringLiteral("override")));
    header->addWidget(stateBadge);
    auto* reset = new QToolButton(card);
    reset->setText(tr("Reset"));
    reset->setToolTip(tr("Restore every setting in this group to its default."));
    header->addWidget(reset);
    outer->addLayout(header);
    auto* form = new QFormLayout();
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(3);
    for (const FieldSpec& field : fields) {
        auto* label = new QLabel(field.label, card);
        label->setObjectName(QStringLiteral("Muted"));
        label->setToolTip(field.tooltip);
        QWidget* editor = createSettingEditor(section, field);
        editor->setToolTip(field.tooltip + tr("\nCtrl+Backspace resets this value to its default."));
        form->addRow(label, editor);
        searchText += QStringLiteral(" ") + field.label + QStringLiteral(" ") + field.tooltip;
    }
    outer->addLayout(form);
    connect(reset, &QToolButton::clicked, this, [this, section] {
        applyDocumentChange(tr("Reset %1").arg(section), [this, section] {
            m_document.clearSettingGroup(section);
        });
        rebuildWorkspace(m_currentPage);
    });
    card->setProperty("searchText", searchText);
    layout->addWidget(card);
    return card;
}

QWidget* MainWindow::createSettingEditor(const QString& section, const FieldSpec& field)
{
    const QJsonValue value = m_document.setting(section, field.key, field.defaultValue);
    const auto identify = [&section, &field](QWidget* editor) {
        editor->setProperty("settingSection", section);
        editor->setProperty("settingKey", field.key);
        return editor;
    };
    // Rebuild requests are queued so the editor emitting the signal is never
    // destroyed mid-emit; rebuildWorkspace preserves page and scroll position.
    const auto queueRebuild = [this] {
        QTimer::singleShot(0, this, [this] { rebuildWorkspace(m_currentPage); });
    };
    if (field.type == FieldType::Number) {
        auto* editor = new DragNumericEdit(this);
        editor->setRange(field.minimum, field.maximum);
        editor->setSingleStep(field.step);
        editor->setDragSensitivity(field.step);
        editor->setDecimals(field.decimals);
        editor->setSuffix(field.suffix);
        editor->setValue(value.toDouble(field.defaultValue.toDouble()));
        installDefaultReset(editor, field.defaultValue);
        connect(editor, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, section, key = field.key, rebuild = field.rebuildOnChange, queueRebuild](double number) {
            applyDocumentChange(tr("Edit %1/%2").arg(section, key), [this, section, key, number] { m_document.setSetting(section, key, number); });
            if (rebuild) queueRebuild();
        });
        return identify(editor);
    }
    if (field.type == FieldType::Toggle) {
        auto* editor = new QCheckBox(this);
        editor->setChecked(value.toBool(field.defaultValue.toBool()));
        installDefaultReset(editor, field.defaultValue);
        connect(editor, &QCheckBox::toggled, this, [this, section, key = field.key, rebuild = field.rebuildOnChange, queueRebuild](bool checked) {
            applyDocumentChange(tr("Toggle %1/%2").arg(section, key), [this, section, key, checked] { m_document.setSetting(section, key, checked); });
            if (rebuild || key == QStringLiteral("expertMode") || key == QStringLiteral("specularAa")
                || (section == QStringLiteral("Landscape/AntiTiling") && key == QStringLiteral("enabled"))) queueRebuild();
        });
        return identify(editor);
    }
    if (field.type == FieldType::Choice) {
        auto* editor = new QComboBox(this);
        WheelFocusGuard::install(editor);
        editor->addItems(field.choices);
        editor->setCurrentText(value.toString(field.defaultValue.toString()));
        installDefaultReset(editor, field.defaultValue);
        connect(editor, &QComboBox::currentTextChanged, this, [this, section, key = field.key, rebuild = field.rebuildOnChange, queueRebuild](const QString& text) {
            applyDocumentChange(tr("Change %1/%2").arg(section, key), [this, section, key, text] { m_document.setSetting(section, key, text); });
            if (rebuild
                || (section == QStringLiteral("Geometry/Source") && key == QStringLiteral("source"))
                || section == QStringLiteral("Transparency/Composition")
                || (section == QStringLiteral("Transparency/Depth") && key == QStringLiteral("sort"))
                || section == QStringLiteral("Shading/Subsurface")
                || section.startsWith(QStringLiteral("DynamicNormals/"))
                || section == QStringLiteral("Vegetation/Profile")
                || section == QStringLiteral("UV1/Composition")
                || section == QStringLiteral("UV2/Composition")
                || ((section == QStringLiteral("UV0") || section == QStringLiteral("UV1") || section == QStringLiteral("UV2")) && key == QStringLiteral("source")))
                queueRebuild();
        });
        return identify(editor);
    }
    if (field.type == FieldType::Color) {
        auto* editor = new ColorEdit(QColor(value.toString(field.defaultValue.toString())), field.colorAlpha, this);
        installDefaultReset(editor, field.defaultValue);
        connect(editor, &ColorEdit::colorChanged, this, [this, section, key = field.key, alpha = field.colorAlpha, rebuild = field.rebuildOnChange, queueRebuild](const QColor& color) {
            const QString text = color.name(alpha ? QColor::HexArgb : QColor::HexRgb).toUpper();
            applyDocumentChange(tr("Change %1/%2 color").arg(section, key), [this, section, key, text] { m_document.setSetting(section, key, text); });
            if (rebuild) queueRebuild();
        });
        return identify(editor);
    }
    // Text commits on editing-finished (matching the texture path field), not
    // per keystroke: one document change and one undo entry per edit.
    auto* editor = new QLineEdit(value.toString(field.defaultValue.toString()), this);
    installDefaultReset(editor, field.defaultValue);
    connect(editor, &QLineEdit::editingFinished, this, [this, editor, section, key = field.key, fallback = field.defaultValue, rebuild = field.rebuildOnChange, queueRebuild] {
        const QString text = editor->text();
        if (m_document.setting(section, key, fallback).toString(fallback.toString()) == text) return;
        applyDocumentChange(tr("Edit %1/%2").arg(section, key), [this, section, key, text] { m_document.setSetting(section, key, text); });
        if (rebuild) queueRebuild();
    });
    return identify(editor);
}


QWidget* MainWindow::createShadingPage()
{
    const auto mode = m_document.shaderMode();
    auto page = createPage(tr("%1 Shading").arg(Pbr::shaderModeName(mode)), Pbr::shaderModeDescription(mode));
    if (mode == Pbr::ShaderMode::UnlitEffect) {
        addSettingsCard(page.content, QStringLiteral("Shading/Unlit"), tr("Unlit output"), {
            numberField("intensity", "Output intensity", 1.0, 0.0, 1000.0, 0.01, "Scales the final unlit color."),
            toggleField("vertexColor", "Multiply vertex color", true, "Multiplies the output by particle or mesh vertex color."),
            toggleField("exposure", "Apply scene exposure", true, "Passes the unlit result through scene exposure and tone mapping.")
        });
    } else {
        // v6 specular model: the IOR scalar is the fallback when no IOR map is
        // declared on the specular colour slot; the ceiling is what texel 255
        // of such a map means. Raising the ceiling rescales the whole map.
        addSettingsCard(page.content, QStringLiteral("Specular"), tr("Specular"), {
            numberField("ior", "Specular IOR", 1.5, 0.0, 100.0, 0.001, "OpenPBR specular_ior. 1.5 is the dielectric default (F0 0.04); 0 makes the specular color the reflectance outright. Ignored where the specular color map declares an IOR alpha."),
            numberField("iorMax", "IOR map ceiling", 4.25, 0.01, 100.0, 0.01, "The IOR that texel 255 of an IOR map stands for. 4.25 puts IOR 1.5 exactly on texel 90.")
        });
        addSettingsCard(page.content, QStringLiteral("Shading/Core"), tr("Core shading"), {
            choiceField("normalSpace", "Normal space", "Tangent", {"Tangent", "Model", "World"}, "Space used to decode the primary normal."),
            numberField("normalStrength", "Final normal multiplier", 1.0, 0.0, 8.0, 0.01, "Final multiplier applied after every texture-specific normal strength."),
            toggleField("twoSided", "Two sided", mode == Pbr::ShaderMode::VegetationSurface || mode == Pbr::ShaderMode::HairSurface, "Renders both sides of thin geometry."),
            toggleField("energyConservation", "Energy conservation", true, "Prevents diffuse and specular lobes from producing more reflected energy than they receive."),
            toggleField("specularAa", "Specular anti-aliasing", true, "Folds normal-map variance into roughness at distance so dense detail stops sparkling and crawling."),
            numberField("specularAaStrength", "Specular AA strength", 1.0, 0.0, 4.0, 0.01, "Scales how much screen-space normal variance widens the specular lobe.")
        });
        setSettingEditorsEnabled(page.page, QStringLiteral("Shading/Core"), {QStringLiteral("specularAaStrength")},
            m_document.setting(QStringLiteral("Shading/Core"), QStringLiteral("specularAa"), true).toBool());
    }

    if (mode == Pbr::ShaderMode::HairSurface || mode == Pbr::ShaderMode::AllFeatures) {
        addSettingsCard(page.content, QStringLiteral("Shading/Hair"), tr("Hair lobes"), {
            choiceField("direction", "Direction source", "Hair Direction RG", {"Hair Direction RG", "Mesh Tangent"}, "Strand direction around which highlights stretch."),
            numberField("primaryStrength", "Primary strength", 1.0, 0.0, 8.0, 0.01, "Strength of the sharp primary hair highlight."),
            numberField("primaryRoughness", "Primary roughness", 0.25, 0.0, 1.0, 0.005, "Width of the primary hair highlight."),
            numberField("primaryShift", "Primary shift", -0.08, -1.0, 1.0, 0.002, "Moves the primary highlight along the strand direction."),
            numberField("secondaryStrength", "Secondary strength", 0.5, 0.0, 8.0, 0.01, "Strength of the broad colored secondary highlight."),
            numberField("secondaryRoughness", "Secondary roughness", 0.45, 0.0, 1.0, 0.005, "Width of the secondary highlight."),
            numberField("secondaryShift", "Secondary shift", 0.12, -1.0, 1.0, 0.002, "Moves the secondary highlight along the strand direction."),
            numberField("scatter", "Scatter", 0.5, 0.0, 4.0, 0.01, "Light scattered through the hair fiber volume."),
            numberField("backlight", "Backlight", 0.5, 0.0, 4.0, 0.01, "Transmission visible when light is behind hair cards.")
        });
    }
    if (mode == Pbr::ShaderMode::GlassSurface || mode == Pbr::ShaderMode::AllFeatures) {
        addSettingsCard(page.content, QStringLiteral("Shading/Glass"), tr("Glass transmission"), {
            numberField("ior", "Index of refraction", 1.5, 1.0, 3.0, 0.001, "Physical glass IOR."),
            numberField("absorptionDistance", "Absorption distance", 1.0, 0.0001, 100000.0, 0.01, "World-space distance at which the authored absorption color is reached."),
            numberField("refractionRoughness", "Refraction roughness", 0.0, 0.0, 1.0, 0.005, "Blurs transmitted scene radiance."),
            toggleField("thinWalled", "Thin walled", false, "Treats geometry as an infinitely thin sheet when thickness is unavailable.")
        });
        addSettingsCard(page.content, QStringLiteral("Shading/BottleLiquid"), tr("Bottle liquid"), {
            toggleField("enabled", "Liquid in the container", false, "Fakes liquid with a clipped fill plane, sloshing spring, meniscus, and depth absorption — no fluid simulation. The runtime drives the slosh from object motion; the preview reacts to the Object Transform controls."),
            numberField("fillLevel", "Fill level", 0.6, 0.0, 1.0, 0.005, "Liquid height as a fraction of the mesh bounds."),
            colorField("color", "Liquid color", "#7A3B12", "Liquid tint (Nuka-Cola amber by default)."),
            numberField("absorption", "Depth darkening", 0.6, 0.0, 1.0, 0.005, "How quickly the liquid darkens with depth below the surface."),
            colorField("meniscusColor", "Meniscus color", "#D9945A", "Bright line where the liquid surface meets the glass."),
            numberField("meniscusWidth", "Meniscus width", 0.02, 0.001, 0.25, 0.001, "Height of the meniscus band."),
            numberField("surfaceBrightness", "Surface brightness", 1.15, 0.0, 4.0, 0.01, "Shading boost of the flat liquid surface cap."),
            numberField("wobbleFrequency", "Wobble frequency", 4.0, 0.5, 20.0, 0.05, "Spring frequency of the slosh oscillation."),
            numberField("wobbleDamping", "Wobble damping", 0.25, 0.01, 2.0, 0.005, "How quickly sloshing settles."),
            numberField("maxTilt", "Max tilt", 25.0, 0.0, 60.0, 0.5, "Clamp on the liquid surface tilt.", 1, "°")
        });
    }
    if (mode == Pbr::ShaderMode::WaterSurface || mode == Pbr::ShaderMode::AllFeatures) {
        addSettingsCard(page.content, QStringLiteral("Shading/Water"), tr("Water optics"), {
            numberField("ior", "Index of refraction", 1.333, 1.0, 2.0, 0.001, "Physical water IOR."),
            numberField("absorptionDistance", "Absorption distance", 300.0, 0.01, 100000.0, 1.0, "Depth over which water reaches its absorption color."),
            numberField("scattering", "Scattering", 0.25, 0.0, 10.0, 0.01, "Light redirected through suspended material."),
            numberField("reflectionStrength", "Reflection strength", 1.0, 0.0, 4.0, 0.01, "Scales environment and screen-space reflection."),
            toggleField("shoreFoam", "Shore foam", true, "Generates foam from water/scene depth intersections.")
        });
    }
    if (mode == Pbr::ShaderMode::StandardSurface || mode == Pbr::ShaderMode::AdvancedSubsurface || mode == Pbr::ShaderMode::VegetationSurface || mode == Pbr::ShaderMode::AllFeatures) {
        addSettingsCard(page.content, QStringLiteral("Shading/Subsurface"), tr("Subsurface and transmission"), {
            choiceField("mode", "Mode", mode == Pbr::ShaderMode::VegetationSurface ? "Thin Transmission" : "Diffusion Profile", {"Disabled", "Diffusion Profile", "Thin Transmission", "Combined"}, "Selects thick diffusion, thin transmission, or both."),
            choiceField("profile", "Diffusion profile", "Skin", {"Skin", "Wax", "Flesh", "Plant", "Custom"}, "Profile defining wavelength-dependent scatter radius."),
            colorField("customScatterColor", "Custom scatter color", "#FFB39D", "Wavelength tint used when Diffusion profile is Custom."),
            numberField("customRadiusR", "Custom radius R", 1.0, 0.001, 1000.0, 0.01, "Red-channel diffusion radius for a Custom profile."),
            numberField("customRadiusG", "Custom radius G", 0.35, 0.001, 1000.0, 0.01, "Green-channel diffusion radius for a Custom profile."),
            numberField("customRadiusB", "Custom radius B", 0.2, 0.001, 1000.0, 0.01, "Blue-channel diffusion radius for a Custom profile."),
            numberField("weight", "Scatter weight", 1.0, 0.0, 4.0, 0.01, "Scales profile-driven scattering."),
            numberField("transmission", "Transmission", 0.0, 0.0, 4.0, 0.01, "Scales light passing through the surface."),
            colorField("transmissionTint", "Transmission tint", "#FFFFFF", "Multiplies the sampled scatter color for the thin-transmission lobe only, so backlight can be tinted independently of diffusion (e.g. a warm leaf backlight over green diffusion)."),
            numberField("thicknessScale", "Thickness scale", 1.0, 0.0, 1000.0, 0.01, "Scales texture or mesh thickness.")
        });
        const bool custom = m_document.setting(QStringLiteral("Shading/Subsurface"), QStringLiteral("profile"), QStringLiteral("Skin")).toString() == QStringLiteral("Custom");
        setSettingEditorsEnabled(page.page, QStringLiteral("Shading/Subsurface"),
            {QStringLiteral("customScatterColor"), QStringLiteral("customRadiusR"), QStringLiteral("customRadiusG"), QStringLiteral("customRadiusB")}, custom);
        const QString subsurfaceMode = m_document.setting(QStringLiteral("Shading/Subsurface"), QStringLiteral("mode"),
            mode == Pbr::ShaderMode::VegetationSurface ? QStringLiteral("Thin Transmission") : QStringLiteral("Diffusion Profile")).toString();
        setSettingEditorsEnabled(page.page, QStringLiteral("Shading/Subsurface"), {QStringLiteral("transmissionTint")},
            subsurfaceMode == QStringLiteral("Thin Transmission") || subsurfaceMode == QStringLiteral("Combined"));
    }
    if (mode == Pbr::ShaderMode::EyeSurface || mode == Pbr::ShaderMode::AllFeatures) {
        addSettingsCard(page.content, QStringLiteral("Shading/Eye"), tr("Cornea and iris"), {
            numberField("corneaIor", "Cornea IOR", 1.376, 1.0, 2.0, 0.001, "Physical cornea index of refraction."),
            numberField("irisDepth", "Iris depth", 0.08, 0.0, 1.0, 0.001, "Distance of the iris below the cornea."),
            numberField("limbalStrength", "Limbal strength", 1.0, 0.0, 4.0, 0.01, "Dark ring around the outside of the iris."),
            numberField("pupilScale", "Pupil scale", 1.0, 0.0, 4.0, 0.01, "Runtime-controllable pupil dilation."),
            numberField("wetness", "Wet reflection", 1.0, 0.0, 4.0, 0.01, "Strength of the clear wet outer lobe.")
        });
    }
    if (mode == Pbr::ShaderMode::StandardSurface || mode == Pbr::ShaderMode::AllFeatures) {
        addSettingsCard(page.content, QStringLiteral("Shading/SurfaceEffects"), tr("Sheen, glints, and iridescence"), {
            numberField("sheenStrength", "Sheen strength", 0.0, 0.0, 8.0, 0.01, "Grazing fabric/fuzz reflection masked by Surface Effects R."),
            numberField("sheenRoughness", "Sheen roughness", 0.6, 0.0, 1.0, 0.005, "Width of the sheen lobe."),
            numberField("glintIntensity", "Glint intensity", 0.0, 0.0, 100.0, 0.01, "Sparse microfacet sparkle masked by Surface Effects G."),
            numberField("glintScale", "Glint scale", 1.0, 0.001, 10000.0, 0.01, "Spatial frequency of procedural glints."),
            numberField("filmIor", "Thin-film IOR", 1.5, 1.0, 3.0, 0.001, "IOR of the iridescent thin film."),
            numberField("iridescenceStrength", "Iridescence strength", 0.0, 0.0, 4.0, 0.01, "Angle-dependent thin-film interference masked by Surface Effects A."),
            numberField("retroStrength", "Retroreflection", 0.0, 0.0, 8.0, 0.01, "Bounces light back toward its source (road signs, safety tape, cat-eye markers). Strongest when the viewer is near the light, e.g. a Pip-Boy lamp at night."),
            numberField("retroWidth", "Retroreflection width", 0.25, 0.0, 1.0, 0.005, "Angular width of the retroreflective lobe.")
        });
    }
    page.content->addStretch();
    return page.page;
}

QWidget* MainWindow::createGeometryPage()
{
    const bool landscape = m_document.shaderMode() == Pbr::ShaderMode::LandscapeSurface;
    auto page = createPage(tr("Height & Geometry"), tr("The Separate Height texture is authored in Primary or Secondary UV. This page selects the source and controls the algorithms that consume it."));
    const QStringList sources = landscape
        ? QStringList{"None", "Landscape Normal Blue", "Landscape Separate Height"}
        : QStringList{"None", "Primary Normal Blue", "Primary Separate Height", "Secondary Normal Blue", "Secondary Separate Height"};
    addSettingsCard(page.content, QStringLiteral("Geometry/Source"), tr("Shared height source"), {
        choiceField("source", "Source", "None", sources, "Selects valid height data used by parallax and displacement."),
        toggleField("invert", "Invert", false, "Reverses high and low height values."),
        numberField("center", "Center", 0.5, 0.0, 1.0, 0.005, "Texture value treated as the undisplaced surface."),
        numberField("bias", "Bias", 0.0, -1.0, 1.0, 0.002, "Adds a constant to decoded height.")
    });
    addSettingsCard(page.content, QStringLiteral("Geometry/Parallax"), tr("Parallax mapping"), {
        toggleField("enabled", "Enabled", false, "Offsets texture sampling according to view direction. Existing materials migrate to Offset mode."),
        choiceField("mode", "Algorithm", "Offset", {"Offset", "Parallax Occlusion Mapping", "Self-Shadowed POM"}, "Offset is one sample; POM ray-marches the height field; Self-Shadowed POM also traces the key light."),
        choiceField("quality", "Quality", "High", {"Low", "Medium", "High", "Ultra"}, "Controls the adaptive view and light ray step counts unless Custom quality is enabled."),
        numberField("depth", "Depth scale", 0.05, 0.0, 1.0, 0.001, "Apparent parallax depth in tangent space."),
        numberField("fadeStart", "Fade start", 512.0, 0.0, 1000000.0, 1.0, "Distance where parallax begins fading."),
        numberField("fadeEnd", "Fade end", 2048.0, 0.0, 1000000.0, 1.0, "Distance where parallax is disabled.")
    });
    addSettingsCard(page.content, QStringLiteral("Geometry/Parallax"), tr("POM quality overrides"), {
        toggleField("customQuality", "Custom quality", false, "Uses the expert step counts below instead of the selected quality preset."),
        numberField("minSteps", "Minimum view steps", 12.0, 1.0, 64.0, 1.0, "Ray steps used when looking straight at the surface.", 0),
        numberField("maxSteps", "Maximum view steps", 40.0, 2.0, 128.0, 1.0, "Ray steps used at grazing view angles.", 0),
        numberField("refinementSteps", "Refinement steps", 3.0, 0.0, 8.0, 1.0, "Binary-search steps that refine the POM intersection.", 0),
        numberField("grazingFade", "Grazing-angle fade", 0.08, 0.0, 1.0, 0.005, "Fades extreme offsets near a tangent view to reduce stretching."),
    });
    addSettingsCard(page.content, QStringLiteral("Geometry/Parallax"), tr("Self-shadowing and integration"), {
        numberField("shadowSteps", "Light trace steps", 20.0, 1.0, 64.0, 1.0, "Height-field samples used by Self-Shadowed POM.", 0),
        numberField("shadowStrength", "Self-shadow strength", 1.0, 0.0, 1.0, 0.01, "Darkening applied to key-light visibility in Self-Shadowed POM."),
        numberField("shadowBias", "Self-shadow bias", 0.015, 0.0, 0.25, 0.001, "Separates the light ray from the sampled surface to reduce self-intersection."),
        numberField("shadowSoftness", "Self-shadow softness", 0.08, 0.0, 1.0, 0.005, "Softens height-field shadow transitions."),
        toggleField("silhouetteClipping", "Clip UV silhouette", false, "Discards fragments whose displaced UV leaves the 0-1 tile; useful for isolated cards, not repeating surfaces."),
        toggleField("depthOutput", "Write parallax depth", false, "Requests runtime parallax depth output for contact effects. Preview lighting does not require it.")
    });
    addSettingsCard(page.content, QStringLiteral("Geometry/Displacement"), tr("Displacement"), {
        toggleField("enabled", "Enabled", false, "Moves geometry vertices and can change the silhouette."),
        numberField("strength", "Strength", 0.0, -1000.0, 1000.0, 0.001, "World-space displacement magnitude."),
        choiceField("subdivision", "Subdivision", "Renderer Controlled", {"Renderer Controlled", "Mesh Vertices", "Tessellation", "Mesh Shader"}, "Geometry density used for displacement."),
        numberField("fadeEnd", "Fade end", 1024.0, 0.0, 1000000.0, 1.0, "Distance where true displacement is disabled.")
    });
    const QString heightSource = m_document.setting(QStringLiteral("Geometry/Source"), QStringLiteral("source"), QStringLiteral("None")).toString();
    bool validHeightSource = false;
    if (heightSource != QStringLiteral("None")) {
        const bool landscapeSource = heightSource.startsWith(QStringLiteral("Landscape"));
        const bool secondary = heightSource.startsWith(QStringLiteral("Secondary"));
        const bool normalBlue = heightSource.endsWith(QStringLiteral("Normal Blue"));
        const QString set = landscapeSource ? QStringLiteral("Landscape") : (secondary ? QStringLiteral("UV1") : QStringLiteral("UV0"));
        const QString prefix = secondary ? QStringLiteral("secondary") : QStringLiteral("primary");
        const QString key = landscapeSource
            ? (normalBlue ? QStringLiteral("normalHeightArray") : QStringLiteral("heightArray"))
            : prefix + (normalBlue ? QStringLiteral("Normal") : QStringLiteral("SeparateHeight"));
        const Pbr::TextureSlotState state = m_document.textureSlot(set, key);
        const bool featureEnabled = !normalBlue || state.values.value(QStringLiteral("heightInBlue")).toBool(false);
        const bool constantOverride = state.values.value(QStringLiteral("overrideHeight")).toBool(true);
        validHeightSource = state.enabled && featureEnabled
            && (constantOverride || !TextureImage::resolvePath(state.path, m_textureRoot).isEmpty());
    }
    for (QFrame* card : page.page->findChildren<QFrame*>(QStringLiteral("Card"), Qt::FindChildrenRecursively)) {
        const QString section = card->property("settingSection").toString();
        if (section.startsWith(QStringLiteral("Geometry/")) && section != QStringLiteral("Geometry/Source")) {
            card->setEnabled(validHeightSource);
            if (!validHeightSource) card->setToolTip(tr("Choose a valid height source first. Normal Blue also requires 'B contains height' on that Normal input."));
        }
    }
    page.content->addStretch();
    return page.page;
}

QWidget* MainWindow::createDynamicNormalsPage()
{
    auto page = createPage(tr("Dynamic Normals"), tr("Dynamic Normal texture sockets live in the UV pages. This page defines what drives them and how they blend."));
    const QVector<FieldSpec> driverFields{
        choiceField("source", "Source", "Facial Expression", {"Facial Expression", "Animation Parameter", "Vertex Value", "Constant"}, "Runtime value controlling the dynamic normal contribution."),
        numberField("constantValue", "Constant value", 1.0, -1000.0, 1000.0, 0.01, "Driver input used when Source is Constant."),
        textField("parameter", "Parameter", "ExpressionWeight", "Animation or runtime parameter name."),
        numberField("minimum", "Input minimum", 0.0, -1000.0, 1000.0, 0.01, "Driver value mapped to zero response."),
        numberField("maximum", "Input maximum", 1.0, -1000.0, 1000.0, 0.01, "Driver value mapped to full response."),
        toggleField("invert", "Invert", false, "Reverses driver response."),
        choiceField("blend", "Blend", "Reoriented", {"Reoriented", "Whiteout", "Linear"}, "Normal-combination method.")
    };
    addSettingsCard(page.content, QStringLiteral("DynamicNormals/Primary"), tr("Primary UV driver"), driverFields);
    if (m_document.secondaryUvEnabled())
        addSettingsCard(page.content, QStringLiteral("DynamicNormals/Secondary"), tr("Secondary UV driver"), driverFields);
    if (m_document.tertiaryUvEnabled())
        addSettingsCard(page.content, QStringLiteral("DynamicNormals/Tertiary"), tr("Tertiary UV driver"), driverFields);
    for (const QString& section : {QStringLiteral("DynamicNormals/Primary"), QStringLiteral("DynamicNormals/Secondary"), QStringLiteral("DynamicNormals/Tertiary")}) {
        const QString source = m_document.setting(section, QStringLiteral("source"), QStringLiteral("Facial Expression")).toString();
        setSettingEditorsEnabled(page.page, section, {QStringLiteral("constantValue")}, source == QStringLiteral("Constant"));
        setSettingEditorsEnabled(page.page, section, {QStringLiteral("parameter")}, source == QStringLiteral("Animation Parameter"));
    }
    page.content->addStretch();
    return page.page;
}

QWidget* MainWindow::createVegetationPage()
{
    auto page = createPage(tr("Vegetation Animation"), tr("Scene-coherent wind deformation driven by vertex-color weights and shared world wind."));
    addSettingsCard(page.content, QStringLiteral("Vegetation/Profile"), tr("Profile and wind source"), {
        choiceField("profile", "Profile", "Leaves", {"Grass", "Leaves", "Branches", "Custom"}, "Selects tuned defaults for the vegetation type."),
        choiceField("source", "Wind source", "Scene Wind", {"Scene Wind", "Material Override"}, "Scene Wind keeps nearby assets moving coherently."),
        numberField("direction", "Override direction", 0.0, -180.0, 180.0, 1.0, "World-space wind heading used by Material Override.", 2, "°"),
        numberField("strength", "Strength multiplier", 1.0, 0.0, 10.0, 0.01, "Scales the shared wind strength."),
        numberField("timeScale", "Time scale", 1.0, 0.0, 10.0, 0.01, "Scales all vegetation animation speed."),
        numberField("gustFrequency", "Override gust frequency", 0.2, 0.0, 100.0, 0.01, "Gust cycles per second used by Material Override."),
        numberField("gustPhase", "Override gust phase", 0.0, -1000.0, 1000.0, 0.01, "Per-material phase offset for overridden gust timing.")
    });
    const bool overrideWind = m_document.setting(QStringLiteral("Vegetation/Profile"), QStringLiteral("source"), QStringLiteral("Scene Wind")).toString() == QStringLiteral("Material Override");
    setSettingEditorsEnabled(page.page, QStringLiteral("Vegetation/Profile"),
        {QStringLiteral("direction"), QStringLiteral("gustFrequency"), QStringLiteral("gustPhase")}, overrideWind);
    addSettingsCard(page.content, QStringLiteral("Vegetation/Weights"), tr("Vertex-color weights"), {
        textField("red", "Red", "Primary sway", "Broad low-frequency movement."),
        textField("green", "Green", "Branch bending", "Secondary branch or stem motion."),
        textField("blue", "Blue", "Leaf / edge flutter", "High-frequency edge movement."),
        textField("alpha", "Alpha", "Anchor / stiffness", "Zero moves freely; one remains anchored.")
    });
    addSettingsCard(page.content, QStringLiteral("Vegetation/Motion"), tr("Motion"), {
        numberField("swayAmount", "Sway amount", 1.0, 0.0, 10.0, 0.01, "Primary displacement amplitude."),
        numberField("swaySpeed", "Sway speed", 1.0, 0.0, 20.0, 0.01, "Primary oscillation speed."),
        numberField("branchAmount", "Branch amount", 0.35, 0.0, 10.0, 0.01, "Branch-bending amplitude."),
        numberField("flutterAmount", "Flutter amount", 0.2, 0.0, 10.0, 0.01, "High-frequency leaf/edge movement."),
        numberField("flutterSpeed", "Flutter speed", 4.0, 0.0, 100.0, 0.05, "Flutter frequency."),
        numberField("gustStrength", "Gust strength", 0.4, 0.0, 10.0, 0.01, "Intermittent wind amplification."),
        numberField("turbulence", "Turbulence", 0.5, 0.0, 10.0, 0.01, "World-space wind noise contribution.")
    });
    page.content->addStretch();
    return page.page;
}

QWidget* MainWindow::createDecalsPage()
{
    auto page = createPage(tr("Projected Decal"), tr("Projects the current material inputs onto existing geometry; it does not introduce a separate texture system."));
    addSettingsCard(page.content, QStringLiteral("Decal"), tr("Projection"), {
        toggleField("enabled", "Enabled", false, "Renders the material through the projected-decal path."),
        choiceField("blend", "Blend", "Normal", {"Normal", "Modulate", "Additive"}, "How the decal combines with the receiving surface."),
        numberField("depthBias", "Depth bias", 0.001, -1.0, 1.0, 0.0001, "Separates the decal from the receiver to avoid z-fighting."),
        numberField("angleFade", "Angle fade", 0.25, 0.0, 1.0, 0.005, "Fades projection at grazing angles."),
        numberField("distanceFade", "Distance fade", 0.0, 0.0, 1000000.0, 1.0, "Optional far-distance fade.")
    });
    addSettingsCard(page.content, QStringLiteral("Decal/Channels"), tr("Channel participation"), {
        toggleField("base", "Base Color", true, "Projects base color."),
        toggleField("normal", "Normal", true, "Projects and blends normal detail."),
        toggleField("roughness", "Roughness", true, "Projects roughness."),
        toggleField("metallic", "Metallic", false, "Projects metallic response."),
        toggleField("ao", "Ambient Occlusion", false, "Projects local indirect occlusion."),
        toggleField("emissive", "Emissive", false, "Projects emission.")
    });
    page.content->addStretch();
    return page.page;
}

QWidget* MainWindow::createParticlesPage()
{
    auto page = createPage(tr("Particles"), tr("Composites simple through advanced particles from the UV layer stack: per-layer flipbooks and polar coordinates, modifier layers that distort and erode the primary layer, palette remapping, per-particle variation, and view fades. The runtime drives particle age and seed; the preview simulates them from the viewport's Particle Simulation controls."));
    addInfoCard(page.content, tr("Layer stack"),
        tr("The Primary layer is the particle's color. Secondary and Tertiary layers either composite color with their Layer blend mode, or act as modifiers whose RG field distorts and whose alpha erodes the Primary layer."));
    const auto flipbookFields = [] {
        return QVector<FieldSpec>{
            toggleField("flipbookEnabled", "Flipbook", false, "Treats this layer's textures as an atlas of animation frames."),
            numberField("flipbookColumns", "Columns", 4.0, 1.0, 64.0, 1.0, "Frames across the atlas horizontally.", 0),
            numberField("flipbookRows", "Rows", 4.0, 1.0, 64.0, 1.0, "Frames across the atlas vertically.", 0),
            choiceField("flipbookDriver", "Playback driver", "Particle Age", {"Particle Age", "Global Time", "Constant"}, "What advances the frame: particle age plays once across the lifetime, global time free-runs, constant picks one frame."),
            numberField("flipbookFps", "Frames per second", 0.0, 0.0, 240.0, 0.5, "Playback rate for Global Time. Zero with Particle Age spreads all frames over the particle lifetime.", 1),
            choiceField("flipbookLoop", "Loop mode", "Loop", {"Loop", "Clamp", "Ping-Pong"}, "Behavior when playback passes the last frame."),
            toggleField("flipbookBlend", "Blend between frames", true, "Cross-fades the layer's color between adjacent frames for smooth playback."),
            numberField("flipbookConstantFrame", "Constant frame", 0.0, 0.0, 4095.0, 1.0, "Frame used when the driver is Constant.", 0),
            toggleField("polarUv", "Polar coordinates", false, "Remaps this layer's UVs to polar space: scroll U becomes rotation around the center, scroll V becomes expanding rings — shockwaves, muzzle rings, portals.")
        };
    };
    const auto modifierFields = [] {
        return QVector<FieldSpec>{
            choiceField("role", "Layer role", "Color", {"Color", "Modifier"}, "Color layers composite with their Layer blend mode. Modifier layers do not draw; their RG field distorts and their alpha erodes the Primary layer."),
            numberField("distortStrength", "Distortion strength", 0.0, 0.0, 2.0, 0.005, "How far this modifier's RG field (0.5/0.5 neutral) pushes the Primary layer's sample coordinates."),
            numberField("advectSpeed", "Flow advection speed", 0.0, 0.0, 10.0, 0.01, "Advects the Primary layer along the modifier's RG flow field with two-phase crossfade sampling — rolling smoke, licking flames. Zero keeps the distortion static."),
            toggleField("erodeEnabled", "Erode Primary over age", false, "Dissolves the Primary layer as the particle ages, using this modifier's alpha as the erosion pattern."),
            numberField("erodeSoftness", "Erosion softness", 0.15, 0.005, 1.0, 0.005, "Width of the dissolve edge."),
            colorField("erodeEdgeTint", "Erosion edge tint", "#FF7A26", "Glow color at the erosion front (burning paper edges)."),
            numberField("erodeEdgeGlow", "Erosion edge glow", 0.0, 0.0, 32.0, 0.05, "Emissive strength of the erosion front.")
        };
    };
    addGroupTitle(page.content, tr("PRIMARY LAYER (UV0)"));
    addSettingsCard(page.content, QStringLiteral("Particles/Layer0"), tr("Primary flipbook & coordinates"), flipbookFields());
    addGroupTitle(page.content, tr("SECONDARY LAYER (UV1)"));
    addSettingsCard(page.content, QStringLiteral("Particles/Layer1"), tr("Secondary role"), modifierFields());
    addSettingsCard(page.content, QStringLiteral("Particles/Layer1"), tr("Secondary flipbook & coordinates"), flipbookFields());
    addGroupTitle(page.content, tr("TERTIARY LAYER (UV2)"));
    addSettingsCard(page.content, QStringLiteral("Particles/Layer2"), tr("Tertiary role"), modifierFields());
    addSettingsCard(page.content, QStringLiteral("Particles/Layer2"), tr("Tertiary flipbook & coordinates"), flipbookFields());
    addGroupTitle(page.content, tr("PARTICLE APPEARANCE"));
    addSettingsCard(page.content, QStringLiteral("Particles/Global"), tr("Palette, variation, and fades"), {
        choiceField("paletteMode", "Palette remap", "Off", {"Off", "Gradient Ramp", "Blackbody Ramp"}, "Remaps the composited color's luminance through a palette: the Particle Gradient texture, or a procedural blackbody ramp (grayscale fire becomes recolorable)."),
        numberField("paletteIntensity", "Palette intensity", 1.0, 0.0, 32.0, 0.01, "Scales the remapped color."),
        toggleField("randomFlipbookStart", "Random start frame", false, "Offsets flipbook playback by a per-particle random frame so emitters do not show synchronized clones."),
        numberField("randomPhase", "Random phase", 0.0, 0.0, 1.0, 0.005, "Per-particle offset applied to UV animation and advection phases."),
        numberField("randomHueJitter", "Hue jitter", 0.0, 0.0, 1.0, 0.005, "Per-particle hue variation of the composited color."),
        numberField("fresnelFade", "Edge-on fade", 0.0, 0.0, 1.0, 0.005, "Fades the particle as its surface turns edge-on to the camera, hiding flat cards seen from the side."),
        numberField("fresnelFadePower", "Edge-on fade power", 2.0, 0.25, 16.0, 0.05, "Sharpness of the edge-on fade."),
        numberField("nearFade", "Camera near fade", 0.0, 0.0, 10.0, 0.01, "Distance below which the particle dissolves instead of clipping through the camera.")
    });
    page.content->addStretch();
    return page.page;
}

QWidget* MainWindow::createSurfaceResponsePage()
{
    auto page = createPage(tr("Surface Response"), tr("How this surface responds to runtime states (rain, dust, frost, heat, blood). State behavior is runtime-owned and defined once globally; the material contributes physical maps — porosity in RMAOS alpha, signed curvature in Normal alpha, plus AO, height, and roughness — and the few bindings below."));
    addInfoCard(page.content, tr("Runtime ownership"),
        tr("Response profiles and state amounts live in the engine's surface state definitions (FO4.PBRM.SurfaceStateDefinitions), never in the material. Use the preview's States tab to simulate them; Settings > Surface State Definitions… selects the previewed set."));
    // Read-only summary of the effective physical inputs, with pointers at
    // the slots that own them.
    const auto rmaos = m_document.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryRmaos"));
    const auto normal = m_document.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryNormal"));
    const bool alphaPorosity = rmaos.values.value(QStringLiteral("alphaCarries")).toString(QStringLiteral("Specular Weight")) == QLatin1String("Porosity");
    const bool porosityOverride = rmaos.values.value(QStringLiteral("overridePorosity")).toBool(false);
    const QString porositySource = porosityOverride
        ? tr("constant %1").arg(rmaos.values.value(QStringLiteral("porosity")).toDouble(.5))
        : (alphaPorosity && !rmaos.path.trimmed().isEmpty()
            ? tr("RMAOS alpha — %1").arg(rmaos.path)
            : tr("derived — saturate(roughness × (1 − metallic))"));
    const bool curvatureAuthored = normal.values.value(QStringLiteral("alphaCarries")).toString(QStringLiteral("None")) == QLatin1String("Curvature") && !normal.path.trimmed().isEmpty();
    const QString curvatureSource = curvatureAuthored
        ? tr("Normal alpha — %1").arg(normal.path)
        : tr("derived from AO (concave only: no edge shedding, no cavity specular occlusion)");
    addInfoCard(page.content, tr("Effective physical inputs"),
        tr("Porosity: %1\nCurvature: %2\nConfigure both on the Primary UV Textures page (RMAOS and Normal + Height inputs).").arg(porositySource, curvatureSource));
    addSettingsCard(page.content, QStringLiteral("SurfaceResponse"), tr("Response bindings"), {
        numberField("adhesion", "Adhesion", 1.0, 0.0, 1.0, 0.005, "Where states can settle at all: 1 = ordinary surface, 0 = treated surface that sheds everything (oiled machinery, force fields)."),
        choiceField("shelterChannel", "Shelter vertex channel", "None", {"None", "Red", "Green", "Blue", "Alpha"}, "Vertex-color channel painted 1 where geometry shelters the surface (under awnings, inside collars). Which states respect shelter is part of the runtime definitions."),
        textField("maskR", "Mask R binds state", "", "Runtime state name (e.g. Blood) placed by the Surface State Masks texture's R channel instead of map-guided accumulation."),
        textField("maskG", "Mask G binds state", "", "State name bound to the Surface State Masks G channel."),
        textField("maskB", "Mask B binds state", "", "State name bound to the Surface State Masks B channel."),
        textField("maskA", "Mask A binds state", "", "State name bound to the Surface State Masks A channel.")
    });
    page.content->addStretch();
    return page.page;
}

QWidget* MainWindow::createLandscapePage()
{
    auto page = createPage(tr("Landscape"), tr("LAND supplies runtime weights while LTEX/TXST selects slices from specialized landscape texture arrays. This workflow does not use the ordinary Primary or Secondary UV pages."));
    addInfoCard(page.content, tr("Runtime ownership"), tr("LAND weight data is supplied by the game and is not an authored texture path. The sockets below define the arrays sampled by the landscape shader."));
    addGroupTitle(page.content, tr("LANDSCAPE TEXTURE RESOURCES"));
    for (const TextureSlotSpec& spec : landscapeSlots()) addTextureSlot(page.content, QStringLiteral("Landscape"), spec);
    addSettingsCard(page.content, QStringLiteral("Landscape/Layers"), tr("Layer model"), {
        choiceField("blend", "Blend", "Weight", {"Weight", "Height Aware"}, "Weight blend is cheapest; height-aware blending improves material transitions."),
        choiceField("storage", "Storage", "Texture Arrays", {"Texture Arrays"}, "Array-backed layers permit dynamic material selection."),
        choiceField("weightSource", "Weight source", "Runtime LAND", {"Runtime LAND"}, "Landscape weights are generated by the engine from LAND data."),
        numberField("budget", "Visible-layer budget", 4.0, 1.0, 16.0, 1.0, "Maximum layers evaluated for a terrain draw.", 0),
        toggleField("sharedSamplers", "Shared samplers", true, "Reuses sampler state across all layer arrays."),
        numberField("heightContrast", "Height-blend contrast", 1.0, 0.0, 16.0, 0.01, "Controls the sharpness of height-aware transitions."),
        numberField("transitionWidth", "Transition width", 0.1, 0.0, 1.0, 0.005, "Softens boundaries between active terrain layers.")
    });
    addSettingsCard(page.content, QStringLiteral("Landscape/AntiTiling"), tr("Anti-tiling"), {
        toggleField("enabled", "Stochastic anti-tiling", false, "Hex-tiled stochastic sampling breaks visible texture repetition on terrain. Costs roughly three texture fetches per sample."),
        numberField("patchScale", "Patch scale", 4.0, 0.25, 64.0, 0.05, "Density of the randomized hex patches in UV space."),
        numberField("blendContrast", "Blend contrast", 4.0, 1.0, 16.0, 0.05, "Sharpens the transition between neighboring randomized patches.")
    });
    setSettingEditorsEnabled(page.page, QStringLiteral("Landscape/AntiTiling"),
        {QStringLiteral("patchScale"), QStringLiteral("blendContrast")},
        m_document.setting(QStringLiteral("Landscape/AntiTiling"), QStringLiteral("enabled"), false).toBool());
    page.content->addStretch();
    return page.page;
}

QWidget* MainWindow::createTransparencyPage()
{
    const auto mode = m_document.shaderMode();
    auto page = createPage(tr("Transparency & Composition"), tr("Coverage, blending, depth, physical transmission, and screen distortion are grouped because they control scene composition."));
    QString defaultMode = QStringLiteral("Opaque");
    if (mode == Pbr::ShaderMode::HairSurface || mode == Pbr::ShaderMode::VegetationSurface) defaultMode = QStringLiteral("Alpha Test");
    else if (mode == Pbr::ShaderMode::GlassSurface) defaultMode = QStringLiteral("Physical Transmission");
    else if (mode == Pbr::ShaderMode::WaterSurface) defaultMode = QStringLiteral("Dedicated Water Pass");
    else if (mode == Pbr::ShaderMode::UnlitEffect || mode == Pbr::ShaderMode::SimpleLitEffect || mode == Pbr::ShaderMode::PhysicallyLitEffect) defaultMode = QStringLiteral("Alpha Blend");
    QStringList compositionModes{"Opaque", "Alpha Test", "Alpha Blend", "Premultiplied", "Additive", "Multiply"};
    if (mode == Pbr::ShaderMode::GlassSurface) compositionModes = {"Physical Transmission", "Alpha Blend", "Opaque"};
    else if (mode == Pbr::ShaderMode::WaterSurface) compositionModes = {"Dedicated Water Pass"};
    else if (mode == Pbr::ShaderMode::HairSurface || mode == Pbr::ShaderMode::VegetationSurface) compositionModes = {"Alpha Test", "Alpha Blend", "Opaque"};
    else if (mode == Pbr::ShaderMode::UnlitEffect || mode == Pbr::ShaderMode::SimpleLitEffect || mode == Pbr::ShaderMode::PhysicallyLitEffect)
        compositionModes = {"Alpha Blend", "Premultiplied", "Additive", "Multiply"};
    else if (mode == Pbr::ShaderMode::LandscapeSurface) compositionModes = {"Opaque"};
    else if (mode == Pbr::ShaderMode::AllFeatures) compositionModes.append({"Physical Transmission", "Dedicated Water Pass"});
    QStringList alphaSources{"Base Color A", "Constant"};
    const bool effect = mode == Pbr::ShaderMode::UnlitEffect || mode == Pbr::ShaderMode::SimpleLitEffect
        || mode == Pbr::ShaderMode::PhysicallyLitEffect;
    if (effect || mode == Pbr::ShaderMode::AllFeatures)
        alphaSources = {"Base Color A", "Particle Gradient A", "Base Color A × Particle Gradient A", "Constant"};
    else if (mode == Pbr::ShaderMode::LandscapeSurface)
        alphaSources = {"Constant"};
    addSettingsCard(page.content, QStringLiteral("Transparency/Composition"), tr("Composition"), {
        choiceField("mode", "Mode", defaultMode, compositionModes, "Selects a composition path supported by the current shader."),
        choiceField("alphaSource", "Alpha source", alphaSources.first(), alphaSources, "Data controlling opacity or alpha testing."),
        numberField("alpha", "Opacity", 1.0, 0.0, 1.0, 0.005, "Global opacity multiplier."),
        numberField("threshold", "Alpha-test threshold", 0.5, 0.0, 1.0, 0.005, "Pixels below this value are discarded."),
        toggleField("dither", "Dithered transition", mode == Pbr::ShaderMode::HairSurface || mode == Pbr::ShaderMode::VegetationSurface, "Uses stable dithering for smooth cutout transitions.")
    });
    addSettingsCard(page.content, QStringLiteral("Transparency/Depth"), tr("Depth and sorting"), {
        toggleField("depthTest", "Depth test", true, "Rejects fragments hidden by existing scene depth."),
        toggleField("depthWrite", "Depth write", defaultMode == QStringLiteral("Opaque") || defaultMode == QStringLiteral("Alpha Test"), "Writes accepted pixels to the depth buffer."),
        numberField("softIntersection", "Soft intersection", 0.0, 0.0, 1000.0, 0.01, "Fades translucent effects near scene intersections."),
        choiceField("sort", "Sort mode", "Automatic", {"Automatic", "Object", "Per Triangle", "Priority"}, "Transparency sorting strategy."),
        numberField("priority", "Sort priority", 0.0, -1000.0, 1000.0, 1.0, "Manual ordering override.", 0)
    });
    if (mode == Pbr::ShaderMode::GlassSurface || mode == Pbr::ShaderMode::WaterSurface || mode == Pbr::ShaderMode::AllFeatures || mode == Pbr::ShaderMode::UnlitEffect || mode == Pbr::ShaderMode::SimpleLitEffect || mode == Pbr::ShaderMode::PhysicallyLitEffect) {
        addSettingsCard(page.content, QStringLiteral("Transparency/Distortion"), tr("Screen Distortion"), {
            choiceField("combine", "Enabled input combination", "Add", {"Add", "Maximum", "Primary Overrides Secondary"}, "Combines enabled Primary and Secondary UV distortion sockets."),
            toggleField("depthAware", "Depth-aware edges", true, "Prevents foreground/background color bleeding."),
            numberField("edgeGuard", "Depth edge guard", 1.0, 0.0, 1000.0, 0.01, "Depth discontinuity threshold used to reject unrelated scene samples."),
            toggleField("writeDepth", "Write distortion depth", false, "Allows later distortion layers to respect this surface.")
        });
    }
    const QString authoredMode = m_document.setting(QStringLiteral("Transparency/Composition"), QStringLiteral("mode"), defaultMode).toString();
    const bool alphaTest = authoredMode == QStringLiteral("Alpha Test");
    const bool translucent = authoredMode == QStringLiteral("Alpha Blend") || authoredMode == QStringLiteral("Premultiplied")
        || authoredMode == QStringLiteral("Additive") || authoredMode == QStringLiteral("Multiply")
        || authoredMode == QStringLiteral("Physical Transmission");
    setSettingEditorsEnabled(page.page, QStringLiteral("Transparency/Composition"),
        {QStringLiteral("alphaSource"), QStringLiteral("alpha")}, alphaTest || translucent);
    setSettingEditorsEnabled(page.page, QStringLiteral("Transparency/Composition"),
        {QStringLiteral("threshold"), QStringLiteral("dither")}, alphaTest);
    setSettingEditorsEnabled(page.page, QStringLiteral("Transparency/Depth"),
        {QStringLiteral("softIntersection"), QStringLiteral("sort")}, translucent);
    const QString sortMode = m_document.setting(QStringLiteral("Transparency/Depth"), QStringLiteral("sort"), QStringLiteral("Automatic")).toString();
    setSettingEditorsEnabled(page.page, QStringLiteral("Transparency/Depth"), {QStringLiteral("priority")}, translucent && sortMode == QStringLiteral("Priority"));
    page.content->addStretch();
    return page.page;
}

QWidget* MainWindow::createEnvironmentPage()
{
    auto page = createPage(tr("Environment & Image-Based Lighting"), tr("The Community Shaders PBR component supplies runtime cubemaps. Preview cubemaps are editor-only preferences and are never written to PBRM."));
    addSettingsCard(page.content, QStringLiteral("Environment"), tr("Environment lighting"), {
        choiceField("source", "Reflection source", "Community Shaders PBR component", {"Community Shaders PBR component"}, "Runtime radiance is supplied externally by Community Shaders."),
        numberField("intensity", "IBL intensity", 1.0, 0.0, 100.0, 0.01, "Scales diffuse and specular image-based lighting."),
        numberField("diffuse", "Diffuse scale", 1.0, 0.0, 100.0, 0.01, "Scales diffuse irradiance."),
        numberField("specular", "Specular scale", 1.0, 0.0, 100.0, 0.01, "Scales reflected radiance."),
        numberField("rotation", "Rotation", 0.0, -360.0, 360.0, 1.0, "Rotates the runtime environment lighting around the world vertical axis.", 2, "°")
    });
    page.content->addStretch();
    return page.page;
}

QWidget* MainWindow::createAdvancedPage()
{
    auto page = createPage(tr("Advanced Rendering"), tr("Renderer-facing controls and optimization hints. Automatic variants are preferred unless a material requires an explicit override."));
    const bool unlit = m_document.shaderMode() == Pbr::ShaderMode::UnlitEffect;
    const bool effect = unlit || m_document.shaderMode() == Pbr::ShaderMode::SimpleLitEffect || m_document.shaderMode() == Pbr::ShaderMode::PhysicallyLitEffect;
    QVector<FieldSpec> rendering;
    if (!unlit) rendering.append(toggleField("receiveShadows", "Receive shadows", true, "Allows scene shadows to attenuate direct lighting."));
    if (!effect) rendering.append(toggleField("castShadows", "Cast shadows", true, "Includes this material in shadow passes."));
    rendering += QVector<FieldSpec>{
        toggleField("motionVectors", "Motion vectors", true, "Writes previous/current deformation for temporal effects."),
        toggleField("fog", "Fog", true, "Composes the material with scene fog."),
        choiceField("queue", "Render queue", "Automatic", {"Automatic", "Opaque", "Alpha Test", "Transparent", "Overlay"}, "Render-order category.")
    };
    addSettingsCard(page.content, QStringLiteral("Advanced/Rendering"), tr("Rendering"), rendering);
    addSettingsCard(page.content, QStringLiteral("Advanced/Optimization"), tr("Optimization"), {
        toggleField("sharedSamplers", "Shared samplers", true, "Reuses compatible texture samplers."),
        numberField("fadeStart", "Feature fade start", 4096.0, 0.0, 1000000.0, 1.0, "Distance where expensive optional features begin fading."),
        numberField("fadeEnd", "Feature fade end", 8192.0, 0.0, 1000000.0, 1.0, "Distance where optional features are fully disabled."),
        toggleField("gpuInstancing", "GPU instancing", true, "Allows compatible material instances to share draw submission.")
    });
    addSettingsCard(page.content, QStringLiteral("Advanced/Expert"), tr("Expert controls"), {
        toggleField("expertMode", "Expert Mode", false, "Shows renderer-facing permutation overrides that ordinary materials should leave automatic.")
    });
    if (m_document.setting(QStringLiteral("Advanced/Expert"), QStringLiteral("expertMode"), false).toBool()) {
        addSettingsCard(page.content, QStringLiteral("Advanced/Variant"), tr("Manual shader override"), {
            choiceField("variant", "Shader variant", "Automatic", {"Automatic", "Core", "Secondary Layer", "Surface Lobes", "Displacement", "Full"}, "Forces a feature family; Automatic avoids unnecessary shader work.")
        });
    }
    page.content->addStretch();
    return page.page;
}

QWidget* MainWindow::createValidationPage()
{
    auto page = createPage(tr("Material Validation"), tr("Checks authoring consistency, texture resolution, shader scope, and inactive resources before export. Findings with a safe automatic remedy carry a Fix button; each fix is a single undo step."));
    QStringList errors;
    QStringList warnings;
    // Findings with a safe one-click remedy. The fix runs through
    // applyDocumentChange (one undo step) and the workspace re-validates
    // immediately after. Texture-file findings stay manual by design.
    struct FixableIssue {
        bool error = false;
        QString text;
        QString fixLabel;
        QString undoText;
        std::function<void()> fix;
    };
    QVector<FixableIssue> fixables;
    const auto addFix = [&fixables](bool error, const QString& text, const QString& fixLabel,
                            const QString& undoText, std::function<void()> fix) {
        fixables.append({error, text, fixLabel, undoText, std::move(fix)});
    };
    int enabledTextures = 0;

    const auto validate = [this, &errors, &warnings, &addFix, &enabledTextures](const QString& setName, const TextureSlotSpec& spec, bool required = false) {
        Pbr::TextureSlotState state = m_document.textureSlot(setName, spec.key);
        if (state.path.isNull()) {
            state.enabled = spec.defaultEnabled;
            state.path = spec.defaultPath;
        }
        if (!state.enabled) {
            if (required) errors << tr("%1/%2 is required by this shader.").arg(setName, spec.name);
            return;
        }
        ++enabledTextures;
        if (TextureCache::hasChannelSwaps(state.values))
            warnings << tr("%1/%2 has unbaked channel sources; use Export Packed Texture before runtime use.").arg(setName, spec.name);
        if (state.path.trimmed().isEmpty()) {
            if (required)
                errors << tr("%1/%2 requires a texture array.").arg(setName, spec.name);
            else if (!TextureCache::hasChannelSwaps(state.values))
                warnings << tr("%1/%2 is enabled without a texture; constants will be used.").arg(setName, spec.name);
        } else if (TextureImage::resolvePath(state.path, m_textureRoot).isEmpty()) {
            errors << tr("%1/%2 cannot be resolved: %3").arg(setName, spec.name, state.path);
        } else {
            const TextureImage::Diagnostics info = TextureImage::inspect(state.path, m_textureRoot);
            if (!info.valid) {
                errors << tr("%1/%2 cannot be inspected: %3").arg(setName, spec.name, info.error);
                return;
            }
            if (QFileInfo(state.path).isAbsolute())
                warnings << tr("%1/%2 uses a nonportable absolute path.").arg(setName, spec.name);
            if (qMax(info.width, info.height) > 1 && info.mipLevels == 1)
                warnings << tr("%1/%2 has no mip chain.").arg(setName, spec.name);
            const bool expectsSrgb = spec.format.startsWith(QStringLiteral("sRGB"), Qt::CaseInsensitive);
            const bool expectsLinear = spec.format.startsWith(QStringLiteral("Linear"), Qt::CaseInsensitive);
            if (expectsSrgb && info.colorSpace != QStringLiteral("sRGB"))
                warnings << tr("%1/%2 should be authored as sRGB.").arg(setName, spec.name);
            if (expectsLinear && info.colorSpace == QStringLiteral("sRGB"))
                warnings << tr("%1/%2 should be authored in linear color space.").arg(setName, spec.name);
            // Channels the texture does not provide automatically fall back to
            // their constant overrides (the slot widget forces this), so a
            // BC5 RG-only normal map is valid as-is. Channels whose sampling
            // genuinely matters get targeted checks below instead of a
            // blanket missing-channel error.
            const auto setSlotValue = [this, setName, key = spec.key](const QString& valueKey, const QJsonValue& value) {
                Pbr::TextureSlotState slotState = m_document.textureSlot(setName, key);
                slotState.values.insert(valueKey, value);
                m_document.setTextureSlot(setName, key, slotState);
            };
            if (spec.key.contains(QStringLiteral("Normal"), Qt::CaseInsensitive)) {
                const bool height = state.values.value(QStringLiteral("heightInBlue")).toBool(false);
                if (height && !info.channels.contains(QStringLiteral("B")))
                    addFix(true, tr("%1/%2 enables \"B contains height\" but the texture has no blue channel.").arg(setName, spec.name),
                        tr("Disable height flag"), tr("Disable B contains height on %1").arg(spec.name),
                        [setSlotValue] { setSlotValue(QStringLiteral("heightInBlue"), false); });
                else if (height && info.format != QStringLiteral("BC7"))
                    warnings << tr("%1/%2 packs height and should use BC7.").arg(setName, spec.name);
                else if (!height && info.format != QStringLiteral("BC5") && info.format != QStringLiteral("BC7"))
                    warnings << tr("%1/%2 should use BC5 or BC7.").arg(setName, spec.name);
            }
            if (spec.key.contains(QStringLiteral("Rmaos"), Qt::CaseInsensitive)) {
                const bool alphaPorosity = state.values.value(QStringLiteral("alphaCarries")).toString(QStringLiteral("Specular Weight")) == QLatin1String("Porosity");
                if (!alphaPorosity && !state.values.value(QStringLiteral("overrideSpecularWeight")).toBool(true)
                    && !info.channels.contains(QStringLiteral("A")))
                    addFix(true, tr("%1/%2 samples specular weight but the texture has no alpha channel.").arg(setName, spec.name),
                        tr("Use weight constant"), tr("Use the weight constant on %1").arg(spec.name),
                        [setSlotValue] { setSlotValue(QStringLiteral("overrideSpecularWeight"), true); });
                // PBRM v5 authored alpha as dielectric F0; v6 reads it as specular
                // weight. The repack rewrites the texture (docs/PBRM-v6-Specular.md
                // section 4) and clears the marker the loader left.
                if (state.values.value(QStringLiteral("legacyF0Alpha")).toBool(false))
                    addFix(true, tr("%1/%2 alpha texels were authored as dielectric F0 (PBRM v5); v6 reads alpha as specular weight. Repack the alpha as weight; texels above 0.04 move into a specular IOR map.").arg(setName, spec.name),
                        tr("Repack as weight"), tr("Repack %1 alpha as specular weight").arg(spec.name),
                        [this, setName, key = spec.key] {
                            QString repackError;
                            if (!SpecularMigrate::repackRmaosSlot(&m_document, setName, key, m_textureRoot, &repackError))
                                QMessageBox::warning(this, tr("Repack failed"), repackError);
                        });
                if (alphaPorosity && !state.values.value(QStringLiteral("overridePorosity")).toBool(false)
                    && !info.channels.contains(QStringLiteral("A")))
                    addFix(true, tr("%1/%2 declares alpha as porosity but the texture has no alpha channel.").arg(setName, spec.name),
                        tr("Use porosity constant"), tr("Use the porosity constant on %1").arg(spec.name),
                        [setSlotValue] { setSlotValue(QStringLiteral("overridePorosity"), true); });
                if (alphaPorosity && state.values.value(QStringLiteral("overrideMetallic")).toBool(true)
                    && state.values.value(QStringLiteral("metallic")).toDouble(0.0) > .5)
                    warnings << tr("%1/%2 dedicates alpha to porosity on a strongly metallic material; sealed metal has no meaningful porosity and specular weight authoring is sacrificed.").arg(setName, spec.name);
            }
            if (spec.channels.size() == 1 && info.channels.size() > 1 && info.format != QStringLiteral("BC4"))
                warnings << tr("%1/%2 only samples one channel but the texture carries %3; a single-channel BC4 bake would save memory.")
                    .arg(setName, spec.name).arg(info.channels.join(QString()));
            // Unused-channel advisories: alpha dead under Opaque composition,
            // RG-only normal usage, and fully-overridden textures.
            if (spec.key.endsWith(QStringLiteral("BaseColor")) && info.channels.contains(QStringLiteral("A"))
                && info.format != QStringLiteral("BC1")
                && m_document.setting(QStringLiteral("Transparency/Composition"), QStringLiteral("mode"), QStringLiteral("Opaque")).toString() == QStringLiteral("Opaque"))
                warnings << tr("%1/%2 carries an alpha channel that Opaque composition never samples; BC1 would halve the memory.").arg(setName, spec.name);
            if (spec.key.contains(QStringLiteral("Normal"), Qt::CaseInsensitive) && !spec.key.contains(QStringLiteral("Dynamic"), Qt::CaseInsensitive)) {
                const QString normalAlpha = state.values.value(QStringLiteral("alphaCarries")).toString(QStringLiteral("None"));
                if (normalAlpha != QLatin1String("None") && !info.channels.contains(QStringLiteral("A")))
                    addFix(true, tr("%1/%2 declares alpha as %3 but the texture has no alpha channel.").arg(setName, spec.name, normalAlpha),
                        tr("Clear alpha selector"), tr("Clear the alpha selector on %1").arg(spec.name),
                        [setSlotValue] { setSlotValue(QStringLiteral("alphaCarries"), QStringLiteral("None")); });
                if (normalAlpha == QLatin1String("Cone Map") && !state.values.value(QStringLiteral("heightInBlue")).toBool(false))
                    addFix(true, tr("%1/%2 declares alpha as a cone map but B does not carry height; a cone map is baked from the height beside it.").arg(setName, spec.name),
                        tr("Clear alpha selector"), tr("Clear the alpha selector on %1").arg(spec.name),
                        [setSlotValue] { setSlotValue(QStringLiteral("alphaCarries"), QStringLiteral("None")); });
                if (state.values.value(QStringLiteral("layerMaskInAlpha")).toBool(false))
                    addFix(false, tr("%1/%2 carries the retired layer-modulation-in-alpha flag; Normal alpha now carries signed curvature. Repack the old mask into a Layer Coverage texture or switch the layer to Height Blend coverage first if the mask mattered.").arg(setName, spec.name),
                        tr("Remove retired keys"), tr("Remove retired layer-mask keys from %1").arg(spec.name),
                        [this, setName, key = spec.key] {
                            Pbr::TextureSlotState slotState = m_document.textureSlot(setName, key);
                            for (const QString& retired : {QStringLiteral("layerMaskInAlpha"), QStringLiteral("overrideLayerMask"), QStringLiteral("layerMask")})
                                slotState.values.remove(retired);
                            m_document.setTextureSlot(setName, key, slotState);
                        });
                if (!state.values.value(QStringLiteral("heightInBlue")).toBool(false)
                    && normalAlpha == QLatin1String("None")
                    && (info.channels.contains(QStringLiteral("B")) || info.channels.contains(QStringLiteral("A")))
                    && info.format != QStringLiteral("BC5"))
                    warnings << tr("%1/%2 only samples RG (height and curvature are off) but the texture carries %3; BC5 would halve the memory.")
                        .arg(setName, spec.name).arg(info.channels.join(QString()));
            }
            if (spec.key.endsWith(QStringLiteral("SpecularColor"))
                && state.values.value(QStringLiteral("legacyTintAlpha")).toBool(false))
                addFix(true, tr("%1/%2 alpha was a tint weight (PBRM v5); v6 reads alpha as the specular IOR map. Bake the weight into RGB and write the material's IOR into alpha.").arg(setName, spec.name),
                    tr("Bake tint weight"), tr("Bake %1 tint weight into RGB").arg(spec.name),
                    [this, setName, key = spec.key] {
                        QString bakeError;
                        if (!SpecularMigrate::bakeTintSlot(&m_document, setName, key, m_textureRoot, &bakeError))
                            QMessageBox::warning(this, tr("Bake failed"), bakeError);
                    });
            QStringList overrideToggles;
            QStringList providedOverrides;
            bool allOverridden = true;
            for (const FieldSpec& field : spec.fields) {
                if (field.type != FieldType::Toggle || !field.key.startsWith(QStringLiteral("override"))) continue;
                overrideToggles << field.key;
                allOverridden = allOverridden && state.values.value(field.key).toBool(field.defaultValue.toBool());
                // Channel letters before the bullet in the label (the slot
                // widget's convention); the fix flips only overrides whose
                // channels the texture actually provides.
                const QString channels = field.label.section(QChar(0x2022), 0, 0).trimmed();
                bool provided = !channels.isEmpty();
                for (const QChar channel : channels)
                    if (QStringLiteral("RGBA").contains(channel)) provided = provided && info.channels.contains(channel);
                if (provided) providedOverrides << field.key;
            }
            if (overrideToggles.size() >= 2 && allOverridden) {
                if (providedOverrides.isEmpty())
                    warnings << tr("%1/%2 has a texture assigned but every channel is overridden by constants; the texture is never sampled.").arg(setName, spec.name);
                else
                    addFix(false, tr("%1/%2 has a texture assigned but every channel is overridden by constants; the texture is never sampled.").arg(setName, spec.name),
                        tr("Use texture channels"), tr("Use texture channels on %1").arg(spec.name),
                        [this, setName, key = spec.key, providedOverrides] {
                            Pbr::TextureSlotState slotState = m_document.textureSlot(setName, key);
                            for (const QString& overrideKey : providedOverrides)
                                slotState.values.insert(overrideKey, false);
                            m_document.setTextureSlot(setName, key, slotState);
                        });
            }
        }
    };

    if (m_document.shaderMode() == Pbr::ShaderMode::LandscapeSurface) {
        const QSet<QString> requiredArrays{
            QStringLiteral("baseColorArray"), QStringLiteral("normalHeightArray"), QStringLiteral("rmaosArray")
        };
        for (const auto& spec : landscapeSlots())
            validate(QStringLiteral("Landscape"), spec, requiredArrays.contains(spec.key));
    } else {
        for (const auto& spec : textureSlotsFor(m_document.shaderMode(), QStringLiteral("UV0"))) validate(QStringLiteral("UV0"), spec);
        const auto validateOverlay = [this, &validate, &addFix](const QString& uvSet, bool layerEnabled, const QString& layerName) {
            if (layerEnabled) {
                for (const auto& spec : textureSlotsFor(m_document.shaderMode(), uvSet)) validate(uvSet, spec);
                return;
            }
            for (const QJsonValue& value : m_document.textureSet(uvSet)) {
                if (Pbr::TextureSlotState::fromJson(value.toObject()).enabled) {
                    const bool tertiary = uvSet == QStringLiteral("UV2");
                    addFix(false, tr("%1 UV contains enabled inputs but the layer itself is disabled.").arg(layerName),
                        tr("Enable layer"), tr("Enable the %1 UV layer").arg(layerName),
                        [this, tertiary] {
                            if (tertiary) m_document.setTertiaryUvEnabled(true);
                            else m_document.setSecondaryUvEnabled(true);
                        });
                    break;
                }
            }
        };
        validateOverlay(QStringLiteral("UV1"), m_document.secondaryUvEnabled(), tr("Secondary"));
        validateOverlay(QStringLiteral("UV2"), m_document.tertiaryUvEnabled(), tr("Tertiary"));
    }
    const QJsonObject parallax = m_document.settingGroup(QStringLiteral("Geometry/Parallax"));
    const QString heightSource = m_document.setting(QStringLiteral("Geometry/Source"), QStringLiteral("source"), QStringLiteral("None")).toString();
    if (parallax.value(QStringLiteral("enabled")).toBool(false)) {
        if (heightSource == QStringLiteral("None"))
            addFix(true, tr("Parallax is enabled without a height source."),
                tr("Disable parallax"), tr("Disable parallax"),
                [this] { m_document.setSetting(QStringLiteral("Geometry/Parallax"), QStringLiteral("enabled"), false); });
        if (heightSource.startsWith(QStringLiteral("Secondary")) && !m_document.secondaryUvEnabled())
            addFix(true, tr("Parallax uses a Secondary height source while Secondary UV is disabled."),
                tr("Enable Secondary UV"), tr("Enable the Secondary UV layer"),
                [this] { m_document.setSecondaryUvEnabled(true); });
        if (heightSource != QStringLiteral("None")) {
            const bool landscapeSource = heightSource.startsWith(QStringLiteral("Landscape"));
            const bool secondarySource = heightSource.startsWith(QStringLiteral("Secondary"));
            const bool normalBlue = heightSource.endsWith(QStringLiteral("Normal Blue"));
            const QString set = landscapeSource ? QStringLiteral("Landscape") : (secondarySource ? QStringLiteral("UV1") : QStringLiteral("UV0"));
            const QString prefix = secondarySource ? QStringLiteral("secondary") : QStringLiteral("primary");
            const QString key = landscapeSource
                ? (normalBlue ? QStringLiteral("normalHeightArray") : QStringLiteral("heightArray"))
                : prefix + (normalBlue ? QStringLiteral("Normal") : QStringLiteral("SeparateHeight"));
            const Pbr::TextureSlotState sourceState = m_document.textureSlot(set, key);
            const auto setSourceValue = [this, set, key](const QString& valueKey, const QJsonValue& value) {
                Pbr::TextureSlotState slotState = m_document.textureSlot(set, key);
                slotState.values.insert(valueKey, value);
                m_document.setTextureSlot(set, key, slotState);
            };
            if (!sourceState.enabled)
                addFix(true, tr("The selected parallax height texture input is disabled."),
                    tr("Enable input"), tr("Enable the parallax height input"),
                    [this, set, key] {
                        Pbr::TextureSlotState slotState = m_document.textureSlot(set, key);
                        slotState.enabled = true;
                        m_document.setTextureSlot(set, key, slotState);
                    });
            if (normalBlue && !sourceState.values.value(QStringLiteral("heightInBlue")).toBool(false))
                addFix(true, tr("The selected Normal Blue source does not have 'B contains height' enabled."),
                    tr("Enable height flag"), tr("Enable B contains height on the parallax source"),
                    [setSourceValue] { setSourceValue(QStringLiteral("heightInBlue"), true); });
            const bool overridden = sourceState.values.value(QStringLiteral("overrideHeight")).toBool(true);
            if (sourceState.path.trimmed().isEmpty() && overridden)
                warnings << tr("Parallax uses a constant height override; without texture variation it cannot produce surface detail.");
        }
        const int minSteps = parallax.value(QStringLiteral("minSteps")).toInt(12);
        const int maxSteps = parallax.value(QStringLiteral("maxSteps")).toInt(40);
        if (parallax.value(QStringLiteral("customQuality")).toBool(false) && minSteps > maxSteps)
            addFix(true, tr("Parallax minimum view steps exceed maximum view steps."),
                tr("Swap values"), tr("Swap parallax view-step bounds"),
                [this, minSteps, maxSteps] {
                    m_document.setSetting(QStringLiteral("Geometry/Parallax"), QStringLiteral("minSteps"), maxSteps);
                    m_document.setSetting(QStringLiteral("Geometry/Parallax"), QStringLiteral("maxSteps"), minSteps);
                });
        const double parallaxFadeStart = parallax.value(QStringLiteral("fadeStart")).toDouble(512.0);
        const double parallaxFadeEnd = parallax.value(QStringLiteral("fadeEnd")).toDouble(2048.0);
        if (parallaxFadeStart > parallaxFadeEnd)
            addFix(true, tr("Parallax fade start exceeds fade end."),
                tr("Swap values"), tr("Swap parallax fade distances"),
                [this, parallaxFadeStart, parallaxFadeEnd] {
                    m_document.setSetting(QStringLiteral("Geometry/Parallax"), QStringLiteral("fadeStart"), parallaxFadeEnd);
                    m_document.setSetting(QStringLiteral("Geometry/Parallax"), QStringLiteral("fadeEnd"), parallaxFadeStart);
                });
    }
    const QJsonObject optimization = m_document.settingGroup(QStringLiteral("Advanced/Optimization"));
    const double advancedFadeStart = optimization.value(QStringLiteral("fadeStart")).toDouble(4096.0);
    const double advancedFadeEnd = optimization.value(QStringLiteral("fadeEnd")).toDouble(8192.0);
    if (advancedFadeStart > advancedFadeEnd)
        addFix(true, tr("Advanced feature fade start exceeds fade end."),
            tr("Swap values"), tr("Swap Advanced fade distances"),
            [this, advancedFadeStart, advancedFadeEnd] {
                m_document.setSetting(QStringLiteral("Advanced/Optimization"), QStringLiteral("fadeStart"), advancedFadeEnd);
                m_document.setSetting(QStringLiteral("Advanced/Optimization"), QStringLiteral("fadeEnd"), advancedFadeStart);
            });
    const QJsonObject transparency = m_document.settingGroup(QStringLiteral("Transparency/Composition"));
    const QString composition = transparency.value(QStringLiteral("mode")).toString(QStringLiteral("Opaque"));
    const QString alphaSource = transparency.value(QStringLiteral("alphaSource")).toString(QStringLiteral("Base Color A"));
    const bool effectShader = m_document.shaderMode() == Pbr::ShaderMode::UnlitEffect
        || m_document.shaderMode() == Pbr::ShaderMode::SimpleLitEffect
        || m_document.shaderMode() == Pbr::ShaderMode::PhysicallyLitEffect
        || m_document.shaderMode() == Pbr::ShaderMode::AllFeatures;
    if (alphaSource.contains(QStringLiteral("Gradient")) && !effectShader)
        errors << tr("The selected alpha source requires a Particle Gradient texture input that this shader does not expose.");
    if (m_document.shaderMode() == Pbr::ShaderMode::LandscapeSurface && composition != QStringLiteral("Opaque"))
        addFix(true, tr("Landscape supports only Opaque composition."),
            tr("Set Opaque"), tr("Set composition to Opaque"),
            [this] { m_document.setSetting(QStringLiteral("Transparency/Composition"), QStringLiteral("mode"), QStringLiteral("Opaque")); });
    if (composition == QStringLiteral("Alpha Test")
        && (transparency.value(QStringLiteral("threshold")).toDouble(0.5) <= 0.0
            || transparency.value(QStringLiteral("threshold")).toDouble(0.5) >= 1.0))
        addFix(false, tr("Alpha-test threshold is at an extreme and will keep or discard nearly every pixel."),
            tr("Reset to 0.5"), tr("Reset the alpha-test threshold"),
            [this] { m_document.setSetting(QStringLiteral("Transparency/Composition"), QStringLiteral("threshold"), 0.5); });
    const QStringList looseSources = TextureImage::searchRoots(m_textureRoot);
    const QStringList archives = TextureImage::archivePaths();
    if (looseSources.isEmpty() && archives.isEmpty())
        warnings << tr("No texture source is configured. Relative texture paths cannot be previewed or validated.");
    if (m_document.shaderMode() == Pbr::ShaderMode::AllFeatures)
        warnings << tr("Advanced is a validation superset; choose a narrower shader for a final asset.");
    if (m_document.setting(QStringLiteral("Advanced/Expert"), QStringLiteral("expertMode"), false).toBool())
        warnings << tr("Expert Mode is enabled. Verify any manual shader variant override before export.");
    {
        // v5: surface state behavior is runtime-owned. Per-material state
        // groups and the response slot are legacy data; the SurfaceResponse
        // bindings are checked against the previewed definitions.
        if (Pbr::hasLegacySurfaceStateGroups(m_document)) {
            if (m_document.fileName().trimmed().isEmpty())
                errors << tr("Legacy per-material surface state groups are authored; state behavior is runtime-owned as of PBRM v5. Save the material to enable the one-click extract & strip.");
            else
                addFix(true, tr("Legacy per-material surface state groups are authored; state behavior is runtime-owned as of PBRM v5."),
                    tr("Extract && strip"), tr("Extract legacy states and strip them"),
                    [this] {
                        const QFileInfo info(m_document.fileName());
                        const QString target = info.absolutePath() + QLatin1Char('/') + info.completeBaseName() + QStringLiteral(".SurfaceStates.json");
                        QString extractError;
                        if (!Pbr::extractLegacyStates(m_document, target, &extractError)) {
                            QMessageBox::warning(this, tr("Extract legacy states"),
                                tr("The definitions JSON could not be written, so nothing was stripped: %1").arg(extractError));
                            return;
                        }
                        // Groups only: a retired response slot keeps its own
                        // finding (and repack breadcrumb) until handled.
                        Pbr::stripLegacySurfaceStateGroups(m_document);
                        statusBar()->showMessage(tr("Extracted legacy states to %1 and stripped the groups").arg(target), 6000);
                    });
        }
        for (const QString& setName : {QStringLiteral("UV0"), QStringLiteral("UV1"), QStringLiteral("UV2")}) {
            const QJsonObject set = m_document.textureSet(setName);
            for (auto it = set.constBegin(); it != set.constEnd(); ++it)
                if (it.key().endsWith(QStringLiteral("SurfaceStateResponse")))
                    errors << tr("%1 carries the retired Surface State Response input. Repack R into RMAOS alpha (porosity) and B into Normal alpha (curvature) with the channel packer, then strip legacy surface data via Batch Operations.").arg(setName);
        }
        const Pbr::TextureSlotState stateMasks = m_document.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryStateMasks"));
        const QJsonObject response = m_document.settingGroup(QStringLiteral("SurfaceResponse"));
        const QVector<Pbr::SurfaceStateSpec> definitions = Pbr::activeSurfaceStateDefinitions();
        QStringList boundNames;
        for (const QString& key : {QStringLiteral("maskR"), QStringLiteral("maskG"), QStringLiteral("maskB"), QStringLiteral("maskA")}) {
            const QString name = response.value(key).toString().trimmed();
            if (name.isEmpty()) continue;
            boundNames << name;
            bool known = false;
            for (const auto& state : definitions) known = known || state.name.compare(name, Qt::CaseInsensitive) == 0;
            if (!known)
                warnings << tr("Surface response binding %1 names state '%2', which is not in the previewed definitions; it may still exist in the shipped runtime set — verify the spelling.").arg(key, name);
        }
        if (!boundNames.isEmpty() && (!stateMasks.enabled || stateMasks.path.trimmed().isEmpty()))
            warnings << tr("Surface state mask bindings are authored but no Surface State Masks texture is assigned.");
    }

    int errorCount = errors.size(), warningCount = warnings.size();
    for (const auto& fixable : fixables) (fixable.error ? errorCount : warningCount)++;
    if (errorCount + warningCount == 0) {
        addInfoCard(page.content, tr("No findings"), tr("The material structure is internally consistent."));
    } else {
        // Structured findings: severity-ordered rows; rows with a safe
        // automatic remedy carry a Fix button (one undo step, immediate
        // re-validation).
        auto* card = new QFrame(page.content->parentWidget());
        card->setObjectName(QStringLiteral("Card"));
        card->setStyleSheet(errorCount > 0
            ? QStringLiteral("QFrame#Card { border-color:#8a4034; background:#362823; }")
            : QStringLiteral("QFrame#Card { border-color:#9a6d2c; background:#392f24; }"));
        auto* rows = new QVBoxLayout(card);
        rows->setContentsMargins(8, 5, 8, 5);
        rows->setSpacing(4);
        auto* heading = new QLabel(errorCount > 0
            ? tr("Findings — %1 error(s), %2 warning(s)").arg(errorCount).arg(warningCount)
            : tr("Findings — %1 warning(s)").arg(warningCount), card);
        heading->setObjectName(QStringLiteral("SectionTitle"));
        rows->addWidget(heading);
        QStringList searchText;
        const auto addRow = [this, card, rows, &searchText](bool error, const QString& text, const FixableIssue* fixable) {
            searchText << text;
            auto* row = new QWidget(card);
            auto* rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->setSpacing(8);
            auto* dot = new QLabel(QStringLiteral("●"), row);
            dot->setStyleSheet(error ? QStringLiteral("color:#e06c5a;") : QStringLiteral("color:#d9a13c;"));
            auto* label = new QLabel(text, row);
            label->setObjectName(QStringLiteral("Muted"));
            label->setWordWrap(true);
            rowLayout->addWidget(dot, 0, Qt::AlignTop);
            rowLayout->addWidget(label, 1);
            if (fixable && fixable->fix) {
                auto* fixButton = new QPushButton(fixable->fixLabel, row);
                const QString undoText = fixable->undoText;
                const std::function<void()> fix = fixable->fix;
                connect(fixButton, &QPushButton::clicked, this, [this, undoText, fix] {
                    applyDocumentChange(undoText, fix);
                    // Refresh after the click finishes so the button is not
                    // destroyed mid-signal (the settings-editor rebuild rule).
                    QTimer::singleShot(0, this, [this] { rebuildWorkspace(m_currentPage); });
                });
                rowLayout->addWidget(fixButton, 0, Qt::AlignTop);
            }
            rows->addWidget(row);
        };
        for (const QString& text : errors) addRow(true, text, nullptr);
        for (const auto& fixable : fixables) if (fixable.error) addRow(true, fixable.text, &fixable);
        for (const QString& text : warnings) addRow(false, text, nullptr);
        for (const auto& fixable : fixables) if (!fixable.error) addRow(false, fixable.text, &fixable);
        card->setProperty("searchText", tr("Findings") + QStringLiteral(" ") + searchText.join(QStringLiteral(" ")));
        page.content->addWidget(card);
    }
    addInfoCard(page.content, tr("Resource summary"), tr("Shader: %1\nActive texture inputs: %2\nTexture sources: %3 loose, %4 archive(s)")
        .arg(Pbr::shaderModeName(m_document.shaderMode())).arg(enabledTextures)
        .arg(looseSources.size()).arg(archives.size()));
    page.content->addStretch();
    return page.page;
}

void MainWindow::applyMaterialTemplate(const QString& templateName)
{
    if (!maybeSave()) return;
    m_document.reset();
    const auto setSlot = [this](const QString& set, const QString& key, bool enabled, const QJsonObject& values = {}) {
        Pbr::TextureSlotState state;
        state.enabled = enabled;
        state.expanded = enabled;
        state.path = QStringLiteral("");
        state.values = values;
        m_document.setTextureSlot(set, key, state);
    };

    if (templateName == QStringLiteral("Standard Metal")) {
        m_document.setShaderMode(Pbr::ShaderMode::StandardSurface);
        setSlot(QStringLiteral("UV0"), QStringLiteral("primaryRmaos"), true,
            {{"roughness", 0.22}, {"metallic", 1.0}, {"ao", 1.0}, {"specularWeight", 1.0}});
    } else if (templateName == QStringLiteral("Painted Metal")) {
        m_document.setShaderMode(Pbr::ShaderMode::StandardSurface);
        setSlot(QStringLiteral("UV0"), QStringLiteral("primaryRmaos"), true,
            {{"roughness", 0.32}, {"metallic", 0.15}, {"ao", 1.0}, {"specularWeight", 1.0}});
        m_document.setSecondaryUvEnabled(true);
        setSlot(QStringLiteral("UV1"), QStringLiteral("secondaryBaseColor"), true,
            {{"tint", QStringLiteral("#FFFFFF")}, {"blend", QStringLiteral("Masked Replace")}});
        setSlot(QStringLiteral("UV1"), QStringLiteral("secondaryLayerCoverage"), true);
    } else if (templateName == QStringLiteral("Plastic")) {
        m_document.setShaderMode(Pbr::ShaderMode::StandardSurface);
        setSlot(QStringLiteral("UV0"), QStringLiteral("primaryRmaos"), true,
            {{"roughness", 0.42}, {"metallic", 0.0}, {"ao", 1.0}, {"specularWeight", 1.0}});
    } else if (templateName == QStringLiteral("Glass")) {
        m_document.setShaderMode(Pbr::ShaderMode::GlassSurface);
        setSlot(QStringLiteral("UV0"), QStringLiteral("primaryTransmissionAbsorption"), true,
            {{"absorption", QStringLiteral("#EAF7FF")}, {"thickness", 1.0}});
        m_document.setSetting(QStringLiteral("Shading/Glass"), QStringLiteral("ior"), 1.5);
        m_document.setSetting(QStringLiteral("Transparency/Composition"), QStringLiteral("mode"), QStringLiteral("Physical Transmission"));
    } else if (templateName == QStringLiteral("Skin")) {
        m_document.setShaderMode(Pbr::ShaderMode::AdvancedSubsurface);
        setSlot(QStringLiteral("UV0"), QStringLiteral("primarySubsurface"), true,
            {{"scatterTint", QStringLiteral("#F2A38F")}, {"weight", 0.8}, {"thickness", 1.0}});
        m_document.setSetting(QStringLiteral("Shading/Subsurface"), QStringLiteral("mode"), QStringLiteral("Combined"));
        m_document.setSetting(QStringLiteral("Shading/Subsurface"), QStringLiteral("profile"), QStringLiteral("Skin"));
    } else if (templateName == QStringLiteral("Vegetation")) {
        m_document.setShaderMode(Pbr::ShaderMode::VegetationSurface);
        setSlot(QStringLiteral("UV0"), QStringLiteral("primarySubsurface"), true,
            {{"scatterTint", QStringLiteral("#A9C779")}, {"weight", 0.45}, {"thickness", 0.35}});
        m_document.setSetting(QStringLiteral("Vegetation/Profile"), QStringLiteral("profile"), QStringLiteral("Leaves"));
    } else if (templateName == QStringLiteral("Hair")) {
        m_document.setShaderMode(Pbr::ShaderMode::HairSurface);
        setSlot(QStringLiteral("UV0"), QStringLiteral("primaryHairDirection"), true);
        setSlot(QStringLiteral("UV0"), QStringLiteral("primaryHairScatter"), true,
            {{"scatterTint", QStringLiteral("#8B5A3C")}});
    } else if (templateName == QStringLiteral("Water")) {
        m_document.setShaderMode(Pbr::ShaderMode::WaterSurface);
        setSlot(QStringLiteral("UV0"), QStringLiteral("primaryWaterFlow"), true,
            {{"speed", 0.05}, {"strength", 1.0}});
        m_document.setSetting(QStringLiteral("Shading/Water"), QStringLiteral("ior"), 1.333);
    } else if (templateName == QStringLiteral("Physically Lit Effect")) {
        m_document.setShaderMode(Pbr::ShaderMode::PhysicallyLitEffect);
        setSlot(QStringLiteral("UV0"), QStringLiteral("primaryParticleGradient"), true);
        setSlot(QStringLiteral("UV0"), QStringLiteral("primaryEmissive"), true,
            {{"color", QStringLiteral("#FFFFFF")}, {"intensity", 1.0}});
    } else if (templateName == QStringLiteral("Landscape")) {
        m_document.setShaderMode(Pbr::ShaderMode::LandscapeSurface);
        setSlot(QStringLiteral("Landscape"), QStringLiteral("baseColorArray"), true);
        setSlot(QStringLiteral("Landscape"), QStringLiteral("normalHeightArray"), true);
        setSlot(QStringLiteral("Landscape"), QStringLiteral("rmaosArray"), true,
            {{"specularWeight", 1.0}});
    }
    m_undoStack->clear();
    m_undoStack->resetClean();
    m_document.setModified(true);
    rebuildWorkspace(m_document.shaderMode() == Pbr::ShaderMode::LandscapeSurface
        ? QStringLiteral("Landscape") : QStringLiteral("Primary UV Textures"));
    updateWindowTitle();
    statusBar()->showMessage(tr("Created %1 template").arg(templateName), 3000);
}

