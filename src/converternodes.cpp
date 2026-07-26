#include "converternodes.h"

#include "convertercore.h"
#include "geometryao.h"
#include "normalderive.h"
#include "previewmesh.h"
#include "textureimage.h"
#include "upscaler.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSaveFile>
#include <QSettings>
#include <QtMath>

using namespace NodeGraph;

namespace ConverterNodes {

const char* const kMaterialTag = "bgsm.material";

namespace {

// ---- parameter helpers ----

float paramFloat(const QJsonObject& params, const char* id, float fallback)
{
    const QJsonValue value = params.value(QLatin1String(id));
    return value.isDouble() ? float(value.toDouble()) : fallback;
}

int paramInt(const QJsonObject& params, const char* id, int fallback)
{
    const QJsonValue value = params.value(QLatin1String(id));
    return value.isDouble() ? value.toInt() : fallback;
}

bool paramBool(const QJsonObject& params, const char* id, bool fallback)
{
    const QJsonValue value = params.value(QLatin1String(id));
    return value.isBool() ? value.toBool() : fallback;
}

QString paramString(const QJsonObject& params, const char* id)
{
    return params.value(QLatin1String(id)).toString();
}

ParamSpec floatParam(const char* id, const char* name, double value, double minimum, double maximum,
    double step = 0.01, const char* tooltip = "")
{
    ParamSpec spec;
    spec.id = QLatin1String(id); spec.name = QLatin1String(name); spec.kind = ParamSpec::Kind::Float;
    spec.minimum = minimum; spec.maximum = maximum; spec.step = step;
    spec.defaultValue = value; spec.tooltip = QLatin1String(tooltip);
    return spec;
}

ParamSpec intParam(const char* id, const char* name, int value, int minimum, int maximum, const char* tooltip = "")
{
    ParamSpec spec;
    spec.id = QLatin1String(id); spec.name = QLatin1String(name); spec.kind = ParamSpec::Kind::Int;
    spec.minimum = minimum; spec.maximum = maximum; spec.step = 1;
    spec.defaultValue = value; spec.tooltip = QLatin1String(tooltip);
    return spec;
}

ParamSpec boolParam(const char* id, const char* name, bool value, const char* tooltip = "")
{
    ParamSpec spec;
    spec.id = QLatin1String(id); spec.name = QLatin1String(name); spec.kind = ParamSpec::Kind::Bool;
    spec.defaultValue = value; spec.tooltip = QLatin1String(tooltip);
    return spec;
}

ParamSpec enumParam(const char* id, const char* name, int value, const QStringList& labels, const char* tooltip = "")
{
    ParamSpec spec;
    spec.id = QLatin1String(id); spec.name = QLatin1String(name); spec.kind = ParamSpec::Kind::Enum;
    spec.enumLabels = labels; spec.minimum = 0; spec.maximum = labels.size() - 1; spec.step = 1;
    spec.defaultValue = value; spec.tooltip = QLatin1String(tooltip);
    return spec;
}

PortSpec imagePort(const char* id, const char* name, bool optional = false)
{
    PortSpec spec;
    spec.id = QLatin1String(id); spec.name = QLatin1String(name);
    spec.type = ValueType::Image; spec.optional = optional;
    return spec;
}

// Specular F0 for a dielectric: 0.04 linear, stored straight into RMAOS.A as
// 8-bit. Every common non-metal sits within a rounding error of this, which is
// why the stock conversions hold it constant instead of deriving it per texel.
constexpr int kDielectricF0 = 10; // 0.04 * 255

// What the three shipped FO4 PBR textures actually carry, stated once. The
// Combine node packs to this layout and the terminal writes it out, so both
// describe their outputs from the same tables — they cannot drift apart.
const QVector<ChannelMap> kBaseColorChannels{
    {QStringLiteral("Base Color"), kChannelsRgb},
    {QStringLiteral("Opacity"), ChannelA}};
const QVector<ChannelMap> kNormalChannels{
    {QStringLiteral("Normal (RG)"), ChannelR | ChannelG},
    {QStringLiteral("Height"), ChannelB},
    {QStringLiteral("Curvature"), ChannelA}};
const QVector<ChannelMap> kRmaosChannels{
    {QStringLiteral("Roughness"), ChannelR},
    {QStringLiteral("Metallic"), ChannelG},
    {QStringLiteral("Ambient Occlusion"), ChannelB},
    {QStringLiteral("Specular F0"), ChannelA}};

// An image output whose channels are separate maps. Declaring them lets the
// preview offer "Roughness" on its own without anything downstream having to
// know that roughness happens to live in RMAOS.R.
PortSpec packedPort(const char* id, const char* name, QVector<ChannelMap> maps, bool optional = false)
{
    PortSpec spec = imagePort(id, name, optional);
    spec.channelMaps = std::move(maps);
    return spec;
}

PortSpec scalarPort(const char* id, const char* name, bool optional = false)
{
    PortSpec spec;
    spec.id = QLatin1String(id); spec.name = QLatin1String(name); spec.type = ValueType::Scalar;
    spec.optional = optional;
    return spec;
}

PortSpec colorPort(const char* id, const char* name, bool optional = false)
{
    PortSpec spec;
    spec.id = QLatin1String(id); spec.name = QLatin1String(name);
    spec.type = ValueType::Color; spec.optional = optional;
    return spec;
}

PortSpec materialPort(const char* id, const char* name, bool optional = false)
{
    PortSpec spec;
    spec.id = QLatin1String(id); spec.name = QLatin1String(name);
    spec.type = ValueType::Opaque; spec.opaqueTag = QLatin1String(kMaterialTag); spec.optional = optional;
    return spec;
}

// The Upscale node's map setting used to be a three-way kind (Colour / Normal /
// Data) and is now a nine-way role. A graph saved under the old setting stores
// "kind", and reading that index as a role would quietly turn every Normal into
// a Reflectance — so it is translated instead. Returns an index into the node's
// role list, where 0 is Auto.
int legacyKindToSlot(int kindIndex)
{
    switch (kindIndex) {
    case 1: return int(Upscaler::allSlots().indexOf(Upscaler::MapSlot::BaseColor)) + 1;
    case 2: return int(Upscaler::allSlots().indexOf(Upscaler::MapSlot::Normal)) + 1;
    case 3: return int(Upscaler::allSlots().indexOf(Upscaler::MapSlot::Other)) + 1;
    default: break;
    }
    return 0; // Auto, which is also what an absent setting means
}

// ---- image helpers ----

QImage toRgba(const QImage& image)
{
    return image.isNull() ? image : image.convertToFormat(QImage::Format_RGBA8888);
}

// Where each authoring convention puts a dielectric and where its metals top
// out, measured over 900 FO76 materials. These four numbers are what align the
// two scales; they describe the game's data, not a preference.
constexpr float kBc1DielectricSrgb = 0.22f;  // 56/255, the authored floor in 83% of BC1 maps
constexpr float kBc1MetalTopSrgb = 0.70f;    // 179/255, the median BC1 map's p99
constexpr float kBc4DielectricSrgb = 0.207f; // 0.035 linear, encoded
constexpr float kBc4MetalTopSrgb = 0.493f;   // 0.208 linear, the median textured BC4 map's p99

QImage grayscaleFromChannel(const QImage& source, int channel)
{
    const QImage rgba = toRgba(source);
    if (rgba.isNull()) return {};
    QImage result(rgba.width(), rgba.height(), QImage::Format_RGBA8888);
    for (int y = 0; y < rgba.height(); ++y) {
        const uchar* in = rgba.constScanLine(y);
        uchar* out = result.scanLine(y);
        for (int x = 0; x < rgba.width(); ++x) {
            const uchar value = in[x * 4 + std::clamp(channel, 0, 3)];
            out[x * 4 + 0] = out[x * 4 + 1] = out[x * 4 + 2] = value;
            out[x * 4 + 3] = 255;
        }
    }
    return result;
}

// Applies fn(x, y, srcPixel) over the larger of the two inputs, sampling the
// smaller nearest-neighbour so mismatched resolutions still compose.
QImage combine(const QImage& a, const QImage& b, const std::function<void(const uchar*, const uchar*, uchar*)>& fn)
{
    const QImage left = toRgba(a);
    const QImage right = toRgba(b);
    if (left.isNull()) return right;
    if (right.isNull()) return left;
    const int width = std::max(left.width(), right.width());
    const int height = std::max(left.height(), right.height());
    QImage result(width, height, QImage::Format_RGBA8888);
    for (int y = 0; y < height; ++y) {
        const uchar* leftLine = left.constScanLine(std::min(y * left.height() / height, left.height() - 1));
        const uchar* rightLine = right.constScanLine(std::min(y * right.height() / height, right.height() - 1));
        uchar* out = result.scanLine(y);
        for (int x = 0; x < width; ++x) {
            const uchar* leftPixel = leftLine + std::min(x * left.width() / width, left.width() - 1) * 4;
            const uchar* rightPixel = rightLine + std::min(x * right.width() / width, right.width() - 1) * 4;
            fn(leftPixel, rightPixel, out + x * 4);
        }
    }
    return result;
}

// The cubemap mean the FO4 metal recovery uses as its hue donor.
bool cubemapTint(const QString& path, const QString& textureRoot, QVector3D* tint)
{
    if (path.trimmed().isEmpty()) return false;
    const QVector<QImage> faces = TextureImage::loadCubemapFaces(path, textureRoot, nullptr);
    if (faces.size() != 6) return false;
    double sums[3] = {0, 0, 0};
    double weight = 0;
    for (const QImage& faceImage : faces) {
        const QImage face = toRgba(faceImage);
        if (face.isNull()) continue;
        for (int y = 0; y < face.height(); ++y) {
            const uchar* line = face.constScanLine(y);
            for (int x = 0; x < face.width(); ++x) {
                for (int channel = 0; channel < 3; ++channel)
                    sums[channel] += Converter::srgbToLinear(line[x * 4 + channel] / 255.0f);
                weight += 1;
            }
        }
    }
    if (weight <= 0) return false;
    *tint = QVector3D(float(sums[0] / weight), float(sums[1] / weight), float(sums[2] / weight));
    return true;
}

const Bgsm::Material* materialInput(const EvalArgs& args, EvalOutput* output)
{
    const auto material = args.inputs.value(QStringLiteral("material")).opaque<Bgsm::Material>(QLatin1String(kMaterialTag));
    if (!material) {
        output->error = QStringLiteral("Material input is missing.");
        return nullptr;
    }
    // The shared_ptr outlives this call: values are held by the evaluator's
    // result table for the whole evaluation.
    return material.get();
}

} // namespace

void registerCategoryColors(Registry& registry)
{
    // Blender's node-editor convention mapped onto our categories, so the
    // muscle memory transfers: inputs red, shaders green, vectors indigo,
    // converters blue-grey, colour yellow-olive.
    //
    // Deliberately desaturated. Blender's defaults are pitched against a
    // lighter theme; at full saturation on this grey, eight header colours on
    // one canvas read as a toy. These are tints, not paint.
    //
    // Source stays off pure red on purpose: a failed node already paints its
    // header red, that is the most important signal on the canvas, and it has
    // to both win and stay distinguishable.
    registry.setCategoryColor(QStringLiteral("Source"), QColor(104, 66, 74));      // input
    registry.setCategoryColor(QStringLiteral("Inference"), QColor(64, 104, 74));   // shader
    registry.setCategoryColor(QStringLiteral("Derive"), QColor(74, 78, 122));      // vector
    registry.setCategoryColor(QStringLiteral("Composite"), QColor(54, 94, 104));   // filter
    registry.setCategoryColor(QStringLiteral("Color"), QColor(110, 102, 56));      // colour
    registry.setCategoryColor(QStringLiteral("Maths"), QColor(70, 84, 106));       // converter
    registry.setCategoryColor(QStringLiteral("Bake"), QColor(80, 78, 86));         // script
    registry.setCategoryColor(QStringLiteral("Output"), QColor(126, 78, 84));      // output
}

void registerAll(Registry& registry)
{
    registerCategoryColors(registry);

    // ---------------- Source ----------------

    {
        // One node per title. Each exposes only the slots its own title uses,
        // so there are no permanently-dead ports, and each refuses the other
        // title's material rather than quietly handing downstream nodes
        // textures that mean something else.
        enum class Profile { Any, Fo4, Fo76 };
        struct SlotPort { const char* id; const char* name; QString Bgsm::Material::* member; };

        static const SlotPort fo4Slots[] = {
            {"diffuse", "Diffuse", &Bgsm::Material::diffuseTexture},
            {"normal", "Normal", &Bgsm::Material::normalTexture},
            {"smoothSpec", "Smooth/Spec (_s)", &Bgsm::Material::smoothSpecTexture},
            {"glow", "Glow (_g)", &Bgsm::Material::glowTexture},
            {"greyscale", "Greyscale", &Bgsm::Material::greyscaleTexture},
            {"envMask", "Env Mask", &Bgsm::Material::envmapMaskTexture},
            {"envMap", "Env Map (cubemap)", &Bgsm::Material::envmapTexture},
            {"innerLayer", "Inner Layer", &Bgsm::Material::innerLayerTexture}};
        static const SlotPort fo76Slots[] = {
            {"diffuse", "Diffuse", &Bgsm::Material::diffuseTexture},
            {"normal", "Normal", &Bgsm::Material::normalTexture},
            {"reflectance", "Reflectance (_r)", &Bgsm::Material::specularTexture},
            {"lighting", "Lighting (_l)", &Bgsm::Material::lightingTexture},
            {"glow", "Glow (_g)", &Bgsm::Material::glowTexture},
            {"greyscale", "Greyscale", &Bgsm::Material::greyscaleTexture},
            {"wrinkles", "Wrinkles", &Bgsm::Material::wrinklesTexture},
            {"flow", "Flow", &Bgsm::Material::flowTexture}};
        static const SlotPort allSlots[] = {
            {"diffuse", "Diffuse", &Bgsm::Material::diffuseTexture},
            {"normal", "Normal", &Bgsm::Material::normalTexture},
            {"smoothSpec", "Smooth/Spec (_s)", &Bgsm::Material::smoothSpecTexture},
            {"reflectance", "Reflectance (_r)", &Bgsm::Material::specularTexture},
            {"lighting", "Lighting (_l)", &Bgsm::Material::lightingTexture},
            {"glow", "Glow (_g)", &Bgsm::Material::glowTexture},
            {"envMask", "Env Mask", &Bgsm::Material::envmapMaskTexture},
            {"envMap", "Env Map (cubemap)", &Bgsm::Material::envmapTexture},
            {"greyscale", "Greyscale", &Bgsm::Material::greyscaleTexture},
            {"wrinkles", "Wrinkles", &Bgsm::Material::wrinklesTexture},
            {"flow", "Flow", &Bgsm::Material::flowTexture},
            {"innerLayer", "Inner Layer", &Bgsm::Material::innerLayerTexture}};

        const auto addSourceMaterial = [&registry](const char* id, const char* name, const char* summary,
                                           Profile profile, const SlotPort* slotPorts, int slotCount) {
            NodeTypeSpec spec;
            spec.id = QLatin1String(id);
            spec.name = QLatin1String(name);
            spec.category = QStringLiteral("Source");
            spec.summary = QLatin1String(summary);
            // A whole material, not one map: no body thumbnail and no channel
            // strip. Selecting it shades the untouched original in the
            // viewport, exactly as the terminal shades the converted result.
            spec.materialPreview = QStringLiteral("source");
            spec.outputs = {materialPort("material", "Material"),
                PortSpec{QStringLiteral("profile"), QStringLiteral("Profile"), ValueType::String, {}, false}};
            // Slots emit loaded images, not paths. This mirrors the other end of
            // the graph: Output Material (PBR) knows the filenames it writes and receives
            // images, so the source knows the filenames it reads and emits
            // images. Both ends own their file names; the middle only ever sees
            // pixels, and the unpack node can wire straight in.
            for (int i = 0; i < slotCount; ++i)
                spec.outputs.append(imagePort(slotPorts[i].id, slotPorts[i].name));
            spec.outputs.append(colorPort("emitColor", "Emittance Color"));
            spec.outputs.append(colorPort("specColor", "Specular Color"));
            spec.outputs.append(scalarPort("emitMult", "Emittance Mult"));
            spec.outputs.append(scalarPort("specMult", "Specular Mult"));
            spec.outputs.append(scalarPort("smoothness", "Smoothness"));
            // Subsurface: FO76's translucency block, FO4's rolloff.
            spec.outputs.append(colorPort("sssColor", "Subsurface Color"));
            spec.outputs.append(scalarPort("transmission", "Transmission"));
            spec.params = {ParamSpec{QStringLiteral("path"), QStringLiteral("Override path"),
                ParamSpec::Kind::Path, 0, 0, 0, QString(), {},
                QStringLiteral("Leave empty to convert whatever the queue selected.")}};
            spec.evaluate = [profile, slotPorts, slotCount](const EvalArgs& args, EvalOutput* output) {
                const QString override = paramString(args.params, "path").trimmed();
                const QString path = override.isEmpty() ? args.context.sourcePath : override;
                if (path.isEmpty()) { output->error = QStringLiteral("No source material."); return; }
                auto material = std::make_shared<Bgsm::Material>();
                QString error;
                if (!Bgsm::readFile(path, material.get(), &error)) {
                    output->error = error.isEmpty() ? QStringLiteral("Could not read the material.") : error;
                    return;
                }
                const bool fo76 = material->isFallout76();
                if ((profile == Profile::Fo4 && fo76) || (profile == Profile::Fo76 && !fo76)) {
                    output->error = QStringLiteral("This is a Fallout %1 material (v%2) — use Input Material (Fallout %1).")
                                        .arg(fo76 ? QStringLiteral("76") : QStringLiteral("4"))
                                        .arg(material->version);
                    return;
                }
                output->outputs.insert(QStringLiteral("material"),
                    Value::fromOpaque(QLatin1String(kMaterialTag), std::static_pointer_cast<void>(material)));
                output->outputs.insert(QStringLiteral("profile"),
                    Value::fromString(fo76 ? QStringLiteral("FO76") : QStringLiteral("FO4")));

                // Emittance off means the game ignores the emissive entirely, so
                // neither the colour nor the glow map leaves this node.
                //
                // It takes BOTH flags to decide that, not just emitEnabled: a
                // material can ship emit=0 with glowmap=1 and still light in
                // game (bloodbugglow.bgsm is exactly that). This is the same
                // test convertercore applies, deliberately — when the graph and
                // the orchestrator disagree about a rule, the bug shows up in
                // one path and gets fixed in the other.
                // BGEM is separate: an effect material carries its colour in
                // baseColor and emits regardless.
                const bool emits = material->emitEnabled || material->glowmap
                    || material->kind == Bgsm::Kind::Bgem;

                int authored = 0, largest = 0;
                QStringList missing;
                for (int i = 0; i < slotCount; ++i) {
                    const QString value = (material.get()->*(slotPorts[i].member)).trimmed();
                    const bool suppressed = !emits
                        && QLatin1String(slotPorts[i].id) == QLatin1String("glow");
                    if (suppressed) {
                        output->outputs.insert(QLatin1String(slotPorts[i].id), Value::fromImage(QImage()));
                        if (!value.isEmpty())
                            output->notes.append(QStringLiteral("glow ignored: emittance is off"));
                        continue;
                    }
                    if (value.isEmpty()) {
                        // An unauthored slot is normal, not an error: downstream
                        // treats a null image as absent, like the orchestrator.
                        output->outputs.insert(QLatin1String(slotPorts[i].id), Value::fromImage(QImage()));
                        continue;
                    }
                    ++authored;
                    QImage image = TextureImage::load(value, args.context.textureRoot, nullptr);
                    if (image.isNull()) missing.append(value);
                    else largest = std::max(largest, std::max(image.width(), image.height()));
                    // _r is authored under two conventions and only the DDS
                    // format tells them apart, so the same stored number means
                    // two different reflectances. Put both on one scale here,
                    // where the file name is still known — see
                    // normaliseReflectanceToSrgbScale for the numbers and why.
                    if (!image.isNull() && QLatin1String(slotPorts[i].id) == QLatin1String("reflectance")) {
                        const TextureImage::Diagnostics info =
                            TextureImage::inspect(value, args.context.textureRoot);
                        image = normaliseReflectanceToSrgbScale(image,
                            info.colorSpace.contains(QLatin1String("sRGB"), Qt::CaseInsensitive));
                    }
                    // Preview evaluation works small so the graph keeps up with
                    // a slider; conversion runs at full resolution.
                    const int cap = args.context.previewMaxSize;
                    if (args.context.previewMode && cap > 0 && !image.isNull()) {
                        const int full = std::max(image.width(), image.height());
                        if (full > cap)
                            image = image.scaled(image.width() * cap / full, image.height() * cap / full,
                                Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                    }
                    output->outputs.insert(QLatin1String(slotPorts[i].id), Value::fromImage(image));
                }
                if (largest > 0) output->nativeResolution = largest;
                for (const QString& absent : missing)
                    output->notes.append(QStringLiteral("missing: %1").arg(absent));
                // BGEM keeps its color in baseColor. The port is left unset when
                // the material does not emit, so nothing downstream can build an
                // emissive map out of a colour the game never uses.
                if (emits)
                    output->outputs.insert(QStringLiteral("emitColor"),
                        Value::fromColor(material->kind == Bgsm::Kind::Bgem && !material->emitEnabled
                            ? material->baseColor : material->emittanceColor));
                output->outputs.insert(QStringLiteral("specColor"), Value::fromColor(material->specularColor));
                output->outputs.insert(QStringLiteral("emitMult"),
                    Value::fromScalar(material->emitEnabled ? material->emittanceMult
                                                            : (material->kind == Bgsm::Kind::Bgem ? material->baseColorScale : 0.0f)));
                output->outputs.insert(QStringLiteral("specMult"), Value::fromScalar(material->specularMult));
                output->outputs.insert(QStringLiteral("smoothness"), Value::fromScalar(material->smoothness));
                output->outputs.insert(QStringLiteral("sssColor"),
                    Value::fromColor(material->translucencySubsurfaceColor));
                // Reports what the conversion will actually use, so the port
                // and the written .pbrm never disagree.
                output->outputs.insert(QStringLiteral("transmission"),
                    Value::fromScalar(material->translucency
                            ? std::clamp(material->translucencyTransmissiveScale, 0.0f, 4.0f)
                            : 0.0f));
                // Reflective in game, dielectric after conversion. Worth saying
                // once per material, because it is the single biggest visible
                // difference between the vanilla look and the converted one,
                // and the correction mask is how you get the metal back.
                if (!fo76 && material->environmentMapping)
                    output->notes.append(QStringLiteral(
                        "reflective in game (%1) — converts dielectric")
                            .arg(material->envmapTexture.trimmed().isEmpty()
                                     ? QStringLiteral("no cubemap")
                                     : QFileInfo(material->envmapTexture).fileName()));

                output->status = QStringLiteral("%1 v%2 · %3 texture%4")
                    .arg(fo76 ? QStringLiteral("FO76") : QStringLiteral("FO4"))
                    .arg(material->version).arg(authored).arg(authored == 1 ? QString() : QStringLiteral("s"));
                output->notes.append(QStringLiteral("%1 v%2 — %3")
                    .arg(fo76 ? QStringLiteral("FO76") : QStringLiteral("FO4"))
                    .arg(material->version).arg(QFileInfo(path).fileName()));
            };
            registry.add(spec);
        };

        addSourceMaterial("source.material.fo4", "Input Material (Fallout 4)",
            "Parses the Fallout 4 .bgsm/.bgem being converted and exposes its slots.",
            Profile::Fo4, fo4Slots, int(std::size(fo4Slots)));
        addSourceMaterial("source.material.fo76", "Input Material (Fallout 76)",
            "Parses the Fallout 76 .bgsm/.bgem being converted and exposes its slots.",
            Profile::Fo76, fo76Slots, int(std::size(fo76Slots)));
        // Kept so graphs saved before the split still load.
        addSourceMaterial("source.material", "Source Material (Any Profile)",
            "Parses whichever .bgsm/.bgem is being converted, either title.",
            Profile::Any, allSlots, int(std::size(allSlots)));
    }

    {
        static const QStringList slotLabels{
            QStringLiteral("Diffuse"), QStringLiteral("Normal"), QStringLiteral("Smooth/Spec (FO4 _s)"),
            QStringLiteral("Reflectance (FO76 _r)"), QStringLiteral("Lighting (FO76 _l)"),
            QStringLiteral("Glow"), QStringLiteral("Greyscale"), QStringLiteral("Env Mask"),
            QStringLiteral("Wrinkles"), QStringLiteral("Flow"), QStringLiteral("Inner Layer")};
        static const QStringList resolutionModes{QStringLiteral("Material max"),
            QStringLiteral("Native"), QStringLiteral("Custom")};
        NodeTypeSpec spec;
        spec.id = QStringLiteral("source.texture");
        spec.name = QStringLiteral("Texture");
        spec.category = QStringLiteral("Source");
        spec.summary = QStringLiteral("Loads a texture: either one of the material's slots, or whatever "
                                      "path is wired into it.");
        // Both inputs are optional: wire a path (from Source Material or any
        // string) to override the slot, or leave it free and pick a slot.
        spec.inputs = {materialPort("material", "Material", true),
            PortSpec{QStringLiteral("path"), QStringLiteral("Path"), ValueType::String, {}, true}};
        spec.outputs = {imagePort("image", "Image"),
            PortSpec{QStringLiteral("path"), QStringLiteral("Path"), ValueType::String, {}, false}};
        spec.params = {enumParam("slot", "Slot", 0, slotLabels),
            enumParam("resolution", "Resolution", 0, resolutionModes,
                "Material max levels every map in the material to its largest texture."),
            intParam("customSize", "Custom size", 2048, 16, 8192)};
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            // A wired path wins — including an EMPTY one, which means the
            // material simply does not author that slot. Falling back to the
            // material input there turned every unauthored slot into an error.
            const bool pathWired = args.inputs.contains(QStringLiteral("path"));
            QString authored = args.inputs.value(QStringLiteral("path")).string().trimmed();
            if (!pathWired) {
                const Bgsm::Material* material = materialInput(args, output);
                if (!material) return;
                switch (paramInt(args.params, "slot", 0)) {
                case 0: authored = material->diffuseTexture; break;
                case 1: authored = material->normalTexture; break;
                case 2: authored = material->smoothSpecTexture; break;
                case 3: authored = material->specularTexture; break;
                case 4: authored = material->lightingTexture; break;
                case 5: authored = material->glowTexture; break;
                case 6: authored = material->greyscaleTexture; break;
                case 7: authored = material->envmapMaskTexture; break;
                case 8: authored = material->wrinklesTexture; break;
                case 9: authored = material->flowTexture; break;
                case 10: authored = material->innerLayerTexture; break;
                default: break;
                }
            }
            output->outputs.insert(QStringLiteral("path"), Value::fromString(authored));
            if (authored.trimmed().isEmpty()) {
                // Unauthored slots are normal, not errors: downstream nodes
                // treat a null image as "absent" exactly like the orchestrator.
                output->outputs.insert(QStringLiteral("image"), Value::fromImage(QImage()));
                output->status = QStringLiteral("slot not authored");
                output->notes.append(QStringLiteral("slot not authored"));
                return;
            }
            QString error;
            QImage image = TextureImage::load(authored, args.context.textureRoot, &error);
            if (image.isNull()) {
                output->outputs.insert(QStringLiteral("image"), Value::fromImage(image));
                output->status = QStringLiteral("missing on disk");
                output->notes.append(QStringLiteral("missing: %1").arg(authored));
                return;
            }

            // Resolution and compression come from the DDS header, not the
            // decoded QImage, so the node reports what actually ships — and
            // reports it at full size even while the preview runs downsampled.
            const TextureImage::Diagnostics info = TextureImage::inspect(authored, args.context.textureRoot);
            const int nativeW = info.valid && info.width > 0 ? info.width : image.width();
            const int nativeH = info.valid && info.height > 0 ? info.height : image.height();
            const int native = std::max(nativeW, nativeH);

            // Levelling: by default every map in a material works at the
            // material's largest resolution, so mismatched slots stop silently
            // limiting whatever they are packed with.
            int target = native;
            switch (paramInt(args.params, "resolution", 0)) {
            case 0: if (args.context.materialMaxResolution > 0) target = args.context.materialMaxResolution; break;
            case 2: target = paramInt(args.params, "customSize", 2048); break;
            default: break; // Native
            }
            if (target <= 0) target = native;
            output->nativeResolution = target;

            const QString compression = info.compressionLabel();
            output->status = compression.isEmpty()
                ? QStringLiteral("%1×%2").arg(nativeW).arg(nativeH)
                : QStringLiteral("%1×%2 · %3").arg(nativeW).arg(nativeH).arg(compression);
            if (target != native) output->status += QStringLiteral("  →%1").arg(target);

            // Work at the target ratio, then let preview mode cap the pixels.
            if (target != native && native > 0) {
                const double factor = double(target) / double(native);
                image = image.scaled(std::max(1, int(std::lround(image.width() * factor))),
                    std::max(1, int(std::lround(image.height() * factor))),
                    Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            }
            const int full = std::max(image.width(), image.height());
            const int cap = args.context.previewMaxSize;
            if (args.context.previewMode && cap > 0 && full > cap) {
                image = image.scaled(image.width() * cap / full, image.height() * cap / full,
                    Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                output->notes.append(QStringLiteral("%1×%2 (preview of %3px)")
                    .arg(image.width()).arg(image.height()).arg(full));
            } else {
                output->notes.append(QStringLiteral("%1×%2").arg(image.width()).arg(image.height()));
            }
            output->outputs.insert(QStringLiteral("image"), Value::fromImage(image));
        };
        registry.add(spec);
    }

    {
        NodeTypeSpec spec;
        spec.id = QStringLiteral("source.cubemapTint");
        spec.name = QStringLiteral("Cubemap Tint");
        spec.category = QStringLiteral("Source");
        spec.summary = QStringLiteral("Mean colour of the referenced _e cubemap — the hue donor for fully black metals.");
        spec.inputs = {materialPort("material", "Material")};
        spec.outputs = {colorPort("tint", "Tint"), scalarPort("valid", "Valid")};
        spec.params = {floatParam("saturationClamp", "Saturation clamp", 0.35, 0.0, 1.0, 0.01,
            "Scene-neutral cubemaps must not tint metals.")};
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            const Bgsm::Material* material = materialInput(args, output);
            if (!material) return;
            QVector3D tint(0.5f, 0.5f, 0.5f);
            const bool valid = cubemapTint(material->envmapTexture, args.context.textureRoot, &tint);
            if (valid) tint = Converter::clampTintSaturation(tint, paramFloat(args.params, "saturationClamp", 0.35f));
            output->outputs.insert(QStringLiteral("tint"), Value::fromColor(tint));
            output->outputs.insert(QStringLiteral("valid"), Value::fromScalar(valid ? 1.0f : 0.0f));
            output->notes.append(valid
                ? QStringLiteral("tint %1 %2 %3").arg(tint.x(), 0, 'f', 3).arg(tint.y(), 0, 'f', 3).arg(tint.z(), 0, 'f', 3)
                : QStringLiteral("no cubemap"));
        };
        registry.add(spec);
    }

    {
        NodeTypeSpec spec;
        spec.id = QStringLiteral("source.image");
        spec.name = QStringLiteral("Image File");
        spec.category = QStringLiteral("Source");
        spec.summary = QStringLiteral("Loads an arbitrary image — correction masks, custom inputs.");
        spec.outputs = {imagePort("image", "Image")};
        spec.params = {ParamSpec{QStringLiteral("path"), QStringLiteral("Path"), ParamSpec::Kind::Path,
            0, 0, 0, QString(), {}, QStringLiteral("Absolute, or relative to the texture root.")}};
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            const QString path = paramString(args.params, "path").trimmed();
            if (path.isEmpty()) { output->outputs.insert(QStringLiteral("image"), Value::fromImage(QImage())); return; }
            QImage image(path);
            if (image.isNull()) image = TextureImage::load(path, args.context.textureRoot, nullptr);
            output->outputs.insert(QStringLiteral("image"), Value::fromImage(image));
            if (image.isNull()) output->notes.append(QStringLiteral("missing: %1").arg(path));
        };
        registry.add(spec);
    }

    // ---------------- Inference ----------------
    // No whole-conversion nodes live here any more. `convert.fo4` and
    // `convert.fo76` used to do an entire title's conversion behind one header,
    // and both stock graphs have since been written out as explicit maths --
    // Separate Maps, Levels, Invert, Mix, colour-space pairs. A registered node
    // that no shipped graph uses is a second, divergent definition of the same
    // conversion waiting to drift from the one that runs, so they are retired.
    // The recipes live in the graph PRESETS instead (defaultFo4Graph,
    // defaultFo76Graph, and anything saved beside them), where every threshold
    // is on the canvas and tunable per material.
    //
    // Converter::convertFo4 / convertFo76 remain as the batch path in
    // convertercore -- that is the same maths, not a duplicate of it, and the
    // graphs are checked against it on real materials.

    // ---------------- Unpack ----------------
    // The mirror of Combine Maps (PBR): these are the only nodes that know how
    // the SOURCE titles pack their textures. Everything downstream then works
    // on plain single maps with honest names, instead of every graph having to
    // remember that FO76 smoothness lives in _l.R and FO4 gloss in _s.G.
    // Per title, like the source nodes, so neither carries dead ports.
    {
        // Pulls one channel out as a grayscale map. Alpha gets a guard: vanilla
        // BC1 maps carry a constant 1.0 alpha that means "no data", and reading
        // it as a mask would light or scatter across an entire surface.
        const auto splitChannel = [](const QImage& source, int channel, bool guardConstant) {
            const QImage rgba = toRgba(source);
            if (rgba.isNull()) return QImage();
            if (guardConstant) {
                uchar low = 255, high = 0;
                for (int y = 0; y < rgba.height(); ++y) {
                    const uchar* line = rgba.constScanLine(y);
                    for (int x = 0; x < rgba.width(); ++x) {
                        const uchar value = line[x * 4 + channel];
                        low = std::min(low, value); high = std::max(high, value);
                    }
                }
                if (low == high) return QImage(); // constant: carries nothing
            }
            QImage out(rgba.width(), rgba.height(), QImage::Format_RGBA8888);
            for (int y = 0; y < rgba.height(); ++y) {
                const uchar* line = rgba.constScanLine(y);
                uchar* dst = out.scanLine(y);
                for (int x = 0; x < rgba.width(); ++x) {
                    const uchar value = line[x * 4 + channel];
                    dst[x * 4] = dst[x * 4 + 1] = dst[x * 4 + 2] = value;
                    dst[x * 4 + 3] = 255;
                }
            }
            return out;
        };
        const auto rgbOnly = [](const QImage& source) {
            QImage rgba = toRgba(source);
            if (rgba.isNull()) return QImage();
            for (int y = 0; y < rgba.height(); ++y) {
                uchar* line = rgba.scanLine(y);
                for (int x = 0; x < rgba.width(); ++x) line[x * 4 + 3] = 255;
            }
            return rgba;
        };

        {
            NodeTypeSpec spec;
            spec.id = QStringLiteral("unpack.fo4");
            spec.name = QStringLiteral("Separate Maps (Fallout 4)");
            spec.category = QStringLiteral("Source");
            spec.summary = QStringLiteral("Splits Fallout 4 source textures into single maps: "
                                          "_s.R is specular strength, _s.G is gloss.");
            // No env-mask port: BGSM v2 has no env-mask texture slot, so it was
            // an input that could only ever be empty.
            spec.inputs = {imagePort("diffuse", "Diffuse (_d)"), imagePort("specular", "Specular (_s)", true),
                imagePort("normal", "Normal (_n)", true), imagePort("glow", "Glow (_g)", true)};
            spec.outputs = {imagePort("diffuse", "Diffuse"), imagePort("opacity", "Opacity"),
                imagePort("specularStrength", "Specular Strength"), imagePort("gloss", "Gloss"),
                imagePort("normal", "Normal"), imagePort("glow", "Glow")};
            // Everything this node emits is one material taken apart, so the
            // preview offers the untouched original plus each map on its own.
            spec.materialPreview = QStringLiteral("source");
            spec.evaluate = [splitChannel, rgbOnly](const EvalArgs& args, EvalOutput* output) {
                const QImage diffuse = args.inputs.value(QStringLiteral("diffuse")).image();
                if (diffuse.isNull()) { output->error = QStringLiteral("Diffuse is required."); return; }
                const QImage specular = args.inputs.value(QStringLiteral("specular")).image();
                output->outputs.insert(QStringLiteral("diffuse"), Value::fromImage(rgbOnly(diffuse)));
                output->outputs.insert(QStringLiteral("opacity"), Value::fromImage(splitChannel(diffuse, 3, true)));
                output->outputs.insert(QStringLiteral("specularStrength"), Value::fromImage(splitChannel(specular, 0, false)));
                output->outputs.insert(QStringLiteral("gloss"), Value::fromImage(splitChannel(specular, 1, false)));
                output->outputs.insert(QStringLiteral("normal"),
                    Value::fromImage(args.inputs.value(QStringLiteral("normal")).image()));
                output->outputs.insert(QStringLiteral("glow"),
                    Value::fromImage(args.inputs.value(QStringLiteral("glow")).image()));
            };
            registry.add(spec);
        }

        {
            NodeTypeSpec spec;
            spec.id = QStringLiteral("unpack.fo76");
            spec.name = QStringLiteral("Separate Maps (Fallout 76)");
            spec.category = QStringLiteral("Source");
            spec.summary = QStringLiteral("Splits Fallout 76 source textures into single maps: "
                                          "_l is R smoothness, G baked AO, B subsurface, A glow mask.");
            spec.inputs = {imagePort("diffuse", "Diffuse (_d)"), imagePort("reflectance", "Reflectance (_r)", true),
                imagePort("normal", "Normal (_n)", true), imagePort("lighting", "Lighting (_l)", true),
                imagePort("glow", "Glow (_g)", true)};
            spec.outputs = {imagePort("diffuse", "Diffuse"), imagePort("opacity", "Opacity"),
                imagePort("reflectance", "Reflectance"), imagePort("normal", "Normal"),
                imagePort("smoothness", "Smoothness"), imagePort("ao", "Ambient Occlusion"),
                imagePort("subsurface", "Subsurface"), imagePort("glowMask", "Glow Mask"),
                imagePort("glow", "Glow")};
            spec.materialPreview = QStringLiteral("source");
            spec.evaluate = [splitChannel, rgbOnly](const EvalArgs& args, EvalOutput* output) {
                const QImage diffuse = args.inputs.value(QStringLiteral("diffuse")).image();
                if (diffuse.isNull()) { output->error = QStringLiteral("Diffuse is required."); return; }
                const QImage lighting = args.inputs.value(QStringLiteral("lighting")).image();
                output->outputs.insert(QStringLiteral("diffuse"), Value::fromImage(rgbOnly(diffuse)));
                output->outputs.insert(QStringLiteral("opacity"), Value::fromImage(splitChannel(diffuse, 3, true)));
                output->outputs.insert(QStringLiteral("reflectance"),
                    Value::fromImage(rgbOnly(args.inputs.value(QStringLiteral("reflectance")).image())));
                output->outputs.insert(QStringLiteral("normal"),
                    Value::fromImage(args.inputs.value(QStringLiteral("normal")).image()));
                output->outputs.insert(QStringLiteral("smoothness"), Value::fromImage(splitChannel(lighting, 0, false)));
                output->outputs.insert(QStringLiteral("ao"), Value::fromImage(splitChannel(lighting, 1, false)));
                // B and A are masks that are simply absent on most materials, so
                // both are guarded: a constant channel returns nothing rather
                // than a flat mask that would scatter or glow everywhere.
                output->outputs.insert(QStringLiteral("subsurface"), Value::fromImage(splitChannel(lighting, 2, true)));
                output->outputs.insert(QStringLiteral("glowMask"), Value::fromImage(splitChannel(lighting, 3, true)));
                output->outputs.insert(QStringLiteral("glow"),
                    Value::fromImage(args.inputs.value(QStringLiteral("glow")).image()));
            };
            registry.add(spec);
        }
    }

    // ---------------- Derive ----------------

    {
        NodeTypeSpec spec;
        spec.id = QStringLiteral("derive.normal");
        spec.name = QStringLiteral("Curvature Generator");
        spec.category = QStringLiteral("Derive");
        spec.summary = QStringLiteral("One Poisson solve feeding height, detail AO, and signed curvature.");
        spec.inputs = {imagePort("normal", "Normal")};
        spec.outputs = {imagePort("curvature", "Curvature"), imagePort("height", "Height"),
            imagePort("ao", "Detail AO"), scalarPort("noiseMetric", "Noise Metric")};
        spec.params = {
            intParam("derivationSize", "Derivation size", 512, 64, 2048, "Height/AO solve resolution cap."),
            floatParam("curvatureScale", "Curvature gain", 2.0, 0.25, 8.0, 0.05),
            floatParam("heightScale", "Height scale", 12.0, 1.0, 64.0, 0.5),
            intParam("aoDirections", "AO directions", 8, 4, 64),
            intParam("aoRadius", "AO radius", 24, 4, 128),
            floatParam("aoStrength", "AO strength", 1.0, 0.0, 4.0, 0.05),
            boolParam("autoDenoise", "Auto denoise", true,
                "The Laplacian amplifies exactly the frequencies compression noise lives in."),
            floatParam("denoiseThreshold", "Denoise threshold", 0.035, 0.0, 0.3, 0.005),
        };
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            const QImage normal = args.inputs.value(QStringLiteral("normal")).image();
            if (normal.isNull()) { output->error = QStringLiteral("Normal map is required."); return; }
            NormalDerive::Options options;
            options.derivationSize = paramInt(args.params, "derivationSize", options.derivationSize);
            options.curvatureScale = paramFloat(args.params, "curvatureScale", options.curvatureScale);
            options.heightScale = paramFloat(args.params, "heightScale", options.heightScale);
            options.aoDirections = paramInt(args.params, "aoDirections", options.aoDirections);
            options.aoRadius = paramInt(args.params, "aoRadius", options.aoRadius);
            options.aoStrength = paramFloat(args.params, "aoStrength", options.aoStrength);
            options.autoDenoise = paramBool(args.params, "autoDenoise", options.autoDenoise);
            options.denoiseThreshold = paramFloat(args.params, "denoiseThreshold", options.denoiseThreshold);
            const NormalDerive::Results results = NormalDerive::derive(normal, options);
            output->outputs.insert(QStringLiteral("curvature"), Value::fromImage(results.curvature));
            output->outputs.insert(QStringLiteral("height"), Value::fromImage(results.height));
            output->outputs.insert(QStringLiteral("ao"), Value::fromImage(results.ao));
            output->outputs.insert(QStringLiteral("noiseMetric"), Value::fromScalar(results.noiseMetric));
            output->notes.append(QStringLiteral("noise %1%2").arg(results.noiseMetric, 0, 'f', 3)
                .arg(results.denoised ? QStringLiteral(" — denoised") : QString()));
        };
        registry.add(spec);
    }

