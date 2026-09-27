package memory

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5"
)

func TestArchiveCommandsPostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_MEMORY_EVAL_URL")
	if dsn == "" {
		t.Skip("set AIMEE_MEMORY_EVAL_URL for PostgreSQL archive commands")
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
	_, err = tx.Exec(ctx, `CREATE SCHEMA archive_command_test;
CREATE FUNCTION archive_command_test.pg_now_text() RETURNS text LANGUAGE sql AS $$ SELECT now()::text $$;
SET LOCAL search_path TO pg_temp,archive_command_test,public;
CREATE TEMP TABLE memories(id bigserial PRIMARY KEY,record_revision bigint NOT NULL DEFAULT 1,key text,content text,tier text DEFAULT 'L2',kind text DEFAULT 'fact',epistemic_kind text DEFAULT 'world_fact',
 scope_type text DEFAULT 'global',scope_value text DEFAULT '_global',confidence double precision DEFAULT 0.8,use_count int DEFAULT 0,
 source_session text DEFAULT 'session',provenance_category text DEFAULT '',artifact_ref text DEFAULT '',lifecycle_state text DEFAULT 'active',created_at text DEFAULT pg_now_text(),updated_at text DEFAULT pg_now_text(),UNIQUE(kind,key,scope_type,scope_value),valid_from text DEFAULT '',valid_until text DEFAULT '',activation_suppressed int DEFAULT 0,activation_sticky_turns bigint DEFAULT 0,activation_cooldown_turns bigint DEFAULT 0,activation_delay_turns bigint DEFAULT 0);
CREATE TEMP TABLE memory_collection_owner(id int PRIMARY KEY,owner_id uuid);
INSERT INTO memory_collection_owner VALUES(1,'00000000-0000-4000-8000-000000000001');
CREATE TEMP TABLE memory_scopes(memory_id bigint,scope_type text,scope_value text);
CREATE TEMP TABLE memory_units(id bigserial PRIMARY KEY,memory_id bigint,unit_type text,unit_key text,unit_text text,weight double precision,memory_kind text,is_episode_card int DEFAULT 0);
CREATE INDEX memory_units_card_fixture_idx ON memory_units(memory_id);
CREATE TEMP TABLE rules(id bigint,record_revision bigint DEFAULT 1,domain text DEFAULT '',expires_at text DEFAULT '');
GRANT SELECT ON rules TO PUBLIC;
CREATE TEMP TABLE memory_lineage(object_type text,object_id bigint,source_kind text,source_ref text,confidence double precision);
CREATE INDEX memory_lineage_card_fixture_idx ON memory_lineage(object_type,object_id);
-- This command fixture isolates card admission. Registry SQL/replay is tested
-- against the shipping schema in dependency_registry_test.go.
CREATE FUNCTION archive_command_test.derived_memory_declare(text,text,jsonb,text) RETURNS bigint LANGUAGE sql AS $$ SELECT jsonb_array_length($3)::bigint $$;
CREATE TEMP TABLE memory_relations(memory_id bigint,src_entity text,relation text,dst_entity text,fact_text text);
INSERT INTO memories(key,content,scope_type,scope_value,artifact_ref) VALUES ('common','shared conventions','global','_global','README.md'),('app','project details','project','app','main.go'),('private','secret source','project','private','');
INSERT INTO memory_scopes VALUES (2,'workspace','team');
CREATE ROLE memory_archive_test NOINHERIT NOBYPASSRLS;
GRANT USAGE ON SCHEMA archive_command_test TO memory_archive_test;
GRANT SELECT ON memory_collection_owner TO memory_archive_test;
GRANT UPDATE ON memories TO memory_archive_test;
GRANT SELECT,INSERT ON memories,memory_scopes,memory_units,memory_lineage,memory_relations TO memory_archive_test;
GRANT USAGE,SELECT ON SEQUENCE memories_id_seq,memory_units_id_seq TO memory_archive_test;
ALTER TABLE memories ENABLE ROW LEVEL SECURITY;
CREATE POLICY test_visibility ON memories USING (scope_type='global' OR current_setting('aimee.memory_scope_all',true)='1' OR (scope_type=current_setting('aimee.memory_scope_type',true) AND scope_value=current_setting('aimee.memory_scope_value',true)));
SET LOCAL ROLE memory_archive_test;`)
	if err != nil {
		t.Fatal(err)
	}
	modelCalls := 0
	backend := &postgresDataStore{db: runtimeRoleDB{evalQueryer{tx}, t}, placement: PlacementKB,
		settings: func() (map[string]any, error) {
			return map[string]any{"memory_episode_summaries_enabled": true, "memory_cognify_command": "fixture"}, nil
		},
		episodeCommand: func(_ context.Context, command string, input []byte) ([]byte, error) {
			modelCalls++
			if strings.Contains(string(input), "ineligible-card-source") {
				t.Fatal("ineligible source sent to card model", string(input))
			}
			if command != "fixture" {
				t.Fatal(command)
			}
			var request struct {
				Turns     []episodeTurn `json:"turns"`
				SessionID string        `json:"session_id"`
			}
			if err := json.Unmarshal(input, &request); err != nil {
				return nil, err
			}
			card := episodeCard{SessionID: request.SessionID, Title: "Session " + request.SessionID}
			for _, turn := range request.Turns {
				card.Events = append(card.Events, turn.Text)
			}
			return json.Marshal(card)
		}}
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, backend)))
	run := func(verb string, args map[string]any) map[string]any {
		t.Helper()
		data, _ := json.Marshal(args)
		return runPublicCommand(t, client, verb, string(data))
	}
	// Exclude unavailable source states before scope compatibility, capacity and
	// the external model call. The fixture uses a savepoint to isolate exports.
	if _, err := tx.Exec(ctx, `SAVEPOINT card_source_eligibility; RESET ROLE;
 INSERT INTO memories(key,content,source_session,lifecycle_state,activation_suppressed,valid_from,valid_until)
 SELECT 'card-source-'||name,'ineligible-card-source '||name,'card-eligibility',state,suppressed,starts,ends
 FROM (VALUES
 ('future','active',0,(CURRENT_TIMESTAMP+interval '1 hour')::text,''),
 ('expired','active',0,'',CURRENT_TIMESTAMP::text),
 ('suppressed','active',1,'',''),
 ('superseded','superseded',0,'',''),
 ('archived','archived',0,'',''),
 ('quarantined','quarantined',0,'',''),
 ('deleted','deleted',0,'',''),
 ('revoked','revoked',0,'','')) fixture(name,state,suppressed,starts,ends);
 SET LOCAL ROLE memory_archive_test;`); err != nil {
		t.Fatal(err)
	}
	callsBefore := modelCalls
	if r := run("episode_card_generate", map[string]any{"source_session": "card-eligibility", "scope_context": true}); r["kind"] != "not_found" || modelCalls != callsBefore {
		t.Fatal("empty eligible source set reached model", r, modelCalls, callsBefore)
	}
	if _, err := tx.Exec(ctx, `RESET ROLE; INSERT INTO memories(key,content,source_session) VALUES('card-source-current','current allowed card source','card-eligibility'); SET LOCAL ROLE memory_archive_test`); err != nil {
		t.Fatal(err)
	}
	if r := run("episode_card_generate", map[string]any{"source_session": "card-eligibility", "scope_context": true}); r["status"] != "ok" || modelCalls != callsBefore+1 {
		t.Fatal("eligible card source was not generated", r, modelCalls, callsBefore)
	}
	if _, err := tx.Exec(ctx, `ROLLBACK TO SAVEPOINT card_source_eligibility; RELEASE SAVEPOINT card_source_eligibility`); err != nil {
		t.Fatal(err)
	}
	for _, verb := range []string{"episode_cards", "episode_card_generate", "export_jsonl", "decisions_export_jsonl"} {
		if r := run(verb, map[string]any{}); r["kind"] != "invalid_argument" {
			t.Fatal(verb, r)
		}
	}
	if r := run("episode_card_generate", map[string]any{"source_session": "absent"}); r["kind"] != "not_found" {
		t.Fatal(r)
	}
	if r := run("episode_card_generate", map[string]any{"source_session": "session"}); r["kind"] != "conflict" {
		t.Fatal(r)
	}
	global := run("episode_card_generate", map[string]any{"source_session": "session", "scope_context": true})
	if global["status"] != "ok" {
		t.Fatal(global)
	}
	args := map[string]any{"source_session": "session", "scope_context": true, "project": "app"}
	private := run("episode_card_generate", args)
	if private["status"] != "ok" || private["memory_unit_id"] == global["memory_unit_id"] {
		t.Fatal(private, global)
	}
	again := run("episode_card_generate", args)
	if again["memory_unit_id"] != private["memory_unit_id"] {
		t.Fatal(again, private)
	}

	for _, listing := range []struct {
		args map[string]any
		want int
	}{
		{map[string]any{"source_session": "session", "scope_context": true}, 1},
		{map[string]any{"source_session": "session", "scope_context": true, "project": "app"}, 2},
		{map[string]any{"source_session": "absent", "scope_context": true}, 0},
	} {
		result := run("episode_cards", listing.args)
		cards, ok := result["cards"].([]any)
		if !ok || len(cards) != listing.want {
			t.Fatal(result)
		}
		for _, card := range cards {
			if strings.Contains(card.(string), "secret") {
				t.Fatal(result)
			}
		}
	}
	// Return to fixture owner to inspect all scopes; the public call used RLS.
	if _, err = tx.Exec(ctx, `RESET ROLE`); err != nil {
		t.Fatal(err)
	}
	var scope, content, epistemic string
	var lineage int
	err = tx.QueryRow(ctx, `SELECT m.scope_value,m.content,m.epistemic_kind FROM memories m JOIN memory_units u ON u.memory_id=m.id WHERE u.id=$1`, int64(private["memory_unit_id"].(float64))).Scan(&scope, &content, &epistemic)
	if err != nil || scope != "app" || strings.Contains(content, "secret") || !strings.Contains(content, "project details") || !strings.Contains(content, "shared conventions") || epistemic != "episode" {
		t.Fatal(scope, content, epistemic, err)
	}
	if err = tx.QueryRow(ctx, `SELECT count(*) FROM memory_lineage WHERE object_type='memory_unit' AND object_id=$1`, int64(private["memory_unit_id"].(float64))).Scan(&lineage); err != nil || lineage != 3 {
		t.Fatal(lineage, err)
	}
	var cardParent int64
	if err := tx.QueryRow(ctx, `SELECT memory_id FROM memory_units WHERE id=$1`, int64(private["memory_unit_id"].(float64))).Scan(&cardParent); err != nil {
		t.Fatal(err)
	}
	checkActivatedCard := func(want int) {
		t.Helper()
		if _, err := tx.Exec(ctx, `SELECT set_config('aimee.memory_scope_all','0',true),set_config('aimee.memory_scope_type','project',true),set_config('aimee.memory_scope_value','app',true)`); err != nil {
			t.Fatal(err)
		}
		records, _, _, err := backend.recallActivated(ctx, &ActivationSnapshot{CurrentTurn: 100, Rows: []ActivationRow{}},
			"m.id=$1", 1, false, false, cardParent)
		if err != nil || len(records) != want {
			t.Fatal("activated card input fence", records, want, err)
		}
	}
	if _, err := tx.Exec(ctx, "SET LOCAL ROLE memory_archive_test"); err != nil {
		t.Fatal(err)
	}
	checkActivatedCard(1)
	if _, err := tx.Exec(ctx, "RESET ROLE"); err != nil {
		t.Fatal(err)
	}
	for _, tc := range []struct {
		name, mutation string
		cards          int
	}{
		{"new private input", "INSERT INTO memories(tier,kind,key,content,scope_type,scope_value,source_session) SELECT 'L2','fact','new-session-input','additional evidence','project','app',source_session FROM memories WHERE key='app'", 1},
		{"shared input changed", "UPDATE memories SET record_revision=record_revision+1 WHERE key='common'", 0},
		{"private input changed", "UPDATE memories SET record_revision=record_revision+1 WHERE key='app'", 1},
		{"private input revoked", "UPDATE memories SET lifecycle_state='revoked' WHERE key='app'", 1},
		{"private input expired", "UPDATE memories SET valid_until=(now()-interval '1 second')::text WHERE key='app'", 1},
		{"private input hidden", "UPDATE memories SET scope_value='private' WHERE key='app'", 1},
		{"unit changed", fmt.Sprintf("UPDATE memory_units SET unit_text='unobserved text' WHERE id=%d", int64(private["memory_unit_id"].(float64))), 1},
		{"observation missing", fmt.Sprintf("DELETE FROM memory_lineage WHERE object_type='memory_unit' AND object_id=%d AND source_kind='episode-card-input-v1'", int64(private["memory_unit_id"].(float64))), 1},
		{"card parent changed", fmt.Sprintf("UPDATE memories SET record_revision=record_revision+1 WHERE id=%d", cardParent), 1},
	} {
		if _, err := tx.Exec(ctx, "SAVEPOINT card_input_observation; "+tc.mutation+"; SET LOCAL ROLE memory_archive_test"); err != nil {
			t.Fatal(err)
		}
		checkActivatedCard(0)
		listed := run("episode_cards", args)
		cards, ok := listed["cards"].([]any)
		if listed["status"] != "ok" || !ok || len(cards) != tc.cards {
			t.Fatal(tc.name, listed)
		}
		if record, err := backend.Get(ctx, Scope{Type: "project", Value: "app"}, cardParent); !errors.Is(err, ErrMemoryNotFound) {
			t.Fatal("canonical card bypassed input fence", tc.name, record, err)
		}
		if _, err := tx.Exec(ctx, "ROLLBACK TO SAVEPOINT card_input_observation; RELEASE SAVEPOINT card_input_observation"); err != nil {
			t.Fatal(err)
		}
	}
	var links int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_relations r JOIN memory_units u ON u.memory_id=r.memory_id WHERE u.id=$1 AND r.relation='REL_SUMMARISES'`, int64(private["memory_unit_id"].(float64))).Scan(&links); err != nil || links != 2 {
		t.Fatal(links, err)
	}
	if _, err := tx.Exec(ctx, `INSERT INTO memories(key,content,source_session) SELECT 'capacity-'||n,'source','capacity' FROM generate_series(1,201)n; SET LOCAL ROLE memory_archive_test`); err != nil {
		t.Fatal(err)
	}
	if r := run("episode_card_generate", map[string]any{"source_session": "capacity", "scope_context": true}); r["kind"] != "capacity_exceeded" {
		t.Fatal(r)
	}
	if _, err := tx.Exec(ctx, `RESET ROLE`); err != nil {
		t.Fatal(err)
	}
	// Failed lineage capture must not leave an orphan parent or unit.
	_, err = tx.Exec(ctx, `INSERT INTO memories(key,content,source_session,scope_type,scope_value) VALUES ('failure','rollback source','failure','project','app');
