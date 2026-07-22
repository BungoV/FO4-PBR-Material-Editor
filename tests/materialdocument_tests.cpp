#include "bgsmreader.h"
#include "converter.h"
#include "convertercore.h"
#include "normalderive.h"
#include "geometryao.h"
#include "maskpaint.h"
#include "materialdocument.h"
#include "disclosureicon.h"
#include "texturearchive.h"
#include "texturecache.h"
#include "textureimage.h"
#include "texturewriter.h"
#include "texturebrowserdialog.h"
#include "textureslotwidget.h"
#include "previewmesh.h"
#include "colorpicker.h"
#include "mainwindow.h"
#include "materialpreviewwidget.h"
#define TINYEXR_USE_MINIZ 0
#include "tinyexr.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDataStream>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QGroupBox>
#include <QTemporaryDir>
#include <QTextStream>
#include <QDoubleSpinBox>
#include <QToolButton>
#include <QTreeWidget>
#include <QTabWidget>
#include <QUndoStack>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <zlib.h>

namespace {

int failures = 0;

void check(bool condition, const QString& message)
{
    QTextStream out(condition ? stdout : stderr);
    out << (condition ? "PASS: " : "FAIL: ") << message << Qt::endl;
    if (!condition) ++failures;
}

QString fixturePath(const QString& name)
{
    const QStringList candidates = {
        QDir::current().filePath(QStringLiteral("fixtures/") + name),
        QDir::current().filePath(QStringLiteral("tests/fixtures/") + name),
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../tests/fixtures/") + name)
    };
    for (const QString& candidate : candidates)
        if (QFileInfo::exists(candidate)) return QDir::cleanPath(candidate);
    return {};
}

QJsonObject readFixture(const QString& name)
{
    QFile file(fixturePath(name));
    if (!file.open(QIODevice::ReadOnly)) return {};
    QJsonParseError error;
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll(), &error);
    return error.error == QJsonParseError::NoError && json.isObject() ? json.object() : QJsonObject();
}

