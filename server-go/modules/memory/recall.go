package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"time"
)

type recallBundle struct {
	RuleCollection  *typedSourceVersion `json:"rule_collection_source,omitempty"`
	AlwaysOnRules   []recallRule        `json:"always_on_rules"`
	ActivationHeld  int                 `json:"activation_held"`
	Identity        []RecallRecord      `json:"identity"`
	Preferences     []RecallRecord      `json:"preferences"`
	ActiveContext   []RecallRecord      `json:"active_context"`
	OpenCommitments []RecallRecord      `json:"open_commitments"`
	Reminders       []recallReminder    `json:"reminders"`
	Directives      []recallDirective   `json:"directives"`
	LimitTokens     int                 `json:"limit_tokens"`
	UsedTokens      int                 `json:"used_tokens"`
	ApproxTokens    int                 `json:"approx_tokens"`
	ElapsedMS       float64             `json:"elapsed_ms"`
	BudgetExceeded  bool                `json:"budget_exceeded,omitempty"`
	SessionStart    bool                `json:"session_start"`
	Explain         []any               `json:"explain"`
}

func recallTokenLimit(tokens int, sessionStart bool) int {
	if tokens <= 0 {
		if sessionStart {
			tokens = 1800
		} else {
			tokens = 600
		}
	}
	return min(max(tokens, 64), 8192)
}

type recallRule struct {
	Source      *typedSourceVersion `json:"source_version,omitempty"`
	ID          int64               `json:"id"`
	Polarity    string              `json:"polarity"`
	Title       string              `json:"title"`
	Description string              `json:"description"`
	Weight      int                 `json:"weight"`
}

// Keep the structured Go fields and the aliases used by existing prompt views.
type recallReminder struct {
	Prospective
	MemoryID int64  `json:"memory_id"`
	Tier     string `json:"tier"`
	Kind     string `json:"kind"`
	Key      string `json:"key"`
	Text     string `json:"text"`
	Why      string `json:"why"`
}

type recallDirective struct {
	Directive
	MemoryID int64  `json:"memory_id"`
	Tier     string `json:"tier"`
	Kind     string `json:"kind"`
	Key      string `json:"key"`
	Text     string `json:"text"`
	Why      string `json:"why"`
}

// Keep the complete stored hard-rule set. This does not confer authority on
// model-selected evidence: only the existing rules relation supplies this class.
// limit_tokens remains a legacy bytes/4 allocation, not a provider token count.
func (b *recallBundle) encodeBudgeted() ([]byte, error) {
	b.BudgetExceeded = false
	full, raw, err := b.encodePrefix(b.optionalCount())
	if err != nil {
		return nil, err
	}
	if full.ApproxTokens <= b.LimitTokens {
		*b = full
		return raw, nil
	}
	best, bestRaw, err := b.encodePrefix(0)
	if err != nil {
		return nil, err
	}
	if best.ApproxTokens > b.LimitTokens {
		if len(b.AlwaysOnRules) > 0 {
			return nil, protectedRecallOverflow()
		}
		// Preserve the legacy empty-envelope diagnostic, never a partial rule set.
		best.BudgetExceeded = true
		best, bestRaw, err = best.encodePrefix(0)
		if err == nil {
			*b = best
		}
		return bestRaw, err
	}
	// The retained prefix is monotonic in serialized size. Binary search avoids
	// serializing the entire remaining bundle for every row removed. Each probe
	// has bounded estimate convergence and keeps complete rows in priority order.
	low, high := 0, b.optionalCount()
	for low+1 < high {
		mid := low + (high-low)/2
		candidate, encoded, err := b.encodePrefix(mid)
		if err != nil {
			return nil, err
		}
		if candidate.ApproxTokens <= b.LimitTokens {
			low, best, bestRaw = mid, candidate, encoded
		} else {
			high = mid
		}
	}
	*b = best
	return bestRaw, nil
}

func protectedRecallOverflow() error {
	return &contextBudgetError{"protected_context_overflow", "complete hard rules exceed the recall allocation"}
}

func (b recallBundle) optionalCount() int {
	return len(b.Identity) + len(b.Preferences) + len(b.ActiveContext) + len(b.OpenCommitments) + len(b.Reminders) + len(b.Directives)
}

