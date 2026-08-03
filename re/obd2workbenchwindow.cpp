#include "obd2workbenchwindow.h"

#include "connections/canconmanager.h"
#include "payloadformatter.h"
#include "diagnosticgraphwindow.h"
#include "obddashboardcanvas.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QProgressBar>
#include <QSettings>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QTextStream>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace {
constexpr int DashboardManagedRole = Qt::UserRole + 20;
struct KnownPid { int pid; const char *name; const char *format; };
const KnownPid knownPids[] = {
    {0x00, "Supported PIDs 01-20", "u32be"},
    {0x01, "Monitor status since DTCs cleared", ""}, {0x02, "Freeze DTC", ""},
    {0x03, "Fuel system status", "system1:u8{0:Unavailable,1:Open-loop-cold,2:Closed-loop,4:Open-loop-load,8:Open-loop-fault,16:Closed-loop-fault,*:Reserved} system2:u8{0:Unavailable,1:Open-loop-cold,2:Closed-loop,4:Open-loop-load,8:Open-loop-fault,16:Closed-loop-fault,*:Reserved}"},
    {0x04, "Calculated engine load", "u8*100/255[%]{1}"}, {0x05, "Engine coolant temperature", "u8-40[C]"},
    {0x06, "Short term fuel trim bank 1", "u8*100/128-100[%]{1}"}, {0x07, "Long term fuel trim bank 1", "u8*100/128-100[%]{1}"},
    {0x08, "Short term fuel trim bank 2", "u8*100/128-100[%]{1}"}, {0x09, "Long term fuel trim bank 2", "u8*100/128-100[%]{1}"},
    {0x0A, "Fuel pressure", "u8*3[kPa]"}, {0x0B, "Intake manifold pressure", "u8[kPa]"},
    {0x0C, "Engine RPM", "u16be/4[rpm]{1}"}, {0x0D, "Vehicle speed", "u8[km/h]"},
    {0x0E, "Timing advance", "u8/2-64[deg]{1}"}, {0x0F, "Intake air temperature", "u8-40[C]"},
    {0x10, "MAF air flow rate", "u16be/100[g/s]{2}"}, {0x11, "Throttle position", "u8*100/255[%]{1}"},
    {0x12, "Commanded secondary air status", "u8{1:Upstream,2:Downstream,4:Atmosphere-off,8:Diagnostic-on,*:Reserved}"}, {0x13, "Oxygen sensors present", ""},
    {0x14, "O2 B1S1 voltage and trim", "voltage:u8/200[V]{3} trim:u8*100/128-100[%]{1}"},
    {0x15, "O2 B1S2 voltage and trim", "voltage:u8/200[V]{3} trim:u8*100/128-100[%]{1}"},
    {0x16, "O2 B1S3 voltage and trim", "voltage:u8/200[V]{3} trim:u8*100/128-100[%]{1}"},
    {0x17, "O2 B1S4 voltage and trim", "voltage:u8/200[V]{3} trim:u8*100/128-100[%]{1}"},
    {0x18, "O2 B2S1 voltage and trim", "voltage:u8/200[V]{3} trim:u8*100/128-100[%]{1}"},
    {0x19, "O2 B2S2 voltage and trim", "voltage:u8/200[V]{3} trim:u8*100/128-100[%]{1}"},
    {0x1A, "O2 B2S3 voltage and trim", "voltage:u8/200[V]{3} trim:u8*100/128-100[%]{1}"},
    {0x1B, "O2 B2S4 voltage and trim", "voltage:u8/200[V]{3} trim:u8*100/128-100[%]{1}"},
    {0x1C, "OBD standards compliance", "u8{1:OBD-II-CARB,2:OBD-EPA,3:OBD-and-OBD-II,4:OBD-I,5:Not-OBD,6:EOBD,7:EOBD-and-OBD-II,8:EOBD-and-OBD,9:EOBD-OBD-OBD-II,10:JOBD,11:JOBD-and-OBD-II,12:JOBD-and-EOBD,13:JOBD-EOBD-OBD-II,17:EMD,18:EMD-plus,19:HD-OBD-C,20:HD-OBD,21:WWH-OBD,23:HD-EOBD-I,24:HD-EOBD-I-N,25:HD-EOBD-II,26:HD-EOBD-II-N,28:OBDBr-1,29:OBDBr-2,30:KOBD,31:IOBD-I,32:IOBD-II,33:HD-EOBD-IV,*:Reserved}"}, {0x1D, "Oxygen sensors present alternate", ""},
    {0x1E, "Auxiliary input status", "pto:u8&1{0:Inactive,1:Active}"}, {0x1F, "Run time since engine start", "u16be[s]"},
    {0x20, "Supported PIDs 21-40", "u32be"}, {0x21, "Distance with MIL on", "u16be[km]"},
    {0x22, "Fuel rail pressure relative", "u16be*0.079[kPa]{3}"}, {0x23, "Fuel rail gauge pressure", "u16be*10[kPa]"},
    {0x24, "O2 S1 wide-range lambda and voltage", "lambda:u16be/32768 voltage:u16be/8192[V]{4}"},
    {0x25, "O2 S2 wide-range lambda and voltage", "lambda:u16be/32768 voltage:u16be/8192[V]{4}"},
    {0x26, "O2 S3 wide-range lambda and voltage", "lambda:u16be/32768 voltage:u16be/8192[V]{4}"},
    {0x27, "O2 S4 wide-range lambda and voltage", "lambda:u16be/32768 voltage:u16be/8192[V]{4}"},
    {0x28, "O2 S5 wide-range lambda and voltage", "lambda:u16be/32768 voltage:u16be/8192[V]{4}"},
    {0x29, "O2 S6 wide-range lambda and voltage", "lambda:u16be/32768 voltage:u16be/8192[V]{4}"},
    {0x2A, "O2 S7 wide-range lambda and voltage", "lambda:u16be/32768 voltage:u16be/8192[V]{4}"},
    {0x2B, "O2 S8 wide-range lambda and voltage", "lambda:u16be/32768 voltage:u16be/8192[V]{4}"},
    {0x2C, "Commanded EGR", "u8*100/255[%]{1}"}, {0x2D, "EGR error", "u8*100/128-100[%]{1}"},
    {0x2E, "Commanded evaporative purge", "u8*100/255[%]{1}"}, {0x2F, "Fuel tank level", "u8*100/255[%]{1}"},
    {0x30, "Warm-ups since DTC clear", "u8"}, {0x31, "Distance since codes cleared", "u16be[km]"},
    {0x32, "Evaporative system vapor pressure", "u16be/4-8192[Pa]{2}"}, {0x33, "Absolute barometric pressure", "u8[kPa]"},
    {0x34, "O2 S1 wide-range lambda and current", "lambda:u16be/32768 current:u16be/256-128[mA]{3}"},
    {0x35, "O2 S2 wide-range lambda and current", "lambda:u16be/32768 current:u16be/256-128[mA]{3}"},
    {0x36, "O2 S3 wide-range lambda and current", "lambda:u16be/32768 current:u16be/256-128[mA]{3}"},
    {0x37, "O2 S4 wide-range lambda and current", "lambda:u16be/32768 current:u16be/256-128[mA]{3}"},
    {0x38, "O2 S5 wide-range lambda and current", "lambda:u16be/32768 current:u16be/256-128[mA]{3}"},
    {0x39, "O2 S6 wide-range lambda and current", "lambda:u16be/32768 current:u16be/256-128[mA]{3}"},
    {0x3A, "O2 S7 wide-range lambda and current", "lambda:u16be/32768 current:u16be/256-128[mA]{3}"},
    {0x3B, "O2 S8 wide-range lambda and current", "lambda:u16be/32768 current:u16be/256-128[mA]{3}"},
    {0x3C, "Catalyst temperature B1S1", "u16be/10-40[C]{1}"}, {0x3D, "Catalyst temperature B2S1", "u16be/10-40[C]{1}"},
    {0x3E, "Catalyst temperature B1S2", "u16be/10-40[C]{1}"}, {0x3F, "Catalyst temperature B2S2", "u16be/10-40[C]{1}"},
    {0x40, "Supported PIDs 41-60", "u32be"}, {0x41, "Monitor status this drive cycle", ""},
    {0x42, "Control module voltage", "u16be/1000[V]{3}"}, {0x43, "Absolute load value", "u16be*100/255[%]{1}"},
    {0x44, "Commanded equivalence ratio", "u16be/32768"}, {0x45, "Relative throttle position", "u8*100/255[%]{1}"},
    {0x46, "Ambient air temperature", "u8-40[C]"}, {0x47, "Absolute throttle position B", "u8*100/255[%]{1}"},
    {0x48, "Absolute throttle position C", "u8*100/255[%]{1}"}, {0x49, "Accelerator pedal position D", "u8*100/255[%]{1}"},
    {0x4A, "Accelerator pedal position E", "u8*100/255[%]{1}"}, {0x4B, "Accelerator pedal position F", "u8*100/255[%]{1}"},
    {0x4C, "Commanded throttle actuator", "u8*100/255[%]{1}"}, {0x4D, "Time run with MIL on", "u16be[min]"},
    {0x4E, "Time since DTCs cleared", "u16be[min]"}, {0x4F, "Maximum values", ""}, {0x50, "Maximum MAF value", ""},
    {0x51, "Fuel type", "u8{1:Gasoline,2:Methanol,3:Ethanol,4:Diesel,5:LPG,6:CNG,7:Propane,8:Electric,9:Bifuel-gasoline,10:Bifuel-methanol,11:Bifuel-ethanol,12:Bifuel-LPG,13:Bifuel-CNG,14:Bifuel-propane,15:Bifuel-electricity,16:Bifuel-mixed,17:Hybrid-gasoline,18:Hybrid-ethanol,19:Hybrid-diesel,20:Hybrid-electric,21:Hybrid-mixed,22:Hybrid-regenerative,23:Bifuel-diesel,*:Reserved}"}, {0x52, "Ethanol fuel percentage", "u8*100/255[%]{1}"},
    {0x53, "Absolute evaporative vapor pressure", "u16be/200[kPa]{3}"}, {0x54, "Evaporative vapor pressure", "u16be-32767[Pa]"},
    {0x55, "Short term secondary O2 trim B1/B3", "bank1:u8*100/128-100[%]{1} bank3:u8*100/128-100[%]{1}"},
    {0x56, "Long term secondary O2 trim B1/B3", "bank1:u8*100/128-100[%]{1} bank3:u8*100/128-100[%]{1}"},
    {0x57, "Short term secondary O2 trim B2/B4", "bank2:u8*100/128-100[%]{1} bank4:u8*100/128-100[%]{1}"},
    {0x58, "Long term secondary O2 trim B2/B4", "bank2:u8*100/128-100[%]{1} bank4:u8*100/128-100[%]{1}"},
    {0x59, "Fuel rail absolute pressure", "u16be*10[kPa]"}, {0x5A, "Relative accelerator pedal position", "u8*100/255[%]{1}"},
    {0x5B, "Hybrid battery remaining life", "u8*100/255[%]{1}"}, {0x5C, "Engine oil temperature", "u8-40[C]"},
    {0x5D, "Fuel injection timing", "u16be/128-210[deg]{2}"}, {0x5E, "Engine fuel rate", "u16be/20[L/h]{2}"},
    {0x5F, "Designed emission requirements", "u8"}, {0x60, "Supported PIDs 61-80", "u32be"},
    {0x61, "Driver demand engine torque", "u8-125[%]"}, {0x62, "Actual engine torque", "u8-125[%]"},
    {0x63, "Engine reference torque", "u16be[Nm]"}, {0x64, "Engine percent torque data", ""},
    {0x65, "Auxiliary input/output supported", ""}, {0x66, "Mass air flow sensor", ""},
    {0x67, "Engine coolant temperature sensors", ""}, {0x68, "Intake air temperature sensors", ""},
    {0x69, "Commanded EGR and EGR error", ""}, {0x6A, "Commanded diesel intake air flow", ""},
    {0x6B, "EGR temperature", ""}, {0x6C, "Commanded throttle actuator control", ""},
    {0x6D, "Fuel pressure control system", ""}, {0x6E, "Injection pressure control system", ""},
    {0x6F, "Turbocharger compressor inlet pressure", ""}, {0x70, "Boost pressure control", ""},
    {0x71, "Variable geometry turbo control", ""}, {0x72, "Wastegate control", ""},
    {0x73, "Exhaust pressure", ""}, {0x74, "Turbocharger RPM", ""}, {0x75, "Turbocharger temperature", ""},
    {0x76, "Turbocharger temperature bank 2", ""}, {0x77, "Charge air cooler temperature", ""},
    {0x78, "Exhaust gas temperature bank 1", ""}, {0x79, "Exhaust gas temperature bank 2", ""},
    {0x7A, "Diesel particulate filter bank 1", ""}, {0x7B, "Diesel particulate filter bank 2", ""},
    {0x7C, "Diesel particulate filter temperature", ""}, {0x7D, "NOx NTE control area status", ""},
    {0x7E, "PM NTE control area status", ""}, {0x7F, "Engine run time", ""},
    {0x80, "Supported PIDs 81-A0", "u32be"}, {0x81, "Engine run time for auxiliary emissions control", ""},
    {0x82, "Engine run time for AECD", ""}, {0x83, "NOx sensor", ""}, {0x84, "Manifold surface temperature", ""},
    {0x85, "NOx reagent system", ""}, {0x86, "Particulate matter sensor", ""},
    {0x87, "Intake manifold absolute pressure", ""}, {0x88, "SCR inducement system", ""},
    {0x89, "Run time for AECD 11-15", ""}, {0x8A, "Run time for AECD 16-20", ""},
    {0x8B, "Diesel aftertreatment status", ""}, {0x8C, "Wide-range O2 sensor", ""},
    {0x8D, "Throttle position G", "u8*100/255[%]{1}"}, {0x8E, "Engine friction percent torque", "u8-125[%]"},
    {0x8F, "Particulate matter sensor banks 1 and 2", ""}, {0x90, "WWH-OBD vehicle information", ""},
    {0x91, "WWH-OBD ECU information", ""}, {0x92, "Fuel system control", ""},
    {0x93, "WWH-OBD counters", ""}, {0x94, "NOx warning and inducement system", ""},
    {0x98, "Exhaust gas temperature sensor", ""}, {0x99, "Exhaust gas temperature sensor", ""},
    {0x9A, "Hybrid or electric vehicle system data", ""}, {0x9B, "Diesel exhaust fluid sensor data", ""},
    {0x9C, "O2 sensor data", ""}, {0x9D, "Engine fuel rate", ""}, {0x9E, "Engine exhaust flow rate", ""},
    {0xA0, "Supported PIDs A1-C0", "u32be"}, {0xA1, "NOx sensor corrected data", ""},
    {0xA2, "Cylinder fuel rate", ""}, {0xA3, "Evaporative system vapor pressure", ""},
    {0xA4, "Transmission actual gear", ""}, {0xA5, "Commanded diesel exhaust fluid dosing", ""},
    {0xA6, "Odometer", "u32be/10[km]{1}"}
};

QString dashboardPidName(int pid)
{
    for (const KnownPid &known : knownPids)
        if (known.pid == pid) return QObject::tr(known.name);
    return QStringLiteral("PID 0x%1").arg(pid, 2, 16, QLatin1Char('0')).toUpper();
}

QSet<QString> pidDescriptionWords(QString text)
{
    text = text.toLower();
    text.replace(QStringLiteral("revolutions per minute"), QStringLiteral(" rpm "));
    text.replace(QStringLiteral("engine speed"), QStringLiteral(" rpm "));
    text.replace(QStringLiteral("vehicle velocity"), QStringLiteral(" vehicle speed "));
    text.replace(QStringLiteral("coolant temp"), QStringLiteral(" coolant temperature "));
    text.replace(QStringLiteral("water temperature"), QStringLiteral(" coolant temperature "));
    text.replace(QStringLiteral("mass airflow"), QStringLiteral(" maf air flow "));
    text.replace(QStringLiteral("mass air flow"), QStringLiteral(" maf air flow "));
    text.replace(QStringLiteral("manifold absolute pressure"), QStringLiteral(" map manifold pressure "));
    text.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral(" "));
    const QSet<QString> ignored = {
        QStringLiteral("the"), QStringLiteral("a"), QStringLiteral("an"),
        QStringLiteral("pid"), QStringLiteral("request"), QStringLiteral("reading"),
        QStringLiteral("value"), QStringLiteral("live"), QStringLiteral("show"),
        QStringLiteral("add"), QStringLiteral("sensor"), QStringLiteral("data")
    };
    QSet<QString> result;
    for (QString word : text.split(QLatin1Char(' '), Qt::SkipEmptyParts))
    {
        if (word == QStringLiteral("temp")) word = QStringLiteral("temperature");
        if (!ignored.contains(word)) result.insert(word);
    }
    return result;
}

