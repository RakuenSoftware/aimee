package memory

import (
	"context"
	"encoding/json"
	"errors"
	"strconv"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func exerciseFactContextReplay(t *testing.T, ctx context.Context, tx pgx.Tx, s *postgresDataStore) {
	t.Helper()
	sql := func(q string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, q, args...); err != nil {
			t.Fatal(err)
		}
	}
	sql(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	user := FactActor{Principal: "test:context-user", TransportIdentity: "verified:context", Role: "user", Rank: 30, Authenticated: 1}
	seed := func(subject, relation, object, source string, actor FactActor) factMutationResult {
		t.Helper()
		tail := NodeOrg
		if relation == "born_in" {
			tail = NodePlace
		}
		if relation == "knows" {
			tail = NodePerson
		}
		kind := "observation"
		if strings.HasPrefix(source, "memory:") {
			kind = "memory"
		}
		r, _, err := s.commitFactCandidate(ctx, FactCandidate{Subject: subject, Relation: relation, Object: object, SubjectKind: NodePerson, ObjectKind: tail, Actor: actor, AssertionKind: "world_fact", Evidence: FactEvidence{SourceKind: kind, SourceID: source}})
		if err != nil {
			t.Fatal(err)
		}
		return r
	}
	high := seed("user", "works_for", "Context User Corp", "context-high", user)
	low := seed("user", "works_for", "Context Model Corp", "context-low", modelFactActor())
	born := seed("user", "born_in", "Context City", "context-born", modelFactActor())
	handler := NewHandler(nil, WithDataStore(PlacementKB, &postgresDataStore{db: runtimeRoleDB{evalQueryer{tx}, t}, placement: PlacementKB}))
	verified := bus.CommandContext{Authenticated: true, UserAuthority: true, Principal: user.Principal, TransportIdentity: user.TransportIdentity}
	call := func(verb, query string) map[string]any {
		t.Helper()
		raw, _ := json.Marshal(map[string]any{"query": query, "authority": "user", "actor": "operator", "scope_context": true})
		r, status := invokeContextCommand(t, handler, 0, verified, verb, string(raw))
		if status != bus.ModuleStatusOK {
			t.Fatal(r, status)
		}
		return r
	}
	r := call("context_block", "please forget my works_for")
	if r["status"] != "ok" || r["retraction"] != "invalidated" {
		t.Fatal(r)
	}
	state := func(id int64) string {
		t.Helper()
		var v string
		if err := tx.QueryRow(ctx, `SELECT lifecycle_state FROM entity_edges WHERE id=$1`, id).Scan(&v); err != nil {
			t.Fatal(err)
		}
		return v
	}
	if state(high.AssertionID) != "persistent" || state(low.AssertionID) != "invalidated" {
		t.Fatal("authenticated model query acquired user authority")
	}
	if r := call("context_block", "please forget my born_in"); r["retraction"] != "operator_required" || state(born.AssertionID) != "candidate" {
		t.Fatal(r)
	}
	if r := call("facts", ""); r["facts"] != "" {
		t.Fatal("empty query recalled facts", r)
	}
	if r := call("facts", "Where do I work?"); r["status"] != "ok" || !strings.Contains(r["facts"].(string), "Context User Corp") {
		t.Fatal(r)
	}
	// The same evidence visibility fence governs recall and query retraction.
	sql(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	var mid int64
	if err := tx.QueryRow(ctx, `INSERT INTO memories(key,content,kind,tier,scope_type,scope_value) VALUES('go-context-private','Private Context Corp','observation','L2','project','private-context') RETURNING id`).Scan(&mid); err != nil {
		t.Fatal(err)
	}
	hidden := seed("user", "works_for", "Private Context Corp", "memory:"+strconv.FormatInt(mid, 10), modelFactActor())
	// Give the hidden candidate a persistent fixture state through a real user assertion.
	seed("user", "works_for", "Private Context Corp", "context-hidden-user", user)
	if r := call("facts", "Where do I work?"); strings.Contains(r["facts"].(string), "Private Context Corp") {
		t.Fatal("private fact leaked", r)
	}
	if r := call("context_block", "please forget my works_for"); r["status"] != "ok" {
		t.Fatal(r)
	}
	if n, err := s.invalidateFacts(ctx, user, "user", "works_for", "Private Context Corp"); err != nil || n != 0 {
		t.Fatal("private fact writable", n, err)
	}
	if state(hidden.AssertionID) != "persistent" {
		t.Fatal("hidden assertion invalidated")
	}
	sql(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	// Actual SQL/WORM errors must roll back the entire public context request.
	rollback := seed("user", "works_for", "Rollback Context Corp", "context-rollback", modelFactActor())
	sql(`RESET ROLE`)
	sql(`REVOKE EXECUTE ON FUNCTION kb_fact_commit_worm_seal(TEXT,TEXT) FROM aimee_store_runtime`)
	sql(`SET LOCAL ROLE aimee_store_runtime`)
	if r := call("context_block", "please forget my works_for"); r["kind"] != "unavailable" {
		t.Fatal(r)
	}
	if state(rollback.AssertionID) != "candidate" {
		t.Fatal("unsealed retraction survived")
	}
	sql(`RESET ROLE`)
	sql(`GRANT EXECUTE ON FUNCTION kb_fact_commit_worm_seal(TEXT,TEXT) TO aimee_store_runtime`)
	sql(`SET LOCAL ROLE aimee_store_runtime`)
	sql(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	// Historical facts are annotate-only, and bounded mutation must never
	// silently truncate a larger matching set.
	history, _, err := s.commitFactCandidate(ctx, FactCandidate{Subject: "context history", Relation: "knows", Object: "Alice", SubjectKind: NodePerson, ObjectKind: NodePerson, Actor: user, AssertionKind: "episode", Evidence: FactEvidence{SourceKind: "observation", SourceID: "context-history"}})
	if err != nil {
		t.Fatal(err)
	}
	if n, err := s.invalidateFacts(ctx, user, "context history", "knows", ""); n != 0 || !errors.Is(err, errFactAnnotateOnly) {
		t.Fatal(n, err)
	}
	if state(history.AssertionID) != "persistent" {
		t.Fatal("historical evidence erased")
	}
	for i := 0; i < 65; i++ {
		seed("context bound", "knows", "Bound Person "+strconv.Itoa(i), "context-bound-"+strconv.Itoa(i), modelFactActor())
	}
	if n, err := s.invalidateFacts(ctx, user, "context bound", "knows", ""); n != 0 || err == nil {
		t.Fatal("partial retraction accepted", n, err)
	}
	var active int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM entity_edges WHERE source='context bound' AND invalidated_at=''`).Scan(&active); err != nil || active != 65 {
		t.Fatal(active, err)
	}

}
