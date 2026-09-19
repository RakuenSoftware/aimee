package memory

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"sort"
	"strings"
	"time"

	store "github.com/JBailes/aimee/server-go/db"
	"github.com/JBailes/aimee/server-go/modules/egress"
)

// EvaluationCorpus is the labelled retrieval corpus used by the standalone
// evaluator. It is not a public command: its caller supplies an isolated store.
type EvaluationCorpus struct {
	Version  int                 `json:"version"`
	Fixtures []EvaluationFixture `json:"fixtures"`
	Cases    []EvaluationCase    `json:"cases"`
}
type EvaluationFixture struct {
	FID     string `json:"fid"`
	Tier    string `json:"tier"`
	Kind    string `json:"kind"`
	Key     string `json:"key"`
	Content string `json:"content"`
}
type EvaluationCase struct {
	ID       string   `json:"id"`
	Query    string   `json:"query"`
	Expected []string `json:"expected"`
	Answer   string   `json:"answer,omitempty"`
}
type EvaluationScores struct {
	MRR      float64 `json:"mrr"`
	NDCG5    float64 `json:"ndcg_5"`
	NDCG10   float64 `json:"ndcg_10"`
	Recall5  float64 `json:"recall_5"`
	Recall10 float64 `json:"recall_10"`
	Cases    int     `json:"n_cases"`
}
type EvaluationResult struct {
	Suite         string           `json:"suite,omitempty"`
	Samples       int              `json:"samples,omitempty"`
	ExcludedCases map[string]int   `json:"excluded_cases,omitempty"`
	Status        string           `json:"status"`
	Scores        EvaluationScores `json:"metrics"`
	// Owner latency includes retrieval and governed embedding, but excludes
	// transport to the standalone evaluator. It is not a live KB hop measurement.
	LatenciesMS []float64 `json:"owner_latency_ms"`
}

// Validate rejects a changed/partial denominator before any fixture is stored.
func (c EvaluationCorpus) Validate() error {
	if err := c.ValidateFixtures(); err != nil {
		return err
	}
	if len(c.Cases) < 1 || len(c.Cases) > 4096 {
		return errors.New("evaluation requires corpus version 1, 1..4096 fixtures and 1..4096 cases")
	}
	fixtures := make(map[string]bool)
	for _, f := range c.Fixtures {
		fixtures[f.FID] = true
	}
	for _, row := range c.Cases {
		if strings.TrimSpace(row.Query) == "" || len(row.Expected) < 1 || len(row.Expected) > 128 {
			return errors.New("evaluation case requires a query and 1..128 relevance labels")
		}
		seen := make(map[string]bool)
		for _, fid := range row.Expected {
			if !fixtures[fid] || seen[fid] {
				return fmt.Errorf("evaluation case references unknown or repeated fixture %q", fid)
			}
			seen[fid] = true
		}
	}
	return nil
}

// ValidateFixtures is shared by retrieval and QA suites; QA need not have
// retrieval relevance labels, but must use the same complete fixture identity.
func (c EvaluationCorpus) ValidateFixtures() error {
	if c.Version != 1 || len(c.Fixtures) < 1 || len(c.Fixtures) > 4096 {
		return errors.New("evaluation requires version 1 and 1..4096 fixtures")
	}
	fixtures := make(map[string]bool)
	for _, f := range c.Fixtures {
		if f.FID == "" || fixtures[f.FID] || strings.TrimSpace(f.Key) == "" || strings.TrimSpace(f.Content) == "" || f.Tier == "" || f.Kind == "" {
			return errors.New("evaluation fixture has a missing field or duplicate fid")
		}
		fixtures[f.FID] = true
	}
	return nil
}

