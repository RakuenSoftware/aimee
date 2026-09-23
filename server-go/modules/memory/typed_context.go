package memory

import (
	"context"
	"crypto/sha256"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"sort"
	"strconv"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/egress"
)

type typedContextOptions struct {
	Requirements  *evidenceRequirementSet `json:"evidence_requirements,omitempty"`
	ContextLimits *ContextLimits          `json:"context_limits,omitempty"`
	Enabled       bool                    `json:"enabled"`
	Flags         map[string]bool         `json:"flags"`
	Budgets       map[string]int          `json:"budgets"`
	Turns         []string                `json:"turns"`
	Latest        string                  `json:"latest"`
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
	source   *typedSourceVersion
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
	Requirements       *evidenceRequirementSet `json:"evidence_requirements,omitempty"`
	Coverage           *evidenceCoverage       `json:"evidence_coverage,omitempty"`
	coverageCandidates []typedItem
	coveragePrior      *evidenceCoverage
	Accounting         ContextAccounting        `json:"context_accounting"`
	ProjectionVersion  int                      `json:"projection_schema_version"`
	SelectionDigest    string                   `json:"selection_digest"`
	ProjectionDigest   string                   `json:"projection_digest"`
	RenderedBytes      int                      `json:"rendered_bytes"`
	TokenCountState    string                   `json:"token_count_state"`
	Retained           []typedProjectionRef     `json:"retained_items"`
	SourceVersionState string                   `json:"source_version_state"`
	Availability       string                   `json:"retrieval_availability"`
	Status             string                   `json:"status"`
	Enabled            bool                     `json:"default_injection"`
	Budget             int                      `json:"total_budget_tokens"`
	Used               int                      `json:"used_tokens"`
	RenderedTokens     int                      `json:"rendered_tokens"`
	EnvelopeExcess     int                      `json:"envelope_excess_tokens,omitempty"`
	Channels           map[string]*typedChannel `json:"channels"`
	Trace              []typedPackTrace         `json:"packing_trace"`
	Watermark          typedWatermark           `json:"watermark"`
	Sufficiency        string                   `json:"context_sufficiency"`
	Reason             string                   `json:"sufficiency_reason"`
	Rendered           string                   `json:"rendered_context"`
	MissingContext     bool                     `json:"active_context_missing"`
	ErrorType          string                   `json:"error_type,omitempty"`
	Message            string                   `json:"message,omitempty"`
	degraded           bool
	limits             *ContextLimits
}

type typedProjectionRef struct {
	Channel string              `json:"channel"`
	ID      string              `json:"stable_id"`
	Source  *typedSourceVersion `json:"source_version,omitempty"`
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
	return handleTypedContextResult(options, invocation, args, true)
}

