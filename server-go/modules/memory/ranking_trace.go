package memory

import (
	"context"
	"sort"
)

type rankingTraceKey struct{}

// Tracing is request-local and opt-in at the diagnostic owner. Ordinary recall
// allocates no trace slices; a nested request cannot replace another's evidence.
func withRankingTrace(ctx context.Context) context.Context {
	if ctx.Value(rankingTraceKey{}) != nil {
		return ctx
	}
	return context.WithValue(ctx, rankingTraceKey{}, true)
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
	if len(out) > limit {
		out = out[:limit]
	}
	if observed != nil {
		for i := range out {
			recordRankingStep(ctx, &out[i], "rrf60", observed[out[i].ID]...)
		}
	}
	return out
}
