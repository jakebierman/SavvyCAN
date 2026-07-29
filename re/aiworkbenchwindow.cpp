#include "aiworkbenchwindow.h"
#include "aiactionregistry.h"
#include "aichattranscript.h"
#include "connections/canconmanager.h"
#include "obd2workbenchwindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTextCursor>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <cmath>
#include <cstdio>

#ifdef Q_OS_LINUX
#include <signal.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#endif

namespace {

struct IdEvidence
{
    quint64 count = 0;
    int minLength = 64;
    int maxLength = 0;
    qint64 firstUs = 0;
    qint64 lastUs = 0;
    qint64 previousUs = 0;
    double intervalTotalMs = 0.0;
    quint64 intervals = 0;
    QByteArray firstPayload;
    QByteArray previousPayload;
    QByteArray changedMask;
    QJsonArray samples;
    QSet<QByteArray> uniquePayloads;
    QVector<QMap<int, quint64>> byteHistograms;
    QVector<quint64> bitTransitions;
};

QString frameKey(const CANFrame &frame)
{
    return QStringLiteral("%1:%2:%3").arg(frame.bus).arg(frame.frameId())
        .arg(frame.hasExtendedFrameFormat() ? 1 : 0);
}

QString modelResourceText(const QString &name)
{
    if (name.startsWith(QStringLiteral("qwen3:8b")))
        return QStringLiteral("Qwen3 8B - est. RAM 6-8 GB, download 5.2 GB");
    if (name.startsWith(QStringLiteral("gemma3:4b")))
        return QStringLiteral("Gemma 3 4B - est. RAM 4-6 GB, download 3.3 GB");
    if (name.startsWith(QStringLiteral("qwen2.5-coder:7b")))
        return QStringLiteral("Qwen 2.5 Coder 7B - est. RAM 6-8 GB, download 4.7 GB");
    if (name.startsWith(QStringLiteral("deepseek-r1:7b")))
        return QStringLiteral("DeepSeek R1 7B - est. RAM 6-8 GB, download 4.7 GB");
    if (name.startsWith(QStringLiteral("gpt-oss:20b")))
        return QStringLiteral("GPT-OSS 20B - est. RAM 16+ GB, download 14 GB");
    if (name.startsWith(QStringLiteral("llama3.1:8b")))
        return QStringLiteral("Llama 3.1 8B - est. RAM 6-8 GB, download 4.9 GB");
    if (name.startsWith(QStringLiteral("mistral:7b")))
        return QStringLiteral("Mistral 7B - est. RAM 5-7 GB, download 4.1 GB");
    return name + QStringLiteral(" - RAM depends on model and context");
}

void addModelItem(QComboBox *combo, const QString &name)
{
    combo->addItem(modelResourceText(name), name);
}

void appendNormalizedAction(const QJsonObject &source, QJsonArray *actions)
{
    if (source.value(QStringLiteral("capability")).isString())
    {
        QJsonObject normalized = source;
        QString capability = normalized.value(QStringLiteral("capability")).toString();
        if (capability == QStringLiteral("obd.enable_requests")
            || capability == QStringLiteral("obd.request_enabled"))
        {
            capability = QStringLiteral("obd.execute");
            normalized.insert(QStringLiteral("capability"), capability);
            normalized.insert(QStringLiteral("arguments"), QJsonObject{
                {QStringLiteral("operation"), QStringLiteral("request_enabled")}
            });
        }
        QJsonValue arguments = normalized.value(QStringLiteral("arguments"));
        if (arguments.isUndefined()) arguments = normalized.value(QStringLiteral("args"));
        if (arguments.isString())
        {
            QJsonParseError error;
            const QJsonDocument parsed =
                QJsonDocument::fromJson(arguments.toString().toUtf8(), &error);
            if (error.error == QJsonParseError::NoError && parsed.isObject())
                arguments = parsed.object();
        }
        if (arguments.isUndefined() || arguments.isNull())
            arguments = QJsonObject();
        QJsonObject argumentObject = arguments.toObject();
        capability = normalized.value(QStringLiteral("capability")).toString();
        if (capability == QStringLiteral("obd.configure_pids"))
        {
            QJsonArray resolvedPidActions;
            QStringList unresolved;
            for (const QJsonValue &pidEntry :
                 argumentObject.value(QStringLiteral("pids")).toArray())
            {
                QJsonObject pidArguments;
                if (pidEntry.isObject())
                    pidArguments = pidEntry.toObject();
                else if (pidEntry.isDouble())
                    pidArguments.insert(QStringLiteral("pid"), pidEntry);
                else
                {
                    bool numeric = false;
                    const uint value = pidEntry.toString().toUInt(&numeric, 0);
                    if (numeric && value <= 0xFF)
                        pidArguments.insert(QStringLiteral("pid"), pidEntry);
                    else
                        pidArguments.insert(QStringLiteral("name"), pidEntry.toString());
                }
                QJsonArray candidate;
                appendNormalizedAction(QJsonObject{
                    {QStringLiteral("capability"), QStringLiteral("obd.add_pid")},
                    {QStringLiteral("arguments"), pidArguments}
                }, &candidate);
                const QJsonObject candidateAction =
                    candidate.isEmpty() ? QJsonObject() : candidate.at(0).toObject();
                QString validationError;
                if (candidateAction.isEmpty()
                    || !AIActionRegistry::validate(candidateAction, &validationError))
                    unresolved.append(pidEntry.isString()
                        ? pidEntry.toString() : QStringLiteral("<invalid PID entry>"));
                else
                    resolvedPidActions.append(candidateAction);
            }
            if (!unresolved.isEmpty())
            {
                argumentObject.insert(
                    QStringLiteral("resolution_error"),
                    QStringLiteral("Could not resolve PID description(s): %1")
                        .arg(unresolved.join(QStringLiteral(", "))));
                actions->append(QJsonObject{
                    {QStringLiteral("capability"), QStringLiteral("obd.configure_pids")},
                    {QStringLiteral("arguments"), argumentObject}
                });
                return;
            }
            if (argumentObject.value(QStringLiteral("clear_existing")).toBool())
                actions->append(QJsonObject{
                    {QStringLiteral("capability"), QStringLiteral("obd.clear_pids")},
                    {QStringLiteral("arguments"), QJsonObject()}
                });
            for (const QJsonValue &action : resolvedPidActions) actions->append(action);
            if (argumentObject.value(QStringLiteral("start_polling")).toBool())
                actions->append(QJsonObject{
                    {QStringLiteral("capability"), QStringLiteral("obd.execute")},
                    {QStringLiteral("arguments"), QJsonObject{
                        {QStringLiteral("operation"), QStringLiteral("start_polling")}
                    }}
                });
            return;
        }
        if (capability == QStringLiteral("obd.add_pid")
            || capability == QStringLiteral("obd.query_pid"))
        {
            const QString name = argumentObject.value(QStringLiteral("name")).toString().toLower();
            if (!argumentObject.contains(QStringLiteral("pid"))
                && argumentObject.contains(QStringLiteral("id")))
                argumentObject.insert(QStringLiteral("pid"), argumentObject.value(QStringLiteral("id")));
            bool pidOk = false;
            const QJsonValue proposedPid = argumentObject.value(QStringLiteral("pid"));
            const uint pidValue = proposedPid.isDouble()
                ? uint(qMax(-1, proposedPid.toInt(-1)))
                : proposedPid.toString().toUInt(&pidOk, 0);
            if (proposedPid.isDouble()) pidOk = proposedPid.toInt(-1) >= 0;
            if ((!pidOk || pidValue > 0xFF) && !name.isEmpty())
            {
                int resolvedPid = -1;
                QString resolvedName;
                QString resolvedFormat;
                if (OBD2WorkbenchWindow::resolvePidDescription(
                        name, &resolvedPid, &resolvedName, &resolvedFormat))
                {
                    argumentObject.insert(QStringLiteral("pid"),
                        QStringLiteral("0x%1").arg(resolvedPid, 2, 16, QLatin1Char('0')).toUpper());
                    argumentObject.insert(QStringLiteral("name"), resolvedName);
                    argumentObject.insert(QStringLiteral("format"),
                                          resolvedFormat.isEmpty()
                                              ? QStringLiteral("auto") : resolvedFormat);
                }
            }
            argumentObject.remove(QStringLiteral("id"));
            argumentObject.remove(QStringLiteral("value_type"));
            if (!argumentObject.contains(QStringLiteral("format")))
                argumentObject.insert(QStringLiteral("format"), QStringLiteral("auto"));
            arguments = argumentObject;
        }
        if (capability.startsWith(QStringLiteral("frame.")))
        {
            if (!argumentObject.contains(QStringLiteral("can_id"))
                && argumentObject.contains(QStringLiteral("id")))
                argumentObject.insert(QStringLiteral("can_id"),
                                      argumentObject.value(QStringLiteral("id")));
            if (!argumentObject.contains(QStringLiteral("payload"))
                && argumentObject.value(QStringLiteral("data")).isArray())
            {
                QStringList bytes;
                for (const QJsonValue &value : argumentObject.value(QStringLiteral("data")).toArray())
                    bytes << QStringLiteral("%1").arg(value.toInt() & 0xFF, 2, 16,
                                                      QLatin1Char('0')).toUpper();
                argumentObject.insert(QStringLiteral("payload"), bytes.join(QLatin1Char(' ')));
            }
            if (!argumentObject.contains(QStringLiteral("bus")))
                argumentObject.insert(QStringLiteral("bus"), 0);
            if (!argumentObject.contains(QStringLiteral("extended")))
                argumentObject.insert(QStringLiteral("extended"), false);
            if (capability == QStringLiteral("frame.send_loop")
                && !argumentObject.contains(QStringLiteral("interval_ms")))
                argumentObject.insert(QStringLiteral("interval_ms"), 100);
            argumentObject.remove(QStringLiteral("id"));
            argumentObject.remove(QStringLiteral("data"));
            arguments = argumentObject;
        }
        normalized.remove(QStringLiteral("args"));
        normalized.insert(QStringLiteral("arguments"), arguments);
        actions->append(normalized);
        return;
    }

    const QString type = source.value(QStringLiteral("type")).toString();
    const QJsonObject args = source.value(QStringLiteral("args")).toObject();
    if (type == QStringLiteral("clear_obd_pid_requests")
        || type == QStringLiteral("obd.clear_pids")
        || (type == QStringLiteral("modify_obd_pid_requests")
            && args.value(QStringLiteral("remove_all")).toBool()))
    {
        actions->append(QJsonObject{
            {QStringLiteral("capability"), QStringLiteral("obd.clear_pids")},
            {QStringLiteral("arguments"), QJsonObject()}
        });
    }
    else if (type == QStringLiteral("add_obd_pids"))
    {
        for (const QJsonValue &pid : args.value(QStringLiteral("pids")).toArray())
        {
            actions->append(QJsonObject{
                {QStringLiteral("capability"), QStringLiteral("obd.add_pid")},
                {QStringLiteral("arguments"), QJsonObject{
                    {QStringLiteral("pid"), pid},
                    {QStringLiteral("format"), QStringLiteral("auto")}
                }}
            });
        }
    }
    else if (type == QStringLiteral("savvycan-action")
             && args.contains(QStringLiteral("id"))
             && args.value(QStringLiteral("data")).isArray())
    {
        bool idOk = false;
        quint32 id = args.value(QStringLiteral("id")).toVariant().toUInt(&idOk);
        if (!idOk && args.value(QStringLiteral("id")).isString())
            id = args.value(QStringLiteral("id")).toString().toUInt(&idOk, 0);
        if (!idOk) return;
        QStringList bytes;
        for (const QJsonValue &value : args.value(QStringLiteral("data")).toArray())
            bytes << QStringLiteral("%1").arg(value.toInt() & 0xFF, 2, 16, QLatin1Char('0')).toUpper();
        actions->append(QJsonObject{
            {QStringLiteral("capability"), QStringLiteral("frame.send_once")},
            {QStringLiteral("arguments"), QJsonObject{
                {QStringLiteral("bus"), args.value(QStringLiteral("bus")).toInt(0)},
                {QStringLiteral("can_id"), QStringLiteral("0x%1").arg(id, 0, 16).toUpper()},
                {QStringLiteral("extended"), id > 0x7FF},
                {QStringLiteral("payload"), bytes.join(QLatin1Char(' '))}
            }}
        });
    }
}

QJsonDocument parseActionJson(const QByteArray &source, QJsonParseError *error)
{
    QJsonDocument document = QJsonDocument::fromJson(source, error);
    if (error->error == QJsonParseError::NoError) return document;

    // Local models commonly emit JavaScript-style hexadecimal numbers in
    // otherwise valid JSON. Convert only unquoted literals before retrying.
    QByteArray normalized;
    normalized.reserve(source.size());
    bool quoted = false;
    bool escaped = false;
    for (int index = 0; index < source.size();)
    {
        const char character = source.at(index);
        if (quoted)
        {
            normalized.append(character);
            if (escaped) escaped = false;
            else if (character == '\\') escaped = true;
            else if (character == '"') quoted = false;
            ++index;
            continue;
        }
        if (character == '"')
        {
            quoted = true;
            normalized.append(character);
            ++index;
            continue;
        }
        if (character == '0' && index + 2 < source.size()
            && (source.at(index + 1) == 'x' || source.at(index + 1) == 'X'))
        {
            int end = index + 2;
            while (end < source.size()
                   && ((source.at(end) >= '0' && source.at(end) <= '9')
                       || (source.at(end) >= 'a' && source.at(end) <= 'f')
                       || (source.at(end) >= 'A' && source.at(end) <= 'F')))
                ++end;
            bool ok = false;
            const quint64 value = source.mid(index + 2, end - index - 2).toULongLong(&ok, 16);
            if (ok)
            {
                normalized.append(QByteArray::number(value));
                index = end;
                continue;
            }
        }
        normalized.append(character);
        ++index;
    }
    return QJsonDocument::fromJson(normalized, error);
}

QString capabilityToolName(const QString &capability)
{
    QString name = QStringLiteral("savvycan_") + capability;
    return name.replace(QLatin1Char('.'), QLatin1Char('_'));
}

QJsonObject capabilityTool(const QJsonObject &definition)
{
    QJsonObject properties;
    QJsonArray required;
    const QJsonObject arguments = definition.value(QStringLiteral("arguments")).toObject();
    for (auto iterator = arguments.constBegin(); iterator != arguments.constEnd(); ++iterator)
    {
        const QString description = iterator.value().toString();
        QString type = QStringLiteral("string");
        if (description.contains(QStringLiteral("array"), Qt::CaseInsensitive))
            type = QStringLiteral("array");
        else if (description.contains(QStringLiteral("boolean"), Qt::CaseInsensitive))
            type = QStringLiteral("boolean");
        else if (description.contains(QStringLiteral("integer"), Qt::CaseInsensitive)
                 || description.contains(QRegularExpression(QStringLiteral("^\\d+-\\d+$"))))
            type = QStringLiteral("integer");
        else if (description.contains(QStringLiteral("number"), Qt::CaseInsensitive))
            type = QStringLiteral("number");
        QJsonObject property{
            {QStringLiteral("type"), type},
            {QStringLiteral("description"), description}
        };
        if (type == QStringLiteral("array"))
            property.insert(QStringLiteral("items"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")}
            });
        properties.insert(iterator.key(), property);
        if (!description.contains(QStringLiteral("optional"), Qt::CaseInsensitive))
            required.append(iterator.key());
    }
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("function")},
        {QStringLiteral("function"), QJsonObject{
            {QStringLiteral("name"), capabilityToolName(
                definition.value(QStringLiteral("capability")).toString())},
            {QStringLiteral("description"), definition.value(QStringLiteral("title"))},
            {QStringLiteral("parameters"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("object")},
                {QStringLiteral("properties"), properties},
                {QStringLiteral("required"), required}
            }}
        }}
    };
}

QJsonObject openAIResponseTool(const QJsonObject &definition)
{
    const QJsonObject function =
        capabilityTool(definition).value(QStringLiteral("function")).toObject();
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("function")},
        {QStringLiteral("name"), function.value(QStringLiteral("name"))},
        {QStringLiteral("description"), function.value(QStringLiteral("description"))},
        {QStringLiteral("parameters"), function.value(QStringLiteral("parameters"))}
    };
}

#ifdef Q_OS_LINUX
QList<qint64> processDescendants(qint64 parent)
{
    QList<qint64> result;
    QFile children(QStringLiteral("/proc/%1/task/%1/children").arg(parent));
    if (!children.open(QIODevice::ReadOnly | QIODevice::Text)) return result;
    for (const QByteArray &field : children.readAll().simplified().split(' '))
    {
        bool ok = false;
        const qint64 child = field.toLongLong(&ok);
        if (!ok || child <= 0) continue;
        result.append(child);
        result.append(processDescendants(child));
    }
    return result;
}
#endif

} // namespace

AIWorkbenchWindow::AIWorkbenchWindow(const QVector<CANFrame> *frames, QWidget *parent) :
    QDialog(parent),
    modelFrames(frames),
    network(new QNetworkAccessManager(this)),
    managedRuntime(new QProcess(this)),
    modelOperation(new QProcess(this)),
    headroomRuntime(new QProcess(this)),
    headroomInstaller(new QProcess(this)),
    resourceTimer(new QTimer(this)),
    liveAnalysisTimer(new QTimer(this)),
    gpuStatsProcess(new QProcess(this))
{
    buildUi();
    chatHistory = QSettings().value(QStringLiteral("AIWorkbench/ChatHistory")).toString();
    if (!chatHistory.isEmpty()) chatOutput->loadHistory(chatHistory.trimmed());
    const QString auditPath = QDir(QStandardPaths::writableLocation(
        QStandardPaths::AppDataLocation)).absoluteFilePath(QStringLiteral("ai-audit.jsonl"));
    QFile previousAudit(auditPath);
    if (previousAudit.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        QByteArray data = previousAudit.readAll();
        if (data.size() > 65536) data = data.right(65536);
        QStringList visibleEntries;
        const QList<QByteArray> lines = data.split('\n');
        for (auto iterator = lines.crbegin(); iterator != lines.crend(); ++iterator)
        {
            if (iterator->trimmed().isEmpty()) continue;
            const QJsonDocument entry = QJsonDocument::fromJson(*iterator);
            if (!entry.isObject()) continue;
            const QJsonObject object = entry.object();
            visibleEntries.append(QStringLiteral("%1  %2")
                .arg(object.value(QStringLiteral("timestamp")).toString(),
                     object.value(QStringLiteral("event")).toString()));
        }
        auditOutput->setPlainText(visibleEntries.join(QLatin1Char('\n')));
    }
    connect(network, &QNetworkAccessManager::finished, this, &AIWorkbenchWindow::handleReply);
    connect(managedRuntime, &QProcess::started, this, &AIWorkbenchWindow::managedRuntimeStarted);
    connect(managedRuntime, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus) { managedRuntimeFinished(exitCode); });
    connect(managedRuntime, &QProcess::readyRead, this, [this]() {
        lastRuntimeOutput += QString::fromUtf8(managedRuntime->readAll());
        if (lastRuntimeOutput.size() > 32768) lastRuntimeOutput = lastRuntimeOutput.right(32768);
    });
    connect(modelOperation, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus) { modelOperationFinished(exitCode); });
    connect(headroomInstaller, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus) {
        headroomInstallFinished(exitCode);
    });
    connect(headroomInstaller, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
        {
            headroomInstallStage = 0;
            headroomStatus->setText(tr("Installer could not start"));
            headroomStatus->setToolTip(headroomInstaller->errorString());
            updateHeadroomControls();
        }
    });
    connect(headroomRuntime, &QProcess::started, this, &AIWorkbenchWindow::headroomStarted);
    connect(headroomRuntime, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus) { headroomFinished(exitCode); });
    connect(headroomRuntime, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
        {
            headroomReady = false;
            updateHeadroomControls();
            headroomStatus->setText(tr("Headroom could not start"));
            headroomStatus->setToolTip(headroomRuntime->errorString());
            emit chatAvailabilityChanged(false, tr("Headroom could not start"));
        }
    });
    connect(resourceTimer, &QTimer::timeout, this, &AIWorkbenchWindow::updateResourceStats);
    connect(gpuStatsProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus) { gpuStatsFinished(exitCode); });
    connect(CANConManager::getInstance(), &CANConManager::framesReceived,
            this, &AIWorkbenchWindow::handleLiveFrames);
    connect(liveAnalysisTimer, &QTimer::timeout, this, &AIWorkbenchWindow::liveAnalysisTick);
    resourceTimer->start(2000);
    updateResourceStats();
    if (!usingOnlineProvider()
        && QFileInfo::exists(managedRuntimeRoot() + QStringLiteral("/bin/ollama")))
    {
        startManagedRuntime();
        QTimer::singleShot(700, this, &AIWorkbenchWindow::refreshModels);
    }
    else if (usingHeadroom() && headroomAutoStartCheck->isChecked()
             && QFileInfo::exists(headroomExecutable()))
    {
        startHeadroom();
        QTimer::singleShot(0, this, &AIWorkbenchWindow::refreshModels);
    }
    else
        QTimer::singleShot(0, this, &AIWorkbenchWindow::refreshModels);
}

void AIWorkbenchWindow::setApplicationContextProvider(
    const std::function<QJsonObject()> &provider)
{
    contextProvider = provider;
}

QJsonObject AIWorkbenchWindow::applicationContext() const
{
    return contextProvider ? contextProvider() : QJsonObject();
}

