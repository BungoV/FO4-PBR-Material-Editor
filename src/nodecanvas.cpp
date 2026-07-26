#include "nodecanvas.h"

#include "dragnumericedit.h"
#include "wheelfocusguard.h"

#include <QStringBuilder>
#include <QCheckBox>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QFileDialog>
#include <QGuiApplication>
#include <QFormLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {

// Geometry comes from NodeGraph::Metrics so generated layouts and the canvas
// cannot disagree about how big a node is.
using NodeGraph::Metrics::kNodeWidth;
using NodeGraph::Metrics::kHeaderHeight;
using NodeGraph::Metrics::kThumbSize;
using NodeGraph::Metrics::kPadding;
using NodeGraph::Metrics::kPortSpacing;
using NodeGraph::Metrics::kPortRadius;
using NodeGraph::Metrics::kParamRowHeight;
using NodeGraph::Metrics::kStatusHeight;

constexpr double kFrameTitleHeight = 22; // grab strip of a container

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
    setToolTip(tr("MMB or empty-space drag pans â€¢ wheel zooms â€¢ drag a port to wire â€¢ right-click to add nodes â€¢ Del removes"));
}

const NodeGraph::NodeTypeSpec* NodeCanvas::specFor(const NodeGraph::Node& node) const
{
    return m_registry.find(node.typeId);
}

void NodeCanvas::setGraph(const NodeGraph::Graph& graph)
{
    if (m_inlineEdit && m_inlineEdit->isVisible()) { m_inlineParam = {}; m_inlineEdit->hide(); }
    m_scrubParam = {};
    m_graph = graph;
    m_thumbCache.clear(); // node ids may mean something else entirely now
    if (!m_graph.node(m_selected)) m_selected.clear();
    m_framed = false; // re-fit the new graph on the next paint
    update();
}

void NodeCanvas::setCornerWidget(QWidget* widget)
{
    m_cornerWidget = widget;
    if (!widget) return;
    widget->setParent(this);
    widget->raise();
    widget->show();
    resizeEvent(nullptr);
}

void NodeCanvas::setPlaceholder(const QString& text)
{
    m_placeholder = text;
    update();
}

void NodeCanvas::resizeEvent(QResizeEvent* event)
{
    if (event) QWidget::resizeEvent(event);
    if (!m_cornerWidget) return;
    const QSize hint = m_cornerWidget->sizeHint();
    m_cornerWidget->setGeometry(width() - hint.width() - 10, 10, hint.width(), hint.height());
}

void NodeCanvas::setResults(const QHash<QString, NodeGraph::NodeResult>& results)
{
    m_results = results;
    m_thumbCache.clear(); // new images behind every key
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

NodeCanvas::NodeLayout NodeCanvas::layoutFor(const NodeGraph::Node& node) const
{
    NodeLayout layout;
    const NodeGraph::NodeTypeSpec* spec = specFor(node);
    if (!spec) {
        layout.rect = QRectF(node.position, QSizeF(kNodeWidth, kHeaderHeight + kPadding));
        layout.header = layout.rect;
        return layout;
    }
    layout.rect = QRectF(node.position, NodeGraph::nodeSize(*spec, node, m_graph));
    layout.header = QRectF(layout.rect.left(), layout.rect.top(), layout.rect.width(), kHeaderHeight);

    const double left = layout.rect.left();
    const double width = layout.rect.width();
    double y = layout.rect.top() + kHeaderHeight + kPadding;

    // Collapsed: nothing but the header, with every port folded onto its edges
    // so the wires still land somewhere sensible. The rows stay parallel to the
    // spec â€” they are simply all the header rect.
    if (node.collapsed) {
        for (int i = 0; i < spec->outputs.size(); ++i) layout.outputRows.append(layout.header);
        for (int i = 0; i < spec->inputs.size(); ++i) layout.inputRows.append(layout.header);
        for (int i = 0; i < spec->params.size(); ++i) layout.paramRows.append(QRectF());
        return layout;
    }

    // Preview sits directly under the header and spans the full body, so it
    // can never land on top of a port or a setting.
    if (node.showPreview && NodeGraph::nodeShowsThumbnail(*spec)) {
        layout.thumb = QRectF(left + kPadding, y, width - kPadding * 2, kThumbSize - kPadding);
        y += kThumbSize;
    }
    // Null on a material-level node, which disables the toggles for drawing and
    // for hit-testing in one move.
    if (NodeGraph::nodeShowsChannelStrip(*spec, node)) {
        layout.channelStrip = QRectF(left + kPadding, y, width - kPadding * 2,
            NodeGraph::Metrics::kChannelStripHeight);
        y += NodeGraph::Metrics::kChannelStripHeight;
    }
    // Hidden ports keep their slot in the parallel array as a null rect, so
    // every index-based lookup downstream stays aligned with the spec.
    for (int i = 0; i < spec->outputs.size(); ++i) {
        if (!NodeGraph::portVisible(node, m_graph, spec->outputs[i].id, true)) {
            layout.outputRows.append(QRectF());
            continue;
        }
        layout.outputRows.append(QRectF(left, y, width, kPortSpacing));
        y += kPortSpacing;
    }
    if (node.showSettings) {
        // The Levels strip sits directly above the settings it drives, so the
        // handles and their numeric readouts read as one control.
        if (spec->histogram.valid()) {
            layout.histogram = QRectF(left + kPadding, y, width - kPadding * 2,
                NodeGraph::Metrics::kHistogramHeight);
            y += NodeGraph::Metrics::kHistogramHeight;
        }
        if (spec->curve.valid()) {
            layout.curve = QRectF(left + kPadding, y, width - kPadding * 2,
                NodeGraph::Metrics::kCurveHeight);
            y += NodeGraph::Metrics::kCurveHeight;
        }
        for (int i = 0; i < spec->params.size(); ++i) {
            layout.paramRows.append(QRectF(left + kPadding, y + 1, width - kPadding * 2, kParamRowHeight - 3));
            y += kParamRowHeight;
        }
    } else {
        for (int i = 0; i < spec->params.size(); ++i) layout.paramRows.append(QRectF());
    }
    layout.statusRow = QRectF(left + kPadding, y, width - kPadding * 2, kStatusHeight);
    y += kStatusHeight;
    layout.warningRow = QRectF(left + kPadding, y, width - kPadding * 2, kStatusHeight);
    y += kStatusHeight;
    for (int i = 0; i < spec->inputs.size(); ++i) {
        if (!NodeGraph::portVisible(node, m_graph, spec->inputs[i].id, false)) {
            layout.inputRows.append(QRectF());
            continue;
        }
        layout.inputRows.append(QRectF(left, y, width, kPortSpacing));
        y += kPortSpacing;
    }
    return layout;
}

QRectF NodeCanvas::nodeRect(const NodeGraph::Node& node) const
{
    return layoutFor(node).rect;
}

QPointF NodeCanvas::portCenter(const NodeGraph::Node& node, const QString& portId, bool isOutput) const
{
    const NodeGraph::NodeTypeSpec* spec = specFor(node);
    if (!spec) return node.position;
    const NodeLayout layout = layoutFor(node);
    const QVector<NodeGraph::PortSpec>& ports = isOutput ? spec->outputs : spec->inputs;
    const QVector<QRectF>& rows = isOutput ? layout.outputRows : layout.inputRows;
    for (int i = 0; i < ports.size() && i < rows.size(); ++i) {
        if (ports[i].id != portId) continue;
        // A hidden port has a null row. Its wires still have to land somewhere,
        // so they anchor to the header edge â€” the same place a collapsed node
        // gathers everything.
        const double y = rows[i].isNull() ? layout.header.center().y() : rows[i].center().y();
        return QPointF(isOutput ? layout.rect.right() : layout.rect.left(), y);
    }
    return layout.rect.center();
}

NodeCanvas::ParamHit NodeCanvas::paramAt(const QPointF& point) const
{
    for (int i = m_graph.nodes.size() - 1; i >= 0; --i) {
        const NodeGraph::Node& node = m_graph.nodes[i];
        const NodeGraph::NodeTypeSpec* spec = specFor(node);
        if (!spec) continue;
        const NodeLayout layout = layoutFor(node);
        for (int p = 0; p < spec->params.size() && p < layout.paramRows.size(); ++p)
            if (layout.paramRows[p].contains(point))
                return {node.id, spec->params[p].id, layout.paramRows[p]};
    }
    return {};
}

namespace {

// The midtone handle rides between the two input handles. Centre is gamma 1;
// dragging left brightens, which is the Photoshop/Substance convention and
// matches pow(value, 1/gamma) getting a smaller exponent as gamma rises.
constexpr double kGammaSpan = 5.0;

double gammaToFraction(double gamma)
{
    const double clamped = std::clamp(gamma, 1.0 / kGammaSpan, kGammaSpan);
    return std::clamp((1.0 - std::log(clamped) / std::log(kGammaSpan)) * 0.5, 0.0, 1.0);
}

double fractionToGamma(double fraction)
{
    return std::pow(kGammaSpan, 1.0 - 2.0 * std::clamp(fraction, 0.0, 1.0));
}

// Sub-rects of the strip: input rail on top, distribution, output rail below.
QRectF histogramPlot(const QRectF& area)
{
    // Inset horizontally so handles parked at 0 and 1 still sit fully inside
    // the node instead of hanging off the plot edge.
    constexpr double kHandleInset = 5.0;
    return QRectF(area.left() + kHandleInset, area.top() + NodeGraph::Metrics::kHistogramRailHeight,
        area.width() - kHandleInset * 2, NodeGraph::Metrics::kHistogramPlotHeight);
}

} // namespace

void NodeCanvas::drawHistogram(QPainter& painter, const NodeGraph::Node& node,
    const NodeGraph::NodeTypeSpec& spec, const QRectF& area)
{
    const NodeGraph::HistogramSpec& binding = spec.histogram;
    const NodeGraph::NodeResult& result = m_results.value(node.id);
    const QRectF plot = histogramPlot(area);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(24, 26, 30));
    painter.drawRoundedRect(plot, 2, 2);

    const auto paramValue = [&](const QString& id, double fallback) {
        const NodeGraph::ParamSpec* ps = spec.param(id);
        const QJsonValue raw = node.params.value(id);
        if (raw.isDouble()) return raw.toDouble();
        return ps && ps->defaultValue.isDouble() ? ps->defaultValue.toDouble() : fallback;
    };
    int channel = 4;
    if (!binding.channel.isEmpty()) {
        const QJsonValue raw = node.params.value(binding.channel);
        const NodeGraph::ParamSpec* ps = spec.param(binding.channel);
        channel = raw.isDouble() ? raw.toInt()
                                 : (ps && ps->defaultValue.isDouble() ? ps->defaultValue.toInt() : 4);
    }
    channel = std::clamp(channel, 0, NodeGraph::kHistogramChannels - 1);

    const int expected = NodeGraph::kHistogramChannels * NodeGraph::kHistogramBuckets;
    if (result.histogram.size() == expected) {
        const quint32* buckets = result.histogram.constData() + channel * NodeGraph::kHistogramBuckets;
        // Normalise to a high percentile, not the maximum: a constant channel
        // (BC1 alpha is always 1.0) puts every texel in one bucket and would
        // flatten the rest of the plot to nothing.
        QVector<quint32> sorted(NodeGraph::kHistogramBuckets);
        std::copy(buckets, buckets + NodeGraph::kHistogramBuckets, sorted.begin());
        std::sort(sorted.begin(), sorted.end());
        const double scale = std::max<double>(1.0, sorted[int(NodeGraph::kHistogramBuckets * 0.98)]);

        static const QColor channelInk[NodeGraph::kHistogramChannels] = {
            QColor(214, 106, 106), QColor(122, 196, 122), QColor(110, 150, 226),
            QColor(180, 180, 188), QColor(158, 164, 174)};
        painter.setBrush(channelInk[channel]);
        const double step = plot.width() / double(NodeGraph::kHistogramBuckets);
        for (int i = 0; i < NodeGraph::kHistogramBuckets; ++i) {
            // Square-root scale. A painted surface piles almost every texel into
            // a couple of buckets, and on a linear plot that spike flattens the
            // sparse metal cluster to nothing â€” which is exactly the feature the
            // black and white points need to be placed between.
            const double height = std::sqrt(std::min(1.0, buckets[i] / scale)) * (plot.height() - 2);
            if (height <= 0.0) continue;
            painter.drawRect(QRectF(plot.left() + i * step, plot.bottom() - height,
                std::max(step, 1.0), height));
        }
    } else {
        painter.setPen(QColor(120, 126, 136));
        QFont hint = painter.font();
        hint.setPixelSize(9);
        painter.setFont(hint);
        painter.drawText(plot, Qt::AlignCenter, tr("no input"));
    }

    // Handles. Input trio rides the rail above the plot, output pair below.
    // A node that resolved its own black/white points — autodetected Levels —
    // publishes them, and those win: handles that sit somewhere other than
    // where the remap actually happens are exactly the confusion this strip
    // exists to remove.
    const bool resolved = result.resolvedLevels.size() == 2;
    const double inBlack = resolved ? result.resolvedLevels.at(0) : paramValue(binding.blackPoint, 0.0);
    const double inWhite = resolved ? result.resolvedLevels.at(1) : paramValue(binding.whitePoint, 1.0);
    const auto markerX = [&](double fraction) {
        return plot.left() + std::clamp(fraction, 0.0, 1.0) * plot.width();
    };
    const auto drawMarker = [&](double x, double top, bool pointingDown, const QColor& fill) {
        const double h = NodeGraph::Metrics::kHistogramRailHeight - 2;
        QPolygonF marker;
        if (pointingDown) marker << QPointF(x - 4, top) << QPointF(x + 4, top) << QPointF(x, top + h);
        else marker << QPointF(x - 4, top + h) << QPointF(x + 4, top + h) << QPointF(x, top);
        painter.setPen(QPen(QColor(18, 20, 24), 0.8));
        painter.setBrush(fill);
        painter.drawPolygon(marker);
    };

    // Vertical guides make it obvious which part of the distribution survives.
    painter.setPen(QPen(QColor(255, 255, 255, 40), 0.8));
    painter.drawLine(QPointF(markerX(inBlack), plot.top()), QPointF(markerX(inBlack), plot.bottom()));
    painter.drawLine(QPointF(markerX(inWhite), plot.top()), QPointF(markerX(inWhite), plot.bottom()));

    const double railTop = area.top();
    drawMarker(markerX(inBlack), railTop, true, QColor(30, 32, 38));
    drawMarker(markerX(inWhite), railTop, true, QColor(238, 240, 244));
    if (!binding.gamma.isEmpty()) {
        const double fraction = gammaToFraction(paramValue(binding.gamma, 1.0));
        drawMarker(markerX(inBlack + (inWhite - inBlack) * fraction), railTop, true, QColor(140, 146, 156));
    }
    const double outRailTop = plot.bottom() + 1;
    if (!binding.outBlack.isEmpty())
        drawMarker(markerX(paramValue(binding.outBlack, 0.0)), outRailTop, false, QColor(30, 32, 38));
    if (!binding.outWhite.isEmpty())
        drawMarker(markerX(paramValue(binding.outWhite, 1.0)), outRailTop, false, QColor(238, 240, 244));
}

