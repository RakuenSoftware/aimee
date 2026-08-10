package delegates

import (
	"encoding/binary"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

const validHandoff = `{"schema_version":"delegate_result_v1","status":"done",` +
	`"changed_files":["src/foo.c"],"outside_ownership_touches":[],` +
	`"tests":[{"name":"unit-test-foo","status":"passed"}],` +
	`"commands_run":["make unit-tests"],"supervisor_actions":[],` +
	`"summary":"Did the thing."}`

func handoffRequest(text, owned string, requireVerification bool) []byte {
	request := make([]byte, handoffHeaderLen+len(text)+len(owned))
	binary.LittleEndian.PutUint32(request[0:4], handoffRequestMagic)
	request[4] = wireVersion
	if requireVerification {
		request[5] = 1
	}
	binary.LittleEndian.PutUint32(request[8:12], uint32(len(text)))
	binary.LittleEndian.PutUint32(request[12:16], uint32(len(owned)))
	copy(request[handoffHeaderLen:], text)
	copy(request[handoffHeaderLen+len(text):], owned)
	return request
}

func decodeFixed(b []byte) string {
	if i := strings.IndexByte(string(b), 0); i >= 0 {
		return string(b[:i])
	}
	return string(b)
}

func handoffCall(t *testing.T, text, owned string, requireVerification bool) HandoffVerdict {
	t.Helper()
	response, status := Handle(bus.ModuleInvocation{StageID: StageHandoff},
		handoffRequest(text, owned, requireVerification))
	if status != bus.ModuleStatusOK || len(response) != handoffResponseLen ||
		binary.LittleEndian.Uint32(response[0:4]) != handoffResponseMagic {
		t.Fatalf("response len %d status %d", len(response), status)
	}
	return HandoffVerdict{
		Valid:                   binary.LittleEndian.Uint32(response[4:8]) == 1,
		RepairAttempted:         binary.LittleEndian.Uint32(response[8:12]) == 1,
		DoneWithoutVerification: binary.LittleEndian.Uint32(response[12:16]) == 1,
		NeedsSupervisorReview:   binary.LittleEndian.Uint32(response[16:20]) == 1,
		ChangedFilesCount:       int(binary.LittleEndian.Uint32(response[20:24])),
		OutsideOwnershipCount:   int(binary.LittleEndian.Uint32(response[24:28])),
		PassedTests:             int(binary.LittleEndian.Uint32(response[28:32])),
		CommandsRun:             int(binary.LittleEndian.Uint32(response[32:36])),
		Status:                  decodeFixed(response[36 : 36+handoffStatusLen]),
		RawStatus:               decodeFixed(response[36+handoffStatusLen : 36+2*handoffStatusLen]),
		Error:                   decodeFixed(response[36+2*handoffStatusLen:]),
	}
}

// Ported from test_delegate_handoff.c.
func TestValidHandoffParses(t *testing.T) {
	v := handoffCall(t, validHandoff, `["src/foo.c"]`, true)
	if !v.Valid || v.Status != "done" || v.RawStatus != "done" {
		t.Fatalf("verdict = %+v", v)
	}
	if v.ChangedFilesCount != 1 || v.CommandsRun != 1 || v.PassedTests != 1 {
		t.Errorf("counts = %d/%d/%d, want 1/1/1", v.ChangedFilesCount, v.CommandsRun, v.PassedTests)
	}
	if v.OutsideOwnershipCount != 0 || v.NeedsSupervisorReview {
		t.Errorf("clean handoff flagged: %+v", v)
	}
}

func TestMalformedHandoffsAreRejectedWithAReason(t *testing.T) {
	cases := []struct {
		name, text, wantErr string
	}{
		{"empty", "", "empty delegate handoff"},
		{"not json", "not json at all", "not valid JSON object"},
		{"wrong schema", `{"schema_version":"v2","status":"done"}`, "delegate_result_v1"},
		{"unknown status",
			`{"schema_version":"delegate_result_v1","status":"finished"}`, "invalid handoff status"},
		{"missing arrays",
			`{"schema_version":"delegate_result_v1","status":"done","summary":"x"}`,
			"missing required fields"},
		// A summary of only whitespace says nothing; same as absent.
		{"blank summary",
			`{"schema_version":"delegate_result_v1","status":"done","changed_files":[],` +
				`"tests":[],"summary":"   "}`, "missing required fields"},
	}
	for _, c := range cases {
		v := handoffCall(t, c.text, "", true)
		if v.Valid {
			t.Errorf("%s: accepted", c.name)
		}
		if !strings.Contains(v.Error, c.wantErr) {
			t.Errorf("%s: error = %q, want it to mention %q", c.name, v.Error, c.wantErr)
		}
		// Every rejection routes to a human rather than passing silently.
		if v.Status != statusNeedsReview || !v.NeedsSupervisorReview {
			t.Errorf("%s: status = %q, want supervisor review", c.name, v.Status)
		}
	}
}

// supervisor_actions is optional, but a non-array is malformed rather than absent.
func TestSupervisorActionsOptionalButTyped(t *testing.T) {
	without := `{"schema_version":"delegate_result_v1","status":"done",` +
		`"changed_files":["src/foo.c"],"tests":[{"status":"passed"}],"summary":"ok"}`
	if v := handoffCall(t, without, "", true); !v.Valid {
		t.Errorf("absent supervisor_actions rejected: %q", v.Error)
	}
	wrongType := `{"schema_version":"delegate_result_v1","status":"done",` +
		`"changed_files":[],"tests":[],"supervisor_actions":"nope","summary":"ok"}`
	if v := handoffCall(t, wrongType, "", true); v.Valid {
		t.Error("a non-array supervisor_actions was accepted")
	}
}

// A delegate does not get to call its own work done with nothing green.
func TestDoneWithoutVerificationDowngrades(t *testing.T) {
	unverified := `{"schema_version":"delegate_result_v1","status":"done",` +
		`"changed_files":["src/foo.c"],"tests":[],"supervisor_actions":[],"summary":"ok"}`

	v := handoffCall(t, unverified, "", true)
	if !v.Valid || v.Status != "partial" || !v.DoneWithoutVerification {
		t.Errorf("verdict = %+v, want a partial downgrade", v)
	}

	v = handoffCall(t, unverified, "", false)
	if v.Status != "done" || v.DoneWithoutVerification {
		t.Errorf("verdict = %+v, want done when verification is not required", v)
	}
}

// Both spellings are written by delegates; rejecting one would read a verified
// run as unverified and downgrade it wrongly.
func TestBothPassSpellingsCount(t *testing.T) {
	for _, spelling := range []string{"pass", "passed"} {
		text := `{"schema_version":"delegate_result_v1","status":"done",` +
			`"changed_files":[],"tests":[{"status":"` + spelling + `"}],"summary":"ok"}`
		if v := handoffCall(t, text, "", true); v.PassedTests != 1 || v.DoneWithoutVerification {
			t.Errorf("%q: passed=%d downgraded=%v", spelling, v.PassedTests,
				v.DoneWithoutVerification)
		}
	}
}

func TestOutsideOwnershipNeedsReview(t *testing.T) {
	text := `{"schema_version":"delegate_result_v1","status":"done",` +
		`"changed_files":["src/foo.c","src/bar.c"],"outside_ownership_touches":[],` +
		`"tests":[{"name":"unit-test-foo","status":"passed"}],"supervisor_actions":[],` +
		`"summary":"Touched one extra file."}`
	v := handoffCall(t, text, `["src/foo.c"]`, true)
	if !v.Valid || v.Status != statusNeedsReview || !v.NeedsSupervisorReview {
		t.Fatalf("verdict = %+v", v)
	}
	if v.OutsideOwnershipCount != 1 || !strings.Contains(v.Error, "outside owned_files") {
		t.Errorf("count = %d, error = %q", v.OutsideOwnershipCount, v.Error)
	}
}

// A file the delegate ADMITTED touching counts once, not twice for also being
// in changed_files.
func TestAdmittedTouchCountsOnce(t *testing.T) {
	text := `{"schema_version":"delegate_result_v1","status":"done",` +
		`"changed_files":["src/foo.c","src/bar.c"],"outside_ownership_touches":["src/bar.c"],` +
		`"tests":[{"status":"passed"}],"summary":"ok"}`
	if v := handoffCall(t, text, `["src/foo.c"]`, true); v.OutsideOwnershipCount != 1 {
		t.Errorf("count = %d, want 1", v.OutsideOwnershipCount)
	}
}

// With no ownership list, ownership was never scoped: inferring violations from
// an absent list would flag every ordinary run.
func TestUnscopedOwnershipCountsOnlyAdmittedTouches(t *testing.T) {
	text := `{"schema_version":"delegate_result_v1","status":"done",` +
		`"changed_files":["src/a.c","src/b.c"],"outside_ownership_touches":[],` +
		`"tests":[{"status":"passed"}],"summary":"ok"}`
	for _, owned := range []string{"", "[]"} {
		v := handoffCall(t, text, owned, true)
		if v.OutsideOwnershipCount != 0 || v.NeedsSupervisorReview {
			t.Errorf("owned=%q: count = %d, review = %v", owned, v.OutsideOwnershipCount,
				v.NeedsSupervisorReview)
		}
	}
}

// Ownership outranks the verification downgrade: both apply, and the operator
// needs the more serious one.
func TestOwnershipOutranksTheVerificationDowngrade(t *testing.T) {
	text := `{"schema_version":"delegate_result_v1","status":"done",` +
		`"changed_files":["src/bar.c"],"outside_ownership_touches":[],"tests":[],` +
		`"summary":"ok"}`
	v := handoffCall(t, text, `["src/foo.c"]`, true)
	if v.Status != statusNeedsReview || v.DoneWithoutVerification {
		t.Errorf("verdict = %+v, want the ownership verdict to win", v)
	}
}

func TestHandoffRejectsInvalidEnvelope(t *testing.T) {
	short := handoffRequest(validHandoff, "", true)[:handoffHeaderLen-1]
	if _, status := Handle(bus.ModuleInvocation{StageID: StageHandoff}, short); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("truncated status = %d", status)
	}
	lying := handoffRequest(validHandoff, "", true)
	binary.LittleEndian.PutUint32(lying[8:12], 9999)
	if _, status := Handle(bus.ModuleInvocation{StageID: StageHandoff}, lying); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("length-mismatch status = %d", status)
	}
	if _, status := Handle(bus.ModuleInvocation{StageID: StageHandoff, DeadlineNS: 1},
		handoffRequest(validHandoff, "", true)); status != bus.ModuleStatusCancelled {
		t.Fatalf("expired status = %d", status)
	}
}
