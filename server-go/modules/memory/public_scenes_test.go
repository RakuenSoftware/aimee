package memory

import (
	"context"
	"os"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5"
)

func TestSceneCommandsPostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_MEMORY_EVAL_URL")
	if dsn == "" {
		t.Skip("set AIMEE_MEMORY_EVAL_URL for scene commands")
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
	_, err = tx.Exec(ctx, `CREATE TEMP TABLE memories(id bigint PRIMARY KEY,key text,scope_value text,lifecycle_state text DEFAULT 'active');
 INSERT INTO memories VALUES(1,repeat('long-key',100),'app','active'),(2,'hidden','private','active'),(3,'retired','app','rejected');
 INSERT INTO memories SELECT n,'member-'||n,'app','active' FROM generate_series(4,604)n;
 CREATE TEMP TABLE memory_scenes(id bigint PRIMARY KEY,workspace_id text,turn_count int,created_at text);
 INSERT INTO memory_scenes VALUES(1,'app',602,'2026-01-01'),(2,'private',1,'2026-02-01'),(3,'retired',1,'2026-03-01');
 CREATE TEMP TABLE memory_scene_members(scene_id bigint,memory_id bigint,membership_strength double precision);
 INSERT INTO memory_scene_members VALUES(1,1,1),(1,2,0.5),(2,2,1),(3,3,1);
 INSERT INTO memory_scene_members SELECT 1,n,0.1 FROM generate_series(4,604)n;
 CREATE ROLE scene_command_test NOINHERIT NOBYPASSRLS;
 GRANT SELECT ON memories,memory_scenes,memory_scene_members TO scene_command_test;
 ALTER TABLE memories ENABLE ROW LEVEL SECURITY;
 CREATE POLICY scene_visibility ON memories USING(current_setting('aimee.memory_scope_all',true)='1' OR scope_value=current_setting('aimee.memory_scope_value',true));
 SET LOCAL ROLE scene_command_test`)
	if err != nil {
		t.Fatal(err)
	}
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, &postgresDataStore{db: runtimeRoleDB{evalQueryer{tx}, t}, placement: PlacementKB})))
	run := func(verb, args string) map[string]any {
		t.Helper()
		r := runPublicCommand(t, client, verb, args)
		if r["status"] != "ok" {
			t.Fatal(r)
		}
		return r
	}
	scenes := run("scene_list", `{"scope_context":true,"project":"app","limit":1}`)["scenes"].([]any)
	if len(scenes) != 1 || scenes[0].(map[string]any)["id"] != float64(1) {
		t.Fatal(scenes)
	}
	if r := run("scene_list", `{"scope_context":true}`); len(r["scenes"].([]any)) != 0 || r["active_context_missing"] != true {
		t.Fatal(r)
	}
	members := run("scene_show", `{"scene_id":1,"scope_context":true,"project":"app","limit":999}`)["members"].([]any)
	if len(members) != 512 || members[0].(map[string]any)["key"] != strings.Repeat("long-key", 100) {
		t.Fatal(len(members), members[0])
	}
	for _, row := range members {
		if row.(map[string]any)["memory_id"] == float64(2) {
			t.Fatal("hidden member", row)
		}
	}
	for _, args := range []string{`{"scene_id":2,"scope_context":true,"project":"app"}`, `{"scene_id":3}`, `{"scene_id":99}`} {
		if r := run("scene_show", args); len(r["members"].([]any)) != 0 {
			t.Fatal(r)
		}
	}
	for _, args := range []string{`{}`, `{"scene_id":0}`, `{"scene_id":1.5}`} {
		if r := runPublicCommand(t, client, "scene_show", args); r["kind"] != "invalid_argument" {
			t.Fatal(r)
		}
	}
}
