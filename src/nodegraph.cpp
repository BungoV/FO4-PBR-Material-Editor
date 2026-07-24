#include "nodegraph.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QUuid>

namespace NodeGraph {
namespace {

quint64 hashBytes(const QByteArray& bytes)
{
    // FNV-1a: cheap, stable across runs (qHash is salted per process, which
    // would defeat cache reuse the moment anything is persisted).
    quint64 hash = 1469598103934665603ull;
    for (char byte : bytes) {
        hash ^= quint8(byte);
        hash *= 1099511628211ull;
    }
    return hash;
}

QImage makeThumbnail(const QImage& image, int size)
{
    if (image.isNull() || size <= 0) return {};
    return image.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

} // namespace

// ---- Value ----

Value Value::fromImage(const QImage& image)
{
    Value value; value.m_type = ValueType::Image; value.m_image = image; return value;
}

Value Value::fromScalar(float scalar)
{
    Value value; value.m_type = ValueType::Scalar; value.m_scalar = scalar; return value;
}

Value Value::fromColor(const QVector3D& color)
{
    Value value; value.m_type = ValueType::Color; value.m_color = color; return value;
}

Value Value::fromString(const QString& text)
{
    Value value; value.m_type = ValueType::String; value.m_string = text; return value;
}

Value Value::fromOpaque(const QString& tag, const std::shared_ptr<void>& payload)
{
    Value value; value.m_type = ValueType::Opaque; value.m_opaqueTag = tag; value.m_opaque = payload; return value;
}

QString valueTypeName(ValueType type)
{
    switch (type) {
    case ValueType::Image: return QStringLiteral("Image");
    case ValueType::Scalar: return QStringLiteral("Scalar");
    case ValueType::Color: return QStringLiteral("Color");
    case ValueType::String: return QStringLiteral("String");
    case ValueType::Opaque: return QStringLiteral("Data");
    case ValueType::Invalid: break;
    }
    return QStringLiteral("Invalid");
}

bool portsCompatible(const PortSpec& from, const PortSpec& to)
{
    if (from.type != to.type) return false;
    if (from.type != ValueType::Opaque) return true;
    // Opaque payloads must carry the same tag; an empty sink tag accepts any.
    return to.opaqueTag.isEmpty() || from.opaqueTag == to.opaqueTag;
}

// ---- NodeTypeSpec ----

const ParamSpec* NodeTypeSpec::param(const QString& id) const
{
    for (const ParamSpec& spec : params) if (spec.id == id) return &spec;
    return nullptr;
}

const PortSpec* NodeTypeSpec::input(const QString& id) const
{
    for (const PortSpec& spec : inputs) if (spec.id == id) return &spec;
    return nullptr;
}

const PortSpec* NodeTypeSpec::output(const QString& id) const
{
    for (const PortSpec& spec : outputs) if (spec.id == id) return &spec;
    return nullptr;
}

QJsonObject NodeTypeSpec::defaultParams() const
{
    QJsonObject object;
    for (const ParamSpec& spec : params) object.insert(spec.id, spec.defaultValue);
    return object;
}

// ---- Registry ----

void Registry::add(const NodeTypeSpec& spec)
{
    if (!m_types.contains(spec.id)) m_order.append(spec.id);
    m_types.insert(spec.id, spec);
}

const NodeTypeSpec* Registry::find(const QString& typeId) const
{
    auto it = m_types.constFind(typeId);
    return it == m_types.constEnd() ? nullptr : &it.value();
}

QVector<const NodeTypeSpec*> Registry::all() const
{
    QVector<const NodeTypeSpec*> result;
    result.reserve(m_order.size());
    // const operator[] on QHash yields a temporary; take the address of the
    // stored value through an iterator instead.
    for (const QString& id : m_order) {
        auto it = m_types.constFind(id);
        if (it != m_types.constEnd()) result.append(&it.value());
    }
    return result;
}

QStringList Registry::categories() const
{
    QStringList result;
    for (const QString& id : m_order) {
        auto it = m_types.constFind(id);
        if (it == m_types.constEnd()) continue;
        const QString category = it.value().category;
        if (!result.contains(category)) result.append(category);
    }
    return result;
}

// ---- Connection / Graph ----

bool Connection::operator==(const Connection& other) const
{
    return fromNode == other.fromNode && fromPort == other.fromPort
        && toNode == other.toNode && toPort == other.toPort;
}

const Node* Graph::node(const QString& id) const
{
    for (const Node& candidate : nodes) if (candidate.id == id) return &candidate;
    return nullptr;
}

Node* Graph::node(const QString& id)
{
    for (Node& candidate : nodes) if (candidate.id == id) return &candidate;
    return nullptr;
}

QString Graph::addNode(const QString& typeId, const NodeTypeSpec& spec, const QPointF& position)
{
    Node created;
    created.id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    created.typeId = typeId;
    created.position = position;
    created.params = spec.defaultParams();
    nodes.append(created);
    return created.id;
}

void Graph::removeNode(const QString& id)
{
    for (int i = nodes.size() - 1; i >= 0; --i) if (nodes[i].id == id) nodes.removeAt(i);
    for (int i = connections.size() - 1; i >= 0; --i)
        if (connections[i].fromNode == id || connections[i].toNode == id) connections.removeAt(i);
}

bool Graph::connect(const Registry& registry, const Connection& connection, QString* error)
{
    const auto fail = [error](const QString& message) { if (error) *error = message; return false; };
    const Node* source = node(connection.fromNode);
    const Node* sink = node(connection.toNode);
    if (!source || !sink) return fail(QStringLiteral("Unknown node."));
    if (connection.fromNode == connection.toNode) return fail(QStringLiteral("A node cannot feed itself."));

    const NodeTypeSpec* sourceType = registry.find(source->typeId);
    const NodeTypeSpec* sinkType = registry.find(sink->typeId);
    if (!sourceType || !sinkType) return fail(QStringLiteral("Unknown node type."));

    const PortSpec* fromPort = sourceType->output(connection.fromPort);
    const PortSpec* toPort = sinkType->input(connection.toPort);
    if (!fromPort || !toPort) return fail(QStringLiteral("Unknown port."));
    if (!portsCompatible(*fromPort, *toPort))
        return fail(QStringLiteral("%1 output does not fit a %2 input.")
            .arg(valueTypeName(fromPort->type), valueTypeName(toPort->type)));

    // One connection per input port: reconnecting replaces.
    for (int i = connections.size() - 1; i >= 0; --i)
        if (connections[i].toNode == connection.toNode && connections[i].toPort == connection.toPort)
            connections.removeAt(i);

    connections.append(connection);
    if (topologicalOrder().isEmpty() && !nodes.isEmpty()) {
        connections.removeLast();
        return fail(QStringLiteral("That link would create a cycle."));
    }
    return true;
}

void Graph::disconnect(const Connection& connection)
{
    for (int i = connections.size() - 1; i >= 0; --i)
        if (connections[i] == connection) connections.removeAt(i);
}

QVector<Connection> Graph::connectionsTo(const QString& nodeId) const
{
    QVector<Connection> result;
    for (const Connection& connection : connections)
        if (connection.toNode == nodeId) result.append(connection);
    return result;
}

QVector<Connection> Graph::connectionsFrom(const QString& nodeId) const
{
    QVector<Connection> result;
    for (const Connection& connection : connections)
        if (connection.fromNode == nodeId) result.append(connection);
    return result;
}

QStringList Graph::topologicalOrder() const
{
    QHash<QString, int> pending;
    QHash<QString, QStringList> dependents;
    for (const Node& candidate : nodes) pending.insert(candidate.id, 0);
    for (const Connection& connection : connections) {
        if (!pending.contains(connection.fromNode) || !pending.contains(connection.toNode)) continue;
        pending[connection.toNode] += 1;
        dependents[connection.fromNode].append(connection.toNode);
    }
    QStringList ready;
    for (const Node& candidate : nodes) if (pending.value(candidate.id) == 0) ready.append(candidate.id);

    QStringList order;
    while (!ready.isEmpty()) {
        const QString current = ready.takeFirst();
        order.append(current);
        for (const QString& dependent : dependents.value(current))
            if (--pending[dependent] == 0) ready.append(dependent);
    }
    return order.size() == nodes.size() ? order : QStringList();
}

bool Graph::validate(const Registry& registry, QStringList* errors) const
{
    QStringList found;
    QSet<QString> seenIds;
    for (const Node& candidate : nodes) {
        if (seenIds.contains(candidate.id)) found.append(QStringLiteral("Duplicate node id %1.").arg(candidate.id));
        seenIds.insert(candidate.id);
        const NodeTypeSpec* spec = registry.find(candidate.typeId);
        if (!spec) { found.append(QStringLiteral("Unknown node type %1.").arg(candidate.typeId)); continue; }
        for (const PortSpec& port : spec->inputs) {
            if (port.optional) continue;
            const bool connected = std::any_of(connections.begin(), connections.end(), [&](const Connection& c) {
                return c.toNode == candidate.id && c.toPort == port.id;
            });
            if (!connected)
                found.append(QStringLiteral("%1: required input '%2' is unconnected.").arg(spec->name, port.name));
        }
    }
    if (topologicalOrder().isEmpty() && !nodes.isEmpty()) found.append(QStringLiteral("The graph contains a cycle."));
    if (errors) *errors = found;
    return found.isEmpty();
}

QJsonObject Graph::toJson() const
{
    QJsonArray nodeArray;
    for (const Node& candidate : nodes) {
        QJsonObject object{
            {QStringLiteral("id"), candidate.id},
            {QStringLiteral("type"), candidate.typeId},
            {QStringLiteral("x"), candidate.position.x()},
            {QStringLiteral("y"), candidate.position.y()},
            {QStringLiteral("params"), candidate.params}};
        if (!candidate.label.isEmpty()) object.insert(QStringLiteral("label"), candidate.label);
        nodeArray.append(object);
    }
    QJsonArray connectionArray;
    for (const Connection& connection : connections) {
        connectionArray.append(QJsonObject{
            {QStringLiteral("fromNode"), connection.fromNode},
            {QStringLiteral("fromPort"), connection.fromPort},
            {QStringLiteral("toNode"), connection.toNode},
            {QStringLiteral("toPort"), connection.toPort}});
    }
    return QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("FO4.PBRM.NodeGraph")},
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("nodes"), nodeArray},
        {QStringLiteral("connections"), connectionArray}};
}

