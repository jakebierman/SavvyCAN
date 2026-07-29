#include <QDebug>
#include <QListWidgetItem>
#include <QFile>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QShortcut>
#include <QSet>
#include <QTableWidget>
#include <QVBoxLayout>
#include <cmath>
#include <numeric>
#include <qevent.h>
#include "snifferwindow.h"
#include "ui_snifferwindow.h"
#include "helpwindow.h"
#include "connections/canconmanager.h"
#include "SnifferDelegate.h"
#include "utility.h"

SnifferWindow::SnifferWindow(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::snifferWindow),
    mModel(this),
    mGUITimer(this),
    mFilter(false)
{
    ui->setupUi(this);
    setWindowFlags(Qt::Window);
    setProperty("helpPage", QStringLiteral("sniffer.md"));
    QShortcut *helpShortcut = new QShortcut(QKeySequence::HelpContents, this);
    helpShortcut->setContext(Qt::WindowShortcut);
    connect(helpShortcut, &QShortcut::activated, this, []() {
        HelpWindow::getRef()->showHelp(QStringLiteral("sniffer.md"));
    });
    ui->treeView->setModel(&mModel);

    sniffDel = new SnifferDelegate();
    defaultDel = ui->treeView->itemDelegate();

    /* set column width */
    ui->treeView->setColumnWidth(tc::ID, 80);
    ui->treeView->setColumnWidth(tc::LAST, 1);
    for(int i=tc::DATA_0 ; i<=tc::DATA_7 ; i++)
        ui->treeView->setColumnWidth(i, 92);
    ui->treeView->setUniformRowHeights(true);
    ui->treeView->header()->setDefaultAlignment(Qt::AlignCenter);
    //ui->treeView->setItemDelegate(new SnifferDelegate());

    /* activate sorting */
    ui->listWidget->setSortingEnabled(true);

    /* connect timer */
    connect(&mGUITimer, &QTimer::timeout, this, &SnifferWindow::update);
    mGUITimer.setInterval(200);
    connect(&mNotchTimer, &QTimer::timeout,this, &SnifferWindow::notchTick);
    mNotchTimer.setInterval(ui->spinNotchInterval->value());

    notchPingPong = false;

    /* connect buttons */
    connect(ui->btNotch, &QPushButton::clicked, &mModel, &SnifferModel::notch);
    connect(ui->btUnNotch, &QPushButton::clicked, &mModel, &SnifferModel::unNotch);
    connect(ui->btAll, &QPushButton::clicked, this, &SnifferWindow::fltAll);
    connect(ui->btNone, &QPushButton::clicked, this, &SnifferWindow::fltNone);
    connect(&mModel, &SnifferModel::idChange, this, &SnifferWindow::idChange);
    connect(ui->listWidget, &QListWidget::itemChanged, this, &SnifferWindow::itemChanged);

    connect(ui->cbFadeInactive, &QCheckBox::stateChanged, this, [this](int val){mModel.setFadeInactive(val);sniffDel->setFadeInactive(val);});
    connect(ui->cbMuteNotched, &QCheckBox::stateChanged, this, [this](int val){mModel.setMuteNotched(val);});
    connect(ui->cbNoExpire, &QCheckBox::stateChanged, this, [this](int val){mModel.setNeverExpire(val);});
    connect(ui->cbViewBits, &QCheckBox::stateChanged, this,
            [this](int val)
            {
                if (val) ui->treeView->setItemDelegate(sniffDel);
                else
                {
                    ui->treeView->setItemDelegate(defaultDel);
                }
            }
    );
    connect(ui->spinNotchInterval, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int i){mNotchTimer.setInterval(i);});
    connect(ui->spinExpireInterval, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int i){mModel.setExpireInterval(i);});

    QGroupBox *experimentBox = new QGroupBox(tr("Differential experiment"), this);
    QGridLayout *experimentLayout = new QGridLayout(experimentBox);
    QPushButton *baseline = new QPushButton(tr("Baseline"), experimentBox);
    QPushButton *action = new QPushButton(tr("Action"), experimentBox);
    QPushButton *control = new QPushButton(tr("Control"), experimentBox);
    QPushButton *stop = new QPushButton(tr("Stop"), experimentBox);
    QPushButton *analyze = new QPushButton(tr("Analyze"), experimentBox);
    mExperimentStatus = new QLabel(tr("Not recording"), experimentBox);
    mExperimentStatus->setWordWrap(true);
    experimentLayout->addWidget(baseline, 0, 0);
    experimentLayout->addWidget(action, 0, 1);
    experimentLayout->addWidget(control, 1, 0);
    experimentLayout->addWidget(stop, 1, 1);
    experimentLayout->addWidget(analyze, 2, 0, 1, 2);
    experimentLayout->addWidget(mExperimentStatus, 3, 0, 1, 2);
    ui->verticalLayout_2->insertWidget(0, experimentBox);

    QGroupBox *evidenceBox = new QGroupBox(tr("Evidence tools"), this);
    QGridLayout *evidenceLayout = new QGridLayout(evidenceBox);
    QPushButton *counters = new QPushButton(tr("Counters / checksums"), evidenceBox);
    QPushButton *diagnostics = new QPushButton(tr("Diagnostic correlation"), evidenceBox);
    QPushButton *clusters = new QPushButton(tr("Signal clusters"), evidenceBox);
    QPushButton *dbc = new QPushButton(tr("Export DBC candidates"), evidenceBox);
    evidenceLayout->addWidget(counters, 0, 0);
    evidenceLayout->addWidget(diagnostics, 0, 1);
    evidenceLayout->addWidget(clusters, 1, 0);
    evidenceLayout->addWidget(dbc, 1, 1);
    ui->verticalLayout_2->insertWidget(1, evidenceBox);
    connect(baseline, &QPushButton::clicked, this, [this]() { beginExperimentCapture(0); });
    connect(action, &QPushButton::clicked, this, [this]() { beginExperimentCapture(1); });
    connect(control, &QPushButton::clicked, this, [this]() { beginExperimentCapture(2); });
    connect(stop, &QPushButton::clicked, this, [this]() {
        mExperimentPhase = -1;
        mExperimentStatus->setText(tr("Stopped: B %1, A %2, C %3 frames")
            .arg(mBaselineFrames.size()).arg(mActionFrames.size()).arg(mControlFrames.size()));
    });
    connect(analyze, &QPushButton::clicked, this, &SnifferWindow::analyzeExperiment);
    connect(counters, &QPushButton::clicked, this, &SnifferWindow::inferCountersAndChecksums);
    connect(diagnostics, &QPushButton::clicked, this, &SnifferWindow::correlateDiagnostics);
    connect(clusters, &QPushButton::clicked, this, &SnifferWindow::clusterSignals);
    connect(dbc, &QPushButton::clicked, this, &SnifferWindow::exportDbcCandidates);
}

