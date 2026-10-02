package memory

import (
	"context"
	"os"
	"testing"

	"github.com/jackc/pgx/v5"
)

func TestSelectedPersonalExportUsesCurrentEligibility(t *testing.T) {
	dsn := os.Getenv("AIMEE_TEST_DATABASE_URL")
	if dsn == "" {
		t.Skip("set disposable database URL")
	}
	ctx := context.Background()
	conn, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close(ctx)
	if _, err = conn.Exec(ctx, `BEGIN;
CREATE TEMP TABLE user_memories (id bigint PRIMARY KEY, lifecycle_state text, valid_until timestamptz, content text);
INSERT INTO user_memories VALUES
 (1,'active',NULL,'available'),
 (2,'retired',NULL,'retired'),
 (3,'active',now()-interval '1 day','expired');`); err != nil {
		t.Fatal(err)
	}
	defer conn.Exec(ctx, "ROLLBACK")
	db := restrictedDB{conn}
	rows, err := ExportMemoryRowsSelected(ctx, db, PlacementServer, Scope{Type: ScopeUser}, []string{"1", "2", "3"})
	if err != nil || len(rows.Rows) != 1 || rows.Scope.Type != ScopeUser || rows.Scope.Value != "_user" {
		t.Fatalf("selected export: %+v %v", rows, err)
	}
	if _, err = ExportMemoryRowsSelected(ctx, db, PlacementServer, Scope{Type: ScopeUser}, []string{"01"}); err == nil {
		t.Fatal("noncanonical ID accepted")
	}
	if _, err = ExportMemoryRowsSelected(ctx, db, PlacementServer, Scope{Type: ScopeGlobal}, []string{"1"}); err == nil {
		t.Fatal("KB scope accepted by personal export")
	}
	empty, err := ExportMemoryRowsSelected(ctx, db, PlacementServer, Scope{}, nil)
	if err != nil || len(empty.Rows) != 0 {
		t.Fatalf("empty selection widened: %+v %v", empty, err)
	}
}
