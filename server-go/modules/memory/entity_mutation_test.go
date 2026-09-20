package memory

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"strconv"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/audit"
	"github.com/jackc/pgx/v5"
)

func TestEntityMutationBoundary(t *testing.T) {
	operator := bus.CommandContext{Authenticated: true, UserAuthority: true, Principal: "operator:test"}
	handler := NewHandler(nil, WithDataStore(PlacementKB, nil))
	for _, op := range []string{"entity-review", "entity-mutate"} {
		args := `{"operation":"` + op + `","action":"merge","from_id":1,"into_id":2}`
		if _, status := invokeContextCommand(t, handler, 73, operator, "runtime", args); status != bus.ModuleStatusInvalidRequest {
			t.Fatal(status)
		}
		if _, status := invokeContextCommand(t, NewHandler(nil, WithDataStore(PlacementServer, nil)), 0, operator, "runtime", args); status != bus.ModuleStatusCapabilityAbsent {
			t.Fatal(status)
		}
	}
	for _, caller := range []bus.CommandContext{{}, {Authenticated: true, Principal: "user"}} {
		r, status := invokeContextCommand(t, handler, 0, caller, "runtime", `{"operation":"entity-review","action":"merge","from_id":1,"into_id":2,"actor":{"rank":40}}`)
		if status != bus.ModuleStatusOK || r["http_status"] != float64(403) {
			t.Fatal(r, status)
		}
	}
	for _, id := range []string{`0`, `-1`, `1.5`, `9007199254740992`, `true`, `null`, `"+1"`, `"01"`, `"0"`, `"-1"`, `"9223372036854775808"`, `"1.5"`} {
		r, status := invokeContextCommand(t, handler, 0, operator, "runtime", `{"operation":"entity-review","action":"merge","from_id":`+id+`,"into_id":2}`)
		if status != bus.ModuleStatusOK || r["http_status"] != float64(400) {
			t.Fatal(id, r, status)
		}
	}
	for _, body := range []string{`{"operation":"entity-review","action":"merge","from_id":1,"into_id":1}`, `{"operation":"entity-mutate","action":"unmerge"}`, `{"operation":"entity-mutate","action":"delete"}`} {
		r, status := invokeContextCommand(t, handler, 0, operator, "runtime", body)
		if status != bus.ModuleStatusOK || r["http_status"] != float64(400) {
			t.Fatal(body, r, status)
		}
	}
}

