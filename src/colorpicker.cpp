#include "colorpicker.h"

#include <QApplication>
#include <QClipboard>
#include <QEventLoop>
#include <QGuiApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QPixmap>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScreen>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <memory>
#include <vector>

namespace {

QString chipStyle(const QColor& color)
{
    return QStringLiteral("background:%1; border:1px solid #777c83; border-radius:2px;")
        .arg(color.name(QColor::HexRgb));
}

QString sliderStyle(const QString& stops)
{
    return QStringLiteral(
        "QSlider::groove:horizontal{height:10px;border:1px solid #22252a;border-radius:2px;"
        "background:qlineargradient(x1:0,y1:0,x2:1,y2:0,%1);}"
        "QSlider::handle:horizontal{width:10px;margin:-4px 0;border:1px solid #e8eaed;"
        "border-radius:5px;background:#4a4e54;}").arg(stops);
}

QString twoStops(const QColor& first, const QColor& second)
{
    return QStringLiteral("stop:0 %1,stop:1 %2")
        .arg(first.name(QColor::HexRgb), second.name(QColor::HexRgb));
}

class ScreenSamplerOverlay final : public QWidget {
    Q_OBJECT

public:
    explicit ScreenSamplerOverlay(const QColor& fallback)
        : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
        , m_color(fallback)
    {
        const auto screens = QGuiApplication::screens();
        for (QScreen* screen : screens) m_virtual = m_virtual.united(screen->geometry());
        m_snapshot = QImage(m_virtual.size(), QImage::Format_RGB32);
        m_snapshot.fill(Qt::black);
        QPainter painter(&m_snapshot);
        for (QScreen* screen : screens) {
            const QRect target(screen->geometry().topLeft() - m_virtual.topLeft(), screen->geometry().size());
            painter.drawPixmap(target, screen->grabWindow(0));
        }
        setGeometry(m_virtual);
        setCursor(Qt::CrossCursor);
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        setWindowModality(Qt::ApplicationModal);
    }

    static QColor pick(const QColor& fallback)
    {
        ScreenSamplerOverlay overlay(fallback);
        QEventLoop loop;
        QColor result;
        QObject::connect(&overlay, &ScreenSamplerOverlay::finished, &loop, [&](bool accepted, const QColor& color) {
            if (accepted) result = color;
            loop.quit();
        });
        overlay.show();
        overlay.raise();
        overlay.activateWindow();
        overlay.setFocus();
        QTimer::singleShot(0, &overlay, [&overlay] {
            overlay.grabMouse();
            overlay.grabKeyboard();
        });
        loop.exec();
        return result;
    }

signals:
    void finished(bool accepted, const QColor& color);

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.drawImage(rect(), m_snapshot);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(Qt::white, 1));
        painter.drawLine(m_position.x() - 14, m_position.y(), m_position.x() + 14, m_position.y());
        painter.drawLine(m_position.x(), m_position.y() - 14, m_position.x(), m_position.y() + 14);
        painter.setPen(QPen(Qt::black, 1));
        painter.drawEllipse(m_position, 5, 5);

        QRect bubble(m_position + QPoint(18, 18), QSize(164, 48));
        if (bubble.right() > width()) bubble.moveRight(m_position.x() - 18);
        if (bubble.bottom() > height()) bubble.moveBottom(m_position.y() - 18);
        painter.fillRect(bubble, QColor(30, 32, 36, 235));
        painter.setPen(QColor(100, 104, 111));
        painter.drawRect(bubble.adjusted(0, 0, -1, -1));
        painter.fillRect(QRect(bubble.left() + 8, bubble.top() + 8, 30, 30), m_color);
        painter.setPen(Qt::white);
        painter.drawText(bubble.adjusted(47, 7, -6, -6), Qt::AlignLeft | Qt::AlignVCenter,
            m_color.name(QColor::HexRgb).toUpper() + QStringLiteral("\nClick to select • Esc to cancel"));
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        m_position = event->position().toPoint();
        if (m_snapshot.rect().contains(m_position)) m_color = m_snapshot.pixelColor(m_position);
        update();
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton) complete(true);
        else if (event->button() == Qt::RightButton) complete(false);
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_Escape) complete(false);
        else QWidget::keyPressEvent(event);
    }

