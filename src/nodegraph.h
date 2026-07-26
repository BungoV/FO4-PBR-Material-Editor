#pragma once

#include <QColor>
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QRectF>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QVector3D>

#include <functional>
#include <memory>

// Domain-agnostic node graph: typed ports, a node-type registry, JSON
// serialization, and a caching evaluator that produces a thumbnail for every
// image output (Substance Designer semantics).
//
// Nothing here knows about materials, textures, or Fallout: the converter
// registers its node library in converternodes.h, and the material editor will
// register a second library on this same core later. Keep it that way.
namespace NodeGraph {

enum class ValueType { Invalid, Image, Scalar, Color, String, Opaque };

// A port value. Opaque carries a tagged shared payload so domain libraries can
// pass their own structs (materials, documents, meshes) without the core
// knowing the types.
class Value {
public:
    Value() = default;

    static Value fromImage(const QImage& image);
    static Value fromScalar(float value);
    static Value fromColor(const QVector3D& color);
    static Value fromString(const QString& text);
    static Value fromOpaque(const QString& tag, const std::shared_ptr<void>& payload);

    ValueType type() const { return m_type; }
    bool isValid() const { return m_type != ValueType::Invalid; }

    const QImage& image() const { return m_image; }
    float scalar() const { return m_scalar; }
    QVector3D color() const { return m_color; }
    QString string() const { return m_string; }
    QString opaqueTag() const { return m_opaqueTag; }

