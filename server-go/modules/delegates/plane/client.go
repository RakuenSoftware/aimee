// Package plane is the client side of the delegate resource plane: launching a
// delegate, polling it to a terminal state, cancelling it, and releasing its
// replay reservation.
//
// It lives outside internal/ because the roundtable runs as a module process
// and has to reach the same plane, and there must not be two implementations of
// this to keep in step. It holds no database handle -- every durable decision
// (reserving a replay, reclaiming an unassigned job) is asked of the side that
// owns agent_jobs, which is what makes it safe to run in a module.
package plane

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"time"
)

type DelegateRequest struct {
	Role     string
	Persona  string
	Delegate string
	// Participant is an opaque handle returned by DelegateGroup. Follow-up
	// discussion uses it for continuity without learning agent/provider details.
	Participant      string
	Prompt           string
	Workdir          string
	Tools            bool
	WorkItemID       string
	Stage            string
	ExecutionVersion string
	RetryTag         string
	// MaxCostUSD is a positive per-call hard safety bound assigned by the
	// workflow runner. The resource plane rejects or stops work that cannot fit.
	MaxCostUSD float64
	// MaxTurnsCap bounds a delegate without overriding a smaller role/agent cap.
	// It is forwarded to the resource plane as max_turns_cap.
	MaxTurnsCap int
	// ToolLoopTimeoutMSCap bounds the delegate's whole multi-turn tool loop
	// without increasing a smaller configured agent budget. Workflow runners set
	// it from the enclosing stage deadline so the resource plane can return a
	// partial result before a hard context cancellation.
	ToolLoopTimeoutMSCap int
	// ReplayOnly forbids launching a replacement job when a lifecycle retry is
	// consuming an already-billed durable result.
	ReplayOnly bool
	// DurableSlot distinguishes parallel logical consumers of an otherwise
	// identical request. It participates in the local job key only and is never
	// sent to the resource plane as routing configuration.
	DurableSlot string
	// ArtifactStage is the validated workflow artifact stage a review delegate
	// must evaluate. It is carried structurally so runner collaborators and test
	// doubles never need to recover authority from prompt prose.
	ArtifactStage string
	// ArtifactHash is the authoritative inline artifact identity for review
	// phases. It remains Go-local; the prompt carries the value to the delegate.
	ArtifactHash string
	// ProvidedTarget tells the resource-plane transport that Prompt already
	// contains the complete artifact under review. It suppresses unrelated
	// worktree-diff evidence. Only true emits the strict JSON boolean opt-in;
	// false omits it and preserves automatic evidence. This is a deliberate wire
	// field; DurableSlot and RetryTag are Go-local and must never be sent.
	ProvidedTarget bool
	// routeSelected distinguishes a delegate-service group assignment from an
	// operator pin. A selected seat may be generically rerouted after a terminal
	// provider failure; an operator pin never may.
	routeSelected bool
	// AcceptPartial is reserved for native branch-producing blocks whose worktree
	// output is independently committed and verified by the caller. Structured and
	// prose blocks must receive a complete resource-plane result.
	//
	// It is the caller's policy, not the plane's: whether a partial result is
	// usable depends on what the calling block required of it.
	AcceptPartial bool
}

type DelegateResult struct {
	Response string
	Agent    string
	// Participant is an opaque delegate-service continuation token. Consumers
	// may return it to the delegate service but must not interpret it.
	Participant string
	CostUSD     float64
	// CostUnknown means the resource plane recorded no measurement for this
	// call. CostUSD is then not a measured zero and must not be committed as
	// actual spend.
	CostUnknown bool
	Partial     bool
}

type AgentClient interface {
	Delegate(context.Context, DelegateRequest) (DelegateResult, error)
}

// DelegateGroup is the generic resource-plane contract for X independent
// delegates with Y per-seat specifications. Callers describe roles, personas,
// evidence and optional positive pins; delegate owns all unpinned routing.
type DelegateGroupResult struct {
	Participant string
	Response    string
	CostUSD     float64
	// CostUnknown mirrors DelegateResult.CostUnknown: no measurement was
	// recorded, so CostUSD is a lower bound rather than actual spend.
	CostUnknown bool
	Err         error
}

