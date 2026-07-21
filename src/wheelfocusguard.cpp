#include "wheelfocusguard.h"

#include <QEvent>
#include <QWidget>

WheelFocusGuard::WheelFocusGuard(QWidget* widget)
    : QObject(widget)
{
}

void WheelFocusGuard::install(QWidget* widget)
{
    widget->setFocusPolicy(Qt::StrongFocus);
    widget->installEventFilter(new WheelFocusGuard(widget));
}

bool WheelFocusGuard::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::Wheel) {
        auto* widget = static_cast<QWidget*>(watched);
        if (!widget->hasFocus()) {
            // Ignored but filtered: the widget never sees the wheel and the
            // unaccepted event propagates on to the enclosing scroll area.
            event->ignore();
            return true;
        }
    }
    return QObject::eventFilter(watched, event);
}
