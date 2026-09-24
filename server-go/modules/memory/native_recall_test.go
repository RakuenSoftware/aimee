package memory

import (
	"crypto/sha256"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"strings"
	"testing"
)

func nativeRecallFixture() recallBundle {
	return recallBundle{AlwaysOnRules: []recallRule{}, Identity: []RecallRecord{}, Preferences: []RecallRecord{}, ActiveContext: []RecallRecord{}, OpenCommitments: []RecallRecord{}, Reminders: []recallReminder{}, Directives: []recallDirective{}, Explain: []any{}, LimitTokens: 8192}
}
func TestNativeRecallCompleteRulesAndWholeRows(t *testing.T) {
	b := nativeRecallFixture()
	for i := 0; i < 20; i++ {
		b.AlwaysOnRules = append(b.AlwaysOnRules, recallRule{ID: int64(i + 1), Title: fmt.Sprintf("rule-%02d 界", i), Description: "preserve the complete required constraint", Polarity: "must", Weight: 5})
	}
	b.Identity = recallItems([]Record{{ID: math.MaxInt64, Key: "identity:fixture", Content: "complete 界🦊 identity"}})
	b.Reminders = []recallReminder{{MemoryID: math.MaxInt64, Text: "retained reminder", Key: "trigger"}, {MemoryID: math.MaxInt64 - 1, Text: strings.Repeat("optional", 100)}}
	full, n, err := projectNativeRecall(b, maxDataBody)
	if err != nil || n != 3 || full.Bytes != len(full.Text) || full.Digest != fmt.Sprintf("sha256:%x", sha256.Sum256([]byte(full.Text))) {
		t.Fatal(full, n, err)
	}
	for _, r := range b.AlwaysOnRules {
		if !strings.Contains(full.Text, r.Title) || !strings.Contains(full.Text, r.Description) {
			t.Fatal("required rule lost", r)
		}
	}
	exact, n, err := projectNativeRecall(b, full.Bytes)
	if err != nil || n != 3 || exact.Text != full.Text {
		t.Fatal("exact fit changed", exact, n, err)
	}
	trimmed, n, err := projectNativeRecall(b, full.Bytes-1)
	if err != nil || n != 2 || len(trimmed.Reminders) != 1 || trimmed.Reminders[0] != "9223372036854775807" || strings.Contains(trimmed.Text, b.Reminders[1].Text) {
		t.Fatal("partial row or imprecise/omitted reminder retained", trimmed, n, err)
	}
	mandatory := b
	mandatory.Identity = []RecallRecord{}
	mandatory.Reminders = []recallReminder{}
	required, _, err := projectNativeRecall(mandatory, maxDataBody)
	if err != nil {
		t.Fatal(err)
	}
	for _, limit := range []int{0, required.Bytes - 1} {
		_, _, err = projectNativeRecall(b, limit)
		var refusal *contextBudgetError
		if !errors.As(err, &refusal) || refusal.kind != "protected_context_overflow" {
			t.Fatal("required rules truncated", limit, err)
		}
	}
	kept, n, err := projectNativeRecall(b, required.Bytes)
	if err != nil || n != 0 || kept.Text != required.Text || len(kept.Reminders) != 0 {
		t.Fatal("mandatory exact fit", kept, n, err)
	}
}
func TestNativeRecallZeroAndEmptyProjection(t *testing.T) {
	b := nativeRecallFixture()
	b.Identity = recallItems([]Record{{ID: 1, Content: "optional"}})
	p, n, err := projectNativeRecall(b, 0)
	if err != nil || n != 0 || p.Text != "" || p.Bytes != 0 || len(p.Reminders) != 0 {
		t.Fatal(p, n, err)
	}
}
func TestNativeRecallEnvelopeRetainedIdentity(t *testing.T) {
	b := nativeRecallFixture()
	b.Identity = recallItems([]Record{{ID: math.MaxInt64, Content: "first"}})
	b.Reminders = []recallReminder{{MemoryID: math.MaxInt64, Text: "second"}, {MemoryID: 5, Text: strings.Repeat("omit", 100)}}
	first := b
	first.Reminders = first.Reminders[:1]
	cap, _, _ := projectNativeRecall(first, maxDataBody)
	raw, _ := json.Marshal(map[string]any{"status": "ok", "store": "composed", "recall": b})
	args := commandArgs{"native_context_bytes": json.RawMessage(fmt.Sprint(cap.Bytes))}
	projected, err := nativeRecallEnvelope(raw, args)
	if err != nil {
		t.Fatal(err)
	}
	var result struct {
		Recall     recallBundle           `json:"recall"`
		Projection nativeRecallProjection `json:"native_context"`
	}
	if json.Unmarshal(projected, &result) != nil || len(result.Recall.Reminders) != 1 || result.Recall.Identity[0].MemoryID != math.MaxInt64 || result.Projection.Reminders[0] != "9223372036854775807" {
		t.Fatal(string(projected))
	}
	source, _ := json.Marshal(b)
	if result.Projection.SourceDigest != fmt.Sprintf("sha256:%x", sha256.Sum256(source)) {
		t.Fatal("source commitment missing")
	}
	unchanged, err := nativeRecallEnvelope(raw, commandArgs{})
	if err != nil || string(unchanged) != string(raw) {
		t.Fatal("ordinary recall changed")
	}
	for _, status := range []string{"error", "degraded", "quarantined"} {
		failure := []byte(fmt.Sprintf(`{"status":%q,"kind":"protected_context_overflow","message":"owner diagnostic"}`, status))
		got, err := nativeRecallEnvelope(failure, args)
		if err != nil || string(got) != string(failure) {
			t.Fatal("refusal changed", string(got), err)
		}
	}
	for _, raw := range []string{`{}`, `{"status":"ok","recall":{}}`, `{"status":"ok","recall":null}`} {
		if _, err := nativeRecallEnvelope([]byte(raw), args); err == nil {
			t.Fatal("malformed source accepted", raw)
		}
	}
}
func TestNativeRecallAllocationValidation(t *testing.T) {
	for _, value := range []string{"null", "-1", "1.5", `"100"`, "1048577"} {
		if _, err := nativeRecallLimit(commandArgs{"native_context_bytes": json.RawMessage(value)}); err == nil {
			t.Fatal("invalid native allocation accepted", value)
		}
	}
}

