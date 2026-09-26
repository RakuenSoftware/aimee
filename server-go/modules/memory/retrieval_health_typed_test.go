package memory

import (
	"encoding/json"
	"math"
	"strings"
	"testing"
	"time"
)

func healthTypedFixture(t *testing.T) *typedContextResult {
	t.Helper()
	r := newTypedContext(DataRequest{TypedContext: typedTestOptions(t, `{}`)})
	hit := assertionHit{ID: 9007199254743001, StableID: "9007199254743001", Version: 2, Kind: "world_fact", Lifecycle: "persistent", ConfidenceClass: "A", ValidFrom: "2026-01-01T00:00:00Z", Subject: "private subject", Object: "private object", Rendered: "private assertion", ownerID: "00000000-0000-4000-8000-000000000001", memoryParentsObserved: true}
	total := 1.0/61 + 1.0/63
	hit.Retrieval = []assertionTrace{{Channel: "lexical", Rank: 1, Raw: 3, Fused: total}, {Channel: "vector", Rank: 3, Raw: .8, Fused: total}}
	r.add("current_assertions", typedItem{id: hit.StableID, source: hit.sourceVersion(), value: hit, text: hit.Rendered})
	if err := r.finish(); err != nil {
		t.Fatal(err)
	}
	return r
}

func TestHealthTypedFinalSelectionCarriesActualArms(t *testing.T) {
	r := healthTypedFixture(t)
	raw, _ := json.Marshal(r)
	decoded, err := decodeTypedProjection(string(raw))
	if err != nil {
		t.Fatal(err)
	}
	records := typedHealthRecords(decoded)
	if len(records) != 1 {
		t.Fatal(records)
	}
	record := records[0]
	if record.Kind != "world_fact" || record.State != "persistent" || record.ConfidenceClass != "A" || record.ValidFrom != "2026-01-01T00:00:00Z" || len(record.Arms) != 2 || record.Arms[0].Contribution != 1.0/61 || record.Arms[1].Contribution != 1.0/63 {
		t.Fatal(record)
	}
	if record.LowTrust != nil || record.LifecycleViolation != nil || record.Family != "" {
		t.Fatal("inferred unobserved labels", record)
	}
	private, _ := json.Marshal(records)
	if strings.Contains(string(private), "private") {
		t.Fatal("content escaped telemetry", string(private))
	}
	decoded.Channels["current_assertions"].selected[0].source.Version.RecordRevision = "3"
	if got := typedHealthRecords(decoded); len(got) != 0 {
		t.Fatal("metadata accepted different source version", got)
	}
	decoded.Channels["current_assertions"].selected = nil
	if got := typedHealthRecords(decoded); len(got) != 0 {
		t.Fatal("omitted item retained metadata", got)
	}
}

func TestHealthTypedIngressPreservesServingCommitment(t *testing.T) {
	r := healthTypedFixture(t)
	raw, _ := json.Marshal(r)
	budget := 16000
	request := ingressAssemblyRequest{TypedContextJSON: string(raw), ContextLimits: &ContextLimits{SchemaVersion: 1, MaxContextBytes: &budget}}
	t.Setenv("AIMEE_MEMORY_HEALTH_ENABLED", "")
	off, err := ingressAssemble(request)
	if err != nil {
		t.Fatal(err)
	}
	t.Setenv("AIMEE_MEMORY_HEALTH_ENABLED", "1")
	on, err := ingressAssemble(request)
	if err != nil {
		t.Fatal(err)
	}
	records, ok := on["health_records"].([]healthRecord)
	if !ok || len(records) != 1 || off["health_records"] != nil {
		t.Fatal("missing or nonoptional metadata", records)
	}
	if off["envelope"] != on["envelope"] || healthIndependentAssemblyDigest(off) != healthIndependentAssemblyDigest(on) {
		t.Fatal("telemetry changed serving commitment")
	}
	budget = 384
	dropped, err := ingressAssemble(request)
	if err != nil {
		t.Fatal(err)
	}
	droppedRecords, _ := dropped["health_records"].([]healthRecord)
	if len(droppedRecords) > 0 {
		t.Fatal("budget-dropped metadata retained")
	}
}

func TestHealthArmsRejectFabricatedContributions(t *testing.T) {
	good := []healthArm{{Name: "lexical", Rank: 1, RawScore: 2, Contribution: 1.0 / 61, FusedScore: 1.0 / 61, Policy: "assertion_rrf_k60_v1"}}
	if len(validatedHealthArms(good)) != 1 {
		t.Fatal("valid contribution lost")
	}
	for _, mutate := range []func(*healthArm){func(a *healthArm) { a.Contribution = .9 }, func(a *healthArm) { a.FusedScore = .9 }, func(a *healthArm) { a.RawScore = math.Inf(1) }, func(a *healthArm) { a.Rank = 0 }, func(a *healthArm) { a.Name = "invented" }, func(a *healthArm) { a.Policy = "future" }} {
		changed := append([]healthArm(nil), good...)
		mutate(&changed[0])
		if validatedHealthArms(changed) != nil {
			t.Fatal("invalid arms admitted", changed)
		}
	}
	if validatedHealthArms(append(good, good[0])) != nil {
		t.Fatal("duplicate arm admitted")
	}
}

func TestHealthTypedReceiptImportsArmsWithoutSafetyLabels(t *testing.T) {
	state := &sourceReleaseState{}
	args := receiptTestAdmission(t, state)
	plan := sourceReleaseCall(t, state, args)
	var prepared providerReceiptEvent
	json.Unmarshal([]byte(plan["prepared_detail"].(string)), &prepared)
	var refs []typedProjectionRef
	json.Unmarshal(prepared.Binding.Sources, &refs)
	records := typedHealthRecords(healthTypedFixture(t))
	// Bind fixture metadata to the receipt's actual selected version.
	records[0].RecordID = healthSourceIdentity(refs[0].Source)
	records[0].VersionID = refs[0].Source.Version.RecordRevision
	metadata, _ := json.Marshal(map[string]any{"health_records": records})
	j, _ := newHealthJournal("owner", "operator", "private", "", time.Now())
	got, err := healthSnapshotFromReceipt(inspectedHealthReceipt{Prepared: &prepared, Sequence: "1", Assembly: metadata}, j, 1000000)
	if err != nil || len(got.Invocation.Records) != 1 {
		t.Fatal(got, err)
	}
	record := got.Invocation.Records[0]
	if len(record.Arms) != 2 || record.State != "persistent" || record.LowTrust != nil || record.LifecycleViolation != nil {
		t.Fatal(record)
	}
	gaps := strings.Join(got.Invocation.MetadataGaps, ",")
	if strings.Contains(gaps, "arm_contributions") || !strings.Contains(gaps, "release_labels") || !strings.Contains(gaps, "trust") {
		t.Fatal(gaps)
	}
}
