package memory

import (
	"context"
	"encoding/json"
	"os"
	"reflect"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/db"
	"github.com/jackc/pgx/v5"
)

type scopeCountingStore struct {
	store.Queryer
	queries int
}

func (s *scopeCountingStore) Query(ctx context.Context, q string, args ...any) (store.Rows, error) {
	s.queries++
	return s.Queryer.Query(ctx, q, args...)
}

func TestScopeOwnerPostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_MEMORY_EVAL_URL")
	if dsn == "" {
		t.Skip("set AIMEE_MEMORY_EVAL_URL")
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
	sql := func(q string) {
		t.Helper()
		if _, err := tx.Exec(ctx, q); err != nil {
			t.Fatal(err)
		}
	}
	sql(`CREATE SCHEMA scope_owner_test;
CREATE FUNCTION scope_owner_test.pg_now_text() RETURNS text LANGUAGE sql AS $$ SELECT now()::text $$;
SET LOCAL search_path TO pg_temp,scope_owner_test,public;
CREATE TEMP TABLE memories(id bigint PRIMARY KEY,scope_type text DEFAULT 'global',scope_value text DEFAULT '_global',updated_at text);
CREATE TEMP TABLE memory_scopes(memory_id bigint REFERENCES memories(id) ON DELETE CASCADE,scope_type text,scope_value text,PRIMARY KEY(memory_id,scope_type,scope_value));
CREATE TEMP TABLE memory_workspaces(memory_id bigint REFERENCES memories(id) ON DELETE CASCADE,workspace text CHECK(workspace<>'reject-projection'),PRIMARY KEY(memory_id,workspace));
INSERT INTO memories(id,scope_type,scope_value) VALUES(1,'project','app'),(2,'workspace','team'),(3,'workspace','_shared'),(4,'global','_global'),(5,'project','private');
INSERT INTO memories(id) VALUES(6);
INSERT INTO memory_scopes VALUES(5,'project','app');
CREATE ROLE memory_scope_owner_test NOINHERIT NOBYPASSRLS;
GRANT USAGE ON SCHEMA scope_owner_test TO memory_scope_owner_test;
GRANT SELECT,INSERT,UPDATE,DELETE ON memories,memory_scopes,memory_workspaces TO memory_scope_owner_test;
ALTER TABLE memories ENABLE ROW LEVEL SECURITY;
CREATE POLICY scoped ON memories USING(current_setting('aimee.memory_scope_all',true)='1' OR scope_type='global' OR
 (scope_type='project' AND scope_value=current_setting('aimee.memory_project',true)) OR
 (scope_type='workspace' AND (scope_value='_shared' OR scope_value=current_setting('aimee.memory_workspace',true)))) WITH CHECK(true);
SET LOCAL ROLE memory_scope_owner_test;
SELECT set_config('aimee.memory_scope_all','1',true);`)
	db := runtimeRoleDB{evalQueryer{tx}, t}
	backend := &postgresDataStore{db: db, placement: PlacementKB}
	handler := NewHandler(nil, WithDataStore(PlacementKB, backend))
	call := func(req DataRequest) (DataResponse, bus.ModuleStatus) {
		t.Helper()
		raw, status := handler(bus.ModuleInvocation{StageID: StageData}, dataRequest(t, req))
		var out DataResponse
		if status == bus.ModuleStatusOK && json.Unmarshal(raw, &out) != nil {
			t.Fatal(string(raw))
		}
		return out, status
	}
	okCall := func(req DataRequest) DataResponse {
		t.Helper()
		out, status := call(req)
		if status != bus.ModuleStatusOK {
			t.Fatal(req, status)
		}
		return out
	}
	tag := func(id int64, kind, value string) {
		t.Helper()
		r := okCall(DataRequest{Operation: "scope-tag", ID: id, TagScope: &Scope{Type: kind, Value: value}, IncludeAll: true})
		if !r.Updated {
			t.Fatal("tag not applied", r)
		}
	}
	collect := func(id int64) []ScopeTag {
		return okCall(DataRequest{Operation: "scope-collect", ID: id, IncludeAll: true}).Scopes
	}
	primary := func(id int64) ScopeTag {
		return okCall(DataRequest{Operation: "scope-primary", ID: id, IncludeAll: true}).Scopes[0]
	}
	if tags := collect(6); !reflect.DeepEqual(tags, []ScopeTag{{Type: "global", Value: "_global"}}) {
		t.Fatal(tags)
	}
	if p := primary(6); p.Type != "global" || p.Value != "_global" {
		t.Fatal(p)
	}
	tag(6, "project", "app")
	tag(6, "workspace", "team")
	tag(6, "workspace", "team")
	tag(6, "workspace", "second")
	tag(6, "global", "")
	tags := collect(6)
	if len(tags) != 4 || tags[0] != (ScopeTag{Type: "global", Value: "_global"}) || primary(6) != tags[0] {
		t.Fatal("canonical scope lost or duplicated", tags, primary(6))
	}
	var projections int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_workspaces WHERE memory_id=6`).Scan(&projections); err != nil || projections != 2 {
		t.Fatal(projections, err)
	}
	if len(collect(999)) != 0 || primary(999) != (ScopeTag{}) {
		t.Fatal("missing row invented scope")
	}
	// A stale compatibility tag must not make a foreign row visible.
	hidden := okCall(DataRequest{Operation: "scope-collect", ID: 5, Project: "app"})
	if len(hidden.Scopes) != 0 {
		t.Fatal(hidden)
	}
	if _, status := call(DataRequest{Operation: "scope-tag", ID: 6, TagScope: &Scope{Type: "workspace", Value: "reject-projection"}, IncludeAll: true}); status != bus.ModuleStatusInternal {
		t.Fatal("projection failure accepted", status)
	}
	if primary(6) != (ScopeTag{Type: "global", Value: "_global"}) || len(collect(6)) != 4 {
		t.Fatal("partial scope mutation committed")
	}
	for _, private := range []Scope{{Type: "user", Value: "alice"}, {Type: "agent", Value: "reviewer"}} {
		if _, status := call(DataRequest{Operation: "scope-tag", ID: 6, TagScope: &private, IncludeAll: true}); status != bus.ModuleStatusInvalidRequest {
			t.Fatal("KB accepted private scope", private, status)
		}
	}
	ids := []int64{1, 2, 3, 4, 1, 9999, 5}
	sql(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	counter := &scopeCountingStore{Queryer: db}
	ranked := &postgresDataStore{db: counter, placement: PlacementKB}
	batch, err := ranked.ScopeRanks(ctx, ids, "team", "app", false)
	if err != nil || counter.queries != 1 {
		t.Fatal(batch, err, counter.queries)
	}
	for i, want := range []int{3, 2, 1, 1, 3, 0, 0} {
		if batch[i].Rank != want || batch[i].ID != ids[i] {
			t.Fatal(batch)
		}
		single, err := ranked.ScopeRanks(ctx, ids[i:i+1], "team", "app", false)
		if err != nil || single[0] != batch[i] {
			t.Fatal(single, err)
		}
	}
	if counter.queries != 1+len(ids) {
		t.Fatal(counter.queries)
	}
	all, err := ranked.ScopeRanks(ctx, ids, "", "", true)
	if err != nil {
		t.Fatal(err)
	}
	for i, row := range all {
		want := 1
		if ids[i] == 9999 {
			want = 0
		}
		if row.Rank != want {
			t.Fatal(all)
		}
	}
	// Native cascade regression, now against the owner's canonical projections.
	sql(`DELETE FROM memories WHERE id=6`)
	if err := tx.QueryRow(ctx, `SELECT (SELECT count(*) FROM memory_scopes WHERE memory_id=6)+(SELECT count(*) FROM memory_workspaces WHERE memory_id=6)`).Scan(&projections); err != nil || projections != 0 {
		t.Fatal(projections, err)
	}
}

func exerciseScopeReplay(t *testing.T, ctx context.Context, tx pgx.Tx, handler bus.ModuleHandler) {
	t.Helper()
	if _, err := tx.Exec(ctx, `SELECT set_config('aimee.memory_scope_all','1',true)`); err != nil {
		t.Fatal(err)
	}
	var id int64
	if err := tx.QueryRow(ctx, `INSERT INTO memories(key,content,tier,kind) VALUES('scope-replay','scope projection parity','L2','fact') RETURNING id`).Scan(&id); err != nil {
		t.Fatal(err)
	}
	call := func(req DataRequest) DataResponse {
		t.Helper()
		raw, status := handler(bus.ModuleInvocation{StageID: StageData}, dataRequest(t, req))
		var out DataResponse
		if status != bus.ModuleStatusOK || json.Unmarshal(raw, &out) != nil {
			t.Fatalf("scope replay %s: %v %s", req.Operation, status, raw)
		}
		return out
	}
	for _, scope := range []Scope{{Type: "project", Value: "scope-replay"}, {Type: "workspace", Value: "scope-work"}, {Type: "workspace", Value: "scope-work"}} {
		if !call(DataRequest{Operation: "scope-tag", ID: id, TagScope: &scope, IncludeAll: true}).Updated {
			t.Fatal("packaged tag failed")
		}
	}
	if tags := call(DataRequest{Operation: "scope-collect", ID: id, IncludeAll: true}).Scopes; len(tags) != 2 || tags[0] != (ScopeTag{Type: "workspace", Value: "scope-work"}) {
		t.Fatal(tags)
	}
	if tags := call(DataRequest{Operation: "scope-primary", ID: id, IncludeAll: true}).Scopes; len(tags) != 1 || tags[0] != (ScopeTag{Type: "workspace", Value: "scope-work"}) {
		t.Fatal(tags)
	}
	var n int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_workspaces WHERE memory_id=$1`, id).Scan(&n); err != nil || n != 1 {
		t.Fatal(n, err)
	}
	if !call(DataRequest{Operation: "delete", ID: id, Scope: Scope{Type: "workspace", Value: "scope-work"}, IncludeAll: true}).Deleted {
		t.Fatal("packaged delete failed")
	}
	var lifecycle string
	if err := tx.QueryRow(ctx, `SELECT lifecycle_state FROM memories WHERE id=$1`, id).Scan(&lifecycle); err != nil || lifecycle != "superseded" {
		t.Fatal(lifecycle, err)
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_workspaces WHERE memory_id=$1`, id).Scan(&n); err != nil || n != 1 {
		t.Fatal("archive lost compatibility history", n, err)
	}
	if _, err := tx.Exec(ctx, `DELETE FROM memories WHERE id=$1`, id); err != nil {
		t.Fatal(err)
	}
	if err := tx.QueryRow(ctx, `SELECT (SELECT count(*) FROM memory_workspaces WHERE memory_id=$1)+(SELECT count(*) FROM memory_scopes WHERE memory_id=$1)`, id).Scan(&n); err != nil || n != 0 {
		t.Fatal(n, err)
	}
}