func (b recallBundle) encodePrefix(count int) (recallBundle, []byte, error) {
	take := func(n int) int { kept := min(n, count); count -= kept; return kept }
	b.Identity = b.Identity[:take(len(b.Identity))]
	b.Preferences = b.Preferences[:take(len(b.Preferences))]
	b.ActiveContext = b.ActiveContext[:take(len(b.ActiveContext))]
	b.OpenCommitments = b.OpenCommitments[:take(len(b.OpenCommitments))]
	b.Reminders = b.Reminders[:take(len(b.Reminders))]
	b.Directives = b.Directives[:take(len(b.Directives))]
	b.ApproxTokens, b.UsedTokens = 0, 0
	// Only the decimal widths of the two count fields can change the size.
	// Starting at zero makes convergence monotonic; eight passes also bounds
	// malformed/internal input instead of looping without a limit.
	for range 8 {
		raw, err := json.Marshal(b)
		if err != nil {
			return b, nil, err
		}
		tokens := (len(raw) + 3) / 4
		if b.ApproxTokens == tokens {
			return b, raw, nil
		}
		b.ApproxTokens, b.UsedTokens = tokens, tokens
	}
	return b, nil, fmt.Errorf("memory: recall accounting did not converge")
}

func (s *postgresDataStore) RecallBundle(ctx context.Context, query string, tokens int, sessionStart bool) (json.RawMessage, error) {
	return s.recallBundleActivated(ctx, query, tokens, sessionStart, nil)
}

func (s *postgresDataStore) RecallBundleWithActivation(ctx context.Context, query string, tokens int, sessionStart bool, raw json.RawMessage) (json.RawMessage, error) {
	var snapshot *ActivationSnapshot
	if s.placement == PlacementKB {
		snapshot = parseActivation(raw)
	}
	return s.recallBundleActivated(ctx, query, tokens, sessionStart, snapshot)
}

