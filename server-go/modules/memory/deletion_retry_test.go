package memory

import (
	"context"
	"encoding/json"
	"reflect"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func exerciseDeletionRetryReplay(t *testing.T, ctx context.Context, tx pgx.Tx, handler bus.ModuleHandler) {
	t.Helper()
	caller := bus.CommandContext{Authenticated: true, UserAuthority: true, Principal: "user:delete-retry-fixture", TransportIdentity: "cert:delete-fixture"}
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
	exec(`SAVEPOINT indexed_deletion_retry`)
	defer func() { exec(`ROLLBACK TO SAVEPOINT indexed_deletion_retry; RELEASE SAVEPOINT indexed_deletion_retry`) }()
	for _, authority := range []string{"model", "user"} {
		scope := "deletion-retry-" + authority
		created := invoke("store", map[string]any{"key": scope, "content": "deletion fixture", "authority": authority, "project": scope, "scope_context": true})
		if created["status"] != "ok" {
			t.Fatal(created)
		}
		id := int64(created["id"].(float64))
		// Exercise the same derived rows the asynchronous indexer can create
		// before a deletion. A bare freshly inserted parent misses that boundary.
		if out := invoke("reindex", map[string]any{"project": scope, "scope_context": true}); out["status"] != "ok" {
			t.Fatal("index deletion fixture", out)
		}
		if scalar(`SELECT count(*) FROM memory_episodes WHERE memory_id=$1`, id) == 0 ||
			scalar(`SELECT count(*) FROM memory_units WHERE memory_id=$1`, id) == 0 {
			t.Fatal("deletion fixture lacks indexed dependencies")
		}
		get := map[string]any{"id": id, "include_version": true, "project": scope, "scope_context": true}
		read := invoke("get", get)
		version := read["memory"].(map[string]any)["version"]
		args := map[string]any{"id": id, "authority": authority, "view": "server", "project": scope, "scope_context": true, "expected_version": version, "idempotency_key": "deletion-fixture-" + authority}
		stale := map[string]any{}
		for k, v := range version.(map[string]any) {
			stale[k] = v
		}
		stale["record_revision"] = "9223372036854775807"
		args["expected_version"] = stale
		if out := invoke("delete", args); out["reason"] != "expected_version_conflict" {
			t.Fatal("stale deletion", out)
		}
		args["expected_version"] = version
		args["project"] = "hidden-deletion-scope"
		if out := invoke("delete", args); out["kind"] != "not_found" {
			t.Fatal("deletion leaked scope", out)
		}
		args["project"] = scope
		if authority == "user" {
			args["authority"] = "model"
			if out := invoke("delete", args); out["kind"] != "review_required" {
				t.Fatal("model deletion of user assertion", out)
			}
			args["authority"] = "user"
		}
		commits := scalar(`SELECT count(*) FROM fact_graph_commits`)
		generation := scalar(`SELECT generation FROM memory_collection_generations WHERE scope_type='project' AND scope_value=$1`, scope)
		exec(`RESET ROLE; ALTER TABLE memory_mutation_receipts ADD CONSTRAINT deletion_fixture_fail CHECK(operation='correction') NOT VALID; SET LOCAL ROLE aimee_store_runtime`)
		if out := invoke("delete", args); out["kind"] != "unavailable" {
			t.Fatal("late receipt failure", out)
		}
		if scalar(`SELECT count(*) FROM fact_graph_commits`) != commits || scalar(`SELECT generation FROM memory_collection_generations WHERE scope_type='project' AND scope_value=$1`, scope) != generation || scalar(`SELECT count(*) FROM memories WHERE id=$1 AND lifecycle_state='active'`, id) != 1 {
			t.Fatal("deletion receipt failure did not roll back atomically")
		}
		exec(`RESET ROLE; ALTER TABLE memory_mutation_receipts DROP CONSTRAINT deletion_fixture_fail; SET LOCAL ROLE aimee_store_runtime`)
		first := invoke("delete", args)
		if first["status"] != "ok" || first["deleted"] != true || first["destroyed"] != (authority == "user") {
			t.Fatal(first)
		}
		receipt := first["mutation_receipt"].(map[string]any)
		if scalar(`SELECT count(*) FROM memory_evidence_events WHERE changeset_id=$1 AND
 ((operation='purge' AND (before_ref<>'' OR after_ref<>'')) OR
  (before_ref<>'' AND NOT starts_with(before_ref,object_kind||':'||object_id||':')) OR
  (after_ref<>'' AND NOT starts_with(after_ref,object_kind||':'||object_id||':')))`, receipt["commit_id"]) != 0 {
			var evidence string
			if err := tx.QueryRow(ctx, `SELECT json_agg(json_build_object('kind',object_kind,'id',object_id,'op',operation,'before',before_ref,'after',after_ref))::text FROM memory_evidence_events WHERE changeset_id=$1`, receipt["commit_id"]).Scan(&evidence); err != nil {
				t.Fatal(err)
			}
			t.Fatal("one changed object rewrote another object's evidence references", evidence)
		}
		if authority == "user" && scalar(`SELECT count(*) FROM memory_evidence_events WHERE changeset_id=$1 AND operation='purge'`, receipt["commit_id"]) < 2 {
			t.Fatal("indexed destruction did not retain its cascaded purge events")
		}
		if receipt["schema_version"] != float64(2) || receipt["replayed"] != false || !reflect.DeepEqual(receipt["target_version"], version) {
			t.Fatal(receipt)
		}
		if authority == "user" {
			if receipt["outcome"] != "destroyed" || receipt["version"] != nil {
				t.Fatal("destruction invented a current version", receipt)
			}
		} else if receipt["outcome"] != "retired" || receipt["version"] == nil {
			t.Fatal(receipt)
		}
		exec(`SAVEPOINT forged_deletion_receipt`)
		_, forgedErr := tx.Exec(ctx, `INSERT INTO memory_mutation_receipts(owner_id,actor_principal,key_hash,request_hash,commit_id,
            result_id,result_revision,operation,target_revision,scope_type,scope_value)
            SELECT owner_id,actor_principal,repeat('f',64),request_hash,commit_id,result_id,result_revision,operation,
            target_revision+1,scope_type,scope_value FROM memory_mutation_receipts WHERE commit_id=$1`, receipt["commit_id"])
		if forgedErr == nil || !strings.Contains(forgedErr.Error(), "requires its admitted canonical audit") {
			t.Fatal("forged deletion receipt", forgedErr)
		}
		exec(`ROLLBACK TO SAVEPOINT forged_deletion_receipt; RELEASE SAVEPOINT forged_deletion_receipt`)
		commits = scalar(`SELECT count(*) FROM fact_graph_commits`)
		generation = scalar(`SELECT generation FROM memory_collection_generations WHERE scope_type='project' AND scope_value=$1`, scope)
		replay := invoke("delete", args)
		retried, ok := replay["mutation_receipt"].(map[string]any)
		if replay["status"] != "ok" || !ok || retried["commit_id"] != receipt["commit_id"] || retried["replayed"] != true {
			t.Fatal(replay)
		}
		if scalar(`SELECT count(*) FROM fact_graph_commits`) != commits || scalar(`SELECT generation FROM memory_collection_generations WHERE scope_type='project' AND scope_value=$1`, scope) != generation {
			t.Fatal("replay repeated audit or invalidation")
		}
		args["expected_version"] = stale
		if out := invoke("delete", args); out["reason"] != "idempotency_conflict" {
			t.Fatal("changed retry payload", out)
		}
		args["expected_version"] = version
		args["content"] = "cannot switch operation"
		if out := invoke("update", args); out["reason"] != "idempotency_conflict" {
			t.Fatal("cross-verb key reuse", out)
		}
		delete(args, "content")
		if authority == "model" {
			args["view"] = "mcp"
			if out := invoke("delete", args); out["mutation_receipt"] == nil || out["audit_id"] != nil {
				t.Fatal("MCP replay lost receipt or duplicated audit", out)
			}
			exec(`RESET ROLE`)
			exec(`UPDATE memories SET lifecycle_state='active',activation_suppressed=0,valid_until=NULL WHERE id=$1`, id)
			exec(`SET LOCAL ROLE aimee_store_runtime`)
		} else {
			// A reused ID in another scope is invisible to the caller's normal SELECT.
			// It must still prevent a destruction receipt from certifying absence.
			exec(`RESET ROLE`)
			exec(`INSERT INTO memories(id,key,content,tier,kind,scope_type,scope_value,provenance_category)
    VALUES($1,'resurrected','hidden new assertion','L2','fact','project','hidden-resurrection','user_stated')`, id)
			exec(`SET LOCAL ROLE aimee_store_runtime`)
		}
		if out := invoke("delete", args); out["reason"] != "idempotent_result_unavailable" || out["mutation_receipt"] != nil {
			t.Fatal("replayed against reactivated or hidden resurrected ID", out)
		}
		fresh := invoke("store", map[string]any{"key": scope + "-unkeyed", "content": "conditional only", "authority": authority, "project": scope, "scope_context": true})
		get["id"] = fresh["id"]
		next := invoke("get", get)
		conditional := map[string]any{"id": fresh["id"], "authority": authority, "view": "server", "project": scope, "scope_context": true,
			"expected_version": next["memory"].(map[string]any)["version"]}
		if out := invoke("delete", conditional); out["status"] != "ok" || out["deleted"] != true || out["mutation_receipt"] != nil {
			t.Fatal("unkeyed conditional deletion", out)
		}

	}
}
