#include "canopenworkbenchwindow.h"

#include "connections/canconmanager.h"
#include "diagnosticgraphwindow.h"

#include <QComboBox>
#include <QCheckBox>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QTextStream>
#include <QTime>
#include <QVBoxLayout>

#include <algorithm>

namespace {
quint32 readLe32(const QByteArray &data, int offset)
{
    return quint32(quint8(data.at(offset))) | (quint32(quint8(data.at(offset + 1))) << 8)
        | (quint32(quint8(data.at(offset + 2))) << 16) | (quint32(quint8(data.at(offset + 3))) << 24);
}

void appendLe32(QByteArray &data, quint32 value)
{
    for (int i = 0; i < 4; ++i) data.append(char((value >> (i * 8)) & 0xFF));
}
}

CANopenWorkbenchWindow::CANopenWorkbenchWindow(QWidget *parent) : QDialog(parent)
{
    buildUi();
    loadSettings();
    connect(CANConManager::getInstance(), &CANConManager::framesReceived,
            this, &CANopenWorkbenchWindow::processFrames);
    connect(&ageTimer, &QTimer::timeout, this, &CANopenWorkbenchWindow::refreshAges);
    ageTimer.start(250);
}

CANopenWorkbenchWindow::~CANopenWorkbenchWindow()
{
    saveSettings();
}

bool CANopenWorkbenchWindow::addObjectDefinition(int nodeId, int index, int subIndex,
                                                  const QString &name, const QString &dataType,
                                                  const QString &access, const QString &value,
                                                  QString *error)
{
    if (nodeId < 1 || nodeId > 127 || index < 0 || index > 0xFFFF
        || subIndex < 0 || subIndex > 0xFF)
    {
        if (error) *error = tr("Node, index, or sub-index is outside the CANopen range.");
        return false;
    }
    nodeSpin->setValue(nodeId);
    const int row = ensureObjectRow(index, subIndex);
    objectTable->item(row, ObjectName)->setText(name);
    objectTable->item(row, ObjectType)->setText(dataType.isEmpty() ? QStringLiteral("UNSIGNED8") : dataType);
    objectTable->item(row, ObjectAccess)->setText(access.isEmpty() ? QStringLiteral("rw") : access);
    objectTable->item(row, ObjectValue)->setText(value);
    objectTable->selectRow(row);
    return true;
}

bool CANopenWorkbenchWindow::executeAIRequest(const QString &operation,
                                              const QJsonObject &arguments, QString *error)
{
    if (transfer.active) {
        if (error) *error = tr("An SDO transfer is already active.");
        return false;
    }
    if (operation == QStringLiteral("scan_nodes")) { scanNodes(); return true; }
    if (operation == QStringLiteral("sync")) { sendSync(); return true; }
    if (operation == QStringLiteral("time")) { sendTime(); return true; }
    if (operation == QStringLiteral("discover_lss")) { discoverLss(); return true; }
    if (operation == QStringLiteral("nmt")) {
        const int node = arguments.value("node_id").toInt();
        const QString command = arguments.value("command").toString().toLower();
        const QMap<QString, int> commands = {
            {QStringLiteral("start"), 0x01},
            {QStringLiteral("stop"), 0x02},
            {QStringLiteral("pre_operational"), 0x80},
            {QStringLiteral("reset_node"), 0x81},
            {QStringLiteral("reset_communication"), 0x82}
        };
        if (node < 1 || node > 127 || !commands.contains(command)) {
            if (error) *error = tr("Invalid CANopen node or NMT command.");
            return false;
        }
        nodeSpin->setValue(node);
        sendFrame(0x000, QByteArray({char(commands.value(command)), char(node)}));
        return true;
    }
    if (operation == QStringLiteral("read_drive_state")) {
        const int node = arguments.value("node_id").toInt();
        if (node < 1 || node > 127) {
            if (error) *error = tr("Invalid CANopen node.");
            return false;
        }
        nodeSpin->setValue(node);
        readDriveState();
        return transfer.active;
    }
    if (operation == QStringLiteral("remove_object")) {
        bool indexOk = false;
        const int index = number(arguments.value("index").toString(), &indexOk);
        const int subIndex = arguments.value("subindex").toVariant().toInt();
        const int row = indexOk ? findObjectRow(index, subIndex) : -1;
        if (row < 0) {
            if (error) *error = tr("Object Dictionary entry was not found.");
            return false;
        }
        objectTable->removeRow(row);
        return true;
    }
    if (operation == QStringLiteral("clear_emcy")) {
        emcyTable->setRowCount(0);
        for (auto it = nodes.begin(); it != nodes.end(); ++it) {
            it->emergency.clear();
            if (it->row >= 0) nodeTable->item(it->row, NodeEmergency)->setText(QString());
        }
        return true;
    }
    if (operation == QStringLiteral("import_eds"))
        return importEdsFile(arguments.value("path").toString(), error);
    if (operation == QStringLiteral("export_dcf"))
        return exportDcfFile(arguments.value("path").toString(), error);
    if (operation == QStringLiteral("configure_lss")) {
        const int node = arguments.value("node_id").toInt();
        const int bitrate = arguments.value("bitrate_index").toInt(-1);
        const int comboIndex = lssBitrateCombo->findData(bitrate);
        if (node < 1 || node > 127 || comboIndex < 0) {
            if (error) *error = tr("Invalid LSS node ID or CiA bitrate index.");
            return false;
        }
        lssNodeSpin->setValue(node);
        lssBitrateCombo->setCurrentIndex(comboIndex);
        QByteArray request(8, 0);
        request[0] = 0x04; request[1] = 0x01; sendFrame(0x7E5, request);
        request.fill(0); request[0] = 0x11; request[1] = char(node); sendFrame(0x7E5, request);
        request.fill(0); request[0] = 0x13; request[2] = char(bitrate); sendFrame(0x7E5, request);
        request.fill(0); request[0] = 0x17; sendFrame(0x7E5, request);
        request.fill(0); request[0] = 0x04; sendFrame(0x7E5, request);
        lssStatusLabel->setText(tr("LSS configuration commands sent"));
        return true;
    }
    if (operation == QStringLiteral("remap_pdo")) {
        const int node = arguments.value("node_id").toInt();
        const int pdoNumber = arguments.value("pdo").toInt();
        const bool transmit = arguments.value("direction").toString().toLower()
            != QStringLiteral("rpdo");
        bool cobOk = false;
        const quint32 cobId = number(arguments.value("cob_id").toString(), &cobOk);
        QList<quint32> mappings;
        int totalBits = 0;
        for (const QJsonValue &value : arguments.value("mappings").toArray()) {
            const QJsonObject item = value.toObject();
            bool indexOk = false;
            const int index = number(item.value("index").toString(), &indexOk);
            const int sub = item.value("subindex").toInt(-1);
            const int bits = item.value("bits").toInt();
            if (!indexOk || index < 0 || index > 0xFFFF || sub < 0 || sub > 0xFF
                || bits < 1 || bits > 64) {
                mappings.clear();
                break;
            }
            mappings.append(quint32(index) << 16 | quint32(sub) << 8 | quint32(bits));
            totalBits += bits;
        }
        if (node < 1 || node > 127 || pdoNumber < 1 || pdoNumber > 4 || !cobOk
            || cobId > 0x1FFFFFFF || mappings.isEmpty() || totalBits > 64) {
            if (error) *error = tr("Invalid node, PDO, COB-ID, or mapping; mapped bits must total 64 or less.");
            return false;
        }
        auto bytes = [](quint32 value, int count) {
            QByteArray result;
            for (int i = 0; i < count; ++i)
                result.append(char((value >> (8 * i)) & 0xFF));
            return result;
        };
        nodeSpin->setValue(node);
        const int communicationIndex = (transmit ? 0x1800 : 0x1400) + pdoNumber - 1;
        const int mappingIndex = (transmit ? 0x1A00 : 0x1600) + pdoNumber - 1;
        writeQueue.clear();
        writeQueue.enqueue({communicationIndex, 1, bytes(cobId | 0x80000000U, 4)});
        writeQueue.enqueue({mappingIndex, 0, bytes(0, 1)});
        for (int i = 0; i < mappings.size(); ++i)
            writeQueue.enqueue({mappingIndex, i + 1, bytes(mappings[i], 4)});
        writeQueue.enqueue({mappingIndex, 0, bytes(mappings.size(), 1)});
        writeQueue.enqueue({communicationIndex, 1, bytes(cobId, 4)});
        startNextQueuedWrite();
        return transfer.active;
    }
    if (operation == QStringLiteral("upload_objects")) {
        const int node = arguments.value("node_id").toInt();
        if (node < 1 || node > 127) {
            if (error) *error = tr("Invalid CANopen node.");
            return false;
        }
        nodeSpin->setValue(node);
        uploadQueue.clear();
        for (const QJsonValue &value : arguments.value("objects").toArray()) {
            const QJsonObject item = value.toObject();
            bool ok = false;
            const int index = number(item.value("index").toString(), &ok);
            const int sub = item.value("subindex").toInt(-1);
            if (!ok || sub < 0 || sub > 0xFF) {
                if (error) *error = tr("Invalid object in bulk upload list.");
                uploadQueue.clear();
                return false;
            }
            uploadQueue.enqueue(qMakePair(index, sub));
        }
        startNextQueuedWrite();
        return transfer.active;
    }
    if (operation == QStringLiteral("write_objects")) {
        const int node = arguments.value("node_id").toInt();
        if (node < 1 || node > 127) {
            if (error) *error = tr("Invalid CANopen node.");
            return false;
        }
        nodeSpin->setValue(node);
        writeQueue.clear();
        for (const QJsonValue &value : arguments.value("objects").toArray()) {
            const QJsonObject item = value.toObject();
            bool ok = false;
            const int index = number(item.value("index").toString(), &ok);
            const int sub = item.value("subindex").toInt(-1);
            const QByteArray bytes = QByteArray::fromHex(item.value("value").toString().toLatin1());
            if (!ok || sub < 0 || sub > 0xFF || bytes.isEmpty() || bytes.size() > 4) {
                if (error) *error = tr("Bulk expedited writes require 1-4 bytes per valid object.");
                writeQueue.clear();
                return false;
            }
            writeQueue.enqueue({index, sub, bytes});
        }
        startNextQueuedWrite();
        return transfer.active;
    }
    bool indexOk = false;
    const int index = number(arguments.value("index").toString(), &indexOk);
    const int node = arguments.value("node_id").toVariant().toInt();
    const int subIndex = arguments.value("subindex").toVariant().toInt();
    if (!indexOk || node < 1 || node > 127 || subIndex < 0 || subIndex > 255) {
        if (error) *error = tr("Invalid CANopen node, index, or sub-index.");
        return false;
    }
    nodeSpin->setValue(node);
    indexEdit->setText(QStringLiteral("0x%1").arg(index, 4, 16, QLatin1Char('0')).toUpper());
    subIndexSpin->setValue(subIndex);
    if (operation == QStringLiteral("upload")) { uploadObject(); return transfer.active; }
    if (operation == QStringLiteral("write")) {
        sdoValueEdit->setText(arguments.value("value").toString());
        downloadObject();
        return transfer.active;
    }
    if (error) *error = tr("Unsupported CANopen AI operation: %1").arg(operation);
    return false;
}