void AIWorkbenchWindow::updateResourceStats()
{
    auto procValueKb = [](const QByteArray &line, const char *key) -> quint64 {
        unsigned long long value = 0;
        QByteArray pattern(key);
        pattern.append(": %llu kB");
        return std::sscanf(line.constData(), pattern.constData(), &value) == 1
            ? quint64(value) : 0;
    };
    QFile statFile(QStringLiteral("/proc/stat"));
    double cpuPercent = 0.0;
    if (statFile.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        const QList<QByteArray> fields = statFile.readLine().simplified().split(' ');
        quint64 total = 0;
        for (int i = 1; i < fields.size(); ++i) total += fields.at(i).toULongLong();
        const quint64 idle = fields.value(4).toULongLong() + fields.value(5).toULongLong();
        if (previousCpuTotal && total > previousCpuTotal)
            cpuPercent = 100.0 * double((total - previousCpuTotal) - (idle - previousCpuIdle))
                / double(total - previousCpuTotal);
        previousCpuTotal = total;
        previousCpuIdle = idle;
    }

    quint64 memoryTotalKb = 0, memoryAvailableKb = 0, appRssKb = 0;
    QFile memoryFile(QStringLiteral("/proc/meminfo"));
    if (memoryFile.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        while (!memoryFile.atEnd())
        {
            const QByteArray line = memoryFile.readLine();
            if (line.startsWith("MemTotal:"))
                memoryTotalKb = procValueKb(line, "MemTotal");
            if (line.startsWith("MemAvailable:"))
                memoryAvailableKb = procValueKb(line, "MemAvailable");
        }
    }
    QFile processFile(QStringLiteral("/proc/self/status"));
    if (processFile.open(QIODevice::ReadOnly | QIODevice::Text))
        while (!processFile.atEnd())
        {
            const QByteArray line = processFile.readLine();
            if (line.startsWith("VmRSS:"))
            {
                appRssKb = procValueKb(line, "VmRSS");
                break;
            }
        }
#ifdef Q_OS_LINUX
    if (memoryTotalKb == 0)
    {
        struct sysinfo information;
        if (sysinfo(&information) == 0)
        {
            const quint64 unit = information.mem_unit;
            memoryTotalKb = quint64(information.totalram) * unit / 1024;
            memoryAvailableKb = (quint64(information.freeram)
                + quint64(information.bufferram)) * unit / 1024;
        }
    }
#endif
    const QStorageInfo disk(QDir::currentPath());
    if (memoryTotalKb > 0 && memoryAvailableKb <= memoryTotalKb)
        resourceStatus->setText(tr("CPU %1% | RAM used %2/%3 GiB, %4 GiB available | SavvyCAN %5 MiB | Disk %6 GiB free")
            .arg(cpuPercent, 0, 'f', 0)
            .arg(double(memoryTotalKb - memoryAvailableKb) / 1048576.0, 0, 'f', 1)
            .arg(double(memoryTotalKb) / 1048576.0, 0, 'f', 1)
            .arg(double(memoryAvailableKb) / 1048576.0, 0, 'f', 1)
            .arg(double(appRssKb) / 1024.0, 0, 'f', 0)
            .arg(double(disk.bytesAvailable()) / 1073741824.0, 0, 'f', 1));
    else
        resourceStatus->setText(tr("CPU %1% | RAM unavailable | SavvyCAN %2 MiB | Disk %3 GiB free")
            .arg(cpuPercent, 0, 'f', 0)
            .arg(double(appRssKb) / 1024.0, 0, 'f', 0)
            .arg(double(disk.bytesAvailable()) / 1073741824.0, 0, 'f', 1));

    if (gpuStatsProcess->state() == QProcess::NotRunning)
    {
        gpuStatsProcess->setProgram(QStringLiteral("nvidia-smi"));
        gpuStatsProcess->setArguments({
            QStringLiteral("--query-gpu=name,utilization.gpu,memory.used,memory.total,temperature.gpu,power.draw"),
            QStringLiteral("--format=csv,noheader,nounits")
        });
        gpuStatsProcess->start();
    }
}

void AIWorkbenchWindow::gpuStatsFinished(int exitCode)
{
    if (exitCode != 0)
    {
        gpuStatus->setText(tr("NVIDIA statistics unavailable"));
        return;
    }
    const QStringList values = QString::fromUtf8(gpuStatsProcess->readAllStandardOutput())
        .trimmed().split(QLatin1Char(','));
    if (values.size() < 6) return;
    gpuStatus->setText(tr("%1 | GPU utilization %2% | Allocated VRAM %3/%4 MB | %5 C | %6 W%7")
        .arg(values.at(0).trimmed(), values.at(1).trimmed(), values.at(2).trimmed(),
             values.at(3).trimmed(), values.at(4).trimmed(), values.at(5).trimmed(),
             activeReply ? tr(" | model request active") : tr(" | idle sample")));
}

void AIWorkbenchWindow::startLiveCapture()
{
    if (liveCaptureState == LiveCaptureState::Stopped)
    {
        captureStartMs = QDateTime::currentMSecsSinceEpoch();
        lastLiveAnalysisFrame = liveCaptureFrames.size();
    }
    liveCaptureState = LiveCaptureState::Running;
    captureSourceCombo->setCurrentIndex(captureSourceCombo->findData(QStringLiteral("live")));
    appendAudit(tr("AI live capture started"));
    updateCaptureStatus();
}

void AIWorkbenchWindow::pauseLiveCapture()
{
    if (liveCaptureState == LiveCaptureState::Running) liveCaptureState = LiveCaptureState::Paused;
    else if (liveCaptureState == LiveCaptureState::Paused) liveCaptureState = LiveCaptureState::Running;
    updateCaptureStatus();
}

void AIWorkbenchWindow::stopLiveCapture()
{
    liveCaptureState = LiveCaptureState::Stopped;
    appendAudit(tr("AI live capture stopped at %1 frames").arg(liveCaptureFrames.size()));
    updateCaptureStatus();
}

void AIWorkbenchWindow::resetLiveCapture()
{
    liveCaptureState = LiveCaptureState::Stopped;
    liveCaptureFrames.clear();
    experimentMarkers = QJsonArray();
    lastLiveAnalysisFrame = 0;
    evidenceOutput->clear();
    appendAudit(tr("AI live capture reset"));
    updateCaptureStatus();
}

void AIWorkbenchWindow::addExperimentMarker()
{
    const QString label = markerEdit->text().trimmed();
    if (label.isEmpty()) return;
    experimentMarkers.append(QJsonObject{
        {QStringLiteral("label"), label},
        {QStringLiteral("timestamp"), QDateTime::currentDateTime().toString(Qt::ISODateWithMs)},
        {QStringLiteral("capture_frame"), liveCaptureFrames.size()}
    });
    markerEdit->clear();
    appendAudit(tr("Experiment marker: %1").arg(label));
}

void AIWorkbenchWindow::previewEvidence()
{
    bool ok = false;
    QString error;
    const QJsonObject evidence = buildEvidence(&ok, &error);
    evidenceOutput->setPlainText(ok
        ? QString::fromUtf8(QJsonDocument(evidence).toJson(QJsonDocument::Indented))
        : error);
}

bool AIWorkbenchWindow::acceptLiveFrame(const CANFrame &frame, QByteArray *maskedPayload) const
{
    bool filterOk = false;
    QString filterError;
    const QSet<quint32> aiIds = parseIdFilter(&filterOk, &filterError);
    if (!filterOk || (busSpin->value() >= 0 && int(frame.bus) != busSpin->value())) return false;
    const QJsonObject context = applicationContext();
    QSet<quint32> mainIds;
    for (const QJsonValue &value : context.value(QStringLiteral("main_filter_ids")).toArray())
        mainIds.insert(value.toString().mid(2).toUInt(nullptr, 16));
    QSet<quint32> snifferIds;
    QByteArray notchMask;
    for (const QJsonValue &value : context.value(QStringLiteral("sniffer")).toObject()
             .value(QStringLiteral("items")).toArray())
    {
        const QJsonObject item = value.toObject();
        const quint32 id = item.value(QStringLiteral("id")).toString().mid(2).toUInt(nullptr, 16);
        snifferIds.insert(id);
        if (id == frame.frameId()) notchMask = QByteArray::fromHex(
            item.value(QStringLiteral("notch_mask")).toString().toLatin1());
    }
    const QString source = filterSourceCombo->currentData().toString();
    if ((source == QStringLiteral("ai") || source == QStringLiteral("combined"))
        && !aiIds.isEmpty() && !aiIds.contains(frame.frameId())) return false;
    if ((source == QStringLiteral("main") || source == QStringLiteral("combined"))
        && !mainIds.isEmpty() && !mainIds.contains(frame.frameId())) return false;
    if ((source == QStringLiteral("sniffer") || source == QStringLiteral("combined"))
        && !snifferIds.contains(frame.frameId())) return false;
    if (maskedPayload)
    {
        *maskedPayload = frame.payload();
        if (ignoreNotchedCheck->isChecked())
            for (int i = 0; i < qMin(maskedPayload->size(), notchMask.size()); ++i)
                (*maskedPayload)[i] = char(quint8(maskedPayload->at(i)) & ~quint8(notchMask.at(i)));
    }
    return true;
}

void AIWorkbenchWindow::handleLiveFrames(CANConnection *, QVector<CANFrame> &frames)
{
    if (liveCaptureState != LiveCaptureState::Running) return;
    for (const CANFrame &incoming : frames)
    {
        QByteArray payload;
        if (!acceptLiveFrame(incoming, &payload)) continue;
        CANFrame frame = incoming;
        frame.setPayload(payload);
        liveCaptureFrames.append(frame);
    }
    const int overflow = liveCaptureFrames.size() - captureMaxFramesSpin->value();
    if (overflow > 0) liveCaptureFrames.remove(0, overflow);
    if (captureStartMs && QDateTime::currentMSecsSinceEpoch() - captureStartMs
        >= qint64(captureMaxSecondsSpin->value()) * 1000)
        stopLiveCapture();
    updateCaptureStatus();
}

void AIWorkbenchWindow::updateCaptureStatus()
{
    const QString state = liveCaptureState == LiveCaptureState::Running ? tr("Recording")
        : liveCaptureState == LiveCaptureState::Paused ? tr("Paused") : tr("Stopped");
    captureStatus->setText(tr("%1 - %2 frames, %3 markers")
        .arg(state).arg(liveCaptureFrames.size()).arg(experimentMarkers.size()));
}

void AIWorkbenchWindow::liveAnalysisTick()
{
    if (!autoLiveAnalysisCheck->isChecked() || activeReply
        || liveCaptureFrames.size() <= lastLiveAnalysisFrame) return;
    int changedBits = 0;
    QMap<quint32, QByteArray> previous;
    for (int i = qMax(0, lastLiveAnalysisFrame - 1); i < liveCaptureFrames.size(); ++i)
    {
        const CANFrame &frame = liveCaptureFrames.at(i);
        const QByteArray payload = frame.payload();
        if (previous.contains(frame.frameId()))
            for (int byte = 0; byte < qMin(payload.size(), previous[frame.frameId()].size()); ++byte)
                changedBits += __builtin_popcount(
                    quint8(payload.at(byte)) ^ quint8(previous[frame.frameId()].at(byte)));
        previous[frame.frameId()] = payload;
    }
    if (changedBits < meaningfulBitsSpin->value()) return;
    lastLiveAnalysisFrame = liveCaptureFrames.size();
    submitChat(tr("Review the latest live Sniffer changes since the previous update. "
                  "Update hypotheses and recommend the next controlled experiment."), true);
}

const QVector<CANFrame> *AIWorkbenchWindow::evidenceFrames() const
{
    return captureSourceCombo && captureSourceCombo->currentData().toString() == QStringLiteral("live")
        ? &liveCaptureFrames : modelFrames;
}

int AIWorkbenchWindow::estimatedTokens(const QString &text) const
{
    return qMax(1, int(std::ceil(text.size() / 3.5)));
}

bool AIWorkbenchWindow::checkResourceBudget(const QString &prompt, QString *error) const
{
    const int tokens = estimatedTokens(prompt);
    if (usingOnlineProvider())
    {
        const int onlineLimit = 100000;
        if (tokens > onlineLimit)
        {
            if (error) *error = tr(
                "Estimated prompt size is %1 tokens; SavvyCAN's network upload limit is %2.")
                                    .arg(tokens).arg(onlineLimit);
            return false;
        }
        return true;
    }
    const QString model = selectedModel(primaryModelCombo);
    const int conservativeLimit = model.contains(QStringLiteral("20b"), Qt::CaseInsensitive)
        ? 12000 : 24000;
    if (tokens > conservativeLimit)
    {
        if (error) *error = tr("Estimated prompt size is %1 tokens; the local safety limit for %2 is %3.")
            .arg(tokens).arg(model).arg(conservativeLimit);
        return false;
    }
    return true;
}

void AIWorkbenchWindow::emergencyStop()
{
    emit emergencyStopRequested();
    stopRequest();
    stopLiveCapture();
    liveAnalysisTimer->stop();
    autoLiveAnalysisCheck->setChecked(false);
    armAccessCheck->setChecked(false);
    stopManagedRuntime();
    appendAudit(tr("Emergency stop activated"));
}

void AIWorkbenchWindow::persistChat() const
{
    QSettings().setValue(QStringLiteral("AIWorkbench/ChatHistory"), chatHistory.right(48000));
}

QString AIWorkbenchWindow::compressedChatHistory() const
{
    if (chatHistory.size() <= 28000) return chatHistory;
    QStringList retained;
    const QString older = chatHistory.left(chatHistory.size() - 20000);
    for (const QString &line : older.split(QLatin1Char('\n')))
        if (line.startsWith(QStringLiteral("Application:"))
            || line.contains(QStringLiteral("confirmed"), Qt::CaseInsensitive)
            || line.contains(QStringLiteral("hypothesis"), Qt::CaseInsensitive))
            retained << line.left(500);
    QString summary = QStringLiteral("[Older conversation compressed; %1 characters omitted.]\n")
        .arg(older.size());
    if (!retained.isEmpty())
        summary += retained.mid(qMax(0, retained.size() - 20)).join(QLatin1Char('\n'))
            + QLatin1Char('\n');
    return summary + chatHistory.right(20000);
}

QString AIWorkbenchWindow::locateGraphifyExecutable() const
{
    const QString fromPath = QStandardPaths::findExecutable(QStringLiteral("graphify"));
    if (!fromPath.isEmpty()) return fromPath;
    const QString applicationDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        QDir::current().absoluteFilePath(QStringLiteral("local-ai/graphify/bin/graphify")),
        QDir(applicationDir).absoluteFilePath(QStringLiteral("local-ai/graphify/bin/graphify")),
        QDir(applicationDir).absoluteFilePath(QStringLiteral("../local-ai/graphify/bin/graphify")),
        QDir(applicationDir).absoluteFilePath(QStringLiteral("../../local-ai/graphify/bin/graphify"))
    };
    for (const QString &candidate : candidates)
        if (QFileInfo(candidate).isExecutable()) return QDir::cleanPath(candidate);
    return QString();
}

QString AIWorkbenchWindow::locateGraphifyGraph() const
{
    const QString configured = graphifyPathEdit
        ? graphifyPathEdit->text().trimmed() : QString();
    if (!configured.isEmpty())
    {
        QString expanded = configured;
        if (expanded == QStringLiteral("~"))
            expanded = QDir::homePath();
        else if (expanded.startsWith(QStringLiteral("~/")))
            expanded = QDir::home().absoluteFilePath(expanded.mid(2));
        return QDir::cleanPath(QFileInfo(expanded).absoluteFilePath());
    }

    const QString applicationDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        QDir::current().absoluteFilePath(QStringLiteral("graphify-out/graph.json")),
        QDir(applicationDir).absoluteFilePath(QStringLiteral("graphify-out/graph.json")),
        QDir(applicationDir).absoluteFilePath(QStringLiteral("../graphify-out/graph.json")),
        QDir(applicationDir).absoluteFilePath(QStringLiteral("../../graphify-out/graph.json"))
    };
    for (const QString &candidate : candidates)
        if (QFileInfo::exists(candidate)) return QDir::cleanPath(candidate);
    return QString();
}

void AIWorkbenchWindow::updateGraphifyStatus()
{
    if (!graphifyStatus) return;
    const bool enabled = graphifyEnabledCheck->isChecked();
    graphifyPathEdit->setEnabled(enabled);
    graphifyBudgetSpin->setEnabled(enabled);
    graphifyOnlineCheck->setEnabled(enabled);
    if (!enabled)
    {
        graphifyStatus->setText(tr("Disabled. Application skills and live state remain available."));
        return;
    }

    const QString graphPath = locateGraphifyGraph();
    if (graphPath.isEmpty() || !QFileInfo::exists(graphPath))
    {
        graphifyStatus->setText(tr(
            "No graph found. Generate graphify-out/graph.json or select an existing graph."));
        return;
    }
    const QString executable = locateGraphifyExecutable();
    const QString reportPath =
        QDir(QFileInfo(graphPath).absolutePath()).absoluteFilePath(QStringLiteral("GRAPH_REPORT.md"));
    if (!executable.isEmpty())
        graphifyStatus->setText(tr("Ready: bounded Graphify queries will use %1")
                                    .arg(QDir::toNativeSeparators(graphPath)));
    else if (QFileInfo::exists(reportPath))
        graphifyStatus->setText(tr(
            "Graphify CLI unavailable; bounded GRAPH_REPORT.md context will be used."));
    else
        graphifyStatus->setText(tr(
            "Graph found, but the Graphify CLI and GRAPH_REPORT.md are unavailable."));
}

void AIWorkbenchWindow::validateSkills()
{
    const QJsonObject diagnostics =
        AIActionRegistry::skillDiagnostics(applicationContext());
    const int passed = diagnostics.value(QStringLiteral("evaluations_passed")).toInt();
    const int total = diagnostics.value(QStringLiteral("evaluations_total")).toInt();
    const int uncovered =
        diagnostics.value(QStringLiteral("uncovered_capabilities")).toArray().size();
    const int unknown = diagnostics
        .value(QStringLiteral("unknown_capability_references")).toArray().size();
    const bool valid = diagnostics.value(QStringLiteral("valid")).toBool();
    skillStatus->setText(valid
        ? tr("v%1: %2 skills, %3 capabilities; %4/%5 routing checks passed")
              .arg(diagnostics.value(QStringLiteral("version")).toString())
              .arg(diagnostics.value(QStringLiteral("skill_count")).toInt())
              .arg(diagnostics.value(QStringLiteral("capability_count")).toInt())
              .arg(passed).arg(total)
        : tr("v%1: %2/%3 routing checks passed; %4 uncovered and %5 unknown capabilities")
              .arg(diagnostics.value(QStringLiteral("version")).toString())
              .arg(passed).arg(total).arg(uncovered).arg(unknown));
    skillStatus->setToolTip(QString::fromUtf8(
        QJsonDocument(diagnostics).toJson(QJsonDocument::Indented)));
    appendAudit(tr("Skill validation v%1: %2/%3 evaluations passed, "
                   "%4 uncovered, %5 unknown")
        .arg(diagnostics.value(QStringLiteral("version")).toString())
        .arg(passed).arg(total).arg(uncovered).arg(unknown));
}

