package memory

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"strconv"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/egress"
)

type typedContextOptions struct {
	Enabled bool            `json:"enabled"`
	Flags   map[string]bool `json:"flags"`
	Budgets map[string]int  `json:"budgets"`
	Turns   []string        `json:"turns"`
	Latest  string          `json:"latest"`
}

var typedChannelOrder = []string{"current_assertions", "historical_assertions", "episodes", "summaries", "observations", "approved_procedures", "working_context"}
var typedBudgetDefaults = map[string]int{"total": 2400, "current_assertions": 800, "historical_assertions": 400, "episodes": 500, "summaries": 300, "observations": 500, "approved_procedures": 500, "working_context": 500}

type typedPackTrace struct {
	Channel  string `json:"channel"`
	ID       string `json:"stable_id"`
	Tokens   int    `json:"estimated_tokens"`
	Decision string `json:"decision"`
	Reason   string `json:"reason"`
}
type typedItem struct {
	value    any
	id, text string
	tokens   int
}
type typedChannel struct {
	Enabled  bool   `json:"enabled"`
	Budget   int    `json:"budget_tokens"`
	Used     int    `json:"used_tokens"`
	Status   string `json:"status"`
	Reason   string `json:"reason,omitempty"`
	Items    []any  `json:"items"`
	selected []typedItem
}
type typedWatermark struct {
	Durable      string `json:"durable_at"`
	Observations string `json:"observations_at"`
	Latest       string `json:"latest_turn_at"`
	Lag          bool   `json:"durable_lag"`
	Status       string `json:"status"`
	Reason       string `json:"reason,omitempty"`
}
type typedContextResult struct {
	Status         string                   `json:"status"`
	Enabled        bool                     `json:"default_injection"`
	Budget         int                      `json:"total_budget_tokens"`
	Used           int                      `json:"used_tokens"`
	RenderedTokens int                      `json:"rendered_tokens"`
	EnvelopeExcess int                      `json:"envelope_excess_tokens,omitempty"`
	Channels       map[string]*typedChannel `json:"channels"`
	Trace          []typedPackTrace         `json:"packing_trace"`
	Watermark      typedWatermark           `json:"watermark"`
	Sufficiency    string                   `json:"context_sufficiency"`
	Reason         string                   `json:"sufficiency_reason"`
	Rendered       string                   `json:"rendered_context"`
	MissingContext bool                     `json:"active_context_missing"`
	ErrorType      string                   `json:"error_type,omitempty"`
	Message        string                   `json:"message,omitempty"`
	degraded       bool
}

