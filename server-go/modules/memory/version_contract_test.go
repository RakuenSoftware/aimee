package memory

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestExpectedVersionValidation(t *testing.T) {
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, nil)))
	valid := `{"schema_version":1,"owner_id":"00000000-0000-0000-0000-000000000001","record_id":"9007199254740993","record_revision":"9223372036854775807"}`
	for _, version := range []string{"null", `{}`, strings.Replace(valid, `"schema_version":1`, `"schema_version":2`, 1), strings.Replace(valid, `"record_revision":"9223372036854775807"`, `"record_revision":"01"`, 1), strings.Replace(valid, `"record_id":"9007199254740993"`, `"record_id":9007199254740993`, 1), strings.Replace(valid, `"record_id":"9007199254740993"`, `"record_id":"1"`, 1), strings.Replace(valid, `"schema_version":1`, `"extra":true,"schema_version":1`, 1)} {
		r := runPublicCommand(t, client, "supersede", `{"old_id":"9007199254740993","new_content":"change","expected_version":`+version+`}`)
		if r["kind"] != "invalid_argument" {
			t.Fatal(version, r)
		}
	}
	if r := runPublicCommand(t, client, "update", `{"id":1,"content":"change","expected_version":null}`); r["kind"] != "invalid_argument" {
		t.Fatal(r)
	}
	for _, verb := range []string{"store", "touch", "runtime"} {
		r := runPublicCommand(t, client, verb, `{"expected_version":`+valid+`}`)
		if r["kind"] != "unsupported_mode" {
			t.Fatal(verb, r)
		}
	}
	for _, value := range []string{"null", `"true"`, "1", "{}"} {
		r := runPublicCommand(t, client, "get", `{"id":1,"include_version":`+value+`}`)
		if r["kind"] != "invalid_argument" {
			t.Fatal(value, r)
		}
	}
	personal := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementServer, nil)))
	if r := runPublicCommand(t, personal, "delete", `{"id":1,"include_version":true}`); r["kind"] != "unsupported_mode" {
		t.Fatal(r)
	}
	for _, version := range []string{"null", `{}`, strings.Replace(valid, `"record_id":"9007199254740993"`, `"record_id":"1"`, 1)} {
		if r := runPublicCommand(t, personal, "get", `{"id":"9007199254740993","at_version":`+version+`}`); r["kind"] != "invalid_argument" {
			t.Fatal("personal history accepted malformed version", version, r)
		}
	}
	for _, args := range []string{`{"id":"9007199254740993","include_version":null}`, `{"id":"9007199254740993","at_version":` + valid + `,"read_policy":{"schema_version":1,"mode":"current"}}`} {
		if r := runPublicCommand(t, personal, "get", args); r["kind"] != "invalid_argument" {
			t.Fatal("ambiguous personal version request", r)
		}
	}
	if r := runPublicCommand(t, client, "get", `{"id":"9007199254740993","at_version":`+valid+`}`); r["kind"] != "unsupported_mode" {
		t.Fatal("KB silently ignored private version", r)
	}
	for _, args := range []string{`{"id":1,"include_version":true,"view":"session"}`, `{"id":1,"include_version":true,"view":"console","format":"text"}`} {
		if r := runPublicCommand(t, client, "get", args); r["kind"] != "unsupported_mode" {
			t.Fatal(args, r)
		}
	}
	var version MemoryRecordVersion
	if json.Unmarshal([]byte(valid), &version) != nil || !version.validFor(9007199254740993) {
		t.Fatal(version)
	}
	projected, err := json.Marshal(consoleMemoryRecord(publicMemoryRecord{ID: 9007199254740993, Version: &version}))
	if err != nil || !strings.Contains(string(projected), `"version":`+valid) {
		t.Fatal("console discarded exact mutation precondition", string(projected), err)
	}
}

// Run through the real non-owner handler, with shipping revision and scope guards.
func exerciseExpectedVersionReplay(t *testing.T, ctx context.Context, tx pgx.Tx, handler bus.ModuleHandler) {
	t.Helper()
	caller := bus.CommandContext{Authenticated: true, Principal: "user:version-fixture", UserAuthority: true, TransportIdentity: "cert:fixture"}
	invoke := func(verb string, args any) map[string]any {
		t.Helper()
		raw, err := json.Marshal(args)
		if err != nil {
			t.Fatal(err)
		}
		out, status := invokeContextCommand(t, handler, 0, caller, verb, string(raw))
		if status != bus.ModuleStatusOK {
			t.Fatal(status, out)
		}
		return out
	}
	row := invoke("store", map[string]any{"key": "runtime-version-guard", "content": "observed original", "authority": "user", "scope_context": true, "project": "version-guard"})
	if row["status"] != "ok" {
		t.Fatal(row)
	}
	id := int64(row["id"].(float64))
	get := map[string]any{"id": id, "scope_context": true, "project": "version-guard", "include_version": true}
	read := invoke("get", get)
	version := read["memory"].(map[string]any)["version"].(map[string]any)
	if version["record_id"] != fmt.Sprint(id) || version["record_revision"] != "1" {
		t.Fatal(read)
	}
	args := map[string]any{"old_id": id, "new_content": "corrected", "authority": "user", "scope_context": true, "project": "version-guard", "expected_version": version}
	originalOwner := version["owner_id"]
	version["owner_id"] = "00000000-0000-0000-0000-000000000000"
	if r := invoke("supersede", args); r["reason"] != "expected_version_conflict" {
		t.Fatal(r)
	}
	version["owner_id"] = originalOwner
	// A governed tag change invalidates a previously observed revision.
	if _, err := tx.Exec(ctx, `INSERT INTO memory_scopes VALUES($1,'workspace','version-tag')`, id); err != nil {
		t.Fatal(err)
	}
	if r := invoke("supersede", args); r["reason"] != "expected_version_conflict" {
		t.Fatal(r)
	}
	read = invoke("get", get)
	version = read["memory"].(map[string]any)["version"].(map[string]any)
	args["expected_version"] = version
	if version["record_revision"] != "2" || read["memory"].(map[string]any)["content"] != "observed original" {
		t.Fatal(read)
	}
	// Counter-only reads cannot invalidate an otherwise admissible correction.
	if _, err := tx.Exec(ctx, `UPDATE memories SET use_count=use_count+1,last_used_at=pg_now_text() WHERE id=$1`, id); err != nil {
		t.Fatal(err)
	}
	args["project"] = "hidden"
	if r := invoke("supersede", args); r["kind"] != "not_found" {
		t.Fatal("hidden ID leaked version", r)
	}
	args["project"] = "version-guard"
	args["authority"] = "model"
	if r := invoke("supersede", args); r["kind"] != "review_required" {
		t.Fatal("version bypassed authority", r)
	}
	args["authority"] = "user"
	corrected := invoke("supersede", args)
	if corrected["status"] != "ok" || corrected["memory"].(map[string]any)["id"] == float64(id) {
		t.Fatal(corrected)
	}
	if r := invoke("supersede", args); r["reason"] != "expected_version_conflict" {
		t.Fatal("replayed correction admitted", r)
	}
}

func TestExpectedVersionConcurrentCorrections(t *testing.T) {
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
	scope := Scope{Type: ScopeProject, Value: fmt.Sprintf("version-race-%d", time.Now().UnixNano())}
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
		_, err := (&postgresDataStore{db: evalQueryer{b}, placement: PlacementKB}).replaceKBVersion(ctx, row.ID, "loser", 1, "", AuthorityUser, nil, observed.Version)
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
			t.Fatal("second correction bypassed row lock", e)
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
