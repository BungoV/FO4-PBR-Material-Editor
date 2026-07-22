#pragma once

#include "converter.h"

#include <QWidget>

#include <atomic>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QThread;
class QTreeWidget;
class QTreeWidgetItem;

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

    void browseFolder(QLineEdit* edit, const QString& title);
    void scanSources();
    void startRun(bool dryRun, bool selectedOnly);
    void cancelRun();
    void finishRun();
    void handleResult(int row, const Converter::JobResult& result);
    void updateDetail();
    void updateButtons();
    Converter::JobOptions collectOptions(bool dryRun) const;
    void persistSettings() const;

    QLineEdit* m_sourceEdit = nullptr;
    QLineEdit* m_textureRootEdit = nullptr;
    QLineEdit* m_outputEdit = nullptr;
    QTreeWidget* m_queue = nullptr;
    QLabel* m_queueSummary = nullptr;
    QListWidget* m_detail = nullptr;
    QPushButton* m_openInAuthor = nullptr;
    QPushButton* m_scanButton = nullptr;
    QPushButton* m_dryRunButton = nullptr;
    QPushButton* m_convertSelectedButton = nullptr;
    QPushButton* m_convertAllButton = nullptr;
    QPushButton* m_cancelButton = nullptr;
    QDoubleSpinBox* m_recovery = nullptr;
    QDoubleSpinBox* m_glossCurve = nullptr;
    QSpinBox* m_derivationSize = nullptr;
    QCheckBox* m_heightInBlue = nullptr;
    QCheckBox* m_detailAo = nullptr;
    QCheckBox* m_metalDiagnostic = nullptr;

    QVector<QueueEntry> m_entries;
    QThread* m_worker = nullptr;
    std::atomic<bool> m_cancelRequested{false};
    bool m_running = false;
    bool m_lastRunDry = false;
    int m_pendingResults = 0;
};
