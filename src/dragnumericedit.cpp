#include "dragnumericedit.h"

#include <QApplication>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QStyleOptionSpinBox>

DragNumericEdit::DragNumericEdit(QWidget* parent)
    : QDoubleSpinBox(parent)
{
    setButtonSymbols(QAbstractSpinBox::NoButtons);
    setDecimals(4);
    setRange(-1000000.0, 1000000.0);
    setSingleStep(0.01);
    setAlignment(Qt::AlignCenter);
    setKeyboardTracking(false);
    setMouseTracking(true);
    // Click or Tab focus only: the WheelFocus default lets a page scroll edit
    // whichever field the cursor happens to cross.
    setFocusPolicy(Qt::StrongFocus);
    lineEdit()->setMouseTracking(true);
    lineEdit()->installEventFilter(this);
    setToolTip(tr("Enter a number, drag horizontally over the value, or click first and use the mouse wheel. Shift makes dragging precise; Ctrl makes it faster."));
}

void DragNumericEdit::wheelEvent(QWheelEvent* event)
{
    if (!hasFocus()) {
        event->ignore();
        return;
    }
    QDoubleSpinBox::wheelEvent(event);
}

bool DragNumericEdit::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != lineEdit())
        return QDoubleSpinBox::eventFilter(watched, event);

    if (event->type() == QEvent::Enter) {
        m_hovered = true;
        lineEdit()->setCursor(Qt::SizeHorCursor);
        update();
    } else if (event->type() == QEvent::Leave && !m_pressed) {
        m_hovered = false;
        lineEdit()->unsetCursor();
        update();
    } else if (event->type() == QEvent::MouseButtonPress) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            m_pressed = true;
            m_dragging = false;
            m_pressPosition = mouse->globalPosition().toPoint();
            m_pressValue = value();
        }
    } else if (event->type() == QEvent::MouseMove && m_pressed) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->buttons() & Qt::LeftButton) {
            const int delta = mouse->globalPosition().toPoint().x() - m_pressPosition.x();
            if (qAbs(delta) >= QApplication::startDragDistance())
                m_dragging = true;
            if (m_dragging) {
                double scale = m_dragSensitivity;
                if (mouse->modifiers() & Qt::ShiftModifier) scale *= 0.1;
                if (mouse->modifiers() & Qt::ControlModifier) scale *= 10.0;
                setValue(m_pressValue + static_cast<double>(delta) * scale);
                update();
                return true;
            }
        }
    } else if (event->type() == QEvent::MouseButtonRelease) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            const bool wasDragging = m_dragging;
            m_pressed = false;
            m_dragging = false;
            update();
            if (wasDragging)
                return true;
        }
    }
    return QDoubleSpinBox::eventFilter(watched, event);
}

void DragNumericEdit::setDragSensitivity(double sensitivity)
{
    m_dragSensitivity = qMax(0.000001, sensitivity);
}

void DragNumericEdit::enterEvent(QEnterEvent* event)
{
    m_hovered = true;
    setCursor(Qt::SizeHorCursor);
    update();
    QDoubleSpinBox::enterEvent(event);
}

void DragNumericEdit::leaveEvent(QEvent* event)
{
    if (!m_pressed) {
        m_hovered = false;
        unsetCursor();
        update();
    }
    QDoubleSpinBox::leaveEvent(event);
}

void DragNumericEdit::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_pressed = true;
        m_dragging = false;
        m_pressPosition = event->globalPosition().toPoint();
        m_pressValue = value();
    }
    QDoubleSpinBox::mousePressEvent(event);
}

void DragNumericEdit::mouseMoveEvent(QMouseEvent* event)
{
    if (m_pressed && (event->buttons() & Qt::LeftButton)) {
        const int delta = event->globalPosition().toPoint().x() - m_pressPosition.x();
        if (qAbs(delta) >= QApplication::startDragDistance())
            m_dragging = true;
        if (m_dragging) {
            double scale = m_dragSensitivity;
            if (event->modifiers() & Qt::ShiftModifier)
                scale *= 0.1;
            if (event->modifiers() & Qt::ControlModifier)
                scale *= 10.0;
            setValue(m_pressValue + static_cast<double>(delta) * scale);
            lineEdit()->deselect();
            event->accept();
            update();
            return;
        }
    }
    QDoubleSpinBox::mouseMoveEvent(event);
}

void DragNumericEdit::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        const bool wasDragging = m_dragging;
        m_pressed = false;
        m_dragging = false;
        if (wasDragging) {
            event->accept();
            update();
            return;
        }
    }
    QDoubleSpinBox::mouseReleaseEvent(event);
}

void DragNumericEdit::paintEvent(QPaintEvent* event)
{
    QDoubleSpinBox::paintEvent(event);
    if (!m_hovered && !m_dragging)
        return;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor color = m_dragging ? QColor(255, 181, 74) : palette().color(QPalette::Text);
    painter.setPen(QPen(color, 1.4));
    const int centerY = height() / 2;
    const int left = 8;
    const int right = width() - 8;
    painter.drawLine(left + 4, centerY - 4, left, centerY);
    painter.drawLine(left, centerY, left + 4, centerY + 4);
    painter.drawLine(right - 4, centerY - 4, right, centerY);
    painter.drawLine(right, centerY, right - 4, centerY + 4);
}
