#include "nodegraph.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QSet>
#include <QUuid>

#include <algorithm>

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

bool nodeShowsThumbnail(const NodeTypeSpec& spec)
{
    // A whole material has no one image to show, however many it carries.
    if (!spec.materialPreview.isEmpty()) return false;
    for (const PortSpec& port : spec.outputs)
        if (port.type == ValueType::Image) return true;
    return false;
}

bool nodeShowsChannelStrip(const NodeTypeSpec& spec, const Node& node)
{
    return node.showPreview && spec.materialPreview.isEmpty() && nodeShowsThumbnail(spec);
}

QVector<QPointF> curvePoints(const QString& json)
{
    QVector<QPointF> points;
    for (const QJsonValue& value : QJsonDocument::fromJson(json.toUtf8()).array()) {
        const QJsonArray pair = value.toArray();
        if (pair.size() != 2) continue;
        points.append(QPointF(std::clamp(pair.at(0).toDouble(), 0.0, 1.0),
            std::clamp(pair.at(1).toDouble(), 0.0, 1.0)));
    }
    std::sort(points.begin(), points.end(),
        [](const QPointF& a, const QPointF& b) { return a.x() < b.x(); });
    // Fewer than two points is not a curve; fall back to identity rather than
    // letting a malformed param silently blank the image.
    if (points.size() < 2) points = {QPointF(0, 0), QPointF(1, 1)};
    return points;
}

QString curveToString(const QVector<QPointF>& points)
{
    QJsonArray array;
    for (const QPointF& point : points) array.append(QJsonArray{point.x(), point.y()});
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

double evaluateCurve(const QVector<QPointF>& points, double x)
{
    if (points.size() < 2) return x;
    if (x <= points.first().x()) return points.first().y();
    if (x >= points.last().x()) return points.last().y();
    int i = 0;
    while (i + 2 < points.size() && x > points[i + 1].x()) ++i;
    const QPointF& p0 = points[i];
    const QPointF& p1 = points[i + 1];
    const double h = p1.x() - p0.x();
    if (h <= 1e-9) return p1.y();

    const auto slope = [&](int a, int b) {
        const double dx = points[b].x() - points[a].x();
        return dx > 1e-9 ? (points[b].y() - points[a].y()) / dx : 0.0;
    };
    // Fritsch-Carlson tangents. A plain cubic spline overshoots between control
    // points, which on a tone curve means invented highlights and crushed
    // shadows the user never asked for; this stays monotone between them.
    const double d = slope(i, i + 1);
    double m0 = i == 0 ? d : (slope(i - 1, i) + d) * 0.5;
    double m1 = (i + 2 >= points.size()) ? d : (d + slope(i + 1, i + 2)) * 0.5;
    if (std::abs(d) < 1e-12) {
        m0 = m1 = 0.0; // a flat segment must stay flat
    } else {
        const double a = m0 / d, b = m1 / d;
        if (a < 0.0) m0 = 0.0;
        if (b < 0.0) m1 = 0.0;
        const double magnitude = a * a + b * b;
        if (magnitude > 9.0) {
            const double scale = 3.0 / std::sqrt(magnitude);
            m0 = scale * a * d;
            m1 = scale * b * d;
        }
    }
    const double t = (x - p0.x()) / h;
    const double t2 = t * t, t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * p0.y() + (t3 - 2 * t2 + t) * h * m0
         + (-2 * t3 + 3 * t2) * p1.y() + (t3 - t2) * h * m1;
}

QImage applyChannelMask(const QImage& source, int mask)
{
    if (source.isNull()) return source;
    mask &= kChannelsRgba;
    if (mask == kChannelsRgba) return source;

    QImage image = source.convertToFormat(QImage::Format_RGBA8888);
    const bool bits[4] = {bool(mask & ChannelR), bool(mask & ChannelG),
        bool(mask & ChannelB), bool(mask & ChannelA)};
    const int enabled = int(bits[0]) + int(bits[1]) + int(bits[2]) + int(bits[3]);
    int only = -1;
    for (int c = 0; c < 4; ++c) if (bits[c]) { only = c; break; }

    for (int y = 0; y < image.height(); ++y) {
        uchar* line = image.scanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            uchar* texel = line + x * 4;
            if (enabled == 1) {
                // Isolated: the channel's own value across R, G and B, so it
                // reads as the grayscale mask it actually is.
                const uchar value = texel[only];
                texel[0] = texel[1] = texel[2] = value;
                texel[3] = 255;
            } else {
                for (int c = 0; c < 3; ++c) if (!bits[c]) texel[c] = 0;
                if (!bits[3]) texel[3] = 255;
            }
        }
    }
    return image;
}

