#include "bgsmreader.h"
#include "converter.h"
#include "convertercore.h"
#include "converternodes.h"
#include "nodecanvas.h"
#include "nodegraph.h"
#include "normalderive.h"
#include "upscaler.h"
#include "geometryao.h"
#include "maskpaint.h"
#include "materialdocument.h"
#include "specularmigrate.h"
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
        // Both preview panels fold, from one code path. The defaults differ
        // because the panels mean different things: in the author workspace
        // these controls are the work, in the converter they are setup and the
        // viewport is the verdict.
        auto* previewSettings = window.findChild<QGroupBox*>(QStringLiteral("PreviewSettings"));
        check(previewSettings && previewSettings->isCheckable() && previewSettings->isChecked(),
            QStringLiteral("the author's preview settings fold away but start open"));
        auto* converterSettings = window.findChild<QGroupBox*>(QStringLiteral("ConverterPreviewSettings"));
        check(converterSettings && converterSettings->isCheckable() && !converterSettings->isChecked(),
            QStringLiteral("the converter's preview settings fold away and start folded"));
        check(converterSettings && converterSettings->maximumHeight() <= 26,
            QStringLiteral("folding the converter's preview settings hands the height back to the viewport"));
        // Folding one must not fold the other: they are separate widgets with
        // separate persisted state, and a shared key would tie them together.
        if (previewSettings && converterSettings) {
            previewSettings->setChecked(false);
            check(previewSettings->maximumHeight() <= 26 && !converterSettings->isChecked(),
                QStringLiteral("folding the author panel leaves the converter panel alone"));
            converterSettings->setChecked(true);
            check(converterSettings->maximumHeight() > 26 && !previewSettings->isChecked(),
                QStringLiteral("and unfolding the converter panel leaves the author panel alone"));
            previewSettings->setChecked(true);
            converterSettings->setChecked(false);
        }
        // The Material Converter is experimental and opt-in. With the option
        // off it must be ABSENT from the switcher rather than disabled, and a
        // switcher left with one destination hides itself.
        auto* modeCombo = window.findChild<QComboBox*>(QStringLiteral("WorkspaceModeCombo"));
        check(modeCombo && modeCombo->count() == 1
                && modeCombo->itemText(0) == QStringLiteral("Material Editor"),
            QStringLiteral("the experimental converter is absent from the mode switcher by default"));
        check(modeCombo && modeCombo->isHidden(),
            QStringLiteral("a mode switcher with one destination hides itself"));
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
        // Turning the experimental option ON puts the converter in the
        // switcher; the --converter harness flag reaches it either way but
        // must never write the stored option on the user's behalf.
        QSettings().setValue(QStringLiteral("Interface/experimentalConverter"), true);
        MainWindow optedIn;
        auto* optedInCombo = optedIn.findChild<QComboBox*>(QStringLiteral("WorkspaceModeCombo"));
        check(optedInCombo && optedInCombo->count() == 2
                && optedInCombo->itemText(1) == QStringLiteral("Material Converter")
                && !optedInCombo->isHidden(),
            QStringLiteral("the experimental option puts the converter back in the mode switcher"));
        QSettings().remove(QStringLiteral("Interface/experimentalConverter"));

        MainWindow gatedOff;
        auto* gatedCombo = gatedOff.findChild<QComboBox*>(QStringLiteral("WorkspaceModeCombo"));
        gatedOff.showConverter();
        check(gatedCombo && gatedCombo->count() == 2 && gatedCombo->currentIndex() == 1
                && !gatedCombo->isHidden(),
            QStringLiteral("--converter still opens the converter while the option is off"));
        check(!QSettings().contains(QStringLiteral("Interface/experimentalConverter")),
            QStringLiteral("opening the converter by flag never writes the experimental option"));
    }

    {
        // SPECULAR COLOR + SPECULAR IOR. RMAOS alpha carries the specular
        // weight, so a painted color is the only way to author a tinted
        // dielectric reflection. The tint itself has no constants; the only
        // values on the slot are its RGB override (constant tint, white = no
        // change) and the v6 specular IOR (bungo 2026-09-16: a feature's
        // parameters sit on its texture input): the A override toggle, the
        // IOR constant and the map ceiling. Values are sparse (defaults
        // implicit), so an untouched slot with no map carries only the two
        // override toggles, forced on because no texture provides RGB or A.
        MainWindow specularWindow;
        const TextureSlotWidget* specularSlot = nullptr;
        for (const TextureSlotWidget* candidate : specularWindow.findChildren<TextureSlotWidget*>())
            if (candidate->key() == QStringLiteral("primarySpecularColor")) { specularSlot = candidate; break; }
        check(specularSlot != nullptr,
            QStringLiteral("the primary layer offers a specular color map"));
        const QJsonObject specularValues = specularSlot ? specularSlot->state().values : QJsonObject();
        check(specularSlot && specularValues.keys() == QStringList{QStringLiteral("overrideColor"), QStringLiteral("overrideIor")}
                && specularValues.value(QStringLiteral("overrideColor")).toBool() && specularValues.value(QStringLiteral("overrideIor")).toBool(),
            QStringLiteral("an untouched specular color input carries only the forced-on RGB and IOR overrides (%1)")
                .arg(specularValues.keys().join(QLatin1Char(','))));

        QTemporaryDir specularDirectory;
        const QString specularTexture = QStringLiteral("plate_specular_color.png");
        QImage specularImage(2, 2, QImage::Format_RGBA8888);
        specularImage.fill(QColor(64, 96, 40, 255));
        check(specularImage.save(specularDirectory.filePath(specularTexture)),
            QStringLiteral("specular color fixture can be created"));

        Pbr::MaterialDocument plainMaterial;
        check(!plainMaterial.toJson().value(QStringLiteral("resources")).toObject()
                .value(QStringLiteral("UV0")).toObject()
                .contains(QStringLiteral("primarySpecularColor")),
            QStringLiteral("a material that never paints a specular color carries no such socket"));

        Pbr::TextureSlotState paintedSpecular;
        paintedSpecular.enabled = true;
        paintedSpecular.path = specularTexture;
        Pbr::MaterialDocument authoredSpecular;
        authoredSpecular.setTextureSlot(QStringLiteral("UV0"), QStringLiteral("primarySpecularColor"), paintedSpecular);
        QString specularError;
        const QString specularFile = specularDirectory.filePath(QStringLiteral("speccolor.pbrm"));
        check(authoredSpecular.save(specularFile, &specularError),
            QStringLiteral("a specular color material saves: %1").arg(specularError));
        Pbr::MaterialDocument reloadedSpecular;
        check(reloadedSpecular.load(specularFile, &specularError)
                && reloadedSpecular.textureSlot(QStringLiteral("UV0"), QStringLiteral("primarySpecularColor")).enabled
                && reloadedSpecular.textureSlot(QStringLiteral("UV0"), QStringLiteral("primarySpecularColor")).path == specularTexture,
            QStringLiteral("the specular color socket round-trips through the envelope"));

        // The preview's F0 lerp reads this map as a special layer, so the layer
        // being bound is the wiring the shading term depends on. Both
        // directions are asserted: a preview that never binds fails the second
        // check, one that always binds fails the first.
        PbrPreviewGL specularPreview;
        specularPreview.setMaterial(plainMaterial, specularDirectory.path());
        check(!specularPreview.specularColorLayerBound(),
            QStringLiteral("without the map the preview keeps the scalar F0 path"));
        specularPreview.setMaterial(authoredSpecular, specularDirectory.path());
        check(specularPreview.specularColorLayerBound(),
            QStringLiteral("with the map the preview resolves it into the F0 term"));
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
            && migratedValues.value(QStringLiteral("overrideSpecularWeight")).toBool(false)
            && !migratedValues.contains(QStringLiteral("overrideF0"))
            && !migratedValues.value(QStringLiteral("overrideMask")).toBool(true)
            && migratedValues.value(QStringLiteral("overrideMaskR")).toBool(false)
            && qFuzzyCompare(migratedValues.value(QStringLiteral("height")).toDouble(), 0.25)
            && !migratedValues.contains(QStringLiteral("useRoughness"))
            && !migratedValues.contains(QStringLiteral("useF0"))
            && !migratedValues.contains(QStringLiteral("useMask"))
            && !migratedValues.contains(QStringLiteral("fallback")),
        QStringLiteral("legacy source booleans and height fallback become positive constant overrides"));

    // ---- PBRM v6: specular weight, specular IOR, normal alpha selector ----
    // docs/PBRM-v6-Specular.md section 4. Constants migrate exactly (weight =
    // min(f0 / 0.04, 1); f0 > 0.04 moves the level into the material IOR);
    // texels cannot and are marked for the repack.
    {
        const auto v5Root = [](int schemaVersion, const QJsonObject& rmaosValues, const QJsonObject& normalValues, bool withSpecColor) {
            QJsonObject primary{
                {QStringLiteral("primaryRmaos"), QJsonObject{{QStringLiteral("enabled"), true}, {QStringLiteral("expanded"), true},
                    {QStringLiteral("path"), QStringLiteral("old_rmaos.dds")}, {QStringLiteral("values"), rmaosValues}}},
                {QStringLiteral("primaryNormal"), QJsonObject{{QStringLiteral("enabled"), true}, {QStringLiteral("expanded"), true},
                    {QStringLiteral("path"), QStringLiteral("old_n.dds")}, {QStringLiteral("values"), normalValues}}}};
            if (withSpecColor)
                primary.insert(QStringLiteral("primarySpecularColor"), QJsonObject{{QStringLiteral("enabled"), true}, {QStringLiteral("expanded"), true},
                    {QStringLiteral("path"), QStringLiteral("old_spec.dds")}, {QStringLiteral("values"), QJsonObject()}});
            return QJsonObject{{QStringLiteral("schema"), QStringLiteral("FO4.PBRM.Material")},
                {QStringLiteral("schemaVersion"), schemaVersion}, {QStringLiteral("shader"), QStringLiteral("Standard")},
                {QStringLiteral("secondaryUvEnabled"), false}, {QStringLiteral("primaryUv"), primary},
                {QStringLiteral("secondaryUv"), QJsonObject()}, {QStringLiteral("resources"), QJsonObject()},
                {QStringLiteral("settings"), QJsonObject()}};
        };
        QString v6Error;
        const QJsonObject constantLow{{QStringLiteral("f0"), 0.02}, {QStringLiteral("overrideF0"), true}};
        Pbr::MaterialDocument low;
        check(low.fromJson(v5Root(5, constantLow, QJsonObject{{QStringLiteral("curvatureInAlpha"), true}}, false), &v6Error),
            QStringLiteral("v6: a v5 document loads: %1").arg(v6Error));
        const QJsonObject lowValues = low.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryRmaos")).values;
        check(qFuzzyCompare(lowValues.value(QStringLiteral("specularWeight")).toDouble(), 0.5)
                && lowValues.value(QStringLiteral("overrideSpecularWeight")).toBool(false)
                && !lowValues.contains(QStringLiteral("f0")) && !lowValues.contains(QStringLiteral("overrideF0"))
                && !lowValues.contains(QStringLiteral("legacyF0Alpha")),
            QStringLiteral("v6: constant F0 0.02 becomes specular weight 0.5 at IOR 1.5 (exact)"));
        check(!low.textureSlot(QStringLiteral("UV0"), QStringLiteral("primarySpecularColor")).values.contains(QStringLiteral("ior"))
                && !low.settingGroup(QStringLiteral("Specular")).contains(QStringLiteral("ior")),
            QStringLiteral("v6: an F0 at or under 0.04 writes no specular IOR"));
        check(low.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryNormal")).values.value(QStringLiteral("alphaCarries")).toString() == QLatin1String("Curvature")
                && !low.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryNormal")).values.contains(QStringLiteral("curvatureInAlpha")),
            QStringLiteral("v6: curvatureInAlpha=true becomes the Curvature selector and the bool is gone"));

        const QJsonObject constantHigh{{QStringLiteral("f0"), 0.16}, {QStringLiteral("overrideF0"), true}};
        Pbr::MaterialDocument high;
        check(high.fromJson(v5Root(5, constantHigh, QJsonObject(), false), &v6Error), QStringLiteral("v6: high constant loads"));
        check(qFuzzyCompare(high.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryRmaos")).values.value(QStringLiteral("specularWeight")).toDouble(), 1.0)
                && qFuzzyCompare(high.textureSlot(QStringLiteral("UV0"), QStringLiteral("primarySpecularColor")).values.value(QStringLiteral("ior")).toDouble(), (1.0 + 0.4) / (1.0 - 0.4))
                && high.settingGroup(QStringLiteral("Specular")).isEmpty(),
            QStringLiteral("v6: constant F0 0.16 becomes weight 1 at IOR 2.333 on the specular colour input (exact)"));

        // Feature parameters live on the feature's input (bungo 2026-09-16).
        // The first v6 cut and v5 kept them in Shading-page setting groups;
        // the loader moves them onto the primary slot and the groups vanish.
        {
            QJsonObject legacy = v5Root(6, QJsonObject{{QStringLiteral("specularWeight"), 1.0}}, QJsonObject(), true);
            QJsonObject primary = legacy.value(QStringLiteral("primaryUv")).toObject();
            QJsonObject specColor = primary.value(QStringLiteral("primarySpecularColor")).toObject();
            specColor.insert(QStringLiteral("values"), QJsonObject{{QStringLiteral("iorInAlpha"), true}});
            specColor.insert(QStringLiteral("enabled"), true);
            specColor.insert(QStringLiteral("path"), QStringLiteral("textures/test/spec_s.dds"));
            primary.insert(QStringLiteral("primarySpecularColor"), specColor);
            primary.insert(QStringLiteral("primarySubsurface"), QJsonObject{{QStringLiteral("enabled"), false}, {QStringLiteral("expanded"), true},
                {QStringLiteral("path"), QString()}, {QStringLiteral("values"), QJsonObject{{QStringLiteral("thickness"), 2.0}, {QStringLiteral("weight"), 0.25}}}});
            legacy.insert(QStringLiteral("primaryUv"), primary);
            legacy.insert(QStringLiteral("shader"), QStringLiteral("Advanced Subsurface"));
            legacy.insert(QStringLiteral("settings"), QJsonObject{
                {QStringLiteral("Specular"), QJsonObject{{QStringLiteral("ior"), 1.8}, {QStringLiteral("iorMax"), 5.0}}},
                {QStringLiteral("Shading/Subsurface"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("Combined")}, {QStringLiteral("profile"), QStringLiteral("Custom")},
                    {QStringLiteral("weight"), 1.5}, {QStringLiteral("thicknessScale"), 3.0}}},
                {QStringLiteral("Shading/SurfaceEffects"), QJsonObject{{QStringLiteral("sheenStrength"), 0.7}, {QStringLiteral("retroStrength"), 0.5}}},
                {QStringLiteral("Shading/Hair"), QJsonObject{{QStringLiteral("scatter"), 1.25}, {QStringLiteral("primaryShift"), -0.1}}},
                {QStringLiteral("Shading/Glass"), QJsonObject{{QStringLiteral("ior"), 1.52}, {QStringLiteral("thinWalled"), true}}},
                {QStringLiteral("Shading/Eye"), QJsonObject{{QStringLiteral("corneaIor"), 1.4}, {QStringLiteral("pupilScale"), 1.3}}}});
            Pbr::MaterialDocument moved;
            check(moved.fromJson(legacy, &v6Error), QStringLiteral("v6: a document with Shading-page feature groups loads: %1").arg(v6Error));
            const auto spec = moved.textureSlot(QStringLiteral("UV0"), QStringLiteral("primarySpecularColor")).values;
            check(qFuzzyCompare(spec.value(QStringLiteral("ior")).toDouble(), 1.8) && qFuzzyCompare(spec.value(QStringLiteral("iorMax")).toDouble(), 5.0)
                    && spec.contains(QStringLiteral("overrideIor")) && !spec.value(QStringLiteral("overrideIor")).toBool(true) && !spec.contains(QStringLiteral("iorInAlpha")),
                QStringLiteral("v6: Specular/ior + iorMax move onto the specular colour input and iorInAlpha=true becomes overrideIor=false"));
            check(spec.contains(QStringLiteral("overrideColor")) && !spec.value(QStringLiteral("overrideColor")).toBool(true),
                QStringLiteral("v6: an authored specular colour map without the RGB override key is read (overrideColor=false written on load)"));
            {
                // 2026-09-17 override audit: channels that gained the editor-wide
                // override after their slot shipped keep reading authored maps.
                QJsonObject lateRoot = v5Root(6, QJsonObject{{QStringLiteral("specularWeight"), 1.0}}, QJsonObject(), true);
                QJsonObject latePrimary = lateRoot.value(QStringLiteral("primaryUv")).toObject();
                for (const char* key : {"primarySixWayA", "primarySixWayB", "primaryStateMasks", "primaryInteriorMap"}) {
                    latePrimary.insert(QLatin1String(key), QJsonObject{{QStringLiteral("enabled"), true}, {QStringLiteral("expanded"), true},
                        {QStringLiteral("path"), QStringLiteral("textures/test/%1.dds").arg(QLatin1String(key))}, {QStringLiteral("values"), QJsonObject()}});
                }
                lateRoot.insert(QStringLiteral("primaryUv"), latePrimary);
                Pbr::MaterialDocument late;
                QString lateError;
                check(late.fromJson(lateRoot, &lateError), QStringLiteral("late-override document loads: %1").arg(lateError));
                const auto sixA = late.textureSlot(QStringLiteral("UV0"), QStringLiteral("primarySixWayA")).values;
                const auto sixB = late.textureSlot(QStringLiteral("UV0"), QStringLiteral("primarySixWayB")).values;
                const auto masks = late.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryStateMasks")).values;
                const auto room = late.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryInteriorMap")).values;
                check(sixA.contains(QStringLiteral("overrideRight")) && !sixA.value(QStringLiteral("overrideRight")).toBool(true)
                        && !sixA.value(QStringLiteral("overrideFront")).toBool(true) && !sixB.value(QStringLiteral("overrideBack")).toBool(true),
                    QStringLiteral("an authored six-way map without the RGB override keys keeps sampling RGB (overrides written false on load)"));
                check(masks.contains(QStringLiteral("overrideMaskA")) && !masks.value(QStringLiteral("overrideMaskR")).toBool(true),
                    QStringLiteral("an authored state-mask map without override keys keeps sampling all four channels"));
                check(room.contains(QStringLiteral("overrideWindowMask")) && !room.value(QStringLiteral("overrideWindowMask")).toBool(true),
                    QStringLiteral("an authored interior cubemap without the window-mask override key keeps reading its alpha"));
            }
            {
                // 2026-09-17: emission is luminance in nits; intensity x 100.
                QJsonObject nitsRoot = v5Root(6, QJsonObject{{QStringLiteral("specularWeight"), 1.0}}, QJsonObject(), true);
                QJsonObject nitsPrimary = nitsRoot.value(QStringLiteral("primaryUv")).toObject();
                nitsPrimary.insert(QStringLiteral("primaryEmissive"), QJsonObject{{QStringLiteral("enabled"), true}, {QStringLiteral("expanded"), true},
                    {QStringLiteral("path"), QStringLiteral("textures/test/primaryEmissive.dds")},
                    {QStringLiteral("values"), QJsonObject{{QStringLiteral("intensity"), 2.5}}}});
                nitsRoot.insert(QStringLiteral("primaryUv"), nitsPrimary);
                Pbr::MaterialDocument nits;
                QString nitsError;
                check(nits.fromJson(nitsRoot, &nitsError), QStringLiteral("emissive-intensity document loads: %1").arg(nitsError));
                const auto em = nits.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryEmissive")).values;
                check(!em.contains(QStringLiteral("intensity")) && std::abs(em.value(QStringLiteral("luminance")).toDouble() - 250.0) < 1e-9,
                    QStringLiteral("emissive intensity 2.5 loads as luminance 250 nits and the intensity key is gone"));
                Pbr::MaterialDocument nitsAgain;
                QString nitsAgainError;
                check(nitsAgain.fromJson(nits.toJson(), &nitsAgainError)
                        && std::abs(nitsAgain.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryEmissive")).values.value(QStringLiteral("luminance")).toDouble() - 250.0) < 1e-9,
                    QStringLiteral("the luminance migration is idempotent (a second load keeps 250 nits)"));
            }
            const auto sss = moved.textureSlot(QStringLiteral("UV0"), QStringLiteral("primarySubsurface"));
            check(sss.enabled && sss.values.value(QStringLiteral("mode")).toString() == QLatin1String("Combined")
                    && sss.values.value(QStringLiteral("profile")).toString() == QLatin1String("Custom")
                    && qFuzzyCompare(sss.values.value(QStringLiteral("weight")).toDouble(), 1.5)
                    && qFuzzyCompare(sss.values.value(QStringLiteral("thickness")).toDouble(), 6.0),
                QStringLiteral("v6: subsurface parameters move onto the input (page weight wins, thickness = scale x slot), and the input is enabled for a subsurface family"));
            const auto fx = moved.textureSlot(QStringLiteral("UV0"), QStringLiteral("primarySurfaceEffects"));
            check(fx.enabled && qFuzzyCompare(fx.values.value(QStringLiteral("fuzzStrength")).toDouble(), 0.7) && !fx.values.contains(QStringLiteral("sheenStrength")),
                QStringLiteral("v6: a lit surface-effect strength enables the surface effects input, and sheen lands as fuzz"));
            const auto retro = moved.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryRetroreflection"));
            check(retro.enabled && qFuzzyCompare(retro.values.value(QStringLiteral("retroStrength")).toDouble(), 0.5)
                    && !fx.values.contains(QStringLiteral("retroStrength")),
                QStringLiteral("v6: a lit retroreflection strength enables the retroreflection input, not the surface effects input"));
            check(qFuzzyCompare(moved.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryHairScatter")).values.value(QStringLiteral("scatterStrength")).toDouble(), 1.25)
                    && qFuzzyCompare(moved.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryHairDirection")).values.value(QStringLiteral("primaryShift")).toDouble(), -0.1)
                    && qFuzzyCompare(moved.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryTransmissionAbsorption")).values.value(QStringLiteral("ior")).toDouble(), 1.52)
                    && moved.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryTransmissionAbsorption")).values.value(QStringLiteral("thinWalled")).toBool(false)
                    && qFuzzyCompare(moved.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryCornea")).values.value(QStringLiteral("ior")).toDouble(), 1.4)
                    && qFuzzyCompare(moved.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryIrisMasks")).values.value(QStringLiteral("pupilScale")).toDouble(), 1.3),
                QStringLiteral("v6: hair, glass and eye parameters land on their inputs (scatter/backlight renamed *Strength, corneaIor -> ior)"));
            const QJsonObject settingsLeft = moved.toJson().value(QStringLiteral("settings")).toObject();
            check(!settingsLeft.contains(QStringLiteral("Specular")) && !settingsLeft.contains(QStringLiteral("Shading/Subsurface"))
                    && !settingsLeft.contains(QStringLiteral("Shading/SurfaceEffects")) && !settingsLeft.contains(QStringLiteral("Shading/Hair"))
                    && !settingsLeft.contains(QStringLiteral("Shading/Glass")) && !settingsLeft.contains(QStringLiteral("Shading/Eye")),
                QStringLiteral("v6: the retired setting groups are gone after the move"));
            QJsonObject disabledSss = legacy;
            QJsonObject disabledSettings = disabledSss.value(QStringLiteral("settings")).toObject();
            disabledSettings.insert(QStringLiteral("Shading/Subsurface"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("Disabled")}});
            disabledSss.insert(QStringLiteral("settings"), disabledSettings);
            Pbr::MaterialDocument off;
            check(off.fromJson(disabledSss, &v6Error) && !off.textureSlot(QStringLiteral("UV0"), QStringLiteral("primarySubsurface")).enabled
                    && !off.textureSlot(QStringLiteral("UV0"), QStringLiteral("primarySubsurface")).values.contains(QStringLiteral("mode")),
                QStringLiteral("v6: subsurface mode Disabled becomes a disabled input"));
        }
        check(high.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryNormal")).values.value(QStringLiteral("alphaCarries")).toString(QStringLiteral("None")) == QLatin1String("None"),
            QStringLiteral("v6: a normal without curvature gets the None selector"));

        const QJsonObject authored{{QStringLiteral("f0"), 0.04}, {QStringLiteral("overrideF0"), false}};
        Pbr::MaterialDocument marked;
        check(marked.fromJson(v5Root(5, authored, QJsonObject(), true), &v6Error), QStringLiteral("v6: authored-alpha document loads"));
        const QJsonObject markedValues = marked.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryRmaos")).values;
        check(markedValues.value(QStringLiteral("legacyF0Alpha")).toBool(false) && markedValues.value(QStringLiteral("overrideSpecularWeight")).toBool(false),
            QStringLiteral("v6: an authored F0 alpha is marked legacy and the constant override is forced on"));
        check(marked.textureSlot(QStringLiteral("UV0"), QStringLiteral("primarySpecularColor")).values.value(QStringLiteral("legacyTintAlpha")).toBool(false),
            QStringLiteral("v6: a v5 specular colour map is marked as carrying a tint-weight alpha"));

        const QJsonObject porosity{{QStringLiteral("f0"), 0.04}, {QStringLiteral("overrideF0"), false}, {QStringLiteral("alphaCarries"), QStringLiteral("Porosity")}};
        Pbr::MaterialDocument porous;
        check(porous.fromJson(v5Root(5, porosity, QJsonObject(), false), &v6Error), QStringLiteral("v6: porosity document loads"));
        check(!porous.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryRmaos")).values.contains(QStringLiteral("legacyF0Alpha")),
            QStringLiteral("v6: a porosity alpha carries no F0 and gets no marker"));

        Pbr::MaterialDocument gate;
        check(!gate.fromJson(v5Root(3, constantLow, QJsonObject(), false), &v6Error), QStringLiteral("v6: schema 3 is refused"));
        check(!gate.fromJson(v5Root(7, constantLow, QJsonObject(), false), &v6Error), QStringLiteral("v6: schema 7 is refused"));
        check(gate.fromJson(v5Root(6, QJsonObject{{QStringLiteral("specularWeight"), 0.7}}, QJsonObject(), false), &v6Error),
            QStringLiteral("v6: schema 6 loads: %1").arg(v6Error));
        check(qFuzzyCompare(gate.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryRmaos")).values.value(QStringLiteral("specularWeight")).toDouble(), 0.7)
                && gate.toJson().value(QStringLiteral("schemaVersion")).toInt() == 6,
            QStringLiteral("v6: a v6 document is not migrated and saves as schema 6"));

        // Hermetic repacks.
        check(qFuzzyCompare(SpecularMigrate::specularIorForF0(0.04), 1.5), QStringLiteral("v6: F0 0.04 is IOR 1.5"));
        QImage f0Map(2, 1, QImage::Format_RGBA8888);
        f0Map.setPixelColor(0, 0, QColor(10, 20, 30, 10)); // F0 0.039: weight 250, material IOR
        f0Map.setPixelColor(1, 0, QColor(10, 20, 30, 41)); // F0 0.161: weight 1, IOR 2.339
        const SpecularMigrate::F0Repack repack = SpecularMigrate::repackF0Alpha(f0Map, 1.5, 4.25);
        check(repack.rmaos.pixelColor(0, 0).alpha() == 250 && repack.rmaos.pixelColor(1, 0).alpha() == 255
                && repack.rmaos.pixelColor(0, 0).red() == 10,
            QStringLiteral("v6 repack: alpha becomes weight (F0/0.04, capped at 1), RGB untouched"));
        check(!repack.iorAlpha.isNull() && repack.iorAlpha.pixelColor(0, 0).red() == 90 && repack.iorAlpha.pixelColor(1, 0).red() == 140
                && repack.texelsAboveDefault == 1 && repack.texelsAboveCeiling == 0,
            QStringLiteral("v6 repack: the IOR map carries the material IOR (texel 90) and IOR 2.339 (texel 140)"));
        QImage lowOnly(1, 1, QImage::Format_RGBA8888);
        lowOnly.setPixelColor(0, 0, QColor(0, 0, 0, 5));
        check(SpecularMigrate::repackF0Alpha(lowOnly, 1.5, 4.25).iorAlpha.isNull(),
            QStringLiteral("v6 repack: no texel above 0.04 means no IOR map"));
        QImage tint(2, 1, QImage::Format_RGBA8888);
        tint.setPixelColor(0, 0, QColor(0, 0, 0, 0));
        tint.setPixelColor(1, 0, QColor(0, 0, 0, 255));
        const QImage baked = SpecularMigrate::bakeTintAlpha(tint, 1.5, 4.25);
        check(baked.pixelColor(0, 0).red() == 255 && baked.pixelColor(1, 0).red() == 0 && baked.pixelColor(0, 0).alpha() == 90,
            QStringLiteral("v6 bake: tint weight 0 is white, 1 is the colour, alpha is the material IOR"));
        check(SpecularMigrate::specularPathFor(QStringLiteral("textures/foo_RMAOS.dds")) == QStringLiteral("textures/foo_spec.dds")
                && SpecularMigrate::specularPathFor(QStringLiteral("a\\b_n.dds")) == QStringLiteral("a\\b_n_spec.dds"),
            QStringLiteral("v6 repack: the specular map name derives from the RMAOS name"));

        // Converter anchors: the level moved from the alpha into the IOR.
        const auto f0Of = [](float ior) { const float r = (ior - 1.0f) / (ior + 1.0f); return r * r; };
        check(std::abs(f0Of(Converter::fo4SpecularIor(Converter::Options{})) - 0.04f / 0.212f) < 1e-4f,
            QStringLiteral("v6 converter: the FO4 IOR reproduces the corpus anchor F0 0.04/0.212"));
        check(std::abs(f0Of(Converter::fo76SpecularIor(Converter::Options{})) - 0.08f) < 1e-5f,
            QStringLiteral("v6 converter: the FO76 IOR reproduces the old window ceiling F0 0.08"));
    }

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
    {
        Pbr::TextureSlotState subsurfaceSlot = document.textureSlot(QStringLiteral("UV0"), QStringLiteral("primarySubsurface"));
        subsurfaceSlot.values.insert(QStringLiteral("customRadiusR"), 1.25);
        document.setTextureSlot(QStringLiteral("UV0"), QStringLiteral("primarySubsurface"), subsurfaceSlot);
    }
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
    check(loaded.textureSlot(QStringLiteral("UV0"), QStringLiteral("primarySubsurface")).values.value(QStringLiteral("customRadiusR")).toDouble() == 1.25
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
        check(v5Root.value(QStringLiteral("schemaVersion")).toInt() == 6,
            QStringLiteral("migrated documents serialize as schema version 6"));
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
        // A template that names itself is vanilla, not a fault:
        // Template/DefaultTemplate_Wet.bgsm does exactly this, and roughly 3400
        // of the 6616 vanilla materials inherit through it. Revisiting a
        // template can contribute nothing — every pass only fills fields the
        // child left EMPTY — so the chain is complete, not broken.
        Bgsm::Material cyclic;
        check(Bgsm::read(v2.data, &cyclic, &bgsmError), QStringLiteral("cycle-test child reparses"));
        const auto cyclicLookup = [&v2](const QString&, QByteArray* data) { *data = v2.data; return true; };
        check(Bgsm::resolveRootChain(&cyclic, cyclicLookup),
            QStringLiteral("a self-referencing template resolves instead of erroring"));
        check(cyclic.diffuseTexture == QStringLiteral("textures\\armor\\a_d.dds"),
            QStringLiteral("and the borrow that did happen is kept"));
        // An unresolvable root is still a real failure.
        Bgsm::Material orphan;
        check(Bgsm::read(v2.data, &orphan, &bgsmError), QStringLiteral("orphan-test child reparses"));
        const auto missingLookup = [](const QString&, QByteArray*) { return false; };
        check(!Bgsm::resolveRootChain(&orphan, missingLookup),
            QStringLiteral("a root that cannot be resolved at all is still rejected"));

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
            builder.str(QString());                        // greyscale
            // Slot 5 is the cubemap, and in Fallout 4 the cubemap IS the metal
            // declaration — nothing else in the format states metallicity. The
            // file does not exist here on purpose: the name drives the metal
            // decision, while the tint donor falls back to neutral.
            builder.str(QStringLiteral("shared/cubemaps/metalchrome01cube_e.dds"));
            builder.str(QString()); builder.str(QString());
            builder.str(QString()); builder.str(QString());
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
            QStringLiteral("strong spec stencils metal (coverage %1)").arg(first.metalCoverage));
        check(first.flagged,
            QStringLiteral("unmasked spec-derived metal is flagged as unfinished work"));
        {
            // Setting the window past the top of the range converts fully
            // dielectric, which is how a material with no metal in it is
            // resolved. Its own output root: texture names come from the SOURCE
            // textures rather than the material, so both runs would write
            // armor/conv_BC.dds, and a second TextureCache cannot see the first
            // run's claim — the dielectric pass would silently overwrite the
            // metal one the assertions below go on to read.
            QTemporaryDir dielectricOut;
            Converter::JobOptions dielectric = jobOptions;
            dielectric.math.metalSpecStart = 1.0f;
            dielectric.math.metalSpecFull = 1.0f;
            dielectric.outputRoot = dielectricOut.path();
            Converter::TextureCache dielectricCache;
            const Converter::JobResult plain =
                Converter::convertMaterial(buildConvMaterial(QStringLiteral("convDielectric.bgsm")),
                    dielectric, &dielectricCache);
            check(plain.converted && qFuzzyIsNull(plain.metalCoverage) && !plain.flagged,
                QStringLiteral("closing the window converts dielectric, and nothing is flagged"));
        }
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
        check(convertedNormalSlot.values.value(QStringLiteral("alphaCarries")).toString() == QLatin1String("Curvature"),
            QStringLiteral("normal slot declares curvature in alpha (v6 selector)"));

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

        // FO4 metal declaration: the cubemap name is the ONLY place Fallout 4
        // ever states metallicity, so these strings are load-bearing.
        check(Converter::cubemapDeclaresMetal(QStringLiteral("textures\\shared\\cubemaps\\MetalChrome01Cube_e.dds"))
            && Converter::cubemapDeclaresMetal(QStringLiteral("Textures/Shared/Cubemaps/bronze_e.dds"))
            && Converter::cubemapDeclaresMetal(QStringLiteral("metalbrushed02cube_e.dds")),
            QStringLiteral("metal-named cubemaps declare metal, whatever the case or separator"));
        check(!Converter::cubemapDeclaresMetal(QStringLiteral("textures\\shared\\cubemaps\\mipblur_defaultoutside1.dds"))
            && !Converter::cubemapDeclaresMetal(QStringLiteral("outsideoldtownreflectcube_e.dds"))
            && !Converter::cubemapDeclaresMetal(QString()),
            QStringLiteral("the generic sky cubes declare nothing — they are used on brick"));
        // It orders the review queue; it does not gate a single texel.
        {
            Bgsm::Material declared, generic;
            declared.envmapTexture = QStringLiteral("shared/cubemaps/metalchrome01cube_e.dds");
            generic.envmapTexture = QStringLiteral("shared/cubemaps/mipblur_defaultoutside1.dds");
            QImage bright(4, 4, QImage::Format_RGBA8888);
            bright.fill(QColor(200, 128, 0));
            Converter::Fo4Sources sameTextures;
            sameTextures.diffuse = bright;
            sameTextures.specular = bright;
            const float a = Converter::convertFo4(sameTextures, declared, options).metalCoverage;
            const float b = Converter::convertFo4(sameTextures, generic, options).metalCoverage;
            check(qFuzzyCompare(a + 1.0f, b + 1.0f) && a > 0.9f,
                QStringLiteral("the cubemap name changes no pixel — the stencil is the same either way"));
        }
        // Deliberately NOT tempting: mipblur_defaultoutside1 is 68% metal by
        // asset naming and would be the single biggest win by volume, but the
        // other 32% are brick walls and hair, and admitting it chromes them.
        check(!Converter::cubemapDeclaresMetal(QStringLiteral("mipblur_defaultoutside1_dielectric.dds")),
            QStringLiteral("a cubemap's own _dielectric suffix is not a surface claim either"));

        // FO4 metal is a stencil off _s.R: bright spec is a metal candidate,
        // dark spec is not, and the mask subtracts the rest.
        check(Converter::metallicConfidenceFo4(0.9f, options) > 0.9f,
            QStringLiteral("FO4 stencils metal from bright spec"));
        check(qFuzzyIsNull(Converter::metallicConfidenceFo4(0.1f, options)),
            QStringLiteral("and leaves dark-spec grime dielectric"));
        // The window is narrow so it commits, and low so real metal survives.
        check(Converter::metallicConfidenceFo4(0.19f, options) < 0.02f
            && Converter::metallicConfidenceFo4(0.31f, options) > 0.98f,
            QStringLiteral("the stencil window commits rather than smearing into the grey band"));
        // specularMult must NOT reach the stencil. It scales the specular
        // response, not the material class: MetalVerTrimFuturistic01 is metal
        // trim peaking at 84/255 with a mult of 0.400, and folding the mult in
        // stencilled it at 0% metal.
        {
            QImage trim(4, 4, QImage::Format_RGBA8888);
            trim.fill(QColor(84, 128, 0));      // the real material's p90
            Bgsm::Material dimmed;
            dimmed.specularMult = 0.4f;
            Converter::Fo4Sources trimSources;
            trimSources.diffuse = trim;
            trimSources.specular = trim;
            check(Converter::convertFo4(trimSources, dimmed, options).metalCoverage > 0.9f,
                QStringLiteral("a low specularMult dims the highlight without erasing the metal"));
        }
        {
            // Fully dielectric is still reachable, without a separate switch.
            Converter::Options dielectric = options;
            dielectric.metalSpecStart = 1.0f;
            dielectric.metalSpecFull = 1.0f;
            check(qFuzzyIsNull(Converter::metallicConfidenceFo4(0.99f, dielectric)),
                QStringLiteral("a start of 1 converts fully dielectric"));
        }

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
        fo4Material.envmapTexture = QStringLiteral("textures\\shared\\cubemaps\\metalchrome01cube_e.dds");
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
        // FO4's _s is BC5 — R specular strength, G gloss — so there is no color
        // in it to take. The color an FO4 material does author is
        // cSpecularColor, and the default white one must add nothing.
        check(fo4Out.specularColor.isNull(),
            QStringLiteral("a white FO4 specular constant leaves the material exactly as it was"));
        Bgsm::Material tintedFo4 = fo4Material;
        tintedFo4.specularColor = QVector3D(0.945f, 0.220f, 0.012f); // vanilla BloodBugFur
        check(!Converter::convertFo4(fo4Sources, tintedFo4, options).specularColor.isNull(),
            QStringLiteral("an authored FO4 specular color becomes the material's specular tint"));

        // The mask is what removes metal the stencil got wrong — the brick-wall
        // case. Painting black over the metal half collapses it.
        {
            QImage mask(8, 8, QImage::Format_RGBA8888);
            mask.fill(QColor(128, 128, 128));   // 0.5 neutral: change nothing
            for (int y = 0; y < 8; ++y)
                for (int x = 4; x < 8; ++x) mask.setPixelColor(x, y, QColor(0, 0, 0));
            Converter::Fo4Sources masked = fo4Sources;
            masked.correctionMask = mask;
            const Converter::ConvertedImages out = Converter::convertFo4(masked, fo4Material, options);
            check(qFuzzyIsNull(out.metalCoverage)
                && QColor(out.rmaos.pixelColor(6, 4)).greenF() < 0.05,
                QStringLiteral("painting the mask black subtracts metal the stencil got wrong"));
        }

        // The .bgsm smoothness scalar is the gloss where _s.G says nothing, and
        // is ignored where it does. Measured: non-default in 80.9% of flat-G
        // maps against 20.8% of textured ones.
        {
            // Flat G is written PURE WHITE in vanilla — measured median 255
            // across the 827 materials whose gloss channel carries nothing,
            // against a median scalar of 0.50. Read literally that is gloss 1.0
            // and a mirror finish on 13.4% of the corpus, so the fallback is
            // load-bearing, not a nicety.
            QImage flatGloss(4, 4, QImage::Format_RGBA8888);
            flatGloss.fill(QColor(120, 255, 0));   // R spec .47, G gloss flat at white
            Bgsm::Material scalarMaterial;
            scalarMaterial.smoothness = 0.2f;      // rough, and the only real signal
            Converter::Fo4Sources scalarSources;
            scalarSources.diffuse = diffuse;
            scalarSources.specular = flatGloss;
            const Converter::ConvertedImages out = Converter::convertFo4(scalarSources, scalarMaterial, options);
            check(QColor(out.rmaos.pixelColor(1, 1)).redF() > 0.7,
                QStringLiteral("a white flat _s.G falls back to the scalar instead of shipping a mirror"));

            Bgsm::Material texturedMaterial = scalarMaterial;
            const Converter::ConvertedImages textured =
                Converter::convertFo4(fo4Sources, texturedMaterial, options);
            check(QColor(textured.rmaos.pixelColor(6, 4)).redF() < 0.3,
                QStringLiteral("a textured _s.G ignores the scalar rather than multiplying by it"));
        }

        // Dielectric F0 is anchored on the corpus median, not on specR * 0.08 —
        // which put the median texel of the median material under the 0.02
        // floor and pinned the whole axis flat.
        {
            QImage medianSpec(4, 4, QImage::Format_RGBA8888);
            medianSpec.fill(QColor(54, 128, 0));   // the measured _s.R median
            Bgsm::Material plain;
            Converter::Fo4Sources plainSources;
            plainSources.diffuse = diffuse;
            plainSources.specular = medianSpec;
            const Converter::ConvertedImages out = Converter::convertFo4(plainSources, plain, options);
            // v6: alpha is the raw weight; the level is the material IOR the
            // converter anchors, so weight * F0(ior) is what the median renders.
            const double weight = QColor(out.rmaos.pixelColor(1, 1)).alphaF();
            const double iorF0 = std::pow((out.specularIor - 1.0) / (out.specularIor + 1.0), 2.0);
            const double f0 = weight * iorF0;
            check(f0 > 0.035 && f0 < 0.045,
                QStringLiteral("the corpus-median specular weight times the FO4 IOR lands on the physical dielectric 0.04 (%1)").arg(f0));
            check(weight > 0.15 && weight < 0.3,
                QStringLiteral("the corpus-median specular is written raw as the weight (%1)").arg(weight));
        }

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

        // SPECULAR TINT. The F0 above is a luminance, and a scalar cannot hold
        // the hue of an RGB reflectance map. The tint keeps that hue at unit
        // brightness so the level stays with RMAOS.A.
        check(!fo76Out.specularColor.isNull(),
            QStringLiteral("a colored FO76 reflectance map yields a specular tint"));
        if (!fo76Out.specularColor.isNull()) {
            const QColor tint = fo76Out.specularColor.pixelColor(2, 2);
            check(tint.red() > tint.green() && tint.green() > tint.blue() && tint.alpha() == 255,
                QStringLiteral("the tint keeps the reflectance hue, warm for gold, at full weight"));
            // The LEVEL must not move: RMAOS.A is what it was before the tint
            // existed, so the tint can only redistribute, never brighten.
            Converter::Fo76Sources greySources = fo76Sources;
            QImage greyReflectance(4, 4, QImage::Format_RGBA8888);
            greyReflectance.fill(QColor(160, 160, 160));
            greySources.reflectance = greyReflectance;
            const Converter::ConvertedImages greyOut =
                Converter::convertFo76(greySources, fo76Material, options);
            check(greyOut.specularColor.isNull(),
                QStringLiteral("an achromatic reflectance map earns no tint texture"));
        }
        // The two identity paths, on the constant form: white is no tint at all.
        check(Converter::constantSpecularTint(QVector3D(1, 1, 1)).isNull(),
            QStringLiteral("a white specular constant produces no tint texture"));
        const QImage blueTint = Converter::constantSpecularTint(QVector3D(0.506f, 0.659f, 1.0f));
        check(!blueTint.isNull() && QColor(blueTint.pixel(0, 0)).blue() == 255
                && QColor(blueTint.pixel(0, 0)).red() < 140 && qAlpha(blueTint.pixel(0, 0)) == 255,
            QStringLiteral("a colored specular constant becomes a flat full-weight tint"));

        // Encoding: FO76 authors _r as BC1_UNORM_SRGB, so a dielectric sitting at
        // linear F0 0.04 arrives as byte 56 (sRGB 0.22). Measured without
        // decoding it reads 0.22 -- inside the 0.12-0.40 metal ramp and above the
        // 0.08 F0 ceiling -- which is grey metalness on painted surfaces and a
        // maxed-out F0 everywhere. Decoded it lands back at 0.04: fully
        // dielectric, F0 mid-band. The diffuse here is near-black so the
        // dark-diffuse gate passes and metallic is decided by reflectance alone.
        QImage darkDiffuse(4, 4, QImage::Format_RGBA8888);
        darkDiffuse.fill(QColor(3, 3, 3));
        QImage dielectricRefl(4, 4, QImage::Format_RGBA8888);
        dielectricRefl.fill(QColor(56, 56, 56)); // sRGB encoding of linear 0.04
        Converter::Fo76Sources dielectricSources;
        dielectricSources.diffuse = darkDiffuse;
        dielectricSources.reflectance = dielectricRefl;
        const Converter::ConvertedImages dielectricOut =
            Converter::convertFo76(dielectricSources, fo76Material, options);
        const QColor dielectricRmaos = dielectricOut.rmaos.pixelColor(2, 2);
        check(dielectricRmaos.greenF() < 0.02,
            QStringLiteral("FO76 a dielectric _r reads as non-metal, not grey (metallic %1)")
                .arg(dielectricRmaos.greenF()));
        const double fo76IorF0 = std::pow((dielectricOut.specularIor - 1.0) / (dielectricOut.specularIor + 1.0), 2.0);
        check(std::abs(dielectricRmaos.alphaF() * fo76IorF0 - 0.04) < 0.015,
            QStringLiteral("FO76 dielectric weight times the FO76 IOR lands mid-band, not pinned at the 0.08 ceiling (%1)")
                .arg(dielectricRmaos.alphaF()));

        QImage metalRefl(4, 4, QImage::Format_RGBA8888);
        metalRefl.fill(QColor(243, 243, 243)); // sRGB encoding of linear 0.90
        Converter::Fo76Sources metalSources;
        metalSources.diffuse = darkDiffuse;
        metalSources.reflectance = metalRefl;
        const Converter::ConvertedImages metalOut =
            Converter::convertFo76(metalSources, fo76Material, options);
        check(metalOut.rmaos.pixelColor(2, 2).greenF() > 0.98,
            QStringLiteral("FO76 a bright _r still reads as fully metal once decoded (metallic %1)")
                .arg(metalOut.rmaos.pixelColor(2, 2).greenF()));

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

        // FO76 translucency. Selecting the subsurface shader family is not
        // enough: the authored scatter colour and transmissive scale have to
        // land in the document, or the conversion picks a subsurface material
        // and then ships it with default scattering.
        // v6: the values live on the primarySubsurface input, which the
        // conversion enables so the feature is on without a map.
        const auto sssValue = [](const Pbr::MaterialDocument& doc, const QString& key) {
            return doc.textureSlot(QStringLiteral("UV0"), QStringLiteral("primarySubsurface")).values.value(key);
        };
        Bgsm::Material thin;
        thin.version = 20;
        thin.translucency = true;
        thin.translucencyThickObject = false;
        thin.translucencyTransmissiveScale = 1.75f;
        thin.translucencySubsurfaceColor = QVector3D(0.9f, 0.35f, 0.28f);
        thin.translucencyTurbulence = 0.4f;
        Pbr::MaterialDocument thinConverted;
        QStringList thinUnhonored;
        Converter::mapParameters(thin, &thinConverted, &thinUnhonored);
        check(thinConverted.shaderMode() == Pbr::ShaderMode::AdvancedSubsurface
                && sssValue(thinConverted, QStringLiteral("mode")).toString()
                    == QStringLiteral("Thin Transmission"),
            QStringLiteral("a thin FO76 translucency maps to Thin Transmission"));
        check(thinConverted.textureSlot(QStringLiteral("UV0"), QStringLiteral("primarySubsurface")).enabled,
            QStringLiteral("the conversion enables the subsurface input so the feature is on (v6: slot on = feature on)"));
        check(std::abs(sssValue(thinConverted, QStringLiteral("transmission")).toDouble() - 1.75) < 1e-6,
            QStringLiteral("the authored transmissive scale reaches the document"));
        check(sssValue(thinConverted, QStringLiteral("profile")).toString() == QStringLiteral("Custom")
                && QColor(sssValue(thinConverted, QStringLiteral("customScatterColor")).toString()).isValid(),
            QStringLiteral("an authored subsurface colour becomes a Custom diffusion profile"));
        check(thinUnhonored.join(QStringLiteral(";")).contains(QStringLiteral("Turbulence")),
            QStringLiteral("turbulence has no equivalent and is recorded as unhonored"));

        Bgsm::Material thick;
        thick.version = 20;
        thick.translucency = true;
        thick.translucencyThickObject = true;
        Pbr::MaterialDocument thickConverted;
        Converter::mapParameters(thick, &thickConverted, nullptr);
        check(sssValue(thickConverted, QStringLiteral("mode")).toString() == QStringLiteral("Combined"),
            QStringLiteral("a thick FO76 translucency scatters through the volume too"));
        check(sssValue(thickConverted, QStringLiteral("profile")).toString(QStringLiteral("Skin"))
                == QStringLiteral("Skin"),
            QStringLiteral("a default white subsurface colour leaves the stock profile alone"));

        // FO4's rolloff normalises against its own default, so an untouched
        // material lands on the editor's neutral weight.
        Bgsm::Material fo4Sss;
        fo4Sss.version = 2;
        fo4Sss.subsurfaceLighting = true;
        fo4Sss.subsurfaceLightingRolloff = 0.3f;
        Pbr::MaterialDocument fo4SssConverted;
        Converter::mapParameters(fo4Sss, &fo4SssConverted, nullptr);
        check(sssValue(fo4SssConverted, QStringLiteral("mode")).toString()
                    == QStringLiteral("Diffusion Profile")
                && std::abs(sssValue(fo4SssConverted, QStringLiteral("weight")).toDouble() - 1.0) < 1e-6,
            QStringLiteral("FO4 subsurface lighting maps to a diffusion profile at neutral weight"));

        // FO76 packs a subsurface mask into _l.B and a glow mask into _l.A.
        // The alpha half is only usable when the file really has alpha: many
        // vanilla _l maps are BC1, whose alpha decodes to a constant 1.0, and
        // reading that as a mask would make the entire surface emit.
        {
            const int size = 8;
            QImage lighting(size, size, QImage::Format_RGBA8888);
            for (int y = 0; y < size; ++y) {
                uchar* line = lighting.scanLine(y);
                for (int x = 0; x < size; ++x) {
                    line[x * 4 + 0] = 160;                    // smoothness
                    line[x * 4 + 1] = 220;                    // baked AO
                    line[x * 4 + 2] = uchar(x * 255 / (size - 1)); // subsurface mask
                    line[x * 4 + 3] = 255;                    // BC1-style constant alpha
                }
            }
            QImage diffuse(size, size, QImage::Format_RGBA8888);
            diffuse.fill(QColor(120, 100, 90, 255));

            Converter::Fo76Sources sources;
            sources.diffuse = diffuse;
            sources.lighting = lighting;
            Bgsm::Material scattering;
            scattering.version = 20;
            scattering.translucency = true;
            scattering.emitEnabled = true; // emits, but the _l alpha is degenerate
            const Converter::ConvertedImages scattered =
                Converter::convertFo76(sources, scattering, Converter::Options{});
            check(!scattered.subsurface.isNull()
                    && qGray(scattered.subsurface.pixel(0, 0)) == 0
                    && qGray(scattered.subsurface.pixel(size - 1, 0)) == 255,
                QStringLiteral("_l.B becomes the subsurface mask"));
            check(scattered.emissive.isNull(),
                QStringLiteral("a constant _l alpha is not mistaken for a glow mask"));

            // Give the alpha real variation and it becomes the glow mask.
            for (int y = 0; y < size; ++y) {
                uchar* line = lighting.scanLine(y);
                for (int x = 0; x < size; ++x) line[x * 4 + 3] = uchar(x * 255 / (size - 1));
            }
            sources.lighting = lighting;
            const Converter::ConvertedImages glowing =
                Converter::convertFo76(sources, scattering, Converter::Options{});
            check(!glowing.emissive.isNull()
                    && qAlpha(glowing.emissive.pixel(0, 0)) == 0
                    && qAlpha(glowing.emissive.pixel(size - 1, 0)) == 255,
                QStringLiteral("a varying _l.A becomes the glow mask when no _g is authored"));

            // A material that does not scatter gets no subsurface map at all.
            Bgsm::Material plainFo76;
            plainFo76.version = 20;
            const Converter::ConvertedImages plainScatter =
                Converter::convertFo76(sources, plainFo76, Converter::Options{});
            check(plainScatter.subsurface.isNull(),
                QStringLiteral("a non-scattering material gets no subsurface map"));
        }

        // A material with no subsurface data must not gain the group at all.
        Bgsm::Material plain;
        plain.version = 20;
        Pbr::MaterialDocument plainConverted;
        Converter::mapParameters(plain, &plainConverted, nullptr);
        check(sssValue(plainConverted, QStringLiteral("mode")).toString().isEmpty(),
            QStringLiteral("a material without subsurface data gets no subsurface settings"));
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
        // The payload is Indented for human readability (formatting is free —
        // both the editor and the FO4CS runtime parser skip whitespace), so
        // exactness is judged on the parsed document, not the bytes.
        check(QJsonDocument::fromJson(savedPayload).object() == expected,
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

    {
        // Node graph: core mechanics (typing, cycles, ordering, serialization,
        // caching) plus the converter library's parity with the orchestrator's
        // math — a graph and a batch run must agree.
        NodeGraph::Registry registry;
        ConverterNodes::registerAll(registry);
        check(registry.find(QStringLiteral("unpack.fo4")) && registry.find(QStringLiteral("unpack.fo76"))
                && registry.find(QStringLiteral("derive.normal")) && registry.find(QStringLiteral("image.channelPack")),
            QStringLiteral("converter node library registers its pipeline stages"));
        // The whole-conversion nodes are RETIRED. A registered node no shipped
        // graph uses is a second definition of the same conversion waiting to
        // drift from the one that runs; the recipes are the graph presets now.
        check(!registry.find(QStringLiteral("convert.fo4"))
                && !registry.find(QStringLiteral("convert.fo76")),
            QStringLiteral("no whole-conversion node is registered for either title"));

        const NodeGraph::NodeTypeSpec* invertSpec = registry.find(QStringLiteral("image.invert"));
        const NodeGraph::NodeTypeSpec* deriveSpec = registry.find(QStringLiteral("derive.normal"));
        NodeGraph::Graph graph;
        const QString first = graph.addNode(QStringLiteral("image.invert"), *invertSpec, QPointF(0, 0));
        const QString second = graph.addNode(QStringLiteral("image.invert"), *invertSpec, QPointF(200, 0));
        const QString derive = graph.addNode(QStringLiteral("derive.normal"), *deriveSpec, QPointF(0, 200));
        QString linkError;
        check(graph.connect(registry, {first, QStringLiteral("image"), second, QStringLiteral("image")}, &linkError),
            QStringLiteral("compatible ports link: %1").arg(linkError));
        check(!graph.connect(registry, {second, QStringLiteral("image"), first, QStringLiteral("image")}, &linkError)
                && linkError.contains(QStringLiteral("cycle")),
            QStringLiteral("a link that would close a cycle is rejected"));
        check(!graph.connect(registry, {derive, QStringLiteral("noiseMetric"), first, QStringLiteral("image")}, &linkError),
            QStringLiteral("a scalar output cannot feed an image input"));
        check(graph.topologicalOrder().size() == 3, QStringLiteral("topological order covers every node"));
        const NodeGraph::Graph restored = NodeGraph::Graph::fromJson(graph.toJson());
        check(restored.nodes.size() == graph.nodes.size() && restored.connections.size() == graph.connections.size()
                && restored.node(first) && restored.node(first)->typeId == QStringLiteral("image.invert"),
            QStringLiteral("graphs round-trip through JSON"));

        // validate() ASSIGNS the error list, and check()'s two arguments are
        // evaluated in an unspecified order — inlining the call meant the
        // message was built from an empty list and a failure said nothing.
        QStringList graphErrors;
        const bool fo4Valid = ConverterNodes::defaultFo4Graph().validate(registry, &graphErrors);
        check(fo4Valid, QStringLiteral("default FO4 graph validates: %1").arg(graphErrors.join(QStringLiteral("; "))));
        const bool fo76Valid = ConverterNodes::defaultFo76Graph().validate(registry, &graphErrors);
        check(fo76Valid, QStringLiteral("default FO76 graph validates: %1").arg(graphErrors.join(QStringLiteral("; "))));

        // Node boxes are sized from the spec plus the node's own fold state, and
        // the stock layouts are built on that size. Hard-coded spacing is what
        // previously let stock graphs draw on top of themselves.
        {
            const NodeGraph::NodeTypeSpec* texture = registry.find(QStringLiteral("source.texture"));
            const NodeGraph::NodeTypeSpec* pack = registry.find(QStringLiteral("image.channelPack"));
            check(texture && pack, QStringLiteral("sizing needs the stock node types"));
            NodeGraph::Graph sizing;
            const QString texId = sizing.addNode(QStringLiteral("source.texture"), *texture, QPointF(0, 0));
            const QString packId = sizing.addNode(QStringLiteral("image.channelPack"), *pack, QPointF(400, 0));
            const QSizeF textureSize = NodeGraph::nodeSize(*texture, *sizing.node(texId), sizing);
            check(textureSize.height() > NodeGraph::Metrics::kThumbSize,
                QStringLiteral("a node with a preview is taller than the preview itself"));
            check(NodeGraph::nodeSize(*pack, *sizing.node(packId), sizing).height() > textureSize.height(),
                QStringLiteral("more ports and settings make a taller node"));

            for (const NodeGraph::Graph& stock :
                    {ConverterNodes::defaultFo4Graph(), ConverterNodes::defaultFo76Graph()}) {
                QVector<QRectF> boxes;
                for (const NodeGraph::Node& node : stock.nodes) {
                    const NodeGraph::NodeTypeSpec* spec = registry.find(node.typeId);
                    if (spec) boxes.append(QRectF(node.position, NodeGraph::nodeSize(*spec, node, stock)));
                }
                bool overlaps = false;
                for (int a = 0; a < boxes.size() && !overlaps; ++a)
                    for (int b = a + 1; b < boxes.size() && !overlaps; ++b)
                        overlaps = boxes[a].intersects(boxes[b]);
                check(!overlaps && boxes.size() == stock.nodes.size(),
                    QStringLiteral("no two nodes in a stock graph overlap"));
            }

            // Re-laying out a graph must not disturb what it computes.
            NodeGraph::Graph scrambled = ConverterNodes::defaultFo4Graph();
            const int connections = scrambled.connections.size();
            for (NodeGraph::Node& node : scrambled.nodes) node.position = QPointF(0, 0);
            NodeGraph::layoutGraph(scrambled, registry);
            check(scrambled.connections.size() == connections,
                QStringLiteral("arranging a graph preserves its wiring"));
            QSet<QString> places;
            for (const NodeGraph::Node& node : scrambled.nodes)
                places.insert(QStringLiteral("%1/%2").arg(node.position.x()).arg(node.position.y()));
            check(places.size() == scrambled.nodes.size(),
                QStringLiteral("arranging separates nodes that shared a position"));
        }

        // Splitting channels and the placeholder upscaler.
        {
            const NodeGraph::NodeTypeSpec* separate = registry.find(QStringLiteral("image.separate"));
            check(separate && separate->outputs.size() == 4,
                QStringLiteral("Separate Channels exposes R, G, B and A"));
            QImage source(4, 4, QImage::Format_RGBA8888);
            source.fill(QColor(200, 120, 40, 90));
            NodeGraph::EvalArgs args;
            args.inputs.insert(QStringLiteral("image"), NodeGraph::Value::fromImage(source));
            NodeGraph::EvalOutput separated;
            separate->evaluate(args, &separated);
            check(qGray(separated.outputs.value(QStringLiteral("r")).image().pixel(0, 0)) == 200
                    && qGray(separated.outputs.value(QStringLiteral("a")).image().pixel(0, 0)) == 90,
                QStringLiteral("Separate Channels routes each channel to its own output"));

            const NodeGraph::NodeTypeSpec* upscale = registry.find(QStringLiteral("image.upscale"));
            check(upscale, QStringLiteral("the upscaler is registered"));
            // Its own image at a real size: the node measures what it is handed,
            // because the resolution carried down a wire is the material's
            // largest map rather than this one's.
            QImage halfRes(256, 256, QImage::Format_RGBA8888);
            halfRes.fill(QColor(70, 90, 110, 255));
            NodeGraph::EvalArgs up;
            up.inputs.insert(QStringLiteral("image"), NodeGraph::Value::fromImage(halfRes));
            up.params = upscale->defaultParams();
            // Mode 2 = Match highest. The default is now "Project setting",
            // which defers to a policy that ships off, so an explicit mode is
            // needed to ask for the raise this test is about.
            up.params.insert(QStringLiteral("mode"), 2);
            up.context.materialMaxResolution = 512;
            up.context.previewMode = false;
            NodeGraph::EvalOutput raised;
            upscale->evaluate(up, &raised);
            check(raised.nativeResolution == 512 && raised.error.isEmpty(),
                QStringLiteral("the upscaler reports the material's resolution as its own"));
            check(raised.outputs.value(QStringLiteral("image")).image().width() == 512,
                QStringLiteral("the upscaler resamples by the resolution ratio"));

            // Already at the ceiling: nothing to do, and it says so.
            NodeGraph::EvalArgs same = up;
            same.context.materialMaxResolution = 256;
            NodeGraph::EvalOutput untouched;
            upscale->evaluate(same, &untouched);
            check(untouched.outputs.value(QStringLiteral("image")).image().size() == halfRes.size()
                    && untouched.nativeResolution == 256,
                QStringLiteral("an at-target image passes through the upscaler untouched"));
        }

        // Under-resolution is flagged against the material's own ceiling.
        {
            const NodeGraph::NodeTypeSpec* constant = registry.find(QStringLiteral("image.constant"));
            check(constant, QStringLiteral("the constant node backs the resolution test"));
            NodeGraph::Graph flagged;
            const QString small = flagged.addNode(QStringLiteral("image.constant"), *constant, QPointF());
            NodeGraph::EvalContext context;
            context.materialMaxResolution = 4096;
            NodeGraph::Evaluator resolutionEval(registry);
            const NodeGraph::NodeResult result = resolutionEval.evaluate(flagged, context).value(small);
            check(!result.warning.isEmpty() && result.warning.contains(QStringLiteral("4096")),
                QStringLiteral("a node below the material's resolution is warned about: %1").arg(result.warning));
            check(!result.status.isEmpty(),
                QStringLiteral("every image node reports its size: %1").arg(result.status));

            context.materialMaxResolution = 0; // unknown ceiling: nothing to flag
            NodeGraph::Evaluator quietEval(registry);
            check(quietEval.evaluate(flagged, context).value(small).warning.isEmpty(),
                QStringLiteral("no ceiling means no under-resolution warning"));
        }

        // The stock graphs must reproduce the orchestrator's writes, so every
        // shipped channel needs an explicit route into the terminal node. The
        // normal is the one that bit historically: when the graph packed it
        // channel by channel, leaving B unconnected defaulted it to white and
        // the written DDS stopped matching a batch run. Feeding the whole
        // source normal in makes that structurally impossible — this checks it
        // stays that way, and that each title still routes its own AO.
        const struct { const char* title; NodeGraph::Graph graph; bool derivesAo; } stockGraphs[] = {
            {"FO4", ConverterNodes::defaultFo4Graph(), true},
            {"FO76", ConverterNodes::defaultFo76Graph(), false}};
        for (const auto& stock : stockGraphs) {
            QStringList terminals;
            for (const NodeGraph::Node& node : stock.graph.nodes)
                if (node.typeId == QStringLiteral("output.pbrMaterial")) terminals.append(node.id);
            check(terminals.size() == 1,
                QStringLiteral("%1 stock graph ends in exactly one Output Material (PBR) node").arg(stock.title));

            // The terminal now takes finished textures only; packing happens in
            // Combine Maps (PBR), so the per-map guarantees are checked there and
            // the terminal is checked for receiving the packed results.
            QHash<QString, QString> intoTerminal;
            for (const NodeGraph::Connection& connection : stock.graph.connectionsTo(terminals.value(0)))
                intoTerminal.insert(connection.toPort, connection.fromNode);
            check(intoTerminal.contains(QStringLiteral("baseColor"))
                    && intoTerminal.contains(QStringLiteral("normal"))
                    && intoTerminal.contains(QStringLiteral("rmaos"))
                    && intoTerminal.contains(QStringLiteral("emissive")),
                QStringLiteral("%1 routes every shipped texture into the terminal: %2")
                    .arg(stock.title, intoTerminal.keys().join(QStringLiteral(","))));

            QStringList combiners;
            for (const NodeGraph::Node& node : stock.graph.nodes)
                if (node.typeId == QStringLiteral("composite.pbrChannels")) combiners.append(node.id);
            check(combiners.size() == 1,
                QStringLiteral("%1 packs its channels in exactly one place").arg(stock.title));
            // Everything the terminal receives must come from that one node —
            // no side route that skips the packing rules.
            check(std::all_of(intoTerminal.constBegin(), intoTerminal.constEnd(),
                    [&combiners](const QString& from) { return from == combiners.value(0); }),
                QStringLiteral("%1 feeds the terminal only from the combiner").arg(stock.title));

            QHash<QString, QString> wiredFrom; // combiner input -> source node
            for (const NodeGraph::Connection& connection : stock.graph.connectionsTo(combiners.value(0)))
                wiredFrom.insert(connection.toPort, connection.fromNode);
            check(wiredFrom.contains(QStringLiteral("baseColor"))
                    && wiredFrom.contains(QStringLiteral("normal"))
                    && wiredFrom.contains(QStringLiteral("curvature"))
                    && wiredFrom.contains(QStringLiteral("emissive")),
                QStringLiteral("%1 routes every shipped map into the combiner: %2")
                    .arg(stock.title, wiredFrom.keys().join(QStringLiteral(","))));

            // Where a graph has an unpack node, it must be the ONLY reader of
            // the source textures. Anything else reaching past it would be
            // working on packed data while claiming to work on single maps,
            // which is the confusion these nodes exist to remove.
            QStringList unpackers;
            for (const NodeGraph::Node& node : stock.graph.nodes)
                if (node.typeId.startsWith(QStringLiteral("unpack."))) unpackers.append(node.id);
            if (!unpackers.isEmpty()) {
                bool onlyUnpackReadsTextures = true;
                for (const NodeGraph::Connection& connection : stock.graph.connections) {
                    const NodeGraph::Node* from = stock.graph.node(connection.fromNode);
                    if (!from || from->typeId != QStringLiteral("source.texture")) continue;
                    if (!unpackers.contains(connection.toNode)) onlyUnpackReadsTextures = false;
                }
                check(onlyUnpackReadsTextures,
                    QStringLiteral("%1 routes every source texture through the unpack node first")
                        .arg(stock.title));

                // And the normal still arrives WHOLE rather than reassembled
                // from channels. It may pass through nodes that preserve it
                // intact — resolution levelling sits on this path — so walk
                // back through those and check where the map originates.
                QString normalOrigin = wiredFrom.value(QStringLiteral("normal"));
                for (int hop = 0; hop < 4; ++hop) {
                    const NodeGraph::Node* at = stock.graph.node(normalOrigin);
                    if (!at || at->typeId != QStringLiteral("image.upscale")) break;
                    QString feeding;
                    for (const NodeGraph::Connection& link : stock.graph.connections)
                        if (link.toNode == normalOrigin) feeding = link.fromNode;
                    if (feeding.isEmpty()) break;
                    normalOrigin = feeding;
                }
                const NodeGraph::Node* normalSource = stock.graph.node(normalOrigin);
                check(normalSource && normalSource->typeId.startsWith(QStringLiteral("unpack.")),
                    QStringLiteral("%1 takes the shipped normal from the unpack node, whole")
                        .arg(stock.title));
            } else {
                // No unpack node yet: the normal must come straight off the
                // source material, which loads it, never from a reassembly.
                const NodeGraph::Node* normalSource = stock.graph.node(wiredFrom.value(QStringLiteral("normal")));
                check(normalSource && normalSource->typeId.startsWith(QStringLiteral("source.material")),
                    QStringLiteral("%1 feeds the shipped normal straight from the source material").arg(stock.title));
            }

            // FO4 substitutes derived detail AO; FO76 keeps its baked _l.G. Both
            // now wire the AO port — what differs is where it comes from.
            const NodeGraph::Node* aoSource = stock.graph.node(wiredFrom.value(QStringLiteral("ao")));
            check(aoSource && (aoSource->typeId == QStringLiteral("derive.normal")) == stock.derivesAo,
                QStringLiteral("%1 takes AO from %2")
                    .arg(stock.title, stock.derivesAo ? QStringLiteral("the derivation")
                                                      : QStringLiteral("the shipped _l.G")));
        }

        // Emittance off: the source node must hand downstream nothing to build
        // an emissive from — not the colour, and not the glow map either.
        // Suppressing only the colour is not enough, because Solid Color falls
        // back to its own param and would emit the glow at full strength.
        {
            const NodeGraph::NodeTypeSpec* fo76Source = registry.find(QStringLiteral("source.material.fo76"));
            check(fo76Source && fo76Source->output(QStringLiteral("emitColor")),
                QStringLiteral("the source node still declares an emittance colour port"));
            // The port has to stay declared even when it emits nothing, or a
            // saved graph wired to it would fail to load rather than go dark.
            check(fo76Source->output(QStringLiteral("glow")),
                QStringLiteral("the source node still declares a glow port"));
        }

        // Each title's source node refuses the other title's material rather
        // than handing downstream nodes textures that mean something else.
        {
            const NodeGraph::NodeTypeSpec* fo4Source = registry.find(QStringLiteral("source.material.fo4"));
            const NodeGraph::NodeTypeSpec* fo76Source = registry.find(QStringLiteral("source.material.fo76"));
            check(fo4Source && fo76Source, QStringLiteral("both profile source nodes are registered"));
            check(fo4Source->name == QStringLiteral("Input Material (Fallout 4)")
                    && fo76Source->name == QStringLiteral("Input Material (Fallout 76)"),
                QStringLiteral("the profile source nodes are named per title"));
            // FO4 has no reflectance/lighting ports; FO76 has no smooth/spec.
            check(!fo4Source->output(QStringLiteral("reflectance"))
                    && !fo4Source->output(QStringLiteral("lighting"))
                    && fo4Source->output(QStringLiteral("smoothSpec")),
                QStringLiteral("the FO4 source exposes only Fallout 4 slots"));
            check(fo76Source->output(QStringLiteral("reflectance"))
                    && fo76Source->output(QStringLiteral("lighting"))
                    && !fo76Source->output(QStringLiteral("smoothSpec")),
                QStringLiteral("the FO76 source exposes only Fallout 76 slots"));
        }

        // Combine Maps (PBR) is the only place channels get packed, and each named
        // input has to land in its own slot and nowhere else.
        {
            const NodeGraph::NodeTypeSpec* terminal = registry.find(QStringLiteral("output.pbrMaterial"));
            const NodeGraph::NodeTypeSpec* combiner = registry.find(QStringLiteral("composite.pbrChannels"));
            check(terminal && combiner,
                QStringLiteral("the Output Material (PBR) and Combine Maps (PBR) nodes are registered"));
            // The terminal must not offer a second way in for any channel.
            check(!terminal->input(QStringLiteral("roughness")) && !terminal->input(QStringLiteral("metallic"))
                    && !terminal->input(QStringLiteral("ao")) && !terminal->input(QStringLiteral("specular"))
                    && !terminal->input(QStringLiteral("curvature")) && !terminal->input(QStringLiteral("height"))
                    && !terminal->input(QStringLiteral("opacity")),
                QStringLiteral("the terminal takes finished textures only"));

            QImage sourceNormal(4, 4, QImage::Format_RGBA8888);
            sourceNormal.fill(QColor(128, 128, 255, 255));
            const auto flat = [](int value) {
                QImage image(4, 4, QImage::Format_RGBA8888);
                image.fill(QColor(value, value, value, 255));
                return image;
            };

            NodeGraph::EvalArgs args;
            args.inputs.insert(QStringLiteral("baseColor"), NodeGraph::Value::fromImage(flat(10)));
            args.inputs.insert(QStringLiteral("roughness"), NodeGraph::Value::fromImage(flat(40)));
            args.inputs.insert(QStringLiteral("metallic"), NodeGraph::Value::fromImage(flat(50)));
            args.inputs.insert(QStringLiteral("ao"), NodeGraph::Value::fromImage(flat(200)));
            args.inputs.insert(QStringLiteral("specular"), NodeGraph::Value::fromImage(flat(70)));
            args.inputs.insert(QStringLiteral("normal"), NodeGraph::Value::fromImage(sourceNormal));
            args.inputs.insert(QStringLiteral("curvature"), NodeGraph::Value::fromImage(flat(200)));
            args.params = combiner->defaultParams();
            NodeGraph::EvalOutput assembled;
            combiner->evaluate(args, &assembled);

            const QColor rmaos = assembled.outputs.value(QStringLiteral("rmaos")).image().pixelColor(0, 0);
            check(rmaos.red() == 40 && rmaos.green() == 50 && rmaos.blue() == 200 && rmaos.alpha() == 70,
                QStringLiteral("each named map lands in its own RMAOS slot: %1,%2,%3,%4")
                    .arg(rmaos.red()).arg(rmaos.green()).arg(rmaos.blue()).arg(rmaos.alpha()));
            const QColor shipped = assembled.outputs.value(QStringLiteral("normal")).image().pixelColor(0, 0);
            check(shipped.red() == 128 && shipped.green() == 128 && shipped.blue() == 255 && shipped.alpha() == 200,
                QStringLiteral("curvature lands in Normal.A and leaves RGB alone: %1,%2,%3,%4")
                    .arg(shipped.red()).arg(shipped.green()).arg(shipped.blue()).arg(shipped.alpha()));
        }

        // Auto levels. The property that matters is CONSISTENCY: two maps that
        // look alike must get the same clamps, or the same physical surface
        // converts differently in two materials and the whole inference stops
        // meaning anything. So it classifies and snaps rather than fitting.
        {
            const NodeGraph::NodeTypeSpec* levels = registry.find(QStringLiteral("image.levels"));
            const auto autoLevels = [&](int low, int high) {
                // A two-population map: a floor, and a brighter tail above it.
                QImage image(64, 64, QImage::Format_RGBA8888);
                image.fill(QColor(low, low, low, 255));
                for (int y = 0; y < 12; ++y)
                    for (int x = 0; x < 64; ++x) image.setPixelColor(x, y, QColor(high, high, high, 255));
                NodeGraph::EvalArgs args;
                args.inputs.insert(QStringLiteral("image"), NodeGraph::Value::fromImage(image));
                args.params = levels->defaultParams();
                args.params.insert(QStringLiteral("autoLevels"), true);
                NodeGraph::EvalOutput out;
                levels->evaluate(args, &out);
                return out;
            };

            const NodeGraph::EvalOutput fitted = autoLevels(56, 180);
            check(fitted.resolvedLevels.size() == 2,
                QStringLiteral("auto levels publishes what it resolved, so the handles can draw it"));
            // Snapped to the 0.05 ladder, not to the raw percentiles.
            const auto onLadder = [](float value) {
                return std::abs(value / 0.05f - std::round(value / 0.05f)) < 1e-4f;
            };
            check(fitted.resolvedLevels.size() == 2 && onLadder(fitted.resolvedLevels.at(0))
                    && onLadder(fitted.resolvedLevels.at(1)),
                QStringLiteral("and lands both points on the ladder: %1 / %2")
                    .arg(fitted.resolvedLevels.value(0)).arg(fitted.resolvedLevels.value(1)));

            // The consistency guarantee: a map whose distribution differs only
            // slightly — one texel brighter, a hair more tail — must not get a
            // different window, or a 512 and a 1024 copy of one surface diverge.
            const NodeGraph::EvalOutput nudged = autoLevels(57, 178);
            check(fitted.resolvedLevels == nudged.resolvedLevels,
                QStringLiteral("two near-identical maps snap to the SAME clamps: %1/%2 vs %3/%4")
                    .arg(fitted.resolvedLevels.value(0)).arg(fitted.resolvedLevels.value(1))
                    .arg(nudged.resolvedLevels.value(0)).arg(nudged.resolvedLevels.value(1)));

            // A flat map has no metal population to separate, so every one of
            // them gets the same fixed answer rather than a fitted window.
            QImage flat(32, 32, QImage::Format_RGBA8888);
            flat.fill(QColor(56, 56, 56, 255));
            NodeGraph::EvalArgs flatArgs;
            flatArgs.inputs.insert(QStringLiteral("image"), NodeGraph::Value::fromImage(flat));
            flatArgs.params = levels->defaultParams();
            flatArgs.params.insert(QStringLiteral("autoLevels"), true);
            NodeGraph::EvalOutput flatOut;
            levels->evaluate(flatArgs, &flatOut);
            check(flatOut.resolvedLevels.size() == 2
                    && qFuzzyCompare(flatOut.resolvedLevels.at(0), 0.25f)
                    && qFuzzyCompare(flatOut.resolvedLevels.at(1), 0.70f),
                QStringLiteral("a flat map falls back to the stock 0.25/0.70, identically every time"));
            check(flatOut.status.contains(QStringLiteral("no metal")),
                QStringLiteral("and says why: \"%1\"").arg(flatOut.status));

            // A node wired inBlack ABOVE inWhite is inverted deliberately — the
            // diffuse gate means "darker is more metal". Autodetect reasons in
            // ascending terms, and handing that gate an ascending pair reverses
            // its meaning silently: bright albedo starts counting as metal and
            // the mask collapses. Caught in the wild — steel converted at 0%
            // metal the moment auto levels was switched on by default.
            QImage twoTone(64, 64, QImage::Format_RGBA8888);
            twoTone.fill(QColor(40, 40, 40, 255));
            for (int y = 0; y < 20; ++y)
                for (int x = 0; x < 64; ++x) twoTone.setPixelColor(x, y, QColor(160, 160, 160, 255));
            NodeGraph::EvalArgs invArgs;
            invArgs.inputs.insert(QStringLiteral("image"), NodeGraph::Value::fromImage(twoTone));
            invArgs.params = levels->defaultParams();
            invArgs.params.insert(QStringLiteral("inBlack"), 0.20); // above inWhite: inverted
            invArgs.params.insert(QStringLiteral("inWhite"), 0.11);
            NodeGraph::EvalOutput invOut;
            levels->evaluate(invArgs, &invOut);
            check(invOut.resolvedLevels.size() == 2
                    && invOut.resolvedLevels.at(0) > invOut.resolvedLevels.at(1),
                QStringLiteral("auto levels keeps an inverted node inverted: %1 / %2")
                    .arg(invOut.resolvedLevels.value(0)).arg(invOut.resolvedLevels.value(1)));
            // And an ascending node stays ascending.
            check(fitted.resolvedLevels.at(0) < fitted.resolvedLevels.at(1),
                QStringLiteral("and an ascending one ascending"));

            // On by default. A single stored pair cannot fit every map — one
            // graph is shared by every material of a profile — so the node
            // deciding per material IS the intended behaviour, and the stored
            // 0.25/0.70 becomes the fallback for the flat-dielectric case.
            check(levels->param(QStringLiteral("autoLevels"))
                    && levels->param(QStringLiteral("autoLevels"))->defaultValue.toBool(),
                QStringLiteral("auto levels ships on"));
        }

        // Upscaling an under-sized map. The engine has to hold two properties
        // whatever backend runs: a resample that does not shift the image, and
        // a normal that comes back a unit vector — a model trained on pictures
        // will happily return a blue channel that is not sqrt(1-x²-y²).
        {
            QImage flatNormal(8, 8, QImage::Format_RGBA8888);
            flatNormal.fill(QColor(128, 128, 255, 255));
            const QImage zeroed = Upscaler::zeroBlue(flatNormal);
            check(zeroed.pixelColor(3, 3).blue() == 0 && zeroed.pixelColor(3, 3).red() == 128,
                QStringLiteral("a normal goes into the model as RG0, leaving RG alone"));
            const QImage rebuilt = Upscaler::reconstructBlue(zeroed);
            check(std::abs(rebuilt.pixelColor(3, 3).blue() - 255) <= 1,
                QStringLiteral("and comes back a unit vector: flat normal Z = %1")
                    .arg(rebuilt.pixelColor(3, 3).blue()));
            // A tilted normal has to rebuild to the RIGHT Z, not just a high
            // one — this is the check that catches a sign or scale slip.
            QImage tilted(4, 4, QImage::Format_RGBA8888);
            tilted.fill(QColor(200, 128, 0, 255)); // x = +0.568, y = 0
            const int expected = int(std::lround((std::sqrt(std::max(0.0,
                1.0 - std::pow(200 / 127.5 - 1.0, 2.0))) + 1.0) * 127.5));
            check(std::abs(Upscaler::reconstructBlue(tilted).pixelColor(1, 1).blue() - expected) <= 1,
                QStringLiteral("a tilted normal rebuilds to its own Z, expected %1").arg(expected));

            // An over-long XY, which is what the normal-trained models warn
            // they can return ("do not rely on this network producing unit
            // vectors"). Clamping Z to zero and leaving XY alone would keep a
            // non-unit normal and ship a shading error, so XY is rescaled onto
            // the unit circle instead.
            QImage overshoot(4, 4, QImage::Format_RGBA8888);
            overshoot.fill(QColor(243, 243, 0, 255)); // x = y = +0.906, |xy| = 1.28
            const QColor fixed = Upscaler::reconstructBlue(overshoot).pixelColor(1, 1);
            const double fx = fixed.red() / 127.5 - 1.0;
            const double fy = fixed.green() / 127.5 - 1.0;
            const double fz = fixed.blue() / 127.5 - 1.0;
            const double length = std::sqrt(fx * fx + fy * fy + fz * fz);
            check(std::abs(length - 1.0) < 0.02,
                QStringLiteral("an over-long normal is renormalised, not just clamped (|n| = %1)")
                    .arg(length, 0, 'f', 3));
            // Rescaling must not swing the direction: equal X and Y in, equal
            // X and Y out.
            check(std::abs(fx - fy) < 0.02 && fx > 0.6,
                QStringLiteral("and keeps its direction while doing so (x %1, y %2)")
                    .arg(fx, 0, 'f', 3).arg(fy, 0, 'f', 3));

            // Lanczos on flat input must stay flat: ringing shows up here first,
            // and a filter that shifts would move the whole texture half a texel.
            QImage flat(16, 16, QImage::Format_RGBA8888);
            flat.fill(QColor(90, 140, 200, 255));
            const QImage enlarged = Upscaler::lanczos(flat, QSize(32, 32));
            check(enlarged.size() == QSize(32, 32)
                    && enlarged.pixelColor(16, 16) == QColor(90, 140, 200, 255)
                    && enlarged.pixelColor(0, 0) == QColor(90, 140, 200, 255),
                QStringLiteral("Lanczos enlarges flat colour without ringing or shift"));

            // With no upscaler configured the request still has to return a
            // usable image at the requested size — the node must never have to
            // care whether a model was available.
            Upscaler::Request request;
            request.image = flat;
            request.kind = Upscaler::MapKind::Data;
            request.factor = 2;
            const Upscaler::Result produced = Upscaler::upscale(request);
            check(produced.image.size() == QSize(32, 32) && !produced.engine.isEmpty(),
                QStringLiteral("an upscale always returns the asked-for size, engine \"%1\"")
                    .arg(produced.engine));
            check(Upscaler::defaultModel(Upscaler::MapKind::Data).isEmpty(),
                QStringLiteral("and a data map never gets a neural model"));
        }

        // The Upscale node reads the conversion-wide policy when left on
        // "Project setting", and expresses its target against the material's
        // own highest map so one rule covers a batch of mixed resolutions.
        {
            const NodeGraph::NodeTypeSpec* upscale = registry.find(QStringLiteral("image.upscale"));
            check(upscale && upscale->param(QStringLiteral("mode"))
                    && upscale->param(QStringLiteral("mode"))->enumLabels.size() == 6,
                QStringLiteral("Upscale offers project/off/match/2x/4x/fixed"));
            // A real 512 map, not a small stand-in claiming to be one: the node
            // measures the image it was handed, because the resolution reported
            // down a wire is the material's largest map, not this one's.
            QImage small(512, 512, QImage::Format_RGBA8888);
            small.fill(QColor(120, 120, 120, 255));
            const auto run = [&](int policy, int mode, int materialMax) {
                NodeGraph::EvalArgs args;
                args.inputs.insert(QStringLiteral("image"), NodeGraph::Value::fromImage(small));
                args.inputNative.insert(QStringLiteral("image"), 512);
                args.params = upscale->defaultParams();
                args.params.insert(QStringLiteral("mode"), mode);
                args.context.previewMode = false;
                args.context.materialMaxResolution = materialMax;
                args.context.upscalePolicy = policy;
                NodeGraph::EvalOutput out;
                upscale->evaluate(args, &out);
                return out;
            };
            // 512 map in a 1024 material: off does nothing, match doubles it,
            // and 2x-highest goes to 2048.
            check(run(0, 0, 1024).nativeResolution == 512,
                QStringLiteral("policy off leaves an under-sized map alone"));
            check(run(1, 0, 1024).nativeResolution == 1024,
                QStringLiteral("policy match raises 512 to the material's 1024"));
            check(run(2, 0, 1024).nativeResolution == 2048,
                QStringLiteral("policy 2x targets twice the highest map"));
            check(run(3, 0, 1024).nativeResolution == 4096,
                QStringLiteral("policy 4x targets four times it"));
            // A per-node override beats the policy in both directions.
            check(run(3, 1, 1024).nativeResolution == 512,
                QStringLiteral("a node set to Off ignores a 4x policy"));
            check(run(0, 2, 1024).nativeResolution == 1024,
                QStringLiteral("and a node set to Match ignores an off policy"));
            // Already at the material's best: nothing to do, and it says so
            // rather than resampling for no reason.
            const NodeGraph::EvalOutput level = run(1, 0, 512);
            check(level.nativeResolution == 512 && level.status.contains(QStringLiteral("already")),
                QStringLiteral("a map already at the material max is passed through: \"%1\"")
                    .arg(level.status));
        }

        // Nodes that stand for a whole material rather than one map: the two
        // ends of the graph plus the Separate/Combine pair. Selecting any of
        // them shades a material and offers its maps one at a time, so none of
        // them may draw the R/G/B/A strip that isolates a single image.
        {
            struct MaterialNode { const char* id; const char* side; };
            static const MaterialNode materialNodes[] = {
                {"source.material.fo4", "source"}, {"source.material.fo76", "source"},
                {"unpack.fo4", "source"}, {"unpack.fo76", "source"},
                {"composite.pbrChannels", "output"}, {"output.pbrMaterial", "output"}};
            NodeGraph::Node probe;
            probe.showPreview = true;
            for (const MaterialNode& entry : materialNodes) {
                const NodeGraph::NodeTypeSpec* spec = registry.find(QLatin1String(entry.id));
                if (!spec) { check(false, QStringLiteral("%1 is registered").arg(QLatin1String(entry.id))); continue; }
                check(spec->materialPreview == QLatin1String(entry.side),
                    QStringLiteral("%1 previews the %2 material: \"%3\"")
                        .arg(QLatin1String(entry.id), QLatin1String(entry.side), spec->materialPreview));
                check(!NodeGraph::nodeShowsChannelStrip(*spec, probe),
                    QStringLiteral("%1 draws no R/G/B/A strip").arg(QLatin1String(entry.id)));
                // The terminal ends the graph and so keeps no outputs at all;
                // what it holds is what arrives, and the preview reads that off
                // its inputs. Every other material node still publishes maps.
                const bool terminal = spec->outputs.isEmpty();
                int maps = 0;
                for (const NodeGraph::PortSpec& port : terminal ? spec->inputs : spec->outputs)
                    if (port.type == NodeGraph::ValueType::Image) ++maps;
                check(maps > 1, QStringLiteral("%1 offers %2 maps to preview separately, from its %3")
                    .arg(QLatin1String(entry.id)).arg(maps)
                    .arg(terminal ? QStringLiteral("inputs") : QStringLiteral("outputs")));
            }
            // Nothing may hang off the end of the graph.
            const NodeGraph::NodeTypeSpec* sink = registry.find(QStringLiteral("output.pbrMaterial"));
            check(sink && sink->outputs.isEmpty(),
                QStringLiteral("Output Material (PBR) has no outputs — it is where a material stops"));
            // Separating FO76 must reach every map the shader needs, or the
            // picker cannot offer them however the graph is wired downstream.
            const NodeGraph::NodeTypeSpec* unpack76 = registry.find(QStringLiteral("unpack.fo76"));
            check(unpack76 && unpack76->output(QStringLiteral("diffuse")) && unpack76->output(QStringLiteral("normal"))
                    && unpack76->output(QStringLiteral("reflectance")) && unpack76->output(QStringLiteral("smoothness"))
                    && unpack76->output(QStringLiteral("ao")) && unpack76->output(QStringLiteral("opacity"))
                    && unpack76->output(QStringLiteral("subsurface")) && unpack76->output(QStringLiteral("glowMask")),
                QStringLiteral("Separate Maps (Fallout 76) exposes every source map individually"));
        }

        // The packed textures declare what lives in each channel, so the preview
        // can offer roughness on its own. The declaration has to match what the
        // packer actually wrote, or the picker shows the wrong map under the
        // right name — the one failure mode nothing else here would catch.
        {
            const NodeGraph::NodeTypeSpec* combiner = registry.find(QStringLiteral("composite.pbrChannels"));
            const NodeGraph::NodeTypeSpec* terminal = registry.find(QStringLiteral("output.pbrMaterial"));
            const auto channelsOf = [](const NodeGraph::NodeTypeSpec* spec, const char* portId) {
                const NodeGraph::PortSpec* port = spec ? spec->output(QLatin1String(portId)) : nullptr;
                return port ? port->channelMaps : QVector<NodeGraph::ChannelMap>{};
            };
            // The terminal declares the layout on its INPUTS, being where the
            // packed textures arrive rather than where they are produced.
            const auto arrivingAt = [](const NodeGraph::NodeTypeSpec* spec, const char* portId) {
                const NodeGraph::PortSpec* port = spec ? spec->input(QLatin1String(portId)) : nullptr;
                return port ? port->channelMaps : QVector<NodeGraph::ChannelMap>{};
            };
            check(channelsOf(combiner, "rmaos").size() == 4 && channelsOf(combiner, "normal").size() == 3
                    && channelsOf(combiner, "baseColor").size() == 2,
                QStringLiteral("the three packed textures declare the maps inside them"));
            check(channelsOf(combiner, "rmaos") == arrivingAt(terminal, "rmaos")
                    && channelsOf(combiner, "normal") == arrivingAt(terminal, "normal")
                    && channelsOf(combiner, "baseColor") == arrivingAt(terminal, "baseColor"),
                QStringLiteral("what Combine packs and what the terminal receives are the same layout"));

            // Pack four distinguishable values, then pull each declared map back
            // out the way the preview does and check it is the one named.
            const auto flat = [](int value) {
                QImage image(4, 4, QImage::Format_RGBA8888);
                image.fill(QColor(value, value, value, 255));
                return image;
            };
            NodeGraph::EvalArgs args;
            args.inputs.insert(QStringLiteral("baseColor"), NodeGraph::Value::fromImage(flat(10)));
            args.inputs.insert(QStringLiteral("roughness"), NodeGraph::Value::fromImage(flat(41)));
            args.inputs.insert(QStringLiteral("metallic"), NodeGraph::Value::fromImage(flat(52)));
            args.inputs.insert(QStringLiteral("ao"), NodeGraph::Value::fromImage(flat(203)));
            args.inputs.insert(QStringLiteral("specular"), NodeGraph::Value::fromImage(flat(74)));
            args.params = combiner->defaultParams();
            NodeGraph::EvalOutput packed;
            combiner->evaluate(args, &packed);
            const QImage rmaos = packed.outputs.value(QStringLiteral("rmaos")).image();

            struct Expected { const char* label; int value; };
            static const Expected wanted[] = {{"Roughness", 41}, {"Metallic", 52},
                {"Ambient Occlusion", 203}, {"Specular Weight", 74}};
            for (const NodeGraph::ChannelMap& map : channelsOf(combiner, "rmaos")) {
                const QColor texel = NodeGraph::applyChannelMask(rmaos, map.mask).pixelColor(0, 0);
                int expected = -1;
                for (const Expected& candidate : wanted)
                    if (map.label == QLatin1String(candidate.label)) expected = candidate.value;
                check(expected >= 0 && texel.red() == expected && texel.red() == texel.green()
                        && texel.green() == texel.blue(),
                    QStringLiteral("isolating \"%1\" gives grey %2, expected %3")
                        .arg(map.label).arg(texel.red()).arg(expected));
            }
            // Opacity is the base colour's alpha, not one of its colour channels.
            const QImage base = packed.outputs.value(QStringLiteral("baseColor")).image();
            for (const NodeGraph::ChannelMap& map : channelsOf(combiner, "baseColor"))
                if (map.label == QStringLiteral("Base Color"))
                    check(NodeGraph::applyChannelMask(base, map.mask).pixelColor(0, 0).alpha() == 255,
                        QStringLiteral("the base colour view drops opacity rather than keying on it"));

            // Specular F0 stays at the dielectric constant when nothing is
            // wired to it. It used to default to full, which would have shipped
            // a mirror finish on every material that left the slot alone. Its
            // own evaluation, since the pack above deliberately supplies one.
            NodeGraph::EvalArgs bare = args;
            bare.inputs.remove(QStringLiteral("specular"));
            NodeGraph::EvalOutput noSpecular;
            combiner->evaluate(bare, &noSpecular);
            const QColor unwired = noSpecular.outputs.value(QStringLiteral("rmaos")).image().pixelColor(0, 0);
            check(unwired.alpha() == 10,
                QStringLiteral("an unwired Specular F0 packs the 0.04 dielectric constant, got %1")
                    .arg(unwired.alpha()));
        }

        // The stock FO76 conversion does not derive specular per texel: _r is a
        // reflectance colour, and pushing its luma through the 0.02-0.08 band
        // varied F0 across surfaces that physically have none to vary.
        {
            const NodeGraph::Graph stock = ConverterNodes::defaultFo76Graph();
            QString packer;
            for (const NodeGraph::Node& node : stock.nodes)
                if (node.typeId == QStringLiteral("composite.pbrChannels")) packer = node.id;
            const bool specularWired = std::any_of(stock.connections.cbegin(), stock.connections.cend(),
                [&packer](const NodeGraph::Connection& link) {
                    return link.toNode == packer && link.toPort == QStringLiteral("specular");
                });
            check(!specularWired,
                QStringLiteral("the stock FO76 graph leaves Specular F0 unwired, holding it constant"));

            // Both metal signals read as Levels, not Smoothstep: Levels is the
            // only node that draws the distribution under its own handles, and
            // these thresholds are exactly the ones that need placing by eye.
            // The second is deliberately inverted — darker diffuse means metal.
            int levelsNodes = 0;
            bool inverted = false, ascending = false;
            for (const NodeGraph::Node& node : stock.nodes) {
                if (node.typeId != QStringLiteral("image.levels")) continue;
                ++levelsNodes;
                const double black = node.params.value(QStringLiteral("inBlack")).toDouble();
                const double white = node.params.value(QStringLiteral("inWhite")).toDouble();
                if (qFuzzyCompare(black, 0.25) && qFuzzyCompare(white, 0.70)) ascending = true;
                if (qFuzzyCompare(black, 0.20) && qFuzzyCompare(white, 0.11)) inverted = true;
            }
            check(levelsNodes == 2 && ascending && inverted,
                QStringLiteral("both metal signals are Levels: reflectance 0.25/0.70, diffuse "
                               "0.20/0.11 mirrored: %1 nodes, ascending=%2 inverted=%3")
                    .arg(levelsNodes).arg(ascending).arg(inverted));

            // The diffuse gate can only ever REMOVE metal, so its window has to
            // sit in the measured gap and lean towards metal: 3.2x darker under
            // metal than under dielectric across 200 materials (median 17.8 vs
            // 64.3 of 255). Full signal at metal's p75, zero by dielectric's
            // p25 — anything tighter starts deleting real metal surfaces.
            check(0.11 * 255 >= 26.4 && 0.20 * 255 <= 53.1,
                QStringLiteral("the diffuse window sits inside the measured gap: full signal by "
                               "metal's p75 of 26.4, zero by dielectric's p25 of 53.1"));

            // The reflectance threshold reads the map in the space it is STORED
            // in. 0.25/0.70 are stored-value numbers taken from surveying the
            // corpus; linearising first would collapse the distribution into the
            // bottom fifth and make both numbers meaningless.
            QString reflectanceLuma;
            for (const NodeGraph::Connection& link : stock.connections) {
                const NodeGraph::Node* target = stock.node(link.toNode);
                if (!target || target->typeId != QStringLiteral("image.levels")) continue;
                const NodeGraph::Node* source = stock.node(link.fromNode);
                if (!source || source->typeId != QStringLiteral("image.luminance")) continue;
                const NodeGraph::Node* levels = target;
                if (qFuzzyCompare(levels->params.value(QStringLiteral("inWhite")).toDouble(), 0.70))
                    reflectanceLuma = source->id;
            }
            const NodeGraph::Node* lumaNode = stock.node(reflectanceLuma);
            check(lumaNode && !lumaNode->params.value(QStringLiteral("linearize")).toBool(),
                QStringLiteral("the reflectance luminance feeding it does not linearise"));
            // Both mask thresholds were measured on stored values, so neither
            // luminance may linearise. One of them doing so silently moves its
            // window by a gamma and the threshold stops meaning anything.
            const bool anyLinearising = std::any_of(stock.nodes.cbegin(), stock.nodes.cend(),
                [](const NodeGraph::Node& node) {
                    return node.typeId == QStringLiteral("image.luminance")
                        && node.params.value(QStringLiteral("linearize")).toBool();
                });
            check(!anyLinearising,
                QStringLiteral("neither mask luminance linearises — both windows are stored-space"));

            // One threshold pair only works on both authoring conventions
            // because BC4 is mapped onto the BC1 scale first. These are the two
            // anchors: a BC4 dielectric has to land on the BC1 dielectric floor
            // and a BC4 metal top on the BC1 metal top, or the pair above stops
            // reaching full metal on 64% of the corpus.
            const auto flat = [](int value) {
                QImage image(4, 4, QImage::Format_RGBA8888);
                image.fill(QColor(value, value, value, 255));
                return image;
            };
            const auto alignedValue = [&flat](int stored) {
                return ConverterNodes::normaliseReflectanceToSrgbScale(flat(stored), false)
                    .pixelColor(0, 0).red();
            };
            const int dielectric = alignedValue(9);   // 0.035 linear, the flat 4x4 constant
            const int metalTop = alignedValue(53);    // 0.208 linear, textured BC4 p99
            check(std::abs(dielectric - 56) <= 1,
                QStringLiteral("a BC4 dielectric lands on the BC1 floor of 56: got %1").arg(dielectric));
            check(std::abs(metalTop - 179) <= 2,
                QStringLiteral("a BC4 metal top lands on the BC1 top of ~179: got %1").arg(metalTop));
            check(dielectric < int(0.25 * 255) && metalTop >= int(0.70 * 255),
                QStringLiteral("so 0.25/0.70 spans BC4 end to end: %1 below black, %2 at/above white")
                    .arg(dielectric).arg(metalTop));
            // An sRGB map is already on the reference scale and must not move.
            const QImage untouched = ConverterNodes::normaliseReflectanceToSrgbScale(flat(56), true);
            check(untouched.pixelColor(0, 0).red() == 56,
                QStringLiteral("a BC1 sRGB map passes through untouched"));

            // The conversion path harvests the terminal's maps, and it reads
            // them the same way the preview does — by following the wires INTO
            // it, since the terminal keeps no outputs. These are two separate
            // readers of one fact: when only the preview was updated, every
            // conversion failed with "no base colour" while the preview looked
            // perfect. Nothing but running the real path catches that.
            {
                const NodeGraph::Graph stockFo76 = ConverterNodes::defaultFo76Graph();
                QString terminal;
                for (const NodeGraph::Node& node : stockFo76.nodes)
                    if (node.typeId == QStringLiteral("output.pbrMaterial")) terminal = node.id;
                QStringList wired;
                for (const NodeGraph::Connection& link : stockFo76.connections)
                    if (link.toNode == terminal) wired.append(link.toPort);
                check(wired.contains(QStringLiteral("baseColor")),
                    QStringLiteral("the terminal is fed a base colour to harvest: %1")
                        .arg(wired.join(QStringLiteral(","))));
                // Everything the writer needs must arrive by a wire, because a
                // wire is now the only way anything reaches it.
                check(wired.contains(QStringLiteral("normal")) && wired.contains(QStringLiteral("rmaos")),
                    QStringLiteral("along with the normal and RMAOS it writes"));
            }
            check(std::none_of(stock.nodes.cbegin(), stock.nodes.cend(),
                    [](const NodeGraph::Node& node) {
                        return node.typeId == QStringLiteral("image.smoothstep");
                    }),
                QStringLiteral("and no Smoothstep is left behind"));
            // Everything else it packs must still arrive, or "hold F0 constant"
            // would be indistinguishable from having dropped a whole branch.
            QSet<QString> packed;
            for (const NodeGraph::Connection& link : stock.connections)
                if (link.toNode == packer) packed.insert(link.toPort);
            check(packed.contains(QStringLiteral("baseColor")) && packed.contains(QStringLiteral("normal"))
                    && packed.contains(QStringLiteral("roughness")) && packed.contains(QStringLiteral("metallic"))
                    && packed.contains(QStringLiteral("ao")) && packed.contains(QStringLiteral("curvature")),
                QStringLiteral("and still packs every other channel: %1")
                    .arg(QStringList(packed.cbegin(), packed.cend()).join(QStringLiteral(","))));
        }

        // An ordinary single-image node keeps its channel strip: the suppression
        // above must key off materialPreview, not off having image outputs.
        {
            const NodeGraph::NodeTypeSpec* levels = registry.find(QStringLiteral("image.levels"));
            NodeGraph::Node probe;
            probe.showPreview = true;
            check(levels && levels->materialPreview.isEmpty()
                    && NodeGraph::nodeShowsChannelStrip(*levels, probe),
                QStringLiteral("a single-map node still shows its R/G/B/A toggles"));
            // Folding the preview away takes the strip with it, as before.
            probe.showPreview = false;
            check(levels && !NodeGraph::nodeShowsChannelStrip(*levels, probe),
                QStringLiteral("hiding a node's preview hides its channel strip too"));
        }

        // Graph files round-trip through disk (what `convert --graph` consumes).
        {
            QTemporaryDir graphDir;
            if (graphDir.isValid()) {
                const QString path = graphDir.filePath(QStringLiteral("stock.pbrgraph"));
                QString saveError, loadError;
                check(ConverterNodes::saveGraphFile(ConverterNodes::defaultFo76Graph(), path, &saveError),
                    QStringLiteral("a graph writes to disk: %1").arg(saveError));
                const NodeGraph::Graph reloaded = ConverterNodes::loadGraphFile(path, &loadError);
                check(reloaded.nodes.size() == ConverterNodes::defaultFo76Graph().nodes.size()
                        && reloaded.connections.size() == ConverterNodes::defaultFo76Graph().connections.size()
                        && reloaded.validate(registry, &graphErrors),
                    QStringLiteral("a graph file reloads and validates: %1").arg(loadError));
            }
        }

        // Preset-level parity. The conversion nodes are retired, so there is no
        // wrapper left to diff against the math -- what can drift now is the
        // stock GRAPH against convertercore, because the thresholds are written
        // in two places. Pin them to each other.
        {
            const NodeGraph::Graph fo4 = ConverterNodes::defaultFo4Graph();
            const Converter::Options defaults;

            const auto carriesConversionNode = [](const NodeGraph::Graph& graph) {
                for (const NodeGraph::Node& node : graph.nodes)
                    if (node.typeId.startsWith(QStringLiteral("convert."))) return true;
                return false;
            };
            check(!carriesConversionNode(fo4) && !carriesConversionNode(ConverterNodes::defaultFo76Graph()),
                QStringLiteral("neither preset carries a whole-conversion node"));

            // The metal stencil's window has to be the one convertFo4 uses.
            // Walk back from Combine Maps > Metallic: the painted correction
            // Mix sits in between now, so follow its A input to the Levels.
            const auto sourceOf = [&fo4](const QString& node, const QString& port) {
                for (const NodeGraph::Connection& link : fo4.connections)
                    if (link.toPort == port && (node.isEmpty() || link.toNode == node))
                        return link.fromNode;
                return QString();
            };
            QString metallicSource = sourceOf(QString(), QStringLiteral("metallic"));
            if (const NodeGraph::Node* mix = metallicSource.isEmpty() ? nullptr : fo4.node(metallicSource);
                mix && mix->typeId == QStringLiteral("image.mix"))
                metallicSource = sourceOf(mix->id, QStringLiteral("a"));
            const NodeGraph::Node* stencil = metallicSource.isEmpty() ? nullptr : fo4.node(metallicSource);
            check(stencil && stencil->typeId == QStringLiteral("image.levels"),
                QStringLiteral("metallic is fed by a Levels node the user can drag"));
            if (stencil) {
                const double black = stencil->params.value(QStringLiteral("inBlack")).toDouble();
                const double white = stencil->params.value(QStringLiteral("inWhite")).toDouble();
                check(std::abs(black - defaults.metalSpecStart) < 1e-4
                        && std::abs(white - defaults.metalSpecFull) < 1e-4,
                    QStringLiteral("and its window matches convertercore (%1/%2 against %3/%4)")
                        .arg(black).arg(white).arg(defaults.metalSpecStart).arg(defaults.metalSpecFull));
                // Auto levels would re-derive the window per material and
                // stretch a brick wall's own spec variation into full metal.
                check(!stencil->params.value(QStringLiteral("autoLevels")).toBool(true),
                    QStringLiteral("auto levels is off on the stencil — _s.R has no anchor to re-derive against"));
            }

            // Same for the F0 ramp: its out-points are the dielectric window.
            QString specularSource;
            for (const NodeGraph::Connection& link : fo4.connections)
                if (link.toPort == QStringLiteral("specular")) specularSource = link.fromNode;
            const NodeGraph::Node* f0 = specularSource.isEmpty() ? nullptr : fo4.node(specularSource);
            // The gloss fallback has to reach the GRAPH, not just convertercore.
            // Vanilla writes a flat _s.G pure white and puts the real value in
            // the .bgsm smoothness, so without this the graph ships a mirror
            // finish on 827 materials — and the workspace only ever converts
            // through the graph, so "convertercore does it" is no protection.
            {
                const NodeGraph::NodeTypeSpec* unpack =
                    ConverterNodes::sharedRegistry().find(QStringLiteral("unpack.fo4"));
                QImage flatWhiteGloss(4, 4, QImage::Format_RGBA8888);
                flatWhiteGloss.fill(QColor(120, 255, 0));  // R spec, G flat white
                NodeGraph::EvalArgs args;
                args.params = unpack ? unpack->defaultParams() : QJsonObject();
                args.inputs.insert(QStringLiteral("diffuse"), NodeGraph::Value::fromImage(flatWhiteGloss));
                args.inputs.insert(QStringLiteral("specular"), NodeGraph::Value::fromImage(flatWhiteGloss));
                args.inputs.insert(QStringLiteral("smoothness"), NodeGraph::Value::fromScalar(0.2f));
                NodeGraph::EvalOutput out;
                if (unpack) unpack->evaluate(args, &out);
                const QImage gloss = out.outputs.value(QStringLiteral("gloss")).image();
                check(!gloss.isNull() && std::abs(QColor(gloss.pixelColor(1, 1)).red() - 51) <= 1,
                    QStringLiteral("a flat _s.G unpacks to the .bgsm smoothness, not white (%1)")
                        .arg(gloss.isNull() ? -1 : QColor(gloss.pixelColor(1, 1)).red()));

                // A textured G ignores the scalar, as measured.
                QImage texturedGloss(4, 4, QImage::Format_RGBA8888);
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 4; ++x)
                        texturedGloss.setPixelColor(x, y, QColor(120, 40 + x * 50, 0));
                args.inputs.insert(QStringLiteral("specular"), NodeGraph::Value::fromImage(texturedGloss));
                NodeGraph::EvalOutput textured;
                if (unpack) unpack->evaluate(args, &textured);
                const QImage keptGloss = textured.outputs.value(QStringLiteral("gloss")).image();
                check(!keptGloss.isNull() && QColor(keptGloss.pixelColor(3, 0)).red() == 190,
                    QStringLiteral("a textured _s.G keeps its own values"));

                // And the preset must actually wire the scalar in.
                bool wired = false;
                for (const NodeGraph::Connection& link : fo4.connections)
                    if (link.toPort == QStringLiteral("smoothness")) wired = true;
                check(wired, QStringLiteral("the FO4 preset wires the smoothness scalar into Separate Maps"));
            }

            // The painted mask has to be WIRED, in both presets. It was an
            // optional port on the retired conversion nodes that no preset ever
            // connected, and the orchestrator only read the sidecar on the
            // non-graph path — so every mask painted in the workspace was
            // silently discarded, because the workspace always uses the graph.
            for (const auto& preset : {std::make_pair(QStringLiteral("FO4"), fo4),
                     std::make_pair(QStringLiteral("FO76"), ConverterNodes::defaultFo76Graph())}) {
                QString paintNode;
                for (const NodeGraph::Node& node : preset.second.nodes)
                    if (node.typeId == QStringLiteral("source.paintMask")) paintNode = node.id;
                bool factorWired = false, valueWired = false;
                for (const NodeGraph::Connection& link : preset.second.connections) {
                    if (link.fromNode != paintNode) continue;
                    if (link.fromPort == QStringLiteral("factor")) factorWired = true;
                    if (link.fromPort == QStringLiteral("value")) valueWired = true;
                }
                check(!paintNode.isEmpty() && factorWired && valueWired,
                    QStringLiteral("the %1 preset wires a Paint Mask into the metal path").arg(preset.first));
            }

            // v6: the ramp is an identity clamp; the level lives in the output
            // node's specular IOR, whose F0 is the corpus anchor 0.04 / 0.212.
            check(f0 && f0->typeId == QStringLiteral("image.levels")
                    && std::abs(f0->params.value(QStringLiteral("outBlack")).toDouble()) < 1e-6
                    && std::abs(f0->params.value(QStringLiteral("outWhite")).toDouble() - 1.0) < 1e-6,
                QStringLiteral("the specular weight ramp is an identity clamp (v6: the level is the output IOR)"));
            Q_UNUSED(defaults);
        }

        // Levels: the histogram it publishes is what the node body plots, and
        // the channel setting decides how much of the texel the remap touches.
        {
            const NodeGraph::NodeTypeSpec* levelsSpec = registry.find(QStringLiteral("image.levels"));
            check(levelsSpec && levelsSpec->histogram.valid()
                && levelsSpec->histogram.blackPoint == QStringLiteral("inBlack")
                && levelsSpec->histogram.whitePoint == QStringLiteral("inWhite"),
                QStringLiteral("Levels declares a histogram strip bound to its own params"));

            QImage source(4, 4, QImage::Format_RGBA8888);
            source.fill(QColor(64, 128, 192, 255));
            NodeGraph::EvalArgs args;
            args.inputs.insert(QStringLiteral("image"), NodeGraph::Value::fromImage(source));
            args.params = levelsSpec->defaultParams();

            NodeGraph::EvalOutput plotted;
            levelsSpec->evaluate(args, &plotted);
            const int expected = NodeGraph::kHistogramChannels * NodeGraph::kHistogramBuckets;
            const bool sized = plotted.histogram.size() == expected;
            // A flat 4x4 image puts all 16 texels in one bucket per channel.
            check(sized && plotted.histogram[0 * NodeGraph::kHistogramBuckets + 64] == 16u
                && plotted.histogram[1 * NodeGraph::kHistogramBuckets + 128] == 16u
                && plotted.histogram[2 * NodeGraph::kHistogramBuckets + 192] == 16u
                && plotted.histogram[3 * NodeGraph::kHistogramBuckets + 255] == 16u,
                QStringLiteral("Levels publishes a per-channel histogram of its input"));

            // Luminance (the default) drives R, G and B together and leaves
            // alpha alone; a single channel scopes the edit to that channel.
            // Auto levels off: these check the REMAP against a stated window,
            // and autodetection would pick its own and answer a different
            // question. It ships on, so saying so explicitly is required.
            args.params.insert(QStringLiteral("autoLevels"), false);
            args.params.insert(QStringLiteral("inBlack"), 0.0);
            args.params.insert(QStringLiteral("inWhite"), 0.5);
            NodeGraph::EvalOutput lumaOut;
            levelsSpec->evaluate(args, &lumaOut);
            const QColor lumaTexel = lumaOut.outputs.value(QStringLiteral("image")).image().pixelColor(1, 1);
            check(lumaTexel.red() == 128 && lumaTexel.green() == 255 && lumaTexel.blue() == 255
                && lumaTexel.alpha() == 255,
                QStringLiteral("Levels on Luminance remaps RGB together and leaves alpha alone"));

            args.params.insert(QStringLiteral("channel"), 0); // Red
            NodeGraph::EvalOutput redOut;
            levelsSpec->evaluate(args, &redOut);
            const QColor redTexel = redOut.outputs.value(QStringLiteral("image")).image().pixelColor(1, 1);
            check(redTexel.red() == 128 && redTexel.green() == 128 && redTexel.blue() == 192,
                QStringLiteral("Levels scoped to one channel leaves the others untouched"));
        }

        // The filter and mask nodes, each checked on the property that is the
        // reason it exists rather than on an exact texel.
        {
            const auto run = [&](const char* id, const QImage& in, const QJsonObject& params) {
                const NodeGraph::NodeTypeSpec* s = registry.find(QLatin1String(id));
                NodeGraph::EvalArgs a;
                a.inputs.insert(QStringLiteral("image"), NodeGraph::Value::fromImage(in));
                a.params = s ? s->defaultParams() : QJsonObject();
                for (auto it = params.begin(); it != params.end(); ++it) a.params.insert(it.key(), it.value());
                NodeGraph::EvalOutput out;
                if (s) s->evaluate(a, &out);
                return out.outputs.value(QStringLiteral("image")).image();
            };

            // Histogram Scan: full contrast is a hard threshold at the position.
            QImage ramp(256, 1, QImage::Format_RGBA8888);
            for (int x = 0; x < 256; ++x) ramp.setPixelColor(x, 0, QColor(x, x, x, 255));
            const QImage scanned = run("image.histogramScan", ramp,
                {{QStringLiteral("position"), 0.5}, {QStringLiteral("contrast"), 1.0}});
            check(!scanned.isNull() && scanned.pixelColor(60, 0).red() == 0
                && scanned.pixelColor(200, 0).red() == 255,
                QStringLiteral("Histogram Scan at full contrast thresholds at the position"));

            // Median blur is the one that removes an isolated spike outright,
            // which is why it is the tool for block-compression noise.
            QImage speckled(9, 9, QImage::Format_RGBA8888);
            speckled.fill(QColor(40, 40, 40, 255));
            speckled.setPixelColor(4, 4, QColor(255, 255, 255, 255));
            const QImage medianed = run("image.blur", speckled,
                {{QStringLiteral("kind"), 2}, {QStringLiteral("radius"), 1}});
            const QImage boxed = run("image.blur", speckled,
                {{QStringLiteral("kind"), 0}, {QStringLiteral("radius"), 1}});
            check(medianed.pixelColor(4, 4).red() == 40 && boxed.pixelColor(4, 4).red() > 40,
                QStringLiteral("Median removes an isolated spike where Box only spreads it"));

            // Flip Normal Y touches green and nothing else.
            QImage normal(2, 2, QImage::Format_RGBA8888);
            normal.fill(QColor(128, 200, 255, 255));
            const QColor flipped = run("image.normalFlipY", normal, {}).pixelColor(0, 0);
            check(flipped.red() == 128 && flipped.green() == 55 && flipped.blue() == 255,
                QStringLiteral("Flip Normal Y inverts green alone"));

            // Height to Normal: a flat height field is flat blue, and a slope
            // has to tilt the normal away from it.
            QImage flat(8, 8, QImage::Format_RGBA8888);
            flat.fill(QColor(128, 128, 128, 255));
            const QColor flatNormal = run("derive.heightToNormal", flat, {}).pixelColor(4, 4);
            QImage slope(8, 8, QImage::Format_RGBA8888);
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x) slope.setPixelColor(x, y, QColor(x * 32, x * 32, x * 32, 255));
            const QColor slopeNormal = run("derive.heightToNormal", slope, {}).pixelColor(4, 4);
            check(std::abs(flatNormal.red() - 128) <= 1 && flatNormal.blue() > 250
                && slopeNormal.red() < 120,
                QStringLiteral("Height to Normal is flat on flat height and tilts on a slope"));

            // Resize reports the size it actually produced, since the status is
            // what the under-resolution warning reads.
            const NodeGraph::NodeTypeSpec* resizeSpec = registry.find(QStringLiteral("image.resize"));
            NodeGraph::EvalArgs resizeArgs;
            resizeArgs.inputs.insert(QStringLiteral("image"), NodeGraph::Value::fromImage(flat));
            resizeArgs.params = resizeSpec->defaultParams();
            resizeArgs.params.insert(QStringLiteral("size"), 32);
            NodeGraph::EvalOutput resized;
            resizeSpec->evaluate(resizeArgs, &resized);
            check(resized.outputs.value(QStringLiteral("image")).image().size() == QSize(32, 32)
                && resized.nativeResolution == 32,
                QStringLiteral("Resize reaches the requested size and reports it"));
        }

        // Channel isolation is display-only, and has to mean the same thing in
        // the node thumbnail and in the 3D viewport, so both call this.
        {
            QImage source(2, 2, QImage::Format_RGBA8888);
            source.fill(QColor(10, 120, 240, 64));

            const QColor green = NodeGraph::applyChannelMask(source, NodeGraph::ChannelG).pixelColor(0, 0);
            check(green.red() == 120 && green.green() == 120 && green.blue() == 120 && green.alpha() == 255,
                QStringLiteral("isolating one channel shows it as opaque grayscale"));

            const QColor alpha = NodeGraph::applyChannelMask(source, NodeGraph::ChannelA).pixelColor(0, 0);
            check(alpha.red() == 64 && alpha.alpha() == 255,
                QStringLiteral("isolating alpha shows the alpha values, not transparency"));

            // Several channels: the disabled ones zero out, and alpha goes
            // opaque so switching a channel off never reads as transparent.
            const QColor rb = NodeGraph::applyChannelMask(source,
                NodeGraph::ChannelR | NodeGraph::ChannelB).pixelColor(0, 0);
            check(rb.red() == 10 && rb.green() == 0 && rb.blue() == 240 && rb.alpha() == 255,
                QStringLiteral("a partial mask zeroes the disabled channels and stays opaque"));

            check(NodeGraph::applyChannelMask(source, NodeGraph::kChannelsRgba).pixelColor(0, 0).alpha() == 64,
                QStringLiteral("the full RGBA mask leaves the image untouched"));
        }

        // Curves: the interpolation has to stay monotone. A plain cubic spline
        // overshoots between control points, which on a tone curve invents
        // highlights and crushes shadows nobody asked for.
        {
            const QVector<QPointF> identity = NodeGraph::curvePoints(QStringLiteral("[[0,0],[1,1]]"));
            check(std::abs(NodeGraph::evaluateCurve(identity, 0.25) - 0.25) < 1e-6
                && std::abs(NodeGraph::evaluateCurve(identity, 0.8) - 0.8) < 1e-6,
                QStringLiteral("an identity curve is the identity"));

            // A hard step between two close points is where a naive spline
            // rings; every sample must stay inside the control points' range.
            const QVector<QPointF> step =
                NodeGraph::curvePoints(QStringLiteral("[[0,0],[0.45,0.02],[0.55,0.98],[1,1]]"));
            bool inRange = true, monotone = true;
            double previous = -1.0;
            for (int i = 0; i <= 200; ++i) {
                const double y = NodeGraph::evaluateCurve(step, i / 200.0);
                if (y < -1e-6 || y > 1.0 + 1e-6) inRange = false;
                if (y < previous - 1e-6) monotone = false;
                previous = y;
            }
            check(inRange && monotone,
                QStringLiteral("a steep curve never overshoots or reverses"));

            // Malformed input must fall back to identity, not blank the image.
            const QVector<QPointF> broken = NodeGraph::curvePoints(QStringLiteral("nonsense"));
            check(broken.size() == 2 && std::abs(NodeGraph::evaluateCurve(broken, 0.5) - 0.5) < 1e-6,
                QStringLiteral("a malformed curve param falls back to identity"));

            const QString round = NodeGraph::curveToString(step);
            check(NodeGraph::curvePoints(round).size() == step.size(),
                QStringLiteral("curve points round-trip through their JSON form"));
        }

        // Folding: every state has to change the node's measured height, or the
        // layout would keep reserving space for what is no longer drawn.
        {
            const NodeGraph::NodeTypeSpec* texSpec = registry.find(QStringLiteral("source.texture"));
            const NodeGraph::NodeTypeSpec* packSpec = registry.find(QStringLiteral("image.channelPack"));
            NodeGraph::Graph folded;
            const QString producer = folded.addNode(QStringLiteral("source.texture"), *texSpec, QPointF(0, 0));
            const QString consumer = folded.addNode(QStringLiteral("image.channelPack"), *packSpec, QPointF(300, 0));
            folded.connect(registry, {producer, QStringLiteral("image"), consumer, QStringLiteral("r")});

            NodeGraph::Node* node = folded.node(producer);
            const double full = NodeGraph::nodeSize(*texSpec, *node, folded).height();
            node->showPreview = false;
            const double noPreview = NodeGraph::nodeSize(*texSpec, *node, folded).height();
            node->showPreview = true;
            node->showSettings = false;
            const double noSettings = NodeGraph::nodeSize(*texSpec, *node, folded).height();
            node->showSettings = true;
            node->collapsed = true;
            const double collapsed = NodeGraph::nodeSize(*texSpec, *node, folded).height();
            node->collapsed = false;
            check(noPreview < full && noSettings < full && collapsed < noPreview && collapsed < noSettings,
                QStringLiteral("each fold state shrinks the node (full %1, no preview %2, no settings %3, collapsed %4)")
                    .arg(full).arg(noPreview).arg(noSettings).arg(collapsed));

            // Hiding unused ports must never drop a port that carries a wire.
            NodeGraph::Node* sink = folded.node(consumer);
            sink->hideUnusedPorts = true;
            check(NodeGraph::portVisible(*sink, folded, QStringLiteral("r"), false)
                && !NodeGraph::portVisible(*sink, folded, QStringLiteral("g"), false),
                QStringLiteral("hiding unused ports keeps the connected one and drops the rest"));
            check(NodeGraph::nodeSize(*packSpec, *sink, folded).height()
                    < NodeGraph::nodeSize(*packSpec, *folded.node(producer), folded).height() + 1000,
                QStringLiteral("hiding unused ports is reflected in the node height"));

            // Fold state has to survive a save/load round trip, or reopening a
            // graph would silently unfold everything.
            folded.node(producer)->showPreview = false;
            folded.node(producer)->collapsed = true;
            const NodeGraph::Graph reloaded = NodeGraph::Graph::fromJson(folded.toJson());
            const NodeGraph::Node* reloadedNode = reloaded.node(producer);
            const NodeGraph::Node* reloadedSink = reloaded.node(consumer);
            check(reloadedNode && !reloadedNode->showPreview && reloadedNode->collapsed
                && reloadedNode->showSettings && reloadedSink && reloadedSink->hideUnusedPorts,
                QStringLiteral("fold state round-trips through the graph file"));
        }

        // Evaluator: dependency order, thumbnails, and incremental caching, on
        // a graph of pure generators so the test needs nothing from disk.
        {
            NodeGraph::Graph chain;
            const QString constant = chain.addNode(QStringLiteral("image.constant"),
                *registry.find(QStringLiteral("image.constant")), QPointF(0, 0));
            const QString levels = chain.addNode(QStringLiteral("image.levels"),
                *registry.find(QStringLiteral("image.levels")), QPointF(200, 0));
            const QString inverted = chain.addNode(QStringLiteral("image.invert"), *invertSpec, QPointF(400, 0));
            chain.node(constant)->params.insert(QStringLiteral("size"), 32);
            chain.node(constant)->params.insert(QStringLiteral("r"), 0.25);
            chain.connect(registry, {constant, QStringLiteral("image"), levels, QStringLiteral("image")});
            chain.connect(registry, {levels, QStringLiteral("image"), inverted, QStringLiteral("image")});

            NodeGraph::Evaluator evaluator(registry);
            NodeGraph::EvalContext context;
            const QHash<QString, NodeGraph::NodeResult> first = evaluator.evaluate(chain, context);
            check(first[constant].error.isEmpty() && first[inverted].error.isEmpty()
                    && first[inverted].outputs.value(QStringLiteral("image")).image().size() == QSize(32, 32),
                QStringLiteral("the evaluator runs a chain in dependency order"));
            check(!first[constant].thumbnails.value(QStringLiteral("image")).isNull()
                    && !first[inverted].thumbnails.value(QStringLiteral("image")).isNull(),
                QStringLiteral("every image output carries a thumbnail"));
            check(first[inverted].outputs.value(QStringLiteral("image")).image().pixelColor(4, 4).red()
                    == 255 - first[levels].outputs.value(QStringLiteral("image")).image().pixelColor(4, 4).red(),
                QStringLiteral("values actually flow down the chain"));

            const QHash<QString, NodeGraph::NodeResult> second = evaluator.evaluate(chain, context);
            bool allCached = true;
            for (auto it = second.constBegin(); it != second.constEnd(); ++it) allCached = allCached && it.value().cached;
            check(allCached, QStringLiteral("an unchanged second evaluation is served entirely from cache"));

            chain.node(levels)->params.insert(QStringLiteral("gamma"), 2.0);
            const QHash<QString, NodeGraph::NodeResult> third = evaluator.evaluate(chain, context);
            check(third[constant].cached && !third[levels].cached && !third[inverted].cached,
                QStringLiteral("a parameter change re-runs that node and its dependents, not its inputs"));
        }
    }

    // ---- Standard upscale models, one per map role -------------------------
    {
        // Not named "slots": that is a Qt keyword macro and expands to nothing.
        const QVector<Upscaler::MapSlot> roles = Upscaler::allSlots();
        check(roles.size() == 9, QStringLiteral("every map role is offered (%1)").arg(roles.size()));
        QSet<QString> keys, labels;
        for (const Upscaler::MapSlot slot : roles) {
            keys.insert(Upscaler::slotKey(slot));
            labels.insert(Upscaler::slotLabel(slot));
        }
        check(keys.size() == roles.size() && labels.size() == roles.size(),
            QStringLiteral("roles have distinct settings keys and labels"));

        // Handling class per role. Reflectance rides with the pictures because
        // it carries a metal's hue; it earns its own ROW, not its own kind.
        check(Upscaler::kindForSlot(Upscaler::MapSlot::Normal) == Upscaler::MapKind::Normal
                && Upscaler::kindForSlot(Upscaler::MapSlot::BaseColor) == Upscaler::MapKind::Color
                && Upscaler::kindForSlot(Upscaler::MapSlot::Glow) == Upscaler::MapKind::Color
                && Upscaler::kindForSlot(Upscaler::MapSlot::Reflectance) == Upscaler::MapKind::Color
                && Upscaler::kindForSlot(Upscaler::MapSlot::Smoothness) == Upscaler::MapKind::Data
                && Upscaler::kindForSlot(Upscaler::MapSlot::AmbientOcclusion) == Upscaler::MapKind::Data,
            QStringLiteral("each role maps to the right handling class"));

        // Auto reads the port label, so the labels Separate Maps actually emits
        // have to land on the right role — this is the whole mechanism.
        check(Upscaler::slotForLabel(QStringLiteral("Diffuse")) == Upscaler::MapSlot::BaseColor
                && Upscaler::slotForLabel(QStringLiteral("Reflectance")) == Upscaler::MapSlot::Reflectance
                && Upscaler::slotForLabel(QStringLiteral("Normal")) == Upscaler::MapSlot::Normal
                && Upscaler::slotForLabel(QStringLiteral("Smoothness")) == Upscaler::MapSlot::Smoothness
                && Upscaler::slotForLabel(QStringLiteral("Ambient Occlusion")) == Upscaler::MapSlot::AmbientOcclusion
                && Upscaler::slotForLabel(QStringLiteral("Subsurface")) == Upscaler::MapSlot::Subsurface
                && Upscaler::slotForLabel(QStringLiteral("Glow Mask")) == Upscaler::MapSlot::Glow,
            QStringLiteral("Separate Maps port labels resolve to their roles"));
        // Ordering matters: "Reflectance" contains no colour word but a naive
        // pass that checked colour first would still have to not claim it.
        check(Upscaler::slotForLabel(QStringLiteral("Specular / Gloss")) == Upscaler::MapSlot::SpecularGloss
                && Upscaler::slotForLabel(QStringLiteral("Curvature")) == Upscaler::MapSlot::Other,
            QStringLiteral("and an unrecognised label falls to Other rather than guessing"));

        // The preference match, against a real install's model list. This is
        // pure name-matching and it got it wrong once in a way that looked
        // right: a "conservative" hint for data maps matched Photo-Conservative
        // — the photographic class the list existed to avoid — so every data
        // role silently resolved to a photo model.
        const QStringList installedFixture{
            QStringLiteral("Anime-HQ-W4xEX"), QStringLiteral("AnimeVideo-MiniV1.8-W2xEX"),
            QStringLiteral("Omni-MiniV2-W2xEX"), QStringLiteral("Photo-Conservative-x4"),
            QStringLiteral("Photo-HQ-W4xEX"), QStringLiteral("Photo-Small-W2xEX"),
            QStringLiteral("Universal-FastV2-W2xEX"), QStringLiteral("realesr-general-wdn-x4v3"),
            QStringLiteral("realesr-general-x4v3"), QStringLiteral("realesrgan-x4plus")};
        for (const Upscaler::MapSlot dataRole : {Upscaler::MapSlot::Reflectance,
                 Upscaler::MapSlot::Smoothness, Upscaler::MapSlot::AmbientOcclusion,
                 Upscaler::MapSlot::Other}) {
            const QString picked = Upscaler::chooseModel(dataRole, installedFixture);
            check(!picked.contains(QStringLiteral("Photo"), Qt::CaseInsensitive)
                    && !picked.contains(QStringLiteral("Anime"), Qt::CaseInsensitive),
                QStringLiteral("%1 does not resolve to a picture model (%2)")
                    .arg(Upscaler::slotLabel(dataRole), picked));
        }
        check(Upscaler::chooseModel(Upscaler::MapSlot::Smoothness, installedFixture)
                == QStringLiteral("realesr-general-x4v3"),
            QStringLiteral("data roles take the plain general model, not its denoising variant"));
        check(Upscaler::chooseModel(Upscaler::MapSlot::BaseColor, installedFixture)
                .contains(QStringLiteral("Photo")),
            QStringLiteral("base colour does take a photographic model"));
        // No normal-trained model in that list, and a photo model would bend
        // the vectors in ways reconstructBlue cannot undo.
        check(Upscaler::chooseModel(Upscaler::MapSlot::Normal, installedFixture).isEmpty(),
            QStringLiteral("normal declines outright rather than borrowing a picture model"));
        check(Upscaler::chooseModel(Upscaler::MapSlot::Normal,
                  installedFixture + QStringList{QStringLiteral("4x-Normal-RG0")})
                == QStringLiteral("4x-Normal-RG0"),
            QStringLiteral("and takes a normal-trained one the moment it is installed"));
        // An install of nothing but anime models — entirely possible, since the
        // official ncnn release ships no models and a real install is whatever
        // was assembled by hand. A network built to invent clean line art out
        // of gradients is the worst thing to hand a roughness map, so the data
        // roles decline and resample rather than take it.
        const QStringList animeOnly{QStringLiteral("realesrgan-x4plus-anime"),
            QStringLiteral("realesr-animevideov3-x4"), QStringLiteral("Anime-HQ-W4xEX")};
        check(Upscaler::chooseModel(Upscaler::MapSlot::Smoothness, animeOnly).isEmpty()
                && Upscaler::chooseModel(Upscaler::MapSlot::Reflectance, animeOnly).isEmpty(),
            QStringLiteral("with only anime models installed, data roles fall to Lanczos"));
        check(!Upscaler::chooseModel(Upscaler::MapSlot::BaseColor, animeOnly).isEmpty(),
            QStringLiteral("while base colour still takes one — it is at least a picture"));
        // The real install, where the plain model and its anime sibling differ
        // by a suffix. A hint is a substring, so "realesrgan-x4plus" matches
        // BOTH — and the anime file sorts FIRST, because the listing compares
        // full filenames and '-' precedes '.'. Taking the first hit handed base
        // colour the anime model with the plain one sitting right beside it.
        const QStringList officialSet{QStringLiteral("realesr-animevideov3-x2"),
            QStringLiteral("realesr-animevideov3-x3"), QStringLiteral("realesr-animevideov3-x4"),
            QStringLiteral("realesr-general-wdn-x4v3"), QStringLiteral("realesr-general-x4v3"),
            QStringLiteral("realesrgan-x4plus-anime"), QStringLiteral("realesrgan-x4plus")};
        check(Upscaler::chooseModel(Upscaler::MapSlot::BaseColor, officialSet)
                == QStringLiteral("realesrgan-x4plus"),
            QStringLiteral("base colour takes the plain model, not its anime sibling (%1)")
                .arg(Upscaler::chooseModel(Upscaler::MapSlot::BaseColor, officialSet)));
        check(Upscaler::chooseModel(Upscaler::MapSlot::Smoothness, officialSet)
                == QStringLiteral("realesr-general-x4v3"),
            QStringLiteral("and data roles the plain general one (%1)")
                .arg(Upscaler::chooseModel(Upscaler::MapSlot::Smoothness, officialSet)));

        check(Upscaler::modelNativeScale(QStringLiteral("realesrgan-x4plus")) == 4
                && Upscaler::modelNativeScale(QStringLiteral("Photo-Small-W2xEX")) == 2
                && Upscaler::modelNativeScale(QStringLiteral("some-model")) == 0,
            QStringLiteral("a model's native scale is read from its name"));

        // Pinning round trip. The test binary has its own settings scope, so
        // this cannot reach the real configuration.
        const QString pinned = QStringLiteral("test-model-x4");
        Upscaler::setModelForSlot(Upscaler::MapSlot::Smoothness, pinned);
        check(Upscaler::slotModelIsPinned(Upscaler::MapSlot::Smoothness),
            QStringLiteral("choosing a model pins it"));
        // Not installed here, so it must fall back rather than run a network
        // that is not present — a choice made against another install.
        check(Upscaler::modelForSlot(Upscaler::MapSlot::Smoothness)
                == Upscaler::suggestedModelForSlot(Upscaler::MapSlot::Smoothness),
            QStringLiteral("a pinned model missing from this install falls back to the suggestion"));
        Upscaler::setModelForSlot(Upscaler::MapSlot::Smoothness, QString());
        check(Upscaler::modelForSlot(Upscaler::MapSlot::Smoothness).isEmpty(),
            QStringLiteral("pinning Lanczos explicitly is honoured, not read as unset"));
        Upscaler::clearSlotModel(Upscaler::MapSlot::Smoothness);
        check(!Upscaler::slotModelIsPinned(Upscaler::MapSlot::Smoothness),
            QStringLiteral("and clearing it goes back to suggested"));

        // The node's role list and the dialog's are the same list, or a graph
        // and the settings would disagree about what index means what.
        const NodeGraph::NodeTypeSpec* upscale =
            ConverterNodes::sharedRegistry().find(QStringLiteral("image.upscale"));
        const NodeGraph::ParamSpec* role = upscale ? upscale->param(QStringLiteral("mapSlot")) : nullptr;
        check(role && role->enumLabels.size() == roles.size() + 1,
            QStringLiteral("the Upscale node offers Auto plus every role (%1)")
                .arg(role ? role->enumLabels.size() : -1));
        bool listsMatch = role != nullptr;
        for (int index = 0; role && index < roles.size(); ++index)
            if (role->enumLabels.at(index + 1) != Upscaler::slotLabel(roles.at(index))) listsMatch = false;
        check(listsMatch, QStringLiteral("and in the same order the settings table uses"));
    }

    {
        // Copying one channel off a node: the value has to land in all three
        // colour channels or it pastes as a red/green/blue tint instead of the
        // greyscale map it is, and alpha has to come back OPAQUE or a copied
        // alpha channel pastes as transparency and shows nothing at all.
        QImage source(2, 2, QImage::Format_RGBA8888);
        source.fill(QColor(10, 90, 170, 40));
        for (int channel = 0; channel < 4; ++channel) {
            const QImage extracted = NodeCanvas::channelAsImage(source, channel);
            const QColor texel = extracted.pixelColor(1, 1);
            const int expected = channel == 0 ? 10 : channel == 1 ? 90 : channel == 2 ? 170 : 40;
            check(texel.red() == expected && texel.green() == expected
                    && texel.blue() == expected && texel.alpha() == 255,
                QStringLiteral("channel %1 copies as opaque greyscale (%2)")
                    .arg(channel).arg(texel.red()));
        }
        check(NodeCanvas::channelAsImage(QImage(), 0).isNull()
            && NodeCanvas::channelAsImage(source, 9).isNull(),
            QStringLiteral("a null image or a bad channel index copies nothing"));

        // Paint Mask splits the 0.5-neutral sidecar into the two things a Mix
        // needs. Both are computed in the node because |m - 0.5| * 2 needs an
        // intermediate outside 0-1 and every image port is 8-bit.
        {
            QTemporaryDir maskDir;
            QImage painted(4, 4, QImage::Format_RGBA8888);
            painted.fill(QColor(128, 128, 128));            // neutral
            painted.setPixelColor(0, 0, QColor(0, 0, 0));   // hard dielectric
            painted.setPixelColor(1, 0, QColor(255, 255, 255)); // hard metal
            painted.setPixelColor(2, 0, QColor(64, 64, 64));    // half-strength dielectric
            const QString maskPath = QDir(maskDir.path()).filePath(QStringLiteral("m_correction.png"));
            painted.save(maskPath);

            const NodeGraph::NodeTypeSpec* paintSpec =
                ConverterNodes::sharedRegistry().find(QStringLiteral("source.paintMask"));
            NodeGraph::EvalArgs args;
            args.params = paintSpec ? paintSpec->defaultParams() : QJsonObject();
            args.context.correctionMaskPath = maskPath;
            NodeGraph::EvalOutput out;
            if (paintSpec) paintSpec->evaluate(args, &out);

            // Each node owns its OWN pixels: same material, two node ids, two
            // masks. This is the whole point of the node holding the image
            // rather than there being one correction per material.
            {
                const QString dir = maskDir.path();
                QImage blackMask(4, 4, QImage::Format_RGBA8888);
                blackMask.fill(QColor(0, 0, 0));
                QImage whiteMask(4, 4, QImage::Format_RGBA8888);
                whiteMask.fill(QColor(255, 255, 255));
                const QString pathA = ConverterNodes::paintMaskPath(dir, QStringLiteral("mat"),
                    QStringLiteral("aaaa1111"), false);
                const QString pathB = ConverterNodes::paintMaskPath(dir, QStringLiteral("mat"),
                    QStringLiteral("bbbb2222"), false);
                check(!pathA.isEmpty() && pathA != pathB,
                    QStringLiteral("two node ids resolve to two different mask files"));
                blackMask.save(pathA);
                whiteMask.save(pathB);

                NodeGraph::EvalArgs perNode;
                perNode.params = args.params;
                perNode.context.maskDirectory = dir;
                perNode.context.maskMaterialKey = QStringLiteral("mat");
                perNode.nodeId = QStringLiteral("aaaa1111");
                NodeGraph::EvalOutput outA;
                if (paintSpec) paintSpec->evaluate(perNode, &outA);
                perNode.nodeId = QStringLiteral("bbbb2222");
                NodeGraph::EvalOutput outB;
                if (paintSpec) paintSpec->evaluate(perNode, &outB);
                const QImage valueA = outA.outputs.value(QStringLiteral("value")).image();
                const QImage valueB = outB.outputs.value(QStringLiteral("value")).image();
                check(!valueA.isNull() && !valueB.isNull()
                        && QColor(valueA.pixelColor(0, 0)).red() == 0
                        && QColor(valueB.pixelColor(0, 0)).red() == 255,
                    QStringLiteral("and each node reads its own, not the other's"));

                // A shared-scope node lifts out of the per-material key, so the
                // same node serves every material the preset is applied to.
                const QString sharedPath = ConverterNodes::paintMaskPath(dir,
                    QStringLiteral("mat"), QStringLiteral("aaaa1111"), true);
                const QString otherMaterial = ConverterNodes::paintMaskPath(dir,
                    QStringLiteral("other"), QStringLiteral("aaaa1111"), true);
                check(sharedPath == otherMaterial && sharedPath != pathA,
                    QStringLiteral("shared scope drops the material from the path"));
                check(ConverterNodes::paintMaskPath(dir, QString(),
                          QStringLiteral("aaaa1111"), false).isEmpty(),
                    QStringLiteral("a per-material mask with no material resolves to nothing"));
            }
            const QImage factor = out.outputs.value(QStringLiteral("factor")).image();
            const QImage value = out.outputs.value(QStringLiteral("value")).image();
            check(!factor.isNull() && !value.isNull()
                    && QColor(factor.pixelColor(3, 3)).red() == 0,
                QStringLiteral("an unpainted texel has zero factor, so a Mix leaves the inference alone"));
            check(QColor(factor.pixelColor(0, 0)).red() == 255
                    && QColor(value.pixelColor(0, 0)).red() == 0,
                QStringLiteral("a black stroke is full-strength and overrides to dielectric"));
            check(QColor(factor.pixelColor(1, 0)).red() == 255
                    && QColor(value.pixelColor(1, 0)).red() == 255,
                QStringLiteral("a white stroke is full-strength and overrides to metal"));
            const int partial = QColor(factor.pixelColor(2, 0)).red();
            check(partial > 100 && partial < 155,
                QStringLiteral("a half-strength stroke blends rather than snapping (%1)").arg(partial));

            // Nothing painted must not fail — it is the normal case.
            NodeGraph::EvalArgs empty;
            empty.params = args.params;
            NodeGraph::EvalOutput none;
            if (paintSpec) paintSpec->evaluate(empty, &none);
            check(none.error.isEmpty()
                    && none.outputs.value(QStringLiteral("factor")).image().isNull(),
                QStringLiteral("no mask painted is not an error, and overrides nothing"));
        }

        // Every node that carries texture data must be able to offer this, so
        // the menu has something to enumerate: image outputs, by port.
        int withImageOutputs = 0;
        for (const NodeGraph::NodeTypeSpec* spec : ConverterNodes::sharedRegistry().all())
            for (const NodeGraph::PortSpec& port : spec->outputs)
                if (port.type == NodeGraph::ValueType::Image) { ++withImageOutputs; break; }
        check(withImageOutputs > 20,
            QStringLiteral("%1 node types expose an image output to copy from").arg(withImageOutputs));
    }

    // ---------------------------------------------------------------------
    // LANE OPENPBR1 (2026-09-03) -- the OpenPBR Surface v1.1.1 alignment's
    // editor half. bungo's ruling: "Okay, so we match what's in the
    // whitepaper".
    //
    // These are SOURCE PINS on the preview ubershader rather than rendered
    // comparisons, and that is a deliberate limit: this suite builds no GL
    // context, so it cannot execute the shader. What it CAN hold is that the
    // two lines the alignment named are present, in the form the rule
    // requires, and that the identity case is written as an identity.
    // ---------------------------------------------------------------------
    {
        const auto repoFile = [](const QString& relative) -> QString {
            QDir dir = QDir::current();
            for (int up = 0; up < 6; ++up) {
                const QString candidate = dir.filePath(relative);
                if (QFileInfo::exists(candidate)) return QDir::cleanPath(candidate);
                if (!dir.cdUp()) break;
            }
            return {};
        };
        const auto readRepoFile = [&](const QString& relative) -> QString {
            const QString path = repoFile(relative);
            if (path.isEmpty()) return {};
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) return {};
            QString text = QString::fromUtf8(file.readAll());
            text.remove(QLatin1Char('\r'));
            return text;
        };

        const QString preview =
            readRepoFile(QStringLiteral("src/materialpreviewwidget.cpp"));
        check(preview.size() > 10000,
            QStringLiteral("OPENPBR1: the preview source was found and read"));

        // ---- A4, superseded by PBRM v6 (docs/PBRM-v6-Specular.md) ----
        //
        // OPENPBR1 A4 pinned the runtime's 0.16 conductor guard on the authored
        // F0 scalar. v6 has no authored F0: RMAOS alpha is a weight that can
        // only LOWER the level the IOR sets, so there is nothing left to clamp
        // and the runtime drops min(f4fxF0Scalar, 0.16) in the same change.
        check(preview.contains(QStringLiteral("vec3 dielectricF0=defaults?vec3(.04):vec3(specWeight*iorF0)*specularTint;")),
            QStringLiteral("PBRM v6: dielectric F0 = weight * F0(ior) * tint"));
        check(preview.contains(QStringLiteral("float iorF0=(specIor-1.0)/(specIor+1.0);iorF0*=iorF0;")),
            QStringLiteral("PBRM v6: F0(ior) = ((ior-1)/(ior+1))^2"));
        check(preview.contains(QStringLiteral("if(uIorInAlpha)specIor=clamp(specularColorSample.a,0.0,1.0)*uSpecularIorMax;")),
            QStringLiteral("PBRM v6: the IOR map replaces the scalar over [0, iorMax] (Option A)"));
        check(!preview.contains(QStringLiteral(",0.16)")) && !preview.contains(QStringLiteral("min(dielectric,0.16)")),
            QStringLiteral("PBRM v6: the 0.16 conductor guard is gone from the preview"));
        // The tint and the IOR map are resolved ABOVE the surface-state blend,
        // so states weather the resolved dielectric F0, and the blend stays
        // above the metal mix so metals keep their base-colour F0.
        const int resolveAt = preview.indexOf(QStringLiteral("vec3 dielectricF0=defaults?vec3(.04):"));
        const int stateBlendAt = preview.indexOf(QStringLiteral("dielectricF0=mix(dielectricF0,vec3(uStateF0Target[i])"));
        // 2026-09-17: the metal side of the mix is the OpenPBR F82 Fresnel
        // (Specular Color as the 82-degree edge tint), the dielectric side Schlick.
        const int metalMixAt = preview.indexOf(QStringLiteral(",base,metal),F=min(mix(fresnel(VoH,dielectricF0),f82(VoH,base,metalTint),metal)"));
        check(resolveAt >= 0 && stateBlendAt > resolveAt && metalMixAt > stateBlendAt,
            QStringLiteral("PBRM v6: states weather the resolved dielectric F0, above the metal mix"));

        // ---- A5: emission is BELOW the coat ----
        //
        // OpenPBR spec line 1136 puts the emission slab under the coat and
        // lines 1357-1358 give L_e = lerp(1, T_coat, C) * E. The preview's coat
        // multiply ran before the emission add, so coat_color never tinted the
        // glow: a red lacquer over a white lamp previewed with a white glow.
        check(preview.contains(QStringLiteral("vec3 coatEmissionTransmittance=vec3(1.0);")),
            QStringLiteral("OPENPBR1 A5: the coat transmittance is accumulated for emission"));
        check(preview.contains(QStringLiteral("coatEmissionTransmittance*=coatTransmittance;")),
            QStringLiteral("OPENPBR1 A5: every coat layer contributes its transmittance"));
        check(preview.contains(QStringLiteral("color+=emission*coatEmissionTransmittance;")),
            QStringLiteral("OPENPBR1 A5: the emission add is tinted by the coat"));
        check(!preview.contains(QStringLiteral("\n            color+=emission;")),
            QStringLiteral("OPENPBR1 A5: the untinted emission add is gone"));
        // THE IDENTITY. The accumulator is declared OUTSIDE the coat loop and
        // initialised to vec3(1.0), and the loop `continue`s on a material with
        // no coat layer -- so a material without a coat multiplies emission by
        // exactly 1.0 and is bit-identical to the previous build.
        check(preview.indexOf(QStringLiteral("vec3 coatEmissionTransmittance=vec3(1.0);")) <
                  preview.indexOf(QStringLiteral("if(defaults||!hasSpecial(uCoatSpecialLayer[ci]))continue;")),
            QStringLiteral("OPENPBR1 A5: the accumulator is declared above the coat loop (identity when no coat)"));

        // ---- A6: the thin-film unit, and the mapping table ----
        const QString formatDoc = readRepoFile(QStringLiteral("docs/PBRM-v6.md"));
        check(formatDoc.size() > 5000,
            QStringLiteral("OPENPBR1: the PBRM v6 format doc was found and read"));
        check(formatDoc.contains(QStringLiteral("thin-film thickness\nin MICROMETRES"))
                  || formatDoc.contains(QStringLiteral("MICROMETRES")),
            QStringLiteral("OPENPBR1 A6: thin-film thickness is documented as micrometres"));
        check(formatDoc.contains(QStringLiteral("## OpenPBR Surface v1.1.1 mapping")),
            QStringLiteral("OPENPBR1 A6: the doc carries an OpenPBR mapping section"));
        // The table must cover the slots the alignment mapped, with a verdict
        // apiece. Spot-checked on the rows a mistaken importer would trip on.
        for (const char* row : { "base_metalness", "specular_ior", "specular_color",
                 "subsurface_radius", "fuzz_weight", "coat_color",
                 "thin_film_thickness", "emission_luminance", "geometry_opacity" }) {
            check(formatDoc.contains(QString::fromLatin1(row)),
                QStringLiteral("OPENPBR1 A6: the mapping table carries %1").arg(QString::fromLatin1(row)));
        }
        check(formatDoc.contains(QStringLiteral("`F0 = ((η−1)/(η+1))²`")),
            QStringLiteral("OPENPBR1 A6: the IOR->F0 conversion an importer needs is written down"));
    }

    QTextStream(stdout) << "Failures: " << failures << Qt::endl;
    return failures == 0 ? 0 : 1;
}
