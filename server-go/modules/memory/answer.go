package memory

import (
	"context"
	"encoding/json"
	"errors"
	"math"
	"os"
	"strconv"
	"strings"
	"unicode"
)

type AnswerEvidence struct {
	Decision        string  `json:"decision"`
	Reason          string  `json:"reason"`
	CandidateIDs    []int64 `json:"candidate_ids"`
	RankedCount     int     `json:"ranked_count"`
	AnchorID        int64   `json:"anchor_id"`
	AnchorRank      int     `json:"anchor_rank"`
	TopKGrounding   float64 `json:"topk_grounding"`
	AnchorCoverage  float64 `json:"anchor_coverage"`
	ClusterCoverage float64 `json:"cluster_coverage"`
	Threshold       float64 `json:"threshold"`
	ChunkFloor      float64 `json:"chunk_floor"`
	Structural      bool    `json:"structural"`
	Exempt          bool    `json:"exempt"`
	TraceTruncated  bool    `json:"trace_truncated"`
}

type answerPolicy struct {
	abstain, stripUnverified, reprompt bool
	citations                          string
	threshold, chunkFloor              float64
}

func (s *postgresDataStore) answerPolicy() (answerPolicy, error) {
	p := answerPolicy{threshold: 0.4}
	if s.settings != nil {
		values, err := s.settings()
		if err != nil {
			return p, err
		}
		number := func(key string) float64 {
			switch v := values[key].(type) {
			case float64:
				return v
			case int:
				return float64(v)
			case bool:
				if v {
					return 1
				}
			case json.Number:
				n, _ := v.Float64()
				return n
			}
			return 0
		}
		p.abstain = number("memory_abstain_enabled") != 0
		p.stripUnverified = number("memory_citations_strip_unverified") != 0
		p.reprompt = number("memory_citations_reprompt_on_miss") != 0
		p.citations, _ = values["memory_citations_mode"].(string)
		if n := number("memory_abstain_gate"); n > 0 {
			p.threshold = n
		}
		p.chunkFloor = math.Max(0, math.Min(1, number("memory_chunk_min_confidence")))
	}
	if mode := os.Getenv("AIMEE_MEMORY_CITATIONS_MODE"); mode != "" {
		p.citations = mode
	}
	if value := os.Getenv("AIMEE_MEMORY_CITATIONS_STRIP_UNVERIFIED"); value != "" {
		n, _ := strconv.Atoi(value)
		p.stripUnverified = n != 0
	}
	return p, nil
}

func answerTerms(query string) []string {
	terms := strings.FieldsFunc(strings.ToLower(query), func(r rune) bool { return !unicode.IsLetter(r) && !unicode.IsDigit(r) })
	if len(terms) > 32 {
		terms = terms[:32]
	}
	stop := " a an the is in on at to for of and or but it its was are be been has have had do does did will would could should may might me my we us our you your he she they them what when where who how why which that this "
	out := []string{}
	for _, term := range terms {
		if len(term) >= 3 && !strings.Contains(stop, " "+term+" ") {
			out = append(out, term)
		}
	}
	return out
}

func answerCoverage(terms []string, rows []publicMemoryRecord) float64 {
	if len(terms) == 0 {
		return 0
	}
	covered := 0
	for _, term := range terms {
		for _, row := range rows {
			if strings.Contains(strings.ToLower(row.Key), term) || strings.Contains(strings.ToLower(row.Content), term) {
				covered++
				break
			}
		}
	}
	return float64(covered) / float64(len(terms))
}

func answerCluster(rows []publicMemoryRecord, anchor, candidate int) bool {
	return anchor == candidate || rows[anchor].SourceSession != "" && rows[anchor].SourceSession == rows[candidate].SourceSession
}

func answerClusterScore(rows []publicMemoryRecord, anchor int) float64 {
	score := 1 / float64(anchor+1)
	count := 0
	variety, episode, fact := false, false, false
	for i, row := range rows {
		if !answerCluster(rows, anchor, i) {
			continue
		}
		count++
		score += 0.35 / float64(i+1)
		variety = variety || row.Kind != rows[anchor].Kind
		episode = episode || row.Kind == "episode"
		fact = fact || row.Kind == "fact"
	}
	if count > 1 {
		score += float64(min(count, 4)) * 0.08
	}
	if variety {
		score += 0.10
	}
	if episode {
		score += 0.12
	}
	if fact {
		score += 0.08
	}
	return score
}

