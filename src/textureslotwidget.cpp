#include "textureslotwidget.h"

#include "colorpicker.h"
#include "disclosureicon.h"
#include "dragnumericedit.h"
#include "texturebrowserdialog.h"
#include "texturecache.h"
#include "textureimage.h"
#include "texturewriter.h"
#include "wheelfocusguard.h"

#include <QAction>
#include <QCheckBox>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QGridLayout>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QIcon>
#include <QFileInfo>
#include <QFontMetrics>
#include <QJsonDocument>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSet>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

bool isChannelSource(const FieldSpec& field)
{
    return field.type == FieldType::Toggle
        && field.key.startsWith(QStringLiteral("override"))
        && field.label.contains(QChar(0x2022));
}

QString fallbackKey(const QString& sourceKey, const QVector<FieldSpec>& fields)
{
    if (!sourceKey.startsWith(QStringLiteral("override")) || sourceKey.size() <= 8) return {};
    QString base = sourceKey.mid(8);
    base[0] = base[0].toLower();
    const QStringList candidates{base, base + QStringLiteral("Fallback")};
    for (const QString& candidate : candidates)
        for (const FieldSpec& field : fields)
            if (field.key == candidate && field.type != FieldType::Toggle) return candidate;
    return {};
}

bool isOverrideChannel(const FieldSpec& field, const QVector<FieldSpec>& fields)
{
    return isChannelSource(field) && !fallbackKey(field.key, fields).isEmpty();
}

void installDefaultReset(QWidget* editor, const QJsonValue& defaultValue)
{
    auto* reset = new QAction(editor);
    reset->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Backspace));
    reset->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    reset->setText(QObject::tr("Reset focused value to default"));
    editor->addAction(reset);
    QObject::connect(reset, &QAction::triggered, editor, [editor, defaultValue] {
        if (auto* spin = qobject_cast<QDoubleSpinBox*>(editor)) spin->setValue(defaultValue.toDouble());
        else if (auto* check = qobject_cast<QCheckBox*>(editor)) check->setChecked(defaultValue.toBool());
        else if (auto* combo = qobject_cast<QComboBox*>(editor)) combo->setCurrentText(defaultValue.toString());
        else if (auto* color = qobject_cast<ColorEdit*>(editor)) color->setColor(QColor(defaultValue.toString()));
        else if (auto* line = qobject_cast<QLineEdit*>(editor)) line->setText(defaultValue.toString());
    });
}

} // namespace