void CANopenWorkbenchWindow::buildUi()
{
    setWindowTitle(tr("CANopen Workbench"));
    resize(1100, 700);
    QVBoxLayout *root = new QVBoxLayout(this);
    QHBoxLayout *endpoint = new QHBoxLayout;
    busSpin = new QSpinBox(this);
    busSpin->setRange(0, qMax(0, CANConManager::getInstance()->getNumBuses() - 1));
    nodeSpin = new QSpinBox(this);
    nodeSpin->setRange(1, 127);
    statusLabel = new QLabel(tr("Listening for CANopen traffic"), this);
    endpoint->addWidget(new QLabel(tr("Bus"), this));
    endpoint->addWidget(busSpin);
    endpoint->addWidget(new QLabel(tr("Target node"), this));
    endpoint->addWidget(nodeSpin);
    endpoint->addStretch();
    endpoint->addWidget(statusLabel);
    root->addLayout(endpoint);

    QTabWidget *tabs = new QTabWidget(this);
    QWidget *nodesPage = new QWidget(tabs);
    QVBoxLayout *nodesLayout = new QVBoxLayout(nodesPage);
    nodeTable = new QTableWidget(0, NodeColumnCount, nodesPage);
    nodeTable->setHorizontalHeaderLabels({tr("Node"), tr("NMT state"), tr("Heartbeat age"),
                                          tr("Identity"), tr("Last emergency")});
    nodeTable->resizeColumnsToContents();
    nodeTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    nodeTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    nodesLayout->addWidget(nodeTable);
    QHBoxLayout *nodeControls = new QHBoxLayout;
    QPushButton *scan = new QPushButton(tr("Probe nodes"), nodesPage);
    nmtCommandCombo = new QComboBox(nodesPage);
    nmtCommandCombo->addItem(tr("Start"), 0x01);
    nmtCommandCombo->addItem(tr("Stop"), 0x02);
    nmtCommandCombo->addItem(tr("Pre-operational"), 0x80);
    nmtCommandCombo->addItem(tr("Reset node"), 0x81);
    nmtCommandCombo->addItem(tr("Reset communication"), 0x82);
    QPushButton *nmt = new QPushButton(tr("Send NMT"), nodesPage);
    nodeControls->addWidget(scan);
    nodeControls->addStretch();
    nodeControls->addWidget(nmtCommandCombo);
    nodeControls->addWidget(nmt);
    nodesLayout->addLayout(nodeControls);
    connect(scan, &QPushButton::clicked, this, &CANopenWorkbenchWindow::scanNodes);
    connect(nmt, &QPushButton::clicked, this, &CANopenWorkbenchWindow::sendNmt);
    connect(nodeTable, &QTableWidget::itemSelectionChanged, this, [this]() {
        if (!nodeTable->selectedItems().isEmpty())
            nodeSpin->setValue(nodeTable->item(nodeTable->currentRow(), NodeId)->data(Qt::UserRole).toInt());
    });
    tabs->addTab(nodesPage, tr("Nodes"));

    QWidget *objectsPage = new QWidget(tabs);
    QVBoxLayout *objectsLayout = new QVBoxLayout(objectsPage);
    objectTable = new QTableWidget(0, ObjectColumnCount, objectsPage);
    objectTable->setHorizontalHeaderLabels({tr("Index"), tr("Sub"), tr("Name"), tr("Data type"),
                                            tr("Access"), tr("Value"), tr("Default")});
    objectTable->resizeColumnsToContents();
    objectTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    objectTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    objectTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    objectsLayout->addWidget(objectTable);
    QHBoxLayout *objectControls = new QHBoxLayout;
    indexEdit = new QLineEdit(QStringLiteral("0x1000"), objectsPage);
    subIndexSpin = new QSpinBox(objectsPage);
    subIndexSpin->setRange(0, 255);
    dataTypeCombo = new QComboBox(objectsPage);
    dataTypeCombo->addItems({tr("Hex bytes"), tr("Unsigned 8"), tr("Unsigned 16"),
                             tr("Unsigned 32"), tr("Signed 32"), tr("String")});
    sdoValueEdit = new QLineEdit(objectsPage);
    blockSdoCheck = new QCheckBox(tr("Block transfer"), objectsPage);
    QPushButton *upload = new QPushButton(tr("Upload"), objectsPage);
    QPushButton *download = new QPushButton(tr("Download"), objectsPage);
    QPushButton *loadEds = new QPushButton(tr("Import EDS/DCF"), objectsPage);
    QPushButton *saveDcf = new QPushButton(tr("Export DCF"), objectsPage);
    objectControls->addWidget(new QLabel(tr("Index"), objectsPage));
    objectControls->addWidget(indexEdit);
    objectControls->addWidget(new QLabel(tr("Sub"), objectsPage));
    objectControls->addWidget(subIndexSpin);
    objectControls->addWidget(dataTypeCombo);
    objectControls->addWidget(sdoValueEdit, 1);
    objectControls->addWidget(blockSdoCheck);
    objectControls->addWidget(upload);
    objectControls->addWidget(download);
    objectControls->addWidget(loadEds);
    objectControls->addWidget(saveDcf);
    objectsLayout->addLayout(objectControls);
    QHBoxLayout *listControls = new QHBoxLayout;
    QPushButton *addObject = new QPushButton(tr("Add OD entry"), objectsPage);
    QPushButton *removeObjects = new QPushButton(tr("Remove selected"), objectsPage);
    QPushButton *uploadObjects = new QPushButton(tr("Upload selected"), objectsPage);
    QPushButton *downloadObjects = new QPushButton(tr("Write selected"), objectsPage);
    listControls->addWidget(addObject);
    listControls->addWidget(removeObjects);
    listControls->addStretch();
    listControls->addWidget(uploadObjects);
    listControls->addWidget(downloadObjects);
    objectsLayout->addLayout(listControls);
    sdoLog = new QTextEdit(objectsPage);
    sdoLog->setReadOnly(true);
    sdoLog->setMaximumHeight(110);
    objectsLayout->addWidget(sdoLog);
    connect(upload, &QPushButton::clicked, this, &CANopenWorkbenchWindow::uploadObject);
    connect(download, &QPushButton::clicked, this, &CANopenWorkbenchWindow::downloadObject);
    connect(loadEds, &QPushButton::clicked, this, &CANopenWorkbenchWindow::importEds);
    connect(saveDcf, &QPushButton::clicked, this, &CANopenWorkbenchWindow::exportDcf);
    connect(addObject, &QPushButton::clicked, this, &CANopenWorkbenchWindow::addObjectEntry);
    connect(removeObjects, &QPushButton::clicked, this, &CANopenWorkbenchWindow::removeObjectEntries);
    connect(uploadObjects, &QPushButton::clicked, this, &CANopenWorkbenchWindow::uploadSelectedObjects);
    connect(downloadObjects, &QPushButton::clicked, this, &CANopenWorkbenchWindow::downloadSelectedObjects);
    connect(objectTable, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        indexEdit->setText(objectTable->item(row, ObjectIndex)->text());
        subIndexSpin->setValue(objectTable->item(row, ObjectSubIndex)->text().toInt(nullptr, 16));
        sdoValueEdit->setText(objectTable->item(row, ObjectValue)->text());
    });
    tabs->addTab(objectsPage, tr("Object dictionary / SDO"));

    QWidget *pdoPage = new QWidget(tabs);
    QVBoxLayout *pdoLayout = new QVBoxLayout(pdoPage);
    pdoTable = new QTableWidget(0, PdoColumnCount, pdoPage);
    pdoTable->setHorizontalHeaderLabels({tr("COB-ID"), tr("Node"), tr("PDO"), tr("Data"),
                                         tr("Mapped values"), tr("Frames"), tr("Updated")});
    pdoTable->resizeColumnsToContents();
    pdoTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    pdoLayout->addWidget(pdoTable);
    QPushButton *pdoGraphButton = new QPushButton(tr("Live mapped-value graph"), pdoPage);
    QPushButton *pdoRemapButton = new QPushButton(tr("Remap selected PDO"), pdoPage);
    QHBoxLayout *pdoButtons = new QHBoxLayout;
    pdoButtons->addStretch();
    pdoButtons->addWidget(pdoGraphButton);
    pdoButtons->addWidget(pdoRemapButton);
    pdoLayout->addLayout(pdoButtons);
    connect(pdoGraphButton, &QPushButton::clicked, this, &CANopenWorkbenchWindow::showPdoGraph);
    connect(pdoRemapButton, &QPushButton::clicked, this, &CANopenWorkbenchWindow::remapSelectedPdo);
    tabs->addTab(pdoPage, tr("PDO monitor"));

    QWidget *emcyPage = new QWidget(tabs);
    QVBoxLayout *emcyLayout = new QVBoxLayout(emcyPage);
    emcyTable = new QTableWidget(0, 6, emcyPage);
    emcyTable->setHorizontalHeaderLabels({tr("Time"), tr("Node"), tr("Error code"), tr("Error register"),
                                          tr("Manufacturer data"), tr("Description")});
    emcyTable->resizeColumnsToContents();
    emcyTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    emcyLayout->addWidget(emcyTable);
    tabs->addTab(emcyPage, tr("EMCY history"));

    QWidget *networkPage = new QWidget(tabs);
    QFormLayout *networkLayout = new QFormLayout(networkPage);
    syncCobIdEdit = new QLineEdit(QStringLiteral("0x080"), networkPage);
    timeCobIdEdit = new QLineEdit(QStringLiteral("0x100"), networkPage);
    QPushButton *sync = new QPushButton(tr("Produce SYNC"), networkPage);
    QPushButton *time = new QPushButton(tr("Produce TIME"), networkPage);
    networkLayout->addRow(tr("SYNC COB-ID"), syncCobIdEdit);
    networkLayout->addRow(QString(), sync);
    networkLayout->addRow(tr("TIME COB-ID"), timeCobIdEdit);
    networkLayout->addRow(QString(), time);
    connect(sync, &QPushButton::clicked, this, &CANopenWorkbenchWindow::sendSync);
    connect(time, &QPushButton::clicked, this, &CANopenWorkbenchWindow::sendTime);
    tabs->addTab(networkPage, tr("SYNC / TIME"));

    QWidget *lssPage = new QWidget(tabs);
    QFormLayout *lssLayout = new QFormLayout(lssPage);
    QPushButton *lssDiscover = new QPushButton(tr("Discover unconfigured node"), lssPage);
    lssNodeSpin = new QSpinBox(lssPage);
    lssNodeSpin->setRange(1, 127);
    lssBitrateCombo = new QComboBox(lssPage);
    lssBitrateCombo->addItem(tr("1 Mbit/s"), 0);
    lssBitrateCombo->addItem(tr("800 kbit/s"), 1);
    lssBitrateCombo->addItem(tr("500 kbit/s"), 2);
    lssBitrateCombo->addItem(tr("250 kbit/s"), 3);
    lssBitrateCombo->addItem(tr("125 kbit/s"), 4);
    lssBitrateCombo->addItem(tr("50 kbit/s"), 6);
    lssBitrateCombo->addItem(tr("20 kbit/s"), 7);
    lssBitrateCombo->addItem(tr("10 kbit/s"), 8);
    QPushButton *lssConfigure = new QPushButton(tr("Configure and store"), lssPage);
    lssStatusLabel = new QLabel(tr("No LSS response"), lssPage);
    lssLayout->addRow(lssDiscover);
    lssLayout->addRow(tr("New node ID"), lssNodeSpin);
    lssLayout->addRow(tr("Bitrate"), lssBitrateCombo);
    lssLayout->addRow(lssConfigure);
    lssLayout->addRow(tr("Status"), lssStatusLabel);
    connect(lssDiscover, &QPushButton::clicked, this, &CANopenWorkbenchWindow::discoverLss);
    connect(lssConfigure, &QPushButton::clicked, this, &CANopenWorkbenchWindow::configureLss);
    tabs->addTab(lssPage, tr("LSS"));

    QWidget *drivePage = new QWidget(tabs);
    QFormLayout *driveLayout = new QFormLayout(drivePage);
    driveStateLabel = new QLabel(tr("Not read"), drivePage);
    driveCommandCombo = new QComboBox(drivePage);
    driveCommandCombo->addItem(tr("Shutdown"), 0x0006);
    driveCommandCombo->addItem(tr("Switch on"), 0x0007);
    driveCommandCombo->addItem(tr("Enable operation"), 0x000F);
    driveCommandCombo->addItem(tr("Disable voltage"), 0x0000);
    driveCommandCombo->addItem(tr("Quick stop"), 0x0002);
    driveCommandCombo->addItem(tr("Fault reset"), 0x0080);
    QPushButton *readDrive = new QPushButton(tr("Read statusword 0x6041"), drivePage);
    QPushButton *controlDrive = new QPushButton(tr("Write controlword 0x6040"), drivePage);
    driveLayout->addRow(tr("CiA 402 state"), driveStateLabel);
    driveLayout->addRow(readDrive);
    driveLayout->addRow(tr("Command"), driveCommandCombo);
    driveLayout->addRow(controlDrive);
    connect(readDrive, &QPushButton::clicked, this, &CANopenWorkbenchWindow::readDriveState);
    connect(controlDrive, &QPushButton::clicked, this, &CANopenWorkbenchWindow::sendDriveCommand);
    tabs->addTab(drivePage, tr("CiA 402"));
    root->addWidget(tabs, 1);
    eventLog = new QTextEdit(this);
    eventLog->setReadOnly(true);
    eventLog->setMaximumHeight(90);
    root->addWidget(eventLog);
}

