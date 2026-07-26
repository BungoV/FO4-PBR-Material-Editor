#include "clicommands.h"

#include "converter.h"
#include "converternodes.h"
#include "geometryao.h"
#include "materialdocument.h"
#include "texturearchive.h"
#include "texturecache.h"
#include "textureimage.h"
#include "texturewriter.h"

#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSettings>
#include <QTextStream>

#include <cmath>

namespace PbrmCli {

namespace {

QTextStream& out()
{
    static QTextStream stream(stdout);
    return stream;
}

// Splits args into flags (with optional values) and positional paths.
struct Options {
    QStringList paths;
    QHash<QString, QString> flags;   // "--flag" -> value ("" for bare flags)
    bool has(const QString& flag) const { return flags.contains(flag); }
    QString value(const QString& flag, const QString& fallback = {}) const { return flags.value(flag, fallback); }
};

// Flags in this set consume the following argument as their value.
Options parseOptions(const QStringList& args, const QSet<QString>& valueFlags)
{
    Options options;
    for (int i = 0; i < args.size(); ++i) {
        const QString& arg = args[i];
        if (arg.startsWith(QStringLiteral("--"))) {
            if (valueFlags.contains(arg) && i + 1 < args.size()) options.flags.insert(arg, args[++i]);
            else options.flags.insert(arg, QString());
        } else {
            options.paths.append(arg);
        }
    }
    return options;
}

QString effectiveTextureRoot(const Options& options)
{
    if (options.has(QStringLiteral("--texture-root"))) return options.value(QStringLiteral("--texture-root"));
    // Fallback: the editor's configured sources (predictable CI machines pass
    // the flag explicitly).
    QSettings settings;
    QString root = settings.value(QStringLiteral("textureRoot")).toString();
    if (root.isEmpty())
        root = settings.value(QStringLiteral("TextureSources/modFolders")).toStringList().value(0);
    return root;
}

QStringList collectMaterialFiles(const QStringList& targets, QStringList* missing = nullptr)
{
    QStringList files;
    for (const QString& target : targets) {
        const QFileInfo info(target);
        if (info.isDir()) {
            QDirIterator it(info.absoluteFilePath(), {QStringLiteral("*.pbrm")}, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) files.append(it.next());
        } else if (info.isFile()) {
            files.append(info.absoluteFilePath());
        } else if (missing) {
            missing->append(target);
        }
    }
    return files;
}

// Reads the raw JSON payload of a .pbrm envelope (any accepted version).
QByteArray rawPayload(const QString& fileName)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    quint32 magic = 0, version = 0, size = 0;
    stream >> magic >> version >> size;
    if (magic != Pbr::MaterialDocument::Magic) return {};
    return file.read(size);
}

struct Finding {
    QString file;
    bool error = false;
    QString message;
};

// Prints findings as text lines or one JSON document; returns 1 if any error.
int reportFindings(const QVector<Finding>& findings, int fileCount, bool json, const QString& action)
{
    int errors = 0, warnings = 0;
    for (const Finding& finding : findings) (finding.error ? errors : warnings)++;
    if (json) {
        QJsonArray array;
        for (const Finding& finding : findings)
            array.append(QJsonObject{{QStringLiteral("file"), finding.file},
                {QStringLiteral("severity"), finding.error ? QStringLiteral("error") : QStringLiteral("warning")},
                {QStringLiteral("message"), finding.message}});
        const QJsonObject root{{QStringLiteral("action"), action}, {QStringLiteral("files"), fileCount},
            {QStringLiteral("errors"), errors}, {QStringLiteral("warnings"), warnings},
            {QStringLiteral("findings"), array}};
        out() << QJsonDocument(root).toJson(QJsonDocument::Indented) << Qt::flush;
    } else {
        for (const Finding& finding : findings)
            out() << finding.file << (finding.error ? ": error: " : ": warning: ") << finding.message << '\n';
        out() << fileCount << " file(s), " << errors << " error(s), " << warnings << " warning(s)\n" << Qt::flush;
    }
    return errors > 0 ? 1 : 0;
}

} // namespace