void NodeCanvas::drawCurve(QPainter& painter, const NodeGraph::Node& node,
    const NodeGraph::NodeTypeSpec& spec, const QRectF& area)
{
    const QRectF plot = area.adjusted(4, 3, -4, -3);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(24, 26, 30));
    painter.drawRoundedRect(plot, 2, 2);

    int channel = 4;
    if (!spec.curve.channel.isEmpty()) {
        const QJsonValue raw = node.params.value(spec.curve.channel);
        if (raw.isDouble()) channel = raw.toInt();
    }
    channel = std::clamp(channel, 0, NodeGraph::kHistogramChannels - 1);

    // Input distribution behind the curve, the way Photoshop shows it: the
    // curve means little without seeing which levels it actually acts on.
    const NodeGraph::NodeResult& result = m_results.value(node.id);
    const int expected = NodeGraph::kHistogramChannels * NodeGraph::kHistogramBuckets;
    if (result.histogram.size() == expected) {
        const quint32* buckets = result.histogram.constData() + channel * NodeGraph::kHistogramBuckets;
        QVector<quint32> sorted(NodeGraph::kHistogramBuckets);
        std::copy(buckets, buckets + NodeGraph::kHistogramBuckets, sorted.begin());
        std::sort(sorted.begin(), sorted.end());
        const double scale = std::max<double>(1.0, sorted[int(NodeGraph::kHistogramBuckets * 0.98)]);
        painter.setBrush(QColor(255, 255, 255, 26));
        const double step = plot.width() / double(NodeGraph::kHistogramBuckets);
        for (int i = 0; i < NodeGraph::kHistogramBuckets; ++i) {
            const double h = std::sqrt(std::min(1.0, buckets[i] / scale)) * plot.height();
            if (h <= 0.0) continue;
            painter.drawRect(QRectF(plot.left() + i * step, plot.bottom() - h, std::max(step, 1.0), h));
        }
    }

    painter.setPen(QPen(QColor(255, 255, 255, 26), 0.8));
    for (int i = 1; i < 4; ++i) {
        const double fx = plot.left() + plot.width() * i / 4.0;
        const double fy = plot.top() + plot.height() * i / 4.0;
        painter.drawLine(QPointF(fx, plot.top()), QPointF(fx, plot.bottom()));
        painter.drawLine(QPointF(plot.left(), fy), QPointF(plot.right(), fy));
    }

    const QVector<QPointF> points =
        NodeGraph::curvePoints(node.params.value(spec.curve.points).toString());
    const auto toPlot = [&](double x, double y) {
        return QPointF(plot.left() + x * plot.width(), plot.bottom() - y * plot.height());
    };

    // The same evaluateCurve the node applies, so what is drawn is what runs.
    QPainterPath path;
    const int samples = std::max(24, int(plot.width()));
    for (int i = 0; i <= samples; ++i) {
        const double x = double(i) / samples;
        const QPointF p = toPlot(x, NodeGraph::evaluateCurve(points, x));
        if (i == 0) path.moveTo(p); else path.lineTo(p);
    }
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(226, 230, 236), 1.4));
    painter.drawPath(path);

    painter.setPen(QPen(QColor(18, 20, 24), 1));
    for (const QPointF& point : points) {
        painter.setBrush(QColor(238, 240, 244));
        painter.drawEllipse(toPlot(point.x(), point.y()), 3.0, 3.0);
    }
}

int NodeCanvas::curvePointAt(const QPointF& graphPoint, QString* nodeId, QRectF* plotOut) const
{
    for (int i = m_graph.nodes.size() - 1; i >= 0; --i) {
        const NodeGraph::Node& node = m_graph.nodes[i];
        const NodeGraph::NodeTypeSpec* spec = specFor(node);
        if (!spec || !spec->curve.valid()) continue;
        const NodeLayout layout = layoutFor(node);
        if (layout.curve.isNull() || !layout.curve.contains(graphPoint)) continue;
        const QRectF plot = layout.curve.adjusted(4, 3, -4, -3);
        const QVector<QPointF> points =
            NodeGraph::curvePoints(node.params.value(spec->curve.points).toString());
        int best = -1;
        double bestDistance = 9.0;
        for (int p = 0; p < points.size(); ++p) {
            const QPointF at(plot.left() + points[p].x() * plot.width(),
                plot.bottom() - points[p].y() * plot.height());
            const double distance = QLineF(graphPoint, at).length();
            if (distance < bestDistance) { bestDistance = distance; best = p; }
        }
        if (nodeId) *nodeId = node.id;
        if (plotOut) *plotOut = plot;
        return best;
    }
    return -1;
}

NodeCanvas::HandleHit NodeCanvas::handleAt(const QPointF& point) const
{
    for (int i = m_graph.nodes.size() - 1; i >= 0; --i) {
        const NodeGraph::Node& node = m_graph.nodes[i];
        const NodeGraph::NodeTypeSpec* spec = specFor(node);
        if (!spec || !spec->histogram.valid()) continue;
        const NodeLayout layout = layoutFor(node);
        if (layout.histogram.isNull() || !layout.histogram.contains(point)) continue;
        const QRectF plot = histogramPlot(layout.histogram);
        const NodeGraph::HistogramSpec& binding = spec->histogram;

        const auto value = [&](const QString& id, double fallback) {
            const QJsonValue raw = node.params.value(id);
            const NodeGraph::ParamSpec* ps = spec->param(id);
            if (raw.isDouble()) return raw.toDouble();
            return ps && ps->defaultValue.isDouble() ? ps->defaultValue.toDouble() : fallback;
        };
        const double inBlack = value(binding.blackPoint, 0.0);
        const double inWhite = value(binding.whitePoint, 1.0);

        // Which rail was pressed decides which trio is in play, so an input and
        // an output handle at the same x never fight over the press.
        const bool lowerRail = point.y() > plot.bottom();
        QVector<QPair<QString, double>> candidates;
        if (lowerRail) {
            if (!binding.outBlack.isEmpty()) candidates.append({binding.outBlack, value(binding.outBlack, 0.0)});
            if (!binding.outWhite.isEmpty()) candidates.append({binding.outWhite, value(binding.outWhite, 1.0)});
        } else {
            candidates.append({binding.blackPoint, inBlack});
            candidates.append({binding.whitePoint, inWhite});
            if (!binding.gamma.isEmpty())
                candidates.append({binding.gamma,
                    inBlack + (inWhite - inBlack) * gammaToFraction(value(binding.gamma, 1.0))});
        }
        QString best;
        double bestDistance = 12.0; // graph units; generous, these are small marks
        for (const auto& candidate : candidates) {
            const double x = plot.left() + std::clamp(candidate.second, 0.0, 1.0) * plot.width();
            const double distance = std::abs(point.x() - x);
            if (distance < bestDistance) { bestDistance = distance; best = candidate.first; }
        }
        if (!best.isEmpty()) return {node.id, best, plot};
    }
    return {};
}

void NodeCanvas::setAllCollapsed(bool collapsed)
{
    for (NodeGraph::Node& node : m_graph.nodes) node.collapsed = collapsed;
    emit graphEdited();
    update();
}

void NodeCanvas::setNodePreviewChannels(const QString& nodeId, int mask)
{
    NodeGraph::Node* node = m_graph.node(nodeId);
    if (!node || node->previewChannels == mask) return;
    node->previewChannels = mask;
    emit graphEdited(); // a view setting, but it still has to persist and undo
    update();
}

void NodeCanvas::setAllPreviews(bool shown)
{
    for (NodeGraph::Node& node : m_graph.nodes) node.showPreview = shown;
    emit graphEdited();
    update();
}