    template <typename T>
    std::shared_ptr<const T> opaque(const QString& tag) const
    {
        if (m_type != ValueType::Opaque || m_opaqueTag != tag) return {};
        return std::static_pointer_cast<const T>(m_opaque);
    }

private:
    ValueType m_type = ValueType::Invalid;
    QImage m_image;
    float m_scalar = 0;
    QVector3D m_color;
    QString m_string;
    QString m_opaqueTag;
    std::shared_ptr<void> m_opaque;
};

// A named map living inside a packed image output. RMAOS is one texture but
// four separate maps, and there is no way to know that from the pixels — the
// node that packed them says so here, and hosts offer each as its own view.
// The mask is a ChannelBit combination, so applyChannelMask extracts it.
struct ChannelMap {
    QString label;
    int mask = 0;
    bool operator==(const ChannelMap& other) const
    {
        return label == other.label && mask == other.mask;
    }
};

struct PortSpec {
    QString id;
    QString name;
    ValueType type = ValueType::Image;
    QString opaqueTag;     // required payload tag when type == Opaque
    bool optional = false; // inputs only: evaluation proceeds when unconnected
    QVector<ChannelMap> channelMaps; // image outputs only; empty when unpacked
};

struct ParamSpec {
    enum class Kind { Float, Int, Bool, Enum, Color, Path, Text };
    QString id;
    QString name;
    Kind kind = Kind::Float;
    double minimum = 0;
    double maximum = 1;
    double step = 0.01;
    QJsonValue defaultValue;
    QStringList enumLabels; // Kind::Enum
    QString tooltip;
    // Kind::Path — what the browse dialog offers. Stays LAST: several specs
    // build a ParamSpec positionally and end at the tooltip, so a field
    // inserted before it silently steals the tooltip.
    QString fileFilter;
};

struct EvalContext {
    QString textureRoot;   // where source textures resolve from
    QString sourcePath;    // the material being converted
    QString outputRoot;    // mod folder for writes
    QString referenceMesh;       // optional NIF for geometry bakes
    QString correctionMaskPath;  // painted 0.5-neutral metallic correction for this material
    bool previewMode = true; // preview evaluates but never writes to disk
    int thumbnailSize = 128;
    // Largest authored texture dimension in this material. Nodes whose native
    // resolution falls below it are flagged, so an under-sized source is
    // visible on the canvas instead of silently shipping.
    int materialMaxResolution = 0;
    // Preview-only source downsample so interactive edits stay responsive on
    // 2K/4K sources; 0 evaluates at full resolution (what conversion uses).
    int previewMaxSize = 0;
    // Conversion-wide upscale policy, applied by any node left on "Project
    // setting": 0 off, 1 match the material's highest map, 2 twice it, 3 four
    // times. Expressed against the material rather than an absolute size so one
    // rule covers a batch of mixed resolutions.
    int upscalePolicy = 0;
};

struct EvalArgs {
    QHash<QString, Value> inputs;
    // Full-resolution size behind each input, so a node can reason about the
    // real texture even when the preview handed it a downsampled copy.
    QHash<QString, int> inputNative;
    // The upstream port's label for each input ("Normal", "Diffuse", ...), so
    // a node can act on what a map MEANS without a setting that just restates
    // the wiring. Empty for an unconnected input.
    QHash<QString, QString> inputLabels;
    QJsonObject params;
    EvalContext context;
};

struct EvalOutput {
    QHash<QString, Value> outputs;
    QStringList notes;
    QString status;  // one-line fact for the node body; falls back to W×H
    QString warning; // non-fatal problem worth showing on the node
    QString error;
    // True full-resolution size of this node's image, independent of any
    // preview downsample. 0 lets the evaluator inherit it from upstream.
    int nativeResolution = 0;
    // Optional distribution for a HistogramSpec strip: kHistogramChannels
    // blocks of kHistogramBuckets counts, in R,G,B,A,Luma order. Empty unless
    // the node fills it. Nodes publish their *input* distribution, so the
    // handles sit on the data they are about to remap.
    QVector<quint32> histogram;
    // Black/white points a node RESOLVED for itself, when it is picking them
    // rather than taking the stored params — autodetected Levels. Empty means
    // the params stand. The canvas draws these so the handles show what is
    // actually applied; handles disagreeing with the maths is a bug that has
    // already cost real debugging time.
    QVector<float> resolvedLevels; // {black, white}, or empty
};

using EvalFn = std::function<void(const EvalArgs&, EvalOutput*)>;

// A Levels-style histogram strip drawn inside the node body, with the input
// handles riding on top of the distribution and the output handles on a rail
// below it. Naming the params here rather than special-casing a node type in
// the canvas is what keeps the canvas domain-agnostic: it draws a strip for any
// node whose spec names these and whose result carries histogram buckets. An
// empty blackPoint means the node has no strip.
struct HistogramSpec {
    QString blackPoint;   // input black, top rail
    QString gamma;        // midtone, top rail, positioned by the other two
    QString whitePoint;   // input white, top rail
    QString outBlack;     // bottom rail
    QString outWhite;     // bottom rail
    QString channel;      // Kind::Enum param selecting which channel is shown
    bool valid() const { return !blackPoint.isEmpty() && !whitePoint.isEmpty(); }
};

// A curve editor drawn in the node body. Like HistogramSpec, this names the
// params rather than the node type, so the canvas draws it for anything that
// asks. The points param holds a JSON array of [x, y] pairs, both in 0..1.
struct CurveSpec {
    QString points;
    QString channel;  // optional Kind::Enum param, shares HistogramSpec's meaning
    bool valid() const { return !points.isEmpty(); }
};

// Curve maths, shared by whichever node applies a curve and by the canvas
// editor that draws it, so the drawn curve and the applied curve are the same
// function. This lives here rather than in the converter library because the
// canvas must not depend on any particular node set.
QVector<QPointF> curvePoints(const QString& json);
QString curveToString(const QVector<QPointF>& points);
double evaluateCurve(const QVector<QPointF>& points, double x);

struct NodeTypeSpec {
    QString id;        // "convert.fo4"
    QString name;      // "FO4 Conversion"
    QString category;  // "Inference"
    QString summary;
    QVector<PortSpec> inputs;
    QVector<PortSpec> outputs;
    QVector<ParamSpec> params;
    HistogramSpec histogram;
    CurveSpec curve;
    // Non-empty when the node stands for a whole material rather than one map.
    // There is then no single image to put in a body thumbnail and no single
    // image to isolate channels of, so the canvas draws neither; the host
    // instead offers the shaded material and each image output as separate
    // views. The value names WHICH material and means nothing here — only the
    // host reads it.
    QString materialPreview;
    EvalFn evaluate;

    const ParamSpec* param(const QString& id) const;
    const PortSpec* input(const QString& id) const;
    const PortSpec* output(const QString& id) const;
    QJsonObject defaultParams() const;
};

class Registry {
public:
    void add(const NodeTypeSpec& spec);
    const NodeTypeSpec* find(const QString& typeId) const;
    QVector<const NodeTypeSpec*> all() const;
    QStringList categories() const;

