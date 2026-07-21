#pragma once

#include <QColor>
#include <QDialog>
#include <QWidget>

class QLineEdit;
class QPushButton;
class QToolButton;
class QSlider;
class QSpinBox;
class QVBoxLayout;
class HsvColorField;

class ColorPickerDialog final : public QDialog {
    Q_OBJECT

public:
    explicit ColorPickerDialog(const QColor& initial, bool alphaEnabled = true, QWidget* parent = nullptr);
    QColor selectedColor() const;
    static QColor choose(const QColor& initial, bool alphaEnabled, QWidget* parent = nullptr);

private:
    void applyColor(const QColor& color, bool updateField = true);
    void refreshControls();
    void refreshGradients();
    void addValueRow(QVBoxLayout* layout, const QString& label, QSlider*& slider, QSpinBox*& spin, int maximum);
    QString displayHex() const;

    QColor m_original;
    QColor m_current;
    bool m_alphaEnabled = true;
    bool m_syncing = false;
    HsvColorField* m_field = nullptr;
    QPushButton* m_previous = nullptr;
    QPushButton* m_currentSwatch = nullptr;
    QLineEdit* m_hex = nullptr;
    QSlider* m_redSlider = nullptr;
    QSlider* m_greenSlider = nullptr;
    QSlider* m_blueSlider = nullptr;
    QSlider* m_hueSlider = nullptr;
    QSlider* m_saturationSlider = nullptr;
    QSlider* m_valueSlider = nullptr;
    QSlider* m_alphaSlider = nullptr;
    QSpinBox* m_red = nullptr;
    QSpinBox* m_green = nullptr;
    QSpinBox* m_blue = nullptr;
    QSpinBox* m_hue = nullptr;
    QSpinBox* m_saturation = nullptr;
    QSpinBox* m_value = nullptr;
    QSpinBox* m_alpha = nullptr;
};

class ColorEdit final : public QWidget {
    Q_OBJECT

public:
    explicit ColorEdit(const QColor& color = Qt::white, bool alphaEnabled = true, QWidget* parent = nullptr);
    QColor color() const;
    void setColor(const QColor& color);

signals:
    void colorChanged(const QColor& color);

private:
    void updateDisplay();

    QColor m_color;
    bool m_alphaEnabled = true;
    QToolButton* m_swatch = nullptr;
    QLineEdit* m_lineEdit = nullptr;
};
