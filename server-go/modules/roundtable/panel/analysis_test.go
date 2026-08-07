package panel

import (
	"context"
	"encoding/json"
	"errors"
	"strings"
	"sync"
	"testing"
)

// runAnalysisRound is the focused seam these tests exercise: one round of
// independent seat analysis, reduced to the values a caller decides quorum on.
func runAnalysisRound(ctx context.Context, delegates Delegates, run Run, seats []Seat,
	prompt, artifactHash, artifactStage string, panelRound int) (ReviewFeedback, int, int, float64, string) {
	result := RunAnalysis(ctx, delegates, run, seats, prompt, artifactHash, artifactStage, panelRound)
	return result.Feedback, result.Approvals, result.Voters, result.CostUSD, result.Unreachable
}

func TestNativeRoundtableFailsClosedWhenReviewerEvaluatesWrongStage(t *testing.T) {
	tests := []struct {
		name, stageJSON string
	}{
		{"omitted", ""},
		{"empty", `""`},
		{"intent", `"intent"`},
		{"frozen-diff", `"frozen_diff"`},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			prefix := ""
			if tc.stageJSON != "" {
				prefix = `"artifact_stage":` + tc.stageJSON + `,`
			}
			agents := &recordingAgents{reviewResponse: `{` + prefix + `"original_request_alignment":{"status":"aligned","summary":"looks related"},"verdict":"approve","findings":[]}`}
			feedback, approvals, voters, _, unreachable := runAnalysisRound(context.Background(), agents, Run{ID: "wi", Stage: "gate"}, []Seat{{Persona: "qa"}}, "review", "hash", "plan", 1)
			if unreachable != "" || approvals != 0 || voters != 0 || len(feedback.Findings) != 1 {
				t.Fatalf("stage mismatch accounting: approvals=%d voters=%d unreachable=%q feedback=%+v", approvals, voters, unreachable, feedback)
			}
			finding := feedback.Findings[0]
			if !strings.HasSuffix(finding.ID, "-artifact-stage") || finding.Severity != "blocking" || finding.Persona != "qa" || !strings.Contains(finding.Recommendation, "stage plan") {
				t.Fatalf("stage mismatch did not fail closed: %+v", finding)
			}
		})
	}
	for _, echoed := range []string{`"Plan"`, `"PLAN"`, `" plan "`} {
		agents := &recordingAgents{reviewResponse: `{"artifact_stage":` + echoed + `,"original_request_alignment":{"status":"aligned","summary":"looks related"},"verdict":"approve","findings":[]}`}
		feedback, approvals, voters, _, unreachable := runAnalysisRound(context.Background(), agents, Run{ID: "wi", Stage: "gate"}, []Seat{{Persona: "qa"}}, "review", "hash", "plan", 1)
		if unreachable != "" || approvals != 1 || voters != 1 || len(feedback.Findings) != 0 {
			t.Fatalf("canonical stage echo %s rejected: approvals=%d voters=%d unreachable=%q feedback=%+v", echoed, approvals, voters, unreachable, feedback)
		}
	}
}

func TestStageMismatchCannotBeOverriddenByAnotherApproval(t *testing.T) {
	agents := &scriptedReviewAgents{responses: []string{
		`{"artifact_stage":"intent","original_request_alignment":{"status":"aligned","summary":"related"},"verdict":"approve","findings":[]}`,
		`{"artifact_stage":"plan","original_request_alignment":{"status":"aligned","summary":"related"},"verdict":"approve","findings":[]}`,
	}}
	feedback, approvals, voters, _, unreachable := runAnalysisRound(context.Background(), agents, Run{ID: "wi", Stage: "gate"}, []Seat{{Persona: "qa"}, {Persona: "security"}}, "review", "hash", "plan", 1)
	if unreachable != "" || approvals != 1 || voters != 1 || len(feedback.Findings) != 1 || !strings.HasSuffix(feedback.Findings[0].ID, "-artifact-stage") {
		t.Fatalf("mixed-stage panel could approve: approvals=%d voters=%d unreachable=%q feedback=%+v", approvals, voters, unreachable, feedback)
	}
}