QString AIWorkbenchWindow::graphifyContext(const QString &question) const
{
    if (!graphifyEnabledCheck || !graphifyEnabledCheck->isChecked()) return QString();
    if (usingOnlineProvider()
        && (!graphifyOnlineCheck || !graphifyOnlineCheck->isChecked()))
        return QString();
    const QRegularExpression sourceQuestion(
        QStringLiteral("\\b(source|source code|codebase|class|function|method|implementation|"
                       "implemented|architecture|dependency|call graph|file|compile|build)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    if (!sourceQuestion.match(question).hasMatch()) return QString();

    const QString graphPath = locateGraphifyGraph();
    if (graphPath.isEmpty() || !QFileInfo::exists(graphPath)) return QString();
    const QString executable = locateGraphifyExecutable();
    if (!executable.isEmpty())
    {
        QProcess query;
        query.setProgram(executable);
        query.setArguments({
            QStringLiteral("query"),
            question,
            QStringLiteral("--budget"),
            QString::number(graphifyBudgetSpin->value()),
            QStringLiteral("--graph"),
            graphPath
        });
        query.start();
        if (query.waitForStarted(1000) && query.waitForFinished(3500)
            && query.exitStatus() == QProcess::NormalExit && query.exitCode() == 0)
        {
            const QString output = QString::fromUtf8(query.readAllStandardOutput()).trimmed();
            if (!output.isEmpty())
                return QStringLiteral(
                    "Graphify relevant repository subgraph. Treat EXTRACTED edges as parsed "
                    "facts and INFERRED or AMBIGUOUS edges as hypotheses:\n%1")
                    .arg(output.left(16000));
        }
        if (query.state() != QProcess::NotRunning)
        {
            query.kill();
            query.waitForFinished(500);
        }
    }

    QFile report(QDir(QFileInfo(graphPath).absolutePath())
                     .absoluteFilePath(QStringLiteral("GRAPH_REPORT.md")));
    if (report.open(QIODevice::ReadOnly | QIODevice::Text))
        return QStringLiteral(
            "Graphify repository report fallback. This is broad orientation rather than a "
            "question-specific graph traversal:\n%1")
            .arg(QString::fromUtf8(report.read(8000)));
    return QString();
}

bool AIWorkbenchWindow::authorizeTransmit(const QString &capability,
                                          const QJsonObject &, QString *error) const
{
    const AIActionRegistry::Risk risk =
        AIActionRegistry::risk(AIActionRegistry::definition(capability));
    if (risk != AIActionRegistry::ConfirmSend && risk != AIActionRegistry::ArmedConfirmSend)
        return true;
    if (accessCombo->currentIndex() != 1 || !armAccessCheck->isChecked()
        || (!accessArmedIndefinitely && QDateTime::currentDateTime() >= accessArmedUntil))
    {
        if (error) *error = tr("Full bus access is not armed or has expired.");
        return false;
    }
    return true;
}

int AIWorkbenchWindow::transmissionConfirmationMode() const
{
    return confirmationCombo->currentData().toInt();
}

bool AIWorkbenchWindow::previewActionsOnly() const
{
    return previewActionsCheck && previewActionsCheck->isChecked();
}

void AIWorkbenchWindow::beginActionBatch(int expectedActions)
{
    actionBatchActive = !detailedActionResults;
    actionBatchExpected = expectedActions;
    actionBatchSucceeded = 0;
    actionBatchFailed = 0;
    actionBatchFailures.clear();
    actionBatchChanges.clear();
}

void AIWorkbenchWindow::endActionBatch()
{
    if (!actionBatchActive)
    {
        detailedActionResults = false;
        return;
    }

    const int completed = actionBatchSucceeded + actionBatchFailed;
    QString result = tr("Completed %1 action(s): %2 succeeded, %3 failed.")
        .arg(completed).arg(actionBatchSucceeded).arg(actionBatchFailed);
    if (completed != actionBatchExpected)
        result += tr(" %1 action(s) were proposed.").arg(actionBatchExpected);
    if (!actionBatchFailures.isEmpty())
    {
        const int shown = qMin(5, actionBatchFailures.size());
        result += tr("\nFailures:\n- %1")
            .arg(actionBatchFailures.mid(0, shown).join(QStringLiteral("\n- ")));
        if (actionBatchFailures.size() > shown)
            result += tr("\n- ...and %1 more failure(s).")
                .arg(actionBatchFailures.size() - shown);
    }
    if (!actionBatchChanges.isEmpty())
    {
        const int shown = qMin(5, actionBatchChanges.size());
        result += tr("\nApplied:\n- %1")
            .arg(actionBatchChanges.mid(0, shown).join(QStringLiteral("\n- ")));
        if (actionBatchChanges.size() > shown)
            result += tr("\n- ...and %1 more successful action(s).")
                .arg(actionBatchChanges.size() - shown);
    }

    chatOutput->addSystemMessage(result);
    emit chatLineAdded(tr("System"), result);
    chatHistory += QStringLiteral("\nApplication: ") + result;
    persistChat();

    actionBatchActive = false;
    detailedActionResults = false;
    actionBatchExpected = 0;
    actionBatchFailures.clear();
    actionBatchChanges.clear();
}

void AIWorkbenchWindow::recordActionResult(const QString &capability, bool success,
                                           const QString &message)
{
    const QString result = tr("Action %1: %2 (%3)")
        .arg(capability, success ? tr("success") : tr("failed"), message);
    appendAudit(result);
    if (actionBatchActive)
    {
        if (success)
        {
            ++actionBatchSucceeded;
            actionBatchChanges.append(tr("%1: %2").arg(capability, message));
        }
        else
        {
            ++actionBatchFailed;
            actionBatchFailures.append(tr("%1: %2").arg(capability, message));
        }
        return;
    }
    chatOutput->addSystemMessage(result);
    emit chatLineAdded(tr("System"), result);
    chatHistory += QStringLiteral("\nApplication: ") + result;
    persistChat();
}

void AIWorkbenchWindow::recordActionPreview(const QJsonArray &actions)
{
    QStringList capabilities;
    for (const QJsonValue &value : actions)
    {
        const QJsonObject action = value.toObject();
        const QString capability = action.value(QStringLiteral("capability")).toString();
        capabilities.append(capability);
        appendAudit(tr("Preview action %1")
            .arg(QString::fromUtf8(QJsonDocument(action).toJson(QJsonDocument::Compact))));
    }
    const QString result = tr(
        "Previewed %1 validated action(s): %2. Preview only; no GUI state or CAN traffic changed.")
        .arg(actions.size()).arg(capabilities.join(QStringLiteral(", ")));
    chatOutput->addSystemMessage(result);
    emit chatLineAdded(tr("System"), result);
    chatHistory += QStringLiteral("\nApplication: ") + result;
    persistChat();
}

AIWorkbenchWindow::~AIWorkbenchWindow()
{
    if (modelOperation->state() != QProcess::NotRunning)
    {
        modelOperation->terminate();
        if (!modelOperation->waitForFinished(2000)) modelOperation->kill();
    }
    if (headroomInstaller->state() != QProcess::NotRunning)
    {
        headroomInstaller->terminate();
        if (!headroomInstaller->waitForFinished(2000)) headroomInstaller->kill();
    }
    stopHeadroom();
    stopManagedRuntime();
}

void AIWorkbenchWindow::buildUi()
{
    setWindowTitle(tr("AI Analysis Workbench"));
    QVBoxLayout *root = new QVBoxLayout(this);
    QTabWidget *configurationTabs = new QTabWidget(this);
    configurationTabs->setDocumentMode(true);

    QGroupBox *runtime = new QGroupBox(this);
    QFormLayout *runtimeLayout = new QFormLayout(runtime);
    providerCombo = new QComboBox(runtime);
    providerCombo->addItem(tr("Local - Ollama"), QStringLiteral("ollama"));
    providerCombo->addItem(tr("Online - OpenAI API"), QStringLiteral("openai"));
    providerCombo->addItem(tr("Gateway - Headroom / OpenAI compatible"),
                           QStringLiteral("headroom"));
    const int providerIndex = providerCombo->findData(QSettings().value(
        QStringLiteral("AIWorkbench/Provider"), QStringLiteral("ollama")).toString());
    providerCombo->setCurrentIndex(providerIndex >= 0 ? providerIndex : 0);
    runtimeLayout->addRow(tr("Provider"), providerCombo);
    QWidget *endpointRow = new QWidget(runtime);
    QHBoxLayout *endpointLayout = new QHBoxLayout(endpointRow);
    endpointLayout->setContentsMargins(0, 0, 0, 0);
    endpointEdit = new QLineEdit(QSettings().value(
        QStringLiteral("AIWorkbench/OllamaEndpoint"), QStringLiteral("http://127.0.0.1:11434")).toString(),
        endpointRow);
    QPushButton *refreshButton = new QPushButton(tr("Refresh models"), endpointRow);
    connectionStatus = new QLabel(tr("Not checked"), endpointRow);
    endpointLayout->addWidget(endpointEdit, 1);
    endpointLayout->addWidget(refreshButton);
    endpointLayout->addWidget(connectionStatus);
    runtimeLayout->addRow(tr("Ollama endpoint"), endpointRow);
    QWidget *managedRow = new QWidget(runtime);
    QHBoxLayout *managedLayout = new QHBoxLayout(managedRow);
    managedLayout->setContentsMargins(0, 0, 0, 0);
    QPushButton *startRuntimeButton = new QPushButton(tr("Start bundled runtime"), managedRow);
    QPushButton *stopRuntimeButton = new QPushButton(tr("Stop"), managedRow);
    runtimeStatus = new QLabel(managedRow);
    managedLayout->addWidget(startRuntimeButton);
    managedLayout->addWidget(stopRuntimeButton);
    managedLayout->addWidget(runtimeStatus, 1);
    runtimeLayout->addRow(tr("Project runtime"), managedRow);
    primaryModelCombo = new QComboBox(runtime);
    primaryModelCombo->setEditable(true);
    reviewModelCombo = new QComboBox(runtime);
    reviewModelCombo->setEditable(true);
    reviewModelCombo->addItem(tr("None"));
    runtimeLayout->addRow(tr("Analysis model"), primaryModelCombo);
    runtimeLayout->addRow(tr("Review model"), reviewModelCombo);
    QWidget *modelManagerRow = new QWidget(runtime);
    QHBoxLayout *modelManagerLayout = new QHBoxLayout(modelManagerRow);
    modelManagerLayout->setContentsMargins(0, 0, 0, 0);
    modelManagerCombo = new QComboBox(modelManagerRow);
    modelManagerCombo->setEditable(true);
    const QStringList suggestedModels = {
        QStringLiteral("qwen3:8b"),
        QStringLiteral("gemma3:4b"),
        QStringLiteral("qwen2.5-coder:7b"),
        QStringLiteral("deepseek-r1:7b"),
        QStringLiteral("gpt-oss:20b"),
        QStringLiteral("llama3.1:8b"),
        QStringLiteral("mistral:7b")
    };
    for (const QString &model : suggestedModels) addModelItem(modelManagerCombo, model);
    modelManagerCombo->setToolTip(tr(
        "Estimated runtime RAM varies with context length and GPU offload. "
        "Select a suggestion or enter any Ollama model tag."));
    pullModelButton = new QPushButton(tr("Download"), modelManagerRow);
    pullModelButton->setToolTip(tr("Download the selected model into local-ai/models for offline use"));
    removeModelButton = new QPushButton(tr("Remove"), modelManagerRow);
    modelOperationStatus = new QLabel(modelManagerRow);
    modelManagerLayout->addWidget(modelManagerCombo, 1);
    modelManagerLayout->addWidget(pullModelButton);
    modelManagerLayout->addWidget(removeModelButton);
    modelManagerLayout->addWidget(modelOperationStatus);
    runtimeLayout->addRow(tr("Manage models"), modelManagerRow);
    configurationTabs->addTab(runtime, tr("Models"));

    QGroupBox *online = new QGroupBox(this);
    QFormLayout *onlineLayout = new QFormLayout(online);
    openAIEndpointEdit = new QLineEdit(online);
    openAIKeyEdit = new QLineEdit(online);
    openAIKeyEdit->setEchoMode(QLineEdit::Password);
    openAIKeyEdit->setPlaceholderText(tr(
        "Session only; otherwise use OPENAI_API_KEY or HEADROOM_API_KEY"));
    openAIEnabledCheck = new QCheckBox(
        tr("Allow requests to the selected network provider"), online);
    openAIEnabledCheck->setChecked(QSettings().value(
        QStringLiteral("AIWorkbench/OnlineEnabled"), false).toBool());
    openAIContextCheck = new QCheckBox(tr("Include current application state"), online);
    openAIContextCheck->setChecked(QSettings().value(
        QStringLiteral("AIWorkbench/OnlineIncludeContext"), true).toBool());
    openAICaptureCheck = new QCheckBox(tr("Permit filtered CAN evidence uploads"), online);
    openAICaptureCheck->setChecked(QSettings().value(
        QStringLiteral("AIWorkbench/OnlineIncludeCapture"), false).toBool());
    openAIHistoryCheck = new QCheckBox(tr("Include compressed chat history"), online);
    openAIHistoryCheck->setChecked(QSettings().value(
        QStringLiteral("AIWorkbench/OnlineIncludeHistory"), true).toBool());
    openAIConfirmCheck = new QCheckBox(
        tr("Confirm each online API call (show complete prompt preview)"), online);
    openAIConfirmCheck->setChecked(QSettings().value(
        QStringLiteral("AIWorkbench/OnlineConfirm"), true).toBool());
    openAIConfirmCheck->setToolTip(tr(
        "Disable this to send without a popup. A selected Review model makes a "
        "second API call after the primary response."));
    openAIStoreCheck = new QCheckBox(tr("Allow provider-side response storage"), online);
    openAIStoreCheck->setChecked(QSettings().value(
        QStringLiteral("AIWorkbench/OpenAIStore"), false).toBool());
    headroomModeCombo = new QComboBox(online);
    headroomModeCombo->addItem(tr("Token reduction"), QStringLiteral("token"));
    headroomModeCombo->addItem(tr("Cache stability"), QStringLiteral("cache"));
    const int headroomModeIndex = headroomModeCombo->findData(QSettings().value(
        QStringLiteral("AIWorkbench/HeadroomMode"), QStringLiteral("token")).toString());
    headroomModeCombo->setCurrentIndex(headroomModeIndex >= 0 ? headroomModeIndex : 0);
    headroomUpstreamKeyEdit = new QLineEdit(online);
    headroomUpstreamKeyEdit->setEchoMode(QLineEdit::Password);
    headroomUpstreamKeyEdit->setPlaceholderText(
        tr("OpenAI key used by the managed local Headroom proxy"));
    headroomRememberKeyCheck = new QCheckBox(
        tr("Remember upstream key on this computer"), online);
    headroomRememberKeyCheck->setChecked(QSettings().value(
        QStringLiteral("AIWorkbench/HeadroomRememberKey"), false).toBool());
    if (headroomRememberKeyCheck->isChecked())
        headroomUpstreamKeyEdit->setText(loadHeadroomKey());
    QWidget *headroomRow = new QWidget(online);
    QHBoxLayout *headroomLayout = new QHBoxLayout(headroomRow);
    headroomLayout->setContentsMargins(0, 0, 0, 0);
    headroomInstallButton = new QPushButton(tr("Install"), headroomRow);
    headroomStartButton = new QPushButton(tr("Start"), headroomRow);
    headroomStopButton = new QPushButton(tr("Stop"), headroomRow);
    headroomAutoStartCheck = new QCheckBox(tr("Auto-start"), headroomRow);
    headroomAutoStartCheck->setChecked(QSettings().value(
        QStringLiteral("AIWorkbench/HeadroomAutoStart"), true).toBool());
    headroomStatus = new QLabel(headroomRow);
    headroomLayout->addWidget(headroomInstallButton);
    headroomLayout->addWidget(headroomStartButton);
    headroomLayout->addWidget(headroomStopButton);
    headroomLayout->addWidget(headroomAutoStartCheck);
    headroomLayout->addWidget(headroomStatus, 1);
    openAIStatus = new QLabel(online);
    openAIStatus->setWordWrap(true);
    openAIUsage = new QLabel(tr("No network usage this session"), online);
    onlineTestButton = new QPushButton(tr("Test connection"), online);
    onlineLayout->addRow(QString(), openAIEnabledCheck);
    onlineLayout->addRow(tr("Endpoint"), openAIEndpointEdit);
    onlineLayout->addRow(tr("API key"), openAIKeyEdit);
    onlineLayout->addRow(tr("Headroom mode"), headroomModeCombo);
    onlineLayout->addRow(tr("Headroom upstream key"), headroomUpstreamKeyEdit);
    onlineLayout->addRow(QString(), headroomRememberKeyCheck);
    onlineLayout->addRow(tr("Managed Headroom"), headroomRow);
    onlineLayout->addRow(QString(), onlineTestButton);
    onlineLayout->addRow(QString(), openAIContextCheck);
    onlineLayout->addRow(QString(), openAICaptureCheck);
    onlineLayout->addRow(QString(), openAIHistoryCheck);
    onlineLayout->addRow(QString(), openAIConfirmCheck);
    onlineLayout->addRow(QString(), openAIStoreCheck);
    onlineLayout->addRow(tr("Privacy"), openAIStatus);
    onlineLayout->addRow(tr("Usage"), openAIUsage);
    configurationTabs->addTab(online, tr("Online Provider"));

    QGroupBox *context = new QGroupBox(this);
    QFormLayout *contextLayout = new QFormLayout(context);
    graphifyEnabledCheck = new QCheckBox(
        tr("Use Graphify repository context when available"), context);
    graphifyEnabledCheck->setChecked(QSettings().value(
        QStringLiteral("AIWorkbench/GraphifyEnabled"), true).toBool());
    graphifyOnlineCheck = new QCheckBox(
        tr("Permit retrieved Graphify context in network requests"), context);
    graphifyOnlineCheck->setChecked(QSettings().value(
        QStringLiteral("AIWorkbench/GraphifyOnline"), false).toBool());
    QWidget *graphPathRow = new QWidget(context);
    QHBoxLayout *graphPathLayout = new QHBoxLayout(graphPathRow);
    graphPathLayout->setContentsMargins(0, 0, 0, 0);
    graphifyPathEdit = new QLineEdit(QSettings().value(
        QStringLiteral("AIWorkbench/GraphifyPath")).toString(), graphPathRow);
    graphifyPathEdit->setPlaceholderText(tr("Auto-detect graphify-out/graph.json"));
    QPushButton *graphifyBrowseButton = new QPushButton(tr("Browse"), graphPathRow);
    graphPathLayout->addWidget(graphifyPathEdit, 1);
    graphPathLayout->addWidget(graphifyBrowseButton);
    graphifyBudgetSpin = new QSpinBox(context);
    graphifyBudgetSpin->setRange(200, 4000);
    graphifyBudgetSpin->setSingleStep(200);
    graphifyBudgetSpin->setSuffix(tr(" tokens"));
    graphifyBudgetSpin->setValue(QSettings().value(
        QStringLiteral("AIWorkbench/GraphifyBudget"), 1200).toInt());
    graphifyStatus = new QLabel(context);
    graphifyStatus->setWordWrap(true);
    QPushButton *validateSkillsButton = new QPushButton(tr("Validate skills"), context);
    skillStatus = new QLabel(context);
    skillStatus->setWordWrap(true);
    contextLayout->addRow(QString(), graphifyEnabledCheck);
    contextLayout->addRow(tr("Graph file"), graphPathRow);
    contextLayout->addRow(tr("Query budget"), graphifyBudgetSpin);
    contextLayout->addRow(QString(), graphifyOnlineCheck);
    contextLayout->addRow(tr("Status"), graphifyStatus);
    contextLayout->addRow(QString(), validateSkillsButton);
    contextLayout->addRow(tr("Skill bundle"), skillStatus);
    configurationTabs->addTab(context, tr("Context"));

    QGroupBox *scope = new QGroupBox(this);
    QFormLayout *scopeLayout = new QFormLayout(scope);
    busSpin = new QSpinBox(scope);
    busSpin->setRange(-1, 255);
    busSpin->setSpecialValueText(tr("All buses"));
    idFilterEdit = new QLineEdit(scope);
    idFilterEdit->setPlaceholderText(tr("All IDs, or 123, 7E0-7EF, 18DAF100"));
    frameLimitSpin = new QSpinBox(scope);
    frameLimitSpin->setRange(100, 1000000);
    frameLimitSpin->setValue(50000);
    frameLimitSpin->setSingleStep(1000);
    accessCombo = new QComboBox(scope);
    accessCombo->addItem(tr("Read only"));
    accessCombo->addItem(tr("Full bus access"));
    accessCombo->setCurrentIndex(qBound(
        0, QSettings().value(QStringLiteral("AIWorkbench/AccessMode"), 0).toInt(),
        accessCombo->count() - 1));
    accessDurationCombo = new QComboBox(scope);
    accessDurationCombo->addItem(tr("1 minute"), 60);
    accessDurationCombo->addItem(tr("5 minutes"), 300);
    accessDurationCombo->addItem(tr("15 minutes"), 900);
    accessDurationCombo->addItem(tr("1 hour"), 3600);
    accessDurationCombo->addItem(tr("Indefinite"), 0);
    accessDurationCombo->setCurrentIndex(qBound(
        0, QSettings().value(QStringLiteral("AIWorkbench/ArmDurationIndex"), 1).toInt(),
        accessDurationCombo->count() - 1));
    confirmationCombo = new QComboBox(scope);
    confirmationCombo->addItem(tr("Every transmission"), 0);
    confirmationCombo->addItem(tr("Once per workflow"), 1);
    confirmationCombo->addItem(tr("No popups while armed"), 2);
    confirmationCombo->setCurrentIndex(qBound(
        0, QSettings().value(QStringLiteral("AIWorkbench/ConfirmationMode"), 0).toInt(),
        confirmationCombo->count() - 1));
    confirmationCombo->setToolTip(tr(
        "Controls only CAN and diagnostic transmission approval popups. "
        "Online prompt privacy confirmation is configured in Online Provider. "
        "Access checks and auditing always remain active."));
    armAccessCheck = new QCheckBox(tr("Arm full access"), scope);
    previewActionsCheck = new QCheckBox(
        tr("Preview AI actions without applying them"), scope);
    previewActionsCheck->setChecked(QSettings().value(
        QStringLiteral("AIWorkbench/PreviewActions"), false).toBool());
    accessStatus = new QLabel(scope);
    QWidget *accessRow = new QWidget(scope);
    QHBoxLayout *accessLayout = new QHBoxLayout(accessRow);
    accessLayout->setContentsMargins(0, 0, 0, 0);
    accessLayout->addWidget(accessCombo);
    accessLayout->addWidget(armAccessCheck);
    accessLayout->addWidget(accessStatus, 1);
    scopeLayout->addRow(tr("Bus"), busSpin);
    scopeLayout->addRow(tr("CAN-ID allowlist"), idFilterEdit);
    scopeLayout->addRow(tr("Recent frame limit"), frameLimitSpin);
    scopeLayout->addRow(tr("Arm duration"), accessDurationCombo);
    scopeLayout->addRow(tr("Confirm CAN transmissions"), confirmationCombo);
    scopeLayout->addRow(QString(), previewActionsCheck);
    scopeLayout->addRow(tr("Model access"), accessRow);
    configurationTabs->addTab(scope, tr("Scope && Access"));

    QGroupBox *resources = new QGroupBox(this);
    QFormLayout *resourceLayout = new QFormLayout(resources);
    resourceStatus = new QLabel(tr("Reading CPU, RAM and disk..."), resources);
    gpuStatus = new QLabel(tr("Reading GPU..."), resources);
    tokenEstimateLabel = new QLabel(tr("Estimated prompt: 0 tokens"), resources);
    resourceLayout->addRow(tr("System"), resourceStatus);
    resourceLayout->addRow(tr("GPU"), gpuStatus);
    resourceLayout->addRow(tr("Context"), tokenEstimateLabel);

    captureSourceCombo = new QComboBox(resources);
    captureSourceCombo->addItem(tr("Main capture"), QStringLiteral("main"));
    captureSourceCombo->addItem(tr("AI live buffer"), QStringLiteral("live"));
    filterSourceCombo = new QComboBox(resources);
    filterSourceCombo->addItem(tr("AI allowlist"), QStringLiteral("ai"));
    filterSourceCombo->addItem(tr("Main-window filters"), QStringLiteral("main"));
    filterSourceCombo->addItem(tr("Sniffer filters"), QStringLiteral("sniffer"));
    filterSourceCombo->addItem(tr("Combined"), QStringLiteral("combined"));
    resourceLayout->addRow(tr("Analyze source"), captureSourceCombo);
    resourceLayout->addRow(tr("Live filter"), filterSourceCombo);

    QWidget *limitsRow = new QWidget(resources);
    QHBoxLayout *limits = new QHBoxLayout(limitsRow);
    limits->setContentsMargins(0, 0, 0, 0);
    captureMaxFramesSpin = new QSpinBox(limitsRow);
    captureMaxFramesSpin->setRange(100, 1000000);
    captureMaxFramesSpin->setValue(20000);
    captureMaxSecondsSpin = new QSpinBox(limitsRow);
    captureMaxSecondsSpin->setRange(1, 86400);
    captureMaxSecondsSpin->setValue(300);
    limits->addWidget(new QLabel(tr("Frames"), limitsRow));
    limits->addWidget(captureMaxFramesSpin);
    limits->addWidget(new QLabel(tr("Seconds"), limitsRow));
    limits->addWidget(captureMaxSecondsSpin);
    resourceLayout->addRow(tr("Buffer limits"), limitsRow);

    QWidget *captureButtonsRow = new QWidget(resources);
    QHBoxLayout *captureButtons = new QHBoxLayout(captureButtonsRow);
    captureButtons->setContentsMargins(0, 0, 0, 0);
    QPushButton *captureStart = new QPushButton(tr("Start"), captureButtonsRow);
    QPushButton *capturePause = new QPushButton(tr("Pause / Resume"), captureButtonsRow);
    QPushButton *captureStop = new QPushButton(tr("Stop"), captureButtonsRow);
    QPushButton *captureReset = new QPushButton(tr("Reset"), captureButtonsRow);
    QPushButton *capturePreview = new QPushButton(tr("Preview"), captureButtonsRow);
    captureButtons->addWidget(captureStart);
    captureButtons->addWidget(capturePause);
    captureButtons->addWidget(captureStop);
    captureButtons->addWidget(captureReset);
    captureButtons->addWidget(capturePreview);
    resourceLayout->addRow(tr("AI capture"), captureButtonsRow);
    captureStatus = new QLabel(tr("Stopped - 0 frames"), resources);
    resourceLayout->addRow(tr("Status"), captureStatus);

    ignoreNotchedCheck = new QCheckBox(tr("Ignore Sniffer-notched bits"), resources);
    ignoreNotchedCheck->setChecked(true);
    autoLiveAnalysisCheck = new QCheckBox(tr("Analyze meaningful changes automatically"), resources);
    liveIntervalSpin = new QSpinBox(resources);
    liveIntervalSpin->setRange(1, 300);
    liveIntervalSpin->setValue(5);
    liveIntervalSpin->setSuffix(tr(" s"));
    meaningfulBitsSpin = new QSpinBox(resources);
    meaningfulBitsSpin->setRange(1, 512);
    meaningfulBitsSpin->setValue(1);
    resourceLayout->addRow(QString(), ignoreNotchedCheck);
    resourceLayout->addRow(QString(), autoLiveAnalysisCheck);
    resourceLayout->addRow(tr("Live interval"), liveIntervalSpin);
    resourceLayout->addRow(tr("Minimum changed bits"), meaningfulBitsSpin);

    QWidget *markerRow = new QWidget(resources);
    QHBoxLayout *markerLayout = new QHBoxLayout(markerRow);
    markerLayout->setContentsMargins(0, 0, 0, 0);
    markerEdit = new QLineEdit(markerRow);
    markerEdit->setPlaceholderText(tr("Experiment marker, e.g. brake pressed"));
    QPushButton *markerButton = new QPushButton(tr("Add marker"), markerRow);
    markerLayout->addWidget(markerEdit, 1);
    markerLayout->addWidget(markerButton);
    resourceLayout->addRow(tr("Marker"), markerRow);
    configurationTabs->addTab(resources, tr("Live Capture"));
    root->addWidget(configurationTabs);

    instructionEdit = new QPlainTextEdit(this);
    instructionEdit->setPlaceholderText(tr("Describe the experiment or what you want decoded."));
    instructionEdit->setPlainText(tr(
        "Identify likely signals, counters, state bits, scaling hypotheses and useful next experiments. "
        "Explain the evidence for every proposal and return candidate SavvyCAN formatter expressions."));
    instructionEdit->setMaximumHeight(110);
    root->addWidget(instructionEdit);

    QHBoxLayout *commands = new QHBoxLayout;
    analyzeButton = new QPushButton(tr("Analyze capture"), this);
    stopButton = new QPushButton(tr("Stop"), this);
    QPushButton *emergencyButton = new QPushButton(tr("Emergency stop"), this);
    stopButton->setEnabled(false);
    commands->addWidget(analyzeButton);
    commands->addWidget(stopButton);
    commands->addWidget(emergencyButton);
    commands->addStretch();
    root->addLayout(commands);

    QTabWidget *outputs = new QTabWidget(this);
    QWidget *chatPanel = new QWidget(outputs);
    QVBoxLayout *chatLayout = new QVBoxLayout(chatPanel);
    chatLayout->setContentsMargins(4, 4, 4, 4);
    chatOutput = new AIChatTranscript(chatPanel);
    chatInput = new QPlainTextEdit(chatPanel);
    chatInput->setPlaceholderText(tr("Message the selected AI provider..."));
    chatInput->setMaximumHeight(90);
    QHBoxLayout *chatCommands = new QHBoxLayout;
    chatCaptureCheck = new QCheckBox(tr("Include capture snapshot"), chatPanel);
    chatSendButton = new QPushButton(tr("Send"), chatPanel);
    QPushButton *clearChatButton = new QPushButton(tr("Clear"), chatPanel);
    chatCommands->addWidget(chatCaptureCheck);
    chatCommands->addStretch();
    chatCommands->addWidget(clearChatButton);
    chatCommands->addWidget(chatSendButton);
    chatLayout->addWidget(chatOutput, 1);
    chatLayout->addWidget(chatInput);
    chatLayout->addLayout(chatCommands);
    evidenceOutput = new QPlainTextEdit(outputs);
    resultOutput = new QPlainTextEdit(outputs);
    QWidget *auditPanel = new QWidget(outputs);
    QVBoxLayout *auditLayout = new QVBoxLayout(auditPanel);
    auditLayout->setContentsMargins(4, 4, 4, 4);
    auditOutput = new QPlainTextEdit(auditPanel);
    QPushButton *clearAuditButton = new QPushButton(tr("Clear audit log"), auditPanel);
    clearAuditButton->setToolTip(tr(
        "Clear the visible audit history and delete the persisted audit log"));
    QHBoxLayout *auditCommands = new QHBoxLayout;
    auditCommands->addStretch();
    auditCommands->addWidget(clearAuditButton);
    auditLayout->addWidget(auditOutput, 1);
    auditLayout->addLayout(auditCommands);
    evidenceOutput->setReadOnly(true);
    resultOutput->setReadOnly(true);
    auditOutput->setReadOnly(true);
    outputs->addTab(chatPanel, tr("Chat"));
    outputs->addTab(resultOutput, tr("Analysis"));
    outputs->addTab(evidenceOutput, tr("Evidence"));
    outputs->addTab(auditPanel, tr("Audit"));
    root->addWidget(outputs, 1);

    connect(refreshButton, &QPushButton::clicked, this, &AIWorkbenchWindow::refreshModels);
    connect(providerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AIWorkbenchWindow::providerChanged);
    connect(startRuntimeButton, &QPushButton::clicked, this, &AIWorkbenchWindow::startManagedRuntime);
    connect(stopRuntimeButton, &QPushButton::clicked, this, &AIWorkbenchWindow::stopManagedRuntime);
    connect(pullModelButton, &QPushButton::clicked, this, &AIWorkbenchWindow::pullModel);
    connect(removeModelButton, &QPushButton::clicked, this, &AIWorkbenchWindow::removeModel);
    connect(modelManagerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AIWorkbenchWindow::updateModelButtons);
    connect(modelManagerCombo->lineEdit(), &QLineEdit::textChanged,
            this, &AIWorkbenchWindow::updateModelButtons);
    connect(analyzeButton, &QPushButton::clicked, this, &AIWorkbenchWindow::analyzeCapture);
    connect(stopButton, &QPushButton::clicked, this, &AIWorkbenchWindow::stopRequest);
    connect(emergencyButton, &QPushButton::clicked, this, &AIWorkbenchWindow::emergencyStop);
    connect(chatSendButton, &QPushButton::clicked, this, &AIWorkbenchWindow::sendChatMessage);
    chatInput->installEventFilter(this);
    connect(clearChatButton, &QPushButton::clicked, this, &AIWorkbenchWindow::clearChat);
    connect(clearAuditButton, &QPushButton::clicked, this, &AIWorkbenchWindow::clearAudit);
    connect(captureStart, &QPushButton::clicked, this, &AIWorkbenchWindow::startLiveCapture);
    connect(capturePause, &QPushButton::clicked, this, &AIWorkbenchWindow::pauseLiveCapture);
    connect(captureStop, &QPushButton::clicked, this, &AIWorkbenchWindow::stopLiveCapture);
    connect(captureReset, &QPushButton::clicked, this, &AIWorkbenchWindow::resetLiveCapture);
    connect(capturePreview, &QPushButton::clicked, this, &AIWorkbenchWindow::previewEvidence);
    connect(markerButton, &QPushButton::clicked, this, &AIWorkbenchWindow::addExperimentMarker);
    connect(autoLiveAnalysisCheck, &QCheckBox::toggled, this, [this](bool enabled) {
        if (enabled) liveAnalysisTimer->start(liveIntervalSpin->value() * 1000);
        else liveAnalysisTimer->stop();
    });
    connect(liveIntervalSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int seconds) {
        if (autoLiveAnalysisCheck->isChecked()) liveAnalysisTimer->start(seconds * 1000);
    });
    connect(primaryModelCombo, &QComboBox::currentTextChanged, this, [this](const QString &model) {
        if (model.isEmpty()) return;
        QSettings().setValue(usingOnlineProvider()
            ? QStringLiteral("AIWorkbench/OnlinePrimaryModel")
            : QStringLiteral("AIWorkbench/PrimaryModel"), model);
    });
    connect(reviewModelCombo, &QComboBox::currentTextChanged, this, [this](const QString &model) {
        if (model.isEmpty()) return;
        QSettings().setValue(usingOnlineProvider()
            ? QStringLiteral("AIWorkbench/OnlineReviewModel")
            : QStringLiteral("AIWorkbench/ReviewModel"), model);
    });
    connect(accessCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AIWorkbenchWindow::accessModeChanged);
    connect(accessCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [](int index) {
        QSettings().setValue(QStringLiteral("AIWorkbench/AccessMode"), index);
    });
    connect(accessDurationCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        QSettings().setValue(QStringLiteral("AIWorkbench/ArmDurationIndex"), index);
        accessModeChanged();
    });
    connect(confirmationCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [](int index) {
        QSettings().setValue(QStringLiteral("AIWorkbench/ConfirmationMode"), index);
    });
    connect(openAIEnabledCheck, &QCheckBox::toggled, this, [this](bool enabled) {
        QSettings().setValue(QStringLiteral("AIWorkbench/OnlineEnabled"), enabled);
        connectionStatus->setText(enabled
            ? tr("Network provider configured; test not run")
            : tr("Network provider disabled"));
        emit chatAvailabilityChanged(enabled,
            enabled ? tr("Ready - use Test connection to verify provider")
                    : tr("Network provider disabled"));
    });
    connect(openAIContextCheck, &QCheckBox::toggled, this, [](bool enabled) {
        QSettings().setValue(QStringLiteral("AIWorkbench/OnlineIncludeContext"), enabled);
    });
    connect(openAICaptureCheck, &QCheckBox::toggled, this, [](bool enabled) {
        QSettings().setValue(QStringLiteral("AIWorkbench/OnlineIncludeCapture"), enabled);
    });
    connect(openAIHistoryCheck, &QCheckBox::toggled, this, [](bool enabled) {
        QSettings().setValue(QStringLiteral("AIWorkbench/OnlineIncludeHistory"), enabled);
    });
    connect(openAIConfirmCheck, &QCheckBox::toggled, this, [](bool enabled) {
        QSettings().setValue(QStringLiteral("AIWorkbench/OnlineConfirm"), enabled);
    });
    connect(openAIStoreCheck, &QCheckBox::toggled, this, [](bool enabled) {
        QSettings().setValue(QStringLiteral("AIWorkbench/OpenAIStore"), enabled);
    });
    connect(headroomModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this]() {
        QSettings().setValue(QStringLiteral("AIWorkbench/HeadroomMode"),
                             headroomModeCombo->currentData().toString());
        if (headroomRuntime->state() != QProcess::NotRunning)
            headroomStatus->setText(tr("Running; restart to apply mode"));
    });
    connect(headroomInstallButton, &QPushButton::clicked,
            this, &AIWorkbenchWindow::installHeadroom);
    connect(headroomStartButton, &QPushButton::clicked,
            this, &AIWorkbenchWindow::startHeadroom);
    connect(headroomStopButton, &QPushButton::clicked,
            this, &AIWorkbenchWindow::stopHeadroom);
    connect(headroomAutoStartCheck, &QCheckBox::toggled, this, [](bool enabled) {
        QSettings().setValue(QStringLiteral("AIWorkbench/HeadroomAutoStart"), enabled);
    });
    connect(headroomRememberKeyCheck, &QCheckBox::toggled, this, [this](bool remember) {
        QSettings().setValue(QStringLiteral("AIWorkbench/HeadroomRememberKey"), remember);
        saveHeadroomKey();
    });
    connect(headroomUpstreamKeyEdit, &QLineEdit::editingFinished,
            this, &AIWorkbenchWindow::saveHeadroomKey);
    connect(onlineTestButton, &QPushButton::clicked,
            this, &AIWorkbenchWindow::testOnlineProvider);
    connect(openAIEndpointEdit, &QLineEdit::editingFinished, this, [this]() {
        const QString key = usingHeadroom()
            ? QStringLiteral("AIWorkbench/HeadroomEndpoint")
            : QStringLiteral("AIWorkbench/OpenAIEndpoint");
        QSettings().setValue(key, openAIEndpointEdit->text().trimmed());
    });
    connect(graphifyEnabledCheck, &QCheckBox::toggled, this, [this](bool enabled) {
        QSettings().setValue(QStringLiteral("AIWorkbench/GraphifyEnabled"), enabled);
        updateGraphifyStatus();
    });
    connect(graphifyOnlineCheck, &QCheckBox::toggled, this, [](bool enabled) {
        QSettings().setValue(QStringLiteral("AIWorkbench/GraphifyOnline"), enabled);
    });
    connect(graphifyPathEdit, &QLineEdit::editingFinished, this, [this]() {
        QSettings().setValue(QStringLiteral("AIWorkbench/GraphifyPath"),
                             graphifyPathEdit->text().trimmed());
        updateGraphifyStatus();
    });
    connect(graphifyBudgetSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int budget) {
        QSettings().setValue(QStringLiteral("AIWorkbench/GraphifyBudget"), budget);
    });
    connect(graphifyBrowseButton, &QPushButton::clicked, this, [this]() {
        const QString current = locateGraphifyGraph();
        const QString selected = QFileDialog::getOpenFileName(
            this, tr("Select Graphify graph"),
            current.isEmpty() ? QDir::currentPath() : current,
            tr("Graphify graph (graph.json);;JSON files (*.json);;All files (*)"));
        if (selected.isEmpty()) return;
        graphifyPathEdit->setText(QDir::cleanPath(selected));
        QSettings().setValue(QStringLiteral("AIWorkbench/GraphifyPath"),
                             graphifyPathEdit->text());
        updateGraphifyStatus();
    });
    connect(validateSkillsButton, &QPushButton::clicked,
            this, &AIWorkbenchWindow::validateSkills);
    connect(previewActionsCheck, &QCheckBox::toggled, this, [](bool enabled) {
        QSettings().setValue(QStringLiteral("AIWorkbench/PreviewActions"), enabled);
    });
    connect(armAccessCheck, &QCheckBox::toggled, this, [this](bool checked) {
        QSettings().setValue(QStringLiteral("AIWorkbench/ArmChecked"), checked);
        accessModeChanged();
    });
    const bool restoreIndefiniteArm =
        accessCombo->currentIndex() == 1
        && accessDurationCombo->currentData().toInt() == 0
        && QSettings().value(QStringLiteral("AIWorkbench/ArmChecked"), false).toBool();
    armAccessCheck->setChecked(restoreIndefiniteArm);
    accessModeChanged();
    runtimeStatus->setText(QFileInfo::exists(managedRuntimeRoot() + QStringLiteral("/bin/ollama"))
        ? tr("Installed, stopped") : tr("Not installed in local-ai"));
    updateHeadroomControls();
    updateGraphifyStatus();
    validateSkills();
    providerChanged();
    updateModelButtons();
}

