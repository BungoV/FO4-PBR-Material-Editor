#pragma once

// Shared helpers for the MainWindow implementation, which is split across
// mainwindow.cpp (window/document logic), mainwindow_pages.cpp (page
// builders), and mainwindow_slots.cpp (texture-slot spec tables).

#include "colorpicker.h"
#include "textureslotwidget.h"

#include <QAction>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QJsonValue>
#include <QKeySequence>
#include <QLineEdit>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

inline FieldSpec numberField(const QString& key, const QString& label, double value,
    double minimum = 0.0, double maximum = 1.0, double step = 0.01,
    const QString& tooltip = {}, int decimals = 4, const QString& suffix = {})
{
    FieldSpec field;
    field.key = key; field.label = label; field.type = FieldType::Number;
    field.defaultValue = value; field.minimum = minimum; field.maximum = maximum;
    field.step = step; field.tooltip = tooltip; field.decimals = decimals; field.suffix = suffix;
    return field;
}

inline FieldSpec toggleField(const QString& key, const QString& label, bool value, const QString& tooltip)
{
    FieldSpec field;
    field.key = key; field.label = label; field.type = FieldType::Toggle;
    field.defaultValue = value; field.tooltip = tooltip;
    return field;
}

inline FieldSpec choiceField(const QString& key, const QString& label, const QString& value,
    const QStringList& choices, const QString& tooltip)
{
    FieldSpec field;
    field.key = key; field.label = label; field.type = FieldType::Choice;
    field.defaultValue = value; field.choices = choices; field.tooltip = tooltip;
    return field;
}

inline FieldSpec enabledWhen(FieldSpec field, const QString& key, const QStringList& values)
{
    field.enabledByKey = key; field.enabledByValues = values;
    return field;
}

inline FieldSpec textField(const QString& key, const QString& label, const QString& value, const QString& tooltip)
{
    FieldSpec field;
    field.key = key; field.label = label; field.type = FieldType::Text;
    field.defaultValue = value; field.tooltip = tooltip;
    return field;
}

inline FieldSpec colorField(const QString& key, const QString& label, const QString& value,
    const QString& tooltip, bool alpha = false)
{
    FieldSpec field;
    field.key = key; field.label = label; field.type = FieldType::Color;
    field.defaultValue = value; field.tooltip = tooltip; field.colorAlpha = alpha;
    return field;
}

inline TextureSlotSpec slot(const QString& key, const QString& name, const QString& abbreviation,
    const QString& format, const QColor& color, bool enabled, const QString& path,
    const QString& tooltip, const QVector<FieldSpec>& fields = {},
    const QStringList& channels = {QStringLiteral("R"), QStringLiteral("G"), QStringLiteral("B"), QStringLiteral("A")})
{
    TextureSlotSpec spec;
    spec.key = key; spec.name = name; spec.abbreviation = abbreviation; spec.format = format;
    spec.color = color; spec.defaultEnabled = enabled; spec.defaultExpanded = enabled;
    spec.defaultPath = path; spec.tooltip = tooltip; spec.fields = fields; spec.channels = channels;
    return spec;
}

