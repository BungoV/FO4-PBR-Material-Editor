#pragma once

#include "textureimage.h"

#include <QDialog>
#include <QVector>

class QLabel;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

class TextureBrowserDialog final : public QDialog {
public:
    explicit TextureBrowserDialog(const QString& textureRoot = {}, QWidget* parent = nullptr);

    QString selectedPath() const { return m_selectedPath; }
    QString selectedAbsolutePath() const { return m_selectedAbsolutePath; }

private:
    void reload();
    void applyFilter(const QString& text);
    void updatePreview(QTreeWidgetItem* item);
    void acceptSelection();
    void requestChildThumbnails(QTreeWidgetItem* folder);

    int m_generation = 0;
    QString m_textureRoot;
    QString m_selectedPath;
    QString m_selectedAbsolutePath;
    QVector<TextureImage::Resource> m_resources;
    QLineEdit* m_search = nullptr;
    QTreeWidget* m_tree = nullptr;
    QLabel* m_preview = nullptr;
    QLabel* m_details = nullptr;
};
