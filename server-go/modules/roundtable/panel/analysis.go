package panel

import (
	"context"
	"fmt"
	"strings"
)

const (
	delegateRole        = "review"
	delegateMaxTurnsCap = 24
)

type SeatReport struct {
	Seat     Seat
	Response panelResponse
}

// Analysis is the outcome of one round of independent seat reviews, before any
// discussion or chairing.
type Analysis struct {
	Feedback    ReviewFeedback
	Approvals   int
	Voters      int
	CostUSD     float64
	CostUnknown bool
	// Unreachable is the joined per-seat failure summary. Non-empty means at
	// least one seat produced no usable verdict; whether that matters is the
	// caller's quorum decision, not this function's.
	Unreachable string
	Reports     []SeatReport
	Failures    []ParticipantFailure
	// ReplayLost records that a seat could not be replayed because its durable
	// result is gone. Retrying cannot fix that; only reservation recovery can,
	// and it is reached by returning an error rather than parking.
	ReplayLost bool
}

type seatOutcome struct {
	seat        Seat
	result      panelResponse
	raw         string
	cost        float64
	costUnknown bool
	err         error
	category    string
	detail      string
	replayLost  bool
}

// RunAnalysis convenes every seat once, concurrently, and folds their verdicts
// into one feedback set.
//
// A seat that fails is dropped from the vote rather than counted against the
// artifact: absence of evidence is not evidence of a defect, and letting one
// garbled response veto the panel produced gates no revision could satisfy. The
// two exceptions are deliberate and fail closed -- a seat that reviewed the
// wrong artifact stage, or whose alignment is not established, contributes a
// blocking finding, because both are how a prompt-injected review would look.
func RunAnalysis(ctx context.Context, delegates Delegates, run Run, seats []Seat,
	prompt, artifactHash, artifactStage string, panelRound int) Analysis {
	requests := make([]SeatRequest, len(seats))
	for i, seat := range seats {
		// Repeated persona/agent specifications must not collide and reuse one
		// remote result, so each capacity seat carries a distinct durable slot.
		// An empty Selector is deliberate: generic delegation resolves eligibility.
		requests[i] = SeatRequest{Role: delegateRole, Persona: seat.Persona, Selector: seat.Selector,
			Prompt: prompt, Tools: true, MaxTurnsCap: delegateMaxTurnsCap,
			DurableSlot:   seatDurableSlot(run, panelRound, seat.Ordinal),
			ArtifactStage: artifactStage, ArtifactHash: artifactHash}
	}
	delegated := delegates.Group(ctx, run, requests)
	outcomes := make([]seatOutcome, len(seats))
	repairIndexes := make([]int, 0, len(seats))
	for i, call := range delegated {
		parsed, err := parsePanelResponse(call.Response, call.Err)
		seat := seats[i]
		seat.Participant = call.Participant
		outcomes[i] = seatOutcome{seat: seat, result: parsed, raw: call.Response, cost: call.CostUSD,
			costUnknown: call.CostUnknown, err: err, category: call.FailureCategory,
			detail: call.FailureDetail, replayLost: call.ReplayLost}
		// A verdict that contradicts its own findings carries no more reviewable
		// signal than unparseable text, so it earns the same one repair attempt.
		// Without this it was charged against the artifact having never been retried.
		if call.Err == nil && strings.TrimSpace(call.Participant) != "" && (err != nil || panelVerdictError(parsed) != nil) {
			repairIndexes = append(repairIndexes, i)
		}
	}
	if len(repairIndexes) > 0 && run.CostLimitUSD > 0 {
		var spent float64
		for _, outcome := range outcomes {
			spent += outcome.cost
		}
		run.CostLimitUSD = remainingCostLimit(run.CostLimitUSD, spent)
		if run.CostLimitUSD <= 0 {
			repairIndexes = nil
		}
	}
	if len(repairIndexes) > 0 {
		repairs := make([]SeatRequest, len(repairIndexes))
		for i, outcomeIndex := range repairIndexes {
			seat := outcomes[outcomeIndex].seat
			repairs[i] = SeatRequest{
				Role:        delegateRole,
				Persona:     seat.Persona,
				Participant: seat.Participant,
				Prompt:      panelResponseRepairPrompt(run.ID, artifactHash, artifactStage, outcomes[outcomeIndex].raw),
				// Preserve the review delegate's tool-capable transport: a CLI-backed
				// agent has no HTTP request URL, and tools:false would send its
				// continuation down the simple path instead.
				Tools:         true,
				MaxTurnsCap:   delegateMaxTurnsCap,
				DurableSlot:   seatDurableSlot(run, panelRound, seat.Ordinal) + ":repair:1",
				ArtifactStage: artifactStage,
				ArtifactHash:  artifactHash,
			}
		}
		for i, call := range delegates.Group(ctx, run, repairs) {
			outcomeIndex := repairIndexes[i]
			parsed, err := parsePanelResponse(call.Response, call.Err)
			outcomes[outcomeIndex].cost += call.CostUSD
			outcomes[outcomeIndex].costUnknown = outcomes[outcomeIndex].costUnknown || call.CostUnknown
			outcomes[outcomeIndex].result = parsed
			outcomes[outcomeIndex].err = err
			outcomes[outcomeIndex].category = call.FailureCategory
			outcomes[outcomeIndex].detail = call.FailureDetail
			outcomes[outcomeIndex].replayLost = call.ReplayLost
		}
	}

	feedback := ReviewFeedback{SchemaVersion: 1, ArtifactHash: artifactHash}
	reports := make([]SeatReport, 0, len(seats))
	approvals, voters := 0, len(seats)
	var cost float64
	costUnknown := false
	var seatFailures []string
	failures := make([]ParticipantFailure, 0, len(seats))
	replayLost := false
	for _, o := range outcomes {
		cost += o.cost
		costUnknown = costUnknown || o.costUnknown
		drop := func(category, detail string) {
			seatFailures = append(seatFailures, o.seat.Persona+": "+category+": "+detail)
			failures = append(failures, ParticipantFailure{Seat: o.seat.Ordinal + 1,
				Persona: o.seat.Persona, Category: category, Detail: detail})
			voters--
		}
		if o.err != nil {
			if o.replayLost {
				replayLost = true
			}
			// The transport names and redacts its own failures; only fall back to
			// the raw error when it declined to classify one.
			drop(firstNonempty(o.category, "malformed_after_repair"), firstNonempty(o.detail, o.err.Error()))
			continue
		}
		if o.result.RunID != run.ID || o.result.ArtifactHash != artifactHash {
			drop("identity_mismatch", "roundtable response identity mismatch")
			continue
		}
		echoStage, echoOK := normalizeRoundtableStage(o.result.ArtifactStage)
		if !echoOK || echoStage != artifactStage {
			failures = append(failures, ParticipantFailure{Seat: o.seat.Ordinal + 1, Persona: o.seat.Persona,
				Category: "artifact_stage_mismatch", Detail: "reviewer did not evaluate artifact stage " + artifactStage})
			feedback.Findings = append(feedback.Findings, Finding{ID: o.seat.Persona + "-artifact-stage",
				Persona: o.seat.Persona, Severity: "blocking",
				Summary:        "reviewer did not evaluate the declared artifact stage",
				Recommendation: "review the artifact at stage " + artifactStage + " and echo that exact artifact_stage"})
			// The response is unusable for quorum just like an identity mismatch.
			// Keep the blocking finding as the fail-closed anti-injection signal,
			// but do not also count a failed participant as a voter.
			voters--
			continue
		}
		// The stage echo is checked above and supersedes this: a seat that reviewed
		// the wrong stage is a blocking anti-injection failure, never an abstention.
		// Past that, a verdict still unusable after its repair attempt is absence of
		// evidence, not evidence of a defect, so the seat abstains exactly like an
		// unreachable one and min_successful decides.
		if verdictErr := panelVerdictError(o.result); verdictErr != nil {
			drop("malformed_after_repair", verdictErr.Error())
			continue
		}
		reports = append(reports, SeatReport{Seat: o.seat, Response: o.result})
		alignment := strings.ToLower(strings.TrimSpace(o.result.OriginalRequestAlignment.Status))
		alignmentOK := alignment == "aligned"
		if !alignmentOK {
			if alignment != "drifted" && alignment != "unclear" {
				alignment = "unclear"
			}
			summary := strings.TrimSpace(o.result.OriginalRequestAlignment.Summary)
			if summary == "" {
				summary = "reviewer did not establish that the direction follows the original request"
			}
			feedback.Findings = append(feedback.Findings, Finding{
				ID:             o.seat.Persona + "-original-request-alignment",
				Persona:        o.seat.Persona,
				Severity:       "blocking",
				Summary:        "original-request alignment is " + alignment + ": " + summary,
				Recommendation: "revise the direction so it directly serves the original request, then rerun the panel",
			})
		}
		// An enumerated requirement the artifact does NOT address is blocking,
		// whatever verdict the seat attached. This is the "nope, that is still a
		// defect" step: a reviewer that writes down the unaddressed item cannot
		// also approve past it, and a seat that returns approve alongside an
		// unaddressed requirement has contradicted itself rather than judged.
		//
		// The finding is raised here rather than trusted to the seat because the
		// failure being fixed is precisely a seat that saw the whole request and
		// still said "no findings": on am_312e901904 the ticket named two defects,
		// the artifact fixed one, and the review approved it.
		coverageGap := false
		for _, req := range o.result.Coverage {
			if req.Addressed {
				continue
			}
			coverageGap = true
			summary := strings.TrimSpace(req.Requirement)
			if summary == "" {
				summary = "a requirement of the original request"
			}
			detail := strings.TrimSpace(req.Evidence)
			if detail == "" {
				detail = "the artifact does not address it"
			}
			feedback.Findings = append(feedback.Findings, Finding{
				ID:             o.seat.Persona + "-requirement-unaddressed",
				Persona:        o.seat.Persona,
				Severity:       "blocking",
				Summary:        "original request not fully addressed: " + summary,
				Recommendation: "address it in the artifact, or record it explicitly as out of scope: " + detail,
			})
		}
		defaultSeverity := "blocking"
		if panelVerdict(o.result) == "approve" {
			// The alignment finding above already prevents advancement; also exclude
			// this vote from quorum so the fail-closed invariant is local and explicit.
			if alignmentOK && !coverageGap {
				approvals++
			}
			// An approval may carry non-blocking deficiencies. They are debt to
			// record, not grounds to hold the artifact, so they must still reach
			// the feedback or approving would silently discard them.
			defaultSeverity = "suggestion"
		}
		for i, f := range o.result.Findings {
			feedback.Findings = append(feedback.Findings, Finding{
				ID:             firstNonempty(f.ID, fmt.Sprintf("%s-%d", o.seat.Persona, i+1)),
				Persona:        o.seat.Persona,
				Severity:       firstNonempty(f.Severity, defaultSeverity),
				Location:       f.Location,
				Summary:        f.Summary,
				Recommendation: f.Recommendation})
		}
	}
	return Analysis{Feedback: feedback, Approvals: approvals, Voters: voters, CostUSD: cost,
		CostUnknown: costUnknown, Unreachable: strings.Join(seatFailures, "; "),
		Reports: reports, Failures: failures, ReplayLost: replayLost}
}
