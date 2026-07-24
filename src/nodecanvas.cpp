#include "nodecanvas.h"

#include "dragnumericedit.h"
#include "wheelfocusguard.h"

#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace {

// Node box metrics, in graph units.
constexpr double kNodeWidth = 190;
constexpr double kHeaderHeight = 24;
constexpr double kThumbSize = 128;
constexpr double kPadding = 8;
constexpr double kPortSpacing = 17;
constexpr double kPortRadius = 4.5;

QColor colorForType(NodeGraph::ValueType type)
{
    switch (type) {
    case NodeGraph::ValueType::Image: return QColor(122, 178, 234);
    case NodeGraph::ValueType::Scalar: return QColor(150, 205, 130);
    case NodeGraph::ValueType::Color: return QColor(232, 186, 96);
    case NodeGraph::ValueType::String: return QColor(190, 160, 220);
    case NodeGraph::ValueType::Opaque: return QColor(226, 130, 118);
    case NodeGraph::ValueType::Invalid: break;
    }
    return QColor(150, 150, 150);
}

} // namespace

// ---------------------------------------------------------------- canvas ----

NodeCanvas::NodeCanvas(const NodeGraph::Registry& registry, QWidget* parent)
    : QWidget(parent), m_registry(registry)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumSize(320, 240);
    setToolTip(tr("MMB or empty-space drag pans • wheel zooms • drag a port to wire • right-click to add nodes • Del removes"));
}

const NodeGraph::NodeTypeSpec* NodeCanvas::specFor(const NodeGraph::Node& node) const
{
    return m_registry.find(node.typeId);
}

void NodeCanvas::setGraph(const NodeGraph::Graph& graph)
{
    m_graph = graph;
    if (!m_graph.node(m_selected)) m_selected.clear();
    update();
}

void NodeCanvas::setResults(const QHash<QString, NodeGraph::NodeResult>& results)
{
    m_results = results;
    update();
}

QString NodeCanvas::previewPort(const NodeGraph::Node& node) const
{
    const QString chosen = m_previewPort.value(node.id);
    if (!chosen.isEmpty()) return chosen;
    const NodeGraph::NodeTypeSpec* spec = specFor(node);
    if (!spec) return {};
    for (const NodeGraph::PortSpec& port : spec->outputs)
        if (port.type == NodeGraph::ValueType::Image) return port.id;
    return {};
}

QRectF NodeCanvas::nodeRect(const NodeGraph::Node& node) const
{
    const NodeGraph::NodeTypeSpec* spec = specFor(node);
    const int ports = spec ? std::max(spec->inputs.size(), spec->outputs.size()) : 0;
    const bool hasThumb = spec && !previewPort(node).isEmpty();
    const double body = std::max(hasThumb ? kThumbSize : 0.0, ports * kPortSpacing + kPadding);
    return QRectF(node.position, QSizeF(kNodeWidth, kHeaderHeight + body + kPadding));
}

QPointF NodeCanvas::portCenter(const NodeGraph::Node& node, const QString& portId, bool isOutput) const
{
    const NodeGraph::NodeTypeSpec* spec = specFor(node);
    if (!spec) return node.position;
    const QVector<NodeGraph::PortSpec>& ports = isOutput ? spec->outputs : spec->inputs;
    int index = 0;
    for (int i = 0; i < ports.size(); ++i) if (ports[i].id == portId) index = i;
    const QRectF rect = nodeRect(node);
    const double top = rect.top() + kHeaderHeight + kPortSpacing * 0.5;
    return QPointF(isOutput ? rect.right() : rect.left(), top + index * kPortSpacing);
}

NodeCanvas::PortHit NodeCanvas::portAt(const QPointF& point) const
{
    const double reach = kPortRadius * 2.4;
    for (const NodeGraph::Node& node : m_graph.nodes) {
        const NodeGraph::NodeTypeSpec* spec = specFor(node);
        if (!spec) continue;
        for (const NodeGraph::PortSpec& port : spec->inputs)
            if (QLineF(point, portCenter(node, port.id, false)).length() <= reach)
                return {node.id, port.id, false};
        for (const NodeGraph::PortSpec& port : spec->outputs)
            if (QLineF(point, portCenter(node, port.id, true)).length() <= reach)
                return {node.id, port.id, true};
    }
    return {};
}

