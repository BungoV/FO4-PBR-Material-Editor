#include "upscaler.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>

namespace Upscaler {
namespace {

constexpr auto kBinaryKey = "Upscaler/binaryPath";
constexpr auto kSlotModelPrefix = "Upscaler/model/";
constexpr auto kModelsKey = "Upscaler/modelsPath";
constexpr auto kRestoreKey = "Upscaler/restorationModel";

// Lanczos-3: three lobes is the usual quality/ringing compromise, and it is
// what image editors mean by "Lanczos" without further qualification.
constexpr int kLobes = 3;

double lanczosWeight(double x)
{
    if (x == 0.0) return 1.0;
    const double a = double(kLobes);
    if (x <= -a || x >= a) return 0.0;
    const double pix = M_PI * x;
    return (a * std::sin(pix) * std::sin(pix / a)) / (pix * pix);
}

// One separable pass. Weights are built per output column/row rather than per
// texel: the kernel only depends on the sub-texel phase, and at 4x that is a
// few thousand weights instead of a few hundred million.
QImage resampleAxis(const QImage& source, int targetLength, bool horizontal)
{
    const int sourceLength = horizontal ? source.width() : source.height();
    const int otherLength = horizontal ? source.height() : source.width();
    if (sourceLength <= 0 || targetLength <= 0) return {};
    QImage result(horizontal ? targetLength : source.width(),
        horizontal ? source.height() : targetLength, QImage::Format_RGBA8888);

    const double scale = double(targetLength) / double(sourceLength);
    // Downscaling widens the kernel so the filter averages the texels it is
    // discarding; upscaling keeps the unit kernel and interpolates.
    const double support = scale < 1.0 ? double(kLobes) / scale : double(kLobes);
    const double step = scale < 1.0 ? scale : 1.0;

    for (int out = 0; out < targetLength; ++out) {
        const double center = (out + 0.5) / scale - 0.5;
        const int first = int(std::ceil(center - support));
        const int last = int(std::floor(center + support));
        QVarLengthArray<double, 32> weights;
        double total = 0;
        for (int tap = first; tap <= last; ++tap) {
            const double weight = lanczosWeight((tap - center) * step);
            weights.append(weight);
            total += weight;
        }
        if (total == 0) total = 1;

        for (int other = 0; other < otherLength; ++other) {
            double sums[4] = {0, 0, 0, 0};
            for (int tap = first; tap <= last; ++tap) {
                const int clamped = std::clamp(tap, 0, sourceLength - 1);
                const uchar* texel = horizontal
                    ? source.constScanLine(other) + clamped * 4
                    : source.constScanLine(clamped) + other * 4;
                const double weight = weights[tap - first];
                for (int c = 0; c < 4; ++c) sums[c] += weight * texel[c];
            }
            uchar* target = horizontal
                ? result.scanLine(other) + out * 4
                : result.scanLine(out) + other * 4;
            for (int c = 0; c < 4; ++c)
                target[c] = uchar(std::clamp(std::lround(sums[c] / total), 0L, 255L));
        }
    }
    return result;
}

} // namespace

QString binaryPath() { return QSettings().value(QString::fromLatin1(kBinaryKey)).toString(); }

void setBinaryPath(const QString& path)
{
    QSettings().setValue(QString::fromLatin1(kBinaryKey), path);
}

bool available()
{
    const QString path = binaryPath();
    return !path.isEmpty() && QFileInfo(path).isExecutable();
}

QString modelsPath()
{
    const QString pinned = QSettings().value(QString::fromLatin1(kModelsKey)).toString();
    if (!pinned.isEmpty()) return pinned;
    const QString binary = binaryPath();
    if (binary.isEmpty()) return {};
    return QFileInfo(binary).absolutePath() + QStringLiteral("/models");
}

void setModelsPath(const QString& path)
{
    QSettings().setValue(QString::fromLatin1(kModelsKey), path);
}

void clearModelsPath()
{
    QSettings().remove(QString::fromLatin1(kModelsKey));
}

bool modelsPathIsPinned()
{
    return !QSettings().value(QString::fromLatin1(kModelsKey)).toString().isEmpty();
}

QStringList models()
{
    // The binary still has to exist — a models folder full of weights with
    // nothing to run them is not an install.
    if (!available()) return {};
    // ncnn ships each model as a .param/.bin pair; the .param basename is the
    // name the binary wants for -n. A .param with no .bin beside it is a broken
    // half-copy and is skipped rather than offered and then failing at run time.
    const QDir folder(modelsPath());
    QStringList found;
    for (const QFileInfo& entry : folder.entryInfoList({QStringLiteral("*.param")}, QDir::Files, QDir::Name)) {
        if (!QFileInfo::exists(folder.filePath(entry.completeBaseName() + QStringLiteral(".bin"))))
            continue;
        found.append(entry.completeBaseName());
    }
    return found;
}

QVector<MapSlot> allSlots()
{
    return {MapSlot::BaseColor, MapSlot::Reflectance, MapSlot::SpecularGloss, MapSlot::Normal,
        MapSlot::Smoothness, MapSlot::AmbientOcclusion, MapSlot::Subsurface, MapSlot::Glow,
        MapSlot::Other};
}

QString slotKey(MapSlot slot)
{
    switch (slot) {
    case MapSlot::BaseColor: return QStringLiteral("baseColor");
    case MapSlot::Reflectance: return QStringLiteral("reflectance");
    case MapSlot::SpecularGloss: return QStringLiteral("specularGloss");
    case MapSlot::Normal: return QStringLiteral("normal");
    case MapSlot::Smoothness: return QStringLiteral("smoothness");
    case MapSlot::AmbientOcclusion: return QStringLiteral("ambientOcclusion");
    case MapSlot::Subsurface: return QStringLiteral("subsurface");
    case MapSlot::Glow: return QStringLiteral("glow");
    case MapSlot::Other: break;
    }
    return QStringLiteral("other");
}

QString slotLabel(MapSlot slot)
{
    switch (slot) {
    case MapSlot::BaseColor: return QObject::tr("Base Colour / Diffuse (_d)");
    case MapSlot::Reflectance: return QObject::tr("Reflectance (_r)");
    case MapSlot::SpecularGloss: return QObject::tr("Specular / Gloss (_s)");
    case MapSlot::Normal: return QObject::tr("Normal (_n)");
    case MapSlot::Smoothness: return QObject::tr("Smoothness / Roughness");
    case MapSlot::AmbientOcclusion: return QObject::tr("Ambient Occlusion");
    case MapSlot::Subsurface: return QObject::tr("Subsurface");
    case MapSlot::Glow: return QObject::tr("Glow / Emissive (_g)");
    case MapSlot::Other: break;
    }
    return QObject::tr("Other / mask");
}

MapKind kindForSlot(MapSlot slot)
{
    switch (slot) {
    case MapSlot::Normal: return MapKind::Normal;
    // Pictures: safe to hand a photo-trained model, and no channel needs
    // rebuilding afterwards.
    case MapSlot::BaseColor:
    case MapSlot::Glow: return MapKind::Color;
    // Reflectance is a colour map by handling — it carries a metal's hue — but
    // it is listed here rather than with the pictures because its own row in
    // the model table is the point of the split.
    case MapSlot::Reflectance: return MapKind::Color;
    default: break;
    }
    return MapKind::Data;
}

MapSlot slotForLabel(const QString& label)
{
    const QString lowered = label.toLower();
    // Ordered by specificity: "reflectance" must win before the colour words,
    // and "specular" before it collapses into reflectance.
    if (lowered.contains(QStringLiteral("normal"))) return MapSlot::Normal;
    if (lowered.contains(QStringLiteral("reflect"))) return MapSlot::Reflectance;
    if (lowered.contains(QStringLiteral("specular")) || lowered.contains(QStringLiteral("gloss")))
        return MapSlot::SpecularGloss;
    if (lowered.contains(QStringLiteral("smooth")) || lowered.contains(QStringLiteral("rough")))
        return MapSlot::Smoothness;
    if (lowered.contains(QStringLiteral("occlusion")) || lowered.contains(QStringLiteral("ao")))
        return MapSlot::AmbientOcclusion;
    if (lowered.contains(QStringLiteral("subsurface")) || lowered.contains(QStringLiteral("scatter")))
        return MapSlot::Subsurface;
    if (lowered.contains(QStringLiteral("glow")) || lowered.contains(QStringLiteral("emis")))
        return MapSlot::Glow;
    if (lowered.contains(QStringLiteral("diffuse")) || lowered.contains(QStringLiteral("colour"))
        || lowered.contains(QStringLiteral("color")) || lowered.contains(QStringLiteral("base")))
        return MapSlot::BaseColor;
    return MapSlot::Other;
}

int modelNativeScale(const QString& model)
{
    // The scale is in the name by convention (x4, 4x, -x2). Checked largest
    // first so "x4plus" does not match a 2. 1 is included because restoration
    // models are real: "1x-BCGone-Detailed" removes BC block artifacts without
    // changing the size at all, and reading it as scale-0 meant it could never
    // run.
    for (const int candidate : {4, 3, 2, 1}) {
        const QString digit = QString::number(candidate);
        if (model.contains(QStringLiteral("x") + digit, Qt::CaseInsensitive)
            || model.contains(digit + QStringLiteral("x"), Qt::CaseInsensitive))
            return candidate;
    }
    return 0;
}

QString suggestedModelForSlot(MapSlot slot)
{
    return chooseModel(slot, models());
}

QString chooseModel(MapSlot slot, const QStringList& available)
{
    if (available.isEmpty()) return {};
    // Preference lists per role, most specific first. A normal-trained model is
    // the one thing that makes neural upscaling unambiguously correct on a
    // vector field, so it is matched by several spellings.
    QStringList preferred;
    switch (slot) {
    case MapSlot::Normal:
        preferred = {QStringLiteral("normal"), QStringLiteral("rg0")};
        break;
    case MapSlot::BaseColor:
    case MapSlot::Glow:
        preferred = {QStringLiteral("photo"), QStringLiteral("realesrgan-x4plus"),
            QStringLiteral("realesr-general"), QStringLiteral("omni")};
        break;
    default:
        // Reflectance and every data map: general-purpose models, never
        // photographic or anime ones. These are not pictures, and a model
        // trained to invent plausible photo detail invents shading — or, in
        // reflectance's case, metal — that was never authored.
        // "realesr-general-x4" before the bare "general": the same install
        // usually also ships a -wdn- denoising variant that sorts first by
        // name, and softening a data map is not what was asked for.
        // "conservative" is NOT a hint here, tempting as it reads: it matches
        // Photo-Conservative, which is exactly the class being avoided.
        preferred = {QStringLiteral("realesr-general-x4"), QStringLiteral("realesr-general"),
            QStringLiteral("general"), QStringLiteral("universal"), QStringLiteral("omni")};
        break;
    }
    const bool picture = slot == MapSlot::BaseColor || slot == MapSlot::Glow;
    const auto isAnime = [](const QString& name) {
        return name.contains(QStringLiteral("anime"), Qt::CaseInsensitive);
    };
    // Anime variants are held back rather than matched in place. A hint is a
    // substring, and "realesrgan-x4plus" is a substring of
    // "realesrgan-x4plus-anime" — which sorts FIRST, because the listing
    // compares full filenames and '-' precedes '.'. Taking the first hit
    // therefore handed base colour the anime model while the plain one sat
    // right beside it. A non-anime match on ANY later hint beats an anime match
    // on an earlier one.
    QString animeMatch;
    for (const QString& hint : preferred)
        for (const QString& candidate : available) {
            if (!candidate.contains(hint, Qt::CaseInsensitive)) continue;
            if (!isAnime(candidate)) return candidate;
            if (animeMatch.isEmpty()) animeMatch = candidate;
        }
    if (picture && !animeMatch.isEmpty()) return animeMatch;

    // Nothing matched by name, so the fallback is whatever is installed — but
    // not blindly. The official ncnn release ships no models at all, so a real
    // install is whatever the user assembled, and an anime-trained network is
    // the worst thing to hand a data map or a reflectance map: it is built to
    // invent clean line art out of gradients. Better to resample than to
    // hallucinate cel shading into a roughness map.
    for (const QString& candidate : available) {
        if (slot == MapSlot::Normal) break; // handled below
        if (!picture && isAnime(candidate)) continue;
        return candidate;
    }
    // A normal declines outright rather than borrowing a picture model: those
    // bend the vectors in ways reconstructBlue cannot undo. A data role with
    // nothing but anime models installed declines for the same reason.
    return {};
}

QString suggestedRestorationModel()
{
    const QStringList available = models();
    if (available.isEmpty()) return {};
    // Names the compression-removal family actually uses. Deliberately narrow:
    // a general upscaler pressed into a 1:1 pass is not a de-blocker, and
    // guessing one here would run a network that changes the image for no
    // stated reason.
    const QStringList preferred{QStringLiteral("bcgone"), QStringLiteral("dxtdecompressor"),
        QStringLiteral("dxtless"), QStringLiteral("bc1"), QStringLiteral("bc7"),
        QStringLiteral("deblock"), QStringLiteral("dds")};
    for (const QString& hint : preferred)
        for (const QString& candidate : available)
            if (candidate.contains(hint, Qt::CaseInsensitive)
                && modelNativeScale(candidate) == 1) return candidate;
    // Nothing installed for the job: restoration stays off rather than picking
    // an arbitrary network to run over every texture.
    return {};
}

QString restorationModel()
{
    QSettings settings;
    const QString key = QString::fromLatin1(kRestoreKey);
    if (settings.contains(key)) {
        const QString pinned = settings.value(key).toString();
        if (pinned.isEmpty()) return {};
        if (models().contains(pinned)) return pinned;
    }
    return suggestedRestorationModel();
}

void setRestorationModel(const QString& model)
{
    QSettings().setValue(QString::fromLatin1(kRestoreKey), model);
}

void clearRestorationModel()
{
    QSettings().remove(QString::fromLatin1(kRestoreKey));
}

bool restorationModelIsPinned()
{
    return QSettings().contains(QString::fromLatin1(kRestoreKey));
}

Result restore(const QImage& image, MapKind kind, const QString& model)
{
    Result result;
    if (image.isNull()) return result;
    const QString chosen = model.isEmpty() ? restorationModel() : model;
    if (chosen.isEmpty()) {
        // Untouched, not resampled: the size is not changing, so running
        // Lanczos over it would soften the image for nothing.
        result.image = image;
        result.engine = QStringLiteral("none");
        result.note = available() ? QStringLiteral("no restoration model set")
                                  : QStringLiteral("no upscaler configured");
        return result;
    }
    Request request;
    request.image = image;
    request.kind = kind;
    request.model = chosen;
    request.factor = 1;
    return upscale(request);
}

QString modelForSlot(MapSlot slot)
{
    QSettings settings;
    const QString key = QString::fromLatin1(kSlotModelPrefix) + slotKey(slot);
    if (settings.contains(key)) {
        // A pinned empty string is a deliberate "use Lanczos here", which is
        // different from never having chosen.
        const QString pinned = settings.value(key).toString();
        if (pinned.isEmpty()) return {};
        // A choice made against a different install must not silently run the
        // wrong network; fall back to the suggestion when it is gone.
        if (models().contains(pinned)) return pinned;
    }
    return suggestedModelForSlot(slot);
}

void setModelForSlot(MapSlot slot, const QString& model)
{
    QSettings().setValue(QString::fromLatin1(kSlotModelPrefix) + slotKey(slot), model);
}

void clearSlotModel(MapSlot slot)
{
    QSettings().remove(QString::fromLatin1(kSlotModelPrefix) + slotKey(slot));
}

bool slotModelIsPinned(MapSlot slot)
{
    return QSettings().contains(QString::fromLatin1(kSlotModelPrefix) + slotKey(slot));
}

QString defaultModel(MapKind kind)
{
    // Kept for callers that only know the handling class. Slots are the real
    // axis now, so this maps onto a representative one.
    switch (kind) {
    case MapKind::Normal: return modelForSlot(MapSlot::Normal);
    case MapKind::Color: return modelForSlot(MapSlot::BaseColor);
    case MapKind::Data: break;
    }
    return modelForSlot(MapSlot::Other);
}

QImage lanczos(const QImage& source, const QSize& target)
{
    if (source.isNull() || target.isEmpty()) return {};
    const QImage rgba = source.convertToFormat(QImage::Format_RGBA8888);
    if (rgba.size() == target) return rgba;
    // Horizontal then vertical; separable, so the order only affects the size
    // of the intermediate.
    return resampleAxis(resampleAxis(rgba, target.width(), true), target.height(), false);
}

QImage zeroBlue(const QImage& normal)
{
    QImage result = normal.convertToFormat(QImage::Format_RGBA8888);
    for (int y = 0; y < result.height(); ++y) {
        uchar* line = result.scanLine(y);
        for (int x = 0; x < result.width(); ++x) line[x * 4 + 2] = 0;
    }
    return result;
}

QImage reconstructBlue(const QImage& normal)
{
    QImage result = normal.convertToFormat(QImage::Format_RGBA8888);
    for (int y = 0; y < result.height(); ++y) {
        uchar* line = result.scanLine(y);
        for (int x = 0; x < result.width(); ++x) {
            uchar* texel = line + x * 4;
            double nx = texel[0] / 127.5 - 1.0, ny = texel[1] / 127.5 - 1.0;
            // A network is under no obligation to return a unit vector, and the
            // normal-trained models say so outright ("do not rely on this
            // network producing unit vectors"). An over-long XY cannot simply
            // have Z clamped to zero and be left alone: that keeps the too-long
            // XY and ships a non-unit normal, which is a shading error rather
            // than a cosmetic one. Rescale XY onto the unit circle instead, so
            // the texel becomes a real normal lying in the tangent plane.
            const double lengthSq = nx * nx + ny * ny;
            if (lengthSq > 1.0) {
                const double shrink = 1.0 / std::sqrt(lengthSq);
                nx *= shrink;
                ny *= shrink;
                texel[0] = uchar(std::clamp(std::lround((nx + 1.0) * 127.5), 0L, 255L));
                texel[1] = uchar(std::clamp(std::lround((ny + 1.0) * 127.5), 0L, 255L));
            }
            const double nz = std::sqrt(std::max(0.0, 1.0 - nx * nx - ny * ny));
            texel[2] = uchar(std::clamp(std::lround((nz + 1.0) * 127.5), 0L, 255L));
        }
    }
    return result;
}

Result upscale(const Request& request)
{
    Result result;
    if (request.image.isNull()) return result;
    const QSize target(request.image.width() * request.factor,
        request.image.height() * request.factor);

    // An explicit model wins; otherwise the standard-model table decides, which
    // is how a role chosen once in Settings reaches every conversion.
    const QString model = request.model.isEmpty() ? modelForSlot(request.slot) : request.model;
    // A model only runs at the scale it was trained for, and ncnn's -s must
    // match or the network shape is wrong. Run it natively and resample
    // whatever that overshoots back down afterwards; asking a 4x-only model for
    // -s 2 is how a working model looks broken.
    // Unnamed scale: trust the caller's request rather than guess.
    int nativeScale = modelNativeScale(model);
    if (nativeScale == 0) nativeScale = request.factor;
    // 1 is allowed alongside 2 and 4: a restoration model runs at 1:1, and the
    // whole point of it is that the size does not change. 3 is not — no ncnn
    // model here ships at that scale, and letting it through would run a
    // network at a size it was not trained for.
    const bool neuralPossible = available() && !model.isEmpty()
        && (nativeScale == 1 || nativeScale == 2 || nativeScale == 4);
    if (!neuralPossible) {
        result.image = lanczos(request.image, target);
        result.engine = QStringLiteral("Lanczos");
        if (!available()) result.note = QStringLiteral("no upscaler configured");
        else if (model.isEmpty()) result.note = request.kind == MapKind::Normal
            ? QStringLiteral("no normal-trained model installed")
            : QStringLiteral("%1 set to Lanczos").arg(slotLabel(request.slot));
        else result.note = QStringLiteral("%1 runs at %2x, which is not a model scale")
            .arg(model).arg(nativeScale);
        return result;
    }

    QTemporaryDir workspace;
    if (!workspace.isValid()) {
        result.image = lanczos(request.image, target);
        result.engine = QStringLiteral("Lanczos");
        result.note = QStringLiteral("no temporary folder");
        return result;
    }
    const QString inputPath = workspace.filePath(QStringLiteral("in.png"));
    const QString outputPath = workspace.filePath(QStringLiteral("out.png"));
    // A normal goes in as RG0, which is what a normal-trained model expects and
    // what stops the model treating a packed blue channel as picture content.
    const QImage prepared = request.kind == MapKind::Normal
        ? zeroBlue(request.image) : request.image.convertToFormat(QImage::Format_RGBA8888);
    if (!prepared.save(inputPath)) {
        result.image = lanczos(request.image, target);
        result.engine = QStringLiteral("Lanczos");
        result.note = QStringLiteral("could not stage the input");
        return result;
    }

    QProcess process;
    process.start(binaryPath(), {QStringLiteral("-i"), inputPath, QStringLiteral("-o"), outputPath,
        QStringLiteral("-n"), model, QStringLiteral("-s"), QString::number(nativeScale),
        QStringLiteral("-m"), modelsPath()});
    // Generous: a 2K map through a 4x model on a modest GPU is tens of seconds,
    // and a conversion run is not interactive.
    const bool finished = process.waitForFinished(180000);
    QImage produced;
    if (finished && process.exitCode() == 0) produced.load(outputPath);
    if (produced.isNull()) {
        result.image = lanczos(request.image, target);
        result.engine = QStringLiteral("Lanczos");
        result.note = finished ? QStringLiteral("%1 failed — fell back").arg(model)
                               : QStringLiteral("%1 timed out — fell back").arg(model);
        return result;
    }

    produced = produced.convertToFormat(QImage::Format_RGBA8888);
    // The model works at its own fixed scale; if that overshoots what was
    // asked for, Lanczos brings it back down, which is the cheap direction.
    if (produced.size() != target) produced = lanczos(produced, target);
    // Rebuild Z from the enlarged XY so the result is a unit vector again. The
    // model never saw a valid blue and cannot be trusted with one.
    if (request.kind == MapKind::Normal) produced = reconstructBlue(produced);
    // Alpha is data, not picture — models smear it. Carry the original's alpha
    // across at the new size instead of whatever came back.
    if (request.image.hasAlphaChannel()) {
        const QImage alphaSource = lanczos(request.image, target);
        for (int y = 0; y < produced.height(); ++y) {
            uchar* line = produced.scanLine(y);
            const uchar* from = alphaSource.constScanLine(y);
            for (int x = 0; x < produced.width(); ++x) line[x * 4 + 3] = from[x * 4 + 3];
        }
    }
    result.image = produced;
    result.engine = QStringLiteral("Real-ESRGAN %1").arg(model);
    return result;
}

} // namespace Upscaler
