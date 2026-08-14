package roundtable

import (
	"context"
	"crypto/sha256"
	"errors"
	"fmt"
	"strings"

	"github.com/JBailes/aimee/server-go/delegate"
	"github.com/JBailes/aimee/server-go/modules/roundtable/panel"
)

// PanelReviewer convenes a saved roundtable through the delegates bus stage.
//
// It is the whole of what the module process needs: a preset store to resolve
// the named panel, and a delegate client to seat it. There is no database handle
// and no workflow state here -- a review is a bounded question about one
// artifact, and everything durable about it belongs to the side that owns
// agent_jobs.
type PanelReviewer struct {
	presets *panel.Store
	seats   panel.Delegates
}

// NewPanelReviewer takes the seat contract rather than a transport, so what
// convenes a panel is substitutable and this stays testable without a live
// module bus.
func NewPanelReviewer(presets *panel.Store, seats panel.Delegates) (*PanelReviewer, error) {
	if presets == nil || seats == nil {
		return nil, errors.New("roundtable review needs a preset store and a way to seat delegates")
	}
	return &PanelReviewer{presets: presets, seats: seats}, nil
}

// NewBusDelegates seats a panel through the delegates module's bus stage.
func NewBusDelegates(client *delegate.BusClient) panel.Delegates {
	return seatBus{client: client}
}

// Review runs one roundtable and returns its verdict.
//
// The request names a saved roundtable, and resolution fails closed: convening
// a panel the operator never configured is worse than not reviewing at all,
// because the unconfigured shape is invisible in the result.
func (r *PanelReviewer) Review(ctx context.Context, request panel.ReviewRequest) (panel.RunResult, error) {
	if r == nil || r.presets == nil || r.seats == nil {
		return panel.RunResult{}, errors.New("roundtable reviewer is not configured")
	}
	// A caller may name a GitHub pull request instead of pasting its diff.
	// Resolving it here rather than at some caller's edge is what keeps every
	// review -- workflow gate, CLI, MCP tool -- accepting the same artifact
	// forms, and keeps reviewers from spending their tool budget fetching it.
	materialized, err := panel.MaterializeArtifact(ctx, request.Artifact, nil)
	if err != nil {
		return panel.RunResult{}, err
	}
	request.Artifact = materialized

	artifact := strings.TrimSpace(request.Artifact)
	if len(artifact) < 20 {
		return panel.RunResult{}, panel.ValidationError{Message: "roundtable artifact must be at least 20 characters"}
	}
	if len(request.Artifact) > panel.MaxArtifactBytes {
		return panel.RunResult{}, panel.ValidationError{Message: "roundtable artifact exceeds 16 MiB limit"}
	}
	stage := strings.TrimSpace(request.ArtifactStage)
	if stage == "" {
		stage = "frozen_diff"
	}
	original := strings.TrimSpace(request.OriginalRequest)
	if original == "" {
		original = "Review the supplied artifact for correctness, completeness, security, and test quality."
	}
	// A caller-supplied run id is the review's identity and is preserved. Without
	// one, derive it from the request and artifact so an identical review is
	// identifiable rather than arbitrary.
	id := strings.TrimSpace(request.RunID)
	if id == "" {
		sum := sha256.Sum256([]byte(original + "\x00" + request.Artifact))
		id = fmt.Sprintf("roundtable-%x", sum[:12])
	}

	convened, err := r.presets.Resolve(request.Roundtable, request.Lenses, request.Pins)
	if err != nil {
		// An unresolvable panel is not a verdict about the artifact, so it is
		// reported as a park with the reason rather than as a review failure.
		return panel.RunResult{Status: panel.StatusPending, PauseReason: "panel_unreachable",
			Detail: err.Error(), RunID: id}, nil
	}

	run := panel.Run{
		ID:               id,
		Stage:            request.Stage,
		ExecutionVersion: request.ExecutionVersion,
		ReplayOnly:       request.ReplayOnly,
		CostLimitUSD:     request.CostLimitUSD,
		Workdir:          request.Workdir,
		OriginalRequest:  original,
		Reviewed: panel.Artifact{
			Stage:   stage,
			Content: request.Artifact,
			Hash:    panel.Hash([]byte(request.Artifact)),
		},
	}
	result, err := panel.Convene(ctx, r.seats, run, convened, request.Focus)
	if err != nil {
		return result, err
	}
	result.RunID = id
	result.ArtifactHash = run.Reviewed.Hash
	// The verdict must be tied to the bytes it was made about. A result whose
	// feedback names different content is not evidence this artifact was
	// reviewed, so it is surfaced as an error rather than served as a verdict.
	if result.Feedback != nil && result.Feedback.ArtifactHash != run.Reviewed.Hash {
		return result, errors.New("roundtable result artifact identity mismatch")
	}
	return result, nil
}