// EvaluateCorpus uses the production owner for all writes, derivation, versioned
// embedding, activation and retrieval. db MUST belong to a disposable evaluation
// session; the standalone evaluator obtains it from postgres.OpenEvaluationStore.
// No live-store retargeting, native memory structs or driver-specific SQL is used.
func EvaluateCorpus(ctx context.Context, db store.DB, executor egress.Executor, corpus EvaluationCorpus, command string) (EvaluationResult, error) {
	var result EvaluationResult
	if err := corpus.Validate(); err != nil {
		return result, err
	}
	module, err := NewEvaluationModule(ctx, db, executor)
	if err != nil {
		return result, err
	}
	ids, err := module.Seed(corpus.Fixtures, command)
	if err != nil {
		return result, err
	}
	result.LatenciesMS = make([]float64, 0, len(corpus.Cases))
	for _, row := range corpus.Cases {
		expected := make([]string, len(row.Expected))
		for i, fid := range row.Expected {
			expected[i] = ids[fid]
		}
		var score benchmarkScore
		start := time.Now()
		err := module.Call(StageCommand, "runtime", map[string]any{"operation": "benchmark-score", "query": row.Query, "expected_ids": expected}, &score)
		if err != nil {
			return EvaluationResult{}, err
		}
		result.LatenciesMS = append(result.LatenciesMS, float64(time.Since(start))/float64(time.Millisecond))
		result.Scores.MRR += score.MRR
		result.Scores.NDCG5 += score.NDCG5
		result.Scores.NDCG10 += score.NDCG10
		result.Scores.Recall5 += score.Recall5
		result.Scores.Recall10 += score.Recall10
	}
	n := float64(len(corpus.Cases))
	result.Scores.MRR /= n
	result.Scores.NDCG5 /= n
	result.Scores.NDCG10 /= n
	result.Scores.Recall5 /= n
	result.Scores.Recall10 /= n
	result.Scores.Cases = len(corpus.Cases)
	result.Status = "ok"
	return result, ctx.Err()
}

// FormatEvaluation keeps CLI presentation alongside the owner, including exact
// JSON field/profile handling. The latency is measured around this owner's
// retrieval, not a Server-to-KB network hop.
func FormatEvaluation(result EvaluationResult, path, format, fields, profile string) ([]byte, error) {
	if result.Status != "ok" || result.Scores.Cases < 1 || len(result.LatenciesMS) != result.Scores.Cases {
		return nil, errors.New("incomplete evaluation result")
	}
	suite := result.Suite
	if suite == "" {
		suite = "corpus"
	}
	s := result.Scores
	latency := append([]float64(nil), result.LatenciesMS...)
	sort.Float64s(latency)
	percentile := func(p float64) float64 { return latency[int(math.Ceil(float64(len(latency))*p))-1] }
	timing := map[string]any{"p50_ms": percentile(.5), "p95_ms": percentile(.95), "p99_ms": percentile(.99), "min_ms": latency[0], "max_ms": latency[len(latency)-1], "queries": len(latency)}
	if format == "text" {
		metadata := ""
		if result.Suite != "" {
			excluded, err := json.Marshal(result.ExcludedCases)
			if err != nil {
				return nil, err
			}
			metadata = fmt.Sprintf("Samples: %d\nExcluded cases: %s\nFixture policy: full-text-raw-query-v1\n", result.Samples, excluded)
		}
		return []byte(fmt.Sprintf("Memory Benchmark — %s: %s\n%sCases: %d\nMRR: %.6f\nnDCG@5: %.6f\nnDCG@10: %.6f\nRecall@5: %.6f\nRecall@10: %.6f\nLatency (owner): p50=%.2fms p95=%.2fms p99=%.2fms\n", suite, path, metadata, s.Cases, s.MRR, s.NDCG5, s.NDCG10, s.Recall5, s.Recall10, percentile(.5), percentile(.95), percentile(.99))), nil
	}
	if format != "json" {
		return nil, errors.New("evaluation format must be json or text")
	}
	view := map[string]any{"status": "ok", "suite": suite, "dataset": path,
		"metrics": map[string]any{"mrr": s.MRR, "ndcg_5": s.NDCG5, "ndcg_10": s.NDCG10, "recall_5": s.Recall5, "recall_10": s.Recall10, "cases": s.Cases},
		"latency": timing, "latency_scope": "owner", "route_buckets": map[string]any{}, "shape_buckets": map[string]any{}}
	if result.Suite != "" {
		view["samples"] = result.Samples
		view["excluded_cases"] = result.ExcludedCases
		view["fixture_policy"] = "full-text-raw-query-v1"
	}
	return FormatEvaluationJSON(view, fields, profile)
}

// FormatEvaluationJSON applies the same field/profile projection to every suite.
func FormatEvaluationJSON(view any, fields, profile string) ([]byte, error) {
	payload, err := json.Marshal(view)
	if err != nil {
		return nil, err
	}
	args := commandArgs{}
	if fields != "" {
		args["fields"], _ = json.Marshal(fields)
	}
	if profile != "" {
		args["profile"], _ = json.Marshal(profile)
	}
	output, err := memoryJSONOutput(payload, args)
	if err != nil {
		return nil, err
	}
	return []byte(output + "\n"), nil
}
