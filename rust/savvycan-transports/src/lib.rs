//! Native receive-only adapters. No CAN transmission method is exposed.
#![forbid(unsafe_code)]

pub use savvycan_core::{CanReceive, ReceiveCapabilities, ReceiveDescriptor, ReceiveError, ReplayReceive};

pub mod gvret;
pub mod slcan;
pub mod serial_rx;
pub use gvret::{GvretDecodeError, GvretStreamDecoder};
pub use slcan::{SlcanDecodeError, SlcanStreamDecoder};
pub use serial_rx::SlcanSerialReceive;

#[cfg(target_os = "linux")]
mod linux {
    use savvycan_core::{
        CanFrame, CanId, CanReceive, Direction, FdFlags, FrameKind,
        ReceiveCapabilities, ReceiveDescriptor, ReceiveError,
    };
    use socketcan::{CanAnyFrame, CanFdSocket, EmbeddedFrame, Frame, Socket, SocketOptions};
    use std::io::ErrorKind;
    use std::time::{Duration, SystemTime, UNIX_EPOCH};

    /// Reads classic CAN, CAN FD and RTR from one Linux SocketCAN interface.
    /// A CAN_RAW socket is opened for receiving only; this type provides no send path.
    pub struct SocketCanReceive {
        socket: CanFdSocket,
        descriptor: ReceiveDescriptor,
        kernel_timestamp: bool,
    }

    impl SocketCanReceive {
        pub fn open(interface: &str) -> Result<Self, ReceiveError> {
            if interface.is_empty() { return Err(ReceiveError::Protocol("interface cannot be empty".into())); }
            let socket = CanFdSocket::open(interface)
                .map_err(|err| ReceiveError::Io(err.to_string()))?;
            // Prefer the kernel's receive timestamp, falling back to user-space
            // time when SO_TIMESTAMPNS is unavailable on this system.
            let kernel_timestamp = socket.set_recv_timestamp(true).is_ok();
            Ok(Self {
                socket,
                kernel_timestamp,
                descriptor: ReceiveDescriptor {
                    name: "Linux SocketCAN".into(),
                    interface: interface.to_owned(),
                    capabilities: ReceiveCapabilities {
                        classic_can: true, can_fd: true, rtr: true, hardware_timestamps: false,
                    },
                },
            })
        }
    }

    impl CanReceive for SocketCanReceive {
        fn descriptor(&self) -> &ReceiveDescriptor { &self.descriptor }

        fn receive(&mut self, timeout: Duration) -> Result<Option<CanFrame>, ReceiveError> {
            let result = if self.kernel_timestamp {
                self.socket.set_read_timeout(timeout)
                    .and_then(|_| self.socket.read_frame_with_timestamp())
            } else {
                self.socket.read_frame_timeout(timeout)
                    .map(|frame| (frame, SystemTime::now()))
            };
            match result {
                Ok((frame, time)) => {
                    let timestamp_us = time.duration_since(UNIX_EPOCH).ok()
                        .and_then(|since_epoch| u64::try_from(since_epoch.as_micros()).ok());
                    map_frame(&self.descriptor.interface, timestamp_us, frame).map(Some)
                }
                Err(err) if matches!(err.kind(), ErrorKind::TimedOut | ErrorKind::WouldBlock) => Ok(None),
                Err(err) => Err(ReceiveError::Io(err.to_string())),
            }
        }
    }

    fn map_frame(interface: &str, timestamp_us: Option<u64>, incoming: CanAnyFrame)
        -> Result<CanFrame, ReceiveError>
    {
        let extended = incoming.is_extended();
        let id = if extended {
            CanId::extended(incoming.raw_id())
        } else {
            let raw = incoming.raw_id();
            if raw > CanId::MAX_STANDARD as u32 {
                return Err(ReceiveError::Protocol("invalid standard CAN identifier".into()));
            }
            CanId::standard(raw as u16)
        }.map_err(|err| ReceiveError::Protocol(err.to_string()))?;
        let (kind, data, is_fd, fd_flags) = match incoming {
            CanAnyFrame::Normal(frame) =>
                (FrameKind::Data, frame.data().to_vec(), false, FdFlags::default()),
            CanAnyFrame::Remote(frame) => {
                let dlc = u8::try_from(frame.dlc())
                    .map_err(|_| ReceiveError::Protocol("RTR length exceeds u8".into()))?;
                (FrameKind::RemoteRequest { dlc }, Vec::new(), false, FdFlags::default())
            }
            CanAnyFrame::Fd(frame) =>
                (FrameKind::Data, frame.data().to_vec(), true, FdFlags {
                    bitrate_switch: frame.is_brs(),
                    error_state_indicator: frame.is_esi(),
                }),
            CanAnyFrame::Error(_) =>
                return Err(ReceiveError::Protocol("kernel reported a CAN error frame".into())),
        };
        let mut frame = CanFrame::new(timestamp_us, interface, id, kind, data, is_fd, fd_flags)
            .map_err(|err| ReceiveError::Protocol(err.to_string()))?;
        frame.direction = Direction::Received;
        Ok(frame)
    }

    #[cfg(test)]
    mod tests {
        use super::*;
        use socketcan::{CanDataFrame, CanFdFrame, CanRemoteFrame, ExtendedId, StandardId};

        #[test]
        fn maps_can_frames_without_losing_11_vs_29_bit_identity() {
            let standard = StandardId::new(0x123).unwrap();
            let extended = ExtendedId::new(0x123).unwrap();
            let normal = CanDataFrame::new(standard, &[0x10, 0x20]).unwrap();
            let standard_frame = map_frame("vcan0", Some(100), CanAnyFrame::Normal(normal)).unwrap();
            assert_eq!(standard_frame.id, CanId::Standard(0x123));
            assert_eq!(standard_frame.data, vec![0x10, 0x20]);
            assert_eq!(standard_frame.direction, Direction::Received);
            let ext = CanDataFrame::new(extended, &[0xAA]).unwrap();
            let extended_frame = map_frame("vcan0", None, CanAnyFrame::Normal(ext)).unwrap();
            assert_eq!(extended_frame.id, CanId::Extended(0x123));
        }

        #[test]
        fn maps_fd_flags_and_rtr_without_fabricating_payload() {
            let id = StandardId::new(0x700).unwrap();
            let mut fd = CanFdFrame::new(id, &[0x55; 12]).unwrap();
            fd.set_brs(true);
            fd.set_esi(true);
            let mapped = map_frame("can0", Some(1), CanAnyFrame::Fd(fd)).unwrap();
            assert!(mapped.is_fd);
            assert!(mapped.fd_flags.bitrate_switch);
            assert!(mapped.fd_flags.error_state_indicator);
            assert_eq!(mapped.data.len(), 12);
            let remote = CanRemoteFrame::new_remote(id, 8).unwrap();
            let mapped = map_frame("can0", Some(2), CanAnyFrame::Remote(remote)).unwrap();
            assert_eq!(mapped.kind, FrameKind::RemoteRequest { dlc: 8 });
            assert!(mapped.data.is_empty());
            assert!(!mapped.is_fd);
        }
    }
}

#[cfg(target_os = "linux")]
pub use linux::SocketCanReceive;
