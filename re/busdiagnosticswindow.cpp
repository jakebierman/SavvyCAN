#include "busdiagnosticswindow.h"

#include "connections/canconmanager.h"

#include <QDateTime>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTextEdit>
#include <QVBoxLayout>

BusDiagnosticsWindow::BusDiagnosticsWindow(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("Bus Diagnostics"));
    QVBoxLayout *layout = new QVBoxLayout(this);
    busTable = new QTableWidget(0, 11, this);
    busTable->setHorizontalHeaderLabels({tr("Bus"), tr("Controller"), tr("Bitrate"), tr("Load"),
        tr("Frames"), tr("Errors"), tr("Bus off"), tr("Missing ACK"), tr("Protocol"),
        tr("Arbitration"), tr("TEC / REC")});
    busTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    busTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    layout->addWidget(busTable);
    QPushButton *reset = new QPushButton(tr("Reset counters"), this);
    layout->addWidget(reset, 0, Qt::AlignLeft);
    layout->addWidget(new QLabel(tr("Decoded error-frame history"), this));
    errorHistory = new QTextEdit(this);
    errorHistory->setReadOnly(true);
    layout->addWidget(errorHistory, 1);
    connect(reset, &QPushButton::clicked, this, &BusDiagnosticsWindow::resetCounters);
    connect(CANConManager::getInstance(), &CANConManager::framesReceived,
            this, &BusDiagnosticsWindow::processFrames);
    connect(&refreshTimer, &QTimer::timeout, this, &BusDiagnosticsWindow::refresh);
    refreshTimer.start(1000);
    rebuildRows();
}

void BusDiagnosticsWindow::rebuildRows()
{
    const int buses = CANConManager::getInstance()->getNumBuses();
    busTable->setRowCount(buses);
    for (int bus = 0; bus < buses; ++bus)
    {
        stats[bus];
        for (int column = 0; column < busTable->columnCount(); ++column)
            if (!busTable->item(bus, column)) busTable->setItem(bus, column, new QTableWidgetItem);
        busTable->item(bus, 0)->setText(QString::number(bus));
    }
}

void BusDiagnosticsWindow::processFrames(CANConnection *, QVector<CANFrame> &frames)
{
    for (const CANFrame &frame : frames)
    {
        BusStats &bus = stats[frame.bus];
        ++bus.frames;
        const int payloadBits = frame.payload().size() * 8;
        const int overhead = frame.hasExtendedFrameFormat() ? 67 : 47;
        const quint64 estimatedBits = quint64((payloadBits + overhead) * 1.2);
        bus.bitsThisInterval += estimatedBits;
        bus.totalBits += estimatedBits;
        if (frame.frameType() != QCanBusFrame::ErrorFrame) continue;
        ++bus.errors;
        const auto error = frame.error();
        if (error & QCanBusFrame::BusOffError) ++bus.busOff;
        if (error & QCanBusFrame::MissingAcknowledgmentError) ++bus.missingAck;
        if (error & QCanBusFrame::ProtocolViolationError) ++bus.protocol;
        if (error & QCanBusFrame::LostArbitrationError) ++bus.arbitration;
        if (error & QCanBusFrame::ControllerError) ++bus.controller;
        bus.lastError = decodeError(frame);
        bus.lastErrorMs = QDateTime::currentMSecsSinceEpoch();
        errorHistory->append(QStringLiteral("%1  Bus %2  %3  data %4")
            .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs))
            .arg(frame.bus).arg(bus.lastError, QString(frame.payload().toHex(' ')).toUpper()));
    }
}

QString BusDiagnosticsWindow::decodeError(const CANFrame &frame) const
{
    QStringList errors;
    const auto error = frame.error();
    if (error & QCanBusFrame::TransmissionTimeoutError) errors << tr("TX timeout");
    if (error & QCanBusFrame::LostArbitrationError) errors << tr("lost arbitration");
    if (error & QCanBusFrame::ControllerError) errors << tr("controller error");
    if (error & QCanBusFrame::ProtocolViolationError) errors << tr("protocol violation");
    if (error & QCanBusFrame::TransceiverError) errors << tr("transceiver error");
    if (error & QCanBusFrame::MissingAcknowledgmentError) errors << tr("missing ACK");
    if (error & QCanBusFrame::BusOffError) errors << tr("bus off");
    if (error & QCanBusFrame::BusError) errors << tr("bus error");
    if (error & QCanBusFrame::ControllerRestartError) errors << tr("controller restarted");
    if (error & QCanBusFrame::UnknownError) errors << tr("unknown error");
    return errors.isEmpty() ? tr("unspecified CAN error") : errors.join(QStringLiteral(", "));
}

void BusDiagnosticsWindow::refresh()
{
    if (busTable->rowCount() != CANConManager::getInstance()->getNumBuses()) rebuildRows();
    const QList<CANConnection *> connections = CANConManager::getInstance()->getConnections();
    for (int bus = 0; bus < busTable->rowCount(); ++bus)
    {
        CANConnection *owner = nullptr;
        int localBus = -1;
        for (CANConnection *connection : connections)
        {
            const int base = CANConManager::getInstance()->getBusBase(connection);
            if (bus >= base && bus < base + connection->getNumBuses()) {
                owner = connection; localBus = bus - base; break;
            }
        }
        CANBus config;
        const bool configured = owner && owner->getBusSettings(localBus, config);
        BusStats &value = stats[bus];
        const int bitrate = configured ? config.getSpeed() : 0;
        value.load = bitrate > 0 ? qMin(100.0, value.bitsThisInterval * 100.0 / bitrate) : 0.0;
        value.bitsThisInterval = 0;
        QString state = owner ? (owner->getStatus() == CANCon::CONNECTED ? tr("Connected") : tr("Disconnected"))
                              : tr("No adapter");
        if (configured) {
            if (!config.isActive()) state += tr(", disabled");
            else if (config.isListenOnly()) state += tr(", listen-only");
            else state += tr(", active");
        }
        if (value.lastError.contains(tr("bus off"), Qt::CaseInsensitive)) state += tr(", BUS OFF observed");
        busTable->item(bus, 1)->setText(state);
        busTable->item(bus, 2)->setText(bitrate ? tr("%1 bit/s").arg(bitrate) : tr("Unknown"));
        busTable->item(bus, 3)->setText(bitrate ? tr("%1%").arg(value.load, 0, 'f', 1) : tr("N/A"));
        busTable->item(bus, 4)->setText(QString::number(value.frames));
        busTable->item(bus, 5)->setText(QString::number(value.errors));
        busTable->item(bus, 6)->setText(QString::number(value.busOff));
        busTable->item(bus, 7)->setText(QString::number(value.missingAck));
        busTable->item(bus, 8)->setText(QString::number(value.protocol));
        busTable->item(bus, 9)->setText(QString::number(value.arbitration));
        busTable->item(bus, 10)->setText(tr("Not exposed by adapter API"));
    }
}

void BusDiagnosticsWindow::resetCounters()
{
    stats.clear();
    errorHistory->clear();
    rebuildRows();
    refresh();
}
