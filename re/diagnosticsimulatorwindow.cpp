#include "diagnosticsimulatorwindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>
#include "connections/canconmanager.h"

namespace {
QString hexId(quint32 id)
{
    return QStringLiteral("0x%1").arg(id, id > 0x7FF ? 8 : 3, 16, QLatin1Char('0')).toUpper();
}

}

VirtualDiagnosticConnection::VirtualDiagnosticConnection(QObject *owner)
    : CANConnection(QStringLiteral("Diagnostic Simulator"), QStringLiteral("virtual"),
                    CANCon::SIMULATOR, 0, 500000, false, 0, 1, 65536, false),
      mOwner(owner)
{
    CANBus bus;
    bus.setSpeed(500000);
    setBusConfig(0, bus);
}

void VirtualDiagnosticConnection::setEcus(const QList<DiagnosticSimEcu> &ecus) { mEcus = ecus; }
QList<DiagnosticSimEcu> VirtualDiagnosticConnection::ecus() const { return mEcus; }
void VirtualDiagnosticConnection::setFault(const DiagnosticSimFault &fault) { mFault = fault; }
DiagnosticSimFault VirtualDiagnosticConnection::fault() const { return mFault; }

void VirtualDiagnosticConnection::resetState()
{
    for (DiagnosticSimEcu &ecu : mEcus) {
        ecu.session = 1;
        ecu.securityLevel = 0;
    }
    mMultiFrameRequests.clear();
    mMultiFrameLengths.clear();
    mFault.counter = 0;
    emit ecuStateChanged();
}

void VirtualDiagnosticConnection::piStarted()
{
    setStatus(CANCon::CONNECTED);
    emit activity(tr("Virtual diagnostic bus connected"));
}

void VirtualDiagnosticConnection::piStop()
{
    setStatus(CANCon::NOT_CONNECTED);
    emit activity(tr("Virtual diagnostic bus disconnected"));
}

void VirtualDiagnosticConnection::piSetBusSettings(int bus, CANBus settings)
{
    setBusConfig(bus, settings);
}

bool VirtualDiagnosticConnection::piGetBusSettings(int bus, CANBus &settings)
{
    return getBusConfig(bus, settings);
}

void VirtualDiagnosticConnection::piSuspend(bool suspend) { setCapSuspended(suspend); }

QByteArray VirtualDiagnosticConnection::decodeIsoTp(const CANFrame &frame)
{
    const QByteArray data = frame.payload();
    if (data.isEmpty()) return {};
    const quint8 pci = quint8(data[0]);
    if ((pci & 0xF0) == 0x00) return data.mid(1, pci & 0x0F);
    if ((pci & 0xF0) == 0x10) {
        const int length = ((pci & 0x0F) << 8) | quint8(data.at(1));
        QByteArray &buffer = mMultiFrameRequests[frame.frameId()];
        buffer = data.mid(2);
        mMultiFrameLengths[frame.frameId()] = length;
        return QByteArray();
    }
    if ((pci & 0xF0) == 0x20) {
        QByteArray &buffer = mMultiFrameRequests[frame.frameId()];
        buffer.append(data.mid(1));
        const int length = mMultiFrameLengths.value(frame.frameId());
        if (length <= 0 || buffer.size() < length) return QByteArray();
        buffer.truncate(length);
        const QByteArray complete = buffer;
        mMultiFrameRequests.remove(frame.frameId());
        mMultiFrameLengths.remove(frame.frameId());
        return complete;
    }
    return {};
}

DiagnosticSimValue *VirtualDiagnosticConnection::findValue(
    DiagnosticSimEcu &ecu, const QString &kind, const QString &key)
{
    for (DiagnosticSimValue &value : ecu.values)
        if (value.kind.compare(kind, Qt::CaseInsensitive) == 0 &&
            value.key.compare(key, Qt::CaseInsensitive) == 0)
            return &value;
    return nullptr;
}

QByteArray VirtualDiagnosticConnection::udsResponse(DiagnosticSimEcu &ecu,
                                                     const QByteArray &request)
{
    if (request.isEmpty()) return {};
    const quint8 service = quint8(request[0]);
    auto negative = [service](int nrc) {
        return QByteArray::fromRawData("\x7F", 1) + char(service) + char(nrc);
    };
    if (service == 0x10 && request.size() >= 2) {
        ecu.session = quint8(request[1]) & 0x7F;
        emit ecuStateChanged();
        return QByteArray(1, char(0x50)) + char(ecu.session) +
               QByteArray::fromHex("003200C8");
    }
    if (service == 0x3E)
        return QByteArray::fromHex("7E00");
    if (service == 0x11) {
        resetState();
        return QByteArray::fromHex("5101");
    }
    if (service == 0x27 && request.size() >= 2) {
        const int sub = quint8(request[1]);
        if (sub & 1) return QByteArray(1, char(0x67)) + char(sub) + QByteArray::fromHex("12345678");
        ecu.securityLevel = qMax(1, sub / 2);
        emit ecuStateChanged();
        return QByteArray(1, char(0x67)) + char(sub);
    }
    if ((service == 0x22 || service == 0x2E) && request.size() >= 3) {
        const QString key = QStringLiteral("%1").arg(
            (quint8(request[1]) << 8) | quint8(request[2]), 4, 16, QLatin1Char('0')).toUpper();
        DiagnosticSimValue *value = findValue(ecu, QStringLiteral("DID"), key);
        if (!value) return negative(0x31);
        if (service == 0x2E) {
            if (!value->writable) return negative(0x33);
            value->value = request.mid(3);
            emit ecuStateChanged();
            return QByteArray(1, char(0x6E)) + request.mid(1, 2);
        }
        return QByteArray(1, char(0x62)) + request.mid(1, 2) + value->value;
    }
    if (service == 0x19) {
        DiagnosticSimValue *dtcs = findValue(ecu, QStringLiteral("DTC"), QStringLiteral("ALL"));
        return QByteArray::fromHex("5902FF") + (dtcs ? dtcs->value : QByteArray());
    }
    if (service == 0x14)
        return QByteArray(1, char(0x54));
    if (service == 0x31 && request.size() >= 4)
        return QByteArray(1, char(0x71)) + request.mid(1, 3);
    if (service == 0x28 || service == 0x85 || service == 0x2F)
        return QByteArray(1, char(service + 0x40)) + request.mid(1);
    if (service >= 0x34 && service <= 0x37)
        return QByteArray(1, char(service + 0x40)) + (service == 0x34 ? QByteArray::fromHex("200400") : request.mid(1, 1));
    return negative(0x11);
}

