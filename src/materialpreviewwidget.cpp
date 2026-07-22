#include "materialpreviewwidget.h"
#include "colorpicker.h"
#include "disclosureicon.h"
#include "dragnumericedit.h"
#include "previewenvironmentsettings.h"
#include "texturecache.h"
#include "textureimage.h"
#include "wheelfocusguard.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QSet>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QMessageBox>
#include <QMouseEvent>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QOpenGLVertexArrayObject>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabWidget>
#include <QToolButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace {

void reportShaderFailure(const char* label, const QString& log)
{
    qWarning("PBR preview %s shader failed: %s", label, qPrintable(log));
    const QString path=qEnvironmentVariable("PBRM_SHADER_LOG");
    if(path.isEmpty())return;
    QFile file(path);
    if(file.open(QIODevice::WriteOnly|QIODevice::Append|QIODevice::Text)){
        file.write(QByteArray(label)+" shader failed:\n"+log.toUtf8()+"\n");
    }
}

QImage loadTexture(const Pbr::TextureSlotState& state, const QString& root, int arraySlice = -1)
{
    if (!state.enabled || (state.path.trimmed().isEmpty() && !TextureCache::hasChannelSwaps(state.values))) return {};
    // Memoized: repeated preview rebuilds during editing hit the cache, and
    // authored channel swaps are composited into the effective packed image.
    return TextureCache::packedImage(state, root, arraySlice);
}

float numberValue(const QJsonObject& values, const QString& key, float fallback)
{
    return static_cast<float>(values.value(key).toDouble(fallback));
}

bool boolValue(const QJsonObject& values, const QString& key, bool fallback)
{
    return values.contains(key) ? values.value(key).toBool(fallback) : fallback;
}

DragNumericEdit* spin(double minimum, double maximum, double step, const QString& suffix, QWidget* parent)
{
    auto* value = new DragNumericEdit(parent);
    value->setRange(minimum, maximum); value->setSingleStep(step); value->setDecimals(step < 0.1 ? 2 : 1);
    value->setSuffix(suffix); value->setKeyboardTracking(false); value->setDragSensitivity(step);
    return value;
}

QVector3D cubeDirection(int face, float u, float v)
{
    switch (face) {
    case 0: return QVector3D(1, -v, -u).normalized();
    case 1: return QVector3D(-1, -v, u).normalized();
    case 2: return QVector3D(u, 1, v).normalized();
    case 3: return QVector3D(u, -1, -v).normalized();
    case 4: return QVector3D(u, -v, 1).normalized();
    default: return QVector3D(-u, -v, -1).normalized();
    }
}

QColor linearColor(const QVector3D& value)
{
    const auto encode = [](float v) { return int(std::clamp(std::pow(std::max(v, 0.0f), 1.0f / 2.2f), 0.0f, 1.0f) * 255.0f + .5f); };
    return QColor(encode(value.x()), encode(value.y()), encode(value.z()));
}

enum SpecialLayer {
    TintMaskLayer,
    SurfaceEffectsLayer,
    DynamicNormalLayer,
    SubsurfaceLayer,
    HairDirectionLayer,
    HairScatterLayer,
    TransmissionLayer,
    ImperfectionsLayer,
    WaterFlowLayer,
    FoamLayer,
    SubsurfaceDetailLayer,
    IrisMasksLayer,
    CorneaLayer,
    ClearcoatLayer,
    DistortionLayer,
    LandscapeMacroLayer,
    SecondaryGradientLayer,
    ClearcoatSecondaryLayer,
    ClearcoatTertiaryLayer,
    AnisotropyLayer,
    BentNormalLayer,
    SurfaceStateLayer,
    SixWayALayer,
    SixWayBLayer,
    StateMasksLayer,
    MotionVectorsLayer,
    DetailNormalLayer,
    SpecialLayerCount
};

constexpr int ShadowMapSize = 2048;

QVector3D jsonColor(const QJsonObject& values, const QString& key, const QString& fallback)
{
    QColor color(values.value(key).toString(fallback));if(!color.isValid())color=QColor(fallback);
    return QVector3D(color.redF(),color.greenF(),color.blueF());
}

QImage resolvedPackedImage(const Pbr::TextureSlotState& state, const QString& root,
    const QVector4D& fallback, const QVector4D& useTexture)
{
    if(!state.enabled)return {};
    QImage source=loadTexture(state,root);
    QVector4D resolvedUse=useTexture;
    if(source.isNull()){source=QImage(1,1,QImage::Format_RGBA8888);source.fill(Qt::white);resolvedUse=QVector4D();}
    const QSize bounded=source.size().scaled(QSize(512,512),Qt::KeepAspectRatio);
    if(source.size()!=bounded)source=source.scaled(bounded,Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
    source=source.convertToFormat(QImage::Format_RGBA8888);
    const auto byte=[](float value){return std::clamp(int(value*255.0f+.5f),0,255);};
    for(int y=0;y<source.height();++y){
        QRgb* pixels=reinterpret_cast<QRgb*>(source.scanLine(y));
        for(int x=0;x<source.width();++x){const QColor sampled=QColor::fromRgba(pixels[x]);
            pixels[x]=qRgba(resolvedUse.x()>0?sampled.red():byte(fallback.x()),
                resolvedUse.y()>0?sampled.green():byte(fallback.y()),
                resolvedUse.z()>0?sampled.blue():byte(fallback.z()),
                resolvedUse.w()>0?sampled.alpha():byte(fallback.w()));
        }
    }
    return source;
}

} // namespace

TexturePreviewCanvas::TexturePreviewCanvas(QWidget* parent) : QWidget(parent)
{
    setMinimumSize(250, 300);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void TexturePreviewCanvas::showTexture(const QString& name, const QImage& image, const QColor& color,
    const QString& channel, const QString& error)
{
    m_name = name; m_image = image; m_color = color; m_channel = channel; m_error = error; update();
}

void TexturePreviewCanvas::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    constexpr int tile = 18;
    for (int y = 0; y < height(); y += tile) for (int x = 0; x < width(); x += tile)
        painter.fillRect(QRect(x, y, tile, tile), ((x / tile + y / tile) & 1) ? QColor(38,40,44) : QColor(31,33,36));
    QRect rect(22, 28, qMax(80, width() - 44), qMax(80, height() - 88));
    if (!m_image.isNull()) {
        const QImage isolated = TextureImage::isolateChannel(m_image, m_channel);
        const QSize size = isolated.size().scaled(rect.size(), Qt::KeepAspectRatio);
        const QRect target(rect.center() - QPoint(size.width() / 2, size.height() / 2), size);
        painter.drawImage(target, isolated);
        painter.setPen(QColor(235,240,245));
        painter.drawText(QRect(8, height() - 44, width() - 16, 28), Qt::AlignCenter,
            m_name + QStringLiteral(" • ") + m_channel + QStringLiteral(" • %1×%2").arg(m_image.width()).arg(m_image.height()));
        return;
    }
    QLinearGradient gradient(rect.topLeft(), rect.bottomRight());
    gradient.setColorAt(0, m_color.lighter(140)); gradient.setColorAt(1, m_color.darker(150));
    painter.fillRect(rect, gradient);
    painter.setPen(QColor(235,240,245));
    painter.drawText(rect, Qt::AlignCenter, m_error.isEmpty() ? m_channel : m_error);
}

PbrPreviewGL::PbrPreviewGL(QWidget* parent) : QOpenGLWidget(parent)
{
    setMinimumSize(250, 300);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    m_previewClock.start();
    m_animationTimer = new QTimer(this);
    m_animationTimer->setInterval(16);
    connect(m_animationTimer, &QTimer::timeout, this, [this] {
        const qint64 now = m_previewClock.elapsed();
        const double dt = m_timePaused ? 0.0 : double(now - m_lastTickMs) / 1000.0 * double(m_timeScale);
        m_previewTime += dt;
        if (m_liquidEnabled && dt > 0.0) {
            // Damped spring: the liquid surface tilt oscillates back to level.
            const float omega = 6.2831853f * m_wobbleFrequency;
            const float step = float(std::min(dt, 0.05));
            m_wobbleVelocity += (-(omega * omega) * m_wobble - 2.0f * m_wobbleDamping * omega * m_wobbleVelocity) * step;
            m_wobble += m_wobbleVelocity * step;
        }
        m_lastTickMs = now;
        update();
    });
    setShape(Shape::Plane);
    setToolTip(tr("NifSkope controls: MMB orbit • Shift+MMB pan • RMB dolly • wheel zoom • Shift+LMB rotate key light • F frame • Numpad 1/3/7 axis views • Numpad 5 projection"));
}

PbrPreviewGL::~PbrPreviewGL()
{
    if (!context()) return;
    makeCurrent();
    m_baseTexture.reset(); m_normalTexture.reset(); m_heightTexture.reset(); m_rmaosTexture.reset(); m_emissiveTexture.reset(); m_environmentTexture.reset();
    m_prefilteredEnvironmentTexture.reset(); m_brdfLutTexture.reset();
    m_secondaryBaseTexture.reset(); m_secondaryNormalTexture.reset(); m_secondaryRmaosTexture.reset(); m_secondaryEmissiveTexture.reset(); m_layerCoverageTexture.reset(); m_gradientTexture.reset();m_specialTextureArray.reset();
    m_vertexBuffer.destroy(); m_indexBuffer.destroy(); m_groundVertexBuffer.destroy(); m_groundIndexBuffer.destroy();
    if (m_shadowDepthTexture) glDeleteTextures(1, &m_shadowDepthTexture);
    if (m_shadowFramebuffer) glDeleteFramebuffers(1, &m_shadowFramebuffer);
    m_vao.reset(); m_groundVao.reset(); m_program.reset(); m_backgroundProgram.reset();
    m_environmentPrefilterProgram.reset(); m_brdfIntegrationProgram.reset(); m_shadowProgram.reset();
    doneCurrent();
}

bool PbrPreviewGL::rendererReady() const
{
    return m_program && m_program->isLinked()
        && m_backgroundProgram && m_backgroundProgram->isLinked()
        && m_environmentPrefilterProgram && m_environmentPrefilterProgram->isLinked()
        && m_brdfIntegrationProgram && m_brdfIntegrationProgram->isLinked()
        && m_shadowProgram && m_shadowProgram->isLinked()
        && m_shadowFramebuffer != 0 && m_shadowDepthTexture != 0;
}

void PbrPreviewGL::setMaterial(const Pbr::MaterialDocument& document, const QString& textureRoot)
{
    m_document=document;m_textureRoot=textureRoot;
    // m_uvSet: 0/1/2 preview a single layer, 3 previews the combined stack.
    const int solo=m_uvSet<=2?m_uvSet:0;
    const bool secondary=solo==1;
    const QString set=solo==1?QStringLiteral("UV1"):(solo==2?QStringLiteral("UV2"):QStringLiteral("UV0"));
    const QString prefix=solo==1?QStringLiteral("secondary"):(solo==2?QStringLiteral("tertiary"):QStringLiteral("primary"));
    auto base = document.textureSlot(set, prefix+QStringLiteral("BaseColor"));
    auto normal = document.textureSlot(set, prefix+QStringLiteral("Normal"));
    auto rmaos = document.textureSlot(set, prefix+QStringLiteral("Rmaos"));
    auto emissive = document.textureSlot(set, prefix+QStringLiteral("Emissive"));
    auto gradient = document.textureSlot(set, prefix+QStringLiteral("ParticleGradient"));
    if(document.shaderMode()==Pbr::ShaderMode::LandscapeSurface){
        base=document.textureSlot(QStringLiteral("Landscape"),QStringLiteral("baseColorArray"));
        normal=document.textureSlot(QStringLiteral("Landscape"),QStringLiteral("normalHeightArray"));
        rmaos=document.textureSlot(QStringLiteral("Landscape"),QStringLiteral("rmaosArray"));
        emissive={};gradient={};
    }
    if (base.path.isNull()) base.enabled = true;
    if (normal.path.isNull()) normal.enabled = true;
    if (rmaos.path.isNull()) rmaos.enabled = true;
    const int landscapeSlice=document.shaderMode()==Pbr::ShaderMode::LandscapeSurface?m_landscapeLayer:-1;
    m_baseImage = loadTexture(base, textureRoot, landscapeSlice); m_normalImage = loadTexture(normal, textureRoot, landscapeSlice);
    m_rmaosImage = loadTexture(rmaos, textureRoot, landscapeSlice); m_emissiveImage = loadTexture(emissive, textureRoot);
    m_gradientImage = loadTexture(gradient, textureRoot);
    // Effect materials always composite their layer stack (the particle is the
    // combined result), so overlays stay active in every view mode there.
    m_particleActive = document.shaderMode() == Pbr::ShaderMode::UnlitEffect
        || document.shaderMode() == Pbr::ShaderMode::SimpleLitEffect
        || document.shaderMode() == Pbr::ShaderMode::PhysicallyLitEffect;
    m_secondaryLayerActive = (m_uvSet == 3 || m_particleActive) && document.secondaryUvEnabled();
    m_tertiaryLayerActive = (m_uvSet == 3 || m_particleActive) && document.tertiaryUvEnabled();
    const bool combined = m_secondaryLayerActive;
    const auto secondaryBase = document.textureSlot(QStringLiteral("UV1"), QStringLiteral("secondaryBaseColor"));
    const auto secondaryNormal = document.textureSlot(QStringLiteral("UV1"), QStringLiteral("secondaryNormal"));
    const auto secondaryRmaos = document.textureSlot(QStringLiteral("UV1"), QStringLiteral("secondaryRmaos"));
    const auto secondaryEmissive = document.textureSlot(QStringLiteral("UV1"), QStringLiteral("secondaryEmissive"));
    const auto layerCoverage = document.textureSlot(QStringLiteral("UV1"), QStringLiteral("secondaryLayerCoverage"));
    m_secondaryBaseImage = combined ? loadTexture(secondaryBase, textureRoot) : QImage();
    m_secondaryNormalImage = combined ? loadTexture(secondaryNormal, textureRoot) : QImage();
    m_secondaryRmaosImage = combined ? loadTexture(secondaryRmaos, textureRoot) : QImage();
    m_secondaryEmissiveImage = combined ? loadTexture(secondaryEmissive, textureRoot) : QImage();
    m_layerCoverageImage = combined ? loadTexture(layerCoverage, textureRoot) : QImage();
    const auto tertiaryBase = document.textureSlot(QStringLiteral("UV2"), QStringLiteral("tertiaryBaseColor"));
    const auto tertiaryNormal = document.textureSlot(QStringLiteral("UV2"), QStringLiteral("tertiaryNormal"));
    const auto tertiaryRmaos = document.textureSlot(QStringLiteral("UV2"), QStringLiteral("tertiaryRmaos"));
    const auto tertiaryEmissive = document.textureSlot(QStringLiteral("UV2"), QStringLiteral("tertiaryEmissive"));
    const auto tertiaryCoverage = document.textureSlot(QStringLiteral("UV2"), QStringLiteral("tertiaryLayerCoverage"));
    m_tertiaryBaseImage = m_tertiaryLayerActive ? loadTexture(tertiaryBase, textureRoot) : QImage();
    m_tertiaryNormalImage = m_tertiaryLayerActive ? loadTexture(tertiaryNormal, textureRoot) : QImage();
    m_tertiaryRmaosImage = m_tertiaryLayerActive ? loadTexture(tertiaryRmaos, textureRoot) : QImage();
    m_tertiaryEmissiveImage = m_tertiaryLayerActive ? loadTexture(tertiaryEmissive, textureRoot) : QImage();
    m_tertiaryCoverageImage = m_tertiaryLayerActive ? loadTexture(tertiaryCoverage, textureRoot) : QImage();

    const QString heightSource = document.setting(QStringLiteral("Geometry/Source"), QStringLiteral("source"), QStringLiteral("None")).toString();
    const bool heightLandscape = heightSource.startsWith(QStringLiteral("Landscape"));
    const bool heightSecondary = heightSource.startsWith(QStringLiteral("Secondary"));
    m_heightBlueChannel = heightSource.endsWith(QStringLiteral("Normal Blue"));
    m_heightUvSet = heightSecondary ? 1 : 0;
    const QString heightSet = heightLandscape ? QStringLiteral("Landscape") : (heightSecondary ? QStringLiteral("UV1") : QStringLiteral("UV0"));
    const QString heightPrefix = heightSecondary ? QStringLiteral("secondary") : QStringLiteral("primary");
    auto heightState = document.textureSlot(heightSet,heightLandscape
        ? (m_heightBlueChannel?QStringLiteral("normalHeightArray"):QStringLiteral("heightArray"))
        : heightPrefix+(m_heightBlueChannel?QStringLiteral("Normal"):QStringLiteral("SeparateHeight")));
    const bool heightSourceEnabled = heightSource != QStringLiteral("None")
        && (!m_heightBlueChannel || boolValue(heightState.values, QStringLiteral("heightInBlue"), false));
    m_heightImage = heightSourceEnabled ? loadTexture(heightState, textureRoot, heightLandscape?m_landscapeLayer:-1) : QImage();
    m_overrideHeight = boolValue(heightState.values, QStringLiteral("overrideHeight"), m_heightImage.isNull());
    m_heightFallback = numberValue(heightState.values, QStringLiteral("height"), .5f);
    m_hasHeight = heightSourceEnabled && (m_overrideHeight || !m_heightImage.isNull());
    const bool sourceInvert = boolValue(heightState.values, QStringLiteral("invert"), false);
    m_heightInvert = document.setting(QStringLiteral("Geometry/Source"), QStringLiteral("invert"), false).toBool() != sourceInvert;
    m_heightCenter = static_cast<float>(document.setting(QStringLiteral("Geometry/Source"), QStringLiteral("center"), .5).toDouble());
    m_heightBias = static_cast<float>(document.setting(QStringLiteral("Geometry/Source"), QStringLiteral("bias"), 0.0).toDouble());

    const QJsonObject parallax = document.settingGroup(QStringLiteral("Geometry/Parallax"));
    m_parallaxEnabled = boolValue(parallax, QStringLiteral("enabled"), false);
    const QString parallaxMode = parallax.value(QStringLiteral("mode")).toString(QStringLiteral("Offset"));
    m_parallaxMode = parallaxMode == QStringLiteral("Self-Shadowed POM") ? 3
        : parallaxMode == QStringLiteral("Parallax Occlusion Mapping") ? 2 : 1;
    const QString quality = parallax.value(QStringLiteral("quality")).toString(QStringLiteral("High"));
    if (quality == QStringLiteral("Low")) { m_parallaxMinSteps=6; m_parallaxMaxSteps=12; m_parallaxRefinementSteps=1; m_parallaxShadowSteps=6; }
    else if (quality == QStringLiteral("Medium")) { m_parallaxMinSteps=8; m_parallaxMaxSteps=24; m_parallaxRefinementSteps=2; m_parallaxShadowSteps=12; }
    else if (quality == QStringLiteral("Ultra")) { m_parallaxMinSteps=16; m_parallaxMaxSteps=64; m_parallaxRefinementSteps=4; m_parallaxShadowSteps=32; }
    else { m_parallaxMinSteps=12; m_parallaxMaxSteps=40; m_parallaxRefinementSteps=3; m_parallaxShadowSteps=20; }
    if (boolValue(parallax, QStringLiteral("customQuality"), false)) {
        m_parallaxMinSteps = std::clamp(int(numberValue(parallax, QStringLiteral("minSteps"), 12)), 1, 64);
        m_parallaxMaxSteps = std::clamp(int(numberValue(parallax, QStringLiteral("maxSteps"), 40)), m_parallaxMinSteps, 128);
        m_parallaxRefinementSteps = std::clamp(int(numberValue(parallax, QStringLiteral("refinementSteps"), 3)), 0, 8);
        m_parallaxShadowSteps = std::clamp(int(numberValue(parallax, QStringLiteral("shadowSteps"), 20)), 1, 64);
    }
    m_parallaxDepth = numberValue(parallax, QStringLiteral("depth"), .05f);
    m_parallaxGrazingFade = numberValue(parallax, QStringLiteral("grazingFade"), .08f);
    m_parallaxShadowStrength = numberValue(parallax, QStringLiteral("shadowStrength"), 1.0f);
    m_parallaxShadowBias = numberValue(parallax, QStringLiteral("shadowBias"), .015f);
    m_parallaxShadowSoftness = numberValue(parallax, QStringLiteral("shadowSoftness"), .08f);
    m_parallaxClip = boolValue(parallax, QStringLiteral("silhouetteClipping"), false);
    m_parallaxDepthOutput = boolValue(parallax, QStringLiteral("depthOutput"), false);
    m_parallaxFadeStart = numberValue(parallax, QStringLiteral("fadeStart"), 512.0f);
    m_parallaxFadeEnd = numberValue(parallax, QStringLiteral("fadeEnd"), 2048.0f);
    const auto readUv = [&document](const QString& section, QVector4D& transform, float& rotation, int& address) {
        transform = QVector4D(
            float(document.setting(section, QStringLiteral("scaleU"), 1.0).toDouble()),
            float(document.setting(section, QStringLiteral("scaleV"), 1.0).toDouble()),
            float(document.setting(section, QStringLiteral("offsetU"), 0.0).toDouble()),
            float(document.setting(section, QStringLiteral("offsetV"), 0.0).toDouble()));
        rotation = float(document.setting(section, QStringLiteral("rotation"), 0.0).toDouble());
        const QString mode = document.setting(section, QStringLiteral("address"), QStringLiteral("Wrap / Wrap")).toString();
        address = mode == QStringLiteral("Clamp / Clamp") ? 1 : (mode == QStringLiteral("Mirror / Mirror") ? 2 : 0);
    };
    readUv(QStringLiteral("UV0"), m_uvTransform0, m_uvRotation0, m_uvAddress0);
    readUv(QStringLiteral("UV1"), m_uvTransform1, m_uvRotation1, m_uvAddress1);
    readUv(QStringLiteral("UV2"), m_uvTransform2, m_uvRotation2, m_uvAddress2);
    const auto readProjection = [&document](const QString& section, int index, int* modes, int* axes, float* sharpness) {
        const QString source = document.setting(section, QStringLiteral("source"), QString()).toString();
        modes[index] = source == QStringLiteral("World Planar") ? 1 : (source == QStringLiteral("Object Planar") ? 2 : 0);
        const QString axis = document.setting(section, QStringLiteral("projectionAxis"), QStringLiteral("Triplanar")).toString();
        axes[index] = axis == QStringLiteral("X axis") ? 1 : (axis == QStringLiteral("Y axis") ? 2 : (axis == QStringLiteral("Z axis") ? 3 : 0));
        sharpness[index] = float(document.setting(section, QStringLiteral("triplanarBlend"), 4.0).toDouble());
    };
    readProjection(QStringLiteral("UV0"), 0, m_uvSourceMode, m_uvProjAxis, m_triplanarSharp);
    readProjection(QStringLiteral("UV1"), 1, m_uvSourceMode, m_uvProjAxis, m_triplanarSharp);
    readProjection(QStringLiteral("UV2"), 2, m_uvSourceMode, m_uvProjAxis, m_triplanarSharp);
    bool oscillating = false;
    for (int i = 0; i < 3; ++i) {
        const QJsonObject osc = document.settingGroup(QStringLiteral("UV%1/Oscillation").arg(i));
        const QString wave = osc.value(QStringLiteral("waveform")).toString(QStringLiteral("Sine"));
        m_oscWave[i] = wave == QStringLiteral("Triangle") ? 1 : (wave == QStringLiteral("Pulse") ? 2 : (wave == QStringLiteral("Breathing") ? 3 : 0));
        m_oscFreq[i] = float(osc.value(QStringLiteral("frequency")).toDouble(0));
        m_oscPhase[i] = float(osc.value(QStringLiteral("phase")).toDouble(0));
        m_oscDuty[i] = float(osc.value(QStringLiteral("duty")).toDouble(.5));
        m_oscScaleAmp[i] = float(osc.value(QStringLiteral("scaleAmplitude")).toDouble(0));
        m_oscOffsetAmp[i] = QVector2D(float(osc.value(QStringLiteral("offsetAmplitudeU")).toDouble(0)),
            float(osc.value(QStringLiteral("offsetAmplitudeV")).toDouble(0)));
        m_oscRotAmp[i] = float(osc.value(QStringLiteral("rotationAmplitude")).toDouble(0));
        m_oscEmissiveAmp[i] = float(osc.value(QStringLiteral("emissiveAmplitude")).toDouble(0));
        m_oscCoverageAmp[i] = float(osc.value(QStringLiteral("coverageAmplitude")).toDouble(0));
        m_oscNormalAmp[i] = float(osc.value(QStringLiteral("normalAmplitude")).toDouble(0));
        oscillating = oscillating || (m_oscFreq[i] != 0.0f
            && (m_oscScaleAmp[i] != 0.0f || !m_oscOffsetAmp[i].isNull() || m_oscRotAmp[i] != 0.0f
                || m_oscEmissiveAmp[i] != 0.0f || m_oscCoverageAmp[i] != 0.0f || m_oscNormalAmp[i] != 0.0f));
    }
    const auto readAnimation = [&document](const QString& section) {
        return QVector4D(
            float(document.setting(section, QStringLiteral("scrollU"), 0.0).toDouble()),
            float(document.setting(section, QStringLiteral("scrollV"), 0.0).toDouble()),
            float(document.setting(section, QStringLiteral("rotationSpeed"), 0.0).toDouble()),
            float(document.setting(section, QStringLiteral("phase"), 0.0).toDouble()));
    };
    m_uvAnimation0 = readAnimation(QStringLiteral("UV0/Animation"));
    m_uvAnimation1 = readAnimation(QStringLiteral("UV1/Animation"));
    m_uvAnimation2 = readAnimation(QStringLiteral("UV2/Animation"));
    const auto animatedDynamic = document.textureSlot(set, prefix+QStringLiteral("DynamicNormal"));
    const QString dynamicSection = secondary ? QStringLiteral("DynamicNormals/Secondary") : QStringLiteral("DynamicNormals/Primary");
    const bool dynamicDriven = animatedDynamic.enabled
        && document.setting(dynamicSection, QStringLiteral("source"), QStringLiteral("Facial Expression")).toString() != QStringLiteral("Constant");
    const bool gradientAnimated=gradient.enabled&&gradient.values.value(QStringLiteral("driver")).toString(QStringLiteral("Particle Age"))!=QStringLiteral("Constant");
    const bool animated = !m_uvAnimation0.isNull() || !m_uvAnimation1.isNull() || !m_uvAnimation2.isNull() || dynamicDriven || gradientAnimated
        || m_particleActive || m_liquidEnabled || oscillating
        || document.shaderMode()==Pbr::ShaderMode::VegetationSurface || document.shaderMode()==Pbr::ShaderMode::WaterSurface;
    if (animated) { m_lastTickMs = m_previewClock.elapsed(); m_animationTimer->start(); } else m_animationTimer->stop();
    m_mirrorPrimaryUv = document.setting(QStringLiteral("UV1"), QStringLiteral("mirrorPrimary"), false).toBool();
    m_tertiaryUsesUv0 = document.setting(QStringLiteral("UV2"), QStringLiteral("mirrorPrimary"), false).toBool()
        || document.setting(QStringLiteral("UV2"), QStringLiteral("source"), QStringLiteral("Mesh UV1")).toString() == QStringLiteral("Mesh UV0");
    const QString composition = document.setting(QStringLiteral("Transparency/Composition"), QStringLiteral("mode"), QStringLiteral("Opaque")).toString();
    m_compositionMode = composition == QStringLiteral("Alpha Test") ? 1
        : composition == QStringLiteral("Alpha Blend") ? 2
        : composition == QStringLiteral("Premultiplied") ? 3
        : composition == QStringLiteral("Additive") ? 4
        : composition == QStringLiteral("Multiply") ? 5
        : composition == QStringLiteral("Physical Transmission") ? 6
        : composition == QStringLiteral("Dedicated Water Pass") ? 7 : 0;
    m_globalOpacity = float(document.setting(QStringLiteral("Transparency/Composition"), QStringLiteral("alpha"), 1.0).toDouble());
    const QString alphaSource = document.setting(QStringLiteral("Transparency/Composition"), QStringLiteral("alphaSource"), QStringLiteral("Base Color A")).toString();
    m_alphaSource = alphaSource == QStringLiteral("Particle Gradient A") ? 1
        : alphaSource.contains(QStringLiteral("×")) ? 2 : (alphaSource == QStringLiteral("Constant") ? 3 : 0);
    m_alphaThreshold = float(document.setting(QStringLiteral("Transparency/Composition"), QStringLiteral("threshold"), .5).toDouble());
    m_depthWrite=document.setting(QStringLiteral("Transparency/Depth"),QStringLiteral("depthWrite"),m_compositionMode<=1).toBool();
    m_depthTest=document.setting(QStringLiteral("Transparency/Depth"),QStringLiteral("depthTest"),true).toBool();
    m_materialTwoSided=document.setting(QStringLiteral("Shading/Core"),QStringLiteral("twoSided"),document.shaderMode()==Pbr::ShaderMode::VegetationSurface||document.shaderMode()==Pbr::ShaderMode::HairSurface).toBool();
    const QString normalSpace = document.setting(QStringLiteral("Shading/Core"), QStringLiteral("normalSpace"), QStringLiteral("Tangent")).toString();
    m_normalSpace = normalSpace == QStringLiteral("Model") ? 1 : (normalSpace == QStringLiteral("World") ? 2 : 0);
    m_globalNormalStrength = float(document.setting(QStringLiteral("Shading/Core"), QStringLiteral("normalStrength"), 1.0).toDouble());
    m_materialIblIntensity = float(document.setting(QStringLiteral("Environment"), QStringLiteral("intensity"), 1.0).toDouble());
    m_materialDiffuseScale = float(document.setting(QStringLiteral("Environment"), QStringLiteral("diffuse"), 1.0).toDouble());
    m_materialSpecularScale = float(document.setting(QStringLiteral("Environment"), QStringLiteral("specular"), 1.0).toDouble());
    m_materialEnvironmentYaw = float(document.setting(QStringLiteral("Environment"), QStringLiteral("rotation"), 0.0).toDouble());
    m_hasBase = !m_baseImage.isNull(); m_hasNormal = !m_normalImage.isNull();
    m_hasRmaos = !m_rmaosImage.isNull(); m_hasEmissive = !m_emissiveImage.isNull();
    const QColor baseColor(base.values.value(QStringLiteral("color")).toString(QStringLiteral("#FFFFFF")));
    m_baseColor = baseColor.isValid() ? baseColor : Qt::white;
    m_opacity = numberValue(base.values, QStringLiteral("opacity"), 1.0f);
    m_overrideBaseColor = boolValue(base.values, QStringLiteral("overrideColor"), !m_hasBase);
    m_overrideOpacity = boolValue(base.values, QStringLiteral("overrideOpacity"), !m_hasBase);
    const QColor tint(base.values.value(QStringLiteral("tint")).toString(QStringLiteral("#FFFFFF")));
    m_tint = tint.isValid() ? tint : Qt::white;
    m_colorScale = numberValue(base.values, QStringLiteral("scale"), 1.0f);
    const QColor normalColor(normal.values.value(QStringLiteral("normal")).toString(QStringLiteral("#8080FF")));
    m_normalColor = normalColor.isValid() ? normalColor : QColor(QStringLiteral("#8080FF"));
    m_overrideNormal = boolValue(normal.values, QStringLiteral("overrideNormal"), !m_hasNormal);
    m_normalStrength = numberValue(normal.values, QStringLiteral("strength"), 1.0f);
    m_roughness = numberValue(rmaos.values, QStringLiteral("roughness"), 0.5f);
    m_metallic = numberValue(rmaos.values, QStringLiteral("metallic"), 0.0f);
    m_ao = numberValue(rmaos.values, QStringLiteral("ao"), 1.0f);
    m_f0 = numberValue(rmaos.values, QStringLiteral("f0"), 0.04f);
    m_useRoughness = m_hasRmaos && !boolValue(rmaos.values, QStringLiteral("overrideRoughness"), !m_hasRmaos);
    m_useMetallic = m_hasRmaos && !boolValue(rmaos.values, QStringLiteral("overrideMetallic"), !m_hasRmaos);
    m_useAo = m_hasRmaos && !boolValue(rmaos.values, QStringLiteral("overrideAo"), !m_hasRmaos);
    m_useF0 = m_hasRmaos && !boolValue(rmaos.values, QStringLiteral("overrideF0"), !m_hasRmaos);
    // v5 alpha duty: RMAOS.A carries dielectric F0 or porosity. F0 samples the
    // channel only while it owns it; porosity resolves alpha -> constant ->
    // derived saturate(rough * (1 - metal)).
    m_rmaosAlphaPorosity = rmaos.values.value(QStringLiteral("alphaCarries")).toString(QStringLiteral("Dielectric F0")) == QLatin1String("Porosity");
    m_useF0 = m_useF0 && !m_rmaosAlphaPorosity;
    m_porosityOverride = boolValue(rmaos.values, QStringLiteral("overridePorosity"), false);
    m_porosityConstant = numberValue(rmaos.values, QStringLiteral("porosity"), .5f);
    // Signed curvature rides the primary normal alpha; it has no constant.
    m_curvatureInAlpha = m_hasNormal && boolValue(normal.values, QStringLiteral("curvatureInAlpha"), false);
    m_cavitySpecOcclusion = numberValue(normal.values, QStringLiteral("cavitySpecOcclusion"), 1.0f);
    const QColor emission(emissive.values.value(QStringLiteral("color")).toString(QStringLiteral("#FFFFFF")));
    m_emissiveColor = emission.isValid() ? emission : Qt::white;
    m_overrideEmissiveColor = boolValue(emissive.values, QStringLiteral("overrideColor"), !m_hasEmissive);
    m_overrideEmissiveMask = boolValue(emissive.values, QStringLiteral("overrideMask"), !m_hasEmissive);
    m_emissiveMask = numberValue(emissive.values, QStringLiteral("mask"), 1.0f);
    m_emissiveIntensity = numberValue(emissive.values, QStringLiteral("intensity"), 0.0f);
    m_hasGradient = !m_gradientImage.isNull();
    const QColor gradientColor(gradient.values.value(QStringLiteral("color")).toString(QStringLiteral("#FFFFFF")));
    m_gradientColor = gradientColor.isValid() ? gradientColor : Qt::white;
    m_overrideGradientColor = boolValue(gradient.values, QStringLiteral("overrideColor"), !m_hasGradient);
    m_overrideGradientOpacity = boolValue(gradient.values, QStringLiteral("overrideOpacity"), !m_hasGradient);
    m_gradientOpacity = numberValue(gradient.values, QStringLiteral("opacity"), 1.0f);
    m_gradientIntensity = numberValue(gradient.values, QStringLiteral("intensity"), 1.0f);
    m_gradientConstant = numberValue(gradient.values, QStringLiteral("constantDriver"), .5f);
    const QString gradientDriver=gradient.values.value(QStringLiteral("driver")).toString(QStringLiteral("Particle Age"));
    m_gradientDriver=gradientDriver==QStringLiteral("Global Time")?1:(gradientDriver==QStringLiteral("Vertex Alpha")?2:(gradientDriver==QStringLiteral("Constant")?3:0));
    m_unlitIntensity=float(document.setting(QStringLiteral("Shading/Unlit"),QStringLiteral("intensity"),1.0).toDouble());
    m_unlitVertexColor=document.setting(QStringLiteral("Shading/Unlit"),QStringLiteral("vertexColor"),true).toBool();
    m_unlitExposure=document.setting(QStringLiteral("Shading/Unlit"),QStringLiteral("exposure"),true).toBool();
    m_hasSecondaryBase = combined && !m_secondaryBaseImage.isNull();
    m_hasSecondaryNormal = combined && !m_secondaryNormalImage.isNull();
    m_hasSecondaryRmaos = combined && !m_secondaryRmaosImage.isNull();
    m_hasSecondaryEmissive = combined && !m_secondaryEmissiveImage.isNull();
    m_hasLayerCoverage = combined && !m_layerCoverageImage.isNull();
    m_overrideLayerCoverage=boolValue(layerCoverage.values,QStringLiteral("overrideCoverage"),!m_hasLayerCoverage);
    m_layerCoverageFallback=numberValue(layerCoverage.values,QStringLiteral("coverage"),1.0f);
    m_invertLayerCoverage=boolValue(layerCoverage.values,QStringLiteral("invert"),false);
    m_layerCoverageSlotBias=numberValue(layerCoverage.values,QStringLiteral("bias"),0.0f);
    m_layerCoverageSlotContrast=numberValue(layerCoverage.values,QStringLiteral("contrast"),1.0f);
    const QColor secondaryBaseColor(secondaryBase.values.value(QStringLiteral("color")).toString(QStringLiteral("#FFFFFF")));
    m_secondaryBaseColor = secondaryBaseColor.isValid() ? secondaryBaseColor : Qt::white;
    const QColor secondaryTint(secondaryBase.values.value(QStringLiteral("tint")).toString(QStringLiteral("#FFFFFF")));
    m_secondaryTint = secondaryTint.isValid() ? secondaryTint : Qt::white;
    m_secondaryOpacity = numberValue(secondaryBase.values, QStringLiteral("opacity"), 1.0f);
    m_secondaryColorScale = numberValue(secondaryBase.values, QStringLiteral("scale"), 1.0f);
    m_overrideSecondaryBaseColor = boolValue(secondaryBase.values, QStringLiteral("overrideColor"), !m_hasSecondaryBase);
    m_overrideSecondaryOpacity = boolValue(secondaryBase.values, QStringLiteral("overrideOpacity"), !m_hasSecondaryBase);
    const auto blendModeIndex = [](const QString& blend) {
        return blend == QStringLiteral("Multiply") ? 1
            : blend == QStringLiteral("Modulate 2x") ? 2
            : blend == QStringLiteral("Add") ? 3
            : blend == QStringLiteral("Screen") ? 4
            : blend == QStringLiteral("Subtract") ? 5
            : blend == QStringLiteral("Min (Darken)") ? 6
            : blend == QStringLiteral("Max (Lighten)") ? 7
            : blend == QStringLiteral("Overlay") ? 8 : 0;
    };
    m_secondaryBlendMode = blendModeIndex(secondaryBase.values.value(QStringLiteral("blend")).toString(QStringLiteral("Masked Replace")));
    const QColor secondaryNormalColor(secondaryNormal.values.value(QStringLiteral("normal")).toString(QStringLiteral("#8080FF")));
    m_secondaryNormalColor = secondaryNormalColor.isValid() ? secondaryNormalColor : QColor(QStringLiteral("#8080FF"));
    m_secondaryNormalStrength = numberValue(secondaryNormal.values, QStringLiteral("strength"), 1.0f);
    m_overrideSecondaryNormal = boolValue(secondaryNormal.values, QStringLiteral("overrideNormal"), !m_hasSecondaryNormal);
    m_secondaryRoughness = numberValue(secondaryRmaos.values, QStringLiteral("roughness"), .5f);
    m_secondaryMetallic = numberValue(secondaryRmaos.values, QStringLiteral("metallic"), 0.0f);
    m_secondaryAo = numberValue(secondaryRmaos.values, QStringLiteral("ao"), 1.0f);
    m_secondaryF0 = numberValue(secondaryRmaos.values, QStringLiteral("f0"), .04f);
    m_useSecondaryRoughness = m_hasSecondaryRmaos && !boolValue(secondaryRmaos.values, QStringLiteral("overrideRoughness"), !m_hasSecondaryRmaos);
    m_useSecondaryMetallic = m_hasSecondaryRmaos && !boolValue(secondaryRmaos.values, QStringLiteral("overrideMetallic"), !m_hasSecondaryRmaos);
    m_useSecondaryAo = m_hasSecondaryRmaos && !boolValue(secondaryRmaos.values, QStringLiteral("overrideAo"), !m_hasSecondaryRmaos);
    m_useSecondaryF0 = m_hasSecondaryRmaos && !boolValue(secondaryRmaos.values, QStringLiteral("overrideF0"), !m_hasSecondaryRmaos);
    const QColor secondaryEmission(secondaryEmissive.values.value(QStringLiteral("color")).toString(QStringLiteral("#FFFFFF")));
    m_secondaryEmissiveColor = secondaryEmission.isValid() ? secondaryEmission : Qt::white;
    m_overrideSecondaryEmissiveColor = boolValue(secondaryEmissive.values, QStringLiteral("overrideColor"), !m_hasSecondaryEmissive);
    m_overrideSecondaryEmissiveMask = boolValue(secondaryEmissive.values, QStringLiteral("overrideMask"), !m_hasSecondaryEmissive);
    m_secondaryEmissiveMask = numberValue(secondaryEmissive.values, QStringLiteral("mask"), 1.0f);
    m_secondaryEmissiveIntensity = numberValue(secondaryEmissive.values, QStringLiteral("intensity"), 0.0f);
    const QJsonObject layer = document.settingGroup(QStringLiteral("UV1/Composition"));
    const QString coverageMode = layer.value(QStringLiteral("coverageMode")).toString(QStringLiteral("Multiply"));
    m_coverageMode = coverageMode == QStringLiteral("Vertex Only") ? 0
        : coverageMode == QStringLiteral("Pattern Reveal") ? 2 : (coverageMode == QStringLiteral("Height Blend") ? 3 : 1);
    // v5: the Primary Normal Alpha source is gone (Normal.A carries signed
    // curvature); unknown/legacy values resolve to Constant, which matches
    // what a BC5-era normal alpha read as (1.0).
    const QString pattern = layer.value(QStringLiteral("pattern")).toString(QStringLiteral("Constant"));
    m_patternSource = pattern == QStringLiteral("Layer Coverage R") ? 1
        : pattern == QStringLiteral("Secondary Base Alpha") ? 2 : 3;
    m_constantCoverage = float(layer.value(QStringLiteral("constantCoverage")).toDouble(1.0));
    m_coverageBias = float(layer.value(QStringLiteral("bias")).toDouble(0.0));
    m_coverageContrast = float(layer.value(QStringLiteral("contrast")).toDouble(1.0));
    m_coverageSoftness = float(layer.value(QStringLiteral("edgeSoftness")).toDouble(.1));

    m_hasTertiaryBase = m_tertiaryLayerActive && !m_tertiaryBaseImage.isNull();
    m_hasTertiaryNormal = m_tertiaryLayerActive && !m_tertiaryNormalImage.isNull();
    m_hasTertiaryRmaos = m_tertiaryLayerActive && !m_tertiaryRmaosImage.isNull();
    m_hasTertiaryEmissive = m_tertiaryLayerActive && !m_tertiaryEmissiveImage.isNull();
    m_hasTertiaryCoverage = m_tertiaryLayerActive && !m_tertiaryCoverageImage.isNull();
    m_overrideTertiaryCoverage=boolValue(tertiaryCoverage.values,QStringLiteral("overrideCoverage"),!m_hasTertiaryCoverage);
    m_tertiaryCoverageFallback=numberValue(tertiaryCoverage.values,QStringLiteral("coverage"),1.0f);
    m_invertTertiaryCoverage=boolValue(tertiaryCoverage.values,QStringLiteral("invert"),false);
    m_tertiaryCoverageSlotBias=numberValue(tertiaryCoverage.values,QStringLiteral("bias"),0.0f);
    m_tertiaryCoverageSlotContrast=numberValue(tertiaryCoverage.values,QStringLiteral("contrast"),1.0f);
    const QColor tertiaryBaseColor(tertiaryBase.values.value(QStringLiteral("color")).toString(QStringLiteral("#FFFFFF")));
    m_tertiaryBaseColor = tertiaryBaseColor.isValid() ? tertiaryBaseColor : Qt::white;
    const QColor tertiaryTint(tertiaryBase.values.value(QStringLiteral("tint")).toString(QStringLiteral("#FFFFFF")));
    m_tertiaryTint = tertiaryTint.isValid() ? tertiaryTint : Qt::white;
    m_tertiaryOpacity = numberValue(tertiaryBase.values, QStringLiteral("opacity"), 1.0f);
    m_tertiaryColorScale = numberValue(tertiaryBase.values, QStringLiteral("scale"), 1.0f);
    m_overrideTertiaryBaseColor = boolValue(tertiaryBase.values, QStringLiteral("overrideColor"), !m_hasTertiaryBase);
    m_overrideTertiaryOpacity = boolValue(tertiaryBase.values, QStringLiteral("overrideOpacity"), !m_hasTertiaryBase);
    m_tertiaryBlendMode = blendModeIndex(tertiaryBase.values.value(QStringLiteral("blend")).toString(QStringLiteral("Masked Replace")));
    const QColor tertiaryNormalColor(tertiaryNormal.values.value(QStringLiteral("normal")).toString(QStringLiteral("#8080FF")));
    m_tertiaryNormalColor = tertiaryNormalColor.isValid() ? tertiaryNormalColor : QColor(QStringLiteral("#8080FF"));
    m_tertiaryNormalStrength = numberValue(tertiaryNormal.values, QStringLiteral("strength"), 1.0f);
    m_overrideTertiaryNormal = boolValue(tertiaryNormal.values, QStringLiteral("overrideNormal"), !m_hasTertiaryNormal);
    m_tertiaryRoughness = numberValue(tertiaryRmaos.values, QStringLiteral("roughness"), .5f);
    m_tertiaryMetallic = numberValue(tertiaryRmaos.values, QStringLiteral("metallic"), 0.0f);
    m_tertiaryAo = numberValue(tertiaryRmaos.values, QStringLiteral("ao"), 1.0f);
    m_tertiaryF0 = numberValue(tertiaryRmaos.values, QStringLiteral("f0"), .04f);
    m_useTertiaryRoughness = m_hasTertiaryRmaos && !boolValue(tertiaryRmaos.values, QStringLiteral("overrideRoughness"), !m_hasTertiaryRmaos);
    m_useTertiaryMetallic = m_hasTertiaryRmaos && !boolValue(tertiaryRmaos.values, QStringLiteral("overrideMetallic"), !m_hasTertiaryRmaos);
    m_useTertiaryAo = m_hasTertiaryRmaos && !boolValue(tertiaryRmaos.values, QStringLiteral("overrideAo"), !m_hasTertiaryRmaos);
    m_useTertiaryF0 = m_hasTertiaryRmaos && !boolValue(tertiaryRmaos.values, QStringLiteral("overrideF0"), !m_hasTertiaryRmaos);
    const QColor tertiaryEmission(tertiaryEmissive.values.value(QStringLiteral("color")).toString(QStringLiteral("#FFFFFF")));
    m_tertiaryEmissiveColor = tertiaryEmission.isValid() ? tertiaryEmission : Qt::white;
    m_overrideTertiaryEmissiveColor = boolValue(tertiaryEmissive.values, QStringLiteral("overrideColor"), !m_hasTertiaryEmissive);
    m_overrideTertiaryEmissiveMask = boolValue(tertiaryEmissive.values, QStringLiteral("overrideMask"), !m_hasTertiaryEmissive);
    m_tertiaryEmissiveMask = numberValue(tertiaryEmissive.values, QStringLiteral("mask"), 1.0f);
    m_tertiaryEmissiveIntensity = numberValue(tertiaryEmissive.values, QStringLiteral("intensity"), 0.0f);
    const QJsonObject tertiaryLayer = document.settingGroup(QStringLiteral("UV2/Composition"));
    const QString tCoverageMode = tertiaryLayer.value(QStringLiteral("coverageMode")).toString(QStringLiteral("Multiply"));
    m_tCoverageMode = tCoverageMode == QStringLiteral("Vertex Only") ? 0
        : tCoverageMode == QStringLiteral("Pattern Reveal") ? 2 : (tCoverageMode == QStringLiteral("Height Blend") ? 3 : 1);
    const QString tPattern = tertiaryLayer.value(QStringLiteral("pattern")).toString(QStringLiteral("Constant"));
    m_tPatternSource = tPattern == QStringLiteral("Layer Coverage R") ? 1
        : tPattern == QStringLiteral("Tertiary Base Alpha") ? 2 : 3;
    m_tConstantCoverage = float(tertiaryLayer.value(QStringLiteral("constantCoverage")).toDouble(1.0));
    m_tCoverageBias = float(tertiaryLayer.value(QStringLiteral("bias")).toDouble(0.0));
    m_tCoverageContrast = float(tertiaryLayer.value(QStringLiteral("contrast")).toDouble(1.0));
    m_tCoverageSoftness = float(tertiaryLayer.value(QStringLiteral("edgeSoftness")).toDouble(.1));
    // Height Blend coverage compares layer heights packed in Normal+Height blue.
    m_primaryHeightInBlue = boolValue(document.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryNormal")).values, QStringLiteral("heightInBlue"), false);
    m_secondaryHeightInBlue = boolValue(secondaryNormal.values, QStringLiteral("heightInBlue"), false);
    m_tertiaryHeightInBlue = boolValue(tertiaryNormal.values, QStringLiteral("heightInBlue"), false);

    for (int layer = 0; layer < 3; ++layer) {
        const QJsonObject group = document.settingGroup(QStringLiteral("Particles/Layer") + QString::number(layer));
        m_flipbookEnabled[layer] = group.value(QStringLiteral("flipbookEnabled")).toBool(false) ? 1 : 0;
        m_flipbookGrid[layer] = QVector2D(
            qMax(1.0f, float(group.value(QStringLiteral("flipbookColumns")).toDouble(4.0))),
            qMax(1.0f, float(group.value(QStringLiteral("flipbookRows")).toDouble(4.0))));
        const QString driver = group.value(QStringLiteral("flipbookDriver")).toString(QStringLiteral("Particle Age"));
        m_flipbookDriver[layer] = driver == QStringLiteral("Global Time") ? 1 : (driver == QStringLiteral("Constant") ? 2 : 0);
        m_flipbookFps[layer] = float(group.value(QStringLiteral("flipbookFps")).toDouble(0.0));
        const QString loop = group.value(QStringLiteral("flipbookLoop")).toString(QStringLiteral("Loop"));
        m_flipbookLoop[layer] = loop == QStringLiteral("Clamp") ? 1 : (loop == QStringLiteral("Ping-Pong") ? 2 : 0);
        m_flipbookBlend[layer] = group.value(QStringLiteral("flipbookBlend")).toBool(true) ? 1 : 0;
        m_flipbookConstantFrame[layer] = float(group.value(QStringLiteral("flipbookConstantFrame")).toDouble(0.0));
        m_polarUv[layer] = group.value(QStringLiteral("polarUv")).toBool(false) ? 1 : 0;
        m_layerRole[layer] = group.value(QStringLiteral("role")).toString(QStringLiteral("Color")) == QStringLiteral("Modifier") ? 1 : 0;
        m_distortStrength[layer] = float(group.value(QStringLiteral("distortStrength")).toDouble(0.0));
        m_advectSpeed[layer] = float(group.value(QStringLiteral("advectSpeed")).toDouble(0.0));
        m_erodeEnabled[layer] = group.value(QStringLiteral("erodeEnabled")).toBool(false) ? 1 : 0;
        m_erodeSoftness[layer] = float(group.value(QStringLiteral("erodeSoftness")).toDouble(.15));
        m_erodeEdgeTint[layer] = jsonColor(group, QStringLiteral("erodeEdgeTint"), QStringLiteral("#FF7A26"));
        m_erodeEdgeGlow[layer] = float(group.value(QStringLiteral("erodeEdgeGlow")).toDouble(0.0));
    }
    const QJsonObject particleGlobal = document.settingGroup(QStringLiteral("Particles/Global"));
    const QString paletteMode = particleGlobal.value(QStringLiteral("paletteMode")).toString(QStringLiteral("Off"));
    m_paletteMode = paletteMode == QStringLiteral("Gradient Ramp") ? 1 : (paletteMode == QStringLiteral("Blackbody Ramp") ? 2 : 0);
    m_paletteIntensity = float(particleGlobal.value(QStringLiteral("paletteIntensity")).toDouble(1.0));
    m_randomFlipbookStart = particleGlobal.value(QStringLiteral("randomFlipbookStart")).toBool(false) ? 1 : 0;
    m_randomPhase = float(particleGlobal.value(QStringLiteral("randomPhase")).toDouble(0.0));
    m_randomHueJitter = float(particleGlobal.value(QStringLiteral("randomHueJitter")).toDouble(0.0));
    m_particleFresnelFade = float(particleGlobal.value(QStringLiteral("fresnelFade")).toDouble(0.0));
    m_particleFresnelPower = float(particleGlobal.value(QStringLiteral("fresnelFadePower")).toDouble(2.0));
    m_particleNearFade = float(particleGlobal.value(QStringLiteral("nearFade")).toDouble(0.0));

    const QJsonObject bottleLiquid = document.settingGroup(QStringLiteral("Shading/BottleLiquid"));
    m_liquidEnabled = bottleLiquid.value(QStringLiteral("enabled")).toBool(false)
        && (document.shaderMode() == Pbr::ShaderMode::GlassSurface || document.shaderMode() == Pbr::ShaderMode::AllFeatures);
    m_liquidFill = float(bottleLiquid.value(QStringLiteral("fillLevel")).toDouble(.6));
    m_liquidColor = jsonColor(bottleLiquid, QStringLiteral("color"), QStringLiteral("#7A3B12"));
    m_liquidAbsorption = float(bottleLiquid.value(QStringLiteral("absorption")).toDouble(.6));
    m_meniscusColor = jsonColor(bottleLiquid, QStringLiteral("meniscusColor"), QStringLiteral("#D9945A"));
    m_meniscusWidth = float(bottleLiquid.value(QStringLiteral("meniscusWidth")).toDouble(.02));
    m_liquidSurfaceBrightness = float(bottleLiquid.value(QStringLiteral("surfaceBrightness")).toDouble(1.15));
    m_wobbleFrequency = float(bottleLiquid.value(QStringLiteral("wobbleFrequency")).toDouble(4.0));
    m_wobbleDamping = float(bottleLiquid.value(QStringLiteral("wobbleDamping")).toDouble(.25));
    m_maxTiltDegrees = float(bottleLiquid.value(QStringLiteral("maxTilt")).toDouble(25.0));

    m_specialImages=QVector<QImage>(SpecialLayerCount);m_specialMask=0;
    const auto addSpecial=[&](int layerIndex,const QString& key,const QVector4D& fallback,const QVector4D& useTexture,
        const QString& requestedSet=QString(),const QString& requestedPrefix=QString()){
        const QString slotSet=requestedSet.isEmpty()?set:requestedSet,slotPrefix=requestedPrefix.isEmpty()?prefix:requestedPrefix;
        const Pbr::TextureSlotState state=document.textureSlot(slotSet,slotPrefix+key);
        QImage image=resolvedPackedImage(state,textureRoot,fallback,useTexture);
        if(!image.isNull()){m_specialImages[layerIndex]=std::move(image);m_specialMask|=1<<layerIndex;}
    };
    const auto used=[](const QJsonObject& values,const QString& overrideKey){return boolValue(values,overrideKey,true)?0.0f:1.0f;};
    const auto stateFor=[&](const QString& key,const QString& requestedSet=QString(),const QString& requestedPrefix=QString()){
        const QString slotSet=requestedSet.isEmpty()?set:requestedSet,slotPrefix=requestedPrefix.isEmpty()?prefix:requestedPrefix;
        return document.textureSlot(slotSet,slotPrefix+key);
    };
    {
        const auto state=stateFor(QStringLiteral("TintMask"));
        addSpecial(TintMaskLayer,QStringLiteral("TintMask"),QVector4D(numberValue(state.values,"maskR",0),numberValue(state.values,"maskG",0),numberValue(state.values,"maskB",0),numberValue(state.values,"maskA",0)),
            QVector4D(used(state.values,"overrideMaskR"),used(state.values,"overrideMaskG"),used(state.values,"overrideMaskB"),used(state.values,"overrideMaskA")));
    }
    {
        const auto state=stateFor(QStringLiteral("SurfaceEffects"));
        addSpecial(SurfaceEffectsLayer,QStringLiteral("SurfaceEffects"),QVector4D(numberValue(state.values,"sheenFallback",0),numberValue(state.values,"glintsFallback",0),numberValue(state.values,"filmFallback",.5f),numberValue(state.values,"iridescenceFallback",0)),
            QVector4D(used(state.values,"overrideSheen"),used(state.values,"overrideGlints"),used(state.values,"overrideFilm"),used(state.values,"overrideIridescence")));
    }
    {
        const auto state=stateFor(QStringLiteral("DynamicNormal"));const QVector3D color=jsonColor(state.values,"normal","#8080FF");
        const float use=used(state.values,"overrideNormal");addSpecial(DynamicNormalLayer,QStringLiteral("DynamicNormal"),QVector4D(color,1),QVector4D(use,use,0,0));
    }
    {
        const auto state=stateFor(QStringLiteral("Subsurface"));const QVector3D color=jsonColor(state.values,"scatterTint","#FFFFFF");const float useColor=used(state.values,"overrideScatterTint");
        addSpecial(SubsurfaceLayer,QStringLiteral("Subsurface"),QVector4D(color,numberValue(state.values,"thicknessValue",1)),QVector4D(useColor,useColor,useColor,used(state.values,"overrideThicknessValue")));
        // Authored SSS thickness always wins; the convex-curvature proxy only
        // stands in when the alpha is not texture-authored.
        m_sssAlphaAuthored=state.enabled&&!state.path.trimmed().isEmpty()
            &&!boolValue(state.values,QStringLiteral("overrideThicknessValue"),false);
    }
    {
        const auto state=stateFor(QStringLiteral("HairDirection"));const QVector3D flow=jsonColor(state.values,"flow","#808000");const float useFlow=used(state.values,"overrideFlow");
        addSpecial(HairDirectionLayer,QStringLiteral("HairDirection"),QVector4D(flow.x(),flow.y(),numberValue(state.values,"lobeShift",.5f),numberValue(state.values,"scatter",1)),QVector4D(useFlow,useFlow,used(state.values,"overrideLobeShift"),used(state.values,"overrideScatter")));
    }
    {
        const auto state=stateFor(QStringLiteral("HairScatter"));const QVector3D color=jsonColor(state.values,"scatterTint","#8B5A3C");const float useColor=used(state.values,"overrideScatterTint");
        addSpecial(HairScatterLayer,QStringLiteral("HairScatter"),QVector4D(color,numberValue(state.values,"backlight",1)),QVector4D(useColor,useColor,useColor,used(state.values,"overrideBacklight")));
    }
    {
        const auto state=stateFor(QStringLiteral("TransmissionAbsorption"));const QVector3D color=jsonColor(state.values,"absorption","#FFFFFF");const float useColor=used(state.values,"overrideAbsorption");
        addSpecial(TransmissionLayer,QStringLiteral("TransmissionAbsorption"),QVector4D(color,std::min(numberValue(state.values,"thickness",1),1.0f)),QVector4D(useColor,useColor,useColor,used(state.values,"overrideThickness")));
    }
    {
        const auto state=stateFor(QStringLiteral("Imperfections"));
        addSpecial(ImperfectionsLayer,QStringLiteral("Imperfections"),QVector4D(numberValue(state.values,"smudges",0),numberValue(state.values,"scratches",0),numberValue(state.values,"dust",0),numberValue(state.values,"mask",1)),QVector4D(used(state.values,"overrideSmudges"),used(state.values,"overrideScratches"),used(state.values,"overrideDust"),used(state.values,"overrideMask")));
    }
    {
        const auto state=stateFor(QStringLiteral("WaterFlow"));const QVector3D flow=jsonColor(state.values,"flow","#808000");const float useFlow=used(state.values,"overrideFlow");
        addSpecial(WaterFlowLayer,QStringLiteral("WaterFlow"),QVector4D(flow.x(),flow.y(),numberValue(state.values,"turbulence",0),numberValue(state.values,"shoreline",0)),QVector4D(useFlow,useFlow,used(state.values,"overrideTurbulence"),used(state.values,"overrideShoreline")));
    }
    {
        const auto state=stateFor(QStringLiteral("Foam"));const QVector3D color=jsonColor(state.values,"color","#E8F0F2");const float useColor=used(state.values,"overrideColor");
        addSpecial(FoamLayer,QStringLiteral("Foam"),QVector4D(color,numberValue(state.values,"coverage",1)),QVector4D(useColor,useColor,useColor,used(state.values,"overrideCoverage")));
    }
    {
        const auto state=stateFor(QStringLiteral("SubsurfaceDetail"));
        addSpecial(SubsurfaceDetailLayer,QStringLiteral("SubsurfaceDetail"),QVector4D(numberValue(state.values,"epidermal",0),numberValue(state.values,"blood",0),numberValue(state.values,"oil",0),numberValue(state.values,"mask",1)),QVector4D(used(state.values,"overrideEpidermal"),used(state.values,"overrideBlood"),used(state.values,"overrideOil"),used(state.values,"overrideMask")));
    }
    {
        const auto state=stateFor(QStringLiteral("IrisMasks"));
        addSpecial(IrisMasksLayer,QStringLiteral("IrisMasks"),QVector4D(numberValue(state.values,"height",.5f),numberValue(state.values,"limbal",0),numberValue(state.values,"pupil",0),numberValue(state.values,"cornea",1)),QVector4D(used(state.values,"overrideHeight"),used(state.values,"overrideLimbal"),used(state.values,"overridePupil"),used(state.values,"overrideCornea")));
    }
    {
        const auto state=stateFor(QStringLiteral("Cornea"));const QVector3D normalColor=jsonColor(state.values,"normal","#8080FF");const float useNormal=used(state.values,"overrideNormal");
        addSpecial(CorneaLayer,QStringLiteral("Cornea"),QVector4D(normalColor.x(),normalColor.y(),numberValue(state.values,"roughness",.025f),numberValue(state.values,"coverage",1)),QVector4D(useNormal,useNormal,used(state.values,"overrideRoughness"),used(state.values,"overrideCoverage")));
    }
    {
        // A clearcoat can be authored on any UV layer; visible instances follow
        // the current preview mode (solo shows that layer's coat, combined
        // shows every enabled layer's coat).
        const int soloLayer=m_uvSet<=2?m_uvSet:0;const bool combinedView=m_uvSet==3;
        const auto addCoat=[&](int layerIndex,const QString& coatSet,const QString& coatPrefix,bool active){
            if(!active)return;
            const auto state=stateFor(QStringLiteral("Clearcoat"),coatSet,coatPrefix);const QVector3D normalColor=jsonColor(state.values,"normal","#8080FF");const float useNormal=used(state.values,"overrideNormal");
            addSpecial(layerIndex,QStringLiteral("Clearcoat"),QVector4D(normalColor.x(),normalColor.y(),numberValue(state.values,"roughness",.1f),numberValue(state.values,"coverage",1)),QVector4D(useNormal,useNormal,used(state.values,"overrideRoughness"),used(state.values,"overrideCoverage")),coatSet,coatPrefix);
        };
        addCoat(ClearcoatLayer,QStringLiteral("UV0"),QStringLiteral("primary"),soloLayer==0||combinedView);
        addCoat(ClearcoatSecondaryLayer,QStringLiteral("UV1"),QStringLiteral("secondary"),soloLayer==1||(combinedView&&document.secondaryUvEnabled()));
        addCoat(ClearcoatTertiaryLayer,QStringLiteral("UV2"),QStringLiteral("tertiary"),soloLayer==2||(combinedView&&document.tertiaryUvEnabled()));
    }
    {
        const auto state=stateFor(QStringLiteral("ScreenDistortion"));const QVector3D direction=jsonColor(state.values,"direction","#808000");const float useDirection=used(state.values,"overrideDirection");
        addSpecial(DistortionLayer,QStringLiteral("ScreenDistortion"),QVector4D(direction.x(),direction.y(),numberValue(state.values,"strength",0),numberValue(state.values,"mask",1)),QVector4D(useDirection,useDirection,used(state.values,"overrideStrength"),used(state.values,"overrideMask")));
    }
    {
        const auto state=stateFor(QStringLiteral("Anisotropy"));const QVector3D direction=jsonColor(state.values,"direction","#FF8000");const float useDirection=used(state.values,"overrideDirection");
        addSpecial(AnisotropyLayer,QStringLiteral("Anisotropy"),QVector4D(direction.x(),direction.y(),numberValue(state.values,"strength",0),numberValue(state.values,"mask",1)),QVector4D(useDirection,useDirection,used(state.values,"overrideStrength"),used(state.values,"overrideMask")));
    }
    {
        const auto state=stateFor(QStringLiteral("BentNormal"));const QVector3D normalColor=jsonColor(state.values,"normal","#8080FF");const float useNormal=used(state.values,"overrideNormal");
        addSpecial(BentNormalLayer,QStringLiteral("BentNormal"),
            QVector4D(normalColor.x(),normalColor.y(),numberValue(state.values,"coneOcclusion",1),numberValue(state.values,"mask",1)),
            QVector4D(useNormal,useNormal,used(state.values,"overrideConeOcclusion"),used(state.values,"overrideMask")));
    }
    {
        const auto state=stateFor(QStringLiteral("DetailNormal"));const QVector3D normalColor=jsonColor(state.values,"normal","#8080FF");const float useNormal=used(state.values,"overrideNormal");
        addSpecial(DetailNormalLayer,QStringLiteral("DetailNormal"),
            QVector4D(normalColor.x(),normalColor.y(),.5f,numberValue(state.values,"mask",1)),
            QVector4D(useNormal,useNormal,used(state.values,"overrideRoughVariation"),used(state.values,"overrideMask")));
    }
    {
        // Dedicated per-state masks are per-material and live on the primary set.
        addSpecial(StateMasksLayer,QStringLiteral("StateMasks"),QVector4D(0,0,0,0),QVector4D(1,1,1,1),QStringLiteral("UV0"),QStringLiteral("primary"));
        // v5: state behavior comes from the runtime-owned definitions, never
        // the material. The material contributes only the SurfaceResponse
        // bindings: adhesion constant, shelter vertex channel, and StateMasks
        // channel-name bindings matched against definition state names.
        m_surfaceStates=Pbr::activeSurfaceStateDefinitions();
        const QJsonObject response=document.settingGroup(QStringLiteral("SurfaceResponse"));
        m_stateAdhesion=float(response.value(QStringLiteral("adhesion")).toDouble(1.0));
        const QString shelter=response.value(QStringLiteral("shelterChannel")).toString(QStringLiteral("None"));
        m_shelterChannel=shelter==QLatin1String("Red")?0:shelter==QLatin1String("Green")?1
            :shelter==QLatin1String("Blue")?2:shelter==QLatin1String("Alpha")?3:-1;
        const QString maskNames[4]={
            response.value(QStringLiteral("maskR")).toString(),response.value(QStringLiteral("maskG")).toString(),
            response.value(QStringLiteral("maskB")).toString(),response.value(QStringLiteral("maskA")).toString()};
        for(auto& state:m_surfaceStates){
            state.maskChannel=-1;
            for(int channel=0;channel<4;++channel)
                if(!maskNames[channel].isEmpty()&&maskNames[channel].compare(state.name,Qt::CaseInsensitive)==0){state.maskChannel=channel;break;}
        }
        m_stateAmounts.resize(m_surfaceStates.size());
    }
    {
        const auto stateA=stateFor(QStringLiteral("SixWayA"));
        // A alpha carries opacity; the override toggle lets RGB-only sheets
        // (or opaque smoke cards) ignore the texture channel. Unlike other
        // overrides it defaults OFF so existing materials keep sampling alpha.
        addSpecial(SixWayALayer,QStringLiteral("SixWayA"),
            QVector4D(0,0,0,numberValue(stateA.values,"opacity",1)),
            QVector4D(1,1,1,boolValue(stateA.values,QStringLiteral("overrideOpacity"),false)?0.0f:1.0f));
        const auto stateB=stateFor(QStringLiteral("SixWayB"));
        // B alpha is the fire/emissive mask; defaults to the constant override (0).
        addSpecial(SixWayBLayer,QStringLiteral("SixWayB"),
            QVector4D(0,0,0,numberValue(stateB.values,"emissive",0)),
            QVector4D(1,1,1,used(stateB.values,"overrideEmissive")));
        addSpecial(MotionVectorsLayer,QStringLiteral("MotionVectors"),QVector4D(.5f,.5f,0,0),QVector4D(1,1,0,0));
        Q_UNUSED(stateA);
    }
    {
        const auto state=stateFor(QStringLiteral("InteriorMap"));
        m_interiorFaces.clear();
        m_interiorEnabled=false;
        if(state.enabled&&!state.path.trimmed().isEmpty()){
            QString interiorError;
            m_interiorFaces=TextureImage::loadCubemapFaces(state.path,textureRoot,&interiorError);
            m_interiorEnabled=m_interiorFaces.size()==6;
        }
        m_interiorRoomsU=numberValue(state.values,QStringLiteral("roomsU"),1.0f);
        m_interiorRoomsV=numberValue(state.values,QStringLiteral("roomsV"),1.0f);
        m_interiorDepth=numberValue(state.values,QStringLiteral("depth"),1.0f);
        m_interiorBrightness=numberValue(state.values,QStringLiteral("brightness"),1.0f);
        m_interiorEmissive=numberValue(state.values,QStringLiteral("windowEmissive"),0.0f);
    }
    {
        const auto state=document.textureSlot(QStringLiteral("Landscape"),QStringLiteral("macroVariation"));const QVector3D color=jsonColor(state.values,"color","#808080");const float useColor=used(state.values,"overrideColor");
        QImage image=resolvedPackedImage(state,textureRoot,QVector4D(color,numberValue(state.values,"roughness",.5f)),QVector4D(useColor,useColor,useColor,used(state.values,"overrideRoughness")));
        if(!image.isNull()){m_specialImages[LandscapeMacroLayer]=std::move(image);m_specialMask|=1<<LandscapeMacroLayer;}
    }
    {
        const auto state=document.textureSlot(QStringLiteral("UV1"),QStringLiteral("secondaryGradient"));const QVector3D color=jsonColor(state.values,"color","#FFFFFF");const float useColor=used(state.values,"overrideColor");
        QImage image=resolvedPackedImage(state,textureRoot,QVector4D(color,numberValue(state.values,"coverage",1)),QVector4D(useColor,useColor,useColor,used(state.values,"overrideCoverage")));
        if(!image.isNull()){m_specialImages[SecondaryGradientLayer]=std::move(image);m_specialMask|=1<<SecondaryGradientLayer;}
    }
    const QJsonObject displacement=document.settingGroup(QStringLiteral("Geometry/Displacement"));
    const bool displacementTessellation=displacement.value(QStringLiteral("enabled")).toBool(false)&&m_hasHeight;
    if(m_displacementTessellation!=displacementTessellation){
        m_displacementTessellation=displacementTessellation;
        if(m_shape==Shape::Plane){m_mesh=m_displacementTessellation?PreviewMeshes::planeGrid(m_planeOrientation):PreviewMeshes::plane(m_planeOrientation,m_planeTwoSided);m_meshDirty=true;}
    }
    m_texturesDirty = true; update();
}

void PbrPreviewGL::setShape(Shape shape)
{
    m_shape = shape;
    if (shape == Shape::Plane) m_mesh = m_displacementTessellation
        ? PreviewMeshes::planeGrid(m_planeOrientation) : PreviewMeshes::plane(m_planeOrientation, m_planeTwoSided);
    else if (shape == Shape::Cube) m_mesh = PreviewMeshes::cube();
    else if (shape == Shape::Sphere) m_mesh = PreviewMeshes::sphere();
    else if (!m_mesh.isValid()) return;
    m_meshDirty = true; m_submesh = 0; frameObject(); update();
}

void PbrPreviewGL::setPlaneOptions(PreviewMeshes::PlaneOrientation orientation, bool twoSided)
{
    m_planeOrientation = orientation; m_planeTwoSided = twoSided;
    if (m_shape == Shape::Plane) setShape(Shape::Plane);
}

bool PbrPreviewGL::loadCustomMesh(const QString& path, QString* error)
{
    const QString suffix=QFileInfo(path).suffix();
    PreviewMesh imported=suffix.compare(QStringLiteral("nif"),Qt::CaseInsensitive)==0
        ? PreviewMeshes::loadStaticNif(path,error):PreviewMeshes::loadStaticFbx(path,error);
    if (!imported.isValid()) return false;
    m_mesh = std::move(imported); m_shape = Shape::Custom; m_meshDirty = true; m_submesh = 0;
    frameObject(); update(); return true;
}

void PbrPreviewGL::setUvSet(int uvSet) { const int next=std::clamp(uvSet,0,3);if(next==m_uvSet)return;m_uvSet=next;setMaterial(m_document,m_textureRoot); }
void PbrPreviewGL::setMsaaSamples(int samples) { const int next=std::clamp(samples,1,8);if(next==m_msaaSamples)return;m_msaaSamples=next;update(); }
void PbrPreviewGL::setSurfaceStateAmounts(const QVector<float>& amounts)
{
    m_stateAmounts=amounts;
    // Animated liquid detail needs the preview clock running while a
    // ripple-capable state is active.
    bool rippleActive=false;
    for(int i=0;i<m_surfaceStates.size()&&i<m_stateAmounts.size();++i)
        if(m_surfaceStates[i].ripples&&m_stateAmounts[i]>0.0f)rippleActive=true;
    if(rippleActive&&!m_animationTimer->isActive()){m_lastTickMs=m_previewClock.elapsed();m_animationTimer->start();}
    update();
}

void PbrPreviewGL::recordShaderFailure(const char* label,const QString& log)
{
    reportShaderFailure(label,log);
    const QString entry=QStringLiteral("%1: %2").arg(QLatin1String(label),log.left(400));
    if(!m_shaderErrorLog.contains(entry)){
        if(!m_shaderErrorLog.isEmpty())m_shaderErrorLog+=QLatin1Char('\n');
        m_shaderErrorLog+=entry;
        emit shaderErrorsChanged(m_shaderErrorLog);
    }
}

void PbrPreviewGL::setTimePlayback(bool paused,float scale)
{
    m_timePaused=paused;
    m_timeScale=std::clamp(scale,0.05f,8.0f);
    m_lastTickMs=m_previewClock.elapsed();
    update();
}

void PbrPreviewGL::setTimeValue(double seconds)
{
    m_previewTime=std::max(0.0,seconds);
    m_lastTickMs=m_previewClock.elapsed();
    update();
}

void PbrPreviewGL::setParticleSimulation(float age,bool autoAge,float ageRate,float seed)
{
    m_particleAge=std::clamp(age,0.0f,1.0f);
    m_particleAgeAuto=autoAge;
    m_particleAgeRate=std::clamp(ageRate,0.01f,10.0f);
    m_particleSeed=std::clamp(seed,0.0f,1.0f);
    update();
}
void PbrPreviewGL::setLandscapeLayer(int layer){const int next=std::clamp(layer,0,15);if(next==m_landscapeLayer)return;m_landscapeLayer=next;setMaterial(m_document,m_textureRoot);}
void PbrPreviewGL::setSubmesh(int index, bool all) { m_submesh = std::clamp(index, 0, std::max(0, int(m_mesh.submeshes.size()) - 1)); m_applyAllSubmeshes = all; update(); }

PbrPreviewGL::CompanionMaterial PbrPreviewGL::makeCompanionMaterial(const QString& pbrmPath) const
{
    CompanionMaterial companion;
    companion.pbrmPath = pbrmPath;
    Pbr::MaterialDocument document;
    if (!document.load(pbrmPath, &companion.error)) return companion;
    const auto base = document.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryBaseColor"));
    const auto normal = document.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryNormal"));
    const auto rmaos = document.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryRmaos"));
    const auto emissive = document.textureSlot(QStringLiteral("UV0"), QStringLiteral("primaryEmissive"));
    companion.baseImage = loadTexture(base, m_textureRoot);
    companion.normalImage = loadTexture(normal, m_textureRoot);
    companion.rmaosImage = loadTexture(rmaos, m_textureRoot);
    companion.emissiveImage = loadTexture(emissive, m_textureRoot);
    companion.hasBase = !companion.baseImage.isNull();
    companion.hasNormal = !companion.normalImage.isNull();
    companion.hasRmaos = !companion.rmaosImage.isNull();
    companion.hasEmissive = !companion.emissiveImage.isNull();
    companion.baseFallback = jsonColor(base.values, QStringLiteral("color"), QStringLiteral("#FFFFFF"));
    companion.tint = jsonColor(base.values, QStringLiteral("tint"), QStringLiteral("#FFFFFF"));
    companion.opacity = numberValue(base.values, QStringLiteral("opacity"), 1.0f);
    companion.colorScale = numberValue(base.values, QStringLiteral("scale"), 1.0f);
    companion.overrideBaseColor = boolValue(base.values, QStringLiteral("overrideColor"), !companion.hasBase);
    companion.overrideOpacity = boolValue(base.values, QStringLiteral("overrideOpacity"), !companion.hasBase);
    companion.normalFallback = jsonColor(normal.values, QStringLiteral("normal"), QStringLiteral("#8080FF"));
    companion.overrideNormal = boolValue(normal.values, QStringLiteral("overrideNormal"), !companion.hasNormal);
    companion.normalStrength = numberValue(normal.values, QStringLiteral("strength"), 1.0f);
    companion.roughness = numberValue(rmaos.values, QStringLiteral("roughness"), .5f);
    companion.metallic = numberValue(rmaos.values, QStringLiteral("metallic"), 0.0f);
    companion.ao = numberValue(rmaos.values, QStringLiteral("ao"), 1.0f);
    companion.f0 = numberValue(rmaos.values, QStringLiteral("f0"), .04f);
    companion.useRoughness = companion.hasRmaos && !boolValue(rmaos.values, QStringLiteral("overrideRoughness"), !companion.hasRmaos);
    companion.useMetallic = companion.hasRmaos && !boolValue(rmaos.values, QStringLiteral("overrideMetallic"), !companion.hasRmaos);
    companion.useAo = companion.hasRmaos && !boolValue(rmaos.values, QStringLiteral("overrideAo"), !companion.hasRmaos);
    companion.useF0 = companion.hasRmaos && !boolValue(rmaos.values, QStringLiteral("overrideF0"), !companion.hasRmaos);
    companion.emissiveColor = jsonColor(emissive.values, QStringLiteral("color"), QStringLiteral("#FFFFFF"));
    companion.overrideEmissiveColor = boolValue(emissive.values, QStringLiteral("overrideColor"), !companion.hasEmissive);
    companion.overrideEmissiveMask = boolValue(emissive.values, QStringLiteral("overrideMask"), !companion.hasEmissive);
    companion.emissiveMask = numberValue(emissive.values, QStringLiteral("mask"), 1.0f);
    companion.emissiveIntensity = numberValue(emissive.values, QStringLiteral("intensity"), 0.0f);
    companion.uvTransform = QVector4D(
        float(document.setting(QStringLiteral("UV0"), QStringLiteral("scaleU"), 1.0).toDouble()),
        float(document.setting(QStringLiteral("UV0"), QStringLiteral("scaleV"), 1.0).toDouble()),
        float(document.setting(QStringLiteral("UV0"), QStringLiteral("offsetU"), 0.0).toDouble()),
        float(document.setting(QStringLiteral("UV0"), QStringLiteral("offsetV"), 0.0).toDouble()));
    companion.uvRotation = float(document.setting(QStringLiteral("UV0"), QStringLiteral("rotation"), 0.0).toDouble());
    const QString address = document.setting(QStringLiteral("UV0"), QStringLiteral("address"), QStringLiteral("Wrap / Wrap")).toString();
    companion.uvAddress = address == QStringLiteral("Clamp / Clamp") ? 1 : (address == QStringLiteral("Mirror / Mirror") ? 2 : 0);
    companion.uvAnimation = QVector4D(
        float(document.setting(QStringLiteral("UV0/Animation"), QStringLiteral("scrollU"), 0.0).toDouble()),
        float(document.setting(QStringLiteral("UV0/Animation"), QStringLiteral("scrollV"), 0.0).toDouble()),
        float(document.setting(QStringLiteral("UV0/Animation"), QStringLiteral("rotationSpeed"), 0.0).toDouble()),
        float(document.setting(QStringLiteral("UV0/Animation"), QStringLiteral("phase"), 0.0).toDouble()));
    const QString composition = document.setting(QStringLiteral("Transparency/Composition"), QStringLiteral("mode"), QStringLiteral("Opaque")).toString();
    companion.compositionMode = composition == QStringLiteral("Alpha Test") ? 1
        : composition == QStringLiteral("Alpha Blend") ? 2
        : composition == QStringLiteral("Premultiplied") ? 3
        : composition == QStringLiteral("Additive") ? 4
        : composition == QStringLiteral("Multiply") ? 5
        : composition == QStringLiteral("Physical Transmission") ? 6
        : composition == QStringLiteral("Dedicated Water Pass") ? 7 : 0;
    companion.globalOpacity = float(document.setting(QStringLiteral("Transparency/Composition"), QStringLiteral("alpha"), 1.0).toDouble());
    companion.alphaThreshold = float(document.setting(QStringLiteral("Transparency/Composition"), QStringLiteral("threshold"), .5).toDouble());
    const QString alphaSource = document.setting(QStringLiteral("Transparency/Composition"), QStringLiteral("alphaSource"), QStringLiteral("Base Color A")).toString();
    // Companions never bind a particle gradient, so gradient-driven alpha
    // falls back to the base color channel.
    companion.alphaSource = alphaSource == QStringLiteral("Constant") ? 3 : 0;
    companion.twoSided = document.setting(QStringLiteral("Shading/Core"), QStringLiteral("twoSided"),
        document.shaderMode() == Pbr::ShaderMode::VegetationSurface || document.shaderMode() == Pbr::ShaderMode::HairSurface).toBool();
    const QString normalSpace = document.setting(QStringLiteral("Shading/Core"), QStringLiteral("normalSpace"), QStringLiteral("Tangent")).toString();
    companion.normalSpace = normalSpace == QStringLiteral("Model") ? 1 : (normalSpace == QStringLiteral("World") ? 2 : 0);
    companion.globalNormalStrength = float(document.setting(QStringLiteral("Shading/Core"), QStringLiteral("normalStrength"), 1.0).toDouble());
    companion.iblIntensity = float(document.setting(QStringLiteral("Environment"), QStringLiteral("intensity"), 1.0).toDouble());
    companion.diffuseScale = float(document.setting(QStringLiteral("Environment"), QStringLiteral("diffuse"), 1.0).toDouble());
    companion.specularScale = float(document.setting(QStringLiteral("Environment"), QStringLiteral("specular"), 1.0).toDouble());
    companion.environmentYaw = float(document.setting(QStringLiteral("Environment"), QStringLiteral("rotation"), 0.0).toDouble());
    return companion;
}

void PbrPreviewGL::setCompanionMaterials(const QVector<QPair<QString, QString>>& assignments)
{
    if (context()) makeCurrent();
    m_companions.clear();
    QHash<QString, int> companionByPath;
    for (const auto& assignment : assignments) {
        if (assignment.second.isEmpty()) continue;
        int submeshIndex = -1;
        for (int i = 0; i < m_mesh.submeshes.size(); ++i)
            if (m_mesh.submeshes[i].name == assignment.first) { submeshIndex = i; break; }
        if (submeshIndex < 0) continue;
        int companionIndex = companionByPath.value(assignment.second, -1);
        if (companionIndex < 0) {
            companionIndex = int(m_companions.size());
            m_companions.push_back(makeCompanionMaterial(assignment.second));
            companionByPath.insert(assignment.second, companionIndex);
        }
        m_companions[companionIndex].submeshes.append(submeshIndex);
    }
    if (context()) doneCurrent();
    m_companionsDirty = true;
    update();
}

void PbrPreviewGL::uploadCompanionTextures()
{
    for (auto& companion : m_companions) {
        uploadTexture(companion.baseTexture, companion.baseImage, Qt::white);
        uploadTexture(companion.normalTexture, companion.normalImage, QColor(QStringLiteral("#8080FF")));
        uploadTexture(companion.rmaosTexture, companion.rmaosImage, Qt::white);
        uploadTexture(companion.emissiveTexture, companion.emissiveImage, Qt::black);
    }
    m_companionsDirty = false;
}

void PbrPreviewGL::applyCompanionUniforms(const CompanionMaterial& companion)
{
    // Companions run through the standard-surface path: core maps and
    // constants only, with every layered/special feature of the edited
    // document switched off. The next frame's full uniform upload restores
    // the active material's state, so nothing needs to be saved here.
    m_program->setUniformValue("uShaderMode", int(Pbr::ShaderMode::StandardSurface));
    m_program->setUniformValue("uSpecialMask", 0);
    m_program->setUniformValue("uSecondaryLayerActive", false);
    m_program->setUniformValue("uTertiaryLayerActive", false);
    m_program->setUniformValue("uParticleActive", false);
    m_program->setUniformValue("uParallaxMode", 0);
    m_program->setUniformValue("uHasHeight", false);
    m_program->setUniformValue("uDecalEnabled", false);
    m_program->setUniformValue("uDisplacementEnabled", false);
    m_program->setUniformValue("uVegetationEnabled", false);
    m_program->setUniformValue("uSubsurfaceMode", 0);
    // Companions still weather (states are definition-driven), but through the
    // neutral physical inputs: derived porosity, AO-derived curvature, no
    // lighting-side curvature consumers.
    m_program->setUniformValue("uCurvatureInAlpha", false);
    m_program->setUniformValue("uCavitySpecOcclusion", 0.0f);
    m_program->setUniformValue("uRmaosAlphaPorosity", false);
    m_program->setUniformValue("uPorosityOverride", false);
    m_program->setUniformValue("uSssCurvatureThickness", false);
    m_program->setUniformValue("uStateAdhesion", 1.0f);
    m_program->setUniformValue("uShelterChannel", -1);
    m_program->setUniformValue("uSheenStrength", 0.0f);
    m_program->setUniformValue("uGlintIntensity", 0.0f);
    m_program->setUniformValue("uIridescenceStrength", 0.0f);
    m_program->setUniformValue("uRetroStrength", 0.0f);
    m_program->setUniformValue("uInteriorEnabled", false);
    m_program->setUniformValue("uHasGradient", false);
    m_program->setUniformValue("uUvSet", 0);
    m_program->setUniformValue("uMirrorPrimaryUv", false);
    static const GLint companionMeshSources[3] = {0, 0, 0};
    m_program->setUniformValueArray("uUvSourceMode", companionMeshSources, 3);
    static const GLfloat companionZeroFrequency[3] = {0, 0, 0};
    m_program->setUniformValueArray("uOscFreq", companionZeroFrequency, 3, 1);
    m_program->setUniformValue("uHasBase", companion.hasBase);
    m_program->setUniformValue("uHasNormal", companion.hasNormal);
    m_program->setUniformValue("uHasEmissive", companion.hasEmissive);
    m_program->setUniformValue("uOverrideBaseColor", companion.overrideBaseColor);
    m_program->setUniformValue("uOverrideOpacity", companion.overrideOpacity);
    m_program->setUniformValue("uOverrideNormal", companion.overrideNormal);
    m_program->setUniformValue("uOverrideEmissiveColor", companion.overrideEmissiveColor);
    m_program->setUniformValue("uOverrideEmissiveMask", companion.overrideEmissiveMask);
    m_program->setUniformValue("uUseRoughness", companion.useRoughness);
    m_program->setUniformValue("uUseMetallic", companion.useMetallic);
    m_program->setUniformValue("uUseAo", companion.useAo);
    m_program->setUniformValue("uUseF0", companion.useF0);
    m_program->setUniformValue("uBaseFallback", companion.baseFallback);
    m_program->setUniformValue("uTint", companion.tint);
    m_program->setUniformValue("uNormalFallback", companion.normalFallback);
    m_program->setUniformValue("uEmissiveColor", companion.emissiveColor);
    m_program->setUniformValue("uOpacity", companion.opacity);
    m_program->setUniformValue("uColorScale", companion.colorScale);
    m_program->setUniformValue("uNormalStrength", companion.normalStrength);
    m_program->setUniformValue("uRoughness", companion.roughness);
    m_program->setUniformValue("uMetallic", companion.metallic);
    m_program->setUniformValue("uAo", companion.ao);
    m_program->setUniformValue("uF0", companion.f0);
    m_program->setUniformValue("uEmissiveMask", companion.emissiveMask);
    m_program->setUniformValue("uEmissiveIntensity", companion.emissiveIntensity);
    m_program->setUniformValue("uUvTransform0", companion.uvTransform);
    m_program->setUniformValue("uUvAnimation0", companion.uvAnimation);
    m_program->setUniformValue("uUvRotation0", companion.uvRotation);
    m_program->setUniformValue("uUvAddress0", companion.uvAddress);
    m_program->setUniformValue("uCompositionMode", companion.compositionMode);
    m_program->setUniformValue("uGlobalOpacity", companion.globalOpacity);
    m_program->setUniformValue("uAlphaThreshold", companion.alphaThreshold);
    m_program->setUniformValue("uAlphaSource", companion.alphaSource);
    m_program->setUniformValue("uNormalSpace", companion.normalSpace);
    m_program->setUniformValue("uGlobalNormalStrength", companion.globalNormalStrength);
    m_program->setUniformValue("uEnvironmentYaw", m_environmentYaw + companion.environmentYaw);
    m_program->setUniformValue("uEnvironmentIntensity", m_environmentIntensity * companion.iblIntensity);
    m_program->setUniformValue("uDiffuseScale", companion.diffuseScale);
    m_program->setUniformValue("uSpecularScale", companion.specularScale);
}
void PbrPreviewGL::setProjection(Projection value) { m_projection = value; update(); emit cameraChanged(); }
void PbrPreviewGL::setBackground(Background value) { m_background = value; update(); }
void PbrPreviewGL::setDebugView(DebugView value) { m_debugView = value; update(); }
void PbrPreviewGL::setGroundEnabled(bool value) { m_groundEnabled = value; update(); }
void PbrPreviewGL::setShadows(bool enabled, float intensity) { m_shadowsEnabled=enabled;m_shadowIntensity=std::clamp(intensity,0.0f,1.0f);update(); }
void PbrPreviewGL::setDerivedEnvironmentShadow(bool enabled, float directionalWeight) { m_derivedEnvironmentShadow=enabled;m_derivedEnvironmentShadowWeight=std::clamp(directionalWeight,0.0f,1.0f);update(); }
void PbrPreviewGL::setComparisonEnabled(bool value) { m_comparisonEnabled = value; update(); }

void PbrPreviewGL::setEnvironmentCubemap(const QString& path, const QString& textureRoot)
{
    QString error;
    m_environmentFaces = TextureImage::loadCubemapFaces(path, textureRoot, &error);
    m_cachedEnvironmentLight = QFileInfo(path).suffix().compare(QStringLiteral("exr"), Qt::CaseInsensitive) == 0
        ? deriveEnvironmentLight(m_environmentFaces) : EnvironmentLight{};
    m_environmentDirty = true;
    update();
    setToolTip(m_environmentFaces.size() == 6 ? tr("Preview environment: %1").arg(path)
        : tr("Preview environment unavailable (%1). Using the seamless built-in studio environment.").arg(error));
}

PbrPreviewGL::EnvironmentLight PbrPreviewGL::deriveEnvironmentLight(const QVector<QImage>& faces, float environmentYaw)
{
    EnvironmentLight result;
    if (faces.size() != 6 || faces.front().isNull() || faces.front().width() != faces.front().height()
        || faces.front().format() != QImage::Format_RGBA32FPx4) return result;
    const int size = faces.front().width();
    for (const QImage& face : faces)
        if (face.size() != QSize(size, size) || face.format() != QImage::Format_RGBA32FPx4) return result;

    const int stride = std::max(1, size / 256);
    const int radius = std::max(1, stride);
    const auto localColor = [radius, size](const QImage& image, int x, int y) {
        QVector3D color;
        int count = 0;
        for (int oy : {-radius, 0, radius}) for (int ox : {-radius, 0, radius}) {
            const int sx = std::clamp(x + ox, 0, size - 1), sy = std::clamp(y + oy, 0, size - 1);
            const float* pixel = reinterpret_cast<const float*>(image.constScanLine(sy)) + sx * 4;
            const QVector3D value(std::max(pixel[0], 0.0f), std::max(pixel[1], 0.0f), std::max(pixel[2], 0.0f));
            if (qIsFinite(value.x()) && qIsFinite(value.y()) && qIsFinite(value.z())) { color += value; ++count; }
        }
        return count > 0 ? color / float(count) : QVector3D();
    };
    const auto luminance = [](const QVector3D& color) { return QVector3D::dotProduct(color, QVector3D(.2126f, .7152f, .0722f)); };

    double weightedLuminance = 0, solidAngleSum = 0;
    float maximumLuminance = 0;
    QVector3D maximumDirection, maximumColor;
    for (int face = 0; face < 6; ++face) for (int y = stride / 2; y < size; y += stride) for (int x = stride / 2; x < size; x += stride) {
        const float u = 2.0f * (float(x) + .5f) / float(size) - 1.0f;
        const float v = 2.0f * (float(y) + .5f) / float(size) - 1.0f;
        const float solidAngle = std::pow(1.0f + u * u + v * v, -1.5f);
        const QVector3D color = localColor(faces[face], x, y);
        const float value = luminance(color);
        weightedLuminance += double(value) * solidAngle; solidAngleSum += solidAngle;
        if (value > maximumLuminance) { maximumLuminance = value; maximumDirection = cubeDirection(face, u, v); maximumColor = color; }
    }
    const float averageLuminance = solidAngleSum > 0 ? float(weightedLuminance / solidAngleSum) : 0;
    if (!(maximumLuminance > std::max(averageLuminance * 1.25f, 1.0e-5f))) return result;

    const float threshold = std::min(maximumLuminance * .9f, std::max(averageLuminance * 1.5f, maximumLuminance * .35f));
    QVector3D directionSum, colorSum;
    double weightSum = 0;
    for (int face = 0; face < 6; ++face) for (int y = stride / 2; y < size; y += stride) for (int x = stride / 2; x < size; x += stride) {
        const float u = 2.0f * (float(x) + .5f) / float(size) - 1.0f;
        const float v = 2.0f * (float(y) + .5f) / float(size) - 1.0f;
        const QVector3D color = localColor(faces[face], x, y);
        const float value = luminance(color);
        if (value < threshold) continue;
        const float solidAngle = std::pow(1.0f + u * u + v * v, -1.5f);
        const float weight = (value - threshold) * solidAngle;
        directionSum += cubeDirection(face, u, v) * weight; colorSum += color * weight; weightSum += weight;
    }
    const QVector3D cubeLight = directionSum.lengthSquared() > 1.0e-8f ? directionSum.normalized() : maximumDirection;
    const QVector3D patchColor = weightSum > 0 ? colorSum / float(weightSum) : maximumColor;
    QVector3D worldLight(cubeLight.x(), -cubeLight.z(), cubeLight.y());
    worldLight.normalize();
    const float maximumChannel = std::max({patchColor.x(), patchColor.y(), patchColor.z(), 1.0e-6f});
    const QVector3D normalizedColor = patchColor / maximumChannel;
    result.valid = true;
    result.yaw = std::remainder(qRadiansToDegrees(std::atan2(worldLight.x(), worldLight.z())) - environmentYaw, 360.0f);
    result.pitch = std::clamp(qRadiansToDegrees(std::asin(std::clamp(worldLight.y(), -1.0f, 1.0f))), -89.0f, 89.0f);
    const float contrast = luminance(patchColor) / std::max(averageLuminance, 1.0e-5f);
    result.intensity = std::clamp(1.0f + std::log2(std::max(contrast, 1.0f)), .1f, 8.0f);
    result.color = QColor::fromRgbF(std::clamp(normalizedColor.x(), 0.0f, 1.0f), std::clamp(normalizedColor.y(), 0.0f, 1.0f), std::clamp(normalizedColor.z(), 0.0f, 1.0f));
    return result;
}

PbrPreviewGL::EnvironmentLight PbrPreviewGL::derivedEnvironmentLight(float environmentYaw) const
{
    EnvironmentLight result = m_cachedEnvironmentLight;
    if (result.valid) result.yaw = std::remainder(result.yaw - environmentYaw, 360.0f);
    return result;
}

void PbrPreviewGL::setLighting(float keyIntensity, const QColor& keyColor, float keyYaw, float keyPitch,
    float environmentIntensity, float environmentYaw, float exposure)
{
    m_keyIntensity = keyIntensity; m_keyColor = keyColor; m_keyYaw = keyYaw; m_keyPitch = std::clamp(keyPitch, -89.0f, 89.0f);
    m_environmentIntensity = environmentIntensity; m_environmentYaw = environmentYaw; m_exposure = exposure; update();
}

void PbrPreviewGL::setAxisView(AxisView value)
{
    m_pan = {};
    switch (value) {
    case AxisView::Front: m_yaw = 0; m_pitch = 0; break;
    case AxisView::Back: m_yaw = 180; m_pitch = 0; break;
    case AxisView::Left: m_yaw = -90; m_pitch = 0; break;
    case AxisView::Right: m_yaw = 90; m_pitch = 0; break;
    case AxisView::Top: m_yaw = 0; m_pitch = -90; break;
    case AxisView::Bottom: m_yaw = 0; m_pitch = 90; break;
    }
    update(); emit cameraChanged();
}

void PbrPreviewGL::copyCameraFrom(const PbrPreviewGL* other)
{
    if (!other) return;
    if (qFuzzyCompare(m_yaw, other->m_yaw) && qFuzzyCompare(m_pitch, other->m_pitch)
        && m_pan == other->m_pan && qFuzzyCompare(m_distance, other->m_distance)
        && m_projection == other->m_projection) return;
    m_yaw = other->m_yaw;
    m_pitch = other->m_pitch;
    m_pan = other->m_pan;
    m_distance = other->m_distance;
    m_projection = other->m_projection;
    update();
}

void PbrPreviewGL::setObjectTransform(const QVector3D& rotationDegrees, const QVector3D& translation)
{
    if (m_objectRotation == rotationDegrees && m_objectTranslation == translation) return;
    if (m_liquidEnabled) {
        // Moving the bottle kicks the slosh spring.
        const QVector3D rotationDelta = rotationDegrees - m_objectRotation;
        const QVector3D translationDelta = translation - m_objectTranslation;
        m_wobbleVelocity += QVector2D(
            translationDelta.x() * 6.0f + rotationDelta.z() * .06f,
            translationDelta.z() * 6.0f - rotationDelta.x() * .06f);
    }
    m_objectRotation = rotationDegrees;
    m_objectTranslation = translation;
    update();
}

QMatrix4x4 PbrPreviewGL::objectModelMatrix() const
{
    QMatrix4x4 model;
    model.translate(m_objectTranslation);
    model.rotate(m_objectRotation.z(), 0, 0, 1);
    model.rotate(m_objectRotation.y(), 0, 1, 0);
    model.rotate(m_objectRotation.x(), 1, 0, 0);
    return model;
}

void PbrPreviewGL::frameObject()
{
    // Center the (possibly moved) object: cancel its view-space offset.
    QMatrix4x4 rotation;
    rotation.rotate(m_pitch, 1, 0, 0);
    rotation.rotate(m_yaw, 0, 1, 0);
    const QVector3D viewOffset = rotation.map(m_objectTranslation);
    m_pan = QVector2D(-viewOffset.x(), -viewOffset.y());
    m_distance = 3.2f;
    update();
    emit cameraChanged();
}
void PbrPreviewGL::resetView() { m_yaw = 0; m_pitch = 0; m_projection = Projection::Perspective; frameObject(); }

void PbrPreviewGL::initializeGL()
{
    initializeOpenGLFunctions();
    glEnable(GL_DEPTH_TEST); glEnable(GL_MULTISAMPLE); glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    m_vao = std::make_unique<QOpenGLVertexArrayObject>(); m_vao->create();
    m_groundVao = std::make_unique<QOpenGLVertexArrayObject>(); m_groundVao->create();
    m_vertexBuffer.create(); m_indexBuffer.create(); m_groundVertexBuffer.create(); m_groundIndexBuffer.create();

    m_program = std::make_unique<QOpenGLShaderProgram>();
    static const char* vertexShader = R"GLSL(
        #version 330 core
        layout(location=0) in vec3 aPosition;
        layout(location=1) in vec3 aNormal;
        layout(location=2) in vec4 aTangent;
        layout(location=3) in vec2 aUv0;
        layout(location=4) in vec2 aUv1;
        layout(location=5) in vec4 aColor;
        uniform mat4 uViewProjection;
        uniform mat4 uModel;
        uniform mat3 uModelNormal;
        uniform sampler2D uHeight;
        uniform vec3 uCameraPosition;
        uniform vec4 uUvTransform0,uUvTransform1,uUvAnimation0,uUvAnimation1;
        uniform float uUvRotation0,uUvRotation1,uTime,uHeightFallback,uHeightCenter,uHeightBias;
        uniform float uDisplacementStrength,uDisplacementFadeEnd;
        uniform float uWindDirection,uWindStrength,uWindTimeScale,uSwayAmount,uSwaySpeed,uBranchAmount,uFlutterAmount,uFlutterSpeed,uGustStrength,uTurbulence;
        uniform int uHeightUvSet;
        uniform bool uHasHeight,uOverrideHeight,uHeightBlue,uHeightInvert,uDisplacementEnabled,uVegetationEnabled,uMeshHasVertexColors,uIsGround;
        out vec3 vWorldPosition; out vec3 vNormal; out vec4 vTangent; out vec2 vUv0; out vec2 vUv1;out vec4 vColor;out vec3 vObjectPosition;
        vec2 animatedUv(vec2 uv,vec4 transform,vec4 animation,float rotation){
            float t=uTime+animation.w;uv=uv*transform.xy+transform.zw+animation.xy*t;
            float a=radians(rotation+animation.z*t),c=cos(a),s=sin(a);return mat2(c,-s,s,c)*(uv-vec2(.5))+vec2(.5);
        }
        void main() {
            vObjectPosition=aPosition;
            vec3 position=aPosition,normal=normalize(aNormal);
            vec3 tangent=aTangent.xyz;
            if(!uIsGround){
                position=vec3(uModel*vec4(position,1.0));
                normal=normalize(uModelNormal*normal);
                tangent=normalize(uModelNormal*tangent);
            }
            if(uVegetationEnabled&&!uIsGround){
                float anchor=uMeshHasVertexColors?aColor.a:clamp(1.0-(aUv0.y),0.0,1.0),freeWeight=1.0-anchor;
                vec3 weights=uMeshHasVertexColors?aColor.rgb:vec3(freeWeight,freeWeight*.45,freeWeight*.7);
                float heading=radians(uWindDirection);vec3 wind=vec3(cos(heading),0.0,sin(heading));
                float t=uTime*uWindTimeScale,phase=dot(position.xz,vec2(.73,1.17))*uTurbulence;
                float gust=1.0+uGustStrength*max(sin(t*.73+phase),0.0);
                float sway=sin(t*uSwaySpeed+phase)*uSwayAmount*weights.r;
                float branch=sin(t*uSwaySpeed*1.71+phase*1.9)*uBranchAmount*weights.g;
                float flutter=sin(t*uFlutterSpeed+dot(position,vec3(4.1,7.3,2.7)))*uFlutterAmount*weights.b;
                position+=(wind*(sway+branch)+normal*flutter)*uWindStrength*gust*.055*freeWeight;
            }
            if(uDisplacementEnabled&&uHasHeight&&!uIsGround){
                vec2 uv=uHeightUvSet==1?animatedUv(aUv1,uUvTransform1,uUvAnimation1,uUvRotation1):animatedUv(aUv0,uUvTransform0,uUvAnimation0,uUvRotation0);
                vec4 heightSample=textureLod(uHeight,uv,0.0);float h=uOverrideHeight?uHeightFallback:(uHeightBlue?heightSample.b:heightSample.r);
                if(uHeightInvert)h=1.0-h;h=clamp(h+uHeightBias,0.0,1.0);
                float fade=uDisplacementFadeEnd<=0.0?1.0:1.0-smoothstep(uDisplacementFadeEnd*.75,uDisplacementFadeEnd,length(uCameraPosition-position));
                position+=normal*(h-uHeightCenter)*uDisplacementStrength*.01*fade;
            }
            vWorldPosition=position;vNormal=normal;vTangent=vec4(tangent,aTangent.w);vUv0=aUv0;vUv1=aUv1;vColor=aColor;
            gl_Position=uViewProjection*vec4(position,1.0);
        }
    )GLSL";
    static const char* fragmentShader = R"GLSL(
        #version 330 core
        in vec3 vWorldPosition; in vec3 vNormal; in vec4 vTangent; in vec2 vUv0; in vec2 vUv1;in vec4 vColor;in vec3 vObjectPosition;
        out vec4 fragColor;
        uniform sampler2D uBase,uNormal,uHeight,uRmaos,uEmissive,uShadowMap;
        uniform sampler2D uSecondaryBase,uSecondaryNormal,uSecondaryRmaos,uSecondaryEmissive,uLayerCoverage,uGradient;
        uniform sampler2D uTertiaryBase,uTertiaryNormal,uTertiaryRmaos,uTertiaryEmissive,uTertiaryCoverage;
        uniform bool uSecondaryLayerActive,uTertiaryLayerActive,uTertiaryUsesUv0;
        uniform bool uHasTertiaryBase,uHasTertiaryNormal,uHasTertiaryEmissive,uHasTertiaryCoverage;
        uniform bool uOverrideTertiaryBaseColor,uOverrideTertiaryOpacity,uOverrideTertiaryNormal,uOverrideTertiaryEmissiveColor,uOverrideTertiaryEmissiveMask;
        uniform bool uOverrideTertiaryCoverage,uInvertTertiaryCoverage;
        uniform bool uUseTertiaryRoughness,uUseTertiaryMetallic,uUseTertiaryAo,uUseTertiaryF0;
        uniform vec3 uTertiaryBaseFallback,uTertiaryTint,uTertiaryNormalFallback,uTertiaryEmissiveColorFallback;
        uniform float uTertiaryOpacity,uTertiaryColorScale,uTertiaryNormalStrength;
        uniform float uTertiaryRoughness,uTertiaryMetallic,uTertiaryAo,uTertiaryF0,uTertiaryEmissiveMask,uTertiaryEmissiveIntensity;
        uniform int uTertiaryBlendMode,uTCoverageMode,uTPatternSource;
        uniform float uTConstantCoverage,uTCoverageBias,uTCoverageContrast,uTCoverageSoftness;
        uniform float uTertiaryCoverageFallback,uTertiaryCoverageSlotBias,uTertiaryCoverageSlotContrast;
        uniform vec4 uUvTransform2,uUvAnimation2;
        uniform float uUvRotation2;
        uniform int uUvAddress2;
        uniform bool uPrimaryHeightInBlue,uSecondaryHeightInBlue,uTertiaryHeightInBlue;
        uniform vec3 uTransmissionTint;
        uniform int uAnisotropyLayer,uBentNormalLayer;
        uniform float uAnisotropyIntensity,uAnisotropyRotation,uBentNormalStrength;
        uniform bool uSpecularAa;
        uniform float uSpecularAaStrength,uRetroStrength,uRetroWidth;
        uniform bool uAntiTiling;
        uniform float uAntiTileScale,uAntiTileContrast;
        uniform int uStateMasksLayer,uShelterChannel;
        // Generic surface state slots: amounts arrive with interaction physics
        // already applied on the CPU; each slot's profile lives in these arrays.
        uniform float uStateAmount[8],uStateDesat[8],uStatePorosityInfluence[8],uStateRoughTarget[8],uStateRoughWeight[8];
        uniform float uStateF0Target[8],uStateF0Weight[8],uStateFilmF0[8],uStateSparkle[8],uStateGlowIntensity[8];
        uniform float uStateCavityBias[8],uStateThreshold[8],uStateSoftness[8],uStateDirectionality[8],uStateStreaking[8],uStateRetention[8];
        uniform float uStateAoBias[8],uStateHeightBias[8],uStateRoughBias[8],uStateMetalSeal[8],uStateEmissiveDry[8];
        uniform int uStateBlendMode[8],uStateRoughMode[8],uStateKillMetallic[8],uStateRipples[8],uStateMaskChannel[8],uStateRespectsShelter[8];
        uniform bool uCurvatureInAlpha,uRmaosAlphaPorosity,uPorosityOverride,uSssCurvatureThickness;
        uniform float uCavitySpecOcclusion,uPorosityConstant,uStateAdhesion;
        uniform vec3 uStateColor[8];
        uniform bool uMeshHasVertexColors;
        uniform samplerCube uInterior;
        uniform bool uInteriorEnabled;
        uniform vec2 uInteriorRooms;
        uniform float uInteriorDepth,uInteriorBrightness,uInteriorEmissive;
        uniform bool uParticleActive;
        uniform int uFlipbookEnabled[3],uFlipbookDriver[3],uFlipbookLoop[3],uFlipbookBlendOn[3],uPolarUvOn[3];
        uniform vec2 uFlipbookGrid[3];
        uniform float uFlipbookFps[3],uFlipbookConstFrame[3];
        uniform int uLayerRole[3],uErodeOn[3];
        uniform float uDistortStrength[3],uAdvectSpeed[3],uErodeSoftness[3],uErodeGlow[3];
        uniform vec3 uErodeTint[3];
        uniform int uPaletteMode,uRandomFlipStart;
        uniform float uPaletteIntensity,uRandomPhase,uRandomHue;
        uniform float uParticleFresnelFade,uParticleFresnelPower,uParticleNearFade;
        uniform float uParticleAge,uParticleSeed;
        uniform int uSixWayALayer,uSixWayBLayer;
        uniform float uSixWayAmbient,uSixWayIntensity,uSixWayFireIntensity;
        uniform int uMotionVectorsLayer,uDetailNormalLayer;
        uniform float uMotionStrength,uDetailNormalStrength,uDetailNormalScale,uDetailRoughVariation;
        uniform float uStateEdgeShedding[8];
        uniform bool uLandscapeUsePorosityTexture;
        uniform float uLandscapePorosity;
        // Planar / triplanar UV sources per layer: mode 0 mesh, 1 world, 2 object;
        // axis 0 triplanar, 1..3 = X/Y/Z plane.
        uniform int uUvSourceMode[3],uUvProjAxis[3];
        uniform float uTriplanarSharp[3];
        // Per-layer oscillator ("breathing"): one waveform per layer driving UV
        // scale/offset/rotation, emissive intensity, coverage, and normal strength.
        uniform int uOscWave[3];
        uniform float uOscFreq[3],uOscPhase[3],uOscDuty[3];
        uniform float uOscScaleAmp[3],uOscRotAmp[3],uOscEmissiveAmp[3],uOscCoverageAmp[3],uOscNormalAmp[3];
        uniform vec2 uOscOffsetAmp[3];
        uniform bool uLiquidPass;
        uniform vec3 uLiquidPlaneN,uLiquidPlaneNWorld,uLiquidColor,uMeniscusColor;
        uniform float uLiquidFillY,uLiquidAbsorption,uMeniscusWidth,uLiquidSurfaceBrightness;
        uniform samplerCube uEnvironment,uPrefilteredEnvironment;
        uniform sampler2D uBrdfLut;uniform sampler2DArray uSpecial;
        uniform bool uHasBase,uHasNormal,uHasHeight,uHasEmissive,uUseRoughness,uUseMetallic,uUseAo,uUseF0,uOverrideBaseColor,uOverrideOpacity,uOverrideNormal;
        uniform bool uOverrideEmissiveColor,uOverrideEmissiveMask,uOverrideHeight,uHeightBlue,uHeightInvert,uParallaxClip,uIsGround,uCompare,uShadowsEnabled,uDerivedEnvironmentShadow;
        uniform bool uHasSecondaryBase,uHasSecondaryNormal,uHasSecondaryEmissive,uHasLayerCoverage,uOverrideLayerCoverage,uInvertLayerCoverage;
        uniform bool uOverrideSecondaryBaseColor,uOverrideSecondaryOpacity,uOverrideSecondaryNormal,uOverrideSecondaryEmissiveColor,uOverrideSecondaryEmissiveMask;
        uniform bool uUseSecondaryRoughness,uUseSecondaryMetallic,uUseSecondaryAo,uUseSecondaryF0;
        uniform bool uHasGradient,uOverrideGradientColor,uOverrideGradientOpacity,uUnlitVertexColor,uUnlitExposure;
        uniform bool uDecalBaseColor,uDecalNormal,uDecalRoughness,uDecalMetallic,uDecalAo,uDecalEmissive;
        uniform int uUvSet,uHeightUvSet,uDebugView,uParallaxMode,uParallaxMinSteps,uParallaxMaxSteps,uParallaxRefinementSteps,uParallaxShadowSteps;
        uniform int uUvAddress0,uUvAddress1,uCompositionMode,uNormalSpace,uSecondaryBlendMode,uCoverageMode,uPatternSource,uAlphaSource;
        uniform int uShaderMode,uSpecialMask,uTintOverlap,uDynamicDriverSource,uDynamicBlend,uSubsurfaceMode,uDecalBlend,uGradientDriver,uSecondaryGradientDriver;
        uniform vec3 uBaseFallback,uTint,uNormalFallback,uEmissiveColor,uKeyColor,uKeyDirection,uCameraPosition;
        uniform vec3 uSecondaryBaseFallback,uSecondaryTint,uSecondaryNormalFallback,uSecondaryEmissiveColor;
        uniform vec3 uTintColorR,uTintColorG,uTintColorB,uTintColorA,uSubsurfaceProfileColor,uGradientColor;
        uniform vec4 uUvTransform0,uUvTransform1,uUvAnimation0,uUvAnimation1;
        uniform mat4 uShadowMatrix;
        uniform float uOpacity,uColorScale,uNormalStrength,uRoughness,uMetallic,uAo,uF0,uEmissiveMask,uEmissiveIntensity;
        uniform float uHeightFallback,uHeightCenter,uHeightBias,uParallaxDepth,uParallaxGrazingFade,uParallaxShadowStrength,uParallaxShadowBias,uParallaxShadowSoftness,uParallaxFadeStart,uParallaxFadeEnd;
        uniform float uKeyIntensity,uEnvironmentIntensity,uEnvironmentYaw,uExposure,uViewportWidth,uEnvironmentSourceMaxLod,uPrefilteredEnvironmentMaxLod,uShadowIntensity,uDerivedEnvironmentShadowWeight;
        uniform float uUvRotation0,uUvRotation1,uGlobalOpacity,uAlphaThreshold,uGlobalNormalStrength,uDiffuseScale,uSpecularScale,uTime;
        uniform float uSecondaryOpacity,uSecondaryColorScale,uSecondaryNormalStrength,uSecondaryRoughness,uSecondaryMetallic,uSecondaryAo,uSecondaryF0,uSecondaryEmissiveMask,uSecondaryEmissiveIntensity;
        uniform float uConstantCoverage,uCoverageBias,uCoverageContrast,uCoverageSoftness,uLayerCoverageFallback,uLayerCoverageSlotBias,uLayerCoverageSlotContrast;
        uniform float uGradientOpacity,uGradientIntensity,uGradientConstant,uUnlitIntensity;
        uniform float uDynamicStrength,uDynamicDriverValue,uDynamicInputMin,uDynamicInputMax;
        uniform float uSubsurfaceWeight,uSubsurfaceTransmission,uSubsurfaceThickness;
        uniform float uSheenStrength,uSheenRoughness,uGlintIntensity,uGlintScale,uFilmIor,uIridescenceStrength;
        uniform float uHairPrimaryStrength,uHairPrimaryRoughness,uHairPrimaryShift,uHairSecondaryStrength,uHairSecondaryRoughness,uHairSecondaryShift,uHairScatter,uHairBacklight,uHairDirectionStrength;
        uniform float uGlassIor,uGlassAbsorptionDistance,uGlassRefractionRoughness,uGlassThickness;
        uniform float uWaterIor,uWaterAbsorptionDistance,uWaterScattering,uWaterReflectionStrength,uWaterFlowSpeed,uWaterFlowStrength;
        uniform float uEyeCorneaIor,uEyeIrisDepth,uEyeLimbalStrength,uEyePupilScale,uEyeWetness;
        uniform float uDistortionVectorScale,uLandscapeMacroScale,uLandscapeMacroColorStrength,uLandscapeMacroRoughnessStrength,uSecondaryGradientIntensity,uSecondaryGradientConstant;
        uniform float uCoatStrength[3],uCoatIor[3],uCoatNormalStrength[3],uCoatAbsorption[3];
        uniform vec3 uCoatTint[3];
        uniform int uCoatSpecialLayer[3];
        uniform float uDecalAngleFade,uDecalDistanceFade;
        uniform bool uDynamicInvert,uGlassThinWalled,uWaterShoreFoam,uDecalEnabled;
        uniform bool uMirrorPrimaryUv;
        const float PI=3.14159265359;
        vec3 rotateY(vec3 v,float a){float c=cos(a),s=sin(a);return vec3(c*v.x+s*v.z,v.y,-s*v.x+c*v.z);}
        vec2 addressUv(vec2 uv,int mode){
            if(mode==1)return clamp(uv,vec2(0.0),vec2(1.0));
            if(mode==2){vec2 tile=mod(floor(uv),2.0);vec2 f=fract(uv);return mix(f,1.0-f,tile);}
            return fract(uv);
        }
        vec2 transformUv(vec2 uv,vec4 transform,vec4 animation,float degreesValue,int addressMode){
            float timeValue=uTime+animation.w;uv=uv*transform.xy+transform.zw+animation.xy*timeValue;
            float a=radians(degreesValue+animation.z*timeValue),c=cos(a),s=sin(a);
            uv=mat2(c,-s,s,c)*(uv-vec2(.5))+vec2(.5);return addressUv(uv,addressMode);
        }
        vec3 environmentDirection(vec3 v){v=rotateY(v,radians(uEnvironmentYaw));return vec3(v.x,v.z,-v.y);}
        vec3 tonemap(vec3 c){c*=exp2(uExposure);c=c/(c+vec3(1.0));return pow(max(c,vec3(0)),vec3(1.0/2.2));}
        vec3 diffuseIrradiance(vec3 n){
            vec3 up=abs(n.y)<.999?vec3(0,1,0):vec3(1,0,0);
            vec3 tangent=normalize(cross(up,n)),bitangent=cross(n,tangent);
            vec3 irradiance=vec3(0.0);
            const int sampleCount=32;
            const float goldenAngle=2.39996323;
            // Diffuse IBL represents a very broad hemisphere integral. Sampling
            // HDR mip 1 left individual sun/sky texels visible as static white
            // streaks on non-metals. Use the clean generated source chain near
            // its 4x4 level so each directional sample already represents the
            // solid angle covered by this low-count cosine convolution.
            float sourceLod=max(uEnvironmentSourceMaxLod-2.0,0.0);
            for(int i=0;i<sampleCount;++i){
                float xi=(float(i)+.5)/float(sampleCount);
                float radius=sqrt(xi),phi=float(i)*goldenAngle;
                vec3 local=vec3(radius*cos(phi),radius*sin(phi),sqrt(1.0-xi));
                vec3 direction=normalize(tangent*local.x+bitangent*local.y+n*local.z);
                irradiance+=textureLod(uEnvironment,environmentDirection(direction),sourceLod).rgb;
            }
            return irradiance/float(sampleCount);
        }
        float D_GGX(float n,float a){float a2=a*a,d=n*n*(a2-1.0)+1.0;return a2/max(PI*d*d,.0001);}
        float G1(float n,float k){return n/(n*(1.0-k)+k);}
        vec3 fresnel(float h,vec3 f){return f+(1.0-f)*pow(1.0-h,5.0);}
        bool hasSpecial(int layer){return (uSpecialMask&(1<<layer))!=0;}
        vec2 polarizeUv(vec2 uv){
            vec2 centered=uv-vec2(.5);
            return vec2(atan(centered.y,centered.x)/6.2831853+.5,length(centered)*2.0);
        }
        float flipbookFrame(int layer){
            float frames=max(uFlipbookGrid[layer].x*uFlipbookGrid[layer].y,1.0);
            if(uFlipbookDriver[layer]==2)return clamp(uFlipbookConstFrame[layer],0.0,frames-1.0);
            float t=uFlipbookDriver[layer]==0
                ?uParticleAge*(uFlipbookFps[layer]>0.0?uFlipbookFps[layer]:frames)
                :uTime*max(uFlipbookFps[layer],1.0);
            if(uRandomFlipStart==1)t+=floor(uParticleSeed*frames);
            float frame;
            if(uFlipbookLoop[layer]==1)frame=min(t,frames-1.0);
            else if(uFlipbookLoop[layer]==2){float cycle=mod(t,2.0*frames);frame=cycle<frames?cycle:2.0*frames-cycle-.0001;}
            else frame=mod(t,frames);
            return clamp(frame,0.0,frames-1.0);
        }
        vec2 flipbookUv(int layer,vec2 uv,float frame){
            vec2 grid=uFlipbookGrid[layer];
            float index=floor(frame);
            vec2 cell=vec2(mod(index,grid.x),floor(index/grid.x));
            return (fract(uv)+cell)/grid;
        }
        vec3 blendLayer(vec3 base,vec3 layer,float coverage,int mode){
            vec3 blended;
            if(mode==1)blended=base*layer;
            else if(mode==2)blended=base*layer*2.0;
            else if(mode==3)blended=base+layer;
            else if(mode==4)blended=1.0-(1.0-base)*(1.0-layer);
            else if(mode==5)blended=max(base-layer,vec3(0.0));
            else if(mode==6)blended=min(base,layer);
            else if(mode==7)blended=max(base,layer);
            else if(mode==8)blended=mix(2.0*base*layer,1.0-2.0*(1.0-base)*(1.0-layer),step(vec3(.5),base));
            else blended=layer;
            return mix(base,blended,coverage);
        }
        // Heitz-style stochastic hex tiling: three randomized simplex-grid taps
        // blended by sharpened barycentrics break visible terrain repetition.
        vec2 antiTileHash(vec2 p){return fract(sin(vec2(dot(p,vec2(127.1,311.7)),dot(p,vec2(269.5,183.3))))*43758.5453);}
        void antiTileGrid(vec2 uv,out vec3 weights,out vec2 vertex1,out vec2 vertex2,out vec2 vertex3){
            vec2 skewed=vec2(uv.x-uv.y*.57735026,uv.y*1.1547005)*uAntiTileScale;
            vec2 cell=floor(skewed);vec3 corner=vec3(fract(skewed),0.0);corner.z=1.0-corner.x-corner.y;
            if(corner.z>0.0){weights=vec3(corner.z,corner.y,corner.x);vertex1=cell;vertex2=cell+vec2(0.0,1.0);vertex3=cell+vec2(1.0,0.0);}
            else{weights=vec3(-corner.z,1.0-corner.y,1.0-corner.x);vertex1=cell+vec2(1.0,1.0);vertex2=cell+vec2(1.0,0.0);vertex3=cell+vec2(0.0,1.0);}
        }
        vec4 antiTileSample(sampler2D tex,vec2 uv){
            if(!uAntiTiling||uShaderMode!=7)return texture(tex,uv);
            vec3 weights;vec2 vertex1,vertex2,vertex3;antiTileGrid(uv,weights,vertex1,vertex2,vertex3);
            vec2 duvdx=dFdx(uv),duvdy=dFdy(uv);
            weights=pow(max(weights,vec3(0.0)),vec3(uAntiTileContrast));
            weights/=max(weights.x+weights.y+weights.z,.0001);
            return textureGrad(tex,uv+antiTileHash(vertex1),duvdx,duvdy)*weights.x
                +textureGrad(tex,uv+antiTileHash(vertex2),duvdx,duvdy)*weights.y
                +textureGrad(tex,uv+antiTileHash(vertex3),duvdx,duvdy)*weights.z;
        }
        vec4 specialSample(int layer,vec2 uv){return hasSpecial(layer)?texture(uSpecial,vec3(uv,float(layer))):vec4(0.0);}
        vec2 planarProject(vec3 p,int axis){return axis==0?p.zy:(axis==1?p.xz:p.xy);}
        float oscWave(int mode,float t,float duty){
            float p=fract(t);
            if(mode==1)return 4.0*abs(p-.5)-1.0;                       // triangle
            if(mode==2)return p<clamp(duty,.02,.98)?1.0:-1.0;          // pulse
            if(mode==3){float s=sin(p*6.2831853);return sign(s)*pow(abs(s),.55);} // breathing: eased, fuller crests
            return sin(p*6.2831853);
        }
        // Primary-layer triplanar state, filled in main when the layer projects
        // triplanar; layer0Sample blends three normal-weighted planar taps for
        // the core maps. Other samples use the dominant-axis projection.
        bool gTriplanarOn=false;
        vec2 gTriplanarUvX,gTriplanarUvY,gTriplanarUvZ;
        vec3 gTriplanarWeights=vec3(0.0);
        vec4 layer0Sample(sampler2D tex,vec2 uv){
            if(!gTriplanarOn)return antiTileSample(tex,uv);
            return texture(tex,gTriplanarUvX)*gTriplanarWeights.x
                +texture(tex,gTriplanarUvY)*gTriplanarWeights.y
                +texture(tex,gTriplanarUvZ)*gTriplanarWeights.z;
        }
        float hash13(vec3 p){p=fract(p*.1031);p+=dot(p,p.yzx+33.33);return fract((p.x+p.y)*p.z);}
        float vnoise3(vec3 p){
            vec3 i=floor(p),f=fract(p);f=f*f*(3.0-2.0*f);
            float a=hash13(i),b=hash13(i+vec3(1,0,0)),c=hash13(i+vec3(0,1,0)),d=hash13(i+vec3(1,1,0));
            float e=hash13(i+vec3(0,0,1)),g=hash13(i+vec3(1,0,1)),h=hash13(i+vec3(0,1,1)),j=hash13(i+vec3(1,1,1));
            return mix(mix(mix(a,b,f.x),mix(c,d,f.x),f.y),mix(mix(e,g,f.x),mix(h,j,f.x),f.y),f.z);
        }
        // Raindrop drips: droplet heads sliding down vertical surfaces leaving
        // fading trails above them. Evaluates neighboring columns so heads stay
        // continuous across column borders. Returns the trail/head wetting mask
        // and outputs the head's bump gradient in the (horizontal, up) wall plane.
        float stateDripField(vec3 wp,vec3 n,float t,out vec2 gradient){
            gradient=vec2(0.0);
            float verticality=1.0-abs(n.y);
            if(verticality<.25)return 0.0;
            vec2 horizontalN=vec2(n.x,n.z);float horizontalLen=max(length(horizontalN),1e-4);horizontalN/=horizontalLen;
            bool facesX=abs(horizontalN.x)>abs(horizontalN.y);
            float u=facesX?wp.z:wp.x;
            // Planar-projection quality: the u coordinate compresses as the
            // wall turns diagonal toward the projection axis, so fade the
            // drips out before trails can stretch across those regions.
            float across=facesX?abs(horizontalN.y):abs(horizontalN.x);
            float quality=1.0-smoothstep(.5,.72,across);
            if(quality<=0.0)return 0.0;
            const float columns=8.0;
            float mask=0.0;
            for(int c=-1;c<=1;++c){
                float col=floor(u*columns)+float(c);
                float center=(col+.5)/columns;
                float seed=hash13(vec3(col,7.7,3.1));
                float speed=.10+.25*seed;
                float cycle=fract(t*speed+seed*11.0);
                float active=step(.35,hash13(vec3(col,floor(t*speed+seed*11.0),9.3)));
                float headY=1.2-cycle*2.8;
                vec2 offset=vec2((u-center)*6.0,wp.y-headY);
                float radius=length(offset);
                float head=exp(-radius*radius*90.0)*active;
                float above=wp.y-headY;
                float trail=smoothstep(0.0,.08,above)*smoothstep(1.1,.12,above)*smoothstep(.07,.015,abs(u-center))*active;
                mask=max(mask,max(head,trail*.7)*verticality*quality);
                if(radius>1e-4)gradient+=-normalize(offset)*head*verticality*quality;
            }
            return mask;
        }
        // Expanding hashed rain-ripple rings; returns a tangent-plane height
        // gradient for perturbing the shading normal on up-facing wet surfaces.
        // Each octave evaluates its 3x3 cell neighborhood so rings stay
        // continuous across cell borders instead of clipping to squares.
        vec2 stateRippleGradient(vec2 p,float t){
            vec2 gradient=vec2(0.0);
            for(int k=0;k<2;++k){
                vec2 scaled=p*(7.0+float(k)*4.5)+vec2(float(k)*17.0,float(k)*9.0);
                vec2 baseCell=floor(scaled);
                for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x){
                    vec2 cell=baseCell+vec2(float(x),float(y));
                    vec2 center=cell+vec2(hash13(vec3(cell,1.7)),hash13(vec3(cell,9.1)));
                    float phase=hash13(vec3(cell,4.3));
                    float cycle=fract(t*.7+phase);
                    vec2 offset=scaled-center;float radius=length(offset);
                    float wave=radius*18.0-cycle*14.0;
                    float envelope=max(0.0,1.0-cycle*1.15)*smoothstep(1.0,.15,radius)
                        *(1.0-smoothstep(cycle*1.4-.12,cycle*1.4+.12,radius*.9));
                    if(radius>1e-4)gradient+=normalize(offset)*cos(wave)*envelope;
                }
            }
            return gradient;
        }
        vec3 blendNormal(vec3 baseNormal,vec3 detailNormal,float weight,int mode){
            if(mode==2)return normalize(mix(baseNormal,detailNormal,weight));
            if(mode==1)return normalize(vec3(baseNormal.xy+detailNormal.xy,baseNormal.z*detailNormal.z));
            vec3 t=baseNormal+vec3(0,0,1),u=detailNormal*vec3(-1,-1,1);return normalize(mix(baseNormal,t*dot(t,u)/max(t.z,.001)-u,weight));
        }
        vec3 thinFilmColor(float noV,float thickness,float ior){
            float phase=(1.0-noV)*ior*3.0+thickness*6.0;
            return .5+.5*cos(6.2831853*(vec3(0.0,.333,.667)+phase));
        }
        float heightValue(vec2 uv){
            vec4 value=texture(uHeight,uv);float h=uOverrideHeight?uHeightFallback:(uHeightBlue?value.b:value.r);
            if(uHeightInvert)h=1.0-h;return clamp(h+uHeightBias,0.0,1.0);
        }
        float depthValue(vec2 uv){return 1.0-heightValue(uv);}
        float distanceFade(){
            float d=length(uCameraPosition-vWorldPosition);
            if(uParallaxFadeEnd<=uParallaxFadeStart)return d<uParallaxFadeEnd?1.0:0.0;
            return 1.0-smoothstep(uParallaxFadeStart,uParallaxFadeEnd,d);
        }
        vec2 parallaxUv(vec2 baseUv,vec3 viewTs,out float hitDepth){
            float viewZ=max(abs(viewTs.z),.035);
            float grazing=smoothstep(0.0,max(uParallaxGrazingFade,.0001),viewZ);
            float scale=uParallaxDepth*distanceFade()*grazing;
            hitDepth=depthValue(baseUv);
            if(uParallaxMode==1){
                float centered=heightValue(baseUv)-uHeightCenter;
                return baseUv-viewTs.xy/viewZ*(scale*centered);
            }
            float count=mix(float(uParallaxMaxSteps),float(uParallaxMinSteps),clamp(viewZ,0.0,1.0));
            float layerStep=1.0/max(count,1.0);
            vec2 uvStep=viewTs.xy/viewZ*scale/max(count,1.0);
            vec2 current=baseUv+viewTs.xy/viewZ*scale*(.5-uHeightCenter);
            vec2 previous=current;float layer=0.0,previousLayer=0.0,sampled=depthValue(current);
            for(int i=0;i<128;++i){
                if(i>=uParallaxMaxSteps||layer>=sampled)break;
                previous=current;previousLayer=layer;current-=uvStep;layer+=layerStep;sampled=depthValue(current);
            }
            float previousSample=depthValue(previous);
            float after=sampled-layer,before=previousSample-previousLayer;
            float weight=clamp(after/max(after-before,.00001),0.0,1.0);
            vec2 low=current,high=previous;float lowLayer=layer,highLayer=previousLayer;
            current=mix(current,previous,weight);layer=mix(layer,previousLayer,weight);
            for(int i=0;i<8;++i){
                if(i>=uParallaxRefinementSteps)break;
                vec2 mid=(low+high)*.5;float midLayer=(lowLayer+highLayer)*.5;
                if(midLayer>=depthValue(mid)){low=mid;lowLayer=midLayer;}else{high=mid;highLayer=midLayer;}
                current=(low+high)*.5;layer=(lowLayer+highLayer)*.5;
            }
            hitDepth=clamp(layer,0.0,1.0);return current;
        }
        float parallaxShadow(vec2 uv,float hitDepth,vec3 lightTs){
            if(lightTs.z<=.001||hitDepth<=.001)return 1.0;
            int count=max(uParallaxShadowSteps,1);float layerStep=hitDepth/float(count);
            vec2 uvStep=lightTs.xy/max(lightTs.z,.035)*uParallaxDepth*layerStep;
            vec2 sampleUv=uv+uvStep;float rayDepth=hitDepth-layerStep-uParallaxShadowBias;
            float occlusion=0.0;
            for(int i=0;i<64;++i){
                if(i>=count||rayDepth<=0.0)break;
                float blocker=depthValue(sampleUv);
                if(blocker<rayDepth)occlusion=max(occlusion,clamp((rayDepth-blocker)/max(uParallaxShadowSoftness,.001),0.0,1.0));
                sampleUv+=uvStep;rayDepth-=layerStep;
            }
            return 1.0-clamp(occlusion*uParallaxShadowStrength,0.0,1.0);
        }
        float shadowVisibility(vec3 worldPosition,vec3 normal){
            if(!uShadowsEnabled||uShadowIntensity<=0.0)return 1.0;
            vec4 lightPosition=uShadowMatrix*vec4(worldPosition,1.0);
            vec3 coordinate=lightPosition.xyz/lightPosition.w*.5+.5;
            if(any(lessThan(coordinate,vec3(0.0)))||any(greaterThan(coordinate,vec3(1.0))))return 1.0;
            vec2 texel=1.0/vec2(textureSize(uShadowMap,0));
            float bias=max(.00035*(1.0-max(dot(normalize(normal),normalize(uKeyDirection)),0.0)),.00008);
            float occlusion=0.0;
            for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x)
                occlusion+=coordinate.z-bias>texture(uShadowMap,coordinate.xy+vec2(x,y)*texel).r?1.0:0.0;
            return 1.0-uShadowIntensity*occlusion/9.0;
        }
        void main(){
            if(uIsGround){
                float grid=min(abs(fract(vWorldPosition.x)-.5),abs(fract(vWorldPosition.z)-.5));
                vec3 ground=mix(vec3(.11),vec3(.16),smoothstep(.015,.035,grid));
                vec3 groundNormal=normalize(vNormal);float visibility=shadowVisibility(vWorldPosition,groundNormal);
                float environmentLight=.35+.35*uEnvironmentIntensity;
                if(uDerivedEnvironmentShadow)environmentLight*=mix(1.0,visibility,uDerivedEnvironmentShadowWeight);
                float direct=max(dot(groundNormal,normalize(uKeyDirection)),0.0)*uKeyIntensity*.16*visibility;
                fragColor=vec4(tonemap(ground*(environmentLight+direct)),1);return;
            }
            if(uLiquidPass){
                // HLA-style fake liquid: clip against the animated fill plane;
                // backfaces exposed by the clip become the flat surface cap.
                float planeDist=dot(vObjectPosition-vec3(0.0,uLiquidFillY,0.0),uLiquidPlaneN);
                if(planeDist>0.0)discard;
                vec3 liquid=pow(max(uLiquidColor,vec3(.001)),vec3(2.2));
                vec3 keyDir=normalize(uKeyDirection);
                vec3 liquidShaded;
                if(!gl_FrontFacing){
                    float capLight=.35+.65*max(dot(normalize(uLiquidPlaneNWorld),keyDir),0.0);
                    liquidShaded=liquid*uLiquidSurfaceBrightness*capLight*uKeyColor*uKeyIntensity*.35;
                }else{
                    float depth=clamp(-planeDist*uLiquidAbsorption*5.0,0.0,6.0);
                    float sideLight=.3+.7*max(dot(normalize(vNormal),keyDir),0.0);
                    liquidShaded=liquid*exp(-depth)*sideLight*uKeyColor*uKeyIntensity*.3;
                    float meniscus=1.0-clamp(-planeDist/max(uMeniscusWidth,.001),0.0,1.0);
                    liquidShaded+=pow(max(uMeniscusColor,vec3(.001)),vec3(2.2))*meniscus*meniscus;
                }
                fragColor=vec4(tonemap(liquidShaded),1.0);
                return;
            }
            vec3 layerOsc=vec3(0.0);
            for(int i=0;i<3;++i)if(uOscFreq[i]!=0.0)layerOsc[i]=oscWave(uOscWave[i],uTime*uOscFreq[i]+uOscPhase[i],uOscDuty[i]);
            vec4 oscT0=uUvTransform0,oscT1=uUvTransform1,oscT2=uUvTransform2;
            oscT0.xy*=1.0+uOscScaleAmp[0]*layerOsc.x;oscT0.zw+=uOscOffsetAmp[0]*layerOsc.x;
            oscT1.xy*=1.0+uOscScaleAmp[1]*layerOsc.y;oscT1.zw+=uOscOffsetAmp[1]*layerOsc.y;
            oscT2.xy*=1.0+uOscScaleAmp[2]*layerOsc.z;oscT2.zw+=uOscOffsetAmp[2]*layerOsc.z;
            float oscRot0=uUvRotation0+uOscRotAmp[0]*layerOsc.x;
            float oscRot1=uUvRotation1+uOscRotAmp[1]*layerOsc.y;
            float oscRot2=uUvRotation2+uOscRotAmp[2]*layerOsc.z;
            vec2 meshUv0=vUv0,meshUv1=uMirrorPrimaryUv?vUv0:vUv1,meshUv2=uTertiaryUsesUv0?vUv0:vUv1;
            {
                // Planar / triplanar coordinate sources: project world or object
                // position onto the chosen plane; triplanar picks the dominant
                // axis here and (for the primary layer) blends three taps later.
                vec3 projAbsN=abs(normalize(vNormal));
                int dominantAxis=projAbsN.x>projAbsN.y?(projAbsN.x>projAbsN.z?0:2):(projAbsN.y>projAbsN.z?1:2);
                if(uUvSourceMode[0]>0){vec3 p=uUvSourceMode[0]==1?vWorldPosition:vObjectPosition;meshUv0=planarProject(p,uUvProjAxis[0]==0?dominantAxis:uUvProjAxis[0]-1);}
                if(uUvSourceMode[1]>0){vec3 p=uUvSourceMode[1]==1?vWorldPosition:vObjectPosition;meshUv1=planarProject(p,uUvProjAxis[1]==0?dominantAxis:uUvProjAxis[1]-1);}
                if(uUvSourceMode[2]>0){vec3 p=uUvSourceMode[2]==1?vWorldPosition:vObjectPosition;meshUv2=planarProject(p,uUvProjAxis[2]==0?dominantAxis:uUvProjAxis[2]-1);}
                if(uUvSourceMode[0]>0&&uUvProjAxis[0]==0){
                    vec3 p=uUvSourceMode[0]==1?vWorldPosition:vObjectPosition;
                    gTriplanarOn=true;
                    gTriplanarUvX=transformUv(planarProject(p,0),oscT0,uUvAnimation0,oscRot0,uUvAddress0);
                    gTriplanarUvY=transformUv(planarProject(p,1),oscT0,uUvAnimation0,oscRot0,uUvAddress0);
                    gTriplanarUvZ=transformUv(planarProject(p,2),oscT0,uUvAnimation0,oscRot0,uUvAddress0);
                    vec3 w=pow(max(projAbsN,vec3(1e-4)),vec3(max(uTriplanarSharp[0],1.0)));
                    gTriplanarWeights=w/(w.x+w.y+w.z);
                }
            }
            if(uParticleActive){
                if(uPolarUvOn[0]==1)meshUv0=polarizeUv(meshUv0);
                if(uPolarUvOn[1]==1)meshUv1=polarizeUv(meshUv1);
                if(uPolarUvOn[2]==1)meshUv2=polarizeUv(meshUv2);
            }
            vec2 uv0=transformUv(meshUv0,oscT0,uUvAnimation0,oscRot0,uUvAddress0);
            vec2 uv1=transformUv(meshUv1,oscT1,uUvAnimation1,oscRot1,uUvAddress1);
            vec2 uv2=transformUv(meshUv2,oscT2,uUvAnimation2,oscRot2,uUvAddress2);
            vec2 uv0Pre=uv0;float frame0=0.0,frame0Blend=0.0,frame0Count=1.0;
            if(uParticleActive){
                if(uFlipbookEnabled[0]==1){
                    frame0=flipbookFrame(0);frame0Count=max(uFlipbookGrid[0].x*uFlipbookGrid[0].y,1.0);
                    uv0=flipbookUv(0,uv0Pre,frame0);
                    if(uFlipbookBlendOn[0]==1)frame0Blend=fract(frame0);
                }
                if(uFlipbookEnabled[1]==1)uv1=flipbookUv(1,uv1,flipbookFrame(1));
                if(uFlipbookEnabled[2]==1)uv2=flipbookUv(2,uv2,flipbookFrame(2));
            }
            // Modifier layers: their RG field distorts (or advects) the Primary
            // layer and their alpha erodes it over particle age.
            vec2 advectFlow=vec2(0.0);float advectOn=0.0;float advectPhase=0.0;
            float particleErode=1.0;vec3 particleEdgeGlow=vec3(0.0);
            if(uParticleActive){
                if(uSecondaryLayerActive&&uLayerRole[1]==1){
                    vec4 fieldSample=texture(uSecondaryBase,uv1);
                    vec2 field=fieldSample.rg*2.0-1.0;
                    if(uAdvectSpeed[1]>0.0){advectFlow+=field*uDistortStrength[1];advectOn=1.0;advectPhase=fract(uTime*uAdvectSpeed[1]*.1+uParticleSeed*uRandomPhase);}
                    else uv0+=field*uDistortStrength[1];
                    if(uErodeOn[1]==1){
                        float pattern=fieldSample.a;float soft=max(uErodeSoftness[1],.005);
                        particleErode*=smoothstep(uParticleAge-soft,uParticleAge+soft,pattern);
                        float edge=1.0-clamp(abs(pattern-uParticleAge)/soft,0.0,1.0);
                        particleEdgeGlow+=pow(max(uErodeTint[1],vec3(.001)),vec3(2.2))*uErodeGlow[1]*edge;
                    }
                }
                if(uTertiaryLayerActive&&uLayerRole[2]==1){
                    vec4 fieldSample=texture(uTertiaryBase,uv2);
                    vec2 field=fieldSample.rg*2.0-1.0;
                    if(uAdvectSpeed[2]>0.0){advectFlow+=field*uDistortStrength[2];advectOn=1.0;advectPhase=fract(uTime*uAdvectSpeed[2]*.1+uParticleSeed*uRandomPhase);}
                    else uv0+=field*uDistortStrength[2];
                    if(uErodeOn[2]==1){
                        float pattern=fieldSample.a;float soft=max(uErodeSoftness[2],.005);
                        particleErode*=smoothstep(uParticleAge-soft,uParticleAge+soft,pattern);
                        float edge=1.0-clamp(abs(pattern-uParticleAge)/soft,0.0,1.0);
                        particleEdgeGlow+=pow(max(uErodeTint[2],vec3(.001)),vec3(2.2))*uErodeGlow[2]*edge;
                    }
                }
            }
            vec2 uv=uUvSet==1?uv1:(uUvSet==2?uv2:uv0);
            vec2 heightUv=uHeightUvSet==1?uv1:uv0;
            bool defaults=uCompare&&gl_FragCoord.x>uViewportWidth*.5;
            vec3 geometricN=normalize(gl_FrontFacing?vNormal:-vNormal);
            vec3 T=normalize(vTangent.xyz-geometricN*dot(geometricN,vTangent.xyz));
            vec3 B=normalize(cross(geometricN,T))*vTangent.w;
            mat3 tangentToWorld=mat3(T,B,geometricN),worldToTangent=transpose(tangentToWorld);
            vec3 V=normalize(uCameraPosition-vWorldPosition),L=normalize(uKeyDirection);
            float hitDepth=0.0;
            if(!defaults&&uHasHeight&&uParallaxMode>0){
                vec2 displaced=parallaxUv(heightUv,normalize(worldToTangent*V),hitDepth);
                vec2 delta=displaced-heightUv;
                if(uUvSet==3){uv0+=delta;uv1+=delta;uv2+=delta;uv=uv0;}else uv+=delta;
                if(uParallaxClip&&(any(lessThan(uv,vec2(0.0)))||any(greaterThan(uv,vec2(1.0)))))discard;
            }
            float layerCoverage=0.0;
            if(uSecondaryLayerActive&&!defaults&&(!uParticleActive||uLayerRole[1]==0)){
                float pattern=uPatternSource==1?((uHasLayerCoverage&&!uOverrideLayerCoverage)?texture(uLayerCoverage,uv1).r:uLayerCoverageFallback)
                    :(uPatternSource==2?(uHasSecondaryBase?texture(uSecondaryBase,uv1).a:uSecondaryOpacity):uConstantCoverage);
                if(uPatternSource==1){if(uInvertLayerCoverage)pattern=1.0-pattern;pattern=clamp((pattern-.5)*uLayerCoverageSlotContrast+.5+uLayerCoverageSlotBias,0.0,1.0);}
                pattern=clamp((pattern-.5)*uCoverageContrast+.5+uCoverageBias,0.0,1.0);
                if(uCoverageMode==0)layerCoverage=1.0;
                else if(uCoverageMode==2)layerCoverage=smoothstep(.5-uCoverageSoftness,.5+uCoverageSoftness,pattern);
                else if(uCoverageMode==3&&(uPrimaryHeightInBlue||uSecondaryHeightInBlue)){
                    float under=(uPrimaryHeightInBlue&&uHasNormal)?texture(uNormal,uv0).b:.5;
                    float over=(uSecondaryHeightInBlue&&uHasSecondaryNormal)?texture(uSecondaryNormal,uv1).b:.5;
                    float softness=max(uCoverageSoftness,.001);
                    layerCoverage=smoothstep(-softness,softness,over-under+(pattern-.5)*2.0);
                }
                else layerCoverage=pattern;
                layerCoverage=clamp(layerCoverage*(1.0+uOscCoverageAmp[1]*layerOsc.y),0.0,1.0);
            }
            float tertiaryCoverage=0.0;
            if(uTertiaryLayerActive&&!defaults&&(!uParticleActive||uLayerRole[2]==0)){
                float pattern=uTPatternSource==1?((uHasTertiaryCoverage&&!uOverrideTertiaryCoverage)?texture(uTertiaryCoverage,uv2).r:uTertiaryCoverageFallback)
                    :(uTPatternSource==2?(uHasTertiaryBase?texture(uTertiaryBase,uv2).a:uTertiaryOpacity):uTConstantCoverage);
                if(uTPatternSource==1){if(uInvertTertiaryCoverage)pattern=1.0-pattern;pattern=clamp((pattern-.5)*uTertiaryCoverageSlotContrast+.5+uTertiaryCoverageSlotBias,0.0,1.0);}
                pattern=clamp((pattern-.5)*uTCoverageContrast+.5+uTCoverageBias,0.0,1.0);
                if(uTCoverageMode==0)tertiaryCoverage=1.0;
                else if(uTCoverageMode==2)tertiaryCoverage=smoothstep(.5-uTCoverageSoftness,.5+uTCoverageSoftness,pattern);
                else if(uTCoverageMode==3&&(uPrimaryHeightInBlue||uSecondaryHeightInBlue||uTertiaryHeightInBlue)){
                    float primaryHeight=(uPrimaryHeightInBlue&&uHasNormal)?texture(uNormal,uv0).b:.5;
                    float secondaryHeight=(uSecondaryHeightInBlue&&uHasSecondaryNormal)?texture(uSecondaryNormal,uv1).b:.5;
                    float under=mix(primaryHeight,secondaryHeight,layerCoverage);
                    float over=(uTertiaryHeightInBlue&&uHasTertiaryNormal)?texture(uTertiaryNormal,uv2).b:.5;
                    float softness=max(uTCoverageSoftness,.001);
                    tertiaryCoverage=smoothstep(-softness,softness,over-under+(pattern-.5)*2.0);
                }
                else tertiaryCoverage=pattern;
                tertiaryCoverage=clamp(tertiaryCoverage*(1.0+uOscCoverageAmp[2]*layerOsc.z),0.0,1.0);
            }
            vec3 N=geometricN;
            if((uHasNormal||uOverrideNormal)&&!defaults){vec3 encoded=uOverrideNormal?uNormalFallback:layer0Sample(uNormal,uv).xyz;vec3 map=encoded*2.0-1.0;map.xy*=uNormalStrength*uGlobalNormalStrength*max(0.0,1.0+uOscNormalAmp[0]*layerOsc.x);map.z=sqrt(max(.001,1.0-dot(map.xy,map.xy)));N=uNormalSpace==0?normalize(tangentToWorld*map):normalize(map);}
            if(uSecondaryLayerActive&&layerCoverage>0.0&&(uHasSecondaryNormal||uOverrideSecondaryNormal)&&!defaults){
                vec3 encoded=uOverrideSecondaryNormal?uSecondaryNormalFallback:texture(uSecondaryNormal,uv1).xyz;
                vec3 map=encoded*2.0-1.0;map.xy*=uSecondaryNormalStrength*uGlobalNormalStrength*max(0.0,1.0+uOscNormalAmp[1]*layerOsc.y);map.z=sqrt(max(.001,1.0-dot(map.xy,map.xy)));
                vec3 secondaryN=uNormalSpace==0?normalize(tangentToWorld*map):normalize(map);N=normalize(mix(N,secondaryN,layerCoverage));
            }
            if(uTertiaryLayerActive&&tertiaryCoverage>0.0&&(uHasTertiaryNormal||uOverrideTertiaryNormal)&&!defaults){
                vec3 encoded=uOverrideTertiaryNormal?uTertiaryNormalFallback:texture(uTertiaryNormal,uv2).xyz;
                vec3 map=encoded*2.0-1.0;map.xy*=uTertiaryNormalStrength*uGlobalNormalStrength*max(0.0,1.0+uOscNormalAmp[2]*layerOsc.z);map.z=sqrt(max(.001,1.0-dot(map.xy,map.xy)));
                vec3 tertiaryN=uNormalSpace==0?normalize(tangentToWorld*map):normalize(map);N=normalize(mix(N,tertiaryN,tertiaryCoverage));
            }
            if(hasSpecial(2)&&!defaults){
                vec3 detail=specialSample(2,uv).xyz*2.0-1.0;detail.z=sqrt(max(.001,1.0-dot(detail.xy,detail.xy)));
                float driver=uDynamicDriverSource==2?vColor.r:(uDynamicDriverSource==3?uDynamicDriverValue:(.5+.5*sin(uTime*(uDynamicDriverSource==0?.73:1.17))));
                driver=clamp((driver-uDynamicInputMin)/max(uDynamicInputMax-uDynamicInputMin,.0001),0.0,1.0);if(uDynamicInvert)driver=1.0-driver;
                vec3 currentTs=normalize(worldToTangent*N),combinedTs=blendNormal(currentTs,detail,clamp(driver*uDynamicStrength,0.0,1.0),uDynamicBlend);N=normalize(tangentToWorld*combinedTs);
            }
            if((uShaderMode==4||(uShaderMode==8&&hasSpecial(8)))&&hasSpecial(8)&&!defaults){
                vec4 flow=specialSample(8,uv);vec2 direction=(flow.rg*2.0-1.0)*uWaterFlowStrength;
                float wave=sin(dot(uv+direction*uTime*uWaterFlowSpeed,vec2(37.0,53.0))+uTime*(2.0+flow.b*8.0));
                vec3 waveTs=normalize(vec3(direction*.22+vec2(wave,-wave)*.08,1.0));N=normalize(mix(N,tangentToWorld*waveTs,clamp(uWaterFlowStrength*.35,0.0,1.0)));
            }
            vec4 irisMasks=vec4(0.0);
            if((uShaderMode==6||(uShaderMode==8&&hasSpecial(11)))&&hasSpecial(11)&&!defaults){
                irisMasks=specialSample(11,uv);vec2 centered=uv-vec2(.5);float viewAngle=1.0-abs(dot(N,V));
                uv+=centered*(irisMasks.r-.5)*uEyeIrisDepth*(.5+viewAngle);
            }
            vec4 baseSample;
            if(uParticleActive&&uHasBase&&advectOn>0.5&&uUvSet!=1&&uUvSet!=2){
                float phase1=fract(advectPhase+.5);
                baseSample=mix(texture(uBase,uv-advectFlow*advectPhase),texture(uBase,uv-advectFlow*phase1),abs(2.0*advectPhase-1.0));
            }else if(uParticleActive&&uHasBase&&frame0Blend>0.0&&uUvSet!=1&&uUvSet!=2){
                float nextFrame=uFlipbookLoop[0]==1?min(floor(frame0)+1.0,frame0Count-1.0):mod(floor(frame0)+1.0,frame0Count);
                vec2 uvNext=flipbookUv(0,uv0Pre,nextFrame);
                if(hasSpecial(uMotionVectorsLayer)&&uMotionStrength>0.0){
                    // Motion-vector warped cross-fade: frames advect toward each
                    // other along the authored per-frame motion field, hiding
                    // low flipbook framerates.
                    vec2 cellScale=1.0/max(uFlipbookGrid[0],vec2(1.0));
                    vec2 motionA=(specialSample(uMotionVectorsLayer,uv).rg*2.0-1.0)*uMotionStrength*cellScale;
                    vec2 motionB=(specialSample(uMotionVectorsLayer,uvNext).rg*2.0-1.0)*uMotionStrength*cellScale;
                    baseSample=mix(texture(uBase,uv+motionA*frame0Blend),texture(uBase,uvNext-motionB*(1.0-frame0Blend)),frame0Blend);
                }else baseSample=mix(texture(uBase,uv),texture(uBase,uvNext),frame0Blend);
            }else baseSample=uHasBase?layer0Sample(uBase,uv):vec4(1.0);
            vec3 base=defaults?vec3(.72):((uHasBase&&!uOverrideBaseColor)?pow(baseSample.rgb,vec3(2.2)):pow(uBaseFallback,vec3(2.2)));
            if(!defaults)base*=pow(uTint,vec3(2.2))*uColorScale;
            float baseAlpha=defaults?1.0:((uHasBase&&!uOverrideOpacity)?baseSample.a:uOpacity);
            baseAlpha*=clamp(1.0+uOscCoverageAmp[0]*layerOsc.x,0.0,1.0);
            float gradientDriver=uGradientDriver==2?vColor.a:(uGradientDriver==3?uGradientConstant
                :(uGradientDriver==0?(uParticleActive?uParticleAge:fract(uTime*.18)):fract(uTime*.1)));
            vec4 gradientSample=texture(uGradient,vec2(gradientDriver,.5));
            vec3 gradientColor=(uHasGradient&&!uOverrideGradientColor)?pow(gradientSample.rgb,vec3(2.2)):pow(uGradientColor,vec3(2.2));
            float gradientAlpha=(uHasGradient&&!uOverrideGradientOpacity)?gradientSample.a:uGradientOpacity;
            if(uShaderMode>=9&&!defaults)base*=gradientColor*uGradientIntensity;
            if(uParticleActive&&uPaletteMode>0&&!defaults){
                float luminance=clamp(dot(base,vec3(.299,.587,.114)),0.0,1.0);
                vec3 ramp;
                if(uPaletteMode==1)ramp=pow(max(texture(uGradient,vec2(luminance,.5)).rgb,vec3(.001)),vec3(2.2));
                else ramp=vec3(pow(luminance,1.1),pow(luminance,2.4)*.85,pow(luminance,5.5)*.72);
                base=ramp*uPaletteIntensity;
            }
            if(uParticleActive&&uRandomHue>0.0&&!defaults){
                float hueAngle=(uParticleSeed-.5)*6.2831853*uRandomHue;
                float hc=cos(hueAngle),hs=sin(hueAngle);
                vec3 rotated;
                rotated.r=dot(base,vec3(.299+.701*hc+.168*hs,.587-.587*hc+.330*hs,.114-.114*hc-.497*hs));
                rotated.g=dot(base,vec3(.299-.299*hc-.328*hs,.587+.413*hc+.035*hs,.114-.114*hc+.292*hs));
                rotated.b=dot(base,vec3(.299-.300*hc+1.25*hs,.587-.588*hc-1.05*hs,.114+.886*hc-.203*hs));
                base=max(rotated,vec3(0.0));
            }
            float sourceAlpha=uAlphaSource==1?gradientAlpha:(uAlphaSource==2?baseAlpha*gradientAlpha:(uAlphaSource==3?1.0:baseAlpha));
            float opacity=sourceAlpha*uGlobalOpacity;
            if(uSecondaryLayerActive&&layerCoverage>0.0&&!defaults){
                vec4 secondarySample=uHasSecondaryBase?texture(uSecondaryBase,uv1):vec4(1.0);
                vec3 secondaryBase=(uHasSecondaryBase&&!uOverrideSecondaryBaseColor)?pow(secondarySample.rgb,vec3(2.2)):pow(uSecondaryBaseFallback,vec3(2.2));
                secondaryBase*=pow(uSecondaryTint,vec3(2.2))*uSecondaryColorScale;
                if(hasSpecial(16)){
                    float driver=uSecondaryGradientDriver==1?vColor.r:(uSecondaryGradientDriver==2?uSecondaryGradientConstant:layerCoverage);vec4 gradient=specialSample(16,vec2(clamp(driver,0.0,1.0),.5));
                    secondaryBase*=pow(max(gradient.rgb,vec3(.001)),vec3(2.2))*uSecondaryGradientIntensity;layerCoverage*=gradient.a*uSecondaryGradientIntensity;
                }
                base=blendLayer(base,secondaryBase,layerCoverage,uSecondaryBlendMode);
                if(uAlphaSource==0||uAlphaSource==2){
                    float secondaryAlpha=(uHasSecondaryBase&&!uOverrideSecondaryOpacity)?secondarySample.a:uSecondaryOpacity;
                    if(uAlphaSource==2)secondaryAlpha*=gradientAlpha;
                    opacity=mix(opacity,secondaryAlpha*uGlobalOpacity,layerCoverage);
                }
            }
            if(uTertiaryLayerActive&&tertiaryCoverage>0.0&&!defaults){
                vec4 tertiarySample=uHasTertiaryBase?texture(uTertiaryBase,uv2):vec4(1.0);
                vec3 tertiaryBase=(uHasTertiaryBase&&!uOverrideTertiaryBaseColor)?pow(tertiarySample.rgb,vec3(2.2)):pow(uTertiaryBaseFallback,vec3(2.2));
                tertiaryBase*=pow(uTertiaryTint,vec3(2.2))*uTertiaryColorScale;
                base=blendLayer(base,tertiaryBase,tertiaryCoverage,uTertiaryBlendMode);
                if(uAlphaSource==0||uAlphaSource==2){
                    float tertiaryAlpha=(uHasTertiaryBase&&!uOverrideTertiaryOpacity)?tertiarySample.a:uTertiaryOpacity;
                    if(uAlphaSource==2)tertiaryAlpha*=gradientAlpha;
                    opacity=mix(opacity,tertiaryAlpha*uGlobalOpacity,tertiaryCoverage);
                }
            }
            if(hasSpecial(0)&&!defaults){
                vec4 masks=specialSample(0,uv);float total=dot(masks,vec4(1.0));
                if(uTintOverlap==0&&total>1.0)masks/=total;
                else if(uTintOverlap==2){masks=vec4(masks.r,masks.g*(1.0-masks.r),masks.b*(1.0-masks.r)*(1.0-masks.g),masks.a*(1.0-masks.r)*(1.0-masks.g)*(1.0-masks.b));}
                vec3 tintMix=max(vec3(0.0),vec3(1.0-dot(masks,vec4(1.0)))+uTintColorR*masks.r+uTintColorG*masks.g+uTintColorB*masks.b+uTintColorA*masks.a);
                base*=tintMix;
            }
            vec4 landscapeMacro=vec4(.5);
            if(uShaderMode==7&&hasSpecial(15)&&!defaults){
                float macroFrequency=max(1.0,2048.0/max(uLandscapeMacroScale,.001));landscapeMacro=specialSample(15,fract(uv*macroFrequency));
                vec3 macroColor=pow(max(landscapeMacro.rgb,vec3(.001)),vec3(2.2))*2.0;base*=mix(vec3(1.0),macroColor,clamp(uLandscapeMacroColorStrength,0.0,4.0));
            }
            if((uShaderMode==6||(uShaderMode==8&&hasSpecial(11)))&&!defaults){
                base*=mix(vec3(1.0),vec3(.06,.025,.015),clamp(irisMasks.g*uEyeLimbalStrength,0.0,1.0));
                float pupil=clamp(irisMasks.b*uEyePupilScale,0.0,1.0);base=mix(base,vec3(.003),pupil);
            }
            if(uParticleActive&&!defaults){
                opacity*=particleErode;
                if(uParticleFresnelFade>0.0){
                    vec3 fadeView=normalize(uCameraPosition-vWorldPosition);
                    float facing=abs(dot(normalize(vNormal),fadeView));
                    opacity*=mix(1.0,pow(clamp(facing,0.0,1.0),uParticleFresnelPower),uParticleFresnelFade);
                }
                if(uParticleNearFade>0.0)
                    opacity*=smoothstep(uParticleNearFade*.5,uParticleNearFade,length(uCameraPosition-vWorldPosition));
            }
            if(uCompositionMode==0)opacity=1.0;
            if(uCompositionMode==1){if(opacity<uAlphaThreshold)discard;opacity=1.0;}
            if(uDecalEnabled&&!defaults){
                float angle=abs(dot(geometricN,V));float projection=smoothstep(uDecalAngleFade,1.0,angle);
                if(uDecalDistanceFade>0.0)projection*=1.0-smoothstep(uDecalDistanceFade*.75,uDecalDistanceFade,length(uCameraPosition-vWorldPosition));
                opacity*=projection;if(!uDecalBaseColor)base=vec3(.5);if(!uDecalNormal)N=geometricN;if(opacity<.001)discard;
            }
            bool unlitMaterial=uShaderMode==9&&!defaults;
            vec4 channelData=layer0Sample(uRmaos,uv);
            float rough=defaults?.5:clamp(uUseRoughness?channelData.r:uRoughness,.035,1.0);
            float metal=defaults?0:clamp(uUseMetallic?channelData.g:uMetallic,0.0,1.0);
            float ao=defaults?1:clamp(uUseAo?channelData.b:uAo,0.0,1.0);
            float dielectric=defaults?.04:clamp(uUseF0?channelData.a:uF0,0.0,1.0);
            if(uDecalEnabled&&!defaults){if(!uDecalRoughness)rough=.5;if(!uDecalMetallic)metal=0.0;if(!uDecalAo)ao=1.0;}
            if(uShaderMode==7&&hasSpecial(15)&&!defaults)rough=clamp(rough+(landscapeMacro.a-.5)*2.0*uLandscapeMacroRoughnessStrength,.035,1.0);
            if(uSecondaryLayerActive&&layerCoverage>0.0&&!defaults){
                vec4 secondaryChannels=texture(uSecondaryRmaos,uv1);
                float sr=clamp(uUseSecondaryRoughness?secondaryChannels.r:uSecondaryRoughness,.035,1.0);
                float sm=clamp(uUseSecondaryMetallic?secondaryChannels.g:uSecondaryMetallic,0.0,1.0);
                float sa=clamp(uUseSecondaryAo?secondaryChannels.b:uSecondaryAo,0.0,1.0);
                float sf=clamp(uUseSecondaryF0?secondaryChannels.a:uSecondaryF0,0.0,1.0);
                rough=mix(rough,sr,layerCoverage);metal=mix(metal,sm,layerCoverage);ao=mix(ao,sa,layerCoverage);dielectric=mix(dielectric,sf,layerCoverage);
            }
            if(uTertiaryLayerActive&&tertiaryCoverage>0.0&&!defaults){
                vec4 tertiaryChannels=texture(uTertiaryRmaos,uv2);
                float tr=clamp(uUseTertiaryRoughness?tertiaryChannels.r:uTertiaryRoughness,.035,1.0);
                float tm=clamp(uUseTertiaryMetallic?tertiaryChannels.g:uTertiaryMetallic,0.0,1.0);
                float ta=clamp(uUseTertiaryAo?tertiaryChannels.b:uTertiaryAo,0.0,1.0);
                float tf=clamp(uUseTertiaryF0?tertiaryChannels.a:uTertiaryF0,0.0,1.0);
                rough=mix(rough,tr,tertiaryCoverage);metal=mix(metal,tm,tertiaryCoverage);ao=mix(ao,ta,tertiaryCoverage);dielectric=mix(dielectric,tf,tertiaryCoverage);
            }
            if(hasSpecial(uDetailNormalLayer)&&!defaults&&uDetailNormalStrength>0.0){
                // Tiling micro-detail normal: cheap close-range surface texture
                // (fabric weave, pores, machining) blended whiteout-style.
                vec4 detail=specialSample(uDetailNormalLayer,uv*uDetailNormalScale);
                float detailMask=detail.a;
                vec2 detailXy=(detail.rg*2.0-1.0)*uDetailNormalStrength*detailMask;
                N=normalize(N+tangentToWorld*vec3(detailXy,0.0));
                // B carries micro roughness breakup at the same detail scale.
                rough=clamp(rough+(detail.b-.5)*2.0*uDetailRoughVariation*detailMask,0.0,1.0);
            }
            // Signed curvature: authored in the primary normal alpha (0.5
            // neutral, <0.5 convex edge, >0.5 concave cavity) or derived
            // concave-only from AO. The derived half stays at or above the
            // signed neutral so open surfaces never register as shedding
            // edges, and it feeds STATES ONLY — every lighting-side consumer
            // requires the authored map.
            bool curvatureAuthored=uCurvatureInAlpha&&uHasNormal&&!defaults;
            float curvature=curvatureAuthored?layer0Sample(uNormal,uv).a:(.5+(1.0-ao)*.5);
            vec3 stateHeatGlow=vec3(0.0);
            float stateSparkle=0.0;
            bool anyState=false;
            for(int i=0;i<8;++i)if(uStateAmount[i]>0.0)anyState=true;
            if(anyState&&!defaults){
                // v5 response resolution — physical inputs only, no response
                // map: porosity from RMAOS alpha / constant / the standard
                // saturate(rough * (1 - metal)) heuristic (landscape keeps its
                // array-alpha porosity), cavity from signed curvature, adhesion
                // from the material constant, variation from world noise.
                float porosity=uPorosityOverride?uPorosityConstant
                    :(uShaderMode==7?(uLandscapeUsePorosityTexture?texture(uNormal,uv).a:uLandscapePorosity)
                    :(uRmaosAlphaPorosity?channelData.a:clamp(rough*(1.0-metal),0.0,1.0)));
                float adhesion=uStateAdhesion,cavity=curvature;
                float variation=clamp(vnoise3(vWorldPosition*9.0)*.7+.15,0.0,1.0);
                float upness=clamp(N.y*.5+.5,0.0,1.0);
                // Rain only lands on genuinely up-facing surfaces; undersides
                // never ripple.
                float rainUp=smoothstep(.05,.35,N.y);
                float verticality=1.0-abs(N.y);
                float streakNoise=vnoise3(vWorldPosition*vec3(22.0,2.5,22.0));
                float rippleCover=0.0;
                vec2 dripGradient=vec2(0.0);
                float dripField=stateDripField(vWorldPosition,N,uTime,dripGradient);
                float dripNormalCover=0.0;
                // Map-guided accumulation: free signals from channels already
                // sampled. Snapshots keep earlier states in the loop from
                // feeding their own roughness/metal edits into later biases.
                float preStateRough=rough,preStateMetal=metal;
                float stateHeight=uHeightFallback;
                if(uHasHeight&&!uOverrideHeight){vec4 stateHeightSample=texture(uHeight,heightUv);stateHeight=uHeightBlue?stateHeightSample.b:stateHeightSample.r;}
                if(uHeightInvert)stateHeight=1.0-stateHeight;
                float glowFactor=clamp(((uHasEmissive&&!uOverrideEmissiveMask)?texture(uEmissive,uv).a:uEmissiveMask)*min(uEmissiveIntensity,4.0)*.5,0.0,1.0);
                for(int i=0;i<8;++i){
                    float amount=uStateAmount[i];
                    if(amount<=0.0)continue;
                    // Glowing-hot areas suppress the state (rain steams off).
                    amount*=1.0-glowFactor*uStateEmissiveDry[i];
                    if(amount<=0.0)continue;
                    // Metallic seals porous absorption for this state.
                    float statePorosity=porosity*(1.0-preStateMetal*uStateMetalSeal[i]);
                    // Dissolve-style coverage: variation orders reveal, cavity
                    // pulls collection in first, adhesion caps it all. A
                    // dedicated mask channel replaces the shared accumulation.
                    float cover;
                    if(uStateMaskChannel[i]>=0&&hasSpecial(uStateMasksLayer)){
                        vec4 masks=specialSample(uStateMasksLayer,uv);
                        float m=uStateMaskChannel[i]==0?masks.r:(uStateMaskChannel[i]==1?masks.g:(uStateMaskChannel[i]==2?masks.b:masks.a));
                        cover=m*adhesion*clamp(amount*2.0,0.0,1.0);
                    }else{
                        float a=amount-uStateThreshold[i];
                        // Retention: porous areas collect first and dry last;
                        // AO, low height, and roughness add free map guidance.
                        float guidance=cavity*uStateCavityBias[i]+statePorosity*uStateRetention[i]
                            +(1.0-ao)*uStateAoBias[i]+(1.0-stateHeight)*uStateHeightBias[i]+(preStateRough-.5)*uStateRoughBias[i];
                        cover=smoothstep(variation-uStateSoftness[i],variation+uStateSoftness[i],a+guidance)*adhesion*clamp(a*4.0,0.0,1.0);
                    }
                    cover*=mix(1.0,upness*upness,uStateDirectionality[i]);
                    cover*=mix(1.0,clamp(streakNoise*1.7,0.0,1.0),uStateStreaking[i]*verticality);
                    // Signed-curvature edge shedding: response B below 0.5 marks
                    // exposed edges that shed accumulation (worn corners stay clean).
                    cover*=1.0-uStateEdgeShedding[i]*max(0.0,1.0-cavity*2.0);
                    if(uStateRespectsShelter[i]==1&&uShelterChannel>=0&&uMeshHasVertexColors){
                        float shelter=uShelterChannel==0?vColor.r:(uShelterChannel==1?vColor.g:(uShelterChannel==2?vColor.b:vColor.a));
                        cover*=1.0-shelter;
                    }
                    // Drips wet their trails even where base accumulation is low.
                    if(uStateRipples[i]==1&&dripField>0.0){
                        cover=max(cover,dripField*clamp(amount*1.5,0.0,1.0)*adhesion);
                        dripNormalCover=max(dripNormalCover,clamp(amount*1.5,0.0,1.0));
                    }
                    cover=clamp(cover,0.0,1.0);
                    if(cover<=0.0)continue;
                    float porosityMix=uStatePorosityInfluence[i];
                    if(uStateBlendMode[i]==0){
                        // Darken: multiplies toward the raw (non-gamma) color.
                        base*=mix(vec3(1.0),max(uStateColor[i],vec3(0.0)),cover*mix(1.0,statePorosity,porosityMix));
                    }else if(uStateBlendMode[i]==1){
                        vec3 tint=pow(max(uStateColor[i],vec3(.001)),vec3(2.2));
                        vec3 grayBase=vec3(dot(base,vec3(.299,.587,.114)));
                        base=mix(base,mix(base,grayBase,uStateDesat[i])*tint/max(dot(tint,vec3(.333)),.05)*.35+tint*.65,cover);
                    }else{
                        vec3 tint=pow(max(uStateColor[i],vec3(.001)),vec3(2.2));
                        base=mix(base,tint*mix(1.0,.55,statePorosity*porosityMix),cover);
                    }
                    float roughWeight=uStateRoughWeight[i]*cover*mix(1.0,1.0-statePorosity*.6,porosityMix);
                    if(uStateRoughMode[i]==1)rough=mix(rough,clamp(rough+uStateRoughTarget[i],0.0,1.0),roughWeight);
                    else rough=mix(rough,uStateRoughTarget[i],roughWeight);
                    dielectric=mix(dielectric,uStateF0Target[i],uStateF0Weight[i]*cover);
                    dielectric=max(dielectric,uStateFilmF0[i]*cover*(1.0-statePorosity*porosityMix));
                    if(uStateKillMetallic[i]==1)metal=mix(metal,0.0,cover);
                    stateSparkle+=cover*uStateSparkle[i];
                    if(uStateGlowIntensity[i]>0.0){
                        // Blackbody-style ramp: dull red through orange to white.
                        vec3 glow=vec3(pow(amount,1.1),pow(amount,2.4)*.85,pow(amount,5.5)*.72);
                        stateHeatGlow+=glow*cover*uStateGlowIntensity[i];
                    }
                    if(uStateRipples[i]==1)rippleCover=max(rippleCover,cover*rainUp);
                }
                if(rippleCover>0.0){
                    // Subtract the height gradient so ring crests read as raised.
                    vec2 rippleGradient=stateRippleGradient(vWorldPosition.xz,uTime)*rippleCover*.35;
                    N=normalize(N-vec3(rippleGradient.x,0.0,rippleGradient.y));
                }
                if(dripNormalCover>0.0){
                    vec3 horizontalDir=abs(N.x)>abs(N.z)?vec3(0.0,0.0,1.0):vec3(1.0,0.0,0.0);
                    N=normalize(N+(horizontalDir*dripGradient.x+vec3(0.0,1.0,0.0)*dripGradient.y)*dripNormalCover*.4);
                }
            }
            vec4 emissionSample=layer0Sample(uEmissive,uv);
            vec3 emissionColor=(uHasEmissive&&!uOverrideEmissiveColor)?pow(emissionSample.rgb,vec3(2.2)):pow(uEmissiveColor,vec3(2.2));
            float emissionMask=(uHasEmissive&&!uOverrideEmissiveMask)?emissionSample.a:uEmissiveMask;
            vec3 emission=defaults?vec3(0):emissionColor*emissionMask*uEmissiveIntensity*max(0.0,1.0+uOscEmissiveAmp[0]*layerOsc.x);
            if(uDecalEnabled&&!uDecalEmissive)emission=vec3(0);
            if(uSecondaryLayerActive&&layerCoverage>0.0&&!defaults){
                vec4 secondaryEmissionSample=texture(uSecondaryEmissive,uv1);
                vec3 secondaryEmissionColor=(uHasSecondaryEmissive&&!uOverrideSecondaryEmissiveColor)?pow(secondaryEmissionSample.rgb,vec3(2.2)):pow(uSecondaryEmissiveColor,vec3(2.2));
                float secondaryMask=(uHasSecondaryEmissive&&!uOverrideSecondaryEmissiveMask)?secondaryEmissionSample.a:uSecondaryEmissiveMask;
                emission=mix(emission,secondaryEmissionColor*secondaryMask*uSecondaryEmissiveIntensity*max(0.0,1.0+uOscEmissiveAmp[1]*layerOsc.y),layerCoverage);
            }
            if(uTertiaryLayerActive&&tertiaryCoverage>0.0&&!defaults){
                vec4 tertiaryEmissionSample=texture(uTertiaryEmissive,uv2);
                vec3 tertiaryEmissionColor=(uHasTertiaryEmissive&&!uOverrideTertiaryEmissiveColor)?pow(tertiaryEmissionSample.rgb,vec3(2.2)):pow(uTertiaryEmissiveColorFallback,vec3(2.2));
                float tertiaryMask=(uHasTertiaryEmissive&&!uOverrideTertiaryEmissiveMask)?tertiaryEmissionSample.a:uTertiaryEmissiveMask;
                emission=mix(emission,tertiaryEmissionColor*tertiaryMask*uTertiaryEmissiveIntensity*max(0.0,1.0+uOscEmissiveAmp[2]*layerOsc.z),tertiaryCoverage);
            }
            emission+=stateHeatGlow+particleEdgeGlow;
            vec3 resolvedTangentNormal=normalize(worldToTangent*N);
            float resolvedHeight=defaults?.5:uHeightFallback;
            if(!defaults&&uHasHeight&&!uOverrideHeight){vec4 heightSample=texture(uHeight,heightUv);resolvedHeight=uHeightBlue?heightSample.b:heightSample.r;}
            if(!defaults&&uHeightInvert)resolvedHeight=1.0-resolvedHeight;resolvedHeight=clamp(resolvedHeight+uHeightBias,0.0,1.0);
            if(uDebugView==1){fragColor=vec4(pow(base,vec3(1.0/2.2)),opacity);return;}
            if(uDebugView==2){fragColor=vec4(N*.5+.5,1);return;}
            if(uDebugView==3){fragColor=vec4(vec3(rough),1);return;}
            if(uDebugView==4){fragColor=vec4(vec3(metal),1);return;}
            if(uDebugView==5){fragColor=vec4(vec3(ao),1);return;}
            if(uDebugView==6){fragColor=vec4(vec3(dielectric),1);return;}
            if(uDebugView==7){fragColor=vec4(tonemap(emission),1);return;}
            if(uDebugView==8){vec2 q=floor(uv*16.0);float c=mod(q.x+q.y,2.0);fragColor=vec4(mix(vec3(.04,.12,.7),vec3(.95,.95,.95),c),1);return;}
            if(uDebugView==10){fragColor=vec4(T*.5+.5,1);return;}
            if(uDebugView==11){fragColor=vec4(vec3(opacity),1);return;}
            if(uDebugView==12){fragColor=vec4(resolvedTangentNormal*.5+.5,1);return;}
            if(uDebugView==13){fragColor=vec4(vec3(resolvedHeight),1);return;}
            if(uDebugView==14){fragColor=vec4(rough,metal,0.0,1);return;}
            if(uDebugView==15){fragColor=vec4(rough,metal,ao,1);return;}
            if(uDebugView==16){fragColor=vec4(vec3(emissionMask),1);return;}
            if(uDebugView==17){fragColor=vec4(vec3(layerCoverage),1);return;}
            if(uDebugView==18){fragColor=vec4(pow(max(emissionColor,vec3(0.0)),vec3(1.0/2.2)),1);return;}
            if(unlitMaterial){vec3 unlit=base*uUnlitIntensity+particleEdgeGlow;if(uUnlitVertexColor)unlit*=vColor.rgb;fragColor=vec4(uUnlitExposure?tonemap(unlit):pow(max(unlit,vec3(0.0)),vec3(1.0/2.2)),opacity);return;}
            if(uSpecularAa&&!defaults){
                // Tokuyoshi/Kaplanyan screen-space variance: widen the lobe where
                // the shaded normal changes quickly so detail stops sparkling.
                vec3 dndx=dFdx(N),dndy=dFdy(N);
                float kernelRoughness=min(2.0*uSpecularAaStrength*(dot(dndx,dndx)+dot(dndy,dndy)),.18);
                rough=clamp(sqrt(rough*rough+kernelRoughness),.035,1.0);
            }
            vec3 H=normalize(V+L);
            float NoL=max(dot(N,L),0.0),NoV=max(dot(N,V),.001),NoH=max(dot(N,H),0.0),VoH=max(dot(V,H),0.0);
            vec3 f0=mix(vec3(dielectric),base,metal),F=fresnel(VoH,f0);
            float alpha=rough*rough,k=(rough+1.0)*(rough+1.0)/8.0;
            vec2 brdf=texture(uBrdfLut,vec2(NoV,rough)).rg;
            // Kulla-Conty multi-scatter energy compensation: single-scatter GGX
            // loses energy at high roughness, rendering rough metals too dark.
            float energySS=clamp(brdf.x+brdf.y,.001,1.0);
            vec3 fAvg=f0+(vec3(1.0)-f0)/21.0;
            vec3 multiScatter=vec3(1.0)+fAvg*(1.0-energySS)/energySS;
            vec3 spec=D_GGX(NoH,alpha)*G1(NoL,k)*G1(NoV,k)*F/max(4.0*NoL*NoV,.001)*multiScatter;
            vec3 anisoBentN=N;
            if(hasSpecial(uAnisotropyLayer)&&!defaults){
                vec4 aniso=specialSample(uAnisotropyLayer,uv);
                float anisoAmount=clamp(aniso.b*uAnisotropyIntensity,0.0,1.0)*aniso.a;
                if(anisoAmount>.001){
                    vec2 dir2=aniso.rg*2.0-1.0;if(dot(dir2,dir2)<1e-6)dir2=vec2(1.0,0.0);dir2=normalize(dir2);
                    float rotA=radians(uAnisotropyRotation);dir2=mat2(cos(rotA),-sin(rotA),sin(rotA),cos(rotA))*dir2;
                    vec3 aT=normalize(tangentToWorld*vec3(dir2,0.0));vec3 aB=normalize(cross(N,aT));aT=cross(aB,N);
                    float at=max(alpha*(1.0+.9*anisoAmount),.0005),ab=max(alpha*(1.0-.9*anisoAmount),.0005);
                    float ToH=dot(aT,H),BoH=dot(aB,H);
                    float dTerm=ToH*ToH/(at*at)+BoH*BoH/(ab*ab)+NoH*NoH;
                    float dAniso=1.0/(PI*at*ab*max(dTerm*dTerm,.0001));
                    spec=dAniso*G1(NoL,k)*G1(NoV,k)*F/max(4.0*NoL*NoV,.001)*multiScatter;
                    // Approximate anisotropic IBL by bending the reflection normal
                    // along the grain (UE-style bent normal).
                    vec3 grainN=normalize(cross(aT,cross(aT,V)));
                    anisoBentN=normalize(mix(N,-grainN,anisoAmount*.5));
                }
            }
            if(curvatureAuthored&&uCavitySpecOcclusion>0.0){
                // Cavity specular occlusion: direct-light highlights break and
                // die inside seams and pores — meso-features far below
                // shadow-map resolution. Authored curvature only: the
                // AO-derived fallback would sneak AO onto direct light.
                spec*=1.0-clamp((curvature-.5)*2.0,0.0,1.0)*uCavitySpecOcclusion;
            }
            vec3 diffuse=(1.0-F)*(1.0-metal)*base/PI;
            float heightVisibility=(uParallaxMode==3&&!defaults)?parallaxShadow(uv,hitDepth,normalize(worldToTangent*L)):1.0;
            float keyShadow=shadowVisibility(vWorldPosition,N);
            vec3 color=(uShaderMode==10
                ? (base*(.15+.85*NoL)+vec3(dielectric)*pow(NoH,mix(8.0,96.0,1.0-rough))*NoL)*uKeyColor*uKeyIntensity
                : (diffuse+spec)*uKeyColor*uKeyIntensity*NoL*heightVisibility)*keyShadow;
            vec3 R=reflect(-V,anisoBentN);
            vec3 environmentR=environmentDirection(R);
            float environmentLod=rough*uPrefilteredEnvironmentMaxLod;
            vec3 filteredSpecular=textureLod(uPrefilteredEnvironment,environmentR,environmentLod).rgb;
            vec3 sharpSpecular=textureLod(uEnvironment,environmentR,0.0).rgb;
            vec3 envSpec=mix(sharpSpecular,filteredSpecular,smoothstep(0.0,.08,rough));
            vec3 diffuseN=N;float specularOcclusion=1.0;
            if(hasSpecial(uBentNormalLayer)&&!defaults){
                vec4 bent=specialSample(uBentNormalLayer,uv);
                vec3 bentTs=vec3(bent.rg*2.0-1.0,0.0);bentTs.z=sqrt(max(.001,1.0-dot(bentTs.xy,bentTs.xy)));
                vec3 bentN=normalize(mix(N,normalize(tangentToWorld*bentTs),clamp(uBentNormalStrength,0.0,1.0)*bent.a));
                diffuseN=bentN;
                // Cone-style specular occlusion from bent-normal visibility + AO,
                // further scaled by the baked occlusion-cone channel (B; 1 = open).
                float bentVisibility=clamp(dot(bentN,R)*.5+.5,0.0,1.0);
                specularOcclusion=clamp(pow(bentVisibility+ao,exp2(-16.0*rough-1.0))-1.0+ao,0.0,1.0)*bent.b;
            }
            vec3 envDiffuse=diffuseIrradiance(diffuseN);
            vec3 environmentF=fresnel(NoV,f0);
            vec3 environmentDiffuse=(1.0-environmentF)*(1.0-metal)*base*envDiffuse*.45*uDiffuseScale;
            vec3 environmentSpecular=envSpec*(f0*brdf.x+brdf.y)*multiScatter*specularOcclusion*uSpecularScale;
            vec3 environmentLighting=(environmentDiffuse+environmentSpecular)*uEnvironmentIntensity*ao;
            if(uShaderMode!=10)color+=environmentLighting;
            if(uParticleActive&&uShaderMode>=10&&hasSpecial(uSixWayALayer)&&hasSpecial(uSixWayBLayer)&&!defaults){
                // Six-way lightmap smoke: per-frame lighting baked from six axis
                // directions, relit by projecting the scene light onto that basis.
                vec4 sixA=specialSample(uSixWayALayer,uv);
                vec4 sixB=specialSample(uSixWayBLayer,uv);
                vec3 lightTs=normalize(worldToTangent*L);
                float lightmap=sixA.r*max(lightTs.x,0.0)+sixB.r*max(-lightTs.x,0.0)
                    +sixA.g*max(lightTs.y,0.0)+sixB.g*max(-lightTs.y,0.0)
                    +sixA.b*max(lightTs.z,0.0)+sixB.b*max(-lightTs.z,0.0);
                float sixAmbient=dot(sixA.rgb+sixB.rgb,vec3(1.0/6.0));
                color=base*(lightmap*uSixWayIntensity*uKeyColor*uKeyIntensity*keyShadow
                    +sixAmbient*uSixWayAmbient*envDiffuse*uEnvironmentIntensity);
                opacity*=sixA.a;
                if(uSixWayFireIntensity>0.0){
                    // Fire mask in the second lightmap's alpha: blackbody ramp
                    // self-illuminates burning regions inside the smoke.
                    float fireMask=sixB.a;
                    color+=vec3(pow(fireMask,1.1),pow(fireMask,2.4)*.85,pow(fireMask,5.5)*.72)*uSixWayFireIntensity*fireMask;
                }
            }

            if(stateSparkle>0.0&&!defaults){
                float sparkle=pow(hash13(vec3(floor(uv*max(uGlintScale,1.0)*220.0),1.0)),24.0);
                color+=uKeyColor*uKeyIntensity*sparkle*stateSparkle*8.0*NoL*keyShadow;
            }
            if(uRetroStrength>0.0&&!defaults){
                // Small lobe aimed back toward the light: visible when the viewer
                // sits near the light source, like tape and signs under a headlamp.
                float retro=pow(max(dot(V,L),0.0),mix(192.0,12.0,clamp(uRetroWidth,0.0,1.0)));
                color+=uKeyColor*uKeyIntensity*uRetroStrength*retro*NoL*keyShadow;
            }
            if(hasSpecial(1)&&!defaults){
                vec4 surface=specialSample(1,uv);
                float sheenMask=surface.r*uSheenStrength,glintMask=surface.g*uGlintIntensity;
                color+=base*sheenMask*pow(1.0-NoV,max(.25,4.0*(1.0-uSheenRoughness)))*(envDiffuse+uKeyColor*uKeyIntensity*.15);
                float glint=step(1.0-clamp(glintMask*.01,0.0,.98),hash13(vWorldPosition*max(uGlintScale,.001)+N*17.0))*pow(NoH,256.0);
                color+=uKeyColor*uKeyIntensity*glint*NoL;
                float iri=surface.a*uIridescenceStrength;vec3 film=thinFilmColor(NoV,surface.b,uFilmIor);
                color+=environmentSpecular*(film-vec3(.5))*iri*2.0;
            }

            if(hasSpecial(3)&&uSubsurfaceMode>0&&!defaults){
                vec4 subsurface=specialSample(3,uv);vec3 scatterTint=subsurface.rgb*uSubsurfaceProfileColor;
                if(hasSpecial(10)){vec4 detail=specialSample(10,uv);scatterTint*=mix(vec3(1.0),vec3(1.15,.72,.58),detail.g*detail.a);rough=clamp(rough-detail.b*detail.a*.2,.035,1.0);}
                // Pre-integrated-style scatter broadening: curvature widens the
                // diffusion falloff so ears, noses, and creases bleed across
                // the terminator while flat areas stay crisp. The geometric
                // term comes free from screen-space derivatives; the authored
                // map adds meso detail the derivatives cannot see.
                float geomCurv=clamp(length(fwidth(N))/max(length(fwidth(vWorldPosition)),1e-4)*.15,0.0,1.0);
                float scatterCurv=clamp(geomCurv+(curvatureAuthored?abs(curvature-.5)*2.0:0.0),0.0,1.0);
                float wrap=.45+scatterCurv*.4;
                float thickness=subsurface.a*uSubsurfaceThickness,wrapped=max((dot(N,L)+wrap)/(1.0+wrap),0.0);
                // Convex-curvature thickness proxy: thin edges (leaf rims, ear
                // cartilage) transmit when no authored thickness exists.
                if(uSssCurvatureThickness)thickness*=mix(.35,1.0,clamp((.5-curvature)*2.0,0.0,1.0));
                if(uSubsurfaceMode==1||uSubsurfaceMode==3)color+=base*scatterTint*uSubsurfaceWeight*max(wrapped-NoL,0.0)*uKeyColor*uKeyIntensity;
                if(uSubsurfaceMode==2||uSubsurfaceMode==3){float back=max(dot(-N,L),0.0);color+=base*scatterTint*pow(max(uTransmissionTint,vec3(.001)),vec3(2.2))*uSubsurfaceTransmission*back*thickness*uKeyColor*uKeyIntensity;}
            }

            if((uShaderMode==2||(uShaderMode==8&&(hasSpecial(4)||hasSpecial(5))))&&!defaults){
                vec4 directionData=specialSample(4,uv),scatterData=specialSample(5,uv);vec2 flow=directionData.rg*2.0-1.0;
                vec3 strand=normalize(mix(T,normalize(T*flow.x+B*flow.y),clamp(uHairDirectionStrength,0.0,1.0)));
                vec3 shiftedPrimary=normalize(strand+N*(uHairPrimaryShift+(directionData.b-.5)*.25));
                vec3 shiftedSecondary=normalize(strand+N*(uHairSecondaryShift+(directionData.b-.5)*.25));
                float primary=pow(sqrt(max(0.0,1.0-dot(shiftedPrimary,H)*dot(shiftedPrimary,H))),mix(96.0,4.0,uHairPrimaryRoughness));
                float secondary=pow(sqrt(max(0.0,1.0-dot(shiftedSecondary,H)*dot(shiftedSecondary,H))),mix(64.0,2.0,uHairSecondaryRoughness));
                vec3 scatterTint=pow(max(scatterData.rgb,vec3(.001)),vec3(2.2));
                color+=(uHairPrimaryStrength*primary*vec3(1.0)+uHairSecondaryStrength*secondary*scatterTint)*NoL*uKeyColor*uKeyIntensity;
                color+=scatterTint*(uHairScatter*directionData.a+.5*uHairBacklight*scatterData.a*max(dot(-N,L),0.0))*base;
            }

            for(int ci=0;ci<3;++ci){
                if(defaults||!hasSpecial(uCoatSpecialLayer[ci]))continue;
                vec2 coatUv=ci==0?uv0:(ci==1?uv1:uv2);
                vec4 coat=specialSample(uCoatSpecialLayer[ci],coatUv);vec3 coatTs=vec3(coat.rg*2.0-1.0,1.0);coatTs.xy*=uCoatNormalStrength[ci];coatTs.z=sqrt(max(.001,1.0-dot(coatTs.xy,coatTs.xy)));vec3 coatN=normalize(tangentToWorld*coatTs);
                float coatRough=clamp(coat.b,.025,1.0),coatCoverage=coat.a*uCoatStrength[ci],coatNoV=max(dot(coatN,V),.001),coatNoL=max(dot(coatN,L),0.0);
                float coatF0=pow((uCoatIor[ci]-1.0)/(uCoatIor[ci]+1.0),2.0);vec3 coatF=fresnel(max(dot(V,normalize(V+L)),0.0),vec3(coatF0));
                float coatAlpha=coatRough*coatRough,coatNoH=max(dot(coatN,normalize(V+L)),0.0),coatK=(coatRough+1.0)*(coatRough+1.0)/8.0;
                vec3 coatDirect=D_GGX(coatNoH,coatAlpha)*G1(coatNoL,coatK)*G1(coatNoV,coatK)*coatF/max(4.0*coatNoL*coatNoV,.001)*coatNoL*uKeyColor*uKeyIntensity;
                vec3 coatR=environmentDirection(reflect(-V,coatN));vec3 coatEnv=textureLod(uPrefilteredEnvironment,coatR,coatRough*uPrefilteredEnvironmentMaxLod).rgb;vec2 coatBrdf=texture(uBrdfLut,vec2(coatNoV,coatRough)).rg;
                vec3 coatTint=pow(max(uCoatTint[ci],vec3(.001)),vec3(2.2));
                color*=mix(vec3(1.0),coatTint,clamp(uCoatAbsorption[ci]*coatCoverage,0.0,1.0));
                color=mix(color*(1.0-coatF0*coatCoverage),color+coatDirect*coatCoverage+coatEnv*(coatF0*coatBrdf.x+coatBrdf.y)*coatCoverage*uEnvironmentIntensity,clamp(coatCoverage,0.0,1.0));
            }

            if((uShaderMode==6||(uShaderMode==8&&hasSpecial(12)))&&hasSpecial(12)&&!defaults){
                vec4 cornea=specialSample(12,uv);vec3 corneaTs=vec3(cornea.rg*2.0-1.0,1.0);corneaTs.z=sqrt(max(.001,1.0-dot(corneaTs.xy,corneaTs.xy)));vec3 corneaN=normalize(tangentToWorld*corneaTs);
                float wetRough=clamp(cornea.b,.015,1.0),wetNoV=max(dot(corneaN,V),.001),wetF0=pow((uEyeCorneaIor-1.0)/(uEyeCorneaIor+1.0),2.0);
                vec3 wetEnv=textureLod(uPrefilteredEnvironment,environmentDirection(reflect(-V,corneaN)),wetRough*uPrefilteredEnvironmentMaxLod).rgb;vec2 wetBrdf=texture(uBrdfLut,vec2(wetNoV,wetRough)).rg;
                color+=wetEnv*(wetF0*wetBrdf.x+wetBrdf.y)*cornea.a*uEyeWetness*uEnvironmentIntensity;
            }

            if((uShaderMode==3||uCompositionMode==6||(uShaderMode==8&&hasSpecial(6)))&&!defaults){
                vec4 transmission=hasSpecial(6)?specialSample(6,uv):vec4(1.0);vec4 imperfections=hasSpecial(7)?specialSample(7,uv):vec4(0.0);
                float imperfection=(imperfections.r*.5+imperfections.g*.35+imperfections.b*.2)*imperfections.a;
                float refractionRough=clamp(uGlassRefractionRoughness+imperfection*.55,0.0,1.0),thickness=uGlassThinWalled?.02:max(transmission.a*uGlassThickness,.001);
                vec3 refracted=refract(-V,N,1.0/max(uGlassIor,1.001));if(dot(refracted,refracted)<.001)refracted=-V;
                vec3 transmitted;vec3 interiorGlow=vec3(0.0);
                if(uInteriorEnabled){
                    // Interior mapping: intersect the view ray with a unit room
                    // box behind each window pane and sample its cubemap.
                    vec3 viewTs=normalize(worldToTangent*(-V));
                    vec2 roomUv=fract(uv*uInteriorRooms);
                    vec3 rayStart=vec3(roomUv*2.0-1.0,1.0);
                    vec3 rayDir=vec3(viewTs.xy,min(viewTs.z,-.0001)/max(uInteriorDepth,.05));
                    vec3 invDir=1.0/rayDir;
                    vec3 kPlanes=abs(invDir)-rayStart*invDir;
                    float kMin=min(min(kPlanes.x,kPlanes.y),kPlanes.z);
                    vec3 hit=rayStart+kMin*rayDir;
                    vec4 interiorSample=texture(uInterior,hit);
                    vec3 interiorLinear=pow(max(interiorSample.rgb,vec3(0.0)),vec3(2.2));
                    transmitted=interiorLinear*uInteriorBrightness/max(uEnvironmentIntensity,.05);
                    // Cubemap alpha marks self-lit interior areas: those windows
                    // glow on their own, independent of scene lighting.
                    interiorGlow=interiorLinear*interiorSample.a*uInteriorEmissive;
                }else{
                    transmitted=textureLod(uPrefilteredEnvironment,environmentDirection(refracted),refractionRough*uPrefilteredEnvironmentMaxLod).rgb;
                }
                vec3 absorption=pow(max(transmission.rgb,vec3(.001)),vec3(thickness/max(uGlassAbsorptionDistance,.0001)));
                float glassF0=pow((uGlassIor-1.0)/(uGlassIor+1.0),2.0);vec3 glassF=fresnel(NoV,vec3(glassF0));
                color=mix(transmitted*absorption,envSpec,glassF)*uEnvironmentIntensity+environmentDiffuse*.08+interiorGlow*absorption;opacity=clamp(uGlobalOpacity,0.02,1.0);
            }

            if((uShaderMode==4||(uShaderMode==8&&(hasSpecial(8)||hasSpecial(9))))&&!defaults){
                vec4 flow=hasSpecial(8)?specialSample(8,uv):vec4(0.5,0.5,0,0),foam=hasSpecial(9)?specialSample(9,uv):vec4(0);
                vec3 refracted=refract(-V,N,1.0/max(uWaterIor,1.001));vec3 waterThrough=textureLod(uPrefilteredEnvironment,environmentDirection(refracted),rough*uPrefilteredEnvironmentMaxLod).rgb;
                vec3 waterAbsorption=pow(max(base,vec3(.001)),vec3(1.0/max(uWaterAbsorptionDistance*.01,.001)));
                float waterF0=pow((uWaterIor-1.0)/(uWaterIor+1.0),2.0);vec3 waterF=fresnel(NoV,vec3(waterF0));
                color=mix(waterThrough*waterAbsorption+base*uWaterScattering,envSpec*uWaterReflectionStrength,waterF)*uEnvironmentIntensity;
                float foamCoverage=foam.a*(uWaterShoreFoam?max(flow.a,1.0-NoV):flow.a);color=mix(color,pow(foam.rgb,vec3(2.2))*uKeyIntensity,clamp(foamCoverage,0.0,1.0));opacity=clamp(uGlobalOpacity,.15,1.0);
            }

            if(hasSpecial(14)&&!defaults){
                vec4 distortion=specialSample(14,uv);vec2 direction=distortion.rg*2.0-1.0;float amount=distortion.b*distortion.a*uDistortionVectorScale;
                vec3 distortedDirection=normalize(R+T*direction.x*amount*.08+B*direction.y*amount*.08);
                vec3 distorted=textureLod(uPrefilteredEnvironment,environmentDirection(distortedDirection),rough*uPrefilteredEnvironmentMaxLod).rgb;
                color=mix(color,distorted*uEnvironmentIntensity,clamp(abs(amount),0.0,1.0));
            }
            if(uDecalEnabled&&!defaults){if(uDecalBlend==1)color*=mix(vec3(1.0),base,opacity);else if(uDecalBlend==2)color+=base*opacity;}
            color+=emission;
            vec3 mapped=tonemap(color);if(uCompositionMode==3)mapped*=opacity;
            fragColor=vec4(mapped,opacity);
        }
    )GLSL";
    if (!m_program->addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShader)
        || !m_program->addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShader)
        || !m_program->link()) recordShaderFailure("mesh",m_program->log());

    m_backgroundProgram = std::make_unique<QOpenGLShaderProgram>();
    static const char* backgroundVertex = R"GLSL(
        #version 330 core
        out vec2 vNdc;
        void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);vNdc=p*2.0-1.0;gl_Position=vec4(vNdc,1,1);}
    )GLSL";
    static const char* backgroundFragment = R"GLSL(
        #version 330 core
        in vec2 vNdc;out vec4 fragColor;uniform samplerCube uEnvironment;uniform mat4 uInvViewProjection;
        uniform vec3 uCameraPosition;uniform int uBackground;uniform float uEnvironmentYaw,uEnvironmentIntensity,uExposure;
        vec3 rotateY(vec3 v,float a){float c=cos(a),s=sin(a);return vec3(c*v.x+s*v.z,v.y,-s*v.x+c*v.z);}
        vec3 environmentDirection(vec3 v){v=rotateY(v,radians(uEnvironmentYaw));return vec3(v.x,v.z,-v.y);}
        vec3 tonemap(vec3 c){c*=exp2(uExposure);c=c/(c+vec3(1));return pow(max(c,vec3(0)),vec3(1.0/2.2));}
        void main(){
            vec3 color;
            if(uBackground==0){vec4 p=uInvViewProjection*vec4(vNdc,1,1);vec3 ray=normalize(p.xyz/p.w-uCameraPosition);color=tonemap(texture(uEnvironment,environmentDirection(ray)).rgb*uEnvironmentIntensity);}
            else if(uBackground==1)color=vec3(.18);else if(uBackground==2)color=vec3(.005);else if(uBackground==3)color=vec3(.82);
            else{vec2 q=floor(gl_FragCoord.xy/18.0);color=mod(q.x+q.y,2.0)==0.0?vec3(.11):vec3(.16);}
            fragColor=vec4(color,1);
        }
    )GLSL";
    if (!m_backgroundProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, backgroundVertex)
        || !m_backgroundProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, backgroundFragment)
        || !m_backgroundProgram->link()) recordShaderFailure("background", m_backgroundProgram->log());

    static const char* integrationVertex = R"GLSL(
        #version 330 core
        out vec2 vUv;
        void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);vUv=p;gl_Position=vec4(p*2.0-1.0,0,1);}
    )GLSL";
    static const char* prefilterFragment = R"GLSL(
        #version 330 core
        in vec2 vUv;out vec4 fragColor;
        uniform samplerCube uSourceEnvironment;
        uniform int uFace;
        uniform float uRoughness,uSourceResolution,uSourceMaxLod;
        uniform int uSampleCount;
        uniform bool uUseSourceMipFiltering;
        const float PI=3.14159265359;
        vec3 cubeDirection(int face,vec2 p){
            if(face==0)return normalize(vec3(1.0,-p.y,-p.x));
            if(face==1)return normalize(vec3(-1.0,-p.y,p.x));
            if(face==2)return normalize(vec3(p.x,1.0,p.y));
            if(face==3)return normalize(vec3(p.x,-1.0,-p.y));
            if(face==4)return normalize(vec3(p.x,-p.y,1.0));
            return normalize(vec3(-p.x,-p.y,-1.0));
        }
        float radicalInverse(uint bits){
            bits=(bits<<16u)|(bits>>16u);
            bits=((bits&0x55555555u)<<1u)|((bits&0xAAAAAAAAu)>>1u);
            bits=((bits&0x33333333u)<<2u)|((bits&0xCCCCCCCCu)>>2u);
            bits=((bits&0x0F0F0F0Fu)<<4u)|((bits&0xF0F0F0F0u)>>4u);
            bits=((bits&0x00FF00FFu)<<8u)|((bits&0xFF00FF00u)>>8u);
            return float(bits)*2.3283064365386963e-10;
        }
        vec2 hammersley(uint i,uint count){return vec2(float(i)/float(count),radicalInverse(i));}
        vec3 importanceSampleGgx(vec2 xi,vec3 n,float roughness){
            float a=roughness*roughness,a2=a*a;
            float phi=2.0*PI*xi.x;
            float cosTheta=sqrt((1.0-xi.y)/(1.0+(a2-1.0)*xi.y));
            float sinTheta=sqrt(max(0.0,1.0-cosTheta*cosTheta));
            vec3 h=vec3(cos(phi)*sinTheta,sin(phi)*sinTheta,cosTheta);
            vec3 up=abs(n.z)<.999?vec3(0,0,1):vec3(1,0,0);
            vec3 tangent=normalize(cross(up,n)),bitangent=cross(n,tangent);
            return normalize(tangent*h.x+bitangent*h.y+n*h.z);
        }
        float distributionGgx(float noH,float roughness){
            float a=roughness*roughness,a2=a*a,d=noH*noH*(a2-1.0)+1.0;
            return a2/max(PI*d*d,1e-5);
        }
        void main(){
            vec3 n=cubeDirection(uFace,vUv*2.0-1.0),v=n;
            vec3 color=vec3(0.0);float weight=0.0;
            const int maxSampleCount=1024;
            for(int i=0;i<maxSampleCount;++i){
                if(i>=uSampleCount)break;
                vec3 h=importanceSampleGgx(hammersley(uint(i),uint(uSampleCount)),n,max(uRoughness,.001));
                vec3 l=normalize(2.0*dot(v,h)*h-v);
                float noL=max(dot(n,l),0.0);
                if(noL>0.0){
                    float noH=max(dot(n,h),0.0),hoV=max(dot(h,v),0.0);
                    float pdf=distributionGgx(noH,max(uRoughness,.001))*noH/max(4.0*hoV,1e-5)+1e-5;
                    float texelSolidAngle=4.0*PI/(6.0*uSourceResolution*uSourceResolution);
                    float sampleSolidAngle=1.0/(float(uSampleCount)*pdf);
                    float sourceLod=uUseSourceMipFiltering&&uRoughness>.001
                        ?.5*log2(sampleSolidAngle/texelSolidAngle):0.0;
                    color+=textureLod(uSourceEnvironment,l,clamp(sourceLod,0.0,uSourceMaxLod)).rgb*noL;
                    weight+=noL;
                }
            }
            fragColor=vec4(color/max(weight,1e-4),1.0);
        }
    )GLSL";
    m_environmentPrefilterProgram = std::make_unique<QOpenGLShaderProgram>();
    if (!m_environmentPrefilterProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, integrationVertex)
        || !m_environmentPrefilterProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, prefilterFragment)
        || !m_environmentPrefilterProgram->link()) recordShaderFailure("environment prefilter", m_environmentPrefilterProgram->log());

    static const char* brdfFragment = R"GLSL(
        #version 330 core
        in vec2 vUv;out vec4 fragColor;
        const float PI=3.14159265359;
        float radicalInverse(uint bits){
            bits=(bits<<16u)|(bits>>16u);
            bits=((bits&0x55555555u)<<1u)|((bits&0xAAAAAAAAu)>>1u);
            bits=((bits&0x33333333u)<<2u)|((bits&0xCCCCCCCCu)>>2u);
            bits=((bits&0x0F0F0F0Fu)<<4u)|((bits&0xF0F0F0F0u)>>4u);
            bits=((bits&0x00FF00FFu)<<8u)|((bits&0xFF00FF00u)>>8u);
            return float(bits)*2.3283064365386963e-10;
        }
        vec2 hammersley(uint i,uint count){return vec2(float(i)/float(count),radicalInverse(i));}
        vec3 importanceSampleGgx(vec2 xi,float roughness){
            float a=roughness*roughness,a2=a*a,phi=2.0*PI*xi.x;
            float cosTheta=sqrt((1.0-xi.y)/(1.0+(a2-1.0)*xi.y));
            float sinTheta=sqrt(max(0.0,1.0-cosTheta*cosTheta));
            return vec3(cos(phi)*sinTheta,sin(phi)*sinTheta,cosTheta);
        }
        float geometrySchlickGgx(float noV,float roughness){
            float a=roughness, k=a*a*.5;
            return noV/max(noV*(1.0-k)+k,1e-5);
        }
        void main(){
            float noV=clamp(vUv.x,.001,.999),roughness=clamp(vUv.y,.001,.999);
            vec3 v=vec3(sqrt(max(0.0,1.0-noV*noV)),0.0,noV);
            float a=0.0,b=0.0;const uint sampleCount=256u;
            for(uint i=0u;i<sampleCount;++i){
                vec3 h=importanceSampleGgx(hammersley(i,sampleCount),roughness);
                vec3 l=normalize(2.0*dot(v,h)*h-v);
                float noL=max(l.z,0.0),noH=max(h.z,0.0),voH=max(dot(v,h),0.0);
                if(noL>0.0){
                    float g=geometrySchlickGgx(noV,roughness)*geometrySchlickGgx(noL,roughness);
                    float visibility=g*voH/max(noH*noV,1e-5);
                    float fc=pow(1.0-voH,5.0);
                    a+=(1.0-fc)*visibility;b+=fc*visibility;
                }
            }
            fragColor=vec4(a/float(sampleCount),b/float(sampleCount),0.0,1.0);
        }
    )GLSL";
    m_brdfIntegrationProgram = std::make_unique<QOpenGLShaderProgram>();
    if (!m_brdfIntegrationProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, integrationVertex)
        || !m_brdfIntegrationProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, brdfFragment)
        || !m_brdfIntegrationProgram->link()) recordShaderFailure("BRDF integration", m_brdfIntegrationProgram->log());

    m_shadowProgram = std::make_unique<QOpenGLShaderProgram>();
    static const char* shadowVertex = R"GLSL(
        #version 330 core
        layout(location=0) in vec3 aPosition;
        uniform mat4 uLightViewProjection;
        uniform mat4 uModel;
        void main(){gl_Position=uLightViewProjection*uModel*vec4(aPosition,1.0);}
    )GLSL";
    static const char* shadowFragment = R"GLSL(
        #version 330 core
        void main(){}
    )GLSL";
    if (!m_shadowProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, shadowVertex)
        || !m_shadowProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, shadowFragment)
        || !m_shadowProgram->link()) recordShaderFailure("shadow map", m_shadowProgram->log());

    glGenTextures(1, &m_shadowDepthTexture);
    glBindTexture(GL_TEXTURE_2D, m_shadowDepthTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, ShadowMapSize, ShadowMapSize, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    const GLfloat border[] = {1, 1, 1, 1};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
    glGenFramebuffers(1, &m_shadowFramebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, m_shadowFramebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_shadowDepthTexture, 0);
    glDrawBuffer(GL_NONE); glReadBuffer(GL_NONE);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        qWarning("PBR preview shadow framebuffer is incomplete.");
        glDeleteFramebuffers(1, &m_shadowFramebuffer); m_shadowFramebuffer = 0;
        glDeleteTextures(1, &m_shadowDepthTexture); m_shadowDepthTexture = 0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
    glBindTexture(GL_TEXTURE_2D, 0);

    createBrdfLut();
    createEnvironmentCubemap();
    m_texturesDirty = true; m_meshDirty = true;
}

void PbrPreviewGL::resizeGL(int, int) {}

void PbrPreviewGL::uploadTexture(std::unique_ptr<QOpenGLTexture>& texture, const QImage& source, const QColor& fallback)
{
    QImage image = source;
    if (image.isNull()) { image = QImage(1,1,QImage::Format_RGBA8888); image.fill(fallback); }
    image = image.convertToFormat(QImage::Format_RGBA8888).flipped(Qt::Vertical);
    texture = std::make_unique<QOpenGLTexture>(image);
    texture->setWrapMode(QOpenGLTexture::Repeat);
    texture->setMinMagFilters(QOpenGLTexture::LinearMipMapLinear, QOpenGLTexture::Linear);
    texture->generateMipMaps();
}

void PbrPreviewGL::uploadTextures()
{
    uploadTexture(m_baseTexture,m_baseImage,Qt::white);
    uploadTexture(m_normalTexture,m_normalImage,QColor(128,128,255));
    uploadTexture(m_heightTexture,m_heightImage,QColor::fromRgbF(m_heightCenter,m_heightCenter,m_heightCenter));
    uploadTexture(m_rmaosTexture,m_rmaosImage,QColor::fromRgbF(m_roughness,m_metallic,m_ao,m_f0));
    uploadTexture(m_emissiveTexture,m_emissiveImage,Qt::white);
    uploadTexture(m_secondaryBaseTexture,m_secondaryBaseImage,Qt::white);
    uploadTexture(m_secondaryNormalTexture,m_secondaryNormalImage,QColor(128,128,255));
    uploadTexture(m_secondaryRmaosTexture,m_secondaryRmaosImage,QColor::fromRgbF(m_secondaryRoughness,m_secondaryMetallic,m_secondaryAo,m_secondaryF0));
    uploadTexture(m_secondaryEmissiveTexture,m_secondaryEmissiveImage,Qt::white);
    uploadTexture(m_layerCoverageTexture,m_layerCoverageImage,Qt::white);
    uploadTexture(m_tertiaryBaseTexture,m_tertiaryBaseImage,Qt::white);
    uploadTexture(m_tertiaryNormalTexture,m_tertiaryNormalImage,QColor(128,128,255));
    uploadTexture(m_tertiaryRmaosTexture,m_tertiaryRmaosImage,QColor::fromRgbF(m_tertiaryRoughness,m_tertiaryMetallic,m_tertiaryAo,m_tertiaryF0));
    uploadTexture(m_tertiaryEmissiveTexture,m_tertiaryEmissiveImage,Qt::white);
    uploadTexture(m_tertiaryCoverageTexture,m_tertiaryCoverageImage,Qt::white);
    {
        const int faceSize=m_interiorFaces.size()==6?m_interiorFaces.first().width():4;
        m_interiorTexture=std::make_unique<QOpenGLTexture>(QOpenGLTexture::TargetCubeMap);
        m_interiorTexture->setSize(faceSize,faceSize);
        m_interiorTexture->setFormat(QOpenGLTexture::RGBA8_UNorm);
        m_interiorTexture->setMipLevels(1);
        m_interiorTexture->allocateStorage();
        for(int face=0;face<6;++face){
            QImage image=m_interiorFaces.size()==6
                ? m_interiorFaces[face].scaled(faceSize,faceSize).convertToFormat(QImage::Format_RGBA8888)
                : QImage(faceSize,faceSize,QImage::Format_RGBA8888);
            if(m_interiorFaces.size()!=6)image.fill(QColor(24,22,20));
            m_interiorTexture->setData(0,0,static_cast<QOpenGLTexture::CubeMapFace>(QOpenGLTexture::CubeMapPositiveX+face),QOpenGLTexture::RGBA,QOpenGLTexture::UInt8,image.constBits());
        }
        m_interiorTexture->setWrapMode(QOpenGLTexture::ClampToEdge);
        m_interiorTexture->setMinMagFilters(QOpenGLTexture::Linear,QOpenGLTexture::Linear);
    }
    uploadTexture(m_gradientTexture,m_gradientImage,Qt::white);
    int width=1,height=1;for(const QImage& image:m_specialImages)if(!image.isNull()){width=std::max(width,image.width());height=std::max(height,image.height());}
    width=std::min(width,512);height=std::min(height,512);
    m_specialTextureArray=std::make_unique<QOpenGLTexture>(QOpenGLTexture::Target2DArray);
    m_specialTextureArray->setSize(width,height);m_specialTextureArray->setLayers(SpecialLayerCount);m_specialTextureArray->setFormat(QOpenGLTexture::RGBA8_UNorm);
    m_specialTextureArray->setMipLevels(qMax(1,int(std::floor(std::log2(qMax(width,height))))+1));m_specialTextureArray->allocateStorage();
    for(int layer=0;layer<SpecialLayerCount;++layer){
        QImage image=layer<m_specialImages.size()?m_specialImages[layer]:QImage();
        if(image.isNull()){image=QImage(width,height,QImage::Format_RGBA8888);image.fill(Qt::transparent);}
        else image=image.scaled(width,height,Qt::IgnoreAspectRatio,Qt::SmoothTransformation).convertToFormat(QImage::Format_RGBA8888).flipped(Qt::Vertical);
        m_specialTextureArray->setData(0,layer,QOpenGLTexture::RGBA,QOpenGLTexture::UInt8,image.constBits());
    }
    m_specialTextureArray->setWrapMode(QOpenGLTexture::Repeat);m_specialTextureArray->setMinMagFilters(QOpenGLTexture::LinearMipMapLinear,QOpenGLTexture::Linear);m_specialTextureArray->generateMipMaps();
    m_texturesDirty=false;
}

void PbrPreviewGL::createEnvironmentCubemap()
{
    const int size = m_environmentFaces.size() == 6 ? m_environmentFaces.first().width() : 64;
    const bool hdr = m_environmentFaces.size() == 6
        && m_environmentFaces.first().format() == QImage::Format_RGBA32FPx4;
    m_environmentTexture = std::make_unique<QOpenGLTexture>(QOpenGLTexture::TargetCubeMap);
    m_environmentTexture->setSize(size, size);
    m_environmentTexture->setFormat(hdr ? QOpenGLTexture::RGBA16F : QOpenGLTexture::RGBA8_UNorm);
    const int levels = qMax(1, int(std::floor(std::log2(qMax(1, size)))) + 1);
    m_environmentTexture->setMipLevels(levels); m_environmentTexture->allocateStorage();
    for (int face = 0; face < 6; ++face) {
        QImage image = m_environmentFaces.size() == 6
            ? (hdr ? m_environmentFaces[face] : m_environmentFaces[face].scaled(size, size).convertToFormat(QImage::Format_RGBA8888))
            : QImage(size,size,QImage::Format_RGBA8888);
        if (m_environmentFaces.size() != 6) for (int y=0;y<size;++y) for(int x=0;x<size;++x) {
            const QVector3D d=cubeDirection(face,2.0f*(x+.5f)/size-1.0f,2.0f*(y+.5f)/size-1.0f);
            const float vertical=-d.z()*.5f+.5f;
            QVector3D color=QVector3D(.025f,.035f,.055f)*(1.0f-vertical)+QVector3D(.25f,.3f,.38f)*vertical;
            const float panel=std::pow(std::max(QVector3D::dotProduct(d,QVector3D(-.45f,.72f,-.65f).normalized()),0.0f),48.0f);
            const float fill=std::pow(std::max(QVector3D::dotProduct(d,QVector3D(.7f,.5f,-.1f).normalized()),0.0f),24.0f);
            color+=QVector3D(2.5f,2.25f,1.95f)*panel+QVector3D(.25f,.42f,.75f)*fill;
            image.setPixelColor(x,y,linearColor(color));
        }
        m_environmentTexture->setData(0,0,static_cast<QOpenGLTexture::CubeMapFace>(QOpenGLTexture::CubeMapPositiveX+face),
            QOpenGLTexture::RGBA,hdr ? QOpenGLTexture::Float32 : QOpenGLTexture::UInt8,image.constBits());
    }
    m_environmentTexture->generateMipMaps();
    m_environmentTexture->setWrapMode(QOpenGLTexture::ClampToEdge);
    m_environmentTexture->setMinMagFilters(QOpenGLTexture::LinearMipMapLinear,QOpenGLTexture::Linear);

    const int prefilterSize = qMin(size, 256);
    const int prefilterLevels = qMax(1, int(std::floor(std::log2(qMax(1, prefilterSize)))) + 1);
    m_prefilteredEnvironmentMaxLod = float(prefilterLevels - 1);
    m_prefilteredEnvironmentTexture = std::make_unique<QOpenGLTexture>(QOpenGLTexture::TargetCubeMap);
    m_prefilteredEnvironmentTexture->setSize(prefilterSize,prefilterSize);
    m_prefilteredEnvironmentTexture->setFormat(QOpenGLTexture::RGBA16F);
    m_prefilteredEnvironmentTexture->setMipLevels(prefilterLevels);
    m_prefilteredEnvironmentTexture->allocateStorage();
    m_prefilteredEnvironmentTexture->setWrapMode(QOpenGLTexture::ClampToEdge);
    m_prefilteredEnvironmentTexture->setMinMagFilters(QOpenGLTexture::LinearMipMapLinear,QOpenGLTexture::Linear);

    if (!m_environmentPrefilterProgram || !m_environmentPrefilterProgram->isLinked()) return;
    GLuint framebuffer=0;glGenFramebuffers(1,&framebuffer);glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);
    glDisable(GL_DEPTH_TEST);m_environmentTexture->bind(0);m_environmentPrefilterProgram->bind();
    m_environmentPrefilterProgram->setUniformValue("uSourceEnvironment",0);
    m_environmentPrefilterProgram->setUniformValue("uSourceResolution",float(size));
    m_environmentPrefilterProgram->setUniformValue("uSourceMaxLod",float(levels-1));
    // loadCubemapFaces() deliberately decodes only mip 0 from Fallout DDS
    // cubemaps. generateMipMaps() above therefore creates a clean replacement
    // chain from those six base faces. Rough convolution must use that generated
    // chain; forcing DDS samples to LOD 0 exposes the original face boundaries
    // as a mirrored-looking rear hemisphere seam. EXRs reach this point through
    // their separate floating-point latitude-longitude conversion.
    m_environmentPrefilterProgram->setUniformValue("uUseSourceMipFiltering",true);
    m_vao->bind();
    bool complete=true;
    for(int level=0;level<prefilterLevels&&complete;++level){
        const int levelSize=qMax(1,prefilterSize>>level);
        const float roughness=prefilterLevels>1?float(level)/float(prefilterLevels-1):0.0f;
        // Fallout-authored lower mips are never uploaded. DDS filtering uses
        // only the clean chain generated above from the full-resolution faces;
        // only HDR EXRs need the larger rough-lobe sample budget.
        const int sampleCount = level == 0 ? 1 : (hdr && roughness >= .25f ? 1024 : 256);
        glViewport(0,0,levelSize,levelSize);m_environmentPrefilterProgram->setUniformValue("uRoughness",roughness);
        m_environmentPrefilterProgram->setUniformValue("uSampleCount",sampleCount);
        for(int face=0;face<6;++face){
            glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_CUBE_MAP_POSITIVE_X+face,m_prefilteredEnvironmentTexture->textureId(),level);
            if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){complete=false;break;}
            m_environmentPrefilterProgram->setUniformValue("uFace",face);glDrawArrays(GL_TRIANGLES,0,3);
        }
    }
    m_vao->release();m_environmentPrefilterProgram->release();m_environmentTexture->release();
    glBindFramebuffer(GL_FRAMEBUFFER,defaultFramebufferObject());glDeleteFramebuffers(1,&framebuffer);
    glViewport(0,0,int(width()*devicePixelRatioF()),int(height()*devicePixelRatioF()));
    if(!complete)qWarning("PBR preview GGX environment prefilter framebuffer is incomplete.");
}

void PbrPreviewGL::createBrdfLut()
{
    constexpr int size=256;
    m_brdfLutTexture=std::make_unique<QOpenGLTexture>(QOpenGLTexture::Target2D);
    m_brdfLutTexture->setSize(size,size);m_brdfLutTexture->setFormat(QOpenGLTexture::RGBA16F);
    m_brdfLutTexture->setMipLevels(1);m_brdfLutTexture->allocateStorage();
    m_brdfLutTexture->setWrapMode(QOpenGLTexture::ClampToEdge);
    m_brdfLutTexture->setMinMagFilters(QOpenGLTexture::Linear,QOpenGLTexture::Linear);
    if(!m_brdfIntegrationProgram||!m_brdfIntegrationProgram->isLinked())return;
    GLuint framebuffer=0;glGenFramebuffers(1,&framebuffer);glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,m_brdfLutTexture->textureId(),0);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE){
        glViewport(0,0,size,size);glDisable(GL_DEPTH_TEST);m_brdfIntegrationProgram->bind();m_vao->bind();
        glDrawArrays(GL_TRIANGLES,0,3);m_vao->release();m_brdfIntegrationProgram->release();
    }else qWarning("PBR preview BRDF LUT framebuffer is incomplete.");
    glBindFramebuffer(GL_FRAMEBUFFER,defaultFramebufferObject());glDeleteFramebuffers(1,&framebuffer);
    glViewport(0,0,int(width()*devicePixelRatioF()),int(height()*devicePixelRatioF()));
}

void PbrPreviewGL::uploadMesh()
{
    if (!m_mesh.isValid() || !m_program || !m_program->isLinked()) return;
    m_program->bind(); m_vao->bind();
    m_vertexBuffer.bind(); m_vertexBuffer.allocate(m_mesh.vertices.constData(), int(m_mesh.vertices.size()*sizeof(PreviewVertex)));
    m_indexBuffer.bind(); m_indexBuffer.allocate(m_mesh.indices.constData(), int(m_mesh.indices.size()*sizeof(quint32)));
    const int stride=sizeof(PreviewVertex);
    m_program->enableAttributeArray(0); m_program->setAttributeBuffer(0,GL_FLOAT,offsetof(PreviewVertex,position),3,stride);
    m_program->enableAttributeArray(1); m_program->setAttributeBuffer(1,GL_FLOAT,offsetof(PreviewVertex,normal),3,stride);
    m_program->enableAttributeArray(2); m_program->setAttributeBuffer(2,GL_FLOAT,offsetof(PreviewVertex,tangent),4,stride);
    m_program->enableAttributeArray(3); m_program->setAttributeBuffer(3,GL_FLOAT,offsetof(PreviewVertex,uv0),2,stride);
    m_program->enableAttributeArray(4); m_program->setAttributeBuffer(4,GL_FLOAT,offsetof(PreviewVertex,uv1),2,stride);
    m_program->enableAttributeArray(5); m_program->setAttributeBuffer(5,GL_FLOAT,offsetof(PreviewVertex,color),4,stride);
    m_vao->release();

    PreviewMesh ground=PreviewMeshes::plane(PreviewMeshes::PlaneOrientation::XZ,false);
    for(auto& v:ground.vertices){v.position.setY(-1.08f);v.position.setX(v.position.x()*5);v.position.setZ(v.position.z()*5);}
    m_groundVao->bind(); m_groundVertexBuffer.bind(); m_groundVertexBuffer.allocate(ground.vertices.constData(),int(ground.vertices.size()*sizeof(PreviewVertex)));
    m_groundIndexBuffer.bind();m_groundIndexBuffer.allocate(ground.indices.constData(),int(ground.indices.size()*sizeof(quint32)));
    m_program->enableAttributeArray(0);m_program->setAttributeBuffer(0,GL_FLOAT,offsetof(PreviewVertex,position),3,stride);
    m_program->enableAttributeArray(1);m_program->setAttributeBuffer(1,GL_FLOAT,offsetof(PreviewVertex,normal),3,stride);
    m_program->enableAttributeArray(2);m_program->setAttributeBuffer(2,GL_FLOAT,offsetof(PreviewVertex,tangent),4,stride);
    m_program->enableAttributeArray(3);m_program->setAttributeBuffer(3,GL_FLOAT,offsetof(PreviewVertex,uv0),2,stride);
    m_program->enableAttributeArray(4);m_program->setAttributeBuffer(4,GL_FLOAT,offsetof(PreviewVertex,uv1),2,stride);
    m_program->enableAttributeArray(5);m_program->setAttributeBuffer(5,GL_FLOAT,offsetof(PreviewVertex,color),4,stride);
    m_groundVao->release(); m_program->release(); m_meshDirty=false;
}

QMatrix4x4 PbrPreviewGL::viewMatrix() const
{
    QMatrix4x4 view; view.translate(m_pan.x(),m_pan.y(),-m_distance); view.rotate(m_pitch,1,0,0); view.rotate(m_yaw,0,1,0); return view;
}

QMatrix4x4 PbrPreviewGL::projectionMatrix() const
{
    QMatrix4x4 projection; const float aspect=float(std::max(width(),1))/float(std::max(height(),1));
    if(m_projection==Projection::Perspective)projection.perspective(45.0f,aspect,.02f,100.0f);
    else{const float h=1.45f*m_distance/3.2f;projection.ortho(-h*aspect,h*aspect,-h,h,-100.0f,100.0f);}
    return projection;
}

QVector3D PbrPreviewGL::keyDirection() const
{
    const float yaw=qDegreesToRadians(m_keyYaw),pitch=qDegreesToRadians(m_keyPitch),cp=std::cos(pitch);
    return QVector3D(std::sin(yaw)*cp,std::sin(pitch),std::cos(yaw)*cp).normalized();
}

void PbrPreviewGL::renderShadowMap()
{
    const QVector3D direction=keyDirection();
    QMatrix4x4 lightView,lightProjection;
    const QVector3D up=std::abs(QVector3D::dotProduct(direction,QVector3D(0,1,0)))>.95f
        ? QVector3D(0,0,1):QVector3D(0,1,0);
    lightView.lookAt(direction*4.0f,QVector3D(),up);
    lightProjection.ortho(-2.4f,2.4f,-2.4f,2.4f,.1f,8.0f);
    m_shadowMatrix=lightProjection*lightView;
    if(!m_shadowsEnabled||m_shadowIntensity<=0.0f||!m_shadowProgram||!m_shadowProgram->isLinked()
        ||!m_shadowFramebuffer||!m_shadowDepthTexture||!m_mesh.isValid())return;

    glBindFramebuffer(GL_FRAMEBUFFER,m_shadowFramebuffer);
    glViewport(0,0,ShadowMapSize,ShadowMapSize);
    glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE);glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_POLYGON_OFFSET_FILL);glPolygonOffset(2.0f,4.0f);
    m_shadowProgram->bind();m_shadowProgram->setUniformValue("uLightViewProjection",m_shadowMatrix);m_shadowProgram->setUniformValue("uModel",objectModelMatrix());m_vao->bind();
    if(m_applyAllSubmeshes||m_mesh.submeshes.isEmpty())glDrawElements(GL_TRIANGLES,m_mesh.indices.size(),GL_UNSIGNED_INT,nullptr);
    else{const auto& s=m_mesh.submeshes[std::clamp(m_submesh,0,int(m_mesh.submeshes.size())-1)];glDrawElements(GL_TRIANGLES,s.indexCount,GL_UNSIGNED_INT,reinterpret_cast<const void*>(quintptr(s.firstIndex*sizeof(quint32))));}
    m_vao->release();m_shadowProgram->release();glDisable(GL_POLYGON_OFFSET_FILL);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER,defaultFramebufferObject());
}

void PbrPreviewGL::applyCommonUniforms(QOpenGLShaderProgram& p)
{
    const QMatrix4x4 view=viewMatrix(),projection=projectionMatrix(); const QMatrix4x4 inverse=(projection*view).inverted();
    const QVector4D eye4=view.inverted()*QVector4D(0,0,0,1); const QVector3D eye=eye4.toVector3DAffine();
    p.setUniformValue("uViewProjection",projection*view);p.setUniformValue("uInvViewProjection",inverse);p.setUniformValue("uCameraPosition",eye);
    p.setUniformValue("uEnvironmentYaw",m_environmentYaw);p.setUniformValue("uEnvironmentIntensity",m_environmentIntensity);p.setUniformValue("uExposure",m_exposure);
}

void PbrPreviewGL::paintGL()
{
    glViewport(0,0,int(width()*devicePixelRatioF()),int(height()*devicePixelRatioF()));
    glClearColor(.1f,.1f,.1f,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    if(!m_program||!m_program->isLinked()||!m_backgroundProgram||!m_backgroundProgram->isLinked())return;
    if(m_texturesDirty) uploadTextures();
    if(m_companionsDirty) uploadCompanionTextures();
    if(m_meshDirty) uploadMesh();
    if(m_environmentDirty){createEnvironmentCubemap();m_environmentDirty=false;}
    renderShadowMap();
    const int pixelWidth=int(width()*devicePixelRatioF()),pixelHeight=int(height()*devicePixelRatioF());
    if(m_msaaSamples>1){
        if(!m_msaaFbo||m_msaaFbo->size()!=QSize(pixelWidth,pixelHeight)||m_msaaFbo->format().samples()!=m_msaaSamples){
            QOpenGLFramebufferObjectFormat msaaFormat;
            msaaFormat.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
            msaaFormat.setSamples(m_msaaSamples);
            m_msaaFbo=std::make_unique<QOpenGLFramebufferObject>(pixelWidth,pixelHeight,msaaFormat);
        }
        m_msaaFbo->bind();
        glClearColor(.1f,.1f,.1f,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    } else if(m_msaaFbo) m_msaaFbo.reset();
    glViewport(0,0,pixelWidth,pixelHeight);
    m_environmentTexture->bind(5);m_prefilteredEnvironmentTexture->bind(12);m_brdfLutTexture->bind(13);m_specialTextureArray->bind(14);
    glDisable(GL_DEPTH_TEST);m_backgroundProgram->bind();m_backgroundProgram->setUniformValue("uEnvironment",5);m_backgroundProgram->setUniformValue("uBackground",int(m_background));applyCommonUniforms(*m_backgroundProgram);m_vao->bind();glDrawArrays(GL_TRIANGLES,0,3);m_vao->release();m_backgroundProgram->release();
    glEnable(GL_DEPTH_TEST);m_program->bind();
    m_baseTexture->bind(0);m_normalTexture->bind(1);m_rmaosTexture->bind(2);m_emissiveTexture->bind(3);m_heightTexture->bind(4);
    m_secondaryBaseTexture->bind(6);m_secondaryNormalTexture->bind(7);m_secondaryRmaosTexture->bind(8);m_secondaryEmissiveTexture->bind(9);m_layerCoverageTexture->bind(10);m_gradientTexture->bind(11);
    m_tertiaryBaseTexture->bind(16);m_tertiaryNormalTexture->bind(17);m_tertiaryRmaosTexture->bind(18);m_tertiaryEmissiveTexture->bind(19);m_tertiaryCoverageTexture->bind(20);
    m_interiorTexture->bind(21);m_program->setUniformValue("uInterior",21);
    m_program->setUniformValue("uInteriorEnabled",m_interiorEnabled);m_program->setUniformValue("uInteriorRooms",QVector2D(m_interiorRoomsU,m_interiorRoomsV));m_program->setUniformValue("uInteriorDepth",m_interiorDepth);m_program->setUniformValue("uInteriorBrightness",m_interiorBrightness);m_program->setUniformValue("uInteriorEmissive",m_interiorEmissive);
    m_program->setUniformValue("uBase",0);m_program->setUniformValue("uNormal",1);m_program->setUniformValue("uRmaos",2);m_program->setUniformValue("uEmissive",3);m_program->setUniformValue("uHeight",4);m_program->setUniformValue("uEnvironment",5);
    glActiveTexture(GL_TEXTURE15);glBindTexture(GL_TEXTURE_2D,m_shadowDepthTexture);m_program->setUniformValue("uShadowMap",15);m_program->setUniformValue("uShadowMatrix",m_shadowMatrix);m_program->setUniformValue("uShadowsEnabled",m_shadowsEnabled);m_program->setUniformValue("uShadowIntensity",m_shadowIntensity);m_program->setUniformValue("uDerivedEnvironmentShadow",m_derivedEnvironmentShadow);m_program->setUniformValue("uDerivedEnvironmentShadowWeight",m_derivedEnvironmentShadowWeight);
    m_program->setUniformValue("uPrefilteredEnvironment",12);m_program->setUniformValue("uBrdfLut",13);m_program->setUniformValue("uSpecial",14);m_program->setUniformValue("uEnvironmentSourceMaxLod",float(qMax(0,m_environmentTexture->mipLevels()-1)));m_program->setUniformValue("uPrefilteredEnvironmentMaxLod",m_prefilteredEnvironmentMaxLod);
    m_program->setUniformValue("uSecondaryBase",6);m_program->setUniformValue("uSecondaryNormal",7);m_program->setUniformValue("uSecondaryRmaos",8);m_program->setUniformValue("uSecondaryEmissive",9);m_program->setUniformValue("uLayerCoverage",10);m_program->setUniformValue("uGradient",11);
    m_program->setUniformValue("uTertiaryBase",16);m_program->setUniformValue("uTertiaryNormal",17);m_program->setUniformValue("uTertiaryRmaos",18);m_program->setUniformValue("uTertiaryEmissive",19);m_program->setUniformValue("uTertiaryCoverage",20);
    m_program->setUniformValue("uSecondaryLayerActive",m_secondaryLayerActive);m_program->setUniformValue("uTertiaryLayerActive",m_tertiaryLayerActive);m_program->setUniformValue("uTertiaryUsesUv0",m_tertiaryUsesUv0);
    m_program->setUniformValue("uHasTertiaryBase",m_hasTertiaryBase);m_program->setUniformValue("uHasTertiaryNormal",m_hasTertiaryNormal);m_program->setUniformValue("uHasTertiaryEmissive",m_hasTertiaryEmissive);m_program->setUniformValue("uHasTertiaryCoverage",m_hasTertiaryCoverage);
    m_program->setUniformValue("uOverrideTertiaryCoverage",m_overrideTertiaryCoverage);m_program->setUniformValue("uInvertTertiaryCoverage",m_invertTertiaryCoverage);m_program->setUniformValue("uTertiaryCoverageFallback",m_tertiaryCoverageFallback);m_program->setUniformValue("uTertiaryCoverageSlotBias",m_tertiaryCoverageSlotBias);m_program->setUniformValue("uTertiaryCoverageSlotContrast",m_tertiaryCoverageSlotContrast);
    m_program->setUniformValue("uOverrideTertiaryBaseColor",m_overrideTertiaryBaseColor);m_program->setUniformValue("uOverrideTertiaryOpacity",m_overrideTertiaryOpacity);m_program->setUniformValue("uOverrideTertiaryNormal",m_overrideTertiaryNormal);
    m_program->setUniformValue("uOverrideTertiaryEmissiveColor",m_overrideTertiaryEmissiveColor);m_program->setUniformValue("uOverrideTertiaryEmissiveMask",m_overrideTertiaryEmissiveMask);
    m_program->setUniformValue("uUseTertiaryRoughness",m_useTertiaryRoughness);m_program->setUniformValue("uUseTertiaryMetallic",m_useTertiaryMetallic);m_program->setUniformValue("uUseTertiaryAo",m_useTertiaryAo);m_program->setUniformValue("uUseTertiaryF0",m_useTertiaryF0);
    m_program->setUniformValue("uTertiaryBaseFallback",QVector3D(m_tertiaryBaseColor.redF(),m_tertiaryBaseColor.greenF(),m_tertiaryBaseColor.blueF()));m_program->setUniformValue("uTertiaryTint",QVector3D(m_tertiaryTint.redF(),m_tertiaryTint.greenF(),m_tertiaryTint.blueF()));m_program->setUniformValue("uTertiaryNormalFallback",QVector3D(m_tertiaryNormalColor.redF(),m_tertiaryNormalColor.greenF(),m_tertiaryNormalColor.blueF()));m_program->setUniformValue("uTertiaryEmissiveColorFallback",QVector3D(m_tertiaryEmissiveColor.redF(),m_tertiaryEmissiveColor.greenF(),m_tertiaryEmissiveColor.blueF()));
    m_program->setUniformValue("uTertiaryOpacity",m_tertiaryOpacity);m_program->setUniformValue("uTertiaryColorScale",m_tertiaryColorScale);m_program->setUniformValue("uTertiaryNormalStrength",m_tertiaryNormalStrength);
    m_program->setUniformValue("uTertiaryRoughness",m_tertiaryRoughness);m_program->setUniformValue("uTertiaryMetallic",m_tertiaryMetallic);m_program->setUniformValue("uTertiaryAo",m_tertiaryAo);m_program->setUniformValue("uTertiaryF0",m_tertiaryF0);
    m_program->setUniformValue("uTertiaryEmissiveMask",m_tertiaryEmissiveMask);m_program->setUniformValue("uTertiaryEmissiveIntensity",m_tertiaryEmissiveIntensity);
    m_program->setUniformValue("uTertiaryBlendMode",m_tertiaryBlendMode);m_program->setUniformValue("uTCoverageMode",m_tCoverageMode);m_program->setUniformValue("uTPatternSource",m_tPatternSource);
    m_program->setUniformValue("uTConstantCoverage",m_tConstantCoverage);m_program->setUniformValue("uTCoverageBias",m_tCoverageBias);m_program->setUniformValue("uTCoverageContrast",m_tCoverageContrast);m_program->setUniformValue("uTCoverageSoftness",m_tCoverageSoftness);
    m_program->setUniformValue("uUvTransform2",m_uvTransform2);m_program->setUniformValue("uUvAnimation2",m_uvAnimation2);m_program->setUniformValue("uUvRotation2",m_uvRotation2);m_program->setUniformValue("uUvAddress2",m_uvAddress2);
    m_program->setUniformValue("uPrimaryHeightInBlue",m_primaryHeightInBlue);m_program->setUniformValue("uSecondaryHeightInBlue",m_secondaryHeightInBlue);m_program->setUniformValue("uTertiaryHeightInBlue",m_tertiaryHeightInBlue);
    applyCommonUniforms(*m_program);
    const QMatrix4x4 model=objectModelMatrix();
    m_program->setUniformValue("uModel",model);m_program->setUniformValue("uModelNormal",model.normalMatrix());
    m_program->setUniformValue("uEnvironmentYaw",m_environmentYaw+m_materialEnvironmentYaw);
    m_program->setUniformValue("uEnvironmentIntensity",m_environmentIntensity*m_materialIblIntensity);
    m_program->setUniformValue("uHasBase",m_hasBase);m_program->setUniformValue("uHasNormal",m_hasNormal);m_program->setUniformValue("uHasEmissive",m_hasEmissive);
    m_program->setUniformValue("uHasSecondaryBase",m_hasSecondaryBase);m_program->setUniformValue("uHasSecondaryNormal",m_hasSecondaryNormal);m_program->setUniformValue("uHasSecondaryEmissive",m_hasSecondaryEmissive);m_program->setUniformValue("uHasLayerCoverage",m_hasLayerCoverage);
    m_program->setUniformValue("uOverrideLayerCoverage",m_overrideLayerCoverage);m_program->setUniformValue("uInvertLayerCoverage",m_invertLayerCoverage);m_program->setUniformValue("uLayerCoverageFallback",m_layerCoverageFallback);m_program->setUniformValue("uLayerCoverageSlotBias",m_layerCoverageSlotBias);m_program->setUniformValue("uLayerCoverageSlotContrast",m_layerCoverageSlotContrast);
    m_program->setUniformValue("uHasGradient",m_hasGradient);m_program->setUniformValue("uOverrideGradientColor",m_overrideGradientColor);m_program->setUniformValue("uOverrideGradientOpacity",m_overrideGradientOpacity);
    m_program->setUniformValue("uGradientColor",QVector3D(m_gradientColor.redF(),m_gradientColor.greenF(),m_gradientColor.blueF()));m_program->setUniformValue("uGradientOpacity",m_gradientOpacity);m_program->setUniformValue("uGradientIntensity",m_gradientIntensity);m_program->setUniformValue("uGradientConstant",m_gradientConstant);m_program->setUniformValue("uGradientDriver",m_gradientDriver);
    m_program->setUniformValue("uUnlitIntensity",m_unlitIntensity);m_program->setUniformValue("uUnlitVertexColor",m_unlitVertexColor);m_program->setUniformValue("uUnlitExposure",m_unlitExposure);m_program->setUniformValue("uAlphaSource",m_alphaSource);
    m_program->setUniformValue("uOverrideBaseColor",m_overrideBaseColor);m_program->setUniformValue("uOverrideOpacity",m_overrideOpacity);
    m_program->setUniformValue("uOverrideNormal",m_overrideNormal);m_program->setUniformValue("uOverrideEmissiveColor",m_overrideEmissiveColor);m_program->setUniformValue("uOverrideEmissiveMask",m_overrideEmissiveMask);
    m_program->setUniformValue("uOverrideSecondaryBaseColor",m_overrideSecondaryBaseColor);m_program->setUniformValue("uOverrideSecondaryOpacity",m_overrideSecondaryOpacity);m_program->setUniformValue("uOverrideSecondaryNormal",m_overrideSecondaryNormal);
    m_program->setUniformValue("uOverrideSecondaryEmissiveColor",m_overrideSecondaryEmissiveColor);m_program->setUniformValue("uOverrideSecondaryEmissiveMask",m_overrideSecondaryEmissiveMask);
    m_program->setUniformValue("uUseRoughness",m_useRoughness);m_program->setUniformValue("uUseMetallic",m_useMetallic);m_program->setUniformValue("uUseAo",m_useAo);m_program->setUniformValue("uUseF0",m_useF0);
    m_program->setUniformValue("uUseSecondaryRoughness",m_useSecondaryRoughness);m_program->setUniformValue("uUseSecondaryMetallic",m_useSecondaryMetallic);m_program->setUniformValue("uUseSecondaryAo",m_useSecondaryAo);m_program->setUniformValue("uUseSecondaryF0",m_useSecondaryF0);
    m_program->setUniformValue("uBaseFallback",QVector3D(m_baseColor.redF(),m_baseColor.greenF(),m_baseColor.blueF()));m_program->setUniformValue("uTint",QVector3D(m_tint.redF(),m_tint.greenF(),m_tint.blueF()));m_program->setUniformValue("uNormalFallback",QVector3D(m_normalColor.redF(),m_normalColor.greenF(),m_normalColor.blueF()));m_program->setUniformValue("uEmissiveColor",QVector3D(m_emissiveColor.redF(),m_emissiveColor.greenF(),m_emissiveColor.blueF()));
    m_program->setUniformValue("uSecondaryBaseFallback",QVector3D(m_secondaryBaseColor.redF(),m_secondaryBaseColor.greenF(),m_secondaryBaseColor.blueF()));m_program->setUniformValue("uSecondaryTint",QVector3D(m_secondaryTint.redF(),m_secondaryTint.greenF(),m_secondaryTint.blueF()));m_program->setUniformValue("uSecondaryNormalFallback",QVector3D(m_secondaryNormalColor.redF(),m_secondaryNormalColor.greenF(),m_secondaryNormalColor.blueF()));m_program->setUniformValue("uSecondaryEmissiveColor",QVector3D(m_secondaryEmissiveColor.redF(),m_secondaryEmissiveColor.greenF(),m_secondaryEmissiveColor.blueF()));
    m_program->setUniformValue("uKeyColor",QVector3D(m_keyColor.redF(),m_keyColor.greenF(),m_keyColor.blueF()));m_program->setUniformValue("uKeyDirection",keyDirection());m_program->setUniformValue("uKeyIntensity",effectiveKeyIntensity());
    m_program->setUniformValue("uOpacity",m_opacity);m_program->setUniformValue("uColorScale",m_colorScale);m_program->setUniformValue("uNormalStrength",m_normalStrength);m_program->setUniformValue("uRoughness",m_roughness);m_program->setUniformValue("uMetallic",m_metallic);m_program->setUniformValue("uAo",m_ao);m_program->setUniformValue("uF0",m_f0);m_program->setUniformValue("uEmissiveMask",m_emissiveMask);m_program->setUniformValue("uEmissiveIntensity",m_emissiveIntensity);
    m_program->setUniformValue("uSecondaryOpacity",m_secondaryOpacity);m_program->setUniformValue("uSecondaryColorScale",m_secondaryColorScale);m_program->setUniformValue("uSecondaryNormalStrength",m_secondaryNormalStrength);
    m_program->setUniformValue("uSecondaryRoughness",m_secondaryRoughness);m_program->setUniformValue("uSecondaryMetallic",m_secondaryMetallic);m_program->setUniformValue("uSecondaryAo",m_secondaryAo);m_program->setUniformValue("uSecondaryF0",m_secondaryF0);
    m_program->setUniformValue("uSecondaryEmissiveMask",m_secondaryEmissiveMask);m_program->setUniformValue("uSecondaryEmissiveIntensity",m_secondaryEmissiveIntensity);
    m_program->setUniformValue("uSecondaryBlendMode",m_secondaryBlendMode);m_program->setUniformValue("uCoverageMode",m_coverageMode);m_program->setUniformValue("uPatternSource",m_patternSource);
    m_program->setUniformValue("uConstantCoverage",m_constantCoverage);m_program->setUniformValue("uCoverageBias",m_coverageBias);m_program->setUniformValue("uCoverageContrast",m_coverageContrast);m_program->setUniformValue("uCoverageSoftness",m_coverageSoftness);
    m_program->setUniformValueArray("uUvSourceMode",m_uvSourceMode,3);
    m_program->setUniformValueArray("uUvProjAxis",m_uvProjAxis,3);
    m_program->setUniformValueArray("uTriplanarSharp",m_triplanarSharp,3,1);
    m_program->setUniformValueArray("uOscWave",m_oscWave,3);
    m_program->setUniformValueArray("uOscFreq",m_oscFreq,3,1);
    m_program->setUniformValueArray("uOscPhase",m_oscPhase,3,1);
    m_program->setUniformValueArray("uOscDuty",m_oscDuty,3,1);
    m_program->setUniformValueArray("uOscScaleAmp",m_oscScaleAmp,3,1);
    m_program->setUniformValueArray("uOscRotAmp",m_oscRotAmp,3,1);
    m_program->setUniformValueArray("uOscEmissiveAmp",m_oscEmissiveAmp,3,1);
    m_program->setUniformValueArray("uOscCoverageAmp",m_oscCoverageAmp,3,1);
    m_program->setUniformValueArray("uOscNormalAmp",m_oscNormalAmp,3,1);
    m_program->setUniformValueArray("uOscOffsetAmp",m_oscOffsetAmp,3);
    m_program->setUniformValue("uUvTransform0",m_uvTransform0);m_program->setUniformValue("uUvTransform1",m_uvTransform1);
    m_program->setUniformValue("uUvAnimation0",m_uvAnimation0);m_program->setUniformValue("uUvAnimation1",m_uvAnimation1);
    m_program->setUniformValue("uTime",float(m_previewTime));
    m_program->setUniformValue("uUvRotation0",m_uvRotation0);m_program->setUniformValue("uUvRotation1",m_uvRotation1);
    m_program->setUniformValue("uUvAddress0",m_uvAddress0);m_program->setUniformValue("uUvAddress1",m_uvAddress1);m_program->setUniformValue("uMirrorPrimaryUv",m_mirrorPrimaryUv);
    m_program->setUniformValue("uCompositionMode",m_compositionMode);m_program->setUniformValue("uGlobalOpacity",m_globalOpacity);m_program->setUniformValue("uAlphaThreshold",m_alphaThreshold);
    m_program->setUniformValue("uNormalSpace",m_normalSpace);m_program->setUniformValue("uGlobalNormalStrength",m_globalNormalStrength);
    m_program->setUniformValue("uDiffuseScale",m_materialDiffuseScale);m_program->setUniformValue("uSpecularScale",m_materialSpecularScale);
    m_program->setUniformValue("uHasHeight",m_hasHeight);m_program->setUniformValue("uOverrideHeight",m_overrideHeight);m_program->setUniformValue("uHeightBlue",m_heightBlueChannel);m_program->setUniformValue("uHeightInvert",m_heightInvert);
    m_program->setUniformValue("uHeightUvSet",m_heightUvSet);m_program->setUniformValue("uHeightFallback",m_heightFallback);m_program->setUniformValue("uHeightCenter",m_heightCenter);m_program->setUniformValue("uHeightBias",m_heightBias);
    m_program->setUniformValue("uParallaxMode",m_parallaxEnabled?m_parallaxMode:0);m_program->setUniformValue("uParallaxMinSteps",m_parallaxMinSteps);m_program->setUniformValue("uParallaxMaxSteps",m_parallaxMaxSteps);
    m_program->setUniformValue("uParallaxRefinementSteps",m_parallaxRefinementSteps);m_program->setUniformValue("uParallaxShadowSteps",m_parallaxShadowSteps);m_program->setUniformValue("uParallaxClip",m_parallaxClip);
    m_program->setUniformValue("uParallaxDepth",m_parallaxDepth);m_program->setUniformValue("uParallaxGrazingFade",m_parallaxGrazingFade);m_program->setUniformValue("uParallaxShadowStrength",m_parallaxShadowStrength);
    m_program->setUniformValue("uParallaxShadowBias",m_parallaxShadowBias);m_program->setUniformValue("uParallaxShadowSoftness",m_parallaxShadowSoftness);m_program->setUniformValue("uParallaxFadeStart",m_parallaxFadeStart);m_program->setUniformValue("uParallaxFadeEnd",m_parallaxFadeEnd);
    m_program->setUniformValue("uUvSet",m_uvSet);m_program->setUniformValue("uDebugView",int(m_debugView));m_program->setUniformValue("uCompare",m_comparisonEnabled);m_program->setUniformValue("uViewportWidth",float(width()*devicePixelRatioF()));
    const int shaderMode=int(m_document.shaderMode());m_program->setUniformValue("uShaderMode",shaderMode);m_program->setUniformValue("uSpecialMask",m_specialMask);
    const int soloSet=m_uvSet<=2?m_uvSet:0;
    const QString activeSet=soloSet==1?QStringLiteral("UV1"):(soloSet==2?QStringLiteral("UV2"):QStringLiteral("UV0")),activePrefix=soloSet==1?QStringLiteral("secondary"):(soloSet==2?QStringLiteral("tertiary"):QStringLiteral("primary"));
    const auto slot=[&](const QString& key,const QString& setName=QString(),const QString& prefixName=QString()){
        return m_document.textureSlot(setName.isEmpty()?activeSet:setName,(prefixName.isEmpty()?activePrefix:prefixName)+key);
    };
    const auto settingNumber=[&](const QString& section,const QString& key,float fallback){return float(m_document.setting(section,key,fallback).toDouble(fallback));};
    const auto settingBool=[&](const QString& section,const QString& key,bool fallback){return m_document.setting(section,key,fallback).toBool(fallback);};
    const auto setColorUniform=[&](const char* name,const QJsonObject& values,const QString& key,const QString& fallback){m_program->setUniformValue(name,jsonColor(values,key,fallback));};
    const auto tintState=slot(QStringLiteral("TintMask"));
    setColorUniform("uTintColorR",tintState.values,"colorR","#FFFFFF");setColorUniform("uTintColorG",tintState.values,"colorG","#FFFFFF");setColorUniform("uTintColorB",tintState.values,"colorB","#FFFFFF");setColorUniform("uTintColorA",tintState.values,"colorA","#FFFFFF");
    const QString tintOverlap=tintState.values.value(QStringLiteral("overlap")).toString(QStringLiteral("Normalize"));m_program->setUniformValue("uTintOverlap",tintOverlap==QStringLiteral("Add")?1:(tintOverlap==QStringLiteral("Priority RGBA")?2:0));
    const QString dynamicSection=soloSet==1?QStringLiteral("DynamicNormals/Secondary"):(soloSet==2?QStringLiteral("DynamicNormals/Tertiary"):QStringLiteral("DynamicNormals/Primary"));const QJsonObject dynamic=m_document.settingGroup(dynamicSection);const QString dynamicSource=dynamic.value(QStringLiteral("source")).toString(QStringLiteral("Facial Expression"));
    m_program->setUniformValue("uDynamicDriverSource",dynamicSource==QStringLiteral("Animation Parameter")?1:(dynamicSource==QStringLiteral("Vertex Value")?2:(dynamicSource==QStringLiteral("Constant")?3:0)));
    const QString dynamicBlend=dynamic.value(QStringLiteral("blend")).toString(QStringLiteral("Reoriented"));m_program->setUniformValue("uDynamicBlend",dynamicBlend==QStringLiteral("Whiteout")?1:(dynamicBlend==QStringLiteral("Linear")?2:0));
    m_program->setUniformValue("uDynamicDriverValue",float(dynamic.value(QStringLiteral("constantValue")).toDouble(1.0)));m_program->setUniformValue("uDynamicInputMin",float(dynamic.value(QStringLiteral("minimum")).toDouble(0.0)));m_program->setUniformValue("uDynamicInputMax",float(dynamic.value(QStringLiteral("maximum")).toDouble(1.0)));m_program->setUniformValue("uDynamicInvert",dynamic.value(QStringLiteral("invert")).toBool(false));m_program->setUniformValue("uDynamicStrength",numberValue(slot(QStringLiteral("DynamicNormal")).values,QStringLiteral("strength"),1.0f));
    const QJsonObject subsurface=m_document.settingGroup(QStringLiteral("Shading/Subsurface"));const QString subsurfaceMode=subsurface.value(QStringLiteral("mode")).toString(QStringLiteral("Disabled"));m_program->setUniformValue("uSubsurfaceMode",subsurfaceMode==QStringLiteral("Diffusion Profile")?1:(subsurfaceMode==QStringLiteral("Thin Transmission")?2:(subsurfaceMode==QStringLiteral("Combined")?3:0)));
    const QString profile=subsurface.value(QStringLiteral("profile")).toString(QStringLiteral("Skin"));QVector3D profileColor=profile==QStringLiteral("Wax")?QVector3D(1,.72,.4):(profile==QStringLiteral("Flesh")?QVector3D(1,.3,.22):(profile==QStringLiteral("Plant")?QVector3D(.42,.8,.28):(profile==QStringLiteral("Custom")?jsonColor(subsurface,QStringLiteral("customScatterColor"),QStringLiteral("#FFB39D")):QVector3D(1,.55,.42))));m_program->setUniformValue("uSubsurfaceProfileColor",profileColor);
    m_program->setUniformValue("uSubsurfaceWeight",float(subsurface.value(QStringLiteral("weight")).toDouble(1.0)));m_program->setUniformValue("uSubsurfaceTransmission",float(subsurface.value(QStringLiteral("transmission")).toDouble(0.0)));m_program->setUniformValue("uTransmissionTint",jsonColor(subsurface,QStringLiteral("transmissionTint"),QStringLiteral("#FFFFFF")));m_program->setUniformValue("uSubsurfaceThickness",float(subsurface.value(QStringLiteral("thicknessScale")).toDouble(1.0))*numberValue(slot(QStringLiteral("Subsurface")).values,QStringLiteral("thickness"),1.0f));
    m_program->setUniformValue("uSheenStrength",settingNumber(QStringLiteral("Shading/SurfaceEffects"),QStringLiteral("sheenStrength"),0));m_program->setUniformValue("uSheenRoughness",settingNumber(QStringLiteral("Shading/SurfaceEffects"),QStringLiteral("sheenRoughness"),.6f));m_program->setUniformValue("uGlintIntensity",settingNumber(QStringLiteral("Shading/SurfaceEffects"),QStringLiteral("glintIntensity"),0));m_program->setUniformValue("uGlintScale",settingNumber(QStringLiteral("Shading/SurfaceEffects"),QStringLiteral("glintScale"),1));m_program->setUniformValue("uFilmIor",settingNumber(QStringLiteral("Shading/SurfaceEffects"),QStringLiteral("filmIor"),1.5f));m_program->setUniformValue("uIridescenceStrength",settingNumber(QStringLiteral("Shading/SurfaceEffects"),QStringLiteral("iridescenceStrength"),0));
    const QJsonObject hair=m_document.settingGroup(QStringLiteral("Shading/Hair"));m_program->setUniformValue("uHairPrimaryStrength",float(hair.value("primaryStrength").toDouble(1)));m_program->setUniformValue("uHairPrimaryRoughness",float(hair.value("primaryRoughness").toDouble(.25)));m_program->setUniformValue("uHairPrimaryShift",float(hair.value("primaryShift").toDouble(-.08)));m_program->setUniformValue("uHairSecondaryStrength",float(hair.value("secondaryStrength").toDouble(.5)));m_program->setUniformValue("uHairSecondaryRoughness",float(hair.value("secondaryRoughness").toDouble(.45)));m_program->setUniformValue("uHairSecondaryShift",float(hair.value("secondaryShift").toDouble(.12)));m_program->setUniformValue("uHairScatter",float(hair.value("scatter").toDouble(.5)));m_program->setUniformValue("uHairBacklight",float(hair.value("backlight").toDouble(.5)));m_program->setUniformValue("uHairDirectionStrength",numberValue(slot(QStringLiteral("HairDirection")).values,QStringLiteral("strength"),1));
    const auto transmissionState=slot(QStringLiteral("TransmissionAbsorption"));m_program->setUniformValue("uGlassIor",settingNumber(QStringLiteral("Shading/Glass"),QStringLiteral("ior"),1.5f));m_program->setUniformValue("uGlassAbsorptionDistance",settingNumber(QStringLiteral("Shading/Glass"),QStringLiteral("absorptionDistance"),1));m_program->setUniformValue("uGlassRefractionRoughness",settingNumber(QStringLiteral("Shading/Glass"),QStringLiteral("refractionRoughness"),0));m_program->setUniformValue("uGlassThinWalled",settingBool(QStringLiteral("Shading/Glass"),QStringLiteral("thinWalled"),false));m_program->setUniformValue("uGlassThickness",numberValue(transmissionState.values,QStringLiteral("thickness"),1));
    const auto waterFlow=slot(QStringLiteral("WaterFlow"));m_program->setUniformValue("uWaterIor",settingNumber(QStringLiteral("Shading/Water"),QStringLiteral("ior"),1.333f));m_program->setUniformValue("uWaterAbsorptionDistance",settingNumber(QStringLiteral("Shading/Water"),QStringLiteral("absorptionDistance"),300));m_program->setUniformValue("uWaterScattering",settingNumber(QStringLiteral("Shading/Water"),QStringLiteral("scattering"),.25f));m_program->setUniformValue("uWaterReflectionStrength",settingNumber(QStringLiteral("Shading/Water"),QStringLiteral("reflectionStrength"),1));m_program->setUniformValue("uWaterShoreFoam",settingBool(QStringLiteral("Shading/Water"),QStringLiteral("shoreFoam"),true));m_program->setUniformValue("uWaterFlowSpeed",numberValue(waterFlow.values,QStringLiteral("speed"),.05f));m_program->setUniformValue("uWaterFlowStrength",numberValue(waterFlow.values,QStringLiteral("strength"),1));
    m_program->setUniformValue("uEyeCorneaIor",settingNumber(QStringLiteral("Shading/Eye"),QStringLiteral("corneaIor"),1.376f));m_program->setUniformValue("uEyeIrisDepth",settingNumber(QStringLiteral("Shading/Eye"),QStringLiteral("irisDepth"),.08f));m_program->setUniformValue("uEyeLimbalStrength",settingNumber(QStringLiteral("Shading/Eye"),QStringLiteral("limbalStrength"),1));m_program->setUniformValue("uEyePupilScale",settingNumber(QStringLiteral("Shading/Eye"),QStringLiteral("pupilScale"),1));m_program->setUniformValue("uEyeWetness",settingNumber(QStringLiteral("Shading/Eye"),QStringLiteral("wetness"),1));
    {
        const char* coatSets[3]={"UV0","UV1","UV2"};const char* coatPrefixes[3]={"primary","secondary","tertiary"};
        GLfloat coatStrength[3],coatIor[3],coatNormalStrength[3],coatAbsorption[3];QVector3D coatTint[3];
        const GLint coatLayers[3]={ClearcoatLayer,ClearcoatSecondaryLayer,ClearcoatTertiaryLayer};
        for(int ci=0;ci<3;++ci){
            const auto state=m_document.textureSlot(QString::fromLatin1(coatSets[ci]),QString::fromLatin1(coatPrefixes[ci])+QStringLiteral("Clearcoat"));
            coatStrength[ci]=numberValue(state.values,QStringLiteral("strength"),1);
            coatIor[ci]=numberValue(state.values,QStringLiteral("ior"),1.5f);
            coatNormalStrength[ci]=numberValue(state.values,QStringLiteral("normalStrength"),1);
            coatAbsorption[ci]=numberValue(state.values,QStringLiteral("absorption"),1);
            coatTint[ci]=jsonColor(state.values,QStringLiteral("tint"),QStringLiteral("#FFFFFF"));
        }
        m_program->setUniformValueArray("uCoatStrength",coatStrength,3,1);
        m_program->setUniformValueArray("uCoatIor",coatIor,3,1);
        m_program->setUniformValueArray("uCoatNormalStrength",coatNormalStrength,3,1);
        m_program->setUniformValueArray("uCoatAbsorption",coatAbsorption,3,1);
        m_program->setUniformValueArray("uCoatTint",coatTint,3);
        m_program->setUniformValueArray("uCoatSpecialLayer",coatLayers,3);
    }
    m_program->setUniformValue("uDistortionVectorScale",numberValue(slot(QStringLiteral("ScreenDistortion")).values,QStringLiteral("vectorScale"),1));
    const auto anisotropy=slot(QStringLiteral("Anisotropy"));m_program->setUniformValue("uAnisotropyLayer",int(AnisotropyLayer));m_program->setUniformValue("uAnisotropyIntensity",numberValue(anisotropy.values,QStringLiteral("intensity"),1));m_program->setUniformValue("uAnisotropyRotation",numberValue(anisotropy.values,QStringLiteral("rotation"),0));
    m_program->setUniformValue("uBentNormalLayer",int(BentNormalLayer));m_program->setUniformValue("uBentNormalStrength",numberValue(slot(QStringLiteral("BentNormal")).values,QStringLiteral("strength"),1));
    const auto detailNormal=slot(QStringLiteral("DetailNormal"));
    m_program->setUniformValue("uDetailNormalLayer",int(DetailNormalLayer));
    m_program->setUniformValue("uDetailNormalStrength",numberValue(detailNormal.values,QStringLiteral("strength"),1));
    m_program->setUniformValue("uDetailNormalScale",numberValue(detailNormal.values,QStringLiteral("uvScale"),4));
    m_program->setUniformValue("uDetailRoughVariation",numberValue(detailNormal.values,QStringLiteral("roughVariation"),.5f));
    const auto landscapeNormalArray=m_document.textureSlot(QStringLiteral("Landscape"),QStringLiteral("normalHeightArray"));
    m_program->setUniformValue("uLandscapeUsePorosityTexture",!boolValue(landscapeNormalArray.values,QStringLiteral("overridePorosity"),true));
    m_program->setUniformValue("uLandscapePorosity",numberValue(landscapeNormalArray.values,QStringLiteral("porosity"),.5f));
    m_program->setUniformValue("uSpecularAa",settingBool(QStringLiteral("Shading/Core"),QStringLiteral("specularAa"),true));m_program->setUniformValue("uSpecularAaStrength",settingNumber(QStringLiteral("Shading/Core"),QStringLiteral("specularAaStrength"),1.0f));
    m_program->setUniformValue("uRetroStrength",settingNumber(QStringLiteral("Shading/SurfaceEffects"),QStringLiteral("retroStrength"),0.0f));m_program->setUniformValue("uRetroWidth",settingNumber(QStringLiteral("Shading/SurfaceEffects"),QStringLiteral("retroWidth"),.25f));
    const QJsonObject decal=m_document.settingGroup(QStringLiteral("Decal"));const QString decalBlend=decal.value(QStringLiteral("blend")).toString(QStringLiteral("Normal"));m_program->setUniformValue("uDecalEnabled",decal.value(QStringLiteral("enabled")).toBool(false));m_program->setUniformValue("uDecalBlend",decalBlend==QStringLiteral("Modulate")?1:(decalBlend==QStringLiteral("Additive")?2:0));m_program->setUniformValue("uDecalAngleFade",float(decal.value(QStringLiteral("angleFade")).toDouble(.25)));m_program->setUniformValue("uDecalDistanceFade",float(decal.value(QStringLiteral("distanceFade")).toDouble(0)));
    const QJsonObject decalChannels=m_document.settingGroup(QStringLiteral("Decal/Channels"));m_program->setUniformValue("uDecalBaseColor",decalChannels.value(QStringLiteral("baseColor")).toBool(true));m_program->setUniformValue("uDecalNormal",decalChannels.value(QStringLiteral("normal")).toBool(true));m_program->setUniformValue("uDecalRoughness",decalChannels.value(QStringLiteral("roughness")).toBool(true));m_program->setUniformValue("uDecalMetallic",decalChannels.value(QStringLiteral("metallic")).toBool(false));m_program->setUniformValue("uDecalAo",decalChannels.value(QStringLiteral("ao")).toBool(false));m_program->setUniformValue("uDecalEmissive",decalChannels.value(QStringLiteral("emissive")).toBool(false));
    const QJsonObject displacement=m_document.settingGroup(QStringLiteral("Geometry/Displacement"));m_program->setUniformValue("uDisplacementEnabled",displacement.value(QStringLiteral("enabled")).toBool(false));m_program->setUniformValue("uDisplacementStrength",float(displacement.value(QStringLiteral("strength")).toDouble(0)));m_program->setUniformValue("uDisplacementFadeEnd",float(displacement.value(QStringLiteral("fadeEnd")).toDouble(1024)));
    const QJsonObject vegetationProfile=m_document.settingGroup(QStringLiteral("Vegetation/Profile")),vegetationMotion=m_document.settingGroup(QStringLiteral("Vegetation/Motion"));m_program->setUniformValue("uVegetationEnabled",shaderMode==int(Pbr::ShaderMode::VegetationSurface));m_program->setUniformValue("uWindDirection",float(vegetationProfile.value(QStringLiteral("direction")).toDouble(35)));m_program->setUniformValue("uWindStrength",float(vegetationProfile.value(QStringLiteral("strength")).toDouble(1)));m_program->setUniformValue("uWindTimeScale",float(vegetationProfile.value(QStringLiteral("timeScale")).toDouble(1)));m_program->setUniformValue("uSwayAmount",float(vegetationMotion.value(QStringLiteral("swayAmount")).toDouble(1)));m_program->setUniformValue("uSwaySpeed",float(vegetationMotion.value(QStringLiteral("swaySpeed")).toDouble(1)));m_program->setUniformValue("uBranchAmount",float(vegetationMotion.value(QStringLiteral("branchAmount")).toDouble(.35)));m_program->setUniformValue("uFlutterAmount",float(vegetationMotion.value(QStringLiteral("flutterAmount")).toDouble(.2)));m_program->setUniformValue("uFlutterSpeed",float(vegetationMotion.value(QStringLiteral("flutterSpeed")).toDouble(4)));m_program->setUniformValue("uGustStrength",float(vegetationMotion.value(QStringLiteral("gustStrength")).toDouble(.4)));m_program->setUniformValue("uTurbulence",float(vegetationMotion.value(QStringLiteral("turbulence")).toDouble(.5)));
    const auto landscapeMacro=m_document.textureSlot(QStringLiteral("Landscape"),QStringLiteral("macroVariation"));m_program->setUniformValue("uLandscapeMacroScale",numberValue(landscapeMacro.values,QStringLiteral("scale"),2048));m_program->setUniformValue("uLandscapeMacroColorStrength",numberValue(landscapeMacro.values,QStringLiteral("colorStrength"),.15f));m_program->setUniformValue("uLandscapeMacroRoughnessStrength",numberValue(landscapeMacro.values,QStringLiteral("roughnessStrength"),.1f));
    m_program->setUniformValue("uAntiTiling",settingBool(QStringLiteral("Landscape/AntiTiling"),QStringLiteral("enabled"),false));m_program->setUniformValue("uAntiTileScale",settingNumber(QStringLiteral("Landscape/AntiTiling"),QStringLiteral("patchScale"),4.0f));m_program->setUniformValue("uAntiTileContrast",settingNumber(QStringLiteral("Landscape/AntiTiling"),QStringLiteral("blendContrast"),4.0f));
    m_program->setUniformValue("uStateMasksLayer",int(StateMasksLayer));
    // v5 material-side surface response: physical inputs and bindings only.
    m_program->setUniformValue("uStateAdhesion",m_stateAdhesion);
    m_program->setUniformValue("uShelterChannel",m_shelterChannel);
    m_program->setUniformValue("uCurvatureInAlpha",m_curvatureInAlpha);
    m_program->setUniformValue("uCavitySpecOcclusion",m_cavitySpecOcclusion);
    m_program->setUniformValue("uRmaosAlphaPorosity",m_rmaosAlphaPorosity&&m_hasRmaos);
    m_program->setUniformValue("uPorosityOverride",m_porosityOverride);
    m_program->setUniformValue("uPorosityConstant",m_porosityConstant);
    m_program->setUniformValue("uSssCurvatureThickness",m_curvatureInAlpha&&!m_sssAlphaAuthored);
    {
        constexpr int MaxStates=Pbr::MaxSurfaceStates;
        GLfloat amount[MaxStates]={},desat[MaxStates]={},porosityInfluence[MaxStates]={},roughTarget[MaxStates]={},roughWeight[MaxStates]={};
        GLfloat f0Target[MaxStates]={},f0Weight[MaxStates]={},filmF0[MaxStates]={},sparkle[MaxStates]={},glowIntensity[MaxStates]={};
        GLfloat cavityBias[MaxStates]={},threshold[MaxStates]={},softness[MaxStates]={},directionality[MaxStates]={},streaking[MaxStates]={},edgeShedding[MaxStates]={},retention[MaxStates]={};
        GLfloat aoBias[MaxStates]={},heightBias[MaxStates]={},roughBias[MaxStates]={},metalSeal[MaxStates]={},emissiveDry[MaxStates]={};
        GLint blendMode[MaxStates]={},roughMode[MaxStates]={},killMetallic[MaxStates]={},ripples[MaxStates]={},maskChannel[MaxStates],respectsShelter[MaxStates]={};
        QVector3D color[MaxStates];
        for(int i=0;i<MaxStates;++i)maskChannel[i]=-1;
        for(int i=0;i<m_surfaceStates.size()&&i<MaxStates;++i){
            const auto& state=m_surfaceStates[i];
            amount[i]=i<m_stateAmounts.size()?std::clamp(m_stateAmounts[i],0.0f,1.0f):0.0f;
            const QColor stateColor=QColor(state.color).isValid()?QColor(state.color):QColor(Qt::white);
            color[i]=QVector3D(stateColor.redF(),stateColor.greenF(),stateColor.blueF());
            blendMode[i]=state.blendMode;desat[i]=float(state.desaturation);porosityInfluence[i]=float(state.porosityInfluence);
            roughTarget[i]=float(state.roughnessTarget);roughWeight[i]=float(state.roughnessWeight);roughMode[i]=state.roughnessMode;
            f0Target[i]=float(state.f0Target);f0Weight[i]=float(state.f0Weight);filmF0[i]=float(state.filmF0);
            killMetallic[i]=state.killMetallic?1:0;sparkle[i]=float(state.sparkle);glowIntensity[i]=float(state.glowIntensity);
            cavityBias[i]=float(state.cavityBias);threshold[i]=float(state.threshold);softness[i]=float(state.softness);
            directionality[i]=float(state.directionality);streaking[i]=float(state.streaking);edgeShedding[i]=float(state.edgeShedding);retention[i]=float(state.retention);ripples[i]=state.ripples?1:0;
            aoBias[i]=float(state.aoBias);heightBias[i]=float(state.heightBias);roughBias[i]=float(state.roughnessBias);metalSeal[i]=float(state.metalSeal);emissiveDry[i]=float(state.emissiveDry);
            maskChannel[i]=state.maskChannel;respectsShelter[i]=state.respectsShelter?1:0;
        }
        m_program->setUniformValueArray("uStateAmount",amount,MaxStates,1);
        m_program->setUniformValueArray("uStateDesat",desat,MaxStates,1);
        m_program->setUniformValueArray("uStatePorosityInfluence",porosityInfluence,MaxStates,1);
        m_program->setUniformValueArray("uStateRoughTarget",roughTarget,MaxStates,1);
        m_program->setUniformValueArray("uStateRoughWeight",roughWeight,MaxStates,1);
        m_program->setUniformValueArray("uStateF0Target",f0Target,MaxStates,1);
        m_program->setUniformValueArray("uStateF0Weight",f0Weight,MaxStates,1);
        m_program->setUniformValueArray("uStateFilmF0",filmF0,MaxStates,1);
        m_program->setUniformValueArray("uStateSparkle",sparkle,MaxStates,1);
        m_program->setUniformValueArray("uStateGlowIntensity",glowIntensity,MaxStates,1);
        m_program->setUniformValueArray("uStateCavityBias",cavityBias,MaxStates,1);
        m_program->setUniformValueArray("uStateThreshold",threshold,MaxStates,1);
        m_program->setUniformValueArray("uStateSoftness",softness,MaxStates,1);
        m_program->setUniformValueArray("uStateDirectionality",directionality,MaxStates,1);
        m_program->setUniformValueArray("uStateStreaking",streaking,MaxStates,1);
        m_program->setUniformValueArray("uStateEdgeShedding",edgeShedding,MaxStates,1);
        m_program->setUniformValueArray("uStateRetention",retention,MaxStates,1);
        m_program->setUniformValueArray("uStateAoBias",aoBias,MaxStates,1);
        m_program->setUniformValueArray("uStateHeightBias",heightBias,MaxStates,1);
        m_program->setUniformValueArray("uStateRoughBias",roughBias,MaxStates,1);
        m_program->setUniformValueArray("uStateMetalSeal",metalSeal,MaxStates,1);
        m_program->setUniformValueArray("uStateEmissiveDry",emissiveDry,MaxStates,1);
        m_program->setUniformValueArray("uStateBlendMode",blendMode,MaxStates);
        m_program->setUniformValueArray("uStateRoughMode",roughMode,MaxStates);
        m_program->setUniformValueArray("uStateKillMetallic",killMetallic,MaxStates);
        m_program->setUniformValueArray("uStateRipples",ripples,MaxStates);
        m_program->setUniformValueArray("uStateMaskChannel",maskChannel,MaxStates);
        m_program->setUniformValueArray("uStateRespectsShelter",respectsShelter,MaxStates);
        m_program->setUniformValueArray("uStateColor",color,MaxStates);
    }
    {
        m_program->setUniformValue("uParticleActive",m_particleActive);
        m_program->setUniformValueArray("uFlipbookEnabled",m_flipbookEnabled,3);
        m_program->setUniformValueArray("uFlipbookGrid",m_flipbookGrid,3);
        m_program->setUniformValueArray("uFlipbookDriver",m_flipbookDriver,3);
        m_program->setUniformValueArray("uFlipbookFps",m_flipbookFps,3,1);
        m_program->setUniformValueArray("uFlipbookLoop",m_flipbookLoop,3);
        m_program->setUniformValueArray("uFlipbookBlendOn",m_flipbookBlend,3);
        m_program->setUniformValueArray("uFlipbookConstFrame",m_flipbookConstantFrame,3,1);
        m_program->setUniformValueArray("uPolarUvOn",m_polarUv,3);
        m_program->setUniformValueArray("uLayerRole",m_layerRole,3);
        m_program->setUniformValueArray("uDistortStrength",m_distortStrength,3,1);
        m_program->setUniformValueArray("uAdvectSpeed",m_advectSpeed,3,1);
        m_program->setUniformValueArray("uErodeOn",m_erodeEnabled,3);
        m_program->setUniformValueArray("uErodeSoftness",m_erodeSoftness,3,1);
        m_program->setUniformValueArray("uErodeTint",m_erodeEdgeTint,3);
        m_program->setUniformValueArray("uErodeGlow",m_erodeEdgeGlow,3,1);
        m_program->setUniformValue("uPaletteMode",m_paletteMode);m_program->setUniformValue("uPaletteIntensity",m_paletteIntensity);
        m_program->setUniformValue("uRandomFlipStart",m_randomFlipbookStart);m_program->setUniformValue("uRandomPhase",m_randomPhase);m_program->setUniformValue("uRandomHue",m_randomHueJitter);
        m_program->setUniformValue("uParticleFresnelFade",m_particleFresnelFade);m_program->setUniformValue("uParticleFresnelPower",m_particleFresnelPower);m_program->setUniformValue("uParticleNearFade",m_particleNearFade);
        const float particleAge=m_particleAgeAuto?float(std::fmod(m_previewTime*double(m_particleAgeRate),1.0)):m_particleAge;
        m_program->setUniformValue("uParticleAge",particleAge);m_program->setUniformValue("uParticleSeed",m_particleSeed);
        m_program->setUniformValue("uSixWayALayer",int(SixWayALayer));m_program->setUniformValue("uSixWayBLayer",int(SixWayBLayer));
        const auto sixWay=slot(QStringLiteral("SixWayA"));
        m_program->setUniformValue("uSixWayAmbient",numberValue(sixWay.values,QStringLiteral("ambient"),1));
        m_program->setUniformValue("uSixWayIntensity",numberValue(sixWay.values,QStringLiteral("intensity"),1));
        const auto sixWayB=slot(QStringLiteral("SixWayB"));
        m_program->setUniformValue("uSixWayFireIntensity",numberValue(sixWayB.values,QStringLiteral("fireIntensity"),0));
        m_program->setUniformValue("uMotionVectorsLayer",int(MotionVectorsLayer));
        m_program->setUniformValue("uMotionStrength",numberValue(slot(QStringLiteral("MotionVectors")).values,QStringLiteral("strength"),1));
    }
    const auto secondaryGradient=m_document.textureSlot(QStringLiteral("UV1"),QStringLiteral("secondaryGradient"));const QString secondaryGradientDriver=secondaryGradient.values.value(QStringLiteral("driver")).toString(QStringLiteral("Coverage to U"));m_program->setUniformValue("uSecondaryGradientDriver",secondaryGradientDriver==QStringLiteral("Vertex Color to U")?1:(secondaryGradientDriver==QStringLiteral("Constant")?2:0));m_program->setUniformValue("uSecondaryGradientIntensity",numberValue(secondaryGradient.values,QStringLiteral("intensity"),1));m_program->setUniformValue("uSecondaryGradientConstant",numberValue(secondaryGradient.values,QStringLiteral("constantDriver"),.5f));
    const bool meshHasVertexColors=std::any_of(m_mesh.vertices.cbegin(),m_mesh.vertices.cend(),[](const PreviewVertex& vertex){return (vertex.color-QVector4D(1,1,1,1)).lengthSquared()>1e-6f;});m_program->setUniformValue("uMeshHasVertexColors",meshHasVertexColors);
    if(m_groundEnabled&&m_shape!=Shape::Plane){m_program->setUniformValue("uIsGround",true);m_groundVao->bind();glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_INT,nullptr);m_groundVao->release();}
    m_program->setUniformValue("uIsGround",false);m_vao->bind();
    if(m_liquidEnabled){
        // Liquid sub-pass before the glass shell: clipped mesh with backface cap.
        const float maxTilt=std::tan(qDegreesToRadians(m_maxTiltDegrees));
        const QVector2D tilt(std::clamp(m_wobble.x(),-maxTilt,maxTilt),std::clamp(m_wobble.y(),-maxTilt,maxTilt));
        const QVector3D planeNormal=QVector3D(tilt.x(),1.0f,tilt.y()).normalized();
        m_program->setUniformValue("uLiquidPass",true);
        m_program->setUniformValue("uLiquidPlaneN",planeNormal);
        m_program->setUniformValue("uLiquidPlaneNWorld",objectModelMatrix().mapVector(planeNormal).normalized());
        m_program->setUniformValue("uLiquidFillY",-1.0f+2.0f*m_liquidFill);
        m_program->setUniformValue("uLiquidColor",m_liquidColor);
        m_program->setUniformValue("uLiquidAbsorption",m_liquidAbsorption);
        m_program->setUniformValue("uMeniscusColor",m_meniscusColor);
        m_program->setUniformValue("uMeniscusWidth",m_meniscusWidth);
        m_program->setUniformValue("uLiquidSurfaceBrightness",m_liquidSurfaceBrightness);
        glDisable(GL_CULL_FACE);
        glDrawElements(GL_TRIANGLES,m_mesh.indices.size(),GL_UNSIGNED_INT,nullptr);
        m_program->setUniformValue("uLiquidPass",false);
    }else{
        m_program->setUniformValue("uLiquidPass",false);
    }
    const bool transparent=m_compositionMode>=2&&m_compositionMode<=7;
    if(transparent){
        glEnable(GL_BLEND);
        if(m_compositionMode==3)glBlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
        else if(m_compositionMode==4)glBlendFunc(GL_SRC_ALPHA,GL_ONE);
        else if(m_compositionMode==5)glBlendFunc(GL_DST_COLOR,GL_ZERO);
        else glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(m_depthWrite?GL_TRUE:GL_FALSE);
    }else if(!m_depthWrite)glDepthMask(GL_FALSE);
    if(!m_depthTest)glDisable(GL_DEPTH_TEST);
    const bool cullBackFace=!m_materialTwoSided&&(m_shape!=Shape::Plane||!m_planeTwoSided);
    if(cullBackFace){glEnable(GL_CULL_FACE);glCullFace(GL_BACK);}
    if(m_debugView==DebugView::Wireframe)glPolygonMode(GL_FRONT_AND_BACK,GL_LINE);
    // Companion-covered submeshes are drawn later with their own materials.
    QSet<int> companionSubmeshes;
    if(m_applyAllSubmeshes)for(const auto& companion:m_companions)for(int index:companion.submeshes)companionSubmeshes.insert(index);
    const auto drawSubmeshRange=[this](const PreviewSubmesh& s){glDrawElements(GL_TRIANGLES,s.indexCount,GL_UNSIGNED_INT,reinterpret_cast<const void*>(quintptr(s.firstIndex*sizeof(quint32))));};
    if(m_applyAllSubmeshes||m_mesh.submeshes.isEmpty()){
        if(companionSubmeshes.isEmpty()||m_mesh.submeshes.isEmpty())glDrawElements(GL_TRIANGLES,m_mesh.indices.size(),GL_UNSIGNED_INT,nullptr);
        else for(int i=0;i<m_mesh.submeshes.size();++i)if(!companionSubmeshes.contains(i))drawSubmeshRange(m_mesh.submeshes[i]);
    }
    else drawSubmeshRange(m_mesh.submeshes[std::clamp(m_submesh,0,int(m_mesh.submeshes.size())-1)]);
    if(cullBackFace)glDisable(GL_CULL_FACE);
    if(!m_depthTest)glEnable(GL_DEPTH_TEST);
    if(transparent||!m_depthWrite)glDepthMask(GL_TRUE);
    if(transparent)glDisable(GL_BLEND);
    if(!companionSubmeshes.isEmpty()){
        for(const auto& companion:m_companions){
            if(companion.submeshes.isEmpty())continue;
            applyCompanionUniforms(companion);
            companion.baseTexture->bind(0);companion.normalTexture->bind(1);companion.rmaosTexture->bind(2);companion.emissiveTexture->bind(3);
            const bool companionTransparent=companion.compositionMode>=2&&companion.compositionMode<=7;
            if(companionTransparent){
                glEnable(GL_BLEND);
                if(companion.compositionMode==3)glBlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
                else if(companion.compositionMode==4)glBlendFunc(GL_SRC_ALPHA,GL_ONE);
                else if(companion.compositionMode==5)glBlendFunc(GL_DST_COLOR,GL_ZERO);
                else glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
                glDepthMask(GL_FALSE);
            }
            const bool companionCull=!companion.twoSided;
            if(companionCull){glEnable(GL_CULL_FACE);glCullFace(GL_BACK);}
            for(int index:companion.submeshes)if(index<m_mesh.submeshes.size())drawSubmeshRange(m_mesh.submeshes[index]);
            if(companionCull)glDisable(GL_CULL_FACE);
            if(companionTransparent){glDisable(GL_BLEND);glDepthMask(GL_TRUE);}
        }
    }
    if(m_debugView==DebugView::Wireframe)glPolygonMode(GL_FRONT_AND_BACK,GL_FILL);
    m_vao->release();m_program->release();
    if(m_msaaSamples>1&&m_msaaFbo){
        glBindFramebuffer(GL_READ_FRAMEBUFFER,m_msaaFbo->handle());
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,defaultFramebufferObject());
        glBlitFramebuffer(0,0,pixelWidth,pixelHeight,0,0,pixelWidth,pixelHeight,GL_COLOR_BUFFER_BIT,GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER,defaultFramebufferObject());
    }
}

void PbrPreviewGL::mousePressEvent(QMouseEvent* event)
{
    setFocus();m_lastMouse=event->position();m_dragButton=event->button();event->accept();
}

void PbrPreviewGL::mouseMoveEvent(QMouseEvent* event)
{
    const QPointF delta=event->position()-m_lastMouse;m_lastMouse=event->position();
    if(m_dragButton==Qt::MiddleButton){
        if(event->modifiers()&Qt::ShiftModifier){const float scale=.0035f*m_distance;m_pan+=QVector2D(float(delta.x())*scale,float(-delta.y())*scale);}
        else{m_yaw+=float(delta.x())*.5f;m_pitch=std::clamp(m_pitch+float(delta.y())*.5f,-89.9f,89.9f);}
        update();emit cameraChanged();
    }else if(m_dragButton==Qt::RightButton){m_distance=std::clamp(m_distance+float(delta.x()+delta.y())*.012f,.25f,25.0f);update();emit cameraChanged();}
    else if(m_dragButton==Qt::LeftButton&&(event->modifiers()&Qt::ShiftModifier)){m_keyYaw+=float(delta.x())*.5f;m_keyPitch=std::clamp(m_keyPitch-float(delta.y())*.5f,-89.0f,89.0f);update();emit lightAnglesChanged(m_keyYaw,m_keyPitch);}
}

void PbrPreviewGL::mouseReleaseEvent(QMouseEvent* event){if(event->button()==m_dragButton)m_dragButton=Qt::NoButton;event->accept();}
void PbrPreviewGL::wheelEvent(QWheelEvent* event){m_distance=std::clamp(m_distance*std::pow(.85f,float(event->angleDelta().y())/120.0f),.25f,25.0f);update();emit cameraChanged();event->accept();}

void PbrPreviewGL::keyPressEvent(QKeyEvent* event)
{
    const bool ctrl=event->modifiers()&Qt::ControlModifier;
    if(event->key()==Qt::Key_F){frameObject();event->accept();return;}
    if(event->key()==Qt::Key_Home){resetView();event->accept();return;}
    if(event->key()==Qt::Key_5){setProjection(m_projection==Projection::Perspective?Projection::Orthographic:Projection::Perspective);event->accept();return;}
    if(event->key()==Qt::Key_1){setAxisView(ctrl?AxisView::Back:AxisView::Front);event->accept();return;}
    if(event->key()==Qt::Key_3){setAxisView(ctrl?AxisView::Left:AxisView::Right);event->accept();return;}
    if(event->key()==Qt::Key_7){setAxisView(ctrl?AxisView::Bottom:AxisView::Top);event->accept();return;}
    QOpenGLWidget::keyPressEvent(event);
}

MaterialPreviewWidget::MaterialPreviewWidget(QWidget* parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("PreviewPanel"));
    auto* layout=new QVBoxLayout(this);layout->setContentsMargins(8,8,8,8);layout->setSpacing(6);
    m_title=new QLabel(tr("Material Preview"),this);m_title->setObjectName(QStringLiteral("SectionTitle"));layout->addWidget(m_title);
    auto* shaderErrorLabel=new QLabel(this);
    shaderErrorLabel->setWordWrap(true);
    shaderErrorLabel->setStyleSheet(QStringLiteral("background:#5a2326;color:#ffd4d6;border:1px solid #8a3a3e;border-radius:2px;padding:4px;"));
    shaderErrorLabel->hide();
    layout->addWidget(shaderErrorLabel);
    m_stack=new QStackedWidget(this);
    m_previewContainer=new QWidget(m_stack);
    auto* previewSplit=new QHBoxLayout(m_previewContainer);previewSplit->setContentsMargins(0,0,0,0);previewSplit->setSpacing(2);
    m_materialCanvas=new PbrPreviewGL(m_previewContainer);
    m_referenceCanvas=new PbrPreviewGL(m_previewContainer);
    m_referenceCanvas->hide();
    previewSplit->addWidget(m_materialCanvas,1);previewSplit->addWidget(m_referenceCanvas,1);
    m_textureCanvas=new TexturePreviewCanvas(m_stack);
    m_stack->addWidget(m_previewContainer);m_stack->addWidget(m_textureCanvas);layout->addWidget(m_stack,1);
    connect(m_materialCanvas,&PbrPreviewGL::shaderErrorsChanged,this,[shaderErrorLabel](const QString& log){
        shaderErrorLabel->setText(tr("Shader error — the preview may be incomplete:\n")+log.left(600));
        shaderErrorLabel->setVisible(!log.isEmpty());
    });
    connect(m_materialCanvas,&PbrPreviewGL::cameraChanged,this,[this]{if(m_referenceCanvas->isVisible())m_referenceCanvas->copyCameraFrom(m_materialCanvas);});
    connect(m_referenceCanvas,&PbrPreviewGL::cameraChanged,this,[this]{if(m_referenceCanvas->isVisible())m_materialCanvas->copyCameraFrom(m_referenceCanvas);});
    m_buttonHost=new QWidget(this);m_buttons=new QHBoxLayout(m_buttonHost);m_buttons->setContentsMargins(0,0,0,0);m_buttons->setSpacing(5);layout->addWidget(m_buttonHost);
    buildPreviewControls(layout);
    // The settings tabs live in scroll areas; without the guard, scrolling the
    // panel silently changes any combo under the cursor and savePreviewSettings
    // persists the accident (this is how projection kept flipping to ortho).
    for(QComboBox* combo:m_controls->findChildren<QComboBox*>())WheelFocusGuard::install(combo);
    auto* help=new QLabel(tr("Viewport: MMB orbit • Shift+MMB pan • RMB dolly • wheel zoom • F frame"),this);
    help->setObjectName(QStringLiteral("Muted"));help->setWordWrap(true);
    help->setToolTip(tr("Viewport controls: MMB orbit; Shift+MMB pan; RMB dolly; wheel zoom; Shift+LMB rotate the key light; F frame; Numpad 1/3/7 axis views; Numpad 5 toggle projection."));
    layout->addWidget(help);
    connect(m_materialCanvas,&PbrPreviewGL::lightAnglesChanged,this,[this](float yaw,float pitch){QSignalBlocker a(m_keyYaw),b(m_keyPitch);m_keyYaw->setValue(yaw);m_keyPitch->setValue(pitch);savePreviewSettings();});
    loadPreviewSettings();showFullMaterial();
}

void MaterialPreviewWidget::buildPreviewControls(QVBoxLayout* layout)
{
    // Collapsible header: folding the settings away hands their height to the
    // viewport (m_stack carries the layout stretch).
    m_settingsToggle=new QToolButton(this);
    m_settingsToggle->setText(tr("Preview Settings"));
    m_settingsToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_settingsToggle->setAutoRaise(true);
    m_settingsToggle->setCheckable(true);
    m_settingsToggle->setToolTip(tr("Show or hide the preview settings to give the viewport more room."));
    layout->addWidget(m_settingsToggle);
    m_controls=new QGroupBox(QString(),this);m_controls->setObjectName(QStringLiteral("PreviewSettings"));
    const bool settingsExpanded=!QSettings().value(QStringLiteral("Preview/settingsCollapsed"),false).toBool();
    m_settingsToggle->setChecked(settingsExpanded);
    m_settingsToggle->setIcon(disclosureArrowIcon(settingsExpanded,14,QColor(223,226,230)));
    m_controls->setVisible(settingsExpanded);
    connect(m_settingsToggle,&QToolButton::toggled,this,[this](bool expanded){
        m_controls->setVisible(expanded);
        m_settingsToggle->setIcon(disclosureArrowIcon(expanded,14,QColor(223,226,230)));
        QSettings().setValue(QStringLiteral("Preview/settingsCollapsed"),!expanded);
    });
    auto* box=new QVBoxLayout(m_controls);box->setContentsMargins(6,8,6,6);box->setSpacing(0);
    auto* tabs=new QTabWidget(m_controls);tabs->setObjectName(QStringLiteral("PreviewSettingsTabs"));
    // The tab pages outgrew the fixed panel height; scrolling beats letting
    // the form layout compress and clip its rows.
    const auto wrapSettingsTab=[tabs](QWidget* page){
        auto* scroll=new QScrollArea(tabs);
        scroll->setWidget(page);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        return scroll;
    };

    const auto addHeading=[](QFormLayout* form,const QString& text){auto* heading=new QLabel(text,form->parentWidget());heading->setObjectName(QStringLiteral("PreviewSubheading"));form->addRow(heading);};

    auto* scene=new QWidget(tabs);auto* sceneForm=new QFormLayout(scene);sceneForm->setContentsMargins(8,8,8,8);sceneForm->setVerticalSpacing(6);
    addHeading(sceneForm,tr("Preview Object"));
    auto* modelField=new QWidget(scene);auto* modelRow=new QHBoxLayout(modelField);modelRow->setContentsMargins(0,0,0,0);modelRow->setSpacing(5);
    m_shape=new QComboBox(modelField);m_shape->setObjectName(QStringLiteral("PreviewModel"));m_shape->addItems({tr("Plane"),tr("Cube"),tr("Sphere"),tr("Imported Mesh")});m_shape->setToolTip(tr("Choose the geometry used to preview the current material."));modelRow->addWidget(m_shape,1);
    m_loadMesh=new QPushButton(tr("Import…"),modelField);m_loadMesh->setToolTip(tr("Import a static FBX or Fallout 4 NIF preview mesh."));modelRow->addWidget(m_loadMesh);sceneForm->addRow(tr("Model"),modelField);
    m_planeOptionsLabel=new QLabel(tr("Plane setup"),scene);
    m_planeOptions=new QWidget(scene);auto* planeRow=new QHBoxLayout(m_planeOptions);planeRow->setContentsMargins(0,0,0,0);planeRow->setSpacing(7);
    m_planeOrientation=new QComboBox(m_planeOptions);m_planeOrientation->addItems({tr("Facing Camera (XY)"),tr("Ground Plane (XZ)"),tr("Side Plane (YZ)")});planeRow->addWidget(m_planeOrientation,1);
    m_twoSided=new QCheckBox(tr("Two-sided"),m_planeOptions);m_twoSided->setChecked(true);planeRow->addWidget(m_twoSided);sceneForm->addRow(m_planeOptionsLabel,m_planeOptions);
    m_uvSetLabel=new QLabel(tr("Material layers"),scene);
    m_uvSet=new QComboBox(scene);m_uvSet->setObjectName(QStringLiteral("PreviewMaterialLayers"));m_uvSet->addItems({tr("Primary Material (UV0)"),tr("Secondary Material (UV1)"),tr("Tertiary Material (UV2)"),tr("Combined Material (All Layers)")});m_uvSet->setToolTip(tr("Choose which authored material layer and mesh UV set to preview."));sceneForm->addRow(m_uvSetLabel,m_uvSet);
    m_landscapeLayerLabel=new QLabel(tr("Landscape layer"),scene);m_landscapeLayer=new QComboBox(scene);for(int i=0;i<16;++i)m_landscapeLayer->addItem(tr("Layer %1").arg(i),i);sceneForm->addRow(m_landscapeLayerLabel,m_landscapeLayer);
    m_submeshLabel=new QLabel(tr("Submeshes"),scene);
    m_submeshOptions=new QWidget(scene);auto* submeshRow=new QHBoxLayout(m_submeshOptions);submeshRow->setContentsMargins(0,0,0,0);submeshRow->setSpacing(7);
    m_submesh=new QComboBox(m_submeshOptions);submeshRow->addWidget(m_submesh,1);m_allSubmeshes=new QCheckBox(tr("All"),m_submeshOptions);m_allSubmeshes->setToolTip(tr("Apply the material to every submesh in the imported model."));m_allSubmeshes->setChecked(true);submeshRow->addWidget(m_allSubmeshes);
    m_submeshMaterials=new QPushButton(tr("Materials…"),m_submeshOptions);m_submeshMaterials->setToolTip(tr("Assign saved .pbrm materials to individual submeshes. Unassigned submeshes use the material being edited. Assignments are stored next to the mesh in a .pbrmset file."));submeshRow->addWidget(m_submeshMaterials);sceneForm->addRow(m_submeshLabel,m_submeshOptions);
    m_importStatus=new QLabel(scene);m_importStatus->setObjectName(QStringLiteral("Muted"));m_importStatus->setWordWrap(true);sceneForm->addRow(m_importStatus);
    addHeading(sceneForm,tr("Camera"));
    m_projection=new QComboBox(scene);m_projection->addItems({tr("Perspective"),tr("Orthographic")});sceneForm->addRow(tr("Projection"),m_projection);
    m_view=new QComboBox(scene);m_view->addItems({tr("Free"),tr("Front"),tr("Back"),tr("Left"),tr("Right"),tr("Top"),tr("Bottom")});m_view->setToolTip(tr("Snap the camera to a standard axis view."));sceneForm->addRow(tr("Axis view"),m_view);
    auto* cameraActions=new QWidget(scene);auto* cameraRow=new QHBoxLayout(cameraActions);cameraRow->setContentsMargins(0,0,0,0);cameraRow->setSpacing(5);
    auto* frame=new QPushButton(tr("Frame Object"),cameraActions);frame->setToolTip(tr("Frame the preview object (F)."));auto* reset=new QPushButton(tr("Reset Camera"),cameraActions);reset->setToolTip(tr("Restore the default camera position (Home)."));cameraRow->addWidget(frame);cameraRow->addWidget(reset);cameraRow->addStretch();sceneForm->addRow(QString(),cameraActions);
    addHeading(sceneForm,tr("Object Transform"));
    const auto makeObjectField=[scene](double minimum,double maximum,double step,const QString& tooltip,const QString& suffix=QString()){
        auto* field=new DragNumericEdit(scene);field->setRange(minimum,maximum);field->setSingleStep(step);field->setDecimals(2);field->setDragSensitivity(step);if(!suffix.isEmpty())field->setSuffix(suffix);field->setToolTip(tooltip);return field;
    };
    const auto makeTripleRow=[scene](DragNumericEdit* x,DragNumericEdit* y,DragNumericEdit* z){
        auto* row=new QWidget(scene);auto* layout=new QHBoxLayout(row);layout->setContentsMargins(0,0,0,0);layout->setSpacing(4);
        layout->addWidget(x,1);layout->addWidget(y,1);layout->addWidget(z,1);return row;
    };
    m_objectRotationX=makeObjectField(-360,360,1,tr("Rotation around the X axis."),QStringLiteral("°"));
    m_objectRotationY=makeObjectField(-360,360,1,tr("Rotation around the Y axis."),QStringLiteral("°"));
    m_objectRotationZ=makeObjectField(-360,360,1,tr("Rotation around the Z axis."),QStringLiteral("°"));
    m_objectPositionX=makeObjectField(-10,10,.01,tr("Object position along the X axis."));
    m_objectPositionY=makeObjectField(-10,10,.01,tr("Object position along the Y axis."));
    m_objectPositionZ=makeObjectField(-10,10,.01,tr("Object position along the Z axis."));
    sceneForm->addRow(tr("Rotation X / Y / Z"),makeTripleRow(m_objectRotationX,m_objectRotationY,m_objectRotationZ));
    sceneForm->addRow(tr("Position X / Y / Z"),makeTripleRow(m_objectPositionX,m_objectPositionY,m_objectPositionZ));
    auto* objectActions=new QWidget(scene);auto* objectRow=new QHBoxLayout(objectActions);objectRow->setContentsMargins(0,0,0,0);objectRow->setSpacing(5);
    auto* resetObject=new QPushButton(tr("Reset Transform"),objectActions);resetObject->setToolTip(tr("Return the preview object to the origin with no rotation."));objectRow->addWidget(resetObject);objectRow->addStretch();sceneForm->addRow(QString(),objectActions);
    const auto applyObjectTransform=[this]{
        m_materialCanvas->setObjectTransform(
            QVector3D(float(m_objectRotationX->value()),float(m_objectRotationY->value()),float(m_objectRotationZ->value())),
            QVector3D(float(m_objectPositionX->value()),float(m_objectPositionY->value()),float(m_objectPositionZ->value())));
        savePreviewSettings();
    };
    for(DragNumericEdit* field:{m_objectRotationX,m_objectRotationY,m_objectRotationZ,m_objectPositionX,m_objectPositionY,m_objectPositionZ})
        connect(field,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[applyObjectTransform](double){applyObjectTransform();});
    connect(resetObject,&QPushButton::clicked,this,[this,applyObjectTransform]{
        for(DragNumericEdit* field:{m_objectRotationX,m_objectRotationY,m_objectRotationZ,m_objectPositionX,m_objectPositionY,m_objectPositionZ}){QSignalBlocker blocker(field);field->setValue(0);}
        applyObjectTransform();
    });
    addHeading(sceneForm,tr("Time"));
    auto* timePause=new QCheckBox(tr("Pause animation"),scene);
    timePause->setToolTip(tr("Freezes UV animation, flipbooks, advection, water, and vegetation motion for inspection."));
    auto* timeValue=new DragNumericEdit(scene);timeValue->setRange(0,100000);timeValue->setSingleStep(.01);timeValue->setDecimals(2);timeValue->setDragSensitivity(.02);timeValue->setSuffix(tr(" s"));timeValue->setEnabled(false);
    timeValue->setToolTip(tr("Scrub the animation clock while paused."));
    auto* timeSpeed=new DragNumericEdit(scene);timeSpeed->setRange(.05,8);timeSpeed->setSingleStep(.01);timeSpeed->setDecimals(2);timeSpeed->setDragSensitivity(.01);timeSpeed->setValue(1);timeSpeed->setSuffix(QStringLiteral("x"));
    timeSpeed->setToolTip(tr("Playback speed multiplier."));
    sceneForm->addRow(QString(),timePause);
    sceneForm->addRow(tr("Time"),timeValue);
    sceneForm->addRow(tr("Speed"),timeSpeed);
    connect(timePause,&QCheckBox::toggled,this,[this,timeValue,timeSpeed](bool paused){
        timeValue->setEnabled(paused);
        if(paused){QSignalBlocker blocker(timeValue);timeValue->setValue(m_materialCanvas->timeValue());}
        m_materialCanvas->setTimePlayback(paused,float(timeSpeed->value()));
    });
    connect(timeValue,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this](double seconds){m_materialCanvas->setTimeValue(seconds);});
    connect(timeSpeed,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this,timePause](double scale){m_materialCanvas->setTimePlayback(timePause->isChecked(),float(scale));});
    addHeading(sceneForm,tr("Particle Simulation"));
    auto* particleAgeAuto=new QCheckBox(tr("Auto-loop age"),scene);particleAgeAuto->setChecked(true);
    particleAgeAuto->setToolTip(tr("Loops particle age continuously. Uncheck to scrub a specific age."));
    auto* particleAge=new DragNumericEdit(scene);particleAge->setRange(0,1);particleAge->setSingleStep(.005);particleAge->setDecimals(3);particleAge->setDragSensitivity(.004);particleAge->setValue(.5);particleAge->setEnabled(false);
    particleAge->setToolTip(tr("Particle age from birth (0) to death (1); drives flipbooks, gradients, and erosion."));
    auto* particleRate=new DragNumericEdit(scene);particleRate->setRange(.05,5);particleRate->setSingleStep(.01);particleRate->setDecimals(2);particleRate->setDragSensitivity(.01);particleRate->setValue(.2);particleRate->setSuffix(tr(" /s"));
    particleRate->setToolTip(tr("Auto-loop speed: how much age advances per second."));
    auto* particleSeed=new DragNumericEdit(scene);particleSeed->setRange(0,1);particleSeed->setSingleStep(.005);particleSeed->setDecimals(3);particleSeed->setDragSensitivity(.004);particleSeed->setValue(0);
    particleSeed->setToolTip(tr("Per-particle random seed; drives start-frame, phase, and hue variation."));
    sceneForm->addRow(QString(),particleAgeAuto);
    sceneForm->addRow(tr("Age"),particleAge);
    sceneForm->addRow(tr("Loop speed"),particleRate);
    sceneForm->addRow(tr("Seed"),particleSeed);
    const auto applyParticleSim=[this,particleAge,particleAgeAuto,particleRate,particleSeed]{
        particleAge->setEnabled(!particleAgeAuto->isChecked());
        m_materialCanvas->setParticleSimulation(float(particleAge->value()),particleAgeAuto->isChecked(),float(particleRate->value()),float(particleSeed->value()));
    };
    connect(particleAgeAuto,&QCheckBox::toggled,this,[applyParticleSim](bool){applyParticleSim();});
    for(DragNumericEdit* field:{particleAge,particleRate,particleSeed})
        connect(field,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[applyParticleSim](double){applyParticleSim();});
    tabs->addTab(wrapSettingsTab(scene),tr("Scene"));

    auto* lighting=new QWidget(tabs);auto* lightingForm=new QFormLayout(lighting);lightingForm->setContentsMargins(8,8,8,8);lightingForm->setVerticalSpacing(6);
    m_lightingPreset=new QComboBox(lighting);m_lightingPreset->addItems({tr("Studio"),tr("Direct Sun"),tr("Outdoor Shadow"),tr("Interior"),tr("Neutral Validation"),tr("Custom")});m_lightingPreset->setToolTip(tr("Apply a complete preview-lighting preset."));lightingForm->addRow(tr("Lighting preset"),m_lightingPreset);
    m_deriveExrLighting=new QCheckBox(tr("Derive shadow direction from EXR"),lighting);m_deriveExrLighting->setObjectName(QStringLiteral("DeriveExrLighting"));m_deriveExrLighting->setEnabled(false);m_deriveExrLighting->setToolTip(tr("Uses the dominant HDR radiance direction only to cast an environment shadow. The EXR remains the sole lighting source, so its bright region is not counted twice."));lightingForm->addRow(QString(),m_deriveExrLighting);
    addHeading(lightingForm,tr("Key Light"));
    m_keyIntensity=spin(0,20,.25,QString(),lighting);m_keyYaw=spin(-360,360,1,tr("°"),lighting);m_keyPitch=spin(-89,89,1,tr("°"),lighting);
    m_temperature=new QComboBox(lighting);m_temperature->setPlaceholderText(tr("From EXR"));m_temperature->addItem(tr("Warm 3200 K"),3200);m_temperature->addItem(tr("Neutral 5500 K"),5500);m_temperature->addItem(tr("Daylight 6500 K"),6500);m_temperature->addItem(tr("Cool 9000 K"),9000);m_temperature->setCurrentIndex(1);
    m_keyColor=new ColorEdit(temperatureColor(5500),false,lighting);
    m_cubemap=new QComboBox(lighting);
    m_cubemap->setObjectName(QStringLiteral("PreviewEnvironment"));m_cubemap->setToolTip(tr("Environment maps are managed in Editor Settings and may be DDS cubemaps or EXR panoramas."));
    m_shadows=new QCheckBox(tr("Cast"),lighting);m_shadows->setObjectName(QStringLiteral("PreviewShadows"));m_shadows->setChecked(true);
    m_shadowIntensity=spin(0,1,.05,QString(),lighting);m_shadowIntensity->setObjectName(QStringLiteral("PreviewShadowIntensity"));m_shadowIntensity->setValue(.75);
    m_environmentIntensity=spin(0,8,.1,QString(),lighting);m_environmentYaw=spin(-360,360,1,tr("°"),lighting);m_exposure=spin(-8,8,.25,tr(" EV"),lighting);
    lightingForm->addRow(tr("Intensity"),m_keyIntensity);lightingForm->addRow(tr("Color temperature"),m_temperature);lightingForm->addRow(tr("Color"),m_keyColor);
    auto* directionField=new QWidget(lighting);auto* directionRow=new QHBoxLayout(directionField);directionRow->setContentsMargins(0,0,0,0);directionRow->setSpacing(5);directionRow->addWidget(new QLabel(tr("Azimuth"),directionField));directionRow->addWidget(m_keyYaw,1);directionRow->addWidget(new QLabel(tr("Elevation"),directionField));directionRow->addWidget(m_keyPitch,1);lightingForm->addRow(tr("Direction"),directionField);
    auto* shadowField=new QWidget(lighting);auto* shadowRow=new QHBoxLayout(shadowField);shadowRow->setContentsMargins(0,0,0,0);shadowRow->setSpacing(7);shadowRow->addWidget(m_shadows);shadowRow->addWidget(new QLabel(tr("Strength"),shadowField));shadowRow->addWidget(m_shadowIntensity,1);lightingForm->addRow(tr("Shadows"),shadowField);
    addHeading(lightingForm,tr("Environment"));
    lightingForm->addRow(tr("Map"),m_cubemap);lightingForm->addRow(tr("Intensity"),m_environmentIntensity);lightingForm->addRow(tr("Rotation"),m_environmentYaw);lightingForm->addRow(tr("Scene exposure"),m_exposure);
    tabs->addTab(wrapSettingsTab(lighting),tr("Lighting"));
    auto* display=new QWidget(tabs);auto* displayForm=new QFormLayout(display);displayForm->setContentsMargins(8,8,8,8);displayForm->setVerticalSpacing(6);
    m_background=new QComboBox(display);m_background->addItems({tr("Environment"),tr("Neutral Gray"),tr("Black"),tr("White"),tr("Checkerboard")});
    m_debug=new QComboBox(display);m_debug->setObjectName(QStringLiteral("PreviewRenderMode"));
    const auto addRenderMode=[this](const QString& name,PbrPreviewGL::DebugView mode){m_debug->addItem(name,int(mode));};
    addRenderMode(tr("Shaded"),PbrPreviewGL::DebugView::Shaded);addRenderMode(tr("Base Color"),PbrPreviewGL::DebugView::BaseColor);addRenderMode(tr("Opacity"),PbrPreviewGL::DebugView::Opacity);
    addRenderMode(tr("Normal Map (Tangent Space)"),PbrPreviewGL::DebugView::TangentNormal);addRenderMode(tr("Normal (World Space)"),PbrPreviewGL::DebugView::Normals);addRenderMode(tr("Height"),PbrPreviewGL::DebugView::Height);
    addRenderMode(tr("Roughness"),PbrPreviewGL::DebugView::Roughness);addRenderMode(tr("Metallic"),PbrPreviewGL::DebugView::Metallic);addRenderMode(tr("Roughness + Metallic (R/G)"),PbrPreviewGL::DebugView::RoughnessMetallic);
    addRenderMode(tr("Ambient Occlusion"),PbrPreviewGL::DebugView::AmbientOcclusion);addRenderMode(tr("Specular F0"),PbrPreviewGL::DebugView::F0);addRenderMode(tr("RMAOS (Packed RGB)"),PbrPreviewGL::DebugView::Rmaos);
    addRenderMode(tr("Emissive Color"),PbrPreviewGL::DebugView::EmissiveColor);addRenderMode(tr("Emissive Mask"),PbrPreviewGL::DebugView::EmissiveMask);addRenderMode(tr("Emissive (Final)"),PbrPreviewGL::DebugView::Emissive);addRenderMode(tr("Layer Coverage"),PbrPreviewGL::DebugView::LayerCoverage);
    addRenderMode(tr("UV Checker"),PbrPreviewGL::DebugView::UvChecker);addRenderMode(tr("Wireframe"),PbrPreviewGL::DebugView::Wireframe);addRenderMode(tr("Tangent Basis"),PbrPreviewGL::DebugView::TangentBasis);
    m_antiAliasing=new QComboBox(display);m_antiAliasing->addItem(tr("Off"),1);m_antiAliasing->addItem(tr("MSAA 2x"),2);m_antiAliasing->addItem(tr("MSAA 4x"),4);m_antiAliasing->addItem(tr("MSAA 8x"),8);m_antiAliasing->setCurrentIndex(2);m_antiAliasing->setToolTip(tr("Multisampled viewport rendering. Editor preference only; never stored in the material."));
    m_ground=new QCheckBox(tr("Show ground plane"),display);m_ground->setChecked(true);m_compare=new QCheckBox(tr("Compare with defaults (split view)"),display);m_compare->setToolTip(tr("Split the object between the current material and its default values."));
    displayForm->addRow(tr("Background"),m_background);displayForm->addRow(tr("Render mode"),m_debug);displayForm->addRow(tr("Anti-aliasing"),m_antiAliasing);displayForm->addRow(QString(),m_ground);displayForm->addRow(QString(),m_compare);
    auto* referenceRow=new QWidget(display);auto* referenceLayout=new QHBoxLayout(referenceRow);referenceLayout->setContentsMargins(0,0,0,0);referenceLayout->setSpacing(5);
    auto* referenceLoad=new QPushButton(tr("Load…"),referenceRow);referenceLoad->setToolTip(tr("Show a second read-only material beside this one with a synchronized camera. Lighting and environment are snapshotted when the reference loads."));
    auto* referenceClear=new QPushButton(tr("Clear"),referenceRow);referenceClear->setEnabled(false);
    m_referenceLabel=new QLabel(tr("None"),referenceRow);m_referenceLabel->setObjectName(QStringLiteral("Muted"));
    referenceLayout->addWidget(referenceLoad);referenceLayout->addWidget(referenceClear);referenceLayout->addWidget(m_referenceLabel,1);
    displayForm->addRow(tr("Reference"),referenceRow);
    connect(referenceLoad,&QPushButton::clicked,this,[this,referenceClear]{
        QSettings settings;
        const QString start=settings.value(QStringLiteral("Preview/referenceDirectory"),settings.value(QStringLiteral("recentDirectory"))).toString();
        const QString fileName=QFileDialog::getOpenFileName(this,tr("Load reference material"),start,tr("PBR materials (*.pbrm)"));
        if(fileName.isEmpty())return;
        QString error;
        Pbr::MaterialDocument reference;
        if(!reference.load(fileName,&error)){QMessageBox::warning(this,tr("Reference material"),error);return;}
        settings.setValue(QStringLiteral("Preview/referenceDirectory"),QFileInfo(fileName).absolutePath());
        m_referenceDocument=reference;
        m_referenceCanvas->setShape(static_cast<PbrPreviewGL::Shape>(qMin(m_shape->currentIndex(),2)));
        m_referenceCanvas->setLighting(float(m_keyIntensity->value()),m_keyColor->color(),float(m_keyYaw->value()),float(m_keyPitch->value()),float(m_environmentIntensity->value()),float(m_environmentYaw->value()),float(m_exposure->value()));
        m_referenceCanvas->setShadows(m_shadows->isChecked(),float(m_shadowIntensity->value()));
        m_referenceCanvas->setEnvironmentCubemap(m_cubemap->currentData().toString(),m_textureRoot);
        m_referenceCanvas->setMsaaSamples(m_antiAliasing->currentData().toInt());
        m_referenceCanvas->setUvSet(m_uvSet->currentIndex());
        m_referenceCanvas->setMaterial(m_referenceDocument,m_textureRoot);
        m_referenceCanvas->copyCameraFrom(m_materialCanvas);
        m_referenceCanvas->show();
        referenceClear->setEnabled(true);
        m_referenceLabel->setText(QFileInfo(fileName).fileName());
    });
    connect(referenceClear,&QPushButton::clicked,this,[this,referenceClear]{
        m_referenceCanvas->hide();
        referenceClear->setEnabled(false);
        m_referenceLabel->setText(tr("None"));
    });
    tabs->addTab(wrapSettingsTab(display),tr("Display"));
    auto* states=new QWidget(tabs);auto* statesForm=new QFormLayout(states);statesForm->setContentsMargins(8,8,8,8);statesForm->setVerticalSpacing(6);
    auto* statesIntro=new QLabel(tr("Simulates the runtime's per-state amounts, with the tag interaction physics applied. Preview-only; never stored in the material."),states);
    statesIntro->setObjectName(QStringLiteral("Muted"));statesIntro->setWordWrap(true);statesForm->addRow(statesIntro);
    m_stateRows=new QWidget(states);auto* stateRowsForm=new QFormLayout(m_stateRows);stateRowsForm->setContentsMargins(0,0,0,0);stateRowsForm->setVerticalSpacing(6);
    statesForm->addRow(m_stateRows);
    m_weatherSim=new QComboBox(states);
    m_weatherSim->addItems({tr("None"),tr("Rain Storm"),tr("Dust Storm"),tr("Blizzard"),tr("Heat Wave")});
    m_weatherSim->setToolTip(tr("Drives the raw amounts on a looping 30-second weather timeline so accumulation, washing, and drying play out through the tag physics."));
    statesForm->addRow(tr("Weather"),m_weatherSim);
    auto* weatherHint=new QLabel(tr("Sliders are driven by the weather simulation."),states);
    weatherHint->setObjectName(QStringLiteral("Muted"));weatherHint->setWordWrap(true);weatherHint->setVisible(false);
    statesForm->addRow(QString(),weatherHint);
    auto* statesReset=new QPushButton(tr("Clear States"),states);statesForm->addRow(QString(),statesReset);
    m_weatherTimer=new QTimer(this);m_weatherTimer->setInterval(100);
    connect(m_weatherTimer,&QTimer::timeout,this,[this]{tickWeatherSimulation();});
    connect(m_weatherSim,qOverload<int>(&QComboBox::currentIndexChanged),this,[this,weatherHint](int index){
        m_weatherClock=0;
        weatherHint->setVisible(index!=0);
        for(DragNumericEdit* field:m_stateFields)field->setEnabled(index==0);
        if(index==0){m_weatherTimer->stop();applyStateAmounts();}
        else m_weatherTimer->start();
    });
    connect(statesReset,&QPushButton::clicked,this,[this]{
        {QSignalBlocker blocker(m_weatherSim);m_weatherSim->setCurrentIndex(0);}
        m_weatherTimer->stop();
        for(DragNumericEdit* field:m_stateFields){QSignalBlocker blocker(field);field->setValue(0);field->setEnabled(true);}
        applyStateAmounts();
    });
    tabs->addTab(wrapSettingsTab(states),tr("States"));
    box->addWidget(tabs);
    layout->addWidget(m_controls);

    auto updatePlane=[this]{m_materialCanvas->setPlaneOptions(static_cast<PreviewMeshes::PlaneOrientation>(m_planeOrientation->currentIndex()),m_twoSided->isChecked());savePreviewSettings();};
    connect(m_planeOrientation,qOverload<int>(&QComboBox::currentIndexChanged),this,[updatePlane](int){updatePlane();});connect(m_twoSided,&QCheckBox::toggled,this,[updatePlane](bool){updatePlane();});
    connect(m_shape,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int index){
        if(index==3){m_loadMesh->click();return;}m_materialCanvas->setShape(static_cast<PbrPreviewGL::Shape>(index));refreshMeshControls();savePreviewSettings();});
    connect(m_loadMesh,&QPushButton::clicked,this,[this]{
        QSettings settings;
        const QString path=QFileDialog::getOpenFileName(this,tr("Load static preview mesh"),settings.value(QStringLiteral("Preview/lastMeshDirectory"),settings.value(QStringLiteral("Preview/lastFbxDirectory"))).toString(),tr("Static Meshes (*.fbx *.nif);;FBX Mesh (*.fbx);;Fallout 4 NIF (*.nif)"));
        if(path.isEmpty()){if(m_shape->currentIndex()==3&&!m_materialCanvas->mesh().sourcePath.size()){QSignalBlocker b(m_shape);m_shape->setCurrentIndex(0);}return;}
        QString error;if(!m_materialCanvas->loadCustomMesh(path,&error)){QMessageBox::warning(this,tr("Mesh import failed"),error);QSignalBlocker b(m_shape);m_shape->setCurrentIndex(0);return;}
        settings.setValue(QStringLiteral("Preview/lastMeshDirectory"),QFileInfo(path).absolutePath());settings.setValue(QStringLiteral("Preview/customMesh"),path);QSignalBlocker b(m_shape);m_shape->setCurrentIndex(3);refreshMeshControls();savePreviewSettings();});
    connect(m_submesh,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int i){m_materialCanvas->setSubmesh(i,m_allSubmeshes->isChecked());savePreviewSettings();});
    connect(m_allSubmeshes,&QCheckBox::toggled,this,[this](bool all){m_submesh->setEnabled(!all);m_materialCanvas->setSubmesh(m_submesh->currentIndex(),all);savePreviewSettings();});
    connect(m_submeshMaterials,&QPushButton::clicked,this,[this]{showSubmeshMaterialsDialog();});
    connect(m_uvSet,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int i){m_materialCanvas->setUvSet(i);const QString layer=i==1?tr("Secondary (UV1)"):(i==2?tr("Tertiary (UV2)"):(i==3?tr("Combined (All Layers)"):tr("Primary (UV0)")));m_title->setText(tr("Material Preview • %1").arg(layer));savePreviewSettings();});
    connect(m_landscapeLayer,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int i){m_materialCanvas->setLandscapeLayer(i);savePreviewSettings();});
    connect(m_antiAliasing,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int){m_materialCanvas->setMsaaSamples(m_antiAliasing->currentData().toInt());savePreviewSettings();});
    connect(m_cubemap,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int){
        if(m_cubemap->currentIndex()<0)return;
        const QString path=m_cubemap->currentData().toString();m_materialCanvas->setEnvironmentCubemap(path,m_textureRoot);
        QSettings settings(PreviewEnvironmentSettings::filePath(),QSettings::IniFormat);PreviewEnvironmentSettings::migrateLegacy(settings);settings.setValue(QStringLiteral("PreviewCubemaps/activePath"),path);settings.sync();
        refreshDerivedEnvironmentLight(true);
    });
    connect(m_projection,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int i){m_materialCanvas->setProjection(static_cast<PbrPreviewGL::Projection>(i));savePreviewSettings();});
    connect(m_view,qOverload<int>(&QComboBox::activated),this,[this](int i){if(i>0)m_materialCanvas->setAxisView(static_cast<PbrPreviewGL::AxisView>(i-1));m_view->setCurrentIndex(0);});
    connect(frame,&QPushButton::clicked,m_materialCanvas,&PbrPreviewGL::frameObject);connect(reset,&QPushButton::clicked,m_materialCanvas,&PbrPreviewGL::resetView);
    connect(m_lightingPreset,qOverload<int>(&QComboBox::activated),this,[this](int index){if(index<5)applyLightingPreset(index);});
    auto updateLight=[this]{m_materialCanvas->setLighting(float(m_keyIntensity->value()),m_keyColor->color(),float(m_keyYaw->value()),float(m_keyPitch->value()),float(m_environmentIntensity->value()),float(m_environmentYaw->value()),float(m_exposure->value()));savePreviewSettings();};
    const auto overrideDerived=[this]{
        if(m_applyingLightingPreset||m_applyingDerivedLight)return;
        if(m_deriveExrLighting->isChecked()){
            m_deriveExrLightingPreferred=false;m_manualLightStored=false;QSignalBlocker blocker(m_deriveExrLighting);m_deriveExrLighting->setChecked(false);
        }
        m_lightingPreset->setCurrentIndex(5);
    };
    for(auto* s:{m_keyIntensity,m_keyYaw,m_keyPitch})connect(s,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[overrideDerived,updateLight](double){overrideDerived();updateLight();});
    for(auto* s:{m_environmentIntensity,m_exposure})connect(s,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this,updateLight](double){if(!m_applyingLightingPreset&&!m_deriveExrLighting->isChecked())m_lightingPreset->setCurrentIndex(5);updateLight();});
    connect(m_environmentYaw,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this,updateLight](double){if(m_deriveExrLighting->isChecked())refreshDerivedEnvironmentLight(false);else{if(!m_applyingLightingPreset)m_lightingPreset->setCurrentIndex(5);updateLight();}});
    connect(m_temperature,qOverload<int>(&QComboBox::currentIndexChanged),this,[this,overrideDerived,updateLight](int){overrideDerived();m_keyColor->setColor(temperatureColor(m_temperature->currentData().toInt()));updateLight();});
    connect(m_keyColor,&ColorEdit::colorChanged,this,[overrideDerived,updateLight](const QColor&){overrideDerived();updateLight();});
    connect(m_deriveExrLighting,&QCheckBox::toggled,this,[this](bool checked){
        if(!m_deriveExrLighting->isEnabled()||m_applyingDerivedLight)return;
        m_deriveExrLightingPreferred=checked;
        if(checked)refreshDerivedEnvironmentLight(true);else{restoreManualLight();m_lightingPreset->setCurrentIndex(5);savePreviewSettings();}
    });
    auto updateShadows=[this]{m_shadowIntensity->setEnabled(m_shadows->isChecked());m_materialCanvas->setShadows(m_shadows->isChecked(),float(m_shadowIntensity->value()));savePreviewSettings();};
    connect(m_shadows,&QCheckBox::toggled,this,[updateShadows](bool){updateShadows();});connect(m_shadowIntensity,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[updateShadows](double){updateShadows();});
    connect(m_background,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int i){m_materialCanvas->setBackground(static_cast<PbrPreviewGL::Background>(i));savePreviewSettings();});
    connect(m_debug,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int i){m_materialCanvas->setDebugView(static_cast<PbrPreviewGL::DebugView>(m_debug->itemData(i).toInt()));savePreviewSettings();});
    connect(m_ground,&QCheckBox::toggled,this,[this](bool v){m_materialCanvas->setGroundEnabled(v);savePreviewSettings();});connect(m_compare,&QCheckBox::toggled,this,[this](bool v){m_materialCanvas->setComparisonEnabled(v);savePreviewSettings();});
}

void MaterialPreviewWidget::refreshMeshControls()
{
    const auto& mesh=m_materialCanvas->mesh();QSignalBlocker blocker(m_submesh);m_submesh->clear();
    for(const auto& submesh:mesh.submeshes) m_submesh->addItem(submesh.name);
    const bool showSubmeshes=m_shape->currentIndex()==3&&m_submesh->count()>1;
    m_submeshLabel->setVisible(showSubmeshes);m_submeshOptions->setVisible(showSubmeshes);
    m_submesh->setEnabled(showSubmeshes&&!m_allSubmeshes->isChecked());
    if(mesh.sourcePath!=m_assignmentsMeshPath){m_assignmentsMeshPath=mesh.sourcePath;loadSubmeshAssignments();}
    applySubmeshAssignments();
    const QString uvHint=mesh.hasUv1?tr("Uses the mesh's authored UV1 coordinates."):tr("This mesh has no distinct UV1 coordinates; UV1 mirrors UV0 for preview.");
    m_uvSet->setItemData(1,uvHint,Qt::ToolTipRole);m_uvSet->setItemData(2,uvHint,Qt::ToolTipRole);m_uvSet->setItemData(3,uvHint,Qt::ToolTipRole);
    const bool showPlaneOptions=m_shape->currentIndex()==0;m_planeOptionsLabel->setVisible(showPlaneOptions);m_planeOptions->setVisible(showPlaneOptions);
    QString status=mesh.sourcePath.isEmpty()?tr("Built-in mesh: %1 vertices, %2 triangles.").arg(mesh.vertices.size()).arg(mesh.indices.size()/3):mesh.diagnostics.join(QStringLiteral(" "));
    m_shape->setToolTip(status);m_importStatus->setText(status);m_importStatus->setToolTip(mesh.sourcePath);m_importStatus->setVisible(m_shape->currentIndex()==3);m_materialCanvas->setSubmesh(m_submesh->currentIndex(),m_allSubmeshes->isChecked());
}

void MaterialPreviewWidget::loadSubmeshAssignments()
{
    m_submeshAssignments.clear();
    if(m_assignmentsMeshPath.isEmpty())return;
    QFile sidecar(m_assignmentsMeshPath+QStringLiteral(".pbrmset"));
    if(!sidecar.open(QIODevice::ReadOnly))return;
    const QJsonObject root=QJsonDocument::fromJson(sidecar.readAll()).object();
    const QJsonObject assignments=root.value(QStringLiteral("assignments")).toObject();
    const QDir meshDir=QFileInfo(m_assignmentsMeshPath).dir();
    for(auto it=assignments.begin();it!=assignments.end();++it){
        const QString stored=it.value().toString();
        if(stored.isEmpty())continue;
        const QString resolved=QDir::isAbsolutePath(stored)?stored:QDir::cleanPath(meshDir.filePath(stored));
        m_submeshAssignments.insert(it.key(),resolved);
    }
}

void MaterialPreviewWidget::saveSubmeshAssignments() const
{
    if(m_assignmentsMeshPath.isEmpty())return;
    const QString sidecarPath=m_assignmentsMeshPath+QStringLiteral(".pbrmset");
    if(m_submeshAssignments.isEmpty()){QFile::remove(sidecarPath);return;}
    const QDir meshDir=QFileInfo(m_assignmentsMeshPath).dir();
    QJsonObject assignments;
    for(auto it=m_submeshAssignments.begin();it!=m_submeshAssignments.end();++it){
        const QString relative=meshDir.relativeFilePath(it.value());
        assignments.insert(it.key(),relative.startsWith(QStringLiteral(".."))?it.value():relative);
    }
    QJsonObject root;
    root.insert(QStringLiteral("schema"),QStringLiteral("FO4.PBRM.MaterialSet"));
    root.insert(QStringLiteral("version"),1);
    root.insert(QStringLiteral("assignments"),assignments);
    QFile sidecar(sidecarPath);
    if(sidecar.open(QIODevice::WriteOnly))sidecar.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

void MaterialPreviewWidget::applySubmeshAssignments()
{
    QVector<QPair<QString,QString>> assignments;
    assignments.reserve(m_submeshAssignments.size());
    for(auto it=m_submeshAssignments.begin();it!=m_submeshAssignments.end();++it)
        assignments.append({it.key(),it.value()});
    m_materialCanvas->setCompanionMaterials(assignments);
}

void MaterialPreviewWidget::showSubmeshMaterialsDialog()
{
    const auto& mesh=m_materialCanvas->mesh();
    if(mesh.submeshes.isEmpty())return;
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Submesh Materials"));
    dialog.setMinimumWidth(560);
    auto* layout=new QVBoxLayout(&dialog);
    auto* info=new QLabel(tr("Assign a saved .pbrm material to each submesh. Unassigned submeshes use the material being edited (full fidelity); assigned submeshes render their material through the standard PBR path. Assignments are saved next to the mesh as %1.")
        .arg(QFileInfo(m_assignmentsMeshPath).fileName()+QStringLiteral(".pbrmset")),&dialog);
    info->setWordWrap(true);layout->addWidget(info);
    auto* grid=new QGridLayout;grid->setColumnStretch(1,1);layout->addLayout(grid);
    QVector<QLineEdit*> pathEdits;
    for(int i=0;i<mesh.submeshes.size();++i){
        const QString name=mesh.submeshes[i].name;
        grid->addWidget(new QLabel(name,&dialog),i,0);
        auto* pathEdit=new QLineEdit(m_submeshAssignments.value(name),&dialog);
        pathEdit->setPlaceholderText(tr("Edited material"));
        grid->addWidget(pathEdit,i,1);
        auto* browse=new QToolButton(&dialog);browse->setText(QStringLiteral("…"));
        connect(browse,&QToolButton::clicked,&dialog,[this,pathEdit]{
            const QString start=pathEdit->text().isEmpty()?QFileInfo(m_assignmentsMeshPath).path():QFileInfo(pathEdit->text()).path();
            const QString path=QFileDialog::getOpenFileName(pathEdit,tr("Choose Material"),start,tr("PBR Materials (*.pbrm)"));
            if(!path.isEmpty())pathEdit->setText(QDir::toNativeSeparators(path));
        });
        grid->addWidget(browse,i,2);
        auto* clear=new QToolButton(&dialog);clear->setText(tr("Clear"));
        connect(clear,&QToolButton::clicked,pathEdit,&QLineEdit::clear);
        grid->addWidget(clear,i,3);
        pathEdits.append(pathEdit);
    }
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    layout->addWidget(buttons);
    if(dialog.exec()!=QDialog::Accepted)return;
    m_submeshAssignments.clear();
    QStringList missing;
    for(int i=0;i<mesh.submeshes.size();++i){
        const QString path=pathEdits[i]->text().trimmed();
        if(path.isEmpty())continue;
        if(!QFileInfo::exists(path)){missing<<tr("%1: %2").arg(mesh.submeshes[i].name,path);continue;}
        m_submeshAssignments.insert(mesh.submeshes[i].name,path);
    }
    if(!missing.isEmpty())
        QMessageBox::warning(this,tr("Missing Materials"),tr("These assignments were skipped because the file does not exist:\n%1").arg(missing.join(QLatin1Char('\n'))));
    saveSubmeshAssignments();
    applySubmeshAssignments();
}

void MaterialPreviewWidget::applyLightingPreset(int index)
{
    struct Preset{double key,yaw,pitch,env,exposure;int temperature;};
    static const Preset values[]={{4,-35,45,1,0,5500},{7,-45,55,.65,.25,6500},{1.4,-25,35,1.6,0,9000},{2.1,25,25,.55,-.25,3200},{3.2,-30,40,.8,0,6500}};
    const auto& p=values[std::clamp(index,0,4)];m_applyingLightingPreset=true;m_deriveExrLightingPreferred=false;m_manualLightStored=false;
    QSignalBlocker deriveBlocker(m_deriveExrLighting),presetBlocker(m_lightingPreset),colorBlocker(m_keyColor),keyBlocker(m_keyIntensity),yawBlocker(m_keyYaw),pitchBlocker(m_keyPitch),environmentBlocker(m_environmentIntensity),exposureBlocker(m_exposure),temperatureBlocker(m_temperature);
    m_deriveExrLighting->setChecked(false);m_lightingPreset->setCurrentIndex(index);m_keyColor->setColor(temperatureColor(p.temperature));m_keyIntensity->setValue(p.key);m_keyYaw->setValue(p.yaw);m_keyPitch->setValue(p.pitch);m_environmentIntensity->setValue(p.env);m_exposure->setValue(p.exposure);
    const int tempIndex=m_temperature->findData(p.temperature);if(tempIndex>=0)m_temperature->setCurrentIndex(tempIndex);m_applyingLightingPreset=false;
    setDerivedLightingControls(false);m_materialCanvas->setDerivedEnvironmentShadow(false,0);m_materialCanvas->setLighting(float(m_keyIntensity->value()),m_keyColor->color(),float(m_keyYaw->value()),float(m_keyPitch->value()),float(m_environmentIntensity->value()),float(m_environmentYaw->value()),float(m_exposure->value()));savePreviewSettings();
}

void MaterialPreviewWidget::setDerivedLightingControls(bool derived)
{
    m_keyIntensity->setEnabled(!derived);m_temperature->setEnabled(!derived);m_keyColor->setEnabled(!derived);m_keyYaw->setEnabled(!derived);m_keyPitch->setEnabled(!derived);
    const QString explanation=derived?tr("The EXR provides all illumination; this analytic key-light control is disabled while only its shadow direction is derived."):QString();
    for(QWidget* control:{static_cast<QWidget*>(m_keyIntensity),static_cast<QWidget*>(m_temperature),static_cast<QWidget*>(m_keyColor),static_cast<QWidget*>(m_keyYaw),static_cast<QWidget*>(m_keyPitch)})control->setToolTip(explanation);
}

void MaterialPreviewWidget::captureManualLight()
{
    if(m_manualLightStored)return;
    m_manualKeyIntensity=m_keyIntensity->value();m_manualKeyYaw=m_keyYaw->value();m_manualKeyPitch=m_keyPitch->value();m_manualKeyColor=m_keyColor->color();m_manualTemperature=m_temperature->currentData().toInt();m_manualShadows=m_shadows->isChecked();m_manualLightStored=true;
}

void MaterialPreviewWidget::restoreManualLight()
{
    if(!m_manualLightStored)return;
    m_applyingDerivedLight=true;
    {
        QSignalBlocker intensityBlocker(m_keyIntensity),yawBlocker(m_keyYaw),pitchBlocker(m_keyPitch),colorBlocker(m_keyColor),temperatureBlocker(m_temperature),shadowBlocker(m_shadows);
        m_keyIntensity->setValue(m_manualKeyIntensity);m_keyYaw->setValue(m_manualKeyYaw);m_keyPitch->setValue(m_manualKeyPitch);m_keyColor->setColor(m_manualKeyColor);m_shadows->setChecked(m_manualShadows);
        const int temperatureIndex=m_temperature->findData(m_manualTemperature);m_temperature->setCurrentIndex(temperatureIndex);
    }
    m_manualLightStored=false;m_applyingDerivedLight=false;setDerivedLightingControls(false);m_materialCanvas->setDerivedEnvironmentShadow(false,0);
    m_materialCanvas->setLighting(float(m_keyIntensity->value()),m_keyColor->color(),float(m_keyYaw->value()),float(m_keyPitch->value()),float(m_environmentIntensity->value()),float(m_environmentYaw->value()),float(m_exposure->value()));m_materialCanvas->setShadows(m_shadows->isChecked(),float(m_shadowIntensity->value()));
}

void MaterialPreviewWidget::refreshDerivedEnvironmentLight(bool enableShadows)
{
    const bool wasActive=m_deriveExrLighting->isChecked();
    const PbrPreviewGL::EnvironmentLight light=m_materialCanvas->derivedEnvironmentLight(float(m_environmentYaw->value()));
    const bool active=light.valid&&m_deriveExrLightingPreferred;
    m_applyingDerivedLight=true;
    {
        QSignalBlocker deriveBlocker(m_deriveExrLighting);m_deriveExrLighting->setEnabled(light.valid);m_deriveExrLighting->setChecked(active);
    }
    if(active){
        captureManualLight();
        QSignalBlocker intensityBlocker(m_keyIntensity),yawBlocker(m_keyYaw),pitchBlocker(m_keyPitch),colorBlocker(m_keyColor),temperatureBlocker(m_temperature),presetBlocker(m_lightingPreset),shadowBlocker(m_shadows);
        m_keyIntensity->setValue(0);m_keyYaw->setValue(light.yaw);m_keyPitch->setValue(light.pitch);m_keyColor->setColor(light.color);m_temperature->setCurrentIndex(-1);m_lightingPreset->setCurrentIndex(5);if(enableShadows)m_shadows->setChecked(true);
        const float directionalWeight=std::clamp((light.intensity-1.0f)/7.0f,.2f,.85f);setDerivedLightingControls(true);m_materialCanvas->setDerivedEnvironmentShadow(true,directionalWeight);
        m_materialCanvas->setLighting(0,m_keyColor->color(),float(m_keyYaw->value()),float(m_keyPitch->value()),float(m_environmentIntensity->value()),float(m_environmentYaw->value()),float(m_exposure->value()));m_materialCanvas->setShadows(m_shadows->isChecked(),float(m_shadowIntensity->value()));
    }else if(wasActive)restoreManualLight();else{setDerivedLightingControls(false);m_materialCanvas->setDerivedEnvironmentShadow(false,0);}
    m_applyingDerivedLight=false;savePreviewSettings();
}

QColor MaterialPreviewWidget::temperatureColor(int kelvin) const
{
    const double t=std::clamp(kelvin,1000,40000)/100.0;double r,g,b;
    if(t<=66){r=255;g=99.4708025861*std::log(t)-161.1195681661;b=t<=19?0:138.5177312231*std::log(t-10)-305.0447927307;}
    else{r=329.698727446*std::pow(t-60,-.1332047592);g=288.1221695283*std::pow(t-60,-.0755148492);b=255;}
    return QColor(std::clamp(int(r),0,255),std::clamp(int(g),0,255),std::clamp(int(b),0,255));
}

void MaterialPreviewWidget::loadPreviewSettings()
{
    QSettings s;
    if (s.value(QStringLiteral("Preview/settingsVersion"), 0).toInt() != 1) {
        s.beginGroup(QStringLiteral("Preview")); s.remove(QString()); s.endGroup();
    }
    QSignalBlocker a(m_shape),b(m_projection),c(m_background),d(m_debug),e(m_ground),f(m_compare),h(m_planeOrientation),i(m_twoSided),j(m_allSubmeshes),k(m_uvSet),landscapeBlocker(m_landscapeLayer),aaBlocker(m_antiAliasing);
    QSignalBlocker l(m_keyIntensity),m(m_keyYaw),n(m_keyPitch),o(m_environmentIntensity),p(m_environmentYaw),q(m_exposure),r(m_temperature),shadowBlocker(m_shadows),shadowIntensityBlocker(m_shadowIntensity),deriveBlocker(m_deriveExrLighting),presetBlocker(m_lightingPreset);
    m_shape->setCurrentIndex(std::clamp(s.value(QStringLiteral("Preview/shape"),0).toInt(),0,3));
    m_planeOrientation->setCurrentIndex(std::clamp(s.value(QStringLiteral("Preview/planeOrientation"),0).toInt(),0,2));m_twoSided->setChecked(s.value(QStringLiteral("Preview/twoSided"),true).toBool());
    m_projection->setCurrentIndex(std::clamp(s.value(QStringLiteral("Preview/projection"),0).toInt(),0,1));m_background->setCurrentIndex(std::clamp(s.value(QStringLiteral("Preview/background"),0).toInt(),0,4));const int debugIndex=m_debug->findData(std::clamp(s.value(QStringLiteral("Preview/debug"),0).toInt(),0,18));m_debug->setCurrentIndex(debugIndex>=0?debugIndex:0);
    m_ground->setChecked(s.value(QStringLiteral("Preview/ground"),true).toBool());m_compare->setChecked(s.value(QStringLiteral("Preview/compare"),false).toBool());m_allSubmeshes->setChecked(s.value(QStringLiteral("Preview/allSubmeshes"),true).toBool());m_uvSet->setCurrentIndex(std::clamp(s.value(QStringLiteral("Preview/layers"),0).toInt(),0,3));m_landscapeLayer->setCurrentIndex(std::clamp(s.value(QStringLiteral("Preview/landscapeLayer"),0).toInt(),0,15));m_materialCanvas->setLandscapeLayer(m_landscapeLayer->currentIndex());
    const int aaIndex=m_antiAliasing->findData(s.value(QStringLiteral("Preview/antiAliasing"),4).toInt());m_antiAliasing->setCurrentIndex(aaIndex>=0?aaIndex:2);m_materialCanvas->setMsaaSamples(m_antiAliasing->currentData().toInt());
    {
        const auto restoreField=[&s](DragNumericEdit* field,const QString& key){QSignalBlocker blocker(field);field->setValue(s.value(key,0.0).toDouble());};
        restoreField(m_objectRotationX,QStringLiteral("Preview/objectRotationX"));restoreField(m_objectRotationY,QStringLiteral("Preview/objectRotationY"));restoreField(m_objectRotationZ,QStringLiteral("Preview/objectRotationZ"));
        restoreField(m_objectPositionX,QStringLiteral("Preview/objectPositionX"));restoreField(m_objectPositionY,QStringLiteral("Preview/objectPositionY"));restoreField(m_objectPositionZ,QStringLiteral("Preview/objectPositionZ"));
        m_materialCanvas->setObjectTransform(
            QVector3D(float(m_objectRotationX->value()),float(m_objectRotationY->value()),float(m_objectRotationZ->value())),
            QVector3D(float(m_objectPositionX->value()),float(m_objectPositionY->value()),float(m_objectPositionZ->value())));
    }
    m_keyIntensity->setValue(s.value(QStringLiteral("Preview/keyIntensity"),4).toDouble());m_keyYaw->setValue(s.value(QStringLiteral("Preview/keyYaw"),-35).toDouble());m_keyPitch->setValue(s.value(QStringLiteral("Preview/keyPitch"),45).toDouble());m_environmentIntensity->setValue(s.value(QStringLiteral("Preview/environmentIntensity"),1).toDouble());m_environmentYaw->setValue(s.value(QStringLiteral("Preview/environmentYaw"),0).toDouble());m_exposure->setValue(s.value(QStringLiteral("Preview/exposure"),0).toDouble());
    m_shadows->setChecked(s.value(QStringLiteral("Preview/shadows"),true).toBool());m_shadowIntensity->setValue(s.value(QStringLiteral("Preview/shadowIntensity"),.75).toDouble());m_shadowIntensity->setEnabled(m_shadows->isChecked());m_deriveExrLightingPreferred=s.value(QStringLiteral("Preview/deriveExrLighting"),true).toBool();m_lightingPreset->setCurrentIndex(5);
    const int temperature=m_temperature->findData(s.value(QStringLiteral("Preview/temperature"),5500).toInt());if(temperature>=0)m_temperature->setCurrentIndex(temperature);
    m_keyColor->setColor(QColor(s.value(QStringLiteral("Preview/keyColor"),temperatureColor(m_temperature->currentData().toInt()).name()).toString()));
    m_materialCanvas->setPlaneOptions(static_cast<PreviewMeshes::PlaneOrientation>(m_planeOrientation->currentIndex()),m_twoSided->isChecked());
    if(m_shape->currentIndex()==3){const QString path=s.value(QStringLiteral("Preview/customMesh"),s.value(QStringLiteral("Preview/customFbx"))).toString();QString error;if(!m_materialCanvas->loadCustomMesh(path,&error)){m_shape->setCurrentIndex(0);m_materialCanvas->setShape(PbrPreviewGL::Shape::Plane);}}
    else m_materialCanvas->setShape(static_cast<PbrPreviewGL::Shape>(m_shape->currentIndex()));
    m_materialCanvas->setProjection(static_cast<PbrPreviewGL::Projection>(m_projection->currentIndex()));m_materialCanvas->setBackground(static_cast<PbrPreviewGL::Background>(m_background->currentIndex()));m_materialCanvas->setDebugView(static_cast<PbrPreviewGL::DebugView>(m_debug->currentData().toInt()));m_materialCanvas->setGroundEnabled(m_ground->isChecked());m_materialCanvas->setComparisonEnabled(m_compare->isChecked());m_materialCanvas->setUvSet(m_uvSet->currentIndex());
    m_materialCanvas->setLighting(float(m_keyIntensity->value()),m_keyColor->color(),float(m_keyYaw->value()),float(m_keyPitch->value()),float(m_environmentIntensity->value()),float(m_environmentYaw->value()),float(m_exposure->value()));m_materialCanvas->setShadows(m_shadows->isChecked(),float(m_shadowIntensity->value()));reloadPreviewCubemaps();refreshMeshControls();savePreviewSettings();
}

void MaterialPreviewWidget::savePreviewSettings() const
{
    const bool preserveManual=m_manualLightStored&&m_deriveExrLighting->isChecked();
    QSettings s;s.setValue(QStringLiteral("Preview/settingsVersion"),1);s.setValue(QStringLiteral("Preview/shape"),m_shape->currentIndex());s.setValue(QStringLiteral("Preview/planeOrientation"),m_planeOrientation->currentIndex());s.setValue(QStringLiteral("Preview/twoSided"),m_twoSided->isChecked());s.setValue(QStringLiteral("Preview/projection"),m_projection->currentIndex());s.setValue(QStringLiteral("Preview/background"),m_background->currentIndex());s.setValue(QStringLiteral("Preview/debug"),m_debug->currentData());s.setValue(QStringLiteral("Preview/ground"),m_ground->isChecked());s.setValue(QStringLiteral("Preview/compare"),m_compare->isChecked());s.setValue(QStringLiteral("Preview/allSubmeshes"),m_allSubmeshes->isChecked());s.setValue(QStringLiteral("Preview/layers"),m_uvSet->currentIndex());s.setValue(QStringLiteral("Preview/landscapeLayer"),m_landscapeLayer->currentIndex());s.setValue(QStringLiteral("Preview/antiAliasing"),m_antiAliasing->currentData());s.setValue(QStringLiteral("Preview/objectRotationX"),m_objectRotationX->value());s.setValue(QStringLiteral("Preview/objectRotationY"),m_objectRotationY->value());s.setValue(QStringLiteral("Preview/objectRotationZ"),m_objectRotationZ->value());s.setValue(QStringLiteral("Preview/objectPositionX"),m_objectPositionX->value());s.setValue(QStringLiteral("Preview/objectPositionY"),m_objectPositionY->value());s.setValue(QStringLiteral("Preview/objectPositionZ"),m_objectPositionZ->value());s.setValue(QStringLiteral("Preview/keyIntensity"),preserveManual?m_manualKeyIntensity:m_keyIntensity->value());s.setValue(QStringLiteral("Preview/keyYaw"),preserveManual?m_manualKeyYaw:m_keyYaw->value());s.setValue(QStringLiteral("Preview/keyPitch"),preserveManual?m_manualKeyPitch:m_keyPitch->value());s.setValue(QStringLiteral("Preview/temperature"),preserveManual?m_manualTemperature:m_temperature->currentData());s.setValue(QStringLiteral("Preview/keyColor"),(preserveManual?m_manualKeyColor:m_keyColor->color()).name());s.setValue(QStringLiteral("Preview/shadows"),preserveManual?m_manualShadows:m_shadows->isChecked());s.setValue(QStringLiteral("Preview/shadowIntensity"),m_shadowIntensity->value());s.setValue(QStringLiteral("Preview/environmentIntensity"),m_environmentIntensity->value());s.setValue(QStringLiteral("Preview/environmentYaw"),m_environmentYaw->value());s.setValue(QStringLiteral("Preview/exposure"),m_exposure->value());s.setValue(QStringLiteral("Preview/deriveExrLighting"),m_deriveExrLightingPreferred);
}

void MaterialPreviewWidget::reloadPreviewCubemaps()
{
    QSettings s(PreviewEnvironmentSettings::filePath(),QSettings::IniFormat);PreviewEnvironmentSettings::migrateLegacy(s);const QString defaultPath=s.value(QStringLiteral("PreviewCubemaps/defaultPath"),QStringLiteral("Shared/Cubemaps/mipblur_DefaultOutside1.dds")).toString();
    const QString active=s.value(QStringLiteral("PreviewCubemaps/activePath"),defaultPath).toString();const QStringList names=s.value(QStringLiteral("PreviewCubemaps/names")).toStringList();const QStringList paths=s.value(QStringLiteral("PreviewCubemaps/paths")).toStringList();
    QSignalBlocker blocker(m_cubemap);m_cubemap->clear();m_cubemap->addItem(tr("Default — %1").arg(QFileInfo(defaultPath).fileName()),defaultPath);
    for(int i=0;i<qMin(names.size(),paths.size());++i)m_cubemap->addItem(names[i],paths[i]);
    int index=m_cubemap->findData(active);if(index<0)index=0;m_cubemap->setCurrentIndex(index);m_materialCanvas->setEnvironmentCubemap(m_cubemap->currentData().toString(),m_textureRoot);refreshDerivedEnvironmentLight(true);
}

void MaterialPreviewWidget::setTextureRoot(const QString& root){m_textureRoot=root;m_materialCanvas->setMaterial(m_document,m_textureRoot);reloadPreviewCubemaps();}
void MaterialPreviewWidget::setMaterial(const Pbr::MaterialDocument& document){m_document=document;const bool landscape=document.shaderMode()==Pbr::ShaderMode::LandscapeSurface;m_landscapeLayerLabel->setVisible(landscape);m_landscapeLayer->setVisible(landscape);m_uvSetLabel->setVisible(!landscape);m_uvSet->setVisible(!landscape);m_materialCanvas->setMaterial(m_document,m_textureRoot);rebuildStateControls();if(m_stack->currentWidget()==m_previewContainer)showFullMaterial();}

void MaterialPreviewWidget::rebuildStateControls()
{
    // v5: the simulator runs the active runtime definitions (built-in defaults
    // or a user-loaded definitions JSON), never material-authored states.
    const QVector<Pbr::SurfaceStateSpec> resolved=Pbr::activeSurfaceStateDefinitions();
    // Preserve simulated amounts across material edits by state name.
    QHash<QString,double> previous;
    for(int i=0;i<m_resolvedStates.size()&&i<m_stateFields.size();++i)
        previous.insert(m_resolvedStates[i].name,m_stateFields[i]->value());
    m_resolvedStates=resolved;
    auto* form=static_cast<QFormLayout*>(m_stateRows->layout());
    while(form->rowCount()>0)form->removeRow(0);
    m_stateFields.clear();
    const bool manual=m_weatherSim->currentIndex()==0;
    for(const auto& state:m_resolvedStates){
        auto* field=new DragNumericEdit(m_stateRows);
        field->setRange(0,1);field->setSingleStep(.005);field->setDecimals(3);field->setDragSensitivity(.004);
        field->setValue(previous.value(state.name,0.0));
        QStringList tags;
        if(state.tagLiquid)tags<<tr("liquid");if(state.tagParticulate)tags<<tr("particulate");if(state.tagFrozen)tags<<tr("frozen");
        if(state.tagThermal)tags<<tr("thermal");if(state.tagViscous)tags<<tr("viscous");
        field->setToolTip(tags.isEmpty()?tr("Untagged state; ignores the interaction physics."):tr("Tags: %1.").arg(tags.join(QStringLiteral(", "))));
        field->setEnabled(manual);
        connect(field,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this](double){applyStateAmounts();});
        form->addRow(state.name,field);
        m_stateFields.append(field);
    }
    applyStateAmounts();
}

void MaterialPreviewWidget::applyStateAmounts()
{
    QVector<float> raw;
    raw.reserve(m_stateFields.size());
    for(DragNumericEdit* field:m_stateFields)raw.append(float(field->value()));
    m_materialCanvas->setSurfaceStateAmounts(Pbr::surfaceStateInteractions(m_resolvedStates,raw));
}

void MaterialPreviewWidget::tickWeatherSimulation()
{
    // Looping 30 s event: build up, hold, then decay. The envelope is the
    // exposure target; each state's amount chases it at its own authored
    // gain/dry rate, so cloth soaks fast and stays damp long after the storm.
    m_weatherClock+=.1;
    const double phase=std::fmod(m_weatherClock,30.0)/30.0;
    const double buildUp=std::clamp(phase/.35,0.0,1.0);
    const double decay=std::clamp((1.0-phase)/.35,0.0,1.0);
    const double envelope=std::min(buildUp,decay);
    const int mode=m_weatherSim->currentIndex();
    for(int i=0;i<m_resolvedStates.size()&&i<m_stateFields.size();++i){
        const auto& state=m_resolvedStates[i];
        double target=0;
        if(mode==1&&state.tagLiquid&&!state.tagViscous)target=envelope;
        else if(mode==2&&state.tagParticulate)target=envelope;
        else if(mode==3){if(state.tagFrozen)target=envelope;else if(state.tagLiquid&&!state.tagViscous)target=envelope*.25;}
        else if(mode==4&&state.tagThermal)target=envelope;
        const double current=m_stateFields[i]->value();
        const double rate=target>current?state.gainRate:state.dryRate;
        const double next=current+(target-current)*std::min(1.0,rate*1.2*.1);
        QSignalBlocker blocker(m_stateFields[i]);
        m_stateFields[i]->setValue(next);
    }
    applyStateAmounts();
}

void MaterialPreviewWidget::inspectTexture(const QString& name,const QString& path,const QStringList& channels,const QColor& color)
{
    m_textureName=name;m_textureColor=color;m_textureError.clear();m_textureImage=TextureImage::load(path,m_textureRoot,&m_textureError);
    m_title->setText(name+tr(" • Texture Preview"));m_textureCanvas->showTexture(name,m_textureImage,color,QStringLiteral("RGBA"),m_textureError);m_stack->setCurrentWidget(m_textureCanvas);m_controls->hide();m_settingsToggle->hide();m_buttonHost->show();rebuildChannelButtons(channels);
}

void MaterialPreviewWidget::showFullMaterial()
{
    const int layerIndex=m_uvSet->currentIndex();const QString layer=layerIndex==1?tr("Secondary (UV1)"):(layerIndex==2?tr("Tertiary (UV2)"):(layerIndex==3?tr("Combined (All Layers)"):tr("Primary (UV0)")));
    m_title->setText(tr("Material Preview • %1").arg(layer));
    m_title->setToolTip(tr("Live editor preview includes texture channels and overrides, UV layering, transparency, environment lighting, parallax/displacement, and enabled shader-family effects. Custom NIF meshes are displayed in static bind pose; skinned animation is intentionally not evaluated."));
    // Returning from texture preview restores the settings only if the user
    // has them expanded — the disclosure toggle owns that preference.
    m_stack->setCurrentWidget(m_previewContainer);m_settingsToggle->show();m_controls->setVisible(m_settingsToggle->isChecked());
    while(QLayoutItem* item=m_buttons->takeAt(0)){delete item->widget();delete item;}m_buttonHost->hide();
}

void MaterialPreviewWidget::rebuildChannelButtons(const QStringList& channels)
{
    while(QLayoutItem* item=m_buttons->takeAt(0)){delete item->widget();delete item;}auto* full=new QPushButton(tr("Full"),m_buttonHost);connect(full,&QPushButton::clicked,this,&MaterialPreviewWidget::showFullMaterial);m_buttons->addWidget(full);
    QStringList names{QStringLiteral("RGBA")};names.append(channels);for(const QString& channel:names){auto* button=new QToolButton(m_buttonHost);button->setText(channel);button->setCheckable(true);button->setAutoExclusive(true);button->setChecked(channel==QStringLiteral("RGBA"));connect(button,&QToolButton::clicked,this,[this,channel]{m_textureCanvas->showTexture(m_textureName,m_textureImage,m_textureColor,channel,m_textureError);});m_buttons->addWidget(button);}m_buttons->addStretch();
}
