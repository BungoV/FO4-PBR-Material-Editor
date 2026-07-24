#include "convertworkspace.h"

#include "bgsmreader.h"
#include "converternodes.h"
#include "dragnumericedit.h"
#include "maskpaint.h"
#include "materialdocument.h"
#include "materialpreviewwidget.h"
#include "nodecanvas.h"
#include "textureimage.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

QString statusText(const Converter::JobResult& result, bool dryRun)
{
    if (!result.errors.isEmpty() || !result.converted) return QObject::tr("Failed");
    if (result.flagged) return QObject::tr("Flagged");
    return dryRun ? QObject::tr("Would convert") : QObject::tr("Converted");
}

} // namespace

ConvertWorkspace::ConvertWorkspace(QWidget* parent)
    : QWidget(parent)
{
    QSettings settings;
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(5);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    root->addWidget(splitter);

    // ---- Left: sources + queue ----
    auto* side = new QFrame(splitter);
    side->setObjectName(QStringLiteral("SidePanel"));
    auto* sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(7, 7, 7, 7);
    sideLayout->setSpacing(5);

    auto* sourcesTitle = new QLabel(tr("Sources"), side);
    sourcesTitle->setObjectName(QStringLiteral("SectionTitle"));
    sideLayout->addWidget(sourcesTitle);
    const auto addPathRow = [&](const QString& label, const QString& settingsKey, const QString& dialogTitle) {
        auto* caption = new QLabel(label, side);
        sideLayout->addWidget(caption);
        auto* row = new QHBoxLayout();
        row->setSpacing(4);
        auto* edit = new QLineEdit(side);
        edit->setText(settings.value(settingsKey).toString());
        row->addWidget(edit, 1);
        auto* browse = new QPushButton(tr("…"), side);
        browse->setFixedWidth(28);
        connect(browse, &QPushButton::clicked, this, [this, edit, dialogTitle] { browseFolder(edit, dialogTitle); });
        row->addWidget(browse);
        sideLayout->addLayout(row);
        return edit;
    };
    m_sourceEdit = addPathRow(tr("Source materials (folder with .bgsm/.bgem)"),
        QStringLiteral("Convert/sourceRoot"), tr("Choose the source materials folder"));
    m_textureRootEdit = addPathRow(tr("Texture root (the Data folder with textures\\)"),
        QStringLiteral("Convert/textureRoot"), tr("Choose the source texture root"));
    m_outputEdit = addPathRow(tr("Output mod folder"),
        QStringLiteral("Convert/outputRoot"), tr("Choose the output mod folder"));

    m_scanButton = new QPushButton(tr("Scan Sources"), side);
    connect(m_scanButton, &QPushButton::clicked, this, &ConvertWorkspace::scanSources);
    sideLayout->addWidget(m_scanButton);

    auto* queueTitle = new QLabel(tr("Queue"), side);
    queueTitle->setObjectName(QStringLiteral("SectionTitle"));
    sideLayout->addSpacing(4);
    sideLayout->addWidget(queueTitle);
    m_queue = new QTreeWidget(side);
    m_queue->setHeaderLabels({tr("Material"), tr("Status"), tr("Metal")});
    m_queue->setRootIsDecorated(false);
    m_queue->setAlternatingRowColors(true);
    m_queue->header()->setStretchLastSection(false);
    m_queue->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_queue->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_queue->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    connect(m_queue, &QTreeWidget::currentItemChanged, this, [this] {
        loadOverrideControls(); updateDetail(); syncGraphToSelection();
    });
    sideLayout->addWidget(m_queue, 1);

    // The review loop: walk exactly the materials that need eyes.
    auto* flaggedRow = new QHBoxLayout();
    flaggedRow->setSpacing(4);
    m_prevFlagged = new QPushButton(tr("◀ Flagged"), side);
    m_prevFlagged->setToolTip(tr("Select the previous flagged or failed material."));
    connect(m_prevFlagged, &QPushButton::clicked, this, [this] { stepFlagged(-1); });
    flaggedRow->addWidget(m_prevFlagged);
    m_nextFlagged = new QPushButton(tr("Flagged ▶"), side);
    m_nextFlagged->setToolTip(tr("Select the next flagged or failed material."));
    connect(m_nextFlagged, &QPushButton::clicked, this, [this] { stepFlagged(1); });
    flaggedRow->addWidget(m_nextFlagged);
    m_flaggedSummary = new QLabel(side);
    flaggedRow->addWidget(m_flaggedSummary, 1);
    sideLayout->addLayout(flaggedRow);

    m_queueSummary = new QLabel(tr("Scan a source folder to build the queue."), side);
    m_queueSummary->setWordWrap(true);
    sideLayout->addWidget(m_queueSummary);
    splitter->addWidget(side);

    // ---- Center: actions on top, then batch options, the selected
    // material's tools, and the findings — one concern per card.
    auto* main = new QFrame(this); // holder only; its children are redistributed below
    auto* mainLayout = new QVBoxLayout(main);
    mainLayout->setContentsMargins(7, 7, 7, 7);
    mainLayout->setSpacing(7);

    // The editor's drag-scrub number fields — same left/right slider feel as
    // the authoring pages.
    const auto makeSpin = [main](double value, double min, double max, double step, int decimals = 2) {
        auto* spin = new DragNumericEdit(main);
        spin->setRange(min, max);
        spin->setSingleStep(step);
        spin->setDecimals(decimals);
        spin->setValue(value);
        return spin;
    };

    // Primary verbs.
    auto* actionsRow = new QHBoxLayout();
    actionsRow->setSpacing(5);
    m_dryRunButton = new QPushButton(tr("Dry Run"), main);
    m_dryRunButton->setToolTip(tr("Run the full pipeline without writing anything; statuses and findings fill in."));
    connect(m_dryRunButton, &QPushButton::clicked, this, [this] { startRun(true, RunScope::All); });
    actionsRow->addWidget(m_dryRunButton);
    m_convertSelectedButton = new QPushButton(tr("Convert Checked"), main);
    m_convertSelectedButton->setToolTip(tr("Convert the queue rows with a checked box."));
    connect(m_convertSelectedButton, &QPushButton::clicked, this, [this] { startRun(false, RunScope::Checked); });
    actionsRow->addWidget(m_convertSelectedButton);
    m_convertAllButton = new QPushButton(tr("Convert All"), main);
    connect(m_convertAllButton, &QPushButton::clicked, this, [this] { startRun(false, RunScope::All); });
    actionsRow->addWidget(m_convertAllButton);
    m_cancelButton = new QPushButton(tr("Cancel"), main);
    m_cancelButton->setEnabled(false);
    connect(m_cancelButton, &QPushButton::clicked, this, &ConvertWorkspace::cancelRun);
    actionsRow->addWidget(m_cancelButton);
    actionsRow->addStretch();
    m_openInAuthor = new QPushButton(tr("Open in Author"), main);
    m_openInAuthor->setEnabled(false);
    m_openInAuthor->setToolTip(tr("Open the converted sibling .pbrm in the Author workspace."));
    connect(m_openInAuthor, &QPushButton::clicked, this, [this] {
        QTreeWidgetItem* item = m_queue->currentItem();
        const int row = item ? m_queue->indexOfTopLevelItem(item) : -1;
        if (row >= 0 && row < m_entries.size() && m_entries[row].hasResult
            && QFileInfo::exists(m_entries[row].result.pbrmPath))
            emit openInAuthorRequested(m_entries[row].result.pbrmPath);
    });
    actionsRow->addWidget(m_openInAuthor);
    m_actionsRow = actionsRow;

    // Correction masking stays material-scoped, so it keeps its own group,
    // disabled until a queue row is chosen.
    m_selectedGroup = new QGroupBox(tr("Selected Material"), side);
    auto* selectedLayout = new QVBoxLayout(m_selectedGroup);
    selectedLayout->setContentsMargins(9, 9, 9, 9);
    selectedLayout->setSpacing(6);

    // Correction masking: load the material's reference NIF, click parts in
    // the preview to force dielectric/metal, save the 0.5-neutral sidecar.
    auto* maskRow = new QHBoxLayout();
    maskRow->setSpacing(9);
    m_maskLoadNif = new QPushButton(tr("Load Reference NIF…"), m_selectedGroup);
    m_maskLoadNif->setToolTip(tr("Show the material's mesh in the preview so fills can be picked in 3D."));
    connect(m_maskLoadNif, &QPushButton::clicked, this, &ConvertWorkspace::loadMaskReference);
    maskRow->addWidget(m_maskLoadNif);
    m_maskPickEnabled = new QCheckBox(tr("Pick fills"), m_selectedGroup);
    m_maskPickEnabled->setToolTip(tr("While checked, clicking the mesh applies the fill; camera drags still work."));
    connect(m_maskPickEnabled, &QCheckBox::toggled, this, [this](bool on) {
        m_preview->canvas()->setTrianglePickingEnabled(on);
    });
    maskRow->addWidget(m_maskPickEnabled);
    maskRow->addWidget(new QLabel(tr("Fill"), m_selectedGroup));
    m_maskFillMode = new QComboBox(m_selectedGroup);
    m_maskFillMode->addItems({tr("Triangle"), tr("Element"), tr("Submesh")});
    m_maskFillMode->setCurrentIndex(1);
    m_maskFillMode->setToolTip(tr("Triangle fills one face; Element fills the connected part under the click (position-welded); Submesh fills the whole shape."));
    maskRow->addWidget(m_maskFillMode);
    m_maskFillValue = new QComboBox(m_selectedGroup);
    m_maskFillValue->addItem(tr("Force Dielectric"), 0.0);
    m_maskFillValue->addItem(tr("Force Metal"), 1.0);
    m_maskFillValue->addItem(tr("Back to Inferred"), 0.5);
    m_maskFillValue->setToolTip(tr("What the fill paints into the 0.5-neutral correction mask."));
    maskRow->addWidget(m_maskFillValue);
    m_maskClear = new QPushButton(tr("Clear Mask"), m_selectedGroup);
    connect(m_maskClear, &QPushButton::clicked, this, &ConvertWorkspace::clearMaskFills);
    maskRow->addWidget(m_maskClear);
    m_maskSave = new QPushButton(tr("Save Mask"), m_selectedGroup);
    m_maskSave->setToolTip(tr("Write the sidecar and attach it to this material's conversions."));
    connect(m_maskSave, &QPushButton::clicked, this, &ConvertWorkspace::saveMask);
    maskRow->addWidget(m_maskSave);
    maskRow->addStretch();
    selectedLayout->addLayout(maskRow);
    m_maskStatus = new QLabel(m_selectedGroup);
    m_maskStatus->setObjectName(QStringLiteral("Muted"));
    m_maskStatus->setWordWrap(true);
    selectedLayout->addWidget(m_maskStatus);
    m_selectedGroup->setEnabled(false);
    sideLayout->addWidget(m_selectedGroup);

    auto* findingsGroup = new QGroupBox(tr("Findings"), side);
    auto* findingsLayout = new QVBoxLayout(findingsGroup);
    findingsLayout->setContentsMargins(9, 9, 9, 9);
    m_detail = new QListWidget(findingsGroup);
    m_detail->setWordWrap(true);
    m_detail->setFrameShape(QFrame::NoFrame);
    m_detail->setMaximumHeight(150);
    findingsLayout->addWidget(m_detail);
    sideLayout->addWidget(findingsGroup);

    // The centre pane IS the graph: the conversion's settings are its node
    // parameters, so there is no second panel to switch to.
    buildGraphMode(splitter);

    // ---- Far right: the same live 3D preview the editor uses, showing the
    // selected material's CONVERTED sibling with the output mod folder as
    // its texture root.
    m_preview = new MaterialPreviewWidget(splitter, /*converterMode=*/true);
    connect(m_preview->canvas(), &PbrPreviewGL::meshTrianglePicked, this, &ConvertWorkspace::handleTrianglePicked);
    splitter->addWidget(m_preview);
    // The 3D viewport opens at a third of the window width, matching the
    // authoring workspace.
    splitter->setSizes({340, 940, 640});
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 2);
    splitter->setStretchFactor(2, 1);
    updatePreview(nullptr);

    updateButtons();
    updateDetail();
}

