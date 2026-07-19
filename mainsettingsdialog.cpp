#include "mainsettingsdialog.h"
#include "ui_mainsettingsdialog.h"
#include "helpwindow.h"
#include <qevent.h>
#include <QDebug>
#include <QDir>
#include <QRegExp>
#include <QCoreApplication>
#include <QDebug>
#include "simplecrypt.h"
#include "payloadformatter.h"

//using this simple encryption library to obfuscate stored password a bit. It's not super secure but better than
//storing a password in straight plaintext. You have the source to this application anyway, whatever algorithm used,
//whatever key, you'd see it. Just behave yourselves
SimpleCrypt crypto(Q_UINT64_C(0xdeadbeefface6285));

static QStringList availableLanguageCodes()
{
    QStringList codes;
    QDir transDir(QCoreApplication::applicationDirPath() + "/translations");
    QStringList list = transDir.entryList(QStringList() << "SavvyCAN_*.qm");
    //* just for test purposes
    if (list.isEmpty()) {
        // maybe only ts files exist during development
        list = transDir.entryList(QStringList() << "SavvyCAN_*.ts");
    }
    for (QString file : list) {
        QRegExp re("SavvyCAN_(.*)\\.(qm|ts)");
        if (re.indexIn(file) != -1) {
            codes << re.cap(1);
        }
    }
    return codes;
}

void MainSettingsDialog::populateLanguageCombo()
{
    ui->comboLanguage->clear();
    QStringList codes = availableLanguageCodes();
    if (codes.isEmpty()) {
        codes << "en"; // fallback
    }
    for (const QString &code : codes) {
        QLocale locale(code);
        QString name = locale.nativeLanguageName();
        qInfo() << code;
        if (name.isEmpty())
            name = code;
        ui->comboLanguage->addItem(name, code);
    }
}

