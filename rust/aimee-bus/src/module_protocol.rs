//! The AMOD envelope for core <-> process-module calls.
//!
//! The bus frame carries routing, correlation and cancellation; this 40-byte
//! little-endian header describes the operation inside each inline payload.
//! Contract: `src/core/event_bus/include/aimee/core/event_bus/module_protocol.h`.

use crate::layout::*;
use std::fmt;

/// "AMOD", little-endian.
pub const MODULE_MESSAGE_MAGIC: u32 = 0x444f_4d41;
pub const MODULE_MESSAGE_VERSION: u16 = 1;
pub const MODULE_MESSAGE_HEADER_LEN: usize = 40;
/// Largest assembled body, across all fragments of one call.
pub const MODULE_MESSAGE_MAX_BODY: u32 = 16 * 1024 * 1024;

/// What a message asks for.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u16)]
pub enum Operation {
    Invoke = 1,
    Result = 2,
}

/// A stage's outcome. Non-OK results never carry a body.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u16)]
pub enum ModuleStatus {
    Ok = 0,
    CapabilityAbsent = 1,
    Cancelled = 2,
    DeadlineExceeded = 3,
    InvalidRequest = 4,
    Internal = 5,
}

impl ModuleStatus {
    fn from_wire(v: u16) -> Option<ModuleStatus> {
        Some(match v {
            0 => ModuleStatus::Ok,
            1 => ModuleStatus::CapabilityAbsent,
            2 => ModuleStatus::Cancelled,
            3 => ModuleStatus::DeadlineExceeded,
            4 => ModuleStatus::InvalidRequest,
            5 => ModuleStatus::Internal,
            _ => return None,
        })
    }
}

/// Why an envelope was refused (`aimee_module_message_result_t`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MessageError {
    Short,
    Magic,
    Version,
    Header,
    Operation,
    Status,
    Stage,
    Flags,
    Body,
}

impl MessageError {
    /// The contract name, as `aimee_module_message_result_name` spells it.
    pub fn name(self) -> &'static str {
        match self {
            MessageError::Short => "ERR_SHORT",
            MessageError::Magic => "ERR_MAGIC",
            MessageError::Version => "ERR_VERSION",
            MessageError::Header => "ERR_HEADER",
            MessageError::Operation => "ERR_OPERATION",
            MessageError::Status => "ERR_STATUS",
            MessageError::Stage => "ERR_STAGE",
            MessageError::Flags => "ERR_FLAGS",
            MessageError::Body => "ERR_BODY",
        }
    }
}

impl fmt::Display for MessageError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.name())
    }
}

impl std::error::Error for MessageError {}

/// The envelope header. The body follows it in the same inline payload.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ModuleMessage {
    pub operation: Operation,
    pub status: ModuleStatus,
    pub stage_id: u32,
    pub flags: u32,
    pub body_len: u32,
    /// Absolute `CLOCK_MONOTONIC` nanoseconds; zero means no deadline.
    pub deadline_ns: u64,
    pub trace_id: u64,
}

impl ModuleMessage {
    fn validate(&self) -> Result<(), MessageError> {
        if self.operation == Operation::Invoke && self.status != ModuleStatus::Ok {
            return Err(MessageError::Status);
        }
        if self.stage_id == 0 {
            return Err(MessageError::Stage);
        }
        if self.flags != 0 {
            return Err(MessageError::Flags);
        }
        if self.body_len > MODULE_MESSAGE_MAX_BODY {
            return Err(MessageError::Body);
        }
        Ok(())
    }

    /// Encode the header into the first [`MODULE_MESSAGE_HEADER_LEN`] bytes.
    pub fn encode(&self, out: &mut [u8]) -> Result<(), MessageError> {
        if out.len() < MODULE_MESSAGE_HEADER_LEN {
            return Err(MessageError::Short);
        }
        self.validate()?;
        let b = &mut out[..MODULE_MESSAGE_HEADER_LEN];
        b.fill(0);
        b[AMOD_OFF_MAGIC..][..4].copy_from_slice(&MODULE_MESSAGE_MAGIC.to_le_bytes());
        b[AMOD_OFF_VERSION..][..2].copy_from_slice(&MODULE_MESSAGE_VERSION.to_le_bytes());
        b[AMOD_OFF_HEADER_LEN..][..2]
            .copy_from_slice(&(MODULE_MESSAGE_HEADER_LEN as u16).to_le_bytes());
        b[AMOD_OFF_OPERATION..][..2].copy_from_slice(&(self.operation as u16).to_le_bytes());
        b[AMOD_OFF_STATUS..][..2].copy_from_slice(&(self.status as u16).to_le_bytes());
        b[AMOD_OFF_STAGE..][..4].copy_from_slice(&self.stage_id.to_le_bytes());
        b[AMOD_OFF_FLAGS..][..4].copy_from_slice(&self.flags.to_le_bytes());
        b[AMOD_OFF_BODY_LEN..][..4].copy_from_slice(&self.body_len.to_le_bytes());
        b[AMOD_OFF_DEADLINE..][..8].copy_from_slice(&self.deadline_ns.to_le_bytes());
        b[AMOD_OFF_TRACE..][..8].copy_from_slice(&self.trace_id.to_le_bytes());
        Ok(())
    }

