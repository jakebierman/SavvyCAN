#ifndef OBD2WORKBENCHWINDOW_H
#define OBD2WORKBENCHWINDOW_H

#include <QDialog>
#include <QMap>
#include <QSet>
#include <QTimer>

#include "bus_protocols/uds_handler.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QJsonArray;
class QListWidget;
class QSpinBox;
class QTableWidget;
class QTextEdit;

class OBD2WorkbenchWindow : public QDialog
{
    Q_OBJECT

public:
    explicit OBD2WorkbenchWindow(QWidget *parent = nullptr);
    ~OBD2WorkbenchWindow();

private slots:
    void connectEndpoint();
    void addPid();
    void removePid();
    void requestSelectedPid();
    void requestEnabledPids();
    void loadPidList();
    void savePidList();
    void pollPids();
    void readStoredDtcs();
    void readPendingDtcs();
    void readPermanentDtcs();
    void clearDtcs();
    void requestVehicleInfo();
    void scanModules();
    void scanSupportedPids();
    void addSelectedScannedPids();
    void gotReply(UDS_MESSAGE message);
    void requestFinished();

private:
    enum PidColumn { PidEnabled, PidName, PidNumber, PidFormat, PidResponses, PidUpdated, PidColumnCount };
    enum Context { None, LivePid, StoredDtc, PendingDtc, PermanentDtc, ClearDtc, VehicleInfo, ModuleScan, SupportedPidScan };

    void buildUi();
    void sendRequest(int mode, const QByteArray &data, Context context);
    QString pidText(int row) const;
    void setPidText(int row, const QString &text);
    QString knownPidName(int pid) const;
    QString knownPidFormat(int pid) const;
    QString decodeCompoundPid(int pid, const QByteArray &data) const;
    QString supportedPidText(const QByteArray &data, int basePid, QSet<int> *result = nullptr) const;
    void finishSupportedPidScan();
    QString decodeObdDtcs(const QByteArray &data) const;
    QString decodeVehicleInfo(int pid, const QByteArray &data) const;
    void loadSettings();
    void saveSettings() const;
    QJsonArray pidRowsToJson() const;
    void loadPidRows(const QJsonArray &rows);
    uint32_t parseNumber(const QString &text, bool *ok = nullptr) const;

    UDS_HANDLER *handler;
    QSpinBox *busSpin;
    QLineEdit *requestIdEdit;
    QLabel *connectionStatus;
    QTableWidget *pidTable;
    QCheckBox *pollCheck;
    QSpinBox *pollIntervalSpin;
    QTextEdit *dtcOutput;
    QLineEdit *vehiclePidEdit;
    QTextEdit *vehicleOutput;
    QTextEdit *discoveryOutput;
    QListWidget *discoveryPidList;
    QTextEdit *eventLog;
    QTimer responseTimer;
    QTimer pollTimer;
    QList<int> pidQueue;
    QList<int> scanPidBases;
    QMap<uint32_t, QSet<int>> supportedPidsByEcu;
    Context context = None;
    int activeMode = -1;
    int activePid = -1;
    int activePidRow = -1;
    bool connected = false;
};

#endif