func (s *postgresDataStore) recallBundleActivated(ctx context.Context, query string, tokens int, sessionStart bool, snapshot *ActivationSnapshot) (json.RawMessage, error) {
	started := time.Now()
	defer runtimeMetricState.recallCalls.observe(started)
	tokens = recallTokenLimit(tokens, sessionStart)
	identityCap, preferencesCap, activeCap, commitmentsCap, remindersCap, directivesCap := 3, 4, 5, 3, 3, 2
	if sessionStart {
		identityCap, preferencesCap, activeCap, commitmentsCap, remindersCap, directivesCap = 6, 8, 10, 6, 5, 5
	}
	held := 0
	reasons := make(map[int64]string)
	fetch := func(where string, limit int, sticky bool, args ...any) ([]Record, error) {
		if snapshot == nil {
			return s.recallRecords(ctx, where, limit, args...)
		}
		records, why, count, err := s.recallActivated(ctx, snapshot, where, limit, sticky, false, args...)
		held += count
		for id, reason := range why {
			reasons[id] = reason
		}
		return records, err
	}
	identity, err := fetch(`kind='fact' AND tier IN ('L2','L3','L4','L5') AND
(key LIKE 'identity:%' OR key LIKE 'name:%' OR key LIKE 'role:%' OR key LIKE 'user:%' OR key LIKE 'self:%')`, identityCap, false)
	if err != nil {
		return nil, err
	}
	preferences, err := fetch(`kind='preference' AND tier IN ('L2','L3','L4','L5')`, preferencesCap, false)
	if err != nil {
		return nil, err
	}
	active, err := fetch(`$1='' OR key ILIKE '%'||$1||'%' OR content ILIKE '%'||$1||'%'`, activeCap, true, query)
	if err != nil {
		return nil, err
	}
	if s.placement == PlacementServer && query != "" {
		active, err = s.Search(ctx, Scope{Type: ScopeUser, Value: "_user"}, query, "", "", activeCap)
		if err != nil {
			return nil, err
		}
		// Search chooses IDs from lexical and optional dense lanes. Observe the
		// final payload and its version together before composing native context.
		ids := make([]int64, len(active))
		for i := range active {
			ids[i] = active[i].ID
		}
		active, err = s.readRecallRecords(ctx, `SELECT id,scope_type,scope_value,tier,kind,key,content,confidence`+s.recallVersionColumns()+`
FROM `+s.recallSource()+` WHERE lifecycle_state='active' AND id=ANY($1::text::bigint[])
ORDER BY array_position($1::text::bigint[],id)`, memoryIDsParameter(ids))
		if err != nil {
			return nil, err
		}
	}
	if s.placement == PlacementKB && query != "" {
		lexical := active
		active, err = s.fuseMemoryGraph(ctx, DataRequest{Query: query, IncludeAll: true, Limit: activeCap}, false, active)
		if err != nil {
			return nil, err
		}
		if snapshot != nil {
			var why map[int64]string
			var count int
			active, why, count, err = s.activationAfterFusion(ctx, snapshot, active, lexical, activeCap)
			if err != nil {
				return nil, err
			}
			held += count
			for id, reason := range why {
				reasons[id] = reason
			}
		}
	}
	var commitments []Record
	if snapshot != nil {
		var why map[int64]string
		var count int
		commitments, why, count, err = s.recallActivated(ctx, snapshot, "true", commitmentsCap, false, true)
		held += count
		for id, reason := range why {
			reasons[id] = reason
		}
	} else {
		commitments, err = s.readRecallRecords(ctx, `SELECT id,scope_type,scope_value,tier,kind,key,content,confidence`+s.recallVersionColumns()+`
FROM `+s.recallSource()+` WHERE lifecycle_state='pending' AND activation_suppressed=0 ORDER BY `+queryScopeOrder+`,updated_at DESC,id DESC LIMIT $1`, commitmentsCap)
	}
	if err != nil {
		return nil, err
	}
	reminders := make([]Prospective, 0)
	directives := make([]Directive, 0)
	rules := make([]recallRule, 0)
	var ruleCollection *typedSourceVersion
	// Structured reminder and directive relations currently belong to the
	// shared schema. Their absence must not prevent recall from a user store.
	if s.placement == PlacementKB {
		rules, ruleCollection, err = s.recallHardRules(ctx, tokens*4)
		if err != nil {
			return nil, err
		}
		reminders, err = s.prospectiveMatch(ctx, query, "", "", remindersCap, true)
		if err != nil {
			return nil, err
		}
		directives, err = s.directiveMatch(ctx, query, "", "", directivesCap, true)
		if err != nil {
			return nil, err
		}
		if len(directives) == 0 {
			directives, err = s.recallOpenDirectives(ctx, directivesCap)
			if err != nil {
				return nil, err
			}
		}
	}
	bundle := recallBundle{Identity: recallItems(identity), Preferences: recallItems(preferences), ActiveContext: recallItems(active),
		OpenCommitments: recallItems(commitments), AlwaysOnRules: rules, RuleCollection: ruleCollection, Reminders: []recallReminder{}, Directives: []recallDirective{},
		LimitTokens: tokens, SessionStart: sessionStart, Explain: []any{}}
	for _, r := range reminders {
		bundle.Reminders = append(bundle.Reminders, recallReminder{Prospective: r, MemoryID: r.ID, Kind: "reminder", Key: r.TriggerText, Text: r.ActionText, Why: "prospective matcher fired"})
	}
	for _, d := range directives {
		bundle.Directives = append(bundle.Directives, recallDirective{Directive: d, MemoryID: d.ID, Kind: "directive", Key: d.Topic, Text: d.Question, Why: "directive:" + d.Cause})
	}
	bundle.ActivationHeld = held
	for n, section := range [][]RecallRecord{bundle.Identity, bundle.Preferences, bundle.ActiveContext, bundle.OpenCommitments} {
		for i := range section {
			section[i].ActivationManaged = snapshot != nil
			section[i].Why = []string{"identity key prefix", "stable preference", "recent active context", "pending commitment"}[n]
			if reason := reasons[section[i].ID]; reason != "" {
				section[i].Why = reason
			}
		}
	}
	bundle.ElapsedMS = float64(time.Since(started).Microseconds()) / 1000
	encoded, err := bundle.encodeBudgeted()
	if err != nil {
		return nil, err
	}
	// Count only directives that survive the final budget. A failed write
	// propagates to the enclosing request transaction, which rolls back.
	for _, d := range bundle.Directives {
		if _, err := s.DirectiveMarkSurfaced(ctx, d.ID); err != nil {
			return nil, err
		}
	}
	if err == nil {
		runtimeMetricState.recallAssemblies.Add(1)
		if sessionStart {
			runtimeMetricState.recallStarts.Add(1)
		}
	}
	return encoded, err
}

