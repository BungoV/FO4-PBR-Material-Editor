#include "bgsmreader.h"
#include "clicommands.h"
#include "converternodes.h"
#include "convertworkspace.h"
#include "mainwindow.h"
#include "materialpreviewwidget.h"
#include "nodecanvas.h"
#include "textureimage.h"
#include "upscaler.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QElapsedTimer>
#include <QFont>
#include <QGroupBox>
#include <QHeaderView>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QStandardItemModel>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QTreeWidget>

#include <array>
#include <utility>

int main(int argc, char* argv[])
{
    QSurfaceFormat previewFormat;
    previewFormat.setRenderableType(QSurfaceFormat::OpenGL);
    previewFormat.setVersion(3, 3);
    previewFormat.setProfile(QSurfaceFormat::CoreProfile);
    previewFormat.setDepthBufferSize(24);
    // No surface multisampling: the viewport's own MSAA FBO owns AA. A
    // multisampled backing store would make the resolve blit illegal for any
    // sample count other than its own (8x MSAA rendered black through 8->4).
    QSurfaceFormat::setDefaultFormat(previewFormat);

    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("FO4CommunityShaders"));
    QCoreApplication::setApplicationName(QStringLiteral("PBRMaterialEditor"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    application.setWindowIcon(QIcon(QStringLiteral(":/app-icon.png")));

    // Compatibility alias; the pbrm.exe console companion is the primary CLI.
    const int validateIndex = application.arguments().indexOf(QStringLiteral("--validate"));
    if (validateIndex >= 0)
        return PbrmCli::runValidate(application.arguments().mid(validateIndex + 1));

    QFile style(QStringLiteral(":/style.qss"));
    if (style.open(QIODevice::ReadOnly))
        application.setStyleSheet(QString::fromUtf8(style.readAll()));

    // Renders a stock converter graph straight to a PNG, no GL and no window
    // flow needed. This is how node layout changes get checked: nothing else
    // shows whether ports, settings, and previews actually fit their boxes.
    const int graphShotIndex = application.arguments().indexOf(QStringLiteral("--graph-shot"));
    if (graphShotIndex >= 0 && graphShotIndex + 1 < application.arguments().size()) {
        const QStringList arguments = application.arguments();
        const bool fo76 = arguments.contains(QStringLiteral("--fo76"));
        NodeCanvas canvas(ConverterNodes::sharedRegistry());
        NodeGraph::Graph graph = fo76 ? ConverterNodes::defaultFo76Graph() : ConverterNodes::defaultFo4Graph();
        // --hide-ports checks the folded layout of a real graph, which is where
        // the source node's two dozen unused slots actually cost something.
        if (arguments.contains(QStringLiteral("--hide-ports")))
            for (NodeGraph::Node& node : graph.nodes) node.hideUnusedPorts = true;
        canvas.setGraph(graph);
        // Stand-in results so status lines, warnings, and the under-resolution
        // colouring all render without needing real textures on disk.
        QHash<QString, NodeGraph::NodeResult> results;
        int index = 0;
        for (const NodeGraph::Node& node : graph.nodes) {
            NodeGraph::NodeResult result;
            const bool undersized = (index % 4) == 1;
            result.status = undersized ? QStringLiteral("1024×1024 · BC7_UNORM_SRGB")
                                       : QStringLiteral("2048×2048 · BC7_UNORM_SRGB");
            if (undersized) result.warning = QStringLiteral("1024px — below the material's 2048px");
            results.insert(node.id, result);
            ++index;
        }
        canvas.setResults(results);
        canvas.resize(1800, 1100);
        canvas.show();
        const QString output = arguments.at(graphShotIndex + 1);
        QTimer::singleShot(400, &application, [&canvas, output, &application] {
            application.exit(canvas.grab().save(output) ? 0 : 3);
        });
        return application.exec();
    }

    // Renders a lone Levels node with a synthetic distribution, so the
    // histogram strip and its handles can be checked without a real material
    // loaded. The stand-in data is the shape this node exists to read: a tall
    // dielectric cluster low down and a sparse metal cluster high up, with the
    // gap between them where the black/white points belong.
    const int levelsShotIndex = application.arguments().indexOf(QStringLiteral("--levels-shot"));
    if (levelsShotIndex >= 0 && levelsShotIndex + 1 < application.arguments().size()) {
        NodeCanvas canvas(ConverterNodes::sharedRegistry());
        NodeGraph::Graph graph;
        const NodeGraph::NodeTypeSpec* spec = ConverterNodes::sharedRegistry().find(QStringLiteral("image.levels"));
        if (!spec) return 4;
        // Four copies, one per fold state, so a layout change is checked in
        // every configuration at once rather than only the default one.
        struct Variant { const char* label; bool preview; bool settings; bool collapsed; int channels; };
        static const Variant variants[] = {
            {"Levels — full", true, true, false, NodeGraph::kChannelsRgb},
            {"Levels — green only", true, true, false, NodeGraph::ChannelG},
            {"Levels — no settings", true, false, false, NodeGraph::ChannelA},
            {"Levels — collapsed", true, true, true, NodeGraph::kChannelsRgb},
        };
        QHash<QString, NodeGraph::NodeResult> results;
        NodeGraph::NodeResult result;
        result.status = QStringLiteral("1024×1024");
        result.histogram.assign(NodeGraph::kHistogramChannels * NodeGraph::kHistogramBuckets, 0u);
        for (int c = 0; c < NodeGraph::kHistogramChannels; ++c) {
            quint32* buckets = result.histogram.data() + c * NodeGraph::kHistogramBuckets;
            for (int i = 0; i < NodeGraph::kHistogramBuckets; ++i) {
                const double x = i / 255.0;
                const double paint = std::exp(-std::pow((x - 0.05) / 0.03, 2.0)) * 9000.0;
                const double metal = std::exp(-std::pow((x - 0.72) / 0.09, 2.0)) * 700.0;
                buckets[i] = quint32(paint + metal);
            }
        }
        double x = 40;
        for (const Variant& variant : variants) {
            const QString id = graph.addNode(QStringLiteral("image.levels"), *spec, QPointF(x, 40));
            NodeGraph::Node* added = graph.node(id);
            added->label = QString::fromUtf8(variant.label);
            added->params.insert(QStringLiteral("inBlack"), 0.18);
            added->params.insert(QStringLiteral("inWhite"), 0.42);
            added->params.insert(QStringLiteral("gamma"), 1.6);
            added->showPreview = variant.preview;
            added->showSettings = variant.settings;
            added->collapsed = variant.collapsed;
            added->previewChannels = variant.channels;
            results.insert(id, result);
            x += NodeGraph::Metrics::kNodeWidth + 30;
        }
        // A Curves node alongside them, with a deliberately steep curve: this is
        // where a non-monotone interpolation would visibly ring.
        if (const NodeGraph::NodeTypeSpec* curveSpec =
                ConverterNodes::sharedRegistry().find(QStringLiteral("image.curves"))) {
            const QString id = graph.addNode(QStringLiteral("image.curves"), *curveSpec, QPointF(x, 40));
            graph.node(id)->params.insert(QStringLiteral("points"),
                QStringLiteral("[[0,0],[0.35,0.08],[0.62,0.9],[1,1]]"));
            results.insert(id, result);
        }
        canvas.setGraph(graph);
        canvas.setResults(results);
        canvas.resize(1420, 500);
        canvas.show();
        const QString output = application.arguments().at(levelsShotIndex + 1);
        QTimer::singleShot(400, &application, [&canvas, output, &application] {
            application.exit(canvas.grab().save(output) ? 0 : 3);
        });
        return application.exec();
    }

    // Runs a real material through its stock graph and reports what every node
    // produced — mean colour per output, plus the terminal's maps written out
    // as PNGs. Looking at a thumbnail cannot tell "white" from "missing", and
    // this is the only way to see where in the chain a map goes wrong.
    const int evalIndex = application.arguments().indexOf(QStringLiteral("--graph-eval"));
    if (evalIndex >= 0 && evalIndex + 2 < application.arguments().size()) {
        const QStringList arguments = application.arguments();
        const QString materialPath = arguments.at(evalIndex + 1);
        const QDir outputDir(arguments.at(evalIndex + 2));
        const int rootIndex = arguments.indexOf(QStringLiteral("--texture-root"));
        const QString textureRoot = rootIndex >= 0 && rootIndex + 1 < arguments.size()
            ? arguments.at(rootIndex + 1) : QString();
        outputDir.mkpath(QStringLiteral("."));

        Bgsm::Material material;
        if (!Bgsm::readFile(materialPath, &material, nullptr)) return 2;
        const NodeGraph::Graph graph = ConverterNodes::defaultGraphFor(material);
        const NodeGraph::Registry& registry = ConverterNodes::sharedRegistry();
        NodeGraph::EvalContext context;
        context.sourcePath = materialPath;
        context.textureRoot = textureRoot;
        context.previewMode = true;
        context.materialMaxResolution = ConverterNodes::materialMaxResolution(material, textureRoot);
        // Same policy the workspace and a conversion read, or this harness
        // reports on a pipeline nobody actually runs.
        context.upscalePolicy = QSettings().value(QStringLiteral("Convert/upscalePolicy"), 0).toInt();
        NodeGraph::Evaluator evaluator(registry);
        const QHash<QString, NodeGraph::NodeResult> results = evaluator.evaluate(graph, context);

        // Mean of each channel: a map that arrived flat white reads 255,255,255
        // here, and one that never arrived has no line at all.
        const auto describe = [](const QImage& image) {
            if (image.isNull()) return QStringLiteral("(none)");
            const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
            double sums[4] = {0, 0, 0, 0};
            for (int y = 0; y < rgba.height(); ++y) {
                const uchar* line = rgba.constScanLine(y);
                for (int x = 0; x < rgba.width(); ++x)
                    for (int c = 0; c < 4; ++c) sums[c] += line[x * 4 + c];
            }
            const double texels = double(rgba.width()) * rgba.height();
            // Percentiles of the red channel alongside the means. A mean says
            // nothing about where to put a Levels handle — the threshold has to
            // be placed against the spread, and this is that spread in text.
            std::array<int, 256> buckets{}, greenBuckets{};
            qint64 overHalf = 0;
            for (int y = 0; y < rgba.height(); ++y) {
                const uchar* line = rgba.constScanLine(y);
                for (int x = 0; x < rgba.width(); ++x) {
                    ++buckets[line[x * 4]];
                    ++greenBuckets[line[x * 4 + 1]];
                    if (line[x * 4 + 1] > 127) ++overHalf; // RMAOS.G is metallic
                }
            }
            const auto pick = [texels](const std::array<int, 256>& source, double fraction) {
                const double target = fraction * texels;
                double seen = 0;
                for (int i = 0; i < 256; ++i) { seen += source[i]; if (seen >= target) return i; }
                return 255;
            };
            return QStringLiteral("%1x%2 mean %3,%4,%5,%6  R p50=%7 p90=%8 p99=%9"
                                  "  G p90=%10 p99=%11 over-half=%12%")
                .arg(rgba.width()).arg(rgba.height())
                .arg(sums[0] / texels, 0, 'f', 1).arg(sums[1] / texels, 0, 'f', 1)
                .arg(sums[2] / texels, 0, 'f', 1).arg(sums[3] / texels, 0, 'f', 1)
                .arg(pick(buckets, 0.50)).arg(pick(buckets, 0.90)).arg(pick(buckets, 0.99))
                .arg(pick(greenBuckets, 0.90)).arg(pick(greenBuckets, 0.99))
                .arg(100.0 * overHalf / texels, 0, 'f', 2);
        };

        QStringList report;
        report << QStringLiteral("%1 -> %2 nodes").arg(materialPath).arg(graph.nodes.size());
        for (const NodeGraph::Node& node : graph.nodes) {
            const NodeGraph::NodeTypeSpec* spec = registry.find(node.typeId);
            const NodeGraph::NodeResult& result = results.value(node.id);
            // Status carries what a node DECIDED — the auto-levels verdict lives
            // here and nowhere else, so a survey without it cannot tell whether
            // the classifier adapted per material or returned the same answer.
            if (!result.status.isEmpty())
                report << QStringLiteral("    status: %1").arg(result.status);
            report << QStringLiteral("[%1] %2%3").arg(node.typeId,
                spec ? spec->name : QStringLiteral("?"),
                result.error.isEmpty() ? QString() : QStringLiteral("  ERROR: ") + result.error);
            for (auto it = result.outputs.constBegin(); it != result.outputs.constEnd(); ++it) {
                if (it.value().type() != NodeGraph::ValueType::Image) continue;
                report << QStringLiteral("    %1: %2").arg(it.key(), describe(it.value().image()));
            }
            // The terminal's maps also come out as files, so a wrong one can be
            // looked at rather than inferred from a mean.
            if (node.typeId != QStringLiteral("output.pbrMaterial")) continue;
            for (auto it = result.outputs.constBegin(); it != result.outputs.constEnd(); ++it)
                if (it.value().type() == NodeGraph::ValueType::Image && !it.value().image().isNull())
                    it.value().image().save(outputDir.filePath(it.key() + QStringLiteral(".png")));
        }
        QFile reportFile(outputDir.filePath(QStringLiteral("graph-eval.txt")));
        if (reportFile.open(QIODevice::WriteOnly | QIODevice::Text))
            reportFile.write(report.join(QChar('\n')).toUtf8() + '\n');

        // --shot also drives the preview the way the workspace does: the live
        // material on the shaded path, the terminal's BC on the node-output
        // path, split down the middle. A map that evaluates correctly but
        // reaches the viewport wrong is only visible here.
        if (!arguments.contains(QStringLiteral("--shot"))) return 0;
        const NodeGraph::Node* terminal = nullptr;
        for (const NodeGraph::Node& node : graph.nodes)
            if (node.typeId == QStringLiteral("output.pbrMaterial")) terminal = &node;
        const auto intoTerminal = [&](const char* portId) {
            if (!terminal) return QImage();
            for (const NodeGraph::Connection& link : graph.connections)
                if (link.toNode == terminal->id && link.toPort == QLatin1String(portId))
                    return results.value(link.fromNode).outputs.value(link.fromPort).image();
            return QImage();
        };
        auto* preview = new MaterialPreviewWidget(nullptr, true);
        preview->resize(1000, 700);
        preview->show();
        preview->canvas()->setLiveMaterialImages(intoTerminal("baseColor"), intoTerminal("normal"),
            intoTerminal("rmaos"), intoTerminal("emissive"));
        preview->canvas()->setNodeOutputImage(intoTerminal("baseColor"));
        // The original maps too, so the comparison half has something to shade.
        // Without these the converter's "Original (FO76)" side is just grey and
        // any split test would be measuring nothing.
        {
            const auto loadMap = [&textureRoot](const QString& authored) {
                return authored.isEmpty() ? QImage() : TextureImage::load(authored, textureRoot, nullptr);
            };
            preview->canvas()->setSourceComparisonImages(loadMap(material.diffuseTexture),
                loadMap(!material.specularTexture.isEmpty() ? material.specularTexture : material.smoothSpecTexture),
                loadMap(material.normalTexture),
                loadMap(!material.lightingTexture.isEmpty() ? material.lightingTexture : material.greyscaleTexture),
                loadMap(material.glowTexture));
            PbrPreviewGL::NativeMaterial native;
            native.valid = true;
            native.fallout76 = material.isFallout76();
            native.specularColor = material.specularColor;
            native.specularMult = material.specularMult;
            native.smoothness = material.smoothness;
            native.envMapScale = material.environmentMappingMaskScale;
            native.specularEnabled = material.specularEnabled;
            native.emitEnabled = material.emitEnabled;
            native.emittanceColor = material.emittanceColor;
            native.emittanceMult = material.emittanceMult;
            preview->canvas()->setNativeMaterial(native);
        }
        // Exactly the converter's arrangement: the selected node on the left,
        // the original on the right, split down the middle.
        if (arguments.contains(QStringLiteral("--split"))) {
            preview->canvas()->setDebugSplit(0.5f);
            preview->setRenderModeLeft(PbrPreviewGL::DebugView::Shaded);
            preview->setRenderMode(PbrPreviewGL::DebugView::NativeFo76);
            QTimer::singleShot(2500, &application, [preview, outputDir, &application] {
                const bool saved = preview->grab().save(outputDir.filePath(QStringLiteral("split.png")));
                delete preview;
                application.exit(saved ? 0 : 3);
            });
            return application.exec();
        }
        preview->canvas()->setDebugSplit(0.0f);
        // One shot per channel the shader reads. Comparing these against the
        // means above says whether a map arrived wrong or is being DECODED
        // wrong, which no single shaded frame can tell apart.
        // key/env of -1 leaves the restored rig alone; the last three re-light
        // the same shaded frame, which is how "does lighting reach this at all"
        // gets answered by comparison instead of by eye.
        struct View { PbrPreviewGL::DebugView mode; const char* name; float key; float env; };
        static const View views[] = {{PbrPreviewGL::DebugView::Shaded, "shaded", -1, -1},
            {PbrPreviewGL::DebugView::BaseColor, "basecolor", -1, -1},
            {PbrPreviewGL::DebugView::Roughness, "roughness", -1, -1},
            {PbrPreviewGL::DebugView::Metallic, "metallic", -1, -1},
            {PbrPreviewGL::DebugView::AmbientOcclusion, "ao", -1, -1},
            {PbrPreviewGL::DebugView::F0, "f0", -1, -1},
            {PbrPreviewGL::DebugView::Normals, "normals", -1, -1},
            {PbrPreviewGL::DebugView::NodeOutput, "nodeoutput", -1, -1},
            {PbrPreviewGL::DebugView::Shaded, "lit-dark", 0.2f, 0.05f},
            {PbrPreviewGL::DebugView::Shaded, "lit-mid", 4.0f, 1.0f},
            {PbrPreviewGL::DebugView::Shaded, "lit-bright", 12.0f, 3.0f}};
        // With an EXR loaded the key light is derived and switched off; the
        // checkbox is the way back to a manual key. --manual-key unticks it, so
        // the two runs can be compared to prove the toggle reaches the render.
        if (arguments.contains(QStringLiteral("--manual-key")))
            for (QCheckBox* box : preview->findChildren<QCheckBox*>())
                if (box->text().contains(QStringLiteral("EXR")) && box->isChecked()) box->setChecked(false);
        auto* index = new int(0);
        auto* timer = new QTimer(preview);
        timer->setInterval(700);
        QObject::connect(timer, &QTimer::timeout, preview, [preview, outputDir, index, timer, &application] {
            const int count = int(std::size(views));
            // Tick N saves what tick N-1 asked for, so every frame has had a
            // full interval to render before it is grabbed.
            if (*index > 0)
                preview->grab().save(outputDir.filePath(QLatin1String(views[*index - 1].name)
                    + QStringLiteral(".png")));
            if (*index >= count) { timer->stop(); delete preview; application.exit(0); return; }
            const View& view = views[*index];
            if (view.key >= 0)
                preview->canvas()->setLighting(view.key, QColor(255, 237, 222), -35, 45, view.env, 0, 0);
            preview->setRenderMode(view.mode);
            ++(*index);
        });
        timer->start();
        return application.exec();
    }

    // Asks whether the diffuse gate in the metal mask earns its place. The mask
    // is reflectance x darkDiffuse, on the theory that a metal has a near-black
    // albedo — so within one material the high-_r texels should be markedly
    // darker in _d than the low-_r ones. If they are not, the gate is voting on
    // nothing and should go. Compares the two populations texel by texel.
    const int albedoIndex = application.arguments().indexOf(QStringLiteral("--survey-albedo"));
    if (albedoIndex >= 0 && albedoIndex + 2 < application.arguments().size()) {
        const QStringList arguments = application.arguments();
        const QString root = arguments.at(albedoIndex + 1);
        const QString reportPath = arguments.at(albedoIndex + 2);
        const int rootIndex = arguments.indexOf(QStringLiteral("--texture-root"));
        const QString textureRoot = rootIndex >= 0 && rootIndex + 1 < arguments.size()
            ? arguments.at(rootIndex + 1) : QString();
        const int limitIndex = arguments.indexOf(QStringLiteral("--limit"));
        const int limit = limitIndex >= 0 && limitIndex + 1 < arguments.size()
            ? arguments.at(limitIndex + 1).toInt() : 300;

        QStringList rows;
        rows << QStringLiteral("material\tmetalPct\tdiffuseOnMetal\tdiffuseOnDielectric\tratio");
        QDirIterator walker(root, {QStringLiteral("*.bgsm")}, QDir::Files, QDirIterator::Subdirectories);
        int seen = 0;
        const auto luma = [](const uchar* texel) {
            return 0.2126 * texel[0] + 0.7152 * texel[1] + 0.0722 * texel[2];
        };
        while (walker.hasNext() && seen < limit) {
            const QString path = walker.next();
            Bgsm::Material material;
            if (!Bgsm::readFile(path, &material, nullptr) || !material.isFallout76()) continue;
            const QString reflectancePath = !material.specularTexture.isEmpty()
                ? material.specularTexture : material.smoothSpecTexture;
            if (reflectancePath.isEmpty() || material.diffuseTexture.isEmpty()) continue;
            QImage reflectance = TextureImage::load(reflectancePath, textureRoot, nullptr);
            const QImage diffuse = TextureImage::load(material.diffuseTexture, textureRoot, nullptr);
            if (reflectance.isNull() || diffuse.isNull()) continue;
            // Same normalisation the converter applies, so the thresholds below
            // mean what they mean in the graph.
            const TextureImage::Diagnostics info = TextureImage::inspect(reflectancePath, textureRoot);
            reflectance = ConverterNodes::normaliseReflectanceToSrgbScale(reflectance,
                info.colorSpace.contains(QLatin1String("sRGB"), Qt::CaseInsensitive));
            reflectance = reflectance.convertToFormat(QImage::Format_RGBA8888);
            const QImage diffuseRgba = diffuse.convertToFormat(QImage::Format_RGBA8888);

            double metalSum = 0, metalCount = 0, dielectricSum = 0, dielectricCount = 0;
            for (int y = 0; y < reflectance.height(); ++y) {
                const uchar* rLine = reflectance.constScanLine(y);
                const uchar* dLine = diffuseRgba.constScanLine(
                    std::min(y * diffuseRgba.height() / reflectance.height(), diffuseRgba.height() - 1));
                for (int x = 0; x < reflectance.width(); ++x) {
                    const double r = luma(rLine + x * 4);
                    const double d = luma(dLine
                        + std::min(x * diffuseRgba.width() / reflectance.width(), diffuseRgba.width() - 1) * 4);
                    if (r >= 0.70 * 255) { metalSum += d; ++metalCount; }
                    else if (r <= 0.25 * 255) { dielectricSum += d; ++dielectricCount; }
                }
            }
            // Only materials that actually contain metal can answer the question.
            const double texels = double(reflectance.width()) * reflectance.height();
            if (metalCount < texels * 0.005 || dielectricCount < texels * 0.005) continue;
            ++seen;
            const double onMetal = metalSum / metalCount, onDielectric = dielectricSum / dielectricCount;
            rows << QStringLiteral("%1\t%2\t%3\t%4\t%5").arg(QDir(root).relativeFilePath(path))
                        .arg(100.0 * metalCount / texels, 0, 'f', 1)
                        .arg(onMetal, 0, 'f', 1).arg(onDielectric, 0, 'f', 1)
                        .arg(onDielectric > 0 ? onMetal / onDielectric : 0.0, 0, 'f', 2);
        }
        QFile reportFile(reportPath);
        if (reportFile.open(QIODevice::WriteOnly | QIODevice::Text))
            reportFile.write(rows.join(QChar('\n')).toUtf8() + '\n');
        return 0;
    }

    // Writes a stock graph to a .pbrgraph, so `pbrm convert --graph` can be
    // pointed at the same pipeline the GUI runs. Without this the graph
    // conversion path could only be exercised by hand through the workspace.
    const int saveGraphIndex = application.arguments().indexOf(QStringLiteral("--save-graph"));
    if (saveGraphIndex >= 0 && saveGraphIndex + 1 < application.arguments().size()) {
        const bool fo76 = application.arguments().contains(QStringLiteral("--fo76"));
        QString error;
        const bool saved = ConverterNodes::saveGraphFile(
            fo76 ? ConverterNodes::defaultFo76Graph() : ConverterNodes::defaultFo4Graph(),
            application.arguments().at(saveGraphIndex + 1), &error);
        return saved ? 0 : 5;
    }

    // Walks a materials tree and reports, per FO76 material, what its _r map
    // actually contains alongside how the material file scales it. The question
    // this exists to answer is why some _r maps span the full range and others
    // sit in a narrow band, which decides whether one pair of Levels thresholds
    // can ever serve every material.
    // Actually RUNS the upscaler, per role, and reports what came back. The
    // table verb below only reports which model a role resolves to; this is the
    // one that proves the process is invoked correctly, the temp staging works,
    // and a 4x model serves a 2x request. Slow by nature — it runs real
    // networks — so it is its own verb rather than part of a suite.
    const int upTestIndex = application.arguments().indexOf(QStringLiteral("--upscale-test"));
    if (upTestIndex >= 0) {
        QImage source(64, 64, QImage::Format_RGBA8888);
        for (int y = 0; y < source.height(); ++y)
            for (int x = 0; x < source.width(); ++x)
                source.setPixel(x, y, qRgba(x * 4 % 256, y * 4 % 256, (x + y) * 2 % 256, 255));

        QStringList lines;
        lines << QStringLiteral("role\tfactor\tin\tout\texpected\tengine\tnote");
        struct Case { Upscaler::MapSlot slot; int factor; };
        for (const Case& probe : {Case{Upscaler::MapSlot::BaseColor, 4},
                 Case{Upscaler::MapSlot::BaseColor, 2},   // 4x model, 2x asked: must resample down
                 Case{Upscaler::MapSlot::Smoothness, 4},
                 Case{Upscaler::MapSlot::Normal, 4}}) {
            Upscaler::Request request;
            request.image = source;
            request.slot = probe.slot;
            request.kind = Upscaler::kindForSlot(probe.slot);
            request.factor = probe.factor;
            const Upscaler::Result result = Upscaler::upscale(request);
            lines << QStringLiteral("%1\t%2x\t%3\t%4\t%5\t%6\t%7")
                        .arg(Upscaler::slotLabel(probe.slot)).arg(probe.factor)
                        .arg(QStringLiteral("64x64"))
                        .arg(QStringLiteral("%1x%2").arg(result.image.width()).arg(result.image.height()))
                        .arg(QStringLiteral("%1x%1").arg(64 * probe.factor))
                        .arg(result.engine, result.note.isEmpty() ? QStringLiteral("-") : result.note);
        }
        const Upscaler::Result restored = Upscaler::restore(source);
        lines << QStringLiteral("restore (1:1)\t1x\t64x64\t%1\t64x64\t%2\t%3")
                    .arg(QStringLiteral("%1x%2").arg(restored.image.width()).arg(restored.image.height()))
                    .arg(restored.engine,
                        restored.note.isEmpty() ? QStringLiteral("-") : restored.note);

        const QString text = lines.join(QChar('\n')) + QChar('\n');
        if (upTestIndex + 1 < application.arguments().size()
            && !application.arguments().at(upTestIndex + 1).startsWith(QLatin1Char('-'))) {
            QFile out(application.arguments().at(upTestIndex + 1));
            if (out.open(QIODevice::WriteOnly | QIODevice::Text)) out.write(text.toUtf8());
        }
        return 0;
    }

    // What each map role actually resolves to on this machine. The table is a
    // dialog, but the answer it produces is what a conversion uses, and that is
    // worth being able to read without opening a window.
    const int modelsIndex = application.arguments().indexOf(QStringLiteral("--upscale-models"));
    if (modelsIndex >= 0) {
        QStringList lines;
        lines << QStringLiteral("upscaler\t%1").arg(Upscaler::binaryPath().isEmpty()
            ? QStringLiteral("(none configured)") : Upscaler::binaryPath());
        lines << QStringLiteral("installed\t%1").arg(Upscaler::models().join(QStringLiteral(", ")));
        lines << QString() << QStringLiteral("role\tmodel\tscale\tsource\thandling");
        for (const Upscaler::MapSlot slot : Upscaler::allSlots()) {
            const QString model = Upscaler::modelForSlot(slot);
            const int scale = Upscaler::modelNativeScale(model);
            const Upscaler::MapKind kind = Upscaler::kindForSlot(slot);
            lines << QStringLiteral("%1\t%2\t%3\t%4\t%5")
                        .arg(Upscaler::slotLabel(slot),
                            model.isEmpty() ? QStringLiteral("Lanczos") : model,
                            model.isEmpty() ? QStringLiteral("-")
                                            : (scale ? QStringLiteral("%1x").arg(scale)
                                                     : QStringLiteral("unstated")),
                            Upscaler::slotModelIsPinned(slot) ? QStringLiteral("pinned")
                                                              : QStringLiteral("suggested"),
                            kind == Upscaler::MapKind::Normal ? QStringLiteral("RG0 in, Z rebuilt")
                                : kind == Upscaler::MapKind::Color ? QStringLiteral("picture")
                                                                   : QStringLiteral("data"));
        }
        const QString text = lines.join(QChar('\n')) + QChar('\n');
        if (modelsIndex + 1 < application.arguments().size()
            && !application.arguments().at(modelsIndex + 1).startsWith(QLatin1Char('-'))) {
            QFile out(application.arguments().at(modelsIndex + 1));
            if (out.open(QIODevice::WriteOnly | QIODevice::Text)) out.write(text.toUtf8());
        }
        return 0;
    }

    // Does FO76's smoothness channel (_l.R) use its full range, or is it
    // compressed the way _r turned out to be? The whole reflectance lesson was
    // that "0-1 means 0-1" is not true of this corpus, and roughness = 1 - _l.R
    // assumes exactly that. Same shape as --survey-r, one channel over.
    const int glossIndex = application.arguments().indexOf(QStringLiteral("--survey-gloss"));
    if (glossIndex >= 0 && glossIndex + 2 < application.arguments().size()) {
        const QStringList arguments = application.arguments();
        const QString root = arguments.at(glossIndex + 1);
        const QString reportPath = arguments.at(glossIndex + 2);
        const int rootIndex = arguments.indexOf(QStringLiteral("--texture-root"));
        const QString textureRoot = rootIndex >= 0 && rootIndex + 1 < arguments.size()
            ? arguments.at(rootIndex + 1) : QString();
        const int limitIndex = arguments.indexOf(QStringLiteral("--limit"));
        const int limit = limitIndex >= 0 && limitIndex + 1 < arguments.size()
            ? arguments.at(limitIndex + 1).toInt() : 400;

        QStringList rows;
        rows << QStringLiteral("material\tsmoothnessScalar\tpbr\tlPath\tformat\tspace\twidth\theight"
                               "\tp1\tp10\tp50\tp90\tp99\tspread\tatBlackPct\tatWhitePct\tflat");
        // Aggregates, split by compression: the reflectance survey only made
        // sense once BC1 and BC4 were separated, because they turned out to be
        // two different authoring conventions wearing the same channel.
        QHash<QString, QVector<int>> p1ByFormat, p99ByFormat;
        int flatMaps = 0, reachesBlack = 0, reachesWhite = 0, reachesBoth = 0, scanned = 0;

        QDirIterator walker(root, {QStringLiteral("*.bgsm"), QStringLiteral("*.bgem")},
            QDir::Files, QDirIterator::Subdirectories);
        while (walker.hasNext() && scanned < limit) {
            const QString path = walker.next();
            Bgsm::Material material;
            if (!Bgsm::readFile(path, &material, nullptr)) continue;
            if (!material.isFallout76()) continue;
            if (material.lightingTexture.trimmed().isEmpty()) continue;
            const QImage map = TextureImage::load(material.lightingTexture, textureRoot, nullptr);
            if (map.isNull()) continue;
            const TextureImage::Diagnostics info =
                TextureImage::inspect(material.lightingTexture, textureRoot);
            ++scanned;

            // Channel 0 only, stored values, no transfer applied: this is the
            // number the invert actually operates on.
            const QImage rgba = map.convertToFormat(QImage::Format_RGBA8888);
            std::array<int, 256> buckets{};
            for (int y = 0; y < rgba.height(); ++y) {
                const uchar* line = rgba.constScanLine(y);
                for (int x = 0; x < rgba.width(); ++x) ++buckets[line[x * 4]];
            }
            const double texels = double(rgba.width()) * rgba.height();
            const auto percentile = [&buckets, texels](double fraction) {
                double seen = 0;
                for (int i = 0; i < 256; ++i) {
                    seen += buckets[i];
                    if (seen >= fraction * texels) return i;
                }
                return 255;
            };
            const int p1 = percentile(0.01), p10 = percentile(0.10), p50 = percentile(0.50);
            const int p90 = percentile(0.90), p99 = percentile(0.99);
            double atBlack = 0, atWhite = 0;
            for (int i = 0; i <= 2; ++i) atBlack += buckets[i];
            for (int i = 253; i <= 255; ++i) atWhite += buckets[i];
            // "Flat" means the map carries no smoothness variation at all — a
            // constant that the .bgsm scalar may be the real control for.
            const bool flat = (p99 - p1) < 8;

            const QString format = info.compressionLabel().isEmpty() ? QStringLiteral("?")
                                                                     : info.compressionLabel();
            p1ByFormat[format].append(p1);
            p99ByFormat[format].append(p99);
            if (flat) ++flatMaps;
            const bool black = p1 <= 4, white = p99 >= 250;
            if (black) ++reachesBlack;
            if (white) ++reachesWhite;
            if (black && white) ++reachesBoth;

            rows << QStringLiteral("%1\t%2\t%3\t%4\t%5\t%6\t%7\t%8\t%9\t%10\t%11\t%12\t%13\t%14\t%15\t%16\t%17")
                        .arg(QDir(root).relativeFilePath(path))
                        .arg(material.smoothness, 0, 'f', 3).arg(material.pbr ? 1 : 0)
                        .arg(material.lightingTexture, format,
                            info.colorSpace.isEmpty() ? QStringLiteral("?") : info.colorSpace)
                        .arg(info.width).arg(info.height)
                        .arg(p1).arg(p10).arg(p50).arg(p90).arg(p99).arg(p99 - p1)
                        .arg(100.0 * atBlack / texels, 0, 'f', 1)
                        .arg(100.0 * atWhite / texels, 0, 'f', 1)
                        .arg(flat ? 1 : 0);
        }

        const auto median = [](QVector<int> values) {
            if (values.isEmpty()) return -1;
            std::sort(values.begin(), values.end());
            return values.at(values.size() / 2);
        };
        rows << QString() << QStringLiteral("== summary ==");
        rows << QStringLiteral("materials scanned\t%1").arg(scanned);
        rows << QStringLiteral("flat (no variation)\t%1\t%2%")
                    .arg(flatMaps).arg(scanned ? 100.0 * flatMaps / scanned : 0.0, 0, 'f', 1);
        rows << QStringLiteral("p1 reaches black\t%1\t%2%")
                    .arg(reachesBlack).arg(scanned ? 100.0 * reachesBlack / scanned : 0.0, 0, 'f', 1);
        rows << QStringLiteral("p99 reaches white\t%1\t%2%")
                    .arg(reachesWhite).arg(scanned ? 100.0 * reachesWhite / scanned : 0.0, 0, 'f', 1);
        rows << QStringLiteral("uses the full range\t%1\t%2%")
                    .arg(reachesBoth).arg(scanned ? 100.0 * reachesBoth / scanned : 0.0, 0, 'f', 1);
        for (auto it = p1ByFormat.constBegin(); it != p1ByFormat.constEnd(); ++it)
            rows << QStringLiteral("%1\tn=%2\tmedian p1=%3\tmedian p99=%4")
                        .arg(it.key()).arg(it.value().size())
                        .arg(median(it.value())).arg(median(p99ByFormat.value(it.key())));

        QFile glossReport(reportPath);
        if (glossReport.open(QIODevice::WriteOnly | QIODevice::Text))
            glossReport.write(rows.join(QChar('\n')).toUtf8() + '\n');
        return 0;
    }

    const int surveyIndex = application.arguments().indexOf(QStringLiteral("--survey-r"));
    if (surveyIndex >= 0 && surveyIndex + 2 < application.arguments().size()) {
        const QStringList arguments = application.arguments();
        const QString root = arguments.at(surveyIndex + 1);
        const QString reportPath = arguments.at(surveyIndex + 2);
        const int rootIndex = arguments.indexOf(QStringLiteral("--texture-root"));
        const QString textureRoot = rootIndex >= 0 && rootIndex + 1 < arguments.size()
            ? arguments.at(rootIndex + 1) : QString();
        const int limitIndex = arguments.indexOf(QStringLiteral("--limit"));
        const int limit = limitIndex >= 0 && limitIndex + 1 < arguments.size()
            ? arguments.at(limitIndex + 1).toInt() : 400;

        QStringList rows;
        rows << QStringLiteral("material\tspecMult\tspecColorLuma\tsmoothness\tpbr\trPath\tformat\tspace"
                               "\twidth\theight\tmips\tbytes\tp1\tp50\tp99\tspread\tatFloorPct\tjustAbovePct");
        QDirIterator walker(root, {QStringLiteral("*.bgsm"), QStringLiteral("*.bgem")},
            QDir::Files, QDirIterator::Subdirectories);
        int seen = 0;
        while (walker.hasNext() && seen < limit) {
            const QString path = walker.next();
            Bgsm::Material material;
            if (!Bgsm::readFile(path, &material, nullptr)) continue;
            if (!material.isFallout76()) continue;
            const QString reflectance = !material.specularTexture.isEmpty()
                ? material.specularTexture : material.smoothSpecTexture;
            if (reflectance.isEmpty()) continue;
            const QImage map = TextureImage::load(reflectance, textureRoot, nullptr);
            if (map.isNull()) continue;
            const TextureImage::Diagnostics info = TextureImage::inspect(reflectance, textureRoot);
            ++seen;

            // Percentiles of the STORED luma, before any linearisation: this is
            // what the file holds and what a histogram shows untouched.
            const QImage rgba = map.convertToFormat(QImage::Format_RGBA8888);
            std::array<int, 256> buckets{};
            for (int y = 0; y < rgba.height(); ++y) {
                const uchar* line = rgba.constScanLine(y);
                for (int x = 0; x < rgba.width(); ++x) {
                    const uchar* texel = line + x * 4;
                    const int luma = int(0.2126 * texel[0] + 0.7152 * texel[1] + 0.0722 * texel[2]);
                    ++buckets[std::clamp(luma, 0, 255)];
                }
            }
            const double texels = double(rgba.width()) * rgba.height();
            const auto percentile = [&buckets, texels](double fraction) {
                double seenTexels = 0;
                for (int i = 0; i < 256; ++i) {
                    seenTexels += buckets[i];
                    if (seenTexels >= fraction * texels) return i;
                }
                return 255;
            };
            const int p1 = percentile(0.01), p50 = percentile(0.50), p99 = percentile(0.99);
            // How much of the map sits ON the 56/255 dielectric floor versus
            // just above it. BC1 is 4bpp and quantises hard, so a "flat" floor
            // decodes with a spread — and that spread decides whether a black
            // point AT the floor leaks low-level metal or not.
            double atFloor = 0, justAbove = 0;
            for (int i = 0; i <= 56; ++i) atFloor += buckets[i];
            for (int i = 57; i <= 64; ++i) justAbove += buckets[i];
            const QVector3D specColor = material.specularColor;
            const double specLuma = 0.2126 * specColor.x() + 0.7152 * specColor.y() + 0.0722 * specColor.z();
            rows << QStringLiteral("%1\t%2\t%3\t%4\t%5\t%6\t%7\t%8\t%9\t%10\t%11\t%12\t%13\t%14\t%15\t%16\t%17\t%18")
                        .arg(QDir(root).relativeFilePath(path))
                        .arg(material.specularMult, 0, 'f', 3).arg(specLuma, 0, 'f', 3)
                        .arg(material.smoothness, 0, 'f', 3).arg(material.pbr ? 1 : 0)
                        .arg(reflectance, info.format,
                            info.colorSpace.isEmpty() ? QStringLiteral("?") : info.colorSpace)
                        .arg(info.width).arg(info.height)
                        .arg(info.mipLevels).arg(info.fileSize)
                        .arg(p1).arg(p50).arg(p99).arg(p99 - p1)
                        .arg(100.0 * atFloor / texels, 0, 'f', 1)
                        .arg(100.0 * justAbove / texels, 0, 'f', 1);
        }
        QFile reportFile(reportPath);
        if (reportFile.open(QIODevice::WriteOnly | QIODevice::Text))
            reportFile.write(rows.join(QChar('\n')).toUtf8() + '\n');
        return 0;
    }

    // Drives the knife with synthetic mouse events over a real graph: a stroke
    // that crosses a wire must cut it, one that misses must not, and no other
    // wire may be disturbed. Cutting is destructive, so "roughly works" is not
    // good enough to ship on a visual check.
    const int knifeIndex = application.arguments().indexOf(QStringLiteral("--knife-check"));
    if (knifeIndex >= 0 && knifeIndex + 1 < application.arguments().size()) {
        const QString reportPath = application.arguments().at(knifeIndex + 1);
        QStringList report;
        int failures = 0;
        const auto require = [&failures, &report](bool condition, const QString& what) {
            report << QStringLiteral("%1: %2").arg(condition ? QStringLiteral("PASS")
                                                             : QStringLiteral("FAIL"), what);
            if (!condition) ++failures;
        };

        NodeCanvas canvas(ConverterNodes::sharedRegistry());
        NodeGraph::Graph graph = ConverterNodes::defaultFo76Graph();
        const int before = graph.connections.size();
        canvas.setGraph(graph);
        canvas.resize(1400, 900);
        canvas.show();
        canvas.frameAll();

        // K arms it. Without this the same drag would pan or move a node.
        QKeyEvent arm(QEvent::KeyPress, Qt::Key_K, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &arm);

        const auto stroke = [&canvas](const QPointF& from, const QPointF& to) {
            QMouseEvent press(QEvent::MouseButtonPress, from, canvas.mapToGlobal(from),
                Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(&canvas, &press);
            for (int step = 1; step <= 24; ++step) {
                const QPointF at = from + (to - from) * (double(step) / 24.0);
                QMouseEvent move(QEvent::MouseMove, at, canvas.mapToGlobal(at),
                    Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(&canvas, &move);
            }
            QMouseEvent release(QEvent::MouseButtonRelease, to, canvas.mapToGlobal(to),
                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(&canvas, &release);
        };

        // Well above every node, so it can only cross empty canvas.
        stroke(QPointF(20, 3), QPointF(1380, 3));
        require(canvas.graph().connections.size() == before,
            QStringLiteral("a stroke over empty canvas cuts nothing: %1 of %2 wires left")
                .arg(canvas.graph().connections.size()).arg(before));

        // Straight down the middle of the framed graph: it has to meet wires.
        stroke(QPointF(700, 20), QPointF(700, 880));
        const int after = canvas.graph().connections.size();
        require(after < before, QStringLiteral("a stroke across the graph cuts wires: %1 -> %2")
            .arg(before).arg(after));
        // Whatever survived must still be wires the graph actually had, and
        // every node must still be there — a knife may not delete nodes.
        require(canvas.graph().nodes.size() == graph.nodes.size(),
            QStringLiteral("the knife leaves every node alone: %1 of %2")
                .arg(canvas.graph().nodes.size()).arg(graph.nodes.size()));
        bool survivorsWereOriginal = true;
        for (const NodeGraph::Connection& survivor : canvas.graph().connections)
            if (!graph.connections.contains(survivor)) survivorsWereOriginal = false;
        require(survivorsWereOriginal,
            QStringLiteral("and invents no new ones"));

        // Disarmed, the same drag must leave the graph alone.
        QKeyEvent disarm(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &disarm);
        const int armedCount = canvas.graph().connections.size();
        stroke(QPointF(700, 20), QPointF(700, 880));
        require(canvas.graph().connections.size() == armedCount,
            QStringLiteral("Escape disarms it: %1 wires before, %2 after")
                .arg(armedCount).arg(canvas.graph().connections.size()));

        report << QStringLiteral("Failures: %1").arg(failures);
        QFile reportFile(reportPath);
        if (reportFile.open(QIODevice::WriteOnly | QIODevice::Text))
            reportFile.write(report.join(QChar('\n')).toUtf8() + '\n');
        else ++failures;
        return failures ? 1 : 0;
    }

    // The queue's folder tree and its multi-selection. Built against a
    // throwaway material tree rather than the corpus, because what is under
    // test is the tree and the selection, not any material's contents.
    const int queueIndex = application.arguments().indexOf(QStringLiteral("--queue-check"));
    if (queueIndex >= 0 && queueIndex + 1 < application.arguments().size()) {
        const QString reportPath = application.arguments().at(queueIndex + 1);
        QStringList report;
        int failures = 0;
        const auto require = [&failures, &report](bool condition, const QString& what) {
            report << QStringLiteral("%1: %2").arg(condition ? QStringLiteral("PASS")
                                                             : QStringLiteral("FAIL"), what);
            if (!condition) ++failures;
        };

        QTemporaryDir sourceDir;
        const QStringList layout = {QStringLiteral("armor/steel/helmet.bgsm"),
            QStringLiteral("armor/steel/plate.bgsm"), QStringLiteral("armor/leather/vest.bgsm"),
            QStringLiteral("weapons/pipe.bgsm"), QStringLiteral("loose.bgsm")};
        for (const QString& relative : layout) {
            const QString path = sourceDir.filePath(relative);
            QDir().mkpath(QFileInfo(path).absolutePath());
            QFile file(path);
            if (file.open(QIODevice::WriteOnly)) file.write("BGSM", 4);
        }
        // The workspace reads and writes QSettings all over: source folders,
        // preview debug views, splitter state, upscale policy — and its
        // destructor persists whatever state the throwaway instance ended in.
        // So the WHOLE scope moves somewhere scratch for the duration rather
        // than restoring key by key. Restoring individually is how this harness
        // clobbered the real source folder once, and then the preview's split
        // render mode a second time; enumerating the keys a widget tree might
        // touch is not something a test can be trusted to get right.
        const QString realApp = QCoreApplication::applicationName();
        struct RestoreSettingsScope {
            QString name;
            ~RestoreSettingsScope()
            {
                QSettings scratch;
                scratch.clear(); // drop the scratch tree, then point back at the real one
                QCoreApplication::setApplicationName(name);
            }
        } restoreSettingsScope{realApp};
        QCoreApplication::setApplicationName(realApp + QStringLiteral("-HarnessScratch"));
        QSettings().setValue(QStringLiteral("Convert/sourceRoot"), sourceDir.path());

        ConvertWorkspace workspace;
        workspace.resize(1500, 900);
        workspace.show();
        workspace.scanNow();
        auto* queue = workspace.findChild<QTreeWidget*>(QStringLiteral("ConvertQueue"));
        if (!queue) {
            report << QStringLiteral("FAIL: the queue tree is missing");
            QFile bail(reportPath);
            if (bail.open(QIODevice::WriteOnly | QIODevice::Text))
                bail.write(report.join(QChar('\n')).toUtf8() + '\n');
            return 1;
        }

        require(queue->columnCount() == 2
                && queue->headerItem()->text(0) == QStringLiteral("Material")
                && queue->headerItem()->text(1) == QStringLiteral("Status"),
            QStringLiteral("the queue is Material | Status, with the Metal column gone"));
        require(workspace.entryCountForTest() == layout.size(),
            QStringLiteral("every material is queued (%1 of %2)")
                .arg(workspace.entryCountForTest()).arg(layout.size()));
        // armor, weapons, loose.bgsm — the folders the source tree actually has.
        require(queue->topLevelItemCount() == 3,
            QStringLiteral("the source folders become tree rows (%1 top level)")
                .arg(queue->topLevelItemCount()));
        QTreeWidgetItem* armor = nullptr;
        for (int i = 0; i < queue->topLevelItemCount(); ++i)
            if (queue->topLevelItem(i)->text(0) == QStringLiteral("armor"))
                armor = queue->topLevelItem(i);
        require(armor && armor->childCount() == 2,
            QStringLiteral("nested folders nest (armor holds %1)")
                .arg(armor ? armor->childCount() : -1));
        require(armor && workspace.entryIndexOfForTest(armor) < 0,
            QStringLiteral("a folder row is not itself a material"));

        // Ticking a folder has to carry its whole subtree, or a folder handle
        // is decoration.
        if (armor) armor->setCheckState(0, Qt::Checked);
        require(workspace.checkedCountForTest() == 3,
            QStringLiteral("ticking a folder ticks the materials under it (%1 of 3)")
                .arg(workspace.checkedCountForTest()));
        if (armor) armor->setCheckState(0, Qt::Unchecked);
        require(workspace.checkedCountForTest() == 0,
            QStringLiteral("and unticking it clears them"));

        // Selecting the folder stands for its materials, so right-click
        // Convert on a folder converts what is in it.
        require(queue->selectionMode() == QAbstractItemView::ExtendedSelection,
            QStringLiteral("shift-range and ctrl-toggle are available"));
        queue->clearSelection();
        if (armor) armor->setSelected(true);
        require(workspace.selectedEntriesForTest().size() == 3,
            QStringLiteral("selecting a folder selects the materials beneath it (%1)")
                .arg(workspace.selectedEntriesForTest().size()));

        // Double-click toggles the tick, and must not also expand the folder
        // it landed on — one gesture, one meaning.
        require(!queue->expandsOnDoubleClick(),
            QStringLiteral("double-click is the tick, not the expander"));
        QTreeWidgetItem* nested = armor && armor->childCount() && armor->child(0)->childCount()
            ? armor->child(0)->child(0) : nullptr;
        if (nested) {
            const QRect row = queue->visualItemRect(nested);
            // Right of the checkbox, or the press toggles the tick on its own
            // and the double-click reads as a second toggle.
            const QPointF at = QPointF(row.center().x() + 30, row.center().y());
            for (int click = 0; click < 2; ++click) {
                // A press first: QAbstractItemView drops a double-click whose
                // index does not match the one the press recorded.
                QMouseEvent press(QEvent::MouseButtonPress, at, queue->viewport()->mapToGlobal(at),
                    Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(queue->viewport(), &press);
                QMouseEvent release(QEvent::MouseButtonRelease, at, queue->viewport()->mapToGlobal(at),
                    Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(queue->viewport(), &release);
                QMouseEvent doubled(QEvent::MouseButtonDblClick, at, queue->viewport()->mapToGlobal(at),
                    Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(queue->viewport(), &doubled);
                require(nested->checkState(0) == (click == 0 ? Qt::Checked : Qt::Unchecked),
                    click == 0 ? QStringLiteral("double-clicking a row ticks it")
                               : QStringLiteral("and double-clicking again unticks it"));
            }
        }

        // Row colouring, sampled off a real render. Three separate mechanisms
        // have painted over this kind of background before, and none of them
        // are visible from the code that sets the colour.
        queue->clearSelection();
        QTreeWidgetItem* weapons = nullptr;
        for (int i = 0; i < queue->topLevelItemCount(); ++i)
            if (queue->topLevelItem(i)->text(0) == QStringLiteral("weapons"))
                weapons = queue->topLevelItem(i);
        QTreeWidgetItem* pipe = weapons && weapons->childCount() ? weapons->child(0) : nullptr;
        QTreeWidgetItem* loose = nullptr;
        for (int i = 0; i < queue->topLevelItemCount(); ++i)
            if (queue->topLevelItem(i)->text(0) == QStringLiteral("loose.bgsm"))
                loose = queue->topLevelItem(i);
        if (pipe && loose) {
            // Current first, then extend: the one-argument setCurrentItem is a
            // ClearAndSelect and would drop everything selected before it.
            queue->setCurrentItem(pipe);
            loose->setSelected(true);
            QApplication::processEvents();
            const QPixmap shot = queue->viewport()->grab();
            const QImage rendered = shot.toImage();
            // Per row: the background colour at the far right (past the text)
            // and the most saturated warm pixel, which is the text ink.
            const auto sample = [&rendered, queue](QTreeWidgetItem* item) {
                const QRect row = queue->visualItemRect(item);
                QColor background = rendered.pixelColor(
                    std::min(row.right() - 2, rendered.width() - 1), row.center().y());
                QColor ink;
                int bestWarmth = -1000;
                for (int y = row.top() + 1; y < row.bottom() && y < rendered.height(); ++y)
                    for (int x = row.left(); x < row.right() && x < rendered.width(); ++x) {
                        const QColor pixel = rendered.pixelColor(x, y);
                        const int warmth = pixel.red() - pixel.blue();
                        if (warmth > bestWarmth) { bestWarmth = warmth; ink = pixel; }
                    }
                return std::make_pair(background, ink);
            };
            const auto primary = sample(pipe);
            const auto secondary = sample(loose);
            require(primary.first == QColor(74, 122, 176) && secondary.first == QColor(74, 122, 176),
                QStringLiteral("both selected rows carry the same blue (%1 / %2)")
                    .arg(primary.first.name(), secondary.first.name()));
            // Orange for the primary, redder orange for the rest: same hue
            // family, so green is what separates them.
            require(primary.second.red() > 200 && secondary.second.red() > 200,
                QStringLiteral("both rows ink in the orange family (%1 / %2)")
                    .arg(primary.second.name(), secondary.second.name()));
            require(primary.second.green() > secondary.second.green() + 20,
                QStringLiteral("the primary row is the lighter orange (G %1 vs %2)")
                    .arg(primary.second.green()).arg(secondary.second.green()));

            // Moving the current row has to move the primary ink with it, or
            // the two colours are decoration rather than a signal. NoUpdate so
            // the selection survives the move.
            queue->setCurrentItem(loose, 0, QItemSelectionModel::NoUpdate);
            QApplication::processEvents();
            const QImage moved = queue->viewport()->grab().toImage();
            const auto sampleFrom = [&moved, queue](QTreeWidgetItem* item) {
                const QRect row = queue->visualItemRect(item);
                QColor ink;
                int bestWarmth = -1000;
                for (int y = row.top() + 1; y < row.bottom() && y < moved.height(); ++y)
                    for (int x = row.left(); x < row.right() && x < moved.width(); ++x) {
                        const QColor pixel = moved.pixelColor(x, y);
                        const int warmth = pixel.red() - pixel.blue();
                        if (warmth > bestWarmth) { bestWarmth = warmth; ink = pixel; }
                    }
                return ink;
            };
            require(sampleFrom(loose).green() > sampleFrom(pipe).green() + 20,
                QStringLiteral("and the colours follow the current row when it moves"));
        } else {
            require(false, QStringLiteral("the rows needed for the colour check were not built"));
        }

        // One checkbox look everywhere. QTreeView::indicator and
        // QGroupBox::indicator are separate stylesheet selectors from
        // QCheckBox::indicator, so sharing the rule text proves nothing — the
        // colours have to be found in a real render of each widget.
        const auto findsColour = [](const QImage& image, const QColor& wanted) {
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    if (image.pixelColor(x, y) == wanted) return true;
            return false;
        };
        const QColor tickBlue(0x3d, 0x78, 0xae);   // ::indicator:checked fill
        const QColor emptyEdge(0x69, 0x6d, 0x73);  // ::indicator:unchecked border
        const QColor partBlue(0x2f, 0x54, 0x76);   // ::indicator:indeterminate fill
        if (pipe && weapons) {
            queue->clearSelection();
            pipe->setCheckState(0, Qt::Checked);
            QApplication::processEvents();
            const QImage ticked = queue->viewport()->grab().toImage();
            require(findsColour(ticked, tickBlue),
                QStringLiteral("a ticked material row uses the standard checkbox blue"));
            require(findsColour(ticked, emptyEdge),
                QStringLiteral("and an unticked one the standard empty box"));
            // The folder above it is now partially ticked, which needs to read
            // as neither of those.
            require(weapons->checkState(0) == Qt::PartiallyChecked
                    || weapons->childCount() == 1,
                QStringLiteral("a folder with some children ticked goes partial"));
            if (armor) {
                armor->child(0)->child(0)->setCheckState(0, Qt::Checked);
                QApplication::processEvents();
                require(armor->checkState(0) == Qt::PartiallyChecked,
                    QStringLiteral("a part-ticked folder reports partial"));
                require(findsColour(queue->viewport()->grab().toImage(), partBlue),
                    QStringLiteral("and draws the dimmed partial box"));
                armor->child(0)->child(0)->setCheckState(0, Qt::Unchecked);
            }
            pipe->setCheckState(0, Qt::Unchecked);
        }
        for (QGroupBox* box : workspace.findChildren<QGroupBox*>()) {
            if (!box->isCheckable()) continue;
            const QString what = box->title().section(QChar(0x2014), 0, 0).trimmed();
            const QImage header = box->grab(QRect(0, 0, std::min(box->width(), 80), 26)).toImage();
            require(findsColour(header, box->isChecked() ? tickBlue : emptyEdge),
                QStringLiteral("the %1 disclosure uses the same checkbox").arg(what));
        }

        // The primary button and the context menu must agree about scope, and
        // one selected row must NOT narrow it: clicking a row is how you
        // inspect a material, and silently turning "convert everything" into
        // "convert this one" because you looked at it is a trap.
        QPushButton* convert = nullptr;
        for (QPushButton* button : workspace.findChildren<QPushButton*>())
            if (button->text().startsWith(QStringLiteral("Convert"))) convert = button;
        if (convert && pipe && loose) {
            queue->clearSelection();
            QApplication::processEvents();
            require(convert->text().contains(QString::number(layout.size())),
                QStringLiteral("with nothing selected the button converts the queue (\"%1\")")
                    .arg(convert->text()));
            queue->setCurrentItem(pipe);
            QApplication::processEvents();
            require(convert->text().contains(QString::number(layout.size())),
                QStringLiteral("inspecting one row does not narrow it (\"%1\")").arg(convert->text()));
            loose->setSelected(true);
            QApplication::processEvents();
            require(convert->text().contains(QStringLiteral("2 selected")),
                QStringLiteral("selecting two does (\"%1\")").arg(convert->text()));
            pipe->setCheckState(0, Qt::Checked);
            QApplication::processEvents();
            require(convert->text().contains(QStringLiteral("checked")),
                QStringLiteral("and a tick outranks a selection (\"%1\")").arg(convert->text()));
            pipe->setCheckState(0, Qt::Unchecked);
            queue->clearSelection();
        }

        // Every navigation key has to be written down somewhere.
        bool hintCoversNavigation = false;
        for (QLabel* label : workspace.findChildren<QLabel*>())
            if (label->text().contains(QStringLiteral("Ctrl+F"))
                && label->text().contains(QStringLiteral("Shift+F"))
                && label->text().contains(QStringLiteral("minimap")))
                hintCoversNavigation = true;
        require(hintCoversNavigation,
            QStringLiteral("the canvas hint names find, frame and the minimap"));

        // The Corrections heading carries a material name longer than the
        // panel; a QGroupBox title does not elide itself, it just runs off and
        // gets cut mid-word.
        workspace.selectFirstMaterial();
        QApplication::processEvents();
        for (QGroupBox* box : workspace.findChildren<QGroupBox*>()) {
            if (!box->title().startsWith(QStringLiteral("Corrections"))) continue;
            const QFontMetrics metrics(box->font());
            // Narrowed hard, because the synthetic tree's names are short and
            // the real ones that overflow run to fifty characters.
            box->setFixedWidth(150);
            QApplication::processEvents();
            require(box->title() == QStringLiteral("Corrections"),
                QStringLiteral("too narrow to name the material, the heading drops it (\"%1\")")
                    .arg(box->title()));
            box->setFixedWidth(340);
            QApplication::processEvents();
            require(box->title().startsWith(QStringLiteral("Corrections —"))
                    && metrics.horizontalAdvance(box->title()) <= 340,
                QStringLiteral("and at panel width it names it and still fits (\"%1\", %2px)")
                    .arg(box->title()).arg(metrics.horizontalAdvance(box->title())));
            box->setMinimumWidth(0);
            box->setMaximumWidth(QWIDGETSIZE_MAX);
        }

        // Folding the preview settings has to actually hand the height over.
        // The strip can collapse while the splitter keeps its restored sizes,
        // which leaves it floating at the top of a pane-sized empty gap.
        QApplication::processEvents();
        auto* previewSettings = workspace.findChild<QGroupBox*>(
            QStringLiteral("ConverterPreviewSettings"));
        require(previewSettings && !previewSettings->isChecked(),
            QStringLiteral("the converter's preview settings start folded"));
        if (previewSettings && previewSettings->parentWidget()) {
            const int stripHeight = previewSettings->parentWidget()->height();
            require(stripHeight <= 40,
                QStringLiteral("and the folded strip takes no pane height (%1px)").arg(stripHeight));
            previewSettings->setChecked(true);
            QApplication::processEvents();
            require(previewSettings->parentWidget()->height() > 100,
                QStringLiteral("unfolding gives it back (%1px)")
                    .arg(previewSettings->parentWidget()->height()));
            previewSettings->setChecked(false);
        }

        report << QStringLiteral("Failures: %1").arg(failures);
        QFile reportFile(reportPath);
        if (reportFile.open(QIODevice::WriteOnly | QIODevice::Text))
            reportFile.write(report.join(QChar('\n')).toUtf8() + '\n');
        else ++failures;
        return failures ? 1 : 0;
    }

    // Canvas repaint cost, on the real FO76 graph with thumbnails attached.
    // Reported rather than asserted: a threshold would be a guess about this
    // machine, and the number that matters is the before/after of a change.
    const int perfIndex = application.arguments().indexOf(QStringLiteral("--canvas-perf"));
    if (perfIndex >= 0) {
        NodeCanvas canvas(ConverterNodes::sharedRegistry());
        const NodeGraph::Graph graph = ConverterNodes::defaultFo76Graph();
        canvas.setGraph(graph);
        canvas.resize(1400, 900);
        canvas.show();

        // Every node carries a thumbnail on every output, which is what a real
        // evaluation produces and what the masking pass chews through.
        QImage swatch(128, 128, QImage::Format_RGBA8888);
        for (int y = 0; y < swatch.height(); ++y)
            for (int x = 0; x < swatch.width(); ++x)
                swatch.setPixel(x, y, qRgba(x * 2 % 256, y * 2 % 256, (x + y) % 256, 255));
        QHash<QString, NodeGraph::NodeResult> results;
        for (const NodeGraph::Node& node : graph.nodes) {
            NodeGraph::NodeResult result;
            if (const NodeGraph::NodeTypeSpec* spec = ConverterNodes::sharedRegistry().find(node.typeId))
                for (const NodeGraph::PortSpec& port : spec->outputs)
                    result.thumbnails.insert(port.id, swatch);
            result.status = QStringLiteral("1024×1024 · BC1 sRGB");
            results.insert(node.id, result);
        }
        canvas.setResults(results);

        QStringList lines;
        const auto timeFrames = [&canvas](const QString& what, int frames) {
            canvas.grab(); // warm: first paint frames the graph and builds caches
            QElapsedTimer timer;
            timer.start();
            for (int i = 0; i < frames; ++i) canvas.grab();
            const double elapsed = double(timer.nsecsElapsed()) / 1e6;
            return QStringLiteral("%1\t%2 frames\t%3 ms total\t%4 ms/frame")
                .arg(what).arg(frames).arg(elapsed, 0, 'f', 1).arg(elapsed / frames, 0, 'f', 2);
        };
        canvas.frameAll();
        lines << timeFrames(QStringLiteral("framed (overview zoom)"), 60);
        canvas.selectNodeForTest(graph.nodes.isEmpty() ? QString() : graph.nodes.first().id);
        if (!graph.nodes.isEmpty()) canvas.frameNode(graph.nodes.first().id);
        lines << timeFrames(QStringLiteral("zoomed to one node"), 60);

        const QString text = lines.join(QChar('\n')) + QChar('\n');
        if (perfIndex + 1 < application.arguments().size()
            && !application.arguments().at(perfIndex + 1).startsWith(QLatin1Char('-'))) {
            QFile out(application.arguments().at(perfIndex + 1));
            if (out.open(QIODevice::WriteOnly | QIODevice::Text)) out.write(text.toUtf8());
        }
        return 0;
    }

    // Canvas navigation aids: the overview label pass, the minimap and the
    // node search. All three are painter-space or focus behaviour that a
    // screenshot at one zoom cannot confirm, so they get driven directly.
    const int navIndex = application.arguments().indexOf(QStringLiteral("--canvas-nav-check"));
    if (navIndex >= 0 && navIndex + 1 < application.arguments().size()) {
        const QString reportPath = application.arguments().at(navIndex + 1);
        QStringList report;
        int failures = 0;
        const auto require = [&failures, &report](bool condition, const QString& what) {
            report << QStringLiteral("%1: %2").arg(condition ? QStringLiteral("PASS")
                                                             : QStringLiteral("FAIL"), what);
            if (!condition) ++failures;
        };

        NodeCanvas canvas(ConverterNodes::sharedRegistry());
        const NodeGraph::Graph graph = ConverterNodes::defaultFo76Graph();
        canvas.setGraph(graph);
        canvas.resize(1400, 900);
        canvas.show();
        canvas.frameAll();

        // Frame All on a graph this size lands under the detail threshold,
        // which is exactly the zoom the labels exist for.
        const double framedZoom = canvas.zoomForTest();
        require(framedZoom < 0.55,
            QStringLiteral("framing the whole FO76 graph zooms past the detail threshold (%1)")
                .arg(framedZoom, 0, 'f', 3));

        // Shift+F on a selection has to end up somewhere readable, or framing
        // a node you went looking for was pointless.
        const QString target = graph.nodes.isEmpty() ? QString() : graph.nodes.last().id;
        canvas.selectNodeForTest(target);
        QKeyEvent frameSelected(QEvent::KeyPress, Qt::Key_F, Qt::ShiftModifier);
        QApplication::sendEvent(&canvas, &frameSelected);
        require(canvas.zoomForTest() >= 0.55,
            QStringLiteral("Shift+F frames the selection at a readable zoom (%1)")
                .arg(canvas.zoomForTest(), 0, 'f', 3));
        const NodeGraph::Node* framed = canvas.graph().node(target);
        require(framed && QRectF(0, 0, 1400, 900).contains(canvas.widgetPointForTest(framed->position)),
            QStringLiteral("and puts it inside the viewport"));

        // The map only earns its corner once there is something to map.
        require(!canvas.minimapRectForTest().isNull(),
            QStringLiteral("the minimap claims a corner on a real graph"));
        require(QRectF(0, 0, 1400, 900).contains(canvas.minimapRectForTest()),
            QStringLiteral("and stays inside the canvas"));
        NodeCanvas empty(ConverterNodes::sharedRegistry());
        empty.resize(1400, 900);
        require(empty.minimapRectForTest().isNull(),
            QStringLiteral("an empty graph draws no map"));
        // Parented, or a top-level window is clamped to a platform minimum. The
        // size asked for is above NodeCanvas's own 320x240 floor and below the
        // map's threshold, which is the only window where this guard can fire.
        QWidget crampedHost;
        crampedHost.resize(900, 600);
        auto* cramped = new NodeCanvas(ConverterNodes::sharedRegistry(), &crampedHost);
        cramped->setGraph(graph);
        cramped->resize(380, 280);
        require(cramped->minimapRectForTest().isNull(),
            QStringLiteral("nor does a canvas too small to spare the corner (%1x%2)")
                .arg(cramped->width()).arg(cramped->height()));
        // Between the two: the map appears but scales down with the pane
        // rather than taking the same 190px it would on a wide canvas.
        auto* narrow = new NodeCanvas(ConverterNodes::sharedRegistry(), &crampedHost);
        narrow->setGraph(graph);
        narrow->resize(500, 340);
        const QRectF narrowMap = narrow->minimapRectForTest();
        require(!narrowMap.isNull() && narrowMap.width() <= 500 * 0.22 + 1.0,
            QStringLiteral("and a narrow one gets a proportionally smaller map (%1px wide)")
                .arg(narrowMap.width(), 0, 'f', 1));

        // A click on the map has to move the view, and land near what was
        // clicked — the failure mode here is a map that looks right and pans
        // somewhere else because the two use different bounds.
        canvas.frameAll();
        const QRectF map = canvas.minimapRectForTest();
        const QPointF mapPoint = map.topLeft() + QPointF(map.width() * 0.8, map.height() * 0.5);
        QMouseEvent mapPress(QEvent::MouseButtonPress, mapPoint, canvas.mapToGlobal(mapPoint),
            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &mapPress);
        QMouseEvent mapRelease(QEvent::MouseButtonRelease, mapPoint, canvas.mapToGlobal(mapPoint),
            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &mapRelease);
        const QPointF centred = canvas.graphPointForTest(QPointF(700, 450));
        const QPointF asked = canvas.minimapGraphPointForTest(mapPoint);
        require(QLineF(centred, asked).length() < 4.0,
            QStringLiteral("clicking the map centres the point it was pointing at (off by %1)")
                .arg(QLineF(centred, asked).length(), 0, 'f', 2));

        // Labels must not leave the canvas. Framed, the input and terminal
        // nodes sit hard against the edges, and a label centred on them
        // overhangs and gets sliced by the widget border.
        canvas.frameAll();
        canvas.grab(); // force a paint so the overlay records its placements
        const QVector<QRectF> labels = canvas.overlayLabelsForTest();
        require(labels.size() >= 8,
            QStringLiteral("the overview labels every node it can (%1)").arg(labels.size()));
        bool allInside = true;
        for (const QRectF& label : labels)
            if (!QRectF(0, 0, 1400, 900).contains(label)) allInside = false;
        require(allInside, QStringLiteral("and every label sits inside the canvas"));
        bool noOverlap = true;
        for (int i = 0; i < labels.size(); ++i)
            for (int j = i + 1; j < labels.size(); ++j)
                if (labels.at(i).intersects(labels.at(j))) noOverlap = false;
        require(noOverlap, QStringLiteral("and none of them overlap"));

        // Search: Ctrl+F opens it, typing selects, Enter walks the matches.
        QKeyEvent openSearch(QEvent::KeyPress, Qt::Key_F, Qt::ControlModifier);
        QApplication::sendEvent(&canvas, &openSearch);
        require(canvas.searchVisibleForTest(), QStringLiteral("Ctrl+F opens the search field"));
        canvas.setSearchTextForTest(QStringLiteral("Levels"));
        const QStringList matches = canvas.searchMatchesForTest();
        require(matches.size() >= 2,
            QStringLiteral("searching finds every Levels node (%1)").arg(matches.size()));
        require(matches.contains(canvas.selectedNode()),
            QStringLiteral("and selects one of them without a second keypress"));
        const QString first = canvas.selectedNode();
        QKeyEvent nextMatch(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(canvas.searchFieldForTest(), &nextMatch);
        require(canvas.selectedNode() != first && matches.contains(canvas.selectedNode()),
            QStringLiteral("Enter walks to the next match"));
        QKeyEvent closeSearch(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &closeSearch);
        require(!canvas.searchVisibleForTest() && canvas.searchMatchesForTest().isEmpty(),
            QStringLiteral("Escape closes it and drops the rings"));
        // Escape must not also have disarmed something else: the knife shares
        // the key, and the search has to win only while it is open.
        QKeyEvent armKnife(QEvent::KeyPress, Qt::Key_K, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &armKnife);
        QKeyEvent escapeAgain(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &escapeAgain);
        require(!canvas.knifeModeForTest(),
            QStringLiteral("with the search closed, Escape still disarms the knife"));

        // Arrange must actually reduce crossings. Measured GEOMETRICALLY here —
        // segment intersections between wires — rather than by reusing the
        // rank proxy the layout optimises, so the test cannot agree with the
        // implementation by construction.
        //
        // Routed wires are measured along their ACTUAL path, lane by lane. The
        // point of routing is that a wire no longer runs corner-to-corner, so
        // measuring it as if it did would report the routing as worthless — the
        // straight-line count is what the previous version of this check
        // reported, and it was right to say Arrange changed nothing then.
        const auto pathOf = [](const NodeGraph::Graph& subject,
                                const NodeGraph::Connection& wire) {
            QVector<QPointF> path;
            const NodeGraph::Node* from = subject.node(wire.fromNode);
            const NodeGraph::Node* to = subject.node(wire.toNode);
            if (from) path.append(from->position);
            path.append(wire.waypoints);
            if (to) path.append(to->position);
            return path;
        };
        const auto pathsCross = [](const QVector<QPointF>& a, const QVector<QPointF>& b) {
            for (int i = 1; i < a.size(); ++i)
                for (int j = 1; j < b.size(); ++j) {
                    QPointF hit;
                    if (QLineF(a.at(i - 1), a.at(i)).intersects(QLineF(b.at(j - 1), b.at(j)), &hit)
                        == QLineF::BoundedIntersection) return true;
                }
            return false;
        };
        const auto routedCrossings = [&](const NodeGraph::Graph& subject) {
            int total = 0;
            for (int i = 0; i < subject.connections.size(); ++i)
                for (int j = i + 1; j < subject.connections.size(); ++j) {
                    const NodeGraph::Connection& a = subject.connections.at(i);
                    const NodeGraph::Connection& b = subject.connections.at(j);
                    if (a.fromNode == b.fromNode || a.toNode == b.toNode) continue;
                    if (pathsCross(pathOf(subject, a), pathOf(subject, b))) ++total;
                }
            return total;
        };
        const auto wireCrossings = [](const NodeGraph::Graph& subject) {
            const auto centre = [&subject](const QString& id) {
                const NodeGraph::Node* node = subject.node(id);
                return node ? node->position : QPointF();
            };
            int total = 0;
            for (int i = 0; i < subject.connections.size(); ++i)
                for (int j = i + 1; j < subject.connections.size(); ++j) {
                    const NodeGraph::Connection& a = subject.connections.at(i);
                    const NodeGraph::Connection& b = subject.connections.at(j);
                    // Wires out of the same port fan from one point; they meet
                    // at the origin, not at a crossing.
                    if (a.fromNode == b.fromNode || a.toNode == b.toNode) continue;
                    QPointF hit;
                    if (QLineF(centre(a.fromNode), centre(a.toNode))
                            .intersects(QLineF(centre(b.fromNode), centre(b.toNode)), &hit)
                        == QLineF::BoundedIntersection) ++total;
                }
            return total;
        };

        NodeGraph::Graph tidy = ConverterNodes::defaultFo76Graph();
        const int asAuthored = wireCrossings(tidy);
        NodeGraph::layoutGraph(tidy, ConverterNodes::sharedRegistry());
        const int arranged = wireCrossings(tidy);
        require(arranged <= asAuthored,
            QStringLiteral("Arrange is not worse than the stock layout (%1 -> %2 crossings)")
                .arg(asAuthored).arg(arranged));

        // Same graph, seeded from scrambled positions. The sweeps are supposed
        // to find the ordering, not inherit it — if they only preserved a good
        // seed this would blow up.
        NodeGraph::Graph scrambled = ConverterNodes::defaultFo76Graph();
        for (int index = 0; index < scrambled.nodes.size(); ++index) {
            NodeGraph::Node& node = scrambled.nodes[index];
            node.position.setY(double(((index * 7919) % 1000) - 500));
        }
        const int beforeLayout = wireCrossings(scrambled);
        NodeGraph::layoutGraph(scrambled, ConverterNodes::sharedRegistry());
        const int recovered = wireCrossings(scrambled);
        require(recovered < beforeLayout,
            QStringLiteral("Arrange untangles a scrambled graph (%1 -> %2 crossings)")
                .arg(beforeLayout).arg(recovered));
        require(recovered <= arranged + arranged / 2 + 2,
            QStringLiteral("and lands near the same answer whatever the seed (%1 vs %2)")
                .arg(recovered).arg(arranged));

        // Deterministic: a second Arrange on an already-arranged graph must be
        // a no-op, or repeated presses would shuffle the layout under you.
        NodeGraph::Graph again = tidy;
        NodeGraph::layoutGraph(again, ConverterNodes::sharedRegistry());
        bool stable = again.nodes.size() == tidy.nodes.size();
        for (int index = 0; stable && index < again.nodes.size(); ++index)
            if (again.nodes.at(index).position != tidy.nodes.at(index).position) stable = false;
        require(stable, QStringLiteral("and arranging twice changes nothing"));

        // Routing is the half that ordering could not do. Straight-line, the
        // arranged graph crosses itself ~117 times and no amount of reordering
        // moves that, because the crossings come from wires spanning five
        // columns. Following the lanes is what removes them.
        const int straight = wireCrossings(tidy);
        const int routed = routedCrossings(tidy);
        int lanedWires = 0;
        for (const NodeGraph::Connection& wire : tidy.connections)
            if (!wire.waypoints.isEmpty()) ++lanedWires;
        require(lanedWires > 0,
            QStringLiteral("column-spanning wires get routing lanes (%1 of %2)")
                .arg(lanedWires).arg(tidy.connections.size()));
        require(routed < straight,
            QStringLiteral("and following them crosses less than running straight (%1 -> %2)")
                .arg(straight).arg(routed));
        // Lanes must land BETWEEN the columns they belong to, or the wire would
        // double back to reach them.
        bool lanesAdvance = true;
        for (const NodeGraph::Connection& wire : tidy.connections) {
            for (int lane = 1; lane < wire.waypoints.size(); ++lane)
                if (wire.waypoints.at(lane).x() <= wire.waypoints.at(lane - 1).x()) lanesAdvance = false;
        }
        require(lanesAdvance, QStringLiteral("lanes advance left to right, so no wire doubles back"));

        report << QStringLiteral("Failures: %1").arg(failures);
        QFile reportFile(reportPath);
        if (reportFile.open(QIODevice::WriteOnly | QIODevice::Text))
            reportFile.write(report.join(QChar('\n')).toUtf8() + '\n');
        else ++failures;
        return failures ? 1 : 0;
    }

    // Exercises the left-half view picker without driving the converter by
    // hand: fills it for a material node, reads the entries back, and reports
    // what selecting one emits. The overlay's connects have failed silently
    // before, and nothing about that is visible in a screenshot.
    const int pickerIndex = application.arguments().indexOf(QStringLiteral("--split-picker-check"));
    if (pickerIndex >= 0 && pickerIndex + 1 < application.arguments().size()) {
        // The report goes to a file: this is a windows-subsystem binary, so
        // anything written to stdout is discarded.
        const QString reportPath = application.arguments().at(pickerIndex + 1);
        auto* preview = new MaterialPreviewWidget(nullptr, true);
        preview->resize(900, 700);
        preview->show();
        preview->setSplitPickerVisible(true);
        int failures = 0;
        QStringList report;
        const auto require = [&failures, &report](bool condition, const QString& what) {
            report << QStringLiteral("%1: %2").arg(condition ? QStringLiteral("PASS")
                                                             : QStringLiteral("FAIL"), what);
            if (!condition) ++failures;
        };

        QString lastMap;
        int mapSignals = 0, channelSignals = 0;
        QObject::connect(preview, &MaterialPreviewWidget::splitMapChanged,
            preview, [&lastMap, &mapSignals](const QString& port) { lastMap = port; ++mapSignals; });
        QObject::connect(preview, &MaterialPreviewWidget::splitChannelsChanged,
            preview, [&channelSignals](int) { ++channelSignals; });

        // Exactly what Separate Maps (Fallout 76) offers once every map has evaluated.
        using MapEntry = MaterialPreviewWidget::MapEntry;
        const QVector<MapEntry> maps{{QStringLiteral("diffuse"), QStringLiteral("Diffuse"), false},
            {QStringLiteral("normal"), QStringLiteral("Normal"), false},
            {QStringLiteral("smoothness"), QStringLiteral("Smoothness"), false},
            {QStringLiteral("ao"), QStringLiteral("Ambient Occlusion"), false}};
        preview->setSplitPickerMode(MaterialPreviewWidget::SplitPicker::Maps, maps);
        QComboBox* picker = nullptr;
        for (QComboBox* candidate : preview->findChildren<QComboBox*>())
            if (candidate->count() && candidate->itemText(0) == QStringLiteral("Material")) picker = candidate;
        require(picker, QStringLiteral("the left picker holds the map list"));
        if (picker) {
            require(picker->count() == maps.size() + 1,
                QStringLiteral("Material plus %1 maps = %2 entries").arg(maps.size()).arg(picker->count()));
            require(picker->itemData(0).toString().isEmpty(),
                QStringLiteral("the Material entry carries no port id"));
            require(picker->itemText(3) == QStringLiteral("Smoothness")
                    && picker->itemData(3).toString() == QStringLiteral("smoothness"),
                QStringLiteral("entry 3 is Smoothness/smoothness: %1/%2")
                    .arg(picker->itemText(3), picker->itemData(3).toString()));

            picker->setCurrentIndex(3);
            require(mapSignals == 1 && lastMap == QStringLiteral("smoothness"),
                QStringLiteral("picking a map emits its port id: %1 signal(s), \"%2\"")
                    .arg(mapSignals).arg(lastMap));
            picker->setCurrentIndex(0);
            require(mapSignals == 2 && lastMap.isEmpty(),
                QStringLiteral("picking Material emits an empty id, back to the shaded material"));
            require(channelSignals == 0,
                QStringLiteral("map mode never emits a channel mask"));

            // Re-sending the same list must not disturb the choice: this runs on
            // every graph evaluation, and rebuilding would snap back to Material.
            picker->setCurrentIndex(2);
            const int before = mapSignals;
            preview->setSplitPickerMode(MaterialPreviewWidget::SplitPicker::Maps, maps);
            require(picker->currentIndex() == 2 && mapSignals == before,
                QStringLiteral("an unchanged map list leaves the choice alone: index %1")
                    .arg(picker->currentIndex()));

            // Moving to a different node that happens to carry the same maps —
            // Combine and the terminal do — must re-point the shown entry at
            // what THAT node remembers, silently.
            preview->setSplitPickerCurrentMap(QString());
            require(picker->currentIndex() == 0 && mapSignals == before,
                QStringLiteral("a same-list node re-points the entry without emitting: index %1, %2 signal(s)")
                    .arg(picker->currentIndex()).arg(mapSignals));
            preview->setSplitPickerCurrentMap(QStringLiteral("ao"));
            require(picker->currentText() == QStringLiteral("Ambient Occlusion") && mapSignals == before,
                QStringLiteral("and follows a remembered map: \"%1\"").arg(picker->currentText()));

            // Grouped list: captions go in disabled so they read as captions,
            // and the packed maps carry a "port#mask" id the workspace decodes.
            const QVector<MapEntry> grouped{{QString(), QStringLiteral("Textures"), true},
                {QStringLiteral("rmaos"), QStringLiteral("RMAOS"), false},
                {QString(), QStringLiteral("Channels"), true},
                {QStringLiteral("rmaos#1"), QStringLiteral("Roughness"), false},
                {QStringLiteral("rmaos#2"), QStringLiteral("Metallic"), false}};
            preview->setSplitPickerMode(MaterialPreviewWidget::SplitPicker::Maps, grouped);
            const auto rowEnabled = [picker](int row) {
                auto* model = qobject_cast<QStandardItemModel*>(picker->model());
                return model && model->item(row) && model->item(row)->isEnabled();
            };
            require(picker->count() == 6 && picker->itemText(1) == QStringLiteral("Textures"),
                QStringLiteral("the grouped list keeps its captions: %1 rows").arg(picker->count()));
            require(!rowEnabled(1) && !rowEnabled(3) && rowEnabled(2) && rowEnabled(4),
                QStringLiteral("captions are unselectable and maps are not"));
            picker->setCurrentIndex(4);
            require(lastMap == QStringLiteral("rmaos#1"),
                QStringLiteral("an isolated channel carries its mask: \"%1\"").arg(lastMap));

            // A single-image node switches the same control to channels.
            const int mapsSoFar = mapSignals;
            preview->setSplitPickerMode(MaterialPreviewWidget::SplitPicker::Channels);
            require(picker->count() == 5 && picker->itemText(0) == QStringLiteral("RGB"),
                QStringLiteral("a one-map node gets R/G/B/A instead: %1 entries, first \"%2\"")
                    .arg(picker->count()).arg(picker->itemText(0)));
            picker->setCurrentIndex(4);
            require(channelSignals == 1 && mapSignals == mapsSoFar,
                QStringLiteral("channel mode emits a mask and no map"));
        }
        report << QStringLiteral("Failures: %1").arg(failures);
        QFile reportFile(reportPath);
        if (reportFile.open(QIODevice::WriteOnly | QIODevice::Text))
            reportFile.write(report.join(QChar('\n')).toUtf8() + '\n');
        else ++failures;
        // Destroyed here rather than leaked: the event loop is still running, so
        // the GL objects go down with a context current instead of fail-fasting
        // at exit and burying the real result in a teardown crash.
        delete preview;
        QTimer::singleShot(0, &application, [&application, failures] { application.exit(failures ? 1 : 0); });
        return application.exec();
    }

    // Shades a real .bgsm/.bgem in the native FO4/FO76 view and saves it, so
    // the legacy shading path can be looked at without driving the whole
    // converter workspace by hand.
    const int nativeIndex = application.arguments().indexOf(QStringLiteral("--native-shot"));
    if (nativeIndex >= 0 && nativeIndex + 2 < application.arguments().size()) {
        const QStringList arguments = application.arguments();
        const QString materialPath = arguments.at(nativeIndex + 1);
        const QString output = arguments.at(nativeIndex + 2);
        const int rootIndex = arguments.indexOf(QStringLiteral("--texture-root"));
        const QString textureRoot = rootIndex >= 0 && rootIndex + 1 < arguments.size()
            ? arguments.at(rootIndex + 1) : QString();
        // Overrides the render mode so an unlit map view can be shot too:
        // if the source maps render but the shaded view does not, the fault is
        // in the lighting, not in the textures.
        const int modeIndex = arguments.indexOf(QStringLiteral("--shot-mode"));
        const int shotMode = modeIndex >= 0 && modeIndex + 1 < arguments.size()
            ? arguments.at(modeIndex + 1).toInt() : -1;
        const bool shot3d = arguments.contains(QStringLiteral("--shot-3d"));

        auto* preview = new MaterialPreviewWidget(nullptr, true);
        preview->resize(900, 800);
        preview->show();
        Bgsm::Material source;
        PbrPreviewGL::NativeMaterial native;
        if (Bgsm::readFile(materialPath, &source, nullptr)) {
            const auto loadMap = [&textureRoot](const QString& authored) {
                return authored.isEmpty() ? QImage() : TextureImage::load(authored, textureRoot, nullptr);
            };
            preview->canvas()->setSourceComparisonImages(loadMap(source.diffuseTexture),
                loadMap(!source.specularTexture.isEmpty() ? source.specularTexture : source.smoothSpecTexture),
                loadMap(source.normalTexture),
                loadMap(!source.lightingTexture.isEmpty() ? source.lightingTexture : source.greyscaleTexture),
                loadMap(source.glowTexture));
            native.valid = true;
            native.fallout76 = source.isFallout76();
            native.specularColor = source.specularColor;
            native.specularMult = source.specularMult;
            native.smoothness = source.smoothness;
            native.envMapScale = source.environmentMappingMaskScale;
            native.specularEnabled = source.specularEnabled;
            native.emitEnabled = source.emitEnabled;
            native.emittanceColor = source.emittanceColor;
            native.emittanceMult = source.emittanceMult;
        }
        preview->canvas()->setNativeMaterial(native);
        preview->setRenderMode(shotMode >= 0 ? PbrPreviewGL::DebugView(shotMode)
            : (native.fallout76 ? PbrPreviewGL::DebugView::NativeFo76
                                : PbrPreviewGL::DebugView::NativeFo4));
        if (shot3d) preview->canvas()->setFlatView(false);
        QTimer::singleShot(2500, &application, [preview, output, &application] {
            const bool saved = preview->grab().save(output);
            const bool ready = preview->canvas()->rendererReady();
            // Torn down inside the event loop, so the GL objects release with a
            // context current. Leaking it here fail-fasted at exit and buried
            // the exit code the caller was reading.
            delete preview;
            application.exit(saved && ready ? 0 : 3);
        });
        return application.exec();
    }

    MainWindow window;
    // Only a real path opens a document. Testing argv[1] against one specific
    // flag meant every other flag was handed to openFile as a filename.
    static const QStringList flagsTakingAValue{QStringLiteral("--ui-smoke")};
    QString startupFile;
    for (int i = 1; i < application.arguments().size(); ++i) {
        const QString argument = application.arguments().at(i);
        if (argument.startsWith(QStringLiteral("--"))) {
            if (flagsTakingAValue.contains(argument)) ++i; // its value is not a document
            continue;
        }
        startupFile = argument;
        break;
    }
    if (!startupFile.isEmpty()) window.openFile(startupFile);
    else window.restoreLastSession();
    window.showMaximized();
    if (application.arguments().contains(QStringLiteral("--converter"))) window.showConverter();
    if (application.arguments().contains(QStringLiteral("--converter-scan"))) {
        window.showConverter();
        window.scanConverterSources();
        // Without a selected row the canvas only shows its placeholder, so a
        // capture of the graph itself needs this too.
        if (application.arguments().contains(QStringLiteral("--converter-select")))
            window.selectFirstConvertMaterial();
    }
    // Opens the standard-models table straight away. It is modal, so a capture
    // has no other way in, and a settings dialog built without ever being
    // looked at is how a column ends up two pixels wide.
    if (application.arguments().contains(QStringLiteral("--upscale-models-dialog")))
        QTimer::singleShot(400, &window, [&window] { window.showUpscaleModelsDialog(); });
    const int smokeIndex = application.arguments().indexOf(QStringLiteral("--ui-smoke"));
    if (smokeIndex >= 0 && smokeIndex + 1 < application.arguments().size()) {
        const QString output = application.arguments().at(smokeIndex + 1);
        QTimer::singleShot(1800, &window, [&application, &window, output] {
            window.grab().save(output);
            const auto* preview = window.findChild<PbrPreviewGL*>();
            application.exit(preview && preview->rendererReady() ? 0 : 2);
        });
    }
    return application.exec();
}
