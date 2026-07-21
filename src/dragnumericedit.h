#pragma once

#include <QDoubleSpinBox>
#include <QPoint>

class DragNumericEdit final : public QDoubleSpinBox {
    Q_OBJECT

public:
    explicit DragNumericEdit(QWidget* parent = nullptr);
    void setDragSensitivity(double sensitivity);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    bool m_hovered = false;
    bool m_pressed = false;
    bool m_dragging = false;
    QPoint m_pressPosition;
    double m_pressValue = 0.0;
    double m_dragSensitivity = 0.01;
};
