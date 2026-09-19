package memory

import (
	"context"
	"encoding/json"
	"errors"
	"math"
	"strings"
	"sync"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

const pageRankCandidateCap = 128
const pageRankLinkCap = pageRankCandidateCap * 64

type pageRankRequest struct {
	IDs        []int64  `json:"ids"`
	Iterations int      `json:"iterations"`
	Weight     float64  `json:"weight"`
	Relations  []string `json:"relations"`
}

type pageRankScore struct {
	ID    int64   `json:"memory_id"`
	Score float64 `json:"score"`
}

type pageRankResult struct {
	visibleIDs []int64
	recall     bool
	Status     string          `json:"status"`
	Scores     []pageRankScore `json:"scores"`
	Candidates int             `json:"candidates"`
	Edges      int             `json:"edges"` // directed adjacency entries, matching the native counter
	ElapsedMS  float64         `json:"elapsed_ms"`
}

func validPageRankRequest(req *pageRankRequest) bool {
	if req == nil || len(req.IDs) == 0 || len(req.IDs) > pageRankCandidateCap || req.Iterations < 1 || req.Iterations > 16 || req.Weight <= 0 || req.Weight > 10 || math.IsNaN(req.Weight) || math.IsInf(req.Weight, 0) || len(req.Relations) > 64 {
		return false
	}
	seen := map[int64]bool{}
	for _, id := range req.IDs {
		if id <= 0 || seen[id] {
			return false
		}
		seen[id] = true
	}
	for _, relation := range req.Relations {
		if relation == "" || len(relation) > 255 {
			return false
		}
	}
	return true
}

// pageRankScores preserves the retired native kernel: each allowed link adds
// one adjacency in each direction, parallel links add multiplicity, dangling
// mass is uniform, damping is .85 and bonuses are normalized to the maximum.
// Input identities have already passed scope/lifecycle checks in the store.
func pageRankScores(ctx context.Context, ids []int64, links []MemoryLink, req pageRankRequest) ([]pageRankScore, int, error) {
	if ctx == nil || len(ids) > pageRankCandidateCap || len(links) > pageRankLinkCap || req.Iterations < 1 || req.Iterations > 16 || req.Weight <= 0 || req.Weight > 10 || math.IsNaN(req.Weight) || math.IsInf(req.Weight, 0) {
		return nil, 0, errors.New("memory: invalid PageRank bounds")
	}
	if err := ctx.Err(); err != nil {
		return nil, 0, err
	}
	scores := make([]pageRankScore, 0, len(ids))
	index := make(map[int64]int, len(ids))
	for i, id := range ids {
		if id <= 0 {
			return nil, 0, errors.New("memory: invalid PageRank identity")
		}
		if _, exists := index[id]; exists {
			return nil, 0, errors.New("memory: duplicate PageRank identity")
		}
		index[id] = i
	}
	if len(ids) <= 1 {
		return scores, 0, nil
	}
	allowed := map[string]bool{}
	for _, relation := range req.Relations {
		allowed[relation] = true
	}
	n := len(ids)
	adjacency := make([]float64, n*n)
	rank, next := make([]float64, n), make([]float64, n)
	edges := 0
	for _, link := range links {
		i, left := index[link.SourceID]
		j, right := index[link.TargetID]
		if !left || !right || i == j || link.Relation == "" || (len(allowed) != 0 && !allowed[link.Relation]) {
			continue
		}
		adjacency[i*n+j]++
		adjacency[j*n+i]++
		edges += 2
	}
	for i := range rank {
		rank[i] = 1 / float64(n)
	}
	for range req.Iterations {
		if err := ctx.Err(); err != nil {
			return nil, 0, err
		}
		for i := range next {
			next[i] = (1 - .85) / float64(n)
		}
		for i := range rank {
			degree := 0.0
			for j := range rank {
				degree += adjacency[i*n+j]
			}
			if degree == 0 {
				for j := range next {
					next[j] += .85 * rank[i] / float64(n)
				}
				continue
			}
			for j := range next {
				if adjacency[i*n+j] != 0 {
					next[j] += .85 * rank[i] * (adjacency[i*n+j] / degree)
				}
			}
		}
		rank, next = next, rank
	}
	maximum := 0.0
	for _, value := range rank {
		maximum = max(maximum, value)
	}
	for i, id := range ids {
		scores = append(scores, pageRankScore{id, rank[i] / maximum * req.Weight})
	}
	return scores, edges, nil
}

// PageRank is KB graph scoring for a supplied candidate set, not a retrieval
// policy switch. Scope and lifecycle filtering precede graph work and its cap.
func (s *postgresDataStore) pageRank(ctx context.Context, req DataRequest, exact bool) (pageRankResult, error) {
	start := time.Now()
	result := pageRankResult{Status: "ok", Scores: []pageRankScore{}}
	if !validPageRankRequest(req.PageRank) {
		return result, errors.New("memory: invalid PageRank request")
	}
	args := append(graphScopeArgs(req, exact), memoryIDsParameter(req.PageRank.IDs))
	rows, err := s.db.Query(ctx, `WITH visible AS MATERIALIZED (`+graphVisible+` AND id=ANY($9::text::bigint[]))
 SELECT id FROM visible
 ORDER BY array_position($9::text::bigint[],id)`, args...)
	if err != nil {
		return result, err
	}
	ids := []int64{}
	for rows.Next() {
		var id int64
		if err := rows.Scan(&id); err != nil {
			rows.Close()
			return result, err
		}
		ids = append(ids, id)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return result, err
	}
	result.Candidates = len(ids)
	result.visibleIDs = ids
	links := []MemoryLink{}
	if len(ids) > 1 {
		relations, err := json.Marshal(append([]string{}, req.PageRank.Relations...))
		if err != nil {
			return result, err
		}
		rows, err = s.db.Query(ctx, `SELECT source_id,target_id,relation FROM memory_links
 WHERE source_id=ANY($1::text::bigint[]) AND target_id=ANY($1::text::bigint[]) AND source_id<>target_id AND relation<>''
 AND ($2::jsonb='[]'::jsonb OR relation IN (SELECT jsonb_array_elements_text($2::jsonb)))
 ORDER BY id LIMIT $3`, memoryIDsParameter(ids), string(relations), pageRankLinkCap+1)
		if err != nil {
			return result, err
		}
		for rows.Next() {
			var link MemoryLink
			if err := rows.Scan(&link.SourceID, &link.TargetID, &link.Relation); err != nil {
				rows.Close()
				return result, err
			}
			links = append(links, link)
		}
		err = rows.Err()
		rows.Close()
		if err != nil {
			return result, err
		}
		if len(links) > pageRankLinkCap {
			return result, errors.New("memory: PageRank graph exceeds work budget")
		}
	}
	result.Scores, result.Edges, err = pageRankScores(ctx, ids, links, *req.PageRank)
	if err != nil {
		return result, err
	}
	result.ElapsedMS = float64(time.Since(start)) / float64(time.Millisecond)
	return result, nil
}

func handlePageRank(options handlerOptions, invocation bus.ModuleInvocation, args commandArgs) ([]byte, bus.ModuleStatus) {
	for _, key := range []string{"iterations", "weight", "relations"} {
		if strings.TrimSpace(string(args[key])) == "null" {
			return nil, bus.ModuleStatusInvalidRequest
		}
	}
	request := defaultPageRankRequest()
	raw, err := json.Marshal(args)
	if err != nil || json.Unmarshal(raw, &request) != nil || !validPageRankRequest(&request) {
		return nil, bus.ModuleStatusInvalidRequest
	}
	response, status := benchmarkRead(options, invocation, args, DataRequest{Operation: "pagerank", PageRank: &request})
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	if len(response.Payload) == 0 {
		return nil, bus.ModuleStatusInternal
	}
	return commandResult(json.RawMessage(response.Payload))
}

type pageRankMetrics struct {
	RecallSamples    int64   `json:"recall_samples"`
	CandidateSamples int64   `json:"candidate_samples"`
	LastMS           float64 `json:"last_ms"`
	AverageMS        float64 `json:"avg_ms"`
	MaximumMS        float64 `json:"max_ms"`
	Samples          int64   `json:"samples"`
	Candidates       int     `json:"last_candidates"`
	Edges            int     `json:"last_edges"`
}

type pageRankMetricCounters struct {
	sync.Mutex
	value pageRankMetrics
}

var pageRankMetricState pageRankMetricCounters

func (m *pageRankMetricCounters) observe(result pageRankResult) {
	m.Lock()
	defer m.Unlock()
	m.value.Samples++
	if result.recall {
		m.value.RecallSamples++
	} else {
		m.value.CandidateSamples++
	}
	m.value.LastMS = result.ElapsedMS
	m.value.AverageMS += (result.ElapsedMS - m.value.AverageMS) / float64(m.value.Samples)
	m.value.MaximumMS = max(m.value.MaximumMS, result.ElapsedMS)
	m.value.Candidates, m.value.Edges = result.Candidates, result.Edges
}
func (m *pageRankMetricCounters) snapshot() pageRankMetrics {
	m.Lock()
	defer m.Unlock()
	return m.value
}
