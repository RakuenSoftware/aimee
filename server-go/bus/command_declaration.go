package bus

import (
	"encoding/binary"
	"errors"
	"strings"
)

// StageDescribeCommands is the optional, common discovery stage for fixed
// modules. Plugin instances retain their version-1 admission protocol.
const StageDescribeCommands uint32 = 255

// CommandDefinition describes a public surface; invocation uses EncodeCommand.
type CommandDefinition struct {
	Group, Verb, Summary string
	Surfaces, Visibility uint32
}

// A zero surface mask declares a host-internal command. It must never enter
// an external command registry.
// EncodeCommandDeclaration answers DCMD version 2. DCMR adds an invocation
// stage after the count; the event kind is derived from the serving principal.
// Records retain the version-1 16-byte header and length-prefixed strings.
func EncodeCommandDeclaration(request []byte, stage uint32, commands []CommandDefinition) ([]byte, error) {
	if len(request) != 8 || binary.LittleEndian.Uint32(request) != 0x444d4344 || binary.LittleEndian.Uint32(request[4:]) != 2 || stage == 0 || stage >= StageDescribeCommands || len(commands) > 4096 {
		return nil, errors.New("invalid command declaration")
	}
	out := make([]byte, 16)
	binary.LittleEndian.PutUint32(out, 0x524d4344)
	binary.LittleEndian.PutUint32(out[4:], 2)
	binary.LittleEndian.PutUint32(out[8:], uint32(len(commands)))
	binary.LittleEndian.PutUint32(out[12:], stage)
	seen := make(map[string]bool, len(commands))
	for _, c := range commands {
		key := c.Group + "." + c.Verb
		if !commandNameValid(c.Group) || !commandNameValid(c.Verb) || len(c.Group) > 127 || len(c.Verb) > 127 || len(c.Summary) > 65535 || c.Surfaces & ^uint32(15) != 0 || c.Visibility > 1 ||
			(c.Surfaces&4 != 0 && c.Surfaces&1 == 0) || strings.ContainsAny(c.Group, "\x00.") || strings.ContainsAny(c.Verb, "\x00.") || strings.ContainsRune(c.Summary, 0) || seen[key] {
			return nil, errors.New("invalid declared command")
		}
		seen[key] = true
		var record [16]byte
		binary.LittleEndian.PutUint32(record[:4], c.Surfaces)
		binary.LittleEndian.PutUint32(record[4:8], c.Visibility)
		binary.LittleEndian.PutUint16(record[8:10], uint16(len(c.Group)))
		binary.LittleEndian.PutUint16(record[10:12], uint16(len(c.Verb)))
		binary.LittleEndian.PutUint16(record[12:14], uint16(len(c.Summary)))
		out = append(out, record[:]...)
		out = append(out, c.Group...)
		out = append(out, c.Verb...)
		out = append(out, c.Summary...)
		if len(out) > 256*1024 {
			return nil, errors.New("command declaration too large")
		}
	}
	return out, nil
}

func commandNameValid(name string) bool {
	if name == "" {
		return false
	}
	for _, c := range name {
		if !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
			return false
		}
	}
	return true
}
