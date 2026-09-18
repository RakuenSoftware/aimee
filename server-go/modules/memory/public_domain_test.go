package memory

import (
	"context"
	"encoding/json"
	"os"
	"testing"

	"github.com/jackc/pgx/v5"
)

func TestDomainPublicValidation(t *testing.T) {
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, nil)))
	for _, test := range []struct{ verb, args string }{
		{"entity_profile", `{"entity":null}`}, {"entity_edges", `{"entity":12}`},
		{"search_graph", `{}`}, {"search_graph_as_of", `{"query":"app"}`},
		{"get_episode", `{}`}, {"get_provenance", `{"memory_id":1.5}`},
		{"link_query", `{"memory_id":0}`}, {"link_create", `{"source_id":1,"target_id":1,"relation":"same"}`},
		{"link_delete", `{"link_id":-1}`},
	} {
		result := runPublicCommand(t, client, test.verb, test.args)
		if result["status"] != "error" || result["kind"] != "invalid_argument" {
			t.Fatalf("%s: %v", test.verb, result)
		}
	}
}

func TestDomainPublicScope(t *testing.T) {
	for _, test := range []struct {
		args               string
		scoped, all        bool
		workspace, project string
	}{
		{`{"include_all":false,"workspace":"ignored"}`, false, true, "", ""},
		{`{"scope_context":true,"workspace":"repo","project":"app","scope":{"type":"global"}}`, true, false, "repo", "app"},
		{`{"scope_context":true,"include_all":true}`, true, true, "", ""},
	} {
		var args commandArgs
		if err := json.Unmarshal([]byte(test.args), &args); err != nil {
			t.Fatal(err)
		}
		req := DataRequest{}
		scoped := commandScope(args, &req)
		if scoped != test.scoped || req.IncludeAll != test.all || req.Workspace != test.workspace || req.Project != test.project || req.Scope.Type != "" {
			t.Fatalf("%s: %+v scoped=%v", test.args, req, scoped)
		}
	}
}

