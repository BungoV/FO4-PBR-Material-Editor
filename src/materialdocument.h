#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace Pbr {

enum class ShaderMode {
    StandardSurface,
    VegetationSurface,
    HairSurface,
    GlassSurface,
    WaterSurface,
    AdvancedSubsurface,
    EyeSurface,
    LandscapeSurface,
    AllFeatures,
    UnlitEffect,
    SimpleLitEffect,
    PhysicallyLitEffect
};

QString shaderModeName(ShaderMode mode);
QString shaderModeDescription(ShaderMode mode);
QStringList shaderModeNames();
ShaderMode shaderModeFromName(const QString& name);

struct TextureSlotState {
    bool enabled = false;
    bool expanded = true;
    QString path;
    QJsonObject values;

    QJsonObject toJson() const;
    static TextureSlotState fromJson(const QJsonObject& object);
};

class MaterialDocument {
public:
    static constexpr quint32 Magic = 0x4D524250u;
    // v5: surface state behavior moved out of the material into runtime-owned
    // definitions; the material keeps physical maps (porosity in RMAOS.A,
    // signed curvature in Normal.A) plus the small SurfaceResponse bindings.
    // v4 files load with an in-memory migration and save as v5.
    // v6 (docs/PBRM-v6-Specular.md): RMAOS.A is OpenPBR specular_weight, the
    // specular colour's alpha is an IOR map, the material carries specularIor
    // and specularIorMax scalars, and the normal's alpha is a selector
    // (None/Curvature/Cone Map). v4 and v5 files load with in-memory
    // migrations; texture repacks are left to validation and `pbrm migrate`.
    static constexpr quint32 Version = 6u;
    static constexpr quint32 LegacyVersion = 4u;

    MaterialDocument();

    void reset();
    bool load(const QString& fileName, QString* error = nullptr);
    bool save(const QString& fileName, QString* error = nullptr) const;

    ShaderMode shaderMode() const;
    void setShaderMode(ShaderMode mode);

    bool secondaryUvEnabled() const;
    void setSecondaryUvEnabled(bool enabled);

    bool tertiaryUvEnabled() const;
    void setTertiaryUvEnabled(bool enabled);

    TextureSlotState textureSlot(const QString& uvSet, const QString& key) const;
    void setTextureSlot(const QString& uvSet, const QString& key, const TextureSlotState& state);
    void removeTextureSlot(const QString& uvSet, const QString& key);
    QJsonObject textureSet(const QString& setName) const;
    void setTextureSet(const QString& setName, const QJsonObject& set);
    void clearTextureSet(const QString& setName);

    QJsonValue setting(const QString& section, const QString& key, const QJsonValue& fallback = {}) const;
    void setSetting(const QString& section, const QString& key, const QJsonValue& value);
    QJsonObject settingGroup(const QString& section) const;
    void setSettingGroup(const QString& section, const QJsonObject& group);
    void clearSettingGroup(const QString& section);

    QString fileName() const;
    void setFileName(const QString& fileName);
    bool modified() const;
    void setModified(bool modified);

    QJsonObject toJson() const;
    bool fromJson(const QJsonObject& object, QString* error = nullptr);

private:
    ShaderMode m_shaderMode = ShaderMode::StandardSurface;
    bool m_secondaryUvEnabled = false;
    bool m_tertiaryUvEnabled = false;
    QJsonObject m_uv0;
    QJsonObject m_uv1;
    // Serialized sparsely: "tertiaryUv"/"tertiaryUvEnabled" are only written
    // when authored, so pre-tertiary documents round-trip byte-identically.
    QJsonObject m_uv2;
    QJsonObject m_resources;
    QJsonObject m_settings;
	// Unknown additive v4 root members are preserved across load/save. This lets
	// an older editor safely edit known fields without destroying future metadata
	// or runtime capability requirements.
	QJsonObject m_unknownRoot;
    QString m_fileName;
    bool m_modified = false;
};

// Surface state behavior definitions. As of PBRM v5 these are runtime-owned
// (FO4.PBRM.SurfaceStateDefinitions JSON shipped with the engine), never
// authored per material; the editor previews and simulates them. Materials
// contribute only physical maps plus the SurfaceResponse bindings (adhesion
// constant, shelter vertex channel, StateMasks channel-name bindings).
constexpr int MaxSurfaceStates = 8;