    {
        NodeTypeSpec spec;
        spec.id = QStringLiteral("derive.denoise");
        spec.name = QStringLiteral("Denoise Normal");
        spec.category = QStringLiteral("Derive");
        spec.summary = QStringLiteral("Median + bilateral cleanup of the XY field (derivation-side only).");
        spec.inputs = {imagePort("normal", "Normal")};
        spec.outputs = {imagePort("normal", "Normal"), scalarPort("noiseMetric", "Noise Metric")};
        spec.params = {floatParam("denoiseThreshold", "Threshold", 0.035, 0.0, 0.3, 0.005),
            floatParam("bilateralRangeSigma", "Range sigma", 0.10, 0.01, 1.0, 0.01),
            boolParam("forceMedian", "Force median", false)};
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            const QImage normal = args.inputs.value(QStringLiteral("normal")).image();
            if (normal.isNull()) { output->error = QStringLiteral("Normal map is required."); return; }
            NormalDerive::Options options;
            options.denoiseThreshold = paramFloat(args.params, "denoiseThreshold", options.denoiseThreshold);
            options.bilateralRangeSigma = paramFloat(args.params, "bilateralRangeSigma", options.bilateralRangeSigma);
            options.forceMedian = paramBool(args.params, "forceMedian", options.forceMedian);
            output->outputs.insert(QStringLiteral("normal"), Value::fromImage(NormalDerive::denoiseNormal(normal, options)));
            output->outputs.insert(QStringLiteral("noiseMetric"), Value::fromScalar(NormalDerive::noiseMetric(normal)));
        };
        registry.add(spec);
    }

    // ---------------- Bake ----------------

    {
        NodeTypeSpec spec;
        spec.id = QStringLiteral("bake.geometryAo");
        spec.name = QStringLiteral("Geometry AO");
        spec.category = QStringLiteral("Bake");
        spec.summary = QStringLiteral("Bakes ambient occlusion from the reference mesh in UV space.");
        spec.outputs = {imagePort("ao", "Geometry AO")};
        spec.params = {
            ParamSpec{QStringLiteral("mesh"), QStringLiteral("Mesh override"), ParamSpec::Kind::Path,
                0, 0, 0, QString(), {}, QStringLiteral("Leave empty to use the workspace's reference NIF.")},
            intParam("mapSize", "Map size", 512, 64, 2048),
            intParam("directions", "Directions", 64, 8, 256),
            floatParam("strength", "Strength", 1.0, 0.0, 4.0, 0.05),
            boolParam("canonicalIslands", "Canonical islands", true,
                "Stacked duplicate UV islands bake once, from their brightest instance."),
            intParam("dilate", "Dilate", 4, 0, 16),
        };
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            const QString override = paramString(args.params, "mesh").trimmed();
            const QString path = override.isEmpty() ? args.context.referenceMesh : override;
            if (path.isEmpty()) { output->error = QStringLiteral("No reference mesh."); return; }
            QString error;
            const PreviewMesh mesh = path.endsWith(QStringLiteral(".nif"), Qt::CaseInsensitive)
                ? PreviewMeshes::loadStaticNif(path, &error)
                : PreviewMeshes::loadStaticFbx(path, &error);
            if (!mesh.isValid()) {
                output->error = error.isEmpty() ? QStringLiteral("Could not load the mesh.") : error;
                return;
            }
            GeometryAO::Options options;
            options.mapSize = paramInt(args.params, "mapSize", options.mapSize);
            options.directions = paramInt(args.params, "directions", options.directions);
            options.strength = paramFloat(args.params, "strength", options.strength);
            options.canonicalIslands = paramBool(args.params, "canonicalIslands", options.canonicalIslands);
            options.dilate = paramInt(args.params, "dilate", options.dilate);
            QStringList notes;
            output->outputs.insert(QStringLiteral("ao"), Value::fromImage(GeometryAO::bake(mesh, options, &notes)));
            output->notes = notes;
        };
        registry.add(spec);
    }

    // ---------------- Composite ----------------

    {
        NodeTypeSpec spec;
        spec.id = QStringLiteral("image.channelPack");
        spec.name = QStringLiteral("Combine Channels");
        spec.category = QStringLiteral("Composite");
        spec.summary = QStringLiteral("Assembles four grayscale inputs into one RGBA texture.");
        spec.inputs = {imagePort("r", "R", true), imagePort("g", "G", true),
            imagePort("b", "B", true), imagePort("a", "A", true)};
        spec.outputs = {imagePort("image", "Image")};
        spec.params = {floatParam("defaultR", "Default R", 0.0, 0.0, 1.0),
            floatParam("defaultG", "Default G", 0.0, 0.0, 1.0),
            floatParam("defaultB", "Default B", 1.0, 0.0, 1.0),
            floatParam("defaultA", "Default A", 1.0, 0.0, 1.0)};
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            const char* const ids[4] = {"r", "g", "b", "a"};
            const char* const defaults[4] = {"defaultR", "defaultG", "defaultB", "defaultA"};
            QImage planes[4];
            int width = 0, height = 0;
            for (int i = 0; i < 4; ++i) {
                planes[i] = toRgba(args.inputs.value(QLatin1String(ids[i])).image());
                width = std::max(width, planes[i].width());
                height = std::max(height, planes[i].height());
            }
            if (width <= 0 || height <= 0) { output->error = QStringLiteral("No connected channels."); return; }
            QImage result(width, height, QImage::Format_RGBA8888);
            for (int y = 0; y < height; ++y) {
                uchar* out = result.scanLine(y);
                for (int i = 0; i < 4; ++i) {
                    const QImage& plane = planes[i];
                    const uchar fallback = uchar(std::lround(std::clamp(
                        paramFloat(args.params, defaults[i], i == 2 || i == 3 ? 1.0f : 0.0f), 0.0f, 1.0f) * 255.0f));
                    if (plane.isNull()) {
                        for (int x = 0; x < width; ++x) out[x * 4 + i] = fallback;
                        continue;
                    }
                    const uchar* line = plane.constScanLine(std::min(y * plane.height() / height, plane.height() - 1));
                    for (int x = 0; x < width; ++x)
                        out[x * 4 + i] = line[std::min(x * plane.width() / width, plane.width() - 1) * 4];
                }
            }
            output->outputs.insert(QStringLiteral("image"), Value::fromImage(result));
        };
        registry.add(spec);
    }

    {
        NodeTypeSpec spec;
        spec.id = QStringLiteral("image.constant");
        spec.name = QStringLiteral("Uniform Color");
        spec.category = QStringLiteral("Composite");
        spec.summary = QStringLiteral("A flat colour plate — constant inputs and quick tests.");
        spec.outputs = {imagePort("image", "Image")};
        spec.params = {intParam("size", "Size", 256, 1, 4096),
            floatParam("r", "R", 0.5, 0.0, 1.0), floatParam("g", "G", 0.5, 0.0, 1.0),
            floatParam("b", "B", 0.5, 0.0, 1.0), floatParam("a", "A", 1.0, 0.0, 1.0)};
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            const int size = std::clamp(paramInt(args.params, "size", 256), 1, 4096);
            const auto channel = [&args](const char* id, float fallback) {
                return uchar(std::lround(std::clamp(paramFloat(args.params, id, fallback), 0.0f, 1.0f) * 255.0f));
            };
            QImage image(size, size, QImage::Format_RGBA8888);
            image.fill(QColor(channel("r", 0.5f), channel("g", 0.5f), channel("b", 0.5f), channel("a", 1.0f)));
            output->outputs.insert(QStringLiteral("image"), Value::fromImage(image));
        };
        registry.add(spec);
    }

    {
        static const QStringList channelLabels{QStringLiteral("R"), QStringLiteral("G"),
            QStringLiteral("B"), QStringLiteral("A")};
        NodeTypeSpec spec;
        spec.id = QStringLiteral("image.channel");
        spec.name = QStringLiteral("Extract Channel");
        spec.category = QStringLiteral("Composite");
        spec.summary = QStringLiteral("Pulls one channel out as grayscale.");
        spec.inputs = {imagePort("image", "Image")};
        spec.outputs = {imagePort("image", "Image")};
        spec.params = {enumParam("channel", "Channel", 0, channelLabels)};
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            const QImage source = args.inputs.value(QStringLiteral("image")).image();
            if (source.isNull()) { output->error = QStringLiteral("Image is required."); return; }
            output->outputs.insert(QStringLiteral("image"),
                Value::fromImage(grayscaleFromChannel(source, paramInt(args.params, "channel", 0))));
        };
        registry.add(spec);
    }

    // ---------------- Color space ----------------
    // Exact piecewise sRGB, shared with the conversion math — a 2.2 gamma
    // approximation would disagree with everything else in the pipeline.
    {
        const auto colorSpaceNode = [&registry](const char* id, const char* name, const char* summary,
                                        float (*transfer)(float)) {
            NodeTypeSpec spec;
            spec.id = QLatin1String(id);
            spec.name = QLatin1String(name);
            spec.category = QStringLiteral("Color");
            spec.summary = QLatin1String(summary);
            spec.inputs = {imagePort("image", "Image")};
            spec.outputs = {imagePort("image", "Image")};
            spec.params = {boolParam("alpha", "Convert alpha", false,
                "Alpha is almost always data, not color; leave this off.")};
            spec.evaluate = [transfer](const EvalArgs& args, EvalOutput* output) {
                const QImage source = toRgba(args.inputs.value(QStringLiteral("image")).image());
                if (source.isNull()) { output->error = QStringLiteral("Image is required."); return; }
                const bool withAlpha = paramBool(args.params, "alpha", false);
                // 256 possible inputs: build the curve once instead of per texel.
                uchar table[256];
                for (int i = 0; i < 256; ++i)
                    table[i] = uchar(std::lround(std::clamp(transfer(i / 255.0f), 0.0f, 1.0f) * 255.0f));
                QImage result = source.copy();
                for (int y = 0; y < result.height(); ++y) {
                    uchar* line = result.scanLine(y);
                    for (int x = 0; x < result.width(); ++x) {
                        line[x * 4 + 0] = table[line[x * 4 + 0]];
                        line[x * 4 + 1] = table[line[x * 4 + 1]];
                        line[x * 4 + 2] = table[line[x * 4 + 2]];
                        if (withAlpha) line[x * 4 + 3] = table[line[x * 4 + 3]];
                    }
                }
                output->outputs.insert(QStringLiteral("image"), Value::fromImage(result));
            };
            registry.add(spec);
        };
        colorSpaceNode("color.srgbToLinear", "sRGB to Linear",
            "Decodes an sRGB-encoded texture to linear light.",
            static_cast<float (*)(float)>(&Converter::srgbToLinear));
        colorSpaceNode("color.linearToSrgb", "Linear to sRGB",
            "Encodes linear light back to sRGB for a color texture.",
            static_cast<float (*)(float)>(&Converter::linearToSrgb));
    }

    {
        // The one piece of the Fallout 4 conversion that generic nodes cannot
        // express, and the reason it gets a node of its own rather than staying
        // inside a monolith. Rebuilding a metal's albedo means normalising a
        // colour to unit luminance and then scaling it to a target brightness:
        // the intermediate routinely exceeds 1 (a near-black diffuse divided by
        // its own luma is enormous), and every image port here is 8-bit, so a
        // Divide followed by a Multiply would clip the hue away between them.
        // Per-channel clamping instead of a max-rescale FLATTENS hue — a real
        // bug the tests already pin.
        NodeTypeSpec spec;
        spec.id = QStringLiteral("color.metalAlbedo");
        spec.name = QStringLiteral("Metal Albedo");
        spec.category = QStringLiteral("Inference");
        spec.summary = QStringLiteral("Rebuilds a metal's base colour: hue from the diffuse "
                                      "(or the cubemap where the diffuse is too dark to trust), "
                                      "brightness from specular strength.");
        spec.inputs = {imagePort("diffuse", "Diffuse (linear)"),
            imagePort("specular", "Specular Strength", true),
            colorPort("tint", "Cubemap Tint", true)};
        spec.outputs = {imagePort("image", "Image")};
        spec.params = {
            floatParam("reliability", "Diffuse trust", 0.09, 0.0, 0.5, 0.005,
                "Linear luma below which the diffuse is too dark to carry hue and the "
                "cubemap tint takes over. Vanilla authors some metals fully black."),
            floatParam("f0Min", "Brightness min", 0.35, 0.0, 1.0, 0.01),
            floatParam("f0Max", "Brightness max", 0.85, 0.0, 1.0, 0.01),
            floatParam("tintClamp", "Tint saturation clamp", 0.35, 0.0, 1.0, 0.01,
                "Stops a scene-neutral cubemap (DefaultOutside1's sky blue) tinting metals.")};
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            const QImage diffuse = toRgba(args.inputs.value(QStringLiteral("diffuse")).image());
            if (diffuse.isNull()) {
                output->outputs.insert(QStringLiteral("image"), Value::fromImage(QImage()));
                output->status = QStringLiteral("no input");
                return;
            }
            const QImage specular = toRgba(args.inputs.value(QStringLiteral("specular")).image());
            const Value tintValue = args.inputs.value(QStringLiteral("tint"));
            const bool hasTint = tintValue.type() == ValueType::Color;
            const QVector3D tint = hasTint ? tintValue.color() : QVector3D(0.5f, 0.5f, 0.5f);

            Converter::Options options;
            options.diffuseReliabilityThreshold = paramFloat(args.params, "reliability", 0.09f);
            options.metalF0Min = paramFloat(args.params, "f0Min", 0.35f);
            options.metalF0Max = paramFloat(args.params, "f0Max", 0.85f);
            options.cubemapTintSaturationClamp = paramFloat(args.params, "tintClamp", 0.35f);

            QImage result(diffuse.width(), diffuse.height(), QImage::Format_RGBA8888);
            for (int y = 0; y < diffuse.height(); ++y) {
                const uchar* in = diffuse.constScanLine(y);
                uchar* out = result.scanLine(y);
                const uchar* specLine = specular.isNull() ? nullptr
                    : specular.constScanLine(std::min(y * specular.height() / diffuse.height(),
                          specular.height() - 1));
                for (int x = 0; x < diffuse.width(); ++x) {
                    const QVector3D linear(in[x * 4] / 255.0f, in[x * 4 + 1] / 255.0f,
                        in[x * 4 + 2] / 255.0f);
                    const float strength = specLine
                        ? specLine[std::min(x * specular.width() / diffuse.width(),
                              specular.width() - 1) * 4] / 255.0f
                        : 1.0f;
                    const QVector3D metal = Converter::recoverMetalAlbedoFo4(linear, strength,
                        tint, hasTint, options);
                    out[x * 4] = uchar(std::lround(std::clamp(metal.x(), 0.0f, 1.0f) * 255.0f));
                    out[x * 4 + 1] = uchar(std::lround(std::clamp(metal.y(), 0.0f, 1.0f) * 255.0f));
                    out[x * 4 + 2] = uchar(std::lround(std::clamp(metal.z(), 0.0f, 1.0f) * 255.0f));
                    out[x * 4 + 3] = in[x * 4 + 3];
                }
            }
            output->outputs.insert(QStringLiteral("image"), Value::fromImage(result));
            output->status = hasTint ? QStringLiteral("hue: diffuse + cubemap")
                                     : QStringLiteral("hue: diffuse only");
        };
        registry.add(spec);
    }

    // ---------------- Maths ----------------

    {
        static const QStringList operations{
            QStringLiteral("Add"), QStringLiteral("Subtract"), QStringLiteral("Multiply"),
            QStringLiteral("Divide"), QStringLiteral("Power"), QStringLiteral("Minimum"),
            QStringLiteral("Maximum"), QStringLiteral("Absolute"), QStringLiteral("Square Root"),
            QStringLiteral("Floor"), QStringLiteral("Ceil"), QStringLiteral("Round"),
            QStringLiteral("Modulo"), QStringLiteral("Greater Than"), QStringLiteral("Less Than")};
        NodeTypeSpec spec;
        spec.id = QStringLiteral("math.op");
        spec.name = QStringLiteral("Math");
        spec.category = QStringLiteral("Maths");
        spec.summary = QStringLiteral("Per-channel maths on a texture, against a second texture or a constant.");
        spec.inputs = {imagePort("a", "A"), imagePort("b", "B", true)};
        spec.outputs = {imagePort("image", "Image")};
        spec.params = {enumParam("operation", "Operation", 2, operations),
            floatParam("value", "Value", 1.0, -8.0, 8.0, 0.01, "Used when B is not connected."),
            boolParam("clamp", "Clamp to 0-1", true),
            boolParam("alpha", "Include alpha", false)};
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            const QImage a = args.inputs.value(QStringLiteral("a")).image();
            if (a.isNull()) {
                // The evaluator already rejects an unconnected required input,
                // so a null here means the upstream slot is unauthored — which
                // is normal, and passes through as "nothing" like every other
                // absent map in the pipeline.
                output->outputs.insert(QStringLiteral("image"), Value::fromImage(QImage()));
                output->status = QStringLiteral("no input");
                return;
            }
            const QImage b = args.inputs.value(QStringLiteral("b")).image();
            const int operation = paramInt(args.params, "operation", 2);
            const float constant = paramFloat(args.params, "value", 1.0f);
            const bool clampResult = paramBool(args.params, "clamp", true);
            const bool withAlpha = paramBool(args.params, "alpha", false);

            const auto apply = [operation](float x, float y) -> float {
                switch (operation) {
                case 0: return x + y;
                case 1: return x - y;
                case 2: return x * y;
                case 3: return std::abs(y) < 1e-6f ? 0.0f : x / y;
                case 4: return std::pow(std::max(x, 0.0f), y);
                case 5: return std::min(x, y);
                case 6: return std::max(x, y);
                case 7: return std::abs(x);
                case 8: return std::sqrt(std::max(x, 0.0f));
                case 9: return std::floor(x);
                case 10: return std::ceil(x);
                case 11: return std::round(x);
                case 12: return std::abs(y) < 1e-6f ? 0.0f : std::fmod(x, y);
                case 13: return x > y ? 1.0f : 0.0f;
                case 14: return x < y ? 1.0f : 0.0f;
                default: return x;
                }
            };
            const auto write = [&](const uchar* left, const uchar* right, uchar* out) {
                for (int c = 0; c < 4; ++c) {
                    if (c == 3 && !withAlpha) { out[3] = left[3]; continue; }
                    const float x = left[c] / 255.0f;
                    const float y = right ? right[c] / 255.0f : constant;
                    float value = apply(x, y);
                    if (clampResult) value = std::clamp(value, 0.0f, 1.0f);
                    out[c] = uchar(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
                }
            };

            if (b.isNull()) {
                const QImage source = toRgba(a);
                QImage result = source.copy();
                for (int y = 0; y < result.height(); ++y) {
                    const uchar* in = source.constScanLine(y);
                    uchar* out = result.scanLine(y);
                    for (int x = 0; x < result.width(); ++x) write(in + x * 4, nullptr, out + x * 4);
                }
                output->outputs.insert(QStringLiteral("image"), Value::fromImage(result));
                return;
            }
            output->outputs.insert(QStringLiteral("image"), Value::fromImage(combine(a, b,
                [&](const uchar* left, const uchar* right, uchar* out) { write(left, right, out); })));
        };
        registry.add(spec);
    }

    {
        NodeTypeSpec spec;
        spec.id = QStringLiteral("image.mix");
        spec.name = QStringLiteral("Mix");
        spec.category = QStringLiteral("Maths");
        spec.summary = QStringLiteral("Blends A toward B, optionally masked by a third texture.");
        spec.inputs = {imagePort("a", "A"), imagePort("b", "B"), imagePort("factor", "Factor", true)};
        spec.outputs = {imagePort("image", "Image")};
        spec.params = {floatParam("factor", "Factor", 0.5, 0.0, 1.0, 0.01,
            "Used where no Factor texture is connected.")};
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            const QImage a = args.inputs.value(QStringLiteral("a")).image();
            const QImage b = args.inputs.value(QStringLiteral("b")).image();
            if (a.isNull() && b.isNull()) {
                output->outputs.insert(QStringLiteral("image"), Value::fromImage(QImage()));
                output->status = QStringLiteral("no input");
                return;
            }
            // One side absent means the other passes through unblended.
            if (a.isNull() || b.isNull()) {
                output->outputs.insert(QStringLiteral("image"), Value::fromImage(a.isNull() ? b : a));
                return;
            }
            const QImage mask = toRgba(args.inputs.value(QStringLiteral("factor")).image());
            const float constant = std::clamp(paramFloat(args.params, "factor", 0.5f), 0.0f, 1.0f);
            if (mask.isNull()) {
                output->outputs.insert(QStringLiteral("image"), Value::fromImage(combine(a, b,
                    [&](const uchar* left, const uchar* right, uchar* out) {
                        for (int c = 0; c < 4; ++c)
                            out[c] = uchar(std::lround(left[c] + (right[c] - left[c]) * constant));
                    })));
                return;
            }
            // With a mask the per-texel factor replaces the constant — this is
            // the spec-to-metalness blend, so it has to be per pixel.
            const QImage left = toRgba(a), right = toRgba(b);
            const int width = std::max(left.width(), right.width());
            const int height = std::max(left.height(), right.height());
            QImage result(width, height, QImage::Format_RGBA8888);
            for (int y = 0; y < height; ++y) {
                const uchar* leftLine = left.constScanLine(std::min(y * left.height() / height, left.height() - 1));
                const uchar* rightLine = right.constScanLine(std::min(y * right.height() / height, right.height() - 1));
                const uchar* maskLine = mask.constScanLine(std::min(y * mask.height() / height, mask.height() - 1));
                uchar* out = result.scanLine(y);
                for (int x = 0; x < width; ++x) {
                    const uchar* lp = leftLine + std::min(x * left.width() / width, left.width() - 1) * 4;
                    const uchar* rp = rightLine + std::min(x * right.width() / width, right.width() - 1) * 4;
                    const float t = maskLine[std::min(x * mask.width() / width, mask.width() - 1) * 4] / 255.0f;
                    for (int c = 0; c < 4; ++c)
                        out[x * 4 + c] = uchar(std::lround(lp[c] + (rp[c] - lp[c]) * t));
                }
            }
            output->outputs.insert(QStringLiteral("image"), Value::fromImage(result));
        };
        registry.add(spec);
    }

    {
        NodeTypeSpec spec;
        spec.id = QStringLiteral("image.luminance");
        spec.name = QStringLiteral("Luminance");
        spec.category = QStringLiteral("Maths");
        spec.summary = QStringLiteral("Rec.709 luma of the RGB channels — the intensity the metal "
                                      "inference keys off.");
        spec.inputs = {imagePort("image", "Image")};
        spec.outputs = {imagePort("image", "Image")};
        spec.params = {boolParam("linearize", "Treat as sRGB", false,
            "Decode to linear light first; the conversion math measures linear luma.")};
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            const QImage source = toRgba(args.inputs.value(QStringLiteral("image")).image());
            if (source.isNull()) { output->error = QStringLiteral("Image is required."); return; }
            const bool linearize = paramBool(args.params, "linearize", false);
            QImage result(source.width(), source.height(), QImage::Format_RGBA8888);
            for (int y = 0; y < source.height(); ++y) {
                const uchar* in = source.constScanLine(y);
                uchar* out = result.scanLine(y);
                for (int x = 0; x < source.width(); ++x) {
                    QVector3D rgb(in[x * 4] / 255.0f, in[x * 4 + 1] / 255.0f, in[x * 4 + 2] / 255.0f);
                    if (linearize) rgb = Converter::srgbToLinear(rgb);
                    const uchar luma = uchar(std::lround(
                        std::clamp(Converter::luminance(rgb), 0.0f, 1.0f) * 255.0f));
                    out[x * 4] = out[x * 4 + 1] = out[x * 4 + 2] = luma;
                    out[x * 4 + 3] = 255;
                }
            }
            output->outputs.insert(QStringLiteral("image"), Value::fromImage(result));
        };
        registry.add(spec);
    }

    {
        NodeTypeSpec spec;
        spec.id = QStringLiteral("image.smoothstep");
        spec.name = QStringLiteral("Smoothstep");
        spec.category = QStringLiteral("Maths");
        spec.summary = QStringLiteral("Hermite ramp between two edges. Edge A above edge B inverts it, "
                                      "which is how the dark-diffuse signal is built.");
        spec.inputs = {imagePort("image", "Image")};
        spec.outputs = {imagePort("image", "Image")};
        spec.params = {floatParam("edge0", "Edge A", 0.0, 0.0, 1.0, 0.01),
            floatParam("edge1", "Edge B", 1.0, 0.0, 1.0, 0.01)};
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            const QImage source = toRgba(args.inputs.value(QStringLiteral("image")).image());
            if (source.isNull()) { output->error = QStringLiteral("Image is required."); return; }
            const float edge0 = paramFloat(args.params, "edge0", 0.0f);
            const float edge1 = paramFloat(args.params, "edge1", 1.0f);
            if (qFuzzyCompare(edge0, edge1)) { output->error = QStringLiteral("The edges are equal."); return; }
            uchar table[256];
            for (int i = 0; i < 256; ++i) {
                const float t = std::clamp((i / 255.0f - edge0) / (edge1 - edge0), 0.0f, 1.0f);
                table[i] = uchar(std::lround(t * t * (3.0f - 2.0f * t) * 255.0f));
            }
            QImage result = source.copy();
            for (int y = 0; y < result.height(); ++y) {
                uchar* line = result.scanLine(y);
                for (int x = 0; x < result.width(); ++x)
                    for (int c = 0; c < 3; ++c) line[x * 4 + c] = table[line[x * 4 + c]];
            }
            output->outputs.insert(QStringLiteral("image"), Value::fromImage(result));
        };
        registry.add(spec);
    }

    {
        NodeTypeSpec spec;
        spec.id = QStringLiteral("image.solidColor");
        spec.name = QStringLiteral("Solid Color");
        spec.category = QStringLiteral("Source");
        spec.summary = QStringLiteral("A flat fill — wire a color from Source Material, or pick one, for "
                                      "slots the material states as a value rather than a texture.");
        // The scalar input is the bridge from a material's numbers into the
        // image maths: Specular Mult, Smoothness and Emittance Mult are all
        // scalar ports, and without somewhere to plug them a graph can only
        // hard-code what the .bgsm already states.
        spec.inputs = {colorPort("color", "Color", true), scalarPort("value", "Value", true)};
        spec.outputs = {imagePort("image", "Image")};
        spec.params = {ParamSpec{QStringLiteral("swatch"), QStringLiteral("Color"),
                           ParamSpec::Kind::Color, 0, 0, 0, QStringLiteral("#ffffff"), {},
                           QStringLiteral("Used when no color is wired in.")},
            floatParam("multiplier", "Multiplier", 1.0, 0.0, 8.0, 0.01,
                "Scales the colour. A wired Value replaces it."),
            enumParam("size", "Size", 0, QStringList{QStringLiteral("Material max"),
                                             QStringLiteral("256"), QStringLiteral("512"),
                                             QStringLiteral("1024"), QStringLiteral("2048")})};
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            QVector3D color(1, 1, 1);
            const Value wired = args.inputs.value(QStringLiteral("color"));
            if (wired.type() == ValueType::Color) {
                color = wired.color();
            } else {
                const QColor swatch(paramString(args.params, "swatch"));
                if (swatch.isValid()) color = QVector3D(float(swatch.redF()), float(swatch.greenF()), float(swatch.blueF()));
            }
            const Value wiredValue = args.inputs.value(QStringLiteral("value"));
            const float multiplier = wiredValue.type() == ValueType::Scalar
                ? wiredValue.scalar() : paramFloat(args.params, "multiplier", 1.0f);

            int size = 0;
            switch (paramInt(args.params, "size", 0)) {
            case 1: size = 256; break;
            case 2: size = 512; break;
            case 3: size = 1024; break;
            case 4: size = 2048; break;
            default: size = args.context.materialMaxResolution > 0 ? args.context.materialMaxResolution : 512; break;
            }
            output->nativeResolution = size;
            if (args.context.previewMode && args.context.previewMaxSize > 0)
                size = std::min(size, args.context.previewMaxSize);
            size = std::clamp(size, 1, 8192);

            QImage result(size, size, QImage::Format_RGBA8888);
            result.fill(QColor::fromRgbF(std::clamp(color.x() * multiplier, 0.0f, 1.0f),
                std::clamp(color.y() * multiplier, 0.0f, 1.0f),
                std::clamp(color.z() * multiplier, 0.0f, 1.0f)));
            output->outputs.insert(QStringLiteral("image"), Value::fromImage(result));
            output->status = QStringLiteral("#%1 · %2px")
                .arg(QColor::fromRgbF(std::clamp(color.x(), 0.0f, 1.0f), std::clamp(color.y(), 0.0f, 1.0f),
                         std::clamp(color.z(), 0.0f, 1.0f)).name().mid(1))
                .arg(output->nativeResolution);
        };
        registry.add(spec);
    }

    {
        NodeTypeSpec spec;
        spec.id = QStringLiteral("image.separate");
        spec.name = QStringLiteral("Separate Channels");
        spec.category = QStringLiteral("Composite");
        spec.summary = QStringLiteral("Splits one texture into its four channels in a single node.");
        spec.inputs = {imagePort("image", "Image")};
        spec.outputs = {imagePort("r", "R"), imagePort("g", "G"),
            imagePort("b", "B"), imagePort("a", "A")};
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            const QImage source = args.inputs.value(QStringLiteral("image")).image();
            if (source.isNull()) { output->error = QStringLiteral("Image is required."); return; }
            const char* const ids[4] = {"r", "g", "b", "a"};
            for (int i = 0; i < 4; ++i)
                output->outputs.insert(QLatin1String(ids[i]),
                    Value::fromImage(grayscaleFromChannel(source, i)));
        };
        registry.add(spec);
    }

    {
        // "Project setting" defers to the conversion-wide policy so one control
        // raises every under-sized map in a run; the rest are per-node overrides
        // for the map that needs treating differently from the batch.
        static const QStringList upscaleModes{QStringLiteral("Project setting"),
            QStringLiteral("Off"), QStringLiteral("Match highest"),
            QStringLiteral("2x highest"), QStringLiteral("4x highest"),
            QStringLiteral("Fixed size")};
        // What the map IS, which picks the model out of the standard-model
        // table (Settings ▸ Upscaling ▸ Standard Models) and the handling
        // around it. A normal is a vector field, not a picture: it goes in as
        // RG0 and its Z is rebuilt afterwards.
        static const QStringList upscaleSlots = [] {
            QStringList entries{QStringLiteral("Auto (by port)")};
            for (const Upscaler::MapSlot slot : Upscaler::allSlots())
                entries.append(Upscaler::slotLabel(slot));
            return entries;
        }();
        // Read once, when the library is built: the models are whatever sits
        // beside the configured upscaler. Entry 0 defers to the map kind, so a
        // graph stays meaningful on a machine with a different install — or
        // none at all, where every entry past 0 simply is not offered.
        static const QStringList upscaleModels =
            QStringList{QStringLiteral("Standard (by map role)")} + Upscaler::models();
        // Restoration is the pass this corpus actually wants and could not run:
        // every FO4/FO76 source map arrives BC-compressed, and the damage is
        // block artifacts rather than missing detail — a median 10.2% of a BC1
        // reflectance map smears into an eight-value band (docs/FO4-Conversion).
        // A 4x upscaler run on that faithfully ENLARGES the blocks. De-block at
        // 1:1 first, then enlarge.
        {
        static const QStringList restoreModels = [] {
            QStringList entries{QStringLiteral("Standard (Settings)")};
            for (const QString& model : Upscaler::models())
                if (Upscaler::modelNativeScale(model) == 1) entries.append(model);
            return entries;
        }();
        NodeTypeSpec spec;
        spec.id = QStringLiteral("image.restore");
        spec.name = QStringLiteral("Restore");
        spec.category = QStringLiteral("Composite");
        spec.summary = QStringLiteral("Removes block-compression artifacts at 1:1, without resizing. "
                                      "Runs before Upscale, so detail is not invented on top of blocks.");
        spec.inputs = {imagePort("image", "Image")};
        spec.outputs = {imagePort("image", "Image")};
        spec.params = {enumParam("model", "Model", 0, restoreModels,
            "Which restoration model runs. Standard uses the one set in "
            "Settings ▸ Upscaling ▸ Standard Models. Only 1× models are offered — "
            "anything else would change the size.")};
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            const QImage source = toRgba(args.inputs.value(QStringLiteral("image")).image());
            if (source.isNull()) { output->error = QStringLiteral("Image is required."); return; }
            // A neural pass takes seconds; the graph re-evaluates on every
            // keystroke, so the preview shows the input untouched and says so.
            // Conversion is where it actually runs.
            if (args.context.previewMode) {
                output->outputs.insert(QStringLiteral("image"), Value::fromImage(source));
                output->status = QStringLiteral("%1×%2 · runs on convert")
                    .arg(source.width()).arg(source.height());
                return;
            }
            const int modelIndex = paramInt(args.params, "model", 0);
            const QString model = (modelIndex > 0 && modelIndex < restoreModels.size())
                ? restoreModels.at(modelIndex) : QString();
            const Upscaler::MapSlot slot =
                Upscaler::slotForLabel(args.inputLabels.value(QStringLiteral("image")));
            const Upscaler::Result result =
                Upscaler::restore(source, Upscaler::kindForSlot(slot), model);
            output->outputs.insert(QStringLiteral("image"), Value::fromImage(result.image));
            output->status = result.engine;
            if (!result.note.isEmpty()) output->warning = result.note;
        };
        registry.add(spec);
        }

        NodeTypeSpec spec;
        spec.id = QStringLiteral("image.upscale");
        spec.name = QStringLiteral("Upscale");
        spec.category = QStringLiteral("Composite");
        spec.summary = QStringLiteral("Raises a map that ships below the rest of its material. "
                                      "Uses a neural model when one is configured, Lanczos otherwise.");
        spec.inputs = {imagePort("image", "Image")};
        spec.outputs = {imagePort("image", "Image")};
        spec.params = {enumParam("mode", "Target", 0, upscaleModes),
            intParam("size", "Fixed size", 2048, 64, 8192,
                "Only used when Target is Fixed size."),
            enumParam("mapSlot", "Map role", 0, upscaleSlots,
                "What this map is, which decides both the standard model it uses and the "
                "handling around it. Normals go through the model as RG0 and have Z rebuilt."),
            enumParam("model", "Model", 0, upscaleModels,
                "Which trained model runs. Standard defers to the role's entry in "
                "Settings ▸ Upscaling ▸ Standard Models; the rest of the list is whatever "
                "sits beside the configured upscaler, to override one map without "
                "changing the standard.")};
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            const QImage source = toRgba(args.inputs.value(QStringLiteral("image")).image());
            if (source.isNull()) { output->error = QStringLiteral("Image is required."); return; }
            // What size this MAP really is, which is not what upstream reports.
            // nativeResolution is one number per node, and a Separate node
            // hands out a dozen maps of different sizes from a single result —
            // so the figure inherited down this wire is the material's LARGEST
            // map, i.e. exactly the thing being measured against. A 4x4 flat _r
            // in a 1024 material claimed to be 1024 and never triggered.
            //
            // Below the preview cap nothing was downsampled, so the size on
            // hand is the true one; at or above it the image may have been
            // shrunk and the reported figure is the better guess. Conversion
            // runs uncapped, so it always measures the map itself.
            const int actual = std::max(source.width(), source.height());
            const int previewCap = args.context.previewMode && args.context.previewMaxSize > 0
                ? args.context.previewMaxSize : 0;
            const int incoming = (previewCap == 0 || actual < previewCap)
                ? actual : args.inputNative.value(QStringLiteral("image"), actual);
            const int highest = args.context.materialMaxResolution;

            // Resolve the target. Everything but Fixed size is expressed
            // against the material's own highest-resolution map, so a rule set
            // once holds for every material in a batch whatever size it is.
            int mode = paramInt(args.params, "mode", 0);
            if (mode == 0) mode = args.context.upscalePolicy + 1; // policy: 0 Off .. 3 4x
            int target = 0;
            switch (mode) {
            case 2: target = highest; break;
            case 3: target = highest * 2; break;
            case 4: target = highest * 4; break;
            case 5: target = paramInt(args.params, "size", 2048); break;
            default: target = 0; break; // Off
            }

            if (target <= 0 || incoming <= 0 || target <= incoming) {
                output->outputs.insert(QStringLiteral("image"), Value::fromImage(source));
                output->nativeResolution = std::max(incoming, 0);
                // The detection is the point, so it reports either way: under
                // the material's best map, or already level with it.
                output->status = target <= 0
                    ? QStringLiteral("%1px · off").arg(incoming)
                    : QStringLiteral("%1px · already at %2px").arg(incoming).arg(target);
                return;
            }
            if (highest > 0 && incoming < highest)
                output->notes.append(QStringLiteral("%1px under the material's %2px")
                    .arg(incoming).arg(highest));

            // Which role this map plays, which is what picks the model out of
            // the standard-model table. Graphs saved before roles existed carry
            // the old three-way "kind" instead, so it migrates rather than
            // being read as a slot index and silently meaning something else.
            int slotIndex = args.params.contains(QStringLiteral("mapSlot"))
                ? paramInt(args.params, "mapSlot", 0)
                : legacyKindToSlot(paramInt(args.params, "kind", 0));
            Upscaler::MapSlot slot = slotIndex > 0
                && slotIndex <= Upscaler::allSlots().size()
                ? Upscaler::allSlots().at(slotIndex - 1) : Upscaler::MapSlot::Other;
            if (slotIndex == 0) {
                // Auto reads the label the upstream port gave this image, which
                // is how a graph says what a map is without a second setting.
                slot = Upscaler::slotForLabel(args.inputLabels.value(QStringLiteral("image")));
            }
            const Upscaler::MapKind kind = Upscaler::kindForSlot(slot);

            // Preview runs small so the graph keeps up with a slider; the
            // reported native resolution is still the real one, which is what
            // the under-resolution warnings key off. A neural model is far too
            // slow to run per keystroke, so preview always resamples.
            const int cap = args.context.previewMode && args.context.previewMaxSize > 0
                ? args.context.previewMaxSize : target;
            const double factor = double(target) / double(incoming);
            const int wantW = std::max(1, int(std::lround(source.width() * factor)));
            const int wantH = std::max(1, int(std::lround(source.height() * factor)));
            const int limit = std::max(wantW, wantH);
            if (args.context.previewMode || limit > cap) {
                const int previewW = limit > cap ? std::max(1, wantW * cap / limit) : wantW;
                const int previewH = limit > cap ? std::max(1, wantH * cap / limit) : wantH;
                output->outputs.insert(QStringLiteral("image"),
                    Value::fromImage(Upscaler::lanczos(source, QSize(previewW, previewH))));
                output->nativeResolution = target;
                output->status = QStringLiteral("%1px → %2px · preview resample").arg(incoming).arg(target);
                return;
            }

            Upscaler::Request request;
            request.image = source;
            request.kind = kind;
            request.slot = slot;
            const int modelIndex = paramInt(args.params, "model", 0);
            request.model = (modelIndex > 0 && modelIndex < upscaleModels.size())
                ? upscaleModels.at(modelIndex) : QString();
            // Models come at fixed scales; ask for the one that covers the gap
            // and let the engine bring any overshoot back down.
            request.factor = factor > 2.0 ? 4 : 2;
            const Upscaler::Result produced = Upscaler::upscale(request);
            QImage scaled = produced.image;
            if (scaled.size() != QSize(wantW, wantH))
                scaled = Upscaler::lanczos(scaled, QSize(wantW, wantH));
            output->outputs.insert(QStringLiteral("image"), Value::fromImage(scaled));
            output->nativeResolution = target;
            output->status = QStringLiteral("%1px → %2px · %3").arg(incoming).arg(target).arg(produced.engine);
            if (!produced.note.isEmpty()) output->notes.append(produced.note);
        };
        registry.add(spec);
    }

    {
        NodeTypeSpec spec;
        spec.id = QStringLiteral("image.multiply");
        spec.name = QStringLiteral("Multiply");
        spec.category = QStringLiteral("Composite");
        spec.summary = QStringLiteral("Multiplies two images, or one image by a constant.");
        spec.inputs = {imagePort("a", "A"), imagePort("b", "B", true)};
        spec.outputs = {imagePort("image", "Image")};
        spec.params = {floatParam("scale", "Scale", 1.0, 0.0, 4.0, 0.01)};
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            const QImage a = args.inputs.value(QStringLiteral("a")).image();
            if (a.isNull()) { output->error = QStringLiteral("Input A is required."); return; }
            const QImage b = args.inputs.value(QStringLiteral("b")).image();
            const float scale = paramFloat(args.params, "scale", 1.0f);
            QImage result;
            if (b.isNull()) {
                result = toRgba(a);
                for (int y = 0; y < result.height(); ++y) {
                    uchar* line = result.scanLine(y);
                    for (int x = 0; x < result.width() * 4; ++x)
                        if (x % 4 != 3) line[x] = uchar(std::clamp(std::lround(line[x] * scale), 0L, 255L));
                }
            } else {
                result = combine(a, b, [scale](const uchar* left, const uchar* right, uchar* out) {
                    for (int i = 0; i < 3; ++i)
                        out[i] = uchar(std::clamp(std::lround(left[i] * (right[i] / 255.0) * scale), 0L, 255L));
                    out[3] = left[3];
                });
            }
            output->outputs.insert(QStringLiteral("image"), Value::fromImage(result));
        };
        registry.add(spec);
    }

    {
        NodeTypeSpec spec;
        spec.id = QStringLiteral("image.levels");
        spec.name = QStringLiteral("Levels");
        spec.category = QStringLiteral("Composite");
        spec.summary = QStringLiteral("Input/output range remap with gamma, over a histogram of the input.");
        spec.inputs = {imagePort("image", "Image")};
        spec.outputs = {imagePort("image", "Image")};
        spec.params = {
            enumParam("channel", "Channel", 4, {QStringLiteral("Red"), QStringLiteral("Green"),
                QStringLiteral("Blue"), QStringLiteral("Alpha"), QStringLiteral("Luminance")},
                "Which channel the histogram plots. Luminance drives R, G and B together; "
                "picking one scopes the remap to it and leaves the others untouched."),
            floatParam("inBlack", "In black", 0.0, 0.0, 1.0),
            floatParam("inWhite", "In white", 1.0, 0.0, 1.0),
            floatParam("gamma", "Gamma", 1.0, 0.1, 5.0, 0.05),
            floatParam("outBlack", "Out black", 0.0, 0.0, 1.0),
            floatParam("outWhite", "Out white", 1.0, 0.0, 1.0),
            boolParam("autoLevels", "Auto levels", true,
                "Pick the black and white points from this map's own distribution, snapped to a "
                "fixed ladder so two maps that look alike always get the same pair."),
        };
        // Substance's Levels has a Clamp checkbox; this one deliberately does
        // not. Every image here is 8-bit RGBA, so storage clamps whatever the
        // remap produces and the control could not change a single texel.
        // Drives the strip drawn in the node body.
        spec.histogram.blackPoint = QStringLiteral("inBlack");
        spec.histogram.gamma = QStringLiteral("gamma");
        spec.histogram.whitePoint = QStringLiteral("inWhite");
        spec.histogram.outBlack = QStringLiteral("outBlack");
        spec.histogram.outWhite = QStringLiteral("outWhite");
        spec.histogram.channel = QStringLiteral("channel");
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            QImage image = toRgba(args.inputs.value(QStringLiteral("image")).image());
            if (image.isNull()) { output->error = QStringLiteral("Image is required."); return; }
            float inBlack = paramFloat(args.params, "inBlack", 0.0f);
            float inWhite = paramFloat(args.params, "inWhite", 1.0f);
            const float gamma = std::max(paramFloat(args.params, "gamma", 1.0f), 0.001f);
            const float outBlack = paramFloat(args.params, "outBlack", 0.0f);
            const float outWhite = paramFloat(args.params, "outWhite", 1.0f);
            const int channel = std::clamp(paramInt(args.params, "channel", 4), 0, 4);

            // Histogram of the INPUT, before the remap, so the handles sit on
            // the distribution they are about to move. Substance shows it the
            // same way; a post-remap plot would chase its own tail as you drag.
            output->histogram.assign(NodeGraph::kHistogramChannels * NodeGraph::kHistogramBuckets, 0u);
            quint32* buckets = output->histogram.data();
            for (int y = 0; y < image.height(); ++y) {
                const uchar* line = image.constScanLine(y);
                for (int x = 0; x < image.width(); ++x) {
                    const uchar* texel = line + x * 4;
                    for (int c = 0; c < 4; ++c) ++buckets[c * NodeGraph::kHistogramBuckets + texel[c]];
                    const int luma = int(std::lround(0.2126 * texel[0] + 0.7152 * texel[1] + 0.0722 * texel[2]));
                    ++buckets[4 * NodeGraph::kHistogramBuckets + std::clamp(luma, 0, 255)];
                }
            }

            // Autodetect. The hard requirement is CONSISTENCY: fitting each map to
            // its own p1/p99 would make two copies of one surface convert
            // differently, which is exactly the invariant the whole conversion
            // rests on. So this classifies rather than fits — it finds the
            // distribution's floor and ceiling, then snaps both to a 0.05 ladder.
            // Maps that look alike land on the same rung; a map with no metal in
            // it at all gets one fixed answer instead of a fitted one.
            if (paramBool(args.params, "autoLevels", false)) {
                const quint32* luma = output->histogram.constData()
                    + 4 * NodeGraph::kHistogramBuckets;
                double total = 0;
                for (int i = 0; i < NodeGraph::kHistogramBuckets; ++i) total += luma[i];
                if (total > 0) {
                    const auto percentile = [luma, total](double fraction) {
                        double seen = 0;
                        for (int i = 0; i < NodeGraph::kHistogramBuckets; ++i) {
                            seen += luma[i];
                            if (seen >= fraction * total) return i / 255.0;
                        }
                        return 1.0;
                    };
                    // 5% rungs, and deliberately not finer. A 1% ladder was tried
                    // and the consistency test caught it immediately: two maps
                    // differing by one value in the floor and two in the ceiling
                    // resolved to 0.71 and 0.70, which is the arbitrary drift this
                    // is supposed to prevent. Per-map adaptation comes from the
                    // classifier below deciding WHICH case a map is, not from
                    // giving the window more decimal places to wander in.
                    const auto snap = [](double value) {
                        return std::clamp(std::round(value / 0.05) * 0.05, 0.0, 1.0);
                    };
                    // The floor is a percentile, but the CEILING must not be:
                    // metal is usually a small bright minority on a mostly dark
                    // map, and p99 lands inside the dark bulk and reports the
                    // whole thing flat. Take the highest bucket that still holds
                    // a real population instead — small enough to catch a metal
                    // trim on a corner of the sheet, large enough that a few
                    // stray compression texels cannot stretch the window.
                    const double floorAt = percentile(0.01);
                    // 0.02%: on a 1024 map that is ~200 texels — a metal trim on
                    // one corner survives, a handful of compression outliers do not.
                    const double presence = std::max(1.0, total * 0.0002);
                    double ceilingAt = floorAt;
                    for (int i = NodeGraph::kHistogramBuckets - 1; i >= 0; --i) {
                        if (luma[i] < presence) continue;
                        ceilingAt = i / 255.0;
                        break;
                    }
                    QString verdict;
                    if (ceilingAt - floorAt < 0.03) {
                        // One population, nothing to separate. But flat does NOT
                        // mean dielectric: a uniformly metal part is equally flat,
                        // just parked higher. Which it is depends on where it sits
                        // relative to the authored dielectric floor of 0.22.
                        if (floorAt > 0.35) {
                            // Uniformly metal. Put the window below it so the whole
                            // surface reads as metal rather than landing mid-ramp
                            // in the band a PBR validator flags as ambiguous.
                            inBlack = float(snap(floorAt - 0.10));
                            inWhite = float(snap(floorAt - 0.02));
                            verdict = QStringLiteral("flat, all metal");
                        } else {
                            inBlack = 0.25f;
                            inWhite = 0.70f;
                            verdict = QStringLiteral("flat, no metal");
                        }
                    } else {
                        // The 0.03 of headroom is the BC1 quantisation smear above
                        // the authored dielectric floor; without it that smear
                        // ramps up as low-level metal.
                        inBlack = float(snap(floorAt + 0.03));
                        inWhite = float(std::max(snap(ceilingAt), double(inBlack) + 0.05));
                        verdict = QStringLiteral("%1-%2").arg(floorAt, 0, 'f', 2).arg(ceilingAt, 0, 'f', 2);
                    }
                    // A node wired with inBlack ABOVE inWhite is inverted on
                    // purpose — the diffuse gate reads "darker means more", and
                    // Levels takes a signed span to express it. Autodetect works
                    // in ascending terms, so without this it silently flips that
                    // gate's meaning: bright albedo would start counting as
                    // metal and the mask it guards collapses. Preserve the
                    // direction the graph was built with.
                    if (paramFloat(args.params, "inBlack", 0.0f)
                        > paramFloat(args.params, "inWhite", 1.0f))
                        std::swap(inBlack, inWhite);
                    output->status = QStringLiteral("auto %1/%2 · %3")
                        .arg(inBlack, 0, 'f', 2).arg(inWhite, 0, 'f', 2).arg(verdict);
                    // Publish so the handles draw where the maths actually is.
                    output->resolvedLevels = {inBlack, inWhite};
                }
            }

            const float span = std::abs(inWhite - inBlack) < 1e-5f ? 1e-5f : inWhite - inBlack;
            uchar table[256];
            for (int i = 0; i < 256; ++i) {
                // Clamped before pow() so a negative base can never reach it.
                float value = std::clamp((i / 255.0f - inBlack) / span, 0.0f, 1.0f);
                value = std::pow(value, 1.0f / gamma);
                table[i] = uchar(std::lround(std::clamp(outBlack + value * (outWhite - outBlack),
                    0.0f, 1.0f) * 255.0f));
            }
            // Luminance drives the three colour channels together, which is the
            // grayscale-map case every conversion node feeds this. A single
            // channel scopes the edit and leaves the rest of the texel alone.
            const bool all = channel == 4;
            for (int y = 0; y < image.height(); ++y) {
                uchar* line = image.scanLine(y);
                for (int x = 0; x < image.width(); ++x) {
                    uchar* texel = line + x * 4;
                    if (all) { for (int c = 0; c < 3; ++c) texel[c] = table[texel[c]]; }
                    else texel[channel] = table[texel[channel]];
                }
            }
            output->outputs.insert(QStringLiteral("image"), Value::fromImage(image));
        };
        registry.add(spec);
    }

    {
        NodeTypeSpec spec;
        spec.id = QStringLiteral("image.curves");
        spec.name = QStringLiteral("Curves");
        spec.category = QStringLiteral("Composite");
        spec.summary = QStringLiteral("Arbitrary tone curve. Levels gives linear plus gamma; this shapes the rest.");
        spec.inputs = {imagePort("image", "Image")};
        spec.outputs = {imagePort("image", "Image")};
        spec.params = {
            enumParam("channel", "Channel", 4, {QStringLiteral("Red"), QStringLiteral("Green"),
                QStringLiteral("Blue"), QStringLiteral("Alpha"), QStringLiteral("Luminance")}),
        };
        // The control points live in their own param as a JSON array; the canvas
        // edits them through the CurveSpec binding rather than a text row.
        spec.params.append([] {
            ParamSpec points;
            points.id = QStringLiteral("points");
            points.name = QStringLiteral("Points");
            points.kind = ParamSpec::Kind::Text;
            points.defaultValue = QStringLiteral("[[0,0],[1,1]]");
            return points;
        }());
        spec.curve.points = QStringLiteral("points");
        spec.curve.channel = QStringLiteral("channel");
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            QImage image = toRgba(args.inputs.value(QStringLiteral("image")).image());
            if (image.isNull()) { output->error = QStringLiteral("Image is required."); return; }
            const int channel = std::clamp(paramInt(args.params, "channel", 4), 0, 4);

            const QVector<QPointF> points = NodeGraph::curvePoints(
                paramString(args.params, "points"));
            uchar table[256];
            for (int i = 0; i < 256; ++i)
                table[i] = uchar(std::lround(std::clamp(
                    NodeGraph::evaluateCurve(points, i / 255.0), 0.0, 1.0) * 255.0));

            // Same histogram contract as Levels, so the editor can draw the
            // input distribution behind the curve the way Photoshop does.
            output->histogram.assign(NodeGraph::kHistogramChannels * NodeGraph::kHistogramBuckets, 0u);
            quint32* buckets = output->histogram.data();
            for (int y = 0; y < image.height(); ++y) {
                const uchar* line = image.constScanLine(y);
                for (int x = 0; x < image.width(); ++x) {
                    const uchar* texel = line + x * 4;
                    for (int c = 0; c < 4; ++c) ++buckets[c * NodeGraph::kHistogramBuckets + texel[c]];
                    const int luma = int(std::lround(0.2126 * texel[0] + 0.7152 * texel[1] + 0.0722 * texel[2]));
                    ++buckets[4 * NodeGraph::kHistogramBuckets + std::clamp(luma, 0, 255)];
                }
            }

            const bool all = channel == 4;
            for (int y = 0; y < image.height(); ++y) {
                uchar* line = image.scanLine(y);
                for (int x = 0; x < image.width(); ++x) {
                    uchar* texel = line + x * 4;
                    if (all) { for (int c = 0; c < 3; ++c) texel[c] = table[texel[c]]; }
                    else texel[channel] = table[texel[channel]];
                }
            }
            output->outputs.insert(QStringLiteral("image"), Value::fromImage(image));
        };
        registry.add(spec);
    }

    {
        // Substance's workhorse mask node: one knob for where the edge sits,
        // one for how hard it is. Levels can approximate it, but this states the
        // intent directly and keeps the edge centred as contrast changes.
        NodeTypeSpec spec;
        spec.id = QStringLiteral("image.histogramScan");
        spec.name = QStringLiteral("Histogram Scan");
        spec.category = QStringLiteral("Composite");
        spec.summary = QStringLiteral("Grayscale to mask: position picks the threshold, contrast sharpens it.");
        spec.inputs = {imagePort("image", "Image")};
        spec.outputs = {imagePort("image", "Image")};
        spec.params = {
            floatParam("position", "Position", 0.5, 0.0, 1.0, 0.005,
                "Input level that lands on mid-grey in the output."),
            floatParam("contrast", "Contrast", 0.5, 0.0, 1.0, 0.005,
                "0 passes the image through. 1 is a hard threshold at the position."),
            boolParam("smooth", "Smooth edge", true,
                "Ease the ramp instead of a straight line, so the transition has no visible seam."),
        };
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            QImage image = toRgba(args.inputs.value(QStringLiteral("image")).image());
            if (image.isNull()) { output->error = QStringLiteral("Image is required."); return; }
            const float position = paramFloat(args.params, "position", 0.5f);
            const float contrast = std::clamp(paramFloat(args.params, "contrast", 0.5f), 0.0f, 1.0f);
            const bool smooth = paramBool(args.params, "smooth", true);
            // Contrast 1 collapses the window to nothing (a hard step at the
            // position); contrast 0 opens it to the whole range (a pass-through).
            const float halfWidth = std::max((1.0f - contrast) * 0.5f, 1e-4f);
            const float low = position - halfWidth;
            const float high = position + halfWidth;
            uchar table[256];
            for (int i = 0; i < 256; ++i) {
                float t = std::clamp((i / 255.0f - low) / (high - low), 0.0f, 1.0f);
                if (smooth) t = t * t * (3.0f - 2.0f * t);
                table[i] = uchar(std::lround(t * 255.0f));
            }
            for (int y = 0; y < image.height(); ++y) {
                uchar* line = image.scanLine(y);
                for (int x = 0; x < image.width(); ++x)
                    for (int c = 0; c < 3; ++c) line[x * 4 + c] = table[line[x * 4 + c]];
            }
            output->outputs.insert(QStringLiteral("image"), Value::fromImage(image));
        };
        registry.add(spec);
    }

    {
        static const QStringList blurKinds{QStringLiteral("Box"), QStringLiteral("Gaussian"),
            QStringLiteral("Median")};
        NodeTypeSpec spec;
        spec.id = QStringLiteral("image.blur");
        spec.name = QStringLiteral("Blur");
        spec.category = QStringLiteral("Composite");
        spec.summary = QStringLiteral("Box, Gaussian, or Median. Median clears block compression noise without softening edges.");
        spec.inputs = {imagePort("image", "Image")};
        spec.outputs = {imagePort("image", "Image")};
        spec.params = {
            enumParam("kind", "Kind", 1, blurKinds,
                "Median is the one for BC block artefacts: it removes them without rounding off real edges."),
            intParam("radius", "Radius", 2, 1, 24),
            boolParam("alpha", "Include alpha", false),
        };
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            const QImage source = toRgba(args.inputs.value(QStringLiteral("image")).image());
            if (source.isNull()) { output->error = QStringLiteral("Image is required."); return; }
            const int kind = std::clamp(paramInt(args.params, "kind", 1), 0, 2);
            const int radius = std::max(1, paramInt(args.params, "radius", 2));
            const bool withAlpha = paramBool(args.params, "alpha", false);
            const int channels = withAlpha ? 4 : 3;
            const int width = source.width(), height = source.height();
            QImage result = source;

            if (kind == 2) {
                // Median: no separable trick, so keep the window honest and let
                // the radius cap keep it affordable.
                std::vector<uchar> window;
                window.reserve((2 * radius + 1) * (2 * radius + 1));
                for (int y = 0; y < height; ++y) {
                    uchar* out = result.scanLine(y);
                    for (int x = 0; x < width; ++x) {
                        for (int c = 0; c < channels; ++c) {
                            window.clear();
                            for (int dy = -radius; dy <= radius; ++dy) {
                                const int sy = std::clamp(y + dy, 0, height - 1);
                                const uchar* line = source.constScanLine(sy);
                                for (int dx = -radius; dx <= radius; ++dx) {
                                    const int sx = std::clamp(x + dx, 0, width - 1);
                                    window.push_back(line[sx * 4 + c]);
                                }
                            }
                            const size_t middle = window.size() / 2;
                            std::nth_element(window.begin(), window.begin() + middle, window.end());
                            out[x * 4 + c] = window[middle];
                        }
                    }
                }
            } else {
                // Box and Gaussian are both separable, so this runs as two 1D
                // passes rather than one quadratic one.
                std::vector<float> kernel(2 * radius + 1, 1.0f);
                if (kind == 1) {
                    const float sigma = std::max(radius / 2.0f, 0.5f);
                    for (int i = -radius; i <= radius; ++i)
                        kernel[i + radius] = std::exp(-(i * i) / (2.0f * sigma * sigma));
                }
                float weight = 0.0f;
                for (float k : kernel) weight += k;
                for (float& k : kernel) k /= weight;

                QImage pass = source;
                const auto sweep = [&](const QImage& in, QImage& out, bool horizontal) {
                    for (int y = 0; y < height; ++y) {
                        uchar* outLine = out.scanLine(y);
                        for (int x = 0; x < width; ++x) {
                            float sums[4] = {0, 0, 0, 0};
                            for (int i = -radius; i <= radius; ++i) {
                                const int sx = horizontal ? std::clamp(x + i, 0, width - 1) : x;
                                const int sy = horizontal ? y : std::clamp(y + i, 0, height - 1);
                                const uchar* texel = in.constScanLine(sy) + sx * 4;
                                for (int c = 0; c < channels; ++c) sums[c] += texel[c] * kernel[i + radius];
                            }
                            for (int c = 0; c < channels; ++c)
                                outLine[x * 4 + c] = uchar(std::lround(std::clamp(sums[c], 0.0f, 255.0f)));
                        }
                    }
                };
                sweep(source, pass, true);
                sweep(pass, result, false);
            }
            output->outputs.insert(QStringLiteral("image"), Value::fromImage(result));
        };
        registry.add(spec);
    }

    {
        NodeTypeSpec spec;
        spec.id = QStringLiteral("image.normalFlipY");
        spec.name = QStringLiteral("Flip Normal Y");
        spec.category = QStringLiteral("Composite");
        spec.summary = QStringLiteral("Invert the green channel: converts a normal map between DirectX and OpenGL convention.");
        spec.inputs = {imagePort("image", "Normal")};
        spec.outputs = {imagePort("image", "Normal")};
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            QImage image = toRgba(args.inputs.value(QStringLiteral("image")).image());
            if (image.isNull()) { output->error = QStringLiteral("Normal map is required."); return; }
            // Green only. Doing this with separate/invert/combine works but is
            // three nodes for a convention flip, and it is silent when wrong.
            for (int y = 0; y < image.height(); ++y) {
                uchar* line = image.scanLine(y);
                for (int x = 0; x < image.width(); ++x)
                    line[x * 4 + 1] = uchar(255 - line[x * 4 + 1]);
            }
            output->outputs.insert(QStringLiteral("image"), Value::fromImage(image));
        };
        registry.add(spec);
    }

    {
        NodeTypeSpec spec;
        spec.id = QStringLiteral("derive.heightToNormal");
        spec.name = QStringLiteral("Height to Normal");
        spec.category = QStringLiteral("Derive");
        spec.summary = QStringLiteral("Sobel slope of a height map into a tangent-space normal.");
        spec.inputs = {imagePort("image", "Height")};
        spec.outputs = {imagePort("image", "Normal")};
        spec.params = {
            floatParam("strength", "Strength", 4.0, 0.1, 32.0, 0.1),
            boolParam("flipY", "OpenGL green", false,
                "Off writes DirectX convention (green down), which is what FO4 and FO76 ship."),
        };
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            const QImage height = toRgba(args.inputs.value(QStringLiteral("image")).image());
            if (height.isNull()) { output->error = QStringLiteral("Height map is required."); return; }
            const float strength = paramFloat(args.params, "strength", 4.0f);
            const bool flipY = paramBool(args.params, "flipY", false);
            const int width = height.width(), rows = height.height();
            QImage normal(width, rows, QImage::Format_RGBA8888);
            const auto sample = [&](int x, int y) {
                const int sx = std::clamp(x, 0, width - 1);
                const int sy = std::clamp(y, 0, rows - 1);
                return height.constScanLine(sy)[sx * 4] / 255.0f;
            };
            for (int y = 0; y < rows; ++y) {
                uchar* line = normal.scanLine(y);
                for (int x = 0; x < width; ++x) {
                    // Sobel: less noisy than a plain central difference, which
                    // matters because the height itself is usually derived.
                    const float dx = (sample(x + 1, y - 1) + 2 * sample(x + 1, y) + sample(x + 1, y + 1))
                                   - (sample(x - 1, y - 1) + 2 * sample(x - 1, y) + sample(x - 1, y + 1));
                    const float dy = (sample(x - 1, y + 1) + 2 * sample(x, y + 1) + sample(x + 1, y + 1))
                                   - (sample(x - 1, y - 1) + 2 * sample(x, y - 1) + sample(x + 1, y - 1));
                    QVector3D n(-dx * strength, (flipY ? 1.0f : -1.0f) * dy * strength, 1.0f);
                    n.normalize();
                    line[x * 4] = uchar(std::lround((n.x() * 0.5f + 0.5f) * 255.0f));
                    line[x * 4 + 1] = uchar(std::lround((n.y() * 0.5f + 0.5f) * 255.0f));
                    line[x * 4 + 2] = uchar(std::lround((n.z() * 0.5f + 0.5f) * 255.0f));
                    line[x * 4 + 3] = 255;
                }
            }
            output->outputs.insert(QStringLiteral("image"), Value::fromImage(normal));
        };
        registry.add(spec);
    }

    {
        static const QStringList resizeFilters{QStringLiteral("Nearest"), QStringLiteral("Bilinear"),
            QStringLiteral("Smooth")};
        NodeTypeSpec spec;
        spec.id = QStringLiteral("image.resize");
        spec.name = QStringLiteral("Resize");
        spec.category = QStringLiteral("Composite");
        spec.summary = QStringLiteral("Resample to a fixed size. Nearest keeps data maps exact; Smooth is for colour.");
        spec.inputs = {imagePort("image", "Image")};
        spec.outputs = {imagePort("image", "Image")};
        spec.params = {
            intParam("size", "Size", 1024, 4, 8192, "Longest edge; aspect is kept."),
            enumParam("filter", "Filter", 2, resizeFilters,
                "Nearest for masks and index-like data. Smooth for colour and normals."),
        };
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            const QImage source = toRgba(args.inputs.value(QStringLiteral("image")).image());
            if (source.isNull()) { output->error = QStringLiteral("Image is required."); return; }
            const int size = std::max(4, paramInt(args.params, "size", 1024));
            const int filter = std::clamp(paramInt(args.params, "filter", 2), 0, 2);
            const int longest = std::max(source.width(), source.height());
            if (longest == 0) { output->error = QStringLiteral("Image is empty."); return; }
            const double factor = double(size) / double(longest);
            const QSize target(std::max(1, int(std::lround(source.width() * factor))),
                std::max(1, int(std::lround(source.height() * factor))));
            const QImage scaled = source.scaled(target, Qt::IgnoreAspectRatio,
                filter == 0 ? Qt::FastTransformation : Qt::SmoothTransformation);
            output->nativeResolution = std::max(target.width(), target.height());
            output->status = QStringLiteral("%1×%2").arg(target.width()).arg(target.height());
            output->outputs.insert(QStringLiteral("image"), Value::fromImage(scaled));
        };
        registry.add(spec);
    }

    {
        NodeTypeSpec spec;
        spec.id = QStringLiteral("image.invert");
        spec.name = QStringLiteral("Invert");
        spec.category = QStringLiteral("Composite");
        spec.summary = QStringLiteral("1 - value per colour channel.");
        spec.inputs = {imagePort("image", "Image")};
        spec.outputs = {imagePort("image", "Image")};
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            QImage image = toRgba(args.inputs.value(QStringLiteral("image")).image());
            if (image.isNull()) { output->error = QStringLiteral("Image is required."); return; }
            image.invertPixels(QImage::InvertRgb);
            output->outputs.insert(QStringLiteral("image"), Value::fromImage(image));
        };
        registry.add(spec);
    }

    {
        static const QStringList blendLabels{QStringLiteral("Mix"), QStringLiteral("Multiply"),
            QStringLiteral("Screen"), QStringLiteral("Add"), QStringLiteral("Subtract")};
        NodeTypeSpec spec;
        spec.id = QStringLiteral("image.blend");
        spec.name = QStringLiteral("Blend");
        spec.category = QStringLiteral("Composite");
        spec.summary = QStringLiteral("Blends B over A, optionally through a mask.");
        spec.inputs = {imagePort("a", "A"), imagePort("b", "B"), imagePort("mask", "Mask", true)};
        spec.outputs = {imagePort("image", "Image")};
        spec.params = {enumParam("mode", "Mode", 0, blendLabels),
            floatParam("opacity", "Opacity", 1.0, 0.0, 1.0, 0.01)};
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            const QImage a = args.inputs.value(QStringLiteral("a")).image();
            const QImage b = args.inputs.value(QStringLiteral("b")).image();
            if (a.isNull() || b.isNull()) { output->error = QStringLiteral("Both inputs are required."); return; }
            const int mode = paramInt(args.params, "mode", 0);
            const float opacity = paramFloat(args.params, "opacity", 1.0f);
            const QImage mask = toRgba(args.inputs.value(QStringLiteral("mask")).image());
            QImage blended = combine(a, b, [mode, opacity](const uchar* left, const uchar* right, uchar* out) {
                for (int i = 0; i < 3; ++i) {
                    const float base = left[i] / 255.0f, top = right[i] / 255.0f;
                    float value = top;
                    switch (mode) {
                    case 1: value = base * top; break;
                    case 2: value = 1.0f - (1.0f - base) * (1.0f - top); break;
                    case 3: value = base + top; break;
                    case 4: value = base - top; break;
                    default: break;
                    }
                    value = base + (std::clamp(value, 0.0f, 1.0f) - base) * opacity;
                    out[i] = uchar(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
                }
                out[3] = left[3];
            });
            if (!mask.isNull())
                blended = combine(toRgba(a), blended, [&mask](const uchar* base, const uchar* top, uchar* out) {
                    // Mask sampling rides the blended image's own grid.
                    for (int i = 0; i < 3; ++i) out[i] = top[i];
                    out[3] = base[3];
                });
            output->outputs.insert(QStringLiteral("image"), Value::fromImage(blended));
        };
        registry.add(spec);
    }

    // ---------------- Output ----------------

    {
        // The terminal node: everything ends here. It mirrors Source Material —
        // that one opens a game material up, this one closes a PBR one.
        // Channels can arrive packed or individually, and an individual input
        // always wins, which is how the derived AO reaches RMAOS.B and the
        // derived curvature reaches Normal.A without disturbing anything else.
        NodeTypeSpec spec;
        // Everything upstream of here works on plain single maps; this node is
        // the one place that knows how the FO4 PBR format packs them. Keeping
        // that knowledge in one node is why the terminal can take finished
        // textures only, instead of offering a packed input and a per-channel
        // input for the same data and having to arbitrate between them.
        spec.id = QStringLiteral("composite.pbrChannels");
        spec.name = QStringLiteral("Combine Maps (PBR)");
        spec.category = QStringLiteral("Composite");
        spec.summary = QStringLiteral("Packs single maps into the three shipped textures: "
                                      "BC = colour+opacity, N = normal+height+curvature, RMAOS.");
        spec.inputs = {
            imagePort("baseColor", "Base Color"), imagePort("opacity", "Opacity", true),
            imagePort("normal", "Normal", true), imagePort("height", "Height", true),
            imagePort("curvature", "Curvature", true),
            imagePort("roughness", "Roughness", true), imagePort("metallic", "Metallic", true),
            imagePort("ao", "Ambient Occlusion", true), imagePort("specular", "Specular F0 / Porosity", true),
            imagePort("emissive", "Emissive", true), imagePort("subsurface", "Subsurface", true)};
        spec.outputs = {packedPort("baseColor", "BC", kBaseColorChannels),
            packedPort("normal", "N", kNormalChannels),
            packedPort("rmaos", "RMAOS", kRmaosChannels), imagePort("emissive", "Emissive"),
            imagePort("subsurface", "Subsurface")};
        // The assembled PBR material: preview it shaded, or one shipped texture
        // at a time. Its packed maps are what the terminal writes out.
        spec.materialPreview = QStringLiteral("output");
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            const auto in = [&args](const char* id) {
                return toRgba(args.inputs.value(QLatin1String(id)).image());
            };
            const QImage baseColor = in("baseColor");
            if (baseColor.isNull()) { output->error = QStringLiteral("Base Color is required."); return; }

            // Contributors follow the base colour's resolution; a smaller one is
            // point-sampled up rather than refused, which is what the packer did.
            const auto blitChannel = [](QImage& target, const QImage& source, int channel) {
                if (target.isNull() || source.isNull()) return;
                const int width = target.width(), height = target.height();
                for (int y = 0; y < height; ++y) {
                    const uchar* line = source.constScanLine(
                        std::min(y * source.height() / height, source.height() - 1));
                    uchar* out = target.scanLine(y);
                    for (int x = 0; x < width; ++x)
                        out[x * 4 + channel] = line[std::min(x * source.width() / width, source.width() - 1) * 4];
                }
            };

            QImage base = baseColor.copy();
            blitChannel(base, in("opacity"), 3);

            // Shipped normal: RGB untouched, height in B and signed curvature in
            // A only where they were actually supplied.
            QImage normal = in("normal");
            if (!normal.isNull()) {
                normal = normal.copy();
                blitChannel(normal, in("height"), 2);
                blitChannel(normal, in("curvature"), 3);
            }

            const QImage roughness = in("roughness"), metallic = in("metallic");
            const QImage ao = in("ao"), specular = in("specular");
            QImage rmaos;
            for (const QImage* candidate : {&roughness, &metallic, &ao, &specular}) {
                if (candidate->isNull()) continue;
                rmaos = QImage(candidate->width(), candidate->height(), QImage::Format_RGBA8888);
                // Unwired defaults: no roughness, no metal, AO fully open, and
                // F0 at the dielectric constant. 0.04 linear is where glass,
                // plastic, cloth and paint all sit — leaving this slot at full
                // would ship a mirror finish on anything that never wired it.
                rmaos.fill(QColor(0, 0, 255, kDielectricF0));
                break;
            }
            blitChannel(rmaos, roughness, 0);
            blitChannel(rmaos, metallic, 1);
            blitChannel(rmaos, ao, 2);
            blitChannel(rmaos, specular, 3);

            output->outputs.insert(QStringLiteral("baseColor"), Value::fromImage(base));
            output->outputs.insert(QStringLiteral("normal"), Value::fromImage(normal));
            output->outputs.insert(QStringLiteral("rmaos"), Value::fromImage(rmaos));
            output->outputs.insert(QStringLiteral("emissive"), Value::fromImage(in("emissive")));
            output->outputs.insert(QStringLiteral("subsurface"), Value::fromImage(in("subsurface")));

            QStringList packed;
            packed << QStringLiteral("BC");
            if (!normal.isNull()) packed << QStringLiteral("N");
            if (!rmaos.isNull()) packed << QStringLiteral("RMAOS");
            output->status = QStringLiteral("%1×%2 · %3")
                .arg(base.width()).arg(base.height()).arg(packed.join(QStringLiteral(" ")));
        };
        registry.add(spec);
    }

    {
        NodeTypeSpec spec;
        spec.id = QStringLiteral("output.pbrMaterial");
        spec.name = QStringLiteral("Output Material (PBR)");
        spec.category = QStringLiteral("Output");
        spec.summary = QStringLiteral("The converted material. Wire the finished textures in here — "
                                      "this is what gets written.");
        // A whole material, not one map: no body thumbnail and no channel
        // strip. Selecting it shades that material in the 3D view, exactly as
        // the source material nodes shade the untouched original.
        spec.materialPreview = QStringLiteral("output");
        // Finished textures only. Packing lives in Combine Maps (PBR), so there is
        // exactly one route into every channel and nothing to arbitrate here.
        spec.inputs = {packedPort("baseColor", "BC", kBaseColorChannels),
            packedPort("normal", "N", kNormalChannels, true),
            packedPort("rmaos", "RMAOS", kRmaosChannels, true),
            imagePort("emissive", "Emissive", true), imagePort("subsurface", "Subsurface", true)};
        // No outputs: this is where a material stops. Nothing downstream can
        // consume it, so ports here would only invite wiring past the end of
        // the graph. Previewing it reads the textures that ARRIVE — the same
        // images a pass-through output would have handed back.
        spec.outputs = {};
        spec.params = {
            ParamSpec{QStringLiteral("baseSuffix"), QStringLiteral("Base suffix"), ParamSpec::Kind::Text,
                0, 0, 0, QStringLiteral("_BC"), {}, QStringLiteral("Filename suffix for the base color DDS.")},
            ParamSpec{QStringLiteral("normalSuffix"), QStringLiteral("Normal suffix"), ParamSpec::Kind::Text,
                0, 0, 0, QStringLiteral("_N"), {}, QStringLiteral("Filename suffix for the normal DDS.")},
            ParamSpec{QStringLiteral("rmaosSuffix"), QStringLiteral("RMAOS suffix"), ParamSpec::Kind::Text,
                0, 0, 0, QStringLiteral("_RMAOS"), {}, QStringLiteral("Filename suffix for the RMAOS DDS.")},
            ParamSpec{QStringLiteral("emissiveSuffix"), QStringLiteral("Emissive suffix"), ParamSpec::Kind::Text,
                0, 0, 0, QStringLiteral("_E"), {}, QStringLiteral("Filename suffix for the emissive DDS.")}};
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            const auto in = [&args](const char* id) {
                return toRgba(args.inputs.value(QLatin1String(id)).image());
            };
            // Nothing is assembled here any more: this node names the files and
            // states what will be written. Everything arriving is already packed.
            const QImage base = in("baseColor");
            if (base.isNull()) { output->error = QStringLiteral("BC is required."); return; }
            const QImage normal = in("normal");
            const QImage rmaos = in("rmaos");
            const QImage emissive = in("emissive");
            const QImage subsurface = in("subsurface");

            QStringList written;
            written << QStringLiteral("BC");
            if (!normal.isNull()) written << QStringLiteral("N");
            if (!rmaos.isNull()) written << QStringLiteral("RMAOS");
            if (!emissive.isNull()) written << QStringLiteral("E");
            if (!subsurface.isNull()) written << QStringLiteral("SSS");
            output->status = QStringLiteral("%1×%2 · %3")
                .arg(base.width()).arg(base.height()).arg(written.join(QStringLiteral(" ")));
        };
        registry.add(spec);
    }

    {
        // Superseded by Output Material (PBR), kept registered so graphs saved against
        // it still load and run.
        static const QStringList roleLabels{QStringLiteral("Base Color"), QStringLiteral("Normal"),
            QStringLiteral("RMAOS"), QStringLiteral("Emissive")};
        NodeTypeSpec spec;
        spec.id = QStringLiteral("output.texture");
        spec.name = QStringLiteral("Output Texture");
        spec.category = QStringLiteral("Output");
        spec.summary = QStringLiteral("Marks an image as one of the converted material's textures.");
        spec.inputs = {imagePort("image", "Image")};
        spec.outputs = {imagePort("image", "Image")};
        spec.params = {enumParam("role", "Role", 0, roleLabels),
            ParamSpec{QStringLiteral("suffix"), QStringLiteral("Suffix"), ParamSpec::Kind::Text,
                0, 0, 0, QStringLiteral("_BC"), {}, QStringLiteral("Filename suffix for the written DDS.")}};
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            const QImage image = args.inputs.value(QStringLiteral("image")).image();
            const QString role = roleLabels.value(paramInt(args.params, "role", 0));
            output->outputs.insert(QStringLiteral("image"), Value::fromImage(image));
            // A null image is normal, not an error: materials that do not emit
            // have no emissive map, exactly like an unauthored source slot.
            output->notes.append(image.isNull()
                ? QStringLiteral("%1 — nothing to write").arg(role)
                : QStringLiteral("%1 %2×%3").arg(role).arg(image.width()).arg(image.height()));
        };
        registry.add(spec);
    }
}

