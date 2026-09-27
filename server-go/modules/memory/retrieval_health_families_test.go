package memory

import (
	"encoding/json"
	"strings"
	"testing"
)

func TestHealthFamilyUsesCanonicalExactVersionEvidence(t *testing.T) {
	source := &typedSourceVersion{Kind: "user_memory_record", Version: MemoryRecordVersion{SchemaVersion: 1, OwnerID: "owner", RecordID: "7", RecordRevision: "9"}}
	family := "sha256:" + strings.Repeat("a", 64)
	makeEvidence := func(owner, record, revision, state string, unknown any, families []string) json.RawMessage {
		raw, _ := json.Marshal(map[string]any{"status": "ok", "owner_id": owner, "record_id": record, "lineage_state": state, "unknown_origin_count": unknown, "origin_families": families, "evidence": []map[string]string{{"record_id": record, "record_revision": revision}}})
		return raw
	}
	raw := makeEvidence("owner", "7", "9", "complete", 0, []string{family})
	if healthFamilyFromEvidence(raw, source) != family {
		t.Fatal("canonical family lost")
	}
	for _, raw := range []json.RawMessage{
		makeEvidence("foreign", "7", "9", "complete", 0, []string{family}),
		makeEvidence("owner", "8", "9", "complete", 0, []string{family}),
		makeEvidence("owner", "7", "10", "complete", 0, []string{family}),
		makeEvidence("owner", "7", "9", "partial", nil, []string{family}),
		makeEvidence("owner", "7", "9", "complete", 1, []string{family}),
		makeEvidence("owner", "7", "9", "complete", 0, []string{family, "sha256:" + strings.Repeat("b", 64)}),
		makeEvidence("owner", "7", "9", "complete", 0, []string{"invented-parent"}),
	} {
		if healthFamilyFromEvidence(raw, source) != "" {
			t.Fatal("unestablished family attributed")
		}
	}
	source.Kind = "memory_record"
	if healthFamilyFromEvidence(raw, source) != "" {
		t.Fatal("shared family attributed by private owner")
	}
}

func TestHealthSharedAndMultipleFamiliesRequireExactCanonicalSnapshot(t *testing.T) {
	source := &typedSourceVersion{Kind: "memory_record", Version: MemoryRecordVersion{SchemaVersion: 1, OwnerID: "owner", RecordID: "7", RecordRevision: "9"}}
	a, b := "sha256:"+strings.Repeat("a", 64), "sha256:"+strings.Repeat("b", 64)
	payload := map[string]any{"status": "ok", "owner_id": "owner", "record_id": "7", "lineage_state": "complete", "unknown_origin_count": 0, "origin_families": []string{b, a, a}, "evidence": []map[string]string{{"record_id": "7", "record_revision": "9"}}}
	raw, _ := json.Marshal(payload)
	families := healthFamiliesFromEvidence(raw, source)
	if len(families) != 2 || families[0] != a || families[1] != b {
		t.Fatal(families)
	}
	record := healthRecord{RecordID: healthSourceIdentity(source), VersionID: "9", Kind: "fact"}
	metadata, _ := json.Marshal([]healthRecord{{RecordID: record.RecordID, VersionID: "9", Families: families}})
	result := mergeNativeHealthFamilies([]healthRecord{record}, metadata)
	if len(result[0].Families) != 2 || result[0].Family != "" {
		t.Fatal("multiple origins collapsed", result)
	}
	record.VersionID = "10"
	if result := mergeNativeHealthFamilies([]healthRecord{record}, metadata); len(result[0].Families) != 0 {
		t.Fatal("stale family metadata reused")
	}
	payload["unknown_origin_count"] = 1
	raw, _ = json.Marshal(payload)
	if healthFamiliesFromEvidence(raw, source) != nil {
		t.Fatal("partial ancestry certified")
	}
	if validHealthFamilies([]string{a, "copied-parent"}) != nil {
		t.Fatal("parent mistaken for canonical family")
	}
}
