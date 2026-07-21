#include "disclosureicon.h"

#include <QPainter>
#include <QPixmap>

QIcon disclosureArrowIcon(bool expanded, int size, const QColor& color)
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
        const qreal s = size;
        QPolygonF triangle;
        if (expanded)
            triangle << QPointF(s * 0.18, s * 0.32) << QPointF(s * 0.82, s * 0.32) << QPointF(s * 0.5, s * 0.74);
        else
            triangle << QPointF(s * 0.32, s * 0.18) << QPointF(s * 0.32, s * 0.82) << QPointF(s * 0.74, s * 0.5);
        painter.drawPolygon(triangle);
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}
