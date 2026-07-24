#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "can_structs.h"
#include <QDateTime>
#include <QFileDialog>
#include <QtSerialPort/QSerialPortInfo>
#include "connections/canconmanager.h"
#include "connections/connectionwindow.h"
#include "helpwindow.h"
#include "utility.h"
#include "filterutility.h"
#include "re/aiactionregistry.h"
#include "re/aichattranscript.h"

#include <QClipboard>
#include <QSignalBlocker>
#include <QShortcut>
#include <QDockWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QInputDialog>
#include <QMessageBox>
#include <QDialogButtonBox>
#include <QTableWidget>
#include <QTabWidget>
#include <QPlainTextEdit>
#include <QCheckBox>
#include <QHeaderView>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QTimer>

/*
Some notes on things I'd like to put into the program but haven't put on github (yet)

Allow scripts to read/write signals from DBC files
allow scripts to load DBC files in support of the script - maybe the graphing system too.
*/

QString MainWindow::loadedFileName = "";
MainWindow *MainWindow::selfRef = nullptr;

MainWindow *MainWindow::getReference()
{
    return selfRef;
}

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    setupWorkspaceTabs();
    setupPayloadDock();
    populatePayloadDisplayCombo();
    QShortcut *helpShortcut = new QShortcut(QKeySequence::HelpContents, this);
    helpShortcut->setContext(Qt::WindowShortcut);
    connect(helpShortcut, &QShortcut::activated, this, [this]() {
        HelpWindow::getRef()->showHelp(currentHelpPage());
    });
#if QT_VERSION < QT_VERSION_CHECK( 6, 0, 0 )
    qRegisterMetaTypeStreamOperators<QVector<QString>>();
    qRegisterMetaTypeStreamOperators<QVector<int>>();
#endif

    useHex = true;
    useColorsByCanId = false;
    selfRef = this;

    this->setWindowTitle("Savvy CAN V" + QString::number(VERSION) + " [Built " + QString(__DATE__) +"]");

    model = new CANFrameModel(this); // set parent to mainwindow to prevent canframemodel to change thread (might be done by setModel but just in case)

    proxyModel = new QSortFilterProxyModel(this);
    proxyModel->setSourceModel(model);

    ui->canFramesView->setModel(proxyModel);

    settingsDialog = new MainSettingsDialog(); //instantiate the settings dialog so it can initialize settings if this is the first run or the config file was deleted.
    settingsDialog->updateSettings(); //write out all the settings. If this is the first run it'll write defaults out.

    readSettings();

    QHeaderView *verticalHeader = ui->canFramesView->verticalHeader();
    verticalHeader->setSectionResizeMode(QHeaderView::Fixed);
    QSettings settings;
    int fontSize = settings.value("Main/FontSize", 9).toUInt();
    QFont sysFont;
    if(settings.value("Main/FontFixedWidth", false).toBool())
        sysFont = QFontDatabase::systemFont(QFontDatabase::FixedFont); //get default fixed width font
    else
        sysFont = QFont();  //get default font
    sysFont.setPointSize(fontSize);
    verticalHeader->setDefaultSectionSize(sysFont.pixelSize());
    verticalHeader->setFont(QFont());
    ui->canFramesView->setFont(sysFont);

    QHeaderView *HorzHdr = ui->canFramesView->horizontalHeader();
    HorzHdr->setFont(QFont());
    HorzHdr->setStretchLastSection(true); //causes the data column to automatically fill the tableview
    connect(HorzHdr, SIGNAL(sectionClicked(int)), this, SLOT(headerClicked(int)));

    lastGraphingWindow = nullptr;
    frameInfoWindow = nullptr;
    playbackWindow = nullptr;
    flowViewWindow = nullptr;
    frameSenderWindow = nullptr;
    dbcMainEditor = nullptr;
    comparatorWindow = nullptr;
    settingsDialog = nullptr;
    udsFirmwareUploaderWindow = nullptr;
    discreteStateWindow = nullptr;
    connectionWindow = nullptr;
    rangeWindow = nullptr;
    dbcFileWindow = nullptr;
    fuzzingWindow = nullptr;
    udsScanWindow = nullptr;
    udsWorkbenchWindow = nullptr;
    obd2WorkbenchWindow = nullptr;
    canopenWorkbenchWindow = nullptr;
    busDiagnosticsWindow = nullptr;
    aiWorkbenchWindow = nullptr;
    aiAssistantDock = nullptr;
    motorctrlConfigWindow = nullptr;
    isoWindow = nullptr;
    snifferWindow = nullptr;
    bisectWindow = nullptr;
    signalViewerWindow = nullptr;
    temporalGraphWindow = nullptr;
    dbcComparatorWindow = nullptr;
    canBridgeWindow = nullptr;
    udsWorkbenchWindow = new UDSWorkbenchWindow(this);
    udsWorkbenchWindow->setWindowFlags(Qt::Widget);
    udsWorkbenchWindow->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    workspaceTabs->addTab(udsWorkbenchWindow, tr("UDS Workbench"));
    udsWorkbenchWindow->setProperty("helpPage", QStringLiteral("uds_workbench.md"));
    obd2WorkbenchWindow = new OBD2WorkbenchWindow(this);
    obd2WorkbenchWindow->setWindowFlags(Qt::Widget);
    obd2WorkbenchWindow->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    connect(obd2WorkbenchWindow, &OBD2WorkbenchWindow::tripPlaybackPositionChanged,
            this, &MainWindow::syncTripPlayback);
    workspaceTabs->addTab(obd2WorkbenchWindow, tr("OBD-II Workbench"));
    obd2WorkbenchWindow->setProperty("helpPage", QStringLiteral("obd2_workbench.md"));
    canopenWorkbenchWindow = new CANopenWorkbenchWindow(this);
    canopenWorkbenchWindow->setWindowFlags(Qt::Widget);
    canopenWorkbenchWindow->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    workspaceTabs->addTab(canopenWorkbenchWindow, tr("CANopen Workbench"));
    canopenWorkbenchWindow->setProperty("helpPage", QStringLiteral("canopen_workbench.md"));
    busDiagnosticsWindow = new BusDiagnosticsWindow(this);
    busDiagnosticsWindow->setWindowFlags(Qt::Widget);
    busDiagnosticsWindow->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    workspaceTabs->addTab(busDiagnosticsWindow, tr("Bus Diagnostics"));
    busDiagnosticsWindow->setProperty("helpPage", QStringLiteral("bus_diagnostics.md"));
    aiWorkbenchWindow = new AIWorkbenchWindow(model->getListReference(), this);
    aiWorkbenchWindow->setApplicationContextProvider([this]() { return aiApplicationContext(); });
    aiWorkbenchWindow->setWindowFlags(Qt::Widget);
    aiWorkbenchWindow->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    aiWorkbenchWindow->setProperty("helpPage", QStringLiteral("ai_workbench.md"));
    workspaceTabs->addTab(aiWorkbenchWindow, tr("AI Workbench"));

    aiAssistantDock = new QDockWidget(tr("AI Chat"), this);
    aiAssistantDock->setObjectName(QStringLiteral("aiAssistantDock"));
    aiAssistantDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    aiAssistantDock->setFeatures(QDockWidget::DockWidgetMovable |
                                 QDockWidget::DockWidgetFloatable |
                                 QDockWidget::DockWidgetClosable);
    QWidget *chatPanel = new QWidget(aiAssistantDock);
    chatPanel->setProperty("helpPage", QStringLiteral("ai_workbench.md"));
    QVBoxLayout *chatLayout = new QVBoxLayout(chatPanel);
    chatLayout->setContentsMargins(6, 6, 6, 6);
    QLabel *chatStatus = new QLabel(tr("Checking installed models..."), chatPanel);
    AIChatTranscript *chatTranscript = new AIChatTranscript(chatPanel);
    QPlainTextEdit *chatPrompt = new QPlainTextEdit(chatPanel);
    chatPrompt->setPlaceholderText(tr("Ask the local model..."));
    chatPrompt->setMaximumHeight(82);
    QCheckBox *chatCapture = new QCheckBox(tr("Include capture snapshot"), chatPanel);
    QHBoxLayout *chatButtons = new QHBoxLayout;
    QPushButton *openWorkbench = new QPushButton(tr("Workbench"), chatPanel);
    QPushButton *clearChat = new QPushButton(tr("Clear"), chatPanel);
    QPushButton *stopChat = new QPushButton(tr("Stop"), chatPanel);
    QPushButton *sendChat = new QPushButton(tr("Send"), chatPanel);
    sendChat->setEnabled(false);
    stopChat->setEnabled(false);
    chatButtons->addWidget(openWorkbench);
    chatButtons->addStretch();
    chatButtons->addWidget(clearChat);
    chatButtons->addWidget(stopChat);
    chatButtons->addWidget(sendChat);
    chatLayout->addWidget(chatStatus);
    chatLayout->addWidget(chatTranscript, 1);
    chatLayout->addWidget(chatPrompt);
    chatLayout->addWidget(chatCapture);
    chatLayout->addLayout(chatButtons);
    aiAssistantDock->setMinimumWidth(280);
    aiAssistantDock->setWidget(chatPanel);
    addDockWidget(Qt::RightDockWidgetArea, aiAssistantDock);
    resizeDocks({aiAssistantDock}, {340}, Qt::Horizontal);
    ui->menu_RE_Tools->addAction(aiAssistantDock->toggleViewAction());
    connect(sendChat, &QPushButton::clicked, this,
            [this, chatPrompt, chatCapture]() {
        const QString question = chatPrompt->toPlainText().trimmed();
        if (question.isEmpty()) return;
        chatPrompt->clear();
        aiWorkbenchWindow->submitChat(question, chatCapture->isChecked());
    });
    chatPrompt->setProperty("aiChatSendButton", QVariant::fromValue<QObject *>(sendChat));
    chatPrompt->installEventFilter(this);
    connect(clearChat, &QPushButton::clicked, aiWorkbenchWindow, &AIWorkbenchWindow::clearChat);
    connect(stopChat, &QPushButton::clicked, aiWorkbenchWindow, &AIWorkbenchWindow::stopRequest);
    connect(openWorkbench, &QPushButton::clicked, this, &MainWindow::showAIWorkbenchWindow);
    connect(aiWorkbenchWindow, &AIWorkbenchWindow::chatLineAdded, this,
            [chatTranscript](const QString &speaker, const QString &text) {
        chatTranscript->addMessage(speaker, text);
    });
    connect(aiWorkbenchWindow, &AIWorkbenchWindow::chatCleared,
            chatTranscript, &AIChatTranscript::clearMessages);
    connect(aiWorkbenchWindow, &AIWorkbenchWindow::chatAvailabilityChanged, this,
            [chatStatus, sendChat](bool available, const QString &status) {
        chatStatus->setText(status);
        sendChat->setProperty("modelAvailable", available);
        sendChat->setEnabled(available);
    });
    connect(aiWorkbenchWindow, &AIWorkbenchWindow::chatBusyChanged, this,
            [sendChat, stopChat](bool busy) {
        sendChat->setEnabled(!busy && sendChat->property("modelAvailable").toBool());
        stopChat->setEnabled(busy);
    });
    connect(aiWorkbenchWindow, &AIWorkbenchWindow::runtimeStateChanged,
            &lbAIStatus, &QLabel::setText);
    connect(aiWorkbenchWindow, &AIWorkbenchWindow::actionsProposed,
            this, &MainWindow::handleAIActions);
    connect(aiWorkbenchWindow, &AIWorkbenchWindow::emergencyStopRequested, this, [this]() {
        for (QTimer *timer : findChildren<QTimer *>(QStringLiteral("aiFrameLoop")))
        {
            timer->stop();
            timer->deleteLater();
        }
        for (int row = 0; row < ui->tableSimpleSender->rowCount(); ++row)
            if (ui->tableSimpleSender->item(row, SC_COL_EN))
                ui->tableSimpleSender->item(row, SC_COL_EN)->setCheckState(Qt::Unchecked);
        if (frameSenderWindow) frameSenderWindow->disableAllRows();
    });
    workspaceTabs->setCurrentWidget(traceWorkspace);
    dbcHandler = DBCHandler::getReference();
    bDirty = false;
    inhibitFilterUpdate = false;
    rxFrames = 0;
    framesPerSec = 0;
    continuousLogging = false;
    continuousLogFlushCounter = 0;

    //handlers for all menu entries
    connect(ui->actionSetup, SIGNAL(triggered(bool)), SLOT(showConnectionSettingsWindow()));
    connect(ui->actionOpen_Log_File, &QAction::triggered, this, &MainWindow::handleLoadFile);
    connect(ui->actionGraph_Dta, &QAction::triggered, this, &MainWindow::showGraphingWindow);
    connect(ui->actionFrame_Data_Analysis, &QAction::triggered, this, &MainWindow::showFrameDataAnalysis);
    connect(ui->actionSave_Log_File, &QAction::triggered, this, &MainWindow::handleSaveFile);
    connect(ui->actionSave_Filtered_Log_File, &QAction::triggered, this, &MainWindow::handleSaveFilteredFile);
    connect(ui->actionLoad_Filter_Definition, &QAction::triggered, this, &MainWindow::handleLoadFilters);
    connect(ui->actionSave_Filter_Definition, &QAction::triggered, this, &MainWindow::handleSaveFilters);
    connect(ui->action_Playback, &QAction::triggered, this, &MainWindow::showPlaybackWindow);
    connect(ui->actionFlow_View, &QAction::triggered, this, &MainWindow::showFlowViewWindow);
    connect(ui->action_Custom, &QAction::triggered, this, &MainWindow::showFrameSenderWindow);
    connect(ui->actionExit_Application, &QAction::triggered, this, &MainWindow::exitApp);
    connect(ui->actionFuzzy_Scope, &QAction::triggered, this, &MainWindow::showFuzzyScopeWindow);
    connect(ui->actionRange_State_2, &QAction::triggered, this, &MainWindow::showRangeWindow);
    connect(ui->actionSave_Decoded_Frames, &QAction::triggered, this, &MainWindow::handleSaveDecoded);
    connect(ui->actionSave_Decoded_Frames_CSV, &QAction::triggered, this, &MainWindow::handleSaveDecodedCsv);
    connect(ui->actionSingle_Multi_State_2, &QAction::triggered, this, &MainWindow::showSingleMultiWindow);
    connect(ui->actionFile_Comparison, &QAction::triggered, this, &MainWindow::showComparisonWindow);
    connect(ui->actionDBC_Comparison, &QAction::triggered, this, &MainWindow::showDBCComparisonWindow);
    connect(ui->actionScripting_INterface, &QAction::triggered, this, &MainWindow::showScriptingWindow);
    connect(ui->actionPreferences, &QAction::triggered, this, &MainWindow::showSettingsDialog);
    connect(ui->actionUDS_Firmware_Update, &QAction::triggered, this, &MainWindow::showUDSFirmwareUploaderWindow);
    connect(ui->actionDBC_File_Manager, &QAction::triggered, this, &MainWindow::showDBCFileWindow);
    connect(ui->actionFuzzing, &QAction::triggered, this, &MainWindow::showFuzzingWindow);
    connect(ui->actionUDS_Scanner, &QAction::triggered, this, &MainWindow::showUDSScanWindow);
    QAction *udsWorkbenchAction = new QAction(tr("UDS Workbench"), this);
    ui->menuSend_Frames->insertAction(ui->actionUDS_Scanner, udsWorkbenchAction);
    connect(udsWorkbenchAction, &QAction::triggered, this, &MainWindow::showUDSWorkbenchWindow);
    QAction *obd2WorkbenchAction = new QAction(tr("OBD-II Workbench"), this);
    ui->menuSend_Frames->insertAction(ui->actionUDS_Scanner, obd2WorkbenchAction);
    connect(obd2WorkbenchAction, &QAction::triggered, this, &MainWindow::showOBD2WorkbenchWindow);
    QAction *canopenWorkbenchAction = new QAction(tr("CANopen Workbench"), this);
    ui->menuSend_Frames->insertAction(ui->actionUDS_Scanner, canopenWorkbenchAction);
    connect(canopenWorkbenchAction, &QAction::triggered, this, &MainWindow::showCANopenWorkbenchWindow);
    QAction *busDiagnosticsAction = new QAction(tr("Bus Diagnostics"), this);
    ui->menuSend_Frames->insertAction(ui->actionUDS_Scanner, busDiagnosticsAction);
    connect(busDiagnosticsAction, &QAction::triggered, this, &MainWindow::showBusDiagnosticsWindow);
    QAction *aiWorkbenchAction = new QAction(tr("AI Analysis Workbench"), this);
    ui->menuSend_Frames->insertAction(ui->actionUDS_Scanner, aiWorkbenchAction);
    connect(aiWorkbenchAction, &QAction::triggered, this, &MainWindow::showAIWorkbenchWindow);
    connect(ui->actionISO_TP_Decoder, &QAction::triggered, this, &MainWindow::showISOInterpreterWindow);
    connect(ui->actionSniffer, &QAction::triggered, this, &MainWindow::showSnifferWindow);
    connect(ui->actionMotorControlConfig, &QAction::triggered, this, &MainWindow::showMCConfigWindow);
    connect(ui->actionCapture_Bisector, &QAction::triggered, this, &MainWindow::showBisectWindow);
    connect(ui->actionSignal_Viewer, &QAction::triggered, this, &MainWindow::showSignalViewer);
    connect(ui->actionSave_Continuous_Logfile, &QAction::triggered, this, &MainWindow::handleContinousLogging);
    connect(ui->actionTemporal_Graph, &QAction::triggered, this, &MainWindow::showTemporalGraphWindow);
    connect(ui->actionCAN_Bridge, &QAction::triggered, this, &MainWindow::showCANBridgeWindow);

    //handlers fror interactions with the main can frame view table
    connect(ui->canFramesView, &QAbstractItemView::clicked, this, &MainWindow::gridClicked);
    connect(ui->canFramesView, &QAbstractItemView::doubleClicked, this, &MainWindow::gridDoubleClicked);
    ui->canFramesView->setContextMenuPolicy(Qt::CustomContextMenu);

    copyAct = new QAction(tr("Copy"), this);
    copyAct->setShortcut(QKeySequence::Copy);
    connect(copyAct, &QAction::triggered, this, &MainWindow::copyFromTable);
    ui->canFramesView->addAction(copyAct);
    connect(ui->canFramesView, &QAbstractItemView::customContextMenuRequested, this, &MainWindow::gridContextMenuRequest);

    connect(model, &CANFrameModel::updatedFiltersList, this, &MainWindow::updateFilterList);
    connect(CANConManager::getInstance(), &CANConManager::framesReceived, model, &CANFrameModel::addFrames);
    //new implementation for continuous logging
    connect(CANConManager::getInstance(), &CANConManager::framesReceived, this, &MainWindow::logReceivedFrame);

    connect(ui->cbInterpret, &QAbstractButton::toggled, this, &MainWindow::interpretToggled);
    connect(ui->cbOverwrite, &QAbstractButton::toggled, this, &MainWindow::overwriteToggled);
    connect(ui->cbPersistentFilters, &QAbstractButton::toggled, this, &MainWindow::presistentFiltersToggled);
    connect(ui->listFilters, &QListWidget::itemChanged, this, &MainWindow::filterListItemChanged);
    connect(ui->listBusFilters, &QListWidget::itemChanged, this, &MainWindow::busFilterListItemChanged);

    connect(ui->btnCaptureToggle, &QAbstractButton::clicked, this, &MainWindow::toggleCapture);
    connect(ui->btnClearFrames, &QAbstractButton::clicked, this, &MainWindow::clearFrames);
    connect(ui->btnNormalize, &QAbstractButton::clicked, this, &MainWindow::normalizeTiming);
    connect(ui->btnFilterAll, &QAbstractButton::clicked, this, &MainWindow::filterSetAll);
    connect(ui->btnFilterNone, &QAbstractButton::clicked, this, &MainWindow::filterClearAll);
    connect(ui->btnExpandAll, &QAbstractButton::clicked, this, &MainWindow::expandAllRows);
    connect(ui->btnCollapseAll, &QAbstractButton::clicked, this, &MainWindow::collapseAllRows);
    connect(ui->comboPayloadDisplay, SIGNAL(currentIndexChanged(int)), this, SLOT(payloadDisplayChanged()));
    connect(ui->linePayloadFormat, SIGNAL(textChanged(QString)), this, SLOT(payloadDisplayChanged()));
    connect(ui->btnApplyPayloadDisplay, &QAbstractButton::clicked, this, &MainWindow::applyPayloadDisplay);
    connect(ui->cbShowRawPayload, &QAbstractButton::toggled, this, &MainWindow::applyPayloadDisplay);
    connect(ui->comboRecentPayloadFormats, SIGNAL(currentIndexChanged(int)), this, SLOT(recentPayloadFormatSelected(int)));
    connect(ui->canFramesView->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, [this](const QModelIndex &, const QModelIndex &) { updatePayloadPreview(); });

    connect(ui->tableSimpleSender, SIGNAL(cellChanged(int,int)), this, SLOT(onSenderCellChanged(int,int)));

    lbStatusConnected.setText(tr("Connected to 0 buses"));
    lbAIStatus.setText(tr("AI: stopped"));
    lbAIStatus.setAlignment(Qt::AlignCenter);
    lbAIStatus.setToolTip(tr("Local AI runtime state. The model can unload after its idle timeout while the runtime remains ready."));
    lbHelp.setText(tr("Press F1 on any screen for help"));
    lbHelp.setAlignment(Qt::AlignCenter);
    QFont boldFont;
    boldFont.setBold(true);
    lbHelp.setFont(boldFont);
    updateFileStatus();
    //lbStatusDatabase.setText(tr("No DBC database loaded"));
    ui->statusBar->insertWidget(0, &lbStatusConnected, 1);
    ui->statusBar->insertWidget(1, &lbStatusFilename, 1);
    ui->statusBar->insertWidget(2, &lbAIStatus);
    ui->statusBar->insertWidget(3, &lbHelp, 1);
    //ui->statusBar->addWidget(&lbStatusDatabase);
    ui->lblRemoteConn->setVisible(false);
    ui->lineRemoteKey->setVisible(false);

    ui->lbFPS->setText("0");
    ui->lbNumFrames->setText("0");

    // Prevent annoying accidental horizontal scrolling when filter list is populated with long interpreted message names
    ui->listFilters->horizontalScrollBar()->setEnabled(false);

    connect(&updateTimer, &QTimer::timeout, this, &MainWindow::tickGUIUpdate);
    updateTimer.setInterval(250);
    updateTimer.start();

    elapsedTime = new QElapsedTimer;
    elapsedTime->start();

    isConnected = false;
    allowCapture = true;

    //create a temporary frame to be able to capture the correct
    //default height of an item in the table. Need to do this in case
    //of scaling or font differences between different computers.
    CANFrame temp;
    temp.bus = 0;
    temp.setFrameId(0x100);
    temp.isReceived = true;
    temp.setTimeStamp(QCanBusFrame::TimeStamp(0, 100000000));
    model->addFrame(temp, true);
    ui->canFramesView->resizeRowToContents(0);      // Resize the row to fit the contents so we get a proper height value
    qApp->processEvents();
    tickGUIUpdate(); //force a GUI refresh so that the row exists to measure
    normalRowHeight = ui->canFramesView->rowHeight(0);
    if (normalRowHeight == 0) normalRowHeight = 30; //should not be necessary but provides a sane number if something stupid happened.
    qDebug() << "normal row height = " << normalRowHeight;
    model->clearFrames();

    ui->canFramesView->verticalHeader()->setDefaultSectionSize(normalRowHeight);    // Set the default height for all rows to the height that was calculated

    //connect(CANConManager::getInstance(), CANConManager::connectionStatusUpdated, this, MainWindow::connectionStatusUpdated);
    connect(CANConManager::getInstance(), SIGNAL(connectionStatusUpdated(int)), this, SLOT(connectionStatusUpdated(int)));

    //Automatically create the connection window so it can be updated even if we never opened it.
    connectionWindow = new ConnectionWindow();
    connect(this, SIGNAL(suspendCapturing(bool)), connectionWindow, SLOT(setSuspendAll(bool)));

    //these either are unfinished/not working or are not for general use. But,they exist
    //so if you want to enable them and play with them then go for it.
    //ui->actionFirmware_Update->setVisible(false);
    ui->actionMotorControlConfig->setVisible(false);
    ui->actionSingle_Multi_State_2->setVisible(false);

    QStringList headers;
    headers << "En" << "Bus" << "ID" << "Ext" << "Rem" << "Data"
            << "Interval" << "Count";
    ui->tableSimpleSender->setColumnCount(8);
    ui->tableSimpleSender->setColumnWidth(SIMP_COL::SC_COL_EN, 70);
    ui->tableSimpleSender->setColumnWidth(SIMP_COL::SC_COL_BUS, 70);
    ui->tableSimpleSender->setColumnWidth(SIMP_COL::SC_COL_ID, 70);
    ui->tableSimpleSender->setColumnWidth(SIMP_COL::SC_COL_EXT, 70);
    ui->tableSimpleSender->setColumnWidth(SIMP_COL::SC_COL_REM, 70);
    ui->tableSimpleSender->setColumnWidth(SIMP_COL::SC_COL_DATA, 300);
    ui->tableSimpleSender->setColumnWidth(SIMP_COL::SC_COL_INTERVAL, 100);
    ui->tableSimpleSender->setColumnWidth(SIMP_COL::SC_COL_COUNT, 100);
    ui->tableSimpleSender->setHorizontalHeaderLabels(headers);

    createSenderRow();

    frameSender = new FrameSenderObject(model->getListReference());

    frameSender->initialize(); //creates the thread and sets things up
    frameSender->startSending(); //start the timer in the object so enabled things can send

    installEventFilter(this);
}

