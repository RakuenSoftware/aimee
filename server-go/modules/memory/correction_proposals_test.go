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

func installProposalFixture(t *testing.T, ctx context.Context, tx pgx.Tx) {
	t.Helper()
	schema, err := os.ReadFile("../../../src/modules/db2/c/schema.sql")
	if err != nil {
		t.Fatal(err)
	}
	body := string(schema)
	a := strings.Index(body, "CREATE TABLE IF NOT EXISTS memory_correction_proposals (")
	b := strings.Index(body[a:], "CREATE INDEX IF NOT EXISTS memory_correction_proposals_pending") + a
	ddl := strings.Replace(body[a:b], "CREATE TABLE IF NOT EXISTS", "CREATE TEMP TABLE", 1)
	_, err = tx.Exec(ctx, `ALTER TABLE memories ADD COLUMN record_revision bigint NOT NULL DEFAULT 1;
 CREATE TEMP TABLE memory_collection_owner(id int PRIMARY KEY,owner_id uuid);
 INSERT INTO memory_collection_owner VALUES(1,gen_random_uuid());
 CREATE TEMP TABLE fact_graph_commits(commit_id text PRIMARY KEY);`+ddl)
	if err != nil {
		t.Fatal(err)
	}
}

func TestCorrectionProposalRuntimeReplay(t *testing.T) {
	dsn := os.Getenv("AIMEE_DB2_REPLAY_URL")
	if dsn == "" {
		if os.Getenv("AIMEE_MEMORY_REPLAY_REQUIRED") == "1" {
			t.Fatal("AIMEE_DB2_REPLAY_URL required")
		}
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
	if _, err = tx.Exec(ctx, `DO $$ BEGIN
 IF NOT EXISTS(SELECT 1 FROM pg_roles WHERE rolname='aimee_store_runtime') THEN
 CREATE ROLE aimee_store_runtime NOINHERIT NOBYPASSRLS;
 END IF; END $$; GRANT USAGE ON SCHEMA public TO aimee_store_runtime`); err != nil {
		t.Fatal(err)
	}
	schema, err := os.ReadFile("../../../src/modules/db2/c/schema.sql")
	if err != nil {
		t.Fatal(err)
	}
	body := string(schema)
	a, b := strings.Index(body, "DO $memory_store_grants$"), strings.Index(body, "END\n$memory_store_grants$;")
	if a < 0 || b < a {
		t.Fatal("runtime grant migration missing")
	}
	if _, err = tx.Exec(ctx, body[a:b+len("END\n$memory_store_grants$;")]); err != nil {
		t.Fatal(err)
	}
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, "RESET ROLE"); err != nil {
			t.Fatal(err)
		}
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
		if _, err := tx.Exec(ctx, "SET LOCAL ROLE aimee_store_runtime"); err != nil {
			t.Fatal(err)
		}
	}
	scalar := func(sql string, args ...any) int64 {
		t.Helper()
		if _, err := tx.Exec(ctx, "RESET ROLE"); err != nil {
			t.Fatal(err)
		}
		var n int64
		if err := tx.QueryRow(ctx, sql, args...).Scan(&n); err != nil {
			t.Fatal(err)
		}
		if _, err := tx.Exec(ctx, "SET LOCAL ROLE aimee_store_runtime"); err != nil {
			t.Fatal(err)
		}
		return n
	}
	exec("SELECT 1")
	handler := NewHandler(nil, WithDataStore(PlacementKB, &postgresDataStore{db: runtimeRoleDB{evalQueryer{tx}, t}, placement: PlacementKB}))
	user := bus.CommandContext{Authenticated: true, UserAuthority: true, Principal: "user:proposal-review", TransportIdentity: "cert:operator"}
	model := bus.CommandContext{Authenticated: true, Principal: "model:proposal-writer", TransportIdentity: "cert:model"}
	invoke := func(caller bus.CommandContext, verb string, args map[string]any) map[string]any {
		t.Helper()
		raw, e := json.Marshal(args)
		if e != nil {
			t.Fatal(e)
		}
		out, status := invokeContextCommand(t, handler, 0, caller, verb, string(raw))
		if status != bus.ModuleStatusOK {
			t.Fatal(verb, status, out)
		}
		return out
	}
	scope := "proposal-fixture"
	args := func(values map[string]any) map[string]any {
		values["project"] = scope
		values["scope_context"] = true
		return values
	}
	created := invoke(user, "store", args(map[string]any{"key": "proposal-original", "content": "authoritative original", "authority": "user", "confidence": .95, "tier": "L2"}))
	if created["status"] != "ok" {
		t.Fatal(created)
	}
	id := int64(created["id"].(float64))
	read := invoke(user, "get", args(map[string]any{"id": id, "include_version": true}))
	version := read["memory"].(map[string]any)["version"]
	generation := scalar(`SELECT generation FROM memory_collection_generations WHERE scope_type='project' AND scope_value=$1`, scope)
	correction := args(map[string]any{"id": id, "content": "proposed replacement", "authority": "user", "expected_version": version, "idempotency_key": "proposal-keyed-edit"})
	first := invoke(model, "update", correction)
	if first["kind"] != "review_required" || first["proposal"] == nil {
		t.Fatal(first)
	}
	proposal := first["proposal"].(map[string]any)
	if proposal["state"] != "pending" || proposal["origin_authority"] != "model" || proposal["draft"] != nil {
		t.Fatal(proposal)
	}
	pid, digest := proposal["proposal_id"], proposal["payload_digest"]
	if scalar(`SELECT count(*) FROM memories WHERE id=$1 AND content='authoritative original' AND provenance_category='user_stated' AND lifecycle_state='active'`, id) != 1 || scalar(`SELECT count(*) FROM memories WHERE content='proposed replacement'`) != 0 {
		t.Fatal("proposal changed recall records")
	}
	if scalar(`SELECT generation FROM memory_collection_generations WHERE scope_type='project' AND scope_value=$1`, scope) != generation {
		t.Fatal("draft invalidated canonical memory")
	}
	if scalar(`SELECT count(*) FROM fact_graph_changes WHERE object_kind='review' AND object_key=$1 AND after_state::jsonb->>'payload_digest'=$2`, pid, digest) != 1 {
		t.Fatal("proposal lacks exact digest audit")
	}
	if scalar(`SELECT count(*) FROM memory_mutation_receipts r JOIN memories m ON m.id=r.result_id WHERE r.proposal_id=$1::uuid AND r.result_revision=0 AND r.result_revision<>m.record_revision`, pid) != 1 {
		t.Fatal("older retry reader could mistake draft for canonical correction")
	}
	retry := invoke(model, "update", correction)
	if retry["proposal"].(map[string]any)["proposal_id"] != pid || scalar(`SELECT count(*) FROM memory_correction_proposals WHERE target_id=$1`, id) != 1 {
		t.Fatal("retry duplicated draft", retry)
	}
	for _, verb := range []string{"store", "supersede"} {
		variant := args(map[string]any{"key": "proposal-original", "tier": "L2", "content": "proposed replacement", "old_id": id, "new_content": "proposed replacement"})
		out := invoke(model, verb, variant)
		if out["kind"] != "review_required" || out["proposal"].(map[string]any)["proposal_id"] != pid {
			t.Fatal("canonical entry points did not share proposal", verb, out)
		}
	}
	correction["content"] = "changed same retry key"
	if r := invoke(model, "update", correction); r["reason"] != "idempotency_conflict" {
		t.Fatal(r)
	}
	correction["content"] = "proposed replacement"
	listed := invoke(user, "correction_proposals", args(map[string]any{"proposal_id": pid}))
	draft := listed["proposals"].([]any)[0].(map[string]any)["draft"].(map[string]any)
	if draft["content"] != "proposed replacement" || draft["confidence"] != .8 {
		t.Fatal(draft)
	}
	review := args(map[string]any{"proposal_id": pid, "payload_digest": digest, "expected_version": version, "action": "approve"})
	if r := invoke(model, "review_correction", review); r["kind"] != "forbidden" {
		t.Fatal("model approved draft", r)
	}
	review["payload_digest"] = strings.Repeat("a", 64)
	if r := invoke(user, "review_correction", review); r["kind"] != "conflict" {
		t.Fatal(r)
	}
	review["payload_digest"] = digest
	review["project"] = "other-project"
	if r := invoke(user, "review_correction", review); r["kind"] != "not_found" {
		t.Fatal(r)
	}
	hidden := invoke(user, "correction_proposals", map[string]any{"proposal_id": pid, "project": "other-project", "scope_context": true})
	if len(hidden["proposals"].([]any)) != 0 {
		t.Fatal("draft escaped parent scope", hidden)
	}
	review["project"] = scope
	if r := invoke(user, "correction_proposals", args(map[string]any{"proposal_id": pid})); len(r["proposals"].([]any)) != 1 {
		t.Fatal("guard fixture must be visible", r)
	}
	queue := invoke(user, "correction_proposals", args(map[string]any{"limit": 100}))
	if len(queue["proposals"].([]any)) != 1 || queue["proposals"].([]any)[0].(map[string]any)["draft"] != nil {
		t.Fatal("queue fetched draft payloads", queue)
	}
	for _, ownerEdit := range []bool{false, true} {
		savepoint, e := tx.Begin(ctx)
		if e != nil {
			t.Fatal(e)
		}
		if ownerEdit {
			if _, e = savepoint.Exec(ctx, "RESET ROLE"); e != nil {
				t.Fatal(e)
			}
		}
		_, e = savepoint.Exec(ctx, `UPDATE memory_correction_proposals SET payload='{}' WHERE proposal_id=$1::uuid`, pid)
		if e == nil {
			t.Fatal("draft payload was mutable", ownerEdit)
		}
		if e = savepoint.Rollback(ctx); e != nil {
			t.Fatal(e)
		}
	}
	savepoint, e := tx.Begin(ctx)
	if e != nil {
		t.Fatal(e)
	}
	if _, e = savepoint.Exec(ctx, `SELECT set_config('aimee.authority','model',true),set_config('aimee.principal','model:proposal-writer',true)`); e != nil {
		t.Fatal(e)
	}
	_, e = savepoint.Exec(ctx, `UPDATE memory_correction_proposals SET state='rejected',reviewer_principal='model:proposal-writer',decision_id='forged' WHERE proposal_id=$1::uuid`, pid)
	if e == nil {
		t.Fatal("model SQL bypassed review guard")
	}
	if e = savepoint.Rollback(ctx); e != nil {
		t.Fatal(e)
	}
	var forbidden bool
	if e = tx.QueryRow(ctx, `SELECT has_table_privilege(current_user,'memory_correction_proposals','DELETE') OR has_table_privilege(current_user,'memory_correction_proposals','TRUNCATE') OR has_column_privilege(current_user,'memory_correction_proposals','payload','UPDATE')`).Scan(&forbidden); e != nil || forbidden {
		t.Fatal("proposal runtime grants", forbidden, e)
	}
	// Fail after canonical replacement and extraction have run. The approval,
	// original, jobs, decision and audit must remain an atomic transaction.
	exec(`ALTER TABLE knowledge_review_decisions ADD CONSTRAINT proposal_fixture_failure CHECK(source_queue<>'memory_correction') NOT VALID`)
	if r := invoke(user, "review_correction", review); r["kind"] != "unavailable" {
		t.Fatal(r)
	}
	exec(`ALTER TABLE knowledge_review_decisions DROP CONSTRAINT proposal_fixture_failure`)
	if scalar(`SELECT count(*) FROM memories WHERE content='proposed replacement'`) != 0 || scalar(`SELECT count(*) FROM memory_correction_proposals WHERE proposal_id=$1::uuid AND state='pending'`, pid) != 1 || scalar(`SELECT generation FROM memory_collection_generations WHERE scope_type='project' AND scope_value=$1`, scope) != generation {
		t.Fatal("failed approval escaped transaction")
	}
	approved := invoke(user, "review_correction", review)
	if approved["status"] != "ok" {
		t.Fatal(approved)
	}
	ap := approved["proposal"].(map[string]any)
	result := ap["result_version"].(map[string]any)
	next := result["record_id"]
	if ap["state"] != "approved" || ap["origin_authority"] != "model" || ap["revision_authority"] != "model" || ap["review_authority"] != "user" || ap["reviewer"] != user.Principal {
		t.Fatal(ap)
	}
	if scalar(`SELECT count(*) FROM memories m JOIN memory_fact_actors a ON a.memory_id=m.id WHERE m.id=$1::bigint AND m.provenance_category='reviewed_model' AND m.confidence=.8 AND a.actor_role='model' AND a.authority_rank=10 AND a.authenticated=0`, next) != 1 {
		t.Fatal("approval laundered draft authorship/confidence")
	}
	if scalar(`SELECT count(*) FROM knowledge_review_decisions WHERE item_id=$1 AND source_queue='memory_correction' AND evidence_snapshot::jsonb->>'payload_digest'=$2 AND authenticated_actor=$3`, pid, digest, user.Principal) != 1 {
		t.Fatal("missing exact approval decision")
	}
	if scalar(`SELECT count(*) FROM fact_graph_changes WHERE commit_id=$1 AND object_kind='memory' AND action='insert' AND after_state::jsonb->>'provenance_category'='reviewed_model'`, ap["review_commit_id"]) != 1 {
		t.Fatal("canonical audit lacks model authorship")
	}
	if scalar(`SELECT count(*) FROM memories WHERE id=$1 AND provenance_category='user_stated' AND content='authoritative original' AND lifecycle_state='superseded'`, id) != 1 {
		t.Fatal("approval erased original author")
	}
	count := scalar(`SELECT count(*) FROM fact_graph_commits`)
	if r := invoke(user, "review_correction", review); r["status"] != "ok" || r["proposal"].(map[string]any)["review_commit_id"] != ap["review_commit_id"] {
		t.Fatal(r)
	}
	if scalar(`SELECT count(*) FROM fact_graph_commits`) != count {
		t.Fatal("approval replay duplicated audit")
	}
	if r := invoke(model, "update", correction); r["proposal"].(map[string]any)["state"] != "approved" {
		t.Fatal(r)
	}
	// Approved model text remains protected; a subsequent model edit is a new
	// linked proposal, never an unreviewed overwrite of the reviewed version.
	second := invoke(model, "update", args(map[string]any{"id": next, "content": "follow-up draft"}))
	if second["kind"] != "review_required" || second["proposal"] == nil {
		t.Fatal(second)
	}
	sp := second["proposal"].(map[string]any)
	rejection := args(map[string]any{"proposal_id": sp["proposal_id"], "payload_digest": sp["payload_digest"], "expected_version": sp["target_version"], "action": "reject"})
	if r := invoke(user, "review_correction", rejection); r["status"] != "ok" || r["proposal"].(map[string]any)["state"] != "rejected" {
		t.Fatal(r)
	}
	if r := invoke(model, "update", args(map[string]any{"id": next, "content": "follow-up draft"})); r["proposal"].(map[string]any)["state"] != "rejected" {
		t.Fatal("rejected draft reopened", r)
	}
	otherModel := model
	otherModel.Principal = "model:another-proposer"
	if r := invoke(otherModel, "update", args(map[string]any{"id": next, "content": "follow-up draft"})); r["proposal"].(map[string]any)["proposal_id"] != sp["proposal_id"] || r["proposal"].(map[string]any)["state"] != "rejected" {
		t.Fatal("changing proposer reopened rejected draft", r)
	}
	rejection["action"] = "approve"
	if r := invoke(user, "review_correction", rejection); r["kind"] != "conflict" {
		t.Fatal("rejected draft approved", r)
	}
	// A scope-tag mutation alone invalidates a pending draft's reviewed target.
	stale := invoke(model, "update", args(map[string]any{"id": next, "content": "stale draft"}))["proposal"].(map[string]any)
	exec(`INSERT INTO memory_scopes(memory_id,scope_type,scope_value) VALUES($1::bigint,'workspace','proposal-revision-change')`, next)
	if r := invoke(user, "review_correction", args(map[string]any{"proposal_id": stale["proposal_id"], "payload_digest": stale["payload_digest"], "expected_version": stale["target_version"], "action": "approve"})); r["reason"] != "expected_version_conflict" {
		t.Fatal(r)
	}
	if r := invoke(model, "update", correction); r["reason"] != "idempotent_result_unavailable" {
		t.Fatal("obsolete result escaped proposal replay", r)
	}
	metadata := invoke(model, "store", args(map[string]any{"key": "proposal-original", "content": "reviewed metadata draft", "tier": "L5", "use_cases": "explicit use", "epistemic_kind": "mental_model", "confidence": 1}))["proposal"].(map[string]any)
	resultMetadata := invoke(user, "review_correction", args(map[string]any{"proposal_id": metadata["proposal_id"], "payload_digest": metadata["payload_digest"], "expected_version": metadata["target_version"], "action": "approve"}))
	if resultMetadata["status"] != "ok" {
		t.Fatal(resultMetadata)
	}
	metadataID := resultMetadata["proposal"].(map[string]any)["result_version"].(map[string]any)["record_id"]
	if scalar(`SELECT count(*) FROM memories WHERE id=$1::bigint AND tier='L5' AND use_cases='explicit use' AND epistemic_kind='mental_model' AND confidence=.5 AND confidence_ceiling=.5 AND provenance_category='reviewed_model'`, metadataID) != 1 {
		t.Fatal("approved metadata diverged from draft")
	}
	// Cascading erasure removes draft content but never releases its retry key.
	if r := invoke(user, "delete", args(map[string]any{"id": id, "authority": "user"})); r["status"] != "ok" {
		t.Fatal(r)
	}
	if scalar(`SELECT count(*) FROM memory_correction_proposals WHERE proposal_id=$1::uuid`, pid) != 0 {
		t.Fatal("erasure retained draft text")
	}
	if r := invoke(model, "update", correction); r["reason"] != "idempotent_result_unavailable" {
		t.Fatal(r)
	}
	if scalar(`SELECT count(*) FROM memory_mutation_receipts WHERE proposal_id=$1::uuid`, pid) != 1 {
		t.Fatal("erasure released retry identity")
	}
}

