package memory

import (
	"context"
	"encoding/json"
	"time"
)

type recallBundle struct {
	AlwaysOnRules   []recallRule      `json:"always_on_rules"`
	ActivationHeld  int               `json:"activation_held"`
	Identity        []RecallRecord    `json:"identity"`
	Preferences     []RecallRecord    `json:"preferences"`
	ActiveContext   []RecallRecord    `json:"active_context"`
	OpenCommitments []RecallRecord    `json:"open_commitments"`
	Reminders       []recallReminder  `json:"reminders"`
	Directives      []recallDirective `json:"directives"`
	LimitTokens     int               `json:"limit_tokens"`
	UsedTokens      int               `json:"used_tokens"`
	ApproxTokens    int               `json:"approx_tokens"`
	ElapsedMS       float64           `json:"elapsed_ms"`
	BudgetExceeded  bool              `json:"budget_exceeded,omitempty"`
	SessionStart    bool              `json:"session_start"`
	Explain         []any             `json:"explain"`
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
	ID          int64  `json:"id"`
	Polarity    string `json:"polarity"`
	Title       string `json:"title"`
	Description string `json:"description"`
	Weight      int    `json:"weight"`
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

// Budget the serialized bundle, including aliases, escaping and metadata.
// Drop whole rows in reverse priority; hard rules survive until last. At the
// legacy 64-token minimum the empty envelope itself may exceed the budget:
// preserve its required arrays and explicitly report that unavoidable excess.
func (b *recallBundle) encodeBudgeted() ([]byte, error) {
	for {
		raw, err := json.Marshal(b)
		if err != nil {
			return nil, err
		}
		tokens := (len(raw) + 3) / 4
		if b.ApproxTokens != tokens || b.UsedTokens != tokens {
			b.ApproxTokens, b.UsedTokens = tokens, tokens
			continue
		}
		if tokens <= b.LimitTokens || b.BudgetExceeded {
			return raw, nil
		}
		switch {
		case len(b.Directives) > 0:
			b.Directives = b.Directives[:len(b.Directives)-1]
		case len(b.Reminders) > 0:
			b.Reminders = b.Reminders[:len(b.Reminders)-1]
		case len(b.OpenCommitments) > 0:
			b.OpenCommitments = b.OpenCommitments[:len(b.OpenCommitments)-1]
		case len(b.ActiveContext) > 0:
			b.ActiveContext = b.ActiveContext[:len(b.ActiveContext)-1]
		case len(b.Preferences) > 0:
			b.Preferences = b.Preferences[:len(b.Preferences)-1]
		case len(b.Identity) > 0:
			b.Identity = b.Identity[:len(b.Identity)-1]
		case len(b.AlwaysOnRules) > 0:
			b.AlwaysOnRules = b.AlwaysOnRules[:len(b.AlwaysOnRules)-1]
		default:
			b.BudgetExceeded = true
		}
	}
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
	rulesCap, identityCap, preferencesCap, activeCap, commitmentsCap, remindersCap, directivesCap := 8, 3, 4, 5, 3, 3, 2
	if sessionStart {
		rulesCap, identityCap, preferencesCap, activeCap, commitmentsCap, remindersCap, directivesCap = 16, 6, 8, 10, 6, 5, 5
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
		commitments, err = s.readRecallRecords(ctx, `SELECT id,scope_type,scope_value,tier,kind,key,content,confidence
FROM `+s.recallSource()+` WHERE lifecycle_state='pending' AND activation_suppressed=0 ORDER BY `+queryScopeOrder+`,updated_at DESC,id DESC LIMIT $1`, commitmentsCap)
	}
	if err != nil {
		return nil, err
	}
	reminders := make([]Prospective, 0)
	directives := make([]Directive, 0)
	rules := make([]recallRule, 0)
	// Structured reminder and directive relations currently belong to the
	// shared schema. Their absence must not prevent recall from a user store.
	if s.placement == PlacementKB {
		rules, err = s.recallHardRules(ctx, rulesCap)
		if err != nil {
			return nil, err
		}
		reminders, err = s.ProspectiveMatch(ctx, query, "", "", remindersCap)
		if err != nil {
			return nil, err
		}
		directives, err = s.DirectiveMatch(ctx, query, "", "", directivesCap)
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
		OpenCommitments: recallItems(commitments), AlwaysOnRules: rules, Reminders: []recallReminder{}, Directives: []recallDirective{},
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

func (s *postgresDataStore) recallHardRules(ctx context.Context, limit int) ([]recallRule, error) {
	rows, err := s.db.Query(ctx, `SELECT id,polarity,title,description,weight FROM rules WHERE directive_type='hard' ORDER BY weight DESC,title,id LIMIT $1`, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	rules := []recallRule{}
	for rows.Next() {
		var r recallRule
		if err := rows.Scan(&r.ID, &r.Polarity, &r.Title, &r.Description, &r.Weight); err != nil {
			return nil, err
		}
		rules = append(rules, r)
	}
	return rules, rows.Err()
}

func (s *postgresDataStore) recallOpenDirectives(ctx context.Context, limit int) ([]Directive, error) {
	rows, err := s.db.Query(ctx, `SELECT `+directiveColumns+` FROM epistemic_directives WHERE state='open'
 AND (valid_until='' OR rtrim(replace(valid_until,'T',' '),'Z') >= rtrim(replace(pg_now_text(),'T',' '),'Z'))
 ORDER BY priority DESC,created_at DESC,id DESC LIMIT $1`, limit)
	if err != nil {
		return nil, err
	}
	return scanDirectiveRows(rows)
}