int runValidate(const QStringList& args)
{
    const Options options = parseOptions(args, {QStringLiteral("--texture-root")});
    const QString textureRoot = effectiveTextureRoot(options);
    QStringList missing;
    const QStringList files = collectMaterialFiles(options.paths, &missing);
    QVector<Finding> findings;
    for (const QString& target : missing) findings.append({target, true, QStringLiteral("path does not exist")});
    if (files.isEmpty() && missing.isEmpty()) {
        out() << "no .pbrm files found\n" << Qt::flush;
        return 1;
    }
    for (const QString& file : files) {
        Pbr::MaterialDocument document;
        QString loadError;
        if (!document.load(file, &loadError)) {
            findings.append({file, true, loadError});
            continue;
        }
        if (Pbr::hasLegacySurfaceStateGroups(document))
            findings.append({file, true, QStringLiteral("legacy per-material surface state groups; extract to a definitions JSON and strip them (pbrm extract-states + strip-legacy)")});
        for (const QString& setName : {QStringLiteral("UV0"), QStringLiteral("UV1"), QStringLiteral("UV2")}) {
            const QJsonObject set = document.textureSet(setName);
            for (auto it = set.constBegin(); it != set.constEnd(); ++it) {
                if (it.key().endsWith(QStringLiteral("SurfaceStateResponse")))
                    findings.append({file, true, setName + QStringLiteral(": retired Surface State Response input; repack R into RMAOS alpha and B into Normal alpha, then strip legacy surface data")});
                const QJsonObject slotValues = it.value().toObject().value(QStringLiteral("values")).toObject();
                if (it.key().endsWith(QStringLiteral("Normal")) && slotValues.value(QStringLiteral("layerMaskInAlpha")).toBool(false))
                    findings.append({file, false, setName + QLatin1Char('/') + it.key()
                        + QStringLiteral(": retired layer-modulation-in-alpha flag; repack the mask to a Layer Coverage texture or use Height Blend coverage")});
            }
        }
        const auto checkSet = [&](const QString& setName, const QJsonObject& set) {
            for (auto it = set.constBegin(); it != set.constEnd(); ++it) {
                const auto state = Pbr::TextureSlotState::fromJson(it.value().toObject());
                if (!state.enabled) continue;
                if (TextureCache::hasChannelSwaps(state.values))
                    findings.append({file, false, setName + QLatin1Char('/') + it.key()
                        + QStringLiteral(": unbaked channel sources; export a packed texture before runtime use")});
                if (state.path.trimmed().isEmpty()) continue;
                if (TextureImage::resolvePath(state.path, textureRoot).isEmpty()) {
                    findings.append({file, true, setName + QLatin1Char('/') + it.key()
                        + QStringLiteral(": texture cannot be resolved: ") + state.path});
                    continue;
                }
                const TextureImage::Diagnostics info = TextureImage::inspect(state.path, textureRoot);
                if (!info.valid)
                    findings.append({file, true, setName + QLatin1Char('/') + it.key() + QStringLiteral(": ") + info.error});
                else if (qMax(info.width, info.height) > 1 && info.mipLevels == 1)
                    findings.append({file, false, setName + QLatin1Char('/') + it.key() + QStringLiteral(": no mip chain")});
            }
        };
        for (const QString& set : {QStringLiteral("UV0"), QStringLiteral("UV1"), QStringLiteral("UV2")})
            checkSet(set, document.textureSet(set));
        const QJsonObject resources = document.toJson().value(QStringLiteral("resources")).toObject();
        for (auto it = resources.constBegin(); it != resources.constEnd(); ++it)
            checkSet(it.key(), it.value().toObject());
    }
    return reportFindings(findings, files.size(), options.has(QStringLiteral("--json")), QStringLiteral("validate"));
}

int runMigrate(const QStringList& args)
{
    const Options options = parseOptions(args, {});
    const bool dryRun = options.has(QStringLiteral("--dry-run"));
    const QStringList files = collectMaterialFiles(options.paths);
    if (files.isEmpty()) { out() << "no .pbrm files found\n" << Qt::flush; return 1; }
    QVector<Finding> findings;
    int migrated = 0;
    for (const QString& file : files) {
        Pbr::MaterialDocument document;
        QString error;
        if (!document.load(file, &error)) {
            findings.append({file, true, error});
            continue;
        }
        const QByteArray current = rawPayload(file);
        const QByteArray next = QJsonDocument(document.toJson()).toJson(QJsonDocument::Compact);
        if (current == next) continue;
        if (!dryRun && !document.save(file, &error)) {
            findings.append({file, true, QStringLiteral("save failed: ") + error});
            continue;
        }
        ++migrated;
        findings.append({file, false, dryRun ? QStringLiteral("would migrate to the current schema") : QStringLiteral("migrated to the current schema")});
    }
    if (!options.has(QStringLiteral("--json")))
        out() << migrated << (dryRun ? " file(s) would migrate\n" : " file(s) migrated\n");
    return reportFindings(findings, files.size(), options.has(QStringLiteral("--json")), QStringLiteral("migrate"));
}

int runExtractStates(const QStringList& args)
{
    const Options options = parseOptions(args, {});
    const QStringList files = collectMaterialFiles(options.paths);
    if (files.isEmpty()) { out() << "no .pbrm files found\n" << Qt::flush; return 1; }
    QVector<Finding> findings;
    for (const QString& file : files) {
        Pbr::MaterialDocument document;
        QString error;
        if (!document.load(file, &error)) {
            findings.append({file, true, error});
            continue;
        }
        if (!Pbr::hasLegacySurfaceStateGroups(document)) continue;
        const QFileInfo info(file);
        const QString target = info.absolutePath() + QLatin1Char('/') + info.completeBaseName() + QStringLiteral(".SurfaceStates.json");
        if (Pbr::extractLegacyStates(document, target, &error))
            findings.append({file, false, QStringLiteral("extracted legacy states to ") + target});
        else
            findings.append({file, true, QStringLiteral("extraction failed: ") + error});
    }
    return reportFindings(findings, files.size(), options.has(QStringLiteral("--json")), QStringLiteral("extract-states"));
}

int runStripLegacy(const QStringList& args)
{
    const Options options = parseOptions(args, {});
    const bool dryRun = options.has(QStringLiteral("--dry-run"));
    const QStringList files = collectMaterialFiles(options.paths);
    if (files.isEmpty()) { out() << "no .pbrm files found\n" << Qt::flush; return 1; }
    QVector<Finding> findings;
    for (const QString& file : files) {
        Pbr::MaterialDocument document;
        QString error;
        if (!document.load(file, &error)) {
            findings.append({file, true, error});
            continue;
        }
        const int removed = Pbr::stripLegacySurfaceData(document);
        if (removed == 0) continue;
        if (!dryRun && !document.save(file, &error)) {
            findings.append({file, true, QStringLiteral("save failed: ") + error});
            continue;
        }
        findings.append({file, false, (dryRun ? QStringLiteral("would strip ") : QStringLiteral("stripped "))
            + QString::number(removed) + QStringLiteral(" legacy surface item(s)")});
    }
    return reportFindings(findings, files.size(), options.has(QStringLiteral("--json")), QStringLiteral("strip-legacy"));
}

