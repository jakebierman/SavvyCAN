use savvycan_core::{
    CanFrame, CanId, DbcDatabase, DecodeError, FdFlags, FrameKind,
};

fn classic(id: u16, data: &[u8]) -> CanFrame {
    CanFrame::new(
        None, "can0", CanId::standard(id).unwrap(), FrameKind::Data,
        data.into(), false, FdFlags::default(),
    ).unwrap()
}

#[test]
fn imports_real_savvycan_bms_example() {
    let dbc = DbcDatabase::parse(include_str!("../../../examples/bms.dbc")).unwrap();
    assert_eq!(dbc.messages.len(), 6);
    let decoded = dbc.decode(&classic(1542, &[
        0x10, 0x27, 0x9c, 0xff, 0x80, 0x01, 0x00, 0x00
    ])).unwrap().unwrap();
    assert_eq!(decoded.name, "BMS_STATUS");
    let volts = decoded.signals.iter().find(|signal| signal.name == "PACKVOLTS").unwrap();
    let amps = decoded.signals.iter().find(|signal| signal.name == "PACKAMPS").unwrap();
    let soc = decoded.signals.iter().find(|signal| signal.name == "STATEOFCHARGE").unwrap();
    assert_eq!(volts.value, 100.0);
    assert_eq!(volts.unit, "V");
    assert_eq!(amps.value, -1.0);
    assert!((soc.value - 50.176).abs() < 1e-9);
}

