#pragma once

#include "materialdocument.h"
#include "previewmesh.h"

#include <QColor>
#include <QFrame>
#include <QHash>
#include <QImage>
#include <QElapsedTimer>
#include <QMatrix4x4>
#include <QOpenGLBuffer>
#include <QOpenGLWidget>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions_3_3_Core>
#include <QStringList>

#include <memory>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class DragNumericEdit;
class QGroupBox;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QSlider;
class QStackedWidget;
class QTabWidget;
class QToolButton;
class QSplitter;
class QTimer;
class QVBoxLayout;
class QOpenGLShaderProgram;
class QOpenGLTexture;
class QOpenGLVertexArrayObject;
class ColorEdit;

class TexturePreviewCanvas final : public QWidget {
    Q_OBJECT
public:
    explicit TexturePreviewCanvas(QWidget* parent = nullptr);
    void showTexture(const QString& name, const QImage& image, const QColor& color, const QString& channel, const QString& error = {});
protected:
    void paintEvent(QPaintEvent* event) override;
private:
    QString m_name;
    QString m_channel = QStringLiteral("RGBA");
    QColor m_color = QColor(112, 137, 113);
    QImage m_image;
    QString m_error;
};

class PbrPreviewGL final : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
    Q_OBJECT
public:
    enum class Shape { Plane, Cube, Sphere, Custom };
    enum class Projection { Perspective, Orthographic };
    enum class Background { Cubemap, Gray, Black, White, Checkerboard };
    enum class DebugView {
        Shaded = 0, BaseColor = 1, Normals = 2, Roughness = 3, Metallic = 4,
        AmbientOcclusion = 5, F0 = 6, Emissive = 7, UvChecker = 8,
        Wireframe = 9, TangentBasis = 10, Opacity = 11, TangentNormal = 12,
        Height = 13, RoughnessMetallic = 14, Rmaos = 15, EmissiveMask = 16,
        LayerCoverage = 17, EmissiveColor = 18
    };
    enum class AxisView { Front, Back, Left, Right, Top, Bottom };
    struct EnvironmentLight {
        bool valid = false;
        float yaw = 0;
        float pitch = 0;
        float intensity = 0;
        QColor color = Qt::white;
    };

    explicit PbrPreviewGL(QWidget* parent = nullptr);
    ~PbrPreviewGL() override;
    // QOpenGLWidget's 640x480 default hint would let the canvas drive the
    // main splitter wider whenever the settings box (previously the widest
    // child) is collapsed; the canvas has no natural size — it fills.
    QSize sizeHint() const override { return {300, 300}; }
    bool rendererReady() const;
    void setMaterial(const Pbr::MaterialDocument& document, const QString& textureRoot);
    void setShape(Shape shape);
    void setPlaneOptions(PreviewMeshes::PlaneOrientation orientation, bool twoSided);
    bool loadCustomMesh(const QString& path, QString* error = nullptr);
    const PreviewMesh& mesh() const { return m_mesh; }
    void setUvSet(int uvSet);
    void setLandscapeLayer(int layer);
    void setMsaaSamples(int samples);
    void setObjectTransform(const QVector3D& rotationDegrees, const QVector3D& translation);
    void copyCameraFrom(const PbrPreviewGL* other);
    void setSurfaceStateAmounts(const QVector<float>& amounts);
    void setParticleSimulation(float age, bool autoAge, float ageRate, float seed);
    void setTimePlayback(bool paused, float scale);
    void setTimeValue(double seconds);
    double timeValue() const { return m_previewTime; }
    void setSubmesh(int index, bool applyAll);
    void setCompanionMaterials(const QVector<QPair<QString, QString>>& assignments);
    void setProjection(Projection projection);
    Projection projection() const { return m_projection; }
    void setAxisView(AxisView view);
    void frameObject();
    void resetView();
    void setLighting(float keyIntensity, const QColor& keyColor, float keyYaw, float keyPitch,
        float environmentIntensity, float environmentYaw, float exposure);
    void setBackground(Background background);
    void setDebugView(DebugView view);
    void setGroundEnabled(bool enabled);
    void setGridEnabled(bool enabled);
    void setFreeCamera(bool enabled);
    void cancelFreeCamera();
    bool freeCamera() const { return m_freeCamera; }
    void setShadows(bool enabled, float intensity);
    void setDerivedEnvironmentShadow(bool enabled, float directionalWeight);
    float effectiveKeyIntensity() const { return m_derivedEnvironmentShadow ? 0.0f : m_keyIntensity; }
    void setComparisonEnabled(bool enabled);
    void setEnvironmentCubemap(const QString& path, const QString& textureRoot);
    static EnvironmentLight deriveEnvironmentLight(const QVector<QImage>& faces, float environmentYaw = 0);
    EnvironmentLight derivedEnvironmentLight(float environmentYaw) const;

