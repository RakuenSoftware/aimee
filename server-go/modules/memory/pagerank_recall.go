package memory

import (
	"context"
	"encoding/json"
	"errors"
	"math"
	"os"
	"sort"
	"strconv"
	"strings"
	"time"
)

// The Go recall owner combines reciprocal ranks (k=60), not the retired C
// ranker's text-score units. Convert the bounded kernel bonus to one rank arm's
// units. This policy is opt-in and is not a claim of historical rank-order parity.
const pageRankRecallPolicy = nativePriorPolicy
const recallRankK = 60.0

type pageRankConfig struct {
	enabled bool
	request pageRankRequest
}

func defaultPageRankRequest() pageRankRequest {
	return pageRankRequest{Iterations: 6, Weight: .35, Relations: []string{"depends_on", "related_to", "co_edited", "fixes"}}
}

func loadPageRankConfig(values map[string]any) (pageRankConfig, error) {
	cfg := pageRankConfig{request: defaultPageRankRequest()}
	enabled := configNumber(values, "memory_pagerank_enabled")
	if enabled != 0 && enabled != 1 {
		return cfg, errors.New("memory: invalid PageRank enabled setting")
	}
	if value := os.Getenv("AIMEE_MEMORY_PAGERANK_ENABLED"); value != "" {
		parsed, err := strconv.Atoi(value)
		if err != nil {
			return cfg, errors.New("memory: invalid PageRank enabled environment")
		}
		enabled = float64(max(0, min(1, parsed)))
	}
	cfg.enabled = enabled != 0
	if !cfg.enabled {
		return cfg, nil
	}
	iterations := configNumber(values, "memory_pagerank_iterations")
	weight := configNumber(values, "memory_pagerank_weight")
	if iterations != 0 {
		if math.IsNaN(iterations) || math.IsInf(iterations, 0) || iterations != math.Trunc(iterations) || iterations < 1 || iterations > 16 {
			return cfg, errors.New("memory: invalid PageRank iteration setting")
		}
		cfg.request.Iterations = int(iterations)
	}
	if weight != 0 {
		cfg.request.Weight = weight
	}
	if value := os.Getenv("AIMEE_MEMORY_PAGERANK_WEIGHT"); value != "" {
		parsed, err := strconv.ParseFloat(value, 64)
		if err != nil {
			return cfg, errors.New("memory: invalid PageRank weight environment")
		}
		cfg.request.Weight = parsed
	}
	if value := os.Getenv("AIMEE_MEMORY_PAGERANK_ITERATIONS"); value != "" {
		parsed, err := strconv.Atoi(value)
		if err != nil {
			return cfg, errors.New("memory: invalid PageRank iterations environment")
		}
		cfg.request.Iterations = max(1, min(16, parsed))
	}
	relations, _ := values["memory_pagerank_relations"].(string)
	if value := os.Getenv("AIMEE_MEMORY_PAGERANK_RELATIONS"); value != "" {
		relations = value
	}
	if relations != "" {
		cfg.request.Relations = nil
		for _, relation := range strings.Split(relations, ",") {
			if relation = strings.TrimSpace(relation); relation != "" {
				cfg.request.Relations = append(cfg.request.Relations, relation)
			}
		}
		if len(cfg.request.Relations) == 0 {
			return cfg, errors.New("memory: empty PageRank relation filter")
		}
	}
	check := cfg.request
	check.IDs = []int64{1}
	if !validPageRankRequest(&check) {
		return cfg, errors.New("memory: invalid PageRank configuration")
	}
	return cfg, nil
}

// Capture the effective optional ranking policy once per search. Request JSON
// cannot turn it on or replace its weights. Personal memory has no KB link graph.
func (s *postgresDataStore) planRecall(req DataRequest) (DataRequest, error) {
	req.requestedLimit = req.Limit
	if s.placement != PlacementKB || req.Query == "" {
		return req, nil
	}
	var values map[string]any
	var err error
	if s.settings != nil {
		values, err = s.settings()
		if err != nil {
			return req, err
		}
	}
	cfg, err := loadPageRankConfig(values)
	if err != nil {
		return req, err
	}
	req.pageRankConfig = &cfg
	if cfg.enabled {
		req.Limit = min(pageRankCandidateCap, max(16, min(req.Limit, pageRankCandidateCap)*4))
	}
	return req, nil
}