func exerciseEntityMutationReplay(t *testing.T, ctx context.Context, tx pgx.Tx, backend *postgresDataStore) {
	t.Helper()
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SAVEPOINT entity_mutation_replay`)
	defer func() { exec(`ROLLBACK TO SAVEPOINT entity_mutation_replay; RELEASE SAVEPOINT entity_mutation_replay`) }()
	exec(`RESET ROLE; SELECT set_config('aimee.memory_scope_all','1',true)`)
	for i, name := range []string{"Entity X", "Entity Y", "Entity Z", "Entity Unknown Kind"} {
		id := int64(9007199254740993 + 2*i)
		exec(`INSERT INTO entity_registry(canonical_id,kind,status) VALUES($1,2,'active')`, id)
		exec(`INSERT INTO entity_aliases(name,name_norm,canonical_id,is_preferred) VALUES($1,$2,$3,1)`, name, entityAliasName(name), id)
	}
	// Override only this transaction's inserted merge ID; don't advance a shared
	// sequence to an unsafe range or pollute subsequent replay fixtures.
	exec(`CREATE FUNCTION pg_temp.entity_large_id() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN SELECT COALESCE(max(id),9007199254740992)+1 INTO NEW.id FROM entity_merges; RETURN NEW; END $$; CREATE TRIGGER entity_large_id BEFORE INSERT ON entity_merges FOR EACH ROW EXECUTE FUNCTION pg_temp.entity_large_id(); SET LOCAL ROLE aimee_store_runtime`)
	var observations []audit.Action
	observed := *backend
	observed.auditAction = func(_ context.Context, a audit.Action) error { observations = append(observations, a); return nil }
	handler := NewHandler(nil, WithDataStore(PlacementKB, &observed))
	operator := bus.CommandContext{Authenticated: true, UserAuthority: true, Principal: "operator:entity", TransportIdentity: "verified:console"}
	call := func(op, action string, from, into, mid int64, want int) map[string]json.RawMessage {
		t.Helper()
		args, _ := json.Marshal(map[string]any{"operation": op, "action": action, "from_id": strconv.FormatInt(from, 10), "into_id": strconv.FormatInt(into, 10), "merge_id": strconv.FormatInt(mid, 10), "actor": map[string]any{"rank": 40, "principal": "forged"}})
		out, status := invokeContextCommand(t, handler, 0, operator, "runtime", string(args))
		if status != bus.ModuleStatusOK || out["http_status"] != float64(want) {
			t.Fatalf("%s: %v %v", args, out, status)
		}
		var result map[string]json.RawMessage
		if err := json.Unmarshal([]byte(out["json"].(string)), &result); err != nil {
			t.Fatal(err)
		}
		return result
	}
	idOf := func(r map[string]json.RawMessage) int64 {
		t.Helper()
		var id int64
		if err := json.Unmarshal(r["merge_id"], &id); err != nil {
			t.Fatal(err)
		}
		return id
	}
	resolve := func(name string) string {
		t.Helper()
		s := *backend
		s.db = runtimeRoleTx{evalQueryer{tx}, t}
		out, err := s.canonicalFactEndpoint(ctx, name, NodeDevice)
		if err != nil {
			t.Fatal(err)
		}
		return out
	}
	x, y, z := int64(9007199254740993), int64(9007199254740995), int64(9007199254740997)
	first := call("entity-review", "merge", x, y, 0, 200)
	mxy := idOf(first)
	if mxy != 9007199254740993 || resolve("ENTITY X") != "Entity Y" {
		t.Fatal(mxy)
	}
	call("entity-review", "merge", y, x, 0, 409)
	call("entity-review", "merge", x, z, 0, 409)
	call("entity-review", "merge", z, 12345678, 0, 409)
	second := call("entity-mutate", "merge", y, z, 0, 200)
	myz := idOf(second)
	if resolve("Entity X") != "Entity Y" || resolve("Entity Y") != "Entity Z" {
		t.Fatal("changed single-hop resolution")
	}
	call("entity-review", "unmerge", 0, 0, mxy, 200)
	if resolve("Entity X") != "Entity X" {
		t.Fatal("source not restored after target merged")
	}
	call("entity-review", "unmerge", 0, 0, mxy, 409)
	call("entity-mutate", "unmerge", 0, 0, mxy, 404)
	call("entity-mutate", "unmerge", 0, 0, 12345678, 404)
	call("entity-mutate", "unmerge", 0, 0, myz, 200)
	// Historical merge IDs cannot undo a later merge of the same entity.
	newest := idOf(call("entity-review", "merge", x, z, 0, 200))
	call("entity-review", "unmerge", 0, 0, mxy, 409)
	if resolve("Entity X") != "Entity Z" {
		t.Fatal("stale undo replaced new merge")
	}
	// A still-live audit record whose registry no longer matches also refuses.
	exec(`RESET ROLE`)
	exec(`UPDATE entity_registry SET merged_into=$2 WHERE canonical_id=$1`, x, y)
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	call("entity-review", "unmerge", 0, 0, newest, 409)
	exec(`RESET ROLE`)
	exec(`UPDATE entity_registry SET merged_into=$2 WHERE canonical_id=$1`, x, z)
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	call("entity-review", "unmerge", 0, 0, newest, 200)
	var forbidden bool
	if err := tx.QueryRow(ctx, `SELECT has_table_privilege(current_user,'entity_merges','DELETE') OR has_column_privilege(current_user,'entity_merges','from_id','UPDATE') OR has_column_privilege(current_user,'entity_merges','created_at','SELECT') OR has_column_privilege(current_user,'entity_merges','undone','INSERT')`).Scan(&forbidden); err != nil || forbidden {
		t.Fatal(forbidden, err)
	}
	// Audit identity cannot be nominated by either public or console payloads.
	var wrong int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM fact_graph_commits c JOIN fact_graph_changes ch USING(commit_id) WHERE c.operation IN ('entity.merge','entity.unmerge') AND (c.status<>'applied' OR c.actor_principal='forged' OR ch.object_kind<>'entity_merge' OR ch.assertion_id<>0 OR ch.diff_detail<>'external graph transition' OR ch.existed_before<>1 OR ch.existed_after<>1 OR ch.action<>split_part(c.operation,'.',2) OR ch.before_lifecycle<>CASE WHEN ch.action='merge' THEN 'active' ELSE 'merged' END OR ch.after_lifecycle<>CASE WHEN ch.action='merge' THEN 'merged' ELSE 'active' END)`).Scan(&wrong); err != nil || wrong != 0 {
		t.Fatal("invalid graph records", wrong, err)
	}
	var principal, role string
	var rank int
	var publicCommit string
	json.Unmarshal(second["commit_id"], &publicCommit)
	if err := tx.QueryRow(ctx, `SELECT actor_principal,actor_role,authority_rank FROM fact_graph_commits WHERE commit_id=$1`, publicCommit).Scan(&principal, &role, &rank); err != nil || principal != "system:kb-maintenance" || role != "system" || rank != 20 {
		t.Fatal(principal, role, rank, err)
	}
	exec(`RESET ROLE`)
	var seals, commits int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM fact_graph_commits WHERE operation IN ('entity.merge','entity.unmerge')`).Scan(&commits); err != nil || commits != 6 {
		t.Fatal("commit count", commits, err)
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM fact_graph_commits c JOIN fact_graph_changes ch USING(commit_id) JOIN kb_audit_outbox a ON a.detail='commit_id='||c.commit_id AND a.subject=ch.object_key AND a.action=c.operation AND a.actor_principal=c.actor_principal WHERE c.operation IN ('entity.merge','entity.unmerge')`).Scan(&seals); err != nil || seals != commits {
		t.Fatal("seal count", seals, commits, err)
	}
	exec(`CREATE FUNCTION pg_temp.entity_fail_seal() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN IF NEW.operation IN ('entity.merge','entity.unmerge') AND NEW.status='applied' THEN RAISE EXCEPTION 'entity late failure'; END IF; RETURN NEW; END $$; CREATE TRIGGER entity_fail_seal BEFORE UPDATE ON fact_graph_commits FOR EACH ROW EXECUTE FUNCTION pg_temp.entity_fail_seal(); SET LOCAL ROLE aimee_store_runtime`)
	call("entity-review", "merge", x, y, 0, 503)
	if resolve("Entity X") != "Entity X" {
		t.Fatal("failed seal changed registry")
	}
	var after int
	tx.QueryRow(ctx, `SELECT count(*) FROM fact_graph_commits WHERE operation IN ('entity.merge','entity.unmerge')`).Scan(&after)
	if after != commits {
		t.Fatal(after, commits)
	}
	exec(`RESET ROLE; DROP TRIGGER entity_fail_seal ON fact_graph_commits; SET LOCAL ROLE aimee_store_runtime`)
	final := idOf(call("entity-review", "merge", x, y, 0, 200))
	exec(`RESET ROLE; REVOKE UPDATE(undone) ON entity_merges FROM aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`)
	call("entity-review", "unmerge", 0, 0, final, 503)
	if resolve("Entity X") != "Entity Y" {
		t.Fatal("failed undo partially restored registry")
	}
	if len(observations) == 0 || observations[len(observations)-1].Tool != "entities.unmerge" || observations[len(observations)-1].Verdict != "fail" {
		t.Fatal(observations)
	}
	t.Log(fmt.Sprintf("entity mutation replay: %d observations", len(observations)))
}