void CANopenWorkbenchWindow::sendFrame(quint32 id, const QByteArray &payload, bool remote)
{
    CANFrame frame;
    frame.bus = busSpin->value();
    frame.setFrameId(id);
    frame.setPayload(payload);
    frame.setFrameType(remote ? QCanBusFrame::RemoteRequestFrame : QCanBusFrame::DataFrame);
    frame.isReceived = false;
    if (!CANConManager::getInstance()->sendFrame(frame))
        eventLog->append(tr("Could not send COB-ID 0x%1").arg(id, 3, 16, QLatin1Char('0')).toUpper());
}

void CANopenWorkbenchWindow::processFrames(CANConnection *, QVector<CANFrame> &frames)
{
    for (const CANFrame &frame : frames)
        if (frame.bus == busSpin->value() && frame.hasLocalEcho() == false) handleFrame(frame);
}

void CANopenWorkbenchWindow::handleFrame(const CANFrame &frame)
{
    const quint32 id = frame.frameId();
    const QByteArray data = frame.payload();
    if (id >= 0x701 && id <= 0x77F) handleHeartbeat(id - 0x700, data);
    else if (id >= 0x081 && id <= 0x0FF) handleEmergency(id - 0x080, data);
    else if (id >= 0x581 && id <= 0x5FF) handleSdoResponse(id - 0x580, data);
    else if (id == 0x7E4) {
        lssStatusLabel->setText(tr("LSS response: %1").arg(QString(data.toHex(' ')).toUpper()));
    }
    else if ((id >= 0x181 && id <= 0x57F) && ((id & 0x7F) != 0)) handlePdo(id, data);
    else if (id == 0x080) eventLog->append(tr("%1  SYNC received").arg(QTime::currentTime().toString()));
    else if (id == 0x100) eventLog->append(tr("%1  TIME received: %2").arg(
        QTime::currentTime().toString(), QString(data.toHex(' ')).toUpper()));
}

