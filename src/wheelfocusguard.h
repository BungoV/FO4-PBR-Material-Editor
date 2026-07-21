#pragma once

#include <QObject>

class QWidget;

// Makes a wheel-editable widget (combo box, spin box) safe inside a scrolling
// page: focus becomes click/Tab only and wheel events are ignored until the
// widget has focus, so page scrolling passes through instead of editing the
// value under the cursor.
class WheelFocusGuard final : public QObject {
    Q_OBJECT

public:
    static void install(QWidget* widget);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    explicit WheelFocusGuard(QWidget* widget);
};
