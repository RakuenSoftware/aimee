package executionpolicy

import (
	"encoding/json"
	"errors"
	"fmt"
	"sort"
	"strings"
	"time"
)

// Clean retries are an explicit primary-runtime capability. Limits live in the
// existing operator policy; no request may replace them. Costs are conservative
// operator-priced reservations, not billing observations. Without an exact
// tokenizer a requested hard token ceiling is unavailable rather than guessed.
type RetryPolicy struct {
	Enabled             bool    `json:"enabled"`
	MaxAttempts         uint64  `json:"max_attempts"`
	MaxRepeatedFailures uint64  `json:"max_repeated_failures"`
	MaxWallSeconds      uint64  `json:"max_wall_seconds"`
	MaxProviderBytes    uint64  `json:"max_provider_bytes"`
	NanodollarsPerByte  uint64  `json:"nanodollars_per_byte"`
	MaxNanodollars      uint64  `json:"max_nanodollars"`
	MaxTokens           *uint64 `json:"max_tokens,omitempty"`
	RetainInputSeconds  uint64  `json:"retain_input_seconds"`
}

func CurrentRetryPolicy() (RetryPolicy, string, error) {
	p, e := defaultPolicyLoader()
	if e != nil {
		return RetryPolicy{}, "", e
	}
	if p == nil {
		return RetryPolicy{}, actionDigest(p), nil
	}
	r := p.CleanRetry
	if r.Enabled && (r.MaxAttempts < 1 || r.MaxAttempts > 32 || r.MaxRepeatedFailures < 1 || r.MaxRepeatedFailures > 32 || r.MaxWallSeconds < 1 || r.MaxWallSeconds > 86400 || r.MaxProviderBytes < 1 || r.MaxProviderBytes > 1<<40 || r.NanodollarsPerByte < 1 || r.NanodollarsPerByte > 1000000 || r.MaxNanodollars < 1 || r.MaxNanodollars > 1<<60 || r.RetainInputSeconds > 86400) {
		return r, "", errors.New("invalid retry policy")
	}
	return r, actionDigest(p), nil
}

type RetryAttempt struct {
	ID           string      `json:"attempt_id"`
	Request      string      `json:"request_id"`
	Parent       string      `json:"parent_attempt,omitempty"`
	Input        string      `json:"retained_input,omitempty"`
	InputDigest  string      `json:"input_sha256"`
	InputExpires time.Time   `json:"input_expires"`
	Policy       string      `json:"policy_generation"`
	Projection   string      `json:"projection_revision"`
	Renderer     string      `json:"renderer_version"`
	Plan         string      `json:"plan_revision"`
	Started      time.Time   `json:"started"`
	State        string      `json:"state"`
	Failure      string      `json:"failure_class,omitempty"`
	Calls        []RetryCall `json:"provider_calls"`
}
type RetryCall struct {
	ID           string `json:"id"`
	Bytes        uint64 `json:"reserved_bytes"`
	Cost         uint64 `json:"reserved_nanodollars"`
	Rate         uint64 `json:"nanodollars_per_byte"`
	Payload      string `json:"payload_sha256"`
	Receipt      string `json:"receipt_reference"`
	SourceHandle string `json:"source_reference"`
}
type RetryJournal struct {
	Schema    int            `json:"schema_version"`
	Principal string         `json:"principal"`
	Root      string         `json:"root_id"`
	Started   time.Time      `json:"started"`
	Attempts  []RetryAttempt `json:"attempts"`
	Bytes     uint64         `json:"reserved_bytes"`
	Cost      uint64         `json:"reserved_nanodollars"`
}
type RetryRequest struct {
	Operation          string    `json:"operation"`
	Request            string    `json:"request_id"`
	Attempt            string    `json:"attempt_id"`
	Previous           string    `json:"previous_attempt"`
	Input              string    `json:"input"`
	ReplaceConstraints bool      `json:"replace_constraints"`
	Projection         string    `json:"projection_revision"`
	Failure            string    `json:"failure_class"`
	Call               RetryCall `json:"call"`
}
type RetryDecision struct {
	Allowed               bool   `json:"allowed"`
	Reason                string `json:"reason"`
	Attempt               string `json:"attempt_id,omitempty"`
	Plan                  string `json:"plan_revision,omitempty"`
	Summary               string `json:"summary,omitempty"`
	RemainingAttempts     uint64 `json:"remaining_attempts"`
	RemainingBytes        uint64 `json:"remaining_bytes"`
	RemainingNanodollars  uint64 `json:"remaining_nanodollars"`
	WorkspaceRestored     bool   `json:"workspace_restored"`
	FreshAssemblyRequired bool   `json:"fresh_assembly_required"`
}

