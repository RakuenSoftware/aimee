package memory

import (
	"context"
	"encoding/json"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestWikiRendering(t *testing.T) {
	now := time.Date(2026, 9, 18, 12, 30, 0, 0, time.FixedZone("offset", 3600))
	content := strings.Repeat("wiki 界 ", 1500) + "tail-marker"
	row := publicMemoryRecord{Key: "topic", Content: content, Tier: "L2", Kind: "concept", Confidence: 0.8, ProvenanceCategory: "user_explicit"}
	raw, err := buildWiki(now, func(kind string) ([]publicMemoryRecord, error) {
		if kind == "concept" || kind == "" {
			return []publicMemoryRecord{row}, nil
		}
		return nil, nil
	})
	if err != nil {
		t.Fatal(err)
	}
	var bundle wikiBundle
	if err = json.Unmarshal(raw, &bundle); err != nil || bundle.Status != "ok" || len(bundle.Files) != 7 {
		t.Fatal(err, string(raw))
	}
	want := "# Concepts\n\n_1 entry_\n\n## topic\n\n" + content + "\n\n- tier: L2 | kind: concept | confidence: 0.80 | provenance: user_explicit\n\n---\n\n"
	if bundle.Files[0].Text != want || bundle.Files[1].Text != "# Rules\n\n_0 entries_\n\n" {
		t.Fatal("page format/full text changed")
	}
	if bundle.Files[5].Name != "log.md" || !bundle.Files[5].PreserveExisting || bundle.Files[6].Name != "index.md" || !strings.Contains(bundle.Files[6].Text, "2026-09-18T11:30:00Z") || !strings.Contains(bundle.Files[6].Text, "[Facts](facts.md) — 1 entries") {
		t.Fatal(bundle.Files[5:])
	}
	empty, err := renderWikiPage("Facts", []publicMemoryRecord{{}}, 1024)
	if err != nil || !strings.Contains(empty, "## (no key)\n") || strings.Contains(empty, "provenance:") {
		t.Fatal(empty, err)
	}
}

func TestWikiFailures(t *testing.T) {
	failure := errors.New("store unavailable")
	calls := 0
	raw, err := buildWiki(time.Now(), func(kind string) ([]publicMemoryRecord, error) {
		calls++
		if kind == "preference" {
			return nil, failure
		}
		return nil, nil
	})
	if !errors.Is(err, failure) || raw != nil || calls != 3 {
		t.Fatal(err, len(raw), calls)
	}
	for _, content := range []string{strings.Repeat("a", wikiMaxBytes), strings.Repeat("<", wikiMaxBytes/4)} {
		raw, err = buildWiki(time.Now(), func(kind string) ([]publicMemoryRecord, error) {
			if kind == "concept" {
				return []publicMemoryRecord{{Content: content}}, nil
			}
			return nil, nil
		})
		if !errors.Is(err, errWikiTooLarge) || raw != nil {
			t.Fatal(err, len(raw))
		}
	}
}

func exerciseWikiReplay(t *testing.T, ctx context.Context, tx pgx.Tx, handler bus.ModuleHandler) {
	t.Helper()
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SAVEPOINT wiki_view`)
	defer func() { exec(`ROLLBACK TO SAVEPOINT wiki_view; RELEASE SAVEPOINT wiki_view`) }()
	exec(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	content := strings.Repeat("wiki content 界 ", 500) + "wiki-tail-marker"
	exec(`INSERT INTO memories(tier,kind,key,content,scope_type,scope_value,confidence,provenance_category)
 VALUES('L2','concept','wiki-local',$1,'project','wiki-project',0.8,'user_explicit'),
 ('L2','concept','wiki-private','private','project','wiki-private',0.1,'agent_message')`, content)
	exec(`INSERT INTO memories(tier,kind,key,content,scope_type,scope_value,confidence)
 SELECT 'L2','concept','wiki-global-'||n,'global','global','_global',0.9 FROM generate_series(1,505) n`)
	client := clientForHandler(t, handler)
	result := runPublicCommand(t, client, "list", `{"format":"wiki","scope_context":true,"project":"wiki-project"}`)
	if result["status"] != "ok" {
		t.Fatal(result)
	}
	encoded, _ := json.Marshal(result)
	var bundle wikiBundle
	if err := json.Unmarshal(encoded, &bundle); err != nil {
		t.Fatal(err)
	}
	if len(bundle.Files) != 7 {
		t.Fatal(len(bundle.Files))
	}
	page := bundle.Files[0].Text
	if !strings.HasPrefix(page, "# Concepts\n\n_500 entries_\n\n## wiki-local\n") || !strings.Contains(page, content) || strings.Contains(page, "wiki-private") || strings.Contains(page, "wiki-global-1\n") {
		t.Fatal("scope priority, full content or independent cap")
	}

	exec(`SAVEPOINT wiki_size`)
	exec(`INSERT INTO memories(tier,kind,key,content,scope_type,scope_value,confidence)
 SELECT 'L2','concept','wiki-big-'||n,repeat('content ',20000),'project','wiki-project',0.8 FROM generate_series(1,30) n`)
	result = runPublicCommand(t, client, "list", `{"format":"wiki","scope_context":true,"project":"wiki-project"}`)
	if result["kind"] != "capacity_exceeded" || result["files"] != nil {
		t.Fatal(result)
	}
	exec(`ROLLBACK TO SAVEPOINT wiki_size; RELEASE SAVEPOINT wiki_size`)
	// Missing storage privileges must fail the whole bundle, not publish empty pages.
	exec(`RESET ROLE; REVOKE SELECT ON memory_summaries FROM aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`)
	result = runPublicCommand(t, client, "list", `{"format":"wiki","scope_context":true,"project":"wiki-project"}`)
	if result["status"] != "error" || result["files"] != nil {
		t.Fatal(result)
	}
}