QByteArray VirtualDiagnosticConnection::obdResponse(DiagnosticSimEcu &ecu,
                                                     const QByteArray &request)
{
    if (request.size() < 2) return {};
    const int mode = quint8(request[0]);
    const int pid = quint8(request[1]);
    const QString key = QStringLiteral("%1:%2")
        .arg(mode, 2, 16, QLatin1Char('0')).arg(pid, 2, 16, QLatin1Char('0')).toUpper();
    DiagnosticSimValue *value = findValue(ecu, QStringLiteral("PID"), key);
    if (!value) return QByteArray();
    return QByteArray(1, char(mode + 0x40)) + char(pid) + value->value;
}

void VirtualDiagnosticConnection::enqueueFrame(CANFrame frame, int delay)
{
    QTimer::singleShot(qMax(0, delay), this, [this, frame]() mutable {
        if (getStatus() != CANCon::CONNECTED || isCapSuspended()) return;
        CANFrame *slot = getQueue().get();
        if (!slot) return;
        frame.isReceived = true;
        frame.bus = 0;
        frame.setTimeStamp(QCanBusFrame::TimeStamp::fromMicroSeconds(
            QDateTime::currentMSecsSinceEpoch() * 1000));
        *slot = frame;
        getQueue().queue();
        checkTargettedFrame(frame);
    });
}

void VirtualDiagnosticConnection::enqueueResponse(const DiagnosticSimEcu &ecu,
                                                   const QByteArray &payload, int delay)
{
    if (payload.isEmpty()) return;
    QList<QByteArray> packets;
    if (payload.size() <= 7) {
        QByteArray packet(1, char(payload.size()));
        packet += payload;
        packet.append(QByteArray(8 - packet.size(), char(0)));
        packets << packet;
    } else {
        QByteArray first;
        first += char(0x10 | ((payload.size() >> 8) & 0x0F));
        first += char(payload.size() & 0xFF);
        first += payload.left(6);
        first.append(QByteArray(8 - first.size(), char(0)));
        packets << first;
        int offset = 6;
        int sequence = 1;
        while (offset < payload.size()) {
            QByteArray part(1, char(0x20 | (sequence++ & 0x0F)));
            part += payload.mid(offset, 7);
            part.append(QByteArray(8 - part.size(), char(0)));
            packets << part;
            offset += 7;
        }
    }
    for (int i = 0; i < packets.size(); ++i) {
        CANFrame response;
        response.setFrameId(ecu.responseId);
        response.setExtendedFrameFormat(ecu.extended);
        response.setPayload(packets[i]);
        enqueueFrame(response, delay + i * 3);
    }
}