private:
    void complete(bool accepted)
    {
        releaseMouse();
        releaseKeyboard();
        hide();
        emit finished(accepted, m_color);
    }

    QRect m_virtual;
    QImage m_snapshot;
    QPoint m_position;
    QColor m_color;
};

class ColorDragSpinBox final : public QSpinBox {
public:
    explicit ColorDragSpinBox(QWidget* parent=nullptr):QSpinBox(parent){setButtonSymbols(QAbstractSpinBox::NoButtons);setFocusPolicy(Qt::StrongFocus);lineEdit()->installEventFilter(this);lineEdit()->setCursor(Qt::SizeHorCursor);}
protected:
    void wheelEvent(QWheelEvent* event) override{if(!hasFocus()){event->ignore();return;}QSpinBox::wheelEvent(event);}
    bool eventFilter(QObject* watched,QEvent* event) override{
        if(watched==lineEdit()&&event->type()==QEvent::MouseButtonPress){auto* e=static_cast<QMouseEvent*>(event);if(e->button()==Qt::LeftButton){pressed=true;dragging=false;origin=e->globalPosition().toPoint();start=value();}}
        else if(watched==lineEdit()&&event->type()==QEvent::MouseMove&&pressed){auto* e=static_cast<QMouseEvent*>(event);const int dx=e->globalPosition().toPoint().x()-origin.x();if(qAbs(dx)>=QApplication::startDragDistance())dragging=true;if(dragging){double scale=(e->modifiers()&Qt::ShiftModifier)?.1:1.0;if(e->modifiers()&Qt::ControlModifier)scale*=10;setValue(start+qRound(dx*scale));return true;}}
        else if(watched==lineEdit()&&event->type()==QEvent::MouseButtonRelease){const bool was=dragging;pressed=false;dragging=false;if(was)return true;}
        return QSpinBox::eventFilter(watched,event);
    }
private:bool pressed=false,dragging=false;QPoint origin;int start=0;
};

} // namespace

class HsvColorField final : public QWidget {
    Q_OBJECT

public:
    explicit HsvColorField(QWidget* parent = nullptr) : QWidget(parent)
    {
        setFixedSize(250, 250);
        setCursor(Qt::CrossCursor);
    }

    void setColor(const QColor& color)
    {
        const int hue = color.hsvHue();
        if (hue >= 0) m_hue = hue;
        m_saturation = color.hsvSaturationF();
        m_value = color.valueF();
        update();
    }

    void setHue(int hue) { m_hue = qBound(0, hue, 359); update(); }
    QColor color() const { return QColor::fromHsvF(m_hue / 360.0, m_saturation, m_value); }

signals:
    void colorEdited(const QColor& color);

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const int side=qMin(width(),height())-4;QImage wheel(side,side,QImage::Format_ARGB32_Premultiplied);wheel.fill(Qt::transparent);const QPointF c(side/2.0,side/2.0);const double radius=side/2.0-1;
        for(int y=0;y<side;++y)for(int x=0;x<side;++x){const double dx=x-c.x(),dy=y-c.y(),r=std::sqrt(dx*dx+dy*dy);if(r<=radius){double h=std::atan2(dy,dx)*180.0/M_PI;if(h<0)h+=360;wheel.setPixelColor(x,y,QColor::fromHsvF(h/360.0,qBound(0.0,r/radius,1.0),m_value));}}
        painter.drawImage((width()-side)/2,(height()-side)/2,wheel);
        const double angle=qDegreesToRadians(double(m_hue));const QPointF center(width()/2.0,height()/2.0);const QPointF marker=center+QPointF(std::cos(angle),std::sin(angle))*m_saturation*radius;
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(Qt::black, 3));
        painter.drawEllipse(marker, 6, 6);
        painter.setPen(QPen(Qt::white, 1));
        painter.drawEllipse(marker, 6, 6);
    }

    void mousePressEvent(QMouseEvent* event) override { updateFromPosition(event->position()); }
    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (event->buttons() & Qt::LeftButton) updateFromPosition(event->position());
    }