int runSetSetting(const QStringList& args)
{
    const Options options = parseOptions(args, {QStringLiteral("--section"), QStringLiteral("--key"), QStringLiteral("--value")});
    const QString section = options.value(QStringLiteral("--section"));
    const QString key = options.value(QStringLiteral("--key"));
    const QString raw = options.value(QStringLiteral("--value"));
    if (section.isEmpty() || key.isEmpty() || !options.flags.contains(QStringLiteral("--value"))) {
        out() << "usage: pbrm set-setting --section <Section> --key <key> --value <value> <files-or-folders...>\n" << Qt::flush;
        return 2;
    }
    QJsonValue value;
    bool isNumber = false;
    const double number = raw.toDouble(&isNumber);
    if (raw.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0) value = true;
    else if (raw.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0) value = false;
    else if (isNumber) value = number;
    else value = raw;
    const bool dryRun = options.has(QStringLiteral("--dry-run"));
    const QStringList files = collectMaterialFiles(options.paths);
    if (files.isEmpty()) { out() << "no .pbrm files found\n" << Qt::flush; return 1; }
    QVector<Finding> findings;
    for (const QString& file : files) {
        Pbr::MaterialDocument document;
        QString error;
        if (!document.load(file, &error)) {
            findings.append({file, true, error});
            continue;
        }
        const QJsonObject before = document.toJson();
        document.setSetting(section, key, value);
        if (document.toJson() == before) continue;
        if (!dryRun && !document.save(file, &error)) {
            findings.append({file, true, QStringLiteral("save failed: ") + error});
            continue;
        }
        findings.append({file, false, (dryRun ? QStringLiteral("would set ") : QStringLiteral("set "))
            + section + QLatin1Char('/') + key});
    }
    return reportFindings(findings, files.size(), options.has(QStringLiteral("--json")), QStringLiteral("set-setting"));
}

int runRepairPaths(const QStringList& args)
{
    const Options options = parseOptions(args, {QStringLiteral("--texture-root")});
    const QString textureRoot = effectiveTextureRoot(options);
    const bool dryRun = options.has(QStringLiteral("--dry-run"));
    const QStringList files = collectMaterialFiles(options.paths);
    if (files.isEmpty()) { out() << "no .pbrm files found\n" << Qt::flush; return 1; }
    const QVector<TextureImage::Resource> resources = TextureImage::enumerate(textureRoot);
    QVector<Finding> findings;
    for (const QString& file : files) {
        Pbr::MaterialDocument document;
        QString error;
        if (!document.load(file, &error)) {
            findings.append({file, true, error});
            continue;
        }
        const QJsonObject before = document.toJson();
        QStringList notes;
        for (const QString& setName : {QStringLiteral("UV0"), QStringLiteral("UV1"), QStringLiteral("UV2")}) {
            const QJsonObject set = document.textureSet(setName);
            for (auto slotIt = set.constBegin(); slotIt != set.constEnd(); ++slotIt) {
                auto state = Pbr::TextureSlotState::fromJson(slotIt.value().toObject());
                if (state.path.trimmed().isEmpty()) continue;
                if (!TextureImage::resolvePath(state.path, textureRoot).isEmpty()) continue;
                const QString fileName = QFileInfo(state.path).fileName();
                for (const TextureImage::Resource& resource : resources) {
                    if (QFileInfo(resource.path).fileName().compare(fileName, Qt::CaseInsensitive) != 0) continue;
                    notes << setName + QLatin1Char('/') + slotIt.key() + QStringLiteral(": ") + state.path
                        + QStringLiteral(" -> ") + resource.path;
                    state.path = resource.path;
                    document.setTextureSlot(setName, slotIt.key(), state);
                    break;
                }
            }
        }
        if (document.toJson() == before) continue;
        if (!dryRun && !document.save(file, &error)) {
            findings.append({file, true, QStringLiteral("save failed: ") + error});
            continue;
        }
        for (const QString& note : notes)
            findings.append({file, false, (dryRun ? QStringLiteral("would repair ") : QStringLiteral("repaired ")) + note});
    }
    return reportFindings(findings, files.size(), options.has(QStringLiteral("--json")), QStringLiteral("repair-paths"));
}

int runExportDefinitions(const QStringList& args)
{
    const Options options = parseOptions(args, {QStringLiteral("--out")});
    QString target = options.value(QStringLiteral("--out"));
    if (target.isEmpty() && !options.paths.isEmpty()) target = options.paths.first();
    if (target.isEmpty()) {
        out() << "usage: pbrm export-definitions <file.json>\n" << Qt::flush;
        return 2;
    }
    QString error;
    if (!Pbr::saveSurfaceStateDefinitions(target, Pbr::surfaceStatePresets(), &error)) {
        out() << "error: " << error << '\n' << Qt::flush;
        return 1;
    }
    out() << "wrote built-in surface state definitions to " << target << '\n' << Qt::flush;
    return 0;
}

int runExportJson(const QStringList& args)
{
    const Options options = parseOptions(args, {});
    const QStringList files = collectMaterialFiles(options.paths);
    if (files.isEmpty()) { out() << "no .pbrm files found\n" << Qt::flush; return 1; }
    QVector<Finding> findings;
    for (const QString& file : files) {
        Pbr::MaterialDocument document;
        QString error;
        if (!document.load(file, &error)) {
            findings.append({file, true, error});
            continue;
        }
        const QString target = file + QStringLiteral(".json");
        QFile outFile(target);
        if (!outFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            findings.append({file, true, QStringLiteral("cannot write ") + target});
            continue;
        }
        outFile.write(QJsonDocument(document.toJson()).toJson(QJsonDocument::Indented));
        findings.append({file, false, QStringLiteral("exported ") + target});
    }
    return reportFindings(findings, files.size(), options.has(QStringLiteral("--json")), QStringLiteral("export-json"));
}