void NodeCanvas::mouseDoubleClickEvent(QMouseEvent* event)
{
    // Double-clicking a header collapses it: the first thing most people try,
    // and it costs nothing to support alongside the caret.
    const QPointF graphPoint = toGraph(event->position());
    for (int i = m_graph.nodes.size() - 1; i >= 0; --i) {
        NodeGraph::Node& node = m_graph.nodes[i];
        if (!specFor(node)) continue;
        if (!layoutFor(node).header.contains(graphPoint)) continue;
        node.collapsed = !node.collapsed;
        emit graphEdited();
        update();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

QVector<QPair<NodeCanvas::HeaderToggle, QRectF>> NodeCanvas::headerToggles(
    const NodeGraph::Node& node, const NodeGraph::NodeTypeSpec& spec, const QRectF& header) const
{
    QVector<QPair<HeaderToggle, QRectF>> toggles;
    constexpr double kGlyph = 13.0;
    double right = header.right() - 5;
    const double top = header.center().y() - kGlyph / 2;
    // Right to left: collapse outermost, then settings, then preview â€” so the
    // one that survives collapsing sits furthest out and never moves.
    toggles.append({HeaderToggle::Collapse, QRectF(right - kGlyph, top, kGlyph, kGlyph)});
    right -= kGlyph + 3;
    if (node.collapsed) return toggles; // the rest have nothing to act on
    if (!spec.params.isEmpty() || spec.histogram.valid()) {
        toggles.append({HeaderToggle::Settings, QRectF(right - kGlyph, top, kGlyph, kGlyph)});
        right -= kGlyph + 3;
    }
    if (NodeGraph::nodeShowsThumbnail(spec))
        toggles.append({HeaderToggle::Preview, QRectF(right - kGlyph, top, kGlyph, kGlyph)});
    return toggles;
}

void NodeCanvas::applyHistogramAction(const QString& nodeId, const QString& action)
{
    NodeGraph::Node* node = m_graph.node(nodeId);
    const NodeGraph::NodeTypeSpec* spec = node ? specFor(*node) : nullptr;
    if (!spec || !spec->histogram.valid()) return;
    const NodeGraph::HistogramSpec& binding = spec->histogram;
    const auto set = [&](const QString& id, double value) {
        if (!id.isEmpty()) node->params.insert(id, value);
    };

    if (action == QLatin1String("reset")) {
        set(binding.blackPoint, 0.0);
        set(binding.whitePoint, 1.0);
        set(binding.gamma, 1.0);
        set(binding.outBlack, 0.0);
        set(binding.outWhite, 1.0);
    } else if (action == QLatin1String("invert")) {
        const auto value = [&](const QString& id, double fallback) {
            const QJsonValue raw = node->params.value(id);
            const NodeGraph::ParamSpec* ps = spec->param(id);
            if (raw.isDouble()) return raw.toDouble();
            return ps && ps->defaultValue.isDouble() ? ps->defaultValue.toDouble() : fallback;
        };
        const double black = value(binding.outBlack, 0.0);
        const double white = value(binding.outWhite, 1.0);
        set(binding.outBlack, white);
        set(binding.outWhite, black);
    } else if (action == QLatin1String("auto")) {
        const NodeGraph::NodeResult& result = m_results.value(nodeId);
        const int expected = NodeGraph::kHistogramChannels * NodeGraph::kHistogramBuckets;
        if (result.histogram.size() != expected) return;
        int channel = 4;
        if (!binding.channel.isEmpty()) {
            const QJsonValue raw = node->params.value(binding.channel);
            if (raw.isDouble()) channel = raw.toInt();
        }
        channel = std::clamp(channel, 0, NodeGraph::kHistogramChannels - 1);
        const quint32* buckets = result.histogram.constData() + channel * NodeGraph::kHistogramBuckets;
        quint64 total = 0;
        for (int i = 0; i < NodeGraph::kHistogramBuckets; ++i) total += buckets[i];
        if (total == 0) return;
        // Percentile clip rather than absolute min/max: one stray texel should
        // not decide where the black point goes.
        const quint64 tail = quint64(double(total) * 0.001);
        quint64 seen = 0;
        int low = 0, high = NodeGraph::kHistogramBuckets - 1;
        for (int i = 0; i < NodeGraph::kHistogramBuckets; ++i) {
            seen += buckets[i];
            if (seen > tail) { low = i; break; }
        }
        seen = 0;
        for (int i = NodeGraph::kHistogramBuckets - 1; i >= 0; --i) {
            seen += buckets[i];
            if (seen > tail) { high = i; break; }
        }
        if (high <= low) return;
        set(binding.blackPoint, low / 255.0);
        set(binding.whitePoint, high / 255.0);
    } else {
        return;
    }
    emit nodeParamsChanged(nodeId);
    emit graphEdited();
    update();
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

QRectF NodeCanvas::graphBounds() const
{
    // Every node laid out in full, so this is not free — and the minimap's rect
    // and its contents each asked separately, twice per repaint for the same
    // answer. Cached for the DURATION OF ONE PAINT only: paintEvent drops it on
    // entry, so no mutation can leave it stale. Keying it on something like the
    // node count instead would look safer and quietly go wrong the moment a
    // node was dragged, which changes positions and not the count.
    if (m_boundsValid) return m_bounds;
    QRectF bounds;
    for (const NodeGraph::Node& node : m_graph.nodes) {
        const QRectF rect = nodeRect(node);
        bounds = bounds.isNull() ? rect : bounds.united(rect);
    }
    m_bounds = bounds;
    m_boundsValid = true;
    return bounds;
}

void NodeCanvas::frameAll()
{
    m_framed = true;
    if (m_graph.nodes.isEmpty()) { m_pan = QPointF(); m_zoom = 1.0; update(); return; }
    QRectF bounds = graphBounds();
    bounds.adjust(-40, -40, 40, 40);
    const double zoomX = width() / std::max(bounds.width(), 1.0);
    const double zoomY = height() / std::max(bounds.height(), 1.0);
    m_zoom = std::clamp(std::min(zoomX, zoomY), 0.15, 1.4);
    m_pan = QPointF(width() * 0.5, height() * 0.5) - bounds.center() * m_zoom;
    update();
}

void NodeCanvas::frameNode(const QString& nodeId)
{
    const NodeGraph::Node* node = m_graph.node(nodeId);
    if (!node) return;
    m_framed = true;
    const QRectF rect = nodeRect(*node);
    // Zoom to fit the node with room around it, but never below the detail
    // threshold: framing a node to then not be able to read it is no help.
    const double fitX = width() / std::max(rect.width() + 160.0, 1.0);
    const double fitY = height() / std::max(rect.height() + 120.0, 1.0);
    m_zoom = std::clamp(std::min(fitX, fitY), 0.6, 1.6);
    m_pan = QPointF(width() * 0.5, height() * 0.5) - rect.center() * m_zoom;
    update();
}

QVector<QPointF> NodeCanvas::connectionCurve(const QPointF& from, const QPointF& to,
    const QVector<QPointF>& waypoints) const
{
    // One horizontal-tangent cubic per hop, laid out flat: p0 c1 c2 p1 c3 c4 p2…
    // With no waypoints that is the single bezier this has always drawn. With
    // them the wire runs through the lanes the layout reserved, so a long wire
    // travels between the nodes it passes instead of straight over them — and
    // the horizontal tangents keep it reading as a wire, not a polyline.
    QVector<QPointF> points{from};
    QPointF previous = from;
    QVector<QPointF> stops = waypoints;
    stops.append(to);
    for (const QPointF& stop : stops) {
        const double slack = std::clamp(std::abs(stop.x() - previous.x()) * 0.5, 24.0, 120.0);
        points.append(previous + QPointF(slack, 0));
        points.append(stop - QPointF(slack, 0));
        points.append(stop);
        previous = stop;
    }
    return points;
}

namespace {

// Walks a multi-segment cubic path. Every consumer goes through this, so the
// drawn wire and the knife's idea of it cannot diverge.
QPointF bezierAt(const QVector<QPointF>& curve, double t)
{
    const int segments = int(curve.size() - 1) / 3;
    if (segments < 1) return curve.isEmpty() ? QPointF() : curve.first();
    const double scaled = std::clamp(t, 0.0, 1.0) * segments;
    const int segment = std::min(int(scaled), segments - 1);
    const double local = scaled - segment;
    const QPointF* p = curve.constData() + segment * 3;
    const double u = 1.0 - local;
    return p[0] * (u * u * u) + p[1] * (3 * u * u * local)
         + p[2] * (3 * u * local * local) + p[3] * (local * local * local);
}

// Proper straddle test on both sides. Collinear touching does not count as a
// crossing, which is what you want for a knife: brushing a wire lengthways
// should not cut it.
bool segmentsCross(const QPointF& a1, const QPointF& a2, const QPointF& b1, const QPointF& b2)
{
    const auto side = [](const QPointF& o, const QPointF& p, const QPointF& q) {
        return (p.x() - o.x()) * (q.y() - o.y()) - (p.y() - o.y()) * (q.x() - o.x());
    };
    const double d1 = side(a1, a2, b1), d2 = side(a1, a2, b2);
    const double d3 = side(b1, b2, a1), d4 = side(b1, b2, a2);
    return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
}

} // namespace

QVector<int> NodeCanvas::connectionsCrossing(const QVector<QPointF>& stroke) const
{
    QVector<int> hits;
    if (stroke.size() < 2) return hits;
    // Enough segments that the flattened wire tracks the drawn curve to well
    // under a pixel at any zoom the canvas allows. Per hop, so a routed wire
    // through five lanes is sampled as finely as a direct one.
    constexpr int kSamplesPerHop = 32;
    for (int index = 0; index < m_graph.connections.size(); ++index) {
        const NodeGraph::Connection& connection = m_graph.connections.at(index);
        const NodeGraph::Node* from = m_graph.node(connection.fromNode);
        const NodeGraph::Node* to = m_graph.node(connection.toNode);
        if (!from || !to) continue;
        const QVector<QPointF> curve = connectionCurve(
            portCenter(*from, connection.fromPort, true),
            portCenter(*to, connection.toPort, false), connection.waypoints);
        const int kSamples = kSamplesPerHop * std::max(1, int(curve.size() - 1) / 3);
        QPointF previous = curve.first();
        bool cut = false;
        for (int i = 1; i <= kSamples && !cut; ++i) {
            const QPointF current = bezierAt(curve, double(i) / kSamples);
            for (int s = 1; s < stroke.size() && !cut; ++s)
                if (segmentsCross(previous, current, stroke.at(s - 1), stroke.at(s))) cut = true;
            previous = current;
        }
        if (cut) hits.append(index);
    }
    return hits;
}

void NodeCanvas::setKnifeMode(bool on)
{
    if (m_knifeMode == on) return;
    m_knifeMode = on;
    m_knifeCutting = false;
    m_knifeStroke.clear();
    m_knifeDoomed.clear();
    if (on) setCursor(Qt::CrossCursor);
    else unsetCursor();
    update();
}

void NodeCanvas::drawConnection(QPainter& painter, const QPointF& from, const QPointF& to,
    const QColor& color, bool active, const QVector<QPointF>& waypoints)
{
    const QVector<QPointF> curve = connectionCurve(from, to, waypoints);
    QPainterPath path(curve.at(0));
    for (int i = 1; i + 2 < curve.size(); i += 3)
        path.cubicTo(curve.at(i), curve.at(i + 1), curve.at(i + 2));
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(color, active ? 2.6 : 1.8, Qt::SolidLine, Qt::RoundCap));
    painter.drawPath(path);
}

void NodeCanvas::drawParamRow(QPainter& painter, const NodeGraph::Node& node,
    const NodeGraph::ParamSpec& spec, const QRectF& row)
{
    using Kind = NodeGraph::ParamSpec::Kind;
    const QJsonValue raw = node.params.value(spec.id);
    const QJsonValue value = raw.isUndefined() ? spec.defaultValue : raw;

    QFont font = painter.font();
    font.setPixelSize(9);
    font.setBold(false);
    painter.setFont(font);

    // A bool is a labelled checkbox; everything else is a filled widget box
    // with the name on the left and the value on the right, as in Blender.
    if (spec.kind == Kind::Bool) {
        const bool on = value.toBool();
        const QRectF box(row.left() + 1, row.center().y() - 5, 10, 10);
        painter.setPen(QPen(QColor(104, 109, 117), 1));
        painter.setBrush(on ? QColor(232, 176, 74) : QColor(35, 37, 41));
        painter.drawRoundedRect(box, 2, 2);
        if (on) {
            painter.setPen(QPen(QColor(32, 34, 37), 1.6));
            painter.drawPolyline(QPolygonF({{box.left() + 2.2, box.center().y()},
                {box.center().x() - 0.4, box.bottom() - 2.4}, {box.right() - 2.0, box.top() + 2.4}}));
        }
        painter.setPen(QColor(200, 205, 212));
        const QRectF text = row.adjusted(16, 0, 0, 0);
        painter.drawText(text, Qt::AlignVCenter | Qt::AlignLeft,
            painter.fontMetrics().elidedText(spec.name, Qt::ElideRight, int(text.width())));
        return;
    }

    QPainterPath box;
    box.addRoundedRect(row, 3, 3);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(53, 57, 63));
    painter.drawPath(box);

    // Bounded numbers get Blender's fill bar so the value reads at a glance.
    const auto drawFillBar = [&](double v) {
        if (spec.maximum <= spec.minimum) return;
        const double t = std::clamp((v - spec.minimum) / (spec.maximum - spec.minimum), 0.0, 1.0);
        if (t <= 0.001) return;
        painter.save();
        painter.setClipPath(box);
        painter.setBrush(QColor(76, 96, 122));
        painter.drawRect(QRectF(row.left(), row.top(), row.width() * t, row.height()));
        painter.restore();
    };

    QString shown;
    switch (spec.kind) {
    case Kind::Float: {
        const double v = value.toDouble();
        drawFillBar(v);
        shown = QString::number(v, 'f', spec.step >= 1 ? 0 : (spec.step >= 0.1 ? 2 : 3));
        break;
    }
    case Kind::Int:
        drawFillBar(value.toInt());
        shown = QString::number(value.toInt());
        break;
    case Kind::Enum:
        shown = spec.enumLabels.value(value.toInt(), QStringLiteral("â€”"));
        break;
    case Kind::Color: {
        const QColor swatch(value.toString());
        const QRectF chip(row.right() - 26, row.top() + 2, 24, row.height() - 4);
        painter.setBrush(swatch.isValid() ? swatch : QColor(20, 20, 20));
        painter.setPen(QPen(QColor(28, 30, 33), 1));
        painter.drawRoundedRect(chip, 2, 2);
        break;
    }
    case Kind::Path: {
        const QString path = value.toString();
        shown = path.isEmpty() ? tr("(none)") : path.section(QLatin1Char('/'), -1).section(QLatin1Char('\\'), -1);
        break;
    }
    case Kind::Text:
        shown = value.toString();
        break;
    case Kind::Bool:
        break; // handled above
    }

    const QRectF text = row.adjusted(5, 0, -5, 0);
    painter.setPen(QColor(158, 164, 172));
    const int nameWidth = int(text.width() * 0.46);
    painter.drawText(QRectF(text.left(), text.top(), nameWidth, text.height()),
        Qt::AlignVCenter | Qt::AlignLeft,
        painter.fontMetrics().elidedText(spec.name, Qt::ElideRight, nameWidth));
    if (!shown.isEmpty()) {
        painter.setPen(QColor(226, 230, 236));
        const QRectF valueRect(text.left() + nameWidth + 4, text.top(),
            text.width() - nameWidth - 4, text.height());
        painter.drawText(valueRect, Qt::AlignVCenter | Qt::AlignRight,
            painter.fontMetrics().elidedText(shown, Qt::ElideLeft, int(valueRect.width())));
    }
}

void NodeCanvas::drawNode(QPainter& painter, const NodeGraph::Node& node)
{
    const NodeGraph::NodeTypeSpec* spec = specFor(node);
    if (!spec) return;
    const NodeGraph::NodeResult result = m_results.value(node.id);
    const NodeLayout layout = layoutFor(node);
    const QRectF rect = layout.rect;
    // Nothing off-screen is worth drawing, and more to the point nothing
    // off-screen is worth MEASURING: the body below lays out rows, masks a
    // thumbnail and formats text before QPainter would clip any of it. Panning
    // a large graph paid for every node whether it was in view or not.
    if (!QRectF(toWidget(rect.topLeft()), toWidget(rect.bottomRight()))
             .intersects(QRectF(this->rect()).adjusted(-64, -64, 64, 64))) return;
    const bool selected = node.id == m_selected;
    const bool failed = !result.error.isEmpty();

    QPainterPath body;
    body.addRoundedRect(rect, 6, 6);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(44, 47, 52));
    painter.drawPath(body);

    QPainterPath headerPath;
    headerPath.addRoundedRect(layout.header, 6, 6);
    // Category tint, from whatever palette the node library registered. A
    // failure overrides it: the red header is the one signal that must never be
    // out-competed by decoration.
    const QColor category = m_registry.categoryColor(spec->category);
    painter.setBrush(failed ? QColor(120, 52, 52)
                            : (category.isValid() ? category : QColor(62, 66, 73)));
    painter.drawPath(headerPath);
    painter.drawRect(QRectF(layout.header.left(), layout.header.bottom() - 6, layout.header.width(), 6));

    // Header toggles first, so the title can be elided against what is left.
    const QVector<QPair<HeaderToggle, QRectF>> toggles =
        detailVisible() ? headerToggles(node, *spec, layout.header)
                        : QVector<QPair<HeaderToggle, QRectF>>();
    double titleRight = 8;
    for (const auto& toggle : toggles) {
        const QRectF& box = toggle.second;
        titleRight = std::max(titleRight, layout.header.right() - box.left() + 4);
        const bool on = toggle.first == HeaderToggle::Preview  ? node.showPreview
                      : toggle.first == HeaderToggle::Settings ? node.showSettings
                                                               : !node.collapsed;
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 255, 255, on ? 34 : 12));
        painter.drawRoundedRect(box, 3, 3);
        painter.setPen(QPen(QColor(on ? 214 : 130, on ? 219 : 134, on ? 226 : 140), 1.2));
        painter.setBrush(Qt::NoBrush);
        const QRectF g = box.adjusted(3.5, 3.5, -3.5, -3.5);
        switch (toggle.first) {
        case HeaderToggle::Preview: // a picture frame
            painter.drawRect(g);
            painter.drawLine(QPointF(g.left(), g.bottom() - 1.5),
                QPointF(g.center().x(), g.center().y()));
            break;
        case HeaderToggle::Settings: // stacked slider rows
            for (int i = 0; i < 3; ++i) {
                const double y = g.top() + g.height() * (0.15 + 0.35 * i);
                painter.drawLine(QPointF(g.left(), y), QPointF(g.right(), y));
            }
            break;
        case HeaderToggle::Collapse: // a disclosure triangle
            QPolygonF caret;
            if (node.collapsed) caret << QPointF(g.left(), g.top()) << QPointF(g.right(), g.center().y())
                                      << QPointF(g.left(), g.bottom());
            else caret << QPointF(g.left(), g.top()) << QPointF(g.right(), g.top())
                       << QPointF(g.center().x(), g.bottom());
            painter.setBrush(QColor(on ? 214 : 130, on ? 219 : 134, on ? 226 : 140));
            painter.drawPolygon(caret);
            break;
        }
    }

    // At overview zoom the title is drawn by drawNodeOverlay instead, at a
    // fixed screen size: scaled with the graph it shrinks to an illegible
    // smear long before the node itself stops being a useful landmark.
    if (detailVisible()) {
        painter.setPen(QColor(226, 229, 233));
        QFont titleFont = painter.font();
        titleFont.setPixelSize(11);
        titleFont.setBold(true);
        painter.setFont(titleFont);
        painter.drawText(layout.header.adjusted(8, 0, -titleRight, 0), Qt::AlignVCenter | Qt::AlignLeft,
            painter.fontMetrics().elidedText(node.label.isEmpty() ? spec->name : node.label,
                Qt::ElideRight, int(layout.header.width() - 8 - titleRight)));
    }

    // Live preview, pinned to the top of the body.
    const QString preview = previewPort(node);
    if (!layout.thumb.isNull() && thumbnailsVisible()) {
        painter.setBrush(QColor(28, 30, 33));
        painter.setPen(Qt::NoPen);
        painter.drawRect(layout.thumb);
        // Masking allocates a converted copy and walks every texel, and the
        // answer is identical frame to frame — but it never short-circuits,
        // because the default RGB is not RGBA and the pass exists to force
        // alpha opaque. Twenty-two nodes of that per repaint is most of the
        // cost of drawing the graph, so it is cached against everything that
        // can change it and cleared when results or the graph do.
        const QString thumbKey = node.id % QLatin1Char('\x1f') % preview
            % QLatin1Char('\x1f') % QString::number(node.previewChannels);
        auto cached = m_thumbCache.constFind(thumbKey);
        if (cached == m_thumbCache.constEnd()) {
            cached = m_thumbCache.insert(thumbKey, NodeGraph::applyChannelMask(
                result.thumbnails.value(preview), node.previewChannels));
        }
        const QImage& thumb = cached.value();
        if (!thumb.isNull()) {
            const QSizeF scaled = QSizeF(thumb.size()).scaled(layout.thumb.size(), Qt::KeepAspectRatio);
            QRectF target(QPointF(), scaled);
            target.moveCenter(layout.thumb.center());
            painter.drawImage(target, thumb);
        } else {
            painter.setPen(QColor(120, 124, 130));
            QFont small = painter.font();
            small.setPixelSize(10);
            small.setBold(false);
            painter.setFont(small);
            painter.drawText(layout.thumb, Qt::AlignCenter,
                failed ? tr("error") : (result.outputs.isEmpty() ? tr("not evaluated") : tr("no image")));
        }
    }

    // R / G / B / A view toggles, sitting with the thumbnail they filter.
    if (!layout.channelStrip.isNull() && detailVisible()) {
        static const char* const names[4] = {"R", "G", "B", "A"};
        static const QColor inks[4] = {QColor(214, 106, 106), QColor(122, 196, 122),
            QColor(110, 150, 226), QColor(198, 198, 204)};
        QFont chipFont = painter.font();
        chipFont.setPixelSize(9);
        chipFont.setBold(true);
        painter.setFont(chipFont);
        const double chipWidth = layout.channelStrip.width() / 4.0;
        for (int c = 0; c < 4; ++c) {
            const QRectF chip(layout.channelStrip.left() + c * chipWidth + 1,
                layout.channelStrip.top() + 1, chipWidth - 2,
                layout.channelStrip.height() - 3);
            const bool on = node.previewChannels & (1 << c);
            painter.setPen(Qt::NoPen);
            painter.setBrush(on ? QColor(inks[c].red(), inks[c].green(), inks[c].blue(), 70)
                                : QColor(255, 255, 255, 12));
            painter.drawRoundedRect(chip, 2, 2);
            painter.setPen(on ? inks[c] : QColor(112, 116, 124));
            painter.drawText(chip, Qt::AlignCenter, QLatin1String(names[c]));
        }
    }

    QFont portFont = painter.font();
    portFont.setPixelSize(9);
    portFont.setBold(false);
    painter.setFont(portFont);
    const auto drawPorts = [&](const QVector<NodeGraph::PortSpec>& ports,
                               const QVector<QRectF>& rows, bool isOutput) {
        for (int i = 0; i < ports.size() && i < rows.size(); ++i) {
            const NodeGraph::PortSpec& port = ports[i];
            if (rows[i].isNull()) continue; // hidden port
            const QPointF center(isOutput ? rect.right() : rect.left(), rows[i].center().y());
            const bool connected = std::any_of(m_graph.connections.begin(), m_graph.connections.end(),
                [&](const NodeGraph::Connection& c) {
                    return isOutput ? (c.fromNode == node.id && c.fromPort == port.id)
                                    : (c.toNode == node.id && c.toPort == port.id);
                });
            // An output that evaluated to nothing â€” an unauthored texture slot,
            // say â€” is drawn faded so a material's real slots stand out.
            bool empty = false;
            if (isOutput && !result.outputs.isEmpty()) {
                const NodeGraph::Value value = result.outputs.value(port.id);
                empty = !value.isValid()
                    || (value.type() == NodeGraph::ValueType::String && value.string().trimmed().isEmpty())
                    || (value.type() == NodeGraph::ValueType::Image && value.image().isNull());
            }
            // Gold on the selected node, and on whatever sits at the FAR end of
            // its wires: a gold wire landing on a blue dot reads as two
            // different things meeting rather than one connection. The rule
            // matches the wires' — gold if it touches the selection.
            const bool touchesSelection = !m_selected.isEmpty()
                && std::any_of(m_graph.connections.begin(), m_graph.connections.end(),
                    [&](const NodeGraph::Connection& c) {
                        return isOutput ? (c.fromNode == node.id && c.fromPort == port.id
                                           && c.toNode == m_selected)
                                        : (c.toNode == node.id && c.toPort == port.id
                                           && c.fromNode == m_selected);
                    });
            const QColor typeColor = empty ? colorForType(port.type).darker(220) : colorForType(port.type);
            const QColor color = (selected || touchesSelection) ? QColor(240, 196, 80) : typeColor;
            painter.setPen(QPen(color.darker(160), 1));
            painter.setBrush(connected ? color : QColor(44, 47, 52));
            painter.drawEllipse(center, kPortRadius, kPortRadius);
            // A collapsed node gathers every port onto the header, so port
            // names would pile up on top of the title. Blender drops them too.
            if (node.collapsed) continue;
            painter.setPen(empty ? QColor(104, 108, 114) : QColor(168, 173, 180));
            const double inset = kPortRadius * 2 + 2;
            const QRectF label = isOutput
                ? QRectF(rect.left() + kPadding, rows[i].top(), rect.width() - kPadding - inset, rows[i].height())
                : QRectF(rect.left() + inset, rows[i].top(), rect.width() - kPadding - inset, rows[i].height());
            painter.drawText(label, (isOutput ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
                painter.fontMetrics().elidedText(port.name, Qt::ElideRight, int(label.width())));
        }
    };
    // Border first, ports second: a port dot straddles the node edge, and
    // stroking the outline afterwards sliced every one of them in half.
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(selected ? QColor(240, 196, 80) : QColor(24, 26, 29), selected ? 2.0 : 1.0));
    painter.drawPath(body);

    drawPorts(spec->outputs, layout.outputRows, true);
    if (!layout.histogram.isNull() && detailVisible())
        drawHistogram(painter, node, *spec, layout.histogram);
    if (!layout.curve.isNull() && detailVisible())
        drawCurve(painter, node, *spec, layout.curve);
    if (detailVisible()) {
        for (int i = 0; i < spec->params.size() && i < layout.paramRows.size(); ++i) {
            if (layout.paramRows[i].isNull()) continue; // settings folded away
            // The curve's points param holds JSON, which the editor above
            // already shows properly. Printing it as text is pure noise.
            if (spec->curve.valid() && spec->params[i].id == spec->curve.points) {
                const int count = NodeGraph::curvePoints(
                    node.params.value(spec->curve.points).toString()).size();
                const QRectF row = layout.paramRows[i].adjusted(5, 0, -5, 0);
                painter.setPen(QColor(158, 164, 172));
                painter.drawText(row, Qt::AlignVCenter | Qt::AlignLeft, spec->params[i].name);
                painter.setPen(QColor(226, 230, 236));
                // Spelled out rather than %n: with no translator loaded Qt shows
                // the source string verbatim, so "%n point(s)" renders the "(s)".
                painter.drawText(row, Qt::AlignVCenter | Qt::AlignRight,
                    count == 1 ? tr("1 point") : tr("%1 points").arg(count));
                continue;
            }
            drawParamRow(painter, node, spec->params[i], layout.paramRows[i]);
        }
    }
    drawPorts(spec->inputs, layout.inputRows, false);

    // Status line: resolution and compression for texture nodes. It turns red
    // when the evaluator flagged the image as under-resolution.
    QFont statusFont = painter.font();
    statusFont.setPixelSize(9);
    painter.setFont(statusFont);
    const QString status = failed ? result.error
        : (!result.status.isEmpty() ? result.status
                                    : (result.notes.isEmpty() ? QString() : result.notes.first()));
    if (!status.isEmpty()) {
        painter.setPen(failed ? QColor(235, 150, 150)
                              : (result.warning.isEmpty() ? QColor(150, 155, 162) : QColor(236, 116, 108)));
        painter.drawText(layout.statusRow, Qt::AlignLeft | Qt::AlignVCenter,
            painter.fontMetrics().elidedText(status, Qt::ElideRight, int(layout.statusRow.width())));
    }
    if (!result.warning.isEmpty() && !failed) {
        painter.setPen(QColor(232, 176, 74));
        painter.drawText(layout.warningRow, Qt::AlignLeft | Qt::AlignVCenter,
            painter.fontMetrics().elidedText(QStringLiteral("âš  ") + result.warning,
                Qt::ElideRight, int(layout.warningRow.width())));
    }

}

