#pragma once

#include <QString>
#include <QVector3D>

#include <functional>

// Read-only Bethesda material parser for the FO4/FO76 -> PBR converter
// (docs/FO4-Conversion.md). Layouts transcribed from ousnius's Material
// Editor (MaterialLib/BaseMaterialFile.cs, BGSM.cs, BGEM.cs — GPLv3, same
// license family as this project). Version 2 is Fallout 4; versions 3-22 are
// Fallout 76 eras. The editor never writes or edits these formats.
namespace Bgsm {

enum class Kind { Bgsm, Bgem };

enum class AlphaBlendMode { Unknown, None, Standard, Additive, Multiplicative };

struct Material {
    Kind kind = Kind::Bgsm;
    quint32 version = 2;

    // ---- Shared header (BaseMaterialFile) ----
    bool tileU = true, tileV = true;
    float uOffset = 0, vOffset = 0, uScale = 1, vScale = 1;
    float alpha = 1;
    AlphaBlendMode blendMode = AlphaBlendMode::None;
    quint8 alphaTestRef = 128;
    bool alphaTest = false;
    bool zBufferWrite = true, zBufferTest = true;
    bool screenSpaceReflections = false, wetnessControlScreenSpaceReflections = false;
    bool decal = false, twoSided = false, decalNoFade = false, nonOccluder = false;
    bool refraction = false, refractionFalloff = false;
    float refractionPower = 0;
    bool environmentMapping = false;          // header slot in v<10; BGEM re-reads at v>=10
    float environmentMappingMaskScale = 1;
    bool depthBias = false;                   // replaces the header envmap pair at v>=10
    bool grayscaleToPaletteColor = false;
    quint8 maskWrites = 63;                   // v>=6

    // ---- BGSM textures ----
    QString diffuseTexture, normalTexture, smoothSpecTexture, greyscaleTexture;
    // v==2 (FO4):
    QString envmapTexture, innerLayerTexture, displacementTexture;
    // shared/v>2 (FO76):
    QString glowTexture, wrinklesTexture, specularTexture, lightingTexture, flowTexture;
    QString distanceFieldAlphaTexture;        // v>=17

    // ---- BGSM parameters ----
    bool enableEditorAlphaRef = false;
    bool translucency = false, translucencyThickObject = false, translucencyMixAlbedo = false; // v>=8
    QVector3D translucencySubsurfaceColor{1, 1, 1};
    float translucencyTransmissiveScale = 0, translucencyTurbulence = 0;
    bool rimLighting = false;                 // v<8
    float rimPower = 2.0f, backLightPower = 0;
    bool subsurfaceLighting = false;
    float subsurfaceLightingRolloff = 0.3f;
    bool specularEnabled = false;
    QVector3D specularColor{1, 1, 1};
    float specularMult = 1.0f, smoothness = 1.0f, fresnelPower = 5.0f;
    float wetnessSpecScale = -1, wetnessSpecPowerScale = -1, wetnessSpecMinvar = -1;
    float wetnessEnvMapScale = -1;            // v<10
    float wetnessFresnelPower = -1, wetnessMetalness = -1;
    bool pbr = false;                         // v>2 (FO76 PBR flag)
    bool customPorosity = false;              // v>=9
    float porosityValue = 0;
    QString rootMaterialPath;
    bool anisoLighting = false;
    bool emitEnabled = false;
    QVector3D emittanceColor{1, 1, 1};        // present in stream only when emitEnabled
    float emittanceMult = 1.0f;
    bool modelSpaceNormals = false, externalEmittance = false;
    float lumEmittance = 0;                   // v>=12
    bool useAdaptiveEmissive = false;         // v>=13
    float adaptiveEmissiveExposureOffset = 0, adaptiveEmissiveExposureMin = 0, adaptiveEmissiveExposureMax = 0;
    bool backLighting = false;                // v<8
    bool receiveShadows = false, hideSecret = false, castShadows = false, dissolveFade = false, assumeShadowmask = false;
    bool glowmap = false;
    bool environmentMappingWindow = false, environmentMappingEye = false; // v<7
    bool hair = false;
    QVector3D hairTintColor{.5f, .5f, .5f};
    bool tree = false, facegen = false, skinTint = false, tessellate = false;
    float displacementTextureBias = -0.5f, displacementTextureScale = 10.0f; // v<3
    float tessellationPnScale = 1.0f, tessellationBaseFactor = 1.0f, tessellationFadeDistance = 0;
    float grayscaleToPaletteScale = 1.0f;
    bool skewSpecularAlpha = false;           // v>=1
    bool terrain = false;                     // v>=3
    float terrainThresholdFalloff = 0, terrainTilingDistance = 0, terrainRotationAngle = 0;

    // ---- BGEM textures & parameters ----
    QString baseTexture, envmapMaskTexture;   // + grayscale/envmap/normal shared above
    QString glassRoughnessScratch, glassDirtOverlay; // v>=21
    bool glassEnabled = false;
    QVector3D glassFresnelColor{1, 1, 1};
    float glassRefractionScaleBase = 0, glassBlurScaleBase = 0, glassBlurScaleFactor = 0;
    bool bloodEnabled = false, effectLightingEnabled = false;
    bool falloffEnabled = false, falloffColorEnabled = false;
    bool grayscaleToPaletteAlpha = false, softEnabled = false;
    QVector3D baseColor{1, 1, 1};
    float baseColorScale = 1.0f;
    float falloffStartAngle = 0, falloffStopAngle = 0, falloffStartOpacity = 0, falloffStopOpacity = 0;
    float lightingInfluence = 0;
    quint8 envmapMinLOD = 0;
    float softDepth = 0;
    bool effectPbrSpecular = false;           // v>=20

    bool isFallout76() const { return version > 2; }
};

// Parses a complete .bgsm/.bgem byte stream (kind chosen by signature).
bool read(const QByteArray& data, Material* material, QString* error = nullptr);
bool readFile(const QString& fileName, Material* material, QString* error = nullptr);

// Root-material template chains: children frequently leave texture strings
// empty and rely on sRootMaterialPath. This borrows empty texture fields from
// the resolved root (recursively, cycle-guarded); scalar fields always stay
// the child's, because the binary format stores them completely. The lookup
// returns the raw bytes for a material path (loose or BA2) or false.
using MaterialLookup = std::function<bool(const QString& path, QByteArray* data)>;
bool resolveRootChain(Material* material, const MaterialLookup& lookup, QString* error = nullptr);

} // namespace Bgsm