void MainWindow::setupWorkspaceTabs()
{
    traceWorkspace = takeCentralWidget();
    traceWorkspace->setProperty("helpPage", QStringLiteral("mainscreen.md"));
    workspaceTabs = new QTabWidget(this);
    workspaceTabs->setDocumentMode(true);
    workspaceTabs->setMovable(true);
    workspaceTabs->setTabsClosable(false);
    workspaceTabs->addTab(traceWorkspace, tr("CAN Trace"));
    setCentralWidget(workspaceTabs);
}

QString MainWindow::currentHelpPage() const
{
    QWidget *widget = QApplication::focusWidget();
    while (widget && widget != this)
    {
        const QString page = widget->property("helpPage").toString();
        if (!page.isEmpty()) return page;
        widget = widget->parentWidget();
    }
    if (workspaceTabs && workspaceTabs->currentWidget())
    {
        const QString page = workspaceTabs->currentWidget()->property("helpPage").toString();
        if (!page.isEmpty()) return page;
    }
    return QStringLiteral("mainscreen.md");
}

void MainWindow::activateWorkspace(QWidget *page, const QString &title)
{
    int index = workspaceTabs->indexOf(page);
    if (index < 0) index = workspaceTabs->addTab(page, title);
    workspaceTabs->setCurrentIndex(index);
    page->show();
    page->setFocus();
}

QJsonObject MainWindow::aiApplicationContext() const
{
    QJsonObject context;
    context.insert(QStringLiteral("active_workspace"),
                   workspaceTabs ? workspaceTabs->tabText(workspaceTabs->currentIndex()) : QString());
    context.insert(QStringLiteral("capture_running"), allowCapture);
    context.insert(QStringLiteral("frame_count"), model ? model->rowCount() : 0);
    context.insert(QStringLiteral("overwrite_mode"), ui->cbOverwrite->isChecked());
    context.insert(QStringLiteral("dbc_interpretation"), ui->cbInterpret->isChecked());
    context.insert(QStringLiteral("payload_mode"), ui->comboPayloadDisplay->currentData().toString());
    context.insert(QStringLiteral("payload_format"), ui->linePayloadFormat->text());
    context.insert(QStringLiteral("enabled_id_filters"), ui->listFilters->count());
    context.insert(QStringLiteral("connected_buses"), CANConManager::getInstance()->getNumBuses());
    QJsonArray connections;
    for (CANConnection *connection : CANConManager::getInstance()->getConnections())
    {
        QJsonArray buses;
        for (int busIndex = 0; busIndex < connection->getNumBuses(); ++busIndex)
        {
            CANBus bus;
            if (connection->getBusSettings(busIndex, bus))
                buses.append(QJsonObject{
                    {QStringLiteral("index"), busIndex},
                    {QStringLiteral("speed"), bus.getSpeed()},
                    {QStringLiteral("can_fd"), bus.isCanFD()},
                    {QStringLiteral("data_rate"), bus.getDataRate()}
                });
        }
        connections.append(QJsonObject{
            {QStringLiteral("port"), connection->getPort()},
            {QStringLiteral("driver"), connection->getDriver()},
            {QStringLiteral("type"), int(connection->getType())},
            {QStringLiteral("connected"), connection->getStatus() == CANCon::CONNECTED},
            {QStringLiteral("serial_speed"), connection->getSerialSpeed()},
            {QStringLiteral("buses"), buses}
        });
    }
    context.insert(QStringLiteral("connections"), connections);
    QJsonArray enabledFilters;
    if (model)
    {
        const QMap<int, bool> *filters = model->getFiltersReference();
        for (auto it = filters->cbegin(); it != filters->cend(); ++it)
            if (it.value()) enabledFilters.append(QStringLiteral("0x%1").arg(it.key(), 0, 16).toUpper());
    }
    context.insert(QStringLiteral("main_filter_ids"), enabledFilters);
    if (snifferWindow) context.insert(QStringLiteral("sniffer"), snifferWindow->aiState());

    const int row = selectedPayloadSourceRow();
    if (row >= 0)
    {
        const CANFrame frame = model->frameAtFilteredRow(row);
        context.insert(QStringLiteral("selected_frame"), QJsonObject{
            {QStringLiteral("bus"), int(frame.bus)},
            {QStringLiteral("id"), QStringLiteral("0x%1").arg(frame.frameId(), 0, 16).toUpper()},
            {QStringLiteral("extended"), frame.hasExtendedFrameFormat()},
            {QStringLiteral("payload"), QString::fromLatin1(frame.payload().toHex(' ').toUpper())}
        });
    }
    return context;
}