ConvertWorkspace::~ConvertWorkspace()
{
    m_cancelRequested = true;
    if (m_worker) {
        m_worker->wait();
        delete m_worker;
        m_worker = nullptr;
    }
    persistSettings();
}

void ConvertWorkspace::persistSettings() const
{
    QSettings settings;
    settings.setValue(QStringLiteral("Convert/sourceRoot"), m_sourceEdit->text());
    settings.setValue(QStringLiteral("Convert/textureRoot"), m_textureRootEdit->text());
    settings.setValue(QStringLiteral("Convert/outputRoot"), m_outputEdit->text());
}

void ConvertWorkspace::browseFolder(QLineEdit* edit, const QString& title)
{
    const QString start = edit->text().trimmed().isEmpty() ? QDir::homePath() : edit->text();
    const QString folder = QFileDialog::getExistingDirectory(this, title, start);
    if (!folder.isEmpty()) {
        edit->setText(QDir::toNativeSeparators(folder));
        persistSettings();
    }
}

void ConvertWorkspace::scanSources()
{
    const QString source = QDir::fromNativeSeparators(m_sourceEdit->text().trimmed());
    m_entries.clear();
    m_queue->clear();
    m_detail->clear();
    if (source.isEmpty() || !QFileInfo(source).isDir()) {
        m_queueSummary->setText(tr("Choose an existing source folder first."));
        updateButtons();
        return;
    }
    persistSettings();
    QDirIterator it(source, {QStringLiteral("*.bgsm"), QStringLiteral("*.bgem")}, QDir::Files,
        QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QueueEntry entry;
        entry.materialPath = it.next();
        m_entries.append(entry);
    }
    std::sort(m_entries.begin(), m_entries.end(), [](const QueueEntry& a, const QueueEntry& b) {
        return a.materialPath.compare(b.materialPath, Qt::CaseInsensitive) < 0;
    });
    const QDir sourceDir(source);
    for (const QueueEntry& entry : m_entries) {
        auto* item = new QTreeWidgetItem(m_queue);
        item->setText(0, sourceDir.relativeFilePath(entry.materialPath));
        item->setToolTip(0, QDir::toNativeSeparators(entry.materialPath));
        item->setText(1, tr("Pending"));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, Qt::Unchecked);
    }
    m_queueSummary->setText(tr("%n material(s) queued.", nullptr, int(m_entries.size())));
    updateFlaggedSummary();
    loadOverrideControls();
    updateButtons();
}