func handleTypedContextResult(options handlerOptions, invocation bus.ModuleInvocation, args commandArgs, envelope bool) ([]byte, bus.ModuleStatus) {
	if options.placement != PlacementKB {
		return nil, bus.ModuleStatusCapabilityAbsent
	}
	query, ok := args.stringValue("query")
	if !ok || query == "" {
		return nil, bus.ModuleStatusInvalidRequest
	}
	cfg := typedOptions(args)
	if raw, present := args["evidence_requirements"]; present {
		var err error
		cfg.Requirements, err = decodeEvidenceRequirements(raw)
		if err != nil {
			return nil, bus.ModuleStatusInvalidRequest
		}
	}
	if raw, present := args["context_limits"]; present {
		if json.Unmarshal(raw, &cfg.ContextLimits) != nil || cfg.ContextLimits == nil {
			return nil, bus.ModuleStatusInvalidRequest
		}
	}
	if _, err := cfg.ContextLimits.byteLimit(maxDataBody); err != nil {
		var refusal *contextBudgetError
		if errors.As(err, &refusal) {
			encoded, status := commandResult(commandError(refusal.kind, refusal.message))
			if envelope {
				return runtimeJSONText(encoded, status)
			}
			return encoded, status
		}
		return nil, bus.ModuleStatusInvalidRequest
	}
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
	if !envelope {
		return commandResult(response.Payload)
	}
	// Native transports carry the complete owner JSON as text, preserving numeric
	// IDs even through cJSON. The public shape is that JSON object, not this envelope.
	return commandResult(map[string]string{"json": string(response.Payload)})
}
func newTypedContext(request DataRequest) *typedContextResult {
	cfg := request.TypedContext
	result := &typedContextResult{Requirements: cfg.Requirements, Status: "ok", Enabled: cfg.Enabled, Budget: cfg.Budgets["total"], Channels: map[string]*typedChannel{}, Trace: []typedPackTrace{}, MissingContext: request.Project == "" && request.Workspace == "", limits: cfg.ContextLimits}
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
	if r.Requirements != nil && name == "current_assertions" {
		r.coverageCandidates = append(r.coverageCandidates, item)
	}
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

const typedDataOpen = `<memory_data trust="untrusted" authorization="none">`
const typedDataClose = "</memory_data>\n"
const typedProceduresOpen = `<approved_procedures authority="reviewed" authorization="none">`
const typedProceduresClose = `</approved_procedures>`

type typedSerializedChannel struct {
	key    string
	rows   [][]byte
	prefix []int
}
type typedProjectionCache struct {
	names    []string
	channels map[string]typedSerializedChannel
}

// Encode each candidate once. Prefix lengths let tail removal recount complete
// JSON framing in constant work per channel instead of re-encoding all rows.
func cacheTypedProjection(r *typedContextResult) (typedProjectionCache, error) {
	cache := typedProjectionCache{names: append([]string(nil), typedChannelOrder...), channels: map[string]typedSerializedChannel{}}
	sort.Strings(cache.names) // encoding/json's existing map-key order
	for _, name := range cache.names {
		key, _ := json.Marshal(name)
		items := r.Channels[name].Items
		c := typedSerializedChannel{key: string(key), rows: make([][]byte, 0, len(items)), prefix: make([]int, 1, len(items)+1)}
		for _, item := range items {
			raw, err := json.Marshal(item)
			if err != nil {
				return typedProjectionCache{}, err
			}
			c.rows = append(c.rows, raw)
			c.prefix = append(c.prefix, c.prefix[len(c.prefix)-1]+len(raw))
		}
		cache.channels[name] = c
	}
	return cache, nil
}
func (cache typedProjectionCache) bytes(r *typedContextResult) int {
	size := len(typedDataOpen) + len(typedDataClose) + len(typedProceduresOpen) + len(typedProceduresClose) + 4 // {} and []
	fields := 0
	for _, name := range cache.names {
		n := len(r.Channels[name].Items)
		if n == 0 {
			continue
		}
		c := cache.channels[name]
		size += c.prefix[n] + n - 1 // complete serialized items and array commas
		if name != "approved_procedures" {
			size += len(c.key) + 3 // key, colon, brackets
			if fields > 0 {
				size++
			}
			fields++
		}
	}
	return size
}
func (cache typedProjectionCache) render(r *typedContextResult, size int) string {
	var out strings.Builder
	out.Grow(size)
	out.WriteString(typedDataOpen)
	out.WriteByte('{')
	writeRows := func(name string) {
		out.WriteByte('[')
		for i, raw := range cache.channels[name].rows[:len(r.Channels[name].Items)] {
			if i > 0 {
				out.WriteByte(',')
			}
			out.Write(raw)
		}
		out.WriteByte(']')
	}
	fields := 0
	for _, name := range cache.names {
		if name == "approved_procedures" || len(r.Channels[name].Items) == 0 {
			continue
		}
		if fields > 0 {
			out.WriteByte(',')
		}
		out.WriteString(cache.channels[name].key)
		out.WriteByte(':')
		writeRows(name)
		fields++
	}
	out.WriteByte('}')
	out.WriteString(typedDataClose)
	out.WriteString(typedProceduresOpen)
	writeRows("approved_procedures")
	out.WriteString(typedProceduresClose)
	return out.String()
}
func (r *typedContextResult) finish() error {
	byteLimit, err := r.limits.byteLimit(maxDataBody)
	if err != nil {
		return err
	}
	cache, err := cacheTypedProjection(r)
	if err != nil {
		return err
	}
	renderedBytes, omitEmpty := 0, false
	// The legacy per-channel estimates remain visible in used_tokens. Also bound
	// the complete rendered context, including JSON and both trust envelopes.
	// Drop whole rows in reverse packing order; never truncate structured evidence.
	for {
		renderedBytes = cache.bytes(r)
		r.RenderedTokens = renderedBytes/4 + 1
		if r.RenderedTokens <= r.Budget && renderedBytes <= byteLimit {
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
			reason := "complete rendered context exceeds total budget"
			if renderedBytes > byteLimit {
				reason = "complete rendered context exceeds exact byte limit"
			}
			r.trace(name, item.id, item.tokens, false, reason)
			removed = true
			break
		}
		if !removed {
			if renderedBytes > byteLimit {
				// No evidence remains. Empty trust wrappers are optional too;
				// a literal zero limit must not emit an oversized empty shell.
				omitEmpty = true
				r.RenderedTokens = 0
			} else {
				r.EnvelopeExcess = r.RenderedTokens - r.Budget
			}
			break
		}
	}
	if omitEmpty {
		r.Rendered = ""
	} else {
		r.Rendered = cache.render(r, renderedBytes)
		if len(r.Rendered) != renderedBytes {
			return fmt.Errorf("memory: typed projection byte accounting mismatch")
		}
	}
	count := 0
	r.Accounting, err = accountMemoryEnvelope(r.Rendered, byteLimit)
	if err != nil {
		return err
	}
	r.Accounting.Boundary = "typed_memory_projection"
	r.ProjectionVersion = 1
	r.RenderedBytes = r.Accounting.RenderedBytes
	r.ProjectionDigest = r.Accounting.Digest
	// The legacy bytes/4 estimate is not a defensible hard token bound. Keep
	// it for compatibility without claiming exact or conservative tokenization.
	r.TokenCountState = "unavailable"
	r.Retained = []typedProjectionRef{}
	for _, name := range typedChannelOrder {
		c := r.Channels[name]
		count += len(c.Items)
		for _, item := range c.selected {
			r.Retained = append(r.Retained, typedProjectionRef{Channel: name, ID: item.id, Source: item.source})
		}
	}
	r.SelectionDigest = typedSelectionDigest(r.ProjectionDigest, r.Retained)
	r.SourceVersionState = typedSourceVersionState(r.Retained)
	switch {
	case r.degraded:
		r.Availability = "degraded"
		r.Sufficiency = "unknown"
		r.Reason = "a requested channel degraded; task requirements have not been evaluated"
	case count == 0:
		r.Availability = "empty"
		r.Sufficiency = "insufficient"
		r.Reason = "no authorized evidence fit enabled channels"
	default:
		r.Availability = "available"
		r.Sufficiency = "unknown"
		r.Reason = "authorized evidence present; task requirements have not been evaluated"
	}
	r.evaluateCoverage()
	return nil
}

func typedSelectionDigest(projection string, retained []typedProjectionRef) string {
	value := struct {
		Version    int                  `json:"schema_version"`
		Projection string               `json:"projection_digest"`
		Retained   []typedProjectionRef `json:"retained_items"`
	}{1, projection, retained}
	raw, _ := json.Marshal(value)
	return fmt.Sprintf("sha256:%x", sha256.Sum256(raw))
}

func (r *typedContextResult) fitProjectionBytes(limit int) error {
	r.limits = &ContextLimits{SchemaVersion: 1, MaxContextBytes: &limit}
	if err := r.finish(); err != nil {
		return err
	}
	if len(r.Retained) == 0 {
		// Empty wrappers do not constitute a retained evidence channel.
		r.Rendered, r.RenderedBytes, r.RenderedTokens = "", 0, 0
		r.Accounting, _ = accountMemoryEnvelope("", limit)
		r.Accounting.Boundary = "typed_memory_projection"
		r.ProjectionDigest = r.Accounting.Digest
		r.SelectionDigest = typedSelectionDigest(r.ProjectionDigest, r.Retained)
		r.evaluateCoverage()
	}
	return nil
}

// decodeTypedProjection imports an owner response across the C host without
// decoding item numbers through cJSON doubles. These commitments prove assembly
// consistency, not current authorization or provider dispatch.
func decodeTypedProjection(raw string) (*typedContextResult, error) {
	var input struct {
		Requirements    *evidenceRequirementSet `json:"evidence_requirements"`
		Coverage        *evidenceCoverage       `json:"evidence_coverage"`
		SelectionDigest string                  `json:"selection_digest"`
		Availability    string                  `json:"retrieval_availability"`
		Status          string                  `json:"status"`
		Version         int                     `json:"projection_schema_version"`
		Digest          string                  `json:"projection_digest"`
		Bytes           int                     `json:"rendered_bytes"`
		Rendered        string                  `json:"rendered_context"`
		Budget          int                     `json:"total_budget_tokens"`
		Accounting      ContextAccounting       `json:"context_accounting"`
		Retained        []typedProjectionRef    `json:"retained_items"`
		Channels        map[string]struct {
			Items []json.RawMessage `json:"items"`
		} `json:"channels"`
	}
	invalid := func() (*typedContextResult, error) {
		return nil, &contextBudgetError{"invalid_projection", "typed projection identity or serialized evidence mismatch"}
	}
	if len(raw) > maxDataBody || json.Unmarshal([]byte(raw), &input) != nil || input.Status != "ok" || input.Version != 1 || input.Budget < 0 || input.Budget > 4096 {
		return invalid()
	}
	if input.Accounting.MaxContextBytes < 0 || input.Accounting.MaxContextBytes > maxDataBody {
		return invalid()
	}
	accounting, err := accountMemoryEnvelope(input.Rendered, input.Accounting.MaxContextBytes)
	if err != nil {
		return invalid()
	}
	accounting.Boundary = "typed_memory_projection"
	if input.Accounting != accounting || input.Digest != accounting.Digest || input.Bytes != len(input.Rendered) {
		return invalid()
	}
	cfg := typedOptions(commandArgs{})
	cfg.Requirements = input.Requirements
	if cfg.Requirements != nil && !cfg.Requirements.valid() {
		return invalid()
	}
	cfg.Budgets["total"] = input.Budget
	for _, name := range typedChannelOrder {
		cfg.Flags[name] = true
	}
	r := newTypedContext(DataRequest{TypedContext: cfg})
	r.coveragePrior = input.Coverage
	if cfg.Requirements != nil && (input.Coverage == nil || input.Coverage.SelectionDigest != input.SelectionDigest) {
		return invalid()
	}
	for name := range input.Channels {
		if r.Channels[name] == nil {
			return invalid()
		}
	}
	seen := map[[2]string]bool{}
	n := 0
	for _, name := range typedChannelOrder {
		for _, value := range input.Channels[name].Items {
			if n >= len(input.Retained) {
				return invalid()
			}
			ref := input.Retained[n]
			key := [2]string{ref.Channel, ref.ID}
			if ref.Channel != name || ref.ID == "" || seen[key] || !validTypedSourceItem(ref, value) {
				return invalid()
			}
			seen[key] = true
			r.Channels[name].Items = append(r.Channels[name].Items, value)
			item := typedItem{value: value, id: ref.ID, source: ref.Source}
			r.Channels[name].selected = append(r.Channels[name].selected, item)
			if name == "current_assertions" && r.Requirements != nil {
				r.coverageCandidates = append(r.coverageCandidates, item)
			}
			n++
		}
	}
	if n != len(input.Retained) || input.SelectionDigest != typedSelectionDigest(input.Digest, input.Retained) {
		return invalid()
	}
	cache, err := cacheTypedProjection(r)
	if err != nil {
		return invalid()
	}
	if input.Rendered != "" || n != 0 {
		if cache.render(r, cache.bytes(r)) != input.Rendered {
			return invalid()
		}
	}
	r.Rendered, r.ProjectionDigest, r.Retained = input.Rendered, input.Digest, input.Retained
	r.degraded = input.Availability == "degraded"
	r.SelectionDigest = input.SelectionDigest
	r.SourceVersionState = typedSourceVersionState(r.Retained)
	return r, nil
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
		items = append(items, typedItem{value: map[string]any{"proposal_id": id, "target_key": key, "state": "committed", "procedure": json.RawMessage(action)}, id: strconv.FormatInt(id, 10), text: action})
	}
	return items, rows.Err()
}
func (s *postgresDataStore) typedWatermarks(ctx context.Context, request DataRequest, exact Scope) (typedWatermark, error) {
	result := typedWatermark{Latest: request.TypedContext.Latest, Status: "unknown"}
	// A hidden parent denies assertion timestamps just as it denies assertion text.
	err := s.db.QueryRow(ctx, `SELECT COALESCE(max(ts),'') FROM (
 SELECT asserted_at AS ts FROM entity_edges e WHERE edge_class='semantic' AND `+currentMemoryEvidenceSQL("e", `$1='' OR (m.scope_type=$1 AND m.scope_value=$2)`, true)+`
 UNION ALL SELECT me.created_at FROM memory_episodes me JOIN memories m ON m.id=me.memory_id WHERE `+currentMemorySQL("m.")+` AND ($1='' OR (m.scope_type=$1 AND m.scope_value=$2))) q`, exact.Type, exact.Value).Scan(&result.Durable)
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
				source := h.sourceVersion()
				if source != nil {
					source.ReadPolicy = &sourceReadPolicy{ValidAt: request.Assertions.ValidAt, BelievedAt: request.Assertions.BelievedAt, Historical: request.Assertions.Historical}
				}
				r.add(name, typedItem{value: h, id: h.StableID, text: h.Rendered, source: source})
			}
		}
	}
	if !invalid && cfg.Flags["episodes"] {
		var episodes []versionedEpisode
		err := s.typedRead(ctx, func() error {
			var err error
			episodes, err = s.typedEpisodes(ctx, request.Query, 16, exact)
			return err
		})
		if err != nil {
			r.fail("episodes", "episode retrieval unavailable")
		} else {
			for _, e := range episodes {
				id := strconv.FormatInt(e.ID, 10)
				r.add("episodes", typedItem{value: map[string]any{"stable_id": id, "episode_key": e.Key, "excerpt": e.Text, "source_session": e.SourceSession, "reference_time": e.ReferenceTime, "trust": "untrusted_data"}, id: id, text: e.Text, source: e.source})
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
