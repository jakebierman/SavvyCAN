#include "udsworkbenchwindow.h"

#include "connections/canconmanager.h"
#include "payloadformatter.h"
#include "diagnosticgraphwindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QTextStream>
#include <QVBoxLayout>

UDSWorkbenchWindow::UDSWorkbenchWindow(QWidget *parent) : QDialog(parent)
{
    udsHandler = new UDS_HANDLER;
    buildUi();
    loadSettings();
    responseTimer.setSingleShot(true);
    responseTimer.setInterval(1500);
    testerTimer.setInterval(2000);
    pollingTimer.setSingleShot(false);
    connect(udsHandler, &UDS_HANDLER::newUDSMessage, this, &UDSWorkbenchWindow::gotUDSReply);
    connect(&responseTimer, &QTimer::timeout, this, &UDSWorkbenchWindow::requestTimedOut);
    connect(&testerTimer, &QTimer::timeout, this, &UDSWorkbenchWindow::sendTesterPresent);
    connect(&pollingTimer, &QTimer::timeout, this, &UDSWorkbenchWindow::pollEnabledDids);
}

UDSWorkbenchWindow::~UDSWorkbenchWindow()
{
    saveSettings();
    udsHandler->clearAllFilters();
    udsHandler->setReception(false);
    if (csvLogFile) { csvLogFile->close(); delete csvLogFile; }
    delete udsHandler;
}

