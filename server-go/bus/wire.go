// Package bus is the Go reference client for the aimee shared-memory event bus.
//
// It is one of the two independent implementations that keep the wire spec
// honest: this package and the in-source C client are both held to the same
// committed vectors (src/tests/fixtures/bus/wire_vectors.tsv). Agreement between
// them is evidence about the wire, not a codec agreeing with itself.
//
// This file is the frame codec. Its byte layout must match src/core/event_bus/
// bus_wire.c exactly; the vectors are what enforce that, not this comment.
//
// No cgo: the package is pure Go, per suite invariant 12 (no cgo boundary). A Go
// client that linked the C implementation would defeat the whole point of the
// conformance suite.
package bus

import "encoding/binary"

const (
	// WireMagic is "BUS0" little-endian.
	WireMagic uint32 = 0x30535542
	// WireVersion is the encoding version this build speaks.
	WireVersion uint16 = 3
	// HdrLen is the fixed frame header size, frozen by the vectors.
	HdrLen = 64
	// MaxPayload bounds a single event's payload.
	MaxPayload uint32 = 1 << 20
)

// Header flags. Placement and message pattern are separate axes.
const (
	FInline       uint16 = 0x0001
	FArena        uint16 = 0x0002
	FNotification uint16 = 0x0004
	FRequest      uint16 = 0x0008
	FReply        uint16 = 0x0010
	FCancel       uint16 = 0x0020
	FControl      uint16 = 0x0040
	FMore         uint16 = 0x0080

	fPlacementMask = FInline | FArena
	fPatternMask   = FNotification | FRequest | FReply | FCancel
	fKnownMask     = fPlacementMask | fPatternMask | FControl | FMore
)

// Reserved event kinds; module kinds start at KindModuleBase.
const (
	KindAttachRequest    uint32 = 1
	KindAttachReply      uint32 = 2
	KindError            uint32 = 3
	KindCapabilityAbsent uint32 = 4
	KindOverflow         uint32 = 5
	KindProducerReaped   uint32 = 6
	KindEpochChange      uint32 = 7
	KindModuleBase       uint32 = 256
)

// Frame is a decoded event header. Field names and widths mirror bus_frame_t.
type Frame struct {
	HdrFlags      uint16
	WireVersion   uint16
	EventKind     uint32
	PrincipalRef  uint32
	CorrelationID uint64
	Seq           uint64
	LogicalTS     uint64
	PayloadRef    uint64
	PayloadLen    uint32
	SrcHandle     uint32
	DstHandle     uint32
	Generation    uint32 // v2: ARENA lease generation (0 otherwise)
}

// Result mirrors bus_wire_result_t. The string form is part of the
// cross-language contract: the vectors compare against these names.
type Result int

const (
	OK Result = iota
	ErrShort
	ErrMagic
	ErrVersion
	ErrFlags
	ErrPayloadLen
	ErrCorrelation
	ErrReserved
)

func (r Result) String() string {
	switch r {
	case OK:
		return "OK"
	case ErrShort:
		return "ERR_SHORT"
	case ErrMagic:
		return "ERR_MAGIC"
	case ErrVersion:
		return "ERR_VERSION"
	case ErrFlags:
		return "ERR_FLAGS"
	case ErrPayloadLen:
		return "ERR_PAYLOAD_LEN"
	case ErrCorrelation:
		return "ERR_CORRELATION"
	case ErrReserved:
		return "ERR_RESERVED"
	default:
		return "ERR_UNKNOWN"
	}
}

func exactlyOne(v uint16) bool { return v != 0 && v&(v-1) == 0 }

// Validate applies the same rules as bus_wire_validate.
func (f *Frame) Validate() Result {
	if f.WireVersion != WireVersion {
		return ErrVersion
	}
	if f.HdrFlags&^fKnownMask != 0 {
		return ErrFlags
	}
	if !exactlyOne(f.HdrFlags & fPatternMask) {
		return ErrFlags
	}
	placement := f.HdrFlags & fPlacementMask
	if f.PayloadLen > 0 {
		if !exactlyOne(placement) {
			return ErrFlags
		}
		if f.PayloadLen > MaxPayload {
			return ErrPayloadLen
		}
	} else {
		if placement != 0 {
			return ErrFlags
		}
		if f.PayloadRef != 0 {
			return ErrPayloadLen
		}
	}
	if f.HdrFlags&FMore != 0 {
		pattern := f.HdrFlags & fPatternMask
		if (pattern != FRequest && pattern != FReply) || placement != FInline || f.PayloadLen == 0 {
			return ErrFlags
		}
	}
	// generation is an ARENA-only field (v2); non-arena frames must carry 0.
	if f.HdrFlags&FArena == 0 && f.Generation != 0 {
		return ErrFlags
	}
	if f.HdrFlags&FNotification != 0 {
		if f.CorrelationID != 0 {
			return ErrCorrelation
		}
	} else if f.CorrelationID == 0 {
		return ErrCorrelation
	}
	return OK
}