int runPack(const QStringList& args)
{
    const Options options = parseOptions(args, {QStringLiteral("--material"), QStringLiteral("--set"), QStringLiteral("--slot"),
        QStringLiteral("--out"), QStringLiteral("--format"), QStringLiteral("--toksvig"), QStringLiteral("--texture-root")});
    const QString materialFile = options.value(QStringLiteral("--material"), options.paths.value(0));
    const QString setName = options.value(QStringLiteral("--set"), QStringLiteral("UV0"));
    const QString slotKey = options.value(QStringLiteral("--slot"));
    const QString target = options.value(QStringLiteral("--out"));
    if (materialFile.isEmpty() || slotKey.isEmpty() || target.isEmpty()) {
        out() << "usage: pbrm pack --material <file.pbrm> --slot <slotKey> --out <file.dds> [--set UV0]"
                 " [--format bc1|bc4|bc5|bc7|bc7-srgb|rgba8] [--no-mips] [--toksvig <normal texture>] [--texture-root <dir>]\n" << Qt::flush;
        return 2;
    }
    Pbr::MaterialDocument document;
    QString error;
    if (!document.load(materialFile, &error)) {
        out() << "error: " << error << '\n' << Qt::flush;
        return 1;
    }
    const QString textureRoot = effectiveTextureRoot(options);
    const Pbr::TextureSlotState state = document.textureSlot(setName, slotKey);
    const QImage composite = TextureCache::packedImage(state, textureRoot);
    if (composite.isNull()) {
        out() << "error: the slot's composite image is empty (missing texture or unknown slot)\n" << Qt::flush;
        return 1;
    }
    const QString formatName = options.value(QStringLiteral("--format"), QStringLiteral("bc7")).toLower();
    TextureWriter::DdsFormat format = TextureWriter::DdsFormat::Bc7;
    if (formatName == QLatin1String("bc1")) format = TextureWriter::DdsFormat::Bc1;
    else if (formatName == QLatin1String("bc4")) format = TextureWriter::DdsFormat::Bc4;
    else if (formatName == QLatin1String("bc5")) format = TextureWriter::DdsFormat::Bc5;
    else if (formatName == QLatin1String("bc7-srgb")) format = TextureWriter::DdsFormat::Bc7Srgb;
    else if (formatName == QLatin1String("rgba8")) format = TextureWriter::DdsFormat::Rgba8;
    else if (formatName != QLatin1String("bc7")) {
        out() << "error: unknown format '" << formatName << "'\n" << Qt::flush;
        return 2;
    }
    QImage toksvigNormal;
    if (options.flags.contains(QStringLiteral("--toksvig"))) {
        QString toksvigError;
        toksvigNormal = TextureImage::load(options.value(QStringLiteral("--toksvig")), textureRoot, &toksvigError);
        if (toksvigNormal.isNull()) {
            out() << "error: Toksvig normal could not be loaded: " << toksvigError << '\n' << Qt::flush;
            return 1;
        }
    }
    if (!TextureWriter::saveDds(composite, target, format, !options.has(QStringLiteral("--no-mips")), &error, toksvigNormal)) {
        out() << "error: " << error << '\n' << Qt::flush;
        return 1;
    }
    out() << "packed " << setName << '/' << slotKey << " -> " << target << '\n' << Qt::flush;
    return 0;
}

int runBakeAo(const QStringList& args)
{
    const Options options = parseOptions(args, {QStringLiteral("--mesh"), QStringLiteral("--out"),
        QStringLiteral("--size"), QStringLiteral("--directions"), QStringLiteral("--strength")});
    const QString meshPath = options.value(QStringLiteral("--mesh"));
    const QString outPath = options.value(QStringLiteral("--out"));
    if (meshPath.isEmpty() || outPath.isEmpty()) {
        out() << "usage: pbrm bake-ao --mesh <nif or fbx> --out <image> [--size <px>]\n"
                 "                    [--directions <n>] [--strength <exp>]\n" << Qt::flush;
        return 2;
    }
    QString error;
    PreviewMesh mesh = meshPath.endsWith(QStringLiteral(".fbx"), Qt::CaseInsensitive)
        ? PreviewMeshes::loadStaticFbx(meshPath, &error)
        : PreviewMeshes::loadStaticNif(meshPath, &error);
    if (!mesh.isValid()) {
        out() << meshPath << ": error: " << (error.isEmpty() ? QStringLiteral("mesh invalid") : error) << "\n" << Qt::flush;
        return 1;
    }
    GeometryAO::Options aoOptions;
    if (options.has(QStringLiteral("--size"))) aoOptions.mapSize = options.value(QStringLiteral("--size")).toInt();
    if (options.has(QStringLiteral("--directions"))) aoOptions.directions = options.value(QStringLiteral("--directions")).toInt();
    if (options.has(QStringLiteral("--strength"))) aoOptions.strength = options.value(QStringLiteral("--strength")).toFloat();
    QStringList notes;
    const QImage baked = GeometryAO::bake(mesh, aoOptions, &notes);
    if (baked.isNull()) {
        out() << meshPath << ": error: bake produced no image\n" << Qt::flush;
        return 1;
    }
    QDir().mkpath(QFileInfo(outPath).absolutePath());
    if (!baked.save(outPath)) {
        out() << outPath << ": error: could not write the AO image\n" << Qt::flush;
        return 1;
    }
    for (const QString& note : notes) out() << note << "\n";
    out() << "wrote " << outPath << "\n" << Qt::flush;
    return 0;
}

