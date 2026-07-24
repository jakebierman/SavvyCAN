#ifndef BUSDIAGNOSTICSWINDOW_H
#define BUSDIAGNOSTICSWINDOW_H

#include <QDialog>
#include <QMap>
#include <QTimer>

#include "can_structs.h"

class CANConnection;
class QTableWidget;
class QTextEdit;

class BusDiagnosticsWindow : public QDialog
{
    Q_OBJECT

public:
    explicit BusDiagnosticsWindow(QWidget *parent = nullptr);

private slots:
    void processFrames(CANConnection *connection, QVector<CANFrame> &frames);
    void refresh();
    void resetCounters();

private:
    struct BusStats {
        quint64 frames = 0;
        quint64 bitsThisInterval = 0;
        quint64 totalBits = 0;
        quint64 errors = 0;
        quint64 busOff = 0;
        quint64 missingAck = 0;
        quint64 protocol = 0;
        quint64 arbitration = 0;
        quint64 controller = 0;
        double load = 0.0;
        QString lastError;
        qint64 lastErrorMs = 0;
    };

    QString decodeError(const CANFrame &frame) const;
    void rebuildRows();

    QTableWidget *busTable;
    QTextEdit *errorHistory;
    QMap<int, BusStats> stats;
    QTimer refreshTimer;
};

#endif
