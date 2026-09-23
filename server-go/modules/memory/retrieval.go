package memory

import (
	"context"
	"fmt"
	"strings"
)

type Diagnostic struct {
	Memory        Record          `json:"memory"`
	Parts         DiagnosticParts `json:"parts"`
	EpistemicKind string          `json:"epistemic_kind,omitempty"`
}

type DiagnosticParts struct {
	ScoreEvidence string        `json:"score_evidence,omitempty"`
	RankingSteps  []rankingStep `json:"ranking_steps,omitempty"`

	RankingPolicy string  `json:"ranking_policy,omitempty"`
	RetrievalBase float64 `json:"retrieval_base,omitempty"`
	Entity        float64 `json:"entity"`
	Temporal      float64 `json:"temporal"`
	Evidence      float64 `json:"evidence"`
	Semantic      float64 `json:"semantic"`
	State         float64 `json:"state"`
	Intent        float64 `json:"intent"`
	Surprise      float64 `json:"surprise"`
	PageRank      float64 `json:"pagerank"`
	GraphScore    float64 `json:"graph_score"`
	GraphWeight   float64 `json:"graph_weight"`
	CodeProximity float64 `json:"code_proximity"`
	Utility       float64 `json:"utility"`
	Outcome       float64 `json:"outcome"`
	SourceFusion  float64 `json:"source_fusion"`
	Lexical       float64 `json:"lexical"`
	Coverage      float64 `json:"coverage"`
	Confidence    float64 `json:"confidence"`
	Salience      float64 `json:"salience"`
	HybridTotal   float64 `json:"hybrid_total"`
	BlendedTotal  float64 `json:"blended_total"`
	Total         float64 `json:"total"`
}

type AnswerResult struct {
	Evidence       AnswerEvidence `json:"evidence_trace"`
	Answer         string         `json:"answer"`
	Confidence     float64        `json:"confidence"`
	NoAnswer       bool           `json:"no_answer"`
	LowConfidence  bool           `json:"low_confidence"`
	EvidenceMode   string         `json:"evidence_mode"`
	RetrievalCount int            `json:"retrieval_count"`
	CitationIDs    []int64        `json:"citation_ids"`
	Error          string         `json:"error"`
}

// RecallRecord retains the record API fields and the prompt-consumer aliases.
// A scoped handle makes a recalled numeric ID unambiguous on a later get.
type RecallRecord struct {
	ActivationManaged bool   `json:"activation_managed,omitempty"`
	Why               string `json:"why,omitempty"`
	Record
	MemoryID int64  `json:"memory_id"`
	Text     string `json:"text"`
	Store    string `json:"store"`
	Handle   string `json:"handle"`
}

func recallItems(records []Record) []RecallRecord {
	items := make([]RecallRecord, 0, len(records))
	for _, record := range records {
		owner := "kb"
		if record.Scope.Type == ScopeUser {
			owner = "user"
		}
		items = append(items, RecallRecord{Record: record, MemoryID: record.ID,
			Text: record.Content, Store: owner, Handle: fmt.Sprintf("%s:memory:%d", owner, record.ID)})
	}
	return items
}

// recallSource projects the authorized store into one retrieval shape. This is
// storage adaptation, not a second recall implementation. Personal rows retain
// their user scope and expiry without exposing the shared store's table.
func (s *postgresDataStore) recallSource() string {
	if s.placement == PlacementServer {
		return `(SELECT id, 'user'::text AS scope_type, '_user'::text AS scope_value,
 tier, kind, key, content, confidence, use_count, updated_at, lifecycle_state,
 0 AS activation_suppressed FROM user_memories
 WHERE valid_until IS NULL OR valid_until > now()) AS recall_memories`
	}
	return `(SELECT * FROM memories WHERE ` + memoryValiditySQL("") + `) AS recall_memories`
}

func (s *postgresDataStore) recallRecords(ctx context.Context, where string, limit int, args ...any) ([]Record, error) {
	query := fmt.Sprintf(`SELECT id,scope_type,scope_value,tier,kind,key,content,confidence
FROM %s WHERE lifecycle_state='active' AND activation_suppressed=0 AND (%s)
ORDER BY `+queryScopeOrder+`,confidence DESC,use_count DESC,updated_at DESC,id DESC LIMIT $%d`, s.recallSource(), where, len(args)+1)
	args = append(args, limit)
	return s.readRecallRecords(ctx, query, args...)
}

