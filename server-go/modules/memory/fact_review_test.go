package memory

import (
	"context"
	"encoding/json"
	"strconv"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func exerciseFactReviewReplay(t *testing.T, ctx context.Context, tx pgx.Tx, s *postgresDataStore) {
	t.Helper()
	user := FactActor{Principal: "test:review-user", TransportIdentity: "transport:review-user", Role: "user", Rank: 30, Authenticated: 1}
	seed := func(subject, object, source string, actor FactActor) factMutationResult {
		t.Helper()
		r, _, err := s.commitFactCandidate(ctx, FactCandidate{Subject: subject, Relation: "works_for", Object: object, SubjectKind: NodePerson, ObjectKind: NodeOrg, Actor: actor, Evidence: FactEvidence{SourceKind: "observation", SourceID: source}, AssertionKind: "world_fact"})
		if err != nil {
			t.Fatal(err)
		}
		return r
	}
	prior := seed("GoReview Alice", "Incumbent", "review-original", user)
	candidate := seed("GoReview Alice", "Replacement", "review-new", modelFactActor())
	if !candidate.Quarantined {
		t.Fatal(candidate)
	}
	handler := NewHandler(nil, WithDataStore(PlacementKB, &postgresDataStore{db: runtimeRoleDB{evalQueryer{tx}, t}, placement: PlacementKB}))
	operator := bus.CommandContext{Authenticated: true, UserAuthority: true, Principal: "operator:review", TransportIdentity: "verified:console"}
	call := func(id int64, action string, peer uint32, actor bus.CommandContext) map[string]any {
		t.Helper()
		raw, _ := json.Marshal(map[string]any{"operation": "fact-review", "id": id, "action": action, "actor": "forged"})
		result, status := invokeContextCommand(t, handler, peer, actor, "runtime", string(raw))
		if peer != 0 {
			if status != bus.ModuleStatusInvalidRequest {
				t.Fatal("remote review accepted", result, status)
			}
			return nil
		}
		if status != bus.ModuleStatusOK {
			t.Fatal(result, status)
		}
		return result
	}
	call(candidate.AssertionID, "approve", 200, operator)
	if r := call(candidate.AssertionID, "approve", 0, bus.CommandContext{}); r["kind"] != "unauthorized" {
		t.Fatal(r)
	}
	if r := call(candidate.AssertionID, "approve", 0, operator); r["status"] != "ok" || r["lifecycle"] != "promoted" {
		t.Fatal(r)
	}
	state := func(id int64) string {
		t.Helper()
		var value string
		if err := tx.QueryRow(ctx, `SELECT lifecycle_state FROM entity_edges WHERE id=$1`, id).Scan(&value); err != nil {
			t.Fatal(err)
		}
		return value
	}
	if state(prior.AssertionID) != "superseded" {
		t.Fatal("approval left functional incumbent live")
	}
	if r := call(candidate.AssertionID, "undo", 0, operator); r["status"] != "ok" || r["lifecycle"] != "candidate" {
		t.Fatal(r)
	}
	if state(prior.AssertionID) != "persistent" {
		t.Fatal("undo did not restore incumbent")
	}
	if r := call(candidate.AssertionID, "reject", 0, operator); r["status"] != "ok" {
		t.Fatal(r)
	}
	if r := call(candidate.AssertionID, "approve", 0, operator); r["kind"] != "conflict" {
		t.Fatal("approval bypassed rejection", r)
	}
	if state(candidate.AssertionID) != "invalidated" {
		t.Fatal("failed approval changed rejection")
	}
	if r := call(candidate.AssertionID, "undo", 0, operator); r["status"] != "ok" {
		t.Fatal(r)
	}
	// An independent assertion after approval makes the old review undo stale.
	if r := call(candidate.AssertionID, "approve", 0, operator); r["status"] != "ok" {
		t.Fatal(r)
	}
	seed("GoReview Alice", "Replacement", "review-independent", user)
	if r := call(candidate.AssertionID, "undo", 0, operator); r["kind"] != "conflict" {
		t.Fatal("stale undo erased independent evidence", r)
	}
	if state(candidate.AssertionID) != "promoted" {
		t.Fatal("conflicting undo partially applied")
	}
	if r := call(999999999999, "approve", 0, operator); r["kind"] != "not_found" {
		t.Fatal(r)
	}
	prior2 := seed("GoReview Bob", "Prior Corp", "review-bob-old", user)
	candidate2 := seed("GoReview Bob", "Next Corp", "review-bob-new", modelFactActor())
	if r := call(candidate2.AssertionID, "approve", 0, operator); r["status"] != "ok" {
		t.Fatal(r)
	}
	seed("GoReview Bob", "Prior Corp", "review-bob-independent", modelFactActor())
	if r := call(candidate2.AssertionID, "undo", 0, operator); r["kind"] != "conflict" {
		t.Fatal("undo overwrote touched incumbent", r)
	}
	if state(prior2.AssertionID) != "superseded" || state(candidate2.AssertionID) != "promoted" {
		t.Fatal("failed undo partially committed")
	}
	sql := func(q string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, q, args...); err != nil {
			t.Fatal(err)
		}
	}
	sql(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	var privateID int64
	if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value) VALUES('L0','fact','private-fact-review','private source','project','private-review') RETURNING id`).Scan(&privateID); err != nil {
		t.Fatal(err)
	}
	private, _, err := s.commitFactCandidate(ctx, FactCandidate{Subject: "GoReview Private", Relation: "works_for", Object: "Private Corp", SubjectKind: NodePerson, ObjectKind: NodeOrg, Actor: modelFactActor(), Evidence: FactEvidence{SourceKind: "memory", SourceID: "memory:" + strconv.FormatInt(privateID, 10)}, AssertionKind: "world_fact"})
	if err != nil {
		t.Fatal(err)
	}
	sql(`SELECT set_config('aimee.memory_scope_all','0',true),set_config('aimee.memory_scope_type','global',true),set_config('aimee.memory_scope_value','_global',true)`)
	list, err := s.factCandidates(ctx, 64)
	if err != nil {
		t.Fatal(err)
	}
	for _, row := range list {
		if row["id"] == private.AssertionID {
			t.Fatal("private review candidate leaked", row)
		}
	}
	raw, _ := json.Marshal(map[string]any{"operation": "fact-review", "id": private.AssertionID, "action": "approve", "scope_context": true})
	r, status := invokeContextCommand(t, handler, 0, operator, "runtime", string(raw))
	if status != bus.ModuleStatusOK || r["kind"] != "not_found" {
		t.Fatal("private assertion reviewed outside scope", r, status)
	}
	sql(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	var principal string
	if err := tx.QueryRow(ctx, `SELECT actor_principal FROM fact_review_actions WHERE assertion_id=$1 ORDER BY id DESC LIMIT 1`, candidate.AssertionID).Scan(&principal); err != nil || principal != operator.Principal {
		t.Fatal("unverified actor", principal, err)
	}
}
