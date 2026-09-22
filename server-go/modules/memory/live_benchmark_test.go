package memory

import (
	"context"
	"encoding/json"
	"errors"
	"math"
	"strings"
	"testing"
)

type liveBenchmarkStore struct {
	benchmarkDiagnosticStore
	queries []string
	failAt  int
}

func (s *liveBenchmarkStore) Search(ctx context.Context, scope Scope, query, kind, tier string, limit int) ([]Record, error) {
	s.queries = append(s.queries, query)
	if s.failAt == len(s.queries) {
		return nil, errors.New("injected retrieval outage")
	}
	return s.benchmarkDiagnosticStore.Search(ctx, scope, query, kind, tier, limit)
}
func TestLiveBenchmarkOwner(t *testing.T) {
	const id = int64(9007199254740993)
	s := &liveBenchmarkStore{benchmarkDiagnosticStore: benchmarkDiagnosticStore{rows: []Record{{ID: 1}, {ID: 2}, {ID: 3}, {ID: 4}, {ID: 5}, {ID: id}}}}
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, s)))
	query := strings.Repeat("完整 ", 1000) + "last term"
	corpus, _ := json.Marshal(map[string]any{"queries": []any{
		map[string]any{"query": query, "expected_ids": []int64{id}},
		map[string]any{"query": "unlabelled", "expected_ids": []int64{}},
	}})
	run := func(extra map[string]any) map[string]any {
		t.Helper()
		args := map[string]any{"suite": "code-graph-fusion", "corpus_json": string(corpus), "project": "local"}
		for k, v := range extra {
			args[k] = v
		}
		raw, _ := json.Marshal(args)
		return runPublicCommand(t, client, "benchmark", string(raw))
	}
	r := run(nil)
	metrics := r["metrics"].(map[string]any)
	if r["status"] != "ok" || r["queries"] != float64(2) || r["labelled"] != float64(1) || r["release_gate"] != false || r["latency_scope"] != "owner" || metrics["mrr"] != 1.0/6 || math.Abs(metrics["ndcg_10"].(float64)-1/math.Log2(7)) > 1e-12 {
		t.Fatal(r)
	}
	cases := r["case_results"].([]any)
	if cases[0].(map[string]any)["retrieved_ids"].([]any)[5] != "9007199254740993" || cases[0].(map[string]any)["query"] != query || s.queries[0] != query {
		t.Fatal("lost integer or full query")
	}
	for _, mode := range []string{"text", "json"} {
		r = run(map[string]any{"format": mode})
		if r["status"] != "ok" || r["output"] == "" {
			t.Fatal(r)
		}
	}
	before := len(s.queries)
	for _, bad := range []map[string]any{
		{"max_cases": 1}, {"arm": "fusion-off"}, {"fusion_state": "off"}, {"weight_profile": "native"},
		{"suite": "unknown"}, {"corpus_json": `{"queries":[]}`},
		{"corpus_json": `{"queries":[{"query":"x","expected_ids":[1,1]}]}`},
		{"corpus_json": `{"queries":[{"query":"x","expected_ids":[1.5]}]}`},
		{"corpus_json": `{"queries":[{"query":"x","expected_ids":[9223372036854775808]}]}`},
		{"corpus_json": `{"queries":[{"query":"x"},{"query":""}]}`},
	} {
		if r = run(bad); r["kind"] != "invalid_argument" {
			t.Fatal(bad, r)
		}
	}
	if len(s.queries) != before {
		t.Fatal("malformed corpus reached owner retrieval")
	}
	s.failAt = len(s.queries) + 2
	if r = run(nil); r["kind"] != "unavailable" || r["metrics"] != nil || r["case_results"] != nil {
		t.Fatal("partial benchmark published", r)
	}
	s.failAt = 0
	if r = run(map[string]any{"corpus_json": `{"queries":[{"query":"x","expected_ids":[]}]}`}); r["quality_basis"] != "unlabelled" || r["labelled"] != float64(0) {
		t.Fatal(r)
	}
	for _, args := range []string{`{"suite":"live","max_cases":0}`, `{"suite":"live","max_cases":101}`, `{"suite":"live","max_cases":1.5}`, `{"suite":"live","corpus":"file"}`} {
		if r = runPublicCommand(t, client, "benchmark", args); r["kind"] != "invalid_argument" {
			t.Fatal(args, r)
		}
	}
	if r = runPublicCommand(t, client, "benchmark", `{"suite":"locomo"}`); r["status"] != "async-only" {
		t.Fatal(r)
	}
}
