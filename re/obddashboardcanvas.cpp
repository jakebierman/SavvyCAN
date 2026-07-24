#include "obddashboardcanvas.h"

#include "payloadformatter.h"

#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QtMath>

class OBDDashboardTile : public QWidget
{
public:
    OBDDashboardTile(OBDDashboardCanvas *canvas, OBDDashboardCanvas::WidgetConfig *config)
        : QWidget(canvas), owner(canvas), item(config)
    {
        setMinimumSize(80, 60);
        setMouseTracking(true);
    }

    OBDDashboardCanvas::WidgetConfig *config() const { return item; }
    void setSelected(bool value) { selected = value; update(); }

    void updateValue(const QByteArray &payload, const QString &fallback)
    {
        displayText = fallback;
        numeric = false;
        if (!item->format.trimmed().isEmpty())
        {
            PayloadFormatter formatter;
            QString error;
            if (formatter.compile(item->format, &error))
            {
                const QVector<PayloadFormatter::FormattedField> fields = formatter.formatFields(payload);
                if (!fields.isEmpty())
                {
                    bool ok = false;
                    number = fields.first().value.toDouble(&ok);
                    numeric = ok;
                    displayText = fields.first().value;
                    if (!fields.first().unit.isEmpty()) displayText += QStringLiteral(" ") + fields.first().unit;
                }
            }
            else displayText = error;
        }
        update();
    }

protected:
    void contextMenuEvent(QContextMenuEvent *event) override
    {
        owner->selectTile(this);
        owner->editTile(this);
        event->accept();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (!owner->editMode() || event->button() != Qt::LeftButton) return;
        owner->selectTile(this);
        resizing = event->pos().x() >= width() - 18 && event->pos().y() >= height() - 18;
        dragOffset = event->globalPos() - (resizing ? mapToGlobal(rect().bottomRight()) : mapToGlobal(QPoint()));
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!owner->editMode() || !(event->buttons() & Qt::LeftButton)) return;
        owner->moveTile(this, event->globalPos() - dragOffset, resizing);
        event->accept();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF bounds = rect().adjusted(1, 1, -2, -2);
        painter.setPen(QPen(selected ? palette().highlight().color() : palette().mid().color(), selected ? 2 : 1));
        painter.setBrush(palette().base());
        painter.drawRoundedRect(bounds, 5, 5);

        const QString title = item->title.isEmpty()
            ? QStringLiteral("PID 0x%1").arg(item->pid, 2, 16, QLatin1Char('0')).toUpper() : item->title;
        painter.setPen(palette().text().color());
        QFont titleFont = painter.font();
        titleFont.setBold(true);
        painter.setFont(titleFont);
        painter.drawText(QRect(10, 7, width() - 20, 22), Qt::AlignLeft | Qt::AlignVCenter, title);

        QRect content = rect().adjusted(10, 32, -10, -10);
        if (item->type == QStringLiteral("level"))
        {
            const double ratio = numeric && item->maximum > item->minimum
                ? qBound(0.0, (number - item->minimum) / (item->maximum - item->minimum), 1.0) : 0.0;
            painter.setPen(palette().mid().color());
            painter.setBrush(palette().alternateBase());
            painter.drawRect(content);
            QRect fill = content;
            fill.setTop(content.bottom() - qRound(content.height() * ratio));
            painter.setBrush(palette().highlight());
            painter.drawRect(fill);
            painter.setPen(palette().text().color());
            painter.drawText(content, Qt::AlignCenter, displayText);
        }
        else if (item->type == QStringLiteral("gauge"))
        {
            const double ratio = numeric && item->maximum > item->minimum
                ? qBound(0.0, (number - item->minimum) / (item->maximum - item->minimum), 1.0) : 0.0;
            QRectF gauge = content;
            gauge.setHeight(qMin(gauge.height(), gauge.width() * 0.62));
            painter.setPen(QPen(palette().mid().color(), 8, Qt::SolidLine, Qt::RoundCap));
            painter.drawArc(gauge, 225 * 16, -270 * 16);
            painter.setPen(QPen(palette().highlight().color(), 8, Qt::SolidLine, Qt::RoundCap));
            painter.drawArc(gauge, 225 * 16, qRound(-270 * 16 * ratio));
            painter.setPen(palette().text().color());
            painter.drawText(content, Qt::AlignHCenter | Qt::AlignBottom, displayText);
        }
        else
        {
            QFont valueFont = painter.font();
            valueFont.setBold(item->type == QStringLiteral("digital"));
            if (item->type == QStringLiteral("digital")) valueFont.setPointSize(valueFont.pointSize() + 5);
            painter.setFont(valueFont);
            painter.drawText(content, Qt::AlignCenter | Qt::TextWordWrap, displayText);
        }

        if (owner->editMode())
        {
            painter.setBrush(palette().highlight());
            painter.setPen(Qt::NoPen);
            painter.drawRect(width() - 13, height() - 13, 8, 8);
        }
    }

