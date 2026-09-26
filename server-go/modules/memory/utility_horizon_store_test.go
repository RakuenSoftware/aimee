package memory

import (
	"context"
	"encoding/json"
	"errors"
	"os"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/jackc/pgx/v5"
)

func installTestHorizon(t *testing.T, c horizonConfiguration) {
	t.Helper()
	raw, err := json.Marshal(c)
	if err != nil {
		t.Fatal(err)
	}
	t.Setenv("AIMEE_MEMORY_UTILITY_HORIZON_POLICY", string(raw))
}
func TestMemoryUtilityHorizonCanonicalPostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_DB2_REPLAY_URL")
	if dsn == "" {
		t.Skip("AIMEE_DB2_REPLAY_URL required")
	}
	ctx := context.Background()
	conn, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close(ctx)
	tx, err := conn.BeginTx(ctx, pgx.TxOptions{IsoLevel: pgx.RepeatableRead})
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	var id int64
	if err := tx.QueryRow(ctx, `INSERT INTO memories(key,content,tier,kind,scope_type,scope_value,activation_sticky_turns) VALUES('horizon-test','horizonneedle','L2','task_state','project','horizon-test',5) RETURNING id`).Scan(&id); err != nil {
		t.Fatal(err)
	}
	data, err := NewPostgresDataStore(evalQueryer{tx}, PlacementKB)
	if err != nil {
		t.Fatal(err)
	}
	s := data.(*postgresDataStore)
	r, err := s.getAtVersioned(ctx, Scope{Type: ScopeProject, Value: "horizon-test"}, id, false, "", true)
	if err != nil {
		t.Fatal(err)
	}
	_, policy, _ := horizonFixture()
	policy.Kinds["task_state"] = horizonRule{ID: "exact-boundary", Anchor: "created", DurationSeconds: 0}
	c := horizonConfiguration{Policy: policy, Mode: "shadow"}
	installTestHorizon(t, c)
	v, err := s.validity(ctx, id, &MemoryReadResult{Mode: "current"})
	if err != nil || !v.Eligible || v.UtilityHorizon == nil || !v.UtilityHorizon.Elapsed {
		t.Fatal(v, err)
	}
	c.Mode = "enforce"
	installTestHorizon(t, c)
	// A durable record must survive even when a stronger transient candidate is
	// excluded. All independent arms apply the predicate before their LIMIT.
	var durable int64
	if err := tx.QueryRow(ctx, `INSERT INTO memories(key,content,tier,kind,scope_type,scope_value) VALUES('horizon-durable','horizonneedle','L2','constraint','project','horizon-test') RETURNING id`).Scan(&durable); err != nil {
		t.Fatal(err)
	}
	hits, err := s.Search(ctx, r.Scope, "horizonneedle", "", "", 1)
	if err != nil || len(hits) != 1 || hits[0].ID != durable {
		t.Fatal("lexical prelimit", hits, err)
	}
	activated, _, _, err := s.recallActivated(ctx, &ActivationSnapshot{CurrentTurn: 2, Rows: []ActivationRow{{MemoryID: id, LastTurn: 1}}}, "m.content LIKE '%horizonneedle%'", 1, true, false)
	if err != nil || len(activated) != 1 || activated[0].ID != durable {
		t.Fatal("activation bypassed horizon", activated, err)
	}
	var dimension int
	if err := tx.QueryRow(ctx, `SELECT atttypmod FROM pg_attribute WHERE attrelid='memory_embeddings'::regclass AND attname='embedding'`).Scan(&dimension); err != nil {
		t.Fatal(err)
	}
	vector := make([]float64, dimension)
	vector[0] = 1
	encoded, _ := json.Marshal(vector)
	for _, target := range []int64{id, durable} {
		exec(`INSERT INTO memory_embeddings(point_id,embedding,record_type,primary_scope,project,kind,payload_json) SELECT id,$2::vector,'memory',scope_type,scope_value,kind,'{}' FROM memories WHERE id=$1`, target, string(encoded))
	}
	dense, err := s.SearchVectors(ctx, vector, "memory", "", "horizon-test", false, 1)
	if err != nil || len(dense) != 1 || dense[0].ID != durable {
		t.Fatal("dense horizon", dense, err)
	}
	exec(`INSERT INTO memory_links(source_id,target_id,relation) VALUES($1,$2,'depends_on')`, durable, id)
	graph, err := s.pageRank(ctx, DataRequest{Scope: r.Scope, Project: "horizon-test", PageRank: &pageRankRequest{IDs: []int64{id, durable}, Iterations: 8, Weight: 1, Relations: []string{"depends_on"}}}, true)
	if err != nil || graph.Candidates != 1 || graph.Edges != 0 {
		t.Fatal("graph horizon", graph, err)
	}
	bundle, err := s.RecallBundle(ctx, "horizonneedle", 2400, false)
	if err != nil || strings.Contains(string(bundle), `"key":"horizon-test"`) {
		t.Fatal("bundle leaked elapsed key", string(bundle), err)
	}

	_, err = s.Get(ctx, r.Scope, id)
	if !errors.Is(err, ErrMemoryNotFound) {
		t.Fatal("elapsed current read admitted", err)
	}
	v, err = s.validity(ctx, id, &MemoryReadResult{Mode: "current"})
	if err != nil || v.Eligible || v.UtilityHorizon.Reason != "utility_horizon_elapsed" {
		t.Fatal(v, err)
	}
	var clock time.Time
	if err := tx.QueryRow(ctx, `SELECT CURRENT_TIMESTAMP`).Scan(&clock); err != nil {
		t.Fatal(err)
	}
	old, err := s.getAtVersioned(ctx, r.Scope, id, true, clock.UTC().Format(time.RFC3339Nano), true)
	if err != nil || old.ID != id {
		t.Fatal("historical lost", old, err)
	}
	v, err = s.validity(ctx, id, &MemoryReadResult{Mode: "historical", ValidAt: clock.UTC().Format(time.RFC3339Nano)})
	if err != nil || !v.Eligible || v.UtilityHorizon.Reason != "utility_horizon_elapsed_inspection_only" {
		t.Fatal(v, err)
	}
	check := &sourceRevalidation{SchemaVersion: 1, CheckID: strings.Repeat("a", 32), Sources: []typedProjectionRef{{Channel: "native_active_context", ID: strconv.FormatInt(id, 10), Source: &typedSourceVersion{Kind: "memory_record", Version: *r.Version, MemoryParentState: "observed"}}}}
	if allowed, err := s.revalidateSources(ctx, check, r.Scope); err != nil || allowed {
		t.Fatal("release admitted elapsed source", allowed, err)
	}
	// Access metadata never creates a new anchor or changes the governed version.
	exec(`UPDATE memories SET use_count=use_count+50,last_used_at=pg_now_text(),updated_at=pg_now_text() WHERE id=$1`, id)
	d, err := s.utilityHorizonDecision(ctx, id, r.Version, "current", true)
	if err != nil || !d.Elapsed {
		t.Fatal(d, err)
	}
	// Collection observations expire at the next horizon even without a write.
	c.Policy.Kinds["task_state"] = horizonRule{ID: "future-boundary", Anchor: "created", DurationSeconds: 600}
	installTestHorizon(t, c)
	var deadline string
	if err := tx.QueryRow(ctx, "SELECT "+horizonCollectionDeadlineSQL(false, "''")).Scan(&deadline); err != nil {
		t.Fatal(err)
	}
	boundary, err := time.Parse(time.RFC3339Nano, deadline)
	if err != nil || !boundary.Equal(clock.Add(600*time.Second)) {
		t.Fatal("collection boundary", deadline, err)
	}
	c.Policy.Kinds["task_state"] = horizonRule{ID: "exact-boundary", Anchor: "created", DurationSeconds: 0}
	// Exact-version, operator-admitted override can extend the configured duration.
	c.Overrides = []horizonOverride{{Version: *r.Version, Rule: horizonRule{ID: "reviewed-extension", Anchor: "created", DurationSeconds: 3600}}}
	installTestHorizon(t, c)
	if _, err = s.Get(ctx, r.Scope, id); err != nil {
		t.Fatal("override refused", err)
	}
	// Safety wins even over that admitted override.
	c.Policy.Safety = map[string]horizonRule{"task_state": {ID: "safety", Anchor: "created", DurationSeconds: 0}}
	installTestHorizon(t, c)
	if _, err = s.Get(ctx, r.Scope, id); !errors.Is(err, ErrMemoryNotFound) {
		t.Fatal("safety lost", err)
	}
	c.Policy.Safety = nil
	// A policy-only change invalidates cached collection observations.
	collection, err := s.observeRecallCollection(ctx)
	if err != nil {
		t.Fatal(err)
	}
	collectionCheck := &sourceRevalidation{SchemaVersion: 1, CheckID: strings.Repeat("b", 32), Sources: []typedProjectionRef{{Channel: "native_memory_collection", ID: "1", Source: collection}}}
	if allowed, err := s.revalidateSources(ctx, collectionCheck, Scope{}); err != nil || !allowed {
		t.Fatal("collection validation", allowed, err)
	}
	c.Policy.Revision = "new-policy-same-records"
	installTestHorizon(t, c)
	if allowed, err := s.revalidateSources(ctx, collectionCheck, Scope{}); err != nil || allowed {
		t.Fatal("cached collection survived policy change", allowed, err)
	}
	// A changed version invalidates the old override rather than renewing it.
	exec(`UPDATE memories SET content='confirmed horizonneedle' WHERE id=$1`, id)
	installTestHorizon(t, c)
	if _, err = s.Get(ctx, r.Scope, id); !errors.Is(err, ErrMemoryNotFound) {
		t.Fatal("stale override admitted", err)
	}
	current, err := s.getAtVersioned(ctx, r.Scope, id, true, "", true)
	if err != nil {
		t.Fatal(err)
	}
	var generation string
	if err := tx.QueryRow(ctx, `SELECT generation::text FROM memory_invalidation_outbox WHERE memory_id=$1 AND record_revision=$2::bigint AND operation='update' ORDER BY generation LIMIT 1`, id, current.Version.RecordRevision).Scan(&generation); err != nil {
		t.Fatal(err)
	}
	c.Overrides = []horizonOverride{{Version: *current.Version, Rule: horizonRule{ID: "operator-confirmed", Anchor: "confirmed", DurationSeconds: 3600}, ConfirmationGeneration: generation}}
	installTestHorizon(t, c)
	if _, err = s.Get(ctx, r.Scope, id); err != nil {
		t.Fatal("confirmed event refused", err)
	}
	d, err = s.utilityHorizonDecision(ctx, id, current.Version, "current", true)
	if err != nil || d.WouldExclude || d.RuleSource != "record_override" {
		t.Fatal(d, err)
	}
	c.Overrides[0].ConfirmationGeneration = strconv.FormatInt(9223372036854775807, 10)
	installTestHorizon(t, c)
	if _, err = s.Get(ctx, r.Scope, id); !errors.Is(err, ErrMemoryNotFound) {
		t.Fatal("forged confirmation admitted", err)
	}
	exec(`UPDATE memories SET lifecycle_state='pending' WHERE id=$1`, id)
	pendingVersion := *current.Version
	if err := tx.QueryRow(ctx, `SELECT record_revision::text FROM memories WHERE id=$1`, id).Scan(&pendingVersion.RecordRevision); err != nil {
		t.Fatal(err)
	}
	check.Sources[0].Channel = "native_open_commitments"
	check.Sources[0].Source.Version = pendingVersion
	if allowed, err := s.revalidateSources(ctx, check, r.Scope); err != nil || allowed {
		t.Fatal("pending release bypassed horizon", allowed, err)
	}
	t.Setenv("AIMEE_MEMORY_UTILITY_HORIZON_POLICY", "")
	if allowed, err := s.revalidateSources(ctx, check, r.Scope); err != nil || !allowed {
		t.Fatal("baseline pending commitment lost", allowed, err)
	}
	installTestHorizon(t, c)
	exec(`UPDATE memories SET lifecycle_state='revoked' WHERE id=$1`, id)
	if _, err = s.getAtVersioned(ctx, r.Scope, id, true, "", true); !errors.Is(err, ErrMemoryNotFound) {
		t.Fatal("history bypassed revocation", err)
	}
	// Disabling restores horizon admission only; revocation remains enforced.
	t.Setenv("AIMEE_MEMORY_UTILITY_HORIZON_POLICY", "")
	if _, err = s.Get(ctx, r.Scope, id); !errors.Is(err, ErrMemoryNotFound) {
		t.Fatal("rollback bypassed revocation", err)
	}
}
func TestUtilityHorizonConfigurationAdmission(t *testing.T) {
	_, p, _ := horizonFixture()
	c := horizonConfiguration{Policy: p, Mode: "enforce"}
	raw, _ := json.Marshal(c)
	if _, err := decodeHorizonConfiguration(string(raw)); err != nil {
		t.Fatal(err)
	}
	for _, bad := range []string{"null", string(raw) + " {}", `{"policy":{},"mode":"enforce"}`, `{"policy":{},"mode":"enforce","model_override":true}`} {
		if _, err := decodeHorizonConfiguration(bad); err == nil {
			t.Fatal("accepted", bad)
		}
	}
	t.Setenv("AIMEE_MEMORY_UTILITY_HORIZON_POLICY", "bad")
	if utilityHorizonSQL("m.", false) != "FALSE" {
		t.Fatal("malformed artifact failed open")
	}
}

func TestMemoryUtilityHorizonPersonalPostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_MEMORY_EVAL_URL")
	if dsn == "" {
		t.Skip("AIMEE_MEMORY_EVAL_URL required")
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
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	read := func(name string) string {
		t.Helper()
		raw, err := os.ReadFile("../aimee/families/" + name)
		if err != nil {
			t.Fatal(err)
		}
		return string(raw)
	}
	exec(`CREATE SCHEMA horizon_private_test; SET LOCAL search_path=horizon_private_test,public`)
	base := read("schema_conversation.sql")
	start := strings.Index(base, "CREATE TABLE IF NOT EXISTS user_memories (")
	end := strings.Index(base, "CREATE INDEX IF NOT EXISTS user_memories_recall")
	if start < 0 || end <= start {
		t.Fatal("private schema missing")
	}
	exec(base[start:end])
	for _, name := range []string{"schema_personal_memory_changes.sql", "schema_personal_memory_versions.sql", "schema_personal_memory_authority.sql", "schema_personal_memory_proposals.sql", "schema_personal_memory_horizon_index.sql"} {
		exec(read(name))
	}
	exec(`INSERT INTO user_memories(id,kind,tier,key,content) VALUES(1,'task_state','L2','horizon-private','private horizonneedle'),(2,'constraint','L2','durable-private','private horizonneedle')`)
	data, err := NewPostgresDataStore(evalQueryer{tx}, PlacementServer)
	if err != nil {
		t.Fatal(err)
	}
	s := data.(*postgresDataStore)
	scope := Scope{Type: ScopeUser, Value: "_user"}
	original, err := s.getAtVersioned(ctx, scope, 1, false, "", true)
	if err != nil {
		t.Fatal(err)
	}
	_, p, _ := horizonFixture()
	p.Kinds["task_state"] = horizonRule{ID: "instant", Anchor: "created", DurationSeconds: 0}
	c := horizonConfiguration{Policy: p, Mode: "enforce"}
	installTestHorizon(t, c)
	hits, err := s.Search(ctx, scope, "horizonneedle", "", "", 1)
	if err != nil || len(hits) != 1 || hits[0].ID != 2 {
		t.Fatal("private lexical", hits, err)
	}
	if _, err := s.Get(ctx, scope, 1); !errors.Is(err, ErrMemoryNotFound) {
		t.Fatal("private exact current leaked", err)
	}
	history, err := s.personalVersion(ctx, scope, *original.Version)
	if err != nil || !history.Historical || history.ID != 1 {
		t.Fatal("private retained inspection", history, err)
	}
	before, err := s.utilityHorizonDecision(ctx, 1, original.Version, "current", true)
	if err != nil || !before.WouldExclude {
		t.Fatal(before, err)
	}
	exec(`UPDATE user_memories SET use_count=use_count+100,last_used_at=now(),updated_at=now() WHERE id=1`)
	after, err := s.utilityHorizonDecision(ctx, 1, original.Version, "current", true)
	if err != nil || *before != *after {
		t.Fatal("private read renewed anchor", before, after, err)
	}
	c.Mode = "shadow"
	installTestHorizon(t, c)
	r, err := s.Get(ctx, scope, 1)
	if err != nil || r.UtilityHorizon == nil || !r.UtilityHorizon.WouldExclude {
		t.Fatal("private shadow", r, err)
	}
	c.Mode = "enforce"
	p.Kinds["task_state"] = horizonRule{ID: "hour", Anchor: "created", DurationSeconds: 3600}
	c.Policy = p
	installTestHorizon(t, c)
	observation, err := s.observeRecallCollection(ctx)
	if err != nil || observation.CollectionValidUntil == "" || observation.UtilityHorizonPolicyDigest != c.identity().Digest {
		t.Fatal("private collection boundary", observation, err)
	}
}

func TestUtilityHorizonHealthPopulation(t *testing.T) {
	p, events := healthFixture()
	yes, no := true, false
	events[0].Records[0].UtilityHorizonWouldExclude = &yes
	events[1].Records[0].UtilityHorizonWouldExclude = &no
	result, err := aggregateHealth(events, p)
	if err != nil || result.UtilityHorizon.Numerator != 1 || result.UtilityHorizon.Denominator != 2 || result.UtilityHorizon.Unknown != 1 {
		t.Fatal(result.UtilityHorizon, err)
	}
}