func answerAnchor(rows []publicMemoryRecord) int {
	anchor := 0
	for i := 1; i < len(rows); i++ {
		if answerClusterScore(rows, i) > answerClusterScore(rows, anchor) {
			anchor = i
		}
	}
	return anchor
}

func answerTrace(rows []publicMemoryRecord, anchor int) AnswerEvidence {
	trace := AnswerEvidence{Decision: "answerable", Reason: "ok", AnchorRank: -1, RankedCount: len(rows), CandidateIDs: []int64{}, TraceTruncated: len(rows) > 16}
	for _, row := range rows[:min(len(rows), 16)] {
		trace.CandidateIDs = append(trace.CandidateIDs, row.ID)
	}
	if anchor >= 0 && anchor < len(rows) {
		trace.AnchorID = rows[anchor].ID
		trace.AnchorRank = rows[anchor].HybridRank
		if trace.AnchorRank <= 0 {
			trace.AnchorRank = anchor + 1
		}
	}
	return trace
}

func answerGate(rows []publicMemoryRecord, anchor int, query string, citations int, confidence float64, policy answerPolicy) AnswerEvidence {
	trace := answerTrace(rows, anchor)
	trace.Threshold = policy.threshold
	if policy.abstain {
		trace.ChunkFloor = policy.chunkFloor
	}
	abstain := func(reason string, structural bool) AnswerEvidence {
		trace.Decision = "abstain"
		trace.Reason = reason
		trace.Structural = structural
		return trace
	}
	if len(rows) == 0 {
		return abstain("structural_empty", true)
	}
	if rows[anchor].Tier == "L4" || rows[anchor].Tier == "L5" {
		trace.Decision = "exempt"
		trace.Reason = "curated_exempt"
		trace.Exempt = true
		return trace
	}
	if policy.citations == "required" && citations == 0 {
		return abstain("citation_required", true)
	}
	kept := []publicMemoryRecord{}
	for i, row := range rows[:min(len(rows), 16)] {
		if trace.ChunkFloor > 0 && row.RetrievalScore < trace.ChunkFloor {
			continue
		}
		if row.RetrievalScore <= 0 {
			row.RetrievalScore = float64(min(len(rows), 16) - i)
		}
		kept = append(kept, row)
	}
	if trace.ChunkFloor > 0 && len(kept) == 0 {
		return abstain("chunk_floor", false)
	}
	terms := answerTerms(query)
	separation := 0.5
	if len(kept) > 1 {
		top, ref := kept[0].RetrievalScore, kept[min(2, len(kept)-1)].RetrievalScore
		separation = 0
		if top > 1e-9 {
			separation = math.Min(1, (top-ref)/top)
		}
	}
	if len(kept) > 0 {
		trace.TopKGrounding = 0.6*answerCoverage(terms, kept[:min(len(kept), 10)]) + 0.4*separation
	}
	trace.AnchorCoverage = answerCoverage(terms, rows[anchor:anchor+1])
	cluster := []publicMemoryRecord{}
	for i, row := range rows {
		if answerCluster(rows, anchor, i) {
			cluster = append(cluster, row)
		}
	}
	trace.ClusterCoverage = answerCoverage(terms, cluster)
	grounding := math.Min(trace.TopKGrounding, math.Min(trace.AnchorCoverage, trace.ClusterCoverage))
	if policy.abstain && grounding < policy.threshold && !(policy.threshold-grounding < 0.02 && confidence >= policy.threshold) {
		return abstain("grounding_low", false)
	}
	return trace
}

func answerIntent(query string) string {
	normalized := " " + strings.Join(strings.FieldsFunc(strings.ToLower(query), func(r rune) bool { return !unicode.IsLetter(r) && !unicode.IsDigit(r) }), " ") + " "
	for _, set := range []struct{ intent, words string }{{"temporal", "when|what time|what day|date|dated|before|after|between|since|until|last week|next week|last month|next month|ago|year|month|day|time|today|yesterday|tomorrow|latest|recent|earliest|monday|tuesday|wednesday|thursday|friday|saturday|sunday"}, {"procedural", "how|steps|procedure|workflow|setup"}, {"entity", "who|person|people|team|owner"}} {
		for _, word := range strings.Split(set.words, "|") {
			if strings.Contains(normalized, " "+word+" ") {
				return set.intent
			}
		}
	}
	return "general"
}

