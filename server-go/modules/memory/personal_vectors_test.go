package memory

import (
	"context"
	"encoding/json"
	"errors"
	"math"
	"os"
	"strings"
	"testing"
	"time"

	store "github.com/JBailes/aimee/server-go/db"
	"github.com/JBailes/aimee/server-go/modules/egress"
	"github.com/jackc/pgx/v5"
)

type vectorEvalStore struct{ evalQueryer }

func (d vectorEvalStore) Begin(context.Context) (store.Tx, error) { return nil, errors.New("unused") }
func (d vectorEvalStore) CurrentSchemaVersion(context.Context, string) (int64, string, error) {
	return 0, "", nil
}
func (d vectorEvalStore) Migrate(ctx context.Context, m store.MigrationRequest) error {
	var persistence string
	if err := d.QueryRow(ctx, `SELECT COALESCE((SELECT relpersistence::text FROM pg_class WHERE oid=to_regclass('user_memories')),'t')`).Scan(&persistence); err != nil {
		return err
	}
	for _, sql := range m.Statements {
		if persistence == "t" {
			sql = strings.Replace(sql, "CREATE TABLE IF NOT EXISTS", "CREATE TEMP TABLE IF NOT EXISTS", 1)
		}
		if _, err := d.Exec(ctx, sql); err != nil {
			return err
		}
	}
	return nil
}

type vectorTestEgress struct {
	fail        bool
	stall       bool
	serving     string
	seen        []string
	targets     []string
	beforeEmbed func()
}

