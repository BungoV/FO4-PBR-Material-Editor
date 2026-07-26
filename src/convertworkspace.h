#pragma once

#include "converter.h"
// Not forward-declarable: the left picker's row type is a nested struct.
#include "materialpreviewwidget.h"
#include "nodegraph.h"

#include <QHash>
#include <QJsonObject>
#include <QUndoStack>
#include <QWidget>

#include <atomic>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QSplitter;
class QThread;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class MaterialPreviewWidget;
class NodeCanvas;
class NodeInspector;

// The Convert workspace (docs/FO4-Conversion.md): scan a source tree for
// .bgsm/.bgem materials, run the automatic conversion pipeline over them on
// a worker thread, and review per-material findings. This foundation covers
// the batch flow; the shared preview, correction masking, and geometry AO
// arrive with later slices.
class ConvertWorkspace final : public QWidget {
    Q_OBJECT

public:
    explicit ConvertWorkspace(QWidget* parent = nullptr);
    ~ConvertWorkspace() override;

signals:
    void openInAuthorRequested(const QString& pbrmPath);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct QueueEntry {
        QString materialPath;
        Converter::JobResult result;
        bool hasResult = false;
    };
    // Per-material tuning for the manual half of the review loop ("spec maps
    // need clamping, sometimes manually"): overrides the batch math for one
    // material only.
    struct MaterialOverride {
        float recovery = 1.0f;
        float glossCurve = 1.0f;
        float specPercentile = 0.98f;
        QString maskPath;      // saved correction-mask sidecar for this material
    };
    // In-session mask painting state for the selected material.
    struct MaskState {
        QImage mask;                       // 0.5-neutral, R = correction
        QVector<quint32> filledTriangles;  // for the viewport highlight
        QString nifPath;
    };
    enum class RunScope { All, Checked, CurrentRow, Selected };

    void browseFolder(QLineEdit* edit, const QString& title);
    void scanSources();
    // Queue rows nest under folder items, so a row's position in the tree is no
    // longer its index in m_entries. The index rides on the item instead, and
    // everything that needs an entry goes through these.
    int entryIndexOf(const QTreeWidgetItem* item) const;
    int currentEntryIndex() const;
    QTreeWidgetItem* itemForEntry(int index) const;
    // Selected materials, with a selected folder standing for every material
    // beneath it. Sorted and de-duplicated so a run visits each once.
    QVector<int> selectedEntries() const;
    QVector<int> rowsForScope(RunScope scope) const;
    void showQueueMenu(const QPoint& pos);
    // Re-elides the Corrections heading against the panel's current width; a
    // QGroupBox title runs off the edge rather than shortening itself.
    void updateCorrectionsTitle();
    void setCheckedForSelection(bool checked);
    // The selected material's source maps: what resolved, at what size and
    // compression, what is missing, and the metal share the graph is producing.
    void updateSlotInventory();
    void startRun(bool dryRun, RunScope scope);
    void cancelRun();
    void finishRun();
    void handleResult(int row, const Converter::JobResult& result);
    void updateDetail();
    void updatePreview(const QueueEntry* entry);
    void updateButtons();
    void updateFlaggedSummary();
    void stepFlagged(int direction);
    void loadOverrideControls();
    void loadMaskReference();
    void handleTrianglePicked(int triangle);
    // One brush sample, in UV0. Stamps into the same 0.5-neutral mask the fills
    // write, so a material can be corrected with either or both.
    void handleMaskPainted(const QPointF& uv);
    // Pushes the current mask to the viewport as a tinted overlay, or clears it.
    void refreshMaskOverlay();
    void clearMaskFills();
    void saveMask();
    QString currentMaterialKey() const;
    Converter::JobOptions collectOptions(bool dryRun) const;
    void persistSettings() const;

    QLineEdit* m_sourceEdit = nullptr;
    QLineEdit* m_textureRootEdit = nullptr;
    QLineEdit* m_outputEdit = nullptr;
    QTreeWidget* m_queue = nullptr;
    QLabel* m_queueSummary = nullptr;
    QListWidget* m_detail = nullptr;
    MaterialPreviewWidget* m_preview = nullptr;
    QPushButton* m_openInAuthor = nullptr;
    QPushButton* m_scanButton = nullptr;
    QPushButton* m_dryRunButton = nullptr;
    QPushButton* m_convertButton = nullptr; // states its own scope
    QPushButton* m_cancelButton = nullptr;
    // Folder setup, folded away once it is filled in.
    QPushButton* m_projectToggle = nullptr;
    QString m_projectSummaryText;
    // Upscale policy, on the canvas toolbar: a per-run decision flipped
    // while looking at the graph, not a preference buried in Settings.
    QComboBox* m_upscalePolicy = nullptr;
    QString m_framedNode; // last node the preview camera was framed for
    QWidget* m_projectDetails = nullptr;
    QGroupBox* m_findingsGroup = nullptr;   // hidden until there is something to report
    // Source-map inventory for the selected material, in the space the queue
    // was leaving empty. Carries the live metal share the removed Metal %
    // column used to report, but updated as the graph re-evaluates rather than
    // only after a conversion.
    QGroupBox* m_slotGroup = nullptr;
    QTreeWidget* m_slotList = nullptr;
    QLabel* m_slotMetal = nullptr;
    QString m_slotInventoryFor;             // material the inventory was built for
    QLabel* m_nodeHint = nullptr;           // selected node's name + summary
    QPushButton* m_prevFlagged = nullptr;
    QPushButton* m_nextFlagged = nullptr;
    QLabel* m_flaggedSummary = nullptr;
    class QHBoxLayout* m_actionsRow = nullptr; // hoisted above the graph
    QGroupBox* m_selectedGroup = nullptr;
    QString m_correctionsMaterial;          // full name; the title shows an elided form
    QPushButton* m_maskLoadNif = nullptr;
    QComboBox* m_maskFillMode = nullptr;
    QComboBox* m_maskFillValue = nullptr;
    QCheckBox* m_maskBrushEnabled = nullptr;
    QSlider* m_maskBrushSize = nullptr;
    QSlider* m_maskBrushFlow = nullptr;
    QComboBox* m_maskBrushValue = nullptr;
    int m_brushStrokes = 0;   // reported alongside the fill count
    QCheckBox* m_maskPickEnabled = nullptr;
    QPushButton* m_maskClear = nullptr;
    QPushButton* m_maskSave = nullptr;
    QLabel* m_maskStatus = nullptr;

