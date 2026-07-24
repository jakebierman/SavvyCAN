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
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
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
        if (description.contains(QStringLiteral("boolean"), Qt::CaseInsensitive))
            type = QStringLiteral("boolean");
        else if (description.contains(QStringLiteral("integer"), Qt::CaseInsensitive)
                 || description.contains(QRegularExpression(QStringLiteral("^\\d+-\\d+$"))))
            type = QStringLiteral("integer");
        else if (description.contains(QStringLiteral("number"), Qt::CaseInsensitive))
            type = QStringLiteral("number");
        properties.insert(iterator.key(), QJsonObject{
            {QStringLiteral("type"), type},
            {QStringLiteral("description"), description}
        });
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
        auditOutput->setPlainText(QString::fromUtf8(data));
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
    connect(resourceTimer, &QTimer::timeout, this, &AIWorkbenchWindow::updateResourceStats);
    connect(gpuStatsProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus) { gpuStatsFinished(exitCode); });
    connect(CANConManager::getInstance(), &CANConManager::framesReceived,
            this, &AIWorkbenchWindow::handleLiveFrames);
    connect(liveAnalysisTimer, &QTimer::timeout, this, &AIWorkbenchWindow::liveAnalysisTick);
    resourceTimer->start(2000);
    updateResourceStats();
    if (QFileInfo::exists(managedRuntimeRoot() + QStringLiteral("/bin/ollama")))
    {
        startManagedRuntime();
        QTimer::singleShot(700, this, &AIWorkbenchWindow::refreshModels);
    }
    else
        refreshModels();
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

