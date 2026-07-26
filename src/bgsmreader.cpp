#include "bgsmreader.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <cstring>

namespace Bgsm {

namespace {

constexpr quint32 BgsmSignature = 0x4D534742u; // 'BGSM'
constexpr quint32 BgemSignature = 0x4D454742u; // 'BGEM'

// Little-endian cursor with a sticky failure flag: any out-of-bounds read
// poisons the parse instead of returning garbage.
struct Cursor {
    const QByteArray& data;
    int offset = 0;
    bool ok = true;

    bool has(int bytes) { ok = ok && offset + bytes <= data.size(); return ok; }
    quint8 u8() { if (!has(1)) return 0; return quint8(data[offset++]); }
    bool boolean() { return u8() != 0; }
    quint32 u32() {
        if (!has(4)) return 0;
        const auto* bytes = reinterpret_cast<const uchar*>(data.constData() + offset);
        offset += 4;
        return quint32(bytes[0]) | quint32(bytes[1]) << 8 | quint32(bytes[2]) << 16 | quint32(bytes[3]) << 24;
    }
    float f32() { const quint32 raw = u32(); float value; memcpy(&value, &raw, 4); return value; }
    QVector3D color() { const float r = f32(), g = f32(), b = f32(); return {r, g, b}; }
    QString string() {
        const quint32 length = u32();
        if (!ok || length > 4096 || !has(int(length))) { ok = false; return {}; }
        QByteArray raw = data.mid(offset, int(length));
        offset += int(length);
        // Length includes a trailing null.
        if (raw.endsWith('\0')) raw.chop(1);
        return QString::fromLatin1(raw);
    }
};

AlphaBlendMode blendModeFor(quint8 a, quint32 b, quint32 c)
{
    if (a == 0 && b == 6 && c == 7) return AlphaBlendMode::Unknown;
    if (a == 0 && b == 0 && c == 0) return AlphaBlendMode::None;
    if (a == 1 && b == 6 && c == 7) return AlphaBlendMode::Standard;
    if (a == 1 && b == 6 && c == 0) return AlphaBlendMode::Additive;
    if (a == 1 && b == 4 && c == 1) return AlphaBlendMode::Multiplicative;
    return AlphaBlendMode::Unknown;
}

// BaseMaterialFile::Deserialize — the shared header.
void readHeader(Cursor& cursor, Material& material)
{
    material.version = cursor.u32();
    const quint32 tileFlags = cursor.u32();
    material.tileU = (tileFlags & 2) != 0;
    material.tileV = (tileFlags & 1) != 0;
    material.uOffset = cursor.f32();
    material.vOffset = cursor.f32();
    material.uScale = cursor.f32();
    material.vScale = cursor.f32();
    material.alpha = cursor.f32();
    const quint8 blend0 = cursor.u8();
    const quint32 blend1 = cursor.u32();
    const quint32 blend2 = cursor.u32();
    material.blendMode = blendModeFor(blend0, blend1, blend2);
    material.alphaTestRef = cursor.u8();
    material.alphaTest = cursor.boolean();
    material.zBufferWrite = cursor.boolean();
    material.zBufferTest = cursor.boolean();
    material.screenSpaceReflections = cursor.boolean();
    material.wetnessControlScreenSpaceReflections = cursor.boolean();
    material.decal = cursor.boolean();
    material.twoSided = cursor.boolean();
    material.decalNoFade = cursor.boolean();
    material.nonOccluder = cursor.boolean();
    material.refraction = cursor.boolean();
    material.refractionFalloff = cursor.boolean();
    material.refractionPower = cursor.f32();
    if (material.version < 10) {
        material.environmentMapping = cursor.boolean();
        material.environmentMappingMaskScale = cursor.f32();
    } else {
        material.depthBias = cursor.boolean();
    }
    material.grayscaleToPaletteColor = cursor.boolean();
    if (material.version >= 6)
        material.maskWrites = cursor.u8();
}

void readBgsm(Cursor& cursor, Material& material)
{
    material.diffuseTexture = cursor.string();
    material.normalTexture = cursor.string();
    material.smoothSpecTexture = cursor.string();
    material.greyscaleTexture = cursor.string();
    if (material.version > 2) {
        material.glowTexture = cursor.string();
        material.wrinklesTexture = cursor.string();
        material.specularTexture = cursor.string();
        material.lightingTexture = cursor.string();
        material.flowTexture = cursor.string();
        if (material.version >= 17)
            material.distanceFieldAlphaTexture = cursor.string();
    } else {
        material.envmapTexture = cursor.string();
        material.glowTexture = cursor.string();
        material.innerLayerTexture = cursor.string();
        material.wrinklesTexture = cursor.string();
        material.displacementTexture = cursor.string();
    }
    material.enableEditorAlphaRef = cursor.boolean();
    if (material.version >= 8) {
        material.translucency = cursor.boolean();
        material.translucencyThickObject = cursor.boolean();
        material.translucencyMixAlbedo = cursor.boolean();
        material.translucencySubsurfaceColor = cursor.color();
        material.translucencyTransmissiveScale = cursor.f32();
        material.translucencyTurbulence = cursor.f32();
    } else {
        material.rimLighting = cursor.boolean();
        material.rimPower = cursor.f32();
        material.backLightPower = cursor.f32();
        material.subsurfaceLighting = cursor.boolean();
        material.subsurfaceLightingRolloff = cursor.f32();
    }
    material.specularEnabled = cursor.boolean();
    material.specularColor = cursor.color();
    material.specularMult = cursor.f32();
    material.smoothness = cursor.f32();
    material.fresnelPower = cursor.f32();
    material.wetnessSpecScale = cursor.f32();
    material.wetnessSpecPowerScale = cursor.f32();
    material.wetnessSpecMinvar = cursor.f32();
    if (material.version < 10)
        material.wetnessEnvMapScale = cursor.f32();
    material.wetnessFresnelPower = cursor.f32();
    material.wetnessMetalness = cursor.f32();
    if (material.version > 2) {
        material.pbr = cursor.boolean();
        if (material.version >= 9) {
            material.customPorosity = cursor.boolean();
            material.porosityValue = cursor.f32();
        }
    }
    material.rootMaterialPath = cursor.string();
    material.anisoLighting = cursor.boolean();
    material.emitEnabled = cursor.boolean();
    if (material.emitEnabled)
        material.emittanceColor = cursor.color();
    material.emittanceMult = cursor.f32();
    material.modelSpaceNormals = cursor.boolean();
    material.externalEmittance = cursor.boolean();
    if (material.version >= 12)
        material.lumEmittance = cursor.f32();
    if (material.version >= 13) {
        material.useAdaptiveEmissive = cursor.boolean();
        material.adaptiveEmissiveExposureOffset = cursor.f32();
        material.adaptiveEmissiveExposureMin = cursor.f32();
        material.adaptiveEmissiveExposureMax = cursor.f32();
    }
    if (material.version < 8)
        material.backLighting = cursor.boolean();
    material.receiveShadows = cursor.boolean();
    material.hideSecret = cursor.boolean();
    material.castShadows = cursor.boolean();
    material.dissolveFade = cursor.boolean();
    material.assumeShadowmask = cursor.boolean();
    material.glowmap = cursor.boolean();
    if (material.version < 7) {
        material.environmentMappingWindow = cursor.boolean();
        material.environmentMappingEye = cursor.boolean();
    }
    material.hair = cursor.boolean();
    material.hairTintColor = cursor.color();
    material.tree = cursor.boolean();
    material.facegen = cursor.boolean();
    material.skinTint = cursor.boolean();
    material.tessellate = cursor.boolean();
    if (material.version < 3) {
        material.displacementTextureBias = cursor.f32();
        material.displacementTextureScale = cursor.f32();
        material.tessellationPnScale = cursor.f32();
        material.tessellationBaseFactor = cursor.f32();
        material.tessellationFadeDistance = cursor.f32();
    }
    material.grayscaleToPaletteScale = cursor.f32();
    if (material.version >= 1)
        material.skewSpecularAlpha = cursor.boolean();
    if (material.version >= 3) {
        material.terrain = cursor.boolean();
        if (material.terrain) {
            if (material.version == 3)
                cursor.u32(); // UnkInt1
            material.terrainThresholdFalloff = cursor.f32();
            material.terrainTilingDistance = cursor.f32();
            material.terrainRotationAngle = cursor.f32();
        }
    }
}

void readBgem(Cursor& cursor, Material& material)
{
    material.baseTexture = cursor.string();
    material.greyscaleTexture = cursor.string();
    material.envmapTexture = cursor.string();
    material.normalTexture = cursor.string();
    material.envmapMaskTexture = cursor.string();
    if (material.version >= 11) {
        material.specularTexture = cursor.string();
        material.lightingTexture = cursor.string();
        material.glowTexture = cursor.string();
    }
    if (material.version >= 21) {
        material.glassRoughnessScratch = cursor.string();
        material.glassDirtOverlay = cursor.string();
        material.glassEnabled = cursor.boolean();
        if (material.glassEnabled) {
            material.glassFresnelColor = cursor.color();
            material.glassBlurScaleBase = cursor.f32();
            if (material.version >= 22)
                material.glassBlurScaleFactor = cursor.f32();
            material.glassRefractionScaleBase = cursor.f32();
        }
    }
    if (material.version >= 10) {
        material.environmentMapping = cursor.boolean();
        material.environmentMappingMaskScale = cursor.f32();
    }
    material.bloodEnabled = cursor.boolean();
    material.effectLightingEnabled = cursor.boolean();
    material.falloffEnabled = cursor.boolean();
    material.falloffColorEnabled = cursor.boolean();
    material.grayscaleToPaletteAlpha = cursor.boolean();
    material.softEnabled = cursor.boolean();
    material.baseColor = cursor.color();
    material.baseColorScale = cursor.f32();
    material.falloffStartAngle = cursor.f32();
    material.falloffStopAngle = cursor.f32();
    material.falloffStartOpacity = cursor.f32();
    material.falloffStopOpacity = cursor.f32();
    material.lightingInfluence = cursor.f32();
    material.envmapMinLOD = cursor.u8();
    material.softDepth = cursor.f32();
    if (material.version >= 11)
        material.emittanceColor = cursor.color();
    if (material.version >= 15) {
        material.adaptiveEmissiveExposureOffset = cursor.f32();
        material.adaptiveEmissiveExposureMin = cursor.f32();
        material.adaptiveEmissiveExposureMax = cursor.f32();
    }
    if (material.version >= 16)
        material.glowmap = cursor.boolean();
    if (material.version >= 20)
        material.effectPbrSpecular = cursor.boolean();
}

// Materials also ship as JSON in the wild (the Material Editor's DataMember
// names; the engine accepts them). Keys are sparse over the binary defaults.
bool readJsonMaterial(const QJsonObject& object, Kind kindHint, bool hasKindHint, Material* material, QString* error)
{
    Material parsed;
    const auto jstr = [&object](const char* key, const QString& fallback = {}) {
        return object.value(QLatin1String(key)).toString(fallback);
    };
    const auto jbool = [&object](const char* key, bool fallback = false) {
        return object.value(QLatin1String(key)).toBool(fallback);
    };
    const auto jfloat = [&object](const char* key, float fallback = 0.0f) {
        return float(object.value(QLatin1String(key)).toDouble(fallback));
    };
    const auto jcolor = [&object](const char* key, const QVector3D& fallback) {
        const QString hex = object.value(QLatin1String(key)).toString();
        if (hex.size() != 7 || !hex.startsWith(QLatin1Char('#'))) return fallback;
        bool ok = false;
        const uint value = hex.mid(1).toUInt(&ok, 16);
        if (!ok) return fallback;
        return QVector3D(((value >> 16) & 0xFF) / 255.0f, ((value >> 8) & 0xFF) / 255.0f, (value & 0xFF) / 255.0f);
    };
    parsed.kind = hasKindHint ? kindHint
        : ((object.contains(QLatin1String("sBaseTexture")) || object.contains(QLatin1String("bBloodEnabled")))
            ? Kind::Bgem : Kind::Bgsm);
    // JSON carries no version; route the FO76 conversion profile off marker keys.
    const bool fallout76 = object.contains(QLatin1String("bPBR")) || object.contains(QLatin1String("bTranslucency"))
        || object.contains(QLatin1String("bCustomPorosity")) || object.contains(QLatin1String("sLightingTexture"))
        || object.contains(QLatin1String("bEffectPbrSpecular")) || object.contains(QLatin1String("bGlassEnabled"));
    parsed.version = fallout76 ? 20 : 2;
    // Shared header.
    parsed.tileU = jbool("bTileU", true);
    parsed.tileV = jbool("bTileV", true);
    parsed.uOffset = jfloat("fUOffset");
    parsed.vOffset = jfloat("fVOffset");
    parsed.uScale = jfloat("fUScale", 1.0f);
    parsed.vScale = jfloat("fVScale", 1.0f);
    parsed.alpha = jfloat("fAlpha", 1.0f);
    const QString blend = jstr("eAlphaBlendMode", QStringLiteral("None"));
    parsed.blendMode = blend.compare(QLatin1String("Standard"), Qt::CaseInsensitive) == 0 ? AlphaBlendMode::Standard
        : blend.compare(QLatin1String("Additive"), Qt::CaseInsensitive) == 0 ? AlphaBlendMode::Additive
        : blend.compare(QLatin1String("Multiplicative"), Qt::CaseInsensitive) == 0 ? AlphaBlendMode::Multiplicative
        : blend.compare(QLatin1String("Unknown"), Qt::CaseInsensitive) == 0 ? AlphaBlendMode::Unknown
        : AlphaBlendMode::None;
    parsed.alphaTestRef = quint8(object.value(QLatin1String("fAlphaTestRef")).toDouble(128));
    parsed.alphaTest = jbool("bAlphaTest");
    parsed.zBufferWrite = jbool("bZBufferWrite", true);
    parsed.zBufferTest = jbool("bZBufferTest", true);
    parsed.screenSpaceReflections = jbool("bScreenSpaceReflections");
    parsed.wetnessControlScreenSpaceReflections = jbool("bWetnessControl_ScreenSpaceReflections");
    parsed.decal = jbool("bDecal");
    parsed.twoSided = jbool("bTwoSided");
    parsed.decalNoFade = jbool("bDecalNoFade");
    parsed.nonOccluder = jbool("bNonOccluder");
    parsed.refraction = jbool("bRefraction");
    parsed.refractionPower = jfloat("fRefractionPower");
    parsed.environmentMapping = jbool("bEnvironmentMapping");
    parsed.environmentMappingMaskScale = jfloat("fEnvironmentMappingMaskScale", 1.0f);
    parsed.depthBias = jbool("bDepthBias");
    parsed.grayscaleToPaletteColor = jbool("bGrayscaleToPaletteColor");
    if (parsed.kind == Kind::Bgsm) {
        parsed.diffuseTexture = jstr("sDiffuseTexture");
        parsed.normalTexture = jstr("sNormalTexture");
        parsed.smoothSpecTexture = jstr("sSmoothSpecTexture");
        parsed.greyscaleTexture = jstr("sGreyscaleTexture");
        parsed.envmapTexture = jstr("sEnvmapTexture");
        parsed.glowTexture = jstr("sGlowTexture");
        parsed.innerLayerTexture = jstr("sInnerLayerTexture");
        parsed.wrinklesTexture = jstr("sWrinklesTexture");
        parsed.displacementTexture = jstr("sDisplacementTexture");
        parsed.specularTexture = jstr("sSpecularTexture");
        parsed.lightingTexture = jstr("sLightingTexture");
        parsed.flowTexture = jstr("sFlowTexture");
        parsed.distanceFieldAlphaTexture = jstr("sDistanceFieldAlphaTexture");
        parsed.enableEditorAlphaRef = jbool("bEnableEditorAlphaRef");
        parsed.translucency = jbool("bTranslucency");
        parsed.translucencyThickObject = jbool("bTranslucencyThickObject");
        parsed.translucencyMixAlbedo = jbool("bTranslucencyMixAlbedoWithSubsurfaceColor");
        parsed.translucencySubsurfaceColor = jcolor("cTranslucencySubsurfaceColor", parsed.translucencySubsurfaceColor);
        parsed.translucencyTransmissiveScale = jfloat("fTranslucencyTransmissiveScale");
        parsed.translucencyTurbulence = jfloat("fTranslucencyTurbulence");
        parsed.rimLighting = jbool("bRimLighting");
        parsed.rimPower = jfloat("fRimPower", 2.0f);
        parsed.backLightPower = jfloat("fBackLightPower");
        parsed.subsurfaceLighting = jbool("bSubsurfaceLighting");
        parsed.subsurfaceLightingRolloff = jfloat("fSubsurfaceLightingRolloff", 0.3f);
        parsed.specularEnabled = jbool("bSpecularEnabled");
        parsed.specularColor = jcolor("cSpecularColor", parsed.specularColor);
        parsed.specularMult = jfloat("fSpecularMult", 1.0f);
        parsed.smoothness = jfloat("fSmoothness", 1.0f);
        parsed.fresnelPower = jfloat("fFresnelPower", 5.0f);
        parsed.wetnessSpecScale = jfloat("fWetnessControl_SpecScale", -1.0f);
        parsed.wetnessSpecPowerScale = jfloat("fWetnessControl_SpecPowerScale", -1.0f);
        parsed.wetnessSpecMinvar = jfloat("fWetnessControl_SpecMinvar", -1.0f);
        parsed.wetnessEnvMapScale = jfloat("fWetnessControl_EnvMapScale", -1.0f);
        parsed.wetnessFresnelPower = jfloat("fWetnessControl_FresnelPower", -1.0f);
        parsed.wetnessMetalness = jfloat("fWetnessControl_Metalness", -1.0f);
        parsed.pbr = jbool("bPBR");
        parsed.customPorosity = jbool("bCustomPorosity");
        parsed.porosityValue = jfloat("fPorosityValue");
        parsed.rootMaterialPath = jstr("sRootMaterialPath");
        parsed.anisoLighting = jbool("bAnisoLighting");
        parsed.emitEnabled = jbool("bEmitEnabled");
        parsed.emittanceColor = jcolor("cEmittanceColor", parsed.emittanceColor);
        parsed.emittanceMult = jfloat("fEmittanceMult", 1.0f);
        parsed.modelSpaceNormals = jbool("bModelSpaceNormals");
        parsed.externalEmittance = jbool("bExternalEmittance");
        parsed.lumEmittance = jfloat("fLumEmittance");
        parsed.useAdaptiveEmissive = jbool("bUseAdaptativeEmissive");
        parsed.adaptiveEmissiveExposureOffset = jfloat("fAdaptativeEmissive_ExposureOffset");
        parsed.adaptiveEmissiveExposureMin = jfloat("fAdaptativeEmissive_FinalExposureMin");
        parsed.adaptiveEmissiveExposureMax = jfloat("fAdaptativeEmissive_FinalExposureMax");
        parsed.backLighting = jbool("bBackLighting");
        parsed.receiveShadows = jbool("bReceiveShadows");
        parsed.hideSecret = jbool("bHideSecret");
        parsed.castShadows = jbool("bCastShadows");
        parsed.dissolveFade = jbool("bDissolveFade");
        parsed.assumeShadowmask = jbool("bAssumeShadowmask");
        parsed.glowmap = jbool("bGlowmap");
        parsed.environmentMappingWindow = jbool("bEnvironmentMappingWindow");
        parsed.environmentMappingEye = jbool("bEnvironmentMappingEye");
        parsed.hair = jbool("bHair");
        parsed.hairTintColor = jcolor("cHairTintColor", parsed.hairTintColor);
        parsed.tree = jbool("bTree");
        parsed.facegen = jbool("bFacegen");
        parsed.skinTint = jbool("bSkinTint");
        parsed.tessellate = jbool("bTessellate");
        parsed.grayscaleToPaletteScale = jfloat("fGrayscaleToPaletteScale", 1.0f);
        parsed.skewSpecularAlpha = jbool("bSkewSpecularAlpha");
        parsed.terrain = jbool("bTerrain");
        parsed.terrainThresholdFalloff = jfloat("fTerrainThresholdFalloff");
        parsed.terrainTilingDistance = jfloat("fTerrainTilingDistance");
        parsed.terrainRotationAngle = jfloat("fTerrainRotationAngle");
    } else {
        parsed.baseTexture = jstr("sBaseTexture");
        parsed.greyscaleTexture = jstr("sGrayscaleTexture");
        parsed.envmapTexture = jstr("sEnvmapTexture");
        parsed.normalTexture = jstr("sNormalTexture");
        parsed.envmapMaskTexture = jstr("sEnvmapMaskTexture");
        parsed.specularTexture = jstr("sSpecularTexture");
        parsed.lightingTexture = jstr("sLightingTexture");
        parsed.glowTexture = jstr("sGlowTexture");
        parsed.glassRoughnessScratch = jstr("sGlassRoughnessScratch");
        parsed.glassDirtOverlay = jstr("sGlassDirtOverlay");
        parsed.glassEnabled = jbool("bGlassEnabled");
        parsed.glassFresnelColor = jcolor("cGlassFresnelColor", parsed.glassFresnelColor);
        parsed.glassRefractionScaleBase = jfloat("fGlassRefractionScaleBase");
        parsed.glassBlurScaleBase = jfloat("fGlassBlurScaleBase");
        parsed.glassBlurScaleFactor = jfloat("fGlassBlurScaleFactor");
        parsed.bloodEnabled = jbool("bBloodEnabled");
        parsed.effectLightingEnabled = jbool("bEffectLightingEnabled");
        parsed.falloffEnabled = jbool("bFalloffEnabled");
        parsed.falloffColorEnabled = jbool("bFalloffColorEnabled");
        parsed.grayscaleToPaletteAlpha = jbool("bGrayscaleToPaletteAlpha");
        parsed.softEnabled = jbool("bSoftEnabled");
        parsed.baseColor = jcolor("cBaseColor", parsed.baseColor);
        parsed.baseColorScale = jfloat("fBaseColorScale", 1.0f);
        parsed.falloffStartAngle = jfloat("fFalloffStartAngle");
        parsed.falloffStopAngle = jfloat("fFalloffStopAngle");
        parsed.falloffStartOpacity = jfloat("fFalloffStartOpacity");
        parsed.falloffStopOpacity = jfloat("fFalloffStopOpacity");
        parsed.lightingInfluence = jfloat("fLightingInfluence");
        parsed.envmapMinLOD = quint8(object.value(QLatin1String("iEnvmapMinLOD")).toDouble(0));
        parsed.softDepth = jfloat("fSoftDepth");
        parsed.emittanceColor = jcolor("cEmittanceColor", parsed.emittanceColor);
        parsed.glowmap = jbool("bGlowmap");
        parsed.effectPbrSpecular = jbool("bEffectPbrSpecular");
    }
    Q_UNUSED(error);
    *material = parsed;
    return true;
}

bool readBinary(const QByteArray& data, Material* material, QString* error)
{
    Cursor cursor{data};
    const quint32 signature = cursor.u32();
    Material parsed;
    if (signature == BgsmSignature) parsed.kind = Kind::Bgsm;
    else if (signature == BgemSignature) parsed.kind = Kind::Bgem;
    else {
        if (error) *error = QStringLiteral("Not a BGSM/BGEM material (invalid signature).");
        return false;
    }
    readHeader(cursor, parsed);
    if (parsed.version > 22) {
        if (error) *error = QStringLiteral("Unsupported material version %1 (known versions are 2-22).").arg(parsed.version);
        return false;
    }
    if (parsed.kind == Kind::Bgsm) readBgsm(cursor, parsed);
    else readBgem(cursor, parsed);
    if (!cursor.ok) {
        if (error) *error = QStringLiteral("Material data is truncated or malformed.");
        return false;
    }
    *material = parsed;
    return true;
}

bool readDetect(const QByteArray& data, Kind kindHint, bool hasKindHint, Material* material, QString* error)
{
    if (!material) return false;
    // JSON-form materials start with '{' after optional whitespace/BOM.
    int first = 0;
    while (first < data.size() && (uchar(data[first]) <= ' ' || uchar(data[first]) == 0xEF
        || uchar(data[first]) == 0xBB || uchar(data[first]) == 0xBF))
        ++first;
    if (first < data.size() && data[first] == '{') {
        QJsonParseError parseError;
        const QJsonDocument json = QJsonDocument::fromJson(data.mid(first), &parseError);
        if (parseError.error != QJsonParseError::NoError || !json.isObject()) {
            if (error) *error = QStringLiteral("Invalid JSON material: %1").arg(parseError.errorString());
            return false;
        }
        return readJsonMaterial(json.object(), kindHint, hasKindHint, material, error);
    }
    return readBinary(data, material, error);
}

} // namespace

bool read(const QByteArray& data, Material* material, QString* error)
{
    return readDetect(data, Kind::Bgsm, false, material, error);
}

bool readFile(const QString& fileName, Material* material, QString* error)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    // The extension decides the kind for JSON materials; binary self-identifies.
    const bool bgem = fileName.endsWith(QStringLiteral(".bgem"), Qt::CaseInsensitive);
    return readDetect(file.readAll(), bgem ? Kind::Bgem : Kind::Bgsm, true, material, error);
}

