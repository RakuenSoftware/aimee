package memory

import (
	"context"
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"os"
	"strings"
	"testing"
	"time"

	store "github.com/JBailes/aimee/server-go/db"
	"github.com/jackc/pgx/v5"
)

type codeTestStore struct{ runtimeRoleDB }

func (d codeTestStore) CurrentSchemaVersion(context.Context, string) (int64, string, error) {
	return 0, "", nil
}
func (d codeTestStore) Migrate(ctx context.Context, m store.MigrationRequest) error {
	for _, sql := range m.Statements {
		if _, err := d.Exec(ctx, sql); err != nil {
			return err
		}
	}
	return nil
}
func codeFixture(t *testing.T) (context.Context, *postgresDataStore, codeTestStore) {
	t.Helper()
	dsn := os.Getenv("AIMEE_MEMORY_EVAL_URL")
	if dsn == "" {
		if os.Getenv("AIMEE_MEMORY_EVAL_REQUIRED") == "1" {
			t.Fatal("AIMEE_MEMORY_EVAL_URL required")
		}
		t.Skip("requires disposable PostgreSQL with vector")
	}
	ctx := context.Background()
	conn, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close(ctx) })
	tx, err := conn.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { tx.Rollback(ctx) })
	if _, err = tx.Exec(ctx, `CREATE EXTENSION IF NOT EXISTS vector; CREATE SCHEMA code_fixture; SET LOCAL search_path TO code_fixture,public`); err != nil {
		t.Fatal(err)
	}
	db := codeTestStore{runtimeRoleDB{evalQueryer{tx}, t}}
	data, err := NewPostgresDataStore(db, PlacementServer)
	if err != nil {
		t.Fatal(err)
	}
	return ctx, data.(*postgresDataStore), db
}
func TestPrivateCodeIndexPublicationAndFusion(t *testing.T) {
	t.Setenv("AIMEE_GRAPH_FUSION", "on")
	ctx, s, db := codeFixture(t)
	call := func(req CodeIndexRequest) json.RawMessage {
		t.Helper()
		raw, err := s.CodeIndex(ctx, req)
		if err != nil {
			t.Fatalf("%+v: %v", req, err)
		}
		return raw
	}
	scan := func(project, phase, scan string, files []CodeFile, count int) CodeIndexRequest {
		return CodeIndexRequest{Route: "/v1/code/scan", Project: project, Root: "/fixture/" + project, Phase: phase, ScanID: scan, Files: files, ExpectedFiles: count}
	}
	files := []CodeFile{
		{Path: "main.c", Content: "// quasar entry\nvoid entry(){ helper(); }", Definitions: []CodeDefinition{{Name: "entry", Kind: "function", Line: 2, LineEnd: 2}}, Calls: []CodeCall{{Caller: "entry", Callee: "helper", Line: 2}}},
		{Path: "helper.c", Content: "void helper(){}", Definitions: []CodeDefinition{{Name: "helper", Kind: "function", Line: 1, LineEnd: 1}}},
	}
	call(scan("alpha", "begin", "one", nil, 0))
	call(scan("alpha", "stage", "one", files, 0))
	raw := call(CodeIndexRequest{Route: "/v1/code/search?project=alpha&query=quasar"})
	var reply struct {
		Hits []struct {
			Project, Source string
			Path            string `json:"file_path"`
		}
	}
	json.Unmarshal(raw, &reply)
	if len(reply.Hits) != 0 {
		t.Fatal("staged files became searchable")
	}
	if _, err := s.CodeIndex(ctx, scan("alpha", "seal", "one", nil, 3)); err == nil {
		t.Fatal("incomplete manifest published")
	}
	call(scan("alpha", "seal", "one", nil, 2))
	// A second project's identically named helper must not join the first graph.
	call(scan("beta", "", "", files[1:], 0))
	raw = call(CodeIndexRequest{Route: "/v1/code/hybrid?project=alpha&query=quasar"})
	json.Unmarshal(raw, &reply)
	if len(reply.Hits) != 2 || reply.Hits[1].Path != "helper.c" || reply.Hits[1].Source != "graph" || reply.Hits[1].Project != "alpha" {
		t.Fatalf("default graph expansion: %s", raw)
	}
	raw = call(CodeIndexRequest{Route: "/v1/code/find?project=alpha&scope=all&identifier=helper"})
	json.Unmarshal(raw, &reply)
	if len(reply.Hits) != 2 {
		t.Fatalf("explicit all-project search remained confined: %s", raw)
	}
	raw = call(CodeIndexRequest{Route: "/v1/code/context?project=alpha&query=quasar"})
	var contextReply struct {
		Results []struct {
			Generation int
			Span       map[string]any
		}
	}
	if err := json.Unmarshal(raw, &contextReply); err != nil || len(contextReply.Results) == 0 || contextReply.Results[0].Generation != 1 || contextReply.Results[0].Span["line_start"] != float64(1) {
		t.Fatalf("investigation evidence lacks source span: %s", raw)
	}
	// Reopen against the persisted code with a different instance setting.
	t.Setenv("AIMEE_GRAPH_FUSION", "off")
	offData, err := NewPostgresDataStore(db, PlacementServer)
	if err != nil {
		t.Fatal(err)
	}
	off := offData.(*postgresDataStore)
	raw, err = off.CodeIndex(ctx, CodeIndexRequest{Route: "/v1/code/hybrid?project=alpha&query=quasar"})
	if err != nil {
		t.Fatal(err)
	}
	json.Unmarshal(raw, &reply)
	if len(reply.Hits) != 1 || reply.Hits[0].Path != "main.c" {
		t.Fatalf("off instance: %s", raw)
	}
	if !s.graphFusionEnabled() {
		t.Fatal("another instance changed the existing instance")
	}
	call(scan("alpha", "begin", "two", nil, 0))
	call(scan("alpha", "stage", "two", files[:1], 0))
	call(scan("alpha", "seal", "two", nil, 1))
	raw = call(CodeIndexRequest{Route: "/v1/code/find?project=alpha&identifier=helper"})
	json.Unmarshal(raw, &reply)
	if len(reply.Hits) != 0 {
		t.Fatalf("removed file survived publication: %s", raw)
	}
	if _, err := s.CodeIndex(ctx, scan("alpha", "stage", "one", files, 0)); err == nil {
		t.Fatal("stale scan was accepted")
	}
	for _, p := range []string{"../private", "/etc/passwd", ".git/config", "nested/.env", "a/../b", "bad\\file"} {
		if _, err := s.CodeIndex(ctx, scan("alpha", "", "", []CodeFile{{Path: p, Content: "private"}}, 0)); err == nil {
			t.Fatalf("unsafe path %q accepted", p)
		}
	}
	call(CodeIndexRequest{Route: "/v1/code/project/delete", Project: "alpha"})
	raw = call(CodeIndexRequest{Route: "/v1/code/search?project=alpha&query=quasar"})
	json.Unmarshal(raw, &reply)
	if len(reply.Hits) != 0 {
		t.Fatalf("deleted project still searchable: %s", raw)
	}
}