SnifferWindow::~SnifferWindow()
{
    closeEvent(nullptr);
    delete sniffDel;
    delete ui;
}

QJsonObject SnifferWindow::aiState() const
{
    return mModel.aiState();
}

bool SnifferWindow::executeAIRequest(const QString &operation,
                                     const QJsonObject &arguments, QString *error)
{
    Q_UNUSED(arguments);
    if (operation == QStringLiteral("clear")) {
        mModel.clear();
        qDeleteAll(mMap);
        mMap.clear();
        ui->listWidget->clear();
        mFilter = false;
        return true;
    }
    if (operation == QStringLiteral("notch")) {
        mModel.notch();
        return true;
    }
    if (operation == QStringLiteral("unnotch")) {
        mModel.unNotch();
        return true;
    }
    if (operation == QStringLiteral("filter_all")) {
        fltAll();
        return true;
    }
    if (operation == QStringLiteral("filter_none")) {
        fltNone();
        return true;
    }
    if (operation == QStringLiteral("pause")) {
        mGUITimer.stop();
        mNotchTimer.stop();
        return true;
    }
    if (operation == QStringLiteral("resume")) {
        mGUITimer.start();
        mNotchTimer.start();
        return true;
    }
    if (operation == QStringLiteral("experiment_baseline")) beginExperimentCapture(0);
    else if (operation == QStringLiteral("experiment_action")) beginExperimentCapture(1);
    else if (operation == QStringLiteral("experiment_control")) beginExperimentCapture(2);
    else if (operation == QStringLiteral("experiment_stop")) {
        mExperimentPhase = -1;
        mExperimentStatus->setText(tr("Stopped: B %1, A %2, C %3 frames")
            .arg(mBaselineFrames.size()).arg(mActionFrames.size()).arg(mControlFrames.size()));
    }
    else if (operation == QStringLiteral("experiment_analyze")) analyzeExperiment();
    else if (operation == QStringLiteral("infer_counters")) inferCountersAndChecksums();
    else if (operation == QStringLiteral("correlate_diagnostics")) correlateDiagnostics();
    else if (operation == QStringLiteral("cluster_signals")) clusterSignals();
    else {
        if (error) *error = tr("Unknown Sniffer operation.");
        return false;
    }
    return true;
}

