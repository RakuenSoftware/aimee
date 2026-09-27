package memory

import (
	"context"
	"encoding/json"
	"os"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func exerciseLifecycleRetryReplay(t *testing.T, ctx context.Context, tx pgx.Tx, handler bus.ModuleHandler) {
	t.Helper()
	caller := bus.CommandContext{Authenticated: true, UserAuthority: true, Principal: "user:lifecycle-retry", TransportIdentity: "cert:lifecycle"}
	invoke := func(verb string, args map[string]any) map[string]any {
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
	exec := func(q string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, q, args...); err != nil {
			t.Fatal(err)
		}
	}
	scalar := func(q string, args ...any) int64 {
		t.Helper()
		var n int64
		if err := tx.QueryRow(ctx, q, args...).Scan(&n); err != nil {
			t.Fatal(err)
		}
		return n
	}
	exec("SAVEPOINT lifecycle_retries")
	defer exec("ROLLBACK TO SAVEPOINT lifecycle_retries; RELEASE SAVEPOINT lifecycle_retries")
	created := invoke("store", map[string]any{"key": "lifecycle-retry", "content": "retry decision", "authority": "user", "project": "lifecycle-retry", "scope_context": true})
	if created["status"] != "ok" {
		t.Fatal(created)
	}
	id := int64(created["id"].(float64))
	version := func() MemoryRecordVersion {
		t.Helper()
		v := MemoryRecordVersion{SchemaVersion: 1}
		if err := tx.QueryRow(ctx, `SELECT id::text,record_revision::text,(SELECT owner_id::text FROM memory_collection_owner WHERE id=1) FROM memories WHERE id=$1`, id).
			Scan(&v.RecordID, &v.RecordRevision, &v.OwnerID); err != nil {
			t.Fatal(err)
		}
		return v
	}
	var oldReject map[string]any
	for _, verb := range []string{"reject", "restore"} {
		args := map[string]any{"id": id, "reason": "reviewed", "authority": "user", "project": "lifecycle-retry", "scope_context": true, "expected_version": version(), "idempotency_key": "lifecycle-retry-" + verb}
		commits := scalar("SELECT count(*) FROM fact_graph_commits")
		generation := scalar("SELECT generation FROM memory_collection_generations WHERE scope_type='project' AND scope_value='lifecycle-retry'")
		before := version()
		exec("RESET ROLE; ALTER TABLE memory_mutation_receipts ADD CONSTRAINT lifecycle_fixture_fail CHECK(operation='correction') NOT VALID; SET LOCAL ROLE aimee_store_runtime")
		if out := invoke(verb, args); out["kind"] != "unavailable" {
			t.Fatal("late receipt failure", verb, out)
		}
		if version() != before || scalar("SELECT count(*) FROM fact_graph_commits") != commits ||
			scalar("SELECT generation FROM memory_collection_generations WHERE scope_type='project' AND scope_value='lifecycle-retry'") != generation {
			t.Fatal("partial lifecycle commit")
		}
		exec("RESET ROLE; ALTER TABLE memory_mutation_receipts DROP CONSTRAINT lifecycle_fixture_fail; SET LOCAL ROLE aimee_store_runtime")
		first := invoke(verb, args)
		if first["status"] != "ok" {
			t.Fatal(verb, first)
		}
		receipt := first["mutation_receipt"].(map[string]any)
		exec("SAVEPOINT forged_lifecycle_receipt")
		_, forgedErr := tx.Exec(ctx, `INSERT INTO memory_mutation_receipts(owner_id,actor_principal,key_hash,request_hash,commit_id,
 result_id,result_revision,operation,target_revision,scope_type,scope_value)
 SELECT owner_id,actor_principal,repeat('f',64),request_hash,commit_id,result_id,result_revision,operation,
 target_revision+1,scope_type,scope_value FROM memory_mutation_receipts WHERE commit_id=$1`, receipt["commit_id"])
		if forgedErr == nil || !strings.Contains(forgedErr.Error(), "requires its admitted canonical audit") {
			t.Fatal("forged lifecycle receipt", forgedErr)
		}
		exec("ROLLBACK TO SAVEPOINT forged_lifecycle_receipt; RELEASE SAVEPOINT forged_lifecycle_receipt")

		// Migration/reapplication must accept persisted lifecycle receipts and
		// preserve earlier correction, creation and deletion receipts.
		schema, err := os.ReadFile("../../../src/modules/db2/c/schema.sql")
		if err != nil {
			t.Fatal(err)
		}
		start := strings.Index(string(schema), "-- BEGIN memory mutation receipts")
		end := strings.Index(string(schema), "-- END memory mutation receipts")
		if start < 0 || end < start {
			t.Fatal("receipt migration missing")
		}
		exec("RESET ROLE")
		count := scalar("SELECT count(*) FROM memory_mutation_receipts")
		for i := 0; i < 2; i++ {
			exec(string(schema[start:end]))
		}
		if scalar("SELECT count(*) FROM memory_mutation_receipts") != count {
			t.Fatal("migration lost receipts")
		}
		exec("SET LOCAL ROLE aimee_store_runtime")
		originalCaller := caller
		caller.Principal = "user:another-lifecycle-caller"
		if out := invoke(verb, args); out["reason"] != "expected_version_conflict" {
			t.Fatal("cross-caller receipt replay", out)
		}
		caller = originalCaller
		// Reusing a key with a changed scope is a different request, even if
		// the caller knows the original record and version.
		args["project"] = "hidden-lifecycle-retry"
		if out := invoke(verb, args); out["reason"] != "idempotency_conflict" {
			t.Fatal("changed retry scope", out)
		}
		args["project"] = "lifecycle-retry"

		commits = scalar("SELECT count(*) FROM fact_graph_commits")
		generation = scalar("SELECT generation FROM memory_collection_generations WHERE scope_type='project' AND scope_value='lifecycle-retry'")
		replay := invoke(verb, args)
		if replay["status"] != "ok" {
			t.Fatal("retry", verb, replay)
		}
		replayReceipt := replay["mutation_receipt"].(map[string]any)
		if replayReceipt["replayed"] != true || replayReceipt["commit_id"] != receipt["commit_id"] {
			t.Fatal(replay)
		}
		if scalar("SELECT count(*) FROM fact_graph_commits") != commits ||
			scalar("SELECT generation FROM memory_collection_generations WHERE scope_type='project' AND scope_value='lifecycle-retry'") != generation {
			t.Fatal("retry repeated audit or invalidation")
		}
		if verb == "reject" {
			args["reason"] = "changed"
			if out := invoke(verb, args); out["reason"] != "idempotency_conflict" {
				t.Fatal("changed rejection reason", out)
			}
			args["reason"] = "reviewed"
			oldReject = args
			// Fresh-key rejection of an unchanged archived record is a real admitted
			// no-op, with its own receipt but no invented canonical revision.
			noop := map[string]any{}
			for k, v := range args {
				noop[k] = v
			}
			noop["expected_version"] = version()
			noop["idempotency_key"] = "lifecycle-reject-noop"
			if out := invoke(verb, noop); out["status"] != "ok" {
				t.Fatal("noop rejection", out)
			}
			if out := invoke(verb, noop); out["status"] != "ok" || out["mutation_receipt"].(map[string]any)["replayed"] != true {
				t.Fatal("noop replay", out)
			}
		} else {
			if out := invoke("reject", oldReject); out["reason"] != "idempotent_result_unavailable" {
				t.Fatal("old rejection replay after restore", out)
			}
			// A later canonical change makes the restored receipt unusable.
			exec("INSERT INTO memory_scopes VALUES($1,'workspace','lifecycle-retry-tag')", id)
			if out := invoke(verb, args); out["reason"] != "idempotent_result_unavailable" {
				t.Fatal("changed restore result", out)
			}
		}
	}
}
