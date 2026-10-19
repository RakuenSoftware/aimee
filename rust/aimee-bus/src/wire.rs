//! Event frame codec: the fixed 64-byte little-endian header.
//!
//! Written from `docs/proposals/done/event-bus-wire-spec.md` and held to the
//! committed vectors in `src/tests/fixtures/bus/wire_vectors.tsv`. The vectors,
//! not this file, are the authority on byte offsets.

use std::fmt;

/// "BUS0", little-endian.
pub const WIRE_MAGIC: u32 = 0x3053_5542;
/// The encoding version this build speaks.
pub const WIRE_VERSION: u16 = 3;
/// Fixed frame header size.
pub const HDR_LEN: usize = 64;
/// Upper bound on one event's payload.
pub const MAX_PAYLOAD: u32 = 1 << 20;

/// Payload is inline in the ring slot.
pub const F_INLINE: u16 = 0x0001;
/// Payload is an arena lease reference.
pub const F_ARENA: u16 = 0x0002;
/// One-way notification.
pub const F_NOTIFICATION: u16 = 0x0004;
/// Correlated request.
pub const F_REQUEST: u16 = 0x0008;
/// Reply to a correlated request.
pub const F_REPLY: u16 = 0x0010;
/// Cancellation of an outstanding request.
pub const F_CANCEL: u16 = 0x0020;
/// Control-class event (uses the reserved control credits).
pub const F_CONTROL: u16 = 0x0040;
/// A non-final inline fragment of a request or reply.
pub const F_MORE: u16 = 0x0080;

const PLACEMENT_MASK: u16 = F_INLINE | F_ARENA;
const PATTERN_MASK: u16 = F_NOTIFICATION | F_REQUEST | F_REPLY | F_CANCEL;
const KNOWN_MASK: u16 = PLACEMENT_MASK | PATTERN_MASK | F_CONTROL | F_MORE;

/// Reserved event kinds; module kinds start at [`KIND_MODULE_BASE`].
pub const KIND_ATTACH_REQUEST: u32 = 1;
pub const KIND_ATTACH_REPLY: u32 = 2;
pub const KIND_ERROR: u32 = 3;
pub const KIND_CAPABILITY_ABSENT: u32 = 4;
pub const KIND_OVERFLOW: u32 = 5;
pub const KIND_PRODUCER_REAPED: u32 = 6;
pub const KIND_EPOCH_CHANGE: u32 = 7;
pub const KIND_CAPABILITY_DENIED: u32 = 8;
/// Client -> host (D12): return an arena consumer ref. A control-class
/// notification whose inline payload is [`LEASE_RELEASE_LEN`] bytes: lease id
/// then generation, both u32 little-endian. The host consumes it.
pub const KIND_LEASE_RELEASE: u32 = 9;
/// Inline payload length of a [`KIND_LEASE_RELEASE`] event.
pub const LEASE_RELEASE_LEN: usize = 8;
pub const KIND_MODULE_BASE: u32 = 256;

/// A decoded event header.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Frame {
    pub hdr_flags: u16,
    pub wire_version: u16,
    pub event_kind: u32,
    pub principal_ref: u32,
    pub correlation_id: u64,
    pub seq: u64,
    pub logical_ts: u64,
    pub payload_ref: u64,
    pub payload_len: u32,
    pub src_handle: u32,
    pub dst_handle: u32,
    /// Arena lease generation; zero for every non-arena frame.
    pub generation: u32,
}

/// Why a frame was refused. The names are part of the cross-language
/// contract: the vectors compare against them.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WireError {
    Short,
    Magic,
    Version,
    Flags,
    PayloadLen,
    Correlation,
    Reserved,
}

impl WireError {
    /// The contract name, as written in the vector table.
    pub fn name(self) -> &'static str {
        match self {
            WireError::Short => "ERR_SHORT",
            WireError::Magic => "ERR_MAGIC",
            WireError::Version => "ERR_VERSION",
            WireError::Flags => "ERR_FLAGS",
            WireError::PayloadLen => "ERR_PAYLOAD_LEN",
            WireError::Correlation => "ERR_CORRELATION",
            WireError::Reserved => "ERR_RESERVED",
        }
    }
}