void MainWindow::handleAIActions(const QJsonArray &actions)
{
    if (actions.isEmpty()) return;
    QStringList summary;
    bool includesTransmission = false;
    for (const QJsonValue &value : actions)
    {
        const QJsonObject action = value.toObject();
        const QJsonObject definition =
            AIActionRegistry::definition(action.value(QStringLiteral("capability")).toString());
        const AIActionRegistry::Risk risk = AIActionRegistry::risk(definition);
        includesTransmission |= risk == AIActionRegistry::ConfirmSend
            || risk == AIActionRegistry::ArmedConfirmSend;
        summary << QStringLiteral("%1. %2")
                       .arg(summary.size() + 1)
                       .arg(definition.value(QStringLiteral("title")).toString(
                           action.value(QStringLiteral("capability")).toString()));
    }
    const int confirmationMode = aiWorkbenchWindow->transmissionConfirmationMode();
    const bool showWorkflowReview = !includesTransmission || confirmationMode != 2;
    const QString safetyText = includesTransmission && confirmationMode == 1
        ? tr("\n\nApproving this workflow also confirms all listed transmissions.")
        : tr("\n\nContinue to validation and safety checks?");
    if (showWorkflowReview && QMessageBox::question(
            this, tr("Review AI workflow"),
            tr("The assistant proposed %1 ordered action(s):\n\n%2%3")
                .arg(actions.size()).arg(summary.join(QLatin1Char('\n')), safetyText),
            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
    {
        aiWorkbenchWindow->recordActionResult(QStringLiteral("workflow"), false,
                                              tr("User rejected the proposed workflow"));
        return;
    }
    setProperty("aiBatchApproved", true);
    for (const QJsonValue &value : actions) handleAIAction(value.toObject());
    setProperty("aiBatchApproved", false);
}

void MainWindow::handleAIAction(const QJsonObject &action)
{
    const QString capability = action.value(QStringLiteral("capability")).toString();
    QString validationError;
    if (!AIActionRegistry::validate(action, &validationError))
    {
        aiWorkbenchWindow->recordActionResult(capability, false, validationError);
        return;
    }
    const QJsonObject definition = AIActionRegistry::definition(capability);
    const QJsonObject arguments = action.value(QStringLiteral("arguments")).toObject();
    if (definition.isEmpty())
    {
        aiWorkbenchWindow->recordActionResult(
            capability, false,
            tr("The model proposed an unknown capability: %1").arg(capability));
        return;
    }

    auto text = [&arguments](const QString &key, const QString &fallback = QString()) {
        const QJsonValue value = arguments.value(key);
        if (value.isString()) return value.toString();
        if (value.isDouble()) return QString::number(value.toInt());
        if (value.isBool()) return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
        return fallback;
    };
    auto number = [&text](const QString &key, int fallback = 0) {
        const QString value = text(key);
        return value.isEmpty() ? fallback : Utility::ParseStringToNum(value);
    };

    const AIActionRegistry::Risk risk = AIActionRegistry::risk(definition);
    if (risk == AIActionRegistry::ConfirmSend || risk == AIActionRegistry::ArmedConfirmSend)
    {
        QString authorizationError;
        if (!aiWorkbenchWindow->authorizeTransmit(capability, arguments, &authorizationError))
        {
            QMessageBox::warning(this, tr("AI transmission blocked"), authorizationError);
            aiWorkbenchWindow->recordActionResult(capability, false, authorizationError);
            return;
        }
        if (aiWorkbenchWindow->transmissionConfirmationMode() == 0
            && QMessageBox::warning(
                this, tr("Confirm AI transmission"),
                tr("This action can transmit CAN traffic:\n\n%1\n\n%2\n\nExecute it now?")
                    .arg(definition.value(QStringLiteral("title")).toString(),
                         QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Indented))),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        {
            aiWorkbenchWindow->recordActionResult(capability, false, tr("User declined transmission"));
            return;
        }
        const QString target = definition.value(QStringLiteral("ui")).toString();
        QString executionError;
        bool executed = false;
        if (target.contains(QStringLiteral("UDS"))) {
            showUDSWorkbenchWindow();
            executed = udsWorkbenchWindow->executeAIRequest(
                arguments.value(QStringLiteral("operation")).toString(), arguments, &executionError);
        }
        else if (target.contains(QStringLiteral("OBD"))) {
            showOBD2WorkbenchWindow();
            executed = obd2WorkbenchWindow->executeAIRequest(
                arguments.value(QStringLiteral("operation")).toString(), arguments, &executionError);
        }
        else if (target.contains(QStringLiteral("CANopen"))) {
            showCANopenWorkbenchWindow();
            const QString operation = capability == QStringLiteral("canopen.write")
                ? QStringLiteral("write") : arguments.value(QStringLiteral("operation")).toString();
            executed = canopenWorkbenchWindow->executeAIRequest(operation, arguments, &executionError);
        }
        else if (target.contains(QStringLiteral("Fuzz"))) {
            showFuzzingWindow();
            executed = fuzzingWindow->startForDuration(
                arguments.value(QStringLiteral("duration_ms")).toInt(1000), &executionError);
        }
        else if (target.contains(QStringLiteral("Frame"))) {
            if (capability == QStringLiteral("frame.send_once")
                || capability == QStringLiteral("frame.send_loop"))
            {
                QByteArray payload;
                bool payloadValid = true;
                for (QString token : arguments.value(QStringLiteral("payload")).toString()
                         .split(QLatin1Char(' '), Qt::SkipEmptyParts))
                {
                    token.remove(QStringLiteral("0x"), Qt::CaseInsensitive);
                    bool ok = false;
                    const uint value = token.toUInt(&ok, 16);
                    if (!ok || value > 0xFF) { payloadValid = false; break; }
                    payload.append(static_cast<char>(value));
                }
                const quint32 canId = Utility::ParseStringToNum(
                    arguments.value(QStringLiteral("can_id")).toVariant().toString());
                const bool extended = arguments.value(QStringLiteral("extended")).toBool(false);
                if (!payloadValid || payload.size() > 8)
                    executionError = tr("Payload must contain at most 8 hexadecimal bytes.");
                else if (canId > 0x1FFFFFFF || (!extended && canId > 0x7FF))
                    executionError = tr("CAN ID is outside the selected frame format.");
                else
                {
                    if (capability == QStringLiteral("frame.send_loop"))
                    {
                        executed = addTraceSenderLoop(
                            arguments.value(QStringLiteral("bus")).toInt(), canId, extended,
                            payload, arguments.value(QStringLiteral("count")).toInt(),
                            arguments.value(QStringLiteral("interval_ms")).toInt(),
                            &executionError);
                    }
                    else
                    {
                        CANFrame frame;
                        frame.bus = arguments.value(QStringLiteral("bus")).toInt();
                        frame.setFrameId(canId);
                        frame.setExtendedFrameFormat(extended);
                        frame.setPayload(payload);
                        executed = CANConManager::getInstance()->sendFrame(frame);
                        if (!executed)
                            executionError = tr("No connected interface serves the requested bus.");
                    }
                }
            }
            else
            {
                showFrameSenderWindow();
                executed = frameSenderWindow->enableDraftRow(
                    arguments.value(QStringLiteral("row")).toInt(-1), &executionError);
            }
        }
        else if (target.contains(QStringLiteral("Connection"))) {
            showConnectionSettingsWindow();
            executed = connectionWindow->addConnectionProfile(
                arguments.value(QStringLiteral("type")).toString(),
                arguments.value(QStringLiteral("port")).toString(),
                arguments.value(QStringLiteral("driver")).toString(),
                arguments.value(QStringLiteral("serial_speed")).toInt(115200),
                arguments.value(QStringLiteral("bus_speed")).toInt(500000),
                arguments.value(QStringLiteral("can_fd")).toBool(false),
                arguments.value(QStringLiteral("data_rate")).toInt(2000000),
                &executionError);
        }
        if (!executed)
            QMessageBox::warning(this, tr("AI action failed"), executionError);
        aiWorkbenchWindow->recordActionResult(
            capability, executed, executed ? tr("Native operation started") : executionError);
        return;
    }

    if (capability == QStringLiteral("ui.open"))
    {
        const QString target = text(QStringLiteral("target")).toLower();
        if (target == QStringLiteral("trace")) activateWorkspace(traceWorkspace, tr("CAN Trace"));
        else if (target == QStringLiteral("uds")) showUDSWorkbenchWindow();
        else if (target == QStringLiteral("obd")) showOBD2WorkbenchWindow();
        else if (target == QStringLiteral("canopen")) showCANopenWorkbenchWindow();
        else if (target == QStringLiteral("bus_diagnostics")) showBusDiagnosticsWindow();
        else if (target == QStringLiteral("ai")) showAIWorkbenchWindow();
        else if (target == QStringLiteral("fuzzing")) showFuzzingWindow();
        else if (target == QStringLiteral("frame_sender")) showFrameSenderWindow();
        else if (target == QStringLiteral("scripting")) showScriptingWindow();
        else if (target == QStringLiteral("sniffer")) showSnifferWindow();
        else if (target == QStringLiteral("graphing")) showGraphingWindow();
        else if (target == QStringLiteral("playback")) showPlaybackWindow();
        else if (target == QStringLiteral("dbc")) showDBCFileWindow();
        else if (target == QStringLiteral("connection")) showConnectionSettingsWindow();
        else if (target == QStringLiteral("isotp")) showISOInterpreterWindow();
        else if (target == QStringLiteral("bridge")) showCANBridgeWindow();
        else QMessageBox::warning(this, tr("AI action rejected"), tr("Unknown tool target: %1").arg(target));
        aiWorkbenchWindow->recordActionResult(capability, true, tr("Tool opened"));
        return;
    }

    const QString preview = QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Indented));
    if (!property("aiBatchApproved").toBool() && QMessageBox::question(this, tr("Review AI edit"),
                              tr("%1 proposes this non-transmitting edit:\n\n%2\nApply it?")
                                  .arg(definition.value(QStringLiteral("title")).toString(), preview),
                              QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
        return;

    QString error;
    bool applied = false;
    if (capability == QStringLiteral("uds.add_did") || capability == QStringLiteral("uds.read_did"))
    {
        applied = udsWorkbenchWindow->addDidRequest(
            text(QStringLiteral("name")), text(QStringLiteral("did")),
            text(QStringLiteral("format"), QStringLiteral("u8")), number(QStringLiteral("poll_ms")), &error);
        if (applied) showUDSWorkbenchWindow();
    }
    else if (capability == QStringLiteral("obd.add_pid") || capability == QStringLiteral("obd.query_pid"))
    {
        applied = obd2WorkbenchWindow->addPidRequest(
            text(QStringLiteral("name")), text(QStringLiteral("pid")),
            text(QStringLiteral("format"), QStringLiteral("auto")), &error);
        if (applied) showOBD2WorkbenchWindow();
    }
    else if (capability == QStringLiteral("obd.clear_pids"))
    {
        const int removed = obd2WorkbenchWindow->clearPidRequests();
        error = tr("Removed %1 PID request(s)").arg(removed);
        showOBD2WorkbenchWindow();
        applied = true;
    }
    else if (capability == QStringLiteral("canopen.add_object"))
    {
        applied = canopenWorkbenchWindow->addObjectDefinition(
            number(QStringLiteral("node_id"), 1), number(QStringLiteral("index")),
            number(QStringLiteral("subindex")), text(QStringLiteral("name")),
            text(QStringLiteral("data_type")), text(QStringLiteral("access"), QStringLiteral("rw")),
            text(QStringLiteral("value")), &error);
        if (applied) showCANopenWorkbenchWindow();
    }
    else if (capability == QStringLiteral("fuzz.configure"))
    {
        showFuzzingWindow();
        applied = fuzzingWindow->configureDraft(
            number(QStringLiteral("bus")), number(QStringLiteral("start_id")),
            number(QStringLiteral("end_id")), number(QStringLiteral("interval_ms"), 10),
            number(QStringLiteral("burst"), 1), number(QStringLiteral("bytes"), 8), &error);
    }
    else if (capability == QStringLiteral("frame.add_draft"))
    {
        QByteArray payload;
        bool bytesValid = true;
        for (QString token : text(QStringLiteral("payload")).split(QLatin1Char(' '), Qt::SkipEmptyParts))
        {
            token.remove(QStringLiteral("0x"), Qt::CaseInsensitive);
            bool ok = false;
            const uint value = token.toUInt(&ok, 16);
            if (!ok || value > 0xFF) { bytesValid = false; break; }
            payload.append(static_cast<char>(value));
        }
        if (!bytesValid) error = tr("Payload must contain space-separated hexadecimal bytes.");
        else
        {
            showFrameSenderWindow();
            applied = frameSenderWindow->addFrameDraft(
                number(QStringLiteral("bus")), number(QStringLiteral("can_id")),
                arguments.value(QStringLiteral("extended")).toBool(false), payload, &error);
        }
    }
    else if (capability == QStringLiteral("connection.reconnect"))
    {
        showConnectionSettingsWindow();
        applied = connectionWindow->reconnectConnection(
            text(QStringLiteral("port"), QStringLiteral("all")), &error);
    }
    else if (capability == QStringLiteral("connection.suspend")
             || capability == QStringLiteral("connection.resume"))
    {
        showConnectionSettingsWindow();
        applied = connectionWindow->setAllConnectionsSuspended(
            capability == QStringLiteral("connection.suspend"), &error);
    }
    else if (capability == QStringLiteral("script.create_draft"))
    {
        showScriptingWindow();
        scriptingWindows.last()->createDraft(text(QStringLiteral("name"), tr("AI Draft")),
                                              text(QStringLiteral("source")));
        applied = true;
    }
    else if (capability == QStringLiteral("payload.set_format"))
    {
        syncPayloadDisplayControls(QStringLiteral("custom"), text(QStringLiteral("format")));
        if (ui->btnApplyPayloadDisplay->isEnabled())
        {
            applyPayloadDisplay();
            activateWorkspace(traceWorkspace, tr("CAN Trace"));
            applied = true;
        }
        else error = ui->lblPayloadFormatError->text();
    }
    else if (capability == QStringLiteral("trace.set_options"))
    {
        if (arguments.contains(QStringLiteral("overwrite")))
            ui->cbOverwrite->setChecked(arguments.value(QStringLiteral("overwrite")).toBool());
        if (arguments.contains(QStringLiteral("interpret_dbc")))
            ui->cbInterpret->setChecked(arguments.value(QStringLiteral("interpret_dbc")).toBool());
        if (arguments.contains(QStringLiteral("show_raw_payload")))
            ui->cbShowRawPayload->setChecked(arguments.value(QStringLiteral("show_raw_payload")).toBool());
        activateWorkspace(traceWorkspace, tr("CAN Trace"));
        applied = true;
    }
    else if (capability == QStringLiteral("filter.set_id"))
    {
        const int canId = number(QStringLiteral("can_id"), -1);
        if (canId < 0 || canId > 0x1FFFFFFF) error = tr("Invalid CAN ID.");
        else {
            model->setFilterState(canId, arguments.value(QStringLiteral("enabled")).toBool());
            updateFilterList();
            activateWorkspace(traceWorkspace, tr("CAN Trace"));
            applied = true;
        }
    }
    else if (capability == QStringLiteral("graph.add"))
    {
        const int canId = number(QStringLiteral("can_id"), -1);
        const int startBit = number(QStringLiteral("start_bit"), -1);
        const int bitLength = number(QStringLiteral("bit_length"), 0);
        if (canId < 0 || canId > 0x1FFFFFFF || startBit < 0 || bitLength < 1
            || bitLength > 64 || startBit + bitLength > 512)
            error = tr("Invalid graph CAN ID or bit range.");
        else {
            showGraphingWindow();
            GraphParams params;
            params.ID = canId;
            params.bus = number(QStringLiteral("bus"), -1);
            params.startBit = startBit;
            params.numBits = bitLength;
            params.intelFormat = arguments.value(QStringLiteral("little_endian")).toBool(true);
            params.isSigned = arguments.value(QStringLiteral("signed")).toBool(false);
            params.scale = arguments.value(QStringLiteral("scale")).toDouble(1.0);
            params.bias = arguments.value(QStringLiteral("offset")).toDouble(0.0);
            params.graphName = text(QStringLiteral("name"),
                                    QStringLiteral("0x%1 bits %2:%3")
                                        .arg(canId, 0, 16).arg(startBit).arg(bitLength));
            lastGraphingWindow->createGraph(params);
            applied = true;
        }
    }
    else if (capability == QStringLiteral("dashboard.add_pid"))
    {
        applied = obd2WorkbenchWindow->addDashboardPidByNumber(
            number(QStringLiteral("pid"), -1), &error);
        if (applied) showOBD2WorkbenchWindow();
    }

    if (!applied)
        QMessageBox::warning(this, tr("AI action rejected"),
                             error.isEmpty() ? tr("This action could not be applied.") : error);
    aiWorkbenchWindow->recordActionResult(
        capability, applied, applied ? tr("Applied") :
        (error.isEmpty() ? tr("Action could not be applied") : error));
}

void MainWindow::syncTripPlayback(qint64 elapsedMs)
{
    if (!model || model->rowCount() == 0) return;
    const qint64 firstTimestamp = model->frameAtFilteredRow(0).timeStamp().microSeconds();
    const qint64 target = firstTimestamp + elapsedMs * 1000;
    int bestRow = 0;
    qint64 bestDistance = qAbs(model->frameAtFilteredRow(0).timeStamp().microSeconds() - target);
    for (int row = 1; row < model->rowCount(); ++row)
    {
        const qint64 distance = qAbs(model->frameAtFilteredRow(row).timeStamp().microSeconds() - target);
        if (distance >= bestDistance) continue;
        bestDistance = distance;
        bestRow = row;
    }
    const QModelIndex proxyIndex = proxyModel->mapFromSource(model->index(bestRow, 0));
    if (!proxyIndex.isValid()) return;
    ui->canFramesView->selectionModel()->setCurrentIndex(
        proxyIndex, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    ui->canFramesView->scrollTo(proxyIndex, QAbstractItemView::PositionAtCenter);
}

MainWindow::~MainWindow()
{
    updateTimer.stop();
    frameSender->stopSending();
    killEmAll(); //Ride the lightning
    delete ui;
    delete model;
    delete elapsedTime;
    delete dbcHandler;
}

//kill every sub window that could be open. At the moment a hard coded list
//but eventually each window should be registered and be able to be iterated.
void MainWindow::killEmAll()
{
    foreach (GraphingWindow *win, graphWindows)
    {
        killWindow(win);
    }
    killWindow(frameInfoWindow);
    killWindow(playbackWindow);
    killWindow(flowViewWindow);
    killWindow(frameSenderWindow);
    killWindow(comparatorWindow);
    killWindow(dbcMainEditor);
    killWindow(settingsDialog);
    killWindow(discreteStateWindow);
    const QList<ScriptingWindow *> windowsToClose = scriptingWindows;
    for (ScriptingWindow *window : windowsToClose)
        killWindow(window);
    scriptingWindows.clear();
    killWindow(rangeWindow);
    killWindow(dbcFileWindow);
    killWindow(fuzzingWindow);
    killWindow(udsScanWindow);
    killWindow(isoWindow);
    killWindow(snifferWindow);
    killWindow(bisectWindow);
    killWindow(udsFirmwareUploaderWindow);
    killWindow(motorctrlConfigWindow);
    killWindow(signalViewerWindow);
    killWindow(temporalGraphWindow);
    killWindow(canBridgeWindow);

    //trying to kill this window can cause a fault to happen. It's closed last just in case.
    killWindow(connectionWindow);
}

//forcefully close the window, kill it, and salt the earth
//note, for some stupid reason this function causes a seg fault
//it seems that when it runs just before the program closes it'll
//fault out when trying to close the connection window. I assume
//this could be because that window has long running threads open and doesn't
//close quickly or maybe cleanly. Investigate.
void MainWindow::killWindow(QDialog *win)
{
    if (win)
    {
        win->close();
        delete win;
        win = nullptr;
    }
}

void MainWindow::exitApp()
{
    this->close();
    QApplication::quit(); //forces the whole application to terminate when the main window is closed
}


//the close event can be trapped and ignored so put unsaved warnings in here so the user can abort the program closing if they forgot to save things.
void MainWindow::closeEvent(QCloseEvent *event)
{

    QMessageBox::StandardButton confirmDialog;

    for (int i = 0; i < dbcHandler->getFileCount(); i++)
    {
        DBCFile *file = dbcHandler->getFileByIdx(i);
        if (file->getDirtyFlag())
        {
            confirmDialog = QMessageBox::question(this, "Unsaved DBC", "DBC File:\n" + file->getFilename() + "\nAppears to have unsaved changes\nReally close without saving?", QMessageBox::Yes|QMessageBox::No);
            if (confirmDialog != QMessageBox::Yes)
            {
                event->ignore();
                return;
            }
        }
    }

    removeEventFilter(this);
    writeSettings();
    exitApp();
    event->accept();
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::KeyPress
        && obj->property("aiChatSendButton").isValid())
    {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        const bool isEnter =
            keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter;
        const Qt::KeyboardModifiers blocked =
            Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier;
        if (isEnter && !(keyEvent->modifiers() & blocked))
        {
            QObject *button = obj->property("aiChatSendButton").value<QObject *>();
            if (QPushButton *sendButton = qobject_cast<QPushButton *>(button))
                sendButton->click();
            return true;
        }
    }
    if (event->type() == QEvent::KeyRelease) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        switch (keyEvent->key())
        {
        }
        return false;
    } else {
        // standard event processing
        return QObject::eventFilter(obj, event);
    }
    return false;
}


void MainWindow::updateSettings()
{
    readUpdateableSettings();
    emit settingsUpdated();
}

void MainWindow::readSettings()
{
    QSettings settings;
    if (settings.contains("Main/PayloadDockState"))
        restoreState(settings.value("Main/PayloadDockState").toByteArray());
    if (settings.value("Main/SaveRestorePositions", false).toBool())
    {
        resize(settings.value("Main/WindowSize", QSize(800, 750)).toSize());
        move(Utility::constrainedWindowPos(settings.value("Main/WindowPos", QPoint(100, 100)).toPoint()));

        ui->canFramesView->setColumnWidth(0, settings.value("Main/TimeColumn", 150).toUInt()); //time stamp
        ui->canFramesView->setColumnWidth(1, settings.value("Main/IDColumn", 70).toUInt()); //frame ID
        ui->canFramesView->setColumnWidth(2, settings.value("Main/ExtColumn", 40).toUInt()); //extended
        ui->canFramesView->setColumnWidth(3, settings.value("Main/RemColumn", 40).toUInt()); //remote
        ui->canFramesView->setColumnWidth(4, settings.value("Main/DirColumn", 40).toUInt()); //direction
        ui->canFramesView->setColumnWidth(5, settings.value("Main/BusColumn", 40).toUInt()); //bus
        ui->canFramesView->setColumnWidth(6, settings.value("Main/LengthColumn", 40).toUInt()); //length
        ui->canFramesView->setColumnWidth(7, settings.value("Main/AsciiColumn", 50).toUInt()); //ascii
        //ui->canFramesView->setColumnWidth(8, settings.value("Main/DataColumn", 225).toUInt()); //data
    }
    if (settings.value("Main/AutoScroll", false).toBool())
    {
        ui->cbAutoScroll->setChecked(true);
    }
    int fontSize = settings.value("Main/FontSize", 9).toUInt();
    QFont newFont = QFont(ui->canFramesView->font());
    newFont.setPointSize(fontSize);
    ui->canFramesView->setFont(newFont);

    readUpdateableSettings();
}


/*
 * TODO: The way the frame timing mode is specified is DEAD STUPID. There shouldn't be three boolean values
 * for this. Instead switch it all to an ENUM or something sane.
*/
void MainWindow::readUpdateableSettings()
{
    QSettings settings;
    const QString dockPreference = settings.value("Main/PayloadDockPreference", QStringLiteral("remember")).toString();
    if (dockPreference == QStringLiteral("right") || dockPreference == QStringLiteral("bottom"))
    {
        const QSignalBlocker dockBlocker(payloadDock);
        addDockWidget(dockPreference == QStringLiteral("bottom")
                          ? Qt::BottomDockWidgetArea : Qt::RightDockWidgetArea,
                      payloadDock);
    }
    useHex = settings.value("Main/UseHex", true).toBool();
    QString payloadMode;
    if (settings.contains("Main/PayloadDisplayMode"))
        payloadMode = settings.value("Main/PayloadDisplayMode").toString();
    else
        payloadMode = settings.value("Main/UseHex", true).toBool() ? QStringLiteral("raw-hex") : QStringLiteral("raw-decimal");

    const QString customPayloadFormat = settings.value("Main/PayloadFormat", QStringLiteral("u16le")).toString();
    const QSignalBlocker rawBlocker(ui->cbShowRawPayload);
    syncPayloadDisplayControls(payloadMode, customPayloadFormat);
    ui->cbShowRawPayload->setChecked(settings.value("Main/PayloadShowRaw", false).toBool());
    model->setShowRawPayload(ui->cbShowRawPayload->isChecked());

    if (payloadMode == QStringLiteral("raw-hex"))
        model->setPayloadDisplayMode(PayloadDisplayMode::RawHex);
    else if (payloadMode == QStringLiteral("raw-decimal"))
        model->setPayloadDisplayMode(PayloadDisplayMode::RawDecimal);
    else
    {
        const QString format = payloadMode == QStringLiteral("custom")
            ? customPayloadFormat
            : payloadMode;
        if (model->setPayloadFormat(format, nullptr, payloadMode != QStringLiteral("custom")))
            model->setPayloadDisplayMode(PayloadDisplayMode::Typed);
        else
            model->setPayloadDisplayMode(PayloadDisplayMode::RawHex);
    }
    Utility::decimalMode = !useHex;

    useColorsByCanId = settings.value("Main/ColorsByCanId", false).toBool();
    model->setUseColorsByCanId(useColorsByCanId);

    bool tempBool;
    TimeStyle ts = TS_MICROS;
    tempBool = settings.value("Main/TimeSeconds", false).toBool();
    if (tempBool) ts = TS_SECONDS;
    tempBool = settings.value("Main/TimeClock", false).toBool();
    if (tempBool) ts = TS_CLOCK;
    tempBool = settings.value("Main/TimeMillis", false).toBool();
    if (tempBool) ts = TS_MILLIS;
    model->setTimeStyle(ts);

    useFiltered = settings.value("Main/UseFiltered", false).toBool();
    model->setTimeFormat(settings.value("Main/TimeFormat", "MMM-dd HH:mm:ss.zzz").toString());
    ignoreDBCColors = settings.value("Main/IgnoreDBCColors", false).toBool();
    model->setIgnoreDBCColors(ignoreDBCColors);
    int bpl = settings.value("Main/BytesPerLine", 8).toInt();
    model->setBytesPerLine(bpl);

    CSVAbsTime = settings.value("Main/CSVAbsTime", false).toBool();

    if (settings.value("Main/FilterLabeling", false).toBool())
        ui->listFilters->setMaximumWidth(250);
    else
        ui->listFilters->setMaximumWidth(175);
    applyPayloadIdAssignments();
    updateFilterList();    
    updatePayloadPreview();
}    

void MainWindow::populatePayloadDisplayCombo()
{
    ui->comboPayloadDisplay->addItem(tr("Raw hexadecimal"), QStringLiteral("raw-hex"));
    ui->comboPayloadDisplay->addItem(tr("Raw decimal"), QStringLiteral("raw-decimal"));
    ui->comboPayloadDisplay->addItem(tr("Unsigned 8-bit"), QStringLiteral("u8"));
    ui->comboPayloadDisplay->addItem(tr("Signed 8-bit"), QStringLiteral("i8"));
    ui->comboPayloadDisplay->addItem(tr("Unsigned 16-bit LE"), QStringLiteral("u16le"));
    ui->comboPayloadDisplay->addItem(tr("Unsigned 16-bit BE"), QStringLiteral("u16be"));
    ui->comboPayloadDisplay->addItem(tr("Signed 16-bit LE"), QStringLiteral("i16le"));
    ui->comboPayloadDisplay->addItem(tr("Signed 16-bit BE"), QStringLiteral("i16be"));
    ui->comboPayloadDisplay->addItem(tr("Unsigned 32-bit LE"), QStringLiteral("u32le"));
    ui->comboPayloadDisplay->addItem(tr("Unsigned 32-bit BE"), QStringLiteral("u32be"));
    ui->comboPayloadDisplay->addItem(tr("Signed 32-bit LE"), QStringLiteral("i32le"));
    ui->comboPayloadDisplay->addItem(tr("Signed 32-bit BE"), QStringLiteral("i32be"));
    ui->comboPayloadDisplay->addItem(tr("Custom format"), QStringLiteral("custom"));
}

void MainWindow::setupPayloadDock()
{
    payloadDock = new QDockWidget(tr("Payload View"), this);
    payloadDock->setObjectName(QStringLiteral("payloadViewDock"));
    payloadDock->setAllowedAreas(Qt::RightDockWidgetArea | Qt::BottomDockWidgetArea);
    payloadDock->setFeatures(QDockWidget::DockWidgetMovable |
                             QDockWidget::DockWidgetFloatable |
                             QDockWidget::DockWidgetClosable);

    QWidget *panel = new QWidget(payloadDock);
    panel->setProperty("helpPage", QStringLiteral("payload_formatter.md"));
    QBoxLayout *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->addWidget(ui->comboPayloadDisplay);
    layout->addWidget(ui->cbShowRawPayload);
    layout->addWidget(ui->linePayloadFormat);
    layout->addWidget(ui->comboRecentPayloadFormats);
    payloadProfileCombo = new QComboBox(panel);
    payloadProfileCombo->setPlaceholderText(tr("Saved profiles"));
    layout->addWidget(payloadProfileCombo);
    QHBoxLayout *profileButtons = new QHBoxLayout;
    QPushButton *saveProfile = new QPushButton(tr("Save profile"), panel);
    QPushButton *deleteProfile = new QPushButton(tr("Delete"), panel);
    QPushButton *assignProfile = new QPushButton(tr("Assign to ID"), panel);
    QPushButton *clearAssignment = new QPushButton(tr("Clear ID"), panel);
    profileButtons->addWidget(saveProfile);
    profileButtons->addWidget(deleteProfile);
    profileButtons->addWidget(assignProfile);
    profileButtons->addWidget(clearAssignment);
    layout->addLayout(profileButtons);
    QHBoxLayout *editorButtons = new QHBoxLayout;
    QPushButton *visualEditor = new QPushButton(tr("Field editor"), panel);
    QPushButton *fromDbc = new QPushButton(tr("From DBC"), panel);
    editorButtons->addWidget(visualEditor);
    editorButtons->addWidget(fromDbc);
    layout->addLayout(editorButtons);
    layout->addWidget(ui->lblPayloadFormatError);
    layout->addWidget(ui->btnApplyPayloadDisplay);
    layout->addWidget(ui->lblPayloadPreview);
    payloadDock->setWidget(panel);
    ui->labelPayloadView->hide();
    reloadPayloadProfiles();
    connect(payloadProfileCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index >= 0)
        {
            ui->comboPayloadDisplay->setCurrentIndex(ui->comboPayloadDisplay->findData(QStringLiteral("custom")));
            ui->linePayloadFormat->setText(payloadProfileCombo->currentData().toString());
        }
    });
    connect(saveProfile, &QPushButton::clicked, this, &MainWindow::savePayloadProfile);
    connect(deleteProfile, &QPushButton::clicked, this, &MainWindow::deletePayloadProfile);
    connect(assignProfile, &QPushButton::clicked, this, &MainWindow::assignPayloadProfileToSelectedId);
    connect(clearAssignment, &QPushButton::clicked, this, &MainWindow::clearPayloadProfileForSelectedId);
    connect(visualEditor, &QPushButton::clicked, this, &MainWindow::editPayloadFormatVisually);
    connect(fromDbc, &QPushButton::clicked, this, &MainWindow::createPayloadFormatFromDbc);

    QSettings settings;
    const Qt::DockWidgetArea area = static_cast<Qt::DockWidgetArea>(
        settings.value("Main/PayloadDockArea", Qt::RightDockWidgetArea).toInt());
    addDockWidget(area == Qt::BottomDockWidgetArea ? Qt::BottomDockWidgetArea : Qt::RightDockWidgetArea,
                  payloadDock);
    layout->setDirection(area == Qt::BottomDockWidgetArea
                             ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
    ui->menu_RE_Tools->addSeparator();
    ui->menu_RE_Tools->addAction(payloadDock->toggleViewAction());
    connect(payloadDock, &QDockWidget::dockLocationChanged, this, [this, layout](Qt::DockWidgetArea area) {
        layout->setDirection(area == Qt::BottomDockWidgetArea
                                 ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
        QSettings settings;
        settings.setValue("Main/PayloadDockArea", static_cast<int>(area));
        settings.setValue("Main/PayloadDockPreference", QStringLiteral("remember"));
        settings.setValue("Main/PayloadDockState", saveState());
    });
    connect(workspaceTabs, &QTabWidget::currentChanged, this, [this](int) {
        const bool traceActive = workspaceTabs->currentWidget() == traceWorkspace;
        if (!traceActive && payloadDockTraceContext)
        {
            payloadDockTraceVisible = payloadDock->isVisible();
            payloadDockTraceContext = false;
            payloadDock->hide();
            payloadDock->toggleViewAction()->setEnabled(false);
        }
        else if (traceActive && !payloadDockTraceContext)
        {
            payloadDockTraceContext = true;
            payloadDock->toggleViewAction()->setEnabled(true);
            payloadDock->setVisible(payloadDockTraceVisible);
        }
    });
}

void MainWindow::syncPayloadDisplayControls(const QString &mode, const QString &format)
{
    const QSignalBlocker comboBlocker(ui->comboPayloadDisplay);
    const QSignalBlocker formatBlocker(ui->linePayloadFormat);
    int index = ui->comboPayloadDisplay->findData(mode);
    if (index < 0)
        index = ui->comboPayloadDisplay->findData(QStringLiteral("raw-hex"));
    ui->comboPayloadDisplay->setCurrentIndex(index);
    ui->linePayloadFormat->setText(format);
    const QSignalBlocker recentBlocker(ui->comboRecentPayloadFormats);
    ui->comboRecentPayloadFormats->clear();
    ui->comboRecentPayloadFormats->addItems(QSettings().value("Main/PayloadRecentFormats").toStringList());
    ui->comboRecentPayloadFormats->setCurrentIndex(-1);
    payloadDisplayChanged();
}

void MainWindow::payloadDisplayChanged()
{
    const bool custom = ui->comboPayloadDisplay->currentData().toString() == QStringLiteral("custom");
    ui->linePayloadFormat->setVisible(custom);
    ui->comboRecentPayloadFormats->setVisible(custom && ui->comboRecentPayloadFormats->count() > 0);
    ui->lblPayloadFormatError->setVisible(custom);

    PayloadFormatter formatter;
    QString error;
    const bool valid = !custom || formatter.compile(ui->linePayloadFormat->text(), &error);
    ui->lblPayloadFormatError->setText(valid ? QString() : error);
    ui->btnApplyPayloadDisplay->setEnabled(valid);
    updatePayloadPreview();
}

void MainWindow::applyPayloadDisplay()
{
    const QString mode = ui->comboPayloadDisplay->currentData().toString();
    PayloadFormatter formatter;
    QString error;
    if (mode == QStringLiteral("custom") && !formatter.compile(ui->linePayloadFormat->text(), &error))
    {
        ui->lblPayloadFormatError->setText(error);
        return;
    }

    QSettings settings;
    settings.setValue("Main/PayloadDisplayMode", mode);
    settings.setValue("Main/PayloadShowRaw", ui->cbShowRawPayload->isChecked());
    if (mode == QStringLiteral("custom"))
    {
        settings.setValue("Main/PayloadFormat", ui->linePayloadFormat->text().simplified());
        QStringList recent = settings.value("Main/PayloadRecentFormats").toStringList();
        const QString current = ui->linePayloadFormat->text().simplified();
        recent.removeAll(current);
        recent.prepend(current);
        while (recent.size() > 10)
            recent.removeLast();
        settings.setValue("Main/PayloadRecentFormats", recent);
    }
    settings.sync();
    readUpdateableSettings();
}

void MainWindow::recentPayloadFormatSelected(int index)
{
    if (index >= 0)
        ui->linePayloadFormat->setText(ui->comboRecentPayloadFormats->itemText(index));
}

void MainWindow::reloadPayloadProfiles()
{
    if (!payloadProfileCombo)
        return;
    const QSignalBlocker blocker(payloadProfileCombo);
    payloadProfileCombo->clear();
    const QVariantMap profiles = QSettings().value("Main/PayloadProfiles").toMap();
    for (auto it = profiles.constBegin(); it != profiles.constEnd(); ++it)
        payloadProfileCombo->addItem(it.key(), it.value().toString());
    payloadProfileCombo->setCurrentIndex(-1);
}

void MainWindow::savePayloadProfile()
{
    PayloadFormatter formatter;
    QString error;
    const QString format = ui->linePayloadFormat->text().simplified();
    if (!formatter.compile(format, &error))
    {
        QMessageBox::warning(this, tr("Invalid payload format"), error);
        return;
    }
    bool accepted = false;
    const QString name = QInputDialog::getText(this, tr("Save payload profile"),
        tr("Profile name"), QLineEdit::Normal, QString(), &accepted).trimmed();
    if (!accepted || name.isEmpty())
        return;
    QSettings settings;
    QVariantMap profiles = settings.value("Main/PayloadProfiles").toMap();
    profiles.insert(name, format);
    settings.setValue("Main/PayloadProfiles", profiles);
    reloadPayloadProfiles();
    payloadProfileCombo->setCurrentIndex(payloadProfileCombo->findText(name));
}

void MainWindow::deletePayloadProfile()
{
    if (payloadProfileCombo->currentIndex() < 0)
        return;
    QSettings settings;
    QVariantMap profiles = settings.value("Main/PayloadProfiles").toMap();
    profiles.remove(payloadProfileCombo->currentText());
    settings.setValue("Main/PayloadProfiles", profiles);
    reloadPayloadProfiles();
}

int MainWindow::selectedPayloadSourceRow() const
{
    const QModelIndex index = ui->canFramesView->currentIndex();
    return index.isValid() ? proxyModel->mapToSource(index).row() : -1;
}

void MainWindow::assignPayloadProfileToSelectedId()
{
    const int row = selectedPayloadSourceRow();
    if (row < 0 || payloadProfileCombo->currentIndex() < 0)
    {
        QMessageBox::information(this, tr("Assign payload profile"),
            tr("Select a frame and a saved profile first."));
        return;
    }
    const CANFrame frame = model->frameAtFilteredRow(row);
    applyPayloadDisplay();
    QSettings settings;
    QVariantMap assignments = settings.value("Main/PayloadIdFormats").toMap();
    assignments.insert(CANFrameModel::payloadFormatKey(
        frame.bus, frame.frameId(), frame.hasExtendedFrameFormat()),
        payloadProfileCombo->currentData().toString());
    settings.setValue("Main/PayloadIdFormats", assignments);
    applyPayloadIdAssignments();
    updatePayloadPreview();
}

void MainWindow::clearPayloadProfileForSelectedId()
{
    const int row = selectedPayloadSourceRow();
    if (row < 0)
        return;
    QSettings settings;
    QVariantMap assignments = settings.value("Main/PayloadIdFormats").toMap();
    const CANFrame frame = model->frameAtFilteredRow(row);
    assignments.remove(CANFrameModel::payloadFormatKey(
        frame.bus, frame.frameId(), frame.hasExtendedFrameFormat()));
    assignments.remove(QString::number(frame.frameId()));
    settings.setValue("Main/PayloadIdFormats", assignments);
    applyPayloadIdAssignments();
    updatePayloadPreview();
}

void MainWindow::applyPayloadIdAssignments()
{
    model->clearPayloadFormatsById();
    const QVariantMap assignments = QSettings().value("Main/PayloadIdFormats").toMap();
    for (auto it = assignments.constBegin(); it != assignments.constEnd(); ++it)
    {
        const QStringList keyParts = it.key().split(':');
        if (keyParts.size() == 3)
            model->setPayloadFormatForFrame(keyParts.at(0).toInt(), keyParts.at(1).toUInt(),
                                            keyParts.at(2).toInt() != 0, it.value().toString());
        else
            model->setPayloadFormatForFrame(-1, it.key().toUInt(), false, it.value().toString());
    }
}

QString MainWindow::formatFromSelectedDbc(QStringList *warnings) const
{
    const int row = selectedPayloadSourceRow();
    if (row < 0)
        return QString();
    const CANFrame frame = model->frameAtFilteredRow(row);
    DBC_MESSAGE *message = dbcHandler ? dbcHandler->findMessage(frame) : nullptr;
    if (!message || !message->sigHandler)
        return QString();

    QStringList fields;
    for (int i = 0; i < message->sigHandler->getCount(); ++i)
    {
        DBC_SIGNAL *signal = message->sigHandler->findSignalByIdx(i);
        if (!signal)
            continue;
        if (signal->startBit % 8 != 0 ||
            (signal->signalSize != 8 && signal->signalSize != 16 &&
             signal->signalSize != 32 && signal->signalSize != 64))
        {
            if (warnings)
                warnings->append(tr("%1 is not byte-aligned or has an unsupported width").arg(signal->name));
            continue;
        }

        QString name = signal->name;
        name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_]")), QStringLiteral("_"));
        if (name.isEmpty() || name.at(0).isDigit())
            name.prepend('_');
        QString type;
        if (signal->valType == SP_FLOAT && signal->signalSize == 32)
            type = QStringLiteral("f32");
        else if (signal->valType == DP_FLOAT && signal->signalSize == 64)
            type = QStringLiteral("f64");
        else
            type = signal->valType == SIGNED_INT ? QStringLiteral("i") : QStringLiteral("u");
        if (signal->signalSize == 8 && !type.startsWith('f'))
            type += QStringLiteral("8");
        else
            type += QString::number(signal->signalSize) + (signal->intelByteOrder ? QStringLiteral("le") : QStringLiteral("be"));

        QString field = name + ':' + type + '@' + QString::number(signal->startBit / 8);
        if (signal->factor != 1.0)
            field += '*' + QString::number(signal->factor, 'g', 12);
        if (signal->bias > 0.0)
            field += '+' + QString::number(signal->bias, 'g', 12);
        else if (signal->bias < 0.0)
            field += QString::number(signal->bias, 'g', 12);
        if (!signal->unitName.isEmpty())
            field += '[' + signal->unitName + ']';
        fields.append(field);
    }
    return fields.join(' ');
}

