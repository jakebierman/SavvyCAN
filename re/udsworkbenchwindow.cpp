#include "udsworkbenchwindow.h"

#include "connections/canconmanager.h"
#include "canframemodel.h"
#include "mainwindow.h"
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
#include <QInputDialog>
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
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

UDSWorkbenchWindow::UDSWorkbenchWindow(QWidget *parent) : QDialog(parent)
{
    udsHandler = new UDS_HANDLER;
    buildUi();
    loadSettings();
    udsHandler->setFlowCtrl(true);
    responseTimer.setSingleShot(true);
    responseTimer.setInterval(1500);
    testerTimer.setInterval(2000);
    pollingTimer.setSingleShot(true);
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
    addressPresetCombo = new QComboBox(endpoint);
    addressPresetCombo->addItem(tr("11-bit physical"), QStringLiteral("11physical"));
    addressPresetCombo->addItem(tr("11-bit functional"), QStringLiteral("11functional"));
    addressPresetCombo->addItem(tr("29-bit normal fixed"), QStringLiteral("29fixed"));
    addressPresetCombo->addItem(tr("Custom"), QStringLiteral("custom"));
    QPushButton *applyAddressDefaultsButton = new QPushButton(tr("Apply defaults"), endpoint);
    requestIdEdit = new QLineEdit(QStringLiteral("0x7E0"), endpoint);
    responseAddressModeCombo = new QComboBox(endpoint);
    responseAddressModeCombo->addItem(tr("Response ID"), QStringLiteral("id"));
    responseAddressModeCombo->addItem(tr("Request + 0x8"), QStringLiteral("offset8"));
    responseAddressModeCombo->addItem(tr("Request + 0x80"), QStringLiteral("offset80"));
    responseAddressModeCombo->addItem(tr("Custom offset"), QStringLiteral("custom"));
    responseAddressModeCombo->setCurrentIndex(1);
    responseOffsetEdit = new QLineEdit(QStringLiteral("0x8"), endpoint);
    responseOffsetEdit->setPlaceholderText(tr("Offset"));
    responseOffsetEdit->setMaximumWidth(90);
    responseOffsetEdit->setVisible(false);
    responseIdEdit = new QLineEdit(QStringLiteral("0x7E8"), endpoint);
    responseIdEdit->setReadOnly(true);
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
    endpointLayout->addWidget(addressPresetCombo);
    endpointLayout->addWidget(applyAddressDefaultsButton);
    endpointLayout->addWidget(new QLabel(tr("Request ID"), endpoint));
    endpointLayout->addWidget(requestIdEdit);
    endpointLayout->addWidget(new QLabel(tr("Response"), endpoint));
    endpointLayout->addWidget(responseAddressModeCombo);
    endpointLayout->addWidget(responseOffsetEdit);
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
    connect(responseAddressModeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this]() { updateResponseIdFromMode(); });
    connect(requestIdEdit, &QLineEdit::editingFinished,
            this, &UDSWorkbenchWindow::updateResponseIdFromMode);
    connect(responseOffsetEdit, &QLineEdit::editingFinished,
            this, &UDSWorkbenchWindow::updateResponseIdFromMode);
    connect(applyAddressDefaultsButton, &QPushButton::clicked,
            this, &UDSWorkbenchWindow::applyAddressPreset);

    QTabWidget *tabs = new QTabWidget(this);
    QWidget *setupPage = new QWidget(tabs);
    QFormLayout *setupForm = new QFormLayout(setupPage);
    safetyModeCombo = new QComboBox(setupPage);
    safetyModeCombo->addItem(tr("Passive - no transmit"), QStringLiteral("passive"));
    safetyModeCombo->addItem(tr("Read-only active"), QStringLiteral("read"));
    safetyModeCombo->addItem(tr("Full diagnostics"), QStringLiteral("full"));
    safetyModeCombo->setCurrentIndex(1);
    p2TimeoutSpin = new QSpinBox(setupPage);
    p2TimeoutSpin->setRange(20, 60000);
    p2TimeoutSpin->setValue(1500);
    p2TimeoutSpin->setSuffix(tr(" ms"));
    p2StarTimeoutSpin = new QSpinBox(setupPage);
    p2StarTimeoutSpin->setRange(100, 120000);
    p2StarTimeoutSpin->setValue(5000);
    p2StarTimeoutSpin->setSuffix(tr(" ms"));
    flowBlockSizeSpin = new QSpinBox(setupPage);
    flowBlockSizeSpin->setRange(0, 255);
    flowStMinSpin = new QSpinBox(setupPage);
    flowStMinSpin->setRange(0, 255);
    flowStMinSpin->setValue(3);
    flowStMinSpin->setSuffix(tr(" ms"));
    QPushButton *learnResponseButton = new QPushButton(tr("Learn response for current request"), setupPage);
    setupForm->addRow(tr("Safety mode"), safetyModeCombo);
    setupForm->addRow(tr("P2 timeout"), p2TimeoutSpin);
    setupForm->addRow(tr("P2* pending timeout"), p2StarTimeoutSpin);
    setupForm->addRow(tr("ISO-TP block size"), flowBlockSizeSpin);
    setupForm->addRow(tr("ISO-TP STmin"), flowStMinSpin);
    setupForm->addRow(learnResponseButton);
    connect(learnResponseButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::learnResponseAddress);
    connect(p2TimeoutSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        normalResponseTimeout = value;
        responseTimer.setInterval(value);
    });
    connect(flowBlockSizeSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
        udsHandler->setFlowControlParameters(flowBlockSizeSpin->value(), flowStMinSpin->value());
    });
    connect(flowStMinSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
        udsHandler->setFlowControlParameters(flowBlockSizeSpin->value(), flowStMinSpin->value());
    });
    tabs->addTab(setupPage, tr("Setup"));
    QWidget *ecuDiscoveryPage = new QWidget(tabs);
    QVBoxLayout *ecuDiscoveryLayout = new QVBoxLayout(ecuDiscoveryPage);
    QHBoxLayout *ecuScanControls = new QHBoxLayout;
    ecuRequestSpecEdit = new QLineEdit(QStringLiteral("0x7E0-0x7E7"), ecuDiscoveryPage);
    ecuResponseSpecEdit = new QLineEdit(QStringLiteral("0x7E8-0x7EF"), ecuDiscoveryPage);
    ecuAnyResponseCheck = new QCheckBox(tr("Any response ID"), ecuDiscoveryPage);
    ecuScanTimeoutSpin = new QSpinBox(ecuDiscoveryPage);
    ecuScanTimeoutSpin->setRange(20, 5000);
    ecuScanTimeoutSpin->setValue(150);
    ecuScanTimeoutSpin->setSuffix(tr(" ms"));
    ecuScanDelaySpin = new QSpinBox(ecuDiscoveryPage);
    ecuScanDelaySpin->setRange(0, 10000);
    ecuScanDelaySpin->setValue(25);
    ecuScanDelaySpin->setSuffix(tr(" ms gap"));
    ecuScanLimitSpin = new QSpinBox(ecuDiscoveryPage);
    ecuScanLimitSpin->setRange(1, 4096);
    ecuScanLimitSpin->setValue(256);
    ecuScanLimitSpin->setPrefix(tr("Limit "));
    ecuScanStartButton = new QPushButton(tr("Scan ECUs"), ecuDiscoveryPage);
    ecuScanStopButton = new QPushButton(tr("Stop"), ecuDiscoveryPage);
    ecuScanStopButton->setEnabled(false);
    ecuScanControls->addWidget(new QLabel(tr("Request IDs"), ecuDiscoveryPage));
    ecuScanControls->addWidget(ecuRequestSpecEdit);
    ecuScanControls->addWidget(new QLabel(tr("Response IDs"), ecuDiscoveryPage));
    ecuScanControls->addWidget(ecuResponseSpecEdit);
    ecuScanControls->addWidget(ecuAnyResponseCheck);
    ecuScanControls->addWidget(new QLabel(tr("Timeout"), ecuDiscoveryPage));
    ecuScanControls->addWidget(ecuScanTimeoutSpin);
    ecuScanControls->addWidget(ecuScanDelaySpin);
    ecuScanControls->addWidget(ecuScanLimitSpin);
    ecuScanControls->addWidget(ecuScanStartButton);
    ecuScanControls->addWidget(ecuScanStopButton);
    ecuDiscoveryLayout->addLayout(ecuScanControls);
    ecuScanResults = new QListWidget(ecuDiscoveryPage);
    ecuScanResults->setSelectionMode(QAbstractItemView::SingleSelection);
    ecuDiscoveryLayout->addWidget(ecuScanResults, 1);
    QHBoxLayout *ecuResultControls = new QHBoxLayout;
    QPushButton *loadEcuScanButton = new QPushButton(tr("Load results"), ecuDiscoveryPage);
    QPushButton *saveEcuScanButton = new QPushButton(tr("Save results"), ecuDiscoveryPage);
    QPushButton *useEcuButton = new QPushButton(tr("Use selected ECU"), ecuDiscoveryPage);
    QPushButton *passiveEcuButton = new QPushButton(tr("Infer from capture"), ecuDiscoveryPage);
    QPushButton *verifyEcuButton = new QPushButton(tr("Verify physically"), ecuDiscoveryPage);
    QPushButton *editEcuButton = new QPushButton(tr("Name / notes"), ecuDiscoveryPage);
    QPushButton *clearEcuScanButton = new QPushButton(tr("Clear results"), ecuDiscoveryPage);
    ecuResultControls->addWidget(passiveEcuButton);
    ecuResultControls->addWidget(loadEcuScanButton);
    ecuResultControls->addWidget(saveEcuScanButton);
    ecuResultControls->addWidget(clearEcuScanButton);
    ecuResultControls->addStretch();
    ecuResultControls->addWidget(useEcuButton);
    ecuResultControls->addWidget(verifyEcuButton);
    ecuResultControls->addWidget(editEcuButton);
    ecuDiscoveryLayout->addLayout(ecuResultControls);
    connect(ecuScanStartButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::startEcuScan);
    connect(ecuScanStopButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::stopEcuScan);
    connect(loadEcuScanButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::loadEcuScan);
    connect(saveEcuScanButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::saveEcuScan);
    connect(useEcuButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::useSelectedEcu);
    connect(passiveEcuButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::inferPassiveEndpoints);
    connect(verifyEcuButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::verifySelectedEcu);
    connect(editEcuButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::editSelectedEcuDetails);
    connect(clearEcuScanButton, &QPushButton::clicked, ecuScanResults, &QListWidget::clear);
    connect(ecuAnyResponseCheck, &QCheckBox::toggled, ecuResponseSpecEdit, &QWidget::setDisabled);
    tabs->addTab(ecuDiscoveryPage, tr("ECU discovery"));

    QWidget *didPage = new QWidget(tabs);
    QVBoxLayout *didLayout = new QVBoxLayout(didPage);
    didTable = new QTableWidget(0, DidColumnCount, didPage);
    didTable->setHorizontalHeaderLabels({tr("Poll"), tr("Name"), tr("DID"), tr("Payload format"), tr("Poll ms"),
                                         tr("Raw response"), tr("Decoded"), tr("Status"), tr("Updated")});
    didTable->resizeColumnsToContents();
    didTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    didTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    didTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    didLayout->addWidget(didTable);
    QHBoxLayout *didButtons = new QHBoxLayout;
    QPushButton *addButton = new QPushButton(tr("Add DID"), didPage);
    QPushButton *removeButton = new QPushButton(tr("Remove"), didPage);
    QPushButton *importButton = new QPushButton(tr("Load DID list"), didPage);
    QPushButton *exportButton = new QPushButton(tr("Save DID list"), didPage);
    QPushButton *selectedButton = new QPushButton(tr("Send selected once"), didPage);
    QPushButton *allButton = new QPushButton(tr("Send enabled once"), didPage);
    QPushButton *graphButton = new QPushButton(tr("Live graph"), didPage);
    csvLogButton = new QPushButton(tr("Start CSV log"), didPage);
    pollingCheck = new QCheckBox(didPage);
    pollingCheck->hide();
    pollingButton = new QPushButton(tr("Start polling"), didPage);
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
    didButtons->addWidget(pollingButton);
    didButtons->addWidget(new QLabel(tr("Cycle interval"), didPage));
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
    connect(pollingButton, &QPushButton::clicked, this, [this]() {
        pollingCheck->setChecked(!pollingCheck->isChecked());
    });
    connect(pollingCheck, &QCheckBox::toggled, this, [this](bool enabled) {
        pollingTimer.stop();
        pollingButton->setText(enabled ? tr("Stop polling") : tr("Start polling"));
        if (enabled) pollEnabledDids();
    });
    connect(pollIntervalSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        if (pollingCheck->isChecked() && pollingTimer.isActive()) pollingTimer.start(value);
    });
    tabs->addTab(didPage, tr("DID requests"));

    QWidget *discoveryPage = new QWidget(tabs);
    QVBoxLayout *discoveryLayout = new QVBoxLayout(discoveryPage);
    QHBoxLayout *scanControls = new QHBoxLayout;
    scanStartEdit = new QLineEdit(QStringLiteral("0xF180"), discoveryPage);
    scanEndEdit = new QLineEdit(QStringLiteral("0xF1FF"), discoveryPage);
    scanTimeoutSpin = new QSpinBox(discoveryPage);
    scanTimeoutSpin->setRange(20, 5000);
    scanTimeoutSpin->setValue(300);
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
    QPushButton *clearScanButton = new QPushButton(tr("Clear results"), discoveryPage);
    QPushButton *addScannedButton = new QPushButton(tr("Add selected to DID requests"), discoveryPage);
    scanResultButtons->addWidget(loadScanButton);
    scanResultButtons->addWidget(saveScanButton);
    scanResultButtons->addWidget(clearScanButton);
    scanResultButtons->addStretch();
    scanResultButtons->addWidget(addScannedButton);
    discoveryLayout->addLayout(scanResultButtons);
    connect(scanStartButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::startDidScan);
    connect(scanResumeButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::resumeDidScan);
    connect(scanStopButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::stopDidScan);
    connect(loadScanButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::loadDidScan);
    connect(saveScanButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::saveDidScan);
    connect(clearScanButton, &QPushButton::clicked, scanResults, &QListWidget::clear);
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
    QPushButton *loadServiceScanButton = new QPushButton(tr("Load results"), serviceDiscoveryPage);
    QPushButton *saveServiceScanButton = new QPushButton(tr("Save results"), serviceDiscoveryPage);
    QPushButton *clearServiceScanButton = new QPushButton(tr("Clear results"), serviceDiscoveryPage);
    serviceScanStopButton->setEnabled(false);
    serviceScanButtons->addWidget(loadServiceScanButton);
    serviceScanButtons->addWidget(saveServiceScanButton);
    serviceScanButtons->addWidget(clearServiceScanButton);
    serviceScanButtons->addStretch();
    serviceScanButtons->addWidget(serviceScanStartButton);
    serviceScanButtons->addWidget(serviceScanStopButton);
    serviceDiscoveryLayout->addLayout(serviceScanButtons);
    connect(serviceScanStartButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::startServiceScan);
    connect(serviceScanStopButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::stopServiceScan);
    connect(loadServiceScanButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::loadServiceScan);
    connect(saveServiceScanButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::saveServiceScan);
    connect(clearServiceScanButton, &QPushButton::clicked, serviceScanResults, &QListWidget::clear);
    tabs->addTab(serviceDiscoveryPage, tr("Service discovery"));

    QWidget *sessionDiscoveryPage = new QWidget(tabs);
    QVBoxLayout *sessionDiscoveryLayout = new QVBoxLayout(sessionDiscoveryPage);
    sessionScanResults = new QListWidget(sessionDiscoveryPage);
    sessionDiscoveryLayout->addWidget(sessionScanResults, 1);
    QHBoxLayout *sessionButtons = new QHBoxLayout;
    QPushButton *clearSessionScanButton = new QPushButton(tr("Clear results"), sessionDiscoveryPage);
    QPushButton *sessionScanButton = new QPushButton(tr("Scan diagnostic sessions"), sessionDiscoveryPage);
    sessionButtons->addWidget(clearSessionScanButton);
    sessionButtons->addStretch();
    sessionButtons->addWidget(sessionScanButton);
    sessionDiscoveryLayout->addLayout(sessionButtons);
    connect(sessionScanButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::startSessionScan);
    connect(clearSessionScanButton, &QPushButton::clicked, sessionScanResults, &QListWidget::clear);
    tabs->addTab(sessionDiscoveryPage, tr("Session discovery"));

    QWidget *summaryPage = new QWidget(tabs);
    QVBoxLayout *summaryLayout = new QVBoxLayout(summaryPage);
    discoverySummaryTree = new QTreeWidget(summaryPage);
    discoverySummaryTree->setHeaderLabels({tr("Diagnostic discovery"), tr("Details")});
    summaryLayout->addWidget(discoverySummaryTree, 1);
    QHBoxLayout *summaryButtons = new QHBoxLayout;
    QPushButton *refreshSummaryButton = new QPushButton(tr("Refresh"), summaryPage);
    QPushButton *exportSummaryButton = new QPushButton(tr("Export CSV"), summaryPage);
    QPushButton *compareSummaryButton = new QPushButton(tr("Compare snapshot"), summaryPage);
    QPushButton *clearSummaryButton = new QPushButton(tr("Clear"), summaryPage);
    summaryButtons->addWidget(refreshSummaryButton);
    summaryButtons->addWidget(exportSummaryButton);
    summaryButtons->addWidget(compareSummaryButton);
    summaryButtons->addWidget(clearSummaryButton);
    summaryButtons->addStretch();
    summaryLayout->addLayout(summaryButtons);
    connect(refreshSummaryButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::refreshDiscoverySummary);
    connect(exportSummaryButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::exportDiscoveryCsv);
    connect(compareSummaryButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::compareDiscoverySnapshot);
    connect(clearSummaryButton, &QPushButton::clicked, discoverySummaryTree, &QTreeWidget::clear);
    tabs->addTab(summaryPage, tr("Discovery summary"));

    QWidget *dtcPage = new QWidget(tabs);
    QVBoxLayout *dtcLayout = new QVBoxLayout(dtcPage);
    QHBoxLayout *dtcControls = new QHBoxLayout;
    dtcStatusMaskEdit = new QLineEdit(QStringLiteral("0xFF"), dtcPage);
    QPushButton *readDtcsButton = new QPushButton(tr("Read DTCs"), dtcPage);
    QPushButton *clearDtcsButton = new QPushButton(tr("Clear all DTCs"), dtcPage);
    QPushButton *clearDtcOutputButton = new QPushButton(tr("Clear output"), dtcPage);
    dtcControls->addWidget(new QLabel(tr("Status mask"), dtcPage));
    dtcControls->addWidget(dtcStatusMaskEdit);
    dtcControls->addWidget(readDtcsButton);
    dtcControls->addWidget(clearDtcOutputButton);
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
    connect(clearDtcOutputButton, &QPushButton::clicked, dtcResponse, &QTextEdit::clear);
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
    QHBoxLayout *routineButtons = new QHBoxLayout;
    QPushButton *clearRoutineButton = new QPushButton(tr("Clear output"), routinePage);
    QPushButton *routineButton = new QPushButton(tr("Send Routine Control"), routinePage);
    routineButtons->addWidget(clearRoutineButton);
    routineButtons->addStretch();
    routineButtons->addWidget(routineButton);
    routineLayout->addLayout(routineButtons);
    routineResponse = new QTextEdit(routinePage);
    routineResponse->setReadOnly(true);
    routineLayout->addWidget(routineResponse);
    connect(routineButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::sendRoutineControl);
    connect(clearRoutineButton, &QPushButton::clicked, routineResponse, &QTextEdit::clear);
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
    QHBoxLayout *controlButtons = new QHBoxLayout;
    QPushButton *clearControlButton = new QPushButton(tr("Clear output"), controlPage);
    QPushButton *controlButton = new QPushButton(tr("Review and send"), controlPage);
    controlButtons->addWidget(clearControlButton);
    controlButtons->addStretch();
    controlButtons->addWidget(controlButton);
    controlLayout->addLayout(controlButtons);
    controlResponse = new QTextEdit(controlPage);
    controlResponse->setReadOnly(true);
    controlLayout->addWidget(controlResponse);
    connect(controlButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::sendGuardedControl);
    connect(clearControlButton, &QPushButton::clicked, controlResponse, &QTextEdit::clear);
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
    QPushButton *clearSecurityButton = new QPushButton(tr("Clear output"), securityPage);
    securityButtons->addWidget(clearSecurityButton);
    securityButtons->addStretch();
    securityButtons->addWidget(seedButton);
    securityButtons->addWidget(keyButton);
    securityLayout->addLayout(securityButtons);
    securityResponse = new QTextEdit(securityPage);
    securityResponse->setReadOnly(true);
    securityLayout->addWidget(securityResponse);
    connect(seedButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::requestSecuritySeed);
    connect(keyButton, &QPushButton::clicked, this, &UDSWorkbenchWindow::sendSecurityKey);
    connect(clearSecurityButton, &QPushButton::clicked, securityResponse, &QTextEdit::clear);
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
    QHBoxLayout *manualButtons = new QHBoxLayout;
    QPushButton *clearManualButton = new QPushButton(tr("Clear output"), manualPage);
    QPushButton *sendManual = new QPushButton(tr("Send request"), manualPage);
    manualButtons->addWidget(clearManualButton);
    manualButtons->addStretch();
    manualButtons->addWidget(sendManual);
    manualLayout->addLayout(manualButtons);
    manualResponse = new QTextEdit(manualPage);
    manualResponse->setReadOnly(true);
    manualLayout->addWidget(manualResponse);
    connect(sendManual, &QPushButton::clicked, this, &UDSWorkbenchWindow::sendManualRequest);
    connect(clearManualButton, &QPushButton::clicked, manualResponse, &QTextEdit::clear);
    tabs->addTab(manualPage, tr("Manual service"));
    requestControls = {selectedButton, allButton, pollingButton, scanStartButton,
        serviceScanStartButton, readDtcsButton, clearDtcsButton, dtcDetailButton,
        routineButton, controlButton, seedButton, keyButton, sendManual};
    for (QWidget *control : requestControls) control->setEnabled(false);
    root->addWidget(tabs, 1);

    eventLog = new QTextEdit(this);
    eventLog->setReadOnly(true);
    eventLog->setMaximumHeight(130);
    root->addWidget(eventLog);
    QPushButton *clearLogButton = new QPushButton(tr("Clear log"), this);
    root->addWidget(clearLogButton, 0, Qt::AlignRight);
    connect(clearLogButton, &QPushButton::clicked, eventLog, &QTextEdit::clear);
}

