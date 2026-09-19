package memory

import (
	"context"
	"encoding/json"
	"errors"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

type benchmarkDiagnosticStore struct {
	recordingDataStore
	rows              []Record
	record            Record
	searchErr, getErr error
	searched, read    int
}

func (s *benchmarkDiagnosticStore) Search(_ context.Context, scope Scope, _, _, _ string, limit int) ([]Record, error) {
	s.scope = scope
	s.searched++
	return s.rows[:min(limit, len(s.rows))], s.searchErr
}
func (s *benchmarkDiagnosticStore) Get(_ context.Context, scope Scope, _ int64) (Record, error) {
	s.scope = scope
	s.read++
	return s.record, s.getErr
}

func TestBenchmarkHardNegativeArtifact(t *testing.T) {
	const id = int64(9007199254740993)
	full := strings.Repeat("界", 2100) + "\n\"quoted\""
	for _, placement := range []Placement{PlacementServer, PlacementKB} {
		s := &benchmarkDiagnosticStore{rows: []Record{{ID: id, Key: "large", Content: full, Confidence: .75, Tier: "L2", Kind: "fact"}}}
		handler := NewHandler(nil, WithDataStore(placement, s))
		args := `{"operation":"benchmark-hard-negative","suite":"s","task":"t","query":"query","expected":"expected","got":"actual","error":"failed"}`
		result := runHostRuntime(t, handler, args)
		line := result["line"].(string)
		var artifact benchmarkHardNegative
		if err := json.Unmarshal([]byte(line), &artifact); err != nil {
			t.Fatal(err)
		}
		if len(artifact.Candidates) != 1 || artifact.Candidates[0].ID != id || artifact.Candidates[0].Content != full || artifact.Candidates[0].Confidence != .75 || artifact.Candidates[0].Salience != 0 || artifact.Query != "query" || artifact.Got != "actual" || artifact.Error != "failed" || artifact.MemoryError != "" || strings.ContainsAny(line, "\r\n") {
			t.Fatalf("bad artifact: %+v", artifact)
		}
		if _, err := time.Parse(time.RFC3339, artifact.TS); err != nil {
			t.Fatal(err)
		}
		s.searchErr = errors.New("unavailable")
		result = runHostRuntime(t, handler, args)
		if err := json.Unmarshal([]byte(result["line"].(string)), &artifact); err != nil || artifact.MemoryError == "" || len(artifact.Candidates) != 0 {
			t.Fatal(result, err)
		}
		before := s.searched
		result = runHostRuntime(t, handler, strings.Replace(args, `"query":"query"`, `"query":""`, 1))
		artifact = benchmarkHardNegative{}
		if err := json.Unmarshal([]byte(result["line"].(string)), &artifact); err != nil || artifact.MemoryError != "" || s.searched != before {
			t.Fatal(result, err)
		}
		s.searchErr = nil
		s.rows[0].Content = strings.Repeat("x", benchmarkReportLimit)
		frame, _ := bus.EncodeCommand("runtime", []byte(args))
		if _, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame); status == bus.ModuleStatusOK {
			t.Fatal("oversize artifact truncated or accepted")
		}
	}
}