struct SurfaceStateSpec {
    int slot = 0;
    bool enabled = true;
    QString name;
    // Behavior tags drive the fixed interaction physics: liquids wash
    // particulates, thermal dries liquids and melts frozen (yielding liquid),
    // viscous states resist washing and drying.
    bool tagLiquid = false, tagParticulate = false, tagFrozen = false, tagThermal = false, tagViscous = false;
    double resistance = 0;              // 0 = fully susceptible to suppression
    double gainRate = 1, dryRate = 1;   // multipliers on the runtime's accumulation/decay speed
    double retention = 0;               // porous areas collect first and dry last
    QString color = QStringLiteral("#FFFFFF");
    int blendMode = 1;                  // 0 darken (raw multiplier), 1 tint, 2 coat
    double desaturation = 0;
    double porosityInfluence = 0;       // how much surface porosity modulates the state
    double roughnessTarget = .5, roughnessWeight = 1;
    int roughnessMode = 0;              // 0 toward target, 1 additive raise
    double f0Target = .04, f0Weight = 0;
    double filmF0 = 0;                  // max-style liquid film reflectance
    bool killMetallic = false;
    double sparkle = 0, glowIntensity = 0;
    double cavityBias = 0, threshold = 0, softness = .3;
    double aoBias = 0;                  // free accumulation from the RMAOS AO channel (occluded collects)
    double heightBias = 0;              // low height-map areas pool the state (puddles)
    double roughnessBias = 0;           // rough areas grip the state, polished areas shed
    double metalSeal = 0;               // metallic suppresses porous absorption (wet metal beads, never darkens)
    double emissiveDry = 0;             // glowing-hot areas suppress the state (rain steams off the forge)
    double directionality = 0;          // accumulation weight toward up-facing surfaces
    double streaking = 0;               // gravity-elongated accumulation on vertical surfaces
    double edgeShedding = 0;            // exposed edges (signed curvature < 0.5) shed accumulation
    bool ripples = false;               // animated liquid detail on up-facing surfaces
    bool respectsShelter = false;       // material shelter vertex data suppresses this state (weather yes, blood no)
    // Legacy v4 per-material fields, kept only so the extraction/migration
    // tooling can read pre-v5 documents. Definitions never carry them.
    int maskChannel = -1;               // dedicated StateMasks channel, -1 = shared accumulation
    int shelterChannel = -1;            // vertex-color channel that keeps areas clean, -1 = off
};

QVector<SurfaceStateSpec> surfaceStatePresets();
// Legacy v4 reader: resolves the per-material SurfaceStates/* groups (used by
// the extract-to-definitions migration tooling; the preview no longer reads
// material-authored states).
QVector<SurfaceStateSpec> resolveSurfaceStates(const MaterialDocument& document);
// Applies the tag interaction physics to raw runtime amounts, returning
// effective amounts (same order as the specs).
QVector<float> surfaceStateInteractions(const QVector<SurfaceStateSpec>& states, const QVector<float>& amounts);

// FO4.PBRM.SurfaceStateDefinitions v1 — the runtime-owned definitions asset.
QJsonObject surfaceStateDefinitionsToJson(const QVector<SurfaceStateSpec>& states);
QVector<SurfaceStateSpec> surfaceStateDefinitionsFromJson(const QJsonObject& object, QString* error = nullptr);
bool saveSurfaceStateDefinitions(const QString& fileName, const QVector<SurfaceStateSpec>& states, QString* error = nullptr);
bool loadSurfaceStateDefinitions(const QString& fileName, QVector<SurfaceStateSpec>* states, QString* error = nullptr);
// The definitions the editor previews with: the user-loaded definitions file
// (surfaceStates/definitionsPath in QSettings) when set and readable,
// otherwise the built-in defaults from surfaceStatePresets().
QVector<SurfaceStateSpec> activeSurfaceStateDefinitions(QString* sourcePath = nullptr);

// Legacy (pre-v5) surface-data tooling, shared by the GUI batch operations,
// the validation quick-fixes, and the CLI.
bool hasLegacySurfaceStateGroups(const MaterialDocument& document);
// Removes only the legacy SurfaceStates/* setting groups (the quick-fix
// scope: response slots keep their repack breadcrumb). Returns the count.
int stripLegacySurfaceStateGroups(MaterialDocument& document);
// Removes legacy SurfaceStates/* groups, retired SurfaceStateResponse slots,
// and retired layer-mask keys. Returns the number of removed items.
int stripLegacySurfaceData(MaterialDocument& document);
// Writes the document's resolved legacy states as a definitions JSON.
bool extractLegacyStates(const MaterialDocument& document, const QString& fileName, QString* error = nullptr);

} // namespace Pbr
