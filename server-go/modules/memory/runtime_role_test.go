package memory

import (
	"context"
	"encoding/json"
	"os"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/db"
	"github.com/jackc/pgx/v5"
)

type runtimeRoleDB struct {
	evalQueryer
	t *testing.T
}

type runtimeRoleTx struct {
	evalQueryer
	t *testing.T
}

type runtimeRoleRow struct {
	store.Row
	t *testing.T
}

func (r runtimeRoleRow) Scan(dest ...any) error {
	err := r.Row.Scan(dest...)
	if err != nil {
		r.t.Logf("runtime SQL: %v", err)
	}
	return err
}

func (tx runtimeRoleTx) QueryRow(ctx context.Context, sql string, args ...any) store.Row {
	return runtimeRoleRow{tx.evalQueryer.QueryRow(ctx, sql, args...), tx.t}
}

func (db runtimeRoleDB) Begin(ctx context.Context) (store.Tx, error) {
	tx, err := db.Tx.Begin(ctx)
	if err != nil {
		return nil, err
	}
	return runtimeRoleTx{evalQueryer{tx}, db.t}, nil
}

func TestMemoryRuntimeRoleReplay(t *testing.T) {
	url := os.Getenv("AIMEE_DB2_REPLAY_URL")
	if url == "" {
		t.Skip("set AIMEE_DB2_REPLAY_URL to the packaged DB2 replay database")
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
	_, err = tx.Exec(ctx, `DO $$ BEGIN
IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname='aimee_store_runtime') THEN
CREATE ROLE aimee_store_runtime NOINHERIT NOBYPASSRLS;
END IF;
END $$;
GRANT USAGE ON SCHEMA public TO aimee_store_runtime`)
	if err != nil {
		t.Fatal(err)
	}
	// Run the actual migration grant block, including the upgrade/reapply path.
	schema, err := os.ReadFile("../../../src/modules/db2/c/schema.sql")
	if err != nil {
		t.Fatal(err)
	}
	start := strings.Index(string(schema), "DO $memory_store_grants$")
	end := strings.Index(string(schema), "END\n$memory_store_grants$;")
	if start < 0 || end < start {
		t.Fatal("memory runtime grant migration missing")
	}
	grants := string(schema[start : end+len("END\n$memory_store_grants$;")])
	for i := 0; i < 2; i++ {
		if _, err := tx.Exec(ctx, grants); err != nil {
			t.Fatal(err)
		}
	}
	_, err = tx.Exec(ctx, "SET LOCAL ROLE aimee_store_runtime")
	if err != nil {
		t.Fatal(err)
	}
	var forbidden bool
	if err := tx.QueryRow(ctx, `SELECT
has_table_privilege(current_user,'kb_vault_control','UPDATE') OR
has_table_privilege(current_user,'org_vault_secret','SELECT') OR
has_schema_privilege(current_user,'public','CREATE') OR
(SELECT rolsuper OR rolbypassrls FROM pg_roles WHERE rolname=current_user)`).Scan(&forbidden); err != nil || forbidden {
		t.Fatalf("runtime gained owner/secret privileges: forbidden=%v err=%v", forbidden, err)
	}
	backend, err := NewPostgresDataStore(runtimeRoleDB{evalQueryer{tx}, t}, PlacementKB)
	if err != nil {
		t.Fatal(err)
	}
	handler := NewHandler(nil, WithDataStore(PlacementKB, backend))
	call := func(request DataRequest) DataResponse {
		t.Helper()
		reply, status := handler(bus.ModuleInvocation{StageID: StageData}, dataRequest(t, request))
		if status != bus.ModuleStatusOK {
			t.Fatalf("%s (%s): status %d", request.Operation, request.Project, status)
		}
		var response DataResponse
		if err := json.Unmarshal(reply, &response); err != nil {
			t.Fatal(err)
		}
		return response
	}
	for _, project := range []string{"runtime-project-a", "runtime-project-b"} {
		written := call(DataRequest{Operation: "insert-epistemic", Project: project,
			Tier: "L0", Kind: "fact", Key: "runtime-role-probe", Content: project})
		if len(written.Records) != 1 || written.Records[0].ID <= 0 {
			t.Fatalf("missing committed memory: %+v", written)
		}
	}
	for _, project := range []string{"runtime-project-a", "runtime-project-b", ""} {
		result := call(DataRequest{Operation: "search", Project: project, Query: "runtime-role-probe"})
		if project == "" {
			if len(result.Records) != 0 {
				t.Fatal("global request leaked project memory")
			}
		} else if len(result.Records) != 1 || result.Records[0].Content != project {
			t.Fatalf("project scope mismatch: %+v", result)
		}
	}
	var leaked int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memories WHERE key='runtime-role-probe'`).Scan(&leaked); err != nil || leaked != 0 {
		t.Fatalf("request scope leaked into pooled connection: rows=%d err=%v", leaked, err)
	}
}
