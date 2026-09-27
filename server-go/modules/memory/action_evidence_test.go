package memory

import (
	"encoding/json"
	"strings"
	"testing"
	"time"
)

func TestActionEvidenceRequiresCurrentGuardAndExactReceipt(t *testing.T) {
	s := &sourceReleaseState{}
	args := receiptTestAdmission(t, s)
	plan := sourceReleaseCall(t, s, args)
	args["attempt_id"], _ = json.Marshal(plan["attempt_id"])
	args["operation"] = json.RawMessage(`"provider-receipt-started"`)
	sourceReleaseCall(t, s, args)
	args["operation"] = json.RawMessage(`"provider-receipt-observe"`)
	args["http_status"] = json.RawMessage(`200`)
	args["response_sha256"], _ = json.Marshal(strings.Repeat("b", 64))
	args["response_bytes"] = json.RawMessage(`"10"`)
	sourceReleaseCall(t, s, args)
	args["operation"] = json.RawMessage(`"action-evidence"`)
	if result := sourceReleaseCall(t, s, args); result["status"] == "ok" {
		t.Fatal("unguarded action evidence", result)
	}
	args["operation"] = json.RawMessage(`"source-release-plan"`)
	args["send_guard"] = json.RawMessage(`true`)
	guard := sourceReleaseCall(t, s, args)
	revalidation := guard["request"].(map[string]any)["revalidation"].(map[string]any)
	args["operation"] = json.RawMessage(`"source-release-result"`)
	args["owner_response"], _ = json.Marshal(map[string]any{"status": "ok", "eligible": true, "check_id": revalidation["check_id"], "sources_digest": releaseDigest([]typedProjectionRef{releaseTestRef()}), "send_guard": "acquired", "lease_ms": 5000, "guard_schema_version": 2})
	if result := sourceReleaseCall(t, s, args); result["admitted"] != true {
		t.Fatal(result)
	}
	args["operation"] = json.RawMessage(`"action-evidence"`)
	result := sourceReleaseCall(t, s, args)
	if result["status"] != "ok" || result["memory_required"] != true || result["context_receipt"] == "" || result["memory_guard"] == "" {
		t.Fatal(result)
	}
	entry := s.entries[args.stringOr("source_release_ticket", "")]
	original := entry.sources
	entry.sources = json.RawMessage(`[]`)
	if result := sourceReleaseCall(t, s, args); result["status"] == "ok" {
		t.Fatal("different retained sources accepted")
	}
	entry.sources = original
	entry.guardedAdmissionAt = time.Now().Add(-2 * time.Second)
	if result := sourceReleaseCall(t, s, args); result["status"] == "ok" {
		t.Fatal("expired freshness accepted")
	}
	entry.guardedAdmissionAt = time.Now()
	args["principal"] = json.RawMessage(`"foreign"`)
	if result := sourceReleaseCall(t, s, args); result["status"] == "ok" {
		t.Fatal("foreign evidence accepted")
	}
	args["principal"] = json.RawMessage(`"operator"`)
	if result := sourceReleaseCall(t, &sourceReleaseState{}, args); result["status"] == "ok" {
		t.Fatal("restart manufactured evidence")
	}
}
