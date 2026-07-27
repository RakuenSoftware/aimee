package bus

import (
	"encoding/binary"
	"errors"
	"sync/atomic"
	"unsafe"
)

// Ring layout constants, mirroring bus_ring_shared_t. The offsets are frozen by
// the C static asserts (head==64, tail==128, slots==192); this package must
// agree because it maps the same bytes.
const (
	ringMagic     uint32 = 0x474e4952 // "RING"
	ringOffMagic         = 0
	ringOffSlot          = 4
	ringOffCap           = 8
	ringOffMask          = 12
	ringOffSlots         = 16
	ringOffHead          = 64
	ringOffTail          = 128
	ringHdrBytes         = 192
)

// ErrRingLayout is returned when a mapped ring does not match its own header.
var ErrRingLayout = errors.New("bus: ring layout invalid")

// Ring is a view onto a single-producer/single-consumer ring in shared memory.
// It holds no copy of the data; every operation reads or writes the mapping.
type Ring struct {
	mem      []byte // the whole region containing this ring, from the ring base
	slotSize uint32
	capacity uint32
	mask     uint32
	slotsOff uint32
	head     *uint64 // into mem at ringOffHead
	tail     *uint64 // into mem at ringOffTail
}

func u64ptr(b []byte, off int) *uint64 {
	return (*uint64)(unsafe.Pointer(&b[off]))
}

// AttachRing adopts a ring laid out at the start of mem (written by the C host).
// Every geometry field is validated against the bytes actually mapped.
func AttachRing(mem []byte) (*Ring, error) {
	if len(mem) < ringHdrBytes {
		return nil, ErrRingLayout
	}
	magic := atomic.LoadUint32((*uint32)(unsafe.Pointer(&mem[ringOffMagic])))
	if magic != ringMagic {
		return nil, ErrRingLayout
	}
	slot := binary.LittleEndian.Uint32(mem[ringOffSlot:])
	capv := binary.LittleEndian.Uint32(mem[ringOffCap:])
	mask := binary.LittleEndian.Uint32(mem[ringOffMask:])
	slotsOff := binary.LittleEndian.Uint32(mem[ringOffSlots:])
	if capv < 2 || capv&(capv-1) != 0 || slot == 0 || mask != capv-1 ||
		slotsOff != ringHdrBytes {
		return nil, ErrRingLayout
	}
	need := int(slotsOff) + int(slot)*int(capv)
	if len(mem) < need {
		return nil, ErrRingLayout
	}
	head := atomic.LoadUint64(u64ptr(mem, ringOffHead))
	tail := atomic.LoadUint64(u64ptr(mem, ringOffTail))
	if head-tail > uint64(capv) {
		return nil, ErrRingLayout
	}
	return &Ring{
		mem: mem, slotSize: slot, capacity: capv, mask: mask, slotsOff: slotsOff,
		head: u64ptr(mem, ringOffHead), tail: u64ptr(mem, ringOffTail),
	}, nil
}

// Capacity is the number of slots.
func (r *Ring) Capacity() uint32 { return r.capacity }

// SlotSize is the byte size of each slot.
func (r *Ring) SlotSize() uint32 { return r.slotSize }

// Count is a snapshot of the occupied slots.
func (r *Ring) Count() uint64 {
	return atomic.LoadUint64(r.head) - atomic.LoadUint64(r.tail)
}

func (r *Ring) slot(index uint64) []byte {
	off := int(r.slotsOff) + int(index&uint64(r.mask))*int(r.slotSize)
	return r.mem[off : off+int(r.slotSize)]
}

// ProduceBegin returns a writable slot, or nil when the ring is full. The slot
// is not visible to the consumer until ProduceCommit. The producer is the only
// writer of head.
func (r *Ring) ProduceBegin() []byte {
	head := atomic.LoadUint64(r.head)         // our own index
	tail := atomic.LoadUint64(r.tail)         // acquire the consumer's index
	if head-tail >= uint64(r.capacity) {
		return nil
	}
	return r.slot(head)
}

// ProduceCommit publishes the slot most recently begun (a release store on head).
func (r *Ring) ProduceCommit() {
	head := atomic.LoadUint64(r.head)
	atomic.StoreUint64(r.head, head+1)
}

// ConsumeBegin returns the oldest unread slot, or nil when empty. It stays valid
// until ConsumeCommit.
func (r *Ring) ConsumeBegin() []byte {
	tail := atomic.LoadUint64(r.tail)
	head := atomic.LoadUint64(r.head)
	if head == tail {
		return nil
	}
	return r.slot(tail)
}

// ConsumeCommit releases the slot for reuse (a release store on tail).
func (r *Ring) ConsumeCommit() {
	tail := atomic.LoadUint64(r.tail)
	atomic.StoreUint64(r.tail, tail+1)
}

// initRing lays out a ring in mem (used by tests to build a C-compatible ring
// in Go, so the layout agreement is checked without the C host).
func initRing(mem []byte, slotSize, capacity uint32) (*Ring, error) {
	if capacity < 2 || capacity&(capacity-1) != 0 || slotSize == 0 {
		return nil, ErrRingLayout
	}
	need := ringHdrBytes + int(slotSize)*int(capacity)
	if len(mem) < need {
		return nil, ErrRingLayout
	}
	for i := 0; i < need; i++ {
		mem[i] = 0
	}
	binary.LittleEndian.PutUint32(mem[ringOffSlot:], slotSize)
	binary.LittleEndian.PutUint32(mem[ringOffCap:], capacity)
	binary.LittleEndian.PutUint32(mem[ringOffMask:], capacity-1)
	binary.LittleEndian.PutUint32(mem[ringOffSlots:], ringHdrBytes)
	atomic.StoreUint64(u64ptr(mem, ringOffHead), 0)
	atomic.StoreUint64(u64ptr(mem, ringOffTail), 0)
	atomic.StoreUint32((*uint32)(unsafe.Pointer(&mem[ringOffMagic])), ringMagic)
	return AttachRing(mem)
}
