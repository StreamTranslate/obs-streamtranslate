#include <QApplication>
#include <QClipboard>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QToolTip>

#include <array>

namespace {
enum class Icon { Eye, HiddenEye, Copy };

QIcon drawIcon(Icon icon, const QColor &color)
{
    QPixmap pixmap(32, 32);
    pixmap.setDevicePixelRatio(2);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, 1.35, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (icon == Icon::Copy) {
        painter.drawRoundedRect(QRectF(5.5, 5.5, 8.5, 9), 1.1, 1.1);
        QPainterPath back(QPointF(3.5, 11.5));
        back.quadTo(2, 11.5, 2, 10);
        back.lineTo(2, 3.5);
        back.quadTo(2, 2, 3.5, 2);
        back.lineTo(9, 2);
        back.quadTo(10.5, 2, 10.5, 3.5);
        painter.drawPath(back);
    } else {
        QPainterPath eye(QPointF(1, 8));
        eye.quadTo(8, -0.5, 15, 8);
        eye.quadTo(8, 16.5, 1, 8);
        painter.drawPath(eye);
        painter.drawEllipse(QPointF(8, 8), 2, 2);
        if (icon == Icon::HiddenEye)
            painter.drawLine(QPointF(2.5, 2.5), QPointF(13.5, 13.5));
    }
    painter.end();
    return QIcon(pixmap);
}

void compactKeyRow(QHBoxLayout &row, QLineEdit &key, QPushButton &show)
{
    const auto refreshEye = [&show](bool visible) {
        const auto label = visible ? QStringLiteral("Hide plugin key") : QStringLiteral("Show plugin key");
        show.setText({});
        show.setAccessibleName(label);
        show.setToolTip(label);
        show.setIcon(drawIcon(visible ? Icon::Eye : Icon::HiddenEye, show.palette().buttonText().color()));
    };
    refreshEye(show.isChecked());
    QObject::connect(&show, &QPushButton::toggled, &show, refreshEye);
    show.setIconSize(QSize(16, 16));
    show.setFixedWidth(34);

    auto *copy = new QPushButton(&key);
    copy->setFixedWidth(34);
    copy->setIconSize(QSize(16, 16));
    copy->setIcon(drawIcon(Icon::Copy, copy->palette().buttonText().color()));
    copy->setAccessibleName(QStringLiteral("Copy plugin key"));
    copy->setToolTip(QStringLiteral("Copy plugin key"));
    copy->setEnabled(!key.text().isEmpty());
    QObject::connect(&key, &QLineEdit::textChanged, copy, [copy](const QString &text) {
        copy->setEnabled(!text.isEmpty());
    });
    QObject::connect(copy, &QPushButton::clicked, &key, [copy, &key] {
        QApplication::clipboard()->setText(key.text());
        QToolTip::showText(copy->mapToGlobal(QPoint(0, copy->height())), QStringLiteral("Plugin key copied"), copy);
    });
    row.setStretchFactor(&key, 1);
    row.addWidget(copy);
}

void compactActionRows(QFormLayout &form)
{
    std::array<QPushButton *, 3> buttons{};
    for (int row = 0; row < form.rowCount(); ++row) {
        auto *item = form.itemAt(row, QFormLayout::FieldRole);
        auto *button = item ? qobject_cast<QPushButton *>(item->widget()) : nullptr;
        if (!button)
            continue;
        const auto label = button->text();
        if (label == QStringLiteral("Diagnostics") || label == QStringLiteral("Hide diagnostics"))
            buttons[0] = button;
        else if (label == QStringLiteral("Start audio") || label == QStringLiteral("Pause audio"))
            buttons[1] = button;
        else if (label == QStringLiteral("Test server"))
            buttons[2] = button;
    }
    if (!buttons[0] || !buttons[1] || !buttons[2])
        return;

    buttons[0]->setToolTip(QStringLiteral("Show connection and audio details."));
    buttons[1]->setToolTip(buttons[1]->text() == QStringLiteral("Start audio")
        ? QStringLiteral("Resume caption audio from this filter.")
        : QStringLiteral("Pause caption audio from this filter."));
    buttons[2]->setToolTip(QStringLiteral("Check whether the server is reachable. Audio keeps its current state."));

    int row = 0;
    QFormLayout::ItemRole role;
    form.getWidgetPosition(buttons[0], &row, &role);
    auto *actions = new QHBoxLayout;
    for (auto *button : buttons) {
        const auto taken = form.takeRow(button);
        delete taken.labelItem;
        delete taken.fieldItem;
        actions->addWidget(button);
    }
    actions->addStretch();
    form.insertRow(row, actions);
}

void compactPanel(QWidget &panel)
{
    if (panel.property("stCompactProperties").toBool())
        return;
    auto *form = qobject_cast<QFormLayout *>(panel.layout());
    if (!form)
        return;
    for (int row = 0; row < form->rowCount(); ++row) {
        auto *item = form->itemAt(row, QFormLayout::FieldRole);
        auto *layout = item ? qobject_cast<QHBoxLayout *>(item->layout()) : nullptr;
        if (!layout || layout->count() != 2)
            continue;
        auto *key = qobject_cast<QLineEdit *>(layout->itemAt(0)->widget());
        auto *show = qobject_cast<QPushButton *>(layout->itemAt(1)->widget());
        // OBS builds the password row. Its help text identifies only this filter.
        if (!key || !show || key->toolTip() != QStringLiteral("Paste your StreamTranslate plugin key."))
            continue;

        compactKeyRow(*layout, *key, *show);
        compactActionRows(*form);
        panel.setProperty("stCompactProperties", true);
        return;
    }
}

class CompactProperties final : public QObject {
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() == QEvent::Show && object->objectName() == QStringLiteral("PropertiesContainer")) {
            QPointer<QWidget> panel = qobject_cast<QWidget *>(object);
            QTimer::singleShot(0, this, [panel] {
                if (panel)
                    compactPanel(*panel);
            });
        }
        return false;
    }
};

CompactProperties *propertiesUi;
}

extern "C" void st_compact_ui_start(void)
{
    if (!qApp || propertiesUi)
        return;
    propertiesUi = new CompactProperties;
    qApp->installEventFilter(propertiesUi);
}

extern "C" void st_compact_ui_stop(void)
{
    delete propertiesUi;
    propertiesUi = nullptr;
}