TextureSlotWidget::TextureSlotWidget(const QString& uvSet, const TextureSlotSpec& spec,
    const Pbr::TextureSlotState& state, const QString& textureRoot, QWidget* parent)
    : QFrame(parent)
    , m_uvSet(uvSet)
    , m_textureRoot(textureRoot)
    , m_spec(spec)
    , m_state(state)
{
    setObjectName(QStringLiteral("Card"));
    setFrameShape(QFrame::StyledPanel);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    setToolTip(spec.tooltip);
    setAcceptDrops(true);

    if (m_state.path.isNull()) m_state.path = spec.defaultPath;
    if (state.path.isNull() && !state.enabled) m_state.enabled = spec.defaultEnabled;
    if (state.path.isNull()) m_state.expanded = spec.defaultExpanded;

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(5, 4, 5, 4);
    outer->setSpacing(3);

    auto* header = new QHBoxLayout();
    header->setSpacing(5);
    m_enableButton = new QToolButton(this);
    m_enableButton->setObjectName(QStringLiteral("InputToggle"));
    m_enableButton->setCheckable(true);
    m_enableButton->setChecked(m_state.enabled);
    m_enableButton->setFixedSize(25, 25);
    m_enableButton->setToolTip(tr("Enable or disable this texture input without changing its expanded state."));
    header->addWidget(m_enableButton);

    m_collapseButton = new QToolButton(this);
    m_collapseButton->setObjectName(QStringLiteral("DisclosureButton"));
    m_collapseButton->setAutoRaise(false);
    m_collapseButton->setFixedWidth(27);
    m_collapseButton->setIconSize(QSize(14, 14));
    m_collapseButton->setToolTip(tr("Expand or collapse this texture input."));
    header->addWidget(m_collapseButton);

    m_previewButton = new QToolButton(this);
    m_previewButton->setText(spec.abbreviation);
    const int badgeWidth = qBound(54, QFontMetrics(m_previewButton->font()).horizontalAdvance(spec.abbreviation) + 22, 92);
    m_previewButton->setFixedSize(badgeWidth, 30);
    m_previewButton->setStyleSheet(QStringLiteral("QToolButton { background: %1; color: white; font-weight: 600; }").arg(spec.color.name()));
    m_previewButton->setToolTip(tr("Inspect this texture and its packed channels in Material Preview."));
    header->addWidget(m_previewButton);

    auto* titles = new QVBoxLayout();
    titles->setContentsMargins(3, 0, 0, 0);
    titles->setSpacing(0);
    m_title = new QLabel(spec.name, this);
    m_title->setObjectName(QStringLiteral("SectionTitle"));
    m_format = new QLabel(spec.format, this);
    m_format->setObjectName(QStringLiteral("Muted"));
    titles->addWidget(m_title);
    titles->addWidget(m_format);
    header->addLayout(titles, 1);
    auto* actions = new QToolButton(this);
    actions->setText(QStringLiteral("⋯"));
    actions->setPopupMode(QToolButton::InstantPopup);
    actions->setToolTip(tr("Copy, paste, or reset this complete texture input."));
    auto* menu = new QMenu(actions);
    auto* copyAction = menu->addAction(tr("Copy Texture Input"));
    auto* pasteAction = menu->addAction(tr("Paste Texture Input"));
    menu->addSeparator();
    if (!m_spec.channels.isEmpty()) {
        auto* channelSources = menu->addAction(tr("Channel Sources…"));
        channelSources->setToolTip(tr("Redirect single channels of this packed input to another texture file."));
        connect(channelSources, &QAction::triggered, this, &TextureSlotWidget::showChannelSourcesDialog);
        auto* exportPacked = menu->addAction(tr("Export Packed Texture…"));
        exportPacked->setToolTip(tr("Bake the composited channels to a game-ready DDS (or PNG) and assign it."));
        connect(exportPacked, &QAction::triggered, this, &TextureSlotWidget::exportPackedTexture);
        menu->addSeparator();
    }
    auto* resetAction = menu->addAction(tr("Reset to Shader Default"));
    actions->setMenu(menu);
    header->addWidget(actions);
    m_resetButton = actions;
    outer->addLayout(header);

    m_body = new QWidget(this);
    auto* bodyLayout = new QVBoxLayout(m_body);
    bodyLayout->setContentsMargins(62, 0, 0, 0);
    bodyLayout->setSpacing(3);

    auto* pathRow = new QHBoxLayout();
    m_path = new QLineEdit(m_state.path, m_body);
    m_path->setObjectName(QStringLiteral("TexturePath"));
    m_path->setPlaceholderText(tr("Drop or browse for a texture — constant overrides are used when empty"));
    m_path->setToolTip(tr("Portable texture path sampled by this input. Browse configured loose folders and BA2 archives, or drop a file; absolute files under a Textures directory are stored relative to it."));
    auto* browse = new QPushButton(tr("Browse…"), m_body);
    browse->setToolTip(tr("Choose a DDS, TGA, PNG, or other authoring texture."));
    m_findSetButton = new QPushButton(tr("Find Set"), m_body);
    m_findSetButton->setObjectName(QStringLiteral("FindTextureSetButton"));
    m_findSetButton->setToolTip(tr("Find and assign unfilled companion textures from this texture's folder."));
    auto* clear = new QToolButton(m_body);
    clear->setText(tr("Clear"));
    clear->setToolTip(tr("Remove the texture path and use the channel constant overrides."));
    pathRow->addWidget(m_path, 1);
    pathRow->addWidget(browse);
    pathRow->addWidget(m_findSetButton);
    pathRow->addWidget(clear);
    bodyLayout->addLayout(pathRow);
    m_pathBadge = new QLabel(m_body);
    m_pathBadge->setObjectName(QStringLiteral("PathBadge"));
    m_pathBadge->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    bodyLayout->addWidget(m_pathBadge, 0, Qt::AlignLeft);
    m_swapSummary = new QLabel(m_body);
    m_swapSummary->setObjectName(QStringLiteral("Muted"));
    m_swapSummary->setWordWrap(true);
    m_swapSummary->hide();
    bodyLayout->addWidget(m_swapSummary);
    m_diagnostics = new QLabel(m_body);
    m_diagnostics->setObjectName(QStringLiteral("Muted"));
    m_diagnostics->setWordWrap(true);
    bodyLayout->addWidget(m_diagnostics);

    QVector<FieldSpec> channelFields;
    for (const FieldSpec& field : spec.fields)
        if (isOverrideChannel(field, spec.fields)) channelFields.append(field);
    if (!channelFields.isEmpty()) {
        m_channelMaster = new QCheckBox(m_body);
        m_channelMaster->setTristate(true);
        m_channelMaster->setToolTip(tr("Enable or disable every available constant override together. Missing texture channels remain overridden."));
        m_channelMaster->hide();
    }

    QHBoxLayout* channelSourceRow = nullptr;
    if (!channelFields.isEmpty()) {
        auto* sourceHost = new QWidget(m_body);
        channelSourceRow = new QHBoxLayout(sourceHost);
        channelSourceRow->setContentsMargins(0, 0, 0, 0);
        channelSourceRow->setSpacing(12);
        auto* sourceLabel = new QLabel(tr("Constant overrides"), sourceHost);
        sourceLabel->setObjectName(QStringLiteral("Muted"));
        channelSourceRow->addWidget(sourceLabel);
        channelSourceRow->addStretch();
        bodyLayout->addWidget(sourceHost);
    }
    auto* form = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(4);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    for (const FieldSpec& field : spec.fields) {
        const QJsonValue value = m_state.values.contains(field.key) ? m_state.values.value(field.key) : field.defaultValue;
        QWidget* editor = createFieldEditor(field, value);
        auto* label = new QLabel(field.label, m_body);
        label->setObjectName(QStringLiteral("Muted"));
        label->setToolTip(field.tooltip);
        editor->setToolTip(field.tooltip);
        installDefaultReset(editor, field.defaultValue);
        editor->setToolTip(editor->toolTip() + tr("\nCtrl+Backspace resets this value to its default."));
        m_fieldEditors.insert(field.key, editor);
        m_fieldLabels.insert(field.key, label);
        if (channelSourceRow && isOverrideChannel(field, spec.fields)) {
            auto* check = qobject_cast<QCheckBox*>(editor);
            const QString channel = field.label.section(QChar(0x2022), 0, 0).trimmed();
            check->setText(channel);
            check->setToolTip(field.label + QStringLiteral("\n") + field.tooltip
                + tr("\nChecked uses the constant below; unchecked uses the texture channel."));
            label->hide();
            channelSourceRow->insertWidget(channelSourceRow->count() - 1, check);
        } else {
            form->addRow(label, editor);
        }
    }
    bodyLayout->addLayout(form);
    outer->addWidget(m_body);

    connect(m_enableButton, &QToolButton::toggled, this, [this](bool enabled) {
        m_state.enabled = enabled;
        updateEnabledState();
        emitStateChanged();
    });
    connect(m_collapseButton, &QToolButton::clicked, this, [this] {
        m_state.expanded = !m_state.expanded;
        updateExpandedState();
        emitStateChanged();
    });
    connect(m_previewButton, &QToolButton::clicked, this, [this] {
        emit inspectRequested(m_spec.name, m_state.path, m_spec.channels, m_spec.color);
    });
    connect(m_path, &QLineEdit::editingFinished, this, [this] {
        const QFileInfo file(m_path->text().trimmed());
        const QString authored = file.isAbsolute() && file.exists()
            ? TextureImage::authoredPathForFile(file.absoluteFilePath(), m_textureRoot) : m_path->text().trimmed();
        assignTexturePath(authored);
    });
    connect(browse, &QPushButton::clicked, this, [this] {
        TextureBrowserDialog dialog(m_textureRoot, this);
        if (dialog.exec() != QDialog::Accepted || dialog.selectedPath().isEmpty()) return;
        assignTexturePath(dialog.selectedPath());
    });
    connect(clear, &QToolButton::clicked, this, [this] { assignTexturePath(QString()); });
    connect(m_findSetButton, &QPushButton::clicked, this, [this] {
        const QString resolved = TextureImage::resolvePath(m_state.path, m_textureRoot);
        if (QFileInfo(resolved).isFile())
            emit companionTexturesRequested(m_uvSet, m_spec.key, QFileInfo(resolved).absoluteFilePath());
    });
    connect(copyAction, &QAction::triggered, this, [this] {
        const QByteArray json = QJsonDocument(m_state.toJson()).toJson(QJsonDocument::Compact);
        auto* mime = new QMimeData();
        mime->setData(QStringLiteral("application/x-fo4-pbrm-texture-slot"), json);
        mime->setText(QString::fromUtf8(json));
        QApplication::clipboard()->setMimeData(mime);
    });
    connect(pasteAction, &QAction::triggered, this, [this] {
        const QMimeData* mime = QApplication::clipboard()->mimeData();
        const QByteArray json = mime->hasFormat(QStringLiteral("application/x-fo4-pbrm-texture-slot"))
            ? mime->data(QStringLiteral("application/x-fo4-pbrm-texture-slot")) : mime->text().toUtf8();
        const QJsonDocument document = QJsonDocument::fromJson(json);
        if (document.isObject()) emit pasteRequested(m_uvSet, m_spec.key, Pbr::TextureSlotState::fromJson(document.object()));
    });
    connect(resetAction, &QAction::triggered, this, [this] { emit resetRequested(m_uvSet, m_spec.key); });
    if (m_channelMaster) {
        connect(m_channelMaster, &QCheckBox::checkStateChanged, this, [this](Qt::CheckState state) {
            if (state != Qt::PartiallyChecked) setAllChannelSources(state == Qt::Checked);
        });
    }

    Pbr::TextureSlotState normalized = stateWithAllChannelSources(false);
    for (const FieldSpec& field : m_spec.fields) {
        if (!isOverrideChannel(field, m_spec.fields)) continue;
        if (m_state.values.contains(field.key) && m_state.values.value(field.key).toBool())
            normalized.values.insert(field.key, true);
    }
    m_state.values = normalized.values;

    updateThumbnail();
    updateEnabledState();
    updateExpandedState();
    updateChannelControls();
    updateAdvancedVisibility();
}

