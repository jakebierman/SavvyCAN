#ifndef UDSWORKBENCHWINDOW_H
#define UDSWORKBENCHWINDOW_H

#include <QDialog>
#include <QQueue>
#include <QTimer>

#include "bus_protocols/uds_handler.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QJsonArray;
class QJsonObject;
class QFile;
class QListWidget;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTextEdit;
class QTreeWidget;
class DiagnosticGraphWindow;

class UDSWorkbenchWindow : public QDialog
{
    Q_OBJECT

public:
    explicit UDSWorkbenchWindow(QWidget *parent = nullptr);
    ~UDSWorkbenchWindow();
    bool addDidRequest(const QString &name, const QString &did, const QString &format,
                       int pollMs, QString *error = nullptr);
    bool executeAIRequest(const QString &operation, const QJsonObject &arguments,
                          QString *error = nullptr);

private slots:
    void connectEndpoint();
    void disconnectEndpoint();
    void addDid();
    void removeDid();
    void requestSelectedDid();
    void requestAllDids();
    void importProfile();
    void exportProfile();
    void pollEnabledDids();
    void requestDtcs();
    void clearDtcs();
    void sendRoutineControl();
    void sendDetailedDtcRequest();
    void sendGuardedControl();
    void requestSecuritySeed();
    void sendSecurityKey();
    void toggleCsvLogging();
    void sendManualRequest();
    void startDidScan();
    void resumeDidScan();
    void stopDidScan();
    void addSelectedScannedDids();
    void saveDidScan();
    void loadDidScan();
    void startEcuScan();
    void stopEcuScan();
    void useSelectedEcu();
    void saveEcuScan();
    void loadEcuScan();
    void startServiceScan();
    void stopServiceScan();
    void saveServiceScan();
    void loadServiceScan();
    void startSessionScan();
    void refreshDiscoverySummary();
    void exportDiscoveryCsv();
    void compareDiscoverySnapshot();
    void applyAddressPreset();
    void learnResponseAddress();
    void inferPassiveEndpoints();
    void verifySelectedEcu();
    void editSelectedEcuDetails();
    void gotUDSReply(UDS_MESSAGE message);
    void requestTimedOut();
    void sendTesterPresent();
    void showDiagnosticGraph();

private:
    enum DidColumn { DidEnabled, DidName, DidIdentifier, DidFormat, DidPollMs, DidRaw, DidDecoded, DidStatus, DidUpdated, DidColumnCount };
    enum RequestContext { ContextNone, ContextSession, ContextManual, ContextDtcRead, ContextDtcClear,
                          ContextRoutine, ContextDidScan, ContextServiceScan, ContextEcuScan, ContextSessionScan,
                          ContextControl, ContextSecurity };

    void buildUi();
    void sendNextDid();
    void sendDidRow(int row);
    void finishCurrentRequest(const QString &status);
    void log(const QString &text);
    void loadSettings();
    void saveSettings() const;
    QJsonArray didRowsToJson() const;
    void loadDidRows(const QJsonArray &rows);
    void sendServiceRequest(int service, const QByteArray &data, RequestContext context);
    void sendNextDidScan();
    void finishDidScan(const QString &status);
    void sendNextEcuProbe();
    void finishEcuScan(const QString &status);
    void addEcuDiscoveryResult(uint32_t requestId, uint32_t responseId,
                               const QString &status, int confidence);
    uint32_t inferredRequestId(uint32_t responseId, bool *ok = nullptr) const;
    QString describe29BitAddress(uint32_t id) const;
    void updateResponseIdFromMode();
    void sendNextServiceScan();
    void finishServiceScan(const QString &status);
    void sendNextSessionScan();
    void finishSessionScan(const QString &status);
    bool transmissionAllowed(int service, bool modifying = false);
    QString serviceName(int service) const;
    QString decodeDtcResponse(const QByteArray &payload) const;
    void appendCsvRow(int row);
    uint32_t parseNumber(const QString &text, bool *ok = nullptr) const;
    QVector<uint32_t> parseIdSpec(const QString &text, bool *ok = nullptr) const;