int runMaterialInfo(const QStringList& args)
{
    const Options options = parseOptions(args, {});
    if (options.paths.isEmpty()) {
        out() << "usage: pbrm material-info <files.bgsm/.bgem...>\n" << Qt::flush;
        return 2;
    }
    int failures = 0;
    for (const QString& path : options.paths) {
        Bgsm::Material material;
        QString error;
        if (!Bgsm::readFile(path, &material, &error)) {
            out() << path << ": error: " << error << "\n";
            ++failures;
            continue;
        }
        const auto vec = [](const QVector3D& v) {
            return QStringLiteral("%1 %2 %3").arg(v.x(), 0, 'f', 3).arg(v.y(), 0, 'f', 3).arg(v.z(), 0, 'f', 3);
        };
        out() << path << ": " << (material.kind == Bgsm::Kind::Bgem ? "BGEM" : "BGSM")
              << " v" << material.version << (material.isFallout76() ? " (FO76)" : " (FO4)") << "\n";
        const QList<QPair<QString, QString>> textures = {
            {QStringLiteral("diffuse"), material.diffuseTexture}, {QStringLiteral("base"), material.baseTexture},
            {QStringLiteral("normal"), material.normalTexture}, {QStringLiteral("smoothSpec"), material.smoothSpecTexture},
            {QStringLiteral("greyscale"), material.greyscaleTexture}, {QStringLiteral("envmap"), material.envmapTexture},
            {QStringLiteral("envmapMask"), material.envmapMaskTexture}, {QStringLiteral("glow"), material.glowTexture},
            {QStringLiteral("innerLayer"), material.innerLayerTexture}, {QStringLiteral("displacement"), material.displacementTexture},
            {QStringLiteral("specular (_r)"), material.specularTexture}, {QStringLiteral("lighting (_l)"), material.lightingTexture},
            {QStringLiteral("wrinkles"), material.wrinklesTexture}, {QStringLiteral("flow"), material.flowTexture},
            {QStringLiteral("root"), material.rootMaterialPath}};
        for (const auto& pair : textures)
            if (!pair.second.trimmed().isEmpty()) out() << "  " << pair.first << ": " << pair.second << "\n";
        out() << "  tiling U/V " << material.tileU << "/" << material.tileV
              << " scale " << material.uScale << "," << material.vScale
              << " offset " << material.uOffset << "," << material.vOffset << "\n";
        out() << "  alpha " << material.alpha << " blend " << int(material.blendMode)
              << " test " << material.alphaTest << " ref " << material.alphaTestRef
              << " twoSided " << material.twoSided << " decal " << material.decal << "\n";
        out() << "  envMapping " << material.environmentMapping << " maskScale " << material.environmentMappingMaskScale
              << " smoothness " << material.smoothness << " specMult " << material.specularMult
              << " specColor " << vec(material.specularColor) << "\n";
        out() << "  grayscaleToPaletteColor " << material.grayscaleToPaletteColor
              << " grayscaleToPaletteScale " << material.grayscaleToPaletteScale << "\n";
        if (material.emitEnabled || material.glowmap)
            out() << "  emit " << material.emitEnabled << " glowmap " << material.glowmap
                  << " color " << vec(material.emittanceColor) << " mult " << material.emittanceMult << "\n";
        if (material.kind == Bgsm::Kind::Bgem)
            out() << "  baseColor " << vec(material.baseColor) << " scale " << material.baseColorScale
                  << " effectLighting " << material.effectLightingEnabled << " falloff " << material.falloffEnabled << "\n";
        if (material.pbr || material.customPorosity)
            out() << "  pbr " << material.pbr << " customPorosity " << material.customPorosity
                  << " porosity " << material.porosityValue << "\n";
        if (material.skinTint || material.facegen || material.hair || material.translucency || material.subsurfaceLighting)
            out() << "  skinTint " << material.skinTint << " facegen " << material.facegen << " hair " << material.hair
                  << " translucency " << material.translucency << " subsurface " << material.subsurfaceLighting << "\n";
    }
    out() << Qt::flush;
    return failures ? 1 : 0;
}

