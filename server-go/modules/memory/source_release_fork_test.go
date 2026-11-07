package memory

import (
	"bytes"
	"encoding/json"
	"time"

	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
	"strings"
	"testing"
)

func TestNativeInheritedSourceTicketRequiresExplicitBindingTransfer(t *testing.T) {
	state := &sourceReleaseState{}
	owner := sourceReleaseArgs(map[string]any{"request_id": "outer-admitted-request", "principal": "uid:1001", "caller_subject": "webuser:operator", "workspace": "/registered/workspace", "project": "local:project"})
	grades := json.RawMessage(`{"schema_version":1,"principal":"webuser:operator","scope_kind":"project","scope_id":"local:project","coverage":"recent_bounded","items":[{"source":{"event_id":"terminal"},"verification_ref":"proof"}]}`)
	assembly := map[string]any{"execution_test_grades": grades}
	inherited, err := state.prepare(owner, assembly)
	if err != nil || inherited == "" {
		t.Fatal("initial owner", err)
	}
	callback := sourceReleaseArgs(map[string]any{"request_id": "new-native-http-request", "principal": "uid:1001", "caller_subject": "webuser:operator", "workspace": "/registered/workspace", "project": "local:project", "source_release_ticket": inherited})
	if ticket, err := state.prepare(callback, assembly); err == nil || ticket != "" || !strings.Contains(err.Error(), "previous source release unavailable") {
		t.Fatal("changed callback binding did not reproduce refusal", err)
	}
	owner["source_release_ticket"], _ = json.Marshal(inherited)
	if ticket, err := state.prepare(owner, assembly); err != nil || ticket != inherited {
		t.Fatal("original authority damaged", err)
	}
}

