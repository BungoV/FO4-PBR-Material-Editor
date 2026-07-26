#include "convertworkspace.h"

#include "bgsmreader.h"
#include "converternodes.h"
#include "disclosureicon.h"
#include "dragnumericedit.h"
#include "maskpaint.h"
#include "materialdocument.h"
#include "materialpreviewwidget.h"
#include "nodecanvas.h"
#include "textureimage.h"

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QStyledItemDelegate>
#include <QUrl>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QEvent>
#include <QSplitter>
#include <QToolButton>
#include <QStackedWidget>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

// The canvas hint line. Every navigation key lives here or nowhere: a shortcut
// nothing mentions is a shortcut nobody finds, and Ctrl+F, Shift+F and the
// minimap all shipped without a word anywhere in the UI.
// One ink for every drawn arrow in this panel, so the disclosure triangle and
// the flagged-walk pair cannot drift apart. Matches the muted button text in
// style.qss rather than the brighter body colour.
const QColor kArrowInk(198, 203, 210);

// Hermite ease, for the brush's soft edge. A hard disc tiles visibly when
// strokes overlap; easing the outer half makes repeated passes build smoothly.
double smoothstepd(double edge0, double edge1, double value)
{
    if (edge1 <= edge0) return value < edge0 ? 0.0 : 1.0;
    const double t = std::clamp((value - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

QString canvasHint()
{
    return QObject::tr("Right-click to add nodes · drag ports to wire · K cut · "
                       "Ctrl+F find · F fit · Shift+F frame selected · minimap to jump");
}

QString statusText(const Converter::JobResult& result, bool dryRun)
{
    if (!result.errors.isEmpty() || !result.converted) return QObject::tr("Failed");
    if (result.flagged) return QObject::tr("Flagged");
    return dryRun ? QObject::tr("Would convert") : QObject::tr("Converted");
}

// Multi-selection colouring, carried over from the ClaudeNifskope fork's block
// list: every selected row takes the same blue, and the text is what separates
// the primary (current) row from the rest of the selection. One background
// keeps a ten-row selection readable as one block; two text colours still say
// which row the panels on the right are following.
class QueueRowDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
        const QModelIndex& index) const override
    {
        QStyleOptionViewItem opt = option;
        if (opt.state & QStyle::State_Selected) {
            painter->fillRect(opt.rect, QColor(74, 122, 176));
            const auto* view = qobject_cast<const QAbstractItemView*>(opt.widget);
            const bool primary = view
                && view->currentIndex().siblingAtColumn(0) == index.siblingAtColumn(0);
            const QColor ink = primary ? QColor(255, 157, 0)   // #FF9D00 primary
                                       : QColor(255, 114, 0);  // #FF7200 secondary
            opt.palette.setColor(QPalette::Text, ink);
            opt.palette.setColor(QPalette::HighlightedText, ink);
            // The base paint fills the text rect with the palette highlight for
            // selected items, which would bury the blue we just laid down. The
            // same trap the fork hit in NifDelegate::drawDisplay.
            opt.state &= ~QStyle::State_Selected;
        }
        QStyledItemDelegate::paint(painter, opt, index);
    }
};

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

    // Folders are set once per session, so they live on one summary line and
    // only unfold when they need editing. Three permanent label+field+browse
    // rows cost the queue about 140px for settings nobody revisits.
    auto* projectHeader = new QHBoxLayout();
    projectHeader->setSpacing(4);
    // One button carries both the disclosure arrow and the summary text. An
    // arrow button plus a separate label overlapped each other here, and a
    // single widget cannot.
    m_projectToggle = new QPushButton(side);
    m_projectToggle->setCheckable(true);
    m_projectToggle->setStyleSheet(QStringLiteral("text-align: left; padding: 3px 6px;"));
    m_projectToggle->setToolTip(tr("Show or hide the source, texture and output folders."));
    projectHeader->addWidget(m_projectToggle, 1);
    m_scanButton = new QPushButton(tr("Scan"), side);
    m_scanButton->setToolTip(tr("Find every .bgsm/.bgem under the source folder and build the queue."));
    connect(m_scanButton, &QPushButton::clicked, this, &ConvertWorkspace::scanSources);
    projectHeader->addWidget(m_scanButton);
    sideLayout->addLayout(projectHeader);

    m_projectDetails = new QWidget(side);
    auto* projectLayout = new QVBoxLayout(m_projectDetails);
    projectLayout->setContentsMargins(16, 2, 0, 4);
    projectLayout->setSpacing(3);
    const auto addPathRow = [&](const QString& label, const QString& settingsKey, const QString& dialogTitle) {
        auto* caption = new QLabel(label, m_projectDetails);
        caption->setObjectName(QStringLiteral("Muted"));
        projectLayout->addWidget(caption);
        auto* row = new QHBoxLayout();
        row->setSpacing(4);
        auto* edit = new QLineEdit(m_projectDetails);
        edit->setText(settings.value(settingsKey).toString());
        connect(edit, &QLineEdit::textChanged, this, [this] { updateProjectSummary(); });
        row->addWidget(edit, 1);
        auto* browse = new QPushButton(tr("…"), m_projectDetails);
        browse->setFixedWidth(28);
        connect(browse, &QPushButton::clicked, this, [this, edit, dialogTitle] { browseFolder(edit, dialogTitle); });
        row->addWidget(browse);
        projectLayout->addLayout(row);
        return edit;
    };
    m_sourceEdit = addPathRow(tr("Source materials (folder with .bgsm/.bgem)"),
        QStringLiteral("Convert/sourceRoot"), tr("Choose the source materials folder"));
    m_textureRootEdit = addPathRow(tr("Texture root (the Data folder with textures\\)"),
        QStringLiteral("Convert/textureRoot"), tr("Choose the source texture root"));
    m_outputEdit = addPathRow(tr("Output mod folder"),
        QStringLiteral("Convert/outputRoot"), tr("Choose the output mod folder"));
    sideLayout->addWidget(m_projectDetails);
    connect(m_projectToggle, &QPushButton::toggled, this, [this](bool open) {
        m_projectDetails->setVisible(open);
        updateProjectSummary();
    });
    // Unfolded on a fresh setup, folded once the folders are already filled in.
    const bool configured = !m_sourceEdit->text().trimmed().isEmpty()
        && !m_outputEdit->text().trimmed().isEmpty();
    m_projectToggle->setChecked(!configured);
    m_projectDetails->setVisible(!configured);
    updateProjectSummary();

    auto* queueTitle = new QLabel(tr("Queue"), side);
    queueTitle->setObjectName(QStringLiteral("SectionTitle"));
    sideLayout->addSpacing(4);
    sideLayout->addWidget(queueTitle);
    m_queue = new QTreeWidget(side);
    m_queue->setObjectName(QStringLiteral("ConvertQueue"));
    // The source tree's own folders, because a flat list of 400 file names
    // loses the one grouping the author already made. Metal % left with the
    // column: it only had a value after a run, and the live figure now sits in
    // the Source Maps panel where it updates while the graph is edited.
    m_queue->setHeaderLabels({tr("Material"), tr("Status")});
    m_queue->setRootIsDecorated(true);
    m_queue->setAlternatingRowColors(true);
    m_queue->setUniformRowHeights(true);
    m_queue->header()->setStretchLastSection(false);
    m_queue->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_queue->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    // Blender's outliner keys: shift for a range, ctrl to add or drop one, and
    // a held drag to sweep several. Qt's ExtendedSelection is exactly that.
    m_queue->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_queue->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_queue->setDragDropMode(QAbstractItemView::NoDragDrop);
    // Double-click is the check toggle, so it must not also be the expander or
    // one gesture would do two things on every folder row.
    m_queue->setExpandsOnDoubleClick(false);
    m_queue->setItemDelegate(new QueueRowDelegate(m_queue));
    m_queue->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_queue, &QTreeWidget::customContextMenuRequested, this, &ConvertWorkspace::showQueueMenu);
    connect(m_queue, &QTreeWidget::currentItemChanged, this, [this] {
        loadOverrideControls(); updateDetail(); syncGraphToSelection(); updateSlotInventory();
        // Primary and secondary are drawn from the current index, so moving it
        // has to repaint the rows that just swapped roles.
        m_queue->viewport()->update();
    });
    connect(m_queue->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
        updateButtons();
    });
    connect(m_queue, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        if (!item || !(item->flags() & Qt::ItemIsUserCheckable)) return;
        item->setCheckState(0, item->checkState(0) == Qt::Checked ? Qt::Unchecked : Qt::Checked);
    });
    // Ticking a row changes what Convert will do, and the button says so.
    connect(m_queue, &QTreeWidget::itemChanged, this, [this] { updateButtons(); });
    sideLayout->addWidget(m_queue, 1);

    // ---- What the selected material actually brings with it. This is the
    // question the graph cannot answer at a glance: five texture nodes each
    // hold one map, and nothing lists them together or says what is absent.
    m_slotGroup = new QGroupBox(tr("Source Maps"), side);
    auto* slotLayout = new QVBoxLayout(m_slotGroup);
    slotLayout->setContentsMargins(9, 6, 9, 8);
    slotLayout->setSpacing(4);
    m_slotList = new QTreeWidget(m_slotGroup);
    m_slotList->setObjectName(QStringLiteral("ConvertSourceMaps"));
    m_slotList->setHeaderLabels({tr("Slot"), tr("Size"), tr("Format")});
    m_slotList->setRootIsDecorated(false);
    m_slotList->setUniformRowHeights(true);
    m_slotList->setFocusPolicy(Qt::NoFocus);
    m_slotList->setSelectionMode(QAbstractItemView::NoSelection);
    m_slotList->setFrameShape(QFrame::NoFrame);
    m_slotList->setMaximumHeight(190);
    m_slotList->header()->setStretchLastSection(false);
    m_slotList->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_slotList->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_slotList->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    slotLayout->addWidget(m_slotList);
    m_slotMetal = new QLabel(m_slotGroup);
    m_slotMetal->setObjectName(QStringLiteral("Muted"));
    m_slotMetal->setWordWrap(true);
    m_slotMetal->setToolTip(tr("Share of the surface the graph is currently inferring as metal. "
                               "Updates as the graph is edited, so a levels drag can be judged on the spot."));
    slotLayout->addWidget(m_slotMetal);
    m_slotGroup->setVisible(false);
    sideLayout->addWidget(m_slotGroup);

    // The review loop: walk exactly the materials that need eyes.
    auto* flaggedRow = new QHBoxLayout();
    flaggedRow->setSpacing(4);
    m_prevFlagged = new QPushButton(tr("Flagged"), side);
    m_prevFlagged->setIcon(navigationArrowIcon(false, 12, kArrowInk));
    m_prevFlagged->setIconSize(QSize(12, 12));
    m_prevFlagged->setToolTip(tr("Select the previous flagged or failed material."));
    connect(m_prevFlagged, &QPushButton::clicked, this, [this] { stepFlagged(-1); });
    flaggedRow->addWidget(m_prevFlagged);
    m_nextFlagged = new QPushButton(tr("Flagged"), side);
    m_nextFlagged->setIcon(navigationArrowIcon(true, 12, kArrowInk));
    m_nextFlagged->setIconSize(QSize(12, 12));
    // Qt puts a button's icon before its text; mirroring this one keeps the
    // pair reading outward — back-arrow, "Flagged", "Flagged", forward-arrow —
    // which is the direction each button actually moves you.
    m_nextFlagged->setLayoutDirection(Qt::RightToLeft);
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
    // Holder only: its children are redistributed into the panes below. It is
    // never added to a layout, so Qt would otherwise park it at (0,0) at its
    // default 100x30 and paint a stray plate over the side panel's corner.
    auto* main = new QFrame(this);
    main->hide();
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
    // One convert verb that says what it will actually do. Checkboxes plus
    // "Checked" plus "All" plus a row selection was four ways to express the
    // same choice, and none of them told you the outcome.
    m_convertButton = new QPushButton(tr("Convert"), main);
    m_convertButton->setDefault(true);
    connect(m_convertButton, &QPushButton::clicked, this, [this] {
        // Same precedence the label announces. A single selected row does NOT
        // narrow the scope: clicking one row is how you inspect a material, and
        // silently turning "convert everything" into "convert this one" because
        // you looked at it would be a trap. Two or more, or a folder, is a
        // deliberate choice — and the right-click menu still covers the single.
        startRun(false, checkedCount() > 0 ? RunScope::Checked
            : selectedEntries().size() >= 2 ? RunScope::Selected : RunScope::All);
    });
    actionsRow->addWidget(m_convertButton);
    m_cancelButton = new QPushButton(tr("Cancel"), main);
    m_cancelButton->setVisible(false); // only meaningful while a run is going
    connect(m_cancelButton, &QPushButton::clicked, this, &ConvertWorkspace::cancelRun);
    actionsRow->addWidget(m_cancelButton);
    actionsRow->addStretch();
    m_openInAuthor = new QPushButton(tr("Open in Author"), main);
    m_openInAuthor->setEnabled(false);
    m_openInAuthor->setToolTip(tr("Open the converted sibling .pbrm in the Author workspace."));
    connect(m_openInAuthor, &QPushButton::clicked, this, [this] {
        const int row = currentEntryIndex();
        if (row >= 0 && m_entries[row].hasResult && QFileInfo::exists(m_entries[row].result.pbrmPath))
            emit openInAuthorRequested(m_entries[row].result.pbrmPath);
    });
    actionsRow->addWidget(m_openInAuthor);
    m_actionsRow = actionsRow;

    // Correction masking stays material-scoped, so it keeps its own group,
    // disabled until a queue row is chosen.
    // Masking is an occasional expert task, so it folds away: a first run
    // should not open onto six controls whose names mean nothing yet.
    m_selectedGroup = new QGroupBox(tr("Corrections"), side);
    m_selectedGroup->setCheckable(true);
    m_selectedGroup->setChecked(false);
    m_selectedGroup->setToolTip(tr("Paint metal/dielectric fixes onto the material where the inference guessed wrong."));
    // The heading carries the material name, which is longer than the panel;
    // the filter re-elides it whenever the splitter changes that width.
    m_selectedGroup->installEventFilter(this);
    auto* selectedLayout = new QVBoxLayout(m_selectedGroup);
    selectedLayout->setContentsMargins(9, 6, 9, 9);
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

    // Freehand brush over the 2D view. The fills above are for whole parts —
    // "this boot is not metal" — and this is for everything they cannot reach:
    // the strip of grime along a seam, the painted half of a panel. Both write
    // the same 0.5-neutral mask, so a material can use either or both.
    auto* brushRow = new QHBoxLayout();
    brushRow->setSpacing(9);
    m_maskBrushEnabled = new QCheckBox(tr("Brush"), m_selectedGroup);
    m_maskBrushEnabled->setToolTip(tr("Paint the correction mask directly in the viewport. Best in the 2D view, "
                                      "where the painted mask is tinted over the texture. Shift and Ctrl still pan "
                                      "and dolly mid-stroke."));
    connect(m_maskBrushEnabled, &QCheckBox::toggled, this, [this](bool on) {
        m_preview->canvas()->setMaskPaintEnabled(on);
        // One pointer, one meaning: arming the brush disarms the fill picker.
        if (on && m_maskPickEnabled->isChecked()) m_maskPickEnabled->setChecked(false);
        refreshMaskOverlay();
    });
    brushRow->addWidget(m_maskBrushEnabled);
    brushRow->addWidget(new QLabel(tr("Size"), m_selectedGroup));
    m_maskBrushSize = new QSlider(Qt::Horizontal, m_selectedGroup);
    m_maskBrushSize->setRange(2, 250);   // thousandths of the UV square
    m_maskBrushSize->setValue(40);
    m_maskBrushSize->setMaximumWidth(110);
    m_maskBrushSize->setToolTip(tr("Brush radius as a fraction of the texture, so it keeps its size on the "
                                   "material however far you zoom."));
    connect(m_maskBrushSize, &QSlider::valueChanged, this, [this](int value) {
        m_preview->canvas()->setMaskBrushRadius(float(value) / 1000.0f);
    });
    brushRow->addWidget(m_maskBrushSize);
    m_maskBrushFlow = new QSlider(Qt::Horizontal, m_selectedGroup);
    m_maskBrushFlow->setRange(5, 100);
    m_maskBrushFlow->setValue(60);
    m_maskBrushFlow->setMaximumWidth(80);
    m_maskBrushFlow->setToolTip(tr("How fast a stroke reaches full strength. Below 100 it builds up, so a light "
                                   "pass nudges the inference instead of overriding it outright."));
    brushRow->addWidget(new QLabel(tr("Flow"), m_selectedGroup));
    brushRow->addWidget(m_maskBrushFlow);
    m_maskBrushValue = new QComboBox(m_selectedGroup);
    // Dielectric first: the FO4 stencil over-detects by design, so subtracting
    // is the common direction and should be the one already selected.
    m_maskBrushValue->addItem(tr("Paint Dielectric"), 0.0);
    m_maskBrushValue->addItem(tr("Paint Metal"), 1.0);
    m_maskBrushValue->addItem(tr("Erase to Inferred"), 0.5);
    m_maskBrushValue->setToolTip(tr("What the brush paints. Erase returns those texels to whatever the graph inferred."));
    brushRow->addWidget(m_maskBrushValue);
    brushRow->addStretch();
    selectedLayout->addLayout(brushRow);

    m_maskStatus = new QLabel(m_selectedGroup);
    m_maskStatus->setObjectName(QStringLiteral("Muted"));
    m_maskStatus->setWordWrap(true);
    selectedLayout->addWidget(m_maskStatus);
    m_selectedGroup->setEnabled(false);
    // A checkable QGroupBox only greys its children out; hide them so the
    // folded state actually reclaims the space.
    const auto foldCorrections = [this](bool open) {
        for (QObject* child : m_selectedGroup->children())
            if (auto* widget = qobject_cast<QWidget*>(child)) widget->setVisible(open);
        m_selectedGroup->setMaximumHeight(open ? QWIDGETSIZE_MAX : 26);
    };
    connect(m_selectedGroup, &QGroupBox::toggled, this, foldCorrections);
    foldCorrections(false);
    sideLayout->addWidget(m_selectedGroup);

    // Findings only earn space when they have something to say.
    m_findingsGroup = new QGroupBox(tr("Findings"), side);
    auto* findingsLayout = new QVBoxLayout(m_findingsGroup);
    findingsLayout->setContentsMargins(9, 9, 9, 9);
    m_detail = new QListWidget(m_findingsGroup);
    m_detail->setWordWrap(true);
    m_detail->setFrameShape(QFrame::NoFrame);
    m_detail->setMaximumHeight(150);
    findingsLayout->addWidget(m_detail);
    m_findingsGroup->setVisible(false);
    sideLayout->addWidget(m_findingsGroup);

    // The centre pane IS the graph: the conversion's settings are its node
    // parameters, so there is no second panel to switch to.
    buildGraphMode(splitter);

    // ---- Far right: the same live 3D preview the editor uses, showing the
    // selected material's CONVERTED sibling with the output mod folder as
    // its texture root.
    m_preview = new MaterialPreviewWidget(splitter, /*converterMode=*/true);
    connect(m_preview->canvas(), &PbrPreviewGL::meshTrianglePicked, this, &ConvertWorkspace::handleTrianglePicked);
    connect(m_preview->canvas(), &PbrPreviewGL::maskPainted, this, &ConvertWorkspace::handleMaskPainted);
    splitter->addWidget(m_preview);
    // The canvas is the work surface, so it takes the width the folded
    // setup rows and the deleted inspector column gave back.
    splitter->setSizes({330, 990, 600});
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
    m_rowItems.clear();
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
    // Rebuild the tree the source folders already describe. Folders are real
    // rows: they can be selected, and ticking one ticks everything under it.
    const QDir sourceDir(source);
    m_rowItems.clear();
    m_rowItems.resize(m_entries.size()); // value-initialised: every slot starts null
    QHash<QString, QTreeWidgetItem*> folders;
    for (int index = 0; index < m_entries.size(); ++index) {
        const QStringList parts = sourceDir.relativeFilePath(m_entries[index].materialPath)
            .split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (parts.isEmpty()) continue;
        QTreeWidgetItem* parent = nullptr;
        QString folderPath;
        for (int part = 0; part + 1 < parts.size(); ++part) {
            if (!folderPath.isEmpty()) folderPath += QLatin1Char('/');
            folderPath += parts.at(part);
            QTreeWidgetItem*& folder = folders[folderPath];
            if (!folder) {
                folder = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_queue);
                folder->setText(0, parts.at(part));
                folder->setToolTip(0, QDir::toNativeSeparators(folderPath));
                folder->setFirstColumnSpanned(true);
                // Auto-tristate is what makes a folder a useful handle: tick it
                // and the whole subtree follows, and it goes partial on its own
                // when only some children are ticked.
                folder->setFlags(folder->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
                folder->setData(0, Qt::UserRole, -1);
                folder->setCheckState(0, Qt::Unchecked);
            }
            parent = folder;
        }
        auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_queue);
        item->setText(0, parts.last());
        item->setToolTip(0, QDir::toNativeSeparators(m_entries[index].materialPath));
        item->setText(1, tr("Pending"));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        // The row's identity, because its position in the tree is not its index
        // in m_entries once folders are in the way.
        item->setData(0, Qt::UserRole, index);
        item->setCheckState(0, Qt::Unchecked);
        m_rowItems[index] = item;
    }
    // Showing the structure means showing what is in it; a tree that opens
    // fully collapsed has hidden the materials to reveal the folders. Very
    // large scans stop at the top level, where expanding all would be a wall.
    if (m_entries.size() <= 500) m_queue->expandAll();
    else m_queue->expandToDepth(0);
    m_queueSummary->setText(folders.isEmpty()
        ? tr("%n material(s) queued.", nullptr, int(m_entries.size()))
        : tr("%1 in %n folder(s).", nullptr, int(folders.size()))
            .arg(tr("%n material(s)", nullptr, int(m_entries.size()))));
    updateFlaggedSummary();
    loadOverrideControls();
    updateSlotInventory();
    updateButtons();
}

