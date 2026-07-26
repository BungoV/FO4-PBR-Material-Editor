#pragma once

#include "nodegraph.h"

#include <QHash>
#include <QPointF>
#include <QWidget>

class QFormLayout;
class QLineEdit;
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
    // Centres one node at a working zoom. Frame All is the wrong tool once a
    // graph is big enough that "all" is the thing you were trying to escape.
    void frameNode(const QString& nodeId);
    // Ctrl+F: opens the overlay field. Public so a menu or a shortcut outside
    // the canvas can reach the same entry point.
    void beginNodeSearch();
    // Flip every node's fold state at once, for going between an overview of
    // the whole network and working on one part of it.
    void setAllCollapsed(bool collapsed);
    void setAllPreviews(bool shown);
    // Targeted so the viewport's channel picker can drive a node's isolation
    // without going through setGraph, which re-frames the canvas.
    void setNodePreviewChannels(const QString& nodeId, int mask);
    // Floats a small toolbar in the canvas corner, the way node editors keep
    // their view controls out of the surrounding chrome.
    void setCornerWidget(QWidget* widget);
    // Shown centred while the graph is empty; this is where a first-run user
    // is told what to do instead of being handed a blank grid.
    void setPlaceholder(const QString& text);

    // --- Read-only hooks for --canvas-nav-check. The navigation aids are
    // painter-space and focus behaviour, which a screenshot at one zoom cannot
    // confirm; these let the harness drive them instead of eyeballing them.
    double zoomForTest() const { return m_zoom; }
    QRectF minimapRectForTest() const { return minimapRect(); }
    QPointF graphPointForTest(const QPointF& widgetPoint) const { return toGraph(widgetPoint); }
    QPointF widgetPointForTest(const QPointF& graphPoint) const { return toWidget(graphPoint); }
    // The graph point a minimap position stands for, computed the way the map
    // draws rather than the way the pan applies it — so a disagreement between
    // the two shows up as a failure instead of a map that silently lies.
    QPointF minimapGraphPointForTest(const QPointF& widgetPoint) const;
    // Label rects from the last paint, in widget space. Every one must sit
    // inside the canvas — a label centred on an edge node is otherwise sliced.
    QVector<QRectF> overlayLabelsForTest() const { return m_overlayLabels; }
    bool searchVisibleForTest() const;
    QStringList searchMatchesForTest() const { return m_searchMatches; }
    QWidget* searchFieldForTest() const;
    void setSearchTextForTest(const QString& text);
    void selectNodeForTest(const QString& nodeId) { m_selected = nodeId; update(); }
    bool knifeModeForTest() const { return m_knifeMode; }

