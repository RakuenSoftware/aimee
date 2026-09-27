package memory

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"sort"
	"strconv"
	"strings"
	"sync"

	"github.com/JBailes/aimee/server-go/bus"
)

const servedViewPolicy = "served-memory-v1"

var servedViewNames = []string{"briefing", "active_constraints", "current_state", "recent_decisions", "relevant_context", "known_failures", "reviewed_procedures", "open_contradictions", "historical_context"}

type servedViewRequest struct {
	View         string                  `json:"view"`
	Task         string                  `json:"task"`
	Limit        int                     `json:"limit"`
	Limits       *ContextLimits          `json:"context_limits,omitempty"`
	Requirements *evidenceRequirementSet `json:"evidence_requirements,omitempty"`
}

type servedItem struct {
	Text       string               `json:"text"`
	Kind       string               `json:"kind"`
	ID         string               `json:"id"`
	Content    any                  `json:"content"`
	Sources    []typedProjectionRef `json:"sources"`
	Priority   int                  `json:"priority"`
	Historical bool                 `json:"historical"`
	coverage   []typedItem
}

type servedOmission struct {
	Kind   string `json:"kind"`
	Reason string `json:"reason"`
}

type servedViewResult struct {
	ChannelOutcomes map[string]map[string]int `json:"channel_outcomes"`
	Dependencies    []typedProjectionRef      `json:"dependencies"`
	Status          string                    `json:"status"`
	SchemaVersion   int                       `json:"schema_version"`
	PolicyVersion   string                    `json:"policy_version"`
	View            string                    `json:"view"`
	Store           string                    `json:"store"`
	Historical      bool                      `json:"historical"`
	ValidAt         string                    `json:"valid_at,omitempty"`
	BelievedAt      string                    `json:"believed_at,omitempty"`
	Selected        []servedItem              `json:"selected_records"`
	Omissions       []servedOmission          `json:"omissions"`
	Coverage        *evidenceCoverage         `json:"coverage,omitempty"`
	Sufficiency     string                    `json:"context_sufficiency"`
	Freshness       *typedSourceVersion       `json:"collection"`
	Rendered        string                    `json:"rendered_context"`
	Accounting      ContextAccounting         `json:"context_accounting"`
	Receipt         map[string]any            `json:"receipt"`
	Cache           map[string]any            `json:"cache"`
}

func validServedView(v string) bool {
	for _, name := range servedViewNames {
		if name == v {
			return true
		}
	}
	return false
}
func servedDigest(value any) string {
	raw, _ := json.Marshal(value)
	return fmt.Sprintf("sha256:%x", sha256.Sum256(raw))
}

