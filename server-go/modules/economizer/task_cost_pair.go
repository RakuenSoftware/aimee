package economizer

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"math"
	"sort"
	"strconv"
)

type TaskCostCorpusItem struct {
	Task           string `json:"task_id"`
	TaskDigest     string `json:"task_sha256"`
	VerifierDigest string `json:"verifier_sha256"`
}
type TaskCostArm struct {
	Cost            TaskCostRequest `json:"cost"`
	Completion      string          `json:"completion"` // completed, failed, unknown
	QualityPPM      *int            `json:"quality_ppm"`
	QualityEvidence string          `json:"quality_evidence_ref"`
}
type TaskCostPairRequest struct {
	SchemaVersion int                  `json:"schema_version"`
	CorpusDigest  string               `json:"corpus_sha256"`
	Corpus        []TaskCostCorpusItem `json:"corpus"`
	Baseline      []TaskCostArm        `json:"baseline"`
	Candidate     []TaskCostArm        `json:"candidate"`
}
type TaskCostPairReport struct {
	SchemaVersion      int              `json:"schema_version"`
	CorpusDigest       string           `json:"corpus_sha256"`
	Pairs              int              `json:"pairs"`
	BaselineKnown      string           `json:"baseline_known_nanodollars"`
	CandidateKnown     string           `json:"candidate_known_nanodollars"`
	KnownDifference    string           `json:"baseline_minus_candidate_nanodollars"`
	BaselineComplete   int              `json:"baseline_declared_complete"`
	CandidateComplete  int              `json:"candidate_declared_complete"`
	QualityRegressions int              `json:"declared_quality_regressions"`
	UnknownQuality     int              `json:"unknown_quality_pairs"`
	IncompleteCosts    int              `json:"incomplete_cost_pairs"`
	Baseline           []TaskCostReport `json:"baseline"`
	Candidate          []TaskCostReport `json:"candidate"`
	QualityEvidence    []string         `json:"quality_evidence_refs"`
	Gate               string           `json:"release_gate"`
	Interpretation     string           `json:"interpretation"`
}

func validCostDigest(s string) bool {
	b, e := hex.DecodeString(s)
	return e == nil && len(b) == 32 && hex.EncodeToString(b) == s
}

// PairTaskCosts fixes the task population before comparing arms. Missing or
// duplicate tasks are refused, not silently intersected (which cherry-picks
// successes). Caller declarations alone never satisfy the quality release gate.
func PairTaskCosts(req TaskCostPairRequest) (TaskCostPairReport, error) {
	out := TaskCostPairReport{SchemaVersion: 1, Baseline: []TaskCostReport{}, Candidate: []TaskCostReport{}, QualityEvidence: []string{}, Gate: "unqualified_requires_independent_quality_and_coverage_verification", Interpretation: "known_cost_difference_not_claimed_savings; actual_and_estimated_costs_remain_separate"}
	bad := errors.New("invalid frozen paired task population")
	n := len(req.Corpus)
	if req.SchemaVersion != 1 || n == 0 || n > 512 || len(req.Baseline) != n || len(req.Candidate) != n {
		return out, bad
	}
	corpus := append([]TaskCostCorpusItem(nil), req.Corpus...)
	sort.Slice(corpus, func(i, j int) bool { return corpus[i].Task < corpus[j].Task })
	for i, item := range corpus {
		if item.Task == "" || len(item.Task) > 256 || !validCostDigest(item.TaskDigest) || !validCostDigest(item.VerifierDigest) || (i > 0 && corpus[i-1].Task == item.Task) {
			return out, bad
		}
	}
	raw, _ := json.Marshal(corpus)
	digest := sha256.Sum256(raw)
	out.CorpusDigest = hex.EncodeToString(digest[:])
	if req.CorpusDigest != out.CorpusDigest {
		return out, bad
	}
	arms := [2]map[string]TaskCostArm{{}, {}}
	for i, arm := range [][]TaskCostArm{req.Baseline, req.Candidate} {
		for _, task := range arm {
			if _, ok := arms[i][task.Cost.Task]; ok {
				return out, bad
			}
			if task.Completion != "completed" && task.Completion != "failed" && task.Completion != "unknown" {
				return out, bad
			}
			if task.QualityPPM != nil && (*task.QualityPPM < 0 || *task.QualityPPM > 1000000 || task.QualityEvidence == "") {
				return out, bad
			}
			arms[i][task.Cost.Task] = task
		}
	}
	var totals [2]int64
	for _, item := range corpus {
		a, ok := arms[0][item.Task]
		b, okB := arms[1][item.Task]
		if !ok || !okB {
			return out, bad
		}
		var reports [2]TaskCostReport
		for i, arm := range []TaskCostArm{a, b} {
			report, err := ReportTaskCost(arm.Cost)
			if err != nil {
				return out, err
			}
			reports[i] = report
			cost, _ := costInteger(report.KnownTotal)
			if totals[i] > math.MaxInt64-cost {
				return out, bad
			}
			totals[i] += cost
			if arm.QualityEvidence != "" {
				out.QualityEvidence = append(out.QualityEvidence, arm.QualityEvidence)
			}
		}
		out.Baseline = append(out.Baseline, reports[0])
		out.Candidate = append(out.Candidate, reports[1])
		out.Pairs++
		if a.Completion == "completed" {
			out.BaselineComplete++
		}
		if b.Completion == "completed" {
			out.CandidateComplete++
		}
		if a.QualityPPM == nil || b.QualityPPM == nil || a.Completion == "unknown" || b.Completion == "unknown" {
			out.UnknownQuality++
		} else if *b.QualityPPM < *a.QualityPPM || (a.Completion == "completed" && b.Completion != "completed") {
			out.QualityRegressions++
		}
		if reports[0].Coverage == "incomplete" || reports[1].Coverage == "incomplete" {
			out.IncompleteCosts++
		}
	}
	out.BaselineKnown = strconv.FormatInt(totals[0], 10)
	out.CandidateKnown = strconv.FormatInt(totals[1], 10)
	out.KnownDifference = strconv.FormatInt(totals[0]-totals[1], 10)
	sort.Strings(out.QualityEvidence)
	return out, nil
}
