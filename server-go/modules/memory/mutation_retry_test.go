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

func TestIdempotencyContractValidation(t *testing.T) {
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, nil)))
	version := `{"schema_version":1,"owner_id":"00000000-0000-0000-0000-000000000001","record_id":"1","record_revision":"1"}`
	for _, key := range []string{`null`, `""`, `1`, `{}`, `"too-short"`, `"has spaces in key"`, `"has\nnewline-key"`, fmt.Sprintf("%q", strings.Repeat("a", 129))} {
		r := runPublicCommand(t, client, "supersede", `{"old_id":1,"new_content":"new","idempotency_key":`+key+`,"expected_version":`+version+`}`)
		if r["kind"] != "invalid_argument" {
			t.Fatal(key, r)
		}
	}
	for _, verb := range []string{"get", "touch", "runtime"} {
		if r := runPublicCommand(t, client, verb, `{"idempotency_key":"fixture-retry-key"}`); r["kind"] != "unsupported_mode" {
			t.Fatal(verb, r)
		}
	}
	if r := runPublicCommand(t, client, "supersede", `{"old_id":1,"new_content":"new","idempotency_key":"fixture-retry-key"}`); r["kind"] != "invalid_argument" {
		t.Fatal(r)
	}
	if r := runPublicCommand(t, client, "supersede", `{"old_id":1,"new_content":"new","idempotency_key":"fixture-retry-key","expected_version":`+version+`}`); r["kind"] != "forbidden" {
		t.Fatal(r)
	}
	personal := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementServer, nil)))
	if r := runPublicCommand(t, personal, "supersede", `{"idempotency_key":"fixture-retry-key"}`); r["kind"] != "invalid_argument" {
		t.Fatal(r)
	}
}

