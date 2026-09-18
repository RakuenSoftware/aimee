package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"
	"testing"
	"unicode/utf8"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestConventionText(t *testing.T) {
	for _, tc := range []struct{ in, want string }{
		{"", ""}, {" # ** - \t\nKeep changes scoped. Next sentence.", "Keep changes scoped."},
		{"First line\nsecond line\n\nnext paragraph", "First line\nsecond line"},
		{"Use v1.2 for builds.", "Use v1.2 for builds."}, {"Final line\n", "Final line"},
	} {
		if got := conventionSentence(tc.in); got != tc.want {
			t.Fatal(tc, got)
		}
	}
	sentence := conventionSentence(strings.Repeat("界", 100))
	if !utf8.ValidString(sentence) || len(sentence) != 198 {
		t.Fatal(len(sentence), sentence)
	}
	source := conventionSource{project: "project", path: "docs/AGENTS.md", heading: "Build: #test/run"}
	if got := conventionKey(source, "unused"); got != "convention_project_AGENTS.md_Build___test_run" {
		t.Fatal(got)
	}
	source.heading = ""
	if got := conventionKey(source, "Keep changes scoped."); got != "convention_project_AGENTS.md_Keep_changes_scoped." {
		t.Fatal(got)
	}
	source.project = strings.Repeat("界", 50)
	source.heading = strings.Repeat("界", 50)
	if key := conventionKey(source, sentence); !utf8.ValidString(key) || len(key) > 299 {
		t.Fatal(key)
	}
}

func TestConventionHostBoundary(t *testing.T) {
	for _, p := range []Placement{PlacementKB, PlacementServer} {
		handler := NewHandler(nil, WithDataStore(p, nil))
		frame, _ := bus.EncodeCommand("runtime", json.RawMessage(`{"operation":"convention-extract"}`))
		if _, status := handler(bus.ModuleInvocation{StageID: StageCommand, PrincipalRef: 200}, frame); status != bus.ModuleStatusInvalidRequest {
			t.Fatal(p, status)
		}
		if _, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame); status != bus.ModuleStatusCapabilityAbsent {
			t.Fatal(p, status)
		}
	}
}

