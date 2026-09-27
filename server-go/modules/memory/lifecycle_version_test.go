package memory

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestLifecycleVersionValidation(t *testing.T) {
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, nil)))
	for _, verb := range []string{"reject", "restore"} {
		for _, version := range []string{"null", "{}", `{"schema_version":2}`} {
			r := runPublicCommand(t, client, verb, `{"id":1,"expected_version":`+version+`}`)
			if r["kind"] != "invalid_argument" {
				t.Fatal(verb, version, r)
			}
		}
		if r := runPublicCommand(t, client, verb, `{"id":1,"idempotency_key":"missing-version-key"}`); r["kind"] != "invalid_argument" {
			t.Fatal("retry without expected version accepted", verb, r)
		}
	}
}

// The shipping schema and restricted runtime role exercise RLS, revision
// triggers, rejection tombstones, and the actual public command transaction.
func exerciseLifecycleVersionReplay(t *testing.T, ctx context.Context, tx pgx.Tx, handler bus.ModuleHandler) {
	t.Helper()
	caller := bus.CommandContext{Authenticated: true, Principal: "user:lifecycle-version", UserAuthority: true, TransportIdentity: "cert:fixture"}
	invoke := func(verb string, args map[string]any) map[string]any {
		t.Helper()
		raw, err := json.Marshal(args)
		if err != nil {
			t.Fatal(err)
		}
		out, status := invokeContextCommand(t, handler, 0, caller, verb, string(raw))
		if status != bus.ModuleStatusOK {
			t.Fatal(verb, status, out)
		}
		return out
	}
	row := invoke("store", map[string]any{"key": "lifecycle-version", "content": "observed content", "authority": "user", "scope_context": true, "project": "lifecycle-version"})
	if row["status"] != "ok" {
		t.Fatal(row)
	}
	id := int64(row["id"].(float64))
	get := map[string]any{"id": id, "scope_context": true, "project": "lifecycle-version", "include_version": true}
	read := invoke("get", get)
	version := read["memory"].(map[string]any)["version"]
	args := map[string]any{"id": id, "scope_context": true, "project": "lifecycle-version", "reason": "reviewed rejection", "expected_version": version}
	// A metadata change must invalidate a decision made against the old record.
	if _, err := tx.Exec(ctx, `INSERT INTO memory_scopes VALUES($1,'workspace','lifecycle-version-tag')`, id); err != nil {
		t.Fatal(err)
	}
	if out := invoke("reject", args); out["reason"] != "expected_version_conflict" {
		t.Fatal("stale rejection accepted", out)
	}
	if out := invoke("get", get); out["status"] != "ok" {
		t.Fatal("stale rejection changed the record", out)
	}
	read = invoke("get", get)
	args["expected_version"] = read["memory"].(map[string]any)["version"]
	args["project"] = "hidden"
	if out := invoke("reject", args); out["kind"] != "not_found" {
		t.Fatal("version leaked a hidden rejection target", out)
	}
	args["project"] = "lifecycle-version"
	if out := invoke("reject", args); out["status"] != "ok" {
		t.Fatal(out)
	}
	if out := invoke("restore", args); out["reason"] != "expected_version_conflict" {
		t.Fatal("pre-rejection version restored changed content", out)
	}
	var revision, owner string
	var suppressed int
	if err := tx.QueryRow(ctx, `SELECT record_revision::text,activation_suppressed,(SELECT owner_id::text FROM memory_collection_owner WHERE id=1) FROM memories WHERE id=$1`, id).Scan(&revision, &suppressed, &owner); err != nil || suppressed != 1 {
		t.Fatal("stale restore changed suppression", suppressed, err)
	}
	current := MemoryRecordVersion{SchemaVersion: 1, OwnerID: owner, RecordID: fmt.Sprint(id), RecordRevision: revision}
	args["expected_version"] = current
	args["project"] = "hidden"
	if out := invoke("restore", args); out["kind"] != "not_found" {
		t.Fatal("version leaked a hidden restoration target", out)
	}
	args["project"] = "lifecycle-version"
	wrongOwner := current
	wrongOwner.OwnerID = "00000000-0000-0000-0000-000000000000"
	args["expected_version"] = wrongOwner
	if out := invoke("restore", args); out["reason"] != "expected_version_conflict" {
		t.Fatal("wrong owner accepted", out)
	}
	args["expected_version"] = current
	if out := invoke("restore", args); out["status"] != "ok" {
		t.Fatal(out)
	}
	if out := invoke("restore", args); out["reason"] != "expected_version_conflict" {
		t.Fatal("stale restoration replay accepted", out)
	}
	if out := invoke("get", get); out["status"] != "ok" {
		t.Fatal("matching restoration did not reactivate record", out)
	}
}