bool AIWorkbenchWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == chatInput && event->type() == QEvent::KeyPress)
    {
        QKeyEvent *key = static_cast<QKeyEvent *>(event);
        const bool isEnter = key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter;
        const Qt::KeyboardModifiers blocked =
            Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier;
        if (isEnter && !(key->modifiers() & blocked))
        {
            chatSendButton->click();
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

bool AIWorkbenchWindow::usingOnlineProvider() const
{
    return providerCombo
        && providerCombo->currentData().toString() != QStringLiteral("ollama");
}

bool AIWorkbenchWindow::usingOpenAI() const
{
    return providerCombo
        && providerCombo->currentData().toString() == QStringLiteral("openai");
}

bool AIWorkbenchWindow::usingHeadroom() const
{
    return providerCombo
        && providerCombo->currentData().toString() == QStringLiteral("headroom");
}

QString AIWorkbenchWindow::onlineApiKey() const
{
    const QString entered = openAIKeyEdit ? openAIKeyEdit->text().trimmed() : QString();
    if (usingHeadroom())
    {
        const QUrl endpoint(openAIEndpointEdit ? openAIEndpointEdit->text().trimmed()
                                               : QString());
        const bool localEndpoint = endpoint.host() == QStringLiteral("127.0.0.1")
            || endpoint.host() == QStringLiteral("localhost")
            || endpoint.host() == QStringLiteral("::1");
        const bool managedProxy = localEndpoint && headroomRuntime
            && headroomRuntime->state() != QProcess::NotRunning;
        if (managedProxy)
        {
            QString upstreamKey = headroomUpstreamKeyEdit
                ? headroomUpstreamKeyEdit->text().trimmed() : QString();
            if (upstreamKey.isEmpty())
                upstreamKey = qEnvironmentVariable("OPENAI_API_KEY").trimmed();
            if (!upstreamKey.isEmpty()) return upstreamKey;
        }
        if (!entered.isEmpty()) return entered;
        return qEnvironmentVariable("HEADROOM_API_KEY").trimmed();
    }
    if (!entered.isEmpty()) return entered;
    return qEnvironmentVariable("OPENAI_API_KEY").trimmed();
}

QJsonObject AIWorkbenchWindow::promptApplicationContext(const QString &question) const
{
    const QJsonObject full = applicationContext();
    if (usingOnlineProvider() && !openAIContextCheck->isChecked())
        return QJsonObject{
        {QStringLiteral("online_context_withheld"), true},
        {QStringLiteral("active_workspace"), full.value(QStringLiteral("active_workspace"))}
        };

    QJsonObject scoped;
    const QStringList commonKeys = {
        QStringLiteral("active_workspace"),
        QStringLiteral("connected_buses")
    };
    for (const QString &key : commonKeys)
        if (full.contains(key)) scoped.insert(key, full.value(key));

    const QString routingQuestion =
        skillRoutingQuestion.isEmpty() ? question : skillRoutingQuestion;
    const QStringList selected = AIActionRegistry::matchingSkills(routingQuestion, full);
    auto copy = [&full, &scoped](const QString &key) {
        if (full.contains(key)) scoped.insert(key, full.value(key));
    };
    if (selected.contains(QStringLiteral("raw_can")))
        copy(QStringLiteral("selected_frame"));
    if (selected.contains(QStringLiteral("savvycan_interface")))
    {
        const QStringList traceKeys = {
            QStringLiteral("capture_running"), QStringLiteral("frame_count"),
            QStringLiteral("overwrite_mode"), QStringLiteral("dbc_interpretation"),
            QStringLiteral("payload_mode"), QStringLiteral("payload_format"),
            QStringLiteral("enabled_id_filters"), QStringLiteral("main_filter_ids"),
            QStringLiteral("selected_frame")
        };
        for (const QString &key : traceKeys) copy(key);
        if (question.contains(QStringLiteral("sniff"), Qt::CaseInsensitive)
            || question.contains(QStringLiteral("notch"), Qt::CaseInsensitive))
            copy(QStringLiteral("sniffer"));
    }
    if (selected.contains(QStringLiteral("connections")))
        copy(QStringLiteral("connections"));
    if (selected.contains(QStringLiteral("obd")))
        copy(QStringLiteral("obd"));
    if (selected.contains(QStringLiteral("uds")))
        copy(QStringLiteral("uds"));
    if (selected.contains(QStringLiteral("dbc_signals")))
    {
        copy(QStringLiteral("dbc_files"));
        copy(QStringLiteral("selected_frame"));
        copy(QStringLiteral("sniffer"));
    }
    if (selected.contains(QStringLiteral("reverse_engineering")))
    {
        copy(QStringLiteral("sniffer"));
        copy(QStringLiteral("selected_frame"));
        copy(QStringLiteral("dbc_files"));
    }
    scoped.insert(QStringLiteral("selected_skills"), QJsonArray::fromStringList(selected));
    scoped.insert(QStringLiteral("skill_bundle_version"),
                  AIActionRegistry::skillVersion());
    return scoped;
}

bool AIWorkbenchWindow::confirmOnlineRequest(const QString &prompt,
                                             RequestPurpose purpose)
{
    if (!usingOnlineProvider()) return true;
    if (!openAIEnabledCheck->isChecked())
    {
        chatOutput->addSystemMessage(tr(
            "Network requests are disabled. Enable them in Online Provider."));
        return false;
    }
    if (usingOpenAI() && onlineApiKey().isEmpty())
    {
        chatOutput->addSystemMessage(tr(
            "OpenAI API key unavailable. Set OPENAI_API_KEY or enter a session key."));
        return false;
    }
    QUrl endpoint(openAIEndpointEdit->text().trimmed());
    const bool localGateway = endpoint.host() == QStringLiteral("127.0.0.1")
        || endpoint.host() == QStringLiteral("localhost")
        || endpoint.host() == QStringLiteral("::1");
    if (!endpoint.isValid() || endpoint.host().isEmpty()
        || (endpoint.scheme() != QStringLiteral("https") && !localGateway))
    {
        chatOutput->addSystemMessage(tr(
            "Network endpoint must use HTTPS unless it is a localhost gateway."));
        return false;
    }
    if (!openAIConfirmCheck->isChecked()) return true;

    appendAudit(tr("Showing online prompt confirmation because "
                   "'Confirm each online API call' is enabled"));
    QMessageBox preview(this);
    preview.setIcon(QMessageBox::Warning);
    preview.setWindowTitle(tr("Confirm AI network request"));
    preview.setText(tr("Send approximately %1 tokens to %2 using %3?")
        .arg(estimatedTokens(prompt))
        .arg(endpoint.host(), selectedModel(
            purpose == RequestPurpose::Review || purpose == RequestPurpose::ChatReview
                ? reviewModelCombo : primaryModelCombo)));
    preview.setInformativeText(tr(
        "The detailed text is the exact prompt payload before protocol wrapping. "
        "SavvyCAN tool execution and bus permissions remain local."));
    preview.setDetailedText(prompt);
    preview.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    preview.setDefaultButton(QMessageBox::No);
    return preview.exec() == QMessageBox::Yes;
}

void AIWorkbenchWindow::providerChanged()
{
    if (activeReply && requestPurpose == RequestPurpose::ModelList)
    {
        QNetworkReply *obsoleteReply = activeReply;
        activeReply = nullptr;
        requestPurpose = RequestPurpose::None;
        obsoleteReply->setProperty("aiIgnoreReply", true);
        obsoleteReply->abort();
        setBusy(false);
        appendAudit(tr("Cancelled obsolete local model check after provider change"));
    }

    const bool online = usingOnlineProvider();
    QSettings settings;
    settings.setValue(QStringLiteral("AIWorkbench/Provider"),
                      providerCombo->currentData().toString());
    updateGraphifyStatus();

    openAIEndpointEdit->setEnabled(online);
    openAIKeyEdit->setEnabled(online);
    openAIEnabledCheck->setEnabled(online);
    openAIContextCheck->setEnabled(online);
    openAICaptureCheck->setEnabled(online);
    openAIHistoryCheck->setEnabled(online);
    openAIConfirmCheck->setEnabled(online);
    openAIStoreCheck->setEnabled(usingOpenAI());
    onlineTestButton->setEnabled(online);
    headroomModeCombo->setEnabled(usingHeadroom());
    headroomUpstreamKeyEdit->setEnabled(usingHeadroom());
    headroomRememberKeyCheck->setEnabled(usingHeadroom());
    headroomAutoStartCheck->setEnabled(usingHeadroom());
    updateHeadroomControls();

    const QSignalBlocker primaryBlocker(primaryModelCombo);
    const QSignalBlocker reviewBlocker(reviewModelCombo);
    primaryModelCombo->clear();
    reviewModelCombo->clear();
    reviewModelCombo->addItem(tr("None"), QString());

    if (online)
    {
        const QString defaultEndpoint = usingHeadroom()
            ? QStringLiteral("http://127.0.0.1:8787")
            : QStringLiteral("https://api.openai.com/v1");
        openAIEndpointEdit->setText(settings.value(usingHeadroom()
            ? QStringLiteral("AIWorkbench/HeadroomEndpoint")
            : QStringLiteral("AIWorkbench/OpenAIEndpoint"), defaultEndpoint).toString());
        const QStringList onlineModels = {
            QStringLiteral("gpt-5.6-luna"),
            QStringLiteral("gpt-5.6-terra"),
            QStringLiteral("gpt-5.6-sol")
        };
        for (const QString &model : onlineModels)
        {
            primaryModelCombo->addItem(model, model);
            reviewModelCombo->addItem(model, model);
        }
        const QString primary = settings.value(
            QStringLiteral("AIWorkbench/OnlinePrimaryModel"),
            QStringLiteral("gpt-5.6-luna")).toString();
        const QString review = settings.value(
            QStringLiteral("AIWorkbench/OnlineReviewModel")).toString();
        int index = primaryModelCombo->findData(primary);
        if (index >= 0) primaryModelCombo->setCurrentIndex(index);
        else primaryModelCombo->setEditText(primary);
        index = reviewModelCombo->findData(review);
        reviewModelCombo->setCurrentIndex(index >= 0 ? index : 0);
        openAIStatus->setText(usingHeadroom()
            ? tr("Headroom may forward data to its configured upstream provider. "
                 "SavvyCAN sends OpenAI Responses API requests to the gateway.")
            : tr("Direct OpenAI Responses API. API keys are read from the environment "
                 "or held only in this process."));
        connectionStatus->setText(openAIEnabledCheck->isChecked()
            ? tr("Network provider ready") : tr("Network provider disabled"));
        runtimeStatus->setText(tr("Local runtime not used by selected provider"));
        if (usingHeadroom() && headroomAutoStartCheck->isChecked()
            && QFileInfo::exists(headroomExecutable())
            && headroomRuntime->state() == QProcess::NotRunning)
        {
            QTimer::singleShot(0, this, &AIWorkbenchWindow::startHeadroom);
        }
        const bool available = openAIEnabledCheck->isChecked()
            && (!usingHeadroom() || headroomReady);
        emit chatAvailabilityChanged(available,
            available ? tr("Ready - network model selected")
                      : openAIEnabledCheck->isChecked()
                          ? tr("Waiting for Headroom")
                          : tr("Network provider disabled"));
    }
    else
    {
        QList<QString> models = installedModels.values();
        std::sort(models.begin(), models.end());
        for (const QString &model : models)
        {
            addModelItem(primaryModelCombo, model);
            addModelItem(reviewModelCombo, model);
        }
        const QString primary = settings.value(
            QStringLiteral("AIWorkbench/PrimaryModel"), QStringLiteral("qwen3:8b")).toString();
        const QString review = settings.value(
            QStringLiteral("AIWorkbench/ReviewModel"), QStringLiteral("gemma3:4b")).toString();
        int index = primaryModelCombo->findData(primary);
        if (index >= 0) primaryModelCombo->setCurrentIndex(index);
        else primaryModelCombo->setEditText(primary);
        index = reviewModelCombo->findData(review);
        reviewModelCombo->setCurrentIndex(index >= 0 ? index : 0);
        openAIStatus->setText(tr("Local provider selected; no prompt data leaves SavvyCAN."));
        runtimeStatus->setText(QFileInfo::exists(
            managedRuntimeRoot() + QStringLiteral("/bin/ollama"))
            ? tr("Installed, stopped") : tr("Not installed in local-ai"));
    }
    appendAudit(tr("AI provider selected: %1").arg(providerCombo->currentText()));
}

void AIWorkbenchWindow::testOnlineProvider()
{
    if (!usingOnlineProvider()) return;
    if (!openAIEnabledCheck->isChecked())
    {
        openAIStatus->setText(tr(
            "Enable requests to the selected network provider before testing."));
        emit chatAvailabilityChanged(false, tr("Network provider disabled"));
        return;
    }
    if (usingOpenAI() && onlineApiKey().isEmpty())
    {
        openAIStatus->setText(tr("Enter an OpenAI API key before testing."));
        emit chatAvailabilityChanged(false, tr("OpenAI API key unavailable"));
        return;
    }

    QUrl configured(openAIEndpointEdit->text().trimmed());
    if (!configured.isValid() || configured.host().isEmpty())
    {
        openAIStatus->setText(tr("The provider endpoint is invalid."));
        emit chatAvailabilityChanged(false, tr("Invalid network endpoint"));
        return;
    }

    QString endpoint = configured.toString();
    while (endpoint.endsWith(QLatin1Char('/'))) endpoint.chop(1);
    if (usingHeadroom())
    {
        if (endpoint.endsWith(QStringLiteral("/v1"))) endpoint.chop(3);
        endpoint += QStringLiteral("/health");
    }
    else
    {
        if (!endpoint.endsWith(QStringLiteral("/v1"))) endpoint += QStringLiteral("/v1");
        endpoint += QStringLiteral("/models");
    }

    onlineTestButton->setEnabled(false);
    openAIStatus->setText(tr("Testing %1...").arg(QUrl(endpoint).host()));
    connectionStatus->setText(tr("Testing..."));
    QNetworkAccessManager *probe = new QNetworkAccessManager(this);
    QNetworkRequest request{QUrl(endpoint)};
    if (!onlineApiKey().isEmpty())
        request.setRawHeader("Authorization",
                             QByteArray("Bearer ") + onlineApiKey().toUtf8());
    QNetworkReply *reply = probe->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, probe, endpoint]() {
        const int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray response = reply->readAll();
        const bool success = reply->error() == QNetworkReply::NoError
            && status >= 200 && status < 300;
        if (usingHeadroom()) headroomReady = success;
        const QString detail = success
            ? tr("%1 responded successfully (HTTP %2).")
                  .arg(usingHeadroom() ? tr("Headroom") : tr("OpenAI"))
                  .arg(status)
            : tr("Connection test failed: %1 (HTTP %2)")
                  .arg(reply->errorString()).arg(status);
        openAIStatus->setText(detail);
        connectionStatus->setText(success ? tr("Verified") : tr("Unavailable"));
        emit chatAvailabilityChanged(success,
            success ? tr("Ready - provider connection verified")
                    : tr("Provider connection failed"));
        appendAudit(tr("Provider connection test to %1: %2%3")
            .arg(QUrl(endpoint).host(), success ? tr("success") : tr("failed"),
                 success || response.isEmpty()
                    ? QString() : QStringLiteral(" - ")
                        + QString::fromUtf8(response.left(500))));
        onlineTestButton->setEnabled(usingOnlineProvider());
        reply->deleteLater();
        probe->deleteLater();
    });
}