void AIWorkbenchWindow::recordActionResult(const QString &capability, bool success,
                                           const QString &message)
{
    const QString result = tr("Action %1: %2 (%3)")
        .arg(capability, success ? tr("success") : tr("failed"), message);
    appendAudit(result);
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
        "Access checks and auditing always remain active. No-popups mode executes "
        "validated model actions immediately while full access is armed."));
    armAccessCheck = new QCheckBox(tr("Arm full access"), scope);
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
    scopeLayout->addRow(tr("Confirm sends"), confirmationCombo);
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
    chatInput->setPlaceholderText(tr("Message the local model..."));
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
    auditOutput = new QPlainTextEdit(outputs);
    evidenceOutput->setReadOnly(true);
    resultOutput->setReadOnly(true);
    auditOutput->setReadOnly(true);
    outputs->addTab(chatPanel, tr("Chat"));
    outputs->addTab(resultOutput, tr("Analysis"));
    outputs->addTab(evidenceOutput, tr("Evidence"));
    outputs->addTab(auditOutput, tr("Audit"));
    root->addWidget(outputs, 1);

    connect(refreshButton, &QPushButton::clicked, this, &AIWorkbenchWindow::refreshModels);
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
    connect(primaryModelCombo, &QComboBox::currentTextChanged, this, [](const QString &model) {
        if (!model.isEmpty()) QSettings().setValue(QStringLiteral("AIWorkbench/PrimaryModel"), model);
    });
    connect(reviewModelCombo, &QComboBox::currentTextChanged, this, [](const QString &model) {
        if (!model.isEmpty()) QSettings().setValue(QStringLiteral("AIWorkbench/ReviewModel"), model);
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
    resultOutput->setPlainText(tr("Analyzing locally..."));
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
    if (question.isEmpty() || activeReply) return;

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
        "uses frame.send_loop so it is visible and manageable in Frame Sender.")
        .arg(capabilityContext(question)
                 + QStringLiteral("\n\nCurrent application state:\n")
                 + QString::fromUtf8(QJsonDocument(applicationContext()).toJson(QJsonDocument::Compact)),
             compressedChatHistory(), question, context);
    chatHistory += QStringLiteral("\nUser: ") + question;
    persistChat();
    appendAudit(tr("Chat request started with %1").arg(model));
    sendChat(model, QStringLiteral(
        "You are a local assistant embedded in SavvyCAN. Help with CAN reverse engineering, "
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
    QStringList pages;
    pages << QStringLiteral("mainscreen.md") << QStringLiteral("ai_capability_audit.md");
    if (lower.contains(QStringLiteral("payload")) || lower.contains(QStringLiteral("format"))
        || lower.contains(QStringLiteral("decode")) || lower.contains(QStringLiteral("bit")))
        pages << QStringLiteral("payload_formatter.md");
    if (lower.contains(QStringLiteral("uds")) || lower.contains(QStringLiteral("did"))
        || lower.contains(QStringLiteral("diagnostic")))
        pages << QStringLiteral("uds_workbench.md");
    if (lower.contains(QStringLiteral("obd")) || lower.contains(QStringLiteral("pid")))
        pages << QStringLiteral("obd2_workbench.md");
    if (lower.contains(QStringLiteral("canopen")) || lower.contains(QStringLiteral("sdo"))
        || lower.contains(QStringLiteral("object dictionary")))
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

    QString result = AIActionRegistry::catalogText();
    for (const QString &page : pages)
    {
        const QString documentation = readHelpPage(page, 7000);
        if (!documentation.isEmpty())
            result += QStringLiteral("\n\n--- %1 ---\n%2").arg(page, documentation);
    }
    return result.left(24000);
}

void AIWorkbenchWindow::clearChat()
{
    chatHistory.clear();
    chatOutput->clearMessages();
    emit chatCleared();
    persistChat();
    appendAudit(tr("Chat history cleared"));
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
    if (managedRuntime->state() == QProcess::NotRunning
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
    QJsonArray messages;
    messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("system")},
                                {QStringLiteral("content"), systemPrompt}});
    messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                {QStringLiteral("content"), userPrompt}});
    QJsonObject body{
        {QStringLiteral("model"), model},
        {QStringLiteral("stream"), false},
        {QStringLiteral("messages"), messages}
    };
    const bool supportsTools = model.contains(QStringLiteral("llama3.1"), Qt::CaseInsensitive)
        || model.contains(QStringLiteral("qwen"), Qt::CaseInsensitive)
        || model.contains(QStringLiteral("mistral"), Qt::CaseInsensitive);
    if (purpose == RequestPurpose::Chat && supportsTools)
    {
        QJsonArray tools;
        for (const QJsonValue &definition : AIActionRegistry::catalog())
            tools.append(capabilityTool(definition.toObject()));
        body.insert(QStringLiteral("tools"), tools);
    }
    if (purpose == RequestPurpose::PrimaryAnalysis || purpose == RequestPurpose::Review)
        body.insert(QStringLiteral("format"), QStringLiteral("json"));
    QNetworkRequest request(QUrl(endpointEdit->text().trimmed() + QStringLiteral("/api/chat")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    requestPurpose = purpose;
    activeReply = network->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    emit runtimeStateChanged(tr("AI: generating"));
    setBusy(true);
}

void AIWorkbenchWindow::handleReply(QNetworkReply *reply)
{
    const RequestPurpose purpose = requestPurpose;
    activeReply = nullptr;
    requestPurpose = RequestPurpose::None;
    const QByteArray data = reply->readAll();
    const QString networkError = reply->error() == QNetworkReply::NoError
        ? QString() : reply->errorString();
    reply->deleteLater();

    if (!networkError.isEmpty())
    {
        connectionStatus->setText(tr("Unavailable"));
        resultOutput->setPlainText(tr("Local model request failed: %1").arg(networkError));
        appendAudit(tr("Request failed: %1").arg(networkError));
        emit chatAvailabilityChanged(false, tr("Local model runtime unavailable"));
        setBusy(false);
        stopManagedRuntime();
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        resultOutput->setPlainText(tr("The local runtime returned invalid JSON: %1").arg(parseError.errorString()));
        appendAudit(tr("Invalid runtime response"));
        setBusy(false);
        stopManagedRuntime();
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

    const QJsonObject responseMessage =
        document.object().value(QStringLiteral("message")).toObject();
    const QString content = responseMessage.value(QStringLiteral("content")).toString();
    QJsonArray nativeToolActions;
    for (const QJsonValue &value : responseMessage.value(QStringLiteral("tool_calls")).toArray())
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
                        .arg(QString::fromUtf8(QJsonDocument(applicationContext()).toJson(
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
    emit runtimeStateChanged(tr("AI: ready"));
}

void AIWorkbenchWindow::stopRequest()
{
    if (activeReply)
    {
        activeReply->abort();
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
    auditOutput->appendPlainText(QStringLiteral("%1  %2").arg(timestamp, event));
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
    emit chatBusyChanged(busy);
}
