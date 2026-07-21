#include "texturebrowserdialog.h"

#include "texturecache.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStyle>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

constexpr int kResourceIndexRole = Qt::UserRole;
constexpr int kResourcePathRole = Qt::UserRole + 1;
constexpr int kThumbnailRequestedRole = Qt::UserRole + 2;

bool filterTreeItem(QTreeWidgetItem* item, const QString& needle, bool ancestorMatches = false)
{
    const bool ownMatch = ancestorMatches || needle.isEmpty()
        || item->text(0).contains(needle, Qt::CaseInsensitive)
        || item->text(1).contains(needle, Qt::CaseInsensitive)
        || item->text(2).contains(needle, Qt::CaseInsensitive)
        || item->data(0, kResourcePathRole).toString().contains(needle, Qt::CaseInsensitive);
    bool childMatch = false;
    for (int i = 0; i < item->childCount(); ++i)
        childMatch = filterTreeItem(item->child(i), needle, ownMatch) || childMatch;
    const bool visible = ownMatch || childMatch;
    item->setHidden(!visible);
    if (!needle.isEmpty() && childMatch) item->setExpanded(true);
    return visible;
}

} // namespace

TextureBrowserDialog::TextureBrowserDialog(const QString& textureRoot, QWidget* parent)
    : QDialog(parent), m_textureRoot(textureRoot)
{
    setWindowTitle(tr("Texture Browser"));
    resize(1000, 680);
    auto* outer = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("Browse loose textures and configured Fallout 4 BA2 archives in their authored folder hierarchy. Earlier sources override later sources when paths collide."), this);
    intro->setWordWrap(true);
    outer->addWidget(intro);

    auto* searchRow = new QHBoxLayout();
    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("TextureBrowserSearch"));
    m_search->setPlaceholderText(tr("Filter folders, textures, or sources…"));
    auto* refresh = new QPushButton(tr("Refresh"), this);
    auto* looseFile = new QPushButton(tr("Browse File…"), this);
    searchRow->addWidget(m_search, 1);
    searchRow->addWidget(refresh);
    searchRow->addWidget(looseFile);
    outer->addLayout(searchRow);

    auto* content = new QHBoxLayout();
    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("TextureBrowserTree"));
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({tr("Name"), tr("Source"), tr("Kind")});
    m_tree->setRootIsDecorated(true);
    m_tree->setItemsExpandable(true);
    m_tree->setExpandsOnDoubleClick(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setIconSize(QSize(28, 28));
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    content->addWidget(m_tree, 3);

    auto* previewPanel = new QWidget(this);
    auto* previewLayout = new QVBoxLayout(previewPanel);
    previewLayout->setContentsMargins(8, 0, 0, 0);
    m_preview = new QLabel(previewPanel);
    m_preview->setMinimumSize(300, 300);
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setStyleSheet(QStringLiteral("background:#202225;border:1px solid #4b5058;"));
    m_preview->setText(tr("Select a texture"));
    m_details = new QLabel(previewPanel);
    m_details->setWordWrap(true);
    m_details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    previewLayout->addWidget(m_preview, 1);
    previewLayout->addWidget(m_details);
    content->addWidget(previewPanel, 2);
    outer->addLayout(content, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel, this);
    auto* useTexture = buttons->button(QDialogButtonBox::Open);
    useTexture->setText(tr("Use Texture"));
    useTexture->setEnabled(false);
    outer->addWidget(buttons);

    connect(m_search, &QLineEdit::textChanged, this, &TextureBrowserDialog::applyFilter);
    connect(refresh, &QPushButton::clicked, this, &TextureBrowserDialog::reload);
    connect(looseFile, &QPushButton::clicked, this, [this] {
        const QString start = TextureImage::searchRoots(m_textureRoot).value(0, m_textureRoot);
        const QString file = QFileDialog::getOpenFileName(this, tr("Select texture"), start,
            tr("Textures (*.dds *.tga *.png *.jpg *.jpeg *.bmp);;All files (*.*)"));
        if (file.isEmpty()) return;
        m_selectedPath = TextureImage::authoredPathForFile(file, m_textureRoot);
        m_selectedAbsolutePath = QFileInfo(file).absoluteFilePath();
        accept();
    });
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this, useTexture](QTreeWidgetItem* current) {
        updatePreview(current);
        useTexture->setEnabled(current && current->data(0, kResourceIndexRole).toInt() >= 0);
    });
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        if (item && item->data(0, kResourceIndexRole).toInt() >= 0) acceptSelection();
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &TextureBrowserDialog::acceptSelection);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    // Thumbnails decode lazily as folders are expanded, so opening the browser
    // over thousands of BA2 entries stays instant.
    connect(m_tree, &QTreeWidget::itemExpanded, this, &TextureBrowserDialog::requestChildThumbnails);
    reload();
}

