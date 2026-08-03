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
	binary.LittleEndian.PutUint32(out[0:], ModuleMessageMagic)
	binary.LittleEndian.PutUint16(out[4:], ModuleMessageVersion)
	binary.LittleEndian.PutUint16(out[6:], ModuleMessageHeaderLen)
	binary.LittleEndian.PutUint16(out[8:], uint16(m.Operation))
	binary.LittleEndian.PutUint16(out[10:], uint16(m.Status))
	binary.LittleEndian.PutUint32(out[12:], m.StageID)
	binary.LittleEndian.PutUint32(out[16:], m.Flags)
	binary.LittleEndian.PutUint32(out[20:], m.BodyLen)
	binary.LittleEndian.PutUint64(out[24:], m.DeadlineNS)
	binary.LittleEndian.PutUint64(out[32:], m.TraceID)
	return ModuleMessageHeaderLen, nil
}

func DecodeModuleMessage(in []byte) (ModuleMessage, error) {
	if len(in) < ModuleMessageHeaderLen || binary.LittleEndian.Uint32(in[0:]) != ModuleMessageMagic ||
		binary.LittleEndian.Uint16(in[4:]) != ModuleMessageVersion ||
		binary.LittleEndian.Uint16(in[6:]) != ModuleMessageHeaderLen {
		return ModuleMessage{}, ErrModuleMessage
	}
	m := ModuleMessage{
		Operation:  ModuleOperation(binary.LittleEndian.Uint16(in[8:])),
		Status:     ModuleStatus(binary.LittleEndian.Uint16(in[10:])),
		StageID:    binary.LittleEndian.Uint32(in[12:]),
		Flags:      binary.LittleEndian.Uint32(in[16:]),
		BodyLen:    binary.LittleEndian.Uint32(in[20:]),
		DeadlineNS: binary.LittleEndian.Uint64(in[24:]),
		TraceID:    binary.LittleEndian.Uint64(in[32:]),
	}
	if !m.valid() || uint64(m.BodyLen) > uint64(len(in)-ModuleMessageHeaderLen) {
		return ModuleMessage{}, ErrModuleMessage
	}
	return m, nil
}

func (m ModuleMessage) DeadlineExpired(nowNS uint64) bool {
	return m.DeadlineNS != 0 && nowNS >= m.DeadlineNS
}
