package memory

import (
	"context"
	"encoding/json"
	"math"
	"reflect"
	"testing"
)

func TestObservedRankingTrace(t *testing.T) {
	for i := 0; i < 16; i++ {
		t.Run(string(rune('a'+i)), func(t *testing.T) {
			t.Parallel()
			ctx := withRankingTrace(context.Background())
			left := []Record{{ID: 1, Content: "unrelated lexical content"}, {ID: 1}, {ID: 2}}
			right := []Record{{ID: 3, Content: "dense only"}, {ID: 2}}
			plain := fusePersonal(left, right, 3)
			observed := fuseRanked(ctx, left, right, 3, "lexical", "semantic")
			for j, r := range observed {
				if r.ID != plain[j].ID || r.retrievalScore != plain[j].retrievalScore {
					t.Fatal("trace changed rank", r, plain[j])
				}
				p := diagnosticFor(r, "a query absent from every row").Parts
				if p.ScoreEvidence != "observed_ranking_steps" || p.Total != r.retrievalScore || p.Lexical != 0 || p.Coverage != 0 {
					t.Fatal(p)
				}
				step := p.RankingSteps[len(p.RankingSteps)-1]
				sum := 0.0
				for _, c := range step.Contributions {
					sum += c.Value
				}
				if math.Abs(sum-step.Score) > 1e-15 {
					t.Fatal("contributions differ from actual score", step)
				}
				if r.ID == 3 && (len(step.Contributions) != 1 || step.Contributions[0].Arm != "semantic" || step.Contributions[0].Rank != 1) {
					t.Fatal("invented lexical vote", step)
				}
				if r.ID == 2 && (step.Contributions[0].Rank != 2 || step.Contributions[1].Rank != 2) {
					t.Fatal("duplicate changed arm rank", step)
				}
				rows := diagnosticTraceRows([]Diagnostic{{Memory: r, Parts: p}}, []publicDiagnostic{{Memory: publicMemoryRecord{ID: r.ID}, Parts: p}})
				contributions := rows[0]["feature_contributions"].(map[string]float64)
				sum = 0
				for _, v := range contributions {
					sum += v
				}
				if math.Abs(sum-r.retrievalScore) > 1e-15 {
					t.Fatal("trace overcounted prior stages", contributions)
				}
			}
			before, _ := json.Marshal(observed[0].rankingSteps)
			// A second stage retains the actual earlier decision but consumes its rank,
			// not its score; it must not rewrite the prior response's trace.
			again := fuseRanked(ctx, observed, []Record{{ID: 4}, {ID: 2}}, 4, "prior_candidates", "graph")
			for _, r := range again {
				if r.ID == 4 && (r.rankingSteps[0].Contributions[0].Arm != "graph" || len(r.rankingSteps) != 1) {
					t.Fatal(r)
				}
			}
			after, _ := json.Marshal(observed[0].rankingSteps)
			if string(before) != string(after) {
				t.Fatal("nested trace mutated prior evidence")
			}
			if got := fuseRanked(context.Background(), left, right, 3, "lexical", "semantic"); !reflect.DeepEqual(got, plain) {
				t.Fatal("ordinary recall acquired trace state")
			}
		})
	}
}

func TestDiagnosticScoreEvidenceSeparatesEstimates(t *testing.T) {
	r := Record{ID: 1, Key: "query", Content: "query"}
	estimate := diagnosticFor(r, "query").Parts
	if estimate.ScoreEvidence != "text_match_estimate" || estimate.Lexical == 0 {
		t.Fatal(estimate)
	}
	r.retrievalScore = .0123
	r.pageRankApplied = true
	actual := diagnosticFor(r, "query").Parts
	if actual.ScoreEvidence != "observed_final_score" || actual.Total != .0123 || actual.Lexical != 0 || actual.Coverage != 0 {
		t.Fatal(actual)
	}
}

func TestRankingTracePreviewCompatibility(t *testing.T) {
	ctx := context.WithValue(context.Background(), rankingTraceKey{}, false)
	if rankingTraceEnabled(withRankingTrace(ctx)) {
		t.Fatal("preview enabled diagnostic capture")
	}
	r := Record{Key: "deployment", Content: "the project uses docker for deployment", retrievalScore: .0123}
	p := diagnosticFor(r, "docker deployment").Parts
	if p.Total != .35 || p.ScoreEvidence != "text_match_estimate" {
		t.Fatal("preview compatibility score changed", p)
	}
}

func BenchmarkRankingTrace(b *testing.B) {
	left, right := make([]Record, 64), make([]Record, 64)
	for i := range left {
		left[i].ID = int64(i + 1)
		right[i].ID = int64(i + 33)
	}
	for _, mode := range []string{"baseline", "disabled", "enabled"} {
		b.Run(mode, func(b *testing.B) {
			ctx := context.Background()
			if mode == "enabled" {
				ctx = withRankingTrace(ctx)
			}
			b.ReportAllocs()
			for b.Loop() {
				if mode == "baseline" {
					fusePersonal(left, right, 64)
				} else {
					fuseRanked(ctx, left, right, 64, "lexical", "semantic")
				}
			}
		})
	}
}