QString NodeCanvas::nodeAt(const QPointF& point) const
{
    // Reverse order so the visually topmost node wins.
    for (int i = m_graph.nodes.size() - 1; i >= 0; --i)
        if (nodeRect(m_graph.nodes[i]).contains(point)) return m_graph.nodes[i].id;
    return {};
}

QPointF NodeCanvas::toGraph(const QPointF& widgetPoint) const
{
    return (widgetPoint - m_pan) / m_zoom;
}

QPointF NodeCanvas::toWidget(const QPointF& graphPoint) const
{
    return graphPoint * m_zoom + m_pan;
}

void NodeCanvas::frameAll()
{
    if (m_graph.nodes.isEmpty()) { m_pan = QPointF(); m_zoom = 1.0; update(); return; }
    QRectF bounds;
    for (const NodeGraph::Node& node : m_graph.nodes)
        bounds = bounds.isNull() ? nodeRect(node) : bounds.united(nodeRect(node));
    bounds.adjust(-40, -40, 40, 40);
    const double zoomX = width() / std::max(bounds.width(), 1.0);
    const double zoomY = height() / std::max(bounds.height(), 1.0);
    m_zoom = std::clamp(std::min(zoomX, zoomY), 0.15, 1.4);
    m_pan = QPointF(width() * 0.5, height() * 0.5) - bounds.center() * m_zoom;
    update();
}

void NodeCanvas::drawConnection(QPainter& painter, const QPointF& from, const QPointF& to,
    const QColor& color, bool active)
{
    // Horizontal-tangent bezier: the standard node-editor wire.
    const double slack = std::clamp(std::abs(to.x() - from.x()) * 0.5, 24.0, 120.0);
    QPainterPath path(from);
    path.cubicTo(from + QPointF(slack, 0), to - QPointF(slack, 0), to);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(color, active ? 2.6 : 1.8, Qt::SolidLine, Qt::RoundCap));
    painter.drawPath(path);
}