void SnifferWindow::readSettings()
{
    QSettings settings;
    if (settings.value("Main/SaveRestorePositions", false).toBool())
    {
        resize(settings.value("Sniffer/WindowSize", QSize(1100, 750)).toSize());
        move(Utility::constrainedWindowPos(settings.value("Sniffer/WindowPos", QPoint(50, 50)).toPoint()));
        ui->treeView->setColumnWidth(0, settings.value("Sniffer/DeltaColumn", 110).toUInt());
        ui->treeView->setColumnWidth(1, settings.value("Sniffer/FrequencyColumn", 110).toUInt());
        ui->treeView->setColumnWidth(2, settings.value("Sniffer/IDColumn", 70).toUInt());
        ui->treeView->setColumnWidth(3, settings.value("Sniffer/Data0Column", 92).toUInt());
        ui->treeView->setColumnWidth(4, settings.value("Sniffer/Data1Column", 92).toUInt());
        ui->treeView->setColumnWidth(5, settings.value("Sniffer/Data2Column", 92).toUInt());
        ui->treeView->setColumnWidth(6, settings.value("Sniffer/Data3Column", 92).toUInt());
        ui->treeView->setColumnWidth(7, settings.value("Sniffer/Data4Column", 92).toUInt());
        ui->treeView->setColumnWidth(8, settings.value("Sniffer/Data5Column", 92).toUInt());
        ui->treeView->setColumnWidth(9, settings.value("Sniffer/Data6Column", 92).toUInt());
        ui->treeView->setColumnWidth(10, settings.value("Sniffer/Data7Column", 92).toUInt());
    }
}

void SnifferWindow::writeSettings()
{
    QSettings settings;

    if (settings.value("Main/SaveRestorePositions", false).toBool())
    {
        settings.setValue("Sniffer/WindowSize", size());
        settings.setValue("Sniffer/WindowPos", pos());
        settings.setValue("Sniffer/DeltaColumn", ui->treeView->columnWidth(0));
        settings.setValue("Sniffer/FrequencyColumn", ui->treeView->columnWidth(1));
        settings.setValue("Sniffer/IDColumn", ui->treeView->columnWidth(2));
        settings.setValue("Sniffer/Data0Column", ui->treeView->columnWidth(3));
        settings.setValue("Sniffer/Data1Column", ui->treeView->columnWidth(4));
        settings.setValue("Sniffer/Data2Column", ui->treeView->columnWidth(5));
        settings.setValue("Sniffer/Data3Column", ui->treeView->columnWidth(6));
        settings.setValue("Sniffer/Data4Column", ui->treeView->columnWidth(7));
        settings.setValue("Sniffer/Data5Column", ui->treeView->columnWidth(8));
        settings.setValue("Sniffer/Data6Column", ui->treeView->columnWidth(9));
        settings.setValue("Sniffer/Data7Column", ui->treeView->columnWidth(10));
    }
}

void SnifferWindow::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    connect(CANConManager::getInstance(), &CANConManager::framesReceived, &mModel, &SnifferModel::update);
    connect(CANConManager::getInstance(), &CANConManager::framesReceived,
            this, &SnifferWindow::captureExperimentFrames);
    mGUITimer.start();
    mNotchTimer.start();
    readSettings();
    qDebug() << "show";
}


