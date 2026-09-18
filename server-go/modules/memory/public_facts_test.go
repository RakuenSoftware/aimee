package memory

import (
	"context"
	"encoding/json"
	"strconv"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func exerciseExplicitRetractionReplay(t *testing.T, ctx context.Context, tx pgx.Tx, s *postgresDataStore) {
	t.Helper()
	sql := func(q string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, q, args...); err != nil {
			t.Fatal(err)
		}
	}
	sql(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	sql(`INSERT INTO rel_types(rel_type,status) VALUES('lives_in','active'),('parent_of','active'),('child_of','active'),('also_known_as','active'),('member_of','active') ON CONFLICT DO NOTHING`)
	user := FactActor{Principal: "test:explicit-user", TransportIdentity: "verified:explicit", Role: "user", Rank: 30, Authenticated: 1}
	seed := func(relation, object, source string) factMutationResult {
		t.Helper()
		kind := NodeOrg
		if relation == "born_in" {
			kind = NodePlace
		}
		if relation == "parent_of" || relation == "child_of" {
			kind = NodePerson
		}
		evidenceKind := "observation"
		if relation == "lives_in" {
			kind = NodePlace
			evidenceKind = "memory"
		}
		r, _, err := s.commitFactCandidate(ctx, FactCandidate{Subject: "explicit user", Relation: relation, Object: object, SubjectKind: NodePerson, ObjectKind: kind, Actor: user, AssertionKind: "world_fact", Evidence: FactEvidence{SourceKind: evidenceKind, SourceID: source}})
		if err != nil {
			t.Fatal(err)
		}
		return r
	}
	work := seed("works_for", "Explicit Corp", "explicit-work")
	born := seed("born_in", "Explicit City", "explicit-born")
	handler := NewHandler(nil, WithDataStore(PlacementKB, &postgresDataStore{db: runtimeRoleDB{evalQueryer{tx}, t}, placement: PlacementKB}))
	verified := bus.CommandContext{Authenticated: true, UserAuthority: true, Principal: user.Principal, TransportIdentity: user.TransportIdentity}
	call := func(relation, target, authority string, caller bus.CommandContext) map[string]any {
		t.Helper()
		raw, _ := json.Marshal(map[string]any{"source": "explicit user", "relation": relation, "target": target, "authority": authority, "scope_context": true, "actor": "forged", "user_authority": true})
		r, status := invokeContextCommand(t, handler, 0, caller, "retract", string(raw))
		if status != bus.ModuleStatusOK {
			t.Fatal(r, status)
		}
		return r
	}
	for _, test := range []struct {
		authority string
		caller    bus.CommandContext
	}{{"user", bus.CommandContext{}}, {"user", bus.CommandContext{Authenticated: true, Principal: "remote-owner", UserAuthority: false}}, {"model", verified}, {"operator", verified}} {
		r := call("worksFor", "", test.authority, test.caller)
		if r["status"] != "ok" || r["retracted"] != float64(0) || r["authority"] != "model" {
			t.Fatal("authority elevated", r)
		}
	}
	if r := call("born_in", "", "user", bus.CommandContext{}); r["reason"] != "immutable" {
		t.Fatal(r)
	}
	if r := call("born_in", "Explicit City", "user", verified); r["status"] != "ok" || r["retracted"] != float64(1) {
		t.Fatal("verified user immutable correction refused", r)
	}
	for _, relation := range []string{"parent_of", "child_of"} {
		seed(relation, "Family Member", "explicit-"+relation)
		if r := call(relation, "", "model", verified); r["reason"] != "immutable" {
			t.Fatal(r)
		}
		if r := call(relation, "", "user", verified); r["retracted"] != float64(1) {
			t.Fatal(r)
		}
	}
	if r := call("worksFor", "wrong target", "user", verified); r["retracted"] != float64(0) {
		t.Fatal(r)
	}
	if r := call("worksFor", "Explicit Corp", "user", verified); r["status"] != "ok" || r["retracted"] != float64(1) || r["authority"] != "user" {
		t.Fatal(r)
	}
	if r := call("works_for", "Explicit Corp", "user", verified); r["retracted"] != float64(0) {
		t.Fatal("repeated correction changed history", r)
	}
	// Nonfunctional relations preserve sibling values and still respect the
	// authority of each assertion, regardless of the old hard-delete label.
	seed("member_of", "First Group", "explicit-member-first")
	second := seed("member_of", "Second Group", "explicit-member-second")
	if r := call("member_of", "First Group", "user", verified); r["retracted"] != float64(1) {
		t.Fatal(r)
	}
	var siblingState string
	if err := tx.QueryRow(ctx, `SELECT lifecycle_state FROM entity_edges WHERE id=$1`, second.AssertionID).Scan(&siblingState); err != nil || siblingState != "persistent" {
		t.Fatal(siblingState, err)
	}
	for _, actor := range []FactActor{modelFactActor(), user} {
		target := "Alias " + actor.Role
		_, _, err := s.commitFactCandidate(ctx, FactCandidate{Subject: "explicit user", Relation: "also_known_as", Object: target, SubjectKind: NodePerson, ObjectKind: NodeOther, Actor: actor, AssertionKind: "world_fact", Evidence: FactEvidence{SourceKind: "observation", SourceID: "explicit-alias-" + actor.Role}})
		if err != nil {
			t.Fatal(err)
		}
		r := call("also_known_as", target, "model", verified)
		want := float64(1)
		if actor.Rank == 30 {
			want = 0
		}
		if r["retracted"] != want {
			t.Fatal(r)
		}
	}
	var principal string
	if err := tx.QueryRow(ctx, `SELECT c.actor_principal FROM fact_graph_commits c JOIN entity_edges e ON e.commit_id=c.commit_id WHERE e.id=$1`, work.AssertionID).Scan(&principal); err != nil || principal != user.Principal {
		t.Fatal(principal, err)
	}
	var state string
	if err := tx.QueryRow(ctx, `SELECT lifecycle_state FROM entity_edges WHERE id=$1`, born.AssertionID).Scan(&state); err != nil || state != "invalidated" {
		t.Fatal(state, err)
	}
	sql(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	var id int64
	if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value) VALUES('L2','fact','explicit-private','Private City','project','explicit-private') RETURNING id`).Scan(&id); err != nil {
		t.Fatal(err)
	}
	hidden := seed("lives_in", "Private City", "memory:"+strconv.FormatInt(id, 10))
	if r := call("lives_in", "Private City", "user", verified); r["retracted"] != float64(0) {
		t.Fatal("private fact retracted", r)
	}
	if err := tx.QueryRow(ctx, `SELECT lifecycle_state FROM entity_edges WHERE id=$1`, hidden.AssertionID).Scan(&state); err != nil || state != "persistent" {
		t.Fatal(state, err)
	}
	sql(`SELECT set_config('aimee.memory_scope_all','1',true)`)
}

func TestPublicRetractionValidation(t *testing.T) {
	handler := NewHandler(nil, WithDataStore(PlacementKB, nil))
	client := clientForHandler(t, handler)
	for _, args := range []string{`{}`, `{"source":"x","relation":""}`, `{"source":"x","relation":"age","target":2}`, `{"source":"x","relation":"age","authority":true}`} {
		if r := runPublicCommand(t, client, "retract", args); r["kind"] != "invalid_argument" {
			t.Fatal(r)
		}
	}
	if r := runPublicCommand(t, client, "retract", `{"source":"x","relation":"age"}`); r["kind"] != "unavailable" {
		t.Fatal(r)
	}
	verified := bus.CommandContext{Authenticated: true, Principal: "forged", UserAuthority: true}
	if r, status := invokeContextCommand(t, handler, 200, verified, "retract", `{"source":"x","relation":"age","authority":"user"}`); status != bus.ModuleStatusInvalidRequest {
		t.Fatal("peer forged context", r, status)
	}
}
