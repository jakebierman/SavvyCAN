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
class QFile;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTextEdit;

class UDSWorkbenchWindow : public QDialog
{
    Q_OBJECT

public:
    explicit UDSWorkbenchWindow(QWidget *parent = nullptr);
    ~UDSWorkbenchWindow();

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
    void toggleCsvLogging();
    void sendManualRequest();
    void gotUDSReply(UDS_MESSAGE message);
    void requestTimedOut();
    void sendTesterPresent();

private:
    enum DidColumn { DidEnabled, DidName, DidIdentifier, DidFormat, DidPollMs, DidRaw, DidDecoded, DidStatus, DidUpdated, DidColumnCount };
    enum RequestContext { ContextNone, ContextSession, ContextManual, ContextDtcRead, ContextDtcClear, ContextRoutine };

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
    QTextEdit *dtcResponse;
    QComboBox *routineTypeCombo;
    QLineEdit *routineIdEdit;
    QLineEdit *routineDataEdit;
    QTextEdit *routineResponse;
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
};

#endif // UDSWORKBENCHWINDOW_H