uint32_t UDSWorkbenchWindow::parseNumber(const QString &text, bool *ok) const
{
    QString value = text.trimmed();
    int base = 10;
    if (value.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) { value.remove(0, 2); base = 16; }
    return value.toUInt(ok, base);
}

void UDSWorkbenchWindow::updateResponseIdFromMode()
{
    const QString mode = responseAddressModeCombo->currentData().toString();
    const bool customOffset = mode == QStringLiteral("custom");
    const bool explicitId = mode == QStringLiteral("id");
    responseOffsetEdit->setVisible(customOffset);
    responseIdEdit->setReadOnly(!explicitId);
    if (explicitId) return;

    bool requestOk = false, offsetOk = false;
    const uint32_t requestId = parseNumber(requestIdEdit->text(), &requestOk);
    uint32_t offset = 0;
    if (mode == QStringLiteral("offset8")) { offset = 0x8; offsetOk = true; }
    else if (mode == QStringLiteral("offset80")) { offset = 0x80; offsetOk = true; }
    else offset = parseNumber(responseOffsetEdit->text(), &offsetOk);
    if (!requestOk || !offsetOk || quint64(requestId) + offset > 0x1FFFFFFF)
    {
        responseIdEdit->clear();
        responseIdEdit->setPlaceholderText(tr("Invalid request/offset"));
        return;
    }
    responseIdEdit->setPlaceholderText(QString());
    responseIdEdit->setText(QStringLiteral("0x%1").arg(requestId + offset, 0, 16).toUpper());
}