    /// Decode a header and check that its whole declared body is present.
    /// Errors are reported in the C decoder's order.
    pub fn decode(b: &[u8]) -> Result<ModuleMessage, MessageError> {
        if b.len() < MODULE_MESSAGE_HEADER_LEN {
            return Err(MessageError::Short);
        }
        let u16_at = |o: usize| u16::from_le_bytes([b[o], b[o + 1]]);
        let u32_at = |o: usize| u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]]);
        let u64_at = |o: usize| u64::from(u32_at(o)) | (u64::from(u32_at(o + 4)) << 32);
        if u32_at(AMOD_OFF_MAGIC) != MODULE_MESSAGE_MAGIC {
            return Err(MessageError::Magic);
        }
        if u16_at(AMOD_OFF_VERSION) != MODULE_MESSAGE_VERSION {
            return Err(MessageError::Version);
        }
        if usize::from(u16_at(AMOD_OFF_HEADER_LEN)) != MODULE_MESSAGE_HEADER_LEN {
            return Err(MessageError::Header);
        }
        let operation = match u16_at(AMOD_OFF_OPERATION) {
            1 => Operation::Invoke,
            2 => Operation::Result,
            _ => return Err(MessageError::Operation),
        };
        let status =
            ModuleStatus::from_wire(u16_at(AMOD_OFF_STATUS)).ok_or(MessageError::Status)?;
        let m = ModuleMessage {
            operation,
            status,
            stage_id: u32_at(AMOD_OFF_STAGE),
            flags: u32_at(AMOD_OFF_FLAGS),
            body_len: u32_at(AMOD_OFF_BODY_LEN),
            deadline_ns: u64_at(AMOD_OFF_DEADLINE),
            trace_id: u64_at(AMOD_OFF_TRACE),
        };
        m.validate()?;
        if m.body_len as usize > b.len() - MODULE_MESSAGE_HEADER_LEN {
            return Err(MessageError::Body);
        }
        Ok(m)
    }

    /// The body bytes that follow this header in `payload`.
    pub fn body<'a>(&self, payload: &'a [u8]) -> &'a [u8] {
        &payload[MODULE_MESSAGE_HEADER_LEN..MODULE_MESSAGE_HEADER_LEN + self.body_len as usize]
    }

    /// Whether an absolute deadline has passed at `now_ns`.
    pub fn deadline_expired(&self, now_ns: u64) -> bool {
        self.deadline_ns != 0 && now_ns >= self.deadline_ns
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn invoke() -> ModuleMessage {
        ModuleMessage {
            operation: Operation::Invoke,
            status: ModuleStatus::Ok,
            stage_id: 1,
            flags: 0,
            body_len: 3,
            deadline_ns: 0x0102_0304_0506_0708,
            trace_id: 0xfeed,
        }
    }

    #[test]
    fn roundtrip_and_bytes() {
        let mut b = vec![0u8; MODULE_MESSAGE_HEADER_LEN + 3];
        invoke().encode(&mut b).unwrap();
        b[40..].copy_from_slice(b"abc");
        assert_eq!(&b[0..8], &[0x41, 0x4d, 0x4f, 0x44, 1, 0, 40, 0]);
        let m = ModuleMessage::decode(&b).unwrap();
        assert_eq!(m, invoke());
        assert_eq!(m.body(&b), b"abc");
    }

    #[test]
    fn refusals_in_c_order() {
        let mut good = vec![0u8; MODULE_MESSAGE_HEADER_LEN + 3];
        invoke().encode(&mut good).unwrap();
        let with = |off: usize, bytes: &[u8]| {
            let mut b = good.clone();
            b[off..off + bytes.len()].copy_from_slice(bytes);
            ModuleMessage::decode(&b)
        };
        assert_eq!(ModuleMessage::decode(&good[..39]), Err(MessageError::Short));
        assert_eq!(with(0, &[0, 0, 0, 0]), Err(MessageError::Magic));
        assert_eq!(with(4, &[2, 0]), Err(MessageError::Version));
        assert_eq!(with(6, &[41, 0]), Err(MessageError::Header));
        assert_eq!(with(8, &[3, 0]), Err(MessageError::Operation));
        assert_eq!(with(10, &[6, 0]), Err(MessageError::Status));
        assert_eq!(
            with(10, &[1, 0]),
            Err(MessageError::Status),
            "invoke must be OK"
        );
        assert_eq!(with(12, &[0, 0, 0, 0]), Err(MessageError::Stage));
        assert_eq!(with(16, &[1, 0, 0, 0]), Err(MessageError::Flags));
        assert_eq!(
            with(20, &[4, 0, 0, 0]),
            Err(MessageError::Body),
            "body past the payload"
        );
        assert_eq!(
            with(20, &[1, 0, 0, 1]),
            Err(MessageError::Body),
            "body past the limit"
        );
    }

    #[test]
    fn deadline() {
        let m = invoke();
        assert!(!m.deadline_expired(m.deadline_ns - 1));
        assert!(m.deadline_expired(m.deadline_ns));
        assert!(
            !ModuleMessage {
                deadline_ns: 0,
                ..m
            }
            .deadline_expired(u64::MAX)
        );
    }
}
