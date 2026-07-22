#include "convertworkspace.h"

#include "maskpaint.h"
#include "materialdocument.h"
#include "materialpreviewwidget.h"

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
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QThread>
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
    connect(m_queue, &QTreeWidget::currentItemChanged, this, [this] { loadOverrideControls(); updateDetail(); });
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
    auto* main = new QFrame(splitter);
    auto* mainLayout = new QVBoxLayout(main);
    mainLayout->setContentsMargins(7, 7, 7, 7);
    mainLayout->setSpacing(7);

    const auto makeSpin = [main](double value, double min, double max, double step, int decimals = 2) {
        auto* spin = new QDoubleSpinBox(main);
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
    mainLayout->addLayout(actionsRow);

    // Batch options: apply to every conversion this workspace runs.
    auto* batchGroup = new QGroupBox(tr("Batch Options"), main);
    auto* batchGrid = new QGridLayout(batchGroup);
    batchGrid->setContentsMargins(9, 9, 9, 9);
    batchGrid->setHorizontalSpacing(9);
    batchGrid->setVerticalSpacing(6);
    m_recovery = makeSpin(settings.value(QStringLiteral("Convert/recovery"), 1.0).toDouble(), 0.0, 1.0, 0.05);
    m_recovery->setToolTip(tr("How strongly dark FO4 metal albedo is rebuilt from spec + cubemap hue."));
    m_glossCurve = makeSpin(settings.value(QStringLiteral("Convert/glossCurve"), 1.0).toDouble(), 0.25, 4.0, 0.05);
    m_glossCurve->setToolTip(tr("Response curve for the gloss/smoothness to roughness inversion (1 = linear)."));
    m_derivationSize = new QSpinBox(batchGroup);
    m_derivationSize->setRange(64, 2048);
    m_derivationSize->setSingleStep(64);
    m_derivationSize->setValue(settings.value(QStringLiteral("Convert/derivationSize"), 512).toInt());
    m_derivationSize->setToolTip(tr("Resolution cap for the height/AO derivations (curvature always runs full-res)."));
    batchGrid->addWidget(new QLabel(tr("Metal recovery"), batchGroup), 0, 0);
    batchGrid->addWidget(m_recovery, 0, 1);
    batchGrid->addWidget(new QLabel(tr("Gloss curve"), batchGroup), 0, 2);
    batchGrid->addWidget(m_glossCurve, 0, 3);
    batchGrid->addWidget(new QLabel(tr("Derivation size"), batchGroup), 0, 4);
    batchGrid->addWidget(m_derivationSize, 0, 5);
    batchGrid->setColumnStretch(6, 1);
    m_detailAo = new QCheckBox(tr("Detail AO into RMAOS.B (FO4 sources)"), batchGroup);
    m_detailAo->setChecked(settings.value(QStringLiteral("Convert/detailAo"), true).toBool());
    m_heightInBlue = new QCheckBox(tr("Derived height into Normal.B"), batchGroup);
    m_heightInBlue->setChecked(settings.value(QStringLiteral("Convert/heightInBlue"), false).toBool());
    m_metalDiagnostic = new QCheckBox(tr("Write metallic diagnostic mask"), batchGroup);
    m_metalDiagnostic->setChecked(settings.value(QStringLiteral("Convert/metalDiagnostic"), false).toBool());
    batchGrid->addWidget(m_detailAo, 1, 0, 1, 2);
    batchGrid->addWidget(m_heightInBlue, 1, 2, 1, 2);
    batchGrid->addWidget(m_metalDiagnostic, 1, 4, 1, 3);
    mainLayout->addWidget(batchGroup);

    // Selected material: overrides + correction masking, disabled until a
    // queue row is chosen so the scope is unmistakable.
    m_selectedGroup = new QGroupBox(tr("Selected Material"), main);
    auto* selectedLayout = new QVBoxLayout(m_selectedGroup);
    selectedLayout->setContentsMargins(9, 9, 9, 9);
    selectedLayout->setSpacing(6);

    auto* overrideRow = new QHBoxLayout();
    overrideRow->setSpacing(9);
    m_overrideEnabled = new QCheckBox(tr("Override math"), m_selectedGroup);
    m_overrideEnabled->setToolTip(tr("Tune the conversion for this material only; batch runs keep the global options."));
    overrideRow->addWidget(m_overrideEnabled);
    m_overrideRecovery = makeSpin(1.0, 0.0, 1.0, 0.05);
    m_overrideGlossCurve = makeSpin(1.0, 0.25, 4.0, 0.05);
    m_overrideSpecPercentile = makeSpin(0.98, 0.5, 1.0, 0.01);
    m_overrideSpecPercentile->setToolTip(tr("Percentile used to tame blown-out FO4 spec maps; lower clamps harder."));
    overrideRow->addWidget(new QLabel(tr("Recovery"), m_selectedGroup));
    overrideRow->addWidget(m_overrideRecovery);
    overrideRow->addWidget(new QLabel(tr("Gloss curve"), m_selectedGroup));
    overrideRow->addWidget(m_overrideGlossCurve);
    overrideRow->addWidget(new QLabel(tr("Spec clamp %"), m_selectedGroup));
    overrideRow->addWidget(m_overrideSpecPercentile);
    overrideRow->addStretch();
    m_reconvertButton = new QPushButton(tr("Reconvert This"), m_selectedGroup);
    m_reconvertButton->setToolTip(tr("Convert only the selected material with these overrides."));
    connect(m_reconvertButton, &QPushButton::clicked, this, [this] { startRun(false, RunScope::CurrentRow); });
    overrideRow->addWidget(m_reconvertButton);
    selectedLayout->addLayout(overrideRow);
    const auto storeOverride = [this] {
        QTreeWidgetItem* item = m_queue->currentItem();
        const int row = item ? m_queue->indexOfTopLevelItem(item) : -1;
        if (row < 0 || row >= m_entries.size()) return;
        const QString key = m_entries[row].materialPath.toLower();
        const QString maskPath = m_overrides.value(key).maskPath; // survives the toggle
        if (m_overrideEnabled->isChecked()) {
            m_overrides.insert(key, {float(m_overrideRecovery->value()), float(m_overrideGlossCurve->value()),
                float(m_overrideSpecPercentile->value()), maskPath});
        } else if (!maskPath.isEmpty()) {
            m_overrides.insert(key, {1.0f, 1.0f, 0.98f, maskPath});
        } else {
            m_overrides.remove(key);
        }
    };
    connect(m_overrideEnabled, &QCheckBox::toggled, this, storeOverride);
    connect(m_overrideRecovery, qOverload<double>(&QDoubleSpinBox::valueChanged), this, storeOverride);
    connect(m_overrideGlossCurve, qOverload<double>(&QDoubleSpinBox::valueChanged), this, storeOverride);
    connect(m_overrideSpecPercentile, qOverload<double>(&QDoubleSpinBox::valueChanged), this, storeOverride);

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
    mainLayout->addWidget(m_selectedGroup);

    auto* findingsGroup = new QGroupBox(tr("Findings"), main);
    auto* findingsLayout = new QVBoxLayout(findingsGroup);
    findingsLayout->setContentsMargins(9, 9, 9, 9);
    m_detail = new QListWidget(findingsGroup);
    m_detail->setWordWrap(true);
    m_detail->setFrameShape(QFrame::NoFrame);
    findingsLayout->addWidget(m_detail);
    mainLayout->addWidget(findingsGroup, 1);

    splitter->addWidget(main);

    // ---- Far right: the same live 3D preview the editor uses, showing the
    // selected material's CONVERTED sibling with the output mod folder as
    // its texture root.
    m_preview = new MaterialPreviewWidget(splitter);
    connect(m_preview->canvas(), &PbrPreviewGL::meshTrianglePicked, this, &ConvertWorkspace::handleTrianglePicked);
    splitter->addWidget(m_preview);
    splitter->setSizes({430, 620, 310});
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 0);
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
    settings.setValue(QStringLiteral("Convert/recovery"), m_recovery->value());
    settings.setValue(QStringLiteral("Convert/glossCurve"), m_glossCurve->value());
    settings.setValue(QStringLiteral("Convert/derivationSize"), m_derivationSize->value());
    settings.setValue(QStringLiteral("Convert/detailAo"), m_detailAo->isChecked());
    settings.setValue(QStringLiteral("Convert/heightInBlue"), m_heightInBlue->isChecked());
    settings.setValue(QStringLiteral("Convert/metalDiagnostic"), m_metalDiagnostic->isChecked());
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
    Converter::JobOptions job;
    job.outputRoot = QDir::fromNativeSeparators(m_outputEdit->text().trimmed());
    job.textureRoot = QDir::fromNativeSeparators(m_textureRootEdit->text().trimmed());
    job.dryRun = dryRun;
    job.detailAo = m_detailAo->isChecked();
    job.heightInBlue = m_heightInBlue->isChecked();
    job.writeMetallicDiagnostic = m_metalDiagnostic->isChecked();
    job.math.recoveryStrength = float(m_recovery->value());
    job.math.glossCurve = float(m_glossCurve->value());
    job.derive.derivationSize = m_derivationSize->value();
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
        if (override != m_overrides.constEnd()) {
            rowJob.math.recoveryStrength = override->recovery;
            rowJob.math.glossCurve = override->glossCurve;
            rowJob.math.specNormalizePercentile = override->specPercentile;
            rowJob.correctionMaskPath = override->maskPath;
        }
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

    m_worker = QThread::create([this, jobs, rows, paths] {
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
            const Converter::JobResult result = Converter::convertMaterial(paths[index], jobs[index], &cache);
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
    QTreeWidgetItem* item = m_queue->currentItem();
    const int row = item ? m_queue->indexOfTopLevelItem(item) : -1;
    const bool valid = row >= 0 && row < m_entries.size();
    const QSignalBlocker a(m_overrideEnabled), b(m_overrideRecovery), c(m_overrideGlossCurve), d(m_overrideSpecPercentile);
    m_selectedGroup->setEnabled(valid);
    m_selectedGroup->setTitle(valid
        ? tr("Selected Material — %1").arg(QFileInfo(m_entries[row].materialPath).fileName())
        : tr("Selected Material"));
    m_reconvertButton->setEnabled(valid && !m_running);
    if (!valid) { m_overrideEnabled->setChecked(false); return; }
    const auto override = m_overrides.constFind(m_entries[row].materialPath.toLower());
    const bool has = override != m_overrides.constEnd();
    m_overrideEnabled->setChecked(has);
    m_overrideRecovery->setValue(has ? override->recovery : m_recovery->value());
    m_overrideGlossCurve->setValue(has ? override->glossCurve : m_glossCurve->value());
    m_overrideSpecPercentile->setValue(has ? override->specPercentile : 0.98);
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

void ConvertWorkspace::updatePreview(const QueueEntry* entry)
{
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
    m_reconvertButton->setEnabled(!m_running && haveQueue && m_queue->currentItem());
}
