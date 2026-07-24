#ifndef AIWORKBENCHWINDOW_H
#define AIWORKBENCHWINDOW_H

#include <QDialog>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <functional>

#include "can_structs.h"

class QCheckBox;
class AIChatTranscript;
class CANConnection;
class QComboBox;
class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QNetworkReply;
class QPlainTextEdit;
class QPushButton;
class QProcess;
class QProcessEnvironment;
class QSpinBox;
class QTabWidget;
class QTimer;

class AIWorkbenchWindow : public QDialog
{
    Q_OBJECT

public:
    explicit AIWorkbenchWindow(const QVector<CANFrame> *frames, QWidget *parent = nullptr);
    ~AIWorkbenchWindow() override;
    void setApplicationContextProvider(const std::function<QJsonObject()> &provider);
    bool authorizeTransmit(const QString &capability, const QJsonObject &arguments,
                           QString *error) const;
    int transmissionConfirmationMode() const;
    void recordActionResult(const QString &capability, bool success, const QString &message);

public slots:
    void submitChat(const QString &question, bool includeCapture);
    void clearChat();
    void stopRequest();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

signals:
    void chatLineAdded(const QString &speaker, const QString &text);
    void chatCleared();
    void chatAvailabilityChanged(bool available, const QString &status);
    void chatBusyChanged(bool busy);
    void runtimeStateChanged(const QString &status);
    void actionProposed(const QJsonObject &action);
    void actionsProposed(const QJsonArray &actions);
    void emergencyStopRequested();

private slots:
    void refreshModels();
    void analyzeCapture();
    void handleReply(QNetworkReply *reply);
    void accessModeChanged();
    void startManagedRuntime();
    void stopManagedRuntime();
    void managedRuntimeStarted();
    void managedRuntimeFinished(int exitCode);
    void pullModel();
    void removeModel();
    void modelOperationFinished(int exitCode);
    void sendChatMessage();
    void updateModelButtons();
    void updateResourceStats();
    void gpuStatsFinished(int exitCode);
    void startLiveCapture();
    void pauseLiveCapture();
    void stopLiveCapture();
    void resetLiveCapture();
    void addExperimentMarker();
    void previewEvidence();
    void handleLiveFrames(CANConnection *connection, QVector<CANFrame> &frames);
    void liveAnalysisTick();

private:
    enum class RequestPurpose { None, ModelList, PrimaryAnalysis, Review, Chat, ChatReview };

    void buildUi();
    QSet<quint32> parseIdFilter(bool *ok, QString *error) const;
    QJsonObject buildEvidence(bool *ok, QString *error) const;
    void sendChat(const QString &model, const QString &systemPrompt,
                  const QString &userPrompt, RequestPurpose purpose);
    void appendAudit(const QString &event);
    void setBusy(bool busy);
    QString selectedModel(const QComboBox *combo) const;
    QString managedRuntimeRoot() const;
    QProcessEnvironment managedRuntimeEnvironment() const;
    void startModelOperation(const QStringList &arguments, const QString &description);
    QString capabilityContext(const QString &question) const;
    QString readHelpPage(const QString &fileName, int maximumCharacters) const;
    QJsonObject applicationContext() const;
    void persistChat() const;
    const QVector<CANFrame> *evidenceFrames() const;
    bool acceptLiveFrame(const CANFrame &frame, QByteArray *maskedPayload = nullptr) const;
    int estimatedTokens(const QString &text) const;
    bool checkResourceBudget(const QString &prompt, QString *error) const;
    void updateCaptureStatus();
    void emergencyStop();
    QString compressedChatHistory() const;

    const QVector<CANFrame> *modelFrames;
    QNetworkAccessManager *network;
    QProcess *managedRuntime;
    QProcess *modelOperation;
    QNetworkReply *activeReply = nullptr;
    RequestPurpose requestPurpose = RequestPurpose::None;
    QJsonObject currentEvidence;
    QString primaryResult;
    QString primaryChatResult;
    QString chatHistory;
    QSet<QString> installedModels;
    std::function<QJsonObject()> contextProvider;
    QDateTime accessArmedUntil;
    bool accessArmedIndefinitely = false;
    enum class LiveCaptureState { Stopped, Running, Paused };
    LiveCaptureState liveCaptureState = LiveCaptureState::Stopped;
    QVector<CANFrame> liveCaptureFrames;
    QJsonArray experimentMarkers;
    QTimer *resourceTimer;
    QTimer *liveAnalysisTimer;
    QProcess *gpuStatsProcess;
    quint64 previousCpuTotal = 0;
    quint64 previousCpuIdle = 0;
    qint64 captureStartMs = 0;
    int lastLiveAnalysisFrame = 0;
    QString lastRuntimeOutput;

    QLineEdit *endpointEdit;
    QComboBox *primaryModelCombo;
    QComboBox *reviewModelCombo;
    QLabel *connectionStatus;
    QLabel *runtimeStatus;
    QComboBox *modelManagerCombo;
    QLabel *modelOperationStatus;
    QPushButton *pullModelButton;
    QPushButton *removeModelButton;
    QSpinBox *busSpin;
    QLineEdit *idFilterEdit;
    QSpinBox *frameLimitSpin;
    QComboBox *accessCombo;
    QComboBox *accessDurationCombo;
    QComboBox *confirmationCombo;
    QCheckBox *armAccessCheck;
    QLabel *accessStatus;
    QLabel *resourceStatus;
    QLabel *gpuStatus;
    QLabel *captureStatus;
    QLabel *tokenEstimateLabel;
    QComboBox *captureSourceCombo;
    QComboBox *filterSourceCombo;
    QSpinBox *captureMaxFramesSpin;
    QSpinBox *captureMaxSecondsSpin;
    QCheckBox *ignoreNotchedCheck;
    QCheckBox *autoLiveAnalysisCheck;
    QSpinBox *liveIntervalSpin;
    QSpinBox *meaningfulBitsSpin;
    QLineEdit *markerEdit;
    QPlainTextEdit *instructionEdit;
    QPlainTextEdit *evidenceOutput;
    QPlainTextEdit *resultOutput;
    QPlainTextEdit *auditOutput;
    AIChatTranscript *chatOutput;
    QPlainTextEdit *chatInput;
    QCheckBox *chatCaptureCheck;
    QPushButton *analyzeButton;
    QPushButton *stopButton;
    QPushButton *chatSendButton;
};

#endif