    // Header tint per category, Blender's node-editor convention. The palette
    // lives with whichever library defines the categories, so the canvas can
    // colour nodes without knowing a single category name. An unset category
    // returns an invalid colour and the canvas falls back to its neutral header.
    void setCategoryColor(const QString& category, const QColor& color);
    QColor categoryColor(const QString& category) const;

private:
    QHash<QString, NodeTypeSpec> m_types;
    QStringList m_order;
    QHash<QString, QColor> m_categoryColors;
};

// Which channels a node's thumbnail shows. A view setting only: it never
// reaches the node's evaluation, so it cannot change a single texel of what
// the graph produces.
enum ChannelBit { ChannelR = 1, ChannelG = 2, ChannelB = 4, ChannelA = 8 };
inline constexpr int kChannelsRgb = ChannelR | ChannelG | ChannelB;
inline constexpr int kChannelsRgba = kChannelsRgb | ChannelA;

// Filters an image down to the channels in mask, for display only. A single
// enabled channel shows as grayscale, which is what isolating a channel means;
// several show together with the disabled ones zeroed. Alpha is forced opaque
// unless itself enabled, so switching a channel off never makes the image
// transparent instead of black. Shared so the node thumbnail and the 3D
// viewport isolate identically.
QImage applyChannelMask(const QImage& source, int mask);

struct Node {
    QString id;
    QString typeId;
    QString label;      // optional user rename
    QPointF position;
    QJsonObject params;
    // Per-node folding, Blender's node header model. These change the node's
    // height, so nodeSize() takes the node and not just its type.
    bool showPreview = true;   // the thumbnail block
    bool showSettings = true;  // the settings block, including a Levels strip
    bool collapsed = false;    // header only; overrides the two above
    bool hideUnusedPorts = false; // drop unconnected ports from the body
    // Which channels the thumbnail shows, as ChannelBit flags. A view setting
    // like the flags above: it never reaches evaluate(), so it cannot change
    // what the node produces — only what you are looking at.
    int previewChannels = kChannelsRgb;
};

struct Connection {
    QString fromNode, fromPort;
    QString toNode, toPort;
    // Lane centres, in graph space, for a wire that spans more than one column.
    // layoutGraph() reserves a slot in every column such a wire crosses and
    // records it here; the canvas draws through them instead of running the
    // wire corner-to-corner over whatever is in the way. Empty means a direct
    // wire, which is every short one.
    //
    // These go stale the moment an endpoint moves, so whoever moves a node
    // clears them — a wire bending through empty space reads as a bug, and is.
    // Deliberately NOT part of operator==: two wires between the same ports are
    // the same wire whatever route was last computed for them.
    QVector<QPointF> waypoints;
    bool operator==(const Connection& other) const;
};

// A labelled container drawn behind the nodes. Membership is geometric rather
// than a stored list: whatever sits inside the frame belongs to it, so nodes
// dragged in or out never disagree with a bookkeeping list.
struct Frame {
    QString id;
    QString title;
    QRectF rect;              // graph space
    QColor color{58, 62, 70};
};

class Graph {
public:
    QVector<Node> nodes;
    QVector<Connection> connections;
    QVector<Frame> frames;

    QString addFrame(const QString& title, const QRectF& rect);
    void removeFrame(const QString& id);
    const Frame* frame(const QString& id) const;
    Frame* frame(const QString& id);
    // Nodes whose box is wholly inside the frame; they move with it.
    QStringList nodesInFrame(const QString& id, const Registry& registry) const;

    const Node* node(const QString& id) const;
    Node* node(const QString& id);
    QString addNode(const QString& typeId, const NodeTypeSpec& spec, const QPointF& position);
    void removeNode(const QString& id);
    // Rejects type mismatches, duplicate sinks (one connection per input), and
    // any edge that would close a cycle.
    bool connect(const Registry& registry, const Connection& connection, QString* error = nullptr);
    void disconnect(const Connection& connection);
    QVector<Connection> connectionsTo(const QString& nodeId) const;
    QVector<Connection> connectionsFrom(const QString& nodeId) const;