func TestBenchmarkMissRankAndFailures(t *testing.T) {
	const id = int64(9007199254740993)
	full := strings.Repeat("界", 2100)
	for _, placement := range []Placement{PlacementServer, PlacementKB} {
		s := &benchmarkDiagnosticStore{rows: []Record{{ID: id - 1, Key: "other"}, {ID: id, Key: "expected"}}, record: Record{ID: id, Key: "expected", Content: full}}
		handler := NewHandler(nil, WithDataStore(placement, s))
		args := `{"operation":"benchmark-miss","query":"question","expected_ids":["9007199254740993"],"limit":1,"render":true}`
		result := runHostRuntime(t, handler, args)
		text := result["text"].(string)
		if result["rank"] != float64(2) || result["is_miss"] != true || !strings.Contains(text, "expected_id=9007199254740993") || !strings.Contains(text, "expected_content="+full+"\n") || !strings.Contains(text, "[2]9007199254740993:expected") {
			t.Fatal(result)
		}
		result = runHostRuntime(t, handler, strings.Replace(args, `"limit":1`, `"limit":2`, 1))
		if result["is_miss"] != false || result["bucket"] != "hit" || result["text"] != "" || s.read != 1 {
			t.Fatal(result, s.read)
		}
		result = runHostRuntime(t, handler, strings.Replace(args, `"render":true`, `"render":false`, 1))
		if result["text"] != "" || result["is_miss"] != true {
			t.Fatal(result)
		}
		s.getErr = ErrMemoryNotFound
		result = runHostRuntime(t, handler, args)
		if result["bucket"] != "missing" || strings.Contains(result["text"].(string), "expected_id=") {
			t.Fatal("absent record rendered", result)
		}
		s.getErr = errors.New("read unavailable")
		frame, _ := bus.EncodeCommand("runtime", []byte(args))
		if _, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame); status == bus.ModuleStatusOK {
			t.Fatal("get failure became missing")
		}
		s.getErr = nil
		s.searchErr = errors.New("search unavailable")
		if _, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame); status == bus.ModuleStatusOK {
			t.Fatal("search failure became miss")
		}
		s.searchErr = nil
		s.record.Content = strings.Repeat("x", benchmarkReportLimit)
		if _, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame); status == bus.ModuleStatusOK {
			t.Fatal("oversize report accepted")
		}
	}
}

func TestBenchmarkDiagnosticBoundaries(t *testing.T) {
	for _, placement := range []Placement{PlacementServer, PlacementKB} {
		s := &benchmarkDiagnosticStore{}
		handler := NewHandler(nil, WithDataStore(placement, s))
		for _, args := range []string{
			`{"operation":"benchmark-hard-negative","suite":"s","task":"t","query":"q","expected":"","got":"","error":""}`,
			`{"operation":"benchmark-miss","query":"q","expected_ids":[],"render":true}`,
		} {
			frame, _ := bus.EncodeCommand("runtime", []byte(args))
			if _, status := handler(bus.ModuleInvocation{StageID: StageCommand, PrincipalRef: 9}, frame); status != bus.ModuleStatusInvalidRequest {
				t.Fatal(status)
			}
		}
		for _, ids := range []string{`null`, `[9007199254740993]`, `["9007199254740993.0"]`, `["9223372036854775808"]`, `["0"]`, `["-1"]`, `["01"]`, `["+1"]`, `[` + strings.Repeat(`"1",`, 128) + `"1"]`} {
			frame, _ := bus.EncodeCommand("runtime", []byte(`{"operation":"benchmark-miss","query":"q","expected_ids":`+ids+`}`))
			if _, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame); status != bus.ModuleStatusInvalidRequest {
				t.Fatal(ids, status)
			}
		}
		if s.searched != 0 || s.read != 0 {
			t.Fatal("invalid requests reached store")
		}
		// Match the scorer's full relevance-label budget, including labels beyond
		// the old 20-ID native adapter limit.
		ids := make([]string, 128)
		for i := range ids {
			ids[i] = strconv.Itoa(i + 1)
		}
		s.rows = []Record{{ID: 128, Key: "q", Content: "q", Tier: "L2", Kind: "fact"}}
		args, _ := json.Marshal(map[string]any{"operation": "benchmark-miss", "query": "q", "expected_ids": ids})
		result := runHostRuntime(t, handler, string(args))
		if result["is_miss"] != false || result["rank"] != float64(1) {
			t.Fatal(result)
		}
	}
}

func TestBenchmarkFailureBuckets(t *testing.T) {
	if got := benchmarkFailureBucket("q", nil); got != "missing" {
		t.Fatal(got)
	}
	for _, test := range []struct {
		query, content string
		parts          DiagnosticParts
		want           string
	}{
		{"when", "2026", DiagnosticParts{}, "temporal_miss"},
		{"When", "2026", DiagnosticParts{}, "entity_miss"}, // Preserve case-sensitive legacy classifier.
		{"when", "now", DiagnosticParts{}, "entity_miss"},
		{"q", "", DiagnosticParts{Entity: 1, Semantic: 1}, "lexical_gap"},
		{"q", "", DiagnosticParts{Entity: 1, Lexical: 1}, "semantic_gap"},
		{"q", "", DiagnosticParts{Entity: 1, Lexical: 2, State: -.2}, "state_penalty"},
		{"q", "", DiagnosticParts{Entity: 1, Lexical: 2, State: -.1, Coverage: .49}, "granularity_gap"},
		{"q", "", DiagnosticParts{Entity: 1, Lexical: 2, Coverage: .5}, "ranking_gap"},
	} {
		d := Diagnostic{Memory: Record{Content: test.content}, Parts: test.parts}
		if got := benchmarkFailureBucket(test.query, &d); got != test.want {
			t.Fatal(test, got)
		}
	}
}