void SnifferWindow::closeEvent(QCloseEvent *event)
{
    Q_UNUSED(event);
    /* stop timer */
    mGUITimer.stop();
    /* disconnect reception of frames */
    disconnect(CANConManager::getInstance(), 0, &mModel, 0);
    disconnect(CANConManager::getInstance(), 0, this, 0);
    writeSettings();
    /* clear model */
    mModel.clear();
    /* clean list */
    qDeleteAll(mMap);
    mMap.clear();
    /* reset filtering */
    mFilter = false;
}

void SnifferWindow::beginExperimentCapture(int phase)
{
    mExperimentPhase = phase;
    QVector<CANFrame> *target = phase == 0 ? &mBaselineFrames
                                : phase == 1 ? &mActionFrames : &mControlFrames;
    target->clear();
    const QString name = phase == 0 ? tr("baseline") : phase == 1 ? tr("action") : tr("control");
    mExperimentStatus->setText(tr("Recording %1...").arg(name));
}

void SnifferWindow::captureExperimentFrames(CANConnection*, QVector<CANFrame> &frames)
{
    if (mExperimentPhase < 0) return;
    QVector<CANFrame> *target = mExperimentPhase == 0 ? &mBaselineFrames
                                : mExperimentPhase == 1 ? &mActionFrames : &mControlFrames;
    for (const CANFrame &frame : frames)
        if (frame.frameType() == QCanBusFrame::DataFrame && frame.isReceived)
            target->append(frame);
    constexpr int maximumFrames = 250000;
    if (target->size() > maximumFrames)
        target->remove(0, target->size() - maximumFrames);
    mExperimentStatus->setText(tr("Recording: %1 frames").arg(target->size()));
}

QVector<CANFrame> SnifferWindow::experimentFrames() const
{
    QVector<CANFrame> frames = mBaselineFrames;
    frames += mActionFrames;
    frames += mControlFrames;
    return frames;
}

double SnifferWindow::correlation(const QVector<double> &left, const QVector<double> &right)
{
    const int count = qMin(left.size(), right.size());
    if (count < 4) return 0.0;
    double leftMean = 0.0, rightMean = 0.0;
    for (int i = 0; i < count; ++i) {
        leftMean += left[i];
        rightMean += right[i];
    }
    leftMean /= count;
    rightMean /= count;
    double numerator = 0.0, leftPower = 0.0, rightPower = 0.0;
    for (int i = 0; i < count; ++i) {
        const double a = left[i] - leftMean;
        const double b = right[i] - rightMean;
        numerator += a * b;
        leftPower += a * a;
        rightPower += b * b;
    }
    if (leftPower == 0.0 || rightPower == 0.0) return 0.0;
    return numerator / std::sqrt(leftPower * rightPower);
}

void SnifferWindow::addAnalysisRow(const QString &type, quint32 id, const QString &field,
                                   double score, const QString &evidence)
{
    if (!mAnalysisTable) return;
    const int row = mAnalysisTable->rowCount();
    mAnalysisTable->insertRow(row);
    const QStringList fields = {type,
        QStringLiteral("0x%1").arg(id, id > 0x7FF ? 8 : 3, 16, QLatin1Char('0')).toUpper(),
        field, QString::number(score, 'f', 1), evidence};
    for (int col = 0; col < fields.size(); ++col)
        mAnalysisTable->setItem(row, col, new QTableWidgetItem(fields[col]));
    mAnalysisTable->item(row, 0)->setData(Qt::UserRole, id);
    mAnalysisTable->item(row, 3)->setData(Qt::UserRole, score);
}