private:
    void updateFromPosition(const QPointF& position)
    {
        const QPointF delta=position-QPointF(width()/2.0,height()/2.0);const double radius=qMin(width(),height())/2.0-3;m_saturation=qBound(0.0,std::sqrt(delta.x()*delta.x()+delta.y()*delta.y())/radius,1.0);double hue=std::atan2(delta.y(),delta.x())*180.0/M_PI;if(hue<0)hue+=360;m_hue=qRound(hue)%360;
        update();
        emit colorEdited(color());
    }

    int m_hue = 0;
    double m_saturation = 0.0;
    double m_value = 1.0;
};

ColorPickerDialog::ColorPickerDialog(const QColor& initial, bool alphaEnabled, QWidget* parent)
    : QDialog(parent), m_original(initial.isValid() ? initial : Qt::white),
      m_current(m_original), m_alphaEnabled(alphaEnabled)
{
    setWindowTitle(tr("Choose a Color"));
    setMinimumSize(680, 445);
    setModal(true);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(12, 12, 12, 10);
    outer->setSpacing(10);
    auto* content = new QHBoxLayout();
    content->setSpacing(20);
    outer->addLayout(content, 1);

    auto* left = new QVBoxLayout();
    left->setSpacing(7);
    content->addLayout(left, 1);
    auto* previews = new QHBoxLayout();
    previews->setAlignment(Qt::AlignCenter);
    const auto previewColumn = [this, previews](QPushButton*& button, const QString& caption) {
        auto* column = new QVBoxLayout();
        column->setSpacing(2);
        button = new QPushButton(this);
        button->setFixedSize(72, 38);
        column->addWidget(button);
        auto* text = new QLabel(caption, this);
        text->setObjectName(QStringLiteral("Muted"));
        text->setAlignment(Qt::AlignCenter);
        column->addWidget(text);
        previews->addLayout(column);
    };
    previewColumn(m_previous, tr("Previous"));
    auto* arrow = new QLabel(QStringLiteral("→"), this);
    arrow->setAlignment(Qt::AlignCenter);
    previews->addWidget(arrow);
    previewColumn(m_currentSwatch, tr("Current"));
    m_previous->setToolTip(tr("Restore the color that was active when this picker opened."));
    m_currentSwatch->setEnabled(false);
    left->addLayout(previews);

    m_field = new HsvColorField(this);
    left->addWidget(m_field, 0, Qt::AlignHCenter);
    auto* paletteTitle = new QHBoxLayout();
    auto* paletteLabel = new QLabel(tr("Palette"), this);
    paletteLabel->setObjectName(QStringLiteral("SectionTitle"));
    paletteTitle->addWidget(paletteLabel);
    auto* saveCustom = new QPushButton(tr("+ Add Current"), this);
    saveCustom->setToolTip(tr("Save the current color in the rotating custom palette."));
    paletteTitle->addWidget(saveCustom, 0, Qt::AlignRight);
    left->addLayout(paletteTitle);
    auto* paletteGrid = new QGridLayout();
    paletteGrid->setSpacing(4);
    left->addLayout(paletteGrid);
    const QList<QColor> presets{
        Qt::black, QColor(64,64,64), QColor(128,128,128), QColor(192,192,192), Qt::white,
        QColor(128,0,0), Qt::red, QColor(255,128,0), Qt::yellow, QColor(128,128,0),
        QColor(0,128,0), Qt::green, QColor(0,128,128), Qt::cyan, QColor(0,0,128),
        Qt::blue, QColor(128,0,128), Qt::magenta, QColor(255,128,192), QColor(128,64,0)
    };
    for (int i = 0; i < presets.size(); ++i) {
        auto* swatch = new QPushButton(this);
        swatch->setFixedSize(28, 20);
        swatch->setStyleSheet(chipStyle(presets[i]));
        paletteGrid->addWidget(swatch, i / 10, i % 10);
        connect(swatch, &QPushButton::clicked, this, [this, color = presets[i]] {
            QColor next = color; next.setAlpha(m_current.alpha()); applyColor(next);
        });
    }

    auto* customLabel = new QLabel(tr("Custom colors"), this);customLabel->setObjectName(QStringLiteral("SectionTitle"));left->addWidget(customLabel);
    auto* customGrid = new QGridLayout();
    customGrid->setSpacing(4);
    left->addLayout(customGrid);
    QSettings settings;
    auto custom = std::make_shared<QStringList>(settings.value(QStringLiteral("ColorDialog/CustomColors")).toStringList());
    while (custom->size() < 8) custom->append(QStringLiteral("#808080"));
    QList<QPushButton*> customButtons;
    for (int i = 0; i < 8; ++i) {
        auto* swatch = new QPushButton(this);
        swatch->setFixedSize(32, 20);
        swatch->setStyleSheet(chipStyle(QColor(custom->at(i))));
        customGrid->addWidget(swatch, 0, i);
        customButtons.append(swatch);
        connect(swatch, &QPushButton::clicked, this, [this, custom, i] {
            QColor next(custom->at(i)); next.setAlpha(m_current.alpha()); applyColor(next);
        });
    }
    connect(saveCustom, &QPushButton::clicked, this, [this, custom, customButtons]() mutable {
        QSettings settings;
        const int index = settings.value(QStringLiteral("ColorDialog/NextCustomSlot"), 0).toInt() % customButtons.size();
        (*custom)[index] = m_current.name(QColor::HexRgb);
        customButtons[index]->setStyleSheet(chipStyle(m_current));
        settings.setValue(QStringLiteral("ColorDialog/CustomColors"), *custom);
        settings.setValue(QStringLiteral("ColorDialog/NextCustomSlot"), (index + 1) % customButtons.size());
    });

    auto* values = new QVBoxLayout();
    values->setSpacing(7);
    content->addLayout(values, 1);
    auto* hexRow = new QHBoxLayout();
    hexRow->addWidget(new QLabel(tr("Hex"), this));
    m_hex = new QLineEdit(this);
    m_hex->setAlignment(Qt::AlignCenter);
    m_hex->setValidator(new QRegularExpressionValidator(
        QRegularExpression(alphaEnabled ? QStringLiteral("#?[0-9A-Fa-f]{6,8}") : QStringLiteral("#?[0-9A-Fa-f]{6}")), m_hex));
    hexRow->addWidget(m_hex, 1);
    auto* copy = new QToolButton(this);
    copy->setText(tr("Copy"));
    hexRow->addWidget(copy);
    values->addLayout(hexRow);

    auto section = [this, values](const QString& title, const QString& note) {
        auto* row = new QHBoxLayout();
        auto* label = new QLabel(title, this);
        label->setObjectName(QStringLiteral("SectionTitle"));
        row->addWidget(label);
        auto* detail = new QLabel(note, this);
        detail->setObjectName(QStringLiteral("Muted"));
        row->addWidget(detail, 0, Qt::AlignRight);
        values->addLayout(row);
    };
    section(tr("RGB"), tr("0–255"));
    addValueRow(values, QStringLiteral("R"), m_redSlider, m_red, 255);
    addValueRow(values, QStringLiteral("G"), m_greenSlider, m_green, 255);
    addValueRow(values, QStringLiteral("B"), m_blueSlider, m_blue, 255);
    section(tr("HSV"), tr("Hue • Saturation • Value"));
    addValueRow(values, QStringLiteral("H"), m_hueSlider, m_hue, 359);
    addValueRow(values, QStringLiteral("S"), m_saturationSlider, m_saturation, 100);
    addValueRow(values, QStringLiteral("V"), m_valueSlider, m_value, 100);
    m_hueSlider->setStyleSheet(sliderStyle(QStringLiteral(
        "stop:0 #ff0000,stop:0.167 #ffff00,stop:0.333 #00ff00,stop:0.5 #00ffff,"
        "stop:0.667 #0000ff,stop:0.833 #ff00ff,stop:1 #ff0000")));
    m_hue->setSuffix(QStringLiteral("°"));
    m_saturation->setSuffix(QStringLiteral("%"));
    m_value->setSuffix(QStringLiteral("%"));
    if (m_alphaEnabled) {
        section(tr("Alpha"), tr("0–255"));
        addValueRow(values, QStringLiteral("A"), m_alphaSlider, m_alpha, 255);
    } else {
        m_alpha = new QSpinBox(this);
        m_alpha->setRange(255, 255);
        m_alpha->setValue(255);
        m_alpha->hide();
    }

    auto* eyedropper = new QPushButton(tr("⌾  Pick from Screen…"), this);
    eyedropper->setToolTip(tr("Hide this dialog and sample a color anywhere on any connected screen."));
    values->addWidget(eyedropper);
    auto* hint = new QLabel(tr("Click a swatch or drag the color field and channel controls."), this);
    hint->setObjectName(QStringLiteral("Muted"));
    hint->setWordWrap(true);
    values->addWidget(hint);
    values->addStretch();

    auto* buttons = new QHBoxLayout();
    buttons->addStretch();
    auto* cancel = new QPushButton(tr("Cancel"), this);
    auto* accept = new QPushButton(tr("OK"), this);
    accept->setDefault(true);
    buttons->addWidget(cancel);
    buttons->addWidget(accept);
    outer->addLayout(buttons);

    const auto link = [this](QSlider* slider, QSpinBox* spin) {
        connect(slider, &QSlider::valueChanged, spin, &QSpinBox::setValue);
        connect(spin, qOverload<int>(&QSpinBox::valueChanged), slider, &QSlider::setValue);
    };
    link(m_redSlider, m_red); link(m_greenSlider, m_green); link(m_blueSlider, m_blue);
    link(m_hueSlider, m_hue); link(m_saturationSlider, m_saturation); link(m_valueSlider, m_value);
    if (m_alphaEnabled) link(m_alphaSlider, m_alpha);

    connect(m_field, &HsvColorField::colorEdited, this, [this](QColor color) {
        color.setAlpha(m_current.alpha()); applyColor(color, false);
    });
    connect(m_hueSlider, &QSlider::valueChanged, m_field, &HsvColorField::setHue);
    const auto rgbChanged = [this] {
        if (!m_syncing) applyColor(QColor(m_red->value(), m_green->value(), m_blue->value(), m_alpha->value()));
    };
    connect(m_red, qOverload<int>(&QSpinBox::valueChanged), this, rgbChanged);
    connect(m_green, qOverload<int>(&QSpinBox::valueChanged), this, rgbChanged);
    connect(m_blue, qOverload<int>(&QSpinBox::valueChanged), this, rgbChanged);
    const auto hsvChanged = [this] {
        if (!m_syncing) applyColor(QColor::fromHsv(m_hue->value(), qRound(m_saturation->value() * 2.55),
            qRound(m_value->value() * 2.55), m_alpha->value()));
    };
    connect(m_hue, qOverload<int>(&QSpinBox::valueChanged), this, hsvChanged);
    connect(m_saturation, qOverload<int>(&QSpinBox::valueChanged), this, hsvChanged);
    connect(m_value, qOverload<int>(&QSpinBox::valueChanged), this, hsvChanged);
    if (m_alphaEnabled) connect(m_alpha, qOverload<int>(&QSpinBox::valueChanged), this, [this] {
        if (!m_syncing) { QColor next = m_current; next.setAlpha(m_alpha->value()); applyColor(next); }
    });
    connect(m_hex, &QLineEdit::editingFinished, this, [this] {
        QString text = m_hex->text().trimmed();
        if (!text.startsWith(QLatin1Char('#'))) text.prepend(QLatin1Char('#'));
        QColor next(text);
        if (next.isValid()) {
            if (!m_alphaEnabled || text.size() <= 7) next.setAlpha(m_current.alpha());
            applyColor(next);
        } else refreshControls();
    });
    connect(copy, &QToolButton::clicked, this, [this] { QApplication::clipboard()->setText(displayHex()); });
    connect(m_previous, &QPushButton::clicked, this, [this] { applyColor(m_original); });
    connect(eyedropper, &QPushButton::clicked, this, [this] {
        hide();
        QApplication::processEvents();
        QColor sampled = ScreenSamplerOverlay::pick(m_current);
        show(); raise(); activateWindow();
        if (sampled.isValid()) { sampled.setAlpha(m_current.alpha()); applyColor(sampled); }
    });
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(accept, &QPushButton::clicked, this, &QDialog::accept);

    m_previous->setStyleSheet(chipStyle(m_original));
    applyColor(m_current);
}