void NodeCanvas::drawNode(QPainter& painter, const NodeGraph::Node& node)
{
    const NodeGraph::NodeTypeSpec* spec = specFor(node);
    if (!spec) return;
    const NodeGraph::NodeResult result = m_results.value(node.id);
    const QRectF rect = nodeRect(node);
    const bool selected = node.id == m_selected;
    const bool failed = !result.error.isEmpty();

    QPainterPath body;
    body.addRoundedRect(rect, 6, 6);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(44, 47, 52));
    painter.drawPath(body);

    QRectF header(rect.left(), rect.top(), rect.width(), kHeaderHeight);
    QPainterPath headerPath;
    headerPath.addRoundedRect(header, 6, 6);
    painter.setBrush(failed ? QColor(120, 52, 52) : QColor(62, 66, 73));
    painter.drawPath(headerPath);
    painter.setBrush(failed ? QColor(120, 52, 52) : QColor(62, 66, 73));
    painter.drawRect(QRectF(header.left(), header.bottom() - 6, header.width(), 6));

    painter.setPen(QColor(226, 229, 233));
    QFont titleFont = painter.font();
    titleFont.setPixelSize(11);
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.drawText(header.adjusted(8, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft,
        painter.fontMetrics().elidedText(node.label.isEmpty() ? spec->name : node.label,
            Qt::ElideRight, int(header.width() - 16)));

    // Live thumbnail — the reason the graph is worth looking at.
    const QString preview = previewPort(node);
    if (!preview.isEmpty()) {
        const QRectF thumbArea(rect.left() + kPadding, rect.top() + kHeaderHeight + 2,
            rect.width() - kPadding * 2, kThumbSize - 6);
        painter.setBrush(QColor(28, 30, 33));
        painter.setPen(Qt::NoPen);
        painter.drawRect(thumbArea);
        const QImage thumb = result.thumbnails.value(preview);
        if (!thumb.isNull()) {
            const QSizeF scaled = QSizeF(thumb.size()).scaled(thumbArea.size(), Qt::KeepAspectRatio);
            QRectF target(QPointF(), scaled);
            target.moveCenter(thumbArea.center());
            painter.drawImage(target, thumb);
        } else {
            painter.setPen(QColor(120, 124, 130));
            QFont small = painter.font();
            small.setPixelSize(10);
            small.setBold(false);
            painter.setFont(small);
            painter.drawText(thumbArea, Qt::AlignCenter,
                failed ? tr("error") : (result.outputs.isEmpty() ? tr("not evaluated") : tr("no image")));
        }
    }

    QFont portFont = painter.font();
    portFont.setPixelSize(9);
    portFont.setBold(false);
    painter.setFont(portFont);
    const auto drawPorts = [&](const QVector<NodeGraph::PortSpec>& ports, bool isOutput) {
        for (const NodeGraph::PortSpec& port : ports) {
            const QPointF center = portCenter(node, port.id, isOutput);
            const bool connected = std::any_of(m_graph.connections.begin(), m_graph.connections.end(),
                [&](const NodeGraph::Connection& c) {
                    return isOutput ? (c.fromNode == node.id && c.fromPort == port.id)
                                    : (c.toNode == node.id && c.toPort == port.id);
                });
            const QColor color = colorForType(port.type);
            painter.setPen(QPen(color.darker(160), 1));
            painter.setBrush(connected ? color : QColor(44, 47, 52));
            painter.drawEllipse(center, kPortRadius, kPortRadius);
            painter.setPen(QColor(168, 173, 180));
            const QRectF label = isOutput
                ? QRectF(center.x() - 96 - kPortRadius * 2, center.y() - 7, 96, 14)
                : QRectF(center.x() + kPortRadius * 2, center.y() - 7, 96, 14);
            painter.drawText(label, (isOutput ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
                painter.fontMetrics().elidedText(port.name, Qt::ElideRight, 94));
        }
    };
    drawPorts(spec->inputs, false);
    drawPorts(spec->outputs, true);

    if (failed) {
        painter.setPen(QColor(235, 150, 150));
        painter.drawText(QRectF(rect.left() + 6, rect.bottom() - 16, rect.width() - 12, 14),
            Qt::AlignLeft | Qt::AlignVCenter,
            painter.fontMetrics().elidedText(result.error, Qt::ElideRight, int(rect.width() - 12)));
    } else if (!result.notes.isEmpty()) {
        painter.setPen(QColor(150, 155, 162));
        painter.drawText(QRectF(rect.left() + 6, rect.bottom() - 16, rect.width() - 12, 14),
            Qt::AlignLeft | Qt::AlignVCenter,
            painter.fontMetrics().elidedText(result.notes.first(), Qt::ElideRight, int(rect.width() - 12)));
    }

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(selected ? QColor(240, 196, 80) : QColor(24, 26, 29), selected ? 2.0 : 1.0));
    painter.drawPath(body);
}

void NodeCanvas::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor(32, 34, 38));

    // Grid dots, spaced in graph units so they scale with the zoom.
    const double step = 32.0 * m_zoom;
    if (step > 6) {
        painter.setPen(QColor(48, 51, 56));
        const double startX = std::fmod(m_pan.x(), step);
        const double startY = std::fmod(m_pan.y(), step);
        for (double x = startX; x < width(); x += step)
            for (double y = startY; y < height(); y += step)
                painter.drawPoint(QPointF(x, y));
    }

    painter.save();
    painter.translate(m_pan);
    painter.scale(m_zoom, m_zoom);

    for (const NodeGraph::Connection& connection : m_graph.connections) {
        const NodeGraph::Node* from = m_graph.node(connection.fromNode);
        const NodeGraph::Node* to = m_graph.node(connection.toNode);
        if (!from || !to) continue;
        const NodeGraph::NodeTypeSpec* fromSpec = specFor(*from);
        const NodeGraph::PortSpec* port = fromSpec ? fromSpec->output(connection.fromPort) : nullptr;
        const bool touchesSelection = connection.fromNode == m_selected || connection.toNode == m_selected;
        drawConnection(painter, portCenter(*from, connection.fromPort, true),
            portCenter(*to, connection.toPort, false),
            port ? colorForType(port->type) : QColor(150, 150, 150), touchesSelection);
    }

    if (m_wireFrom.valid()) {
        const NodeGraph::Node* from = m_graph.node(m_wireFrom.nodeId);
        if (from)
            drawConnection(painter, portCenter(*from, m_wireFrom.portId, m_wireFrom.isOutput),
                m_wireTo, QColor(240, 196, 80), true);
    }

    for (const NodeGraph::Node& node : m_graph.nodes) drawNode(painter, node);
    painter.restore();
}