func typedEstimate(text string) int {
	if text == "" {
		return 0
	}
	return len(text)/4 + 1
}
func typedOptions(args commandArgs) *typedContextOptions {
	flag := func(name string, fallback bool) bool {
		if _, ok := args[name]; !ok {
			return fallback
		}
		return args.boolean(name)
	}
	enabled := flag("enabled", true)
	semantic := enabled && flag("enable_semantic_assertions", true)
	out := &typedContextOptions{Enabled: enabled, Flags: map[string]bool{"current_assertions": semantic, "historical_assertions": semantic && flag("enable_historical", false), "episodes": enabled && flag("enable_episodes", false), "summaries": enabled && flag("enable_summaries", false), "observations": enabled && flag("enable_observations", true), "approved_procedures": enabled && flag("enable_approved_procedures", true), "working_context": enabled && flag("enable_working_context", false)}, Budgets: map[string]int{}, Latest: args.stringOr("latest_turn_at", "")}
	var budgets commandArgs
	_ = json.Unmarshal(args["channel_budgets"], &budgets)
	for name, fallback := range typedBudgetDefaults {
		out.Budgets[name] = fallback
		if n, ok := budgets.number(name); ok {
			out.Budgets[name] = int(math.Max(0, math.Min(n, 4096)))
		}
	}
	var turns []json.RawMessage
	_ = json.Unmarshal(args["recent_turns"], &turns)
	for _, raw := range turns {
		var text string
		if string(raw) != "null" && json.Unmarshal(raw, &text) == nil {
			out.Turns = append(out.Turns, text)
		}
	}
	return out
}
func handleTypedContext(options handlerOptions, invocation bus.ModuleInvocation, args commandArgs) ([]byte, bus.ModuleStatus) {
	if options.placement != PlacementKB {
		return nil, bus.ModuleStatusCapabilityAbsent
	}
	query, ok := args.stringValue("query")
	if !ok || query == "" {
		return nil, bus.ModuleStatusInvalidRequest
	}
	cfg := typedOptions(args)
	request := DataRequest{Operation: "typed-context", Query: query, Limit: 32, TypedContext: cfg, Assertions: &assertionSearchRequest{ValidAt: args.stringOr("valid_at", ""), BelievedAt: args.stringOr("believed_at", ""), Historical: cfg.Flags["historical_assertions"], Hops: min(2, max(0, args.integer("max_hops", 0)))}, Project: args.stringOr("project", ""), Workspace: args.stringOr("workspace", ""), IncludeAll: args.boolean("include_all")}
	if raw, ok := args["scope"]; ok && json.Unmarshal(raw, &request.Scope) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	body, _ := json.Marshal(request)
	raw, status := handleData(options, invocation, body)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	var response DataResponse
	if json.Unmarshal(raw, &response) != nil || len(response.Payload) == 0 {
		return nil, bus.ModuleStatusInternal
	}
	// Native transports carry the complete owner JSON as text, preserving numeric
	// IDs even through cJSON. The public shape is that JSON object, not this envelope.
	return commandResult(map[string]string{"json": string(response.Payload)})
}
func newTypedContext(request DataRequest) *typedContextResult {
	cfg := request.TypedContext
	result := &typedContextResult{Status: "ok", Enabled: cfg.Enabled, Budget: cfg.Budgets["total"], Channels: map[string]*typedChannel{}, Trace: []typedPackTrace{}, MissingContext: request.Project == "" && request.Workspace == ""}
	for _, name := range typedChannelOrder {
		status := "disabled"
		if cfg.Flags[name] {
			status = "ok"
		}
		result.Channels[name] = &typedChannel{Enabled: cfg.Flags[name], Budget: cfg.Budgets[name], Status: status, Items: []any{}}
	}
	return result
}
func (r *typedContextResult) trace(name, id string, tokens int, included bool, reason string) {
	decision := "dropped"
	if included {
		decision = "included"
	}
	r.Trace = append(r.Trace, typedPackTrace{name, id, tokens, decision, reason})
}
func (r *typedContextResult) add(name string, item typedItem) {
	c := r.Channels[name]
	tokens := typedEstimate(item.text)
	include := c.Enabled && c.Used+tokens <= c.Budget && r.Used+tokens <= r.Budget
	reason := "channel or total token budget exhausted"
	if include {
		reason = "ranked evidence fit channel and total budgets"
		item.tokens = tokens
		c.Items = append(c.Items, item.value)
		c.selected = append(c.selected, item)
		c.Used += tokens
		r.Used += tokens
	}
	r.trace(name, item.id, tokens, include, reason)
}
func (r *typedContextResult) fail(name, reason string) {
	r.degraded = true
	r.Channels[name].Status = "degraded"
	r.Channels[name].Reason = reason
}
func (r *typedContextResult) render() (string, error) {
	channels, err := json.Marshal(r.Channels)
	if err != nil {
		return "", err
	}
	procedures, err := json.Marshal(r.Channels["approved_procedures"].Items)
	if err != nil {
		return "", err
	}
	return `<memory_data trust="untrusted" authorization="none">` + string(channels) + "</memory_data>\n" + `<approved_procedures authority="reviewed" authorization="none">` + string(procedures) + `</approved_procedures>`, nil
}
func (r *typedContextResult) finish() error {
	// The legacy per-channel estimates remain visible in used_tokens. Also bound
	// the complete rendered context, including JSON and both trust envelopes.
	// Drop whole rows in reverse packing order; never truncate structured evidence.
	for {
		rendered, err := r.render()
		if err != nil {
			return err
		}
		r.Rendered = rendered
		r.RenderedTokens = typedEstimate(rendered)
		if r.RenderedTokens <= r.Budget {
			break
		}
		removed := false
		for i := len(typedChannelOrder) - 1; i >= 0; i-- {
			name := typedChannelOrder[i]
			c := r.Channels[name]
			n := len(c.Items)
			if n == 0 {
				continue
			}
			item := c.selected[n-1]
			c.Items = c.Items[:n-1]
			c.selected = c.selected[:n-1]
			c.Used -= item.tokens
			r.Used -= item.tokens
			r.trace(name, item.id, item.tokens, false, "complete rendered context exceeds total budget")
			removed = true
			break
		}
		if !removed {
			r.EnvelopeExcess = r.RenderedTokens - r.Budget
			break
		}
	}
	count := 0
	for _, c := range r.Channels {
		count += len(c.Items)
	}
	switch {
	case count == 0:
		r.Sufficiency = "insufficient"
		r.Reason = "no authorized evidence fit enabled channels"
	case r.degraded:
		r.Sufficiency = "partial"
		r.Reason = "authorized evidence present but a requested channel degraded"
	default:
		r.Sufficiency = "complete"
		r.Reason = "authorized evidence present in every available requested channel"
	}
	return nil
}
func (s *postgresDataStore) typedRead(ctx context.Context, run func() error) error {
	if _, err := s.db.Exec(ctx, `SAVEPOINT typed_context_channel`); err != nil {
		return err
	}
	err := run()
	if err != nil {
		if _, cleanup := s.db.Exec(ctx, `ROLLBACK TO SAVEPOINT typed_context_channel`); cleanup != nil {
			return errors.Join(err, cleanup)
		}
	}
	_, cleanup := s.db.Exec(ctx, `RELEASE SAVEPOINT typed_context_channel`)
	return errors.Join(err, cleanup)
}

