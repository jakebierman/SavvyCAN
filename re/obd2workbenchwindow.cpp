#include "obd2workbenchwindow.h"

#include "connections/canconmanager.h"
#include "payloadformatter.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSaveFile>
#include <QSpinBox>
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QVBoxLayout>

#include <algorithm>

namespace {
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
}

OBD2WorkbenchWindow::OBD2WorkbenchWindow(QWidget *parent) : QDialog(parent), handler(new UDS_HANDLER)
{
    buildUi();
    loadSettings();
    handler->setProcessAllIDs(true);
    responseTimer.setSingleShot(true);
    responseTimer.setInterval(900);
    connect(handler, &UDS_HANDLER::newUDSMessage, this, &OBD2WorkbenchWindow::gotReply);
    connect(&responseTimer, &QTimer::timeout, this, &OBD2WorkbenchWindow::requestFinished);
    connect(&pollTimer, &QTimer::timeout, this, &OBD2WorkbenchWindow::pollPids);
}

OBD2WorkbenchWindow::~OBD2WorkbenchWindow()
{
    saveSettings();
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
    requestIdEdit = new QLineEdit(QStringLiteral("0x7DF"), endpoint);
    QPushButton *connectButton = new QPushButton(tr("Connect"), endpoint);
    connectionStatus = new QLabel(tr("Disconnected"), endpoint);
    endpointLayout->addWidget(new QLabel(tr("Bus"), endpoint));
    endpointLayout->addWidget(busSpin);
    endpointLayout->addWidget(new QLabel(tr("Request ID"), endpoint));
    endpointLayout->addWidget(requestIdEdit);
    endpointLayout->addWidget(connectButton);
    endpointLayout->addWidget(new QLabel(tr("Responses 0x7E8-0x7EF"), endpoint));
    endpointLayout->addStretch();
    endpointLayout->addWidget(connectionStatus);
    root->addWidget(endpoint);
    connect(connectButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::connectEndpoint);

    QTabWidget *tabs = new QTabWidget(this);
    QWidget *livePage = new QWidget(tabs);
    QVBoxLayout *liveLayout = new QVBoxLayout(livePage);
    pidTable = new QTableWidget(0, PidColumnCount, livePage);
    pidTable->setHorizontalHeaderLabels({tr("Use"), tr("Name"), tr("PID lookup / custom"), tr("Payload format"),
                                         tr("ECU responses"), tr("Updated")});
    pidTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    pidTable->horizontalHeader()->setSectionResizeMode(PidResponses, QHeaderView::Stretch);
    pidTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    liveLayout->addWidget(pidTable);
    QHBoxLayout *liveButtons = new QHBoxLayout;
    QPushButton *addButton = new QPushButton(tr("Add PID"), livePage);
    QPushButton *removeButton = new QPushButton(tr("Remove"), livePage);
    QPushButton *loadButton = new QPushButton(tr("Load PID list"), livePage);
    QPushButton *saveButton = new QPushButton(tr("Save PID list"), livePage);
    QPushButton *selectedButton = new QPushButton(tr("Request selected"), livePage);
    QPushButton *enabledButton = new QPushButton(tr("Request enabled"), livePage);
    pollCheck = new QCheckBox(tr("Poll"), livePage);
    pollIntervalSpin = new QSpinBox(livePage);
    pollIntervalSpin->setRange(200, 60000);
    pollIntervalSpin->setValue(1000);
    pollIntervalSpin->setSuffix(tr(" ms"));
    liveButtons->addWidget(addButton);
    liveButtons->addWidget(removeButton);
    liveButtons->addWidget(loadButton);
    liveButtons->addWidget(saveButton);
    liveButtons->addStretch();
    liveButtons->addWidget(pollCheck);
    liveButtons->addWidget(pollIntervalSpin);
    liveButtons->addWidget(selectedButton);
    liveButtons->addWidget(enabledButton);
    liveLayout->addLayout(liveButtons);
    connect(addButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::addPid);
    connect(removeButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::removePid);
    connect(loadButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::loadPidList);
    connect(saveButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::savePidList);
    connect(selectedButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::requestSelectedPid);
    connect(enabledButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::requestEnabledPids);
    connect(pollCheck, &QCheckBox::toggled, this, [this](bool enabled) {
        if (enabled) pollTimer.start(pollIntervalSpin->value()); else pollTimer.stop();
    });
    connect(pollIntervalSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        if (pollTimer.isActive()) pollTimer.start(value);
    });
    tabs->addTab(livePage, tr("Live data"));

    QWidget *discoveryPage = new QWidget(tabs);
    QVBoxLayout *discoveryLayout = new QVBoxLayout(discoveryPage);
    QHBoxLayout *discoveryButtons = new QHBoxLayout;
    QPushButton *moduleScanButton = new QPushButton(tr("Scan modules"), discoveryPage);
    QPushButton *pidScanButton = new QPushButton(tr("Scan available PIDs"), discoveryPage);
    QPushButton *addScannedButton = new QPushButton(tr("Add selected to Live data"), discoveryPage);
    discoveryButtons->addWidget(moduleScanButton);
    discoveryButtons->addWidget(pidScanButton);
    discoveryButtons->addStretch();
    discoveryButtons->addWidget(addScannedButton);
    discoveryLayout->addLayout(discoveryButtons);
    discoveryOutput = new QTextEdit(discoveryPage);
    discoveryOutput->setReadOnly(true);
    discoveryOutput->setMaximumHeight(180);
    discoveryLayout->addWidget(discoveryOutput);
    discoveryPidList = new QListWidget(discoveryPage);
    discoveryPidList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    discoveryLayout->addWidget(discoveryPidList, 1);
    connect(moduleScanButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::scanModules);
    connect(pidScanButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::scanSupportedPids);
    connect(addScannedButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::addSelectedScannedPids);
    tabs->addTab(discoveryPage, tr("Discovery"));

    QWidget *dtcPage = new QWidget(tabs);
    QVBoxLayout *dtcLayout = new QVBoxLayout(dtcPage);
    QHBoxLayout *dtcButtons = new QHBoxLayout;
    QPushButton *storedButton = new QPushButton(tr("Stored (03)"), dtcPage);
    QPushButton *pendingButton = new QPushButton(tr("Pending (07)"), dtcPage);
    QPushButton *permanentButton = new QPushButton(tr("Permanent (0A)"), dtcPage);
    QPushButton *clearButton = new QPushButton(tr("Clear DTCs (04)"), dtcPage);
    dtcButtons->addWidget(storedButton);
    dtcButtons->addWidget(pendingButton);
    dtcButtons->addWidget(permanentButton);
    dtcButtons->addStretch();
    dtcButtons->addWidget(clearButton);
    dtcLayout->addLayout(dtcButtons);
    dtcOutput = new QTextEdit(dtcPage);
    dtcOutput->setReadOnly(true);
    dtcLayout->addWidget(dtcOutput);
    connect(storedButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::readStoredDtcs);
    connect(pendingButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::readPendingDtcs);
    connect(permanentButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::readPermanentDtcs);
    connect(clearButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::clearDtcs);
    tabs->addTab(dtcPage, tr("DTCs"));

    QWidget *infoPage = new QWidget(tabs);
    QVBoxLayout *infoLayout = new QVBoxLayout(infoPage);
    QHBoxLayout *infoControls = new QHBoxLayout;
    vehiclePidEdit = new QLineEdit(QStringLiteral("0x02"), infoPage);
    QPushButton *infoButton = new QPushButton(tr("Request Mode 09"), infoPage);
    infoControls->addWidget(new QLabel(tr("Information PID"), infoPage));
    infoControls->addWidget(vehiclePidEdit);
    infoControls->addWidget(infoButton);
    infoControls->addStretch();
    infoLayout->addLayout(infoControls);
    vehicleOutput = new QTextEdit(infoPage);
    vehicleOutput->setReadOnly(true);
    infoLayout->addWidget(vehicleOutput);
    connect(infoButton, &QPushButton::clicked, this, &OBD2WorkbenchWindow::requestVehicleInfo);
    tabs->addTab(infoPage, tr("Vehicle information"));
    root->addWidget(tabs, 1);

    eventLog = new QTextEdit(this);
    eventLog->setReadOnly(true);
    eventLog->setMaximumHeight(120);
    root->addWidget(eventLog);
}