Converter::JobOptions ConvertWorkspace::collectOptions(bool dryRun) const
{
    // Conversion math now lives in the graph's nodes; this only carries where
    // the run writes and how it reports.
    Converter::JobOptions job;
    job.outputRoot = QDir::fromNativeSeparators(m_outputEdit->text().trimmed());
    job.textureRoot = QDir::fromNativeSeparators(m_textureRootEdit->text().trimmed());
    job.dryRun = dryRun;
    return job;
}

void ConvertWorkspace::startRun(bool dryRun, RunScope scope)
{
    if (m_running || m_entries.isEmpty()) return;
    if (!dryRun && m_outputEdit->text().trimmed().isEmpty()) {
        m_queueSummary->setText(tr("Choose an output mod folder before converting."));
        return;
    }
    persistSettings();
    const int currentRow = m_queue->currentItem() ? m_queue->indexOfTopLevelItem(m_queue->currentItem()) : -1;
    QVector<int> rows;
    for (int row = 0; row < m_entries.size(); ++row) {
        if (scope == RunScope::Checked && m_queue->topLevelItem(row)->checkState(0) != Qt::Checked) continue;
        if (scope == RunScope::CurrentRow && row != currentRow) continue;
        rows.append(row);
        m_queue->topLevelItem(row)->setText(1, tr("Queued"));
        m_queue->topLevelItem(row)->setText(2, QString());
        m_entries[row].hasResult = false;
    }
    if (rows.isEmpty()) {
        m_queueSummary->setText(scope == RunScope::CurrentRow
            ? tr("Select a material to reconvert.") : tr("Check at least one material for a selected run."));
        return;
    }

    const Converter::JobOptions job = collectOptions(dryRun);
    // Per-material tuning: overridden rows convert with their own math.
    QVector<Converter::JobOptions> jobs;
    QVector<QString> paths;
    for (int row : rows) {
        paths.append(m_entries[row].materialPath);
        Converter::JobOptions rowJob = job;
        const auto override = m_overrides.constFind(m_entries[row].materialPath.toLower());
        if (override != m_overrides.constEnd()) rowJob.correctionMaskPath = override->maskPath;
        // A loaded reference mesh also drives the geometry AO bake.
        const auto maskState = m_masks.constFind(m_entries[row].materialPath.toLower());
        if (maskState != m_masks.constEnd()) rowJob.geometryNifPath = maskState->nifPath;
        jobs.append(rowJob);
    }

    m_running = true;
    m_lastRunDry = dryRun;
    m_cancelRequested = false;
    m_pendingResults = rows.size();
    m_queueSummary->setText(dryRun ? tr("Dry run in progress…") : tr("Converting…"));
    updateButtons();

    const NodeGraph::Graph graph = m_nodeGraph; // snapshot: edits during a run do not change it
    m_worker = QThread::create([this, jobs, rows, paths, graph] {
        Converter::TextureCache cache;
        for (int index = 0; index < rows.size(); ++index) {
            if (m_cancelRequested) {
                QMetaObject::invokeMethod(this, [this] { finishRun(); }, Qt::QueuedConnection);
                return;
            }
            const int row = rows[index];
            QMetaObject::invokeMethod(this, [this, row] {
                if (row < m_queue->topLevelItemCount())
                    m_queue->topLevelItem(row)->setText(1, tr("Converting…"));
            }, Qt::QueuedConnection);
            // The graph is the pipeline: it produces this material's images,
            // and the orchestrator writes them exactly as it writes a batch
            // conversion (proven byte-identical).
            Converter::JobOptions rowJob = jobs[index];
            QStringList graphErrors;
            rowJob.supplied = ConverterNodes::runGraph(graph, paths[index],
                rowJob.textureRoot, rowJob.geometryNifPath, &graphErrors);
            Converter::JobResult result;
            if (!rowJob.supplied.active) {
                result.materialPath = paths[index];
                result.errors.append(graphErrors.isEmpty()
                    ? QStringLiteral("the graph produced no output") : graphErrors.join(QStringLiteral("; ")));
            } else {
                result = Converter::convertMaterial(paths[index], rowJob, &cache);
            }
            QMetaObject::invokeMethod(this, [this, row, result] {
                handleResult(row, result);
            }, Qt::QueuedConnection);
        }
        QMetaObject::invokeMethod(this, [this] { finishRun(); }, Qt::QueuedConnection);
    });
    m_worker->start();
}

