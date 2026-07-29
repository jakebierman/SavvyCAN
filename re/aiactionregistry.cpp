#include "aiactionregistry.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

QJsonArray AIActionRegistry::catalog()
{
    static const char json[] = R"JSON([
{"capability":"ui.open","title":"Open a tool","ui":"Application","access":"read","arguments":{"target":"trace|uds|obd|canopen|bus_diagnostics|diagnostic_simulator|ai|fuzzing|frame_sender|scripting|sniffer|graphing|temporal_graph|playback|dbc|dbc_compare|connection|isotp|bridge|uds_scanner|uds_firmware|signal_viewer|flow_view|range|bisect"}},
{"capability":"uds.add_did","title":"Add a DID request","ui":"UDS Workbench","access":"edit","arguments":{"did":"hex DID","name":"optional","format":"payload format","poll_ms":"integer"}},
{"capability":"uds.read_did","title":"Add a DID request","ui":"UDS Workbench","access":"edit","arguments":{"did":"hex DID","name":"optional","format":"payload format","poll_ms":"integer"}},
{"capability":"uds.clear_dids","title":"Clear the DID request list","ui":"UDS Workbench","access":"edit","arguments":{}},
{"capability":"uds.disconnect","title":"Disconnect the UDS endpoint","ui":"UDS Workbench","access":"edit","arguments":{}},
{"capability":"uds.stop_polling","title":"Stop DID polling","ui":"UDS Workbench","access":"edit","arguments":{}},
{"capability":"uds.connect","title":"Connect the configured UDS session","ui":"UDS Workbench","access":"confirm-send","arguments":{}},
{"capability":"uds.request_all","title":"Request all enabled DIDs once","ui":"UDS Workbench","access":"confirm-send","arguments":{}},
{"capability":"uds.start_polling","title":"Start polling enabled DIDs","ui":"UDS Workbench","access":"confirm-send","arguments":{}},
{"capability":"uds.scan_ecus","title":"Start UDS ECU discovery","ui":"UDS Workbench","access":"confirm-send","arguments":{}},
{"capability":"uds.scan_services","title":"Scan UDS services","ui":"UDS Workbench","access":"confirm-send","arguments":{}},
{"capability":"uds.scan_sessions","title":"Scan diagnostic sessions","ui":"UDS Workbench","access":"confirm-send","arguments":{}},
{"capability":"uds.read_dtcs","title":"Read UDS diagnostic trouble codes","ui":"UDS Workbench","access":"confirm-send","arguments":{}},
{"capability":"uds.tester_present","title":"Send UDS Tester Present","ui":"UDS Workbench","access":"confirm-send","arguments":{}},
{"capability":"uds.clear_dtcs","title":"Clear UDS diagnostic information","ui":"UDS Workbench","access":"armed-confirm-send","arguments":{}},
{"capability":"uds.routine_control","title":"Run UDS RoutineControl","ui":"UDS Workbench","access":"armed-confirm-send","arguments":{"control":"start|stop|results","routine_id":"hex routine ID","data":"optional hex bytes"}},
{"capability":"uds.security_seed","title":"Request a UDS SecurityAccess seed","ui":"UDS Workbench","access":"confirm-send","arguments":{"level":"odd security level"}},
{"capability":"uds.security_key","title":"Send a UDS SecurityAccess key","ui":"UDS Workbench","access":"armed-confirm-send","arguments":{"level":"odd security level","key":"hex key bytes"}},
{"capability":"uds.detailed_dtc","title":"Send detailed UDS ReadDTCInformation","ui":"UDS Workbench","access":"confirm-send","arguments":{"subfunction":"UDS subfunction","data":"optional hex bytes"}},
{"capability":"uds.manual_request","title":"Send an arbitrary UDS service request","ui":"UDS Workbench","access":"armed-confirm-send","arguments":{"service":"hex service","data":"optional hex bytes"}},
{"capability":"uds.guarded_control","title":"Send a state-changing UDS control request","ui":"UDS Workbench","access":"armed-confirm-send","arguments":{"service":"hex service","data":"hex bytes"}},
{"capability":"obd.add_pid","title":"Add a PID request","ui":"OBD-II Workbench","access":"edit","arguments":{"pid":"hex PID","name":"optional","format":"auto or payload format"}},
{"capability":"obd.query_pid","title":"Add a PID request","ui":"OBD-II Workbench","access":"edit","arguments":{"pid":"hex PID","name":"optional","format":"auto or payload format"}},
{"capability":"obd.clear_pids","title":"Clear all OBD-II PID requests","ui":"OBD-II Workbench","access":"edit","arguments":{}},
{"capability":"obd.configure_pids","title":"Replace or extend the OBD PID request list","ui":"OBD-II Workbench","access":"edit","arguments":{"pids":"array of PID names or hexadecimal PID values","clear_existing":"optional boolean","start_polling":"optional boolean"}},
{"capability":"obd.connect","title":"Enable the configured OBD endpoint","ui":"OBD-II Workbench","access":"edit","arguments":{}},
{"capability":"obd.disconnect","title":"Disable the OBD endpoint","ui":"OBD-II Workbench","access":"edit","arguments":{}},
{"capability":"obd.stop_polling","title":"Stop OBD PID polling","ui":"OBD-II Workbench","access":"edit","arguments":{}},
{"capability":"obd.request_enabled","title":"Request all enabled OBD PIDs once","ui":"OBD-II Workbench","access":"confirm-send","arguments":{}},
{"capability":"obd.start_polling","title":"Start OBD PID polling","ui":"OBD-II Workbench","access":"confirm-send","arguments":{}},
{"capability":"obd.scan_modules","title":"Discover OBD modules","ui":"OBD-II Workbench","access":"confirm-send","arguments":{}},
{"capability":"obd.scan_pids","title":"Scan supported OBD PIDs","ui":"OBD-II Workbench","access":"confirm-send","arguments":{}},
{"capability":"obd.read_dtcs","title":"Read OBD diagnostic trouble codes","ui":"OBD-II Workbench","access":"confirm-send","arguments":{"kind":"stored|pending|permanent"}},
{"capability":"obd.vehicle_info","title":"Request OBD Mode 09 vehicle information","ui":"OBD-II Workbench","access":"confirm-send","arguments":{"pid":"Mode 09 information PID"}},
{"capability":"obd.freeze_frame","title":"Request an OBD freeze-frame PID","ui":"OBD-II Workbench","access":"confirm-send","arguments":{"pid":"hex PID","frame":"freeze-frame number"}},
{"capability":"obd.monitor_results","title":"Request OBD Mode 06 monitor results","ui":"OBD-II Workbench","access":"confirm-send","arguments":{"mid":"monitor ID"}},
{"capability":"obd.clear_dtcs","title":"Clear OBD DTCs and readiness data","ui":"OBD-II Workbench","access":"armed-confirm-send","arguments":{}},
{"capability":"obd.edit_pid","title":"Edit an OBD PID request","ui":"OBD-II Workbench","access":"edit","arguments":{"pid":"hex PID","name":"optional","format":"optional","enabled":"optional boolean"}},
{"capability":"obd.remove_pids","title":"Remove selected OBD PID requests","ui":"OBD-II Workbench","access":"edit","arguments":{"pids":"array of hexadecimal PIDs"}},
{"capability":"obd.read_readiness","title":"Read OBD monitor readiness","ui":"OBD-II Workbench","access":"confirm-send","arguments":{}},
{"capability":"obd.dashboard_set","title":"Replace the OBD dashboard layout","ui":"OBD-II Workbench","access":"edit","arguments":{"columns":"optional grid columns","widgets":"array of dashboard widget definitions"}},
{"capability":"obd.dashboard_clear","title":"Clear the OBD dashboard","ui":"OBD-II Workbench","access":"edit","arguments":{}},
{"capability":"obd.dashboard_save","title":"Save the OBD dashboard layout","ui":"OBD-II Workbench","access":"edit","arguments":{"path":"destination JSON path"}},
{"capability":"obd.dashboard_load","title":"Load the OBD dashboard layout","ui":"OBD-II Workbench","access":"edit","arguments":{"path":"existing JSON path"}},
{"capability":"canopen.add_object","title":"Add an Object Dictionary entry","ui":"CANopen Workbench","access":"edit","arguments":{"node_id":"1-127","index":"hex index","subindex":"hex/integer","name":"optional","data_type":"optional","access":"ro|rw|wo","value":"optional"}},
{"capability":"canopen.scan_nodes","title":"Probe CANopen nodes","ui":"CANopen Workbench","access":"confirm-send","arguments":{}},
{"capability":"canopen.nmt","title":"Send a CANopen NMT command","ui":"CANopen Workbench","access":"armed-confirm-send","arguments":{"node_id":"1-127","command":"start|stop|pre_operational|reset_node|reset_communication"}},
{"capability":"canopen.sync","title":"Send a CANopen SYNC frame","ui":"CANopen Workbench","access":"confirm-send","arguments":{}},
{"capability":"canopen.time","title":"Send CANopen TIME","ui":"CANopen Workbench","access":"confirm-send","arguments":{}},
{"capability":"canopen.discover_lss","title":"Discover an unconfigured LSS device","ui":"CANopen Workbench","access":"confirm-send","arguments":{}},
{"capability":"canopen.read_drive_state","title":"Read CiA 402 drive state","ui":"CANopen Workbench","access":"confirm-send","arguments":{"node_id":"1-127"}},
{"capability":"canopen.remove_object","title":"Remove an Object Dictionary entry","ui":"CANopen Workbench","access":"edit","arguments":{"index":"hex index","subindex":"hex/integer"}},
{"capability":"canopen.import_eds","title":"Import an EDS or DCF","ui":"CANopen Workbench","access":"edit","arguments":{"path":"existing .eds or .dcf path"}},
{"capability":"canopen.export_dcf","title":"Export the Object Dictionary as DCF","ui":"CANopen Workbench","access":"edit","arguments":{"path":"destination .dcf path"}},
{"capability":"canopen.clear_emcy","title":"Clear CANopen EMCY history","ui":"CANopen Workbench","access":"edit","arguments":{}},
{"capability":"canopen.configure_lss","title":"Configure a CANopen LSS device","ui":"CANopen Workbench","access":"armed-confirm-send","arguments":{"node_id":"new node ID 1-127","bitrate_index":"CiA LSS bitrate table index"}},
{"capability":"canopen.remap_pdo","title":"Guardedly remap a CANopen PDO","ui":"CANopen Workbench","access":"armed-confirm-send","arguments":{"node_id":"1-127","direction":"tpdo|rpdo","pdo":"1-4","cob_id":"hex COB-ID","mappings":"array of index, subindex and bits objects"}},
{"capability":"canopen.upload_objects","title":"Upload several Object Dictionary entries","ui":"CANopen Workbench","access":"confirm-send","arguments":{"node_id":"1-127","objects":"array of index and subindex objects"}},
{"capability":"canopen.write_objects","title":"Write several Object Dictionary entries","ui":"CANopen Workbench","access":"armed-confirm-send","arguments":{"node_id":"1-127","objects":"array of index, subindex and value objects"}},
{"capability":"fuzz.configure","title":"Configure fuzzing without starting it","ui":"Fuzzing","access":"edit","arguments":{"bus":"integer","start_id":"hex CAN ID","end_id":"hex CAN ID","interval_ms":"integer","burst":"integer","bytes":"1-8"}},
{"capability":"frame.add_draft","title":"Add a disabled frame draft","ui":"Frame Sender","access":"edit","arguments":{"bus":"integer","can_id":"hex CAN ID","extended":"boolean","payload":"space-separated hex bytes"}},
{"capability":"frame.update_draft","title":"Edit a Frame Sender row","ui":"Frame Sender","access":"edit","arguments":{"row":"zero-based row","bus":"optional integer","can_id":"optional hex CAN ID","extended":"optional boolean","remote":"optional boolean","payload":"optional hex bytes","trigger":"optional sender trigger","modifications":"optional modifiers","enabled":"optional boolean"}},
{"capability":"frame.set_enabled","title":"Enable or disable a Frame Sender row","ui":"Frame Sender","access":"armed-confirm-send","arguments":{"row":"zero-based row","enabled":"boolean"}},
{"capability":"frame.remove_rows","title":"Remove Frame Sender rows","ui":"Frame Sender","access":"edit","arguments":{"rows":"array of zero-based rows"}},
{"capability":"frame.clear","title":"Clear the Frame Sender grid","ui":"Frame Sender","access":"edit","arguments":{}},
{"capability":"frame.set_all_enabled","title":"Enable or disable all Frame Sender rows","ui":"Frame Sender","access":"armed-confirm-send","arguments":{"enabled":"boolean"}},
{"capability":"frame.save_grid","title":"Save the Frame Sender grid","ui":"Frame Sender","access":"edit","arguments":{"path":"destination .fsd path"}},
{"capability":"frame.load_grid","title":"Load a Frame Sender grid","ui":"Frame Sender","access":"edit","arguments":{"path":"existing .fsd path"}},
{"capability":"trace_sender.add","title":"Add a compact CAN Trace sender row","ui":"Trace Sender","access":"edit","arguments":{"bus":"integer","can_id":"hex CAN ID","extended":"optional boolean","remote":"optional boolean","payload":"space-separated hex bytes","interval_ms":"optional 1-3600000","limit":"optional 0-1000000; zero is unlimited"}},
{"capability":"trace_sender.update","title":"Edit a compact CAN Trace sender row","ui":"Trace Sender","access":"edit","arguments":{"row":"zero-based row","bus":"optional integer","can_id":"optional hex CAN ID","extended":"optional boolean","remote":"optional boolean","payload":"optional hex bytes","interval_ms":"optional 1-3600000","limit":"optional 0-1000000"}},
{"capability":"trace_sender.remove","title":"Remove compact CAN Trace sender rows","ui":"Trace Sender","access":"edit","arguments":{"rows":"array of zero-based rows"}},
{"capability":"trace_sender.clear","title":"Clear the compact CAN Trace sender list","ui":"Trace Sender","access":"edit","arguments":{}},
{"capability":"trace_sender.stop","title":"Stop compact CAN Trace sender rows","ui":"Trace Sender","access":"edit","arguments":{"rows":"array of zero-based rows"}},
{"capability":"trace_sender.copy_selected","title":"Copy the selected CAN Trace frame into the compact sender","ui":"Trace Sender","access":"edit","arguments":{}},
{"capability":"trace_sender.to_advanced","title":"Copy compact rows into the advanced Frame Sender","ui":"Trace Sender","access":"edit","arguments":{"rows":"array of zero-based rows"}},
{"capability":"trace_sender.save","title":"Save the compact CAN Trace sender list","ui":"Trace Sender","access":"edit","arguments":{"path":"destination JSON path"}},
{"capability":"trace_sender.load","title":"Load the compact CAN Trace sender list","ui":"Trace Sender","access":"edit","arguments":{"path":"existing JSON path"}},
{"capability":"trace_sender.start","title":"Start compact CAN Trace sender rows","ui":"Trace Sender","access":"armed-confirm-send","arguments":{"rows":"array of zero-based rows"}},
{"capability":"trace_sender.send_once","title":"Send configured compact CAN Trace sender rows once","ui":"Trace Sender","access":"armed-confirm-send","arguments":{"rows":"array of zero-based rows"}},
{"capability":"script.create_draft","title":"Create an unsaved script draft","ui":"Scripting","access":"edit","arguments":{"name":"optional","source":"JavaScript source"}},
{"capability":"payload.set_format","title":"Set the main payload display format","ui":"CAN Trace","access":"edit","arguments":{"format":"payload format expression"}},
{"capability":"trace.set_options","title":"Set trace display options","ui":"CAN Trace","access":"edit","arguments":{"overwrite":"optional boolean","interpret_dbc":"optional boolean","show_raw_payload":"optional boolean"}},
{"capability":"filter.set_id","title":"Enable or disable a CAN-ID filter","ui":"CAN Trace","access":"edit","arguments":{"can_id":"hex CAN ID","enabled":"boolean"}},
{"capability":"filter.clear","title":"Disable every known CAN-ID filter","ui":"CAN Trace","access":"edit","arguments":{}},
{"capability":"filter.set_range","title":"Set filters for a CAN-ID range","ui":"CAN Trace","access":"edit","arguments":{"start_id":"hex CAN ID","end_id":"hex CAN ID","enabled":"boolean"}},
{"capability":"filter.set_mask","title":"Set filters matching a CAN-ID mask","ui":"CAN Trace","access":"edit","arguments":{"filter":"hex CAN ID","mask":"hex mask","enabled":"boolean"}},
{"capability":"filter.save_profile","title":"Save the current filter profile","ui":"CAN Trace","access":"edit","arguments":{"path":"destination filter file"}},
{"capability":"filter.load_profile","title":"Load a filter profile","ui":"CAN Trace","access":"edit","arguments":{"path":"existing filter file"}},
{"capability":"playback.load_file","title":"Load a capture into Playback","ui":"Playback","access":"edit","arguments":{"path":"existing capture file","loops":"optional sequence loop count"}},
{"capability":"playback.load_live","title":"Load the current trace into Playback","ui":"Playback","access":"edit","arguments":{}},
{"capability":"playback.configure","title":"Configure Playback","ui":"Playback","access":"edit","arguments":{"interval_ms":"optional integer","burst":"optional integer","loop":"optional boolean","original_timing":"optional boolean","bus":"optional bus"}},
{"capability":"playback.delete_sequence","title":"Remove a Playback sequence","ui":"Playback","access":"edit","arguments":{"row":"optional zero-based row"}},
{"capability":"playback.pause","title":"Pause Playback","ui":"Playback","access":"edit","arguments":{}},
{"capability":"playback.stop","title":"Stop Playback","ui":"Playback","access":"edit","arguments":{}},
{"capability":"playback.play","title":"Start forward Playback","ui":"Playback","access":"confirm-send","arguments":{}},
{"capability":"playback.reverse","title":"Start reverse Playback","ui":"Playback","access":"confirm-send","arguments":{}},
{"capability":"playback.step_forward","title":"Transmit the next Playback frame","ui":"Playback","access":"confirm-send","arguments":{}},
{"capability":"playback.step_back","title":"Transmit the previous Playback frame","ui":"Playback","access":"confirm-send","arguments":{}},
{"capability":"sniffer.clear","title":"Clear CAN Sniffer state","ui":"CAN Sniffer","access":"edit","arguments":{}},
{"capability":"sniffer.notch","title":"Notch unchanged Sniffer bits","ui":"CAN Sniffer","access":"edit","arguments":{}},
{"capability":"sniffer.unnotch","title":"Restore notched Sniffer bits","ui":"CAN Sniffer","access":"edit","arguments":{}},
{"capability":"sniffer.filter_all","title":"Show all Sniffer IDs","ui":"CAN Sniffer","access":"edit","arguments":{}},
{"capability":"sniffer.filter_none","title":"Hide all Sniffer IDs","ui":"CAN Sniffer","access":"edit","arguments":{}},
{"capability":"sniffer.pause","title":"Pause Sniffer display updates","ui":"CAN Sniffer","access":"edit","arguments":{}},
{"capability":"sniffer.resume","title":"Resume Sniffer display updates","ui":"CAN Sniffer","access":"edit","arguments":{}},
{"capability":"sniffer.experiment_baseline","title":"Record a differential baseline","ui":"CAN Sniffer","access":"edit","arguments":{}},
{"capability":"sniffer.experiment_action","title":"Record a differential action sample","ui":"CAN Sniffer","access":"edit","arguments":{}},
{"capability":"sniffer.experiment_control","title":"Record a differential control sample","ui":"CAN Sniffer","access":"edit","arguments":{}},
{"capability":"sniffer.experiment_stop","title":"Stop differential recording","ui":"CAN Sniffer","access":"edit","arguments":{}},
{"capability":"sniffer.experiment_analyze","title":"Analyze the differential experiment","ui":"CAN Sniffer","access":"edit","arguments":{}},
{"capability":"sniffer.infer_counters","title":"Infer counters and checksums","ui":"CAN Sniffer","access":"edit","arguments":{}},
{"capability":"sniffer.correlate_diagnostics","title":"Correlate diagnostics with broadcast fields","ui":"CAN Sniffer","access":"edit","arguments":{}},
{"capability":"sniffer.cluster_signals","title":"Cluster correlated signal candidates","ui":"CAN Sniffer","access":"edit","arguments":{}},
{"capability":"dbc.load","title":"Load a DBC file","ui":"DBC","access":"edit","arguments":{"path":"existing .dbc path","bus":"optional associated bus"}},
{"capability":"dbc.create","title":"Create a blank DBC database","ui":"DBC","access":"edit","arguments":{"bus":"optional associated bus"}},
{"capability":"dbc.save","title":"Save a DBC database","ui":"DBC","access":"edit","arguments":{"file_index":"zero-based loaded file","path":"destination .dbc path"}},
{"capability":"dbc.close","title":"Close a loaded DBC database","ui":"DBC","access":"edit","arguments":{"file_index":"zero-based loaded file"}},
{"capability":"dbc.assign_bus","title":"Assign a DBC database to a bus","ui":"DBC","access":"edit","arguments":{"file_index":"zero-based loaded file","bus":"bus number or -1 for all"}},
{"capability":"dbc.add_node","title":"Add a DBC node","ui":"DBC","access":"edit","arguments":{"file_index":"zero-based loaded file","name":"node name","comment":"optional"}},
{"capability":"dbc.remove_node","title":"Remove an unused DBC node","ui":"DBC","access":"edit","arguments":{"file_index":"zero-based loaded file","name":"node name"}},
{"capability":"dbc.add_message","title":"Add a DBC message","ui":"DBC","access":"edit","arguments":{"file_index":"zero-based loaded file","can_id":"hex CAN ID","extended":"boolean","name":"message name","length":"0-64","sender":"optional node","comment":"optional"}},
{"capability":"dbc.update_message","title":"Edit a DBC message","ui":"DBC","access":"edit","arguments":{"file_index":"zero-based loaded file","can_id":"current hex CAN ID","new_can_id":"optional","name":"optional","length":"optional","extended":"optional","sender":"optional","comment":"optional"}},
{"capability":"dbc.remove_message","title":"Remove a DBC message","ui":"DBC","access":"edit","arguments":{"file_index":"zero-based loaded file","can_id":"hex CAN ID"}},
{"capability":"dbc.add_signal","title":"Add a signal to a DBC message","ui":"DBC","access":"edit","arguments":{"file_index":"zero-based loaded file","can_id":"hex CAN ID","name":"signal name","start_bit":"integer, or use byte_offset","bit_length":"1-64, or use byte_length","byte_offset":"optional zero-based byte shortcut","byte_length":"optional byte shortcut","little_endian":"boolean","value_type":"unsigned|signed|float32|float64|string","factor":"number","offset":"number","minimum":"number","maximum":"number","unit":"optional","receiver":"optional node","comment":"optional","values":"optional array of value/label enum objects","multiplex_role":"none|multiplexor|multiplexed|both","multiplex_values":"optional UI range expression","multiplex_parent":"optional signal name"}},
{"capability":"dbc.update_signal","title":"Edit a DBC signal","ui":"DBC","access":"edit","arguments":{"file_index":"zero-based loaded file","can_id":"hex CAN ID","signal":"current signal name","name":"optional","start_bit":"optional","bit_length":"optional","byte_offset":"optional","byte_length":"optional","little_endian":"optional","value_type":"optional unsigned|signed|float32|float64|string","factor":"optional","offset":"optional","minimum":"optional","maximum":"optional","unit":"optional","receiver":"optional node","comment":"optional","values":"optional replacement enum array","multiplex_role":"optional none|multiplexor|multiplexed|both","multiplex_values":"optional UI range expression","multiplex_parent":"optional signal name"}},
{"capability":"dbc.remove_signal","title":"Remove a DBC signal","ui":"DBC","access":"edit","arguments":{"file_index":"zero-based loaded file","can_id":"hex CAN ID","signal":"signal name"}},
{"capability":"graph.add","title":"Add a raw bit-field graph","ui":"Graphing","access":"edit","arguments":{"can_id":"hex CAN ID","bus":"integer","start_bit":"integer","bit_length":"1-64","little_endian":"boolean","signed":"boolean","scale":"number","offset":"number","name":"optional"}},
{"capability":"dashboard.add_pid","title":"Add an OBD PID dashboard widget","ui":"OBD-II Workbench","access":"edit","arguments":{"pid":"hex PID"}},
{"capability":"uds.execute","title":"Execute a prepared UDS request","ui":"UDS Workbench","access":"confirm-send","arguments":{"operation":"read_did|scan_dids|session|dtc"}},
{"capability":"obd.execute","title":"Execute a prepared OBD request","ui":"OBD-II Workbench","access":"confirm-send","arguments":{"operation":"query_pid|request_enabled|start_polling|stop_polling|scan_modules|scan_pids"}},
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

QJsonArray AIActionRegistry::skills()
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
        QFile file(QDir(root).absoluteFilePath(QStringLiteral("ai_skills.json")));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
        if (error.error == QJsonParseError::NoError && document.isArray()
            && !document.array().isEmpty())
            return document.array();
    }

    static const char json[] = R"JSON([
{"id":"obd","title":"OBD-II requests and live data","package":"skills/savvycan-obd",
 "triggers":["obd","obd-ii","pid","mode 01","mode 09","rpm","revs","coolant","vehicle speed","engine speed","throttle","fuel trim","maf","oxygen sensor","vin"],
 "workspaces":["obd-ii workbench","obd workbench"],
 "capabilities":["ui.open","obd.configure_pids","obd.add_pid","obd.query_pid","obd.clear_pids","obd.connect","obd.disconnect","obd.request_enabled","obd.start_polling","obd.stop_polling","obd.scan_modules","obd.scan_pids","obd.read_dtcs","obd.clear_dtcs","obd.vehicle_info","obd.freeze_frame","obd.monitor_results","obd.execute","dashboard.add_pid"],
 "rules":["Resolve human PID descriptions through SavvyCAN's built-in PID catalog.","RPM is Mode 01 PID 0x0C; engine coolant temperature is Mode 01 PID 0x05.","A PID is an 8-bit service parameter, never a CAN arbitration identifier.","Use obd.configure_pids for replace/add/start-polling requests so the operation remains one ordered workflow.","Do not use raw frame tools for a normal OBD request."],
 "examples":[
  {"user":"clear the list, add rpm and coolant temperature, then start polling","capability":"obd.configure_pids","arguments":{"pids":["rpm","coolant temperature"],"clear_existing":true,"start_polling":true}},
  {"user":"add vehicle speed","capability":"obd.add_pid","arguments":{"pid":"0x0D","name":"Vehicle speed","format":"auto"}}
 ]},
{"id":"uds","title":"UDS diagnostics","package":"skills/savvycan-uds",
 "triggers":["uds","did","diagnostic session","tester present","security access","routine control","ecu discovery","service discovery"],
 "workspaces":["uds workbench"],
 "capabilities":["ui.open","uds.add_did","uds.read_did","uds.clear_dids","uds.connect","uds.disconnect","uds.request_all","uds.start_polling","uds.stop_polling","uds.scan_ecus","uds.scan_services","uds.scan_sessions","uds.read_dtcs","uds.clear_dtcs","uds.tester_present","uds.execute"],
 "rules":["DIDs are 16-bit identifiers and are distinct from CAN request and response identifiers.","Manufacturer-specific DID names must come from the active profile, discovery results, or an explicit DID supplied by the user; never invent one.","Use uds.add_did to edit the list and uds.execute only when the user asks to transmit."],
 "examples":[
  {"user":"add DID 0x200A as transmission temperature","capability":"uds.add_did","arguments":{"did":"0x200A","name":"Transmission temperature","format":"auto","poll_ms":0}},
  {"user":"read DID F190","capability":"uds.execute","arguments":{"operation":"read_did","did":"0xF190"}}
 ]},
{"id":"raw_can","title":"Raw CAN frames and transmission","package":"skills/savvycan-raw-can",
 "triggers":["raw can","can frame","frame","frames","packet","packets","trace sender","sender row","send once","send repeatedly","send data","send bytes","loop frame","repeat frame","transmit","arbitration id","can id"],
 "workspaces":["can trace"],
 "capabilities":["ui.open","frame.add_draft","frame.update_draft","frame.set_enabled","frame.remove_rows","frame.clear","frame.set_all_enabled","frame.save_grid","frame.load_grid","frame.send","frame.send_once","frame.send_loop","trace_sender.add","trace_sender.update","trace_sender.remove","trace_sender.clear","trace_sender.stop","trace_sender.copy_selected","trace_sender.to_advanced","trace_sender.save","trace_sender.load","trace_sender.start","trace_sender.send_once"],
 "rules":["Use frame.send_once for an explicitly described one-shot frame.","Use frame.send_loop for a newly requested bounded repetition so it appears in the compact CAN Trace sender.","Use trace_sender capabilities to operate configured compact rows. Row indices come from trace_sender_rows application context.","Use frame draft/grid capabilities only for the advanced Frame Sender with triggers and modifiers.","Payload is a space-separated hexadecimal byte string and can_id is the arbitration identifier."],
 "examples":[
  {"user":"send 123 01 02 once on bus 0","capability":"frame.send_once","arguments":{"bus":0,"can_id":"0x123","extended":false,"payload":"01 02"}},
  {"user":"send that frame ten times every 100 ms","capability":"frame.send_loop","arguments":{"bus":0,"can_id":"0x123","extended":false,"payload":"01 02","count":10,"interval_ms":100}}
 ]},
{"id":"canopen","title":"CANopen operations","package":"skills/savvycan-canopen",
 "triggers":["canopen","can open","sdo","pdo","object dictionary","eds","nmt","emcy"],
 "workspaces":["canopen workbench"],
 "capabilities":["ui.open","canopen.add_object","canopen.scan_nodes","canopen.nmt","canopen.sync","canopen.time","canopen.discover_lss","canopen.read_drive_state","canopen.execute","canopen.write"],
 "rules":["An Object Dictionary address consists of node ID, index and subindex.","Writes require armed full access and confirmation."],
 "examples":[]},
{"id":"fuzzing","title":"CAN fuzzing","package":"skills/savvycan-fuzzing",
 "triggers":["fuzz","fuzzing","brute force"],
 "workspaces":["fuzzing"],
 "capabilities":["ui.open","fuzz.configure","fuzz.start"],
 "rules":["Configure the bounded range first and start only when explicitly requested.","Starting fuzzing requires armed full access."],
 "examples":[]},
{"id":"savvycan_interface","title":"SavvyCAN interface, trace and analysis tools","package":"skills/savvycan-interface",
 "triggers":["savvycan","main window","workbench","open tool","trace","filter","payload","format","overwrite","raw payload","dbc","decode","bit","graph","plot","sniffer","notch","bus diagnostics","playback","frame analysis"],
 "workspaces":["can trace"],
 "capabilities":["ui.open","payload.set_format","trace.set_options","filter.set_id","graph.add"],
 "rules":["A display filter or formatter does not transmit traffic.","Use filter.set_id for a known arbitration ID and payload.set_format for the selected trace payload display."],
 "examples":[]},
{"id":"connections","title":"CAN interfaces and connections","package":"skills/savvycan-connections",
 "triggers":["connect","connection","disconnect","reconnect","driver","interface","socketcan","serial","gvret","kvaser"],
 "workspaces":["connection settings"],
 "capabilities":["ui.open","connection.reconnect","connection.suspend","connection.resume","connection.add"],
 "rules":["Use existing profiles when reconnecting.","Adding a connection requires explicit interface details and armed access."],
 "examples":[]},
{"id":"scripting","title":"JavaScript scripting","package":"skills/savvycan-scripting",
 "triggers":["script","scripting","javascript"],
 "workspaces":["scripting"],
 "capabilities":["ui.open","script.create_draft"],
 "rules":["Only expose scripting when the user explicitly asks for a script or JavaScript."],
 "examples":[]}
])JSON";
    return QJsonDocument::fromJson(QByteArray(json)).array();
}