int runConvert(const QStringList& args)
{
    const Options options = parseOptions(args, {QStringLiteral("--out"), QStringLiteral("--texture-root"),
        QStringLiteral("--recovery"), QStringLiteral("--gloss-curve"), QStringLiteral("--derivation-size"),
        QStringLiteral("--correction-mask"), QStringLiteral("--graph")});
    const QString outputRoot = options.value(QStringLiteral("--out"));
    const QString manifestName = QStringLiteral("/pbrm-conversion-manifest.json");
    if (outputRoot.isEmpty() || (options.paths.isEmpty() && !options.has(QStringLiteral("--clean")))) {
        out() << "usage: pbrm convert --out <mod folder> [--texture-root <dir>] [--dry-run] [--json]\n"
                 "                    [--force] [--height-in-blue] [--no-detail-ao] [--metal-diagnostic]\n"
                 "                    [--recovery <0-1>] [--gloss-curve <v>] [--derivation-size <px>]\n"
                 "                    [--correction-mask <image>]  # 0.5-neutral metallic correction\n"
                 "                    [--graph <file.pbrgraph>]    # convert through a node graph\n"
                 "                    <materials, folders, or .ba2 archives...>\n"
                 "       pbrm convert --out <mod folder> --clean   # remove exactly the recorded output\n" << Qt::flush;
        return 2;
    }

    // --clean: roll back exactly what the manifest records — nothing else.
    if (options.has(QStringLiteral("--clean"))) {
        const QString cleanRoot = QDir::cleanPath(QDir::fromNativeSeparators(outputRoot));
        QFile manifestFile(cleanRoot + manifestName);
        if (!manifestFile.open(QIODevice::ReadOnly)) {
            out() << cleanRoot << ": error: no conversion manifest to clean against\n" << Qt::flush;
            return 1;
        }
        const QJsonObject manifest = QJsonDocument::fromJson(manifestFile.readAll()).object();
        manifestFile.close();
        int removed = 0, missing = 0;
        QSet<QString> pruneDirs;
        const auto removeOutput = [&](const QString& absolute) {
            const QString cleaned = QDir::cleanPath(absolute);
            if (!cleaned.startsWith(cleanRoot + QLatin1Char('/'), Qt::CaseInsensitive)) return; // never step outside
            if (QFile::exists(cleaned)) {
                QFile::remove(cleaned);
                ++removed;
            } else {
                ++missing;
            }
            pruneDirs.insert(QFileInfo(cleaned).absolutePath());
        };
        const QJsonArray entries = manifest.value(QStringLiteral("entries")).toArray();
        for (const QJsonValue& value : entries) {
            const QJsonObject entry = value.toObject();
            removeOutput(entry.value(QStringLiteral("pbrm")).toString());
            const QJsonArray textures = entry.value(QStringLiteral("textures")).toArray();
            for (const QJsonValue& texture : textures)
                removeOutput(cleanRoot + QStringLiteral("/Textures/") + texture.toString());
        }
        QFile::remove(cleanRoot + manifestName);
        // Prune now-empty directories upward (rmdir refuses non-empty ones).
        for (const QString& start : pruneDirs) {
            QDir dir(start);
            while (dir.absolutePath().startsWith(cleanRoot + QLatin1Char('/'), Qt::CaseInsensitive)
                && dir.rmdir(dir.absolutePath())) {
                if (!dir.cdUp()) break;
            }
        }
        out() << removed << " file(s) removed, " << missing << " already gone; manifest cleared\n" << Qt::flush;
        return 0;
    }
    Converter::JobOptions job;
    job.outputRoot = QDir::fromNativeSeparators(outputRoot);
    job.textureRoot = effectiveTextureRoot(options);
    job.dryRun = options.has(QStringLiteral("--dry-run"));
    job.detailAo = !options.has(QStringLiteral("--no-detail-ao"));
    job.heightInBlue = options.has(QStringLiteral("--height-in-blue"));
    job.writeMetallicDiagnostic = options.has(QStringLiteral("--metal-diagnostic"));
    if (options.has(QStringLiteral("--recovery")))
        job.math.recoveryStrength = options.value(QStringLiteral("--recovery")).toFloat();
    if (options.has(QStringLiteral("--gloss-curve")))
        job.math.glossCurve = options.value(QStringLiteral("--gloss-curve")).toFloat();
    if (options.has(QStringLiteral("--derivation-size")))
        job.derive.derivationSize = options.value(QStringLiteral("--derivation-size")).toInt();
    job.correctionMaskPath = options.value(QStringLiteral("--correction-mask"));

    // --graph: the node graph produces the images; the orchestrator still owns
    // portable paths, dedup, the sibling document, and the manifest.
    NodeGraph::Graph conversionGraph;
    const QString graphPath = options.value(QStringLiteral("--graph"));
    if (!graphPath.isEmpty()) {
        QString graphError;
        conversionGraph = ConverterNodes::loadGraphFile(graphPath, &graphError);
        QStringList graphErrors;
        if (conversionGraph.nodes.isEmpty())
            graphErrors.append(graphError.isEmpty() ? QStringLiteral("the graph is empty") : graphError);
        else
            conversionGraph.validate(ConverterNodes::sharedRegistry(), &graphErrors);
        if (!graphErrors.isEmpty()) {
            for (const QString& error : graphErrors) out() << "error: " << graphPath << ": " << error << "\n";
            out() << Qt::flush;
            return 2;
        }
    }

    struct MaterialSource {
        QString path;      // loose file, or the BA2 archive
        QString entry;     // empty = loose; else the material's path inside the archive
        QString display() const { return entry.isEmpty() ? path : path + QStringLiteral(" :: ") + entry; }
    };
    QVector<MaterialSource> materials;
    for (const QString& target : options.paths) {
        const QFileInfo info(target);
        if (info.isDir()) {
            QDirIterator it(info.absoluteFilePath(), {QStringLiteral("*.bgsm"), QStringLiteral("*.bgem")},
                QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) materials.append({it.next(), {}});
        } else if (info.isFile() && target.endsWith(QStringLiteral(".ba2"), Qt::CaseInsensitive)) {
            // Archive-direct: convert every material the BA2 carries.
            QString archiveError;
            QStringList entries = TextureArchive::entryPaths(info.absoluteFilePath(), QStringLiteral(".bgsm"), &archiveError);
            entries += TextureArchive::entryPaths(info.absoluteFilePath(), QStringLiteral(".bgem"), nullptr);
            if (!archiveError.isEmpty()) {
                out() << target << ": error: " << archiveError << "\n";
                continue;
            }
            entries.sort(Qt::CaseInsensitive);
            for (const QString& entry : entries) materials.append({info.absoluteFilePath(), entry});
        } else if (info.isFile()) {
            materials.append({info.absoluteFilePath(), {}});
        } else {
            out() << target << ": error: path does not exist\n";
        }
    }
    if (materials.isEmpty()) {
        out() << "no .bgsm/.bgem materials found\n" << Qt::flush;
        return 1;
    }

    // Incremental re-runs: an entry is up-to-date when the source is
    // unchanged, the same options produced it, and its sibling still exists.
    const QString jobSignature = QStringLiteral("v1|%1|%2|%3|%4|%5|%6|%7|%8")
        .arg(job.math.recoveryStrength).arg(job.math.glossCurve).arg(job.derive.derivationSize)
        .arg(job.heightInBlue).arg(job.detailAo).arg(job.writeMetallicDiagnostic).arg(job.textureRoot.toLower())
        .arg(job.correctionMaskPath.isEmpty() ? QString()
            : job.correctionMaskPath.toLower() + QString::number(QFileInfo(job.correctionMaskPath).lastModified().toSecsSinceEpoch()));
    QHash<QString, QJsonObject> previousEntries;
    {
        QFile manifestFile(job.outputRoot + manifestName);
        if (manifestFile.open(QIODevice::ReadOnly)) {
            const QJsonArray entries = QJsonDocument::fromJson(manifestFile.readAll()).object()
                .value(QStringLiteral("entries")).toArray();
            for (const QJsonValue& value : entries) {
                const QJsonObject entry = value.toObject();
                previousEntries.insert(QDir::cleanPath(entry.value(QStringLiteral("material")).toString()).toLower(), entry);
            }
        }
    }
    const bool force = options.has(QStringLiteral("--force"));

    Converter::TextureCache textureCache;
    QVector<Finding> findings;
    QJsonArray manifestEntries;
    int converted = 0, flagged = 0, failed = 0, upToDate = 0;
    for (const MaterialSource& source : materials) {
        const QString material = source.display();
        // Archive entries inherit the archive's timestamp for skip checks.
        const QString sourceModified = QFileInfo(source.path).lastModified().toString(Qt::ISODateWithMs);
        const QJsonObject previous = previousEntries.value(QDir::cleanPath(material).toLower());
        if (!force && !job.dryRun && previous.value(QStringLiteral("converted")).toBool(false)
            && previous.value(QStringLiteral("sourceModified")).toString() == sourceModified
            && previous.value(QStringLiteral("optionsSignature")).toString() == jobSignature
            && QFileInfo::exists(previous.value(QStringLiteral("pbrm")).toString())) {
            ++upToDate;
            manifestEntries.append(previous);
            continue;
        }
        Converter::JobResult jobResult;
        Converter::JobOptions materialJob = job;
        if (!conversionGraph.nodes.isEmpty() && source.entry.isEmpty()) {
            QStringList graphErrors;
            materialJob.supplied = ConverterNodes::runGraph(conversionGraph, source.path,
                job.textureRoot, job.geometryNifPath, job.correctionMaskPath, &graphErrors);
            if (!materialJob.supplied.active) {
                jobResult.materialPath = material;
                jobResult.errors.append(graphErrors.isEmpty()
                    ? QStringLiteral("the graph produced no output") : graphErrors.join(QStringLiteral("; ")));
                // Fall through to reporting: nothing is written for this one.
            }
        }
        if (!jobResult.errors.isEmpty()) {
            // graph failure above
        } else if (source.entry.isEmpty()) {
            jobResult = Converter::convertMaterial(source.path, materialJob, &textureCache);
        } else {
            QString extractError;
            const QByteArray data = TextureArchive::extract(source.path, source.entry, &extractError);
            if (data.isEmpty()) {
                jobResult.materialPath = material;
                jobResult.errors.append(extractError.isEmpty()
                    ? QStringLiteral("could not extract the material from the archive") : extractError);
            } else {
                jobResult = Converter::convertMaterialData(source.path, source.entry, data, materialJob, &textureCache);
            }
        }
        for (const QString& note : jobResult.notes) findings.append({material, false, note});
        for (const QString& jobError : jobResult.errors) findings.append({material, true, jobError});
        if (jobResult.converted) {
            ++converted;
            findings.append({material, false, (job.dryRun ? QStringLiteral("would convert -> ") : QStringLiteral("converted -> "))
                + jobResult.pbrmPath
                + QStringLiteral(" (metal %1%)").arg(int(std::lround(jobResult.metalCoverage * 100.0f)))});
        } else {
            ++failed;
        }
        if (jobResult.flagged) ++flagged;
        manifestEntries.append(QJsonObject{
            {QStringLiteral("material"), material},
            {QStringLiteral("pbrm"), jobResult.pbrmPath},
            {QStringLiteral("converted"), jobResult.converted},
            {QStringLiteral("flagged"), jobResult.flagged},
            {QStringLiteral("metalCoverage"), double(jobResult.metalCoverage)},
            {QStringLiteral("sourceModified"), sourceModified},
            {QStringLiteral("optionsSignature"), jobSignature},
            {QStringLiteral("textures"), QJsonArray::fromStringList(jobResult.writtenTextures)},
            {QStringLiteral("notes"), QJsonArray::fromStringList(jobResult.notes)},
            {QStringLiteral("errors"), QJsonArray::fromStringList(jobResult.errors)}});
    }
    if (!job.dryRun) {
        QDir().mkpath(job.outputRoot);
        QFile manifest(job.outputRoot + manifestName);
        if (manifest.open(QIODevice::WriteOnly | QIODevice::Truncate))
            manifest.write(QJsonDocument(QJsonObject{
                {QStringLiteral("schema"), QStringLiteral("FO4.PBRM.ConversionManifest")},
                {QStringLiteral("schemaVersion"), 1},
                {QStringLiteral("entries"), manifestEntries}}).toJson(QJsonDocument::Indented));
    }
    if (!options.has(QStringLiteral("--json"))) {
        out() << converted << (job.dryRun ? " material(s) would convert, " : " material(s) converted, ")
              << flagged << " flagged for review, " << failed << " failed";
        if (upToDate) out() << ", " << upToDate << " up-to-date (skipped)";
        out() << "\n";
    }
    const int findingsCode = reportFindings(findings, materials.size(), options.has(QStringLiteral("--json")), QStringLiteral("convert"));
    return failed > 0 ? 1 : findingsCode;
}