QRectF NodeCanvas::minimapRect() const
{
    if (m_graph.nodes.size() < 2) return {};
    const QRectF bounds = graphBounds();
    // The widget's own floor is 320x240, so a fixed pixel threshold below that
    // would never fire. This one is about the pane being squashed: under this
    // the map is clutter competing with the graph it is supposed to serve.
    if (bounds.isNull() || width() < 420 || height() < 300) return {};
    // Aspect-matched to the graph so the map is not a distorted version of
    // what is on screen, then capped both absolutely and as a share of the
    // canvas so a narrow pane gets a proportionally smaller map.
    const double maxW = std::min(190.0, width() * 0.22);
    const double maxH = std::min(130.0, height() * 0.22);
    const double scale = std::min(maxW / std::max(bounds.width(), 1.0),
        maxH / std::max(bounds.height(), 1.0));
    const QSizeF size(std::max(bounds.width() * scale, 40.0), std::max(bounds.height() * scale, 30.0));
    return QRectF(width() - size.width() - 12.0, height() - size.height() - 12.0,
        size.width(), size.height());
}

void NodeCanvas::drawMinimap(QPainter& painter) const
{
    const QRectF map = minimapRect();
    if (map.isNull()) return;
    const QRectF bounds = graphBounds();
    const double scale = std::min(map.width() / std::max(bounds.width(), 1.0),
        map.height() / std::max(bounds.height(), 1.0));
    const auto toMap = [&](const QPointF& graphPoint) {
        return map.topLeft() + (graphPoint - bounds.topLeft()) * scale;
    };

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(24, 26, 29, 205));
    painter.drawRoundedRect(map.adjusted(-4, -4, 4, 4), 4, 4);

    for (const NodeGraph::Node& node : m_graph.nodes) {
        const NodeGraph::NodeTypeSpec* spec = specFor(node);
        const QColor category = spec ? m_registry.categoryColor(spec->category) : QColor();
        // Once, not twice: nodeRect lays the whole node out to answer, and this
        // was asking for the same answer for the corner and for the size.
        const QRectF where = nodeRect(node);
        const QRectF box(toMap(where.topLeft()), where.size() * scale);
        const bool selected = node.id == m_selected;
        painter.setBrush(selected ? QColor(240, 196, 80)
                                  : (category.isValid() ? category.lighter(115) : QColor(96, 101, 109)));
        painter.drawRect(box.adjusted(0, 0, box.width() < 2 ? 2 : 0, box.height() < 2 ? 2 : 0));
    }

    // What is on screen right now. Clipped to the map, or a zoomed-out view
    // draws a rect larger than the thing it is supposed to be inside.
    QRectF viewport(toMap(toGraph(QPointF(0, 0))), toMap(toGraph(QPointF(width(), height()))));
    viewport = viewport.intersected(map);
    if (!viewport.isEmpty()) {
        painter.setBrush(QColor(255, 255, 255, 22));
        painter.setPen(QPen(QColor(226, 229, 233, 190), 1.0));
        painter.drawRect(viewport);
    }
}