void ConvertWorkspace::updateFlaggedSummary()
{
    int flagged = 0;
    for (const QueueEntry& entry : m_entries)
        if (entry.hasResult && (entry.result.flagged || !entry.result.errors.isEmpty())) ++flagged;
    m_flaggedSummary->setText(flagged ? tr("%n to review", nullptr, flagged) : QString());
    m_prevFlagged->setEnabled(flagged > 0);
    m_nextFlagged->setEnabled(flagged > 0);
}

void ConvertWorkspace::stepFlagged(int direction)
{
    if (m_entries.isEmpty()) return;
    const int count = int(m_entries.size());
    const int start = m_queue->currentItem() ? m_queue->indexOfTopLevelItem(m_queue->currentItem()) : -1;
    for (int step = 1; step <= count; ++step) {
        const int row = ((start + direction * step) % count + count) % count;
        const QueueEntry& entry = m_entries[row];
        if (entry.hasResult && (entry.result.flagged || !entry.result.errors.isEmpty())) {
            m_queue->setCurrentItem(m_queue->topLevelItem(row));
            m_queue->scrollToItem(m_queue->topLevelItem(row));
            return;
        }
    }
}

void ConvertWorkspace::loadOverrideControls()
{
    // Conversion tuning moved into the graph; only the mask tools are still
    // scoped to one material.
    QTreeWidgetItem* item = m_queue->currentItem();
    const int row = item ? m_queue->indexOfTopLevelItem(item) : -1;
    const bool valid = row >= 0 && row < m_entries.size();
    m_selectedGroup->setEnabled(valid);
    m_selectedGroup->setTitle(valid
        ? tr("Selected Material — %1").arg(QFileInfo(m_entries[row].materialPath).fileName())
        : tr("Selected Material"));
}