signals:
    void graphEdited();                       // topology or parameters changed
    void nodeSelected(const QString& nodeId); // empty when the selection clears
    // A setting was edited inside the node body; listeners re-sync side panels.
    void nodeParamsChanged(const QString& nodeId);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    struct PortHit {
        QString nodeId;
        QString portId;
        bool isOutput = false;
        bool valid() const { return !nodeId.isEmpty(); }
    };

    // Blender's node body order, resolved once per node: header, preview,
    // outputs, settings, status, inputs. Everything else reads sub-rects from
    // here so no two elements can be placed on top of each other.
    struct NodeLayout {
        QRectF rect;
        QRectF header;
        QRectF thumb;                // null when the node has no image output
        QRectF channelStrip;         // R/G/B/A view toggles under the thumbnail
        QVector<QRectF> outputRows;  // parallel to spec->outputs
        QRectF histogram;            // null unless the spec declares a HistogramSpec
        QRectF curve;                // null unless the spec declares a CurveSpec
        QVector<QRectF> paramRows;   // parallel to spec->params
        QRectF statusRow;
        QRectF warningRow;
        QVector<QRectF> inputRows;   // parallel to spec->inputs
    };
    struct ParamHit {
        QString nodeId;
        QString paramId;
        QRectF row;                  // graph space
        bool valid() const { return !paramId.isEmpty(); }
    };
    // Which Levels handle a press landed on. The params are named by the node's
    // HistogramSpec, so the canvas still knows nothing about what the node does.
    struct HandleHit {
        QString nodeId;
        QString paramId;
        QRectF plot;                 // graph space, the distribution area
        bool valid() const { return !paramId.isEmpty(); }
    };

    NodeLayout layoutFor(const NodeGraph::Node& node) const; // graph space
    QRectF nodeRect(const NodeGraph::Node& node) const;      // graph space
    QPointF portCenter(const NodeGraph::Node& node, const QString& portId, bool isOutput) const;
    PortHit portAt(const QPointF& graphPoint) const;
    ParamHit paramAt(const QPointF& graphPoint) const;
    QString nodeAt(const QPointF& graphPoint) const;
    QPointF toGraph(const QPointF& widgetPoint) const;
    QPointF toWidget(const QPointF& graphPoint) const;
    void drawNode(QPainter& painter, const NodeGraph::Node& node);
    void drawParamRow(QPainter& painter, const NodeGraph::Node& node,
        const NodeGraph::ParamSpec& spec, const QRectF& row);
    // Levels strip: the input distribution with the remap handles on it.
    void drawHistogram(QPainter& painter, const NodeGraph::Node& node,
        const NodeGraph::NodeTypeSpec& spec, const QRectF& area);
    // Blender's header toggles: preview, settings, collapse. Returned in draw
    // order, right-aligned in the header; empty when the node has no body.
    enum class HeaderToggle { Preview, Settings, Collapse };
    QVector<QPair<HeaderToggle, QRectF>> headerToggles(const NodeGraph::Node& node,
        const NodeGraph::NodeTypeSpec& spec, const QRectF& header) const;
    // Two levels of detail, because text and images stop being useful at very
    // different zooms. Layout is untouched by both, so nothing shifts.
    // Setting rows, the Levels strip, header toggles and port labels: below
    // this the text is unreadable, and drawing it is just noise and cost.
    bool detailVisible() const { return m_zoom >= 0.55; }
    // Thumbnails survive much further out — zoomed out they are the main way
    // to tell one branch of the graph from another, even as colour blobs.
    bool thumbnailsVisible() const { return m_zoom >= 0.22; }
    HandleHit handleAt(const QPointF& graphPoint) const;
    // Bounding box of every node, in graph space. Framing and the minimap both
    // need it, and they must agree or the viewport rect drifts off the map.
    // Cached, because answering it lays out every node and three callers ask
    // per repaint; invalidateBounds() must follow anything that moves a node.
    QRectF graphBounds() const;
    // Valid for one paint. paintEvent clears it on entry, so nothing that
    // changes the graph can leave a stale answer behind.
    mutable QRectF m_bounds;
    mutable bool m_boundsValid = false;
    // Minimap: an overview in a corner, because past about twenty nodes
    // panning to find something costs more than glancing at a map. Null when
    // there is nothing to map or the canvas is too small to spare the corner.
    QRectF minimapRect() const;
    void drawMinimap(QPainter& painter) const;
    // Pans so the graph point under a minimap click sits in the middle of the
    // view. Returns false when the point was not on the map.
    bool panFromMinimap(const QPointF& widgetPoint);
    // Node titles and status dots, drawn after the graph transform is popped so
    // they keep a fixed screen size. In graph space an 11px title is 3px at the
    // zoom that fits a whole graph on screen, which is why the overview reads
    // as a wall of grey boxes.
    void drawNodeOverlay(QPainter& painter);
    void refreshSearchMatches();
    void stepSearch(int direction);
    void endNodeSearch();
    // Curve editor: the input distribution behind the transfer curve, with its
    // control points draggable. Same binding trick as the histogram strip.
    void drawCurve(QPainter& painter, const NodeGraph::Node& node,
        const NodeGraph::NodeTypeSpec& spec, const QRectF& area);
    // Index of the control point under the cursor, or -1. plot receives the
    // editor's drawing area so a drag can map back to 0..1.
    int curvePointAt(const QPointF& graphPoint, QString* nodeId, QRectF* plot) const;
    // Reset / Auto / Invert, offered on the node's context menu rather than as
    // inline buttons: the node body is already dense and these are rare.
    void applyHistogramAction(const QString& nodeId, const QString& action);
    void drawConnection(QPainter& painter, const QPointF& from, const QPointF& to,
        const QColor& color, bool active, const QVector<QPointF>& waypoints = {});
    // The control points of a wire, as one cubic per hop through its routing
    // lanes (p0 c1 c2 p1 c3 c4 p2…) — a plain four-point bezier when it has
    // none. Both the painter and the knife read it, so a wire is cut exactly
    // where it looks like it runs.
    QVector<QPointF> connectionCurve(const QPointF& from, const QPointF& to,
        const QVector<QPointF>& waypoints = {}) const;
    // Indices into m_graph.connections whose wire the stroke crosses. Indices
    // rather than copies: the same list highlights the doomed wires during the
    // drag and then cuts them, and the graph cannot change mid-stroke.
    QVector<int> connectionsCrossing(const QVector<QPointF>& stroke) const;
    void setKnifeMode(bool on);
    const NodeGraph::NodeTypeSpec* specFor(const NodeGraph::Node& node) const;
    QString previewPort(const NodeGraph::Node& node) const;
    // In-node editing: activate opens the right editor for the param kind.
    void setParam(const QString& nodeId, const QString& paramId, const QJsonValue& value);
    void activateParam(const ParamHit& hit, const QPoint& globalPos);
    void beginInlineEdit(const ParamHit& hit);

    const NodeGraph::Registry& m_registry;
    NodeGraph::Graph m_graph;
    QHash<QString, NodeGraph::NodeResult> m_results;
    QHash<QString, QString> m_previewPort; // node -> which output the thumbnail shows
    // Channel-masked thumbnails, keyed by node + port + mask. The mask pass is
    // per-texel and its answer never changes between frames; without this it
    // ran for every node on every repaint. Cleared whenever the results or the
    // graph change, which is the only way the source image can differ.
    mutable QHash<QString, QImage> m_thumbCache;

    QPointF m_pan;
    double m_zoom = 1.0;
    QString m_selected;
    QString m_dragNode;
    QPointF m_dragOffset;
    QPointF m_lastMouse;
    bool m_panning = false;
    // Framing waits for the first paint: at setGraph time the widget has not
    // been laid out yet, so fitting against width()/height() there puts the
    // graph off-screen.
    bool m_framed = false;
    PortHit m_wireFrom;
    QPointF m_wireTo;
    QString m_hoverNode;

    // Drag-scrub on a numeric setting, Blender-style: press and drag changes
    // the value, press and release without moving opens a text field.
    ParamHit m_scrubParam;
    double m_scrubStartX = 0;
    double m_scrubStartValue = 0;
    bool m_scrubMoved = false;
    QLineEdit* m_inlineEdit = nullptr;
    ParamHit m_inlineParam;
    // Dragging a Levels handle. Like drag-scrub, this emits graphEdited once on
    // release so a drag is one undo step and one full-resolution re-evaluation.
    HandleHit m_handleDrag;
    QString m_curveNode;      // node whose curve point is being dragged
    int m_curvePoint = -1;    // index into that node's control points
    QRectF m_curvePlot;       // graph space, for mapping the drag back to 0..1
    // Knife: K arms it, dragging draws a stroke, and every wire the stroke
    // crosses is severed on release. Stays armed for repeated cuts until K or
    // Escape, since one bad wiring pass usually means several.
    bool m_knifeMode = false;
    bool m_knifeCutting = false;
    QVector<QPointF> m_knifeStroke; // graph space
    QVector<int> m_knifeDoomed;     // recomputed per move, drawn and then cut
    QWidget* m_cornerWidget = nullptr;
    // Node search: the field lives in the canvas corner rather than in the
    // surrounding chrome, so it is where the eyes already are.
    QLineEdit* m_searchEdit = nullptr;
    QStringList m_searchMatches;
    int m_searchIndex = -1;
    // Overlay label rects from the last paint, kept so placement can be
    // asserted rather than eyeballed. Mutable: paintEvent is const-ish work.
    QVector<QRectF> m_overlayLabels;
    bool m_minimapDragging = false;
    QString m_placeholder;
    QString m_frameDrag;        // container being dragged by its title strip
    QStringList m_frameMembers; // nodes captured when the drag began
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