void TextureBrowserDialog::requestChildThumbnails(QTreeWidgetItem* folder)
{
    if (!folder) return;
    for (int i = 0; i < folder->childCount(); ++i) {
        QTreeWidgetItem* child = folder->child(i);
        const int index = child->data(0, kResourceIndexRole).toInt();
        if (index < 0 || index >= m_resources.size()) continue;
        if (child->data(0, kThumbnailRequestedRole).toBool()) continue;
        child->setData(0, kThumbnailRequestedRole, true);
        const int generation = m_generation;
        TextureCache::requestThumbnail(m_resources[index].path, m_textureRoot, QSize(28, 28), this,
            [this, child, generation](const QImage& image) {
                if (generation != m_generation || image.isNull()) return;
                child->setIcon(0, QIcon(QPixmap::fromImage(image)));
            });
    }
}

void TextureBrowserDialog::reload()
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    ++m_generation;
    QStringList errors;
    m_resources = TextureImage::enumerate(m_textureRoot, &errors);
    m_tree->clear();
    QHash<QString, QTreeWidgetItem*> folders;
    for (int i = 0; i < m_resources.size(); ++i) {
        const auto& resource = m_resources[i];
        const QString normalizedPath = QDir::fromNativeSeparators(resource.path);
        const QStringList parts = normalizedPath.split(QChar('/'), Qt::SkipEmptyParts);
        if (parts.isEmpty()) continue;

        QTreeWidgetItem* parent = nullptr;
        QString folderPath;
        for (int partIndex = 0; partIndex + 1 < parts.size(); ++partIndex) {
            if (!folderPath.isEmpty()) folderPath += QChar('/');
            folderPath += parts[partIndex];
            const QString key = folderPath.toLower();
            QTreeWidgetItem* folder = folders.value(key);
            if (!folder) {
                folder = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
                folder->setText(0, parts[partIndex]);
                folder->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
                folder->setData(0, kResourceIndexRole, -1);
                folder->setData(0, kResourcePathRole, QDir::toNativeSeparators(folderPath));
                folder->setToolTip(0, QDir::toNativeSeparators(folderPath));
                folders.insert(key, folder);
            }
            parent = folder;
        }

        auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
        item->setText(0, parts.constLast());
        item->setText(1, resource.source);
        item->setText(2, resource.archived ? tr("BA2") : tr("Loose"));
        item->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
        item->setData(0, kResourceIndexRole, i);
        item->setData(0, kResourcePathRole, resource.path);
        item->setToolTip(0, resource.archived ? tr("%1 in %2").arg(resource.path, resource.source) : resource.absolutePath);
    }
    QApplication::restoreOverrideCursor();
    applyFilter(m_search->text());
    setWindowTitle(tr("Texture Browser — %1 textures").arg(m_resources.size()));
    if (!errors.isEmpty()) QMessageBox::warning(this, tr("Some archives could not be loaded"), errors.join(QStringLiteral("\n")));
}

void TextureBrowserDialog::applyFilter(const QString& text)
{
    const QString needle = text.trimmed();
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
        filterTreeItem(m_tree->topLevelItem(i), needle);
}

void TextureBrowserDialog::updatePreview(QTreeWidgetItem* item)
{
    const int index = item ? item->data(0, kResourceIndexRole).toInt() : -1;
    if (index < 0 || index >= m_resources.size()) {
        m_preview->setPixmap({});
        m_preview->setText(tr("Select a texture"));
        m_details->setText(item ? tr("Folder: %1").arg(item->data(0, kResourcePathRole).toString()) : QString());
        return;
    }
    const auto& resource = m_resources[index];
    QString error;
    const QImage image = TextureCache::image(resource.path, m_textureRoot);
    const auto diagnostics = TextureImage::inspect(resource.path, m_textureRoot);
    if (!image.isNull()) {
        const QPixmap pixmap = QPixmap::fromImage(image).scaled(m_preview->size() - QSize(18, 18), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        m_preview->setPixmap(pixmap);
    } else {
        m_preview->setPixmap({});
        m_preview->setText(tr("Preview unavailable\n%1").arg(error));
    }
    m_details->setText(tr("%1\nSource: %2 (%3)\n%4")
        .arg(resource.path, resource.source, resource.archived ? tr("BA2 archive") : tr("loose file"), diagnostics.summary()));
}

void TextureBrowserDialog::acceptSelection()
{
    QTreeWidgetItem* item = m_tree->currentItem();
    if (!item || item->isHidden()) return;
    const int index = item->data(0, kResourceIndexRole).toInt();
    if (index < 0 || index >= m_resources.size()) return;
    const auto& resource = m_resources[index];
    m_selectedPath = resource.path;
    m_selectedAbsolutePath = resource.absolutePath;
    accept();
}
