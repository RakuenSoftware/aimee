package delegates

import (
	"encoding/binary"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

// Linux's ceiling, 128+SIGRTMAX. The tests that care about the platform pass
// their own.
const linuxMaxSignalStatus int32 = 192

// Ported from test_delegate_verify.c.
func TestClassifyVerify(t *testing.T) {
	cases := []struct {
		name string
		rc   int32
		want VerifyOutcome
	}{
		{"clean run", 0, VerifyPass},
		{"could not spawn", -1, VerifyInfraError},
		{"ordinary test failure", 1, VerifyFailed},
		{"not executable", 126, VerifyInfraError},
		{"not found", 127, VerifyInfraError},
		{"timeout expiry", 124, VerifyInfraError},
		{"killed by SIGKILL", 137, VerifyInfraError},
		{"killed by SIGTERM", 143, VerifyInfraError},
		// 128 is not 128+N for any signal N, so it is an ordinary status.
		{"exit 128", 128, VerifyFailed},
		// Above the platform's signal ceiling a verifier may document its own
		// meaning; suppressing escalation there is the mistake this
		// classification exists to avoid, in the other direction.
		{"documented high exit", 200, VerifyFailed},
	}
	for _, c := range cases {
		if got := ClassifyVerify(c.rc, linuxMaxSignalStatus); got != c.want {
			t.Errorf("%s: rc %d -> %v, want %v", c.name, c.rc, got, c.want)
		}
	}
}

// The ceiling is the caller's, not a constant: the same status is a signal
// death on a platform with realtime signals and a deliberate exit without them.
func TestClassifyVerifyRespectsThePlatformCeiling(t *testing.T) {
	const noRealtimeSignals int32 = 159 // 128 + 31

	if got := ClassifyVerify(160, linuxMaxSignalStatus); got != VerifyInfraError {
		t.Errorf("160 under a Linux ceiling = %v, want infra_error", got)
	}
	if got := ClassifyVerify(160, noRealtimeSignals); got != VerifyFailed {
		t.Errorf("160 above a low ceiling = %v, want failed", got)
	}

	// The boundary itself, stated as the rule rather than as literals: the
	// ceiling is the last status a signal can produce, and one past it is an
	// ordinary exit again.
	for _, ceiling := range []int32{linuxMaxSignalStatus, noRealtimeSignals} {
		if got := ClassifyVerify(ceiling, ceiling); got != VerifyInfraError {
			t.Errorf("ceiling %d = %v, want infra_error", ceiling, got)
		}
		if got := ClassifyVerify(ceiling+1, ceiling); got != VerifyFailed {
			t.Errorf("ceiling+1 under %d = %v, want failed", ceiling, got)
		}
	}
	// 255 is above every plausible ceiling.
	if got := ClassifyVerify(255, linuxMaxSignalStatus); got != VerifyFailed {
		t.Errorf("255 = %v, want failed", got)
	}
}

func TestVerifyOutcomeNames(t *testing.T) {
	for outcome, want := range map[VerifyOutcome]string{
		VerifyPass:       "pass",
		VerifyFailed:     "failed",
		VerifyInfraError: "infra_error",
		VerifyOutcome(9): "unknown",
	} {
		if got := outcome.String(); got != want {
			t.Errorf("%d = %q, want %q", outcome, got, want)
		}
	}
}

func TestVerifyEscalationWarranted(t *testing.T) {
	if !VerifyEscalationWarranted(0, VerifyFailed) {
		t.Error("a completed run with a genuine verifier failure should be reported")
	}
	// An unfinished delegate run is an availability problem, not a statement
	// about the seat.
	if VerifyEscalationWarranted(1, VerifyFailed) {
		t.Error("a delegate run that did not complete was reported as a misplacement")
	}
	// Infrastructure says nothing about the work product, and no verifier at
	// all is no signal.
	if VerifyEscalationWarranted(0, VerifyInfraError) {
		t.Error("an infrastructure failure was reported as a misplacement")
	}
	if VerifyEscalationWarranted(0, VerifyPass) {
		t.Error("a passing verifier was reported as a misplacement")
	}
}

func verifyRequestBytes(execRC, delegateRC, maxSignal int32) []byte {
	request := make([]byte, verifyRequestLen)
	binary.LittleEndian.PutUint32(request[0:4], verifyRequestMagic)
	request[4] = wireVersion
	binary.LittleEndian.PutUint32(request[8:12], uint32(execRC))
	binary.LittleEndian.PutUint32(request[12:16], uint32(delegateRC))
	binary.LittleEndian.PutUint32(request[16:20], uint32(maxSignal))
	return request
}

func TestVerifyStageRoundTrip(t *testing.T) {
	response, status := Handle(bus.ModuleInvocation{StageID: StageVerify},
		verifyRequestBytes(1, 0, linuxMaxSignalStatus))
	if status != bus.ModuleStatusOK || len(response) != verifyResponseLen ||
		binary.LittleEndian.Uint32(response[0:4]) != verifyResponseMagic {
		t.Fatalf("status %d, %d bytes", status, len(response))
	}
	if VerifyOutcome(binary.LittleEndian.Uint32(response[4:8])) != VerifyFailed {
		t.Error("a test failure did not classify as failed")
	}
	if binary.LittleEndian.Uint32(response[8:12]) != 1 {
		t.Error("escalation was not reported for a genuine failure")
	}

	// A killed verifier is infrastructure, and carries no escalation.
	response, _ = Handle(bus.ModuleInvocation{StageID: StageVerify},
		verifyRequestBytes(137, 0, linuxMaxSignalStatus))
	if VerifyOutcome(binary.LittleEndian.Uint32(response[4:8])) != VerifyInfraError ||
		binary.LittleEndian.Uint32(response[8:12]) != 0 {
		t.Error("an OOM kill was blamed on the model")
	}
}

func TestVerifyStageRejectsInvalidEnvelope(t *testing.T) {
	short := verifyRequestBytes(0, 0, linuxMaxSignalStatus)[:verifyRequestLen-1]
	if _, status := Handle(bus.ModuleInvocation{StageID: StageVerify}, short); status != bus.ModuleStatusInvalidRequest {
		t.Errorf("truncated status = %d", status)
	}
	// A ceiling below 128 cannot be a signal status; taking it would silently
	// reclassify every killed verifier as a work-product failure.
	if _, status := Handle(bus.ModuleInvocation{StageID: StageVerify},
		verifyRequestBytes(137, 0, 12)); status != bus.ModuleStatusInvalidRequest {
		t.Errorf("nonsense ceiling status = %d", status)
	}
	if _, status := Handle(bus.ModuleInvocation{StageID: StageVerify, DeadlineNS: 1},
		verifyRequestBytes(0, 0, linuxMaxSignalStatus)); status != bus.ModuleStatusCancelled {
		t.Errorf("expired status = %d", status)
	}
}
