package delegates

import (
	"encoding/binary"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func economicsRequestBytes(tasks []EconomicsTask, agents []AgentTier) []byte {
	request := make([]byte, economicsReqHeaderLen)
	binary.LittleEndian.PutUint32(request[0:4], economicsRequestMagic)
	request[4] = wireVersion
	binary.LittleEndian.PutUint32(request[8:12], uint32(len(tasks)))
	binary.LittleEndian.PutUint32(request[12:16], uint32(len(agents)))
	for _, t := range tasks {
		var hdr [12]byte
		binary.LittleEndian.PutUint16(hdr[0:2], uint16(len(t.Status)))
		binary.LittleEndian.PutUint16(hdr[2:4], uint16(len(t.ClaimedBy)))
		binary.LittleEndian.PutUint32(hdr[4:8], uint32(len(t.Files)))
		binary.LittleEndian.PutUint32(hdr[8:12], uint32(len(t.Result)))
		request = append(request, hdr[:]...)
		request = append(request, t.Status...)
		request = append(request, t.ClaimedBy...)
		request = append(request, t.Files...)
		request = append(request, t.Result...)
	}
	for _, a := range agents {
		var n [2]byte
		binary.LittleEndian.PutUint16(n[:], uint16(len(a.Name)))
		request = append(request, n[:]...)
		request = append(request, a.Name...)
		var tier [4]byte
		binary.LittleEndian.PutUint32(tier[:], uint32(int32(a.Tier)))
		request = append(request, tier[:]...)
	}
	return request
}

func TestEconomicsStageRoundTrip(t *testing.T) {
	tasks := []EconomicsTask{
		{Status: "done", ClaimedBy: "cheap", Files: `["src/a.c"]`,
			Result: doneHandoff(1, `"src/a.c"`, "")},
		{Status: "failed", ClaimedBy: "cheap"},
	}
	agents := []AgentTier{{Name: "cheap", Tier: 0}}

	response, status := Handle(bus.ModuleInvocation{StageID: StageEconomics},
		economicsRequestBytes(tasks, agents))
	if status != bus.ModuleStatusOK || len(response) != economicsResponseLen ||
		binary.LittleEndian.Uint32(response[0:4]) != economicsResponseMagic {
		t.Fatalf("status %d, %d bytes", status, len(response))
	}

	want := BuildEconomicsReport(tasks, agents)
	if got := int(binary.LittleEndian.Uint32(response[4:8])); got != want.DelegateCount {
		t.Errorf("delegate count = %d, want %d", got, want.DelegateCount)
	}
	at := 4 + 19*4
	if got := decodeFixed(response[at : at+economicsVerdictLen]); got != want.Verdict {
		t.Errorf("verdict = %q, want %q", got, want.Verdict)
	}
	if got := decodeFixed(response[at+economicsVerdictLen:]); got != want.Recommendation {
		t.Errorf("recommendation = %q, want %q", got, want.Recommendation)
	}
}

func TestEconomicsStageRejectsInvalidEnvelope(t *testing.T) {
	good := economicsRequestBytes([]EconomicsTask{{Status: "done", ClaimedBy: "a"}}, nil)

	if _, status := Handle(bus.ModuleInvocation{StageID: StageEconomics},
		good[:economicsReqHeaderLen-1]); status != bus.ModuleStatusInvalidRequest {
		t.Errorf("truncated status = %d", status)
	}

	// A task count that outruns the body must be refused, not read past.
	lying := economicsRequestBytes([]EconomicsTask{{Status: "done"}}, nil)
	binary.LittleEndian.PutUint32(lying[8:12], 99)
	if _, status := Handle(bus.ModuleInvocation{StageID: StageEconomics}, lying); status != bus.ModuleStatusInvalidRequest {
		t.Errorf("overrunning task count status = %d", status)
	}

	// Trailing bytes mean the caller and the module disagree about the frame.
	trailing := append(economicsRequestBytes(nil, nil), 0)
	if _, status := Handle(bus.ModuleInvocation{StageID: StageEconomics}, trailing); status != bus.ModuleStatusInvalidRequest {
		t.Errorf("trailing-byte status = %d", status)
	}

	if _, status := Handle(bus.ModuleInvocation{StageID: StageEconomics, DeadlineNS: 1},
		good); status != bus.ModuleStatusCancelled {
		t.Errorf("expired status = %d", status)
	}
}

// An empty run is answerable, not an error: no delegates ran, so nothing is
// established either way.
func TestEconomicsStageEmptyRun(t *testing.T) {
	response, status := Handle(bus.ModuleInvocation{StageID: StageEconomics},
		economicsRequestBytes(nil, nil))
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %d", status)
	}
	at := 4 + 19*4
	if v := decodeFixed(response[at : at+economicsVerdictLen]); v != "unclear" {
		t.Errorf("verdict = %q, want unclear", v)
	}
}