void UDSWorkbenchWindow::buildUi()
{
    setWindowTitle(tr("UDS Workbench"));
    resize(1050, 680);
    QVBoxLayout *root = new QVBoxLayout(this);

    QGroupBox *endpoint = new QGroupBox(tr("Diagnostic endpoint"), this);
    QHBoxLayout *endpointLayout = new QHBoxLayout(endpoint);
    busSpin = new QSpinBox(endpoint);
    busSpin->setRange(0, qMax(0, CANConManager::getInstance()->getNumBuses() - 1));
    requestIdEdit = new QLineEdit(QStringLiteral("0x7E0"), endpoint);
    responseIdEdit = new QLineEdit(QStringLiteral("0x7E8"), endpoint);
    sessionCombo = new QComboBox(endpoint);
    sessionCombo->addItem(tr("Default (0x01)"), 1);
    sessionCombo->addItem(tr("Programming (0x02)"), 2);
    sessionCombo->addItem(tr("Extended (0x03)"), 3);
    sessionCombo->addItem(tr("Safety (0x04)"), 4);
    testerPresentCheck = new QCheckBox(tr("Tester Present"), endpoint);
    connectButton = new QPushButton(tr("Connect session"), endpoint);
    connectionStatus = new QLabel(tr("Disconnected"), endpoint);
    endpointLayout->addWidget(new QLabel(tr("Bus"), endpoint));
    endpointLayout->addWidget(busSpin);
    endpointLayout->addWidget(new QLabel(tr("Request ID"), endpoint));
    endpointLayout->addWidget(requestIdEdit);
    endpointLayout->addWidget(new QLabel(tr("Response ID"), endpoint));
    endpointLayout->addWidget(responseIdEdit);
    endpointLayout->addWidget(sessionCombo);
    endpointLayout->addWidget(testerPresentCheck);
    endpointLayout->addWidget(connectButton);
    endpointLayout->addWidget(connectionStatus);
    root->addWidget(endpoint);
    connect(connectButton, &QPushButton::clicked, this, [this]() {
        if (endpointConnected || responseTimer.isActive()) disconnectEndpoint(); else connectEndpoint();
    });
    connect(testerPresentCheck, &QCheckBox::toggled, this, [this](bool enabled) {
        if (enabled && endpointConnected) testerTimer.start(); else testerTimer.stop();
    });

    QTabWidget *tabs = new QTabWidget(this);
    QWidget *didPage = new QWidget(tabs);
    QVBoxLayout *didLayout = new QVBoxLayout(didPage);
    didTable = new QTableWidget(0, DidColumnCount, didPage);
    didTable->setHorizontalHeaderLabels({tr("Use"), tr("Name"), tr("DID"), tr("Payload format"), tr("Poll ms"),
                                         tr("Raw response"), tr("Decoded"), tr("Status"), tr("Updated")});
    didTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    didTable->horizontalHeader()->setSectionResizeMode(DidDecoded, QHeaderView::Stretch);
    didTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    didLayout->addWidget(didTable);
    QHBoxLayout *didButtons = new QHBoxLayout;
    QPushButton *addButton = new QPushButton(tr("Add DID"), didPage);
    QPushButton *removeButton = new QPushButton(tr("Remove"), didPage);
    QPushButton *importButton = new QPushButton(tr("Load DID list"), didPage);
    QPushButton *exportButton = new QPushButton(tr("Save DID list"), didPage);
    QPushButton *selectedButton = new QPushButton(tr("Request selected"), didPage);
    QPushButton *allButton = new QPushButton(tr("Request enabled"), didPage);
    QPushButton *graphButton = new QPushButton(tr("Live graph"), didPage);
    csvLogButton = new QPushButton(tr("Start CSV log"), didPage);
    pollingCheck = new QCheckBox(tr("Poll"), didPage);
    pollIntervalSpin = new QSpinBox(didPage);
    pollIntervalSpin->setRange(100, 60000);
    pollIntervalSpin->setSingleStep(100);
    pollIntervalSpin->setSuffix(tr(" ms"));
    pollIntervalSpin->setValue(1000);
    didButtons->addWidget(addButton);
    didButtons->addWidget(removeButton);
    didButtons->addWidget(importButton);
    didButtons->addWidget(exportButton);
    didButtons->addStretch();
    didButtons->addWidget(pollingCheck);
    didButtons->addWidget(pollIntervalSpin);
    didButtons->addWidget(csvLogButton);
    didButtons->addWidget(graphButton);
    didButtons->addWidget(selectedButton);
    didButtons->addWidget(allButton);
    didLayout->addLayout(didButtons);
    connect(addButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::addDid);
    connect(removeButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::removeDid);
    connect(importButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::importProfile);
    connect(exportButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::exportProfile);
    connect(selectedButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::requestSelectedDid);
    connect(allButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::requestAllDids);
    connect(csvLogButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::toggleCsvLogging);
    connect(graphButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::showDiagnosticGraph);
    connect(pollingCheck, &QCheckBox::toggled, this, [this](bool enabled) {
        if (enabled) pollingTimer.start(100); else pollingTimer.stop();
    });
    tabs->addTab(didPage, tr("DID requests"));

    QWidget *discoveryPage = new QWidget(tabs);
    QVBoxLayout *discoveryLayout = new QVBoxLayout(discoveryPage);
    QHBoxLayout *scanControls = new QHBoxLayout;
    scanStartEdit = new QLineEdit(QStringLiteral("0xF180"), discoveryPage);
    scanEndEdit = new QLineEdit(QStringLiteral("0xF1FF"), discoveryPage);
    scanTimeoutSpin = new QSpinBox(discoveryPage);
    scanTimeoutSpin->setRange(20, 5000);
    scanTimeoutSpin->setValue(100);
    scanTimeoutSpin->setSuffix(tr(" ms"));
    scanStartButton = new QPushButton(tr("Scan DIDs"), discoveryPage);
    scanResumeButton = new QPushButton(tr("Resume"), discoveryPage);
    scanResumeButton->setEnabled(false);
    scanStopButton = new QPushButton(tr("Stop"), discoveryPage);
    scanStopButton->setEnabled(false);
    scanControls->addWidget(new QLabel(tr("Start DID"), discoveryPage));
    scanControls->addWidget(scanStartEdit);
    scanControls->addWidget(new QLabel(tr("End DID"), discoveryPage));
    scanControls->addWidget(scanEndEdit);
    scanControls->addWidget(new QLabel(tr("Timeout"), discoveryPage));
    scanControls->addWidget(scanTimeoutSpin);
    scanControls->addWidget(scanStartButton);
    scanControls->addWidget(scanResumeButton);
    scanControls->addWidget(scanStopButton);
    discoveryLayout->addLayout(scanControls);
    scanProgress = new QProgressBar(discoveryPage);
    scanProgress->setValue(0);
    discoveryLayout->addWidget(scanProgress);
    scanResults = new QListWidget(discoveryPage);
    scanResults->setSelectionMode(QAbstractItemView::ExtendedSelection);
    discoveryLayout->addWidget(scanResults, 1);
    QHBoxLayout *scanResultButtons = new QHBoxLayout;
    QPushButton *loadScanButton = new QPushButton(tr("Load results"), discoveryPage);
    QPushButton *saveScanButton = new QPushButton(tr("Save results"), discoveryPage);
    QPushButton *addScannedButton = new QPushButton(tr("Add selected to DID requests"), discoveryPage);
    scanResultButtons->addWidget(loadScanButton);
    scanResultButtons->addWidget(saveScanButton);
    scanResultButtons->addStretch();
    scanResultButtons->addWidget(addScannedButton);
    discoveryLayout->addLayout(scanResultButtons);
    connect(scanStartButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::startDidScan);
    connect(scanResumeButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::resumeDidScan);
    connect(scanStopButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::stopDidScan);
    connect(loadScanButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::loadDidScan);
    connect(saveScanButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::saveDidScan);
    connect(addScannedButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::addSelectedScannedDids);
    tabs->addTab(discoveryPage, tr("DID discovery"));

    QWidget *serviceDiscoveryPage = new QWidget(tabs);
    QVBoxLayout *serviceDiscoveryLayout = new QVBoxLayout(serviceDiscoveryPage);
    QLabel *serviceScanDescription = new QLabel(
        tr("Probes standard service identifiers with an intentionally incomplete request. "
           "A response other than ServiceNotSupported indicates that the ECU recognizes the service."),
        serviceDiscoveryPage);
    serviceScanDescription->setWordWrap(true);
    serviceDiscoveryLayout->addWidget(serviceScanDescription);
    serviceScanResults = new QListWidget(serviceDiscoveryPage);
    serviceDiscoveryLayout->addWidget(serviceScanResults, 1);
    QHBoxLayout *serviceScanButtons = new QHBoxLayout;
    serviceScanStartButton = new QPushButton(tr("Scan services"), serviceDiscoveryPage);
    serviceScanStopButton = new QPushButton(tr("Stop"), serviceDiscoveryPage);
    serviceScanStopButton->setEnabled(false);
    serviceScanButtons->addStretch();
    serviceScanButtons->addWidget(serviceScanStartButton);
    serviceScanButtons->addWidget(serviceScanStopButton);
    serviceDiscoveryLayout->addLayout(serviceScanButtons);
    connect(serviceScanStartButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::startServiceScan);
    connect(serviceScanStopButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::stopServiceScan);
    tabs->addTab(serviceDiscoveryPage, tr("Service discovery"));

    QWidget *dtcPage = new QWidget(tabs);
    QVBoxLayout *dtcLayout = new QVBoxLayout(dtcPage);
    QHBoxLayout *dtcControls = new QHBoxLayout;
    dtcStatusMaskEdit = new QLineEdit(QStringLiteral("0xFF"), dtcPage);
    QPushButton *readDtcsButton = new QPushButton(tr("Read DTCs"), dtcPage);
    QPushButton *clearDtcsButton = new QPushButton(tr("Clear all DTCs"), dtcPage);
    dtcControls->addWidget(new QLabel(tr("Status mask"), dtcPage));
    dtcControls->addWidget(dtcStatusMaskEdit);
    dtcControls->addWidget(readDtcsButton);
    dtcControls->addStretch();
    dtcControls->addWidget(clearDtcsButton);
    dtcLayout->addLayout(dtcControls);
    QHBoxLayout *dtcDetailControls = new QHBoxLayout;
    dtcDetailTypeCombo = new QComboBox(dtcPage);
    dtcDetailTypeCombo->addItem(tr("Snapshot identification (0x03)"), 0x03);
    dtcDetailTypeCombo->addItem(tr("Snapshot by DTC (0x04)"), 0x04);
    dtcDetailTypeCombo->addItem(tr("Extended data by DTC (0x06)"), 0x06);
    dtcDetailTypeCombo->addItem(tr("Severity information (0x09)"), 0x09);
    dtcDetailTypeCombo->addItem(tr("Supported DTCs (0x0A)"), 0x0A);
    dtcDetailDataEdit = new QLineEdit(dtcPage);
    dtcDetailDataEdit->setPlaceholderText(tr("Bytes after subfunction, e.g. 12 34 56 FF"));
    QPushButton *dtcDetailButton = new QPushButton(tr("Request details"), dtcPage);
    dtcDetailControls->addWidget(dtcDetailTypeCombo);
    dtcDetailControls->addWidget(dtcDetailDataEdit, 1);
    dtcDetailControls->addWidget(dtcDetailButton);
    dtcLayout->addLayout(dtcDetailControls);
    dtcResponse = new QTextEdit(dtcPage);
    dtcResponse->setReadOnly(true);
    dtcLayout->addWidget(dtcResponse);
    connect(readDtcsButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::requestDtcs);
    connect(dtcDetailButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::sendDetailedDtcRequest);
    connect(clearDtcsButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::clearDtcs);
    tabs->addTab(dtcPage, tr("DTCs"));

    QWidget *routinePage = new QWidget(tabs);
    QVBoxLayout *routineLayout = new QVBoxLayout(routinePage);
    QFormLayout *routineForm = new QFormLayout;
    routineTypeCombo = new QComboBox(routinePage);
    routineTypeCombo->addItem(tr("Start routine (0x01)"), 1);
    routineTypeCombo->addItem(tr("Stop routine (0x02)"), 2);
    routineTypeCombo->addItem(tr("Request results (0x03)"), 3);
    routineIdEdit = new QLineEdit(QStringLiteral("0x0000"), routinePage);
    routineDataEdit = new QLineEdit(routinePage);
    routineDataEdit->setPlaceholderText(tr("Optional hex bytes"));
    routineForm->addRow(tr("Control"), routineTypeCombo);
    routineForm->addRow(tr("Routine ID"), routineIdEdit);
    routineForm->addRow(tr("Option data"), routineDataEdit);
    routineLayout->addLayout(routineForm);
    QPushButton *routineButton = new QPushButton(tr("Send Routine Control"), routinePage);
    routineLayout->addWidget(routineButton, 0, Qt::AlignRight);
    routineResponse = new QTextEdit(routinePage);
    routineResponse->setReadOnly(true);
    routineLayout->addWidget(routineResponse);
    connect(routineButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::sendRoutineControl);
    tabs->addTab(routinePage, tr("Routine Control"));

    QWidget *controlPage = new QWidget(tabs);
    QVBoxLayout *controlLayout = new QVBoxLayout(controlPage);
    QFormLayout *controlForm = new QFormLayout;
    controlServiceCombo = new QComboBox(controlPage);
    controlServiceCombo->addItem(tr("ECU Reset (0x11)"), UDS_SERVICES::ECU_RESET);
    controlServiceCombo->addItem(tr("Communication Control (0x28)"), UDS_SERVICES::COMM_CTRL);
    controlServiceCombo->addItem(tr("Write Data By Identifier (0x2E)"), UDS_SERVICES::WRITE_BY_ID);
    controlServiceCombo->addItem(tr("Input Output Control (0x2F)"), UDS_SERVICES::IO_CTRL);
    controlDataEdit = new QLineEdit(controlPage);
    controlDataEdit->setPlaceholderText(tr("Complete service payload as hex bytes"));
    controlForm->addRow(tr("Operation"), controlServiceCombo);
    controlForm->addRow(tr("Payload"), controlDataEdit);
    controlLayout->addLayout(controlForm);
    QPushButton *controlButton = new QPushButton(tr("Review and send"), controlPage);
    controlLayout->addWidget(controlButton, 0, Qt::AlignRight);
    controlResponse = new QTextEdit(controlPage);
    controlResponse->setReadOnly(true);
    controlLayout->addWidget(controlResponse);
    connect(controlButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::sendGuardedControl);
    tabs->addTab(controlPage, tr("ECU controls"));

    QWidget *securityPage = new QWidget(tabs);
    QVBoxLayout *securityLayout = new QVBoxLayout(securityPage);
    QFormLayout *securityForm = new QFormLayout;
    securityLevelSpin = new QSpinBox(securityPage);
    securityLevelSpin->setRange(1, 0x7D);
    securityLevelSpin->setSingleStep(2);
    securityKeyEdit = new QLineEdit(securityPage);
    securityKeyEdit->setPlaceholderText(tr("Key bytes supplied by an authorised algorithm"));
    securityForm->addRow(tr("Seed level"), securityLevelSpin);
    securityForm->addRow(tr("Key"), securityKeyEdit);
    securityLayout->addLayout(securityForm);
    QHBoxLayout *securityButtons = new QHBoxLayout;
    QPushButton *seedButton = new QPushButton(tr("Request seed"), securityPage);
    QPushButton *keyButton = new QPushButton(tr("Send key"), securityPage);
    securityButtons->addStretch();
    securityButtons->addWidget(seedButton);
    securityButtons->addWidget(keyButton);
    securityLayout->addLayout(securityButtons);
    securityResponse = new QTextEdit(securityPage);
    securityResponse->setReadOnly(true);
    securityLayout->addWidget(securityResponse);
    connect(seedButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::requestSecuritySeed);
    connect(keyButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::sendSecurityKey);
    tabs->addTab(securityPage, tr("Security Access"));

    QWidget *manualPage = new QWidget(tabs);
    QVBoxLayout *manualLayout = new QVBoxLayout(manualPage);
    QFormLayout *manualForm = new QFormLayout;
    manualServiceEdit = new QLineEdit(QStringLiteral("0x22"), manualPage);
    manualPayloadEdit = new QLineEdit(manualPage);
    manualPayloadEdit->setPlaceholderText(tr("Bytes after service, for example F1 90"));
    manualForm->addRow(tr("Service"), manualServiceEdit);
    manualForm->addRow(tr("Request data"), manualPayloadEdit);
    manualLayout->addLayout(manualForm);
    QPushButton *sendManual = new QPushButton(tr("Send request"), manualPage);
    manualLayout->addWidget(sendManual, 0, Qt::AlignRight);
    manualResponse = new QTextEdit(manualPage);
    manualResponse->setReadOnly(true);
    manualLayout->addWidget(manualResponse);
    connect(sendManual, &QPushButton::clicked, this, &UDSWorkbenchWindow::sendManualRequest);
    tabs->addTab(manualPage, tr("Manual service"));
    root->addWidget(tabs, 1);

    eventLog = new QTextEdit(this);
    eventLog->setReadOnly(true);
    eventLog->setMaximumHeight(130);
    root->addWidget(eventLog);
}