// Encode writes the frame into out (which must be at least HdrLen bytes) and
// returns the number of bytes written, or 0 if out is too small or the frame
// would not survive a decode — the encoder validates the same rules the decoder
// enforces, exactly as the C encoder does.
func (f *Frame) Encode(out []byte) int {
	if len(out) < HdrLen {
		return 0
	}
	if f.Validate() != OK {
		return 0
	}
	b := out[:HdrLen]
	for i := range b {
		b[i] = 0
	}
	binary.LittleEndian.PutUint32(b[0:], WireMagic)
	binary.LittleEndian.PutUint16(b[4:], f.HdrFlags)
	binary.LittleEndian.PutUint16(b[6:], f.WireVersion)
	binary.LittleEndian.PutUint32(b[8:], f.EventKind)
	binary.LittleEndian.PutUint32(b[12:], f.PrincipalRef)
	binary.LittleEndian.PutUint64(b[16:], f.CorrelationID)
	binary.LittleEndian.PutUint64(b[24:], f.Seq)
	binary.LittleEndian.PutUint64(b[32:], f.LogicalTS)
	binary.LittleEndian.PutUint64(b[40:], f.PayloadRef)
	binary.LittleEndian.PutUint32(b[48:], f.PayloadLen)
	binary.LittleEndian.PutUint32(b[52:], f.SrcHandle)
	binary.LittleEndian.PutUint32(b[56:], f.DstHandle)
	binary.LittleEndian.PutUint32(b[60:], f.Generation) // v2: 0 for non-arena
	return HdrLen
}

// Decode reads a frame from in. On OK, *out holds the frame; on any error out is
// untouched, so a caller cannot act on a partial decode.
func Decode(in []byte, out *Frame) Result {
	if len(in) < HdrLen {
		return ErrShort
	}
	if binary.LittleEndian.Uint32(in[0:]) != WireMagic {
		return ErrMagic
	}
	var f Frame
	f.HdrFlags = binary.LittleEndian.Uint16(in[4:])
	f.WireVersion = binary.LittleEndian.Uint16(in[6:])
	f.EventKind = binary.LittleEndian.Uint32(in[8:])
	f.PrincipalRef = binary.LittleEndian.Uint32(in[12:])
	f.CorrelationID = binary.LittleEndian.Uint64(in[16:])
	f.Seq = binary.LittleEndian.Uint64(in[24:])
	f.LogicalTS = binary.LittleEndian.Uint64(in[32:])
	f.PayloadRef = binary.LittleEndian.Uint64(in[40:])
	f.PayloadLen = binary.LittleEndian.Uint32(in[48:])
	f.SrcHandle = binary.LittleEndian.Uint32(in[52:])
	f.DstHandle = binary.LittleEndian.Uint32(in[56:])
	f.Generation = binary.LittleEndian.Uint32(in[60:])
	if r := f.Validate(); r != OK {
		return r
	}
	*out = f
	return OK
}

// CheckPlacement bounds a decoded payload reference against live geometry,
// mirroring bus_wire_check_placement. Necessary but not sufficient for an arena
// reference (the live-lease check is the host's).
func (f *Frame) CheckPlacement(slotSize uint32, arenaSize uint64) Result {
	if r := f.Validate(); r != OK {
		return r
	}
	if f.PayloadLen == 0 {
		return OK
	}
	if f.HdrFlags&FInline != 0 {
		end := f.PayloadRef + uint64(f.PayloadLen)
		if end < f.PayloadRef {
			return ErrPayloadLen
		}
		if f.PayloadRef < HdrLen || end > uint64(slotSize) {
			return ErrPayloadLen
		}
	} else {
		// ARENA (v2): PayloadRef is a lease id; the offset/span bounds are the
		// lease table's business. Only bound the length against the whole arena.
		if uint64(f.PayloadLen) > arenaSize {
			return ErrPayloadLen
		}
	}
	return OK
}
