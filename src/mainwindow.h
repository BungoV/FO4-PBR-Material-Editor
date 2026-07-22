#pragma once

#include "materialdocument.h"
#include "textureslotwidget.h"

#include <QMainWindow>
#include <QMap>
#include <QSet>

#include <functional>

class QAction;
class QCloseEvent;
class QComboBox;
class QDragEnterEvent;
class QDropEvent;
class QCompleter;
class QFileSystemWatcher;
class QLineEdit;
class QTimer;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QStackedWidget;
class QSplitter;
class QToolButton;
class QUndoStack;
class QVBoxLayout;
class ConvertWorkspace;
class MaterialPreviewWidget;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    bool openFile(const QString& fileName);
    void restoreLastSession();
    const Pbr::MaterialDocument& document() const { return m_document; }

protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    struct PageLayout {
        QWidget* page = nullptr;
        QVBoxLayout* content = nullptr;
        QFrame* header = nullptr;
        QVBoxLayout* headerLayout = nullptr;
    };

    void buildWindow();
    void buildMenus();
    void setWorkspaceMode(bool convert);
    void rebuildWorkspace(const QString& preferredPage = QStringLiteral("Primary UV Textures"));
    void rebuildNavigation();
    void addNavigationSection(const QString& title);
    void addNavigationPage(const QString& title);
    void addUvLayerToggleItem(const QString& layerName, bool layerEnabled, const std::function<void(bool)>& setLayerEnabled);
    void toggleNavigationSection(QListWidgetItem* item);
    int validationSeverity() const;
    void setAdvancedInterface(bool enabled);
    void setCurrentTextureCardsExpanded(bool expanded);
    void showPage(const QString& title);
    void filterCurrentPage(const QString& text);
    void refreshSearchIndex();
    void openSearchResult(const QString& displayText);

    PageLayout createPage(const QString& title, const QString& description);
    QWidget* createOverviewPage();
    QWidget* createMaterialInfoPage();
    QWidget* createOverridesPage();
    QWidget* createUvPage(const QString& uvSet);
    QWidget* createUvMappingPage();
    QWidget* createShadingPage();
    QWidget* createGeometryPage();
    QWidget* createDynamicNormalsPage();
    QWidget* createVegetationPage();
    QWidget* createParticlesPage();
    QWidget* createSurfaceResponsePage();
    QWidget* createDecalsPage();
    QWidget* createLandscapePage();
    QWidget* createTransparencyPage();
    QWidget* createEnvironmentPage();
    QWidget* createAdvancedPage();
    QWidget* createValidationPage();
    void showTextureSourceSettings();
    void showEditorSettings();
    void showPathRepairTool();
    void showBatchOperations();

    TextureSlotWidget* addTextureSlot(QVBoxLayout* layout, const QString& uvSet, const TextureSlotSpec& spec);
    void addGroupTitle(QVBoxLayout* layout, const QString& title);
    void addInfoCard(QVBoxLayout* layout, const QString& title, const QString& text, bool warning = false);
    QFrame* addSettingsCard(QVBoxLayout* layout, const QString& section, const QString& title, const QVector<FieldSpec>& fields);
    QWidget* createSettingEditor(const QString& section, const FieldSpec& field);
    QVector<TextureSlotSpec> textureSlotsFor(Pbr::ShaderMode mode, const QString& uvSet) const;
    QVector<TextureSlotSpec> landscapeSlots() const;
    void applyDocumentChange(const QString& description, const std::function<void()>& mutation);
    void restoreDocument(const Pbr::MaterialDocument& document);
    void copyUvLayer(const QString& uvSet);
    void pasteUvLayer(const QString& uvSet);
    void resetUvLayer(const QString& uvSet);
    void applyMaterialTemplate(const QString& templateName);

    void newDocument();
    void openDocument();
    bool saveDocument();
    bool saveDocumentAs();
    bool maybeSave();
    void updateWindowTitle();
    void updateTextureWatcher();
    void handleWatchedTextureChanged(const QString& absolutePath);
    void applyAutosaveSettings();
    void performAutosave();
    void updateRecentDirectory(const QString& fileName);
    void addRecentFile(const QString& fileName);
    void rebuildRecentMenu();
    void rebuildTemplateMenu();
    void saveCurrentAsTemplate();
    void assignCompanionTextures(const QString& uvSet, const QString& sourceKey, const QString& absolutePath);

    Pbr::MaterialDocument m_document;
    QComboBox* m_shaderCombo = nullptr;
    QLineEdit* m_search = nullptr;
    QCompleter* m_searchCompleter = nullptr;
    QListWidget* m_navigation = nullptr;
    QStackedWidget* m_pages = nullptr;
    MaterialPreviewWidget* m_preview = nullptr;
    QSplitter* m_splitter = nullptr;
    QStackedWidget* m_workspaceStack = nullptr;
    ConvertWorkspace* m_convertWorkspace = nullptr;
    QToolButton* m_authorModeButton = nullptr;
    QToolButton* m_convertModeButton = nullptr;
    QMap<QString, QWidget*> m_pageByName;
    QMap<QString, QPair<QString, QString>> m_searchTargets;
    QString m_currentPage;
    QString m_recentDirectory;
    QString m_textureRoot;
    QUndoStack* m_undoStack = nullptr;
    QFileSystemWatcher* m_textureWatcher = nullptr;
    QTimer* m_textureReloadTimer = nullptr;
    QTimer* m_autosaveTimer = nullptr;
    QSet<QString> m_pendingTextureReloads;
    QMenu* m_recentMenu = nullptr;
    QMenu* m_templateMenu = nullptr;
    QAction* m_previewAction = nullptr;
    bool m_advancedInterface = false;
    QString m_navigationSection;
};