QStringList AIActionRegistry::matchingSkills(const QString &question,
                                             const QJsonObject &applicationContext)
{
    const QString lower = question.toLower();
    const QString workspace =
        applicationContext.value(QStringLiteral("active_workspace")).toString().toLower();
    struct Match { QString id; int score; };
    QList<Match> matches;
    for (const QJsonValue &value : skills())
    {
        const QJsonObject skill = value.toObject();
        int score = 0;
        for (const QJsonValue &trigger : skill.value(QStringLiteral("triggers")).toArray())
        {
            const QString term = trigger.toString();
            bool present = lower.contains(term);
            if (term.size() <= 3 && !term.contains(QLatin1Char(' ')))
                present = QRegularExpression(
                    QStringLiteral("\\b%1\\b").arg(QRegularExpression::escape(term)))
                              .match(lower).hasMatch();
            if (present) score += term.contains(QLatin1Char(' ')) ? 4 : 2;
        }
        for (const QJsonValue &candidate : skill.value(QStringLiteral("workspaces")).toArray())
            if (!workspace.isEmpty() && workspace.contains(candidate.toString())) score += 1;
        if (score > 0)
            matches.append({skill.value(QStringLiteral("id")).toString(), score});
    }
    std::sort(matches.begin(), matches.end(), [](const Match &left, const Match &right) {
        if (left.score != right.score) return left.score > right.score;
        return left.id < right.id;
    });
    QStringList result;
    for (const Match &match : matches)
    {
        if (result.size() >= 3) break;
        result.append(match.id);
    }
    return result;
}