uint32_t UDSWorkbenchWindow::parseNumber(const QString &text, bool *ok) const
{
    QString value = text.trimmed();
    int base = 10;
    if (value.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) { value.remove(0, 2); base = 16; }
    return value.toUInt(ok, base);
}

void UDSWorkbenchWindow::connectEndpoint()
{
    bool requestOk = false, responseOk = false;
    const uint32_t requestId = parseNumber(requestIdEdit->text(), &requestOk);
    const uint32_t responseId = parseNumber(responseIdEdit->text(), &responseOk);
    if (!requestOk || !responseOk) { connectionStatus->setText(tr("Invalid ID")); return; }
    if (!CANConManager::getInstance()->isBusConnected(busSpin->value())) {
        endpointConnected = false;
        connectionStatus->setText(tr("Bus %1 is not connected").arg(busSpin->value()));
        log(tr("Cannot start UDS session: selected CAN bus is not connected"));
        return;
    }
    udsHandler->clearAllFilters();
    udsHandler->addFilter(busSpin->value(), responseId, requestId > 0x7FF || responseId > 0x7FF ? 0x1FFFFFFF : 0x7FF);
    udsHandler->setReception(true);
    UDS_MESSAGE message;
    message.bus = busSpin->value();
    message.setFrameId(requestId);
    message.setExtendedFrameFormat(requestId > 0x7FF);
    message.service = UDS_SERVICES::DIAG_CONTROL;
    message.subFuncLen = 1;
    message.subFunc = sessionCombo->currentData().toInt();
    activeService = message.service;
    requestContext = ContextSession;
    if (!udsHandler->sendUDSFrame(message)) {
        endpointConnected = false;
        connectionStatus->setText(tr("Session transmit failed"));
        log(tr("UDS session request was rejected by the CAN interface"));
        return;
    }
    responseTimer.start();
    endpointConnected = false;
    connectButton->setText(tr("Disconnect"));
    connectionStatus->setText(tr("Session requested"));
    log(tr("Requested diagnostic session 0x%1").arg(message.subFunc, 2, 16, QLatin1Char('0')));
}

void UDSWorkbenchWindow::disconnectEndpoint()
{
    responseTimer.stop();
    testerTimer.stop();
    pollingTimer.stop();
    if (pollingCheck->isChecked()) pollingCheck->setChecked(false);
    didQueue.clear();
    activeDidRow = -1;
    activeService = -1;
    requestContext = ContextNone;
    udsHandler->clearAllFilters();
    udsHandler->setReception(false);
    endpointConnected = false;
    connectButton->setText(tr("Connect session"));
    connectionStatus->setText(tr("Disconnected"));
    log(tr("Stopped UDS session activity and response listening"));
}

void UDSWorkbenchWindow::addDid()
{
    const int row = didTable->rowCount();
    didTable->insertRow(row);
    QTableWidgetItem *enabled = new QTableWidgetItem;
    enabled->setCheckState(Qt::Checked);
    didTable->setItem(row, DidEnabled, enabled);
    didTable->setItem(row, DidName, new QTableWidgetItem(tr("DID %1").arg(row + 1)));
    didTable->setItem(row, DidIdentifier, new QTableWidgetItem(QStringLiteral("0xF190")));
    didTable->setItem(row, DidFormat, new QTableWidgetItem(QStringLiteral("u8")));
    didTable->setItem(row, DidPollMs, new QTableWidgetItem(QStringLiteral("0")));
    didTable->setItem(row, DidRaw, new QTableWidgetItem);
    didTable->setItem(row, DidDecoded, new QTableWidgetItem);
    didTable->setItem(row, DidStatus, new QTableWidgetItem(tr("Ready")));
    didTable->setItem(row, DidUpdated, new QTableWidgetItem);
}

bool UDSWorkbenchWindow::addDidRequest(const QString &name, const QString &did,
                                       const QString &format, int pollMs, QString *error)
{
    bool ok = false;
    const uint32_t identifier = parseNumber(did, &ok);
    if (!ok || identifier > 0xFFFF)
    {
        if (error) *error = tr("DID must be between 0x0000 and 0xFFFF.");
        return false;
    }
    if (format.trimmed().isEmpty())
    {
        if (error) *error = tr("Payload format cannot be empty.");
        return false;
    }
    if (pollMs < 0)
    {
        if (error) *error = tr("Poll interval cannot be negative.");
        return false;
    }
    addDid();
    const int row = didTable->rowCount() - 1;
    didTable->item(row, DidName)->setText(name.trimmed().isEmpty()
        ? tr("DID 0x%1").arg(identifier, 4, 16, QLatin1Char('0')).toUpper() : name.trimmed());
    didTable->item(row, DidIdentifier)->setText(
        QStringLiteral("0x%1").arg(identifier, 4, 16, QLatin1Char('0')).toUpper());
    didTable->item(row, DidFormat)->setText(format.trimmed());
    didTable->item(row, DidPollMs)->setText(QString::number(pollMs));
    didTable->selectRow(row);
    return true;
}

