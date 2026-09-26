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