// Real connections prove that approval and rejection share the parent lock,
// and that two approvals do not create two successors or two review decisions.
func TestCorrectionProposalConcurrentReview(t *testing.T) {
	dsn := os.Getenv("AIMEE_DB2_REPLAY_URL")
	if dsn == "" {
		t.Skip("set AIMEE_DB2_REPLAY_URL")
	}
	for _, secondAction := range []string{"approve", "reject"} {
		t.Run(secondAction, func(t *testing.T) {
			ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
			defer cancel()
			connect := func() *pgx.Conn {
				t.Helper()
				c, e := pgx.Connect(ctx, dsn)
				if e != nil {
					t.Fatal(e)
				}
				return c
			}
			owner, first, second := connect(), connect(), connect()
			defer owner.Close(context.Background())
			scope := Scope{Type: ScopeProject, Value: fmt.Sprintf("proposal-concurrent-%d", time.Now().UnixNano())}
			defer func() {
				cleanup, stop := context.WithTimeout(context.Background(), 5*time.Second)
				defer stop()
				first.Close(cleanup)
				second.Close(cleanup)
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
			old, e := backend.InsertEpistemic(ctx, DataRequest{Scope: scope, Tier: "L2", Kind: "fact", Key: "original", Content: "user original", Authority: AuthorityUser})
			if e != nil {
				t.Fatal(e)
			}
			_, e = backend.replaceKBCorrection(ctx, old.ID, "model correction", nil, "", AuthorityModel, nil, nil)
			p := proposedCorrection(e)
			if p == nil {
				t.Fatal(e)
			}
			if e = create.Commit(ctx); e != nil {
				t.Fatal(e)
			}
			caller := &bus.CommandContext{Authenticated: true, UserAuthority: true, Principal: "user:concurrent-review", TransportIdentity: "cert:review"}
			request := correctionReviewRequest{ProposalID: p.ID, Digest: p.Digest, Expected: p.Target, Action: "approve"}
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
			before, e := (&postgresDataStore{db: evalQueryer{a}, placement: PlacementKB}).reviewKBCorrection(ctx, request, caller)
			if e != nil {
				t.Fatal(e)
			}
			type outcome struct {
				p   correctionProposal
				err error
			}
			done := make(chan outcome, 1)
			request.Action = secondAction
			go func() {
				p, e := (&postgresDataStore{db: evalQueryer{b}, placement: PlacementKB}).reviewKBCorrection(ctx, request, caller)
				done <- outcome{p, e}
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
					t.Fatal("review bypassed parent lock", out)
				case <-ctx.Done():
					t.Fatal(ctx.Err())
				case <-time.After(5 * time.Millisecond):
				}
			}
			if e = a.Commit(ctx); e != nil {
				t.Fatal(e)
			}
			select {
			case out := <-done:
				if secondAction == "approve" {
					if out.err != nil || !out.p.Replayed || out.p.CommitID != before.CommitID || *out.p.Result != *before.Result {
						t.Fatal(out)
					}
					if e = b.Commit(ctx); e != nil {
						t.Fatal(e)
					}
				} else {
					if !errors.Is(out.err, errCorrectionReviewConflict) {
						t.Fatal(out)
					}
					if e = b.Rollback(ctx); e != nil {
						t.Fatal(e)
					}
				}
			case <-ctx.Done():
				t.Fatal(ctx.Err())
			}
			var records, decisions int
			if e = owner.QueryRow(ctx, `SELECT (SELECT count(*) FROM memories WHERE scope_type='project' AND scope_value=$1),(SELECT count(*) FROM knowledge_review_decisions WHERE item_id=$2)`, scope.Value, p.ID).Scan(&records, &decisions); e != nil || records != 2 || decisions != 1 {
				t.Fatal(records, decisions, e)
			}
		})
	}
}
