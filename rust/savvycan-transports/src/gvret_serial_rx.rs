//! Receive-only attachment to a GVRET device already emitting binary frames.
//! No E7 mode switch, time-sync request, configuration or CAN send occurs.
//! Opening some USB serial ports can still pulse DTR and reset a board.
use crate::GvretStreamDecoder;
use savvycan_core::{
    CanFrame, CanReceive, ReceiveCapabilities, ReceiveDescriptor, ReceiveError,
};
use std::collections::VecDeque;
use std::io::{ErrorKind, Read};
use std::time::Duration;

pub struct GvretSerialReceive {
    port: Box<dyn serialport::SerialPort>,
    decoder: GvretStreamDecoder,
    pending: VecDeque<Result<CanFrame, ReceiveError>>,
    descriptor: ReceiveDescriptor,
}
impl GvretSerialReceive {
    /// Requires an existing binary GVRET frame stream; never sends a
    /// connection handshake and does not configure the vehicle CAN bus.
    pub fn open(path: &str, baud: u32) -> Result<Self, ReceiveError> {
        if path.trim().is_empty() {
            return Err(ReceiveError::Protocol("serial device path cannot be empty".into()));
        }
        if !(9_600..=3_000_000).contains(&baud) {
            return Err(ReceiveError::Protocol(
                "serial bitrate must be between 9600 and 3000000".into()
            ));
        }
        let port = serialport::new(path, baud)
            .timeout(Duration::from_millis(100))
            .data_bits(serialport::DataBits::Eight)
            .flow_control(serialport::FlowControl::None)
            .parity(serialport::Parity::None)
            .stop_bits(serialport::StopBits::One)
            .dtr_on_open(false)
            .open().map_err(|e| ReceiveError::Io(e.to_string()))?;
        Ok(Self {
            port,
            decoder: GvretStreamDecoder::new(),
            pending: VecDeque::new(),
            descriptor: ReceiveDescriptor {
                name: "GVRET binary serial (preconfigured, receive-only)".into(),
                interface: "bus*".into(),
                capabilities: ReceiveCapabilities {
                    classic_can: true, can_fd: true, rtr: false, hardware_timestamps: false,
                },
            },
        })
    }
}
impl CanReceive for GvretSerialReceive {
    fn descriptor(&self) -> &ReceiveDescriptor { &self.descriptor }
    fn receive(&mut self, timeout: Duration) -> Result<Option<CanFrame>, ReceiveError> {
        if let Some(result) = self.pending.pop_front() { return result.map(Some); }
        self.port.set_timeout(timeout.max(Duration::from_millis(1)))
            .map_err(|e| ReceiveError::Io(e.to_string()))?;
        let mut bytes = [0u8; 512];
        match self.port.read(&mut bytes) {
            Ok(0) => Ok(None),
            Ok(size) => {
                for frame in self.decoder.push_bytes(&bytes[..size]) {
                    self.pending.push_back(frame.map_err(|e|
                        ReceiveError::Protocol(e.to_string())
                    ));
                }
                self.pending.pop_front().transpose()
            }
            Err(e) if matches!(e.kind(), ErrorKind::TimedOut | ErrorKind::WouldBlock) => Ok(None),
            Err(e) => Err(ReceiveError::Io(e.to_string())),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn invalid_ports_fail_before_any_serial_device_is_opened() {
        assert!(matches!(GvretSerialReceive::open("", 115_200),
            Err(ReceiveError::Protocol(_))));
        assert!(matches!(GvretSerialReceive::open("/unused", 0),
            Err(ReceiveError::Protocol(_))));
    }
}
