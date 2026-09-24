package memory

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestLearningPublicValidation(t *testing.T) {
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, nil)))
	for _, args := range []string{`{}`, `{"dirs":null}`, `{"dirs":"folder"}`} {
		if r := runPublicCommand(t, client, "scan_conversations", args); r["kind"] != "invalid_argument" {
			t.Fatal(r)
		}
	}
	for _, verb := range []string{"anti_pattern_extract_from_feedback", "anti_pattern_extract_from_failures", "anti_pattern_escalate", "memory_learn_style", "scan_conversations"} {
		if r := runPublicCommand(t, client, verb, `{"dirs":[]}`); r["kind"] != "unavailable" {
			t.Fatal(verb, r)
		}
	}
}

// Run with the packaged schema's audit triggers, RLS and restricted store role.
// This replaces the native decision-log extraction regression and covers the
// public command path used by the session maintenance callers.
func exerciseMaintenanceReplay(t *testing.T, ctx context.Context, tx pgx.Tx, handler bus.ModuleHandler) {
	t.Helper()
	client := clientForHandler(t, handler)
	run := func(verb, args string) int {
		t.Helper()
		r := runPublicCommand(t, client, verb, args)
		count, ok := r["count"].(float64)
		if r["status"] != "ok" || !ok || count < 0 {
			t.Fatal(verb, r)
		}
		return int(count)
	}
	_, err := tx.Exec(ctx, `INSERT INTO decision_log(options,chosen,rationale,outcome,created_at)
VALUES('safe,unsafe','runtime-dangerous-command','destructive shortcut','failure',pg_now_text());
INSERT INTO rules(polarity,title,description,created_at,updated_at) VALUES
('negative','runtime-avoid-shortcut','unsafe shortcut',pg_now_text(),pg_now_text()),
('positive','runtime-style-a','concise responses',pg_now_text(),pg_now_text()),
('positive','runtime-style-b','brief responses',pg_now_text(),pg_now_text());`)
	if err != nil {
		t.Fatal(err)
	}
	for _, verb := range []string{"anti_pattern_extract_from_feedback", "anti_pattern_extract_from_failures"} {
		if run(verb, `{}`) < 1 || run(verb, `{}`) != 0 {
			t.Fatal("extraction must deduplicate", verb)
		}
	}
	var count int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM anti_patterns WHERE pattern='runtime-dangerous-command' AND description='destructive shortcut' AND source='failure'`).Scan(&count); err != nil || count != 1 {
		t.Fatal(count, err)
	}
	if _, err := tx.Exec(ctx, `UPDATE anti_patterns SET hit_count=5 WHERE pattern='runtime-dangerous-command'`); err != nil {
		t.Fatal(err)
	}
	if run("anti_pattern_escalate", `{"hit_threshold":6}`) != 0 || run("anti_pattern_escalate", `{}`) != 1 || run("anti_pattern_escalate", `{}`) != 0 {
		t.Fatal("escalation threshold or deduplication failed")
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM rules WHERE title='runtime-dangerous-command' AND directive_type='soft'`).Scan(&count); err != nil || count != 1 {
		t.Fatal("automatic observations were promoted to protected rules", count, err)
	}
	if run("memory_learn_style", `{}`) < 1 {
		t.Fatal("style learning produced no preference")
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memories WHERE key='style_verbosity' AND kind='preference' AND content='User explicitly prefers brief responses'`).Scan(&count); err != nil || count != 1 {
		t.Fatal(count, err)
	}
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "conversation.jsonl"), []byte("{\"session_id\":\"runtime-scan\",\"content\":\"a conversation observation\"}\ninvalid JSON\n"), 0600); err != nil {
		t.Fatal(err)
	}
	args, _ := json.Marshal(map[string]any{"dirs": []any{false, dir}})
	if run("scan_conversations", string(args)) != 1 || run("scan_conversations", string(args)) != 1 {
		t.Fatal("scan count changed")
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memories WHERE source_session='runtime-scan' AND content='a conversation observation' AND artifact_type='conversation'`).Scan(&count); err != nil || count != 1 {
		t.Fatal("scan must reuse the source row", count, err)
	}
	if run("scan_conversations", `{"dirs":[]}`) != 0 {
		t.Fatal("empty scan")
	}
	// A filesystem failure must not be acknowledged as status=ok/count=-1.
	bad, _ := json.Marshal(map[string]any{"dirs": []string{filepath.Join(dir, "missing")}})
	if r := runPublicCommand(t, client, "scan_conversations", string(bad)); r["kind"] != "unavailable" {
		t.Fatal(r)
	}
}
