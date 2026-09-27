package memory

import (
	"context"
	"encoding/json"
	"reflect"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func exerciseCreationRetryReplay(t *testing.T, ctx context.Context, tx pgx.Tx, handler bus.ModuleHandler) {
	t.Helper()
	caller := bus.CommandContext{Authenticated: true, UserAuthority: true, Principal: "user:creation-retry-fixture", TransportIdentity: "cert:creation-fixture"}
	invoke := func(verb string, args map[string]any) map[string]any {
		t.Helper()
		raw, e := json.Marshal(args)
		if e != nil {
			t.Fatal(e)
		}
		out, status := invokeContextCommand(t, handler, 0, caller, verb, string(raw))
		if status != bus.ModuleStatusOK {
			t.Fatal(status, out)
		}
		return out
	}
	exec := func(q string, args ...any) {
		t.Helper()
		if _, e := tx.Exec(ctx, q, args...); e != nil {
			t.Fatal(e)
		}
	}
	scalar := func(q string, args ...any) int64 {
		t.Helper()
		var n int64
		if e := tx.QueryRow(ctx, q, args...).Scan(&n); e != nil {
			t.Fatal(e)
		}
		return n
	}
	for _, authority := range []string{"model", "user"} {
		scope := "creation-retry-" + authority
		args := map[string]any{"key": scope, "content": "creation fixture", "authority": authority, "project": scope, "scope_context": true, "idempotency_key": "creation-fixture-" + authority}
		commits := scalar(`SELECT count(*) FROM fact_graph_commits`)
		exec(`RESET ROLE; ALTER TABLE memory_mutation_receipts ADD CONSTRAINT creation_fixture_fail CHECK(operation NOT IN ('store','store_noop')) NOT VALID; SET LOCAL ROLE aimee_store_runtime`)
		if out := invoke("store", args); out["kind"] != "unavailable" {
			t.Fatal("late receipt failure", out)
		}
		if scalar(`SELECT count(*) FROM fact_graph_commits`) != commits || scalar(`SELECT count(*) FROM memories WHERE key=$1`, scope) != 0 {
			t.Fatal("creation failure leaked record or audit")
		}
		exec(`RESET ROLE; ALTER TABLE memory_mutation_receipts DROP CONSTRAINT creation_fixture_fail; SET LOCAL ROLE aimee_store_runtime`)
		first := invoke("store", args)
		if first["status"] != "ok" {
			t.Fatal(first)
		}
		receipt, ok := first["mutation_receipt"].(map[string]any)
		if !ok || receipt["schema_version"] != float64(2) || receipt["outcome"] != "stored" || receipt["replayed"] != false {
			t.Fatal(first)
		}
		id := first["id"]
		commits = scalar(`SELECT count(*) FROM fact_graph_commits`)
		generation := scalar(`SELECT generation FROM memory_collection_generations WHERE scope_type='project' AND scope_value=$1`, scope)
		jobs := scalar(`SELECT generation FROM kb_async_jobs WHERE kind='memory_facts' AND document_id=$1`, id)
		replay := invoke("store", args)
		retried, ok := replay["mutation_receipt"].(map[string]any)
		if !ok || replay["id"] != id || retried["commit_id"] != receipt["commit_id"] || retried["replayed"] != true || !reflect.DeepEqual(retried["version"], receipt["version"]) {
			t.Fatal(replay)
		}
		if scalar(`SELECT count(*) FROM fact_graph_commits`) != commits || scalar(`SELECT generation FROM memory_collection_generations WHERE scope_type='project' AND scope_value=$1`, scope) != generation || scalar(`SELECT generation FROM kb_async_jobs WHERE kind='memory_facts' AND document_id=$1`, id) != jobs {
			t.Fatal("replay repeated audit, invalidation or extraction")
		}
		for field, value := range map[string]any{"key": "changed-key", "content": "changed content", "tier": "L5", "kind": "preference", "confidence": 0.3, "use_cases": "changed purpose", "session_id": "changed-session", "project": "another-project", "authority": map[string]string{"model": "user", "user": "model"}[authority]} {
			old, exists := args[field]
			args[field] = value
			if out := invoke("store", args); out["reason"] != "idempotency_conflict" {
				t.Fatal("changed request", field, out)
			}
			if exists {
				args[field] = old
			} else {
				delete(args, field)
			}
		}
		// A new key with identical admitted content preserves canonical identity,
		// revision and confidence. It is an unchanged observation, not new evidence.
		args["idempotency_key"] = "creation-unchanged-" + authority
		events := scalar(`SELECT count(*) FROM memory_evidence_events`)
		unchanged := invoke("store", args)
		noop, ok := unchanged["mutation_receipt"].(map[string]any)
		if !ok || unchanged["id"] != id || noop["outcome"] != "unchanged" || !reflect.DeepEqual(noop["version"], receipt["version"]) {
			t.Fatal("unchanged store", unchanged)
		}
		if scalar(`SELECT generation FROM memory_collection_generations WHERE scope_type='project' AND scope_value=$1`, scope) != generation || scalar(`SELECT count(*) FROM memory_evidence_events`) != events {
			t.Fatal("unchanged store fabricated evidence or invalidation")
		}
		if out := invoke("store", args); out["mutation_receipt"].(map[string]any)["outcome"] != "unchanged" {
			t.Fatal(out)
		}
		// The receipt guard must reject a changed result revision even for its actor.
		exec(`SAVEPOINT forged_creation_receipt`)
		_, e := tx.Exec(ctx, `INSERT INTO memory_mutation_receipts(owner_id,actor_principal,key_hash,request_hash,commit_id,result_id,result_revision,operation,scope_type,scope_value)
   SELECT owner_id,actor_principal,repeat('f',64),request_hash,commit_id,result_id,result_revision+1,operation,scope_type,scope_value FROM memory_mutation_receipts WHERE commit_id=$1`, noop["commit_id"])
		if e == nil {
			t.Fatal("forged creation receipt accepted")
		}
		exec(`ROLLBACK TO SAVEPOINT forged_creation_receipt; RELEASE SAVEPOINT forged_creation_receipt`)
		args["idempotency_key"] = "creation-replacement-" + authority
		args["content"] = "replacement fixture"
		replacement := invoke("store", args)
		if replacement["status"] != "ok" || replacement["id"] == id {
			t.Fatal("replacement admission", replacement)
		}
		// Replacement inserts its supersession link in the same commit. Bind the
		// receipt to the final dependency revision, not just the initial insert.
		replacedReceipt := replacement["mutation_receipt"].(map[string]any)
		replacedVersion := replacedReceipt["version"].(map[string]any)
		if replacedVersion["record_revision"] != "2" {
			t.Fatal("replacement receipt omitted link revision", replacement)
		}
		if retry := invoke("store", args); retry["status"] != "ok" || !reflect.DeepEqual(retry["mutation_receipt"].(map[string]any)["version"], replacedVersion) {
			t.Fatal("replacement final revision not replayable", retry)
		}
		args["idempotency_key"] = "creation-fixture-" + authority
		args["content"] = "creation fixture"
		if out := invoke("store", args); out["reason"] != "idempotent_result_unavailable" {
			t.Fatal("superseded result replayed", out)
		}
		args["idempotency_key"] = "creation-replacement-" + authority
		args["content"] = "replacement fixture"
		if out := invoke("delete", map[string]any{"id": replacement["id"], "authority": "user", "project": scope, "scope_context": true}); out["status"] != "ok" {
			t.Fatal(out)
		}
		if out := invoke("store", args); out["reason"] != "idempotent_result_unavailable" {
			t.Fatal("erased creation repeated", out)
		}
	}
	// A model's keyed upsert of a human assertion retains the review workflow.
	scope := "creation-review"
	args := map[string]any{"key": scope, "content": "protected assertion", "authority": "user", "project": scope, "scope_context": true}
	original := invoke("store", args)
	if original["status"] != "ok" {
		t.Fatal(original)
	}
	args["authority"] = "model"
	args["content"] = "model proposed correction"
	args["idempotency_key"] = "creation-review-fixture"
	proposed := invoke("store", args)
	if proposed["kind"] != "review_required" || proposed["proposal"] == nil {
		t.Fatal(proposed)
	}
	replay := invoke("store", args)
	if replay["kind"] != "review_required" || replay["proposal"] == nil {
		t.Fatal(replay)
	}
	a, b := proposed["proposal"].(map[string]any), replay["proposal"].(map[string]any)
	if a["proposal_id"] != b["proposal_id"] || b["replayed"] != true {
		t.Fatal(proposed, replay)
	}

	decision := invoke("review_correction", map[string]any{"proposal_id": a["proposal_id"], "payload_digest": a["payload_digest"], "expected_version": a["target_version"], "action": "reject", "project": scope, "scope_context": true})
	if decision["status"] != "ok" {
		t.Fatal("reject creation draft", decision, a)
	}
	caller.Principal = "user:another-creation-retry-fixture"
	args["idempotency_key"] = "creation-reviewed-fixture"
	rejected := invoke("store", args)
	linked, ok := rejected["proposal"].(map[string]any)
	if !ok || linked["proposal_id"] != a["proposal_id"] || linked["state"] != "rejected" {
		t.Fatal("store reopened terminal proposal", rejected)
	}
	if out := invoke("delete", map[string]any{"id": original["id"], "authority": "user", "project": scope, "scope_context": true}); out["status"] != "ok" {
		t.Fatal(out)
	}
	if out := invoke("store", args); out["reason"] != "idempotent_result_unavailable" {
		t.Fatal(out)
	}
}

func TestCreationKeyPublicValidation(t *testing.T) {
	for _, placement := range []Placement{PlacementKB, PlacementServer} {
		client := clientForHandler(t, NewHandler(nil, WithDataStore(placement, nil)))
		for _, key := range []string{`null`, `1`, `"short"`, `"has space in key"`, `"has\nnewline-key"`} {
			if out := runPublicCommand(t, client, "store", `{"key":"fixture","content":"fixture","idempotency_key":`+key+`}`); out["kind"] != "invalid_argument" {
				t.Fatal(placement, key, out)
			}
		}
		if out := runPublicCommand(t, client, "store", `{"key":"fixture","content":"fixture","idempotency_key":"creation-fixture-key"}`); out["kind"] != "forbidden" {
			t.Fatal(placement, out)
		}
		if out := runPublicCommand(t, client, "store", `{"key":"fixture","content":"fixture","expected_version":{"schema_version":1,"owner_id":"00000000-0000-0000-0000-000000000001","record_id":"1","record_revision":"1"}}`); out["kind"] != "unsupported_mode" {
			t.Fatal(placement, out)
		}
	}
}