QString AIWorkbenchWindow::headroomRoot() const
{
    return QDir(managedRuntimeRoot()).absoluteFilePath(QStringLiteral("headroom"));
}

QString AIWorkbenchWindow::headroomExecutable() const
{
#ifdef Q_OS_WIN
    return QDir(headroomRoot()).absoluteFilePath(QStringLiteral("Scripts/headroom.exe"));
#else
    return QDir(headroomRoot()).absoluteFilePath(QStringLiteral("bin/headroom"));
#endif
}

QString AIWorkbenchWindow::headroomKeyPath() const
{
    const QString directory = QStandardPaths::writableLocation(
        QStandardPaths::AppDataLocation);
    return QDir(directory).absoluteFilePath(QStringLiteral("headroom-openai.key"));
}

QString AIWorkbenchWindow::loadHeadroomKey() const
{
    QFile file(headroomKeyPath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(file.readAll()).trimmed();
}

void AIWorkbenchWindow::saveHeadroomKey()
{
    const QString path = headroomKeyPath();
    if (!headroomRememberKeyCheck->isChecked())
    {
        QFile::remove(path);
        return;
    }
    const QString key = headroomUpstreamKeyEdit->text().trimmed();
    if (key.isEmpty()) return;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        headroomStatus->setText(tr("Could not save key"));
        return;
    }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(key.toUtf8());
    file.write("\n");
    if (!file.commit())
    {
        headroomStatus->setText(tr("Could not save key"));
        return;
    }
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    headroomStatus->setText(tr("Key saved privately"));
}

void AIWorkbenchWindow::updateHeadroomControls()
{
    if (!headroomInstallButton) return;
    const bool selected = usingHeadroom();
    const bool installed = QFileInfo::exists(headroomExecutable());
    const bool installing = headroomInstaller->state() != QProcess::NotRunning;
    const bool running = headroomRuntime->state() != QProcess::NotRunning;
    headroomInstallButton->setEnabled(selected && !installing && !installed);
    headroomStartButton->setEnabled(selected && installed && !running && !installing);
    headroomStopButton->setEnabled(selected && running);
    if (installing)
        headroomStatus->setText(tr("Installing..."));
    else if (running)
        headroomStatus->setText(headroomReady
            ? tr("Running on localhost:8787") : tr("Starting proxy..."));
    else if (installed)
        headroomStatus->setText(tr("Installed, stopped"));
    else
        headroomStatus->setText(tr("Not installed"));
}

void AIWorkbenchWindow::installHeadroom()
{
    if (headroomInstaller->state() != QProcess::NotRunning) return;
    QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (python.isEmpty())
        python = QStandardPaths::findExecutable(QStringLiteral("python"));
    if (python.isEmpty())
    {
        headroomStatus->setText(tr("Python 3 is not installed"));
        appendAudit(tr("Headroom installation failed: Python 3 not found"));
        return;
    }
    QDir().mkpath(managedRuntimeRoot());
    headroomInstallStage = 1;
    headroomInstaller->setProgram(python);
    headroomInstaller->setArguments({
        QStringLiteral("-m"), QStringLiteral("venv"), headroomRoot()
    });
    headroomInstaller->setProcessChannelMode(QProcess::MergedChannels);
    headroomInstaller->start();
    updateHeadroomControls();
    appendAudit(tr("Creating project-local Headroom environment"));
}

void AIWorkbenchWindow::headroomInstallFinished(int exitCode)
{
    const QString output = QString::fromUtf8(headroomInstaller->readAll()).trimmed();
    if (exitCode != 0)
    {
        headroomInstallStage = 0;
        appendAudit(tr("Headroom installation failed: %1").arg(output.right(1000)));
        updateHeadroomControls();
        headroomStatus->setText(tr("Installation failed"));
        headroomStatus->setToolTip(output.right(2000));
        return;
    }
    if (headroomInstallStage == 1)
    {
#ifdef Q_OS_WIN
        const QString python = QDir(headroomRoot()).absoluteFilePath(
            QStringLiteral("Scripts/python.exe"));
#else
        const QString python = QDir(headroomRoot()).absoluteFilePath(
            QStringLiteral("bin/python"));
#endif
        headroomInstallStage = 2;
        headroomInstaller->setProgram(python);
        headroomInstaller->setArguments({
            QStringLiteral("-m"), QStringLiteral("pip"), QStringLiteral("install"),
            QStringLiteral("--upgrade"), QStringLiteral("pip"),
            QStringLiteral("headroom-ai[proxy]")
        });
        headroomInstaller->start();
        headroomStatus->setText(tr("Downloading Headroom..."));
        appendAudit(tr("Downloading Headroom into the project-local environment"));
        return;
    }
    headroomInstallStage = 0;
    appendAudit(tr("Headroom installation completed"));
    updateHeadroomControls();
    if (headroomAutoStartCheck->isChecked()) startHeadroom();
}

void AIWorkbenchWindow::startHeadroom()
{
    if (headroomRuntime->state() != QProcess::NotRunning) return;
    headroomReady = false;
    if (!QFileInfo::exists(headroomExecutable()))
    {
        headroomStatus->setText(tr("Install Headroom first"));
        return;
    }
    QString key = headroomUpstreamKeyEdit->text().trimmed();
    if (key.isEmpty()) key = qEnvironmentVariable("OPENAI_API_KEY").trimmed();
    if (key.isEmpty())
    {
        const QString message = tr(
            "Start blocked: enter the Headroom upstream key, or restart SavvyCAN "
            "after OPENAI_API_KEY is exported");
        headroomStatus->setText(tr("Upstream OpenAI key unavailable"));
        headroomStatus->setToolTip(message);
        openAIStatus->setText(message);
        headroomUpstreamKeyEdit->setFocus();
        appendAudit(tr("Managed Headroom start blocked: OPENAI_API_KEY was not inherited "
                       "and no upstream key was entered"));
        return;
    }
    headroomStatus->setToolTip(QString());
    saveHeadroomKey();
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("OPENAI_API_KEY"), key);
    headroomRuntime->setProcessEnvironment(environment);
    headroomRuntime->setProgram(headroomExecutable());
    headroomRuntime->setArguments({
        QStringLiteral("proxy"), QStringLiteral("--backend"), QStringLiteral("openai"),
        QStringLiteral("--mode"), headroomModeCombo->currentData().toString(),
        QStringLiteral("--port"), QStringLiteral("8787")
    });
    headroomRuntime->setProcessChannelMode(QProcess::MergedChannels);
    headroomRuntime->start();
    headroomStatus->setText(tr("Starting..."));
    updateHeadroomControls();
    appendAudit(tr("Starting managed Headroom proxy on localhost:8787"));
}

void AIWorkbenchWindow::stopHeadroom()
{
    if (headroomRuntime->state() == QProcess::NotRunning) return;
#ifdef Q_OS_LINUX
    const QList<qint64> descendants = processDescendants(headroomRuntime->processId());
#endif
    headroomRuntime->terminate();
    if (!headroomRuntime->waitForFinished(2500))
    {
        headroomRuntime->kill();
        headroomRuntime->waitForFinished(1000);
    }
#ifdef Q_OS_LINUX
    for (const qint64 process : descendants)
        if (::kill(pid_t(process), 0) == 0) ::kill(pid_t(process), SIGTERM);
    for (const qint64 process : descendants)
        if (::kill(pid_t(process), 0) == 0) ::kill(pid_t(process), SIGKILL);
#endif
    updateHeadroomControls();
}

void AIWorkbenchWindow::headroomStarted()
{
    openAIEndpointEdit->setText(QStringLiteral("http://127.0.0.1:8787"));
    QSettings().setValue(QStringLiteral("AIWorkbench/HeadroomEndpoint"),
                         openAIEndpointEdit->text());
    headroomReady = false;
    headroomStatus->setText(tr("Waiting for proxy health..."));
    headroomStatus->setToolTip(QString());
    connectionStatus->setText(tr("Headroom starting"));
    updateHeadroomControls();
    emit chatAvailabilityChanged(false, tr("Waiting for Headroom"));
    emit runtimeStateChanged(tr("AI: Headroom starting"));
    appendAudit(tr("Managed Headroom process started; waiting for proxy health"));
    probeHeadroomReady(15);
}