void NodeCanvas::mousePressEvent(QMouseEvent* event)
{
    setFocus();
    const QPointF graphPoint = toGraph(event->position());
    m_lastMouse = event->position();

    if (event->button() == Qt::MiddleButton) { m_panning = true; return; }

    if (event->button() == Qt::LeftButton) {
        const PortHit hit = portAt(graphPoint);
        if (hit.valid()) {
            if (hit.isOutput) {
                // Clicking an output also makes it the previewed thumbnail.
                m_previewPort.insert(hit.nodeId, hit.portId);
                update();
            } else {
                // Dragging from a connected input detaches that wire.
                for (const NodeGraph::Connection& connection : m_graph.connectionsTo(hit.nodeId))
                    if (connection.toPort == hit.portId) {
                        m_graph.disconnect(connection);
                        emit graphEdited();
                        break;
                    }
            }
            m_wireFrom = hit;
            m_wireTo = graphPoint;
            return;
        }
        const QString nodeId = nodeAt(graphPoint);
        if (!nodeId.isEmpty()) {
            m_selected = nodeId;
            emit nodeSelected(nodeId);
            m_dragNode = nodeId;
            m_dragOffset = graphPoint - m_graph.node(nodeId)->position;
            update();
            return;
        }
        m_selected.clear();
        emit nodeSelected(QString());
        m_panning = true; // empty-space drag pans
        update();
    }
}

void NodeCanvas::mouseMoveEvent(QMouseEvent* event)
{
    const QPointF graphPoint = toGraph(event->position());
    if (m_panning) {
        m_pan += event->position() - m_lastMouse;
        m_lastMouse = event->position();
        update();
        return;
    }
    if (!m_dragNode.isEmpty()) {
        if (NodeGraph::Node* node = m_graph.node(m_dragNode)) node->position = graphPoint - m_dragOffset;
        update();
        return;
    }
    if (m_wireFrom.valid()) { m_wireTo = graphPoint; update(); return; }

    const QString hovered = nodeAt(graphPoint);
    if (hovered != m_hoverNode) {
        m_hoverNode = hovered;
        const NodeGraph::Node* node = m_graph.node(hovered);
        const NodeGraph::NodeTypeSpec* spec = node ? specFor(*node) : nullptr;
        QString tip = spec ? spec->summary : QString();
        const NodeGraph::NodeResult result = m_results.value(hovered);
        if (!result.notes.isEmpty()) tip += QStringLiteral("\n") + result.notes.join(QStringLiteral("\n"));
        if (!result.error.isEmpty()) tip += QStringLiteral("\n") + result.error;
        setToolTip(tip.isEmpty()
            ? tr("MMB or empty-space drag pans • wheel zooms • drag a port to wire • right-click to add nodes • Del removes")
            : tip);
    }
}

void NodeCanvas::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_wireFrom.valid()) {
        const PortHit target = portAt(toGraph(event->position()));
        if (target.valid() && target.isOutput != m_wireFrom.isOutput) {
            const PortHit& source = m_wireFrom.isOutput ? m_wireFrom : target;
            const PortHit& sink = m_wireFrom.isOutput ? target : m_wireFrom;
            QString error;
            if (m_graph.connect(m_registry, {source.nodeId, source.portId, sink.nodeId, sink.portId}, &error))
                emit graphEdited();
            else if (!error.isEmpty())
                setToolTip(error);
        }
        m_wireFrom = {};
        update();
    }
    if (!m_dragNode.isEmpty()) { m_dragNode.clear(); emit graphEdited(); }
    m_panning = false;
}