QStringList parseCsvRow(const QString &line)
{
    QStringList fields;
    QString field;
    bool quoted = false;
    for (int i = 0; i < line.size(); ++i)
    {
        const QChar ch = line.at(i);
        if (ch == QLatin1Char('"'))
        {
            if (quoted && i + 1 < line.size() && line.at(i + 1) == QLatin1Char('"'))
            {
                field += QLatin1Char('"');
                ++i;
            }
            else quoted = !quoted;
        }
        else if (ch == QLatin1Char(',') && !quoted)
        {
            fields << field;
            field.clear();
        }
        else field += ch;
    }
    fields << field;
    return fields;
}

bool resolveKnownPidDescription(const QString &description, int *pid,
                                QString *name, QString *format, double *confidence)
{
    const QSet<QString> query = pidDescriptionWords(description);
    if (query.isEmpty()) return false;
    double bestScore = 0.0;
    const KnownPid *best = nullptr;
    for (const KnownPid &known : knownPids)
    {
        const QSet<QString> candidate = pidDescriptionWords(QString::fromLatin1(known.name));
        int intersection = 0;
        for (const QString &word : query)
            if (candidate.contains(word)) ++intersection;
        const int unionSize = query.size() + candidate.size() - intersection;
        double score = unionSize ? double(intersection) / double(unionSize) : 0.0;
        if (candidate == query) score = 1.0;
        else if (query.size() >= 2 && intersection == query.size())
            score = qMax(score, 0.9);
        else if (query.size() == 1 && intersection == 1)
        {
            int matchingNames = 0;
            const QString word = *query.constBegin();
            for (const KnownPid &other : knownPids)
                if (pidDescriptionWords(QString::fromLatin1(other.name)).contains(word))
                    ++matchingNames;
            if (matchingNames == 1) score = qMax(score, 0.85);
        }
        if (score > bestScore)
        {
            bestScore = score;
            best = &known;
        }
    }
    if (!best || bestScore < 0.55) return false;
    if (pid) *pid = best->pid;
    if (name) *name = QObject::tr(best->name);
    if (format) *format = QString::fromLatin1(best->format);
    if (confidence) *confidence = bestScore;
    return true;
}

bool configureDashboardWidget(QWidget *parent, OBDDashboardCanvas::WidgetConfig *config)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Dashboard widget"));
    QFormLayout *form = new QFormLayout(&dialog);
    QComboBox *pid = new QComboBox(&dialog);
    pid->setEditable(true);
    for (const KnownPid &known : knownPids)
        pid->addItem(QStringLiteral("0x%1 - %2").arg(known.pid, 2, 16, QLatin1Char('0'))
                     .arg(QObject::tr(known.name)).toUpper(), known.pid);
    const int pidIndex = pid->findData(config->pid);
    if (pidIndex >= 0) pid->setCurrentIndex(pidIndex);
    else pid->setEditText(QStringLiteral("0x%1").arg(config->pid, 2, 16, QLatin1Char('0')).toUpper());

    QComboBox *type = new QComboBox(&dialog);
    type->addItem(QObject::tr("Digital readout"), QStringLiteral("digital"));
    type->addItem(QObject::tr("Level"), QStringLiteral("level"));
    type->addItem(QObject::tr("Gauge"), QStringLiteral("gauge"));
    type->addItem(QObject::tr("Text / string"), QStringLiteral("text"));
    type->setCurrentIndex(qMax(0, type->findData(config->type)));
    QString suggestedName = dashboardPidName(config->pid);
    QLineEdit *title = new QLineEdit(config->title.isEmpty() ? suggestedName : config->title, &dialog);
    QLineEdit *format = new QLineEdit(config->format, &dialog);
    format->setPlaceholderText(QObject::tr("Empty uses automatic PID decoding"));
    QDoubleSpinBox *minimum = new QDoubleSpinBox(&dialog);
    QDoubleSpinBox *maximum = new QDoubleSpinBox(&dialog);
    for (QDoubleSpinBox *field : {minimum, maximum})
    {
        field->setRange(-1000000000.0, 1000000000.0);
        field->setDecimals(3);
    }
    minimum->setValue(config->minimum);
    maximum->setValue(config->maximum);
    form->addRow(QObject::tr("PID"), pid);
    form->addRow(QObject::tr("Display"), type);
    form->addRow(QObject::tr("Widget name"), title);
    form->addRow(QObject::tr("Payload format"), format);
    form->addRow(QObject::tr("Minimum"), minimum);
    form->addRow(QObject::tr("Maximum"), maximum);
    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                                     Qt::Horizontal, &dialog);
    form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(pid, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, [&](int index) {
        if (index < 0) return;
        const QString nextSuggestion = dashboardPidName(pid->itemData(index).toInt());
        if (title->text().trimmed().isEmpty() || title->text() == suggestedName)
            title->setText(nextSuggestion);
        suggestedName = nextSuggestion;
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
        QString pidText = pid->currentText().section(QStringLiteral(" - "), 0, 0).trimmed();
        const bool hex = pidText.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive);
        if (hex) pidText = pidText.mid(2);
        bool ok = false;
        const int parsedPid = pidText.toInt(&ok, hex ? 16 : 10);
        if (!ok || parsedPid < 0 || parsedPid > 0xFF)
        {
            QMessageBox::warning(&dialog, QObject::tr("Invalid PID"),
                                 QObject::tr("Enter a PID between 0x00 and 0xFF."));
            return;
        }
        if (maximum->value() <= minimum->value())
        {
            QMessageBox::warning(&dialog, QObject::tr("Invalid range"),
                                 QObject::tr("Maximum must be greater than minimum."));
            return;
        }
        if (title->text().trimmed().isEmpty() || title->text() == suggestedName)
            title->setText(dashboardPidName(parsedPid));
        config->pid = parsedPid;
        config->type = type->currentData().toString();
        config->title = title->text();
        config->format = format->text();
        config->minimum = minimum->value();
        config->maximum = maximum->value();
        dialog.accept();
    });
    return dialog.exec() == QDialog::Accepted;
}
}

bool OBD2WorkbenchWindow::resolvePidDescription(const QString &description, int *pid,
                                                QString *name, QString *format,
                                                double *confidence)
{
    return resolveKnownPidDescription(description, pid, name, format, confidence);
}

QJsonObject OBD2WorkbenchWindow::aiState() const
{
    QJsonArray requests;
    for (int row = 0; row < pidTable->rowCount(); ++row)
    {
        requests.append(QJsonObject{
            {QStringLiteral("enabled"),
             pidTable->item(row, PidEnabled)->checkState() == Qt::Checked},
            {QStringLiteral("name"), pidTable->item(row, PidName)->text()},
            {QStringLiteral("pid"), pidText(row)},
            {QStringLiteral("format"), pidTable->item(row, PidFormat)->text()}
        });
    }
    return QJsonObject{
        {QStringLiteral("connected"), connected},
        {QStringLiteral("bus"), busSpin->value()},
        {QStringLiteral("request_id"), requestIdEdit->currentText()},
        {QStringLiteral("polling"), pollCheck->isChecked()},
        {QStringLiteral("poll_cycle_ms"), pollIntervalSpin->value()},
        {QStringLiteral("requests"), requests}
    };
}

OBD2WorkbenchWindow::OBD2WorkbenchWindow(QWidget *parent) : QDialog(parent), handler(new UDS_HANDLER)
{
    buildUi();
    loadSettings();
    handler->setProcessAllIDs(true);
    handler->setFlowCtrl(true);
    responseTimer.setSingleShot(true);
    responseTimer.setInterval(900);
    connect(handler, &UDS_HANDLER::newUDSMessage, this, &OBD2WorkbenchWindow::gotReply);
    connect(&responseTimer, &QTimer::timeout, this, &OBD2WorkbenchWindow::responseTimedOut);
    responseSettleTimer.setSingleShot(true);
    responseSettleTimer.setInterval(40);
    connect(&responseSettleTimer, &QTimer::timeout,
            this, &OBD2WorkbenchWindow::responseBurstFinished);
    pollTimer.setSingleShot(true);
    connect(&pollTimer, &QTimer::timeout, this, &OBD2WorkbenchWindow::pollPids);
    tripPlaybackTimer.setSingleShot(true);
    connect(&tripPlaybackTimer, &QTimer::timeout, this, &OBD2WorkbenchWindow::advanceTripPlayback);
}

OBD2WorkbenchWindow::~OBD2WorkbenchWindow()
{
    saveSettings();
    if (tripLogFile) { tripLogFile->close(); delete tripLogFile; }
    handler->setReception(false);
    delete handler;
}