bool VirtualDiagnosticConnection::piSendFrame(const CANFrame &frame)
{
    const QByteArray raw = frame.payload();
    if (!raw.isEmpty() && (quint8(raw[0]) & 0xF0) == 0x10) {
        decodeIsoTp(frame);
        for (const DiagnosticSimEcu &ecu : mEcus) {
            if (!ecu.enabled || frame.frameId() != ecu.requestId) continue;
            CANFrame flowControl;
            flowControl.setFrameId(ecu.responseId);
            flowControl.setExtendedFrameFormat(ecu.extended);
            flowControl.setPayload(QByteArray::fromHex("3000030000000000"));
            enqueueFrame(flowControl, ecu.responseDelayMs);
            emit activity(tr("%1 issued ISO-TP flow control").arg(ecu.name));
        }
        return true;
    }
    const QByteArray request = decodeIsoTp(frame);
    if (request.isEmpty()) return true;
    for (DiagnosticSimEcu &ecu : mEcus) {
        const bool physical = frame.frameId() == ecu.requestId;
        const bool functional = frame.frameId() == 0x7DF || frame.frameId() == 0x18DB33F1;
        if (!ecu.enabled || (!physical && !functional)) continue;
        emit activity(tr("%1 request %2: %3").arg(ecu.name, hexId(frame.frameId()),
                      QString(request.toHex(' ')).toUpper()));
        QByteArray response = request.size() && quint8(request[0]) <= 0x0A
            ? obdResponse(ecu, request) : udsResponse(ecu, request);
        if (response.isEmpty()) continue;

        DiagnosticSimFault fault = mFault;
        ++mFault.counter;
        const bool apply = fault.enabled && fault.every > 0 &&
                           (mFault.counter % fault.every) == 0;
        if (apply && fault.mode == QStringLiteral("Drop")) {
            emit activity(tr("%1 response dropped by fault policy").arg(ecu.name));
            continue;
        }
        if (apply && fault.mode == QStringLiteral("Negative") && !request.isEmpty())
            response = QByteArray::fromHex("7F") + request.left(1) + char(fault.nrc);
        if (apply && fault.mode == QStringLiteral("Malformed"))
            response.prepend(char(0xFF));
        int delay = ecu.responseDelayMs + (apply ? fault.delayMs : 0);
        enqueueResponse(ecu, response, delay);
        if (apply && fault.mode == QStringLiteral("Duplicate"))
            enqueueResponse(ecu, response, delay + 5);
        emit activity(tr("%1 response %2: %3").arg(ecu.name, hexId(ecu.responseId),
                      QString(response.toHex(' ')).toUpper()));
    }
    return true;
}

DiagnosticSimulatorWindow::DiagnosticSimulatorWindow(const QVector<CANFrame> *frames,
                                                       QWidget *parent)
    : QDialog(parent), mFrames(frames)
{
    buildUi();
    addEcu();
}

DiagnosticSimulatorWindow::~DiagnosticSimulatorWindow()
{
    stopSimulator();
}