QJsonArray AIActionRegistry::catalogForQuestion(const QString &question,
                                                const QJsonObject &applicationContext)
{
    const QStringList selected = matchingSkills(question, applicationContext);
    QSet<QString> capabilities;
    for (const QJsonValue &value : skills())
    {
        const QJsonObject skill = value.toObject();
        if (!selected.contains(skill.value(QStringLiteral("id")).toString())) continue;
        for (const QJsonValue &capability : skill.value(QStringLiteral("capabilities")).toArray())
            capabilities.insert(capability.toString());
    }
    QJsonArray result;
    for (const QJsonValue &value : catalog())
        if (capabilities.contains(value.toObject().value(QStringLiteral("capability")).toString()))
            result.append(value);
    return result;
}

QString AIActionRegistry::skillVersion()
{
    return QStringLiteral("2026.07.4");
}

QString AIActionRegistry::skillContext(const QString &question,
                                       const QJsonObject &applicationContext)
{
    const QStringList selected = matchingSkills(question, applicationContext);
    QJsonArray selectedDefinitions;
    for (const QJsonValue &value : skills())
        if (selected.contains(value.toObject().value(QStringLiteral("id")).toString()))
            selectedDefinitions.append(value);
    QJsonObject context{
        {QStringLiteral("skill_bundle_version"), skillVersion()},
        {QStringLiteral("selected_skills"), selectedDefinitions},
        {QStringLiteral("available_capabilities"),
         catalogForQuestion(question, applicationContext)}
    };
    return QString::fromUtf8(QJsonDocument(context).toJson(QJsonDocument::Indented));
}