bool NodeCanvas::panFromMinimap(const QPointF& widgetPoint)
{
    const QRectF map = minimapRect();
    if (map.isNull() || !map.adjusted(-4, -4, 4, 4).contains(widgetPoint)) return false;
    const QRectF bounds = graphBounds();
    const double scale = std::min(map.width() / std::max(bounds.width(), 1.0),
        map.height() / std::max(bounds.height(), 1.0));
    const QPointF target = bounds.topLeft() + (widgetPoint - map.topLeft()) / scale;
    m_pan = QPointF(width() * 0.5, height() * 0.5) - target * m_zoom;
    update();
    return true;
}

void NodeCanvas::drawNodeOverlay(QPainter& painter)
{
    QFont font = painter.font();
    font.setPixelSize(11);
    font.setBold(true);
    painter.setFont(font);
    const QFontMetricsF metrics(painter.font());
    const bool needLabels = !detailVisible();
    // Doubles as the record --canvas-nav-check reads back: the clamp invariant
    // ("no label leaves the canvas") is not visible in a screenshot at the one
    // zoom a capture happens to use.
    QVector<QRectF>& placed = m_overlayLabels; // greedy rejection, no stacking
    placed.clear();
    const QRectF canvas(rect());
    // A label centred on a node at the edge of the view slides off it and gets
    // sliced by the widget border — and the nodes most likely to be at an edge
    // are the input and the terminal, the two you most need named.
    const auto clampIntoCanvas = [&canvas](QRectF label) {
        if (label.right() > canvas.right() - 3) label.moveRight(canvas.right() - 3);
        if (label.left() < canvas.left() + 3) label.moveLeft(canvas.left() + 3);
        if (label.bottom() > canvas.bottom() - 3) label.moveBottom(canvas.bottom() - 3);
        if (label.top() < canvas.top() + 3) label.moveTop(canvas.top() + 3);
        return label;
    };
    const auto drawLabel = [&](const QRectF& label, const QColor& ink, const QString& text) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(24, 26, 29, 190));
        painter.drawRoundedRect(label, 3, 3);
        painter.setPen(ink);
        painter.drawText(label, Qt::AlignCenter, text);
    };

    // Container titles first, and they get the same treatment as nodes: giving
    // nodes readable names at overview zoom while leaving frames as a smear
    // reads worse than leaving both illegible, because it looks deliberate.
    if (needLabels) {
        for (const NodeGraph::Frame& frame : m_graph.frames) {
            if (frame.title.isEmpty()) continue;
            const QRectF screen(toWidget(frame.rect.topLeft()), toWidget(frame.rect.bottomRight()));
            if (!screen.intersects(canvas)) continue;
            QRectF label(0, 0, metrics.horizontalAdvance(frame.title) + 10.0, metrics.height() + 3.0);
            label.moveCenter(QPointF(screen.center().x(), screen.top() + label.height() * 0.5 + 2.0));
            label = clampIntoCanvas(label);
            placed.append(label); // node labels route around it
            drawLabel(label, frame.color.lighter(150), frame.title);
        }
    }

    for (const NodeGraph::Node& node : m_graph.nodes) {
        const NodeGraph::NodeTypeSpec* spec = specFor(node);
        if (!spec) continue;
        const QRectF rect = nodeRect(node);
        const QRectF screen(toWidget(rect.topLeft()), toWidget(rect.bottomRight()));
        if (!screen.intersects(QRectF(this->rect()))) continue;

        // Search hits ring in place, so a query with several matches shows all
        // of them at once instead of only wherever Enter last landed.
        if (m_searchMatches.contains(node.id)) {
            const bool current = m_searchIndex >= 0 && m_searchMatches.at(m_searchIndex) == node.id;
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(current ? QColor(255, 232, 120) : QColor(255, 232, 120, 120),
                current ? 2.0 : 1.2));
            painter.drawRoundedRect(screen.adjusted(-2, -2, 2, 2), 5, 5);
        }

        if (needLabels && screen.height() >= 8.0) {
            const QString title = node.label.isEmpty() ? spec->name : node.label;
            // Allowed to overflow the node: at this zoom the box is a marker
            // and the word beside it is the information.
            QRectF label(0, 0, metrics.horizontalAdvance(title) + 10.0, metrics.height() + 3.0);
            label.moveCenter(QPointF(screen.center().x(), screen.top() - label.height() * 0.5 - 1.0));
            label = clampIntoCanvas(label);
            bool clash = false;
            for (const QRectF& other : placed)
                if (other.intersects(label)) { clash = true; break; }
            if (!clash) {
                placed.append(label);
                drawLabel(label, node.id == m_selected ? QColor(240, 196, 80) : QColor(214, 219, 226),
                    title);
            }
        }

        // Status dots. Zoom-independent and outside the body, so a broken node
        // is findable from the overview instead of only from its warning row.
        const NodeGraph::NodeResult result = m_results.value(node.id);
        const bool failed = !result.error.isEmpty();
        if (!failed && result.warning.isEmpty()) continue;
        const QPointF dot(screen.left() + 1.0, screen.top() + 1.0);
        painter.setPen(QPen(QColor(24, 26, 29), 1.5));
        painter.setBrush(failed ? QColor(232, 92, 84) : QColor(232, 176, 74));
        painter.drawEllipse(dot, 4.0, 4.0);
    }
}

QPointF NodeCanvas::minimapGraphPointForTest(const QPointF& widgetPoint) const
{
    const QRectF map = minimapRect();
    if (map.isNull()) return {};
    const QRectF bounds = graphBounds();
    const double scale = std::min(map.width() / std::max(bounds.width(), 1.0),
        map.height() / std::max(bounds.height(), 1.0));
    return bounds.topLeft() + (widgetPoint - map.topLeft()) / scale;
}