func TestNativeRecallRetainedVersionSelection(t *testing.T) {
	b := nativeRecallFixture()
	private := Record{ID: 9007199254740993, Scope: Scope{Type: ScopeUser}, Content: "private retained", Version: &MemoryRecordVersion{SchemaVersion: 1, OwnerID: "00000000-0000-0000-0000-000000000001", RecordID: "9007199254740993", RecordRevision: "9007199254740995"}}
	shared := Record{ID: private.ID, Content: "shared omitted", Version: &MemoryRecordVersion{SchemaVersion: 1, OwnerID: "00000000-0000-0000-0000-000000000002", RecordID: private.Version.RecordID, RecordRevision: "2"}}
	b.Identity = recallItems([]Record{private})
	prefix, _, err := projectNativeRecall(b, maxDataBody)
	if err != nil {
		t.Fatal(err)
	}
	b.Preferences = recallItems([]Record{shared})
	limited, n, err := projectNativeRecall(b, prefix.Bytes)
	if err != nil || n != 1 || len(limited.Sources) != 1 || limited.Sources[0].Source.Kind != "user_memory_record" || limited.Sources[0].ID != private.Version.RecordID || limited.Sources[0].Source.Version != *private.Version || limited.SelectionDigest != releaseDigest(limited.Sources) {
		t.Fatalf("retained private source: %+v %d %v", limited, n, err)
	}
	full, _, err := projectNativeRecall(b, maxDataBody)
	if err != nil || len(full.Sources) != 2 || full.Sources[1].Source.Kind != "memory_record" || full.SelectionDigest == limited.SelectionDigest {
		t.Fatalf("mixed owners: %+v %v", full, err)
	}
	changed := *private.Version
	changed.RecordRevision = "9007199254740996"
	b.Identity[0].Version = &changed
	revised, _, err := projectNativeRecall(b, prefix.Bytes)
	if err != nil || revised.Text != limited.Text || revised.Digest != limited.Digest || revised.SelectionDigest == limited.SelectionDigest {
		t.Fatal("version change not independently committed", err)
	}
	b.Identity[0].Store = "unknown"
	if _, _, err := projectNativeRecall(b, maxDataBody); err == nil {
		t.Fatal("unknown owner placement accepted")
	}
	b.Identity[0].Version = nil
	unversioned, _, err := projectNativeRecall(b, prefix.Bytes)
	if err != nil || len(unversioned.Sources) != 0 || unversioned.SelectionDigest != releaseDigest([]typedProjectionRef{}) {
		t.Fatal("invented legacy source evidence", err)
	}
}

func TestNativeRuleMetadataStaysOutsidePrompt(t *testing.T) {
	b := nativeRecallFixture()
	source := &typedSourceVersion{Kind: "memory_rule", Version: MemoryRecordVersion{SchemaVersion: 1, OwnerID: "00000000-0000-0000-0000-000000000001", RecordID: "1", RecordRevision: "7"}, MemoryParentState: "observed"}
	b.AlwaysOnRules = []recallRule{{ID: 1, Title: "Required", Description: "Keep the complete constraint", Source: source}}
	p, _, err := projectNativeRecall(b, 4096)
	if err != nil || len(p.Sources) != 1 || p.Sources[0].Source != source || !strings.Contains(p.Text, "Keep the complete constraint") || strings.Contains(p.Text, "record_revision") || b.AlwaysOnRules[0].Source != source {
		t.Fatalf("rule projection lost content or separated evidence: %+v %v", p, err)
	}
}