// ---------------- default graphs ----------------

namespace {

// Small builder so the default graphs read like the pipeline they encode.
struct Builder {
    Graph graph;
    const Registry& registry;

    explicit Builder(const Registry& reg) : registry(reg) {}

    QString add(const char* typeId, double x, double y, const QJsonObject& params = {})
    {
        const NodeTypeSpec* spec = registry.find(QLatin1String(typeId));
        if (!spec) return {};
        const QString id = graph.addNode(QLatin1String(typeId), *spec, QPointF(x, y));
        if (!params.isEmpty()) {
            Node* node = graph.node(id);
            for (auto it = params.constBegin(); it != params.constEnd(); ++it)
                node->params.insert(it.key(), it.value());
        }
        return id;
    }

    void link(const QString& fromNode, const char* fromPort, const QString& toNode, const char* toPort)
    {
        graph.connect(registry, Connection{fromNode, QLatin1String(fromPort), toNode, QLatin1String(toPort)});
    }
};

Registry& defaultRegistry()
{
    static Registry registry = [] { Registry created; registerAll(created); return created; }();
    return registry;
}

} // namespace

const Registry& sharedRegistry() { return defaultRegistry(); }

QImage normaliseReflectanceToSrgbScale(const QImage& image, bool sourceIsSrgb)
{
    // Already on the reference scale.
    if (sourceIsSrgb || image.isNull()) return image;
    QImage result = toRgba(image);
    if (result.isNull()) return result;
    // Stretch is what makes the two conventions span the same range, so one
    // threshold pair reaches full metal on either. Without it a BC4 map tops
    // out at 0.49 against a 0.70 white point and never gets past ~40% metal.
    const float stretch = (kBc1MetalTopSrgb - kBc1DielectricSrgb)
                        / (kBc4MetalTopSrgb - kBc4DielectricSrgb);
    uchar table[256];
    for (int i = 0; i < 256; ++i) {
        const float encoded = Converter::linearToSrgb(i / 255.0f);
        const float aligned = kBc1DielectricSrgb + (encoded - kBc4DielectricSrgb) * stretch;
        table[i] = uchar(std::lround(std::clamp(aligned, 0.0f, 1.0f) * 255.0f));
    }
    // Colour channels only: alpha is never gamma-encoded and carries no
    // reflectance here.
    for (int y = 0; y < result.height(); ++y) {
        uchar* line = result.scanLine(y);
        for (int x = 0; x < result.width(); ++x)
            for (int c = 0; c < 3; ++c) line[x * 4 + c] = table[line[x * 4 + c]];
    }
    return result;
}