void AIWorkbenchWindow::probeHeadroomReady(int attemptsRemaining)
{
    if (!usingHeadroom() || headroomRuntime->state() == QProcess::NotRunning)
        return;

    QString endpoint = openAIEndpointEdit->text().trimmed();
    while (endpoint.endsWith(QLatin1Char('/'))) endpoint.chop(1);
    const QStringList apiSuffixes = {
        QStringLiteral("/v1/chat/completions"),
        QStringLiteral("/chat/completions"),
        QStringLiteral("/v1/responses"),
        QStringLiteral("/responses")
    };
    for (const QString &suffix : apiSuffixes)
    {
        if (endpoint.endsWith(suffix))
        {
            endpoint.chop(suffix.size());
            break;
        }
    }
    if (endpoint.endsWith(QStringLiteral("/v1"))) endpoint.chop(3);
    const QUrl healthUrl(endpoint + QStringLiteral("/health"));

    QNetworkAccessManager *probe = new QNetworkAccessManager(this);
    QNetworkReply *reply = probe->get(QNetworkRequest(healthUrl));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, probe, attemptsRemaining, healthUrl]() {
        const int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool success = reply->error() == QNetworkReply::NoError
            && status >= 200 && status < 300;
        reply->deleteLater();
        probe->deleteLater();

        if (success)
        {
            headroomReady = true;
            updateHeadroomControls();
            connectionStatus->setText(tr("Headroom verified"));
            openAIStatus->setText(tr("Headroom is ready (HTTP %1).").arg(status));
            emit chatAvailabilityChanged(openAIEnabledCheck->isChecked(),
                openAIEnabledCheck->isChecked()
                    ? tr("Ready - Headroom verified")
                    : tr("Network provider disabled"));
            emit runtimeStateChanged(tr("AI: Headroom ready"));
            appendAudit(tr("Managed Headroom proxy became ready at %1")
                            .arg(healthUrl.toString()));
            requestHeadroomStats(false);
            return;
        }

        if (attemptsRemaining > 1
            && headroomRuntime->state() != QProcess::NotRunning)
        {
            headroomStatus->setText(tr("Waiting for proxy health..."));
            QTimer::singleShot(1000, this, [this, attemptsRemaining]() {
                probeHeadroomReady(attemptsRemaining - 1);
            });
            return;
        }

        headroomReady = false;
        updateHeadroomControls();
        connectionStatus->setText(tr("Headroom unavailable"));
        openAIStatus->setText(tr(
            "Headroom process started but its health endpoint did not become ready."));
        emit chatAvailabilityChanged(false, tr("Headroom unavailable"));
        emit runtimeStateChanged(tr("AI: Headroom unavailable"));
        appendAudit(tr("Managed Headroom health check did not become ready"));
    });
}

void AIWorkbenchWindow::requestHeadroomStats(bool reportRequestDelta)
{
    if (!usingHeadroom()) return;
    QString endpoint = openAIEndpointEdit->text().trimmed();
    while (endpoint.endsWith(QLatin1Char('/'))) endpoint.chop(1);
    if (endpoint.endsWith(QStringLiteral("/v1/chat/completions")))
        endpoint.chop(QStringLiteral("/v1/chat/completions").size());
    else if (endpoint.endsWith(QStringLiteral("/v1")))
        endpoint.chop(3);
    QNetworkReply *reply =
        network->get(QNetworkRequest(QUrl(endpoint + QStringLiteral("/stats"))));
    reply->setProperty("headroomStatsReply", true);
    reply->setProperty("headroomReportDelta", reportRequestDelta);
}

void AIWorkbenchWindow::handleHeadroomStatsReply(QNetworkReply *reply)
{
    const bool reportDelta = reply->property("headroomReportDelta").toBool();
    const QByteArray data = reply->readAll();
    const bool requestOk = reply->error() == QNetworkReply::NoError;
    reply->deleteLater();
    if (!requestOk) return;

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return;
    const QJsonObject root = document.object();
    const QJsonObject tokens = root.value(QStringLiteral("tokens")).toObject();
    const QJsonObject cost = root.value(QStringLiteral("cost")).toObject();
    const QJsonObject prefixCache = root.value(QStringLiteral("prefix_cache")).toObject()
                                        .value(QStringLiteral("totals")).toObject();
    const QJsonObject lifetime = root.value(QStringLiteral("persistent_savings")).toObject()
                                     .value(QStringLiteral("lifetime")).toObject();

    qint64 inputTokens = qint64(tokens.value(QStringLiteral("input")).toDouble(-1));
    qint64 savedTokens = qint64(tokens.value(QStringLiteral("saved")).toDouble(-1));
    if (savedTokens < 0)
        savedTokens = qint64(lifetime.value(QStringLiteral("tokens_saved")).toDouble(-1));
    if (savedTokens < 0)
        savedTokens = qint64(root.value(QStringLiteral("tokens_saved_total")).toDouble(-1));
    if (savedTokens < 0) return;
    const qint64 attemptedTokens =
        qint64(tokens.value(QStringLiteral("proxy_attempted_tokens")).toDouble(0));
    const qint64 cacheReadTokens =
        qint64(prefixCache.value(QStringLiteral("cache_read_tokens")).toDouble(0));
    const double cacheSavingsUsd =
        prefixCache.value(QStringLiteral("net_savings_usd")).toDouble(0.0);

    double savedUsd = cost.value(QStringLiteral("total_savings_usd")).toDouble(-1.0);
    if (savedUsd < 0.0)
        savedUsd = lifetime.value(QStringLiteral("compression_savings_usd")).toDouble(-1.0);
    const bool pricingAvailable =
        (!root.contains(QStringLiteral("litellm_available"))
         || root.value(QStringLiteral("litellm_available")).toBool())
        && savedUsd >= 0.0;

    if (reportDelta && headroomStatsReady)
    {
        const qint64 requestSaved = qMax<qint64>(0, savedTokens - headroomSavedTokens);
        const qint64 requestAttempted =
            qMax<qint64>(0, attemptedTokens - headroomAttemptedTokens);
        const qint64 requestCacheRead =
            qMax<qint64>(0, cacheReadTokens - headroomCacheReadTokens);
        const double requestCacheUsd =
            qMax(0.0, cacheSavingsUsd - headroomCacheSavingsUsd);
        const qint64 requestInput = inputTokens >= 0 && headroomInputTokens >= 0
            ? qMax<qint64>(0, inputTokens - headroomInputTokens) : -1;
        const qint64 originalInput = requestInput >= 0 ? requestInput + requestSaved : -1;
        const double percent = originalInput > 0
            ? 100.0 * double(requestSaved) / double(originalInput) : 0.0;
        const double requestUsd = pricingAvailable && headroomSavedUsd >= 0.0
            ? qMax(0.0, savedUsd - headroomSavedUsd) : -1.0;

        QString event;
        if (requestSaved > 0)
            event = originalInput >= 0
                ? tr("Headroom compressed %1 input token(s) for this request (%2%; %3 original -> %4 delivered)")
                      .arg(requestSaved).arg(percent, 0, 'f', 1)
                      .arg(originalInput).arg(requestInput)
                : tr("Headroom compressed %1 token(s) for this request").arg(requestSaved);
        else if (requestAttempted > 0)
            event = tr("Headroom evaluated %1 eligible tool/schema token(s), but retained them all")
                        .arg(requestAttempted);
        else
            event = tr("Headroom found no eligible tool-result or redundant-schema content to compress; "
                       "instructions, user text and stable prompt prefixes are intentionally retained");
        if (requestCacheRead > 0)
            event += tr("; provider prefix cache reused %1 token(s), estimated net discount $%2 USD")
                         .arg(requestCacheRead).arg(requestCacheUsd, 0, 'f', 6);
        else
            event += tr("; no provider prefix-cache read was reported");
        event += requestUsd >= 0.0
            ? tr("; compression cost saved $%1 USD").arg(requestUsd, 0, 'f', 6)
            : tr("; compression cost estimate unavailable");
        event += tr(". Lifetime: %1 token(s)").arg(savedTokens);
        if (pricingAvailable)
            event += tr(", $%1 USD estimated").arg(savedUsd, 0, 'f', 6);
        appendAudit(event);
        openAIUsage->setText(openAIUsage->text() + tr(
            " | Headroom compressed %1 token(s), cache read %2")
            .arg(requestSaved).arg(requestCacheRead));
    }

    headroomStatsReady = true;
    headroomInputTokens = inputTokens;
    headroomSavedTokens = savedTokens;
    headroomAttemptedTokens = attemptedTokens;
    headroomCacheReadTokens = cacheReadTokens;
    headroomSavedUsd = savedUsd;
    headroomCacheSavingsUsd = cacheSavingsUsd;
}

void AIWorkbenchWindow::headroomFinished(int exitCode)
{
    const QString output = QString::fromUtf8(headroomRuntime->readAll()).trimmed();
    headroomReady = false;
    updateHeadroomControls();
    if (exitCode != 0)
    {
        headroomStatus->setText(tr("Stopped with error"));
        headroomStatus->setToolTip(output.right(2000));
    }
    appendAudit(output.isEmpty()
        ? tr("Managed Headroom proxy stopped")
        : tr("Managed Headroom proxy stopped: %1").arg(output.right(1000)));
    emit runtimeStateChanged(exitCode == 0 ? tr("AI: stopped")
                                           : tr("AI: Headroom error"));
    emit chatAvailabilityChanged(false, exitCode == 0
        ? tr("Headroom stopped") : tr("Headroom stopped with an error"));
}

QString AIWorkbenchWindow::managedRuntimeRoot() const
{
    const QString configured = QSettings().value(QStringLiteral("AIWorkbench/ManagedRuntimeRoot")).toString();
    if (!configured.isEmpty()) return QDir::cleanPath(configured);

    const QString workingCopy = QDir::current().filePath(QStringLiteral("local-ai"));
    if (QFileInfo::exists(workingCopy + QStringLiteral("/bin/ollama")))
        return QDir::cleanPath(workingCopy);

    QDir applicationDir(QCoreApplication::applicationDirPath());
    const QString besideApplication = applicationDir.filePath(QStringLiteral("local-ai"));
    if (QFileInfo::exists(besideApplication + QStringLiteral("/bin/ollama")))
        return QDir::cleanPath(besideApplication);
    return QDir::cleanPath(applicationDir.filePath(QStringLiteral("../../local-ai")));
}

QProcessEnvironment AIWorkbenchWindow::managedRuntimeEnvironment() const
{
    const QString root = managedRuntimeRoot();
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("OLLAMA_HOST"), QStringLiteral("127.0.0.1:11435"));
    environment.insert(QStringLiteral("OLLAMA_MODELS"), root + QStringLiteral("/models"));
    environment.insert(QStringLiteral("OLLAMA_KEEP_ALIVE"), QStringLiteral("30s"));
    environment.insert(QStringLiteral("HOME"), root + QStringLiteral("/home"));
    environment.insert(QStringLiteral("LD_LIBRARY_PATH"),
        root + QStringLiteral("/lib/ollama:") + environment.value(QStringLiteral("LD_LIBRARY_PATH")));
    return environment;
}

void AIWorkbenchWindow::startManagedRuntime()
{
    if (managedRuntime->state() != QProcess::NotRunning) return;
    const QString root = managedRuntimeRoot();
    const QString executable = root + QStringLiteral("/bin/ollama");
    if (!QFileInfo::exists(executable))
    {
        runtimeStatus->setText(tr("Runtime not found in %1").arg(root));
        appendAudit(tr("Bundled runtime start failed: executable not found"));
        return;
    }

    QDir().mkpath(root + QStringLiteral("/models"));
    QDir().mkpath(root + QStringLiteral("/home"));
    managedRuntime->setProcessEnvironment(managedRuntimeEnvironment());
    managedRuntime->setProgram(executable);
    managedRuntime->setArguments({QStringLiteral("serve")});
    managedRuntime->setProcessChannelMode(QProcess::MergedChannels);
    managedRuntime->start();
    runtimeStatus->setText(tr("Starting..."));
    emit runtimeStateChanged(tr("AI: starting"));
    appendAudit(tr("Starting bundled runtime from %1").arg(root));
}

void AIWorkbenchWindow::stopManagedRuntime()
{
    if (managedRuntime->state() == QProcess::NotRunning) return;
#ifdef Q_OS_LINUX
    const QList<qint64> descendants = processDescendants(managedRuntime->processId());
#endif
    const QString executable = managedRuntimeRoot() + QStringLiteral("/bin/ollama");
    QSet<QString> modelsToUnload{
        selectedModel(primaryModelCombo), selectedModel(reviewModelCombo)
    };
    modelsToUnload.remove(QString());
    modelsToUnload.remove(tr("None"));
    for (const QString &model : modelsToUnload)
    {
        QProcess unloader;
        unloader.setProcessEnvironment(managedRuntimeEnvironment());
        unloader.start(executable, {QStringLiteral("stop"), model});
        if (!unloader.waitForFinished(1500))
        {
            unloader.kill();
            unloader.waitForFinished(500);
        }
    }
    managedRuntime->terminate();
    if (!managedRuntime->waitForFinished(2000))
    {
        managedRuntime->kill();
        managedRuntime->waitForFinished(1000);
    }
#ifdef Q_OS_LINUX
    for (const qint64 process : descendants)
        if (::kill(pid_t(process), 0) == 0) ::kill(pid_t(process), SIGTERM);
    for (const qint64 process : descendants)
        if (::kill(pid_t(process), 0) == 0) ::kill(pid_t(process), SIGKILL);
#endif
}

void AIWorkbenchWindow::pullModel()
{
    const QString model = modelManagerCombo->currentData().toString().isEmpty()
        ? modelManagerCombo->currentText().trimmed()
        : modelManagerCombo->currentData().toString();
    if (model.isEmpty()) return;
    startModelOperation({QStringLiteral("pull"), model}, tr("Downloading %1").arg(model));
}

void AIWorkbenchWindow::removeModel()
{
    const QString model = modelManagerCombo->currentData().toString().isEmpty()
        ? modelManagerCombo->currentText().trimmed()
        : modelManagerCombo->currentData().toString();
    if (model.isEmpty()) return;
    startModelOperation({QStringLiteral("rm"), model}, tr("Removing %1").arg(model));
}

void AIWorkbenchWindow::startModelOperation(const QStringList &arguments, const QString &description)
{
    if (modelOperation->state() != QProcess::NotRunning) return;
    const QString executable = managedRuntimeRoot() + QStringLiteral("/bin/ollama");
    if (!QFileInfo::exists(executable))
    {
        modelOperationStatus->setText(tr("Bundled runtime unavailable"));
        return;
    }
    pullModelButton->setEnabled(false);
    removeModelButton->setEnabled(false);
    modelOperationStatus->setText(description);
    if (managedRuntime->state() == QProcess::NotRunning)
    {
        startManagedRuntime();
        QTimer::singleShot(1000, this, [this, arguments, description]() {
            pullModelButton->setEnabled(true);
            removeModelButton->setEnabled(true);
            startModelOperation(arguments, description);
        });
        return;
    }
    modelOperation->setProgram(executable);
    modelOperation->setArguments(arguments);
    modelOperation->setProcessEnvironment(managedRuntimeEnvironment());
    modelOperation->setProcessChannelMode(QProcess::MergedChannels);
    modelOperation->start();
    appendAudit(description);
}

void AIWorkbenchWindow::modelOperationFinished(int exitCode)
{
    const QString output = QString::fromUtf8(modelOperation->readAll()).trimmed();
    modelOperationStatus->setText(exitCode == 0 ? tr("Complete") : tr("Failed"));
    appendAudit(exitCode == 0 ? tr("Model operation completed")
                              : tr("Model operation failed: %1").arg(output.right(500)));
    refreshModels();
}

void AIWorkbenchWindow::updateModelButtons()
{
    const QString model = modelManagerCombo->currentData().toString().isEmpty()
        ? modelManagerCombo->currentText().trimmed()
        : modelManagerCombo->currentData().toString();
    const bool installed = installedModels.contains(model);
    const bool idle = modelOperation->state() == QProcess::NotRunning;
    pullModelButton->setEnabled(idle && !model.isEmpty() && !installed);
    removeModelButton->setEnabled(idle && installed);
}

void AIWorkbenchWindow::managedRuntimeStarted()
{
    endpointEdit->setText(QStringLiteral("http://127.0.0.1:11435"));
    runtimeStatus->setText(tr("Running with project models"));
    emit runtimeStateChanged(tr("AI: ready"));
    appendAudit(tr("Bundled runtime started"));
}

void AIWorkbenchWindow::managedRuntimeFinished(int exitCode)
{
    lastRuntimeOutput += QString::fromUtf8(managedRuntime->readAll());
    const QString output = lastRuntimeOutput.trimmed();
    lastRuntimeOutput.clear();
    runtimeStatus->setText(tr("Stopped (exit %1)").arg(exitCode));
    emit runtimeStateChanged(exitCode == 0 ? tr("AI: stopped") : tr("AI: error"));
    if (exitCode != 0 && (output.contains(QStringLiteral("signal: killed"), Qt::CaseInsensitive)
        || output.contains(QStringLiteral("out of memory"), Qt::CaseInsensitive)))
    {
        armAccessCheck->setChecked(false);
        runtimeStatus->setText(tr("Worker killed - likely RAM/VRAM exhaustion; reduce capture or model"));
        resultOutput->appendPlainText(tr("\nRuntime recovery: your prompt was preserved. "
            "Reduce the frame limit or choose a smaller model, then retry."));
    }
    appendAudit(output.isEmpty() ? tr("Bundled runtime stopped")
                                 : tr("Bundled runtime stopped: %1").arg(output.right(500)));
}

void AIWorkbenchWindow::refreshModels()
{
    if (activeReply) return;
    if (usingOnlineProvider())
    {
        providerChanged();
        const bool available = openAIEnabledCheck->isChecked()
            && (!usingHeadroom() || headroomReady);
        connectionStatus->setText(openAIEnabledCheck->isChecked()
            ? usingHeadroom() && !headroomReady
                ? tr("Waiting for Headroom") : tr("Network provider configured")
            : tr("Network provider disabled"));
        emit chatAvailabilityChanged(available,
            available ? tr("Ready - network model selected")
                      : openAIEnabledCheck->isChecked()
                          ? tr("Waiting for Headroom")
                          : tr("Network provider disabled"));
        return;
    }
    QSettings().setValue(QStringLiteral("AIWorkbench/OllamaEndpoint"), endpointEdit->text().trimmed());
    QUrl url(endpointEdit->text().trimmed() + QStringLiteral("/api/tags"));
    connectionStatus->setText(tr("Checking..."));
    requestPurpose = RequestPurpose::ModelList;
    activeReply = network->get(QNetworkRequest(url));
    setBusy(true);
    appendAudit(tr("Requested local model list from %1").arg(url.toString()));
}

QString AIWorkbenchWindow::selectedModel(const QComboBox *combo) const
{
    const QString model = combo->currentData().toString();
    return model.isEmpty() ? combo->currentText().trimmed() : model;
}

void AIWorkbenchWindow::analyzeCapture()
{
    if (selectedModel(primaryModelCombo).isEmpty())
    {
        resultOutput->setPlainText(tr("Select or enter an analysis model first."));
        return;
    }
    if (usingOnlineProvider() && !openAICaptureCheck->isChecked())
    {
        resultOutput->setPlainText(tr(
            "Filtered CAN evidence uploads are disabled in Online Provider."));
        return;
    }

    bool ok = false;
    QString error;
    currentEvidence = buildEvidence(&ok, &error);
    if (!ok)
    {
        resultOutput->setPlainText(error);
        return;
    }

    evidenceOutput->setPlainText(QString::fromUtf8(
        QJsonDocument(currentEvidence).toJson(QJsonDocument::Indented)));
    resultOutput->setPlainText(usingOnlineProvider()
        ? tr("Preparing network analysis request...") : tr("Analyzing locally..."));
    primaryResult.clear();
    const QString systemPrompt = QStringLiteral(
        "You are a CAN reverse-engineering assistant. Work only from supplied evidence. "
        "Separate observations from hypotheses. Do not invent vehicle-specific meanings. "
        "Propose SavvyCAN payload formatter expressions when supported by evidence. "
        "Return concise JSON with keys summary, observations, hypotheses, formatter_candidates, "
        "next_experiments, and cautions.");
    const QString userPrompt = instructionEdit->toPlainText() + QStringLiteral("\n\nEvidence:\n") +
        QString::fromUtf8(QJsonDocument(currentEvidence).toJson(QJsonDocument::Compact));
    appendAudit(tr("Analysis started with model %1 in %2 mode")
        .arg(selectedModel(primaryModelCombo), accessCombo->currentText()));
    sendChat(selectedModel(primaryModelCombo), systemPrompt, userPrompt, RequestPurpose::PrimaryAnalysis);
}

void AIWorkbenchWindow::sendChatMessage()
{
    submitChat(chatInput->toPlainText().trimmed(), chatCaptureCheck->isChecked());
}

