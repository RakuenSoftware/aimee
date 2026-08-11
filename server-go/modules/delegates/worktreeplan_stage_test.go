package delegates

import (
	"bytes"
	"encoding/binary"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func worktreePlanRequest(writesAllowed bool, delegateID string) []byte {
	req := make([]byte, worktreePlanReqHeaderLen)
	binary.LittleEndian.PutUint32(req[0:4], worktreePlanRequestMagic)
	req[4] = wireVersion
	if writesAllowed {
		req[5] = 1
	}
	binary.LittleEndian.PutUint32(req[8:12], uint32(len(delegateID)))
	return append(req, delegateID...)
}

func decodeWorktreePlan(t *testing.T, response []byte) (isolated, readOnly bool, workName string) {
	t.Helper()
	if len(response) != worktreePlanResponseLen {
		t.Fatalf("response is %d bytes, want %d", len(response), worktreePlanResponseLen)
	}
	if binary.LittleEndian.Uint32(response[0:4]) != worktreePlanResponseMagic {
		t.Fatal("wrong response magic")
	}
	name := response[12:]
	if i := bytes.IndexByte(name, 0); i >= 0 {
		name = name[:i]
	}
	return binary.LittleEndian.Uint32(response[4:8]) == 1,
		binary.LittleEndian.Uint32(response[8:12]) == 1,
		string(name)
}

func callWorktreePlan(t *testing.T, writesAllowed bool, delegateID string) ([]byte, bus.ModuleStatus) {
	t.Helper()
	return Handle(bus.ModuleInvocation{StageID: StageWorktreePlan},
		worktreePlanRequest(writesAllowed, delegateID))
}

// A write role needs its own worktree, named after the delegate so two
// delegates in one session cannot land in the same directory.
func TestWorktreePlanStageWriteRoleIsIsolated(t *testing.T) {
	response, status := callWorktreePlan(t, true, "delegate-7")
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v, want OK", status)
	}
	isolated, readOnly, name := decodeWorktreePlan(t, response)
	if !isolated {
		t.Error("a write role should get its own worktree")
	}
	if readOnly {
		t.Error("a write role's mount must not be read-only")
	}
	if name != "delegate-7" {
		t.Errorf("work name = %q, want the delegate id", name)
	}
}

// A read-only role mounts the parent's worktree, so there is nothing to create
// and no name to give it.
func TestWorktreePlanStageReadRoleCreatesNothing(t *testing.T) {
	response, status := callWorktreePlan(t, false, "delegate-7")
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v, want OK", status)
	}
	isolated, readOnly, name := decodeWorktreePlan(t, response)
	if isolated {
		t.Error("a read-only role should not get its own worktree")
	}
	if !readOnly {
		t.Error("a read-only role's mount must be read-only")
	}
	if name != "" {
		t.Errorf("work name = %q, want empty", name)
	}
}

// The field is sized so the longest accepted name survives intact. Sized
// exactly it would come back one character short -- a different branch, and
// silently.
func TestWorktreePlanStageCarriesTheLongestAcceptedName(t *testing.T) {
	name := strings.Repeat("a", workNameMax)
	response, status := callWorktreePlan(t, true, name)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v, want OK", status)
	}
	_, _, got := decodeWorktreePlan(t, response)
	if got != name {
		t.Errorf("work name came back %d bytes, want %d", len(got), len(name))
	}
}

// A name that cannot be a branch is a refusal, not a plan with the name
// dropped: a write delegate without its own worktree edits its parent's.
func TestWorktreePlanStageRefusesUnusableNames(t *testing.T) {
	for _, id := range []string{
		"", "../escape", "has space", "-leading-dash", "a/b",
		"quote'd", strings.Repeat("a", workNameMax+1),
	} {
		if _, status := callWorktreePlan(t, true, id); status != bus.ModuleStatusInvalidRequest {
			t.Errorf("delegate id %q: status = %v, want InvalidRequest", id, status)
		}
	}
}

// A read-only role needs no name, so an unusable one must not refuse the plan.
func TestWorktreePlanStageIgnoresTheNameWhenNothingIsCreated(t *testing.T) {
	if _, status := callWorktreePlan(t, false, "../escape"); status != bus.ModuleStatusOK {
		t.Errorf("status = %v, want OK", status)
	}
}

func TestWorktreePlanStageRejectsMalformedRequests(t *testing.T) {
	good := worktreePlanRequest(true, "d1")

	cases := map[string][]byte{
		"empty":         {},
		"short header":  good[:8],
		"trailing byte": append(append([]byte{}, good...), 0),
	}
	// A truncated body: the header promises more than is there.
	truncated := append([]byte{}, good...)
	cases["truncated body"] = truncated[:len(truncated)-1]
	// A length field that overruns the buffer entirely.
	overrun := append([]byte{}, good...)
	binary.LittleEndian.PutUint32(overrun[8:12], 1<<20)
	cases["overrun length"] = overrun
	// A length inside the bound that still runs past the end of the buffer.
	short := append([]byte{}, good...)
	binary.LittleEndian.PutUint32(short[8:12], uint32(worktreePlanDelegateIDMax))
	cases["length past the end"] = short

	badMagic := append([]byte{}, good...)
	binary.LittleEndian.PutUint32(badMagic[0:4], 0xDEADBEEF)
	cases["wrong magic"] = badMagic

	badVersion := append([]byte{}, good...)
	badVersion[4] = wireVersion + 1
	cases["wrong version"] = badVersion

	for name, request := range cases {
		_, status := Handle(bus.ModuleInvocation{StageID: StageWorktreePlan}, request)
		if status != bus.ModuleStatusInvalidRequest {
			t.Errorf("%s: status = %v, want InvalidRequest", name, status)
		}
	}
}

func TestWorktreePlanStageHonoursCancellation(t *testing.T) {
	invocation := bus.ModuleInvocation{StageID: StageWorktreePlan, DeadlineNS: 1}
	if _, status := Handle(invocation, worktreePlanRequest(true, "d1")); status != bus.ModuleStatusCancelled {
		t.Errorf("status = %v, want Cancelled", status)
	}
}