func exerciseMutationRetryReplay(t *testing.T, ctx context.Context, tx pgx.Tx, handler bus.ModuleHandler) {
	t.Helper()
	caller := bus.CommandContext{Authenticated: true, Principal: "user:retry-fixture", UserAuthority: true, TransportIdentity: "cert:retry-fixture"}
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
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, e := tx.Exec(ctx, sql, args...); e != nil {
			t.Fatal(e)
		}
	}
	scalar := func(sql string, args ...any) int64 {
		t.Helper()
		var n int64
		if e := tx.QueryRow(ctx, sql, args...).Scan(&n); e != nil {
			t.Fatal(e)
		}
		return n
	}
	row := invoke("store", map[string]any{"key": "retry-original", "content": "original", "authority": "user", "project": "retry-scope", "scope_context": true})
	if row["status"] != "ok" {
		t.Fatal(row)
	}
	id := int64(row["id"].(float64))
	read := invoke("get", map[string]any{"id": id, "include_version": true, "project": "retry-scope", "scope_context": true})
	version := read["memory"].(map[string]any)["version"]
	args := map[string]any{"old_id": id, "new_content": "corrected", "authority": "user", "project": "retry-scope", "scope_context": true, "expected_version": version, "idempotency_key": "retry-fixture-key-01"}
	// Force the last insert to fail, after row replacement, scope copies, actor,
	// extraction job and WORM sealing. None may survive the request rollback.
	before := scalar(`SELECT count(*) FROM fact_graph_commits`)
	generation := scalar(`SELECT generation FROM memory_collection_generations WHERE scope_type='project' AND scope_value='retry-scope'`)
	exec(`RESET ROLE; ALTER TABLE memory_mutation_receipts ADD CONSTRAINT retry_fixture_fail CHECK(actor_principal<>'user:retry-fixture') NOT VALID; SET LOCAL ROLE aimee_store_runtime`)
	if out := invoke("supersede", args); out["status"] == "ok" {
		t.Fatal(out)
	}
	if n := scalar(`SELECT count(*) FROM fact_graph_commits`); n != before {
		t.Fatal("failed receipt leaked audit commit", n, before)
	}
	if n := scalar(`SELECT generation FROM memory_collection_generations WHERE scope_type='project' AND scope_value='retry-scope'`); n != generation {
		t.Fatal("failed receipt published invalidation", n, generation)
	}
	if n := scalar(`SELECT count(*) FROM memories WHERE id=$1 AND lifecycle_state='active' AND content='original'`, id); n != 1 {
		t.Fatal("failed receipt consumed original")
	}
	exec(`RESET ROLE; ALTER TABLE memory_mutation_receipts DROP CONSTRAINT retry_fixture_fail; SET LOCAL ROLE aimee_store_runtime`)
	// Admission must not depend on the audit writer being available. Block
	// canonical audit inserts: refused edits still return the policy/version
	// decision, whereas an admitted edit reaches the constraint and rolls back.
	exec(`RESET ROLE; ALTER TABLE fact_graph_commits ADD CONSTRAINT retry_fixture_admission CHECK(operation NOT IN ('memory.supersede','memory.update')) NOT VALID; SET LOCAL ROLE aimee_store_runtime`)
	for _, verb := range []string{"supersede", "update"} {
		attempt := map[string]any{"old_id": id, "new_content": "corrected", "id": id, "content": "corrected", "authority": "model", "project": "retry-scope", "scope_context": true, "expected_version": version, "idempotency_key": "admission-first-" + verb}
		if out := invoke(verb, attempt); out["kind"] != "review_required" {
			t.Fatal("refused edit reached audit writer", verb, out)
		}
		attempt["authority"] = "user"
		attempt["idempotency_key"] = "admission-user-" + verb
		if out := invoke(verb, attempt); out["kind"] != "unavailable" {
			t.Fatal("admitted edit did not reach audit writer", verb, out)
		}
		stale := map[string]any{}
		for k, v := range version.(map[string]any) {
			stale[k] = v
		}
		stale["record_revision"] = "9223372036854775807"
		attempt["expected_version"] = stale
		attempt["idempotency_key"] = "admission-stale-" + verb
		if out := invoke(verb, attempt); out["reason"] != "expected_version_conflict" {
			t.Fatal("stale edit reached audit writer", verb, out)
		}
	}
	exec(`RESET ROLE; ALTER TABLE fact_graph_commits DROP CONSTRAINT retry_fixture_admission; SET LOCAL ROLE aimee_store_runtime`)
	if n := scalar(`SELECT generation FROM memory_collection_generations WHERE scope_type='project' AND scope_value='retry-scope'`); n != generation {
		t.Fatal("admission failure published invalidation", n, generation)
	}
	if n := scalar(`SELECT count(*) FROM memories WHERE id=$1 AND lifecycle_state='active' AND content='original'`, id); n != 1 {
		t.Fatal("admission failure consumed original")
	}
	if n := scalar(`SELECT count(*) FROM memory_mutation_receipts WHERE actor_principal='user:retry-fixture' AND proposal_id IS NOT NULL`); n != 2 {
		t.Fatal("draft outcomes lack retry references", n)
	}
	// A review refusal commits its draft outcome but no open canonical commit.
	args["authority"] = "model"
	if out := invoke("supersede", args); out["kind"] != "review_required" {
		t.Fatal(out)
	}
	if n := scalar(`SELECT count(*) FROM fact_graph_commits WHERE status='open'`); n != 0 {
		t.Fatal("refusal left an open commit", n)
	}
	args["authority"] = "user"
	args["idempotency_key"] = "retry-fixture-key-user"
	accepted := invoke("supersede", args)
	if accepted["status"] != "ok" {
		t.Fatal(accepted)
	}
	receipt := accepted["mutation_receipt"].(map[string]any)
	newID := int64(accepted["memory"].(map[string]any)["id"].(float64))
	if receipt["replayed"] != false || receipt["commit_id"] == "" {
		t.Fatal(receipt)
	}
	if n := scalar(`SELECT count(*) FROM fact_graph_changes WHERE commit_id=$1 AND object_kind='memory'`, receipt["commit_id"]); n < 2 {
		t.Fatal("receipt not linked to canonical row audit", n)
	}
	if n := scalar(`SELECT count(*) FROM fact_graph_commits WHERE commit_id=$1 AND status='applied'`, receipt["commit_id"]); n != 1 {
		t.Fatal("unsealed commit", n)
	}
	before = scalar(`SELECT count(*) FROM fact_graph_commits`)
	generation = scalar(`SELECT generation FROM memory_collection_generations WHERE scope_type='project' AND scope_value='retry-scope'`)
	jobGeneration := scalar(`SELECT generation FROM kb_async_jobs WHERE kind='memory_facts' AND document_id=$1`, newID)
	// Presentation and connection changes cannot turn a retry into a new write.
	caller.TransportIdentity = "cert:reconnected"
	for _, view := range []string{"", "server", "mcp"} {
		args["view"] = view
		replay := invoke("supersede", args)
		rr, ok := replay["mutation_receipt"].(map[string]any)
		if !ok || replay["status"] != "ok" || rr["replayed"] != true || rr["commit_id"] != receipt["commit_id"] {
			t.Fatal(replay)
		}
	}
	delete(args, "view")
	if n := scalar(`SELECT count(*) FROM fact_graph_commits`); n != before {
		t.Fatal("retry duplicated canonical audit", n, before)
	}
	exec(`UPDATE memories SET use_count=use_count+1 WHERE id=$1`, newID)
	before = scalar(`SELECT count(*) FROM fact_graph_commits`)
	if replay := invoke("supersede", args); replay["status"] != "ok" {
		t.Fatal(replay)
	}
	if n := scalar(`SELECT count(*) FROM fact_graph_commits`); n != before {
		t.Fatal("retry duplicated canonical audit", n, before)
	}
	if n := scalar(`SELECT generation FROM memory_collection_generations WHERE scope_type='project' AND scope_value='retry-scope'`); n != generation {
		t.Fatal("retry published invalidation", n, generation)
	}
	if n := scalar(`SELECT generation FROM kb_async_jobs WHERE kind='memory_facts' AND document_id=$1`, newID); n != jobGeneration {
		t.Fatal("retry requeued extraction", n, jobGeneration)
	}
	args["new_content"] = "different"
	if out := invoke("supersede", args); out["reason"] != "idempotency_conflict" {
		t.Fatal(out)
	}
	args["new_content"] = "corrected"
	caller.Principal = "user:other-retry-fixture"
	if out := invoke("supersede", args); out["reason"] != "expected_version_conflict" {
		t.Fatal("another actor obtained receipt", out)
	}
	caller.Principal = "user:retry-fixture"
	// The same request must not release a result which was moved out of scope.
	exec(`RESET ROLE`)
	exec(`UPDATE memories SET scope_value='retry-hidden' WHERE id=$1`, newID)
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	if out := invoke("supersede", args); out["reason"] != "idempotent_result_unavailable" || out["memory"] != nil || out["mutation_receipt"] != nil {
		t.Fatal(out)
	}
	exec(`RESET ROLE`)
	exec(`UPDATE memories SET scope_value='retry-scope' WHERE id=$1`, newID)
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	if out := invoke("supersede", args); out["reason"] != "idempotent_result_unavailable" {
		t.Fatal("changed revision replayed", out)
	}
	exec(`DELETE FROM memories WHERE id=$1`, newID)
	if out := invoke("supersede", args); out["reason"] != "idempotent_result_unavailable" {
		t.Fatal("erased result recreated", out)
	}
	// Receipt lookup is actor-isolated and runtime cannot rewrite completed keys.
	exec(`SELECT set_config('aimee.principal','user:other-retry-fixture',true)`)
	if n := scalar(`SELECT count(*) FROM memory_mutation_receipts WHERE actor_principal='user:retry-fixture'`); n != 0 {
		t.Fatal("receipt RLS leaked", n)
	}
	var forbidden bool
	if e := tx.QueryRow(ctx, `SELECT has_table_privilege(current_user,'memory_mutation_receipts','UPDATE') OR has_table_privilege(current_user,'memory_mutation_receipts','DELETE') OR has_table_privilege(current_user,'memory_mutation_receipts','TRUNCATE')`).Scan(&forbidden); e != nil || forbidden {
		t.Fatal(forbidden, e)
	}
}

