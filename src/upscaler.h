#pragma once

#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

// Texture upscaling for maps that ship below the rest of their material.
//
// Two engines behind one call. A neural upscaler (Real-ESRGAN's ncnn/Vulkan
// build) when one is configured, because a model trained on the kind of map
// being enlarged reconstructs detail no resampler can invent; and a Lanczos-3
// resampler otherwise, which is the best answer available without a model and
// is what every data map should use anyway. Callers do not branch — they ask
// for a factor and a model and get the best result the machine can give.
namespace Upscaler {

// What the image means, which decides both the default model and the handling
// around it. A normal map is not a picture: its channels are a vector, and a
// model trained on photographs will happily produce a non-unit one.
enum class MapKind {
    Color,      // base colour, emissive — a picture, safe for a photo model
    Normal,     // tangent-space normal, RG carrying direction
    Data,       // roughness, AO, masks — resampled by default
};

// The map ROLE, which is what a model is chosen against. Deliberately finer
// than MapKind: kind decides the *handling* (zero and rebuild blue, whether to
// infer at all) while slot decides *which model runs*, and two slots sharing a
// kind can still want different models.
//
// Reflectance is the reason this split exists. It is a colour map by handling,
// but the metal window is derived from its histogram — so a model that invents
// detail there changes what converts as metal, not merely how it looks. It gets
// its own row so that decision is never inherited from base colour.
enum class MapSlot {
    BaseColor,        // _d diffuse / base colour
    Reflectance,      // FO76 _r — feeds the metal inference
    SpecularGloss,    // FO4 _s — R specular, G gloss
    Normal,           // _n
    Smoothness,       // FO76 _l.R
    AmbientOcclusion, // FO76 _l.G, baked AO
    Subsurface,       // FO76 _l.B
    Glow,             // _g emissive mask
    Other,            // anything unrecognised
};

QVector<MapSlot> allSlots();
// Stable settings key — never localised, so a translated build reads the same
// stored choices.
QString slotKey(MapSlot slot);
QString slotLabel(MapSlot slot);
MapKind kindForSlot(MapSlot slot);
// The slot a port label names, for the node's Auto setting: a graph says what a
// map is by what it is wired to, rather than needing a second declaration.
MapSlot slotForLabel(const QString& label);

// The model pinned for a slot, or the built-in suggestion when nothing is
// pinned. Empty means Lanczos, which is a legitimate answer and not a failure.
QString modelForSlot(MapSlot slot);
// Empty pins Lanczos explicitly; call clearSlotModel to go back to suggested.
void setModelForSlot(MapSlot slot, const QString& model);
void clearSlotModel(MapSlot slot);
bool slotModelIsPinned(MapSlot slot);
// What this build would choose for a slot with nothing pinned, given whatever
// models are installed.
QString suggestedModelForSlot(MapSlot slot);
// The preference match itself, over an arbitrary candidate list. Separated from
// the installed set so the ordering is testable on a machine with no upscaler —
// it is pure name-matching, and a hint in the wrong order picks a wrong-but-
// plausible model silently (a "conservative" hint once matched Photo-
// Conservative for every data map, which is the class it existed to avoid).
QString chooseModel(MapSlot slot, const QStringList& candidates);
// Restoration is a 1:1 pass that removes BC/DXT block artifacts without
// changing the size — the thing every map in this corpus actually suffers from,
// since they all arrive block-compressed. One setting rather than nine: BC
// artifacts are BC artifacts whatever the map means, so the model does not vary
// by role the way an upscaler does.
QString restorationModel();
void setRestorationModel(const QString& model);
void clearRestorationModel();
bool restorationModelIsPinned();
QString suggestedRestorationModel();

// Native scale read from a model's name (4, 3, 2, 1), or 0 when it does not say.
// The dialog shows it because a model only runs at the scale it was trained
// for, and everything else resamples afterwards.
int modelNativeScale(const QString& model);

// Absolute path to realesrgan-ncnn-vulkan.exe, or empty when unconfigured.
// Read from QSettings (Upscaler/binaryPath); the folder beside it is searched
// for models.
QString binaryPath();
void setBinaryPath(const QString& path);
bool available();

// Where the .param/.bin pairs live. Defaults to the `models` folder beside the
// binary, which is the ncnn convention — but only a convention: the official
// Real-ESRGAN release ships no models at all, so a working install is one the
// user assembled and the weights can be anywhere. Hardcoding the sibling folder
// made a perfectly good install report "no models found", which is
// indistinguishable from a broken one.
QString modelsPath();
void setModelsPath(const QString& path);
void clearModelsPath();          // back to the folder beside the binary
bool modelsPathIsPinned();
// Model names the configured install offers, from the .param files beside the
// binary. Empty when nothing is configured.
QStringList models();
// The model this kind of map should use when nothing is named explicitly.
// Data maps deliberately return empty: inventing detail in a roughness map
// invents shading that was never authored.
QString defaultModel(MapKind kind);

struct Request {
    QImage image;
    MapKind kind = MapKind::Color;
    // Which role this map plays. Resolves the model when `model` is empty,
    // which is how the standard-model table reaches a conversion.
    MapSlot slot = MapSlot::Other;
    QString model;       // empty asks for modelForSlot(slot)
    int factor = 2;      // 2 or 4; other values fall back to Lanczos
};

struct Result {
    QImage image;
    QString engine;      // "Real-ESRGAN <model>" or "Lanczos"
    QString note;        // why the neural path was not taken, when it was not
};

// Never fails: an unusable model or a missing binary degrades to Lanczos and
// says so in note. A null input returns a null image.
Result upscale(const Request& request);

// Restores at 1:1 using restorationModel(), or the named model. Falls back to
// returning the image UNTOUCHED — not to Lanczos, which would resample and
// soften for nothing when the size is not meant to change.
Result restore(const QImage& image, MapKind kind = MapKind::Color,
    const QString& model = QString());

// Separable Lanczos-3. Exposed because it is the honest default for data maps
// and the fallback everywhere else, and because it is worth testing directly.
QImage lanczos(const QImage& source, const QSize& target);

// A tangent-space normal has to survive the round trip as a unit vector. Blue
// is dropped going in — the model the user picked for normals (4x-Normal-RG0)
// is trained on RG0 input — and rebuilt from RG coming out, which also repairs
// whatever the model did to a channel it was never taught to preserve.
QImage zeroBlue(const QImage& normal);
QImage reconstructBlue(const QImage& normal);

} // namespace Upscaler
