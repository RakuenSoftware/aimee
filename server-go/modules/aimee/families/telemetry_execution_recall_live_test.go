package families

import (
	"context"
	"encoding/json"
	"fmt"
	wire "github.com/JBailes/aimee/server-go/aimee"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	memory "github.com/JBailes/aimee/server-go/modules/memory"
	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
	"github.com/jackc/pgx/v5/pgxpool"
	"os"
	"strings"
	"testing"
	"time"
)

func TestExecutionGradeRecallRequiresHost(t *testing.T) {
	db := newDelDB()
	frame, err := wire.EncodeFields(opExecutionTestGradeRecall, []string{"operator", "project", ""})
	if err != nil {
		t.Fatal(err)
	}
	_, status := Telemetry.Handler(db)(ring.ModuleInvocation{StageID: StageTelemetry, PrincipalRef: 9}, frame)
	if status != ring.ModuleStatusInvalidRequest || len(db.executed) != 0 {
		t.Fatal("foreign module read history", status)
	}
	for _, fields := range [][]string{{"", "project"}, {"operator", ""}, {"operator", strings.Repeat("p", 257)}} {
		status, _, err := executionTestGradeRecall(context.Background(), nil, fields)
		if status != store.StatusInvalid || err != nil {
			t.Fatal(status, err)
		}
	}
}