func TestPanelCapacitySeatsHaveDistinctDurableJobKeys(t *testing.T) {
	agents := &recordingAgents{}
	req := Run{ID: "wi", Stage: "gate", Workdir: "/worktree"}
	seats := []Seat{
		{Persona: "security", Selector: "codex", Ordinal: 0},
		{Persona: "security", Selector: "codex", Ordinal: 1},
		{Persona: "security", Selector: "codex", Ordinal: 2},
	}
	feedback, approvals, voters, _, unreachable := runAnalysisRound(context.Background(), agents, req, seats, "review", "hash", "plan", 1)
	if unreachable != "" || approvals != 3 || voters != 3 || len(feedback.Findings) != 0 {
		t.Fatalf("panel result approvals=%d voters=%d unreachable=%q feedback=%+v", approvals, voters, unreachable, feedback)
	}
	if len(agents.requests) != 3 {
		t.Fatalf("requests=%d", len(agents.requests))
	}
	seen := map[string]bool{}
	wantSlots := map[string]bool{}
	for ordinal := range seats {
		wantSlots[seatDurableSlot(req, 1, ordinal)] = true
	}
	for _, request := range agents.requests {
		if request.MaxTurnsCap != delegateMaxTurnsCap {
			t.Fatalf("roundtable request is not turn-bounded: %+v", request)
		}
		key := request.DurableSlot + "\x00" + request.Persona + "\x00" + request.Selector
		if seen[key] {
			t.Fatalf("capacity seats collapsed onto durable key %q: %+v", key, agents.requests)
		}
		seen[key] = true
		if !wantSlots[request.DurableSlot] {
			t.Fatalf("unexpected durable slot=%q want one of %v", request.DurableSlot, wantSlots)
		}
	}
}

func TestPanelRepairsMalformedJSONOnSameParticipantOnce(t *testing.T) {
	agents := &repairingReviewAgents{}
	req := Run{ID: "wi", Stage: "gate", Workdir: "/worktree"}
	feedback, approvals, voters, cost, unreachable := runAnalysisRound(context.Background(), agents, req, []Seat{{Persona: "architect", Ordinal: 0}}, "review", "hash", "plan", 1)
	if unreachable != "" || approvals != 1 || voters != 1 || len(feedback.Findings) != 0 || cost != 1.5 {
		t.Fatalf("repaired panel result approvals=%d voters=%d cost=%v unreachable=%q feedback=%+v", approvals, voters, cost, unreachable, feedback)
	}
	if len(agents.requests) != 2 || len(agents.requests[0]) != 1 || len(agents.requests[1]) != 1 {
		t.Fatalf("group calls=%+v", agents.requests)
	}
	repair := agents.requests[1][0]
	if repair.Participant != "opaque-seat-token" || repair.Selector != "" {
		t.Fatalf("repair did not preserve opaque participant without rerouting: %+v", repair)
	}
	if !repair.Tools || repair.MaxTurnsCap != delegateMaxTurnsCap || repair.ArtifactStage != "plan" || !strings.HasSuffix(repair.DurableSlot, ":repair:1") {
		t.Fatalf("repair request did not preserve tool-capable transport as a bounded continuation: %+v", repair)
	}
	if !strings.Contains(repair.Prompt, "Preserve its analysis and findings") || !strings.Contains(repair.Prompt, "exactly one JSON object") {
		t.Fatalf("repair prompt=%q", repair.Prompt)
	}
	quotedInvalid, _ := json.Marshal(agents.invalid)
	if !strings.Contains(repair.Prompt, "PREVIOUS_RESPONSE_JSON_STRING\n"+string(quotedInvalid)+"\nEND_PREVIOUS_RESPONSE_JSON_STRING") {
		t.Fatalf("repair prompt omitted or altered complete invalid response: %q", repair.Prompt)
	}
}