void ConvertWorkspace::selectFirstMaterial()
{
    if (QTreeWidgetItem* item = itemForEntry(0)) {
        for (QTreeWidgetItem* parent = item->parent(); parent; parent = parent->parent())
            parent->setExpanded(true);
        m_queue->setCurrentItem(item);
    }
}

int ConvertWorkspace::entryIndexOf(const QTreeWidgetItem* item) const
{
    if (!item) return -1;
    const QVariant index = item->data(0, Qt::UserRole);
    if (!index.isValid()) return -1;
    const int row = index.toInt();
    return row >= 0 && row < m_entries.size() ? row : -1;
}

int ConvertWorkspace::currentEntryIndex() const
{
    return entryIndexOf(m_queue->currentItem());
}

QTreeWidgetItem* ConvertWorkspace::itemForEntry(int index) const
{
    return index >= 0 && index < m_rowItems.size() ? m_rowItems.at(index) : nullptr;
}

QVector<int> ConvertWorkspace::selectedEntries() const
{
    // A selected folder stands for every material under it, so "select the
    // armour folder, convert" needs no expanding first.
    QSet<int> found;
    const QList<QTreeWidgetItem*> selection = m_queue->selectedItems();
    for (QTreeWidgetItem* item : selection) {
        const int row = entryIndexOf(item);
        if (row >= 0) { found.insert(row); continue; }
        QVector<QTreeWidgetItem*> stack{item};
        while (!stack.isEmpty()) {
            QTreeWidgetItem* node = stack.takeLast();
            for (int child = 0; child < node->childCount(); ++child) {
                QTreeWidgetItem* kid = node->child(child);
                const int kidRow = entryIndexOf(kid);
                if (kidRow >= 0) found.insert(kidRow);
                else stack.append(kid);
            }
        }
    }
    QVector<int> rows(found.cbegin(), found.cend());
    std::sort(rows.begin(), rows.end());
    return rows;
}