void OBD2WorkbenchWindow::buildUi()
{
    setWindowTitle(tr("OBD-II Workbench"));
    resize(1050, 680);
    QVBoxLayout *root = new QVBoxLayout(this);
    QGroupBox *endpoint = new QGroupBox(tr("OBD-II endpoint"), this);
    QHBoxLayout *endpointLayout = new QHBoxLayout(endpoint);
    busSpin = new QSpinBox(endpoint);
    busSpin->setRange(0, qMax(0, CANConManager::getInstance()->getNumBuses() - 1));
    requestIdEdit = new QComboBox(endpoint);
    requestIdEdit->setEditable(true);
    requestIdEdit->setInsertPolicy(QComboBox::NoInsert);
    requestIdEdit->addItem(QStringLiteral("0x7DF"));
    for (int id = 0x7E0; id <= 0x7E7; ++id)
        requestIdEdit->addItem(QStringLiteral("0x%1").arg(id, 0, 16).toUpper());
    requestIdEdit->setCurrentText(QStringLiteral("0x7DF"));
    requestIdEdit->setToolTip(tr("Functional 0x7DF or a physical/custom 11-bit or 29-bit request ID"));
    ecuTargetCombo = new QComboBox(endpoint);
    ecuTargetCombo->addItem(tr("All ECUs (functional)"), QStringLiteral("0x7DF"));
    safetyModeCombo = new QComboBox(endpoint);
    safetyModeCombo->addItem(tr("Passive"), QStringLiteral("passive"));
    safetyModeCombo->addItem(tr("Read-only"), QStringLiteral("read"));
    safetyModeCombo->addItem(tr("Full diagnostics"), QStringLiteral("full"));
    safetyModeCombo->setCurrentIndex(1);
    responseModeCombo = new QComboBox(endpoint);
    responseModeCombo->addItem(tr("Range / List"), QStringLiteral("list"));
    responseModeCombo->addItem(tr("ID + Mask"), QStringLiteral("mask"));
    responseIdEdit = new QLineEdit(QStringLiteral("0x7E8-0x7EF"), endpoint);
    responseMaskEdit = new QLineEdit(QStringLiteral("0x7F8"), endpoint);
    responseMaskLabel = new QLabel(tr("Mask"), endpoint);
    connectButton = new QPushButton(tr("Enable OBD"), endpoint);
    connectionStatus = new QLabel(tr("Disconnected"), endpoint);
    endpointLayout->addWidget(new QLabel(tr("Bus"), endpoint));
    endpointLayout->addWidget(busSpin);
    endpointLayout->addWidget(new QLabel(tr("Request ID"), endpoint));
    endpointLayout->addWidget(requestIdEdit);
    endpointLayout->addWidget(ecuTargetCombo);
    endpointLayout->addWidget(new QLabel(tr("Responses"), endpoint));
    endpointLayout->addWidget(responseModeCombo);
    endpointLayout->addWidget(responseIdEdit);
    endpointLayout->addWidget(responseMaskLabel);
    endpointLayout->addWidget(responseMaskEdit);
    endpointLayout->addWidget(connectButton);
    endpointLayout->addWidget(safetyModeCombo);
    endpointLayout->addStretch();
    endpointLayout->addWidget(connectionStatus);
    root->addWidget(endpoint);
    connect(connectButton, &QPushButton::clicked, this, [this]() {
        if (connected) disconnectEndpoint(); else connectEndpoint();
    });
    connect(responseModeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() {
        const bool maskMode = responseModeCombo->currentData().toString() == QStringLiteral("mask");
        responseMaskLabel->setVisible(maskMode);
        responseMaskEdit->setVisible(maskMode);
        responseIdEdit->setPlaceholderText(maskMode ? tr("Response ID")
            : tr("e.g. 0x7E8-0x7EF, 0x18DAF110"));
    });
    connect(requestIdEdit, qOverload<int>(&QComboBox::activated), this, [this](int) {
        bool ok = false;
        const uint32_t requestId = parseNumber(requestIdEdit->currentText(), &ok);
        if (!ok) return;
        responseModeCombo->setCurrentIndex(responseModeCombo->findData(QStringLiteral("list")));
        if (requestId == 0x7DF)
            responseIdEdit->setText(QStringLiteral("0x7E8-0x7EF"));
        else if (requestId >= 0x7E0 && requestId <= 0x7E7)
            responseIdEdit->setText(QStringLiteral("0x%1").arg(requestId + 8, 0, 16).toUpper());
    });
    connect(ecuTargetCombo, qOverload<int>(&QComboBox::activated), this, [this](int) {
        requestIdEdit->setCurrentText(ecuTargetCombo->currentData().toString());
        bool ok = false;
        const uint32_t requestId = parseNumber(requestIdEdit->currentText(), &ok);
        if (!ok) return;
        responseModeCombo->setCurrentIndex(responseModeCombo->findData(QStringLiteral("list")));
        responseIdEdit->setText(requestId == 0x7DF ? QStringLiteral("0x7E8-0x7EF")
            : QStringLiteral("0x%1").arg(requestId + 8, 0, 16).toUpper());
    });
    responseMaskLabel->setVisible(false);
    responseMaskEdit->setVisible(false);

    QTabWidget *tabs = new QTabWidget(this);
    QWidget *livePage = new QWidget(tabs);
    QVBoxLayout *liveLayout = new QVBoxLayout(livePage);
    pidTable = new QTableWidget(0, PidColumnCount, livePage);
    pidTable->setHorizontalHeaderLabels({tr("Poll"), tr("Name"), tr("PID lookup / custom"),
                                         tr("Payload format"), tr("Raw response"),
                                         tr("ECU responses"), tr("Updated")});
    pidTable->resizeColumnsToContents();
    pidTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    pidTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    pidTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    liveLayout->addWidget(pidTable);
    QHBoxLayout *liveButtons = new QHBoxLayout;
    QPushButton *addButton = new QPushButton(tr("Add PID"), livePage);
    QPushButton *removeButton = new QPushButton(tr("Remove"), livePage);
    QPushButton *loadButton = new QPushButton(tr("Load PID list"), livePage);
    QPushButton *saveButton = new QPushButton(tr("Save PID list"), livePage);
    QPushButton *clearValuesButton = new QPushButton(tr("Clear values"), livePage);
    QPushButton *selectedButton = new QPushButton(tr("Send selected once"), livePage);
    QPushButton *enabledButton = new QPushButton(tr("Send enabled once"), livePage);
    QPushButton *graphButton = new QPushButton(tr("Live graph"), livePage);
    tripRecordButton = new QPushButton(tr("Start trip log"), livePage);
    pollCheck = new QCheckBox(livePage);
    pollCheck->hide();
    liveRequestsCheck = new QCheckBox(tr("Poll live rows"), livePage);
    liveRequestsCheck->setChecked(true);
    pollStartButton = new QPushButton(tr("Start polling"), livePage);
    pollIntervalSpin = new QSpinBox(livePage);
    pollIntervalSpin->setRange(200, 60000);
    pollIntervalSpin->setValue(1000);
    pollIntervalSpin->setSuffix(tr(" ms"));
    liveButtons->addWidget(addButton);
    liveButtons->addWidget(removeButton);
    liveButtons->addWidget(loadButton);
    liveButtons->addWidget(saveButton);
    liveButtons->addWidget(clearValuesButton);
    liveButtons->addStretch();
    liveButtons->addWidget(liveRequestsCheck);
    liveButtons->addWidget(pollStartButton);
    liveButtons->addWidget(new QLabel(tr("Cycle interval"), livePage));
    liveButtons->addWidget(pollIntervalSpin);
    liveButtons->addWidget(graphButton);
    liveButtons->addWidget(tripRecordButton);
    liveButtons->addWidget(selectedButton);
    liveButtons->addWidget(enabledButton);
    liveLayout->addLayout(liveButtons);
    connect(addButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::addPid);
    connect(removeButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::removePid);
    connect(loadButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::loadPidList);
    connect(saveButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::savePidList);
    connect(clearValuesButton, &QPushButton::clicked, this, [this]() {
        for (int row = 0; row < pidTable->rowCount(); ++row)
            for (int column : {PidRaw, PidResponses, PidUpdated})
                if (pidTable->item(row, column)) pidTable->item(row, column)->setText(QString());
    });
    connect(selectedButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::requestSelectedPid);
    connect(enabledButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::requestEnabledPids);
    connect(graphButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::showDiagnosticGraph);
    connect(tripRecordButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::toggleTripRecording);
    connect(pollStartButton, &QPushButton::clicked, this, [this]() {
        if (pollCheck->isChecked()) stopPolling(); else startPolling();
    });
    connect(pollIntervalSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        if (pollCheck->isChecked() && pollTimer.isActive()) pollTimer.start(value);
    });
    tabs->addTab(livePage, tr("Live data"));

    QWidget *dashboardPage = new QWidget(tabs);
    QVBoxLayout *dashboardLayout = new QVBoxLayout(dashboardPage);
    QHBoxLayout *dashboardControls = new QHBoxLayout;
    QHBoxLayout *dashboardPollingControls = new QHBoxLayout;
    dashboardPidCombo = new QComboBox(dashboardPage);
    dashboardPidCombo->setEditable(true);
    for (const KnownPid &known : knownPids)
        dashboardPidCombo->addItem(QStringLiteral("0x%1 - %2").arg(known.pid, 2, 16, QLatin1Char('0'))
            .arg(tr(known.name)).toUpper(), known.pid);
    QPushButton *dashboardAddButton = new QPushButton(tr("Add widget"), dashboardPage);
    QPushButton *dashboardRemoveButton = new QPushButton(tr("Remove selected"), dashboardPage);
    QCheckBox *dashboardEditMode = new QCheckBox(tr("Edit layout"), dashboardPage);
    dashboardColumnsSpin = new QSpinBox(dashboardPage);
    dashboardColumnsSpin->setRange(4, 16);
    dashboardColumnsSpin->setValue(12);
    QPushButton *dashboardLoadButton = new QPushButton(tr("Load layout"), dashboardPage);
    QPushButton *dashboardSaveButton = new QPushButton(tr("Save layout"), dashboardPage);
    dashboardRequestsCheck = new QCheckBox(tr("Poll dashboard widgets"), dashboardPage);
    dashboardRequestsCheck->setChecked(true);
    QPushButton *dashboardSendOnceButton =
        new QPushButton(tr("Send enabled once"), dashboardPage);
    dashboardPollStartButton = new QPushButton(tr("Start polling"), dashboardPage);
    dashboardPollStartButton->setEnabled(false);
    dashboardPollIntervalSpin = new QSpinBox(dashboardPage);
    dashboardPollIntervalSpin->setRange(200, 60000);
    dashboardPollIntervalSpin->setSuffix(tr(" ms"));
    dashboardPollIntervalSpin->setValue(pollIntervalSpin->value());
    dashboardControls->addWidget(new QLabel(tr("PID"), dashboardPage));
    dashboardControls->addWidget(dashboardPidCombo, 1);
    dashboardControls->addWidget(dashboardAddButton);
    dashboardControls->addWidget(dashboardRemoveButton);
    dashboardControls->addWidget(dashboardEditMode);
    dashboardControls->addWidget(new QLabel(tr("Columns"), dashboardPage));
    dashboardControls->addWidget(dashboardColumnsSpin);
    dashboardControls->addWidget(dashboardLoadButton);
    dashboardControls->addWidget(dashboardSaveButton);
    dashboardLayout->addLayout(dashboardControls);
    dashboardPollingControls->addWidget(dashboardRequestsCheck);
    dashboardPollingControls->addWidget(dashboardSendOnceButton);
    dashboardPollingControls->addWidget(dashboardPollStartButton);
    dashboardPollingControls->addWidget(new QLabel(tr("Cycle interval"), dashboardPage));
    dashboardPollingControls->addWidget(dashboardPollIntervalSpin);
    dashboardPollingControls->addStretch();
    dashboardLayout->addLayout(dashboardPollingControls);
    QScrollArea *dashboardScroll = new QScrollArea(dashboardPage);
    dashboardScroll->setWidgetResizable(true);
    widgetDashboardCanvas = new OBDDashboardCanvas(dashboardScroll);
    dashboardScroll->setWidget(widgetDashboardCanvas);
    dashboardLayout->addWidget(dashboardScroll, 1);
    connect(dashboardAddButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::addDashboardPid);
    connect(dashboardRemoveButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::removeDashboardPid);
    connect(dashboardLoadButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::loadDashboardLayout);
    connect(dashboardSaveButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::saveDashboardLayout);
    connect(dashboardSendOnceButton, &QPushButton::clicked,
            this, &OBD2WorkbenchWindow::requestEnabledPids);
    connect(dashboardPollStartButton, &QPushButton::clicked, this, [this]() {
        if (pollCheck->isChecked()) stopPolling(); else startPolling();
    });
    connect(dashboardRequestsCheck, &QCheckBox::toggled, this, [this]() {
        syncDashboardPidRows();
        if (pollCheck->isChecked() && !responseTimer.isActive())
        {
            pollTimer.stop();
            pollPids();
        }
    });
    connect(pollIntervalSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        const QSignalBlocker blocker(dashboardPollIntervalSpin);
        dashboardPollIntervalSpin->setValue(value);
    });
    connect(dashboardPollIntervalSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        pollIntervalSpin->setValue(value);
    });
    connect(dashboardColumnsSpin, qOverload<int>(&QSpinBox::valueChanged), this,
            [this](int value) { widgetDashboardCanvas->setColumns(value); });
    connect(dashboardEditMode, &QCheckBox::toggled, widgetDashboardCanvas, &OBDDashboardCanvas::setEditMode);
    widgetDashboardCanvas->setEditHandler([this](OBDDashboardCanvas::WidgetConfig *) {
        editDashboardWidget();
    });
    tabs->addTab(dashboardPage, tr("Dashboard"));

    QWidget *playbackPage = new QWidget(tabs);
    QVBoxLayout *playbackLayout = new QVBoxLayout(playbackPage);
    QHBoxLayout *playbackControls = new QHBoxLayout;
    QPushButton *tripLoadButton = new QPushButton(tr("Load trip log"), playbackPage);
    QPushButton *tripClearButton = new QPushButton(tr("Clear"), playbackPage);
    tripPlaybackButton = new QPushButton(tr("Play"), playbackPage);
    tripPlaybackButton->setEnabled(false);
    playbackControls->addWidget(tripLoadButton);
    playbackControls->addWidget(tripClearButton);
    playbackControls->addWidget(tripPlaybackButton);
    playbackControls->addStretch();
    playbackLayout->addLayout(playbackControls);
    tripPlaybackSlider = new QSlider(Qt::Horizontal, playbackPage);
    tripPlaybackSlider->setRange(0, 0);
    playbackLayout->addWidget(tripPlaybackSlider);
    tripPlaybackTable = new QTableWidget(0, 7, playbackPage);
    tripPlaybackTable->setHorizontalHeaderLabels(
        {tr("Timestamp"), tr("Bus"), tr("ECU"), tr("PID"), tr("Name"), tr("Raw"), tr("Decoded")});
    tripPlaybackTable->resizeColumnsToContents();
    tripPlaybackTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    tripPlaybackTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    tripPlaybackTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    playbackLayout->addWidget(tripPlaybackTable, 1);
    connect(tripLoadButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::loadTripPlayback);
    connect(tripClearButton, &QPushButton::clicked, this, [this]() {
        tripPlaybackTimer.stop();
        tripPlaybackButton->setText(tr("Play"));
        tripPlaybackButton->setEnabled(false);
        tripPlaybackTable->setRowCount(0);
        tripPlaybackTimes.clear();
        tripPlaybackSlider->setRange(0, 0);
    });
    connect(tripPlaybackButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::toggleTripPlayback);
    connect(tripPlaybackSlider, &QSlider::valueChanged, this, &OBD2WorkbenchWindow::setTripPlaybackRow);
    tabs->addTab(playbackPage, tr("Trip playback"));

    QWidget *discoveryPage = new QWidget(tabs);
    QVBoxLayout *discoveryLayout = new QVBoxLayout(discoveryPage);
    QHBoxLayout *discoveryButtons = new QHBoxLayout;
    QPushButton *moduleScanButton = new QPushButton(tr("Scan modules"), discoveryPage);
    QPushButton *pidScanButton = new QPushButton(tr("Scan available PIDs"), discoveryPage);
    QPushButton *loadDiscoveryButton = new QPushButton(tr("Load results"), discoveryPage);
    QPushButton *saveDiscoveryButton = new QPushButton(tr("Save results"), discoveryPage);
    QPushButton *clearDiscoveryButton = new QPushButton(tr("Clear results"), discoveryPage);
    QPushButton *addScannedButton = new QPushButton(tr("Add selected to Live data"), discoveryPage);
    discoveryButtons->addWidget(moduleScanButton);
    discoveryButtons->addWidget(pidScanButton);
    discoveryButtons->addWidget(loadDiscoveryButton);
    discoveryButtons->addWidget(saveDiscoveryButton);
    discoveryButtons->addWidget(clearDiscoveryButton);
    discoveryButtons->addStretch();
    discoveryButtons->addWidget(addScannedButton);
    discoveryLayout->addLayout(discoveryButtons);
    discoveryOutput = new QTextEdit(discoveryPage);
    discoveryOutput->setReadOnly(true);
    discoveryOutput->setMaximumHeight(180);
    discoveryLayout->addWidget(discoveryOutput);
    QHBoxLayout *discoveryFilter = new QHBoxLayout;
    discoveryEcuCombo = new QComboBox(discoveryPage);
    discoveryEcuCombo->addItem(tr("All ECUs"), QStringLiteral("all"));
    discoveryFilter->addWidget(new QLabel(tr("Show supported PIDs for"), discoveryPage));
    discoveryFilter->addWidget(discoveryEcuCombo, 1);
    discoveryLayout->addLayout(discoveryFilter);
    discoveryPidList = new QListWidget(discoveryPage);
    discoveryPidList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    discoveryLayout->addWidget(discoveryPidList, 1);
    connect(moduleScanButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::scanModules);
    connect(pidScanButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::scanSupportedPids);
    connect(loadDiscoveryButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::loadDiscoveryResults);
    connect(saveDiscoveryButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::saveDiscoveryResults);
    connect(discoveryEcuCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) { refreshDiscoveryPidList(); });
    connect(clearDiscoveryButton, &QPushButton::clicked, this, [this]() {
        discoveryOutput->clear();
        discoveryPidList->clear();
        supportedPidsByEcu.clear();
        discoveryEcuCombo->clear();
        discoveryEcuCombo->addItem(tr("All ECUs"), QStringLiteral("all"));
        refreshEcuTargets();
    });
    connect(addScannedButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::addSelectedScannedPids);
    tabs->addTab(discoveryPage, tr("Discovery"));

    QWidget *dtcPage = new QWidget(tabs);
    QVBoxLayout *dtcLayout = new QVBoxLayout(dtcPage);
    QHBoxLayout *dtcButtons = new QHBoxLayout;
    QPushButton *storedButton = new QPushButton(tr("Stored (03)"), dtcPage);
    QPushButton *pendingButton = new QPushButton(tr("Pending (07)"), dtcPage);
    QPushButton *permanentButton = new QPushButton(tr("Permanent (0A)"), dtcPage);
    QPushButton *clearButton = new QPushButton(tr("Clear DTCs (04)"), dtcPage);
    QPushButton *loadDtcButton = new QPushButton(tr("Load DTC database"), dtcPage);
    QPushButton *reportButton = new QPushButton(tr("Export report"), dtcPage);
    QPushButton *clearDtcOutputButton = new QPushButton(tr("Clear output"), dtcPage);
    dtcButtons->addWidget(storedButton);
    dtcButtons->addWidget(pendingButton);
    dtcButtons->addWidget(permanentButton);
    dtcButtons->addWidget(loadDtcButton);
    dtcButtons->addWidget(reportButton);
    dtcButtons->addWidget(clearDtcOutputButton);
    dtcButtons->addStretch();
    dtcButtons->addWidget(clearButton);
    dtcLayout->addLayout(dtcButtons);
    dtcDatabaseStatus = new QLabel(tr("No DTC description database loaded"), dtcPage);
    dtcLayout->addWidget(dtcDatabaseStatus);
    dtcOutput = new QTextEdit(dtcPage);
    dtcOutput->setReadOnly(true);
    dtcLayout->addWidget(dtcOutput);
    connect(storedButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::readStoredDtcs);
    connect(pendingButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::readPendingDtcs);
    connect(permanentButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::readPermanentDtcs);
    connect(clearButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::clearDtcs);
    connect(loadDtcButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::loadDtcDatabase);
    connect(reportButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::exportDiagnosticReport);
    connect(clearDtcOutputButton, &QPushButton::clicked, dtcOutput, &QTextEdit::clear);
    tabs->addTab(dtcPage, tr("DTCs"));

    QWidget *freezePage = new QWidget(tabs);
    QVBoxLayout *freezeLayout = new QVBoxLayout(freezePage);
    QHBoxLayout *freezeControls = new QHBoxLayout;
    freezePidEdit = new QLineEdit(QStringLiteral("0x02"), freezePage);
    freezeFrameSpin = new QSpinBox(freezePage);
    freezeFrameSpin->setRange(0, 255);
    QPushButton *freezeButton = new QPushButton(tr("Request freeze frame"), freezePage);
    QPushButton *freezeScanButton = new QPushButton(tr("Scan frame PIDs"), freezePage);
    QPushButton *freezeClearButton = new QPushButton(tr("Clear output"), freezePage);
    freezeControls->addWidget(new QLabel(tr("PID"), freezePage));
    freezeControls->addWidget(freezePidEdit);
    freezeControls->addWidget(new QLabel(tr("Frame"), freezePage));
    freezeControls->addWidget(freezeFrameSpin);
    freezeControls->addWidget(freezeButton);
    freezeControls->addWidget(freezeScanButton);
    freezeControls->addWidget(freezeClearButton);
    freezeControls->addStretch();
    freezeLayout->addLayout(freezeControls);
    freezeOutput = new QTextEdit(freezePage);
    freezeOutput->setReadOnly(true);
    freezeLayout->addWidget(freezeOutput);
    connect(freezeButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::requestFreezeFrame);
    connect(freezeScanButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::scanFreezeFramePids);
    connect(freezeClearButton, &QPushButton::clicked, freezeOutput, &QTextEdit::clear);
    tabs->addTab(freezePage, tr("Freeze frames"));

    QWidget *monitorPage = new QWidget(tabs);
    QVBoxLayout *monitorLayout = new QVBoxLayout(monitorPage);
    QHBoxLayout *monitorControls = new QHBoxLayout;
    monitorMidEdit = new QLineEdit(QStringLiteral("0x00"), monitorPage);
    QPushButton *monitorButton = new QPushButton(tr("Request monitor results"), monitorPage);
    QPushButton *monitorScalingButton = new QPushButton(tr("Load UAS table"), monitorPage);
    QPushButton *monitorClearButton = new QPushButton(tr("Clear output"), monitorPage);
    monitorControls->addWidget(new QLabel(tr("Monitor ID"), monitorPage));
    monitorControls->addWidget(monitorMidEdit);
    monitorControls->addWidget(monitorButton);
    monitorControls->addWidget(monitorScalingButton);
    monitorControls->addWidget(monitorClearButton);
    monitorControls->addStretch();
    monitorLayout->addLayout(monitorControls);
    monitorScalingStatus = new QLabel(tr("Raw Mode 06 values (no UAS conversion table loaded)"), monitorPage);
    monitorLayout->addWidget(monitorScalingStatus);
    monitorOutput = new QTextEdit(monitorPage);
    monitorOutput->setReadOnly(true);
    monitorLayout->addWidget(monitorOutput);
    connect(monitorButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::requestMonitorResults);
    connect(monitorScalingButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::loadMode06Scalings);
    connect(monitorClearButton, &QPushButton::clicked, monitorOutput, &QTextEdit::clear);
    tabs->addTab(monitorPage, tr("Monitor tests"));

    QWidget *infoPage = new QWidget(tabs);
    QVBoxLayout *infoLayout = new QVBoxLayout(infoPage);
    QHBoxLayout *infoControls = new QHBoxLayout;
    vehiclePidEdit = new QComboBox(infoPage);
    vehiclePidEdit->setEditable(true);
    vehiclePidEdit->addItem(tr("0x00 - Supported vehicle-information PIDs"), 0x00);
    vehiclePidEdit->addItem(tr("0x01 - VIN message count"), 0x01);
    vehiclePidEdit->addItem(tr("0x02 - Vehicle identification number"), 0x02);
    vehiclePidEdit->addItem(tr("0x03 - Calibration ID message count"), 0x03);
    vehiclePidEdit->addItem(tr("0x04 - Calibration IDs"), 0x04);
    vehiclePidEdit->addItem(tr("0x05 - CVN message count"), 0x05);
    vehiclePidEdit->addItem(tr("0x06 - Calibration verification numbers"), 0x06);
    vehiclePidEdit->addItem(tr("0x07 - Performance tracking message count"), 0x07);
    vehiclePidEdit->addItem(tr("0x08 - In-use performance tracking"), 0x08);
    vehiclePidEdit->addItem(tr("0x09 - ECU name message count"), 0x09);
    vehiclePidEdit->addItem(tr("0x0A - ECU name"), 0x0A);
    vehiclePidEdit->setCurrentIndex(2);
    QPushButton *infoButton = new QPushButton(tr("Request Mode 09"), infoPage);
    QPushButton *infoClearButton = new QPushButton(tr("Clear output"), infoPage);
    infoControls->addWidget(new QLabel(tr("Information PID"), infoPage));
    infoControls->addWidget(vehiclePidEdit);
    infoControls->addWidget(infoButton);
    infoControls->addWidget(infoClearButton);
    infoControls->addStretch();
    infoLayout->addLayout(infoControls);
    vehicleOutput = new QTextEdit(infoPage);
    vehicleOutput->setReadOnly(true);
    infoLayout->addWidget(vehicleOutput);
    connect(infoButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::requestVehicleInfo);
    connect(infoClearButton, &QPushButton::clicked, vehicleOutput, &QTextEdit::clear);
    tabs->addTab(infoPage, tr("Vehicle information"));
    requestControls = {selectedButton, enabledButton, pollStartButton, pollIntervalSpin,
        dashboardSendOnceButton,
        moduleScanButton, pidScanButton,
        storedButton, pendingButton, permanentButton, clearButton, freezeButton, freezeScanButton,
        monitorButton, infoButton};
    for (QWidget *control : requestControls) control->setEnabled(false);
    root->addWidget(tabs, 1);

    eventLog = new QTextEdit(this);
    eventLog->setReadOnly(true);
    eventLog->setMaximumHeight(120);
    root->addWidget(eventLog);
    QPushButton *clearLogButton = new QPushButton(tr("Clear log"), this);
    root->addWidget(clearLogButton, 0, Qt::AlignRight);
    connect(clearLogButton, &QPushButton::clicked, eventLog, &QTextEdit::clear);
}