func TestKBGraphFusionUsesInstancePolicyAndVisibility(t *testing.T) {
	t.Setenv("AIMEE_GRAPH_FUSION", "on")
	ctx, _, db := codeFixture(t)
	_, err := db.Exec(ctx, `CREATE FUNCTION pg_now_text() RETURNS text LANGUAGE sql AS 'SELECT to_char(now(), ''YYYY-MM-DD HH24:MI:SS'')';
CREATE TABLE memories(id bigint PRIMARY KEY,scope_type text,scope_value text,tier text,kind text,key text,content text,
 confidence double precision,lifecycle_state text,activation_suppressed int DEFAULT 0,use_cases text DEFAULT '',updated_at timestamptz DEFAULT now());
CREATE TABLE memory_entities(memory_id bigint,entity text);
CREATE TABLE entity_edges(id bigint,source text,target text,confidence_class text,utility_score double precision,
 suppressed int DEFAULT 0,superseded_at text DEFAULT '',invalidated_at text DEFAULT '',lifecycle_state text DEFAULT 'persistent',valid_from text DEFAULT '',valid_until text DEFAULT '');
INSERT INTO memories(id,scope_type,scope_value,tier,kind,key,content,confidence,lifecycle_state) VALUES
(1,'project','alpha','L2','fact','quasar','entry note',1,'active'),
(2,'project','alpha','L2','fact','helper','indirect note',1,'active'),
(3,'project','beta','L2','fact','private','other project',1,'active'),
(4,'project','alpha','L2','fact','retired','old note',1,'retired');
INSERT INTO memory_entities VALUES(1,'entry'),(2,'helper'),(3,'helper'),(4,'helper');
INSERT INTO entity_edges(id,source,target,confidence_class,utility_score) VALUES(1,'entry','helper','A',0);`)
	if err != nil {
		t.Fatal(err)
	}
	data, err := NewPostgresDataStore(db, PlacementKB)
	if err != nil {
		t.Fatal(err)
	}
	s := data.(*postgresDataStore)
	req := DataRequest{Scope: Scope{Type: ScopeProject, Value: "alpha"}, Query: "quasar", Limit: 10}
	on, err := s.Search(ctx, req.Scope, req.Query, "", "", 10)
	if err != nil {
		t.Fatal(err)
	}
	if len(on) != 2 || on[0].ID != 1 || on[1].ID != 2 {
		t.Fatalf("KB graph or visibility: %+v", on)
	}
	t.Setenv("AIMEE_GRAPH_FUSION", "off")
	offData, err := NewPostgresDataStore(db, PlacementKB)
	if err != nil {
		t.Fatal(err)
	}
	off, err := offData.Search(ctx, req.Scope, req.Query, "", "", 10)
	if err != nil {
		t.Fatal(err)
	}
	if len(off) != 1 || off[0].ID != 1 {
		t.Fatalf("off KB still expanded graph: %+v", off)
	}
	if _, err := db.Exec(ctx, `UPDATE entity_edges SET suppressed=1`); err != nil {
		t.Fatal(err)
	}
	suppressed, err := s.Search(ctx, req.Scope, req.Query, "", "", 10)
	if err != nil {
		t.Fatal(err)
	}
	if len(suppressed) != 1 {
		t.Fatalf("suppressed edge expanded: %+v", suppressed)
	}
}

