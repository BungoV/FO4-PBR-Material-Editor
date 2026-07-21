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
    static constexpr quint32 Version = 4u;

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

// Generic surface state slots (SurfaceStates/State0..7). Each state carries a
// name (the runtime binding ID), behavior tags for the interaction physics,
// and a generic response profile. Legacy named groups (SurfaceStates/Wetness,
// Dust, Frost, Heat, Blood) resolve to preset-derived slots at read time —
// the document itself is never mutated, so old files stay byte-identical.
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
    QString color = QStringLiteral("#FFFFFF");
    int blendMode = 1;                  // 0 darken (raw multiplier), 1 tint, 2 coat
    double desaturation = 0;
    double porosityInfluence = 0;       // how much response porosity modulates the state
    double roughnessTarget = .5, roughnessWeight = 1;
    int roughnessMode = 0;              // 0 toward target, 1 additive raise
    double f0Target = .04, f0Weight = 0;
    double filmF0 = 0;                  // max-style liquid film reflectance
    bool killMetallic = false;
    double sparkle = 0, glowIntensity = 0;
    double cavityBias = 0, threshold = 0, softness = .3;
    double directionality = 0;          // accumulation weight toward up-facing surfaces
    double streaking = 0;               // gravity-elongated accumulation on vertical surfaces
    double edgeShedding = 0;            // exposed edges (response B < 0.5) shed accumulation
    bool ripples = false;               // animated liquid detail on up-facing surfaces
    int maskChannel = -1;               // dedicated StateMasks channel, -1 = shared response map
    int shelterChannel = -1;            // vertex-color channel that keeps areas clean, -1 = off
};

QVector<SurfaceStateSpec> surfaceStatePresets();
QVector<SurfaceStateSpec> resolveSurfaceStates(const MaterialDocument& document);
// Applies the tag interaction physics to raw runtime amounts, returning
// effective amounts (same order as the specs).
QVector<float> surfaceStateInteractions(const QVector<SurfaceStateSpec>& states, const QVector<float>& amounts);

} // namespace Pbr
