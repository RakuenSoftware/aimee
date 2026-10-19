//! Views of the control and queue-pair regions.
//!
//! Layouts are `bus_control_t` and `bus_qpair_hdr_t`, each 96 bytes and
//! frozen by `layout_version` in
//! `src/core/event_bus/include/aimee/core/event_bus/bus_region.h`.

use crate::error::BusError;
use crate::ring::{RING_HDR, Ring, ring_bytes};
use std::ptr::NonNull;
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};

use crate::layout::{ARENA_HDR, CTL_BYTES, CTL_BYTES_WITH_VIEW};
use crate::layout::{
    ARENA_MAGIC, ARENA_OFF_MAGIC, ARENA_OFF_SIZE, CTL_F_LEASE_VIEW, CTL_MAGIC, CTL_OFF_ARENA,
    CTL_OFF_EPOCH, CTL_OFF_FLAGS, CTL_OFF_INLINE, CTL_OFF_LAYOUT, CTL_OFF_LEASE_VIEW_OFF,
    CTL_OFF_LEASE_VIEW_SLOTS, CTL_OFF_MAGIC, CTL_OFF_QUEUE_CAP, CTL_OFF_SLOT, CTL_OFF_SPEC,
    LAYOUT_VERSION, LEASE_OFF_GENERATION, LEASE_OFF_LEN, LEASE_OFF_OFFSET, LEASE_VIEW_ENTRY,
    LEASE_VIEW_SLOTS, QP_BYTES, QP_MAGIC, QP_OFF_CAP, QP_OFF_CONTROL_LOST, QP_OFF_HEARTBEAT,
    QP_OFF_INBOUND, QP_OFF_MAGIC, QP_OFF_OUTBOUND, QP_OFF_SLOT, QP_RING_ALIGN, SPEC_VERSION,
};

fn align64(n: usize) -> usize {
    (n + QP_RING_ALIGN - 1) & !(QP_RING_ALIGN - 1)
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

/// Check the arena region's header (`bus_arena_hdr_t`): the host's magic, and
/// the usable size the attach reply announced. The C client refuses a
/// mismatched arena the same way (`bus_arena_region_attach`); without this a
/// client would trust an arena it cannot read safely.
///
/// # Safety
/// `base..base+len` must be a live mapping.
pub(crate) unsafe fn check_arena(
    base: NonNull<u8>,
    len: usize,
    announced: u64,
) -> Result<(), BusError> {
    if len < ARENA_HDR || base.as_ptr() as usize % 8 != 0 {
        return Err(BusError::Region("arena region short or misaligned"));
    }
    if load_u32(base, ARENA_OFF_MAGIC, Ordering::Acquire) != ARENA_MAGIC {
        return Err(BusError::Region("arena magic"));
    }
    let size = load_u64(base, ARENA_OFF_SIZE, Ordering::Acquire);
    if size != announced || (len - ARENA_HDR) as u64 != size {
        return Err(BusError::Region(
            "arena size disagrees with the attach reply",
        ));
    }
    Ok(())
}

/// Read-only view of the control region.
#[derive(Debug)]
pub(crate) struct Control {
    base: NonNull<u8>,
    /// The host advertises the lease view and the mapping covers it.
    with_view: bool,
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
        if load_u32(base, CTL_OFF_MAGIC, Ordering::Acquire) != CTL_MAGIC {
            return Err(BusError::Region("control magic"));
        }
        if load_u32(base, CTL_OFF_SPEC, Ordering::Relaxed) != SPEC_VERSION
            || load_u32(base, CTL_OFF_LAYOUT, Ordering::Relaxed) != LAYOUT_VERSION
        {
            return Err(BusError::Region("control spec or layout version"));
        }
        let flags = load_u32(base, CTL_OFF_FLAGS, Ordering::Acquire);
        let with_view = flags & CTL_F_LEASE_VIEW != 0 && len >= CTL_BYTES_WITH_VIEW;
        if flags & CTL_F_LEASE_VIEW != 0
            && (load_u32(base, CTL_OFF_LEASE_VIEW_SLOTS, Ordering::Relaxed) as usize
                != LEASE_VIEW_SLOTS
                || load_u64(base, CTL_OFF_LEASE_VIEW_OFF, Ordering::Relaxed) != CTL_BYTES as u64)
        {
            return Err(BusError::Region("control lease view advertisement"));
        }
        let c = Control { base, with_view };
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

    /// Whether arena payloads can be resolved through the lease view (D12).
    pub(crate) fn has_lease_view(&self) -> bool {
        self.with_view
    }

    /// Resolve an arena frame's (lease id, generation, payload length) to an
    /// offset into the arena's usable bytes. Every value is a claim: the entry
    /// must be live with the frame's generation, the payload must fit the span,
    /// and the span must lie inside the arena. The acquire load of generation
    /// pairs with the host's release store, made after offset and len.
    pub(crate) fn resolve_lease(
        &self,
        lease: u32,
        generation: u32,
        payload_len: u32,
        arena_size: u64,
    ) -> Option<u64> {
        if !self.with_view || generation == 0 || payload_len == 0 {
            return None;
        }
        let lease = lease as usize;
        if lease >= LEASE_VIEW_SLOTS {
            return None;
        }
        let e = CTL_BYTES + lease * LEASE_VIEW_ENTRY;
        if load_u32(self.base, e + LEASE_OFF_GENERATION, Ordering::Acquire) != generation {
            return None;
        }
        // Seqlock read (the host's writer is view_publish): acquire on the span
        // pairs with its release stores, so a span written for a later lease
        // makes the re-read see that lease's generation, or 0, and fail.
        let span = load_u32(self.base, e + LEASE_OFF_LEN, Ordering::Acquire);
        let offset = load_u64(self.base, e + LEASE_OFF_OFFSET, Ordering::Acquire);
        if load_u32(self.base, e + LEASE_OFF_GENERATION, Ordering::Relaxed) != generation {
            return None;
        }
        if payload_len > span || offset > arena_size || arena_size - offset < u64::from(payload_len)
        {
            return None;
        }
        Some(offset)
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
        if load_u32(base, QP_OFF_MAGIC, Ordering::Acquire) != QP_MAGIC {
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