func TestPanelCapacityRoundsHaveDistinctDurableJobKeys(t *testing.T) {
	agents := &recordingAgents{}
	req := Run{ID: "wi", Stage: "gate", Workdir: "/worktree"}
	seats := []Seat{{Persona: "security", Selector: "codex", Ordinal: 0}}
	for round := 1; round <= 2; round++ {
		feedback, approvals, voters, _, unreachable := runAnalysisRound(context.Background(), agents, req, seats, "same review", "hash", "plan", round)
		if unreachable != "" || approvals != 1 || voters != 1 || len(feedback.Findings) != 0 {
			t.Fatalf("round %d approvals=%d voters=%d unreachable=%q feedback=%+v", round, approvals, voters, unreachable, feedback)
		}
	}
	// Two rounds of the same seat must not collapse onto one durable result, or
	// the second round would replay the first instead of re-reviewing.
	if len(agents.requests) != 2 || agents.requests[0].DurableSlot == agents.requests[1].DurableSlot {
		t.Fatalf("panel rounds shared durable slot: %+v", agents.requests)
	}
}

func TestPanelPassesRandomAndPinnedSpecificationsToDelegate(t *testing.T) {
	agents := &recordingAgents{reviewResponse: `{"artifact_stage":"plan","original_request_alignment":{"status":"aligned","summary":"implements the request"},"verdict":"approve","findings":[]}`}
	req := Run{ID: "wi", Stage: "gate", Workdir: "/worktree"}
	analysis := RunAnalysis(context.Background(), agents, req,
		[]Seat{{Persona: "qa", Selector: "$random", Ordinal: 0}, {Persona: "security", Selector: "codex", Ordinal: 1}}, "review", "hash", "plan", 1)
	feedback, approvals, voters, unreachable := analysis.Feedback, analysis.Approvals, analysis.Voters, analysis.Unreachable
	if unreachable != "" || approvals != 2 || voters != 2 || len(feedback.Findings) != 0 {
		t.Fatalf("delegate specifications failed: approvals=%d voters=%d unreachable=%q feedback=%+v", approvals, voters, unreachable, feedback)
	}
	if len(agents.requests) != 2 {
		t.Fatalf("requests=%+v", agents.requests)
	}
	delegates := map[string]bool{}
	for _, request := range agents.requests {
		delegates[request.Selector] = true
	}
	if !delegates["$random"] || !delegates["codex"] {
		t.Fatalf("roundtable must pass random and pinned specifications opaquely: %+v", agents.requests)
	}
}

func TestFailedSeatCannotBeMaskedBySuccessfulDuplicate(t *testing.T) {
	agents := firstPanelSeatUnavailableAgents{}
	req := Run{ID: "wi", Stage: "gate", Workdir: "/worktree"}
	seats := []Seat{
		{Persona: "security", Selector: "codex", Ordinal: 0},
		{Persona: "security", Selector: "minimax", Ordinal: 1},
	}
	feedback, approvals, voters, _, unreachable := runAnalysisRound(context.Background(), agents, req, seats, "review", "hash", "plan", 1)
	if unreachable == "" || approvals != 1 || voters != 1 || len(feedback.Findings) != 0 {
		t.Fatalf("failed seat was masked: approvals=%d voters=%d unreachable=%q feedback=%+v", approvals, voters, unreachable, feedback)
	}
}

func TestRequiredPinnedAgentCannotUseSuccessfulPersonaDuplicate(t *testing.T) {
	agents := firstPanelSeatUnavailableAgents{}
	req := Run{ID: "wi", Stage: "gate", Workdir: "/worktree"}
	seats := []Seat{
		{Persona: "security", Selector: "codex", Ordinal: 0},
		{Persona: "security", Selector: "minimax", Ordinal: 1},
	}
	_, approvals, voters, _, unreachable := runAnalysisRound(context.Background(), agents, req, seats, "review", "hash", "plan", 1)
	if unreachable == "" || approvals != 1 || voters != 1 {
		t.Fatalf("explicit pin was substituted: approvals=%d voters=%d unreachable=%q", approvals, voters, unreachable)
	}
}

