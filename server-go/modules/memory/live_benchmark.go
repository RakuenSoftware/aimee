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
	"github.com/JBailes/aimee/server-go/internal/retrievalmetrics"
)

type liveBenchmarkCase struct {
	Query    string  `json:"query"`
	Expected []int64 `json:"expected_ids"`
}

type liveBenchmarkReceipt struct {
	Query     string           `json:"query"`
	Expected  []string         `json:"expected_ids"`
	Retrieved []string         `json:"retrieved_ids"`
	Scores    EvaluationScores `json:"metrics"`
	LatencyMS float64          `json:"owner_latency_ms"`
}

// Preserve integer tokens from a host-owned corpus file. Unlabelled queries
// measure latency but never enter the quality denominator.
func parseLiveBenchmarkCorpus(raw string) ([]liveBenchmarkCase, error) {
	var input struct {
		Queries []struct {
			Query    string            `json:"query"`
			Expected []json.RawMessage `json:"expected_ids"`
		} `json:"queries"`
	}
	if len(raw) > 1<<20 || json.Unmarshal([]byte(raw), &input) != nil || len(input.Queries) == 0 || len(input.Queries) > 256 {
		return nil, errors.New("corpus requires 1..256 queries in at most 1 MiB of JSON")
	}
	cases := make([]liveBenchmarkCase, len(input.Queries))
	for i, q := range input.Queries {
		if strings.TrimSpace(q.Query) == "" || len(q.Query) > 16384 || len(q.Expected) > 128 {
			return nil, errors.New("corpus requires nonempty queries up to 16384 bytes and at most 128 labels")
		}
		cases[i] = liveBenchmarkCase{Query: q.Query, Expected: []int64{}}
		seen := map[int64]bool{}
		for _, rawID := range q.Expected {
			token := string(rawID)
			if len(token) > 0 && token[0] == '"' {
				if json.Unmarshal(rawID, &token) != nil {
					return nil, errors.New("invalid label")
				}
			}
			id, err := strconv.ParseInt(token, 10, 64)
			if err != nil || id <= 0 || strconv.FormatInt(id, 10) != token || seen[id] {
				return nil, errors.New("labels must be unique positive integer IDs")
			}
			seen[id] = true
			cases[i].Expected = append(cases[i].Expected, id)
		}
	}
	return cases, nil
}

