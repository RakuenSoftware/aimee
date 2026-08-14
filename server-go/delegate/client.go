// Package delegate is the shared caller-side contract for delegate execution.
// It is deliberately outside modules/: any peer that calls delegates exchanges
// this JSON over the bus without importing the delegates module's implementation.
// Independently exported callers must also be listed by
// scripts/export_c_repositories.py:go_process_shared_sources.
package delegate

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"regexp"
	"strings"
	"sync/atomic"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

const (
	EventKind       uint32 = 6657
	StageInvoke     uint32 = 1
	EventGroupPlan  uint32 = 6678
	StageGroupPlan  uint32 = 22
	WireVersion            = 2
	DefaultDeadline        = 30 * time.Minute
)

type DelegateRequest struct {
	Role                 string
	Persona              string
	Delegate             string
	Participant          string
	Prompt               string
	Workdir              string
	Tools                bool
	WorkItemID           string
	Stage                string
	ExecutionVersion     string
	RetryTag             string
	MaxCostUSD           float64
	MaxTurnsCap          int
	ToolLoopTimeoutMSCap int
	ReplayOnly           bool
	DurableSlot          string
	ArtifactStage        string
	ArtifactHash         string
	ProvidedTarget       bool
	AcceptPartial        bool
}

type DelegateResult struct {
	Response    string
	Agent       string
	Participant string
	CostUSD     float64
	CostUnknown bool
	Partial     bool
}

type AgentClient interface {
	Delegate(context.Context, DelegateRequest) (DelegateResult, error)
}

type DelegateGroupResult struct {
	Participant string
	Response    string
	CostUSD     float64
	CostUnknown bool
	Err         error
}

type DelegateGroupClient interface {
	DelegateGroup(context.Context, []DelegateRequest) []DelegateGroupResult
}

var (
	ErrDelegateTerminal             = errors.New("delegate execution failed")
	ErrDelegateReplayUnavailable    = errors.New("delegate calls are stateless; replay-only execution is unavailable")
	ErrDelegateUnassignedExpired    = errors.New("delegate was not assigned")
	ErrDelegateCancelUnacknowledged = errors.New("delegate cancellation was not acknowledged")
	ErrDelegateCostLimitUnsupported = errors.New("delegate CLI execution cannot enforce a monetary cost limit")
)

type DelegateExecutionError struct {
	Err        error
	Dispatched bool
	CostKnown  bool
	CostUSD    float64
}

func (e *DelegateExecutionError) Error() string { return e.Err.Error() }
func (e *DelegateExecutionError) Unwrap() error { return e.Err }

// Invocation is the complete delegate-owned wire request. Workflow replay,
// durable-slot, work-item and retry fields intentionally have no JSON mapping.
type Invocation struct {
	Version  int    `json:"version"`
	Role     string `json:"role"`
	Persona  string `json:"persona"`
	Model    string `json:"model,omitempty"`
	Prompt   string `json:"prompt"`
	Workdir  string `json:"workdir,omitempty"`
	Tools    bool   `json:"tools"`
	MaxTurns int    `json:"max_turns,omitempty"`
	// ExecutionTimeoutMS is a delegate-owned run bound, not workflow state. It
	// lets the producer terminate work at the same boundary as the bus caller.
	ExecutionTimeoutMS int64 `json:"execution_timeout_ms,omitempty"`
}

type InvocationResult struct {
	Version   int     `json:"version"`
	Status    string  `json:"status"`
	Response  string  `json:"response,omitempty"`
	Agent     string  `json:"agent,omitempty"`
	Error     string  `json:"error,omitempty"`
	CostUSD   float64 `json:"cost_usd,omitempty"`
	CostKnown bool    `json:"cost_known"`
}

// GroupPlan carries only delegate-selection inputs. Participant continuity is
// translated caller-side into an explicit model selector before this wire.
type GroupPlan struct {
	Version int             `json:"version"`
	Seats   []GroupPlanSeat `json:"seats"`
}

type GroupPlanSeat struct {
	Role    string `json:"role"`
	Persona string `json:"persona"`
	Model   string `json:"model,omitempty"`
}

type GroupPlanResult struct {
	Version int      `json:"version"`
	Models  []string `json:"models"`
}

type stageCaller interface {
	Call(context.Context, uint32, uint32, uint64, time.Duration, []byte) ([]byte, error)
}

type BusClient struct {
	caller   stageCaller
	deadline time.Duration
	trace    atomic.Uint64
}

