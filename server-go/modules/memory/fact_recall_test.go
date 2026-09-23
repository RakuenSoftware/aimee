package memory

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"github.com/jackc/pgx/v5"
	"os"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/db"
)

type factRecallRows struct {
	values [][]any
	index  int
	err    error
}

func (r *factRecallRows) Next() bool {
	r.index++
	return r.index < len(r.values)
}

func (r *factRecallRows) Scan(dest ...any) error {
	if r.index < 0 || r.index >= len(r.values) || len(dest) != len(r.values[r.index]) {
		return errors.New("bad fact recall scan")
	}
	for i, value := range r.values[r.index] {
		switch target := dest[i].(type) {
		case *string:
			*target = value.(string)
		case *float64:
			*target = value.(float64)
		default:
			return errors.New("unsupported fact recall scan target")
		}
	}
	return nil
}

func (r *factRecallRows) Err() error { return r.err }
func (r *factRecallRows) Close()     {}

type factRecallQueryer struct {
	rows []store.Rows
	row  store.Row
}

func (q *factRecallQueryer) Query(context.Context, string, ...any) (store.Rows, error) {
	if len(q.rows) == 0 {
		return nil, errors.New("unexpected fact recall query")
	}
	rows := q.rows[0]
	q.rows = q.rows[1:]
	return rows, nil
}

func (*factRecallQueryer) Exec(context.Context, string, ...any) (store.Tag, error) {
	return store.RowsAffected(0), nil
}

func (q *factRecallQueryer) QueryRow(context.Context, string, ...any) store.Row { return q.row }

type factRecallRow struct{ values []any }

type countedFactQueryer struct {
	evalQueryer
	queries int
}

func (q *countedFactQueryer) Query(ctx context.Context, sql string, args ...any) (store.Rows, error) {
	q.queries++
	return q.evalQueryer.Query(ctx, sql, args...)
}
func (q *countedFactQueryer) QueryRow(ctx context.Context, sql string, args ...any) store.Row {
	q.queries++
	return q.evalQueryer.QueryRow(ctx, sql, args...)
}

// Compare the actual versioned recall implementation across revisions. The
// fixture and RLS role are transaction-owned and do not alter durable records.
func BenchmarkVersionedFactRecall(b *testing.B) {
	benchmarkVersionedFacts(b, false)
}

func BenchmarkSourceRevalidation(b *testing.B) {
	benchmarkVersionedFacts(b, true)
}

