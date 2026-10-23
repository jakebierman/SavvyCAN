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


/// One flattened, read-only DBC catalog entry. A message without signals
/// contributes one entry with empty signal fields.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DbcCatalogEntry {
    pub scope: String,
    /// One-based priority within this scope (lower wins).
    pub database_order: usize,
    pub id: String,
    pub message: String,
    pub length: usize,
    pub sender: String,
    pub signal: String,
    pub layout: String,
    pub kind: String,
    pub scale: String,
    pub unit: String,
    pub multiplex: String,
}

fn append_catalog(
    dbc: &DbcDatabase, scope: &str, database_order: usize,
    terms: &[String], limit: usize,
    rows: &mut Vec<DbcCatalogEntry>, total: &mut usize,
) {
    // HashMap iteration is nondeterministic; stable order is important for
    // reproducible searches and for browsing the first N results.
    let mut messages = dbc.messages.values().collect::<Vec<_>>();
    messages.sort_by_key(|message| (message.id.is_extended(), message.id.value()));
    for message in messages {
        let id = format!("{}:{}", if message.id.is_extended() { "X" } else { "S" }, message.id);
        let signals = message.signals.iter().map(Some)
            .chain(std::iter::once(None).filter(|_| message.signals.is_empty()));
        for signal in signals {
            let (name, layout, kind, scale, unit, multiplex) = match signal {
                Some(signal) => {
                    let layout = format!("{}|{}@{}{}", signal.start_bit, signal.bit_len,
                        if signal.byte_order == crate::ByteOrder::Intel { "1" } else { "0" },
                        if signal.signed { "-" } else { "+" });
                    let kind = match signal.kind {
                        crate::ValueKind::Integer if signal.signed => "signed",
                        crate::ValueKind::Integer => "unsigned",
                        crate::ValueKind::Float32 => "float32",
                        crate::ValueKind::Float64 => "float64",
                    }.to_owned();
                    let multiplex = match &signal.multiplex {
                        Some(mux) => {
                            let ranges = mux.ranges.iter().map(|(low, high)|
                                if low == high { low.to_string() }
                                else { format!("{low}-{high}") })
                                .collect::<Vec<_>>().join(",");
                            format!("{}{}: {ranges}",
                                if signal.is_multiplexor { "MUX " } else { "" },
                                mux.parent.as_deref().unwrap_or("root"))
                        }
                        None if signal.is_multiplexor => "MUX".to_owned(),
                        None => String::new(),
                    };
                    (signal.name.clone(), layout, kind,
                        format!("{} * raw + {}", signal.factor, signal.offset),
                        signal.unit.clone(), multiplex)
                }
                None => (String::new(), String::new(), String::new(),
                    String::new(), String::new(), String::new()),
            };
            let row = DbcCatalogEntry {
                scope: scope.to_owned(), database_order, id: id.clone(),
                message: message.name.clone(), length: message.length,
                sender: message.sender.clone(), signal: name, layout, kind,
                scale, unit, multiplex,
            };
            let searchable = format!("{} {} {} {} {} {} {} {} {} {} {}",
                row.scope, row.id, row.message, row.length, row.sender,
                row.signal, row.layout, row.kind, row.scale, row.unit, row.multiplex)
                .to_lowercase();
            if terms.iter().all(|term| searchable.contains(term)) {
                *total += 1;
                if rows.len() < limit { rows.push(row); }
            }
        }
    }
}

impl DbcStack {
    pub fn new() -> Self { Self::default() }

    pub fn add_global(&mut self, dbc: DbcDatabase) {
        self.global.push(dbc);
    }

    pub fn add_for_interface(&mut self, interface: impl Into<String>, dbc: DbcDatabase) {
        self.per_interface.entry(interface.into()).or_default().push(dbc);
    }

    /// Search all loaded DBC messages and signals without mutating the DBC.
    /// All whitespace-separated query terms must match (case-insensitive).
    /// Scoped databases are listed first, then globals, in decode priority
    /// order within each scope. The total includes entries beyond the limit.
    pub fn catalog_rows(&self, query: &str, limit: usize) -> (Vec<DbcCatalogEntry>, usize) {
        let terms = query.split_whitespace().map(str::to_lowercase).collect::<Vec<_>>();
        let mut rows = Vec::new();
        let mut total = 0;
        let mut scopes = self.per_interface.keys().collect::<Vec<_>>();
        scopes.sort();
        for scope in scopes {
            for (index, dbc) in self.per_interface[scope].iter().enumerate() {
                append_catalog(dbc, scope, index + 1, &terms, limit, &mut rows, &mut total);
            }
        }
        for (index, dbc) in self.global.iter().enumerate() {
            append_catalog(dbc, "*", index + 1, &terms, limit, &mut rows, &mut total);
        }
        (rows, total)
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
    fn catalog_is_deterministic_and_includes_messages_without_signals() {
        let mut stack = DbcStack::new();
        stack.add_global(DbcDatabase::parse("BO_ 292 Empty: 1 ECU\n").unwrap());
        stack.add_for_interface("can1", sample("Later"));
        stack.add_for_interface("can0", DbcDatabase::parse(
            "BO_ 291 Engine: 2 ECU\n SG_ RPM : 0|8@1+ (2,1) [0|255] \"rpm\" ECU\n SG_ Temp : 15|8@0- (1,-40) [-40|215] \"C\" ECU\n"
        ).unwrap());
        let (rows, total) = stack.catalog_rows("", 20);
        assert_eq!(total, 4);
        assert_eq!(rows.iter().map(|row| row.scope.as_str()).collect::<Vec<_>>(),
            vec!["can0", "can0", "can1", "*"]);
        assert_eq!(rows[0].id, "S:123");
        assert_eq!(rows[0].signal, "RPM");
        assert_eq!(rows[0].scale, "2 * raw + 1");
        assert_eq!(rows[1].layout, "8|8@0-");
        assert_eq!(rows[1].kind, "signed");
        assert_eq!(rows[3].message, "Empty");
        assert_eq!(rows[3].signal, "");
        let (limited, count) = stack.catalog_rows("", 1);
        assert_eq!(count, 4);
        assert_eq!(limited.len(), 1);
        let (none, count) = stack.catalog_rows("", 0);
        assert!(none.is_empty());
        assert_eq!(count, 4);
    }

    #[test]
    fn catalog_search_matches_across_columns_without_merging_scopes() {
        let mut stack = DbcStack::new();
        stack.add_for_interface("can0", DbcDatabase::parse(
            "BO_ 291 Engine: 2 ECU\n SG_ RPM : 0|8@1+ (2,1) [0|255] \"rpm\" ECU\n"
        ).unwrap());
        stack.add_global(DbcDatabase::parse(
            "BO_ 291 Global: 2 ECU\n SG_ RPM : 0|8@1+ (1,0) [0|255] \"rpm\" ECU\n"
        ).unwrap());
        let (rows, total) = stack.catalog_rows("CAN0 s:123 rpm", 10);
        assert_eq!(total, 1);
        assert_eq!(rows[0].message, "Engine");
        assert_eq!(rows[0].scope, "can0");
        let (rows, total) = stack.catalog_rows("global rpm", 10);
        assert_eq!(total, 1);
        assert_eq!(rows[0].scope, "*");
        assert_eq!(stack.catalog_rows("unknown", 10).1, 0);
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
