package memory

import (
	"context"
	"encoding/json"
	"fmt"
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

	// Exercise public writes with the packaged audit triggers and the real
	// restricted store role, not only the minimal command fixtures.
	caller := bus.CommandContext{Authenticated: true, Principal: "user:runtime-probe", TransportIdentity: "cert:runtime-probe", UserAuthority: true}
	command := func(verb, args string, verified bool) map[string]any {
		t.Helper()
		context := bus.CommandContext{}
		if verified {
			context = caller
		}
		r, status := invokeContextCommand(t, handler, 0, context, verb, args)
		if status != bus.ModuleStatusOK || r["status"] != "ok" {
			t.Fatalf("%s: %v status=%d", verb, r, status)
		}
		return r
	}
	checkAudit := func(id int64, principal, authority string) {
		t.Helper()
		var count int
		err := tx.QueryRow(ctx, `SELECT count(*) FROM fact_graph_changes c JOIN fact_graph_commits g USING(commit_id)
WHERE c.object_kind='memory' AND c.object_key=$1 AND g.actor_principal=$2 AND g.actor_role=$3 AND g.status='applied'`, fmt.Sprint(id), principal, authority).Scan(&count)
		if err != nil || count == 0 {
			t.Fatalf("missing audit for %d actor=%s authority=%s: count=%d err=%v", id, principal, authority, count, err)
		}
	}
	for _, verb := range []string{"find_facts_visible", "find_facts_scoped"} {
		for _, project := range []string{"runtime-project-a", "runtime-project-b", ""} {
			args := map[string]any{"query": "runtime-role-probe", "include_all": true}
			if verb == "find_facts_visible" {
				args["project"] = project
			} else if project != "" {
				args["scope_type"], args["scope_value"] = "project", project
			}
			encoded, _ := json.Marshal(args)
			r := command(verb, string(encoded), true)
			rows := r["facts"].([]any)
			if project == "" {
				if len(rows) != 0 {
					t.Fatalf("%s leaked project records: %v", verb, rows)
				}
			} else if len(rows) != 1 || rows[0].(map[string]any)["content"] != project {
				t.Fatal(verb, project, rows)
			}
		}
	}
	stored := command("store", `{"key":"runtime-public#v123","content":"original","authority":"user","tier":"L2","scope_context":true,"project":"runtime-project-a"}`, true)
	oldID := int64(stored["id"].(float64))
	checkAudit(oldID, caller.Principal, "user")
	if _, err := tx.Exec(ctx, `INSERT INTO memory_scopes(memory_id,scope_type,scope_value) VALUES ($1,'workspace','runtime-team')`, oldID); err != nil {
		t.Fatal(err)
	}
	updated := command("update", fmt.Sprintf(`{"id":%d,"content":"model replacement","scope_context":true,"project":"runtime-project-a"}`, oldID), true)
	newID := int64(updated["id"].(float64))
	if newID == oldID || updated["superseded"] != true {
		t.Fatal(updated)
	}
	checkAudit(newID, caller.Principal, "model")
	var key, provenance, actor, role string
	var ceiling float64
	var inherited, interval, link bool
	err = tx.QueryRow(ctx, `SELECT n.key,n.provenance_category,n.confidence_ceiling,a.actor_principal,a.actor_role,
o.valid_until=n.valid_from AND n.valid_from<>'',
EXISTS(SELECT 1 FROM memory_scopes WHERE memory_id=n.id AND scope_value='runtime-team'),
EXISTS(SELECT 1 FROM memory_links WHERE source_id=n.id AND target_id=o.id AND relation='supersedes')
FROM memories n JOIN memory_fact_actors a ON a.memory_id=n.id CROSS JOIN memories o WHERE n.id=$1 AND o.id=$2`, newID, oldID).
		Scan(&key, &provenance, &ceiling, &actor, &role, &interval, &inherited, &link)
	if err != nil || key != "runtime-public#v123" || provenance != "agent_message" || ceiling != 0.8 || actor != "system:model-inference" || role != "model" || !interval || !inherited || !link {
		t.Fatalf("replacement: %s %s %g %s %s interval=%v scope=%v link=%v err=%v", key, provenance, ceiling, actor, role, interval, inherited, link, err)
	}
	command("update", fmt.Sprintf(`{"id":%d,"content":"operator correction","authority":"user","scope_context":true,"project":"runtime-project-a"}`, newID), true)
	checkAudit(newID, caller.Principal, "user")
	if err := tx.QueryRow(ctx, `SELECT m.provenance_category,m.confidence_ceiling,a.actor_principal,a.actor_role FROM memories m JOIN memory_fact_actors a ON a.memory_id=m.id WHERE m.id=$1`, newID).Scan(&provenance, &ceiling, &actor, &role); err != nil || provenance != "user_stated" || ceiling != 1 || actor != caller.Principal || role != "user" {
		t.Fatal(provenance, ceiling, actor, role, err)
	}
	forged := command("store", `{"key":"runtime-forged","content":"model note","authority":"user","actor":"user:forged","authenticated":true}`, false)
	checkAudit(int64(forged["id"].(float64)), "system:model-inference", "model")
	command("reject", fmt.Sprintf(`{"id":%d}`, newID), true)
	command("restore", fmt.Sprintf(`{"id":%d}`, newID), true)
	command("delete", fmt.Sprintf(`{"id":%d,"authority":"user"}`, newID), true)
	exerciseMaintenanceReplay(t, ctx, tx, handler)
	exerciseDiagnosticReplay(t, ctx, tx, handler)
	exerciseAnswerReplay(t, ctx, tx, backend.(*postgresDataStore))
	exerciseVectorMaintenanceReplay(t, ctx, tx, backend.(*postgresDataStore))
	exerciseVectorRepairReplay(t, ctx, tx, backend.(*postgresDataStore))
	exerciseVectorVerifyReplay(t, ctx, tx, backend.(*postgresDataStore))
	exerciseSessionReplay(t, ctx, tx, handler)
	exerciseLearningMutationReplay(t, ctx, tx, handler)
	exerciseRuntimeRecordReplay(t, ctx, tx, handler)
	exerciseRetrievalPolicyReplay(t, ctx, tx, backend.(*postgresDataStore))

	// Calls use nested transactions in this fixture; releasing a savepoint
	// retains SET LOCAL until the enclosing transaction ends. Production store
	// transactions are top-level, so inspect reset at that same boundary.
	if err := tx.Rollback(ctx); err != nil {
		t.Fatal(err)
	}
	var retained string
	for _, setting := range []string{"aimee.memory_scope_value", "aimee.principal", "aimee.authority", "aimee.transport_identity", "aimee.correlation_id"} {
		if err := conn.QueryRow(ctx, `SELECT COALESCE(current_setting($1,true),'')`, setting).Scan(&retained); err != nil || retained != "" {
			t.Fatalf("request setting %s retained after transaction: %q %v", setting, retained, err)
		}
	}
}