    // Dependency-ordered node ids; empty when the graph contains a cycle.
    QStringList topologicalOrder() const;
    bool validate(const Registry& registry, QStringList* errors) const;

    QJsonObject toJson() const;
    static Graph fromJson(const QJsonObject& json);
};

struct NodeResult {
    QHash<QString, Value> outputs;
    QHash<QString, QImage> thumbnails; // one per image output port
    QStringList notes;
    QString status;   // one-line fact drawn in the node (resolution, format)
    QString warning;  // drawn in the node in amber; not an error
    QString error;
    int nativeResolution = 0;
    QVector<quint32> histogram; // see EvalOutput::histogram
    QVector<float> resolvedLevels; // see EvalOutput::resolvedLevels
    bool cached = false;
    qint64 elapsedMs = 0;
};

// Evaluates in dependency order, caching per node by content key (type +
// params + upstream keys), so tweaking one parameter only re-runs what is
// downstream of it.
class Evaluator {
public:
    explicit Evaluator(const Registry& registry) : m_registry(registry) {}

    QHash<QString, NodeResult> evaluate(const Graph& graph, const EvalContext& context);
    void clearCache();
    int cacheSize() const { return m_cache.size(); }

private:
    struct CacheEntry {
        quint64 key = 0;
        NodeResult result;
    };
    const Registry& m_registry;
    QHash<QString, CacheEntry> m_cache;
};

QString valueTypeName(ValueType type);
bool portsCompatible(const PortSpec& from, const PortSpec& to);

// Node box geometry lives here, not in the canvas, so anything that positions
// nodes (the canvas, generated default graphs, future auto-layout) agrees on
// how big they are. Hard-coding a guess is how stock graphs ended up
// overlapping what the canvas actually drew.
// Blender's stacking order, top to bottom: header, preview, outputs, the
// node's own settings, a status line, then inputs.
namespace Metrics {
inline constexpr double kNodeWidth = 232;
inline constexpr double kHeaderHeight = 24;
inline constexpr double kThumbSize = 132;
inline constexpr double kPadding = 7;
inline constexpr double kPortSpacing = 17;
inline constexpr double kPortRadius = 4.5;
inline constexpr double kParamRowHeight = 20;
inline constexpr double kStatusHeight = 15;
inline constexpr double kColumnGap = 70;  // between node columns
inline constexpr double kRowGap = 26;     // between stacked nodes
// Levels strip: the distribution plot, plus a rail above for the input handles
// and one below for the output handles.
inline constexpr double kHistogramPlotHeight = 48;
inline constexpr double kHistogramRailHeight = 11;
inline constexpr double kHistogramHeight = kHistogramPlotHeight + 2 * kHistogramRailHeight;
// Curve editor: square-ish, since both axes are the same 0..1 range and a
// squashed box makes the diagonal misleading.
inline constexpr double kCurveHeight = 132;
// R/G/B/A toggles sitting under a node's thumbnail.
inline constexpr double kChannelStripHeight = 16;
} // namespace Metrics

// Histogram buffer shape shared by producers (node evaluate) and the canvas.
inline constexpr int kHistogramBuckets = 256;
inline constexpr int kHistogramChannels = 5; // R, G, B, A, Luma

bool nodeShowsThumbnail(const NodeTypeSpec& spec);
// The R/G/B/A strip belongs to the thumbnail, so it comes and goes with it —
// except on a node that stands for a whole material, where there is no single
// image whose channels the toggles could mean anything about.
bool nodeShowsChannelStrip(const NodeTypeSpec& spec, const Node& node);
// A port draws unless the node is hiding unused ones and nothing is wired to
// it. A connected port always draws, or hiding it would orphan a visible wire.
bool portVisible(const Node& node, const Graph& graph, const QString& portId, bool isOutput);
// Height depends on the node's fold state and on which of its ports survive, so
// this needs the node and the graph rather than the type alone. Anything that
// positions nodes goes through here, so it agrees with what the canvas draws.
QSizeF nodeSize(const NodeTypeSpec& spec, const Node& node, const Graph& graph);

// Reflows a graph into dependency columns, stacking each column by real node
// heights so nothing can overlap. Existing positions only seed the row order
// within a column, which keeps a hand-arranged graph recognisable.
void layoutGraph(Graph& graph, const Registry& registry);

} // namespace NodeGraph
