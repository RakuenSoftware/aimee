package memory

import (
	"encoding/json"
	"strings"
	"testing"
	"time"
)

func TestHealthPositionsUseFinalProjectionOrder(t *testing.T) {
	a, b := releaseTestRef(), releaseTestRef()
	b.Source.Version.RecordID = "2"
	b.ID = "2"
	fence := releaseTestRef()
	fence.Source.Kind = "memory_collection"
	assembly := map[string]any{"typed_projection": map[string]any{
		"projection_digest": "sha256:" + strings.Repeat("a", 64),
		"retained_items":    []typedProjectionRef{fence, b, {ID: "unversioned"}, a, b},
	}}
	// Source-fence order differs from render order and de-duplicates b.
	records := mergeHealthSelectionMetadata(nil, assembly, []typedProjectionRef{a, b, fence})
	if len(records) != 2 || len(records[0].Positions) != 1 || records[0].Positions[0].Rank != 3 ||
		len(records[1].Positions) != 2 || records[1].Positions[0].Rank != 1 || records[1].Positions[1].Rank != 4 {
		t.Fatal(records)
	}
	for _, record := range records {
		if record.Kind != "unknown" || record.LowTrust != nil {
			t.Fatal("position invented kind or trust", record)
		}
	}
	// Omitted and replaced source versions cannot inherit old ranks.
	prior, _ := json.Marshal(map[string]any{"health_records": records})
	a.Source.Version.RecordRevision = "99"
	if got := mergeHealthSelectionMetadata(prior, map[string]any{}, []typedProjectionRef{a}); len(got) != 0 {
		t.Fatal("rank transferred to another revision", got)
	}
}

func TestHealthNativePositionsIncludeUnversionedRows(t *testing.T) {
	t.Setenv("AIMEE_MEMORY_HEALTH_ENABLED", "1")
	b := nativeRecallFixture()
	b.AlwaysOnRules = []recallRule{{ID: 1, Title: "unversioned rule"}}
	version := &MemoryRecordVersion{SchemaVersion: 1, OwnerID: "00000000-0000-4000-8000-000000000001", RecordID: "1", RecordRevision: "1"}
	known := Record{ID: 1, Kind: "fact", Content: "known source", Version: version}
	b.Identity = recallItems([]Record{{ID: 2, Content: "unversioned row"}, known})
	b.ActiveContext = recallItems([]Record{known})
	p, _, err := projectNativeRecall(b, 4096)
	if err != nil {
		t.Fatal(err)
	}
	records := nativeHealthPositions(nativeHealthRecords(b, p.Sources), p)
	if len(records) != 1 || len(records[0].Positions) != 2 || records[0].Positions[0].Rank != 3 || records[0].Positions[1].Rank != 4 {
		t.Fatal(records)
	}
	t.Setenv("AIMEE_MEMORY_HEALTH_ENABLED", "0")
	off, _, err := projectNativeRecall(b, 4096)
	if err != nil || off.Text != p.Text || off.Digest != p.Digest || off.SelectionDigest != p.SelectionDigest || off.healthPositions != nil {
		t.Fatal("optional position capture changed serving", err)
	}
	t.Setenv("AIMEE_MEMORY_HEALTH_ENABLED", "1")
	b.AlwaysOnRules = make([]recallRule, maxReleaseSources)
	p, _, err = projectNativeRecall(b, maxDataBody)
	if err != nil || p.healthPositions != nil {
		t.Fatal("large projection did not drop bounded optional positions", err)
	}
}

func TestHealthPositionRefreshPreservesOtherProjections(t *testing.T) {
	ref := releaseTestRef()
	typed := healthPosition{Projection: "typed_projection", Digest: strings.Repeat("a", 64), Rank: 2}
	oldNative := healthPosition{Projection: "native_projection", Digest: strings.Repeat("b", 64), Rank: 1}
	newNative := healthPosition{Projection: "native_projection", Digest: strings.Repeat("c", 64), Rank: 4}
	record := healthRecord{RecordID: healthSourceIdentity(ref.Source), VersionID: ref.Source.Version.RecordRevision, Kind: "fact", Positions: []healthPosition{typed, oldNative}}
	prior, _ := json.Marshal(map[string]any{"health_records": []healthRecord{record}})
	record.Positions = []healthPosition{newNative}
	assembly := map[string]any{"native_projection": map[string]any{}, "health_records": []healthRecord{record}}
	got := mergeHealthSelectionMetadata(prior, assembly, []typedProjectionRef{ref})
	if len(got) != 1 || len(got[0].Positions) != 2 || got[0].Positions[0] != typed || got[0].Positions[1] != newNative {
		t.Fatal(got)
	}
	assembly["append_native_sources"] = true
	got = mergeHealthSelectionMetadata(prior, assembly, []typedProjectionRef{ref})
	if len(got) != 1 || len(got[0].Positions) != 3 {
		t.Fatal("appended projection lost earlier retained position", got)
	}
}

func TestHealthPositionImporterKeepsLegacyAndInvalidGaps(t *testing.T) {
	state := &sourceReleaseState{}
	plan := sourceReleaseCall(t, state, receiptTestAdmission(t, state))
	var prepared providerReceiptEvent
	json.Unmarshal([]byte(plan["prepared_detail"].(string)), &prepared)
	var refs []typedProjectionRef
	json.Unmarshal(prepared.Binding.Sources, &refs)
	ref := refs[0]
	position := healthPosition{Projection: "typed_projection", Digest: strings.Repeat("a", 64), Rank: 1}
	record := healthRecord{RecordID: healthSourceIdentity(ref.Source), VersionID: ref.Source.Version.RecordRevision, Kind: "unknown", Positions: []healthPosition{position}}
	journal, _ := newHealthJournal("owner", "operator", "private", "", time.Now())
	for _, valid := range []bool{true, false} {
		if !valid {
			record.Positions[0].Rank = 0
		}
		raw, _ := json.Marshal(map[string]any{"health_records": []healthRecord{record}})
		snapshot, err := healthSnapshotFromReceipt(inspectedHealthReceipt{Prepared: &prepared, Sequence: "1", Assembly: raw}, journal, 1000000)
		if err != nil {
			t.Fatal(err)
		}
		gap := strings.Contains(strings.Join(snapshot.Invocation.MetadataGaps, ","), "final_rank")
		if gap == valid {
			t.Fatal("incorrect rank coverage", valid, snapshot.Invocation.MetadataGaps)
		}
	}
}