QString TextureSlotWidget::key() const { return m_spec.key; }
QString TextureSlotWidget::uvSet() const { return m_uvSet; }
Pbr::TextureSlotState TextureSlotWidget::state() const { return m_state; }
bool TextureSlotWidget::isSlotEnabled() const { return m_state.enabled; }

bool TextureSlotWidget::hasChannelSources() const
{
    return m_channelMaster != nullptr;
}

bool TextureSlotWidget::textureProvidesChannels(const FieldSpec& field, const QString& path) const
{
    if (path.trimmed().isEmpty()) return false;
    const TextureImage::Diagnostics info = TextureImage::inspect(path, m_textureRoot);
    if (!info.valid) return false;
    const QString channels = field.label.section(QChar(0x2022), 0, 0).trimmed();
    for (const QChar channel : channels)
        if (QStringLiteral("RGBA").contains(channel) && !info.channels.contains(channel)) return false;
    return true;
}

bool TextureSlotWidget::channelOverrideApplicable(const FieldSpec& field, const QJsonObject& values) const
{
    const auto fieldDefault = [this](const QString& key, bool fallback) {
        for (const FieldSpec& candidate : m_spec.fields)
            if (candidate.key == key) return candidate.defaultValue.toBool(fallback);
        return fallback;
    };
    if (field.key == QStringLiteral("overrideHeight") && m_fieldEditors.contains(QStringLiteral("heightInBlue")))
        return values.value(QStringLiteral("heightInBlue")).toBool(fieldDefault(QStringLiteral("heightInBlue"), false));
    // RMAOS alpha is dual-duty: the F0 channel toggle only applies while the
    // alpha actually carries F0. (Porosity's constant toggle is deliberately
    // not a channel source at all — unchecked with no texture porosity means
    // the derived roughness/metallic heuristic, never a forced constant.)
    if (field.key == QStringLiteral("overrideF0") && m_fieldEditors.contains(QStringLiteral("alphaCarries")))
        return values.value(QStringLiteral("alphaCarries")).toString(QStringLiteral("Dielectric F0")) == QLatin1String("Dielectric F0");
    return true;
}

Pbr::TextureSlotState TextureSlotWidget::stateWithAllChannelSources(bool overrideChannels) const
{
    Pbr::TextureSlotState result = m_state;
    for (const FieldSpec& field : m_spec.fields) {
        if (!isOverrideChannel(field, m_spec.fields)) continue;
        const bool applicable = channelOverrideApplicable(field, result.values);
        result.values.insert(field.key, applicable && (overrideChannels || !textureProvidesChannels(field, result.path)));
    }
    return result;
}

