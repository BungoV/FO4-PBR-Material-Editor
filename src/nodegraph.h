#pragma once

#include <QHash>
#include <QImage>
#include <QJsonObject>
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

struct PortSpec {
    QString id;
    QString name;
    ValueType type = ValueType::Image;
    QString opaqueTag;     // required payload tag when type == Opaque
    bool optional = false; // inputs only: evaluation proceeds when unconnected
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
};

struct EvalContext {
    QString textureRoot;   // where source textures resolve from
    QString sourcePath;    // the material being converted
    QString outputRoot;    // mod folder for writes
    QString referenceMesh; // optional NIF for geometry bakes
    bool previewMode = true; // preview evaluates but never writes to disk
    int thumbnailSize = 128;
    // Preview-only source downsample so interactive edits stay responsive on
    // 2K/4K sources; 0 evaluates at full resolution (what conversion uses).
    int previewMaxSize = 0;
};

struct EvalArgs {
    QHash<QString, Value> inputs;
    QJsonObject params;
    EvalContext context;
};

struct EvalOutput {
    QHash<QString, Value> outputs;
    QStringList notes;
    QString error;
};

using EvalFn = std::function<void(const EvalArgs&, EvalOutput*)>;

struct NodeTypeSpec {
    QString id;        // "convert.fo4"
    QString name;      // "FO4 Conversion"
    QString category;  // "Inference"
    QString summary;
    QVector<PortSpec> inputs;
    QVector<PortSpec> outputs;
    QVector<ParamSpec> params;
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

private:
    QHash<QString, NodeTypeSpec> m_types;
    QStringList m_order;
};

struct Node {
    QString id;
    QString typeId;
    QString label;      // optional user rename
    QPointF position;
    QJsonObject params;
};

struct Connection {
    QString fromNode, fromPort;
    QString toNode, toPort;
    bool operator==(const Connection& other) const;
};

class Graph {
public:
    QVector<Node> nodes;
    QVector<Connection> connections;

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
    QString error;
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

} // namespace NodeGraph
