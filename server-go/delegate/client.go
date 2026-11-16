// Package delegate is the shared caller-side contract for delegate execution.
// It is deliberately outside modules/: any peer that calls delegates exchanges
// this JSON over the bus without importing the delegates module's implementation.
// Independently exported callers must also be listed by
// scripts/export_c_repositories.py:go_process_shared_sources.
package delegate

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"regexp"
	"strings"
	"sync/atomic"
	"time"

	canonical "github.com/JBailes/aimee/server-go/ir"
	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
)

const (
	EventKind            uint32 = 6657
	StageInvoke          uint32 = 1
	EventGroupPlan       uint32 = 6678
	StageGroupPlan       uint32 = 22
	WireVersion                 = 3
	DefaultDeadline             = 30 * time.Minute
	TerminationDetailMax        = 2048

	TerminationTurnCap           = "turn_cap"
	TerminationExecutionDeadline = "execution_deadline"
	TerminationCancelled         = "cancelled"
)

type DelegateRequest struct {
	Role                 string
	Persona              string
	Delegate             string
	Participant          string
	Input                *canonical.Request
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
	Output      *canonical.Response
	History     []canonical.Message
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
	Output      *canonical.Response
	History     []canonical.Message
	Participant string
	CostUSD     float64
	CostUnknown bool
	Err         error
}

// SemanticText is the explicit application-artifact boundary. Only the
// admitted canonical output can supply application text.
func (r DelegateResult) SemanticText() (string, error) {
	if r.Output == nil {
		return "", errors.New("delegate returned no canonical response")
	}
	return r.Output.SemanticText()
}

func (r DelegateGroupResult) SemanticText() (string, error) {
	return (DelegateResult{Output: r.Output}).SemanticText()
}

// AdmitResult validates the producer contract before any display or domain
// projection. It does not grant task verification or source custody.
func AdmitResult(result *InvocationResult) error {
	if result == nil || result.Version != WireVersion ||
		(result.Status != "done" && result.Status != "failed") {
		return errors.New("invalid delegate terminal envelope")
	}
	if result.Output != nil && !result.Output.Valid() {
		return errors.New("invalid delegate canonical response")
	}
	pending := make(map[string]bool)
	seen := make(map[string]bool)
	for _, message := range result.History {
		raw, err := json.Marshal(message)
		if err != nil || !canonical.ValidMessage(raw) {
			return errors.New("invalid delegate canonical history")
		}
		for _, block := range message.Blocks {
			if block.Type == "tool_use" {
				if message.Role != "assistant" || seen[block.ToolID] {
					return errors.New("invalid delegate tool identity history")
				}
				pending[block.ToolID], seen[block.ToolID] = true, true
			} else if block.Type == "tool_result" {
				if (message.Role != "user" && message.Role != "tool") || !pending[block.ToolID] {
					return errors.New("unbound delegate tool result")
				}
				delete(pending, block.ToolID)
			}
		}
	}
	if result.Status == "done" {
		if result.Output == nil || result.Error != "" || result.Termination != nil || len(pending) != 0 || len(result.History) == 0 {
			return errors.New("successful delegate omitted its canonical terminal observation")
		}
		var final []canonical.Block
		for i := len(result.History) - 1; i >= 0; i-- {
			if result.History[i].Role == "assistant" {
				final = result.History[i].Blocks
				break
			}
		}
		projected := make([]canonical.ResponseBlock, len(result.Output.Blocks))
		for i, b := range result.Output.Blocks {
			projected[i] = canonical.ResponseBlock{Block: b.Block}
		}
		observed := make([]canonical.ResponseBlock, len(final))
		for i, b := range final {
			observed[i] = canonical.ResponseBlock{Block: b}
		}
		a, _ := json.Marshal(observed)
		b, _ := json.Marshal(projected)
		if !bytes.Equal(a, b) {
			return errors.New("delegate terminal differs from canonical history")
		}
		if !result.Output.Complete() {
			return errors.New("delegate output is not a complete canonical model response")
		}
	}
	return nil
}

