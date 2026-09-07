// Package providers owns provider connections, model configuration and the
// declaration rules shared by their management clients.
package providers

import (
	"bytes"
	"encoding/binary"

	"github.com/JBailes/aimee/server-go/bus"
)

const (
	PrincipalRef  uint32 = 33
	EventResolve  uint32 = 12545
	EventValidate uint32 = 12546
	EventManage   uint32 = 12547
	StageResolve  uint32 = 1
	StageValidate uint32 = 2
	StageManage   uint32 = 3
	recordLen            = 268
	requestMagic  uint32 = 0x51455250
	responseMagic uint32 = 0x53455250
	wireVersion   uint32 = 1
)
const (
	priceIn       uint32 = 1
	priceOut      uint32 = 2
	priceCached   uint32 = 4
	contextWindow uint32 = 8
	maxOutput     uint32 = 16
	capabilities  uint32 = 32
)

// Rules preserves the provider rule wire format while moving its sole
// implementation out of the retired C adapter. The event allocation belongs to
// providers; the old unregistered allocation overlapped economizer.
func Rules(inv bus.ModuleInvocation, body []byte) ([]byte, bus.ModuleStatus) {
	length := 8 + recordLen
	if inv.StageID == StageResolve {
		length += recordLen
	} else if inv.StageID != StageValidate {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if len(body) != length || binary.LittleEndian.Uint32(body) != requestMagic || binary.LittleEndian.Uint32(body[4:]) != wireVersion {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if inv.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	declared := body[8 : 8+recordLen]
	out := make([]byte, recordLen)
	status := uint32(0)
	mask := binary.LittleEndian.Uint32(declared[236:])
	if inv.StageID == StageValidate {
		copy(out, declared)
		if blank(declared[32:160]) || declared[0] == 0 {
			status = 3
		}
		for _, field := range []struct {
			off    int
			bit    uint32
			source int
		}{{224, contextWindow, 265}, {228, maxOutput, 266}} {
			if binary.LittleEndian.Uint32(out[field.off:]) == 0 {
				mask &^= field.bit
			}
			out[field.source] = 0
			if mask&field.bit != 0 {
				out[field.source] = 1
			}
		}
		if mask&(contextWindow|maxOutput) == contextWindow|maxOutput && binary.LittleEndian.Uint32(out[228:]) > binary.LittleEndian.Uint32(out[224:]) {
			status = 3
		}
		out[267] = 0
		if mask&(priceIn|priceOut|priceCached) != 0 {
			out[267] = 1
		}
	} else {
		fetched := body[8+recordLen:]
		haveDeclared, haveFetched := !blank(declared[32:160]), !blank(fetched[32:160])
		if haveDeclared && haveFetched && !bytes.Equal(declared[:160], fetched[:160]) {
			status = 2
		}
		identity := fetched
		if haveDeclared {
			identity = declared
		}
		copy(out[:160], identity[:160])
		label := declared
		if fetched[160] != 0 {
			label = fetched
		}
		copy(out[160:224], label[160:224])
		for _, field := range []struct {
			off    int
			bit    uint32
			source int
		}{{224, contextWindow, 265}, {228, maxOutput, 266}} {
			value := binary.LittleEndian.Uint32(declared[field.off:])
			source := byte(1)
			if mask&field.bit == 0 || value == 0 {
				value = binary.LittleEndian.Uint32(fetched[field.off:])
				source = 2
			}
			if value == 0 {
				source = 0
			}
			binary.LittleEndian.PutUint32(out[field.off:], value)
			out[field.source] = source
		}
		caps := fetched[232:236]
		if mask&capabilities != 0 {
			caps = declared[232:236]
		}
		copy(out[232:236], caps)
		for _, field := range []struct {
			off int
			bit uint32
		}{{240, priceIn}, {248, priceOut}, {256, priceCached}} {
			if mask&field.bit != 0 {
				copy(out[field.off:field.off+8], declared[field.off:field.off+8])
				out[267] = 1
			}
		}
		if declared[264] != 0 || fetched[264] != 0 {
			out[264] = 1
		}
	}
	binary.LittleEndian.PutUint32(out[236:], mask)
	reply := make([]byte, 16+recordLen)
	binary.LittleEndian.PutUint32(reply, responseMagic)
	binary.LittleEndian.PutUint32(reply[4:], wireVersion)
	binary.LittleEndian.PutUint32(reply[8:], status)
	if status == 0 {
		binary.LittleEndian.PutUint32(reply[12:], 1)
		copy(reply[16:], out)
	}
	return reply, bus.ModuleStatusOK
}
func blank(value []byte) bool {
	for _, b := range value {
		if b != 0 {
			return false
		}
	}
	return true
}