void DiagnosticSimulatorWindow::buildUi()
{
    setWindowTitle(tr("Diagnostic Replay Simulator"));
    setProperty("helpPage", QStringLiteral("diagnostic_simulator.md"));
    QVBoxLayout *root = new QVBoxLayout(this);
    QHBoxLayout *toolbar = new QHBoxLayout;
    QPushButton *start = new QPushButton(tr("Start virtual bus"), this);
    QPushButton *stop = new QPushButton(tr("Stop"), this);
    QPushButton *reset = new QPushButton(tr("Reset state"), this);
    QPushButton *load = new QPushButton(tr("Load project"), this);
    QPushButton *save = new QPushButton(tr("Save project"), this);
    mStatus = new QLabel(tr("Stopped"), this);
    toolbar->addWidget(start);
    toolbar->addWidget(stop);
    toolbar->addWidget(reset);
    toolbar->addSpacing(12);
    toolbar->addWidget(load);
    toolbar->addWidget(save);
    toolbar->addStretch();
    toolbar->addWidget(mStatus);
    root->addLayout(toolbar);

    QTabWidget *tabs = new QTabWidget(this);
    QWidget *ecusPage = new QWidget(tabs);
    QVBoxLayout *ecusLayout = new QVBoxLayout(ecusPage);
    mEcuTable = new QTableWidget(0, 9, ecusPage);
    mEcuTable->setHorizontalHeaderLabels({tr("On"), tr("Name"), tr("Request ID"),
        tr("Response ID"), tr("Ext"), tr("Delay ms"), tr("Session"), tr("Security"), tr("Notes")});
    mEcuTable->resizeColumnsToContents();
    mEcuTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    mEcuTable->horizontalHeader()->setStretchLastSection(true);
    mEcuTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    mEcuTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    QHBoxLayout *ecuButtons = new QHBoxLayout;
    QPushButton *addEcuButton = new QPushButton(tr("Add ECU"), ecusPage);
    QPushButton *removeEcuButton = new QPushButton(tr("Remove"), ecusPage);
    QPushButton *learn = new QPushButton(tr("Learn from capture"), ecusPage);
    ecuButtons->addWidget(addEcuButton);
    ecuButtons->addWidget(removeEcuButton);
    ecuButtons->addWidget(learn);
    ecuButtons->addStretch();
    ecusLayout->addWidget(mEcuTable);
    ecusLayout->addLayout(ecuButtons);
    tabs->addTab(ecusPage, tr("ECUs"));

    QWidget *valuesPage = new QWidget(tabs);
    QVBoxLayout *valuesLayout = new QVBoxLayout(valuesPage);
    mValueTable = new QTableWidget(0, 6, valuesPage);
    mValueTable->setHorizontalHeaderLabels({tr("Kind"), tr("Key"), tr("Name"),
                                            tr("Bytes"), tr("Encoding"), tr("Writable")});
    mValueTable->resizeColumnsToContents();
    mValueTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    mValueTable->horizontalHeader()->setStretchLastSection(true);
    mValueTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    mValueTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    QHBoxLayout *valueButtons = new QHBoxLayout;
    QPushButton *loadEcu = new QPushButton(tr("Load selected ECU"), valuesPage);
    QPushButton *storeEcu = new QPushButton(tr("Apply to ECU"), valuesPage);
    QPushButton *addValueButton = new QPushButton(tr("Add value"), valuesPage);
    QPushButton *removeValueButton = new QPushButton(tr("Remove"), valuesPage);
    valueButtons->addWidget(loadEcu);
    valueButtons->addWidget(storeEcu);
    valueButtons->addWidget(addValueButton);
    valueButtons->addWidget(removeValueButton);
    valueButtons->addStretch();
    valuesLayout->addWidget(mValueTable);
    valuesLayout->addLayout(valueButtons);
    tabs->addTab(valuesPage, tr("DIDs, PIDs and state"));

    QWidget *faultPage = new QWidget(tabs);
    QFormLayout *faultLayout = new QFormLayout(faultPage);
    mFaultMode = new QComboBox(faultPage);
    mFaultMode->addItems({tr("None"), tr("Drop"), tr("Delay"), tr("Negative"),
                          tr("Malformed"), tr("Duplicate")});
    mFaultEvery = new QSpinBox(faultPage);
    mFaultEvery->setRange(1, 100000);
    mFaultDelay = new QSpinBox(faultPage);
    mFaultDelay->setRange(0, 60000);
    mFaultNrc = new QSpinBox(faultPage);
    mFaultNrc->setRange(0, 255);
    mFaultNrc->setDisplayIntegerBase(16);
    mFaultNrc->setValue(0x22);
    faultLayout->addRow(tr("Fault"), mFaultMode);
    faultLayout->addRow(tr("Apply every N responses"), mFaultEvery);
    faultLayout->addRow(tr("Additional delay (ms)"), mFaultDelay);
    faultLayout->addRow(tr("Negative response code"), mFaultNrc);
    QLabel *safety = new QLabel(tr("Simulation remains isolated from physical adapters. "
                                   "Bridging is intentionally not provided."), faultPage);
    safety->setWordWrap(true);
    faultLayout->addRow(safety);
    tabs->addTab(faultPage, tr("Fault injection"));

    QWidget *scenarioPage = new QWidget(tabs);
    QVBoxLayout *scenarioLayout = new QVBoxLayout(scenarioPage);
    mScenarioTable = new QTableWidget(0, 5, scenarioPage);
    mScenarioTable->setHorizontalHeaderLabels({tr("Enabled"), tr("Assertion"),
        tr("Pattern"), tr("Timeout ms"), tr("Result")});
    mScenarioTable->horizontalHeader()->setStretchLastSection(true);
    QPushButton *addScenario = new QPushButton(tr("Add assertion"), scenarioPage);
    QPushButton *run = new QPushButton(tr("Run assertions"), scenarioPage);
    QPushButton *report = new QPushButton(tr("Export report"), scenarioPage);
    QHBoxLayout *scenarioButtons = new QHBoxLayout;
    scenarioButtons->addWidget(addScenario);
    scenarioButtons->addWidget(run);
    scenarioButtons->addWidget(report);
    scenarioButtons->addStretch();
    scenarioLayout->addWidget(mScenarioTable);
    scenarioLayout->addLayout(scenarioButtons);
    tabs->addTab(scenarioPage, tr("Scenarios"));

    QWidget *activityPage = new QWidget(tabs);
    QVBoxLayout *activityLayout = new QVBoxLayout(activityPage);
    mActivity = new QTextEdit(activityPage);
    mActivity->setReadOnly(true);
    activityLayout->addWidget(mActivity);
    tabs->addTab(activityPage, tr("Activity"));
    root->addWidget(tabs);

    connect(start, &QPushButton::clicked, this, &DiagnosticSimulatorWindow::startSimulator);
    connect(stop, &QPushButton::clicked, this, &DiagnosticSimulatorWindow::stopSimulator);
    connect(reset, &QPushButton::clicked, this, &DiagnosticSimulatorWindow::resetSimulator);
    connect(load, &QPushButton::clicked, this, &DiagnosticSimulatorWindow::loadProject);
    connect(save, &QPushButton::clicked, this, &DiagnosticSimulatorWindow::saveProject);
    connect(addEcuButton, &QPushButton::clicked, this, &DiagnosticSimulatorWindow::addEcu);
    connect(removeEcuButton, &QPushButton::clicked, this, &DiagnosticSimulatorWindow::removeSelectedEcus);
    connect(learn, &QPushButton::clicked, this, &DiagnosticSimulatorWindow::learnFromCapture);
    connect(loadEcu, &QPushButton::clicked, this, &DiagnosticSimulatorWindow::loadSelectedEcu);
    connect(storeEcu, &QPushButton::clicked, this, &DiagnosticSimulatorWindow::storeSelectedEcu);
    connect(addValueButton, &QPushButton::clicked, this, &DiagnosticSimulatorWindow::addValue);
    connect(removeValueButton, &QPushButton::clicked, this, &DiagnosticSimulatorWindow::removeSelectedValues);
    connect(mFaultMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &DiagnosticSimulatorWindow::updateFault);
    connect(mFaultEvery, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &DiagnosticSimulatorWindow::updateFault);
    connect(mFaultDelay, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &DiagnosticSimulatorWindow::updateFault);
    connect(mFaultNrc, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &DiagnosticSimulatorWindow::updateFault);
    connect(addScenario, &QPushButton::clicked, this, [this]() {
        const int row = mScenarioTable->rowCount();
        mScenarioTable->insertRow(row);
        for (int col = 0; col < 5; ++col) mScenarioTable->setItem(row, col, new QTableWidgetItem);
        mScenarioTable->item(row, 0)->setCheckState(Qt::Checked);
        mScenarioTable->item(row, 1)->setText(tr("Activity contains"));
        mScenarioTable->item(row, 3)->setText(QStringLiteral("2000"));
    });
    connect(run, &QPushButton::clicked, this, &DiagnosticSimulatorWindow::runScenarios);
    connect(report, &QPushButton::clicked, this, &DiagnosticSimulatorWindow::exportReport);
}