bool UDSWorkbenchWindow::executeAIRequest(const QString &operation,
                                          const QJsonObject &arguments, QString *error)
{
    if (!endpointConnected || responseTimer.isActive()) {
        if (error) *error = tr("Connect the UDS endpoint and wait for any active request.");
        return false;
    }
    if (operation == QStringLiteral("read_did")) {
        bool requestedOk = false;
        const uint32_t requested = parseNumber(arguments.value("did").toString(), &requestedOk);
        for (int row = 0; requestedOk && row < didTable->rowCount(); ++row) {
            bool rowOk = false;
            if (parseNumber(didTable->item(row, DidIdentifier)->text(), &rowOk) == requested && rowOk) {
                didTable->setCurrentCell(row, DidIdentifier);
                requestSelectedDid();
                return responseTimer.isActive();
            }
        }
        if (error) *error = tr("The requested DID is not in the UDS list.");
        return false;
    }
    if (operation == QStringLiteral("scan_dids")) {
        scanStartEdit->setText(arguments.value("start_did").toString());
        scanEndEdit->setText(arguments.value("end_did").toString());
        startDidScan();
        return didScanActive;
    }
    if (operation == QStringLiteral("dtc")) { requestDtcs(); return responseTimer.isActive(); }
    if (error) *error = tr("Unsupported UDS AI operation: %1").arg(operation);
    return false;
}

void UDSWorkbenchWindow::removeDid()
{
    if (didTable->currentRow() >= 0) didTable->removeRow(didTable->currentRow());
}

void UDSWorkbenchWindow::importProfile()
{
    const QString fileName = QFileDialog::getOpenFileName(this, tr("Import UDS profile"), QString(),
                                                           tr("UDS profiles (*.json);;All files (*)"));
    if (fileName.isEmpty()) return;
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) { log(tr("Could not open profile: %1").arg(file.errorString())); return; }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        log(tr("Invalid profile: %1").arg(parseError.errorString()));
        return;
    }
    const QJsonObject profile = document.object();
    if (!profile.value("dids").isArray()) { log(tr("Profile has no DID list")); return; }
    busSpin->setValue(profile.value("bus").toInt(busSpin->value()));
    requestIdEdit->setText(profile.value("requestId").toString(requestIdEdit->text()));
    responseIdEdit->setText(profile.value("responseId").toString(responseIdEdit->text()));
    const int session = profile.value("session").toInt(sessionCombo->currentData().toInt());
    const int sessionIndex = sessionCombo->findData(session);
    if (sessionIndex >= 0) sessionCombo->setCurrentIndex(sessionIndex);
    loadDidRows(profile.value("dids").toArray());
    endpointConnected = false;
    testerTimer.stop();
    connectionStatus->setText(tr("Reconnect imported endpoint"));
    log(tr("Imported %1 DIDs from %2").arg(didTable->rowCount()).arg(fileName));
}

void UDSWorkbenchWindow::exportProfile()
{
    QString fileName = QFileDialog::getSaveFileName(this, tr("Export UDS profile"), QString(),
                                                     tr("UDS profiles (*.json);;All files (*)"));
    if (fileName.isEmpty()) return;
    if (!fileName.endsWith(".json", Qt::CaseInsensitive)) fileName += ".json";
    QJsonObject profile;
    profile["version"] = 1;
    profile["bus"] = busSpin->value();
    profile["requestId"] = requestIdEdit->text();
    profile["responseId"] = responseIdEdit->text();
    profile["session"] = sessionCombo->currentData().toInt();
    profile["dids"] = didRowsToJson();
    QSaveFile file(fileName);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(profile).toJson()) < 0 || !file.commit())
    {
        log(tr("Could not save profile: %1").arg(file.errorString()));
        return;
    }
    log(tr("Exported %1 DIDs to %2").arg(didTable->rowCount()).arg(fileName));
}

void UDSWorkbenchWindow::pollEnabledDids()
{
    if (!endpointConnected || responseTimer.isActive() || activeDidRow >= 0 || !didQueue.isEmpty()) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (int row = 0; row < didTable->rowCount(); ++row)
    {
        if (didTable->item(row, DidEnabled)->checkState() != Qt::Checked) continue;
        bool intervalOk = false;
        int interval = didTable->item(row, DidPollMs)->text().toInt(&intervalOk);
        if (!intervalOk || interval <= 0) interval = pollIntervalSpin->value();
        const qint64 lastRequest = didTable->item(row, DidPollMs)->data(Qt::UserRole).toLongLong();
        if (lastRequest == 0 || now - lastRequest >= interval)
        {
            didQueue.enqueue(row);
            didTable->item(row, DidPollMs)->setData(Qt::UserRole, now);
        }
    }
    sendNextDid();
}

void UDSWorkbenchWindow::requestDtcs()
{
    bool ok = false;
    const uint32_t mask = parseNumber(dtcStatusMaskEdit->text(), &ok);
    if (!ok || mask > 0xFF) { dtcResponse->setText(tr("Invalid status mask")); return; }
    sendServiceRequest(UDS_SERVICES::READ_DTC, QByteArray::fromRawData("\x02", 1) + char(mask), ContextDtcRead);
}

