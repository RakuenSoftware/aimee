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

func (tx runtimeRoleTx) Exec(ctx context.Context, sql string, args ...any) (store.Tag, error) {
	tag, err := tx.evalQueryer.Exec(ctx, sql, args...)
	if err != nil {
		tx.t.Logf("runtime SQL exec: %v", err)
	}
	return tag, err
}
func (tx runtimeRoleTx) Query(ctx context.Context, sql string, args ...any) (store.Rows, error) {
	rows, err := tx.evalQueryer.Query(ctx, sql, args...)
	if err != nil {
		tx.t.Logf("runtime SQL query: %v", err)
	}
	return rows, err
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
		if os.Getenv("AIMEE_MEMORY_REPLAY_REQUIRED") == "1" {
			t.Fatal("AIMEE_DB2_REPLAY_URL required")
		}
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
has_table_privilege(current_user,'css_rules','UPDATE') OR
has_column_privilege(current_user,'css_declarations','value','SELECT') OR
has_table_privilege(current_user,'learning_observations','UPDATE') OR
has_table_privilege(current_user,'learning_proposals','UPDATE') OR
has_column_privilege(current_user,'learning_proposals','evidence_refs','SELECT') OR
has_table_privilege(current_user,'kb_vault_control','UPDATE') OR
has_table_privilege(current_user,'org_vault_secret','SELECT') OR
has_column_privilege(current_user,'files','path','SELECT') OR
has_column_privilege(current_user,'work_outcomes','resulting_action','SELECT') OR
has_schema_privilege(current_user,'public','CREATE') OR
(SELECT rolsuper OR rolbypassrls FROM pg_roles WHERE rolname=current_user)`).Scan(&forbidden); err != nil || forbidden {
		t.Fatalf("runtime gained owner/secret privileges: forbidden=%v err=%v", forbidden, err)
	}
	backend, err := NewPostgresDataStore(runtimeRoleDB{evalQueryer{tx}, t}, PlacementKB)
	if err != nil {
		t.Fatal(err)
	}
	exerciseReembedReplay(t, ctx, tx, backend.(*postgresDataStore))
	exerciseSharedRecallReplay(t, ctx, tx, backend.(*postgresDataStore))
	handler := NewHandler(nil, WithDataStore(PlacementKB, backend))
	exerciseExpectedVersionReplay(t, ctx, tx, handler)
	exerciseMutationRetryReplay(t, ctx, tx, handler)
	exerciseDemotionReplay(t, ctx, tx, handler)
	exerciseCodeContextReplay(t, ctx, tx, handler)
	exerciseScopeReplay(t, ctx, tx, handler)
	exerciseTraceStoreReplay(t, ctx, tx, handler)
	exerciseBenchmarkContextReplay(t, ctx, tx, handler)
	exerciseBenchmarkDiagnosticsReplay(t, ctx, tx, handler)
	exerciseBenchmarkScoreReplay(t, ctx, tx, handler)
	exerciseAssertionSearchReplay(t, ctx, tx, backend.(*postgresDataStore))
	exerciseCurrentFactRecallReplay(t, ctx, tx, backend.(*postgresDataStore))
	exerciseTypedContextReplay(t, ctx, tx, backend.(*postgresDataStore))
	exerciseCSSConventionsReplay(t, ctx, tx, backend.(*postgresDataStore))
	exerciseOntologyReviewReplay(t, ctx, tx, backend.(*postgresDataStore))
	exerciseEntityMutationReplay(t, ctx, tx, backend.(*postgresDataStore))
	exerciseEntityRegistryReplay(t, ctx, tx, backend.(*postgresDataStore))
	exerciseCheckpointReplay(t, ctx, tx, handler)
	exerciseConventionReplay(t, ctx, tx, handler)
	exerciseHybridReplay(t, ctx, tx, handler)
	exerciseSessionQueryReplay(t, ctx, tx, handler)
	exerciseSearchViewReplay(t, ctx, tx, handler)
	exerciseWikiReplay(t, ctx, tx, handler)
	exerciseBriefingReplay(t, ctx, tx, handler)
	exerciseAlertsReplay(t, ctx, tx, handler)
	exerciseRecallReplay(t, ctx, tx, handler)
	exerciseMutationAuditReplay(t, ctx, tx, backend.(*postgresDataStore))
	exerciseReflectionReplay(t, ctx, tx, backend.(*postgresDataStore))
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
	// Replaces the former C fixed-buffer insert/merge regression. Check full
	// storage and read-back under explicit request scope, across alternating
	// project requests, so neither TLS state nor a capped ABI can hide loss.
	for _, size := range []int{2047, 2048, 4096, 40000} {
		content := strings.Repeat("abcdefghijklmnopqrstuvwxyz", (size+25)/26)[:size] + "界"
		key := fmt.Sprintf("long-content-%d", size)
		request := DataRequest{Operation: "insert-epistemic", Project: "long-content-project", Tier: "L2", Kind: "fact", Key: key, Content: content, SessionID: "long-session"}
		first := call(request)
		if len(first.Records) != 1 || first.Records[0].Content != content {
			t.Fatalf("long insert lost content at %d", size)
		}
		id := first.Records[0].ID
		// Same key in another project must not merge with this record.
		other := request
		other.Project, other.Content = "other-long-project", "private other content"
		separate := call(other)
		if len(separate.Records) != 1 || separate.Records[0].ID == id {
			t.Fatal("same-key merge crossed projects", separate)
		}
		again := call(request)
		if len(again.Records) != 1 || again.Records[0].ID != id || again.Records[0].Content != content {
			t.Fatalf("long merge lost content or identity at %d", size)
		}
		got := call(DataRequest{Operation: "get", ID: id, Project: request.Project})
		if len(got.Records) != 1 || got.Records[0].Content != content {
			t.Fatalf("long read lost content at %d", size)
		}
		var stored string
		if err := tx.QueryRow(ctx, `SELECT content FROM memories WHERE id=$1`, id).Scan(&stored); err != nil || stored != content {
			t.Fatalf("long stored content at %d: %v", size, err)
		}
		missing := call(DataRequest{Operation: "search", Query: key})
		if len(missing.Records) != 0 {
			t.Fatal("scope leaked between requests", missing)
		}
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
	stored := command("store", `{"key":"runtime-public#v123","content":"original","tier":"L2","scope_context":true,"project":"runtime-project-a"}`, true)
	oldID := int64(stored["id"].(float64))
	checkAudit(oldID, caller.Principal, "model")
	if _, err := tx.Exec(ctx, `INSERT INTO memory_scopes(memory_id,scope_type,scope_value) VALUES ($1,'workspace','runtime-team')`, oldID); err != nil {
		t.Fatal(err)
	}
	// Scope copies run after the new parent becomes visible to child RLS, but
	// remain in the same request transaction. A late copy failure must restore
	// the original active row and every collection position.
	var generationBefore, generationAfter int64
	if err := tx.QueryRow(ctx, `SELECT generation FROM memory_collection_generations WHERE scope_type='project' AND scope_value='runtime-project-a'`).Scan(&generationBefore); err != nil {
		t.Fatal(err)
	}
	if _, err := tx.Exec(ctx, fmt.Sprintf(`RESET ROLE;
ALTER TABLE memory_scopes ADD CONSTRAINT runtime_scope_copy_failure CHECK(memory_id=%d OR scope_value<>'runtime-team') NOT VALID;
SET LOCAL ROLE aimee_store_runtime`, oldID)); err != nil {
		t.Fatal(err)
	}
	failedCopy, _ := invokeContextCommand(t, handler, 0, caller, "update", fmt.Sprintf(`{"id":%d,"content":"must roll back","scope_context":true,"project":"runtime-project-a"}`, oldID))
	if failedCopy["status"] == "ok" {
		t.Fatal("scope copy failure accepted replacement", failedCopy)
	}
	var originalStillActive bool
	if err := tx.QueryRow(ctx, `SELECT lifecycle_state='active' AND content='original' FROM memories WHERE id=$1`, oldID).Scan(&originalStillActive); err != nil || !originalStillActive {
		t.Fatal("scope copy failure retired the original", originalStillActive, err)
	}
	if err := tx.QueryRow(ctx, `SELECT generation FROM memory_collection_generations WHERE scope_type='project' AND scope_value='runtime-project-a'`).Scan(&generationAfter); err != nil || generationAfter != generationBefore {
		t.Fatal("failed replacement published invalidation", generationBefore, generationAfter, err)
	}
	if _, err := tx.Exec(ctx, `RESET ROLE; ALTER TABLE memory_scopes DROP CONSTRAINT runtime_scope_copy_failure; SET LOCAL ROLE aimee_store_runtime`); err != nil {
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
	corrected := command("update", fmt.Sprintf(`{"id":%d,"content":"operator correction","authority":"user","scope_context":true,"project":"runtime-project-a"}`, newID), true)
	if corrected["superseded"] != true || corrected["id"] == float64(newID) {
		t.Fatal(corrected)
	}
	newID = int64(corrected["id"].(float64))
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
	exerciseCurrentEligibilityReplay(t, ctx, tx, backend.(*postgresDataStore))
	exerciseDerivedTextReplay(t, ctx, tx, backend.(*postgresDataStore))
	exerciseDerivedUnitsReplay(t, ctx, tx, backend.(*postgresDataStore))
	exerciseDerivedRelationsReplay(t, ctx, tx, backend.(*postgresDataStore))
	exerciseCoreferenceReplay(t, ctx, tx, backend.(*postgresDataStore))
	exerciseNegationReplay(t, ctx, tx, backend.(*postgresDataStore))
	exerciseSharedIndexReplay(t, ctx, tx, backend.(*postgresDataStore))
	exerciseGraphFusionReplay(t, ctx, tx, backend.(*postgresDataStore))
	exercisePageRankReplay(t, ctx, tx, backend.(*postgresDataStore))
	exercisePageRankRecallReplay(t, ctx, tx, backend.(*postgresDataStore))
	exerciseCognifyReplay(t, ctx, tx, backend.(*postgresDataStore))
	exerciseRejectionReplay(t, ctx, tx, handler)
	exerciseGraphFeedbackReplay(t, ctx, tx, backend.(*postgresDataStore))

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
