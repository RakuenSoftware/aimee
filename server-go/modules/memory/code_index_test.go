package memory

import (
	"context"
	"encoding/json"
	"os"
	"testing"

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