void SnifferWindow::ensureAnalysisWindow()
{
    if (mAnalysisDialog) return;
    mAnalysisDialog = new QDialog(this);
    mAnalysisDialog->setWindowTitle(tr("Reverse-engineering evidence"));
    mAnalysisDialog->resize(1050, 650);
    QVBoxLayout *layout = new QVBoxLayout(mAnalysisDialog);
    mAnalysisTable = new QTableWidget(0, 5, mAnalysisDialog);
    mAnalysisTable->setHorizontalHeaderLabels(
        {tr("Analysis"), tr("CAN ID"), tr("Field"), tr("Score"), tr("Evidence")});
    mAnalysisTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    mAnalysisTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    mAnalysisTable->setSortingEnabled(true);
    mAnalysisTable->resizeColumnsToContents();
    mAnalysisTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    mAnalysisTable->horizontalHeader()->setStretchLastSection(true);
    QHBoxLayout *buttons = new QHBoxLayout;
    QPushButton *counters = new QPushButton(tr("Counters / checksums"), mAnalysisDialog);
    QPushButton *diagnostics = new QPushButton(tr("Diagnostic correlation"), mAnalysisDialog);
    QPushButton *clusters = new QPushButton(tr("Signal clusters"), mAnalysisDialog);
    QPushButton *dbc = new QPushButton(tr("Export DBC candidates"), mAnalysisDialog);
    buttons->addWidget(counters);
    buttons->addWidget(diagnostics);
    buttons->addWidget(clusters);
    buttons->addStretch();
    buttons->addWidget(dbc);
    layout->addWidget(mAnalysisTable);
    layout->addLayout(buttons);
    connect(counters, &QPushButton::clicked, this, &SnifferWindow::inferCountersAndChecksums);
    connect(diagnostics, &QPushButton::clicked, this, &SnifferWindow::correlateDiagnostics);
    connect(clusters, &QPushButton::clicked, this, &SnifferWindow::clusterSignals);
    connect(dbc, &QPushButton::clicked, this, &SnifferWindow::exportDbcCandidates);
}

void SnifferWindow::analyzeExperiment()
{
    if (mBaselineFrames.isEmpty() || mActionFrames.isEmpty()) {
        QMessageBox::information(this, tr("Differential experiment"),
            tr("Record both baseline and action samples first."));
        return;
    }
    ensureAnalysisWindow();
    mAnalysisTable->setSortingEnabled(false);
    mAnalysisTable->setRowCount(0);

    using Counts = QHash<quint32, QVector<int>>;
    auto bitCounts = [](const QVector<CANFrame> &frames) {
        Counts result;
        QHash<quint32, int> totals;
        for (const CANFrame &frame : frames) {
            QVector<int> &counts = result[frame.frameId()];
            if (counts.isEmpty()) counts.fill(0, 64);
            const QByteArray data = frame.payload();
            for (int byte = 0; byte < qMin(8, data.size()); ++byte)
                for (int bit = 0; bit < 8; ++bit)
                    if (quint8(data[byte]) & (1 << bit)) ++counts[byte * 8 + bit];
            ++totals[frame.frameId()];
        }
        return qMakePair(result, totals);
    };
    const auto baseline = bitCounts(mBaselineFrames);
    const auto action = bitCounts(mActionFrames);
    const auto control = bitCounts(mControlFrames);
    for (auto it = action.first.cbegin(); it != action.first.cend(); ++it) {
        const int actionTotal = action.second.value(it.key());
        const int baselineTotal = baseline.second.value(it.key());
        if (!actionTotal || !baselineTotal) continue;
        for (int bit = 0; bit < 64; ++bit) {
            const double actionRate = double(it.value()[bit]) / actionTotal;
            const double baselineRate = double(baseline.first.value(it.key()).value(bit)) / baselineTotal;
            const int controlTotal = control.second.value(it.key());
            const double controlRate = controlTotal
                ? double(control.first.value(it.key()).value(bit)) / controlTotal : baselineRate;
            const double effect = std::abs(actionRate - baselineRate);
            const double background = std::abs(controlRate - baselineRate);
            const double score = qBound(0.0, (effect - background) * 100.0, 100.0);
            if (score < 15.0) continue;
            addAnalysisRow(tr("Differential"), it.key(),
                tr("byte %1 bit %2").arg(bit / 8).arg(bit % 8), score,
                tr("baseline %1%, action %2%, control %3%")
                    .arg(baselineRate * 100.0, 0, 'f', 1)
                    .arg(actionRate * 100.0, 0, 'f', 1)
                    .arg(controlRate * 100.0, 0, 'f', 1));
        }
    }
    mAnalysisTable->setSortingEnabled(true);
    mAnalysisTable->sortItems(3, Qt::DescendingOrder);
    mAnalysisDialog->show();
    mAnalysisDialog->raise();
}

