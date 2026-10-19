//! A view onto one single-producer/single-consumer ring in shared memory.
//!
//! Layout (`bus_ring_shared_t`, frozen by the C static asserts in
//! `src/core/event_bus/include/aimee/core/event_bus/bus_ring.h`): a 32-byte
//! header of `u32` fields led by the magic, `head` on the second cache line,
//! `tail` on the third, and the slot array from offset 192. `full` and `empty`
//! are told apart by the index pair, never by a count.

use crate::error::BusError;
use std::ptr::NonNull;
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};

pub(crate) const RING_MAGIC: u32 = 0x474e_4952; // "RING"
const OFF_SLOT: usize = 4;
const OFF_CAP: usize = 8;
const OFF_MASK: usize = 12;
const OFF_SLOTS: usize = 16;
const OFF_HEAD: usize = 64;
const OFF_TAIL: usize = 128;
/// Byte offset of the slot array, and the size of the ring header.
pub(crate) const RING_HDR: usize = 192;

/// Bytes a ring of this geometry occupies.
pub(crate) fn ring_bytes(slot_size: u32, capacity: u32) -> usize {
    RING_HDR + slot_size as usize * capacity as usize
}

#[derive(Debug)]
pub(crate) struct Ring {
    base: NonNull<u8>,
    slot_size: usize,
    capacity: u64,
    mask: u64,
}

// SAFETY: a Ring is a view into a MAP_SHARED mapping that outlives it (the
// owning Client holds the mapping). Every cross-process field is accessed
// through atomics, and the Client serialises the single producer and the
// single consumer with its own locks.
unsafe impl Send for Ring {}
// SAFETY: see the Send impl; shared access only performs atomic loads.
unsafe impl Sync for Ring {}

impl Ring {
    /// Adopt a ring laid out at `base`, validating every geometry field
    /// against the `avail` bytes actually mapped.
    ///
    /// # Safety
    /// `base..base+avail` must be a live, readable and writable mapping that
    /// outlives the returned ring.
    pub(crate) unsafe fn attach(base: NonNull<u8>, avail: usize) -> Result<Ring, BusError> {
        if avail < RING_HDR || base.as_ptr() as usize % 64 != 0 {
            return Err(BusError::Region("ring header short or misaligned"));
        }
        // SAFETY: the header lies within `avail` and is 64-byte aligned.
        let (magic, slot, cap, mask, slots_off, head, tail) = unsafe {
            let p = base.as_ptr();
            let u32_at = |o: usize| (*(p.add(o) as *const AtomicU32)).load(Ordering::Acquire);
            let u64_at = |o: usize| (*(p.add(o) as *const AtomicU64)).load(Ordering::Acquire);
            (
                u32_at(0),
                u32_at(OFF_SLOT),
                u32_at(OFF_CAP),
                u32_at(OFF_MASK),
                u32_at(OFF_SLOTS),
                u64_at(OFF_HEAD),
                u64_at(OFF_TAIL),
            )
        };
        if magic != RING_MAGIC
            || cap < 2
            || !cap.is_power_of_two()
            || slot == 0
            || mask != cap - 1
            || slots_off as usize != RING_HDR
        {
            return Err(BusError::Region("ring geometry invalid"));
        }
        if avail < ring_bytes(slot, cap) {
            return Err(BusError::Region("ring larger than its mapping"));
        }
        if head.wrapping_sub(tail) > u64::from(cap) {
            return Err(BusError::Region("ring indices inconsistent"));
        }
        Ok(Ring {
            base,
            slot_size: slot as usize,
            capacity: u64::from(cap),
            mask: u64::from(mask),
        })
    }

    pub(crate) fn slot_size(&self) -> usize {
        self.slot_size
    }

    pub(crate) fn capacity(&self) -> u64 {
        self.capacity
    }

    fn index(&self, off: usize) -> &AtomicU64 {
        // SAFETY: OFF_HEAD and OFF_TAIL are 8-byte aligned inside the
        // validated header, and the mapping outlives self.
        unsafe { &*(self.base.as_ptr().add(off) as *const AtomicU64) }
    }

    /// Occupied slots (a snapshot).
    #[cfg(test)]
    pub(crate) fn count(&self) -> u64 {
        self.index(OFF_HEAD)
            .load(Ordering::Acquire)
            .wrapping_sub(self.index(OFF_TAIL).load(Ordering::Acquire))
    }

    fn slot_ptr(&self, idx: u64) -> *mut u8 {
        let off = RING_HDR + (idx & self.mask) as usize * self.slot_size;
        // SAFETY: idx is masked into [0, capacity), and attach checked that
        // the whole slot array is mapped.
        unsafe { self.base.as_ptr().add(off) }
    }