void OBD2WorkbenchWindow::addDashboardPid()
{
    bool ok = false;
    const int pid = parseNumber(dashboardPidCombo->currentText().section(QStringLiteral(" - "), 0, 0), &ok);
    if (!ok || pid > 0xFF) { eventLog->append(tr("Invalid dashboard PID")); return; }
    OBDDashboardCanvas::WidgetConfig config;
    config.pid = pid;
    config.title = dashboardPidName(pid);
    config.format = knownPidFormat(pid);
    if (!configureDashboardWidget(this, &config)) return;
    int bottom = 0;
    for (const OBDDashboardCanvas::WidgetConfig &existing : widgetDashboardCanvas->configs())
        bottom = qMax(bottom, existing.y + existing.height);
    config.y = bottom;
    widgetDashboardCanvas->addWidgetConfig(config);
    syncDashboardPidRows();
}

void OBD2WorkbenchWindow::removeDashboardPid()
{
    widgetDashboardCanvas->removeSelected();
    syncDashboardPidRows();
}

void OBD2WorkbenchWindow::editDashboardWidget()
{
    OBDDashboardCanvas::WidgetConfig *config = widgetDashboardCanvas->selectedConfig();
    if (!config) { eventLog->append(tr("Right-click a dashboard widget to edit it")); return; }
    if (!configureDashboardWidget(this, config)) return;
    const QVector<OBDDashboardCanvas::WidgetConfig> configs = widgetDashboardCanvas->configs();
    widgetDashboardCanvas->setConfigs(configs);
    syncDashboardPidRows();
}

void OBD2WorkbenchWindow::rebuildDashboard()
{
    if (widgetDashboardCanvas) widgetDashboardCanvas->setColumns(dashboardColumnsSpin->value());
}

void OBD2WorkbenchWindow::syncDashboardPidRows()
{
    QSet<int> dashboardPids;
    for (const OBDDashboardCanvas::WidgetConfig &config : widgetDashboardCanvas->configs())
        dashboardPids.insert(config.pid);

    for (int row = pidTable->rowCount() - 1; row >= 0; --row)
    {
        bool ok = false;
        const int pid = parseNumber(pidText(row), &ok);
        const bool managed = pidTable->item(row, PidEnabled)->data(DashboardManagedRole).toBool();
        if (managed && (!ok || !dashboardPids.contains(pid)))
            pidTable->removeRow(row);
    }

    for (int pid : dashboardPids)
    {
        bool found = false;
        for (int row = 0; row < pidTable->rowCount(); ++row)
        {
            bool ok = false;
            if (parseNumber(pidText(row), &ok) == uint32_t(pid) && ok)
            {
                found = true;
                break;
            }
        }
        if (found) continue;
        addPid();
        const int row = pidTable->rowCount() - 1;
        setPidText(row, QStringLiteral("0x%1").arg(pid, 2, 16, QLatin1Char('0')).toUpper());
        pidTable->item(row, PidName)->setText(knownPidName(pid));
        pidTable->item(row, PidFormat)->setText(knownPidFormat(pid));
        pidTable->item(row, PidEnabled)->setCheckState(Qt::Unchecked);
        pidTable->item(row, PidEnabled)->setData(DashboardManagedRole, true);
    }
}

QList<int> OBD2WorkbenchWindow::combinedPidRows() const
{
    QList<int> rows;
    QSet<int> includedPids;
    QSet<int> dashboardPids;
    if (dashboardRequestsCheck->isChecked())
        for (const OBDDashboardCanvas::WidgetConfig &config : widgetDashboardCanvas->configs())
            dashboardPids.insert(config.pid);

    for (int row = 0; row < pidTable->rowCount(); ++row)
    {
        bool ok = false;
        const int pid = parseNumber(pidText(row), &ok);
        if (!ok || includedPids.contains(pid)) continue;
        const bool liveEnabled = liveRequestsCheck->isChecked() &&
            pidTable->item(row, PidEnabled)->checkState() == Qt::Checked;
        if (liveEnabled || dashboardPids.contains(pid))
        {
            rows.append(row);
            includedPids.insert(pid);
        }
    }
    return rows;
}

QPair<QString, double> OBD2WorkbenchWindow::dashboardDisplayValue(const QString &name, double value) const
{
    return qMakePair(name, value);
}

void OBD2WorkbenchWindow::updateDashboard(int pid, const QString &ecu, const QString &decoded,
                                           const QVector<QPair<QString, double>> &numericValues)
{
    if (!dashboardValueLabels.contains(pid)) return;
    QStringList displayValues;
    for (const auto &sample : numericValues)
    {
        const auto converted = dashboardDisplayValue(sample.first, sample.second);
        displayValues << QStringLiteral("%1: %2").arg(converted.first).arg(converted.second, 0, 'g', 8);
    }
    dashboardValueLabels.value(pid)->setText(ecu + QStringLiteral("\n") +
        (displayValues.isEmpty() ? decoded : displayValues.join(QStringLiteral("\n"))));
    if (numericValues.isEmpty()) return;
    QStringList lines;
    for (const auto &sample : numericValues)
    {
        const auto converted = dashboardDisplayValue(sample.first, sample.second);
        const QString key = QStringLiteral("%1/%2/%3").arg(pid).arg(ecu, converted.first);
        DashboardStats &stats = dashboardStats[key];
        if (stats.count == 0) stats.minimum = stats.maximum = converted.second;
        else
        {
            stats.minimum = qMin(stats.minimum, converted.second);
            stats.maximum = qMax(stats.maximum, converted.second);
        }
        ++stats.count;
        stats.sum += converted.second;
        lines << tr("%1  min %2  avg %3  max %4").arg(converted.first)
            .arg(stats.minimum, 0, 'g', 7).arg(stats.sum / stats.count, 0, 'g', 7)
            .arg(stats.maximum, 0, 'g', 7);
        if (dashboardBars.contains(pid))
        {
            const double span = stats.maximum - stats.minimum;
            dashboardBars.value(pid)->setValue(span > 0.0
                ? int((converted.second - stats.minimum) * 1000.0 / span) : 500);
        }
    }
    dashboardStatsLabels.value(pid)->setText(lines.join('\n'));
}

QJsonArray OBD2WorkbenchWindow::dashboardToJson() const
{
    QJsonArray items;
    for (const OBDDashboardCanvas::WidgetConfig &config : widgetDashboardCanvas->configs())
    {
        QJsonObject item;
        item["type"] = config.type;
        item["pid"] = config.pid;
        item["title"] = config.title;
        item["format"] = config.format;
        item["minimum"] = config.minimum;
        item["maximum"] = config.maximum;
        item["x"] = config.x;
        item["y"] = config.y;
        item["width"] = config.width;
        item["height"] = config.height;
        items.append(item);
    }
    return items;
}

void OBD2WorkbenchWindow::loadDashboardJson(const QJsonArray &items)
{
    QVector<OBDDashboardCanvas::WidgetConfig> configs;
    int migrationY = 0;
    for (const QJsonValue &value : items)
    {
        OBDDashboardCanvas::WidgetConfig config;
        if (value.isDouble())
        {
            config.pid = value.toInt(-1);
            config.title = dashboardPidName(config.pid);
            config.format = knownPidFormat(config.pid);
            config.y = migrationY;
            migrationY += config.height;
        }
        else
        {
            const QJsonObject item = value.toObject();
            config.type = item["type"].toString("digital");
            config.pid = item["pid"].toInt(-1);
            config.title = item["title"].toString();
            config.format = item["format"].toString();
            config.minimum = item["minimum"].toDouble(0.0);
            config.maximum = item["maximum"].toDouble(100.0);
            config.x = item["x"].toInt(0);
            config.y = item["y"].toInt(0);
            config.width = item["width"].toInt(3);
            config.height = item["height"].toInt(2);
        }
        if (config.pid >= 0 && config.pid <= 0xFF) configs.append(config);
    }
    widgetDashboardCanvas->setConfigs(configs);
    syncDashboardPidRows();
}

void OBD2WorkbenchWindow::saveDashboardLayout()
{
    QString fileName = QFileDialog::getSaveFileName(this, tr("Save dashboard layout"), QString(),
                                                     tr("Dashboard layouts (*.json)"));
    if (fileName.isEmpty()) return;
    if (!fileName.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) fileName += QStringLiteral(".json");
    QJsonObject root;
    root["version"] = 2;
    root["columns"] = dashboardColumnsSpin->value();
    root["widgets"] = dashboardToJson();
    QSaveFile file(fileName);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(root).toJson()) < 0 || !file.commit())
        eventLog->append(tr("Could not save dashboard layout"));
}

void OBD2WorkbenchWindow::loadDashboardLayout()
{
    const QString fileName = QFileDialog::getOpenFileName(this, tr("Load dashboard layout"), QString(),
                                                           tr("Dashboard layouts (*.json)"));
    if (fileName.isEmpty()) return;
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) { eventLog->append(tr("Could not open dashboard layout")); return; }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject() ||
        (!document.object()["widgets"].isArray() && !document.object()["pids"].isArray()))
    {
        eventLog->append(tr("Invalid dashboard layout"));
        return;
    }
    dashboardColumnsSpin->setValue(document.object()["columns"].toInt(12));
    loadDashboardJson(document.object().contains("widgets")
        ? document.object()["widgets"].toArray() : document.object()["pids"].toArray());
}

uint32_t OBD2WorkbenchWindow::parseNumber(const QString &text, bool *ok) const
{
    QString value = text.trimmed();
    int base = 10;
    if (value.startsWith("0x", Qt::CaseInsensitive)) { value.remove(0, 2); base = 16; }
    return value.toUInt(ok, base);
}

QVector<QPair<uint32_t, uint32_t>> OBD2WorkbenchWindow::responseRules(bool *ok, QString *error) const
{
    QVector<QPair<uint32_t, uint32_t>> rules;
    bool valid = true;
    if (responseModeCombo->currentData().toString() == QStringLiteral("mask"))
    {
        bool idOk = false, maskOk = false;
        const uint32_t id = parseNumber(responseIdEdit->text(), &idOk);
        const uint32_t mask = parseNumber(responseMaskEdit->text(), &maskOk);
        valid = idOk && maskOk && id <= 0x1FFFFFFF && mask > 0 && mask <= 0x1FFFFFFF;
        if (valid) rules.append(qMakePair(id & mask, mask));
    }
    else
    {
        const QStringList entries = responseIdEdit->text().split(',', Qt::SkipEmptyParts);
        for (const QString &entryText : entries)
        {
            const QString entry = entryText.trimmed();
            const QStringList bounds = entry.split('-', Qt::KeepEmptyParts);
            bool firstOk = false, lastOk = false;
            const uint32_t first = parseNumber(bounds.value(0), &firstOk);
            const uint32_t last = bounds.size() == 1 ? first : parseNumber(bounds.value(1), &lastOk);
            if (bounds.size() == 1) lastOk = firstOk;
            if (bounds.size() > 2 || !firstOk || !lastOk || first > last ||
                last > 0x1FFFFFFF || quint64(last) - first > 4095)
            {
                valid = false;
                break;
            }
            for (quint64 id = first; id <= last; ++id)
                rules.append(qMakePair(uint32_t(id), id > 0x7FF ? 0x1FFFFFFFU : 0x7FFU));
        }
        valid = valid && !rules.isEmpty();
    }
    if (ok) *ok = valid;
    if (!valid && error)
        *error = tr("Use comma-separated IDs/ranges (maximum 4096 IDs per range), or a valid ID and mask");
    return valid ? rules : QVector<QPair<uint32_t, uint32_t>>();
}

bool OBD2WorkbenchWindow::responseMatches(uint32_t id) const
{
    bool ok = false;
    const auto rules = responseRules(&ok);
    if (!ok) return false;
    for (const auto &rule : rules)
        if ((id & rule.second) == rule.first) return true;
    return false;
}

QString OBD2WorkbenchWindow::pidText(int row) const
{
    QComboBox *combo = qobject_cast<QComboBox *>(pidTable->cellWidget(row, PidNumber));
    QString text = combo ? combo->currentText() : QString();
    const int separator = text.indexOf(QStringLiteral(" - "));
    if (separator >= 0) text.truncate(separator);
    return text.trimmed();
}

void OBD2WorkbenchWindow::setPidText(int row, const QString &text)
{
    QComboBox *combo = qobject_cast<QComboBox *>(pidTable->cellWidget(row, PidNumber));
    if (!combo) return;
    bool ok = false;
    const int pid = parseNumber(text, &ok);
    const int knownIndex = ok ? combo->findData(pid) : -1;
    if (knownIndex >= 0) combo->setCurrentIndex(knownIndex);
    else combo->setEditText(text);
}

QString OBD2WorkbenchWindow::knownPidName(int pid) const
{
    for (const KnownPid &known : knownPids) if (known.pid == pid) return tr(known.name);
    return tr("Custom PID");
}

QString OBD2WorkbenchWindow::knownPidFormat(int pid) const
{
    for (const KnownPid &known : knownPids) if (known.pid == pid) return QString::fromLatin1(known.format);
    return QString();
}