ALTER TABLE memory_lineage ADD CONSTRAINT fail_lineage CHECK (object_id<0) NOT VALID;
SET LOCAL ROLE memory_archive_test;`)
	if err != nil {
		t.Fatal(err)
	}
	args["source_session"] = "failure"
	if r := run("episode_card_generate", args); r["kind"] != "unavailable" {
		t.Fatal(r)
	}
	if _, err = tx.Exec(ctx, `RESET ROLE`); err != nil {
		t.Fatal(err)
	}
	var n int
	if err = tx.QueryRow(ctx, `SELECT count(*) FROM memories WHERE key='episode-card:failure'`).Scan(&n); err != nil || n != 0 {
		t.Fatal(n, err)
	}
	_, err = tx.Exec(ctx, `ALTER TABLE memory_lineage DROP CONSTRAINT fail_lineage;
INSERT INTO memories(key,content,source_session,kind) SELECT 'page-'||n,repeat('export content ',400),'export','decision' FROM generate_series(1,130) n;
SET LOCAL ROLE memory_archive_test;`)
	if err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(t.TempDir(), "memories.jsonl")
	args = map[string]any{"path": path, "scope_context": true, "project": "app"}
	r := run("export_jsonl", args)
	if r["status"] != "ok" {
		t.Fatal(r)
	}
	data, err := os.ReadFile(path)
	if err != nil || strings.Contains(string(data), "secret source") {
		t.Fatal(err, string(data[:min(len(data), 100)]))
	}
	lines := strings.Split(strings.TrimSpace(string(data)), "\n")
	if len(lines) != int(r["count"].(float64)) || len(lines) < 130 {
		t.Fatal(len(lines), r)
	}
	for _, line := range lines {
		var row map[string]any
		if json.Unmarshal([]byte(line), &row) != nil || row["scopes"] == nil || row["primary_scope"] == nil {
			t.Fatal(line)
		}
		if strings.HasPrefix(row["key"].(string), "page-") && len(row["content"].(string)) != len(strings.Repeat("export content ", 400)) {
			t.Fatal("export truncated content")
		}
	}
	if info, err := os.Stat(path); err != nil || info.Mode().Perm() != 0600 {
		t.Fatal(info, err)
	}
	// Read failure after staging does not truncate the existing output.
	_, err = tx.Exec(ctx, `RESET ROLE;REVOKE SELECT ON memory_scopes FROM memory_archive_test;SET LOCAL ROLE memory_archive_test;`)
	if err != nil {
		t.Fatal(err)
	}
	if r := run("export_jsonl", args); r["kind"] != "unavailable" {
		t.Fatal(r)
	}
	after, err := os.ReadFile(path)
	if err != nil || string(after) != string(data) {
		t.Fatal("failed export changed destination", err)
	}
	staged, err := filepath.Glob(filepath.Join(filepath.Dir(path), ".aimee-memory-export-*"))
	if err != nil || len(staged) != 0 {
		t.Fatal(staged, err)
	}
	if r := run("decisions_export_jsonl", args); r["status"] != "ok" || r["count"] != float64(130) {
		t.Fatal(r)
	}
	// Search retains the window response fields and follows request visibility.
	r = run("search", map[string]any{"clusters": []any{"shared", nil, 2}, "scope_context": true, "project": "app"})
	if r["status"] != "ok" || len(r["results"].([]any)) == 0 {
		t.Fatal(r)
	}
	for _, row := range r["results"].([]any) {
		if row.(map[string]any)["files"] == nil {
			t.Fatal(row)
		}
	}
	r = run("search", map[string]any{"clusters": []string{"secret"}, "scope_context": true, "project": "app"})
	if r["status"] != "ok" || len(r["results"].([]any)) != 0 {
		t.Fatal(r)
	}
}
