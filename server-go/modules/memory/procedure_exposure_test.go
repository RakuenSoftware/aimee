package memory

import (
	"encoding/json"
	"github.com/JBailes/aimee/server-go/bus"
	"strings"
	"testing"
)

func TestProcedureExposureUsesRetainedVersionAndAcknowledgement(t *testing.T) {
	ref := releaseTestRef()
	ref.Channel = "approved_procedures"
	ref.Source.Kind = "learning_procedure"
	refs, _ := json.Marshal([]typedProjectionRef{ref, releaseTestRef()})
	receipt := &providerReceiptEvent{AttemptID: "attempt", BindingDigest: "receipt", Binding: &providerReceiptBinding{RequestID: "request", Sources: refs}}
	selected := procedureExposures(receipt, false)
	delivered := procedureExposures(receipt, true)
	if len(selected) != 1 || len(delivered) != 1 || selected[0]["state"] != "retrieved" || delivered[0]["state"] != "delivered" {
		t.Fatalf("incorrect exposures: %v %v", selected, delivered)
	}
	v := delivered[0]["procedure"].(map[string]string)
	if v["procedure_id"] != ref.ID || v["revision"] != "2" {
		t.Fatal("lost exact version", v)
	}
	refs, _ = json.Marshal([]typedProjectionRef{{Channel: "approved_procedures", ID: ref.ID}})
	receipt.Binding.Sources = refs
	if len(procedureExposures(receipt, true)) != 0 {
		t.Fatal("unversioned procedure got application evidence")
	}
	receipt.Binding.Sources = json.RawMessage(`[]`)
	if len(procedureExposures(receipt, true)) != 0 {
		t.Fatal("omitted procedure acquired delivery")
	}
}

func TestLegacyExposureRetainsExactIDsWithoutClaimingDelivery(t *testing.T) {
	args := sourceReleaseArgs(map[string]any{"turn_id": "turn", "surfaced_ids": []any{"9007199254743001", 1, 0, -1}})
	raw, status := legacyExposurePlan(args)
	body, err := bus.DecodeCommandResult(raw)
	var result struct {
		Status string   `json:"status"`
		IDs    []string `json:"surfaced_ids"`
		State  string   `json:"exposure_state"`
	}
	if status != bus.ModuleStatusOK || err != nil || json.Unmarshal(body, &result) != nil || len(result.IDs) != 2 || result.IDs[0] != "9007199254743001" || result.State != "retrieved_unverified" {
		t.Fatal(string(body), err)
	}
	args["surfaced_ids"] = json.RawMessage(`[1.5]`)
	raw, _ = legacyExposurePlan(args)
	body, _ = bus.DecodeCommandResult(raw)
	if json.Unmarshal(body, &result) != nil || result.Status != "error" {
		t.Fatal("fractional identity accepted")
	}
}

func TestExperienceContextIsBoundedWithoutInventingCounts(t *testing.T) {
	refs := []string{}
	for i := 0; i < 100; i++ {
		refs = append(refs, strings.Repeat("x", 100))
	}
	raw, _ := json.Marshal(map[string]any{"revision": "12", "state": "observed", "cohorts": []any{map[string]any{"verified_success": 100, "outcome_unknown": 25, "event_refs": refs}}})
	body, revision, err := compactProcedureExperience(string(raw))
	if err != nil || revision != "12" || len(body) > 2048 || !strings.Contains(string(body), `"verified_success":100`) || !strings.Contains(string(body), `"context_coverage":"partial"`) {
		t.Fatal(string(body), err)
	}
	raw, _ = json.Marshal(map[string]any{"revision": "12:erased", "state": "invalidated_by_erasure", "cohorts": []any{map[string]any{"environment": strings.Repeat("x", 4096)}}})
	body, revision, err = compactProcedureExperience(string(raw))
	if err != nil || revision != "12:erased" || len(body) > 2048 || !strings.Contains(string(body), `"state":"omitted_budget"`) {
		t.Fatal(string(body), err)
	}
}