QString OBD2WorkbenchWindow::decodeCompoundPid(int pid, const QByteArray &data) const
{
    const auto byte = [&data](int index) { return quint8(data.at(index)); };
    const auto word = [&byte](int index) { return (byte(index) << 8) | byte(index + 1); };

    if ((pid == 0x01 || pid == 0x41) && data.size() >= 4)
        return decodeReadiness(pid, data);

    if (pid == 0x02 && data.size() >= 2)
    {
        if (word(0) == 0) return tr("No freeze-frame DTC stored");
        const char systems[] = {'P', 'C', 'B', 'U'};
        return QStringLiteral("%1%2%3%4%5").arg(systems[(byte(0) >> 6) & 3]).arg((byte(0) >> 4) & 3)
            .arg(byte(0) & 0xF, 1, 16).arg((byte(1) >> 4) & 0xF, 1, 16).arg(byte(1) & 0xF, 1, 16).toUpper();
    }

    if ((pid == 0x13 || pid == 0x1D) && !data.isEmpty())
    {
        QStringList sensors;
        for (int bit = 0; bit < 8; ++bit)
        {
            if (!(byte(0) & (1 << bit))) continue;
            const int bank = pid == 0x13 ? bit / 4 + 1 : bit / 2 + 1;
            const int sensor = pid == 0x13 ? bit % 4 + 1 : bit % 2 + 1;
            sensors << tr("B%1S%2").arg(bank).arg(sensor);
        }
        return sensors.isEmpty() ? tr("No oxygen sensors reported") : sensors.join(QStringLiteral(", "));
    }

    if ((pid == 0x78 || pid == 0x79) && data.size() >= 9)
    {
        QStringList values;
        const int bank = pid == 0x78 ? 1 : 2;
        for (int sensor = 0; sensor < 4; ++sensor)
        {
            if (!(byte(0) & (1 << sensor))) continue;
            const double temperature = word(1 + sensor * 2) / 10.0 - 40.0;
            values << tr("B%1S%2: %3 C").arg(bank).arg(sensor + 1).arg(temperature, 0, 'f', 1);
        }
        return values.isEmpty() ? tr("No EGT sensors reported as supported") : values.join(QStringLiteral(", "));
    }

    if (pid == 0x7C && data.size() >= 9)
    {
        static const char *names[] = {"B1 inlet", "B1 outlet", "B2 inlet", "B2 outlet"};
        QStringList values;
        for (int sensor = 0; sensor < 4; ++sensor)
        {
            if (!(byte(0) & (1 << sensor))) continue;
            const double temperature = word(1 + sensor * 2) / 10.0 - 40.0;
            values << tr("%1: %2 C").arg(QString::fromLatin1(names[sensor])).arg(temperature, 0, 'f', 1);
        }
        return values.isEmpty() ? tr("No DPF temperature sensors reported as supported") : values.join(QStringLiteral(", "));
    }

    if ((pid == 0x7D || pid == 0x7E) && !data.isEmpty())
    {
        const QString pollutant = pid == 0x7D ? QStringLiteral("NOx") : QStringLiteral("PM");
        QStringList values;
        if (byte(0) & 0x01) values << tr("Inside %1 control area").arg(pollutant);
        if (byte(0) & 0x02) values << tr("Outside %1 control area").arg(pollutant);
        if (byte(0) & 0x04) values << tr("Inside manufacturer %1 carve-out").arg(pollutant);
        if (byte(0) & 0x08) values << tr("%1 deficiency active").arg(pollutant);
        return values.isEmpty() ? tr("No %1 NTE status flags active").arg(pollutant)
                                : values.join(QStringLiteral(", "));
    }

    if (pid == 0x8B && data.size() >= 7)
    {
        QStringList values;
        const quint8 supported = byte(0);
        const quint8 status = byte(1);
        if (supported & 0x01) values << (status & 0x01 ? tr("DPF regen: active") : tr("DPF regen: inactive"));
        if (supported & 0x02) values << (status & 0x02 ? tr("Regen type: active") : tr("Regen type: passive"));
        if (supported & 0x04) values << (status & 0x04 ? tr("NOx adsorber: regenerating") : tr("NOx adsorber: adsorbing"));
        if (supported & 0x08) values << (status & 0x08 ? tr("Desulfurization: active") : tr("Desulfurization: inactive"));
        if (supported & 0x10) values << tr("DPF trigger: %1%").arg(byte(2) / 2.55, 0, 'f', 1);
        if (supported & 0x20) values << tr("Average regen interval: %1 min").arg(word(3));
        if (supported & 0x40) values << tr("Average regen distance: %1 km").arg(word(5));
        return values.isEmpty() ? tr("No aftertreatment fields reported as supported") : values.join(QStringLiteral(", "));
    }

    return QString();
}

QString OBD2WorkbenchWindow::decodeReadiness(int pid, const QByteArray &data) const
{
    const quint8 a = quint8(data[0]);
    const quint8 b = quint8(data[1]);
    const quint8 supported = quint8(data[pid == 0x01 ? 2 : 1]);
    const quint8 incomplete = quint8(data[pid == 0x01 ? 3 : 2]);
    QStringList lines;
    if (pid == 0x01)
        lines << tr("MIL: %1; stored DTC count: %2").arg(a & 0x80 ? tr("ON") : tr("off")).arg(a & 0x7F);
    const quint8 continuous = pid == 0x01 ? b : a;
    static const char *continuousNames[] = {"Misfire", "Fuel system", "Comprehensive components"};
    for (int bit = 0; bit < 3; ++bit)
        if (continuous & (1 << bit))
            lines << tr("%1: %2").arg(tr(continuousNames[bit]))
                .arg(continuous & (1 << (bit + 4)) ? tr("incomplete") : tr("complete"));

    const bool compressionIgnition = (pid == 0x01 ? b : a) & 0x08;
    static const char *sparkNames[] = {
        "Catalyst", "Heated catalyst", "Evaporative system", "Secondary air",
        "A/C refrigerant", "Oxygen sensor", "Oxygen sensor heater", "EGR/VVT"
    };
    static const char *compressionNames[] = {
        "NMHC catalyst", "NOx/SCR monitor", "Reserved", "Boost pressure",
        "Exhaust gas sensor", "PM filter", "EGR/VVT", "Reserved"
    };
    const char *const *names = compressionIgnition ? compressionNames : sparkNames;
    for (int bit = 0; bit < 8; ++bit)
        if (supported & (1 << bit))
            lines << tr("%1: %2").arg(tr(names[bit]))
                .arg(incomplete & (1 << bit) ? tr("incomplete") : tr("complete"));
    return lines.isEmpty() ? tr("No readiness monitors reported") : lines.join(QStringLiteral("; "));
}

void OBD2WorkbenchWindow::connectEndpoint()
{
    bool ok = false;
    const uint32_t requestId = parseNumber(requestIdEdit->currentText(), &ok);
    bool rulesOk = false;
    QString rulesError;
    const auto rules = responseRules(&rulesOk, &rulesError);
    if (!ok || !rulesOk) {
        connectionStatus->setText(ok ? rulesError : tr("Invalid request ID")); return;
    }
    if (!CANConManager::getInstance()->isBusConnected(busSpin->value())) {
        connected = false;
        connectionStatus->setText(tr("Bus %1 is not connected").arg(busSpin->value()));
        eventLog->append(tr("Cannot start OBD-II: selected CAN bus is not connected"));
        return;
    }
    handler->clearAllFilters();
    for (const auto &rule : rules)
        handler->addFilter(busSpin->value(), rule.first, rule.second);
    handler->setFlowCtrl(true);
    handler->setReception(true);
    connected = true;
    connectButton->setText(tr("Disable OBD"));
    for (QWidget *control : requestControls) control->setEnabled(true);
    dashboardPollStartButton->setEnabled(true);
    connectionStatus->setText(tr("Ready - awaiting ECU response"));
    eventLog->append(tr("Listening for OBD-II ECU responses"));
    Q_UNUSED(requestId)
}

bool OBD2WorkbenchWindow::transmissionAllowed(bool modifying)
{
    const QString mode = safetyModeCombo->currentData().toString();
    if (mode == QStringLiteral("passive"))
    {
        connectionStatus->setText(tr("Passive safety mode blocks transmission"));
        return false;
    }
    if (modifying && mode != QStringLiteral("full"))
    {
        connectionStatus->setText(tr("Full diagnostics safety mode required"));
        return false;
    }
    return true;
}

void OBD2WorkbenchWindow::refreshEcuTargets()
{
    const QString selected = ecuTargetCombo->currentData().toString();
    ecuTargetCombo->clear();
    ecuTargetCombo->addItem(tr("All ECUs (functional)"), QStringLiteral("0x7DF"));
    for (auto iterator = supportedPidsByEcu.constBegin(); iterator != supportedPidsByEcu.constEnd(); ++iterator)
    {
        const uint32_t responseId = iterator.key();
        if (responseId >= 8 && responseId <= 0x7FF)
            ecuTargetCombo->addItem(tr("ECU response 0x%1").arg(responseId, 0, 16).toUpper(),
                QStringLiteral("0x%1").arg(responseId - 8, 0, 16).toUpper());
    }
    const int index = ecuTargetCombo->findData(selected);
    ecuTargetCombo->setCurrentIndex(index >= 0 ? index : 0);
}

void OBD2WorkbenchWindow::disconnectEndpoint()
{
    stopPolling();
    responseTimer.stop();
    responseSettleTimer.stop();
    activePidRow = -1;
    activeMode = -1;
    context = None;
    handler->clearAllFilters();
    handler->setReception(false);
    connected = false;
    connectButton->setText(tr("Enable OBD"));
    for (QWidget *control : requestControls) control->setEnabled(false);
    dashboardPollStartButton->setEnabled(false);
    connectionStatus->setText(tr("Disconnected"));
    eventLog->append(tr("Stopped OBD-II requests and response listening"));
}

void OBD2WorkbenchWindow::addPid()
{
    const int row = pidTable->rowCount();
    pidTable->insertRow(row);
    QTableWidgetItem *enabled = new QTableWidgetItem;
    enabled->setCheckState(Qt::Checked);
    pidTable->setItem(row, PidEnabled, enabled);
    pidTable->setItem(row, PidName, new QTableWidgetItem(tr("Engine RPM")));
    QComboBox *pidCombo = new QComboBox(pidTable);
    pidCombo->setEditable(true);
    pidCombo->setInsertPolicy(QComboBox::NoInsert);
    for (const KnownPid &known : knownPids)
        pidCombo->addItem(QStringLiteral("0x%1 - %2").arg(known.pid, 2, 16, QLatin1Char('0')).arg(known.name), known.pid);
    pidTable->setCellWidget(row, PidNumber, pidCombo);
    setPidText(row, QStringLiteral("0x0C"));
    pidTable->setItem(row, PidFormat, new QTableWidgetItem(knownPidFormat(0x0C)));
    pidTable->setItem(row, PidRaw, new QTableWidgetItem);
    pidTable->setItem(row, PidResponses, new QTableWidgetItem);
    pidTable->setItem(row, PidUpdated, new QTableWidgetItem);
    connect(pidCombo, qOverload<int>(&QComboBox::activated), this, [this, pidCombo](int index) {
        const int pid = pidCombo->itemData(index).toInt();
        for (int row = 0; row < pidTable->rowCount(); ++row)
            if (pidTable->cellWidget(row, PidNumber) == pidCombo)
            {
                pidTable->item(row, PidName)->setText(knownPidName(pid));
                pidTable->item(row, PidFormat)->setText(knownPidFormat(pid));
                break;
            }
    });
}

bool OBD2WorkbenchWindow::addPidRequest(const QString &name, const QString &pid,
                                        const QString &format, QString *error)
{
    bool ok = false;
    const uint32_t identifier = parseNumber(pid, &ok);
    if (!ok || identifier > 0xFF)
    {
        if (error) *error = tr("PID must be between 0x00 and 0xFF.");
        return false;
    }
    addPid();
    const int row = pidTable->rowCount() - 1;
    setPidText(row, QStringLiteral("0x%1").arg(identifier, 2, 16, QLatin1Char('0')).toUpper());
    pidTable->item(row, PidName)->setText(name.trimmed().isEmpty()
        ? knownPidName(identifier) : name.trimmed());
    pidTable->item(row, PidFormat)->setText(format.trimmed().isEmpty()
        ? knownPidFormat(identifier) : format.trimmed());
    pidTable->selectRow(row);
    return true;
}

bool OBD2WorkbenchWindow::executeAIRequest(const QString &operation,
                                           const QJsonObject &arguments, QString *error)
{
    if (operation == QStringLiteral("connect")) {
        connectEndpoint();
        return connected;
    }
    if (operation == QStringLiteral("disconnect")) {
        disconnectEndpoint();
        return !connected;
    }
    if (operation == QStringLiteral("stop_polling")) {
        stopPolling();
        return !pollCheck->isChecked();
    }
    if (operation == QStringLiteral("edit_pid")) {
        bool requestedOk = false;
        const uint32_t requested = parseNumber(
            arguments.value("pid").toVariant().toString(), &requestedOk);
        for (int row = 0; requestedOk && row < pidTable->rowCount(); ++row) {
            bool rowOk = false;
            if (parseNumber(pidTable->item(row, PidNumber)->text(), &rowOk) != requested || !rowOk)
                continue;
            if (arguments.contains("name")) pidTable->item(row, PidName)->setText(arguments.value("name").toString());
            if (arguments.contains("format")) pidTable->item(row, PidFormat)->setText(arguments.value("format").toString());
            if (arguments.contains("enabled")) pidTable->item(row, PidEnabled)->setCheckState(
                arguments.value("enabled").toBool() ? Qt::Checked : Qt::Unchecked);
            syncDashboardPidRows();
            return true;
        }
        if (error) *error = tr("The requested PID is not in the OBD-II list.");
        return false;
    }
    if (operation == QStringLiteral("remove_pids")) {
        QSet<int> requested;
        for (const QJsonValue &value : arguments.value("pids").toArray()) {
            bool ok = false;
            const int pid = parseNumber(value.toVariant().toString(), &ok);
            if (ok && pid >= 0 && pid <= 0xFF) requested.insert(pid);
        }
        int removed = 0;
        for (int row = pidTable->rowCount() - 1; row >= 0; --row) {
            bool ok = false;
            const int pid = parseNumber(pidTable->item(row, PidNumber)->text(), &ok);
            if (ok && requested.contains(pid)) {
                pidTable->removeRow(row);
                ++removed;
            }
        }
        syncDashboardPidRows();
        if (error) *error = tr("Removed %1 PID request(s)").arg(removed);
        return true;
    }
    if (operation == QStringLiteral("dashboard_set")) {
        if (arguments.contains("columns"))
            dashboardColumnsSpin->setValue(arguments.value("columns").toInt(12));
        loadDashboardJson(arguments.value("widgets").toArray());
        return true;
    }
    if (operation == QStringLiteral("dashboard_clear")) {
        loadDashboardJson(QJsonArray());
        return true;
    }
    if (operation == QStringLiteral("dashboard_save")) {
        QJsonObject root{{"version", 2}, {"columns", dashboardColumnsSpin->value()},
                         {"widgets", dashboardToJson()}};
        QSaveFile file(arguments.value("path").toString());
        const bool ok = file.open(QIODevice::WriteOnly)
            && file.write(QJsonDocument(root).toJson()) >= 0 && file.commit();
        if (!ok && error) *error = tr("Could not save the dashboard layout.");
        return ok;
    }
    if (operation == QStringLiteral("dashboard_load")) {
        QFile file(arguments.value("path").toString());
        if (!file.open(QIODevice::ReadOnly)) {
            if (error) *error = tr("Could not open the dashboard layout.");
            return false;
        }
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
        if (!document.isObject() || !document.object().value("widgets").isArray()) {
            if (error) *error = tr("Dashboard layout is invalid.");
            return false;
        }
        dashboardColumnsSpin->setValue(document.object().value("columns").toInt(12));
        loadDashboardJson(document.object().value("widgets").toArray());
        return true;
    }
    if (!connected || responseTimer.isActive()) {
        if (error) *error = tr("Connect the OBD-II endpoint and wait for any active request.");
        return false;
    }
    if (operation == QStringLiteral("query_pid")) {
        bool requestedOk = false;
        const uint32_t requested = parseNumber(arguments.value("pid").toString(), &requestedOk);
        for (int row = 0; requestedOk && row < pidTable->rowCount(); ++row) {
            bool rowOk = false;
            if (parseNumber(pidTable->item(row, PidNumber)->text(), &rowOk) == requested && rowOk) {
                pidTable->setCurrentCell(row, PidNumber);
                requestSelectedPid();
                return responseTimer.isActive();
            }
        }
        if (error) *error = tr("The requested PID is not in the OBD-II list.");
        return false;
    }
    if (operation == QStringLiteral("scan_modules")) { scanModules(); return responseTimer.isActive(); }
    if (operation == QStringLiteral("scan_pids")) { scanSupportedPids(); return responseTimer.isActive(); }
    if (operation == QStringLiteral("request_enabled")) {
        requestEnabledPids();
        return responseTimer.isActive();
    }
    if (operation == QStringLiteral("start_polling")) {
        startPolling();
        if (!pollCheck->isChecked() && error)
            *error = tr("Polling could not start; connect OBD and enable at least one request.");
        return pollCheck->isChecked();
    }
    if (operation == QStringLiteral("read_dtcs")) {
        const QString kind = arguments.value("kind").toString().toLower();
        if (kind == QStringLiteral("pending")) readPendingDtcs();
        else if (kind == QStringLiteral("permanent")) readPermanentDtcs();
        else readStoredDtcs();
        return responseTimer.isActive();
    }
    if (operation == QStringLiteral("clear_dtcs")) {
        sendRequest(4, {}, ClearDtc);
        return responseTimer.isActive();
    }
    if (operation == QStringLiteral("vehicle_info")) {
        vehiclePidEdit->setCurrentText(arguments.value("pid").toVariant().toString());
        requestVehicleInfo();
        return responseTimer.isActive();
    }
    if (operation == QStringLiteral("freeze_frame")) {
        freezePidEdit->setText(arguments.value("pid").toVariant().toString());
        freezeFrameSpin->setValue(arguments.value("frame").toInt(0));
        requestFreezeFrame();
        return responseTimer.isActive();
    }
    if (operation == QStringLiteral("monitor_results")) {
        monitorMidEdit->setText(arguments.value("mid").toVariant().toString());
        requestMonitorResults();
        return responseTimer.isActive();
    }
    if (operation == QStringLiteral("read_readiness")) {
        for (int row = 0; row < pidTable->rowCount(); ++row) {
            bool ok = false;
            if (parseNumber(pidTable->item(row, PidNumber)->text(), &ok) == 0x01 && ok) {
                pidTable->setCurrentCell(row, PidNumber);
                requestSelectedPid();
                return responseTimer.isActive();
            }
        }
        if (!addPidRequest(tr("Monitor status since DTCs cleared"), QStringLiteral("0x01"),
                           QStringLiteral("auto"), error))
            return false;
        pidTable->setCurrentCell(pidTable->rowCount() - 1, PidNumber);
        requestSelectedPid();
        return responseTimer.isActive();
    }
    if (error) *error = tr("Unsupported OBD-II AI operation: %1").arg(operation);
    return false;
}

