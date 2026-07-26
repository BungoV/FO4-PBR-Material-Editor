#include "disclosureicon.h"

#include <QPainter>
#include <QPixmap>

namespace {

// One triangle, one place. Every icon here is the same shape at a different
// rotation, so they stay a matched set: identical inset, identical antialiasing,
// identical @1x/@2x handling. Adding a fourth direction should never mean
// re-deriving the geometry by eye.
QIcon trianglePolygonIcon(const QPolygonF& unitShape, int size, const QColor& color)
{
    QIcon icon;
    for (int scale : {1, 2}) {
        QPixmap pixmap(size * scale, size * scale);
        pixmap.setDevicePixelRatio(scale);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        QPolygonF scaled;
        for (const QPointF& point : unitShape) scaled << point * size;
        painter.drawPolygon(scaled);
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

} // namespace

QIcon disclosureArrowIcon(bool expanded, int size, const QColor& color)
{
    // Down is wide and shallow, right is narrow and tall — a disclosure pair
    // reads better when the two states occupy a similar visual weight rather
    // than being one shape rotated.
    const QPolygonF down{{0.18, 0.32}, {0.82, 0.32}, {0.5, 0.74}};
    const QPolygonF right{{0.32, 0.18}, {0.32, 0.82}, {0.74, 0.5}};
    return trianglePolygonIcon(expanded ? down : right, size, color);
}

QIcon navigationArrowIcon(bool forward, int size, const QColor& color)
{
    // Mirrored about the vertical centre, so previous and next carry exactly
    // the same weight — the thing the "◀"/"▶" glyph pair fails at.
    const QPolygonF next{{0.34, 0.18}, {0.34, 0.82}, {0.74, 0.5}};
    const QPolygonF previous{{0.66, 0.18}, {0.66, 0.82}, {0.26, 0.5}};
    return trianglePolygonIcon(forward ? next : previous, size, color);
}