    UDS_HANDLER *udsHandler;
    QSpinBox *busSpin;
    QLineEdit *requestIdEdit;
    QComboBox *addressPresetCombo;
    QComboBox *responseAddressModeCombo;
    QLineEdit *responseOffsetEdit;
    QLineEdit *responseIdEdit;
    QComboBox *sessionCombo;
    QComboBox *safetyModeCombo;
    QSpinBox *p2TimeoutSpin;
    QSpinBox *p2StarTimeoutSpin;
    QSpinBox *flowBlockSizeSpin;
    QSpinBox *flowStMinSpin;
    QCheckBox *testerPresentCheck;
    QCheckBox *pollingCheck;
    QSpinBox *pollIntervalSpin;
    QLabel *connectionStatus;
    QPushButton *connectButton;
    QList<QWidget *> requestControls;
    QTableWidget *didTable;
    QLineEdit *manualServiceEdit;
    QLineEdit *manualPayloadEdit;
    QTextEdit *manualResponse;
    QLineEdit *dtcStatusMaskEdit;
    QComboBox *dtcDetailTypeCombo;
    QLineEdit *dtcDetailDataEdit;
    QTextEdit *dtcResponse;
    QComboBox *routineTypeCombo;
    QLineEdit *routineIdEdit;
    QLineEdit *routineDataEdit;
    QTextEdit *routineResponse;
    QComboBox *controlServiceCombo;
    QLineEdit *controlDataEdit;
    QTextEdit *controlResponse;
    QSpinBox *securityLevelSpin;
    QLineEdit *securityKeyEdit;
    QTextEdit *securityResponse;
    QLineEdit *scanStartEdit;
    QLineEdit *scanEndEdit;
    QSpinBox *scanTimeoutSpin;
    QProgressBar *scanProgress;
    QListWidget *scanResults;
    QPushButton *scanStartButton;
    QPushButton *scanResumeButton;
    QPushButton *scanStopButton;
    QLineEdit *ecuRequestSpecEdit;
    QLineEdit *ecuResponseSpecEdit;
    QCheckBox *ecuAnyResponseCheck;
    QSpinBox *ecuScanTimeoutSpin;
    QSpinBox *ecuScanDelaySpin;
    QSpinBox *ecuScanLimitSpin;
    QListWidget *ecuScanResults;
    QPushButton *ecuScanStartButton;
    QPushButton *ecuScanStopButton;
    QListWidget *serviceScanResults;
    QPushButton *serviceScanStartButton;
    QPushButton *serviceScanStopButton;
    QListWidget *sessionScanResults;
    QTreeWidget *discoverySummaryTree;
    QPushButton *csvLogButton;
    QTextEdit *eventLog;
    QFile *csvLogFile = nullptr;
    QTimer responseTimer;
    QTimer testerTimer;
    QTimer pollingTimer;
    QQueue<int> didQueue;
    int activeDidRow = -1;
    int activeService = -1;
    int activeSessionScan = -1;
    int originalSessionScan = 1;
    RequestContext requestContext = ContextNone;
    bool endpointConnected = false;
    DiagnosticGraphWindow *diagnosticGraph = nullptr;
    bool didScanActive = false;
    bool ecuScanActive = false;
    bool serviceScanActive = false;
    bool sessionScanActive = false;
    int scanCurrentDid = 0;
    int scanEndDid = 0;
    QQueue<int> serviceScanQueue;
    QQueue<int> sessionScanQueue;
    QQueue<uint32_t> ecuRequestQueue;
    QVector<uint32_t> ecuResponseIds;
    bool ecuAcceptAnyResponse = false;
    uint32_t activeEcuRequestId = 0;
    int ecuProbeCount = 0;
    int ecuErrorFrameBaseline = 0;
    int normalResponseTimeout = 1500;
};

#endif // UDSWORKBENCHWINDOW_H