void CANopenWorkbenchWindow::ensureNode(int node)
{
    if (nodes.contains(node)) return;
    NodeInfo info;
    info.row = nodeTable->rowCount();
    nodes.insert(node, info);
    nodeTable->insertRow(info.row);
    for (int column = 0; column < NodeColumnCount; ++column)
        nodeTable->setItem(info.row, column, new QTableWidgetItem);
    nodeTable->item(info.row, NodeId)->setText(QString::number(node));
    nodeTable->item(info.row, NodeId)->setData(Qt::UserRole, node);
}

QString CANopenWorkbenchWindow::stateName(quint8 state) const
{
    switch (state) {
    case 0: return tr("Boot-up");
    case 4: return tr("Stopped");
    case 5: return tr("Operational");
    case 127: return tr("Pre-operational");
    default: return tr("Unknown (0x%1)").arg(state, 2, 16, QLatin1Char('0')).toUpper();
    }
}

void CANopenWorkbenchWindow::handleHeartbeat(int node, const QByteArray &payload)
{
    ensureNode(node);
    NodeInfo &info = nodes[node];
    info.heartbeatMs = QDateTime::currentMSecsSinceEpoch();
    if (!payload.isEmpty()) info.state = stateName(quint8(payload[0]) & 0x7F);
    updateNodeRow(node);
}

void CANopenWorkbenchWindow::updateNodeRow(int node)
{
    const NodeInfo &info = nodes[node];
    nodeTable->item(info.row, NodeState)->setText(info.state);
    nodeTable->item(info.row, NodeIdentity)->setText(info.identity);
    nodeTable->item(info.row, NodeEmergency)->setText(info.emergency);
}

void CANopenWorkbenchWindow::refreshAges()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = nodes.cbegin(); it != nodes.cend(); ++it)
        nodeTable->item(it->row, NodeHeartbeat)->setText(
            it->heartbeatMs ? tr("%1 ms").arg(now - it->heartbeatMs) : tr("No heartbeat"));
}

void CANopenWorkbenchWindow::scanNodes()
{
    for (int node = 1; node <= 127; ++node) sendFrame(0x700 + node, QByteArray(1, 0), true);
    statusLabel->setText(tr("Node guarding probes sent"));
}

int CANopenWorkbenchWindow::selectedNode() const { return nodeSpin->value(); }

void CANopenWorkbenchWindow::sendNmt()
{
    const int command = nmtCommandCombo->currentData().toInt();
    if ((command == 0x81 || command == 0x82)
        && QMessageBox::question(this, tr("Confirm reset"),
            tr("Reset CANopen node %1?").arg(selectedNode())) != QMessageBox::Yes) return;
    sendFrame(0x000, QByteArray({char(command), char(selectedNode())}));
}

void CANopenWorkbenchWindow::uploadObject()
{
    bool ok = false;
    const int index = number(indexEdit->text(), &ok);
    if (!ok || index < 0 || index > 0xFFFF) return;
    transfer = SdoTransfer();
    transfer.active = true;
    transfer.upload = true;
    transfer.node = selectedNode();
    transfer.index = index;
    transfer.subIndex = subIndexSpin->value();
    transfer.block = blockSdoCheck->isChecked();
    QByteArray request(8, 0);
    request[0] = transfer.block ? char(0xA4) : char(0x40);
    request[1] = char(index & 0xFF);
    request[2] = char(index >> 8);
    request[3] = char(transfer.subIndex);
    sendFrame(0x600 + transfer.node, request);
}

QByteArray CANopenWorkbenchWindow::parseBytes(const QString &text, bool *ok) const
{
    QByteArray output;
    bool valid = true;
    if (dataTypeCombo->currentIndex() == 5) output = text.toUtf8();
    else if (dataTypeCombo->currentIndex() == 0)
    {
        QString clean = text;
        clean.remove(QLatin1Char(' '));
        clean.remove(QLatin1Char(':'));
        if (clean.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) clean = clean.mid(2);
        valid = clean.size() % 2 == 0;
        output = QByteArray::fromHex(clean.toLatin1());
    }
    else
    {
        const qlonglong value = text.toLongLong(&valid, 0);
        const int bytes = dataTypeCombo->currentIndex() == 1 ? 1
            : dataTypeCombo->currentIndex() == 2 ? 2 : 4;
        for (int i = 0; i < bytes; ++i) output.append(char((quint64(value) >> (8 * i)) & 0xFF));
    }
    if (ok) *ok = valid;
    return output;
}

void CANopenWorkbenchWindow::downloadObject()
{
    bool indexOk = false, dataOk = false;
    const int index = number(indexEdit->text(), &indexOk);
    const QByteArray value = parseBytes(sdoValueEdit->text(), &dataOk);
    if (!indexOk || index < 0 || index > 0xFFFF || !dataOk) {
        QMessageBox::warning(this, tr("Invalid SDO value"), tr("Check the index, data type and value."));
        return;
    }
    transfer = SdoTransfer();
    transfer.active = true;
    transfer.upload = false;
    transfer.node = selectedNode();
    transfer.index = index;
    transfer.subIndex = subIndexSpin->value();
    transfer.pending = value;
    transfer.block = blockSdoCheck->isChecked() && value.size() > 4;
    QByteArray request(8, 0);
    if (value.size() <= 4)
    {
        request[0] = char(0x23 | ((4 - value.size()) << 2));
        request.replace(4, value.size(), value);
    }
    else if (transfer.block)
    {
        request[0] = char(0xC6);
        QByteArray size;
        appendLe32(size, value.size());
        request.replace(4, 4, size);
    }
    else
    {
        request[0] = 0x21;
        QByteArray size;
        appendLe32(size, value.size());
        request.replace(4, 4, size);
    }
    request[1] = char(index & 0xFF);
    request[2] = char(index >> 8);
    request[3] = char(transfer.subIndex);
    sendFrame(0x600 + transfer.node, request);
}