void SnifferWindow::inferCountersAndChecksums()
{
    const QVector<CANFrame> all = experimentFrames();
    if (all.isEmpty()) {
        QMessageBox::information(this, tr("Counters / checksums"),
            tr("Record a baseline, action, or control sample first."));
        return;
    }
    ensureAnalysisWindow();
    QHash<quint32, QVector<QByteArray>> samples;
    for (const CANFrame &frame : all)
        if (!frame.payload().isEmpty()) samples[frame.frameId()].append(frame.payload());
    mAnalysisTable->setSortingEnabled(false);
    for (auto it = samples.cbegin(); it != samples.cend(); ++it) {
        if (it.value().size() < 6) continue;
        const int length = it.value().first().size();
        for (int byte = 0; byte < length; ++byte) {
            int increments = 0, transitions = 0;
            for (int i = 1; i < it.value().size(); ++i) {
                if (it.value()[i].size() != length) continue;
                const int previous = quint8(it.value()[i - 1][byte]);
                const int current = quint8(it.value()[i][byte]);
                if (current != previous) ++transitions;
                if (current == ((previous + 1) & 0xFF) ||
                    (current & 0x0F) == ((previous + 1) & 0x0F)) ++increments;
            }
            const double counterScore = transitions ? double(increments) * 100.0 / transitions : 0.0;
            if (counterScore >= 70.0)
                addAnalysisRow(tr("Counter"), it.key(), tr("byte %1").arg(byte),
                    counterScore, tr("Sequential in %1/%2 transitions").arg(increments).arg(transitions));

            int xorMatches = 0, sumMatches = 0, valid = 0;
            for (const QByteArray &payload : it.value()) {
                if (payload.size() != length) continue;
                quint8 x = 0, sum = 0;
                for (int pos = 0; pos < length; ++pos) if (pos != byte) {
                    x ^= quint8(payload[pos]);
                    sum = quint8(sum + quint8(payload[pos]));
                }
                const quint8 observed = quint8(payload[byte]);
                if (observed == x || observed == quint8(~x)) ++xorMatches;
                if (observed == sum || observed == quint8(0 - sum) || observed == quint8(0xFF - sum))
                    ++sumMatches;
                ++valid;
            }
            const int best = qMax(xorMatches, sumMatches);
            const double checksumScore = valid ? double(best) * 100.0 / valid : 0.0;
            if (checksumScore >= 70.0)
                addAnalysisRow(tr("Checksum"), it.key(), tr("byte %1").arg(byte),
                    checksumScore, xorMatches >= sumMatches
                        ? tr("Matches XOR/XOR complement in %1/%2 frames").arg(xorMatches).arg(valid)
                        : tr("Matches sum/complement in %1/%2 frames").arg(sumMatches).arg(valid));
        }
    }
    mAnalysisTable->setSortingEnabled(true);
    mAnalysisDialog->show();
    mAnalysisDialog->raise();
}

