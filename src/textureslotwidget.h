#pragma once

#include "materialdocument.h"

#include <QColor>
#include <QFrame>
#include <QHash>
#include <QStringList>
#include <QVector>

class QFormLayout;
class QCheckBox;
class QDragEnterEvent;
class QDropEvent;
class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;
class QVBoxLayout;

enum class FieldType { Number, Toggle, Choice, Text, Color };

struct FieldSpec {
    QString key;
    QString label;
    FieldType type = FieldType::Text;
    QJsonValue defaultValue;
    QStringList choices;
    double minimum = -1000000.0;
    double maximum = 1000000.0;
    double step = 0.01;
    int decimals = 4;
    QString suffix;
    QString tooltip;
    bool colorAlpha = false;
    // Settings-card fields only: editing this field rebuilds the workspace so
    // gated/compact cards and derived titles refresh (queued, scroll-preserving).
    bool rebuildOnChange = false;
    // Slot fields only: the editor is enabled while the sibling field
    // `enabledByKey` holds one of `enabledByValues` (e.g. custom diffusion
    // radii behind Profile = Custom). Empty = always enabled.
    QString enabledByKey;
    QStringList enabledByValues;
};

inline FieldSpec withRebuild(FieldSpec field) { field.rebuildOnChange = true; return field; }

struct TextureSlotSpec {
    QString key;
    QString name;
    QString abbreviation;
    QString format;
    QString tooltip;
    QColor color;
    QStringList channels;
    bool defaultEnabled = false;
    bool defaultExpanded = true;
    QString defaultPath;
    QVector<FieldSpec> fields;
};

class TextureSlotWidget final : public QFrame {
    Q_OBJECT

public:
    explicit TextureSlotWidget(const QString& uvSet, const TextureSlotSpec& spec,
        const Pbr::TextureSlotState& state, const QString& textureRoot = {}, QWidget* parent = nullptr);

    QString key() const;
    QString uvSet() const;
    Pbr::TextureSlotState state() const;
    bool isSlotEnabled() const;
    bool hasChannelSources() const;
    Pbr::TextureSlotState stateWithAllChannelSources(bool overrideChannels) const;
    void setAllChannelSources(bool overrideChannels);
    void setExpanded(bool expanded);
    void setAdvancedVisible(bool visible);

signals:
    void stateChanged(const QString& uvSet, const QString& key, const Pbr::TextureSlotState& state);
    void inspectRequested(const QString& name, const QString& path, const QStringList& channels, const QColor& color);
    void resetRequested(const QString& uvSet, const QString& key);
    void pasteRequested(const QString& uvSet, const QString& key, const Pbr::TextureSlotState& state);
    void companionTexturesRequested(const QString& uvSet, const QString& key, const QString& absolutePath);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void updateExpandedState();
    void updateEnabledState();
    void emitStateChanged();
    void updateThumbnail();
    void showChannelSourcesDialog();
    void exportPackedTexture();
    void updateSwapSummary();
    void updateChannelControls();
    void updateAdvancedVisibility();
    void assignTextureFile(const QString& fileName);
    void assignTexturePath(const QString& authoredPath);
    bool textureProvidesChannels(const FieldSpec& field, const QString& path) const;
    bool channelOverrideApplicable(const FieldSpec& field, const QJsonObject& values) const;
    QWidget* createFieldEditor(const FieldSpec& field, const QJsonValue& value);

    QString m_uvSet;
    QString m_textureRoot;
    TextureSlotSpec m_spec;
    Pbr::TextureSlotState m_state;
    QToolButton* m_enableButton = nullptr;
    QToolButton* m_collapseButton = nullptr;
    QToolButton* m_previewButton = nullptr;
    QToolButton* m_resetButton = nullptr;
    QLabel* m_title = nullptr;
    QLabel* m_format = nullptr;
    QLabel* m_diagnostics = nullptr;
    QLabel* m_pathBadge = nullptr;
    QLabel* m_swapSummary = nullptr;
    QWidget* m_body = nullptr;
    QLineEdit* m_path = nullptr;
    QPushButton* m_findSetButton = nullptr;
    QCheckBox* m_channelMaster = nullptr;
    QHash<QString, QWidget*> m_fieldEditors;
    QHash<QString, QLabel*> m_fieldLabels;
    bool m_advancedVisible = false;
};