void TextureSlotWidget::setAllChannelSources(bool overrideChannels)
{
    bool changed = false;
    for (const FieldSpec& field : m_spec.fields) {
        if (!isOverrideChannel(field, m_spec.fields)) continue;
        const bool applicable = channelOverrideApplicable(field, m_state.values);
        const bool next = applicable && (overrideChannels || !textureProvidesChannels(field, m_state.path));
        const bool current = m_state.values.contains(field.key)
            ? m_state.values.value(field.key).toBool(field.defaultValue.toBool())
            : field.defaultValue.toBool();
        changed = changed || current != next;
        m_state.values.insert(field.key, next);
        if (auto* check = qobject_cast<QCheckBox*>(m_fieldEditors.value(field.key))) {
            const QSignalBlocker blocker(check);
            check->setChecked(next);
        }
    }
    updateChannelControls();
    if (changed) emitStateChanged();
}

void TextureSlotWidget::setExpanded(bool expanded)
{
    if (m_state.expanded == expanded) return;
    m_state.expanded = expanded;
    updateExpandedState();
    emitStateChanged();
}

void TextureSlotWidget::setAdvancedVisible(bool visible)
{
    m_advancedVisible = visible;
    updateAdvancedVisibility();
}

void TextureSlotWidget::updateAdvancedVisibility()
{
    static const QSet<QString> basicKeys{
        QStringLiteral("tint"), QStringLiteral("scale"), QStringLiteral("blend"),
        QStringLiteral("strength"), QStringLiteral("color"), QStringLiteral("opacity"), QStringLiteral("intensity"),
        QStringLiteral("roughness"), QStringLiteral("metallic"), QStringLiteral("ao"), QStringLiteral("f0"),
        QStringLiteral("heightInBlue")
    };
    for (auto it = m_fieldEditors.cbegin(); it != m_fieldEditors.cend(); ++it) {
        const QString key = it.key();
        if (key.startsWith(QStringLiteral("override"))) continue;
        const bool show = m_advancedVisible || basicKeys.contains(key);
        it.value()->setVisible(show);
        if (QLabel* label = m_fieldLabels.value(key)) label->setVisible(show);
    }
    updateChannelControls();
}

void TextureSlotWidget::updateExpandedState()
{
    m_body->setVisible(m_state.enabled && m_state.expanded);
    m_collapseButton->setIcon(disclosureArrowIcon(m_state.expanded, 14, QColor(223, 226, 230)));
    m_format->setVisible(m_state.enabled && m_state.expanded);
}

void TextureSlotWidget::updateEnabledState()
{
    m_enableButton->setText(QString());
    m_enableButton->setIcon(m_state.enabled ? QIcon(QStringLiteral(":/check.xpm")) : QIcon());
    m_enableButton->setIconSize(QSize(14, 14));
    m_title->setEnabled(m_state.enabled);
    m_previewButton->setVisible(m_state.enabled);
    m_collapseButton->setVisible(m_state.enabled);
    m_resetButton->setVisible(true);
    setProperty("slotEnabled", m_state.enabled);
    updateExpandedState();
}