func (s *postgresDataStore) recallHardRules(ctx context.Context, byteBudget int) ([]recallRule, *typedSourceVersion, error) {
	// Every rule occupies at least this many JSON bytes, even with empty text.
	// Fetch one beyond the maximum possible fit so a row cap cannot silently
	// omit mandatory rules. Bound cumulative raw text before it crosses the DB bus;
	// oversized content is refused, never served as a truncated rule.
	minimum, _ := json.Marshal(recallRule{})
	maxRows := byteBudget/len(minimum) + 1
	rows, err := s.db.Query(ctx, `WITH candidates AS MATERIALIZED (
 SELECT id,polarity,title,description,weight,record_revision FROM rules WHERE directive_type='hard' AND `+memoryUnexpiredAtSQL("expires_at", "CURRENT_TIMESTAMP")+`
 ORDER BY weight DESC,title,id LIMIT $2
), bounded AS (
 SELECT *,SUM(octet_length(polarity)::bigint+octet_length(title)+octet_length(description))
 OVER (ORDER BY weight DESC,title,id ROWS UNBOUNDED PRECEDING) AS text_bytes FROM candidates
)
SELECT COALESCE(bounded.id,0),CASE WHEN text_bytes <= $1 THEN polarity ELSE '' END,
 CASE WHEN text_bytes <= $1 THEN title ELSE '' END,
 CASE WHEN text_bytes <= $1 THEN description ELSE '' END,COALESCE(weight,0),COALESCE(text_bytes > $1,false),
 owner_id::text,rules_revision::text,COALESCE(record_revision,1)::text
 FROM memory_collection_owner LEFT JOIN bounded ON true WHERE memory_collection_owner.id=1 ORDER BY weight DESC,title,bounded.id`, byteBudget, maxRows)
	if err != nil {
		return nil, nil, err
	}
	defer rows.Close()
	rules := []recallRule{}
	var collection *typedSourceVersion
	used := 2 // JSON array brackets; metadata is counted by the final packer.
	for rows.Next() {
		var r recallRule
		var oversized bool
		var owner, generation, revision string
		if err := rows.Scan(&r.ID, &r.Polarity, &r.Title, &r.Description, &r.Weight, &oversized, &owner, &generation, &revision); err != nil {
			return nil, nil, err
		}
		collection, err = structuredSource("memory_rule_collection", owner, generation, "[]", 1)
		if err != nil {
			return nil, nil, err
		}
		if r.ID == 0 {
			continue
		}
		r.Source, err = structuredSource("memory_rule", owner, revision, "[]", r.ID)
		if err != nil {
			return nil, nil, err
		}
		if oversized {
			return nil, nil, protectedRecallOverflow()
		}
		raw, err := json.Marshal(r)
		if err != nil {
			return nil, nil, err
		}
		if len(rules) > 0 {
			used++
		}
		used += len(raw)
		if used > byteBudget {
			return nil, nil, protectedRecallOverflow()
		}
		rules = append(rules, r)
	}
	if collection == nil && rows.Err() == nil {
		return nil, nil, fmt.Errorf("memory rule owner unavailable")
	}
	return rules, collection, rows.Err()
}

func (s *postgresDataStore) recallOpenDirectives(ctx context.Context, limit int) ([]Directive, error) {
	rows, err := s.db.Query(ctx, `SELECT `+directiveColumns+structuredSourceColumns("epistemic_directives")+` FROM epistemic_directives WHERE state='open'
 AND `+memoryUnexpiredSQL("")+` AND `+currentDirectiveParentsSQL("epistemic_directives")+`
 ORDER BY priority DESC,created_at DESC,id DESC LIMIT $1`, limit)
	if err != nil {
		return nil, err
	}
	return scanDirectiveRows(rows, true)
}
