package memory

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
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
	if c.Version != 1 || len(c.Fixtures) < 1 || len(c.Fixtures) > 4096 || len(c.Cases) < 1 || len(c.Cases) > 4096 {
		return errors.New("evaluation requires corpus version 1, 1..4096 fixtures and 1..4096 cases")
	}
	fixtures := make(map[string]bool)
	for _, f := range c.Fixtures {
		if f.FID == "" || fixtures[f.FID] || strings.TrimSpace(f.Key) == "" || strings.TrimSpace(f.Content) == "" || f.Tier == "" || f.Kind == "" {
			return errors.New("evaluation fixture has a missing field or duplicate fid")
		}
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

// EvaluateCorpus uses the production owner for all writes, derivation, versioned
// embedding, activation and retrieval. db MUST belong to a disposable evaluation
// session; the standalone evaluator obtains it from postgres.OpenEvaluationStore.
// No live-store retargeting, native memory structs or driver-specific SQL is used.
func EvaluateCorpus(ctx context.Context, db store.DB, executor egress.Executor, corpus EvaluationCorpus, command string) (EvaluationResult, error) {
	var result EvaluationResult
	if err := corpus.Validate(); err != nil {
		return result, err
	}
	if db == nil || strings.TrimSpace(command) == "" {
		return result, errors.New("evaluation requires an isolated store and an embedder")
	}
	if EmbedIsHTTP(command) && executor == nil {
		return result, errors.New("evaluation HTTP embedder requires governed egress")
	}
	data, err := NewPostgresDataStore(db, PlacementKB)
	if err != nil {
		return result, err
	}
	backend := data.(*postgresDataStore)
	backend.requireSemantic = true
	handler := NewHandler(executor, WithDataStore(PlacementKB, data), func(options *handlerOptions) { options.dataContext = ctx })
	call := func(stage uint32, verb string, request any, target any) error {
		if err := ctx.Err(); err != nil {
			return err
		}
		raw, err := json.Marshal(request)
		if err != nil {
			return err
		}
		if stage == StageCommand {
			raw, err = bus.EncodeCommand(verb, raw)
			if err != nil {
				return err
			}
		}
		raw, status := handler(bus.ModuleInvocation{StageID: stage}, raw)
		if status != bus.ModuleStatusOK {
			return fmt.Errorf("evaluation %s: owner status %d", verb, status)
		}
		if stage == StageCommand {
			raw, err = bus.DecodeCommandResult(raw)
			if err != nil {
				return err
			}
		}
		var receipt struct {
			Status  string `json:"status"`
			Message string `json:"message"`
		}
		if err := json.Unmarshal(raw, &receipt); err != nil {
			return err
		}
		if receipt.Status == "error" {
			return fmt.Errorf("evaluation %s: %s", verb, receipt.Message)
		}
		return json.Unmarshal(raw, target)
	}
	ids := make(map[string]string)
	unique := make(map[int64]bool)
	confidence := .9
	for _, f := range corpus.Fixtures {
		var response DataResponse
		err := call(StageData, "seed", DataRequest{Operation: "insert-epistemic", Tier: f.Tier, Kind: f.Kind, Key: f.Key, Content: f.Content, Confidence: &confidence, SessionID: "corpus", EpistemicKind: "world_fact", IncludeAll: true}, &response)
		if err != nil {
			return result, err
		}
		if len(response.Records) != 1 || response.Records[0].ID <= 0 || unique[response.Records[0].ID] {
			return result, errors.New("evaluation fixture was rejected or aliases an existing fixture")
		}
		id := response.Records[0].ID
		ids[f.FID], unique[id] = strconv.FormatInt(id, 10), true
	}
	// Drain metadata with the same transaction/context and derivation as the
	// production worker. Finish this before freezing the version's input set.
	for {
		if err := ctx.Err(); err != nil {
			return result, err
		}
		tx, err := db.Begin(ctx)
		if err != nil {
			return result, err
		}
		worked := false
		err = func() error {
			defer tx.Rollback(context.Background())
			if err := sharedIndexContext(ctx, tx); err != nil {
				return err
			}
			bound := *backend
			bound.db = tx
			var err error
			worked, err = bound.indexSharedRecord(ctx)
			if err != nil {
				return err
			}
			return tx.Commit(ctx)
		}()
		if err != nil {
			return result, err
		}
		if !worked {
			break
		}
	}
	var ready struct {
		Ready  bool `json:"ready"`
		Failed int  `json:"failed"`
	}
	if err := call(StageCommand, "reembed_start", map[string]any{"version": "evaluation", "embedding_command": command}, &ready); err != nil {
		return result, err
	}
	if !ready.Ready || ready.Failed != 0 {
		return result, fmt.Errorf("evaluation corpus embedding incomplete (ready=%t, failed=%d)", ready.Ready, ready.Failed)
	}
	var activated struct {
		Status string `json:"status"`
	}
	if err := call(StageCommand, "reembed_cutover", map[string]any{}, &activated); err != nil {
		return result, err
	}
	if activated.Status != "ok" {
		return result, errors.New("evaluation embedding activation failed")
	}
	result.LatenciesMS = make([]float64, 0, len(corpus.Cases))
	for _, row := range corpus.Cases {
		expected := make([]string, len(row.Expected))
		for i, fid := range row.Expected {
			expected[i] = ids[fid]
		}
		var score benchmarkScore
		start := time.Now()
		err := call(StageCommand, "runtime", map[string]any{"operation": "benchmark-score", "query": row.Query, "expected_ids": expected}, &score)
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