void SnifferWindow::correlateDiagnostics()
{
    const QVector<CANFrame> all = experimentFrames();
    if (all.isEmpty()) {
        QMessageBox::information(this, tr("Diagnostic correlation"),
            tr("Record experiment traffic containing diagnostics and broadcast frames first."));
        return;
    }
    ensureAnalysisWindow();
    struct Series { QString name; quint32 id; QVector<double> values; };
    QList<Series> diagnostic;
    QHash<QString, Series> broadcast;
    for (const CANFrame &frame : all) {
        const QByteArray raw = frame.payload();
        const bool singleFrame = raw.size() >= 3 && (quint8(raw[0]) & 0xF0) == 0 &&
                                 (quint8(raw[0]) & 0x0F) <= raw.size() - 1;
        const QByteArray payload = singleFrame
            ? raw.mid(1, quint8(raw[0]) & 0x0F) : QByteArray();
        if (payload.size() >= 4 && quint8(payload[0]) == 0x62) {
            const QString did = QStringLiteral("DID %1%2")
                .arg(quint8(payload[1]), 2, 16, QLatin1Char('0'))
                .arg(quint8(payload[2]), 2, 16, QLatin1Char('0')).toUpper();
            auto found = std::find_if(diagnostic.begin(), diagnostic.end(),
                [&](const Series &series) { return series.name == did; });
            if (found == diagnostic.end()) {
                diagnostic.append({did, frame.frameId(), {}});
                found = diagnostic.end() - 1;
            }
            quint32 value = 0;
            for (char byte : payload.mid(3, 4)) value = (value << 8) | quint8(byte);
            found->values.append(value);
        } else if (payload.size() >= 3 && quint8(payload[0]) >= 0x41 &&
                   quint8(payload[0]) <= 0x4A) {
            const QString pid = QStringLiteral("Mode %1 PID %2")
                .arg(quint8(payload[0]) - 0x40, 2, 16, QLatin1Char('0'))
                .arg(quint8(payload[1]), 2, 16, QLatin1Char('0')).toUpper();
            auto found = std::find_if(diagnostic.begin(), diagnostic.end(),
                [&](const Series &series) { return series.name == pid; });
            if (found == diagnostic.end()) {
                diagnostic.append({pid, frame.frameId(), {}});
                found = diagnostic.end() - 1;
            }
            quint32 value = 0;
            for (char byte : payload.mid(2, 4)) value = (value << 8) | quint8(byte);
            found->values.append(value);
        } else {
            for (int byte = 0; byte < qMin(8, raw.size()); ++byte) {
                const QString key = QStringLiteral("%1:%2").arg(frame.frameId()).arg(byte);
                Series &series = broadcast[key];
                series.name = tr("byte %1").arg(byte);
                series.id = frame.frameId();
                series.values.append(quint8(raw[byte]));
            }
        }
    }
    mAnalysisTable->setSortingEnabled(false);
    for (const Series &diag : diagnostic) {
        for (const Series &candidate : broadcast) {
            const double corr = correlation(diag.values, candidate.values);
            if (std::abs(corr) < 0.85) continue;
            addAnalysisRow(tr("Diagnostic correlation"), candidate.id, candidate.name,
                std::abs(corr) * 100.0,
                tr("%1 correlation with %2 (%3 samples)")
                    .arg(corr >= 0 ? tr("Positive") : tr("Inverse"))
                    .arg(diag.name).arg(qMin(diag.values.size(), candidate.values.size())));
        }
    }
    mAnalysisTable->setSortingEnabled(true);
    mAnalysisDialog->show();
    mAnalysisDialog->raise();
}

void SnifferWindow::clusterSignals()
{
    const QVector<CANFrame> all = experimentFrames();
    if (all.isEmpty()) {
        QMessageBox::information(this, tr("Signal clusters"),
            tr("Record a baseline, action, or control sample first."));
        return;
    }
    ensureAnalysisWindow();
    struct Series { quint32 id; int byte; QVector<double> values; };
    QHash<QString, Series> byField;
    for (const CANFrame &frame : all) {
        if (frame.frameId() >= 0x700) continue;
        for (int byte = 0; byte < qMin(8, frame.payload().size()); ++byte) {
            const QString key = QStringLiteral("%1:%2").arg(frame.frameId()).arg(byte);
            Series &series = byField[key];
            series.id = frame.frameId();
            series.byte = byte;
            series.values.append(quint8(frame.payload()[byte]));
        }
    }
    const QList<Series> fields = byField.values();
    mAnalysisTable->setSortingEnabled(false);
    for (int left = 0; left < fields.size(); ++left) {
        for (int right = left + 1; right < fields.size(); ++right) {
            if (fields[left].id == fields[right].id) continue;
            const double corr = correlation(fields[left].values, fields[right].values);
            if (std::abs(corr) < 0.92) continue;
            addAnalysisRow(tr("Signal cluster"), fields[left].id,
                tr("byte %1").arg(fields[left].byte), std::abs(corr) * 100.0,
                tr("%1 correlated with 0x%2 byte %3")
                    .arg(corr >= 0 ? tr("Positively") : tr("Inversely"))
                    .arg(fields[right].id, 3, 16, QLatin1Char('0')).arg(fields[right].byte));
        }
    }
    mAnalysisTable->setSortingEnabled(true);
    mAnalysisDialog->show();
    mAnalysisDialog->raise();
}

