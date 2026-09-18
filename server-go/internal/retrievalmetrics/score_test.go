package retrievalmetrics

import (
	"math"
	"testing"
)

func TestLegacyScoring(t *testing.T) {
	for _, test := range []struct {
		name                string
		retrieved, relevant []int64
		k                   int
		mrr, ndcg, recall   float64
	}{
		{"perfect", []int64{11, 22, 33}, []int64{11, 22, 33}, 3, 1, 1, 1},
		{"rank two", []int64{5, 9, 7}, []int64{9}, 3, .5, 1 / math.Log2(3), 1},
		{"partial", []int64{1, 2, 3}, []int64{2, 4}, 2, .5, (1 / math.Log2(3)) / (1 + 1/math.Log2(3)), .5},
		{"cutoff does not limit reciprocal rank", []int64{1, 2}, []int64{2}, 1, .5, 0, 0},
		{"empty relevance", []int64{1, 2}, nil, 2, 0, 0, 0},
		{"empty results", nil, []int64{1}, 5, 0, 0, 0},
		{"duplicate results", []int64{7, 7}, []int64{7}, 2, 1, 1 + 1/math.Log2(3), 2},
		{"duplicate relevance", []int64{7}, []int64{7, 7}, 2, 1, 1 / (1 + 1/math.Log2(3)), .5},
		{"zero cutoff", []int64{7}, []int64{7}, 0, 1, 0, 0},
		{"negative cutoff", []int64{7}, []int64{7}, -1, 1, 0, 0},
		{"exact int64", []int64{9007199254740992, 9007199254740993}, []int64{9007199254740993}, 5, .5, 1 / math.Log2(3), 1},
	} {
		t.Run(test.name, func(t *testing.T) {
			mrr, ndcg, recall := Score(test.retrieved, test.relevant, test.k)
			if math.Abs(mrr-test.mrr) > 1e-12 || math.Abs(ndcg-test.ndcg) > 1e-12 || math.Abs(recall-test.recall) > 1e-12 {
				t.Fatal(mrr, ndcg, recall)
			}
		})
	}
}