void MainWindow::createPayloadFormatFromDbc()
{
    QStringList warnings;
    const QString format = formatFromSelectedDbc(&warnings);
    if (format.isEmpty())
    {
        QMessageBox::information(this, tr("Create format from DBC"),
            tr("The selected frame has no byte-aligned DBC signals that can be represented by the payload formatter."));
        return;
    }
    ui->comboPayloadDisplay->setCurrentIndex(ui->comboPayloadDisplay->findData(QStringLiteral("custom")));
    ui->linePayloadFormat->setText(format);
    if (!warnings.isEmpty())
        QMessageBox::information(this, tr("DBC format created"),
            tr("The compatible signals were added. Skipped signals:\n%1").arg(warnings.join('\n')));
}

void MainWindow::editPayloadFormatVisually()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Payload field editor"));
    dialog.resize(1050, 420);
    QVBoxLayout *dialogLayout = new QVBoxLayout(&dialog);
    QTableWidget *table = new QTableWidget(&dialog);
    const QStringList headers = {tr("Name"), tr("Type"), tr("Byte"), tr("Mask"), tr("Shift"),
        tr("Factor"), tr("Divisor"), tr("Bias"), tr("Unit"), tr("Precision")};
    table->setColumnCount(headers.size());
    table->setHorizontalHeaderLabels(headers);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setStretchLastSection(true);
    dialogLayout->addWidget(table);

    const QRegularExpression parser(QStringLiteral(
        "^(?:([A-Za-z_][A-Za-z0-9_]*):)?(u8|i8|[uif](?:16|32|64)(?:le|be))"
        "(?:@(\\d+))?(?:&(0[xX][0-9A-Fa-f]+|\\d+))?(?:(>>|<<)(\\d+))?"
        "(?:\\*([-+]?\\d+(?:\\.\\d+)?))?(?:/([-+]?\\d+(?:\\.\\d+)?))?"
        "(?:([+-])(\\d+(?:\\.\\d+)?))?(?:\\[([^\\]]+)\\])?(?:\\{(\\d+)\\})?$"));
    const QStringList tokens = ui->linePayloadFormat->text().simplified().split(' ', Qt::SkipEmptyParts);
    for (const QString &token : tokens)
    {
        const QRegularExpressionMatch match = parser.match(token);
        if (!match.hasMatch())
            continue;
        const int row = table->rowCount();
        table->insertRow(row);
        QString shift;
        if (!match.captured(6).isEmpty()) shift = match.captured(5) + match.captured(6);
        QString bias;
        if (!match.captured(10).isEmpty()) bias = match.captured(9) + match.captured(10);
        const QStringList values = {match.captured(1), match.captured(2), match.captured(3),
            match.captured(4), shift, match.captured(7), match.captured(8), bias,
            match.captured(11), match.captured(12)};
        for (int column = 0; column < values.size(); ++column)
            table->setItem(row, column, new QTableWidgetItem(values.at(column)));
    }

    QHBoxLayout *tools = new QHBoxLayout;
    QPushButton *add = new QPushButton(tr("Add field"), &dialog);
    QPushButton *remove = new QPushButton(tr("Remove field"), &dialog);
    QPushButton *dbc = new QPushButton(tr("Replace from DBC"), &dialog);
    tools->addWidget(add);
    tools->addWidget(remove);
    tools->addWidget(dbc);
    tools->addStretch();
    dialogLayout->addLayout(tools);
    connect(add, &QPushButton::clicked, table, [table]() {
        const int row = table->rowCount();
        table->insertRow(row);
        table->setItem(row, 1, new QTableWidgetItem(QStringLiteral("u8")));
    });
    connect(remove, &QPushButton::clicked, table, [table]() {
        if (table->currentRow() >= 0) table->removeRow(table->currentRow());
    });
    connect(dbc, &QPushButton::clicked, &dialog, [this, &dialog]() {
        const QString format = formatFromSelectedDbc();
        if (!format.isEmpty())
        {
            ui->linePayloadFormat->setText(format);
            dialog.reject();
            editPayloadFormatVisually();
        }
    });

    QDialogButtonBox *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    dialogLayout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;

    QStringList result;
    auto textAt = [table](int row, int column) {
        return table->item(row, column) ? table->item(row, column)->text().trimmed() : QString();
    };
    for (int row = 0; row < table->rowCount(); ++row)
    {
        const QString type = textAt(row, 1);
        if (type.isEmpty()) continue;
        QString field = textAt(row, 0).isEmpty() ? type : textAt(row, 0) + ':' + type;
        if (!textAt(row, 2).isEmpty()) field += '@' + textAt(row, 2);
        if (!textAt(row, 3).isEmpty()) field += '&' + textAt(row, 3);
        if (!textAt(row, 4).isEmpty()) field += textAt(row, 4);
        if (!textAt(row, 5).isEmpty()) field += '*' + textAt(row, 5);
        if (!textAt(row, 6).isEmpty()) field += '/' + textAt(row, 6);
        if (!textAt(row, 7).isEmpty())
            field += textAt(row, 7).startsWith('-') || textAt(row, 7).startsWith('+')
                ? textAt(row, 7) : '+' + textAt(row, 7);
        if (!textAt(row, 8).isEmpty()) field += '[' + textAt(row, 8) + ']';
        if (!textAt(row, 9).isEmpty()) field += '{' + textAt(row, 9) + '}';
        result.append(field);
    }
    ui->comboPayloadDisplay->setCurrentIndex(ui->comboPayloadDisplay->findData(QStringLiteral("custom")));
    ui->linePayloadFormat->setText(result.join(' '));
}

