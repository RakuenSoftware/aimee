package memory

import (
	"context"
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"github.com/JBailes/aimee/server-go/modules/egress"
	"os"
	"strings"
	"sync/atomic"
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
CREATE TABLE memory_collection_owner(id int PRIMARY KEY,owner_id uuid);
INSERT INTO memory_collection_owner VALUES(1,'00000000-0000-0000-0000-000000000001');
CREATE TEMP TABLE memory_units(id bigint PRIMARY KEY,memory_id bigint,unit_type text,unit_key text,unit_text text,memory_kind text,weight float8,is_episode_card int DEFAULT 0);
CREATE INDEX memory_units_card_fixture_idx ON memory_units(memory_id);
CREATE TEMP TABLE rules(id bigint,record_revision bigint DEFAULT 1,domain text DEFAULT '',expires_at text DEFAULT '');
GRANT SELECT ON rules TO PUBLIC;
CREATE TEMP TABLE memory_lineage(object_type text,object_id bigint,source_kind text,source_ref text);
CREATE INDEX memory_lineage_card_fixture_idx ON memory_lineage(object_type,object_id);
CREATE TABLE memories(id bigint PRIMARY KEY,record_revision bigint NOT NULL DEFAULT 1,scope_type text,scope_value text,tier text,kind text,key text,content text,
 confidence double precision,lifecycle_state text,activation_suppressed int DEFAULT 0,use_cases text DEFAULT '',updated_at timestamptz DEFAULT now(),valid_from text DEFAULT '',valid_until text DEFAULT '');
CREATE TABLE memory_entities(memory_id bigint,entity text,weight double precision DEFAULT 1);
CREATE TABLE fact_evidence(assertion_id bigint,source_kind text,source_id text);
CREATE TABLE projects(name text,lifecycle_state text);
CREATE TABLE code_projection_generations(id bigint,project text,state text);
CREATE TABLE entity_edges(id bigint,source text,target text,confidence_class text,utility_score double precision,
 relation text DEFAULT 'related_to',weight int DEFAULT 1,structural_weight int DEFAULT 0,utility_touched_at text DEFAULT '',
 edge_class text DEFAULT 'semantic',edge_origin text DEFAULT 'extracted',projection_generation_id bigint,
 suppressed int DEFAULT 0,superseded_at text DEFAULT '',invalidated_at text DEFAULT '',lifecycle_state text DEFAULT 'persistent',valid_from text DEFAULT '',valid_until text DEFAULT '');
INSERT INTO memories(id,scope_type,scope_value,tier,kind,key,content,confidence,lifecycle_state) VALUES
(1,'project','alpha','L2','fact','quasar','entry note',1,'active'),
(2,'project','alpha','L2','fact','helper','indirect note',1,'active'),
(3,'project','beta','L2','fact','private','other project',1,'active'),
(4,'project','alpha','L2','fact','retired','old note',1,'retired');
INSERT INTO memory_entities(memory_id,entity) VALUES(1,'entry'),(2,'helper'),(3,'helper'),(4,'helper');
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
	if len(on) != 2 || on[0].ID != 1 || on[1].ID != 2 || !on[1].Version.ValidFor(2) || on[1].Version.RecordRevision != "1" {
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

func TestCodeGenerationResumeRollbackAndFallback(t *testing.T) {
	t.Setenv("AIMEE_GRAPH_FUSION", "off")
	ctx, s, db := codeFixture(t)
	files := []CodeFile{{Path: "anchor.c", Content: "quasar"}}
	scanID := 0
	publish := func() {
		scanID++
		t.Helper()
		for _, phase := range []string{"begin", "stage", "seal"} {
			req := CodeIndexRequest{Route: "/v1/code/scan", Project: "alpha", Root: "/fixture/alpha", Phase: phase, ScanID: fmt.Sprintf("generation-%d", scanID), ExpectedFiles: len(files)}
			if phase == "stage" {
				req.Files = files
			}
			if _, err := s.CodeIndex(ctx, req); err != nil {
				t.Fatal(err)
			}
		}
	}
	publish()
	executor := &vectorTestEgress{serving: "model-a"}
	p := &personalVectors{endpoint: "https://fixture-embedder", executor: executor, db: db, code: s}
	s.personal = p
	batch := func() {
		t.Helper()
		if err := p.indexCodeBatch(ctx); err != nil {
			t.Fatal(err)
		}
	}
	active := func() string {
		t.Helper()
		var value string
		if err := db.QueryRow(ctx, `SELECT generation FROM user_code_embedding_active WHERE project='alpha'`).Scan(&value); err != nil {
			t.Fatal(err)
		}
		return value
	}
	search := func(query string, want int, readiness string) {
		t.Helper()
		before := len(executor.seen)
		raw, err := s.searchCode(ctx, "alpha", query, 100)
		if err != nil {
			t.Fatal(err)
		}
		var reply struct {
			Hits             []json.RawMessage
			IndexGenerations []struct{ Readiness string } `json:"index_generations"`
		}
		if err = json.Unmarshal(raw, &reply); err != nil {
			t.Fatal(err)
		}
		if len(reply.Hits) != want {
			t.Fatalf("hits: %s", raw)
		}
		if readiness != "" && (len(reply.IndexGenerations) != 1 || reply.IndexGenerations[0].Readiness != readiness) {
			t.Fatalf("readiness: %s", raw)
		}
		if len(executor.seen) != before+1 {
			t.Fatal("query performed document maintenance")
		}
	}
	batch()
	old := active()
	for i := 0; i < 20; i++ {
		files = append(files, CodeFile{Path: fmt.Sprintf("file-%02d.c", i), Content: "worker"})
	}
	publish()
	executor.serving = "model-b"
	batch()
	if active() != old {
		t.Fatal("partial generation activated")
	}
	search("semantic question", 0, "identity_mismatch")
	before := len(executor.seen)
	// Restart the worker and reuse its exact-version completed work.
	p = &personalVectors{endpoint: "https://fixture-embedder", executor: executor, db: db, code: s}
	s.personal = p
	batch()
	if active() != codeGenerationID("model-b") || len(executor.seen)-before != 5 {
		t.Fatal("backfill failed to resume without duplicate work")
	}
	search("semantic question", 21, "ready")
	// Remove an indexed source before rollback. Its vectors must be removed from
	// every retained generation, and the old generation must catch up first.
	files = files[1:]
	publish()
	search("semantic question", 0, "lagging")
	executor.serving = "model-a"
	batch()
	if active() != codeGenerationID("model-b") {
		t.Fatal("incomplete rollback activated")
	}
	batch()
	if active() != old {
		t.Fatal("rollback did not activate")
	}
	search("semantic question", 20, "ready")
	var erased int
	if err := db.QueryRow(ctx, `SELECT count(*) FROM user_code_embedding_versions WHERE path='anchor.c'`).Scan(&erased); err != nil || erased != 0 {
		t.Fatal("deleted vectors survived rollback", erased, err)
	}
	if _, err := db.Exec(ctx, `ALTER TABLE user_code_embedding_versions RENAME TO unavailable_code_vectors`); err != nil {
		t.Fatal(err)
	}
	search("worker", 20, "")
	// The optional SQL failure did not poison the caller's transaction.
	var one int
	if err := db.QueryRow(ctx, `SELECT 1`).Scan(&one); err != nil || one != 1 {
		t.Fatal(err)
	}
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

func TestCodeQueryWaitsForBusyEmbedderWithinCallerBudget(t *testing.T) {
	t.Setenv("AIMEE_GRAPH_FUSION", "off")
	ctx, s, db := codeFixture(t)
	_, err := s.CodeIndex(ctx, CodeIndexRequest{Route: "/v1/code/scan", Project: "alpha", Root: "/fixture/alpha", Files: []CodeFile{{Path: "worker.c", Content: "void worker() {}"}}})
	if err != nil {
		t.Fatal(err)
	}
	executor := &vectorTestEgress{serving: "model-a"}
	p := &personalVectors{endpoint: "https://fixture-embedder", executor: executor, db: db, code: s}
	s.personal = p
	if err := p.indexCodeBatch(ctx); err != nil {
		t.Fatal(err)
	}
	// A document ahead of this query takes longer than the previous 1.5s cap.
	executor.delay = 2 * time.Second
	raw, err := s.searchCode(ctx, "alpha", "semantic question", 10)
	var diagnostic struct {
		DenseState string `json:"dense_state"`
	}
	if err != nil || json.Unmarshal(raw, &diagnostic) != nil || diagnostic.DenseState != "query_executed" {
		t.Fatalf("busy healthy embedder lost semantic retrieval: %s (%v)", raw, err)
	}
	bounded, cancel := context.WithTimeout(ctx, 500*time.Millisecond)
	defer cancel()
	started := time.Now()
	raw, err = s.searchCode(bounded, "alpha", "worker", 10)
	if err != nil || json.Unmarshal(raw, &diagnostic) != nil || diagnostic.DenseState != "unavailable" || time.Since(started) >= 500*time.Millisecond {
		t.Fatalf("short caller budget was not preserved for lexical SQL: %s (%v)", raw, err)
	}
}

func TestCodeRebuildReusesOnlyIdenticalVerifiedInputs(t *testing.T) {
	ctx, s, db := codeFixture(t)
	publish := func(project, path, content string) {
		t.Helper()
		if _, err := s.CodeIndex(ctx, CodeIndexRequest{Route: "/v1/code/scan", Project: project, Root: "/fixture/" + project, Files: []CodeFile{{Path: path, Content: content}}}); err != nil {
			t.Fatal(err)
		}
	}
	publish("alpha", "worker.c", "void worker() {}")
	identity := fixtureEmbeddingIdentity()
	identity.Dimensions = 3
	executor := &vectorTestEgress{identity: &identity}
	p := &personalVectors{endpoint: "https://fixture-embedder", executor: executor, db: db, code: s}
	s.personal = p
	if err := p.indexCodeBatch(ctx); err != nil {
		t.Fatal(err)
	}
	before := len(executor.seen)
	publish("beta", "worker.c", "void worker() {}")
	publish("gamma", "different.c", "void worker() {}")
	if err := p.indexCodeBatch(ctx); err != nil {
		t.Fatal(err)
	}
	if len(executor.seen)-before != 1 {
		t.Fatalf("identical input was recomputed or a different path reused it: %v", executor.seen[before:])
	}
	var active int
	if err := db.QueryRow(ctx, `SELECT count(*) FROM user_code_embedding_active WHERE project IN ('alpha','beta','gamma')`).Scan(&active); err != nil || active != 3 {
		t.Fatal(active, err)
	}
	// Model identity changes invalidate every old cached vector.
	identity.ArtifactRevision = strings.Repeat("c", 40)
	before = len(executor.seen)
	if err := p.indexCodeBatch(ctx); err != nil {
		t.Fatal(err)
	}
	if len(executor.seen)-before != 3 {
		t.Fatal("a different generation borrowed cached vectors")
	}
	// A real content change also requires a fresh embedding.
	publish("beta", "worker.c", "void changed() {}")
	before = len(executor.seen)
	if err := p.indexCodeBatch(ctx); err != nil {
		t.Fatal(err)
	}
	if len(executor.seen)-before != 1 {
		t.Fatal("changed input borrowed a stale vector")
	}
}

// Hold the first two requests until both are in flight. A serial worker times
// out; an unbounded worker exceeds the measured concurrency limit.
type codePairEgress struct {
	base    vectorTestEgress
	entered chan struct{}
	release chan struct{}
	active  atomic.Int32
	peak    atomic.Int32
	calls   atomic.Int32
}

func (e *codePairEgress) Do(ctx context.Context, trace uint64, r egress.HTTPRequest) (egress.HTTPResponse, error) {
	if r.Purpose != "embedding-health" {
		n := e.active.Add(1)
		defer e.active.Add(-1)
		for old := e.peak.Load(); n > old; old = e.peak.Load() {
			if e.peak.CompareAndSwap(old, n) {
				break
			}
		}
		if e.calls.Add(1) <= 2 {
			e.entered <- struct{}{}
			select {
			case <-e.release:
			case <-ctx.Done():
				return egress.HTTPResponse{}, ctx.Err()
			}
		}
	}
	return e.base.Do(ctx, trace, r)
}
func TestCodeIndexBoundsParallelEmbeddingAndPublishesAllFiles(t *testing.T) {
	ctx, s, db := codeFixture(t)
	ctx, cancel := context.WithTimeout(ctx, 5*time.Second)
	defer cancel()
	files := make([]CodeFile, 5)
	for i := range files {
		files[i] = CodeFile{Path: fmt.Sprintf("file%d.c", i), Content: fmt.Sprintf("unique content %d", i)}
	}
	if _, err := s.CodeIndex(ctx, CodeIndexRequest{Route: "/v1/code/scan", Project: "parallel", Root: "/fixture/parallel", Files: files}); err != nil {
		t.Fatal(err)
	}
	e := &codePairEgress{base: vectorTestEgress{serving: "pair-model"}, entered: make(chan struct{}, 2), release: make(chan struct{})}
	p := &personalVectors{endpoint: "https://fixture-embedder", executor: e, db: db, code: s}
	done := make(chan error, 1)
	go func() { done <- p.indexCodeBatch(ctx) }()
	for i := 0; i < 2; i++ {
		select {
		case <-e.entered:
		case <-ctx.Done():
			close(e.release)
			<-done
			t.Fatal("document requests did not overlap")
		}
	}
	close(e.release)
	if err := <-done; err != nil {
		t.Fatal(err)
	}
	if e.peak.Load() != 2 || e.calls.Load() != 5 {
		t.Fatalf("peak=%d calls=%d", e.peak.Load(), e.calls.Load())
	}
	var count int
	if err := db.QueryRow(ctx, `SELECT count(*) FROM user_code_embedding_versions`).Scan(&count); err != nil || count != 5 {
		t.Fatalf("published=%d error=%v", count, err)
	}
}
func TestCodeIndexSharesWorkAndDoesNotStarveReadyProjects(t *testing.T) {
	ctx, s, db := codeFixture(t)
	for i := 0; i < 20; i++ {
		name := fmt.Sprintf("busy-%02d", i)
		files := make([]CodeFile, 20)
		for j := range files {
			files[j] = CodeFile{Path: fmt.Sprintf("file-%02d.c", j), Content: fmt.Sprintf("unique %s %d", name, j)}
		}
		if _, err := s.CodeIndex(ctx, CodeIndexRequest{Route: "/v1/code/scan", Project: name, Root: "/fixture/" + name, Files: files}); err != nil {
			t.Fatal(err)
		}
	}
	// A complete empty repository sorts behind more than sixteen unfinished ones.
	if _, err := db.Exec(ctx, `INSERT INTO user_code_projects(name,root) VALUES('zz-ready','/fixture/ready')`); err != nil {
		t.Fatal(err)
	}
	p := &personalVectors{endpoint: "https://fixture-embedder", executor: &vectorTestEgress{serving: "fair-model"}, db: db, code: s}
	if err := p.indexCodeBatch(ctx); err != nil {
		t.Fatal(err)
	}
	var projects int
	if err := db.QueryRow(ctx, `SELECT count(DISTINCT project) FROM user_code_embedding_versions`).Scan(&projects); err != nil || projects != 16 {
		t.Fatalf("projects advanced=%d error=%v", projects, err)
	}
	var ready bool
	if err := db.QueryRow(ctx, `SELECT EXISTS(SELECT 1 FROM user_code_embedding_active WHERE project='zz-ready')`).Scan(&ready); err != nil || !ready {
		t.Fatalf("completed project starved: ready=%v error=%v", ready, err)
	}
}

func TestCodeIndexRepairsHashOnlyMismatchWithoutDuplicatingOrdinaryWork(t *testing.T) {
	ctx, s, db := codeFixture(t)
	e := &vectorTestEgress{serving: "hash-repair"}
	p := &personalVectors{endpoint: "https://fixture-embedder", executor: e, db: db, code: s}
	if _, err := s.CodeIndex(ctx, CodeIndexRequest{Route: "/v1/code/scan", Project: "alpha", Root: "/fixture/alpha", Files: []CodeFile{{Path: "old.c", Content: "original content"}}}); err != nil {
		t.Fatal(err)
	}
	if err := p.indexCodeBatch(ctx); err != nil {
		t.Fatal(err)
	}
	if _, err := db.Exec(ctx, `UPDATE user_code_embedding_versions SET input_hash='damaged' WHERE project='alpha'`); err != nil {
		t.Fatal(err)
	}
	files := []CodeFile{{Path: "one.c", Content: "new one"}, {Path: "two.c", Content: "new two"}, {Path: "three.c", Content: "new three"}}
	if _, err := s.CodeIndex(ctx, CodeIndexRequest{Route: "/v1/code/scan", Project: "beta", Root: "/fixture/beta", Files: files}); err != nil {
		t.Fatal(err)
	}
	before := len(e.seen)
	if err := p.indexCodeBatch(ctx); err != nil {
		t.Fatal(err)
	}
	if got := len(e.seen) - before; got != 4 {
		t.Fatalf("expected three ordinary inputs and one hash repair, got %d", got)
	}
	var valid int
	if err := db.QueryRow(ctx, `SELECT count(*) FROM user_code_files f JOIN user_code_embedding_versions v USING(project,path) WHERE v.input_hash=`+codeEmbeddingInputHashSQL).Scan(&valid); err != nil || valid != 4 {
		t.Fatalf("valid=%d error=%v", valid, err)
	}
}

func TestCodeIndexRetryExclusionsApplyOnlyToTheCurrentInput(t *testing.T) {
	ctx, s, db := codeFixture(t)
	e := &vectorTestEgress{serving: "retry-model"}
	p := &personalVectors{endpoint: "https://fixture-embedder", executor: e, db: db, code: s}
	files := []CodeFile{{Path: "blocked.c", Content: "blocked content"}, {Path: "changed.c", Content: "changed content"}}
	if _, err := s.CodeIndex(ctx, CodeIndexRequest{Route: "/v1/code/scan", Project: "alpha", Root: "/fixture/alpha", Files: files}); err != nil {
		t.Fatal(err)
	}
	generation, err := p.prepareCodeGeneration(ctx, e.serving)
	if err != nil {
		t.Fatal(err)
	}
	if _, err = db.Exec(ctx, `INSERT INTO user_code_embedding_jobs(project,path,generation,input_hash,state,attempts,next_attempt_at)
 SELECT f.project,f.path,$1,CASE WHEN f.path='blocked.c' THEN `+codeEmbeddingInputHashSQL+` ELSE 'obsolete-input' END,'failed',8,clock_timestamp()+interval '1 hour' FROM user_code_files f`, generation); err != nil {
		t.Fatal(err)
	}
	if err = p.indexCodeBatch(ctx); err != nil {
		t.Fatal(err)
	}
	if len(e.seen) != 1 || !strings.Contains(e.seen[0], "changed.c") {
		t.Fatalf("blocked current input or obsolete retry state handled incorrectly: %v", e.seen)
	}
	if _, err = db.Exec(ctx, `UPDATE user_code_embedding_jobs SET attempts=1,next_attempt_at='-infinity' WHERE path='blocked.c'`); err != nil {
		t.Fatal(err)
	}
	if err = p.indexCodeBatch(ctx); err != nil {
		t.Fatal(err)
	}
	if len(e.seen) != 2 || !strings.Contains(e.seen[1], "blocked.c") {
		t.Fatalf("eligible retry did not resume: %v", e.seen)
	}
}
