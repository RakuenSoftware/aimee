package memory

import (
	"context"
	"encoding/json"
	"os"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5"
)

func TestCollectionEmptyRecallInvalidatedPostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_DB2_REPLAY_URL")
	if dsn == "" {
		t.Skip("set AIMEE_DB2_REPLAY_URL")
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
	exec := func(q string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, q, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SET LOCAL jit=off; DO $$ BEGIN IF NOT EXISTS(SELECT FROM pg_roles WHERE rolname='aimee_store_runtime') THEN CREATE ROLE aimee_store_runtime NOINHERIT NOBYPASSRLS; END IF; END $$; GRANT USAGE ON SCHEMA public TO aimee_store_runtime`)
	schema, err := os.ReadFile("../../../src/modules/db2/c/schema.sql")
	if err != nil {
		t.Fatal(err)
	}
	start, end := strings.Index(string(schema), "DO $memory_store_grants$"), strings.Index(string(schema), "END\n$memory_store_grants$;")
	if start < 0 || end < start {
		t.Fatal("runtime grant migration missing")
	}
	exec(string(schema[start : end+len("END\n$memory_store_grants$;")]))
	exec(`DELETE FROM memories; DELETE FROM rules; DELETE FROM prospective_memories; DELETE FROM epistemic_directives;
 SELECT set_config('aimee.memory_scope_all','0',true),set_config('aimee.memory_scope_type','project',true),set_config('aimee.memory_scope_value','mr04-visible',true),set_config('aimee.memory_project','mr04-visible',true),set_config('aimee.memory_workspace','',true);
 SET LOCAL ROLE aimee_store_runtime`)
	backend := &postgresDataStore{db: evalQueryer{tx}, placement: PlacementKB}
	raw, err := backend.RecallBundle(ctx, "deployment", 8192, false)
	if err != nil {
		t.Fatal(err)
	}
	var bundle recallBundle
	if json.Unmarshal(raw, &bundle) != nil || bundle.CollectionSource == nil {
		t.Fatal("empty recall lost collection observation")
	}
	projection, _, err := projectNativeRecall(bundle, 32768)
	if err != nil || projection.Text != "" || len(projection.Sources) != 2 {
		t.Fatal("expected an empty protected view", projection, err)
	}
	request := &sourceRevalidation{SchemaVersion: 1, CheckID: strings.Repeat("c", 32), Sources: projection.Sources}
	check := func(want bool) {
		t.Helper()
		ok, err := backend.revalidateSources(ctx, request, Scope{Type: ScopeProject, Value: "mr04-visible"})
		if err != nil || ok != want {
			t.Fatal("collection revalidation", ok, want, err)
		}
	}
	check(true)
	exec(`SAVEPOINT hidden_insert; RESET ROLE;
 INSERT INTO memories(tier,kind,key,content,scope_type,scope_value,valid_from) VALUES('L2','fact','hidden constraint','hidden contradiction','project','mr04-hidden',(CURRENT_TIMESTAMP+interval '1 second')::text);
 SET LOCAL ROLE aimee_store_runtime`)
	check(true)
	afterHidden, err := backend.observeRecallCollection(ctx)
	if err != nil {
		t.Fatal(err)
	}
	before, _ := json.Marshal(bundle.CollectionSource)
	after, _ := json.Marshal(afterHidden)
	if string(before) != string(after) {
		t.Fatal("hidden collection changed public observation", string(before), string(after))
	}
	exec(`ROLLBACK TO hidden_insert; RELEASE hidden_insert`)
	exec(`SAVEPOINT visible_insert; RESET ROLE;
 INSERT INTO memories(tier,kind,key,content,scope_type,scope_value) VALUES('L2','fact','deployment constraint','Deployment is prohibited until review','project','mr04-visible');
 SET LOCAL ROLE aimee_store_runtime`)
	check(false)
	exec(`ROLLBACK TO visible_insert; RELEASE visible_insert`)
	check(true)
	// Equal numeric heads from another audience must not validate this view.
	exec(`SELECT set_config('aimee.memory_scope_value','mr04-other',true),set_config('aimee.memory_project','mr04-other',true)`)
	check(false)
	// Retained canonical text also keeps its transitive input fence when an
	// older cached projection has no collection observation.
	exec(`SELECT set_config('aimee.memory_scope_value','mr04-visible',true),set_config('aimee.memory_project','mr04-visible',true); RESET ROLE`)
	ids := make([]int64, 3)
	for i, key := range []string{"original source", "intermediate claim", "cached descendant"} {
		if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value)
 VALUES('L2','fact',$1,$1,'project','mr04-visible') RETURNING id`, key).Scan(&ids[i]); err != nil {
			t.Fatal(err)
		}
	}
	for i := 1; i < len(ids); i++ {
		exec(`INSERT INTO memory_lineage(object_type,object_id,source_kind,source_ref)
 SELECT 'memory',child.id,'memory-cognify-input-v1',jsonb_build_object('schema_version',1,
 'owner_id',o.owner_id::text,'record_id',parent.id::text,'record_revision',parent.record_revision::text,
 'derived_revision',child.record_revision::text)::text FROM memories child,memories parent,memory_collection_owner o
 WHERE child.id=$1 AND parent.id=$2 AND o.id=1`, ids[i], ids[i-1])
	}
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	raw, err = backend.RecallBundle(ctx, "cached descendant", 8192, false)
	if err != nil || json.Unmarshal(raw, &bundle) != nil {
		t.Fatal("cached recall", err)
	}
	projection, _, err = projectNativeRecall(bundle, 32768)
	if err != nil || !strings.Contains(projection.Text, "cached descendant") {
		t.Fatal("missing cached text", err)
	}
	retained := []typedProjectionRef{}
	for _, ref := range projection.Sources {
		if ref.Source.Kind == "memory_record" {
			retained = append(retained, ref)
		}
	}
	if len(retained) != 1 {
		t.Fatal("expected one selected descendant", retained)
	}
	request.Sources = retained
	check(true)
	activationCheck := func(want int) {
		t.Helper()
		rows, _, _, err := backend.recallActivated(ctx, &ActivationSnapshot{CurrentTurn: 3, Rows: []ActivationRow{}}, "m.key='cached descendant'", 1, false, false)
		if err != nil || len(rows) != want {
			t.Fatal("activated descendant", len(rows), want, err)
		}
	}
	recallCheck := func(want int) {
		t.Helper()
		rows, err := backend.recallRecords(ctx, "key='cached descendant'", 1)
		if err != nil || len(rows) != want {
			t.Fatal("ordinary descendant recall", len(rows), want, err)
		}
		decision, err := backend.validity(ctx, ids[2], &MemoryReadResult{Mode: "current"})
		if err != nil || decision.Eligible != (want == 1) {
			t.Fatal("descendant validity", decision, err)
		}
		if want == 0 && !strings.Contains(strings.Join(decision.ReasonCodes, ","), "derived_inputs_unavailable") {
			t.Fatal("missing lineage refusal reason", decision)
		}
	}
	recallCheck(1)
	activationCheck(1)
	exec(`RESET ROLE`)
	exec(`UPDATE memories SET lifecycle_state='revoked' WHERE id=$1`, ids[0])
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	check(false)
	activationCheck(0)
	recallCheck(0)
	var unchanged bool
	if err := tx.QueryRow(ctx, `SELECT record_revision::text=$2 AND content='cached descendant' FROM memories WHERE id=$1`, ids[2], retained[0].Source.Version.RecordRevision).Scan(&unchanged); err != nil || !unchanged {
		t.Fatal("test changed the cached descendant instead of its ancestor", unchanged, err)
	}

	// A future constraint can become applicable during the provider-send lease
	// even when no stored revision or collection counter changes.
	exec(`RESET ROLE`)
	exec(`INSERT INTO memories(tier,kind,key,content,scope_type,scope_value,valid_from)
 VALUES('L2','fact','future deployment constraint','requires review','project','mr04-visible',(CURRENT_TIMESTAMP+interval '2 seconds')::text)`)
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	future, err := backend.observeRecallCollection(ctx)
	if err != nil {
		t.Fatal(err)
	}
	request.Sources = []typedProjectionRef{{Channel: "native_memory_collection", ID: "1", Source: future}}
	check(true)
	request.SendGuard = "acquire"
	check(false)

}