QString MainWindow::formatPayloadForControls(const QByteArray &payload, bool includeRaw) const
{
    const QString mode = ui->comboPayloadDisplay->currentData().toString();
    QString raw;
    for (const char byte : payload)
    {
        if (!raw.isEmpty()) raw.append(' ');
        raw.append(QString::number(static_cast<quint8>(byte), 16).toUpper().rightJustified(2, '0'));
    }
    if (mode == QStringLiteral("raw-hex"))
        return raw;
    if (mode == QStringLiteral("raw-decimal"))
    {
        QStringList decimal;
        for (const char byte : payload)
            decimal.append(QString::number(static_cast<quint8>(byte)));
        return decimal.join(' ');
    }

    PayloadFormatter formatter;
    const QString format = mode == QStringLiteral("custom") ? ui->linePayloadFormat->text() : mode;
    if (!formatter.compile(format, nullptr, mode != QStringLiteral("custom")))
        return QString();
    const QString decoded = formatter.format(payload);
    return includeRaw ? raw + QStringLiteral(" | ") + decoded : decoded;
}

void MainWindow::updatePayloadPreview()
{
    const QModelIndex proxyIndex = ui->canFramesView->currentIndex();
    if (!proxyIndex.isValid())
    {
        ui->lblPayloadPreview->setText(tr("Select a frame to preview"));
        return;
    }
    const int sourceRow = proxyModel->mapToSource(proxyIndex).row();
    const QByteArray payload = model->payloadAtFilteredRow(sourceRow);
    const CANFrame frame = model->frameAtFilteredRow(sourceRow);
    const QVariantMap assignments = QSettings().value("Main/PayloadIdFormats").toMap();
    const bool hasIdFormat = assignments.contains(CANFrameModel::payloadFormatKey(
                                 frame.bus, frame.frameId(), frame.hasExtendedFrameFormat())) ||
                             assignments.contains(QString::number(frame.frameId()));
    QString preview = hasIdFormat ? model->decodedPayload(frame)
                                  : formatPayloadForControls(payload, ui->cbShowRawPayload->isChecked());
    ui->lblPayloadPreview->setStyleSheet(QString());
    if (ui->comboPayloadDisplay->currentData().toString() == QStringLiteral("custom"))
    {
        PayloadFormatter formatter;
        if (formatter.compile(ui->linePayloadFormat->text()))
        {
            const QStringList warnings = formatter.validationWarnings(payload.size());
            if (!warnings.isEmpty())
            {
                preview += QStringLiteral("\n") + warnings.join(QStringLiteral("\n"));
                ui->lblPayloadPreview->setStyleSheet(QStringLiteral("color: #b45309;"));
            }
        }
    }
    ui->lblPayloadPreview->setText(preview);
}


void MainWindow::writeSettings()
{
    QSettings settings;
    settings.setValue("Main/PayloadDockState", saveState());
    settings.setValue("Main/PayloadDockArea", static_cast<int>(dockWidgetArea(payloadDock)));

    if (settings.value("Main/SaveRestorePositions", false).toBool())
    {
        settings.setValue("Main/WindowSize", size());
        settings.setValue("Main/WindowPos", pos());
        settings.setValue("Main/TimeColumn", ui->canFramesView->columnWidth(0));
        settings.setValue("Main/IDColumn", ui->canFramesView->columnWidth(1));
        settings.setValue("Main/ExtColumn", ui->canFramesView->columnWidth(2));
        settings.setValue("Main/RemColumn", ui->canFramesView->columnWidth(3));
        settings.setValue("Main/DirColumn", ui->canFramesView->columnWidth(4));
        settings.setValue("Main/BusColumn", ui->canFramesView->columnWidth(5));
        settings.setValue("Main/LengthColumn", ui->canFramesView->columnWidth(6));
        settings.setValue("Main/AsciiColumn", ui->canFramesView->columnWidth(7));
        //settings.setValue("Main/DataColumn", ui->canFramesView->columnWidth(8));
    }
}

void MainWindow::onSenderCellChanged(int row, int col)
{
    if (inhibitSenderChanged) return;
    qDebug() << "onCellChanged";
    if (row == ui->tableSimpleSender->rowCount() - 1)
    {
        createSenderRow();
    }

    processSenderCellChange(row, col);
}

void MainWindow::processSenderCellChange(int line, int col)
{
    qDebug() << "processSenderCellChange";
    FrameSendData *tempData;
    QStringList tokens;
    int tempVal;

    int numBuses = CANConManager::getInstance()->getNumBuses();
    QByteArray arr;

    tempData = frameSender->getSendRecordRef(line);

    if (!tempData)
    {
        qDebug() << "Need to set up a new entry in senders";
        FrameSendData dat;
        dat.enabled = false;
        dat.count = 0;
        dat.frameCount = 0;
        dat.bus = 0;
        frameSender->addSendRecord(dat);
        tempData = frameSender->getSendRecordRef(line);
    }

    if (!tempData)
    {
        qDebug() << "No data to modify in processSenderCellChange. This is a bug!";
        return;
    }

    switch (col)
    {
    case SIMP_COL::SC_COL_EN: //Enable check box
        if (ui->tableSimpleSender->item(line, 0)->checkState() == Qt::Checked)
        {
            tempData->enabled = true;
        }
        else tempData->enabled = false;
        qDebug() << "Setting enabled to " << tempData->enabled;
        break;
    case SIMP_COL::SC_COL_BUS: //Bus designation
        tempVal = Utility::ParseStringToNum(ui->tableSimpleSender->item(line, SIMP_COL::SC_COL_BUS)->text());
        if (tempVal < -1) tempVal = -1;
        if (tempVal >= numBuses) tempVal = numBuses - 1;
        tempData->bus = tempVal;
        qDebug() << "Setting bus to " << tempVal;
        break;
    case SIMP_COL::SC_COL_ID: //ID field
        tempVal = Utility::ParseStringToNum(ui->tableSimpleSender->item(line, SIMP_COL::SC_COL_ID)->text());
        if (tempVal < 0) tempVal = 0;
        if (tempVal > 0x7FFFFFFF) tempVal = 0x7FFFFFFF;
        tempData->setFrameId(tempVal);
        if (tempData->frameId() > 0x7FF) {
            tempData->setExtendedFrameFormat(true);
            ui->tableSimpleSender->blockSignals(true);
            ui->tableSimpleSender->item(line, ST_COLS::SENDTAB_COL_EXT)->setCheckState(Qt::Checked);
            ui->tableSimpleSender->blockSignals(false);
        }
        qDebug() << "setting ID to " << tempVal;
        break;
    case SIMP_COL::SC_COL_EXT:
        if (ui->tableSimpleSender->item(line, SIMP_COL::SC_COL_EXT)->checkState() == Qt::Checked) {
            tempData->setExtendedFrameFormat(true);
        } else {
            tempData->setExtendedFrameFormat(false);
        }
        break;
    case SIMP_COL::SC_COL_REM:
        if (ui->tableSimpleSender->item(line, SIMP_COL::SC_COL_REM)->checkState() == Qt::Checked) {
            tempData->setFrameType(QCanBusFrame::RemoteRequestFrame);
        } else {
            tempData->setFrameType(QCanBusFrame::DataFrame);
        }
        break;
    case SIMP_COL::SC_COL_DATA: //Data bytes
        for (int i = 0; i < 8; i++) tempData->payload().data()[i] = 0;

#if QT_VERSION >= QT_VERSION_CHECK( 5, 14, 0 )
        tokens = ui->tableSimpleSender->item(line, SIMP_COL::SC_COL_DATA)->text().split(" ", Qt::SkipEmptyParts);
#else
        tokens = ui->tableSimpleSender->item(line, SIMP_COL::SC_COL_DATA)->text().split(" ", QString::SkipEmptyParts);
#endif
        arr.clear();
        arr.reserve(tokens.count());
        for (int j = 0; j < tokens.count(); j++)
        {
            arr.append((uint8_t)Utility::ParseStringToNum(tokens[j]));
        }
        tempData->setPayload(arr);
        break;
    case SIMP_COL::SC_COL_INTERVAL: //interval in ms

        QString trigger = ui->tableSimpleSender->item(line, SIMP_COL::SC_COL_INTERVAL)->text().toUpper();

        Trigger thisTrigger;
        thisTrigger.bus = -1; //-1 means we don't care which
        thisTrigger.ID = -1; //the rest of these being -1 means nothing has changed it
        thisTrigger.maxCount = -1;
        thisTrigger.milliseconds = -1;
        thisTrigger.currCount = 0;
        thisTrigger.msCounter = 0;
        thisTrigger.triggerMask = 0;
        thisTrigger.readyCount = true;

        tempData->triggers.clear();
        tempData->triggers.reserve(1);

        if (trigger != "")
        {
            thisTrigger.milliseconds = Utility::ParseStringToNum(trigger);
            thisTrigger.triggerMask |= TriggerMask::TRG_MS;
        }

        if (thisTrigger.milliseconds < 1) thisTrigger.milliseconds = 1;

        tempData->triggers.append(thisTrigger);

        break;
    }
}

