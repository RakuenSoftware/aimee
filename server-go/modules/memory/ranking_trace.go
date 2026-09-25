package memory

import (
	"context"
	"encoding/json"
	"reflect"
	"sort"
	"strconv"
	"sync"
)

type rankingTraceKey struct{}

// Tracing is request-local and opt-in at the diagnostic owner. Ordinary recall
// allocates no trace slices; a nested request cannot replace another's evidence.
func withRankingTrace(ctx context.Context) context.Context {
	if ctx.Value(rankingTraceKey{}) != nil {
		return ctx
	}
	return context.WithValue(context.WithValue(ctx, rankingTraceKey{}, true), rankingCaptureKey{}, newRankingCapture())
}
func rankingTraceEnabled(ctx context.Context) bool { return ctx.Value(rankingTraceKey{}) == true }

type rankingContribution struct {
	Arm   string  `json:"arm"`
	Rank  int     `json:"rank,omitempty"`
	Value float64 `json:"value"`
}
type rankingStep struct {
	Operation     string                `json:"operation"`
	Score         float64               `json:"score"`
	Contributions []rankingContribution `json:"contributions"`
}

// A step describes the score produced at that stage, not an additive bonus to
// previous stages. RRF consumes arm order, so summing earlier stages is wrong.
func recordRankingStep(ctx context.Context, r *Record, operation string, contributions ...rankingContribution) {
	if !rankingTraceEnabled(ctx) {
		return
	}
	// Copy before append: records can share earlier immutable evidence through
	// fusion/deduplication without allowing one result to mutate another.
	steps := make([]rankingStep, len(r.rankingSteps)+1)
	copy(steps, r.rankingSteps)
	steps[len(steps)-1] = rankingStep{Operation: operation, Score: r.retrievalScore, Contributions: contributions}
	r.rankingSteps = steps
	captureRankingCandidate(ctx, *r, "candidate")
}

func fuseRanked(ctx context.Context, lexical, semantic []Record, limit int, leftArm, rightArm string) []Record {
	var observed map[int64][]rankingContribution
	if rankingTraceEnabled(ctx) {
		observed = map[int64][]rankingContribution{}
	}
	arms := []string{leftArm, rightArm}
	scores := map[int64]float64{}
	records := map[int64]Record{}
	for arm, list := range [][]Record{lexical, semantic} {
		seen := map[int64]bool{}
		rank := 0
		for _, r := range list {
			if seen[r.ID] {
				continue
			}
			seen[r.ID] = true
			contribution := 1 / float64(60+rank+1)
			scores[r.ID] += contribution
			if observed != nil {
				observed[r.ID] = append(observed[r.ID], rankingContribution{Arm: arms[arm], Rank: rank + 1, Value: contribution})
			}
			if _, exists := records[r.ID]; !exists {
				records[r.ID] = r
			} else if observed != nil {
				prior := records[r.ID]
				for _, step := range r.rankingSteps {
					found := false
					for _, old := range prior.rankingSteps {
						if reflect.DeepEqual(old, step) {
							found = true
							break
						}
					}
					if !found {
						prior.rankingSteps = append(append([]rankingStep(nil), prior.rankingSteps...), step)
					}
				}
				records[r.ID] = prior
			}
			rank++
		}
	}
	out := make([]Record, 0, len(records))
	for _, r := range records {
		r.retrievalScore = scores[r.ID]
		out = append(out, r)
	}
	sort.Slice(out, func(i, j int) bool {
		a, b := scores[out[i].ID], scores[out[j].ID]
		if a == b {
			return out[i].ID < out[j].ID
		}
		return a > b
	})
	if observed != nil {
		for i := range out {
			recordRankingStep(ctx, &out[i], "rrf60", observed[out[i].ID]...)
		}
	}
	if len(out) > limit {
		for _, r := range out[max(0, limit):] {
			captureRankingCandidate(ctx, r, "caller_limit")
		}
		out = out[:max(0, limit)]
	}
	return out
}

// The capture lives in the invocation context, never in a store or global buffer.
// Only candidates already admitted by owner SQL enter it. Hidden rows are not
// enumerated to explain exclusion, and an SQL LIMIT never means exhaustive search.
type rankingCaptureKey struct{}
type rankingCandidate struct {
	ID           string               `json:"id"`
	Version      *MemoryRecordVersion `json:"source_version,omitempty"`
	SourceFamily string               `json:"source_family_state"`
	Eligibility  string               `json:"eligibility"`
	Disposition  string               `json:"disposition"`
	Steps        []rankingStep        `json:"steps"`
}
type rankingCapture struct {
	mu            sync.Mutex
	ID            string             `json:"trace_id"`
	SchemaVersion int                `json:"schema_version"`
	Universe      string             `json:"candidate_universe"`
	Exclusions    string             `json:"preselection_exclusions"`
	Limit         int                `json:"candidate_metadata_limit"`
	Truncated     bool               `json:"truncated"`
	Candidates    []rankingCandidate `json:"candidates"`
}

func newRankingCapture() *rankingCapture {
	id, _ := releaseToken()
	return &rankingCapture{ID: id, SchemaVersion: 1, Universe: "bounded_owner_admitted_candidates", Exclusions: "scope_lifecycle_policy_filtered_by_owner_not_enumerated", Limit: 256, Candidates: []rankingCandidate{}}
}
func captureRankingCandidate(ctx context.Context, r Record, disposition string) {
	c, _ := ctx.Value(rankingCaptureKey{}).(*rankingCapture)
	if c == nil {
		return
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	id := strconv.FormatInt(r.ID, 10)
	v := r.Version
	if v == nil {
		v = r.observedVersion
	}
	candidate := rankingCandidate{ID: id, Version: v, SourceFamily: "not_assessed", Eligibility: "owner_admitted", Disposition: disposition, Steps: append([]rankingStep(nil), r.rankingSteps...)}
	for i := range c.Candidates {
		if c.Candidates[i].ID == id {
			c.Candidates[i] = candidate
			return
		}
	}
	if len(c.Candidates) >= c.Limit {
		c.Truncated = true
		return
	}
	c.Candidates = append(c.Candidates, candidate)
}
func finishRankingCapture(ctx context.Context, selected []Record) *rankingCapture {
	c, _ := ctx.Value(rankingCaptureKey{}).(*rankingCapture)
	if c == nil {
		return nil
	}
	for _, r := range selected {
		captureRankingCandidate(ctx, r, "selected")
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	out := newRankingCapture()
	out.ID = c.ID
	out.Truncated = c.Truncated
	out.Candidates = append([]rankingCandidate(nil), c.Candidates...)
	for i := range out.Candidates {
		if out.Candidates[i].Disposition == "candidate" {
			out.Candidates[i].Disposition = "not_retained_after_ranking"
		}
	}
	for {
		raw, _ := json.Marshal(out)
		if len(raw) <= 128<<10 || len(out.Candidates) == 0 {
			break
		}
		out.Truncated = true
		out.Candidates = out.Candidates[:len(out.Candidates)-1]
	}
	return out
}
func recordNativeRank(ctx context.Context, r *Record, arm string, rank int, score float64, semantics string) {
	if !rankingTraceEnabled(ctx) {
		return
	}
	// Native scores describe an arm's ordering; they are not additional RRF votes.
	old := r.retrievalScore
	r.retrievalScore = score
	recordRankingStep(ctx, r, "native_"+semantics, rankingContribution{Arm: arm, Rank: rank, Value: score})
	r.retrievalScore = old
}
