package delegates

import (
	"encoding/binary"
	"encoding/json"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
)

// Whether a delegate's structured handoff can be believed.
//
// A delegate reports what it did as JSON. This decides whether that report is
// well-formed, and then whether it can be taken at face value: one that edited
// files it was not given, or claims "done" with nothing verified, is downgraded
// rather than trusted. Both downgrades exist because the delegate is grading its
// own work.

const (
	StageHandoff uint32 = 5
	EventHandoff uint32 = 6661

	handoffRequestMagic  uint32 = 0x444e4844 /* "DHND" */
	handoffResponseMagic uint32 = 0x564e4844 /* "DHNV" */
	handoffHeaderLen            = 16
	handoffStatusLen            = 32
	handoffErrorLen             = 256
	handoffResponseLen          = 36 + handoffStatusLen*2 + handoffErrorLen
	handoffTextMax              = 1 << 20

	statusNeedsReview = "needs_supervisor_review"
)

// HandoffVerdict mirrors delegate_handoff_validation_t.
type HandoffVerdict struct {
	Valid                   bool
	RepairAttempted         bool
	DoneWithoutVerification bool
	NeedsSupervisorReview   bool
	ChangedFilesCount       int
	OutsideOwnershipCount   int
	PassedTests             int
	CommandsRun             int
	Status                  string
	RawStatus               string
	Error                   string
}

func (v *HandoffVerdict) fail(msg string) {
	v.Error = msg
	v.Status = statusNeedsReview
	v.NeedsSupervisorReview = true
}

func statusAllowed(s string) bool {
	switch s {
	case "done", "partial", "blocked", "failed":
		return true
	}
	return false
}

// countNonEmptyStrings ignores blank entries: a blank names no file and is not
// evidence of one.
func countNonEmptyStrings(raw []any) int {
	n := 0
	for _, item := range raw {
		if s, ok := item.(string); ok && s != "" {
			n++
		}
	}
	return n
}

func containsString(raw []any, needle string) bool {
	if needle == "" {
		return false
	}
	for _, item := range raw {
		if s, ok := item.(string); ok && s == needle {
			return true
		}
	}
	return false
}

// passedTestCount accepts both "passed" and "pass": delegates write either, and
// rejecting one spelling would read a verified run as unverified.
func passedTestCount(raw []any) int {
	n := 0
	for _, item := range raw {
		obj, ok := item.(map[string]any)
		if !ok {
			continue
		}
		if s, ok := obj["status"].(string); ok && (s == "passed" || s == "pass") {
			n++
		}
	}
	return n
}

// outsideOwnedCount counts edits the delegate was not entitled to make: what it
// admitted to, plus anything it changed that its ownership list does not cover
// and it did not already declare. An absent or empty ownership list means
// ownership was never scoped, so only admitted touches count -- inferring
// violations from a missing list would flag every ordinary run.
func outsideOwnedCount(changed, outside, owned []any, ownedPresent bool) int {
	count := countNonEmptyStrings(outside)
	if !ownedPresent || len(owned) == 0 {
		return count
	}
	for _, item := range changed {
		s, ok := item.(string)
		if !ok || s == "" {
			continue
		}
		if !containsString(owned, s) && !containsString(outside, s) {
			count++
		}
	}
	return count
}

func asArray(v any) ([]any, bool) {
	arr, ok := v.([]any)
	return arr, ok
}

