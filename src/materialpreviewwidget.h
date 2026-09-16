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
        LayerCoverage = 17, EmissiveColor = 18, Curvature = 19, Validation = 20,
        // Converter comparison views: the ORIGINAL FO4/FO76 source maps.
        SourceDiffuse = 21, SourceSpec = 22, SourceNormal = 23, SourceExtra = 24,
        NodeOutput = 25, // whatever the selected graph node produced
        // The untouched source material shaded the way its own game shades it,
        // so the conversion can be judged against what it came from.
        NativeFo4 = 26, NativeFo76 = 27
    };
    // Legacy (specular/gloss) shading parameters read straight off the BGSM.
    struct NativeMaterial {
        bool valid = false;
        bool fallout76 = false;
        QVector3D specularColor{1, 1, 1};
        float specularMult = 1.0f;
        float smoothness = 1.0f;   // used where the source has no gloss map
        float envMapScale = 1.0f;
        bool specularEnabled = true;
        bool emitEnabled = false;
        QVector3D emittanceColor{1, 1, 1};
        float emittanceMult = 1.0f;
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
    void setDebugViewLeft(DebugView view);
    void setDebugSplit(float fraction);
    // Converter comparison: the pre-conversion source maps (diffuse, spec or
    // reflectance, normal, lighting/greyscale) shown by the Source* views.
    void setSourceComparisonImages(const QImage& diffuse, const QImage& spec, const QImage& normal,
        const QImage& extra, const QImage& glow = QImage());
    void setNativeMaterial(const NativeMaterial& material);
    // Shows one graph node's output on the preview surface; a null image
    // returns the viewport to the material.
    // Shades the node graph's assembled maps directly, so the converted result
    // can be previewed before it exists on disk.
    void setLiveMaterialImages(const QImage& base, const QImage& normal,
        const QImage& rmaos, const QImage& emissive);
    void clearLiveMaterial();
    void setNodeOutputImage(const QImage& image);
    bool hasNodeOutput() const { return !m_nodeOutputImage.isNull(); }
    void setBloom(bool enabled, float strength);
    void setTonemapOperator(int op);
    void setTurntable(bool enabled, float degreesPerSecond);
    QImage captureScreenshot();
    void setSsao(bool enabled, float strength);
    void setPathTraced(bool enabled);
    bool pathTraced() const { return m_pathTraced; }
    // Substance-Painter-style flat view: the material on a camera-facing lit
    // plane, orthographic and framed, instead of on the 3D mesh.
    void setFlatView(bool enabled);
    bool flatView() const { return m_flatView; }
    // Repeat count for the 2D view; 1 shows the single authored tile.
    void setFlatTiling(int repeats);
    int flatTiling() const { return m_flatTiling; }
    void setTraceDenoise(bool enabled);
    void setGroundEnabled(bool enabled);
    void setGridEnabled(bool enabled);
    void setFreeCamera(bool enabled);
    void cancelFreeCamera();
    bool freeCamera() const { return m_freeCamera; }
    // Correction-mask picking: while enabled, an unmodified LMB press raycasts
    // the mesh and emits meshTrianglePicked instead of starting a drag.
    void setTrianglePickingEnabled(bool enabled);
    int pickTriangle(const QPointF& widgetPos) const;
    // Barycentric weights of the hit come back through outU/outV when asked.
    int pickTriangle(const QPointF& widgetPos, float* outU, float* outV) const;
    // The UV0 coordinate under the cursor, or an invalid point on a miss. Same
    // raycast as pickTriangle, carrying the barycentric weights through to the
    // vertex UVs — so brush strokes land where the texture actually is, in the
    // 2D view and on the 3D mesh alike.
    QPointF pickUv(const QPointF& widgetPos) const;
    // Brush mode. While enabled an unmodified LMB press-and-drag paints instead
    // of orbiting, emitting maskPainted for each sample along the stroke.
    void setMaskPaintEnabled(bool enabled);
    bool maskPaintEnabled() const { return m_maskPaint; }
    // Radius is a fraction of the 0-1 UV square, so a brush keeps its size on
    // the texture whatever the zoom, exactly like Substance's.
    void setMaskBrushRadius(float uvRadius);
    float maskBrushRadius() const { return m_maskBrushRadius; }
    // The painted mask, drawn over the surface so a stroke is visible before
    // anything is reconverted. Null clears it.
    void setMaskOverlay(const QImage& mask);
    void setHighlightTriangles(const QVector<quint32>& triangles);
    void setShadows(bool enabled, float intensity);
    void setDerivedEnvironmentShadow(bool enabled, float directionalWeight);
    float effectiveKeyIntensity() const { return m_derivedEnvironmentShadow ? 0.0f : m_keyIntensity; }
    // Whether the current document's specular-color map reached the shader as a
    // resolved special layer. Needs no GL context, so a host test can pin the
    // document -> shader-input wiring the F0 lerp depends on.
    bool specularColorLayerBound() const;
    void setComparisonEnabled(bool enabled);
    void setEnvironmentCubemap(const QString& path, const QString& textureRoot);
    static EnvironmentLight deriveEnvironmentLight(const QVector<QImage>& faces, float environmentYaw = 0);
    EnvironmentLight derivedEnvironmentLight(float environmentYaw) const;

signals:
    void cameraChanged();
    void projectionChanged();
    // The divider was dragged in the viewport; the settings field follows it.
    void debugSplitDragged(float fraction);
    void lightAnglesChanged(float yawDegrees, float pitchDegrees);
    void environmentYawDragged(float deltaDegrees);
    void shaderErrorsChanged(const QString& log);
    void meshTrianglePicked(int triangleIndex);
    // One sample along a brush stroke, in UV0. Emitted on press and on every
    // drag move; the viewport does not own the mask, it only says where.
    void maskPainted(const QPointF& uv);

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
        bool useRoughness = false, useMetallic = false, useAo = false, useSpecularWeight = false;
        QVector3D baseFallback{1, 1, 1}, tint{1, 1, 1}, normalFallback{.5f, .5f, 1}, emissiveColor{1, 1, 1};
        float opacity = 1, colorScale = 1, normalStrength = 1;
        float roughness = .5f, metallic = 0, ao = 1, specularWeight = 1, emissiveMask = 1, emissiveIntensity = 0;
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
    void drawMaskHighlight();
    void drawViewportOverlays();
    void drawMaskPaintOverlay(QPainter& painter);
    static QImage tintedMask(const QImage& mask);
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
    std::unique_ptr<QOpenGLShaderProgram> m_bloomProgram;      // bright pass + separable blur
    std::unique_ptr<QOpenGLShaderProgram> m_compositeProgram;  // scene + bloom
    std::unique_ptr<QOpenGLFramebufferObject> m_sceneFbo;      // resolved scene when any post pass is on
    std::unique_ptr<QOpenGLFramebufferObject> m_bloomFboA, m_bloomFboB; // half-res ping-pong
    std::unique_ptr<QOpenGLShaderProgram> m_ssaoProgram;
    std::unique_ptr<QOpenGLFramebufferObject> m_ssaoFboA, m_ssaoFboB;  // half-res AO + blur ping-pong
    GLuint m_sceneDepthTexture = 0; // sampleable D24S8 attached to m_sceneFbo for SSAO
    // Path-traced reference mode: mesh BVH in texture buffers, progressive
    // 1 spp/frame accumulation in float ping-pong targets, tonemap at present.
    std::unique_ptr<QOpenGLShaderProgram> m_traceProgram, m_presentProgram, m_denoiseProgram;
    std::unique_ptr<QOpenGLFramebufferObject> m_accumFboA, m_accumFboB;
    std::unique_ptr<QOpenGLFramebufferObject> m_denoiseFboA, m_denoiseFboB;
    GLuint m_auxTextureA = 0, m_auxTextureB = 0;       // first-hit normal+distance, paired with the accum FBOs
    GLuint m_albedoTextureA = 0, m_albedoTextureB = 0; // first-hit albedo: the denoiser filters radiance/albedo so texture detail survives
    bool m_traceDenoise = true;
    GLuint m_bvhNodesBuffer = 0, m_bvhNodesTexture = 0;         // 2 RGBA32F texels per node
    GLuint m_bvhTrianglesBuffer = 0, m_bvhTrianglesTexture = 0; // 8 RGBA32F texels per triangle
    int m_bvhNodeCount = 0, m_bvhTriangleCount = 0;
    bool m_bvhDirty = true;
    bool m_pathTraced = false;
    bool m_flatView = false;
    int m_flatTiling = 1;
    Shape m_shapeBeforeFlat = Shape::Sphere; // restored when leaving the flat view
    int m_accumSamples = 0;
    int m_traceFrame = 0;
    int m_materialGeneration = 0;
    QByteArray m_traceStateHash;
    void uploadTraceBvh();
    void renderPathTraced(int pixelWidth, int pixelHeight);
    void uploadSourceComparisonTextures();
    std::unique_ptr<QOpenGLVertexArrayObject> m_gridVao;
    QOpenGLBuffer m_gridBuffer{QOpenGLBuffer::VertexBuffer};
    int m_gridVertexCount = 0;
    bool m_trianglePicking = false;
    bool m_maskPaint = false;
    bool m_painting = false;          // LMB held with the brush armed
    float m_maskBrushRadius = 0.04f;  // fraction of the UV square
    QPointF m_brushCursor{-1, -1};    // widget space, for the ring
    QImage m_maskOverlay;
    QVector<quint32> m_highlightTriangles;
    std::unique_ptr<QOpenGLVertexArrayObject> m_highlightVao;
    QOpenGLBuffer m_highlightIndexBuffer{QOpenGLBuffer::IndexBuffer};
    int m_highlightIndexCount = 0;
    bool m_highlightDirty = false;
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
    QVector2D m_interiorWindowMask{1, 0};
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
    float m_opacity = 1, m_colorScale = 1, m_normalStrength = 1, m_roughness = .5f, m_metallic = 0, m_ao = 1, m_specularWeight = 1, m_emissiveMask = 1, m_emissiveIntensity = 0;
    bool m_hasBase = false, m_hasNormal = false, m_hasRmaos = false, m_hasEmissive = false;
    bool m_overrideBaseColor = true, m_overrideOpacity = true, m_overrideNormal = true;
    bool m_overrideEmissiveColor = true, m_overrideEmissiveMask = true;
    bool m_useRoughness = false, m_useMetallic = false, m_useAo = false, m_useSpecularWeight = false;
    // v5 surface response: porosity rides RMAOS.A, signed curvature rides
    // Normal.A, adhesion/shelter/mask bindings come from SurfaceResponse.
    bool m_rmaosAlphaPorosity = false, m_porosityOverride = false, m_curvatureInAlpha = false, m_sssAlphaAuthored = false;
    // v6 specular model (docs/PBRM-v6-Specular.md): the material's specular
    // IOR scalar, what texel 255 of an IOR map means, and whether the specular
    // colour slot's alpha is that map.
    float m_specularIor = 1.5f, m_specularIorMax = 4.25f;
    bool m_iorInAlpha = false;
    float m_porosityConstant = .5f, m_cavitySpecOcclusion = 1.0f, m_stateAdhesion = 1.0f;
    int m_shelterChannel = -1;
    QColor m_secondaryBaseColor = Qt::white, m_secondaryTint = Qt::white;
    QColor m_secondaryNormalColor = QColor(QStringLiteral("#8080FF")), m_secondaryEmissiveColor = Qt::white;
    float m_secondaryOpacity = 1, m_secondaryColorScale = 1, m_secondaryNormalStrength = 1;
    float m_secondaryRoughness = .5f, m_secondaryMetallic = 0, m_secondaryAo = 1, m_secondarySpecularWeight = 1;
    float m_secondaryEmissiveMask = 1, m_secondaryEmissiveIntensity = 0, m_constantCoverage = 1;
    bool m_hasSecondaryBase = false, m_hasSecondaryNormal = false, m_hasSecondaryRmaos = false, m_hasSecondaryEmissive = false, m_hasLayerCoverage = false;
    bool m_overrideLayerCoverage = true, m_invertLayerCoverage = false;
    bool m_overrideSecondaryBaseColor = true, m_overrideSecondaryOpacity = true, m_overrideSecondaryNormal = true;
    bool m_overrideSecondaryEmissiveColor = true, m_overrideSecondaryEmissiveMask = true;
    bool m_useSecondaryRoughness = false, m_useSecondaryMetallic = false, m_useSecondaryAo = false, m_useSecondarySpecularWeight = false;
    int m_secondaryBlendMode = 0, m_coverageMode = 1, m_patternSource = 3;
    float m_coverageBias = 0, m_coverageContrast = 1, m_coverageSoftness = .1f;
    float m_layerCoverageFallback = 1, m_layerCoverageSlotBias = 0, m_layerCoverageSlotContrast = 1;
    // Tertiary (UV2) overlay state — mirrors the secondary members above.
    QColor m_tertiaryBaseColor = Qt::white, m_tertiaryTint = Qt::white;
    QColor m_tertiaryNormalColor = QColor(QStringLiteral("#8080FF")), m_tertiaryEmissiveColor = Qt::white;
    float m_tertiaryOpacity = 1, m_tertiaryColorScale = 1, m_tertiaryNormalStrength = 1;
    float m_tertiaryRoughness = .5f, m_tertiaryMetallic = 0, m_tertiaryAo = 1, m_tertiarySpecularWeight = 1;
    float m_tertiaryEmissiveMask = 1, m_tertiaryEmissiveIntensity = 0, m_tConstantCoverage = 1;
    bool m_hasTertiaryBase = false, m_hasTertiaryNormal = false, m_hasTertiaryRmaos = false, m_hasTertiaryEmissive = false, m_hasTertiaryCoverage = false;
    bool m_overrideTertiaryCoverage = true, m_invertTertiaryCoverage = false;
    bool m_overrideTertiaryBaseColor = true, m_overrideTertiaryOpacity = true, m_overrideTertiaryNormal = true;
    bool m_overrideTertiaryEmissiveColor = true, m_overrideTertiaryEmissiveMask = true;
    bool m_useTertiaryRoughness = false, m_useTertiaryMetallic = false, m_useTertiaryAo = false, m_useTertiarySpecularWeight = false;
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
    DebugView m_debugViewLeft = DebugView::Shaded; // left half of the split view
    // Converter source maps: diffuse, spec/reflectance, normal, lighting, glow.
    std::unique_ptr<QOpenGLTexture> m_sourceTextures[5];
    QImage m_sourceImages[5];
    NativeMaterial m_nativeMaterial;
    bool m_sourceDirty = false;
    std::unique_ptr<QOpenGLTexture> m_nodeOutputTexture;
    QImage m_nodeOutputImage;
    bool m_nodeOutputDirty = false;
    float m_debugSplit = 0.0f; // 0 = the map fills the view; else shaded left of the divider
    bool m_splitDrag = false;  // the divider is being dragged
    bool m_liveMaterial = false; // primary maps come from the graph, not paths
    // Screen X of a split fraction. Flat view anchors the split to the
    // texture, so drawing, hit-testing and dragging must all ask here
    // rather than each computing a screen fraction of their own.
    double splitScreenX(double fraction) const;
    bool nearDebugSplit(const QPointF& point) const;
    void dragDebugSplit(const QPointF& point);
    bool m_bloomEnabled = true;
    float m_bloomStrength = 0.4f;
    int m_tonemapOperator = 2; // 0 ACES, 1 AgX, 2 Reinhard (legacy default), 3 None
    QTimer* m_turntableTimer = nullptr;
    float m_turntableSpeed = 20.0f;
    qint64 m_turntableLastMs = 0;
    bool m_screenshotMode = false; // true while grabFramebuffer captures without overlays
    bool m_ssaoEnabled = true;
    float m_ssaoStrength = 0.5f;
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
    explicit MaterialPreviewWidget(QWidget* parent = nullptr, bool converterMode = false);
    void setTextureRoot(const QString& textureRoot);
    // Drives the render-mode combo from outside (the converter selects a graph
    // node and wants its output on the surface).
    void setRenderMode(PbrPreviewGL::DebugView view);
    // The left half of the split; the converter pins the PBR result there.
    void setRenderModeLeft(PbrPreviewGL::DebugView view);
    void setMaterial(const Pbr::MaterialDocument& document);
    void reloadPreviewCubemaps();
    PbrPreviewGL* canvas() const { return m_materialCanvas; }
    // The left-half picker only means anything while the split is showing a
    // selected node against the material, so the converter hides it otherwise.
    void setSplitPickerVisible(bool visible);
    // It also offers only what the selected node can actually show:
    //   Views    — the full render-mode list, for anything unclassified.
    //   Channels — R/G/B/A, for a node that outputs one image. "Original
    //              (FO76)" means nothing when you are looking at a single map.
    //   Maps     — the shaded material plus one entry per map the node carries,
    //              for a node that stands for a whole material.
    enum class SplitPicker { Views, Channels, Maps };
    // One row of the map list. A header carries no id and goes in as a disabled
    // caption, the way the render-mode list already groups itself.
    struct MapEntry {
        QString id;            // port id, or "port#mask" for one map inside it
        QString label;
        bool header = false;
        bool operator==(const MapEntry& other) const
        {
            return id == other.id && label == other.label && header == other.header;
        }
    };
    // maps is read only in Maps mode. Re-sending the same mode and list is a
    // no-op, so a re-evaluation cannot reset the choice out from under the user.
    void setSplitPickerMode(SplitPicker mode, const QVector<MapEntry>& maps = {});
    // Points the picker at a map without emitting. Two different nodes can
    // carry the same map list — Combine and the terminal do — so an unchanged
    // list is not proof the shown entry still matches the node.
    void setSplitPickerCurrentMap(const QString& portId);

signals:
    // Emitted when the left picker is in channel mode. The value is a mask of
    // 1=R 2=G 4=B 8=A, matching NodeGraph's ChannelBit without needing the
    // graph headers here.
    void splitChannelsChanged(int channelMask);
    // Emitted in map mode. Empty means the "Material" entry: shade the whole
    // thing rather than isolate one of its maps.
    void splitMapChanged(const QString& portId);

public:
public slots:
    void inspectTexture(const QString& name, const QString& path, const QStringList& channels, const QColor& color);
    void showFullMaterial();
private:
    void rebuildChannelButtons(const QStringList& channels);
    void buildPreviewControls(QVBoxLayout* layout);
    // Floating controls in the viewport's top-right corner: a 3D/2D switch and
    // the display-channel picker. They drive the Display-page controls rather
    // than holding their own state, so the two can never disagree.
    void buildViewportOverlay();
    void layoutViewportOverlay();

public:
    // A caller-owned strip floated over the bottom of the viewport. The preview
    // owns WHERE it sits; whoever supplies it owns what is in it, so a tool
    // that only makes sense to one workspace does not have to be understood
    // here. Pass nullptr to take it away.
    void setViewportToolbar(QWidget* toolbar);

private:
    QWidget* m_viewportToolbar = nullptr;
    bool eventFilter(QObject* watched, QEvent* event) override; // keeps the overlay in the corner
    QToolButton* m_overlayDimension = nullptr;
    QComboBox* m_overlayChannel = nullptr;
    QComboBox* m_overlayChannelLeft = nullptr;
    SplitPicker m_splitPickerMode = SplitPicker::Views;
    QVector<MapEntry> m_splitPickerMaps;
    void fillViewPicker(QComboBox* picker);
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
    QComboBox* m_tonemap = nullptr;
    QComboBox* m_debugLeft = nullptr;
    const bool m_converterMode = false; // source-map views + session-scoped 50% split default
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
    DragNumericEdit* m_debugSplit = nullptr;
    QCheckBox* m_bloom = nullptr;
    DragNumericEdit* m_bloomStrength = nullptr;
    QCheckBox* m_ssao = nullptr;
    DragNumericEdit* m_ssaoStrength = nullptr;
    QCheckBox* m_pathTrace = nullptr;
    QCheckBox* m_traceDenoise = nullptr;
    QCheckBox* m_flatView = nullptr;
    QCheckBox* m_flatTiling = nullptr;
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
