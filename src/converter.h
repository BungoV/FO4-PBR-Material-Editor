#pragma once

#include "convertercore.h"
#include "normalderive.h"

#include <QHash>
#include <QString>
#include <QStringList>

// End-to-end conversion orchestration (docs/FO4-Conversion.md): resolves a
// material's textures, runs the profile math and derivations, writes the PBR
// texture set and the same-name sibling .pbrm into the output mod folder, and
// records everything in the run manifest. Geometry AO (reference-NIF bake)
// and correction masks arrive with the Convert workspace; this slice covers
// the automatic pipeline.
namespace Converter {

struct JobOptions {
    Options math;
    NormalDerive::Options derive;
    QString outputRoot;        // the output mod folder (Materials/ + Textures/ grow inside)
    QString textureRoot;       // source resolution root; editor sources as fallback
    bool dryRun = false;
    bool detailAo = true;      // FO4 default on; FO76 keeps its shipped AO
    bool heightInBlue = false; // pack derived height into Normal.B
    bool writeMetallicDiagnostic = false;
};

// Dedup by input-set + options: shared pairings convert once, and a source
// texture that legitimately diverges across pairings gets a hash suffix
// instead of a silent overwrite.
struct TextureCache {
    QHash<QString, QString> baseColorBySignature;   // signature -> portable path
    QHash<QString, QString> rmaosBySignature;
    QHash<QString, QString> normalBySignature;
    QHash<QString, QString> emissiveBySignature;
    QHash<QString, QString> claimedOutputs;         // absolute output -> signature
};

struct JobResult {
    QString materialPath;
    QString pbrmPath;          // written (or would-be) sibling
    bool converted = false;
    bool flagged = false;      // wants manual review in the workspace
    float metalCoverage = 0.0f;
    QStringList writtenTextures; // portable paths under <out>/Textures (dedup-shared entries included)
    QStringList notes;
    QStringList errors;
};

JobResult convertMaterial(const QString& materialPath, const JobOptions& options, TextureCache* cache);

// Archive-direct conversion: `entryPath` is the material's path inside the
// BA2 (e.g. "materials/actors/x.bgsm"), `data` its extracted bytes. Textures
// resolve through the texture root's loose files and archives; the archive's
// own folder acts as the Data root for template chains.
JobResult convertMaterialData(const QString& archivePath, const QString& entryPath, const QByteArray& data,
    const JobOptions& options, TextureCache* cache);

} // namespace Converter
