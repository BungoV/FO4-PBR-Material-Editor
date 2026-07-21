#include "materialdocument.h"

#include <QDataStream>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <algorithm>
#include <cmath>
#include <limits>

namespace Pbr {

namespace {

struct ShaderInfo {
    ShaderMode mode;
    const char* name;
    const char* description;
};

constexpr ShaderInfo ShaderInfos[] = {
    {ShaderMode::StandardSurface, "Standard", "General metal/rough physically based shading for props, characters, architecture, and ordinary world geometry."},
    {ShaderMode::AdvancedSubsurface, "Advanced Subsurface", "Profile-driven subsurface diffusion and transmission for skin, wax, flesh, and similar materials."},
    {ShaderMode::VegetationSurface, "Vegetation", "Alpha-tested foliage with thin transmission and vertex-color-weighted wind deformation."},
    {ShaderMode::HairSurface, "Hair", "Hair-card shading with strand direction, dual specular lobes, scattering, transmission, and backlighting."},
    {ShaderMode::EyeSurface, "Eye", "Layered cornea and iris shading with iris depth, limbal masking, refraction, and a wet outer reflection lobe."},
    {ShaderMode::LandscapeSurface, "Landscape", "Array-backed LAND and LTEX/TXST terrain layers with weight or height blending."},
    {ShaderMode::GlassSurface, "Glass", "Physical transmission with IOR, thickness, absorption, rough refraction, and surface imperfections."},
    {ShaderMode::WaterSurface, "Water", "Depth-aware reflection, refraction, absorption, scattering, animated flow normals, foam, and shoreline behavior."},
    {ShaderMode::UnlitEffect, "Unlit Effect", "Translucent or additive effects that ignore scene lighting."},
    {ShaderMode::SimpleLitEffect, "Simple Lit Effect", "Effects with inexpensive diffuse and specular lighting."},
    {ShaderMode::PhysicallyLitEffect, "Physically Lit Effect", "Animated or translucent effects using the full metal/rough PBR response."},
    {ShaderMode::AllFeatures, "Advanced", "Complete authoring and validation superset. Expensive by design; use a narrower shader for final assets."}
};

const ShaderInfo& infoFor(ShaderMode mode)
{
    for (const auto& info : ShaderInfos)
        if (info.mode == mode)
            return info;
    return ShaderInfos[0];
}

bool legacyShaderModeFromName(const QString& name, ShaderMode* mode)
{
    const QPair<const char*, ShaderMode> aliases[] = {
        {"Standard Surface", ShaderMode::StandardSurface},
        {"Vegetation Surface", ShaderMode::VegetationSurface},
        {"Hair Surface", ShaderMode::HairSurface},
        {"Glass Surface", ShaderMode::GlassSurface},
        {"Water Surface", ShaderMode::WaterSurface},
        {"Eye Surface", ShaderMode::EyeSurface},
        {"Landscape Surface", ShaderMode::LandscapeSurface}
        ,{"All Features", ShaderMode::AllFeatures}
    };
    for (const auto& alias : aliases) {
        if (name.compare(QString::fromUtf8(alias.first), Qt::CaseInsensitive) == 0) {
            if (mode) *mode = alias.second;
            return true;
        }
    }
    return false;
}

QJsonObject migrateOverrideValues(QJsonObject values)
{
    const QPair<const char*, const char*> mappings[] = {
        {"useRoughness", "overrideRoughness"}, {"useMetallic", "overrideMetallic"},
        {"useAo", "overrideAo"}, {"useF0", "overrideF0"},
        {"useSheen", "overrideSheen"}, {"useGlints", "overrideGlints"},
        {"useFilm", "overrideFilm"}, {"useIridescence", "overrideIridescence"},
        {"useThickness", "overrideThickness"}, {"useCoverage", "overrideCoverage"},
        {"useMask", "overrideMask"}, {"useNormal", "overrideNormal"},
        {"useR", "overrideMaskR"}, {"useG", "overrideMaskG"},
        {"useB", "overrideMaskB"}, {"useA", "overrideMaskA"},
        {"useHeight", "overrideHeight"}, {"useLimbal", "overrideLimbal"},
        {"usePupil", "overridePupil"}, {"useCornea", "overrideCornea"}
    };
    for (const auto& mapping : mappings) {
        const QString oldKey = QString::fromLatin1(mapping.first);
        const QString newKey = QString::fromLatin1(mapping.second);
        if (values.contains(oldKey) && !values.contains(newKey))
            values.insert(newKey, !values.value(oldKey).toBool());
        values.remove(oldKey);
    }
    if (values.contains(QStringLiteral("fallback")) && !values.contains(QStringLiteral("height"))) {
        values.insert(QStringLiteral("height"), values.value(QStringLiteral("fallback")));
        values.remove(QStringLiteral("fallback"));
    }
    return values;
}

QJsonObject migrateOverridesRecursively(QJsonObject object)
{
    if (object.value(QStringLiteral("values")).isObject())
        object.insert(QStringLiteral("values"), migrateOverrideValues(object.value(QStringLiteral("values")).toObject()));
    for (auto it = object.begin(); it != object.end(); ++it)
        if (it.value().isObject() && it.key() != QStringLiteral("values"))
            it.value() = migrateOverridesRecursively(it.value().toObject());
    return object;
}

} // namespace

QString shaderModeName(ShaderMode mode)
{
    return QString::fromUtf8(infoFor(mode).name);
}

QString shaderModeDescription(ShaderMode mode)
{
    return QString::fromUtf8(infoFor(mode).description);
}

QStringList shaderModeNames()
{
    QStringList names;
    for (const auto& info : ShaderInfos)
        names.append(QString::fromUtf8(info.name));
    return names;
}

ShaderMode shaderModeFromName(const QString& name)
{
    for (const auto& info : ShaderInfos)
        if (name.compare(QString::fromUtf8(info.name), Qt::CaseInsensitive) == 0)
            return info.mode;
    ShaderMode legacy = ShaderMode::StandardSurface;
    if (legacyShaderModeFromName(name, &legacy)) return legacy;
    return ShaderMode::StandardSurface;
}

QJsonObject TextureSlotState::toJson() const
{
    QJsonObject object;
    object.insert(QStringLiteral("enabled"), enabled);
    object.insert(QStringLiteral("expanded"), expanded);
    object.insert(QStringLiteral("path"), path);
    object.insert(QStringLiteral("values"), values);
    return object;
}

TextureSlotState TextureSlotState::fromJson(const QJsonObject& object)
{
    TextureSlotState state;
    state.enabled = object.value(QStringLiteral("enabled")).toBool(false);
    state.expanded = object.value(QStringLiteral("expanded")).toBool(true);
    state.path = object.value(QStringLiteral("path")).toString();
    state.values = object.value(QStringLiteral("values")).toObject();
    return state;
}

MaterialDocument::MaterialDocument()
{
    reset();
}

void MaterialDocument::reset()
{
    m_shaderMode = ShaderMode::StandardSurface;
    m_secondaryUvEnabled = false;
    m_tertiaryUvEnabled = false;
    m_uv0 = QJsonObject();
    m_uv1 = QJsonObject();
    m_uv2 = QJsonObject();
    m_resources = QJsonObject();
    m_settings = QJsonObject();
	m_unknownRoot = QJsonObject();
    m_fileName.clear();
    m_modified = false;
}

bool MaterialDocument::load(const QString& fileName, QString* error)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        return false;
    }

    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    quint32 magic = 0;
    quint32 version = 0;
    quint32 payloadSize = 0;
    stream >> magic >> version >> payloadSize;
    if (stream.status() != QDataStream::Ok) {
        if (error) *error = QStringLiteral("PBRM header is truncated.");
        return false;
    }
    if (magic != Magic) {
        if (error) *error = QStringLiteral("Not a PBRM material (invalid signature).");
        return false;
    }
    if (version != Version) {
        if (error) *error = QStringLiteral("Unsupported PBRM version %1; this editor writes version %2.").arg(version).arg(Version);
        return false;
    }
    constexpr quint32 MaximumPayloadSize = 64u * 1024u * 1024u;
    const qint64 remainingBytes = file.bytesAvailable();
    if (payloadSize > MaximumPayloadSize || remainingBytes != static_cast<qint64>(payloadSize)) {
        if (error) *error = QStringLiteral("PBRM payload length is invalid.");
        return false;
    }

    const QByteArray payload = file.read(payloadSize);
    QJsonParseError parseError;
    const QJsonDocument json = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !json.isObject()) {
        if (error) *error = QStringLiteral("Invalid PBRM payload: %1").arg(parseError.errorString());
        return false;
    }
    if (!fromJson(json.object(), error))
        return false;
    m_fileName = fileName;
    m_modified = false;
    return true;
}

