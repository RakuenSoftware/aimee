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

// Exercise the shipping migration, not a test copy of the trigger. Committed
// fixture state lives only in this test's owned schema so a second connection
// can verify restart visibility and actual writer blocking.
func TestPersonalMemoryChangeJournal(t *testing.T) {
	dsn := os.Getenv("AIMEE_MEMORY_EVAL_URL")
	if dsn == "" {
		if os.Getenv("AIMEE_MEMORY_EVAL_REQUIRED") == "1" {
			t.Fatal("AIMEE_MEMORY_EVAL_URL required")
		}
		t.Skip("set AIMEE_MEMORY_EVAL_URL for durable memory changes")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	conn, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer func() { conn.Close(context.Background()) }()
	schema := fmt.Sprintf("personal_memory_changes_%d", time.Now().UnixNano())
	ident := pgx.Identifier{schema}.Sanitize()
	role := pgx.Identifier{schema + "_runtime"}.Sanitize()
	if _, err := conn.Exec(ctx, "CREATE SCHEMA "+ident); err != nil {
		t.Fatal(err)
	}
	defer func() {
		cleanup, stop := context.WithTimeout(context.Background(), 5*time.Second)
		defer stop()
		_, _ = conn.Exec(cleanup, "RESET ROLE")
		_, _ = conn.Exec(cleanup, "DROP TABLE IF EXISTS pg_temp.user_memory_collection_generation")
		if _, err := conn.Exec(cleanup, "DROP SCHEMA "+ident+" CASCADE"); err != nil {
			t.Error("drop owned change-journal fixture", err)
		}
		if _, err := conn.Exec(cleanup, "DROP ROLE IF EXISTS "+role); err != nil {
			t.Error("drop owned runtime role", err)
		}
	}()
	if _, err := conn.Exec(ctx, "CREATE ROLE "+role+" NOINHERIT NOBYPASSRLS; GRANT USAGE ON SCHEMA "+ident+" TO "+role+"; ALTER DEFAULT PRIVILEGES IN SCHEMA "+ident+" GRANT ALL ON TABLES TO "+role+"; ALTER DEFAULT PRIVILEGES IN SCHEMA "+ident+" GRANT EXECUTE ON FUNCTIONS TO "+role); err != nil {
		t.Fatal(err)
	}
	setScope := func(c *pgx.Conn) {
		t.Helper()
		if _, err := c.Exec(ctx, "SELECT set_config('search_path',$1,false)", schema+",public"); err != nil {
			t.Fatal(err)
		}
	}
	setScope(conn)
	base, err := os.ReadFile("../aimee/families/schema_conversation.sql")
	if err != nil {
		t.Fatal(err)
	}
	body := string(base)
	a := strings.Index(body, "CREATE TABLE IF NOT EXISTS user_memories (")
	b := strings.Index(body, "CREATE INDEX IF NOT EXISTS user_memories_recall")
	if a < 0 || b <= a {
		t.Fatal("shipping private memory schema missing")
	}
	migration, err := os.ReadFile("../aimee/families/schema_personal_memory_changes.sql")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := conn.Exec(ctx, "BEGIN;"+body[a:b]+string(migration)+"COMMIT;"); err != nil {
		_, _ = conn.Exec(context.Background(), "ROLLBACK")
		t.Fatal(err)
	}
	exec := func(query string, args ...any) {
		t.Helper()
		if _, err := conn.Exec(ctx, query, args...); err != nil {
			t.Fatal(err)
		}
	}
	check := func(generation, count int64) {
		t.Helper()
		var gotGeneration, gotCount int64
		if err := conn.QueryRow(ctx, `SELECT generation,(SELECT count(*) FROM user_memory_invalidation_outbox)
 FROM user_memory_collection_generation WHERE id=1`).Scan(&gotGeneration, &gotCount); err != nil || gotGeneration != generation || gotCount != count {
			t.Fatalf("generation=%d events=%d want=%d/%d err=%v", gotGeneration, gotCount, generation, count, err)
		}
	}
	check(0, 0)
	var uncovered int
	if err := conn.QueryRow(ctx, `SELECT count(*) FROM pg_attribute a
 CROSS JOIN (VALUES ('user_memory_assign_revision'),('user_memory_capture_change')) AS expected(name)
 WHERE a.attrelid='user_memories'::regclass AND a.attnum>0 AND NOT a.attisdropped
 AND a.attname NOT IN ('use_count','last_used_at','updated_at')
 AND NOT EXISTS(SELECT 1 FROM pg_trigger t WHERE t.tgrelid=a.attrelid
   AND t.tgname=expected.name AND a.attnum=ANY(t.tgattr::smallint[]))`).Scan(&uncovered); err != nil || uncovered != 0 {
		t.Fatal("governed columns bypass invalidation", uncovered, err)
	}
	exec("SET ROLE " + role)
	var executable bool
	if err := conn.QueryRow(ctx, `SELECT has_function_privilege(current_user,'user_memory_capture_change()','EXECUTE')
 OR has_function_privilege(current_user,'user_memory_assign_revision()','EXECUTE')`).Scan(&executable); err != nil || executable {
		t.Fatal("runtime could attach the owner trigger to a caller-owned relation", executable, err)
	}
	for _, query := range []string{
		`UPDATE user_memory_collection_generation SET generation=900`,
		`DELETE FROM user_memory_collection_generation`,
		`INSERT INTO user_memory_invalidation_outbox VALUES(900,42,1,'delete',now())`,
		`DELETE FROM user_memory_invalidation_outbox`,
		`TRUNCATE user_memory_invalidation_outbox`,
	} {
		if _, err := conn.Exec(ctx, query); err == nil {
			t.Fatal("runtime may forge or erase invalidation progress", query)
		}
	}
	// A malicious temporary shadow must not redirect the security-definer
	// trigger away from its actual, migration-owned tables.
	exec(`CREATE TEMP TABLE user_memory_collection_generation(id int,generation bigint); INSERT INTO pg_temp.user_memory_collection_generation VALUES(1,900)`)
	exec("SET search_path=" + ident + ",pg_temp,public")
	exec(`INSERT INTO user_memories(id,key,content,record_revision) VALUES(42,'retained','first',900)`)
	check(1, 1)
	if _, err := conn.Exec(ctx, `UPDATE user_memories SET id=999 WHERE id=42`); err == nil {
		t.Fatal("mutable source identity would strand derivative references")
	}
	exec(`UPDATE user_memories SET use_count=use_count+1,last_used_at=now(),updated_at=now(),record_revision=900 WHERE id=42`)
	check(1, 1)
	var revision int64
	if err := conn.QueryRow(ctx, `SELECT record_revision FROM user_memories WHERE id=42`).Scan(&revision); err != nil || revision != 1 {
		t.Fatal("caller forged revision or activity changed it", revision, err)
	}
	exec(`BEGIN; UPDATE user_memories SET content='rolled back' WHERE id=42; ROLLBACK;`)
	check(1, 1)
	exec(`UPDATE user_memories SET content='second' WHERE id=42`)
	check(2, 2)
	if err := conn.Close(ctx); err != nil {
		t.Fatal(err)
	}
	restarted, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	conn = restarted
	setScope(conn)
	exec("SET ROLE " + role)

	// A committed event survives the producing connection: there is no required
	// postcommit callback and no content-bearing payload to republish.
	reader, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer reader.Close(context.Background())
	setScope(reader)
	if _, err := reader.Exec(ctx, "SET ROLE "+role); err != nil {
		t.Fatal(err)
	}
	var operation string
	if err := reader.QueryRow(ctx, `SELECT operation,record_revision FROM user_memory_invalidation_outbox WHERE generation=2`).Scan(&operation, &revision); err != nil || operation != "update" || revision != 2 {
		t.Fatal("committed change not replayable", operation, revision, err)
	}

	// Different memory IDs still share a collection commit order. A second
	// writer must not publish position N+1 while N is uncommitted.
	first, err := conn.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer first.Rollback(context.Background())
	if _, err := first.Exec(ctx, `UPDATE user_memories SET content='third' WHERE id=42`); err != nil {
		t.Fatal(err)
	}
	second, err := reader.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer second.Rollback(context.Background())
	written := make(chan error, 1)
	go func() {
		_, err := second.Exec(ctx, `INSERT INTO user_memories(id,key,content) VALUES(43,'concurrent','new')`)
		written <- err
	}()
	// Use a third connection to observe the lock rather than guessing based on
	// wall-clock sleep or sharing a connection with an in-flight operation.
	observer, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer observer.Close(context.Background())
	setScope(observer)
	if _, err := observer.Exec(ctx, "SET ROLE "+role); err != nil {
		t.Fatal(err)
	}
	for {
		var blocked bool
		if err := observer.QueryRow(ctx, `SELECT $1::integer=ANY(pg_blocking_pids($2::integer))`, conn.PgConn().PID(), reader.PgConn().PID()).Scan(&blocked); err != nil {
			t.Fatal(err)
		}
		if blocked {
			break
		}
		select {
		case err := <-written:
			t.Fatalf("second change did not wait for collection commit order: %v", err)
		case <-ctx.Done():
			t.Fatal(ctx.Err())
		case <-time.After(time.Millisecond):
		}
	}
	var visible int64
	if err := observer.QueryRow(ctx, `SELECT generation FROM user_memory_collection_generation WHERE id=1`).Scan(&visible); err != nil || visible != 2 {
		t.Fatal("uncommitted generation visible", visible, err)
	}
	if err := first.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	if err := <-written; err != nil {
		t.Fatal(err)
	}
	if err := second.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	check(4, 4)
	exec(`DELETE FROM user_memories WHERE id=42`)
	check(5, 5)
	if err := conn.QueryRow(ctx, `SELECT operation,record_revision FROM user_memory_invalidation_outbox WHERE generation=5`).Scan(&operation, &revision); err != nil || operation != "delete" || revision != 4 {
		t.Fatal("deletion failed to retain invalidation", operation, revision, err)
	}

	// Simulate an outbox/storage failure: the canonical content and collection
	// generation must both remain unchanged after the failed statement.
	exec("RESET ROLE")
	exec(`ALTER TABLE user_memory_invalidation_outbox ADD CONSTRAINT injected_failure CHECK(memory_id<>43) NOT VALID`)
	exec("SET ROLE " + role)
	if _, err := conn.Exec(ctx, `UPDATE user_memories SET content='must not commit' WHERE id=43`); err == nil {
		t.Fatal("outbox failure accepted canonical mutation")
	}
	check(5, 5)
	var content string
	if err := conn.QueryRow(ctx, `SELECT content FROM user_memories WHERE id=43`).Scan(&content); err != nil || content != "new" {
		t.Fatal("mutation survived failed invalidation", content, err)
	}

	readTx, err := reader.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer readTx.Rollback(context.Background())
	data, err := NewPostgresDataStore(evalQueryer{readTx}, PlacementServer)
	if err != nil {
		t.Fatal(err)
	}
	handler := NewHandler(nil, WithDataStore(PlacementServer, data))
	read := func(after *MemoryChangeCursor, limit int) MemoryChangePage {
		t.Helper()
		body, status := handler(bus.ModuleInvocation{StageID: StageData}, dataRequest(t, DataRequest{
			Operation: "change-feed", Changes: &MemoryChangesRequest{SchemaVersion: 1, After: after, Limit: limit},
		}))
		var response DataResponse
		if status != bus.ModuleStatusOK || json.Unmarshal(body, &response) != nil || response.Changes == nil {
			t.Fatalf("change feed status=%v response=%s", status, body)
		}
		return *response.Changes
	}
	head := read(nil, 2)
	if !head.SnapshotRequired || head.Head.Generation != 5 || head.Head.OwnerID == "" || len(head.Events) != 0 || head.More {
		t.Fatal("bootstrap feed pretended to reconstruct existing content", head)
	}
	cursor := MemoryChangeCursor{OwnerID: head.Head.OwnerID}
	for pageIndex, want := range []int{2, 2, 1} {
		page := read(&cursor, 2)
		if page.SnapshotRequired || len(page.Events) != want || page.Head != head.Head || page.More != (pageIndex < 2) {
			t.Fatal("bounded replay", pageIndex, page)
		}
		for _, event := range page.Events {
			if event.Generation != cursor.Generation+1 {
				t.Fatal("replay skipped or reordered a change", cursor, event)
			}
			cursor.Generation++
		}
		if page.Next != cursor {
			t.Fatal("wrong next position", page.Next, cursor)
		}
	}
	if page := read(&cursor, 2); page.SnapshotRequired || len(page.Events) != 0 || page.More || page.Next != cursor {
		t.Fatal("caught-up replay changed cursor", page)
	}
	for _, invalid := range []MemoryChangeCursor{{OwnerID: "another owner", Generation: 1}, {OwnerID: cursor.OwnerID, Generation: 6}} {
		if page := read(&invalid, 2); !page.SnapshotRequired || len(page.Events) != 0 || page.More {
			t.Fatal("mismatched/rewound owner accepted old progress", page)
		}
	}
	for _, peer := range []uint32{7, 23, 73} {
		body, status := handler(bus.ModuleInvocation{StageID: StageData, PrincipalRef: peer}, dataRequest(t, DataRequest{
			Operation: "change-feed", Changes: &MemoryChangesRequest{SchemaVersion: 1},
		}))
		if status != bus.ModuleStatusInvalidRequest || len(body) != 0 {
			t.Fatal("non-host consumed private record identities", peer, status, string(body))
		}
	}
	for _, invalid := range []MemoryChangesRequest{
		{SchemaVersion: 2}, {SchemaVersion: 1, Limit: 257},
		{SchemaVersion: 1, After: &MemoryChangeCursor{OwnerID: cursor.OwnerID, Generation: -1}},
		{SchemaVersion: 1, After: &MemoryChangeCursor{}},
	} {
		body, status := handler(bus.ModuleInvocation{StageID: StageData}, dataRequest(t, DataRequest{Operation: "change-feed", Changes: &invalid}))
		if status != bus.ModuleStatusInvalidRequest || len(body) != 0 {
			t.Fatal("unbounded or unsupported feed request accepted", invalid, status)
		}
	}
	exec("RESET ROLE")
	exec(`DELETE FROM user_memory_invalidation_outbox WHERE generation=3`)
	exec("SET ROLE " + role)
	cursor.Generation = 2
	if page := read(&cursor, 2); !page.SnapshotRequired || len(page.Events) != 0 || page.More {
		t.Fatal("retention gap skipped without resynchronization", page)
	}

	for _, name := range []string{"schema_personal_memory_versions.sql", "schema_personal_memory_acl.sql", "schema_personal_memory_authority.sql", "schema_personal_memory_proposals.sql"} {
		migration, err := os.ReadFile("../aimee/families/" + name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := readTx.Exec(ctx, "RESET ROLE;"+string(migration)+"SET ROLE "+role); err != nil {
			t.Fatal(err)
		}
	}
	// INSERT ... ON CONFLICT executes BEFORE INSERT even when no row will be
	// inserted. Canonical Put retries must not publish those discarded IDs.
	var before int64
	if err := readTx.QueryRow(ctx, `SELECT generation FROM user_memory_collection_generation WHERE id=1`).Scan(&before); err != nil {
		t.Fatal(err)
	}
	confidence := 0.8
	put := func(content string) int64 {
		t.Helper()
		body, status := handler(bus.ModuleInvocation{StageID: StageData}, dataRequest(t, DataRequest{
			Operation: "store", Key: "upsert-journal", Kind: "fact", Tier: "L2", Content: content, Confidence: &confidence,
		}))
		var response DataResponse
		if status != bus.ModuleStatusOK || json.Unmarshal(body, &response) != nil || len(response.Records) != 1 {
			t.Fatalf("canonical upsert status=%v response=%s", status, body)
		}
		return response.Records[0].ID
	}
	id := put("original fixture")
	if retry := put("original fixture"); retry != id {
		t.Fatal("identical upsert changed identity", id, retry)
	}
	var after int64
	if err := readTx.QueryRow(ctx, `SELECT generation FROM user_memory_collection_generation WHERE id=1`).Scan(&after); err != nil || after != before+1 {
		t.Fatal("upsert retry published a phantom insert", before, after, err)
	}
	if updated := put("corrected fixture"); updated != id {
		t.Fatal("legacy private upsert unexpectedly changed identity", id, updated)
	}
	var events, actualIDs int
	if err := readTx.QueryRow(ctx, `SELECT count(*),count(*) FILTER(WHERE memory_id=$1)
 FROM user_memory_invalidation_outbox WHERE generation>$2`, id, before).Scan(&events, &actualIDs); err != nil || events != 2 || actualIDs != 2 {
		t.Fatal("upsert captured discarded insert identities", events, actualIDs, err)
	}
}