QVector<int> ConvertWorkspace::rowsForScope(RunScope scope) const
{
    if (scope == RunScope::Selected) return selectedEntries();
    const int currentRow = currentEntryIndex();
    QVector<int> rows;
    for (int row = 0; row < m_entries.size(); ++row) {
        const QTreeWidgetItem* item = itemForEntry(row);
        if (!item) continue;
        if (scope == RunScope::Checked && item->checkState(0) != Qt::Checked) continue;
        if (scope == RunScope::CurrentRow && row != currentRow) continue;
        rows.append(row);
    }
    return rows;
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
    const QVector<int> rows = rowsForScope(scope);
    for (int row : rows) {
        if (QTreeWidgetItem* item = itemForEntry(row)) item->setText(1, tr("Queued"));
        m_entries[row].hasResult = false;
    }
    if (rows.isEmpty()) {
        m_queueSummary->setText(scope == RunScope::CurrentRow ? tr("Select a material to reconvert.")
            : scope == RunScope::Selected ? tr("Select at least one material or folder.")
                                          : tr("Check at least one material for a selected run."));
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
                if (QTreeWidgetItem* item = itemForEntry(row)) item->setText(1, tr("Converting…"));
            }, Qt::QueuedConnection);
            // The graph is the pipeline: it produces this material's images,
            // and the orchestrator writes them exactly as it writes a batch
            // conversion (proven byte-identical).
            Converter::JobOptions rowJob = jobs[index];
            QStringList graphErrors;
            rowJob.supplied = ConverterNodes::runGraph(graph, paths[index],
                rowJob.textureRoot, rowJob.geometryNifPath, rowJob.correctionMaskPath, &graphErrors);
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
    const int start = currentEntryIndex();
    for (int step = 1; step <= count; ++step) {
        const int row = ((start + direction * step) % count + count) % count;
        const QueueEntry& entry = m_entries[row];
        if (!entry.hasResult || (!entry.result.flagged && entry.result.errors.isEmpty())) continue;
        QTreeWidgetItem* item = itemForEntry(row);
        if (!item) continue;
        // A flagged row inside a collapsed folder has to be opened to, or the
        // walk silently stops moving.
        for (QTreeWidgetItem* parent = item->parent(); parent; parent = parent->parent())
            parent->setExpanded(true);
        m_queue->setCurrentItem(item);
        m_queue->scrollToItem(item);
        return;
    }
}