func benchmarkVersionedFacts(b *testing.B, revalidate bool) {
	dsn := os.Getenv("AIMEE_MEMORY_EVAL_URL")
	if dsn == "" {
		b.Skip("set AIMEE_MEMORY_EVAL_URL")
	}
	ctx := context.Background()
	conn, err := pgx.Connect(ctx, dsn)
	if err != nil {
		b.Fatal(err)
	}
	defer conn.Close(ctx)
	tx, err := conn.Begin(ctx)
	if err != nil {
		b.Fatal(err)
	}
	defer tx.Rollback(ctx)
	_, err = tx.Exec(ctx, `CREATE TEMP TABLE memories(id bigint PRIMARY KEY,record_revision bigint DEFAULT 1,
 lifecycle_state text DEFAULT 'active',activation_suppressed int DEFAULT 0,valid_from text DEFAULT '',valid_until text DEFAULT '',
 scope_type text DEFAULT 'project',scope_value text DEFAULT 'fact-bench');
 CREATE TEMP TABLE memory_collection_owner(id int,owner_id uuid);
 INSERT INTO memory_collection_owner VALUES(1,'00000000-0000-0000-0000-000000000001');
 INSERT INTO memories(id) SELECT n FROM generate_series(1,36)n;
 CREATE TEMP TABLE entity_edges(id bigint PRIMARY KEY,source text,relation text,target text,confidence float8 DEFAULT .9,version int DEFAULT 1,
 edge_class text DEFAULT 'semantic',assertion_kind text DEFAULT 'world_fact',lifecycle_state text DEFAULT 'persistent',suppressed int DEFAULT 0,
 valid_from text DEFAULT '',valid_until text DEFAULT '',asserted_at text DEFAULT '',superseded_at text DEFAULT '',invalidated_at text DEFAULT '');
 INSERT INTO entity_edges(id,source,relation,target) SELECT n,CASE WHEN n<=4 THEN 'user' ELSE 'Entity'||((n-1)/4)::text END,'role','engineer '||n::text FROM generate_series(1,36)n;
 CREATE INDEX ON entity_edges(source,confidence DESC,id);
 CREATE TEMP TABLE fact_evidence(assertion_id bigint,source_kind text,source_id text,invalidated_at text DEFAULT '',stance text DEFAULT 'supports');
 INSERT INTO fact_evidence(assertion_id,source_kind,source_id) SELECT id,'memory','memory:'||id::text FROM memories;
 CREATE INDEX ON fact_evidence(assertion_id);
 CREATE TEMP TABLE entity_registry(canonical_id bigint,status text);
 CREATE TEMP TABLE memory_episodes(id bigint PRIMARY KEY,memory_id bigint,record_revision bigint);
 CREATE TEMP TABLE derived_memory_dependencies(derived_kind text,derived_memory_id text,input_kind text,input_id text,input_version text,extractor_version text,derivation_policy_version text);
CREATE TEMP TABLE memory_summaries(id bigint PRIMARY KEY,memory_id bigint,record_revision bigint);
 CREATE TEMP TABLE entity_aliases(id bigint,canonical_id bigint,name text,name_norm text,suppressed int,is_preferred int);
 CREATE ROLE aimee_fact_benchmark NOINHERIT NOBYPASSRLS;
 GRANT SELECT ON derived_memory_dependencies,memories,memory_collection_owner,entity_edges,fact_evidence,entity_registry,entity_aliases,memory_episodes,memory_summaries TO aimee_fact_benchmark;
 ALTER TABLE memories ENABLE ROW LEVEL SECURITY;
 CREATE POLICY fact_bench_scope ON memories USING(scope_type='project' AND scope_value=current_setting('aimee.memory_project',true));
 SELECT set_config('aimee.memory_project','fact-bench',true);
 ANALYZE memories; ANALYZE entity_edges; ANALYZE fact_evidence;
 SET LOCAL ROLE aimee_fact_benchmark`)
	if err != nil {
		b.Fatal(err)
	}
	q := &countedFactQueryer{evalQueryer: evalQueryer{tx}}
	backend := &postgresDataStore{db: q, placement: PlacementKB}
	query := "Entity1 Entity2 Entity3 Entity4 Entity5 Entity6 Entity7 Entity8"
	var sources []typedProjectionRef
	for range 5 {
		text, count, p, err := backend.RecallFactProjection(ctx, "", query, false, 2048)
		if err != nil || count != 36 || !p.valid(text) {
			b.Fatal(count, p, err)
		}
		sources = p.Retained
	}
	request := &sourceRevalidation{SchemaVersion: 1, CheckID: strings.Repeat("a", 32), Sources: sources}
	q.queries = 0
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		if revalidate {
			ok, err := backend.revalidateSources(ctx, request, Scope{})
			if err != nil || !ok {
				b.Fatal(ok, err)
			}
			continue
		}
		_, count, p, err := backend.RecallFactProjection(ctx, "", query, false, 2048)
		if err != nil || count != 36 || len(p.Retained) != 36 {
			b.Fatal(count, p, err)
		}
	}
	b.StopTimer()
	b.ReportMetric(float64(q.queries)/float64(b.N), "queries/op")
}

func (r factRecallRow) Scan(dest ...any) error {
	rows := &factRecallRows{values: [][]any{r.values}, index: 0}
	return rows.Scan(dest...)
}

func TestTypedFactRecallPolicyLivesInGo(t *testing.T) {
	longTarget := strings.Repeat("x", factRecallLineCap)
	queryer := &factRecallQueryer{rows: []store.Rows{&factRecallRows{index: -1, values: [][]any{
		{"role", "engineer", .9},
		{"email", "ada@example.test", .9},
		{"password", "never-inject", 1.0},
		{"hobby", "fencing", .2},
		{"note", longTarget, .9},
	}}}}
	backend := &postgresDataStore{db: queryer, placement: PlacementKB}
	block, count, err := backend.RecallFacts(context.Background(), "Ada", "", false, 1024)
	if err != nil {
		t.Fatal(err)
	}
	if block != "- role: engineer\n" || count != 1 {
		t.Fatalf("block=%q count=%d", block, count)
	}

	queryer.rows = []store.Rows{&factRecallRows{index: -1, values: [][]any{
		{"role", "engineer", .9},
		{"email", "ada@example.test", .9},
		{"password", "never-inject", 1.0},
	}}}
	block, count, err = backend.RecallFacts(context.Background(), "Ada", "", true, 1024)
	if err != nil {
		t.Fatal(err)
	}
	if block != "- role: engineer\n- email: ada@example.test\n" || count != 2 {
		t.Fatalf("sensitive block=%q count=%d", block, count)
	}
}

