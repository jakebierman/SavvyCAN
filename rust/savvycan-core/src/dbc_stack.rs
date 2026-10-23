//! Ordered per-interface DBC association and global fallback.
use crate::{CanFrame, DbcDatabase, DecodeError, DecodedMessage};
use std::collections::HashMap;

/// Exact CAN-ID matches win over J1939 PGN fallbacks. Within each matching
/// class, interface-specific databases precede globals and insertion order
/// is preserved; ambiguous PGNs in a single DBC are not guessed.
#[derive(Debug, Clone, Default)]
pub struct DbcStack {
    per_interface: HashMap<String, Vec<DbcDatabase>>,
    global: Vec<DbcDatabase>,
}

impl DbcStack {
    pub fn new() -> Self { Self::default() }

    pub fn add_global(&mut self, dbc: DbcDatabase) {
        self.global.push(dbc);
    }

    pub fn add_for_interface(&mut self, interface: impl Into<String>, dbc: DbcDatabase) {
        self.per_interface.entry(interface.into()).or_default().push(dbc);
    }

    pub fn total_databases(&self) -> usize {
        self.global.len() + self.per_interface.values().map(Vec::len).sum::<usize>()
    }

    pub fn decode(&self, frame: &CanFrame) -> Result<Option<DecodedMessage>, DecodeError> {
        let candidates = || self.per_interface.get(&frame.interface)
            .into_iter().flatten().chain(self.global.iter());
        for dbc in candidates() {
            if dbc.messages.contains_key(&frame.id) {
                return dbc.decode(frame);
            }
        }
        for dbc in candidates() {
            if dbc.j1939_pgn_matching && dbc.matching_message_id(frame.id).is_some() {
                return dbc.decode(frame);
            }
        }
        Ok(None)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{CanId, FdFlags, FrameKind};

    fn sample(name: &str) -> DbcDatabase {
        DbcDatabase::parse(&format!(
            "BO_ 291 {name}: 2 ECU\n SG_ Voltage : 0|8@1+ (1,0) [0|255] \"V\" ECU\n"
        )).unwrap()
    }
    fn frame(interface: &str, id: CanId) -> CanFrame {
        CanFrame::new(None, interface, id, FrameKind::Data, vec![12, 0],
            false, FdFlags::default()).unwrap()
    }

    fn j1939(name: &str, template: u32) -> DbcDatabase {
        DbcDatabase::parse(&format!(
            "BO_ {} {name}: 2 ECU\n SG_ Voltage : 0|8@1+ (1,0) [0|255] \"V\" ECU\nBA_ \"ProtocolType\" \"J1939\";\n",
            template | 0x8000_0000
        )).unwrap()
    }

    #[test]
    fn j1939_pgn_fallback_follows_interface_precedence() {
        let mut stack = DbcStack::new();
        stack.add_global(j1939("GlobalJ1939", 0x18f00400));
        stack.add_for_interface("can0", j1939("EngineJ1939", 0x18f00400));
        assert_eq!(stack.decode(&frame("can0", CanId::Extended(0x0cf004a5)))
            .unwrap().unwrap().name, "EngineJ1939");
        assert_eq!(stack.decode(&frame("can1", CanId::Extended(0x0cf004a5)))
            .unwrap().unwrap().name, "GlobalJ1939");
        assert!(stack.decode(&frame("can0", CanId::Extended(0x0cf005a5)))
            .unwrap().is_none());
    }

    #[test]
    fn exact_id_across_databases_precedes_j1939_pgn_fallback() {
        let mut stack = DbcStack::new();
        stack.add_for_interface("can0", j1939("PgnFallback", 0x18f00400));
        stack.add_global(j1939("ExactGlobal", 0x0cf004a5));
        assert_eq!(stack.decode(&frame("can0", CanId::Extended(0x0cf004a5)))
            .unwrap().unwrap().name, "ExactGlobal");
    }

    #[test]
    fn interface_precedence_and_ordering() {
        let id = CanId::Standard(291);
        let mut stack = DbcStack::new();
        stack.add_global(sample("Fallback"));
        stack.add_global(sample("LowerPriority"));
        stack.add_for_interface("can0", sample("Can0First"));
        stack.add_for_interface("can0", sample("Can0Second"));
        stack.add_for_interface("can1", sample("Can1"));
        assert_eq!(stack.total_databases(), 5);
        assert_eq!(stack.decode(&frame("can0", id)).unwrap().unwrap().name, "Can0First");
        assert_eq!(stack.decode(&frame("can1", id)).unwrap().unwrap().name, "Can1");
        assert_eq!(stack.decode(&frame("vcan0", id)).unwrap().unwrap().name, "Fallback");
        assert!(stack.decode(&frame("can0", CanId::Extended(291))).unwrap().is_none());
    }
    #[test]
    fn unmatched_scoped_database_does_not_block_global() {
        let mut stack = DbcStack::new();
        stack.add_for_interface("can0", DbcDatabase::default());
        stack.add_global(sample("Global"));
        assert_eq!(stack.decode(&frame("can0", CanId::Standard(291))).unwrap().unwrap().name,
            "Global");
    }
}
