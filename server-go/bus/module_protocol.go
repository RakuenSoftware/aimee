package bus

import (
	"encoding/binary"
	"errors"
)

// Module payload wire constants mirror aimee/core/event_bus/module_protocol.h.
const (
	ModuleMessageMagic     uint32 = 0x444f4d41 // "AMOD", little-endian
	ModuleMessageVersion   uint16 = 1
	ModuleMessageHeaderLen        = 40
	ModuleMessageMaxBody   uint32 = 16 * 1024 * 1024
)

// AMOD field offsets, frozen by src/tests/fixtures/bus/layout_vectors.tsv.
const (
	amodOffMagic     = 0
	amodOffVersion   = 4
	amodOffHeaderLen = 6
	amodOffOperation = 8
	amodOffStatus    = 10
	amodOffStageID   = 12
	amodOffFlags     = 16
	amodOffBodyLen   = 20
	amodOffDeadline  = 24
	amodOffTraceID   = 32
)

type ModuleOperation uint16

const (
	ModuleOpInvoke ModuleOperation = 1
	ModuleOpResult ModuleOperation = 2
)

type ModuleStatus uint16

const (
	ModuleStatusOK ModuleStatus = iota
	ModuleStatusCapabilityAbsent
	ModuleStatusCancelled
	ModuleStatusDeadlineExceeded
	ModuleStatusInvalidRequest
	ModuleStatusInternal
)

var ErrModuleMessage = errors.New("bus: invalid module message")

// ModuleMessage is the language-neutral payload header for a module stage.
// Body bytes immediately follow this header in the event's inline payload.
type ModuleMessage struct {
	Operation  ModuleOperation
	Status     ModuleStatus
	StageID    uint32
	Flags      uint32
	BodyLen    uint32
	DeadlineNS uint64
	TraceID    uint64
}

func (m ModuleMessage) valid() bool {
	if m.Operation != ModuleOpInvoke && m.Operation != ModuleOpResult {
		return false
	}
	if m.Status > ModuleStatusInternal || (m.Operation == ModuleOpInvoke && m.Status != ModuleStatusOK) {
		return false
	}
	return m.StageID != 0 && m.Flags == 0 && m.BodyLen <= ModuleMessageMaxBody
}

func (m ModuleMessage) Encode(out []byte) (int, error) {
	if len(out) < ModuleMessageHeaderLen || !m.valid() {
		return 0, ErrModuleMessage
	}
	clear(out[:ModuleMessageHeaderLen])
	binary.LittleEndian.PutUint32(out[amodOffMagic:], ModuleMessageMagic)
	binary.LittleEndian.PutUint16(out[amodOffVersion:], ModuleMessageVersion)
	binary.LittleEndian.PutUint16(out[amodOffHeaderLen:], ModuleMessageHeaderLen)
	binary.LittleEndian.PutUint16(out[amodOffOperation:], uint16(m.Operation))
	binary.LittleEndian.PutUint16(out[amodOffStatus:], uint16(m.Status))
	binary.LittleEndian.PutUint32(out[amodOffStageID:], m.StageID)
	binary.LittleEndian.PutUint32(out[amodOffFlags:], m.Flags)
	binary.LittleEndian.PutUint32(out[amodOffBodyLen:], m.BodyLen)
	binary.LittleEndian.PutUint64(out[amodOffDeadline:], m.DeadlineNS)
	binary.LittleEndian.PutUint64(out[amodOffTraceID:], m.TraceID)
	return ModuleMessageHeaderLen, nil
}

func DecodeModuleMessage(in []byte) (ModuleMessage, error) {
	if len(in) < ModuleMessageHeaderLen || binary.LittleEndian.Uint32(in[amodOffMagic:]) != ModuleMessageMagic ||
		binary.LittleEndian.Uint16(in[amodOffVersion:]) != ModuleMessageVersion ||
		binary.LittleEndian.Uint16(in[amodOffHeaderLen:]) != ModuleMessageHeaderLen {
		return ModuleMessage{}, ErrModuleMessage
	}
	m := ModuleMessage{
		Operation:  ModuleOperation(binary.LittleEndian.Uint16(in[amodOffOperation:])),
		Status:     ModuleStatus(binary.LittleEndian.Uint16(in[amodOffStatus:])),
		StageID:    binary.LittleEndian.Uint32(in[amodOffStageID:]),
		Flags:      binary.LittleEndian.Uint32(in[amodOffFlags:]),
		BodyLen:    binary.LittleEndian.Uint32(in[amodOffBodyLen:]),
		DeadlineNS: binary.LittleEndian.Uint64(in[amodOffDeadline:]),
		TraceID:    binary.LittleEndian.Uint64(in[amodOffTraceID:]),
	}
	if !m.valid() || uint64(m.BodyLen) > uint64(len(in)-ModuleMessageHeaderLen) {
		return ModuleMessage{}, ErrModuleMessage
	}
	return m, nil
}

func (m ModuleMessage) DeadlineExpired(nowNS uint64) bool {
	return m.DeadlineNS != 0 && nowNS >= m.DeadlineNS
}
