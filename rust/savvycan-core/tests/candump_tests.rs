use std::io::Cursor;

use savvycan_core::{
    parse_candump_line, read_candump, summarize, CanFrame, CanId,
    FdFlags, FrameKind,
};

fn parse(line: &str) -> CanFrame {
    parse_candump_line(line).unwrap().unwrap()
}

#[test]
fn classic_with_microsecond_timestamp() {
    let frame = parse("(1690000000.123456) can0 123#DEADBEEF");
    assert_eq!(frame.timestamp_us, Some(1_690_000_000_123_456));
    assert_eq!(frame.interface, "can0");
    assert_eq!(frame.id, CanId::Standard(0x123));
    assert_eq!(frame.data, [0xDE, 0xAD, 0xBE, 0xEF]);
    assert!(!frame.is_fd);
}

#[test]
fn extended_id_distinct_from_standard() {
    let frame = parse("can1 00000123#01");
    assert_eq!(frame.id, CanId::Extended(0x123));
    assert_eq!(frame.id.to_string(), "00000123");
    assert_ne!(frame.id, CanId::Standard(0x123));
}

#[test]
fn parses_fd_and_preserves_flags() {
    let frame = parse("(100.25) can0 18DAF110##3AABBCCDDEE");
    assert_eq!(frame.timestamp_us, Some(100_250_000));
    assert_eq!(frame.id, CanId::Extended(0x18DAF110));
    assert!(frame.is_fd);
    assert_eq!(frame.fd_flags, FdFlags { bitrate_switch: true, error_state_indicator: true });
    assert_eq!(frame.data, [0xAA, 0xBB, 0xCC, 0xDD, 0xEE]);
}

#[test]
fn parses_remote_requests_without_data() {
    let frame = parse("vcan0 123#R8");
    assert_eq!(frame.kind, FrameKind::RemoteRequest { dlc: 8 });
    assert_eq!(frame.len(), 8);
    assert!(frame.data.is_empty());
    assert!(parse_candump_line("vcan0 123#R9").is_err());
}

#[test]
fn parses_bracketed_payload() {
    let frame = parse("can0 7FF [3] 00 aB ff");
    assert_eq!(frame.data, [0, 0xAB, 0xFF]);
}

#[test]
fn rejects_invalid_ids_and_lengths() {
    assert!(CanId::standard(0x800).is_err());
    assert!(CanId::extended(0x2000_0000).is_err());
    assert!(parse_candump_line("can0 800#01").is_err());
    assert!(parse_candump_line("can0 123#001122334455667788").is_err());
    assert!(parse_candump_line("can0 123##4AABB").is_err());
    assert!(parse_candump_line("can0 123#AA0").is_err());
    assert!(parse_candump_line("can0 123 [2] AB").is_err());
}

#[test]
fn allows_sixty_four_byte_fd_payload() {
    let data = "00".repeat(64);
    let frame = parse(&format!("can0 123##0{data}"));
    assert_eq!(frame.data.len(), 64);
    assert!(parse_candump_line(&format!("can0 123##0{data}00")).is_err());
}

#[test]
fn empty_and_comments_are_ignored() {
    assert!(parse_candump_line("   ").unwrap().is_none());
    assert!(parse_candump_line(" # recorded").unwrap().is_none());
}

#[test]
fn reports_errors_with_line_numbers() {
    let input = Cursor::new("can0 123#01\n\ncan0 123#ZZ\n");
    let error = read_candump(input).unwrap_err();
    assert!(error.to_string().contains("line 3"));
}

#[test]
fn preserves_interface_and_id_in_trace_counts() {
    let input = Cursor::new(concat!(
        "(1.0) can0 123#01\n",
        "(1.1) can0 123#02\n",
        "can1 123#01\n",
        "can0 00000123#01\n",
        "can0 123##101\n",
        "can0 123#R8\n",
    ));
    let frames = read_candump(input).unwrap();
    let summary = summarize(&frames);
    assert_eq!(summary.frames, 6);
    assert_eq!(summary.unique_identifiers, 3);
    assert_eq!(summary.fd_frames, 1);
    assert_eq!(summary.remote_requests, 1);
    assert_eq!(summary.timestamped_frames, 2);
}

#[test]
fn parser_rejects_extra_tokens() {
    assert!(parse_candump_line("can0 123#00 unexpected").is_err());
}

#[test]
fn rejects_invalid_timestamp() {
    assert!(parse_candump_line("(99.bad) can0 123#11").is_err());
    assert!(parse_candump_line("(18446744073709551615.0) can0 123#11").is_err());
}
