package memory

import (
	"encoding/binary"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func declareRequest() []byte {
	req := make([]byte, commandsRequestLen)
	binary.LittleEndian.PutUint32(req[0:4], commandsRequestMagic)
	binary.LittleEndian.PutUint32(req[4:8], wireVersion)
	return req
}

// The declaration is what every surface routes from, so the encoding has to be
// readable by the core exactly as written. Decoding it here with an independent
// reader is the point: the delegate mount-table bug was two sides assuming
// different encodings with neither erroring, and a command that fails to decode
// does not fail loudly -- it simply is not there, which reads as "aimee cannot do
// that" rather than as a fault.
func TestDeclareCommandsRoundTrip(t *testing.T) {
	out, status := handleDeclareCommands(bus.ModuleInvocation{StageID: StageDeclareCommands}, declareRequest())
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v, want OK", status)
	}
	if len(out) < 12 {
		t.Fatalf("response too short: %d", len(out))
	}
	if got := binary.LittleEndian.Uint32(out[0:4]); got != commandsResponseMagic {
		t.Fatalf("magic = %#x", got)
	}
	if got := binary.LittleEndian.Uint32(out[4:8]); got != wireVersion {
		t.Fatalf("version = %d", got)
	}
	count := binary.LittleEndian.Uint32(out[8:12])
	if int(count) != len(declaredCommands) {
		t.Fatalf("count = %d, want %d", count, len(declaredCommands))
	}

	seen := map[string]Command{}
	off := 12
	for i := 0; i < int(count); i++ {
		if off+16 > len(out) {
			t.Fatalf("record %d truncated", i)
		}
		surfaces := binary.LittleEndian.Uint32(out[off : off+4])
		vis := binary.LittleEndian.Uint32(out[off+4 : off+8])
		gl := int(binary.LittleEndian.Uint16(out[off+8 : off+10]))
		vl := int(binary.LittleEndian.Uint16(out[off+10 : off+12]))
		sl := int(binary.LittleEndian.Uint16(out[off+12 : off+14]))
		off += 16
		if off+gl+vl+sl > len(out) {
			t.Fatalf("record %d strings truncated", i)
		}
		group := string(out[off : off+gl])
		verb := string(out[off+gl : off+gl+vl])
		summary := string(out[off+gl+vl : off+gl+vl+sl])
		off += gl + vl + sl
		seen[group+"."+verb] = Command{Group: group, Verb: verb, Summary: summary,
			Surfaces: surfaces, Visibility: vis}
	}
	if off != len(out) {
		t.Fatalf("trailing bytes: consumed %d of %d", off, len(out))
	}

	get, ok := seen["memory.get"]
	if !ok {
		t.Fatal("memory.get was not declared")
	}
	if get.Surfaces&SurfaceMCP == 0 || get.Surfaces&SurfaceCLI == 0 {
		t.Fatalf("memory.get surfaces = %#x, want CLI and MCP", get.Surfaces)
	}

	// store is CLI+RPC by DECISION, not by omission: an agent should not write to
	// the store mid-turn. Asserted so the decision cannot be quietly widened.
	store, ok := seen["memory.store"]
	if !ok {
		t.Fatal("memory.store was not declared")
	}
	if store.Surfaces&SurfaceMCP != 0 {
		t.Fatalf("memory.store must not be on the MCP surface: %#x", store.Surfaces)
	}
	if store.Surfaces&(SurfaceCLI|SurfaceRPC) != (SurfaceCLI | SurfaceRPC) {
		t.Fatalf("memory.store surfaces = %#x, want CLI|RPC", store.Surfaces)
	}
}

// A request that is not understood is REFUSED. Answering it anyway would declare
// a partial command surface, and a missing command is invisible.
func TestDeclareCommandsRejectsMalformed(t *testing.T) {
	for _, bad := range [][]byte{
		nil,
		make([]byte, 4),
		func() []byte { b := declareRequest(); binary.LittleEndian.PutUint32(b[0:4], 0xdeadbeef); return b }(),
		func() []byte { b := declareRequest(); binary.LittleEndian.PutUint32(b[4:8], 99); return b }(),
	} {
		if _, status := handleDeclareCommands(bus.ModuleInvocation{}, bad); status == bus.ModuleStatusOK {
			t.Fatalf("malformed request accepted: %v", bad)
		}
	}
}

// Group+Verb is the single spelling every surface derives from. Duplicates would
// make one name resolve two ways depending on where you came in.
func TestDeclaredCommandsAreUnique(t *testing.T) {
	seen := map[string]bool{}
	for _, c := range declaredCommands {
		key := c.Group + "." + c.Verb
		if seen[key] {
			t.Fatalf("duplicate declaration: %s", key)
		}
		seen[key] = true
		if c.Group == "" || c.Verb == "" || c.Surfaces == 0 {
			t.Fatalf("malformed declaration: %+v", c)
		}
	}
}