func (e *vectorTestEgress) Do(ctx context.Context, _ uint64, r egress.HTTPRequest) (egress.HTTPResponse, error) {
	e.targets = append(e.targets, r.TargetURL)
	if e.stall {
		<-ctx.Done()
		return egress.HTTPResponse{}, ctx.Err()
	}
	if e.fail {
		return egress.HTTPResponse{}, errors.New("offline")
	}
	if r.Purpose == "embedding-health" {
		return egress.HTTPResponse{Status: 200, Body: []byte(`{"serving_id":"` + e.serving + `"}`)}, nil
	}
	e.seen = append(e.seen, string(r.Body))
	if e.beforeEmbed != nil {
		e.beforeEmbed()
		e.beforeEmbed = nil
	}
	vector := `[1,0,0]`
	if strings.Contains(string(r.Body), "unrelated") {
		vector = `[0,1,0]`
	}
	return egress.HTTPResponse{Status: 200, Body: []byte(vector)}, nil
}
func TestPersonalVectorsRejectInvalidNumbersAndDeduplicateFusion(t *testing.T) {
	for _, v := range [][]float32{nil, {0, 0}, {float32(math.NaN())}, {float32(math.Inf(1))}} {
		if _, err := vectorLiteral(v); err == nil {
			t.Fatal("invalid vector accepted")
		}
	}
	a := Record{ID: 1}
	b := Record{ID: 2}
	c := Record{ID: 3}
	out := fusePersonal([]Record{a, b}, []Record{b, c}, 2)
	if len(out) != 2 || out[0].ID != 2 || out[1].ID != 1 {
		t.Fatalf("fusion: %+v", out)
	}
}
func TestPersonalVectorPrivacyAndMutationRegression(t *testing.T) {
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
	defer conn.Close(ctx)
	tx, err := conn.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	if _, err = tx.Exec(ctx, `CREATE EXTENSION IF NOT EXISTS vector`); err != nil {
		t.Fatal(err)
	}
	raw, err := os.ReadFile("../aimee/families/schema_conversation.sql")
	if err != nil {
		t.Fatal(err)
	}
	schema := string(raw)
	a, b := strings.Index(schema, "CREATE TABLE IF NOT EXISTS user_memories ("), strings.Index(schema, "CREATE INDEX IF NOT EXISTS user_memories_recall")
	if _, err = tx.Exec(ctx, `CREATE SCHEMA personal_vectors_fixture; SET LOCAL search_path=personal_vectors_fixture,public`+";"+schema[a:b]); err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"schema_personal_memory_changes.sql", "schema_personal_memory_versions.sql", "schema_personal_memory_send_guards.sql", "schema_personal_memory_send_guard_completion.sql", "schema_personal_embedding_cutover.sql"} {
		sql, err := os.ReadFile("../aimee/families/" + name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err = tx.Exec(ctx, string(sql)); err != nil {
			t.Fatal(name, err)
		}
	}
	if _, err = tx.Exec(ctx, `CREATE TEMP TABLE memories(id bigint,content text);
INSERT INTO memories VALUES(42,'shared secret must never be embedded by the personal owner');
INSERT INTO user_memories(id,key,content) VALUES(42,'private-location','I keep my bicycle in the garden shed'),(43,'unrelated','unrelated astronomy');`); err != nil {
		t.Fatal(err)
	}
	db := vectorEvalStore{evalQueryer{tx}}
	executor := &vectorTestEgress{serving: "fixture-revision-1"}
	p := &personalVectors{db: db, executor: executor, endpoint: "https://local.invalid"}
	if err = p.indexBatch(ctx); err != nil {
		t.Fatal(err)
	}
	if len(executor.seen) != 2 || !strings.Contains(executor.seen[0], "bicycle") {
		t.Fatalf("incorrect embedding inputs: %d", len(executor.seen))
	}
	for _, input := range executor.seen {
		if strings.Contains(input, "shared secret") {
			t.Fatal("KB contents leaked to personal embedder")
		}
	}
	data, _ := NewPostgresDataStore(db, PlacementServer)
	s := data.(*postgresDataStore)
	s.personal = p
	if err := s.UpsertEmbedding(ctx, Record{ID: 42}, []float32{1, 0, 0}); err == nil {
		t.Fatal("unscoped vector write accepted for personal placement")
	}
	embedded := EmbedRecord(ctx, 0, executor, s, 42, "https://unselected.invalid", 3)
	if embedded.Error != "" || !embedded.Embedded || embedded.ServingID != executor.serving {
		t.Fatalf("explicit personal embedding: %+v", embedded)
	}
	for _, target := range executor.targets {
		if !strings.HasPrefix(target, "https://local.invalid/") {
			t.Fatal("personal data sent to an unselected model")
		}
	}
	records, err := s.Search(ctx, Scope{Type: ScopeUser, Value: "_user"}, "where is my bike", "", "", 5)
	if err != nil || len(records) != 1 || records[0].ID != 42 {
		t.Fatalf("semantic recall: %+v %v", records, err)
	}
	if _, err = tx.Exec(ctx, `UPDATE user_memories SET content='new location' WHERE id=42`); err != nil {
		t.Fatal(err)
	}
	records, err = p.search(ctx, "where is my bike", "", "", 5)
	if err != nil || len(records) != 0 {
		t.Fatal("stale content vector was recalled")
	}
	executor.beforeEmbed = func() {
		if _, err := tx.Exec(ctx, `UPDATE user_memories SET content='concurrent edit' WHERE id=42`); err != nil {
			t.Fatal(err)
		}
	}
	if err = p.indexBatch(ctx); err != nil {
		t.Fatal(err)
	}
	records, err = p.search(ctx, "where is my bike", "", "", 5)
	if err != nil || len(records) != 0 {
		t.Fatal("in-flight vector overwrote concurrent edit")
	}
	if err = p.indexBatch(ctx); err != nil {
		t.Fatal(err)
	}
	if _, err = tx.Exec(ctx, `UPDATE user_memories SET valid_until=now()-interval '1 second' WHERE id=42`); err != nil {
		t.Fatal(err)
	}
	records, err = p.search(ctx, "where is my bike", "", "", 5)
	if err != nil || len(records) != 0 {
		t.Fatal("expired private memory was recalled")
	}
	executor.serving = "different-model"
	records, err = p.search(ctx, "where is my bike", "", "", 5)
	if err == nil || len(records) != 0 {
		t.Fatal("vector spaces mixed or identity mismatch unreported")
	}
	executor.stall = true
	bounded, cancel := context.WithTimeout(ctx, 500*time.Millisecond)
	records, err = s.Search(bounded, Scope{Type: ScopeUser, Value: "_user"}, "astronomy", "", "", 5)
	if err != nil || len(records) != 1 || records[0].ID != 43 || bounded.Err() != nil {
		t.Fatal("slow embedder consumed the deadline needed to return lexical recall")
	}
	cancel()
	executor.stall = false
	executor.fail = true
	records, err = s.Search(ctx, Scope{Type: ScopeUser, Value: "_user"}, "astronomy", "", "", 5)
	if err != nil || len(records) != 1 || records[0].ID != 43 {
		t.Fatal("embedder outage lost local lexical recall")
	}
	// Exercise the public recall producer, not manually invented source refs.
	rawBundle, err := s.recallBundleActivated(ctx, "astronomy", 8192, false, nil)
	if err != nil {
		t.Fatal(err)
	}
	var bundle recallBundle
	if json.Unmarshal(rawBundle, &bundle) != nil {
		t.Fatal("invalid recall")
	}
	projection, _, err := projectNativeRecall(bundle, 32768)
	if err != nil || len(projection.Sources) != 2 || projection.Sources[0].Source.Kind != "user_memory_collection" || projection.Sources[1].Channel != "native_active_context" || projection.Sources[1].Source.Version.RecordID != "43" {
		t.Fatalf("private active context lacks observed version: %+v %v", projection, err)
	}
	check := &sourceRevalidation{SchemaVersion: 1, CheckID: strings.Repeat("d", 32), Sources: projection.Sources}
	if ok, err := s.revalidatePersonalSources(ctx, check); err != nil || !ok {
		t.Fatal("fresh private source", ok, err)
	}
	if _, err := tx.Exec(ctx, "UPDATE user_memories SET content='edited astronomy after selection',record_revision=record_revision+1 WHERE id=43"); err != nil {
		t.Fatal(err)
	}
	if ok, err := s.revalidatePersonalSources(ctx, check); err != nil || ok {
		t.Fatal("edited private source admitted", ok, err)
	}
	// A vector SQL failure must not abort the surrounding read transaction or
	// consume the already available lexical result.
	executor.fail = false
	s.db = evalQueryer{tx}
	executor.serving = "fixture-revision-1"
	if _, err := tx.Exec(ctx, "ALTER TABLE user_memory_embedding_versions RENAME TO unavailable_vectors"); err != nil {
		t.Fatal(err)
	}
	records, err = s.Search(ctx, Scope{Type: ScopeUser, Value: "_user"}, "astronomy", "", "", 5)
	if err != nil || len(records) != 1 || records[0].ID != 43 {
		t.Fatal("vector SQL failure poisoned lexical recall", records, err)
	}
	if _, err := tx.Exec(ctx, "ALTER TABLE unavailable_vectors RENAME TO user_memory_embedding_versions"); err != nil {
		t.Fatal(err)
	}
	// Retained source versions survive ordinary expiry and generation rebuilds.
	var retained int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM user_memory_embedding_versions WHERE memory_id=42`).Scan(&retained); err != nil || retained < 3 {
		t.Fatal("retained private versions overwritten", retained, err)
	}
	if _, err := tx.Exec(ctx, `INSERT INTO user_memories(id,key,content) SELECT n,'generation-probe-'||n,'rollback fixture content' FROM generate_series(100,120) n`); err != nil {
		t.Fatal(err)
	}
	originalGeneration := personalGenerationID("memory", "fixture-revision-1")
	activeGeneration := func() string {
		t.Helper()
		var value string
		if err := tx.QueryRow(ctx, `SELECT generation FROM user_embedding_active WHERE record_class='memory'`).Scan(&value); err != nil {
			t.Fatal(err)
		}
		return value
	}
	executor.serving = "same-dimension-new-space"
	if err := p.indexBatch(ctx); err != nil {
		t.Fatal(err)
	}
	if activeGeneration() != originalGeneration {
		t.Fatal("partial private generation activated")
	}
	// Restart loses every process-local flag; committed staging rows remain and
	// the next bounded batch resumes rather than regenerating completed vectors.
	resumed := &personalVectors{db: db, executor: executor, endpoint: p.endpoint}
	calls := len(executor.seen)
	for n := 0; n < 3 && activeGeneration() == originalGeneration; n++ {
		if err := resumed.indexBatch(ctx); err != nil {
			t.Fatal(err)
		}
	}
	if activeGeneration() != personalGenerationID("memory", executor.serving) || len(executor.seen)-calls >= 16 {
		t.Fatal("restart did not reuse bounded completed work", len(executor.seen)-calls)
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM user_memory_embedding_versions WHERE memory_id=42 AND generation=$1`, activeGeneration()).Scan(&retained); err != nil || retained < 4 {
		t.Fatal("historical private coverage lost during cutover", retained, err)
	}
	// Revocation removes all generation copies at reconciliation. Switching the
	// provider back cannot resurrect the revoked record from a retained space.
	if _, err := tx.Exec(ctx, `UPDATE user_memories SET lifecycle_state='revoked' WHERE id=100`); err != nil {
		t.Fatal(err)
	}
	executor.fail = true
	if err := resumed.indexBatch(ctx); err == nil {
		t.Fatal("offline provider unexpectedly available")
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM user_memory_embedding_versions WHERE memory_id=100`).Scan(&retained); err != nil || retained != 0 {
		t.Fatal("provider outage delayed revocation cleanup", retained, err)
	}
	executor.fail = false
	executor.serving = "fixture-revision-1"
	for n := 0; n < 3; n++ {
		if err := resumed.indexBatch(ctx); err != nil {
			t.Fatal(err)
		}
	}
	if activeGeneration() != originalGeneration {
		t.Fatal("compatible rollback did not activate")
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM user_memory_embedding_versions WHERE memory_id=100`).Scan(&retained); err != nil || retained != 0 {
		t.Fatal("rollback retained revoked vector", retained, err)
	}
	if _, err := tx.Exec(ctx, `DELETE FROM user_memories WHERE id=42`); err != nil {
		t.Fatal(err)
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM user_memory_embedding_versions WHERE memory_id=42`).Scan(&retained); err != nil || retained != 0 {
		t.Fatal("private erasure left generation vectors", retained, err)
	}

}