func TestExecutionGradeRecallPostgresNextRunScopeAndErasure(t *testing.T) {
	root := livePool(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	schema := fmt.Sprintf("execution_recall_%d", time.Now().UnixNano())
	if _, err := root.Exec(ctx, "CREATE SCHEMA "+schema); err != nil {
		t.Fatal(err)
	}
	defer root.Exec(context.Background(), "DROP SCHEMA "+schema+" CASCADE")
	config, err := pgxpool.ParseConfig(os.Getenv("AIMEE_TEST_PG_URL"))
	if err != nil {
		t.Fatal(err)
	}
	config.ConnConfig.RuntimeParams["search_path"] = schema
	pool, err := pgxpool.NewWithConfig(ctx, config)
	if err != nil {
		t.Fatal(err)
	}
	defer pool.Close()
	for _, name := range []string{"schema_sessions.sql", "schema_session_ir_events.sql", "schema_native_cache_recovery.sql", "schema_learning_projection_outbox.sql", "schema_telemetry.sql", "schema_delegation.sql"} {
		body, err := schemaFS.ReadFile(name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err = pool.Exec(ctx, string(body)); err != nil {
			t.Fatal(err)
		}
	}
	terminals := map[string]string{}
	for _, entry := range []struct{ session, actor, project, verdict string }{{"ours", "operator", "project-a", "passed"}, {"other-project", "operator", "project-b", "passed"}, {"other-actor", "foreign", "project-a", "passed"}, {"failed", "operator", "project-a", "failed"}} {
		if _, err = pool.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES($1,$2)`, entry.session, entry.actor); err != nil {
			t.Fatal(err)
		}
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		metadata := sessionIRVerificationFixture()
		metadata.Session = entry.session
		metadata.Turn = "turn"
		metadata.Actor = entry.actor
		metadata.ScopeID = entry.project
		if entry.verdict == "failed" {
			metadata.Steps[0].ExitCode = "1"
		}
		terminals[entry.session] = sessionIRVerificationEvent(t, metadata)
		status, _, err := sessionIRAppendVerified(ctx, sessionIRLiveTx{tx}, []string{entry.session, "terminal", "turn", entry.actor, terminals[entry.session]})
		if err != nil || status != store.StatusOK {
			t.Fatal(status, err)
		}
		proof, _ := json.Marshal(metadata)
		grade := localExecutionTestGrade{SchemaVersion: 1, Source: executionGradeSource{Kind: "canonical_session_log", Session: entry.session, Turn: "turn", Sequence: "1", Event: "terminal", Actor: entry.actor}, ObservedStatus: "completed", Verification: "tests_bound_revision", Qualification: "configured_tests_only", Verdict: entry.verdict, VerificationID: "sha256:" + strings.Repeat("a", 64), Proof: proof}
		raw, _ := json.Marshal(grade)
		status, _, err = executionTestGradeRecord(ctx, sessionIRLiveTx{tx}, []string{entry.actor, string(raw)})
		if err != nil || status != store.StatusOK {
			t.Fatal(status, err)
		}
		if err = tx.Commit(ctx); err != nil {
			t.Fatal(err)
		}
	}
	recall := func(actor, project string) string {
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		defer tx.Rollback(ctx)
		status, fields, err := executionTestGradeRecall(ctx, sessionIRLiveTx{tx}, []string{actor, project})
		if err != nil || status != store.StatusOK || len(fields) != 1 {
			t.Fatal(status, fields, err)
		}
		return fields[0]
	}
	raw := recall("operator", "project-a")
	if !strings.Contains(raw, `"configuration_sha256":"`+strings.Repeat("d", 64)+`"`) || !strings.Contains(raw, `"profile_sha256":"`+strings.Repeat("e", 64)+`"`) || strings.Contains(raw, `"configuration_digest"`) || strings.Contains(raw, `"profile_digest"`) {
		t.Fatal("recall lost actual configuration/profile cohort keys", raw)
	}
	// A later host invocation consumes the durable projection with server-local
	// memory placement and no KB/executor. This is test evidence, not instructions.

	memoryHandler := memory.NewHandler(nil, memory.WithDataStore(memory.PlacementServer, nil))
	runtimeCall := func(args map[string]any) map[string]any {
		body, _ := json.Marshal(args)
		frame, _ := ring.EncodeCommand("runtime", body)
		rendered, status := memoryHandler(ring.ModuleInvocation{StageID: memory.StageCommand}, frame)
		payload, err := ring.DecodeCommandResult(rendered)
		var result map[string]any
		if status != ring.ModuleStatusOK || err != nil || json.Unmarshal(payload, &result) != nil {
			t.Fatal("runtime contract", status, err, string(rendered))
		}
		return result
	}
	releaseArgs := map[string]any{"operation": "ingress-assemble", "project": "project-a", "budget": 8192, "execution_test_grades": json.RawMessage(raw), "prepare_source_release": true, "request_id": "later-run", "principal": "operator"}
	assembled := runtimeCall(releaseArgs)
	if assembled["status"] != "ok" || !strings.Contains(assembled["envelope"].(string), "configured-test-history") {
		t.Fatal("next-run canonical context", assembled)
	}
	ticket := assembled["source_release_ticket"]
	if value, ok := ticket.(string); !ok || value == "" {
		t.Fatal("assembly did not retain receipt source handle", assembled)
	}
	fenceArgs := map[string]any{"operation": "source-release-plan", "request_id": "later-run", "principal": "operator", "project": "project-a", "source_release_ticket": ticket}
	// Prepare a provider check before the canonical source changes. The host
	// re-reads the actual source after preparation, then submits its joined answer.
	providerPlan := runtimeCall(fenceArgs)
	runFence := func(plan map[string]any) map[string]any {
		expected, _ := json.Marshal(plan["execution_grade_request"])
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		defer tx.Rollback(ctx)
		status, fields, err := executionTestGradeRecall(ctx, sessionIRLiveTx{tx}, []string{"operator", "project-a", string(expected)})
		if err != nil || status != store.StatusOK {
			t.Fatal(status, err, "execution-grade request", string(expected), "source-release plan", plan)
		}
		answer := map[string]any{}
		for k, v := range fenceArgs {
			answer[k] = v
		}
		answer["operation"] = "source-release-result"
		answer["execution_grade_response"] = json.RawMessage(fields[0])
		return runtimeCall(answer)
	}
	if admitted := runFence(providerPlan); admitted["admitted"] != true {
		t.Fatal("unchanged source refused", admitted)
	}
	providerPlan = runtimeCall(fenceArgs)

	// Targeted provider fence re-reads these exact sources, independently of
	// recent-history ordering, and echoes the current opaque admission attempt.
	var expected map[string]any
	_ = json.Unmarshal([]byte(raw), &expected)
	expected["check_id"] = strings.Repeat("b", 32)
	wanted, _ := json.Marshal(expected)
	check := func() string {
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		defer tx.Rollback(ctx)
		status, fields, err := executionTestGradeRecall(ctx, sessionIRLiveTx{tx}, []string{"operator", "project-a", string(wanted)})
		if err != nil || status != store.StatusOK {
			t.Fatal(status, err)
		}
		if !strings.Contains(fields[0], `"check_id":"`+strings.Repeat("b", 32)+`"`) {
			t.Fatal("unbound readback", fields)
		}
		return fields[0]
	}
	if checked := check(); !strings.Contains(checked, `"session_id":"ours"`) {
		t.Fatal("targeted source absent", checked)
	}
	var snapshot struct {
		Items []localExecutionTestGrade `json:"items"`
	}
	if json.Unmarshal([]byte(raw), &snapshot) != nil || len(snapshot.Items) != 2 {
		t.Fatal(raw)
	}
	if strings.Contains(raw, "other-project") || strings.Contains(raw, "other-actor") || !strings.Contains(raw, "configured_tests_only") || !strings.Contains(raw, `"verdict":"failed"`) {
		t.Fatal("cross-scope or false success", raw)
	}
	if raw = recall("nobody", "project-a"); !strings.Contains(raw, `"items":[]`) {
		t.Fatal(raw)
	}
	if _, err = pool.Exec(ctx, `UPDATE session_ir_events SET payload='{"kind":"execution","payload":{"status":"cancelled"}}' WHERE session_id='ours'`); err != nil {
		t.Fatal(err)
	}
	if admitted := runFence(providerPlan); admitted["admitted"] == true {
		t.Fatal("source changed after prepare but dispatch admitted", admitted)
	}
	providerPlan = runtimeCall(fenceArgs)
	if checked := check(); strings.Contains(checked, `"session_id":"ours"`) {
		t.Fatal("provider fence accepted cancelled source", checked)
	}
	if raw = recall("operator", "project-a"); strings.Contains(raw, `"session_id":"ours"`) {
		t.Fatal("invalid terminal recalled", raw)
	}
	if _, err = pool.Exec(ctx, `DELETE FROM server_sessions WHERE id='failed'`); err != nil {
		t.Fatal(err)
	}
	if admitted := runFence(providerPlan); admitted["admitted"] == true {
		t.Fatal("source erased after prepare but dispatch admitted", admitted)
	}
	if raw = recall("operator", "project-a"); !strings.Contains(raw, `"items":[]`) {
		t.Fatal("erased source recalled", raw)
	}

	// Ownership changes after preparation also revoke the old actor's dispatch.
	if _, err = pool.Exec(ctx, `UPDATE session_ir_events SET payload=$1 WHERE session_id='ours'`, terminals["ours"]); err != nil {
		t.Fatal(err)
	}
	releaseArgs["execution_test_grades"] = json.RawMessage(recall("operator", "project-a"))
	assembled = runtimeCall(releaseArgs)
	fenceArgs["source_release_ticket"] = assembled["source_release_ticket"]
	providerPlan = runtimeCall(fenceArgs)
	if _, err = pool.Exec(ctx, `UPDATE server_sessions SET principal='foreign' WHERE id='ours'`); err != nil {
		t.Fatal(err)
	}
	if admitted := runFence(providerPlan); admitted["admitted"] == true {
		t.Fatal("changed session owner retained dispatch", admitted)
	}
}