bool portVisible(const Node& node, const Graph& graph, const QString& portId, bool isOutput)
{
    if (!node.hideUnusedPorts) return true;
    for (const Connection& connection : graph.connections) {
        if (isOutput ? (connection.fromNode == node.id && connection.fromPort == portId)
                     : (connection.toNode == node.id && connection.toPort == portId))
            return true;
    }
    return false;
}

QSizeF nodeSize(const NodeTypeSpec& spec, const Node& node, const Graph& graph)
{
    // Blender's stacking order: header, preview, outputs, settings, status +
    // warning, inputs. Deriving it here means anything that positions nodes
    // agrees with what the canvas draws.
    double height = Metrics::kHeaderHeight;
    // Collapsed is header-only, so the ports bunch onto the header edges and
    // nothing else in the body contributes height.
    if (node.collapsed) return {Metrics::kNodeWidth, height + Metrics::kPadding};

    if (node.showPreview && nodeShowsThumbnail(spec)) height += Metrics::kThumbSize;
    if (nodeShowsChannelStrip(spec, node)) height += Metrics::kChannelStripHeight;
    int outputs = 0, inputs = 0;
    for (const PortSpec& port : spec.outputs)
        if (portVisible(node, graph, port.id, true)) ++outputs;
    for (const PortSpec& port : spec.inputs)
        if (portVisible(node, graph, port.id, false)) ++inputs;
    height += outputs * Metrics::kPortSpacing;
    if (node.showSettings) {
        // The Levels strip and the curve editor sit directly above the settings
        // they drive, and fold with them: leaving them would defeat the fold.
        if (spec.histogram.valid()) height += Metrics::kHistogramHeight;
        if (spec.curve.valid()) height += Metrics::kCurveHeight;
        height += spec.params.size() * Metrics::kParamRowHeight;
    }
    height += Metrics::kStatusHeight * 2;
    height += inputs * Metrics::kPortSpacing;
    return {Metrics::kNodeWidth, height + Metrics::kPadding * 2};
}