bool OBD2WorkbenchWindow::addDashboardPidByNumber(int pid, QString *error)
{
    for (int index = 0; index < dashboardPidCombo->count(); ++index)
    {
        if (dashboardPidCombo->itemData(index).toInt() != pid) continue;
        dashboardPidCombo->setCurrentIndex(index);
        addDashboardPid();
        return true;
    }
    if (error) *error = tr("PID 0x%1 is not available in the dashboard PID list.")
        .arg(pid, 2, 16, QLatin1Char('0')).toUpper();
    return false;
}

int OBD2WorkbenchWindow::clearPidRequests()
{
    const int removed = pidTable->rowCount();
    pidQueue.clear();
    pidTable->setRowCount(0);
    return removed;
}

void OBD2WorkbenchWindow::removePid()
{
    QSet<int> selectedRows;
    for (const QModelIndex &index : pidTable->selectionModel()->selectedRows())
        selectedRows.insert(index.row());
    QList<int> rows = selectedRows.values();
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    for (int row : rows) pidTable->removeRow(row);
}

void OBD2WorkbenchWindow::loadPidList()
{
    const QString fileName = QFileDialog::getOpenFileName(this, tr("Load OBD-II PID list"), QString(),
                                                           tr("OBD-II PID lists (*.json);;All files (*)"));
    if (fileName.isEmpty()) return;
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) { eventLog->append(tr("Could not open PID list: %1").arg(file.errorString())); return; }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject() || !document.object()["pids"].isArray())
    {
        eventLog->append(tr("Invalid PID list: %1").arg(error.errorString()));
        return;
    }
    const QJsonObject profile = document.object();
    busSpin->setValue(profile["bus"].toInt(busSpin->value()));
    requestIdEdit->setCurrentText(profile["requestId"].toString(requestIdEdit->currentText()));
    loadPidRows(profile["pids"].toArray());
    syncDashboardPidRows();
    connected = false;
    connectionStatus->setText(tr("Reconnect imported endpoint"));
    eventLog->append(tr("Loaded %1 PIDs from %2").arg(pidTable->rowCount()).arg(fileName));
}

void OBD2WorkbenchWindow::savePidList()
{
    QString fileName = QFileDialog::getSaveFileName(this, tr("Save OBD-II PID list"), QString(),
                                                     tr("OBD-II PID lists (*.json);;All files (*)"));
    if (fileName.isEmpty()) return;
    if (!fileName.endsWith(".json", Qt::CaseInsensitive)) fileName += ".json";
    QJsonObject profile;
    profile["version"] = 1;
    profile["bus"] = busSpin->value();
    profile["requestId"] = requestIdEdit->currentText();
    profile["pids"] = pidRowsToJson();
    QSaveFile file(fileName);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(profile).toJson()) < 0 || !file.commit())
    {
        eventLog->append(tr("Could not save PID list: %1").arg(file.errorString()));
        return;
    }
    eventLog->append(tr("Saved %1 PIDs to %2").arg(pidTable->rowCount()).arg(fileName));
}

void OBD2WorkbenchWindow::requestSelectedPid()
{
    if (pidTable->currentRow() < 0 || responseTimer.isActive()) return;
    pidQueue = {pidTable->currentRow()};
    requestFinished();
}

void OBD2WorkbenchWindow::requestEnabledPids()
{
    if (responseTimer.isActive()) return;
    syncDashboardPidRows();
    pidQueue = combinedPidRows();
    requestFinished();
}

void OBD2WorkbenchWindow::pollPids()
{
    if (!connected || responseTimer.isActive() || !pidQueue.isEmpty()) return;
    pollCycleStartedMs = QDateTime::currentMSecsSinceEpoch();
    pollCycleActive = true;
    requestEnabledPids();
}

void OBD2WorkbenchWindow::startPolling()
{
    if (!connected)
    {
        connectionStatus->setText(tr("Enable OBD before starting polling"));
        return;
    }
    syncDashboardPidRows();
    if (combinedPidRows().isEmpty())
    {
        connectionStatus->setText(tr("Enable a Live Data row or Dashboard widget before starting polling"));
        return;
    }
    pollCheck->setChecked(true);
    pollStartButton->setText(tr("Stop polling"));
    pollStartButton->setEnabled(true);
    dashboardPollStartButton->setText(tr("Stop polling"));
    dashboardPollStartButton->setEnabled(true);
    connectionStatus->setText(tr("Polling enabled PIDs; %1 ms between cycles").arg(pollIntervalSpin->value()));
    eventLog->append(tr("Started PID polling"));
    pollPids();
}

void OBD2WorkbenchWindow::stopPolling()
{
    const bool wasActive = pollCheck->isChecked();
    pollTimer.stop();
    pollCheck->setChecked(false);
    pidQueue.clear();
    pollCycleActive = false;
    pollCycleStartedMs = 0;
    pollStartButton->setText(tr("Start polling"));
    pollStartButton->setEnabled(connected);
    dashboardPollStartButton->setText(tr("Start polling"));
    dashboardPollStartButton->setEnabled(connected);
    if (wasActive) eventLog->append(tr("Stopped PID polling"));
}

void OBD2WorkbenchWindow::responseTimedOut()
{
    if (context == LivePid && activePidRow >= 0 && activePidRow < pidTable->rowCount()
        && !activePidHadResponse)
    {
        pidTable->item(activePidRow, PidResponses)->setText(
            tr("No complete response (timeout; check ISO-TP consecutive frames)"));
        pidTable->item(activePidRow, PidUpdated)->setText(
            QDateTime::currentDateTime().toString("HH:mm:ss.zzz"));
    }
    requestFinished();
}

void OBD2WorkbenchWindow::responseBurstFinished()
{
    if (responseTimer.isActive()) requestFinished();
}

void OBD2WorkbenchWindow::sendRequest(int mode, const QByteArray &data, Context nextContext)
{
    if (!transmissionAllowed(mode == 4 || mode == 8)) return;
    if (!connected || responseTimer.isActive()) { connectionStatus->setText(tr("Connect first or wait for the active request")); return; }
    bool ok = false;
    const uint32_t requestId = parseNumber(requestIdEdit->currentText(), &ok);
    if (!ok) return;
    UDS_MESSAGE message;
    message.bus = busSpin->value();
    message.setFrameId(requestId);
    message.setExtendedFrameFormat(requestId > 0x7FF);
    message.service = mode;
    message.subFuncLen = 0;
    message.setPayload(data);
    activeMode = mode;
    context = nextContext;
    responseSettleTimer.stop();
    if (!handler->sendUDSFrame(message)) {
        connected = false;
        connectButton->setText(tr("Enable OBD"));
        for (QWidget *control : requestControls) control->setEnabled(false);
        stopPolling();
        connectionStatus->setText(tr("Transmit failed on bus %1").arg(message.bus));
        eventLog->append(tr("OBD-II transmit failed: bus %1, ID 0x%2")
                         .arg(message.bus).arg(requestId, 0, 16).toUpper());
        if (activePidRow >= 0 && activePidRow < pidTable->rowCount())
            pidTable->item(activePidRow, PidResponses)->setText(tr("Transmit failed"));
        pidQueue.clear();
        activePidRow = -1;
        activeMode = -1;
        context = None;
        return;
    }
    eventLog->append(tr("TX bus %1 ID 0x%2 service 0x%3 data %4")
                     .arg(message.bus)
                     .arg(requestId, 0, 16)
                     .arg(mode, 2, 16, QLatin1Char('0'))
                     .arg(QString::fromLatin1(data.toHex(' '))).toUpper());
    responseTimer.start(900);
}

void OBD2WorkbenchWindow::readStoredDtcs() { dtcOutput->clear(); sendRequest(3, {}, StoredDtc); }
void OBD2WorkbenchWindow::readPendingDtcs() { dtcOutput->clear(); sendRequest(7, {}, PendingDtc); }
void OBD2WorkbenchWindow::readPermanentDtcs() { dtcOutput->clear(); sendRequest(0xA, {}, PermanentDtc); }

void OBD2WorkbenchWindow::clearDtcs()
{
    if (QMessageBox::warning(this, tr("Clear OBD-II DTCs"), tr("Clear DTCs and emissions readiness data?"),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes)
        sendRequest(4, {}, ClearDtc);
}

void OBD2WorkbenchWindow::requestVehicleInfo()
{
    bool ok = false;
    QString pidText = vehiclePidEdit->currentText();
    const int separator = pidText.indexOf(QStringLiteral(" - "));
    if (separator >= 0) pidText.truncate(separator);
    const uint32_t pid = parseNumber(pidText, &ok);
    if (!ok || pid > 0xFF) { vehicleOutput->setText(tr("Invalid information PID")); return; }
    vehicleOutput->clear();
    activePid = pid;
    sendRequest(9, QByteArray(1, char(pid)), VehicleInfo);
}

void OBD2WorkbenchWindow::scanModules()
{
    discoveryOutput->clear();
    discoveryPidList->clear();
    supportedPidsByEcu.clear();
    discoveryEcuCombo->clear();
    discoveryEcuCombo->addItem(tr("All ECUs"), QStringLiteral("all"));
    discoveryOutput->setText(tr("Sending functional Mode 01 PID 00 request..."));
    activePid = 0;
    sendRequest(1, QByteArray(1, char(0x00)), ModuleScan);
}

void OBD2WorkbenchWindow::scanSupportedPids()
{
    if (!connected || responseTimer.isActive()) { connectionStatus->setText(tr("Connect first or wait for the active request")); return; }
    discoveryOutput->setText(tr("Scanning standard Mode 01 support pages..."));
    discoveryPidList->clear();
    supportedPidsByEcu.clear();
    discoveryEcuCombo->clear();
    discoveryEcuCombo->addItem(tr("All ECUs"), QStringLiteral("all"));
    scanPidBases = {0x00, 0x20, 0x40, 0x60, 0x80, 0xA0, 0xC0};
    activePid = scanPidBases.takeFirst();
    sendRequest(1, QByteArray(1, char(activePid)), SupportedPidScan);
}

QString OBD2WorkbenchWindow::supportedPidText(const QByteArray &data, int basePid, QSet<int> *result) const
{
    QStringList pids;
    for (int byteIndex = 0; byteIndex < qMin(4, data.size()); ++byteIndex)
        for (int bit = 0; bit < 8; ++bit)
            if (quint8(data.at(byteIndex)) & (0x80 >> bit))
            {
                const int pid = basePid + byteIndex * 8 + bit + 1;
                pids << QStringLiteral("0x%1").arg(pid, 2, 16, QLatin1Char('0')).toUpper();
                if (result) result->insert(pid);
            }
    return pids.isEmpty() ? tr("none reported") : pids.join(QStringLiteral(", "));
}

void OBD2WorkbenchWindow::finishSupportedPidScan()
{
    discoveryOutput->append(QString());
    discoveryOutput->append(tr("Scan complete. Select PIDs below to add them to Live data."));
    refreshDiscoveryPidList();
    refreshEcuTargets();
}

void OBD2WorkbenchWindow::refreshDiscoveryPidList()
{
    const QString previousSelection = discoveryEcuCombo->currentData().toString();
    {
        const QSignalBlocker blocker(discoveryEcuCombo);
        discoveryEcuCombo->clear();
        discoveryEcuCombo->addItem(tr("All ECUs"), QStringLiteral("all"));
        QList<uint32_t> ecuIds = supportedPidsByEcu.keys();
        std::sort(ecuIds.begin(), ecuIds.end());
        for (uint32_t ecuId : ecuIds)
            discoveryEcuCombo->addItem(
                tr("ECU 0x%1 (%2 PIDs)").arg(ecuId, 0, 16)
                    .arg(supportedPidsByEcu.value(ecuId).size()).toUpper(),
                QStringLiteral("0x%1").arg(ecuId, 0, 16).toUpper());
        const int previousIndex = discoveryEcuCombo->findData(previousSelection);
        discoveryEcuCombo->setCurrentIndex(previousIndex >= 0 ? previousIndex : 0);
    }

    QSet<int> visiblePids;
    bool selectedOk = false;
    const uint32_t selectedEcu = parseNumber(discoveryEcuCombo->currentData().toString(), &selectedOk);
    if (selectedOk)
        visiblePids = supportedPidsByEcu.value(selectedEcu);
    else
        for (auto iterator = supportedPidsByEcu.constBegin(); iterator != supportedPidsByEcu.constEnd(); ++iterator)
            visiblePids.unite(iterator.value());

    discoveryPidList->clear();
    QList<int> sortedPids = visiblePids.values();
    std::sort(sortedPids.begin(), sortedPids.end());
    for (int pid : sortedPids)
    {
        if (pid % 0x20 == 0) continue;
        QStringList ecus;
        if (!selectedOk)
            for (auto iterator = supportedPidsByEcu.constBegin(); iterator != supportedPidsByEcu.constEnd(); ++iterator)
                if (iterator.value().contains(pid))
                    ecus << QStringLiteral("0x%1").arg(iterator.key(), 3, 16, QLatin1Char('0')).toUpper();
        const QString name = knownPidName(pid);
        QListWidgetItem *item = new QListWidgetItem(
            selectedOk
                ? QStringLiteral("0x%1  %2").arg(pid, 2, 16, QLatin1Char('0')).arg(name)
                : QStringLiteral("0x%1  %2  [%3]").arg(pid, 2, 16, QLatin1Char('0')).arg(name, ecus.join(", ")),
            discoveryPidList);
        item->setData(Qt::UserRole, pid);
    }
}

void OBD2WorkbenchWindow::addSelectedScannedPids()
{
    QSet<int> existing;
    for (int row = 0; row < pidTable->rowCount(); ++row)
    {
        bool ok = false;
        const int pid = parseNumber(pidText(row), &ok);
        if (ok) existing.insert(pid);
    }
    int added = 0;
    for (QListWidgetItem *item : discoveryPidList->selectedItems())
    {
        const int pid = item->data(Qt::UserRole).toInt();
        if (existing.contains(pid)) continue;
        addPid();
        const int row = pidTable->rowCount() - 1;
        setPidText(row, QStringLiteral("0x%1").arg(pid, 2, 16, QLatin1Char('0')));
        pidTable->item(row, PidName)->setText(knownPidName(pid));
        const QString format = knownPidFormat(pid);
        pidTable->item(row, PidFormat)->setText(format.isEmpty() ? QStringLiteral("auto") : format);
        existing.insert(pid);
        ++added;
    }
    discoveryOutput->append(tr("Added %1 selected PIDs to Live data.").arg(added));
}

void OBD2WorkbenchWindow::saveDiscoveryResults()
{
    if (supportedPidsByEcu.isEmpty())
    {
        discoveryOutput->append(tr("There are no discovery results to save."));
        return;
    }
    QString fileName = QFileDialog::getSaveFileName(this, tr("Save OBD-II discovery results"), QString(),
                                                     tr("OBD-II discovery results (*.json);;All files (*)"));
    if (fileName.isEmpty()) return;
    if (!fileName.endsWith(".json", Qt::CaseInsensitive)) fileName += ".json";

    QJsonArray ecus;
    for (auto iterator = supportedPidsByEcu.constBegin(); iterator != supportedPidsByEcu.constEnd(); ++iterator)
    {
        QList<int> sortedPids = iterator.value().values();
        std::sort(sortedPids.begin(), sortedPids.end());
        QJsonArray pids;
        for (int pid : sortedPids) pids.append(pid);
        QJsonObject ecu;
        ecu["responseId"] = QStringLiteral("0x%1").arg(iterator.key(), 0, 16).toUpper();
        ecu["supportedPids"] = pids;
        ecus.append(ecu);
    }

    QJsonObject document;
    document["version"] = 1;
    document["savedAt"] = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    document["requestId"] = requestIdEdit->currentText();
    document["responseMode"] = responseModeCombo->currentData().toString();
    document["responseSpec"] = responseIdEdit->text();
    document["responseMask"] = responseMaskEdit->text();
    document["ecus"] = ecus;
    QSaveFile file(fileName);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(QJsonDocument(document).toJson()) < 0 || !file.commit())
    {
        discoveryOutput->append(tr("Could not save discovery results: %1").arg(file.errorString()));
        return;
    }
    discoveryOutput->append(tr("Saved discovery results for %1 ECU(s) to %2.")
                            .arg(ecus.size()).arg(fileName));
}