void CANopenWorkbenchWindow::handleSdoResponse(int node, const QByteArray &data)
{
    if (data.size() < 1) return;
    const quint8 command = quint8(data[0]);
    if (command == 0x80 && data.size() >= 8)
    {
        writeQueue.clear();
        finishSdo(tr("SDO abort 0x%1: %2").arg(readLe32(data, 4), 8, 16, QLatin1Char('0'))
                  .arg(abortDescription(readLe32(data, 4))).toUpper());
        return;
    }
    if (!transfer.active || node != transfer.node) return;
    if (transfer.block && transfer.upload)
    {
        if ((command & 0xE0) == 0xC0 && !transfer.blockLast && transfer.sequence == 0)
        {
            QByteArray start(8, 0);
            start[0] = char(0xA3);
            sendFrame(0x600 + transfer.node, start);
            transfer.sequence = 1;
            return;
        }
        if (!transfer.blockLast && (command & 0x7F) > 0)
        {
            const int sequence = command & 0x7F;
            transfer.data += data.mid(1, 7);
            transfer.sequence = sequence;
            const bool last = command & 0x80;
            if (last || sequence >= transfer.blockSize)
            {
                QByteArray ack(8, 0);
                ack[0] = char(0xA2);
                ack[1] = char(sequence);
                ack[2] = char(transfer.blockSize);
                sendFrame(0x600 + transfer.node, ack);
                transfer.blockLast = last;
            }
            return;
        }
        if (transfer.blockLast && (command & 0xE1) == 0xC1)
        {
            const int unused = (command >> 2) & 7;
            if (unused && transfer.data.size() >= unused) transfer.data.chop(unused);
            QByteArray end(8, 0);
            end[0] = char(0xA1);
            sendFrame(0x600 + transfer.node, end);
            setObjectValue(transfer.index, transfer.subIndex, transfer.data);
            finishSdo(tr("Block upload complete: %1 bytes").arg(transfer.data.size()));
            return;
        }
    }
    if (transfer.block && !transfer.upload)
    {
        if ((command & 0xE0) == 0xA0 && transfer.sequence == 0)
        {
            transfer.blockSize = qBound(1, data.size() > 4 ? int(quint8(data.at(4))) : 127, 127);
            sendBlockDownloadWindow();
            return;
        }
        if (command == 0xA2)
        {
            transfer.blockSize = qBound(1, data.size() > 2 ? int(quint8(data.at(2))) : 127, 127);
            if (transfer.pending.isEmpty())
            {
                QByteArray end(8, 0);
                end[0] = char(0xC1 | (transfer.lastUnused << 2));
                sendFrame(0x600 + transfer.node, end);
            }
            else sendBlockDownloadWindow();
            return;
        }
        if (command == 0xA1)
        {
            setObjectValue(transfer.index, transfer.subIndex, transfer.data);
            finishSdo(tr("Block download complete"));
            return;
        }
    }
    if (transfer.upload)
    {
        if ((command & 0xE0) == 0x40 && (command & 0x02))
        {
            const int unused = (command >> 2) & 3;
            transfer.data = data.mid(4, 4 - unused);
            setObjectValue(transfer.index, transfer.subIndex, transfer.data);
            finishSdo(tr("Upload complete: %1").arg(QString(transfer.data.toHex(' ')).toUpper()));
        }
        else if ((command & 0xE0) == 0x40) sendNextUploadSegment();
        else if ((command & 0xE0) == 0x00)
        {
            if (((command >> 4) & 1) != transfer.toggle) { finishSdo(tr("SDO toggle mismatch")); return; }
            transfer.data += data.mid(1, 7 - ((command >> 1) & 7));
            if (command & 1)
            {
                setObjectValue(transfer.index, transfer.subIndex, transfer.data);
                finishSdo(tr("Segmented upload complete: %1 bytes").arg(transfer.data.size()));
            }
            else { transfer.toggle = !transfer.toggle; sendNextUploadSegment(); }
        }
    }
    else if (command == 0x60)
    {
        if (transfer.pending.size() <= 4)
        {
            setObjectValue(transfer.index, transfer.subIndex, transfer.pending);
            finishSdo(tr("Download complete"));
        }
        else sendNextDownloadSegment();
    }
    else if ((command & 0xE0) == 0x20)
    {
        transfer.toggle = !transfer.toggle;
        if (transfer.pending.isEmpty()) {
            setObjectValue(transfer.index, transfer.subIndex, transfer.data);
            finishSdo(tr("Segmented download complete"));
        } else sendNextDownloadSegment();
    }
}

void CANopenWorkbenchWindow::sendBlockDownloadWindow()
{
    transfer.sequence = 0;
    for (int sequence = 1; sequence <= transfer.blockSize && !transfer.pending.isEmpty(); ++sequence)
    {
        const QByteArray part = transfer.pending.left(7);
        transfer.pending.remove(0, part.size());
        transfer.data += part;
        const bool last = transfer.pending.isEmpty();
        transfer.lastUnused = last ? 7 - part.size() : 0;
        QByteArray segment(8, 0);
        segment[0] = char(sequence | (last ? 0x80 : 0));
        segment.replace(1, part.size(), part);
        sendFrame(0x600 + transfer.node, segment);
        transfer.sequence = sequence;
        if (last) break;
    }
}

void CANopenWorkbenchWindow::sendNextUploadSegment()
{
    QByteArray request(8, 0);
    request[0] = transfer.toggle ? 0x70 : 0x60;
    sendFrame(0x600 + transfer.node, request);
}

void CANopenWorkbenchWindow::sendNextDownloadSegment()
{
    const QByteArray part = transfer.pending.left(7);
    transfer.pending.remove(0, part.size());
    transfer.data += part;
    const bool last = transfer.pending.isEmpty();
    QByteArray request(8, 0);
    request[0] = char((transfer.toggle ? 0x10 : 0) | ((7 - part.size()) << 1) | (last ? 1 : 0));
    request.replace(1, part.size(), part);
    sendFrame(0x600 + transfer.node, request);
}

void CANopenWorkbenchWindow::finishSdo(const QString &message)
{
    sdoLog->append(message);
    transfer.active = false;
    if (!writeQueue.isEmpty() || !uploadQueue.isEmpty())
        QTimer::singleShot(0, this, &CANopenWorkbenchWindow::startNextQueuedWrite);
}

void CANopenWorkbenchWindow::startNextQueuedWrite()
{
    if (transfer.active) return;
    if (writeQueue.isEmpty() && !uploadQueue.isEmpty())
    {
        const QPair<int, int> object = uploadQueue.dequeue();
        transfer = SdoTransfer();
        transfer.active = true;
        transfer.upload = true;
        transfer.node = selectedNode();
        transfer.index = object.first;
        transfer.subIndex = object.second;
        QByteArray request(8, 0);
        request[0] = 0x40;
        request[1] = char(transfer.index & 0xFF);
        request[2] = char(transfer.index >> 8);
        request[3] = char(transfer.subIndex);
        sendFrame(0x600 + transfer.node, request);
        return;
    }
    if (writeQueue.isEmpty()) return;
    const PendingWrite write = writeQueue.dequeue();
    transfer = SdoTransfer();
    transfer.active = true;
    transfer.upload = false;
    transfer.node = selectedNode();
    transfer.index = write.index;
    transfer.subIndex = write.subIndex;
    transfer.pending = write.value;
    QByteArray request(8, 0);
    request[0] = char(0x23 | ((4 - write.value.size()) << 2));
    request[1] = char(write.index & 0xFF);
    request[2] = char(write.index >> 8);
    request[3] = char(write.subIndex);
    request.replace(4, write.value.size(), write.value);
    sendFrame(0x600 + transfer.node, request);
}

void CANopenWorkbenchWindow::remapSelectedPdo()
{
    const int row = pdoTable->currentRow();
    if (row < 0) {
        QMessageBox::information(this, tr("Select a PDO"), tr("Select a PDO monitor row first."));
        return;
    }
    const QString pdoType = pdoTable->item(row, PdoType)->text();
    const bool transmit = pdoType.startsWith(QStringLiteral("TPDO"));
    bool ok = false;
    const int pdoNumber = pdoType.mid(4).toInt(&ok);
    if (!ok || pdoNumber < 1 || pdoNumber > 4) return;
    const QString specification = QInputDialog::getText(this, tr("PDO mapping"),
        tr("Objects as index:sub:bits, separated by commas"),
        QLineEdit::Normal, QStringLiteral("6041:00:16"), &ok);
    if (!ok) return;
    QList<quint32> mappings;
    int totalBits = 0;
    for (const QString &entry : specification.split(QLatin1Char(','), Qt::SkipEmptyParts))
    {
        const QStringList fields = entry.trimmed().split(QLatin1Char(':'));
        if (fields.size() != 3) { mappings.clear(); break; }
        bool indexOk = false, subOk = false, bitsOk = false;
        const int index = fields[0].toInt(&indexOk, 16);
        const int sub = fields[1].toInt(&subOk, 16);
        const int bits = fields[2].toInt(&bitsOk, 10);
        if (!indexOk || !subOk || !bitsOk || bits < 1 || bits > 64) { mappings.clear(); break; }
        mappings << (quint32(index) << 16 | quint32(sub) << 8 | quint32(bits));
        totalBits += bits;
    }
    if (mappings.isEmpty() || mappings.size() > 64 || totalBits > 64) {
        QMessageBox::warning(this, tr("Invalid mapping"), tr("Mappings must total no more than 64 bits."));
        return;
    }
    if (QMessageBox::warning(this, tr("Remap PDO"),
        tr("Node %1 will have %2 disabled, remapped, and enabled again. Continue?")
            .arg(selectedNode()).arg(pdoType),
        QMessageBox::Yes | QMessageBox::Cancel) != QMessageBox::Yes) return;
    const quint32 cobId = pdoTable->item(row, PdoCobId)->text().toUInt(nullptr, 0);
    const int communicationIndex = (transmit ? 0x1800 : 0x1400) + pdoNumber - 1;
    const int mappingIndex = (transmit ? 0x1A00 : 0x1600) + pdoNumber - 1;
    auto bytes = [](quint32 value, int count) {
        QByteArray result;
        for (int i = 0; i < count; ++i) result.append(char((value >> (8 * i)) & 0xFF));
        return result;
    };
    writeQueue.clear();
    writeQueue.enqueue({communicationIndex, 1, bytes(cobId | 0x80000000U, 4)});
    writeQueue.enqueue({mappingIndex, 0, bytes(0, 1)});
    for (int i = 0; i < mappings.size(); ++i)
        writeQueue.enqueue({mappingIndex, i + 1, bytes(mappings[i], 4)});
    writeQueue.enqueue({mappingIndex, 0, bytes(mappings.size(), 1)});
    writeQueue.enqueue({communicationIndex, 1, bytes(cobId, 4)});
    startNextQueuedWrite();
}

