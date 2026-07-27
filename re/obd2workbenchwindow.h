#ifndef OBD2WORKBENCHWINDOW_H
#define OBD2WORKBENCHWINDOW_H

#include <QDialog>
#include <QMap>
#include <QSet>
#include <QTimer>
#include <QVector>

#include "bus_protocols/uds_handler.h"

class QCheckBox;
class QComboBox;
class QGridLayout;
class QGroupBox;
class QLabel;
class QLineEdit;
class QJsonArray;
class QJsonObject;
class QFile;
class QListWidget;
class QPushButton;
class QProgressBar;
class QSlider;
class QSpinBox;
class QTableWidget;
class QTextEdit;
class DiagnosticGraphWindow;
class OBDDashboardCanvas;

class OBD2WorkbenchWindow : public QDialog
{
    Q_OBJECT

public:
    explicit OBD2WorkbenchWindow(QWidget *parent = nullptr);
    ~OBD2WorkbenchWindow();
    bool addPidRequest(const QString &name, const QString &pid, const QString &format,
                       QString *error = nullptr);
    bool executeAIRequest(const QString &operation, const QJsonObject &arguments,
                          QString *error = nullptr);
    bool addDashboardPidByNumber(int pid, QString *error = nullptr);
    int clearPidRequests();
    static bool resolvePidDescription(const QString &description, int *pid,
                                      QString *name = nullptr, QString *format = nullptr,
                                      double *confidence = nullptr);

signals:
    void tripPlaybackPositionChanged(qint64 elapsedMs);

private slots:
    void connectEndpoint();
    void disconnectEndpoint();
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
    void requestFreezeFrame();
    void scanFreezeFramePids();
    void requestMonitorResults();
    void scanModules();
    void scanSupportedPids();
    void addSelectedScannedPids();
    void loadDiscoveryResults();
    void saveDiscoveryResults();
    void gotReply(UDS_MESSAGE message);
    void requestFinished();
    void showDiagnosticGraph();
    void addDashboardPid();
    void editDashboardWidget();
    void removeDashboardPid();
    void saveDashboardLayout();
    void loadDashboardLayout();
    void loadDtcDatabase();
    void exportDiagnosticReport();
    void toggleTripRecording();
    void loadMode06Scalings();
    void loadTripPlayback();
    void toggleTripPlayback();
    void advanceTripPlayback();
    void setTripPlaybackRow(int row);

private:
    enum PidColumn { PidEnabled, PidName, PidNumber, PidFormat, PidResponses, PidUpdated, PidColumnCount };
    enum Context { None, LivePid, StoredDtc, PendingDtc, PermanentDtc, ClearDtc, VehicleInfo,
                   ModuleScan, SupportedPidScan, FreezeFrame, FreezeSupportScan, MonitorResults };

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
    QString decodeMonitorResults(const QByteArray &data) const;
    QString decodeReadiness(int pid, const QByteArray &data) const;
    void rebuildDashboard();
    QPair<QString, double> dashboardDisplayValue(const QString &name, double value) const;
    void updateDashboard(int pid, const QString &ecu, const QString &decoded,
                         const QVector<QPair<QString, double>> &numericValues);
    QJsonArray dashboardToJson() const;
    void loadDashboardJson(const QJsonArray &items);
    void loadSettings();
    void saveSettings() const;
    QJsonArray pidRowsToJson() const;
    void loadPidRows(const QJsonArray &rows);
    uint32_t parseNumber(const QString &text, bool *ok = nullptr) const;
    QVector<QPair<uint32_t, uint32_t>> responseRules(bool *ok = nullptr, QString *error = nullptr) const;
    bool responseMatches(uint32_t id) const;

    UDS_HANDLER *handler;
    QSpinBox *busSpin;
    QComboBox *requestIdEdit;
    QComboBox *responseModeCombo;
    QLineEdit *responseIdEdit;
    QLineEdit *responseMaskEdit;
    QLabel *responseMaskLabel;
    QLabel *connectionStatus;
    QPushButton *connectButton;
    QList<QWidget *> requestControls;
    QTableWidget *pidTable;
    QCheckBox *pollCheck;
    QSpinBox *pollIntervalSpin;
    QTextEdit *dtcOutput;
    QLabel *dtcDatabaseStatus;
    QComboBox *vehiclePidEdit;
    QTextEdit *vehicleOutput;
    QLineEdit *freezePidEdit;
    QSpinBox *freezeFrameSpin;
    QTextEdit *freezeOutput;
    QLineEdit *monitorMidEdit;
    QTextEdit *monitorOutput;
    QTextEdit *discoveryOutput;
    QListWidget *discoveryPidList;
    QComboBox *dashboardPidCombo;
    QComboBox *dashboardRemoveCombo;
    QSpinBox *dashboardColumnsSpin;
    QGridLayout *dashboardGrid;
    OBDDashboardCanvas *widgetDashboardCanvas;
    QList<int> dashboardPids;
    QMap<int, QGroupBox *> dashboardTiles;
    QMap<int, QLabel *> dashboardValueLabels;
    QMap<int, QLabel *> dashboardStatsLabels;
    QMap<int, QProgressBar *> dashboardBars;
    struct DashboardStats { qint64 count = 0; double sum = 0; double minimum = 0; double maximum = 0; };
    QMap<QString, DashboardStats> dashboardStats;
    QMap<QString, QString> dtcDescriptions;
    QString dtcDatabaseSource;
    QString dtcDatabaseVersion;
    QPushButton *tripRecordButton;
    QFile *tripLogFile = nullptr;
    struct MonitorScaling { double factor = 1.0; double offset = 0.0; QString unit; bool signedValue = false; };
    QMap<int, MonitorScaling> monitorScalings;
    QLabel *monitorScalingStatus;
    QTableWidget *tripPlaybackTable;
    QSlider *tripPlaybackSlider;
    QPushButton *tripPlaybackButton;
    QTimer tripPlaybackTimer;
    QVector<qint64> tripPlaybackTimes;
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
    DiagnosticGraphWindow *diagnosticGraph = nullptr;
};

#endif
