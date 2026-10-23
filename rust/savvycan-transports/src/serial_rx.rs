//! Opt-in serial attach for an already-streaming LAWICEL/SLCAN device.
//! The adapter never writes to the port or issues S/O/L/Z commands.
//! Some serial drivers pulse DTR on open even when disabled, so users
//! must expect that opening a port may reset certain microcontrollers.
use crate::SlcanStreamDecoder;
use savvycan_core::{
    CanFrame, CanReceive, ReceiveCapabilities, ReceiveDescriptor, ReceiveError,
};
use std::collections::VecDeque;
use std::io::{ErrorKind, Read};
use std::time::Duration;

pub struct SlcanSerialReceive {
    port: Box<dyn serialport::SerialPort>,
    decoder: SlcanStreamDecoder,
    pending: VecDeque<Result<CanFrame, ReceiveError>>,
    descriptor: ReceiveDescriptor,
}

impl SlcanSerialReceive {
    /// Opens a preconfigured, auto-streaming SLCAN adapter. Does not
    /// negotiate bitrate, open the CAN channel, or send anything.
    pub fn open(path: &str, baud: u32, interface: &str) -> Result<Self, ReceiveError> {
        if path.trim().is_empty() {
            return Err(ReceiveError::Protocol("serial device path cannot be empty".into()));
        }
        if interface.trim().is_empty() {
            return Err(ReceiveError::Protocol("SLCAN interface label cannot be empty".into()));
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
            decoder: SlcanStreamDecoder::new(interface),
            pending: VecDeque::new(),
            descriptor: ReceiveDescriptor {
                name: "LAWICEL/SLCAN serial (preconfigured, receive-only)".into(),
                interface: interface.into(),
                capabilities: ReceiveCapabilities {
                    classic_can: true, can_fd: false, rtr: true, hardware_timestamps: false,
                }
            },
        })
    }
}

impl CanReceive for SlcanSerialReceive {
    fn descriptor(&self) -> &ReceiveDescriptor { &self.descriptor }

    fn receive(&mut self, timeout: Duration) -> Result<Option<CanFrame>, ReceiveError> {
        if let Some(result) = self.pending.pop_front() {
            return result.map(Some);
        }
        let wait = timeout.max(Duration::from_millis(1));
        self.port.set_timeout(wait).map_err(|e| ReceiveError::Io(e.to_string()))?;
        let mut bytes = [0_u8; 512];
        match self.port.read(&mut bytes) {
            Ok(0) => Ok(None),
            Ok(length) => {
                for event in self.decoder.push_bytes(&bytes[..length]) {
                    self.pending.push_back(event.map_err(|e|
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
    fn rejects_invalid_port_configuration_before_opening() {
        for (path, baud, iface) in [
            ("", 115_200, "slcan0"),
            ("/unused", 0, "slcan0"),
            ("/unused", 115_200, ""),
        ] {
            assert!(matches!(
                SlcanSerialReceive::open(path, baud, iface),
                Err(ReceiveError::Protocol(_))
            ));
        }
    }
}
