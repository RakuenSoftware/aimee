package memory

import (
	"context"
	"errors"
	"os"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestFactIdentity(t *testing.T) {
	for _, pair := range [][2]string{{" Ａｌｉｃｅ\u00a0 Smith ", "alice smith"}, {"Straße", "STRASSE"}, {"e\u0301", "é"}, {"Ａ\x1fB", "a b"}, {"한", "한"}} {
		if a, b := factIdentityComponent(pair[0]), factIdentityComponent(pair[1]); a == "" || a != b {
			t.Fatal(pair, a, b)
		}
	}
	for _, value := range []string{"\xff", strings.Repeat("a", 1024), "", "\x00"} {
		if got := factIdentityComponent(value); got != "" {
			t.Fatal("invalid identity accepted", got)
		}
	}
	a, subject := factIdentity("Alice", "worksFor", "Acme")
	b, _ := factIdentity(" ALICE ", "works_for", "ACME")
	if a != b || subject != "alice\x1fworks_for" {
		t.Fatal(a, b, subject)
	}
	if a, _ := factIdentity("Alice", "works_for", strings.Repeat("x", 1024)); a != "" {
		t.Fatal("identity truncated")
	}
}

func TestFactMutationRuntimeReplay(t *testing.T) {
	url := os.Getenv("AIMEE_DB2_REPLAY_URL")
	if url == "" {
		t.Skip("set AIMEE_DB2_REPLAY_URL")
	}
	ctx := context.Background()
	conn, err := pgx.Connect(ctx, url)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close(ctx)
	tx, err := conn.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	sql := func(q string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, q, args...); err != nil {
			t.Fatal(err)
		}
	}
	sql(`DO $$ BEGIN IF NOT EXISTS(SELECT 1 FROM pg_roles WHERE rolname='aimee_store_runtime') THEN CREATE ROLE aimee_store_runtime NOINHERIT NOBYPASSRLS; END IF; END $$; GRANT USAGE ON SCHEMA public TO aimee_store_runtime`)
	schema, err := os.ReadFile("../../../src/modules/db2/c/schema.sql")
	if err != nil {
		t.Fatal(err)
	}
	start, end := strings.Index(string(schema), "DO $memory_store_grants$"), strings.Index(string(schema), "END\n$memory_store_grants$;")
	if start < 0 || end < start {
		t.Fatal("missing packaged grants")
	}
	sql(string(schema[start : end+len("END\n$memory_store_grants$;")]))
	sql(`INSERT INTO rel_types(rel_type,status) VALUES('works_for','active'),('born_in','active'),('knows','active') ON CONFLICT(rel_type) DO NOTHING`)
	sql(`SET LOCAL ROLE aimee_store_runtime`)
	sql(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	s := &postgresDataStore{db: runtimeRoleTx{evalQueryer{tx}, t}, placement: PlacementKB}
	model := modelFactActor()
	user := FactActor{Principal: "test:fact-user", Role: "user", Rank: 30, Authenticated: 1, TransportIdentity: "test:transport"}
	candidate := func(subject, relation, object, source string, actor FactActor) FactCandidate {
		tail := NodeOrg
		if relation == "born_in" {
			tail = NodePlace
		}
		if relation == "knows" {
			tail = NodePerson
		}
		return FactCandidate{Subject: subject, Relation: relation, Object: object, SubjectKind: NodePerson, ObjectKind: tail, Actor: actor, AssertionKind: "world_fact", Evidence: FactEvidence{SourceID: source, SourceKind: "observation", EvidenceHash: source, Stance: "supports"}}
	}
	commit := func(c FactCandidate) factMutationResult {
		t.Helper()
		r, _, err := s.commitFactCandidate(ctx, c)
		if err != nil {
			t.Fatal(err)
		}
		if r.AssertionID <= 0 {
			t.Fatal(r)
		}
		return r
	}
	firstInput := candidate("GoFact Alice", "works_for", "First Corp", "go-fact-1", model)
	first := commit(firstInput)
	if first.Lifecycle != "candidate" || !first.Changed || !first.EvidenceAdded || first.CommitID == "" {
		t.Fatal(first)
	}
	replay := commit(firstInput)
	if replay.AssertionID != first.AssertionID || replay.CommitID != "" || replay.Changed || replay.EvidenceAdded {
		t.Fatal("replay mutated state", replay)
	}
	promotedInput := firstInput
	promotedInput.Actor = user
	promotedInput.Evidence.SourceID = "go-fact-2"
	promoted := commit(promotedInput)
	if promoted.AssertionID != first.AssertionID || promoted.Lifecycle != "persistent" {
		t.Fatal(promoted)
	}
	lower := commit(candidate("GoFact Alice", "works_for", "Second Corp", "go-fact-3", model))
	if !lower.Quarantined || lower.Lifecycle != "candidate" {
		t.Fatal("lower authority displaced incumbent", lower)
	}
	var lifecycle string
	if err := tx.QueryRow(ctx, `SELECT lifecycle_state FROM entity_edges WHERE id=$1`, first.AssertionID).Scan(&lifecycle); err != nil || lifecycle != "persistent" {
		t.Fatal(lifecycle, err)
	}
	correction := commit(candidate("GoFact Alice", "works_for", "Second Corp", "go-fact-4", user))
	if correction.AssertionID != lower.AssertionID || correction.Lifecycle != "persistent" || correction.Quarantined {
		t.Fatal(correction)
	}
	if err := tx.QueryRow(ctx, `SELECT lifecycle_state FROM entity_edges WHERE id=$1`, first.AssertionID).Scan(&lifecycle); err != nil || lifecycle != "superseded" {
		t.Fatal(lifecycle, err)
	}
	// Exact delivery replay after correction must never reactivate history.
	if r := commit(promotedInput); r.Lifecycle != "superseded" || r.CommitID != "" {
		t.Fatal("replayed evidence restored history", r)
	}
	born := commit(candidate("GoFact Bob", "born_in", "London", "go-fact-5", user))
	immutable := commit(candidate("GoFact Bob", "born_in", "Paris", "go-fact-6", user))
	if !immutable.Quarantined {
		t.Fatal("immutable correction admitted", born, immutable)
	}
	op := user
	op.Role = "operator"
	op.Rank = 40
	operator := commit(candidate("GoFact Bob", "born_in", "Paris", "go-fact-7", op))
	if operator.Quarantined || operator.Lifecycle != "persistent" {
		t.Fatal(operator)
	}
	unicodeA := commit(candidate("GoFact Straße", "knows", "Élodie", "go-fact-8", user))
	unicodeB := commit(candidate("GoFact STRASSE", "knows", "E\u0301lodie", "go-fact-9", model))
	if unicodeA.AssertionID != unicodeB.AssertionID {
		t.Fatal("normalized identity split", unicodeA, unicodeB)
	}
	sql(`INSERT INTO memory_rejection_tombstones(object_kind,source,relation,target,authority_rank,reason,rejected_by) VALUES('fact','GoFact Rejected','works_for','Blocked Corp',40,'explicit rejection','test:operator')`)
	if _, _, err := s.commitFactCandidate(ctx, candidate("GoFact Rejected", "works_for", "Blocked Corp", "go-fact-10", model)); !errors.Is(err, errFactTombstoned) {
		t.Fatal("tombstone bypass", err)
	}
	if _, _, err := s.commitFactCandidate(ctx, candidate("ＧｏＦａｃｔ Ｒｅｊｅｃｔｅｄ", "works_for", "Ｂｌｏｃｋｅｄ Ｃｏｒｐ", "go-fact-11", model)); !errors.Is(err, errFactTombstoned) {
		t.Fatal("Unicode tombstone bypass", err)
	}
	legacy := commit(candidate("GoFact Legacy", "works_for", "Legacy Corp", "go-fact-legacy", user))
	unrelated := commit(candidate("GoFact Unrelated", "knows", "Unrelated Person", "go-fact-unrelated", user))
	sql(`UPDATE entity_edges SET identity_key='',identity_subject_key='' WHERE id IN ($1,$2)`, legacy.AssertionID, unrelated.AssertionID)
	corrected := commit(candidate("ＧｏＦａｃｔ Ｌｅｇａｃｙ", "works_for", "New Legacy Corp", "go-fact-legacy-new", user))
	if corrected.Quarantined {
		t.Fatal(corrected)
	}
	if err := tx.QueryRow(ctx, `SELECT lifecycle_state FROM entity_edges WHERE id=$1`, legacy.AssertionID).Scan(&lifecycle); err != nil || lifecycle != "superseded" {
		t.Fatal("unkeyed normalized incumbent survived correction", lifecycle, err)
	}
	var unchanged string
	if err := tx.QueryRow(ctx, `SELECT identity_key FROM entity_edges WHERE id=$1`, unrelated.AssertionID).Scan(&unchanged); err != nil || unchanged != "" {
		t.Fatal("unrelated history rewritten", unchanged, err)
	}
	exerciseFactWorkerReplay(t, ctx, tx, s)
	// Deferred semantic evidence guards run before the test rolls back its data.
	sql(`SET CONSTRAINTS ALL IMMEDIATE`)
	var missing int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM fact_graph_commits c WHERE c.actor_principal IN ('test:fact-user','system:model-inference') AND c.operation='fact.assert' AND c.status='open'`).Scan(&missing); err != nil || missing != 0 {
		t.Fatal("open commits", missing, err)
	}
}

func exerciseFactWorkerReplay(t *testing.T, ctx context.Context, tx pgx.Tx, s *postgresDataStore) {
	t.Helper()
	sql := func(q string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, q, args...); err != nil {
			t.Fatal(err)
		}
	}
	seed := func(scope, key, content string) int64 {
		t.Helper()
		var id int64
		err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value) VALUES('L0','fact',$1,$2,$3,$4) RETURNING id`, key, content, scope, map[string]string{"global": "_global", "project": "fact-private"}[scope]).Scan(&id)
		if err != nil {
			t.Fatal(err)
		}
		if err = s.captureStoredFactActor(ctx, id, AuthorityModel, nil); err != nil {
			t.Fatal(err)
		}
		return id
	}
	hidden := seed("project", "fact-worker-hidden", "I work at Hidden Corp.")
	id := seed("global", "fact-worker-source", "I work at Old Corp.")
	sql(`SELECT set_config('aimee.memory_scope_all','0',true),set_config('aimee.memory_scope_type','global',true),set_config('aimee.memory_scope_value','_global',true)`)
	claim := func() *MemoryFactWork {
		t.Helper()
		work, err := s.ClaimMemoryFact(ctx)
		if err != nil || work == nil {
			t.Fatal(work, err)
		}
		return work
	}
	old := claim()
	if old.MemoryID != id || old.Generation != 1 || old.SourceHash == "" || old.LeaseToken == "" {
		t.Fatal("scope or lease", old, hidden)
	}
	sql(`UPDATE memories SET content='I work at Changed Corp.' WHERE id=$1`, id)
	raw := `{"facts":[{"subject":"user","relation":"works_for","object":"Old Corp","source_start":0,"source_end":19}]}`
	if updated, err := s.CompleteMemoryFact(ctx, *old, raw, true, ""); err != nil || updated {
		t.Fatal("stale source accepted", updated, err)
	}
	current := claim()
	if current.Generation != 2 || current.LeaseToken == old.LeaseToken {
		t.Fatal(current)
	}
	if updated, err := s.CompleteMemoryFact(ctx, *old, raw, false, "stale failure"); err != nil || updated {
		t.Fatal("stale failure changed new lease", updated, err)
	}
	raw = `{"facts":[{"subject":"user","relation":"works_for","object":"Changed Corp","source_start":0,"source_end":23}]}`
	if updated, err := s.CompleteMemoryFact(ctx, *current, raw, true, ""); err != nil || !updated {
		t.Fatal(updated, err)
	}
	var count int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM fact_evidence f JOIN entity_edges e ON e.id=f.assertion_id WHERE f.source_id='memory:'||$1::bigint::text AND e.target='Old Corp'`, id).Scan(&count); err != nil || count != 0 {
		t.Fatal("stale provider wrote facts", count, err)
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM fact_evidence f JOIN entity_edges e ON e.id=f.assertion_id WHERE f.source_id='memory:'||$1::bigint::text AND e.target='Changed Corp'`, id).Scan(&count); err != nil || count < 1 {
		t.Fatal("completed extraction missing", count, err)
	}
	if updated, err := s.CompleteMemoryFact(ctx, *current, raw, true, ""); err != nil || updated {
		t.Fatal("completed lease replayed", updated, err)
	}
	// Explicit edits requeue even a completed job and invalidate the old lease.
	if err := s.captureStoredFactActor(ctx, id, AuthorityModel, nil); err != nil {
		t.Fatal(err)
	}
	retry := claim()
	if retry.Generation != 3 {
		t.Fatal(retry)
	}
	if updated, err := s.CompleteMemoryFact(ctx, *retry, "", false, "provider HTTP 503 SECRET_DETAIL"); err != nil || !updated {
		t.Fatal(updated, err)
	}
	var status, lastError string
	var attempts int
	if err := tx.QueryRow(ctx, `SELECT status,attempts,last_error FROM kb_async_jobs WHERE id=$1`, retry.JobID).Scan(&status, &attempts, &lastError); err != nil || status != "pending" || attempts != 0 || strings.Contains(lastError, "SECRET") {
		t.Fatal(status, attempts, lastError, err)
	}
	// Suppression while the model is running permits no derived writes.
	sql(`UPDATE kb_async_jobs SET next_attempt_at='' WHERE id=$1`, retry.JobID)
	suppressed := claim()
	sql(`UPDATE memories SET activation_suppressed=1 WHERE id=$1`, id)
	if updated, err := s.CompleteMemoryFact(ctx, *suppressed, raw, true, ""); err != nil || !updated {
		t.Fatal(updated, err)
	}

	// Completion goes through the production transaction wrapper. A denied WORM
	// seal must roll back facts, aliases, evidence and queue acknowledgement.
	sql(`UPDATE memories SET activation_suppressed=0,content='Rollback Corp' WHERE id=$1`, id)
	if err := s.captureStoredFactActor(ctx, id, AuthorityModel, nil); err != nil {
		t.Fatal(err)
	}
	blocked := claim()
	sql(`RESET ROLE`)
	sql(`REVOKE EXECUTE ON FUNCTION kb_fact_commit_worm_seal(TEXT,TEXT) FROM aimee_store_runtime`)
	sql(`SET LOCAL ROLE aimee_store_runtime`)
	handler := NewHandler(nil, WithDataStore(PlacementKB, &postgresDataStore{db: runtimeRoleDB{evalQueryer{tx}, t}, placement: PlacementKB}))
	request := DataRequest{Operation: "memory-facts-complete", FactWork: blocked, Success: true, Content: `{"facts":[{"subject":"user","relation":"works_for","object":"Rollback Corp","source_start":0,"source_end":13}]}`}
	if _, status := handler(bus.ModuleInvocation{StageID: StageData, PrincipalRef: 200}, dataRequest(t, request)); status != bus.ModuleStatusInvalidRequest {
		t.Fatal("peer supplied extraction completion", status)
	}
	if _, status := handler(bus.ModuleInvocation{StageID: StageData}, dataRequest(t, request)); status != bus.ModuleStatusInternal {
		t.Fatal("unsealed extraction accepted", status)
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM entity_edges WHERE target='Rollback Corp'`).Scan(&count); err != nil || count != 0 {
		t.Fatal("unsealed assertion survived", count, err)
	}
	if err := tx.QueryRow(ctx, `SELECT status FROM kb_async_jobs WHERE id=$1`, blocked.JobID).Scan(&status); err != nil || status != "running" {
		t.Fatal("failed commit acknowledged lease", status, err)
	}
	sql(`RESET ROLE`)
	sql(`GRANT EXECUTE ON FUNCTION kb_fact_commit_worm_seal(TEXT,TEXT) TO aimee_store_runtime`)
	sql(`SET LOCAL ROLE aimee_store_runtime`)
	sql(`SELECT set_config('aimee.memory_scope_all','1',true)`)
}