func TestDomainPublicPostgres(t *testing.T) {
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
	_, err = tx.Exec(ctx, `CREATE SCHEMA domain_command_test;
 CREATE FUNCTION domain_command_test.pg_now_text(shift text DEFAULT '0 seconds') RETURNS text LANGUAGE sql AS $$ SELECT (now()+shift::interval)::text $$;
 SET LOCAL search_path TO pg_temp,domain_command_test,public;
 CREATE TEMP TABLE memories(id bigint PRIMARY KEY,tier text,kind text,scope_type text,scope_value text,created_at text DEFAULT pg_now_text(),last_used_at text,use_count int DEFAULT 0,confidence double precision DEFAULT 1);
 INSERT INTO memories(id,tier,kind,scope_type,scope_value) VALUES (1,'L2','fact','global','_global'),(2,'L1','episode','workspace','repo'),(3,'L2','preference','project','app');
 CREATE TEMP TABLE memory_scopes(memory_id bigint,scope_type text,scope_value text);
 INSERT INTO memory_scopes VALUES (1,'workspace','repo'),(1,'project','app');
 CREATE TEMP TABLE memory_conflicts(id bigserial PRIMARY KEY,memory_a bigint,memory_b bigint,detected_at text DEFAULT pg_now_text(),resolved int DEFAULT 0,resolution text DEFAULT '');
 INSERT INTO memory_conflicts(memory_a,memory_b) VALUES (1,2);
 CREATE TEMP TABLE memory_provenance(id bigserial PRIMARY KEY,memory_id bigint,session_id text,action text,details text,created_at text DEFAULT pg_now_text());
 INSERT INTO memory_provenance(memory_id,session_id,action,details) VALUES (1,'session','created','source detail');
 CREATE TEMP TABLE memory_links(id bigserial PRIMARY KEY,source_id bigint,target_id bigint,relation text,weight double precision DEFAULT 1,created_at text DEFAULT pg_now_text());
 CREATE TEMP TABLE memory_episodes(id bigserial PRIMARY KEY,memory_id bigint,episode_key text,episode_text text,source_session text,reference_time text,created_at text DEFAULT pg_now_text());
 INSERT INTO memory_episodes(memory_id,episode_key,episode_text,source_session,reference_time) VALUES (2,'release','release recap','session','2026-09-01');
 CREATE TEMP TABLE memory_relations(id bigserial PRIMARY KEY,memory_id bigint,episode_id bigint,src_entity text,relation text,dst_entity text,fact_text text,valid_at text,invalid_at text,weight double precision,created_at text DEFAULT pg_now_text());
 INSERT INTO memory_relations(memory_id,episode_id,src_entity,relation,dst_entity,fact_text,valid_at,invalid_at,weight) VALUES
 (1,1,'app','uses','old','app used old','2025-01-01','2026-01-01',0.9),
 (3,1,'app','uses','new','app uses new','2026-01-01','',1);
 CREATE TEMP TABLE memory_health(cycle_at text DEFAULT pg_now_text(),contradictions_detected int,promotions int,demotions int,expirations int);
 INSERT INTO memory_health(contradictions_detected,promotions,demotions,expirations) VALUES (1,2,3,4);`)
	if err != nil {
		t.Fatal(err)
	}
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, &postgresDataStore{db: evalQueryer{tx}, placement: PlacementKB})))
	run := func(verb, args string) map[string]any {
		t.Helper()
		r := runPublicCommand(t, client, verb, args)
		if r["status"] != "ok" {
			t.Fatalf("%s: %v", verb, r)
		}
		return r
	}
	graph := run("search_graph", `{"query":"app","limit":1,"operation":"delete","id":1,"scope_context":true}`)
	rows := graph["relations"].([]any)
	if len(rows) != 1 || rows[0].(map[string]any)["dst_entity"] != "new" || graph["active_context_missing"] != true {
		t.Fatal(graph)
	}
	if _, ok := rows[0].(map[string]any)["target"]; ok {
		t.Fatal("data contract leaked into public response")
	}
	past := run("search_graph_as_of", `{"query":"app","as_of":"2025-06-01"}`)["relations"].([]any)
	if len(past) != 1 || past[0].(map[string]any)["dst_entity"] != "old" {
		t.Fatal(past)
	}
	edges := run("entity_edges", `{"entity":"APP"}`)["edges"].([]any)
	if len(edges) != 2 {
		t.Fatal(edges)
	}
	if rows := run("entity_edges", `{"entity":"missing"}`)["edges"].([]any); len(rows) != 0 {
		t.Fatal(rows)
	}
	profile := run("entity_profile", `{"entity":"app"}`)["profile"].(map[string]any)
	if profile["mention_count"] != float64(2) || profile["latest_episode"] != "release" {
		t.Fatal(profile)
	}
	episode := run("get_episode", `{"episode_key":"release"}`)["episode"].(map[string]any)
	if episode["episode_text"] != "release recap" || len(episode) != 7 {
		t.Fatal(episode)
	}
	if missing := runPublicCommand(t, client, "get_episode", `{"episode_key":"missing"}`); missing["kind"] != "not_found" {
		t.Fatal(missing)
	}
	provenance := run("get_provenance", `{"memory_id":1}`)["entries"].([]any)
	if len(provenance) != 1 || provenance[0].(map[string]any)["details"] != "source detail" {
		t.Fatal(provenance)
	}
	run("link_create", `{"source_id":1,"target_id":2,"relation":"supports","authority":99}`)
	links := run("link_query", `{"memory_id":2}`)["links"].([]any)
	if len(links) != 1 || len(links[0].(map[string]any)) != 5 {
		t.Fatal(links)
	}
	linkID, _ := json.Marshal(map[string]any{"link_id": links[0].(map[string]any)["id"]})
	run("link_delete", string(linkID))
	run("link_delete", string(linkID)) // old API treats an absent link as a successful no-op
	if rows := run("link_query", `{"memory_id":2}`)["links"].([]any); len(rows) != 0 {
		t.Fatal(rows)
	}
	conflicts := run("list_conflicts", `{}`)["conflicts"].([]any)
	if len(conflicts) != 1 || conflicts[0].(map[string]any)["memory_a"] != float64(1) || conflicts[0].(map[string]any)["resolved"] != float64(0) {
		t.Fatal(conflicts)
	}
	stats := run("stats", `{}`)["stats"].(map[string]any)
	if stats["total"] != float64(3) || stats["tier_counts"].([]any)[2] != float64(2) || len(stats["kind_counts"].([]any)) != 10 {
		t.Fatal(stats)
	}
	health := run("query_health", `{}`)["health"].(map[string]any)
	if health["cycles"] != float64(1) || health["total_promotions"] != float64(2) || health["write_to_readable_lag"].(map[string]any)["state"] != "unmeasured" {
		t.Fatal(health)
	}
	// Exercise the historical 256-row public cap (the data transport used to reject it).
	_, err = tx.Exec(ctx, `INSERT INTO memory_conflicts(memory_a,memory_b) SELECT 1,2 FROM generate_series(1,299)`)
	if err != nil {
		t.Fatal(err)
	}
	if rows := run("list_conflicts", `{"max":256}`)["conflicts"].([]any); len(rows) != 256 {
		t.Fatal(len(rows))
	}
	dashboard := run("stats_dashboard", `{}`)["dashboard"].(map[string]any)
	scopes := dashboard["scopes"].([]any)
	if len(dashboard["tiers"].([]any)) != 6 || len(dashboard["tier_kinds"].([]any)) != 3 || len(scopes) != 3 {
		t.Fatal(dashboard)
	}
	if scopes[0].(map[string]any)["count"] != float64(0) || scopes[1].(map[string]any)["count"] != float64(1) || scopes[2].(map[string]any)["count"] != float64(2) || scopes[2].(map[string]any)["conflicted_memories"] != float64(300) {
		t.Fatal(scopes)
	}
	// Derived rows have no RLS of their own. Query through the parent memory
	// policy, with a real non-owner connection and transaction-local scope.
	_, err = tx.Exec(ctx, `CREATE ROLE memory_domain_test NOINHERIT NOBYPASSRLS;
GRANT USAGE ON SCHEMA domain_command_test TO memory_domain_test;
GRANT SELECT ON memories,memory_scopes,memory_conflicts,memory_relations,memory_episodes,memory_provenance,memory_links TO memory_domain_test;
GRANT INSERT,DELETE ON memory_links TO memory_domain_test;
GRANT USAGE,SELECT ON SEQUENCE memory_links_id_seq TO memory_domain_test;
UPDATE memories SET scope_type='project',scope_value='app' WHERE id=2;
INSERT INTO memories(id,tier,kind,scope_type,scope_value) VALUES (4,'L2','fact','project','private');
INSERT INTO memory_relations(memory_id,episode_id,src_entity,relation,dst_entity,fact_text,valid_at,invalid_at,weight)
VALUES (4,1,'app','uses','secret','private graph detail','','',2);
INSERT INTO memory_provenance(memory_id,session_id,action,details) VALUES (4,'private','created','secret');
INSERT INTO memory_links(source_id,target_id,relation) VALUES (1,4,'secret');
INSERT INTO memory_conflicts(memory_a,memory_b) VALUES (1,4);
ALTER TABLE memories ENABLE ROW LEVEL SECURITY;
CREATE POLICY test_memory_visibility ON memories USING
 (scope_type='global' OR current_setting('aimee.memory_scope_all',true)='1' OR
  (scope_type=current_setting('aimee.memory_scope_type',true) AND scope_value=current_setting('aimee.memory_scope_value',true)));
SET LOCAL ROLE memory_domain_test;`)
	if err != nil {
		t.Fatal(err)
	}
	client = clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, &postgresDataStore{db: runtimeRoleDB{evalQueryer{tx}, t}, placement: PlacementKB})))
	for _, verb := range []string{"entity_edges", "search_graph", "search_graph_as_of"} {
		result := run(verb, `{"entity":"app","query":"app","as_of":"2026-09-01","scope_context":true,"project":"app"}`)
		key := "relations"
		if verb == "entity_edges" {
			key = "edges"
		}
		for _, row := range result[key].([]any) {
			if row.(map[string]any)["dst_entity"] == "secret" {
				t.Fatalf("%s leaked private graph: %v", verb, result)
			}
		}
	}
	scopedProfile := run("entity_profile", `{"entity":"app","scope_context":true,"project":"app"}`)["profile"].(map[string]any)
	if scopedProfile["mention_count"] != float64(2) {
		t.Fatal(scopedProfile)
	}
	privateProfile := run("entity_profile", `{"entity":"app","scope_context":true,"project":"private"}`)["profile"].(map[string]any)
	if privateProfile["mention_count"] != float64(2) || privateProfile["latest_episode"] != "" {
		t.Fatal(privateProfile)
	}
	if got := runPublicCommand(t, client, "get_episode", `{"episode_key":"release","scope_context":true,"project":"private"}`); got["kind"] != "not_found" {
		t.Fatalf("public episode lookup leaked across project scope: %v", got)
	}
	if got := run("get_episode", `{"episode_key":"release","scope_context":true,"project":"app"}`); got["episode"] == nil {
		t.Fatalf("public scoped episode lookup lost visible result: %v", got)
	}
	for _, req := range []DataRequest{
		{Operation: "provenance-list", ID: 4, Project: "app"},
		{Operation: "link-query", ID: 1, Project: "app"},
		{Operation: "episode-get", Key: "release", Project: "private"},
	} {
		response, err := client.Data(ctx, 73, req)
		if err != nil {
			t.Fatal(err)
		}
		if len(response.Provenance) != 0 || len(response.Links) != 0 || len(response.Episodes) != 0 {
			t.Fatalf("scoped %s leaked: %+v", req.Operation, response)
		}
	}

	// Each handler call is a savepoint inside this rollback-only fixture.
	// SET LOCAL survives RELEASE SAVEPOINT, so connection reset is observed
	// at the enclosing transaction boundary (as with production pool release).
	if err := tx.Rollback(ctx); err != nil {
		t.Fatal(err)
	}
	var retained string
	if err := conn.QueryRow(ctx, `SELECT COALESCE(current_setting('aimee.memory_scope_value',true),'')`).Scan(&retained); err != nil || retained != "" {
		t.Fatalf("scope retained after transaction: %q %v", retained, err)
	}
}