type DelegateGroupClient interface {
	DelegateGroup(context.Context, []DelegateRequest) []DelegateGroupResult
}

var (
	ErrDelegateTerminal             = errors.New("delegate execution failed")
	ErrDelegateOutputLimit          = errors.New("delegate canonical output token limit reached")
	ErrDelegateReplayUnavailable    = errors.New("delegate calls are stateless; replay-only execution is unavailable")
	ErrDelegateUnassignedExpired    = errors.New("delegate was not assigned")
	ErrDelegateCancelUnacknowledged = errors.New("delegate cancellation was not acknowledged")
	ErrDelegateCostLimitUnsupported = errors.New("delegate CLI execution cannot enforce a monetary cost limit")
	ErrDelegateCapacity             = errors.New("delegate capacity unavailable [aimee_err=concurrency_limit]")
	ErrDelegateCapacityDeadline     = errors.New("delegate capacity wait deadline exceeded [aimee_err=capacity_deadline]")
	ErrDelegateExecutionDeadline    = errors.New("delegate execution deadline exceeded [aimee_err=execution_deadline]")
	ErrDelegateTurnCap              = errors.New("delegate maximum turn count exceeded [aimee_err=turn_cap]")
)

type DelegateExecutionError struct {
	// Output is an admitted producer observation retained only for diagnostics.
	// A failed envelope never grants business/artifact completion authority.
	Output      *canonical.Response
	Err         error
	Dispatched  bool
	CostKnown   bool
	CostUSD     float64
	Termination *TerminationDiagnostic
}

func (e *DelegateExecutionError) Error() string { return e.Err.Error() }
func (e *DelegateExecutionError) Unwrap() error { return e.Err }

// Invocation is the complete delegate-owned wire request. Workflow replay,
// durable-slot, work-item and retry fields intentionally have no JSON mapping.
type Invocation struct {
	Operation string             `json:"operation"`
	Version   int                `json:"version"`
	Role      string             `json:"role"`
	Persona   string             `json:"persona"`
	Model     string             `json:"model,omitempty"`
	Input     *canonical.Request `json:"ir"`
	Workdir   string             `json:"workdir,omitempty"`
	Tools     bool               `json:"tools"`
	MaxTurns  int                `json:"max_turns,omitempty"`
	// ExecutionTimeoutMS is a delegate-owned run bound, not workflow state. It
	// lets the producer terminate work at the same boundary as the bus caller.
	ExecutionTimeoutMS int64 `json:"execution_timeout_ms,omitempty"`
}

type InvocationResult struct {
	Version     int                    `json:"version"`
	Status      string                 `json:"status"`
	Output      *canonical.Response    `json:"ir,omitempty"`
	History     []canonical.Message    `json:"history,omitempty"`
	Agent       string                 `json:"agent,omitempty"`
	Error       string                 `json:"error,omitempty"`
	CostUSD     float64                `json:"cost_usd,omitempty"`
	CostKnown   bool                   `json:"cost_known"`
	Termination *TerminationDiagnostic `json:"termination,omitempty"`
}

// TerminationDiagnostic is the delegate producer's bounded, versioned account
// of why an invocation stopped. Admission failures and ordinary CLI failures do
// not fabricate one: only the actual execution owner (HOST native or external-text producer)
// owns these execution classifications.
type TerminationDiagnostic struct {
	Kind               string `json:"kind"`
	MaxTurns           int    `json:"max_turns,omitempty"`
	ObservedTurns      int    `json:"observed_turns,omitempty"`
	ExecutionTimeoutMS int64  `json:"execution_timeout_ms,omitempty"`
	ElapsedMS          int64  `json:"elapsed_ms"`
	Detail             string `json:"detail,omitempty"`
}

