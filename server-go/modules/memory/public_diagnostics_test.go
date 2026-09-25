package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"math"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestDiagnosticTraceContributions(t *testing.T) {
	if traceFingerprint("") != "14650fb0739d0383" || traceFingerprint("hello") != "005a0d15131ec7a1" {
		t.Fatal("trace fingerprint contract changed")
	}
	for i := 1; i <= 16; i++ {
		t.Run(fmt.Sprint(i), func(t *testing.T) {
			t.Parallel()
			rows := diagnosticTraceRows([]Diagnostic{{Memory: Record{ID: int64(i)}, EpistemicKind: "preference"}}, []publicDiagnostic{{Memory: publicMemoryRecord{ID: int64(i), HybridRank: 1, RetrievalScore: 0.7}, Parts: DiagnosticParts{Lexical: 0.3, Coverage: 0.2, Salience: 0.1, GraphScore: 0.4, GraphWeight: 0.25, Outcome: 0.05, Total: 42}}})
			if len(rows) != 1 || rows[0]["subject_id"] != fmt.Sprint(i) || rows[0]["epistemic_kind"] != "preference" {
				t.Fatal(rows)
			}
			values := rows[0]["feature_values"].(map[string]float64)
			weights := rows[0]["feature_weights"].(map[string]float64)
			contributions := rows[0]["feature_contributions"].(map[string]float64)
			total := 0.0
			for name, value := range values {
				if math.Abs(value*weights[name]-contributions[name]) > 1e-12 {
					t.Fatal(name)
				}
				total += contributions[name]
			}
			if math.Abs(total-0.7) > 1e-12 || contributions["post_rank_residual"] >= 0 {
				t.Fatal(total, contributions)
			}
		})
	}
}

func TestDiagnosticPublicValidation(t *testing.T) {
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, nil)))
	for _, c := range []struct{ verb, args string }{{"diagnose_scoped", `{}`}, {"diagnose_scoped", `{"query":"x","scope_type":"project"}`}, {"explain_match", `{"query":"x","memory_id":1.5}`}} {
		if r := runPublicCommand(t, client, c.verb, c.args); r["kind"] != "invalid_argument" {
			t.Fatal(r)
		}
	}
	if r := runPublicCommand(t, client, "diagnose_scoped", `{"query":"x"}`); r["kind"] != "unavailable" {
		t.Fatal(r)
	}
}