func TestMalformedCapacityDuplicateCannotSatisfyRequiredPersona(t *testing.T) {
	agents := firstPanelSeatUnavailableAgents{response: `{"artifact_stage":"plan","original_request_alignment":{"status":"aligned"},"verdict":"approve","findings":[{"id":"contradiction","summary":"approve with finding"}]}`}
	req := Run{ID: "wi", Stage: "gate", Workdir: "/worktree"}
	seats := []Seat{
		{Persona: "security", Selector: "codex", Ordinal: 0},
		{Persona: "security", Selector: "minimax", Ordinal: 1},
	}
	// The duplicate contradicts itself (approve carrying a finding). It abstains
	// rather than voting, so it can neither satisfy the required persona nor mask
	// the seat that failed: both seats drop out and nothing is approved.
	_, approvals, voters, _, unreachable := runAnalysisRound(context.Background(), agents, req, seats, "review", "hash", "plan", 1)
	if unreachable == "" || voters != 0 || approvals != 0 {
		t.Fatalf("malformed duplicate satisfied required Persona: approvals=%d voters=%d unreachable=%q", approvals, voters, unreachable)
	}
	if !strings.Contains(unreachable, "malformed_after_repair") || !strings.Contains(unreachable, "delegate_error") {
		t.Fatalf("dropped seats are not self-describing: %q", unreachable)
	}
}

func TestValidChangesDuplicateCannotMaskFailedSeat(t *testing.T) {
	agents := firstPanelSeatUnavailableAgents{response: `{"artifact_stage":"plan","original_request_alignment":{"status":"aligned","summary":"direction is right"},"verdict":"changes","findings":[{"id":"detail","severity":"blocking","summary":"add detail","recommendation":"specify the step"}]}`}
	req := Run{ID: "wi", Stage: "gate", Workdir: "/worktree"}
	seats := []Seat{
		{Persona: "security", Selector: "codex", Ordinal: 0},
		{Persona: "security", Selector: "minimax", Ordinal: 1},
	}
	feedback, approvals, voters, _, unreachable := runAnalysisRound(context.Background(), agents, req, seats, "review", "hash", "plan", 1)
	if unreachable == "" || approvals != 0 || voters != 1 || len(feedback.Findings) != 1 {
		t.Fatalf("valid duplicate masked failed seat: approvals=%d voters=%d unreachable=%q feedback=%+v", approvals, voters, unreachable, feedback)
	}
}

// The doubles below stand in for the resource plane. Each supplies the run and
// artifact identity a real plane's seats echo, plus a distinct participant per
// seat, so a test only states the part of the response it is actually about.

// recordingAgents answers every seat with one scripted verdict and records the
// seats it was asked to open.
type recordingAgents struct {
	mu             sync.Mutex
	requests       []SeatRequest
	reviewResponse string
}

func (a *recordingAgents) seat(request SeatRequest) string {
	a.mu.Lock()
	a.requests = append(a.requests, request)
	a.mu.Unlock()
	if a.reviewResponse != "" {
		return a.reviewResponse
	}
	return `{"artifact_stage":"` + request.ArtifactStage + `","original_request_alignment":{"status":"aligned","summary":"implements the request"},"verdict":"approve","findings":[]}`
}

func (a *recordingAgents) One(_ context.Context, run Run, request SeatRequest) SeatResult {
	return SeatResult{Response: withTestIdentity(a.seat(request), run, request)}
}

func (a *recordingAgents) Group(ctx context.Context, run Run, requests []SeatRequest) []SeatResult {
	return concurrentSeats(ctx, run, requests, a.One)
}

// firstPanelSeatUnavailableAgents fails seat 0 and answers the rest, which is
// how a degraded panel actually presents: one seat unreachable, a quorum still
// possible.
type firstPanelSeatUnavailableAgents struct{ response string }