QJsonObject AIActionRegistry::skillDiagnostics(const QJsonObject &applicationContext)
{
    QSet<QString> catalogCapabilities;
    for (const QJsonValue &value : catalog())
        catalogCapabilities.insert(
            value.toObject().value(QStringLiteral("capability")).toString());

    QSet<QString> referencedCapabilities;
    QStringList unknownReferences;
    for (const QJsonValue &value : skills())
    {
        const QJsonObject skill = value.toObject();
        const QString skillId = skill.value(QStringLiteral("id")).toString();
        for (const QJsonValue &capabilityValue :
             skill.value(QStringLiteral("capabilities")).toArray())
        {
            const QString capability = capabilityValue.toString();
            referencedCapabilities.insert(capability);
            if (!catalogCapabilities.contains(capability))
                unknownReferences.append(QStringLiteral("%1: %2").arg(skillId, capability));
        }
    }

    QStringList uncovered;
    for (const QString &capability : catalogCapabilities)
        if (!referencedCapabilities.contains(capability)) uncovered.append(capability);
    uncovered.sort();
    unknownReferences.sort();

    QJsonArray evaluations;
    const QString applicationDir = QCoreApplication::applicationDirPath();
    const QStringList roots = {
        applicationDir + QStringLiteral("/help"),
        QDir(applicationDir).absoluteFilePath(QStringLiteral("../help")),
        QDir(applicationDir).absoluteFilePath(QStringLiteral("../../help")),
        QDir::current().absoluteFilePath(QStringLiteral("help"))
    };
    for (const QString &root : roots)
    {
        QFile file(QDir(root).absoluteFilePath(QStringLiteral("ai_skill_evaluations.json")));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
        if (error.error == QJsonParseError::NoError && document.isArray())
            evaluations = document.array();
        break;
    }

    QJsonArray failures;
    int passed = 0;
    for (const QJsonValue &value : evaluations)
    {
        const QJsonObject evaluation = value.toObject();
        const QString prompt = evaluation.value(QStringLiteral("prompt")).toString();
        QJsonObject context = applicationContext;
        const QString workspace = evaluation.value(QStringLiteral("workspace")).toString();
        if (!workspace.isEmpty())
            context.insert(QStringLiteral("active_workspace"), workspace);
        const QStringList matched = matchingSkills(prompt, context);
        const QJsonArray tools = catalogForQuestion(prompt, context);
        QSet<QString> capabilities;
        for (const QJsonValue &tool : tools)
            capabilities.insert(
                tool.toObject().value(QStringLiteral("capability")).toString());
        const QString expectedSkill =
            evaluation.value(QStringLiteral("expected_skill")).toString();
        const QString expectedCapability =
            evaluation.value(QStringLiteral("expected_capability")).toString();
        const bool ok = (expectedSkill.isEmpty() || matched.contains(expectedSkill))
            && (expectedCapability.isEmpty()
                || capabilities.contains(expectedCapability));
        if (ok)
            ++passed;
        else
            failures.append(QJsonObject{
                {QStringLiteral("prompt"), prompt},
                {QStringLiteral("expected_skill"), expectedSkill},
                {QStringLiteral("expected_capability"), expectedCapability},
                {QStringLiteral("matched_skills"), QJsonArray::fromStringList(matched)}
            });
    }

    return QJsonObject{
        {QStringLiteral("version"), skillVersion()},
        {QStringLiteral("skill_count"), skills().size()},
        {QStringLiteral("capability_count"), catalogCapabilities.size()},
        {QStringLiteral("uncovered_capabilities"), QJsonArray::fromStringList(uncovered)},
        {QStringLiteral("unknown_capability_references"),
         QJsonArray::fromStringList(unknownReferences)},
        {QStringLiteral("evaluations_total"), evaluations.size()},
        {QStringLiteral("evaluations_passed"), passed},
        {QStringLiteral("evaluation_failures"), failures},
        {QStringLiteral("valid"), uncovered.isEmpty() && unknownReferences.isEmpty()
             && passed == evaluations.size()}
    };
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
    if (capability == QStringLiteral("uds.routine_control"))
        return require(QStringLiteral("control")) && require(QStringLiteral("routine_id"));
    if (capability == QStringLiteral("uds.security_seed"))
        return require(QStringLiteral("level"));
    if (capability == QStringLiteral("uds.security_key"))
        return require(QStringLiteral("level")) && require(QStringLiteral("key"));
    if (capability == QStringLiteral("uds.detailed_dtc"))
        return require(QStringLiteral("subfunction"));
    if (capability == QStringLiteral("uds.manual_request")
        || capability == QStringLiteral("uds.guarded_control"))
        return require(QStringLiteral("service"));
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
    if (capability == QStringLiteral("obd.configure_pids"))
    {
        if (arguments.contains(QStringLiteral("resolution_error")))
        {
            if (error)
                *error = arguments.value(QStringLiteral("resolution_error")).toString();
            return false;
        }
        if (!require(QStringLiteral("pids"))) return false;
        if (!arguments.value(QStringLiteral("pids")).isArray()
            || arguments.value(QStringLiteral("pids")).toArray().isEmpty())
        {
            if (error) *error = QStringLiteral("pids must be a non-empty JSON array.");
            return false;
        }
        return true;
    }
    if (capability == QStringLiteral("obd.edit_pid"))
        return require(QStringLiteral("pid"));
    if (capability == QStringLiteral("obd.remove_pids"))
        return require(QStringLiteral("pids")) && arguments.value(QStringLiteral("pids")).isArray();
    if (capability == QStringLiteral("obd.dashboard_set"))
        return require(QStringLiteral("widgets")) && arguments.value(QStringLiteral("widgets")).isArray();
    if (capability == QStringLiteral("obd.dashboard_save")
        || capability == QStringLiteral("obd.dashboard_load"))
        return require(QStringLiteral("path"));
    if (capability == QStringLiteral("canopen.add_object"))
        return require(QStringLiteral("node_id")) && require(QStringLiteral("index"))
            && require(QStringLiteral("subindex"));
    if (capability == QStringLiteral("fuzz.configure"))
        return require(QStringLiteral("start_id")) && require(QStringLiteral("end_id"));
    if (capability == QStringLiteral("frame.add_draft"))
        return require(QStringLiteral("can_id")) && require(QStringLiteral("payload"));
    if (capability == QStringLiteral("frame.update_draft")
        || capability == QStringLiteral("frame.set_enabled"))
        return require(QStringLiteral("row"));
    if (capability == QStringLiteral("frame.remove_rows"))
        return require(QStringLiteral("rows")) && arguments.value(QStringLiteral("rows")).isArray();
    if (capability == QStringLiteral("frame.set_all_enabled"))
        return require(QStringLiteral("enabled"));
    if (capability == QStringLiteral("frame.save_grid")
        || capability == QStringLiteral("frame.load_grid")
        || capability == QStringLiteral("filter.save_profile")
        || capability == QStringLiteral("filter.load_profile"))
        return require(QStringLiteral("path"));
    if (capability == QStringLiteral("playback.load_file"))
        return require(QStringLiteral("path"));
    if (capability == QStringLiteral("dbc.load"))
        return require(QStringLiteral("path"));
    if (capability == QStringLiteral("dbc.save"))
        return require(QStringLiteral("file_index")) && require(QStringLiteral("path"));
    if (capability == QStringLiteral("dbc.close")
        || capability == QStringLiteral("dbc.assign_bus"))
        return require(QStringLiteral("file_index"));
    if (capability == QStringLiteral("dbc.add_node")
        || capability == QStringLiteral("dbc.remove_node"))
        return require(QStringLiteral("file_index")) && require(QStringLiteral("name"));
    if (capability == QStringLiteral("dbc.add_message"))
        return require(QStringLiteral("file_index")) && require(QStringLiteral("can_id"))
            && require(QStringLiteral("name"));
    if (capability == QStringLiteral("dbc.update_message")
        || capability == QStringLiteral("dbc.remove_message"))
        return require(QStringLiteral("file_index")) && require(QStringLiteral("can_id"));
    if (capability == QStringLiteral("dbc.add_signal"))
    {
        if (!require(QStringLiteral("file_index")) || !require(QStringLiteral("can_id"))
            || !require(QStringLiteral("name"))) return false;
        const bool bitAddress = arguments.contains(QStringLiteral("start_bit"))
            && arguments.contains(QStringLiteral("bit_length"));
        const bool byteAddress = arguments.contains(QStringLiteral("byte_offset"))
            && arguments.contains(QStringLiteral("byte_length"));
        if (!bitAddress && !byteAddress) {
            if (error) *error = QStringLiteral(
                "DBC signal needs start_bit and bit_length, or byte_offset and byte_length.");
            return false;
        }
        return true;
    }
    if (capability == QStringLiteral("dbc.update_signal")
        || capability == QStringLiteral("dbc.remove_signal"))
        return require(QStringLiteral("file_index")) && require(QStringLiteral("can_id"))
            && require(QStringLiteral("signal"));
    if (capability == QStringLiteral("script.create_draft"))
        return require(QStringLiteral("source"));
    if (capability == QStringLiteral("payload.set_format"))
        return require(QStringLiteral("format"));
    if (capability == QStringLiteral("filter.set_id"))
        return require(QStringLiteral("can_id")) && require(QStringLiteral("enabled"));
    if (capability == QStringLiteral("filter.set_range"))
        return require(QStringLiteral("start_id")) && require(QStringLiteral("end_id"))
            && require(QStringLiteral("enabled"));
    if (capability == QStringLiteral("filter.set_mask"))
        return require(QStringLiteral("filter")) && require(QStringLiteral("mask"))
            && require(QStringLiteral("enabled"));
    if (capability == QStringLiteral("graph.add"))
        return require(QStringLiteral("can_id")) && require(QStringLiteral("start_bit"))
            && require(QStringLiteral("bit_length"));
    if (capability == QStringLiteral("dashboard.add_pid"))
        return require(QStringLiteral("pid"));
    if (capability == QStringLiteral("obd.vehicle_info")
        || capability == QStringLiteral("obd.freeze_frame"))
        return require(QStringLiteral("pid"));
    if (capability == QStringLiteral("obd.monitor_results"))
        return require(QStringLiteral("mid"));
    if (capability == QStringLiteral("canopen.nmt"))
        return require(QStringLiteral("node_id")) && require(QStringLiteral("command"));
    if (capability == QStringLiteral("canopen.read_drive_state"))
        return require(QStringLiteral("node_id"));
    if (capability == QStringLiteral("canopen.remove_object"))
        return require(QStringLiteral("index")) && require(QStringLiteral("subindex"));
    if (capability == QStringLiteral("canopen.import_eds")
        || capability == QStringLiteral("canopen.export_dcf"))
        return require(QStringLiteral("path"));
    if (capability == QStringLiteral("canopen.configure_lss"))
        return require(QStringLiteral("node_id")) && require(QStringLiteral("bitrate_index"));
    if (capability == QStringLiteral("canopen.remap_pdo"))
        return require(QStringLiteral("node_id")) && require(QStringLiteral("direction"))
            && require(QStringLiteral("pdo")) && require(QStringLiteral("cob_id"))
            && require(QStringLiteral("mappings"));
    if (capability == QStringLiteral("canopen.upload_objects")
        || capability == QStringLiteral("canopen.write_objects"))
        return require(QStringLiteral("node_id")) && require(QStringLiteral("objects"))
            && arguments.value(QStringLiteral("objects")).isArray();
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
    if (capability == QStringLiteral("trace_sender.add"))
        return require(QStringLiteral("bus")) && require(QStringLiteral("can_id"))
            && require(QStringLiteral("payload"));
    if (capability == QStringLiteral("trace_sender.update"))
        return require(QStringLiteral("row"));
    if (capability == QStringLiteral("trace_sender.remove")
        || capability == QStringLiteral("trace_sender.stop")
        || capability == QStringLiteral("trace_sender.to_advanced")
        || capability == QStringLiteral("trace_sender.start")
        || capability == QStringLiteral("trace_sender.send_once"))
        return require(QStringLiteral("rows"))
            && arguments.value(QStringLiteral("rows")).isArray();
    if (capability == QStringLiteral("trace_sender.save")
        || capability == QStringLiteral("trace_sender.load"))
        return require(QStringLiteral("path"));
    if (capability == QStringLiteral("connection.reconnect"))
        return require(QStringLiteral("port"));
    if (capability == QStringLiteral("connection.add"))
        return require(QStringLiteral("type")) && require(QStringLiteral("port"));
    return true;
}