int CANopenWorkbenchWindow::ensureObjectRow(int index, int subIndex)
{
    const int existing = findObjectRow(index, subIndex);
    if (existing >= 0) return existing;
    const int row = objectTable->rowCount();
    objectTable->insertRow(row);
    for (int column = 0; column < ObjectColumnCount; ++column)
        objectTable->setItem(row, column, new QTableWidgetItem);
    objectTable->item(row, ObjectIndex)->setText(QStringLiteral("%1").arg(index, 4, 16, QLatin1Char('0')).toUpper());
    objectTable->item(row, ObjectSubIndex)->setText(QStringLiteral("%1").arg(subIndex, 2, 16, QLatin1Char('0')).toUpper());
    return row;
}

int CANopenWorkbenchWindow::findObjectRow(int index, int subIndex) const
{
    for (int row = 0; row < objectTable->rowCount(); ++row)
        if (objectTable->item(row, ObjectIndex)->text().toInt(nullptr, 16) == index
            && objectTable->item(row, ObjectSubIndex)->text().toInt(nullptr, 16) == subIndex) return row;
    return -1;
}

void CANopenWorkbenchWindow::setObjectValue(int index, int subIndex, const QByteArray &value)
{
    const int row = ensureObjectRow(index, subIndex);
    objectTable->item(row, ObjectValue)->setData(Qt::UserRole, value);
    objectTable->item(row, ObjectValue)->setText(formatObjectValue(row, value));
    if (index == 0x1018 && subIndex >= 1 && subIndex <= 4 && value.size() >= 4)
    {
        ensureNode(transfer.node);
        nodes[transfer.node].identity = tr("0x%1").arg(readLe32(value, 0), 8, 16, QLatin1Char('0')).toUpper();
        updateNodeRow(transfer.node);
    }
    if (index == 0x6041 && subIndex == 0 && value.size() >= 2)
    {
        const quint16 status = quint16(quint8(value[0])) | (quint16(quint8(value[1])) << 8);
        QString state;
        switch (status & 0x4F) {
        case 0x00: state = tr("Not ready to switch on"); break;
        case 0x40: state = tr("Switch on disabled"); break;
        case 0x21: state = tr("Ready to switch on"); break;
        case 0x23: state = tr("Switched on"); break;
        case 0x27: state = tr("Operation enabled"); break;
        case 0x07: state = tr("Quick stop active"); break;
        case 0x0F: state = tr("Fault reaction active"); break;
        case 0x08: state = tr("Fault"); break;
        default: state = tr("Unknown state"); break;
        }
        driveStateLabel->setText(tr("%1 (0x%2)").arg(state).arg(status, 4, 16, QLatin1Char('0')).toUpper());
    }
}

QString CANopenWorkbenchWindow::formatObjectValue(int row, const QByteArray &value, double *numeric) const
{
    if (numeric) *numeric = 0.0;
    QString type = objectTable->item(row, ObjectType)->text().trimmed();
    bool ok = false;
    int typeCode = type.toInt(&ok, 0);
    if (!ok) typeCode = type.toInt(&ok, 16);
    auto unsignedLe = [&]() {
        quint64 result = 0;
        for (int i = 0; i < qMin(8, value.size()); ++i) result |= quint64(quint8(value[i])) << (8 * i);
        return result;
    };
    if (typeCode == 0x0009 || typeCode == 0x000A) return QString::fromUtf8(value).remove(QChar::Null);
    const quint64 raw = unsignedLe();
    qint64 signedValue = qint64(raw);
    if (typeCode == 0x0002 && !value.isEmpty()) signedValue = qint8(value[0]);
    else if (typeCode == 0x0003 && value.size() >= 2) signedValue = qint16(raw);
    else if (typeCode == 0x0004 && value.size() >= 4) signedValue = qint32(raw);
    if (typeCode >= 0x0002 && typeCode <= 0x0004) {
        if (numeric) *numeric = signedValue;
        return QStringLiteral("%1  [0x%2]").arg(signedValue).arg(QString(value.toHex()).toUpper());
    }
    if (typeCode >= 0x0005 && typeCode <= 0x0007) {
        if (numeric) *numeric = double(raw);
        return QStringLiteral("%1  [0x%2]").arg(raw).arg(QString(value.toHex()).toUpper());
    }
    return QString(value.toHex(' ')).toUpper();
}

QString CANopenWorkbenchWindow::decodePdo(int mappingIndex, const QByteArray &payload,
                                          QMap<QString, double> *numeric) const
{
    QStringList decoded;
    int bitOffset = 0;
    for (int sub = 1; sub <= 64 && bitOffset < payload.size() * 8; ++sub)
    {
        const int mapRow = findObjectRow(mappingIndex, sub);
        if (mapRow < 0) break;
        quint32 descriptor = 0;
        const QByteArray stored = objectTable->item(mapRow, ObjectValue)->data(Qt::UserRole).toByteArray();
        if (stored.size() >= 4) descriptor = readLe32(stored, 0);
        else {
            QString text = objectTable->item(mapRow, ObjectValue)->text().section(QLatin1Char(' '), 0, 0);
            descriptor = text.toUInt(nullptr, 0);
        }
        const int bits = descriptor & 0xFF;
        const int subIndex = (descriptor >> 8) & 0xFF;
        const int index = (descriptor >> 16) & 0xFFFF;
        if (bits <= 0 || bitOffset + bits > payload.size() * 8) break;
        quint64 raw = 0;
        for (int bit = 0; bit < bits && bit < 64; ++bit)
            if (quint8(payload[(bitOffset + bit) / 8]) & (1 << ((bitOffset + bit) % 8)))
                raw |= quint64(1) << bit;
        const int objectRow = findObjectRow(index, subIndex);
        const QString name = objectRow >= 0 && !objectTable->item(objectRow, ObjectName)->text().isEmpty()
            ? objectTable->item(objectRow, ObjectName)->text()
            : QStringLiteral("%1:%2").arg(index, 4, 16, QLatin1Char('0')).arg(subIndex, 2, 16, QLatin1Char('0')).toUpper();
        const int bytes = (bits + 7) / 8;
        QByteArray rawBytes;
        for (int byte = 0; byte < bytes; ++byte) rawBytes.append(char((raw >> (byte * 8)) & 0xFF));
        double numberValue = double(raw);
        const QString value = objectRow >= 0 ? formatObjectValue(objectRow, rawBytes, &numberValue)
                                              : QString::number(raw);
        decoded << QStringLiteral("%1=%2").arg(name, value);
        if (numeric) numeric->insert(name, numberValue);
        bitOffset += bits;
    }
    return decoded.join(QStringLiteral("; "));
}

void CANopenWorkbenchWindow::handleEmergency(int node, const QByteArray &data)
{
    ensureNode(node);
    const quint16 code = data.size() >= 2 ? quint16(quint8(data[0])) | (quint16(quint8(data[1])) << 8) : 0;
    const quint8 errorRegister = data.size() >= 3 ? quint8(data[2]) : 0;
    const QString summary = tr("0x%1 / reg 0x%2").arg(code, 4, 16, QLatin1Char('0'))
        .arg(errorRegister, 2, 16, QLatin1Char('0')).toUpper();
    nodes[node].emergency = summary;
    updateNodeRow(node);
    const int row = emcyTable->rowCount();
    emcyTable->insertRow(row);
    const QStringList values = {QDateTime::currentDateTime().toString(Qt::ISODateWithMs), QString::number(node),
        QStringLiteral("0x%1").arg(code, 4, 16, QLatin1Char('0')).toUpper(),
        QStringLiteral("0x%1").arg(errorRegister, 2, 16, QLatin1Char('0')).toUpper(),
        QString(data.mid(3).toHex(' ')).toUpper(), code == 0 ? tr("Error reset") : tr("Emergency")};
    for (int column = 0; column < values.size(); ++column)
        emcyTable->setItem(row, column, new QTableWidgetItem(values[column]));
}

