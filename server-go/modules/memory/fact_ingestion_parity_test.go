package memory

import (
	"context"
	"fmt"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestFactMaintenanceHostBoundary(t *testing.T) {
	for _, body := range []string{`{"operation":"fact-maintenance","action":"promote","value":0}`, `{"operation":"fact-maintenance","action":"erase","value":30}`} {
		frame, _ := bus.EncodeCommand("runtime", []byte(body))
		if _, status := NewHandler(nil, WithDataStore(PlacementKB, nil))(bus.ModuleInvocation{StageID: StageCommand}, frame); status != bus.ModuleStatusInvalidRequest {
			t.Fatal(status)
		}
	}
	frame, _ := bus.EncodeCommand("runtime", []byte(`{"operation":"fact-maintenance","action":"expire","value":30}`))
	for _, placement := range []Placement{PlacementKB, PlacementServer} {
		if _, status := NewHandler(nil, WithDataStore(placement, nil))(bus.ModuleInvocation{StageID: StageCommand, PrincipalRef: 73}, frame); status != bus.ModuleStatusInvalidRequest {
			t.Fatal(status)
		}
	}
	if _, status := NewHandler(nil, WithDataStore(PlacementServer, nil))(bus.ModuleInvocation{StageID: StageCommand}, frame); status != bus.ModuleStatusCapabilityAbsent {
		t.Fatal(status)
	}
}

// Replaces the C gate/commit and confidence fixtures with the actual Go owner,
// including its PostgreSQL permissions, canonical aliases and evidence counts.
func exerciseFactIngestionParity(t *testing.T, ctx context.Context, tx pgx.Tx, s *postgresDataStore) {
	t.Helper()
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SAVEPOINT ingestion_parity`)
	defer func() { exec(`ROLLBACK TO SAVEPOINT ingestion_parity; RELEASE SAVEPOINT ingestion_parity`) }()
	model := modelFactActor()
	user := FactActor{Principal: "test:ingestion-user", Role: "user", Rank: 30, Authenticated: 1, TransportIdentity: "fixture"}
	seq := 0
	seed := func(subject, relation, object string, head, tail NodeKind, actor FactActor) (factMutationResult, FactVerdict) {
		t.Helper()
		seq++
		r, v, err := s.commitFactCandidate(ctx, FactCandidate{Subject: subject, Relation: relation, Object: object, SubjectKind: head, ObjectKind: tail, Actor: actor, Evidence: FactEvidence{SourceKind: "observation", SourceID: fmt.Sprintf("parity:%d", seq), Stance: "supports"}})
		if err != nil {
			t.Fatal(err)
		}
		return r, v
	}
	state := func(id int64) (string, string, float64, int) {
		t.Helper()
		var lifecycle, class string
		var confidence float64
		var rank int
		if err := tx.QueryRow(ctx, `SELECT lifecycle_state,confidence_class,confidence,authority_rank FROM entity_edges WHERE id=$1`, id).Scan(&lifecycle, &class, &confidence, &rank); err != nil {
			t.Fatal(err)
		}
		return lifecycle, class, confidence, rank
	}
	b, verdict := seed("Parity known", "Works For", "Parity Org", NodePerson, NodeOrg, model)
	if verdict != FactAccept {
		t.Fatal(verdict)
	}
	if life, class, conf, rank := state(b.AssertionID); life != "candidate" || class != "B" || conf != .6 || rank != 10 {
		t.Fatal(life, class, conf, rank)
	}
	promoted, _ := seed("Parity known", "works_for", "Parity Org", NodePerson, NodeOrg, user)
	if promoted.AssertionID != b.AssertionID {
		t.Fatal("split identity")
	}
	seed("Parity known", "works_for", "Parity Org", NodePerson, NodeOrg, model)
	if life, class, conf, rank := state(b.AssertionID); life != "persistent" || class != "A" || conf != 1 || rank != 30 {
		t.Fatal(life, class, conf, rank)
	}
	for _, actor := range []FactActor{model, user} {
		r, v := seed("Parity novel "+actor.Role, "parity_novel", "literal", NodePerson, NodeOther, actor)
		if v != FactNovel {
			t.Fatal(v)
		}
		if _, class, conf, _ := state(r.AssertionID); class != "C" || conf != .4 {
			t.Fatal(class, conf)
		}
	}
	var relStatus, sensitivity string
	if err := tx.QueryRow(ctx, `SELECT status,sensitivity FROM rel_types WHERE rel_type='parity_novel'`).Scan(&relStatus, &sensitivity); err != nil || relStatus != "provisional" || sensitivity != "pii" {
		t.Fatal(relStatus, sensitivity, err)
	}
	exec(`RESET ROLE; UPDATE rel_types SET status='active' WHERE rel_type='parity_novel'; SET LOCAL ROLE aimee_store_runtime`)
	known, v := seed("Parity live ontology", "parity_novel", "literal", NodePerson, NodeOther, model)
	if v != FactAccept {
		t.Fatal(v)
	}
	if _, class, conf, _ := state(known.AssertionID); class != "B" || conf != .6 {
		t.Fatal(class, conf)
	}
	// Reject a kind mismatch before creating endpoints or a graph commit.
	if _, v, err := s.commitFactCandidate(ctx, FactCandidate{Subject: "Parity forbidden", Relation: "works_for", Object: "Org", SubjectKind: NodeDevice, ObjectKind: NodeOrg, Actor: model}); err == nil || v != FactRejectKind {
		t.Fatal(v, err)
	}
	var rejected int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM entity_aliases WHERE name='Parity forbidden'`).Scan(&rejected); err != nil || rejected != 0 {
		t.Fatal(rejected, err)
	}
	// Two aliases converge on one canonical source; literal objects stay verbatim.
	canonical, err := s.canonicalFactEndpoint(ctx, "Parity Wilhelmina", NodePerson)
	if err != nil {
		t.Fatal(err)
	}
	exec(`INSERT INTO entity_aliases(name,name_norm,canonical_id,is_preferred) SELECT 'Parity Billie','parity billie',canonical_id,0 FROM entity_aliases WHERE name=$1`, canonical)
	exec(`INSERT INTO entity_aliases(name,name_norm,canonical_id,is_preferred) SELECT 'Parity Will','parity will',canonical_id,0 FROM entity_aliases WHERE name=$1`, canonical)
	a, _ := seed("Parity Billie", "works_for", "Parity Alias Org", NodePerson, NodeOrg, model)
	again, _ := seed("Parity Will", "works_for", "Parity Alias Org", NodePerson, NodeOrg, model)
	if a.AssertionID != again.AssertionID {
		t.Fatal(a, again)
	}
	literal, _ := seed("Parity Will", "has_role", "Literal Role", NodePerson, NodeScalar, model)
	var source, target string
	if err := tx.QueryRow(ctx, `SELECT source,target FROM entity_edges WHERE id=$1`, literal.AssertionID).Scan(&source, &target); err != nil || source != canonical || target != "Literal Role" {
		t.Fatal(source, target, err)
	}
	// Corroboration threshold uses distinct active supporting evidence.
	durable, _ := seed("Parity durable", "works_for", "Org", NodePerson, NodeOrg, model)
	seed("Parity durable", "works_for", "Org", NodePerson, NodeOrg, model)
	seed("Parity durable", "works_for", "Org", NodePerson, NodeOrg, model)
	incumbent, _ := seed("Parity protected", "works_for", "Human Org", NodePerson, NodeOrg, user)
	blocked, _ := seed("Parity protected", "works_for", "Model Org", NodePerson, NodeOrg, model)
	seed("Parity protected", "works_for", "Model Org", NodePerson, NodeOrg, model)
	seed("Parity protected", "works_for", "Model Org", NodePerson, NodeOrg, model)
	// More than a full batch of quarantined conflicts must not prevent later
	// unrelated candidates from reaching the maintenance threshold.
	for i := 0; i < 65; i++ {
		subject := fmt.Sprintf("Parity protected %d", i)
		seed(subject, "works_for", "Human Org", NodePerson, NodeOrg, user)
		for j := 0; j < 3; j++ {
			seed(subject, "works_for", "Blocked Org", NodePerson, NodeOrg, model)
		}
	}
	// Make the entire conflicting queue a legacy, unkeyed spelling variant.
	// Filtering these requires Go matching and must continue onto the next page.
	queueCommit, err := s.openFactCommit(ctx, user, "fixture.legacy-queue", "")
	if err != nil {
		t.Fatal(err)
	}
	exec(`WITH changed AS (UPDATE entity_edges SET source=upper(source),identity_key='',identity_subject_key='',commit_id=$1 WHERE source LIKE 'Parity protected %' AND authority_rank=30 RETURNING id,lifecycle_state,confidence,authority_rank,version)
 INSERT INTO fact_graph_changes(commit_id,assertion_id,action,existed_before,existed_after,before_lifecycle,after_lifecycle,before_confidence,after_confidence,before_authority_rank,after_authority_rank,before_version,after_version)
 SELECT $1,id,'fixture',1,1,lifecycle_state,lifecycle_state,confidence,confidence,authority_rank,authority_rank,version,version FROM changed`, queueCommit)
	if err := s.closeFactCommit(ctx, queueCommit, 0); err != nil {
		t.Fatal(err)
	}
	late, _ := seed("Parity late eligible", "works_for", "Org", NodePerson, NodeOrg, model)
	seed("Parity late eligible", "works_for", "Org", NodePerson, NodeOrg, model)
	seed("Parity late eligible", "works_for", "Org", NodePerson, NodeOrg, model)
	legacy, _ := seed("Parity legacy", "works_for", "Human Org", NodePerson, NodeOrg, user)
	legacyBlocked, _ := seed("Parity legacy", "works_for", "Model Org", NodePerson, NodeOrg, model)
	seed("Parity legacy", "works_for", "Model Org", NodePerson, NodeOrg, model)
	seed("Parity legacy", "works_for", "Model Org", NodePerson, NodeOrg, model)
	beforeLegacy, err := scanFactState(s.db.QueryRow(ctx, `SELECT `+factStateColumns+` FROM entity_edges WHERE id=$1`, legacy.AssertionID))
	if err != nil {
		t.Fatal(err)
	}
	legacyCommit, err := s.openFactCommit(ctx, user, "fixture.legacy", "")
	if err != nil {
		t.Fatal(err)
	}
	exec(`UPDATE entity_edges SET source='Ｐａｒｉｔｙ ｌｅｇａｃｙ',identity_key='',identity_subject_key='',commit_id=$2 WHERE id=$1`, legacy.AssertionID, legacyCommit)
	if err := s.recordFactChange(ctx, legacyCommit, "fixture", "unkeyed legacy", beforeLegacy, beforeLegacy); err != nil {
		t.Fatal(err)
	}
	if err := s.closeFactCommit(ctx, legacyCommit, legacy.AssertionID); err != nil {
		t.Fatal(err)
	}
	tombstoned, _ := seed("Parity tombstoned", "works_for", "Org", NodePerson, NodeOrg, model)
	seed("Parity tombstoned", "works_for", "Org", NodePerson, NodeOrg, model)
	seed("Parity tombstoned", "works_for", "Org", NodePerson, NodeOrg, model)
	exec(`INSERT INTO memory_rejection_tombstones(object_kind,source,relation,target,authority_rank,reason,rejected_by) VALUES('fact','Parity tombstoned','works_for','Org',40,'explicit rejection','test:operator')`)
	backend := *s
	backend.db = runtimeRoleDB{evalQueryer{tx}, t}
	handler := NewHandler(nil, WithDataStore(PlacementKB, &backend))
	maintain := func(action string, value int, want bus.ModuleStatus) {
		t.Helper()
		frame, _ := bus.EncodeCommand("runtime", []byte(fmt.Sprintf(`{"operation":"fact-maintenance","action":%q,"value":%d,"actor":{"rank":40}}`, action, value)))
		_, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame)
		if status != want {
			t.Fatal(action, status, want)
		}
	}
	// A late audit failure rolls back every candidate transition.
	exec(`RESET ROLE; CREATE FUNCTION pg_temp.parity_fail_seal() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN IF NEW.operation='fact.maintenance.promote' AND NEW.status='applied' THEN RAISE EXCEPTION 'maintenance seal failure'; END IF; RETURN NEW; END $$;
 CREATE TRIGGER parity_fail_seal BEFORE UPDATE ON fact_graph_commits FOR EACH ROW EXECUTE FUNCTION pg_temp.parity_fail_seal(); SET LOCAL ROLE aimee_store_runtime`)
	maintain("promote", 3, bus.ModuleStatusInternal)
	if life, _, conf, _ := state(durable.AssertionID); life != "candidate" || conf != .6 {
		t.Fatal(life, conf)
	}
	exec(`RESET ROLE; DROP TRIGGER parity_fail_seal ON fact_graph_commits; SET LOCAL ROLE aimee_store_runtime`)
	maintain("promote", 3, bus.ModuleStatusOK)
	if life, class, conf, rank := state(durable.AssertionID); life != "persistent" || class != "B" || conf != .8 || rank != 10 {
		t.Fatal(life, class, conf, rank)
	}
	if life, _, _, _ := state(late.AssertionID); life != "persistent" {
		t.Fatal("blocked candidates starved eligible batch", life)
	}
	if life, _, _, _ := state(blocked.AssertionID); life != "candidate" {
		t.Fatal("maintenance bypassed human authority", life)
	}
	if life, _, _, _ := state(legacyBlocked.AssertionID); life != "candidate" {
		t.Fatal("maintenance bypassed unkeyed Unicode incumbent", life)
	}
	if life, _, _, _ := state(tombstoned.AssertionID); life != "candidate" {
		t.Fatal("maintenance bypassed explicit rejection", life)
	}
	if life, _, _, _ := state(incumbent.AssertionID); life != "persistent" {
		t.Fatal(life)
	}
	// Expiry includes ISO-T timestamps, preserves confirmed C and never expires
	// persistent/user assertions. Fixtures backdate through a tracked commit.
	stale, _ := seed("Parity stale", "parity_speculation", "value", NodePerson, NodeOther, model)
	confirmed, _ := seed("Parity confirmed", "parity_speculation", "value", NodePerson, NodeOther, model)
	seed("Parity confirmed", "parity_speculation", "value", NodePerson, NodeOther, model)
	for _, r := range []factMutationResult{stale, confirmed, b} {
		before, err := scanFactState(s.db.QueryRow(ctx, `SELECT `+factStateColumns+` FROM entity_edges WHERE id=$1`, r.AssertionID))
		if err != nil {
			t.Fatal(err)
		}
		cid, err := s.openFactCommit(ctx, user, "fixture.backdate", "")
		if err != nil {
			t.Fatal(err)
		}
		exec(`UPDATE entity_edges SET asserted_at='2000-01-01T00:00:00Z',commit_id=$2 WHERE id=$1`, r.AssertionID, cid)
		if err := s.recordFactChange(ctx, cid, "fixture", "backdate", before, before); err != nil {
			t.Fatal(err)
		}
		if err := s.closeFactCommit(ctx, cid, r.AssertionID); err != nil {
			t.Fatal(err)
		}
	}
	maintain("expire", 30, bus.ModuleStatusOK)
	if life, _, _, _ := state(stale.AssertionID); life != "invalidated" {
		t.Fatal(life)
	}
	if life, _, _, _ := state(confirmed.AssertionID); life != "candidate" {
		t.Fatal(life)
	}
	if life, _, _, _ := state(b.AssertionID); life != "persistent" {
		t.Fatal(life)
	}
}