void UDSWorkbenchWindow::applyAddressPreset()
{
    const QString preset = addressPresetCombo->currentData().toString();
    if (preset == QStringLiteral("11physical"))
    {
        requestIdEdit->setText(QStringLiteral("0x7E0"));
        responseAddressModeCombo->setCurrentIndex(
            responseAddressModeCombo->findData(QStringLiteral("offset8")));
    }
    else if (preset == QStringLiteral("11functional"))
    {
        requestIdEdit->setText(QStringLiteral("0x7DF"));
        responseAddressModeCombo->setCurrentIndex(
            responseAddressModeCombo->findData(QStringLiteral("id")));
        responseIdEdit->setText(QStringLiteral("0x7E8"));
    }
    else if (preset == QStringLiteral("29fixed"))
    {
        requestIdEdit->setText(QStringLiteral("0x18DA10F1"));
        responseAddressModeCombo->setCurrentIndex(
            responseAddressModeCombo->findData(QStringLiteral("id")));
        responseIdEdit->setText(QStringLiteral("0x18DAF110"));
    }
    updateResponseIdFromMode();
}

void UDSWorkbenchWindow::learnResponseAddress()
{
    bool requestOk = false;
    const uint32_t requestId = parseNumber(requestIdEdit->text(), &requestOk);
    if (!requestOk) { log(tr("Enter a valid request ID first")); return; }
    ecuRequestSpecEdit->setText(QStringLiteral("0x%1").arg(requestId, 0, 16).toUpper());
    ecuAnyResponseCheck->setChecked(true);
    startEcuScan();
}

uint32_t UDSWorkbenchWindow::inferredRequestId(uint32_t responseId, bool *ok) const
{
    if (responseId >= 0x7E8 && responseId <= 0x7EF)
    {
        if (ok) *ok = true;
        return responseId - 8;
    }
    if ((responseId & 0x1FFF0000U) == 0x18DA0000U)
    {
        if (ok) *ok = true;
        return (responseId & 0x1FFF0000U) |
            ((responseId & 0xFFU) << 8) | ((responseId >> 8) & 0xFFU);
    }
    if (ok) *ok = false;
    return 0;
}

QString UDSWorkbenchWindow::describe29BitAddress(uint32_t id) const
{
    if ((id & 0x1FFF0000U) != 0x18DA0000U) return QString();
    return tr("target 0x%1, source 0x%2")
        .arg((id >> 8) & 0xFF, 2, 16, QLatin1Char('0'))
        .arg(id & 0xFF, 2, 16, QLatin1Char('0')).toUpper();
}

