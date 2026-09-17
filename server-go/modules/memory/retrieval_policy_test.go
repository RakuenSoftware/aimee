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

func TestRecallSufficiency(t *testing.T) {
	for _, c := range []struct {
		count, limit int
		reward       float64
	}{{0, 20, 0}, {4, 20, 1}, {20, 20, 0.5}, {21, 20, 0.5}} {
		if got := recallSufficiency(c.count, c.limit); got != c.reward {
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
	run(nil, 20)
	raw, err := os.ReadFile(capture)
	if err != nil {
		t.Fatal(err)
	}
	var request map[string]any
	if json.Unmarshal(raw, &request) != nil {
		t.Fatal(string(raw))
	}
	inputs := request["inputs"].(map[string]any)
	if request["role"] != "optimize" || inputs["decision_point"] != retrievalDecisionPoint || inputs["allow_explore"] != true || len(inputs["arms"].([]any)) != 2 {
		t.Fatal(request)
	}
	var id string
	var reward, alpha, beta float64
	var rewards int
	if err := tx.QueryRow(ctx, `SELECT id,reward FROM bandit_decisions WHERE decision_point=$1`, retrievalDecisionPoint).Scan(&id, &reward); err != nil || reward != 0.5 {
		t.Fatal(id, reward, err)
	}
	if err := tx.QueryRow(ctx, `SELECT n_rewards,posterior_alpha,posterior_beta FROM bandit_arm_stats WHERE decision_point=$1 AND arm_id='20'`, retrievalDecisionPoint).Scan(&rewards, &alpha, &beta); err != nil || rewards != 1 || alpha != 1.5 || beta != 1.5 {
		t.Fatal(rewards, alpha, beta, err)
	}
	// Idempotent closure must not credit the same response twice.
	direct := bound
	direct.db = runtimeRoleTx{evalQueryer{tx}, t}
	if err := direct.rewardRetrieval(ctx, retrievalDecision{limit: 20, id: id, arm: "20"}, 20); err != nil {
		t.Fatal(err)
	}
	if err := tx.QueryRow(ctx, `SELECT n_rewards FROM bandit_arm_stats WHERE decision_point=$1 AND arm_id='20'`, retrievalDecisionPoint).Scan(&rewards); err != nil || rewards != 1 {
		t.Fatal(rewards, err)
	}
	// An explicit caller limit bypasses the sidecar entirely.
	if err := os.Remove(capture); err != nil {
		t.Fatal(err)
	}
	run(map[string]any{"limit": 2}, 2)
	if _, err := os.Stat(capture); !os.IsNotExist(err) {
		t.Fatal("explicit limit invoked policy", err)
	}
	// Malformed/unknown selection retains the operator's default and logs no decision.
	command(`{"status":"ok","selected_arm":"999","propensity":0.7}`)
	run(nil, 10)
	var decisions int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM bandit_decisions WHERE decision_point=$1`, retrievalDecisionPoint).Scan(&decisions); err != nil || decisions != 1 {
		t.Fatal(decisions, err)
	}
	command(`{"status":"ok","selected_arm":"20","propensity":0.7}`)
	execSQL(`INSERT INTO bandit_decisions(id,decision_point,arm_id,decided_at,is_exploration)
 SELECT 'policy-budget-'||n,'kb_memory_retrieval_limit','10',pg_now_text(),true FROM generate_series(1,20)n`)
	run(nil, 20)
	raw, err = os.ReadFile(capture)
	if err != nil {
		t.Fatal(err)
	}
	json.Unmarshal(raw, &request)
	if request["inputs"].(map[string]any)["allow_explore"] != false {
		t.Fatal("exploration budget ignored", request)
	}
	// Optional telemetry must not poison PostgreSQL's retrieval transaction.
	execSQL(`RESET ROLE; REVOKE INSERT ON bandit_decisions FROM aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`)
	run(nil, 10)
	execSQL(`RESET ROLE; GRANT INSERT ON bandit_decisions TO aimee_store_runtime; REVOKE INSERT ON bandit_arm_stats FROM aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`)
	run(nil, 20)
	execSQL(`RESET ROLE; GRANT INSERT ON bandit_arm_stats TO aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`)
	// Zero results still close the learning loop with zero reward.
	run(map[string]any{"query": "missing-policy-query"}, 0)
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM bandit_decisions WHERE decision_point=$1 AND reward=0`, retrievalDecisionPoint).Scan(&decisions); err != nil || decisions != 1 {
		t.Fatal(decisions, err)
	}
	if err := tx.QueryRow(ctx, `SELECT n_rewards,posterior_alpha,posterior_beta FROM bandit_arm_stats WHERE decision_point=$1 AND arm_id='20'`, retrievalDecisionPoint).Scan(&rewards, &alpha, &beta); err != nil || rewards != 3 || alpha != 2 || beta != 3 {
		t.Fatal(rewards, alpha, beta, err)
	}

}
