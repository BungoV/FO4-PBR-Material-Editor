#pragma once

#include "nodegraph.h"

#include <QHash>
#include <QPointF>
#include <QWidget>

class QFormLayout;
class QScrollArea;

// Domain-agnostic node editor UI over nodegraph.h: a canvas that draws the
// graph with a live thumbnail on every node, and an inspector that builds
// parameter editors from the selected node's ParamSpecs. Neither knows what
// the nodes do, so the material editor can reuse both with its own library.
class NodeCanvas final : public QWidget {
    Q_OBJECT

public:
    explicit NodeCanvas(const NodeGraph::Registry& registry, QWidget* parent = nullptr);

    const NodeGraph::Graph& graph() const { return m_graph; }
    void setGraph(const NodeGraph::Graph& graph);
    // Evaluation results supply thumbnails, notes, and per-node errors.
    void setResults(const QHash<QString, NodeGraph::NodeResult>& results);
    QString selectedNode() const { return m_selected; }
    void frameAll();

signals:
    void graphEdited();                       // topology or parameters changed
    void nodeSelected(const QString& nodeId); // empty when the selection clears

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    struct PortHit {
        QString nodeId;
        QString portId;
        bool isOutput = false;
        bool valid() const { return !nodeId.isEmpty(); }
    };

    QRectF nodeRect(const NodeGraph::Node& node) const;      // graph space
    QPointF portCenter(const NodeGraph::Node& node, const QString& portId, bool isOutput) const;
    PortHit portAt(const QPointF& graphPoint) const;
    QString nodeAt(const QPointF& graphPoint) const;
    QPointF toGraph(const QPointF& widgetPoint) const;
    QPointF toWidget(const QPointF& graphPoint) const;
    void drawNode(QPainter& painter, const NodeGraph::Node& node);
    void drawConnection(QPainter& painter, const QPointF& from, const QPointF& to, const QColor& color, bool active);
    const NodeGraph::NodeTypeSpec* specFor(const NodeGraph::Node& node) const;
    QString previewPort(const NodeGraph::Node& node) const;

    const NodeGraph::Registry& m_registry;
    NodeGraph::Graph m_graph;
    QHash<QString, NodeGraph::NodeResult> m_results;
    QHash<QString, QString> m_previewPort; // node -> which output the thumbnail shows

    QPointF m_pan;
    double m_zoom = 1.0;
    QString m_selected;
    QString m_dragNode;
    QPointF m_dragOffset;
    QPointF m_lastMouse;
    bool m_panning = false;
    PortHit m_wireFrom;
    QPointF m_wireTo;
    QString m_hoverNode;
};

// Parameter editors for the selected node, generated from its ParamSpecs.
class NodeInspector final : public QWidget {
    Q_OBJECT

public:
    explicit NodeInspector(const NodeGraph::Registry& registry, QWidget* parent = nullptr);

    void showNode(const NodeGraph::Graph* graph, const QString& nodeId,
        const NodeGraph::NodeResult& result);
    void clear();

signals:
    void parameterChanged(const QString& nodeId, const QString& paramId, const QJsonValue& value);

private:
    const NodeGraph::Registry& m_registry;
    QFormLayout* m_form = nullptr;
    QWidget* m_body = nullptr;
    QScrollArea* m_scroll = nullptr;
    QString m_nodeId;
};
