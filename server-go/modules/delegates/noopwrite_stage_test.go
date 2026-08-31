package delegates

import (
	"encoding/binary"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func noopWriteRequest(flags uint32, namedCount int) []byte {
	out := make([]byte, noopWriteReqLen)
	binary.LittleEndian.PutUint32(out[0:4], noopWriteRequestMagic)
	out[4] = wireVersion
	binary.LittleEndian.PutUint32(out[8:12], flags)
	binary.LittleEndian.PutUint32(out[12:16], uint32(int32(namedCount)))
	return out
}

func callNoopWrite(t *testing.T, flags uint32, namedCount int) NoopWriteVerdict {
	t.Helper()
	response, status := Handle(bus.ModuleInvocation{StageID: StageNoopWrite},
		noopWriteRequest(flags, namedCount))
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v, want OK", status)
	}
	if len(response) < 12 || binary.LittleEndian.Uint32(response[0:4]) != noopWriteResponseMagic {
		t.Fatal("bad response")
	}
	return NoopWriteVerdict{
		Noop:    binary.LittleEndian.Uint32(response[4:8]) == 1,
		Benign:  binary.LittleEndian.Uint32(response[8:12]) == 1,
		Message: string(response[12:]),
	}
}

const noopWriteRun = noopFlagWritesAllowed | noopFlagSucceeded |
	noopFlagHeadSnapshot

func TestNoopWriteStageCatchesTheEmptyRun(t *testing.T) {
	got := callNoopWrite(t, noopWriteRun, 0)
	if !got.Noop {
		t.Fatalf("%+v, want a no-op", got)
	}
	if got.Message == "" {
		t.Error("a refusal with no explanation")
	}
}

func TestNoopWriteStagePassesRealWork(t *testing.T) {
	if got := callNoopWrite(t, noopWriteRun|noopFlagWorktreeDirty, 0); got.Noop {
		t.Errorf("%+v, want a pass", got)
	}
	if got := callNoopWrite(t, noopWriteRun|noopFlagAnyNamed, 3); got.Noop {
		t.Errorf("%+v, want a pass", got)
	}
}

// The benign cases carry the wording that is the only evidence the guard ran.
func TestNoopWriteStageCarriesTheBenignMessage(t *testing.T) {
	got := callNoopWrite(t, noopWriteRun|noopFlagHeadAdvanced, 2)
	if got.Noop || !got.Benign {
		t.Fatalf("%+v, want benign", got)
	}
	if got.Message == "" {
		t.Error("the benign case arrived with no wording")
	}
}

// An unknown flag means the caller thinks it is sending evidence this module
// does not read. Judging on a partial view is worse than not judging.
func TestNoopWriteStageRefusesUnknownFlags(t *testing.T) {
	request := noopWriteRequest(noopWriteRun|(1<<20), 0)
	if _, status := Handle(bus.ModuleInvocation{StageID: StageNoopWrite}, request); status != bus.ModuleStatusInvalidRequest {
		t.Errorf("status = %v, want InvalidRequest", status)
	}
}

func TestNoopWriteStageRejectsMalformedRequests(t *testing.T) {
	good := noopWriteRequest(noopWriteRun, 0)

	cases := map[string][]byte{
		"empty":         {},
		"short":         good[:8],
		"trailing byte": append(append([]byte{}, good...), 0),
	}
	badMagic := append([]byte{}, good...)
	binary.LittleEndian.PutUint32(badMagic[0:4], 0xDEADBEEF)
	cases["wrong magic"] = badMagic

	badVersion := append([]byte{}, good...)
	badVersion[4] = wireVersion + 1
	cases["wrong version"] = badVersion

	negative := append([]byte{}, good...)
	minusOne := int32(-1)
	binary.LittleEndian.PutUint32(negative[12:16], uint32(minusOne))
	cases["negative named count"] = negative

	for name, request := range cases {
		if _, status := Handle(bus.ModuleInvocation{StageID: StageNoopWrite}, request); status != bus.ModuleStatusInvalidRequest {
			t.Errorf("%s: status = %v, want InvalidRequest", name, status)
		}
	}
}

func TestNoopWriteStageHonoursCancellation(t *testing.T) {
	invocation := bus.ModuleInvocation{StageID: StageNoopWrite, DeadlineNS: 1}
	if _, status := Handle(invocation, noopWriteRequest(noopWriteRun, 0)); status != bus.ModuleStatusCancelled {
		t.Errorf("status = %v, want Cancelled", status)
	}
}
