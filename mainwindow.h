#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "config.h"
#include <QMainWindow>
#include <QSerialPort>
#include <QSerialPortInfo>
#include "canframemodel.h"
#include "can_structs.h"
#include "framefileio.h"
#include "dbc/dbchandler.h"
#include "bus_protocols/isotp_handler.h"
#include "framesenderobject.h"
#include "re/graphingwindow.h"
#include "re/frameinfowindow.h"
#include "frameplaybackwindow.h"
#include "bisectwindow.h"
#include "re/flowviewwindow.h"
#include "framesenderwindow.h"
#include "re/filecomparatorwindow.h"
#include "dbc/dbcmaineditor.h"
#include "mainsettingsdialog.h"
#include "re/discretestatewindow.h"
#include "scriptingwindow.h"
#include "re/rangestatewindow.h"
#include "dbc/dbcloadsavewindow.h"
#include "re/fuzzingwindow.h"
#include "re/udsscanwindow.h"
#include "re/sniffer/snifferwindow.h"
#include "re/isotp_interpreterwindow.h"
#include "motorcontrollerconfigwindow.h"
#include "signalviewerwindow.h"
#include "re/temporalgraphwindow.h"
#include "re/dbccomparatorwindow.h"
#include "re/udsfirmwareuploaderwindow.h"
#include "re/udsworkbenchwindow.h"
#include "re/obd2workbenchwindow.h"
#include "re/canopenworkbenchwindow.h"
#include "re/busdiagnosticswindow.h"
#include "re/aiworkbenchwindow.h"
#include "re/diagnosticsimulatorwindow.h"
#include "canbridgewindow.h"

class CANConnection;
class ConnectionWindow;
class ISOTP_InterpreterWindow;
class QTabWidget;
class ScriptingWindow;
class QSortFilterProxyModel;
class QDockWidget;
class QComboBox;
class QSplitter;
class QToolButton;

enum SIMP_COL
{
    SC_COL_EN = 0,
    SC_COL_BUS = 1,
    SC_COL_ID = 2,
    SC_COL_EXT = 3,
    SC_COL_REM = 4,
    SC_COL_DATA = 5,
    SC_COL_INTERVAL = 6,
    SC_COL_LIMIT = 7,
    SC_COL_COUNT = 8,
    SC_COL_STATUS = 9,
};

namespace Ui {
class MainWindow;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = 0);
    static QString loadedFileName;
    static MainWindow *getReference();
    CANFrameModel * getCANFrameModel();
    ~MainWindow();

    void handleDroppedFile(const QString &filename);

private slots:
    void handleLoadFile();
    void handleSaveFile();
    void handleSaveFilteredFile();
    void handleSaveFilters();
    void handleLoadFilters();
    void handleContinousLogging();
    void showGraphingWindow();
    void showFrameDataAnalysis();
    void clearFrames();
    void expandAllRows();
    void collapseAllRows();
    void showPlaybackWindow();
    void showFlowViewWindow();
    void showFrameSenderWindow();
    void showSingleMultiWindow();
    void showRangeWindow();
    void showFuzzyScopeWindow();
    void showComparisonWindow();
    void showSettingsDialog();
    void showUDSFirmwareUploaderWindow();
    void showConnectionSettingsWindow();
    void showScriptingWindow();
    void showDBCFileWindow();
    void showFuzzingWindow();
    void showMCConfigWindow();
    void showUDSScanWindow();
    void showUDSWorkbenchWindow();
    void showOBD2WorkbenchWindow();
    void showCANopenWorkbenchWindow();
    void showBusDiagnosticsWindow();
    void showAIWorkbenchWindow();
    void showDiagnosticSimulatorWindow();
    void syncTripPlayback(qint64 elapsedMs);
    void showISOInterpreterWindow();
    void showSnifferWindow();
    void showBisectWindow();
    void showSignalViewer();
    void showTemporalGraphWindow();
    void showDBCComparisonWindow();
    void showCANBridgeWindow();
    void exitApp();
    void handleSaveDecoded();
    void handleSaveDecodedCsv();
    void connectionStatusUpdated(int conns);
    void gridClicked(QModelIndex);
    void gridDoubleClicked(QModelIndex);
    void gridContextMenuRequest(QPoint pos);
    void copyFromTable();
    void setupAddToNewGraph();
    void setupSendToLatestGraphWindow();
    void interpretToggled(bool);
    void overwriteToggled(bool);
    void presistentFiltersToggled(bool state);
    void logReceivedFrame(CANConnection*, QVector<CANFrame>);
    void tickGUIUpdate();
    void toggleCapture();
    void normalizeTiming();
    void updateFilterList();
    void filterListItemChanged(QListWidgetItem *item);
    void busFilterListItemChanged(QListWidgetItem *item);
    void filterSetAll();
    void filterClearAll();
    void headerClicked (int logicalIndex);
    void DBCSettingsUpdated();
    void onSenderCellChanged(int, int);
    void payloadDisplayChanged();
    void applyPayloadDisplay();
    void updatePayloadPreview();
    void recentPayloadFormatSelected(int index);
    void copyRawPayload();
    void copyDecodedPayload();
    void savePayloadProfile();
    void deletePayloadProfile();
    void assignPayloadProfileToSelectedId();
    void clearPayloadProfileForSelectedId();
    void editPayloadFormatVisually();
    void createPayloadFormatFromDbc();

