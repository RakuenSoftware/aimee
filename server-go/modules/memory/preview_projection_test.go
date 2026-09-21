package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func previewTestRows() []ingressMemoryPreview {
	parent := MemoryRecordVersion{SchemaVersion: 1, OwnerID: "00000000-0000-4000-8000-000000000001", RecordID: "9007199254745001", RecordRevision: "2"}
	summary := parent
	summary.RecordID = "9007199254745003"
	summary.RecordRevision = "4"
	other := parent
	other.RecordID = "9007199254745002"
	return []ingressMemoryPreview{
		{ID: parent.RecordID, Key: "summary", Headline: "Keep the exact 界 headline", Content: "unused content", Score: .12349999999999998, ScoreText: "0.123", Source: &typedSourceVersion{Kind: "memory_summary", Version: summary, MemoryParents: []MemoryRecordVersion{parent}, MemoryParentState: "observed"}},
		{ID: other.RecordID, Key: "fallback", Content: "canonical content", Score: .75, ScoreText: "0.750", Source: &typedSourceVersion{Kind: "memory_record", Version: other, MemoryParentState: "observed"}},
	}
}

func TestPreviewProjectionIntegrityAndRetention(t *testing.T) {
	rows := previewTestRows()
	projection, err := newPreviewProjection(rows)
	if err != nil {
		t.Fatal(err)
	}
	raw, _ := json.Marshal(projection)
	request := ingressAssemblyRequest{Budget: 4000, Memories: rows, MemoryProjection: raw}
	full, err := ingressAssemble(request)
	if err != nil || !strings.Contains(full["block"].(string), "score=0.123") {
		t.Fatal(full, err)
	}
	// cJSON may round this raw float across a three-decimal boundary. The owner
	// explicitly commits its rendered score, which survives the opaque relay.
	rows[0].Score = .1235
	rows[0].Content = "changed unused content"
	rows[0].Preview = "unused compatibility preview"
	if !projection.valid(rows) {
		t.Fatal("unused transport fields changed commitment")
	}
	for _, field := range []string{"headline", "key", "tier", "score", "revision", "parent", "identity", "scope-policy"} {
		t.Run(field, func(t *testing.T) {
			changed := previewTestRows()
			switch field {
			case "headline":
				changed[0].Headline = "altered"
			case "key":
				changed[0].Key = "altered"
			case "tier":
				changed[0].Tier = "L3"
			case "score":
				changed[0].ScoreText = "0.124"
			case "revision":
				changed[0].Source.Version.RecordRevision = "5"
			case "parent":
				changed[0].Source.MemoryParents[0].RecordRevision = "3"
			case "identity":
				changed[0].ID = "9007199254745009"
			case "scope-policy":
				changed[0].Source.ReadPolicy = &sourceReadPolicy{Historical: true}
			}
			if projection.valid(changed) {
				t.Fatal("changed rendered/source identity accepted")
			}
			request.Memories = changed
			if _, err := ingressAssemble(request); err == nil {
				t.Fatal("invalid commitment reached assembly")
			}
		})
	}
	request.Memories = rows
	request.MemoryProjection = nil
	if _, err := ingressAssemble(request); err == nil {
		t.Fatal("missing source commitment accepted")
	}
	request.MemoryProjection = json.RawMessage(`null`)
	if _, err := ingressAssemble(request); err == nil {
		t.Fatal("null commitment accepted")
	}
	request.MemoryProjection = raw
	retainedSome := false
	for budget := 384; budget <= 1500; budget++ {
		request.Budget = budget
		result, err := ingressAssemble(request)
		if err != nil {
			t.Fatal(err)
		}
		retained := result["memory_projection"].(map[string]any)["retained_items"].([]typedProjectionRef)
		refs := result["retained_memory_source_refs"].([]ingressProjectionEvidenceRef)
		ids := result["retained_memory_ids"].([]string)
		if len(refs) != len(retained) || len(ids) != len(refs) {
			t.Fatal("omitted source kept", result)
		}
		state := &sourceReleaseState{}
		ticket, err := state.prepare(sourceReleaseArgs(map[string]any{"request_id": "preview", "project": "app"}), result)
		if err != nil {
			t.Fatal(err)
		}
		if len(retained) == 0 {
			if ticket != "" {
				t.Fatal("empty projection created release")
			}
			continue
		}
		if !releaseTokenValid(ticket) {
			t.Fatal("missing preview release handle")
		}
		var stored []typedProjectionRef
		if json.Unmarshal(state.entries[ticket].sources, &stored) != nil || releaseDigest(stored) != releaseDigest(retained) {
			t.Fatal("handoff sources differ from retained rows")
		}
		if len(retained) == 1 {
			retainedSome = true
		}
	}
	if !retainedSome {
		t.Fatal("partial packing not exercised")
	}
}