int runInspect(const QStringList& args)
{
    const Options options = parseOptions(args, {QStringLiteral("--texture-root")});
    if (options.paths.isEmpty()) {
        out() << "usage: pbrm inspect [--texture-root <dir>] <textures...>\n" << Qt::flush;
        return 2;
    }
    const QString textureRoot = effectiveTextureRoot(options);
    int failures = 0;
    for (const QString& path : options.paths) {
        const TextureImage::Diagnostics info = TextureImage::inspect(path, textureRoot);
        if (!info.valid) {
            out() << path << ": error: " << info.error << '\n';
            ++failures;
            continue;
        }
        out() << path << ": " << info.width << 'x' << info.height << " " << info.format
              << " " << info.colorSpace << " channels " << info.channels.join(QString())
              << " mips " << info.mipLevels << '\n';
        QString loadError;
        const QImage image = TextureImage::load(path, textureRoot, &loadError).convertToFormat(QImage::Format_RGBA8888);
        if (image.isNull()) {
            out() << "  (decode failed: " << loadError << ")\n";
            continue;
        }
        // Per-channel statistics answer packing questions with data.
        double sum[4] = {}, minimum[4] = {255, 255, 255, 255}, maximum[4] = {};
        double variance[4] = {};
        const double texels = double(image.width()) * image.height();
        for (int y = 0; y < image.height(); ++y) {
            const uchar* line = image.constScanLine(y);
            for (int x = 0; x < image.width(); ++x)
                for (int channel = 0; channel < 4; ++channel) {
                    const double value = line[x * 4 + channel];
                    sum[channel] += value;
                    minimum[channel] = qMin(minimum[channel], value);
                    maximum[channel] = qMax(maximum[channel], value);
                }
        }
        for (int y = 0; y < image.height(); ++y) {
            const uchar* line = image.constScanLine(y);
            for (int x = 0; x < image.width(); ++x)
                for (int channel = 0; channel < 4; ++channel) {
                    const double delta = line[x * 4 + channel] - sum[channel] / texels;
                    variance[channel] += delta * delta;
                }
        }
        const char* channelNames[4] = {"R", "G", "B", "A"};
        for (int channel = 0; channel < 4; ++channel)
            out() << "  " << channelNames[channel]
                  << " mean " << QString::number(sum[channel] / texels / 255.0, 'f', 3)
                  << " min " << QString::number(minimum[channel] / 255.0, 'f', 3)
                  << " max " << QString::number(maximum[channel] / 255.0, 'f', 3)
                  << " stddev " << QString::number(std::sqrt(variance[channel] / texels) / 255.0, 'f', 3) << '\n';
    }
    out() << Qt::flush;
    return failures > 0 ? 1 : 0;
}