bool NodeCanvas::searchVisibleForTest() const
{
    return m_searchEdit && m_searchEdit->isVisible();
}

QWidget* NodeCanvas::searchFieldForTest() const
{
    return m_searchEdit;
}

void NodeCanvas::setSearchTextForTest(const QString& text)
{
    if (!m_searchEdit) beginNodeSearch();
    m_searchEdit->setText(text);
}

void NodeCanvas::beginNodeSearch()
{
    if (!m_searchEdit) {
        m_searchEdit = new QLineEdit(this);
        m_searchEdit->setPlaceholderText(tr("Find node…"));
        m_searchEdit->setClearButtonEnabled(true);
        m_searchEdit->setFixedWidth(190);
        connect(m_searchEdit, &QLineEdit::textChanged, this, [this] { refreshSearchMatches(); });
        connect(m_searchEdit, &QLineEdit::returnPressed, this, [this] { stepSearch(1); });
    }
    m_searchEdit->move(12, 12);
    m_searchEdit->show();
    m_searchEdit->raise();
    m_searchEdit->selectAll();
    m_searchEdit->setFocus();
    refreshSearchMatches();
}

void NodeCanvas::refreshSearchMatches()
{
    m_searchMatches.clear();
    m_searchIndex = -1;
    const QString needle = m_searchEdit ? m_searchEdit->text().trimmed() : QString();
    if (!needle.isEmpty()) {
        for (const NodeGraph::Node& node : m_graph.nodes) {
            const NodeGraph::NodeTypeSpec* spec = specFor(node);
            const QString title = node.label.isEmpty() ? (spec ? spec->name : node.typeId) : node.label;
            if (title.contains(needle, Qt::CaseInsensitive)) m_searchMatches.append(node.id);
        }
    }
    // Typing walks the results as they narrow, rather than making the first
    // match wait for a separate keypress.
    if (!m_searchMatches.isEmpty()) {
        m_searchIndex = 0;
        m_selected = m_searchMatches.first();
        emit nodeSelected(m_selected);
        frameNode(m_selected);
    }
    update();
}

void NodeCanvas::stepSearch(int direction)
{
    if (m_searchMatches.isEmpty()) return;
    m_searchIndex = (m_searchIndex + direction + m_searchMatches.size()) % m_searchMatches.size();
    m_selected = m_searchMatches.at(m_searchIndex);
    emit nodeSelected(m_selected);
    frameNode(m_selected);
}

void NodeCanvas::endNodeSearch()
{
    if (!m_searchEdit) return;
    m_searchEdit->hide();
    m_searchMatches.clear();
    m_searchIndex = -1;
    setFocus();
    update();
}

void NodeCanvas::paintEvent(QPaintEvent*)
{
    // The bounds cache lives for exactly this paint — anything could have moved
    // a node since the last one.
    m_boundsValid = false;

    // First paint is the earliest point where the widget knows its real size;
    // fitting at setGraph time would measure an unlaid-out widget.
    if (!m_framed && !m_graph.nodes.isEmpty() && width() > 50 && height() > 50) frameAll();

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

    // Indexed rather than a range-for: the knife identifies wires by index, and
    // these have to be the same indices connectionsCrossing handed back.
    for (int index = 0; index < m_graph.connections.size(); ++index) {
        const NodeGraph::Connection& connection = m_graph.connections.at(index);
        const NodeGraph::Node* from = m_graph.node(connection.fromNode);
        const NodeGraph::Node* to = m_graph.node(connection.toNode);
        if (!from || !to) continue;
        const NodeGraph::NodeTypeSpec* fromSpec = specFor(*from);
        const NodeGraph::PortSpec* port = fromSpec ? fromSpec->output(connection.fromPort) : nullptr;
        const bool touchesSelection = connection.fromNode == m_selected || connection.toNode == m_selected;
        // A wire under the knife goes red before it is cut, so a stroke can be
        // adjusted while it is being drawn rather than undone afterwards.
        const bool doomed = m_knifeDoomed.contains(index);
        // Gold for every wire touching the selected node. Thickness alone was
        // too weak to trace a route through a crowded graph; colour separates
        // the selection's wires from the rest at a glance.
        drawConnection(painter, portCenter(*from, connection.fromPort, true),
            portCenter(*to, connection.toPort, false),
            doomed ? QColor(232, 92, 84)
                   : (touchesSelection ? QColor(240, 196, 80)
                                       : (port ? colorForType(port->type) : QColor(150, 150, 150))),
            touchesSelection || doomed, connection.waypoints);
    }

    if (m_wireFrom.valid()) {
        const NodeGraph::Node* from = m_graph.node(m_wireFrom.nodeId);
        if (from)
            drawConnection(painter, portCenter(*from, m_wireFrom.portId, m_wireFrom.isOutput),
                m_wireTo, QColor(240, 196, 80), true);
    }

    // Containers sit behind everything so their nodes read as being inside.
    for (const NodeGraph::Frame& item : m_graph.frames) {
        QPainterPath body;
        body.addRoundedRect(item.rect, 8, 8);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(item.color.red(), item.color.green(), item.color.blue(), 70));
        painter.drawPath(body);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(item.color.lighter(130), m_frameDrag == item.id ? 2.0 : 1.2));
        painter.drawPath(body);

        const QRectF title(item.rect.left(), item.rect.top(), item.rect.width(), kFrameTitleHeight);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(item.color.red(), item.color.green(), item.color.blue(), 165));
        painter.drawRect(title);
        // Same rule as node titles: below the detail threshold the text is
        // drawn by drawNodeOverlay at a fixed screen size instead.
        if (detailVisible()) {
            QFont titleFont = painter.font();
            titleFont.setPixelSize(12);
            titleFont.setBold(true);
            painter.setFont(titleFont);
            painter.setPen(QColor(226, 229, 233));
            painter.drawText(title.adjusted(9, 0, -9, 0), Qt::AlignVCenter | Qt::AlignLeft,
                painter.fontMetrics().elidedText(item.title, Qt::ElideRight, int(title.width() - 18)));
        }
    }

    for (const NodeGraph::Node& node : m_graph.nodes) drawNode(painter, node);

    // The knife stroke goes over the nodes: it is cutting wires that pass
    // behind them, and a stroke that vanished under a node would look like it
    // had missed.
    if (m_knifeStroke.size() > 1) {
        QPainterPath stroke(m_knifeStroke.first());
        for (int i = 1; i < m_knifeStroke.size(); ++i) stroke.lineTo(m_knifeStroke.at(i));
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(20, 20, 24, 170), 4.0 / m_zoom, Qt::SolidLine, Qt::RoundCap));
        painter.drawPath(stroke); // dark backing so the blade reads over any wire
        painter.setPen(QPen(QColor(255, 232, 120), 1.6 / m_zoom, Qt::DashLine, Qt::RoundCap));
        painter.drawPath(stroke);
    }
    painter.restore();

    // Everything from here draws in widget space, at a size the zoom cannot
    // shrink: labels, status dots, search rings and the map.
    drawNodeOverlay(painter);
    drawMinimap(painter);

    // Zoom readout. The two detail thresholds change what a node draws without
    // announcing it; a number makes "why did the settings disappear" answerable
    // instead of mysterious.
    if (!m_graph.nodes.isEmpty()) {
        QFont font = painter.font();
        font.setPixelSize(10);
        font.setBold(false);
        painter.setFont(font);
        painter.setPen(QColor(132, 138, 146));
        painter.drawText(QRectF(12, height() - 20.0, 90, 14), Qt::AlignLeft | Qt::AlignVCenter,
            tr("%1%  ·  F fit").arg(int(std::lround(m_zoom * 100.0))));
    }

    // Modes have to announce themselves; an armed knife that looks like a
    // normal canvas is a trap the first time a drag eats a wire.
    if (m_knifeMode) {
        QFont font = painter.font();
        font.setPixelSize(11);
        painter.setFont(font);
        const QString text = tr("Knife — drag across wires to cut  ·  Esc or K to exit");
        const QRectF label(12, height() - 34.0, painter.fontMetrics().horizontalAdvance(text) + 22.0, 24.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(232, 92, 84, 210));
        painter.drawRoundedRect(label, 4, 4);
        painter.setPen(QColor(255, 246, 244));
        painter.drawText(label, Qt::AlignCenter, text);
    }

    // An empty grid tells a new user nothing; the placeholder tells them what
    // to do to fill it.
    if (m_graph.nodes.isEmpty() && !m_placeholder.isEmpty()) {
        QFont font = painter.font();
        font.setPixelSize(13);
        painter.setFont(font);
        painter.setPen(QColor(132, 138, 146));
        painter.drawText(rect().adjusted(30, 30, -30, -30), Qt::AlignCenter | Qt::TextWordWrap, m_placeholder);
    }
}

void NodeCanvas::setParam(const QString& nodeId, const QString& paramId, const QJsonValue& value)
{
    if (NodeGraph::Node* node = m_graph.node(nodeId)) {
        node->params.insert(paramId, value);
        update();
    }
}

void NodeCanvas::activateParam(const ParamHit& hit, const QPoint& globalPos)
{
    using Kind = NodeGraph::ParamSpec::Kind;
    const NodeGraph::Node* node = m_graph.node(hit.nodeId);
    const NodeGraph::NodeTypeSpec* spec = node ? specFor(*node) : nullptr;
    const NodeGraph::ParamSpec* param = spec ? spec->param(hit.paramId) : nullptr;
    if (!param) return;
    const QJsonValue raw = node->params.value(hit.paramId);
    const QJsonValue current = raw.isUndefined() ? param->defaultValue : raw;

    switch (param->kind) {
    case Kind::Bool:
        setParam(hit.nodeId, hit.paramId, QJsonValue(!current.toBool()));
        break;
    case Kind::Enum: {
        QMenu menu(this);
        for (int i = 0; i < param->enumLabels.size(); ++i) {
            QAction* action = menu.addAction(param->enumLabels[i]);
            action->setCheckable(true);
            action->setChecked(i == current.toInt());
            action->setData(i);
        }
        const QAction* chosen = menu.exec(globalPos);
        if (!chosen) return;
        setParam(hit.nodeId, hit.paramId, QJsonValue(chosen->data().toInt()));
        break;
    }
    case Kind::Color: {
        const QColor initial(current.toString());
        const QColor picked = QColorDialog::getColor(initial.isValid() ? initial : QColor(Qt::white),
            this, tr("Select color"));
        if (!picked.isValid()) return;
        setParam(hit.nodeId, hit.paramId, QJsonValue(picked.name()));
        break;
    }
    case Kind::Path: {
        const QString chosen = QFileDialog::getOpenFileName(this, tr("Select file"),
            current.toString(), param->fileFilter);
        if (chosen.isEmpty()) return;
        setParam(hit.nodeId, hit.paramId, QJsonValue(chosen));
        break;
    }
    case Kind::Text:
    case Kind::Float:
    case Kind::Int:
        beginInlineEdit(hit); // commits when the field closes
        return;
    }
    emit nodeParamsChanged(hit.nodeId);
    emit graphEdited();
}