void CANopenWorkbenchWindow::handlePdo(quint32 cobId, const QByteArray &data)
{
    int row = pdoRows.value(cobId, -1);
    if (row < 0)
    {
        row = pdoTable->rowCount();
        pdoRows[cobId] = row;
        pdoTable->insertRow(row);
        for (int column = 0; column < PdoColumnCount; ++column)
            pdoTable->setItem(row, column, new QTableWidgetItem);
        pdoTable->item(row, PdoCount)->setData(Qt::UserRole, 0);
    }
    int base = 0, number = 0;
    for (int candidate = 1; candidate <= 4; ++candidate)
    {
        const int txBase = 0x180 + (candidate - 1) * 0x100;
        const int rxBase = 0x200 + (candidate - 1) * 0x100;
        if (cobId > quint32(txBase) && cobId <= quint32(txBase + 0x7F)) { base = txBase; number = candidate; break; }
        if (cobId > quint32(rxBase) && cobId <= quint32(rxBase + 0x7F)) { base = rxBase; number = -candidate; break; }
    }
    const int node = base ? int(cobId) - base : int(cobId & 0x7F);
    ensureNode(node);
    const int count = pdoTable->item(row, PdoCount)->data(Qt::UserRole).toInt() + 1;
    pdoTable->item(row, PdoCobId)->setText(QStringLiteral("0x%1").arg(cobId, 3, 16, QLatin1Char('0')).toUpper());
    pdoTable->item(row, PdoNode)->setText(QString::number(node));
    pdoTable->item(row, PdoType)->setText(number > 0 ? tr("TPDO%1").arg(number) : tr("RPDO%1").arg(-number));
    pdoTable->item(row, PdoData)->setText(QString(data.toHex(' ')).toUpper());
    QMap<QString, double> numeric;
    const int mappingIndex = number > 0 ? 0x1A00 + number - 1 : 0x1600 + (-number) - 1;
    const QString decoded = decodePdo(mappingIndex, data, &numeric);
    pdoTable->item(row, PdoDecoded)->setText(decoded);
    if (pdoGraph && pdoGraph->isVisible())
        for (auto it = numeric.cbegin(); it != numeric.cend(); ++it)
            pdoGraph->addSample(tr("Node %1 %2").arg(node).arg(it.key()), it.value());
    pdoTable->item(row, PdoCount)->setText(QString::number(count));
    pdoTable->item(row, PdoCount)->setData(Qt::UserRole, count);
    pdoTable->item(row, PdoUpdated)->setText(QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz")));
}

void CANopenWorkbenchWindow::showPdoGraph()
{
    if (!pdoGraph) pdoGraph = new DiagnosticGraphWindow(this);
    pdoGraph->setWindowTitle(tr("CANopen PDO Graph"));
    pdoGraph->show();
    pdoGraph->raise();
}

void CANopenWorkbenchWindow::importEds()
{
    const QString fileName = QFileDialog::getOpenFileName(this, tr("Import EDS or DCF"), QString(),
                                                           tr("CANopen descriptions (*.eds *.dcf);;All files (*)"));
    if (fileName.isEmpty()) return;
    importEdsFile(fileName);
}

bool CANopenWorkbenchWindow::importEdsFile(const QString &fileName, QString *error)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = tr("Could not open the EDS/DCF file.");
        return false;
    }
    objectTable->setRowCount(0);
    QTextStream input(&file);
    int index = -1, sub = 0;
    QMap<QString, QString> fields;
    auto flush = [&]() {
        if (index < 0) return;
        const int row = ensureObjectRow(index, sub);
        objectTable->item(row, ObjectName)->setText(fields.value(QStringLiteral("ParameterName")));
        objectTable->item(row, ObjectType)->setText(fields.value(QStringLiteral("DataType")));
        objectTable->item(row, ObjectAccess)->setText(fields.value(QStringLiteral("AccessType")));
        objectTable->item(row, ObjectDefault)->setText(fields.value(QStringLiteral("DefaultValue")));
        objectTable->item(row, ObjectValue)->setText(fields.value(QStringLiteral("ParameterValue")));
    };
    while (!input.atEnd())
    {
        const QString line = input.readLine().trimmed();
        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']')))
        {
            flush();
            fields.clear();
            QString section = line.mid(1, line.size() - 2);
            const int subMarker = section.indexOf(QStringLiteral("sub"), 0, Qt::CaseInsensitive);
            bool ok = false;
            index = section.left(subMarker < 0 ? section.size() : subMarker).toInt(&ok, 16);
            sub = subMarker < 0 ? 0 : section.mid(subMarker + 3).toInt(nullptr, 16);
            if (!ok) index = -1;
        }
        else
        {
            const int equals = line.indexOf(QLatin1Char('='));
            if (equals > 0) fields[line.left(equals).trimmed()] = line.mid(equals + 1).trimmed();
        }
    }
    flush();
    eventLog->append(tr("Imported %1 object entries from %2").arg(objectTable->rowCount()).arg(fileName));
    return true;
}

void CANopenWorkbenchWindow::exportDcf()
{
    const QString fileName = QFileDialog::getSaveFileName(this, tr("Export DCF"), QString(), tr("DCF (*.dcf)"));
    if (fileName.isEmpty()) return;
    exportDcfFile(fileName);
}

bool CANopenWorkbenchWindow::exportDcfFile(const QString &fileName, QString *error)
{
    QSaveFile file(fileName);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) *error = tr("Could not open the DCF destination.");
        return false;
    }
    QTextStream out(&file);
    out << "[FileInfo]\nFileName=" << QFileInfo(fileName).fileName() << "\nFileVersion=1\n\n";
    for (int row = 0; row < objectTable->rowCount(); ++row)
    {
        out << '[' << objectTable->item(row, ObjectIndex)->text();
        const QString sub = objectTable->item(row, ObjectSubIndex)->text();
        if (sub.toInt(nullptr, 16)) out << "sub" << sub;
        out << "]\nParameterName=" << objectTable->item(row, ObjectName)->text()
            << "\nDataType=" << objectTable->item(row, ObjectType)->text()
            << "\nAccessType=" << objectTable->item(row, ObjectAccess)->text()
            << "\nDefaultValue=" << objectTable->item(row, ObjectDefault)->text()
            << "\nParameterValue=" << objectTable->item(row, ObjectValue)->text() << "\n\n";
    }
    if (!file.commit()) {
        if (error) *error = tr("Could not commit the DCF file.");
        return false;
    }
    return true;
}

void CANopenWorkbenchWindow::addObjectEntry()
{
    int index = 0x2000;
    while (findObjectRow(index, 0) >= 0 && index < 0xFFFF) ++index;
    const int row = ensureObjectRow(index, 0);
    objectTable->item(row, ObjectName)->setText(tr("Custom object"));
    objectTable->item(row, ObjectType)->setText(QStringLiteral("0x0007"));
    objectTable->item(row, ObjectAccess)->setText(QStringLiteral("rw"));
    objectTable->item(row, ObjectValue)->setText(QStringLiteral("0"));
    objectTable->scrollToItem(objectTable->item(row, ObjectIndex));
    objectTable->selectRow(row);
    objectTable->editItem(objectTable->item(row, ObjectIndex));
}

void CANopenWorkbenchWindow::removeObjectEntries()
{
    QModelIndexList rows = objectTable->selectionModel()->selectedRows();
    std::sort(rows.begin(), rows.end(), [](const QModelIndex &left, const QModelIndex &right) {
        return left.row() > right.row();
    });
    for (const QModelIndex &index : rows) objectTable->removeRow(index.row());
}

void CANopenWorkbenchWindow::uploadSelectedObjects()
{
    if (transfer.active || !writeQueue.isEmpty()) {
        QMessageBox::information(this, tr("SDO busy"), tr("Wait for the current SDO operation to finish."));
        return;
    }
    uploadQueue.clear();
    const QModelIndexList rows = objectTable->selectionModel()->selectedRows();
    for (const QModelIndex &selected : rows)
    {
        bool indexOk = false, subOk = false;
        const int index = objectTable->item(selected.row(), ObjectIndex)->text().toInt(&indexOk, 16);
        const int sub = objectTable->item(selected.row(), ObjectSubIndex)->text().toInt(&subOk, 16);
        if (indexOk && subOk) uploadQueue.enqueue(qMakePair(index, sub));
    }
    if (uploadQueue.isEmpty()) {
        QMessageBox::information(this, tr("Select objects"), tr("Select one or more object rows first."));
        return;
    }
    startNextQueuedWrite();
}