type DelegateGroupClient interface {
	DelegateGroup(context.Context, []DelegateRequest) []DelegateGroupResult
}

type AgentHTTPConfig struct {
	BaseURL              string
	UnixSocket           string
	Bearer               string
	PollEvery            time.Duration
	RequestTimeout       time.Duration
	PendingTimeout       time.Duration
	PendingTimeoutSource func() time.Duration
}

const (
	MinDelegatePendingTimeout     = 2 * time.Second
	DefaultDelegatePendingTimeout = 2 * time.Minute
	MaxDelegatePendingTimeout     = time.Hour
	delegateCancelTimeout         = 10 * time.Second
	delegateForgetTimeout         = 3 * time.Second
)

var ErrDelegateUnassignedExpired = errors.New("unassigned delegate lease expired")
var ErrDelegateCancelUnacknowledged = errors.New("delegate cancellation was not acknowledged")
var ErrDelegateNoJobID = errors.New("agent service returned no job id")
var ErrDelegateTerminal = errors.New("delegate job reached a failed terminal state")

// ErrDelegateReplayUnavailable is returned when a replay-only invocation cannot
// find the durable delegate result it is required to reuse (e.g. the in-flight
// job was killed by a restart and its mapping was lost). Replay can never
// succeed, so the engine must recover — re-dispatch a recoverable interruption
// or park a lost reconciled result for a human — instead of retrying forever.
var ErrDelegateReplayUnavailable = errors.New("replay-only delegate result is unavailable")

// DelegateExecutionError carries the billing boundary across transport and
// runner layers. A caller must not infer provider dispatch merely because the
// delegate API was called: validation and admission failures are pre-dispatch,
// while a known job can have either measured or unresolved spend.
type DelegateExecutionError struct {
	Err        error
	Dispatched bool
	CostKnown  bool
	CostUSD    float64
}

func (e *DelegateExecutionError) Error() string { return e.Err.Error() }
func (e *DelegateExecutionError) Unwrap() error { return e.Err }

func delegateExecutionError(err error, dispatched, costKnown bool, costUSD float64) error {
	if err == nil || !dispatched {
		return err
	}
	return &DelegateExecutionError{Err: err, Dispatched: true, CostKnown: costKnown, CostUSD: costUSD}
}

type agentHTTPStatusError struct {
	status int
	detail string
}

func (e *agentHTTPStatusError) Error() string {
	return fmt.Sprintf("agent service %d: %s", e.status, e.detail)
}

// HTTPAgentClient talks to the agent service as a resource plane. It owns no
// workflow state or transitions; losing it parks the Go-owned run and a later
// scheduler pass can retry safely.
type HTTPAgentClient struct {
	baseURL, bearer string
	client          *http.Client
	pollEvery       time.Duration
	pendingTimeout  func() time.Duration
}

func NewHTTPAgentClient(cfg AgentHTTPConfig) (*HTTPAgentClient, error) {
	if cfg.BaseURL == "" {
		cfg.BaseURL = "http://aimee"
	}
	parsedBase, err := url.Parse(cfg.BaseURL)
	if err != nil {
		return nil, fmt.Errorf("parse agent service URL: %w", err)
	}
	host := parsedBase.Hostname()
	loopback := host == "localhost"
	if ip := net.ParseIP(host); ip != nil && ip.IsLoopback() {
		loopback = true
	}
	if cfg.UnixSocket == "" && cfg.Bearer == "" && !loopback {
		return nil, errors.New("remote agent service requires an authenticated bearer or Unix socket")
	}
	transport := http.DefaultTransport.(*http.Transport).Clone()
	if cfg.UnixSocket != "" {
		socket := cfg.UnixSocket
		transport.DialContext = func(ctx context.Context, _, _ string) (net.Conn, error) {
			return (&net.Dialer{}).DialContext(ctx, "unix", socket)
		}
	}
	if cfg.PollEvery <= 0 {
		cfg.PollEvery = time.Second
	}
	if cfg.RequestTimeout <= 0 {
		cfg.RequestTimeout = 30 * time.Second
	}
	if cfg.PendingTimeout <= 0 {
		cfg.PendingTimeout = DefaultDelegatePendingTimeout
	}
	timeoutSource := func() time.Duration { return cfg.PendingTimeout }
	if cfg.PendingTimeoutSource != nil {
		timeoutSource = cfg.PendingTimeoutSource
	}
	return &HTTPAgentClient{baseURL: strings.TrimRight(cfg.BaseURL, "/"), bearer: cfg.Bearer,
		client: &http.Client{Transport: transport, Timeout: cfg.RequestTimeout}, pollEvery: cfg.PollEvery,
		pendingTimeout: timeoutSource}, nil
}

