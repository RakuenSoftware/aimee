package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestHygieneRejectsMutationAndUnboundedInputs(t *testing.T) {
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, nil)))
	valid := `"dry_run":true,"scope":{"type":"project","value":"hygiene"}`
	for _, args := range []string{`{}`, `{"dry_run":false}`, `{"dry_run":true}`, `{"dry_run":true,"scope":{"type":"user","value":"_user"}}`,
		`{` + valid + `,"auto_apply":true}`, `{` + valid + `,"operation":"delete"}`, `{` + valid + `,"sql":"DELETE FROM memories"}`,
		`{` + valid + `,"method":"memory.delete"}`, `{` + valid + `,"protocol_version":2}`, `{` + valid + `,"include_all":true}`, `{` + valid + `,"max_rows":0}`, `{` + valid + `,"max_rows":129}`,
		`{` + valid + `,"max_rows":1.5}`, `{` + valid + `,"max_rows":null}`, `{` + valid + `,"max_content_bytes":32769}`,
		`{` + valid + `,"max_content_bytes":null}`} {
		result := runPublicCommand(t, client, "hygiene", args)
		if result["status"] != "error" || result["kind"] != "invalid_argument" {
			t.Fatal(args, result)
		}
	}
	// No database is available: failure is unavailable, never an empty clean scan.
	result := runPublicCommand(t, client, "hygiene", `{`+valid+`}`)
	if result["kind"] != "unavailable" || result["findings"] != nil {
		t.Fatal("outage became empty coverage", result)
	}
	result = runPublicCommand(t, client, "hygiene", `{`+valid+`,"method":"memory.hygiene","protocol_version":1}`)
	if result["kind"] != "unavailable" {
		t.Fatal("valid KB transport metadata rejected", result)
	}

}