func handleLiveBenchmark(options handlerOptions, invocation bus.ModuleInvocation, _ string, args commandArgs) ([]byte, bus.ModuleStatus) {
	invalid := func(message string) ([]byte, bus.ModuleStatus) {
		return commandResult(commandError("invalid_argument", message))
	}
	for _, name := range []string{"fusion_state", "arm", "weight_profile"} {
		if _, ok := args[name]; ok {
			return invalid("benchmarks observe the configured owner; request policy overrides are unsupported")
		}
	}
	suite := args.stringOr("suite", "code-graph-fusion")
	switch suite {
	case "locomo", "longmemeval", "locomo-qa", "longmemeval-qa", "locomo-session-support", "locomo-misses", "longmemeval-misses":
		return commandResult(map[string]any{"status": "async-only", "suite": suite, "run_via": "aimee memory benchmark " + suite, "reason": "run with the isolated Go evaluation module"})
	case "code-graph-fusion", "memory", "corpus", "memory-retrieval", "live":
	default:
		return invalid("unsupported memory benchmark suite")
	}
	format := args.stringOr("format", "")
	if format != "" && format != "text" && format != "json" {
		return invalid("format must be text or json")
	}
	var cases []liveBenchmarkCase
	quality := "labelled_live"
	if suite == "code-graph-fusion" {
		if args["max_cases"] != nil || args["limit"] != nil {
			return invalid("code-graph-fusion evaluates every supplied query; case caps are unsupported")
		}
		raw, ok := args.stringValue("corpus_json")
		if !ok {
			return invalid("code-graph-fusion requires host-supplied corpus_json")
		}
		var err error
		cases, err = parseLiveBenchmarkCorpus(raw)
		if err != nil {
			return invalid(err.Error())
		}
	} else {
		if args.stringOr("corpus", "") != "" || args["corpus_json"] != nil {
			return invalid("live memory suites do not accept corpus files; use aimee-memory-eval for isolated corpus evaluation")
		}
		limit := 100
		for _, key := range []string{"limit", "max_cases"} {
			if _, exists := args[key]; exists {
				n, ok := args.number(key)
				if !ok || n < 1 || n > 100 || math.Trunc(n) != n {
					return invalid("live max_cases must be an integer between 1 and 100")
				}
				limit = int(n)
			}
		}
		response, status := benchmarkRead(options, invocation, args, DataRequest{Operation: "query-records", Mode: "eval", Limit: limit})
		if status != bus.ModuleStatusOK {
			return commandResult(commandError("unavailable", "live corpus read failed"))
		}
		for _, record := range response.Records {
			query := record.Key
			if query == "" {
				query = record.Content
			}
			if record.ID <= 0 || strings.TrimSpace(query) == "" || len(query) > 16384 {
				return invalid("live corpus contains an invalid or oversized query")
			}
			cases = append(cases, liveBenchmarkCase{Query: query, Expected: []int64{record.ID}})
		}
		if len(cases) == 0 {
			return commandResult(commandError("not_found", "no visible live evaluation records"))
		}
		quality = "self_retrieval"
	}
	parent := options.dataContext
	if parent == nil {
		parent = context.Background()
	}
	ctx, cancel := context.WithTimeout(parent, invocation.Remaining(120*time.Second))
	defer cancel()
	options.dataContext = ctx
	receipts := make([]liveBenchmarkReceipt, 0, len(cases))
	scores := EvaluationScores{}
	latencies := make([]float64, 0, len(cases))
	for _, c := range cases {
		start := time.Now()
		response, status := benchmarkRead(options, invocation, args, DataRequest{Operation: "search", Query: c.Query, Limit: 20})
		if status != bus.ModuleStatusOK {
			return commandResult(commandError("unavailable", "benchmark retrieval failed; no partial result published"))
		}
		elapsed := float64(time.Since(start)) / float64(time.Millisecond)
		ids := make([]int64, len(response.Records))
		seen := map[int64]bool{}
		r := liveBenchmarkReceipt{Query: c.Query, Expected: []string{}, Retrieved: []string{}, LatencyMS: elapsed}
		if len(ids) > 20 {
			return nil, bus.ModuleStatusInternal
		}
		for i, record := range response.Records {
			if record.ID <= 0 || seen[record.ID] {
				return nil, bus.ModuleStatusInternal
			}
			seen[record.ID] = true
			ids[i] = record.ID
			r.Retrieved = append(r.Retrieved, strconv.FormatInt(record.ID, 10))
		}
		for _, id := range c.Expected {
			r.Expected = append(r.Expected, strconv.FormatInt(id, 10))
		}
		if len(c.Expected) > 0 {
			r.Scores.Cases = 1
			r.Scores.MRR, r.Scores.NDCG5, r.Scores.Recall5 = retrievalmetrics.Score(ids, c.Expected, 5)
			_, r.Scores.NDCG10, r.Scores.Recall10 = retrievalmetrics.Score(ids, c.Expected, 10)
			scores.Cases++
			scores.MRR += r.Scores.MRR
			scores.NDCG5 += r.Scores.NDCG5
			scores.NDCG10 += r.Scores.NDCG10
			scores.Recall5 += r.Scores.Recall5
			scores.Recall10 += r.Scores.Recall10
		}
		receipts = append(receipts, r)
		latencies = append(latencies, elapsed)
	}
	if scores.Cases > 0 {
		n := float64(scores.Cases)
		scores.MRR /= n
		scores.NDCG5 /= n
		scores.NDCG10 /= n
		scores.Recall5 /= n
		scores.Recall10 /= n
	} else {
		quality = "unlabelled"
	}
	sort.Float64s(latencies)
	percentile := func(p float64) float64 { return latencies[int(math.Ceil(float64(len(latencies))*p))-1] }
	view := map[string]any{"status": "ok", "suite": suite, "fusion_state": "instance", "queries": len(cases), "labelled": scores.Cases, "errors": 0, "metrics": scores, "case_results": receipts,
		"latency":       map[string]any{"p50_ms": percentile(.5), "p95_ms": percentile(.95), "p99_ms": percentile(.99), "min_ms": latencies[0], "max_ms": latencies[len(latencies)-1], "queries": len(cases)},
		"latency_scope": "owner", "quality_basis": quality, "release_gate": false, "snapshot_isolated": false, "metrics_denominator": "labelled_queries"}
	if format != "" {
		var output []byte
		var err error
		if format == "json" {
			output, err = FormatEvaluationJSON(view, args.stringOr("fields", ""), args.stringOr("profile", ""))
		} else {
			output = []byte(fmt.Sprintf("Memory benchmark — %s\nQueries: %d; labelled: %d; basis: %s\nMRR: %.6f; nDCG@10: %.6f; Recall@10: %.6f\nOwner latency p50=%.3fms p95=%.3fms p99=%.3fms\nLive diagnostic; not a frozen release gate.\n", suite, len(cases), scores.Cases, quality, scores.MRR, scores.NDCG10, scores.Recall10, percentile(.5), percentile(.95), percentile(.99)))
		}
		if err != nil {
			return nil, bus.ModuleStatusInternal
		}
		view = map[string]any{"status": "ok", "output": string(output)}
	}
	raw, err := json.Marshal(view)
	if err != nil || len(raw) > benchmarkReportLimit {
		return commandResult(commandError("capacity_exceeded", "benchmark report exceeds 1 MiB"))
	}
	return commandResult(json.RawMessage(raw))
}