func (c *HTTPAgentClient) delegatePendingTimeout() time.Duration {
	timeout := c.pendingTimeout()
	if timeout < MinDelegatePendingTimeout {
		timeout = MinDelegatePendingTimeout
	}
	if timeout > MaxDelegatePendingTimeout {
		timeout = MaxDelegatePendingTimeout
	}
	return timeout
}

func (c *HTTPAgentClient) Delegate(ctx context.Context, request DelegateRequest) (DelegateResult, error) {
	if request.Delegate == "$random" {
		request.Delegate = ""
	}
	const maxRouteAttempts = 3
	totalCost := 0.0
	costUnknown := false
	originalCostLimit := request.MaxCostUSD
	for attempt := 0; ; attempt++ {
		result, err := c.delegateOnce(ctx, request)
		var execution *DelegateExecutionError
		if errors.As(err, &execution) {
			// CostUSD is the measured prefix even when the current attempt has
			// unresolved spend. Preserve both facts independently.
			totalCost += execution.CostUSD
			if !execution.CostKnown {
				costUnknown = true
			}
		}
		if err == nil {
			result.CostUSD += totalCost
			// An earlier attempt with unmeasured spend makes the accumulated
			// total a lower bound, not a measurement.
			result.CostUnknown = result.CostUnknown || costUnknown
			return result, nil
		}
		if !delegateRouteRetryable(request, err) ||
			attempt+1 >= maxRouteAttempts || ctx.Err() != nil {
			if execution != nil && execution.Dispatched && !execution.CostKnown {
				return result, &DelegateExecutionError{Err: err, Dispatched: true, CostKnown: false, CostUSD: totalCost}
			}
			if totalCost > 0 {
				return result, &DelegateExecutionError{Err: err, Dispatched: true, CostKnown: true, CostUSD: totalCost}
			}
			return result, err
		}
		// The request remains unpinned. A distinct durable key forces a fresh
		// generic delegate admission, whose router can select another currently
		// eligible agent. This includes admission backpressure: planDelegateGroup
		// may race with live occupancy, and the admission rejection is the
		// authoritative signal. No failed agent is persisted as an exclusion.
		request.RetryTag = fmt.Sprintf("route-retry:%d", attempt+1)
		request.Delegate = ""
		request.routeSelected = false
		if originalCostLimit > 0 {
			request.MaxCostUSD = originalCostLimit - totalCost
			if request.MaxCostUSD <= 0 {
				return DelegateResult{}, &DelegateExecutionError{Err: errors.New("delegate route retry exhausted its cost limit"), Dispatched: true, CostKnown: true, CostUSD: totalCost}
			}
		}
	}
}

func delegateRouteRetryable(request DelegateRequest, err error) bool {
	if err == nil || (request.Delegate != "" && !request.routeSelected) || request.Participant != "" {
		return false
	}
	return errors.Is(err, ErrDelegateTerminal) || IsCapacityBackpressure(err)
}