#[test]
fn intel_and_motorola_are_not_swapped() {
    let dbc = DbcDatabase::parse(r#"
BO_ 123 Test: 8 ECU
 SG_ Intel : 0|16@1+ (1,0) [0|65535] "" ECU
 SG_ Motorola : 23|16@0+ (1,0) [0|65535] "" ECU
"#).unwrap();
    let decoded = dbc.decode(&classic(123, &[0x12, 0x34, 0x56, 0x78, 0, 0, 0, 0]))
        .unwrap().unwrap();
    assert_eq!(decoded.signals[0].raw, 0x3412);
    assert_eq!(decoded.signals[1].raw, 0x5678);
}

#[test]
fn signed_and_scaled_fields() {
    let dbc = DbcDatabase::parse(r#"
BO_ 100 Status: 8 ECU
 SG_ Temperature : 0|8@1- (0.5,-40) [-168|87] "C" ECU
 SG_ Torque : 15|16@0- (0.1,0) [-3276|3276] "Nm" ECU
"#).unwrap();
    let decoded = dbc.decode(&classic(100, &[0xFE, 0xFF, 0x9C])).unwrap().unwrap();
    assert_eq!(decoded.signals[0].value, -41.0);
    assert_eq!(decoded.signals[1].raw, 0xFF9C);
    assert_eq!(decoded.signals[1].value, -10.0);
}

#[test]
fn simple_multiplexing_selects_only_active_signals() {
    let dbc = DbcDatabase::parse(r#"
BO_ 256 Mixed: 8 ECU
 SG_ Mode M : 0|8@1+ (1,0) [0|255] "" ECU
 SG_ Voltage m1 : 8|16@1+ (0.1,0) [0|6553] "V" ECU
 SG_ Temperature m2 : 8|16@1+ (0.01,0) [0|655] "C" ECU
 SG_ Counter : 24|8@1+ (1,0) [0|255] "" ECU
"#).unwrap();
    let decoded = dbc.decode(&classic(256, &[1, 100, 0, 4])).unwrap().unwrap();
    let names: Vec<_> = decoded.signals.iter().map(|signal| signal.name.as_str()).collect();
    assert_eq!(names, ["Mode", "Voltage", "Counter"]);
    assert_eq!(decoded.signals[1].value, 10.0);
    let decoded = dbc.decode(&classic(256, &[2, 100, 0, 4])).unwrap().unwrap();
    assert_eq!(decoded.signals[1].name, "Temperature");
    assert_eq!(decoded.signals[1].value, 1.0);
}

#[test]
fn extended_multiplex_ranges_and_nested_muxes() {
    let dbc = DbcDatabase::parse(r#"
BO_ 256 Mixed: 8 ECU
 SG_ Mode M : 0|8@1+ (1,0) [0|255] "" ECU
 SG_ SubMode m1M : 8|8@1+ (1,0) [0|255] "" ECU
 SG_ Channel m2 : 16|8@1+ (1,0) [0|255] "" ECU
SG_MUL_VAL_ 256 Channel SubMode 2-4,6-7;
"#).unwrap();
    let decoded = dbc.decode(&classic(256, &[1, 3, 42])).unwrap().unwrap();
    assert_eq!(decoded.signals.len(), 3);
    assert_eq!(decoded.signals[2].value, 42.0);
    let decoded = dbc.decode(&classic(256, &[0, 3, 42])).unwrap().unwrap();
    assert_eq!(decoded.signals.len(), 1);
    let decoded = dbc.decode(&classic(256, &[1, 5, 42])).unwrap().unwrap();
    assert_eq!(decoded.signals.len(), 2);
}

#[test]
fn value_descriptions_are_preserved() {
    let dbc = DbcDatabase::parse(r#"
BO_ 256 Control: 8 ECU
 SG_ State : 0|8@1+ (1,0) [0|255] "" ECU
VAL_ 256 State 0 "Off" 1 "On State" 2 "Fault";
"#).unwrap();
    let decoded = dbc.decode(&classic(256, &[1])).unwrap().unwrap();
    assert_eq!(decoded.signals[0].text.as_deref(), Some("On State"));
    let decoded = dbc.decode(&classic(256, &[3])).unwrap().unwrap();
    assert_eq!(decoded.signals[0].text, None);
}

#[test]
fn ieee_float_signal_is_interpreted_not_cast_from_bits() {
    let dbc = DbcDatabase::parse(r#"
BO_ 256 Telemetry: 8 ECU
 SG_ Pressure : 0|32@1+ (2,1) [0|100] "kPa" ECU
SIG_VALTYPE_ 256 Pressure : 1;
"#).unwrap();
    let decoded = dbc.decode(&classic(256, &[0x00, 0x00, 0xc0, 0x3f])).unwrap().unwrap();
    assert_eq!(decoded.signals[0].value, 4.0);
}

#[test]
fn validates_bad_signal_definitions() {
    assert!(DbcDatabase::parse("BO_ 256 Test: 8 ECU\n SG_ Bad : 0|0@1+ (1,0) [0|0] \"\" ECU").is_err());
    assert!(DbcDatabase::parse("BO_ 256 Test: 8 ECU\n SG_ Bad : 63|16@1+ (1,0) [0|0] \"\" ECU").is_err());
    assert!(DbcDatabase::parse("BO_ 4096 Test: 8 ECU").is_err());
    assert!(DbcDatabase::parse("BO_ 256 Test: 8 ECU\nBO_ 256 Other: 8 ECU").is_err());
}

#[test]
fn supports_dbc_extended_identifier_flag() {
    let dbc = DbcDatabase::parse("BO_ 2147483939 Extended: 8 ECU").unwrap();
    assert!(dbc.messages.contains_key(&CanId::Extended(0x123)));
    assert!(!dbc.messages.contains_key(&CanId::Standard(0x123)));
}

#[test]
fn truncated_payload_is_not_silently_decoded() {
    let dbc = DbcDatabase::parse(r#"
BO_ 256 Telemetry: 8 ECU
 SG_ Speed : 0|16@1+ (1,0) [0|65535] "" ECU
"#).unwrap();
    assert_eq!(
        dbc.decode(&classic(256, &[0x12])).unwrap_err(),
        DecodeError::SignalOutOfBounds("Speed".into()),
    );
}

#[test]
fn remote_request_is_not_interpreted_as_data() {
    let dbc = DbcDatabase::parse("BO_ 256 Status: 0 ECU").unwrap();
    let frame = CanFrame::new(
        None, "can0", CanId::Standard(256), FrameKind::RemoteRequest { dlc: 0 },
        vec![], false, FdFlags::default(),
    ).unwrap();
    assert!(dbc.decode(&frame).unwrap().is_none());
}

#[test]
fn missing_mux_produces_explicit_error() {
    let dbc = DbcDatabase::parse(r#"
BO_ 256 Test: 8 ECU
 SG_ OnlyMode m3 : 0|8@1+ (1,0) [0|1] "" ECU
"#).unwrap();
    assert_eq!(dbc.decode(&classic(256, &[3])).unwrap_err(),
        DecodeError::MissingMultiplexor("OnlyMode".into()));
}

#[test]
fn imports_leaf_powertrain_example_and_decodes_wheel_speed() {
    let dbc = DbcDatabase::parse(include_str!("../../../examples/LeafPowertrainBus.dbc")).unwrap();
    let decoded = dbc.decode(&classic(644, &[0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0]))
        .unwrap().unwrap();
    assert_eq!(decoded.name, "WheelSpeeds");
    assert_eq!(decoded.signals[0].name, "LeftWheel");
    assert_eq!(decoded.signals[0].raw, 0x1234);
    assert_eq!(decoded.signals[1].raw, 0x5678);
}