void AIWorkbenchWindow::submitChat(const QString &question, bool includeCapture)
{
    if (question.isEmpty()) return;
    if (activeReply)
    {
        const QString message = tr(
            "Another AI request is still running. Stop it or wait for it to finish.");
        chatOutput->addSystemMessage(message);
        emit chatLineAdded(tr("System"), message);
        return;
    }
    if (usingOnlineProvider() && includeCapture && !openAICaptureCheck->isChecked())
    {
        chatOutput->addSystemMessage(tr(
            "Capture upload blocked. Enable filtered CAN evidence uploads in Online Provider."));
        return;
    }

    static const QRegularExpression detailedResultsExpression(
        QStringLiteral("\\b(?:show|list|report|include|give)\\b[^\\n]{0,30}"
                       "\\b(?:each|every|individual|detailed|full)\\b[^\\n]{0,20}"
                       "\\b(?:action\\s+)?results?\\b|"
                       "\\b(?:detailed|individual)\\s+(?:action\\s+)?results?\\b"),
        QRegularExpression::CaseInsensitiveOption);
    detailedActionResults = detailedResultsExpression.match(question).hasMatch();

    const QRegularExpression randomFramesExpression(
        QStringLiteral("\\b(?:send|transmit)\\s+(?:another\\s+)?(\\d+)\\s+"
                       "(?:(more|additional)\\s+)?(?:(random)\\s+)?"
                       "(?:(?:can\\s+)?frames?)?(?:\\s+to\\s+(?:the\\s+)?bus)?\\b"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch randomFramesMatch =
        randomFramesExpression.match(question);
    const bool continuesRandomRequest = !randomFramesMatch.captured(2).isEmpty()
        && chatHistory.contains(QStringLiteral("random"), Qt::CaseInsensitive);
    if (randomFramesMatch.hasMatch()
        && (!randomFramesMatch.captured(3).isEmpty() || continuesRandomRequest))
    {
        const int count = qBound(1, randomFramesMatch.captured(1).toInt(), 100);
        const int bus = qMax(0, busSpin->value());
        QJsonArray actions;
        for (int frameIndex = 0; frameIndex < count; ++frameIndex)
        {
            QStringList bytes;
            for (int byteIndex = 0; byteIndex < 8; ++byteIndex)
                bytes << QStringLiteral("%1")
                             .arg(QRandomGenerator::global()->bounded(256),
                                  2, 16, QLatin1Char('0')).toUpper();
            actions.append(QJsonObject{
                {QStringLiteral("capability"), QStringLiteral("frame.send_once")},
                {QStringLiteral("arguments"), QJsonObject{
                    {QStringLiteral("bus"), bus},
                    {QStringLiteral("can_id"), QStringLiteral("0x%1")
                         .arg(QRandomGenerator::global()->bounded(0x800),
                              0, 16).toUpper()},
                    {QStringLiteral("extended"), false},
                    {QStringLiteral("payload"), bytes.join(QLatin1Char(' '))}
                }}
            });
        }

        chatOutput->addMessage(tr("You"), question);
        emit chatLineAdded(tr("You"), question);
        chatInput->clear();
        const QString answer = tr(
            "Prepared %1 random standard CAN frame(s) on bus %2. "
            "SavvyCAN will apply the configured access and confirmation policy.")
                                   .arg(count).arg(bus);
        chatOutput->addMessage(tr("Assistant"), answer);
        emit chatLineAdded(tr("Assistant"), answer);
        chatHistory += QStringLiteral("\nUser: ") + question
            + QStringLiteral("\nAssistant: ") + answer;
        persistChat();
        appendAudit(tr("Prepared %1 deterministic random-frame action(s)").arg(count));
        emit actionsProposed(actions);
        return;
    }

    const QString model = selectedModel(primaryModelCombo);
    if (model.isEmpty())
    {
        chatOutput->addSystemMessage(tr("Select an installed analysis model first."));
        return;
    }

    QString context;
    if (includeCapture)
    {
        bool ok = false;
        QString error;
        const QJsonObject evidence = buildEvidence(&ok, &error);
        if (!ok)
        {
            chatOutput->addSystemMessage(tr("Capture context error: %1").arg(error));
            return;
        }
        context = QStringLiteral("\n\nCurrent bounded capture evidence:\n%1")
            .arg(QString::fromUtf8(QJsonDocument(evidence).toJson(QJsonDocument::Compact)));
    }

    chatOutput->addMessage(tr("You"), question);
    emit chatLineAdded(tr("You"), question);
    chatInput->clear();
    const bool connectivityPing = usingOnlineProvider()
        && QRegularExpression(
            QStringLiteral("^\\s*(?:test|ping|hello|hi|are you (?:there|working)|"
                           "is (?:this|the online model) working)\\s*[?.!]*\\s*$"),
            QRegularExpression::CaseInsensitiveOption).match(question).hasMatch();
    const QJsonObject routingContext = applicationContext();
    if (!connectivityPing
        && !AIActionRegistry::matchingSkills(question, routingContext).isEmpty())
        skillRoutingQuestion = question;
    else if (!connectivityPing && skillRoutingQuestion.isEmpty())
        skillRoutingQuestion = QStringLiteral("savvycan");
    const QJsonObject appContext = connectivityPing
        ? QJsonObject{{QStringLiteral("connectivity_test"), true}}
        : promptApplicationContext(question);
    selectedCapabilityCatalog = connectivityPing
        ? QJsonArray()
        : AIActionRegistry::catalogForQuestion(skillRoutingQuestion, routingContext);
    suppressChatTools = connectivityPing;
    appendAudit(tr("Selected AI skill(s) v%1: %2; exposed %3 capability tool(s)")
        .arg(AIActionRegistry::skillVersion())
        .arg(connectivityPing ? tr("connectivity ping")
             : AIActionRegistry::matchingSkills(skillRoutingQuestion, routingContext)
                   .join(QStringLiteral(", ")))
        .arg(selectedCapabilityCatalog.size()));
    const QString capabilityPrompt = connectivityPing
        ? QStringLiteral("This is a minimal provider connectivity test. "
                         "Reply with a short confirmation.")
        : capabilityContext(question);
    const QString historyPrompt = connectivityPing
        ? QStringLiteral("[History omitted for connectivity test.]")
        : usingOnlineProvider() && !openAIHistoryCheck->isChecked()
            ? QStringLiteral("[Chat history withheld by online privacy settings.]")
            : compressedChatHistory();
    const QString prompt = QStringLiteral(
        "SavvyCAN capability and documentation context:\n%1\n\n"
        "Conversation so far:\n%2\n\nUser: %3%4\n\n"
        "Answer only the user's current request. Conversation history is reference context, "
        "not a queue of unfinished commands: never repeat or continue an earlier operation "
        "unless the current message explicitly asks you to. Separate observed CAN evidence "
        "from hypotheses. "
        "Never claim that a proposed transmit frame has been sent. When an app action "
        "would help, call the matching native SavvyCAN tool. Only if native tools are unavailable, "
        "provide a fenced savvycan-action JSON object or ordered JSON array containing capability "
        "and arguments. Do not include unrelated actions. Use obd.clear_pids before obd.add_pid "
        "when replacing the OBD request list. Treat GUI actions as the default way to perform tasks. "
        "Do not mention, propose, or generate scripts unless the user explicitly asks for a script "
        "or JavaScript. A single requested CAN frame uses frame.send_once. Repeated transmission "
        "uses frame.send_loop so it is visible and manageable in Frame Sender. Resolve natural-language "
        "entities using the selected application skill. Never invent a PID, DID, CAN ID, bus, or payload. "
        "If a manufacturer-specific DID cannot be resolved from application state, ask for the DID.")
        .arg(capabilityPrompt
                 + QStringLiteral("\n\nCurrent application state:\n")
                 + QString::fromUtf8(QJsonDocument(appContext).toJson(QJsonDocument::Compact)),
             historyPrompt,
             question, context);
    chatHistory += QStringLiteral("\nUser: ") + question;
    persistChat();
    appendAudit(tr("Chat request started with %1").arg(model));
    sendChat(model, QStringLiteral(
        "You are an assistant embedded in SavvyCAN. Help with CAN reverse engineering, "
        "diagnostics, payload formatting and general technical analysis. Prefer registered GUI "
        "capabilities for all tasks. Never mention or generate scripting unless the user explicitly "
        "asks for a script or JavaScript. Any transmit frames are proposals handled by SavvyCAN's "
        "configured access policy."), prompt,
        RequestPurpose::Chat);
}

QString AIWorkbenchWindow::readHelpPage(const QString &fileName, int maximumCharacters) const
{
    const QString applicationDir = QCoreApplication::applicationDirPath();
    const QStringList roots = {
        applicationDir + QStringLiteral("/help"),
        QDir(applicationDir).absoluteFilePath(QStringLiteral("../help")),
        QDir(applicationDir).absoluteFilePath(QStringLiteral("../../help")),
        QDir::current().absoluteFilePath(QStringLiteral("help"))
    };
    for (const QString &root : roots)
    {
        QFile file(QDir(root).absoluteFilePath(fileName));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        return QString::fromUtf8(file.read(maximumCharacters));
    }
    return QString();
}

QString AIWorkbenchWindow::capabilityContext(const QString &question) const
{
    const QString lower = question.toLower();
    const QJsonObject appContext = applicationContext();
    QStringList pages;
    pages << QStringLiteral("ai_chat_skill.md");
    const bool asksHowAppWorks =
        lower.contains(QStringLiteral("how does"))
        || lower.contains(QStringLiteral("how do"))
        || lower.contains(QStringLiteral("explain"))
        || lower.contains(QStringLiteral("documentation"))
        || lower.contains(QStringLiteral("what is"))
        || lower.contains(QStringLiteral("what does"));
    if (lower.contains(QStringLiteral("payload")) || lower.contains(QStringLiteral("format"))
        || lower.contains(QStringLiteral("decode")) || lower.contains(QStringLiteral("bit")))
        pages << QStringLiteral("payload_formatter.md");
    if (asksHowAppWorks && (lower.contains(QStringLiteral("uds"))
        || lower.contains(QStringLiteral("did"))
        || lower.contains(QStringLiteral("diagnostic"))))
        pages << QStringLiteral("uds_workbench.md");
    if (asksHowAppWorks && (lower.contains(QStringLiteral("obd"))
        || lower.contains(QStringLiteral("pid"))))
        pages << QStringLiteral("obd2_workbench.md");
    if (asksHowAppWorks && (lower.contains(QStringLiteral("canopen"))
        || lower.contains(QStringLiteral("sdo"))
        || lower.contains(QStringLiteral("object dictionary"))))
        pages << QStringLiteral("canopen_workbench.md");
    if (lower.contains(QStringLiteral("isotp")) || lower.contains(QStringLiteral("iso-tp")))
        pages << QStringLiteral("isotp_decoder.md");
    if (lower.contains(QStringLiteral("dbc")) || lower.contains(QStringLiteral("signal")))
        pages << QStringLiteral("dbc_editor.md") << QStringLiteral("signaleditor.md");
    if (lower.contains(QStringLiteral("graph")) || lower.contains(QStringLiteral("plot")))
        pages << QStringLiteral("graphwindow.md");
    if (lower.contains(QStringLiteral("sniff")))
        pages << QStringLiteral("sniffer.md");
    if (lower.contains(QStringLiteral("fuzz")))
        pages << QStringLiteral("fuzzingwindow.md");
    if (lower.contains(QStringLiteral("bridge")))
        pages << QStringLiteral("canbridge.md");
    if (lower.contains(QStringLiteral("playback")) || lower.contains(QStringLiteral("log")))
        pages << QStringLiteral("playbackwindow.md");
    if (lower.contains(QStringLiteral("bus load")) || lower.contains(QStringLiteral("error frame"))
        || lower.contains(QStringLiteral("controller state")))
        pages << QStringLiteral("bus_diagnostics.md");
    if (lower.contains(QStringLiteral("script")) || lower.contains(QStringLiteral("javascript")))
        pages << QStringLiteral("scriptingwindow.md");
    pages.removeDuplicates();

    QString result = QStringLiteral(
        "Application skill manifest selected for this request:\n%1")
        .arg(AIActionRegistry::skillContext(
            skillRoutingQuestion.isEmpty() ? question : skillRoutingQuestion, appContext));
    const QString graphContext = graphifyContext(question);
    if (!graphContext.isEmpty())
        result += QStringLiteral("\n\n--- Graphify repository context ---\n%1")
                      .arg(graphContext);
    for (const QString &page : pages)
    {
        const QString documentation = readHelpPage(page, 7000);
        if (!documentation.isEmpty())
            result += QStringLiteral("\n\n--- %1 ---\n%2").arg(page, documentation);
    }
    return result.left(32000);
}

void AIWorkbenchWindow::clearChat()
{
    chatHistory.clear();
    skillRoutingQuestion.clear();
    selectedCapabilityCatalog = QJsonArray();
    chatOutput->clearMessages();
    emit chatCleared();
    persistChat();
    appendAudit(tr("Chat history cleared"));
}

void AIWorkbenchWindow::clearAudit()
{
    auditOutput->clear();
    const QString directory =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QFile::remove(QDir(directory).absoluteFilePath(QStringLiteral("ai-audit.jsonl")));
}

QSet<quint32> AIWorkbenchWindow::parseIdFilter(bool *ok, QString *error) const
{
    QSet<quint32> ids;
    const QString text = idFilterEdit->text().trimmed();
    if (text.isEmpty())
    {
        *ok = true;
        return ids;
    }

    const QStringList parts = text.split(',', Qt::SkipEmptyParts);
    for (QString part : parts)
    {
        part = part.trimmed();
        const int dash = part.indexOf('-');
        QString firstText = dash < 0 ? part : part.left(dash);
        QString lastText = dash < 0 ? part : part.mid(dash + 1);
        bool firstOk = false;
        bool lastOk = false;
        const quint32 first = firstText.toUInt(&firstOk, 16);
        const quint32 last = lastText.toUInt(&lastOk, 16);
        if (!firstOk || !lastOk || first > last || last > 0x1FFFFFFF || last - first > 4096)
        {
            *ok = false;
            *error = tr("Invalid or overly broad CAN-ID entry: %1").arg(part);
            return {};
        }
        for (quint32 id = first; id <= last; ++id) ids.insert(id);
    }
    *ok = true;
    return ids;
}

QJsonObject AIWorkbenchWindow::buildEvidence(bool *ok, QString *error) const
{
    const QVector<CANFrame> *frames = evidenceFrames();
    if (!frames || frames->isEmpty())
    {
        *ok = false;
        *error = tr("The current capture contains no frames.");
        return {};
    }

    bool filterOk = false;
    const QSet<quint32> allowedIds = parseIdFilter(&filterOk, error);
    if (!filterOk)
    {
        *ok = false;
        return {};
    }

    QMap<QString, IdEvidence> byId;
    int accepted = 0;
    const int start = qMax(0, frames->size() - frameLimitSpin->value());
    for (int i = start; i < frames->size(); ++i)
    {
        const CANFrame &frame = frames->at(i);
        if (frame.frameType() != QCanBusFrame::DataFrame) continue;
        if (busSpin->value() >= 0 && int(frame.bus) != busSpin->value()) continue;
        if (!allowedIds.isEmpty() && !allowedIds.contains(frame.frameId())) continue;

        IdEvidence &value = byId[frameKey(frame)];
        const qint64 timestampUs = frame.timeStamp().microSeconds();
        const QByteArray payload = frame.payload();
        const QByteArray priorPayload = value.previousPayload;
        if (value.count == 0)
        {
            value.firstUs = timestampUs;
            value.firstPayload = payload;
            value.previousPayload = payload;
            value.changedMask = QByteArray(payload.size(), 0);
        }
        else
        {
            if (timestampUs >= value.previousUs)
            {
                value.intervalTotalMs += (timestampUs - value.previousUs) / 1000.0;
                ++value.intervals;
            }
            if (value.changedMask.size() < payload.size())
                value.changedMask.resize(payload.size());
            const int common = qMin(payload.size(), value.previousPayload.size());
            for (int byte = 0; byte < common; ++byte)
                value.changedMask[byte] = char(quint8(value.changedMask.at(byte)) |
                                               (quint8(payload.at(byte)) ^ quint8(value.previousPayload.at(byte))));
            value.previousPayload = payload;
        }
        value.previousUs = timestampUs;
        value.lastUs = timestampUs;
        value.minLength = qMin(value.minLength, payload.size());
        value.maxLength = qMax(value.maxLength, payload.size());
        ++value.count;
        value.uniquePayloads.insert(payload);
        if (value.byteHistograms.size() < payload.size())
        {
            value.byteHistograms.resize(payload.size());
            value.bitTransitions.resize(payload.size() * 8);
        }
        for (int byte = 0; byte < payload.size(); ++byte)
        {
            value.byteHistograms[byte][quint8(payload.at(byte))]++;
            if (byte < priorPayload.size())
            {
                const quint8 changed = quint8(payload.at(byte)) ^ quint8(priorPayload.at(byte));
                for (int bit = 0; bit < 8; ++bit)
                    if (changed & (1U << bit)) value.bitTransitions[byte * 8 + bit]++;
            }
        }
        ++accepted;
        if (value.samples.size() < 8)
            value.samples.append(QString::fromLatin1(payload.toHex()));
    }

    QJsonArray identifiers;
    for (auto it = byId.cbegin(); it != byId.cend(); ++it)
    {
        const QStringList key = it.key().split(':');
        const IdEvidence &value = it.value();
        QJsonObject item;
        item.insert(QStringLiteral("bus"), key.at(0).toInt());
        item.insert(QStringLiteral("id"), QStringLiteral("0x%1").arg(key.at(1).toUInt(), 0, 16).toUpper());
        item.insert(QStringLiteral("extended"), key.at(2).toInt() != 0);
        item.insert(QStringLiteral("frames"), static_cast<double>(value.count));
        item.insert(QStringLiteral("dlc_min"), value.minLength);
        item.insert(QStringLiteral("dlc_max"), value.maxLength);
        item.insert(QStringLiteral("duration_ms"), (value.lastUs - value.firstUs) / 1000.0);
        item.insert(QStringLiteral("mean_interval_ms"),
                    value.intervals ? value.intervalTotalMs / value.intervals : 0.0);
        item.insert(QStringLiteral("first_payload"), QString::fromLatin1(value.firstPayload.toHex()));
        item.insert(QStringLiteral("changed_bit_mask"), QString::fromLatin1(value.changedMask.toHex()));
        item.insert(QStringLiteral("unique_payloads"), value.uniquePayloads.size());
        QJsonArray entropy;
        for (const QMap<int, quint64> &histogram : value.byteHistograms)
        {
            double bits = 0.0;
            for (quint64 frequency : histogram)
            {
                const double probability = double(frequency) / double(value.count);
                bits -= probability * std::log2(probability);
            }
            entropy.append(bits);
        }
        QJsonArray transitions;
        for (quint64 count : value.bitTransitions) transitions.append(double(count));
        item.insert(QStringLiteral("byte_entropy_bits"), entropy);
        item.insert(QStringLiteral("bit_transition_counts"), transitions);
        item.insert(QStringLiteral("samples"), value.samples);
        identifiers.append(item);
    }

    QJsonObject scope;
    scope.insert(QStringLiteral("bus"), busSpin->value());
    scope.insert(QStringLiteral("id_allowlist"), idFilterEdit->text().trimmed());
    scope.insert(QStringLiteral("accepted_frames"), accepted);
    scope.insert(QStringLiteral("identifier_count"), identifiers.size());
    scope.insert(QStringLiteral("recent_frame_limit"), frameLimitSpin->value());
    QJsonObject result;
    result.insert(QStringLiteral("scope"), scope);
    result.insert(QStringLiteral("identifiers"), identifiers);
    result.insert(QStringLiteral("experiment_markers"), experimentMarkers);
    result.insert(QStringLiteral("source"), captureSourceCombo->currentData().toString());
    const QString serialized = QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
    if (tokenEstimateLabel)
        tokenEstimateLabel->setText(tr("Evidence: approximately %1 tokens").arg(estimatedTokens(serialized)));
    *ok = true;
    return result;
}

void AIWorkbenchWindow::sendChat(const QString &model, const QString &systemPrompt,
                                 const QString &userPrompt, RequestPurpose purpose)
{
    QString budgetError;
    const QString completePrompt = systemPrompt + userPrompt;
    tokenEstimateLabel->setText(tr("Estimated prompt: %1 tokens")
        .arg(estimatedTokens(completePrompt)));
    if (!checkResourceBudget(completePrompt, &budgetError))
    {
        resultOutput->setPlainText(budgetError);
        if (purpose == RequestPurpose::Chat || purpose == RequestPurpose::ChatReview)
            chatOutput->addSystemMessage(tr("Request blocked: %1").arg(budgetError));
        appendAudit(tr("Request blocked by resource guard: %1").arg(budgetError));
        setBusy(false);
        return;
    }
    if (usingOnlineProvider() && !confirmOnlineRequest(completePrompt, purpose))
    {
        appendAudit(tr("Network AI request cancelled or blocked by privacy controls"));
        setBusy(false);
        return;
    }
    if (!usingOnlineProvider() && managedRuntime->state() == QProcess::NotRunning
        && QFileInfo::exists(managedRuntimeRoot() + QStringLiteral("/bin/ollama"))
        && endpointEdit->text().trimmed().contains(QStringLiteral("127.0.0.1:11435")))
    {
        startManagedRuntime();
        setBusy(true);
        QTimer::singleShot(1000, this, [this, model, systemPrompt, userPrompt, purpose]() {
            sendChat(model, systemPrompt, userPrompt, purpose);
        });
        return;
    }
    if (usingHeadroom() && headroomRuntime->state() == QProcess::NotRunning
        && QFileInfo::exists(headroomExecutable()))
    {
        const QString key = headroomUpstreamKeyEdit->text().trimmed().isEmpty()
            ? qEnvironmentVariable("OPENAI_API_KEY").trimmed()
            : headroomUpstreamKeyEdit->text().trimmed();
        if (key.isEmpty())
        {
            chatOutput->addSystemMessage(tr(
                "Managed Headroom needs an upstream OpenAI API key."));
            setBusy(false);
            return;
        }
        startHeadroom();
        setBusy(true);
        QTimer::singleShot(1200, this,
            [this, model, systemPrompt, userPrompt, purpose]() {
                if (headroomRuntime->state() == QProcess::Running)
                    sendChat(model, systemPrompt, userPrompt, purpose);
                else
                {
                    chatOutput->addSystemMessage(tr(
                        "Managed Headroom did not start. Check its status in Online Provider."));
                    setBusy(false);
                }
            });
        return;
    }
    QJsonArray messages;
    messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("system")},
                                {QStringLiteral("content"), systemPrompt}});
    messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                {QStringLiteral("content"), userPrompt}});
    QJsonObject body;
    body.insert(QStringLiteral("model"), model);
    body.insert(QStringLiteral("stream"), false);
    if (usingOnlineProvider())
    {
        body.insert(QStringLiteral("input"), messages);
        body.insert(QStringLiteral("store"), openAIStoreCheck->isChecked());
    }
    else
        body.insert(QStringLiteral("messages"), messages);
    const bool supportsTools = model.contains(QStringLiteral("llama3.1"), Qt::CaseInsensitive)
        || model.contains(QStringLiteral("qwen"), Qt::CaseInsensitive)
        || model.contains(QStringLiteral("mistral"), Qt::CaseInsensitive)
        || usingOnlineProvider();
    if (purpose == RequestPurpose::Chat && supportsTools && !suppressChatTools)
    {
        QJsonArray tools;
        const QJsonArray definitions = selectedCapabilityCatalog.isEmpty()
            ? AIActionRegistry::catalog() : selectedCapabilityCatalog;
        for (const QJsonValue &definition : definitions)
            tools.append(usingOnlineProvider()
                ? openAIResponseTool(definition.toObject())
                : capabilityTool(definition.toObject()));
        body.insert(QStringLiteral("tools"), tools);
    }
    if (!usingOnlineProvider()
        && (purpose == RequestPurpose::PrimaryAnalysis || purpose == RequestPurpose::Review))
        body.insert(QStringLiteral("format"), QStringLiteral("json"));

    QString providerName = QStringLiteral("ollama");
    QUrl url(endpointEdit->text().trimmed() + QStringLiteral("/api/chat"));
    if (usingOnlineProvider())
    {
        providerName = usingHeadroom() ? QStringLiteral("headroom") : QStringLiteral("openai");
        QString endpoint = openAIEndpointEdit->text().trimmed();
        while (endpoint.endsWith(QLatin1Char('/'))) endpoint.chop(1);
        const QStringList apiSuffixes = {
            QStringLiteral("/v1/chat/completions"),
            QStringLiteral("/chat/completions"),
            QStringLiteral("/v1/responses"),
            QStringLiteral("/responses")
        };
        for (const QString &suffix : apiSuffixes)
        {
            if (endpoint.endsWith(suffix))
            {
                endpoint.chop(suffix.size());
                break;
            }
        }
        endpoint += endpoint.endsWith(QStringLiteral("/v1"))
            ? QStringLiteral("/responses")
            : QStringLiteral("/v1/responses");
        url = QUrl(endpoint);
    }
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (usingOnlineProvider() && !onlineApiKey().isEmpty())
        request.setRawHeader("Authorization",
                             QByteArray("Bearer ") + onlineApiKey().toUtf8());
    requestPurpose = purpose;
    activeReply = network->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    activeReply->setProperty("aiProvider", providerName);
    emit runtimeStateChanged(tr("AI: generating"));
    appendAudit(tr("AI request sent through %1 to %2; prompt approximately %3 tokens")
        .arg(providerName, url.host()).arg(estimatedTokens(completePrompt)));
    setBusy(true);
}