func retryLoad(principal, root string, raw []byte) (RetryJournal, error) {
	j := RetryJournal{Schema: 1, Principal: principal, Root: root}
	if len(raw) == 0 {
		return j, nil
	}
	if len(raw) > 4<<20 || json.Unmarshal(raw, &j) != nil || j.Schema != 1 || j.Principal != principal || j.Root != root || len(j.Attempts) > 32 {
		return j, errors.New("retry journal unavailable")
	}
	var bytes, cost uint64
	ids := map[string]bool{}
	for _, a := range j.Attempts {
		if a.ID == "" || ids[a.ID] || a.InputDigest == "" || a.Started.IsZero() || (a.State != "running" && a.State != "failed" && a.State != "completed") || (a.Input != "" && actionDigest(a.Input) != a.InputDigest) || len(a.Calls) > 1024 {
			return j, errors.New("retry journal corrupt")
		}
		ids[a.ID] = true
		if a.Parent != "" && !ids[a.Parent] {
			return j, errors.New("retry parent unavailable")
		}
		calls := map[string]bool{}
		for _, c := range a.Calls {
			if c.ID == "" || calls[c.ID] || !actionHash(c.Payload) || c.Bytes == 0 || c.Rate == 0 || c.Bytes > ^uint64(0)/c.Rate || c.Cost != c.Bytes*c.Rate || c.Bytes > ^uint64(0)-bytes || c.Cost > ^uint64(0)-cost {
				return j, errors.New("retry reservations corrupt")
			}
			calls[c.ID] = true
			bytes += c.Bytes
			cost += c.Cost
		}
	}
	if bytes != j.Bytes || cost != j.Cost {
		return j, errors.New("retry counters corrupt")
	}
	return j, nil
}

// ActionRetrySummary reads only the verified durable action journal, never
// model-authored success claims. Mutation acknowledgements need reconciliation.
func ActionRetrySummary(principal, root string, raw []byte) (string, bool, error) {
	if len(raw) == 0 {
		return "No governed actions recorded. This is not a workspace restore.", false, nil
	}
	var j ActionJournal
	if len(raw) > 4<<20 || json.Unmarshal(raw, &j) != nil || j.Principal != principal || j.Root != root || j.validate() != nil {
		return "", false, errors.New("action journal unavailable")
	}
	rows := make([]ActionReceipt, 0, len(j.Actions))
	for _, r := range j.Actions {
		rows = append(rows, r)
	}
	sort.Slice(rows, func(i, k int) bool { return rows[i].Sequence < rows[k].Sequence })
	var b strings.Builder
	unknown := false
	b.WriteString("Host action journal; filesystem effects were not rolled back. Inspect receipts before repeating work.\n")
	for _, r := range rows {
		mutation := r.Intent.Class != "read_only" && r.Intent.Class != "sensitive_read"
		if mutation && (r.State == "dispatching" || r.State == "outcome_unknown" || r.State == "acknowledged") {
			unknown = true
		}
		if r.State == "prepared" || r.State == "admitted" {
			continue
		}
		line := fmt.Sprintf("Action %s: %s; object %s; payload %s", r.Intent.ID, CompletionClaim(r), r.Intent.Destination, r.Intent.PayloadDigest)
		if r.Outcome != nil && r.Outcome.ObjectVersion != "" {
			line += "; observed version " + r.Outcome.ObjectVersion
		}
		if b.Len()+len(line)+1 > 8192 {
			return "", unknown, errors.New("action summary exceeds retry bound")
		}
		b.WriteString(line + "\n")
	}
	return b.String(), unknown, nil
}

