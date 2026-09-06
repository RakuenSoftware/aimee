package memory

import (
	"context"
	"errors"
	"strings"
	"testing"

	store "github.com/JBailes/aimee/server-go/db"
)

type factRecallRows struct {
	values [][]any
	index  int
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

func (r *factRecallRows) Err() error { return nil }
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