int runExportGraph(const QStringList& args)
{
    const Options options = parseOptions(args, {QStringLiteral("--out"), QStringLiteral("--profile")});
    const QString outputPath = options.value(QStringLiteral("--out"));
    const QString profile = options.value(QStringLiteral("--profile")).toLower();
    if (outputPath.isEmpty() || (profile != QStringLiteral("fo4") && profile != QStringLiteral("fo76"))) {
        out() << "usage: pbrm export-graph --profile <fo4|fo76> --out <file.pbrgraph>\n"
                 "       writes the stock conversion graph, ready to edit or run with convert --graph\n" << Qt::flush;
        return 2;
    }
    const NodeGraph::Graph graph = profile == QStringLiteral("fo76")
        ? ConverterNodes::defaultFo76Graph() : ConverterNodes::defaultFo4Graph();
    QString error;
    if (!ConverterNodes::saveGraphFile(graph, outputPath, &error)) {
        out() << "error: " << error << "\n" << Qt::flush;
        return 1;
    }
    out() << "wrote " << QDir::toNativeSeparators(outputPath) << " (" << graph.nodes.size() << " nodes, "
          << graph.connections.size() << " links)\n" << Qt::flush;
    return 0;
}

QString helpText()
{
    return QStringLiteral(
        "pbrm — FO4 PBR Material Editor headless companion\n"
        "\n"
        "usage: pbrm <verb> [options] [files or folders...]\n"
        "\n"
        "verbs:\n"
        "  validate            check materials (same findings as the GUI validation)\n"
        "  migrate             re-save materials on the current schema (v4 -> v5)\n"
        "  extract-states      write legacy per-material states to definitions JSON sidecars\n"
        "  strip-legacy        remove legacy surface data (groups, response slots, retired keys)\n"
        "  set-setting         set one setting key across materials\n"
        "  repair-paths        re-point broken texture paths by filename\n"
        "  export-definitions  write the built-in surface state definitions JSON\n"
        "  export-json         write readable .pbrm.json sidecars\n"
        "  pack                composite a slot's channel sources into a game-ready DDS\n"
        "  inspect             texture diagnostics + per-channel statistics\n"
        "  convert             convert FO4/FO76 .bgsm/.bgem materials + textures to PBR\n"
        "                      (incremental via the manifest; --force re-runs, --clean rolls back)\n"
        "  export-graph        write the stock conversion node graph for a title\n"
        "  material-info       dump parsed .bgsm/.bgem fields (textures, scalars, flags)\n"
        "  bake-ao             bake geometry AO from a reference mesh to an image\n"
        "\n"
        "common options: --json (machine-readable reports), --dry-run (report only),\n"
        "  --texture-root <dir> (explicit source root; editor settings are the fallback)\n");
}

int runVerb(const QString& verb, const QStringList& args)
{
    if (verb == QLatin1String("validate")) return runValidate(args);
    if (verb == QLatin1String("migrate")) return runMigrate(args);
    if (verb == QLatin1String("extract-states")) return runExtractStates(args);
    if (verb == QLatin1String("strip-legacy")) return runStripLegacy(args);
    if (verb == QLatin1String("set-setting")) return runSetSetting(args);
    if (verb == QLatin1String("repair-paths")) return runRepairPaths(args);
    if (verb == QLatin1String("export-definitions")) return runExportDefinitions(args);
    if (verb == QLatin1String("export-json")) return runExportJson(args);
    if (verb == QLatin1String("pack")) return runPack(args);
    if (verb == QLatin1String("inspect")) return runInspect(args);
    if (verb == QLatin1String("convert")) return runConvert(args);
    if (verb == QLatin1String("export-graph")) return runExportGraph(args);
    if (verb == QLatin1String("material-info")) return runMaterialInfo(args);
    if (verb == QLatin1String("bake-ao")) return runBakeAo(args);
    out() << helpText() << Qt::flush;
    return verb == QLatin1String("help") || verb == QLatin1String("--help") ? 0 : 2;
}

} // namespace PbrmCli