void NodeCanvas::beginInlineEdit(const ParamHit& hit)
{
    using Kind = NodeGraph::ParamSpec::Kind;
    const NodeGraph::Node* node = m_graph.node(hit.nodeId);
    const NodeGraph::NodeTypeSpec* spec = node ? specFor(*node) : nullptr;
    const NodeGraph::ParamSpec* param = spec ? spec->param(hit.paramId) : nullptr;
    if (!param) return;

    if (!m_inlineEdit) {
        m_inlineEdit = new QLineEdit(this);
        m_inlineEdit->hide();
        connect(m_inlineEdit, &QLineEdit::editingFinished, this, [this] {
            if (!m_inlineParam.valid()) return;
            const ParamHit target = m_inlineParam;
            m_inlineParam = {}; // hide() fires editingFinished again via focus-out
            const QString text = m_inlineEdit->text();
            m_inlineEdit->hide();
            setFocus();

            const NodeGraph::Node* owner = m_graph.node(target.nodeId);
            const NodeGraph::NodeTypeSpec* ownerSpec = owner ? specFor(*owner) : nullptr;
            const NodeGraph::ParamSpec* ps = ownerSpec ? ownerSpec->param(target.paramId) : nullptr;
            if (!ps) return;
            const bool bounded = ps->maximum > ps->minimum;
            bool ok = false;
            if (ps->kind == Kind::Int) {
                const int parsed = text.toInt(&ok);
                if (!ok) return;
                setParam(target.nodeId, target.paramId,
                    QJsonValue(bounded ? int(std::clamp<double>(parsed, ps->minimum, ps->maximum)) : parsed));
            } else if (ps->kind == Kind::Float) {
                const double parsed = text.toDouble(&ok);
                if (!ok) return;
                setParam(target.nodeId, target.paramId,
                    QJsonValue(bounded ? std::clamp(parsed, ps->minimum, ps->maximum) : parsed));
            } else {
                setParam(target.nodeId, target.paramId, QJsonValue(text));
            }
            emit nodeParamsChanged(target.nodeId);
            emit graphEdited();
        });
    }

    const QJsonValue raw = node->params.value(hit.paramId);
    const QJsonValue current = raw.isUndefined() ? param->defaultValue : raw;
    if (param->kind == Kind::Int) m_inlineEdit->setText(QString::number(current.toInt()));
    else if (param->kind == Kind::Float) m_inlineEdit->setText(QString::number(current.toDouble(), 'g', 6));
    else m_inlineEdit->setText(current.toString());

    const QRectF widgetRect(toWidget(hit.row.topLeft()), toWidget(hit.row.bottomRight()));
    m_inlineEdit->setGeometry(widgetRect.toRect().adjusted(-1, -2, 1, 2));
    m_inlineParam = hit;
    m_inlineEdit->show();
    m_inlineEdit->raise();
    m_inlineEdit->selectAll();
    m_inlineEdit->setFocus();
}

void NodeCanvas::mousePressEvent(QMouseEvent* event)
{
    setFocus();
    const QPointF graphPoint = toGraph(event->position());
    m_lastMouse = event->position();

    // The map is on top of the canvas, so it has to claim the press before
    // anything reads a graph coordinate out of it.
    if (event->button() == Qt::LeftButton && panFromMinimap(event->position())) {
        m_minimapDragging = true;
        return;
    }

    if (event->button() == Qt::MiddleButton) { m_panning = true; return; }

    // The knife owns the left button while it is armed, so a stroke can start
    // on top of a node without dragging it.
    if (m_knifeMode && event->button() == Qt::LeftButton) {
        m_knifeCutting = true;
        m_knifeStroke = {graphPoint};
        m_knifeDoomed.clear();
        update();
        return;
    }

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

        // Header toggles take the press before the node-drag path, so clicking
        // one folds instead of nudging the node.
        for (int i = m_graph.nodes.size() - 1; i >= 0; --i) {
            NodeGraph::Node& candidate = m_graph.nodes[i];
            const NodeGraph::NodeTypeSpec* candidateSpec = specFor(candidate);
            if (!candidateSpec || !detailVisible()) break;
            const NodeLayout candidateLayout = layoutFor(candidate);
            if (!candidateLayout.header.contains(graphPoint)) continue;
            bool hit = false;
            for (const auto& toggle : headerToggles(candidate, *candidateSpec, candidateLayout.header)) {
                if (!toggle.second.contains(graphPoint)) continue;
                switch (toggle.first) {
                case HeaderToggle::Preview: candidate.showPreview = !candidate.showPreview; break;
                case HeaderToggle::Settings: candidate.showSettings = !candidate.showSettings; break;
                case HeaderToggle::Collapse: candidate.collapsed = !candidate.collapsed; break;
                }
                hit = true;
                break;
            }
            if (hit) {
                // Folding is a layout change, not a parameter change: it must
                // survive undo and save, but must not re-run the graph.
                emit graphEdited();
                update();
                return;
            }
            break; // the topmost header under the cursor wins either way
        }

        // Channel view toggles. Pure display, so this repaints and re-pushes
        // the 3D preview but never re-runs the graph.
        for (int i = m_graph.nodes.size() - 1; i >= 0; --i) {
            NodeGraph::Node& candidate = m_graph.nodes[i];
            if (!specFor(candidate) || !detailVisible()) break;
            const NodeLayout candidateLayout = layoutFor(candidate);
            if (candidateLayout.channelStrip.isNull()
                || !candidateLayout.channelStrip.contains(graphPoint)) continue;
            const double chipWidth = candidateLayout.channelStrip.width() / 4.0;
            const int index = std::clamp(int((graphPoint.x() - candidateLayout.channelStrip.left())
                / chipWidth), 0, 3);
            candidate.previewChannels ^= (1 << index);
            if (m_selected != candidate.id) { m_selected = candidate.id; emit nodeSelected(candidate.id); }
            else emit nodeSelected(candidate.id); // re-push the isolated view to the 3D preview
            emit graphEdited();
            update();
            return;
        }

        // Curve control points, same precedence as the Levels handles.
        {
            QString curveNode;
            QRectF curvePlot;
            const int point = curvePointAt(graphPoint, &curveNode, &curvePlot);
            if (!curveNode.isEmpty()) {
                if (m_selected != curveNode) { m_selected = curveNode; emit nodeSelected(curveNode); }
                if (point >= 0) {
                    m_curveNode = curveNode;
                    m_curvePoint = point;
                    m_curvePlot = curvePlot;
                } else if (event->button() == Qt::LeftButton) {
                    // Clicking empty space in the editor adds a point there,
                    // which is how every curve editor behaves.
                    NodeGraph::Node* target = m_graph.node(curveNode);
                    const NodeGraph::NodeTypeSpec* targetSpec = target ? specFor(*target) : nullptr;
                    if (target && targetSpec) {
                        QVector<QPointF> points = NodeGraph::curvePoints(
                            target->params.value(targetSpec->curve.points).toString());
                        points.append(QPointF(
                            std::clamp((graphPoint.x() - curvePlot.left()) / curvePlot.width(), 0.0, 1.0),
                            std::clamp((curvePlot.bottom() - graphPoint.y()) / curvePlot.height(), 0.0, 1.0)));
                        std::sort(points.begin(), points.end(),
                            [](const QPointF& a, const QPointF& b) { return a.x() < b.x(); });
                        target->params.insert(targetSpec->curve.points,
                            NodeGraph::curveToString(points));
                        emit nodeParamsChanged(curveNode);
                        emit graphEdited();
                    }
                }
                update();
                return;
            }
        }

        // A Levels handle grabs the press before the node-drag path, so dragging
        // a marker moves the marker rather than the whole node.
        const HandleHit handle = handleAt(graphPoint);
        if (handle.valid()) {
            if (m_selected != handle.nodeId) {
                m_selected = handle.nodeId;
                emit nodeSelected(handle.nodeId);
            }
            m_handleDrag = handle;
            update();
            return;
        }

        // Settings live in the node body, so a press on one edits the setting
        // instead of starting a node drag.
        const ParamHit param = paramAt(graphPoint);
        if (param.valid()) {
            if (m_selected != param.nodeId) {
                m_selected = param.nodeId;
                emit nodeSelected(param.nodeId);
            }
            const NodeGraph::Node* node = m_graph.node(param.nodeId);
            const NodeGraph::NodeTypeSpec* spec = node ? specFor(*node) : nullptr;
            const NodeGraph::ParamSpec* ps = spec ? spec->param(param.paramId) : nullptr;
            const bool numeric = ps && (ps->kind == NodeGraph::ParamSpec::Kind::Float
                                     || ps->kind == NodeGraph::ParamSpec::Kind::Int);
            if (numeric) {
                const QJsonValue raw = node->params.value(param.paramId);
                m_scrubParam = param;
                m_scrubStartX = event->position().x();
                m_scrubStartValue = (raw.isUndefined() ? ps->defaultValue : raw).toDouble();
                m_scrubMoved = false;
            } else {
                activateParam(param, event->globalPosition().toPoint());
            }
            update();
            return;
        }

        // A container's title strip is its handle; dragging it carries the
        // nodes inside along with it.
        for (int i = m_graph.frames.size() - 1; i >= 0; --i) {
            const NodeGraph::Frame& item = m_graph.frames[i];
            const QRectF title(item.rect.left(), item.rect.top(), item.rect.width(), kFrameTitleHeight);
            if (!title.contains(graphPoint)) continue;
            m_frameDrag = item.id;
            m_frameMembers = m_graph.nodesInFrame(item.id, m_registry);
            m_lastMouse = event->position();
            update();
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
    if (m_minimapDragging) {
        // Held drag scrubs the view across the map. The point is clamped into
        // the map rather than dropped when the pointer wanders off it, or the
        // view would freeze mid-drag on a small target.
        const QRectF map = minimapRect();
        if (!map.isNull()) {
            panFromMinimap(QPointF(std::clamp(event->position().x(), map.left(), map.right()),
                std::clamp(event->position().y(), map.top(), map.bottom())));
        }
        m_lastMouse = event->position();
        return;
    }
    if (m_panning) {
        m_pan += event->position() - m_lastMouse;
        m_lastMouse = event->position();
        update();
        return;
    }
    if (m_knifeCutting) {
        // Thinned to roughly one point per 4 device pixels: the stroke stays
        // smooth while keeping the crossing test off a thousand-segment path.
        const double minimumStep = 4.0 / std::max(m_zoom, 0.05);
        if (m_knifeStroke.isEmpty()
            || QLineF(m_knifeStroke.last(), graphPoint).length() >= minimumStep) {
            m_knifeStroke.append(graphPoint);
            m_knifeDoomed = connectionsCrossing(m_knifeStroke);
        }
        update();
        return;
    }
    if (m_curvePoint >= 0) {
        NodeGraph::Node* node = m_graph.node(m_curveNode);
        const NodeGraph::NodeTypeSpec* spec = node ? specFor(*node) : nullptr;
        if (!spec || m_curvePlot.width() <= 0) return;
        QVector<QPointF> points = NodeGraph::curvePoints(
            node->params.value(spec->curve.points).toString());
        if (m_curvePoint >= points.size()) return;
        const double x = std::clamp((graphPoint.x() - m_curvePlot.left()) / m_curvePlot.width(), 0.0, 1.0);
        const double y = std::clamp((m_curvePlot.bottom() - graphPoint.y()) / m_curvePlot.height(), 0.0, 1.0);
        // The end points stay pinned to their edges; letting them slide inward
        // leaves the curve undefined past them and reads as a bug.
        const bool firstPoint = m_curvePoint == 0;
        const bool lastPoint = m_curvePoint == points.size() - 1;
        points[m_curvePoint] = QPointF(firstPoint ? 0.0 : (lastPoint ? 1.0 : x), y);
        node->params.insert(spec->curve.points, NodeGraph::curveToString(points));
        update();
        return;
    }
    if (m_handleDrag.valid()) {
        const NodeGraph::Node* node = m_graph.node(m_handleDrag.nodeId);
        const NodeGraph::NodeTypeSpec* spec = node ? specFor(*node) : nullptr;
        if (!spec) return;
        const QRectF& plot = m_handleDrag.plot;
        const double fraction = plot.width() > 0
            ? std::clamp((graphPoint.x() - plot.left()) / plot.width(), 0.0, 1.0) : 0.0;
        // The midtone handle reports a position between the input handles, so
        // it has to be converted back to a gamma; the rest map 1:1 to 0..1.
        if (m_handleDrag.paramId == spec->histogram.gamma) {
            const auto value = [&](const QString& id, double fallback) {
                const QJsonValue raw = node->params.value(id);
                const NodeGraph::ParamSpec* ps = spec->param(id);
                if (raw.isDouble()) return raw.toDouble();
                return ps && ps->defaultValue.isDouble() ? ps->defaultValue.toDouble() : fallback;
            };
            const double inBlack = value(spec->histogram.blackPoint, 0.0);
            const double inWhite = value(spec->histogram.whitePoint, 1.0);
            const double span = inWhite - inBlack;
            const double within = std::abs(span) < 1e-4 ? 0.5
                : std::clamp((fraction - inBlack) / span, 0.0, 1.0);
            setParam(m_handleDrag.nodeId, m_handleDrag.paramId, fractionToGamma(within));
        } else {
            setParam(m_handleDrag.nodeId, m_handleDrag.paramId, fraction);
        }
        return;
    }
    if (m_scrubParam.valid()) {
        const NodeGraph::Node* node = m_graph.node(m_scrubParam.nodeId);
        const NodeGraph::NodeTypeSpec* spec = node ? specFor(*node) : nullptr;
        const NodeGraph::ParamSpec* ps = spec ? spec->param(m_scrubParam.paramId) : nullptr;
        if (!ps) return;
        const double dx = (event->position().x() - m_scrubStartX) / std::max(m_zoom, 0.05);
        if (std::abs(dx) > 2.0) m_scrubMoved = true;
        if (m_scrubMoved) {
            const double range = ps->maximum - ps->minimum;
            // Full row travel covers the whole range; unbounded params step.
            const double perPixel = range > 0 ? range / 190.0 : std::max(ps->step, 0.01);
            double value = m_scrubStartValue + dx * perPixel;
            if (range > 0) value = std::clamp(value, ps->minimum, ps->maximum);
            setParam(m_scrubParam.nodeId, m_scrubParam.paramId,
                ps->kind == NodeGraph::ParamSpec::Kind::Int ? QJsonValue(int(std::lround(value)))
                                                            : QJsonValue(value));
        }
        return;
    }
    if (!m_frameDrag.isEmpty()) {
        const QPointF delta = (event->position() - m_lastMouse) / std::max(m_zoom, 0.05);
        m_lastMouse = event->position();
        if (NodeGraph::Frame* item = m_graph.frame(m_frameDrag)) item->rect.translate(delta);
        for (const QString& id : m_frameMembers)
            if (NodeGraph::Node* node = m_graph.node(id)) node->position += delta;
        update();
        return;
    }
    if (!m_dragNode.isEmpty()) {
        if (NodeGraph::Node* node = m_graph.node(m_dragNode)) node->position = graphPoint - m_dragOffset;
        // Routing lanes were computed for where this node used to be. Keeping
        // them would bend its wires through empty space, which reads as a bug
        // and is one — a wire has to end where the node is now. Arrange puts
        // them back.
        for (NodeGraph::Connection& connection : m_graph.connections)
            if (connection.fromNode == m_dragNode || connection.toNode == m_dragNode)
                connection.waypoints.clear();
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
            ? tr("MMB or empty-space drag pans â€¢ wheel zooms â€¢ drag a port to wire â€¢ right-click to add nodes â€¢ Del removes")
            : tip);
    }
}

