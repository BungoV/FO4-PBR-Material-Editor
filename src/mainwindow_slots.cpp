#include "mainwindow.h"

#include "colorpicker.h"
#include "disclosureicon.h"
#include "dragnumericedit.h"
#include "materialpreviewwidget.h"
#include "previewenvironmentsettings.h"
#include "texturearchive.h"
#include "texturebrowserdialog.h"
#include "textureimage.h"
#include "wheelfocusguard.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QClipboard>
#include <QComboBox>
#include <QCompleter>
#include <QCoreApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QJsonDocument>
#include <QMenuBar>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QStatusBar>
#include <QStandardPaths>
#include <QStringListModel>
#include <QToolButton>
#include <QTimer>
#include <QUndoCommand>
#include <QUndoStack>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>
#include "mainwindow_detail.h"

QVector<TextureSlotSpec> MainWindow::textureSlotsFor(Pbr::ShaderMode mode, const QString& uvSet) const
{
    // The tertiary layer offers the same overlay slot family as the secondary
    // layer, so `secondary` here means "overlay layer" and only the prefix and
    // display names differ.
    const bool tertiary = uvSet == QStringLiteral("UV2");
    const bool secondary = uvSet == QStringLiteral("UV1") || tertiary;
    const bool all = mode == Pbr::ShaderMode::AllFeatures;
    QVector<TextureSlotSpec> result;
    const auto add = [&result](TextureSlotSpec spec) {
        for (const auto& existing : result)
            if (existing.key == spec.key) return;
        result.append(std::move(spec));
    };

    const QString prefix = tertiary ? QStringLiteral("tertiary")
        : (secondary ? QStringLiteral("secondary") : QStringLiteral("primary"));
    const QString layerName = tertiary ? tr("Tertiary") : tr("Secondary");
    const QString baseName = secondary ? tr("%1 Base Color / Opacity").arg(layerName) : tr("Base Color / Opacity");
    add(slot(prefix + "BaseColor", baseName, secondary ? (tertiary ? "U2 C" : "U1 C") : "BASE", "sRGB • RGB color / A opacity",
        QColor(170, 126, 88), true, {},
        "Surface color. Alpha supplies opacity or cutout coverage when the selected shader uses it.", {
            toggleField("overrideColor", "RGB • Base color", true, "Checked uses the constant base color; unchecked samples texture RGB."),
            colorField("color", "Base color override", "#FFFFFF", "Constant sRGB base color used when RGB is overridden or unavailable."),
            toggleField("overrideOpacity", "A • Opacity", true, "Checked uses constant opacity; unchecked samples texture alpha."),
            numberField("opacity", "Opacity override", 1.0, 0.0, 1.0, 0.005, "Constant opacity used when alpha is overridden or unavailable."),
            colorField("tint", "Tint", "#FFFFFF", "Multiplies the sampled base color."),
            numberField("scale", "Color scale", 1.0, 0.0, 32.0, 0.01, "Scales the decoded base color."),
            choiceField("blend", "Layer blend", secondary ? "Masked Replace" : "Base", {"Base", "Masked Replace", "Multiply", "Modulate 2x", "Add", "Screen", "Subtract", "Min (Darken)", "Max (Lighten)", "Overlay"}, "Controls how this UV layer combines with the previous material color.")
        }));

    const bool effect = mode == Pbr::ShaderMode::UnlitEffect || mode == Pbr::ShaderMode::SimpleLitEffect || mode == Pbr::ShaderMode::PhysicallyLitEffect;
    if (effect) {
        add(slot(prefix + "ParticleGradient", tr("Particle Gradient"), "GRAD", "sRGB • RGB ramp / A opacity ramp", QColor(222, 112, 69), false, {},
            "Maps particle age or another scalar through a color and opacity gradient. The alpha channel multiplies final coverage.", {
                toggleField("overrideColor", "RGB • Gradient color", true, "Checked replaces the sampled gradient color with a constant."),
                colorField("color", "Gradient color override", "#FFFFFF", "Constant gradient color used when RGB is overridden or unavailable."),
                toggleField("overrideOpacity", "A • Gradient opacity", true, "Checked replaces sampled gradient opacity with a constant."),
                numberField("opacity", "Gradient opacity override", 1.0, 0.0, 1.0, 0.005, "Constant gradient opacity."),
                choiceField("driver", "Driver", "Particle Age", {"Particle Age", "Global Time", "Vertex Alpha", "Constant"}, "Selects the value used to sample the gradient."),
                numberField("constantDriver", "Constant driver value", 0.5, 0.0, 1.0, 0.005, "Gradient coordinate used when Driver is Constant."),
                numberField("intensity", "Intensity", 1.0, 0.0, 32.0, 0.01, "Scales gradient color and alpha.")
            }));
        if (mode != Pbr::ShaderMode::UnlitEffect) {
            add(slot(prefix + "Normal", tr("Effect Normal"), "NRM", "Linear • RG tangent normal / B optional height / A mask", QColor(112, 112, 202), false, {},
                "Normal map used by lit effects.", {
                    toggleField("overrideNormal", "RG • Tangent normal", true, "Checked replaces the sampled tangent normal with a constant."),
                    colorField("normal", "Normal override", "#8080FF", "Constant tangent-space normal; neutral is #8080FF."),
                    toggleField("overrideHeight", "B • Height", true, "Checked replaces packed height with a constant."),
                    numberField("height", "Height override", 0.5, 0.0, 1.0, 0.005, "Constant packed height."),
                    toggleField("overrideMask", "A • Effect mask", true, "Checked replaces the sampled effect mask with a constant."),
                    numberField("mask", "Effect mask override", 1.0, 0.0, 1.0, 0.005, "Constant effect mask."),
                    toggleField("heightInBlue", "B contains height", false, "Checked treats blue as packed height."),
                    numberField("strength", "Normal strength", 1.0, 0.0, 8.0, 0.01, "Scales normal perturbation.")
                }));
            add(slot(prefix + "Emissive", tr("Emissive"), "EMI", "sRGB • RGB emission / A intensity mask", QColor(235, 165, 52), false, {},
                "Adds self-illuminated radiance; alpha can modulate its intensity.", {
                    toggleField("overrideColor", "RGB • Emission", true, "Checked replaces sampled emission RGB with a constant."),
                    colorField("color", "Emission override", "#FFFFFF", "Constant emission color used when RGB is overridden or unavailable."),
                    toggleField("overrideMask", "A • Intensity mask", true, "Checked replaces the sampled intensity mask with a constant."),
                    numberField("mask", "Intensity mask override", 1.0, 0.0, 1.0, 0.005, "Constant emissive intensity mask."),
                    numberField("intensity", "Intensity", 0.0, 0.0, 1000.0, 0.05, "Emission multiplier.")
                }));
        }
        if (mode != Pbr::ShaderMode::UnlitEffect) {
            add(slot(prefix + "SixWayA", tr("Six-Way Lightmap • Right / Top / Front"), "6WAY", "Linear • R right / G top / B front / A opacity", QColor(126, 138, 156), false, {},
                "First half of a six-way lightmap flipbook: per-frame lighting baked from the right, top, and front directions. Scene lights relight the smoke through the six-direction basis; alpha carries opacity.", {
                    toggleField("overrideOpacity", "A • Opacity", false, "Checked replaces the sampled opacity with a constant — use for RGB-only sheets or opaque smoke cards whose alpha should be ignored."),
                    numberField("opacity", "Opacity override", 1.0, 0.0, 1.0, 0.005, "Constant opacity when texture A is overridden."),
                    numberField("ambient", "Ambient response", 1.0, 0.0, 8.0, 0.01, "How strongly the averaged six directions respond to environment light."),
                    numberField("intensity", "Lightmap intensity", 1.0, 0.0, 8.0, 0.01, "Scales the six-way lighting result.")
                }));
            add(slot(prefix + "SixWayB", tr("Six-Way Lightmap • Left / Bottom / Back"), "6WAY", "Linear • R left / G bottom / B back / A fire mask", QColor(126, 138, 156), false, {},
                "Second half of the six-way lightmap: lighting baked from the left, bottom, and back directions. Alpha is an emissive fire mask — burning regions self-illuminate through a blackbody ramp. Both halves share this layer's flipbook layout.", {
                    toggleField("overrideEmissive", "A • Fire mask", true, "Checked replaces the sampled fire mask with a constant."),
                    numberField("emissive", "Fire mask override", 0.0, 0.0, 1.0, 0.005, "Constant fire mask when texture A is overridden or absent."),
                    numberField("fireIntensity", "Fire intensity", 0.0, 0.0, 64.0, 0.05, "Peak emissive energy of the blackbody fire ramp; zero disables the fire pass.")
                }));
            add(slot(prefix + "MotionVectors", tr("Flipbook Motion Vectors"), "MVEC", "Linear • RG per-frame motion", QColor(126, 138, 156), false, {},
                "Per-frame motion vectors sharing the primary layer's flipbook grid. Frame cross-fades advect along this field instead of dissolving, hiding low flipbook framerates. Neutral is #808000.", {
                    numberField("strength", "Motion strength", 1.0, 0.0, 4.0, 0.01, "Scales the decoded motion vectors; zero disables warping.")
                }, {QStringLiteral("R"), QStringLiteral("G")}));
        }
        if (mode == Pbr::ShaderMode::PhysicallyLitEffect) {
            add(slot(prefix + "Rmaos", tr("Roughness + Metallic + Ambient Occlusion + Specular"), "RMAOS", "Linear • R roughness / G metallic / B AO / A F0", QColor(108, 136, 111), false, {},
                "Packed physical surface properties for a physically lit effect.", {
                    toggleField("overrideRoughness", "R • Roughness", true, "Checked uses the constant roughness override."),
                    numberField("roughness", "Roughness override", 0.5, 0.0, 1.0, 0.005, "Constant roughness."),
                    toggleField("overrideMetallic", "G • Metallic", true, "Checked uses the constant metallic override."),
                    numberField("metallic", "Metallic override", 0.0, 0.0, 1.0, 0.005, "Constant metallic."),
                    toggleField("overrideAo", "B • Ambient Occlusion", true, "Checked uses the constant AO override."),
                    numberField("ao", "AO override", 1.0, 0.0, 1.0, 0.005, "Constant ambient occlusion."),
                    toggleField("overrideSpecularWeight", "A • Specular Weight", true, "Checked uses the constant specular weight override."),
                    numberField("specularWeight", "Specular weight override", 1.0, 0.0, 1.0, 0.005, "OpenPBR specular_weight: a 0-1 multiplier on the dielectric reflectance the specular IOR sets. 1 is the full level.")
                }));
        }
        add(slot(prefix + "ScreenDistortion", tr("Screen Distortion"), "DIST", "Linear • RG direction / B strength / A mask", QColor(82, 137, 150), false, {},
            "Offsets the already rendered scene color to create refraction-like distortion.", {
                toggleField("overrideDirection", "RG • Distortion direction", true, "Checked replaces sampled distortion direction with a constant."),
                colorField("direction", "Direction override", "#808000", "Constant encoded RG direction; neutral is #808000."),
                toggleField("overrideStrength", "B • Distortion strength", true, "Checked replaces sampled distortion strength with a constant."),
                numberField("strength", "Distortion strength override", 0.0, 0.0, 1.0, 0.005, "Constant distortion strength."),
                toggleField("overrideMask", "A • Distortion mask", true, "Checked replaces sampled distortion mask with a constant."),
                numberField("mask", "Distortion mask override", 1.0, 0.0, 1.0, 0.005, "Constant distortion mask."),
                numberField("vectorScale", "Vector scale", 1.0, -100.0, 100.0, 0.001, "Scales the decoded RG direction and B magnitude."),
            }));
        return result;
    }

    if (mode != Pbr::ShaderMode::LandscapeSurface || all) {
        add(slot(prefix + "Normal", secondary ? tr("%1 Normal + Height").arg(layerName) : tr("Normal + Height"), "NRM", "Linear • RG normal / B optional height / A curvature or cone map", QColor(112, 112, 202), true,
            QString(),
            "Tangent-space normal. Blue may contain parallax height; alpha may carry signed curvature baked with the normal (0.5 neutral, below = convex edges, above = concave cavities).", {
                toggleField("overrideNormal", "RG • Tangent normal", true, "Checked replaces the sampled tangent normal with a constant."),
                colorField("normal", "Normal override", "#8080FF", "Constant tangent-space normal; neutral is #8080FF."),
                toggleField("overrideHeight", "B • Height", true, "Checked replaces packed height with a constant when B contains height is enabled."),
                numberField("height", "Height override", 0.5, 0.0, 1.0, 0.005, "Constant packed height."),
                numberField("strength", "Normal strength", 1.0, 0.0, 8.0, 0.01, "Scales the tangent-space normal."),
                toggleField("heightInBlue", "B contains height", false, "Checked treats blue as packed height. Unchecked ignores blue for height and reconstructs the normal from RG."),
                choiceField("alphaCarries", "A carries", "None", {"None", "Curvature", "Cone Map"}, "What the normal's alpha stores. Curvature: signed curvature from the same bake as the normal (guides surface states, occludes direct specular in seams, broadens subsurface scattering; no constant, derived from AO otherwise). Cone Map: a relaxed-cone-step parallax map baked from the height in blue; reserved, the preview still derives curvature from AO."),
                numberField("cavitySpecOcclusion", "Cavity specular occlusion", 1.0, 0.0, 1.0, 0.005, "Concave curvature occludes direct-light specular so highlights break inside seams and pores. Only active with authored curvature — the AO-derived fallback must never touch direct light.")
            }));
        add(slot(prefix + "Rmaos", secondary ? tr("%1 Roughness + Metallic + Ambient Occlusion + Specular").arg(layerName) : tr("Roughness + Metallic + Ambient Occlusion + Specular"),
            "RMAOS", "Linear • R roughness / G metallic / B AO / A specular weight or porosity", QColor(108, 136, 111), true,
            QString(),
            "Packed physical material properties. Alpha carries specular weight (OpenPBR specular_weight: a 0-1 multiplier on the level the specular IOR sets) or porosity: dielectrics (cloth, leather) rarely author a weight but are exactly where porosity varies, while metals are sealed — the two uses almost never collide.", {
                toggleField("overrideRoughness", "R • Roughness", true, "Checked uses the constant roughness override."),
                numberField("roughness", "Roughness override", 0.5, 0.0, 1.0, 0.005, "Microsurface roughness used when texture R is overridden."),
                toggleField("overrideMetallic", "G • Metallic", true, "Checked uses the constant metallic override."),
                numberField("metallic", "Metallic override", 0.0, 0.0, 1.0, 0.005, "Metallic response used when texture G is overridden."),
                toggleField("overrideAo", "B • Ambient Occlusion", true, "Checked uses the constant AO override."),
                numberField("ao", "AO override", 1.0, 0.0, 1.0, 0.005, "Indirect-occlusion strength used when texture B is overridden."),
                choiceField("alphaCarries", "A carries", "Specular Weight", {"Specular Weight", "Porosity"}, "What the texture's alpha channel stores. The property not carried by alpha uses its constant (or the derived porosity fallback)."),
                toggleField("overrideSpecularWeight", "A • Specular Weight", true, "Checked uses the constant specular weight override."),
                numberField("specularWeight", "Specular weight override", 1.0, 0.0, 1.0, 0.005, "OpenPBR specular_weight: a 0-1 multiplier on the dielectric reflectance the specular IOR sets. 1 is the full level."),
                toggleField("overridePorosity", "Porosity constant", false, "Checked pins porosity to the constant. Unchecked samples alpha when it carries porosity, otherwise derives saturate(roughness × (1 − metallic)) — the standard engine heuristic, so most materials need no porosity authoring at all."),
                numberField("porosity", "Porosity override", 0.5, 0.0, 1.0, 0.005, "How much fluid soaks in: 1 darkens like cloth, 0 beads like sealed metal.")
            }));
        // v6 (docs/PBRM-v6-Specular.md): the specular colour map is the OpenPBR
        // specular_color tint on the dielectric Fresnel, and its alpha, when
        // declared, is the specular_ior map. The level is IOR times weight;
        // the tint can only darken or hue-shift it. A material without the
        // map takes the material's IOR scalar and a white tint, which is the
        // scalar path bit for bit in the preview and the runtime alike.
        add(slot(prefix + "SpecularColor", secondary ? tr("%1 Specular Color + Specular IOR").arg(layerName) : tr("Specular Color + Specular IOR"),
            "SPEC", "sRGB • RGB specular tint / A specular IOR", QColor(128, 130, 156), false,
            QString(),
            "Tints the dielectric specular reflection (OpenPBR specular_color): RGB multiplies the reflectance the specular IOR and weight set, so white is no change and a color can only darken or shift the hue. Alpha is the specular IOR map over 0 to the IOR map ceiling (texel 90 of 255 is IOR 1.5 at the default ceiling 4.25, texel 0 makes the tint the reflectance outright) while its override is unchecked; otherwise the IOR override applies everywhere. Metals are untouched: their F0 is the base color.", {
                toggleField("overrideColor", "RGB • Specular color", true, "Checked uses the constant specular color below; unchecked samples the texture RGB as the tint."),
                colorField("color", "Specular color override", "#FFFFFF", "Constant sRGB specular tint used when RGB is overridden or unavailable; white is no change."),
                toggleField("overrideIor", "A • Specular IOR", true, "Checked uses the constant specular IOR below everywhere. Unchecked reads the texture alpha as the IOR map; an opaque texture decodes alpha as 1, which would be the ceiling, so leave this checked unless the alpha was authored as IOR."),
                numberField("ior", "Specular IOR override", 1.5, 0.0, 100.0, 0.001, "OpenPBR specular_ior. 1.5 is the dielectric default (F0 0.04); 0 makes the specular color the reflectance outright."),
                numberField("iorMax", "IOR map ceiling", 4.25, 0.01, 100.0, 0.01, "The IOR that texel 255 of an IOR map stands for. 4.25 puts IOR 1.5 exactly on texel 90.")
            }));
    }

    if (mode == Pbr::ShaderMode::StandardSurface || mode == Pbr::ShaderMode::AdvancedSubsurface || all) {
        add(slot(prefix + "TintMask", tr("Tint Mask"), "TINT", "Linear • R/G/B/A independent tint regions", QColor(181, 75, 91), false, {},
            "Four tint masks for per-material or per-instance recoloring.", {
                toggleField("overrideMaskR", "R • Primary tint mask", true, "Checked replaces the sampled primary tint mask with a constant."),
                numberField("maskR", "Primary mask override", 0.0, 0.0, 1.0, 0.005, "Constant primary tint mask."),
                toggleField("overrideMaskG", "G • Secondary tint mask", true, "Checked replaces the sampled secondary tint mask with a constant."),
                numberField("maskG", "Secondary mask override", 0.0, 0.0, 1.0, 0.005, "Constant secondary tint mask."),
                toggleField("overrideMaskB", "B • Tertiary tint mask", true, "Checked replaces the sampled tertiary tint mask with a constant."),
                numberField("maskB", "Tertiary mask override", 0.0, 0.0, 1.0, 0.005, "Constant tertiary tint mask."),
                toggleField("overrideMaskA", "A • Accent tint mask", true, "Checked replaces the sampled accent tint mask with a constant."),
                numberField("maskA", "Accent mask override", 0.0, 0.0, 1.0, 0.005, "Constant accent tint mask."),
                choiceField("overlap", "Overlap handling", "Normalize", {"Normalize", "Add", "Priority RGBA"}, "Controls how overlapping tint masks are combined."),
                colorField("colorR", "Primary color", "#FFFFFF", "Default red-region tint; runtime instances may override it."),
                colorField("colorG", "Secondary color", "#FFFFFF", "Default green-region tint; runtime instances may override it."),
                colorField("colorB", "Tertiary color", "#FFFFFF", "Default blue-region tint; runtime instances may override it."),
                colorField("colorA", "Accent color", "#FFFFFF", "Default alpha-region tint; runtime instances may override it.")
            }));
    }
    if (mode == Pbr::ShaderMode::StandardSurface || mode == Pbr::ShaderMode::AdvancedSubsurface || all) {
        add(slot(prefix + "Emissive", tr("Emissive"), "EMI", "sRGB • RGB emission / A intensity mask", QColor(235, 165, 52), false, {},
            "Adds self-illuminated radiance.", {
                toggleField("overrideColor", "RGB • Emission", true, "Checked replaces sampled emission RGB with a constant."),
                colorField("color", "Emission override", "#FFFFFF", "Constant emission color used when RGB is overridden or unavailable."),
                toggleField("overrideMask", "A • Intensity mask", true, "Checked replaces the sampled intensity mask with a constant."),
                numberField("mask", "Intensity mask override", 1.0, 0.0, 1.0, 0.005, "Constant emissive intensity mask."),
                numberField("intensity", "Intensity", 0.0, 0.0, 1000.0, 0.05, "Emission multiplier.")
            }));
    }

    if ((mode == Pbr::ShaderMode::StandardSurface || mode == Pbr::ShaderMode::AdvancedSubsurface || mode == Pbr::ShaderMode::VegetationSurface || all)
        && prefix == QLatin1String("primary")) {
        // Surface state behavior is runtime-owned as of PBRM v5; the material
        // contributes physical maps (porosity in RMAOS.A, curvature in
        // Normal.A) plus these optional hand-authored placement masks.
        add(slot(prefix + "StateMasks", tr("Surface State Masks"), "SMASK", "Linear • R/G/B/A dedicated per-state masks", QColor(96, 148, 154), false, {},
            "Optional hand-authored placement masks. A runtime surface state bound to one of these channels on the Surface Response page uses it instead of map-guided accumulation — e.g. authored blood splatter or hero-asset snow.", {}));
    }
    if (mode == Pbr::ShaderMode::StandardSurface || mode == Pbr::ShaderMode::AdvancedSubsurface || all) {
        add(slot(prefix + "BentNormal", tr("Bent Normal"), "BENT", "Linear • RG bent normal / B occlusion cone / A mask", QColor(104, 128, 158), false, {},
            "Average unoccluded direction, baked alongside AO. Improves the ambient diffuse direction and derives specular occlusion in cavities instead of applying flat AO to reflections. Blue carries the baked occlusion-cone openness (1 = fully open) for measured specular occlusion.", {
                toggleField("overrideNormal", "RG • Bent normal", true, "Checked replaces the sampled bent normal with a constant."),
                colorField("normal", "Bent normal override", "#8080FF", "Constant tangent-space bent normal; neutral #8080FF equals the shading normal."),
                toggleField("overrideConeOcclusion", "B • Occlusion cone", true, "Checked replaces the baked occlusion-cone channel with a constant."),
                numberField("coneOcclusion", "Occlusion cone override", 1.0, 0.0, 1.0, 0.005, "Constant cone openness; 1 leaves specular occlusion fully derived."),
                toggleField("overrideMask", "A • Mask", true, "Checked uses the constant bent-normal mask override."),
                numberField("mask", "Mask override", 1.0, 0.0, 1.0, 0.005, "Constant bent-normal mask."),
                numberField("strength", "Strength", 1.0, 0.0, 1.0, 0.005, "Blends from the shading normal to the authored bent normal.")
            }));
        add(slot(prefix + "DetailNormal", tr("Detail Normal"), "DTL", "Linear • RG detail normal / B roughness variation / A mask (tiling)", QColor(112, 118, 196), false, {},
            "Tiling close-range surface texture (fabric weave, pores, machining marks) layered whiteout-style over the base normal at its own UV scale. Optional B adds micro roughness breakup at the same scale (BC5 textures simply skip it); optional A masks where detail applies.", {
                toggleField("overrideNormal", "RG • Detail normal", true, "Checked replaces the sampled detail normal with a constant."),
                colorField("normal", "Detail normal override", "#8080FF", "Constant tangent-space detail normal; neutral is #8080FF."),
                toggleField("overrideRoughVariation", "B • Roughness variation", true, "Checked ignores the texture's roughness-variation channel (the default for two-channel BC5 detail normals)."),
                toggleField("overrideMask", "A • Detail mask", true, "Checked applies detail everywhere instead of sampling the mask channel."),
                numberField("mask", "Detail mask override", 1.0, 0.0, 1.0, 0.005, "Constant detail application when the mask channel is overridden."),
                numberField("strength", "Detail strength", 1.0, 0.0, 4.0, 0.01, "Scales the detail perturbation; zero disables the layer."),
                numberField("roughVariation", "Roughness variation", 0.5, 0.0, 1.0, 0.005, "Strength of the B-channel micro roughness breakup (centered at 0.5 gray)."),
                numberField("uvScale", "UV scale", 4.0, 0.25, 64.0, 0.25, "Tiling multiplier relative to the primary UV transform.")
            }));
    }
    if (mode == Pbr::ShaderMode::StandardSurface || all) {
        add(slot(prefix + "Anisotropy", tr("Anisotropy"), "ANISO", "Linear • RG tangent direction / B strength / A mask", QColor(150, 132, 96), false, {},
            "Stretches the specular highlight along an authored tangent direction for brushed metal, satin, and machined surfaces. Mesh tangent is the fallback direction.", {
                toggleField("overrideDirection", "RG • Tangent direction", true, "Checked replaces the sampled anisotropy direction with a constant."),
                colorField("direction", "Tangent direction override", "#FF8000", "Constant encoded RG anisotropy direction; #FF8000 follows the mesh tangent."),
                toggleField("overrideStrength", "B • Strength", true, "Checked uses the constant anisotropy-strength override."),
                numberField("strength", "Strength override", 0.0, 0.0, 1.0, 0.005, "Constant anisotropy strength when texture B is overridden or absent."),
                toggleField("overrideMask", "A • Mask", true, "Checked uses the constant anisotropy-mask override."),
                numberField("mask", "Mask override", 1.0, 0.0, 1.0, 0.005, "Constant anisotropy mask."),
                numberField("rotation", "Direction rotation", 0.0, -360.0, 360.0, 1.0, "Rotates the authored tangent direction.", 2, "°"),
                numberField("intensity", "Anisotropy amount", 1.0, 0.0, 2.0, 0.01, "Scales the sampled anisotropy strength.")
            }));
    }
    const bool usesSurfaceEffects = mode == Pbr::ShaderMode::StandardSurface || all;
    if (usesSurfaceEffects) {
        add(slot(prefix + "SurfaceEffects", tr("Surface Effects"), "FX", "Linear • R fuzz / G glints / B film thickness / A iridescence", QColor(137, 91, 169), false, {},
            "Packed masks for optional surface lobes and the lobes' physical response. The lobes are off while this input is disabled.", {
                toggleField("overrideFuzz", "R • Fuzz", true, "Checked uses the constant fuzz override."),
                numberField("fuzzFallback", "Fuzz override", 0.0, 0.0, 1.0, 0.005, "Constant fuzz mask when texture R is overridden or absent."),
                toggleField("overrideGlints", "G • Glints", true, "Checked uses the constant glint override."),
                numberField("glintsFallback", "Glints override", 0.0, 0.0, 1.0, 0.005, "Constant glint density when texture G is overridden or absent."),
                toggleField("overrideFilm", "B • Film thickness", true, "Checked uses the constant film-thickness override."),
                numberField("filmFallback", "Film thickness override", 0.5, 0.0, 1.0, 0.005, "Thin-film thickness when texture B is overridden or absent."),
                toggleField("overrideIridescence", "A • Iridescence", true, "Checked uses the constant iridescence override."),
                numberField("iridescenceFallback", "Iridescence override", 0.0, 0.0, 1.0, 0.005, "Iridescence mask when texture A is overridden or absent."),
                numberField("fuzzStrength", "Fuzz strength", 0.0, 0.0, 8.0, 0.01, "Grazing fabric fuzz (sheen) reflection masked by R."),
                numberField("fuzzRoughness", "Fuzz roughness", 0.6, 0.0, 1.0, 0.005, "Width of the fuzz lobe."),
                numberField("glintIntensity", "Glint intensity", 0.0, 0.0, 100.0, 0.01, "Sparse microfacet sparkle masked by G."),
                numberField("glintScale", "Glint scale", 1.0, 0.001, 10000.0, 0.01, "Spatial frequency of procedural glints."),
                numberField("filmIor", "Thin-film IOR", 1.5, 1.0, 3.0, 0.001, "IOR of the iridescent thin film."),
                numberField("iridescenceStrength", "Iridescence strength", 0.0, 0.0, 4.0, 0.01, "Angle-dependent thin-film interference masked by A.")
            }));
        add(slot(prefix + "Retroreflection", tr("Retroreflection"), "RETRO", "Linear • R retro mask", QColor(196, 150, 62), false, {},
            "Mask of where the surface bounces light back toward its source (road signs, safety tape, cat-eye markers). Strongest when the viewer is near the light, e.g. a Pip-Boy lamp at night. Off while this input is disabled.", {
                toggleField("overrideRetro", "R • Retro mask", true, "Checked uses the constant retro mask override."),
                numberField("retroFallback", "Retro mask override", 1.0, 0.0, 1.0, 0.005, "Constant retro mask when texture R is overridden or absent."),
                numberField("retroStrength", "Retroreflection strength", 1.0, 0.0, 8.0, 0.01, "Strength of the lobe aimed back at the light, masked by R."),
                numberField("retroWidth", "Retroreflection width", 0.25, 0.0, 1.0, 0.005, "Angular width of the retroreflective lobe.")
            }));
    }

    if (mode == Pbr::ShaderMode::StandardSurface || all) {
        add(slot(prefix + "SeparateHeight", tr("Separate Height"), "HGT", "Linear • R height", QColor(126, 126, 126), false, {},
            "Alternative height source when height is not packed in Normal Blue. Geometry settings choose parallax, displacement, or both.", {
                toggleField("overrideHeight", "R • Height", true, "Checked replaces sampled height with a constant."),
                numberField("height", "Height override", 0.5, 0.0, 1.0, 0.005, "Constant height; 0.5 is the flat midpoint."),
                toggleField("invert", "Invert", false, "Reverses high and low regions.")
            }, {"R"}));
    }
    if (mode == Pbr::ShaderMode::StandardSurface || mode == Pbr::ShaderMode::AdvancedSubsurface || all) {
        add(slot(prefix + "DynamicNormal", tr("Dynamic Normal"), "DYN", "Linear • RG/BC5 animation-driven normal", QColor(103, 106, 188), false,
            secondary ? QString() : QStringLiteral("Actors\\Character\\BaseHumanMale\\HeadWrinkles_n.dds"),
            "Additional normal layer driven by facial expression, animation, vertex data, or a runtime parameter. Driver settings live on Dynamic Normals.", {
                toggleField("overrideNormal", "RG • Dynamic normal", true, "Checked replaces the sampled dynamic normal with a constant."),
                colorField("normal", "Dynamic normal override", "#8080FF", "Constant tangent-space normal; neutral is #8080FF."),
                numberField("strength", "Strength", 1.0, 0.0, 8.0, 0.01, "Maximum dynamic-normal contribution."),
                choiceField("blend", "Normal blend", "Reoriented", {"Reoriented", "Whiteout", "Linear"}, "Method used to combine this normal with the base normal.")
            }, {"R", "G"}));
    }

    if (mode == Pbr::ShaderMode::StandardSurface || mode == Pbr::ShaderMode::VegetationSurface || mode == Pbr::ShaderMode::AdvancedSubsurface || all) {
        add(slot(prefix + "Subsurface", tr("Subsurface Scattering / Transmission"), "SS", "Linear • RGB scatter tint / A thickness", QColor(202, 96, 91), false, {},
            "Subsurface diffusion, thin transmission, or both. The feature is on while this input is enabled, with or without a map.", {
                toggleField("overrideScatterTint", "RGB • Scatter tint", true, "Checked replaces sampled scatter tint with a constant."),
                colorField("scatterTint", "Scatter tint override", "#FFFFFF", "Constant scatter or transmission tint."),
                toggleField("overrideThicknessValue", "A • Thickness", true, "Checked replaces sampled thickness with a constant."),
                numberField("thicknessValue", "Thickness override", 1.0, 0.0, 1.0, 0.005, "Constant normalized thickness."),
                choiceField("mode", "Mode", mode == Pbr::ShaderMode::VegetationSurface ? "Thin Transmission" : "Diffusion Profile", {"Diffusion Profile", "Thin Transmission", "Combined"}, "Thick diffusion, thin transmission, or both."),
                choiceField("profile", "Diffusion profile", "Skin", {"Skin", "Wax", "Flesh", "Plant", "Custom"}, "Profile defining wavelength-dependent scatter radius."),
                enabledWhen(colorField("customScatterColor", "Custom scatter color", "#FFB39D", "Wavelength tint used when Diffusion profile is Custom."), "profile", {"Custom"}),
                enabledWhen(numberField("customRadiusR", "Custom radius R", 1.0, 0.001, 1000.0, 0.01, "Red-channel diffusion radius for a Custom profile."), "profile", {"Custom"}),
                enabledWhen(numberField("customRadiusG", "Custom radius G", 0.35, 0.001, 1000.0, 0.01, "Green-channel diffusion radius for a Custom profile."), "profile", {"Custom"}),
                enabledWhen(numberField("customRadiusB", "Custom radius B", 0.2, 0.001, 1000.0, 0.01, "Blue-channel diffusion radius for a Custom profile."), "profile", {"Custom"}),
                numberField("weight", "Scatter weight", 1.0, 0.0, 4.0, 0.01, "Scales profile-driven scattering."),
                numberField("transmission", "Transmission", 0.0, 0.0, 4.0, 0.01, "Scales light passing through the surface."),
                enabledWhen(colorField("transmissionTint", "Transmission tint", "#FFFFFF", "Multiplies the sampled scatter color for the thin-transmission lobe only, so backlight can be tinted independently of diffusion (e.g. a warm leaf backlight over green diffusion)."), "mode", {"Thin Transmission", "Combined"}),
                numberField("thickness", "Thickness scale", 1.0, 0.0, 1000.0, 0.01, "Scales the texture or constant thickness.")
            }));
    }
    if (mode == Pbr::ShaderMode::HairSurface || all) {
        add(slot(prefix + "HairDirection", tr("Hair Direction"), "FLOW", "Linear • RG tangent flow / B lobe shift / A scatter", QColor(142, 104, 73), true, {},
            "Defines strand direction for anisotropic highlights. Mesh tangent is used as the fallback.", {
                toggleField("overrideFlow", "RG • Tangent flow", true, "Checked replaces sampled strand flow with a constant."),
                colorField("flow", "Tangent flow override", "#808000", "Constant encoded RG strand direction."),
                toggleField("overrideLobeShift", "B • Lobe shift", true, "Checked replaces sampled lobe shift with a constant."),
                numberField("lobeShift", "Lobe shift override", 0.5, 0.0, 1.0, 0.005, "Constant lobe-shift value."),
                toggleField("overrideScatter", "A • Scatter", true, "Checked replaces sampled scatter with a constant."),
                numberField("scatter", "Scatter override", 1.0, 0.0, 1.0, 0.005, "Constant scatter mask."),
                numberField("strength", "Direction strength", 1.0, 0.0, 4.0, 0.01, "Blends from mesh tangent to texture-authored direction."),
                choiceField("direction", "Direction source", "Hair Direction RG", {"Hair Direction RG", "Mesh Tangent"}, "Strand direction around which highlights stretch."),
                numberField("primaryStrength", "Primary strength", 1.0, 0.0, 8.0, 0.01, "Strength of the sharp primary hair highlight."),
                numberField("primaryRoughness", "Primary roughness", 0.25, 0.0, 1.0, 0.005, "Width of the primary hair highlight."),
                numberField("primaryShift", "Primary shift", -0.08, -1.0, 1.0, 0.002, "Moves the primary highlight along the strand direction."),
                numberField("secondaryStrength", "Secondary strength", 0.5, 0.0, 8.0, 0.01, "Strength of the broad colored secondary highlight."),
                numberField("secondaryRoughness", "Secondary roughness", 0.45, 0.0, 1.0, 0.005, "Width of the secondary highlight."),
                numberField("secondaryShift", "Secondary shift", 0.12, -1.0, 1.0, 0.002, "Moves the secondary highlight along the strand direction.")
            }));
        add(slot(prefix + "HairScatter", tr("Hair Scatter"), "HSS", "Linear • RGB scatter tint / A backlight mask", QColor(176, 116, 88), false, {},
            "Controls light passing through and around hair cards.", {
                toggleField("overrideScatterTint", "RGB • Scatter tint", true, "Checked replaces sampled hair scatter tint with a constant."),
                colorField("scatterTint", "Scatter tint override", "#8B5A3C", "Constant hair-fiber scatter tint."),
                toggleField("overrideBacklight", "A • Backlight mask", true, "Checked replaces sampled backlight mask with a constant."),
                numberField("backlight", "Backlight override", 1.0, 0.0, 1.0, 0.005, "Constant backlight mask."),
                numberField("scatterStrength", "Scatter", 0.5, 0.0, 4.0, 0.01, "Light scattered through the hair fiber volume."),
                numberField("backlightStrength", "Backlight", 0.5, 0.0, 4.0, 0.01, "Transmission visible when light is behind hair cards.")
            }));
    }
    if (mode == Pbr::ShaderMode::GlassSurface || all) {
        add(slot(prefix + "TransmissionAbsorption", tr("Transmission / Absorption"), "ABS", "Linear • RGB absorption / A thickness", QColor(80, 141, 157), true, {},
            "Beer-Lambert absorption color and optical thickness for glass.", {
                toggleField("overrideAbsorption", "RGB • Absorption", true, "Checked replaces sampled absorption with a constant."),
                colorField("absorption", "Absorption override", "#FFFFFF", "Constant Beer-Lambert absorption color."),
                toggleField("overrideThickness", "A • Thickness", true, "Checked uses the constant optical-thickness override."),
                numberField("thickness", "Thickness override", 1.0, 0.0, 1000.0, 0.01, "Constant thickness when texture alpha is overridden or unavailable."),
                numberField("ior", "Index of refraction", 1.5, 1.0, 3.0, 0.001, "Physical glass IOR."),
                numberField("absorptionDistance", "Absorption distance", 1.0, 0.0001, 100000.0, 0.01, "World-space distance at which the authored absorption color is reached."),
                numberField("refractionRoughness", "Refraction roughness", 0.0, 0.0, 1.0, 0.005, "Blurs transmitted scene radiance."),
                toggleField("thinWalled", "Thin walled", false, "Treats geometry as an infinitely thin sheet when thickness is unavailable.")
            }));
        add(slot(prefix + "InteriorMap", tr("Interior Map"), "ROOM", "sRGB • Room cubemap RGB / A lit-window mask (DDS)", QColor(94, 118, 96), false, {},
            "Parallax cubemap rooms rendered behind the glass (building windows). The runtime projects a fake interior per window pane; the preview evaluates the same ray-box lookup. The cubemap's alpha marks self-lit interior areas that glow independently of scene lighting.", {
                numberField("roomsU", "Rooms across U", 1.0, 0.25, 64.0, 0.25, "Window panes per UV tile horizontally.", 2),
                numberField("roomsV", "Rooms across V", 1.0, 0.25, 64.0, 0.25, "Window panes per UV tile vertically.", 2),
                numberField("depth", "Room depth", 1.0, 0.05, 8.0, 0.01, "Apparent depth of the room relative to its width."),
                numberField("brightness", "Interior brightness", 1.0, 0.0, 32.0, 0.01, "Scales the room's emitted light."),
                numberField("windowEmissive", "Lit-window emissive", 0.0, 0.0, 64.0, 0.05, "Self-illumination of alpha-masked interior areas — windows that glow at night. Zero disables the pass.")
            }, {}));
        add(slot(prefix + "Imperfections", tr("Surface Imperfections"), "IMP", "Linear • R smudges / G scratches / B dust / A mask", QColor(128, 131, 135), false, {},
            "Modulates roughness, normal response, and transmission with small glass imperfections.", {
                toggleField("overrideSmudges", "R • Smudges", true, "Checked replaces sampled smudges with a constant."),
                numberField("smudges", "Smudges override", 0.0, 0.0, 1.0, 0.005, "Constant smudge mask."),
                toggleField("overrideScratches", "G • Scratches", true, "Checked replaces sampled scratches with a constant."),
                numberField("scratches", "Scratches override", 0.0, 0.0, 1.0, 0.005, "Constant scratch mask."),
                toggleField("overrideDust", "B • Dust", true, "Checked replaces sampled dust with a constant."),
                numberField("dust", "Dust override", 0.0, 0.0, 1.0, 0.005, "Constant dust mask."),
                toggleField("overrideMask", "A • Imperfection mask", true, "Checked replaces the sampled imperfection mask with a constant."),
                numberField("mask", "Imperfection mask override", 1.0, 0.0, 1.0, 0.005, "Constant overall imperfection mask.")
            }));
    }
    if (mode == Pbr::ShaderMode::WaterSurface || all) {
        add(slot(prefix + "WaterFlow", tr("Water Flow"), "FLOW", "Linear • RG flow / B turbulence / A shoreline", QColor(63, 124, 154), true, {},
            "Animates normal, foam, and distortion sampling along an authored flow field.", {
                toggleField("overrideFlow", "RG • Flow direction", true, "Checked replaces sampled water flow with a constant."),
                colorField("flow", "Flow direction override", "#808000", "Constant encoded RG flow direction."),
                toggleField("overrideTurbulence", "B • Turbulence", true, "Checked replaces sampled turbulence with a constant."),
                numberField("turbulence", "Turbulence override", 0.0, 0.0, 1.0, 0.005, "Constant turbulence."),
                toggleField("overrideShoreline", "A • Shoreline", true, "Checked replaces sampled shoreline mask with a constant."),
                numberField("shoreline", "Shoreline override", 0.0, 0.0, 1.0, 0.005, "Constant shoreline mask."),
                numberField("speed", "Flow speed", 0.05, -10.0, 10.0, 0.001, "Signed flow-animation speed."),
                numberField("strength", "Flow strength", 1.0, 0.0, 10.0, 0.01, "Flow-field influence.")
            }));
        add(slot(prefix + "Foam", tr("Foam"), "FOAM", "sRGB • RGB foam / A coverage", QColor(181, 202, 208), false, {},
            "Foam color and coverage for shore intersections and wave crests.", {
                toggleField("overrideColor", "RGB • Foam color", true, "Checked replaces sampled foam color with a constant."),
                colorField("color", "Foam color override", "#E8F0F2", "Constant foam color."),
                toggleField("overrideCoverage", "A • Foam coverage", true, "Checked replaces sampled foam coverage with a constant."),
                numberField("coverage", "Foam coverage override", 1.0, 0.0, 1.0, 0.005, "Constant foam coverage.")
            }));
    }
    if (mode == Pbr::ShaderMode::AdvancedSubsurface || all) {
        add(slot(prefix + "SubsurfaceDetail", tr("Subsurface Detail"), "SKIN", "Linear • R epidermal / G blood / B oil / A mask", QColor(181, 102, 89), false, {},
            "Optional profile mixing and high-frequency skin response.", {
                toggleField("overrideEpidermal", "R • Epidermal", true, "Checked replaces sampled epidermal weight with a constant."),
                numberField("epidermal", "Epidermal override", 0.0, 0.0, 1.0, 0.005, "Constant epidermal weight."),
                toggleField("overrideBlood", "G • Blood", true, "Checked replaces sampled blood weight with a constant."),
                numberField("blood", "Blood override", 0.0, 0.0, 1.0, 0.005, "Constant blood weight."),
                toggleField("overrideOil", "B • Oil", true, "Checked replaces sampled oil response with a constant."),
                numberField("oil", "Oil override", 0.0, 0.0, 1.0, 0.005, "Constant oil response."),
                toggleField("overrideMask", "A • Detail mask", true, "Checked replaces sampled detail mask with a constant."),
                numberField("mask", "Detail mask override", 1.0, 0.0, 1.0, 0.005, "Constant detail mask.")
            }));
    }
    if (mode == Pbr::ShaderMode::EyeSurface || all) {
        add(slot(prefix + "IrisMasks", tr("Iris Depth + Masks"), "IRIS", "Linear • R height / G limbal / B pupil / A cornea", QColor(72, 123, 130), true, {},
            "Iris depth and eye-region masks expected by the Eye shader.", {
                toggleField("overrideHeight", "R • Iris height", true, "Checked replaces sampled iris height with a constant."),
                numberField("height", "Iris height override", 0.5, 0.0, 1.0, 0.005, "Constant iris height."),
                toggleField("overrideLimbal", "G • Limbal mask", true, "Checked replaces sampled limbal mask with a constant."),
                numberField("limbal", "Limbal mask override", 0.0, 0.0, 1.0, 0.005, "Constant limbal-ring mask."),
                toggleField("overridePupil", "B • Pupil mask", true, "Checked replaces sampled pupil mask with a constant."),
                numberField("pupil", "Pupil mask override", 0.0, 0.0, 1.0, 0.005, "Constant pupil mask."),
                toggleField("overrideCornea", "A • Cornea coverage", true, "Checked replaces sampled cornea coverage with a constant."),
                numberField("cornea", "Cornea coverage override", 1.0, 0.0, 1.0, 0.005, "Constant wet-layer coverage."),
                numberField("irisDepth", "Iris depth", 0.08, 0.0, 1.0, 0.001, "Distance of the iris below the cornea."),
                numberField("limbalStrength", "Limbal strength", 1.0, 0.0, 4.0, 0.01, "Dark ring around the outside of the iris."),
                numberField("pupilScale", "Pupil scale", 1.0, 0.0, 4.0, 0.01, "Runtime-controllable pupil dilation.")
            }));
        add(slot(prefix + "Cornea", tr("Cornea"), "COR", "Linear • RG normal / B roughness / A coverage", QColor(87, 142, 168), true, {},
            "Clear wet outer lobe covering the iris and sclera.", {
                toggleField("overrideNormal", "RG • Cornea normal", true, "Checked replaces sampled cornea normal with a constant."),
                colorField("normal", "Cornea normal override", "#8080FF", "Constant tangent-space cornea normal."),
                toggleField("overrideRoughness", "B • Cornea roughness", true, "Checked replaces sampled cornea roughness with a constant."),
                numberField("roughness", "Cornea roughness override", 0.025, 0.0, 1.0, 0.001, "Constant outer wet-lobe roughness."),
                toggleField("overrideCoverage", "A • Cornea coverage", true, "Checked replaces sampled cornea coverage with a constant."),
                numberField("coverage", "Cornea coverage override", 1.0, 0.0, 1.0, 0.005, "Constant cornea coverage."),
                numberField("ior", "Cornea IOR", 1.376, 1.0, 2.0, 0.001, "Physical cornea index of refraction."),
                numberField("wetness", "Wet reflection", 1.0, 0.0, 4.0, 0.01, "Strength of the clear wet outer lobe.")
            }));
    }
    if (mode == Pbr::ShaderMode::StandardSurface || mode == Pbr::ShaderMode::GlassSurface || mode == Pbr::ShaderMode::EyeSurface || all) {
        add(slot(prefix + "Clearcoat", tr("Clearcoat"), "COAT", "Linear • RG coat normal / B roughness / A coverage", QColor(82, 136, 167), false, {},
            "Independent clear surface layer. It can be authored on any UV set, so its normal, roughness, and coverage move with that layer's transforms.", {
                toggleField("overrideNormal", "RG • Clearcoat normal", true, "Checked replaces sampled clearcoat normal with a constant."),
                colorField("normal", "Clearcoat normal override", "#8080FF", "Constant tangent-space clearcoat normal."),
                toggleField("overrideRoughness", "B • Clearcoat roughness", true, "Checked uses the constant clearcoat-roughness override."),
                numberField("roughness", "Roughness override", 0.1, 0.0, 1.0, 0.005, "Clearcoat roughness used when texture B is overridden or absent."),
                toggleField("overrideCoverage", "A • Clearcoat coverage", true, "Checked uses the constant clearcoat-coverage override."),
                numberField("coverage", "Coverage override", 1.0, 0.0, 1.0, 0.005, "Clearcoat mask used when texture A is overridden or absent."),
                numberField("normalStrength", "Normal strength", 1.0, 0.0, 8.0, 0.01, "Strength of the independent clearcoat normal."),
                numberField("strength", "Clearcoat strength", 1.0, 0.0, 4.0, 0.01, "Scales the complete clearcoat reflection lobe after texture coverage."),
                numberField("ior", "Clearcoat IOR", 1.5, 1.0, 3.0, 0.001, "Index of refraction used to derive clearcoat dielectric F0."),
                colorField("tint", "Coat tint", "#FFFFFF", "Tints light passing through the coat, darkening and saturating the layers beneath (amber varnish, candy paint). The coat's own reflection stays untinted."),
                numberField("absorption", "Tint absorption", 1.0, 0.0, 1.0, 0.005, "How strongly the coat tint absorbs light on its way through the coat.")
            }));
    }
    if (secondary && (mode == Pbr::ShaderMode::StandardSurface || all)) {
        add(slot(prefix + "Gradient", tr("%1 Color Gradient").arg(layerName), "GRAD", "sRGB • RGB ramp / A coverage", QColor(87, 147, 211), false, {},
            "Recolors a white or neutral underlying surface from a mask-driven gradient. Gradient alpha multiplies layer coverage.", {
                toggleField("overrideColor", "RGB • Gradient color", true, "Checked replaces sampled gradient color with a constant."),
                colorField("color", "Gradient color override", "#FFFFFF", "Constant gradient color."),
                toggleField("overrideCoverage", "A • Gradient coverage", true, "Checked replaces sampled gradient coverage with a constant."),
                numberField("coverage", "Gradient coverage override", 1.0, 0.0, 1.0, 0.005, "Constant gradient coverage."),
                choiceField("driver", "Driver", "Coverage to U", {"Coverage to U", "Vertex Color to U", "Constant"}, "Value used to sample the gradient."),
                numberField("constantDriver", "Constant driver value", 0.5, 0.0, 1.0, 0.005, "Gradient coordinate used when Driver is Constant."),
                numberField("intensity", "Intensity", 1.0, 0.0, 32.0, 0.01, "Scales gradient color and alpha.")
            }));
        add(slot(prefix + "LayerCoverage", tr("Layer Coverage"), "MASK", "Linear • R coverage", QColor(190, 190, 190), false, {},
            "Optional separate Secondary UV coverage mask. It combines with vertex paint and high-frequency pattern modulation.", {
                toggleField("overrideCoverage", "R • Coverage", true, "Checked replaces sampled layer coverage with a constant."),
                numberField("coverage", "Coverage override", 1.0, 0.0, 1.0, 0.005, "Constant layer coverage."),
                toggleField("invert", "Invert", false, "Reverses covered and uncovered regions."),
                numberField("bias", "Bias", 0.0, -1.0, 1.0, 0.002, "Moves the coverage threshold."),
                numberField("contrast", "Contrast", 1.0, 0.0, 16.0, 0.01, "Sharpens or softens the mask.")
            }, {"R"}));
    }
    if (all) {
        add(slot(prefix + "ScreenDistortion", tr("Screen Distortion"), "DIST", "Linear • RG direction / B strength / A mask", QColor(82, 137, 150), false, {},
            "Optional scene-color distortion for the authoring superset.", {
                toggleField("overrideDirection", "RG • Distortion direction", true, "Checked replaces sampled distortion direction with a constant."),
                colorField("direction", "Direction override", "#808000", "Constant encoded RG direction."),
                toggleField("overrideStrength", "B • Distortion strength", true, "Checked replaces sampled distortion strength with a constant."),
                numberField("strength", "Distortion strength override", 0.0, 0.0, 1.0, 0.005, "Constant distortion strength."),
                toggleField("overrideMask", "A • Distortion mask", true, "Checked replaces sampled distortion mask with a constant."),
                numberField("mask", "Distortion mask override", 1.0, 0.0, 1.0, 0.005, "Constant distortion mask.")
            }));
        add(slot(prefix + "ParticleGradient", tr("Particle Gradient"), "GRAD", "sRGB • RGB ramp / A opacity", QColor(222, 112, 69), false, {},
            "Optional effect-style color and opacity gradient.", {
                toggleField("overrideColor", "RGB • Gradient color", true, "Checked replaces sampled gradient color with a constant."),
                colorField("color", "Gradient color override", "#FFFFFF", "Constant gradient color."),
                toggleField("overrideOpacity", "A • Gradient opacity", true, "Checked replaces sampled gradient opacity with a constant."),
                numberField("opacity", "Gradient opacity override", 1.0, 0.0, 1.0, 0.005, "Constant gradient opacity.")
            }));
    }
    return result;
}