void ConvertWorkspace::cancelRun()
{
    m_cancelRequested = true;
    m_queueSummary->setText(tr("Cancelling after the current material…"));
}

void ConvertWorkspace::finishRun()
{
    if (!m_running) return;
    m_running = false;
    if (m_worker) {
        m_worker->wait();
        m_worker->deleteLater();
        m_worker = nullptr;
    }
    int converted = 0, flagged = 0, failed = 0;
    for (const QueueEntry& entry : m_entries) {
        if (!entry.hasResult) continue;
        if (!entry.result.errors.isEmpty() || !entry.result.converted) ++failed;
        else if (entry.result.flagged) ++flagged;
        else ++converted;
    }
    m_queueSummary->setText(m_cancelRequested
        ? tr("Run cancelled. %1 clean, %2 flagged, %3 failed so far.").arg(converted).arg(flagged).arg(failed)
        : tr("Run complete: %1 clean, %2 flagged for review, %3 failed.").arg(converted).arg(flagged).arg(failed));
    updateButtons();
}

void ConvertWorkspace::handleResult(int row, const Converter::JobResult& result)
{
    if (row < 0 || row >= m_entries.size()) return;
    m_entries[row].result = result;
    m_entries[row].hasResult = true;
    updateFlaggedSummary();
    QTreeWidgetItem* item = m_queue->topLevelItem(row);
    if (!item) return;
    item->setText(1, statusText(result, m_lastRunDry));
    if (result.converted)
        item->setText(2, QStringLiteral("%1%").arg(int(std::lround(result.metalCoverage * 100.0f))));
    if (m_queue->currentItem() == item) updateDetail();
}

void ConvertWorkspace::updateDetail()
{
    m_detail->clear();
    QTreeWidgetItem* item = m_queue->currentItem();
    const int row = item ? m_queue->indexOfTopLevelItem(item) : -1;
    const bool valid = row >= 0 && row < m_entries.size();
    updatePreview(valid ? &m_entries[row] : nullptr);
    if (!valid) {
        m_detail->addItem(tr("Select a queue material to see its findings; Dry Run fills them in."));
        m_openInAuthor->setEnabled(false);
        return;
    }
    const QueueEntry& entry = m_entries[row];
    if (!entry.hasResult) {
        m_detail->addItem(tr("Not converted yet — run a dry run or conversion."));
        m_openInAuthor->setEnabled(false);
        return;
    }
    for (const QString& error : entry.result.errors)
        m_detail->addItem(tr("Error: %1").arg(error));
    for (const QString& note : entry.result.notes)
        m_detail->addItem(note);
    if (entry.result.converted) {
        m_detail->addItem(tr("Sibling: %1").arg(QDir::toNativeSeparators(entry.result.pbrmPath)));
        for (const QString& texture : entry.result.writtenTextures)
            m_detail->addItem(tr("Texture: %1").arg(QDir::toNativeSeparators(texture)));
    }
    m_openInAuthor->setEnabled(entry.result.converted && QFileInfo::exists(entry.result.pbrmPath));
}