const typedScopeSQL = `($1 OR scope_kind='global' OR (scope_kind='workspace' AND $2<>'' AND scope_id=$2) OR (scope_kind='project' AND $3<>'' AND scope_id=$3)) AND ($4='' OR (scope_kind=$4 AND ($4='global' OR scope_id=$5)))`

func typedScopeParams(request DataRequest, exact Scope) []any {
	return []any{request.IncludeAll, request.Workspace, request.Project, exact.Type, exact.Value}
}
func (s *postgresDataStore) typedObservations(ctx context.Context, request DataRequest, exact Scope) ([]typedItem, error) {
	rows, err := s.db.Query(ctx, `SELECT observation_id,observation_type,title,summary,confidence,evidence_count FROM learning_observations WHERE status='active' AND `+typedScopeSQL+` ORDER BY refreshed_at DESC,observation_id LIMIT 64`, typedScopeParams(request, exact)...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var items []typedItem
	for rows.Next() {
		var id, kind, title, summary string
		var confidence float64
		var evidence int64
		if err = rows.Scan(&id, &kind, &title, &summary, &confidence, &evidence); err != nil {
			return nil, err
		}
		items = append(items, typedItem{value: map[string]any{"observation_id": id, "type": kind, "title": title, "summary": summary, "confidence": confidence, "evidence_count": evidence, "authority": "derived_read_only"}, id: id, text: summary})
	}
	return items, rows.Err()
}
func (s *postgresDataStore) typedProcedures(ctx context.Context, request DataRequest, exact Scope) ([]typedItem, error) {
	rows, err := s.db.Query(ctx, `WITH proposals AS (SELECT id,target_key,action_json,CASE WHEN action_json IS JSON OBJECT THEN action_json::jsonb ELSE '{}'::jsonb END AS action FROM learning_proposals WHERE state='committed' AND sink='artifact'), scoped AS (SELECT *,COALESCE(action->>'scope_kind','') AS scope_kind,COALESCE(action->>'scope_id','') AS scope_id FROM proposals) SELECT id,target_key,action_json FROM scoped WHERE action_json IS JSON OBJECT AND `+typedScopeSQL+` ORDER BY id DESC LIMIT 32`, typedScopeParams(request, exact)...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var items []typedItem
	for rows.Next() {
		var id int64
		var key, action string
		if err = rows.Scan(&id, &key, &action); err != nil {
			return nil, err
		}
		items = append(items, typedItem{value: map[string]any{"proposal_id": id, "target_key": key, "state": "committed", "procedure": json.RawMessage(action)}, id: key, text: action})
	}
	return items, rows.Err()
}
func (s *postgresDataStore) typedWatermarks(ctx context.Context, request DataRequest, exact Scope) (typedWatermark, error) {
	result := typedWatermark{Latest: request.TypedContext.Latest, Status: "unknown"}
	// A hidden parent denies assertion timestamps just as it denies assertion text.
	err := s.db.QueryRow(ctx, `SELECT COALESCE(max(ts),'') FROM (
 SELECT asserted_at AS ts FROM entity_edges e WHERE edge_class='semantic' AND NOT EXISTS(
 SELECT 1 FROM fact_evidence f LEFT JOIN memories m ON f.source_id='memory:'||m.id::text AND m.lifecycle_state='active' AND m.activation_suppressed=0
 WHERE f.assertion_id=e.id AND f.source_kind='memory' AND f.invalidated_at='' AND (m.id IS NULL OR ($1<>'' AND (m.scope_type<>$1 OR m.scope_value<>$2))))
 UNION ALL SELECT me.created_at FROM memory_episodes me JOIN memories m ON m.id=me.memory_id WHERE m.lifecycle_state='active' AND m.activation_suppressed=0 AND ($1='' OR (m.scope_type=$1 AND m.scope_value=$2))) q`, exact.Type, exact.Value).Scan(&result.Durable)
	if err != nil {
		return result, err
	}
	err = s.db.QueryRow(ctx, `SELECT COALESCE(max(refreshed_at),'') FROM learning_observations WHERE `+typedScopeSQL, typedScopeParams(request, exact)...).Scan(&result.Observations)
	if result.Durable != "" {
		result.Status = "known"
	}
	result.Lag = result.Latest != "" && (result.Durable == "" || result.Durable < result.Latest)
	return result, err
}
func (s *postgresDataStore) assembleTypedContext(ctx context.Context, trace uint64, executor egress.Executor, request DataRequest, explicit bool) (typedContextResult, error) {
	r := newTypedContext(request)
	cfg := request.TypedContext
	exact := Scope{}
	if explicit {
		exact = request.Scope
	}
	invalid := cfg.Flags["current_assertions"] && (!assertionTimestamp(request.Assertions.ValidAt) || !assertionTimestamp(request.Assertions.BelievedAt))
	if invalid {
		r.Status = "error"
		r.ErrorType = "invalid_timestamp"
		r.Message = "timestamps must be second-precision UTC date-times"
		r.Channels["current_assertions"].Status = "error"
		r.Channels["current_assertions"].Reason = "invalid temporal request"
	}
	if !invalid && cfg.Flags["current_assertions"] {
		var result map[string]any
		err := s.typedRead(ctx, func() error {
			var err error
			result, err = s.searchAssertions(ctx, trace, executor, request, explicit)
			return err
		})
		if err != nil {
			r.fail("current_assertions", "semantic retrieval unavailable")
		} else {
			if result["channel_status"] != "ok" {
				r.fail("current_assertions", "lexical fallback; vector unavailable")
			}
			for _, h := range result["assertions"].([]assertionHit) {
				name := "current_assertions"
				if h.Historical {
					name = "historical_assertions"
				}
				if !cfg.Flags[name] {
					r.trace(name, h.StableID, typedEstimate(h.Rendered), false, "historical channel explicitly disabled")
					continue
				}
				r.add(name, typedItem{value: h, id: h.StableID, text: h.Rendered})
			}
		}
	}
	if !invalid && cfg.Flags["episodes"] {
		var episodes []Episode
		err := s.typedRead(ctx, func() error { var err error; episodes, err = s.episodeList(ctx, request.Query, 16, exact); return err })
		if err != nil {
			r.fail("episodes", "episode retrieval unavailable")
		} else {
			for _, e := range episodes {
				id := strconv.FormatInt(e.ID, 10)
				r.add("episodes", typedItem{value: map[string]any{"stable_id": id, "episode_key": e.Key, "excerpt": e.Text, "source_session": e.SourceSession, "reference_time": e.ReferenceTime, "trust": "untrusted_data"}, id: id, text: e.Text})
			}
		}
	}
	if !invalid && cfg.Flags["summaries"] {
		var profile EntityProfile
		err := s.typedRead(ctx, func() error {
			var err error
			profile, err = s.entityProfile(ctx, request.Query, exact)
			if errors.Is(err, ErrMemoryNotFound) {
				return nil
			}
			return err
		})
		if err != nil {
			r.fail("summaries", "entity summary unavailable")
		} else if profile.Summary != "" {
			r.add("summaries", typedItem{value: map[string]any{"entity": profile.Entity, "summary": profile.Summary, "authority": "derived_noncanonical"}, id: profile.Entity, text: profile.Summary})
		}
	}
	for _, channel := range []struct {
		name, reason string
		load         func(context.Context, DataRequest, Scope) ([]typedItem, error)
	}{{"observations", "observation synthesis unavailable", s.typedObservations}, {"approved_procedures", "reviewed procedure store unavailable", s.typedProcedures}} {
		if invalid || !cfg.Flags[channel.name] {
			continue
		}
		var items []typedItem
		err := s.typedRead(ctx, func() error { var err error; items, err = channel.load(ctx, request, exact); return err })
		if err != nil {
			r.fail(channel.name, channel.reason)
		} else {
			for _, item := range items {
				r.add(channel.name, item)
			}
		}
	}
	if !invalid && cfg.Flags["working_context"] {
		for i, text := range cfg.Turns {
			id := fmt.Sprintf("turn:%d", i)
			r.add("working_context", typedItem{value: map[string]any{"stable_id": id, "text": text, "authority": "ephemeral_untrusted_data"}, id: id, text: text})
		}
	}
	err := s.typedRead(ctx, func() error { var err error; r.Watermark, err = s.typedWatermarks(ctx, request, exact); return err })
	if err != nil {
		r.degraded = true
		r.Watermark = typedWatermark{Latest: cfg.Latest, Status: "unavailable", Reason: "memory watermark unavailable"}
	}
	if err = r.finish(); err != nil {
		return *r, err
	}
	if invalid {
		r.Reason = "invalid temporal request; no context assembled"
	}
	return *r, nil
}