void SnifferWindow::exportDbcCandidates()
{
    ensureAnalysisWindow();
    if (mAnalysisTable->rowCount() == 0) {
        QMessageBox::information(this, tr("Export DBC candidates"),
            tr("Run an evidence analysis first so there are candidate rows to export."));
        return;
    }
    const QString path = QFileDialog::getSaveFileName(this, tr("Export DBC candidates"),
        QString(), tr("DBC files (*.dbc)"));
    if (path.isEmpty()) return;
    QMap<quint32, QSet<int>> messageBytes;
    const QModelIndexList selected = mAnalysisTable->selectionModel()->selectedRows();
    auto includeRow = [&](int row) {
        const quint32 id = mAnalysisTable->item(row, 0)->data(Qt::UserRole).toUInt();
        const QString field = mAnalysisTable->item(row, 2)->text();
        QRegularExpression match(QStringLiteral("byte\\s+(\\d+)"));
        const QRegularExpressionMatch result = match.match(field);
        if (result.hasMatch()) messageBytes[id].insert(result.captured(1).toInt());
    };
    if (selected.isEmpty())
        for (int row = 0; row < mAnalysisTable->rowCount(); ++row) includeRow(row);
    else
        for (const QModelIndex &index : selected) includeRow(index.row());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Export DBC candidates"), file.errorString());
        return;
    }
    QByteArray output("VERSION \"SavvyCAN reverse-engineering candidates\"\n\n"
                      "NS_ :\n\tCM_\n\tBA_DEF_\n\tBA_\n\tVAL_\n\n"
                      "BS_:\n\nBU_: Vector__XXX\n\n");
    for (auto it = messageBytes.cbegin(); it != messageBytes.cend(); ++it) {
        output += QStringLiteral("BO_ %1 Candidate_%2: 8 Vector__XXX\n")
            .arg(it.key()).arg(it.key(), 0, 16).toLatin1();
        for (int byte : it.value()) {
            output += QStringLiteral(" SG_ Candidate_Byte_%1 : %2|8@1+ (1,0) [0|255] \"\" Vector__XXX\n")
                .arg(byte).arg(byte * 8).toLatin1();
        }
        output += '\n';
    }
    file.write(output);
}

void SnifferWindow::update()
{
    mModel.refresh();
}

void SnifferWindow::notchTick()
{
    notchPingPong = !notchPingPong;
    if (notchPingPong)
    {
        ui->lblNotch->setBackgroundRole(QPalette::Link);
        ui->lblNotch->repaint();
        //qDebug() << "Tick";
    }
    else
    {
        ui->lblNotch->setBackgroundRole(QPalette::Window);
        ui->lblNotch->repaint();
        //qDebug() << "Tock";
    }
    mModel.updateNotchPoint();
}

void SnifferWindow::fltAll()
{
    filter(false);
}


void SnifferWindow::fltNone()
{
    filter(true);
}

void SnifferWindow::filter(bool pFilter)
{
    mFilter = pFilter;
    mModel.filter(mFilter ? fltType::NONE : fltType::ALL);

    foreach(QListWidgetItem* item, mMap)
        item->setCheckState(mFilter ? Qt::Unchecked : Qt::Checked);
}

void SnifferWindow::idChange(int pId, bool pAdd)
{
    QListWidgetItem* item;

    if(pAdd)
    {
        QString text = QString("0x") + QString("%1").arg(pId, 3, 16, QLatin1Char('0')).toUpper();
        item = new QListWidgetItem(text);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        //item->setCheckState(mFilter ? Qt::Unchecked : Qt::Checked);
        //always check new IDs now. Otherwise any that expire then come back will not be selected
        //and that might be a bigger issue than defaulting them unselected.
        item->setCheckState(Qt::Checked);
        ui->listWidget->addItem(item);
        mMap[pId] = item;
    }
    else
    {
        item = mMap.take(pId);
        ui->listWidget->removeItemWidget(item);
        delete item;
    }
}


void SnifferWindow::itemChanged(QListWidgetItem * item)
{
    bool checked = (Qt::Checked == item->checkState());

    if( !mFilter && checked )
        return;

    mModel.filter(checked ? fltType::ADD : fltType::REMOVE, mMap.key(item));
    mFilter = true;
}
