package memory

import (
	"context"
	"encoding/json"
	"strconv"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestEntityAliasNormalization(t *testing.T) {
	for _, pair := range [][2]string{{"  DevBox  ", "devbox"}, {"My \t  Main\n Box", "my main box"}, {"192.168.1.254", "192.168.1.254"}, {"", ""}, {" \t\r\n\v\f", ""}, {"Registry É界", "registry É界"}} {
		if got := entityAliasName(pair[0]); got != pair[1] {
			t.Fatal(pair, got)
		}
	}
}

func TestEntityConflictsHostBoundary(t *testing.T) {
	handler := NewHandler(nil, WithDataStore(PlacementKB, nil))
	args := `{"operation":"entity-conflicts","action":"count"}`
	if _, status := invokeContextCommand(t, handler, 73, bus.CommandContext{}, "runtime", args); status != bus.ModuleStatusInvalidRequest {
		t.Fatal(status)
	}
	if _, status := invokeContextCommand(t, NewHandler(nil, WithDataStore(PlacementServer, nil)), 0, bus.CommandContext{}, "runtime", args); status != bus.ModuleStatusCapabilityAbsent {
		t.Fatal(status)
	}
	for _, args := range []string{`{"operation":"entity-conflicts","action":"record","name":" "}`, `{"operation":"entity-conflicts","action":"status","id":1,"status":"approved"}`, `{"operation":"entity-conflicts","action":"status","id":1.5,"status":"open"}`, `{"operation":"entity-conflicts","action":"delete"}`} {
		out, status := invokeContextCommand(t, handler, 0, bus.CommandContext{}, "runtime", args)
		if status != bus.ModuleStatusOK || out["kind"] != "invalid_argument" {
			t.Fatal(out, status)
		}
	}
}

// Replaces the C registry fixture with the production Go identity resolver and
// private conflict queue, under the packaged runtime role and real PostgreSQL.
func exerciseEntityRegistryReplay(t *testing.T, ctx context.Context, tx pgx.Tx, backend *postgresDataStore) {
	t.Helper()
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SAVEPOINT entity_registry_replay`)
	defer func() { exec(`ROLLBACK TO SAVEPOINT entity_registry_replay; RELEASE SAVEPOINT entity_registry_replay`) }()
	owner := *backend
	owner.db = runtimeRoleTx{evalQueryer{tx}, t}
	canonical := func(name string, kind NodeKind) string {
		t.Helper()
		got, err := owner.canonicalFactEndpoint(ctx, name, kind)
		if err != nil {
			t.Fatal(name, err)
		}
		return got
	}
	const preferred = "Registry DevBox"
	if got := canonical(preferred, NodeDevice); got != preferred {
		t.Fatal(got)
	}
	for _, name := range []string{"registry devbox", "  REGISTRY   DEVBOX  ", preferred} {
		if got := canonical(name, NodeOrg); got != preferred {
			t.Fatal(got)
		}
	}
	var id int64
	var kind int
	if err := tx.QueryRow(ctx, `SELECT r.canonical_id,r.kind FROM entity_registry r JOIN entity_aliases a USING(canonical_id) WHERE a.name_norm='registry devbox'`).Scan(&id, &kind); err != nil || kind != int(NodeDevice) {
		t.Fatal(id, kind, err)
	}
	exec(`INSERT INTO entity_aliases(name,name_norm,canonical_id,is_preferred) VALUES('Registry workstation','registry workstation',$1,0)`, id)
	if got := canonical("Registry Workstation", NodeDevice); got != preferred {
		t.Fatal(got)
	}
	// Preference is deterministic, and suppressed aliases never become canonical.
	exec(`UPDATE entity_aliases SET suppressed=1 WHERE name_norm='registry devbox'`)
	if got := canonical("Registry Workstation", NodeDevice); got != "Registry workstation" {
		t.Fatal(got)
	}
	if _, err := owner.canonicalFactEndpoint(ctx, preferred, NodeDevice); err == nil {
		t.Fatal("suppressed alias revived")
	}
	exec(`UPDATE entity_aliases SET suppressed=0 WHERE name_norm='registry devbox'`)
	var rows int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM entity_aliases WHERE canonical_id=$1`, id).Scan(&rows); err != nil || rows != 2 {
		t.Fatal(rows, err)
	}
	// Scalar endpoints are exact values, never aliases or registry rows.
	if got := canonical("Case Sensitive Scalar", NodeScalar); got != "Case Sensitive Scalar" {
		t.Fatal(got)
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM entity_aliases WHERE name_norm='case sensitive scalar'`).Scan(&rows); err != nil || rows != 0 {
		t.Fatal(rows, err)
	}
	unicodeName := strings.Repeat("界", 85)
	if got := canonical(unicodeName, NodePerson); got != unicodeName {
		t.Fatal(got)
	}
	for _, name := range []string{"", "   ", strings.Repeat("界", 86)} {
		if _, err := owner.canonicalFactEndpoint(ctx, name, NodeDevice); err == nil {
			t.Fatal("invalid identity accepted", name)
		}
	}
	// A dangling legacy alias must not be rebound or leave a half-created entity.
	exec(`INSERT INTO entity_aliases(name,name_norm,canonical_id,is_preferred) VALUES('Registry dangling','registry dangling',9223372036854775807,1)`)
	var before int
	tx.QueryRow(ctx, `SELECT count(*) FROM entity_registry`).Scan(&before)
	nested, err := tx.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	isolated := owner
	isolated.db = runtimeRoleTx{evalQueryer{nested}, t}
	_, err = isolated.canonicalFactEndpoint(ctx, "Registry dangling", NodeDevice)
	if err == nil {
		t.Fatal("dangling alias silently rebound")
	}
	if err = nested.Rollback(ctx); err != nil {
		t.Fatal(err)
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM entity_registry`).Scan(&rows); err != nil || rows != before {
		t.Fatal("failed registration leaked an entity", rows, before, err)
	}
	handler := NewHandler(nil, WithDataStore(PlacementKB, backend))
	call := func(action, name, status string, id int64, want bus.ModuleStatus) map[string]any {
		t.Helper()
		args, _ := json.Marshal(map[string]any{"operation": "entity-conflicts", "action": action, "name": name, "status": status, "id": strconv.FormatInt(id, 10)})
		out, got := invokeContextCommand(t, handler, 0, bus.CommandContext{}, "runtime", string(args))
		if got != want {
			t.Fatal(action, out, got)
		}
		return out
	}
	record := call("record", "Registry Ambiguous Theo", "", 0, bus.ModuleStatusOK)
	conflict := int64(record["id"].(float64))
	if conflict <= 0 {
		t.Fatal(record)
	}
	if r := call("priority", "registry ambiguous theo", "", 0, bus.ModuleStatusOK); r["priority"] != float64(1) || r["found"] != true {
		t.Fatal(r)
	}
	if r := call("record", "  REGISTRY   Ambiguous Theo ", "", 0, bus.ModuleStatusOK); r["id"] != record["id"] {
		t.Fatal(r)
	}
	if r := call("priority", "Registry Ambiguous Theo", "", 0, bus.ModuleStatusOK); r["priority"] != float64(2) {
		t.Fatal(r)
	}
	if r := call("count", "", "open", 0, bus.ModuleStatusOK); r["count"] != float64(1) {
		t.Fatal(r)
	}
	if r := call("status", "", "resolved", conflict, bus.ModuleStatusOK); r["updated"] != true {
		t.Fatal(r)
	}
	if r := call("count", "", "open", 0, bus.ModuleStatusOK); r["count"] != float64(0) {
		t.Fatal(r)
	}
	if r := call("count", "", "", 0, bus.ModuleStatusOK); r["count"] != float64(1) {
		t.Fatal(r)
	}
	call("record", "Registry Ambiguous Theo", "", 0, bus.ModuleStatusOK)
	if r := call("priority", "Registry Ambiguous Theo", "", 0, bus.ModuleStatusOK); r["priority"] != float64(3) {
		t.Fatal(r)
	}
	if r := call("count", "", "resolved", 0, bus.ModuleStatusOK); r["count"] != float64(1) {
		t.Fatal("observation reopened decision", r)
	}
	call("status", "", "failed", conflict, bus.ModuleStatusOK)
	if r := call("count", "", "failed", 0, bus.ModuleStatusOK); r["count"] != float64(1) {
		t.Fatal(r)
	}
	if r := call("status", "", "open", 9223372036854775807, bus.ModuleStatusOK); r["updated"] != false {
		t.Fatal(r)
	}
	if r := call("priority", "Registry never recorded", "", 0, bus.ModuleStatusOK); r["priority"] != float64(-1) || r["found"] != false {
		t.Fatal(r)
	}
	if r := call("count", "", "arbitrary status", 0, bus.ModuleStatusOK); r["count"] != float64(0) {
		t.Fatal(r)
	}
	var forbidden bool
	if err := tx.QueryRow(ctx, `SELECT has_table_privilege(current_user,'entity_name_conflicts','DELETE') OR has_column_privilege(current_user,'entity_name_conflicts','name_norm','UPDATE') OR has_column_privilege(current_user,'entity_name_conflicts','retries','SELECT') OR has_column_privilege(current_user,'entity_name_conflicts','created_at','INSERT')`).Scan(&forbidden); err != nil || forbidden {
		t.Fatal(forbidden, err)
	}
	exec(`RESET ROLE; REVOKE UPDATE(priority) ON entity_name_conflicts FROM aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`)
	call("record", "Registry Ambiguous Theo", "", 0, bus.ModuleStatusInternal)
	if r := call("priority", "Registry Ambiguous Theo", "", 0, bus.ModuleStatusOK); r["priority"] != float64(3) {
		t.Fatal(r)
	}
	exec(`RESET ROLE; REVOKE SELECT(priority) ON entity_name_conflicts FROM aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`)
	call("priority", "Registry Ambiguous Theo", "", 0, bus.ModuleStatusInternal)
}