func TestCodeVectorsRejectStaleContentAndModel(t *testing.T) {
	t.Setenv("AIMEE_GRAPH_FUSION", "off")
	ctx, s, db := codeFixture(t)
	ingest := func(content string) {
		t.Helper()
		_, err := s.CodeIndex(ctx, CodeIndexRequest{Route: "/v1/code/scan", Project: "alpha", Root: "/fixture/alpha", Files: []CodeFile{{Path: "worker.c", Content: content}}})
		if err != nil {
			t.Fatal(err)
		}
	}
	ingest("void worker() {}")
	executor := &vectorTestEgress{serving: "model-a"}
	p := &personalVectors{endpoint: "https://fixture-embedder", executor: executor, db: db, code: s}
	s.personal = p
	if err := p.indexCodeBatch(ctx); err != nil {
		t.Fatal(err)
	}
	search := func(want int) {
		t.Helper()
		raw, err := s.searchCode(ctx, "alpha", "semantic question", 10)
		if err != nil {
			t.Fatal(err)
		}
		var result struct{ Hits []struct{ Source string } }
		if err = json.Unmarshal(raw, &result); err != nil {
			t.Fatal(err)
		}
		if len(result.Hits) != want {
			t.Fatalf("semantic results: %s", raw)
		}
		if want > 0 && result.Hits[0].Source != "vector" {
			t.Fatalf("wrong source: %s", raw)
		}
	}
	search(1)
	ingest("void renamed() {}")
	search(0)
	if err := p.indexCodeBatch(ctx); err != nil {
		t.Fatal(err)
	}
	search(1)
	executor.serving = "model-b"
	search(0)
	// A restarted instance resumes pending detached-file embeddings before a query.
	s.code = codeIndexState{}
	if err := p.indexCodeBatch(ctx); err != nil {
		t.Fatal(err)
	}
	search(1)
	// A simultaneous edit between embedding and publication must discard the vector.
	ingest("void third() {}")
	executor.beforeEmbed = func() { executor.beforeEmbed = nil; ingest("void fourth() {}") }
	if err := p.indexCodeBatch(ctx); err != nil {
		t.Fatal(err)
	}
	search(0)
}