func (s *postgresDataStore) extractAnswer(ctx context.Context, rows []publicMemoryRecord, anchor int, query string) (string, error) {
	ids := []int64{}
	for i, row := range rows {
		if answerCluster(rows, anchor, i) {
			ids = append(ids, row.ID)
		}
	}
	type detail struct{ actor, action, object, location, eventTime, temporal, summary string }
	details := map[int64]detail{}
	if s.placement == PlacementKB {
		selected, err := s.db.Query(ctx, `SELECT m.id,COALESCE(e.actor,''),COALESCE(e.action,''),COALESCE(e.object,''),COALESCE(e.location,''),COALESCE(e.event_time,''),COALESCE(t.ref_key,''),COALESCE(s.summary,'')
FROM memories m
LEFT JOIN LATERAL (SELECT actor,action,object,location,event_time FROM memory_event_frames WHERE memory_id=m.id ORDER BY id LIMIT 1) e ON true
LEFT JOIN LATERAL (SELECT ref_key FROM memory_temporal_refs WHERE memory_id=m.id ORDER BY CASE granularity WHEN 'date_phrase' THEN 0 WHEN 'absolute_day' THEN 1 WHEN 'month' THEN 2 WHEN 'weekday' THEN 3 WHEN 'year' THEN 4 ELSE 5 END,weight DESC,id LIMIT 1) t ON true
LEFT JOIN LATERAL (SELECT summary FROM memory_summaries WHERE memory_id=m.id ORDER BY id LIMIT 1) s ON true
WHERE m.id=ANY($1::text::bigint[])`, memoryIDsParameter(ids))
		if err != nil {
			return "", err
		}
		defer selected.Close()
		for selected.Next() {
			var id int64
			var d detail
			if err := selected.Scan(&id, &d.actor, &d.action, &d.object, &d.location, &d.eventTime, &d.temporal, &d.summary); err != nil {
				return "", err
			}
			details[id] = d
		}
		if err := selected.Err(); err != nil {
			return "", err
		}
	}
	intent := answerIntent(query)
	for _, id := range ids {
		d := details[id]
		switch {
		case intent == "temporal" && d.eventTime != "":
			return d.eventTime, nil
		case intent == "entity" && d.actor != "":
			return d.actor, nil
		case d.location != "" && (strings.Contains(d.action, "move") || strings.Contains(d.action, "visit")):
			return d.location, nil
		case d.object != "":
			return d.object, nil
		}
	}
	if intent == "temporal" {
		for _, id := range ids {
			if d := details[id]; d.temporal != "" {
				return d.temporal, nil
			}
		}
	}
	for _, id := range ids {
		if d := details[id]; d.summary != "" {
			return d.summary, nil
		}
	}
	text := rows[anchor].Content
	if text == "" {
		text = rows[anchor].Key
	}
	return string([]rune(text)[:min(len([]rune(text)), 180)]), nil
}

func (s *postgresDataStore) Ask(ctx context.Context, scope Scope, query string, limit int) (AnswerResult, error) {
	return s.askRequest(ctx, DataRequest{Scope: scope, Query: query, Limit: limit})
}

func (s *postgresDataStore) askRequest(ctx context.Context, request DataRequest) (AnswerResult, error) {
	if strings.TrimSpace(request.Query) == "" {
		return AnswerResult{}, errors.New("memory: query is required")
	}
	policy, err := s.answerPolicy()
	if err != nil {
		return AnswerResult{}, err
	}
	return s.askWithPolicy(ctx, request, policy)
}