func (s *postgresDataStore) readRecallRecords(ctx context.Context, query string, args ...any) ([]Record, error) {
	rows, err := s.db.Query(ctx, query, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	items := make([]Record, 0)
	for rows.Next() {
		var item Record
		if err := rows.Scan(&item.ID, &item.Scope.Type, &item.Scope.Value, &item.Tier,
			&item.Kind, &item.Key, &item.Content, &item.Confidence); err != nil {
			return nil, err
		}
		items = append(items, item)
	}
	return items, rows.Err()
}

func (s *postgresDataStore) AssembleContext(ctx context.Context, scope Scope, query, blockType string, limit int) (string, error) {
	if limit <= 0 {
		limit = 12
	}
	records, err := s.Search(ctx, scope, query, "", "", limit)
	if err != nil {
		return "", err
	}
	return renderMemoryContext(records, blockType), nil
}

func renderMemoryContext(records []Record, blockType string) string {
	text, _ := renderMemoryContextBounded(records, blockType, nil)
	return text
}

// A byte allocation retains a whole prefix in retrieval order. Each row is
// formatted once; headers and the terminal newline count toward the allocation.
func renderMemoryContextBounded(records []Record, blockType string, limit *int) (string, int) {
	var out strings.Builder
	header := "# Memory Context\n"
	if blockType != "" {
		header += fmt.Sprintf("\nType: %s\n", blockType)
	}
	if limit != nil && len(header) > *limit {
		return "", 0
	}
	out.WriteString(header)
	count := 0
	for _, item := range records {
		line := fmt.Sprintf("\n- [#%d] %s: %s", item.ID, item.Key, item.Content)
		if limit != nil && out.Len()+len(line)+1 > *limit {
			break
		}
		out.WriteString(line)
		count++
	}
	if count > 0 {
		out.WriteByte('\n')
	}
	return out.String(), count
}

// Confidence is display metadata and cannot cross the ranking input boundary.
type rankingInput struct{ Key, Content string }

func rankText(input rankingInput, query string) DiagnosticParts {
	lower := strings.ToLower(query)
	p := DiagnosticParts{}
	if lower != "" && (strings.Contains(strings.ToLower(input.Key), lower) || strings.Contains(strings.ToLower(input.Content), lower)) {
		p.Lexical = 0.65
	}
	terms := answerTerms(query)
	covered := 0
	for _, term := range terms {
		if strings.Contains(strings.ToLower(input.Key), term) || strings.Contains(strings.ToLower(input.Content), term) {
			covered++
		}
	}
	if len(terms) > 0 {
		p.Coverage = 0.35 * float64(covered) / float64(len(terms))
	}
	p.Total = p.Lexical + p.Coverage
	p.HybridTotal, p.BlendedTotal = p.Total, p.Total
	return p
}

func diagnosticFor(record Record, query string) Diagnostic {
	parts := DiagnosticParts{}
	if len(record.rankingSteps) == 0 && !record.pageRankApplied {
		parts = rankText(rankingInput{record.Key, record.Content}, query)
		parts.ScoreEvidence = "text_match_estimate"
	}
	if len(record.rankingSteps) > 0 {
		parts = DiagnosticParts{ScoreEvidence: "observed_final_score", RankingPolicy: "ordered-rrf60-v1",
			Total: record.retrievalScore, HybridTotal: record.retrievalScore, BlendedTotal: record.retrievalScore}
	}
	parts.Confidence = record.Confidence
	parts.GraphScore, parts.CodeProximity = record.graphScore, record.codeProximity
	if record.pageRankApplied {
		parts = DiagnosticParts{RankingPolicy: pageRankRecallPolicy, RetrievalBase: record.retrievalBase, PageRank: record.pageRankBonus, Confidence: record.Confidence, Total: record.retrievalScore, HybridTotal: record.retrievalScore, BlendedTotal: record.retrievalScore}
	}
	if len(record.rankingSteps) > 0 {
		parts.ScoreEvidence = "observed_ranking_steps"
		parts.RankingSteps = record.rankingSteps
	} else if record.pageRankApplied {
		parts.ScoreEvidence = "observed_final_score"
	}
	return Diagnostic{Memory: record, Parts: parts}
}

func (s *postgresDataStore) Diagnose(ctx context.Context, scope Scope, query string, limit int) ([]Diagnostic, error) {
	records, err := s.Search(withRankingTrace(ctx), scope, query, "", "", limit)
	if err != nil {
		return nil, err
	}
	result := make([]Diagnostic, 0, len(records))
	for _, item := range records {
		result = append(result, diagnosticFor(item, query))
	}
	return result, nil
}

func (s *postgresDataStore) Explain(ctx context.Context, scope Scope, query string, id int64) (Diagnostic, error) {
	record, err := s.Get(ctx, scope, id)
	if err != nil {
		return Diagnostic{}, err
	}
	return diagnosticFor(record, query), nil
}
