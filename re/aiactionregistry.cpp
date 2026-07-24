#include "aiactionregistry.h"

#include <QJsonDocument>

QJsonArray AIActionRegistry::catalog()
{
    static const char json[] = R"JSON([
{"capability":"ui.open","title":"Open a tool","ui":"Application","access":"read","arguments":{"target":"trace|uds|obd|canopen|bus_diagnostics|ai|fuzzing|frame_sender|scripting|sniffer|graphing|playback|dbc|connection|isotp|bridge"}},
{"capability":"uds.add_did","title":"Add a DID request","ui":"UDS Workbench","access":"edit","arguments":{"did":"hex DID","name":"optional","format":"payload format","poll_ms":"integer"}},
{"capability":"uds.read_did","title":"Add a DID request","ui":"UDS Workbench","access":"edit","arguments":{"did":"hex DID","name":"optional","format":"payload format","poll_ms":"integer"}},
{"capability":"obd.add_pid","title":"Add a PID request","ui":"OBD-II Workbench","access":"edit","arguments":{"pid":"hex PID","name":"optional","format":"auto or payload format"}},
{"capability":"obd.query_pid","title":"Add a PID request","ui":"OBD-II Workbench","access":"edit","arguments":{"pid":"hex PID","name":"optional","format":"auto or payload format"}},
{"capability":"obd.clear_pids","title":"Clear all OBD-II PID requests","ui":"OBD-II Workbench","access":"edit","arguments":{}},
{"capability":"canopen.add_object","title":"Add an Object Dictionary entry","ui":"CANopen Workbench","access":"edit","arguments":{"node_id":"1-127","index":"hex index","subindex":"hex/integer","name":"optional","data_type":"optional","access":"ro|rw|wo","value":"optional"}},
{"capability":"fuzz.configure","title":"Configure fuzzing without starting it","ui":"Fuzzing","access":"edit","arguments":{"bus":"integer","start_id":"hex CAN ID","end_id":"hex CAN ID","interval_ms":"integer","burst":"integer","bytes":"1-8"}},
{"capability":"frame.add_draft","title":"Add a disabled frame draft","ui":"Frame Sender","access":"edit","arguments":{"bus":"integer","can_id":"hex CAN ID","extended":"boolean","payload":"space-separated hex bytes"}},
{"capability":"script.create_draft","title":"Create an unsaved script draft","ui":"Scripting","access":"edit","arguments":{"name":"optional","source":"JavaScript source"}},
{"capability":"payload.set_format","title":"Set the main payload display format","ui":"CAN Trace","access":"edit","arguments":{"format":"payload format expression"}},
{"capability":"trace.set_options","title":"Set trace display options","ui":"CAN Trace","access":"edit","arguments":{"overwrite":"optional boolean","interpret_dbc":"optional boolean","show_raw_payload":"optional boolean"}},
{"capability":"filter.set_id","title":"Enable or disable a CAN-ID filter","ui":"CAN Trace","access":"edit","arguments":{"can_id":"hex CAN ID","enabled":"boolean"}},
{"capability":"graph.add","title":"Add a raw bit-field graph","ui":"Graphing","access":"edit","arguments":{"can_id":"hex CAN ID","bus":"integer","start_bit":"integer","bit_length":"1-64","little_endian":"boolean","signed":"boolean","scale":"number","offset":"number","name":"optional"}},
{"capability":"dashboard.add_pid","title":"Add an OBD PID dashboard widget","ui":"OBD-II Workbench","access":"edit","arguments":{"pid":"hex PID"}},
{"capability":"uds.execute","title":"Execute a prepared UDS request","ui":"UDS Workbench","access":"confirm-send","arguments":{"operation":"read_did|scan_dids|session|dtc"}},
{"capability":"obd.execute","title":"Execute a prepared OBD request","ui":"OBD-II Workbench","access":"confirm-send","arguments":{"operation":"query_pid|request_enabled|scan_modules|scan_pids"}},
{"capability":"canopen.execute","title":"Execute a prepared CANopen operation","ui":"CANopen Workbench","access":"confirm-send","arguments":{"operation":"upload|scan_nodes"}},
{"capability":"canopen.write","title":"Write a prepared CANopen object","ui":"CANopen Workbench","access":"armed-confirm-send","arguments":{"node_id":"1-127","index":"hex index","subindex":"hex/integer","value":"bytes"}},
{"capability":"fuzz.start","title":"Start configured fuzzing","ui":"Fuzzing","access":"armed-confirm-send","arguments":{"duration_ms":"100-60000"}},
{"capability":"frame.send","title":"Enable a prepared Frame Sender row","ui":"Frame Sender","access":"armed-confirm-send","arguments":{"row":"integer"}},
{"capability":"frame.send_once","title":"Send one CAN frame now","ui":"Frame Sender","access":"armed-confirm-send","arguments":{"bus":"integer","can_id":"hex CAN ID","extended":"boolean","payload":"space-separated hex bytes"}}
,
{"capability":"frame.send_loop","title":"Send a bounded CAN frame loop","ui":"Frame Sender","access":"armed-confirm-send","arguments":{"bus":"integer","can_id":"hex CAN ID","extended":"boolean","payload":"space-separated hex bytes","count":"1-1000","interval_ms":"1-60000"}}
,
{"capability":"connection.reconnect","title":"Reconnect CAN connection profiles","ui":"Connection Settings","access":"edit","arguments":{"port":"profile port/interface, or all"}},
{"capability":"connection.suspend","title":"Suspend all CAN connections","ui":"Connection Settings","access":"edit","arguments":{}},
{"capability":"connection.resume","title":"Resume all CAN connections","ui":"Connection Settings","access":"edit","arguments":{}},
{"capability":"connection.add","title":"Add and start a CAN connection profile","ui":"Connection Settings","access":"armed-confirm-send","arguments":{"type":"gvret_serial|kvaser|serialbus|remote|kayak|mqtt|lawicel|canserver|canlogserver","port":"device/interface/host","driver":"optional Qt CAN driver","serial_speed":"integer","bus_speed":"integer","can_fd":"boolean","data_rate":"integer"}}
])JSON";
    return QJsonDocument::fromJson(QByteArray(json)).array();
}

