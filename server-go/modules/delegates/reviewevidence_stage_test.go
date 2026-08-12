package delegates

import (
	"encoding/binary"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func reviewRequest(role, response string, flags uint32) []byte {
	out := make([]byte, reviewEvidenceHeaderLen+len(role)+len(response))
	binary.LittleEndian.PutUint32(out[0:4], reviewEvidenceRequestMagic)
	out[4] = wireVersion
	binary.LittleEndian.PutUint32(out[8:12], flags)
	binary.LittleEndian.PutUint32(out[12:16], uint32(len(role)))
	copy(out[reviewEvidenceHeaderLen:], role)
	copy(out[reviewEvidenceHeaderLen+len(role):], response)
	return out
}

func decodeReviewVerdict(t *testing.T, b []byte) (uint32, string) {
	t.Helper()
	if len(b) < 8 || binary.LittleEndian.Uint32(b[0:4]) != reviewEvidenceResponseMagic {
		t.Fatalf("bad response: %v", b)
	}
	return binary.LittleEndian.Uint32(b[4:8]), string(b[8:])
}

func TestReviewEvidenceStageRoundTrip(t *testing.T) {
	request := reviewRequest("validate", "No uncommitted diff exists.", reviewFlagWorktreeDirty)
	response, status := handleReviewEvidence(bus.ModuleInvocation{}, request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status %v", status)
	}
	verdict, message := decodeReviewVerdict(t, response)
	if verdict&reviewVerdictGuarded == 0 || verdict&reviewVerdictCheckSnippets == 0 ||
		verdict&reviewVerdictContradiction == 0 {
		t.Fatalf("verdict %#x", verdict)
	}
	if message == "" {
		t.Error("a contradiction must carry its wording; the caller has none of its own")
	}
}

// A role that is guarded but not snippet-checked, with no contradiction: the
// caller gets "guarded, nothing to do", which is different from "not guarded".
func TestReviewEvidenceStageDistinguishesGuardedFromChecked(t *testing.T) {
	response, status := handleReviewEvidence(bus.ModuleInvocation{},
		reviewRequest("diagnose", "a quote from an old log", 0))
	if status != bus.ModuleStatusOK {
		t.Fatalf("status %v", status)
	}
	verdict, message := decodeReviewVerdict(t, response)
	if verdict&reviewVerdictGuarded == 0 {
		t.Error("diagnose is guarded")
	}
	if verdict&reviewVerdictCheckSnippets != 0 {
		t.Error("diagnose is not snippet-checked")
	}
	if message != "" {
		t.Errorf("no contradiction, so no wording: %q", message)
	}
}

func TestReviewEvidenceStageRejectsMalformedRequests(t *testing.T) {
	good := reviewRequest("review", "a report", 0)

	badMagic := append([]byte(nil), good...)
	binary.LittleEndian.PutUint32(badMagic[0:4], reviewEvidenceRequestMagic+1)

	badVersion := append([]byte(nil), good...)
	badVersion[4] = wireVersion + 1

	reservedSet := append([]byte(nil), good...)
	reservedSet[5] = 1

	unknownFlag := append([]byte(nil), good...)
	binary.LittleEndian.PutUint32(unknownFlag[8:12], reviewFlagsKnown|(1<<7))

	roleTooLong := append([]byte(nil), good...)
	binary.LittleEndian.PutUint32(roleTooLong[12:16], reviewRoleMax+1)

	rolePastEnd := append([]byte(nil), good...)
	binary.LittleEndian.PutUint32(rolePastEnd[12:16], uint32(len(good)))

	for name, request := range map[string][]byte{
		"bad magic":     badMagic,
		"bad version":   badVersion,
		"reserved byte": reservedSet,
		"unknown flag":  unknownFlag,
		"role too long": roleTooLong,
		"role past end": rolePastEnd,
		"short header":  good[:12],
		"empty":         nil,
	} {
		if _, status := handleReviewEvidence(bus.ModuleInvocation{}, request); status != bus.ModuleStatusInvalidRequest {
			t.Errorf("%s: want InvalidRequest, got %v", name, status)
		}
	}
}

func TestReviewEvidenceStageHonoursCancellation(t *testing.T) {
	request := reviewRequest("review", "a report", 0)
	if _, status := handleReviewEvidence(bus.ModuleInvocation{DeadlineNS: 1}, request); status != bus.ModuleStatusCancelled {
		t.Errorf("want Cancelled, got %v", status)
	}
}
