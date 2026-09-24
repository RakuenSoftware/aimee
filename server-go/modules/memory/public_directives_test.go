package memory

import (
	"context"
	"encoding/json"
	"os"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5"
)

func TestDirectivePublicPostgresLifecycle(t *testing.T) {
	dsn := os.Getenv("AIMEE_MEMORY_EVAL_URL")
	if dsn == "" {
		t.Skip("set AIMEE_MEMORY_EVAL_URL for PostgreSQL command regression")
	}
	ctx := context.Background()
	conn, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close(ctx)
	tx, err := conn.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	_, err = tx.Exec(ctx, `CREATE SCHEMA directive_test;
 CREATE FUNCTION directive_test.pg_now_text() RETURNS text LANGUAGE sql AS $$ SELECT now()::text $$;
 SET LOCAL search_path TO pg_temp,directive_test,public;
 CREATE TEMP TABLE memories(id bigint PRIMARY KEY,record_revision bigint DEFAULT 1,
 lifecycle_state text DEFAULT 'active',activation_suppressed bigint DEFAULT 0,valid_from text DEFAULT '',valid_until text DEFAULT '');
 CREATE TEMP TABLE memory_units(id bigint,memory_id bigint,unit_type text,unit_key text,unit_text text,memory_kind text,weight float8,is_episode_card int);
 CREATE TEMP TABLE memory_lineage(object_type text,object_id bigint,source_kind text,source_ref text);
 CREATE TEMP TABLE memory_collection_owner(id int,owner_id uuid);
 INSERT INTO memory_collection_owner VALUES(1,'00000000-0000-4000-8000-000000000001');
 CREATE TEMP TABLE epistemic_directives(id bigserial PRIMARY KEY,record_revision bigint NOT NULL DEFAULT 1, question text, topic text DEFAULT '',
 anchor_entity text DEFAULT '',anchor_file text DEFAULT '',cause text,priority bigint DEFAULT 50,
 state text DEFAULT 'open',memory_a_id bigint DEFAULT 0,memory_b_id bigint DEFAULT 0,
 resolution_memory_id bigint DEFAULT 0,evidence text DEFAULT '',source_session text DEFAULT '',
 surfaced_count bigint DEFAULT 0,last_surfaced_at text DEFAULT '',resolved_at text DEFAULT '',
 valid_until text DEFAULT '',created_at text DEFAULT pg_now_text(),updated_at text DEFAULT pg_now_text());
 CREATE UNIQUE INDEX ON epistemic_directives(cause,topic) WHERE topic<>'' AND cause IN ('retrieval_failure','missing_config');
 CREATE UNIQUE INDEX ON epistemic_directives(memory_a_id,memory_b_id) WHERE cause='contradiction' AND memory_a_id<>0 AND memory_b_id<>0;`)
	if err != nil {
		t.Fatal(err)
	}
	s := &postgresDataStore{db: evalQueryer{tx}, placement: PlacementKB}
	handler := NewHandler(nil, WithDataStore(PlacementKB, s))
	exerciseDeadlineReplay(t, ctx, tx, handler, "directive")
	client := clientForHandler(t, handler)
	for _, verb := range []string{"directive_list", "directive_briefing", "directive_dashboard"} {
		if result := runPublicCommand(t, client, verb, `{}`); result["status"] != "ok" {
			t.Fatal(result)
		}
	}
	create := func(args string) map[string]any {
		result := runPublicCommand(t, client, "directive_create", args)
		if result["status"] != "ok" || result["dedup"] != false {
			t.Fatal(result)
		}
		row, ok := result["directive"].(map[string]any)
		if !ok || len(row) != 17 {
			t.Fatal("public directive envelope changed", result)
		}
		if _, exists := row["source_session"]; exists {
			t.Fatal("internal session leaked")
		}
		return row
	}
	if result := runPublicCommand(t, client, "directive_list", `{"state":"bogus"}`); result["status"] != "ok" || len(result["directives"].([]any)) != 0 {
		t.Fatal(result)
	}
	before := directiveMetrics().Created
	topic := create(`{"question":"Which backend?", "topic":"db", "cause":"missing_config","priority":0,"session":"s1","memory_a_id":99,"evidence":"untrusted"}`)
	if topic["priority"] != float64(0) || topic["memory_a_id"] != float64(0) || topic["evidence"] != "" {
		t.Fatal("defaults/selected fields lost", topic)
	}
	duplicate := runPublicCommand(t, client, "directive_create", `{"question":"Again?","topic":"db","cause":"missing_config"}`)
	if duplicate["dedup"] != true || duplicate["directive"] != nil || directiveMetrics().Created != before+1 {
		t.Fatal("dedup changed records or inflated telemetry", duplicate)
	}
	follow := create(`{"question":"Review design?"}`)
	if follow["priority"] != float64(50) || follow["cause"] != "user_follow_up" {
		t.Fatal(follow)
	}
	high := create(`{"question":"Urgent?","priority":999}`)
	if high["priority"] != float64(100) {
		t.Fatal(high)
	}
	block := runHostRuntime(t, handler, `{"operation":"directive-briefing","limit":1}`)["block"].(string)
	if block != "# Open Questions\n- [p100 · user_follow_up] Urgent?\n\n" {
		t.Fatal(block)
	}
	args, _ := json.Marshal(map[string]any{"id": topic["id"], "with_memory": 42, "note": "must not overwrite evidence"})
	if result := runPublicCommand(t, client, "directive_resolve", string(args)); result["id"] != topic["id"] {
		t.Fatal(result)
	}
	if result := runPublicCommand(t, client, "directive_resolve", string(args)); result["status"] != "error" {
		t.Fatal("resolved twice", result)
	}
	resolved, err := s.DirectiveGet(ctx, int64(topic["id"].(float64)))
	if err != nil || resolved.ResolutionMemoryID != 42 || resolved.Evidence != "" || resolved.SourceSession != "s1" {
		t.Fatalf("resolved=%+v %v", resolved, err)
	}
	args, _ = json.Marshal(map[string]any{"id": high["id"]})
	if result := runPublicCommand(t, client, "directive_suppress", string(args)); result["id"] != high["id"] {
		t.Fatal(result)
	}
	if result := runPublicCommand(t, client, "directive_resolve", string(args)); result["status"] != "error" {
		t.Fatal("suppressed row resolved", result)
	}
	create(`{"question":"Expired?","valid_until":"2000-01-01"}`)
	if result := runPublicCommand(t, client, "directive_sweep_expired", `{}`); result["expired"] != float64(1) {
		t.Fatal(result)
	}
	dashboard := runHostRuntime(t, handler, `{"operation":"directive-dashboard"}`)["dashboard"].(map[string]any)
	counts := dashboard["counts"].(map[string]any)
	if counts["open"] != float64(1) || counts["total"] != float64(4) || counts["suppressed"] != float64(1) || counts["resolved"] != float64(1) || counts["expired"] != float64(1) {
		t.Fatal(dashboard)
	}
	block = runHostRuntime(t, handler, `{"operation":"directive-briefing"}`)["block"].(string)
	if strings.Contains(block, "Urgent?") || !strings.Contains(block, "Review design?") {
		t.Fatal(block)
	}
	_, err = tx.Exec(ctx, `INSERT INTO epistemic_directives(question,cause) SELECT 'extra','user_follow_up' FROM generate_series(1,300)`)
	if err != nil {
		t.Fatal(err)
	}
	list := runPublicCommand(t, client, "directive_list", `{"limit":999}`)
	if len(list["directives"].([]any)) != 256 {
		t.Fatal("list cap changed")
	}
	promoted := runHostRuntime(t, NewHandler(nil, WithDataStore(PlacementKB, s)), `{"operation":"directive-create","question":"Promoted finding?","cause":"promoted_directive","evidence":"artifact:123","priority":1}`)["directive"].(map[string]any)
	if promoted["cause"] != "promoted_directive" || promoted["evidence"] != "artifact:123" {
		t.Fatal(promoted)
	}

}