void NodeCanvas::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_minimapDragging) { m_minimapDragging = false; return; }
    if (m_knifeCutting) {
        // Resolved to values before any disconnect: severing one wire shifts
        // every index after it, so cutting by index as we go would cut the
        // wrong ones from the second onwards.
        QVector<NodeGraph::Connection> doomed;
        for (int index : connectionsCrossing(m_knifeStroke))
            if (index >= 0 && index < m_graph.connections.size())
                doomed.append(m_graph.connections.at(index));
        for (const NodeGraph::Connection& connection : doomed) m_graph.disconnect(connection);
        m_knifeCutting = false;
        m_knifeStroke.clear();
        m_knifeDoomed.clear();
        m_panning = false;
        if (!doomed.isEmpty()) emit graphEdited();
        update();
        return;
    }
    if (m_curvePoint >= 0) {
        const QString nodeId = m_curveNode;
        m_curvePoint = -1;
        m_curveNode.clear();
        m_panning = false;
        emit nodeParamsChanged(nodeId);
        emit graphEdited();
        return;
    }
    if (m_handleDrag.valid()) {
        const QString nodeId = m_handleDrag.nodeId;
        m_handleDrag = {};
        m_panning = false;
        // One re-evaluation and one undo entry per drag, as with drag-scrub.
        emit nodeParamsChanged(nodeId);
        emit graphEdited();
        return;
    }
    if (m_scrubParam.valid()) {
        const ParamHit hit = m_scrubParam;
        const bool moved = m_scrubMoved;
        m_scrubParam = {};
        m_scrubMoved = false;
        m_panning = false;
        // Re-evaluate once when the drag ends, not on every mouse move.
        if (moved) {
            emit nodeParamsChanged(hit.nodeId);
            emit graphEdited();
        } else {
            beginInlineEdit(hit);
        }
        return;
    }
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
    if (!m_frameDrag.isEmpty()) { m_frameDrag.clear(); m_frameMembers.clear(); emit graphEdited(); }
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
    if (event->key() == Qt::Key_F && (event->modifiers() & Qt::ControlModifier)) {
        beginNodeSearch();
        return;
    }
    if (event->key() == Qt::Key_K) { setKnifeMode(!m_knifeMode); return; }
    if (event->key() == Qt::Key_Escape) {
        if (m_searchEdit && m_searchEdit->isVisible()) { endNodeSearch(); return; }
        if (m_knifeMode) { setKnifeMode(false); return; }
    }
    // Shift+F frames the selection, which is what is wanted once the graph is
    // large enough that fitting all of it is how you lost the node.
    if (event->key() == Qt::Key_F) {
        if ((event->modifiers() & Qt::ShiftModifier) && !m_selected.isEmpty()) frameNode(m_selected);
        else frameAll();
        return;
    }
    // Blender's H: fold the selected node to its header.
    if (event->key() == Qt::Key_H && !m_selected.isEmpty()) {
        if (NodeGraph::Node* node = m_graph.node(m_selected)) {
            if (event->modifiers() & Qt::ControlModifier) node->hideUnusedPorts = !node->hideUnusedPorts;
            else node->collapsed = !node->collapsed;
            emit graphEdited();
            update();
        }
        return;
    }
    QWidget::keyPressEvent(event);
}

QImage NodeCanvas::channelAsImage(const QImage& source, int channel)
{
    if (source.isNull() || channel < 0 || channel > 3) return QImage();
    const QImage rgba = source.convertToFormat(QImage::Format_RGBA8888);
    QImage result(rgba.width(), rgba.height(), QImage::Format_RGBA8888);
    for (int y = 0; y < rgba.height(); ++y) {
        const uchar* in = rgba.constScanLine(y);
        uchar* out = result.scanLine(y);
        for (int x = 0; x < rgba.width(); ++x) {
            const uchar value = in[x * 4 + channel];
            out[x * 4] = out[x * 4 + 1] = out[x * 4 + 2] = value;
            out[x * 4 + 3] = 255;
        }
    }
    return result;
}

void NodeCanvas::contextMenuEvent(QContextMenuEvent* event)
{
    const QPointF graphPoint = toGraph(event->pos());
    QMenu menu(this);
    const QString underCursor = nodeAt(graphPoint);
    if (!underCursor.isEmpty()) {
        const NodeGraph::Node* node = m_graph.node(underCursor);
        const NodeGraph::NodeTypeSpec* spec = node ? specFor(*node) : nullptr;
        if (node) {
            QAction* ports = menu.addAction(tr("Hide Unused Ports"), this, [this, underCursor] {
                NodeGraph::Node* target = m_graph.node(underCursor);
                if (!target) return;
                target->hideUnusedPorts = !target->hideUnusedPorts;
                emit graphEdited();
                update();
            });
            ports->setCheckable(true);
            ports->setChecked(node->hideUnusedPorts);
            ports->setToolTip(tr("Drop ports with nothing wired to them. Connected ports always stay."));
            menu.addSeparator();
        }
        // Levels actions live here rather than as inline buttons: the node body
        // is already dense, and these are occasional rather than per-drag.
        if (spec && spec->histogram.valid()) {
            menu.addAction(tr("Auto Levels"), this,
                [this, underCursor] { applyHistogramAction(underCursor, QStringLiteral("auto")); });
            menu.addAction(tr("Invert Levels"), this,
                [this, underCursor] { applyHistogramAction(underCursor, QStringLiteral("invert")); });
            menu.addAction(tr("Reset Levels"), this,
                [this, underCursor] { applyHistogramAction(underCursor, QStringLiteral("reset")); });
            menu.addSeparator();
        }
        // Copy what a node actually produced to the clipboard, whole or one
        // channel at a time. Reading a packed map is otherwise a round trip
        // through a Separate Channels node and an export, and the question is
        // usually "what is in the green channel here" rather than a permanent
        // edit to the graph.
        if (spec && m_results.contains(underCursor)) {
            const NodeGraph::NodeResult& result = m_results.value(underCursor);
            QVector<QPair<QString, QImage>> images;
            for (const NodeGraph::PortSpec& port : spec->outputs) {
                if (port.type != NodeGraph::ValueType::Image) continue;
                const QImage image = result.outputs.value(port.id).image();
                if (!image.isNull()) images.append({port.name, image});
            }
            if (!images.isEmpty()) {
                QMenu* copy = menu.addMenu(tr("Copy"));
                // The resolution is on the label because the canvas evaluates
                // at preview size: what lands on the clipboard is what the node
                // holds here, not necessarily what a conversion would write.
                for (const QPair<QString, QImage>& entry : images) {
                    const QImage image = entry.second;
                    QMenu* target = images.size() == 1 ? copy : copy->addMenu(entry.first);
                    target->addAction(tr("Image  (%1×%2)").arg(image.width()).arg(image.height()),
                        this, [image] { QGuiApplication::clipboard()->setImage(image); });
                    target->addSeparator();
                    static const char* const channelNames[] = {
                        QT_TR_NOOP("Red"), QT_TR_NOOP("Green"), QT_TR_NOOP("Blue"), QT_TR_NOOP("Alpha")};
                    for (int channel = 0; channel < 4; ++channel) {
                        target->addAction(tr(channelNames[channel]), this, [image, channel] {
                            QGuiApplication::clipboard()->setImage(
                                NodeCanvas::channelAsImage(image, channel));
                        });
                    }
                }
                menu.addSeparator();
            }
        }
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
    // Containers: group a stretch of the pipeline and move it as one.
    menu.addAction(tr("Add Container"), this, [this, graphPoint] {
        QRectF bounds;
        for (const NodeGraph::Node& node : m_graph.nodes) {
            if (node.id != m_selected) continue;
            bounds = nodeRect(node);
        }
        const QRectF rect = bounds.isNull()
            ? QRectF(graphPoint, QSizeF(420, 320))
            : bounds.adjusted(-30, -kFrameTitleHeight - 20, 30, 30);
        m_graph.addFrame(tr("Group"), rect);
        emit graphEdited();
        update();
    });
    for (int i = m_graph.frames.size() - 1; i >= 0; --i) {
        const NodeGraph::Frame& item = m_graph.frames[i];
        if (!item.rect.contains(graphPoint)) continue;
        const QString id = item.id;
        menu.addAction(tr("Rename Containerâ€¦"), this, [this, id] {
            NodeGraph::Frame* target = m_graph.frame(id);
            if (!target) return;
            bool accepted = false;
            const QString name = QInputDialog::getText(this, tr("Rename container"), tr("Name"),
                QLineEdit::Normal, target->title, &accepted);
            if (!accepted) return;
            target->title = name;
            emit graphEdited();
            update();
        });
        menu.addAction(tr("Remove Container"), this, [this, id] {
            // The nodes inside outlive the container.
            m_graph.removeFrame(id);
            emit graphEdited();
            update();
        });
        break;
    }
    menu.addAction(tr("Arrange Nodes"), this, [this] {
        NodeGraph::layoutGraph(m_graph, m_registry);
        frameAll();
        emit graphEdited();
    });
    menu.addAction(tr("Frame All"), this, &NodeCanvas::frameAll);
    menu.addSeparator();
    // Whole-graph folding: flip between an overview of the network and the
    // detail, without visiting 25 headers.
    QMenu* fold = menu.addMenu(tr("Fold"));
    fold->addAction(tr("Collapse All Nodes"), this, [this] { setAllCollapsed(true); });
    fold->addAction(tr("Expand All Nodes"), this, [this] { setAllCollapsed(false); });
    fold->addSeparator();
    fold->addAction(tr("Hide All Previews"), this, [this] { setAllPreviews(false); });
    fold->addAction(tr("Show All Previews"), this, [this] { setAllPreviews(true); });
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
            auto* browse = new QPushButton(tr("â€¦"), row);
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
