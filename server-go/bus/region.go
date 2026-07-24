package bus

import (
	"encoding/binary"
	"errors"
	"sync/atomic"
	"unsafe"
)

// Control region layout, mirroring bus_control_t (96 bytes).
const (
	ctlMagic         uint32 = 0x4c544342 // "BCTL"
	ctlOffMagic             = 0
	ctlOffSpec              = 4
	ctlOffLayout            = 8
	ctlOffSlot              = 16
	ctlOffInline            = 20
	ctlOffQueueCap          = 24
	ctlOffArena             = 32
	ctlOffEpoch             = 40
	ctlOffHeartbeat         = 48
	ctlBytes                = 96

	specVersion   uint32 = 1
	layoutVersion uint32 = 1
)

// Queue-pair region layout, mirroring bus_qpair_hdr_t (96 bytes) followed by the
// inbound and outbound rings.
const (
	qpMagic         uint32 = 0x52504251 // "QPBR"
	qpOffMagic             = 0
	qpOffSlot              = 4
	qpOffCap               = 8
	qpOffCtlCredits        = 16
	qpOffCtlLost           = 20
	qpOffInboundOff        = 24
	qpOffOutboundOff       = 28
	qpOffHeartbeat         = 32
	qpBytes                = 96
)

// ErrRegion is returned when a mapped region fails validation.
var ErrRegion = errors.New("bus: region invalid")

// Control is a read-only view of the control region.
type Control struct {
	mem []byte
}

// AttachControl validates a mapped control region.
func AttachControl(mem []byte) (*Control, error) {
	if len(mem) < ctlBytes {
		return nil, ErrRegion
	}
	if atomic.LoadUint32((*uint32)(unsafe.Pointer(&mem[ctlOffMagic]))) != ctlMagic {
		return nil, ErrRegion
	}
	if binary.LittleEndian.Uint32(mem[ctlOffSpec:]) != specVersion ||
		binary.LittleEndian.Uint32(mem[ctlOffLayout:]) != layoutVersion {
		return nil, ErrRegion
	}
	slot := binary.LittleEndian.Uint32(mem[ctlOffSlot:])
	inl := binary.LittleEndian.Uint32(mem[ctlOffInline:])
	arena := binary.LittleEndian.Uint64(mem[ctlOffArena:])
	if slot == 0 || inl > slot || arena == 0 {
		return nil, ErrRegion
	}
	return &Control{mem: mem}, nil
}

func (c *Control) SlotSize() uint32     { return binary.LittleEndian.Uint32(c.mem[ctlOffSlot:]) }
func (c *Control) InlineBudget() uint32 { return binary.LittleEndian.Uint32(c.mem[ctlOffInline:]) }
func (c *Control) QueueCapacity() uint32 {
	return binary.LittleEndian.Uint32(c.mem[ctlOffQueueCap:])
}
func (c *Control) ArenaSize() uint64 { return binary.LittleEndian.Uint64(c.mem[ctlOffArena:]) }

// Epoch is the current host epoch (bumped on restart).
func (c *Control) Epoch() uint64 {
	return atomic.LoadUint64(u64ptr(c.mem, ctlOffEpoch))
}

// QueuePair is a view of a client's queue-pair region: two rings plus metadata.
type QueuePair struct {
	mem      []byte
	Inbound  *Ring
	Outbound *Ring
}

// AttachQueuePair validates a mapped queue-pair region and resolves both rings.
func AttachQueuePair(mem []byte) (*QueuePair, error) {
	if len(mem) < qpBytes {
		return nil, ErrRegion
	}
	if atomic.LoadUint32((*uint32)(unsafe.Pointer(&mem[qpOffMagic]))) != qpMagic {
		return nil, ErrRegion
	}
	inOff := binary.LittleEndian.Uint32(mem[qpOffInboundOff:])
	outOff := binary.LittleEndian.Uint32(mem[qpOffOutboundOff:])
	if int(inOff) >= len(mem) || int(outOff) >= len(mem) || inOff == outOff {
		return nil, ErrRegion
	}
	inbound, err := AttachRing(mem[inOff:])
	if err != nil {
		return nil, err
	}
	outbound, err := AttachRing(mem[outOff:])
	if err != nil {
		return nil, err
	}
	return &QueuePair{mem: mem, Inbound: inbound, Outbound: outbound}, nil
}

// ControlLost reports whether the host set the sticky control_lost flag.
func (q *QueuePair) ControlLost() bool {
	return atomic.LoadUint32((*uint32)(unsafe.Pointer(&q.mem[qpOffCtlLost]))) != 0
}

// SetHeartbeat writes the client heartbeat the host reads for liveness.
func (q *QueuePair) SetHeartbeat(now uint64) {
	atomic.StoreUint64(u64ptr(q.mem, qpOffHeartbeat), now)
}
