package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"
	"time"
)

type Diagnostic struct {
	Memory Record          `json:"memory"`
	Parts  DiagnosticParts `json:"parts"`
}

type DiagnosticParts struct {
	Lexical      float64 `json:"lexical"`
	Coverage     float64 `json:"coverage"`
	Confidence   float64 `json:"confidence"`
	Salience     float64 `json:"salience"`
	HybridTotal  float64 `json:"hybrid_total"`
	BlendedTotal float64 `json:"blended_total"`
	Total        float64 `json:"total"`
}

type AnswerResult struct {
	Answer         string  `json:"answer"`
	Confidence     float64 `json:"confidence"`
	NoAnswer       bool    `json:"no_answer"`
	LowConfidence  bool    `json:"low_confidence"`
	EvidenceMode   string  `json:"evidence_mode"`
	RetrievalCount int     `json:"retrieval_count"`
	CitationIDs    []int64 `json:"citation_ids"`
	Error          string  `json:"error"`
}

// RecallRecord retains the record API fields and the prompt-consumer aliases.
// A scoped handle makes a recalled numeric ID unambiguous on a later get.
type RecallRecord struct {
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

type recallBundle struct {
	Identity        []RecallRecord `json:"identity"`
	Preferences     []RecallRecord `json:"preferences"`
	ActiveContext   []RecallRecord `json:"active_context"`
	OpenCommitments []RecallRecord `json:"open_commitments"`
	Reminders       []Prospective  `json:"reminders"`
	Directives      []Directive    `json:"directives"`
	LimitTokens     int            `json:"limit_tokens"`
	UsedTokens      int            `json:"used_tokens"`
	SessionStart    bool           `json:"session_start"`
	Explain         []any          `json:"explain"`
}

func retrievalLimit(tokens int, sessionStart bool) int {
	if tokens <= 0 {
		if sessionStart {
			tokens = 1800
		} else {
			tokens = 600
		}
	}
	limit := tokens / 96
	if limit < 4 {
		limit = 4
	}
	if limit > 40 {
		limit = 40
	}
	return limit
}

func approximateTokens(records ...[]Record) int {
	bytes := 0
	for _, group := range records {
		for _, item := range group {
			bytes += len(item.Key) + len(item.Content) + 24
		}
	}
	return (bytes + 3) / 4
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
	return "memories"
}

func (s *postgresDataStore) recallRecords(ctx context.Context, where string, limit int, args ...any) ([]Record, error) {
	query := fmt.Sprintf(`SELECT id,scope_type,scope_value,tier,kind,key,content,confidence
FROM %s WHERE lifecycle_state='active' AND activation_suppressed=0 AND (%s)
ORDER BY confidence DESC,use_count DESC,updated_at DESC,id DESC LIMIT $%d`, s.recallSource(), where, len(args)+1)
	args = append(args, limit)
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

func (s *postgresDataStore) RecallBundle(ctx context.Context, query string, tokens int, sessionStart bool) (json.RawMessage, error) {
	started := time.Now()
	defer runtimeMetricState.recallCalls.observe(started)
	limit := retrievalLimit(tokens, sessionStart)
	identity, err := s.recallRecords(ctx, `kind='fact' AND tier IN ('L2','L3','L4','L5') AND
(key ILIKE '%name%' OR key ILIKE '%role%' OR key ILIKE '%identity%')`, limit/4+1)
	if err != nil {
		return nil, err
	}
	preferences, err := s.recallRecords(ctx, `kind='preference' AND tier IN ('L2','L3','L4','L5')`, limit/4+1)
	if err != nil {
		return nil, err
	}
	active, err := s.recallRecords(ctx, `$1='' OR key ILIKE '%'||$1||'%' OR content ILIKE '%'||$1||'%'`, limit/2+1, query)
	if err != nil {
		return nil, err
	}
	rows, err := s.db.Query(ctx, `SELECT id,scope_type,scope_value,tier,kind,key,content,confidence
FROM `+s.recallSource()+` WHERE lifecycle_state='pending' ORDER BY updated_at DESC,id DESC LIMIT $1`, limit/4+1)
	if err != nil {
		return nil, err
	}
	commitments := make([]Record, 0)
	for rows.Next() {
		var item Record
		if scanErr := rows.Scan(&item.ID, &item.Scope.Type, &item.Scope.Value, &item.Tier,
			&item.Kind, &item.Key, &item.Content, &item.Confidence); scanErr != nil {
			rows.Close()
			return nil, scanErr
		}
		commitments = append(commitments, item)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return nil, err
	}
	reminders := make([]Prospective, 0)
	directives := make([]Directive, 0)
	// Structured reminder and directive relations currently belong to the
	// shared schema. Their absence must not prevent recall from a user store.
	if s.placement == PlacementKB {
		reminders, err = s.ProspectiveMatch(ctx, query, "", "", 8)
		if err != nil {
			return nil, err
		}
		directives, err = s.DirectiveMatch(ctx, query, "", "", 4)
		if err != nil {
			return nil, err
		}
	}
	if tokens <= 0 {
		if sessionStart {
			tokens = 1800
		} else {
			tokens = 600
		}
	}
	bundle := recallBundle{Identity: recallItems(identity), Preferences: recallItems(preferences), ActiveContext: recallItems(active),
		OpenCommitments: recallItems(commitments), Reminders: reminders, Directives: directives,
		LimitTokens: tokens, SessionStart: sessionStart, Explain: []any{}}
	bundle.UsedTokens = approximateTokens(identity, preferences, active, commitments)
	encoded, err := json.Marshal(bundle)
	if err == nil {
		runtimeMetricState.recallAssemblies.Add(1)
		if sessionStart {
			runtimeMetricState.recallStarts.Add(1)
		}
	}
	return encoded, err
}

type briefingActivity struct {
	SessionID     string `json:"session_id"`
	Summary       string `json:"summary"`
	ReferenceTime string `json:"reference_time"`
	CreatedAt     string `json:"created_at"`
}

type briefingEntity struct {
	Name     string `json:"name"`
	Mentions int    `json:"mentions"`
	LastSeen string `json:"last_seen"`
}

func (s *postgresDataStore) BriefingBundle(ctx context.Context, tokens int) (json.RawMessage, error) {
	if err := s.requireKBDomain(); err != nil {
		return nil, err
	}
	if tokens <= 0 {
		tokens = 2048
	}
	limit := retrievalLimit(tokens, true)
	facts, err := s.recallRecords(ctx, `tier IN ('L2','L3','L4','L5')`, limit)
	if err != nil {
		return nil, err
	}
	activities := make([]briefingActivity, 0)
	rows, err := s.db.Query(ctx, `SELECT source_session,episode_text,reference_time,created_at
FROM memory_episodes ORDER BY reference_time DESC,created_at DESC LIMIT $1`, limit/3+1)
	if err != nil {
		return nil, err
	}
	for rows.Next() {
		var item briefingActivity
		if err := rows.Scan(&item.SessionID, &item.Summary, &item.ReferenceTime, &item.CreatedAt); err != nil {
			rows.Close()
			return nil, err
		}
		activities = append(activities, item)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return nil, err
	}
	entities := make([]briefingEntity, 0)
	rows, err = s.db.Query(ctx, `SELECT entity,COUNT(*),MAX(m.updated_at) FROM memory_entities me
JOIN memories m ON m.id=me.memory_id WHERE m.lifecycle_state='active'
GROUP BY entity ORDER BY COUNT(*) DESC,entity LIMIT $1`, limit/3+1)
	if err != nil {
		return nil, err
	}
	for rows.Next() {
		var item briefingEntity
		if err := rows.Scan(&item.Name, &item.Mentions, &item.LastSeen); err != nil {
			rows.Close()
			return nil, err
		}
		entities = append(entities, item)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return nil, err
	}
	return json.Marshal(map[string]any{"key_facts": facts, "recent_activity": activities,
		"active_entities": entities, "style": "compact", "limit_tokens": tokens,
		"approx_tokens": approximateTokens(facts)})
}

func (s *postgresDataStore) AlertsBundle(ctx context.Context, since string) (json.RawMessage, error) {
	if err := s.requireKBDomain(); err != nil {
		return nil, err
	}
	if since == "" {
		since = "1970-01-01T00:00:00Z"
	}
	stale := make([]map[string]any, 0)
	rows, err := s.db.Query(ctx, `SELECT id,content,created_at,ttl_at FROM memories
WHERE lifecycle_state='pending' AND ttl_at<>'' AND ttl_at<=pg_now_text('+2 days')
ORDER BY ttl_at LIMIT 64`)
	if err != nil {
		return nil, err
	}
	for rows.Next() {
		var id int64
		var content, created, ttl string
		if err := rows.Scan(&id, &content, &created, &ttl); err != nil {
			rows.Close()
			return nil, err
		}
		stale = append(stale, map[string]any{"memory_id": id, "text": content, "created_at": created, "ttl_at": ttl})
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return nil, err
	}
	conflicts, err := s.ConflictList(ctx, 64)
	if err != nil {
		return nil, err
	}
	superseded := make([]Record, 0)
	rows, err = s.db.Query(ctx, `SELECT id,scope_type,scope_value,tier,kind,key,content,confidence
FROM memories WHERE lifecycle_state='superseded' AND updated_at>=$1 ORDER BY updated_at DESC LIMIT 64`, since)
	if err != nil {
		return nil, err
	}
	for rows.Next() {
		var item Record
		if err := rows.Scan(&item.ID, &item.Scope.Type, &item.Scope.Value, &item.Tier,
			&item.Kind, &item.Key, &item.Content, &item.Confidence); err != nil {
			rows.Close()
			return nil, err
		}
		superseded = append(superseded, item)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return nil, err
	}
	return json.Marshal(map[string]any{"stale_pending": stale, "unresolved_contradictions": conflicts,
		"newly_superseded": superseded})
}

func (s *postgresDataStore) AssembleContext(ctx context.Context, scope Scope, query, blockType string, limit int) (string, error) {
	if limit <= 0 {
		limit = 12
	}
	records, err := s.Search(ctx, scope, query, "", "", limit)
	if err != nil {
		return "", err
	}
	var out strings.Builder
	out.WriteString("# Memory Context\n")
	if blockType != "" {
		fmt.Fprintf(&out, "\nType: %s\n", blockType)
	}
	for _, item := range records {
		fmt.Fprintf(&out, "\n- [#%d] %s: %s", item.ID, item.Key, item.Content)
	}
	if len(records) > 0 {
		out.WriteByte('\n')
	}
	return out.String(), nil
}

func diagnosticFor(record Record, query string) Diagnostic {
	lower := strings.ToLower(query)
	lexical := 0.0
	if lower != "" && (strings.Contains(strings.ToLower(record.Key), lower) ||
		strings.Contains(strings.ToLower(record.Content), lower)) {
		lexical = 1
	}
	total := 0.65*lexical + 0.35*record.Confidence
	return Diagnostic{Memory: record, Parts: DiagnosticParts{Lexical: lexical, Coverage: lexical,
		Confidence: record.Confidence, Salience: record.Confidence, HybridTotal: total,
		BlendedTotal: total, Total: total}}
}

func (s *postgresDataStore) Diagnose(ctx context.Context, scope Scope, query string, limit int) ([]Diagnostic, error) {
	records, err := s.Search(ctx, scope, query, "", "", limit)
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

func (s *postgresDataStore) Ask(ctx context.Context, scope Scope, query string, limit int) (AnswerResult, error) {
	records, err := s.Search(ctx, scope, query, "", "", limit)
	if err != nil {
		return AnswerResult{}, err
	}
	result := AnswerResult{NoAnswer: len(records) == 0, EvidenceMode: "memory", RetrievalCount: len(records)}
	if len(records) == 0 {
		return result, nil
	}
	var answer strings.Builder
	maxConfidence := 0.0
	for i, item := range records {
		if i > 0 {
			answer.WriteString(" ")
		}
		fmt.Fprintf(&answer, "%s [#%d]", item.Content, item.ID)
		result.CitationIDs = append(result.CitationIDs, item.ID)
		if item.Confidence > maxConfidence {
			maxConfidence = item.Confidence
		}
	}
	result.Answer = answer.String()
	result.Confidence = maxConfidence
	result.LowConfidence = maxConfidence < 0.5
	return result, nil
}