impl fmt::Display for WireError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.name())
    }
}

impl std::error::Error for WireError {}

fn exactly_one(v: u16) -> bool {
    v.is_power_of_two()
}

fn le_u16(b: &[u8], o: usize) -> u16 {
    u16::from_le_bytes([b[o], b[o + 1]])
}

fn le_u32(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]])
}

fn le_u64(b: &[u8], o: usize) -> u64 {
    let mut a = [0u8; 8];
    a.copy_from_slice(&b[o..o + 8]);
    u64::from_le_bytes(a)
}

impl Frame {
    /// Apply the frame rules: version, known flags, exactly one pattern,
    /// placement consistent with the payload, fragment and generation rules,
    /// and correlation by pattern.
    pub fn validate(&self) -> Result<(), WireError> {
        if self.wire_version != WIRE_VERSION {
            return Err(WireError::Version);
        }
        if self.hdr_flags & !KNOWN_MASK != 0 || !exactly_one(self.hdr_flags & PATTERN_MASK) {
            return Err(WireError::Flags);
        }
        let placement = self.hdr_flags & PLACEMENT_MASK;
        if self.payload_len > 0 {
            if !exactly_one(placement) {
                return Err(WireError::Flags);
            }
            if self.payload_len > MAX_PAYLOAD {
                return Err(WireError::PayloadLen);
            }
        } else {
            if placement != 0 {
                return Err(WireError::Flags);
            }
            if self.payload_ref != 0 {
                return Err(WireError::PayloadLen);
            }
        }
        if self.hdr_flags & F_MORE != 0 {
            let pattern = self.hdr_flags & PATTERN_MASK;
            if (pattern != F_REQUEST && pattern != F_REPLY)
                || placement != F_INLINE
                || self.payload_len == 0
            {
                return Err(WireError::Flags);
            }
        }
        if self.hdr_flags & F_ARENA == 0 && self.generation != 0 {
            return Err(WireError::Flags);
        }
        if self.hdr_flags & F_NOTIFICATION != 0 {
            if self.correlation_id != 0 {
                return Err(WireError::Correlation);
            }
        } else if self.correlation_id == 0 {
            return Err(WireError::Correlation);
        }
        Ok(())
    }

    /// Encode into the first [`HDR_LEN`] bytes of `out`. The encoder refuses
    /// a frame the decoder would refuse.
    pub fn encode(&self, out: &mut [u8]) -> Result<(), WireError> {
        if out.len() < HDR_LEN {
            return Err(WireError::Short);
        }
        self.validate()?;
        let b = &mut out[..HDR_LEN];
        b[0..4].copy_from_slice(&WIRE_MAGIC.to_le_bytes());
        b[4..6].copy_from_slice(&self.hdr_flags.to_le_bytes());
        b[6..8].copy_from_slice(&self.wire_version.to_le_bytes());
        b[8..12].copy_from_slice(&self.event_kind.to_le_bytes());
        b[12..16].copy_from_slice(&self.principal_ref.to_le_bytes());
        b[16..24].copy_from_slice(&self.correlation_id.to_le_bytes());
        b[24..32].copy_from_slice(&self.seq.to_le_bytes());
        b[32..40].copy_from_slice(&self.logical_ts.to_le_bytes());
        b[40..48].copy_from_slice(&self.payload_ref.to_le_bytes());
        b[48..52].copy_from_slice(&self.payload_len.to_le_bytes());
        b[52..56].copy_from_slice(&self.src_handle.to_le_bytes());
        b[56..60].copy_from_slice(&self.dst_handle.to_le_bytes());
        b[60..64].copy_from_slice(&self.generation.to_le_bytes());
        Ok(())
    }

