package memory

import (
	"context"
	"encoding/json"
	"fmt"
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
	full := preview(128, 32768)
	if full.Partial || full.RowsConsidered != 4 || full.RowsCompared != 4 || len(full.Findings) != 1 || len(full.Findings[0].Targets) != 3 || !full.DryRun || full.CanonicalWrites != 0 || full.ProposalWrites != 0 {
		t.Fatalf("full preview: %+v", full)
	}
	for _, version := range full.Findings[0].Targets {
		if version.RecordID != fmt.Sprint(ids[0]) && version.RecordID != fmt.Sprint(ids[1]) && version.RecordID != fmt.Sprint(ids[3]) {
			t.Fatal("hidden/ineligible source leaked", version)
		}
	}
	repeated := preview(128, 32768)
	if repeated.Findings[0].ID != full.Findings[0].ID || repeated.Generation != full.Generation {
		t.Fatal("identical preview changed identity")
	}
	limited := preview(2, 32768)
	if !limited.Partial || limited.Unvisited != "remaining_eligible_content_unknown" || limited.ResumeAvailable || len(limited.Findings) != 1 || len(limited.Findings[0].Targets) != 2 {
		t.Fatalf("bounded rows: %+v", limited)
	}
	tiny := preview(128, 1)
	if !tiny.Partial || tiny.RowsCompared != 0 || len(tiny.Findings) != 0 || tiny.Unvisited == "none_for_this_detector" {
		t.Fatalf("unvisited bytes reported clean: %+v", tiny)
	}
	if digest() != before {
		t.Fatal("hygiene preview changed canonical rows")
	}
	exec("UPDATE memories SET content='changed canonical evidence' WHERE id=$1", ids[0])
	changed := preview(128, 32768)
	if len(changed.Findings) != 1 || changed.Findings[0].ID == full.Findings[0].ID || changed.Generation == full.Generation {
		t.Fatal("changed targets reused old preview identity")
	}
}