func TestLifecycleVersionWaitsForConcurrentCorrection(t *testing.T) {
	dsn := os.Getenv("AIMEE_DB2_REPLAY_URL")
	if dsn == "" {
		t.Skip("set AIMEE_DB2_REPLAY_URL")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	connect := func() *pgx.Conn {
		t.Helper()
		c, e := pgx.Connect(ctx, dsn)
		if e != nil {
			t.Fatal(e)
		}
		t.Cleanup(func() { c.Close(context.Background()) })
		return c
	}
	owner, first, second := connect(), connect(), connect()
	scope := Scope{Type: ScopeProject, Value: fmt.Sprintf("lifecycle-version-race-%d", time.Now().UnixNano())}
	defer func() {
		cancel()
		cleanup, stop := context.WithTimeout(context.Background(), 5*time.Second)
		defer stop()
		if _, e := owner.Exec(cleanup, `DELETE FROM memories WHERE scope_type='project' AND scope_value=$1`, scope.Value); e != nil {
			t.Error(e)
		}
	}()
	create, e := owner.Begin(ctx)
	if e != nil {
		t.Fatal(e)
	}
	defer create.Rollback(context.Background())
	backend := &postgresDataStore{db: evalQueryer{create}, placement: PlacementKB}
	row, e := backend.InsertEpistemic(ctx, DataRequest{Scope: scope, Tier: "L2", Kind: "fact", Key: "concurrent", Content: "original", Authority: AuthorityUser})
	if e != nil {
		t.Fatal(e)
	}
	observed, e := backend.getAtVersioned(ctx, scope, row.ID, false, "", true)
	if e != nil {
		t.Fatal(e)
	}
	if e = create.Commit(ctx); e != nil {
		t.Fatal(e)
	}
	a, e := first.Begin(ctx)
	if e != nil {
		t.Fatal(e)
	}
	defer a.Rollback(context.Background())
	b, e := second.Begin(ctx)
	if e != nil {
		t.Fatal(e)
	}
	defer b.Rollback(context.Background())
	winner, e := (&postgresDataStore{db: evalQueryer{a}, placement: PlacementKB}).replaceKBVersion(ctx, row.ID, "winner", 1, "", AuthorityUser, nil, observed.Version)
	if e != nil {
		t.Fatal(e)
	}
	done := make(chan error, 1)
	go func() {
		backend := &postgresDataStore{db: evalQueryer{b}, placement: PlacementKB}
		err := backend.lockKBLifecycleVersion(ctx, row.ID, observed.Version)
		if err == nil {
			_, err = backend.Reject(ctx, row.ID, "stale rejection")
		}
		done <- err
	}()
	for {
		var blocked bool
		if e = owner.QueryRow(ctx, `SELECT $1=ANY(pg_blocking_pids($2))`, first.PgConn().PID(), second.PgConn().PID()).Scan(&blocked); e != nil {
			t.Fatal(e)
		}
		if blocked {
			break
		}
		select {
		case e := <-done:
			t.Fatal("conditional rejection bypassed row lock", e)
		case <-ctx.Done():
			t.Fatal(ctx.Err())
		case <-time.After(5 * time.Millisecond):
		}
	}
	if e = a.Commit(ctx); e != nil {
		t.Fatal(e)
	}
	select {
	case e := <-done:
		if !errors.Is(e, errMutationVersionConflict) {
			t.Fatal(e)
		}
	case <-ctx.Done():
		t.Fatal(ctx.Err())
	}
	if e = b.Commit(ctx); e != nil {
		t.Fatal(e)
	}
	var count int
	if e = owner.QueryRow(ctx, `SELECT count(*) FROM memories WHERE scope_type='project' AND scope_value=$1 AND lifecycle_state='active' AND id=$2 AND content='winner'`, scope.Value, winner.ID).Scan(&count); e != nil || count != 1 {
		t.Fatal(count, e)
	}
}