void TextureSlotWidget::updateThumbnail()
{
    const QSize thumbnailSize(m_previewButton->width() - 4, 34);
    const auto applyThumbnail = [this, thumbnailSize](const QImage& image) {
        if (image.isNull()) return;
        m_previewButton->setIcon(QIcon(QPixmap::fromImage(image)));
        m_previewButton->setIconSize(thumbnailSize);
        m_previewButton->setText(QString());
        m_previewButton->setToolTip(tr("Inspect %1 and its packed channels.").arg(QFileInfo(TextureImage::resolvePath(m_state.path, m_textureRoot)).fileName()));
    };
    const QImage cached = TextureCache::cachedThumbnail(m_state, m_textureRoot, thumbnailSize);
    if (!cached.isNull()) {
        applyThumbnail(cached);
    } else {
        // Show the abbreviation badge immediately; the decode arrives async.
        m_previewButton->setIcon(QIcon());
        m_previewButton->setText(m_spec.abbreviation);
        m_previewButton->setToolTip(tr("Inspect this texture input."));
        if (!m_state.path.trimmed().isEmpty() || TextureCache::hasChannelSwaps(m_state.values))
            TextureCache::requestThumbnail(m_state, m_textureRoot, thumbnailSize, this, applyThumbnail);
    }
    updateSwapSummary();

    const QString trimmedPath = m_state.path.trimmed();
    const QString resolvedPath = TextureImage::resolvePath(trimmedPath, m_textureRoot);
    const bool hasLooseSource = QFileInfo(resolvedPath).isFile();
    m_findSetButton->setEnabled(m_state.enabled && hasLooseSource);
    m_findSetButton->setToolTip(hasLooseSource
        ? tr("Find and assign unfilled companion textures from this texture's folder.")
        : tr("Find Set is available for a resolved loose texture."));
    if (trimmedPath.isEmpty()) {
        m_pathBadge->hide();
    } else {
        const bool absolute = QFileInfo(trimmedPath).isAbsolute();
        const bool resolved = !TextureImage::resolvePath(trimmedPath, m_textureRoot).isEmpty();
        const QString pathKind = !resolved ? QStringLiteral("invalid")
            : (absolute ? QStringLiteral("absolute") : QStringLiteral("portable"));
        m_pathBadge->setText(!resolved ? tr("UNRESOLVED")
            : (absolute ? tr("ABSOLUTE • NONPORTABLE") : tr("PORTABLE RELATIVE PATH")));
        m_pathBadge->setProperty("pathKind", pathKind);
        m_pathBadge->style()->unpolish(m_pathBadge);
        m_pathBadge->style()->polish(m_pathBadge);
        m_pathBadge->show();
    }

    if (m_state.enabled && !m_state.path.trimmed().isEmpty()) {
        const TextureImage::Diagnostics info = TextureImage::inspect(m_state.path, m_textureRoot);
        if (info.valid) {
            QStringList missing;
            for (const FieldSpec& field : m_spec.fields) {
                if (!isOverrideChannel(field, m_spec.fields) || !channelOverrideApplicable(field, m_state.values)) continue;
                const bool overridden = m_state.values.contains(field.key)
                    ? m_state.values.value(field.key).toBool(field.defaultValue.toBool())
                    : field.defaultValue.toBool();
                if (overridden) continue;
                const QString channel = field.label.section(QChar(0x2022), 0, 0).trimmed();
                for (const QChar component : channel)
                    if (QStringLiteral("RGBA").contains(component) && !info.channels.contains(component))
                        missing.append(component);
            }
            missing.removeDuplicates();
            QStringList warnings;
            if (qMax(info.width, info.height) > 1 && info.mipLevels == 1)
                warnings.append(tr("missing mip chain"));
            const bool expectsSrgb = m_spec.format.startsWith(QStringLiteral("sRGB"), Qt::CaseInsensitive);
            const bool expectsLinear = m_spec.format.startsWith(QStringLiteral("Linear"), Qt::CaseInsensitive);
            if (expectsSrgb && info.colorSpace != QStringLiteral("sRGB")) warnings.append(tr("expected sRGB"));
            if (expectsLinear && info.colorSpace == QStringLiteral("sRGB")) warnings.append(tr("expected linear color space"));
            if (m_spec.key.contains(QStringLiteral("Normal"), Qt::CaseInsensitive)) {
                const bool height = m_state.values.value(QStringLiteral("heightInBlue")).toBool(false);
                if (height && info.format != QStringLiteral("BC7")) warnings.append(tr("normal+height should use BC7"));
                else if (!height && info.format != QStringLiteral("BC5") && info.format != QStringLiteral("BC7"))
                    warnings.append(tr("normal maps should use BC5 or BC7"));
            }
            if (m_spec.key.contains(QStringLiteral("Rmaos"), Qt::CaseInsensitive)
                && !m_state.values.value(QStringLiteral("overrideF0")).toBool(true)
                && info.format != QStringLiteral("BC3") && info.format != QStringLiteral("BC7")
                && info.format != QStringLiteral("RGBA32"))
                warnings.append(tr("authored F0 requires an alpha-capable format such as BC7"));
            QString text = info.summary();
            if (!warnings.isEmpty()) text += tr(" | Warning: %1").arg(warnings.join(QStringLiteral("; ")));
            if (!missing.isEmpty()) text += tr(" • Missing %1 (constant/optional)").arg(missing.join(QStringLiteral(", ")));
            m_diagnostics->setText(text);
            m_diagnostics->setToolTip(info.resolvedPath);
            m_diagnostics->show();
        } else {
            m_diagnostics->setText(info.error);
            m_diagnostics->show();
        }
    } else {
        m_diagnostics->clear();
        m_diagnostics->hide();
    }
}

void TextureSlotWidget::updateChannelControls()
{
    int overrideCount = 0;
    int channelCount = 0;
    for (const FieldSpec& field : m_spec.fields) {
        if (!isOverrideChannel(field, m_spec.fields)) continue;
        const bool applicable = channelOverrideApplicable(field, m_state.values);
        if (applicable) ++channelCount;
        const bool available = applicable && textureProvidesChannels(field, m_state.path);
        const bool overridden = applicable && (!available || (m_state.values.contains(field.key)
            ? m_state.values.value(field.key).toBool(field.defaultValue.toBool())
            : field.defaultValue.toBool()));
        if (!applicable || !available) m_state.values.insert(field.key, overridden);
        if (overridden) ++overrideCount;
        if (auto* check = qobject_cast<QCheckBox*>(m_fieldEditors.value(field.key))) {
            const QSignalBlocker blocker(check);
            check->setChecked(overridden);
            check->setEnabled(applicable && available);
            check->setToolTip(!applicable
                ? field.label + tr("\nThis channel is disabled by the texture input's feature switch.")
                : available
                ? field.label + tr("\nChecked uses the constant below; unchecked uses the texture channel.")
                : field.label + tr("\nThe texture does not provide this channel, so its constant override is required."));
        }
        const QString fallback = fallbackKey(field.key, m_spec.fields);
        if (fallback.isEmpty()) continue;
        QWidget* editor = m_fieldEditors.value(fallback);
        QLabel* label = m_fieldLabels.value(fallback);
        if (!editor) continue;
        editor->setEnabled(overridden);
        const QString reason = overridden
            ? tr("Active because %1 uses its constant override.").arg(field.label)
            : tr("Disabled because %1 is sourced from the texture. Check the override to edit this value.").arg(field.label);
        editor->setToolTip(reason + tr("\nCtrl+Backspace resets this value to its default."));
        const bool basicAllowed = true;
        editor->setVisible(basicAllowed);
        if (label) { label->setEnabled(overridden); label->setToolTip(reason); label->setVisible(basicAllowed); }
    }
    if (m_channelMaster) {
        const QSignalBlocker blocker(m_channelMaster);
        m_channelMaster->setCheckState(overrideCount == 0 ? Qt::Unchecked
            : (overrideCount == channelCount ? Qt::Checked : Qt::PartiallyChecked));
    }
    if (auto* height = qobject_cast<QCheckBox*>(m_fieldEditors.value(QStringLiteral("heightInBlue")))) {
        const TextureImage::Diagnostics info = TextureImage::inspect(m_state.path, m_textureRoot);
        const bool hasTexture = !m_state.path.trimmed().isEmpty();
        const bool hasBlue = info.valid && info.channels.contains(QChar('B'));
        height->setEnabled(!hasTexture || hasBlue);
        height->setToolTip(hasTexture && !hasBlue
            ? tr("This texture has no blue channel. Height-in-blue is unavailable; RG remains a normal-only map.")
            : tr("Checked treats blue as packed height. Unchecked ignores blue for height and reconstructs the tangent-space normal from RG."));
    }
}