void ConvertWorkspace::buildGraphMode(QSplitter* parent)
{
    ConverterNodes::registerAll(m_nodeRegistry);

    auto* page = new QWidget(parent);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    auto* actions = new QHBoxLayout();
    actions->setSpacing(7);
    if (m_actionsRow) { layout->addLayout(m_actionsRow); m_actionsRow = nullptr; }
    auto* reset = new QPushButton(tr("Reset to Default"), page);
    reset->setToolTip(tr("Rebuild the profile's stock conversion graph, discarding edits."));
    connect(reset, &QPushButton::clicked, this, [this] {
        m_graphProfile.clear();
        syncGraphToSelection();
    });
    actions->addWidget(reset);
    auto* frame = new QPushButton(tr("Frame All"), page);
    connect(frame, &QPushButton::clicked, this, [this] { if (m_canvas) m_canvas->frameAll(); });
    actions->addWidget(frame);
    auto* save = new QPushButton(tr("Save Graph…"), page);
    save->setToolTip(tr("Write a .pbrgraph that `pbrm convert --graph` can run headlessly."));
    connect(save, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, tr("Save conversion graph"),
            m_outputEdit->text().trimmed(), tr("Conversion graphs (*.pbrgraph)"));
        if (path.isEmpty()) return;
        QString error;
        if (!ConverterNodes::saveGraphFile(m_nodeGraph, path, &error))
            QMessageBox::warning(this, tr("Save graph"), error);
    });
    actions->addWidget(save);
    auto* load = new QPushButton(tr("Load Graph…"), page);
    connect(load, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Load conversion graph"),
            m_outputEdit->text().trimmed(), tr("Conversion graphs (*.pbrgraph)"));
        if (path.isEmpty()) return;
        QString error;
        const NodeGraph::Graph loaded = ConverterNodes::loadGraphFile(path, &error);
        if (loaded.nodes.isEmpty()) {
            QMessageBox::warning(this, tr("Load graph"), error.isEmpty() ? tr("The graph is empty.") : error);
            return;
        }
        m_nodeGraph = loaded;
        m_graphProfile = QStringLiteral("(loaded)"); // survives selection changes
        m_canvas->setGraph(m_nodeGraph);
        m_canvas->frameAll();
        m_graphEval->start();
    });
    actions->addWidget(load);
    actions->addStretch();
    auto* hint = new QLabel(tr("Right-click to add nodes · drag ports to wire · click an output to preview it"), page);
    hint->setObjectName(QStringLiteral("Muted"));
    actions->addWidget(hint);
    layout->addLayout(actions);

    auto* split = new QSplitter(Qt::Horizontal, page);
    m_canvas = new NodeCanvas(m_nodeRegistry, split);
    m_inspector = new NodeInspector(m_nodeRegistry, split);
    split->addWidget(m_canvas);
    split->addWidget(m_inspector);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    split->setSizes({720, 240});
    layout->addWidget(split, 1);
    parent->addWidget(page);

    // Debounced re-evaluation: dragging a node or a slider should not run the
    // pipeline on every intermediate value.
    m_graphEval = new QTimer(this);
    m_graphEval->setSingleShot(true);
    m_graphEval->setInterval(180);
    connect(m_graphEval, &QTimer::timeout, this, &ConvertWorkspace::evaluateGraph);

    connect(m_canvas, &NodeCanvas::graphEdited, this, [this] {
        m_nodeGraph = m_canvas->graph();
        m_graphEval->start();
    });
    connect(m_canvas, &NodeCanvas::nodeSelected, this, [this](const QString& nodeId) {
        if (nodeId.isEmpty()) {
            m_inspector->clear();
            // Back to the converted material on the surface.
            m_preview->canvas()->setNodeOutputImage(QImage());
            m_preview->setRenderMode(PbrPreviewGL::DebugView::Shaded);
            return;
        }
        m_inspector->showNode(&m_nodeGraph, nodeId, m_lastGraphResults.value(nodeId));
        showNodeInPreview(nodeId);
    });
    connect(m_inspector, &NodeInspector::parameterChanged, this,
        [this](const QString& nodeId, const QString& paramId, const QJsonValue& value) {
            if (NodeGraph::Node* node = m_nodeGraph.node(nodeId)) {
                node->params.insert(paramId, value);
                m_canvas->setGraph(m_nodeGraph);
                m_graphEval->start();
            }
        });
}

void ConvertWorkspace::syncGraphToSelection()
{
    if (!m_canvas) return;
    QTreeWidgetItem* item = m_queue->currentItem();
    const int row = item ? m_queue->indexOfTopLevelItem(item) : -1;
    if (row < 0 || row >= m_entries.size()) return;

    Bgsm::Material material;
    if (!Bgsm::readFile(m_entries[row].materialPath, &material, nullptr)) return;
    const QString profile = material.version > 2 ? QStringLiteral("FO76") : QStringLiteral("FO4");
    // Edits survive moving between materials of the same title; switching
    // titles loads that profile's stock graph.
    if (profile != m_graphProfile) {
        m_graphProfile = profile;
        m_nodeGraph = ConverterNodes::defaultGraphFor(material);
        m_canvas->setGraph(m_nodeGraph);
        m_canvas->frameAll();
    }
    m_graphEval->start();
}