void NodeCanvas::wheelEvent(QWheelEvent* event)
{
    const double factor = event->angleDelta().y() > 0 ? 1.12 : 1.0 / 1.12;
    const QPointF anchor = toGraph(event->position());
    m_zoom = std::clamp(m_zoom * factor, 0.15, 2.5);
    // Keep the point under the cursor fixed.
    m_pan = event->position() - anchor * m_zoom;
    update();
}

void NodeCanvas::keyPressEvent(QKeyEvent* event)
{
    if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) && !m_selected.isEmpty()) {
        m_graph.removeNode(m_selected);
        m_selected.clear();
        emit nodeSelected(QString());
        emit graphEdited();
        update();
        return;
    }
    if (event->key() == Qt::Key_F) { frameAll(); return; }
    QWidget::keyPressEvent(event);
}

void NodeCanvas::contextMenuEvent(QContextMenuEvent* event)
{
    const QPointF graphPoint = toGraph(event->pos());
    QMenu menu(this);
    const QString underCursor = nodeAt(graphPoint);
    if (!underCursor.isEmpty()) {
        menu.addAction(tr("Delete Node"), this, [this, underCursor] {
            m_graph.removeNode(underCursor);
            if (m_selected == underCursor) { m_selected.clear(); emit nodeSelected(QString()); }
            emit graphEdited();
            update();
        });
        menu.addSeparator();
    }
    QMenu* add = menu.addMenu(tr("Add Node"));
    for (const QString& category : m_registry.categories()) {
        QMenu* group = add->addMenu(category);
        for (const NodeGraph::NodeTypeSpec* spec : m_registry.all()) {
            if (spec->category != category) continue;
            const QString typeId = spec->id;
            QAction* action = group->addAction(spec->name, this, [this, typeId, graphPoint] {
                const NodeGraph::NodeTypeSpec* target = m_registry.find(typeId);
                if (!target) return;
                const QString id = m_graph.addNode(typeId, *target, graphPoint);
                m_selected = id;
                emit nodeSelected(id);
                emit graphEdited();
                update();
            });
            action->setToolTip(spec->summary);
        }
    }
    menu.addSeparator();
    menu.addAction(tr("Frame All"), this, &NodeCanvas::frameAll);
    menu.exec(event->globalPos());
}

// ------------------------------------------------------------- inspector ----

NodeInspector::NodeInspector(const NodeGraph::Registry& registry, QWidget* parent)
    : QWidget(parent), m_registry(registry)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_body = new QWidget(m_scroll);
    m_form = new QFormLayout(m_body);
    m_form->setContentsMargins(8, 8, 8, 8);
    m_form->setVerticalSpacing(6);
    m_scroll->setWidget(m_body);
    outer->addWidget(m_scroll);
    clear();
}

void NodeInspector::clear()
{
    m_nodeId.clear();
    while (m_form->count() > 0) {
        QLayoutItem* item = m_form->takeAt(0);
        if (QWidget* widget = item->widget()) widget->deleteLater();
        delete item;
    }
    auto* hint = new QLabel(tr("Select a node to edit its parameters."), m_body);
    hint->setObjectName(QStringLiteral("Muted"));
    hint->setWordWrap(true);
    m_form->addRow(hint);
}

