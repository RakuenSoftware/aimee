package delegates

import (
	"encoding/binary"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func mayWriteRequest(role, prompt string) []byte {
	out := make([]byte, mayWriteReqHeaderLen)
	binary.LittleEndian.PutUint32(out[0:4], mayWriteRequestMagic)
	out[4] = wireVersion
	binary.LittleEndian.PutUint32(out[8:12], uint32(len(role)))
	binary.LittleEndian.PutUint32(out[12:16], uint32(len(prompt)))
	out = append(out, role...)
	return append(out, prompt...)
}

func callMayWrite(t *testing.T, role, prompt string) (may, byRole, byPrompt bool) {
	t.Helper()
	response, status := Handle(bus.ModuleInvocation{StageID: StageMayWrite},
		mayWriteRequest(role, prompt))
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v, want OK", status)
	}
	if len(response) != mayWriteResponseLen ||
		binary.LittleEndian.Uint32(response[0:4]) != mayWriteResponseMagic {
		t.Fatal("bad response")
	}
	return binary.LittleEndian.Uint32(response[4:8]) == 1,
		binary.LittleEndian.Uint32(response[8:12]) == 1,
		binary.LittleEndian.Uint32(response[12:16]) == 1
}

func TestMayWriteStageComposesBothInputs(t *testing.T) {
	may, byRole, byPrompt := callMayWrite(t, "code", "fix the parser")
	if !may || !byRole || !byPrompt {
		t.Errorf("may=%v role=%v prompt=%v, want all true", may, byRole, byPrompt)
	}

	// The role permits it; the brief does not ask for it.
	may, byRole, byPrompt = callMayWrite(t, "code", "read-only: review the parser")
	if may {
		t.Error("a write role told to read was allowed to write")
	}
	if !byRole || byPrompt {
		t.Errorf("halves wrong: role=%v prompt=%v", byRole, byPrompt)
	}

	// The brief asks; the role does not permit.
	may, byRole, byPrompt = callMayWrite(t, "review", "fix the parser")
	if may {
		t.Error("a read-only role was allowed to write by its brief")
	}
	if byRole || !byPrompt {
		t.Errorf("halves wrong: role=%v prompt=%v", byRole, byPrompt)
	}
}

// The halves are what make a refusal debuggable: "the delegate could not edit
// anything" is otherwise a mystery.
func TestMayWriteStageReportsWhichHalfWithheld(t *testing.T) {
	_, byRole, byPrompt := callMayWrite(t, "review", "summarise it")
	if byRole || byPrompt {
		t.Errorf("both halves should be false: role=%v prompt=%v", byRole, byPrompt)
	}
}

// A brief is carried whole, so a large one must still be answered.
func TestMayWriteStageHandlesALargeBrief(t *testing.T) {
	prompt := strings.Repeat("context. ", 20000) + "implement the fix"
	if may, _, _ := callMayWrite(t, "code", prompt); !may {
		t.Error("a large brief that asks for a change was refused")
	}
}

func TestMayWriteStageRejectsMalformedRequests(t *testing.T) {
	good := mayWriteRequest("code", "fix it")

	cases := map[string][]byte{
		"empty":         {},
		"short header":  good[:8],
		"trailing byte": append(append([]byte{}, good...), 0),
		"truncated":     good[:len(good)-1],
	}
	badMagic := append([]byte{}, good...)
	binary.LittleEndian.PutUint32(badMagic[0:4], 0xDEADBEEF)
	cases["wrong magic"] = badMagic

	badVersion := append([]byte{}, good...)
	badVersion[4] = wireVersion + 1
	cases["wrong version"] = badVersion

	longRole := append([]byte{}, good...)
	binary.LittleEndian.PutUint32(longRole[8:12], uint32(roleMax+1))
	cases["oversized role"] = longRole

	overrun := append([]byte{}, good...)
	binary.LittleEndian.PutUint32(overrun[12:16], 1<<20)
	cases["prompt length overruns"] = overrun

	for name, request := range cases {
		if _, status := Handle(bus.ModuleInvocation{StageID: StageMayWrite}, request); status != bus.ModuleStatusInvalidRequest {
			t.Errorf("%s: status = %v, want InvalidRequest", name, status)
		}
	}
}

func TestMayWriteStageHonoursCancellation(t *testing.T) {
	invocation := bus.ModuleInvocation{StageID: StageMayWrite, DeadlineNS: 1}
	if _, status := Handle(invocation, mayWriteRequest("code", "fix it")); status != bus.ModuleStatusCancelled {
		t.Errorf("status = %v, want Cancelled", status)
	}
}
