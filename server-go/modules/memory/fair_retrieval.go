package memory

import "context"

// The kernel remains bounded to 128 records. Graph neighbors reserve at most
// half of that work budget independently of the initial pool. Empty graph slots
// return to the relevance-ordered pool; a full lexical pool never vetoes graph
// admission. This is candidate admission, not a graph score or final top-k.
func fairPageRankPool(ctx context.Context, base, neighbors []Record) []Record {
	seen := map[int64]bool{}
	for _, r := range base {
		seen[r.ID] = true
	}
	graph := []Record{}
	for _, r := range neighbors {
		if !seen[r.ID] && len(graph) < pageRankCandidateCap/2 {
			seen[r.ID] = true
			graph = append(graph, r)
		}
	}
	keep := min(len(base), pageRankCandidateCap-len(graph))
	out := append([]Record(nil), base[:keep]...)
	for _, r := range base[keep:] {
		captureRankingCandidate(ctx, r, "pagerank_candidate_work_budget")
	}
	return append(out, graph...)
}

func sameRankedVersion(a, b Record) bool {
	av, bv := a.Version, b.Version
	if av == nil {
		av = a.observedVersion
	}
	if bv == nil {
		bv = b.observedVersion
	}
	if av == nil || bv == nil {
		return av == nil && bv == nil
	}
	return *av == *bv
}
