package engine

import (
	"context"
	"errors"

	roundtablecfg "github.com/JBailes/aimee/server-go/modules/roundtable/panel"
)

// panelDelegates is the resource plane the roundtable convenes over.
//
// It exists because the panel is a module process that cannot import this
// package, and should not have to: which delegate serves a seat, how a delegate
// failure is classified, and how a diagnostic is redacted are all transport
// concerns. The panel describes the seat it wants and reads back a category it
// never has to parse an error to obtain -- so the failure taxonomy has exactly
// one definition, here, next to the errors it names.
type panelDelegates struct{ runner *NativeRunner }

func (p panelDelegates) request(run roundtablecfg.Run, seat roundtablecfg.SeatRequest) DelegateRequest {
	return DelegateRequest{
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

// result classifies and redacts here rather than in the panel, so the panel
// never grows a second copy of this taxonomy to keep in step with these errors.
func panelSeatResult(participant, response string, cost float64, costUnknown bool, err error) roundtablecfg.SeatResult {
	out := roundtablecfg.SeatResult{Participant: participant, Response: response,
		CostUSD: cost, CostUnknown: costUnknown, Err: err}
	if err == nil {
		return out
	}
	out.ReplayLost = errors.Is(err, ErrDelegateReplayUnavailable)
	out.FailureCategory = panelFailureCategory(err, true)
	out.FailureDetail = safeDiagnostic(err.Error())
	return out
}

func (p panelDelegates) Group(ctx context.Context, run roundtablecfg.Run, seats []roundtablecfg.SeatRequest) []roundtablecfg.SeatResult {
	out := make([]roundtablecfg.SeatResult, len(seats))
	if len(seats) == 0 {
		return out
	}
	requests := make([]DelegateRequest, len(seats))
	for i, seat := range seats {
		requests[i] = p.request(run, seat)
		if run.CostLimitUSD > 0 {
			// Group calls execute concurrently, so their individual ceilings must
			// sum to no more than the review's reservation.
			requests[i].MaxCostUSD = run.CostLimitUSD / float64(len(seats))
		}
	}
	group, ok := p.runner.agents.(DelegateGroupClient)
	if !ok {
		// A roundtable is concurrent seats sharing one reservation, and it never
		// reconstructs grouping or participant identity from single calls. A plane
		// without the generic group contract simply cannot host one.
		unavailable := errors.New("delegate service does not support grouped delegation")
		for i := range out {
			out[i] = panelSeatResult("", "", 0, false, unavailable)
		}
		return out
	}
	for i, call := range group.DelegateGroup(ctx, requests) {
		out[i] = panelSeatResult(call.Participant, call.Response, call.CostUSD, call.CostUnknown, call.Err)
	}
	return out
}

func (p panelDelegates) One(ctx context.Context, run roundtablecfg.Run, seat roundtablecfg.SeatRequest) roundtablecfg.SeatResult {
	request := p.request(run, seat)
	request.MaxCostUSD = run.CostLimitUSD
	result, err := p.runner.agents.Delegate(ctx, request)
	return panelSeatResult(result.Participant, result.Response, result.CostUSD, result.CostUnknown, err)
}
