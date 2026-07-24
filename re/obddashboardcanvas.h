#ifndef OBDDASHBOARDCANVAS_H
#define OBDDASHBOARDCANVAS_H

#include <QByteArray>
#include <QVector>
#include <QWidget>
#include <functional>

class OBDDashboardTile;

class OBDDashboardCanvas : public QWidget
{
public:
    struct WidgetConfig
    {
        QString type = QStringLiteral("digital");
        int pid = 0x0C;
        QString title;
        QString format;
        double minimum = 0.0;
        double maximum = 100.0;
        int x = 0;
        int y = 0;
        int width = 3;
        int height = 2;
    };

    explicit OBDDashboardCanvas(QWidget *parent = nullptr);

    void setEditMode(bool enabled);
    bool editMode() const;
    void setColumns(int columns);
    int columns() const;
    void addWidgetConfig(const WidgetConfig &config);
    void removeSelected();
    WidgetConfig *selectedConfig();
    const QVector<WidgetConfig> &configs() const;
    void setConfigs(const QVector<WidgetConfig> &configs);
    void updatePid(int pid, const QByteArray &payload, const QString &fallbackText);
    void setEditHandler(const std::function<void(WidgetConfig *)> &handler);
    void selectTile(OBDDashboardTile *tile);
    void editTile(OBDDashboardTile *tile);
    void moveTile(OBDDashboardTile *tile, const QPoint &globalPosition, bool resizing);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void rebuild();
    void layoutTiles();
    int cellWidth() const;

    QVector<WidgetConfig> widgetConfigs;
    QVector<OBDDashboardTile *> tiles;
    OBDDashboardTile *selected = nullptr;
    bool editing = false;
    int gridColumns = 12;
    int gridRowHeight = 72;
    std::function<void(WidgetConfig *)> editHandler;
};

#endif