// One hop over eligible endpoints only. Relation filtering and endpoint
// visibility precede the work cap, so hidden links cannot spend its budget.
func (s *postgresDataStore) pageRankNeighbors(ctx context.Context, req DataRequest, exact bool, base []Record) ([]Record, error) {
	if len(base) == 0 {
		return base, nil
	}
	ids := make([]int64, 0, len(base))
	seen := map[int64]bool{}
	for _, r := range base {
		if len(ids) < pageRankCandidateCap {
			ids = append(ids, r.ID)
		}
		seen[r.ID] = true
	}
	neighbors := []Record{}
	relations, _ := json.Marshal(req.pageRankConfig.request.Relations)
	rows, err := s.db.Query(ctx, `WITH visible AS NOT MATERIALIZED (`+graphVisibleSQL(true)+`)
 SELECT n.id,n.scope_type,n.scope_value,n.tier,n.kind,n.key,n.content,n.confidence,
 (SELECT owner_id::text FROM memory_collection_owner WHERE id=1),n.record_revision::text
 FROM memory_links l JOIN visible a ON a.id=l.source_id JOIN visible b ON b.id=l.target_id
 JOIN visible n ON n.id=CASE WHEN l.source_id=ANY($9::text::bigint[]) THEN l.target_id ELSE l.source_id END
 WHERE (l.source_id=ANY($9::text::bigint[]) OR l.target_id=ANY($9::text::bigint[]))
 AND l.source_id<>l.target_id AND l.relation<>''
 AND (jsonb_array_length($10::jsonb)=0 OR l.relation IN (SELECT jsonb_array_elements_text($10::jsonb)))
 ORDER BY CASE WHEN $1 THEN 0 WHEN n.scope_type='project' AND n.scope_value=$5 THEN 0
 WHEN n.scope_type='workspace' AND n.scope_value=$6 THEN 1
 WHEN n.scope_type='global' OR (n.scope_type='workspace' AND n.scope_value='_shared') THEN 2 ELSE 3 END,l.id LIMIT $11`, append(graphScopeArgs(req, exact), memoryIDsParameter(ids), string(relations), pageRankLinkCap+1)...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	count := 0
	for rows.Next() {
		count++
		if count > pageRankLinkCap {
			return nil, errors.New("memory: PageRank neighbor graph exceeds work budget")
		}
		var r Record
		r.Version = &MemoryRecordVersion{SchemaVersion: 1}
		r.currentRead = true
		if err := rows.Scan(&r.ID, &r.Scope.Type, &r.Scope.Value, &r.Tier, &r.Kind, &r.Key, &r.Content, &r.Confidence, &r.Version.OwnerID, &r.Version.RecordRevision); err != nil {
			return nil, err
		}
		r.Version.RecordID = strconv.FormatInt(r.ID, 10)
		if !r.Version.validFor(r.ID) {
			return nil, errors.New("memory: invalid PageRank neighbor version")
		}
		if !seen[r.ID] && len(neighbors) < pageRankCandidateCap/2 {
			seen[r.ID] = true
			// A link-only candidate has no lexical or semantic rank-arm contribution.
			neighbors = append(neighbors, r)
			req.lanes.add([]Record{r}, laneGraph)
		}
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	recordRetrievalArm(ctx, "graph", retrievalArmObservation{State: "available", Reason: "independent_pagerank_neighbor_pool", Candidates: len(neighbors), Quota: pageRankCandidateCap / 2, IndexReadiness: "eligible_current_endpoints"})
	return fairPageRankPool(ctx, base, neighbors), nil
}

func (s *postgresDataStore) rerankPageRank(ctx context.Context, req DataRequest, exact bool, base []Record) ([]Record, error) {
	if req.pageRankConfig == nil || !req.pageRankConfig.enabled || len(base) == 0 {
		return base, nil
	}
	start := time.Now()
	var err error
	base, err = s.pageRankNeighbors(ctx, req, exact, base)
	if err != nil {
		return nil, err
	}
	pr := req.pageRankConfig.request
	for _, r := range base {
		pr.IDs = append(pr.IDs, r.ID)
	}
	req.PageRank = &pr
	measured, err := s.pageRank(ctx, req, exact)
	if err != nil {
		return nil, err
	}
	bonuses := map[int64]float64{}
	for _, score := range measured.Scores {
		bonuses[score.ID] = score.Score / (recallRankK + 1)
	}
	// A concurrent lifecycle/scope change can remove a candidate at graph read.
	// Do not return the earlier copy merely because it had a lexical score.
	visible := map[int64]bool{}
	for _, id := range measured.visibleIDs {
		visible[id] = true
	}
	out := make([]Record, 0, len(base))
	for _, r := range base {
		if !visible[r.ID] {
			continue
		}
		r.pageRankApplied = true
		r.pageRankBonus = bonuses[r.ID]
		r.retrievalBase = r.retrievalScore
		r.retrievalScore += r.pageRankBonus
		out = append(out, r)
	}
	// Scope priority remains the existing hard ordering stratum. Priors cannot
	// move a global candidate into a higher-priority project stratum.
	if !exact {
		sort.SliceStable(out, func(i, j int) bool { return recallScopeRank(out[i], req) < recallScopeRank(out[j], req) })
	}
	ranked := make([]Record, 0, len(out))
	for start := 0; start < len(out); {
		end := start + 1
		for end < len(out) && (exact || recallScopeRank(out[start], req) == recallScopeRank(out[end], req)) {
			end++
		}
		ranked = append(ranked, boundedPriorOrder(out[start:end], nativePriorRankDisplacement)...)
		start = end
	}
	out = ranked
	for i := range out {
		if rankingTraceEnabled(ctx) {
			r := &out[i]
			recordRankingStep(ctx, r, "pagerank", rankingContribution{Arm: "retrieval_base", Value: r.retrievalBase}, rankingContribution{Arm: "pagerank", Value: r.pageRankBonus})
			step := &r.rankingSteps[len(r.rankingSteps)-1]
			step.PriorPolicy = nativePriorPolicy
			step.BaseRank = r.priorBaseRank
			step.FinalRank = r.priorFinalRank
			step.MaxRankDisplacement = nativePriorRankDisplacement
			captureRankingCandidate(ctx, *r, "candidate")
		}
	}
	measured.ElapsedMS = float64(time.Since(start)) / float64(time.Millisecond)
	measured.recall = true
	s.recordPageRankSample(measured)
	return out, nil
}

func (s *postgresDataStore) recordPageRankSample(result pageRankResult) {
	if s.pageRankSamples != nil {
		*s.pageRankSamples = append(*s.pageRankSamples, result)
	}
}
