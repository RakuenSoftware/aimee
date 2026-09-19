package memory

import (
	"context"
	"errors"
	"fmt"
	"github.com/jackc/pgx/v5"
	"strings"
	"testing"

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
