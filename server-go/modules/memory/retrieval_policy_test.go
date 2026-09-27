package memory

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5"
)

func TestResultCountAvailability(t *testing.T) {
	for _, c := range []struct {
		count, limit int
		truncated    bool
	}{{0, 20, false}, {1, 20, false}, {20, 20, true}, {21, 20, true}} {
		got := resultCountAvailability(c.count, c.limit)
		if got.Count != c.count || got.Truncated != c.truncated {
			t.Fatal(c, got)
		}
	}
}

func exerciseRetrievalPolicyReplay(t *testing.T, ctx context.Context, tx pgx.Tx, backend *postgresDataStore) {
	t.Helper()
	execSQL := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	execSQL(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	execSQL(`INSERT INTO memories(tier,kind,key,content,scope_type,scope_value)
 SELECT 'L2','fact','policy-fixture-'||n,'policyneedle '||repeat('long-content ',500),'project','policy-app' FROM generate_series(1,25)n;
 INSERT INTO memories(tier,kind,key,content,scope_type,scope_value) VALUES('L2','fact','hidden-policy','policyneedle hidden','project','hidden-policy');
 DELETE FROM bandit_decisions WHERE decision_point='kb_memory_retrieval_limit';
 DELETE FROM bandit_arm_stats WHERE decision_point='kb_memory_retrieval_limit';
 RESET ROLE;
 INSERT INTO bandit_promotions(decision_point,arm_id) VALUES('kb_memory_retrieval_limit','10') ON CONFLICT(decision_point) DO UPDATE SET arm_id='10';
 SET LOCAL ROLE aimee_store_runtime`)
	values := map[string]any{}
	bound := *backend
	bound.fusionEnabled = false
	bound.settings = func() (map[string]any, error) { return values, nil }
	handler := NewHandler(nil, WithDataStore(PlacementKB, &bound))
	client := clientForHandler(t, handler)
	run := func(extra map[string]any, want int) map[string]any {
		t.Helper()
		args := map[string]any{"query": "policyneedle", "scope_context": true, "project": "policy-app"}
		for k, v := range extra {
			args[k] = v
		}
		raw, _ := json.Marshal(args)
		r := runPublicCommand(t, client, "find_facts", string(raw))
		facts, ok := r["facts"].([]any)
		if !ok || r["status"] != "ok" || len(facts) != want {
			t.Fatal(r, want)
		}
		for _, fact := range facts {
			row := fact.(map[string]any)
			if row["key"] == "hidden-policy" || len(row["content"].(string)) <= 4096 {
				t.Fatal(row)
			}
		}
		return r
	}
	run(nil, 10) // Operator promotion is honored while live decisions are disabled.
	run(map[string]any{"limit": 3}, 3)
	run(map[string]any{"limit": 0}, 20)
	// Server facts and compatibility windows are one scoped Go operation. Both
	// preserve complete records, and a second-lane failure cannot look empty.
	serverSearch := func(terms []string, want int) {
		t.Helper()
		raw, _ := json.Marshal(map[string]any{"view": "server", "keywords": terms, "limit": 3,
			"scope_context": true, "project": "policy-app"})
		r := runPublicCommand(t, client, "search", string(raw))
		if r["status"] != "ok" || r["store"] != "kb" || r["active_context_missing"] != false {
			t.Fatal(r)
		}
		facts, windows := r["facts"].([]any), r["windows"].([]any)
		if len(facts) != want || len(windows) != want {
			t.Fatalf("server search facts=%d windows=%d want=%d", len(facts), len(windows), want)
		}
		for _, fact := range facts {
			row := fact.(map[string]any)
			if row["key"] == "hidden-policy" || len(row["content"].(string)) <= 4096 {
				t.Fatal("server search lost scope or full content")
			}
		}
		for _, window := range windows {
			if len(window.(map[string]any)["summary"].(string)) <= 4096 {
				t.Fatal("server search truncated window")
			}
		}
	}
	serverSearch([]string{"policyneedle"}, 3)
	serverSearch([]string{strings.Repeat("policyneedle ", 250), "absentfinalterm"}, 0)
	execSQL(`RESET ROLE; ALTER TABLE memories RENAME COLUMN artifact_ref TO fixture_hidden_artifact; SET LOCAL ROLE aimee_store_runtime`)
	if r := runPublicCommand(t, client, "search", `{"view":"server","keywords":["policyneedle"],"limit":3,"scope_context":true,"project":"policy-app"}`); r["kind"] != "unavailable" || r["facts"] != nil || r["windows"] != nil {
		t.Fatal("window failure published partial search", r)
	}
	execSQL(`RESET ROLE; ALTER TABLE memories RENAME COLUMN fixture_hidden_artifact TO artifact_ref; SET LOCAL ROLE aimee_store_runtime`)
	serverSearch([]string{"policyneedle"}, 3)
	dir := t.TempDir()
	capture, script := filepath.Join(dir, "input.json"), filepath.Join(dir, "optimize")
	command := func(response string) {
		t.Helper()
		source := "#!/bin/sh\ncat > '" + strings.ReplaceAll(capture, "'", "'\\''") + "'\nprintf '%s' '" + strings.ReplaceAll(response, "'", "'\\''") + "'\n"
		if err := os.WriteFile(script, []byte(source), 0700); err != nil {
			t.Fatal(err)
		}
	}
	command(`{"status":"ok","selected_arm":"20","propensity":0.7}`)
	values["bandit_live_decision_enabled"], values["bandit_optimize_command"], values["bandit_exploration_fraction"] = true, "'"+strings.ReplaceAll(script, "'", "'\\''")+"'", 1.0
	run(nil, 10)
	if _, err := os.Stat(capture); !os.IsNotExist(err) {
		t.Fatal("observe-only point sampled count-trained weights", err)
	}
	var decisions int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM bandit_decisions WHERE decision_point=$1`, retrievalDecisionPoint).Scan(&decisions); err != nil || decisions != 0 {
		t.Fatal(decisions, err)
	}
	// Even an irrelevant singleton on a previously opened decision is metadata.
	execSQL(`INSERT INTO bandit_decisions(id,decision_point,arm_id,propensity,decided_at) VALUES('count-only-fixture','kb_memory_retrieval_limit','20',1,pg_now_text())`)
	direct := bound
	direct.db = runtimeRoleTx{evalQueryer{tx}, t}
	decision := retrievalDecision{limit: 20, id: "count-only-fixture", arm: "20"}
	if err := direct.recordRetrievalAvailability(ctx, decision, 1); err != nil {
		t.Fatal(err)
	}
	if err := direct.recordRetrievalAvailability(ctx, decision, 20); err != nil {
		t.Fatal(err)
	}
	var count int
	var truncated, unrewarded, unclosed bool
	if err := tx.QueryRow(ctx, `SELECT result_count,result_truncated,reward IS NULL,closed_at='' FROM bandit_decisions WHERE id='count-only-fixture'`).Scan(&count, &truncated, &unrewarded, &unclosed); err != nil || count != 1 || truncated || !unrewarded || !unclosed {
		t.Fatal(count, truncated, unrewarded, unclosed, err)
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM bandit_arm_stats WHERE decision_point=$1`, retrievalDecisionPoint).Scan(&decisions); err != nil || decisions != 0 {
		t.Fatal("count updated posterior", decisions, err)
	}
	run(map[string]any{"limit": 2}, 2)
	run(map[string]any{"query": "missing-policy-query"}, 0)
	// Missing policy metadata does not poison a successful scoped lookup.
	execSQL(`RESET ROLE; REVOKE SELECT ON bandit_promotions FROM aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`)
	run(nil, 20)
	execSQL(`RESET ROLE; GRANT SELECT ON bandit_promotions TO aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`)
}