func TestPrivateCodeIndexLargeGraphAndMissingFile(t *testing.T) {
	t.Setenv("AIMEE_GRAPH_FUSION", "on")
	ctx, s, db := codeFixture(t)
	if err := s.ensureCodeIndex(ctx); err != nil {
		t.Fatal(err)
	}
	// A repository-sized graph: each file has several definitions and calls.
	// A file-first join expands millions of unrelated pairs before matching names.
	_, err := db.Exec(ctx, `INSERT INTO user_code_projects(name,root,generation) VALUES('large','/fixture/large',1);
 INSERT INTO user_code_files(project,path,content,definitions,calls,fingerprint)
 SELECT 'large', 'file-'||i||'.c', CASE WHEN i=1 THEN 'quasar' ELSE 'source' END,
 (SELECT jsonb_agg(jsonb_build_object('name','symbol-'||i||'-'||d,'line',d,'line_end',d,'kind','function')) FROM generate_series(1,20) d),
 (SELECT jsonb_agg(jsonb_build_object('callee','symbol-'||(i%1500+1)||'-'||c,'caller','entry','line',c)) FROM generate_series(1,20) c),
 md5(i::text) FROM generate_series(1,1500) i`)
	if err != nil {
		t.Fatal(err)
	}
	bounded, cancel := context.WithTimeout(ctx, 5*time.Second)
	defer cancel()
	raw, err := s.CodeIndex(bounded, CodeIndexRequest{Route: "/v1/code/hybrid?project=large&query=quasar"})
	if err != nil {
		t.Fatal(err)
	}
	var reply struct {
		Hits []struct {
			Path string `json:"file_path"`
		}
	}
	if err := json.Unmarshal(raw, &reply); err != nil || len(reply.Hits) != 3 {
		t.Fatalf("graph neighbors missing: %s (%v)", raw, err)
	}
	for _, route := range []string{"/v1/code/structure?project=large&file_path=missing.c", "/v1/code/structure?project=missing&file_path=missing.c"} {
		raw, err = s.CodeIndex(ctx, CodeIndexRequest{Route: route})
		if err != nil {
			t.Fatal(err)
		}
		var structure struct {
			Definitions []CodeDefinition `json:"definitions"`
		}
		if err := json.Unmarshal(raw, &structure); err != nil || structure.Definitions == nil || len(structure.Definitions) != 0 {
			t.Fatalf("missing file: %s (%v)", raw, err)
		}
	}
}

func TestPrivateCodeMissingBlastRadius(t *testing.T) {
	ctx, s, _ := codeFixture(t)
	raw, err := s.CodeIndex(ctx, CodeIndexRequest{Route: "/v1/code/blast-radius?project=missing&file_path=missing.c"})
	if err != nil {
		t.Fatal(err)
	}
	var reply struct {
		Resolved   bool
		Dependents []string
	}
	if err := json.Unmarshal(raw, &reply); err != nil || reply.Resolved || reply.Dependents == nil {
		t.Fatalf("missing file: %s (%v)", raw, err)
	}
}

func TestPrivateCodePublishedSpan(t *testing.T) {
	ctx, s, _ := codeFixture(t)
	_, err := s.CodeIndex(ctx, CodeIndexRequest{Route: "/v1/code/scan", Project: "detached", Root: "/unreadable/client/path", Files: []CodeFile{{Path: "main.c", Content: "one\ntwo\nthree\n"}}})
	if err != nil {
		t.Fatal(err)
	}
	raw, err := s.CodeIndex(ctx, CodeIndexRequest{Route: "/v1/code/span", Project: "detached", FilePath: "main.c", LineStart: 2, LineEnd: 100, MaxLines: 1})
	if err != nil {
		t.Fatal(err)
	}
	var span struct {
		Content   string
		Truncated bool
		LineCount int    `json:"line_count"`
		Version   string `json:"source_version"`
	}
	if err := json.Unmarshal(raw, &span); err != nil || span.Content != "two\n" || !span.Truncated || span.LineCount != 1 || len(span.Version) != 64 {
		t.Fatalf("span: %s (%v)", raw, err)
	}
	for _, path := range []string{"../main.c", "/etc/passwd", ".env", "missing.c"} {
		raw, err = s.CodeIndex(ctx, CodeIndexRequest{Route: "/v1/code/span", Project: "detached", FilePath: path})
		if err != nil || !strings.Contains(string(raw), `"error"`) {
			t.Fatalf("path %s: %s (%v)", path, raw, err)
		}
	}
}

// A repository with a submodule manifest must publish atomically, while hidden
// credential/config paths are rejected before changing the published generation.
func TestPrivateCodeManifestPublication(t *testing.T) {
	ctx, s, _ := codeFixture(t)
	call := func(phase string, files []CodeFile, count int) {
		t.Helper()
		_, err := s.CodeIndex(ctx, CodeIndexRequest{Route: "/v1/code/scan", Project: "manifest", Root: "/fixture/manifest", ScanID: "first", Phase: phase, Files: files, ExpectedFiles: count})
		if err != nil {
			t.Fatal(err)
		}
	}
	call("begin", nil, 0)
	call("stage", []CodeFile{{Path: ".gitmodules", Content: "[submodule]"}, {Path: "nested/.gitmodules", Content: "[submodule]"}, {Path: "source.c", Content: "int visible;"}}, 0)
	call("seal", nil, 3)
	raw, err := s.CodeIndex(ctx, CodeIndexRequest{Route: "/v1/code/project-stats?project=manifest"})
	var stats struct{ Files int }
	if err != nil || json.Unmarshal(raw, &stats) != nil || stats.Files != 3 {
		t.Fatalf("publication: %s (%v)", raw, err)
	}
	for _, path := range []string{".mcp.json", "nested/.env.production.json", ".git/config", ".gitmodules/source.c", "nested/.gitmodules/config", "x/../.gitmodules"} {
		_, err := s.CodeIndex(ctx, CodeIndexRequest{Route: "/v1/code/scan", Project: "manifest", Root: "/fixture/manifest", Files: []CodeFile{{Path: path, Content: "hidden"}}})
		if err == nil {
			t.Fatalf("hidden path accepted: %s", path)
		}
	}
	raw, err = s.CodeIndex(ctx, CodeIndexRequest{Route: "/v1/code/span", Project: "manifest", FilePath: "source.c", LineStart: 1, LineEnd: 1})
	if err != nil || !strings.Contains(string(raw), `"generation":1`) || !strings.Contains(string(raw), "int visible;") {
		t.Fatalf("rejected scan changed publication: %s (%v)", raw, err)
	}
}