    /// Decode and validate. A refused frame yields no partial value.
    pub fn decode(b: &[u8]) -> Result<Frame, WireError> {
        if b.len() < HDR_LEN {
            return Err(WireError::Short);
        }
        if le_u32(b, 0) != WIRE_MAGIC {
            return Err(WireError::Magic);
        }
        let f = Frame {
            hdr_flags: le_u16(b, 4),
            wire_version: le_u16(b, 6),
            event_kind: le_u32(b, 8),
            principal_ref: le_u32(b, 12),
            correlation_id: le_u64(b, 16),
            seq: le_u64(b, 24),
            logical_ts: le_u64(b, 32),
            payload_ref: le_u64(b, 40),
            payload_len: le_u32(b, 48),
            src_handle: le_u32(b, 52),
            dst_handle: le_u32(b, 56),
            generation: le_u32(b, 60),
        };
        f.validate()?;
        Ok(f)
    }

    /// Bound a payload reference against the live geometry. For an inline
    /// payload the span must lie after the header and inside the slot. For an
    /// arena reference only the length is bounded; the live-lease check is the
    /// host's.
    pub fn check_placement(&self, slot_size: u32, arena_size: u64) -> Result<(), WireError> {
        self.validate()?;
        if self.payload_len == 0 {
            return Ok(());
        }
        if self.hdr_flags & F_INLINE != 0 {
            let end = self
                .payload_ref
                .checked_add(u64::from(self.payload_len))
                .ok_or(WireError::PayloadLen)?;
            if self.payload_ref < HDR_LEN as u64 || end > u64::from(slot_size) {
                return Err(WireError::PayloadLen);
            }
        } else if u64::from(self.payload_len) > arena_size {
            return Err(WireError::PayloadLen);
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn note() -> Frame {
        Frame {
            hdr_flags: F_NOTIFICATION,
            wire_version: WIRE_VERSION,
            event_kind: 256,
            ..Default::default()
        }
    }

    #[test]
    fn encode_refuses_what_decode_refuses() {
        let mut out = [0u8; HDR_LEN];
        let bad = Frame {
            correlation_id: 9,
            ..note()
        };
        assert_eq!(bad.encode(&mut out), Err(WireError::Correlation));
        assert_eq!(
            note().encode(&mut out[..HDR_LEN - 1]),
            Err(WireError::Short)
        );
    }

    #[test]
    fn roundtrip() {
        let f = Frame {
            hdr_flags: F_REPLY | F_INLINE | F_MORE,
            wire_version: WIRE_VERSION,
            event_kind: 5889,
            principal_ref: 7,
            correlation_id: u64::MAX,
            seq: 1,
            logical_ts: 2,
            payload_ref: HDR_LEN as u64,
            payload_len: 10,
            src_handle: 3,
            dst_handle: 4,
            generation: 0,
        };
        let mut out = [0u8; HDR_LEN];
        f.encode(&mut out).unwrap();
        assert_eq!(Frame::decode(&out).unwrap(), f);
    }

    #[test]
    fn placement_bounds() {
        let f = Frame {
            hdr_flags: F_NOTIFICATION | F_INLINE,
            payload_ref: HDR_LEN as u64,
            payload_len: 192,
            ..note()
        };
        assert!(f.check_placement(256, 1024).is_ok());
        assert_eq!(f.check_placement(255, 1024), Err(WireError::PayloadLen));
        let under = Frame {
            payload_ref: 8,
            ..f
        };
        assert_eq!(under.check_placement(256, 1024), Err(WireError::PayloadLen));
        let wrap = Frame {
            payload_ref: u64::MAX,
            ..f
        };
        assert_eq!(wrap.check_placement(256, 1024), Err(WireError::PayloadLen));
        let arena = Frame {
            hdr_flags: F_NOTIFICATION | F_ARENA,
            payload_ref: 1,
            payload_len: 2048,
            generation: 1,
            ..note()
        };
        assert_eq!(arena.check_placement(256, 1024), Err(WireError::PayloadLen));
        assert!(arena.check_placement(256, 4096).is_ok());
    }

    #[test]
    fn oversized_payload() {
        let f = Frame {
            hdr_flags: F_NOTIFICATION | F_ARENA,
            payload_ref: 1,
            payload_len: MAX_PAYLOAD + 1,
            generation: 1,
            ..note()
        };
        assert_eq!(f.validate(), Err(WireError::PayloadLen));
    }
}