void ColorPickerDialog::addValueRow(QVBoxLayout* layout, const QString& label,
    QSlider*& slider, QSpinBox*& spin, int maximum)
{
    auto* row = new QHBoxLayout();
    row->setSpacing(8);
    auto* name = new QLabel(label, this);
    name->setFixedWidth(18);
    name->setAlignment(Qt::AlignCenter);
    slider = new QSlider(Qt::Horizontal, this);
    slider->setRange(0, maximum);
    spin = new ColorDragSpinBox(this);
    spin->setRange(0, maximum);
    spin->setFixedWidth(76);
    row->addWidget(name);
    row->addWidget(slider, 1);
    row->addWidget(spin);
    layout->addLayout(row);
}

void ColorPickerDialog::applyColor(const QColor& color, bool updateField)
{
    if (!color.isValid()) return;
    m_current = color;
    if (!m_alphaEnabled) m_current.setAlpha(255);
    if (updateField && m_field) m_field->setColor(m_current);
    refreshControls();
}

void ColorPickerDialog::refreshControls()
{
    m_syncing = true;
    const QList<QObject*> controls{m_hex, m_red, m_green, m_blue, m_hue, m_saturation, m_value, m_alpha,
        m_redSlider, m_greenSlider, m_blueSlider, m_hueSlider, m_saturationSlider, m_valueSlider, m_alphaSlider};
    std::vector<std::unique_ptr<QSignalBlocker>> blockers;
    blockers.reserve(static_cast<std::size_t>(controls.size()));
    for (QObject* control : controls) if (control) blockers.emplace_back(std::make_unique<QSignalBlocker>(control));
    m_hex->setText(displayHex());
    m_red->setValue(m_current.red()); m_green->setValue(m_current.green()); m_blue->setValue(m_current.blue());
    m_hue->setValue(qMax(0, m_current.hsvHue()));
    m_saturation->setValue(qRound(m_current.hsvSaturationF() * 100.0));
    m_value->setValue(qRound(m_current.valueF() * 100.0));
    m_alpha->setValue(m_current.alpha());
    m_redSlider->setValue(m_current.red()); m_greenSlider->setValue(m_current.green()); m_blueSlider->setValue(m_current.blue());
    m_hueSlider->setValue(qMax(0, m_current.hsvHue()));
    m_saturationSlider->setValue(qRound(m_current.hsvSaturationF() * 100.0));
    m_valueSlider->setValue(qRound(m_current.valueF() * 100.0));
    if (m_alphaSlider) m_alphaSlider->setValue(m_current.alpha());
    m_currentSwatch->setStyleSheet(chipStyle(m_current));
    refreshGradients();
    m_syncing = false;
}