uint32_t OBD2WorkbenchWindow::parseNumber(const QString &text, bool *ok) const
{
    QString value = text.trimmed();
    int base = 10;
    if (value.startsWith("0x", Qt::CaseInsensitive)) { value.remove(0, 2); base = 16; }
    return value.toUInt(ok, base);
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

void OBD2WorkbenchWindow::connectEndpoint()
{
    bool ok = false;
    const uint32_t requestId = parseNumber(requestIdEdit->text(), &ok);
    if (!ok) { connectionStatus->setText(tr("Invalid request ID")); return; }
    handler->clearAllFilters();
    handler->addFilter(busSpin->value(), 0x7E8, 0x7F8);
    handler->setReception(true);
    connected = true;
    connectionStatus->setText(tr("Connected"));
    eventLog->append(tr("Listening for OBD-II ECU responses"));
    Q_UNUSED(requestId)
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

void OBD2WorkbenchWindow::removePid() { if (pidTable->currentRow() >= 0) pidTable->removeRow(pidTable->currentRow()); }

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
    requestIdEdit->setText(profile["requestId"].toString(requestIdEdit->text()));
    loadPidRows(profile["pids"].toArray());
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
    profile["requestId"] = requestIdEdit->text();
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
    pidQueue.clear();
    for (int row = 0; row < pidTable->rowCount(); ++row)
        if (pidTable->item(row, PidEnabled)->checkState() == Qt::Checked) pidQueue.append(row);
    requestFinished();
}

void OBD2WorkbenchWindow::pollPids() { if (connected && !responseTimer.isActive() && pidQueue.isEmpty()) requestEnabledPids(); }

void OBD2WorkbenchWindow::sendRequest(int mode, const QByteArray &data, Context nextContext)
{
    if (!connected || responseTimer.isActive()) { connectionStatus->setText(tr("Connect first or wait for the active request")); return; }
    bool ok = false;
    const uint32_t requestId = parseNumber(requestIdEdit->text(), &ok);
    if (!ok) return;
    UDS_MESSAGE message;
    message.bus = busSpin->value();
    message.setFrameId(requestId);
    message.setExtendedFrameFormat(requestId > 0x7FF);
    message.service = mode;
    message.subFuncLen = 0;
    message.payload() = data;
    activeMode = mode;
    context = nextContext;
    handler->sendUDSFrame(message);
    responseTimer.start();
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
    const uint32_t pid = parseNumber(vehiclePidEdit->text(), &ok);
    if (!ok || pid > 0xFF) { vehicleOutput->setText(tr("Invalid information PID")); return; }
    vehicleOutput->clear();
    activePid = pid;
    sendRequest(9, QByteArray(1, char(pid)), VehicleInfo);
}

void OBD2WorkbenchWindow::scanModules()
{
    discoveryOutput->clear();
    discoveryPidList->clear();
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
    QSet<int> allSupported;
    for (auto iterator = supportedPidsByEcu.constBegin(); iterator != supportedPidsByEcu.constEnd(); ++iterator)
        allSupported.unite(iterator.value());
    QList<int> sortedPids = allSupported.values();
    std::sort(sortedPids.begin(), sortedPids.end());
    for (int pid : sortedPids)
    {
        if (pid % 0x20 == 0) continue;
        QStringList ecus;
        for (auto iterator = supportedPidsByEcu.constBegin(); iterator != supportedPidsByEcu.constEnd(); ++iterator)
            if (iterator.value().contains(pid))
                ecus << QStringLiteral("0x%1").arg(iterator.key(), 3, 16, QLatin1Char('0')).toUpper();
        const QString name = knownPidName(pid);
        QListWidgetItem *item = new QListWidgetItem(
            QStringLiteral("0x%1  %2  [%3]").arg(pid, 2, 16, QLatin1Char('0')).arg(name, ecus.join(", ")),
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

QString OBD2WorkbenchWindow::decodeObdDtcs(const QByteArray &data) const
{
    QStringList codes;
    const char systems[] = {'P', 'C', 'B', 'U'};
    for (int i = 0; i + 1 < data.size(); i += 2)
    {
        const int a = quint8(data[i]), b = quint8(data[i + 1]);
        if (a == 0 && b == 0) continue;
        codes << QStringLiteral("%1%2%3%4%5").arg(systems[(a >> 6) & 3]).arg((a >> 4) & 3)
                 .arg(a & 0xF, 1, 16).arg((b >> 4) & 0xF, 1, 16).arg(b & 0xF, 1, 16).toUpper();
    }
    return codes.isEmpty() ? tr("No DTCs reported") : codes.join(", ");
}

QString OBD2WorkbenchWindow::decodeVehicleInfo(int pid, const QByteArray &data) const
{
    QByteArray value = data;
    if (!value.isEmpty()) value.remove(0, 1); // record count or sequence byte
    if (pid == 0x02 || pid == 0x04 || pid == 0x0A)
        return QString::fromLatin1(value).remove(QChar('\0')).trimmed();
    return QString::fromLatin1(data.toHex(' ').toUpper());
}

void OBD2WorkbenchWindow::gotReply(UDS_MESSAGE message)
{
    if (message.bus != busSpin->value() || message.frameId() < 0x7E8 || message.frameId() > 0x7EF ||
        message.service != activeMode + 0x40) return;
    responseTimer.start();
    QByteArray payload = message.payload();
    const QString ecu = QStringLiteral("0x%1").arg(message.frameId(), 3, 16, QLatin1Char('0')).toUpper();
    if (context == LivePid)
    {
        if (payload.isEmpty() || quint8(payload[0]) != activePid) return;
        payload.remove(0, 1);
        QString current = pidTable->item(activePidRow, PidResponses)->text();
        if (!current.isEmpty()) current += QStringLiteral(" | ");
        const QString rowFormat = pidTable->item(activePidRow, PidFormat)->text().trimmed();
        QString decoded;
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
            decoded = formatter.compile(effectiveFormat, &error) ? formatter.format(payload) : error;
        }
        pidTable->item(activePidRow, PidResponses)->setText(current + ecu + QStringLiteral(": ") + decoded);
        pidTable->item(activePidRow, PidUpdated)->setText(QDateTime::currentDateTime().toString("HH:mm:ss.zzz"));
    }
    else if (context == StoredDtc || context == PendingDtc || context == PermanentDtc)
        dtcOutput->append(ecu + QStringLiteral(": ") + decodeObdDtcs(payload));
    else if (context == ClearDtc) dtcOutput->append(ecu + tr(": DTCs cleared"));
    else if (context == VehicleInfo && !payload.isEmpty() && quint8(payload[0]) == activePid)
    {
        payload.remove(0, 1);
        vehicleOutput->append(ecu + QStringLiteral(": ") + decodeVehicleInfo(activePid, payload));
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
}

void OBD2WorkbenchWindow::requestFinished()
{
    responseTimer.stop();
    const Context finishedContext = context;
    context = None;
    activeMode = -1;
    if (finishedContext == SupportedPidScan)
    {
        if (scanPidBases.isEmpty()) { finishSupportedPidScan(); return; }
        activePid = scanPidBases.takeFirst();
        sendRequest(1, QByteArray(1, char(activePid)), SupportedPidScan);
        return;
    }
    if (pidQueue.isEmpty()) { activePidRow = -1; return; }
    activePidRow = pidQueue.takeFirst();
    bool ok = false;
    activePid = parseNumber(pidText(activePidRow), &ok);
    if (!ok || activePid > 0xFF) { pidTable->item(activePidRow, PidResponses)->setText(tr("Invalid PID")); requestFinished(); return; }
    pidTable->item(activePidRow, PidResponses)->setText(QString());
    sendRequest(1, QByteArray(1, char(activePid)), LivePid);
}

void OBD2WorkbenchWindow::loadSettings()
{
    QSettings settings;
    busSpin->setValue(settings.value("OBD2Workbench/Bus", 0).toInt());
    requestIdEdit->setText(settings.value("OBD2Workbench/RequestId", "0x7DF").toString());
    pollIntervalSpin->setValue(settings.value("OBD2Workbench/PollInterval", 1000).toInt());
    const QJsonArray rows = QJsonDocument::fromJson(settings.value("OBD2Workbench/Pids").toByteArray()).array();
    loadPidRows(rows);
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
    }
}

void OBD2WorkbenchWindow::saveSettings() const
{
    QSettings settings;
    settings.setValue("OBD2Workbench/Bus", busSpin->value());
    settings.setValue("OBD2Workbench/RequestId", requestIdEdit->text());
    settings.setValue("OBD2Workbench/PollInterval", pollIntervalSpin->value());
    settings.setValue("OBD2Workbench/Pids", QJsonDocument(pidRowsToJson()).toJson(QJsonDocument::Compact));
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
        rows.append(item);
    }
    return rows;
}