func TestDirectivePublicValidation(t *testing.T) {
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, nil)))
	for _, test := range []struct{ verb, args string }{
		{"directive_create", `{}`}, {"directive_create", `{"question":"x","cause":"bogus"}`},
		{"directive_resolve", `{"id":-1}`}, {"directive_suppress", `{"id":1.5}`},
	} {
		if result := runPublicCommand(t, client, test.verb, test.args); result["kind"] != "invalid_argument" {
			t.Fatal(result)
		}
	}
}

// Referenced memories remain mandatory inputs even when the directive itself
// is open and unexpired. Exercise the packaged non-owner runtime and pre-limit
// backfill independently of directive expiry and ranking.
func TestDirectiveParentEligibilityPostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_DB2_REPLAY_URL")
	if dsn == "" {
		t.Skip("set AIMEE_DB2_REPLAY_URL for packaged directive parent replay")
	}
	ctx := context.Background()
	conn, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close(ctx)
	tx, err := conn.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	exec := func(query string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, query, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`DO $$ BEGIN
 IF NOT EXISTS(SELECT 1 FROM pg_roles WHERE rolname='aimee_store_runtime') THEN
 CREATE ROLE aimee_store_runtime NOINHERIT NOBYPASSRLS;
 END IF; END $$;
 GRANT USAGE ON SCHEMA public TO aimee_store_runtime;
 GRANT SELECT ON memories,memory_scopes,memory_units,memory_lineage,memory_collection_owner,epistemic_directives TO aimee_store_runtime;
 UPDATE epistemic_directives SET state='suppressed';
 SELECT set_config('aimee.memory_scope_all','1',true)`)
	const project = "directive-parent-eligibility"
	var wanted, currentParent, foreignParent int64
	for _, state := range []string{"current", "future", "expired", "suppressed", "superseded", "archived", "quarantined", "deleted", "revoked", "cross-scope"} {
		lifecycle, scope, suppressed, priority := "active", project, 0, 100
		switch state {
		case "superseded", "archived", "quarantined", "deleted", "revoked":
			lifecycle = state
		case "cross-scope":
			scope = project + "-foreign"
		case "suppressed":
			suppressed = 1
		case "current":
			priority = 1
		}
		var parent, id int64
		if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value,lifecycle_state,activation_suppressed,valid_from,valid_until)
 VALUES('L2','fact',$1,'directive parent','project',$2,$3,$4,
 CASE WHEN $5='future' THEN (CURRENT_TIMESTAMP+interval '1 day')::text ELSE '' END,
 CASE WHEN $5='expired' THEN CURRENT_TIMESTAMP::text ELSE '' END) RETURNING id`, project+"-"+state, scope, lifecycle, suppressed, state).Scan(&parent); err != nil {
			t.Fatal(err)
		}
		if err := tx.QueryRow(ctx, `INSERT INTO epistemic_directives(question,topic,cause,priority,memory_a_id)
 VALUES($1,$2,'user_follow_up',$3,$4) RETURNING id`, project+" "+state, project, priority, parent).Scan(&id); err != nil {
			t.Fatal(err)
		}
		if state == "current" {
			wanted, currentParent = id, parent
		}
		if state == "cross-scope" {
			foreignParent = parent
		}
	}
	// Each parent position is mandatory, including mixed authorized/foreign inputs.
	exec(`INSERT INTO epistemic_directives(question,topic,cause,priority,memory_a_id,memory_b_id,resolution_memory_id)
 VALUES('foreign second parent',$1,'user_follow_up',100,$2,$3,0),
 ('foreign resolution parent',$1,'user_follow_up',100,$2,0,$3),
 ('missing parent',$1,'user_follow_up',100,-1,0,0),
 ('authored without parents',$1,'user_follow_up',0,0,0,0)`, project, currentParent, foreignParent)
	exec(`SELECT set_config('aimee.memory_scope_all','0',true),set_config('aimee.memory_project',$1,true),
 set_config('aimee.memory_scope_type','project',true),set_config('aimee.memory_scope_value',$1,true)`, project)
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	backend := &postgresDataStore{db: runtimeRoleDB{evalQueryer{tx}, t}, placement: PlacementKB}
	for name, query := range map[string]func() ([]Directive, error){
		"matched": func() ([]Directive, error) { return backend.DirectiveMatch(ctx, project, "", "", 1) },
		"recall":  func() ([]Directive, error) { return backend.recallOpenDirectives(ctx, 1) },
	} {
		t.Run(name, func(t *testing.T) {
			rows, err := query()
			if err != nil || len(rows) != 1 || rows[0].ID != wanted {
				t.Error("ineligible directive parent survived before limit", rows, wanted, err)
			}
		})
	}
	rows, err := backend.DirectiveMatch(ctx, project, "", "", 100)
	if err != nil || len(rows) != 2 || rows[0].ID != wanted || rows[1].Question != "authored without parents" {
		t.Error("parentless authoring compatibility or all-parent eligibility changed", rows, err)
	}

}
