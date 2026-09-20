// Package retrievalmetrics provides shared information-retrieval evaluation math.
package retrievalmetrics

import "math"

func isRelevant(id int64, relevant []int64) bool {
	for _, candidate := range relevant {
		if candidate == id {
			return true
		}
	}
	return false
}

// Score returns reciprocal rank over all retrieved rows and NDCG/recall at k.
// Duplicate results and duplicate relevance entries retain the legacy metrics'
// counting behavior; callers must not silently deduplicate benchmark inputs.
func Score(retrieved, relevant []int64, k int) (mrr, ndcg, recall float64) {
	for index, id := range retrieved {
		if isRelevant(id, relevant) {
			mrr = 1 / float64(index+1)
			break
		}
	}
	if len(relevant) == 0 || k <= 0 {
		return mrr, 0, 0
	}

	limit := min(len(retrieved), k)
	var dcg float64
	found := 0
	for index, id := range retrieved[:limit] {
		if isRelevant(id, relevant) {
			dcg += 1 / math.Log2(float64(index)+2)
			found++
		}
	}
	idealLimit := min(len(relevant), k)
	var idcg float64
	for index := range idealLimit {
		idcg += 1 / math.Log2(float64(index)+2)
	}
	if idcg > 0 {
		ndcg = dcg / idcg
	}
	return mrr, ndcg, float64(found) / float64(len(relevant))
}
