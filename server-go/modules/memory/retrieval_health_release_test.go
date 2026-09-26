package memory

import (
	"encoding/json"
	"fmt"
	"github.com/JBailes/aimee/server-go/bus"
	"strings"
	"testing"
	"time"
)

func TestHealthReleaseLabelRequiresBoundGuardedOwnerAdmission(t *testing.T) {
	t.Setenv("AIMEE_MEMORY_HEALTH_ENABLED", "1")
	now := time.Now().UTC()
	digest := strings.Repeat("a", 64)
	b := providerReceiptBinding{SourceCheckID: strings.Repeat("b", 32), SourcesDigest: strings.Repeat("c", 64), SourceCoverage: "retained_versioned_inputs"}
	entry := &sourceReleaseEntry{guardedAdmission: b.SourceCheckID, guardedAdmissionAt: now.Add(-time.Millisecond)}
	metadata := receiptMetadataWithRelease(json.RawMessage(`{}`), entry, b, digest, now)
	var fields struct {
		Release *healthReleaseCheck `json:"health_release"`
	}
	json.Unmarshal(metadata, &fields)
	if !healthReleaseValid(fields.Release, b, digest, now) {
		t.Fatal(string(metadata))
	}
	for _, change := range []func(*healthReleaseCheck){func(p *healthReleaseCheck) { p.Check = "foreign" }, func(p *healthReleaseCheck) { p.Sources = strings.Repeat("d", 64) }, func(p *healthReleaseCheck) { p.Binding = strings.Repeat("e", 64) }, func(p *healthReleaseCheck) { p.ObservedAt = now.Add(-6 * time.Second).Format(time.RFC3339Nano) }, func(p *healthReleaseCheck) { p.ObservedAt = now.Add(time.Second).Format(time.RFC3339Nano) }, func(p *healthReleaseCheck) { p.Verifier = "unguarded" }} {
		modified := *fields.Release
		change(&modified)
		if healthReleaseValid(&modified, b, digest, now) {
			t.Fatal("invalid release proof accepted", modified)
		}
	}
	entry.guardedAdmission = ""
	if got := receiptMetadataWithRelease(json.RawMessage(`{}`), entry, b, digest, now); string(got) != "{}" {
		t.Fatal("legacy admission certified", string(got))
	}
}
func TestHealthReleaseImporterKeepsSafetyAndSemanticLabelsSeparate(t *testing.T) {
	state := &sourceReleaseState{}
	plan := sourceReleaseCall(t, state, receiptTestAdmission(t, state))
	var prepared providerReceiptEvent
	json.Unmarshal([]byte(plan["prepared_detail"].(string)), &prepared)
	at, _ := time.Parse(time.RFC3339Nano, prepared.At)
	proof := healthReleaseCheck{Binding: prepared.BindingDigest, Check: prepared.Binding.SourceCheckID, Sources: prepared.Binding.SourcesDigest, ObservedAt: at.Format(time.RFC3339Nano), Verifier: "source_owner_guard_v2"}
	raw, _ := json.Marshal(map[string]any{"health_release": proof})
	j, _ := newHealthJournal("owner", "operator", "private", "", time.Now())
	snapshot, err := healthSnapshotFromReceipt(inspectedHealthReceipt{Prepared: &prepared, Sequence: "1", Assembly: raw}, j, 1000000)
	if err != nil || len(snapshot.Invocation.Records) != 1 || snapshot.Invocation.Records[0].LifecycleViolation == nil || *snapshot.Invocation.Records[0].LifecycleViolation || snapshot.Invocation.Labels != nil {
		t.Fatal(snapshot, err)
	}
	if strings.Contains(strings.Join(snapshot.Invocation.MetadataGaps, ","), "release_labels") {
		t.Fatal("owner proof ignored")
	}
	proof.Check = "wrong"
	raw, _ = json.Marshal(map[string]any{"health_release": proof})
	snapshot, err = healthSnapshotFromReceipt(inspectedHealthReceipt{Prepared: &prepared, Sequence: "1", Assembly: raw}, j, 1000000)
	if err != nil || snapshot.Invocation.Records[0].LifecycleViolation != nil {
		t.Fatal("invalid proof certified", err)
	}
}

