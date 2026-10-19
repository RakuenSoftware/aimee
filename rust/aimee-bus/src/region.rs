//! Views of the control and queue-pair regions.
//!
//! Layouts are `bus_control_t` and `bus_qpair_hdr_t`, each 96 bytes and
//! frozen by `layout_version` in
//! `src/core/event_bus/include/aimee/core/event_bus/bus_region.h`.

use crate::error::BusError;
use crate::ring::{RING_HDR, Ring, ring_bytes};
use std::ptr::NonNull;
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};

const CTL_MAGIC: u32 = 0x4c54_4342; // "BCTL"
const CTL_OFF_SPEC: usize = 4;
const CTL_OFF_LAYOUT: usize = 8;
const CTL_OFF_SLOT: usize = 16;
const CTL_OFF_INLINE: usize = 20;
const CTL_OFF_QUEUE_CAP: usize = 24;
const CTL_OFF_ARENA: usize = 32;
const CTL_OFF_EPOCH: usize = 40;
pub(crate) const CTL_BYTES: usize = 96;
const SPEC_VERSION: u32 = 1;
const LAYOUT_VERSION: u32 = 1;

const QP_MAGIC: u32 = 0x5250_4251; // "QPBR"
const QP_OFF_SLOT: usize = 4;
const QP_OFF_CAP: usize = 8;
const QP_OFF_CONTROL_LOST: usize = 20;
const QP_OFF_INBOUND: usize = 24;
const QP_OFF_OUTBOUND: usize = 28;
const QP_OFF_HEARTBEAT: usize = 32;
const QP_BYTES: usize = 96;

/// `bus_arena_hdr_t` precedes the usable arena bytes.
pub(crate) const ARENA_HDR: usize = 16;

fn align64(n: usize) -> usize {
    (n + 63) & !63
}

/// Size of a queue-pair region: header, then two 64-aligned rings
/// (`bus_qpair_bytes`).
pub(crate) fn qpair_bytes(slot_size: u32, capacity: u32) -> usize {
    let ring = ring_bytes(slot_size, capacity);
    let in_off = align64(QP_BYTES);
    let out_off = align64(in_off + ring);
    out_off + ring
}

// Atomic access to a mapped header. Values are copied in and out; no
// reference into the mapping escapes these helpers.

fn load_u32(base: NonNull<u8>, off: usize, order: Ordering) -> u32 {
    // SAFETY: callers pass 4-aligned offsets inside a validated mapping that
    // outlives the view; the reference lives only for this load.
    unsafe { (*(base.as_ptr().add(off) as *const AtomicU32)).load(order) }
}

fn load_u64(base: NonNull<u8>, off: usize, order: Ordering) -> u64 {
    // SAFETY: as load_u32, with 8-aligned offsets.
    unsafe { (*(base.as_ptr().add(off) as *const AtomicU64)).load(order) }
}

fn store_u64(base: NonNull<u8>, off: usize, value: u64, order: Ordering) {
    // SAFETY: as load_u64; the queue-pair mapping is writable.
    unsafe { (*(base.as_ptr().add(off) as *const AtomicU64)).store(value, order) }
}

/// Read-only view of the control region.
#[derive(Debug)]
pub(crate) struct Control {
    base: NonNull<u8>,
}

// SAFETY: read-only atomic loads of a mapping the Client keeps alive.
unsafe impl Send for Control {}
// SAFETY: see Send.
unsafe impl Sync for Control {}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct ControlGeometry {
    pub slot_size: u32,
    pub inline_budget: u32,
    pub queue_capacity: u32,
    pub arena_size: u64,
}

impl Control {
    /// # Safety
    /// `base..base+len` must be a live mapping that outlives the view.
    pub(crate) unsafe fn attach(base: NonNull<u8>, len: usize) -> Result<Control, BusError> {
        if len < CTL_BYTES || base.as_ptr() as usize % 8 != 0 {
            return Err(BusError::Region("control region short or misaligned"));
        }
        if load_u32(base, 0, Ordering::Acquire) != CTL_MAGIC {
            return Err(BusError::Region("control magic"));
        }
        if load_u32(base, CTL_OFF_SPEC, Ordering::Relaxed) != SPEC_VERSION
            || load_u32(base, CTL_OFF_LAYOUT, Ordering::Relaxed) != LAYOUT_VERSION
        {
            return Err(BusError::Region("control spec or layout version"));
        }
        let c = Control { base };
        let g = c.geometry();
        if g.slot_size == 0 || g.inline_budget > g.slot_size || g.arena_size == 0 {
            return Err(BusError::Region("control geometry"));
        }
        Ok(c)
    }