func handleServedViewCommand(options handlerOptions, invocation bus.ModuleInvocation, verb string, args commandArgs) ([]byte, bus.ModuleStatus) {
	if invocation.PrincipalRef != 0 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	invalid := func(message string) ([]byte, bus.ModuleStatus) {
		return commandResult(commandError("invalid_argument", message))
	}
	var ok bool
	args, ok = commandDomainArgs(args, "memory."+verb)
	if !ok {
		return invalid("invalid memory view envelope")
	}
	request := DataRequest{Operation: "served-view", Project: args.stringOr("project", ""), Workspace: args.stringOr("workspace", ""), IncludeAll: args.boolean("include_all")}
	if options.placement == PlacementServer {
		request.Scope = Scope{Type: ScopeUser}
		if args.stringOr("store", "user") != "user" {
			return invalid("private views require store=user")
		}
	} else if args.stringOr("store", "kb") != "kb" {
		return invalid("shared views require store=kb")
	}
	if raw, present := args["scope"]; present {
		if options.placement == PlacementServer || json.Unmarshal(raw, &request.Scope) != nil || request.Scope.Type == "" {
			return invalid("scope requires a shared scope object")
		}
	}
	if verb == "claim_card" {
		request.Operation = "claim-card"
		if raw, present := args["expand_evidence"]; present {
			if string(raw) == "null" || json.Unmarshal(raw, &request.Detail) != nil {
				return invalid("expand_evidence must be boolean")
			}
		}
		request.ID, ok = args.decimalID("id")
		if !ok {
			return invalid("claim_card requires a positive memory id")
		}
	} else {
		v := &servedViewRequest{View: args.stringOr("view", ""), Task: strings.TrimSpace(args.stringOr("task", "")), Limit: 32}
		if !validServedView(v.View) || v.Task == "" || len(v.Task) > 8192 {
			return invalid("serve requires a named view and task of at most 8192 bytes")
		}
		if raw, present := args["limit"]; present {
			if json.Unmarshal(raw, &v.Limit) != nil || v.Limit < 0 || v.Limit > 64 {
				return invalid("limit must be an integer from 0 to 64")
			}
		}
		if raw, present := args["context_limits"]; present {
			if json.Unmarshal(raw, &v.Limits) != nil || v.Limits == nil {
				return invalid("invalid context_limits")
			}
		}
		if _, err := v.Limits.byteLimit(16384); err != nil {
			return invalid(err.Error())
		}
		var err error
		if raw, present := args["evidence_requirements"]; present {
			v.Requirements, err = decodeEvidenceRequirements(raw)
			if err != nil {
				return invalid(err.Error())
			}
		}
		a := &assertionSearchRequest{}
		for name, target := range map[string]*string{"valid_at": &a.ValidAt, "believed_at": &a.BelievedAt} {
			if raw, present := args[name]; present {
				if string(raw) == "null" || json.Unmarshal(raw, target) != nil || !assertionTimestamp(*target) {
					return invalid("temporal coordinates require second-precision UTC timestamps")
				}
			}
		}
		a.Historical = v.View == "historical_context"
		if a.Historical && a.ValidAt == "" && a.BelievedAt == "" {
			return invalid("historical_context requires explicit valid_at or believed_at")
		}
		if !a.Historical && (a.ValidAt != "" || a.BelievedAt != "") {
			return invalid("temporal coordinates require historical_context")
		}
		request.ServedView = v
		request.Assertions = a
		request.Query = v.Task
		request.Limit = 64
	}
	raw, _ := json.Marshal(request)
	body, status := handleData(options, invocation, raw)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	var response DataResponse
	if json.Unmarshal(body, &response) != nil || len(response.Payload) == 0 {
		return nil, bus.ModuleStatusInternal
	}
	return commandResult(response.Payload)
}

// This cache stores only serialized, already selected projections. Every hit
// follows a fresh owner transaction, including eligibility, temporal selection,
// collection observation and coverage. A local invalidation consumer never
// certifies freshness. Bounded process state is disposable, never canonical.
var servedProjectionCache = struct {
	sync.Mutex
	values map[string]string
	order  []string
}{values: map[string]string{}}

func cacheServedProjection(key, rendered string) (string, bool) {
	servedProjectionCache.Lock()
	defer servedProjectionCache.Unlock()
	if old, ok := servedProjectionCache.values[key]; ok && old == rendered {
		return old, true
	}
	if len(rendered) > 65536 {
		return rendered, false
	}
	if len(servedProjectionCache.order) >= 64 {
		delete(servedProjectionCache.values, servedProjectionCache.order[0])
		servedProjectionCache.order = servedProjectionCache.order[1:]
	}
	servedProjectionCache.order = append(servedProjectionCache.order, key)
	servedProjectionCache.values[key] = rendered
	return rendered, false
}

// Each item is an indivisible evidence bundle. Constraints precede assertions,
// contradictions retain both sides, and a dropped protected bundle stops lower
// priority content from presenting a misleadingly complete briefing.
func packServedItems(items []servedItem, limit, bytes int) ([]servedItem, []servedOmission, string, error) {
	sort.SliceStable(items, func(i, j int) bool { return items[i].Priority < items[j].Priority })
	selected := []servedItem{}
	omissions := []servedOmission{}
	render := func(rows []servedItem) string {
		if len(rows) == 0 {
			return ""
		}
		model := make([]map[string]any, 0, len(rows))
		for _, row := range rows {
			text := row.Text
			if text == "" {
				raw, _ := json.Marshal(row.Content)
				text = string(raw)
			}
			model = append(model, map[string]any{"kind": row.Kind, "id": row.ID, "text": text, "historical": row.Historical})
		}
		raw, _ := json.Marshal(model)
		return typedDataOpen + string(raw) + typedDataClose
	}
	protectedGap := false
	for _, item := range items {
		trial := append(append([]servedItem{}, selected...), item)
		if protectedGap || len(trial) > limit || len(render(trial)) > bytes {
			reason := "budget_exhausted"
			if protectedGap {
				reason = "required_bundle_missing"
			}
			omissions = append(omissions, servedOmission{item.Kind, reason})
			if item.Priority < 2 {
				protectedGap = true
			}
			continue
		}
		selected = trial
	}
	rendered := render(selected)
	if _, err := accountMemoryEnvelope(rendered, bytes); err != nil {
		return nil, nil, "", err
	}
	return selected, omissions, rendered, nil
}