func TestPrivateCodePublishedSpanBoundsAndDrift(t *testing.T) {
	ctx, s, _ := codeFixture(t)
	files := []CodeFile{{Path: "short.c", Content: "one\ntwo\nthree"}, {Path: "empty.c", Content: ""}, {Path: "many.c", Content: strings.Repeat("line\n", 450)}, {Path: "wide.c", Content: "ok\n" + strings.Repeat("x", 65536) + "\n"}}
	publish := func(phase, scan string, files []CodeFile) {
		t.Helper()
		_, err := s.CodeIndex(ctx, CodeIndexRequest{Route: "/v1/code/scan", Project: "spans", Root: "/unreadable/client", Phase: phase, ScanID: scan, Files: files, ExpectedFiles: 1})
		if err != nil {
			t.Fatal(err)
		}
	}
	publish("", "", files)
	for _, tc := range []struct {
		name, path                   string
		start, end, max, lines, last int
		truncated                    bool
		content                      string
	}{
		{"defaults", "short.c", 0, 0, 0, 1, 1, false, "one\n"},
		{"unterminated last line", "short.c", 2, 3, 400, 2, 3, false, "two\nthree"},
		{"short file not truncated", "short.c", 1, 1000, 400, 3, 3, false, "one\ntwo\nthree"},
		{"past EOF", "short.c", 90, 100, 400, 0, 0, false, ""},
		{"empty", "empty.c", 1, 10, 400, 0, 0, false, ""},
		{"hard line cap", "many.c", 1, 1000, 1000, 400, 400, true, strings.Repeat("line\n", 400)},
		{"byte cap", "wide.c", 1, 2, 400, 1, 1, true, "ok\n"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			raw, err := s.CodeIndex(ctx, CodeIndexRequest{Route: "/v1/code/span", Project: "spans", FilePath: tc.path, LineStart: tc.start, LineEnd: tc.end, MaxLines: tc.max})
			var reply struct {
				Content    string
				LineCount  int `json:"line_count"`
				LineEnd    int `json:"line_end"`
				Truncated  bool
				Version    string `json:"source_version"`
				Generation int
				Freshness  string
			}
			if err != nil || json.Unmarshal(raw, &reply) != nil {
				t.Fatalf("span: %s (%v)", raw, err)
			}
			if reply.Content != tc.content || reply.LineCount != tc.lines || reply.LineEnd != tc.last || reply.Truncated != tc.truncated || reply.Generation != 1 || reply.Freshness != "published" {
				t.Fatalf("incorrect span: %+v", reply)
			}
			for _, file := range files {
				if file.Path == tc.path && reply.Version != fmt.Sprintf("%x", sha256.Sum256([]byte(file.Content))) {
					t.Fatal("version does not hash the whole published file")
				}
			}
		})
	}
	publish("begin", "next", nil)
	publish("stage", "next", []CodeFile{{Path: "short.c", Content: "changed\n"}})
	raw, err := s.CodeIndex(ctx, CodeIndexRequest{Route: "/v1/code/span", Project: "spans", FilePath: "short.c", LineStart: 1, LineEnd: 1})
	if err != nil || !strings.Contains(string(raw), `"content":"one\n"`) {
		t.Fatalf("staged bytes escaped: %s (%v)", raw, err)
	}
	publish("seal", "next", nil)
	raw, err = s.CodeIndex(ctx, CodeIndexRequest{Route: "/v1/code/span", Project: "spans", FilePath: "short.c", LineStart: 1, LineEnd: 1})
	if err != nil || !strings.Contains(string(raw), `"content":"changed\n"`) || !strings.Contains(string(raw), `"generation":2`) {
		t.Fatalf("new generation not visible: %s (%v)", raw, err)
	}
}