void TextureSlotWidget::assignTextureFile(const QString& fileName)
{
    const QString authoredPath = TextureImage::authoredPathForFile(fileName, m_textureRoot);
    assignTexturePath(authoredPath);
}

void TextureSlotWidget::assignTexturePath(const QString& authoredPath)
{
    const QString oldPath = m_state.path;
    for (const FieldSpec& field : m_spec.fields) {
        if (!isOverrideChannel(field, m_spec.fields)) continue;
        const bool oldAvailable = textureProvidesChannels(field, oldPath);
        const bool newAvailable = textureProvidesChannels(field, authoredPath);
        const bool currentOverride = m_state.values.contains(field.key)
            ? m_state.values.value(field.key).toBool(field.defaultValue.toBool())
            : field.defaultValue.toBool();
        const bool manualOverride = currentOverride && oldAvailable;
        m_state.values.insert(field.key, !newAvailable || manualOverride);
    }
    if (auto* height = qobject_cast<QCheckBox*>(m_fieldEditors.value(QStringLiteral("heightInBlue")))) {
        const TextureImage::Diagnostics info = TextureImage::inspect(authoredPath, m_textureRoot);
        if (!authoredPath.isEmpty() && (!info.valid || !info.channels.contains(QChar('B')))) {
            m_state.values.insert(QStringLiteral("heightInBlue"), false);
            const QSignalBlocker blocker(height);
            height->setChecked(false);
        }
    }
    m_state.path = authoredPath.trimmed();
    {
        const QSignalBlocker blocker(m_path);
        m_path->setText(m_state.path);
    }
    updateChannelControls();
    emitStateChanged();
}

void TextureSlotWidget::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls() && !event->mimeData()->urls().isEmpty()
        && event->mimeData()->urls().first().isLocalFile()) {
        const QString suffix = QFileInfo(event->mimeData()->urls().first().toLocalFile()).suffix().toLower();
        if (QStringList{QStringLiteral("dds"), QStringLiteral("tga"), QStringLiteral("png"), QStringLiteral("jpg"),
                QStringLiteral("jpeg"), QStringLiteral("bmp")}.contains(suffix)) {
            event->acceptProposedAction();
            return;
        }
    }
    event->ignore();
}

void TextureSlotWidget::dropEvent(QDropEvent* event)
{
    if (!event->mimeData()->hasUrls() || event->mimeData()->urls().isEmpty()) return;
    assignTextureFile(event->mimeData()->urls().first().toLocalFile());
    event->acceptProposedAction();
}

void TextureSlotWidget::emitStateChanged()
{
    updateThumbnail();
    emit stateChanged(m_uvSet, m_spec.key, m_state);
}

QWidget* TextureSlotWidget::createFieldEditor(const FieldSpec& field, const QJsonValue& value)
{
    switch (field.type) {
    case FieldType::Number: {
        auto* editor = new DragNumericEdit(m_body);
        editor->setRange(field.minimum, field.maximum);
        editor->setSingleStep(field.step);
        editor->setDragSensitivity(field.step);
        editor->setDecimals(field.decimals);
        editor->setSuffix(field.suffix);
        editor->setValue(value.toDouble(field.defaultValue.toDouble()));
        connect(editor, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, key = field.key](double number) {
            m_state.values.insert(key, number);
            emitStateChanged();
        });
        return editor;
    }
    case FieldType::Toggle: {
        auto* editor = new QCheckBox(m_body);
        editor->setChecked(value.toBool(field.defaultValue.toBool()));
        connect(editor, &QCheckBox::toggled, this, [this, key = field.key](bool checked) {
            m_state.values.insert(key, checked);
            updateChannelControls();
            emitStateChanged();
        });
        return editor;
    }
    case FieldType::Choice: {
        auto* editor = new QComboBox(m_body);
        WheelFocusGuard::install(editor);
        editor->addItems(field.choices);
        const int index = qMax(0, editor->findText(value.toString(field.defaultValue.toString())));
        editor->setCurrentIndex(index);
        connect(editor, &QComboBox::currentTextChanged, this, [this, key = field.key](const QString& text) {
            m_state.values.insert(key, text);
            emitStateChanged();
        });
        return editor;
    }
    case FieldType::Color: {
        auto* editor = new ColorEdit(QColor(value.toString(field.defaultValue.toString())), field.colorAlpha, m_body);
        connect(editor, &ColorEdit::colorChanged, this, [this, key = field.key, alpha = field.colorAlpha](const QColor& color) {
            m_state.values.insert(key, color.name(alpha ? QColor::HexArgb : QColor::HexRgb).toUpper());
            emitStateChanged();
        });
        return editor;
    }
    case FieldType::Text:
    default: {
        // Commits on editing-finished like the texture path field: one state
        // change per edit instead of one per keystroke.
        auto* editor = new QLineEdit(value.toString(field.defaultValue.toString()), m_body);
        connect(editor, &QLineEdit::editingFinished, this, [this, editor, key = field.key] {
            const QString text = editor->text();
            if (m_state.values.value(key).toString() == text) return;
            m_state.values.insert(key, text);
            emitStateChanged();
        });
        return editor;
    }
    }
}