func TestHealthReleaseProducerRequiresActualGuardConfirmation(t *testing.T) {
	t.Setenv("AIMEE_MEMORY_HEALTH_ENABLED", "1")
	s := &sourceReleaseState{}
	args := receiptTestAdmission(t, s)
	for _, guarded := range []bool{true, false} {
		args["operation"] = json.RawMessage(`"source-release-plan"`)
		args["send_guard"], _ = json.Marshal(guarded)
		planned := sourceReleaseCall(t, s, args)
		check := planned["request"].(map[string]any)["revalidation"].(map[string]any)["check_id"].(string)
		args["operation"] = json.RawMessage(`"source-release-result"`)
		args["owner_response"], _ = json.Marshal(map[string]any{"status": "ok", "eligible": true, "check_id": check, "sources_digest": releaseDigest([]typedProjectionRef{releaseTestRef()}), "send_guard": "acquired", "lease_ms": 5000, "guard_schema_version": 2})
		if result := sourceReleaseCall(t, s, args); result["admitted"] != true {
			t.Fatal(result)
		}
		args["operation"] = json.RawMessage(`"provider-receipt-plan"`)
		receipt := sourceReleaseCall(t, s, args)
		if receipt["status"] != "ok" {
			t.Fatal(receipt)
		}
		events := receipt["assembly_events"].([]any)
		var event providerReceiptEvent
		json.Unmarshal([]byte(events[0].(map[string]any)["detail"].(string)), &event)
		var projection struct {
			Release *healthReleaseCheck `json:"health_release"`
		}
		json.Unmarshal(event.Projection, &projection)
		if (projection.Release != nil) != guarded {
			t.Fatal("guard proof leaked between admissions", string(event.Projection))
		}
	}
}

func TestHealthReleaseRealTransportOrdering(t *testing.T) {
	t.Setenv("AIMEE_MEMORY_HEALTH_ENABLED", "1")
	s := &sourceReleaseState{}
	args := receiptTestAdmission(t, s)
	receipt := sourceReleaseCall(t, s, args)
	var prepared providerReceiptEvent
	json.Unmarshal([]byte(receipt["prepared_detail"].(string)), &prepared)
	args["attempt_id"], _ = json.Marshal(prepared.AttemptID)
	args["operation"] = json.RawMessage(`"source-release-plan"`)
	args["send_guard"] = json.RawMessage(`true`)
	planned := sourceReleaseCall(t, s, args)
	check := planned["request"].(map[string]any)["revalidation"].(map[string]any)["check_id"].(string)
	args["operation"] = json.RawMessage(`"source-release-result"`)
	args["owner_response"], _ = json.Marshal(map[string]any{"status": "ok", "eligible": true, "check_id": check, "sources_digest": releaseDigest([]typedProjectionRef{releaseTestRef()}), "send_guard": "acquired", "lease_ms": 5000, "guard_schema_version": 2})
	sourceReleaseCall(t, s, args)
	args["operation"] = json.RawMessage(`"provider-receipt-started"`)
	result := sourceReleaseCall(t, s, args)
	var dispatched providerReceiptEvent
	json.Unmarshal([]byte(result["observation_detail"].(string)), &dispatched)
	if !healthDispatchReleaseValid(&dispatched, &prepared) || dispatched.HealthRelease.GuardCheck == prepared.Binding.SourceCheckID {
		t.Fatal(result)
	}
	j, _ := newHealthJournal("owner", "operator", "private", "", time.Now())
	snapshot, err := healthSnapshotFromReceipt(inspectedHealthReceipt{Prepared: &prepared, Dispatch: &dispatched, Sequence: "1"}, j, 1000000)
	if err != nil || snapshot.Invocation.ReleaseVerifier != "source_owner_guard_v2" || snapshot.Invocation.Records[0].LifecycleViolation == nil {
		t.Fatal(snapshot, err)
	}
	rows := []map[string]any{}
	for _, detail := range []string{receipt["prepared_detail"].(string), receipt["admitted_detail"].(string), result["observation_detail"].(string)} {
		var e providerReceiptEvent
		json.Unmarshal([]byte(detail), &e)
		rows = append(rows, map[string]any{"sequence": fmt.Sprint(len(rows) + 1), "action": "memory.provider." + e.Stage, "attempt_id": e.AttemptID, "detail": detail, "row_hash": strings.Repeat("f", 64)})
	}
	inspected, status := inspectProviderReceipts(sourceReleaseArgs(map[string]any{"dispatch_owner": strings.Repeat("a", 32), "receipt_request_id": "request", "chain_intact": true, "ledger_events": rows}))
	body, decodeErr := bus.DecodeCommandResult(inspected)
	var ledger struct {
		Receipts []inspectedHealthReceipt `json:"receipts"`
	}
	if status != 0 || decodeErr != nil || json.Unmarshal(body, &ledger) != nil || len(ledger.Receipts) != 1 || !healthDispatchReleaseValid(ledger.Receipts[0].Dispatch, ledger.Receipts[0].Prepared) {
		t.Fatal(string(body), status, decodeErr)
	}
	for _, mutate := range []func(*providerReceiptEvent){
		func(e *providerReceiptEvent) { e.AttemptID = "foreign" },
		func(e *providerReceiptEvent) { e.BindingDigest = strings.Repeat("e", 64) },
		func(e *providerReceiptEvent) { e.Stage = "dispatch_admitted" },
		func(e *providerReceiptEvent) { e.At = time.Now().Add(6 * time.Second).UTC().Format(time.RFC3339Nano) },
	} {
		copy := dispatched
		mutate(&copy)
		if healthDispatchReleaseValid(&copy, &prepared) {
			t.Fatal("foreign/stale dispatch accepted")
		}
	}
}
