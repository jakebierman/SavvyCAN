#ifndef SNIFFER_H
#define SNIFFER_H

#include <QDialog>
#include <QHash>
#include <QListWidgetItem>
#include <QJsonObject>
#include "sniffermodel.h"
#include "SnifferDelegate.h"

namespace Ui {
class snifferWindow;
}

enum tc
{
    DELTA = 0,
    FREQUENCY,
    ID,
    DATA_0,
    DATA_1,
    DATA_2,
    DATA_3,
    DATA_4,
    DATA_5,
    DATA_6,
    DATA_7,
    LAST
};


class SnifferWindow : public QDialog
{
    Q_OBJECT

public:
    explicit SnifferWindow(QWidget *parent = 0);
    ~SnifferWindow();

    void showEvent(QShowEvent*);
    void closeEvent(QCloseEvent*);
    QJsonObject aiState() const;
    bool executeAIRequest(const QString &operation, const QJsonObject &arguments,
                          QString *error = nullptr);

public slots:
    void update();
    void notchTick();
    void idChange(int, bool);
    void fltAll();
    void fltNone();
    void itemChanged(QListWidgetItem*);
    void captureExperimentFrames(CANConnection*, QVector<CANFrame>& frames);
    void analyzeExperiment();
    void inferCountersAndChecksums();
    void correlateDiagnostics();
    void clusterSignals();
    void exportDbcCandidates();

private:
    void filter(bool pFilter);
    void readSettings();
    void writeSettings();
    void beginExperimentCapture(int phase);
    void ensureAnalysisWindow();
    void addAnalysisRow(const QString &type, quint32 id, const QString &field,
                        double score, const QString &evidence);
    QVector<CANFrame> experimentFrames() const;
    static double correlation(const QVector<double> &left, const QVector<double> &right);

    Ui::snifferWindow*          ui;
    SnifferModel                mModel;
    QTimer                      mGUITimer;
    QTimer                      mNotchTimer;
    QMap<int, QListWidgetItem*> mMap;
    bool                        mFilter;
    SnifferDelegate             *sniffDel;
    QAbstractItemDelegate       *defaultDel;
    bool                        notchPingPong;
    int                         mExperimentPhase = -1;
    QVector<CANFrame>           mBaselineFrames;
    QVector<CANFrame>           mActionFrames;
    QVector<CANFrame>           mControlFrames;
    class QLabel                *mExperimentStatus = nullptr;
    class QDialog               *mAnalysisDialog = nullptr;
    class QTableWidget          *mAnalysisTable = nullptr;
};

#endif // SNIFFER_H