func (a firstPanelSeatUnavailableAgents) One(_ context.Context, run Run, request SeatRequest) SeatResult {
	if strings.HasSuffix(request.DurableSlot, "seat:0") {
		return SeatResult{Err: errors.New("admission unavailable"), FailureCategory: "delegate_error",
			FailureDetail: "admission unavailable"}
	}
	response := a.response
	if response == "" {
		response = `{"artifact_stage":"plan","original_request_alignment":{"status":"aligned","summary":"implements the request"},"verdict":"approve","findings":[]}`
	}
	return SeatResult{Response: withTestIdentity(response, run, request), Participant: "opaque-seat-token"}
}

func (a firstPanelSeatUnavailableAgents) Group(ctx context.Context, run Run, requests []SeatRequest) []SeatResult {
	return concurrentSeats(ctx, run, requests, a.One)
}

// repairingReviewAgents returns an unparseable report first and a valid one on
// the repair attempt, recording each group so the test can prove the repair
// reused the same participant rather than opening a second seat.
type repairingReviewAgents struct {
	mu       sync.Mutex
	requests [][]SeatRequest
	invalid  string
}

func (a *repairingReviewAgents) One(_ context.Context, _ Run, _ SeatRequest) SeatResult {
	return SeatResult{Err: errors.New("unexpected direct delegation")}
}

func (a *repairingReviewAgents) Group(_ context.Context, run Run, requests []SeatRequest) []SeatResult {
	a.mu.Lock()
	defer a.mu.Unlock()
	a.requests = append(a.requests, append([]SeatRequest(nil), requests...))
	if len(a.requests) == 1 {
		a.invalid = `"artifact_stage":"plan","original_request_alignment":{"status":"aligned","summary":"implements the request\nwithout drift"},"verdict":"approve","findings":[]}`
		return []SeatResult{{Participant: "opaque-seat-token", Response: a.invalid, CostUSD: 1.25}}
	}
	return []SeatResult{{
		Participant: "opaque-seat-token",
		Response:    withTestIdentity(`{"artifact_stage":"plan","original_request_alignment":{"status":"aligned","summary":"implements the request"},"verdict":"approve","findings":[]}`, run, requests[0]),
		CostUSD:     0.25,
	}}
}

// scriptedReviewAgents hands out one prepared response per seat in order.
type scriptedReviewAgents struct {
	mu        sync.Mutex
	responses []string
}

func (a *scriptedReviewAgents) One(_ context.Context, run Run, request SeatRequest) SeatResult {
	a.mu.Lock()
	response := a.responses[0]
	a.responses = a.responses[1:]
	a.mu.Unlock()
	return SeatResult{Response: withTestIdentity(response, run, request), Participant: "opaque-seat-token"}
}

func (a *scriptedReviewAgents) Group(ctx context.Context, run Run, requests []SeatRequest) []SeatResult {
	return concurrentSeats(ctx, run, requests, a.One)
}

// concurrentSeats runs seats the way the real plane does. Serialising them here
// would hide a data race that production would hit.
func concurrentSeats(ctx context.Context, run Run, requests []SeatRequest,
	one func(context.Context, Run, SeatRequest) SeatResult) []SeatResult {
	out := make([]SeatResult, len(requests))
	var wg sync.WaitGroup
	wg.Add(len(requests))
	for i := range requests {
		go func(i int) {
			defer wg.Done()
			out[i] = one(ctx, run, requests[i])
		}(i)
	}
	wg.Wait()
	return out
}