// Exercise actual commits and connection loss, not nested test savepoints. The
// waiter must block on the key until the first transaction's outcome is known.
func TestIdempotentMutationConcurrentCommitAndDisconnect(t *testing.T) {
	dsn := os.Getenv("AIMEE_DB2_REPLAY_URL")
	if dsn == "" {
		t.Skip("set AIMEE_DB2_REPLAY_URL")
	}
	for _, test := range []struct {
		operation   string
		commitFirst bool
		authority   int
	}{
		{"insert-epistemic", true, AuthorityUser}, {"insert-epistemic", false, AuthorityUser},
		{"insert-epistemic", true, AuthorityModel}, {"insert-epistemic", false, AuthorityModel},
		{"supersede", true, AuthorityUser}, {"supersede", false, AuthorityUser}, {"update-as", true, AuthorityUser}, {"update-as", false, AuthorityUser},
		{"delete-as", true, AuthorityModel}, {"delete-as", false, AuthorityModel},
		{"delete-as", true, AuthorityUser}, {"delete-as", false, AuthorityUser},
		{"reject", true, AuthorityUser}, {"reject", false, AuthorityUser},
		{"reject", true, AuthorityModel}, {"reject", false, AuthorityModel},
		{"restore", true, AuthorityUser}, {"restore", false, AuthorityUser},
	} {
		operation, commitFirst, authority := test.operation, test.commitFirst, test.authority
		t.Run(fmt.Sprintf("%s/authority=%d/commit=%v", operation, authority, commitFirst), func(t *testing.T) {
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
			scope := Scope{Type: ScopeProject, Value: fmt.Sprintf("retry-concurrent-%d", time.Now().UnixNano())}
			caller := &bus.CommandContext{Authenticated: true, UserAuthority: true, Principal: "user:" + scope.Value, TransportIdentity: "cert:concurrent"}
			defer func() {
				cancel()
				cleanup, stop := context.WithTimeout(context.Background(), 5*time.Second)
				defer stop()
				// Closing connections first releases any failing test's uncommitted locks.
				first.Close(cleanup)
				second.Close(cleanup)
				if _, e := owner.Exec(cleanup, `DELETE FROM memory_mutation_receipts WHERE actor_principal=$1`, caller.Principal); e != nil {
					t.Error(e)
				}
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
			originalConfidence := 0.47
			old, e := backend.InsertEpistemic(ctx, DataRequest{Confidence: &originalConfidence, Scope: scope, Tier: "L2", Kind: "fact", Key: "concurrent", Content: "original", Authority: authority})
			if e != nil {
				t.Fatal(e)
			}
			observed, e := backend.getAtVersioned(ctx, scope, old.ID, false, "", true)
			if e != nil {
				t.Fatal(e)
			}
			if operation == "restore" {
				if ok, err := backend.Reject(ctx, old.ID, "concurrent fixture"); err != nil || !ok {
					t.Fatal(ok, err)
				}
				if err := create.QueryRow(ctx, "SELECT record_revision::text FROM memories WHERE id=$1", old.ID).Scan(&observed.Version.RecordRevision); err != nil {
					t.Fatal(err)
				}
			}
			if e = create.Commit(ctx); e != nil {
				t.Fatal(e)
			}
			confidence := 1.0
			request := DataRequest{Operation: operation, Scope: scope, ID: old.ID, Content: "corrected", Confidence: &confidence, Authority: authority, ExpectedVersion: observed.Version, IdempotencyKey: "concurrent-fixture-key"}
			if operation == "insert-epistemic" {
				request.ID, request.ExpectedVersion = 0, nil
				request.Key, request.Tier, request.Kind = "new-concurrent", "L2", "fact"
			}
			if operation == "update-as" {
				request.Confidence = nil
			}
			begin := func(c *pgx.Conn) (pgx.Tx, *postgresDataStore) {
				t.Helper()
				tx, e := c.Begin(ctx)
				if e != nil {
					t.Fatal(e)
				}
				if _, e = tx.Exec(ctx, `SELECT set_config('aimee.principal',$1,true),set_config('aimee.memory_scope_type','project',true),set_config('aimee.memory_scope_value',$2,true)`, caller.Principal, scope.Value); e != nil {
					t.Fatal(e)
				}
				return tx, &postgresDataStore{db: evalQueryer{tx}, placement: PlacementKB}
			}
			mutate := func(s *postgresDataStore) (Record, *MemoryMutationReceipt, error) {
				if operation == "insert-epistemic" {
					return s.storeKBIdempotent(ctx, request, caller, "")
				}
				if operation == "reject" || operation == "restore" {
					receipt, err := s.lifecycleKBIdempotent(ctx, request, caller, "")
					return Record{ID: request.ID}, receipt, err
				}
				if operation == "delete-as" {
					receipt, err := s.deleteKBIdempotent(ctx, request, authority, caller, "")
					return Record{ID: request.ID}, receipt, err
				}
				return s.replaceKBIdempotent(ctx, request, authority, caller, "")
			}
			if operation == "delete-as" {
				request.Content = ""
				request.Confidence = nil
			}
			a, as := begin(first)
			defer a.Rollback(context.Background())
			b, bs := begin(second)
			defer b.Rollback(context.Background())
			record, receipt, e := mutate(as)
			if e != nil {
				t.Fatal(e)
			}
			type outcome struct {
				row     Record
				receipt *MemoryMutationReceipt
				err     error
			}
			done := make(chan outcome, 1)
			go func() {
				r, c, e := mutate(bs)
				done <- outcome{r, c, e}
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
				case out := <-done:
					t.Fatal("same-key retry bypassed outcome lock", out)
				case <-ctx.Done():
					t.Fatal(ctx.Err())
				case <-time.After(5 * time.Millisecond):
				}
			}
			if commitFirst {
				e = a.Commit(ctx)
			} else {
				e = first.Close(ctx)
			}
			if e != nil {
				t.Fatal(e)
			}
			var result outcome
			select {
			case result = <-done:
			case <-ctx.Done():
				t.Fatal(ctx.Err())
			}
			if result.err != nil || result.receipt == nil || result.receipt.Replayed != commitFirst {
				t.Fatal(result)
			}
			if commitFirst && (result.row.ID != record.ID || result.receipt.CommitID != receipt.CommitID) {
				t.Fatal("retry did not identify first commit", result)
			}
			if !commitFirst && ((operation != "delete-as" && operation != "reject" && operation != "restore" && result.row.ID == record.ID) || result.receipt.CommitID == receipt.CommitID) {
				t.Fatal("uncommitted result survived disconnect", result)
			}
			if e = b.Commit(ctx); e != nil {
				t.Fatal(e)
			}
			// Reconnect after the acknowledged/aborted writer is gone: response loss
			// cannot require an in-process map, and no extraction generation is added.
			third := connect()
			replayTx, replayStore := begin(third)
			defer replayTx.Rollback(context.Background())
			replay, replayed, e := mutate(replayStore)
			if e != nil || replayed == nil || !replayed.Replayed || replay.ID != result.row.ID || replayed.CommitID != result.receipt.CommitID {
				t.Fatal(replay, replayed, e)
			}
			if operation == "update-as" && replay.Confidence != originalConfidence {
				t.Fatal("update changed inherited confidence", replay.Confidence)
			}
			if e = replayTx.Commit(ctx); e != nil {
				t.Fatal(e)
			}
			wantRows, wantJobs := 2, 1
			if operation == "delete-as" {
				wantRows, wantJobs = 1, 0
				if authority == AuthorityUser {
					wantRows = 0
				}
			}
			if operation == "reject" || operation == "restore" {
				wantRows, wantJobs = 1, 0
			}
			var rows, receipts, jobs int
			if e = owner.QueryRow(ctx, `SELECT
    (SELECT count(*) FROM memories WHERE scope_type='project' AND scope_value=$1),
    (SELECT count(*) FROM memory_mutation_receipts WHERE actor_principal=$2),
    (SELECT count(*) FROM kb_async_jobs WHERE kind='memory_facts' AND document_id=$3 AND generation=1)`, scope.Value, caller.Principal, replay.ID).Scan(&rows, &receipts, &jobs); e != nil || rows != wantRows || receipts != 1 || jobs != wantJobs {
				t.Fatal(rows, receipts, jobs, e)
			}
		})
	}
}