void TextureSlotWidget::showChannelSourcesDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Channel Sources — %1").arg(m_spec.name));
    auto* outer = new QVBoxLayout(&dialog);
    auto* intro = new QLabel(tr("Redirect single channels of this packed input to another texture file. The thumbnail and preview show the composited result; export a packed texture before runtime use."), &dialog);
    intro->setWordWrap(true);
    outer->addWidget(intro);

    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(5);
    struct SwapRow { QComboBox* mode; QLineEdit* path; QComboBox* channel; QCheckBox* invert; };
    QVector<QPair<QString, SwapRow>> rows;
    static const char* sourceChannelNames[5] = {"R", "G", "B", "A", "Luminance"};
    int rowIndex = 0;
    for (const QString& channelName : m_spec.channels) {
        SwapRow row;
        auto* label = new QLabel(channelName, &dialog);
        label->setObjectName(QStringLiteral("SectionTitle"));
        row.mode = new QComboBox(&dialog);
        row.mode->addItems({tr("Main texture"), tr("Custom file")});
        row.path = new QLineEdit(&dialog);
        row.path->setPlaceholderText(tr("Texture supplying this channel"));
        auto* browse = new QPushButton(tr("…"), &dialog);
        browse->setFixedWidth(30);
        row.channel = new QComboBox(&dialog);
        row.channel->addItems({QStringLiteral("R"), QStringLiteral("G"), QStringLiteral("B"), QStringLiteral("A"), tr("Luminance")});
        row.channel->setToolTip(tr("Which channel of the source file feeds this channel."));
        row.invert = new QCheckBox(tr("Invert"), &dialog);
        row.invert->setToolTip(tr("Inverts the sampled channel — converts gloss maps to roughness."));

        const QJsonObject swap = m_state.values.value(QStringLiteral("swap") + channelName).toObject();
        const bool custom = !swap.value(QStringLiteral("path")).toString().trimmed().isEmpty();
        row.mode->setCurrentIndex(custom ? 1 : 0);
        row.path->setText(swap.value(QStringLiteral("path")).toString());
        const QString sourceChannel = swap.value(QStringLiteral("channel")).toString(QStringLiteral("R"));
        int channelIndex = 0;
        for (int i = 0; i < 5; ++i)
            if (sourceChannel == QLatin1String(sourceChannelNames[i])) channelIndex = i;
        row.channel->setCurrentIndex(channelIndex);
        row.invert->setChecked(swap.value(QStringLiteral("invert")).toBool(false));

        const auto updateRowEnabled = [row] {
            const bool on = row.mode->currentIndex() == 1;
            row.path->setEnabled(on);
            row.channel->setEnabled(on);
            row.invert->setEnabled(on);
        };
        updateRowEnabled();
        connect(row.mode, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, [updateRowEnabled](int) { updateRowEnabled(); });
        connect(browse, &QPushButton::clicked, &dialog, [this, &dialog, pathEdit = row.path, modeCombo = row.mode] {
            const QString file = QFileDialog::getOpenFileName(&dialog, tr("Select channel source texture"), QString(),
                tr("Textures (*.dds *.tga *.png *.jpg *.jpeg *.bmp);;All files (*.*)"));
            if (file.isEmpty()) return;
            pathEdit->setText(TextureImage::authoredPathForFile(file, m_textureRoot));
            modeCombo->setCurrentIndex(1);
        });

        grid->addWidget(label, rowIndex, 0);
        grid->addWidget(row.mode, rowIndex, 1);
        grid->addWidget(row.path, rowIndex, 2);
        grid->addWidget(browse, rowIndex, 3);
        grid->addWidget(row.channel, rowIndex, 4);
        grid->addWidget(row.invert, rowIndex, 5);
        rows.append({channelName, row});
        ++rowIndex;
    }
    grid->setColumnStretch(2, 1);
    outer->addLayout(grid);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    outer->addWidget(buttons);
    dialog.resize(760, dialog.sizeHint().height());
    if (dialog.exec() != QDialog::Accepted) return;

    for (const auto& entry : rows) {
        const SwapRow& row = entry.second;
        const QString key = QStringLiteral("swap") + entry.first;
        const QString path = row.path->text().trimmed();
        if (row.mode->currentIndex() == 0 || path.isEmpty()) {
            m_state.values.remove(key);
            continue;
        }
        QJsonObject swap;
        swap.insert(QStringLiteral("path"), path);
        swap.insert(QStringLiteral("channel"), QLatin1String(sourceChannelNames[row.channel->currentIndex()]));
        if (row.invert->isChecked()) swap.insert(QStringLiteral("invert"), true);
        m_state.values.insert(key, swap);
    }
    emitStateChanged();
    updateThumbnail();
}

void TextureSlotWidget::updateSwapSummary()
{
    if (!m_swapSummary) return;
    QStringList parts;
    for (const QString& channelName : m_spec.channels) {
        const QJsonObject swap = m_state.values.value(QStringLiteral("swap") + channelName).toObject();
        const QString path = swap.value(QStringLiteral("path")).toString().trimmed();
        if (path.isEmpty()) continue;
        parts.append(channelName + QStringLiteral(" ← ") + QFileInfo(path).fileName()
            + QStringLiteral(" (") + swap.value(QStringLiteral("channel")).toString(QStringLiteral("R"))
            + (swap.value(QStringLiteral("invert")).toBool(false) ? tr(", inverted") : QString()) + QLatin1Char(')'));
    }
    if (parts.isEmpty()) {
        m_swapSummary->clear();
        m_swapSummary->hide();
    } else {
        m_swapSummary->setText(tr("Swapped channels: ") + parts.join(QStringLiteral("  ·  ")));
        m_swapSummary->show();
    }
}