// seatBus adapts the delegate bus client to the panel's seat contract.
//
// Classifying and redacting a seat failure happens here because this is the
// side that knows the transport's error taxonomy; the panel reports what it is
// told and never parses an error itself.
type seatBus struct{ client *delegate.BusClient }

func (s seatBus) request(run panel.Run, seat panel.SeatRequest) delegate.DelegateRequest {
	return delegate.DelegateRequest{
		Role:        seat.Role,
		Persona:     seat.Persona,
		Delegate:    seat.Selector,
		Participant: seat.Participant,
		Prompt:      seat.Prompt,
		Workdir:     run.Workdir,
		Tools:       seat.Tools,
		MaxTurnsCap: seat.MaxTurnsCap,
		DurableSlot: seat.DurableSlot,
		// The seat prompt already carries the complete artifact, so unrelated
		// worktree-diff evidence would only compete with it for attention.
		ProvidedTarget:   true,
		ArtifactStage:    seat.ArtifactStage,
		ArtifactHash:     seat.ArtifactHash,
		WorkItemID:       run.ID,
		Stage:            run.Stage,
		ExecutionVersion: run.ExecutionVersion,
		ReplayOnly:       run.ReplayOnly,
		MaxCostUSD:       seat.MaxCostUSD,
	}
}

func seatResult(participant, response string, cost float64, costUnknown bool, err error) panel.SeatResult {
	out := panel.SeatResult{Participant: participant, Response: response,
		CostUSD: cost, CostUnknown: costUnknown, Err: err}
	if err == nil {
		return out
	}
	out.ReplayLost = errors.Is(err, delegate.ErrDelegateReplayUnavailable)
	out.FailureCategory = seatFailureCategory(err)
	out.FailureDetail = delegate.SafeDiagnostic(err.Error())
	return out
}

// seatFailureCategory names why a seat failed in the transport's own terms, so
// a panel that cannot review is distinguishable from a reviewer that would not.
func seatFailureCategory(err error) string {
	switch {
	case errors.Is(err, delegate.ErrDelegateCapacityDeadline):
		return "capacity_deadline"
	case errors.Is(err, context.DeadlineExceeded):
		return "deadline"
	case errors.Is(err, delegate.ErrDelegateReplayUnavailable):
		return "replay_unavailable"
	case errors.Is(err, delegate.ErrDelegateCostLimitUnsupported):
		return "cost_limit_unsupported"
	case errors.Is(err, delegate.ErrDelegateUnassignedExpired):
		return "unassigned_expired"
	case delegate.IsCapacityBackpressure(err):
		return "capacity_backpressure"
	case errors.Is(err, delegate.ErrDelegateTerminal):
		return "delegate_terminal"
	default:
		return "delegate_error"
	}
}

func (s seatBus) Group(ctx context.Context, run panel.Run, seats []panel.SeatRequest) []panel.SeatResult {
	out := make([]panel.SeatResult, len(seats))
	if len(seats) == 0 {
		return out
	}
	requests := make([]delegate.DelegateRequest, len(seats))
	for i, seat := range seats {
		requests[i] = s.request(run, seat)
		if run.CostLimitUSD > 0 {
			// Seats execute concurrently, so their individual ceilings must sum to
			// no more than the review's reservation.
			requests[i].MaxCostUSD = run.CostLimitUSD / float64(len(seats))
		}
	}
	for i, call := range s.client.DelegateGroup(ctx, requests) {
		out[i] = seatResult(call.Participant, call.Response, call.CostUSD, call.CostUnknown, call.Err)
	}
	return out
}

func (s seatBus) One(ctx context.Context, run panel.Run, seat panel.SeatRequest) panel.SeatResult {
	request := s.request(run, seat)
	request.MaxCostUSD = run.CostLimitUSD
	result, err := s.client.Delegate(ctx, request)
	return seatResult(result.Participant, result.Response, result.CostUSD, result.CostUnknown, err)
}