Graph Graph::fromJson(const QJsonObject& json)
{
    Graph graph;
    for (const QJsonValue& value : json.value(QStringLiteral("nodes")).toArray()) {
        const QJsonObject object = value.toObject();
        Node node;
        node.id = object.value(QStringLiteral("id")).toString();
        node.typeId = object.value(QStringLiteral("type")).toString();
        node.label = object.value(QStringLiteral("label")).toString();
        node.position = QPointF(object.value(QStringLiteral("x")).toDouble(), object.value(QStringLiteral("y")).toDouble());
        node.params = object.value(QStringLiteral("params")).toObject();
        if (!node.id.isEmpty() && !node.typeId.isEmpty()) graph.nodes.append(node);
    }
    for (const QJsonValue& value : json.value(QStringLiteral("connections")).toArray()) {
        const QJsonObject object = value.toObject();
        Connection connection;
        connection.fromNode = object.value(QStringLiteral("fromNode")).toString();
        connection.fromPort = object.value(QStringLiteral("fromPort")).toString();
        connection.toNode = object.value(QStringLiteral("toNode")).toString();
        connection.toPort = object.value(QStringLiteral("toPort")).toString();
        if (!connection.fromNode.isEmpty() && !connection.toNode.isEmpty()) graph.connections.append(connection);
    }
    return graph;
}

