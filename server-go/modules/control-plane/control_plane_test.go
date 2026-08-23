package controlplane

import (
	"encoding/binary"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func TestHealthAnswersItsOwnStage(t *testing.T) {
	reply, status := Handle(
		bus.ModuleInvocation{StageID: StageHealth}, EncodeHealthRequest())
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if len(reply) != responseLen {
		t.Fatalf("reply = %d bytes, want %d", len(reply), responseLen)
	}
	if binary.LittleEndian.Uint32(reply[0:4]) != responseMagic ||
		binary.LittleEndian.Uint32(reply[4:8]) != wireVersion {
		t.Fatal("reply header is not this module's")
	}
}

func TestHealthRefusesAnotherStage(t *testing.T) {
	// A module that answers a stage it does not serve returns plausible
	// nonsense, which is worse than capability-absent: the caller gets an
	// answer and believes it.
	if _, status := Handle(
		bus.ModuleInvocation{StageID: 99}, EncodeHealthRequest()); status !=
		bus.ModuleStatusCapabilityAbsent {
		t.Fatalf("status = %v for a stage this does not serve", status)
	}
}

func TestHealthRefusesAMalformedRequest(t *testing.T) {
	for name, body := range map[string][]byte{
		"empty": {},
		"short": {1, 2, 3},
		"wrong magic": binary.LittleEndian.AppendUint32(
			binary.LittleEndian.AppendUint32(nil, 0xdeadbeef), wireVersion),
		"wrong version": binary.LittleEndian.AppendUint32(
			binary.LittleEndian.AppendUint32(nil, requestMagic), 99),
	} {
		if _, status := Handle(
			bus.ModuleInvocation{StageID: StageHealth}, body); status !=
			bus.ModuleStatusInvalidRequest {
			t.Errorf("%s: status = %v, want invalid-request", name, status)
		}
	}
}

func TestTheEventKindFollowsTheRegistryRule(t *testing.T) {
	// 4097 + 256*ref + (stage - 1), with control-plane at principal ref 32.
	// Derived here rather than transcribed, so a kind that drifts from the
	// registry fails rather than being discovered by a caller that gets no
	// answer.
	const ref = 32
	if want := uint32(4097 + 256*ref + int(StageHealth) - 1); EventHealth != want {
		t.Fatalf("EventHealth = %d, want %d", EventHealth, want)
	}
}
