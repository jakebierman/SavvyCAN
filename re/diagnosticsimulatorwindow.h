#ifndef DIAGNOSTICSIMULATORWINDOW_H
#define DIAGNOSTICSIMULATORWINDOW_H

#include <QDialog>
#include <QJsonArray>
#include <QMap>
#include <QPointer>
#include "can_structs.h"
#include "connections/canconnection.h"

class QLabel;
class QSpinBox;
class QTableWidget;
class QTabWidget;
class QTextEdit;

struct DiagnosticSimValue
{
    QString kind;
    QString key;
    QString name;
    QByteArray value;
    QString encoding;
    bool writable = false;
};

struct DiagnosticSimEcu
{
    bool enabled = true;
    QString name;
    quint32 requestId = 0x7E0;
    quint32 responseId = 0x7E8;
    bool extended = false;
    int responseDelayMs = 15;
    int session = 1;
    int securityLevel = 0;
    QString notes;
    QList<DiagnosticSimValue> values;
};

struct DiagnosticSimFault
{
    bool enabled = false;
    QString mode = QStringLiteral("None");
    int every = 1;
    int delayMs = 0;
    int nrc = 0x22;
    int counter = 0;
};

class VirtualDiagnosticConnection : public CANConnection
{
    Q_OBJECT
public:
    explicit VirtualDiagnosticConnection(QObject *owner = nullptr);
    void setEcus(const QList<DiagnosticSimEcu> &ecus);
    QList<DiagnosticSimEcu> ecus() const;
    void setFault(const DiagnosticSimFault &fault);
    DiagnosticSimFault fault() const;
    void resetState();

signals:
    void activity(QString text);
    void ecuStateChanged();

protected:
    void piStarted() override;
    void piStop() override;
    void piSetBusSettings(int bus, CANBus settings) override;
    bool piGetBusSettings(int bus, CANBus &settings) override;
    void piSuspend(bool suspend) override;
    bool piSendFrame(const CANFrame &frame) override;

private:
    QByteArray decodeIsoTp(const CANFrame &frame);
    QByteArray udsResponse(DiagnosticSimEcu &ecu, const QByteArray &request);
    QByteArray obdResponse(DiagnosticSimEcu &ecu, const QByteArray &request);
    void enqueueResponse(const DiagnosticSimEcu &ecu, const QByteArray &payload, int delay);
    void enqueueFrame(CANFrame frame, int delay);
    DiagnosticSimValue *findValue(DiagnosticSimEcu &ecu, const QString &kind,
                                  const QString &key);

    QList<DiagnosticSimEcu> mEcus;
    DiagnosticSimFault mFault;
    QMap<quint32, QByteArray> mMultiFrameRequests;
    QMap<quint32, int> mMultiFrameLengths;
    QPointer<QObject> mOwner;
};

class DiagnosticSimulatorWindow : public QDialog
{
    Q_OBJECT
public:
    explicit DiagnosticSimulatorWindow(const QVector<CANFrame> *frames,
                                       QWidget *parent = nullptr);
    ~DiagnosticSimulatorWindow() override;

private slots:
    void startSimulator();
    void stopSimulator();
    void resetSimulator();
    void addEcu();
    void removeSelectedEcus();
    void addValue();
    void removeSelectedValues();
    void loadSelectedEcu();
    void storeSelectedEcu();
    void learnFromCapture();
    void loadProject();
    void saveProject();
    void exportReport();
    void runScenarios();
    void updateFault();
    void appendActivity(const QString &text);

private:
    void buildUi();
    QList<DiagnosticSimEcu> ecusFromTables() const;
    void populateEcus(const QList<DiagnosticSimEcu> &ecus);
    void populateValues(const QList<DiagnosticSimValue> &values);
    QJsonObject projectJson() const;
    bool loadProjectJson(const QJsonObject &root, QString *error = nullptr);
    static quint32 parseId(const QString &text, bool *ok = nullptr);
    static QByteArray parseBytes(const QString &text);
    static QString bytesText(const QByteArray &data);

    const QVector<CANFrame> *mFrames;
    VirtualDiagnosticConnection *mConnection = nullptr;
    QTableWidget *mEcuTable = nullptr;
    QTableWidget *mValueTable = nullptr;
    QTableWidget *mScenarioTable = nullptr;
    QTableWidget *mTimelineTable = nullptr;
    QTextEdit *mActivity = nullptr;
    QLabel *mStatus = nullptr;
    QSpinBox *mFaultEvery = nullptr;
    QSpinBox *mFaultDelay = nullptr;
    QSpinBox *mFaultNrc = nullptr;
    class QComboBox *mFaultMode = nullptr;
    int mLoadedEcuRow = -1;
};

#endif