    pub(crate) fn geometry(&self) -> ControlGeometry {
        ControlGeometry {
            slot_size: load_u32(self.base, CTL_OFF_SLOT, Ordering::Relaxed),
            inline_budget: load_u32(self.base, CTL_OFF_INLINE, Ordering::Relaxed),
            queue_capacity: load_u32(self.base, CTL_OFF_QUEUE_CAP, Ordering::Relaxed),
            arena_size: load_u64(self.base, CTL_OFF_ARENA, Ordering::Relaxed),
        }
    }

    /// The current host epoch; a change means the host restarted.
    pub(crate) fn epoch(&self) -> u64 {
        load_u64(self.base, CTL_OFF_EPOCH, Ordering::Acquire)
    }
}

/// View of one client's queue-pair region: two rings plus metadata.
#[derive(Debug)]
pub(crate) struct QueuePair {
    base: NonNull<u8>,
    pub(crate) inbound: Ring,
    pub(crate) outbound: Ring,
}

// SAFETY: as Control; ring access is serialised by the Client.
unsafe impl Send for QueuePair {}
// SAFETY: see Send.
unsafe impl Sync for QueuePair {}

impl QueuePair {
    /// # Safety
    /// `base..base+len` must be a live read/write mapping that outlives the view.
    pub(crate) unsafe fn attach(base: NonNull<u8>, len: usize) -> Result<QueuePair, BusError> {
        if len < QP_BYTES || base.as_ptr() as usize % 64 != 0 {
            return Err(BusError::Region("queue-pair region short or misaligned"));
        }
        if load_u32(base, 0, Ordering::Acquire) != QP_MAGIC {
            return Err(BusError::Region("queue-pair magic"));
        }
        let in_off = load_u32(base, QP_OFF_INBOUND, Ordering::Relaxed) as usize;
        let out_off = load_u32(base, QP_OFF_OUTBOUND, Ordering::Relaxed) as usize;
        if in_off < QP_BYTES
            || out_off < QP_BYTES
            || in_off == out_off
            || in_off >= len
            || out_off >= len
        {
            return Err(BusError::Region("queue-pair ring offsets"));
        }
        let ring_at = |off: usize| {
            // SAFETY: off < len was checked; Ring::attach bounds the rest.
            unsafe { Ring::attach(NonNull::new_unchecked(base.as_ptr().add(off)), len - off) }
        };
        let inbound = ring_at(in_off)?;
        let outbound = ring_at(out_off)?;
        let (lo, lo_ring, hi) = if in_off < out_off {
            (in_off, &inbound, out_off)
        } else {
            (out_off, &outbound, in_off)
        };
        if lo + RING_HDR + lo_ring.slot_size() * lo_ring.capacity() as usize > hi {
            return Err(BusError::Region("queue-pair rings overlap"));
        }
        let slot = load_u32(base, QP_OFF_SLOT, Ordering::Relaxed) as usize;
        let cap = u64::from(load_u32(base, QP_OFF_CAP, Ordering::Relaxed));
        if slot != inbound.slot_size()
            || cap != inbound.capacity()
            || slot != outbound.slot_size()
            || cap != outbound.capacity()
        {
            return Err(BusError::Region(
                "queue-pair header disagrees with its rings",
            ));
        }
        Ok(QueuePair {
            base,
            inbound,
            outbound,
        })
    }

    /// The sticky flag the host sets when a control-class event was shed.
    pub(crate) fn control_lost(&self) -> bool {
        load_u32(self.base, QP_OFF_CONTROL_LOST, Ordering::Acquire) != 0
    }

    /// Advance the liveness value the host reads to decide on a reap.
    pub(crate) fn set_heartbeat(&self, now: u64) {
        store_u64(self.base, QP_OFF_HEARTBEAT, now, Ordering::Release);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn qpair_sizing_matches_layout() {
        // Header rounded to 96 -> 128, ring = 192 + 256*16 = 4288.
        assert_eq!(qpair_bytes(256, 16), 128 + align64(4288) + 4288);
        assert_eq!(align64(96), 128);
    }
}