// Exercise two real owner calls on separate connections without committing any
// fixture rows. The first transaction's rollback releases the mutation lock;
// the waiting call must then re-read endpoints and refuse the absent entities.
// The sequential replay above covers committed A->B / B->A cycle rejection.
func TestEntityMutationConcurrentPostgres(t *testing.T) {
	url := os.Getenv("AIMEE_DB2_REPLAY_URL")
	if url == "" {
		t.Skip("set AIMEE_DB2_REPLAY_URL")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	aConn, err := pgx.Connect(ctx, url)
	if err != nil {
		t.Fatal(err)
	}
	defer aConn.Close(context.Background())
	bConn, err := pgx.Connect(ctx, url)
	if err != nil {
		t.Fatal(err)
	}
	defer bConn.Close(context.Background())
	a, err := aConn.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer a.Rollback(context.Background())
	b, err := bConn.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer b.Rollback(context.Background())
	var from, into int64
	if err = a.QueryRow(ctx, `INSERT INTO entity_registry(kind,status) VALUES(2,'active') RETURNING canonical_id`).Scan(&from); err != nil {
		t.Fatal(err)
	}
	if err = a.QueryRow(ctx, `INSERT INTO entity_registry(kind,status) VALUES(2,'active') RETURNING canonical_id`).Scan(&into); err != nil {
		t.Fatal(err)
	}
	actor := FactActor{Principal: "operator:concurrency", TransportIdentity: "fixture", Role: "operator", Rank: 40, Authenticated: 1}
	owner := &postgresDataStore{db: evalQueryer{a}, placement: PlacementKB}
	if _, err = owner.mutateEntity(ctx, actor, "merge", from, into, 0); err != nil {
		t.Fatal(err)
	}
	done := make(chan error, 1)
	go func() {
		_, err := (&postgresDataStore{db: evalQueryer{b}, placement: PlacementKB}).mutateEntity(ctx, actor, "merge", into, from, 0)
		done <- err
	}()
	// Confirm the second backend is actually waiting on the shared lock; elapsed
	// time alone must not be mistaken for evidence that it reached the mutation.
	waiting := false
	for !waiting {
		if err = a.QueryRow(ctx, `SELECT EXISTS(SELECT 1 FROM pg_locks WHERE pid=$1 AND locktype='advisory' AND NOT granted)`, int64(bConn.PgConn().PID())).Scan(&waiting); err != nil {
			t.Fatal(err)
		}
		if !waiting {
			select {
			case err := <-done:
				t.Fatal("owner bypassed open mutation lock", err)
			case <-ctx.Done():
				t.Fatal(ctx.Err())
			case <-time.After(5 * time.Millisecond):
			}
		}
	}
	if err = a.Rollback(ctx); err != nil {
		t.Fatal(err)
	}
	select {
	case err := <-done:
		if !errors.Is(err, errEntityTransition) {
			t.Fatal(err)
		}
	case <-ctx.Done():
		t.Fatal(ctx.Err())
	}
}
