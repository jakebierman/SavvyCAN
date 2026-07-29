#ifndef AIWORKBENCHWINDOW_H
#define AIWORKBENCHWINDOW_H

#include <QDialog>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
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
    bool previewActionsOnly() const;
    void beginActionBatch(int expectedActions);
    void endActionBatch();
    void recordActionResult(const QString &capability, bool success, const QString &message);
    void recordActionPreview(const QJsonArray &actions);

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
    void clearAudit();
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
    void providerChanged();
    void testOnlineProvider();
    void updateGraphifyStatus();
    void validateSkills();
    void installHeadroom();
    void startHeadroom();
    void stopHeadroom();
    void headroomInstallFinished(int exitCode);
    void headroomStarted();
    void headroomFinished(int exitCode);
    void saveHeadroomKey();

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
    QString locateGraphifyExecutable() const;
    QString locateGraphifyGraph() const;
    QString graphifyContext(const QString &question) const;
    bool usingOnlineProvider() const;
    bool usingOpenAI() const;
    bool usingHeadroom() const;
    QString onlineApiKey() const;
    QString headroomRoot() const;
    QString headroomExecutable() const;
    QString headroomKeyPath() const;
    QString loadHeadroomKey() const;
    void updateHeadroomControls();
    void probeHeadroomReady(int attemptsRemaining);
    void requestHeadroomStats(bool reportRequestDelta);
    void handleHeadroomStatsReply(QNetworkReply *reply);
    bool confirmOnlineRequest(const QString &prompt, RequestPurpose purpose);
    QJsonObject promptApplicationContext(const QString &question = QString()) const;
    QJsonArray selectedCapabilityCatalog;
    QString skillRoutingQuestion;

    const QVector<CANFrame> *modelFrames;
    QNetworkAccessManager *network;
    QProcess *managedRuntime;
    QProcess *modelOperation;
    QProcess *headroomRuntime;
    QProcess *headroomInstaller;
    QNetworkReply *activeReply = nullptr;
    RequestPurpose requestPurpose = RequestPurpose::None;
    QJsonObject currentEvidence;
    QString primaryResult;
    QString primaryChatResult;
    QString chatHistory;
    bool suppressChatTools = false;
    bool headroomReady = false;
    bool headroomStatsReady = false;
    qint64 headroomInputTokens = -1;
    qint64 headroomSavedTokens = 0;
    qint64 headroomAttemptedTokens = 0;
    qint64 headroomCacheReadTokens = 0;
    double headroomSavedUsd = -1.0;
    double headroomCacheSavingsUsd = 0.0;
    bool detailedActionResults = false;
    bool actionBatchActive = false;
    int actionBatchExpected = 0;
    int actionBatchSucceeded = 0;
    int actionBatchFailed = 0;
    QStringList actionBatchFailures;
    QStringList actionBatchChanges;
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
    QComboBox *providerCombo;
    QLineEdit *openAIEndpointEdit;
    QLineEdit *openAIKeyEdit;
    QCheckBox *openAIEnabledCheck;
    QCheckBox *openAIContextCheck;
    QCheckBox *openAICaptureCheck;
    QCheckBox *openAIHistoryCheck;
    QCheckBox *openAIConfirmCheck;
    QCheckBox *openAIStoreCheck;
    QComboBox *headroomModeCombo;
    QPushButton *onlineTestButton;
    QLineEdit *headroomUpstreamKeyEdit;
    QCheckBox *headroomRememberKeyCheck;
    QCheckBox *headroomAutoStartCheck;
    QPushButton *headroomInstallButton;
    QPushButton *headroomStartButton;
    QPushButton *headroomStopButton;
    QLabel *headroomStatus;
    QLabel *openAIStatus;
    QLabel *openAIUsage;
    QCheckBox *graphifyEnabledCheck;
    QCheckBox *graphifyOnlineCheck;
    QLineEdit *graphifyPathEdit;
    QSpinBox *graphifyBudgetSpin;
    QLabel *graphifyStatus;
    QLabel *skillStatus;
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
    QCheckBox *previewActionsCheck;
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
    int headroomInstallStage = 0;
};

#endif