void ColorPickerDialog::refreshGradients()
{
    m_redSlider->setStyleSheet(sliderStyle(twoStops(QColor(0, m_current.green(), m_current.blue()), QColor(255, m_current.green(), m_current.blue()))));
    m_greenSlider->setStyleSheet(sliderStyle(twoStops(QColor(m_current.red(), 0, m_current.blue()), QColor(m_current.red(), 255, m_current.blue()))));
    m_blueSlider->setStyleSheet(sliderStyle(twoStops(QColor(m_current.red(), m_current.green(), 0), QColor(m_current.red(), m_current.green(), 255))));
    const int hue = qMax(0, m_current.hsvHue());
    m_saturationSlider->setStyleSheet(sliderStyle(twoStops(QColor::fromHsv(hue, 0, m_current.value()), QColor::fromHsv(hue, 255, m_current.value()))));
    m_valueSlider->setStyleSheet(sliderStyle(twoStops(Qt::black, QColor::fromHsv(hue, m_current.hsvSaturation(), 255))));
    if (m_alphaSlider) m_alphaSlider->setStyleSheet(sliderStyle(twoStops(QColor(48,48,48), m_current)));
}

QString ColorPickerDialog::displayHex() const
{
    return m_current.name(m_alphaEnabled ? QColor::HexArgb : QColor::HexRgb).toUpper();
}