func ValidTerminationDiagnostic(d *TerminationDiagnostic) bool {
	if d == nil || d.ElapsedMS < 0 || d.ExecutionTimeoutMS < 0 ||
		d.MaxTurns < 0 || d.ObservedTurns < 0 || len(d.Detail) > TerminationDetailMax {
		return false
	}
	switch d.Kind {
	case TerminationTurnCap:
		return d.MaxTurns > 0 && d.ObservedTurns >= d.MaxTurns
	case TerminationExecutionDeadline:
		return d.ExecutionTimeoutMS > 0 && d.MaxTurns == 0 && d.ObservedTurns == 0
	case TerminationCancelled:
		return d.MaxTurns == 0 && d.ObservedTurns == 0
	default:
		return false
	}
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
	Models  []string `json:"models,omitempty"`
	// Error is a delegate-domain failure carried by a successful bus exchange.
	// The bus strips response bodies from non-OK transport replies, so load
	// classifications must travel in this versioned envelope.
	Error string `json:"error,omitempty"`
}

// StageCaller is the delegate bus-call seam. The production implementation is
// ring.ConcurrentModuleCaller; exposing the narrow contract also lets integration
// tests cross the real module handler without opening a daemon socket.
type StageCaller interface {
	Call(context.Context, uint32, uint32, uint64, time.Duration, []byte) ([]byte, error)
}

type BusClient struct {
	caller   StageCaller
	deadline time.Duration
	trace    atomic.Uint64
}

func NewBusClient(caller *ring.ConcurrentModuleCaller, deadline time.Duration) (*BusClient, error) {
	return NewClient(caller, deadline)
}

func NewClient(caller StageCaller, deadline time.Duration) (*BusClient, error) {
	if caller == nil {
		return nil, errors.New("delegate bus caller is required")
	}
	if deadline <= 0 {
		deadline = DefaultDeadline
	}
	return &BusClient{caller: caller, deadline: deadline}, nil
}

