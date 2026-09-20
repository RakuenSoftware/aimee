package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func sharedChangeMigration(t *testing.T) string {
	t.Helper()
	body, err := os.ReadFile("../../../src/modules/db2/c/schema.sql")
	if err != nil {
		t.Fatal(err)
	}
	a := strings.Index(string(body), "-- BEGIN memory change journal")
	b := strings.Index(string(body), "-- END memory change journal")
	if a < 0 || b < a {
		t.Fatal("shipping shared change migration missing")
	}
	return string(body[a:b])
}

func TestSharedMemoryCollectionCommitOrder(t *testing.T) {
	dsn := os.Getenv("AIMEE_MEMORY_EVAL_URL")
	if dsn == "" {
		if os.Getenv("AIMEE_MEMORY_EVAL_REQUIRED") == "1" {
			t.Fatal("AIMEE_MEMORY_EVAL_URL required")
		}
		t.Skip("set AIMEE_MEMORY_EVAL_URL")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	owner, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer owner.Close(context.Background())
	schema := fmt.Sprintf("shared_change_order_%d", time.Now().UnixNano())
	ident := pgx.Identifier{schema}.Sanitize()
	if _, err := owner.Exec(ctx, "CREATE SCHEMA "+ident); err != nil {
		t.Fatal(err)
	}
	defer func() {
		cleanup, stop := context.WithTimeout(context.Background(), 5*time.Second)
		defer stop()
		if _, err := owner.Exec(cleanup, "DROP SCHEMA "+ident+" CASCADE"); err != nil {
			t.Error("drop owned shared journal fixture", err)
		}
	}()
	if _, err := owner.Exec(ctx, "SET search_path="+ident+`,public;
 CREATE TABLE memories(id BIGINT PRIMARY KEY,scope_type TEXT NOT NULL,scope_value TEXT NOT NULL,content TEXT);
 CREATE TABLE memory_scopes(memory_id BIGINT REFERENCES memories(id),scope_type TEXT,scope_value TEXT);
 CREATE FUNCTION memory_row_scope_visible(TEXT,TEXT) RETURNS BOOLEAN LANGUAGE sql AS $$ SELECT true $$;
 `+sharedChangeMigration(t)+`
 INSERT INTO memories VALUES(1,'project','alpha','one',1,0),(2,'project','alpha','two',1,0),(3,'project','beta','three',1,0);`); err != nil {
		t.Fatal(err)
	}
	connect := func() *pgx.Conn {
		t.Helper()
		c, err := pgx.Connect(ctx, dsn)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := c.Exec(ctx, "SET search_path="+ident+",public"); err != nil {
			c.Close(context.Background())
			t.Fatal(err)
		}
		return c
	}
	first, second, independent := connect(), connect(), connect()
	defer first.Close(context.Background())
	defer second.Close(context.Background())
	defer independent.Close(context.Background())
	if _, err := first.Exec(ctx, `BEGIN; UPDATE memories SET content='first committed' WHERE id=1`); err != nil {
		t.Fatal(err)
	}
	if _, err := second.Exec(ctx, "BEGIN"); err != nil {
		t.Fatal(err)
	}
	written := make(chan error, 1)
	go func() {
		_, err := second.Exec(ctx, `UPDATE memories SET content='second committed' WHERE id=2`)
		written <- err
	}()
	// Observe actual lock dependency rather than assume a scheduled goroutine
	// reached its write. Same-collection positions cannot commit out of order.
	blocked := false
	for !blocked {
		if err := owner.QueryRow(ctx, `SELECT $1=ANY(pg_blocking_pids($2))`, first.PgConn().PID(), second.PgConn().PID()).Scan(&blocked); err != nil {
			t.Fatal(err)
		}
		if !blocked {
			select {
			case err := <-written:
				t.Fatal("same-collection writer did not await predecessor", err)
			case <-ctx.Done():
				t.Fatal(ctx.Err())
			case <-time.After(time.Millisecond):
			}
		}
	}
	// A different collection can commit while both alpha writers remain open.
	// This is a lock-independence check, not a latency benchmark.
	otherContext, stop := context.WithTimeout(ctx, 5*time.Second)
	_, err = independent.Exec(otherContext, `UPDATE memories SET content='independent' WHERE id=3`)
	stop()
	if err != nil {
		t.Fatal("unrelated collection blocked behind alpha", err)
	}
	var alpha, beta int64
	if err := owner.QueryRow(ctx, `SELECT
 (SELECT generation FROM memory_collection_generations WHERE scope_value='alpha'),
 (SELECT generation FROM memory_collection_generations WHERE scope_value='beta')`).Scan(&alpha, &beta); err != nil || alpha != 2 || beta != 2 {
		t.Fatal("uncommitted or unrelated progress", alpha, beta, err)
	}
	if _, err := first.Exec(ctx, "COMMIT"); err != nil {
		t.Fatal(err)
	}
	if err := <-written; err != nil {
		t.Fatal(err)
	}
	if _, err := second.Exec(ctx, "COMMIT"); err != nil {
		t.Fatal(err)
	}
	var order []int64
	if err := owner.QueryRow(ctx, `SELECT array_agg(memory_id ORDER BY generation)
 FROM memory_invalidation_outbox WHERE scope_value='alpha' AND generation>2`).Scan(&order); err != nil || len(order) != 2 || order[0] != 1 || order[1] != 2 {
		t.Fatal("collection positions differ from commit order", order, err)
	}
}

func TestSharedMemoryChangeJournal(t *testing.T) {
	dsn := os.Getenv("AIMEE_MEMORY_EVAL_URL")
	if dsn == "" {
		if os.Getenv("AIMEE_MEMORY_EVAL_REQUIRED") == "1" {
			t.Fatal("AIMEE_MEMORY_EVAL_URL required")
		}
		t.Skip("set AIMEE_MEMORY_EVAL_URL")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	conn, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close(context.Background())
	tx, err := conn.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(context.Background())
	exec := func(q string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, q, args...); err != nil {
			t.Fatal(err)
		}
	}
	// Entire schema, role and mutations roll back, preserving the caller's DB.
	exec(`CREATE SCHEMA shared_change_journal_test;
 SET LOCAL search_path=shared_change_journal_test,public;
 CREATE ROLE shared_change_journal_runtime NOINHERIT NOBYPASSRLS;
 GRANT USAGE ON SCHEMA shared_change_journal_test TO shared_change_journal_runtime;
 ALTER DEFAULT PRIVILEGES IN SCHEMA shared_change_journal_test GRANT ALL ON TABLES TO shared_change_journal_runtime;
 ALTER DEFAULT PRIVILEGES IN SCHEMA shared_change_journal_test GRANT EXECUTE ON FUNCTIONS TO shared_change_journal_runtime;
 CREATE TABLE memories(id BIGINT PRIMARY KEY,key TEXT UNIQUE,content TEXT,scope_type TEXT NOT NULL,scope_value TEXT NOT NULL,
 use_count BIGINT NOT NULL DEFAULT 0,last_used_at TEXT,updated_at TEXT);
 CREATE TABLE memory_scopes(memory_id BIGINT REFERENCES memories(id) ON DELETE CASCADE,
 scope_type TEXT,scope_value TEXT,PRIMARY KEY(memory_id,scope_type,scope_value));
 CREATE FUNCTION memory_row_scope_visible(t TEXT,v TEXT) RETURNS BOOLEAN LANGUAGE sql STABLE AS $$
 SELECT (t='global' AND v='_global') OR current_setting('aimee.memory_scope_all',true)='1'
 OR (t=current_setting('aimee.memory_scope_type',true) AND v=current_setting('aimee.memory_scope_value',true)) $$;
 ALTER TABLE memories ENABLE ROW LEVEL SECURITY;
 CREATE POLICY visible ON memories USING(memory_row_scope_visible(scope_type,scope_value))
 WITH CHECK(memory_row_scope_visible(scope_type,scope_value));`)
	exec(sharedChangeMigration(t))
	exec(sharedChangeMigration(t)) // upgrade/reapply must preserve the owner
	var owner string
	if err := tx.QueryRow(ctx, `SELECT owner_id::text FROM memory_collection_owner`).Scan(&owner); err != nil {
		t.Fatal(err)
	}
	exec(`SET LOCAL ROLE shared_change_journal_runtime;
 SELECT set_config('aimee.memory_scope_all','1',true)`)
	exec(`INSERT INTO memories(id,key,content,scope_type,scope_value,record_revision) VALUES
 (1,'a','original','project','alpha',99),(2,'b','hidden','project','beta',99)`)
	var revision int64
	if err := tx.QueryRow(ctx, `SELECT record_revision FROM memories WHERE id=1`).Scan(&revision); err != nil || revision != 1 {
		t.Fatal("forged initial revision", revision, err)
	}
	db := runtimeRoleDB{evalQueryer{tx}, t}
	backend := &postgresDataStore{db: db, placement: PlacementKB}
	handler := NewHandler(nil, WithDataStore(PlacementKB, backend))
	read := func(scope string, cursor *MemoryChangeCursor, limit int) MemoryChangePage {
		t.Helper()
		body, status := handler(bus.ModuleInvocation{StageID: StageData}, dataRequest(t, DataRequest{
			Operation: "change-feed", Scope: Scope{Type: ScopeProject, Value: scope},
			Changes: &MemoryChangesRequest{SchemaVersion: 1, After: cursor, Limit: limit},
		}))
		var response DataResponse
		if status != bus.ModuleStatusOK || json.Unmarshal(body, &response) != nil || response.Changes == nil {
			t.Fatalf("scoped feed status=%v body=%s", status, body)
		}
		return *response.Changes
	}
	head := read("alpha", nil, 1)
	if !head.SnapshotRequired || head.Head.OwnerID != owner || head.Head.Collection != "project:alpha" || head.Head.Generation != 1 {
		t.Fatal("invalid bootstrap", head)
	}
	cursor := head.Head
	cursor.Generation = 0
	page := read("alpha", &cursor, 1)
	if page.SnapshotRequired || page.More || len(page.Events) != 1 || page.Events[0].MemoryID != 1 {
		t.Fatal("scoped replay leaked another collection", page)
	}
	cursor = page.Next
	if empty := read("empty", nil, 1); !empty.SnapshotRequired || empty.Head.Generation != 0 || empty.Head.OwnerID != owner {
		t.Fatal("empty collection lost owner identity", empty)
	}
	if wrong := read("beta", &cursor, 1); !wrong.SnapshotRequired || len(wrong.Events) != 0 {
		t.Fatal("cross-collection cursor accepted", wrong)
	}
	exec(`SELECT set_config('aimee.memory_scope_all','1',true);
 UPDATE memories SET use_count=use_count+1,last_used_at='now',updated_at='now',record_revision=900 WHERE id=1;
 INSERT INTO memories(id,key,content,scope_type,scope_value) VALUES(3,'a','original','project','alpha')
 ON CONFLICT(key) DO UPDATE SET content=EXCLUDED.content`)
	if unchanged := read("alpha", &cursor, 4); unchanged.Head != cursor || len(unchanged.Events) != 0 {
		t.Fatal("counter/identical upsert emitted an event", unchanged)
	}
	exec(`SELECT set_config('aimee.memory_scope_all','1',true);
 INSERT INTO memories(id,key,content,scope_type,scope_value) VALUES(4,'a','corrected','project','alpha')
 ON CONFLICT(key) DO UPDATE SET content=EXCLUDED.content`)
	page = read("alpha", &cursor, 4)
	if len(page.Events) != 1 || page.Events[0].MemoryID != 1 || page.Events[0].RecordRevision != 2 || page.Events[0].Operation != "update" {
		t.Fatal("changed upsert emitted discarded identity", page)
	}
	cursor = page.Next
	exec(`SELECT set_config('aimee.memory_scope_all','1',true);
	 INSERT INTO memory_scopes VALUES(1,'project','beta'),(1,'workspace','team'),(1,'workspace','other')`)
	page = read("alpha", &cursor, 4)
	if len(page.Events) != 1 || page.Events[0].RecordRevision != 3 {
		t.Fatal("scope tag batch must invalidate its parent once", page)
	}
	cursor = page.Next
	exec(`UPDATE memory_scopes SET scope_value=scope_value WHERE memory_id=1`)
	if unchanged := read("alpha", &cursor, 4); unchanged.Head != cursor || len(unchanged.Events) != 0 {
		t.Fatal("unchanged tag batch advanced progress", unchanged)
	}
	if beta := read("beta", nil, 4); beta.Head.Generation != 1 {
		t.Fatal("secondary tag exposed primary identity in another collection", beta)
	}
	cursor = page.Next
	exec(`SELECT set_config('aimee.memory_scope_all','1',true);
 UPDATE memories SET scope_value='beta' WHERE id=1`)
	old := read("alpha", &cursor, 4)
	if len(old.Events) != 1 || old.Events[0].RecordRevision != 4 {
		t.Fatal("scope move failed to invalidate old collection", old)
	}
	if beta := read("beta", nil, 4); beta.Head.Generation != 2 {
		t.Fatal("scope move failed to invalidate new collection", beta)
	}
	// RLS applies directly to metadata and secondary tags, not only to the Go
	// endpoint. A runtime reader of alpha cannot inspect beta's IDs or progress.
	exec(`SELECT set_config('aimee.memory_scope_all','0',true),
 set_config('aimee.memory_scope_type','project',true),set_config('aimee.memory_scope_value','alpha',true)`)
	var hidden int
	if err := tx.QueryRow(ctx, `SELECT (SELECT count(*) FROM memory_invalidation_outbox WHERE scope_value='beta')+
 (SELECT count(*) FROM memory_collection_generations WHERE scope_value='beta')+(SELECT count(*) FROM memory_scopes)`).Scan(&hidden); err != nil || hidden != 0 {
		t.Fatal("hidden metadata escaped scope RLS", hidden, err)
	}
	for _, q := range []string{
		`UPDATE memory_collection_owner SET owner_id=gen_random_uuid()`,
		`DELETE FROM memory_collection_generations`,
		`TRUNCATE memory_invalidation_outbox`,
		`INSERT INTO memory_scopes VALUES(2,'project','alpha')`,
	} {
		exec("SAVEPOINT forbidden")
		if _, err := tx.Exec(ctx, q); err == nil {
			t.Fatal("runtime altered protected or hidden state", q)
		}
		exec("ROLLBACK TO SAVEPOINT forbidden")
	}
	exec(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	var callable bool
	if err := tx.QueryRow(ctx, `SELECT has_function_privilege(current_user,'memory_capture_record_change()','EXECUTE')
 OR has_function_privilege(current_user,'memory_assign_record_revision()','EXECUTE')
 OR has_function_privilege(current_user,'memory_capture_scope_change()','EXECUTE')`).Scan(&callable); err != nil || callable {
		t.Fatal("runtime may attach protected triggers", callable, err)
	}
	beta := read("beta", nil, 4).Head
	exec(`SELECT set_config('aimee.memory_scope_all','1',true); DELETE FROM memories WHERE id=1`)
	page = read("beta", &beta, 4)
	if len(page.Events) != 1 || page.Events[0].Operation != "delete" || page.Events[0].RecordRevision != 5 {
		t.Fatal("cascade lost or duplicated deletion", page)
	}
	beta = page.Next
	exec(`INSERT INTO memory_scopes VALUES(2,'workspace','one'),(2,'workspace','two');
 UPDATE memory_scopes SET scope_value=scope_value||'-renamed' WHERE memory_id=2;
 DELETE FROM memory_scopes WHERE memory_id=2`)
	page = read("beta", &beta, 4)
	if len(page.Events) != 3 {
		t.Fatal("tag insert/update/delete batches did not advance once each", page)
	}
	for i, event := range page.Events {
		if event.MemoryID != 2 || event.RecordRevision != int64(i+2) || event.Operation != "update" {
			t.Fatal("tag batch invalidated wrong record or revision", page)
		}
	}
	beta = page.Next
	exec(`RESET ROLE;
 ALTER TABLE memory_invalidation_outbox ADD CONSTRAINT journal_failure CHECK(memory_id<>2) NOT VALID;
 SET LOCAL ROLE shared_change_journal_runtime;
 SELECT set_config('aimee.memory_scope_all','1',true);
 SAVEPOINT journal_failure`)
	if _, err := tx.Exec(ctx, `UPDATE memories SET content='must roll back' WHERE id=2`); err == nil {
		t.Fatal("outbox failure accepted canonical write")
	}
	exec("ROLLBACK TO SAVEPOINT journal_failure")
	var content string
	if err := tx.QueryRow(ctx, `SELECT content FROM memories WHERE id=2`).Scan(&content); err != nil || content != "hidden" {
		t.Fatal("outbox failure changed canonical content", content, err)
	}
	if unchanged := read("beta", &beta, 4); unchanged.Head != beta || len(unchanged.Events) != 0 {
		t.Fatal("outbox failure advanced progress", unchanged)
	}
	// An owner-side retention gap must force a snapshot, never skip ahead.
	exec(`RESET ROLE; DELETE FROM memory_invalidation_outbox WHERE scope_value='beta' AND generation=2;
 SET LOCAL ROLE shared_change_journal_runtime`)
	beta.Generation = 1
	if gap := read("beta", &beta, 4); !gap.SnapshotRequired || len(gap.Events) != 0 {
		t.Fatal("retention gap accepted", gap)
	}
}