func NewBusClient(caller *bus.ConcurrentModuleCaller, deadline time.Duration) (*BusClient, error) {
	if caller == nil {
		return nil, errors.New("delegate bus caller is required")
	}
	if deadline <= 0 {
		deadline = DefaultDeadline
	}
	return &BusClient{caller: caller, deadline: deadline}, nil
}

func (c *BusClient) Delegate(ctx context.Context, request DelegateRequest) (DelegateResult, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	if request.ReplayOnly {
		// Replay identity remains caller-owned by contract. Tell the workflow
		// engine that this request represents an earlier dispatch so its durable
		// reservation recovery decides whether a fresh independent call is safe.
		return DelegateResult{}, &DelegateExecutionError{Err: ErrDelegateReplayUnavailable,
			Dispatched: true, CostKnown: false}
	}
	if request.MaxCostUSD > 0 {
		// CLI agents do not expose a trustworthy live dollar meter. Refuse before
		// dispatch instead of accepting a ceiling the producer cannot enforce.
		return DelegateResult{}, ErrDelegateCostLimitUnsupported
	}
	if request.Role == "" || request.Persona == "" || request.Prompt == "" {
		return DelegateResult{}, errors.New("delegate role, persona, and prompt are required")
	}
	deadline := c.deadline
	if contextDeadline, ok := ctx.Deadline(); ok {
		remaining := time.Until(contextDeadline)
		if remaining <= 0 {
			return DelegateResult{}, errors.Join(ErrDelegateTerminal, context.DeadlineExceeded)
		}
		if remaining < deadline {
			deadline = remaining
		}
	}
	if request.ToolLoopTimeoutMSCap > 0 {
		cap := time.Duration(request.ToolLoopTimeoutMSCap) * time.Millisecond
		if cap < deadline {
			deadline = cap
		}
	}
	wire := Invocation{Version: WireVersion, Role: request.Role, Persona: request.Persona,
		Model: request.Delegate, Prompt: request.Prompt, Workdir: request.Workdir,
		Tools: request.Tools, MaxTurns: request.MaxTurnsCap,
		ExecutionTimeoutMS: max(1, deadline.Milliseconds())}
	body, err := json.Marshal(wire)
	if err != nil {
		return DelegateResult{}, err
	}
	reply, err := c.caller.Call(ctx, EventKind, StageInvoke, c.trace.Add(1), deadline, body)
	if err != nil {
		if errors.Is(err, bus.ErrModuleCallCapabilityAbsent) ||
			errors.Is(err, bus.ErrModuleCallRejected) ||
			errors.Is(err, bus.ErrModuleCallNotDispatched) {
			return DelegateResult{}, err
		}
		// Once handed to the bus, loss of the reply cannot prove the provider did
		// not run. Preserve that uncertainty at the caller's billing boundary.
		if errors.Is(err, bus.ErrModuleCallDeadline) {
			err = errors.Join(err, context.DeadlineExceeded)
		}
		return DelegateResult{}, &DelegateExecutionError{Err: err, Dispatched: true, CostKnown: false}
	}
	var result InvocationResult
	if err := json.Unmarshal(reply, &result); err != nil {
		return DelegateResult{}, &DelegateExecutionError{Err: fmt.Errorf("decode delegate result: %w", err),
			Dispatched: true, CostKnown: false}
	}
	if result.Version != WireVersion || (result.Status != "done" && result.Status != "failed") {
		return DelegateResult{}, &DelegateExecutionError{
			Err:        errors.New("delegate module returned an invalid terminal result"),
			Dispatched: true, CostKnown: false}
	}
	if result.Status == "failed" {
		detail := result.Error
		if detail == "" {
			detail = ErrDelegateTerminal.Error()
		}
		return DelegateResult{}, &DelegateExecutionError{Err: fmt.Errorf("%w: %s", ErrDelegateTerminal, detail),
			Dispatched: true, CostKnown: result.CostKnown, CostUSD: result.CostUSD}
	}
	participant := request.Participant
	if participant == "" {
		participant = result.Agent
	}
	return DelegateResult{Response: result.Response, Agent: result.Agent, Participant: participant,
		CostUSD: result.CostUSD, CostUnknown: !result.CostKnown}, nil
}