func delegateHasInput(input *canonical.Request) bool {
	if input == nil || !input.Valid() {
		return false
	}
	for _, message := range input.Messages {
		if message.Role != "user" {
			continue
		}
		for _, block := range message.Blocks {
			if block.Type == "image" || block.Type == "document" ||
				block.Type == "text" && block.Text != nil && *block.Text != "" {
				return true
			}
		}
	}
	return false
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
	if request.Role == "" || request.Persona == "" || !delegateHasInput(request.Input) {
		return DelegateResult{}, errors.New("delegate role, persona, and canonical input are required")
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
	wire := Invocation{Version: WireVersion, Operation: request.WorkItemID, Role: request.Role, Persona: request.Persona,
		Model: request.Delegate, Input: request.Input, Workdir: request.Workdir,
		Tools: request.Tools, MaxTurns: request.MaxTurnsCap,
		ExecutionTimeoutMS: max(1, deadline.Milliseconds())}
	body, err := json.Marshal(wire)
	if err != nil {
		return DelegateResult{}, err
	}
	reply, err := c.caller.Call(ctx, EventKind, StageInvoke, c.trace.Add(1), deadline, body)
	if err != nil {
		err = classifyDelegateError(err)
		if errors.Is(err, ring.ErrModuleCallCapabilityAbsent) ||
			errors.Is(err, ring.ErrModuleCallRejected) ||
			errors.Is(err, ring.ErrModuleCallNotDispatched) {
			return DelegateResult{}, err
		}
		// Once handed to the bus, loss of the reply cannot prove the provider did
		// not run. Preserve that uncertainty at the caller's billing boundary.
		if errors.Is(err, ring.ErrModuleCallDeadline) {
			err = errors.Join(err, context.DeadlineExceeded)
		}
		return DelegateResult{}, &DelegateExecutionError{Err: err, Dispatched: true, CostKnown: false}
	}
	var result InvocationResult
	if !canonical.Decode(reply, &result) || AdmitResult(&result) != nil {
		return DelegateResult{}, &DelegateExecutionError{Err: errors.New("decode delegate result: invalid canonical terminal response"),
			Dispatched: true, CostKnown: false}
	}
	if result.Version != WireVersion || (result.Status != "done" && result.Status != "failed") ||
		(result.Status == "done" && result.Termination != nil) ||
		(result.Termination != nil && (!ValidTerminationDiagnostic(result.Termination) ||
			result.Termination.ExecutionTimeoutMS != wire.ExecutionTimeoutMS)) {
		return DelegateResult{}, &DelegateExecutionError{
			Err:        errors.New("delegate module returned an invalid terminal result"),
			Dispatched: true, CostKnown: false}
	}
	if result.Status == "failed" {
		detail := result.Error
		if detail == "" {
			detail = ErrDelegateTerminal.Error()
		}
		failure := classifyDelegateError(errors.New(detail))
		if result.Termination != nil {
			switch result.Termination.Kind {
			case TerminationTurnCap:
				failure = errors.Join(ErrDelegateTurnCap, errors.New(detail))
			case TerminationExecutionDeadline:
				failure = errors.Join(ErrDelegateExecutionDeadline, context.DeadlineExceeded,
					errors.New(detail))
			case TerminationCancelled:
				failure = errors.Join(context.Canceled, errors.New(detail))
			}
		}
		if !IsCapacityBackpressure(failure) && !IsCapacityDeadline(failure) && !IsExecutionDeadline(failure) {
			if result.Termination == nil {
				failure = fmt.Errorf("%w: %s", ErrDelegateTerminal, detail)
			}
		}
		if result.Output != nil && result.Output.StopReason == canonical.StopMaxTokens {
			failure = errors.Join(ErrDelegateOutputLimit, failure)
		}
		return DelegateResult{}, &DelegateExecutionError{Err: failure, Output: result.Output,
			Dispatched: true, CostKnown: result.CostKnown, CostUSD: result.CostUSD,
			Termination: result.Termination}
	}
	participant := request.Participant
	if participant == "" {
		participant = result.Agent
	}
	return DelegateResult{Output: result.Output, History: result.History,
		Agent: result.Agent, Participant: participant,
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
			if decodeErr := json.Unmarshal(reply, &result); decodeErr != nil || result.Version != WireVersion {
				err = errors.New("delegate module returned an invalid group plan")
			} else if strings.TrimSpace(result.Error) != "" {
				err = errors.New(result.Error)
			} else if len(result.Models) != len(planned) {
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
		err = classifyDelegateError(err)
		for i := range out {
			out[i] = DelegateGroupResult{Participant: requests[i].Participant, Err: err}
		}
		return out
	}
	done := make(chan int, len(requests))
	for i := range planned {
		go func(i int) {
			result, err := c.Delegate(ctx, planned[i])
			out[i] = DelegateGroupResult{Participant: result.Participant, Output: result.Output,
				History: result.History,
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

// IsCapacityDeadline recognizes both the in-process sentinel and its stable
// bus-safe slug. Module status errors cross a process boundary and therefore
// cannot preserve Go error identity without this explicit wire vocabulary.
func IsCapacityDeadline(err error) bool {
	return err != nil && (errors.Is(err, ErrDelegateCapacityDeadline) ||
		strings.Contains(err.Error(), "aimee_err=capacity_deadline"))
}

func IsExecutionDeadline(err error) bool {
	return err != nil && (errors.Is(err, ErrDelegateExecutionDeadline) ||
		strings.Contains(err.Error(), "aimee_err=execution_deadline"))
}

// classifyDelegateError preserves the specific typed deadline sentinel even
// though both deadline classes intentionally unwrap to context.DeadlineExceeded.
func classifyDelegateError(err error) error {
	switch {
	case err == nil:
		return nil
	case IsCapacityDeadline(err):
		return errors.Join(ErrDelegateCapacityDeadline, context.DeadlineExceeded, err)
	case IsCapacityBackpressure(err):
		return errors.Join(ErrDelegateCapacity, err)
	case IsExecutionDeadline(err):
		return errors.Join(ErrDelegateExecutionDeadline, context.DeadlineExceeded, err)
	default:
		return err
	}
}
