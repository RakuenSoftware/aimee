package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
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
 CREATE TEMP TABLE memories(id bigint PRIMARY KEY,record_revision bigint DEFAULT 1,tier text,kind text,scope_type text,scope_value text,lifecycle_state text DEFAULT 'active',activation_suppressed int DEFAULT 0,valid_from text DEFAULT '',valid_until text DEFAULT '',created_at text DEFAULT pg_now_text(),last_used_at text,use_count int DEFAULT 0,confidence double precision DEFAULT 1,effectiveness double precision DEFAULT 0.2);
 INSERT INTO memories(id,tier,kind,scope_type,scope_value) VALUES (1,'L2','fact','global','_global'),(2,'L1','episode','workspace','repo'),(3,'L2','preference','project','app');
 CREATE TEMP TABLE memory_scopes(memory_id bigint,scope_type text,scope_value text);
 INSERT INTO memory_scopes VALUES (1,'workspace','repo'),(1,'project','app');
 CREATE TEMP TABLE memory_entities(memory_id bigint,entity text,role text DEFAULT 'mention');
 INSERT INTO memory_entities(memory_id,entity) VALUES (1,'app'),(3,'app');
 CREATE TEMP TABLE entity_edges(id bigint,source text,target text,edge_class text,lifecycle_state text,suppressed int,superseded_at text,invalidated_at text,valid_from text,valid_until text);
 CREATE TEMP TABLE fact_evidence(assertion_id bigint,source_kind text,source_id text,invalidated_at text,stance text);
 CREATE TEMP TABLE memory_conflicts(id bigserial PRIMARY KEY,memory_a bigint,memory_b bigint,detected_at text DEFAULT pg_now_text(),resolved int DEFAULT 0,resolution text DEFAULT '');
 INSERT INTO memory_conflicts(memory_a,memory_b) VALUES (1,2);
 CREATE TEMP TABLE memory_provenance(id bigserial PRIMARY KEY,memory_id bigint,session_id text,action text,details text,created_at text DEFAULT pg_now_text());
 INSERT INTO memory_provenance(memory_id,session_id,action,details) VALUES (1,'session','created','source detail');
 CREATE TEMP TABLE memory_links(id bigserial PRIMARY KEY,source_id bigint,target_id bigint,relation text,weight double precision DEFAULT 1,created_at text DEFAULT pg_now_text());
 CREATE TEMP TABLE memory_episodes(id bigserial PRIMARY KEY,memory_id bigint,episode_key text,episode_text text,source_session text,reference_time text,created_at text DEFAULT pg_now_text());
 INSERT INTO memory_episodes(memory_id,episode_key,episode_text,source_session,reference_time) VALUES (2,'release','release recap','session','2026-09-01');
 CREATE TEMP TABLE memory_lineage(object_type text,object_id bigint,source_kind text,source_ref text);
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
	if _, err := tx.Exec(ctx, `INSERT INTO memory_provenance(id,memory_id,session_id,action,details) VALUES (9007199254740993,1,'session','created','exact')`); err != nil {
		t.Fatal(err)
	}
	mcpProvenance := run("get_provenance", `{"memory_id":"1","format":"mcp"}`)["output"].(string)
	if !strings.Contains(mcpProvenance, "9007199254740993") {
		t.Fatal(mcpProvenance)
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
	console := run("stats", `{"view":"console","effectiveness":true,"operation":"delete","id":1}`)
	display := console["display"].(map[string]any)
	if display["total"] != float64(3) || display["tiers"].(map[string]any)["L2"] != float64(2) ||
		display["effectiveness"].(map[string]any)["low_effectiveness"] != float64(3) ||
		display["effectiveness"].(map[string]any)["never_surfaced_l2"] != float64(2) {
		t.Fatal(console)
	}
	timing := pageRankMetricState.snapshot()
	if !strings.Contains(console["text"].(string), "L0=0 L1=1 L2=2 L3=0 L4=0 L5=0\n") ||
		console["pagerank_timing"].(map[string]any)["elapsed_ms"] != timing.LastMS ||
		console["pagerank_text"] != fmt.Sprintf("PageRank: elapsed=%.3fms avg=%.3fms max=%.3fms samples=%d candidates=%d edges=%d\n", timing.LastMS, timing.AverageMS, timing.MaximumMS, timing.Samples, timing.Candidates, timing.Edges) {
		t.Fatal(console)
	}
	if _, exists := run("stats", `{"view":"console"}`)["display"].(map[string]any)["effectiveness"]; exists {
		t.Fatal("human view unexpectedly requested effectiveness")
	}
	if _, exists := run("stats", `{}`)["display"]; exists {
		t.Fatal("console fields leaked into ordinary stats")
	}
	consoleHealth := run("query_health", `{"view":"console"}`)
	if !strings.Contains(consoleHealth["text"].(string), "Memory Health (last 7 days, 1 cycles):\n") ||
		!strings.Contains(consoleHealth["text"].(string), "  Write-to-readable:  unmeasured\n  Expirations:        4\n") ||
		consoleHealth["health"].(map[string]any)["total_promotions"] != float64(2) {
		t.Fatal(consoleHealth)
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
	t.Run("link console", func(t *testing.T) {
		_, err := tx.Exec(ctx, `INSERT INTO memories(id,tier,kind,scope_type,scope_value) VALUES
   (9007199254740993,'L2','fact','global','_global'),
   (9223372036854775807,'L2','fact','global','_global');
   ALTER SEQUENCE memory_links_id_seq RESTART WITH 9007199254740993`)
		if err != nil {
			t.Fatal(err)
		}
		create := `{"view":"console","source_id":"9007199254740993","target_id":"9223372036854775807","relation":"related_to","format":"text"}`
		if got := run("link_create", create)["output"]; got != "Linked memory 9007199254740993 -[related_to]-> 9223372036854775807\n" {
			t.Fatal(got)
		}
		for _, test := range []struct{ id, direction, other string }{
			{"9007199254740993", "->", "9223372036854775807"},
			{"9223372036854775807", "<-", "9007199254740993"},
		} {
			got := run("link_query", `{"view":"console","memory_id":"`+test.id+`","format":"text"}`)["output"].(string)
			if !strings.Contains(got, "[9007199254740993] "+test.direction+" [related_to] "+test.other+"  (") {
				t.Fatal(got)
			}
		}
		output := run("link_query", `{"view":"console","memory_id":"9007199254740993"}`)["output"].(string)
		var links []map[string]json.RawMessage
		if err := json.Unmarshal([]byte(output), &links); err != nil || len(links) != 1 || len(links[0]) != 5 ||
			string(links[0]["id"]) != "9007199254740993" || string(links[0]["source_id"]) != "9007199254740993" || string(links[0]["target_id"]) != "9223372036854775807" {
			t.Fatal(output, err)
		}
		for _, test := range []struct{ verb, args string }{
			{"link_create", strings.Replace(create, `"text"`, `"invalid"`, 1)},
			{"link_create", strings.Replace(create, "related_to", "fixes", 1)},
			{"link_delete", `{"view":"console","link_id":"9007199254740993","format":"invalid"}`},
		} {
			result := runPublicCommand(t, client, test.verb, test.args)
			if result["kind"] != "invalid_argument" || result["output"] != nil {
				t.Fatal(result)
			}
		}
		for _, id := range []string{`"0"`, `"-1"`, `"01"`, `"+1"`, `" 1"`, `"1x"`, `"9223372036854775808"`, `9007199254740993`, `1.5`, `null`} {
			for _, verb := range []string{"link_create", "link_query", "link_delete"} {
				args := `{"view":"console","source_id":` + id + `,"target_id":"9223372036854775807","relation":"related_to","memory_id":` + id + `,"link_id":` + id + `}`
				result := runPublicCommand(t, client, verb, args)
				if result["kind"] != "invalid_argument" {
					t.Fatalf("%s %s: %v", verb, id, result)
				}
			}
		}
		var count int
		if err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_links WHERE source_id=9007199254740993`).Scan(&count); err != nil || count != 1 {
			t.Fatal(count, err)
		}
		run("link_create", `{"source_id":"9223372036854775807","target_id":"9007199254740993","relation":"fixes"}`)
		for _, relation := range []string{"supersedes", "depends_on", "contradicts"} {
			run("link_create", strings.Replace(create, "related_to", relation, 1))
		}
		for i := 0; i < 2; i++ {
			if got := run("link_delete", `{"view":"console","link_id":"9007199254740993","format":"text"}`)["output"]; got != "Deleted link 9007199254740993\n" {
				t.Fatal(got)
			}
		}
		if _, err := tx.Exec(ctx, `DELETE FROM memory_links WHERE source_id>=9007199254740993 OR target_id>=9007199254740993; DELETE FROM memories WHERE id>=9007199254740993`); err != nil {
			t.Fatal(err)
		}
		if got := run("link_query", `{"view":"console","memory_id":"9007199254740993"}`)["output"]; got != "[]\n" {
			t.Fatal(got)
		}
		if got := run("link_query", `{"view":"console","memory_id":"9007199254740993","format":"text"}`)["output"]; got != "No links for memory 9007199254740993\n" {
			t.Fatal(got)
		}
	})

	// Derived rows have no RLS of their own. Query through the parent memory
	// policy, with a real non-owner connection and transaction-local scope.
	_, err = tx.Exec(ctx, `CREATE ROLE memory_domain_test NOINHERIT NOBYPASSRLS;
GRANT USAGE ON SCHEMA domain_command_test TO memory_domain_test;
GRANT SELECT ON memory_lineage,memories,memory_scopes,memory_conflicts,memory_relations,memory_episodes,memory_provenance,memory_links,memory_entities,entity_edges,fact_evidence TO memory_domain_test;
GRANT INSERT,DELETE ON memory_links TO memory_domain_test;
GRANT USAGE,SELECT ON SEQUENCE memory_links_id_seq TO memory_domain_test;
UPDATE memories SET scope_type='project',scope_value='app' WHERE id=2;
INSERT INTO memories(id,tier,kind,scope_type,scope_value) VALUES (4,'L2','fact','project','private');
INSERT INTO memory_relations(memory_id,episode_id,src_entity,relation,dst_entity,fact_text,valid_at,invalid_at,weight)
VALUES (4,1,'app','uses','secret','private graph detail','','',2);
INSERT INTO memory_provenance(memory_id,session_id,action,details) VALUES (4,'private','created','secret');
INSERT INTO memory_links(source_id,target_id,relation) VALUES (1,4,'secret');
INSERT INTO memory_conflicts(memory_a,memory_b) VALUES (1,4);
INSERT INTO memories(id,tier,kind,scope_type,scope_value,lifecycle_state,activation_suppressed) VALUES
 (5,'L2','fact','project','app','archived',0),(6,'L2','fact','project','app','active',1);
INSERT INTO memory_entities(memory_id,entity,role) VALUES (5,'hidden-entity','mention'),(6,'hidden-entity','mention'),(3,'app','actor');
INSERT INTO memory_relations(memory_id,episode_id,src_entity,relation,dst_entity,fact_text,valid_at,invalid_at,weight) VALUES
 (5,1,'hidden-entity','uses','archived','hidden archived','','',100),
 (6,1,'hidden-entity','uses','suppressed','hidden suppressed','','',100),
 (1,1,'rank-entity','uses','global','global high weight','','',100),
 (3,1,'rank-entity','uses','local','local low weight','','',0.1);
INSERT INTO memory_episodes(memory_id,episode_key,episode_text,source_session,reference_time) VALUES
 (5,'hidden-episode-archived','hidden','','2099'),(6,'hidden-episode-suppressed','hidden','','2099'),
 (1,'rank-episode-global','rank-episode','','2099'),(3,'rank-episode-local','rank-episode','','2020');
INSERT INTO entity_edges(id,source,target,edge_class,lifecycle_state,suppressed,superseded_at,invalidated_at,valid_from,valid_until) VALUES
 (1,'typed-only','one','semantic','persistent',0,'','','',''),
 (2,'typed-only','two','semantic','promoted',0,'','','',''),
 (3,'typed-only','candidate','semantic','candidate',0,'','','',''),
 (4,'typed-only','suppressed','semantic','persistent',1,'','','',''),
 (5,'typed-only','invalid','semantic','persistent',0,'','2026','',''),
 (6,'typed-only','superseded','semantic','persistent',0,'2026','','',''),
 (7,'typed-only','private','semantic','persistent',0,'','','',''),
 (8,'typed-only','future','semantic','persistent',0,'','','2999-01-01T00:00:00Z',''),
 (9,'typed-only','cooccur','cooccurrence','persistent',0,'','','',''),
 (10,'typed-only','expired','semantic','persistent',0,'','','','2000-01-01T00:00:00Z'),
 (11,'typed-only','retired-memory','semantic','persistent',0,'','','','');
INSERT INTO fact_evidence(assertion_id,source_kind,source_id,invalidated_at,stance) VALUES
 (2,'memory','memory:3','','supports'),(7,'memory','memory:4','','supports'),(11,'memory','memory:5','','supports');
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
	if privateProfile["mention_count"] != float64(1) || privateProfile["latest_episode"] != "" {
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

	for _, verb := range []string{"search_graph", "search_graph_as_of", "entity_edges"} {
		key := "relations"
		if verb == "entity_edges" {
			key = "edges"
		}
		got := run(verb, `{"query":"hidden-entity","entity":"hidden-entity","as_of":"2026-09-01","scope_context":true,"project":"app"}`)
		if len(got[key].([]any)) != 0 {
			t.Fatalf("%s recalled retired parents: %v", verb, got)
		}
		got = run(verb, `{"query":"rank-entity","entity":"rank-entity","as_of":"2026-09-01","limit":1,"scope_context":true,"project":"app"}`)
		if rows := got[key].([]any); len(rows) != 1 || rows[0].(map[string]any)["dst_entity"] != "local" {
			t.Fatalf("%s lost local-first cap: %v", verb, got)
		}
	}
	for _, entity := range []string{"hidden-entity", "missing-entity"} {
		if got := runPublicCommand(t, client, "entity_profile", `{"entity":"`+entity+`","scope_context":true,"project":"app"}`); got["kind"] != "not_found" {
			t.Fatal(got)
		}
	}
	for _, key := range []string{"hidden-episode-archived", "hidden-episode-suppressed"} {
		if got := runPublicCommand(t, client, "get_episode", `{"episode_key":"`+key+`","scope_context":true,"project":"app"}`); got["kind"] != "not_found" {
			t.Fatal(got)
		}
	}
	profile = run("entity_profile", `{"entity":"typed-only","scope_context":true,"project":"app"}`)["profile"].(map[string]any)
	if profile["mention_count"] != float64(0) || profile["relation_count"] != float64(2) {
		t.Fatalf("typed-only profile currency/scope: %v", profile)
	}
	if _, err := tx.Exec(ctx, `SAVEPOINT profile_mixed_evidence; RESET ROLE;
 INSERT INTO fact_evidence(assertion_id,source_kind,source_id,invalidated_at,stance)
 VALUES(2,'memory','memory:4','','supports'); SET LOCAL ROLE memory_domain_test`); err != nil {
		t.Fatal(err)
	}
	profile = run("entity_profile", `{"entity":"typed-only","scope_context":true,"project":"app"}`)["profile"].(map[string]any)
	if profile["relation_count"] != float64(1) {
		t.Fatal("visible source admitted hidden profile evidence", profile)
	}
	if _, err := tx.Exec(ctx, `ROLLBACK TO SAVEPOINT profile_mixed_evidence; RELEASE SAVEPOINT profile_mixed_evidence`); err != nil {
		t.Fatal(err)
	}
	profile = run("entity_profile", `{"entity":"rank-entity","scope_context":true,"project":"app"}`)["profile"].(map[string]any)
	if profile["summary"] != "local low weight" || profile["relation_count"] != float64(2) {
		t.Fatal(profile)
	}
	for _, operation := range []string{"episode-list", "entity-profile"} {
		handler := NewHandler(nil, WithDataStore(PlacementKB, &postgresDataStore{db: runtimeRoleDB{evalQueryer{tx}, t}, placement: PlacementKB}))
		args := `{"operation":"` + operation + `","entity":"typed-only","query":"rank-episode","limit":1,"scope_context":true,"project":"app"}`
		got, status := invokeContextCommand(t, handler, 0, bus.CommandContext{}, "runtime", args)
		if status != bus.ModuleStatusOK || got["status"] != "ok" {
			t.Fatal(got, status)
		}
		if _, status := invokeContextCommand(t, handler, 200, bus.CommandContext{}, "runtime", args); status != bus.ModuleStatusInvalidRequest {
			t.Fatal(status)
		}
		if operation == "episode-list" {
			if rows := got["episodes"].([]any); len(rows) != 1 || rows[0].(map[string]any)["episode_key"] != "rank-episode-local" {
				t.Fatal(got)
			}
		} else if got["profile"].(map[string]any)["relation_count"] != float64(2) {
			t.Fatal(got)
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

func TestStatisticsConsoleUnavailable(t *testing.T) {
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, nil)))
	for _, verb := range []string{"stats", "query_health", "link_query", "link_create", "link_delete"} {
		body, err := client.Command(context.Background(), 73, verb, json.RawMessage(`{"view":"console","effectiveness":true,"memory_id":"1","source_id":"1","target_id":"2","link_id":"1","relation":"related_to"}`))
		if err == nil || len(body) != 0 {
			t.Fatalf("unavailable %s looked healthy: %s, %v", verb, body, err)
		}
	}
}