void TextureSlotWidget::exportPackedTexture()
{
    const QImage composite = TextureCache::packedImage(m_state, m_textureRoot);
    if (composite.isNull()) {
        QMessageBox::information(this, tr("Export Packed Texture"),
            tr("Assign a main texture or at least one channel source first."));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Export Packed Texture — %1").arg(m_spec.name));
    auto* outer = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout();
    auto* formatCombo = new QComboBox(&dialog);
    formatCombo->addItem(tr("BC7 (sRGB color)"), int(TextureWriter::DdsFormat::Bc7Srgb));
    formatCombo->addItem(tr("BC7 (linear packed data)"), int(TextureWriter::DdsFormat::Bc7));
    formatCombo->addItem(tr("BC5 (two-channel normal)"), int(TextureWriter::DdsFormat::Bc5));
    formatCombo->addItem(tr("BC4 (single channel)"), int(TextureWriter::DdsFormat::Bc4));
    formatCombo->addItem(tr("BC1 (low-cost color)"), int(TextureWriter::DdsFormat::Bc1));
    formatCombo->addItem(tr("Uncompressed RGBA8"), int(TextureWriter::DdsFormat::Rgba8));
    formatCombo->addItem(tr("PNG (authoring interchange)"), -1);
    // Suggest by slot semantics: sRGB slots -> BC7 sRGB, RG-normal slots -> BC5,
    // single-channel slots -> BC4, everything else packed linear BC7.
    const bool normalMapSlot = m_spec.format.contains(QStringLiteral("RG normal"))
        || m_spec.format.contains(QStringLiteral("RG bent"))
        || m_spec.format.contains(QStringLiteral("RG tangent"))
        || m_spec.format.contains(QStringLiteral("RG coat"))
        || m_spec.format.contains(QStringLiteral("RG detail"))
        || m_spec.format.contains(QStringLiteral("RG per-frame"));
    if (m_spec.format.startsWith(QStringLiteral("sRGB"))) formatCombo->setCurrentIndex(0);
    else if (m_spec.channels.size() == 1) formatCombo->setCurrentIndex(3);
    else if (normalMapSlot) formatCombo->setCurrentIndex(2);
    else formatCombo->setCurrentIndex(1);
    auto* mipsCheck = new QCheckBox(tr("Generate full mip chain"), &dialog);
    mipsCheck->setChecked(true);
    form->addRow(tr("Format"), formatCombo);
    form->addRow(QString(), mipsCheck);
    // RMAOS exports can fold the paired normal map's per-mip variance into the
    // roughness chain (Toksvig), so minified speculars stop aliasing where the
    // normal detail mips away. Offline and physically correct — the runtime
    // keeps only its screen-space specular AA.
    QLineEdit* toksvigEdit = nullptr;
    if (m_spec.key.contains(QStringLiteral("Rmaos"), Qt::CaseInsensitive)) {
        auto* toksvigRow = new QWidget(&dialog);
        auto* toksvigLayout = new QHBoxLayout(toksvigRow);
        toksvigLayout->setContentsMargins(0, 0, 0, 0);
        toksvigEdit = new QLineEdit(toksvigRow);
        toksvigEdit->setPlaceholderText(tr("Optional: paired normal map for roughness-mip variance"));
        auto* toksvigBrowse = new QPushButton(tr("Browse…"), toksvigRow);
        toksvigLayout->addWidget(toksvigEdit, 1);
        toksvigLayout->addWidget(toksvigBrowse);
        connect(toksvigBrowse, &QPushButton::clicked, &dialog, [&dialog, toksvigEdit] {
            const QString file = QFileDialog::getOpenFileName(&dialog, tr("Select paired normal map"), QString(),
                tr("Textures (*.dds *.png *.tga *.jpg);;All files (*.*)"));
            if (!file.isEmpty()) toksvigEdit->setText(file);
        });
        form->addRow(tr("Toksvig normal"), toksvigRow);
    }
    outer->addLayout(form);
    auto* note = new QLabel(tr("The baked file becomes this input's texture path and the channel sources are cleared."), &dialog);
    note->setWordWrap(true);
    note->setObjectName(QStringLiteral("Muted"));
    outer->addWidget(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    outer->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) return;

    const bool png = formatCombo->currentData().toInt() < 0;
    QString suggested;
    const QString resolvedMain = TextureImage::resolvePath(m_state.path, m_textureRoot);
    if (!resolvedMain.isEmpty() && QFileInfo(resolvedMain).isFile())
        suggested = QFileInfo(resolvedMain).absolutePath() + QLatin1Char('/')
            + QFileInfo(resolvedMain).completeBaseName() + QStringLiteral("_packed");
    else
        suggested = QDir::homePath() + QLatin1Char('/') + m_spec.key;
    suggested += png ? QStringLiteral(".png") : QStringLiteral(".dds");
    const QString filter = png ? tr("PNG image (*.png)") : tr("DDS texture (*.dds)");
    QString fileName = QFileDialog::getSaveFileName(this, tr("Export packed texture"), suggested, filter);
    if (fileName.isEmpty()) return;

    QString error;
    bool saved = false;
    if (png) {
        saved = composite.save(fileName, "PNG");
        if (!saved) error = tr("The PNG could not be written.");
    } else {
        if (!fileName.endsWith(QStringLiteral(".dds"), Qt::CaseInsensitive)) fileName += QStringLiteral(".dds");
        QImage toksvigNormal;
        if (toksvigEdit && !toksvigEdit->text().trimmed().isEmpty()) {
            QString toksvigError;
            toksvigNormal = TextureImage::load(toksvigEdit->text().trimmed(), m_textureRoot, &toksvigError);
            if (toksvigNormal.isNull()) {
                QMessageBox::critical(this, tr("Export Packed Texture"),
                    tr("The Toksvig normal map could not be loaded: %1").arg(toksvigError));
                return;
            }
        }
        saved = TextureWriter::saveDds(composite, fileName,
            TextureWriter::DdsFormat(formatCombo->currentData().toInt()), mipsCheck->isChecked(), &error,
            toksvigNormal, normalMapSlot);
    }
    if (!saved) {
        QMessageBox::critical(this, tr("Export Packed Texture"), error);
        return;
    }

    for (const QString& channelName : {QStringLiteral("R"), QStringLiteral("G"), QStringLiteral("B"), QStringLiteral("A")})
        m_state.values.remove(QStringLiteral("swap") + channelName);
    assignTextureFile(fileName);
}