void ConvertWorkspace::evaluateGraph()
{
    if (!m_canvas) return;
    QTreeWidgetItem* item = m_queue->currentItem();
    const int row = item ? m_queue->indexOfTopLevelItem(item) : -1;
    if (row < 0 || row >= m_entries.size()) return;

    NodeGraph::EvalContext context;
    context.sourcePath = m_entries[row].materialPath;
    context.textureRoot = m_textureRootEdit->text().trimmed();
    context.outputRoot = m_outputEdit->text().trimmed();
    context.referenceMesh = m_masks.value(currentMaterialKey()).nifPath;
    context.previewMode = true;
    context.previewMaxSize = 512; // interactive previews; conversion runs full res
    m_lastGraphResults = m_nodeEvaluator.evaluate(m_nodeGraph, context);
    m_canvas->setResults(m_lastGraphResults);
    if (!m_canvas->selectedNode().isEmpty()) {
        m_inspector->showNode(&m_nodeGraph, m_canvas->selectedNode(),
            m_lastGraphResults.value(m_canvas->selectedNode()));
        showNodeInPreview(m_canvas->selectedNode());
    }
}

void ConvertWorkspace::showNodeInPreview(const QString& nodeId)
{
    // Selecting a node puts its output on the preview surface at full
    // resolution — the graph thumbnail is only 128px, this is the real look.
    const NodeGraph::Node* node = m_nodeGraph.node(nodeId);
    const NodeGraph::NodeTypeSpec* spec = node ? m_nodeRegistry.find(node->typeId) : nullptr;
    if (!spec) return;
    const NodeGraph::NodeResult& result = m_lastGraphResults.value(nodeId);
    QImage image;
    for (const NodeGraph::PortSpec& port : spec->outputs) {
        if (port.type != NodeGraph::ValueType::Image) continue;
        const QImage candidate = result.outputs.value(port.id).image();
        if (!candidate.isNull()) { image = candidate; break; }
    }
    m_preview->canvas()->setNodeOutputImage(image);
    m_preview->setRenderMode(image.isNull()
        ? PbrPreviewGL::DebugView::Shaded : PbrPreviewGL::DebugView::NodeOutput);
}

void ConvertWorkspace::updatePreview(const QueueEntry* entry)
{
    // Source-map comparison: load the ORIGINAL game textures for the Source*
    // render modes so the split view can pair them with converted channels.
    const QString sourceKey = entry ? entry->materialPath : QString();
    if (sourceKey != m_sourceImagesFor) {
        m_sourceImagesFor = sourceKey;
        QImage diffuse, spec, normal, extra;
        Bgsm::Material source;
        if (!sourceKey.isEmpty() && Bgsm::readFile(sourceKey, &source, nullptr)) {
            const QString textureRoot = m_textureRootEdit->text().trimmed();
            const auto loadMap = [&textureRoot](const QString& authored) {
                return authored.isEmpty() ? QImage() : TextureImage::load(authored, textureRoot, nullptr);
            };
            diffuse = loadMap(source.diffuseTexture);
            spec = loadMap(!source.specularTexture.isEmpty() ? source.specularTexture : source.smoothSpecTexture);
            normal = loadMap(source.normalTexture);
            extra = loadMap(!source.lightingTexture.isEmpty() ? source.lightingTexture : source.greyscaleTexture);
        }
        m_preview->canvas()->setSourceComparisonImages(diffuse, spec, normal, extra);
    }
    // The preview always resolves against the output mod folder; converted
    // siblings carry portable Textures-relative paths.
    m_preview->setTextureRoot(QDir::fromNativeSeparators(m_outputEdit->text().trimmed())
        + QStringLiteral("/Textures"));
    if (entry && entry->hasResult && entry->result.converted
        && QFileInfo::exists(entry->result.pbrmPath)) {
        Pbr::MaterialDocument document;
        if (document.load(entry->result.pbrmPath, nullptr)) {
            m_preview->setMaterial(document);
            return;
        }
    }
    // Nothing converted on disk for this row (or no row): neutral material.
    m_preview->setMaterial(Pbr::MaterialDocument());
}

QString ConvertWorkspace::currentMaterialKey() const
{
    QTreeWidgetItem* item = m_queue->currentItem();
    const int row = item ? m_queue->indexOfTopLevelItem(item) : -1;
    return row >= 0 && row < m_entries.size() ? m_entries[row].materialPath.toLower() : QString();
}