    QString m_sourceImagesFor;           // material whose source maps are loaded in the preview

    // Node-graph mode: the conversion pipeline as an editable graph with a
    // live thumbnail on every node.
    void buildGraphMode(QSplitter* parent);
    void evaluateGraph();
    void syncGraphToSelection();
    void showNodeInPreview(const QString& nodeId);
    // Points the left-half picker at what this node can show. Re-run after every
    // evaluation, because a map appearing or vanishing changes the list.
    void syncSplitPicker(const QString& nodeId);
    // Rows for the left picker: each image the node carries, then every named
    // map packed inside those images.
    QVector<MaterialPreviewWidget::MapEntry> nodeMapEntries(const QString& nodeId) const;
    // The image on one of a node's ports. Outputs come from its own result;
    // inputs are followed back up the wire, which is the only way to see what
    // a terminal holds now that it has no outputs of its own.
    QImage portImage(const QString& nodeId, const QString& portId, bool isOutput) const;
    // Image ports a material-level node offers for preview: its outputs, or its
    // inputs when it has none because it ends the graph.
    QVector<NodeGraph::PortSpec> previewPorts(const NodeGraph::NodeTypeSpec& spec, bool* fromInputs) const;
    // Which map of a material-level node is on the surface; absent means the
    // whole material, shaded. Per node, so moving away and back is stable.
    QHash<QString, QString> m_nodeMapPort;
    void describeNode(const QString& nodeId);
    // Snapshot undo for every graph edit: add, delete, wire, move, parameter.
    void pushLiveMaterialToPreview();
    void recordGraphEdit(const QString& text);
    void applyGraphState(const QJsonObject& state);
public:
    // What the window title shows while the converter is on screen.
    QString sourceLabel() const;
    // Builds the queue from the configured source folder, as pressing Scan
    // does; --converter-scan uses it so UI captures need no synthetic clicks.
    void scanNow() { scanSources(); }
    // First material row in tree order, folders skipped. Capture harnesses use
    // it; without a selection the canvas only shows its placeholder.
    void selectFirstMaterial();

    // --- Read-only hooks for --queue-check. Folder rows, tri-state ticking and
    // the selection's expansion to materials are all logic a screenshot cannot
    // confirm, and the row colouring has three known ways to silently not
    // render (see the fork's block-list notes).
    QVector<int> selectedEntriesForTest() const { return selectedEntries(); }
    int checkedCountForTest() const { return checkedCount(); }
    int entryCountForTest() const { return int(m_entries.size()); }
    int entryIndexOfForTest(const QTreeWidgetItem* item) const { return entryIndexOf(item); }

private:
    void updateProjectSummary();
    int checkedCount() const;
    NodeGraph::Registry m_nodeRegistry;
    NodeGraph::Evaluator m_nodeEvaluator{m_nodeRegistry};
    NodeGraph::Graph m_nodeGraph;
    QString m_graphProfile;              // "FO4"/"FO76" the loaded graph was built for
    // Per-title graphs, so switching materials never re-creates a node the
    // user deleted; only Reset Graph goes back to the stock definition.
    QHash<QString, NodeGraph::Graph> m_graphsByProfile;
    QUndoStack m_graphUndo;
    QJsonObject m_graphSnapshot;
    int m_materialMaxResolution = 0;     // largest source texture in the current material
    NodeCanvas* m_canvas = nullptr;
    QTimer* m_graphEval = nullptr;
    QTimer* m_graphRefine = nullptr; // sharp pass once editing stops
    bool m_graphRefined = false;
    QHash<QString, NodeGraph::NodeResult> m_lastGraphResults;
    QHash<QString, MaterialOverride> m_overrides;
    QHash<QString, MaskState> m_masks;
    QVector<int> m_maskElements;      // cached element ids for the loaded mesh
    QString m_maskElementsFor;        // nif path the cache belongs to
    QVector<QueueEntry> m_entries;
    // Parallel to m_entries: the tree item each one is drawn as. Folder items
    // are not in here, which is what makes "material rows only" walks cheap.
    QVector<QTreeWidgetItem*> m_rowItems;
    QThread* m_worker = nullptr;
    std::atomic<bool> m_cancelRequested{false};
    bool m_running = false;
    bool m_lastRunDry = false;
    int m_pendingResults = 0;
};