QByteArray envelopeFor(const QJsonObject& object)
{
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    QByteArray envelope;
    QDataStream stream(&envelope, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << Pbr::MaterialDocument::Magic << Pbr::MaterialDocument::Version << static_cast<quint32>(payload.size());
    envelope.append(payload);
    return envelope;
}

QByteArray payloadFromEnvelope(const QByteArray& envelope)
{
    QByteArray copy = envelope;
    QDataStream stream(&copy, QIODevice::ReadOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    quint32 magic = 0;
    quint32 version = 0;
    quint32 size = 0;
    stream >> magic >> version >> size;
    if (magic != Pbr::MaterialDocument::Magic || version != Pbr::MaterialDocument::Version
        || size != static_cast<quint32>(copy.size() - 12)) return {};
    return copy.mid(12, size);
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("FO4CommunityShadersTests"));
    QCoreApplication::setApplicationName(QStringLiteral("PBRMaterialEditorTests"));

    {
        QTemporaryDir textures;
        const QString folder = textures.filePath(QStringLiteral("Textures/Actors/PowerArmor"));
        QDir().mkpath(folder);
        QFile texture(QDir(folder).filePath(QStringLiteral("X01_Torso_BC.dds")));
        if (texture.open(QIODevice::WriteOnly)) {
            texture.write("DDS ");
            texture.close();
        }

        TextureBrowserDialog browser(textures.path());
        auto* tree = browser.findChild<QTreeWidget*>(QStringLiteral("TextureBrowserTree"));
        QTreeWidgetItem* actors = nullptr;
        if (tree) {
            for (int i = 0; i < tree->topLevelItemCount(); ++i)
                if (tree->topLevelItem(i)->text(0) == QStringLiteral("Actors")) actors = tree->topLevelItem(i);
        }
        const auto childNamed = [](QTreeWidgetItem* parent, const QString& name) {
            if (!parent) return static_cast<QTreeWidgetItem*>(nullptr);
            for (int i = 0; i < parent->childCount(); ++i)
                if (parent->child(i)->text(0) == name) return parent->child(i);
            return static_cast<QTreeWidgetItem*>(nullptr);
        };
        QTreeWidgetItem* powerArmor = childNamed(actors, QStringLiteral("PowerArmor"));
        QTreeWidgetItem* leaf = childNamed(powerArmor, QStringLiteral("X01_Torso_BC.dds"));
        check(tree && tree->rootIsDecorated() && actors && powerArmor && leaf,
            QStringLiteral("texture browser builds collapsible authored folder nodes"));
        check(leaf && leaf->text(0) == QStringLiteral("X01_Torso_BC.dds")
                && leaf->text(2) == QStringLiteral("Loose"),
            QStringLiteral("texture browser keeps source metadata on texture leaves"));

        const QString setFolder = textures.filePath(QStringLiteral("Textures/Sets"));
        QDir().mkpath(setFolder);
        QFile setTexture(QDir(setFolder).filePath(QStringLiteral("demo_base.dds")));
        if (setTexture.open(QIODevice::WriteOnly)) {
            setTexture.write("DDS ");
            setTexture.close();
        }

        TextureSlotSpec spec;
        spec.key = QStringLiteral("primaryBaseColor");
        spec.name = QStringLiteral("Base Color");
        spec.abbreviation = QStringLiteral("BC");
        spec.format = QStringLiteral("sRGB RGBA");
        spec.color = QColor(QStringLiteral("#5577AA"));
        Pbr::TextureSlotState state;
        state.enabled = true;
        state.expanded = true;
        TextureSlotWidget slot(QStringLiteral("UV0"), spec, state, textures.path());
        auto* path = slot.findChild<QLineEdit*>(QStringLiteral("TexturePath"));
        auto* findSet = slot.findChild<QPushButton*>(QStringLiteral("FindTextureSetButton"));
        int requests = 0;
        QObject::connect(&slot, &TextureSlotWidget::companionTexturesRequested,
            [&requests](const QString&, const QString&, const QString&) { ++requests; });
        if (path) {
            path->setText(QStringLiteral("Sets\\demo_base.dds"));
            QMetaObject::invokeMethod(path, "editingFinished", Qt::DirectConnection);
        }
        check(requests == 0 && findSet && findSet->isEnabled(),
            QStringLiteral("selecting a loose texture no longer prompts or auto-requests companions"));
        if (findSet) findSet->click();
        check(requests == 1,
            QStringLiteral("Find Set explicitly requests companion assignment"));
    }

    const QStringList names = Pbr::shaderModeNames();
    check(names.size() == 12, QStringLiteral("all twelve shader families are registered"));
    check(names == QStringList({QStringLiteral("Standard"), QStringLiteral("Advanced Subsurface"), QStringLiteral("Vegetation"),
              QStringLiteral("Hair"), QStringLiteral("Eye"), QStringLiteral("Landscape"),
              QStringLiteral("Glass"), QStringLiteral("Water"), QStringLiteral("Unlit Effect"),
              QStringLiteral("Simple Lit Effect"), QStringLiteral("Physically Lit Effect"), QStringLiteral("Advanced")}),
        QStringLiteral("shader families use concise names and keep Advanced last"));
    QSet<QString> uniqueNames;
    for (const QString& name : names) uniqueNames.insert(name);
    check(uniqueNames.size() == names.size(), QStringLiteral("shader names are unique"));
    for (const QString& name : names)
        check(Pbr::shaderModeName(Pbr::shaderModeFromName(name)) == name, QStringLiteral("shader round-trip: %1").arg(name));
    check(Pbr::shaderModeFromName(QStringLiteral("Standard Surface")) == Pbr::ShaderMode::StandardSurface,
        QStringLiteral("legacy verbose shader names remain readable"));
    check(Pbr::shaderModeFromName(QStringLiteral("All Features")) == Pbr::ShaderMode::AllFeatures,
        QStringLiteral("legacy All Features shader name remains readable"));
    check(Pbr::shaderModeName(Pbr::ShaderMode::AllFeatures) == QStringLiteral("Advanced"),
        QStringLiteral("authoring superset serializes as Advanced"));

    {
        MainWindow window;
        bool primaryOnlyCount = false;
        for (QLabel* label : window.findChildren<QLabel*>())
            if (label->text().contains(QStringLiteral("Active texture inputs: 3"))) { primaryOnlyCount = true; break; }
        check(primaryOnlyCount, QStringLiteral("validation excludes default Secondary UV sockets while the layer is disabled"));
        QAction* glassTemplate = nullptr;
        for (QAction* action : window.findChildren<QAction*>())
            if (action->text() == QStringLiteral("Glass")) { glassTemplate = action; break; }
        check(glassTemplate != nullptr, QStringLiteral("Glass template action is registered"));
        if (glassTemplate) glassTemplate->trigger();
        check(window.document().shaderMode() == Pbr::ShaderMode::GlassSurface,
            QStringLiteral("Glass template selects the Glass shader"));
        check(window.document().setting(QStringLiteral("Transparency/Composition"), QStringLiteral("mode")).toString() == QStringLiteral("Physical Transmission"),
            QStringLiteral("Glass template authors the live transparency composition key"));
        auto* undoStack = window.findChild<QUndoStack*>();
        check(window.document().modified() && undoStack && !undoStack->isClean(),
            QStringLiteral("new template remains dirty even at the undo baseline"));
        auto* previewShadows = window.findChild<QCheckBox*>(QStringLiteral("PreviewShadows"));
        auto* previewShadowIntensity = window.findChild<QDoubleSpinBox*>(QStringLiteral("PreviewShadowIntensity"));
        check(previewShadows && previewShadowIntensity,
            QStringLiteral("preview exposes shadow toggle and intensity controls"));
        if (previewShadows && previewShadowIntensity) {
            previewShadows->setChecked(false);
            check(!previewShadowIntensity->isEnabled(),
                QStringLiteral("disabling preview shadows disables their intensity field"));
            previewShadows->setChecked(true);
        }
        auto* previewSettings = window.findChild<QGroupBox*>(QStringLiteral("PreviewSettings"));
        check(previewSettings && !previewSettings->isCheckable(),
            QStringLiteral("preview settings use a plain section heading instead of a checkbox-like disclosure"));
        auto* previewTabs = window.findChild<QTabWidget*>(QStringLiteral("PreviewSettingsTabs"));
        check(previewTabs && previewTabs->count() == 4
                && previewTabs->tabText(0) == QStringLiteral("Scene")
                && previewTabs->tabText(1) == QStringLiteral("Lighting")
                && previewTabs->tabText(2) == QStringLiteral("Display")
                && previewTabs->tabText(3) == QStringLiteral("States"),
            QStringLiteral("preview controls are organized into task-based Scene, Lighting, Display, and States tabs"));
        auto* previewModel = window.findChild<QComboBox*>(QStringLiteral("PreviewModel"));
        check(previewModel && previewModel->itemText(3) == QStringLiteral("Imported Mesh"),
            QStringLiteral("preview geometry uses an explicit imported-mesh name"));
        auto* previewLayers = window.findChild<QComboBox*>(QStringLiteral("PreviewMaterialLayers"));
        check(previewLayers && previewLayers->itemText(0) == QStringLiteral("Primary Material (UV0)")
                && previewLayers->itemText(1) == QStringLiteral("Secondary Material (UV1)")
                && previewLayers->itemText(2) == QStringLiteral("Tertiary Material (UV2)")
                && previewLayers->itemText(3) == QStringLiteral("Combined Material (All Layers)"),
            QStringLiteral("preview material layers state their UV mapping consistently"));
        auto* deriveExrLighting = window.findChild<QCheckBox*>(QStringLiteral("DeriveExrLighting"));
        check(deriveExrLighting && deriveExrLighting->text().contains(QStringLiteral("EXR")),
            QStringLiteral("preview exposes explicit EXR-derived environment-shadow control"));
        auto* renderMode = window.findChild<QComboBox*>(QStringLiteral("PreviewRenderMode"));
        QStringList renderModes;
        if(renderMode)for(int index=0;index<renderMode->count();++index)renderModes.append(renderMode->itemText(index));
        check(renderModes.contains(QStringLiteral("Normal Map (Tangent Space)"))
                && renderModes.contains(QStringLiteral("Roughness + Metallic (R/G)"))
                && renderModes.contains(QStringLiteral("RMAOS (Packed RGB)")),
            QStringLiteral("render-mode inspector exposes resolved normal and combined packed-channel views"));
        check(renderModes.contains(QStringLiteral("Opacity"))&&renderModes.contains(QStringLiteral("Height"))
                && renderModes.contains(QStringLiteral("Emissive Mask"))&&renderModes.contains(QStringLiteral("Layer Coverage")),
            QStringLiteral("render-mode inspector exposes the remaining resolved material channels"));
    }

    {
        PbrPreviewGL preview;
        preview.setLighting(8.0f,Qt::white,30.0f,25.0f,1.0f,0.0f,0.0f);
        check(qAbs(preview.effectiveKeyIntensity()-8.0f)<.001f,
            QStringLiteral("manual preview lighting retains its analytic key light"));
        preview.setDerivedEnvironmentShadow(true,.8f);
        check(preview.effectiveKeyIntensity()==0.0f,
            QStringLiteral("EXR-derived shadow mode cannot double-count an analytic key light"));
    }

    QTemporaryDir directory;
    check(directory.isValid(), QStringLiteral("temporary directory is available"));
    const QString channelTexture = directory.filePath(QStringLiteral("rgba-channels.png"));
    QImage channelImage(2, 2, QImage::Format_RGBA8888);
    channelImage.fill(QColor(128, 64, 32, 255));
    check(channelImage.save(channelTexture), QStringLiteral("RGBA channel fixture can be created"));

    TextureSlotSpec channelSpec;
    channelSpec.key = QStringLiteral("primaryRmaos");
    channelSpec.name = QStringLiteral("RMAOS");
    channelSpec.defaultEnabled = true;
    channelSpec.defaultExpanded = true;
    FieldSpec sourceField;
    sourceField.key = QStringLiteral("overrideRoughness");
    sourceField.label = QStringLiteral("R • Roughness");
    sourceField.type = FieldType::Toggle;
    sourceField.defaultValue = true;
    FieldSpec fallbackField;
    fallbackField.key = QStringLiteral("roughness");
    fallbackField.label = QStringLiteral("Roughness fallback");
    fallbackField.type = FieldType::Number;
    fallbackField.defaultValue = 0.5;
    fallbackField.minimum = 0.0;
    fallbackField.maximum = 1.0;
    channelSpec.fields = {sourceField, fallbackField};
    Pbr::TextureSlotState channelState;
    channelState.enabled = true;
    channelState.expanded = true;
    channelState.path = channelTexture;
    TextureSlotWidget channelWidget(QStringLiteral("UV0"), channelSpec, channelState);
    check(channelWidget.hasChannelSources(), QStringLiteral("texture inputs detect channels with constant fallbacks"));
    const auto fallbackEditors = channelWidget.findChildren<QDoubleSpinBox*>();
    check(fallbackEditors.size() == 1 && !fallbackEditors.first()->isEnabled(),
        QStringLiteral("constant override is disabled while its texture channel is available"));
    channelWidget.setAllChannelSources(true);
    check(fallbackEditors.first()->isEnabled(), QStringLiteral("fallback value is enabled for a constant override"));
    check(channelWidget.state().values.value(QStringLiteral("overrideRoughness")).toBool(false),
        QStringLiteral("per-texture master control persists the positive override state"));
    channelWidget.setAllChannelSources(false);
    check(!fallbackEditors.first()->isEnabled()
            && !channelWidget.state().values.value(QStringLiteral("overrideRoughness")).toBool(true),
        QStringLiteral("available texture channels can disable their constant overrides"));
    auto* channelPath = channelWidget.findChild<QLineEdit*>();
    channelWidget.setAllChannelSources(true);
    channelPath->setText(channelTexture);
    QMetaObject::invokeMethod(channelPath, "editingFinished", Qt::DirectConnection);
    check(channelWidget.state().values.value(QStringLiteral("overrideRoughness")).toBool(false),
        QStringLiteral("deliberate overrides survive replacement by another complete texture"));
    channelWidget.setAllChannelSources(false);
    channelPath->clear();
    QMetaObject::invokeMethod(channelPath, "editingFinished", Qt::DirectConnection);
    check(channelWidget.state().values.value(QStringLiteral("overrideRoughness")).toBool(false),
        QStringLiteral("clearing a texture forces its constant override on"));
    channelPath->setText(channelTexture);
    QMetaObject::invokeMethod(channelPath, "editingFinished", Qt::DirectConnection);
    check(!channelWidget.state().values.value(QStringLiteral("overrideRoughness")).toBool(true),
        QStringLiteral("loading a complete texture clears an automatically forced override"));

    check(channelWidget.findChild<QLabel*>(QStringLiteral("StateBadge")) == nullptr,
        QStringLiteral("texture cards no longer show a status dot"));
    auto* disclosure = channelWidget.findChild<QToolButton*>(QStringLiteral("DisclosureButton"));
    const auto iconImage = [](const QIcon& icon) { return icon.pixmap(14, 14).toImage(); };
    const QColor disclosureColor(223, 226, 230);
    const QImage expandedImage = disclosure ? iconImage(disclosure->icon()) : QImage();
    if (disclosure) disclosure->click();
    check(disclosure && disclosure->text().isEmpty() && !channelWidget.state().expanded
            && expandedImage == iconImage(disclosureArrowIcon(true, 14, disclosureColor))
            && iconImage(disclosure->icon()) == iconImage(disclosureArrowIcon(false, 14, disclosureColor)),
        QStringLiteral("folded texture cards use the painted right-facing disclosure icon"));

    TextureSlotSpec dependentSpec;
    dependentSpec.key = QStringLiteral("primaryNormal");
    dependentSpec.name = QStringLiteral("Normal + Height");
    dependentSpec.defaultEnabled = true;
    dependentSpec.defaultExpanded = true;
    FieldSpec overrideNormal = sourceField;
    overrideNormal.key = QStringLiteral("overrideNormal");
    overrideNormal.label = QStringLiteral("RG • Tangent normal");
    FieldSpec normalValue;
    normalValue.key = QStringLiteral("normal");
    normalValue.label = QStringLiteral("Normal override");
    normalValue.type = FieldType::Color;
    normalValue.defaultValue = QStringLiteral("#8080FF");
    FieldSpec overrideHeight = sourceField;
    overrideHeight.key = QStringLiteral("overrideHeight");
    overrideHeight.label = QStringLiteral("B • Height");
    FieldSpec heightValue = fallbackField;
    heightValue.key = QStringLiteral("height");
    heightValue.label = QStringLiteral("Height override");
    FieldSpec heightEnabled;
    heightEnabled.key = QStringLiteral("heightInBlue");
    heightEnabled.label = QStringLiteral("B contains height");
    heightEnabled.type = FieldType::Toggle;
    heightEnabled.defaultValue = false;
    dependentSpec.fields = {overrideNormal, normalValue, overrideHeight, heightValue, heightEnabled};
    TextureSlotWidget dependentWidget(QStringLiteral("UV0"), dependentSpec, Pbr::TextureSlotState{});
    check(dependentWidget.state().values.value(QStringLiteral("overrideNormal")).toBool(false)
            && !dependentWidget.state().values.value(QStringLiteral("overrideHeight")).toBool(true),
        QStringLiteral("normal constants are available while disabled height channels stay off"));

    QJsonObject legacyValues{{QStringLiteral("useRoughness"), true}, {QStringLiteral("useF0"), false},
        {QStringLiteral("useMask"), true}, {QStringLiteral("useR"), false}, {QStringLiteral("fallback"), 0.25}};
    QJsonObject legacySlot{{QStringLiteral("enabled"), true}, {QStringLiteral("expanded"), true},
        {QStringLiteral("path"), QStringLiteral("legacy_rmaos.dds")}, {QStringLiteral("values"), legacyValues}};
    QJsonObject legacyRoot{{QStringLiteral("schema"), QStringLiteral("FO4.PBRM.Material")},
        {QStringLiteral("schemaVersion"), 4}, {QStringLiteral("shader"), QStringLiteral("Standard")},
        {QStringLiteral("secondaryUvEnabled"), false},
        {QStringLiteral("primaryUv"), QJsonObject{{QStringLiteral("primaryRmaos"), legacySlot}}},
        {QStringLiteral("secondaryUv"), QJsonObject()}, {QStringLiteral("resources"), QJsonObject()},
        {QStringLiteral("settings"), QJsonObject()}};
    Pbr::MaterialDocument migratedOverrides;
    QString migrationError;
    check(migratedOverrides.fromJson(legacyRoot, &migrationError), QStringLiteral("legacy texture-source booleans migrate: %1").arg(migrationError));
    const QJsonObject migratedValues = migratedOverrides.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryRmaos")).values;
    check(!migratedValues.value(QStringLiteral("overrideRoughness")).toBool(true)
            && migratedValues.value(QStringLiteral("overrideF0")).toBool(false)
            && !migratedValues.value(QStringLiteral("overrideMask")).toBool(true)
            && migratedValues.value(QStringLiteral("overrideMaskR")).toBool(false)
            && qFuzzyCompare(migratedValues.value(QStringLiteral("height")).toDouble(), 0.25)
            && !migratedValues.contains(QStringLiteral("useRoughness"))
            && !migratedValues.contains(QStringLiteral("useF0"))
            && !migratedValues.contains(QStringLiteral("useMask"))
            && !migratedValues.contains(QStringLiteral("fallback")),
        QStringLiteral("legacy source booleans and height fallback become positive constant overrides"));

    ColorEdit colorEdit(QColor(QStringLiteral("#FFEEDD")), false);
    bool colorSignal = false;
    QObject::connect(&colorEdit, &ColorEdit::colorChanged, [&](const QColor&) { colorSignal = true; });
    auto* colorHex = colorEdit.findChild<QLineEdit*>();
    check(colorHex != nullptr, QStringLiteral("inline color editor exposes a real selectable hex field"));
    colorHex->setText(QStringLiteral("#123456"));
    QMetaObject::invokeMethod(colorHex, "editingFinished", Qt::DirectConnection);
    check(colorEdit.color() == QColor(QStringLiteral("#123456")) && colorSignal,
        QStringLiteral("typed or pasted hex values update the material color and propagate"));

    Pbr::MaterialDocument document;
    document.setShaderMode(Pbr::ShaderMode::AllFeatures);
    document.setSecondaryUvEnabled(true);
    document.setTertiaryUvEnabled(true);
    Pbr::TextureSlotState tertiaryBase;
    tertiaryBase.enabled = true;
    tertiaryBase.path = QStringLiteral("textures\\armor\\prototype_overlay.dds");
    tertiaryBase.values.insert(QStringLiteral("blend"), QStringLiteral("Masked Replace"));
    document.setTextureSlot(QStringLiteral("UV2"), QStringLiteral("tertiaryBaseColor"), tertiaryBase);
    document.setSetting(QStringLiteral("UV2"), QStringLiteral("source"), QStringLiteral("Mesh UV0"));
    document.setSetting(QStringLiteral("UV2/Composition"), QStringLiteral("edgeSoftness"), 0.25);
    Pbr::TextureSlotState stateResponse;
    stateResponse.enabled = true;
    stateResponse.path = QStringLiteral("textures\\armor\\prototype_state.dds");
    stateResponse.values.insert(QStringLiteral("porosity"), 0.8);
    document.setTextureSlot(QStringLiteral("UV0"), QStringLiteral("primarySurfaceStateResponse"), stateResponse);
    document.setSetting(QStringLiteral("SurfaceStates/Wetness"), QStringLiteral("wetRoughness"), 0.06);
    Pbr::TextureSlotState normal;
    normal.enabled = true;
    normal.expanded = false;
    normal.path = QStringLiteral("textures\\armor\\prototype_n.dds");
    normal.values.insert(QStringLiteral("strength"), 0.75);
    normal.values.insert(QStringLiteral("heightInBlue"), true);
    document.setTextureSlot(QStringLiteral("UV0"), QStringLiteral("primaryNormal"), normal);
    Pbr::TextureSlotState landscapeArray;
    landscapeArray.enabled = true;
    landscapeArray.path = QStringLiteral("textures\\landscape\\terrain_base_array.dds");
    document.setTextureSlot(QStringLiteral("Landscape"), QStringLiteral("baseColorArray"), landscapeArray);
    Pbr::TextureSlotState legacyCubemap;
    legacyCubemap.enabled = true;
    legacyCubemap.path = QStringLiteral("Shared/Cubemaps/mipblur_DefaultOutside1.dds");
    document.setTextureSlot(QStringLiteral("Environment"), QStringLiteral("fallbackCubemap"), legacyCubemap);
    document.setSetting(QStringLiteral("Geometry/Source"), QStringLiteral("source"), QStringLiteral("Secondary Separate Height"));
    document.setSetting(QStringLiteral("Geometry/Parallax"), QStringLiteral("enabled"), true);
    document.setSetting(QStringLiteral("Geometry/Parallax"), QStringLiteral("mode"), QStringLiteral("Self-Shadowed POM"));
    document.setSetting(QStringLiteral("Geometry/Parallax"), QStringLiteral("quality"), QStringLiteral("Ultra"));
    document.setSetting(QStringLiteral("Geometry/Parallax"), QStringLiteral("depth"), 0.0625);
    document.setSetting(QStringLiteral("Geometry/Parallax"), QStringLiteral("shadowSteps"), 32);
    document.setSetting(QStringLiteral("Geometry/Parallax"), QStringLiteral("silhouetteClipping"), true);
    document.setSetting(QStringLiteral("Geometry/Parallax"), QStringLiteral("depthOutput"), true);
    document.setSetting(QStringLiteral("UV0"), QStringLiteral("source"), QStringLiteral("World Planar"));
    document.setSetting(QStringLiteral("UV0"), QStringLiteral("projectionAxis"), QStringLiteral("Triplanar"));
    document.setSetting(QStringLiteral("UV0/Animation"), QStringLiteral("scrollU"), 0.125);
    document.setSetting(QStringLiteral("UV1/Oscillation"), QStringLiteral("waveform"), QStringLiteral("Breathing"));
    document.setSetting(QStringLiteral("UV1/Oscillation"), QStringLiteral("frequency"), 0.4);
    document.setSetting(QStringLiteral("UV1/Oscillation"), QStringLiteral("scaleAmplitude"), 0.08);
    document.setSetting(QStringLiteral("UV1/Composition"), QStringLiteral("constantCoverage"), 0.75);
    document.setSetting(QStringLiteral("Transparency/Composition"), QStringLiteral("mode"), QStringLiteral("Alpha Test"));
    document.setSetting(QStringLiteral("Transparency/Composition"), QStringLiteral("threshold"), 0.375);
    document.setSetting(QStringLiteral("Shading/Subsurface"), QStringLiteral("customRadiusR"), 1.25);
    document.setSetting(QStringLiteral("Shading/BottleLiquid"), QStringLiteral("enabled"), true);
    document.setSetting(QStringLiteral("Shading/BottleLiquid"), QStringLiteral("fillLevel"), 0.55);
    document.setSetting(QStringLiteral("DynamicNormals/Primary"), QStringLiteral("constantValue"), 0.625);
    check(!document.toJson().value(QStringLiteral("resources")).toObject()
        .value(QStringLiteral("Environment")).toObject().contains(QStringLiteral("fallbackCubemap")),
        QStringLiteral("editor preview cubemap is never serialized into PBRM"));

    const QString fileName = directory.filePath(QStringLiteral("roundtrip.pbrm"));
    QString error;
    check(document.save(fileName, &error), QStringLiteral("PBRM V4 saves: %1").arg(error));

    Pbr::MaterialDocument loaded;
    check(loaded.load(fileName, &error), QStringLiteral("PBRM V4 loads: %1").arg(error));
    check(loaded.shaderMode() == Pbr::ShaderMode::AllFeatures, QStringLiteral("shader mode survives save/load"));
    check(loaded.secondaryUvEnabled(), QStringLiteral("Secondary UV state survives save/load"));
    check(loaded.tertiaryUvEnabled(), QStringLiteral("Tertiary UV state survives save/load"));
    check(loaded.textureSlot(QStringLiteral("UV2"), QStringLiteral("tertiaryBaseColor")).path == tertiaryBase.path
            && loaded.setting(QStringLiteral("UV2/Composition"), QStringLiteral("edgeSoftness")).toDouble() == 0.25,
        QStringLiteral("Tertiary UV textures and composition settings survive save/load"));
    Pbr::MaterialDocument sparseTertiary;
    check(!sparseTertiary.toJson().contains(QStringLiteral("tertiaryUv"))
            && !sparseTertiary.toJson().contains(QStringLiteral("tertiaryUvEnabled")),
        QStringLiteral("tertiary members are absent from documents that never author them"));
    check(qFuzzyCompare(loaded.textureSlot(QStringLiteral("UV0"), QStringLiteral("primarySurfaceStateResponse")).values.value(QStringLiteral("porosity")).toDouble(), 0.8)
            && loaded.setting(QStringLiteral("SurfaceStates/Wetness"), QStringLiteral("wetRoughness")).toDouble() == 0.06,
        QStringLiteral("legacy response input with an assigned map and state-look settings are preserved inert"));
    check(qFuzzyCompare(loaded.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryRmaos")).values.value(QStringLiteral("porosity")).toDouble(), 0.8),
        QStringLiteral("the response porosity constant folds onto RMAOS at load"));
    const auto loadedNormal = loaded.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryNormal"));
    check(loadedNormal.enabled && !loadedNormal.expanded, QStringLiteral("texture enable/collapse state survives save/load"));
    check(loadedNormal.path == normal.path, QStringLiteral("texture path survives save/load"));
    check(qFuzzyCompare(loadedNormal.values.value(QStringLiteral("strength")).toDouble(), 0.75), QStringLiteral("texture numeric values survive save/load"));
    check(loaded.textureSlot(QStringLiteral("Landscape"), QStringLiteral("baseColorArray")).path == landscapeArray.path,
        QStringLiteral("specialized non-UV resources survive save/load"));
    check(loaded.setting(QStringLiteral("Geometry/Parallax"), QStringLiteral("depth")).toDouble() == 0.0625, QStringLiteral("material settings survive save/load"));
    check(loaded.setting(QStringLiteral("Geometry/Source"), QStringLiteral("source")).toString() == QStringLiteral("Secondary Separate Height"),
        QStringLiteral("parallax height source survives save/load"));
    check(loaded.setting(QStringLiteral("Geometry/Parallax"), QStringLiteral("mode")).toString() == QStringLiteral("Self-Shadowed POM"),
        QStringLiteral("parallax algorithm survives save/load"));
    check(loaded.setting(QStringLiteral("Geometry/Parallax"), QStringLiteral("quality")).toString() == QStringLiteral("Ultra")
        && loaded.setting(QStringLiteral("Geometry/Parallax"), QStringLiteral("shadowSteps")).toInt() == 32,
        QStringLiteral("parallax quality controls survive save/load"));
    check(loaded.setting(QStringLiteral("Geometry/Parallax"), QStringLiteral("silhouetteClipping")).toBool()
        && loaded.setting(QStringLiteral("Geometry/Parallax"), QStringLiteral("depthOutput")).toBool(),
        QStringLiteral("parallax integration flags survive save/load"));
    check(loaded.setting(QStringLiteral("UV0"), QStringLiteral("projectionAxis")).toString() == QStringLiteral("Triplanar")
        && loaded.setting(QStringLiteral("UV0/Animation"), QStringLiteral("scrollU")).toDouble() == 0.125,
        QStringLiteral("planar projection and UV animation settings survive save/load"));
    check(loaded.setting(QStringLiteral("UV1/Composition"), QStringLiteral("constantCoverage")).toDouble() == 0.75,
        QStringLiteral("secondary constant coverage survives save/load"));
    check(loaded.setting(QStringLiteral("UV1/Oscillation"), QStringLiteral("waveform")).toString() == QStringLiteral("Breathing")
        && loaded.setting(QStringLiteral("UV1/Oscillation"), QStringLiteral("frequency")).toDouble() == 0.4
        && loaded.setting(QStringLiteral("UV1/Oscillation"), QStringLiteral("scaleAmplitude")).toDouble() == 0.08,
        QStringLiteral("per-layer oscillation settings survive save/load"));
    check(loaded.setting(QStringLiteral("Transparency/Composition"), QStringLiteral("mode")).toString() == QStringLiteral("Alpha Test")
        && loaded.setting(QStringLiteral("Transparency/Composition"), QStringLiteral("threshold")).toDouble() == 0.375,
        QStringLiteral("transparency preview settings survive save/load"));
    check(loaded.setting(QStringLiteral("Shading/Subsurface"), QStringLiteral("customRadiusR")).toDouble() == 1.25
        && loaded.setting(QStringLiteral("DynamicNormals/Primary"), QStringLiteral("constantValue")).toDouble() == 0.625,
        QStringLiteral("custom profile and constant-driver values survive save/load"));
    check(loaded.setting(QStringLiteral("Shading/BottleLiquid"), QStringLiteral("enabled")).toBool()
        && loaded.setting(QStringLiteral("Shading/BottleLiquid"), QStringLiteral("fillLevel")).toDouble() == 0.55,
        QStringLiteral("bottle liquid settings survive save/load"));

    {
        // Generic surface state slots: empty documents resolve to the five
        // factory presets, legacy constants fold in, authored slots override.
        Pbr::MaterialDocument statesDocument;
        const auto presets = Pbr::resolveSurfaceStates(statesDocument);
        check(presets.size() == 5 && presets[0].name == QStringLiteral("Wetness") && presets[0].tagLiquid
            && presets[1].tagParticulate && presets[2].tagFrozen && presets[3].tagThermal && presets[4].tagViscous,
            QStringLiteral("an unauthored document resolves to the five factory preset states"));
        check(presets[1].aoBias == 0.35 && presets[0].heightBias == 0.3 && presets[0].metalSeal == 0.8
            && presets[2].emissiveDry == 0.7 && presets[1].roughnessBias == 0.25,
            QStringLiteral("map-guided accumulation biases resolve from the factory presets"));
        statesDocument.setSetting(QStringLiteral("SurfaceStates/Wetness"), QStringLiteral("wetRoughness"), 0.12);
        const auto legacy = Pbr::resolveSurfaceStates(statesDocument);
        check(legacy.size() == 5 && legacy[0].roughnessTarget == 0.12 && !legacy[0].ripples,
            QStringLiteral("legacy named-group constants fold into the resolved presets with enhancements zeroed"));
        statesDocument.setSetting(QStringLiteral("SurfaceStates/State5"), QStringLiteral("enabled"), true);
        statesDocument.setSetting(QStringLiteral("SurfaceStates/State5"), QStringLiteral("name"), QStringLiteral("Oil"));
        statesDocument.setSetting(QStringLiteral("SurfaceStates/State5"), QStringLiteral("tagLiquid"), true);
        statesDocument.setSetting(QStringLiteral("SurfaceStates/State5"), QStringLiteral("tagViscous"), true);
        statesDocument.setSetting(QStringLiteral("SurfaceStates/State5"), QStringLiteral("gainRate"), 2.5);
        statesDocument.setSetting(QStringLiteral("SurfaceStates/State5"), QStringLiteral("dryRate"), 0.1);
        statesDocument.setSetting(QStringLiteral("SurfaceStates/State1"), QStringLiteral("enabled"), false);
        const auto merged = Pbr::resolveSurfaceStates(statesDocument);
        bool oilFound = false, dustFound = false;
        double oilGainRate = 0, oilDryRate = 0;
        for (const auto& state : merged) {
            if (state.name == QStringLiteral("Oil")) { oilFound = true; oilGainRate = state.gainRate; oilDryRate = state.dryRate; }
            if (state.slot == 1) dustFound = true;
        }
        check(oilFound && !dustFound && merged.size() == 5,
            QStringLiteral("authored slots merge with the presets: custom Oil appears, disabled Dust disappears"));
        check(oilGainRate == 2.5 && oilDryRate == 0.1,
            QStringLiteral("per-state gain and dry rates resolve from authored slots"));
        const QString statesFile = directory.filePath(QStringLiteral("states.pbrm"));
        check(statesDocument.save(statesFile, &error), QStringLiteral("state-slot document saves: %1").arg(error));
        Pbr::MaterialDocument statesLoaded;
        check(statesLoaded.load(statesFile, &error)
            && statesLoaded.setting(QStringLiteral("SurfaceStates/State5"), QStringLiteral("name")).toString() == QStringLiteral("Oil"),
            QStringLiteral("generic state slot keys survive save/load"));
        // Tag physics: thermal dries liquid and melts frozen into liquid;
        // liquid washes particulate; viscous resists.
        const auto physicsStates = Pbr::surfaceStatePresets();
        QVector<float> amounts{1.0f, 1.0f, 1.0f, 1.0f, 1.0f}; // wet, dust, frost, heat, blood
        const auto effective = Pbr::surfaceStateInteractions(physicsStates, amounts);
        check(effective[0] < 1.0f && effective[2] == 0.0f && effective[3] == 1.0f,
            QStringLiteral("full heat dries wetness and eliminates frost while thermal itself persists"));
        const auto washed = Pbr::surfaceStateInteractions(physicsStates, {1.0f, 1.0f, 0.0f, 0.0f, 0.0f});
        check(washed[1] < 0.25f && washed[0] == 1.0f,
            QStringLiteral("full wetness washes most dust away"));
        const auto melted = Pbr::surfaceStateInteractions(physicsStates, {0.0f, 0.0f, 1.0f, 0.5f, 0.0f});
        check(melted[0] > 0.0f && melted[2] < 1.0f,
            QStringLiteral("heat melting frost yields liquid in the wetness slot"));
    }

    {
        // v4 -> v5 migration: the retired response slot folds into its new
        // homes, per-state bindings lift into SurfaceResponse, and the retired
        // pattern source becomes Constant.
        QJsonObject responseValues{{QStringLiteral("porosity"), 0.35}, {QStringLiteral("overridePorosity"), true},
            {QStringLiteral("adhesion"), 0.6}};
        QJsonObject responseSlot{{QStringLiteral("enabled"), true}, {QStringLiteral("expanded"), true},
            {QStringLiteral("path"), QString()}, {QStringLiteral("values"), responseValues}};
        QJsonObject v4Settings{
            {QStringLiteral("SurfaceStates/State0"), QJsonObject{{QStringLiteral("name"), QStringLiteral("Wetness")},
                {QStringLiteral("maskChannel"), QStringLiteral("G")}, {QStringLiteral("shelterChannel"), QStringLiteral("R")}}},
            {QStringLiteral("UV1/Composition"), QJsonObject{{QStringLiteral("pattern"), QStringLiteral("Primary Normal Alpha")}}}
        };
        QJsonObject v4Root{{QStringLiteral("schema"), QStringLiteral("FO4.PBRM.Material")},
            {QStringLiteral("schemaVersion"), 4}, {QStringLiteral("shader"), QStringLiteral("Standard")},
            {QStringLiteral("secondaryUvEnabled"), true},
            {QStringLiteral("primaryUv"), QJsonObject{{QStringLiteral("primarySurfaceStateResponse"), responseSlot}}},
            {QStringLiteral("secondaryUv"), QJsonObject()}, {QStringLiteral("resources"), QJsonObject()},
            {QStringLiteral("settings"), v4Settings}};
        const QByteArray v4Payload = QJsonDocument(v4Root).toJson(QJsonDocument::Compact);
        QByteArray v4Envelope;
        QDataStream envelopeStream(&v4Envelope, QIODevice::WriteOnly);
        envelopeStream.setByteOrder(QDataStream::LittleEndian);
        envelopeStream << Pbr::MaterialDocument::Magic << Pbr::MaterialDocument::LegacyVersion << quint32(v4Payload.size());
        v4Envelope.append(v4Payload);
        const QString v4File = directory.filePath(QStringLiteral("legacy-v4.pbrm"));
        QFile v4Out(v4File);
        check(v4Out.open(QIODevice::WriteOnly) && v4Out.write(v4Envelope) == v4Envelope.size(),
            QStringLiteral("legacy v4 envelope can be written"));
        v4Out.close();
        Pbr::MaterialDocument migrated;
        check(migrated.load(v4File, &error), QStringLiteral("legacy v4 envelope loads into the v5 editor: %1").arg(error));
        check(!migrated.textureSet(QStringLiteral("UV0")).contains(QStringLiteral("primarySurfaceStateResponse")),
            QStringLiteral("v4->v5: a pathless response slot is removed after folding"));
        const QJsonObject foldedRmaos = migrated.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryRmaos")).values;
        check(qFuzzyCompare(foldedRmaos.value(QStringLiteral("porosity")).toDouble(), 0.35)
            && foldedRmaos.value(QStringLiteral("overridePorosity")).toBool(false),
            QStringLiteral("v4->v5: the response porosity constant folds onto RMAOS"));
        check(qFuzzyCompare(migrated.setting(QStringLiteral("SurfaceResponse"), QStringLiteral("adhesion")).toDouble(), 0.6),
            QStringLiteral("v4->v5: the response adhesion constant folds into SurfaceResponse"));
        check(migrated.setting(QStringLiteral("SurfaceResponse"), QStringLiteral("maskG")).toString() == QStringLiteral("Wetness")
            && migrated.setting(QStringLiteral("SurfaceResponse"), QStringLiteral("shelterChannel")).toString() == QStringLiteral("Red"),
            QStringLiteral("v4->v5: per-state mask/shelter selections lift into SurfaceResponse bindings"));
        check(migrated.setting(QStringLiteral("UV1/Composition"), QStringLiteral("pattern")).toString() == QStringLiteral("Constant"),
            QStringLiteral("v4->v5: the retired Primary Normal Alpha pattern source becomes Constant"));
        check(!migrated.settingGroup(QStringLiteral("SurfaceStates/State0")).isEmpty(),
            QStringLiteral("v4->v5: legacy state groups are preserved inert for the extraction tooling"));
        const QString v5File = directory.filePath(QStringLiteral("migrated-v5.pbrm"));
        check(migrated.save(v5File, &error), QStringLiteral("migrated document saves as v5: %1").arg(error));
        QFile v5In(v5File);
        check(v5In.open(QIODevice::ReadOnly), QStringLiteral("migrated v5 file reopens"));
        const QJsonObject v5Root = QJsonDocument::fromJson(payloadFromEnvelope(v5In.readAll())).object();
        v5In.close();
        check(v5Root.value(QStringLiteral("schemaVersion")).toInt() == 5,
            QStringLiteral("migrated documents serialize as schema version 5"));
        // Idempotency: reloading and re-saving the migrated document must be
        // byte-identical — the migration never re-fires on its own output.
        Pbr::MaterialDocument reloadedV5;
        check(reloadedV5.load(v5File, &error), QStringLiteral("migrated v5 reloads: %1").arg(error));
        const QString v5FileAgain = directory.filePath(QStringLiteral("migrated-v5-again.pbrm"));
        check(reloadedV5.save(v5FileAgain, &error), QStringLiteral("reloaded v5 re-saves: %1").arg(error));
        QFile firstSave(v5File), secondSave(v5FileAgain);
        check(firstSave.open(QIODevice::ReadOnly) && secondSave.open(QIODevice::ReadOnly)
            && firstSave.readAll() == secondSave.readAll(),
            QStringLiteral("v4->v5 migration is idempotent: the second save is byte-identical"));
    }

    {
        // Runtime definitions asset: presets -> JSON -> parse round trip plus
        // file save/load and schema rejection.
        const auto definitionPresets = Pbr::surfaceStatePresets();
        check(definitionPresets[0].respectsShelter && definitionPresets[1].respectsShelter && definitionPresets[2].respectsShelter
            && !definitionPresets[3].respectsShelter && !definitionPresets[4].respectsShelter,
            QStringLiteral("weather-driven presets respect shelter; heat and blood do not"));
        const QJsonObject definitionsJson = Pbr::surfaceStateDefinitionsToJson(definitionPresets);
        check(definitionsJson.value(QStringLiteral("schema")).toString() == QStringLiteral("FO4.PBRM.SurfaceStateDefinitions")
            && definitionsJson.value(QStringLiteral("states")).toArray().size() == definitionPresets.size(),
            QStringLiteral("definitions JSON carries the schema id and every state"));
        check(!definitionsJson.value(QStringLiteral("states")).toArray().first().toObject().contains(QStringLiteral("maskChannel")),
            QStringLiteral("definitions never serialize the material-side mask binding"));
        QString definitionsError;
        const auto parsedDefinitions = Pbr::surfaceStateDefinitionsFromJson(definitionsJson, &definitionsError);
        check(parsedDefinitions.size() == definitionPresets.size() && parsedDefinitions[0].name == definitionPresets[0].name
            && qFuzzyCompare(parsedDefinitions[0].roughnessTarget + 1.0, definitionPresets[0].roughnessTarget + 1.0)
            && parsedDefinitions[4].tagViscous && parsedDefinitions[0].respectsShelter,
            QStringLiteral("definitions JSON round-trips the full profile: %1").arg(definitionsError));
        const QString definitionsFile = directory.filePath(QStringLiteral("SurfaceStates.json"));
        check(Pbr::saveSurfaceStateDefinitions(definitionsFile, definitionPresets, &definitionsError),
            QStringLiteral("definitions save to disk: %1").arg(definitionsError));
        QVector<Pbr::SurfaceStateSpec> reloadedDefinitions;
        check(Pbr::loadSurfaceStateDefinitions(definitionsFile, &reloadedDefinitions, &definitionsError)
            && reloadedDefinitions.size() == definitionPresets.size(),
            QStringLiteral("definitions load from disk: %1").arg(definitionsError));
        QString rejection;
        check(Pbr::surfaceStateDefinitionsFromJson(QJsonObject{{QStringLiteral("schema"), QStringLiteral("Nope")}}, &rejection).isEmpty()
            && !rejection.isEmpty(),
            QStringLiteral("unknown definitions schemas are rejected"));
    }

    {
        // BGSM/BGEM reader: byte-exact synthetic materials mirroring the
        // ousnius MaterialLib serialize order, plus root-chain borrowing and
        // an optional real-corpus sweep (PBRM_TEST_BGSM_DIR).
        struct MaterialBuilder {
            QByteArray data;
            void u8(quint8 v) { data.append(char(v)); }
            void boolean(bool v) { u8(v ? 1 : 0); }
            void u32(quint32 v) { for (int i = 0; i < 4; ++i) data.append(char((v >> (8 * i)) & 0xFF)); }
            void f32(float v) { quint32 raw; std::memcpy(&raw, &v, 4); u32(raw); }
            void color(float r, float g, float b) { f32(r); f32(g); f32(b); }
            void str(const QString& s) { const QByteArray latin = s.toLatin1() + '\0'; u32(quint32(latin.size())); data.append(latin); }
            void header(quint32 signature, quint32 version, bool envMapping, float envMaskScale) {
                u32(signature); u32(version);
                u32(3); // tile U+V
                f32(.25f); f32(.5f); f32(2.0f); f32(4.0f); // offsets/scales
                f32(.75f); // alpha
                u8(1); u32(6); u32(7); // Standard blend
                u8(96); boolean(true); // alphaTestRef, alphaTest
                boolean(true); boolean(true); boolean(false); boolean(false); // z/ssr
                boolean(false); boolean(true); boolean(false); boolean(false); // decal/twoSided/...
                boolean(true); boolean(false); f32(.6f); // refraction block
                if (version < 10) { boolean(envMapping); f32(envMaskScale); }
                else boolean(false); // depthBias
                boolean(false); // grayscaleToPalette
                if (version >= 6) u8(63);
            }
        };
        constexpr quint32 BgsmSig = 0x4D534742u, BgemSig = 0x4D454742u;

        // Fallout 4 BGSM (version 2).
        MaterialBuilder v2;
        v2.header(BgsmSig, 2, true, 1.5f);
        v2.str(QStringLiteral("textures\\armor\\a_d.dds"));
        v2.str(QStringLiteral("textures\\armor\\a_n.dds"));
        v2.str(QStringLiteral("textures\\armor\\a_s.dds"));
        v2.str(QString());
        v2.str(QStringLiteral("textures\\shared\\cubemaps\\chrome_e.dds"));
        v2.str(QStringLiteral("textures\\armor\\a_g.dds"));
        v2.str(QString()); v2.str(QString()); v2.str(QString());
        v2.boolean(false); // editor alpha ref
        v2.boolean(true); v2.f32(3.0f); v2.f32(.4f); // rim block (v<8)
        v2.boolean(false); v2.f32(.3f);
        v2.boolean(true); v2.color(1.0f, .8f, .6f); v2.f32(1.7f); v2.f32(.82f); // spec
        v2.f32(5.0f); v2.f32(-1); v2.f32(-1); v2.f32(-1); v2.f32(-1); v2.f32(-1); v2.f32(-1); // fresnel+wetness (incl. envmap, v<10)
        v2.str(QStringLiteral("materials\\shared\\root.bgsm"));
        v2.boolean(false); // aniso
        v2.boolean(true); v2.color(.2f, .9f, .3f); // emit + color
        v2.f32(2.5f); // emittance mult
        v2.boolean(false); v2.boolean(false); // msn/external
        v2.boolean(false); // backlighting (v<8)
        v2.boolean(true); v2.boolean(false); v2.boolean(true); v2.boolean(false); v2.boolean(false); // shadows...
        v2.boolean(true); // glowmap
        v2.boolean(false); v2.boolean(false); // envmap window/eye (v<7)
        v2.boolean(false); v2.color(.5f, .5f, .5f); // hair
        v2.boolean(false); v2.boolean(false); v2.boolean(false); v2.boolean(false); // tree/facegen/skin/tess
        v2.f32(-.5f); v2.f32(10.0f); v2.f32(1.0f); v2.f32(1.0f); v2.f32(0.0f); // displacement/tess (v<3)
        v2.f32(1.0f); v2.boolean(false); // grayscale scale, skew (v>=1)
        Bgsm::Material fo4;
        QString bgsmError;
        check(Bgsm::read(v2.data, &fo4, &bgsmError), QStringLiteral("synthetic FO4 BGSM v2 parses: %1").arg(bgsmError));
        check(fo4.kind == Bgsm::Kind::Bgsm && fo4.version == 2 && !fo4.isFallout76(),
            QStringLiteral("v2 identifies as a Fallout 4 BGSM"));
        check(fo4.blendMode == Bgsm::AlphaBlendMode::Standard && fo4.alphaTest && fo4.alphaTestRef == 96
            && fo4.twoSided && fo4.refraction && qFuzzyCompare(fo4.refractionPower, .6f),
            QStringLiteral("v2 header fields parse (blend, alpha test, two-sided, refraction)"));
        check(fo4.environmentMapping && qFuzzyCompare(fo4.environmentMappingMaskScale, 1.5f),
            QStringLiteral("v2 environment mapping and mask scale parse"));
        check(fo4.diffuseTexture == QStringLiteral("textures\\armor\\a_d.dds")
            && fo4.envmapTexture == QStringLiteral("textures\\shared\\cubemaps\\chrome_e.dds")
            && fo4.glowTexture == QStringLiteral("textures\\armor\\a_g.dds"),
            QStringLiteral("v2 texture set parses (diffuse, envmap cubemap, glow)"));
        check(fo4.rimLighting && qFuzzyCompare(fo4.rimPower, 3.0f) && fo4.specularEnabled
            && qFuzzyCompare(fo4.specularMult, 1.7f) && qFuzzyCompare(fo4.smoothness, .82f)
            && qFuzzyCompare(fo4.specularColor.y(), .8f),
            QStringLiteral("v2 rim and specular parameters parse"));
        check(fo4.emitEnabled && qFuzzyCompare(fo4.emittanceColor.y(), .9f) && qFuzzyCompare(fo4.emittanceMult, 2.5f)
            && fo4.glowmap && fo4.rootMaterialPath == QStringLiteral("materials\\shared\\root.bgsm"),
            QStringLiteral("v2 conditional emittance color, glowmap, and root path parse"));

        // Fallout 76 BGSM (version 20): FO76 texture set, translucency, PBR.
        MaterialBuilder v20;
        v20.header(BgsmSig, 20, false, 1.0f);
        v20.str(QStringLiteral("textures\\armor76\\b_d.dds"));
        v20.str(QStringLiteral("textures\\armor76\\b_n.dds"));
        v20.str(QString());
        v20.str(QString());
        v20.str(QStringLiteral("textures\\armor76\\b_g.dds"));
        v20.str(QString());
        v20.str(QStringLiteral("textures\\armor76\\b_s.dds"));
        v20.str(QStringLiteral("textures\\armor76\\b_l.dds"));
        v20.str(QString());
        v20.str(QString()); // distance field alpha (v>=17)
        v20.boolean(false); // editor alpha ref
        v20.boolean(true); v20.boolean(false); v20.boolean(true); // translucency block (v>=8)
        v20.color(.9f, .5f, .4f); v20.f32(.65f); v20.f32(.2f);
        v20.boolean(true); v20.color(1, 1, 1); v20.f32(1.0f); v20.f32(.35f); // spec
        v20.f32(5.0f); v20.f32(-1); v20.f32(-1); v20.f32(-1); // no wetness envmap (v>=10)
        v20.f32(-1); v20.f32(-1);
        v20.boolean(true); // PBR (v>2)
        v20.boolean(true); v20.f32(.42f); // porosity (v>=9)
        v20.str(QString());
        v20.boolean(false);
        v20.boolean(false); // emit disabled -> no color in stream
        v20.f32(1.0f);
        v20.boolean(false); v20.boolean(false);
        v20.f32(100.0f); // lum (v>=12)
        v20.boolean(false); v20.f32(0); v20.f32(0); v20.f32(0); // adaptive (v>=13)
        v20.boolean(true); v20.boolean(false); v20.boolean(true); v20.boolean(false); v20.boolean(false);
        v20.boolean(false); // glowmap
        v20.boolean(false); v20.color(.5f, .5f, .5f);
        v20.boolean(false); v20.boolean(false); v20.boolean(true); v20.boolean(false); // skinTint true
        v20.f32(1.0f); v20.boolean(false);
        v20.boolean(false); // terrain (v>=3)
        Bgsm::Material fo76;
        check(Bgsm::read(v20.data, &fo76, &bgsmError), QStringLiteral("synthetic FO76 BGSM v20 parses: %1").arg(bgsmError));
        check(fo76.isFallout76() && fo76.pbr && fo76.customPorosity && qFuzzyCompare(fo76.porosityValue, .42f),
            QStringLiteral("v20 PBR flag and porosity parse"));
        check(fo76.specularTexture == QStringLiteral("textures\\armor76\\b_s.dds")
            && fo76.lightingTexture == QStringLiteral("textures\\armor76\\b_l.dds"),
            QStringLiteral("v20 FO76 specular/lighting textures parse"));
        check(fo76.translucency && fo76.translucencyMixAlbedo && qFuzzyCompare(fo76.translucencySubsurfaceColor.x(), .9f)
            && qFuzzyCompare(fo76.translucencyTransmissiveScale, .65f),
            QStringLiteral("v20 translucency block parses"));
        check(!fo76.emitEnabled && qFuzzyCompare(fo76.lumEmittance, 100.0f) && fo76.skinTint,
            QStringLiteral("v20 disabled emit skips the color and lum emittance parses"));

        // Fallout 4 BGEM (version 2).
        MaterialBuilder gem;
        gem.header(BgemSig, 2, true, 2.0f);
        gem.str(QStringLiteral("textures\\effects\\glow_d.dds"));
        gem.str(QString());
        gem.str(QString());
        gem.str(QString());
        gem.str(QString());
        gem.boolean(false); gem.boolean(true); // blood, effect lighting
        gem.boolean(true); gem.boolean(false); gem.boolean(false); gem.boolean(true); // falloff/soft
        gem.color(.1f, .6f, 1.0f); gem.f32(8.0f); // base color + scale
        gem.f32(10.0f); gem.f32(80.0f); gem.f32(1.0f); gem.f32(0.0f); // falloff
        gem.f32(.25f); gem.u8(2); gem.f32(.15f); // lighting influence, minLOD, soft depth
        Bgsm::Material effect;
        check(Bgsm::read(gem.data, &effect, &bgsmError), QStringLiteral("synthetic FO4 BGEM v2 parses: %1").arg(bgsmError));
        check(effect.kind == Bgsm::Kind::Bgem && effect.baseTexture == QStringLiteral("textures\\effects\\glow_d.dds")
            && effect.effectLightingEnabled && effect.falloffEnabled && effect.softEnabled,
            QStringLiteral("BGEM identifies and effect flags parse"));
        check(qFuzzyCompare(effect.baseColor.z(), 1.0f) && qFuzzyCompare(effect.baseColorScale, 8.0f)
            && qFuzzyCompare(effect.lightingInfluence, .25f) && effect.envmapMinLOD == 2,
            QStringLiteral("BGEM base color/scale (the diffuse-as-emission inputs) parse"));

        // Rejections: wrong signature and truncated data.
        QByteArray bogus = v2.data; bogus[0] = 'X';
        check(!Bgsm::read(bogus, &fo4), QStringLiteral("invalid signatures are rejected"));
        check(!Bgsm::read(v2.data.left(v2.data.size() / 2), &fo4), QStringLiteral("truncated materials are rejected"));

        // Root chain: the child borrows only empty texture fields.
        Bgsm::Material chained;
        check(Bgsm::read(v2.data, &chained, &bgsmError), QStringLiteral("root-chain child reparses"));
        MaterialBuilder rootBuilder;
        rootBuilder.header(BgsmSig, 2, false, 1.0f);
        rootBuilder.str(QStringLiteral("textures\\shared\\root_d.dds"));
        rootBuilder.str(QStringLiteral("textures\\shared\\root_n.dds"));
        rootBuilder.str(QStringLiteral("textures\\shared\\root_s.dds"));
        rootBuilder.str(QStringLiteral("textures\\shared\\root_grey.dds"));
        rootBuilder.str(QString()); rootBuilder.str(QString()); rootBuilder.str(QString());
        rootBuilder.str(QString()); rootBuilder.str(QString());
        rootBuilder.boolean(false);
        rootBuilder.boolean(false); rootBuilder.f32(2.0f); rootBuilder.f32(0.0f);
        rootBuilder.boolean(false); rootBuilder.f32(.3f);
        rootBuilder.boolean(false); rootBuilder.color(1, 1, 1); rootBuilder.f32(1.0f); rootBuilder.f32(1.0f);
        rootBuilder.f32(5.0f); rootBuilder.f32(-1); rootBuilder.f32(-1); rootBuilder.f32(-1); rootBuilder.f32(-1); rootBuilder.f32(-1); rootBuilder.f32(-1);
        rootBuilder.str(QString());
        rootBuilder.boolean(false);
        rootBuilder.boolean(false);
        rootBuilder.f32(1.0f);
        rootBuilder.boolean(false); rootBuilder.boolean(false);
        rootBuilder.boolean(false);
        rootBuilder.boolean(true); rootBuilder.boolean(false); rootBuilder.boolean(true); rootBuilder.boolean(false); rootBuilder.boolean(false);
        rootBuilder.boolean(false);
        rootBuilder.boolean(false); rootBuilder.boolean(false);
        rootBuilder.boolean(false); rootBuilder.color(.5f, .5f, .5f);
        rootBuilder.boolean(false); rootBuilder.boolean(false); rootBuilder.boolean(false); rootBuilder.boolean(false);
        rootBuilder.f32(-.5f); rootBuilder.f32(10.0f); rootBuilder.f32(1.0f); rootBuilder.f32(1.0f); rootBuilder.f32(0.0f);
        rootBuilder.f32(1.0f); rootBuilder.boolean(false);
        const auto lookup = [&rootBuilder](const QString& path, QByteArray* data) {
            if (!path.compare(QStringLiteral("materials\\shared\\root.bgsm"), Qt::CaseInsensitive)) {
                *data = rootBuilder.data;
                return true;
            }
            return false;
        };
        check(Bgsm::resolveRootChain(&chained, lookup, &bgsmError), QStringLiteral("root chain resolves: %1").arg(bgsmError));
        check(chained.diffuseTexture == QStringLiteral("textures\\armor\\a_d.dds")
            && chained.greyscaleTexture == QStringLiteral("textures\\shared\\root_grey.dds"),
            QStringLiteral("root chain borrows only the textures the child left empty"));
        Bgsm::Material cyclic;
        check(Bgsm::read(v2.data, &cyclic, &bgsmError), QStringLiteral("cycle-test child reparses"));
        const auto cyclicLookup = [&v2](const QString&, QByteArray* data) { *data = v2.data; return true; };
        check(!Bgsm::resolveRootChain(&cyclic, cyclicLookup), QStringLiteral("root chain cycles are rejected"));

        // Real-corpus sweep: parse every material under PBRM_TEST_BGSM_DIR.
        const QString corpusDir = qEnvironmentVariable("PBRM_TEST_BGSM_DIR");
        if (!corpusDir.isEmpty() && QFileInfo(corpusDir).isDir()) {
            int parsed = 0, failed = 0;
            QString firstFailure;
            QDirIterator it(corpusDir, {QStringLiteral("*.bgsm"), QStringLiteral("*.bgem")}, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString file = it.next();
                Bgsm::Material corpusMaterial;
                QString corpusError;
                if (Bgsm::readFile(file, &corpusMaterial, &corpusError)) ++parsed;
                else {
                    ++failed;
                    if (firstFailure.isEmpty()) firstFailure = file + QStringLiteral(": ") + corpusError;
                }
            }
            check(parsed > 0 && failed == 0,
                QStringLiteral("real material corpus parses cleanly: %1 parsed, %2 failed (%3)")
                    .arg(parsed).arg(failed).arg(firstFailure));
        }

        // ---- Orchestrator: a synthetic FO4 metal through the full pipeline
        // (sources -> conversion -> derivations -> DDS set -> sibling .pbrm),
        // then the dedup and dry-run contracts.
        QTemporaryDir convRoot, convOut;
        check(convRoot.isValid() && convOut.isValid(), QStringLiteral("orchestrator temp roots create"));
        QDir(convRoot.path()).mkpath(QStringLiteral("materials/armor"));
        QDir(convRoot.path()).mkpath(QStringLiteral("textures/armor"));
        QImage convDiffuse(16, 16, QImage::Format_RGBA8888);
        convDiffuse.fill(QColor(90, 60, 45, 255)); // dark warm metal (hue-reliable)
        convDiffuse.save(QDir(convRoot.path()).filePath(QStringLiteral("textures/armor/conv_d.png")));
        QImage convSpec(16, 16, QImage::Format_RGBA8888);
        convSpec.fill(QColor(230, 128, 0, 255)); // R spec strength, G gloss
        convSpec.save(QDir(convRoot.path()).filePath(QStringLiteral("textures/armor/conv_s.png")));
        QImage convNormal(16, 16, QImage::Format_RGBA8888);
        convNormal.fill(QColor(128, 128, 255, 255));
        convNormal.save(QDir(convRoot.path()).filePath(QStringLiteral("textures/armor/conv_n.png")));

        const auto buildConvMaterial = [&](const QString& fileName) {
            MaterialBuilder builder;
            builder.header(BgsmSig, 2, true, 1.0f);
            builder.str(QStringLiteral("armor/conv_d.png"));
            builder.str(QStringLiteral("armor/conv_n.png"));
            builder.str(QStringLiteral("armor/conv_s.png"));
            builder.str(QString()); builder.str(QString()); builder.str(QString());
            builder.str(QString()); builder.str(QString()); builder.str(QString());
            builder.boolean(false);
            builder.boolean(false); builder.f32(2.0f); builder.f32(0.0f);
            builder.boolean(false); builder.f32(.3f);
            builder.boolean(true); builder.color(1, 1, 1); builder.f32(1.0f); builder.f32(.5f);
            builder.f32(5.0f); builder.f32(-1); builder.f32(-1); builder.f32(-1); builder.f32(-1); builder.f32(-1); builder.f32(-1);
            builder.str(QString()); // no root chain
            builder.boolean(false);
            builder.boolean(false); // emit disabled -> no color in stream
            builder.f32(1.0f);
            builder.boolean(false); builder.boolean(false);
            builder.boolean(false);
            builder.boolean(true); builder.boolean(false); builder.boolean(true); builder.boolean(false); builder.boolean(false);
            builder.boolean(false); // glowmap
            builder.boolean(false); builder.boolean(false);
            builder.boolean(false); builder.color(.5f, .5f, .5f);
            builder.boolean(false); builder.boolean(false); builder.boolean(false); builder.boolean(false);
            builder.f32(-.5f); builder.f32(10.0f); builder.f32(1.0f); builder.f32(1.0f); builder.f32(0.0f);
            builder.f32(1.0f); builder.boolean(false);
            const QString path = QDir(convRoot.path()).filePath(QStringLiteral("materials/armor/") + fileName);
            QFile file(path);
            file.open(QIODevice::WriteOnly);
            file.write(builder.data);
            file.close();
            return path;
        };

        Converter::JobOptions jobOptions;
        jobOptions.outputRoot = convOut.path();
        jobOptions.textureRoot = convRoot.path() + QStringLiteral("/textures");
        jobOptions.derive.derivationSize = 64;
        Converter::TextureCache textureCache;
        const Converter::JobResult first = Converter::convertMaterial(buildConvMaterial(QStringLiteral("conv.bgsm")), jobOptions, &textureCache);
        check(first.converted && first.errors.isEmpty(),
            QStringLiteral("orchestrator converts the synthetic metal: %1").arg(first.errors.join(QStringLiteral("; "))));
        check(first.metalCoverage > 0.9f,
            QStringLiteral("dark diffuse + strong spec + envmap infers metal (coverage %1)").arg(first.metalCoverage));
        check(QFileInfo::exists(first.pbrmPath) && first.pbrmPath.endsWith(QStringLiteral("Materials/armor/conv.pbrm")),
            QStringLiteral("sibling .pbrm lands on the materials-relative path"));
        for (const QString& suffix : {QStringLiteral("_BC"), QStringLiteral("_RMAOS"), QStringLiteral("_N")})
            check(QFileInfo::exists(convOut.path() + QStringLiteral("/Textures/armor/conv") + suffix + QStringLiteral(".dds")),
                QStringLiteral("converted %1 texture is written").arg(suffix));
        check(!QFileInfo::exists(convOut.path() + QStringLiteral("/Textures/armor/conv_E.dds")),
            QStringLiteral("no emissive output for a non-emissive material"));

        Pbr::MaterialDocument convertedDoc;
        QString convertedError;
        check(convertedDoc.load(first.pbrmPath, &convertedError), QStringLiteral("converted .pbrm loads: %1").arg(convertedError));
        const Pbr::TextureSlotState convertedBase = convertedDoc.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryBaseColor"));
        const Pbr::TextureSlotState convertedNormalSlot = convertedDoc.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryNormal"));
        check(convertedBase.enabled && QDir::fromNativeSeparators(convertedBase.path) == QStringLiteral("armor/conv_BC.dds"),
            QStringLiteral("base color slot wires the portable texture path"));
        check(convertedNormalSlot.values.value(QStringLiteral("curvatureInAlpha")).toBool(false),
            QStringLiteral("normal slot declares curvature in alpha"));

        QString decodeError;
        const QImage convertedBc = TextureImage::load(QStringLiteral("armor/conv_BC.dds"),
            convOut.path() + QStringLiteral("/Textures"), &decodeError);
        check(!convertedBc.isNull(), QStringLiteral("converted BC decodes: %1").arg(decodeError));
        if (!convertedBc.isNull()) {
            const QColor recovered = convertedBc.pixelColor(8, 8);
            check(recovered.redF() > 0.5f && recovered.redF() > recovered.blueF() + 0.05f,
                QStringLiteral("metal albedo recovery brightens and keeps the warm hue (%1 %2 %3)")
                    .arg(recovered.redF()).arg(recovered.greenF()).arg(recovered.blueF()));
        }
        const QImage convertedNormalOut = TextureImage::load(QStringLiteral("armor/conv_N.dds"),
            convOut.path() + QStringLiteral("/Textures"), &decodeError);
        check(!convertedNormalOut.isNull()
            && std::abs(convertedNormalOut.pixelColor(8, 8).alphaF() - 0.5f) < 0.08f,
            QStringLiteral("flat normal ships neutral signed curvature in alpha"));

        // Dedup: a second material over the same sources reuses the outputs.
        const Converter::JobResult second = Converter::convertMaterial(buildConvMaterial(QStringLiteral("conv2.bgsm")), jobOptions, &textureCache);
        check(second.converted && textureCache.baseColorBySignature.size() == 1,
            QStringLiteral("identical pairings dedup to one converted texture set"));
        Pbr::MaterialDocument secondDoc;
        check(secondDoc.load(second.pbrmPath, nullptr)
            && QDir::fromNativeSeparators(secondDoc.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryBaseColor")).path)
                == QStringLiteral("armor/conv_BC.dds"),
            QStringLiteral("the deduped sibling points at the shared textures"));

        // Dry run: reports, writes nothing.
        QTemporaryDir dryOut;
        Converter::JobOptions dryOptions = jobOptions;
        dryOptions.outputRoot = dryOut.path();
        dryOptions.dryRun = true;
        Converter::TextureCache dryCache;
        const Converter::JobResult dry = Converter::convertMaterial(buildConvMaterial(QStringLiteral("conv3.bgsm")), dryOptions, &dryCache);
        check(dry.converted && QDir(dryOut.path()).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).isEmpty(),
            QStringLiteral("dry run reports the conversion without writing"));

        // Texture-free BGEM (editor markers, colored glass): a constants-only
        // sibling, not a failure — the FO76 effects sweep is full of these.
        MaterialBuilder markerBuilder;
        markerBuilder.header(BgemSig, 2, false, 1.0f);
        for (int i = 0; i < 5; ++i) markerBuilder.str(QString()); // v2: base/greyscale/envmap/normal/envmapMask
        markerBuilder.boolean(false); markerBuilder.boolean(false);
        markerBuilder.boolean(false); markerBuilder.boolean(false); markerBuilder.boolean(false); markerBuilder.boolean(false);
        markerBuilder.color(.2f, .5f, .9f); markerBuilder.f32(2.0f);
        markerBuilder.f32(0); markerBuilder.f32(0); markerBuilder.f32(0); markerBuilder.f32(0);
        markerBuilder.f32(0); markerBuilder.u8(0); markerBuilder.f32(0);
        const QString markerPath = QDir(convRoot.path()).filePath(QStringLiteral("materials/armor/marker.bgem"));
        QFile markerFile(markerPath);
        markerFile.open(QIODevice::WriteOnly);
        markerFile.write(markerBuilder.data);
        markerFile.close();
        const Converter::JobResult marker = Converter::convertMaterial(markerPath, jobOptions, &textureCache);
        check(marker.converted && marker.errors.isEmpty(),
            QStringLiteral("texture-free BGEM converts: %1").arg(marker.errors.join(QStringLiteral("; "))));
        Pbr::MaterialDocument markerDoc;
        const Pbr::TextureSlotState markerBase = markerDoc.load(marker.pbrmPath, nullptr)
            ? markerDoc.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryBaseColor")) : Pbr::TextureSlotState{};
        check(markerBase.enabled && markerBase.path.isEmpty()
            && markerBase.values.value(QStringLiteral("overrideColor")).toBool(false)
            && QColor(markerBase.values.value(QStringLiteral("color")).toString()).blue() > 200,
            QStringLiteral("constants-only sibling carries the effect color with no texture"));

        // ---- BA2-direct: a hand-built v7 GNRL archive (the FO76 layout is
        // identical to v1) carrying the marker BGEM, converted straight from
        // the archive.
        {
            QByteArray ba2;
            const auto putU32 = [&ba2](quint32 value) {
                for (int i = 0; i < 4; ++i) ba2.append(char((value >> (8 * i)) & 0xFF));
            };
            const auto putU64 = [&ba2](quint64 value) {
                for (int i = 0; i < 8; ++i) ba2.append(char((value >> (8 * i)) & 0xFF));
            };
            const QByteArray entryName = QByteArrayLiteral("materials\\effects\\ba2marker.bgem");
            const QByteArray payload = markerBuilder.data;
            const quint64 dataOffset = 24 + 36;                 // header + one GNRL record
            const quint64 namesOffset = dataOffset + quint64(payload.size());
            ba2.append("BTDX");
            putU32(7);                                          // FO76 version, same layout
            ba2.append("GNRL");
            putU32(1);                                          // one file
            putU64(namesOffset);
            putU32(0); putU32(0); putU32(0); putU32(0);         // hashes/ext (unused by the reader)
            putU64(dataOffset);
            putU32(0);                                          // packed = 0 (stored)
            putU32(quint32(payload.size()));
            putU32(0xBAADF00Du);                                // alignment field
            ba2.append(payload);
            ba2.append(char(entryName.size() & 0xFF));
            ba2.append(char((entryName.size() >> 8) & 0xFF));
            ba2.append(entryName);
            const QString ba2Path = QDir(convRoot.path()).filePath(QStringLiteral("test - Materials.ba2"));
            QFile ba2File(ba2Path);
            ba2File.open(QIODevice::WriteOnly);
            ba2File.write(ba2);
            ba2File.close();

            QString archiveError;
            const QStringList archiveMaterials = TextureArchive::entryPaths(ba2Path, QStringLiteral(".bgem"), &archiveError);
            check(archiveMaterials.size() == 1 && archiveMaterials.first().compare(
                    QStringLiteral("materials/effects/ba2marker.bgem"), Qt::CaseInsensitive) == 0,
                QStringLiteral("v7 GNRL archive enumerates its material: %1").arg(archiveError));
            const QByteArray extracted = TextureArchive::extract(ba2Path, archiveMaterials.value(0), &archiveError);
            check(extracted == payload, QStringLiteral("archive extraction round-trips the material bytes"));
            const Converter::JobResult fromArchive = Converter::convertMaterialData(ba2Path, archiveMaterials.value(0),
                extracted, jobOptions, &textureCache);
            check(fromArchive.converted && fromArchive.errors.isEmpty()
                && fromArchive.pbrmPath.endsWith(QStringLiteral("Materials/effects/ba2marker.pbrm")),
                QStringLiteral("archive-direct conversion writes the materials-relative sibling: %1")
                    .arg(fromArchive.errors.join(QStringLiteral("; "))));
        }

        // ---- Correction-mask fills (docs/FO4-Conversion.md masking): two
        // separate quads weld into two elements; a fill rasterizes only its
        // own UV footprint; the saved mask forces the conversion's metallic.
        {
            PreviewMesh twoQuads;
            const auto addQuad = [&twoQuads](float xOffset, float uOffset) {
                const int base = int(twoQuads.vertices.size());
                for (int corner = 0; corner < 4; ++corner) {
                    PreviewVertex vertex;
                    const float x = (corner == 1 || corner == 2) ? 1.0f : 0.0f;
                    const float y = (corner >= 2) ? 1.0f : 0.0f;
                    vertex.position = QVector3D(xOffset + x, y, 0);
                    vertex.uv0 = QVector2D(uOffset + x * 0.4f, y);
                    twoQuads.vertices.append(vertex);
                }
                for (quint32 index : {0u, 1u, 2u, 0u, 2u, 3u}) twoQuads.indices.append(quint32(base) + index);
            };
            addQuad(0.0f, 0.05f);   // left quad -> left UV half
            addQuad(5.0f, 0.55f);   // right quad, far away -> right UV half
            const QVector<int> elements = MaskPaint::triangleElements(twoQuads);
            check(elements.size() == 4 && elements[0] == elements[1] && elements[2] == elements[3]
                && elements[0] != elements[2],
                QStringLiteral("position weld splits the quads into two elements"));
            const QVector<quint32> leftTriangles = MaskPaint::trianglesInElement(twoQuads, elements, 0);
            check(leftTriangles == QVector<quint32>({0u, 1u}),
                QStringLiteral("element fill collects exactly the connected triangles"));
            QImage mask = MaskPaint::neutralMask(128);
            MaskPaint::rasterizeFill(mask, twoQuads, leftTriangles, 0.0f);
            check(qGray(mask.pixel(32, 64)) < 40 && qGray(mask.pixel(96, 64)) > 100,
                QStringLiteral("fill rasterizes its own UV footprint and leaves the rest neutral"));

            // Full-dielectric mask over the synthetic metal: coverage collapses.
            QImage forceDielectric = MaskPaint::neutralMask(64);
            forceDielectric.fill(QColor(0, 0, 0, 255));
            const QString maskPath = QDir(convRoot.path()).filePath(QStringLiteral("force_dielectric.png"));
            forceDielectric.save(maskPath);
            Converter::JobOptions maskedOptions = jobOptions;
            maskedOptions.correctionMaskPath = maskPath;
            const Converter::JobResult masked = Converter::convertMaterial(buildConvMaterial(QStringLiteral("conv4.bgsm")),
                maskedOptions, &textureCache);
            check(masked.converted && masked.metalCoverage < 0.05f && first.metalCoverage > 0.9f,
                QStringLiteral("correction mask forces the inferred metal to dielectric (%1 -> %2)")
                    .arg(first.metalCoverage).arg(masked.metalCoverage));
        }

        // ---- Geometry AO: a plate under a lid reads occluded; alone it
        // reads open. (CPU depth-map bake, docs/FO4-Conversion.md.)
        {
            const auto plate = [](float y, bool facingUp, float uMin, float uMax) {
                QVector<PreviewVertex> vertices;
                for (int corner = 0; corner < 4; ++corner) {
                    PreviewVertex vertex;
                    const float x = (corner == 1 || corner == 2) ? 1.0f : -1.0f;
                    const float z = (corner >= 2) ? 1.0f : -1.0f;
                    vertex.position = QVector3D(x, y, z);
                    vertex.normal = QVector3D(0, facingUp ? 1.0f : -1.0f, 0);
                    vertex.uv0 = QVector2D(uMin + (x * 0.5f + 0.5f) * (uMax - uMin), z * 0.5f + 0.5f);
                    vertices.append(vertex);
                }
                return vertices;
            };
            const auto addPlate = [](PreviewMesh& mesh, const QVector<PreviewVertex>& vertices, bool flip) {
                const quint32 base = quint32(mesh.vertices.size());
                mesh.vertices += vertices;
                const quint32 order[6] = {0, 1, 2, 0, 2, 3};
                const quint32 flipped[6] = {0, 2, 1, 0, 3, 2};
                for (quint32 index : (flip ? flipped : order)) mesh.indices.append(base + index);
            };
            GeometryAO::Options aoOptions;
            aoOptions.mapSize = 64;
            aoOptions.depthSize = 128;
            aoOptions.directions = 48;
            aoOptions.dilate = 0;

            PreviewMesh lone;
            addPlate(lone, plate(0.0f, true, 0.05f, 0.45f), false);
            lone.radius = 1.5f;
            const QImage loneAo = GeometryAO::bake(lone, aoOptions);
            check(!loneAo.isNull() && qGray(loneAo.pixel(16, 32)) > 215,
                QStringLiteral("a lone plate bakes open geometry AO (%1)").arg(qGray(loneAo.pixel(16, 32))));

            PreviewMesh covered = lone;
            addPlate(covered, plate(0.35f, false, 0.55f, 0.95f), true); // lid close above, facing down
            const QImage coveredAo = GeometryAO::bake(covered, aoOptions);
            check(!coveredAo.isNull() && qGray(coveredAo.pixel(16, 32)) < 160,
                QStringLiteral("a lid above the plate darkens its geometry AO (%1)").arg(qGray(coveredAo.pixel(16, 32))));
        }
    }

    {
        // Signed BC4/BC5 (SNORM) DDS decode: FO76 normal maps ship as
        // BC5_SNORM; the unsigned path would fold negative components into
        // garbage. One 4x4 block per channel, extremes pinned.
        QByteArray snorm;
        snorm.append("DDS ");
        auto appendU32 = [&snorm](quint32 value) {
            for (int i = 0; i < 4; ++i) snorm.append(char((value >> (8 * i)) & 0xFF));
        };
        appendU32(124);
        appendU32(0x1007); // caps/height/width/pixelformat flags
        appendU32(4); appendU32(4); // height, width
        while (snorm.size() < 76) snorm.append('\0');
        appendU32(32); appendU32(4); // pixel format size, FOURCC flag
        snorm.append("DX10");
        while (snorm.size() < 128) snorm.append('\0');
        appendU32(84); // DXGI_FORMAT_BC5_SNORM
        appendU32(3); appendU32(0); appendU32(1); appendU32(0); // dimension/misc/arraySize/misc2
        // Red block: e0=+127, e1=-127 (e0>e1 ramp), all indices 0 -> +1.0.
        snorm.append(char(127)); snorm.append(char(-127));
        for (int i = 0; i < 6; ++i) snorm.append('\0');
        // Green block: e0=-127, e1=+127 (e0<=e1 specials), all indices 7 -> +1.0.
        snorm.append(char(-127)); snorm.append(char(127));
        for (int i = 0; i < 6; ++i) snorm.append(char(0xFF));
        QTemporaryDir snormDir;
        QDir(snormDir.path()).mkpath(QStringLiteral("textures"));
        QFile snormFile(QDir(snormDir.path()).filePath(QStringLiteral("textures/signed_n.dds")));
        snormFile.open(QIODevice::WriteOnly);
        snormFile.write(snorm);
        snormFile.close();
        QString snormError;
        const QImage signedImage = TextureImage::load(QStringLiteral("signed_n.dds"), snormDir.path() + QStringLiteral("/textures"), &snormError);
        check(!signedImage.isNull(), QStringLiteral("BC5_SNORM DDS decodes: %1").arg(snormError));
        if (!signedImage.isNull()) {
            const QColor texel = signedImage.pixelColor(0, 0);
            check(texel.red() == 255 && texel.green() == 255,
                QStringLiteral("SNORM +1.0 endpoints decode to full channels (%1 %2)").arg(texel.red()).arg(texel.green()));
        }
    }

    {
        // Conversion math core: the per-texel functions are the contract the
        // whole converter stands on — pin them.
        const Converter::Options options;

        // Gloss/smoothness inversion.
        check(qFuzzyIsNull(Converter::glossToRoughness(1.0f, options))
            && qFuzzyCompare(Converter::glossToRoughness(0.0f, options), 1.0f),
            QStringLiteral("gloss 1 -> roughness 0 and gloss 0 -> roughness 1"));

        // FO4 metallic inference: needs env mapping AND bright spec AND dark
        // diffuse; each missing leg kills it.
        check(Converter::metallicConfidenceFo4(0.9f, 0.05f, true, 1.0f, 1.0f, options) > 0.9f,
            QStringLiteral("FO4 inference fires on envmap + bright spec + dark diffuse"));
        check(qFuzzyIsNull(Converter::metallicConfidenceFo4(0.9f, 0.05f, false, 1.0f, 1.0f, options)),
            QStringLiteral("FO4 inference is dead without environment mapping"));
        check(qFuzzyIsNull(Converter::metallicConfidenceFo4(0.05f, 0.05f, true, 1.0f, 1.0f, options)),
            QStringLiteral("FO4 inference is dead without spec strength"));
        check(qFuzzyIsNull(Converter::metallicConfidenceFo4(0.9f, 0.6f, true, 1.0f, 1.0f, options)),
            QStringLiteral("FO4 inference is dead on a bright diffuse"));

        // FO76 inference: bright reflectance + black diffuse.
        check(Converter::metallicConfidenceFo76({0.7f, 0.6f, 0.45f}, 0.02f, options) > 0.9f,
            QStringLiteral("FO76 inference fires on bright reflectance over black diffuse"));
        check(qFuzzyIsNull(Converter::metallicConfidenceFo76({0.04f, 0.04f, 0.04f}, 0.4f, options)),
            QStringLiteral("FO76 inference is dead on dielectric reflectance"));

        // Painted correction: 0.5 neutral, extremes override fully.
        check(qFuzzyCompare(Converter::applyMetallicCorrection(0.8f, 0.5f), 0.8f)
            && qFuzzyIsNull(Converter::applyMetallicCorrection(0.8f, 0.0f))
            && qFuzzyCompare(Converter::applyMetallicCorrection(0.1f, 1.0f), 1.0f),
            QStringLiteral("metallic corrections respect the 0.5-neutral contract"));

        // Metal albedo recovery: a black diffuse takes its hue from the
        // cubemap tint (the vault-of-gold case); a hued dark diffuse keeps
        // its own hue; brightness always comes from spec.
        const QVector3D goldTint(0.6f, 0.45f, 0.2f);
        const QVector3D fromBlack = Converter::recoverMetalAlbedoFo4({0.001f, 0.001f, 0.001f}, 1.0f, goldTint, true, options);
        check(fromBlack.x() > fromBlack.z() * 1.5f && fromBlack.x() > 0.4f,
            QStringLiteral("black-diffuse metal recovers the cubemap's warm hue at metal brightness"));
        const QVector3D fromHue = Converter::recoverMetalAlbedoFo4({0.25f, 0.03f, 0.03f}, 1.0f, goldTint, true, options);
        check(fromHue.x() > fromHue.y() * 2.0f,
            QStringLiteral("a hued dark diffuse keeps its own hue through recovery"));
        const QVector3D dim = Converter::recoverMetalAlbedoFo4({0.25f, 0.03f, 0.03f}, 0.0f, goldTint, true, options);
        check(Converter::luminance(dim) < Converter::luminance(fromHue),
            QStringLiteral("spec strength drives recovered brightness"));

        // Saturation clamp: sky-blue scene cubes cannot tint metals.
        const QVector3D clamped = Converter::clampTintSaturation({0.2f, 0.4f, 0.9f}, 0.2f);
        const float clampedLuma = Converter::luminance(clamped);
        check(std::abs(clamped.z() - clampedLuma) < std::abs(0.9f - Converter::luminance(QVector3D(0.2f, 0.4f, 0.9f))),
            QStringLiteral("tint saturation clamp pulls scene colors toward neutral"));

        // Image-level FO4 conversion: half dielectric red plastic, half
        // authored-black metal under a warm cubemap.
        QImage diffuse(8, 8, QImage::Format_RGBA8888);
        QImage specular(8, 8, QImage::Format_RGBA8888);
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                const bool metal = x >= 4;
                diffuse.setPixelColor(x, y, metal ? QColor(2, 2, 2) : QColor(180, 40, 40));
                // R spec strength, G gloss.
                specular.setPixelColor(x, y, metal ? QColor(230, 200, 0) : QColor(30, 60, 0));
            }
        Bgsm::Material fo4Material;
        fo4Material.environmentMapping = true;
        fo4Material.environmentMappingMaskScale = 1.0f;
        fo4Material.specularMult = 1.0f;
        Converter::Fo4Sources fo4Sources;
        fo4Sources.diffuse = diffuse;
        fo4Sources.specular = specular;
        fo4Sources.cubemapTint = goldTint;
        fo4Sources.hasCubemapTint = true;
        const Converter::ConvertedImages fo4Out = Converter::convertFo4(fo4Sources, fo4Material, options);
        check(!fo4Out.baseColor.isNull() && fo4Out.metalCoverage > 0.4f && fo4Out.metalCoverage < 0.6f,
            QStringLiteral("FO4 conversion reports the authored metal coverage"));
        const QColor metalBase = fo4Out.baseColor.pixelColor(6, 4);
        const QColor plasticBase = fo4Out.baseColor.pixelColor(1, 4);
        check(metalBase.redF() > 0.4 && metalBase.redF() > metalBase.blueF(),
            QStringLiteral("FO4 black metal converts to a bright warm base color"));
        check(plasticBase.redF() > 0.5 && qFuzzyCompare(float(plasticBase.greenF()), float(QColor(180, 40, 40).greenF())),
            QStringLiteral("FO4 dielectric base color passes through untouched"));
        const QColor metalRmaos = QColor(fo4Out.rmaos.pixelColor(6, 4));
        const QColor plasticRmaos = QColor(fo4Out.rmaos.pixelColor(1, 4));
        check(metalRmaos.greenF() > 0.9 && plasticRmaos.greenF() < 0.05,
            QStringLiteral("FO4 metallic channel splits the halves"));
        check(metalRmaos.redF() < plasticRmaos.redF(),
            QStringLiteral("FO4 glossy metal converts smoother than dull plastic"));

        // Image-level FO76 conversion: gold over black diffuse with _l data.
        QImage diffuse76(4, 4, QImage::Format_RGBA8888);
        QImage reflectance76(4, 4, QImage::Format_RGBA8888);
        QImage lighting76(4, 4, QImage::Format_RGBA8888);
        diffuse76.fill(QColor(3, 3, 3));
        reflectance76.fill(QColor(185, 167, 123));
        lighting76.fill(QColor(160, 200, 0)); // R smoothness .63, G AO .78
        Bgsm::Material fo76Material;
        fo76Material.version = 20;
        Converter::Fo76Sources fo76Sources;
        fo76Sources.diffuse = diffuse76;
        fo76Sources.reflectance = reflectance76;
        fo76Sources.lighting = lighting76;
        const Converter::ConvertedImages fo76Out = Converter::convertFo76(fo76Sources, fo76Material, options);
        const QColor goldBase = fo76Out.baseColor.pixelColor(2, 2);
        // sRGB encoding compresses channel ratios; assert the warm ordering
        // rather than a linear-space ratio.
        check(fo76Out.metalCoverage > 0.99f && goldBase.redF() > 0.6
            && goldBase.redF() > goldBase.greenF() && goldBase.greenF() > goldBase.blueF()
            && goldBase.redF() - goldBase.blueF() > 0.1,
            QStringLiteral("FO76 black-diffuse gold reconstructs its color from the reflectance map"));
        const QColor goldRmaos = fo76Out.rmaos.pixelColor(2, 2);
        check(goldRmaos.greenF() > 0.95 && std::abs(goldRmaos.blueF() - 200.0 / 255.0) < 0.02
            && std::abs(goldRmaos.redF() - (1.0 - 160.0 / 255.0)) < 0.02,
            QStringLiteral("FO76 RMAOS carries metallic, the shipped AO, and inverted smoothness"));

        // Emissive rule: grayscale glow x material color, mask in alpha.
        QImage glow(2, 2, QImage::Format_RGBA8888);
        glow.fill(QColor(128, 128, 128));
        fo4Material.emitEnabled = true;
        fo4Material.emittanceColor = QVector3D(1.0f, 0.5f, 0.0f);
        fo4Sources.glow = glow;
        const Converter::ConvertedImages emissiveOut = Converter::convertFo4(fo4Sources, fo4Material, options);
        const QColor emissive = emissiveOut.emissive.pixelColor(0, 0);
        check(!emissiveOut.emissive.isNull() && std::abs(emissive.redF() - 0.5) < 0.01
            && std::abs(emissive.greenF() - 0.25) < 0.01 && qFuzzyIsNull(float(emissive.blueF()))
            && std::abs(emissive.alphaF() - 0.5) < 0.01,
            QStringLiteral("FO4 emissive bakes mask x material color with the raw mask in alpha"));

        // Parameter mapping: the four-fate audit's mapped fates.
        Bgsm::Material mapped;
        mapped.alphaTest = true;
        mapped.alphaTestRef = 96;
        mapped.twoSided = true;
        mapped.modelSpaceNormals = true;
        mapped.refraction = true;
        mapped.refractionPower = 0.4f;
        mapped.customPorosity = true;
        mapped.porosityValue = 0.7f;
        mapped.grayscaleToPaletteColor = true;
        Pbr::MaterialDocument converted;
        QStringList unhonoredFields;
        Converter::mapParameters(mapped, &converted, &unhonoredFields);
        check(converted.setting(QStringLiteral("Transparency/Composition"), QStringLiteral("mode")).toString() == QStringLiteral("Alpha Test")
            && std::abs(converted.setting(QStringLiteral("Transparency/Composition"), QStringLiteral("threshold")).toDouble() - 96.0 / 255.0) < 1e-6,
            QStringLiteral("alpha test maps to composition mode + threshold"));
        check(converted.setting(QStringLiteral("Shading/Core"), QStringLiteral("twoSided")).toBool()
            && converted.setting(QStringLiteral("Shading/Core"), QStringLiteral("normalSpace")).toString() == QStringLiteral("Model"),
            QStringLiteral("two-sided and model-space normals map to core shading"));
        check(converted.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryScreenDistortion")).enabled,
            QStringLiteral("refraction maps to the Screen Distortion input"));
        const QJsonObject porosityValues = converted.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryRmaos")).values;
        check(porosityValues.value(QStringLiteral("overridePorosity")).toBool(false)
            && std::abs(porosityValues.value(QStringLiteral("porosity")).toDouble() - 0.7) < 1e-6,
            QStringLiteral("FO76 vanilla porosity maps onto the v5 porosity constant"));
        check(!unhonoredFields.isEmpty(),
            QStringLiteral("unrepresentable fields are reported, never silently dropped"));
        Bgsm::Material effectMapped;
        effectMapped.kind = Bgsm::Kind::Bgem;
        effectMapped.effectLightingEnabled = false;
        effectMapped.baseColorScale = 6.0f;
        Pbr::MaterialDocument effectConverted;
        Converter::mapParameters(effectMapped, &effectConverted, nullptr);
        check(effectConverted.shaderMode() == Pbr::ShaderMode::UnlitEffect
            && std::abs(effectConverted.setting(QStringLiteral("Shading/Unlit"), QStringLiteral("intensity")).toDouble() - 6.0) < 1e-6,
            QStringLiteral("unlit BGEM maps its diffuse-as-emission scale to Unlit intensity"));
    }

    {
        // Normal-map derivations: a synthetic pit (inverted Gaussian) has
        // fully known signs — normals converge toward the center, the
        // reconstructed height dips, AO darkens inside, and signed curvature
        // reads concave in the bowl.
        const int size = 96;
        const auto makeField = [size](float amplitude) {
            QImage normal(size, size, QImage::Format_RGBA8888);
            const float sigma = size / 7.0f;
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x) {
                    const float dx = x - size / 2.0f, dy = y - size / 2.0f;
                    const float r2 = dx * dx + dy * dy;
                    const float gaussian = amplitude * std::exp(-r2 / (2.0f * sigma * sigma));
                    // h = -gaussian (a pit for positive amplitude); slope = dh.
                    const float gx = gaussian * dx / (sigma * sigma);
                    const float gy = gaussian * dy / (sigma * sigma);
                    // n = normalize(-gx, -gy, 1)
                    const float inverseLength = 1.0f / std::sqrt(gx * gx + gy * gy + 1.0f);
                    const float nx = -gx * inverseLength, ny = -gy * inverseLength;
                    normal.setPixelColor(x, y, QColor(
                        int(std::lround((nx * 0.5f + 0.5f) * 255.0f)),
                        int(std::lround((ny * 0.5f + 0.5f) * 255.0f)),
                        int(std::lround((inverseLength * 0.5f + 0.5f) * 255.0f))));
                }
            return normal;
        };
        const QImage pitNormal = makeField(6.0f);
        NormalDerive::Options deriveOptions;
        deriveOptions.derivationSize = 96;
        deriveOptions.autoDenoise = false;
        const NormalDerive::Results pit = NormalDerive::derive(pitNormal, deriveOptions);
        check(!pit.curvature.isNull() && !pit.height.isNull() && !pit.ao.isNull(),
            QStringLiteral("derivations produce curvature, height, and AO"));
        const int center = size / 2, corner = 6;
        check(pit.height.pixelColor(center, center).redF() < pit.height.pixelColor(corner, corner).redF() - 0.2,
            QStringLiteral("Poisson height reconstructs the pit below the surrounding surface"));
        check(pit.ao.pixelColor(center, center).redF() < pit.ao.pixelColor(corner, corner).redF() - 0.05,
            QStringLiteral("horizon AO darkens inside the pit and stays open on the flat"));
        check(pit.curvature.pixelColor(center, center).redF() > 0.5,
            QStringLiteral("pit floor reads concave (curvature above the 0.5 neutral)"));
        check(std::abs(pit.curvature.pixelColor(corner, corner).redF() - 0.5) < 0.02,
            QStringLiteral("flat regions stay at the signed-curvature neutral"));
        // The dome is the mirror: convex crown, no self-occlusion at the top.
        const QImage domeNormal = makeField(-6.0f);
        const NormalDerive::Results dome = NormalDerive::derive(domeNormal, deriveOptions);
        check(dome.curvature.pixelColor(center, center).redF() < 0.5,
            QStringLiteral("dome crown reads convex (curvature below neutral)"));
        check(dome.height.pixelColor(center, center).redF() > dome.height.pixelColor(corner, corner).redF() + 0.2,
            QStringLiteral("Poisson height reconstructs the dome above the surrounding surface"));

        // Noise metric and derivation-side denoise: checkerboard XY noise
        // scores far above a clean gradient and cleans up measurably.
        QImage noisy = pitNormal;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                QColor texel = noisy.pixelColor(x, y);
                // Hash jitter approximates compression garbage (a perfect
                // checkerboard would be both the median's and the bilateral's
                // blind spot, and real noise is not phase-perfect).
                const int jitter = int((uint(x) * 73u ^ uint(y) * 151u) % 29u) - 14;
                texel.setRed(std::clamp(texel.red() + jitter, 0, 255));
                texel.setGreen(std::clamp(texel.green() - jitter, 0, 255));
                noisy.setPixelColor(x, y, texel);
            }
        const float cleanMetric = NormalDerive::noiseMetric(pitNormal);
        const float noisyMetric = NormalDerive::noiseMetric(noisy);
        check(noisyMetric > cleanMetric * 3.0f && noisyMetric > deriveOptions.denoiseThreshold,
            QStringLiteral("the noise metric separates dithered normals from clean ones"));
        NormalDerive::Options medianOptions = deriveOptions;
        medianOptions.forceMedian = true;
        const QImage cleaned = NormalDerive::denoiseNormal(noisy, medianOptions);
        check(NormalDerive::noiseMetric(cleaned) < noisyMetric * 0.5f,
            QStringLiteral("median + bilateral denoising removes most of the dither energy"));
        deriveOptions.autoDenoise = true;
        const NormalDerive::Results denoisedRun = NormalDerive::derive(noisy, deriveOptions);
        check(denoisedRun.denoised && denoisedRun.noiseMetric > deriveOptions.denoiseThreshold,
            QStringLiteral("derive() auto-gates the derivation-side denoise on the metric"));
    }

    {
        // Shared legacy-data helpers: the groups-only strip (the validation
        // quick-fix scope) preserves response slots and retired keys; the
        // full strip removes everything.
        Pbr::MaterialDocument legacyDoc;
        legacyDoc.setSetting(QStringLiteral("SurfaceStates/State0"), QStringLiteral("name"), QStringLiteral("Wetness"));
        Pbr::TextureSlotState responseSlot;
        responseSlot.enabled = true;
        responseSlot.path = QStringLiteral("foo_response.dds");
        legacyDoc.setTextureSlot(QStringLiteral("UV0"), QStringLiteral("primarySurfaceStateResponse"), responseSlot);
        Pbr::TextureSlotState legacyNormal;
        legacyNormal.enabled = true;
        legacyNormal.values.insert(QStringLiteral("layerMaskInAlpha"), true);
        legacyDoc.setTextureSlot(QStringLiteral("UV0"), QStringLiteral("primaryNormal"), legacyNormal);
        check(Pbr::hasLegacySurfaceStateGroups(legacyDoc), QStringLiteral("legacy group detection sees authored slots"));
        Pbr::MaterialDocument groupsOnly = legacyDoc;
        check(Pbr::stripLegacySurfaceStateGroups(groupsOnly) == 1
            && !Pbr::hasLegacySurfaceStateGroups(groupsOnly)
            && groupsOnly.textureSet(QStringLiteral("UV0")).contains(QStringLiteral("primarySurfaceStateResponse"))
            && groupsOnly.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryNormal")).values.contains(QStringLiteral("layerMaskInAlpha")),
            QStringLiteral("groups-only strip keeps the response slot and retired keys"));
        Pbr::MaterialDocument fullStrip = legacyDoc;
        check(Pbr::stripLegacySurfaceData(fullStrip) == 3
            && !fullStrip.textureSet(QStringLiteral("UV0")).contains(QStringLiteral("primarySurfaceStateResponse"))
            && !fullStrip.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryNormal")).values.contains(QStringLiteral("layerMaskInAlpha")),
            QStringLiteral("full strip removes groups, response slots, and retired keys"));
        const QString extractTarget = directory.filePath(QStringLiteral("helper-extract.json"));
        QString helperError;
        check(Pbr::extractLegacyStates(legacyDoc, extractTarget, &helperError) && QFileInfo::exists(extractTarget),
            QStringLiteral("extract helper writes the definitions sidecar: %1").arg(helperError));
    }

    {
        // Packer mip discipline: plain mean-preserving averages for packed
        // data channels, and Toksvig variance folding into the roughness mips.
        QImage packed(8, 8, QImage::Format_RGBA8888);
        QImage toksvigNormals(8, 8, QImage::Format_RGBA8888);
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                // R roughness 0.2 everywhere; A splits 64/192 so the mip mean
                // must stay 128 (the curvature mean-preservation rule).
                packed.setPixelColor(x, y, QColor(51, 128, 255, x < 4 ? 64 : 192));
                // Left half flat; right half alternates +X/-X normals so the
                // averaged vector collapses and Toksvig must raise roughness.
                const bool tilt = x >= 4 && ((x + y) & 1);
                toksvigNormals.setPixelColor(x, y, tilt ? QColor(255, 128, 128) : (x >= 4 ? QColor(0, 128, 128) : QColor(128, 128, 255)));
            }
        const QString ddsFile = directory.filePath(QStringLiteral("toksvig.dds"));
        QString ddsError;
        check(TextureWriter::saveDds(packed, ddsFile, TextureWriter::DdsFormat::Rgba8, true, &ddsError, toksvigNormals),
            QStringLiteral("uncompressed DDS with Toksvig mips saves: %1").arg(ddsError));
        QFile ddsIn(ddsFile);
        check(ddsIn.open(QIODevice::ReadOnly), QStringLiteral("packed DDS reopens"));
        const QByteArray dds = ddsIn.readAll();
        // Legacy 128-byte header, then mip0 (8x8x4); mip1 (4x4x4) follows.
        const int mip1 = 128 + 8 * 8 * 4;
        check(dds.size() >= mip1 + 4 * 4 * 4, QStringLiteral("DDS carries the generated mip chain"));
        double alphaSum = 0, flatRough = 0, noisyRough = 0;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                const uchar* texel = reinterpret_cast<const uchar*>(dds.constData()) + mip1 + (y * 4 + x) * 4;
                alphaSum += texel[3];
                if (x < 2) flatRough += texel[0]; else noisyRough += texel[0];
            }
        check(std::abs(alphaSum / 16.0 - 128.0) <= 2.0,
            QStringLiteral("packed alpha (curvature) mips stay mean-preserving"));
        check(flatRough / 8.0 <= 52.0 && noisyRough / 8.0 >= 90.0,
            QStringLiteral("Toksvig raises roughness only where normal variance was averaged away"));
    }

    {
        // Normal mips average decoded vectors rather than encoded bytes. Two
        // +X and two +Y normals have a normalized mean of (.707, .707, 0),
        // which encodes near (218, 218), not the byte average (192, 192).
        QImage normals(2, 2, QImage::Format_RGBA8888);
        normals.setPixelColor(0, 0, QColor(255, 128, 0, 64));
        normals.setPixelColor(1, 0, QColor(128, 255, 255, 192));
        normals.setPixelColor(0, 1, QColor(255, 128, 0, 64));
        normals.setPixelColor(1, 1, QColor(128, 255, 255, 192));
        const QString normalDdsFile = directory.filePath(QStringLiteral("vector-filtered-normal.dds"));
        QString normalDdsError;
        check(TextureWriter::saveDds(normals, normalDdsFile, TextureWriter::DdsFormat::Rgba8,
            true, &normalDdsError, QImage(), true),
            QStringLiteral("vector-filtered normal DDS saves: %1").arg(normalDdsError));
        QFile normalDdsIn(normalDdsFile);
        check(normalDdsIn.open(QIODevice::ReadOnly), QStringLiteral("vector-filtered normal DDS reopens"));
        const QByteArray normalDds = normalDdsIn.readAll();
        const int normalMip1 = 128 + 2 * 2 * 4;
        const uchar* baseTexel = reinterpret_cast<const uchar*>(normalDds.constData()) + 128;
        const uchar* filteredTexel = reinterpret_cast<const uchar*>(normalDds.constData()) + normalMip1;
        check(normalDds.size() >= normalMip1 + 4 && baseTexel[0] == 255 && baseTexel[1] == 128,
            QStringLiteral("vector filtering leaves normal mip 0 unchanged"));
        check(filteredTexel[0] >= 216 && filteredTexel[0] <= 220
            && filteredTexel[1] >= 216 && filteredTexel[1] <= 220,
            QStringLiteral("normal mip direction is vector-averaged and renormalized"));
        check(std::abs(int(filteredTexel[2]) - 128) <= 1 && std::abs(int(filteredTexel[3]) - 128) <= 1,
            QStringLiteral("normal mip auxiliary channels remain mean-preserving"));
    }

    const QStringList goldenNames = {
        QStringLiteral("minimal-standard-surface.json"),
        QStringLiteral("secondary-uv.json"),
        QStringLiteral("landscape.json"),
        QStringLiteral("effect-particle.json"),
        QStringLiteral("advanced-authored.json"),
        QStringLiteral("tertiary-uv.json")
    };
    for (const QString& goldenName : goldenNames) {
        const QJsonObject expected = readFixture(goldenName);
        check(!expected.isEmpty(), QStringLiteral("golden fixture is available: %1").arg(goldenName));
        const QString fixtureFile = directory.filePath(goldenName + QStringLiteral(".pbrm"));
        QFile rawFixture(fixtureFile);
        const QByteArray expectedEnvelope = envelopeFor(expected);
        check(rawFixture.open(QIODevice::WriteOnly) && rawFixture.write(expectedEnvelope) == expectedEnvelope.size(),
            QStringLiteral("golden PBRM envelope can be created: %1").arg(goldenName));
        rawFixture.close();

        Pbr::MaterialDocument golden;
        check(golden.load(fixtureFile, &error), QStringLiteral("golden PBRM parses: %1 (%2)").arg(goldenName, error));
        check(golden.toJson() == expected, QStringLiteral("golden PBRM maps to the exact typed JSON contract: %1").arg(goldenName));

        const QString savedFixture = directory.filePath(QStringLiteral("saved-") + goldenName + QStringLiteral(".pbrm"));
        check(golden.save(savedFixture, &error), QStringLiteral("golden PBRM saves after parsing: %1 (%2)").arg(goldenName, error));
        QFile saved(savedFixture);
        check(saved.open(QIODevice::ReadOnly), QStringLiteral("saved golden envelope can be read: %1").arg(goldenName));
        const QByteArray savedPayload = payloadFromEnvelope(saved.readAll());
        check(savedPayload == QJsonDocument(expected).toJson(QJsonDocument::Compact),
            QStringLiteral("golden PBRM round-trip payload is exact: %1").arg(goldenName));
    }

    QJsonObject copiedUv = loaded.textureSet(QStringLiteral("UV0"));
    loaded.setTextureSet(QStringLiteral("UV1"), copiedUv);
    check(loaded.textureSlot(QStringLiteral("UV1"), QStringLiteral("primaryNormal")).path == normal.path,
        QStringLiteral("complete UV texture sets can be copied"));
    loaded.clearTextureSet(QStringLiteral("UV1"));
    check(loaded.textureSet(QStringLiteral("UV1")).isEmpty(), QStringLiteral("complete UV texture sets can be reset"));
    loaded.removeTextureSlot(QStringLiteral("UV0"), QStringLiteral("primaryNormal"));
    check(loaded.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryNormal")).path.isNull(),
        QStringLiteral("texture input reset removes its authored override"));
    loaded.setSettingGroup(QStringLiteral("Test/Group"), QJsonObject{{QStringLiteral("value"), 7}});
    check(loaded.settingGroup(QStringLiteral("Test/Group")).value(QStringLiteral("value")).toInt() == 7,
        QStringLiteral("whole setting groups can be assigned"));
    loaded.clearSettingGroup(QStringLiteral("Test/Group"));
    check(loaded.settingGroup(QStringLiteral("Test/Group")).isEmpty(), QStringLiteral("setting groups reset to implicit defaults"));

    {
        const QString swapBase = directory.filePath(QStringLiteral("swap_base.png"));
        const QString swapSource = directory.filePath(QStringLiteral("swap_source.png"));
        QImage baseImage(8, 8, QImage::Format_RGBA8888);
        baseImage.fill(QColor(200, 40, 90, 255));
        QImage sourceImage(8, 8, QImage::Format_RGBA8888);
        sourceImage.fill(QColor(30, 210, 120, 255));
        check(baseImage.save(swapBase) && sourceImage.save(swapSource),
            QStringLiteral("channel-swap fixture textures can be created"));

        Pbr::TextureSlotState swapState;
        swapState.enabled = true;
        swapState.path = swapBase;
        swapState.values.insert(QStringLiteral("swapG"), QJsonObject{
            {QStringLiteral("path"), swapSource}, {QStringLiteral("channel"), QStringLiteral("R")},
            {QStringLiteral("invert"), true}});
        check(TextureCache::hasChannelSwaps(swapState.values), QStringLiteral("channel swaps are detected in slot values"));
        const QImage packed = TextureCache::packedImage(swapState, QString());
        const QColor packedColor = packed.isNull() ? QColor() : packed.pixelColor(4, 4);
        check(packedColor == QColor(200, 225, 90, 255),
            QStringLiteral("packed image keeps main channels and swaps the redirected one inverted (got %1,%2,%3)")
                .arg(packedColor.red()).arg(packedColor.green()).arg(packedColor.blue()));

        QString writeError;
        const QString bakedBc7 = directory.filePath(QStringLiteral("swap_baked_bc7.dds"));
        check(TextureWriter::saveDds(packed, bakedBc7, TextureWriter::DdsFormat::Bc7, true, &writeError),
            QStringLiteral("packed image bakes to a BC7 DDS with mips (%1)").arg(writeError));
        QString reloadError;
        const QImage bc7Reloaded = TextureImage::load(bakedBc7, QString(), &reloadError);
        const QColor bc7Color = bc7Reloaded.isNull() ? QColor() : bc7Reloaded.pixelColor(4, 4);
        check(!bc7Reloaded.isNull() && bc7Reloaded.size() == packed.size()
                && qAbs(bc7Color.red() - 200) <= 8 && qAbs(bc7Color.green() - 225) <= 8 && qAbs(bc7Color.blue() - 90) <= 8,
            QStringLiteral("baked BC7 decodes back within compression tolerance (%1)").arg(reloadError));

        const QString bakedRaw = directory.filePath(QStringLiteral("swap_baked_raw.dds"));
        check(TextureWriter::saveDds(packed, bakedRaw, TextureWriter::DdsFormat::Rgba8, false, &writeError),
            QStringLiteral("packed image bakes to an uncompressed DDS (%1)").arg(writeError));
        const QImage rawReloaded = TextureImage::load(bakedRaw, QString(), &reloadError);
        check(!rawReloaded.isNull() && rawReloaded.pixelColor(4, 4) == packedColor,
            QStringLiteral("uncompressed DDS bake is bit-exact (%1)").arg(reloadError));

        const QString bakedBc5 = directory.filePath(QStringLiteral("swap_baked_bc5.dds"));
        check(TextureWriter::saveDds(packed, bakedBc5, TextureWriter::DdsFormat::Bc5, true, &writeError),
            QStringLiteral("packed image bakes to a BC5 DDS (%1)").arg(writeError));
        const QImage bc5Reloaded = TextureImage::load(bakedBc5, QString(), &reloadError);
        const QColor bc5Color = bc5Reloaded.isNull() ? QColor() : bc5Reloaded.pixelColor(4, 4);
        check(!bc5Reloaded.isNull() && qAbs(bc5Color.red() - 200) <= 4 && qAbs(bc5Color.green() - 225) <= 4,
            QStringLiteral("baked BC5 keeps its two channels within tolerance (%1)").arg(reloadError));
    }

    QFile corrupt(directory.filePath(QStringLiteral("corrupt.pbrm")));
    check(corrupt.open(QIODevice::WriteOnly), QStringLiteral("corrupt fixture can be created"));
    corrupt.write("NOPE\x04\0\0\0\0\0\0\0", 12);
    corrupt.close();
    Pbr::MaterialDocument rejected;
    check(!rejected.load(corrupt.fileName(), &error), QStringLiteral("invalid signature is rejected"));

    QJsonObject invalidJson = document.toJson();
    invalidJson.insert(QStringLiteral("schemaVersion"), 3);
    check(!rejected.fromJson(invalidJson, &error), QStringLiteral("mismatched JSON schema version is rejected"));
    invalidJson = document.toJson();
    invalidJson.insert(QStringLiteral("shader"), QStringLiteral("Unknown Surface"));
    check(!rejected.fromJson(invalidJson, &error), QStringLiteral("unknown shader family is rejected"));
    invalidJson = document.toJson();
    invalidJson.insert(QStringLiteral("primaryUv"), QJsonArray());
    check(!rejected.fromJson(invalidJson, &error), QStringLiteral("non-object material collections are rejected"));

	QJsonObject additiveJson = document.toJson();
	const QJsonObject requirements{{QStringLiteral("standard.clearcoat"), 1}};
	const QJsonObject futureMetadata{{QStringLiteral("tool"), QStringLiteral("Future Editor")},
	    {QStringLiteral("revision"), 7}};
	additiveJson.insert(QStringLiteral("requirements"), requirements);
	additiveJson.insert(QStringLiteral("futureMetadata"), futureMetadata);
	Pbr::MaterialDocument additiveDocument;
	check(additiveDocument.fromJson(additiveJson, &error),
	    QStringLiteral("known shaders accept additive v4 root fields"));
	const QJsonObject additiveRoundTrip = additiveDocument.toJson();
	check(additiveRoundTrip.value(QStringLiteral("requirements")).toObject() == requirements,
	    QStringLiteral("runtime capability requirements survive editor round-trip"));
	check(additiveRoundTrip.value(QStringLiteral("futureMetadata")).toObject() == futureMetadata,
	    QStringLiteral("unknown additive root metadata survives editor round-trip"));
	QJsonObject invalidRequirements = document.toJson();
	invalidRequirements.insert(QStringLiteral("requirements"),
	    QJsonObject{{QStringLiteral("standard.clearcoat"), 0.5}});
	check(!rejected.fromJson(invalidRequirements, &error),
	    QStringLiteral("non-integer capability requirements are rejected"));

    QFile trailing(directory.filePath(QStringLiteral("trailing.pbrm")));
    const QByteArray trailingBytes = envelopeFor(document.toJson()) + QByteArrayLiteral("extra");
    check(trailing.open(QIODevice::WriteOnly) && trailing.write(trailingBytes) == trailingBytes.size(),
        QStringLiteral("trailing-byte fixture can be created"));
    trailing.close();
    check(!rejected.load(trailing.fileName(), &error), QStringLiteral("bytes after the declared payload are rejected"));

    QByteArray dds(128 + 4, '\0');
    dds.replace(0, 4, "DDS ");
    const auto put32 = [&dds](int offset, quint32 value) { qToLittleEndian(value, reinterpret_cast<uchar*>(dds.data() + offset)); };
    put32(4, 124); put32(12, 1); put32(16, 1); put32(76, 32); put32(88, 32);
    put32(92, 0x00ff0000); put32(96, 0x0000ff00); put32(100, 0x000000ff); put32(104, 0xff000000);
    put32(128, 0xff3366cc);
    const QString ddsPath = directory.filePath(QStringLiteral("preview.dds"));
    QFile ddsFile(ddsPath);
    check(ddsFile.open(QIODevice::WriteOnly) && ddsFile.write(dds) == dds.size(), QStringLiteral("DDS preview fixture can be created"));
    ddsFile.close();
    const QImage preview = TextureImage::load(ddsPath, {}, &error);
    check(preview.size() == QSize(1, 1), QStringLiteral("uncompressed DDS thumbnail loads"));
    check(!preview.isNull() && preview.pixelColor(0, 0) == QColor(0x33, 0x66, 0xcc, 0xff), QStringLiteral("DDS thumbnail preserves RGBA channels"));
    const TextureImage::Diagnostics diagnostics = TextureImage::inspect(ddsPath);
    check(diagnostics.valid && diagnostics.width == 1 && diagnostics.height == 1,
        QStringLiteral("texture diagnostics report DDS dimensions"));
    check(diagnostics.channels == QStringList({QStringLiteral("R"), QStringLiteral("G"), QStringLiteral("B"), QStringLiteral("A")}),
        QStringLiteral("texture diagnostics report available channels"));
    QByteArray cubemapDds(128 + 6 * (4 * 4 * 4 + 2 * 2 * 4), '\0');
    cubemapDds.replace(0, 4, "DDS ");
    const auto cubePut32 = [&cubemapDds](int offset, quint32 value) {
        qToLittleEndian(value, reinterpret_cast<uchar*>(cubemapDds.data() + offset));
    };
    cubePut32(4, 124); cubePut32(12, 4); cubePut32(16, 4); cubePut32(28, 2);
    cubePut32(76, 32); cubePut32(88, 32);
    cubePut32(92, 0x00ff0000); cubePut32(96, 0x0000ff00);
    cubePut32(100, 0x000000ff); cubePut32(104, 0xff000000);
    constexpr int cubeFaceStride = 4 * 4 * 4 + 2 * 2 * 4;
    for (int face = 0; face < 6; ++face) {
        const quint32 base = 0xff000000u | (quint32(20 + face * 25) << 16)
            | (quint32(40 + face * 15) << 8) | quint32(60 + face * 10);
        for (int pixel = 0; pixel < 16; ++pixel)
            cubePut32(128 + face * cubeFaceStride + pixel * 4, base);
        // Poison authored mip 1. The preview loader must never expose it; the
        // renderer generates a replacement chain from the decoded base faces.
        for (int pixel = 0; pixel < 4; ++pixel)
            cubePut32(128 + face * cubeFaceStride + 64 + pixel * 4, 0xffff00ffu);
    }
    const QString cubemapDdsPath = directory.filePath(QStringLiteral("preview-cubemap-with-bad-mips.dds"));
    QFile cubemapDdsFile(cubemapDdsPath);
    check(cubemapDdsFile.open(QIODevice::WriteOnly)
            && cubemapDdsFile.write(cubemapDds) == cubemapDds.size(),
        QStringLiteral("DDS cubemap bad-mip fixture can be created"));
    cubemapDdsFile.close();
    const QVector<QImage> cubemapFaces = TextureImage::loadCubemapFaces(cubemapDdsPath, {}, &error);
    bool cubemapBasesOnly = cubemapFaces.size() == 6;
    for (int face = 0; face < cubemapFaces.size() && cubemapBasesOnly; ++face) {
        const QColor expected(20 + face * 25, 40 + face * 15, 60 + face * 10, 255);
        cubemapBasesOnly = cubemapFaces[face].size() == QSize(4, 4)
            && cubemapFaces[face].pixelColor(0, 0) == expected;
    }
    check(cubemapBasesOnly,
        QStringLiteral("Fallout DDS cubemap authored mips are discarded in favor of base faces"));
    const QString realCubemapPath = qEnvironmentVariable("PBRM_TEST_CUBEMAP");
    if (!realCubemapPath.isEmpty()) {
        const QVector<QImage> realFaces = TextureImage::loadCubemapFaces(realCubemapPath, {}, &error);
        check(realFaces.size() == 6, QStringLiteral("real Fallout DDS cubemap decodes to six faces: %1").arg(error));
        const QString dumpPath = qEnvironmentVariable("PBRM_TEST_CUBEMAP_DUMP");
        if (!dumpPath.isEmpty()) {
            QDir().mkpath(dumpPath);
            for (int face = 0; face < realFaces.size(); ++face)
                realFaces[face].save(QDir(dumpPath).filePath(QStringLiteral("face-%1.png").arg(face)));
        }
    }
    QVector<float> exrPixels(8 * 4 * 4);
    for (int pixel = 0; pixel < 8 * 4; ++pixel) {
        exrPixels[pixel * 4] = 3.5f; exrPixels[pixel * 4 + 1] = .75f;
        exrPixels[pixel * 4 + 2] = .25f; exrPixels[pixel * 4 + 3] = 1.0f;
    }
    const QString exrPath = directory.filePath(QStringLiteral("preview-environment.exr"));
    const QByteArray exrNativePath = QFile::encodeName(exrPath);
    const char* exrWriteError = nullptr;
    const int exrResult = SaveEXR(exrPixels.constData(), 8, 4, 4, 1, exrNativePath.constData(), &exrWriteError);
    check(exrResult == TINYEXR_SUCCESS, QStringLiteral("HDR OpenEXR environment fixture can be created"));
    if (exrWriteError) FreeEXRErrorMessage(exrWriteError);
    const QVector<QImage> exrFaces = TextureImage::loadCubemapFaces(exrPath, {}, &error);
    check(exrFaces.size() == 6 && exrFaces.front().format() == QImage::Format_RGBA32FPx4,
        QStringLiteral("2:1 OpenEXR environment converts to six floating-point cubemap faces: %1").arg(error));
    const float* exrFacePixel = exrFaces.isEmpty() ? nullptr : reinterpret_cast<const float*>(exrFaces.front().constScanLine(0));
    check(exrFacePixel && exrFacePixel[0] > 3.4f,
        QStringLiteral("OpenEXR cubemap conversion preserves HDR values above one"));
    QVector<float> orientedExrPixels(16 * 8 * 4);
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 16; ++x) {
        const int pixel = (y * 16 + x) * 4;
        orientedExrPixels[pixel] = y < 4 ? 4.0f : .05f;
        orientedExrPixels[pixel + 1] = .05f;
        orientedExrPixels[pixel + 2] = y < 4 ? .05f : 4.0f;
        orientedExrPixels[pixel + 3] = 1.0f;
    }
    const QString orientedExrPath = directory.filePath(QStringLiteral("oriented-environment.exr"));
    const QByteArray orientedExrNativePath = QFile::encodeName(orientedExrPath);
    const char* orientedExrError = nullptr;
    const int orientedExrResult = SaveEXR(orientedExrPixels.constData(), 16, 8, 4, 1,
        orientedExrNativePath.constData(), &orientedExrError);
    check(orientedExrResult == TINYEXR_SUCCESS, QStringLiteral("oriented OpenEXR fixture can be created"));
    if (orientedExrError) FreeEXRErrorMessage(orientedExrError);
    const QVector<QImage> orientedFaces = TextureImage::loadCubemapFaces(orientedExrPath, {}, &error);
    const auto centerFloatPixel = [&orientedFaces](int face) -> const float* {
        if (orientedFaces.size() != 6) return nullptr;
        const QImage& image = orientedFaces[face];
        return reinterpret_cast<const float*>(image.constScanLine(image.height() / 2)) + (image.width() / 2) * 4;
    };
    const float* worldUp = centerFloatPixel(5);
    const float* worldDown = centerFloatPixel(4);
    check(worldUp && worldDown && worldUp[0] > 3.0f && worldUp[2] < .2f
            && worldDown[2] > 3.0f && worldDown[0] < .2f,
        QStringLiteral("OpenEXR latitude-longitude poles follow the preview world axes"));
    const PbrPreviewGL::EnvironmentLight derivedExrLight = PbrPreviewGL::deriveEnvironmentLight(orientedFaces);
    check(derivedExrLight.valid && derivedExrLight.pitch > 70.0f,
        QStringLiteral("dominant OpenEXR radiance derives the expected world-space light direction"));
    check(derivedExrLight.color.redF() > derivedExrLight.color.blueF() && derivedExrLight.intensity > 0.0f,
        QStringLiteral("dominant OpenEXR radiance derives key-light color and intensity"));
    QVector<QImage> directionalFaces;
    for (int face = 0; face < 6; ++face) {
        QImage image(16, 16, QImage::Format_RGBA32FPx4);
        for (int y = 0; y < image.height(); ++y) {
            float* row = reinterpret_cast<float*>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x) { row[x * 4] = .1f; row[x * 4 + 1] = .1f; row[x * 4 + 2] = .1f; row[x * 4 + 3] = 1.0f; }
        }
        directionalFaces.append(std::move(image));
    }
    for (int y = 5; y <= 10; ++y) {
        float* row = reinterpret_cast<float*>(directionalFaces[0].scanLine(y));
        for (int x = 5; x <= 10; ++x) { row[x * 4] = 10.0f; row[x * 4 + 1] = 5.0f; row[x * 4 + 2] = 2.0f; }
    }
    const PbrPreviewGL::EnvironmentLight unrotatedLight = PbrPreviewGL::deriveEnvironmentLight(directionalFaces);
    const PbrPreviewGL::EnvironmentLight rotatedLight = PbrPreviewGL::deriveEnvironmentLight(directionalFaces, 45.0f);
    check(unrotatedLight.valid && rotatedLight.valid && qAbs(std::remainder(unrotatedLight.yaw - rotatedLight.yaw, 360.0f) - 45.0f) < 1.0f,
        QStringLiteral("environment rotation keeps the EXR-derived key light and shadow direction aligned"));
    QByteArray arrayDds(148 + 16, '\0');
    arrayDds.replace(0, 4, "DDS ");
    const auto arrayPut32 = [&arrayDds](int offset, quint32 value) { qToLittleEndian(value, reinterpret_cast<uchar*>(arrayDds.data() + offset)); };
    const auto arrayPut16 = [&arrayDds](int offset, quint16 value) { qToLittleEndian(value, reinterpret_cast<uchar*>(arrayDds.data() + offset)); };
    arrayPut32(4,124);arrayPut32(12,4);arrayPut32(16,4);arrayPut32(28,1);arrayPut32(76,32);arrayDds.replace(84,4,"DX10");
    arrayPut32(128,71);arrayPut32(132,3);arrayPut32(140,2);arrayPut16(148,0xf800);arrayPut16(150,0);arrayPut16(156,0x001f);arrayPut16(158,0);
    const QString arrayPath=directory.filePath(QStringLiteral("preview-array.dds"));QFile arrayFile(arrayPath);
    check(arrayFile.open(QIODevice::WriteOnly)&&arrayFile.write(arrayDds)==arrayDds.size(),QStringLiteral("DDS array fixture can be created"));arrayFile.close();
    int arraySlices=0;const QImage arraySlice=TextureImage::loadArraySlice(arrayPath,1,{},&arraySlices,&error);
    check(arraySlices==2&&arraySlice.size()==QSize(4,4),QStringLiteral("DDS texture-array metadata and selected slice load"));
    check(!arraySlice.isNull()&&arraySlice.pixelColor(0,0).blue()>240,QStringLiteral("landscape preview selects the requested DDS array slice"));
    const QString sampleBc7 = qEnvironmentVariable("PBRM_TEST_BC7");
    if (!sampleBc7.isEmpty()) {
        const TextureImage::Diagnostics bc7Info = TextureImage::inspect(sampleBc7);
        check(bc7Info.valid && bc7Info.format == QStringLiteral("BC7"), QStringLiteral("real BC7 texture is identified"));
        check(!TextureImage::load(sampleBc7, {}, &error).isNull(), QStringLiteral("real BC7 texture decodes for viewport shading: %1").arg(error));
    }

    QSettings sourceSettings;
    const QVariant oldMods = sourceSettings.value(QStringLiteral("TextureSources/modFolders"));
    const QVariant oldGame = sourceSettings.value(QStringLiteral("TextureSources/fallout4Textures"));
    const QVariant oldArchives = sourceSettings.value(QStringLiteral("TextureSources/archives"));
    const QString modTextures = directory.filePath(QStringLiteral("ExampleMod/Textures"));
    QDir().mkpath(directory.filePath(QStringLiteral("ExampleMod/Textures/PBR/Test")));
    const QString looseTexture = directory.filePath(QStringLiteral("ExampleMod/Textures/PBR/Test/preview.dds"));
    QFile::copy(ddsPath, looseTexture);
    sourceSettings.setValue(QStringLiteral("TextureSources/modFolders"), QStringList{modTextures});
    sourceSettings.remove(QStringLiteral("TextureSources/fallout4Textures"));
    sourceSettings.remove(QStringLiteral("TextureSources/archives"));
    sourceSettings.sync();
    const QString portable = TextureImage::authoredPathForFile(looseTexture);
    check(QFileInfo::exists(looseTexture), QStringLiteral("loose texture fixture exists"));
    check(TextureImage::searchRoots().contains(QFileInfo(modTextures).absoluteFilePath()),
        QStringLiteral("configured mod folder is visible to the resolver: %1").arg(TextureImage::searchRoots().join(QStringLiteral("; "))));
    check(QDir::fromNativeSeparators(portable) == QStringLiteral("PBR/Test/preview.dds"),
        QStringLiteral("browsed textures are authored relative to their Textures source"));
    check(TextureImage::load(portable).size() == QSize(1, 1),
        QStringLiteral("portable texture paths resolve through configured mod folders"));

    const QByteArray archiveName = QByteArrayLiteral("textures/test/archive.dds");
    QByteArray rawBc1(8, '\0');
    rawBc1[0] = char(0xff); rawBc1[1] = char(0xff);
    QByteArray packedBc1(static_cast<qsizetype>(compressBound(rawBc1.size())), Qt::Uninitialized);
    uLongf packedBc1Size = packedBc1.size();
    const int compressionResult = compress2(reinterpret_cast<Bytef*>(packedBc1.data()), &packedBc1Size,
        reinterpret_cast<const Bytef*>(rawBc1.constData()), rawBc1.size(), Z_BEST_SPEED);
    packedBc1.resize(static_cast<qsizetype>(packedBc1Size));
    check(compressionResult == Z_OK, QStringLiteral("BA2 compressed texture chunk can be created"));
    const quint64 archiveDataOffset = 24 + 24 + 24 + 2 + archiveName.size();
    QByteArray ba2(static_cast<qsizetype>(archiveDataOffset + packedBc1.size()), '\0');
    ba2.replace(0, 4, "BTDX"); ba2.replace(8, 4, "DX10");
    const auto ba2Put16 = [&ba2](int offset, quint16 value) { qToLittleEndian(value, reinterpret_cast<uchar*>(ba2.data() + offset)); };
    const auto ba2Put32 = [&ba2](int offset, quint32 value) { qToLittleEndian(value, reinterpret_cast<uchar*>(ba2.data() + offset)); };
    const auto ba2Put64 = [&ba2](int offset, quint64 value) { qToLittleEndian(value, reinterpret_cast<uchar*>(ba2.data() + offset)); };
    ba2Put32(4, 1); ba2Put32(12, 1); ba2Put64(16, 72);
    ba2[24 + 13] = 1; ba2Put16(24 + 14, 24); ba2Put16(24 + 16, 4); ba2Put16(24 + 18, 4);
    ba2[24 + 20] = 1; ba2[24 + 21] = 71;
    ba2Put64(48, archiveDataOffset); ba2Put32(48 + 8, packedBc1.size()); ba2Put32(48 + 12, 8); ba2Put16(48 + 16, 0); ba2Put16(48 + 18, 0);
    ba2Put16(72, archiveName.size()); ba2.replace(74, archiveName.size(), archiveName);
    ba2.replace(static_cast<qsizetype>(archiveDataOffset), packedBc1.size(), packedBc1);
    const QString ba2Path = directory.filePath(QStringLiteral("test-textures.ba2"));
    QFile ba2File(ba2Path);
    check(ba2File.open(QIODevice::WriteOnly) && ba2File.write(ba2) == ba2.size(),
        QStringLiteral("BA2 texture fixture can be created"));
    ba2File.close();
    sourceSettings.setValue(QStringLiteral("TextureSources/archives"), QStringList{ba2Path});
    sourceSettings.sync();
    check(TextureImage::archivePaths().contains(QFileInfo(ba2Path).absoluteFilePath()),
        QStringLiteral("configured BA2 is visible to the resolver: %1").arg(TextureImage::archivePaths().join(QStringLiteral("; "))));
    check(TextureImage::resolvePath(QStringLiteral("test/archive.dds")).contains(QStringLiteral("test-textures.ba2")),
        QStringLiteral("relative texture paths resolve inside configured BA2 archives"));
    check(TextureImage::load(QStringLiteral("test/archive.dds")).size() == QSize(4, 4),
        QStringLiteral("BA2 DX10 texture chunks are reconstructed and decoded for preview"));
    const QVector<TextureImage::Resource> browserResources = TextureImage::enumerate();
    check(std::any_of(browserResources.cbegin(), browserResources.cend(), [](const TextureImage::Resource& resource) {
        return resource.archived && QDir::fromNativeSeparators(resource.path) == QStringLiteral("test/archive.dds");
    }), QStringLiteral("texture browser enumerates configured BA2 textures"));
    if (oldMods.isValid()) sourceSettings.setValue(QStringLiteral("TextureSources/modFolders"), oldMods); else sourceSettings.remove(QStringLiteral("TextureSources/modFolders"));
    if (oldGame.isValid()) sourceSettings.setValue(QStringLiteral("TextureSources/fallout4Textures"), oldGame); else sourceSettings.remove(QStringLiteral("TextureSources/fallout4Textures"));
    if (oldArchives.isValid()) sourceSettings.setValue(QStringLiteral("TextureSources/archives"), oldArchives); else sourceSettings.remove(QStringLiteral("TextureSources/archives"));

    const PreviewMesh plane = PreviewMeshes::plane(PreviewMeshes::PlaneOrientation::XY, true);
    check(plane.isValid() && plane.indices.size() == 6 && plane.vertices.size() == 4,
        QStringLiteral("two-sided preview plane uses one non-overlapping quad"));
    check(plane.submeshes.size() == 1, QStringLiteral("preview plane exposes one selectable submesh"));
    const PreviewMesh displacedPlane = PreviewMeshes::planeGrid(PreviewMeshes::PlaneOrientation::XY, 16);
    check(displacedPlane.vertices.size() == 289 && displacedPlane.indices.size() == 1536,
        QStringLiteral("displacement preview plane provides deterministic tessellation"));
    check(displacedPlane.submeshes.size() == 1 && displacedPlane.submeshes.front().name == QStringLiteral("Displaced Plane"),
        QStringLiteral("tessellated displacement plane remains one selectable surface"));
    const PreviewMesh cube = PreviewMeshes::cube();
    check(cube.vertices.size() == 24 && cube.indices.size() == 36, QStringLiteral("preview cube has per-face normals and UV seams"));
    const PreviewMesh sphere = PreviewMeshes::sphere(16, 8);
    check(sphere.isValid() && sphere.vertices.size() == 153 && sphere.indices.size() == 768,
        QStringLiteral("preview sphere tessellation is deterministic"));
    bool sphereNormalsValid = true;
    for (const PreviewVertex& vertex : sphere.vertices)
        sphereNormalsValid = sphereNormalsValid && qAbs(vertex.normal.length() - 1.0f) < 0.001f;
    check(sphereNormalsValid, QStringLiteral("preview sphere normals are unit length"));
    bool sphereWindingOutward = true;
    for (int triangle = 0; triangle + 2 < sphere.indices.size(); triangle += 3) {
        const PreviewVertex& a = sphere.vertices[int(sphere.indices[triangle])];
        const PreviewVertex& b = sphere.vertices[int(sphere.indices[triangle + 1])];
        const PreviewVertex& c = sphere.vertices[int(sphere.indices[triangle + 2])];
        const QVector3D face = QVector3D::crossProduct(b.position - a.position, c.position - a.position);
        if (face.lengthSquared() > 1.0e-10f && QVector3D::dotProduct(face, a.normal + b.normal + c.normal) <= 0.0f) sphereWindingOutward = false;
    }
    check(sphereWindingOutward, QStringLiteral("preview sphere winding faces outward for back-face culling"));
    PreviewMesh offset = cube;
    for (PreviewVertex& vertex : offset.vertices) vertex.position += QVector3D(100, -30, 7);
    PreviewMeshes::normalizeAndCenter(offset);
    float normalizedRadius = 0.0f;
    for (const PreviewVertex& vertex : offset.vertices) normalizedRadius = qMax(normalizedRadius, vertex.position.length());
    check(qAbs(normalizedRadius - 1.0f) < 0.001f, QStringLiteral("custom preview meshes are centered and normalized"));
    QString fbxError;
    check(!PreviewMeshes::loadStaticFbx(QStringLiteral("not-a-mesh.obj"), &fbxError).isValid() && fbxError.contains(QStringLiteral(".fbx")),
        QStringLiteral("custom preview importer rejects non-FBX files"));
    const QString sampleFbx = qEnvironmentVariable("PBRM_TEST_FBX");
    if (!sampleFbx.isEmpty()) {
        const PreviewMesh imported = PreviewMeshes::loadStaticFbx(sampleFbx, &fbxError);
        check(imported.isValid(), QStringLiteral("valid static FBX imports: %1").arg(fbxError));
        check(imported.radius > 0.99f && imported.radius < 1.01f, QStringLiteral("imported FBX is normalized to unit radius"));
        check(!imported.submeshes.isEmpty() && imported.indices.size() % 3 == 0,
            QStringLiteral("imported FBX exposes indexed static submeshes"));
    }
    QString nifError;
    check(!PreviewMeshes::loadStaticNif(QStringLiteral("not-a-mesh.obj"), &nifError).isValid() && nifError.contains(QStringLiteral(".nif")),
        QStringLiteral("custom preview importer rejects non-NIF files"));
    const QString sampleNif = qEnvironmentVariable("PBRM_TEST_NIF");
    if (!sampleNif.isEmpty()) {
        const PreviewMesh imported = PreviewMeshes::loadStaticNif(sampleNif, &nifError);
        check(imported.isValid(), QStringLiteral("valid static Fallout 4 NIF imports: %1").arg(nifError));
        check(imported.radius > 0.99f && imported.radius < 1.01f, QStringLiteral("imported NIF is normalized to unit radius"));
        check(!imported.submeshes.isEmpty() && imported.indices.size() % 3 == 0,
            QStringLiteral("imported NIF exposes indexed static submeshes"));
    }

    QTextStream(stdout) << "Failures: " << failures << Qt::endl;
    return failures == 0 ? 0 : 1;
}