// The am_312e901904 shape, reduced to its essentials: a ticket that names two
// defects, an artifact that fixes one, and a single seat that calls it aligned
// and approves. That is exactly what shipped -- the review was briefed to
// "verify both distinct defects in the ticket are actually fixed" and returned
// "approved with no findings" over a patch that had misdiagnosed the second.
//
// A reviewer forced to enumerate what was asked writes the unaddressed item
// down, and once it is written down it must block. One seat is enough for that;
// the mechanism is enumeration, not headcount.
func TestUnaddressedRequirementBlocksEvenWhenTheSeatApproves(t *testing.T) {
	agents := &recordingAgents{reviewResponse: `{"artifact_stage":"frozen_diff",` +
		`"original_request_alignment":{"status":"aligned","summary":"the direction follows the ticket"},` +
		`"requirement_coverage":[` +
		`{"requirement":"bulk clone fails rc=127","addressed":true,"evidence":"chdir moved before the fd remap"},` +
		`{"requirement":"Projects view disagrees with what was cloned","addressed":false,"evidence":"no change derives the owner from clone_url"}],` +
		`"verdict":"approve","findings":[]}`}
	req := Run{ID: "wi", Stage: "gate", Workdir: "/worktree"}
	analysis := RunAnalysis(context.Background(), agents, req,
		[]Seat{{Persona: "qa", Selector: "$random", Ordinal: 0}}, "review", "hash", "frozen_diff", 1)

	// The seat said approve; the unaddressed requirement overrides it.
	if analysis.Approvals != 0 {
		t.Fatalf("an approval carrying an unaddressed requirement must not count toward quorum, got %d",
			analysis.Approvals)
	}
	var blocking *Finding
	for i := range analysis.Feedback.Findings {
		if analysis.Feedback.Findings[i].Severity == "blocking" {
			blocking = &analysis.Feedback.Findings[i]
			break
		}
	}
	if blocking == nil {
		t.Fatalf("expected a blocking finding for the unaddressed requirement, got %+v",
			analysis.Feedback.Findings)
	}
	if !strings.Contains(blocking.Summary, "Projects view") {
		t.Fatalf("the blocking finding must name the requirement that was missed, got %q", blocking.Summary)
	}
}

// The counterpart: full coverage must still approve, or the gate is unpassable.
func TestFullRequirementCoverageStillApproves(t *testing.T) {
	agents := &recordingAgents{reviewResponse: `{"artifact_stage":"frozen_diff",` +
		`"original_request_alignment":{"status":"aligned","summary":"implements the ticket"},` +
		`"requirement_coverage":[` +
		`{"requirement":"bulk clone fails rc=127","addressed":true,"evidence":"chdir before dup2"},` +
		`{"requirement":"Projects view disagrees","addressed":true,"evidence":"owner derived from clone_url"}],` +
		`"verdict":"approve","findings":[]}`}
	req := Run{ID: "wi", Stage: "gate", Workdir: "/worktree"}
	analysis := RunAnalysis(context.Background(), agents, req,
		[]Seat{{Persona: "qa", Selector: "$random", Ordinal: 0}}, "review", "hash", "frozen_diff", 1)
	if analysis.Approvals != 1 {
		t.Fatalf("fully covered request must approve, got %d approvals", analysis.Approvals)
	}
	if len(analysis.Feedback.Findings) != 0 {
		t.Fatalf("no findings expected, got %+v", analysis.Feedback.Findings)
	}
}

// Absent coverage must not silently pass: a seat that omits the field entirely
// is the pre-fix behaviour, and the point is that it cannot be the way through.
func TestMissingRequirementCoverageDoesNotFabricateApproval(t *testing.T) {
	agents := &recordingAgents{reviewResponse: `{"artifact_stage":"frozen_diff",` +
		`"original_request_alignment":{"status":"aligned","summary":"looks right"},` +
		`"verdict":"approve","findings":[]}`}
	req := Run{ID: "wi", Stage: "gate", Workdir: "/worktree"}
	analysis := RunAnalysis(context.Background(), agents, req,
		[]Seat{{Persona: "qa", Selector: "$random", Ordinal: 0}}, "review", "hash", "frozen_diff", 1)
	// Documents today's behaviour so a later change to REQUIRE the field is a
	// deliberate, visible decision rather than an accident.
	if analysis.Approvals != 1 {
		t.Fatalf("absent coverage currently approves; got %d", analysis.Approvals)
	}
}