func (c *HTTPAgentClient) delegateOnce(ctx context.Context, request DelegateRequest) (DelegateResult, error) {
	if request.Role == "" || request.Persona == "" || request.Prompt == "" {
		return DelegateResult{}, errors.New("delegate role, persona, and prompt are required")
	}
	payload := map[string]any{"role": request.Role, "persona": request.Persona, "prompt": request.Prompt, "cwd": request.Workdir, "tools": request.Tools}
	if request.MaxCostUSD > 0 {
		payload["max_cost_usd"] = request.MaxCostUSD
	}
	if request.MaxTurnsCap > 0 {
		payload["max_turns_cap"] = request.MaxTurnsCap
	}
	if request.ToolLoopTimeoutMSCap > 0 {
		payload["tool_loop_timeout_ms_cap"] = request.ToolLoopTimeoutMSCap
	}
	if request.ProvidedTarget {
		payload["provided_target"] = true
	}
	// Empty means ordinary eligibility routing. A non-empty delegate is an
	// explicit positive pin from the workflow, never an exclusion list.
	if request.Delegate != "" {
		payload["via"] = request.Delegate
	}
	if request.Participant != "" {
		payload["participant"] = request.Participant
	}
	key := DelegateJobKey(request)
	var launched struct {
		JobID       int    `json:"job_id"`
		Participant string `json:"participant"`
		Replayed    bool   `json:"replayed"`
		Error       string `json:"error"`
	}
	// The reservation lives with agent_jobs on the resource plane, so the lookup,
	// the launch and the write happen in one call on the side that owns both.
	// Splitting them across the network left a paid-for job unreplayable whenever
	// this process died between launching and recording.
	//
	// WorkItemID scopes the reservation to a workflow item; without one there is
	// nothing to replay into, so the key is withheld and every call launches.
	if request.WorkItemID != "" {
		payload["work_item_id"] = request.WorkItemID
		// A replay-only attempt must never cause a launch: its spend was already
		// reconciled, so a second job would be unreconciled duplicate spend.
		if request.ReplayOnly {
			payload["replay_only"] = true
		}
	} else {
		key = ""
	}
	if err := c.doJSONKey(ctx, http.MethodPost, "/v1/delegate/run", payload, &launched, key); err != nil {
		// A non-2xx response is an authoritative admission rejection. A
		// transport/decode failure after sending the idempotent request is
		// ambiguous and must retain the reservation for durable replay.
		var rejected *agentHTTPStatusError
		return DelegateResult{}, delegateExecutionError(err, !errors.As(err, &rejected), false, 0)
	}
	// A replay-only attempt that found no reservation is reported as an absent
	// job rather than a launch, because the plane was forbidden from launching.
	if launched.JobID <= 0 && request.ReplayOnly {
		return DelegateResult{}, &DelegateExecutionError{Err: ErrDelegateReplayUnavailable, Dispatched: true, CostKnown: false}
	}
	if launched.JobID <= 0 {
		detail := strings.TrimSpace(SafeDiagnostic(launched.Error))
		if detail == "" {
			detail = "empty launch response"
		} else {
			var printable strings.Builder
			for _, r := range detail {
				if r < ' ' || r == 0x7f {
					_, _ = fmt.Fprintf(&printable, `\x%02x`, r)
					continue
				}
				printable.WriteRune(r)
			}
			detail = printable.String()
		}
		return DelegateResult{}, fmt.Errorf("%w: %s", ErrDelegateNoJobID, detail)
	}
	ticker := time.NewTicker(c.pollEvery)
	defer ticker.Stop()
	// A successful launch is initially unassigned until status proves otherwise.
	// This also bounds jobs whose status endpoint never becomes readable.
	unassignedSince := time.Now()
	for {
		if err := ctx.Err(); err != nil {
			_ = c.CancelAndRelease(launched.JobID, key, ctx)
			return DelegateResult{}, delegateExecutionError(err, true, false, 0)
		}
		var status struct {
			JobStatus string  `json:"job_status"`
			Result    string  `json:"result"`
			Agent     string  `json:"agent_name"`
			CostUSD   float64 `json:"cost_usd"`
			CostKnown bool    `json:"cost_known"`
			Error     string  `json:"error"`
		}
		if err := c.doJSON(ctx, http.MethodPost, "/v1/delegate/status", map[string]any{"job_id": launched.JobID, "full_result": true, "result_limit": -1}, &status); err != nil {
			if ctx.Err() != nil {
				_ = c.CancelAndRelease(launched.JobID, key, ctx)
				return DelegateResult{}, delegateExecutionError(ctx.Err(), true, false, 0)
			}
			// Once the resource plane has acknowledged that a job is waiting
			// unassigned, transient status failures cannot extend its lease.
			if !unassignedSince.IsZero() && time.Since(unassignedSince) >= c.delegatePendingTimeout() {
				return DelegateResult{}, c.expireUnassigned(launched.JobID, key, unassignedSince)
			}
			if !unassignedSince.IsZero() {
				select {
				case <-ctx.Done():
					_ = c.CancelAndRelease(launched.JobID, key, ctx)
					return DelegateResult{}, ctx.Err()
				case <-ticker.C:
					continue
				}
			}
			// An assigned observation clears the lease. Preserve its durable mapping
			// when the status plane later fails so a retry cannot overlap the job.
			return DelegateResult{}, delegateExecutionError(err, true, false, 0)
		}
		// Any nonterminal status without a worker lease is not progress. The
		// resource plane persists agent_name as soon as routing selects an agent,
		// while the job is still pending; only its transition to running proves a
		// worker actually started. Treating a routed pending job as assigned makes
		// it immortal when capacity never admits it.
		assigned := status.JobStatus == "running" && strings.TrimSpace(status.Agent) != ""
		terminal := isTerminalDelegateStatus(status.JobStatus)
		if !assigned && !terminal {
			if unassignedSince.IsZero() {
				unassignedSince = time.Now()
			} else if time.Since(unassignedSince) >= c.delegatePendingTimeout() {
				return DelegateResult{}, c.expireUnassigned(launched.JobID, key, unassignedSince)
			}
		} else {
			unassignedSince = time.Time{}
		}
		switch status.JobStatus {
		case "done":
			return DelegateResult{Response: status.Result, Agent: status.Agent, Participant: launched.Participant, CostUSD: status.CostUSD, CostUnknown: !status.CostKnown}, nil
		case "partial":
			if strings.TrimSpace(status.Result) == "" {
				c.releaseReservation(context.WithoutCancel(ctx), key, 0)
				return DelegateResult{}, delegateExecutionError(fmt.Errorf("delegate job %d returned an empty partial result", launched.JobID), true, status.CostKnown, status.CostUSD)
			}
			if !request.AcceptPartial {
				c.releaseReservation(context.WithoutCancel(ctx), key, 0)
				return DelegateResult{}, delegateExecutionError(fmt.Errorf("delegate job %d returned a partial result for a block that requires completion", launched.JobID), true, status.CostKnown, status.CostUSD)
			}
			// A delegate can leave a complete, independently verifiable artifact and
			// still be labelled partial when its final synthesis fails. Preserve that
			// artifact and its terminal durable mapping: an identical retry must replay
			// the same artifact idempotently, not launch overlapping work. Callers that
			// need corrective synthesis change the prompt, which changes the job key.
			// The native branch-producing block validates its own output contract.
			return DelegateResult{Response: status.Result, Agent: status.Agent, CostUSD: status.CostUSD, CostUnknown: !status.CostKnown, Partial: true}, nil
		case "failed", "cancelled", "stopped", "invalid", "not_found":
			c.releaseReservation(context.WithoutCancel(ctx), key, 0)
			detail := firstNonempty(strings.TrimSpace(status.Error), strings.TrimSpace(status.Result))
			if detail != "" {
				return DelegateResult{}, delegateExecutionError(fmt.Errorf("%w: job %d %s: %s", ErrDelegateTerminal, launched.JobID, status.JobStatus, detail), true, status.CostKnown, status.CostUSD)
			}
			return DelegateResult{}, delegateExecutionError(fmt.Errorf("%w: job %d %s", ErrDelegateTerminal, launched.JobID, status.JobStatus), true, status.CostKnown, status.CostUSD)
		}
		select {
		case <-ctx.Done():
			// The remote resource-plane job outlives an HTTP poll unless it is
			// explicitly cancelled. Wait for the cancellation acknowledgement so
			// a wall-cap resume cannot overlap the old job in the same worktree.
			_ = c.CancelAndRelease(launched.JobID, key, ctx)
			return DelegateResult{}, delegateExecutionError(ctx.Err(), true, false, 0)
		case <-ticker.C:
		}
	}
}