void DiagnosticSimulatorWindow::addEcu()
{
    const int row = mEcuTable->rowCount();
    mEcuTable->insertRow(row);
    const QStringList defaults = {QString(), tr("Simulated ECU %1").arg(row + 1),
        hexId(0x7E0 + row), hexId(0x7E8 + row), QString(), QStringLiteral("15"),
        QStringLiteral("1"), QStringLiteral("0"), QString()};
    for (int col = 0; col < defaults.size(); ++col) {
        QTableWidgetItem *item = new QTableWidgetItem(defaults[col]);
        if (col == 0 || col == 4) item->setCheckState(col == 0 ? Qt::Checked : Qt::Unchecked);
        mEcuTable->setItem(row, col, item);
    }
    if (row == 0) {
        mEcuTable->selectRow(0);
        mLoadedEcuRow = 0;
        addValue();
        mValueTable->item(0, 0)->setText(QStringLiteral("DID"));
        mValueTable->item(0, 1)->setText(QStringLiteral("F190"));
        mValueTable->item(0, 2)->setText(QStringLiteral("VIN"));
        mValueTable->item(0, 3)->setText(QStringLiteral("53 41 56 56 59 43 41 4E 53 49 4D 30 30 30 30 30 31"));
        mValueTable->item(0, 4)->setText(QStringLiteral("ASCII"));
    }
}

void DiagnosticSimulatorWindow::removeSelectedEcus()
{
    QList<int> rows;
    for (const QModelIndex &index : mEcuTable->selectionModel()->selectedRows()) rows << index.row();
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    for (int row : rows) mEcuTable->removeRow(row);
    mLoadedEcuRow = -1;
    mValueTable->setRowCount(0);
}

void DiagnosticSimulatorWindow::addValue()
{
    const int row = mValueTable->rowCount();
    mValueTable->insertRow(row);
    const QStringList defaults = {QStringLiteral("PID"), QStringLiteral("01:0C"),
        tr("Engine RPM"), QStringLiteral("1A F8"), QStringLiteral("Raw bytes"), QString()};
    for (int col = 0; col < defaults.size(); ++col) {
        QTableWidgetItem *item = new QTableWidgetItem(defaults[col]);
        if (col == 5) item->setCheckState(Qt::Unchecked);
        mValueTable->setItem(row, col, item);
    }
}

void DiagnosticSimulatorWindow::removeSelectedValues()
{
    QList<int> rows;
    for (const QModelIndex &index : mValueTable->selectionModel()->selectedRows()) rows << index.row();
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    for (int row : rows) mValueTable->removeRow(row);
}

quint32 DiagnosticSimulatorWindow::parseId(const QString &text, bool *ok)
{
    QString value = text.trimmed();
    int base = value.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive) ? 16 : 16;
    if (value.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) value.remove(0, 2);
    return value.toUInt(ok, base);
}

QByteArray DiagnosticSimulatorWindow::parseBytes(const QString &text)
{
    QString compact = text;
    compact.remove(QLatin1Char(' '));
    compact.remove(QLatin1Char(','));
    compact.remove(QStringLiteral("0x"), Qt::CaseInsensitive);
    return QByteArray::fromHex(compact.toLatin1());
}

QString DiagnosticSimulatorWindow::bytesText(const QByteArray &data)
{
    return QString(data.toHex(' ')).toUpper();
}

QList<DiagnosticSimEcu> DiagnosticSimulatorWindow::ecusFromTables() const
{
    QList<DiagnosticSimEcu> ecus;
    for (int row = 0; row < mEcuTable->rowCount(); ++row) {
        DiagnosticSimEcu ecu;
        ecu.enabled = mEcuTable->item(row, 0)->checkState() == Qt::Checked;
        ecu.name = mEcuTable->item(row, 1)->text();
        ecu.requestId = parseId(mEcuTable->item(row, 2)->text());
        ecu.responseId = parseId(mEcuTable->item(row, 3)->text());
        ecu.extended = mEcuTable->item(row, 4)->checkState() == Qt::Checked;
        ecu.responseDelayMs = mEcuTable->item(row, 5)->text().toInt();
        ecu.session = mEcuTable->item(row, 6)->text().toInt();
        ecu.securityLevel = mEcuTable->item(row, 7)->text().toInt();
        ecu.notes = mEcuTable->item(row, 8)->text();
        if (row == mLoadedEcuRow) {
            for (int valueRow = 0; valueRow < mValueTable->rowCount(); ++valueRow) {
                DiagnosticSimValue value;
                value.kind = mValueTable->item(valueRow, 0)->text();
                value.key = mValueTable->item(valueRow, 1)->text();
                value.name = mValueTable->item(valueRow, 2)->text();
                value.value = parseBytes(mValueTable->item(valueRow, 3)->text());
                value.encoding = mValueTable->item(valueRow, 4)->text();
                value.writable = mValueTable->item(valueRow, 5)->checkState() == Qt::Checked;
                ecu.values << value;
            }
        } else if (mConnection) {
            const QList<DiagnosticSimEcu> running = mConnection->ecus();
            if (row < running.size()) ecu.values = running[row].values;
        }
        ecus << ecu;
    }
    return ecus;
}