func exerciseDiagnosticReplay(t *testing.T, ctx context.Context, tx pgx.Tx, handler bus.ModuleHandler) {
	t.Helper()
	caller := bus.CommandContext{Authenticated: true, Principal: "user:diagnostic", TransportIdentity: "cert:diagnostic", ScopeKind: "project", ScopeID: "runtime-project-b"}
	run := func(args string, verified bool) map[string]any {
		t.Helper()
		c := bus.CommandContext{}
		if verified {
			c = caller
		}
		r, status := invokeContextCommand(t, handler, 0, c, "diagnose_scoped", args)
		if status != bus.ModuleStatusOK || r["status"] != "ok" {
			t.Fatal(status, r)
		}
		return r
	}
	base := `"query":"runtime-role-probe","scope_type":"project","scope_value":"runtime-project-b"`
	foreign := caller
	foreign.ScopeID = "verified-other-scope"
	if result, status := invokeContextCommand(t, handler, 0, foreign, "diagnose_scoped", `{`+base+`}`); status == bus.ModuleStatusOK && result["status"] == "ok" {
		t.Fatal("diagnostic admitted scope beyond verified credential", result)
	}
	plain := run(`{`+base+`}`, true)
	traced := run(`{`+base+`,"trace":true,"persist_trace":true,"retrieval_event_id":"runtime-diagnostic","turn_id":"runtime-turn"}`, true)
	before, _ := json.Marshal(plain["rows"])
	after, _ := json.Marshal(traced["rows"])
	if string(before) != string(after) {
		t.Fatal("tracing changed ranking or content", plain, traced)
	}
	trace, ok := traced["trace"].(map[string]any)
	if !ok || trace["persisted"] != true || trace["scope_id"] != "runtime-project-b" || trace["query_fingerprint"] != traceFingerprint("runtime-role-probe") {
		t.Fatal(traced)
	}
	rows := traced["rows"].([]any)
	if len(rows) != 1 || len(rows[0].(map[string]any)["memory"].(map[string]any)) != 16 {
		t.Fatal(rows)
	}
	parts := rows[0].(map[string]any)["parts"].(map[string]any)
	if parts["score_evidence"] != "observed_ranking_steps" || parts["ranking_steps"] == nil {
		t.Fatal("diagnostic lost actual ranking evidence", parts)
	}
	var features string
	if err := tx.QueryRow(ctx, `SELECT r.feature_values FROM recall_traces t JOIN recall_trace_results r USING(trace_id)
 WHERE t.retrieval_event_id='runtime-diagnostic' AND t.scope_id='runtime-project-b'`).Scan(&features); err != nil {
		t.Fatal(err)
	}
	var decoded map[string]float64
	if err := json.Unmarshal([]byte(features), &decoded); err != nil || decoded["ranking_trace_schema"] != 1 || decoded["stage.1.candidate_order.candidate_order.rank"] != 1 || decoded["stage.0.native_pg_ts_rank_cd_exact_key_scope_priority.lexical.rank"] != 1 || decoded["stage.0.native_pg_ts_rank_cd_exact_key_scope_priority.score"] <= 0 {
		t.Fatal("durable trace omitted observed rank", features, err)
	}
	var metadata string
	if err := tx.QueryRow(ctx, `SELECT candidate_metadata FROM recall_traces WHERE retrieval_event_id='runtime-diagnostic' AND scope_id='runtime-project-b'`).Scan(&metadata); err != nil {
		t.Fatal(err)
	}
	var capture rankingCapture
	if json.Unmarshal([]byte(metadata), &capture) != nil || len(capture.Candidates) != 1 || capture.Candidates[0].Version == nil || capture.Candidates[0].Disposition != "selected" {
		t.Fatal("lost durable candidate metadata", metadata)
	}
	var count int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM recall_traces t JOIN recall_trace_results r USING(trace_id) WHERE t.retrieval_event_id='runtime-diagnostic' AND t.scope_id='runtime-project-b' AND r.epistemic_kind='world_fact'`).Scan(&count); err != nil || count != 1 {
		t.Fatal(count, err)
	}
	denied := run(`{`+base+`,"trace":true,"authenticated":true,"principal":"user:forged","scope_kind":"project","scope_id":"forged"}`, false)
	if denied["trace_status"] != "authenticated actor required" || denied["trace"] != nil {
		t.Fatal(denied)
	}
	// Without an explicit retrieval scope, trace metadata comes only from the
	// separately verified host context, never similarly named request fields.
	fallback := run(`{"query":"absent","trace":true,"scope_kind":"project","scope_id":"forged"}`, true)
	if trace, ok := fallback["trace"].(map[string]any); !ok || trace["scope_id"] != "runtime-project-b" {
		t.Fatal(fallback)
	}
	// Persistence failure is isolated from the successful read transaction.
	if _, err := tx.Exec(ctx, `RESET ROLE; ALTER TABLE recall_traces ADD CONSTRAINT runtime_trace_failure CHECK (retrieval_event_id<>'runtime-trace-failure') NOT VALID; SET LOCAL ROLE aimee_store_runtime`); err != nil {
		t.Fatal(err)
	}
	failed := run(`{`+base+`,"trace":true,"persist_trace":true,"retrieval_event_id":"runtime-trace-failure"}`, true)
	if failed["trace_status"] != "trace recording failed" || len(failed["rows"].([]any)) != 1 {
		t.Fatal(failed)
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM recall_traces WHERE retrieval_event_id='runtime-trace-failure'`).Scan(&count); err != nil || count != 0 {
		t.Fatal(count, err)
	}
	if _, err := tx.Exec(ctx, `RESET ROLE; ALTER TABLE recall_traces DROP CONSTRAINT runtime_trace_failure; SET LOCAL ROLE aimee_store_runtime`); err != nil {
		t.Fatal(err)
	}
}
