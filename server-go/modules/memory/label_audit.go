package memory

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"sort"
	"strconv"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
)

type labelCase struct {
	query    string
	expected int64
	rows     []Diagnostic
}

func parseAuditLabels(text string) ([]labelCase, error) {
	if len(text) > 1<<20 {
		return nil, errors.New("labels exceed 1 MiB")
	}
	scanner := bufio.NewScanner(strings.NewReader(text))
	scanner.Buffer(make([]byte, 4096), 1<<20)
	result := []labelCase{}
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		parts := strings.Split(line, "\t")
		if len(parts) != 2 || strings.TrimSpace(parts[0]) == "" {
			return nil, errors.New("labels require query<TAB>positive memory ID")
		}
		idText := strings.TrimSpace(parts[1])
		id, err := strconv.ParseInt(idText, 10, 64)
		if err != nil || id <= 0 || strconv.FormatInt(id, 10) != idText {
			return nil, errors.New("invalid label memory ID")
		}
		result = append(result, labelCase{query: strings.TrimSpace(parts[0]), expected: id})
		if len(result) > 4096 {
			return nil, errors.New("labels exceed 4096 cases")
		}
	}
	if err := scanner.Err(); err != nil {
		return nil, err
	}
	if len(result) == 0 {
		return nil, errors.New("labels contain no cases")
	}
	return result, nil
}

type auditMultipliers [9]float64

var auditFeatureNames = [9]string{"lexical", "coverage", "entity", "temporal", "evidence", "semantic", "state", "intent", "confidence"}

func auditFeatures(p DiagnosticParts) [9]float64 {
	return [9]float64{p.Lexical, p.Coverage, p.Entity, p.Temporal, p.Evidence, p.Semantic, p.State, p.Intent, p.Confidence}
}
func auditRescore(p DiagnosticParts, weights auditMultipliers) float64 {
	score := p.Salience + p.Surprise + p.PageRank + p.RetrievalBase + p.GraphScore*p.GraphWeight
	for i, value := range auditFeatures(p) {
		score += value * weights[i]
	}
	return score
}
func auditRank(rows []Diagnostic, expected int64) int {
	for i, row := range rows {
		if row.Memory.ID == expected {
			return i + 1
		}
	}
	return 0
}
func auditScore(cases []labelCase, weights *auditMultipliers, limit int) (float64, int) {
	mrr, hits := 0.0, 0
	for _, row := range cases {
		candidates := row.rows
		if weights != nil {
			candidates = append([]Diagnostic(nil), candidates...)
			sort.SliceStable(candidates, func(i, j int) bool {
				return auditRescore(candidates[i].Parts, *weights) > auditRescore(candidates[j].Parts, *weights)
			})
		}
		rank := auditRank(candidates, row.expected)
		if rank > 0 {
			mrr += 1 / float64(rank)
			if rank <= limit {
				hits++
			}
		}
	}
	return mrr / float64(len(cases)), hits
}
func calibrateAudit(ctx context.Context, cases []labelCase, limit, rounds int) (auditMultipliers, float64, int, error) {
	best := auditMultipliers{1, 1, 1, 1, 1, 1, 1, 1, 1}
	bestMRR, _ := auditScore(cases, &best, limit)
	for round := 0; round < rounds; round++ {
		for f := range best {
			if err := ctx.Err(); err != nil {
				return best, 0, 0, err
			}
			value, score := best[f], bestMRR
			for _, candidate := range []float64{.5, .75, 1, 1.25, 1.5, 1.75, 2} {
				best[f] = candidate
				trial, _ := auditScore(cases, &best, limit)
				if trial > score+1e-9 {
					score, value = trial, candidate
				}
			}
			best[f], bestMRR = value, score
		}
	}
	mrr, hits := auditScore(cases, &best, limit)
	return best, mrr, hits, nil
}