void UDSWorkbenchWindow::addEcuDiscoveryResult(uint32_t requestId, uint32_t responseId,
                                               const QString &status, int confidence)
{
    int responsesForRequest = 0;
    for (int index = 0; index < ecuScanResults->count(); ++index)
    {
        QListWidgetItem *existing = ecuScanResults->item(index);
        if (existing->data(Qt::UserRole).toUInt() == requestId) ++responsesForRequest;
        if (existing->data(Qt::UserRole).toUInt() == requestId &&
            existing->data(Qt::UserRole + 1).toUInt() == responseId)
        {
            if (confidence > existing->data(Qt::UserRole + 2).toInt())
                existing->setData(Qt::UserRole + 2, confidence);
            return;
        }
    }
    QString text = tr("REQ 0x%1 -> RESP 0x%2  [%3%] %4")
        .arg(requestId, 0, 16).arg(responseId, 0, 16).arg(confidence).arg(status);
    const QString address = describe29BitAddress(responseId);
    if (!address.isEmpty()) text += QStringLiteral("  ") + address;
    QListWidgetItem *item = new QListWidgetItem(text.toUpper(), ecuScanResults);
    item->setData(Qt::UserRole, requestId);
    item->setData(Qt::UserRole + 1, responseId);
    item->setData(Qt::UserRole + 2, confidence);
    if (responsesForRequest > 0)
    {
        item->setToolTip(tr("Multiple response IDs were observed for this request. Verify each endpoint physically."));
        log(tr("Ambiguous endpoint: request 0x%1 has multiple responding IDs")
            .arg(requestId, 0, 16).toUpper());
    }
}

void UDSWorkbenchWindow::inferPassiveEndpoints()
{
    const QVector<CANFrame> *frames = MainWindow::getReference()->getCANFrameModel()->getListReference();
    int added = 0;
    for (const CANFrame &frame : *frames)
    {
        if (!frame.isReceived || frame.bus != busSpin->value() ||
            frame.frameType() != QCanBusFrame::DataFrame) continue;
        const QByteArray payload = frame.payload();
        if (payload.size() < 2 || (quint8(payload[0]) >> 4) != 0) continue;
        const int length = quint8(payload[0]) & 0xF;
        const int service = quint8(payload[1]);
        if (length < 1 || !((service >= 0x50 && service <= 0x7E) || service == 0x7F)) continue;
        bool requestOk = false;
        const uint32_t requestId = inferredRequestId(frame.frameId(), &requestOk);
        if (!requestOk) continue;
        const int before = ecuScanResults->count();
        addEcuDiscoveryResult(requestId, frame.frameId(), tr("passive UDS response"), 70);
        if (ecuScanResults->count() > before) ++added;
    }
    log(tr("Passive inference added %1 endpoint pair(s)").arg(added));
    refreshDiscoverySummary();
}

void UDSWorkbenchWindow::verifySelectedEcu()
{
    QListWidgetItem *item = ecuScanResults->currentItem();
    if (!item) { log(tr("Select an endpoint to verify")); return; }
    ecuRequestSpecEdit->setText(QStringLiteral("0x%1")
        .arg(item->data(Qt::UserRole).toUInt(), 0, 16).toUpper());
    ecuResponseSpecEdit->setText(QStringLiteral("0x%1")
        .arg(item->data(Qt::UserRole + 1).toUInt(), 0, 16).toUpper());
    ecuAnyResponseCheck->setChecked(false);
    startEcuScan();
}

void UDSWorkbenchWindow::editSelectedEcuDetails()
{
    QListWidgetItem *item = ecuScanResults->currentItem();
    if (!item) return;
    bool accepted = false;
    const QString name = QInputDialog::getText(this, tr("Endpoint name"), tr("Name"),
        QLineEdit::Normal, item->data(Qt::UserRole + 3).toString(), &accepted);
    if (!accepted) return;
    const QString notes = QInputDialog::getMultiLineText(this, tr("Endpoint notes"), tr("Notes"),
        item->data(Qt::UserRole + 4).toString(), &accepted);
    if (!accepted) return;
    item->setData(Qt::UserRole + 3, name);
    item->setData(Qt::UserRole + 4, notes);
    item->setToolTip(notes);
    if (!name.trimmed().isEmpty() && !item->text().startsWith(name + QStringLiteral(": ")))
        item->setText(name + QStringLiteral(": ") + item->text());
}

bool UDSWorkbenchWindow::transmissionAllowed(int service, bool modifying)
{
    const QString mode = safetyModeCombo->currentData().toString();
    if (mode == QStringLiteral("passive"))
    {
        connectionStatus->setText(tr("Passive safety mode blocks transmission"));
        log(tr("Blocked service 0x%1 by passive safety mode").arg(service, 2, 16, QLatin1Char('0')));
        return false;
    }
    if (modifying && mode != QStringLiteral("full"))
    {
        connectionStatus->setText(tr("Full diagnostics safety mode required"));
        log(tr("Blocked modifying service 0x%1 in read-only mode").arg(service, 2, 16, QLatin1Char('0')));
        return false;
    }
    return true;
}

QVector<uint32_t> UDSWorkbenchWindow::parseIdSpec(const QString &text, bool *ok) const
{
    QVector<uint32_t> ids;
    bool valid = true;
    for (const QString &entryText : text.split(',', Qt::SkipEmptyParts))
    {
        const QStringList bounds = entryText.trimmed().split('-', Qt::KeepEmptyParts);
        bool firstOk = false, lastOk = false;
        const uint32_t first = parseNumber(bounds.value(0), &firstOk);
        const uint32_t last = bounds.size() == 1 ? first : parseNumber(bounds.value(1), &lastOk);
        if (bounds.size() == 1) lastOk = firstOk;
        if (bounds.size() > 2 || !firstOk || !lastOk || first > last ||
            last > 0x1FFFFFFF || quint64(last) - first > 4095)
        {
            valid = false;
            break;
        }
        for (quint64 id = first; id <= last; ++id)
            if (!ids.contains(uint32_t(id))) ids.append(uint32_t(id));
    }
    valid = valid && !ids.isEmpty();
    if (ok) *ok = valid;
    return valid ? ids : QVector<uint32_t>();
}

