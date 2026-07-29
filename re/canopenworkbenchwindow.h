#ifndef CANOPENWORKBENCHWINDOW_H
#define CANOPENWORKBENCHWINDOW_H

#include <QByteArray>
#include <QDialog>
#include <QMap>
#include <QQueue>
#include <QTimer>

#include "can_structs.h"

class QComboBox;
class CANConnection;
class QCheckBox;
class QLabel;
class QLineEdit;
class QJsonObject;
class QSpinBox;
class QTableWidget;
class QTextEdit;
class DiagnosticGraphWindow;

class CANopenWorkbenchWindow : public QDialog
{
    Q_OBJECT

public:
    explicit CANopenWorkbenchWindow(QWidget *parent = nullptr);
    ~CANopenWorkbenchWindow();
    bool addObjectDefinition(int nodeId, int index, int subIndex, const QString &name,
                             const QString &dataType, const QString &access,
                             const QString &value, QString *error = nullptr);
    bool executeAIRequest(const QString &operation, const QJsonObject &arguments,
                          QString *error = nullptr);

private slots:
    void processFrames(CANConnection *connection, QVector<CANFrame> &frames);
    void scanNodes();
    void sendNmt();
    void uploadObject();
    void downloadObject();
    void importEds();
    void exportDcf();
    void addObjectEntry();
    void removeObjectEntries();
    void uploadSelectedObjects();
    void downloadSelectedObjects();
    void sendSync();
    void sendTime();
    void showPdoGraph();
    void discoverLss();
    void configureLss();
    void readDriveState();
    void sendDriveCommand();
    void remapSelectedPdo();
    void refreshAges();

private:
    enum NodeColumn { NodeId, NodeState, NodeHeartbeat, NodeIdentity, NodeEmergency, NodeColumnCount };
    enum ObjectColumn { ObjectIndex, ObjectSubIndex, ObjectName, ObjectType, ObjectAccess,
                        ObjectValue, ObjectDefault, ObjectColumnCount };
    enum PdoColumn { PdoCobId, PdoNode, PdoType, PdoData, PdoDecoded, PdoCount, PdoUpdated, PdoColumnCount };

    struct NodeInfo {
        int row = -1;
        qint64 heartbeatMs = 0;
        QString state;
        QString identity;
        QString emergency;
    };
    struct SdoTransfer {
        bool active = false;
        bool upload = true;
        int node = 0;
        int index = 0;
        int subIndex = 0;
        bool toggle = false;
        QByteArray data;
        QByteArray pending;
        bool block = false;
        int sequence = 0;
        int blockSize = 127;
        int lastUnused = 0;
        bool blockLast = false;
    };
    struct PendingWrite {
        int index = 0;
        int subIndex = 0;
        QByteArray value;
    };

    void buildUi();
    void sendFrame(quint32 id, const QByteArray &payload, bool remote = false);
    void handleFrame(const CANFrame &frame);
    void handleHeartbeat(int node, const QByteArray &payload);
    void handleEmergency(int node, const QByteArray &payload);
    void handleSdoResponse(int node, const QByteArray &payload);
    void handlePdo(quint32 cobId, const QByteArray &payload);
    QString decodePdo(int mappingIndex, const QByteArray &payload, QMap<QString, double> *numeric) const;
    QString formatObjectValue(int row, const QByteArray &value, double *numeric = nullptr) const;
    void ensureNode(int node);
    void updateNodeRow(int node);
    int findObjectRow(int index, int subIndex) const;
    int ensureObjectRow(int index, int subIndex);
    void setObjectValue(int index, int subIndex, const QByteArray &value);
    void sendNextUploadSegment();
    void sendNextDownloadSegment();
    void sendBlockDownloadWindow();
    void finishSdo(const QString &message);
    void startNextQueuedWrite();
    bool importEdsFile(const QString &fileName, QString *error = nullptr);
    bool exportDcfFile(const QString &fileName, QString *error = nullptr);
    int selectedNode() const;
    int number(const QString &text, bool *ok = nullptr) const;
    QByteArray parseBytes(const QString &text, bool *ok = nullptr) const;
    QByteArray objectRowBytes(int row, bool *ok = nullptr) const;
    QString abortDescription(quint32 code) const;
    QString stateName(quint8 state) const;
    void loadSettings();
    void saveSettings() const;

    QSpinBox *busSpin;
    QSpinBox *nodeSpin;
    QLabel *statusLabel;
    QTableWidget *nodeTable;
    QComboBox *nmtCommandCombo;
    QLineEdit *indexEdit;
    QSpinBox *subIndexSpin;
    QComboBox *dataTypeCombo;
    QLineEdit *sdoValueEdit;
    QCheckBox *blockSdoCheck;
    QTextEdit *sdoLog;
    QTableWidget *objectTable;
    QTableWidget *pdoTable;
    QTableWidget *emcyTable;
    QLineEdit *syncCobIdEdit;
    QLineEdit *timeCobIdEdit;
    QTextEdit *eventLog;
    QLabel *lssStatusLabel;
    QSpinBox *lssNodeSpin;
    QComboBox *lssBitrateCombo;
    QLabel *driveStateLabel;
    QComboBox *driveCommandCombo;
    QMap<int, NodeInfo> nodes;
    QMap<quint32, int> pdoRows;
    SdoTransfer transfer;
    QQueue<PendingWrite> writeQueue;
    QQueue<QPair<int, int>> uploadQueue;
    QTimer ageTimer;
    DiagnosticGraphWindow *pdoGraph = nullptr;
};

#endif