// Labelled analysis uses only the owner-returned candidate pool. Gold IDs never
// enter candidate generation; absent labels remain misses. Calibration is an
// offline diagnostic experiment, not a deployment setting for the Go ranker.
func handleLabelAudit(options handlerOptions, invocation bus.ModuleInvocation, verb string, args commandArgs) ([]byte, bus.ModuleStatus) {
	format := args.stringOr("format", "text")
	if format != "json" && format != "text" {
		return commandResult(commandError("invalid_argument", "format must be json or text"))
	}
	labels, ok := args.stringValue("labels_tsv")
	if !ok {
		return commandResult(commandError("invalid_argument", "labels_tsv required"))
	}
	cases, err := parseAuditLabels(labels)
	if err != nil {
		return commandResult(commandError("invalid_argument", err.Error()))
	}
	limit, pool, rounds := args.integer("limit", 10), args.integer("candidate_limit", 24), args.integer("rounds", 2)
	if limit < 1 || limit > 64 || pool < limit || pool > 64 || rounds < 0 || rounds > 8 {
		return commandResult(commandError("invalid_argument", "require 1 <= limit <= candidate_limit <= 64 and rounds 0..8"))
	}
	if args.boolean("apply_config") {
		return commandResult(commandError("unsupported_mode", "diagnostic multipliers are not a serving policy; automatic deployment is unavailable"))
	}
	for i := range cases {
		response, status := benchmarkRead(options, invocation, args, DataRequest{Operation: "diagnose", Query: cases[i].query, Limit: pool})
		if status != bus.ModuleStatusOK {
			return nil, status
		}
		if len(response.Diagnostics) > pool {
			return nil, bus.ModuleStatusInternal
		}
		seen := map[int64]bool{}
		for _, row := range response.Diagnostics {
			if row.Memory.ID <= 0 || seen[row.Memory.ID] {
				return nil, bus.ModuleStatusInternal
			}
			seen[row.Memory.ID] = true
			if value := auditRescore(row.Parts, auditMultipliers{1, 1, 1, 1, 1, 1, 1, 1, 1}); math.IsNaN(value) || math.IsInf(value, 0) {
				return nil, bus.ModuleStatusInternal
			}
		}
		cases[i].rows = response.Diagnostics
	}
	mrr, hits := auditScore(cases, nil, limit)
	result := map[string]any{"status": "ok", "labels": args.stringOr("labels", ""), "case_count": len(cases), "mrr": mrr, "recall_at_k": float64(hits) / float64(len(cases)), "candidate_limit": pool, "limit": limit, "policy": "owner-candidate-order-v1"}
	if verb == "audit" {
		buckets := map[string]int{}
		for _, row := range cases {
			rank := auditRank(row.rows, row.expected)
			if rank > 0 && rank <= limit {
				continue
			}
			if rank == 0 {
				buckets["missing"]++
				continue
			}
			buckets[benchmarkFailureBucket(row.query, &row.rows[rank-1])]++
		}
		result["buckets"] = buckets
	} else {
		ctx := options.dataContext
		if ctx == nil {
			ctx = context.Background()
		}
		weights, trial, trialHits, err := calibrateAudit(ctx, cases, limit, rounds)
		if err != nil {
			return nil, bus.ModuleStatusInternal
		}
		multipliers := map[string]float64{}
		for i, name := range auditFeatureNames {
			multipliers[name] = weights[i]
		}
		result["policy"] = "diagnostic-multipliers-v1"
		result["deployable"] = false
		result["applied_config"] = false
		result["baseline_mrr"] = mrr
		result["mrr"] = trial
		result["recall_at_k"] = float64(trialHits) / float64(len(cases))
		result["multipliers"] = multipliers
	}
	// The host only transports/render bytes, including any requested artifact.
	profile, err := json.MarshalIndent(result, "", "  ")
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	var output []byte
	if format == "json" {
		output, err = FormatEvaluationJSON(result, args.stringOr("fields", ""), args.stringOr("profile", ""))
	} else {
		output = append(profile, '\n')
	}
	if err != nil {
		return commandResult(commandError("invalid_argument", fmt.Sprint(err)))
	}
	return commandResult(map[string]any{"status": "ok", "output": string(output), "artifact": string(profile) + "\n"})
}