func sourceForkCall(t *testing.T, state *sourceReleaseState, args commandArgs) map[string]any {
	t.Helper()
	raw, status := handleSourceReleaseFork(state, args)
	if status != ring.ModuleStatusOK {
		t.Fatal(status)
	}
	body, err := ring.DecodeCommandResult(raw)
	var result map[string]any
	if err != nil || json.Unmarshal(body, &result) != nil {
		t.Fatal(err)
	}
	return result
}
func sourceForkPrivateRef() typedProjectionRef {
	private := releaseTestRef()
	private.Channel = "native_preferences"
	private.Source.Kind = "user_memory_record"
	return private
}
func sourceForkOwner(t *testing.T) (*sourceReleaseState, commandArgs, string, json.RawMessage) {
	t.Helper()
	state := &sourceReleaseState{}
	args := sourceReleaseArgs(map[string]any{"request_id": "outer-delegate-request", "principal": "uid:1001", "caller_subject": "webuser:operator", "project": "local:project", "workspace": "/registered/worktree"})
	grades := json.RawMessage(`{"schema_version":1,"principal":"webuser:operator","scope_kind":"project","scope_id":"local:project","coverage":"recent_bounded","items":[{"source":{"event_id":"terminal"},"verification_ref":"proof"}]}`)
	token, err := state.prepare(args, map[string]any{"execution_test_grades": grades, "facts_projection": map[string]any{"retained_items": []typedProjectionRef{releaseTestRef(), sourceForkPrivateRef()}}})
	if err != nil {
		t.Fatal(err)
	}
	args["source_release_ticket"], _ = json.Marshal(token)
	args["new_request_id"] = json.RawMessage(`"native-callback-1"`)
	args["operation"] = json.RawMessage(`"source-release-fork-plan"`)
	return state, args, token, grades
}
func sourceForkAnswers(t *testing.T, args commandArgs, plan map[string]any, grades json.RawMessage) {
	t.Helper()
	check := plan["request"].(map[string]any)["revalidation"].(map[string]any)["check_id"]
	args["local_response"], _ = json.Marshal(map[string]any{"status": "ok", "eligible": true, "check_id": check, "sources_digest": releaseDigest([]typedProjectionRef{sourceForkPrivateRef()})})
	args["owner_response"], _ = json.Marshal(map[string]any{"status": "ok", "eligible": true, "check_id": check, "sources_digest": releaseDigest([]typedProjectionRef{releaseTestRef()})})
	var answer map[string]any
	if json.Unmarshal(grades, &answer) != nil {
		t.Fatal("grades")
	}
	answer["check_id"] = check
	args["execution_grade_response"], _ = json.Marshal(answer)
	args["operation"] = json.RawMessage(`"source-release-fork-result"`)
	args["fork_ticket"], _ = json.Marshal(plan["fork_ticket"])
}
func TestSourceReleaseForkFreshDelegateAndParentIsolation(t *testing.T) {
	state, args, parent, grades := sourceForkOwner(t)
	original := state.entries[parent]
	original.admitted = "parent-admission"
	original.guardedAdmission = "parent-guard"
	original.guardedAdmissionAt = time.Now()
	original.healthAttempt = "parent-attempt"
	expiry := original.expires
	originalBytes := state.bytes
	plan := sourceForkCall(t, state, args)
	token, ok := plan["fork_ticket"].(string)
	if !ok || !releaseTokenValid(token) || plan["new_request_id"] != "native-callback-1" {
		t.Fatal("fork plan", plan)
	}
	if _, usable := state.entries[token]; usable {
		t.Fatal("pending fork became source authority")
	}
	candidate := state.forks[token].entry
	if !candidate.expires.Equal(expiry) || candidate.admitted != "" || candidate.guardedAdmission != "" || candidate.healthAttempt != "" || candidate.previous != "" {
		t.Fatal("inherited dispatch/lifecycle authority")
	}
	if !bytes.Equal(candidate.sources, original.sources) || !bytes.Equal(candidate.executionGrades, original.executionGrades) || candidate.assemblyDigest != original.assemblyDigest || !bytes.Equal(candidate.assemblyMetadata, original.assemblyMetadata) {
		t.Fatal("retained source provenance changed")
	}
	if got := sourceReleaseCall(t, state, sourceForkArgs(args, "native-callback-1", token, "source-release-plan")); got["status"] != "error" {
		t.Fatal("private fork challenge was usable as source handle")
	}
	sourceForkAnswers(t, args, plan, grades)
	result := sourceForkCall(t, state, args)
	if result["admitted"] != true || result["source_release_ticket"] != token || result["new_request_id"] != "native-callback-1" {
		t.Fatal("fork admission", result)
	}
	fresh := state.entries[token]
	if fresh == nil || fresh.binding != releaseBinding(sourceForkArgs(args, "native-callback-1", token, "")) || !fresh.expires.Equal(expiry) || fresh.admitted != "" || fresh.guardedAdmission != "" {
		t.Fatal("fork copied reusable dispatch permission")
	}
	if original.admitted != "parent-admission" || original.guardedAdmission != "parent-guard" || original.healthAttempt != "parent-attempt" || state.entries[parent] != original {
		t.Fatal("fork altered parent authority")
	}
	// Fresh callbacks can now accumulate actual configured-test context normally.
	refreshed := sourceForkArgs(args, "native-callback-1", token, "")
	if next, err := state.prepare(refreshed, map[string]any{"execution_test_grades": grades}); err != nil || next == "" {
		t.Fatal("delegate assembly still rejected", err)
	}
	// Provider receipt preparation cannot reuse the fork's identity check as send admission.
	if got := sourceReleaseCall(t, state, sourceForkArgs(args, "native-callback-1", token, "provider-receipt-plan")); got["status"] != "error" {
		t.Fatal("fork admission authorized provider dispatch")
	}
	// Finishing the independent child never consumes the parent's retained proof.
	sourceReleaseCall(t, state, sourceForkArgs(args, "native-callback-1", token, "source-release-finish"))
	if state.entries[parent] != original || state.bytes < originalBytes {
		t.Fatal("child finish consumed parent provenance")
	}
}
func TestSourceReleaseForkRefusesChangedOwnerSourceExpiryAndReplay(t *testing.T) {
	for _, scenario := range []string{"old-request", "principal", "caller", "empty-new", "same-new", "source-revoked", "private-revoked", "private-missing", "grade-changed", "wrong-check", "missing-answer", "expired", "parent-finished", "policy-changed", "replay", "send-guard"} {
		t.Run(scenario, func(t *testing.T) {
			state, args, parent, grades := sourceForkOwner(t)
			switch scenario {
			case "old-request":
				args["request_id"] = json.RawMessage(`"foreign-request"`)
			case "principal":
				args["principal"] = json.RawMessage(`"foreign-principal"`)
			case "caller":
				args["caller_subject"] = json.RawMessage(`"foreign-caller"`)
			case "empty-new":
				args["new_request_id"] = json.RawMessage(`""`)
			case "same-new":
				args["new_request_id"] = args["request_id"]
			case "send-guard":
				args["send_guard"] = json.RawMessage(`true`)
			}
			plan := sourceForkCall(t, state, args)
			if scenario == "old-request" || scenario == "principal" || scenario == "caller" || scenario == "empty-new" || scenario == "same-new" || scenario == "send-guard" {
				if plan["status"] != "error" || len(state.forks) != 0 {
					t.Fatal("invalid fork owner accepted", plan)
				}
				return
			}
			sourceForkAnswers(t, args, plan, grades)
			switch scenario {
			case "source-revoked":
				var answer map[string]any
				_ = json.Unmarshal(args["owner_response"], &answer)
				answer["eligible"] = false
				args["owner_response"], _ = json.Marshal(answer)
			case "private-revoked":
				var answer map[string]any
				_ = json.Unmarshal(args["local_response"], &answer)
				answer["eligible"] = false
				args["local_response"], _ = json.Marshal(answer)
			case "private-missing":
				delete(args, "local_response")
			case "grade-changed":
				var answer map[string]any
				_ = json.Unmarshal(args["execution_grade_response"], &answer)
				answer["items"] = []any{}
				args["execution_grade_response"], _ = json.Marshal(answer)
			case "wrong-check":
				var answer map[string]any
				_ = json.Unmarshal(args["owner_response"], &answer)
				answer["check_id"] = "old-check"
				args["owner_response"], _ = json.Marshal(answer)
			case "missing-answer":
				delete(args, "execution_grade_response")
			case "expired":
				state.entries[parent].expires = time.Now().Add(-time.Second)
				state.forks[plan["fork_ticket"].(string)].entry.expires = state.entries[parent].expires
			case "parent-finished":
				sourceReleaseCall(t, state, sourceForkArgs(args, args.stringOr("request_id", ""), parent, "source-release-finish"))
			case "policy-changed":
				state.entries[parent].assemblyParts = []sourceReleasePart{{SelectionPolicy: "obsolete", SelectionPolicyDigest: "old"}}
			}
			result := sourceForkCall(t, state, args)
			if scenario == "replay" {
				if result["admitted"] != true {
					t.Fatal("first fork", result)
				}
				if again := sourceForkCall(t, state, args); again["status"] != "error" {
					t.Fatal("fork result replay", again)
				}
				args["operation"] = json.RawMessage(`"source-release-fork-plan"`)
				if again := sourceForkCall(t, state, args); again["status"] != "error" {
					t.Fatal("new request binding reused", again)
				}
			} else if result["status"] != "error" || result["admitted"] == true {
				t.Fatal("stale or unauthenticated fork admitted", result)
			}
		})
	}
}
func TestSourceReleaseForkRuntimeRemainsHostOnly(t *testing.T) {
	handler := NewHandler(nil)
	for _, operation := range []string{"source-release-fork-plan", "source-release-fork-result"} {
		frame, _ := ring.EncodeCommand("runtime", json.RawMessage(`{"operation":"`+operation+`"}`))
		if _, status := handler(ring.ModuleInvocation{StageID: StageCommand, PrincipalRef: 200}, frame); status != ring.ModuleStatusInvalidRequest {
			t.Fatal("model principal forked authority", status)
		}
	}
}