MainSettingsDialog::MainSettingsDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::MainSettingsDialog)
{
    QSettings settings;
    ui->setupUi(this);

    populateLanguageCombo();

    //TODO: This is still hard coded to support only two buses. Sometimes there is none, sometimes 1, sometimes much more than 2. Fix this.
    ui->comboSendingBus->addItem(tr("None"));
    ui->comboSendingBus->addItem(tr("0"));
    ui->comboSendingBus->addItem(tr("1"));
    ui->comboSendingBus->addItem(tr("All"));
    ui->comboSendingBus->addItem(tr("From File"));

    ui->comboPayloadDisplay->addItem(tr("Raw hexadecimal"), QStringLiteral("raw-hex"));
    ui->comboPayloadDisplay->addItem(tr("Raw decimal"), QStringLiteral("raw-decimal"));
    ui->comboPayloadDisplay->addItem(tr("Unsigned 8-bit"), QStringLiteral("u8"));
    ui->comboPayloadDisplay->addItem(tr("Signed 8-bit"), QStringLiteral("i8"));
    ui->comboPayloadDisplay->addItem(tr("Unsigned 16-bit, little endian"), QStringLiteral("u16le"));
    ui->comboPayloadDisplay->addItem(tr("Unsigned 16-bit, big endian"), QStringLiteral("u16be"));
    ui->comboPayloadDisplay->addItem(tr("Signed 16-bit, little endian"), QStringLiteral("i16le"));
    ui->comboPayloadDisplay->addItem(tr("Signed 16-bit, big endian"), QStringLiteral("i16be"));
    ui->comboPayloadDisplay->addItem(tr("Unsigned 32-bit, little endian"), QStringLiteral("u32le"));
    ui->comboPayloadDisplay->addItem(tr("Unsigned 32-bit, big endian"), QStringLiteral("u32be"));
    ui->comboPayloadDisplay->addItem(tr("Signed 32-bit, little endian"), QStringLiteral("i32le"));
    ui->comboPayloadDisplay->addItem(tr("Signed 32-bit, big endian"), QStringLiteral("i32be"));
    ui->comboPayloadDisplay->addItem(tr("Custom format"), QStringLiteral("custom"));
    ui->comboPayloadDockPosition->addItem(tr("Remember payload panel position"), QStringLiteral("remember"));
    ui->comboPayloadDockPosition->addItem(tr("Payload panel on right"), QStringLiteral("right"));
    ui->comboPayloadDockPosition->addItem(tr("Payload panel on bottom"), QStringLiteral("bottom"));

    //update the GUI with all the settings we have stored giving things
    //defaults if nothing was stored (if this is the first time)
    ui->cbDisplayHex->setChecked(settings.value("Main/UseHex", true).toBool());
    QString payloadMode;
    if (settings.contains("Main/PayloadDisplayMode"))
        payloadMode = settings.value("Main/PayloadDisplayMode").toString();
    else
        payloadMode = settings.value("Main/UseHex", true).toBool() ? QStringLiteral("raw-hex") : QStringLiteral("raw-decimal");
    int payloadModeIndex = ui->comboPayloadDisplay->findData(payloadMode);
    if (payloadModeIndex < 0)
        payloadModeIndex = ui->comboPayloadDisplay->findData(QStringLiteral("custom"));
    ui->comboPayloadDisplay->setCurrentIndex(payloadModeIndex);
    ui->linePayloadFormat->setText(settings.value("Main/PayloadFormat", QStringLiteral("u16le")).toString());
    ui->cbShowRawPayload->setChecked(settings.value("Main/PayloadShowRaw", false).toBool());
    const int dockPosition = ui->comboPayloadDockPosition->findData(
        settings.value("Main/PayloadDockPreference", QStringLiteral("remember")).toString());
    ui->comboPayloadDockPosition->setCurrentIndex(dockPosition < 0 ? 0 : dockPosition);
    ui->cbFlowAutoRef->setChecked(settings.value("FlowView/AutoRef", false).toBool());
    ui->cbHexGraphFlow->setChecked(settings.value("FlowView/GraphHex", false).toBool());
    ui->cbFlowUseTimestamp->setChecked(settings.value("FlowView/UseTimestamp", true).toBool());
    ui->cbHexGraphInfo->setChecked(settings.value("InfoCompare/GraphHex", false).toBool());
    ui->cbInfoAutoExpand->setChecked(settings.value("InfoCompare/AutoExpand", false).toBool());
    ui->cbMainAutoScroll->setChecked(settings.value("Main/AutoScroll", false).toBool());
    ui->cbPlaybackLoop->setChecked(settings.value("Playback/AutoLoop", false).toBool());
    ui->cbRestorePositions->setChecked(settings.value("Main/SaveRestorePositions", true).toBool());
    ui->cbValidate->setChecked(settings.value("Main/ValidateComm", true).toBool());
    ui->spinPlaybackSpeed->setValue(settings.value("Playback/DefSpeed", 5).toInt());
    ui->lineClockFormat->setText(settings.value("Main/TimeFormat", "MMM-dd HH:mm:ss.zzz").toString());
    ui->lineRemoteHost->setText(settings.value("Remote/Host", "api.savvycan.com").toString());
    ui->lineRemotePort->setText(settings.value("Remote/Port", "8883").toString()); //default port for SSL enabled MQTT
    ui->lineRemoteUser->setText(settings.value("Remote/User", "Anonymous").toString());
    QByteArray encPass = settings.value("Remote/Pass", "").toByteArray();
    QString decPass = crypto.decryptToString(encPass);
    ui->lineRemotePassword->setText(decPass);

    ui->cbLoadConnections->setChecked(settings.value("Main/SaveRestoreConnections", false).toBool());

    ui->spinFontSize->setValue(settings.value("Main/FontSize", ui->cbDisplayHex->font().pointSize()).toUInt());
    ui->cbFontFixedWidth->setChecked(settings.value("Main/FontFixedWidth", false).toBool());

    bool secondsMode = settings.value("Main/TimeSeconds", false).toBool();
    bool clockMode = settings.value("Main/TimeClock", false).toBool();
    bool milliMode = settings.value("Main/TimeMillis", false).toBool();
    if (clockMode)
    {
        ui->rbSeconds->setChecked(false);
        ui->rbMicros->setChecked(false);
        ui->rbSysClock->setChecked(true);
        ui->rbMillis->setChecked(false);
    }
    else
    {
        if (secondsMode)
        {
            ui->rbSeconds->setChecked(true);
            ui->rbMicros->setChecked(false);
            ui->rbSysClock->setChecked(false);
            ui->rbMillis->setChecked(false);
        }
        else if (milliMode)
        {
            ui->rbSeconds->setChecked(false);
            ui->rbMicros->setChecked(false);
            ui->rbSysClock->setChecked(false);
            ui->rbMillis->setChecked(true);
        }
        else
        {
            ui->rbSeconds->setChecked(false);
            ui->rbMicros->setChecked(true);
            ui->rbSysClock->setChecked(false);
            ui->rbMillis->setChecked(false);
        }
    }

    ui->cbCSVAbsTime->setChecked(settings.value("Main/CSVAbsTime", false).toBool());
    ui->comboSendingBus->setCurrentIndex(settings.value("Playback/SendingBus", 4).toInt());
    ui->cbUseFiltered->setChecked(settings.value("Main/UseFiltered", false).toBool());
    ui->cbUseOpenGL->setChecked(settings.value("Main/UseOpenGL", false).toBool());
    ui->cbFilterLabeling->setChecked(settings.value("Main/FilterLabeling", true).toBool());
    ui->cbFilterLabeling->setChecked(settings.value("Main/FilterLabeling", true).toBool());
    ui->cbIgnoreDBCColors->setChecked(settings.value("Main/IgnoreDBCColors", false).toBool());
    ui->cbColorsByCanId->setChecked(settings.value("Main/ColorsByCanId", false).toBool());

    if (QString savedLang = settings.value("Main/Language").toString(); !savedLang.isEmpty()) {
        int idx = ui->comboLanguage->findData(savedLang);
        if (idx >= 0)
            ui->comboLanguage->setCurrentIndex(idx);
    }

    int maxFramesDefault;
    if (QSysInfo::WordSize > 32)
    {
        qDebug() << "64 bit OS detected. Requesting a large preallocation";
        maxFramesDefault = 10000000;
    }
    else //if compiling for 32 bit you can't ask for gigabytes of preallocation so tone it down.
    {
        qDebug() << "32 bit OS detected. Requesting a much restricted prealloc";
        maxFramesDefault = 2000000;
    }

    ui->spinMaximumFrames->setValue(settings.value("Main/MaximumFrames", maxFramesDefault).toInt());
    ui->spinBytesPerLine->setValue(settings.value("Main/BytesPerLine", 8).toInt());

    //just for simplicity they all call the same function and that function updates all settings at once
    connect(ui->comboLanguage, SIGNAL(currentIndexChanged(int)), this, SLOT(updateSettings()));
    connect(ui->cbDisplayHex, SIGNAL(toggled(bool)), this, SLOT(updateSettings()));
    connect(ui->comboPayloadDisplay, SIGNAL(currentIndexChanged(int)), this, SLOT(updatePayloadFormatState()));
    connect(ui->comboPayloadDisplay, SIGNAL(currentIndexChanged(int)), this, SLOT(updateSettings()));
    connect(ui->linePayloadFormat, SIGNAL(textChanged(QString)), this, SLOT(updatePayloadFormatState()));
    connect(ui->linePayloadFormat, SIGNAL(editingFinished()), this, SLOT(updateSettings()));
    connect(ui->cbShowRawPayload, SIGNAL(toggled(bool)), this, SLOT(updateSettings()));
    connect(ui->comboPayloadDockPosition, SIGNAL(currentIndexChanged(int)), this, SLOT(updateSettings()));
    connect(ui->cbFlowAutoRef, SIGNAL(toggled(bool)), this, SLOT(updateSettings()));
    connect(ui->cbFlowUseTimestamp, SIGNAL(toggled(bool)), this, SLOT(updateSettings()));
    connect(ui->cbInfoAutoExpand, SIGNAL(toggled(bool)), this, SLOT(updateSettings()));
    connect(ui->cbMainAutoScroll, SIGNAL(toggled(bool)), this, SLOT(updateSettings()));
    connect(ui->cbPlaybackLoop, SIGNAL(toggled(bool)), this, SLOT(updateSettings()));
    connect(ui->cbRestorePositions, SIGNAL(toggled(bool)), this, SLOT(updateSettings()));
    connect(ui->cbValidate, SIGNAL(toggled(bool)), this, SLOT(updateSettings()));
    connect(ui->spinPlaybackSpeed, SIGNAL(valueChanged(int)), this, SLOT(updateSettings()));
    connect(ui->rbSeconds, SIGNAL(toggled(bool)), this, SLOT(updateSettings()));
    connect(ui->rbMicros, SIGNAL(toggled(bool)), this, SLOT(updateSettings()));
    connect(ui->rbSysClock, SIGNAL(toggled(bool)), this, SLOT(updateSettings()));
    connect(ui->rbMillis, SIGNAL(toggled(bool)), this, SLOT(updateSettings()));
    connect(ui->cbCSVAbsTime, SIGNAL(toggled(bool)), this, SLOT(updateSettings()));
    connect(ui->comboSendingBus, SIGNAL(currentIndexChanged(int)), this, SLOT(updateSettings()));
    connect(ui->cbUseFiltered, SIGNAL(toggled(bool)), this, SLOT(updateSettings()));
    connect(ui->lineClockFormat, SIGNAL(editingFinished()), this, SLOT(updateSettings()));
    connect(ui->cbUseOpenGL, SIGNAL(toggled(bool)), this, SLOT(updateSettings()));
    connect(ui->lineRemoteHost, SIGNAL(editingFinished()), this, SLOT(updateSettings()));
    connect(ui->lineRemotePort, SIGNAL(editingFinished()), this, SLOT(updateSettings()));
    connect(ui->lineRemoteUser, SIGNAL(editingFinished()), this, SLOT(updateSettings()));
    connect(ui->lineRemotePassword, SIGNAL(editingFinished()), this, SLOT(updateSettings()));
    connect(ui->cbLoadConnections, SIGNAL(toggled(bool)), this, SLOT(updateSettings()));
    connect(ui->cbFilterLabeling, SIGNAL(toggled(bool)), this, SLOT(updateSettings()));
    connect(ui->cbHexGraphFlow, SIGNAL(toggled(bool)), this, SLOT(updateSettings()));
    connect(ui->cbHexGraphInfo, SIGNAL(toggled(bool)), this, SLOT(updateSettings()));
    connect(ui->cbIgnoreDBCColors, SIGNAL(toggled(bool)), this, SLOT(updateSettings()));
    connect(ui->spinMaximumFrames, SIGNAL(valueChanged(int)), this, SLOT(updateSettings()));
    connect(ui->cbFontFixedWidth, SIGNAL(toggled(bool)), this, SLOT(updateSettings()));
    connect(ui->spinBytesPerLine, SIGNAL(valueChanged(int)), this, SLOT(updateSettings()));

    installEventFilter(this);
    updatePayloadFormatState();
}