int materialMaxResolution(const Bgsm::Material& material, const QString& textureRoot)
{
    // Not named "slots": that is a Qt keyword macro and expands to nothing.
    const QString authoredSlots[] = {material.diffuseTexture, material.normalTexture,
        material.smoothSpecTexture, material.specularTexture, material.lightingTexture,
        material.glowTexture, material.greyscaleTexture, material.envmapMaskTexture,
        material.wrinklesTexture, material.flowTexture, material.innerLayerTexture};
    int largest = 0;
    for (const QString& authored : authoredSlots) {
        if (authored.trimmed().isEmpty()) continue;
        // Header-only read: this runs once per material, not per node.
        const TextureImage::Diagnostics info = TextureImage::inspect(authored, textureRoot);
        if (!info.valid) continue;
        largest = std::max(largest, std::max(info.width, info.height));
    }
    return largest;
}

Converter::SuppliedImages runGraph(const Graph& graph, const QString& materialPath,
    const QString& textureRoot, const QString& referenceMesh, const QString& correctionMaskPath,
    QStringList* errors)
{
    Converter::SuppliedImages supplied;
    const Registry& registry = defaultRegistry();

    EvalContext context;
    context.sourcePath = materialPath;
    context.textureRoot = textureRoot;
    context.referenceMesh = referenceMesh;
    context.correctionMaskPath = correctionMaskPath;
    context.previewMode = false;   // full resolution, no preview downsample
    context.previewMaxSize = 0;
    context.thumbnailSize = 0;     // no thumbnails needed for a write run
    // Conversion is where upscaling actually runs: preview always resamples,
    // because a neural model takes seconds and the graph re-evaluates on every
    // keystroke.
    context.upscalePolicy = QSettings().value(QStringLiteral("Convert/upscalePolicy"), 0).toInt();
    {
        // Resolution levelling needs the material's own ceiling up front.
        Bgsm::Material material;
        if (Bgsm::readFile(materialPath, &material, nullptr))
            context.materialMaxResolution = materialMaxResolution(material, textureRoot);
    }

    Evaluator evaluator(registry);
    const QHash<QString, NodeResult> results = evaluator.evaluate(graph, context);

    bool reportedCoverage = false;
    for (const Node& node : graph.nodes) {
        const NodeResult& result = results.value(node.id);
        if (!result.error.isEmpty() && errors) {
            const NodeTypeSpec* spec = registry.find(node.typeId);
            errors->append(QStringLiteral("%1: %2").arg(spec ? spec->name : node.typeId, result.error));
        }
        // Keyed on the PORT, not on a node type. Nothing in the stock presets
        // publishes coverage now that the conversion nodes are retired, so the
        // measurement further down is the normal path — but a hand-wired graph
        // can still state it, and a stated number beats a measured one.
        const Value coverage = result.outputs.value(QStringLiteral("metalCoverage"));
        if (coverage.isValid()) {
            supplied.metalCoverage = coverage.scalar();
            reportedCoverage = true;
            supplied.notes.append(result.notes);
        } else if (node.typeId == QStringLiteral("derive.normal")) {
            supplied.notes.append(result.notes);
        }
        if (node.typeId == QStringLiteral("output.pbrMaterial")) {
            // The terminal ends the graph and keeps no outputs, so what it
            // holds is whatever was wired INTO it: follow each input back one
            // link. This used to read result.outputs, which stopped existing
            // when the terminal lost its ports — the preview was taught to
            // follow the wires and this path was not, so every conversion
            // failed with "no base colour" while the preview looked correct.
            const auto arriving = [&graph, &results, &node](const char* portId) {
                for (const Connection& link : graph.connections)
                    if (link.toNode == node.id && link.toPort == QLatin1String(portId))
                        return results.value(link.fromNode).outputs.value(link.fromPort).image();
                return QImage();
            };
            supplied.baseColor = arriving("baseColor");
            supplied.normal = arriving("normal");
            supplied.rmaos = arriving("rmaos");
            supplied.emissive = arriving("emissive");
            supplied.subsurface = arriving("subsurface");
            continue;
        }
        if (node.typeId != QStringLiteral("output.texture")) continue;
        const QImage image = result.outputs.value(QStringLiteral("image")).image();
        switch (node.params.value(QStringLiteral("role")).toInt()) {
        case 0: supplied.baseColor = image; break;
        case 1: supplied.normal = image; break;
        case 2: supplied.rmaos = image; break;
        case 3: supplied.emissive = image; break;
        default: break;
        }
    }

    // Metal coverage is a report, and the orchestrator's review flagging keys
    // off it too — but only the monolithic convert.* nodes publish it, and the
    // stock graphs decompose those into explicit maths. So every graph
    // conversion reported "metal 0%" and nothing was ever flagged for review,
    // however much metal it actually packed. Measure what was packed instead;
    // same definition convertercore uses, texels with metallic above half.
    if (!reportedCoverage && !supplied.rmaos.isNull()) {
        const QImage rmaos = supplied.rmaos.convertToFormat(QImage::Format_RGBA8888);
        qint64 metalTexels = 0;
        for (int y = 0; y < rmaos.height(); ++y) {
            const uchar* line = rmaos.constScanLine(y);
            for (int x = 0; x < rmaos.width(); ++x)
                if (line[x * 4 + 1] > 127) ++metalTexels;
        }
        const double texels = double(rmaos.width()) * rmaos.height();
        if (texels > 0) supplied.metalCoverage = float(double(metalTexels) / texels);
    }

    // The graph's identity joins the incremental signature, so editing it
    // invalidates previously converted outputs instead of silently skipping.
    supplied.signature = QStringLiteral("|graph%1|")
        .arg(QString::number(qHash(QJsonDocument(graph.toJson()).toJson(QJsonDocument::Compact)), 16));
    supplied.active = !supplied.baseColor.isNull();
    if (!supplied.active && errors)
        errors->append(QStringLiteral("the graph produced no base colour output"));
    return supplied;
}