void DiagnosticSimulatorWindow::populateValues(const QList<DiagnosticSimValue> &values)
{
    mValueTable->setRowCount(0);
    for (const DiagnosticSimValue &value : values) {
        const int row = mValueTable->rowCount();
        mValueTable->insertRow(row);
        const QStringList fields = {value.kind, value.key, value.name, bytesText(value.value),
                                    value.encoding, QString()};
        for (int col = 0; col < fields.size(); ++col) {
            QTableWidgetItem *item = new QTableWidgetItem(fields[col]);
            if (col == 5) item->setCheckState(value.writable ? Qt::Checked : Qt::Unchecked);
            mValueTable->setItem(row, col, item);
        }
    }
}

void DiagnosticSimulatorWindow::loadSelectedEcu()
{
    const int row = mEcuTable->currentRow();
    if (row < 0) return;
    if (mLoadedEcuRow >= 0) storeSelectedEcu();
    mLoadedEcuRow = row;
    const QList<DiagnosticSimEcu> ecus = mConnection ? mConnection->ecus() : ecusFromTables();
    populateValues(row < ecus.size() ? ecus[row].values : QList<DiagnosticSimValue>());
}

void DiagnosticSimulatorWindow::storeSelectedEcu()
{
    if (mLoadedEcuRow < 0) return;
    QList<DiagnosticSimEcu> ecus = ecusFromTables();
    if (mConnection) mConnection->setEcus(ecus);
    appendActivity(tr("Applied value definitions to %1").arg(
        mEcuTable->item(mLoadedEcuRow, 1)->text()));
}

void DiagnosticSimulatorWindow::startSimulator()
{
    if (mConnection) {
        mConnection->setEcus(ecusFromTables());
        mStatus->setText(tr("Running"));
        return;
    }
    mConnection = new VirtualDiagnosticConnection(this);
    mConnection->setEcus(ecusFromTables());
    connect(mConnection, &VirtualDiagnosticConnection::activity,
            this, &DiagnosticSimulatorWindow::appendActivity);
    connect(mConnection, &VirtualDiagnosticConnection::ecuStateChanged, this, [this]() {
        if (!mConnection) return;
        populateEcus(mConnection->ecus());
    });
    CANConManager::getInstance()->add(mConnection);
    mConnection->start();
    updateFault();
    mStatus->setText(tr("Running on virtual bus %1").arg(
        CANConManager::getInstance()->getBusBase(mConnection)));
}

void DiagnosticSimulatorWindow::stopSimulator()
{
    if (!mConnection) return;
    mConnection->stop();
    CANConManager::getInstance()->remove(mConnection);
    mConnection->deleteLater();
    mConnection = nullptr;
    mStatus->setText(tr("Stopped"));
}

void DiagnosticSimulatorWindow::resetSimulator()
{
    if (mConnection) mConnection->resetState();
    appendActivity(tr("Simulator state reset"));
}

void DiagnosticSimulatorWindow::updateFault()
{
    if (!mConnection) return;
    DiagnosticSimFault fault;
    fault.mode = mFaultMode->currentText();
    fault.enabled = mFaultMode->currentIndex() != 0;
    fault.every = mFaultEvery->value();
    fault.delayMs = mFaultDelay->value();
    fault.nrc = mFaultNrc->value();
    mConnection->setFault(fault);
}