func TestPrivateCollectionEmptyRecallInvalidatedPostgres(t *testing.T) {
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
	exec := func(q string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, q, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SET LOCAL jit=off; CREATE SCHEMA private_collection_test; CREATE ROLE private_collection_test NOINHERIT NOBYPASSRLS; SET LOCAL search_path=private_collection_test,public`)
	read := func(name string) string {
		t.Helper()
		raw, err := os.ReadFile("../aimee/families/" + name)
		if err != nil {
			t.Fatal(err)
		}
		return string(raw)
	}
	base := read("schema_conversation.sql")
	start, end := strings.Index(base, "CREATE TABLE IF NOT EXISTS user_memories ("), strings.Index(base, "CREATE INDEX IF NOT EXISTS user_memories_recall")
	if start < 0 || end < start {
		t.Fatal("private memory schema missing")
	}
	exec(base[start:end] + read("schema_personal_memory_changes.sql"))
	exec(`GRANT USAGE ON SCHEMA private_collection_test TO private_collection_test; GRANT SELECT ON ALL TABLES IN SCHEMA private_collection_test TO private_collection_test; SET LOCAL ROLE private_collection_test`)
	backend := &postgresDataStore{db: evalQueryer{tx}, placement: PlacementServer}
	raw, err := backend.RecallBundle(ctx, "deployment", 8192, false)
	if err != nil {
		t.Fatal(err)
	}
	var bundle recallBundle
	if json.Unmarshal(raw, &bundle) != nil || bundle.CollectionSource == nil {
		t.Fatal("empty private recall lost observation")
	}
	projection, _, err := projectNativeRecall(bundle, 32768)
	if err != nil || projection.Text != "" || len(projection.Sources) != 1 || projection.Sources[0].Source.Kind != "user_memory_collection" {
		t.Fatal("empty private view", projection, err)
	}
	request := &sourceRevalidation{SchemaVersion: 1, CheckID: strings.Repeat("d", 32), Sources: projection.Sources}
	if ok, err := backend.revalidatePersonalSources(ctx, request); err != nil || !ok {
		t.Fatal("fresh private collection", ok, err)
	}
	exec(`RESET ROLE; INSERT INTO user_memories(key,content) VALUES('deployment constraint','Deployment requires review'); SET LOCAL ROLE private_collection_test`)
	if ok, err := backend.revalidatePersonalSources(ctx, request); err != nil || ok {
		t.Fatal("new private constraint missed", ok, err)
	}
	exec(`RESET ROLE; UPDATE user_memories SET valid_until=CURRENT_TIMESTAMP+interval '2 seconds'; SET LOCAL ROLE private_collection_test`)
	future, err := backend.observeRecallCollection(ctx)
	if err != nil {
		t.Fatal(err)
	}
	request.Sources = []typedProjectionRef{{Channel: "native_memory_collection", ID: "1", Source: future}}
	if ok, err := backend.revalidatePersonalSources(ctx, request); err != nil || !ok {
		t.Fatal("fresh expiring private collection", ok, err)
	}
	request.SendGuard = "acquire"
	if ok, err := backend.revalidatePersonalSources(ctx, request); err != nil || ok {
		t.Fatal("private collection outlived expiry during send", ok, err)
	}

}

func TestCollectionDeadlineSyntax(t *testing.T) {
	for _, value := range []string{"", "2026-09-25T12:00:00Z", "2026-09-25T12:00:00.123456Z"} {
		if !validCollectionDeadline(value) {
			t.Fatal("valid deadline rejected", value)
		}
	}
	for _, value := range []string{"now", "infinity", "2026-09-25", "2026-09-25T12:00:00+01:00", "0000-01-01T00:00:00Z", "2026-99-25T12:00:00Z"} {
		if validCollectionDeadline(value) {
			t.Fatal("invalid deadline accepted", value)
		}
	}
}