QColor ColorPickerDialog::selectedColor() const { return m_current; }

QColor ColorPickerDialog::choose(const QColor& initial, bool alphaEnabled, QWidget* parent)
{
    ColorPickerDialog dialog(initial, alphaEnabled, parent);
    return dialog.exec() == QDialog::Accepted ? dialog.selectedColor() : QColor();
}

ColorEdit::ColorEdit(const QColor& color, bool alphaEnabled, QWidget* parent)
    : QWidget(parent), m_color(color.isValid() ? color : Qt::white), m_alphaEnabled(alphaEnabled)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setToolTip(tr("Open the color picker. The picker includes exact channel values, palettes, and a screen eyedropper."));
    auto* layout=new QHBoxLayout(this);layout->setContentsMargins(0,0,0,0);layout->setSpacing(5);
    m_swatch=new QToolButton(this);m_swatch->setFixedWidth(42);m_swatch->setToolTip(tr("Open color picker"));
    m_lineEdit=new QLineEdit(this);m_lineEdit->setClearButtonEnabled(false);m_lineEdit->setAlignment(Qt::AlignCenter);layout->addWidget(m_swatch);layout->addWidget(m_lineEdit,1);
    m_lineEdit->setValidator(new QRegularExpressionValidator(QRegularExpression(m_alphaEnabled?QStringLiteral("#?[0-9A-Fa-f]{6,8}"):QStringLiteral("#?[0-9A-Fa-f]{6}")),m_lineEdit));
    connect(m_swatch, &QToolButton::clicked, this, [this] {
        const QColor selected = ColorPickerDialog::choose(m_color, m_alphaEnabled, this);
        if (!selected.isValid() || selected == m_color) return;
        m_color = selected;
        updateDisplay();
        emit colorChanged(m_color);
    });
    connect(m_lineEdit,&QLineEdit::editingFinished,this,[this]{QString text=m_lineEdit->text().trimmed();if(!text.startsWith('#'))text.prepend('#');QColor next(text);if(!next.isValid()){updateDisplay();return;}if(!m_alphaEnabled)next.setAlpha(255);if(next!=m_color){m_color=next;updateDisplay();emit colorChanged(m_color);}});
    updateDisplay();
}

QColor ColorEdit::color() const { return m_color; }

void ColorEdit::setColor(const QColor& color)
{
    if (!color.isValid()) return;
    m_color = color;
    if (!m_alphaEnabled) m_color.setAlpha(255);
    updateDisplay();
}

void ColorEdit::updateDisplay()
{
    QPixmap swatch(36, 18);
    swatch.fill(Qt::transparent);
    QPainter painter(&swatch);
    const int tile = 6;
    for (int y = 0; y < swatch.height(); y += tile)
        for (int x = 0; x < swatch.width(); x += tile)
            painter.fillRect(x, y, tile, tile, ((x / tile + y / tile) & 1) ? QColor(80, 82, 86) : QColor(125, 128, 133));
    painter.fillRect(swatch.rect(), m_color);
    painter.setPen(QColor(210, 212, 216));
    painter.drawRect(swatch.rect().adjusted(0, 0, -1, -1));
    m_swatch->setIcon(QIcon(swatch));
    m_swatch->setIconSize(swatch.size());
    m_lineEdit->setText(m_color.name(m_alphaEnabled ? QColor::HexArgb : QColor::HexRgb).toUpper());
}

#include "colorpicker.moc"