func (c *BusClient) DelegateGroup(ctx context.Context, requests []DelegateRequest) []DelegateGroupResult {
	if ctx == nil {
		ctx = context.Background()
	}
	out := make([]DelegateGroupResult, len(requests))
	if len(requests) == 0 {
		return out
	}
	planned := append([]DelegateRequest(nil), requests...)
	plan := GroupPlan{Version: WireVersion, Seats: make([]GroupPlanSeat, len(planned))}
	for i := range planned {
		model := planned[i].Delegate
		if planned[i].Participant != "" && (model == "" || model == "$random") {
			model = planned[i].Participant
		}
		plan.Seats[i] = GroupPlanSeat{Role: planned[i].Role, Persona: planned[i].Persona, Model: model}
	}
	body, err := json.Marshal(plan)
	if err == nil {
		var reply []byte
		reply, err = c.caller.Call(ctx, EventGroupPlan, StageGroupPlan, c.trace.Add(1), c.deadline, body)
		if err == nil {
			var result GroupPlanResult
			if decodeErr := json.Unmarshal(reply, &result); decodeErr != nil ||
				result.Version != WireVersion || len(result.Models) != len(planned) {
				err = errors.New("delegate module returned an invalid group plan")
			} else {
				for i := range planned {
					if strings.TrimSpace(result.Models[i]) == "" {
						err = errors.New("delegate module returned an empty group assignment")
						break
					}
					planned[i].Delegate = result.Models[i]
				}
			}
		}
	}
	if err != nil {
		for i := range out {
			out[i] = DelegateGroupResult{Participant: requests[i].Participant, Err: err}
		}
		return out
	}
	done := make(chan int, len(requests))
	for i := range planned {
		go func(i int) {
			result, err := c.Delegate(ctx, planned[i])
			out[i] = DelegateGroupResult{Participant: result.Participant, Response: result.Response,
				CostUSD: result.CostUSD, CostUnknown: result.CostUnknown, Err: err}
			done <- i
		}(i)
	}
	for range requests {
		<-done
	}
	return out
}

var diagnosticRedactions = []struct {
	pattern     *regexp.Regexp
	replacement string
}{
	{regexp.MustCompile(`(?i)(authorization\s*:\s*(?:bearer|basic)\s+)[^\s,;]+`), `${1}[REDACTED]`},
	{regexp.MustCompile(`(?i)(cookie\s*:\s*)[^\r\n]+`), `${1}[REDACTED]`},
	{regexp.MustCompile(`(?i)([a-z][a-z0-9+.-]*://)[^/@\s]+@`), `${1}[REDACTED]@`},
	{regexp.MustCompile(`(?i)("?(?:api[_-]?key|access[_-]?token|token|password|secret)"?\s*[:=]\s*)"(?:\\.|[^"\\])*"`), `${1}"[REDACTED]"`},
	{regexp.MustCompile(`(?i)((?:api[_-]?key|access[_-]?token|token|password|secret)\s*[:=]\s*)'(?:\\.|[^'\\])*'`), `${1}'[REDACTED]'`},
	{regexp.MustCompile(`(?im)("?(?:api[_-]?key|access[_-]?token|token|password|secret)"?\s*[:=]\s*)"(?:\\.|[^"\\\r\n])*(?:\\)?$`), `${1}"[REDACTED]`},
	{regexp.MustCompile(`(?im)((?:api[_-]?key|access[_-]?token|token|password|secret)\s*[:=]\s*)'(?:\\.|[^'\\\r\n])*(?:\\)?$`), `${1}'[REDACTED]`},
	{regexp.MustCompile(`(?i)((?:api[_-]?key|access[_-]?token|token|password|secret)["']?\s*[:=]\s*["']?)[^\s,"';}]+`), `${1}[REDACTED]`},
	{regexp.MustCompile(`\bAKIA[0-9A-Z]{16}\b`), `[REDACTED_AWS_ACCESS_KEY]`},
	{regexp.MustCompile(`\beyJ[A-Za-z0-9_-]*\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\b`), `[REDACTED_JWT]`},
	{regexp.MustCompile(`(?s)-----BEGIN [^-\r\n]*PRIVATE KEY-----.*?-----END [^-\r\n]*PRIVATE KEY-----`), `[REDACTED_PRIVATE_KEY]`},
}

func SafeDiagnostic(detail string) string {
	for _, redaction := range diagnosticRedactions {
		detail = redaction.pattern.ReplaceAllString(detail, redaction.replacement)
	}
	return detail
}

func IsCapacityBackpressure(err error) bool {
	if err == nil {
		return false
	}
	detail := err.Error()
	return strings.Contains(detail, "aimee_err=concurrency_limit") ||
		strings.Contains(detail, "is rate-limited; retry in")
}