void OBD2WorkbenchWindow::loadDiscoveryResults()
{
    const QString fileName = QFileDialog::getOpenFileName(this, tr("Load OBD-II discovery results"), QString(),
                                                          tr("OBD-II discovery results (*.json);;All files (*)"));
    if (fileName.isEmpty()) return;
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly))
    {
        discoveryOutput->append(tr("Could not open discovery results: %1").arg(file.errorString()));
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
        !document.object()["ecus"].isArray())
    {
        discoveryOutput->append(tr("Invalid discovery results: %1").arg(parseError.errorString()));
        return;
    }

    QMap<uint32_t, QSet<int>> loaded;
    for (const QJsonValue &value : document.object()["ecus"].toArray())
    {
        const QJsonObject ecu = value.toObject();
        bool idOk = false;
        const uint32_t responseId = parseNumber(ecu["responseId"].toString(), &idOk);
        if (!idOk || responseId > 0x1FFFFFFF || !ecu["supportedPids"].isArray()) continue;
        QSet<int> pids;
        for (const QJsonValue &pidValue : ecu["supportedPids"].toArray())
        {
            const int pid = pidValue.toInt(-1);
            if (pid >= 0 && pid <= 0xFF) pids.insert(pid);
        }
        loaded.insert(responseId, pids);
    }
    if (loaded.isEmpty())
    {
        discoveryOutput->append(tr("Discovery file contains no valid ECU results."));
        return;
    }
    supportedPidsByEcu = loaded;
    refreshEcuTargets();
    discoveryOutput->clear();
    discoveryPidList->clear();
    discoveryOutput->append(tr("Loaded discovery results for %1 ECU(s) from %2.")
                            .arg(loaded.size()).arg(fileName));
    finishSupportedPidScan();
}

QString OBD2WorkbenchWindow::decodeObdDtcs(const QByteArray &data) const
{
    QStringList codes;
    const char systems[] = {'P', 'C', 'B', 'U'};
    for (int i = 0; i + 1 < data.size(); i += 2)
    {
        const int a = quint8(data[i]), b = quint8(data[i + 1]);
        if (a == 0 && b == 0) continue;
        const QString code = QStringLiteral("%1%2%3%4%5").arg(systems[(a >> 6) & 3]).arg((a >> 4) & 3)
            .arg(a & 0xF, 1, 16).arg((b >> 4) & 0xF, 1, 16).arg(b & 0xF, 1, 16).toUpper();
        const QString description = dtcDescriptions.value(code);
        codes << (description.isEmpty() ? code : code + QStringLiteral(" - ") + description);
    }
    return codes.isEmpty() ? tr("No DTCs reported") : codes.join(", ");
}

void OBD2WorkbenchWindow::loadDtcDatabase()
{
    const QString fileName = QFileDialog::getOpenFileName(this, tr("Load DTC description database"), QString(),
                                                           tr("DTC databases (*.json)"));
    if (fileName.isEmpty()) return;
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) { dtcDatabaseStatus->setText(tr("Could not open database")); return; }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject() || !document.object()["codes"].isObject())
    {
        dtcDatabaseStatus->setText(tr("Invalid database: expected a codes object"));
        return;
    }
    const QJsonObject root = document.object();
    dtcDescriptions.clear();
    const QJsonObject codes = root["codes"].toObject();
    for (auto it = codes.constBegin(); it != codes.constEnd(); ++it)
        dtcDescriptions.insert(it.key().trimmed().toUpper(), it.value().toString());
    dtcDatabaseSource = root["source"].toString(tr("Unspecified source"));
    dtcDatabaseVersion = root["version"].toVariant().toString();
    dtcDatabaseStatus->setText(tr("%1 codes; %2; version %3")
        .arg(dtcDescriptions.size()).arg(dtcDatabaseSource, dtcDatabaseVersion));
}

void OBD2WorkbenchWindow::exportDiagnosticReport()
{
    QString fileName = QFileDialog::getSaveFileName(this, tr("Export diagnostic report"), QString(),
                                                     tr("Text reports (*.txt)"));
    if (fileName.isEmpty()) return;
    if (!fileName.endsWith(QStringLiteral(".txt"), Qt::CaseInsensitive)) fileName += QStringLiteral(".txt");
    QSaveFile file(fileName);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    QTextStream out(&file);
    out << "SavvyCAN OBD-II diagnostic report\n"
        << "Generated: " << QDateTime::currentDateTime().toString(Qt::ISODateWithMs) << '\n'
        << "Bus: " << busSpin->value() << "  Request ID: " << requestIdEdit->currentText() << '\n'
        << "DTC database: " << dtcDatabaseSource << " " << dtcDatabaseVersion << "\n\nLive data\n";
    for (int row = 0; row < pidTable->rowCount(); ++row)
        out << pidTable->item(row, PidName)->text() << " (" << pidText(row) << "): "
            << pidTable->item(row, PidResponses)->text() << '\n';
    out << "\nDTCs\n" << dtcOutput->toPlainText()
        << "\n\nFreeze frames\n" << freezeOutput->toPlainText()
        << "\n\nMonitor tests\n" << monitorOutput->toPlainText()
        << "\n\nVehicle information\n" << vehicleOutput->toPlainText() << '\n';
    if (!file.commit()) eventLog->append(tr("Could not save diagnostic report"));
}

void OBD2WorkbenchWindow::toggleTripRecording()
{
    if (tripLogFile)
    {
        tripLogFile->close();
        delete tripLogFile;
        tripLogFile = nullptr;
        tripRecordButton->setText(tr("Start trip log"));
        return;
    }
    const QString fileName = QFileDialog::getSaveFileName(this, tr("Record diagnostic trip"), QString(),
                                                           tr("CSV files (*.csv)"));
    if (fileName.isEmpty()) return;
    tripLogFile = new QFile(fileName, this);
    if (!tripLogFile->open(QIODevice::WriteOnly | QIODevice::Text))
    {
        delete tripLogFile;
        tripLogFile = nullptr;
        return;
    }
    QTextStream(tripLogFile) << "timestamp,bus,ecu,pid,name,raw,decoded\n";
    tripRecordButton->setText(tr("Stop trip log"));
}

void OBD2WorkbenchWindow::loadTripPlayback()
{
    const QString fileName = QFileDialog::getOpenFileName(this, tr("Load diagnostic trip"), QString(),
                                                           tr("CSV files (*.csv)"));
    if (fileName.isEmpty()) return;
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    tripPlaybackTimer.stop();
    tripPlaybackButton->setText(tr("Play"));
    tripPlaybackTable->setRowCount(0);
    tripPlaybackTimes.clear();
    QTextStream stream(&file);
    if (!stream.atEnd()) stream.readLine();
    qint64 firstTimestamp = -1;
    while (!stream.atEnd())
    {
        const QStringList fields = parseCsvRow(stream.readLine());
        if (fields.size() < 7) continue;
        const qint64 timestamp = QDateTime::fromString(fields.at(0), Qt::ISODateWithMs).toMSecsSinceEpoch();
        if (timestamp < 0) continue;
        if (firstTimestamp < 0) firstTimestamp = timestamp;
        const int row = tripPlaybackTable->rowCount();
        tripPlaybackTable->insertRow(row);
        for (int column = 0; column < 7; ++column)
            tripPlaybackTable->setItem(row, column, new QTableWidgetItem(fields.at(column)));
        tripPlaybackTimes.append(timestamp - firstTimestamp);
    }
    tripPlaybackSlider->setRange(0, qMax(0, tripPlaybackTimes.size() - 1));
    tripPlaybackSlider->setValue(0);
    tripPlaybackButton->setEnabled(!tripPlaybackTimes.isEmpty());
    if (!tripPlaybackTimes.isEmpty()) setTripPlaybackRow(0);
}

void OBD2WorkbenchWindow::toggleTripPlayback()
{
    if (tripPlaybackTimer.isActive())
    {
        tripPlaybackTimer.stop();
        tripPlaybackButton->setText(tr("Play"));
        return;
    }
    if (tripPlaybackSlider->value() >= tripPlaybackSlider->maximum())
        tripPlaybackSlider->setValue(0);
    tripPlaybackButton->setText(tr("Pause"));
    advanceTripPlayback();
}

void OBD2WorkbenchWindow::advanceTripPlayback()
{
    const int current = tripPlaybackSlider->value();
    if (current >= tripPlaybackSlider->maximum())
    {
        tripPlaybackButton->setText(tr("Play"));
        return;
    }
    const int next = current + 1;
    const qint64 delay = qBound<qint64>(10, tripPlaybackTimes.at(next) - tripPlaybackTimes.at(current), 2000);
    tripPlaybackSlider->setValue(next);
    tripPlaybackTimer.start(int(delay));
}

void OBD2WorkbenchWindow::setTripPlaybackRow(int row)
{
    if (row < 0 || row >= tripPlaybackTable->rowCount() || row >= tripPlaybackTimes.size()) return;
    tripPlaybackTable->selectRow(row);
    tripPlaybackTable->scrollToItem(tripPlaybackTable->item(row, 0), QAbstractItemView::PositionAtCenter);
    emit tripPlaybackPositionChanged(tripPlaybackTimes.at(row));
}

QString OBD2WorkbenchWindow::decodeVehicleInfo(int pid, const QByteArray &data) const
{
    QByteArray value = data;
    if (!value.isEmpty()) value.remove(0, 1); // record count or sequence byte
    if (pid == 0x02 || pid == 0x04 || pid == 0x0A)
        return QString::fromLatin1(value).remove(QChar('\0')).trimmed();
    if (pid == 0x01 || pid == 0x03 || pid == 0x05 || pid == 0x07 || pid == 0x09)
        return data.isEmpty() ? tr("No message count returned")
                              : tr("%1 message(s)").arg(quint8(data.at(data.size() - 1)));
    if (pid == 0x06)
    {
        QStringList values;
        for (int offset = 0; offset + 3 < value.size(); offset += 4)
        {
            const uint32_t cvn = (quint8(value[offset]) << 24) | (quint8(value[offset + 1]) << 16) |
                                 (quint8(value[offset + 2]) << 8) | quint8(value[offset + 3]);
            values << QStringLiteral("0x%1").arg(cvn, 8, 16, QLatin1Char('0')).toUpper();
        }
        if (!values.isEmpty()) return values.join(QStringLiteral(", "));
    }
    return QString::fromLatin1(data.toHex(' ').toUpper());
}

void OBD2WorkbenchWindow::requestFreezeFrame()
{
    bool ok = false;
    const uint32_t pid = parseNumber(freezePidEdit->text(), &ok);
    if (!ok || pid > 0xFF) { freezeOutput->setText(tr("Invalid PID")); return; }
    activePid = int(pid);
    freezeOutput->clear();
    QByteArray request;
    request.append(char(pid));
    request.append(char(freezeFrameSpin->value()));
    sendRequest(2, request, FreezeFrame);
}

void OBD2WorkbenchWindow::scanFreezeFramePids()
{
    QByteArray request;
    request.append(char(0x00));
    request.append(char(freezeFrameSpin->value()));
    freezeOutput->clear();
    activePid = 0x00;
    sendRequest(2, request, FreezeSupportScan);
}

void OBD2WorkbenchWindow::requestMonitorResults()
{
    bool ok = false;
    const uint32_t mid = parseNumber(monitorMidEdit->text(), &ok);
    if (!ok || mid > 0xFF) { monitorOutput->setText(tr("Invalid monitor ID")); return; }
    activePid = int(mid);
    monitorOutput->clear();
    sendRequest(6, QByteArray(1, char(mid)), MonitorResults);
}

void OBD2WorkbenchWindow::loadMode06Scalings()
{
    const QString fileName = QFileDialog::getOpenFileName(this, tr("Load Mode 06 UAS table"), QString(),
                                                           tr("Mode 06 UAS tables (*.json)"));
    if (fileName.isEmpty()) return;
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) { monitorScalingStatus->setText(tr("Could not open UAS table")); return; }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject() || !document.object()["units"].isObject())
    {
        monitorScalingStatus->setText(tr("Invalid UAS table: expected a units object"));
        return;
    }
    monitorScalings.clear();
    const QJsonObject root = document.object();
    const QJsonObject units = root["units"].toObject();
    for (auto it = units.constBegin(); it != units.constEnd(); ++it)
    {
        bool ok = false;
        QString key = it.key();
        if (key.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) key.remove(0, 2);
        const int uas = key.toInt(&ok, 16);
        if (!ok || uas < 0 || uas > 0xFF || !it.value().isObject()) continue;
        const QJsonObject item = it.value().toObject();
        MonitorScaling scaling;
        scaling.factor = item["factor"].toDouble(1.0);
        scaling.offset = item["offset"].toDouble(0.0);
        scaling.unit = item["unit"].toString();
        scaling.signedValue = item["signed"].toBool(false);
        monitorScalings.insert(uas, scaling);
    }
    monitorScalingStatus->setText(tr("%1 UAS definitions; %2; version %3")
        .arg(monitorScalings.size()).arg(root["source"].toString(tr("Unspecified source")),
                                        root["version"].toVariant().toString()));
}

QString OBD2WorkbenchWindow::decodeMonitorResults(const QByteArray &data) const
{
    if (data.size() < 8) return tr("Short or unsupported monitor response: %1")
        .arg(QString::fromLatin1(data.toHex(' ').toUpper()));
    QStringList lines;
    for (int offset = 0; offset + 7 < data.size(); offset += 8)
    {
        const int tid = quint8(data[offset]);
        const int uas = quint8(data[offset + 1]);
        int value = (quint8(data[offset + 2]) << 8) | quint8(data[offset + 3]);
        int minimum = (quint8(data[offset + 4]) << 8) | quint8(data[offset + 5]);
        int maximum = (quint8(data[offset + 6]) << 8) | quint8(data[offset + 7]);
        QString valueText = QString::number(value);
        QString minimumText = QString::number(minimum);
        QString maximumText = QString::number(maximum);
        if (monitorScalings.contains(uas))
        {
            const MonitorScaling scaling = monitorScalings.value(uas);
            if (scaling.signedValue)
            {
                value = qint16(value);
                minimum = qint16(minimum);
                maximum = qint16(maximum);
            }
            const auto scaled = [&scaling](int raw) { return raw * scaling.factor + scaling.offset; };
            valueText = QStringLiteral("%1 %2").arg(scaled(value), 0, 'g', 8).arg(scaling.unit);
            minimumText = QStringLiteral("%1 %2").arg(scaled(minimum), 0, 'g', 8).arg(scaling.unit);
            maximumText = QStringLiteral("%1 %2").arg(scaled(maximum), 0, 'g', 8).arg(scaling.unit);
        }
        lines << tr("TID 0x%1  UAS 0x%2  value %3  limits %4..%5  %6")
            .arg(tid, 2, 16, QLatin1Char('0')).arg(uas, 2, 16, QLatin1Char('0'))
            .arg(valueText, minimumText, maximumText)
            .arg(value >= minimum && value <= maximum ? tr("PASS") : tr("FAIL")).toUpper();
    }
    return lines.join('\n');
}