void UDSWorkbenchWindow::clearDtcs()
{
    if (QMessageBox::warning(this, tr("Clear diagnostic information"),
            tr("Clear all DTCs from the connected ECU?"), QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) return;
    sendServiceRequest(UDS_SERVICES::CLEAR_DIAG, QByteArray::fromHex("FFFFFF"), ContextDtcClear);
}

void UDSWorkbenchWindow::sendRoutineControl()
{
    bool ok = false;
    const uint32_t routineId = parseNumber(routineIdEdit->text(), &ok);
    if (!ok || routineId > 0xFFFF) { routineResponse->setText(tr("Invalid routine ID")); return; }
    QByteArray data;
    data.append(char(routineTypeCombo->currentData().toInt()));
    data.append(char(routineId >> 8));
    data.append(char(routineId));
    data.append(QByteArray::fromHex(routineDataEdit->text().toLatin1()));
    sendServiceRequest(UDS_SERVICES::ROUTINE_CTRL, data, ContextRoutine);
}

void UDSWorkbenchWindow::sendDetailedDtcRequest()
{
    QByteArray data;
    data.append(char(dtcDetailTypeCombo->currentData().toInt()));
    data.append(QByteArray::fromHex(dtcDetailDataEdit->text().toLatin1()));
    sendServiceRequest(UDS_SERVICES::READ_DTC, data, ContextDtcRead);
}

void UDSWorkbenchWindow::sendGuardedControl()
{
    const int service = controlServiceCombo->currentData().toInt();
    const QByteArray data = QByteArray::fromHex(controlDataEdit->text().toLatin1());
    const QString request = tr("Service 0x%1\nPayload: %2\n\nSend this state-changing request?")
        .arg(service, 2, 16, QLatin1Char('0'))
        .arg(QString::fromLatin1(data.toHex(' ').toUpper())).toUpper();
    if (data.isEmpty() || QMessageBox::warning(this, tr("Confirm diagnostic control"), request,
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    controlResponse->setText(tr("Waiting for response..."));
    sendServiceRequest(service, data, ContextControl);
}

void UDSWorkbenchWindow::requestSecuritySeed()
{
    int level = securityLevelSpin->value();
    if ((level & 1) == 0) --level;
    securityResponse->setText(tr("Requesting seed for level 0x%1...").arg(level, 2, 16, QLatin1Char('0')));
    sendServiceRequest(UDS_SERVICES::SECURITY_ACCESS, QByteArray(1, char(level)), ContextSecurity);
}

void UDSWorkbenchWindow::sendSecurityKey()
{
    int level = securityLevelSpin->value();
    if ((level & 1) == 0) --level;
    const QByteArray key = QByteArray::fromHex(securityKeyEdit->text().toLatin1());
    if (key.isEmpty()) { securityResponse->setText(tr("Enter key bytes first")); return; }
    if (QMessageBox::warning(this, tr("Confirm SecurityAccess key"),
            tr("Send an authorised key for security level 0x%1?").arg(level, 2, 16, QLatin1Char('0')),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    QByteArray data(1, char(level + 1));
    data.append(key);
    securityResponse->setText(tr("Sending key..."));
    sendServiceRequest(UDS_SERVICES::SECURITY_ACCESS, data, ContextSecurity);
}

void UDSWorkbenchWindow::sendServiceRequest(int service, const QByteArray &data, RequestContext context)
{
    if (!endpointConnected) { connectionStatus->setText(tr("Connect a session first")); return; }
    if (responseTimer.isActive() || activeDidRow >= 0 || !didQueue.isEmpty()) { log(tr("Another request is active")); return; }
    bool requestOk = false;
    const uint32_t requestId = parseNumber(requestIdEdit->text(), &requestOk);
    if (!requestOk) { connectionStatus->setText(tr("Invalid request ID")); return; }
    UDS_MESSAGE message;
    message.bus = busSpin->value();
    message.setFrameId(requestId);
    message.setExtendedFrameFormat(requestId > 0x7FF);
    message.service = service;
    message.subFuncLen = 0;
    message.setPayload(data);
    activeService = service;
    requestContext = context;
    if (!udsHandler->sendUDSFrame(message)) {
        connectionStatus->setText(tr("Transmit failed on bus %1").arg(message.bus));
        log(tr("UDS service 0x%1 transmit failed on bus %2")
            .arg(service, 2, 16, QLatin1Char('0')).arg(message.bus).toUpper());
        activeService = -1;
        requestContext = ContextNone;
        return;
    }
    log(tr("TX bus %1 ID 0x%2 service 0x%3 data %4")
        .arg(message.bus).arg(requestId, 0, 16)
        .arg(service, 2, 16, QLatin1Char('0'))
        .arg(QString::fromLatin1(data.toHex(' '))).toUpper());
    responseTimer.start();
    responseTimer.start();
}

QString UDSWorkbenchWindow::decodeDtcResponse(const QByteArray &payload) const
{
    if (payload.size() < 2) return tr("Short DTC response");
    QStringList lines;
    lines << tr("Availability mask: 0x%1").arg(quint8(payload.at(1)), 2, 16, QLatin1Char('0')).toUpper();
    for (int offset = 2; offset + 3 < payload.size(); offset += 4)
    {
        const uint32_t code = (quint8(payload[offset]) << 16) | (quint8(payload[offset + 1]) << 8) |
                              quint8(payload[offset + 2]);
        lines << tr("0x%1   status 0x%2").arg(code, 6, 16, QLatin1Char('0'))
                 .arg(quint8(payload[offset + 3]), 2, 16, QLatin1Char('0')).toUpper();
    }
    if (lines.size() == 1) lines << tr("No matching DTCs reported");
    return lines.join('\n');
}

void UDSWorkbenchWindow::toggleCsvLogging()
{
    if (csvLogFile)
    {
        csvLogFile->close();
        delete csvLogFile;
        csvLogFile = nullptr;
        csvLogButton->setText(tr("Start CSV log"));
        return;
    }
    const QString fileName = QFileDialog::getSaveFileName(this, tr("Log DID values"), QString(), tr("CSV files (*.csv)"));
    if (fileName.isEmpty()) return;
    csvLogFile = new QFile(fileName, this);
    if (!csvLogFile->open(QIODevice::WriteOnly | QIODevice::Text))
    {
        log(tr("Could not open CSV log: %1").arg(csvLogFile->errorString()));
        delete csvLogFile;
        csvLogFile = nullptr;
        return;
    }
    QTextStream stream(csvLogFile);
    stream << "timestamp,name,did,raw,decoded\n";
    csvLogButton->setText(tr("Stop CSV log"));
}

void UDSWorkbenchWindow::appendCsvRow(int row)
{
    if (!csvLogFile) return;
    auto quote = [](QString value) { value.replace('"', "\"\""); return '"' + value + '"'; };
    QTextStream stream(csvLogFile);
    stream << quote(QDateTime::currentDateTime().toString(Qt::ISODateWithMs)) << ','
           << quote(didTable->item(row, DidName)->text()) << ','
           << quote(didTable->item(row, DidIdentifier)->text()) << ','
           << quote(didTable->item(row, DidRaw)->text()) << ','
           << quote(didTable->item(row, DidDecoded)->text()) << '\n';
    csvLogFile->flush();
}

void UDSWorkbenchWindow::requestSelectedDid()
{
    if (!endpointConnected) { connectionStatus->setText(tr("Connect a session first")); return; }
    if (didTable->currentRow() < 0 || activeDidRow >= 0 || responseTimer.isActive()) return;
    didQueue.clear();
    didQueue.enqueue(didTable->currentRow());
    sendNextDid();
}

void UDSWorkbenchWindow::requestAllDids()
{
    if (!endpointConnected) { connectionStatus->setText(tr("Connect a session first")); return; }
    if (activeDidRow >= 0 || responseTimer.isActive()) return;
    didQueue.clear();
    for (int row = 0; row < didTable->rowCount(); ++row)
        if (didTable->item(row, DidEnabled) && didTable->item(row, DidEnabled)->checkState() == Qt::Checked)
            didQueue.enqueue(row);
    sendNextDid();
}

void UDSWorkbenchWindow::sendNextDid()
{
    if (didQueue.isEmpty()) { activeDidRow = -1; activeService = -1; return; }
    sendDidRow(didQueue.dequeue());
}

void UDSWorkbenchWindow::sendDidRow(int row)
{
    if (row < 0 || row >= didTable->rowCount()) return;
    bool didOk = false, requestOk = false;
    const uint32_t did = parseNumber(didTable->item(row, DidIdentifier)->text(), &didOk);
    const uint32_t requestId = parseNumber(requestIdEdit->text(), &requestOk);
    if (!didOk || did > 0xFFFF || !requestOk) { didTable->item(row, DidStatus)->setText(tr("Invalid DID/ID")); sendNextDid(); return; }
    UDS_MESSAGE message;
    message.bus = busSpin->value();
    message.setFrameId(requestId);
    message.setExtendedFrameFormat(requestId > 0x7FF);
    message.service = UDS_SERVICES::READ_BY_ID;
    message.subFuncLen = 2;
    message.subFunc = did;
    activeDidRow = row;
    activeService = message.service;
    requestContext = ContextNone;
    didTable->item(row, DidStatus)->setText(tr("Waiting"));
    if (!udsHandler->sendUDSFrame(message)) {
        endpointConnected = false;
        pollingTimer.stop();
        testerTimer.stop();
        if (pollingCheck->isChecked()) pollingCheck->setChecked(false);
        didTable->item(row, DidStatus)->setText(tr("Transmit failed"));
        log(tr("DID 0x%1 transmit failed on bus %2")
            .arg(did, 4, 16, QLatin1Char('0')).arg(message.bus).toUpper());
        activeDidRow = -1;
        activeService = -1;
        didQueue.clear();
        return;
    }
    log(tr("TX bus %1 ID 0x%2 ReadDataByIdentifier 0x%3")
        .arg(message.bus).arg(requestId, 0, 16)
        .arg(did, 4, 16, QLatin1Char('0')).toUpper());
    responseTimer.start();
}

void UDSWorkbenchWindow::sendManualRequest()
{
    if (!endpointConnected) { manualResponse->setText(tr("Connect a session first")); return; }
    if (activeDidRow >= 0 || !didQueue.isEmpty() || responseTimer.isActive()) return;
    bool serviceOk = false, requestOk = false;
    const uint32_t service = parseNumber(manualServiceEdit->text(), &serviceOk);
    const uint32_t requestId = parseNumber(requestIdEdit->text(), &requestOk);
    QByteArray bytes = QByteArray::fromHex(manualPayloadEdit->text().toLatin1());
    if (!serviceOk || service > 0xFF || !requestOk) { manualResponse->setText(tr("Invalid service or request ID")); return; }
    UDS_MESSAGE message;
    message.bus = busSpin->value();
    message.setFrameId(requestId);
    message.setExtendedFrameFormat(requestId > 0x7FF);
    message.service = service;
    message.subFuncLen = 0;
    message.setPayload(bytes);
    activeService = service;
    requestContext = ContextManual;
    udsHandler->sendUDSFrame(message);
    responseTimer.start();
    manualResponse->setText(tr("Waiting for response..."));
}

void UDSWorkbenchWindow::startDidScan()
{
    if (!endpointConnected) { connectionStatus->setText(tr("Connect a session first")); return; }
    if (responseTimer.isActive() || activeDidRow >= 0 || !didQueue.isEmpty()) { log(tr("Another request is active")); return; }
    bool startOk = false, endOk = false;
    const uint32_t start = parseNumber(scanStartEdit->text(), &startOk);
    const uint32_t end = parseNumber(scanEndEdit->text(), &endOk);
    if (!startOk || !endOk || start > 0xFFFF || end > 0xFFFF || start > end)
    {
        log(tr("Invalid DID scan range"));
        return;
    }
    const uint32_t count = end - start + 1;
    if (count > 4096 && QMessageBox::warning(this, tr("Large DID scan"),
            tr("Scan %1 DIDs? This can take a long time and creates active diagnostic traffic.").arg(count),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;

    didScanActive = true;
    scanCurrentDid = int(start);
    scanEndDid = int(end);
    scanResults->clear();
    scanProgress->setRange(scanCurrentDid, scanEndDid + 1);
    scanProgress->setValue(scanCurrentDid);
    scanStartButton->setEnabled(false);
    scanResumeButton->setEnabled(false);
    scanStopButton->setEnabled(true);
    pollingTimer.stop();
    responseTimer.setInterval(scanTimeoutSpin->value());
    log(tr("Scanning DIDs 0x%1 through 0x%2").arg(start, 4, 16, QLatin1Char('0'))
        .arg(end, 4, 16, QLatin1Char('0')).toUpper());
    sendNextDidScan();
}

void UDSWorkbenchWindow::sendNextDidScan()
{
    if (!didScanActive) return;
    if (scanCurrentDid > scanEndDid)
    {
        finishDidScan(tr("DID scan complete"));
        return;
    }
    bool requestOk = false;
    const uint32_t requestId = parseNumber(requestIdEdit->text(), &requestOk);
    if (!requestOk) { finishDidScan(tr("Invalid request ID")); return; }
    UDS_MESSAGE message;
    message.bus = busSpin->value();
    message.setFrameId(requestId);
    message.setExtendedFrameFormat(requestId > 0x7FF);
    message.service = UDS_SERVICES::READ_BY_ID;
    message.subFuncLen = 2;
    message.subFunc = scanCurrentDid;
    activeService = message.service;
    requestContext = ContextDidScan;
    udsHandler->sendUDSFrame(message);
    responseTimer.start();
}

void UDSWorkbenchWindow::resumeDidScan()
{
    if (!endpointConnected || didScanActive || scanCurrentDid > scanEndDid) return;
    if (responseTimer.isActive() || activeDidRow >= 0 || !didQueue.isEmpty()) { log(tr("Another request is active")); return; }
    didScanActive = true;
    scanStartButton->setEnabled(false);
    scanResumeButton->setEnabled(false);
    scanStopButton->setEnabled(true);
    pollingTimer.stop();
    responseTimer.setInterval(scanTimeoutSpin->value());
    log(tr("Resuming DID scan at 0x%1").arg(scanCurrentDid, 4, 16, QLatin1Char('0')).toUpper());
    sendNextDidScan();
}

void UDSWorkbenchWindow::stopDidScan()
{
    if (didScanActive) finishDidScan(tr("DID scan stopped"));
}

void UDSWorkbenchWindow::finishDidScan(const QString &status)
{
    responseTimer.stop();
    didScanActive = false;
    activeService = -1;
    requestContext = ContextNone;
    responseTimer.setInterval(normalResponseTimeout);
    scanStartButton->setEnabled(true);
    scanResumeButton->setEnabled(scanCurrentDid <= scanEndDid);
    scanStopButton->setEnabled(false);
    if (pollingCheck->isChecked()) pollingTimer.start(100);
    log(tr("%1; %2 DIDs found").arg(status).arg(scanResults->count()));
}

void UDSWorkbenchWindow::saveDidScan()
{
    QString fileName = QFileDialog::getSaveFileName(this, tr("Save DID scan"), QString(),
                                                     tr("DID scan results (*.json)"));
    if (fileName.isEmpty()) return;
    if (!fileName.endsWith(".json", Qt::CaseInsensitive)) fileName += ".json";
    QJsonObject root;
    root["version"] = 1;
    root["start"] = scanProgress->minimum();
    root["next"] = scanCurrentDid;
    root["end"] = scanEndDid;
    QJsonArray results;
    for (int i = 0; i < scanResults->count(); ++i)
    {
        QJsonObject result;
        result["did"] = scanResults->item(i)->data(Qt::UserRole).toInt();
        result["text"] = scanResults->item(i)->text();
        results.append(result);
    }
    root["results"] = results;
    QSaveFile file(fileName);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(root).toJson()) < 0 || !file.commit())
        log(tr("Could not save DID scan: %1").arg(file.errorString()));
    else
        log(tr("Saved %1 DID scan results").arg(results.size()));
}

void UDSWorkbenchWindow::loadDidScan()
{
    const QString fileName = QFileDialog::getOpenFileName(this, tr("Load DID scan"), QString(),
                                                           tr("DID scan results (*.json)"));
    if (fileName.isEmpty()) return;
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) { log(tr("Could not open DID scan")); return; }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) { log(tr("Invalid DID scan file")); return; }
    const QJsonObject root = document.object();
    scanCurrentDid = root["next"].toInt();
    scanEndDid = root["end"].toInt();
    scanProgress->setRange(root["start"].toInt(), scanEndDid + 1);
    scanProgress->setValue(scanCurrentDid);
    scanResults->clear();
    for (const QJsonValue &value : root["results"].toArray())
    {
        const QJsonObject result = value.toObject();
        QListWidgetItem *item = new QListWidgetItem(result["text"].toString(), scanResults);
        item->setData(Qt::UserRole, result["did"].toInt());
    }
    scanResumeButton->setEnabled(scanCurrentDid <= scanEndDid);
    log(tr("Loaded %1 DID scan results").arg(scanResults->count()));
}

void UDSWorkbenchWindow::addSelectedScannedDids()
{
    const QList<QListWidgetItem *> selected = scanResults->selectedItems();
    for (QListWidgetItem *result : selected)
    {
        const int did = result->data(Qt::UserRole).toInt();
        bool alreadyPresent = false;
        for (int row = 0; row < didTable->rowCount(); ++row)
        {
            bool ok = false;
            if (int(parseNumber(didTable->item(row, DidIdentifier)->text(), &ok)) == did && ok)
            {
                alreadyPresent = true;
                break;
            }
        }
        if (alreadyPresent) continue;
        addDid();
        const int row = didTable->rowCount() - 1;
        didTable->item(row, DidName)->setText(did == 0xF190 ? tr("Vehicle identification number")
                                                            : tr("DID 0x%1").arg(did, 4, 16, QLatin1Char('0')).toUpper());
        didTable->item(row, DidIdentifier)->setText(QStringLiteral("0x%1").arg(did, 4, 16, QLatin1Char('0')).toUpper());
        didTable->item(row, DidFormat)->setText(did == 0xF190 ? QStringLiteral("ascii17") : QStringLiteral("u8"));
    }
    log(tr("Added selected discovered DIDs to the request list"));
}

QString UDSWorkbenchWindow::serviceName(int service) const
{
    switch (service)
    {
    case 0x10: return tr("DiagnosticSessionControl");
    case 0x11: return tr("ECUReset");
    case 0x14: return tr("ClearDiagnosticInformation");
    case 0x19: return tr("ReadDTCInformation");
    case 0x22: return tr("ReadDataByIdentifier");
    case 0x23: return tr("ReadMemoryByAddress");
    case 0x24: return tr("ReadScalingDataByIdentifier");
    case 0x27: return tr("SecurityAccess");
    case 0x28: return tr("CommunicationControl");
    case 0x2A: return tr("ReadDataByPeriodicIdentifier");
    case 0x2C: return tr("DynamicallyDefineDataIdentifier");
    case 0x2E: return tr("WriteDataByIdentifier");
    case 0x2F: return tr("InputOutputControlByIdentifier");
    case 0x31: return tr("RoutineControl");
    case 0x34: return tr("RequestDownload");
    case 0x35: return tr("RequestUpload");
    case 0x36: return tr("TransferData");
    case 0x37: return tr("RequestTransferExit");
    case 0x38: return tr("RequestFileTransfer");
    case 0x3D: return tr("WriteMemoryByAddress");
    case 0x3E: return tr("TesterPresent");
    case 0x83: return tr("AccessTimingParameter");
    case 0x84: return tr("SecuredDataTransmission");
    case 0x85: return tr("ControlDTCSetting");
    case 0x86: return tr("ResponseOnEvent");
    case 0x87: return tr("LinkControl");
    default: return tr("Unknown");
    }
}

void UDSWorkbenchWindow::startServiceScan()
{
    if (!endpointConnected) { connectionStatus->setText(tr("Connect a session first")); return; }
    if (responseTimer.isActive() || activeDidRow >= 0 || !didQueue.isEmpty() || didScanActive)
    {
        log(tr("Another request is active"));
        return;
    }
    if (QMessageBox::warning(this, tr("Confirm service discovery"),
            tr("Service discovery sends incomplete requests for standard UDS services. "
               "Although these probes do not contain valid control or write parameters, they create active "
               "diagnostic traffic and some ECUs may behave unexpectedly. Continue only on an authorised system."),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;

    static const int services[] = {
        0x10, 0x11, 0x14, 0x19, 0x22, 0x23, 0x24, 0x27, 0x28, 0x2A, 0x2C,
        0x2E, 0x2F, 0x31, 0x34, 0x35, 0x36, 0x37, 0x38, 0x3D, 0x3E,
        0x83, 0x84, 0x85, 0x86, 0x87
    };
    serviceScanQueue.clear();
    for (int service : services) serviceScanQueue.enqueue(service);
    serviceScanResults->clear();
    serviceScanActive = true;
    serviceScanStartButton->setEnabled(false);
    serviceScanStopButton->setEnabled(true);
    pollingTimer.stop();
    responseTimer.setInterval(scanTimeoutSpin->value());
    log(tr("Scanning %1 standard UDS services").arg(serviceScanQueue.size()));
    sendNextServiceScan();
}

void UDSWorkbenchWindow::sendNextServiceScan()
{
    if (!serviceScanActive) return;
    if (serviceScanQueue.isEmpty())
    {
        finishServiceScan(tr("Service scan complete"));
        return;
    }
    bool requestOk = false;
    const uint32_t requestId = parseNumber(requestIdEdit->text(), &requestOk);
    if (!requestOk) { finishServiceScan(tr("Invalid request ID")); return; }
    UDS_MESSAGE message;
    message.bus = busSpin->value();
    message.setFrameId(requestId);
    message.setExtendedFrameFormat(requestId > 0x7FF);
    message.service = serviceScanQueue.dequeue();
    message.subFuncLen = 0;
    activeService = message.service;
    requestContext = ContextServiceScan;
    udsHandler->sendUDSFrame(message);
    responseTimer.start();
}

void UDSWorkbenchWindow::stopServiceScan()
{
    if (serviceScanActive) finishServiceScan(tr("Service scan stopped"));
}

void UDSWorkbenchWindow::finishServiceScan(const QString &status)
{
    responseTimer.stop();
    serviceScanActive = false;
    serviceScanQueue.clear();
    activeService = -1;
    requestContext = ContextNone;
    responseTimer.setInterval(normalResponseTimeout);
    serviceScanStartButton->setEnabled(true);
    serviceScanStopButton->setEnabled(false);
    if (pollingCheck->isChecked()) pollingTimer.start(100);
    log(tr("%1; %2 recognized services").arg(status).arg(serviceScanResults->count()));
}

void UDSWorkbenchWindow::gotUDSReply(UDS_MESSAGE message)
{
    bool responseOk = false;
    const uint32_t responseId = parseNumber(responseIdEdit->text(), &responseOk);
    if (!responseOk || message.bus != busSpin->value() || message.frameId() != responseId ||
        (message.service != activeService && message.service != activeService + 0x40))
        return;
    if (message.isErrorReply)
    {
        const int nrc = message.subFunc;
        if (nrc == 0x78) { responseTimer.start(); log(tr("Response pending")); return; }
        const QString status = tr("NRC 0x%1: %2").arg(nrc, 2, 16, QLatin1Char('0'))
            .arg(udsHandler->getNegativeResponseShort(nrc));
        if (requestContext == ContextSession)
        {
            endpointConnected = false;
            connectButton->setText(tr("Connect session"));
            connectionStatus->setText(status);
        }
        if (requestContext == ContextDidScan)
        {
            responseTimer.stop();
            ++scanCurrentDid;
            scanProgress->setValue(scanCurrentDid);
            sendNextDidScan();
        }
        else if (requestContext == ContextServiceScan)
        {
            responseTimer.stop();
            if (nrc != 0x11)
                serviceScanResults->addItem(tr("0x%1  %2  NRC 0x%3: %4")
                    .arg(activeService, 2, 16, QLatin1Char('0')).arg(serviceName(activeService))
                    .arg(nrc, 2, 16, QLatin1Char('0')).arg(udsHandler->getNegativeResponseShort(nrc))
                    .toUpper());
            sendNextServiceScan();
        }
        else if (activeDidRow >= 0) finishCurrentRequest(status);
        else
        {
            responseTimer.stop();
            if (requestContext == ContextDtcRead || requestContext == ContextDtcClear) dtcResponse->setText(status);
            else if (requestContext == ContextRoutine) routineResponse->setText(status);
            else if (requestContext == ContextControl) controlResponse->setText(status);
            else if (requestContext == ContextSecurity) securityResponse->setText(status);
            else manualResponse->setText(status);
            activeService = -1;
            requestContext = ContextNone;
        }
        return;
    }
    responseTimer.stop();
    if (requestContext == ContextServiceScan)
    {
        serviceScanResults->addItem(tr("0x%1  %2  positive response")
            .arg(activeService, 2, 16, QLatin1Char('0')).arg(serviceName(activeService)).toUpper());
        sendNextServiceScan();
    }
    else if (requestContext == ContextDidScan && message.service == UDS_SERVICES::READ_BY_ID + 0x40)
    {
        const QByteArray payload = message.payload();
        if (payload.size() >= 2)
        {
            const int responseDid = (quint8(payload[0]) << 8) | quint8(payload[1]);
            if (responseDid == scanCurrentDid)
            {
                const QByteArray value = payload.mid(2);
                QListWidgetItem *item = new QListWidgetItem(
                    tr("0x%1  %2 bytes  %3").arg(responseDid, 4, 16, QLatin1Char('0'))
                        .arg(value.size()).arg(QString::fromLatin1(value.toHex(' ').toUpper())),
                    scanResults);
                item->setData(Qt::UserRole, responseDid);
            }
        }
        ++scanCurrentDid;
        scanProgress->setValue(scanCurrentDid);
        sendNextDidScan();
    }
    else if (activeDidRow >= 0 && message.service == UDS_SERVICES::READ_BY_ID + 0x40)
    {
        QByteArray payload = message.payload();
        if (payload.size() < 2) { finishCurrentRequest(tr("Short response")); return; }
        const int responseDid = (static_cast<quint8>(payload[0]) << 8) | static_cast<quint8>(payload[1]);
        bool expectedOk = false;
        const int expectedDid = parseNumber(didTable->item(activeDidRow, DidIdentifier)->text(), &expectedOk);
        if (!expectedOk || responseDid != expectedDid) return;
        payload.remove(0, 2);
        didTable->item(activeDidRow, DidRaw)->setText(QString::fromLatin1(payload.toHex(' ').toUpper()));
        PayloadFormatter formatter;
        QString error;
        const QString format = didTable->item(activeDidRow, DidFormat)->text();
        const bool repeatSimpleType = !format.simplified().contains(' ') && !format.contains(':');
        if (formatter.compile(format, &error, repeatSimpleType))
        {
            didTable->item(activeDidRow, DidDecoded)->setText(formatter.format(payload));
            if (diagnosticGraph)
            {
                const QString prefix = didTable->item(activeDidRow, DidName)->text();
                for (const PayloadFormatter::FormattedField &field : formatter.formatFields(payload))
                {
                    bool numeric = false;
                    const double value = field.value.toDouble(&numeric);
                    if (numeric) diagnosticGraph->addSample(prefix + QStringLiteral("/") + field.name, value);
                }
            }
        }
        else
            didTable->item(activeDidRow, DidDecoded)->setText(error);
        didTable->item(activeDidRow, DidUpdated)->setText(QDateTime::currentDateTime().toString("HH:mm:ss.zzz"));
        appendCsvRow(activeDidRow);
        finishCurrentRequest(tr("Positive"));
    }
    else if (requestContext == ContextDtcRead)
    {
        dtcResponse->setText(decodeDtcResponse(message.payload()));
        activeService = -1;
        requestContext = ContextNone;
    }
    else if (requestContext == ContextDtcClear)
    {
        dtcResponse->setText(tr("Diagnostic information cleared"));
        activeService = -1;
        requestContext = ContextNone;
    }
    else if (requestContext == ContextRoutine)
    {
        routineResponse->setText(tr("Positive response\n%1")
            .arg(QString::fromLatin1(message.payload().toHex(' ').toUpper())));
        activeService = -1;
        requestContext = ContextNone;
    }
    else if (requestContext == ContextControl)
    {
        controlResponse->setText(tr("Positive response 0x%1\n%2")
            .arg(message.service, 2, 16, QLatin1Char('0'))
            .arg(QString::fromLatin1(message.payload().toHex(' ').toUpper())));
        activeService = -1;
        requestContext = ContextNone;
    }
    else if (requestContext == ContextSecurity)
    {
        securityResponse->setText(tr("Positive response\n%1")
            .arg(QString::fromLatin1(message.payload().toHex(' ').toUpper())));
        activeService = -1;
        requestContext = ContextNone;
    }
    else
    {
        if (requestContext != ContextSession)
            manualResponse->setText(tr("Positive response 0x%1\n%2")
                .arg(message.service, 2, 16, QLatin1Char('0'))
                .arg(QString::fromLatin1(message.payload().toHex(' ').toUpper())));
        activeService = -1;
        requestContext = ContextNone;
    }
    if (message.service == UDS_SERVICES::DIAG_CONTROL + 0x40)
    {
        endpointConnected = true;
        connectButton->setText(tr("Disconnect"));
        connectionStatus->setText(tr("Session active"));
        if (testerPresentCheck->isChecked()) testerTimer.start();
        if (pollingCheck->isChecked()) pollingTimer.start(100);
    }
}

void UDSWorkbenchWindow::showDiagnosticGraph()
{
    if (!diagnosticGraph) diagnosticGraph = new DiagnosticGraphWindow(this);
    diagnosticGraph->show();
    diagnosticGraph->raise();
}

void UDSWorkbenchWindow::finishCurrentRequest(const QString &status)
{
    responseTimer.stop();
    if (activeDidRow >= 0 && didTable->item(activeDidRow, DidStatus))
        didTable->item(activeDidRow, DidStatus)->setText(status);
    activeDidRow = -1;
    sendNextDid();
}

void UDSWorkbenchWindow::requestTimedOut()
{
    if (requestContext == ContextDidScan)
    {
        ++scanCurrentDid;
        scanProgress->setValue(scanCurrentDid);
        sendNextDidScan();
    }
    else if (requestContext == ContextServiceScan)
    {
        sendNextServiceScan();
    }
    else if (activeDidRow >= 0) finishCurrentRequest(tr("Timeout"));
    else
    {
        if (requestContext == ContextDtcRead || requestContext == ContextDtcClear) dtcResponse->setText(tr("Request timed out"));
        else if (requestContext == ContextRoutine) routineResponse->setText(tr("Request timed out"));
        else if (requestContext == ContextControl) controlResponse->setText(tr("Request timed out"));
        else if (requestContext == ContextSecurity) securityResponse->setText(tr("Request timed out"));
        else if (requestContext != ContextSession) manualResponse->setText(tr("Request timed out"));
        connectionStatus->setText(tr("No response"));
        if (activeService == UDS_SERVICES::DIAG_CONTROL)
        {
            endpointConnected = false;
            connectButton->setText(tr("Connect session"));
            testerTimer.stop();
        }
        activeService = -1;
        requestContext = ContextNone;
    }
}

void UDSWorkbenchWindow::sendTesterPresent()
{
    bool requestOk = false;
    const uint32_t requestId = parseNumber(requestIdEdit->text(), &requestOk);
    if (!endpointConnected || !requestOk || responseTimer.isActive()) return;
    UDS_MESSAGE message;
    message.bus = busSpin->value();
    message.setFrameId(requestId);
    message.setExtendedFrameFormat(requestId > 0x7FF);
    message.service = UDS_SERVICES::TESTER_PRESENT;
    message.subFuncLen = 1;
    message.subFunc = 0x80;
    if (!udsHandler->sendUDSFrame(message)) {
        connectionStatus->setText(tr("Tester Present transmit failed"));
        log(tr("Tester Present was rejected by the CAN interface"));
    }
}

void UDSWorkbenchWindow::log(const QString &text)
{
    eventLog->append(QStringLiteral("[%1] %2").arg(QDateTime::currentDateTime().toString("HH:mm:ss.zzz"), text));
}

void UDSWorkbenchWindow::loadSettings()
{
    QSettings settings;
    busSpin->setValue(settings.value("UDSWorkbench/Bus", 0).toInt());
    requestIdEdit->setText(settings.value("UDSWorkbench/RequestId", "0x7E0").toString());
    responseIdEdit->setText(settings.value("UDSWorkbench/ResponseId", "0x7E8").toString());
    sessionCombo->setCurrentIndex(settings.value("UDSWorkbench/Session", 2).toInt());
    pollIntervalSpin->setValue(settings.value("UDSWorkbench/PollInterval", 1000).toInt());
    pollingCheck->setChecked(settings.value("UDSWorkbench/PollEnabled", false).toBool());
    const QJsonArray rows = QJsonDocument::fromJson(settings.value("UDSWorkbench/Dids").toByteArray()).array();
    loadDidRows(rows);
    if (didTable->rowCount() == 0) addDid();
}

void UDSWorkbenchWindow::saveSettings() const
{
    QSettings settings;
    settings.setValue("UDSWorkbench/Bus", busSpin->value());
    settings.setValue("UDSWorkbench/RequestId", requestIdEdit->text());
    settings.setValue("UDSWorkbench/ResponseId", responseIdEdit->text());
    settings.setValue("UDSWorkbench/Session", sessionCombo->currentIndex());
    settings.setValue("UDSWorkbench/PollInterval", pollIntervalSpin->value());
    settings.setValue("UDSWorkbench/PollEnabled", pollingCheck->isChecked());
    settings.setValue("UDSWorkbench/Dids", QJsonDocument(didRowsToJson()).toJson(QJsonDocument::Compact));
}

QJsonArray UDSWorkbenchWindow::didRowsToJson() const
{
    QJsonArray rows;
    for (int row = 0; row < didTable->rowCount(); ++row)
    {
        QJsonObject object;
        object["enabled"] = didTable->item(row, DidEnabled)->checkState() == Qt::Checked;
        object["name"] = didTable->item(row, DidName)->text();
        object["did"] = didTable->item(row, DidIdentifier)->text();
        object["format"] = didTable->item(row, DidFormat)->text();
        object["pollMs"] = didTable->item(row, DidPollMs)->text();
        rows.append(object);
    }
    return rows;
}

void UDSWorkbenchWindow::loadDidRows(const QJsonArray &rows)
{
    didTable->setRowCount(0);
    for (const QJsonValue &value : rows)
    {
        if (!value.isObject()) continue;
        addDid();
        const int row = didTable->rowCount() - 1;
        const QJsonObject object = value.toObject();
        didTable->item(row, DidEnabled)->setCheckState(object["enabled"].toBool(true) ? Qt::Checked : Qt::Unchecked);
        didTable->item(row, DidName)->setText(object["name"].toString());
        didTable->item(row, DidIdentifier)->setText(object["did"].toString());
        didTable->item(row, DidFormat)->setText(object["format"].toString("u8"));
        didTable->item(row, DidPollMs)->setText(object["pollMs"].toString("0"));
    }
}