MainSettingsDialog::~MainSettingsDialog()
{
    delete ui;
}

void MainSettingsDialog::closeEvent(QCloseEvent *event)
{
    Q_UNUSED(event);
    removeEventFilter(this);
    updateSettings();
}

bool MainSettingsDialog::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::KeyRelease) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        switch (keyEvent->key())
        {
        case Qt::Key_F1:
            HelpWindow::getRef()->showHelp("preferences.md");
            break;
        }
        return true;
    } else {
        // standard event processing
        return QObject::eventFilter(obj, event);
    }
    return false;
}


void MainSettingsDialog::updateSettings()
{
    QSettings settings;

    settings.setValue("Main/Language", ui->comboLanguage->currentData().toString());
    settings.setValue("Main/UseHex", ui->cbDisplayHex->isChecked());
    const QString payloadMode = ui->comboPayloadDisplay->currentData().toString();
    settings.setValue("Main/PayloadDisplayMode", payloadMode);
    PayloadFormatter payloadFormatter;
    if (payloadFormatter.compile(ui->linePayloadFormat->text()))
        settings.setValue("Main/PayloadFormat", ui->linePayloadFormat->text().simplified());
    settings.setValue("Main/PayloadShowRaw", ui->cbShowRawPayload->isChecked());
    settings.setValue("Main/PayloadDockPreference", ui->comboPayloadDockPosition->currentData().toString());
    settings.setValue("FlowView/AutoRef", ui->cbFlowAutoRef->isChecked());
    settings.setValue("FlowView/UseTimestamp", ui->cbFlowUseTimestamp->isChecked());
    settings.setValue("FlowView/GraphHex", ui->cbHexGraphFlow->isChecked());
    settings.setValue("InfoCompare/GraphHex", ui->cbHexGraphInfo->isChecked());
    settings.setValue("InfoCompare/AutoExpand", ui->cbInfoAutoExpand->isChecked());
    settings.setValue("Main/AutoScroll", ui->cbMainAutoScroll->isChecked());
    settings.setValue("Playback/AutoLoop", ui->cbPlaybackLoop->isChecked());
    settings.setValue("Main/SaveRestorePositions", ui->cbRestorePositions->isChecked());
    settings.setValue("Main/SaveRestoreConnections", ui->cbLoadConnections->isChecked());
    settings.setValue("Main/ValidateComm", ui->cbValidate->isChecked());
    settings.setValue("Playback/DefSpeed", ui->spinPlaybackSpeed->value());
    settings.setValue("Main/TimeSeconds", ui->rbSeconds->isChecked());
    settings.setValue("Main/TimeMillis", ui->rbMillis->isChecked());
    settings.setValue("Main/TimeClock", ui->rbSysClock->isChecked());
    settings.setValue("Main/CSVAbsTime", ui->cbCSVAbsTime->isChecked());
    settings.setValue("Playback/SendingBus", ui->comboSendingBus->currentIndex());
    settings.setValue("Main/UseFiltered", ui->cbUseFiltered->isChecked());
    settings.setValue("Main/UseOpenGL", ui->cbUseOpenGL->isChecked());
    settings.setValue("Main/TimeFormat", ui->lineClockFormat->text());
    settings.setValue("Main/FontSize", ui->spinFontSize->value());
    settings.setValue("Remote/Host", ui->lineRemoteHost->text());
    settings.setValue("Remote/Port", ui->lineRemotePort->text());
    settings.setValue("Remote/User", ui->lineRemoteUser->text());
    QByteArray encPass = crypto.encryptToByteArray(ui->lineRemotePassword->text());
    settings.setValue("Remote/Pass", encPass);
    settings.setValue("Main/FilterLabeling", ui->cbFilterLabeling->isChecked());
    settings.setValue("Main/IgnoreDBCColors", ui->cbIgnoreDBCColors->isChecked());
    settings.setValue("Main/MaximumFrames", ui->spinMaximumFrames->value());
    settings.setValue("Main/BytesPerLine", ui->spinBytesPerLine->value());
    settings.setValue("Main/FontFixedWidth", ui->cbFontFixedWidth->isChecked());
    settings.setValue("Main/ColorsByCanId", ui->cbColorsByCanId->isChecked());

    settings.sync();
    emit updatedSettings();
}

void MainSettingsDialog::updatePayloadFormatState()
{
    const bool custom = ui->comboPayloadDisplay->currentData().toString() == QStringLiteral("custom");
    ui->linePayloadFormat->setEnabled(custom);

    PayloadFormatter formatter;
    QString error;
    const bool valid = formatter.compile(ui->linePayloadFormat->text(), &error);
    ui->lblPayloadFormatError->setText(custom && !valid ? error : QString());
}