void OBD2WorkbenchWindow::gotReply(UDS_MESSAGE message)
{
    if (message.bus != busSpin->value() || !responseMatches(message.frameId())) return;
    const QString ecu = QStringLiteral("0x%1").arg(message.frameId(), 3, 16, QLatin1Char('0')).toUpper();
    if (message.isErrorReply && message.service == activeMode)
    {
        const int responseCode = message.subFunc;
        const QString detail = handler->getNegativeResponseLong(responseCode);
        if (responseCode == 0x78)
        {
            connectionStatus->setText(tr("%1: response pending").arg(ecu));
            responseTimer.start(5000);
            if (context == VehicleInfo)
                vehicleOutput->setPlainText(tr("%1: response pending...").arg(ecu));
            return;
        }

        const QString error = tr("%1: negative response 0x%2 - %3")
            .arg(ecu).arg(responseCode, 2, 16, QLatin1Char('0')).arg(detail).toUpper();
        connectionStatus->setText(error);
        eventLog->append(error);
        responseSettleTimer.start();
        if (context == LivePid && activePidRow >= 0 && activePidRow < pidTable->rowCount())
        {
            activePidHadResponse = true;
            pidTable->item(activePidRow, PidRaw)->setText(
                ecu + QStringLiteral(": 7F ") +
                QStringLiteral("%1 %2").arg(activeMode, 2, 16, QLatin1Char('0'))
                                         .arg(responseCode, 2, 16, QLatin1Char('0')).toUpper());
            pidTable->item(activePidRow, PidResponses)->setText(error);
            pidTable->item(activePidRow, PidUpdated)->setText(
                QDateTime::currentDateTime().toString("HH:mm:ss.zzz"));
        }
        else if (context == VehicleInfo)
            vehicleOutput->append(error);
        else if (context == StoredDtc || context == PendingDtc || context == PermanentDtc || context == ClearDtc)
            dtcOutput->append(error);
        else if (context == FreezeFrame || context == FreezeSupportScan)
            freezeOutput->append(error);
        else if (context == MonitorResults)
            monitorOutput->append(error);
        else if (context == ModuleScan || context == SupportedPidScan)
            discoveryOutput->append(error);
        return;
    }
    if (message.service != activeMode + 0x40) return;
    connectionStatus->setText(tr("ECU responding"));
    QByteArray payload = message.payload();
    if (context == LivePid)
    {
        if (payload.isEmpty() || quint8(payload[0]) != activePid) return;
        payload.remove(0, 1);
        QString current;
        if (activePidHadResponse)
            current = pidTable->item(activePidRow, PidResponses)->text();
        activePidHadResponse = true;
        pidTable->item(activePidRow, PidRaw)->setText(
            ecu + QStringLiteral(": ") + QString::fromLatin1(payload.toHex(' ').toUpper()));
        if (!current.isEmpty()) current += QStringLiteral(" | ");
        const QString rowFormat = pidTable->item(activePidRow, PidFormat)->text().trimmed();
        QString decoded;
        QVector<QPair<QString, double>> numericValues;
        const bool automatic = rowFormat.isEmpty() || rowFormat.compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0;
        QString effectiveFormat = rowFormat;
        if (automatic)
            effectiveFormat = knownPidFormat(activePid);
        if (automatic)
            decoded = decodeCompoundPid(activePid, payload);
        if (decoded.isEmpty() && effectiveFormat.isEmpty())
            decoded = QString::fromLatin1(payload.toHex(' ').toUpper());
        else if (decoded.isEmpty())
        {
            PayloadFormatter formatter;
            QString error;
            if (formatter.compile(effectiveFormat, &error))
            {
                decoded = formatter.format(payload);
                const QString prefix = pidTable->item(activePidRow, PidName)->text() + QStringLiteral("/") + ecu;
                for (const PayloadFormatter::FormattedField &field : formatter.formatFields(payload))
                {
                    bool numeric = false;
                    const double value = field.value.toDouble(&numeric);
                    if (!numeric) continue;
                    const QString sampleName = field.unit.isEmpty() ? field.name
                        : field.name + QStringLiteral(" [") + field.unit + QLatin1Char(']');
                    numericValues.append(qMakePair(sampleName, value));
                    if (diagnosticGraph) diagnosticGraph->addSample(prefix + QStringLiteral("/") + field.name, value);
                }
            }
            else decoded = error;
        }
        pidTable->item(activePidRow, PidResponses)->setText(current + ecu + QStringLiteral(": ") + decoded);
        pidTable->item(activePidRow, PidUpdated)->setText(QDateTime::currentDateTime().toString("HH:mm:ss.zzz"));
        updateDashboard(activePid, ecu, decoded, numericValues);
        widgetDashboardCanvas->updatePid(activePid, payload, ecu + QStringLiteral(": ") + decoded);
        if (tripLogFile)
        {
            auto quote = [](QString value) { value.replace('"', QStringLiteral("\"\"")); return '"' + value + '"'; };
            QTextStream out(tripLogFile);
            out << quote(QDateTime::currentDateTime().toString(Qt::ISODateWithMs)) << ','
                << busSpin->value() << ',' << quote(ecu) << ','
                << QStringLiteral("0x%1").arg(activePid, 2, 16, QLatin1Char('0')).toUpper() << ','
                << quote(pidTable->item(activePidRow, PidName)->text()) << ','
                << quote(QString::fromLatin1(payload.toHex(' ').toUpper())) << ',' << quote(decoded) << '\n';
            tripLogFile->flush();
        }
    }
    else if (context == StoredDtc || context == PendingDtc || context == PermanentDtc)
        dtcOutput->append(ecu + QStringLiteral(": ") + decodeObdDtcs(payload));
    else if (context == ClearDtc) dtcOutput->append(ecu + tr(": DTCs cleared"));
    else if (context == VehicleInfo && !payload.isEmpty() && quint8(payload[0]) == activePid)
    {
        payload.remove(0, 1);
        if (vehicleOutput->toPlainText().contains(tr("response pending...")))
            vehicleOutput->clear();
        vehicleOutput->append(ecu + QStringLiteral(": ") + decodeVehicleInfo(activePid, payload));
    }
    else if (context == FreezeFrame && payload.size() >= 2 && quint8(payload[0]) == activePid)
    {
        const int frame = quint8(payload[1]);
        payload.remove(0, 2);
        QString decoded = decodeCompoundPid(activePid, payload);
        const QString format = knownPidFormat(activePid);
        if (decoded.isEmpty() && !format.isEmpty())
        {
            PayloadFormatter formatter;
            QString error;
            decoded = formatter.compile(format, &error) ? formatter.format(payload) : error;
        }
        if (decoded.isEmpty()) decoded = QString::fromLatin1(payload.toHex(' ').toUpper());
        freezeOutput->append(tr("%1 frame %2: %3").arg(ecu).arg(frame).arg(decoded));
    }
    else if (context == FreezeSupportScan && payload.size() >= 6 && quint8(payload[0]) == 0x00)
    {
        const int frame = quint8(payload[1]);
        QSet<int> supported;
        const QString pids = supportedPidText(payload.mid(2), 0x00, &supported);
        freezeOutput->append(tr("%1 frame %2 supports: %3").arg(ecu).arg(frame).arg(pids));
    }
    else if (context == MonitorResults && !payload.isEmpty() && quint8(payload[0]) == activePid)
    {
        payload.remove(0, 1);
        monitorOutput->append(ecu + QStringLiteral(":\n") + decodeMonitorResults(payload));
    }
    else if ((context == ModuleScan || context == SupportedPidScan) && !payload.isEmpty() && quint8(payload[0]) == activePid)
    {
        payload.remove(0, 1);
        QSet<int> discovered;
        const QString supported = supportedPidText(payload, activePid, &discovered);
        supportedPidsByEcu[message.frameId()].unite(discovered);
        discoveryOutput->append(tr("%1 page 0x%2: %3").arg(ecu)
            .arg(activePid, 2, 16, QLatin1Char('0')).arg(supported).toUpper());
    }
    responseSettleTimer.start();
}

void OBD2WorkbenchWindow::showDiagnosticGraph()
{
    if (!diagnosticGraph) diagnosticGraph = new DiagnosticGraphWindow(this);
    diagnosticGraph->show();
    diagnosticGraph->raise();
}

void OBD2WorkbenchWindow::requestFinished()
{
    responseTimer.stop();
    responseSettleTimer.stop();
    const Context finishedContext = context;
    context = None;
    activeMode = -1;
    if (finishedContext == ModuleScan)
    {
        refreshEcuTargets();
        refreshDiscoveryPidList();
    }
    if (finishedContext == SupportedPidScan)
    {
        if (scanPidBases.isEmpty()) { finishSupportedPidScan(); return; }
        activePid = scanPidBases.takeFirst();
        sendRequest(1, QByteArray(1, char(activePid)), SupportedPidScan);
        return;
    }
    if (pidQueue.isEmpty())
    {
        activePidRow = -1;
        if (pollCheck->isChecked() && connected)
        {
            const qint64 elapsed = pollCycleActive
                ? QDateTime::currentMSecsSinceEpoch() - pollCycleStartedMs : 0;
            pollTimer.start(qMax(0, pollIntervalSpin->value() - int(elapsed)));
        }
        pollCycleActive = false;
        return;
    }
    activePidRow = pidQueue.takeFirst();
    bool ok = false;
    activePid = parseNumber(pidText(activePidRow), &ok);
    if (!ok || activePid > 0xFF) { pidTable->item(activePidRow, PidResponses)->setText(tr("Invalid PID")); requestFinished(); return; }
    activePidHadResponse = false;
    pidTable->item(activePidRow, PidRaw)->setText(QString());
    pidTable->item(activePidRow, PidResponses)->setText(tr("Waiting for response..."));
    sendRequest(1, QByteArray(1, char(activePid)), LivePid);
}

void OBD2WorkbenchWindow::loadSettings()
{
    QSettings settings;
    busSpin->setValue(settings.value("OBD2Workbench/Bus", 0).toInt());
    requestIdEdit->setCurrentText(settings.value("OBD2Workbench/RequestId", "0x7DF").toString());
    safetyModeCombo->setCurrentIndex(qMax(0, safetyModeCombo->findData(
        settings.value("OBD2Workbench/SafetyMode", "read").toString())));
    if (settings.contains("OBD2Workbench/ResponseMode"))
    {
        const int modeIndex = responseModeCombo->findData(settings.value("OBD2Workbench/ResponseMode").toString());
        responseModeCombo->setCurrentIndex(qMax(0, modeIndex));
        responseIdEdit->setText(settings.value("OBD2Workbench/ResponseSpec", "0x7E8-0x7EF").toString());
    }
    else if (settings.contains("OBD2Workbench/ResponseId"))
    {
        responseModeCombo->setCurrentIndex(responseModeCombo->findData(QStringLiteral("mask")));
        responseIdEdit->setText(settings.value("OBD2Workbench/ResponseId", "0x7E8").toString());
    }
    else
    {
        responseModeCombo->setCurrentIndex(responseModeCombo->findData(QStringLiteral("list")));
        responseIdEdit->setText(QStringLiteral("0x7E8-0x7EF"));
    }
    responseMaskEdit->setText(settings.value("OBD2Workbench/ResponseMask", "0x7F8").toString());
    pollIntervalSpin->setValue(settings.value("OBD2Workbench/PollInterval", 1000).toInt());
    liveRequestsCheck->setChecked(settings.value("OBD2Workbench/LiveRequestSource", true).toBool());
    dashboardRequestsCheck->setChecked(settings.value("OBD2Workbench/DashboardRequestSource", true).toBool());
    const QJsonArray rows = QJsonDocument::fromJson(settings.value("OBD2Workbench/Pids").toByteArray()).array();
    loadPidRows(rows);
    dashboardColumnsSpin->setValue(settings.value("OBD2Workbench/DashboardColumns", 12).toInt());
    QByteArray dashboardData = settings.value("OBD2Workbench/DashboardWidgets").toByteArray();
    if (dashboardData.isEmpty()) dashboardData = settings.value("OBD2Workbench/DashboardPids").toByteArray();
    const QJsonArray dashboard = QJsonDocument::fromJson(dashboardData).array();
    loadDashboardJson(dashboard);
    if (pidTable->rowCount() == 0)
    {
        const QList<QPair<QString, QString>> defaults = {{tr("Engine RPM"), "0x0C"}, {tr("Vehicle speed"), "0x0D"},
            {tr("Coolant temperature"), "0x05"}, {tr("Throttle position"), "0x11"}};
        for (const auto &entry : defaults) { addPid(); const int row = pidTable->rowCount() - 1;
            pidTable->item(row, PidName)->setText(entry.first); setPidText(row, entry.second);
            bool ok = false; const int pid = parseNumber(entry.second, &ok);
            if (ok) pidTable->item(row, PidFormat)->setText(knownPidFormat(pid)); }
    }
}

void OBD2WorkbenchWindow::loadPidRows(const QJsonArray &rows)
{
    pidTable->setRowCount(0);
    for (const QJsonValue &value : rows)
    {
        addPid();
        const int row = pidTable->rowCount() - 1;
        const QJsonObject item = value.toObject();
        pidTable->item(row, PidEnabled)->setCheckState(item["enabled"].toBool(true) ? Qt::Checked : Qt::Unchecked);
        pidTable->item(row, PidName)->setText(item["name"].toString());
        setPidText(row, item["pid"].toString());
        pidTable->item(row, PidFormat)->setText(item["format"].toString("auto"));
        pidTable->item(row, PidEnabled)->setData(
            DashboardManagedRole, item["dashboardManaged"].toBool(false));
    }
}

void OBD2WorkbenchWindow::saveSettings() const
{
    QSettings settings;
    settings.setValue("OBD2Workbench/Bus", busSpin->value());
    settings.setValue("OBD2Workbench/RequestId", requestIdEdit->currentText());
    settings.setValue("OBD2Workbench/SafetyMode", safetyModeCombo->currentData().toString());
    settings.setValue("OBD2Workbench/ResponseMode", responseModeCombo->currentData().toString());
    settings.setValue("OBD2Workbench/ResponseSpec", responseIdEdit->text());
    settings.setValue("OBD2Workbench/ResponseMask", responseMaskEdit->text());
    settings.setValue("OBD2Workbench/PollInterval", pollIntervalSpin->value());
    settings.setValue("OBD2Workbench/LiveRequestSource", liveRequestsCheck->isChecked());
    settings.setValue("OBD2Workbench/DashboardRequestSource", dashboardRequestsCheck->isChecked());
    settings.setValue("OBD2Workbench/Pids", QJsonDocument(pidRowsToJson()).toJson(QJsonDocument::Compact));
    settings.setValue("OBD2Workbench/DashboardColumns", dashboardColumnsSpin->value());
    settings.setValue("OBD2Workbench/DashboardPids",
                      QJsonDocument(dashboardToJson()).toJson(QJsonDocument::Compact));
    settings.setValue("OBD2Workbench/DashboardWidgets",
                      QJsonDocument(dashboardToJson()).toJson(QJsonDocument::Compact));
}

QJsonArray OBD2WorkbenchWindow::pidRowsToJson() const
{
    QJsonArray rows;
    for (int row = 0; row < pidTable->rowCount(); ++row)
    {
        QJsonObject item;
        item["enabled"] = pidTable->item(row, PidEnabled)->checkState() == Qt::Checked;
        item["name"] = pidTable->item(row, PidName)->text();
        item["pid"] = pidText(row);
        item["format"] = pidTable->item(row, PidFormat)->text();
        item["dashboardManaged"] =
            pidTable->item(row, PidEnabled)->data(DashboardManagedRole).toBool();
        rows.append(item);
    }
    return rows;
}
