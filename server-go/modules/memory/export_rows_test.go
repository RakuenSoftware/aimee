package memory

import (
	"context"
	"encoding/json"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"reflect"
	"strings"
	"testing"
)

type exportRows struct {
	done bool
	raw  []byte
}

func (r *exportRows) Next() bool {
	if r.done {
		return false
	}
	r.done = true
	return true
}
func (r *exportRows) Scan(dst ...any) error { *dst[0].(*[]byte) = r.raw; return nil }
func (r *exportRows) Err() error            { return nil }
func (r *exportRows) Close()                {}

type exportDB struct {
	sql  string
	args []any
	raw  []byte
}

func (q *exportDB) Query(_ context.Context, sql string, args ...any) (store.Rows, error) {
	q.sql = sql
	q.args = args
	return &exportRows{raw: q.raw}, nil
}
func (q *exportDB) QueryRow(context.Context, string, ...any) store.Row { panic("unexpected QueryRow") }
func (q *exportDB) Exec(context.Context, string, ...any) (store.Tag, error) {
	panic("export attempted write")
}
func TestExportRowsPreservesSourceAndPlacement(t *testing.T) {
	raw := []byte(`{"id":5,"source_session":"s1","content":"oolong","lifecycle_state":"active"}`)
	db := &exportDB{raw: raw}
	got, e := ExportMemoryRows(context.Background(), db, PlacementServer, Scope{})
	if e != nil || len(got.Rows) != 1 || string(got.Rows[0]) != string(raw) || got.Scope != (Scope{ScopeUser, "_user"}) {
		t.Fatalf("%+v %v", got, e)
	}
	if !strings.Contains(db.sql, "FROM user_memories") || len(db.args) != 0 {
		t.Fatal("server accessed KB")
	}
	before := db.sql
	if _, e = ExportMemoryRows(context.Background(), db, PlacementServer, Scope{Type: ScopeGlobal}); e == nil || db.sql != before {
		t.Fatal("wrong scope reached database")
	}
	got, e = ExportMemoryRows(context.Background(), db, PlacementKB, Scope{Type: ScopeProject, Value: "p"})
	if e != nil || got.Scope.Value != "p" || !strings.Contains(db.sql, "FROM memories") || !reflect.DeepEqual(db.args, []any{"project", "p"}) {
		t.Fatalf("%+v %v", got, e)
	}
	if _, e = ExportMemoryRows(context.Background(), db, PlacementKB, Scope{Type: ScopeUser}); e == nil {
		t.Fatal("KB exported user rows")
	}
}
func TestExportSelectionNeverBroadensOrWrites(t *testing.T) {
	db := &exportDB{raw: []byte(`{"id":5}`)}
	for _, ids := range [][]string{nil, {}} {
		got, e := ExportMemoryRowsSelected(context.Background(), db, PlacementServer, Scope{}, ids)
		if e != nil || len(got.Rows) != 0 || db.sql != "" {
			t.Fatal("empty selection reached database")
		}
	}
	if _, e := ExportMemoryRowsSelected(context.Background(), db, PlacementServer, Scope{}, []string{"5 OR 1=1"}); e == nil || db.sql != "" {
		t.Fatal("invalid ID reached database")
	}
	got, e := ExportMemoryRowsSelected(context.Background(), db, PlacementServer, Scope{}, []string{"5"})
	if e != nil || !json.Valid(got.Rows[0]) || !strings.Contains(db.sql, "WHERE id IN") || !reflect.DeepEqual(db.args, []any{`["5"]`}) {
		t.Fatalf("%+v %v", got, e)
	}
}