void MainWindow::createSenderRow()
{
    int row = ui->tableSimpleSender->rowCount();
    ui->tableSimpleSender->insertRow(row);

    QTableWidgetItem *item = new QTableWidgetItem();
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Unchecked);
    inhibitSenderChanged = true;
    ui->tableSimpleSender->setItem(row, SIMP_COL::SC_COL_EN, item);

    item = new QTableWidgetItem();
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Unchecked);
    ui->tableSimpleSender->setItem(row, SIMP_COL::SC_COL_EXT, item);

    item = new QTableWidgetItem();
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Unchecked);
    ui->tableSimpleSender->setItem(row, SIMP_COL::SC_COL_REM, item);

    for (int i = 1; i <= SIMP_COL::SC_COL_COUNT; i++)
    {
        if (i != SIMP_COL::SC_COL_EXT && i != SIMP_COL::SC_COL_REM) {
            item = new QTableWidgetItem("");
            ui->tableSimpleSender->setItem(row, i, item);
        }
    }

    inhibitSenderChanged = false;
}

bool MainWindow::addTraceSenderLoop(int bus, quint32 canId, bool extended,
                                    const QByteArray &payload, int count,
                                    int intervalMs, QString *error)
{
    if (count < 1 || count > 1000 || intervalMs < 1 || intervalMs > 60000)
    {
        if (error) *error = tr("Loop count must be 1-1000 and interval 1-60000 ms.");
        return false;
    }
    const int row = ui->tableSimpleSender->rowCount() - 1;
    ui->tableSimpleSender->item(row, SC_COL_BUS)->setText(QString::number(bus));
    ui->tableSimpleSender->item(row, SC_COL_ID)->setText(
        QStringLiteral("0x%1").arg(canId, 0, 16).toUpper());
    ui->tableSimpleSender->item(row, SC_COL_EXT)->setCheckState(
        extended ? Qt::Checked : Qt::Unchecked);
    QStringList bytes;
    for (const char byte : payload)
        bytes << QStringLiteral("0x%1").arg(quint8(byte), 2, 16, QLatin1Char('0')).toUpper();
    ui->tableSimpleSender->item(row, SC_COL_DATA)->setText(bytes.join(QLatin1Char(' ')));
    ui->tableSimpleSender->item(row, SC_COL_INTERVAL)->setText(QString::number(intervalMs));
    FrameSendData *record = frameSender->getSendRecordRef(row);
    if (!record || record->triggers.isEmpty())
    {
        if (error) *error = tr("Could not create the CAN Trace sender entry.");
        return false;
    }
    record->triggers.first().maxCount = count;
    ui->tableSimpleSender->item(row, SC_COL_EN)->setCheckState(Qt::Checked);
    activateWorkspace(traceWorkspace, tr("CAN Trace"));
    return true;
}

void MainWindow::updateConnectionSettings(QString connectionType, QString port, int speed0, int speed1)
{
    Q_UNUSED(connectionType);
    Q_UNUSED(port);
    Q_UNUSED(speed0);
    Q_UNUSED(speed1);
    //connType = connectionType;
    //portName = port;

    //canSpeed0 = speed0;
    //canSpeed1 = speed1;
    if (isConnected)
    {
        //emit updateBaudRates(speed0, speed1);
    }
}

void MainWindow::headerClicked(int logicalIndex)
{
    //ui->canFramesView->sortByColumn(logicalIndex);
    model->sortByColumn(logicalIndex);

    manageRowExpansion();
}

void MainWindow::expandAllRows()
{
    bool goAhead = false;
    int numRows = ui->canFramesView->model()->rowCount();

    if (numRows > 20000)
    {
        QMessageBox::StandardButton confirmDialog;
        confirmDialog = QMessageBox::question(this, "Really?", "It's not recommended to use this\non more than 20000 frames.\nIt can take a long time.\n\nYou have been warned!\nStill do it?",
                                  QMessageBox::Yes|QMessageBox::No);

        if (confirmDialog == QMessageBox::Yes) goAhead = true;
    }
    else goAhead = true;

    if (goAhead)
    {
        ui->canFramesView->resizeRowsToContents();

        rowExpansionActive = true;
    }
}

void MainWindow::manageRowExpansion()
{
    int numRows = ui->canFramesView->model()->rowCount();
    if(numRows < 20000)
    {
        if(rowExpansionActive && model->getInterpretMode())
            ui->canFramesView->resizeRowsToContents();
    }
    else
    {
        disableAutoRowExpansion();
    }
}

void MainWindow::disableAutoRowExpansion()
{
    rowExpansionActive = false;
}

void MainWindow::collapseAllRows()
{
    bool goAhead = false;
    int numRows = ui->canFramesView->model()->rowCount();

    if (numRows > 50000)
    {
        QMessageBox::StandardButton confirmDialog;
        confirmDialog = QMessageBox::question(this, "Really?", "It's not recommended to use this\non more than 50000 frames.\nIt can take a long time.\n\nYou have been warned!\nStill do it?",
                                  QMessageBox::Yes|QMessageBox::No);

        if (confirmDialog == QMessageBox::Yes) goAhead = true;
    }
    else goAhead = true;

    if (goAhead)
    {
        for (int i = 0; i < numRows; i++) ui->canFramesView->setRowHeight(i, normalRowHeight);

        rowExpansionActive = false;
    }
}

void MainWindow::gridClicked(QModelIndex idx)
{
    //qDebug() << "Grid Clicked";
    if (ui->canFramesView->rowHeight(idx.row()) > normalRowHeight)
    {
        ui->canFramesView->setRowHeight(idx.row(), normalRowHeight);
    }
    else {
        ui->canFramesView->resizeRowToContents(idx.row());
    }
}

void MainWindow::gridDoubleClicked(QModelIndex idx)
{
    qDebug() << "Grid double clicked";
    //grab ID and timestamp and send them away
    CANFrame frame = model->getListReference()->at(idx.row());
    emit sendCenterTimeID(frame.frameId(), frame.timeStamp().microSeconds() / 1000000.0);
}

void MainWindow::gridContextMenuRequest(QPoint pos)
{
    QModelIndex idx = ui->canFramesView->indexAt(pos); //figure out where in the view we clicked (row, column)
    qDebug() << "Pos: " << pos << " Row :" << idx.row() << " Col: " << idx.column();

    if (!idx.isValid()) return;

    QMenu *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    menu->addAction(copyAct);

    if (idx.column() == 8) //we're over the DATA column
    {
        contextMenuPosition = pos;
        payloadContextSourceRow = proxyModel->mapToSource(idx).row();
        menu->addSeparator();
        menu->addAction(tr("Copy raw payload"), this, SLOT(copyRawPayload()));
        menu->addAction(tr("Copy decoded payload"), this, SLOT(copyDecodedPayload()));
        menu->addSeparator();
        menu->addAction(tr("Add to a new graphing window"), this, SLOT(setupAddToNewGraph()));
        menu->addAction(tr("Add to latest graphing window"), this, SLOT(setupSendToLatestGraphWindow()));
    }

    menu->popup(ui->canFramesView->viewport()->mapToGlobal(pos));
}

void MainWindow::copyRawPayload()
{
    const QByteArray payload = model->payloadAtFilteredRow(payloadContextSourceRow);
    QString raw;
    for (const char byte : payload)
    {
        if (!raw.isEmpty()) raw.append(' ');
        raw.append(QString::number(static_cast<quint8>(byte), 16).toUpper().rightJustified(2, '0'));
    }
    QApplication::clipboard()->setText(raw);
}

void MainWindow::copyDecodedPayload()
{
    const CANFrame frame = model->frameAtFilteredRow(payloadContextSourceRow);
    const QVariantMap assignments = QSettings().value("Main/PayloadIdFormats").toMap();
    const bool assigned = assignments.contains(CANFrameModel::payloadFormatKey(
                              frame.bus, frame.frameId(), frame.hasExtendedFrameFormat())) ||
                          assignments.contains(QString::number(frame.frameId()));
    QApplication::clipboard()->setText(assigned
        ? model->decodedPayload(frame) : formatPayloadForControls(frame.payload(), false));
}

void MainWindow::copyFromTable()
{
    copySelection();
}

void MainWindow::copySelection()
{
    QItemSelectionModel *selectionModel = ui->canFramesView->selectionModel();
    QModelIndexList selectedIndexes = selectionModel->selectedIndexes();

    if(selectedIndexes.isEmpty())
        return;

    // QModelIndex::operator< sorts by row and then by column.
    std::sort(selectedIndexes.begin(), selectedIndexes.end());

    QString selectedText;
    int currentRow = -1;
    int lastRow = selectedIndexes.last().row();

    for(const QModelIndex &current : selectedIndexes)
    {
        if (currentRow != -1 && current.row() != currentRow)
        {
            // remove last tab
            if (selectedText.endsWith(QLatin1Char('\t')))
                selectedText.chop(1);
            selectedText.append(QLatin1Char('\n'));
        }
        currentRow = current.row();

        QString cellText = current.data(Qt::DisplayRole).toString();

        // Replace newlines within a cell to avoid breaking the table structure in Excel
        cellText.replace(QLatin1Char('\n'), QLatin1String("  "));

        selectedText.append(cellText);

        if (current.row() != lastRow || current != selectedIndexes.last())
        {
            selectedText.append(QLatin1Char('\t'));
        }
    }

    // remove last tab if it exists
    if (selectedText.endsWith(QLatin1Char('\t')))
        selectedText.chop(1);

    QClipboard *clipboard = QApplication::clipboard();
    clipboard->setText(selectedText);
}

QString MainWindow::getSignalNameFromPosition(QPoint pos)
{
    //there's a bit of an issue to solve. The data column is one big string even if there are a number
    //of signals in there. So, the basic idea is to find out how tall the font is and where the user
    //clicked within the cell. Then find out which line that puts us over.
    QModelIndex idx = ui->canFramesView->indexAt(pos); //figure out where in the view we clicked (row, column)
    int fontHeight = ui->canFramesView->fontMetrics().height();
    int cellBaseY = ui->canFramesView->rowViewportPosition(idx.row());
    int lineOffset = (pos.y() - cellBaseY) / fontHeight;
    qDebug() << "Offset: " << lineOffset;
    QString lineText = idx.data().toString().split("\n")[lineOffset];
    qDebug() << "Line Text: " << lineText;
    return lineText.split(":")[0];
}

uint32_t MainWindow::getMessageIDFromPosition(QPoint pos)
{
    QModelIndex idx = ui->canFramesView->indexAt(pos); //figure out where in the view we clicked (row, column)
    QString idText = ui->canFramesView->model()->index(idx.row(), 1).data().toString();
    return Utility::ParseStringToNum(idText);
}

void MainWindow::setupAddToNewGraph()
{
    showGraphingWindow(); //creates a new window and sets it as latest
    setupSendToLatestGraphWindow(); //then call the other function to finish
}


void MainWindow::setupSendToLatestGraphWindow()
{
    if (!lastGraphingWindow) showGraphingWindow();
    GraphParams param;
    QString signalName = getSignalNameFromPosition(contextMenuPosition);
    param.ID = getMessageIDFromPosition(contextMenuPosition);
    DBC_MESSAGE *msg = dbcHandler->findMessage(param.ID);
    if(msg)
    {
        DBC_SIGNAL *sig = msg->sigHandler->findSignalByName(signalName);
        if(sig)
        {
            param.associatedSignal = sig;
            param.bias = sig->bias;
            param.intelFormat = sig->intelByteOrder;
            param.isSigned = sig->valType == SIGNED_INT ? true : false;
            param.numBits = sig->signalSize;
            param.scale = sig->factor;
            param.startBit = sig->startBit;
            param.stride = 1;
            param.graphName = sig->name;
            param.lineColor = QColor(QRandomGenerator::global()->bounded(160), QRandomGenerator::global()->bounded(160), QRandomGenerator::global()->bounded(160));
            param.lineWidth = 1;
            param.fillColor = QColor(128, 128, 128, 0);
            param.mask = 0xFFFFFFFFFFFFFFFFull;
            param.drawOnlyPoints = false;
            param.pointType = 0;

            lastGraphingWindow->createGraph(param); //add the new graph to the window
        }
        else
        {
            QMessageBox msgbox;
            QString boxmsg = "Cannot find ID 0x" + QStringLiteral("%1").arg(param.ID, 3, 16, QLatin1Char('0')) + " in DBC message " + msg->name + ". Not adding graph.";
            msgbox.setText(boxmsg);
            msgbox.exec();
        }
    }
    else
    {
        QMessageBox msgbox;
        QString boxmsg = "Cannot find ID 0x" + QStringLiteral("%1").arg(param.ID, 3, 16, QLatin1Char('0')) + " in DBC file(s). Not adding graph.";
        msgbox.setText(boxmsg);
        msgbox.exec();
    }
}
void MainWindow::interpretToggled(bool state)
{
    model->setInterpretMode(state);
    //ui->canFramesView->resizeRowsToContents();   //a VERY costly operation!
}

void MainWindow::overwriteToggled(bool state)
{
    if (state)
    {
        QMessageBox::StandardButton confirmDialog;
        confirmDialog = QMessageBox::question(this, "Danger Will Robinson", "Enabling Overwrite mode will\ndelete your captured frames\nand replace them with one\nframe per ID.\n\nAre you ready to do that?",
                                      QMessageBox::Yes|QMessageBox::No);
        if (confirmDialog == QMessageBox::Yes)
        {
            model->setOverwriteMode(true);
        }
        else ui->cbOverwrite->setCheckState(Qt::Unchecked);
    }
    else
    {
        rowExpansionActive = false;
        model->setOverwriteMode(false);
    }
}

void MainWindow::presistentFiltersToggled(bool state)
{
    if (state)
    {
        model->setClearMode(true);
    }
    else
    {
        model->setClearMode(false);
    }
}

void MainWindow::updateFilterList()
{
    if (model == nullptr) return;
    const QMap<int, bool> *filters = model->getFiltersReference();
    const QMap<int, bool> *busFilters = model->getBusFiltersReference();
    if (filters == nullptr || busFilters == nullptr) return;

    qDebug() << "updateFilterList called on MainWindow";

    inhibitFilterUpdate = true;

    ui->listFilters->clear();
    ui->listBusFilters->clear();

    if (filters->isEmpty()) return;

    QMap<int, bool>::const_iterator filterIter;
    for (filterIter = filters->begin(); filterIter != filters->end(); ++filterIter)
    {
        /*QListWidgetItem *thisItem = */FilterUtility::createCheckableFilterItem(filterIter.key(), filterIter.value(), ui->listFilters);
    }

    if (busFilters->isEmpty()) return;

    for (filterIter = busFilters->begin(); filterIter != busFilters->end(); ++filterIter)
    {
        /*QListWidgetItem *thisItem = */ FilterUtility::createCheckableBusFilterItem(filterIter.key(), filterIter.value(), ui->listBusFilters);
    }
    inhibitFilterUpdate = false;
}