    /// Write one slot if there is room: `parts` are copied in order from
    /// the start of the slot. The slot becomes visible to the consumer only
    /// afterwards, through a release store on head. Returns false when the
    /// ring is full.
    ///
    /// The slot is shared memory another process maps, so it is only ever
    /// touched through raw-pointer copies, never through a Rust reference: a
    /// misbehaving peer writing concurrently corrupts bytes, as the C
    /// contract says, rather than causing undefined behaviour here.
    ///
    /// Callers must be the ring's only producer.
    pub(crate) fn produce(&self, parts: &[&[u8]]) -> bool {
        let total: usize = parts.iter().map(|p| p.len()).sum();
        assert!(total <= self.slot_size, "slot overrun");
        let head = self.index(OFF_HEAD).load(Ordering::Relaxed);
        let tail = self.index(OFF_TAIL).load(Ordering::Acquire);
        if head.wrapping_sub(tail) >= self.capacity {
            return false;
        }
        let dst = self.slot_ptr(head);
        let mut off = 0;
        for part in parts {
            // SAFETY: off + part.len() <= total <= slot_size, inside slot
            // `head`, which the producer owns until it publishes it.
            unsafe { std::ptr::copy_nonoverlapping(part.as_ptr(), dst.add(off), part.len()) };
            off += part.len();
        }
        self.index(OFF_HEAD)
            .store(head.wrapping_add(1), Ordering::Release);
        true
    }

    /// Copy the oldest slot into `out` (which must hold a whole slot) and
    /// release it. Returns false when the ring is empty.
    ///
    /// Callers must be the ring's only consumer.
    pub(crate) fn consume(&self, out: &mut [u8]) -> bool {
        assert!(out.len() >= self.slot_size, "buffer smaller than a slot");
        let tail = self.index(OFF_TAIL).load(Ordering::Relaxed);
        let head = self.index(OFF_HEAD).load(Ordering::Acquire);
        if head == tail {
            return false;
        }
        // SAFETY: the slot at tail was published by the producer's release
        // store on head, which the acquire load above observed; out is large
        // enough and does not overlap the mapping.
        unsafe {
            std::ptr::copy_nonoverlapping(self.slot_ptr(tail), out.as_mut_ptr(), self.slot_size)
        };
        self.index(OFF_TAIL)
            .store(tail.wrapping_add(1), Ordering::Release);
        true
    }
}

#[cfg(test)]
pub(crate) mod testing {
    use super::*;

    /// A 64-byte aligned buffer laid out as a C-compatible ring, so layout
    /// agreement is exercised without the C host.
    pub(crate) struct OwnedRing {
        _buf: Vec<u64>,
        base: NonNull<u8>,
        avail: usize,
    }

    impl OwnedRing {
        pub(crate) fn new(slot_size: u32, capacity: u32) -> OwnedRing {
            let bytes = ring_bytes(slot_size, capacity);
            let mut buf = vec![0u64; bytes / 8 + 16];
            let raw = buf.as_mut_ptr() as *mut u8;
            let skip = (64 - raw as usize % 64) % 64;
            let base = NonNull::new(raw.wrapping_add(skip)).unwrap();
            let r = OwnedRing {
                avail: buf.len() * 8 - skip,
                _buf: buf,
                base,
            };
            r.poke_u32(OFF_SLOT, slot_size);
            r.poke_u32(OFF_CAP, capacity);
            r.poke_u32(OFF_MASK, capacity.wrapping_sub(1));
            r.poke_u32(OFF_SLOTS, RING_HDR as u32);
            r.poke_u32(0, RING_MAGIC);
            r
        }

        pub(crate) fn base(&self) -> NonNull<u8> {
            self.base
        }

        pub(crate) fn avail(&self) -> usize {
            self.avail
        }

        pub(crate) fn poke_u32(&self, off: usize, v: u32) {
            // SAFETY: in bounds of the owned buffer, 4-aligned, and derived
            // from its mutable pointer.
            unsafe { (self.base.as_ptr().add(off) as *mut u32).write(v) }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::testing::OwnedRing;
    use super::*;

    #[test]
    fn fifo_full_and_empty() {
        let o = OwnedRing::new(64, 4);
        // SAFETY: the buffer outlives the ring within this test.
        let r = unsafe { Ring::attach(o.base(), o.avail()) }.unwrap();
        let mut out = vec![0u8; 64];
        assert!(!r.consume(&mut out));
        for i in 0..4u8 {
            assert!(r.produce(&[&[i], &[i + 100]]));
        }
        assert!(!r.produce(&[&[9]]), "a full ring refuses");
        assert_eq!(r.count(), 4);
        for i in 0..4u8 {
            assert!(r.consume(&mut out));
            assert_eq!(&out[..2], &[i, i + 100]);
        }
        assert!(!r.consume(&mut out));
        // Wrap around several times.
        for i in 0..20u8 {
            assert!(r.produce(&[&[7, i]]));
            assert!(r.consume(&mut out));
            assert_eq!(out[1], i);
        }
    }

    #[test]
    fn rejects_bad_geometry() {
        let cases: [(usize, u32); 5] = [
            (0, 0xdead_beef), // magic
            (OFF_CAP, 3),     // not a power of two
            (OFF_MASK, 1),    // mask mismatch
            (OFF_SLOTS, 128), // slot array moved
            (OFF_SLOT, 0),    // zero slot size
        ];
        for (off, v) in cases {
            let o = OwnedRing::new(64, 4);
            o.poke_u32(off, v);
            // SAFETY: the buffer outlives the attempt.
            let attached = unsafe { Ring::attach(o.base(), o.avail()) };
            assert!(attached.is_err(), "offset {off}");
        }
        let o = OwnedRing::new(64, 4);
        // SAFETY: a short length is exactly what is being refused.
        assert!(unsafe { Ring::attach(o.base(), RING_HDR + 64) }.is_err());
    }
}