void AIWorkbenchWindow::handleReply(QNetworkReply *reply)
{
    if (reply->property("headroomStatsReply").toBool())
    {
        handleHeadroomStatsReply(reply);
        return;
    }
    if (reply->property("aiIgnoreReply").toBool())
    {
        reply->deleteLater();
        return;
    }
    const RequestPurpose purpose = requestPurpose;
    const QString provider = reply->property("aiProvider").toString();
    const bool networkProvider = provider == QStringLiteral("openai")
        || provider == QStringLiteral("headroom");
    const int httpStatus = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString retryAfter =
        QString::fromLatin1(reply->rawHeader("Retry-After")).trimmed();
    activeReply = nullptr;
    requestPurpose = RequestPurpose::None;
    const QByteArray data = reply->readAll();
    const QString networkError = reply->error() == QNetworkReply::NoError
        ? QString() : reply->errorString();
    reply->deleteLater();

    if (!networkError.isEmpty())
    {
        QJsonParseError providerError;
        const QJsonDocument providerDocument = QJsonDocument::fromJson(data, &providerError);
        QString detail;
        if (providerError.error == QJsonParseError::NoError)
            detail = providerDocument.object().value(QStringLiteral("error")).toObject()
                         .value(QStringLiteral("message")).toString();
        const QString failure = detail.isEmpty() ? networkError
                                                  : networkError + QStringLiteral(": ") + detail;
        const QString requestId =
            QString::fromLatin1(reply->rawHeader("x-request-id")).trimmed();
        const QString remainingTokens =
            QString::fromLatin1(reply->rawHeader("x-ratelimit-remaining-tokens")).trimmed();
        const QString remainingRequests =
            QString::fromLatin1(reply->rawHeader("x-ratelimit-remaining-requests")).trimmed();
        const QString failureMessage = tr("%1 request failed: %2")
            .arg(networkProvider ? tr("Network model") : tr("Local model"), failure);
        resultOutput->setPlainText(failureMessage);
        if (purpose == RequestPurpose::Chat || purpose == RequestPurpose::ChatReview)
        {
            chatOutput->addSystemMessage(failureMessage);
            emit chatLineAdded(tr("System"), failureMessage);
        }
        appendAudit(tr("%1 request failed (HTTP %2): %3%4%5%6")
            .arg(provider).arg(httpStatus).arg(failure,
                 requestId.isEmpty() ? QString()
                     : tr("; request ID %1").arg(requestId),
                 remainingTokens.isEmpty() ? QString()
                     : tr("; remaining tokens %1").arg(remainingTokens),
                 remainingRequests.isEmpty() ? QString()
                     : tr("; remaining requests %1").arg(remainingRequests)));
        if (networkProvider && httpStatus == 429)
        {
            const QString retryText = retryAfter.isEmpty()
                ? tr("Retry after a short delay.")
                : tr("Retry after %1 seconds.").arg(retryAfter);
            connectionStatus->setText(tr("Rate limited"));
            openAIStatus->setText(tr(
                "The selected online provider remains active but is temporarily "
                "rate limited. %1").arg(retryText));
            emit chatAvailabilityChanged(true,
                tr("Online provider rate limited - %1").arg(retryText));
            emit runtimeStateChanged(tr("AI: online rate limited"));
        }
        else
        {
            connectionStatus->setText(tr("Unavailable"));
            emit chatAvailabilityChanged(false, networkProvider
                ? tr("Network model unavailable") : tr("Local model runtime unavailable"));
            emit runtimeStateChanged(networkProvider
                ? tr("AI: network error") : tr("AI: local runtime error"));
        }
        setBusy(false);
        suppressChatTools = false;
        if (!networkProvider) stopManagedRuntime();
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        const QString failureMessage = tr("The %1 provider returned invalid JSON: %2")
            .arg(provider.isEmpty() ? QStringLiteral("local") : provider,
                 parseError.errorString());
        resultOutput->setPlainText(failureMessage);
        if (purpose == RequestPurpose::Chat || purpose == RequestPurpose::ChatReview)
        {
            chatOutput->addSystemMessage(failureMessage);
            emit chatLineAdded(tr("System"), failureMessage);
        }
        appendAudit(tr("Invalid %1 provider response").arg(provider));
        setBusy(false);
        suppressChatTools = false;
        if (!networkProvider) stopManagedRuntime();
        return;
    }

    if (purpose == RequestPurpose::ModelList)
    {
        QString previousPrimary = selectedModel(primaryModelCombo);
        QString previousReview = reviewModelCombo->count() <= 1
            ? QString() : selectedModel(reviewModelCombo);
        if (previousPrimary.isEmpty())
            previousPrimary = QSettings().value(
                QStringLiteral("AIWorkbench/PrimaryModel"), QStringLiteral("qwen3:8b")).toString();
        if (previousReview.isEmpty() || previousReview == tr("None"))
            previousReview = QSettings().value(
                QStringLiteral("AIWorkbench/ReviewModel"), QStringLiteral("gemma3:4b")).toString();
        primaryModelCombo->clear();
        reviewModelCombo->clear();
        reviewModelCombo->addItem(tr("None"), QString());
        installedModels.clear();
        const QJsonArray models = document.object().value(QStringLiteral("models")).toArray();
        for (const QJsonValue &model : models)
        {
            const QString name = model.toObject().value(QStringLiteral("name")).toString();
            if (name.isEmpty()) continue;
            installedModels.insert(name);
            addModelItem(primaryModelCombo, name);
            addModelItem(reviewModelCombo, name);
        }
        const int primaryIndex = primaryModelCombo->findData(previousPrimary);
        const int reviewIndex = reviewModelCombo->findData(previousReview);
        if (primaryIndex >= 0) primaryModelCombo->setCurrentIndex(primaryIndex);
        else if (primaryModelCombo->count() > 0) primaryModelCombo->setCurrentIndex(0);
        if (reviewIndex >= 0) reviewModelCombo->setCurrentIndex(reviewIndex);
        QSettings().setValue(QStringLiteral("AIWorkbench/PrimaryModel"), selectedModel(primaryModelCombo));
        QSettings().setValue(QStringLiteral("AIWorkbench/ReviewModel"), selectedModel(reviewModelCombo));
        connectionStatus->setText(tr("%1 model(s)").arg(models.size()));
        updateModelButtons();
        emit chatAvailabilityChanged(!models.isEmpty(),
            models.isEmpty() ? tr("No installed model")
                             : tr("Ready - model starts when you send"));
        appendAudit(tr("Discovered %1 local model(s)").arg(models.size()));
        setBusy(false);
        emit runtimeStateChanged(tr("AI: ready"));
        return;
    }

    QJsonObject responseMessage;
    QString content;
    QJsonArray nativeToolActions;
    if (networkProvider)
    {
        for (const QJsonValue &value :
             document.object().value(QStringLiteral("output")).toArray())
        {
            const QJsonObject item = value.toObject();
            const QString type = item.value(QStringLiteral("type")).toString();
            if (type == QStringLiteral("message"))
            {
                for (const QJsonValue &part :
                     item.value(QStringLiteral("content")).toArray())
                {
                    const QJsonObject output = part.toObject();
                    if (output.value(QStringLiteral("type")).toString()
                        == QStringLiteral("output_text"))
                    {
                        if (!content.isEmpty()) content += QLatin1Char('\n');
                        content += output.value(QStringLiteral("text")).toString();
                    }
                }
            }
            else if (type == QStringLiteral("function_call"))
            {
                QJsonParseError argumentsError;
                const QJsonDocument argumentsDocument = parseActionJson(
                    item.value(QStringLiteral("arguments")).toString().toUtf8(),
                    &argumentsError);
                if (argumentsError.error != QJsonParseError::NoError
                    || !argumentsDocument.isObject())
                    continue;
                QString capability;
                const QString functionName = item.value(QStringLiteral("name")).toString();
                for (const QJsonValue &definition : AIActionRegistry::catalog())
                {
                    const QString candidate =
                        definition.toObject().value(QStringLiteral("capability")).toString();
                    if (capabilityToolName(candidate) == functionName)
                    {
                        capability = candidate;
                        break;
                    }
                }
                if (!capability.isEmpty())
                    appendNormalizedAction(QJsonObject{
                        {QStringLiteral("capability"), capability},
                        {QStringLiteral("arguments"), argumentsDocument.object()}
                    }, &nativeToolActions);
            }
        }
    }
    else
    {
        responseMessage =
            document.object().value(QStringLiteral("message")).toObject();
        content = responseMessage.value(QStringLiteral("content")).toString();
    }
    for (const QJsonValue &value :
         responseMessage.value(QStringLiteral("tool_calls")).toArray())
    {
        const QJsonObject function = value.toObject().value(QStringLiteral("function")).toObject();
        const QString functionName = function.value(QStringLiteral("name")).toString();
        QJsonObject arguments = function.value(QStringLiteral("arguments")).toObject();
        if (arguments.isEmpty() && function.value(QStringLiteral("arguments")).isString())
        {
            QJsonParseError argumentsError;
            const QJsonDocument argumentsDocument = parseActionJson(
                function.value(QStringLiteral("arguments")).toString().toUtf8(), &argumentsError);
            if (argumentsError.error == QJsonParseError::NoError && argumentsDocument.isObject())
                arguments = argumentsDocument.object();
        }
        if (functionName == QStringLiteral("savvycan_action"))
            appendNormalizedAction(arguments, &nativeToolActions);
        else
        {
            QString capability;
            for (const QJsonValue &definition : AIActionRegistry::catalog())
            {
                const QString candidate =
                    definition.toObject().value(QStringLiteral("capability")).toString();
                if (capabilityToolName(candidate) == functionName)
                {
                    capability = candidate;
                    break;
                }
            }
            if (!capability.isEmpty())
                appendNormalizedAction(QJsonObject{
                    {QStringLiteral("capability"), capability},
                    {QStringLiteral("arguments"), arguments}
                }, &nativeToolActions);
        }
    }
    if (networkProvider)
    {
        const QJsonObject usage = document.object().value(QStringLiteral("usage")).toObject();
        const int inputTokens = usage.value(QStringLiteral("input_tokens")).toInt();
        const int outputTokens = usage.value(QStringLiteral("output_tokens")).toInt();
        openAIUsage->setText(tr(
            "Last API call: %1 input + %2 output tokens (one request via %3)")
            .arg(inputTokens).arg(outputTokens).arg(provider));
        openAIUsage->setToolTip(tr(
            "Input tokens include the question, selected SavvyCAN skills and tool "
            "schemas, permitted application state, and permitted chat/context data."));
        openAIStatus->setText(tr("Last network request completed successfully through %1")
                                  .arg(provider));
        if (provider == QStringLiteral("headroom"))
            requestHeadroomStats(true);
    }
    if (purpose == RequestPurpose::PrimaryAnalysis)
    {
        primaryResult = content;
        resultOutput->setPlainText(content);
        appendAudit(tr("Primary analysis completed with %1").arg(selectedModel(primaryModelCombo)));
        const QString reviewer = selectedModel(reviewModelCombo);
        if (!reviewer.isEmpty() && reviewer.compare(tr("None"), Qt::CaseInsensitive) != 0)
        {
            resultOutput->appendPlainText(tr("\n\nReviewing with %1...").arg(reviewer));
            const QString reviewPrompt = QStringLiteral(
                "Review the following CAN-analysis result against its evidence. Correct unsupported "
                "claims, preserve useful hypotheses, and return improved JSON with the same keys.\n\n"
                "Evidence:\n%1\n\nPrimary result:\n%2")
                .arg(QString::fromUtf8(QJsonDocument(currentEvidence).toJson(QJsonDocument::Compact)),
                     primaryResult);
            appendAudit(tr("Second-model review started with %1").arg(reviewer));
            sendChat(reviewer, QStringLiteral(
                "You are the independent reviewer in a CAN reverse-engineering pipeline. "
                "Reject claims that are not grounded in the supplied evidence."), reviewPrompt,
                RequestPurpose::Review);
            return;
        }
    }
    else if (purpose == RequestPurpose::Review)
    {
        resultOutput->setPlainText(content);
        appendAudit(tr("Second-model review completed with %1").arg(selectedModel(reviewModelCombo)));
    }
    else if (purpose == RequestPurpose::Chat || purpose == RequestPurpose::ChatReview)
    {
        if (purpose == RequestPurpose::Chat)
        {
            const QString reviewer = selectedModel(reviewModelCombo);
            if (nativeToolActions.isEmpty() && !reviewer.isEmpty()
                && reviewer.compare(tr("None"), Qt::CaseInsensitive) != 0)
            {
                primaryChatResult = content;
                appendAudit(tr("Chat review started with %1").arg(reviewer));
                sendChat(reviewer, QStringLiteral(
                    "Review another assistant's SavvyCAN answer. Correct unsupported claims and "
                    "unsafe actions. Preserve valid savvycan-action blocks and return the final answer. "
                    "Do not introduce scripting unless the user explicitly requested it."),
                    QStringLiteral("Application context:\n%1\n\nDraft answer:\n%2")
                        .arg(QString::fromUtf8(QJsonDocument(promptApplicationContext()).toJson(
                                 QJsonDocument::Compact)), primaryChatResult),
                    RequestPurpose::ChatReview);
                return;
            }
        }
        QString answer = content;
        QJsonArray structuredActions;
        QJsonParseError contentError;
        const QJsonDocument contentDocument = parseActionJson(content.toUtf8(), &contentError);
        if (contentError.error == QJsonParseError::NoError && contentDocument.isObject())
        {
            const QJsonObject object = contentDocument.object();
            if (object.value(QStringLiteral("answer")).isString())
                answer = object.value(QStringLiteral("answer")).toString();
            appendNormalizedAction(object, &structuredActions);
            for (const QJsonValue &value : object.value(QStringLiteral("actions")).toArray())
                if (value.isObject()) appendNormalizedAction(value.toObject(), &structuredActions);
        }
        else if (contentError.error == QJsonParseError::NoError && contentDocument.isArray())
            for (const QJsonValue &value : contentDocument.array())
                if (value.isObject()) appendNormalizedAction(value.toObject(), &structuredActions);
        if (answer.trimmed().isEmpty() && !nativeToolActions.isEmpty())
            answer = tr("Prepared %1 native SavvyCAN action(s). Access and confirmation checks will be applied.")
                         .arg(nativeToolActions.size());
        chatOutput->addMessage(tr("Assistant"), answer);
        emit chatLineAdded(tr("Assistant"), answer);
        const QRegularExpression actionExpression(
            QStringLiteral("```\\s*(?:savvycan-action|json)\\s*([\\s\\S]*?)\\s*```"),
            QRegularExpression::CaseInsensitiveOption);
        QJsonArray proposed = nativeToolActions;
        for (const QJsonValue &value : structuredActions) proposed.append(value);
        QRegularExpressionMatchIterator matches = actionExpression.globalMatch(answer);
        while (matches.hasNext())
        {
            const QRegularExpressionMatch actionMatch = matches.next();
            QJsonParseError actionError;
            const QJsonDocument actionDocument =
                parseActionJson(actionMatch.captured(1).toUtf8(), &actionError);
            if (actionError.error == QJsonParseError::NoError)
            {
                if (actionDocument.isObject())
                    appendNormalizedAction(actionDocument.object(), &proposed);
                else if (actionDocument.isArray())
                    for (const QJsonValue &value : actionDocument.array())
                        if (value.isObject()) appendNormalizedAction(value.toObject(), &proposed);
            }
        }
        if (!proposed.isEmpty())
        {
            emit actionsProposed(proposed);
            for (const QJsonValue &value : proposed)
                appendAudit(tr("Chat proposed action %1")
                    .arg(QString::fromUtf8(QJsonDocument(value.toObject()).toJson(
                        QJsonDocument::Compact))));
        }
        chatHistory += QStringLiteral("\nAssistant: ") + answer;
        if (chatHistory.size() > 48000) chatHistory = chatHistory.right(48000);
        persistChat();
        appendAudit(purpose == RequestPurpose::ChatReview
            ? tr("Reviewed chat response completed with %1").arg(selectedModel(reviewModelCombo))
            : tr("Chat response completed with %1").arg(selectedModel(primaryModelCombo)));
    }
    setBusy(false);
    suppressChatTools = false;
    emit runtimeStateChanged(tr("AI: ready"));
}

void AIWorkbenchWindow::stopRequest()
{
    if (activeReply)
    {
        activeReply->abort();
        suppressChatTools = false;
        appendAudit(tr("Request stopped by user"));
    }
}

void AIWorkbenchWindow::accessModeChanged()
{
    const bool requestedFull = accessCombo->currentIndex() == 1;
    armAccessCheck->setEnabled(requestedFull);
    if (!requestedFull)
    {
        armAccessCheck->setChecked(false);
        accessDurationCombo->setEnabled(false);
        accessArmedIndefinitely = false;
        accessStatus->setText(tr("No transmit tools exposed"));
    }
    else if (!armAccessCheck->isChecked())
    {
        accessDurationCombo->setEnabled(true);
        accessArmedIndefinitely = false;
        accessStatus->setText(tr("Disarmed"));
    }
    else
    {
        accessDurationCombo->setEnabled(true);
        const int seconds = accessDurationCombo->currentData().toInt();
        accessArmedIndefinitely = seconds == 0;
        if (accessArmedIndefinitely)
        {
            accessStatus->setText(tr("Armed indefinitely; each transmission still needs confirmation"));
            appendAudit(tr("Full access armed indefinitely"));
        }
        else
        {
            accessArmedUntil = QDateTime::currentDateTime().addSecs(seconds);
            accessStatus->setText(tr("Armed for %1; each transmission still needs confirmation")
                                      .arg(accessDurationCombo->currentText()));
            appendAudit(tr("Full access armed for %1").arg(accessDurationCombo->currentText()));
        }
    }
}

void AIWorkbenchWindow::appendAudit(const QString &event)
{
    const QString timestamp = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    QTextCursor cursor(auditOutput->document());
    cursor.movePosition(QTextCursor::Start);
    cursor.insertText(QStringLiteral("%1  %2\n").arg(timestamp, event));
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(directory);
    QFile file(QDir(directory).absoluteFilePath(QStringLiteral("ai-audit.jsonl")));
    if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        file.write(QJsonDocument(QJsonObject{{QStringLiteral("timestamp"), timestamp},
                                             {QStringLiteral("event"), event}})
                       .toJson(QJsonDocument::Compact) + '\n');
}

void AIWorkbenchWindow::setBusy(bool busy)
{
    analyzeButton->setEnabled(!busy);
    chatSendButton->setEnabled(!busy);
    stopButton->setEnabled(busy);
    providerCombo->setEnabled(!busy);
    emit chatBusyChanged(busy);
}