void MainWindow::filterListItemChanged(QListWidgetItem *item)
{
    if (inhibitFilterUpdate) return;
    //qDebug() << item->text();

    // strip away possible filter label
    int ID = FilterUtility::getIdAsInt(item);
    bool isSet = false;
    if (item->checkState() == Qt::Checked) isSet = true;

    model->setFilterState(ID, isSet);

    manageRowExpansion();
}

void MainWindow::busFilterListItemChanged(QListWidgetItem *item)
{
    if (inhibitFilterUpdate) return;
    //qDebug() << item->text();

    // strip away possible filter label
    int ID = FilterUtility::getIdAsInt(item);
    bool isSet = false;
    if (item->checkState() == Qt::Checked) isSet = true;

    model->setBusFilterState(ID, isSet);

    manageRowExpansion();
}

void MainWindow::filterSetAll()
{
    inhibitFilterUpdate = true;
    for (int i = 0; i < ui->listFilters->count(); i++)
    {
        ui->listFilters->item(i)->setCheckState(Qt::Checked);
    }
    inhibitFilterUpdate = false;
    model->setAllFilters(true);

    manageRowExpansion();
}

void MainWindow::filterClearAll()
{
    inhibitFilterUpdate = true;
    for (int i = 0; i < ui->listFilters->count(); i++)
    {
        ui->listFilters->item(i)->setCheckState(Qt::Unchecked);
    }
    inhibitFilterUpdate = false;
    model->setAllFilters(false);
}

void MainWindow::logReceivedFrame(CANConnection* conn, QVector<CANFrame> frames)
{
    Q_UNUSED(conn);
    if (continuousLogging)
    {
        FrameFileIO::writeContinuousNative(&frames, 0);
    }
}

void MainWindow::tickGUIUpdate()
{
    rxFrames = model->sendBulkRefresh();
    //if(rxFrames>0)
    //{
        int elapsed = elapsedTime->elapsed();
        if(elapsed) {
            framesPerSec = (framesPerSec + (rxFrames * 1000 / elapsed)) / 2;
            elapsedTime->restart();
        }
        else
            framesPerSec = 0;

        ui->lbNumFrames->setText(QString::number(model->rowCount()));
        if (rxFrames > 0 && /*allowCapture && */ ui->cbAutoScroll->isChecked())
                ui->canFramesView->scrollToBottom();
        ui->lbFPS->setText(QString::number(framesPerSec));
        if (rxFrames > 0)
        {
            bDirty = true;
            emit framesUpdated(rxFrames); //anyone care that frames were updated?
            manageRowExpansion();
        }

        if (model->needsFilterRefresh()) updateFilterList();

        if (continuousLogging)
        {
//            const QVector<CANFrame> *modelFrames = model->getListReference();
//            FrameFileIO::writeContinuousNative(modelFrames, modelFrames->count() - rxFrames);

            continuousLogFlushCounter++;
            if ((continuousLogFlushCounter % 3) == 0)
            {
                if (ui->lblContMsg->text().length() > 2)
                {
                    ui->lblContMsg->setText("");
                }
                else
                {
                    ui->lblContMsg->setText("LOGGING");
                }
            }
            if (continuousLogFlushCounter > 8)
            {
                continuousLogFlushCounter = 0;
                FrameFileIO::flushContinuousNative();
            }
        }

        //refresh the count for all the frame senders
        FrameSendData *tempData;
        int numRows = ui->tableSimpleSender->rowCount();
        for (int i = 0; i < numRows; i++)
        {
            tempData = frameSender->getSendRecordRef(i);
            if (tempData)
            {
                ui->tableSimpleSender->item(i, SIMP_COL::SC_COL_COUNT)->setText(QString::number( tempData->count ));
                if (!tempData->enabled
                    && ui->tableSimpleSender->item(i, SIMP_COL::SC_COL_EN)->checkState() == Qt::Checked)
                {
                    ui->tableSimpleSender->blockSignals(true);
                    ui->tableSimpleSender->item(i, SIMP_COL::SC_COL_EN)
                        ->setCheckState(Qt::Unchecked);
                    ui->tableSimpleSender->blockSignals(false);
                }
            }
        }

        rxFrames = 0;
    //}
}

void MainWindow::gotFrames(int framesSinceLastUpdate)
{
    rxFrames += framesSinceLastUpdate;
    emit frameUpdateRapid(framesSinceLastUpdate);
}

void MainWindow::addFrameToDisplay(CANFrame &frame, bool autoRefresh = false)
{
    model->addFrame(frame, autoRefresh);
    if (autoRefresh)
    {
        if (ui->cbAutoScroll->isChecked()) ui->canFramesView->scrollToBottom();
        ui->lbNumFrames->setText(QString::number(model->rowCount()));
    }
}

//A sub-window is sending us a center on timestamp and ID signal
//try to find the relevant frame in the list and focus on it.
void MainWindow::gotCenterTimeID(uint32_t ID, double timestamp)
{
    int idx = model->getIndexFromTimeID(ID, timestamp);
    if (idx > -1)
    {
        ui->canFramesView->selectRow(idx);
    }
}

void MainWindow::clearFrames()
{
    ui->canFramesView->scrollToTop();
    model->clearFrames();
    CANConManager::getInstance()->resetTimeBasis();
    ui->lbNumFrames->setText(QString::number(model->rowCount()));
    bDirty = false;
    loadedFileName = "";
    updateFileStatus();
    emit framesUpdated(-1);
}

void MainWindow::normalizeTiming()
{
    model->normalizeTiming();
    emit framesUpdated(-2); //claim an all new set of frames because every frame was updated.
}

void MainWindow::handleLoadFile()
{
    QString filename;
    QVector<CANFrame> tempFrames;

    QMessageBox::StandardButton confirmDialog;

    bool loadResult = FrameFileIO::loadFrameFile(filename, &tempFrames);

    if (!loadResult)
    {
        if (tempFrames.count() > 0) //only ask if at least one frame was decoded.
        {
            confirmDialog = QMessageBox::question(this, "Error Loading", "Do you want to salvage what could be loaded?",
                                      QMessageBox::Yes|QMessageBox::No);
            if (confirmDialog == QMessageBox::Yes) {
                loadResult = true;
            }
        }
    }

    if (loadResult)
    {
        disableAutoRowExpansion();
        ui->canFramesView->scrollToTop();
        model->clearFrames();
        model->insertFrames(tempFrames);
        loadedFileName = filename;
        model->recalcOverwrite();
        ui->lbNumFrames->setText(QString::number(model->rowCount()));
        if (ui->cbAutoScroll->isChecked()) ui->canFramesView->scrollToBottom();

        updateFileStatus();
        emit framesUpdated(-1);
    }
}

void MainWindow::handleDroppedFile(const QString &filename)
{
    QProgressDialog progress(qApp->activeWindow());
    progress.setWindowModality(Qt::WindowModal);
    progress.setLabelText("Loading file...");
    progress.setCancelButton(nullptr);
    progress.setRange(0,0);
    progress.setMinimumDuration(0);
    progress.show();
    
    QVector<CANFrame> loadedFrames;
    bool loadResult = FrameFileIO::autoDetectLoadFile(filename, &loadedFrames);
    
    progress.cancel();
    
    if (!loadResult)
    {
        if (loadedFrames.count() > 0) //only ask if at least one frame was decoded.
        {
            QMessageBox::StandardButton confirmDialog = QMessageBox::question(this, "Error Loading", "Do you want to salvage what could be loaded?",
                                      QMessageBox::Yes|QMessageBox::No);
            if (confirmDialog == QMessageBox::Yes)
            {
                loadResult = true;
            }
        }
    }

    if (loadResult)
    {
        disableAutoRowExpansion();
        ui->canFramesView->scrollToTop();
        model->clearFrames();
        model->insertFrames(loadedFrames);
        loadedFileName = filename;
        model->recalcOverwrite();
        ui->lbNumFrames->setText(QString::number(model->rowCount()));
        if (ui->cbAutoScroll->isChecked()) ui->canFramesView->scrollToBottom();

        updateFileStatus();
        emit framesUpdated(-1);
    }
}


void MainWindow::handleSaveFile()
{
    QString filename;

    if (FrameFileIO::saveFrameFile(filename, model->getListReference()))
    {
        loadedFileName = filename;
        updateFileStatus();
    }
}

void MainWindow::handleContinousLogging()
{
    continuousLogging = !continuousLogging;

    if (continuousLogging)
    {
        ui->actionSave_Continuous_Logfile->setText(tr("Cease Continuous Logging"));
        FrameFileIO::openContinuousNative();
    }
    else
    {
        ui->actionSave_Continuous_Logfile->setText(tr("Start Continuous Logging"));
        ui->lblContMsg->setText("");
        FrameFileIO::closeContinuousNative();
    }
}

void MainWindow::handleSaveFilteredFile()
{
    QString filename;

    if (FrameFileIO::saveFrameFile(filename, model->getFilteredListReference()))
    {
        loadedFileName = filename;
        updateFileStatus();
    }
}

void MainWindow::handleSaveFilters()
{
    QString filename;
    QFileDialog dialog(this);
    QSettings settings;

    QStringList filters;
    filters.append(QString(tr("Filter list (*.ftl)")));

    dialog.setDirectory(settings.value("Filters/LoadSaveDirectory", dialog.directory().path()).toString());
    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setNameFilters(filters);
    dialog.setViewMode(QFileDialog::Detail);
    dialog.setAcceptMode(QFileDialog::AcceptSave);

    if (dialog.exec() == QDialog::Accepted)
    {
        filename = dialog.selectedFiles()[0];
        if (!filename.contains('.')) filename += ".ftl";
        if (dialog.selectedNameFilter() == filters[0]) model->saveFilterFile(filename);
        settings.setValue("Filters/LoadSaveDirectory", dialog.directory().path());
    }
}

void MainWindow::handleLoadFilters()
{
    QString filename;
    QFileDialog dialog(this);
    QSettings settings;

    QStringList filters;
    filters.append(QString(tr("Filter List (*.ftl)")));

    dialog.setDirectory(settings.value("Filters/LoadSaveDirectory", dialog.directory().path()).toString());
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setNameFilters(filters);
    dialog.setViewMode(QFileDialog::Detail);

    if (dialog.exec() == QDialog::Accepted)
    {
        filename = dialog.selectedFiles()[0];
        //right now there is only one file type that can be loaded here so just do it.
        model->loadFilterFile(filename);
        settings.setValue("Filters/LoadSaveDirectory", dialog.directory().path());
    }
}

void MainWindow::handleSaveDecoded()
{
    handleSaveDecodedMethod(false);
}

void MainWindow::handleSaveDecodedCsv()
{
    handleSaveDecodedMethod(true);
}

void MainWindow::handleSaveDecodedMethod(bool csv)
{
    QString filename;
    QFileDialog dialog(this);
    QSettings settings;

    QStringList filters;
    if (!csv) filters.append(QString(tr("Text File (*.txt *.TXT)")));
    else filters.append(QString(tr("CSV File (*.csv *.CSV)")));

    dialog.setDirectory(settings.value("FileIO/LoadSaveDirectory", dialog.directory().path()).toString());
    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setNameFilters(filters);
    dialog.setViewMode(QFileDialog::Detail);
    dialog.setAcceptMode(QFileDialog::AcceptSave);

    if (dialog.exec() == QDialog::Accepted)
    {
        filename = dialog.selectedFiles()[0];
        if (!filename.contains('.'))
        {
            if (!csv) filename += ".txt";
            else filename += ".csv";
        }

        if(csv)
            saveDecodedTextFileAsColumns(filename);
        else
            saveDecodedTextFile(filename);

        settings.setValue("FileIO/LoadSaveDirectory", dialog.directory().path());
    }
}

void MainWindow::saveDecodedTextFileAsColumns(QString filename)
{
    QFile *outFile = new QFile(filename);
    const QVector<CANFrame> *frames = model->getFilteredListReference();

    //const unsigned char *data;
    int dataLen;
    const CANFrame *frame;

    if (!outFile->open(QIODevice::WriteOnly | QIODevice::Text))
        return;
/*
Time: 205.173000   ID: 0x20E Std Bus: 0 Len: 8
Data Bytes: 88 10 00 13 BB 00 06 00
    SignalName	Value
*/
    QList<QPair<uint32_t, int>> msgsAndColumns;
    QStringList payloadColumns;
    for (const CANFrame &payloadFrame : *frames)
    {
        const QVector<PayloadFormatter::FormattedField> decoded = model->decodedPayloadFields(payloadFrame);
        for (const PayloadFormatter::FormattedField &field : decoded)
        {
            const QString column = field.unit.isEmpty()
                ? field.name : QStringLiteral("%1 [%2]").arg(field.name, field.unit);
            if (!payloadColumns.contains(column)) payloadColumns.append(column);
        }
    }
    int columnsAdded = 0;
    int dataStartCol = 0;

    QString builderString;
    if (CSVAbsTime)
    {
        builderString += tr("Year") + "," + tr("Month") + "," + tr("Day") + "," + tr("Hour") + "," + tr("Minute") + "," + tr("Second") + "," + tr("Ms") + ",";
        dataStartCol += 7;
    }
    else
    {
        //time
        builderString += tr("Time") + ",";
        dataStartCol++;
    }
    //id
    builderString += tr("ID") + ",";
    dataStartCol++;
    //if (frame->hasExtendedFrameFormat()) builderString += tr(" Ext ");
    //else builderString += tr(" Std ");
    //bus
    builderString += tr("Bus") + ",";
    dataStartCol++;
    //len
    builderString += tr("DataLen") + ",";
    dataStartCol++;
    builderString += tr("RawPayload") + ",";
    dataStartCol++;
    for (const QString &column : payloadColumns)
    {
        QString escapedColumn = column;
        escapedColumn.replace('"', QStringLiteral("\"\""));
        builderString += QStringLiteral("\"Payload.%1\",").arg(escapedColumn);
        dataStartCol++;
    }

    columnsAdded = dataStartCol;

    //loop through all the frames and the message data therein
    for (int c = 0; c < frames->count(); c++)
    {
        frame = &frames->at(c);
        //data = reinterpret_cast<const unsigned char *>(frame->payload().constData());
        dataLen = frame->payload().count();

        //add all column names
        if (dbcHandler != nullptr)
        {
            DBC_MESSAGE *msg = dbcHandler->findMessage(*frame);
            if (msg != nullptr)
            {
                bool found = false;
                for (int j = 0; j < msg->sigHandler->getCount(); j++)
                {
                    if(j==0)
                    {
                        for(int m=0; m<msgsAndColumns.count(); m++)
                        {
                            if(msgsAndColumns[m].first == msg->ID)
                                found = true;
                        }
                        if(found == false)
                            msgsAndColumns.append(QPair<uint32_t,int>(msg->ID, columnsAdded));
                    }

                    if(found == false)
                    {
                        QString temp;
                        if (msg->sigHandler->findSignalByIdx(j)->processAsText(*frame, temp))
                        {
                            builderString.append(msg->sigHandler->findSignalByIdx(j)->name);
                            builderString.append(",");
                            columnsAdded++;
                        }
                    }
                }
            }
        }
    }

    //add EOL
    builderString += "\n";
    //write out the header row
    outFile->write(builderString.toUtf8());

        //builderString = tr("Data Bytes: ");
        //for (int temp = 0; temp < dataLen; temp++)
        //{
        //    builderString += Utility::formatNumber(data[temp]) + " ";
        //}
        //builderString += "\n";
        //outFile->write(builderString.toUtf8());

    int dataColumnsAdded = 0;
    builderString = "";
    for (int c = 0; c < frames->count(); c++)
    {
        dataColumnsAdded = 0;
        frame = &frames->at(c);
        //data = reinterpret_cast<const unsigned char *>(frame->payload().constData());
        dataLen = frame->payload().count();

        QString builderString;
        if (CSVAbsTime)
        {
            QDateTime dt = QDateTime::fromMSecsSinceEpoch(frame->timeStamp().microSeconds() / 1000);
            builderString += QString::number(dt.date().year()) + "," + QString::number(dt.date().month()) + ",";
            builderString += QString::number(dt.date().day()) + "," + QString::number(dt.time().hour()) + ",";
            builderString += QString::number(dt.time().minute()) + "," + QString::number(dt.time().second()) + ",";
            builderString += QString::number(dt.time().msec()) + ",";
            dataColumnsAdded += 7;
        }
        else {
            builderString += QString::number((frame->timeStamp().microSeconds() / 1000000.0), 'f', 6) + ",";
            dataColumnsAdded++;
        }
        //id
        builderString += Utility::formatCANID(frame->frameId(), frame->hasExtendedFrameFormat()) + ",";
        dataColumnsAdded++;
        //if (frame->hasExtendedFrameFormat()) builderString += tr(" Ext ");
        //else builderString += tr(" Std ");
        //bus
        builderString += QString::number(frame->bus) + ",";
        dataColumnsAdded++;
        //len
        builderString += QString::number(dataLen) + ",";
        dataColumnsAdded++;
        builderString += '"' + QString::fromLatin1(frame->payload().toHex(' ').toUpper()) + QStringLiteral("\",");
        dataColumnsAdded++;
        QHash<QString, QString> payloadValues;
        const QVector<PayloadFormatter::FormattedField> decodedFields = model->decodedPayloadFields(*frame);
        for (const PayloadFormatter::FormattedField &field : decodedFields)
        {
            const QString column = field.unit.isEmpty()
                ? field.name : QStringLiteral("%1 [%2]").arg(field.name, field.unit);
            payloadValues.insert(column, field.value);
        }
        for (const QString &column : payloadColumns)
        {
            QString value = payloadValues.value(column);
            value.replace('"', QStringLiteral("\"\""));
            builderString += '"' + value + QStringLiteral("\",");
            dataColumnsAdded++;
        }

        if (dbcHandler != nullptr)
        {
            DBC_MESSAGE *msg = dbcHandler->findMessage(*frame);
            if (msg != nullptr)
            {
                for (int j = 0; j < msg->sigHandler->getCount(); j++)
                {
                    if(j==0)
                    {
                        for(int i = 0; i<msgsAndColumns.count(); i++)
                        {
                            if(msgsAndColumns[i].first == msg->ID)
                            {
                                int startCol = msgsAndColumns[i].second;
                                while(dataColumnsAdded < startCol)
                                {
                                    builderString += ",";
                                    dataColumnsAdded++;
                                }
                            }
                        }
                    }

                    QString temp;
                    if (msg->sigHandler->findSignalByIdx(j)->processAsText(*frame, temp, false, false))
                    {
                        builderString.append(temp);
                        builderString.append(",");
                        dataColumnsAdded++;
                    }
                }
            }
            builderString.append("\n");
            outFile->write(builderString.toUtf8());
        }
    }
    outFile->close();
}

