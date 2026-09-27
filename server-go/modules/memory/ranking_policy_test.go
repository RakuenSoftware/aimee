package memory

import (
	"context"
	"encoding/json"
	"math"
	"math/rand"
	"os"
	"reflect"
	"testing"
)

func TestPriorRankDisplacementBoundAndDeterminism(t *testing.T) {
	random := rand.New(rand.NewSource(9009))
	for trial := 0; trial < 1000; trial++ {
		count := 1 + random.Intn(128)
		base := make([]Record, count)
		for i := range base {
			base[i] = Record{ID: int64(i + 1), retrievalScore: float64(random.Intn(20))}
		}
		for _, cap := range []int{0, 1, 3} {
			ordered := boundedPriorOrder(base, cap)
			again := boundedPriorOrder(base, cap)
			seen := map[int64]bool{}
			for rank, r := range ordered {
				if seen[r.ID] || math.Abs(float64(rank+1)-float64(r.ID)) > float64(cap) || again[rank].ID != r.ID {
					t.Fatal("prior exceeded joint displacement", trial, rank, r, cap)
				}
				seen[r.ID] = true
			}
		}
	}
	ties := []Record{{ID: 5, retrievalScore: 1}, {ID: 1, retrievalScore: 1}, {ID: 9, retrievalScore: 1}}
	for i, r := range boundedPriorOrder(ties, 3) {
		if r.ID != ties[i].ID {
			t.Fatal("unstable base ties")
		}
	}
}
func TestStagePriorCapsAndMaximumOverturn(t *testing.T) {
	for _, bound := range []float64{assertionLexicalPriorBound, graphRankingPriorBound} {
		for _, signA := range []float64{-1, 0, 1} {
			for _, signB := range []float64{-1, 0, 1} {
				makePrior := func(base, sign float64) scorePriorResult {
					return boundedScorePriors("test", base, bound, scorePriorAdjustment{Name: "a", Raw: sign * 1e6, Bound: bound}, scorePriorAdjustment{Name: "b", Raw: sign * 1e6, Bound: bound})
				}
				low, high := makePrior(1, signA), makePrior(1+2*bound+1e-8, signB)
				if low.Final >= high.Final || math.Abs(low.JointDelta) > bound || math.Abs(high.JointDelta) > bound {
					t.Fatal("joint priors reversed a gap greater than 2B", low, high)
				}
				for _, p := range low.Adjustments {
					if math.Abs(p.Applied) > p.Bound {
						t.Fatal("individual prior exceeded cap")
					}
				}
			}
		}
	}
	strong := boundedGraphRelevance("defines", false, 0, 0, -1e9, 1, "C")
	weak := boundedGraphRelevance("imports", false, 0, 100000, 1e9, 1, "A")
	if strong.Final <= weak.Final || !validScorePriorResult(&strong) || !validScorePriorResult(&weak) {
		t.Fatal(strong, weak)
	}
	invalid := boundedGraphRelevance("calls", false, 0, 1, math.Inf(1), 1, "A")
	if invalid.Adjustments[1].InputState != "nonfinite_ignored" || !validScorePriorResult(&invalid) {
		t.Fatal("nonfinite utility hidden", invalid)
	}
	forged := strong
	forged.JointDelta = 1
	if validScorePriorResult(&forged) {
		t.Fatal("forged score proof")
	}
}
func TestHealthRejectsUnboundedOrForgedPriorMetadata(t *testing.T) {
	proof := boundedGraphRelevance("calls", false, 0, 10, 2, 1, "B")
	steps := []rankingStep{{Operation: "native_bounded_graph_relevance", PriorPolicy: proof.Policy, PriorScore: &proof, Score: proof.Final, Contributions: []rankingContribution{{Arm: "base", Value: proof.Final}}}}
	if len(validatedHealthRanking(steps)) != 1 {
		t.Fatal("valid proof refused")
	}
	proof.Final++
	if validatedHealthRanking(steps) != nil {
		t.Fatal("forged score accepted")
	}
	steps = []rankingStep{{Operation: "pagerank", PriorPolicy: nativePriorPolicy, BaseRank: 1, FinalRank: 10, MaxRankDisplacement: 3, Score: 1, Contributions: []rankingContribution{{Arm: "base", Value: 1}}}}
	if validatedHealthRanking(steps) != nil {
		t.Fatal("unbounded prior accepted")
	}
	ctx := withRankingTrace(context.Background())
	record := Record{ID: 1, retrievalScore: 1}
	recordRankingStep(ctx, &record, "pagerank", rankingContribution{Arm: "base", Value: 1})
	if len(record.rankingSteps) != 1 {
		t.Fatal(record)
	}
}

func TestRankingArtifactsFrozen(t *testing.T) {
	for _, tc := range []struct {
		name      string
		selection bool
	}{{"baseline", false}, {"diversity-v2", true}} {
		raw, err := os.ReadFile("../../../tests/eval/memory_mr09/" + tc.name + "-policy.json")
		if err != nil {
			t.Fatal(err)
		}
		var frozen retrievalPolicyArtifact
		if json.Unmarshal(raw, &frozen) != nil || !reflect.DeepEqual(frozen, rankingArtifact(tc.selection)) {
			t.Fatal("policy changed without versioned artifact", tc.name)
		}
		if rankingArtifactDigest(tc.selection) == "" {
			t.Fatal("missing artifact identity")
		}
	}
	if rankingArtifactDigest(false) == rankingArtifactDigest(true) {
		t.Fatal("different policies share identity")
	}
}
