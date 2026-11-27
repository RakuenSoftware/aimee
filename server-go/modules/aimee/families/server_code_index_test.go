package families

import (
	"context"
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"os"
	"testing"
	"time"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/jackc/pgx/v5/pgxpool"
)

func TestRuntimeCodeIndexRetainedPublicationPostgres(t *testing.T) {
	root := livePool(t)
	ctx := context.Background()
	if _, err := root.Exec(ctx, `CREATE EXTENSION IF NOT EXISTS vector`); err != nil {
		t.Fatal(err)
	}
	schema := fmt.Sprintf("code_retained_%d", time.Now().UnixNano())
	if _, err := root.Exec(ctx, "CREATE SCHEMA "+schema); err != nil {
		t.Fatal(err)
	}
	cfg, err := pgxpool.ParseConfig(os.Getenv("AIMEE_TEST_PG_URL"))
	if err != nil {
		t.Fatal(err)
	}
	cfg.ConnConfig.RuntimeParams["search_path"] = schema + ",public"
	pool, err := pgxpool.NewWithConfig(ctx, cfg)
	if err != nil {
		t.Fatal(err)
	}
	defer pool.Close()
	// Import the old index unchanged; no old table, row or vector is removed.
	_, err = pool.Exec(ctx, `CREATE TABLE user_code_projects(name text,root text,generation bigint,scanned_at timestamptz);
 CREATE TABLE user_code_files(project text,path text,content text,definitions jsonb,calls jsonb,imports jsonb,module_identity text,embedding vector,embedding_serving text);
 INSERT INTO user_code_projects VALUES('legacy','/owned/legacy',4,now());
 INSERT INTO user_code_files VALUES('legacy','old.c','historical source','[]','[]','[]','',NULL,'');`)
	if err != nil {
		t.Fatal(err)
	}
	for _, file := range []string{"schema_server_code_index.sql", "schema_server_code_scan_progress.sql"} {
		body, _ := schemaFS.ReadFile(file)
		if _, err = pool.Exec(ctx, string(body)); err != nil {
			t.Fatal(err)
		}
	}
	db := explorationLiveDB{liveQueryer{pool}}
	call := func(r codeIndexRequest) map[string]any {
		t.Helper()
		b, _ := json.Marshal(r)
		status, fields, e := runtimeCodeIndex(ctx, db, []string{string(b)})
		if e != nil || status != store.StatusOK || len(fields) != 1 {
			t.Fatalf("code operation failed status=%d err=%v", status, e)
		}
		var out map[string]any
		if e = json.Unmarshal([]byte(fields[0]), &out); e != nil {
			t.Fatal(e)
		}
		if out["status"] == "error" {
			t.Fatalf("code operation refused: %v", out)
		}
		return out
	}
	// refusal returns the structured refusal code, or "" when the store failed
	// outright; either way the request was not accepted.
	refusal := func(r codeIndexRequest) string {
		t.Helper()
		b, _ := json.Marshal(r)
		s, fields, e := runtimeCodeIndex(ctx, db, []string{string(b)})
		if e != nil || s != store.StatusOK {
			return ""
		}
		var out map[string]any
		if len(fields) != 1 || json.Unmarshal([]byte(fields[0]), &out) != nil || out["status"] != "error" {
			t.Fatalf("invalid/stale publication was accepted: %v", fields)
		}
		code, _ := out["code"].(string)
		return code
	}
	reject := func(r codeIndexRequest) { t.Helper(); refusal(r) }
	scan := func(phase, id string, files ...codeFile) codeIndexRequest {
		return codeIndexRequest{Route: "/v1/code/scan", Project: "fixture", Root: "/owned/fixture", Phase: phase, ScanID: id, Files: files, ExpectedFiles: len(files)}
	}
	find := func(symbol string) []any {
		return call(codeIndexRequest{Route: "/v1/code/find?project=fixture&identifier=" + symbol})["hits"].([]any)
	}
	file := codeFile{Path: "engine.c", Content: "int engine(void) { return 42; }", Definitions: []codeDefinition{{Name: "engine", Kind: "function", Line: 1, LineEnd: 1}}, Embedding: []float64{1, 0}, EmbeddingServing: "verified-fixture-identity", EmbeddingPolicy: codeGenerationPolicy}
	call(scan("begin", "first"))
	call(scan("stage", "first", file))
	call(scan("stage", "first", file))
	if len(find("engine")) != 0 {
		t.Fatal("unpublished staged source became readable")
	}
	changed := file
	changed.Content = "changed retry"
	reject(scan("stage", "first", changed))
	incomplete := scan("seal", "first")
	incomplete.ExpectedFiles = 2
	reject(incomplete)
	seal := scan("seal", "first")
	seal.ExpectedFiles = 1
	call(seal)
	if len(find("engine")) != 1 {
		t.Fatal("published symbol unavailable")
	}
	t.Setenv("AIMEE_GRAPH_FUSION", "on")
	dense := call(codeIndexRequest{Route: "/v1/code/hybrid?project=fixture&query=indirect", Embedding: []float64{1, 0}, EmbeddingServing: file.EmbeddingServing, EmbeddingPolicy: codeGenerationPolicy})
	if dense["dense_state"] != "query_executed" || len(dense["hits"].([]any)) != 1 {
		t.Fatal("matching immutable vector was not queried")
	}
	mismatch := call(codeIndexRequest{Route: "/v1/code/hybrid?project=fixture&query=indirect", Embedding: []float64{1, 0}, EmbeddingServing: "other-space", EmbeddingPolicy: codeGenerationPolicy})
	if len(mismatch["hits"].([]any)) != 0 {
		t.Fatal("foreign vector space used")
	}
	// A later scan reuses the stored vector for an unchanged file, under the same
	// embedder identity only, and never for changed content.
	lookup := func(serving string, files ...codeFile) map[string]any {
		return call(codeIndexRequest{Route: "/v1/code/embeddings", Project: "fixture", EmbeddingServing: serving, Files: files})["embeddings"].(map[string]any)
	}
	print := func(content string) string { return fmt.Sprintf("%x", sha256.Sum256([]byte(content))) }
	same := codeFile{Path: "engine.c", Fingerprint: print(file.Content)}
	if got := lookup(file.EmbeddingServing, same); len(got) != 1 || len(got["engine.c"].([]any)) != 2 {
		t.Fatalf("unchanged file's vector not reused: %v", got)
	}
	if got := lookup("other-space", same); len(got) != 0 {
		t.Fatal("vector reused across embedder identities")
	}
	if got := lookup(file.EmbeddingServing, codeFile{Path: "engine.c", Fingerprint: print("changed")}); len(got) != 0 {
		t.Fatal("vector reused for changed content")
	}
	if got := lookup(file.EmbeddingServing, codeFile{Path: "other.c", Fingerprint: print(file.Content)}); len(got) != 0 {
		t.Fatal("vector reused for another path")
	}
	call(scan("begin", "abandoned"))
	call(scan("stage", "abandoned", changed))
	call(scan("abort", "abandoned"))
	if len(find("engine")) != 1 {
		t.Fatal("abort changed current snapshot")
	}
	call(scan("begin", "second"))
	reject(scan("stage", "first", file))
	call(scan("seal", "second"))
	if len(find("engine")) != 0 {
		t.Fatal("omitted source remains available")
	}
	// Concurrent clients join instead of cancelling each other: a scan that is
	// making progress keeps the project, and a second begin is refused with
	// scan_in_progress. A scan that stopped making progress is taken over, and
	// its client learns that from scan_superseded.
	call(scan("begin", "live"))
	if code := refusal(scan("begin", "contender")); code != "scan_in_progress" {
		t.Fatalf("second scan raced the live one: %q", code)
	}
	call(scan("stage", "live"))
	if _, e := pool.Exec(ctx, `UPDATE server_code_scans SET touched_at=now()-interval '10 minutes' WHERE project='fixture' AND scan_id='live'`); e != nil {
		t.Fatal(e)
	}
	call(scan("begin", "takeover"))
	if code := refusal(scan("stage", "live")); code != "scan_superseded" {
		t.Fatalf("abandoned scan kept staging: %q", code)
	}
	call(scan("abort", "takeover"))
	call(codeIndexRequest{Route: "/v1/code/project/delete?project=fixture"})
	reject(scan("begin", "reactivation"))
	for _, sql := range []string{
		"UPDATE server_code_file_versions SET content='rewritten' WHERE project='fixture'",
		"DELETE FROM server_code_file_versions WHERE project='fixture'",
		"DELETE FROM server_code_scans WHERE project='fixture'",
		"DELETE FROM server_code_projects WHERE name='fixture'",
		"TRUNCATE server_code_file_versions",
	} {
		if _, e := pool.Exec(ctx, sql); e == nil {
			t.Fatal("retained source history was mutable", sql)
		}
	}
	var retained, old int
	if e := pool.QueryRow(ctx, `SELECT count(*) FROM server_code_file_versions WHERE project='fixture'`).Scan(&retained); e != nil || retained != 2 {
		t.Fatalf("source history lost: %d %v", retained, e)
	}
	if e := pool.QueryRow(ctx, `SELECT count(*) FROM user_code_files WHERE content='historical source'`).Scan(&old); e != nil || old != 1 {
		t.Fatal("legacy original modified", e)
	}
	var migrated string
	if e := pool.QueryRow(ctx, `SELECT content FROM server_code_files_current WHERE project='legacy'`).Scan(&migrated); e != nil || migrated != "historical source" {
		t.Fatal("legacy current head not imported", e)
	}
}
func TestRuntimeCodeIndexRejectsMalformedWithoutDatabase(t *testing.T) {
	for _, input := range []string{"{", `{"route":"not a URI"}`, `{"route":"/v1/code/scan","project":"x"}`} {
		s, _, err := runtimeCodeIndex(context.Background(), nil, []string{input})
		if err == nil && s == store.StatusOK {
			t.Fatal("malformed request succeeded")
		}
	}
	for _, p := range []string{"../escape", "/root", "dir/../file", "a\\b", ".git/config"} {
		if validCodePath(p) {
			t.Fatal("unsafe code path", p)
		}
	}
}