void layoutGraph(Graph& graph, const Registry& registry)
{
    const QStringList order = graph.topologicalOrder();
    if (order.isEmpty()) return; // cyclic graphs keep whatever they had

    QHash<QString, int> depth;
    for (const QString& id : order) {
        int deepest = 0;
        for (const Connection& connection : graph.connectionsTo(id))
            deepest = std::max(deepest, depth.value(connection.fromNode) + 1);
        depth.insert(id, deepest);
    }

    QMap<int, QStringList> columns; // QMap so columns come out left to right
    QHash<QString, int> columnOf;
    for (const QString& id : order) {
        columns[depth.value(id)].append(id);
        columnOf.insert(id, depth.value(id));
    }

    // Route every wire that skips columns through the columns it crosses, as
    // a chain of placeholders. Without them a wire from column 0 to column 8
    // contributes nothing to how columns 1-7 order themselves while crossing
    // everything in them — which is most of this graph, and why ordering by
    // barycentre alone barely moved the crossing count. The placeholders exist
    // only to be ordered; they are dropped before anything is positioned.
    QVector<QPair<QString, QString>> segments; // always between adjacent columns
    for (int index = 0; index < graph.connections.size(); ++index) {
        const Connection& connection = graph.connections.at(index);
        const int from = columnOf.value(connection.fromNode, -1);
        const int to = columnOf.value(connection.toNode, -1);
        if (from < 0 || to < 0 || to <= from) continue; // layering guarantees the rest
        QString previous = connection.fromNode;
        for (int column = from + 1; column < to; ++column) {
            // \x01 cannot occur in a node id, so a placeholder can never
            // collide with something real.
            const QString placeholder = QStringLiteral("\x01%1:%2").arg(index).arg(column);
            columns[column].append(placeholder);
            columnOf.insert(placeholder, column);
            segments.append({previous, placeholder});
            previous = placeholder;
        }
        segments.append({previous, connection.toNode});
    }
    QHash<QString, QStringList> predecessors, successors;
    for (const auto& segment : segments) {
        successors[segment.first].append(segment.second);
        predecessors[segment.second].append(segment.first);
    }

    // Seed each column from the positions the graph already had, so a
    // hand-arranged network stays recognisable through a reflow. Placeholders
    // have no position and sort to the middle, where their first sweep moves
    // them somewhere meaningful.
    for (auto it = columns.begin(); it != columns.end(); ++it) {
        std::stable_sort(it.value().begin(), it.value().end(),
            [&graph](const QString& a, const QString& b) {
                const Node* left = graph.node(a);
                const Node* right = graph.node(b);
                return (left ? left->position.y() : 0.0) < (right ? right->position.y() : 0.0);
            });
    }

    // Where a node sits in its column, normalised so columns of different
    // sizes compare. Ordering is settled on ranks, not pixels: it is a
    // combinatorial problem and the real heights are not needed to solve it.
    const auto rankOf = [&columns, &columnOf](const QString& id) {
        const QStringList& column = columns[columnOf.value(id)];
        const qsizetype index = column.indexOf(id);
        if (index < 0 || column.size() < 2) return 0.5;
        return double(index) / double(column.size() - 1);
    };
    // Every segment now spans exactly one column gap, so this is a real
    // adjacent-layer crossing count rather than a proxy: two segments between
    // the same pair of columns cross exactly when their endpoints are ordered
    // oppositely.
    const auto crossings = [&segments, &columnOf, &rankOf] {
        int total = 0;
        for (int i = 0; i < segments.size(); ++i)
            for (int j = i + 1; j < segments.size(); ++j) {
                if (columnOf.value(segments.at(i).first)
                    != columnOf.value(segments.at(j).first)) continue;
                if ((rankOf(segments.at(i).first) - rankOf(segments.at(j).first))
                        * (rankOf(segments.at(i).second) - rankOf(segments.at(j).second)) < 0)
                    ++total;
            }
        return total;
    };
    // Ordering by inputs alone — which is all this used to do — can only ever
    // see half of a node's wires: something feeding four nodes downstream never
    // gets pulled toward them. Sweeping back the other way sees that half, and
    // alternating settles far better than either direction on its own.
    const auto sweep = [&](bool byInputs) {
        const QList<int> keys = columns.keys();
        for (int step = 0; step < keys.size(); ++step) {
            const int key = byInputs ? keys.at(step) : keys.at(keys.size() - 1 - step);
            QStringList& column = columns[key];
            if (column.size() < 2) continue;
            QHash<QString, double> sortKey;
            for (const QString& id : column) {
                const QStringList& neighbours = byInputs ? predecessors[id] : successors[id];
                double sum = 0;
                for (const QString& other : neighbours) sum += rankOf(other);
                // Nothing on that side: hold station rather than being flung to
                // an arbitrary end of the column.
                sortKey.insert(id, neighbours.isEmpty() ? rankOf(id)
                                                        : sum / double(neighbours.size()));
            }
            std::stable_sort(column.begin(), column.end(),
                [&sortKey](const QString& a, const QString& b) {
                    return sortKey.value(a) < sortKey.value(b);
                });
        }
    };

    // Keep the best ordering seen, not the last one: sweeps are not monotonic
    // and the final round can be worse than one before it.
    // Keep the best ordering seen, not the last one: sweeps are not monotonic
    // and the final round can be worse than one before it. More rounds than the
    // four this used to run, because with placeholders there is more to settle.
    QMap<int, QStringList> best = columns;
    int bestCrossings = crossings();
    for (int round = 0; round < 12 && bestCrossings > 0; ++round) {
        sweep(true);
        sweep(false);
        const int score = crossings();
        if (score < bestCrossings) { bestCrossings = score; best = columns; }
    }
    columns = best;

    // Placeholders keep their slot through placement: reserving a lane is what
    // stops a long wire being drawn straight over the nodes it passes. They are
    // narrow — a wire needs a gap, not a node's worth of room.
    constexpr double kLaneHeight = 14.0;
    for (Connection& connection : graph.connections) connection.waypoints.clear();
    QHash<QString, QPointF> lanePosition;

    double x = 0;
    for (auto it = columns.begin(); it != columns.end(); ++it) {
        const QStringList& column = it.value();
        QHash<QString, QSizeF> sizes;
        double total = 0;
        for (const QString& id : column) {
            const Node* node = graph.node(id);
            const NodeTypeSpec* spec = node ? registry.find(node->typeId) : nullptr;
            const QSizeF size = node && spec ? nodeSize(*spec, *node, graph)
                : (node ? QSizeF(Metrics::kNodeWidth, Metrics::kHeaderHeight + Metrics::kPadding)
                        : QSizeF(Metrics::kNodeWidth, kLaneHeight));
            sizes.insert(id, size);
            total += size.height() + Metrics::kRowGap;
        }
        if (total > 0) total -= Metrics::kRowGap;

        double y = -total * 0.5; // columns share a horizontal centre line
        for (const QString& id : column) {
            const QSizeF size = sizes.value(id);
            if (Node* node = graph.node(id)) node->position = QPointF(x, y);
            else lanePosition.insert(id, QPointF(x + Metrics::kNodeWidth * 0.5, y + size.height() * 0.5));
            y += size.height() + Metrics::kRowGap;
        }
        x += Metrics::kNodeWidth + Metrics::kColumnGap;
    }

    // Hand each wire the lanes reserved for it, in the order it crosses them.
    for (int index = 0; index < graph.connections.size(); ++index) {
        Connection& connection = graph.connections[index];
        const int from = columnOf.value(connection.fromNode, -1);
        const int to = columnOf.value(connection.toNode, -1);
        if (from < 0 || to <= from + 1) continue; // adjacent or unknown: draw it straight
        for (int column = from + 1; column < to; ++column) {
            const auto lane = lanePosition.constFind(
                QStringLiteral("\x01%1:%2").arg(index).arg(column));
            if (lane != lanePosition.constEnd()) connection.waypoints.append(lane.value());
        }
    }
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

// Workflow order, so the Add Node menu reads the way a graph flows rather than
// in whatever order the library happened to register its types.
static int categoryRank(const QString& category)
{
    static const QStringList order{QStringLiteral("Source"), QStringLiteral("Inference"),
        QStringLiteral("Derive"), QStringLiteral("Bake"), QStringLiteral("Composite"),
        QStringLiteral("Maths"), QStringLiteral("Color"), QStringLiteral("Output")};
    const int index = order.indexOf(category);
    return index < 0 ? order.size() : index;
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
    std::stable_sort(result.begin(), result.end(),
        [](const QString& a, const QString& b) { return categoryRank(a) < categoryRank(b); });
    return result;
}

void Registry::setCategoryColor(const QString& category, const QColor& color)
{
    m_categoryColors.insert(category, color);
}

QColor Registry::categoryColor(const QString& category) const
{
    return m_categoryColors.value(category, QColor());
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
        // Fold state is written only when it differs from the default, so an
        // untouched graph serialises exactly as it did before folding existed.
        if (!candidate.showPreview) object.insert(QStringLiteral("showPreview"), false);
        if (!candidate.showSettings) object.insert(QStringLiteral("showSettings"), false);
        if (candidate.collapsed) object.insert(QStringLiteral("collapsed"), true);
        if (candidate.hideUnusedPorts) object.insert(QStringLiteral("hideUnusedPorts"), true);
        if (candidate.previewChannels != kChannelsRgb)
            object.insert(QStringLiteral("previewChannels"), candidate.previewChannels);
        nodeArray.append(object);
    }
    QJsonArray connectionArray;
    for (const Connection& connection : connections) {
        QJsonObject object{
            {QStringLiteral("fromNode"), connection.fromNode},
            {QStringLiteral("fromPort"), connection.fromPort},
            {QStringLiteral("toNode"), connection.toNode},
            {QStringLiteral("toPort"), connection.toPort}};
        // Written only when a wire actually has lanes, so a graph of short
        // wires serialises exactly as it did before routing existed.
        if (!connection.waypoints.isEmpty()) {
            QJsonArray lanes;
            for (const QPointF& lane : connection.waypoints) { lanes.append(lane.x()); lanes.append(lane.y()); }
            object.insert(QStringLiteral("waypoints"), lanes);
        }
        connectionArray.append(object);
    }
    QJsonArray frameArray;
    for (const Frame& item : frames) {
        frameArray.append(QJsonObject{
            {QStringLiteral("id"), item.id},
            {QStringLiteral("title"), item.title},
            {QStringLiteral("x"), item.rect.x()}, {QStringLiteral("y"), item.rect.y()},
            {QStringLiteral("w"), item.rect.width()}, {QStringLiteral("h"), item.rect.height()},
            {QStringLiteral("color"), item.color.name()}});
    }
    QJsonObject object{
        {QStringLiteral("schema"), QStringLiteral("FO4.PBRM.NodeGraph")},
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("nodes"), nodeArray},
        {QStringLiteral("connections"), connectionArray}};
    // Only written when used, so existing .pbrgraph files stay byte-identical.
    if (!frameArray.isEmpty()) object.insert(QStringLiteral("frames"), frameArray);
    return object;
}