func TestQueryRecallOwnsSensitiveClassification(t *testing.T) {
	for _, test := range []struct {
		query      string
		callerFlag bool
		wantEmail  bool
	}{
		{"what is my email", false, true},
		{"tell me about work", true, false},
		{"what is my password", true, true},
		{"", true, false},
	} {
		t.Run(test.query, func(t *testing.T) {
			queryer := &factRecallQueryer{rows: []store.Rows{
				&factRecallRows{index: -1, values: [][]any{
					{"role", "engineer", .9},
					{"email", "ada@example.test", .9},
					{"password", "never-inject", 1.0},
				}},
				&factRecallRows{index: -1},
				&factRecallRows{index: -1},
			}}
			backend := &postgresDataStore{db: queryer, placement: PlacementKB}
			client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, backend)))
			reply, err := client.Data(context.Background(), 73, DataRequest{
				Operation: "fact-recall", Query: test.query,
				TurnRequestsSensitive: test.callerFlag, ContentCapacity: 1024,
			})
			if err != nil || reply.Block == nil || reply.Count == nil {
				t.Fatalf("recall: %+v %v", reply, err)
			}
			want := "- role: engineer\n"
			count := 1
			if test.wantEmail {
				want += "- email: ada@example.test\n"
				count++
			}
			if *reply.Block != want || *reply.Count != count {
				t.Fatalf("block=%q count=%d want=%q", *reply.Block, *reply.Count, want)
			}
		})
	}
}

func TestMemoryValidAtUsesOpenBitemporalBounds(t *testing.T) {
	queryer := &factRecallQueryer{row: factRecallRow{values: []any{"2026-01-01 00:00:00", ""}}}
	backend := &postgresDataStore{db: queryer, placement: PlacementKB}
	valid, err := backend.ValidAt(context.Background(), 41, "2026-06-12T00:00:00Z")
	if err != nil || !valid {
		t.Fatalf("valid=%v err=%v", valid, err)
	}
	queryer.row = factRecallRow{values: []any{"2026-01-01 00:00:00", "2026-07-01 00:00:00"}}
	valid, err = backend.ValidAt(context.Background(), 41, "2026-07-01 00:00:00")
	if err != nil || valid {
		t.Fatalf("exclusive valid_until: valid=%v err=%v", valid, err)
	}
}

func TestTypedFactRecallRefusesPartialReads(t *testing.T) {
	user := func() store.Rows { return &factRecallRows{index: -1, values: [][]any{{"role", "engineer", .9}}} }
	registry := func() store.Rows { return &factRecallRows{index: -1, values: [][]any{{"Atlas"}}} }
	empty := func() store.Rows { return &factRecallRows{index: -1} }
	for name, rows := range map[string][]store.Rows{
		"registry unavailable":       {user()},
		"registry iteration":         {user(), &factRecallRows{index: -1, err: errors.New("read failed")}},
		"registry scan":              {user(), &factRecallRows{index: -1, values: [][]any{{"Atlas", "extra"}}}},
		"edge discovery unavailable": {user(), registry()},
		"entity block unavailable":   {user(), registry(), empty()},
		"entity block iteration":     {user(), registry(), empty(), &factRecallRows{index: -1, values: [][]any{{"role", "operator", .9}}, err: errors.New("read failed")}},
	} {
		t.Run(name, func(t *testing.T) {
			backend := &postgresDataStore{db: &factRecallQueryer{rows: rows}, placement: PlacementKB}
			block, count, err := backend.RecallFacts(context.Background(), "", "Atlas", false, 4096)
			if err == nil || block != "" || count != 0 {
				t.Fatalf("partial result %q count=%d err=%v", block, count, err)
			}
		})
	}
}

