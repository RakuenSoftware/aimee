package bus

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"strings"
)

// CommandContext is supplied by the authenticating host, separately from user
// arguments. Receiving modules must check the transport principal before using
// it. The external bus admission policy reserves principal ref zero for the
// host's in-process clients; grant files cannot admit an external ref zero.
type CommandContext struct {
	Authenticated     bool   `json:"authenticated"`
	Principal         string `json:"principal"`
	TransportIdentity string `json:"transport_identity"`
	UserAuthority     bool   `json:"user_authority"`
}

const commandContextMax = 4096

func (c CommandContext) valid() bool {
	return len(c.Principal) <= 576 && len(c.TransportIdentity) <= 576 &&
		!strings.ContainsRune(c.Principal, '\x00') && !strings.ContainsRune(c.TransportIdentity, '\x00') &&
		(!c.Authenticated || c.Principal != "") && (!c.UserAuthority || c.Authenticated)
}

// Version two adds a bounded context length after the ordinary header. The
// payload is verb, arguments, then context. Results retain the version-one frame.
func EncodeCommandWithContext(verb string, args json.RawMessage, caller CommandContext) ([]byte, error) {
	if !caller.valid() {
		return nil, fmt.Errorf("module command: invalid caller context")
	}
	plain, err := EncodeCommand(verb, args)
	if err != nil {
		return nil, err
	}
	contextJSON, err := json.Marshal(caller)
	if err != nil || len(contextJSON) > commandContextMax || len(plain)+4+len(contextJSON) > int(ModuleMessageMaxBody) {
		return nil, fmt.Errorf("module command: context exceeds bounds")
	}
	out := make([]byte, len(plain)+4+len(contextJSON))
	copy(out, plain[:commandHeader])
	binary.LittleEndian.PutUint32(out[4:], 2)
	binary.LittleEndian.PutUint32(out[16:], uint32(len(contextJSON)))
	copy(out[20:], plain[16:])
	copy(out[len(plain)+4:], contextJSON)
	return out, nil
}

func DecodeCommandWithContext(frame []byte) (string, json.RawMessage, *CommandContext, error) {
	if len(frame) >= 8 && binary.LittleEndian.Uint32(frame[4:]) == 1 {
		verb, args, err := DecodeCommand(frame)
		return verb, args, nil, err
	}
	bad := func() (string, json.RawMessage, *CommandContext, error) {
		return "", nil, nil, fmt.Errorf("module command: invalid context frame")
	}
	if len(frame) < 20 || len(frame) > int(ModuleMessageMaxBody) || binary.LittleEndian.Uint32(frame[4:]) != 2 {
		return bad()
	}
	contextLen := uint64(binary.LittleEndian.Uint32(frame[16:]))
	verbLen := uint64(binary.LittleEndian.Uint16(frame[8:]))
	argsLen := uint64(binary.LittleEndian.Uint32(frame[12:]))
	if contextLen == 0 || contextLen > commandContextMax || 20+verbLen+argsLen+contextLen != uint64(len(frame)) {
		return bad()
	}
	plain := make([]byte, len(frame)-int(contextLen)-4)
	copy(plain, frame[:16])
	binary.LittleEndian.PutUint32(plain[4:], 1)
	copy(plain[16:], frame[20:len(frame)-int(contextLen)])
	verb, args, err := DecodeCommand(plain)
	if err != nil {
		return bad()
	}
	raw := frame[len(frame)-int(contextLen):]
	if len(bytes.TrimSpace(raw)) == 0 || bytes.TrimSpace(raw)[0] != '{' || !json.Valid(raw) {
		return bad()
	}
	var caller CommandContext
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.DisallowUnknownFields()
	if decoder.Decode(&caller) != nil || !caller.valid() {
		return bad()
	}
	return verb, args, &caller, nil
}