Graph loadGraphFile(const QString& path, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = QStringLiteral("Invalid graph file: %1").arg(parseError.errorString());
        return {};
    }
    return Graph::fromJson(document.object());
}

bool saveGraphFile(const Graph& graph, const QString& path, QString* error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    file.write(QJsonDocument(graph.toJson()).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

Graph defaultFo4Graph()
{
    Builder builder(defaultRegistry());
    const QString material = builder.add("source.material.fo4", 0, 200);
    const QString tint = builder.add("source.cubemapTint", 260, 620);

    // Same shape as the FO76 graph: the source owns the filenames, the unpack
    // node is the only thing that knows how FO4 packs them, and no packed
    // texture travels past it. _s is BC5 across 99.8% of the vanilla corpus --
    // two channels, linear, R specular strength and G gloss, with no blue or
    // alpha to read.
    const QString unpack = builder.add("unpack.fo4", 300, 260);
    builder.link(material, "material", tint, "material");
    builder.link(material, "diffuse", unpack, "diffuse");
    builder.link(material, "smoothSpec", unpack, "specular");
    builder.link(material, "normal", unpack, "normal");
    builder.link(material, "glow", unpack, "glow");

    const QString upDiffuse = builder.add("image.upscale", 300, 40);
    const QString upSpecular = builder.add("image.upscale", 300, 120);
    const QString upNormal = builder.add("image.upscale", 300, 480);
    builder.link(unpack, "diffuse", upDiffuse, "image");
    builder.link(unpack, "specularStrength", upSpecular, "image");
    builder.link(unpack, "normal", upNormal, "image");

    const QString derive = builder.add("derive.normal", 700, 540);
    builder.link(upNormal, "image", derive, "normal");

    // ---- FO4 conversion, written out as nodes -------------------------------
    // Structurally unlike FO76's, because the two titles do not store the same
    // thing:
    //
    //   metallic  = levels(.35,.45, _s.R)   a STENCIL, then masked by hand
    //   roughness = 1 - _s.G                (the .bgsm smoothness where G is flat)
    //   F0        = clamp(_s.R) into the dielectric window, via one Levels
    //
    // FO76 states reflectance per texel in _r, so its metal mask is a
    // measurement and the Levels window is calibrated for accuracy. Fallout 4
    // states metallicity nowhere: as a CLASSIFIER _s.R manages 53% precision,
    // measured over 6616 materials, and _s.R with _s.G jointly does worse. But
    // the artist put the panel seams, rivets and wear into that channel, so its
    // SHAPE is right and what it gets wrong is whole regions -- the brick body,
    // the leather grip, the wood stock. Those are a couple of fills to paint
    // out, where building metal up from nothing means redrawing all of it.
    //
    // Because the workflow SUBTRACTS, the window is placed low and kept narrow.
    // Low because a metal object stencilled at 0% gives nothing to paint out --
    // measured, 0.35/0.45 leaves 21% of name-labelled metal materials dead and
    // killed MetalVerTrimFuturistic01 outright, where 0.20/0.30 leaves 7.8%.
    // Narrow because what it does emit should commit rather than smear across
    // the ambiguous band: 14.2% of the median map, against 31.8% at 0.20/0.45.
    // Drag the handles against the histogram per material -- that is the first
    // half of the workflow, and painting is the second.
    //
    // Auto levels OFF, unlike the FO76 reflectance window. FO76's _r has a real
    // absolute anchor -- the 0.22 dielectric floor -- so re-deriving per
    // material stays tied to something, and a flat map stays flat. _s.R has no
    // anchor at all, so auto levels would stretch a brick wall's own spec
    // variation across the full range and stencil it as metal, every time.
    const QString metallic = builder.add("image.levels", 700, 120,
        {{QStringLiteral("inBlack"), 0.20}, {QStringLiteral("inWhite"), 0.30},
            {QStringLiteral("autoLevels"), false}});
    builder.link(upSpecular, "image", metallic, "image");

    const QString roughness = builder.add("image.invert", 700, 260);
    builder.link(unpack, "gloss", roughness, "image");

    // ---- Base colour --------------------------------------------------------
    // Mixed in LINEAR light and encoded once at the end. Mixing an sRGB diffuse
    // against a linear metal albedo and then encoding applies the transfer
    // twice over metal -- the same trap the FO76 path documents.
    const QString diffuseLinear = builder.add("color.srgbToLinear", 700, 40);
    builder.link(upDiffuse, "image", diffuseLinear, "image");

    const QString metalAlbedo = builder.add("color.metalAlbedo", 920, 40);
    builder.link(diffuseLinear, "image", metalAlbedo, "diffuse");
    builder.link(upSpecular, "image", metalAlbedo, "specular");
    builder.link(tint, "tint", metalAlbedo, "tint");

    const QString baseMix = builder.add("image.mix", 1140, 60);
    const QString baseColor = builder.add("color.linearToSrgb", 1360, 60);
    builder.link(diffuseLinear, "image", baseMix, "a");
    builder.link(metalAlbedo, "image", baseMix, "b");
    // The same stencil that writes the metallic channel decides where the
    // recovered colour lands, so the two can never disagree.
    builder.link(metallic, "image", baseMix, "factor");
    builder.link(baseMix, "image", baseColor, "image");

    // ---- Dielectric F0 ------------------------------------------------------
    // _s.R is a Blinn-Phong intensity with no physical anchor, so the mapping is
    // anchored on the corpus instead: the measured median (54/255 = 0.212) maps
    // to the physical dielectric default 0.04, and the 0.02-0.08 window clamps
    // the tails. The two in-points are that same line read backwards -- 0.106
    // is where it crosses 0.02, 0.424 where it crosses 0.08 -- so one Levels
    // node reproduces the clamped ramp exactly, and shows the histogram under
    // it. Auto levels OFF: this is a physical quantity, not a classification,
    // and stretching it per material is what the roughness rule warns against.
    const QString specMultImage = builder.add("image.solidColor", 700, 340);
    builder.link(material, "specMult", specMultImage, "value");
    const QString specScaled = builder.add("math.op", 920, 320, {{QStringLiteral("operation"), 2}});
    builder.link(upSpecular, "image", specScaled, "a");
    builder.link(specMultImage, "image", specScaled, "b");
    const QString specularF0 = builder.add("image.levels", 1140, 320,
        {{QStringLiteral("inBlack"), 0.106}, {QStringLiteral("inWhite"), 0.424},
            {QStringLiteral("outBlack"), 0.02}, {QStringLiteral("outWhite"), 0.08},
            {QStringLiteral("autoLevels"), false}});
    builder.link(specScaled, "image", specularF0, "image");

    // ---- Emissive -----------------------------------------------------------
    // The glow mask tinted by the material's emittance colour, exactly as FO76
    // does it. The source node withholds both when the material does not emit,
    // so an unauthored glow costs nothing.
    const QString emitColor = builder.add("image.solidColor", 700, 620);
    const QString emissive = builder.add("math.op", 920, 620, {{QStringLiteral("operation"), 2}});
    builder.link(material, "emitColor", emitColor, "color");
    builder.link(unpack, "glow", emissive, "a");
    builder.link(emitColor, "image", emissive, "b");

    // The shipped normal keeps the original's RGB and takes only the derived
    // signed curvature in A. Feeding the whole normal in makes the old "B
    // defaulted to white" class of bug impossible.
    const QString channels = builder.add("composite.pbrChannels", 1580, 240);
    builder.link(baseColor, "image", channels, "baseColor");
    // Opacity now travels as its own map rather than riding the diffuse alpha
    // through a conversion node, which is the whole point of Separate Maps.
    builder.link(unpack, "opacity", channels, "opacity");
    builder.link(upNormal, "image", channels, "normal");
    builder.link(derive, "curvature", channels, "curvature");
    builder.link(roughness, "image", channels, "roughness");
    builder.link(metallic, "image", channels, "metallic");
    builder.link(derive, "ao", channels, "ao"); // derived detail AO, not the conversion's B
    builder.link(specularF0, "image", channels, "specular");
    builder.link(emissive, "image", channels, "emissive");

    const QString pbr = builder.add("output.pbrMaterial", 1580, 300);
    builder.link(channels, "baseColor", pbr, "baseColor");
    builder.link(channels, "normal", pbr, "normal");
    builder.link(channels, "rmaos", pbr, "rmaos");
    builder.link(channels, "emissive", pbr, "emissive");
    // The coordinates above only seed row order; real spacing comes from the
    // node sizes the canvas actually draws.
    NodeGraph::layoutGraph(builder.graph, defaultRegistry());
    return builder.graph;
}

Graph defaultFo76Graph()
{
    Builder builder(defaultRegistry());
    const QString material = builder.add("source.material.fo76", 0, 200);

    // Straight from the source into the unpack node — the mirror of Combine PBR
    // Maps handing finished textures straight to Output Material (PBR). The source owns
    // the filenames it reads and emits images; the unpack node is the only
    // thing that knows how FO76 packs them, and no packed texture travels past
    // it. A Texture or Image File node still goes inline on any slot that needs
    // overriding or resizing.
    const QString unpack = builder.add("unpack.fo76", 300, 260);
    builder.link(material, "diffuse", unpack, "diffuse");
    builder.link(material, "reflectance", unpack, "reflectance");
    builder.link(material, "normal", unpack, "normal");
    builder.link(material, "lighting", unpack, "lighting");
    builder.link(material, "glow", unpack, "glow");

    // Resolution levelling, before anything reads the maps. FO76 routinely
    // ships one map below the rest of its set — a 512 _r against a 1024 _d, or
    // a 4x4 flat — and every consumer downstream then works from whatever the
    // packer's point-sample made of it. These sit on the three maps that carry
    // real detail; each is on "Project setting", so with the policy off (the
    // default) they pass their input through untouched and cost nothing.
    const QString upDiffuse = builder.add("image.upscale", 300, 40);
    const QString upReflectance = builder.add("image.upscale", 300, 120);
    const QString upNormal = builder.add("image.upscale", 300, 460);
    builder.link(unpack, "diffuse", upDiffuse, "image");
    builder.link(unpack, "reflectance", upReflectance, "image");
    builder.link(unpack, "normal", upNormal, "image");

    const QString derive = builder.add("derive.normal", 700, 520);
    builder.link(upNormal, "image", derive, "normal");

    // ---- FO76 inference, written out as nodes -------------------------------
    // The same maths Converter::convertFo76 does, made visible so it can be
    // tuned and inspected map by map. That function is still the batch path, so
    // it stays the reference to diff against; it runs in float where these
    // nodes run in 8-bit, so small differences are expected. There is no
    // convert.fo76 NODE any more -- a registered node no preset uses is a
    // second definition of the conversion waiting to drift from the one that
    // runs, and this preset is the recipe now.
    //
    //   metallic  = levels(.25,.70, luma(reflectance_srgb)) x levels(.20,.11, luma(diffuse_srgb))
    //   roughness = 1 - _l.R          (no curve; see docs/FO4-Conversion.md)
    //   baseColor = mix(diffuse_linear, reflectance_linear, metallic) -> sRGB
    //   RMAOS     = (roughness, metallic, _l.G, clamp(luma(reflectance_linear), .02, .08))
    const QString diffuseLinear = builder.add("color.srgbToLinear", 480, 40);
    // _r arrives sRGB-encoded whatever it was authored as: the source node
    // normalises the BC4 linear maps up front, so this decode is correct for
    // every material rather than only the 36% that ship BC1 sRGB. It feeds the
    // colour path, which is linear-space maths -- mixing an encoded _r against
    // a linear diffuse and re-encoding applied the transfer twice over metal.
    const QString reflectanceLinear = builder.add("color.srgbToLinear", 480, 80);
    // NOT linearised, unlike the colour path above. The metal threshold is a
    // mask decision, not a light calculation, and it is placed by eye against
    // the histogram — so it belongs in the space the map is stored in, where
    // the numbers are the ones a survey of the corpus actually reports. In
    // linear the whole distribution collapses into the leftmost fifth of the
    // strip and the handles have no resolution where the data is.
    const QString reflLuma = builder.add("image.luminance", 700, 40,
        {{QStringLiteral("linearize"), false}});
    // Stored space, like the reflectance luma above and for the same reason:
    // the window below was measured on stored values, and the colour path has
    // its own decode. This fed the mask only, never the maths.
    const QString diffLuma = builder.add("image.luminance", 480, 200,
        {{QStringLiteral("linearize"), false}});
    // Levels rather than Smoothstep for both metal signals. Not for the remap —
    // Levels is a straight line where Smoothstep eases — but because Levels is
    // the only node that draws the input distribution under its own handles.
    // These thresholds decide what counts as metal, and picking them blind
    // against data nobody could see is how everything ended up grey. Levels
    // takes a signed span, so the descending signal below needs no Invert.
    // Both numbers come from surveying 900 FO76 materials rather than from
    // physical reflectance tables, which do not describe this corpus: steel
    // reads 0.09 linear here where real steel F0 is 0.56.
    //   0.25 black — the authored dielectric floor is 56/255 = 0.22, but BC1 is
    //     4bpp and quantises hard, smearing a median 10% of every map into
    //     57..64. A black point ON the floor lets that smear through as
    //     low-level metal, which is the grey-everywhere failure. The 0.03 of
    //     headroom is what clamps it.
    //   0.70 white — the median map's p99 is 179/255 = 0.70, so its brightest
    //     metal reaches FULL metal. At 0.75 it stalls at 90%, inside the band a
    //     PBR validator flags as ambiguous.
    const QString reflSignal = builder.add("image.levels", 700, 120,
        {{QStringLiteral("inBlack"), 0.25}, {QStringLiteral("inWhite"), 0.70}});
    // Mirrored: darker diffuse means a stronger metal signal. This is the
    // second opinion in the mask — a texel counts as metal only if it is BOTH
    // reflective AND dark in albedo, which is what stops a bright polished
    // dielectric passing on reflectance alone.
    //
    // Comparing the two populations texel by texel across 200 materials that
    // contain both, the diffuse under metal is 3.2x darker than under
    // dielectric (median 17.8 vs 64.3 of 255), and it holds in 185 of the 200.
    // The window sits in the gap, deliberately generous towards metal: this
    // gate can only ever REMOVE metal, so a false negative here silently
    // destroys a real metal surface, while a false positive still has to get
    // past the reflectance signal it is multiplied with.
    //   0.11 white — just above metal's p75 of 0.104, so at least three
    //     quarters of metal area passes at full strength.
    //   0.20 black — just below dielectric's p25 of 0.208, so at least three
    //     quarters of dielectric area is cut to zero outright.
    // The previous 0.75/0.25 was in linear and measured nothing: it output
    // 252-255 on every material tested, making the multiply a no-op.
    const QString darkSignal = builder.add("image.levels", 700, 200,
        {{QStringLiteral("inBlack"), 0.20}, {QStringLiteral("inWhite"), 0.11}});
    const QString metallic = builder.add("math.op", 920, 160, {{QStringLiteral("operation"), 2}});
    builder.link(upDiffuse, "image", diffuseLinear, "image");
    builder.link(upReflectance, "image", reflectanceLinear, "image");
    builder.link(upReflectance, "image", reflLuma, "image");
    builder.link(upDiffuse, "image", diffLuma, "image");
    builder.link(reflLuma, "image", reflSignal, "image");
    builder.link(diffLuma, "image", darkSignal, "image");
    builder.link(reflSignal, "image", metallic, "a");
    builder.link(darkSignal, "image", metallic, "b");

    // Roughness is the inversion and nothing else. A gloss-curve node used to
    // sit here at pow(x, 1.0) — the identity — as a place to dial a curve in,
    // but a node that computes nothing is clutter, and the curve already has
    // two homes that do something: `pbrm convert --gloss-curve` and
    // convertercore::glossToRoughness.
    // The 900-material survey behind this (docs/FO4-Conversion.md) found FO76's
    // smoothness range is authored intent, so the default here is no curve at
    // all — and "no curve" is best expressed by there being no node.
    const QString roughness = builder.add("image.invert", 700, 280);
    builder.link(unpack, "smoothness", roughness, "image");

    // Specular F0 is deliberately NOT derived here. FO76's _r is a reflectance
    // colour, and squeezing its luma into the 0.02-0.08 dielectric band varies
    // F0 across a surface that physically has none to vary: every common
    // dielectric sits at 0.04, and the differences that survive the clamp are
    // texture detail, not material change. Metals get their reflectance through
    // base colour and the metallic mask instead. Leaving the packer's slot
    // unwired holds F0 at the 0.04 constant. Wire something into Combine Maps
    // (PBR) -> Specular F0 for the rare material that needs otherwise.
    const QString baseMix = builder.add("image.mix", 1140, 100);
    const QString baseColor = builder.add("color.linearToSrgb", 1360, 100);
    builder.link(diffuseLinear, "image", baseMix, "a");
    // Both operands decoded: mixing encoded _r against a linear diffuse and then
    // re-encoding the result applied the transfer twice over metal.
    builder.link(reflectanceLinear, "image", baseMix, "b");
    builder.link(metallic, "image", baseMix, "factor");
    builder.link(baseMix, "image", baseColor, "image");

    // Emissive: the glow mask tinted by the material's emittance color.
    const QString emitColor = builder.add("image.solidColor", 700, 460);
    const QString emissive = builder.add("math.op", 920, 460, {{QStringLiteral("operation"), 2}});
    builder.link(material, "emitColor", emitColor, "color");
    builder.link(unpack, "glow", emissive, "a");
    builder.link(emitColor, "image", emissive, "b");

    // Every channel is packed in one node, named for what the map is rather
    // than which slot it lands in, so metallic cannot be wired into AO by
    // accident. The terminal receives finished textures only.
    const QString channels = builder.add("composite.pbrChannels", 1360, 240);
    builder.link(baseColor, "image", channels, "baseColor");
    builder.link(upNormal, "image", channels, "normal");
    builder.link(derive, "curvature", channels, "curvature");
    builder.link(roughness, "image", channels, "roughness");
    builder.link(metallic, "image", channels, "metallic");
    builder.link(unpack, "ao", channels, "ao"); // FO76 ships baked AO in _l.G
    builder.link(emissive, "image", channels, "emissive");
    builder.link(unpack, "subsurface", channels, "subsurface");

    const QString pbr = builder.add("output.pbrMaterial", 1580, 200);
    builder.link(channels, "baseColor", pbr, "baseColor");
    builder.link(channels, "normal", pbr, "normal");
    builder.link(channels, "rmaos", pbr, "rmaos");
    builder.link(channels, "emissive", pbr, "emissive");
    builder.link(channels, "subsurface", pbr, "subsurface");

    NodeGraph::layoutGraph(builder.graph, defaultRegistry());
    // Wrap the inference in a container, sized after layout so it actually
    // encloses the nodes it names.
    // emitColor is deliberately left out: it has no inputs, so the layout puts
    // it in the first column and it would drag the container's left edge back
    // over the Texture nodes.
    const QStringList inferenceNodes{diffuseLinear, reflectanceLinear, reflLuma, diffLuma, reflSignal, darkSignal,
        metallic, unpack, roughness, baseMix, baseColor, emissive,
        upDiffuse, upReflectance, upNormal};
    QRectF bounds;
    for (const QString& id : inferenceNodes) {
        const NodeGraph::Node* node = builder.graph.node(id);
        const NodeGraph::NodeTypeSpec* spec = node ? defaultRegistry().find(node->typeId) : nullptr;
        if (!spec) continue;
        const QRectF box(node->position, NodeGraph::nodeSize(*spec, *node, builder.graph));
        bounds = bounds.isNull() ? box : bounds.united(box);
    }
    if (!bounds.isNull())
        builder.graph.addFrame(QStringLiteral("FO76 Inference"), bounds.adjusted(-26, -46, 26, 26));
    return builder.graph;
}

Graph defaultGraphFor(const Bgsm::Material& material)
{
    return material.version > 2 ? defaultFo76Graph() : defaultFo4Graph();
}

} // namespace ConverterNodes