func exerciseCurrentFactRecallReplay(t *testing.T, ctx context.Context, tx pgx.Tx, backend *postgresDataStore) {
	t.Helper()
	exec := func(query string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, query, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SAVEPOINT current_fact_replay; SELECT set_config('aimee.memory_scope_all','1',true);
 INSERT INTO fact_graph_commits(commit_id,operation,actor_principal,actor_role,authority_rank,status)
 VALUES('current-fact-replay','fact.assert','test:current-fact','model',10,'open')`)
	defer func() { exec(`ROLLBACK TO SAVEPOINT current_fact_replay; RELEASE SAVEPOINT current_fact_replay`) }()
	parent := func(key, scope string) int64 {
		t.Helper()
		var id int64
		if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value)
 VALUES('L2','fact',$1,'current fact evidence','project',$2) RETURNING id`, key, scope).Scan(&id); err != nil {
			t.Fatal(err)
		}
		return id
	}
	local := parent("current-fact-local", "current-fact-project")
	hidden := parent("current-fact-private", "current-fact-private")
	future := parent("current-fact-future", "current-fact-project")
	expired := parent("current-fact-expired", "current-fact-project")
	suppressed := parent("current-fact-suppressed", "current-fact-project")
	exec(`UPDATE memories SET valid_from=(CURRENT_TIMESTAMP+interval '1 day')::text WHERE id=$1`, future)
	exec(`UPDATE memories SET valid_until=CURRENT_TIMESTAMP::text WHERE id=$1`, expired)
	exec(`UPDATE memories SET activation_suppressed=1 WHERE id=$1`, suppressed)
	fact := func(target string, confidence float64, parents ...int64) int64 {
		t.Helper()
		var id int64
		if err := tx.QueryRow(ctx, `INSERT INTO entity_edges(source,relation,target,edge_class,assertion_kind,lifecycle_state,confidence,confidence_class,commit_id,ontology_version)
 VALUES('CurrentFactEntity','role',$1,'semantic','world_fact','persistent',$2,'A','current-fact-replay',1) RETURNING id`, target, confidence).Scan(&id); err != nil {
			t.Fatal(err)
		}
		for _, p := range parents {
			exec(`INSERT INTO fact_evidence(assertion_id,source_kind,source_id) VALUES($1,'memory','memory:'||$2::bigint::text)`, id, p)
		}
		return id
	}
	good := fact("eligible operator", .9, local)
	for name, p := range map[string]int64{"hidden": hidden, "future": future, "expired": expired, "suppressed": suppressed, "missing": 9223372036854775807} {
		fact("excluded-"+name, .99, local, p)
	}
	// Enough high-confidence future assertions to crowd out the eligible row if
	// temporal selection happens after the fact cap.
	for i := 0; i < 40; i++ {
		id := fact(fmt.Sprintf("excluded-future-edge-%d", i), .99, local)
		exec(`UPDATE entity_edges SET valid_from=(CURRENT_TIMESTAMP+interval '1 day')::text WHERE id=$1`, id)
	}
	for _, column := range []string{"valid_until", "superseded_at", "invalidated_at"} {
		id := fact("excluded-"+column, .99, local)
		exec(`UPDATE entity_edges SET `+column+`=CURRENT_TIMESTAMP::text WHERE id=$1`, id)
	}
	id := fact("excluded-future-belief", .99, local)
	exec(`UPDATE entity_edges SET asserted_at=(CURRENT_TIMESTAMP+interval '1 day')::text WHERE id=$1`, id)
	exec(`SELECT set_config('aimee.memory_scope_all','0',true),set_config('aimee.memory_project','current-fact-project',true)`)
	check := func() {
		t.Helper()
		block, count, err := backend.RecallFacts(ctx, "CurrentFactEntity", "", false, 8192)
		if err != nil || block != "- role: eligible operator\n" || count != 1 {
			t.Fatalf("current fact recall %q count=%d err=%v", block, count, err)
		}
	}
	check()
	blockBefore, countBefore, projection, err := backend.RecallFactProjection(ctx, "CurrentFactEntity", "", false, 8192)
	if err != nil || countBefore != 1 || !projection.valid(blockBefore) || len(projection.Retained) != 1 || projection.Retained[0].ID != fmt.Sprint(good) || projection.Retained[0].Source.MemoryParents[0].RecordID != fmt.Sprint(local) {
		t.Fatal("fact source projection lost selected assertion or parent", projection, err)
	}
	exerciseSourceRevalidationReplay(t, ctx, tx, backend, projection.Retained)
	public, status := invokeContextCommand(t, NewHandler(nil, WithDataStore(PlacementKB, backend)), 0, bus.CommandContext{}, "facts", `{"query":"CurrentFactEntity","project":"current-fact-project","scope_context":true}`)
	if status != bus.ModuleStatusOK || public["status"] != "ok" {
		t.Fatal("public fact projection unavailable", public, status)
	}
	encoded, _ := json.Marshal(public["fact_projection"])
	var transported factProjection
	if json.Unmarshal(encoded, &transported) != nil || !transported.valid(public["facts"].(string)) {
		t.Fatal("public fact projection lost binding", public)
	}
	found := false
	for _, ref := range transported.Retained {
		if ref.ID == fmt.Sprint(good) {
			found = true
		}
	}
	if !found {
		t.Fatal("public fact projection omitted selected assertion", public)
	}
	exec(`SAVEPOINT fact_batch_parity`)
	exec(`WITH edge AS (INSERT INTO entity_edges(source,relation,target,edge_class,assertion_kind,lifecycle_state,confidence,confidence_class,commit_id,ontology_version)
 VALUES('ZuluFactEntity','role','x','semantic','world_fact','persistent',.9,'A','current-fact-replay',1) RETURNING id)
 INSERT INTO fact_evidence(assertion_id,source_kind,source_id) SELECT id,'memory','memory:'||$1::bigint::text FROM edge`, local)
	for _, capacity := range []int{1, 12, 26, 37, 200, 8192} {
		legacy, legacyCount, err := backend.RecallFacts(ctx, "", "CurrentFactEntity ZuluFactEntity", false, capacity)
		if err != nil {
			t.Fatal(err)
		}
		batched, batchedCount, metadata, err := backend.RecallFactProjection(ctx, "", "CurrentFactEntity ZuluFactEntity", false, capacity)
		if err != nil || batched != legacy || batchedCount != legacyCount || !metadata.valid(batched) {
			t.Fatal("batched fact query changed ordered per-entity selection", capacity, legacy, batched, metadata, err)
		}
		if capacity == 12 && !strings.Contains(batched, "role: x") {
			t.Fatal("oversized entity displaced smaller later entity", batched)
		}
	}
	exec(`ROLLBACK TO SAVEPOINT fact_batch_parity; RELEASE SAVEPOINT fact_batch_parity`)
	exec(`SAVEPOINT fact_source_revision`)
	exec(`UPDATE memories SET content=content||' changed supporting text' WHERE id=$1`, local)
	blockAfter, _, changed, err := backend.RecallFactProjection(ctx, "CurrentFactEntity", "", false, 8192)
	if err != nil || blockAfter != blockBefore || changed.ProjectionDigest != projection.ProjectionDigest || changed.SelectionDigest == projection.SelectionDigest || changed.Retained[0].Source.Version != projection.Retained[0].Source.Version || changed.Retained[0].Source.MemoryParents[0].RecordRevision == projection.Retained[0].Source.MemoryParents[0].RecordRevision {
		t.Fatal("same rendered fact reused stale parent binding", changed, err)
	}
	blockAfter, countAfter, empty, err := backend.RecallFactProjection(ctx, "CurrentFactEntity", "", false, 1)
	if err != nil || blockAfter != "" || countAfter != 0 || !empty.valid("") || len(empty.Retained) != 0 || empty.SourceVersionState != "unavailable" {
		t.Fatal("omitted fact claims retained sources", empty, err)
	}
	// A selected assertion with an incomplete parent set must refuse its block.
	exec(`WITH parents AS (INSERT INTO memories(tier,kind,key,content,scope_type,scope_value)
 SELECT 'L2','fact','fact-overflow-'||n,'source','project','current-fact-project' FROM generate_series(1,$1)n RETURNING id)
 INSERT INTO fact_evidence(assertion_id,source_kind,source_id) SELECT $2,'memory','memory:'||id::text FROM parents`, maxTypedMemoryParents, good)
	blockAfter, countAfter, overflow, err := backend.RecallFactProjection(ctx, "CurrentFactEntity", "", false, 8192)
	if err == nil || blockAfter != "" || countAfter != 0 || overflow != nil {
		t.Fatal("partial fact provenance presented as complete", overflow, err)
	}
	check() // Ordinary legacy recall still uses its existing selection contract.
	exec(`ROLLBACK TO SAVEPOINT fact_source_revision; RELEASE SAVEPOINT fact_source_revision`)
	// Malformed world time must refuse the result, never leave a plausible
	// partial fact block. The caller can recover after its scoped rollback.
	exec(`SAVEPOINT current_fact_malformed`)
	exec(`UPDATE entity_edges SET valid_from='tomorrow' WHERE id=$1`, good)
	block, count, err := backend.RecallFacts(ctx, "CurrentFactEntity", "", false, 8192)
	if err == nil || block != "" || count != 0 {
		t.Fatal("malformed current fact admitted", block, count, err)
	}
	exec(`ROLLBACK TO SAVEPOINT current_fact_malformed; RELEASE SAVEPOINT current_fact_malformed`)
	check()
}
