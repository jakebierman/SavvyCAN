#include "udsworkbenchwindow.h"

#include "connections/canconmanager.h"
#include "payloadformatter.h"

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
#include <QMessageBox>
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
    QPushButton *connectButton = new QPushButton(tr("Connect session"), endpoint);
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
    connect(connectButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::connectEndpoint);
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
    connect(pollingCheck, &QCheckBox::toggled, this, [this](bool enabled) {
        if (enabled) pollingTimer.start(100); else pollingTimer.stop();
    });
    tabs->addTab(didPage, tr("DID requests"));

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
    dtcResponse = new QTextEdit(dtcPage);
    dtcResponse->setReadOnly(true);
    dtcLayout->addWidget(dtcResponse);
    connect(readDtcsButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::requestDtcs);
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
    udsHandler->sendUDSFrame(message);
    responseTimer.start();
    endpointConnected = true;
    connectionStatus->setText(tr("Session requested"));
    if (testerPresentCheck->isChecked()) testerTimer.start();
    log(tr("Requested diagnostic session 0x%1").arg(message.subFunc, 2, 16, QLatin1Char('0')));
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
    message.payload() = data;
    activeService = service;
    requestContext = context;
    udsHandler->sendUDSFrame(message);
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
    udsHandler->sendUDSFrame(message);
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
    message.payload() = bytes;
    activeService = service;
    requestContext = ContextManual;
    udsHandler->sendUDSFrame(message);
    responseTimer.start();
    manualResponse->setText(tr("Waiting for response..."));
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
        if (activeDidRow >= 0) finishCurrentRequest(status);
        else
        {
            responseTimer.stop();
            if (requestContext == ContextDtcRead || requestContext == ContextDtcClear) dtcResponse->setText(status);
            else if (requestContext == ContextRoutine) routineResponse->setText(status);
            else manualResponse->setText(status);
            activeService = -1;
            requestContext = ContextNone;
        }
        return;
    }
    responseTimer.stop();
    if (activeDidRow >= 0 && message.service == UDS_SERVICES::READ_BY_ID + 0x40)
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
            didTable->item(activeDidRow, DidDecoded)->setText(formatter.format(payload));
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
        connectionStatus->setText(tr("Session active"));
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
    if (activeDidRow >= 0) finishCurrentRequest(tr("Timeout"));
    else
    {
        if (requestContext == ContextDtcRead || requestContext == ContextDtcClear) dtcResponse->setText(tr("Request timed out"));
        else if (requestContext == ContextRoutine) routineResponse->setText(tr("Request timed out"));
        else if (requestContext != ContextSession) manualResponse->setText(tr("Request timed out"));
        connectionStatus->setText(tr("No response"));
        if (activeService == UDS_SERVICES::DIAG_CONTROL)
        {
            endpointConnected = false;
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
    udsHandler->sendUDSFrame(message);
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