// ValidateHandoff parses a delegate's handoff and judges it. ok=false means the
// report is malformed, which is a different thing from a well-formed report that
// must not be trusted; the caller needs those apart.
func ValidateHandoff(text, ownedFilesJSON string, requireVerification bool) (v HandoffVerdict, ok bool) {
	v.Status = statusNeedsReview

	if text == "" {
		v.fail("empty delegate handoff")
		return v, false
	}
	var root map[string]any
	if err := json.Unmarshal([]byte(text), &root); err != nil || root == nil {
		v.fail("handoff is not valid JSON object")
		return v, false
	}
	if s, isString := root["schema_version"].(string); !isString || s != "delegate_result_v1" {
		v.fail("missing schema_version delegate_result_v1")
		return v, false
	}
	status, isString := root["status"].(string)
	if !isString || !statusAllowed(status) {
		v.fail("invalid handoff status")
		return v, false
	}
	v.RawStatus = status
	v.Status = status

	changed, changedOK := asArray(root["changed_files"])
	tests, testsOK := asArray(root["tests"])
	summary, summaryOK := root["summary"].(string)
	supervisor, supervisorPresent := root["supervisor_actions"]
	_, supervisorIsArray := asArray(supervisor)
	if !changedOK || !testsOK || (supervisorPresent && supervisor != nil && !supervisorIsArray) ||
		!summaryOK || strings.TrimSpace(summary) == "" {
		v.fail("handoff missing required fields")
		return v, false
	}

	commands, _ := asArray(root["commands_run"])
	outside, _ := asArray(root["outside_ownership_touches"])
	v.ChangedFilesCount = countNonEmptyStrings(changed)
	v.CommandsRun = len(commands)
	v.PassedTests = passedTestCount(tests)

	var owned []any
	ownedPresent := false
	if ownedFilesJSON != "" {
		if err := json.Unmarshal([]byte(ownedFilesJSON), &owned); err == nil {
			ownedPresent = true
		}
	}
	v.OutsideOwnershipCount = outsideOwnedCount(changed, outside, owned, ownedPresent)

	v.Valid = true
	switch {
	case v.OutsideOwnershipCount > 0:
		v.Status = statusNeedsReview
		v.Error = "handoff touched files outside owned_files"
		v.NeedsSupervisorReview = true
	case v.RawStatus == "done" && requireVerification && v.PassedTests == 0:
		// A delegate grading its own work does not get to call it done with
		// nothing green. Downgraded, not rejected: the work may be fine, but it
		// is unverified and must be read that way.
		v.Status = "partial"
		v.Error = "status=done without passed focused verification; downgraded to partial"
		v.DoneWithoutVerification = true
	}
	return v, true
}

func putBool(dst []byte, b bool) {
	if b {
		binary.LittleEndian.PutUint32(dst, 1)
	}
}

func putFixed(dst []byte, s string) {
	if len(s) >= len(dst) {
		s = s[:len(dst)-1]
	}
	copy(dst, s)
}

func handleHandoff(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if len(request) < handoffHeaderLen ||
		binary.LittleEndian.Uint32(request[0:4]) != handoffRequestMagic ||
		request[4] != wireVersion || request[5] > 1 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	textLen := int(binary.LittleEndian.Uint32(request[8:12]))
	ownedLen := int(binary.LittleEndian.Uint32(request[12:16]))
	if textLen > handoffTextMax || ownedLen > handoffTextMax ||
		len(request) != handoffHeaderLen+textLen+ownedLen {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	text := string(request[handoffHeaderLen : handoffHeaderLen+textLen])
	owned := string(request[handoffHeaderLen+textLen:])
	verdict, _ := ValidateHandoff(text, owned, request[5] == 1)

	// A malformed handoff is a verdict, not a transport failure: the caller
	// needs the reason, and the reason travels in the body.
	response := make([]byte, handoffResponseLen)
	binary.LittleEndian.PutUint32(response[0:4], handoffResponseMagic)
	putBool(response[4:8], verdict.Valid)
	putBool(response[8:12], verdict.RepairAttempted)
	putBool(response[12:16], verdict.DoneWithoutVerification)
	putBool(response[16:20], verdict.NeedsSupervisorReview)
	binary.LittleEndian.PutUint32(response[20:24], uint32(verdict.ChangedFilesCount))
	binary.LittleEndian.PutUint32(response[24:28], uint32(verdict.OutsideOwnershipCount))
	binary.LittleEndian.PutUint32(response[28:32], uint32(verdict.PassedTests))
	binary.LittleEndian.PutUint32(response[32:36], uint32(verdict.CommandsRun))
	putFixed(response[36:36+handoffStatusLen], verdict.Status)
	putFixed(response[36+handoffStatusLen:36+2*handoffStatusLen], verdict.RawStatus)
	putFixed(response[36+2*handoffStatusLen:], verdict.Error)
	return response, bus.ModuleStatusOK
}