QJsonObject AIActionRegistry::definition(const QString &capability)
{
    for (const QJsonValue &value : catalog())
    {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("capability")).toString() == capability)
            return object;
    }
    return QJsonObject();
}

QString AIActionRegistry::catalogText()
{
    return QString::fromUtf8(QJsonDocument(catalog()).toJson(QJsonDocument::Indented));
}

AIActionRegistry::Risk AIActionRegistry::risk(const QJsonObject &definition)
{
    const QString access = definition.value(QStringLiteral("access")).toString();
    if (access == QStringLiteral("edit")) return Edit;
    if (access == QStringLiteral("confirm-send")) return ConfirmSend;
    if (access == QStringLiteral("armed-confirm-send")) return ArmedConfirmSend;
    return ReadOnly;
}

bool AIActionRegistry::validate(const QJsonObject &action, QString *error)
{
    const QString capability = action.value(QStringLiteral("capability")).toString();
    if (definition(capability).isEmpty())
    {
        if (error) *error = QStringLiteral("Unknown capability: %1").arg(capability);
        return false;
    }
    if (!action.value(QStringLiteral("arguments")).isObject())
    {
        if (error) *error = QStringLiteral("arguments must be a JSON object");
        return false;
    }
    const QJsonObject arguments = action.value(QStringLiteral("arguments")).toObject();
    auto require = [&](const QString &key) {
        if (arguments.contains(key) && !arguments.value(key).isNull()) return true;
        if (error) *error = QStringLiteral("Missing required argument: %1").arg(key);
        return false;
    };
    if (capability == QStringLiteral("ui.open")) return require(QStringLiteral("target"));
    if (capability.startsWith(QStringLiteral("uds.")) &&
        (capability.endsWith(QStringLiteral("did")) || capability == QStringLiteral("uds.read_did")))
        return require(QStringLiteral("did"));
    if (capability.startsWith(QStringLiteral("obd.")) &&
        (capability.endsWith(QStringLiteral("pid")) || capability == QStringLiteral("obd.query_pid")))
    {
        if (!require(QStringLiteral("pid"))) return false;
        bool ok = false;
        uint pid = 0;
        const QJsonValue value = arguments.value(QStringLiteral("pid"));
        if (value.isDouble())
        {
            const int numeric = value.toInt(-1);
            ok = numeric >= 0;
            pid = ok ? uint(numeric) : 0;
        }
        else
            pid = value.toString().toUInt(&ok, 0);
        if (!ok || pid > 0xFF)
        {
            if (error) *error = QStringLiteral(
                "OBD PID must be between 0x00 and 0xFF; a CAN identifier is not a PID.");
            return false;
        }
        return true;
    }
    if (capability == QStringLiteral("canopen.add_object"))
        return require(QStringLiteral("node_id")) && require(QStringLiteral("index"))
            && require(QStringLiteral("subindex"));
    if (capability == QStringLiteral("fuzz.configure"))
        return require(QStringLiteral("start_id")) && require(QStringLiteral("end_id"));
    if (capability == QStringLiteral("frame.add_draft"))
        return require(QStringLiteral("can_id")) && require(QStringLiteral("payload"));
    if (capability == QStringLiteral("script.create_draft"))
        return require(QStringLiteral("source"));
    if (capability == QStringLiteral("payload.set_format"))
        return require(QStringLiteral("format"));
    if (capability == QStringLiteral("filter.set_id"))
        return require(QStringLiteral("can_id")) && require(QStringLiteral("enabled"));
    if (capability == QStringLiteral("graph.add"))
        return require(QStringLiteral("can_id")) && require(QStringLiteral("start_bit"))
            && require(QStringLiteral("bit_length"));
    if (capability == QStringLiteral("dashboard.add_pid"))
        return require(QStringLiteral("pid"));
    if (capability.endsWith(QStringLiteral(".execute")))
    {
        if (!require(QStringLiteral("operation"))) return false;
        const QString operation = arguments.value(QStringLiteral("operation")).toString();
        if (capability == QStringLiteral("uds.execute") && operation == QStringLiteral("read_did"))
            return require(QStringLiteral("did"));
        if (capability == QStringLiteral("uds.execute") && operation == QStringLiteral("scan_dids"))
            return require(QStringLiteral("start_did")) && require(QStringLiteral("end_did"));
        if (capability == QStringLiteral("obd.execute") && operation == QStringLiteral("query_pid"))
            return require(QStringLiteral("pid"));
        if (capability == QStringLiteral("canopen.execute") && operation == QStringLiteral("upload"))
            return require(QStringLiteral("node_id")) && require(QStringLiteral("index"))
                && require(QStringLiteral("subindex"));
        return true;
    }
    if (capability == QStringLiteral("canopen.write"))
        return require(QStringLiteral("node_id")) && require(QStringLiteral("index"))
            && require(QStringLiteral("subindex")) && require(QStringLiteral("value"));
    if (capability == QStringLiteral("fuzz.start"))
        return require(QStringLiteral("duration_ms"));
    if (capability == QStringLiteral("frame.send"))
        return require(QStringLiteral("row"));
    if (capability == QStringLiteral("frame.send_once"))
        return require(QStringLiteral("bus")) && require(QStringLiteral("can_id"))
            && require(QStringLiteral("payload"));
    if (capability == QStringLiteral("frame.send_loop"))
    {
        if (!require(QStringLiteral("bus")) || !require(QStringLiteral("can_id"))
            || !require(QStringLiteral("payload")) || !require(QStringLiteral("count"))
            || !require(QStringLiteral("interval_ms"))) return false;
        const int count = arguments.value(QStringLiteral("count")).toInt();
        const int interval = arguments.value(QStringLiteral("interval_ms")).toInt();
        if (count < 1 || count > 1000 || interval < 1 || interval > 60000)
        {
            if (error) *error = QStringLiteral(
                "Frame loop count must be 1-1000 and interval_ms must be 1-60000.");
            return false;
        }
        return true;
    }
    if (capability == QStringLiteral("connection.reconnect"))
        return require(QStringLiteral("port"));
    if (capability == QStringLiteral("connection.add"))
        return require(QStringLiteral("type")) && require(QStringLiteral("port"));
    return true;
}
