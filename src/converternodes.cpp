#include "converternodes.h"

#include "convertercore.h"
#include "geometryao.h"
#include "normalderive.h"
#include "previewmesh.h"
#include "textureimage.h"

#include <QFileInfo>
#include <QJsonValue>
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

PortSpec scalarPort(const char* id, const char* name)
{
    PortSpec spec;
    spec.id = QLatin1String(id); spec.name = QLatin1String(name); spec.type = ValueType::Scalar;
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

// ---- image helpers ----

QImage toRgba(const QImage& image)
{
    return image.isNull() ? image : image.convertToFormat(QImage::Format_RGBA8888);
}

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

Converter::Options mathOptions(const QJsonObject& params)
{
    Converter::Options options;
    options.recoveryStrength = paramFloat(params, "recovery", options.recoveryStrength);
    options.glossCurve = paramFloat(params, "glossCurve", options.glossCurve);
    options.normalizeSpec = paramBool(params, "normalizeSpec", options.normalizeSpec);
    options.specNormalizePercentile = paramFloat(params, "specPercentile", options.specNormalizePercentile);
    options.metalSpecStart = paramFloat(params, "metalSpecStart", options.metalSpecStart);
    options.metalSpecFull = paramFloat(params, "metalSpecFull", options.metalSpecFull);
    options.metalDarkStart = paramFloat(params, "metalDarkStart", options.metalDarkStart);
    options.metalDarkFull = paramFloat(params, "metalDarkFull", options.metalDarkFull);
    options.dielectricF0Min = paramFloat(params, "dielectricF0Min", options.dielectricF0Min);
    options.dielectricF0Max = paramFloat(params, "dielectricF0Max", options.dielectricF0Max);
    options.fo76ReflectanceStart = paramFloat(params, "fo76ReflectanceStart", options.fo76ReflectanceStart);
    options.fo76ReflectanceFull = paramFloat(params, "fo76ReflectanceFull", options.fo76ReflectanceFull);
    options.fo76DarkStart = paramFloat(params, "fo76DarkStart", options.fo76DarkStart);
    options.fo76DarkFull = paramFloat(params, "fo76DarkFull", options.fo76DarkFull);
    return options;
}

void addConvertedOutputs(const Converter::ConvertedImages& converted, EvalOutput* output)
{
    output->outputs.insert(QStringLiteral("baseColor"), Value::fromImage(converted.baseColor));
    output->outputs.insert(QStringLiteral("rmaos"), Value::fromImage(converted.rmaos));
    output->outputs.insert(QStringLiteral("emissive"), Value::fromImage(converted.emissive));
    output->outputs.insert(QStringLiteral("metallic"), Value::fromImage(converted.metallic));
    output->outputs.insert(QStringLiteral("metalCoverage"), Value::fromScalar(converted.metalCoverage));
    output->notes.append(QStringLiteral("metal coverage %1%").arg(int(std::lround(converted.metalCoverage * 100.0f))));
}

// Shared parameter block for both conversion profiles.
QVector<ParamSpec> sharedMathParams()
{
    return {
        floatParam("glossCurve", "Gloss curve", 1.0, 0.25, 4.0, 0.05,
            "Response curve from glossiness/smoothness to roughness. 1 = linear invert."),
        floatParam("dielectricF0Min", "Dielectric F0 min", 0.02, 0.0, 0.2, 0.005),
        floatParam("dielectricF0Max", "Dielectric F0 max", 0.08, 0.0, 0.3, 0.005),
    };
}

} // namespace

