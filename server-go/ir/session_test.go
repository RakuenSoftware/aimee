package ir

import (
	"encoding/json"
	"strings"
	"testing"
)

func TestHistoryImportReportIsMetadataAndExact(t *testing.T) {
	r := HistoryImportReport{Version: 1, SourceFormat: "canonical", Outcome: "exact", SourceSHA256: strings.Repeat("a", 64), SourceSession: "source", SourceAgent: "agent", SourceProvider: "anthropic", SourceMessages: 1, ImportedMessages: 1, ToolStructure: "preserved"}
	valid := func(h HistoryEvent) bool { body, _ := json.Marshal(h); return ValidHistoryEvent(body) }
	if !ValidHistoryImportReport(&r) || !valid(HistoryEvent{Scope: "primary", Phase: "import_report", ImportReport: &r}) {
		t.Fatal("valid typed metadata refused")
	}
	if valid(HistoryEvent{Scope: "primary", Phase: "commit", Count: 1, ImportReport: &r}) {
		t.Fatal("report is not a checkpoint/message")
	}
	text := "screen with no attributable tool structure"
	if !ValidHistoryEvent([]byte(`{"scope":"legacy-archive","phase":"archive","index":0,"archive_text":"screen"}`)) || valid(HistoryEvent{Scope: "primary", Phase: "archive", ArchiveText: &text}) {
		t.Fatal("archive must stay outside model scope")
	}
	for _, change := range []func(*HistoryImportReport){func(r *HistoryImportReport) { r.SourceSHA256 = "not-a-digest" }, func(r *HistoryImportReport) { r.RemovedThinkingBindings = 1 }, func(r *HistoryImportReport) { r.ToolStructure = "unrecoverable" }, func(r *HistoryImportReport) { r.ArchiveChunks = 1 }} {
		bad := r
		change(&bad)
		if ValidHistoryImportReport(&bad) {
			t.Fatal("invalid exact fidelity accepted", bad)
		}
	}
}

func TestHistoryMessageFragmentStrictWire(t *testing.T) {
	good := `{"scope":"primary","phase":"message_fragment","fragment":{"message_index":0,"part_index":0,"part_count":2,"message_sha256":"` + strings.Repeat("a", 64) + `","text":"canonical JSON prefix"}}`
	if !ValidHistoryEvent([]byte(good)) {
		t.Fatal("valid fragment refused")
	}
	for _, bad := range []string{strings.Replace(good, `"part_index":0`, `"part_index":null`, 1), strings.Replace(good, `"part_count":2`, `"part_count":0`, 1), strings.Replace(good, `"message_index":0,`, "", 1), strings.Replace(good, `"part_index":0`, `"part_index":2`, 1), strings.Replace(good, `"text":"canonical JSON prefix"`, `"text":""`, 1)} {
		if ValidHistoryEvent([]byte(bad)) {
			t.Fatal("invalid fragment admitted", bad)
		}
	}
}