bool MaterialDocument::save(const QString& fileName, QString* error) const
{
    const QByteArray payload = QJsonDocument(toJson()).toJson(QJsonDocument::Compact);
    constexpr qsizetype MaximumPayloadSize = 64u * 1024u * 1024u;
    if (payload.size() > MaximumPayloadSize
        || payload.size() > static_cast<qsizetype>(std::numeric_limits<quint32>::max())) {
        if (error) *error = QStringLiteral("PBRM payload exceeds the 64 MiB limit.");
        return false;
    }
    QSaveFile file(fileName);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << Magic << Version << static_cast<quint32>(payload.size());
    if (file.write(payload) != payload.size()) {
        if (error) *error = file.errorString();
        return false;
    }
    if (!file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

ShaderMode MaterialDocument::shaderMode() const { return m_shaderMode; }
void MaterialDocument::setShaderMode(ShaderMode mode) { m_shaderMode = mode; m_modified = true; }
bool MaterialDocument::secondaryUvEnabled() const { return m_secondaryUvEnabled; }
void MaterialDocument::setSecondaryUvEnabled(bool enabled) { m_secondaryUvEnabled = enabled; m_modified = true; }
bool MaterialDocument::tertiaryUvEnabled() const { return m_tertiaryUvEnabled; }
void MaterialDocument::setTertiaryUvEnabled(bool enabled) { m_tertiaryUvEnabled = enabled; m_modified = true; }

TextureSlotState MaterialDocument::textureSlot(const QString& uvSet, const QString& key) const
{
    const QJsonObject set = textureSet(uvSet);
    if (!set.contains(key)) return {};
    return TextureSlotState::fromJson(set.value(key).toObject());
}

void MaterialDocument::setTextureSlot(const QString& uvSet, const QString& key, const TextureSlotState& state)
{
    if (uvSet.compare(QStringLiteral("UV0"), Qt::CaseInsensitive) == 0) {
        m_uv0.insert(key, state.toJson());
    } else if (uvSet.compare(QStringLiteral("UV1"), Qt::CaseInsensitive) == 0) {
        m_uv1.insert(key, state.toJson());
    } else if (uvSet.compare(QStringLiteral("UV2"), Qt::CaseInsensitive) == 0) {
        m_uv2.insert(key, state.toJson());
    } else {
        QJsonObject set = m_resources.value(uvSet).toObject();
        set.insert(key, state.toJson());
        m_resources.insert(uvSet, set);
    }
    m_modified = true;
}

void MaterialDocument::removeTextureSlot(const QString& uvSet, const QString& key)
{
    if (uvSet.compare(QStringLiteral("UV0"), Qt::CaseInsensitive) == 0) {
        m_uv0.remove(key);
    } else if (uvSet.compare(QStringLiteral("UV1"), Qt::CaseInsensitive) == 0) {
        m_uv1.remove(key);
    } else if (uvSet.compare(QStringLiteral("UV2"), Qt::CaseInsensitive) == 0) {
        m_uv2.remove(key);
    } else {
        QJsonObject set = m_resources.value(uvSet).toObject();
        set.remove(key);
        if (set.isEmpty()) m_resources.remove(uvSet);
        else m_resources.insert(uvSet, set);
    }
    m_modified = true;
}

QJsonObject MaterialDocument::textureSet(const QString& setName) const
{
    if (setName.compare(QStringLiteral("UV0"), Qt::CaseInsensitive) == 0) return m_uv0;
    if (setName.compare(QStringLiteral("UV1"), Qt::CaseInsensitive) == 0) return m_uv1;
    if (setName.compare(QStringLiteral("UV2"), Qt::CaseInsensitive) == 0) return m_uv2;
    return m_resources.value(setName).toObject();
}

void MaterialDocument::setTextureSet(const QString& setName, const QJsonObject& set)
{
    if (setName.compare(QStringLiteral("UV0"), Qt::CaseInsensitive) == 0) m_uv0 = set;
    else if (setName.compare(QStringLiteral("UV1"), Qt::CaseInsensitive) == 0) m_uv1 = set;
    else if (setName.compare(QStringLiteral("UV2"), Qt::CaseInsensitive) == 0) m_uv2 = set;
    else m_resources.insert(setName, set);
    m_modified = true;
}

void MaterialDocument::clearTextureSet(const QString& setName)
{
    setTextureSet(setName, QJsonObject());
}

QJsonValue MaterialDocument::setting(const QString& section, const QString& key, const QJsonValue& fallback) const
{
    const QJsonObject group = m_settings.value(section).toObject();
    return group.contains(key) ? group.value(key) : fallback;
}

void MaterialDocument::setSetting(const QString& section, const QString& key, const QJsonValue& value)
{
    QJsonObject group = m_settings.value(section).toObject();
    group.insert(key, value);
    m_settings.insert(section, group);
    m_modified = true;
}

QJsonObject MaterialDocument::settingGroup(const QString& section) const
{
    return m_settings.value(section).toObject();
}

void MaterialDocument::setSettingGroup(const QString& section, const QJsonObject& group)
{
    m_settings.insert(section, group);
    m_modified = true;
}

void MaterialDocument::clearSettingGroup(const QString& section)
{
    m_settings.remove(section);
    m_modified = true;
}

QString MaterialDocument::fileName() const { return m_fileName; }
void MaterialDocument::setFileName(const QString& fileName) { m_fileName = fileName; }
bool MaterialDocument::modified() const { return m_modified; }
void MaterialDocument::setModified(bool modified) { m_modified = modified; }

QJsonObject MaterialDocument::toJson() const
{
    QJsonObject object = m_unknownRoot;
    object.insert(QStringLiteral("schema"), QStringLiteral("FO4.PBRM.Material"));
    object.insert(QStringLiteral("schemaVersion"), static_cast<int>(Version));
    object.insert(QStringLiteral("shader"), shaderModeName(m_shaderMode));
    object.insert(QStringLiteral("secondaryUvEnabled"), m_secondaryUvEnabled);
    object.insert(QStringLiteral("primaryUv"), m_uv0);
    object.insert(QStringLiteral("secondaryUv"), m_uv1);
    // Additive v4 members, written sparsely so pre-tertiary materials keep a
    // byte-identical payload.
    if (m_tertiaryUvEnabled) object.insert(QStringLiteral("tertiaryUvEnabled"), true);
    else object.remove(QStringLiteral("tertiaryUvEnabled"));
    if (!m_uv2.isEmpty()) object.insert(QStringLiteral("tertiaryUv"), m_uv2);
    else object.remove(QStringLiteral("tertiaryUv"));
    QJsonObject resources = m_resources;
    QJsonObject environment = resources.value(QStringLiteral("Environment")).toObject();
    environment.remove(QStringLiteral("fallbackCubemap"));
    if (environment.isEmpty()) resources.remove(QStringLiteral("Environment"));
    else resources.insert(QStringLiteral("Environment"), environment);
    object.insert(QStringLiteral("resources"), resources);
    object.insert(QStringLiteral("settings"), m_settings);
    return object;
}

bool MaterialDocument::fromJson(const QJsonObject& object, QString* error)
{
    if (object.value(QStringLiteral("schema")).toString() != QStringLiteral("FO4.PBRM.Material")) {
        if (error) *error = QStringLiteral("The PBRM payload schema is not recognized.");
        return false;
    }
    if (object.value(QStringLiteral("schemaVersion")).toInt(-1) != static_cast<int>(Version)) {
        if (error) *error = QStringLiteral("The PBRM JSON schema version is not supported.");
        return false;
    }

    const QString shaderName = object.value(QStringLiteral("shader")).toString();
    bool knownShader = false;
    for (const QString& candidate : shaderModeNames()) {
        if (shaderName.compare(candidate, Qt::CaseInsensitive) == 0) {
            knownShader = true;
            break;
        }
    }
    knownShader = knownShader || legacyShaderModeFromName(shaderName, nullptr);
    if (!knownShader) {
        if (error) *error = QStringLiteral("The PBRM shader family is not recognized.");
        return false;
    }

    const QStringList objectFields = {
        QStringLiteral("primaryUv"),
        QStringLiteral("secondaryUv"),
        QStringLiteral("tertiaryUv"),
        QStringLiteral("resources"),
        QStringLiteral("settings"),
		QStringLiteral("requirements")
    };
    for (const QString& field : objectFields) {
        if (object.contains(field) && !object.value(field).isObject()) {
            if (error) *error = QStringLiteral("The PBRM '%1' field must be an object.").arg(field);
            return false;
        }
    }
    for (const QString& boolField : {QStringLiteral("secondaryUvEnabled"), QStringLiteral("tertiaryUvEnabled")}) {
        if (object.contains(boolField) && !object.value(boolField).isBool()) {
            if (error) *error = QStringLiteral("The PBRM '%1' field must be a boolean.").arg(boolField);
            return false;
        }
    }
	const QJsonObject requirements = object.value(QStringLiteral("requirements")).toObject();
	for (auto it = requirements.begin(); it != requirements.end(); ++it) {
		const double version = it.value().toDouble(-1.0);
		if (!it.value().isDouble() || version < 1.0 ||
		    version > static_cast<double>(std::numeric_limits<quint32>::max()) ||
		    std::floor(version) != version) {
			if (error) *error = QStringLiteral("The PBRM requirement '%1' must be a positive integer version.").arg(it.key());
			return false;
		}
	}

    m_shaderMode = shaderModeFromName(shaderName);
    m_secondaryUvEnabled = object.value(QStringLiteral("secondaryUvEnabled")).toBool(false);
    m_tertiaryUvEnabled = object.value(QStringLiteral("tertiaryUvEnabled")).toBool(false);
    m_uv0 = migrateOverridesRecursively(object.value(QStringLiteral("primaryUv")).toObject());
    m_uv1 = migrateOverridesRecursively(object.value(QStringLiteral("secondaryUv")).toObject());
    m_uv2 = migrateOverridesRecursively(object.value(QStringLiteral("tertiaryUv")).toObject());
    m_resources = migrateOverridesRecursively(object.value(QStringLiteral("resources")).toObject());
    QJsonObject environment = m_resources.value(QStringLiteral("Environment")).toObject();
    environment.remove(QStringLiteral("fallbackCubemap"));
    if (environment.isEmpty()) m_resources.remove(QStringLiteral("Environment"));
    else m_resources.insert(QStringLiteral("Environment"), environment);
    m_settings = object.value(QStringLiteral("settings")).toObject();
	m_unknownRoot = object;
	for (const QString& known : {
	         QStringLiteral("schema"), QStringLiteral("schemaVersion"), QStringLiteral("shader"),
	         QStringLiteral("secondaryUvEnabled"), QStringLiteral("primaryUv"), QStringLiteral("secondaryUv"),
	         QStringLiteral("tertiaryUvEnabled"), QStringLiteral("tertiaryUv"),
	         QStringLiteral("resources"), QStringLiteral("settings")}) {
		m_unknownRoot.remove(known);
	}
    m_modified = false;
    return true;
}

QVector<SurfaceStateSpec> surfaceStatePresets()
{
    // The five original named states, expressed through the generic profile.
    // Constants mirror the legacy SurfaceStates/* defaults; the directional /
    // ripple enhancements are enabled where they make physical sense.
    SurfaceStateSpec wetness;
    wetness.slot = 0; wetness.name = QStringLiteral("Wetness");
    wetness.tagLiquid = true;
    // Darken mode multiplies toward the raw color: 1-0.65 legacy darkening.
    wetness.color = QStringLiteral("#595959"); wetness.blendMode = 0;
    wetness.porosityInfluence = 1;
    wetness.roughnessTarget = .08; wetness.roughnessWeight = 1;
    wetness.filmF0 = .02;
    wetness.cavityBias = .2;
    wetness.directionality = .2; wetness.streaking = .35; wetness.ripples = true;
    wetness.edgeShedding = .3;
    wetness.gainRate = 1.5; wetness.dryRate = .3; wetness.retention = .3;
    wetness.aoBias = .15; wetness.heightBias = .3; wetness.roughnessBias = .1;
    wetness.metalSeal = .8; wetness.emissiveDry = .4;

    SurfaceStateSpec dust;
    dust.slot = 1; dust.name = QStringLiteral("Dust");
    dust.tagParticulate = true;
    dust.color = QStringLiteral("#B8A88A"); dust.blendMode = 1;
    dust.desaturation = .5;
    dust.roughnessTarget = .4; dust.roughnessWeight = 1; dust.roughnessMode = 1;
    dust.cavityBias = .3;
    dust.directionality = .5;
    dust.edgeShedding = .35;
    dust.gainRate = .7; dust.dryRate = .2;
    dust.aoBias = .35; dust.heightBias = .1; dust.roughnessBias = .25; dust.emissiveDry = .3;

    SurfaceStateSpec frost;
    frost.slot = 2; frost.name = QStringLiteral("Frost");
    frost.tagFrozen = true;
    frost.color = QStringLiteral("#DDE8F0"); frost.blendMode = 2;
    frost.roughnessTarget = .55; frost.roughnessWeight = 1;
    frost.f0Target = .023; frost.f0Weight = 1;
    frost.killMetallic = true;
    frost.sparkle = .35;
    frost.cavityBias = .25;
    frost.directionality = .35;
    frost.edgeShedding = .25;
    frost.gainRate = .8; frost.dryRate = .5;
    frost.aoBias = .2; frost.heightBias = .15; frost.roughnessBias = .15; frost.emissiveDry = .7;

    SurfaceStateSpec heat;
    heat.slot = 3; heat.name = QStringLiteral("Heat");
    heat.tagThermal = true;
    heat.color = QStringLiteral("#B3B3B3"); heat.blendMode = 0;  // char darkening .7
    heat.roughnessTarget = .85; heat.roughnessWeight = .7;
    heat.glowIntensity = 4;
    heat.gainRate = 1.2; heat.dryRate = .6;

    SurfaceStateSpec blood;
    blood.slot = 4; blood.name = QStringLiteral("Blood");
    blood.tagLiquid = true; blood.tagViscous = true;
    blood.color = QStringLiteral("#5A0E0C"); blood.blendMode = 2;
    blood.porosityInfluence = 1;
    blood.roughnessTarget = .45; blood.roughnessWeight = 1;
    blood.filmF0 = .01;
    blood.resistance = .3;
    blood.streaking = .5;
    blood.edgeShedding = .15;
    blood.gainRate = 2; blood.dryRate = .15; blood.retention = .2;
    blood.aoBias = .1; blood.heightBias = .2; blood.metalSeal = .5;

    return {wetness, dust, frost, heat, blood};
}

QVector<SurfaceStateSpec> resolveSurfaceStates(const MaterialDocument& document)
{
    const auto channelIndex = [](const QString& value) {
        return value == QStringLiteral("R") ? 0 : value == QStringLiteral("G") ? 1
            : value == QStringLiteral("B") ? 2 : value == QStringLiteral("A") ? 3 : -1;
    };
    const auto channelName = [](int channel) {
        return channel == 0 ? QStringLiteral("R") : channel == 1 ? QStringLiteral("G")
            : channel == 2 ? QStringLiteral("B") : channel == 3 ? QStringLiteral("A") : QStringLiteral("None");
    };
    // Slots 0-4 default to the factory presets (overridden by any legacy
    // named-group constants, so pre-redesign documents keep their exact look);
    // slots 5-7 default to disabled blanks. Authored SurfaceStates/StateN keys
    // are sparse overrides of those per-slot defaults, so a document that
    // tweaks one key stays consistent with everything the editor displays.
    QVector<SurfaceStateSpec> presets = surfaceStatePresets();
    const QJsonObject wet = document.settingGroup(QStringLiteral("SurfaceStates/Wetness"));
    const QJsonObject dust = document.settingGroup(QStringLiteral("SurfaceStates/Dust"));
    const QJsonObject frost = document.settingGroup(QStringLiteral("SurfaceStates/Frost"));
    const QJsonObject heat = document.settingGroup(QStringLiteral("SurfaceStates/Heat"));
    const QJsonObject blood = document.settingGroup(QStringLiteral("SurfaceStates/Blood"));
    const bool legacy = !wet.isEmpty() || !dust.isEmpty() || !frost.isEmpty() || !heat.isEmpty() || !blood.isEmpty();
    const auto grayHex = [](double value) {
        const int c = std::clamp(int(std::lround(value * 255.0)), 0, 255);
        return QStringLiteral("#%1%1%1").arg(c, 2, 16, QLatin1Char('0')).toUpper();
    };
    if (!wet.isEmpty()) {
        presets[0].color = grayHex(1.0 - wet.value(QStringLiteral("darkening")).toDouble(.65));
        presets[0].roughnessTarget = wet.value(QStringLiteral("wetRoughness")).toDouble(.08);
        presets[0].filmF0 = wet.value(QStringLiteral("filmF0")).toDouble(.02);
    }
    if (!dust.isEmpty()) {
        presets[1].color = dust.value(QStringLiteral("color")).toString(presets[1].color);
        presets[1].roughnessTarget = dust.value(QStringLiteral("roughnessRaise")).toDouble(.4);
        presets[1].desaturation = dust.value(QStringLiteral("desaturation")).toDouble(.5);
    }
    if (!frost.isEmpty()) {
        presets[2].color = frost.value(QStringLiteral("color")).toString(presets[2].color);
        presets[2].roughnessTarget = frost.value(QStringLiteral("frostRoughness")).toDouble(.55);
        presets[2].sparkle = frost.value(QStringLiteral("sparkle")).toDouble(.35);
    }
    if (!heat.isEmpty()) {
        presets[3].glowIntensity = heat.value(QStringLiteral("glowIntensity")).toDouble(4);
        presets[3].color = grayHex(heat.value(QStringLiteral("charDarkening")).toDouble(.7));
        presets[3].roughnessTarget = heat.value(QStringLiteral("charRoughness")).toDouble(.85);
    }
    if (!blood.isEmpty()) {
        presets[4].color = blood.value(QStringLiteral("color")).toString(presets[4].color);
        presets[4].roughnessTarget = .6 - .45 * blood.value(QStringLiteral("wetSheen")).toDouble(.35);
    }
    if (legacy) {
        // Visual parity with the pre-redesign shader: the enhancements did not
        // exist when these documents were authored.
        for (SurfaceStateSpec& state : presets) {
            state.directionality = 0; state.streaking = 0; state.ripples = false;
            state.edgeShedding = 0; state.retention = 0; state.aoBias = 0;
            state.heightBias = 0; state.roughnessBias = 0; state.metalSeal = 0; state.emissiveDry = 0;
        }
    }

    QVector<SurfaceStateSpec> states;
    for (int slot = 0; slot < MaxSurfaceStates; ++slot) {
        SurfaceStateSpec state;
        if (slot < presets.size()) state = presets[slot];
        else { state.enabled = false; state.name = QStringLiteral("State %1").arg(slot + 1); }
        state.slot = slot;
        const QJsonObject group = document.settingGroup(QStringLiteral("SurfaceStates/State%1").arg(slot));
        if (!group.isEmpty()) {
            state.enabled = group.value(QStringLiteral("enabled")).toBool(state.enabled);
            state.name = group.value(QStringLiteral("name")).toString(state.name);
            state.tagLiquid = group.value(QStringLiteral("tagLiquid")).toBool(state.tagLiquid);
            state.tagParticulate = group.value(QStringLiteral("tagParticulate")).toBool(state.tagParticulate);
            state.tagFrozen = group.value(QStringLiteral("tagFrozen")).toBool(state.tagFrozen);
            state.tagThermal = group.value(QStringLiteral("tagThermal")).toBool(state.tagThermal);
            state.tagViscous = group.value(QStringLiteral("tagViscous")).toBool(state.tagViscous);
            state.resistance = group.value(QStringLiteral("resistance")).toDouble(state.resistance);
            state.gainRate = group.value(QStringLiteral("gainRate")).toDouble(state.gainRate);
            state.dryRate = group.value(QStringLiteral("dryRate")).toDouble(state.dryRate);
            state.retention = group.value(QStringLiteral("retention")).toDouble(state.retention);
            state.color = group.value(QStringLiteral("color")).toString(state.color);
            const QString defaultBlend = state.blendMode == 0 ? QStringLiteral("Darken") : (state.blendMode == 2 ? QStringLiteral("Coat") : QStringLiteral("Tint"));
            const QString blend = group.value(QStringLiteral("blendMode")).toString(defaultBlend);
            state.blendMode = blend == QStringLiteral("Darken") ? 0 : (blend == QStringLiteral("Coat") ? 2 : 1);
            state.desaturation = group.value(QStringLiteral("desaturation")).toDouble(state.desaturation);
            state.porosityInfluence = group.value(QStringLiteral("porosityInfluence")).toDouble(state.porosityInfluence);
            state.roughnessTarget = group.value(QStringLiteral("roughnessTarget")).toDouble(state.roughnessTarget);
            state.roughnessWeight = group.value(QStringLiteral("roughnessWeight")).toDouble(state.roughnessWeight);
            const QString defaultRoughMode = state.roughnessMode == 1 ? QStringLiteral("Additive Raise") : QStringLiteral("Toward Target");
            state.roughnessMode = group.value(QStringLiteral("roughnessMode")).toString(defaultRoughMode) == QStringLiteral("Additive Raise") ? 1 : 0;
            state.f0Target = group.value(QStringLiteral("f0Target")).toDouble(state.f0Target);
            state.f0Weight = group.value(QStringLiteral("f0Weight")).toDouble(state.f0Weight);
            state.filmF0 = group.value(QStringLiteral("filmF0")).toDouble(state.filmF0);
            state.killMetallic = group.value(QStringLiteral("killMetallic")).toBool(state.killMetallic);
            state.sparkle = group.value(QStringLiteral("sparkle")).toDouble(state.sparkle);
            state.glowIntensity = group.value(QStringLiteral("glowIntensity")).toDouble(state.glowIntensity);
            state.cavityBias = group.value(QStringLiteral("cavityBias")).toDouble(state.cavityBias);
            state.aoBias = group.value(QStringLiteral("aoBias")).toDouble(state.aoBias);
            state.heightBias = group.value(QStringLiteral("heightBias")).toDouble(state.heightBias);
            state.roughnessBias = group.value(QStringLiteral("roughnessBias")).toDouble(state.roughnessBias);
            state.metalSeal = group.value(QStringLiteral("metalSeal")).toDouble(state.metalSeal);
            state.emissiveDry = group.value(QStringLiteral("emissiveDry")).toDouble(state.emissiveDry);
            state.threshold = group.value(QStringLiteral("threshold")).toDouble(state.threshold);
            state.softness = group.value(QStringLiteral("softness")).toDouble(state.softness);
            state.directionality = group.value(QStringLiteral("directionality")).toDouble(state.directionality);
            state.streaking = group.value(QStringLiteral("streaking")).toDouble(state.streaking);
            state.edgeShedding = group.value(QStringLiteral("edgeShedding")).toDouble(state.edgeShedding);
            state.ripples = group.value(QStringLiteral("ripples")).toBool(state.ripples);
            state.maskChannel = channelIndex(group.value(QStringLiteral("maskChannel")).toString(channelName(state.maskChannel)));
            state.shelterChannel = channelIndex(group.value(QStringLiteral("shelterChannel")).toString(channelName(state.shelterChannel)));
        }
        if (state.enabled) states.append(state);
    }
    return states;
}

QVector<float> surfaceStateInteractions(const QVector<SurfaceStateSpec>& states, const QVector<float>& amounts)
{
    const auto clamp01 = [](float value) { return std::clamp(value, 0.0f, 1.0f); };
    const int count = std::min(int(states.size()), int(amounts.size()));
    QVector<float> effective(states.size(), 0.0f);
    float thermal = 0;
    for (int i = 0; i < count; ++i)
        if (states[i].tagThermal) thermal = std::max(thermal, clamp01(amounts[i]));
    // Thermal dries liquids and melts frozen; melting yields liquid.
    float meltYield = 0;
    for (int i = 0; i < count; ++i) {
        float amount = clamp01(amounts[i]);
        const auto& state = states[i];
        const float susceptibility = float(1.0 - state.resistance) * (state.tagViscous ? .4f : 1.0f);
        if (state.tagFrozen && !state.tagThermal) {
            meltYield = std::max(meltYield, amount * thermal * susceptibility * .5f);
            amount *= 1.0f - thermal * susceptibility;
        }
        if (state.tagLiquid && !state.tagThermal)
            amount *= 1.0f - thermal * susceptibility;
        effective[i] = amount;
    }
    // Melted frost feeds the first non-viscous liquid state.
    for (int i = 0; i < count; ++i)
        if (states[i].tagLiquid && !states[i].tagViscous) { effective[i] = clamp01(effective[i] + meltYield); break; }
    float thinLiquid = 0;
    for (int i = 0; i < count; ++i)
        if (states[i].tagLiquid && !states[i].tagViscous) thinLiquid = std::max(thinLiquid, effective[i]);
    // Thin liquids wash particulates and dilute viscous liquids.
    for (int i = 0; i < count; ++i) {
        const auto& state = states[i];
        const float susceptibility = float(1.0 - state.resistance);
        if (state.tagParticulate)
            effective[i] *= 1.0f - thinLiquid * .8f * susceptibility * (state.tagViscous ? .5f : 1.0f);
        else if (state.tagLiquid && state.tagViscous)
            effective[i] *= 1.0f - thinLiquid * .4f * susceptibility;
        effective[i] = clamp01(effective[i]);
    }
    return effective;
}

} // namespace Pbr
