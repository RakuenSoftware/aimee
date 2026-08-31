package delegates

import (
	"encoding/binary"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func chainRequest(op byte, flags [4]bool, parent, max int32) []byte {
	request := make([]byte, chainRequestLen)
	binary.LittleEndian.PutUint32(request[0:4], chainRequestMagic)
	request[4] = wireVersion
	request[5] = op
	for i, f := range flags {
		request[6+i] = boolByte(f)
	}
	binary.LittleEndian.PutUint32(request[12:16], uint32(parent))
	binary.LittleEndian.PutUint32(request[16:20], uint32(max))
	return request
}

func chainCall(t *testing.T, request []byte) (bool, int32) {
	t.Helper()
	response, status := Handle(bus.ModuleInvocation{StageID: StageChain}, request)
	if status != bus.ModuleStatusOK || len(response) != chainResponseLen ||
		binary.LittleEndian.Uint32(response[0:4]) != chainResponseMagic {
		t.Fatalf("response = %x, status = %d", response, status)
	}
	return response[4] == 1, int32(binary.LittleEndian.Uint32(response[8:12]))
}

// An inherited depth is only trustworthy while the chain it describes still
// exists. Both ways it goes stale must clear it, or a delegation is judged
// against a chain that has already gone.
func TestStaleInheritedDepthIsCleared(t *testing.T) {
	cases := []struct {
		name                                          string
		hasDepth, hasParent, parentKnown, parentAlive bool
		want                                          bool
	}{
		{"depth with no parent marker is a leftover", true, false, false, false, true},
		{"parent known to have exited", true, true, true, false, true},
		{"parent still running", true, true, true, true, false},
		// A failed check must not clear: discarding the depth would quietly
		// raise the ceiling for every delegation underneath it.
		{"parent liveness unknown", true, true, false, false, false},
		{"nothing inherited at all", false, false, false, false, false},
	}
	for _, c := range cases {
		flags := [4]bool{c.hasDepth, c.hasParent, c.parentKnown, c.parentAlive}
		got, _ := chainCall(t, chainRequest(ChainOpShouldClear, flags, 0, 0))
		if got != c.want {
			t.Errorf("%s: clear = %v, want %v", c.name, got, c.want)
		}
	}
}

// The depth reported is the child's, so a delegation at the limit is refused
// before it runs rather than after.
func TestDepthIsCheckedAgainstTheChildNotTheParent(t *testing.T) {
	cases := []struct {
		parent, max, wantDepth int32
		wantAllowed            bool
	}{
		{0, 3, 1, true},  // first delegation
		{2, 3, 3, true},  // exactly at the limit is still allowed
		{3, 3, 4, false}, // one past it is not
		{0, 0, 1, false}, // a zero limit forbids delegating at all
	}
	for _, c := range cases {
		allowed, depth := chainCall(t,
			chainRequest(ChainOpCheckDepth, [4]bool{}, c.parent, c.max))
		if allowed != c.wantAllowed || depth != c.wantDepth {
			t.Errorf("parent=%d max=%d: allowed=%v depth=%d, want %v/%d",
				c.parent, c.max, allowed, depth, c.wantAllowed, c.wantDepth)
		}
	}
}

func TestChainRejectsInvalidEnvelope(t *testing.T) {
	short := chainRequest(ChainOpCheckDepth, [4]bool{}, 0, 1)[:chainRequestLen-1]
	if _, status := Handle(bus.ModuleInvocation{StageID: StageChain}, short); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("truncated status = %d", status)
	}

	if _, status := Handle(bus.ModuleInvocation{StageID: StageChain},
		chainRequest(9, [4]bool{}, 0, 1)); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("unknown-op status = %d", status)
	}

	// A flag byte that is neither 0 nor 1 is not a boolean; refuse rather than
	// coerce it, since coercion would silently pick an answer.
	bad := chainRequest(ChainOpShouldClear, [4]bool{}, 0, 0)
	bad[7] = 2
	if _, status := Handle(bus.ModuleInvocation{StageID: StageChain}, bad); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("non-boolean flag status = %d", status)
	}

	if _, status := Handle(bus.ModuleInvocation{StageID: StageChain, DeadlineNS: 1},
		chainRequest(ChainOpCheckDepth, [4]bool{}, 0, 1)); status != bus.ModuleStatusCancelled {
		t.Fatalf("expired status = %d", status)
	}
}