func (c *HTTPAgentClient) DelegateGroup(ctx context.Context, requests []DelegateRequest) []DelegateGroupResult {
	planned, err := c.planDelegateGroup(ctx, requests)
	if err != nil {
		out := make([]DelegateGroupResult, len(requests))
		for i := range out {
			out[i].Err = err
		}
		return out
	}
	out := make([]DelegateGroupResult, len(requests))
	var wg sync.WaitGroup
	wg.Add(len(requests))
	for i := range requests {
		go func(i int) {
			defer wg.Done()
			result, err := c.Delegate(ctx, planned[i])
			out[i].Response, out[i].CostUSD, out[i].CostUnknown, out[i].Participant, out[i].Err = result.Response, result.CostUSD, result.CostUnknown, result.Participant, err
			if out[i].Err == nil && strings.TrimSpace(out[i].Participant) == "" {
				out[i].Err = errors.New("delegate service returned no participant handle")
			}
		}(i)
	}
	wg.Wait()
	return out
}

type delegateCandidate struct {
	Name, Provider, Model string
	Roles, Personas       []string
	MaxParallel           int
	// ActiveDelegates is the agent's live occupancy as reported by the delegate
	// service, or -1 when it did not report any. Unknown must behave as idle:
	// this only orders and narrows candidates, so treating ignorance as "busy"
	// would drop seats for a reason we cannot substantiate.
	ActiveDelegates int
}