func exerciseConventionReplay(t *testing.T, ctx context.Context, tx pgx.Tx, handler bus.ModuleHandler) {
	t.Helper()
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`RESET ROLE;
 INSERT INTO projects(name,root,scanned_at,current_generation,lifecycle_state) VALUES
 ('convention-fixture','/convention','now',2,'current'),('convention-private','/private','now',2,'current'),('convention-retired','/retired','now',2,'detached')`)
	insert := func(project, path, heading, content, kind string, generation int) {
		t.Helper()
		exec(`INSERT INTO kb_documents(project,file_path,file_hash,chunk_index,heading_path,content,generation,doc_kind) VALUES($1,$2,'fixture',0,$3,$4,$5,$6)`, project, path, heading, content, generation, kind)
	}
	paths := []string{"CONTRIBUTING.md", "contributing.RST", "AGENTS.md", "STYLE.md", "styleguide.md", "CODING.md", "CODE_STYLE.md", "CODING_STANDARDS.md", ".aimee-rules", "aimee-rules.md", ".aimee/rules.md", ".aimee/context.md", "docs/ADR/0001.md", "adr-build.md"}
	for i, path := range paths {
		insert("convention-fixture", path, fmt.Sprintf("Rule %d", i), "Keep changes scoped. Next sentence.", "", 2)
	}
	insert("convention-fixture", "docs/adr/0001.pdf", "PDF", "Restricted PDF content must remain outside conventions.", "pdf", 2)
	insert("convention-fixture", "docs/STYLE.md", "Old", "Old generation must stay out of conventions.", "", 1)
	insert("convention-fixture", "docs/AGENTS.md", "Tiny", "Short.", "", 2)
	insert("convention-private", "AGENTS.md", "Private", "Private project must stay private.", "", 2)
	insert("convention-retired", "AGENTS.md", "Retired", "Retired project must stay out of conventions.", "", 2)
	// Ineligible names must be filtered before the bounded candidate window.
	exec(`INSERT INTO kb_documents(project,file_path,file_hash,chunk_index,content,generation) SELECT 'convention-fixture','000-unrelated-'||i,'fixture',0,'Not a convention source.',2 FROM generate_series(1,501) i`)
	firstKey := conventionKey(conventionSource{project: "convention-fixture", path: paths[0], heading: "Rule 0"}, "")
	exec(`INSERT INTO memories(tier,kind,key,content,scope_type,scope_value) VALUES('L4','fact',$1,'Operator-approved content','project','convention-fixture')`, derivedNormalize(firstKey))
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	run := func() (map[string]any, bus.ModuleStatus) {
		t.Helper()
		frame, _ := bus.EncodeCommand("runtime", json.RawMessage(`{"operation":"convention-extract","scope_context":true,"project":"convention-fixture"}`))
		out, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame)
		if status != bus.ModuleStatusOK {
			return nil, status
		}
		body, err := bus.DecodeCommandResult(out)
		if err != nil {
			t.Fatal(err)
		}
		var result map[string]any
		if json.Unmarshal(body, &result) != nil {
			t.Fatal(string(body))
		}
		return result, status
	}
	got, status := run()
	if status != bus.ModuleStatusOK || got["emitted"] != float64(len(paths)-1) {
		t.Fatal(got, status)
	}
	got, status = run()
	if status != bus.ModuleStatusOK || got["emitted"] != float64(0) {
		t.Fatal("non-idempotent", got, status)
	}
	var count int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memories m JOIN memory_fact_actors a ON a.memory_id=m.id JOIN kb_async_jobs j ON j.document_id=m.id AND j.kind='memory_facts' WHERE m.scope_type='project' AND m.scope_value='convention-fixture' AND m.tier='L3' AND m.kind='fact' AND m.confidence=.6 AND m.content='Keep changes scoped.' AND a.actor_role='model' AND j.status='pending'`).Scan(&count); err != nil || count != len(paths)-1 {
		t.Fatal(count, err)
	}
	// A project-scoped run has no authority to manufacture records in other projects.
	exec(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memories WHERE scope_value IN ('convention-private','convention-retired')`).Scan(&count); err != nil || count != 0 {
		t.Fatal(count, err)
	}
	var content string
	if err := tx.QueryRow(ctx, `SELECT content FROM memories WHERE key=$1 AND scope_value='convention-fixture'`, derivedNormalize(firstKey)).Scan(&content); err != nil || content != "Operator-approved content" {
		t.Fatal(content, err)
	}
	// A later-row failure must roll back earlier successful candidates in the same run.
	exec(`RESET ROLE`)
	insert("convention-fixture", "000/AGENTS.md", "New first", "First new candidate must roll back.", "", 2)
	insert("convention-fixture", "001/AGENTS.md", "Failure", "Later candidate forces transaction rollback.", "", 2)
	exec(`ALTER TABLE memories ADD CONSTRAINT convention_failure CHECK(key NOT LIKE '%_Failure') NOT VALID; SET LOCAL ROLE aimee_store_runtime`)
	if _, status := run(); status != bus.ModuleStatusInternal {
		t.Fatal("failed batch acknowledged", status)
	}
	exec(`RESET ROLE; ALTER TABLE memories DROP CONSTRAINT convention_failure; SET LOCAL ROLE aimee_store_runtime; SELECT set_config('aimee.memory_scope_all','1',true)`)
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memories WHERE content IN ('First new candidate must roll back.','Later candidate forces transaction rollback.')`).Scan(&count); err != nil || count != 0 {
		t.Fatal("partial convention batch", count, err)
	}
	got, status = run()
	if status != bus.ModuleStatusOK || got["emitted"] != float64(2) {
		t.Fatal("retry lost candidates", got, status)
	}
}