void ConvertWorkspace::loadOverrideControls()
{
    // Conversion tuning moved into the graph; only the mask tools are still
    // scoped to one material.
    const int row = currentEntryIndex();
    const bool valid = row >= 0;
    m_selectedGroup->setEnabled(valid);
    m_correctionsMaterial = valid ? QFileInfo(m_entries[row].materialPath).fileName() : QString();
    updateCorrectionsTitle();
}

void ConvertWorkspace::updateCorrectionsTitle()
{
    if (!m_selectedGroup) return;
    if (m_correctionsMaterial.isEmpty()) {
        m_selectedGroup->setTitle(tr("Corrections"));
        return;
    }
    // A QGroupBox title does not elide — it just runs off the panel and gets
    // cut mid-word, which is what these names do at this width. Budget is the
    // box minus the checkbox, the border insets and the fixed prefix.
    const QString prefix = tr("Corrections — ");
    const QFontMetrics metrics(m_selectedGroup->font());
    const int budget = m_selectedGroup->width() - metrics.horizontalAdvance(prefix) - 46;
    // Too narrow to say anything useful about which material: drop the name
    // rather than leaving a dangling dash. The tooltip still carries it.
    m_selectedGroup->setTitle(budget > 40
        ? prefix + metrics.elidedText(m_correctionsMaterial, Qt::ElideMiddle, budget)
        : tr("Corrections"));
    m_selectedGroup->setToolTip(tr("Paint metal/dielectric fixes onto %1 where the inference "
                                   "guessed wrong.").arg(m_correctionsMaterial));
}

bool ConvertWorkspace::eventFilter(QObject* watched, QEvent* event)
{
    // The title has to be re-elided whenever the panel changes width, and the
    // splitter resizes the panel without ever resizing this widget.
    if (watched == m_selectedGroup && event->type() == QEvent::Resize) updateCorrectionsTitle();
    return QWidget::eventFilter(watched, event);
}