// freeSlots is how many more delegates this agent can take right now. Occupancy
// the agent already carries counts against its cap, not just the seats being
// assigned in this group -- a panel that ignores it seats a saturated agent and
// the seat fails at admission with aimee_err=concurrency_limit.
func (c delegateCandidate) freeSlots(groupUses int) int {
	active := c.ActiveDelegates
	if active < 0 {
		active = 0
	}
	return c.MaxParallel - active - groupUses
}

func (c *HTTPAgentClient) planDelegateGroup(ctx context.Context, requests []DelegateRequest) ([]DelegateRequest, error) {
	planned := append([]DelegateRequest(nil), requests...)
	needsRoute := false
	for _, request := range planned {
		if delegateSelectorUnbound(request.Delegate) && request.Participant == "" {
			needsRoute = true
			break
		}
	}
	if !needsRoute {
		return planned, nil
	}
	candidates, err := c.delegateCandidates(ctx)
	if err != nil {
		return nil, fmt.Errorf("load delegate group eligibility: %w", err)
	}
	providerUses, modelUses, agentUses := map[string]int{}, map[string]int{}, map[string]int{}
	for _, request := range planned {
		if delegateSelectorUnbound(request.Delegate) || request.Participant != "" {
			continue
		}
		for _, candidate := range candidates {
			if candidate.Name == request.Delegate {
				providerUses[candidate.Provider]++
				modelUses[candidate.Model]++
				agentUses[candidate.Name]++
				break
			}
		}
	}
	for i := range planned {
		request := &planned[i]
		if !delegateSelectorUnbound(request.Delegate) || request.Participant != "" {
			continue
		}
		// Prefer a seat that can actually start now, then fall back to the full
		// eligible set. PREFER, never exclude: if every eligible agent is busy the
		// seat is still filled and the dispatch is retried as capacity
		// backpressure (isCapacityBackpressure), which is recoverable -- whereas
		// refusing to route makes the whole panel unreachable, which is not.
		best := -1
		for _, freeOnly := range []bool{true, false} {
			for j, candidate := range candidates {
				if !matchesSelector(candidate.Roles, request.Role) || !matchesSelector(candidate.Personas, request.Persona) {
					continue
				}
				if agentUses[candidate.Name] >= candidate.MaxParallel {
					continue
				}
				if freeOnly && candidate.freeSlots(agentUses[candidate.Name]) < 1 {
					continue
				}
				if best < 0 || candidateLess(candidate, candidates[best], providerUses, modelUses, agentUses) {
					best = j
				}
			}
			if best >= 0 {
				break
			}
		}
		if best < 0 {
			return nil, fmt.Errorf("delegate service cannot fill seat %d (%s/%s) within enabled capacity", i+1, request.Role, request.Persona)
		}
		chosen := candidates[best]
		request.Delegate = chosen.Name
		request.routeSelected = true
		providerUses[chosen.Provider]++
		modelUses[chosen.Model]++
		agentUses[chosen.Name]++
	}
	return planned, nil
}

func delegateSelectorUnbound(selector string) bool {
	return selector == "" || selector == "$random"
}

func candidateLess(a, b delegateCandidate, providerUses, modelUses, agentUses map[string]int) bool {
	as := boolInt(providerUses[a.Provider] > 0) + boolInt(modelUses[a.Model] > 0)
	bs := boolInt(providerUses[b.Provider] > 0) + boolInt(modelUses[b.Model] > 0)
	if as != bs {
		return as < bs
	}
	if agentUses[a.Name] != agentUses[b.Name] {
		return agentUses[a.Name] < agentUses[b.Name]
	}
	return a.Name < b.Name
}