QString Graph::addFrame(const QString& title, const QRectF& rect)
{
    Frame item;
    item.id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    item.title = title;
    item.rect = rect;
    frames.append(item);
    return item.id;
}

void Graph::removeFrame(const QString& id)
{
    // Removing a container never removes what it holds.
    frames.erase(std::remove_if(frames.begin(), frames.end(),
                     [&](const Frame& item) { return item.id == id; }),
        frames.end());
}

const Frame* Graph::frame(const QString& id) const
{
    for (const Frame& item : frames) if (item.id == id) return &item;
    return nullptr;
}

Frame* Graph::frame(const QString& id)
{
    for (Frame& item : frames) if (item.id == id) return &item;
    return nullptr;
}

QStringList Graph::nodesInFrame(const QString& id, const Registry& registry) const
{
    const Frame* item = frame(id);
    if (!item) return {};
    QStringList inside;
    for (const Node& node : nodes) {
        const NodeTypeSpec* spec = registry.find(node.typeId);
        const QSizeF size = spec ? nodeSize(*spec, node, *this) : QSizeF(Metrics::kNodeWidth, Metrics::kHeaderHeight);
        if (item->rect.contains(QRectF(node.position, size))) inside.append(node.id);
    }
    return inside;
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
        node.showPreview = object.value(QStringLiteral("showPreview")).toBool(true);
        node.showSettings = object.value(QStringLiteral("showSettings")).toBool(true);
        node.collapsed = object.value(QStringLiteral("collapsed")).toBool(false);
        node.hideUnusedPorts = object.value(QStringLiteral("hideUnusedPorts")).toBool(false);
        node.previewChannels = object.value(QStringLiteral("previewChannels")).toInt(kChannelsRgb);
        if (!node.id.isEmpty() && !node.typeId.isEmpty()) graph.nodes.append(node);
    }
    for (const QJsonValue& value : json.value(QStringLiteral("connections")).toArray()) {
        const QJsonObject object = value.toObject();
        Connection connection;
        connection.fromNode = object.value(QStringLiteral("fromNode")).toString();
        connection.fromPort = object.value(QStringLiteral("fromPort")).toString();
        connection.toNode = object.value(QStringLiteral("toNode")).toString();
        connection.toPort = object.value(QStringLiteral("toPort")).toString();
        // Routing lanes are layout, and layout is saved with the graph — a
        // reloaded network should look exactly as it was left.
        const QJsonArray lanes = object.value(QStringLiteral("waypoints")).toArray();
        for (int lane = 0; lane + 1 < lanes.size(); lane += 2)
            connection.waypoints.append(QPointF(lanes.at(lane).toDouble(), lanes.at(lane + 1).toDouble()));
        if (!connection.fromNode.isEmpty() && !connection.toNode.isEmpty()) graph.connections.append(connection);
    }
    for (const QJsonValue& value : json.value(QStringLiteral("frames")).toArray()) {
        const QJsonObject object = value.toObject();
        Frame item;
        item.id = object.value(QStringLiteral("id")).toString();
        item.title = object.value(QStringLiteral("title")).toString();
        item.rect = QRectF(object.value(QStringLiteral("x")).toDouble(), object.value(QStringLiteral("y")).toDouble(),
            object.value(QStringLiteral("w")).toDouble(), object.value(QStringLiteral("h")).toDouble());
        const QColor color(object.value(QStringLiteral("color")).toString());
        if (color.isValid()) item.color = color;
        if (!item.id.isEmpty()) graph.frames.append(item);
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
        + context.correctionMaskPath.toUtf8() + '\x1f'
        + context.maskDirectory.toUtf8() + '\x1f' + context.maskMaterialKey.toUtf8() + '\x1f'
        // Paint Mask nodes read a file the key cannot otherwise see. The
        // workspace bumps this every time a stroke is committed, so painting
        // invalidates the cache — without it the graph would keep serving the
        // mask as it was when the material was first evaluated.
        + QByteArray::number(context.maskGeneration) + '\x1f'
        + QByteArray::number(context.previewMaxSize) + '\x1f'
        + QByteArray::number(context.materialMaxResolution) + '\x1f'
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
        args.nodeId = node->id;
        QByteArray keyBytes = node->typeId.toUtf8() + '\x1f'
            + QJsonDocument(node->params).toJson(QJsonDocument::Compact);
        bool upstreamFailed = false;
        for (const Connection& connection : graph.connectionsTo(nodeId)) {
            const NodeResult& upstream = results[connection.fromNode];
            if (!upstream.error.isEmpty()) upstreamFailed = true;
            args.inputNative.insert(connection.toPort, upstream.nativeResolution);
            // The upstream port's own label, so a node can tell what KIND of
            // map reached it without a second setting duplicating the wiring.
            if (const Node* from = graph.node(connection.fromNode))
                if (const NodeTypeSpec* fromSpec = m_registry.find(from->typeId))
                    if (const PortSpec* fromPort = fromSpec->output(connection.fromPort))
                        args.inputLabels.insert(connection.toPort, fromPort->name);
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
        result.status = output.status;
        result.warning = output.warning;
        result.error = output.error;
        result.nativeResolution = output.nativeResolution;
        result.histogram = output.histogram;
        result.resolvedLevels = output.resolvedLevels;

        int largestImage = 0;
        for (auto it = result.outputs.constBegin(); it != result.outputs.constEnd(); ++it) {
            if (it.value().type() != ValueType::Image) continue;
            const QImage& image = it.value().image();
            largestImage = std::max(largestImage, std::max(image.width(), image.height()));
            result.thumbnails.insert(it.key(), makeThumbnail(image, context.thumbnailSize));
        }

        // Native resolution is what the node would produce without the preview
        // downsample, so it flows down from whichever source fed this node
        // unless the node states its own (a resample, an upscale).
        if (result.nativeResolution == 0) {
            for (const Connection& connection : graph.connectionsTo(nodeId))
                result.nativeResolution = std::max(result.nativeResolution,
                    results[connection.fromNode].nativeResolution);
        }
        if (result.nativeResolution == 0) result.nativeResolution = largestImage;

        if (result.status.isEmpty() && largestImage > 0) {
            for (const PortSpec& port : spec->outputs) {
                if (port.type != ValueType::Image) continue;
                const QImage primary = result.outputs.value(port.id).image();
                if (primary.isNull()) continue;
                result.status = QStringLiteral("%1×%2").arg(primary.width()).arg(primary.height());
                break;
            }
        }
        // The material's own largest texture is the bar every map is held to.
        if (result.warning.isEmpty() && context.materialMaxResolution > 0 && result.nativeResolution > 0
            && result.nativeResolution < context.materialMaxResolution) {
            result.warning = QStringLiteral("%1px — below the material's %2px")
                .arg(result.nativeResolution).arg(context.materialMaxResolution);
        }

        if (context.previewMode) m_cache.insert(nodeId, {key, result});
        results.insert(nodeId, result);
    }
    return results;
}

} // namespace NodeGraph