void DiagnosticSimulatorWindow::appendActivity(const QString &text)
{
    mActivity->append(QStringLiteral("%1  %2")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")), text));
}

void DiagnosticSimulatorWindow::learnFromCapture()
{
    if (!mFrames || mFrames->isEmpty()) {
        QMessageBox::information(this, tr("Learn from capture"), tr("No captured frames are available."));
        return;
    }
    QMap<quint32, quint32> pairs;
    QMap<quint32, QByteArray> requests;
    QList<DiagnosticSimEcu> learned;
    for (const CANFrame &frame : *mFrames) {
        const QByteArray data = frame.payload();
        if (data.size() < 2 || (quint8(data[0]) & 0xF0) != 0) continue;
        const QByteArray payload = data.mid(1, quint8(data[0]) & 0x0F);
        if (!frame.isReceived) {
            requests[frame.frameId()] = payload;
            continue;
        }
        for (auto it = requests.cbegin(); it != requests.cend(); ++it) {
            if (payload.isEmpty() || it.value().isEmpty()) continue;
            const int expected = quint8(it.value()[0]) + 0x40;
            if (quint8(payload[0]) != expected && quint8(payload[0]) != 0x7F) continue;
            pairs[it.key()] = frame.frameId();
            DiagnosticSimEcu *ecu = nullptr;
            for (DiagnosticSimEcu &candidate : learned)
                if (candidate.requestId == it.key() && candidate.responseId == frame.frameId())
                    ecu = &candidate;
            if (!ecu) {
                DiagnosticSimEcu created;
                created.name = tr("Learned ECU %1").arg(learned.size() + 1);
                created.requestId = it.key();
                created.responseId = frame.frameId();
                created.extended = frame.hasExtendedFrameFormat();
                created.notes = tr("Inferred from captured request/response evidence");
                learned << created;
                ecu = &learned.last();
            }
            if (quint8(it.value()[0]) == 0x22 && it.value().size() >= 3 &&
                payload.size() >= 3) {
                DiagnosticSimValue value;
                value.kind = QStringLiteral("DID");
                value.key = QStringLiteral("%1%2")
                    .arg(quint8(it.value()[1]), 2, 16, QLatin1Char('0'))
                    .arg(quint8(it.value()[2]), 2, 16, QLatin1Char('0')).toUpper();
                value.name = tr("Learned DID %1").arg(value.key);
                value.value = payload.mid(3);
                ecu->values << value;
            } else if (quint8(it.value()[0]) <= 0x0A && it.value().size() >= 2 &&
                       payload.size() >= 2) {
                DiagnosticSimValue value;
                value.kind = QStringLiteral("PID");
                value.key = QStringLiteral("%1:%2")
                    .arg(quint8(it.value()[0]), 2, 16, QLatin1Char('0'))
                    .arg(quint8(it.value()[1]), 2, 16, QLatin1Char('0')).toUpper();
                value.name = tr("Learned PID %1").arg(value.key);
                value.value = payload.mid(2);
                ecu->values << value;
            }
            break;
        }
    }
    if (learned.isEmpty()) {
        QMessageBox::information(this, tr("Learn from capture"),
                                 tr("No ISO-TP diagnostic request/response pairs were found."));
        return;
    }
    QList<DiagnosticSimEcu> merged = ecusFromTables();
    merged.append(learned);
    populateEcus(merged);
    appendActivity(tr("Learned %1 ECU definition(s) from capture").arg(learned.size()));
}

void DiagnosticSimulatorWindow::populateEcus(const QList<DiagnosticSimEcu> &ecus)
{
    const int selected = mLoadedEcuRow;
    mEcuTable->setRowCount(0);
    for (const DiagnosticSimEcu &ecu : ecus) {
        const int row = mEcuTable->rowCount();
        mEcuTable->insertRow(row);
        const QStringList fields = {QString(), ecu.name, hexId(ecu.requestId), hexId(ecu.responseId),
            QString(), QString::number(ecu.responseDelayMs), QString::number(ecu.session),
            QString::number(ecu.securityLevel), ecu.notes};
        for (int col = 0; col < fields.size(); ++col) {
            QTableWidgetItem *item = new QTableWidgetItem(fields[col]);
            if (col == 0) item->setCheckState(ecu.enabled ? Qt::Checked : Qt::Unchecked);
            if (col == 4) item->setCheckState(ecu.extended ? Qt::Checked : Qt::Unchecked);
            mEcuTable->setItem(row, col, item);
        }
    }
    mLoadedEcuRow = qBound(-1, selected, ecus.size() - 1);
    if (mLoadedEcuRow >= 0) populateValues(ecus[mLoadedEcuRow].values);
}

QJsonObject DiagnosticSimulatorWindow::projectJson() const
{
    QJsonArray ecuArray;
    for (const DiagnosticSimEcu &ecu : ecusFromTables()) {
        QJsonArray values;
        for (const DiagnosticSimValue &value : ecu.values) {
            values.append(QJsonObject{{"kind", value.kind}, {"key", value.key},
                {"name", value.name}, {"value", bytesText(value.value)},
                {"encoding", value.encoding}, {"writable", value.writable}});
        }
        ecuArray.append(QJsonObject{{"enabled", ecu.enabled}, {"name", ecu.name},
            {"request_id", QString::number(ecu.requestId, 16)},
            {"response_id", QString::number(ecu.responseId, 16)}, {"extended", ecu.extended},
            {"delay_ms", ecu.responseDelayMs}, {"session", ecu.session},
            {"security", ecu.securityLevel}, {"notes", ecu.notes}, {"values", values}});
    }
    QJsonArray scenarios;
    for (int row = 0; row < mScenarioTable->rowCount(); ++row)
        scenarios.append(QJsonObject{{"enabled", mScenarioTable->item(row, 0)->checkState() == Qt::Checked},
            {"assertion", mScenarioTable->item(row, 1)->text()},
            {"pattern", mScenarioTable->item(row, 2)->text()},
            {"timeout_ms", mScenarioTable->item(row, 3)->text().toInt()}});
    return QJsonObject{{"format", "SavvyCAN diagnostic simulator"},
        {"version", 1}, {"ecus", ecuArray}, {"scenarios", scenarios},
        {"fault", QJsonObject{{"mode", mFaultMode->currentText()},
            {"every", mFaultEvery->value()}, {"delay_ms", mFaultDelay->value()},
            {"nrc", mFaultNrc->value()}}}};
}

bool DiagnosticSimulatorWindow::loadProjectJson(const QJsonObject &root, QString *error)
{
    if (root.value("format").toString() != QStringLiteral("SavvyCAN diagnostic simulator")) {
        if (error) *error = tr("Not a SavvyCAN diagnostic simulator project.");
        return false;
    }
    QList<DiagnosticSimEcu> ecus;
    for (const QJsonValue &entry : root.value("ecus").toArray()) {
        const QJsonObject object = entry.toObject();
        DiagnosticSimEcu ecu;
        ecu.enabled = object.value("enabled").toBool(true);
        ecu.name = object.value("name").toString();
        ecu.requestId = object.value("request_id").toString().toUInt(nullptr, 16);
        ecu.responseId = object.value("response_id").toString().toUInt(nullptr, 16);
        ecu.extended = object.value("extended").toBool();
        ecu.responseDelayMs = object.value("delay_ms").toInt(15);
        ecu.session = object.value("session").toInt(1);
        ecu.securityLevel = object.value("security").toInt();
        ecu.notes = object.value("notes").toString();
        for (const QJsonValue &valueEntry : object.value("values").toArray()) {
            const QJsonObject valueObject = valueEntry.toObject();
            DiagnosticSimValue value;
            value.kind = valueObject.value("kind").toString();
            value.key = valueObject.value("key").toString();
            value.name = valueObject.value("name").toString();
            value.value = parseBytes(valueObject.value("value").toString());
            value.encoding = valueObject.value("encoding").toString();
            value.writable = valueObject.value("writable").toBool();
            ecu.values << value;
        }
        ecus << ecu;
    }
    populateEcus(ecus);
    mScenarioTable->setRowCount(0);
    for (const QJsonValue &entry : root.value("scenarios").toArray()) {
        const QJsonObject scenario = entry.toObject();
        const int row = mScenarioTable->rowCount();
        mScenarioTable->insertRow(row);
        for (int col = 0; col < 5; ++col) mScenarioTable->setItem(row, col, new QTableWidgetItem);
        mScenarioTable->item(row, 0)->setCheckState(scenario.value("enabled").toBool(true)
                                                   ? Qt::Checked : Qt::Unchecked);
        mScenarioTable->item(row, 1)->setText(scenario.value("assertion").toString());
        mScenarioTable->item(row, 2)->setText(scenario.value("pattern").toString());
        mScenarioTable->item(row, 3)->setText(QString::number(scenario.value("timeout_ms").toInt()));
    }
    const QJsonObject fault = root.value("fault").toObject();
    mFaultMode->setCurrentText(fault.value("mode").toString(tr("None")));
    mFaultEvery->setValue(fault.value("every").toInt(1));
    mFaultDelay->setValue(fault.value("delay_ms").toInt());
    mFaultNrc->setValue(fault.value("nrc").toInt(0x22));
    if (mConnection) mConnection->setEcus(ecus);
    return true;
}

void DiagnosticSimulatorWindow::saveProject()
{
    const QString path = QFileDialog::getSaveFileName(this, tr("Save simulator project"),
        QString(), tr("Simulator projects (*.json)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(QJsonDocument(projectJson()).toJson(QJsonDocument::Indented)) < 0)
        QMessageBox::warning(this, tr("Save project"), file.errorString());
}

void DiagnosticSimulatorWindow::loadProject()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Load simulator project"),
        QString(), tr("Simulator projects (*.json)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Load project"), file.errorString());
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    QString error;
    if (parseError.error != QJsonParseError::NoError ||
        !loadProjectJson(document.object(), &error))
        QMessageBox::warning(this, tr("Load project"),
            error.isEmpty() ? parseError.errorString() : error);
}

void DiagnosticSimulatorWindow::runScenarios()
{
    const QString activity = mActivity->toPlainText();
    int passed = 0;
    int failed = 0;
    for (int row = 0; row < mScenarioTable->rowCount(); ++row) {
        if (mScenarioTable->item(row, 0)->checkState() != Qt::Checked) continue;
        const QString assertion = mScenarioTable->item(row, 1)->text();
        const QString pattern = mScenarioTable->item(row, 2)->text();
        bool result = activity.contains(pattern, Qt::CaseInsensitive);
        if (assertion.contains(QStringLiteral("not"), Qt::CaseInsensitive)) result = !result;
        mScenarioTable->item(row, 4)->setText(result ? tr("PASS") : tr("FAIL"));
        mScenarioTable->item(row, 4)->setBackground(
            result ? QColor(210, 245, 218) : QColor(255, 215, 215));
        result ? ++passed : ++failed;
    }
    appendActivity(tr("Scenario run complete: %1 passed, %2 failed").arg(passed).arg(failed));
}

void DiagnosticSimulatorWindow::exportReport()
{
    const QString path = QFileDialog::getSaveFileName(this, tr("Export simulator report"),
        QString(), tr("JSON report (*.json);;Text report (*.txt)"));
    if (path.isEmpty()) return;
    QJsonArray assertions;
    for (int row = 0; row < mScenarioTable->rowCount(); ++row)
        assertions.append(QJsonObject{{"assertion", mScenarioTable->item(row, 1)->text()},
            {"pattern", mScenarioTable->item(row, 2)->text()},
            {"result", mScenarioTable->item(row, 4)->text()}});
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return;
    if (path.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive))
        file.write(QJsonDocument(QJsonObject{{"generated", QDateTime::currentDateTime().toString(Qt::ISODate)},
            {"assertions", assertions}, {"activity", mActivity->toPlainText()}})
            .toJson(QJsonDocument::Indented));
    else
        file.write(mActivity->toPlainText().toUtf8());
}