func exercisePreviewSourceReplay(t *testing.T, ctx context.Context, tx pgx.Tx, backend *postgresDataStore, handler bus.ModuleHandler) {
	t.Helper()
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec("SAVEPOINT preview_sources")
	defer func() { exec("ROLLBACK TO SAVEPOINT preview_sources; RELEASE SAVEPOINT preview_sources") }()
	const project = "preview-version-project"
	exec(`RESET ROLE; SELECT set_config('aimee.memory_scope_all','1',true)`)
	exec(`INSERT INTO memories(id,tier,kind,key,content,scope_type,scope_value) VALUES
 (9007199254745001,'L2','fact','preview-version-one','canonical preview content','project',$1),
 (9007199254745002,'L2','fact','preview-version-two','fallback preview content','project',$1);
 `, project)
	exec(`INSERT INTO memory_summaries(id,memory_id,scope,summary) VALUES(9007199254745003,9007199254745001,'headline','independently revised headline')`)
	exec(`SELECT set_config('aimee.memory_scope_all','0',true),set_config('aimee.memory_project',$1,true),set_config('aimee.memory_scope_type','',true),set_config('aimee.memory_scope_value','',true)`, project)
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	diagnostics := []Diagnostic{{Memory: Record{ID: 9007199254745001}, Parts: DiagnosticParts{Total: .12349999999999998}}, {Memory: Record{ID: 9007199254745002}, Parts: DiagnosticParts{Total: .75}}}
	rows, p, err := backend.ingressMemoryPreviews(ctx, diagnostics, Scope{Type: "project", Value: project})
	if err != nil || len(rows) != 2 || !p.valid(rows) || rows[0].Source.Kind != "memory_summary" || rows[1].Source.Kind != "memory_record" || rows[0].ScoreText != "0.123" {
		t.Fatal(rows, p, err)
	}
	request := &sourceRevalidation{SchemaVersion: 1, CheckID: strings.Repeat("b", 32), Sources: p.Retained}
	check := func(want bool) {
		t.Helper()
		ok, err := backend.revalidateSources(ctx, request, Scope{Type: "project", Value: project})
		if err != nil || ok != want {
			t.Fatal("preview revalidation", ok, want, err)
		}
	}
	check(true)
	// No-op maintenance must preserve both versions and rendered commitments.
	exec(`UPDATE memory_summaries SET summary=summary,record_revision=999 WHERE id=9007199254745003`)
	check(true)
	for _, change := range []string{
		`UPDATE memory_summaries SET summary='changed independently' WHERE id=9007199254745003`,
		`UPDATE memory_summaries SET memory_id=9007199254745002 WHERE id=9007199254745003`,
		`DELETE FROM memory_summaries WHERE id=9007199254745003`,
		`UPDATE memories SET content='changed parent' WHERE id=9007199254745001`,
		`UPDATE memories SET content='changed fallback' WHERE id=9007199254745002`,
		`UPDATE memories SET activation_suppressed=1 WHERE id=9007199254745001`,
		`UPDATE memories SET valid_until=CURRENT_TIMESTAMP::text WHERE id=9007199254745002`,
		`SELECT set_config('aimee.memory_project','other-project',true)`,
	} {
		exec("SAVEPOINT preview_change")
		exec(change)
		check(false)
		exec("ROLLBACK TO SAVEPOINT preview_change; RELEASE SAVEPOINT preview_change")
		check(true)
	}
	if _, _, err := backend.ingressMemoryPreviews(ctx, diagnostics, Scope{Type: "project", Value: "other-project"}); err == nil {
		t.Fatal("rehydration widened explicit scope")
	}
	exec(`UPDATE memory_summaries SET summary='new headline' WHERE id=9007199254745003`)
	refreshed, newProjection, err := backend.ingressMemoryPreviews(ctx, diagnostics, Scope{})
	if err != nil || !newProjection.valid(refreshed) || newProjection.SelectionDigest == p.SelectionDigest || refreshed[0].Source.Version.RecordRevision != "2" {
		t.Fatal(refreshed, err)
	}
	request.Sources = newProjection.Retained
	check(true)
	result, status := invokeContextCommand(t, handler, 0, bus.CommandContext{}, "diagnose_scoped", fmt.Sprintf(`{"query":"preview-version","scope_context":true,"project":%q,"format":"ingress","limit":5}`, project))
	if status != bus.ModuleStatusOK || result["status"] != "ok" {
		t.Fatal(status, result)
	}
	encoded, _ := json.Marshal(result)
	var public struct {
		Memories   []ingressMemoryPreview `json:"memories"`
		Projection *previewProjection     `json:"memory_projection"`
	}
	if json.Unmarshal(encoded, &public) != nil || len(public.Memories) != 2 || !public.Projection.valid(public.Memories) {
		t.Fatal("public preview commitment", string(encoded))
	}
}
