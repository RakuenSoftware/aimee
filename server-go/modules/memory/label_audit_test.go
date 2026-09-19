package memory

import (
	"context"
	"encoding/json"
	"errors"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

type labelAuditStore struct {
	recordingDataStore
	retrievalDataStore
	rows  []Diagnostic
	calls int
	err   error
}

func (s *labelAuditStore) Diagnose(_ context.Context, scope Scope, query string, limit int) ([]Diagnostic, error) {
	s.calls++
	s.scope = scope
	return s.rows[:min(limit, len(s.rows))], s.err
}

func TestLabelAuditUsesOwnerOrderWithoutGoldInjection(t *testing.T) {
	s := &labelAuditStore{rows: []Diagnostic{{Memory: Record{ID: 9007199254740993}, Parts: DiagnosticParts{Lexical: .1}}, {Memory: Record{ID: 2}, Parts: DiagnosticParts{Lexical: 100}}}}
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, s)))
	args := map[string]any{"labels_tsv": "# comment\nquery\t9007199254740993\nmissing\t999\n", "limit": 1, "candidate_limit": 2, "format": "json"}
	raw, _ := json.Marshal(args)
	reply := runPublicCommand(t, client, "audit", string(raw))
	var report struct {
		MRR     float64        `json:"mrr"`
		Recall  float64        `json:"recall_at_k"`
		Cases   int            `json:"case_count"`
		Buckets map[string]int `json:"buckets"`
	}
	if err := json.Unmarshal([]byte(reply["output"].(string)), &report); err != nil {
		t.Fatal(err)
	}
	if report.MRR != .5 || report.Recall != .5 || report.Cases != 2 || report.Buckets["missing"] != 1 || len(report.Buckets) != 1 || s.calls != 2 {
		t.Fatal(reply, s.calls)
	}
	s.err = errors.New("storage unavailable")
	frame, _ := bus.EncodeCommand("audit", raw)
	if _, status := NewHandler(nil, WithDataStore(PlacementKB, s))(bus.ModuleInvocation{StageID: StageCommand}, frame); status == bus.ModuleStatusOK {
		t.Fatal("failed read published successful audit")
	}
}
func TestLabelCalibrationFreezesCandidatesAndCannotDeploy(t *testing.T) {
	s := &labelAuditStore{rows: []Diagnostic{{Memory: Record{ID: 1}, Parts: DiagnosticParts{Lexical: 1.2}}, {Memory: Record{ID: 2}, Parts: DiagnosticParts{Semantic: 1}}}}
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, s)))
	args := map[string]any{"labels_tsv": "query\t2", "limit": 1, "candidate_limit": 2, "format": "json"}
	raw, _ := json.Marshal(args)
	reply := runPublicCommand(t, client, "calibrate", string(raw))
	var report map[string]any
	if err := json.Unmarshal([]byte(reply["artifact"].(string)), &report); err != nil {
		t.Fatal(err)
	}
	if report["baseline_mrr"] != .5 || report["mrr"] != float64(1) || report["deployable"] != false || report["applied_config"] != false || s.calls != 1 {
		t.Fatal(reply, report, s.calls)
	}
	args["apply_config"] = true
	raw, _ = json.Marshal(args)
	reply = runPublicCommand(t, client, "calibrate", string(raw))
	if reply["kind"] != "unsupported_mode" || s.calls != 1 {
		t.Fatal("calibration deployed or read before refusal", reply, s.calls)
	}
}
func TestAuditLabelValidationAndCancellation(t *testing.T) {
	for _, raw := range []string{"", "# only comment", "query\t01", "query\t0", "query\t9223372036854775808", "query\t1\textra", "query\t1\nbad", strings.Repeat("query\t1\n", 4097)} {
		if _, err := parseAuditLabels(raw); err == nil {
			t.Fatal("bad labels accepted", raw[:min(80, len(raw))])
		}
	}
	rows, err := parseAuditLabels("  文 query\t9007199254740993\n")
	if err != nil || rows[0].expected != 9007199254740993 || rows[0].query != "文 query" {
		t.Fatal(rows, err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, _, _, err := calibrateAudit(ctx, rows, 1, 1); err == nil {
		t.Fatal("cancelled calibration continued")
	}
}