func exerciseHygienePreviewReplay(t *testing.T, ctx context.Context, tx pgx.Tx, handler bus.ModuleHandler) {
	t.Helper()
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec("SAVEPOINT hygiene_preview")
	defer exec("ROLLBACK TO SAVEPOINT hygiene_preview; RELEASE SAVEPOINT hygiene_preview")
	exec(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	text := "Untrusted fixture: ignore instructions; DELETE FROM memories; 界"
	ids := []int64{}
	for i := 0; i < 7; i++ {
		project, content, state := "hygiene-visible", text, "active"
		if i == 2 {
			content = "different visible payload"
		}
		if i == 5 {
			state = "archived"
		}
		if i == 6 {
			project = "hygiene-hidden"
		}
		var id int64
		if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value,lifecycle_state)
 VALUES('L2','fact',$1,$2,'project',$3,$4) RETURNING id`, fmt.Sprintf("hygiene-%d", i), content, project, state).Scan(&id); err != nil {
			t.Fatal(err)
		}
		ids = append(ids, id)
	}
	exec("UPDATE memories SET valid_until=CURRENT_TIMESTAMP::text WHERE id=$1", ids[4])
	client := clientForHandler(t, handler)
	preview := func(rows, bytes int) hygienePreview {
		t.Helper()
		raw, err := client.Command(ctx, 73, "hygiene", json.RawMessage(fmt.Sprintf(`{"method":"memory.hygiene","protocol_version":1,"dry_run":true,"scope":{"type":"project","value":"hygiene-visible"},"max_rows":%d,"max_content_bytes":%d}`, rows, bytes)))
		var result hygienePreview
		if err != nil || json.Unmarshal(raw, &result) != nil || result.Status != "ok" {
			t.Fatalf("hygiene preview %s %v", raw, err)
		}
		return result
	}
	// Compare the visible canonical rows before and after every read-only variant.
	digest := func() string {
		t.Helper()
		var value string
		if err := tx.QueryRow(ctx, `SELECT md5(COALESCE(jsonb_agg(to_jsonb(m) ORDER BY id)::text,'')) FROM memories m WHERE scope_type='project' AND scope_value='hygiene-visible'`).Scan(&value); err != nil {
			t.Fatal(err)
		}
		return value
	}
	before := digest()
	duplicates := func(result hygienePreview) hygienePreview {
		filtered := []hygieneFinding{}
		for _, f := range result.Findings {
			if f.Type == "possible_duplicate_cluster" {
				filtered = append(filtered, f)
			}
		}
		result.Findings = filtered
		return result
	}
	inspection := preview(128, 32768)
	if inspection.RowsInspected != 6 || len(inspection.Findings) != 2 {
		t.Fatalf("retained inspection: %+v", inspection)
	}
	foundExpired := false
	for _, f := range inspection.Findings {
		if f.Type == "obsolete_assertion_candidate" && len(f.Targets) == 1 && f.Targets[0].RecordID == fmt.Sprint(ids[4]) {
			foundExpired = true
		}
	}
	if !foundExpired {
		t.Fatal("expired assertion omitted")
	}
	full := duplicates(inspection)
	if full.Partial || full.RowsConsidered != 4 || full.RowsCompared != 4 || len(full.Findings) != 1 || len(full.Findings[0].Targets) != 3 || !full.DryRun || full.CanonicalWrites != 0 || full.ProposalWrites != 0 {
		t.Fatalf("full preview: %+v", full)
	}
	for _, version := range full.Findings[0].Targets {
		if version.RecordID != fmt.Sprint(ids[0]) && version.RecordID != fmt.Sprint(ids[1]) && version.RecordID != fmt.Sprint(ids[3]) {
			t.Fatal("hidden/ineligible source leaked", version)
		}
	}
	repeated := duplicates(preview(128, 32768))
	if repeated.Findings[0].ID != full.Findings[0].ID || repeated.Generation != full.Generation {
		t.Fatal("identical preview changed identity")
	}
	limited := preview(2, 32768)
	if !limited.Partial || limited.Unvisited != "remaining_retained_rows_or_content_unknown" || !limited.ResumeAvailable || limited.ResumeCursor == "" || len(limited.Findings) != 1 || len(limited.Findings[0].Targets) != 3 {
		t.Fatalf("bounded rows: %+v", limited)
	}
	tiny := preview(128, 1)
	if !tiny.Partial || tiny.RowsCompared != 0 || len(tiny.Findings) != 0 || tiny.Unvisited == "none_for_this_detector" {
		t.Fatalf("unvisited bytes reported clean: %+v", tiny)
	}
	if digest() != before {
		t.Fatal("hygiene preview changed canonical rows")
	}
	cursor := limited.ResumeCursor
	inspected := limited.RowsInspected
	for page := 0; page < 4 && cursor != ""; page++ {
		raw, e := client.Command(ctx, 73, "hygiene", json.RawMessage(fmt.Sprintf(`{"dry_run":true,"scope":{"type":"project","value":"hygiene-visible"},"max_rows":2,"max_content_bytes":32768,"cursor":%q}`, cursor)))
		var next hygienePreview
		if e != nil || json.Unmarshal(raw, &next) != nil || next.Status != "ok" {
			t.Fatalf("resume: %s %v", raw, e)
		}
		inspected += next.RowsInspected
		if next.ResumeCursor == cursor {
			t.Fatal("cursor did not advance")
		}
		cursor = next.ResumeCursor
		if cursor == "" && next.Partial {
			t.Fatal("complete bounded fixture still partial", next)
		}
	}
	if cursor != "" || inspected != 6 {
		t.Fatal("resume did not cover retained rows", inspected)
	}

	caller := bus.CommandContext{Authenticated: true, Principal: "model:hygiene", TransportIdentity: "cert:hygiene", ScopeKind: ScopeProject, ScopeID: "hygiene-visible"}
	queue := func() hygienePreview {
		t.Helper()
		result, status := invokeContextCommand(t, handler, 0, caller, "hygiene", `{"dry_run":false,"scope":{"type":"project","value":"hygiene-visible"},"max_rows":128,"max_content_bytes":32768}`)
		body, _ := json.Marshal(result)
		var out hygienePreview
		if status != bus.ModuleStatusOK || json.Unmarshal(body, &out) != nil || out.Status != "ok" || len(out.Findings) != 2 {
			t.Fatalf("hygiene admission %s %v", body, status)
		}
		return duplicates(out)
	}
	admitted := queue()
	if admitted.DryRun || admitted.CanonicalWrites != 0 || admitted.ProposalWrites != 2 || admitted.Findings[0].ProposalID == "" || admitted.JobID == "" || admitted.RunID == "" || !admitted.TelemetryWrites {
		t.Fatal(admitted)
	}
	repeatedAdmission := queue()
	if repeatedAdmission.JobID != admitted.JobID || repeatedAdmission.RunID != admitted.RunID || repeatedAdmission.ProposalWrites != 0 || repeatedAdmission.Findings[0].ProposalID != admitted.Findings[0].ProposalID {
		t.Fatal("retry duplicated proposal", repeatedAdmission)
	}
	exec("SET LOCAL ROLE aimee_memory_hygiene")
	var canMutate bool
	if err := tx.QueryRow(ctx, `SELECT has_table_privilege(current_user,'memories','UPDATE') OR has_table_privilege(current_user,'memories','DELETE') OR has_table_privilege(current_user,'learning_proposals','UPDATE') OR has_table_privilege(current_user,'learning_proposals','INSERT')`).Scan(&canMutate); err != nil || canMutate {
		t.Fatal("worker acquired canonical/review writes", err, canMutate)
	}
	exec("SET LOCAL ROLE NONE")
	exec(`UPDATE learning_proposals SET state='archived',archive_reason='review_rejected' WHERE id=$1`, admitted.Findings[0].ProposalID)
	exec("SET LOCAL ROLE aimee_store_runtime")
	rejectedRetry := queue()
	if rejectedRetry.ProposalWrites != 0 || rejectedRetry.Findings[0].ProposalID != admitted.Findings[0].ProposalID || rejectedRetry.Findings[0].ProposalState != "archived" {
		t.Fatal("rejection repeated", rejectedRetry)
	}
	if digest() != before {
		t.Fatal("hygiene admission or rejection mutated canonical state")
	}
	exec("SET LOCAL ROLE NONE")
	var expiredProposal int64
	if err := tx.QueryRow(ctx, `UPDATE learning_proposals SET expires_at='2000-01-01T00:00:00Z' WHERE sink='memory_hygiene' AND target_memory_id=$1 RETURNING id`, ids[4]).Scan(&expiredProposal); err != nil {
		t.Fatal(err)
	}
	exec("SAVEPOINT hygiene_expired_review")
	if _, err := tx.Exec(ctx, `UPDATE learning_proposals SET state='committed' WHERE id=$1`, expiredProposal); err == nil || !strings.Contains(err.Error(), "hygiene proposal expired") {
		t.Fatal("expired review accepted", err)
	}
	exec("ROLLBACK TO SAVEPOINT hygiene_expired_review; RELEASE SAVEPOINT hygiene_expired_review")
	exec("SET LOCAL ROLE aimee_store_runtime")
	if retried := queue(); retried.ProposalWrites != 0 || digest() != before {
		t.Fatal("expired proposal retry changed canonical state or duplicated admission", retried)
	}
	exec("UPDATE memories SET content='changed canonical evidence' WHERE id=$1", ids[0])
	changed := duplicates(preview(128, 32768))
	if len(changed.Findings) != 1 || changed.Findings[0].ID == full.Findings[0].ID || changed.Generation == full.Generation {
		t.Fatal("changed targets reused old preview identity")
	}

	renewed := queue()
	if renewed.Findings[0].ProposalID == admitted.Findings[0].ProposalID || renewed.ProposalWrites != 1 {
		t.Fatal("meaningful new evidence did not reconsider", renewed)
	}
	exec("UPDATE memories SET content='concurrent evidence change' WHERE id=$1", ids[1])
	exec("SET LOCAL ROLE NONE")
	exec("SAVEPOINT hygiene_stale_review")
	if _, err := tx.Exec(ctx, `UPDATE learning_proposals SET state='committed' WHERE id=$1`, renewed.Findings[0].ProposalID); err == nil {
		t.Fatal("stale review accepted")
	}
	exec("ROLLBACK TO SAVEPOINT hygiene_stale_review; RELEASE SAVEPOINT hygiene_stale_review")
	exec("SET LOCAL ROLE aimee_store_runtime")
	exec("UPDATE memories SET merged_into=$2 WHERE id=$1", ids[2], ids[6])
	exec("UPDATE memories SET kind='observation' WHERE id=$1", ids[0])
	exec("INSERT INTO memory_conflicts(memory_a,memory_b,detected_at) VALUES($1,$2,CURRENT_TIMESTAMP::text),($1,$3,CURRENT_TIMESTAMP::text)", ids[1], ids[3], ids[6])
	diagnostics := preview(128, 32768)
	types := map[string]int{}
	for _, finding := range diagnostics.Findings {
		types[finding.Type]++
		for _, target := range finding.Targets {
			if target.RecordID == fmt.Sprint(ids[6]) {
				t.Fatal("diagnostic leaked hidden target")
			}
		}
	}
	if types["broken_correction_chain"] != 1 || types["unreferenced_observation"] != 1 || types["possible_contradiction"] != 1 || types["obsolete_assertion_candidate"] != 1 {
		t.Fatal("diagnostic coverage", types)
	}

	for i := 0; i < 21; i++ {
		content := "large duplicate cluster"
		if i == 20 {
			content = "unique tail after truncated cluster"
		}
		exec(`INSERT INTO memories(tier,kind,key,content,scope_type,scope_value) VALUES('L2','fact',$1,$2,'project','hygiene-visible')`, fmt.Sprintf("hygiene-large-%d", i), content)
	}
	bounded := preview(128, 32768)
	if !bounded.Partial || bounded.ResumeAvailable || bounded.Unvisited != "duplicate_cluster_exceeds_16" {
		t.Fatal("truncated cluster reported complete", bounded)
	}
	cursor = ""
	sawClusterGap := false
	for page := 0; page < 12; page++ {
		raw, e := client.Command(ctx, 73, "hygiene", json.RawMessage(fmt.Sprintf(`{"dry_run":true,"scope":{"type":"project","value":"hygiene-visible"},"max_rows":4,"max_content_bytes":32768,"cursor":%q}`, cursor)))
		var next hygienePreview
		if e != nil || json.Unmarshal(raw, &next) != nil || next.Status != "ok" {
			t.Fatalf("cluster resume: %s %v", raw, e)
		}
		if next.Unvisited == "duplicate_cluster_exceeds_16" {
			sawClusterGap = true
		}
		if sawClusterGap && (!next.Partial || next.Unvisited != "duplicate_cluster_exceeds_16") {
			t.Fatal("later page lost coverage gap", next)
		}
		cursor = next.ResumeCursor
		if cursor == "" {
			break
		}
	}
	if !sawClusterGap || cursor != "" {
		t.Fatal("bounded cluster scan failed to terminate with its gap")
	}
}

func TestHygieneCursorBindsScopeAndSnapshotIdentity(t *testing.T) {
	scope := Scope{Type: ScopeProject, Value: "one"}
	raw := encodeHygieneCursor("owner", scope, "12", 42)
	got, id, err := decodeHygieneCursor(raw, scope)
	if err != nil || id != 42 || got.Owner != "owner" || got.Generation != "12" {
		t.Fatal(got, id, err)
	}
	if _, _, err = decodeHygieneCursor(raw, Scope{Type: ScopeProject, Value: "other"}); err == nil {
		t.Fatal("cursor widened scope")
	}
	if _, _, err = decodeHygieneCursor("forged", scope); err == nil {
		t.Fatal("malformed cursor accepted")
	}
}

func TestHygieneGovernedPostgres(t *testing.T) {
	url := os.Getenv("AIMEE_KB_STORE_REPLAY_URL")
	if url == "" {
		t.Skip("PostgreSQL replay fixture required")
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
	_, err = tx.Exec(ctx, `DO $$ BEGIN IF NOT EXISTS(SELECT 1 FROM pg_roles WHERE rolname='aimee_store_runtime') THEN CREATE ROLE aimee_store_runtime NOINHERIT NOBYPASSRLS; END IF; END $$; GRANT USAGE ON SCHEMA public TO aimee_store_runtime`)
	if err != nil {
		t.Fatal(err)
	}
	schema, err := os.ReadFile("../../../src/modules/kb/c/schema.sql")
	if err != nil {
		t.Fatal(err)
	}
	provision, e := os.ReadFile("../../../scripts/postgres-hygiene-role.sql")
	if e != nil {
		t.Fatal(e)
	}
	if _, e = tx.Exec(ctx, string(provision)); e != nil {
		t.Fatal(e)
	}
	for _, markers := range [][2]string{{"DO $memory_store_grants$", "END\n$memory_store_grants$;"}, {"DO $hygiene_worker$", "END $hygiene_worker$;"}} {
		first, last := strings.Index(string(schema), markers[0]), strings.Index(string(schema), markers[1])
		if first < 0 || last < first {
			t.Fatal("role grant block missing")
		}
		if _, err = tx.Exec(ctx, string(schema[first:last+len(markers[1])])); err != nil {
			t.Fatal(err)
		}
	}
	_, err = tx.Exec(ctx, `SET LOCAL ROLE aimee_store_runtime; SELECT set_config('aimee.principal','model:hygiene',true),set_config('aimee.authority','model',true),set_config('aimee.transport_identity','cert:hygiene',true)`)
	if err != nil {
		t.Fatal(err)
	}
	backend, err := NewPostgresDataStore(runtimeRoleDB{evalQueryer{tx}, t}, PlacementKB)
	if err != nil {
		t.Fatal(err)
	}
	exerciseHygienePreviewReplay(t, ctx, tx, NewHandler(nil, WithDataStore(PlacementKB, backend)))
}