void registerAll(Registry& registry)
{
    // ---------------- Source ----------------

    {
        NodeTypeSpec spec;
        spec.id = QStringLiteral("source.material");
        spec.name = QStringLiteral("Source Material");
        spec.category = QStringLiteral("Source");
        spec.summary = QStringLiteral("Parses the .bgsm/.bgem being converted.");
        spec.outputs = {materialPort("material", "Material"),
            PortSpec{QStringLiteral("profile"), QStringLiteral("Profile"), ValueType::String, {}, false}};
        spec.params = {ParamSpec{QStringLiteral("path"), QStringLiteral("Override path"),
            ParamSpec::Kind::Path, 0, 0, 0, QString(), {},
            QStringLiteral("Leave empty to convert whatever the queue selected.")}};
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            const QString override = paramString(args.params, "path").trimmed();
            const QString path = override.isEmpty() ? args.context.sourcePath : override;
            if (path.isEmpty()) { output->error = QStringLiteral("No source material."); return; }
            auto material = std::make_shared<Bgsm::Material>();
            QString error;
            if (!Bgsm::readFile(path, material.get(), &error)) {
                output->error = error.isEmpty() ? QStringLiteral("Could not read the material.") : error;
                return;
            }
            const bool fo76 = material->version > 2;
            output->outputs.insert(QStringLiteral("material"),
                Value::fromOpaque(QLatin1String(kMaterialTag), std::static_pointer_cast<void>(material)));
            output->outputs.insert(QStringLiteral("profile"),
                Value::fromString(fo76 ? QStringLiteral("FO76") : QStringLiteral("FO4")));
            output->notes.append(QStringLiteral("%1 v%2 — %3")
                .arg(fo76 ? QStringLiteral("FO76") : QStringLiteral("FO4"))
                .arg(material->version).arg(QFileInfo(path).fileName()));
        };
        registry.add(spec);
    }

    {
        static const QStringList slotLabels{
            QStringLiteral("Diffuse"), QStringLiteral("Normal"), QStringLiteral("Smooth/Spec (FO4 _s)"),
            QStringLiteral("Reflectance (FO76 _r)"), QStringLiteral("Lighting (FO76 _l)"),
            QStringLiteral("Glow"), QStringLiteral("Greyscale"), QStringLiteral("Env Mask"),
            QStringLiteral("Wrinkles"), QStringLiteral("Flow"), QStringLiteral("Inner Layer")};
        NodeTypeSpec spec;
        spec.id = QStringLiteral("source.texture");
        spec.name = QStringLiteral("Source Texture");
        spec.category = QStringLiteral("Source");
        spec.summary = QStringLiteral("Loads one of the material's authored texture slots.");
        spec.inputs = {materialPort("material", "Material")};
        spec.outputs = {imagePort("image", "Image"),
            PortSpec{QStringLiteral("path"), QStringLiteral("Path"), ValueType::String, {}, false}};
        spec.params = {enumParam("slot", "Slot", 0, slotLabels)};
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            const Bgsm::Material* material = materialInput(args, output);
            if (!material) return;
            const int slot = paramInt(args.params, "slot", 0);
            QString authored;
            switch (slot) {
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
            output->outputs.insert(QStringLiteral("path"), Value::fromString(authored));
            if (authored.trimmed().isEmpty()) {
                // Unauthored slots are normal, not errors: downstream nodes
                // treat a null image as "absent" exactly like the orchestrator.
                output->outputs.insert(QStringLiteral("image"), Value::fromImage(QImage()));
                output->notes.append(QStringLiteral("slot not authored"));
                return;
            }
            QString error;
            QImage image = TextureImage::load(authored, args.context.textureRoot, &error);
            if (image.isNull()) {
                output->outputs.insert(QStringLiteral("image"), Value::fromImage(image));
                output->notes.append(QStringLiteral("missing: %1").arg(authored));
                return;
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

    {
        NodeTypeSpec spec;
        spec.id = QStringLiteral("convert.fo4");
        spec.name = QStringLiteral("FO4 Conversion");
        spec.category = QStringLiteral("Inference");
        spec.summary = QStringLiteral("Metallic inference and metal albedo recovery for Fallout 4 sources.");
        spec.inputs = {materialPort("material", "Material"), imagePort("diffuse", "Diffuse"),
            imagePort("specular", "Specular (_s)", true), imagePort("glow", "Glow", true),
            imagePort("envMask", "Env Mask", true), imagePort("correctionMask", "Correction Mask", true),
            colorPort("cubemapTint", "Cubemap Tint", true)};
        spec.outputs = {imagePort("baseColor", "Base Colour"), imagePort("rmaos", "RMAOS"),
            imagePort("emissive", "Emissive"), imagePort("metallic", "Metallic"),
            scalarPort("metalCoverage", "Metal Coverage")};
        spec.params = sharedMathParams();
        spec.params.prepend(floatParam("recovery", "Metal recovery", 1.0, 0.0, 1.0, 0.05,
            "How strongly dark vanilla metals are rebuilt toward their true albedo."));
        spec.params.append(boolParam("normalizeSpec", "Normalize spec", true,
            "Percentile-tame blown-out vanilla spec maps."));
        spec.params.append(floatParam("specPercentile", "Spec clamp %", 0.98, 0.5, 1.0, 0.01));
        spec.params.append(floatParam("metalSpecStart", "Metal spec start", 0.25, 0.0, 1.0, 0.01));
        spec.params.append(floatParam("metalSpecFull", "Metal spec full", 0.65, 0.0, 1.0, 0.01));
        spec.params.append(floatParam("metalDarkStart", "Metal dark start", 0.35, 0.0, 1.0, 0.01));
        spec.params.append(floatParam("metalDarkFull", "Metal dark full", 0.12, 0.0, 1.0, 0.01));
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            const Bgsm::Material* material = materialInput(args, output);
            if (!material) return;
            Converter::Fo4Sources sources;
            sources.diffuse = args.inputs.value(QStringLiteral("diffuse")).image();
            sources.specular = args.inputs.value(QStringLiteral("specular")).image();
            sources.glow = args.inputs.value(QStringLiteral("glow")).image();
            sources.envMask = args.inputs.value(QStringLiteral("envMask")).image();
            sources.correctionMask = args.inputs.value(QStringLiteral("correctionMask")).image();
            const Value tint = args.inputs.value(QStringLiteral("cubemapTint"));
            if (tint.isValid()) { sources.cubemapTint = tint.color(); sources.hasCubemapTint = true; }
            if (sources.diffuse.isNull()) { output->error = QStringLiteral("Diffuse is required."); return; }
            addConvertedOutputs(Converter::convertFo4(sources, *material, mathOptions(args.params)), output);
        };
        registry.add(spec);
    }

    {
        NodeTypeSpec spec;
        spec.id = QStringLiteral("convert.fo76");
        spec.name = QStringLiteral("FO76 Conversion");
        spec.category = QStringLiteral("Inference");
        spec.summary = QStringLiteral("Specular->metalness reconstruction for Fallout 76 sources.");
        spec.inputs = {materialPort("material", "Material"), imagePort("diffuse", "Diffuse"),
            imagePort("reflectance", "Reflectance (_r)", true), imagePort("lighting", "Lighting (_l)", true),
            imagePort("glow", "Glow", true), imagePort("correctionMask", "Correction Mask", true)};
        spec.outputs = {imagePort("baseColor", "Base Colour"), imagePort("rmaos", "RMAOS"),
            imagePort("emissive", "Emissive"), imagePort("metallic", "Metallic"),
            scalarPort("metalCoverage", "Metal Coverage")};
        spec.params = sharedMathParams();
        spec.params.append(floatParam("fo76ReflectanceStart", "Reflectance start", 0.12, 0.0, 1.0, 0.01));
        spec.params.append(floatParam("fo76ReflectanceFull", "Reflectance full", 0.40, 0.0, 1.0, 0.01));
        spec.params.append(floatParam("fo76DarkStart", "Diffuse dark start", 0.30, 0.0, 1.0, 0.01));
        spec.params.append(floatParam("fo76DarkFull", "Diffuse dark full", 0.08, 0.0, 1.0, 0.01));
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            const Bgsm::Material* material = materialInput(args, output);
            if (!material) return;
            Converter::Fo76Sources sources;
            sources.diffuse = args.inputs.value(QStringLiteral("diffuse")).image();
            sources.reflectance = args.inputs.value(QStringLiteral("reflectance")).image();
            sources.lighting = args.inputs.value(QStringLiteral("lighting")).image();
            sources.glow = args.inputs.value(QStringLiteral("glow")).image();
            sources.correctionMask = args.inputs.value(QStringLiteral("correctionMask")).image();
            if (sources.diffuse.isNull()) { output->error = QStringLiteral("Diffuse is required."); return; }
            addConvertedOutputs(Converter::convertFo76(sources, *material, mathOptions(args.params)), output);
        };
        registry.add(spec);
    }

    // ---------------- Derive ----------------

    {
        NodeTypeSpec spec;
        spec.id = QStringLiteral("derive.normal");
        spec.name = QStringLiteral("Normal Derivations");
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
        spec.name = QStringLiteral("Channel Pack");
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
        spec.name = QStringLiteral("Uniform Colour");
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
        spec.summary = QStringLiteral("Input/output range remap with gamma.");
        spec.inputs = {imagePort("image", "Image")};
        spec.outputs = {imagePort("image", "Image")};
        spec.params = {floatParam("inBlack", "In black", 0.0, 0.0, 1.0),
            floatParam("inWhite", "In white", 1.0, 0.0, 1.0),
            floatParam("gamma", "Gamma", 1.0, 0.1, 5.0, 0.05),
            floatParam("outBlack", "Out black", 0.0, 0.0, 1.0),
            floatParam("outWhite", "Out white", 1.0, 0.0, 1.0)};
        spec.evaluate = [](const EvalArgs& args, EvalOutput* output) {
            QImage image = toRgba(args.inputs.value(QStringLiteral("image")).image());
            if (image.isNull()) { output->error = QStringLiteral("Image is required."); return; }
            const float inBlack = paramFloat(args.params, "inBlack", 0.0f);
            const float inWhite = paramFloat(args.params, "inWhite", 1.0f);
            const float gamma = std::max(paramFloat(args.params, "gamma", 1.0f), 0.001f);
            const float outBlack = paramFloat(args.params, "outBlack", 0.0f);
            const float outWhite = paramFloat(args.params, "outWhite", 1.0f);
            const float span = std::abs(inWhite - inBlack) < 1e-5f ? 1e-5f : inWhite - inBlack;
            uchar table[256];
            for (int i = 0; i < 256; ++i) {
                float value = std::clamp((i / 255.0f - inBlack) / span, 0.0f, 1.0f);
                value = std::pow(value, 1.0f / gamma);
                table[i] = uchar(std::lround(std::clamp(outBlack + value * (outWhite - outBlack), 0.0f, 1.0f) * 255.0f));
            }
            for (int y = 0; y < image.height(); ++y) {
                uchar* line = image.scanLine(y);
                for (int x = 0; x < image.width(); ++x)
                    for (int i = 0; i < 3; ++i) line[x * 4 + i] = table[line[x * 4 + i]];
            }
            output->outputs.insert(QStringLiteral("image"), Value::fromImage(image));
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
        static const QStringList roleLabels{QStringLiteral("Base Colour"), QStringLiteral("Normal"),
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

Graph defaultFo4Graph()
{
    Builder builder(defaultRegistry());
    const QString material = builder.add("source.material", 0, 200);
    const QString diffuse = builder.add("source.texture", 220, 40, {{QStringLiteral("slot"), 0}});
    const QString normal = builder.add("source.texture", 220, 150, {{QStringLiteral("slot"), 1}});
    const QString specular = builder.add("source.texture", 220, 260, {{QStringLiteral("slot"), 2}});
    const QString glow = builder.add("source.texture", 220, 370, {{QStringLiteral("slot"), 5}});
    const QString envMask = builder.add("source.texture", 220, 480, {{QStringLiteral("slot"), 7}});
    const QString tint = builder.add("source.cubemapTint", 220, 590);
    const QString convert = builder.add("convert.fo4", 480, 200);
    const QString derive = builder.add("derive.normal", 480, 520);

    builder.link(material, "material", diffuse, "material");
    builder.link(material, "material", normal, "material");
    builder.link(material, "material", specular, "material");
    builder.link(material, "material", glow, "material");
    builder.link(material, "material", envMask, "material");
    builder.link(material, "material", tint, "material");
    builder.link(material, "material", convert, "material");
    builder.link(diffuse, "image", convert, "diffuse");
    builder.link(specular, "image", convert, "specular");
    builder.link(glow, "image", convert, "glow");
    builder.link(envMask, "image", convert, "envMask");
    builder.link(tint, "tint", convert, "cubemapTint");
    builder.link(normal, "image", derive, "normal");

    // RMAOS.B carries the detail AO for FO4 sources (FO76 keeps its baked _l.G).
    const QString rmaosRough = builder.add("image.channel", 760, 380, {{QStringLiteral("channel"), 0}});
    const QString rmaosMetal = builder.add("image.channel", 760, 470, {{QStringLiteral("channel"), 1}});
    const QString rmaosF0 = builder.add("image.channel", 760, 560, {{QStringLiteral("channel"), 3}});
    const QString pack = builder.add("image.channelPack", 1000, 440);
    builder.link(convert, "rmaos", rmaosRough, "image");
    builder.link(convert, "rmaos", rmaosMetal, "image");
    builder.link(convert, "rmaos", rmaosF0, "image");
    builder.link(rmaosRough, "image", pack, "r");
    builder.link(rmaosMetal, "image", pack, "g");
    builder.link(derive, "ao", pack, "b");
    builder.link(rmaosF0, "image", pack, "a");

    const QString outBase = builder.add("output.texture", 1000, 120,
        {{QStringLiteral("role"), 0}, {QStringLiteral("suffix"), QStringLiteral("_BC")}});
    const QString outNormal = builder.add("output.texture", 1000, 230,
        {{QStringLiteral("role"), 1}, {QStringLiteral("suffix"), QStringLiteral("_N")}});
    const QString outRmaos = builder.add("output.texture", 1240, 440,
        {{QStringLiteral("role"), 2}, {QStringLiteral("suffix"), QStringLiteral("_RMAOS")}});
    const QString outEmissive = builder.add("output.texture", 1000, 330,
        {{QStringLiteral("role"), 3}, {QStringLiteral("suffix"), QStringLiteral("_E")}});
    builder.link(convert, "baseColor", outBase, "image");
    builder.link(normal, "image", outNormal, "image");
    builder.link(pack, "image", outRmaos, "image");
    builder.link(convert, "emissive", outEmissive, "image");
    return builder.graph;
}

Graph defaultFo76Graph()
{
    Builder builder(defaultRegistry());
    const QString material = builder.add("source.material", 0, 200);
    const QString diffuse = builder.add("source.texture", 220, 40, {{QStringLiteral("slot"), 0}});
    const QString normal = builder.add("source.texture", 220, 150, {{QStringLiteral("slot"), 1}});
    const QString reflectance = builder.add("source.texture", 220, 260, {{QStringLiteral("slot"), 3}});
    const QString lighting = builder.add("source.texture", 220, 370, {{QStringLiteral("slot"), 4}});
    const QString glow = builder.add("source.texture", 220, 480, {{QStringLiteral("slot"), 5}});
    const QString convert = builder.add("convert.fo76", 480, 200);
    const QString derive = builder.add("derive.normal", 480, 520);

    builder.link(material, "material", diffuse, "material");
    builder.link(material, "material", normal, "material");
    builder.link(material, "material", reflectance, "material");
    builder.link(material, "material", lighting, "material");
    builder.link(material, "material", glow, "material");
    builder.link(material, "material", convert, "material");
    builder.link(diffuse, "image", convert, "diffuse");
    builder.link(reflectance, "image", convert, "reflectance");
    builder.link(lighting, "image", convert, "lighting");
    builder.link(glow, "image", convert, "glow");
    builder.link(normal, "image", derive, "normal");

    const QString outBase = builder.add("output.texture", 800, 120,
        {{QStringLiteral("role"), 0}, {QStringLiteral("suffix"), QStringLiteral("_BC")}});
    const QString outNormal = builder.add("output.texture", 800, 230,
        {{QStringLiteral("role"), 1}, {QStringLiteral("suffix"), QStringLiteral("_N")}});
    const QString outRmaos = builder.add("output.texture", 800, 340,
        {{QStringLiteral("role"), 2}, {QStringLiteral("suffix"), QStringLiteral("_RMAOS")}});
    const QString outEmissive = builder.add("output.texture", 800, 450,
        {{QStringLiteral("role"), 3}, {QStringLiteral("suffix"), QStringLiteral("_E")}});
    builder.link(convert, "baseColor", outBase, "image");
    builder.link(normal, "image", outNormal, "image");
    builder.link(convert, "rmaos", outRmaos, "image");
    builder.link(convert, "emissive", outEmissive, "image");
    return builder.graph;
}

Graph defaultGraphFor(const Bgsm::Material& material)
{
    return material.version > 2 ? defaultFo76Graph() : defaultFo4Graph();
}

} // namespace ConverterNodes