inline QVector<FieldSpec> coordinateFields(int layer)
{
    const bool overlay = layer > 0;
    QVector<FieldSpec> fields;
    if (overlay)
        fields.append(toggleField("mirrorPrimary", "Mirror Primary UV layout", false, "Uses UV0 coordinates while retaining independent overlay transforms and animation."));
    // FO4 meshes carry two texture-coordinate channels, so the tertiary layer
    // chooses either mesh channel rather than introducing a third.
    const QStringList sources = layer == 0 ? QStringList{"Mesh UV0", "World Planar", "Object Planar"}
        : layer == 1 ? QStringList{"Mesh UV1", "World Planar", "Object Planar"}
                     : QStringList{"Mesh UV1", "Mesh UV0", "World Planar", "Object Planar"};
    fields.append(choiceField("source", "Coordinates", layer == 0 ? "Mesh UV0" : "Mesh UV1", sources,
            "Selects the base coordinate source used by every texture in this set."));
    fields.append(choiceField("projectionAxis", "Planar projection", "Triplanar",
            {"Triplanar", "X axis", "Y axis", "Z axis"},
            "Axis used by World or Object Planar coordinates. Triplanar blends all three projections."));
    fields.append(numberField("triplanarBlend", "Triplanar blend", 4.0, 1.0, 32.0, 0.1,
            "Sharpness of the normal-weighted transition between triplanar projections."));
    fields += QVector<FieldSpec>{
        numberField("scaleU", "Scale U", 1.0, -1000.0, 1000.0, 0.01, "Horizontal texture-coordinate scale."),
        numberField("scaleV", "Scale V", 1.0, -1000.0, 1000.0, 0.01, "Vertical texture-coordinate scale."),
        numberField("offsetU", "Offset U", 0.0, -1000.0, 1000.0, 0.01, "Horizontal texture-coordinate offset."),
        numberField("offsetV", "Offset V", 0.0, -1000.0, 1000.0, 0.01, "Vertical texture-coordinate offset."),
        numberField("rotation", "Rotation", 0.0, -360.0, 360.0, 1.0, "Rotation around the UV origin.", 2, "°"),
        choiceField("address", "Address U / V", "Wrap / Wrap", {"Wrap / Wrap", "Clamp / Clamp", "Mirror / Mirror"}, "Controls sampling outside the zero-to-one UV range.")
    };
    return fields;
}

inline QStringList materialTemplateNames()
{
    return {QStringLiteral("Standard Metal"), QStringLiteral("Painted Metal"), QStringLiteral("Plastic"),
        QStringLiteral("Glass"), QStringLiteral("Skin"), QStringLiteral("Vegetation"),
        QStringLiteral("Hair"), QStringLiteral("Water"), QStringLiteral("Physically Lit Effect"),
        QStringLiteral("Landscape")};
}

inline void installDefaultReset(QWidget* editor, const QJsonValue& defaultValue)
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

inline void setSettingEditorsEnabled(QWidget* root, const QString& section, const QStringList& keys, bool enabled)
{
    for (QWidget* editor : root->findChildren<QWidget*>()) {
        if (editor->property("settingSection").toString() == section
            && keys.contains(editor->property("settingKey").toString()))
            editor->setEnabled(enabled);
    }
}

inline QString companionBaseName(const QString& fileName)
{
    QString base = QFileInfo(fileName).completeBaseName();
    const QStringList suffixes{
        QStringLiteral("_basecolor"), QStringLiteral("_diffuse"), QStringLiteral("_emissive"),
        QStringLiteral("_normal"), QStringLiteral("_rmaos"), QStringLiteral("_base"),
        QStringLiteral("_n"), QStringLiteral("_d"), QStringLiteral("_e")
    };
    for (const QString& suffix : suffixes) {
        if (base.endsWith(suffix, Qt::CaseInsensitive)) {
            base.chop(suffix.size());
            break;
        }
    }
    return base;
}

inline QStringList companionSuffixes(const QString& key)
{
    if (key.contains(QStringLiteral("BaseColor"), Qt::CaseInsensitive))
        return {QStringLiteral("_basecolor"), QStringLiteral("_base"), QStringLiteral("_d"), QStringLiteral("_diffuse")};
    if (key.contains(QStringLiteral("Normal"), Qt::CaseInsensitive))
        return {QStringLiteral("_n"), QStringLiteral("_normal")};
    if (key.contains(QStringLiteral("Rmaos"), Qt::CaseInsensitive))
        return {QStringLiteral("_rmaos")};
    if (key.contains(QStringLiteral("Emissive"), Qt::CaseInsensitive))
        return {QStringLiteral("_e"), QStringLiteral("_emissive")};
    return {};
}