// ---- Evaluator ----

void Evaluator::clearCache() { m_cache.clear(); }

QHash<QString, NodeResult> Evaluator::evaluate(const Graph& graph, const EvalContext& context)
{
    QHash<QString, NodeResult> results;
    const QStringList order = graph.topologicalOrder();
    if (order.isEmpty() && !graph.nodes.isEmpty()) {
        for (const Node& node : graph.nodes) {
            NodeResult result;
            result.error = QStringLiteral("The graph contains a cycle.");
            results.insert(node.id, result);
        }
        return results;
    }

    // The context participates in every cache key: a different texture root or
    // source material invalidates the whole graph, as it must.
    QByteArray contextBytes;
    contextBytes += context.textureRoot.toUtf8() + '\x1f' + context.sourcePath.toUtf8() + '\x1f'
        + context.outputRoot.toUtf8() + '\x1f' + context.referenceMesh.toUtf8() + '\x1f'
        + QByteArray::number(context.previewMaxSize) + '\x1f'
        + (context.previewMode ? "preview" : "write");
    const quint64 contextKey = hashBytes(contextBytes);

    QHash<QString, quint64> nodeKeys;
    for (const QString& nodeId : order) {
        const Node* node = graph.node(nodeId);
        const NodeTypeSpec* spec = node ? m_registry.find(node->typeId) : nullptr;
        if (!node || !spec) {
            NodeResult result;
            result.error = QStringLiteral("Unknown node type.");
            results.insert(nodeId, result);
            continue;
        }

        // Gather inputs from upstream results.
        EvalArgs args;
        args.params = node->params;
        args.context = context;
        QByteArray keyBytes = node->typeId.toUtf8() + '\x1f'
            + QJsonDocument(node->params).toJson(QJsonDocument::Compact);
        bool upstreamFailed = false;
        for (const Connection& connection : graph.connectionsTo(nodeId)) {
            const NodeResult& upstream = results[connection.fromNode];
            if (!upstream.error.isEmpty()) upstreamFailed = true;
            const Value value = upstream.outputs.value(connection.fromPort);
            if (value.isValid()) args.inputs.insert(connection.toPort, value);
            keyBytes += '\x1f' + connection.toPort.toUtf8() + '='
                + QByteArray::number(nodeKeys.value(connection.fromNode)) + ':' + connection.fromPort.toUtf8();
        }
        const quint64 key = hashBytes(keyBytes) ^ contextKey;
        nodeKeys.insert(nodeId, key);

        if (upstreamFailed) {
            NodeResult result;
            result.error = QStringLiteral("Upstream node failed.");
            results.insert(nodeId, result);
            continue;
        }

        // Reuse when nothing this node depends on has changed. Write-mode
        // evaluations always re-run: their effect is on disk, not in the cache.
        auto cached = m_cache.constFind(nodeId);
        if (context.previewMode && cached != m_cache.constEnd() && cached->key == key) {
            NodeResult result = cached->result;
            result.cached = true;
            results.insert(nodeId, result);
            continue;
        }

        // Missing required inputs are reported rather than evaluated.
        QString missing;
        for (const PortSpec& port : spec->inputs) {
            if (port.optional || args.inputs.contains(port.id)) continue;
            missing = port.name;
            break;
        }
        NodeResult result;
        if (!missing.isEmpty()) {
            result.error = QStringLiteral("Input '%1' is not connected.").arg(missing);
            results.insert(nodeId, result);
            continue;
        }

        QElapsedTimer timer;
        timer.start();
        EvalOutput output;
        if (spec->evaluate) spec->evaluate(args, &output);
        else output.error = QStringLiteral("Node type has no implementation.");
        result.elapsedMs = timer.elapsed();
        result.outputs = output.outputs;
        result.notes = output.notes;
        result.error = output.error;
        for (auto it = result.outputs.constBegin(); it != result.outputs.constEnd(); ++it)
            if (it.value().type() == ValueType::Image)
                result.thumbnails.insert(it.key(), makeThumbnail(it.value().image(), context.thumbnailSize));

        if (context.previewMode) m_cache.insert(nodeId, {key, result});
        results.insert(nodeId, result);
    }
    return results;
}

} // namespace NodeGraph