func boolInt(value bool) int {
	if value {
		return 1
	}
	return 0
}

func matchesSelector(values []string, wanted string) bool {
	if len(values) == 0 {
		return true
	}
	for _, value := range values {
		if value == "all" || value == wanted {
			return true
		}
	}
	return false
}

func (c *HTTPAgentClient) delegateCandidates(ctx context.Context) ([]delegateCandidate, error) {
	var response struct {
		Agents []struct {
			Name        string `json:"name"`
			Provider    string `json:"provider"`
			Model       string `json:"model"`
			Enabled     bool   `json:"enabled"`
			Available   *bool  `json:"delegate_available"`
			PrimaryOnly bool   `json:"primary_only"`
			MaxParallel int    `json:"max_parallel"`
			// Absent when the delegate service cannot determine occupancy; a
			// pointer so "absent" stays distinguishable from a reported zero.
			ActiveDelegates *int     `json:"active_delegates"`
			Roles           []string `json:"roles"`
			Personas        []string `json:"personas"`
		} `json:"agents"`
	}
	if err := c.doJSON(ctx, http.MethodGet, "/v1/model/list", nil, &response); err != nil {
		return nil, err
	}
	var candidates []delegateCandidate
	for _, agent := range response.Agents {
		if !agent.Enabled || (agent.Available != nil && !*agent.Available) || agent.PrimaryOnly || strings.TrimSpace(agent.Name) == "" || agent.MaxParallel < 1 {
			continue
		}
		provider := strings.TrimSpace(agent.Provider)
		model := strings.TrimSpace(agent.Model)
		if provider == "" {
			provider = agent.Name
		}
		if model == "" {
			model = agent.Name
		}
		active := -1
		if agent.ActiveDelegates != nil {
			active = *agent.ActiveDelegates
		}
		candidates = append(candidates, delegateCandidate{Name: agent.Name, Provider: provider, Model: model,
			Roles: agent.Roles, Personas: agent.Personas, MaxParallel: agent.MaxParallel, ActiveDelegates: active})
	}
	return candidates, nil
}

func isTerminalDelegateStatus(status string) bool {
	switch status {
	case "done", "partial", "failed", "cancelled", "stopped", "invalid", "not_found":
		return true
	default:
		return false
	}
}

// expireUnassigned reclaims a delegate job no worker ever picked up.
//
// agent_jobs belongs to the resource plane, so the transition is asked for
// rather than performed here: only the plane can decide atomically that the row
// is still unassigned, which is what stops a worker starting in the same
// instant from being cancelled underneath. This client therefore holds no
// database handle at all, which is what lets it run inside a module process.
func (c *HTTPAgentClient) expireUnassigned(jobID int, key string, since time.Time) error {
	cancelCtx, cancel := context.WithTimeout(context.Background(), delegateCancelTimeout)
	defer cancel()
	var response struct {
		Cancelled *bool `json:"cancelled"`
	}
	err := c.doJSON(cancelCtx, http.MethodPost, "/v1/delegate/cancel_unassigned", map[string]any{
		"job_id":              jobID,
		"reason":              "unassigned delegate lease expired",
		"min_unassigned_secs": int(c.delegatePendingTimeout() / time.Second),
	}, &response)
	if err != nil {
		return errors.Join(ErrDelegateUnassignedExpired, fmt.Errorf("cancel expired delegate job %d: %w", jobID, err))
	}
	if response.Cancelled == nil || !*response.Cancelled {
		// The job was assigned, already terminal, or not yet old enough. Keep the
		// reservation: it may still produce a result worth replaying.
		return errors.Join(ErrDelegateUnassignedExpired,
			fmt.Errorf("delegate job %d is no longer pending and unassigned; durable reservation retained", jobID))
	}
	c.releaseReservation(context.Background(), key, 0)
	return fmt.Errorf("%w: job %d (%s) remained unassigned for %s", ErrDelegateUnassignedExpired, jobID, key, time.Since(since).Round(time.Millisecond))
}

