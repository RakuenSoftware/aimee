package memory

import (
	"context"
	"sort"
	"strings"
	"unicode"
)

type negationToken struct {
	word  string
	start int
}

func negationMarker(word string) bool {
	return wordIn(word, "not never no without haven't hasn't didn't doesn't can't won't neither nor nobody nothing nowhere none")
}
func negationTokens(text string) []string {
	tokens := []negationToken{}
	start := -1
	flush := func(end int) {
		if start < 0 {
			return
		}
		word := strings.ToLower(textBound(text[start:end], 63))
		if len(word) >= 2 && len(tokens) < 256 {
			tokens = append(tokens, negationToken{word, start})
		}
		start = -1
	}
	for offset, r := range text {
		if unicode.IsLetter(r) || unicode.IsDigit(r) || r == '\'' {
			if start < 0 {
				start = offset
			}
		} else {
			flush(offset)
		}
		if len(tokens) >= 256 {
			break
		}
	}
	flush(len(text))
	out := []string{}
	size := 0
	stopwords := "the a an and or but in on at to for of with by from is are was were be been being have has had do does did will would could should may might can shall not no nor so if then than too very just about up out into over after before between under again further once here there when where why how all each every both few more most other some such only own same that this these those it its"
	for i, token := range tokens {
		if len(token.word) < 3 || negationMarker(token.word) || wordIn(token.word, stopwords) {
			continue
		}
		for j := max(0, i-3); j < min(len(tokens), i+4); j++ {
			if j == i || !negationMarker(tokens[j].word) {
				continue
			}
			low, high := min(token.start, tokens[j].start), max(token.start, tokens[j].start)
			if strings.ContainsAny(text[low:high], ".!?;:") {
				continue
			}
			synthetic := "not_" + token.word
			if size+len(synthetic)+2 <= 2048 {
				out = append(out, synthetic)
				size += len(synthetic) + 1
			}
			break
		}
	}
	return out
}
func negationOverlap(query, content []string) float64 {
	if len(query) == 0 {
		return 0
	}
	seen := map[string]bool{}
	for _, word := range content {
		seen[word] = true
	}
	shared := 0
	for _, word := range query {
		if seen[word] {
			shared++
		}
	}
	return min(10, 3*float64(shared)*(.5+.5*min(1, float64(shared)/float64(len(query)))))
}

func (s *postgresDataStore) finalizeRecall(ctx context.Context, req DataRequest, exact bool, base []Record) (result []Record, resultErr error) {
	if req.lanes == nil {
		req.lanes = recallLanes{}
		req.lanes.add(base, laneLexical)
	}
	for i := range base {
		base[i].retrievalScore = 1 / (recallRankK + float64(i) + 1)
		if rankingTraceEnabled(ctx) {
			recordRankingStep(ctx, &base[i], "candidate_order", rankingContribution{Arm: "candidate_order", Rank: i + 1, Value: base[i].retrievalScore})
		}
	}
	result, resultErr = s.collectRecall(ctx, req, exact, base)
	if resultErr == nil {
		result, resultErr = s.rerankPageRank(ctx, req, exact, result)
	}
	if resultErr == nil {
		limit := req.requestedLimit
		if limit <= 0 {
			limit = req.Limit
		}
		result = result[:min(len(result), max(0, limit))]
		if req.Query != "" {
			req.lanes.observe(result)
		}
	}
	return result, resultErr
}

func (s *postgresDataStore) collectRecall(ctx context.Context, req DataRequest, exact bool, base []Record) ([]Record, error) {
	base, err := s.fuseSharedSemantic(ctx, req, exact, base)
	if err != nil {
		return nil, err
	}
	base, err = s.fuseMemoryGraph(ctx, req, exact, base)
	if err != nil {
		return nil, err
	}
	if req.Query == "" || s.settings == nil {
		return base, nil
	}
	values, err := s.settings()
	if err != nil || configNumber(values, "memory_negation_enabled") == 0 {
		return base, nil
	}
	query := negationTokens(req.Query)
	if len(query) == 0 {
		return base, nil
	}
	candidates := append([]Record{}, base...)
	if s.placement == PlacementKB {
		rows, err := s.db.Query(ctx, `SELECT id,scope_type,scope_value,tier,kind,key,content,confidence FROM memories
 WHERE `+currentMemorySQL("")+` AND
 CASE WHEN $1 THEN scope_type=$2 AND scope_value=$3 ELSE $4 OR scope_type='global' OR (scope_type='workspace' AND scope_value='_shared')
 OR (scope_type='project' AND scope_value=$5) OR (scope_type='workspace' AND scope_value=$6) END
 AND ($7='' OR kind=$7) AND ($8='' OR tier=$8)
 AND memory_negation_fts_tsv @@ websearch_to_tsquery('simple',$9)
 ORDER BY CASE WHEN scope_type='project' AND scope_value=$5 THEN 0
 WHEN scope_type='workspace' AND scope_value=$6 THEN 1 WHEN scope_type='global' OR (scope_type='workspace' AND scope_value='_shared') THEN 2 ELSE 3 END,
 ts_rank_cd(memory_negation_fts_tsv,websearch_to_tsquery('simple',$9)) DESC,id DESC LIMIT 64`,
			exact, req.Scope.Type, req.Scope.Value, req.IncludeAll, req.Project, req.Workspace, req.Kind, req.Tier, strings.Join(query, " or "))
		if err != nil {
			return nil, err
		}
		extra, err := scanRecordRows(rows)
		if err != nil {
			return nil, err
		}
		req.lanes.add(extra, laneLexical)
		candidates = append(candidates, extra...)
	}
	type scored struct {
		record Record
		score  float64
		scope  int
	}
	ordered := []scored{}
	seen := map[int64]bool{}
	for i, r := range candidates {
		if seen[r.ID] {
			continue
		}
		seen[r.ID] = true
		scope := 0
		if !exact && s.placement == PlacementKB {
			switch {
			case r.Scope.Type == ScopeProject && r.Scope.Value == req.Project:
				scope = 0
			case r.Scope.Type == ScopeWorkspace && r.Scope.Value == req.Workspace:
				scope = 1
			case r.Scope.Type == ScopeGlobal || (r.Scope.Type == ScopeWorkspace && r.Scope.Value == "_shared"):
				scope = 2
			default:
				scope = 3
			}
		}
		score := 1/float64(60+i+1) + negationOverlap(query, negationTokens(textBound(r.Key+" "+r.Content, 3071)))
		r.retrievalScore = score
		if rankingTraceEnabled(ctx) {
			recordRankingStep(ctx, &r, "negation", rankingContribution{Arm: "candidate_order", Rank: i + 1, Value: 1 / float64(60+i+1)}, rankingContribution{Arm: "negation_overlap", Value: score - 1/float64(60+i+1)})
		}
		ordered = append(ordered, scored{r, score, scope})
	}
	sort.SliceStable(ordered, func(i, j int) bool {
		if ordered[i].scope != ordered[j].scope {
			return ordered[i].scope < ordered[j].scope
		}
		return ordered[i].score > ordered[j].score
	})
	result := make([]Record, 0, min(req.Limit, len(ordered)))
	for _, item := range ordered {
		if len(result) == req.Limit {
			break
		}
		result = append(result, item.record)
	}
	return result, nil
}
