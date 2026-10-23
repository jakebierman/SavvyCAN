//! Deterministic, strictly offline trace replay cursor.
//! This module only indexes already-loaded frames. It never uses a CAN transport.
use crate::{CanFrame, TraceFilter};

/// How matching frames are spaced in an offline replay.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ReplayClock {
    /// All matching frames had source timestamps, which are used as offsets.
    SourceTimestamps,
    /// At least one matching frame lacked a timestamp, so every matching
    /// frame is spaced by 10 ms instead of mixing incompatible time bases.
    FixedTenMilliseconds,
}

/// A playback cursor over a filtered snapshot of a loaded trace.
///
/// Replaying the same input/filter with the same tick sequence produces
/// exactly the same positions. This is a read-only visualization utility.
#[derive(Debug, Clone)]
pub struct OfflineReplay {
    indices: Vec<usize>,
    offsets_us: Vec<u64>,
    cursor: usize,
    elapsed_us: u64,
    clock: ReplayClock,
}

impl OfflineReplay {
    pub fn new(frames: &[CanFrame], filter: &TraceFilter) -> Self {
        let matching = frames.iter().enumerate()
            .filter(|(_, frame)| filter.matches(frame))
            .collect::<Vec<_>>();
        let all_timestamps = matching.iter().all(|(_, frame)| frame.timestamp_us.is_some());
        let clock = if all_timestamps {
            ReplayClock::SourceTimestamps
        } else {
            ReplayClock::FixedTenMilliseconds
        };
        let mut indices = Vec::with_capacity(matching.len());
        let mut offsets_us = Vec::with_capacity(matching.len());
        let initial = matching.first().and_then(|(_, frame)| frame.timestamp_us).unwrap_or(0);
        let mut last_offset = 0u64;
        for (position, (index, frame)) in matching.into_iter().enumerate() {
            let offset = if all_timestamps {
                frame.timestamp_us.expect("validated timestamp presence")
                    .saturating_sub(initial).max(last_offset)
            } else {
                (position as u64).saturating_mul(10_000)
            };
            indices.push(index);
            offsets_us.push(offset);
            last_offset = offset;
        }
        Self { indices, offsets_us, cursor: 0, elapsed_us: 0, clock }
    }

    pub fn clock(&self) -> ReplayClock { self.clock }
    pub fn total(&self) -> usize { self.indices.len() }
    pub fn position(&self) -> usize { self.cursor }
    pub fn elapsed_us(&self) -> u64 { self.elapsed_us }
    pub fn duration_us(&self) -> u64 { self.offsets_us.last().copied().unwrap_or(0) }
    pub fn finished(&self) -> bool { self.cursor >= self.indices.len() }
    pub fn reset(&mut self) {
        self.cursor = 0;
        self.elapsed_us = 0;
    }

    /// Advance by logical replay time (the caller controls the playback rate).
    /// The first matching frame becomes visible at time zero.
    pub fn advance(&mut self, delta_us: u64) -> usize {
        self.elapsed_us = self.elapsed_us.saturating_add(delta_us).min(self.duration_us());
        let before = self.cursor;
        while self.cursor < self.indices.len()
            && self.offsets_us[self.cursor] <= self.elapsed_us
        {
            self.cursor += 1;
        }
        self.cursor - before
    }

    /// Reveal the next N matching frames even if their timestamps coincide.
    pub fn step_frames(&mut self, count: usize) -> usize {
        let before = self.cursor;
        self.cursor = self.cursor.saturating_add(count).min(self.indices.len());
        if self.cursor > before {
            self.elapsed_us = self.offsets_us[self.cursor - 1];
        }
        self.cursor - before
    }

    /// Indices into the original (unmodified) trace of the most recently
    /// revealed frames. Empty until playback has started or been stepped.
    pub fn recent_indices(&self, limit: usize) -> &[usize] {
        let first = self.cursor.saturating_sub(limit);
        &self.indices[first..self.cursor]
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{CanId, FdFlags, FrameKind};

    fn frame(time: Option<u64>, id: u16) -> CanFrame {
        CanFrame::new(time, "can0", CanId::Standard(id), FrameKind::Data,
            vec![1], false, FdFlags::default()).unwrap()
    }

    #[test]
    fn timestamp_timing_and_filtered_indices() {
        let frames = vec![frame(Some(1_000_000), 0x123), frame(Some(1_010_000), 0x456),
            frame(Some(1_050_000), 0x123), frame(Some(1_070_000), 0x123)];
        let filter = TraceFilter::parse("", "S:123").unwrap();
        let mut replay = OfflineReplay::new(&frames, &filter);
        assert_eq!(replay.clock(), ReplayClock::SourceTimestamps);
        assert_eq!(replay.total(), 3);
        assert_eq!(replay.duration_us(), 70_000);
        assert_eq!(replay.advance(0), 1);
        assert_eq!(replay.recent_indices(100), &[0]);
        assert_eq!(replay.advance(49_999), 0);
        assert_eq!(replay.advance(1), 1);
        assert_eq!(replay.recent_indices(1), &[2]);
        assert_eq!(replay.advance(20_000), 1);
        assert!(replay.finished());
        assert_eq!(replay.recent_indices(2), &[2, 3]);
        replay.reset();
        assert_eq!((replay.position(), replay.elapsed_us()), (0, 0));
        assert!(!replay.finished());
    }

    #[test]
    fn missing_timestamps_use_unified_fixed_clock() {
        let frames = vec![frame(Some(1), 0x123), frame(None, 0x123),
            frame(Some(100_000_000), 0x123)];
        let mut replay = OfflineReplay::new(&frames, &TraceFilter::default());
        assert_eq!(replay.clock(), ReplayClock::FixedTenMilliseconds);
        assert_eq!(replay.duration_us(), 20_000);
        assert_eq!(replay.step_frames(2), 2);
        assert_eq!(replay.recent_indices(10), &[0, 1]);
        assert_eq!(replay.advance(10_000), 1);
        assert!(replay.finished());
    }

    #[test]
    fn regressing_timestamps_never_move_clock_backwards() {
        let frames = vec![frame(Some(1_000), 1), frame(Some(900), 1),
            frame(Some(1_500), 1)];
        let mut replay = OfflineReplay::new(&frames, &TraceFilter::default());
        assert_eq!(replay.advance(0), 2);
        assert_eq!(replay.advance(499), 0);
        assert_eq!(replay.advance(1), 1);
        assert_eq!(replay.recent_indices(10), &[0, 1, 2]);
    }

    #[test]
    fn empty_replay_is_finished_and_safe() {
        let frames = vec![frame(Some(100), 0x123)];
        let filter = TraceFilter::parse("", "S:124").unwrap();
        let mut replay = OfflineReplay::new(&frames, &filter);
        assert_eq!(replay.total(), 0);
        assert_eq!(replay.advance(u64::MAX), 0);
        assert_eq!(replay.step_frames(10), 0);
        assert_eq!(replay.recent_indices(10), &[]);
        assert!(replay.finished());
    }
}