void ConvertWorkspace::showQueueMenu(const QPoint& pos)
{
    QTreeWidgetItem* under = m_queue->itemAt(pos);
    // Right-clicking outside the selection moves to what was clicked, which is
    // what every file manager does; right-clicking inside it keeps the set.
    if (under && !m_queue->selectedItems().contains(under)) {
        m_queue->setCurrentItem(under);
        under->setSelected(true);
    }
    const QVector<int> rows = selectedEntries();
    const int current = currentEntryIndex();

    QMenu menu(this);
    QAction* convert = menu.addAction(rows.size() == 1
        ? tr("Convert This Material") : tr("Convert %n Selected", nullptr, int(rows.size())));
    convert->setEnabled(!rows.isEmpty() && !m_running);
    connect(convert, &QAction::triggered, this, [this] { startRun(false, RunScope::Selected); });
    QAction* dry = menu.addAction(tr("Dry Run Selected"));
    dry->setEnabled(!rows.isEmpty() && !m_running);
    connect(dry, &QAction::triggered, this, [this] { startRun(true, RunScope::Selected); });

    menu.addSeparator();
    QAction* check = menu.addAction(tr("Check Selected"));
    check->setEnabled(!rows.isEmpty());
    connect(check, &QAction::triggered, this, [this] { setCheckedForSelection(true); });
    QAction* uncheck = menu.addAction(tr("Uncheck Selected"));
    uncheck->setEnabled(!rows.isEmpty());
    connect(uncheck, &QAction::triggered, this, [this] { setCheckedForSelection(false); });

    menu.addSeparator();
    const bool converted = current >= 0 && m_entries[current].hasResult
        && m_entries[current].result.converted
        && QFileInfo::exists(m_entries[current].result.pbrmPath);
    QAction* author = menu.addAction(tr("Open in Author"));
    author->setEnabled(converted);
    connect(author, &QAction::triggered, this, [this, current] {
        emit openInAuthorRequested(m_entries[current].result.pbrmPath);
    });
    QAction* reveal = menu.addAction(tr("Show in Explorer"));
    reveal->setEnabled(current >= 0);
    connect(reveal, &QAction::triggered, this, [this, current] {
        const QString path = QDir::toNativeSeparators(m_entries[current].materialPath);
        // /select, needs the argument unquoted-but-whole, so it goes through
        // the argument list rather than a composed command line.
        if (!QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,") + path}))
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
    });
    QAction* copy = menu.addAction(rows.size() > 1 ? tr("Copy %n Paths", nullptr, int(rows.size()))
                                                   : tr("Copy Path"));
    copy->setEnabled(!rows.isEmpty());
    connect(copy, &QAction::triggered, this, [this, rows] {
        QStringList paths;
        for (int row : rows) paths.append(QDir::toNativeSeparators(m_entries[row].materialPath));
        QGuiApplication::clipboard()->setText(paths.join(QLatin1Char('\n')));
    });

    menu.addSeparator();
    connect(menu.addAction(tr("Expand All")), &QAction::triggered, m_queue, &QTreeWidget::expandAll);
    connect(menu.addAction(tr("Collapse All")), &QAction::triggered, m_queue, &QTreeWidget::collapseAll);
    menu.exec(m_queue->viewport()->mapToGlobal(pos));
}

void ConvertWorkspace::setCheckedForSelection(bool checked)
{
    const QVector<int> rows = selectedEntries();
    // One repaint and one button refresh for the whole batch: itemChanged fires
    // per row, and per-row work over a folder full of materials is visible.
    const QSignalBlocker block(m_queue);
    for (int row : rows)
        if (QTreeWidgetItem* item = itemForEntry(row))
            item->setCheckState(0, checked ? Qt::Checked : Qt::Unchecked);
    updateButtons();
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
    QTreeWidgetItem* item = itemForEntry(row);
    if (!item) return;
    item->setText(1, statusText(result, m_lastRunDry));
    if (m_queue->currentItem() == item) { updateDetail(); updateSlotInventory(); }
}

void ConvertWorkspace::updateDetail()
{
    m_detail->clear();
    const int row = currentEntryIndex();
    const bool valid = row >= 0;
    updatePreview(valid ? &m_entries[row] : nullptr);
    // Nothing to report means no panel: an empty box that says "nothing yet"
    // is just reserved space.
    if (!valid || !m_entries[row].hasResult) {
        if (m_findingsGroup) m_findingsGroup->setVisible(false);
        m_openInAuthor->setEnabled(false);
        return;
    }
    if (m_findingsGroup) m_findingsGroup->setVisible(true);
    const QueueEntry& entry = m_entries[row];
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

void ConvertWorkspace::updateSlotInventory()
{
    if (!m_slotGroup) return;
    const int row = currentEntryIndex();
    if (row < 0) {
        m_slotGroup->setVisible(false);
        m_slotInventoryFor.clear();
        return;
    }

    const QString path = m_entries[row].materialPath;
    // One header read per slot, so this rebuilds when the material changes and
    // not on every graph evaluation — the metal line below is the live part.
    if (path != m_slotInventoryFor) {
        m_slotInventoryFor = path;
        m_slotList->clear();
        Bgsm::Material material;
        if (Bgsm::readFile(path, &material, nullptr)) {
            // Labelled with the FO76 suffix the map is normally authored under,
            // because that is what the file names on disk say.
            const QVector<QPair<QString, QString>> declared = {
                {tr("Diffuse (_d)"), material.kind == Bgsm::Kind::Bgem ? material.baseTexture
                                                                       : material.diffuseTexture},
                {tr("Normal (_n)"), material.normalTexture},
                {tr("Reflectance (_r)"), material.specularTexture},
                {tr("Lighting (_l)"), material.lightingTexture},
                {tr("Smooth/Spec (_s)"), material.smoothSpecTexture},
                {tr("Glow (_g)"), material.glowTexture},
                {tr("Greyscale"), material.greyscaleTexture},
                {tr("Envmap"), material.envmapTexture},
                {tr("Envmap Mask (_m)"), material.envmapMaskTexture},
                {tr("Inner Layer"), material.innerLayerTexture},
                {tr("Wrinkles"), material.wrinklesTexture},
                {tr("Flow"), material.flowTexture},
                {tr("Displacement"), material.displacementTexture},
            };
            const QString textureRoot = m_textureRootEdit->text().trimmed();
            int best = 0;
            for (const auto& slot : declared) {
                if (slot.second.trimmed().isEmpty()) continue;
                const TextureImage::Diagnostics info = TextureImage::inspect(slot.second, textureRoot);
                auto* item = new QTreeWidgetItem(m_slotList);
                item->setText(0, slot.first);
                item->setToolTip(0, QDir::toNativeSeparators(slot.second));
                if (info.valid) {
                    item->setText(1, QStringLiteral("%1×%2").arg(info.width).arg(info.height));
                    item->setText(2, info.compressionLabel());
                    best = std::max(best, std::max(info.width, info.height));
                } else {
                    // A slot the material names but the disk does not have is
                    // the failure this panel exists to make obvious.
                    item->setText(1, tr("missing"));
                    for (int column = 0; column < 3; ++column)
                        item->setForeground(column, QColor(226, 118, 108));
                    item->setToolTip(0, tr("%1\nNot found under the texture root.")
                        .arg(QDir::toNativeSeparators(slot.second)));
                }
            }
            // Under-sized maps are only meaningful against the material's own
            // best one, so mark them once the ceiling is known.
            for (int index = 0; best > 0 && index < m_slotList->topLevelItemCount(); ++index) {
                QTreeWidgetItem* item = m_slotList->topLevelItem(index);
                const QStringList size = item->text(1).split(QChar(0x00D7));
                if (size.size() != 2) continue;
                if (std::max(size.at(0).toInt(), size.at(1).toInt()) < best)
                    item->setForeground(1, QColor(232, 176, 74));
            }
        }
        if (m_slotList->topLevelItemCount() == 0)
            new QTreeWidgetItem(m_slotList, {tr("No textures declared")});
    }

    // The live metal share, which is what the removed Metal % column reported
    // only after a run. Read from whichever inference node published it, and
    // failing that measured off the packed RMAOS the terminal is holding.
    float coverage = -1.0f;
    for (const NodeGraph::Node& node : m_nodeGraph.nodes) {
        if (!node.typeId.startsWith(QStringLiteral("convert."))) continue;
        const NodeGraph::Value value =
            m_lastGraphResults.value(node.id).outputs.value(QStringLiteral("metalCoverage"));
        if (value.isValid()) { coverage = value.scalar(); break; }
    }
    if (coverage < 0.0f) {
        for (const NodeGraph::Node& node : m_nodeGraph.nodes) {
            if (node.typeId != QStringLiteral("output.pbrMaterial")) continue;
            QImage rmaos = portImage(node.id, QStringLiteral("rmaos"), false);
            if (rmaos.isNull()) break;
            if (rmaos.format() != QImage::Format_RGBA8888)
                rmaos = rmaos.convertToFormat(QImage::Format_RGBA8888);
            // Strided: this runs on every evaluation, and a metal share does
            // not need every texel of a preview-sized map to be accurate.
            qint64 metal = 0, total = 0;
            for (int y = 0; y < rmaos.height(); y += 2) {
                const uchar* line = rmaos.constScanLine(y);
                for (int x = 0; x < rmaos.width(); x += 2) {
                    if (line[x * 4 + 1] > 127) ++metal;
                    ++total;
                }
            }
            if (total > 0) coverage = float(double(metal) / double(total));
            break;
        }
    }
    m_slotMetal->setText(coverage < 0.0f
        ? tr("Metal: not evaluated yet")
        : tr("Metal: %1% of the surface").arg(int(std::lround(coverage * 100.0f))));
    m_slotGroup->setVisible(true);
}

void ConvertWorkspace::buildGraphMode(QSplitter* parent)
{
    ConverterNodes::registerAll(m_nodeRegistry);

    auto* page = new QWidget(parent);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    if (m_actionsRow) { layout->addLayout(m_actionsRow); m_actionsRow = nullptr; }

    // Graph controls belong to the canvas, not to the conversion toolbar they
    // used to sit beside — a row of them up top read as conversion verbs.
    auto* canvasTools = new QWidget(page);
    canvasTools->setObjectName(QStringLiteral("CanvasTools"));
    auto* actions = new QHBoxLayout(canvasTools);
    actions->setContentsMargins(4, 3, 4, 3);
    actions->setSpacing(3);
    const auto toolButton = [&](const QString& text, const QString& tip) {
        auto* button = new QToolButton(canvasTools);
        button->setText(text);
        button->setToolTip(tip);
        button->setAutoRaise(true);
        actions->addWidget(button);
        return button;
    };
    // Upscale policy rides on the canvas rather than in Settings: it is a
    // per-run decision that gets flipped while looking at the graph, not a
    // preference set once and forgotten.
    m_upscalePolicy = new QComboBox(canvasTools);
    m_upscalePolicy->addItem(tr("Upscale: off"), 0);
    m_upscalePolicy->addItem(tr("Upscale: match"), 1);
    m_upscalePolicy->addItem(tr("Upscale: 2x"), 2);
    m_upscalePolicy->addItem(tr("Upscale: 4x"), 3);
    m_upscalePolicy->setToolTip(tr("How far under-sized maps are raised, measured against each "
                                   "material's own highest-resolution map. Every Upscale node left "
                                   "on \"Project setting\" follows this."));
    {
        const int stored = QSettings().value(QStringLiteral("Convert/upscalePolicy"), 0).toInt();
        const int index = m_upscalePolicy->findData(stored);
        if (index >= 0) m_upscalePolicy->setCurrentIndex(index);
    }
    connect(m_upscalePolicy, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        QSettings().setValue(QStringLiteral("Convert/upscalePolicy"),
            m_upscalePolicy->itemData(index).toInt());
        // Re-evaluate so the graph shows the new targets straight away.
        m_graphEval->start();
    });
    actions->addWidget(m_upscalePolicy);

    QToolButton* reset = toolButton(tr("Reset Graph"),
        tr("Rebuild the profile's stock conversion graph, discarding edits."));
    connect(reset, &QToolButton::clicked, this, [this] {
        m_graphProfile.clear();
        syncGraphToSelection();
    });
    QToolButton* arrange = toolButton(tr("Arrange"), tr("Reflow the graph into dependency columns."));
    connect(arrange, &QToolButton::clicked, this, [this] {
        if (!m_canvas) return;
        NodeGraph::layoutGraph(m_nodeGraph, m_nodeRegistry);
        m_canvas->setGraph(m_nodeGraph);
        m_canvas->frameAll();
    });
    QToolButton* frame = toolButton(tr("Frame"), tr("Fit the whole graph in view (F)."));
    connect(frame, &QToolButton::clicked, this, [this] { if (m_canvas) m_canvas->frameAll(); });
    QToolButton* save = toolButton(tr("Save…"),
        tr("Write a .pbrgraph that `pbrm convert --graph` can run headlessly."));
    connect(save, &QToolButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, tr("Save conversion graph"),
            m_outputEdit->text().trimmed(), tr("Conversion graphs (*.pbrgraph)"));
        if (path.isEmpty()) return;
        QString error;
        if (!ConverterNodes::saveGraphFile(m_nodeGraph, path, &error))
            QMessageBox::warning(this, tr("Save graph"), error);
    });
    QToolButton* load = toolButton(tr("Load…"), tr("Open a saved .pbrgraph."));
    connect(load, &QToolButton::clicked, this, [this] {
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
    // The canvas gets the whole pane: settings are edited in the node bodies,
    // so the old inspector column was a permanently near-empty strip.
    m_canvas = new NodeCanvas(m_nodeRegistry, page);
    m_canvas->setCornerWidget(canvasTools);
    // The converter has its own undo timeline; the authoring stack belongs to
    // the document and must not be disturbed by graph edits.
    auto* undoAction = new QAction(tr("Undo graph edit"), this);
    undoAction->setShortcut(QKeySequence::Undo);
    undoAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(undoAction, &QAction::triggered, this, [this] { if (m_graphUndo.canUndo()) m_graphUndo.undo(); });
    auto* redoAction = new QAction(tr("Redo graph edit"), this);
    redoAction->setShortcut(QKeySequence::Redo);
    redoAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(redoAction, &QAction::triggered, this, [this] { if (m_graphUndo.canRedo()) m_graphUndo.redo(); });
    m_canvas->addAction(undoAction);
    m_canvas->addAction(redoAction);
    m_canvas->setContextMenuPolicy(Qt::DefaultContextMenu);
    m_canvas->setPlaceholder(tr("1.  Set the source materials folder on the left and press Scan\n"
                                "2.  Pick a material in the queue\n"
                                "3.  Its conversion graph appears here — tune it, then Convert"));
    layout->addWidget(m_canvas, 1);

    // What the inspector column was actually still good for.
    m_nodeHint = new QLabel(page);
    m_nodeHint->setObjectName(QStringLiteral("Muted"));
    m_nodeHint->setText(canvasHint());
    layout->addWidget(m_nodeHint);
    parent->addWidget(page);

    // Debounced re-evaluation: dragging a node or a slider should not run the
    // pipeline on every intermediate value.
    m_graphEval = new QTimer(this);
    m_graphEval->setSingleShot(true);
    m_graphEval->setInterval(180);
    connect(m_graphEval, &QTimer::timeout, this, [this] { m_graphRefined = false; evaluateGraph(); });
    m_graphRefine = new QTimer(this);
    m_graphRefine->setSingleShot(true);
    m_graphRefine->setInterval(500);
    connect(m_graphRefine, &QTimer::timeout, this, [this] { m_graphRefined = true; evaluateGraph(); });

    connect(m_canvas, &NodeCanvas::graphEdited, this, [this] {
        m_nodeGraph = m_canvas->graph();
        recordGraphEdit(tr("Edit graph"));
        m_graphEval->start();
    });
    // Picking a channel on the left drives the same per-node isolation the
    // node's own R/G/B/A chips use, so the two controls cannot disagree about
    // what you are looking at.
    connect(m_preview, &MaterialPreviewWidget::splitChannelsChanged, this, [this](int mask) {
        const QString selected = m_canvas->selectedNode();
        if (selected.isEmpty()) return;
        m_canvas->setNodePreviewChannels(selected, mask);
        m_nodeGraph = m_canvas->graph();
        showNodeInPreview(selected);
    });
    connect(m_canvas, &NodeCanvas::nodeSelected, this, [this](const QString& nodeId) {
        if (nodeId.isEmpty()) {
            m_nodeHint->setText(canvasHint());
            // Back to the converted material on the surface. With nothing
            // selected there is only one surface, so the left-half picker has
            // nothing to control and goes away.
            m_preview->canvas()->setNodeOutputImage(QImage());
            m_preview->setRenderModeLeft(PbrPreviewGL::DebugView::Shaded);
            m_preview->setSplitPickerVisible(false);
            return;
        }
        m_preview->setSplitPickerVisible(true);
        syncSplitPicker(nodeId);
        describeNode(nodeId);
        showNodeInPreview(nodeId);
    });
    // Picking a map on the left swaps which of a material node's textures is on
    // the surface; "Material" (an empty id) goes back to shading the lot.
    connect(m_preview, &MaterialPreviewWidget::splitMapChanged, this, [this](const QString& portId) {
        const QString selected = m_canvas->selectedNode();
        if (selected.isEmpty()) return;
        if (portId.isEmpty()) m_nodeMapPort.remove(selected);
        else m_nodeMapPort.insert(selected, portId);
        showNodeInPreview(selected);
    });
    // Settings are edited in the node body; adopt the canvas copy so the
    // workspace's graph and the drawn one never disagree.
    connect(m_canvas, &NodeCanvas::nodeParamsChanged, this, [this](const QString& nodeId) {
        m_nodeGraph = m_canvas->graph();
        describeNode(nodeId);
    });
}

void ConvertWorkspace::describeNode(const QString& nodeId)
{
    const NodeGraph::Node* node = m_nodeGraph.node(nodeId);
    const NodeGraph::NodeTypeSpec* spec = node ? m_nodeRegistry.find(node->typeId) : nullptr;
    if (!spec) return;
    const NodeGraph::NodeResult& result = m_lastGraphResults.value(nodeId);
    QString text = QStringLiteral("<b>%1</b> — %2").arg(spec->name, spec->summary);
    if (!result.error.isEmpty()) text += QStringLiteral("  ·  %1").arg(result.error);
    else if (!result.warning.isEmpty()) text += QStringLiteral("  ·  %1").arg(result.warning);
    m_nodeHint->setText(text);
}

void ConvertWorkspace::syncGraphToSelection()
{
    if (!m_canvas) return;
    const int row = currentEntryIndex();
    if (row < 0) return;

    Bgsm::Material material;
    if (!Bgsm::readFile(m_entries[row].materialPath, &material, nullptr)) return;
    const QString profile = material.version > 2 ? QStringLiteral("FO76") : QStringLiteral("FO4");
    // Computed once per material: every node compares against this to decide
    // whether it is carrying an under-sized map.
    m_materialMaxResolution = ConverterNodes::materialMaxResolution(
        material, m_textureRootEdit->text().trimmed());
    // Edits survive moving between materials — including across titles. Each
    // profile's graph is remembered, so a node you deleted stays deleted
    // instead of reappearing the next time that title comes round. Only
    // "Reset Graph" rebuilds from the stock definition.
    if (profile != m_graphProfile) {
        if (!m_graphProfile.isEmpty()) m_graphsByProfile.insert(m_graphProfile, m_nodeGraph);
        m_graphProfile = profile;
        auto remembered = m_graphsByProfile.constFind(profile);
        m_nodeGraph = remembered != m_graphsByProfile.constEnd()
            ? remembered.value() : ConverterNodes::defaultGraphFor(material);
        m_canvas->setGraph(m_nodeGraph);
        m_canvas->frameAll();
        // A different graph is a different undo timeline.
        m_graphUndo.clear();
        m_graphSnapshot = m_nodeGraph.toJson();
    }
    m_graphEval->start();
}

void ConvertWorkspace::pushLiveMaterialToPreview()
{
    // The converted result comes from the graph, not from disk: previewing it
    // used to require converting first, which made the terminal node look
    // broken on a Pending material.
    for (const NodeGraph::Node& node : m_nodeGraph.nodes) {
        if (node.typeId != QStringLiteral("output.pbrMaterial")) continue;
        // Read off the terminal's inputs, not its outputs: it has none, and
        // what it holds is exactly what was wired into it.
        const QImage base = portImage(node.id, QStringLiteral("baseColor"), false);
        if (base.isNull()) continue;
        m_preview->canvas()->setLiveMaterialImages(base,
            portImage(node.id, QStringLiteral("normal"), false),
            portImage(node.id, QStringLiteral("rmaos"), false),
            portImage(node.id, QStringLiteral("emissive"), false));
        // The RIGHT half is the finished PBR material, always. It is the
        // reference every node edit gets judged against, so it cannot be
        // left on whatever channel view happened to be persisted. The left
        // half stays the selected node's, under its own picker.
        m_preview->setRenderMode(PbrPreviewGL::DebugView::Shaded);
        return;
    }
    m_preview->canvas()->clearLiveMaterial();
}

void ConvertWorkspace::recordGraphEdit(const QString& text)
{
    // Snapshot undo: the graph is small and JSON round-trips exactly, so a
    // before/after pair is simpler and safer than per-operation commands.
    const QJsonObject after = m_nodeGraph.toJson();
    if (after == m_graphSnapshot) return;
    const QJsonObject before = m_graphSnapshot;
    m_graphSnapshot = after;

    class GraphEdit final : public QUndoCommand {
    public:
        GraphEdit(ConvertWorkspace* workspace, QJsonObject before, QJsonObject after, const QString& label)
            : QUndoCommand(label), m_workspace(workspace), m_before(std::move(before)), m_after(std::move(after)) {}
        void undo() override { m_workspace->applyGraphState(m_before); }
        void redo() override { if (m_first) { m_first = false; return; } m_workspace->applyGraphState(m_after); }
    private:
        ConvertWorkspace* m_workspace;
        QJsonObject m_before, m_after;
        bool m_first = true; // the edit is already applied when it is pushed
    };
    m_graphUndo.push(new GraphEdit(this, before, after, text));
}

void ConvertWorkspace::applyGraphState(const QJsonObject& state)
{
    m_nodeGraph = NodeGraph::Graph::fromJson(state);
    m_graphSnapshot = state;
    m_canvas->setGraph(m_nodeGraph);
    m_graphEval->start();
}

void ConvertWorkspace::evaluateGraph()
{
    if (!m_canvas) return;
    const int row = currentEntryIndex();
    if (row < 0) return;

    NodeGraph::EvalContext context;
    context.sourcePath = m_entries[row].materialPath;
    context.textureRoot = m_textureRootEdit->text().trimmed();
    context.outputRoot = m_outputEdit->text().trimmed();
    context.referenceMesh = m_masks.value(currentMaterialKey()).nifPath;
    context.previewMode = true;
    // Two tiers: edits evaluate at a coarse size so the graph keeps up with a
    // slider, then a refine pass runs once the edits stop. Conversion always
    // runs full res regardless.
    context.previewMaxSize = m_graphRefined ? 1024 : 384;
    context.materialMaxResolution = m_materialMaxResolution;
    context.upscalePolicy = QSettings().value(QStringLiteral("Convert/upscalePolicy"), 0).toInt();
    m_lastGraphResults = m_nodeEvaluator.evaluate(m_nodeGraph, context);
    m_canvas->setResults(m_lastGraphResults);
    pushLiveMaterialToPreview();
    // The metal share moves with the graph, so a levels drag can be judged
    // without converting first.
    updateSlotInventory();
    // Schedule the sharp pass; another edit restarts the wait, so a drag never
    // pays for full-resolution work it is about to throw away.
    if (!m_graphRefined) m_graphRefine->start();
    if (!m_canvas->selectedNode().isEmpty()) {
        describeNode(m_canvas->selectedNode());
        // The map list is built from what the evaluation produced, so it has to
        // be re-checked here; setSplitPickerMode ignores an unchanged list, so
        // this cannot disturb the view the user picked.
        syncSplitPicker(m_canvas->selectedNode());
        showNodeInPreview(m_canvas->selectedNode());
    }
}

QImage ConvertWorkspace::portImage(const QString& nodeId, const QString& portId, bool isOutput) const
{
    if (isOutput) return m_lastGraphResults.value(nodeId).outputs.value(portId).image();
    for (const NodeGraph::Connection& link : m_nodeGraph.connections)
        if (link.toNode == nodeId && link.toPort == portId)
            return m_lastGraphResults.value(link.fromNode).outputs.value(link.fromPort).image();
    return {};
}

QVector<NodeGraph::PortSpec> ConvertWorkspace::previewPorts(
    const NodeGraph::NodeTypeSpec& spec, bool* fromInputs) const
{
    QVector<NodeGraph::PortSpec> ports;
    for (const NodeGraph::PortSpec& port : spec.outputs)
        if (port.type == NodeGraph::ValueType::Image) ports.append(port);
    if (fromInputs) *fromInputs = ports.isEmpty();
    if (!ports.isEmpty()) return ports;
    // A terminal keeps no outputs, so what it holds is what was wired into it.
    for (const NodeGraph::PortSpec& port : spec.inputs)
        if (port.type == NodeGraph::ValueType::Image) ports.append(port);
    return ports;
}

QVector<MaterialPreviewWidget::MapEntry> ConvertWorkspace::nodeMapEntries(const QString& nodeId) const
{
    // Only maps that actually came out of the last evaluation are offered. A
    // material with no glow would otherwise list "Glow" and show nothing.
    using MapEntry = MaterialPreviewWidget::MapEntry;
    QVector<MapEntry> textures, channels;
    const NodeGraph::Node* node = m_nodeGraph.node(nodeId);
    const NodeGraph::NodeTypeSpec* spec = node ? m_nodeRegistry.find(node->typeId) : nullptr;
    if (!spec) return textures;
    bool fromInputs = false;
    for (const NodeGraph::PortSpec& port : previewPorts(*spec, &fromInputs)) {
        if (portImage(nodeId, port.id, !fromInputs).isNull()) continue;
        textures.append({port.id, port.name, false});
        // A packed texture also offers the maps inside it one at a time —
        // roughness on its own, without the metal and AO alongside it.
        for (const NodeGraph::ChannelMap& map : port.channelMaps)
            channels.append({QStringLiteral("%1#%2").arg(port.id).arg(map.mask), map.label, false});
    }
    // Captions only once there are two groups to tell apart. A node whose
    // outputs are already single maps gets a plain list, as before.
    if (channels.isEmpty()) return textures;
    QVector<MapEntry> entries{{QString(), tr("Textures"), true}};
    entries += textures;
    entries.append({QString(), tr("Channels"), true});
    return entries + channels;
}

void ConvertWorkspace::syncSplitPicker(const QString& nodeId)
{
    // What the left picker can offer follows the node: a whole material lists
    // its maps, a node that draws a thumbnail outputs one image so it lists
    // that image's channels, and anything else keeps the full view list.
    const NodeGraph::Node* node = m_nodeGraph.node(nodeId);
    const NodeGraph::NodeTypeSpec* spec = node ? m_nodeRegistry.find(node->typeId) : nullptr;
    if (spec && !spec->materialPreview.isEmpty()) {
        const QVector<MaterialPreviewWidget::MapEntry> maps = nodeMapEntries(nodeId);
        // A map that disappeared from the graph must not stay on the surface.
        const QString chosen = m_nodeMapPort.value(nodeId);
        if (!chosen.isEmpty()
            && std::none_of(maps.cbegin(), maps.cend(),
                [&chosen](const MaterialPreviewWidget::MapEntry& map) { return map.id == chosen; }))
            m_nodeMapPort.remove(nodeId);
        m_preview->setSplitPickerMode(MaterialPreviewWidget::SplitPicker::Maps, maps);
        // Combine and the terminal carry the same five maps, so moving between
        // them leaves the list untouched — the shown entry still has to be
        // re-pointed at what this node remembers.
        m_preview->setSplitPickerCurrentMap(m_nodeMapPort.value(nodeId));
        return;
    }
    m_preview->setSplitPickerMode(spec && NodeGraph::nodeShowsThumbnail(*spec)
        ? MaterialPreviewWidget::SplitPicker::Channels
        : MaterialPreviewWidget::SplitPicker::Views);
}

void ConvertWorkspace::showNodeInPreview(const QString& nodeId)
{
    // Selecting a node puts its output on the preview surface. The image is
    // the interactive evaluation's, so it is capped at previewMaxSize rather
    // than the material's real resolution — conversion runs full res.
    const NodeGraph::Node* node = m_nodeGraph.node(nodeId);
    const NodeGraph::NodeTypeSpec* spec = node ? m_nodeRegistry.find(node->typeId) : nullptr;
    if (!spec) return;
    const NodeGraph::NodeResult& result = m_lastGraphResults.value(nodeId);

    // A node that stands for a whole material shades that material, unless a
    // single map has been picked out of it on the left. Source-side nodes shade
    // the untouched original, output-side ones the converted PBR result —
    // before and after, one click apart, with every map in between reachable.
    if (!spec->materialPreview.isEmpty()) {
        // "rmaos#1" is the roughness inside RMAOS; a bare "rmaos" is the packed
        // texture whole. Isolation reuses the same mask the node's own channel
        // toggles use, so a single channel reads as the grey mask it is.
        const QString chosen = m_nodeMapPort.value(nodeId);
        if (!chosen.isEmpty()) {
            const int hash = chosen.indexOf(QLatin1Char('#'));
            const QString port = hash < 0 ? chosen : chosen.left(hash);
            const int mask = hash < 0 ? NodeGraph::kChannelsRgba
                                      : chosen.mid(hash + 1).toInt();
            bool fromInputs = false;
            previewPorts(*spec, &fromInputs);
            const QImage map = portImage(nodeId, port, !fromInputs);
            if (!map.isNull()) {
                m_preview->canvas()->setNodeOutputImage(NodeGraph::applyChannelMask(map, mask));
                m_preview->setRenderModeLeft(PbrPreviewGL::DebugView::NodeOutput);
                return;
            }
        }
        m_preview->canvas()->setNodeOutputImage(QImage());
        if (spec->materialPreview == QStringLiteral("output")) {
            m_preview->setRenderModeLeft(PbrPreviewGL::DebugView::Shaded);
            return;
        }
        // The node may publish the profile itself; otherwise the queue's
        // current material already settled it when the graph was chosen.
        const QString profile = result.outputs.value(QStringLiteral("profile")).string();
        const bool fallout76 = profile.isEmpty()
            ? m_graphProfile == QStringLiteral("FO76") : profile == QStringLiteral("FO76");
        m_preview->setRenderModeLeft(fallout76 ? PbrPreviewGL::DebugView::NativeFo76
                                               : PbrPreviewGL::DebugView::NativeFo4);
        return;
    }

    QImage image;
    for (const NodeGraph::PortSpec& port : spec->outputs) {
        if (port.type != NodeGraph::ValueType::Image) continue;
        const QImage candidate = result.outputs.value(port.id).image();
        if (!candidate.isNull()) { image = candidate; break; }
    }
    // The node's channel toggles isolate the viewport too, so what is on the
    // surface matches the thumbnail you set it from.
    image = NodeGraph::applyChannelMask(image, node->previewChannels);
    m_preview->canvas()->setNodeOutputImage(image);
    // The LEFT half is the selected node, because that is the half its picker
    // sits over. This used to drive the right half, so picking a map on the
    // left changed the other side of the split and left the picker's own half
    // showing the comparison.
    m_preview->setRenderModeLeft(image.isNull()
        ? PbrPreviewGL::DebugView::Shaded : PbrPreviewGL::DebugView::NodeOutput);
    // Frame only when the selection actually moves to a different node. Doing
    // it on every push reset the camera on each parameter edit, because a
    // re-evaluation pushes the image again.
    if (!image.isNull() && nodeId != m_framedNode) {
        m_preview->canvas()->frameObject();
        m_framedNode = nodeId;
    }
}

void ConvertWorkspace::updatePreview(const QueueEntry* entry)
{
    // Source-map comparison: load the ORIGINAL game textures for the Source*
    // render modes so the split view can pair them with converted channels.
    const QString sourceKey = entry ? entry->materialPath : QString();
    if (sourceKey != m_sourceImagesFor) {
        m_sourceImagesFor = sourceKey;
        QImage diffuse, spec, normal, extra, glow;
        Bgsm::Material source;
        PbrPreviewGL::NativeMaterial native;
        if (!sourceKey.isEmpty() && Bgsm::readFile(sourceKey, &source, nullptr)) {
            const QString textureRoot = m_textureRootEdit->text().trimmed();
            const auto loadMap = [&textureRoot](const QString& authored) {
                return authored.isEmpty() ? QImage() : TextureImage::load(authored, textureRoot, nullptr);
            };
            diffuse = loadMap(source.diffuseTexture);
            spec = loadMap(!source.specularTexture.isEmpty() ? source.specularTexture : source.smoothSpecTexture);
            normal = loadMap(source.normalTexture);
            extra = loadMap(!source.lightingTexture.isEmpty() ? source.lightingTexture : source.greyscaleTexture);
            glow = loadMap(source.glowTexture);

            // Legacy shading parameters, so the native views render this
            // material's own specular response rather than a generic one.
            native.valid = true;
            native.fallout76 = source.isFallout76();
            native.specularColor = source.specularColor;
            native.specularMult = source.specularMult;
            native.smoothness = source.smoothness;
            native.envMapScale = source.environmentMappingMaskScale;
            native.specularEnabled = source.specularEnabled;
            native.emitEnabled = source.emitEnabled;
            native.emittanceColor = source.emittanceColor;
            native.emittanceMult = source.emittanceMult;
        }
        m_preview->canvas()->setSourceComparisonImages(diffuse, spec, normal, extra, glow);
        m_preview->canvas()->setNativeMaterial(native);
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
    const int row = currentEntryIndex();
    return row >= 0 ? m_entries[row].materialPath.toLower() : QString();
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
    refreshMaskOverlay();
    m_maskStatus->setText(tr("%n triangle(s) filled — Save Mask to apply", nullptr, int(state.filledTriangles.size())));
}

void ConvertWorkspace::handleMaskPainted(const QPointF& uv)
{
    const QString key = currentMaterialKey();
    if (key.isEmpty()) return;
    MaskState& state = m_masks[key];
    if (state.mask.isNull()) state.mask = MaskPaint::neutralMask();

    const float target = float(m_maskBrushValue->currentData().toDouble());
    const float flow = float(m_maskBrushFlow->value()) / 100.0f;
    const float radius = float(m_maskBrushSize->value()) / 1000.0f;

    // UV wraps, so a stroke over a seam has to wrap with it — clamping would
    // pile the whole brush up against the edge instead.
    const int size = state.mask.width();
    const double centreX = uv.x() * size;
    const double centreY = uv.y() * size;
    const double pixelRadius = std::max(double(radius) * size, 1.0);
    const int span = int(std::ceil(pixelRadius));

    for (int dy = -span; dy <= span; ++dy) {
        for (int dx = -span; dx <= span; ++dx) {
            const double distance = std::hypot(dx, dy);
            if (distance > pixelRadius) continue;
            // Soft edge: full strength for the inner half, easing to nothing at
            // the rim, so overlapping passes build instead of tiling hard discs.
            const double falloff = 1.0 - smoothstepd(pixelRadius * 0.5, pixelRadius, distance);
            const float amount = float(falloff) * flow;
            if (amount <= 0.0f) continue;
            const int x = ((int(std::floor(centreX)) + dx) % size + size) % size;
            const int y = ((int(std::floor(centreY)) + dy) % size + size) % size;
            uchar* texel = state.mask.scanLine(y) + x * 4;
            const float current = texel[0] / 255.0f;
            const float blended = current + (target - current) * amount;
            const uchar written = uchar(std::lround(std::clamp(blended, 0.0f, 1.0f) * 255.0f));
            texel[0] = texel[1] = texel[2] = written;
            texel[3] = 255;
        }
    }
    ++m_brushStrokes;
    refreshMaskOverlay();
    m_maskStatus->setText(tr("painted — Save Mask to apply"));
}

void ConvertWorkspace::refreshMaskOverlay()
{
    const QString key = currentMaterialKey();
    const auto state = key.isEmpty() ? m_masks.constEnd() : m_masks.constFind(key);
    // Only while the brush is armed. A permanent tint would sit over every
    // material the queue walks through and make the preview a liar about what
    // the conversion looks like.
    const bool show = m_maskBrushEnabled && m_maskBrushEnabled->isChecked()
        && state != m_masks.constEnd() && !state->mask.isNull();
    m_preview->canvas()->setMaskOverlay(show ? state->mask : QImage());
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
    const int row = currentEntryIndex();
    if (row < 0) { m_maskStatus->setText(tr("Select a material first.")); return; }
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

int ConvertWorkspace::checkedCount() const
{
    // Material rows only: a ticked folder is a handle on its children, not a
    // job of its own, and counting it would over-report what Convert will run.
    int checked = 0;
    for (const QTreeWidgetItem* item : m_rowItems)
        if (item && item->checkState(0) == Qt::Checked) ++checked;
    return checked;
}

QString ConvertWorkspace::sourceLabel() const
{
    const QString source = QDir::fromNativeSeparators(m_sourceEdit->text().trimmed());
    if (source.isEmpty()) return {};
    return source.section(QLatin1Char('/'), -1, -1, QString::SectionSkipEmpty);
}

void ConvertWorkspace::updateProjectSummary()
{
    if (!m_projectToggle) return;
    const auto leaf = [](const QString& path) {
        const QString trimmed = QDir::fromNativeSeparators(path.trimmed());
        if (trimmed.isEmpty()) return QStringLiteral("—");
        return trimmed.section(QLatin1Char('/'), -1, -1, QString::SectionSkipEmpty);
    };
    // The disclosure state is an ICON, not a glyph in the label — which is the
    // whole reason disclosureArrowIcon exists (see its header: "▾"/"▸" are not
    // size-matched in Segoe UI, and the right-pointing one falls back to a tiny
    // symbol, so the two states looked like different controls).
    m_projectToggle->setIcon(disclosureArrowIcon(m_projectToggle->isChecked(), 12, kArrowInk));
    m_projectToggle->setIconSize(QSize(12, 12));
    m_projectSummaryText = tr("%1  →  %2")
        .arg(leaf(m_sourceEdit->text()), leaf(m_outputEdit->text()));
    m_projectToggle->setText(m_projectSummaryText);
    m_projectToggle->setToolTip(tr("Source: %1\nTextures: %2\nOutput: %3")
        .arg(m_sourceEdit->text().trimmed(), m_textureRootEdit->text().trimmed(), m_outputEdit->text().trimmed()));
}

void ConvertWorkspace::updateButtons()
{
    const bool haveQueue = !m_entries.isEmpty();
    m_scanButton->setEnabled(!m_running);
    m_dryRunButton->setEnabled(!m_running && haveQueue);
    // The button states its own scope, so there is nothing to guess about
    // which of several convert verbs applies — and it has to agree with the
    // right-click menu, or the same click means different things depending on
    // which control you reached for.
    const int checked = checkedCount();
    const int selected = int(selectedEntries().size());
    m_convertButton->setEnabled(!m_running && haveQueue);
    m_convertButton->setText(checked > 0 ? tr("Convert %n checked", nullptr, checked)
        : selected >= 2 ? tr("Convert %n selected", nullptr, selected)
                        : tr("Convert %n material(s)", nullptr, m_entries.size()));
    m_convertButton->setToolTip(checked > 0
        ? tr("Convert the %n checked queue row(s).", nullptr, checked)
        : selected >= 2 ? tr("Convert the %n selected material(s). Tick rows to pin a set.", nullptr, selected)
                        : tr("Convert every material in the queue. Tick rows, or select several, "
                             "to convert only those."));
    m_cancelButton->setVisible(m_running);
    m_cancelButton->setEnabled(m_running);
}