private:
    OBDDashboardCanvas *owner;
    OBDDashboardCanvas::WidgetConfig *item;
    QString displayText = QStringLiteral("Waiting for data");
    double number = 0.0;
    bool numeric = false;
    bool selected = false;
    bool resizing = false;
    QPoint dragOffset;
};

OBDDashboardCanvas::OBDDashboardCanvas(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(500);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::MinimumExpanding);
}

void OBDDashboardCanvas::setEditMode(bool enabled)
{
    editing = enabled;
    if (!editing) selectTile(nullptr);
    update();
    for (OBDDashboardTile *tile : tiles) tile->update();
}

bool OBDDashboardCanvas::editMode() const { return editing; }
void OBDDashboardCanvas::setColumns(int columns) { gridColumns = qMax(1, columns); layoutTiles(); update(); }
int OBDDashboardCanvas::columns() const { return gridColumns; }
const QVector<OBDDashboardCanvas::WidgetConfig> &OBDDashboardCanvas::configs() const { return widgetConfigs; }

void OBDDashboardCanvas::addWidgetConfig(const WidgetConfig &config)
{
    widgetConfigs.append(config);
    rebuild();
    if (!tiles.isEmpty()) selectTile(tiles.last());
}

void OBDDashboardCanvas::removeSelected()
{
    const int index = tiles.indexOf(selected);
    if (index < 0) return;
    widgetConfigs.removeAt(index);
    rebuild();
}

OBDDashboardCanvas::WidgetConfig *OBDDashboardCanvas::selectedConfig()
{
    const int index = tiles.indexOf(selected);
    return index >= 0 ? &widgetConfigs[index] : nullptr;
}

void OBDDashboardCanvas::setConfigs(const QVector<WidgetConfig> &configs)
{
    widgetConfigs = configs;
    rebuild();
}

void OBDDashboardCanvas::updatePid(int pid, const QByteArray &payload, const QString &fallbackText)
{
    for (int i = 0; i < tiles.size(); ++i)
        if (widgetConfigs.at(i).pid == pid) tiles.at(i)->updateValue(payload, fallbackText);
}

void OBDDashboardCanvas::setEditHandler(const std::function<void(WidgetConfig *)> &handler)
{
    editHandler = handler;
}

void OBDDashboardCanvas::selectTile(OBDDashboardTile *tile)
{
    selected = tile;
    for (OBDDashboardTile *candidate : tiles) candidate->setSelected(candidate == selected);
}

void OBDDashboardCanvas::editTile(OBDDashboardTile *tile)
{
    const int index = tiles.indexOf(tile);
    if (index >= 0 && editHandler) editHandler(&widgetConfigs[index]);
}

void OBDDashboardCanvas::moveTile(OBDDashboardTile *tile, const QPoint &globalPosition, bool resizing)
{
    const int index = tiles.indexOf(tile);
    if (index < 0) return;
    WidgetConfig &config = widgetConfigs[index];
    const QPoint local = mapFromGlobal(globalPosition);
    if (resizing)
    {
        config.width = qBound(1, qRound(double(local.x() - tile->x()) / cellWidth()), gridColumns - config.x);
        config.height = qMax(1, qRound(double(local.y() - tile->y()) / gridRowHeight));
    }
    else
    {
        config.x = qBound(0, qRound(double(local.x()) / cellWidth()), gridColumns - config.width);
        config.y = qMax(0, qRound(double(local.y()) / gridRowHeight));
    }
    layoutTiles();
}

void OBDDashboardCanvas::paintEvent(QPaintEvent *)
{
    if (!editing) return;
    QPainter painter(this);
    painter.setPen(QPen(palette().mid().color(), 1, Qt::DotLine));
    for (int column = 1; column < gridColumns; ++column)
        painter.drawLine(column * cellWidth(), 0, column * cellWidth(), height());
    for (int y = gridRowHeight; y < height(); y += gridRowHeight) painter.drawLine(0, y, width(), y);
}

void OBDDashboardCanvas::resizeEvent(QResizeEvent *) { layoutTiles(); }
int OBDDashboardCanvas::cellWidth() const { return qMax(1, width() / gridColumns); }

void OBDDashboardCanvas::rebuild()
{
    qDeleteAll(tiles);
    tiles.clear();
    selected = nullptr;
    for (WidgetConfig &config : widgetConfigs) tiles.append(new OBDDashboardTile(this, &config));
    layoutTiles();
    for (OBDDashboardTile *tile : tiles) tile->show();
}

void OBDDashboardCanvas::layoutTiles()
{
    int rows = 7;
    for (int i = 0; i < tiles.size(); ++i)
    {
        WidgetConfig &config = widgetConfigs[i];
        config.width = qBound(1, config.width, gridColumns);
        config.x = qBound(0, config.x, gridColumns - config.width);
        config.height = qMax(1, config.height);
        config.y = qMax(0, config.y);
        tiles[i]->setGeometry(config.x * cellWidth() + 3, config.y * gridRowHeight + 3,
                              config.width * cellWidth() - 6, config.height * gridRowHeight - 6);
        rows = qMax(rows, config.y + config.height + 1);
    }
    setMinimumHeight(rows * gridRowHeight);
}
