#pragma once

#include "converter.h"
#include "nodegraph.h"

#include <QHash>
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
    enum class RunScope { All, Checked, CurrentRow };

    void browseFolder(QLineEdit* edit, const QString& title);
    void scanSources();
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
    QPushButton* m_convertSelectedButton = nullptr;
    QPushButton* m_convertAllButton = nullptr;
    QPushButton* m_cancelButton = nullptr;
    QPushButton* m_prevFlagged = nullptr;
    QPushButton* m_nextFlagged = nullptr;
    QLabel* m_flaggedSummary = nullptr;
    class QHBoxLayout* m_actionsRow = nullptr; // hoisted above the graph
    QGroupBox* m_selectedGroup = nullptr;
    QPushButton* m_maskLoadNif = nullptr;
    QComboBox* m_maskFillMode = nullptr;
    QComboBox* m_maskFillValue = nullptr;
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
    NodeGraph::Registry m_nodeRegistry;
    NodeGraph::Evaluator m_nodeEvaluator{m_nodeRegistry};
    NodeGraph::Graph m_nodeGraph;
    QString m_graphProfile;              // "FO4"/"FO76" the loaded graph was built for
    NodeCanvas* m_canvas = nullptr;
    NodeInspector* m_inspector = nullptr;
    QTimer* m_graphEval = nullptr;
    QHash<QString, NodeGraph::NodeResult> m_lastGraphResults;
    QHash<QString, MaterialOverride> m_overrides;
    QHash<QString, MaskState> m_masks;
    QVector<int> m_maskElements;      // cached element ids for the loaded mesh
    QString m_maskElementsFor;        // nif path the cache belongs to
    QVector<QueueEntry> m_entries;
    QThread* m_worker = nullptr;
    std::atomic<bool> m_cancelRequested{false};
    bool m_running = false;
    bool m_lastRunDry = false;
    int m_pendingResults = 0;
};
