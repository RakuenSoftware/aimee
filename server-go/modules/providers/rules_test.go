package providers

import (
	"encoding/binary"
	"github.com/JBailes/aimee/server-go/bus"
	"testing"
)

func ruleRequest(stage uint32) []byte {
	n := 8 + recordLen
	if stage == StageResolve {
		n += recordLen
	}
	b := make([]byte, n)
	binary.LittleEndian.PutUint32(b, requestMagic)
	binary.LittleEndian.PutUint32(b[4:], wireVersion)
	copy(b[8:], "vendor")
	copy(b[40:], "model")
	return b
}
func TestDeclarationRulesPreserveZeroAndRejectMismatchedModels(t *testing.T) {
	b := ruleRequest(StageResolve)
	copy(b[8+recordLen:], b[8:8+recordLen])
	decl, fetched := b[8:], b[8+recordLen:]
	binary.LittleEndian.PutUint32(decl[236:], priceIn|contextWindow)
	binary.LittleEndian.PutUint32(fetched[224:], 32768)
	binary.LittleEndian.PutUint32(fetched[228:], 4096)
	reply, status := Rules(bus.ModuleInvocation{StageID: StageResolve}, b)
	if status != bus.ModuleStatusOK {
		t.Fatal(status)
	}
	out := reply[16:]
	if binary.LittleEndian.Uint32(out[224:]) != 32768 || out[265] != 2 || out[267] != 1 || binary.LittleEndian.Uint64(out[240:]) != 0 {
		t.Fatal("resolution lost unknown-capacity or declared-free distinction")
	}
	copy(fetched[32:], "other")
	reply, status = Rules(bus.ModuleInvocation{StageID: StageResolve}, b)
	if status != bus.ModuleStatusOK || binary.LittleEndian.Uint32(reply[8:]) != 2 || binary.LittleEndian.Uint32(reply[12:]) != 0 {
		t.Fatal("cross-model limits accepted")
	}
}
func TestDeclarationValidationAndMalformedWire(t *testing.T) {
	b := ruleRequest(StageValidate)
	record := b[8:]
	binary.LittleEndian.PutUint32(record[236:], contextWindow|maxOutput|priceOut)
	binary.LittleEndian.PutUint32(record[228:], 4096)
	reply, status := Rules(bus.ModuleInvocation{StageID: StageValidate}, b)
	if status != bus.ModuleStatusOK || binary.LittleEndian.Uint32(reply[16+236:])&contextWindow != 0 {
		t.Fatal("zero capacity remained declared")
	}
	binary.LittleEndian.PutUint32(record[224:], 1024)
	reply, status = Rules(bus.ModuleInvocation{StageID: StageValidate}, b)
	if status != bus.ModuleStatusOK || binary.LittleEndian.Uint32(reply[8:]) != 3 {
		t.Fatal("output above context accepted")
	}
	for _, bad := range [][]byte{nil, b[:7], append(b, 0)} {
		if _, status := Rules(bus.ModuleInvocation{StageID: StageValidate}, bad); status != bus.ModuleStatusInvalidRequest {
			t.Fatal("malformed request accepted")
		}
	}
}