bool resolveRootChain(Material* material, const MaterialLookup& lookup, QString* error)
{
    if (!material || !lookup) return false;
    // Case/slash-normalized visited set guards template cycles.
    const auto normalized = [](const QString& path) {
        QString key = path.toLower();
        key.replace(QLatin1Char('/'), QLatin1Char('\\'));
        return key;
    };
    QSet<QString> visited;
    Material* child = material;
    Material root;
    QString rootPath = child->rootMaterialPath;
    while (!rootPath.trimmed().isEmpty()) {
        const QString key = normalized(rootPath);
        if (visited.contains(key)) {
            // Revisiting a template is not a failure, and vanilla Fallout 4
            // depends on that: Template/DefaultTemplate_Wet.bgsm names ITSELF
            // as its root, and roughly 3400 of the 6616 vanilla materials
            // inherit through it. Treating that as an error flagged half the
            // corpus over a template that had already resolved correctly.
            //
            // Stopping here loses nothing either way. Every pass only fills
            // fields the child left EMPTY, so a template already walked has no
            // remaining field it could contribute -- a second lap is a no-op by
            // construction, whether the loop is a deliberate self-reference or
            // a genuine A->B->A authoring mistake. The chain is complete; the
            // only real failure is a root that cannot be resolved at all, and
            // that is still reported below.
            return true;
        }
        visited.insert(key);
        QByteArray data;
        if (!lookup(rootPath, &data)) {
            if (error) *error = QStringLiteral("Root material could not be resolved: %1").arg(rootPath);
            return false;
        }
        if (!read(data, &root, error)) return false;
        if (root.kind != material->kind) {
            if (error) *error = QStringLiteral("Root material kind differs from the child: %1").arg(rootPath);
            return false;
        }
        // Borrow textures the child left empty — the well-defined half of
        // template inheritance (the binary stores scalars completely, so
        // scalar fields always stay the child's).
        const auto borrow = [](QString& childField, const QString& rootField) {
            if (childField.trimmed().isEmpty()) childField = rootField;
        };
        borrow(material->diffuseTexture, root.diffuseTexture);
        borrow(material->normalTexture, root.normalTexture);
        borrow(material->smoothSpecTexture, root.smoothSpecTexture);
        borrow(material->greyscaleTexture, root.greyscaleTexture);
        borrow(material->envmapTexture, root.envmapTexture);
        borrow(material->glowTexture, root.glowTexture);
        borrow(material->innerLayerTexture, root.innerLayerTexture);
        borrow(material->wrinklesTexture, root.wrinklesTexture);
        borrow(material->displacementTexture, root.displacementTexture);
        borrow(material->specularTexture, root.specularTexture);
        borrow(material->lightingTexture, root.lightingTexture);
        borrow(material->flowTexture, root.flowTexture);
        borrow(material->distanceFieldAlphaTexture, root.distanceFieldAlphaTexture);
        borrow(material->baseTexture, root.baseTexture);
        borrow(material->envmapMaskTexture, root.envmapMaskTexture);
        borrow(material->glassRoughnessScratch, root.glassRoughnessScratch);
        borrow(material->glassDirtOverlay, root.glassDirtOverlay);
        rootPath = root.rootMaterialPath;
        child = &root;
    }
    return true;
}

} // namespace Bgsm