void NodeInspector::showNode(const NodeGraph::Graph* graph, const QString& nodeId,
    const NodeGraph::NodeResult& result)
{
    clear();
    const NodeGraph::Node* node = graph ? graph->node(nodeId) : nullptr;
    const NodeGraph::NodeTypeSpec* spec = node ? m_registry.find(node->typeId) : nullptr;
    if (!node || !spec) return;
    m_nodeId = nodeId;

    while (m_form->count() > 0) { // drop the hint row
        QLayoutItem* item = m_form->takeAt(0);
        if (QWidget* widget = item->widget()) widget->deleteLater();
        delete item;
    }

    auto* title = new QLabel(spec->name, m_body);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    title->setFont(titleFont);
    m_form->addRow(title);
    if (!spec->summary.isEmpty()) {
        auto* summary = new QLabel(spec->summary, m_body);
        summary->setObjectName(QStringLiteral("Muted"));
        summary->setWordWrap(true);
        m_form->addRow(summary);
    }

    for (const NodeGraph::ParamSpec& param : spec->params) {
        const QJsonValue current = node->params.value(param.id).isUndefined()
            ? param.defaultValue : node->params.value(param.id);
        const QString id = param.id;
        QWidget* editor = nullptr;
        switch (param.kind) {
        case NodeGraph::ParamSpec::Kind::Float:
        case NodeGraph::ParamSpec::Kind::Int: {
            auto* spin = new DragNumericEdit(m_body);
            const bool integral = param.kind == NodeGraph::ParamSpec::Kind::Int;
            spin->setDecimals(integral ? 0 : 3);
            spin->setRange(param.minimum, param.maximum);
            spin->setSingleStep(param.step > 0 ? param.step : (integral ? 1 : 0.01));
            spin->setValue(current.toDouble(param.defaultValue.toDouble()));
            connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, id, integral](double value) {
                emit parameterChanged(m_nodeId, id, integral ? QJsonValue(int(value)) : QJsonValue(value));
            });
            editor = spin;
            break;
        }
        case NodeGraph::ParamSpec::Kind::Bool: {
            auto* box = new QCheckBox(m_body);
            box->setChecked(current.toBool(param.defaultValue.toBool()));
            connect(box, &QCheckBox::toggled, this, [this, id](bool value) {
                emit parameterChanged(m_nodeId, id, QJsonValue(value));
            });
            editor = box;
            break;
        }
        case NodeGraph::ParamSpec::Kind::Enum: {
            auto* combo = new QComboBox(m_body);
            combo->addItems(param.enumLabels);
            combo->setCurrentIndex(current.toInt(param.defaultValue.toInt()));
            WheelFocusGuard::install(combo);
            connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, id](int value) {
                emit parameterChanged(m_nodeId, id, QJsonValue(value));
            });
            editor = combo;
            break;
        }
        case NodeGraph::ParamSpec::Kind::Path: {
            auto* row = new QWidget(m_body);
            auto* layout = new QHBoxLayout(row);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(5);
            auto* edit = new QLineEdit(current.toString(), row);
            auto* browse = new QPushButton(tr("…"), row);
            browse->setFixedWidth(28);
            layout->addWidget(edit, 1);
            layout->addWidget(browse);
            connect(edit, &QLineEdit::editingFinished, this, [this, id, edit] {
                emit parameterChanged(m_nodeId, id, QJsonValue(edit->text()));
            });
            connect(browse, &QPushButton::clicked, this, [this, id, edit] {
                const QString chosen = QFileDialog::getOpenFileName(this, tr("Select file"), edit->text());
                if (chosen.isEmpty()) return;
                edit->setText(chosen);
                emit parameterChanged(m_nodeId, id, QJsonValue(chosen));
            });
            editor = row;
            break;
        }
        case NodeGraph::ParamSpec::Kind::Color:
        case NodeGraph::ParamSpec::Kind::Text: {
            auto* edit = new QLineEdit(current.toString(), m_body);
            connect(edit, &QLineEdit::editingFinished, this, [this, id, edit] {
                emit parameterChanged(m_nodeId, id, QJsonValue(edit->text()));
            });
            editor = edit;
            break;
        }
        }
        if (!editor) continue;
        if (!param.tooltip.isEmpty()) editor->setToolTip(param.tooltip);
        m_form->addRow(param.name, editor);
    }

    if (!result.error.isEmpty()) {
        auto* error = new QLabel(result.error, m_body);
        error->setWordWrap(true);
        error->setStyleSheet(QStringLiteral("color:#e89090;"));
        m_form->addRow(error);
    }
    for (const QString& note : result.notes) {
        auto* label = new QLabel(note, m_body);
        label->setObjectName(QStringLiteral("Muted"));
        label->setWordWrap(true);
        m_form->addRow(label);
    }
    if (result.elapsedMs > 0)
        m_form->addRow(new QLabel(tr("%1 ms").arg(result.elapsedMs), m_body));
}