void UDSWorkbenchWindow::connectEndpoint()
{
    if (!transmissionAllowed(UDS_SERVICES::DIAG_CONTROL)) return;
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
    udsHandler->setFlowCtrl(true);
    udsHandler->setFlowControlParameters(flowBlockSizeSpin->value(), flowStMinSpin->value());
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
    if (ecuScanActive)
    {
        finishEcuScan(tr("ECU scan stopped"));
        return;
    }
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
    for (QWidget *control : requestControls) control->setEnabled(false);
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

QJsonObject UDSWorkbenchWindow::aiState() const
{
    QJsonArray requests;
    for (int row = 0; row < didTable->rowCount(); ++row)
    {
        requests.append(QJsonObject{
            {QStringLiteral("enabled"),
             didTable->item(row, DidEnabled)->checkState() == Qt::Checked},
            {QStringLiteral("name"), didTable->item(row, DidName)->text()},
            {QStringLiteral("did"), didTable->item(row, DidIdentifier)->text()},
            {QStringLiteral("format"), didTable->item(row, DidFormat)->text()},
            {QStringLiteral("poll_ms"), didTable->item(row, DidPollMs)->text().toInt()}
        });
    }
    return QJsonObject{
        {QStringLiteral("connected"), endpointConnected},
        {QStringLiteral("bus"), busSpin->value()},
        {QStringLiteral("request_id"), requestIdEdit->text()},
        {QStringLiteral("response_id"), responseIdEdit->text()},
        {QStringLiteral("polling"), pollingCheck->isChecked()},
        {QStringLiteral("poll_cycle_ms"), pollIntervalSpin->value()},
        {QStringLiteral("requests"), requests}
    };
}

bool UDSWorkbenchWindow::executeAIRequest(const QString &operation,
                                          const QJsonObject &arguments, QString *error)
{
    if (operation == QStringLiteral("connect")) {
        connectEndpoint();
        return endpointConnected || responseTimer.isActive();
    }
    if (operation == QStringLiteral("disconnect")) {
        disconnectEndpoint();
        return !endpointConnected && !responseTimer.isActive();
    }
    if (operation == QStringLiteral("clear_dids")) {
        didQueue.clear();
        activeDidRow = -1;
        didTable->setRowCount(0);
        return true;
    }
    if (operation == QStringLiteral("stop_polling")) {
        pollingCheck->setChecked(false);
        return !pollingCheck->isChecked();
    }
    if (operation == QStringLiteral("scan_ecus")) {
        startEcuScan();
        return ecuScanActive;
    }
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
    if (operation == QStringLiteral("request_all")) {
        requestAllDids();
        return responseTimer.isActive() || activeDidRow >= 0;
    }
    if (operation == QStringLiteral("start_polling")) {
        pollingCheck->setChecked(true);
        return pollingCheck->isChecked();
    }
    if (operation == QStringLiteral("scan_services")) {
        startServiceScan();
        return serviceScanActive;
    }
    if (operation == QStringLiteral("scan_sessions")) {
        startSessionScan();
        return sessionScanActive;
    }
    if (operation == QStringLiteral("dtc") || operation == QStringLiteral("read_dtcs")) {
        requestDtcs();
        return responseTimer.isActive();
    }
    if (operation == QStringLiteral("clear_dtcs")) {
        sendServiceRequest(UDS_SERVICES::CLEAR_DIAG, QByteArray::fromHex("FFFFFF"),
                           ContextDtcClear);
        return responseTimer.isActive();
    }
    if (operation == QStringLiteral("tester_present")) {
        sendTesterPresent();
        return true;
    }
    if (operation == QStringLiteral("routine_control")) {
        const QString control = arguments.value("control").toString().toLower();
        const QMap<QString, int> controls = {
            {QStringLiteral("start"), 1}, {QStringLiteral("stop"), 2},
            {QStringLiteral("results"), 3}
        };
        const int type = controls.value(control, arguments.value("control").toInt(1));
        const int index = routineTypeCombo->findData(type);
        if (index < 0) {
            if (error) *error = tr("Routine control must be start, stop, or results.");
            return false;
        }
        routineTypeCombo->setCurrentIndex(index);
        routineIdEdit->setText(arguments.value("routine_id").toVariant().toString());
        routineDataEdit->setText(arguments.value("data").toString());
        sendRoutineControl();
        return responseTimer.isActive();
    }
    if (operation == QStringLiteral("security_seed")) {
        securityLevelSpin->setValue(arguments.value("level").toInt(1));
        requestSecuritySeed();
        return responseTimer.isActive();
    }
    if (operation == QStringLiteral("security_key")) {
        int level = arguments.value("level").toInt(1);
        if ((level & 1) == 0) --level;
        const QByteArray key = QByteArray::fromHex(arguments.value("key").toString().toLatin1());
        if (key.isEmpty()) {
            if (error) *error = tr("SecurityAccess key bytes are required.");
            return false;
        }
        QByteArray data(1, char(level + 1));
        data.append(key);
        sendServiceRequest(UDS_SERVICES::SECURITY_ACCESS, data, ContextSecurity);
        return responseTimer.isActive();
    }
    if (operation == QStringLiteral("detailed_dtc")) {
        const int subfunction = arguments.value("subfunction").toVariant().toInt();
        const int index = dtcDetailTypeCombo->findData(subfunction);
        if (index < 0) {
            if (error) *error = tr("Unsupported ReadDTCInformation subfunction.");
            return false;
        }
        dtcDetailTypeCombo->setCurrentIndex(index);
        dtcDetailDataEdit->setText(arguments.value("data").toString());
        sendDetailedDtcRequest();
        return responseTimer.isActive();
    }
    if (operation == QStringLiteral("manual_request")
        || operation == QStringLiteral("guarded_control")) {
        bool ok = false;
        const int service = parseNumber(arguments.value("service").toVariant().toString(), &ok);
        if (!ok || service < 0 || service > 0xFF) {
            if (error) *error = tr("UDS service must be an 8-bit value.");
            return false;
        }
        sendServiceRequest(service,
            QByteArray::fromHex(arguments.value("data").toString().toLatin1()),
            operation == QStringLiteral("guarded_control") ? ContextControl : ContextManual);
        return responseTimer.isActive();
    }
    if (error) *error = tr("Unsupported UDS AI operation: %1").arg(operation);
    return false;
}

void UDSWorkbenchWindow::removeDid()
{
    QSet<int> selectedRows;
    for (const QModelIndex &index : didTable->selectionModel()->selectedRows())
        selectedRows.insert(index.row());
    QList<int> rows = selectedRows.values();
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    for (int row : rows) didTable->removeRow(row);
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
    responseOffsetEdit->setText(profile.value("responseOffset").toString(QStringLiteral("0x8")));
    const int addressMode = responseAddressModeCombo->findData(
        profile.value("responseMode").toString(QStringLiteral("id")));
    responseAddressModeCombo->setCurrentIndex(qMax(0, addressMode));
    updateResponseIdFromMode();
    const int session = profile.value("session").toInt(sessionCombo->currentData().toInt());
    const int sessionIndex = sessionCombo->findData(session);
    if (sessionIndex >= 0) sessionCombo->setCurrentIndex(sessionIndex);
    const int safetyIndex = safetyModeCombo->findData(profile.value("safetyMode").toString(QStringLiteral("read")));
    safetyModeCombo->setCurrentIndex(qMax(0, safetyIndex));
    p2TimeoutSpin->setValue(profile.value("p2Timeout").toInt(p2TimeoutSpin->value()));
    p2StarTimeoutSpin->setValue(profile.value("p2StarTimeout").toInt(p2StarTimeoutSpin->value()));
    flowBlockSizeSpin->setValue(profile.value("blockSize").toInt(flowBlockSizeSpin->value()));
    flowStMinSpin->setValue(profile.value("stMin").toInt(flowStMinSpin->value()));
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
    profile["responseMode"] = responseAddressModeCombo->currentData().toString();
    profile["responseOffset"] = responseOffsetEdit->text();
    profile["session"] = sessionCombo->currentData().toInt();
    profile["safetyMode"] = safetyModeCombo->currentData().toString();
    profile["p2Timeout"] = p2TimeoutSpin->value();
    profile["p2StarTimeout"] = p2StarTimeoutSpin->value();
    profile["blockSize"] = flowBlockSizeSpin->value();
    profile["stMin"] = flowStMinSpin->value();
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
    if (didQueue.isEmpty() && activeDidRow < 0 && pollingCheck->isChecked())
        pollingTimer.start(pollIntervalSpin->value());
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
    const bool modifying = service == UDS_SERVICES::CLEAR_DIAG || service == UDS_SERVICES::ECU_RESET ||
        service == UDS_SERVICES::COMM_CTRL || service == UDS_SERVICES::WRITE_BY_ID ||
        service == UDS_SERVICES::IO_CTRL || service == UDS_SERVICES::ROUTINE_CTRL ||
        service == UDS_SERVICES::SECURITY_ACCESS || service == UDS_SERVICES::REQUEST_DOWNLOAD ||
        service == UDS_SERVICES::REQUEST_UPLOAD || service == UDS_SERVICES::WRITE_BY_ADDR;
    if (!transmissionAllowed(service, modifying)) return;
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
    if (didQueue.isEmpty())
    {
        activeDidRow = -1;
        activeService = -1;
        if (pollingCheck->isChecked() && endpointConnected)
            pollingTimer.start(pollIntervalSpin->value());
        return;
    }
    sendDidRow(didQueue.dequeue());
}

void UDSWorkbenchWindow::sendDidRow(int row)
{
    if (!transmissionAllowed(UDS_SERVICES::READ_BY_ID)) { didQueue.clear(); return; }
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
    if (!transmissionAllowed(service, service != UDS_SERVICES::READ_BY_ID &&
        service != UDS_SERVICES::READ_DTC && service != UDS_SERVICES::TESTER_PRESENT)) return;
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

void UDSWorkbenchWindow::startEcuScan()
{
    if (!transmissionAllowed(UDS_SERVICES::TESTER_PRESENT)) return;
    if (endpointConnected || responseTimer.isActive() || didScanActive || serviceScanActive)
    {
        log(tr("Disconnect the active session or wait for the current request"));
        return;
    }
    if (!CANConManager::getInstance()->isBusConnected(busSpin->value()))
    {
        connectionStatus->setText(tr("Bus %1 is not connected").arg(busSpin->value()));
        return;
    }
    bool requestsOk = false, responsesOk = false;
    const QVector<uint32_t> requests = parseIdSpec(ecuRequestSpecEdit->text(), &requestsOk);
    ecuAcceptAnyResponse = ecuAnyResponseCheck->isChecked();
    if (ecuAcceptAnyResponse)
        responsesOk = true;
    else
        ecuResponseIds = parseIdSpec(ecuResponseSpecEdit->text(), &responsesOk);
    if (!requestsOk || !responsesOk)
    {
        log(tr("Invalid ECU request/response ID list or range"));
        return;
    }
    QVector<uint32_t> limitedRequests = requests;
    if (limitedRequests.size() > ecuScanLimitSpin->value())
        limitedRequests.resize(ecuScanLimitSpin->value());
    QSettings resumeSettings;
    const QString resumeText = resumeSettings.value("UDSWorkbench/EcuScanRemaining").toString();
    if (!resumeText.isEmpty() && QMessageBox::question(this, tr("Resume ECU scan"),
            tr("Resume the previously interrupted ECU discovery job?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) == QMessageBox::Yes)
    {
        bool resumeOk = false;
        const QVector<uint32_t> resumed = parseIdSpec(resumeText, &resumeOk);
        if (resumeOk) limitedRequests = resumed;
    }
    if (limitedRequests.size() > 256 && QMessageBox::warning(this, tr("Large ECU scan"),
            tr("Probe %1 request IDs? This creates active diagnostic traffic.").arg(limitedRequests.size()),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;

    udsHandler->clearAllFilters();
    if (ecuAcceptAnyResponse)
        udsHandler->addFilter(busSpin->value(), 0, 0);
    else
        for (uint32_t responseId : ecuResponseIds)
            udsHandler->addFilter(busSpin->value(), responseId, responseId > 0x7FF ? 0x1FFFFFFF : 0x7FF);
    udsHandler->setReception(true);
    ecuRequestQueue.clear();
    for (uint32_t requestId : limitedRequests) ecuRequestQueue.enqueue(requestId);
    ecuScanActive = true;
    ecuProbeCount = 0;
    ecuErrorFrameBaseline = 0;
    const QVector<CANFrame> *frames = MainWindow::getReference()->getCANFrameModel()->getListReference();
    for (const CANFrame &frame : *frames)
        if (frame.bus == busSpin->value() && frame.frameType() == QCanBusFrame::ErrorFrame)
            ++ecuErrorFrameBaseline;
    ecuScanStartButton->setEnabled(false);
    ecuScanStopButton->setEnabled(true);
    responseTimer.setInterval(ecuScanTimeoutSpin->value());
    connectionStatus->setText(tr("Scanning ECU IDs"));
    log(ecuAcceptAnyResponse
        ? tr("Scanning %1 request ID(s) for any UDS response ID").arg(limitedRequests.size())
        : tr("Scanning %1 request ID(s) against %2 response ID(s)")
            .arg(limitedRequests.size()).arg(ecuResponseIds.size()));
    sendNextEcuProbe();
}

void UDSWorkbenchWindow::sendNextEcuProbe()
{
    if (!ecuScanActive) return;
    int errorFrames = 0;
    const QVector<CANFrame> *frames = MainWindow::getReference()->getCANFrameModel()->getListReference();
    for (const CANFrame &frame : *frames)
        if (frame.bus == busSpin->value() && frame.frameType() == QCanBusFrame::ErrorFrame)
            ++errorFrames;
    if (errorFrames > ecuErrorFrameBaseline)
    {
        finishEcuScan(tr("ECU scan aborted after CAN error frame"));
        return;
    }
    if (ecuRequestQueue.isEmpty())
    {
        finishEcuScan(tr("ECU scan complete"));
        return;
    }
    activeEcuRequestId = ecuRequestQueue.dequeue();
    ++ecuProbeCount;
    QStringList remaining;
    remaining << QStringLiteral("0x%1").arg(activeEcuRequestId, 0, 16);
    for (uint32_t requestId : ecuRequestQueue)
        remaining << QStringLiteral("0x%1").arg(requestId, 0, 16);
    QSettings().setValue("UDSWorkbench/EcuScanRemaining", remaining.join(','));
    UDS_MESSAGE message;
    message.bus = busSpin->value();
    message.setFrameId(activeEcuRequestId);
    message.setExtendedFrameFormat(activeEcuRequestId > 0x7FF);
    message.service = UDS_SERVICES::TESTER_PRESENT;
    message.subFuncLen = 1;
    message.subFunc = 0;
    activeService = message.service;
    requestContext = ContextEcuScan;
    if (!udsHandler->sendUDSFrame(message))
    {
        finishEcuScan(tr("ECU scan transmit failed"));
        return;
    }
    responseTimer.start();
}

void UDSWorkbenchWindow::stopEcuScan()
{
    if (ecuScanActive) finishEcuScan(tr("ECU scan stopped"));
}

void UDSWorkbenchWindow::finishEcuScan(const QString &status)
{
    responseTimer.stop();
    const bool completed = status == tr("ECU scan complete");
    ecuScanActive = false;
    ecuRequestQueue.clear();
    ecuResponseIds.clear();
    ecuAcceptAnyResponse = false;
    if (completed) QSettings().remove("UDSWorkbench/EcuScanRemaining");
    activeEcuRequestId = 0;
    activeService = -1;
    requestContext = ContextNone;
    responseTimer.setInterval(normalResponseTimeout);
    udsHandler->clearAllFilters();
    udsHandler->setReception(false);
    ecuScanStartButton->setEnabled(true);
    ecuScanStopButton->setEnabled(false);
    connectionStatus->setText(tr("Disconnected"));
    log(tr("%1; %2 ECU endpoint pair(s)").arg(status).arg(ecuScanResults->count()));
    refreshDiscoverySummary();
}

void UDSWorkbenchWindow::useSelectedEcu()
{
    QListWidgetItem *item = ecuScanResults->currentItem();
    if (!item) { log(tr("Select a discovered ECU first")); return; }
    requestIdEdit->setText(QStringLiteral("0x%1").arg(item->data(Qt::UserRole).toUInt(), 0, 16).toUpper());
    responseAddressModeCombo->setCurrentIndex(
        responseAddressModeCombo->findData(QStringLiteral("id")));
    responseIdEdit->setText(QStringLiteral("0x%1").arg(item->data(Qt::UserRole + 1).toUInt(), 0, 16).toUpper());
    connectionStatus->setText(tr("Endpoint selected - connect session"));
    log(tr("Selected request %1 and response %2")
        .arg(requestIdEdit->text(), responseIdEdit->text()));
}

void UDSWorkbenchWindow::saveEcuScan()
{
    if (ecuScanResults->count() == 0) { log(tr("There are no ECU scan results to save")); return; }
    QString fileName = QFileDialog::getSaveFileName(this, tr("Save ECU discovery results"), QString(),
                                                     tr("UDS ECU discovery (*.json);;All files (*)"));
    if (fileName.isEmpty()) return;
    if (!fileName.endsWith(".json", Qt::CaseInsensitive)) fileName += ".json";
    QJsonArray results;
    for (int index = 0; index < ecuScanResults->count(); ++index)
    {
        QListWidgetItem *item = ecuScanResults->item(index);
        QJsonObject result;
        result["requestId"] = QStringLiteral("0x%1").arg(item->data(Qt::UserRole).toUInt(), 0, 16).toUpper();
        result["responseId"] = QStringLiteral("0x%1").arg(item->data(Qt::UserRole + 1).toUInt(), 0, 16).toUpper();
        result["status"] = item->text();
        result["confidence"] = item->data(Qt::UserRole + 2).toInt();
        result["name"] = item->data(Qt::UserRole + 3).toString();
        result["notes"] = item->data(Qt::UserRole + 4).toString();
        results.append(result);
    }
    QJsonObject root;
    root["version"] = 1;
    root["results"] = results;
    QSaveFile file(fileName);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(QJsonDocument(root).toJson()) < 0 || !file.commit())
        log(tr("Could not save ECU discovery: %1").arg(file.errorString()));
    else
        log(tr("Saved %1 ECU discovery result(s)").arg(results.size()));
}

void UDSWorkbenchWindow::loadEcuScan()
{
    const QString fileName = QFileDialog::getOpenFileName(this, tr("Load ECU discovery results"), QString(),
                                                          tr("UDS ECU discovery (*.json);;All files (*)"));
    if (fileName.isEmpty()) return;
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) { log(tr("Could not open ECU discovery file")); return; }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject() ||
        !document.object()["results"].isArray())
    {
        log(tr("Invalid ECU discovery file"));
        return;
    }
    for (const QJsonValue &value : document.object()["results"].toArray())
    {
        const QJsonObject result = value.toObject();
        bool requestOk = false, responseOk = false;
        const uint32_t requestId = parseNumber(result["requestId"].toString(), &requestOk);
        const uint32_t responseId = parseNumber(result["responseId"].toString(), &responseOk);
        if (!requestOk || !responseOk) continue;
        const int before = ecuScanResults->count();
        addEcuDiscoveryResult(requestId, responseId, result["status"].toString(),
                              result["confidence"].toInt(75));
        if (ecuScanResults->count() > before)
        {
            QListWidgetItem *item = ecuScanResults->item(ecuScanResults->count() - 1);
            item->setData(Qt::UserRole + 3, result["name"].toString());
            item->setData(Qt::UserRole + 4, result["notes"].toString());
            item->setToolTip(result["notes"].toString());
        }
    }
    log(tr("Loaded %1 ECU discovery result(s)").arg(ecuScanResults->count()));
}

void UDSWorkbenchWindow::startDidScan()
{
    if (!transmissionAllowed(UDS_SERVICES::READ_BY_ID)) return;
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
    if (!udsHandler->sendUDSFrame(message))
    {
        finishDidScan(tr("DID scan transmit failed"));
        return;
    }
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
    if (pollingCheck->isChecked()) pollingTimer.start(pollIntervalSpin->value());
    log(tr("%1; %2 DIDs found").arg(status).arg(scanResults->count()));
    refreshDiscoverySummary();
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
    if (!transmissionAllowed(0)) return;
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

void UDSWorkbenchWindow::startSessionScan()
{
    if (!endpointConnected) { connectionStatus->setText(tr("Connect a session first")); return; }
    if (!transmissionAllowed(UDS_SERVICES::DIAG_CONTROL) || responseTimer.isActive()) return;
    sessionScanQueue.clear();
    originalSessionScan = sessionCombo->currentData().toInt();
    for (int session : {1, 2, 3, 4}) sessionScanQueue.enqueue(session);
    sessionScanResults->clear();
    sessionScanActive = true;
    pollingTimer.stop();
    sendNextSessionScan();
}

void UDSWorkbenchWindow::sendNextSessionScan()
{
    if (!sessionScanActive) return;
    if (sessionScanQueue.isEmpty())
    {
        finishSessionScan(tr("Session scan complete"));
        return;
    }
    bool requestOk = false;
    const uint32_t requestId = parseNumber(requestIdEdit->text(), &requestOk);
    if (!requestOk) { finishSessionScan(tr("Invalid request ID")); return; }
    activeSessionScan = sessionScanQueue.dequeue();
    UDS_MESSAGE message;
    message.bus = busSpin->value();
    message.setFrameId(requestId);
    message.setExtendedFrameFormat(requestId > 0x7FF);
    message.service = UDS_SERVICES::DIAG_CONTROL;
    message.subFuncLen = 1;
    message.subFunc = activeSessionScan;
    activeService = message.service;
    requestContext = ContextSessionScan;
    if (!udsHandler->sendUDSFrame(message))
    {
        finishSessionScan(tr("Session scan transmit failed"));
        return;
    }
    responseTimer.start();
}

void UDSWorkbenchWindow::finishSessionScan(const QString &status)
{
    responseTimer.stop();
    bool requestOk = false;
    const uint32_t requestId = parseNumber(requestIdEdit->text(), &requestOk);
    if (requestOk)
    {
        UDS_MESSAGE restore;
        restore.bus = busSpin->value();
        restore.setFrameId(requestId);
        restore.setExtendedFrameFormat(requestId > 0x7FF);
        restore.service = UDS_SERVICES::DIAG_CONTROL;
        restore.subFuncLen = 1;
        restore.subFunc = originalSessionScan;
        if (udsHandler->sendUDSFrame(restore))
            log(tr("Restored diagnostic session 0x%1 after discovery")
                .arg(originalSessionScan, 2, 16, QLatin1Char('0')));
    }
    sessionScanQueue.clear();
    sessionScanActive = false;
    activeSessionScan = -1;
    activeService = -1;
    requestContext = ContextNone;
    if (pollingCheck->isChecked()) pollingTimer.start(pollIntervalSpin->value());
    log(tr("%1; %2 supported session(s)").arg(status).arg(sessionScanResults->count()));
    refreshDiscoverySummary();
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
    if (!udsHandler->sendUDSFrame(message))
    {
        finishServiceScan(tr("Service scan transmit failed"));
        return;
    }
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
    if (pollingCheck->isChecked()) pollingTimer.start(pollIntervalSpin->value());
    log(tr("%1; %2 recognized services").arg(status).arg(serviceScanResults->count()));
    refreshDiscoverySummary();
}

void UDSWorkbenchWindow::saveServiceScan()
{
    if (serviceScanResults->count() == 0) { log(tr("There are no service scan results to save")); return; }
    QString fileName = QFileDialog::getSaveFileName(this, tr("Save service discovery results"), QString(),
                                                     tr("UDS service discovery (*.json);;All files (*)"));
    if (fileName.isEmpty()) return;
    if (!fileName.endsWith(".json", Qt::CaseInsensitive)) fileName += ".json";
    QJsonArray results;
    for (int index = 0; index < serviceScanResults->count(); ++index)
    {
        QListWidgetItem *item = serviceScanResults->item(index);
        QJsonObject result;
        result["service"] = item->data(Qt::UserRole).toInt();
        result["nrc"] = item->data(Qt::UserRole + 1).toInt();
        result["status"] = item->text();
        results.append(result);
    }
    QJsonObject root;
    root["version"] = 1;
    root["results"] = results;
    QSaveFile file(fileName);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(QJsonDocument(root).toJson()) < 0 || !file.commit())
        log(tr("Could not save service discovery: %1").arg(file.errorString()));
    else
        log(tr("Saved %1 service discovery result(s)").arg(results.size()));
}

void UDSWorkbenchWindow::loadServiceScan()
{
    const QString fileName = QFileDialog::getOpenFileName(this, tr("Load service discovery results"), QString(),
                                                          tr("UDS service discovery (*.json);;All files (*)"));
    if (fileName.isEmpty()) return;
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) { log(tr("Could not open service discovery file")); return; }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject() ||
        !document.object()["results"].isArray())
    {
        log(tr("Invalid service discovery file"));
        return;
    }
    serviceScanResults->clear();
    for (const QJsonValue &value : document.object()["results"].toArray())
    {
        const QJsonObject result = value.toObject();
        if (!result["service"].isDouble() || !result["status"].isString()) continue;
        QListWidgetItem *item = new QListWidgetItem(result["status"].toString(), serviceScanResults);
        item->setData(Qt::UserRole, result["service"].toInt());
        item->setData(Qt::UserRole + 1, result["nrc"].toInt(-1));
    }
    log(tr("Loaded %1 service discovery result(s)").arg(serviceScanResults->count()));
}

void UDSWorkbenchWindow::refreshDiscoverySummary()
{
    discoverySummaryTree->clear();
    QTreeWidgetItem *endpoint = new QTreeWidgetItem(discoverySummaryTree,
        {tr("Endpoint"), tr("%1 -> %2").arg(requestIdEdit->text(), responseIdEdit->text())});
    QTreeWidgetItem *ecus = new QTreeWidgetItem(endpoint, {tr("ECUs"), QString::number(ecuScanResults->count())});
    for (int i = 0; i < ecuScanResults->count(); ++i)
        new QTreeWidgetItem(ecus, {ecuScanResults->item(i)->text(), QString()});
    QTreeWidgetItem *sessions = new QTreeWidgetItem(endpoint,
        {tr("Sessions"), QString::number(sessionScanResults->count())});
    for (int i = 0; i < sessionScanResults->count(); ++i)
        new QTreeWidgetItem(sessions, {sessionScanResults->item(i)->text(), QString()});
    QTreeWidgetItem *services = new QTreeWidgetItem(endpoint,
        {tr("Services"), QString::number(serviceScanResults->count())});
    for (int i = 0; i < serviceScanResults->count(); ++i)
        new QTreeWidgetItem(services, {serviceScanResults->item(i)->text(), QString()});
    QTreeWidgetItem *dids = new QTreeWidgetItem(endpoint,
        {tr("Discovered DIDs"), QString::number(scanResults->count())});
    for (int i = 0; i < scanResults->count(); ++i)
        new QTreeWidgetItem(dids, {scanResults->item(i)->text(), QString()});
    discoverySummaryTree->expandAll();
}

void UDSWorkbenchWindow::exportDiscoveryCsv()
{
    refreshDiscoverySummary();
    QString fileName = QFileDialog::getSaveFileName(this, tr("Export discovery summary"), QString(),
                                                     tr("CSV files (*.csv);;All files (*)"));
    if (fileName.isEmpty()) return;
    if (!fileName.endsWith(".csv", Qt::CaseInsensitive)) fileName += ".csv";
    QSaveFile file(fileName);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) { log(tr("Could not export summary")); return; }
    QTextStream stream(&file);
    stream << "category,item,details\n";
    for (int category = 0; category < discoverySummaryTree->topLevelItem(0)->childCount(); ++category)
    {
        QTreeWidgetItem *group = discoverySummaryTree->topLevelItem(0)->child(category);
        for (int row = 0; row < group->childCount(); ++row)
            stream << '"' << group->text(0).replace('"', "\"\"") << "\",\""
                   << group->child(row)->text(0).replace('"', "\"\"") << "\",\""
                   << group->child(row)->text(1).replace('"', "\"\"") << "\"\n";
    }
    if (!file.commit()) log(tr("Could not commit discovery CSV"));
    else log(tr("Exported discovery summary to %1").arg(fileName));
}

void UDSWorkbenchWindow::compareDiscoverySnapshot()
{
    refreshDiscoverySummary();
    QStringList current;
    QTreeWidgetItem *root = discoverySummaryTree->topLevelItem(0);
    for (int category = 0; root && category < root->childCount(); ++category)
        for (int row = 0; row < root->child(category)->childCount(); ++row)
            current << root->child(category)->text(0) + QStringLiteral("|") +
                       root->child(category)->child(row)->text(0);

    QMessageBox box(QMessageBox::Question, tr("Discovery snapshot"),
                    tr("Save the current discovery state or compare it with an earlier snapshot?"),
                    QMessageBox::Save | QMessageBox::Open | QMessageBox::Cancel, this);
    const int choice = box.exec();
    if (choice == QMessageBox::Save)
    {
        QString fileName = QFileDialog::getSaveFileName(this, tr("Save discovery snapshot"), QString(),
                                                         tr("Discovery snapshot (*.json)"));
        if (fileName.isEmpty()) return;
        if (!fileName.endsWith(".json", Qt::CaseInsensitive)) fileName += ".json";
        QJsonArray entries;
        for (const QString &entry : current) entries.append(entry);
        QJsonObject object;
        object["version"] = 1;
        object["entries"] = entries;
        QSaveFile file(fileName);
        if (file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(object).toJson()) >= 0 && file.commit())
            log(tr("Saved discovery snapshot"));
        else
            log(tr("Could not save discovery snapshot"));
        return;
    }
    if (choice != QMessageBox::Open) return;
    const QString fileName = QFileDialog::getOpenFileName(this, tr("Compare discovery snapshot"), QString(),
                                                          tr("Discovery snapshot (*.json)"));
    if (fileName.isEmpty()) return;
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) { log(tr("Could not open discovery snapshot")); return; }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    QStringList previous;
    for (const QJsonValue &value : document.object()["entries"].toArray()) previous << value.toString();
    QStringList added, removed;
    for (const QString &entry : current) if (!previous.contains(entry)) added << entry;
    for (const QString &entry : previous) if (!current.contains(entry)) removed << entry;
    QMessageBox::information(this, tr("Discovery comparison"),
        tr("Added (%1):\n%2\n\nMissing (%3):\n%4")
            .arg(added.size()).arg(added.join('\n'))
            .arg(removed.size()).arg(removed.join('\n')));
}