void MainWindow::saveDecodedTextFile(QString filename)
{
    QFile *outFile = new QFile(filename);
    const QVector<CANFrame> *frames = model->getFilteredListReference();

    const unsigned char *data;
    int dataLen;
    const CANFrame *frame;

    if (!outFile->open(QIODevice::WriteOnly | QIODevice::Text))
        return;
/*
Time: 205.173000   ID: 0x20E Std Bus: 0 Len: 8
Data Bytes: 88 10 00 13 BB 00 06 00
    SignalName	Value
*/
    for (int c = 0; c < frames->count(); c++)
    {
        frame = &frames->at(c);
        data = reinterpret_cast<const unsigned char *>(frame->payload().constData());
        dataLen = frame->payload().count();

        QString builderString;
        builderString += tr("Time: ") + QString::number((frame->timeStamp().microSeconds() / 1000000.0), 'f', 6);
        builderString += tr("    ID: ") + Utility::formatCANID(frame->frameId(), frame->hasExtendedFrameFormat());
        if (frame->hasExtendedFrameFormat()) builderString += tr(" Ext ");
        else builderString += tr(" Std ");
        builderString += tr("Bus: ") + QString::number(frame->bus);
        builderString += " Len: " + QString::number(dataLen) + "\n";
        outFile->write(builderString.toUtf8());

        builderString = tr("Data Bytes: ");
        for (int temp = 0; temp < dataLen; temp++)
        {
            builderString += Utility::formatNumber(data[temp]) + " ";
        }
        builderString += "\n";
        outFile->write(builderString.toUtf8());

        builderString = "";
        if (dbcHandler != nullptr)
        {
            DBC_MESSAGE *msg = dbcHandler->findMessage(*frame);
            if (msg != nullptr)
            {
                for (int j = 0; j < msg->sigHandler->getCount(); j++)
                {

                    QString temp;
                    if (msg->sigHandler->findSignalByIdx(j)->processAsText(*frame, temp))
                    {
                        builderString.append("\t" + temp);
                        builderString.append("\n");
                    }
                }
            }
            builderString.append("\n");
            outFile->write(builderString.toUtf8());
        }
    }
    outFile->close();
}

void MainWindow::toggleCapture()
{
    allowCapture = !allowCapture;
    if (allowCapture)
        ui->btnCaptureToggle->setText("Suspend Capturing");
    else
        ui->btnCaptureToggle->setText("Restart Capturing");

    emit suspendCapturing(!allowCapture);
}

void MainWindow::connectionStatusUpdated(int conns)
{
    lbStatusConnected.setText(tr("Connected to ") + QString::number(conns) + tr(" buses"));
}

void MainWindow::updateFileStatus()
{
    QString output;
    if (model->rowCount() == 0)
    {
        output = tr("No packets loaded");
    }
    else
    {
        if (loadedFileName.length() > 2)
        {
            output = loadedFileName + " loaded";
        }
        else
        {
            output = tr("No file loaded");
        }

        if (bDirty)
        {
            output += " (X)";
        }
    }
    lbStatusFilename.setText(output);
}

CANFrameModel* MainWindow::getCANFrameModel()
{
    return model;
}


/*
 * All functions past this point set up the various other windows that can be opened
*/

void MainWindow::showSettingsDialog()
{
    if (!settingsDialog)
    {
        settingsDialog = new MainSettingsDialog();
        connect (settingsDialog, SIGNAL(updatedSettings()), this, SLOT(readUpdateableSettings()));
    }
    settingsDialog->show();
}

//always gets unfiltered list. You ask for the graphs so there is no need to send filtered frames
//now always creates a new window. This allows for multiple independent graphing windows
void MainWindow::showGraphingWindow()
{
/* could only allow the latest window to have these centering signals.
   if (lastGraphingWindow)
    {
        disconnect(lastGraphingWindow, SIGNAL(sendCenterTimeID(uint32_t,double)), this, SLOT(gotCenterTimeID(int32_t,double)));
        disconnect(this, SIGNAL(sendCenterTimeID(uint32_t,double)), lastGraphingWindow, SLOT(gotCenterTimeID(int32_t,double)));
        if (flowViewWindow)
        {
            disconnect(lastGraphingWindow, SIGNAL(sendCenterTimeID(uint32_t,double)), flowViewWindow, SLOT(gotCenterTimeID(int32_t,double)));
            disconnect(flowViewWindow, SIGNAL(sendCenterTimeID(uint32_t,double)), lastGraphingWindow, SLOT(gotCenterTimeID(int32_t,double)));
        }
    }
*/
    lastGraphingWindow = new GraphingWindow(model->getListReference());
    graphWindows.append(lastGraphingWindow);

    connect(lastGraphingWindow, SIGNAL(sendCenterTimeID(uint32_t,double)), this, SLOT(gotCenterTimeID(uint32_t,double)));
    connect(this, SIGNAL(sendCenterTimeID(uint32_t,double)), lastGraphingWindow, SLOT(gotCenterTimeID(uint32_t,double)));

    if (flowViewWindow) //connect the two external windows together
    {
        connect(lastGraphingWindow, SIGNAL(sendCenterTimeID(uint32_t,double)), flowViewWindow, SLOT(gotCenterTimeID(uint32_t,double)));
        connect(flowViewWindow, SIGNAL(sendCenterTimeID(uint32_t,double)), lastGraphingWindow, SLOT(gotCenterTimeID(uint32_t,double)));
    }

    lastGraphingWindow->show();
}

void MainWindow::showTemporalGraphWindow()
{
    //only create an instance of the object if we dont have one. Otherwise just display the existing one.
    if (!temporalGraphWindow)
    {
        const QVector<CANFrame> *frames;
        if (!useFiltered)
            frames = model->getListReference();
        else
            frames = model->getFilteredListReference();

        if(frames->count() > 2000)
        {
            QMessageBox::StandardButton confirmDialog;
            confirmDialog = QMessageBox::question(this, "Danger Will Robinson", "There are a lot of frames (>2000) to plot, this may take a while or crash the app. Crash likely with more than 10k frames. Continue?",
                                          QMessageBox::Yes|QMessageBox::No);
            if (confirmDialog == QMessageBox::No)
            {
                return;
            }
        }

        temporalGraphWindow = new TemporalGraphWindow(frames);
    }

    temporalGraphWindow->show();
}

void MainWindow::showFrameDataAnalysis()
{
    //only create an instance of the object if we dont have one. Otherwise just display the existing one.
    if (!frameInfoWindow)
    {
        if (!useFiltered)
            frameInfoWindow = new FrameInfoWindow(model->getListReference());
        else
            frameInfoWindow = new FrameInfoWindow(model->getFilteredListReference());
    }
    frameInfoWindow->show();
}

void MainWindow::showISOInterpreterWindow()
{
    if (!isoWindow)
    {
        if (!useFiltered)
            isoWindow = new ISOTP_InterpreterWindow(model->getListReference());
        else
            isoWindow = new ISOTP_InterpreterWindow(model->getFilteredListReference());
    }
    isoWindow->show();
}

void MainWindow::showSnifferWindow()
{
    if (!snifferWindow)
        snifferWindow = new SnifferWindow(this);
    snifferWindow->show();
}

void MainWindow::showBisectWindow()
{
    if (!bisectWindow)
    {
        bisectWindow = new BisectWindow(model->getListReference());
    }
    bisectWindow->show();
}

void MainWindow::showCANBridgeWindow()
{
    if (!canBridgeWindow)
    {
        canBridgeWindow = new CANBridgeWindow(model->getListReference());
    }
    canBridgeWindow->show();
}

void MainWindow::showFrameSenderWindow()
{
    if (!frameSenderWindow)
    {
        if (!useFiltered)
            frameSenderWindow = new FrameSenderWindow(model->getListReference());
        else
            frameSenderWindow = new FrameSenderWindow(model->getFilteredListReference());
    }
    frameSenderWindow->show();
}

void MainWindow::showPlaybackWindow()
{
    if (!playbackWindow)
    {
        if (!useFiltered)
            playbackWindow = new FramePlaybackWindow(model->getListReference());
        else
            playbackWindow = new FramePlaybackWindow(model->getFilteredListReference());
    }
    playbackWindow->show();
}

void MainWindow::showUDSFirmwareUploaderWindow()
{
    if (!udsFirmwareUploaderWindow)
    {
        udsFirmwareUploaderWindow = new UDSFirmwareUploaderWindow(model->getListReference());
    }
    udsFirmwareUploaderWindow->show();
}

void MainWindow::showComparisonWindow()
{
    if (!comparatorWindow)
    {
        comparatorWindow = new FileComparatorWindow();
    }
    comparatorWindow->show();
}

void MainWindow::showDBCComparisonWindow()
{
    if (!dbcComparatorWindow)
    {
        dbcComparatorWindow = new DBCComparatorWindow();
    }
    dbcComparatorWindow->show();
}

void MainWindow::showSingleMultiWindow()
{
    if (!discreteStateWindow)
    {
        discreteStateWindow = new DiscreteStateWindow(model->getListReference());
    }
    discreteStateWindow->show();
}

void MainWindow::showFuzzingWindow()
{
    if (!fuzzingWindow)
    {
        fuzzingWindow = new FuzzingWindow(model->getListReference());
    }
    fuzzingWindow->show();
}

void MainWindow::showMCConfigWindow()
{
    if (!motorctrlConfigWindow)
    {
        motorctrlConfigWindow = new MotorControllerConfigWindow(model->getListReference());
        //connect(motorctrlConfigWindow, SIGNAL(sendCANFrame(const CANFrame*,int)), worker, SLOT(sendFrame(const CANFrame*,int)));
        //connect(motorctrlConfigWindow, SIGNAL(sendFrameBatch(const QList<CANFrame>*)), worker, SLOT(sendFrameBatch(const QList<CANFrame>*)));
    }
    motorctrlConfigWindow->show();
}

void MainWindow::showUDSScanWindow()
{
    if (!udsScanWindow)
    {
        udsScanWindow = new UDSScanWindow(model->getListReference());
    }
    udsScanWindow->show();
}

void MainWindow::showUDSWorkbenchWindow()
{
    if (!udsWorkbenchWindow)
    {
        udsWorkbenchWindow = new UDSWorkbenchWindow(this);
        udsWorkbenchWindow->setWindowFlags(Qt::Widget);
        udsWorkbenchWindow->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        udsWorkbenchWindow->setProperty("helpPage", QStringLiteral("uds_workbench.md"));
    }
    activateWorkspace(udsWorkbenchWindow, tr("UDS Workbench"));
}

void MainWindow::showOBD2WorkbenchWindow()
{
    if (!obd2WorkbenchWindow)
    {
        obd2WorkbenchWindow = new OBD2WorkbenchWindow(this);
        obd2WorkbenchWindow->setWindowFlags(Qt::Widget);
        obd2WorkbenchWindow->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        obd2WorkbenchWindow->setProperty("helpPage", QStringLiteral("obd2_workbench.md"));
        connect(obd2WorkbenchWindow, &OBD2WorkbenchWindow::tripPlaybackPositionChanged,
                this, &MainWindow::syncTripPlayback);
    }
    activateWorkspace(obd2WorkbenchWindow, tr("OBD-II Workbench"));
}

void MainWindow::showCANopenWorkbenchWindow()
{
    if (!canopenWorkbenchWindow)
    {
        canopenWorkbenchWindow = new CANopenWorkbenchWindow(this);
        canopenWorkbenchWindow->setWindowFlags(Qt::Widget);
        canopenWorkbenchWindow->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        canopenWorkbenchWindow->setProperty("helpPage", QStringLiteral("canopen_workbench.md"));
    }
    activateWorkspace(canopenWorkbenchWindow, tr("CANopen Workbench"));
}

void MainWindow::showBusDiagnosticsWindow()
{
    if (!busDiagnosticsWindow)
    {
        busDiagnosticsWindow = new BusDiagnosticsWindow(this);
        busDiagnosticsWindow->setWindowFlags(Qt::Widget);
        busDiagnosticsWindow->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        busDiagnosticsWindow->setProperty("helpPage", QStringLiteral("bus_diagnostics.md"));
    }
    activateWorkspace(busDiagnosticsWindow, tr("Bus Diagnostics"));
}

void MainWindow::showAIWorkbenchWindow()
{
    if (!aiWorkbenchWindow)
    {
        aiWorkbenchWindow = new AIWorkbenchWindow(model->getListReference(), this);
        aiWorkbenchWindow->setApplicationContextProvider([this]() { return aiApplicationContext(); });
        aiWorkbenchWindow->setWindowFlags(Qt::Widget);
        aiWorkbenchWindow->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        aiWorkbenchWindow->setProperty("helpPage", QStringLiteral("ai_workbench.md"));
        connect(aiWorkbenchWindow, &AIWorkbenchWindow::actionsProposed,
                this, &MainWindow::handleAIActions);
    }
    activateWorkspace(aiWorkbenchWindow, tr("AI Workbench"));
}

void MainWindow::showScriptingWindow()
{
    ScriptingWindow *window = new ScriptingWindow(model->getListReference());
    window->setAttribute(Qt::WA_DeleteOnClose);
    scriptingWindows.append(window);
    connect(window, &QObject::destroyed, this, [this, window]() {
        scriptingWindows.removeAll(window);
    });
    window->show();
}

void MainWindow::showRangeWindow()
{
    if (!rangeWindow)
    {
        rangeWindow = new RangeStateWindow(model->getListReference());
    }
    rangeWindow->show();
}

void MainWindow::showFuzzyScopeWindow()
{
    //not done yet
}

void MainWindow::showFlowViewWindow()
{
    if (!flowViewWindow)
    {
        if (!useFiltered)
            flowViewWindow = new FlowViewWindow(model->getListReference());
        else
            flowViewWindow = new FlowViewWindow(model->getFilteredListReference());
        connect(flowViewWindow, SIGNAL(sendCenterTimeID(uint32_t,double)), this, SLOT(gotCenterTimeID(int32_t,double)));
        connect(this, SIGNAL(sendCenterTimeID(uint32_t,double)), flowViewWindow, SLOT(gotCenterTimeID(int32_t,double)));
    }

    if (lastGraphingWindow)
    {
        connect(lastGraphingWindow, SIGNAL(sendCenterTimeID(uint32_t,double)), flowViewWindow, SLOT(gotCenterTimeID(int32_t,double)));
        connect(flowViewWindow, SIGNAL(sendCenterTimeID(uint32_t,double)), lastGraphingWindow, SLOT(gotCenterTimeID(int32_t,double)));
    }

    flowViewWindow->show();
}


void MainWindow::DBCSettingsUpdated()
    {
    updateFilterList();
    model->sendRefresh();
    }

void MainWindow::showDBCFileWindow()
{
    if (!dbcFileWindow)
    {
        dbcFileWindow = new DBCLoadSaveWindow(model->getListReference());
        connect(dbcFileWindow, &DBCLoadSaveWindow::updatedDBCSettings, this, &MainWindow::DBCSettingsUpdated);
    }
    dbcFileWindow->show();
}

void MainWindow::showSignalViewer()
{
    if (!signalViewerWindow)
    {
        if (!useFiltered)
            signalViewerWindow = new SignalViewerWindow(model->getListReference());
        else
            signalViewerWindow = new SignalViewerWindow(model->getFilteredListReference());
    }
    signalViewerWindow->show();
}

void MainWindow::showConnectionSettingsWindow()
{
    if (!connectionWindow)
    {
        connectionWindow = new ConnectionWindow();
    }
    connectionWindow->show();
}