// releaseReservation drops the plane-side reservation for key so the next
// attempt launches fresh. Pass jobID to make it a compare-delete; zero releases
// unconditionally.
//
// Whether a result is replayable is the caller's judgement, not the plane's --
// an acceptable partial for one block is a failure for another -- so the policy
// stays here while the storage stays with agent_jobs.
//
// Failing to release is not fatal: the reservation still points at a terminal
// job, and a replay observes that terminal row rather than reusing a result.
func (c *HTTPAgentClient) releaseReservation(ctx context.Context, key string, jobID int) {
	if key == "" {
		return
	}
	payload := map[string]any{"execution_key": key}
	if jobID > 0 {
		payload["job_id"] = jobID
	}
	releaseCtx, cancel := context.WithTimeout(ctx, delegateForgetTimeout)
	defer cancel()
	_ = c.doJSON(releaseCtx, http.MethodPost, "/v1/delegate/reservation/forget", payload, nil)
}

// DelegateJobKey is the execution key a call reserves under. It is exported
// because the reservation is a contract between this client and the plane that
// stores it: anything reasoning about replay -- including the scheduler's
// terminal sweep -- has to name the same key.
func DelegateJobKey(request DelegateRequest) string {
	// Invocation-time safety controls do not identify different logical work.
	// Reconciliation replaces the original reservation with measured cost, and
	// every invocation derives a fresh remaining-time cap. A replay must still
	// derive the exact key used by the original dispatch or an existing terminal
	// result appears to be lost.
	request.ReplayOnly = false
	request.MaxCostUSD = 0
	request.ToolLoopTimeoutMSCap = 0
	keyMaterial, _ := json.Marshal(request)
	return fmt.Sprintf("%s:%s:%s:%x", request.WorkItemID, request.Stage, request.ExecutionVersion, sha256.Sum256(keyMaterial))
}

func (c *HTTPAgentClient) CancelAndRelease(jobID int, key string, parent context.Context) error {
	cancelCtx, cancel := context.WithTimeout(context.WithoutCancel(parent), delegateCancelTimeout)
	defer cancel()
	var response struct {
		Cancelled *bool `json:"cancelled"`
	}
	// Delegate jobs live on the plural /v1/jobs resource. The singular
	// /v1/job/cancel endpoint controls coordinated jobs and can return HTTP 200
	// while leaving a delegate pending forever.
	if err := c.doJSON(cancelCtx, http.MethodPost, "/v1/jobs/cancel",
		map[string]any{"job_id": jobID, "reason": "WFE turn cancelled"}, &response); err != nil {
		return fmt.Errorf("%w: job %d: %w", ErrDelegateCancelUnacknowledged, jobID, err)
	}
	if response.Cancelled == nil || !*response.Cancelled {
		return fmt.Errorf("%w: job %d", ErrDelegateCancelUnacknowledged, jobID)
	}
	// /v1/jobs/cancel atomically marks the delegate job cancelled before it
	// returns cancelled=true. Release only the reservation that still names this
	// job, so a retry which has already reserved a newer one survives. If the
	// release fails, replay remains safe because status observes that terminal row.
	c.releaseReservation(context.WithoutCancel(parent), key, jobID)
	return nil
}

func (c *HTTPAgentClient) doJSON(ctx context.Context, method, path string, input, output any) error {
	return c.doJSONKey(ctx, method, path, input, output, "")
}
func (c *HTTPAgentClient) doJSONKey(ctx context.Context, method, path string, input, output any, idempotencyKey string) error {
	var body io.Reader
	if input != nil {
		encoded, err := json.Marshal(input)
		if err != nil {
			return err
		}
		body = bytes.NewReader(encoded)
	}
	req, err := http.NewRequestWithContext(ctx, method, c.baseURL+path, body)
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")
	if idempotencyKey != "" {
		req.Header.Set("Idempotency-Key", idempotencyKey)
	}
	if c.bearer != "" {
		req.Header.Set("Authorization", "Bearer "+c.bearer)
	}
	resp, err := c.client.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		data, _ := io.ReadAll(resp.Body)
		return &agentHTTPStatusError{status: resp.StatusCode, detail: strings.TrimSpace(string(data))}
	}
	if err := json.NewDecoder(resp.Body).Decode(output); err != nil {
		return fmt.Errorf("decode agent response: %w", err)
	}
	return nil
}