void CANopenWorkbenchWindow::downloadSelectedObjects()
{
    if (transfer.active || !writeQueue.isEmpty() || !uploadQueue.isEmpty()) {
        QMessageBox::information(this, tr("SDO busy"), tr("Wait for the current SDO operation to finish."));
        return;
    }
    const QModelIndexList rows = objectTable->selectionModel()->selectedRows();
    for (const QModelIndex &selected : rows)
    {
        const int row = selected.row();
        const QString access = objectTable->item(row, ObjectAccess)->text().toLower();
        if (access == QStringLiteral("ro") || access == QStringLiteral("const")) continue;
        bool indexOk = false, subOk = false, valueOk = false;
        const int index = objectTable->item(row, ObjectIndex)->text().toInt(&indexOk, 16);
        const int sub = objectTable->item(row, ObjectSubIndex)->text().toInt(&subOk, 16);
        const QByteArray value = objectRowBytes(row, &valueOk);
        if (indexOk && subOk && valueOk && !value.isEmpty()) writeQueue.enqueue({index, sub, value});
        else {
            QMessageBox::warning(this, tr("Invalid object value"),
                tr("Could not encode row %1. Check its index, sub-index, data type, and value.").arg(row + 1));
            writeQueue.clear();
            return;
        }
    }
    if (writeQueue.isEmpty()) {
        QMessageBox::information(this, tr("No writable objects"),
            tr("Select one or more writable object rows first."));
        return;
    }
    if (QMessageBox::question(this, tr("Write object list"),
        tr("Write %1 selected object values to node %2?").arg(writeQueue.size()).arg(selectedNode()))
        != QMessageBox::Yes) { writeQueue.clear(); return; }
    startNextQueuedWrite();
}

QByteArray CANopenWorkbenchWindow::objectRowBytes(int row, bool *ok) const
{
    QString type = objectTable->item(row, ObjectType)->text().trimmed().toUpper();
    QString value = objectTable->item(row, ObjectValue)->text().trimmed();
    bool typeOk = false;
    int code = type.toInt(&typeOk, 0);
    if (!typeOk) code = type.toInt(&typeOk, 16);
    if (!typeOk)
    {
        if (type.contains(QStringLiteral("UNSIGNED8"))) code = 0x0005;
        else if (type.contains(QStringLiteral("UNSIGNED16"))) code = 0x0006;
        else if (type.contains(QStringLiteral("UNSIGNED32"))) code = 0x0007;
        else if (type.contains(QStringLiteral("INTEGER8"))) code = 0x0002;
        else if (type.contains(QStringLiteral("INTEGER16"))) code = 0x0003;
        else if (type.contains(QStringLiteral("INTEGER32"))) code = 0x0004;
        else if (type.contains(QStringLiteral("STRING"))) code = 0x0009;
    }
    if (code == 0x0009 || code == 0x000A) { if (ok) *ok = true; return value.toUtf8(); }
    const int bracket = value.indexOf(QStringLiteral("  [0x"));
    if (bracket > 0) value.truncate(bracket);
    bool valueOk = false;
    const qlonglong numberValue = value.toLongLong(&valueOk, 0);
    int bytes = (code == 0x0002 || code == 0x0005) ? 1
        : (code == 0x0003 || code == 0x0006) ? 2
        : (code == 0x0004 || code == 0x0007) ? 4 : 0;
    QByteArray result;
    if (bytes && valueOk)
        for (int byte = 0; byte < bytes; ++byte)
            result.append(char((quint64(numberValue) >> (byte * 8)) & 0xFF));
    else
    {
        QString hex = value;
        hex.remove(QLatin1Char(' '));
        if (hex.startsWith(QStringLiteral("0X"))) hex = hex.mid(2);
        valueOk = !hex.isEmpty() && hex.size() % 2 == 0;
        result = QByteArray::fromHex(hex.toLatin1());
    }
    if (ok) *ok = valueOk;
    return result;
}

void CANopenWorkbenchWindow::sendSync()
{
    bool ok = false; const int id = number(syncCobIdEdit->text(), &ok);
    if (ok) sendFrame(id, QByteArray());
}

void CANopenWorkbenchWindow::sendTime()
{
    bool ok = false; const int id = number(timeCobIdEdit->text(), &ok);
    if (!ok) return;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QDate epoch(1984, 1, 1);
    const quint16 days = epoch.daysTo(now.date());
    const quint32 milliseconds = QTime(0, 0).msecsTo(now.time());
    QByteArray payload; appendLe32(payload, milliseconds);
    payload.append(char(days & 0xFF)); payload.append(char(days >> 8));
    sendFrame(id, payload);
}

void CANopenWorkbenchWindow::discoverLss()
{
    QByteArray request(8, 0);
    request[0] = 0x4C;
    sendFrame(0x7E5, request);
    lssStatusLabel->setText(tr("Waiting for an unconfigured LSS node"));
}

void CANopenWorkbenchWindow::configureLss()
{
    if (QMessageBox::warning(this, tr("Configure LSS node"),
        tr("This changes the selected unconfigured device to node ID %1 and stores its bitrate. "
           "Continue only with exactly one LSS device in configuration mode.").arg(lssNodeSpin->value()),
        QMessageBox::Yes | QMessageBox::Cancel) != QMessageBox::Yes) return;
    QByteArray request(8, 0);
    request[0] = 0x04; request[1] = 0x01;
    sendFrame(0x7E5, request);
    request.fill(0); request[0] = 0x11; request[1] = char(lssNodeSpin->value());
    sendFrame(0x7E5, request);
    request.fill(0); request[0] = 0x13; request[1] = 0; request[2] = char(lssBitrateCombo->currentData().toInt());
    sendFrame(0x7E5, request);
    request.fill(0); request[0] = 0x17;
    sendFrame(0x7E5, request);
    request.fill(0); request[0] = 0x15; request[1] = char(100 & 0xFF); request[2] = char(100 >> 8);
    sendFrame(0x7E5, request);
    request.fill(0); request[0] = 0x04; request[1] = 0;
    sendFrame(0x7E5, request);
    lssStatusLabel->setText(tr("Configuration commands sent; reconnect at the selected bitrate"));
}

void CANopenWorkbenchWindow::readDriveState()
{
    indexEdit->setText(QStringLiteral("0x6041"));
    subIndexSpin->setValue(0);
    uploadObject();
}

void CANopenWorkbenchWindow::sendDriveCommand()
{
    if (QMessageBox::question(this, tr("Send CiA 402 command"),
        tr("Send '%1' to node %2?").arg(driveCommandCombo->currentText()).arg(selectedNode()))
        != QMessageBox::Yes) return;
    indexEdit->setText(QStringLiteral("0x6040"));
    subIndexSpin->setValue(0);
    dataTypeCombo->setCurrentIndex(2);
    sdoValueEdit->setText(QString::number(driveCommandCombo->currentData().toInt()));
    downloadObject();
}

int CANopenWorkbenchWindow::number(const QString &text, bool *ok) const
{
    QString value = text.trimmed();
    const bool hex = value.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive);
    if (hex) value = value.mid(2);
    return value.toInt(ok, hex ? 16 : 10);
}

QString CANopenWorkbenchWindow::abortDescription(quint32 code) const
{
    static const QMap<quint32, QString> descriptions = {
        {0x05030000, tr("Toggle bit not alternated")}, {0x05040000, tr("SDO protocol timed out")},
        {0x05040001, tr("Invalid command specifier")}, {0x06010000, tr("Unsupported access")},
        {0x06010001, tr("Attempt to read a write-only object")}, {0x06010002, tr("Attempt to write a read-only object")},
        {0x06020000, tr("Object does not exist")}, {0x06040041, tr("Object cannot be mapped to PDO")},
        {0x06070010, tr("Data type or length mismatch")}, {0x06090011, tr("Sub-index does not exist")},
        {0x06090030, tr("Value range exceeded")}, {0x08000000, tr("General error")}
    };
    return descriptions.value(code, tr("Unknown abort code"));
}

void CANopenWorkbenchWindow::loadSettings()
{
    QSettings settings;
    busSpin->setValue(settings.value(QStringLiteral("CANopenWorkbench/Bus"), 0).toInt());
    nodeSpin->setValue(settings.value(QStringLiteral("CANopenWorkbench/Node"), 1).toInt());
}

void CANopenWorkbenchWindow::saveSettings() const
{
    QSettings settings;
    settings.setValue(QStringLiteral("CANopenWorkbench/Bus"), busSpin->value());
    settings.setValue(QStringLiteral("CANopenWorkbench/Node"), nodeSpin->value());
}