func exerciseUpdateRetryReplay(t *testing.T, ctx context.Context, tx pgx.Tx, handler bus.ModuleHandler) {
	t.Helper()
	caller := bus.CommandContext{Authenticated: true, Principal: "user:update-retry", UserAuthority: true, TransportIdentity: "cert:fixture"}
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
	row := invoke("store", map[string]any{"key": "update-retry", "content": "original", "authority": "user", "confidence": 0.37, "project": "update-retry", "scope_context": true})
	if row["status"] != "ok" {
		t.Fatal(row)
	}
	id := int64(row["id"].(float64))
	readArgs := map[string]any{"id": id, "include_version": true, "project": "update-retry", "scope_context": true}
	read := invoke("get", readArgs)
	version := read["memory"].(map[string]any)["version"]
	args := map[string]any{"id": id, "content": "corrected", "authority": "user", "expected_version": version, "idempotency_key": "update-retry-key-01", "project": "update-retry", "scope_context": true}
	args["authority"] = "model"
	args["idempotency_key"] = "update-proposal-key"
	if out := invoke("update", args); out["kind"] != "review_required" {
		t.Fatal("precondition granted authority", out)
	}
	args["authority"] = "user"
	args["idempotency_key"] = "update-retry-key-01"
	args["project"] = "hidden"
	if out := invoke("update", args); out["kind"] != "not_found" {
		t.Fatal(out)
	}
	args["project"] = "update-retry"
	out := invoke("update", args)
	if out["status"] != "ok" || out["superseded"] != true {
		t.Fatal(out)
	}
	newID := int64(out["id"].(float64))
	receipt := out["mutation_receipt"].(map[string]any)
	readArgs["id"] = newID
	current := invoke("get", readArgs)
	if current["memory"].(map[string]any)["confidence"] != 0.37 {
		t.Fatal("update failed to preserve confidence", current)
	}
	var operation string
	if e := tx.QueryRow(ctx, `SELECT operation FROM fact_graph_commits WHERE commit_id=$1`, receipt["commit_id"]).Scan(&operation); e != nil || operation != "memory.update" {
		t.Fatal(operation, e)
	}
	for _, view := range []string{"", "server", "mcp"} {
		args["view"] = view
		replay := invoke("update", args)
		if replay["status"] != "ok" || replay["mutation_receipt"].(map[string]any)["commit_id"] != receipt["commit_id"] || replay["mutation_receipt"].(map[string]any)["replayed"] != true {
			t.Fatal(replay)
		}
		if view == "mcp" && replay["audit_id"] != nil {
			t.Fatal("MCP replay requests duplicate host audit", replay)
		}
	}
	delete(args, "view")
	args["content"] = "different"
	if out := invoke("update", args); out["reason"] != "idempotency_conflict" {
		t.Fatal(out)
	}
	args["content"] = "corrected"
	// The verb is bound even where target, content and requested authority match.
	crossVerb := map[string]any{"old_id": id, "new_content": "corrected", "authority": "user", "expected_version": version, "idempotency_key": "update-retry-key-01", "project": "update-retry", "scope_context": true}
	if out := invoke("supersede", crossVerb); out["reason"] != "idempotency_conflict" {
		t.Fatal("key crossed operation boundary", out)
	}
	delete(args, "idempotency_key")
	if out := invoke("update", args); out["reason"] != "expected_version_conflict" {
		t.Fatal(out)
	}
	// Expected-version update without a retry key uses the same locked admission.
	args["id"] = newID
	args["expected_version"] = current["memory"].(map[string]any)["version"]
	args["content"] = "next"
	next := invoke("update", args)
	if next["status"] != "ok" || next["id"] == float64(newID) || next["mutation_receipt"] != nil {
		t.Fatal(next)
	}
	args["id"] = id
	args["expected_version"] = version
	args["content"] = "corrected"
	args["idempotency_key"] = "update-retry-key-01"
	if out := invoke("update", args); out["reason"] != "idempotent_result_unavailable" || out["id"] != nil {
		t.Fatal("replayed obsolete update result", out)
	}
}

// This value was produced by the original schema-one supersede digest. Extending
// supported verbs must not invalidate receipts already committed by that writer.
func TestCorrectionDigestCompatibility(t *testing.T) {
	confidence := 0.7
	request := DataRequest{Operation: "supersede", ID: 42, Content: "corrected", Confidence: &confidence, SessionID: "session", Scope: Scope{Type: ScopeProject, Value: "app"}, Project: "app", ExpectedVersion: &MemoryRecordVersion{SchemaVersion: 1, OwnerID: "00000000-0000-0000-0000-000000000001", RecordID: "42", RecordRevision: "3"}}
	got, err := correctionDigest(request, AuthorityUser)
	if err != nil || got != "d6cbd202c768aa87459f2e921702ae269675e347e750b4f3c5c39937791324ea" {
		t.Fatal(got, err)
	}
	request.Operation = "update-as"
	other, err := correctionDigest(request, AuthorityUser)
	if err != nil || got == other {
		t.Fatal("operation not bound", other, err)
	}
}