void ConvertWorkspace::loadMaskReference()
{
    const QString key = currentMaterialKey();
    if (key.isEmpty()) { m_maskStatus->setText(tr("Select a material first.")); return; }
    const QString start = QFileInfo(QDir::fromNativeSeparators(m_sourceEdit->text())).absolutePath();
    const QString path = QFileDialog::getOpenFileName(this, tr("Choose the reference mesh"), start,
        tr("Meshes (*.nif *.fbx)"));
    if (path.isEmpty()) return;
    QString error;
    if (!m_preview->canvas()->loadCustomMesh(path, &error)) {
        m_maskStatus->setText(tr("Mesh failed: %1").arg(error));
        return;
    }
    m_preview->canvas()->setShape(PbrPreviewGL::Shape::Custom);
    MaskState& state = m_masks[key];
    state.nifPath = path;
    if (state.mask.isNull()) state.mask = MaskPaint::neutralMask();
    m_maskElements.clear();
    m_maskElementsFor.clear();
    m_maskPickEnabled->setChecked(true);
    m_preview->canvas()->setHighlightTriangles(state.filledTriangles);
    m_maskStatus->setText(tr("%1 — click parts to fill").arg(QFileInfo(path).fileName()));
}

void ConvertWorkspace::handleTrianglePicked(int triangle)
{
    const QString key = currentMaterialKey();
    if (key.isEmpty() || triangle < 0) return;
    const PreviewMesh& mesh = m_preview->canvas()->mesh();
    if (!mesh.isValid()) return;
    MaskState& state = m_masks[key];
    if (state.mask.isNull()) state.mask = MaskPaint::neutralMask();

    QVector<quint32> triangles;
    switch (m_maskFillMode->currentIndex()) {
    case 0: triangles.append(quint32(triangle)); break;
    case 1: {
        if (m_maskElementsFor != mesh.sourcePath || m_maskElements.isEmpty()) {
            m_maskElements = MaskPaint::triangleElements(mesh);
            m_maskElementsFor = mesh.sourcePath;
        }
        triangles = MaskPaint::trianglesInElement(mesh, m_maskElements, triangle);
        break;
    }
    default: triangles = MaskPaint::trianglesInSubmesh(mesh, triangle); break;
    }
    if (triangles.isEmpty()) return;

    const float value = float(m_maskFillValue->currentData().toDouble());
    MaskPaint::rasterizeFill(state.mask, mesh, triangles, value);
    if (qFuzzyCompare(value, 0.5f)) {
        for (quint32 filled : triangles) state.filledTriangles.removeAll(filled);
    } else {
        for (quint32 filled : triangles)
            if (!state.filledTriangles.contains(filled)) state.filledTriangles.append(filled);
    }
    m_preview->canvas()->setHighlightTriangles(state.filledTriangles);
    m_maskStatus->setText(tr("%n triangle(s) filled — Save Mask to apply", nullptr, int(state.filledTriangles.size())));
}

void ConvertWorkspace::clearMaskFills()
{
    const QString key = currentMaterialKey();
    if (key.isEmpty()) return;
    MaskState& state = m_masks[key];
    state.mask = MaskPaint::neutralMask();
    state.filledTriangles.clear();
    m_preview->canvas()->setHighlightTriangles({});
    m_overrides[key].maskPath.clear();
    m_maskStatus->setText(tr("Mask cleared."));
}

void ConvertWorkspace::saveMask()
{
    const QString key = currentMaterialKey();
    if (key.isEmpty()) { m_maskStatus->setText(tr("Select a material first.")); return; }
    const auto state = m_masks.constFind(key);
    if (state == m_masks.constEnd() || state->mask.isNull()) {
        m_maskStatus->setText(tr("Nothing to save — load a mesh and pick fills first."));
        return;
    }
    const QString outputRoot = QDir::fromNativeSeparators(m_outputEdit->text().trimmed());
    if (outputRoot.isEmpty()) { m_maskStatus->setText(tr("Choose an output mod folder first.")); return; }
    QTreeWidgetItem* item = m_queue->currentItem();
    const int row = m_queue->indexOfTopLevelItem(item);
    const QString baseName = QFileInfo(m_entries[row].materialPath).completeBaseName();
    const QString maskPath = outputRoot + QStringLiteral("/Masks/") + baseName + QStringLiteral("_correction.png");
    QDir().mkpath(QFileInfo(maskPath).absolutePath());
    if (!state->mask.save(maskPath)) {
        m_maskStatus->setText(tr("Could not write %1").arg(QDir::toNativeSeparators(maskPath)));
        return;
    }
    m_overrides[key].maskPath = maskPath;
    m_maskStatus->setText(tr("Saved — reconvert to apply (%1)").arg(QDir::toNativeSeparators(maskPath)));
}

void ConvertWorkspace::updateButtons()
{
    const bool haveQueue = !m_entries.isEmpty();
    m_scanButton->setEnabled(!m_running);
    m_dryRunButton->setEnabled(!m_running && haveQueue);
    m_convertSelectedButton->setEnabled(!m_running && haveQueue);
    m_convertAllButton->setEnabled(!m_running && haveQueue);
    m_cancelButton->setEnabled(m_running);
}