func exerciseBenchmarkDiagnosticsReplay(t *testing.T, ctx context.Context, tx pgx.Tx, handler bus.ModuleHandler) {
	t.Helper()
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SAVEPOINT benchmark_diagnostics_replay`)
	defer func() {
		exec(`ROLLBACK TO SAVEPOINT benchmark_diagnostics_replay; RELEASE SAVEPOINT benchmark_diagnostics_replay`)
	}()
	exec(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	full := strings.Repeat("界", 2100)
	var local, private int64
	for _, item := range []struct {
		scope, content string
		id             *int64
	}{
		{"benchmark-diagnostics-local", full, &local}, {"benchmark-diagnostics-private", "private-content", &private},
	} {
		if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,confidence,scope_type,scope_value) VALUES ('L2','fact','benchmark-diagnostics-replay',$1,.5,'project',$2) RETURNING id`, item.content, item.scope).Scan(item.id); err != nil {
			t.Fatal(err)
		}
	}
	args := map[string]any{"operation": "benchmark-miss", "query": "benchmark-diagnostics-replay", "expected_ids": []string{strconv.FormatInt(local, 10)}, "project": "benchmark-diagnostics-local", "limit": 0, "render": true}
	run := func() map[string]any { raw, _ := json.Marshal(args); return runHostRuntime(t, handler, string(raw)) }
	result := run()
	if result["rank"] != float64(1) || !strings.Contains(result["text"].(string), full) || strings.Contains(result["text"].(string), "private-content") {
		t.Fatal(result)
	}
	args["expected_ids"] = []string{strconv.FormatInt(private, 10)}
	result = run()
	if result["bucket"] != "missing" || strings.Contains(result["text"].(string), "expected_id=") {
		t.Fatal("hidden expected row disclosed", result)
	}
	delete(args, "project")
	args["expected_ids"] = []string{strconv.FormatInt(local, 10)}
	result = run()
	if result["rank"] != float64(0) || result["bucket"] != "missing" || strings.Contains(result["text"].(string), full) {
		t.Fatal("no-context widened", result)
	}
	args["scope"] = Scope{Type: ScopeProject, Value: "benchmark-diagnostics-local"}
	args["include_all"] = true
	result = run()
	if result["rank"] != float64(1) || !strings.Contains(result["text"].(string), full) {
		t.Fatal(result)
	}
	args["expected_ids"] = []string{strconv.FormatInt(private, 10)}
	result = run()
	if result["bucket"] != "missing" || strings.Contains(result["text"].(string), "expected_id=") {
		t.Fatal("include-all widened explicit expected scope", result)
	}
	args["expected_ids"] = []string{strconv.FormatInt(local, 10)}
	args["operation"] = "benchmark-hard-negative"
	for _, key := range []string{"suite", "task", "expected", "got", "error"} {
		args[key] = ""
	}
	result = run()
	if !strings.Contains(result["line"].(string), full) || strings.Contains(result["line"].(string), "private-content") {
		t.Fatal(result)
	}
	exec(`RESET ROLE; REVOKE SELECT ON memories FROM aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`)
	result = run()
	var artifact benchmarkHardNegative
	if err := json.Unmarshal([]byte(result["line"].(string)), &artifact); err != nil || artifact.MemoryError == "" || len(artifact.Candidates) != 0 {
		t.Fatal(result, err)
	}
	args["operation"] = "benchmark-miss"
	raw, _ := json.Marshal(args)
	frame, _ := bus.EncodeCommand("runtime", raw)
	if _, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame); status == bus.ModuleStatusOK {
		t.Fatal("revoked read became a miss")
	}
}