func (s *postgresDataStore) serveView(ctx context.Context, request DataRequest, explicit bool) (servedViewResult, error) {
	v := request.ServedView
	if v == nil || !validServedView(v.View) || v.Limit < 0 || v.Limit > 64 || len(v.Task) > 8192 || strings.TrimSpace(v.Task) == "" || request.Assertions == nil || (v.Requirements != nil && !v.Requirements.valid()) {
		return servedViewResult{}, errors.New("memory: invalid served view")
	}
	a := request.Assertions
	if !assertionTimestamp(a.ValidAt) || !assertionTimestamp(a.BelievedAt) || a.Historical != (v.View == "historical_context") || (a.Historical && a.ValidAt == "" && a.BelievedAt == "") || (!a.Historical && (a.ValidAt != "" || a.BelievedAt != "")) {
		return servedViewResult{}, errors.New("memory: inconsistent served view time mode")
	}
	var replica bool
	if err := s.db.QueryRow(ctx, `SELECT pg_is_in_recovery()`).Scan(&replica); err != nil {
		return servedViewResult{}, err
	}
	if replica {
		return servedViewResult{}, errors.New("memory: current owner evidence unavailable on replica")
	}
	bytes, err := v.Limits.byteLimit(16384)
	if err != nil {
		return servedViewResult{}, err
	}
	result := servedViewResult{Status: "ok", SchemaVersion: 1, PolicyVersion: servedViewPolicy, View: v.View, Store: "kb", Historical: request.Assertions.Historical, ValidAt: request.Assertions.ValidAt, BelievedAt: request.Assertions.BelievedAt, Selected: []servedItem{}, Omissions: []servedOmission{}, Sufficiency: "unknown"}
	if s.placement == PlacementServer {
		result.Store = "user"
	}
	result.Freshness, err = s.observeRecallCollection(ctx)
	if err != nil {
		return result, err
	}
	result.Dependencies = []typedProjectionRef{{Channel: "native_memory_collection", ID: "1", Source: result.Freshness}}
	protected := []servedItem{}
	if s.placement == PlacementKB && (v.View == "briefing" || v.View == "active_constraints") {
		rules, collection, err := s.recallHardRules(ctx, 16384)
		if err != nil {
			return result, err
		}
		result.Dependencies = append(result.Dependencies, typedProjectionRef{Channel: "native_rule_collection", ID: "1", Source: collection})
		if len(rules) > 0 {
			sources := []typedProjectionRef{}
			for _, rule := range rules {
				sources = append(sources, typedProjectionRef{Channel: "native_rules", ID: strconv.FormatInt(rule.ID, 10), Source: rule.Source})
			}
			protected = append(protected, servedItem{Kind: "hard_constraints", ID: "rules", Content: rules, Text: servedRuleText(rules), Sources: sources, Priority: -1})
		}
	}
	items, omissions, err := s.servedCandidates(ctx, request, explicit)
	if err != nil {
		return result, err
	}
	items = append(protected, items...)
	admitted := []servedItem{}
	for _, item := range items {
		valid := len(item.Sources) > 0
		for _, ref := range item.Sources {
			valid = valid && validTypedSource(ref)
		}
		if valid {
			admitted = append(admitted, item)
		} else {
			omissions = append(omissions, servedOmission{item.Kind, "source_version_unavailable"})
		}
	}
	items = admitted
	result.Selected, result.Omissions, result.Rendered, err = packServedItems(items, v.Limit, bytes)
	if err != nil {
		return result, err
	}
	result.Omissions = append(result.Omissions, omissions...)
	// Request identity commits to effective scope and principal, explicit time,
	// budgets, requirement revision, owner collection, exact selected versions,
	// and the renderer/eligibility/horizon policy. Raw identities are never emitted.
	var principal, audience string
	if err = s.db.QueryRow(ctx, `SELECT COALESCE(current_setting('aimee.principal',true),''),COALESCE(current_setting('aimee.transport_identity',true),'')`).Scan(&principal, &audience); err != nil {
		return result, err
	}
	key := servedDigest([]any{servedViewPolicy, currentEligibilityPolicy, request.Scope, request.Project, request.Workspace, request.IncludeAll, principal, audience, v, request.Assertions, result.Dependencies, result.Selected, result.Omissions})
	var hit bool
	result.Rendered, hit = cacheServedProjection(key, result.Rendered)
	result.Cache = map[string]any{"state": map[bool]string{true: "projection_reused", false: "rebuilt"}[hit], "identity": key, "owner_revalidated": true, "temporal_selection": "recomputed"}
	result.Accounting, err = accountMemoryEnvelope(result.Rendered, bytes)
	if err != nil {
		return result, err
	}
	refs := append([]typedProjectionRef{}, result.Dependencies...)
	cfg := typedOptions(commandArgs{})
	cfg.Requirements = v.Requirements
	coverage := newTypedContext(DataRequest{TypedContext: cfg})
	coverage.SelectionDigest = servedDigest(result.Selected)
	for _, item := range items {
		coverage.coverageCandidates = append(coverage.coverageCandidates, item.coverage...)
	}
	for _, item := range result.Selected {
		refs = append(refs, item.Sources...)
		for _, candidate := range item.coverage {
			name := "current_assertions"
			if item.Historical {
				name = "historical_assertions"
			}
			coverage.Channels[name].selected = append(coverage.Channels[name].selected, candidate)
		}
	}
	coverage.evaluateCoverage()
	if coverage.Coverage == nil {
		coverage.Coverage = &evidenceCoverage{RequirementsVersion: 1, PlannerVersion: servedViewPolicy, TaskRevision: servedViewPolicy, RequirementDigest: servedDigest([]any{v.View, v.Task}), SelectionDigest: coverage.SelectionDigest, Boundary: "served_memory_projection", ReleaseState: "owner_snapshot_observed", Status: "unknown", Roles: []evidenceRoleCoverage{}, Reasons: []string{"task_obligations_not_supplied"}}
	}
	result.ChannelOutcomes = map[string]map[string]int{}
	for _, item := range items {
		if result.ChannelOutcomes[item.Kind] == nil {
			result.ChannelOutcomes[item.Kind] = map[string]int{"candidate_bundles": 0, "retained_bundles": 0}
		}
		result.ChannelOutcomes[item.Kind]["candidate_bundles"]++
	}
	for _, item := range result.Selected {
		result.ChannelOutcomes[item.Kind]["retained_bundles"]++
	}
	result.Coverage = coverage.Coverage
	if coverage.Sufficiency != "" {
		result.Sufficiency = coverage.Sufficiency
	}
	if len(result.Selected) == 0 || len(result.Omissions) > 0 {
		result.Status = "degraded"
		if len(result.Selected) == 0 {
			result.Omissions = append(result.Omissions, servedOmission{"view", "no_authorized_applicable_evidence"})
		}
	}
	nonce := make([]byte, 16)
	if _, err = rand.Read(nonce); err != nil {
		return result, err
	}
	result.Receipt = map[string]any{"schema_version": 1, "invocation_id": hex.EncodeToString(nonce), "stage": "projection_prepared", "durability": "invocation_response", "payload_sha256": result.Accounting.Digest, "payload_bytes": len(result.Rendered), "sources": refs, "sources_digest": servedDigest(refs), "selection_digest": coverage.SelectionDigest, "request_identity": key, "release_state": "owner_snapshot_observed", "provider_dispatch": "not_requested"}
	return result, nil
}

func servedRecordItem(r Record, priority int) servedItem {
	kind := "memory_record"
	if r.Scope.Type == ScopeUser {
		kind = "user_memory_record"
	}
	ref := typedProjectionRef{Channel: "native_active_context", ID: strconv.FormatInt(r.ID, 10), Source: &typedSourceVersion{Kind: kind, Version: *r.Version, MemoryParentState: "observed"}}
	return servedItem{Kind: r.Kind, ID: ref.ID, Content: r, Text: r.Content, Sources: []typedProjectionRef{ref}, Priority: priority, Historical: r.Historical}
}

func servedRuleText(rules []recallRule) string {
	var text strings.Builder
	for _, rule := range rules {
		fmt.Fprintf(&text, "[%s] %s: %s\n", rule.Polarity, rule.Title, rule.Description)
	}
	return text.String()
}