public slots:
    void gotFrames(int);
    void updateSettings();
    void readUpdateableSettings();
    void gotCenterTimeID(uint32_t ID, double timestamp);
    void updateConnectionSettings(QString connectionType, QString port, int speed0, int speed1);

signals:
    void sendCANFrame(const CANFrame *, int);
    void suspendCapturing(bool);

    //-1 = frames cleared, -2 = a new file has been loaded (so all frames are different), otherwise # of new frames
    void framesUpdated(int numFrames); //something has updated the frame list (send at gui update frequency)
    void frameUpdateRapid(int numFrames);
    void settingsUpdated();
    void sendCenterTimeID(uint32_t ID, double timestamp);

private:
    Ui::MainWindow *ui;
    QSortFilterProxyModel *proxyModel;
    QDockWidget *payloadDock;
    QDockWidget *aiAssistantDock;
    bool payloadDockTraceVisible = true;
    bool payloadDockTraceContext = true;
    QTabWidget *workspaceTabs;
    QWidget *traceWorkspace;
    QSplitter *traceSenderSplitter;
    QWidget *traceSenderPanel;
    QToolButton *traceSenderCollapseButton;
    void setupWorkspaceTabs();
    void setupTraceSenderPanel();
    void activateWorkspace(QWidget *page, const QString &title);
    void populatePayloadDisplayCombo();
    void setupPayloadDock();
    QString currentHelpPage() const;
    void syncPayloadDisplayControls(const QString &mode, const QString &format);
    QString formatPayloadForControls(const QByteArray &payload, bool includeRaw) const;
    void reloadPayloadProfiles();
    void applyPayloadIdAssignments();
    int selectedPayloadSourceRow() const;
    QString formatFromSelectedDbc(QStringList *warnings = nullptr) const;
    QComboBox *payloadProfileCombo;
    int payloadContextSourceRow = -1;
    QAction *copyAct;
    static MainWindow *selfRef;

    //canbus related data
    CANFrameModel *model;
    DBCHandler *dbcHandler;
    QByteArray inputBuffer;
    QTimer updateTimer;
    QElapsedTimer *elapsedTime;
    FrameSenderObject *frameSender;
    int framesPerSec;
    int rxFrames;
    bool inhibitFilterUpdate;
    bool useHex;
    bool useColorsByCanId;
    bool allowCapture;
    bool ignoreDBCColors;
    bool CSVAbsTime;
    bool bDirty; //have frames been added or subtracted since the last save/load?
    bool useFiltered; //should sub-windows use the unfiltered or filtered frames list?
    bool inhibitSenderChanged;

    bool continuousLogging;
    int continuousLogFlushCounter;

    //References to other windows we can display

    //Graph window is allowed to instantiate more than once. All the rest are not (yet).
    GraphingWindow *lastGraphingWindow;
    QList<GraphingWindow *> graphWindows;

    FrameInfoWindow *frameInfoWindow;
    FramePlaybackWindow *playbackWindow;
    FlowViewWindow *flowViewWindow;
    FrameSenderWindow *frameSenderWindow;
    DBCMainEditor *dbcMainEditor;
    FileComparatorWindow *comparatorWindow;
    MainSettingsDialog *settingsDialog;
    DiscreteStateWindow *discreteStateWindow;
    UDSFirmwareUploaderWindow *udsFirmwareUploaderWindow;
    ConnectionWindow *connectionWindow;
    QList<ScriptingWindow *> scriptingWindows;
    RangeStateWindow *rangeWindow;
    DBCLoadSaveWindow *dbcFileWindow;
    FuzzingWindow *fuzzingWindow;
    UDSScanWindow *udsScanWindow;
    UDSWorkbenchWindow *udsWorkbenchWindow;
    OBD2WorkbenchWindow *obd2WorkbenchWindow;
    CANopenWorkbenchWindow *canopenWorkbenchWindow;
    BusDiagnosticsWindow *busDiagnosticsWindow;
    AIWorkbenchWindow *aiWorkbenchWindow;
    DiagnosticSimulatorWindow *diagnosticSimulatorWindow;
    ISOTP_InterpreterWindow *isoWindow;
    SnifferWindow* snifferWindow;
    MotorControllerConfigWindow *motorctrlConfigWindow;
    BisectWindow* bisectWindow;
    SignalViewerWindow *signalViewerWindow;
    TemporalGraphWindow *temporalGraphWindow;
    DBCComparatorWindow *dbcComparatorWindow;
    CANBridgeWindow *canBridgeWindow;

    //various private storage
    QLabel lbStatusConnected;
    QLabel lbStatusFilename;
    QLabel lbAIStatus;
    QLabel lbStatusDatabase;
    QLabel lbHelp;
    int normalRowHeight;
    bool isConnected;
    QPoint contextMenuPosition;
    bool rowExpansionActive = false;

    //private methods
    QString getSignalNameFromPosition(QPoint pos);
    uint32_t getMessageIDFromPosition(QPoint pos);
    void copySelection();
    void handleSaveDecodedMethod(bool csv);
    void saveDecodedTextFile(QString);
    void saveDecodedTextFileAsColumns(QString);
    void addFrameToDisplay(CANFrame &, bool);
    void updateFileStatus();
    void closeEvent(QCloseEvent *event);
    void killEmAll();
    void killWindow(QDialog *win);
    void readSettings();
    void writeSettings();
    bool eventFilter(QObject *obj, QEvent *event);
    void manageRowExpansion();
    void disableAutoRowExpansion();
    int createSenderRow();
    QList<int> selectedSenderRows() const;
    bool validateSenderRow(int row, QString *error = nullptr) const;
    void rebuildSenderTrigger(int row);
    void updateSenderRowStatus(int row);
    bool setSenderRowsEnabled(const QList<int> &rows, bool enabled,
                              QString *error = nullptr);
    bool sendSenderRowsOnce(const QList<int> &rows, QString *error = nullptr);
    void deleteSenderRows(const QList<int> &rows);
    void duplicateSenderRows(const QList<int> &rows);
    bool updateTraceSenderRow(int row, const QJsonObject &values,
                              QString *error = nullptr);
    bool updateTraceSenderBits(int row, const QJsonArray &changes,
                               QString *error = nullptr);
    void editSenderBits(int row = -1);
    void copySelectedTraceToSender();
    void moveSenderRowsToAdvanced(const QList<int> &rows);
    void saveTraceSenderList();
    void loadTraceSenderList();
    bool saveTraceSenderListToPath(const QString &path, QString *error = nullptr);
    bool loadTraceSenderListFromPath(const QString &path, QString *error = nullptr);
    void showSenderContextMenu(const QPoint &pos);
    void showSenderHeaderMenu(const QPoint &pos);
    void setTraceSenderCollapsed(bool collapsed);
    bool addTraceSenderLoop(int bus, quint32 canId, bool extended,
                            const QByteArray &payload, int count,
                            int intervalMs, QString *error);
    void handleAIAction(const QJsonObject &action);
    void handleAIActions(const QJsonArray &actions);
    QJsonObject aiApplicationContext() const;
    void processSenderCellChange(int line, int col);
};

#endif // MAINWINDOW_H
