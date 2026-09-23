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

func TestPersonalMemoryRetainedVersions(t *testing.T) {
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
	schema := fmt.Sprintf("personal_versions_%d", time.Now().UnixNano())
	ident := pgx.Identifier{schema}.Sanitize()
	role := pgx.Identifier{schema + "_runtime"}.Sanitize()
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := conn.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec("CREATE SCHEMA " + ident + "; CREATE ROLE " + role + " NOINHERIT NOBYPASSRLS; GRANT USAGE ON SCHEMA " + ident + " TO " + role + "; ALTER DEFAULT PRIVILEGES IN SCHEMA " + ident + " GRANT ALL ON TABLES TO " + role + "; ALTER DEFAULT PRIVILEGES IN SCHEMA " + ident + " GRANT EXECUTE ON FUNCTIONS TO " + role)
	defer func() {
		cleanup, stop := context.WithTimeout(context.Background(), 5*time.Second)
		defer stop()
		if _, err := conn.Exec(cleanup, "DROP SCHEMA "+ident+" CASCADE; DROP ROLE "+role); err != nil {
			t.Error(err)
		}
	}()
	exec("SET search_path=" + ident + ",public")
	read := func(path string) string {
		t.Helper()
		b, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		return string(b)
	}
	base := read("../aimee/families/schema_conversation.sql")
	a, b := strings.Index(base, "CREATE TABLE IF NOT EXISTS user_memories ("), strings.Index(base, "CREATE INDEX IF NOT EXISTS user_memories_recall")
	if a < 0 || b <= a {
		t.Fatal("private schema missing")
	}
	exec(base[a:b] + read("../aimee/families/schema_personal_memory_changes.sql"))
	exec(`INSERT INTO user_memories(id,key,content,confidence,source_session) VALUES(42,'private','original',.9,'original-session'); INSERT INTO user_memories(id,key,content,lifecycle_state,valid_until) VALUES(43,'pending fixture','pending','pending',NULL),(44,'expired fixture','expired','active',now())`)
	exec(read("../aimee/families/schema_personal_memory_versions.sql"))
	// Older PostgreSQL restart reconciliation restored blanket grants after
	// migrations. The repair migration must remove them from existing stores.
	exec("GRANT ALL ON ALL TABLES IN SCHEMA " + ident + " TO " + role + "; GRANT EXECUTE ON ALL FUNCTIONS IN SCHEMA " + ident + " TO " + role)
	exec(read("../aimee/families/schema_personal_memory_acl.sql"))
	exec(read("../aimee/families/schema_personal_memory_authority.sql"))
	exec(read("../aimee/families/schema_personal_memory_proposals.sql"))
	scalar := func(sql string) int64 {
		t.Helper()
		var n int64
		if err := conn.QueryRow(ctx, sql).Scan(&n); err != nil {
			t.Fatal(err)
		}
		return n
	}
	if scalar(`SELECT count(*) FROM user_memory_versions`) != 0 {
		t.Fatal("migration invented history")
	}
	begin := func(c *pgx.Conn) pgx.Tx {
		t.Helper()
		tx, err := c.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := tx.Exec(ctx, "SET LOCAL search_path="+ident+",public; SET LOCAL ROLE "+role); err != nil {
			t.Fatal(err)
		}
		return tx
	}
	invoke := func(tx pgx.Tx, req DataRequest) (DataResponse, bus.ModuleStatus) {
		t.Helper()
		backend, err := NewPostgresDataStore(evalQueryer{tx}, PlacementServer)
		if err != nil {
			t.Fatal(err)
		}
		req.Authority = AuthorityUser
		raw, status := handleData(handlerOptions{placement: PlacementServer, data: backend, commandContext: &bus.CommandContext{Authenticated: true, UserAuthority: true, Principal: "fixture:user", TransportIdentity: "fixture:http"}}, bus.ModuleInvocation{StageID: StageData}, dataRequest(t, req))
		var out DataResponse
		if status == bus.ModuleStatusOK {
			if err := json.Unmarshal(raw, &out); err != nil {
				t.Fatal(err)
			}
		}
		return out, status
	}
	call := func(req DataRequest) (DataResponse, bus.ModuleStatus) {
		t.Helper()
		tx := begin(conn)
		defer tx.Rollback(context.Background())
		out, status := invoke(tx, req)
		if status == bus.ModuleStatusOK {
			if err := tx.Commit(ctx); err != nil {
				t.Fatal(err)
			}
		}
		return out, status
	}
	get := func() Record {
		t.Helper()
		r, s := call(DataRequest{Operation: "get", ID: 42, IncludeVersion: true})
		if s != bus.ModuleStatusOK || len(r.Records) != 1 || r.Records[0].Version == nil {
			t.Fatal(s, r)
		}
		return r.Records[0]
	}
	assertRecall := func(want Record) {
		t.Helper()
		tx := begin(conn)
		defer tx.Rollback(context.Background())
		backend := &postgresDataStore{db: evalQueryer{tx}, placement: PlacementServer}
		rows, err := backend.recallRecords(ctx, "id=42", 1)
		if err != nil || len(rows) != 1 || rows[0].Version == nil || *rows[0].Version != *want.Version || rows[0].Content != want.Content {
			t.Fatalf("private recall payload/revision: %+v %v", rows, err)
		}
	}
	checkRelease := func(version *MemoryRecordVersion, channel string, want bool) {
		t.Helper()
		tx := begin(conn)
		defer tx.Rollback(context.Background())
		backend := &postgresDataStore{db: evalQueryer{tx}, placement: PlacementServer}
		request := &sourceRevalidation{SchemaVersion: 1, CheckID: strings.Repeat("a", 32), Sources: []typedProjectionRef{{Channel: channel, ID: version.RecordID, Source: &typedSourceVersion{Kind: "user_memory_record", Version: *version, MemoryParentState: "observed"}}}}
		eligible, err := backend.revalidatePersonalSources(ctx, request)
		if err != nil || eligible != want {
			t.Fatalf("private release eligible=%v want=%v err=%v", eligible, want, err)
		}
		args := commandArgs{}
		args["revalidation"], _ = json.Marshal(request)
		options := handlerOptions{placement: PlacementServer, data: backend}
		encoded, status := handlePersonalSourceRevalidation(options, bus.ModuleInvocation{}, args)
		raw, err := bus.DecodeCommandResult(encoded)
		var reply struct {
			Status   string
			Eligible bool
			CheckID  string `json:"check_id"`
			Digest   string `json:"sources_digest"`
		}
		if status != bus.ModuleStatusOK || err != nil || json.Unmarshal(raw, &reply) != nil || reply.Status != "ok" || reply.Eligible != want || reply.CheckID != request.CheckID || reply.Digest != releaseDigest(request.Sources) {
			t.Fatalf("private owner answer: %s %v %v", raw, status, err)
		}
		if _, status := handlePersonalSourceRevalidation(options, bus.ModuleInvocation{PrincipalRef: 1}, args); status != bus.ModuleStatusCapabilityAbsent {
			t.Fatal("public caller reached private revalidation", status)
		}
		options.placement = PlacementKB
		if _, status := handlePersonalSourceRevalidation(options, bus.ModuleInvocation{}, args); status != bus.ModuleStatusCapabilityAbsent {
			t.Fatal("shared owner reached private revalidation", status)
		}
	}
	for _, fixture := range []struct {
		id      int64
		channel string
		want    bool
	}{{43, "native_preferences", false}, {43, "native_open_commitments", true}, {44, "native_preferences", false}} {
		version := MemoryRecordVersion{SchemaVersion: 1, RecordID: fmt.Sprint(fixture.id)}
		if err := conn.QueryRow(ctx, "SELECT record_revision::text,(SELECT owner_id::text FROM user_memory_collection_generation WHERE id=1) FROM user_memories WHERE id=$1", fixture.id).Scan(&version.RecordRevision, &version.OwnerID); err != nil {
			t.Fatal(err)
		}
		checkRelease(&version, fixture.channel, fixture.want)
	}
	original := get()
	assertRecall(original)
	checkRelease(original.Version, "native_preferences", true)
	checkRelease(original.Version, "native_open_commitments", false)
	if original.Version.RecordRevision != "1" {
		t.Fatal(original)
	}
	confidence := .7
	stored, status := call(DataRequest{Operation: "store", Key: "private", Kind: "fact", Tier: "L2", Content: "corrected", Confidence: &confidence})
	if status != bus.ModuleStatusOK || len(stored.Records) != 1 || stored.Records[0].ID != 42 {
		t.Fatal(status, stored)
	}
	current := get()
	assertRecall(current)
	checkRelease(original.Version, "native_preferences", false)
	checkRelease(current.Version, "native_preferences", true)
	if current.Version.RecordRevision != "2" || current.Content != "corrected" {
		t.Fatal(current)
	}
	prior, status := call(DataRequest{Operation: "get", ID: 42, AtVersion: original.Version})
	if status != bus.ModuleStatusOK || len(prior.Records) != 1 || prior.Records[0].Content != "original" || !prior.Records[0].Historical || *prior.Records[0].Version != *original.Version {
		t.Fatal(status, prior)
	}
	if scalar(`SELECT count(*) FROM user_memory_versions WHERE record->>'source_session'='original-session' AND record->>'provenance_category'='unknown'`) != 1 {
		t.Fatal("lost private metadata or invented legacy authorship")
	}
	wrongOwner := *original.Version
	wrongOwner.OwnerID = "00000000-0000-0000-0000-000000000001"
	checkRelease(&wrongOwner, "native_preferences", false)
	if out, status := call(DataRequest{Operation: "get", ID: 42, AtVersion: &wrongOwner}); status != bus.ModuleStatusOK || len(out.Records) != 0 {
		t.Fatal("wrong owner history", status, out)
	}
	correction := DataRequest{Operation: "supersede", ID: 42, Content: "third", Confidence: &confidence, ExpectedVersion: original.Version}
	if out, status := call(correction); status != bus.ModuleStatusOK || out.Code == nil || *out.Code != MutationVersionConflict {
		t.Fatal("stale correction", status, out)
	}
	correction.ExpectedVersion = current.Version
	changed, status := call(correction)
	if status != bus.ModuleStatusOK || len(changed.Records) != 1 || changed.Records[0].Version.RecordRevision != "3" {
		t.Fatal(status, changed)
	}
	current = get()
	// Identical upserts and read accounting do not allocate history or positions.
	generation := scalar(`SELECT generation FROM user_memory_collection_generation`)
	exec(`UPDATE user_memories SET use_count=use_count+1,last_used_at=now(),updated_at=now() WHERE id=42`)
	if _, status := call(DataRequest{Operation: "store", Key: "private", Kind: "fact", Tier: "L2", Content: "third", Confidence: &confidence}); status != bus.ModuleStatusOK {
		t.Fatal(status)
	}
	if scalar(`SELECT count(*) FROM user_memory_versions`) != 2 || scalar(`SELECT generation FROM user_memory_collection_generation`) != generation {
		t.Fatal("read/no-op allocated history")
	}
	// Table-owner failure after the row/outbox writes must roll everything back.
	exec(`ALTER TABLE user_memory_versions ADD CONSTRAINT fixture_failure CHECK(record->>'content'<>'third')`)
	correction.ExpectedVersion = current.Version
	correction.Content = "must roll back"
	if _, status := call(correction); status != bus.ModuleStatusInternal {
		t.Fatal("late history failure accepted", status)
	}
	if get().Content != "third" || scalar(`SELECT generation FROM user_memory_collection_generation`) != generation || scalar(`SELECT count(*) FROM user_memory_versions`) != 2 {
		t.Fatal("history failure partially committed")
	}
	exec(`ALTER TABLE user_memory_versions DROP CONSTRAINT fixture_failure`)
	for _, sql := range []string{
		`INSERT INTO user_memory_versions(memory_id,record_revision,record) VALUES(42,99,'{}')`,
		`UPDATE user_memory_versions SET record='{}'`, `DELETE FROM user_memory_versions`, `TRUNCATE user_memory_versions`,
	} {
		tx := begin(conn)
		_, err := tx.Exec(ctx, sql)
		_ = tx.Rollback(ctx)
		if err == nil {
			t.Fatal("runtime rewrote history", sql)
		}
	}
	// A same-name temporary table cannot intercept the privileged history write.
	exec(`CREATE TEMP TABLE user_memory_versions(memory_id BIGINT,record_revision BIGINT,record JSONB)`)
	exec(`SELECT set_config('aimee.private_authority','user',false),set_config('aimee.private_principal','fixture:admin',false); UPDATE ` + ident + `.user_memories SET content='legacy writer' WHERE id=42`)
	if scalar(`SELECT count(*) FROM pg_temp.user_memory_versions`) != 0 {
		t.Fatal("history captured into runtime shadow")
	}
	exec(`DROP TABLE pg_temp.user_memory_versions`)
	if scalar(`SELECT count(*) FROM user_memory_versions`) != 3 {
		t.Fatal("legacy writer failed to retain history")
	}
	current = get()
	correction.ExpectedVersion = current.Version
	correction.Content = "race winner"
	secondConn, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer secondConn.Close(context.Background())
	observer, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer observer.Close(context.Background())
	first, second := begin(conn), begin(secondConn)
	defer first.Rollback(context.Background())
	defer second.Rollback(context.Background())
	winner, status := invoke(first, correction)
	if status != bus.ModuleStatusOK || len(winner.Records) != 1 {
		t.Fatal(status, winner)
	}
	type result struct {
		out    DataResponse
		status bus.ModuleStatus
	}
	done := make(chan result, 1)
	go func() { out, status := invoke(second, correction); done <- result{out, status} }()
	for {
		var blocked bool
		if err := observer.QueryRow(ctx, `SELECT $1=ANY(pg_blocking_pids($2))`, conn.PgConn().PID(), secondConn.PgConn().PID()).Scan(&blocked); err != nil {
			t.Fatal(err)
		}
		if blocked {
			break
		}
		select {
		case result := <-done:
			t.Fatal("competing correction did not wait", result)
		case <-ctx.Done():
			t.Fatal(ctx.Err())
		case <-time.After(time.Millisecond):
		}
	}
	if err := first.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	loser := <-done
	if loser.status != bus.ModuleStatusOK || loser.out.Code == nil || *loser.out.Code != MutationVersionConflict {
		t.Fatal("competing stale writer accepted", loser)
	}
	if err := second.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	if scalar(`SELECT count(*) FROM user_memory_versions`) != 4 || get().Content != "race winner" {
		t.Fatal("competing correction wrote extra history")
	}
	// Current revocation gates all old payloads. Retirement permits explicit
	// historical inspection, but ordinary recall remains current-only.
	beforeRevocation := get()
	exec(`UPDATE user_memories SET lifecycle_state='rejected' WHERE id=42`)
	checkRelease(beforeRevocation.Version, "native_preferences", false)
	if out, status := call(DataRequest{Operation: "get", ID: 42, AtVersion: original.Version}); status != bus.ModuleStatusOK || len(out.Records) != 0 {
		t.Fatal("revoked parent released history", status, out)
	}
	exec(`ALTER TABLE user_memories DISABLE TRIGGER user_memory_00_authority; UPDATE user_memories SET lifecycle_state='retired' WHERE id=42; ALTER TABLE user_memories ENABLE TRIGGER user_memory_00_authority`)
	if out, status := call(DataRequest{Operation: "get", ID: 42}); status != bus.ModuleStatusOK || len(out.Records) != 0 {
		t.Fatal("retired current read", status, out)
	}
	if out, status := call(DataRequest{Operation: "get", ID: 42, AtVersion: original.Version}); status != bus.ModuleStatusOK || len(out.Records) != 1 {
		t.Fatal("retained revision missing", status, out)
	}
	exec(`DELETE FROM user_memories WHERE id=42`)
	checkRelease(beforeRevocation.Version, "native_preferences", false)
	if scalar(`SELECT count(*) FROM user_memory_versions`) != 0 {
		t.Fatal("parent erasure retained private payload")
	}
	if out, status := call(DataRequest{Operation: "get", ID: 42, AtVersion: original.Version}); status != bus.ModuleStatusOK || len(out.Records) != 0 {
		t.Fatal("erased version released", status, out)
	}
}