void UDSWorkbenchWindow::gotUDSReply(UDS_MESSAGE message)
{
    if (ecuScanActive && requestContext == ContextEcuScan)
    {
        if (message.bus != busSpin->value() ||
            (!ecuAcceptAnyResponse && !ecuResponseIds.contains(message.frameId())) ||
            (message.service != activeService && message.service != activeService + 0x40))
            return;
        QString result;
        if (message.isErrorReply)
            result += tr("NRC 0x%1: %2").arg(message.subFunc, 2, 16, QLatin1Char('0'))
                .arg(udsHandler->getNegativeResponseShort(message.subFunc));
        else
            result += tr("TesterPresent positive");
        addEcuDiscoveryResult(activeEcuRequestId, message.frameId(), result,
                              message.isErrorReply ? 85 : 100);
        return;
    }

    bool responseOk = false;
    const uint32_t responseId = parseNumber(responseIdEdit->text(), &responseOk);
    if (!responseOk || message.bus != busSpin->value() || message.frameId() != responseId ||
        (message.service != activeService && message.service != activeService + 0x40))
        return;
    if (message.isErrorReply)
    {
        const int nrc = message.subFunc;
        if (nrc == 0x78) {
            responseTimer.start(p2StarTimeoutSpin->value());
            log(tr("Response pending; P2* timeout active"));
            return;
        }
        const QString status = tr("NRC 0x%1: %2").arg(nrc, 2, 16, QLatin1Char('0'))
            .arg(udsHandler->getNegativeResponseShort(nrc));
        if (requestContext == ContextSession)
        {
            endpointConnected = false;
            connectButton->setText(tr("Connect session"));
            for (QWidget *control : requestControls) control->setEnabled(false);
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
            {
                QListWidgetItem *item = new QListWidgetItem(
                    tr("0x%1  %2  NRC 0x%3: %4")
                        .arg(activeService, 2, 16, QLatin1Char('0')).arg(serviceName(activeService))
                        .arg(nrc, 2, 16, QLatin1Char('0')).arg(udsHandler->getNegativeResponseShort(nrc))
                        .toUpper(), serviceScanResults);
                item->setData(Qt::UserRole, activeService);
                item->setData(Qt::UserRole + 1, nrc);
            }
            sendNextServiceScan();
        }
        else if (requestContext == ContextSessionScan)
        {
            responseTimer.stop();
            if (nrc != 0x12)
                sessionScanResults->addItem(tr("0x%1  NRC 0x%2: %3")
                    .arg(activeSessionScan, 2, 16, QLatin1Char('0'))
                    .arg(nrc, 2, 16, QLatin1Char('0'))
                    .arg(udsHandler->getNegativeResponseShort(nrc)).toUpper());
            sendNextSessionScan();
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
        QListWidgetItem *item = new QListWidgetItem(
            tr("0x%1  %2  positive response")
                .arg(activeService, 2, 16, QLatin1Char('0')).arg(serviceName(activeService)).toUpper(),
            serviceScanResults);
        item->setData(Qt::UserRole, activeService);
        item->setData(Qt::UserRole + 1, -1);
        sendNextServiceScan();
    }
    else if (requestContext == ContextSessionScan)
    {
        sessionScanResults->addItem(tr("0x%1  positive response")
            .arg(activeSessionScan, 2, 16, QLatin1Char('0')).toUpper());
        sendNextSessionScan();
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
        for (QWidget *control : requestControls) control->setEnabled(true);
        connectionStatus->setText(tr("Session active"));
        if (testerPresentCheck->isChecked()) testerTimer.start();
        if (pollingCheck->isChecked()) pollingTimer.start(pollIntervalSpin->value());
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
    if (requestContext == ContextEcuScan)
    {
        QTimer::singleShot(ecuScanDelaySpin->value(), this, &UDSWorkbenchWindow::sendNextEcuProbe);
    }
    else if (requestContext == ContextSessionScan)
    {
        sendNextSessionScan();
    }
    else if (requestContext == ContextDidScan)
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
            for (QWidget *control : requestControls) control->setEnabled(false);
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
    QString addressPreset = settings.value("UDSWorkbench/AddressPreset").toString();
    if (addressPreset.isEmpty())
    {
        bool requestOk = false;
        const uint32_t request = parseNumber(requestIdEdit->text(), &requestOk);
        if (requestOk && request == 0x7DF) addressPreset = QStringLiteral("11functional");
        else if (requestOk && request > 0x7FF) addressPreset = QStringLiteral("29fixed");
        else if (requestOk && request == 0x7E0) addressPreset = QStringLiteral("11physical");
        else addressPreset = QStringLiteral("custom");
    }
    addressPresetCombo->setCurrentIndex(
        qMax(0, addressPresetCombo->findData(addressPreset)));
    responseOffsetEdit->setText(settings.value("UDSWorkbench/ResponseOffset", "0x8").toString());
    QString responseMode = settings.value("UDSWorkbench/ResponseMode").toString();
    if (responseMode.isEmpty())
    {
        bool requestOk = false, responseOk = false;
        const uint32_t request = parseNumber(requestIdEdit->text(), &requestOk);
        const uint32_t response = parseNumber(responseIdEdit->text(), &responseOk);
        responseMode = requestOk && responseOk && quint64(request) + 8 == response
            ? QStringLiteral("offset8") : QStringLiteral("id");
    }
    responseAddressModeCombo->setCurrentIndex(
        qMax(0, responseAddressModeCombo->findData(responseMode)));
    updateResponseIdFromMode();
    const int safetyIndex = safetyModeCombo->findData(
        settings.value("UDSWorkbench/SafetyMode", "read").toString());
    safetyModeCombo->setCurrentIndex(qMax(0, safetyIndex));
    p2TimeoutSpin->setValue(settings.value("UDSWorkbench/P2Timeout", 1500).toInt());
    p2StarTimeoutSpin->setValue(settings.value("UDSWorkbench/P2StarTimeout", 5000).toInt());
    flowBlockSizeSpin->setValue(settings.value("UDSWorkbench/FlowBlockSize", 0).toInt());
    flowStMinSpin->setValue(settings.value("UDSWorkbench/FlowStMin", 3).toInt());
    scanTimeoutSpin->setValue(settings.value("UDSWorkbench/DidScanTimeout", 300).toInt());
    normalResponseTimeout = p2TimeoutSpin->value();
    udsHandler->setFlowControlParameters(flowBlockSizeSpin->value(), flowStMinSpin->value());
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
    settings.setValue("UDSWorkbench/AddressPreset", addressPresetCombo->currentData().toString());
    settings.setValue("UDSWorkbench/RequestId", requestIdEdit->text());
    settings.setValue("UDSWorkbench/ResponseId", responseIdEdit->text());
    settings.setValue("UDSWorkbench/ResponseMode", responseAddressModeCombo->currentData().toString());
    settings.setValue("UDSWorkbench/ResponseOffset", responseOffsetEdit->text());
    settings.setValue("UDSWorkbench/SafetyMode", safetyModeCombo->currentData().toString());
    settings.setValue("UDSWorkbench/P2Timeout", p2TimeoutSpin->value());
    settings.setValue("UDSWorkbench/P2StarTimeout", p2StarTimeoutSpin->value());
    settings.setValue("UDSWorkbench/FlowBlockSize", flowBlockSizeSpin->value());
    settings.setValue("UDSWorkbench/FlowStMin", flowStMinSpin->value());
    settings.setValue("UDSWorkbench/DidScanTimeout", scanTimeoutSpin->value());
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