func (s *postgresDataStore) askWithPolicy(ctx context.Context, request DataRequest, policy answerPolicy) (AnswerResult, error) {
	var err error
	if request.Limit <= 0 {
		request.Limit = 5
	}
	request.Limit = min(request.Limit, 8)
	var records []Record
	if s.placement == PlacementKB && request.Scope.Type == "" {
		records, err = s.SearchVisible(ctx, request)
	} else {
		records, err = s.Search(ctx, request.Scope, request.Query, "", "", request.Limit)
	}
	if err != nil {
		return AnswerResult{}, err
	}
	result := AnswerResult{CitationIDs: []int64{}, RetrievalCount: len(records), Evidence: answerTrace(nil, -1)}
	if len(records) == 0 {
		result.NoAnswer = true
		result.Evidence.Decision = "abstain"
		result.Evidence.Reason = "structural_empty"
		result.Evidence.Structural = true
		return result, nil
	}
	var rows []publicMemoryRecord
	if s.placement == PlacementKB {
		rows, err = s.publicRecords(ctx, records)
		if err != nil {
			return result, err
		}
	} else {
		for _, r := range records {
			rows = append(rows, publicMemoryRecord{ID: r.ID, Tier: r.Tier, Kind: r.Kind, Key: r.Key, Content: r.Content})
		}
	}
	if len(rows) != len(records) {
		return result, errors.New("memory: answer metadata unavailable")
	}
	for i := range rows {
		rows[i].HybridRank = i + 1
		rows[i].RetrievalScore = diagnosticFor(records[i], request.Query).Parts.Total
	}
	anchor := answerAnchor(rows)
	result.Answer, err = s.extractAnswer(ctx, rows, anchor, request.Query)
	if err != nil {
		return result, err
	}
	if result.Answer == "" {
		result.NoAnswer = true
		result.Evidence = answerTrace(rows, anchor)
		result.Evidence.Decision = "abstain"
		result.Evidence.Reason = "structural_no_extract"
		result.Evidence.Structural = true
		return result, nil
	}
	if policy.citations == "required" {
		runtimeMetricState.citationRequired.Add(1)
		cited := false
		for i, row := range rows {
			cited = cited || answerCluster(rows, anchor, i) && row.ID > 0
		}
		if !cited && policy.reprompt {
			// Preserve the caller's scope on the one recovery attempt.
			policy.reprompt = false
			request.Limit = 5
			runtimeMetricState.citationReprompted.Add(1)
			retry, retryErr := s.askWithPolicy(ctx, request, policy)
			if retryErr != nil {
				return retry, retryErr
			}
			if len(retry.CitationIDs) > 0 {
				return retry, nil
			}
		}
	}
	result = finishAnswer(rows, anchor, request.Query, result.Answer, policy)
	return result, nil
}

func finishAnswer(rows []publicMemoryRecord, anchor int, query, answer string, policy answerPolicy) AnswerResult {
	result := AnswerResult{Answer: answer, CitationIDs: []int64{}, RetrievalCount: len(rows), EvidenceMode: "verbatim"}
	a := rows[anchor]
	if a.Tier == "L5" || a.Kind == "synthesis" || a.Kind == "restoration" || a.ProvenanceCategory == "synthesis" || a.ProvenanceCategory == "restoration" {
		result.EvidenceMode = "synthesised"
	}
	seen := map[int64]bool{}
	for i, row := range rows {
		if answerCluster(rows, anchor, i) && row.ID > 0 && !seen[row.ID] && len(result.CitationIDs) < 4 {
			result.CitationIDs = append(result.CitationIDs, row.ID)
			seen[row.ID] = true
		}
	}
	if len(result.CitationIDs) > 0 {
		runtimeMetricState.citationVerified.Add(1)
	} else if policy.citations != "" && policy.citations != "off" {
		runtimeMetricState.citationMissing.Add(1)
	}
	if len(result.CitationIDs) == 0 && policy.citations != "" && policy.citations != "off" && policy.stripUnverified {
		runtimeMetricState.citationStripped.Add(1)
		result.NoAnswer = true
		result.Answer = ""
		result.Evidence = answerTrace(rows, anchor)
		result.Evidence.Decision = "abstain"
		result.Evidence.Reason = "citation_required"
		result.Evidence.Structural = true
		if a.Tier == "L4" || a.Tier == "L5" {
			result.Evidence.Decision = "exempt"
			result.Evidence.Reason = "curated_exempt"
			result.Evidence.Structural = false
			result.Evidence.Exempt = true
		}
		return result
	}
	result.LowConfidence = len(result.CitationIDs) == 0 && policy.citations == "required"
	citationSupport := 1.0
	if len(result.CitationIDs) == 0 {
		citationSupport = 0.35
		if result.LowConfidence {
			citationSupport = 0
		}
	}
	result.Confidence = 0.45*math.Min(1, answerClusterScore(rows, anchor)/2) + 0.20/float64(anchor+1) + 0.20*float64(min(len(rows), 4))/4 + 0.15*citationSupport
	if result.LowConfidence {
		result.Confidence *= 0.45
	}
	result.Evidence = answerGate(rows, anchor, query, len(result.CitationIDs), result.Confidence, policy)
	if result.Evidence.Decision == "abstain" {
		runtimeMetricState.answerAbstained.Add(1)
		result.NoAnswer = true
		result.LowConfidence = true
		result.Answer = ""
		result.CitationIDs = []int64{}
	} else if result.LowConfidence {
		result.Answer = "## Retrieval Confidence: LOW\nNo verified citations could be attached to this answer. The information below is unverified and may be inaccurate.\n\n" + result.Answer
	}
	return result
}