QVector<TextureSlotSpec> MainWindow::landscapeSlots() const
{
    return {
        slot("baseColorArray", tr("Landscape Base Color Array"), "L-BASE", "sRGB texture array • RGB color / A coverage",
            QColor(150, 111, 75), true, {}, "Base-color slices selected by LTEX/TXST layer indices.", {
                toggleField("overrideColor", "RGB • Base color", true, "Checked replaces sampled landscape base color with a constant."),
                colorField("color", "Base color override", "#FFFFFF", "Constant landscape base color."),
                toggleField("overrideCoverage", "A • Coverage", true, "Checked replaces sampled landscape coverage with a constant."),
                numberField("coverage", "Coverage override", 1.0, 0.0, 1.0, 0.005, "Constant landscape coverage.")
            }),
        slot("normalHeightArray", tr("Landscape Normal + Height Array"), "L-NRM", "Linear texture array • RG normal / B height / A porosity",
            QColor(104, 106, 190), true, {}, "Normal and optional height slices for each landscape material. Alpha carries per-layer porosity so terrain responds to runtime surface states (rain darkening, drying).", {
                toggleField("overrideNormal", "RG • Tangent normal", true, "Checked replaces sampled landscape normal with a constant."),
                colorField("normal", "Normal override", "#8080FF", "Constant tangent-space normal."),
                toggleField("overrideHeight", "B • Height", true, "Checked replaces sampled landscape height with a constant."),
                numberField("height", "Height override", 0.5, 0.0, 1.0, 0.005, "Constant landscape height."),
                toggleField("overridePorosity", "A • Porosity", true, "Checked replaces the sampled surface-state porosity with a constant."),
                numberField("porosity", "Porosity override", 0.5, 0.0, 1.0, 0.005, "How much fluid soaks into this terrain layer: 1 darkens like soil, 0 beads like rock."),
                toggleField("heightInBlue", "B • Height", true, "Uses blue for height-aware blending and optional parallax."),
                numberField("normalStrength", "Normal strength", 1.0, 0.0, 8.0, 0.01, "Landscape normal multiplier.")
            }),
        slot("rmaosArray", tr("Landscape Roughness + Metallic + AO + Specular Array"), "L-RMAOS", "Linear texture array • R roughness / G metallic / B AO / A F0",
            QColor(101, 132, 106), true, {}, "Packed physical-property slices for every landscape material.", {
                toggleField("overrideRoughness", "R • Roughness", true, "Checked uses constant roughness for the array."),
                numberField("roughness", "Roughness override", 0.5, 0.0, 1.0, 0.005, "Constant landscape roughness."),
                toggleField("overrideMetallic", "G • Metallic", true, "Checked uses constant metallic for the array."),
                numberField("metallic", "Metallic override", 0.0, 0.0, 1.0, 0.005, "Constant landscape metallic value."),
                toggleField("overrideAo", "B • Ambient Occlusion", true, "Checked uses constant ambient occlusion for the array."),
                numberField("ao", "AO override", 1.0, 0.0, 1.0, 0.005, "Constant landscape ambient occlusion."),
                toggleField("overrideSpecularWeight", "A • Specular Weight", true, "Checked uses the constant specular weight override."),
                numberField("specularWeight", "Specular weight override", 1.0, 0.0, 1.0, 0.005, "OpenPBR specular_weight: a 0-1 multiplier on the dielectric reflectance the specular IOR sets. 1 is the full level.")
            }),
        slot("heightArray", tr("Optional Separate Height Array"), "L-HGT", "Linear texture array • R height",
            QColor(126, 126, 126), false, {}, "Alternative height slices when height is not stored in the normal array.", {
                toggleField("overrideHeight", "R • Height", true, "Checked replaces sampled landscape height with a constant."),
                numberField("height", "Height override", 0.5, 0.0, 1.0, 0.005, "Constant landscape height.")
            }, {"R"}),
        slot("macroVariation", tr("Macro Variation"), "MACRO", "sRGB/Linear • RGB color breakup / A roughness breakup",
            QColor(118, 126, 89), false, {}, "Large-scale world-mapped breakup shared across terrain layers.", {
                toggleField("overrideColor", "RGB • Color breakup", true, "Checked replaces sampled macro color with a constant."),
                colorField("color", "Color breakup override", "#808080", "Constant macro color breakup."),
                toggleField("overrideRoughness", "A • Roughness breakup", true, "Checked replaces sampled roughness breakup with a constant."),
                numberField("roughness", "Roughness breakup override", 0.5, 0.0, 1.0, 0.005, "Constant macro roughness breakup."),
                numberField("scale", "World scale", 2048.0, 0.001, 1000000.0, 1.0, "World-space macro texture scale."),
                numberField("colorStrength", "Color strength", 0.15, 0.0, 4.0, 0.005, "Macro color variation strength."),
                numberField("roughnessStrength", "Roughness strength", 0.1, 0.0, 4.0, 0.005, "Alpha-driven roughness variation strength.")
            })
    };
}