signals:
    void cameraChanged();
    void projectionChanged();
    void lightAnglesChanged(float yawDegrees, float pitchDegrees);
    void shaderErrorsChanged(const QString& log);

protected:
    void initializeGL() override;
    void resizeGL(int width, int height) override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    // Companion material: another .pbrm rendered on assigned submeshes through
    // the standard-surface path (core maps + constants only) while the edited
    // document keeps full fidelity on the remaining submeshes.
    struct CompanionMaterial {
        QString pbrmPath;
        QVector<int> submeshes;
        QImage baseImage, normalImage, rmaosImage, emissiveImage;
        std::unique_ptr<QOpenGLTexture> baseTexture, normalTexture, rmaosTexture, emissiveTexture;
        bool hasBase = false, hasNormal = false, hasRmaos = false, hasEmissive = false;
        bool overrideBaseColor = true, overrideOpacity = true, overrideNormal = true;
        bool overrideEmissiveColor = true, overrideEmissiveMask = true;
        bool useRoughness = false, useMetallic = false, useAo = false, useF0 = false;
        QVector3D baseFallback{1, 1, 1}, tint{1, 1, 1}, normalFallback{.5f, .5f, 1}, emissiveColor{1, 1, 1};
        float opacity = 1, colorScale = 1, normalStrength = 1;
        float roughness = .5f, metallic = 0, ao = 1, f0 = .04f, emissiveMask = 1, emissiveIntensity = 0;
        QVector4D uvTransform{1, 1, 0, 0}, uvAnimation{0, 0, 0, 0};
        float uvRotation = 0;
        int uvAddress = 0;
        int compositionMode = 0, alphaSource = 0, normalSpace = 0;
        float globalOpacity = 1, alphaThreshold = .5f, globalNormalStrength = 1;
        bool twoSided = false;
        float iblIntensity = 1, environmentYaw = 0, diffuseScale = 1, specularScale = 1;
        QString error;
    };
    CompanionMaterial makeCompanionMaterial(const QString& pbrmPath) const;
    void uploadCompanionTextures();
    void applyCompanionUniforms(const CompanionMaterial& companion);

    void uploadTextures();
    void uploadMesh();
    void uploadTexture(std::unique_ptr<QOpenGLTexture>& texture, const QImage& image, const QColor& fallback);
    void createEnvironmentCubemap();
    void createBrdfLut();
    void renderShadowMap();
    QMatrix4x4 viewMatrix() const;
    QMatrix4x4 projectionMatrix() const;
    QVector3D keyDirection() const;
    void applyCommonUniforms(QOpenGLShaderProgram& program);
    QVector3D cameraEye() const;

    // Viewport aids ported from the ClaudeNifskope build: ground grid with
    // axis directions, top-right navigation gizmo, Shift+F free camera.
    void buildGridGeometry();
    void drawGrid();
    void drawViewportOverlays();
    void navGizmoLayout(QPointF& center, float& radius, float& ballRadius) const;
    void navGizmoBalls(QPointF positions[6], float depths[6]) const;
    int navGizmoHitTest(const QPointF& position) const;
    void drawNavGizmo(QPainter& painter);
    void freeCameraTick();

    std::unique_ptr<QOpenGLShaderProgram> m_program;
    std::unique_ptr<QOpenGLShaderProgram> m_backgroundProgram;
    std::unique_ptr<QOpenGLShaderProgram> m_environmentPrefilterProgram;
    std::unique_ptr<QOpenGLShaderProgram> m_brdfIntegrationProgram;
    std::unique_ptr<QOpenGLShaderProgram> m_shadowProgram;
    std::unique_ptr<QOpenGLVertexArrayObject> m_vao;
    std::unique_ptr<QOpenGLVertexArrayObject> m_groundVao;
    std::unique_ptr<QOpenGLShaderProgram> m_lineProgram;
    std::unique_ptr<QOpenGLVertexArrayObject> m_gridVao;
    QOpenGLBuffer m_gridBuffer{QOpenGLBuffer::VertexBuffer};
    int m_gridVertexCount = 0;
    QOpenGLBuffer m_vertexBuffer{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer m_indexBuffer{QOpenGLBuffer::IndexBuffer};
    QOpenGLBuffer m_groundVertexBuffer{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer m_groundIndexBuffer{QOpenGLBuffer::IndexBuffer};
    std::unique_ptr<QOpenGLTexture> m_baseTexture;
    std::unique_ptr<QOpenGLTexture> m_normalTexture;
    std::unique_ptr<QOpenGLTexture> m_heightTexture;
    std::unique_ptr<QOpenGLTexture> m_rmaosTexture;
    std::unique_ptr<QOpenGLTexture> m_emissiveTexture;
    std::unique_ptr<QOpenGLTexture> m_environmentTexture;
    std::unique_ptr<QOpenGLTexture> m_prefilteredEnvironmentTexture;
    std::unique_ptr<QOpenGLTexture> m_brdfLutTexture;
    std::unique_ptr<QOpenGLTexture> m_secondaryBaseTexture, m_secondaryNormalTexture, m_secondaryRmaosTexture;
    std::unique_ptr<QOpenGLTexture> m_secondaryEmissiveTexture, m_layerCoverageTexture;
    std::unique_ptr<QOpenGLTexture> m_tertiaryBaseTexture, m_tertiaryNormalTexture, m_tertiaryRmaosTexture;
    std::unique_ptr<QOpenGLTexture> m_tertiaryEmissiveTexture, m_tertiaryCoverageTexture;
    std::unique_ptr<QOpenGLTexture> m_gradientTexture;
    std::unique_ptr<QOpenGLTexture> m_specialTextureArray;
    std::unique_ptr<QOpenGLFramebufferObject> m_msaaFbo;
    int m_msaaSamples = 4;
    int m_msaaRequested = 0; // guard against per-frame recreation when the driver clamps samples
    std::unique_ptr<QOpenGLTexture> m_interiorTexture;
    QVector<QImage> m_interiorFaces;
    bool m_interiorEnabled = false;
    float m_interiorRoomsU = 1, m_interiorRoomsV = 1, m_interiorDepth = 1, m_interiorBrightness = 1;
    float m_interiorEmissive = 0;
    GLuint m_shadowFramebuffer = 0;
    GLuint m_shadowDepthTexture = 0;
    QMatrix4x4 m_shadowMatrix;
    QVector<QImage> m_environmentFaces;
    EnvironmentLight m_cachedEnvironmentLight;
    float m_prefilteredEnvironmentMaxLod = 0;
    bool m_environmentDirty = false;
    QImage m_baseImage, m_normalImage, m_heightImage, m_rmaosImage, m_emissiveImage;
    QImage m_secondaryBaseImage, m_secondaryNormalImage, m_secondaryRmaosImage, m_secondaryEmissiveImage, m_layerCoverageImage;
    QImage m_tertiaryBaseImage, m_tertiaryNormalImage, m_tertiaryRmaosImage, m_tertiaryEmissiveImage, m_tertiaryCoverageImage;
    QImage m_gradientImage;
    QVector<QImage> m_specialImages;
    int m_specialMask = 0;
    Pbr::MaterialDocument m_document;
    QString m_textureRoot;
    PreviewMesh m_mesh;
    Shape m_shape = Shape::Plane;
    PreviewMeshes::PlaneOrientation m_planeOrientation = PreviewMeshes::PlaneOrientation::XY;
    bool m_planeTwoSided = true;
    bool m_displacementTessellation = false;
    int m_uvSet = 0;
    int m_landscapeLayer = 0;
    int m_submesh = 0;
    bool m_applyAllSubmeshes = true;
    std::vector<CompanionMaterial> m_companions;
    bool m_companionsDirty = false;
    QColor m_baseColor = Qt::white, m_tint = Qt::white, m_normalColor = QColor(QStringLiteral("#8080FF")), m_emissiveColor = Qt::white, m_keyColor = Qt::white;
    float m_opacity = 1, m_colorScale = 1, m_normalStrength = 1, m_roughness = .5f, m_metallic = 0, m_ao = 1, m_f0 = .04f, m_emissiveMask = 1, m_emissiveIntensity = 0;
    bool m_hasBase = false, m_hasNormal = false, m_hasRmaos = false, m_hasEmissive = false;
    bool m_overrideBaseColor = true, m_overrideOpacity = true, m_overrideNormal = true;
    bool m_overrideEmissiveColor = true, m_overrideEmissiveMask = true;
    bool m_useRoughness = false, m_useMetallic = false, m_useAo = false, m_useF0 = false;
    // v5 surface response: porosity rides RMAOS.A, signed curvature rides
    // Normal.A, adhesion/shelter/mask bindings come from SurfaceResponse.
    bool m_rmaosAlphaPorosity = false, m_porosityOverride = false, m_curvatureInAlpha = false, m_sssAlphaAuthored = false;
    float m_porosityConstant = .5f, m_cavitySpecOcclusion = 1.0f, m_stateAdhesion = 1.0f;
    int m_shelterChannel = -1;
    QColor m_secondaryBaseColor = Qt::white, m_secondaryTint = Qt::white;
    QColor m_secondaryNormalColor = QColor(QStringLiteral("#8080FF")), m_secondaryEmissiveColor = Qt::white;
    float m_secondaryOpacity = 1, m_secondaryColorScale = 1, m_secondaryNormalStrength = 1;
    float m_secondaryRoughness = .5f, m_secondaryMetallic = 0, m_secondaryAo = 1, m_secondaryF0 = .04f;
    float m_secondaryEmissiveMask = 1, m_secondaryEmissiveIntensity = 0, m_constantCoverage = 1;
    bool m_hasSecondaryBase = false, m_hasSecondaryNormal = false, m_hasSecondaryRmaos = false, m_hasSecondaryEmissive = false, m_hasLayerCoverage = false;
    bool m_overrideLayerCoverage = true, m_invertLayerCoverage = false;
    bool m_overrideSecondaryBaseColor = true, m_overrideSecondaryOpacity = true, m_overrideSecondaryNormal = true;
    bool m_overrideSecondaryEmissiveColor = true, m_overrideSecondaryEmissiveMask = true;
    bool m_useSecondaryRoughness = false, m_useSecondaryMetallic = false, m_useSecondaryAo = false, m_useSecondaryF0 = false;
    int m_secondaryBlendMode = 0, m_coverageMode = 1, m_patternSource = 3;
    float m_coverageBias = 0, m_coverageContrast = 1, m_coverageSoftness = .1f;
    float m_layerCoverageFallback = 1, m_layerCoverageSlotBias = 0, m_layerCoverageSlotContrast = 1;
    // Tertiary (UV2) overlay state — mirrors the secondary members above.
    QColor m_tertiaryBaseColor = Qt::white, m_tertiaryTint = Qt::white;
    QColor m_tertiaryNormalColor = QColor(QStringLiteral("#8080FF")), m_tertiaryEmissiveColor = Qt::white;
    float m_tertiaryOpacity = 1, m_tertiaryColorScale = 1, m_tertiaryNormalStrength = 1;
    float m_tertiaryRoughness = .5f, m_tertiaryMetallic = 0, m_tertiaryAo = 1, m_tertiaryF0 = .04f;
    float m_tertiaryEmissiveMask = 1, m_tertiaryEmissiveIntensity = 0, m_tConstantCoverage = 1;
    bool m_hasTertiaryBase = false, m_hasTertiaryNormal = false, m_hasTertiaryRmaos = false, m_hasTertiaryEmissive = false, m_hasTertiaryCoverage = false;
    bool m_overrideTertiaryCoverage = true, m_invertTertiaryCoverage = false;
    bool m_overrideTertiaryBaseColor = true, m_overrideTertiaryOpacity = true, m_overrideTertiaryNormal = true;
    bool m_overrideTertiaryEmissiveColor = true, m_overrideTertiaryEmissiveMask = true;
    bool m_useTertiaryRoughness = false, m_useTertiaryMetallic = false, m_useTertiaryAo = false, m_useTertiaryF0 = false;
    int m_tertiaryBlendMode = 0, m_tCoverageMode = 1, m_tPatternSource = 3;
    float m_tCoverageBias = 0, m_tCoverageContrast = 1, m_tCoverageSoftness = .1f;
    float m_tertiaryCoverageFallback = 1, m_tertiaryCoverageSlotBias = 0, m_tertiaryCoverageSlotContrast = 1;
    bool m_secondaryLayerActive = false, m_tertiaryLayerActive = false;
    bool m_primaryHeightInBlue = false, m_secondaryHeightInBlue = false, m_tertiaryHeightInBlue = false;
    bool m_hasHeight = false, m_heightBlueChannel = false, m_heightInvert = false, m_overrideHeight = true;
    bool m_parallaxEnabled = false, m_parallaxClip = false, m_parallaxDepthOutput = false;
    int m_heightUvSet = 0, m_parallaxMode = 0, m_parallaxMinSteps = 12, m_parallaxMaxSteps = 40;
    int m_parallaxRefinementSteps = 3, m_parallaxShadowSteps = 20;
    float m_heightFallback = .5f, m_heightCenter = .5f, m_heightBias = 0, m_parallaxDepth = .05f, m_parallaxGrazingFade = .08f;
    float m_parallaxShadowStrength = 1, m_parallaxShadowBias = .015f, m_parallaxShadowSoftness = .08f;
    float m_parallaxFadeStart = 512, m_parallaxFadeEnd = 2048;
    // Planar / triplanar coordinate sources: mode 0 mesh UV, 1 world planar,
    // 2 object planar; axis 0 triplanar, 1..3 = X/Y/Z projection plane.
    int m_uvSourceMode[3] = {0, 0, 0};
    int m_uvProjAxis[3] = {0, 0, 0};
    float m_triplanarSharp[3] = {4, 4, 4};
    // Per-layer oscillator ("breathing"): waveform + amplitudes for UV
    // transform, emissive, coverage, and normal strength.
    int m_oscWave[3] = {0, 0, 0};
    float m_oscFreq[3] = {0, 0, 0}, m_oscPhase[3] = {0, 0, 0}, m_oscDuty[3] = {.5f, .5f, .5f};
    float m_oscScaleAmp[3] = {0, 0, 0}, m_oscRotAmp[3] = {0, 0, 0}, m_oscEmissiveAmp[3] = {0, 0, 0};
    float m_oscCoverageAmp[3] = {0, 0, 0}, m_oscNormalAmp[3] = {0, 0, 0};
    QVector2D m_oscOffsetAmp[3];
    QVector4D m_uvTransform0{1, 1, 0, 0}, m_uvTransform1{1, 1, 0, 0}, m_uvTransform2{1, 1, 0, 0};
    QVector4D m_uvAnimation0{0, 0, 0, 0}, m_uvAnimation1{0, 0, 0, 0}, m_uvAnimation2{0, 0, 0, 0};
    float m_uvRotation0 = 0, m_uvRotation1 = 0, m_uvRotation2 = 0;
    int m_uvAddress0 = 0, m_uvAddress1 = 0, m_uvAddress2 = 0;
    bool m_mirrorPrimaryUv = false, m_tertiaryUsesUv0 = false;
    int m_compositionMode = 0, m_normalSpace = 0;
    int m_alphaSource = 0;
    float m_globalOpacity = 1, m_alphaThreshold = .5f, m_globalNormalStrength = 1;
    QColor m_gradientColor = Qt::white;
    bool m_hasGradient = false, m_overrideGradientColor = true, m_overrideGradientOpacity = true;
    int m_gradientDriver = 0;
    float m_gradientOpacity = 1, m_gradientIntensity = 1, m_gradientConstant = .5f;
    float m_unlitIntensity = 1;
    bool m_unlitVertexColor = true, m_unlitExposure = true;
    float m_materialIblIntensity = 1, m_materialDiffuseScale = 1, m_materialSpecularScale = 1, m_materialEnvironmentYaw = 0;
    QElapsedTimer m_previewClock;
    QTimer* m_animationTimer = nullptr;
    bool m_texturesDirty = true, m_meshDirty = true;
    Projection m_projection = Projection::Perspective;
    Background m_background = Background::Cubemap;
    DebugView m_debugView = DebugView::Shaded;
    bool m_groundEnabled = true, m_shadowsEnabled = true, m_comparisonEnabled = false;
    bool m_gridEnabled = true;
    // Navigation gizmo interaction state.
    int m_navHover = -1;      // 0..5 axis balls, 6 orbit ring, -1 none
    bool m_navDrag = false;
    // Shift+F free camera (WASD fly, mouse look; LMB confirms, RMB returns
    // to the pose the flight started from).
    bool m_freeCamera = false;
    QVector3D m_freeEye;
    float m_freeCamSpeed = 1.0f;
    quint32 m_freeKeys = 0;  // bit 0..5 = W A S D Q E, bit 6 = Shift
    QTimer* m_freeTimer = nullptr;
    QElapsedTimer m_freeClock;
    bool m_recentering = false;
    float m_freeEntryYaw = 0, m_freeEntryPitch = 0, m_freeEntryDistance = 3.2f;
    QVector2D m_freeEntryPan;
    float m_shadowIntensity = .75f;
    bool m_derivedEnvironmentShadow = false;
    float m_derivedEnvironmentShadowWeight = 0;
    bool m_depthWrite = true, m_depthTest = true, m_materialTwoSided = false;
    QPointF m_lastMouse;
    Qt::MouseButton m_dragButton = Qt::NoButton;
    QVector2D m_pan;
    QVector3D m_objectRotation, m_objectTranslation;
    QMatrix4x4 objectModelMatrix() const;
    // Generic surface state slots resolved from the document; amounts arrive
    // from the widget with interaction physics already applied.
    QVector<Pbr::SurfaceStateSpec> m_surfaceStates;
    QVector<float> m_stateAmounts;
    // Particle stack (effect families). Index = UV layer; role/modifier entries
    // are meaningful for layers 1 and 2 only.
    bool m_particleActive = false;
    int m_flipbookEnabled[3] = {0, 0, 0};
    QVector2D m_flipbookGrid[3] = {{4, 4}, {4, 4}, {4, 4}};
    int m_flipbookDriver[3] = {0, 0, 0};
    float m_flipbookFps[3] = {0, 0, 0};
    int m_flipbookLoop[3] = {0, 0, 0};
    int m_flipbookBlend[3] = {1, 1, 1};
    float m_flipbookConstantFrame[3] = {0, 0, 0};
    int m_polarUv[3] = {0, 0, 0};
    int m_layerRole[3] = {0, 0, 0};
    float m_distortStrength[3] = {0, 0, 0};
    float m_advectSpeed[3] = {0, 0, 0};
    int m_erodeEnabled[3] = {0, 0, 0};
    float m_erodeSoftness[3] = {.15f, .15f, .15f};
    QVector3D m_erodeEdgeTint[3];
    float m_erodeEdgeGlow[3] = {0, 0, 0};
    int m_paletteMode = 0;
    float m_paletteIntensity = 1;
    int m_randomFlipbookStart = 0;
    float m_randomPhase = 0, m_randomHueJitter = 0;
    float m_particleFresnelFade = 0, m_particleFresnelPower = 2, m_particleNearFade = 0;
    float m_particleAge = .5f, m_particleSeed = 0, m_particleAgeRate = .2f;
    bool m_particleAgeAuto = true;
    // Scrubbable preview time: accumulated by the animation timer instead of
    // reading the wall clock, so pause/scrub/speed work.
    double m_previewTime = 0;
    qint64 m_lastTickMs = 0;
    bool m_timePaused = false;
    float m_timeScale = 1;
    QString m_shaderErrorLog;
    void recordShaderFailure(const char* label, const QString& log);
    // Bottle liquid: damped-spring slosh state, kicked by object-transform deltas.
    bool m_liquidEnabled = false;
    float m_liquidFill = .6f, m_liquidAbsorption = .6f, m_meniscusWidth = .02f, m_liquidSurfaceBrightness = 1.15f;
    float m_wobbleFrequency = 4, m_wobbleDamping = .25f, m_maxTiltDegrees = 25;
    QVector3D m_liquidColor{.48f, .23f, .07f}, m_meniscusColor{.85f, .58f, .35f};
    QVector2D m_wobble, m_wobbleVelocity;
    float m_yaw = 0, m_pitch = 0, m_distance = 3.2f;
    float m_keyYaw = -35, m_keyPitch = 45, m_keyIntensity = 4.0f;
    float m_environmentYaw = 0, m_environmentIntensity = 1.0f, m_exposure = 0;
};

class MaterialPreviewWidget final : public QFrame {
    Q_OBJECT
public:
    explicit MaterialPreviewWidget(QWidget* parent = nullptr);
    void setTextureRoot(const QString& textureRoot);
    void setMaterial(const Pbr::MaterialDocument& document);
    void reloadPreviewCubemaps();
public slots:
    void inspectTexture(const QString& name, const QString& path, const QStringList& channels, const QColor& color);
    void showFullMaterial();
private:
    void rebuildChannelButtons(const QStringList& channels);
    void buildPreviewControls(QVBoxLayout* layout);
    void refreshMeshControls();
    void showSubmeshMaterialsDialog();
    void loadSubmeshAssignments();
    void saveSubmeshAssignments() const;
    void applySubmeshAssignments();
    void rebuildStateControls();
    void applyStateAmounts();
    void tickWeatherSimulation();
    void loadPreviewSettings();
    void savePreviewSettings() const;
    void applyLightingPreset(int index);
    void refreshDerivedEnvironmentLight(bool enableShadows);
    void setDerivedLightingControls(bool derived);
    void captureManualLight();
    void restoreManualLight();
    QColor temperatureColor(int kelvin) const;

    QLabel* m_title = nullptr;
    QStackedWidget* m_stack = nullptr;
    QWidget* m_previewContainer = nullptr;
    PbrPreviewGL* m_materialCanvas = nullptr;
    PbrPreviewGL* m_referenceCanvas = nullptr;
    Pbr::MaterialDocument m_referenceDocument;
    QLabel* m_referenceLabel = nullptr;
    TexturePreviewCanvas* m_textureCanvas = nullptr;
    QWidget* m_buttonHost = nullptr;
    QHBoxLayout* m_buttons = nullptr;
    QToolButton* m_settingsToggle = nullptr;
    QSplitter* m_previewSplitter = nullptr; // viewport above, settings below — user-draggable height
    QWidget* m_settingsBottom = nullptr;
    QGroupBox* m_controls = nullptr;
    QComboBox* m_shape = nullptr;
    QLabel* m_planeOptionsLabel = nullptr;
    QWidget* m_planeOptions = nullptr;
    QComboBox* m_planeOrientation = nullptr;
    QCheckBox* m_twoSided = nullptr;
    QPushButton* m_loadMesh = nullptr;
    QComboBox* m_submesh = nullptr;
    QWidget* m_submeshOptions = nullptr;
    QCheckBox* m_allSubmeshes = nullptr;
    QLabel* m_submeshLabel = nullptr;
    QPushButton* m_submeshMaterials = nullptr;
    QHash<QString, QString> m_submeshAssignments;
    QString m_assignmentsMeshPath;
    QComboBox* m_uvSet = nullptr;
    QLabel* m_uvSetLabel = nullptr;
    QLabel* m_landscapeLayerLabel = nullptr;
    QComboBox* m_landscapeLayer = nullptr;
    QComboBox* m_projection = nullptr;
    QComboBox* m_view = nullptr;
    QComboBox* m_lightingPreset = nullptr;
    QCheckBox* m_deriveExrLighting = nullptr;
    DragNumericEdit* m_keyIntensity = nullptr;
    DragNumericEdit* m_keyYaw = nullptr;
    DragNumericEdit* m_objectRotationX = nullptr;
    DragNumericEdit* m_objectRotationY = nullptr;
    DragNumericEdit* m_objectRotationZ = nullptr;
    DragNumericEdit* m_objectPositionX = nullptr;
    DragNumericEdit* m_objectPositionY = nullptr;
    DragNumericEdit* m_objectPositionZ = nullptr;
    DragNumericEdit* m_keyPitch = nullptr;
    DragNumericEdit* m_shadowIntensity = nullptr;
    DragNumericEdit* m_environmentIntensity = nullptr;
    DragNumericEdit* m_environmentYaw = nullptr;
    DragNumericEdit* m_exposure = nullptr;
    QComboBox* m_cubemap = nullptr;
    QComboBox* m_temperature = nullptr;
    ColorEdit* m_keyColor = nullptr;
    QCheckBox* m_shadows = nullptr;
    QComboBox* m_background = nullptr;
    QComboBox* m_antiAliasing = nullptr;
    QComboBox* m_debug = nullptr;
    QCheckBox* m_ground = nullptr;
    QCheckBox* m_grid = nullptr;
    QCheckBox* m_compare = nullptr;
    QLabel* m_importStatus = nullptr;
    // Dynamic surface-state simulation controls (States tab): one slider per
    // resolved state, tag physics applied CPU-side, optional weather timeline.
    QWidget* m_stateRows = nullptr;
    QVector<DragNumericEdit*> m_stateFields;
    QVector<Pbr::SurfaceStateSpec> m_resolvedStates;
    QComboBox* m_weatherSim = nullptr;
    QTimer* m_weatherTimer = nullptr;
    double m_weatherClock = 0;
    QString m_textureName;
    QColor m_textureColor;
    QImage m_textureImage;
    QString m_textureError, m_textureRoot;
    Pbr::MaterialDocument m_document;
    bool m_deriveExrLightingPreferred = true;
    bool m_applyingLightingPreset = false;
    bool m_applyingDerivedLight = false;
    bool m_manualLightStored = false;
    double m_manualKeyIntensity = 4, m_manualKeyYaw = -35, m_manualKeyPitch = 45;
    QColor m_manualKeyColor = Qt::white;
    int m_manualTemperature = 5500;
    bool m_manualShadows = true;
};
