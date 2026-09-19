package bus

import (
	"encoding/binary"
	"encoding/json"
	"fmt"
)

// Command framing is shared by fixed modules and external plugin modules.
// The transport carries an opaque JSON object; argument policy belongs to the
// module that implements the command.
const (
	commandRequestMagic   = 0x51504d43
	commandResponseMagic  = 0x53504d43
	commandVersion        = 1
	commandHeader         = 16
	commandResponseHeader = 12
	commandVerbMax        = 127
)

func EncodeCommand(verb string, args json.RawMessage) ([]byte, error) {
	if len(args) == 0 {
		args = json.RawMessage(`{}`)
	}
	if len(verb) == 0 || len(verb) > commandVerbMax || !json.Valid(args) ||
		len(args)+len(verb)+commandHeader > int(ModuleMessageMaxBody) {
		return nil, fmt.Errorf("module command: invalid verb or arguments")
	}
	out := make([]byte, commandHeader+len(verb)+len(args))
	binary.LittleEndian.PutUint32(out[0:4], commandRequestMagic)
	binary.LittleEndian.PutUint32(out[4:8], commandVersion)
	binary.LittleEndian.PutUint16(out[8:10], uint16(len(verb)))
	binary.LittleEndian.PutUint32(out[12:16], uint32(len(args)))
	copy(out[commandHeader:], verb)
	copy(out[commandHeader+len(verb):], args)
	return out, nil
}

func DecodeCommand(frame []byte) (string, json.RawMessage, error) {
	if len(frame) < commandHeader || len(frame) > int(ModuleMessageMaxBody) ||
		binary.LittleEndian.Uint32(frame[0:4]) != commandRequestMagic ||
		binary.LittleEndian.Uint32(frame[4:8]) != commandVersion ||
		binary.LittleEndian.Uint16(frame[10:12]) != 0 {
		return "", nil, fmt.Errorf("module command: invalid frame")
	}
	verbLen := int(binary.LittleEndian.Uint16(frame[8:10]))
	argsLen := uint64(binary.LittleEndian.Uint32(frame[12:16]))
	if verbLen == 0 || verbLen > commandVerbMax || uint64(commandHeader+verbLen)+argsLen != uint64(len(frame)) {
		return "", nil, fmt.Errorf("module command: invalid length")
	}
	args := json.RawMessage(frame[commandHeader+verbLen:])
	if len(args) == 0 {
		args = json.RawMessage(`{}`)
	}
	if !json.Valid(args) {
		return "", nil, fmt.Errorf("module command: invalid JSON")
	}
	return string(frame[commandHeader : commandHeader+verbLen]), args, nil
}

func EncodeCommandResult(body json.RawMessage) ([]byte, error) {
	if !json.Valid(body) || len(body)+commandResponseHeader > int(ModuleMessageMaxBody) {
		return nil, fmt.Errorf("module command: invalid result")
	}
	out := make([]byte, commandResponseHeader+len(body))
	binary.LittleEndian.PutUint32(out[0:4], commandResponseMagic)
	binary.LittleEndian.PutUint32(out[4:8], commandVersion)
	binary.LittleEndian.PutUint32(out[8:12], uint32(len(body)))
	copy(out[commandResponseHeader:], body)
	return out, nil
}

func DecodeCommandResult(frame []byte) (json.RawMessage, error) {
	if len(frame) < commandResponseHeader || len(frame) > int(ModuleMessageMaxBody) ||
		binary.LittleEndian.Uint32(frame[0:4]) != commandResponseMagic ||
		binary.LittleEndian.Uint32(frame[4:8]) != commandVersion ||
		uint64(binary.LittleEndian.Uint32(frame[8:12]))+commandResponseHeader != uint64(len(frame)) ||
		!json.Valid(frame[commandResponseHeader:]) {
		return nil, fmt.Errorf("module command: invalid result frame")
	}
	return json.RawMessage(frame[commandResponseHeader:]), nil
}
