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
    void startServiceScan();
    void stopServiceScan();
    void gotUDSReply(UDS_MESSAGE message);
    void requestTimedOut();
    void sendTesterPresent();
    void showDiagnosticGraph();

private:
    enum DidColumn { DidEnabled, DidName, DidIdentifier, DidFormat, DidPollMs, DidRaw, DidDecoded, DidStatus, DidUpdated, DidColumnCount };
    enum RequestContext { ContextNone, ContextSession, ContextManual, ContextDtcRead, ContextDtcClear,
                          ContextRoutine, ContextDidScan, ContextServiceScan, ContextControl, ContextSecurity };

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
    void sendNextServiceScan();
    void finishServiceScan(const QString &status);
    QString serviceName(int service) const;
    QString decodeDtcResponse(const QByteArray &payload) const;
    void appendCsvRow(int row);
    uint32_t parseNumber(const QString &text, bool *ok = nullptr) const;

    UDS_HANDLER *udsHandler;
    QSpinBox *busSpin;
    QLineEdit *requestIdEdit;
    QLineEdit *responseIdEdit;
    QComboBox *sessionCombo;
    QCheckBox *testerPresentCheck;
    QCheckBox *pollingCheck;
    QSpinBox *pollIntervalSpin;
    QLabel *connectionStatus;
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
    QListWidget *serviceScanResults;
    QPushButton *serviceScanStartButton;
    QPushButton *serviceScanStopButton;
    QPushButton *csvLogButton;
    QTextEdit *eventLog;
    QFile *csvLogFile = nullptr;
    QTimer responseTimer;
    QTimer testerTimer;
    QTimer pollingTimer;
    QQueue<int> didQueue;
    int activeDidRow = -1;
    int activeService = -1;
    RequestContext requestContext = ContextNone;
    bool endpointConnected = false;
    DiagnosticGraphWindow *diagnosticGraph = nullptr;
    bool didScanActive = false;
    bool serviceScanActive = false;
    int scanCurrentDid = 0;
    int scanEndDid = 0;
    QQueue<int> serviceScanQueue;
    int normalResponseTimeout = 1500;
};

#endif // UDSWORKBENCHWINDOW_H
