package memory

import (
	"context"
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"strings"
	"testing"
	"unicode/utf8"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestTypedProjectionRendersReviewedProcedureOnce(t *testing.T) {
	build := func() *typedContextResult {
		r := newTypedContext(DataRequest{TypedContext: typedTestOptions(t, `{}`)})
		r.add("observations", typedItem{id: "observed-1", text: "reported", value: map[string]any{"summary": "reported"}})
		r.add("approved_procedures", typedItem{id: "reviewed-1", text: "do not erase 界", value: map[string]any{
			"proposal_id": "9007199254740993", "state": "committed", "procedure": "do not erase 界",
		}})
		if err := r.finish(); err != nil {
			t.Fatal(err)
		}
		return r
	}
	r := build()
	boundary := strings.Index(r.Rendered, `<approved_procedures authority="reviewed" authorization="none">`)
	if boundary < 0 || strings.Count(r.Rendered, "do not erase 界") != 1 || strings.Contains(r.Rendered[:boundary], "9007199254740993") {
		t.Fatal("reviewed procedure duplicated or lost trust separation", r.Rendered)
	}
	for _, diagnostic := range []string{"budget_tokens", "used_tokens", "enabled", "packing_trace"} {
		if strings.Contains(r.Rendered, diagnostic) {
			t.Fatal("response diagnostics leaked into prompt projection", diagnostic)
		}
	}
	if r.RenderedBytes != len(r.Rendered) || r.ProjectionDigest != fmt.Sprintf("sha256:%x", sha256.Sum256([]byte(r.Rendered))) || r.TokenCountState != "unavailable" || r.ProjectionVersion != 1 {
		t.Fatal("projection accounting misrepresents bytes or token certainty", r)
	}
	if r.Sufficiency != "unknown" || r.Availability != "available" {
		t.Fatal("nonempty projection claimed task coverage without requirements", r)
	}
	if len(r.Retained) != 2 || r.Retained[0] != (typedProjectionRef{"observations", "observed-1"}) || r.Retained[1] != (typedProjectionRef{"approved_procedures", "reviewed-1"}) {
		t.Fatal("projection retained identities mismatch", r.Retained)
	}
	if again := build(); again.Rendered != r.Rendered || again.ProjectionDigest != r.ProjectionDigest {
		t.Fatal("projection is not deterministic", r, again)
	}
	legacyChannels, _ := json.Marshal(r.Channels)
	legacyProcedures, _ := json.Marshal(r.Channels["approved_procedures"].Items)
	legacy := `<memory_data trust="untrusted" authorization="none">` + string(legacyChannels) + "</memory_data>\n" + `<approved_procedures authority="reviewed" authorization="none">` + string(legacyProcedures) + `</approved_procedures>`
	t.Logf("same evidence: previous projection %d bytes, current projection %d bytes", len(legacy), r.RenderedBytes)
	if r.RenderedBytes >= len(legacy) {
		t.Fatal("projection retained diagnostic or duplicate-procedure overhead")
	}
	degraded := newTypedContext(DataRequest{TypedContext: typedTestOptions(t, `{}`)})
	degraded.fail("observations", "owner unavailable")
	if err := degraded.finish(); err != nil || degraded.Availability != "degraded" || degraded.Sufficiency != "unknown" {
		t.Fatal("unavailable retrieval was reported as an empty successful result", degraded, err)
	}
}

func typedTestOptions(t *testing.T, raw string) *typedContextOptions {
	t.Helper()
	var args commandArgs
	if err := json.Unmarshal([]byte(raw), &args); err != nil {
		t.Fatal(err)
	}
	return typedOptions(args)
}
func TestTypedContextDefaultsAndCompleteBudget(t *testing.T) {
	cfg := typedTestOptions(t, `{}`)
	if !cfg.Enabled || !cfg.Flags["current_assertions"] || !cfg.Flags["observations"] || !cfg.Flags["approved_procedures"] || cfg.Flags["historical_assertions"] || cfg.Flags["episodes"] || cfg.Budgets["total"] != 2400 {
		t.Fatal(cfg)
	}
	disabled := typedTestOptions(t, `{"enabled":false,"enable_episodes":true,"channel_budgets":{"total":1e100,"episodes":-5}}`)
	for _, enabled := range disabled.Flags {
		if enabled {
			t.Fatal(disabled)
		}
	}
	if disabled.Budgets["total"] != 4096 || disabled.Budgets["episodes"] != 0 {
		t.Fatal(disabled.Budgets)
	}
	cfg.Flags["working_context"] = true
	r := newTypedContext(DataRequest{TypedContext: cfg})
	r.add("current_assertions", typedItem{value: map[string]any{"assertion_id": int64(9007199254740993), "text": "useful 界 fact"}, id: "9007199254740993", text: "useful 界 fact"})
	// A short excerpt cannot hide an oversized metadata field from final budgeting.
	r.add("working_context", typedItem{value: map[string]any{"text": "x", "metadata": strings.Repeat("界", 5000)}, id: "turn:0", text: "x"})
	if err := r.finish(); err != nil {
		t.Fatal(err)
	}
	if len(r.Channels["current_assertions"].Items) != 1 || len(r.Channels["working_context"].Items) != 0 || r.RenderedTokens > r.Budget || !utf8.ValidString(r.Rendered) || !strings.Contains(r.Rendered, `"assertion_id":9007199254740993`) {
		t.Fatal(r)
	}
	if !strings.Contains(r.Rendered, `<memory_data trust="untrusted" authorization="none">`) || !strings.Contains(r.Rendered, `<approved_procedures authority="reviewed" authorization="none">`) {
		t.Fatal(r.Rendered)
	}
	cfg.Budgets["total"] = 0
	r = newTypedContext(DataRequest{TypedContext: cfg})
	if err := r.finish(); err != nil {
		t.Fatal(err)
	}
	if r.EnvelopeExcess <= 0 || r.Used != 0 || r.Sufficiency != "insufficient" {
		t.Fatal(r)
	}
}
func TestTypedContextHostBoundary(t *testing.T) {
	frame, _ := bus.EncodeCommand("runtime", []byte(`{"operation":"typed-context","query":"x"}`))
	if _, status := NewHandler(nil, WithDataStore(PlacementKB, nil))(bus.ModuleInvocation{StageID: StageCommand, PrincipalRef: 73}, frame); status != bus.ModuleStatusInvalidRequest {
		t.Fatal(status)
	}
	if _, status := NewHandler(nil, WithDataStore(PlacementServer, nil))(bus.ModuleInvocation{StageID: StageCommand}, frame); status != bus.ModuleStatusCapabilityAbsent {
		t.Fatal(status)
	}
}
func exerciseTypedContextReplay(t *testing.T, ctx context.Context, tx pgx.Tx, backend *postgresDataStore) {
	t.Helper()
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SAVEPOINT typed_context_replay`)
	defer func() { exec(`ROLLBACK TO SAVEPOINT typed_context_replay; RELEASE SAVEPOINT typed_context_replay`) }()
	exec(`RESET ROLE; SELECT set_config('aimee.memory_scope_all','1',true)`)
	var parent, hidden, signal int64
	if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value) VALUES('L2','fact','typed-fixture','typed-fixture episode','project','typed-local') RETURNING id`).Scan(&parent); err != nil {
		t.Fatal(err)
	}
	if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value) VALUES('L2','fact','typed-fixture','hidden','project','typed-private') RETURNING id`).Scan(&hidden); err != nil {
		t.Fatal(err)
	}
	const large int64 = 9007199254743001
	exec(`INSERT INTO memory_episodes(id,memory_id,episode_key,episode_text,source_session,reference_time,created_at) VALUES($1,$2,'typed-fixture','typed-fixture episode 界','session','2026-01-01','2026-01-02')`, large, parent)
	exec(`INSERT INTO memory_episodes(memory_id,episode_key,episode_text,reference_time,created_at) VALUES($1,'typed-fixture','hidden episode','9999-12-31','9999-12-31')`, hidden)
	exec(`INSERT INTO memory_relations(memory_id,src_entity,relation,dst_entity,fact_text) VALUES($1,'typed-fixture','has','summary','visible summary 界')`, parent)
	exec(`INSERT INTO learning_observations(observation_id,scope_kind,scope_id,observation_type,title,summary,status,confidence,synthesis_policy_version,evidence_count,refreshed_at) VALUES
 ('typed-local','project','typed-local','successful_recovery','visible','visible observation 界','active',.9,'fixture',2,'2026-01-02'),
 ('typed-global','global','','successful_recovery','global','global observation','active',.8,'fixture',1,'2026-01-01')`)
	exec(`INSERT INTO learning_observations(observation_id,scope_kind,scope_id,observation_type,title,summary,status,confidence,synthesis_policy_version,evidence_count,refreshed_at)
 SELECT 'typed-hidden-'||i,'project','typed-private','successful_recovery','hidden','hidden observation','active',1,'fixture',1,'9999-12-31' FROM generate_series(1,70)i`)
	if err := tx.QueryRow(ctx, `INSERT INTO learning_signals(signal_type) VALUES('typed-fixture') RETURNING id`).Scan(&signal); err != nil {
		t.Fatal(err)
	}
	exec(`INSERT INTO learning_proposals(id,signal_id,sink,state,target_key,action_json) VALUES($1,$2,'artifact','committed','typed-local','{"scope_kind":"project","scope_id":"typed-local","step":"visible procedure","reference":9007199254743001}')`, large, signal)
	exec(`INSERT INTO learning_proposals(id,signal_id,sink,state,target_key,action_json) SELECT $1::bigint+i,$2,'artifact','committed','typed-hidden-'||i,'{"scope_kind":"project","scope_id":"typed-private","step":"hidden procedure"}' FROM generate_series(1,40)i`, large, signal)
	exec(`INSERT INTO learning_proposals(id,signal_id,sink,state,target_key,action_json) VALUES($1,$2,'artifact','committed','typed-malformed','{broken')`, large+41, signal)
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	handler := NewHandler(nil, WithDataStore(PlacementKB, backend))
	args := map[string]any{"operation": "typed-context", "query": "typed-fixture", "project": "typed-local", "enable_semantic_assertions": false, "enable_episodes": true, "enable_summaries": true, "enable_working_context": true, "recent_turns": []any{"recent 界 turn", 42}, "latest_turn_at": "2026-01-03", "channel_budgets": map[string]int{"total": 4096}}
	call := func() (typedContextResult, string) {
		t.Helper()
		raw, _ := json.Marshal(args)
		payload, err := clientForHandler(t, func(invocation bus.ModuleInvocation, frame []byte) ([]byte, bus.ModuleStatus) {
			// The KB RPC host invokes these fixed-owner data operations.
			invocation.PrincipalRef = 0
			return handler(invocation, frame)
		}).Command(ctx, 73, "assemble_typed_context", raw)
		if err != nil {
			t.Fatal(err)
		}
		body := string(payload)
		var result typedContextResult
		if err := json.Unmarshal([]byte(body), &result); err != nil {
			t.Fatal(err)
		}
		return result, body
	}
	got, body := call()
	for _, name := range []string{"episodes", "summaries", "approved_procedures", "working_context"} {
		if len(got.Channels[name].Items) != 1 {
			t.Fatal(name, body)
		}
	}
	if len(got.Channels["observations"].Items) != 2 || strings.Contains(body, "hidden") || strings.Contains(body, "9999-12-31") || !strings.Contains(body, `"proposal_id":9007199254743001`) || !strings.Contains(body, `"stable_id":"9007199254743001"`) || got.RenderedTokens > 4096 || got.Sufficiency != "unknown" || got.Availability != "available" {
		t.Fatal(body)
	}
	if got.Watermark.Observations != "2026-01-02" || got.Watermark.Durable == "9999-12-31" {
		t.Fatal(got.Watermark)
	}
	// Missing host context admits only the global learning row, not local/private outputs.
	delete(args, "project")
	got, body = call()
	if !got.MissingContext || len(got.Channels["observations"].Items) != 1 || len(got.Channels["episodes"].Items) != 0 || len(got.Channels["approved_procedures"].Items) != 0 {
		t.Fatal(body)
	}
	args["project"] = "typed-local"
	args["include_all"] = true
	args["scope"] = Scope{Type: ScopeProject, Value: "typed-local"}
	got, body = call()
	if len(got.Channels["observations"].Items) != 1 || len(got.Channels["episodes"].Items) != 1 || strings.Contains(body, "hidden") || strings.Contains(body, "9999-12-31") {
		t.Fatal(body)
	}
	args["scope"] = Scope{Type: ScopeGlobal, Value: "_global"}
	got, body = call()
	if len(got.Channels["observations"].Items) != 1 || len(got.Channels["episodes"].Items) != 0 || len(got.Channels["approved_procedures"].Items) != 0 || strings.Contains(body, "hidden") {
		t.Fatal("canonical global scope omitted legacy global rows", body)
	}
	delete(args, "include_all")
	delete(args, "scope")
	// A failed channel rolls back to its savepoint without erasing other channels.
	exec(`SAVEPOINT typed_denied; RESET ROLE; REVOKE SELECT(action_json) ON learning_proposals FROM aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`)
	got, body = call()
	if got.Channels["approved_procedures"].Status != "degraded" || got.Sufficiency != "unknown" || got.Availability != "degraded" || len(got.Channels["observations"].Items) != 2 || len(got.Channels["episodes"].Items) != 1 {
		t.Fatal(body)
	}
	exec(`ROLLBACK TO SAVEPOINT typed_denied; RELEASE SAVEPOINT typed_denied`)
	exec(`SAVEPOINT typed_watermark_denied; RESET ROLE; REVOKE SELECT(refreshed_at) ON learning_observations FROM aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`)
	got, body = call()
	if got.Watermark.Status != "unavailable" || got.Sufficiency != "unknown" || got.Availability != "degraded" {
		t.Fatal(body)
	}
	exec(`ROLLBACK TO SAVEPOINT typed_watermark_denied; RELEASE SAVEPOINT typed_watermark_denied`)
	args["enabled"] = false
	got, body = call()
	if got.Enabled || got.Used != 0 || got.Sufficiency != "insufficient" {
		t.Fatal(body)
	}
	args["enabled"] = true
	args["enable_semantic_assertions"] = true
	args["valid_at"] = "2026-02-30T00:00:00Z"
	got, body = call()
	if got.Status != "error" || got.ErrorType != "invalid_timestamp" || got.Used != 0 || got.Sufficiency != "insufficient" {
		t.Fatal(body)
	}
	delete(args, "valid_at")
	args["enable_semantic_assertions"] = false
	// Each channel is optional independently of the master setting.
	for _, name := range []string{"episodes", "summaries", "observations", "approved_procedures", "working_context"} {
		args["enable_"+name] = false
	}
	got, body = call()
	if got.Used != 0 || got.Sufficiency != "insufficient" {
		t.Fatal(body)
	}
}