// CleanRetry is a pure reducer. The DB1 owner authenticates and serializes the
// root with the action journal, and commits reservations before any dispatch.
func CleanRetry(principal, root string, raw, actions []byte, r RetryRequest, p RetryPolicy, generation string, now time.Time) ([]byte, RetryDecision, error) {
	j, e := retryLoad(principal, root, raw)
	if e != nil {
		return raw, RetryDecision{}, e
	}
	d := RetryDecision{Reason: "retry_disabled", FreshAssemblyRequired: true}
	refuse := func(reason string) ([]byte, RetryDecision, error) { d.Reason = reason; return raw, d, nil }
	if !p.Enabled && (r.Operation == "begin" || r.Operation == "send") {
		return refuse("retry_disabled")
	}
	if (r.Operation == "begin" || r.Operation == "send") && (p.NanodollarsPerByte == 0 || p.MaxAttempts == 0 || p.MaxAttempts > 32) {
		return refuse("retry_policy_invalid")
	}
	if !j.Started.IsZero() && now.Before(j.Started) {
		return refuse("retry_clock_regressed")
	}
	if p.MaxTokens != nil && (r.Operation == "begin" || r.Operation == "send") {
		return refuse("exact_token_counter_unavailable")
	}
	remaining := func() {
		if uint64(len(j.Attempts)) < p.MaxAttempts {
			d.RemainingAttempts = p.MaxAttempts - uint64(len(j.Attempts))
		}
		if j.Bytes < p.MaxProviderBytes {
			d.RemainingBytes = p.MaxProviderBytes - j.Bytes
		}
		if j.Cost < p.MaxNanodollars {
			d.RemainingNanodollars = p.MaxNanodollars - j.Cost
		}
	}
	remaining()
	var a *RetryAttempt
	for n := range j.Attempts {
		if j.Attempts[n].ID == r.Attempt {
			a = &j.Attempts[n]
			break
		}
	}
	switch r.Operation {
	case "begin":
		if r.Request == "" || len(r.Request) > 128 || len(r.Input) == 0 || len(r.Input) > 32768 || len(r.Projection) > 128 {
			return refuse("invalid_retry_input")
		}
		id := actionDigest([]string{principal, root, r.Request})
		for _, old := range j.Attempts {
			if old.ID == id {
				return refuse("attempt_already_admitted")
			}
		}
		if uint64(len(j.Attempts)) >= p.MaxAttempts {
			return refuse("task_attempt_ceiling")
		}
		if !j.Started.IsZero() && !now.Before(j.Started.Add(time.Duration(p.MaxWallSeconds)*time.Second)) {
			return refuse("task_wall_ceiling")
		}
		summary := ""
		if r.Previous != "" {
			var prior *RetryAttempt
			for n := range j.Attempts {
				if j.Attempts[n].ID == r.Previous {
					prior = &j.Attempts[n]
					break
				}
			}
			if prior == nil {
				return refuse("baseline_unavailable_fresh_plan_required")
			}
			if prior.State != "failed" {
				return refuse("attempt_not_failed")
			}
			if prior.Input == "" || !prior.InputExpires.After(now) || actionDigest(prior.Input) != prior.InputDigest {
				return refuse("baseline_unavailable_fresh_plan_required")
			}
			if !r.ReplaceConstraints {
				return refuse("current_complete_constraints_required")
			}
			var unknown bool
			summary, unknown, e = ActionRetrySummary(principal, root, actions)
			if e != nil {
				return refuse("action_reconstruction_gap")
			}
			if unknown {
				return refuse("unknown_effect_reconciliation_required")
			}
			repeated := uint64(0)
			for _, old := range j.Attempts {
				if old.State == "failed" && old.Failure == prior.Failure {
					repeated++
				}
			}
			if repeated >= p.MaxRepeatedFailures {
				return refuse("repeated_failure_ceiling")
			}
			summary = "Bounded host failure summary (non-authoritative lessons): prior attempt " + prior.ID + " failed: " + prior.Failure + ". Failed speculative reasoning is omitted. Current supplied instructions replace prior user constraints.\n" + summary
		}
		if j.Started.IsZero() {
			j.Started = now
		}
		next := RetryAttempt{ID: id, Request: r.Request, Parent: r.Previous, InputDigest: actionDigest(r.Input), Policy: generation, Projection: r.Projection, Renderer: "clean-retry-v1", Plan: id + ":plan:1", Started: now, State: "running"}
		if p.RetainInputSeconds > 0 {
			next.Input = r.Input
			next.InputExpires = now.Add(time.Duration(p.RetainInputSeconds) * time.Second)
		}
		j.Attempts = append(j.Attempts, next)
		d.Attempt = id
		d.Plan = next.Plan
		d.Summary = summary
	case "send":
		if a == nil || a.State != "running" {
			return refuse("attempt_not_running")
		}
		if !now.Before(j.Started.Add(time.Duration(p.MaxWallSeconds) * time.Second)) {
			return refuse("task_wall_ceiling")
		}
		c := r.Call
		if c.ID == "" || len(c.ID) > 128 || c.Bytes == 0 || !actionHash(c.Payload) || len(c.Receipt) > 128 || len(c.SourceHandle) > 128 {
			return refuse("invalid_provider_reservation")
		}
		for _, old := range a.Calls {
			if old.ID == c.ID {
				return refuse("provider_attempt_already_reserved")
			}
		}
		if len(a.Calls) >= 1024 || j.Bytes > p.MaxProviderBytes || c.Bytes > p.MaxProviderBytes-j.Bytes {
			return refuse("task_provider_byte_ceiling")
		}
		if c.Bytes > ^uint64(0)/p.NanodollarsPerByte {
			return refuse("task_cost_ceiling")
		}
		c.Rate = p.NanodollarsPerByte
		c.Cost = c.Bytes * c.Rate
		if j.Cost > p.MaxNanodollars || c.Cost > p.MaxNanodollars-j.Cost {
			return refuse("task_cost_ceiling")
		}
		a.Calls = append(a.Calls, c)
		j.Bytes += c.Bytes
		j.Cost += c.Cost
		d.Attempt = a.ID
	case "finish":
		if a == nil {
			return refuse("attempt_unavailable")
		}
		if r.Failure != "completed" && r.Failure != "provider_failure" && r.Failure != "context_refused" && r.Failure != "execution_failure" && r.Failure != "interrupted" {
			return refuse("invalid_host_failure_class")
		}
		state := "failed"
		if r.Failure == "completed" {
			state = "completed"
		}
		if a.State != "running" && (a.State != state || a.Failure != r.Failure) {
			return refuse("attempt_outcome_conflict")
		}
		a.State = state
		a.Failure = r.Failure
		d.Attempt = a.ID
	case "inspect":
		if a == nil {
			return refuse("attempt_unavailable")
		}
		d.Attempt = a.ID
		d.Plan = a.Plan
		d.Reason = a.State
	default:
		return refuse("invalid_retry_operation")
	}
	// Expired replay inputs are disposable. Digests, failures, reservations and
	// action receipts are retained, and cannot be refreshed by a later attempt.
	for n := range j.Attempts {
		if !j.Attempts[n].InputExpires.After(now) {
			j.Attempts[n].Input = ""
		}
	}
	remaining()
	d.Allowed = true
	if d.Reason == "retry_disabled" {
		d.Reason = "admitted"
	}
	next, e := json.Marshal(j)
	if len(next) > 4<<20 {
		return refuse("retry_journal_full")
	}
	return next, d, e
}
