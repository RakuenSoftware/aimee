package roundtable

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"

	"github.com/JBailes/aimee/server-go/delegate"
	"github.com/JBailes/aimee/server-go/modules/roundtable/panel"
)

func testPresets(t *testing.T, seats int) *panel.Store {
	t.Helper()
	dir := t.TempDir()
	type seat struct {
		Model   string `json:"model"`
		Persona string `json:"persona"`
	}
	saved := struct {
		Name          string `json:"name"`
		Seats         []seat `json:"seats"`
		MinSuccessful int    `json:"min_successful"`
	}{Name: "default", MinSuccessful: seats}
	for i := 0; i < seats; i++ {
		saved.Seats = append(saved.Seats, seat{Persona: "reviewer"})
	}
	body, err := json.Marshal(saved)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "default.json"), body, 0o600); err != nil {
		t.Fatal(err)
	}
	store, err := panel.NewStore(dir)
	if err != nil {
		t.Fatal(err)
	}
	return store
}

func testReviewer(t *testing.T, seats int) *PanelReviewer {
	t.Helper()
	reviewer, err := NewPanelReviewer(testPresets(t, seats), &scriptedSeats{})
	if err != nil {
		t.Fatal(err)
	}
	return reviewer
}

func TestReviewerRequiresItsCollaborators(t *testing.T) {
	if _, err := NewPanelReviewer(nil, nil); err == nil {
		t.Fatal("a reviewer with no preset store and no plane was accepted")
	}
}

// An artifact too short or too large to review is rejected before any agent is
// paid to look at it.
func TestReviewerRejectsUnreviewableArtifacts(t *testing.T) {
	reviewer := testReviewer(t, 1)
	for _, tc := range []struct {
		name     string
		artifact string
	}{
		{name: "too short", artifact: "too short"},
		{name: "empty", artifact: ""},
		{name: "oversize", artifact: strings.Repeat("x", panel.MaxArtifactBytes+1)},
	} {
		t.Run(tc.name, func(t *testing.T) {
			_, err := reviewer.Review(t.Context(), panel.ReviewRequest{
				Artifact: tc.artifact, Roundtable: "default"})
			var validation panel.ValidationError
			if err == nil || !asValidation(err, &validation) {
				t.Fatalf("err = %v, want a validation error", err)
			}
		})
	}
}

// Resolution fails closed: convening a panel the operator never configured is
// worse than not reviewing, because the unconfigured shape is invisible in the
// result. It is a park with a reason, not a verdict about the artifact.
func TestReviewerParksRatherThanInventingAPanel(t *testing.T) {
	reviewer := testReviewer(t, 1)
	for _, name := range []string{"", "not-a-saved-roundtable"} {
		result, err := reviewer.Review(t.Context(), panel.ReviewRequest{
			Artifact: "a diff long enough to be reviewable", Roundtable: name})
		if err != nil {
			t.Fatalf("unresolvable panel returned an error rather than a park: %v", err)
		}
		if result.Status != panel.StatusPending || result.PauseReason != "panel_unreachable" {
			t.Fatalf("result = %+v, want a panel_unreachable park", result)
		}
		if result.Approved {
			t.Fatal("an unresolvable panel reported an approval")
		}
		if result.Detail == "" {
			t.Fatal("park gave no reason for a human to act on")
		}
		// No seat may be convened. Substituting some other saved panel would also
		// end in a panel_unreachable park once its seats failed, so the park alone
		// does not prove the request was refused -- having convened nobody, and
		// spent nothing, does.
		if result.ParticipantsTotal != 0 || result.CostUSD != 0 {
			t.Fatalf("a panel was convened for an unresolvable request: %+v", result)
		}
		if name != "" && !strings.Contains(result.Detail, name) {
			t.Fatalf("park detail %q does not name the requested roundtable %q", result.Detail, name)
		}
	}
}

// A caller's run id is the review's identity and must survive; without one the
// identity is derived from the request and artifact so it is reproducible.
func TestReviewerPreservesCallerRunIdentity(t *testing.T) {
	reviewer := testReviewer(t, 1)
	const artifact = "a diff long enough to be reviewable"
	result, err := reviewer.Review(t.Context(), panel.ReviewRequest{
		Artifact: artifact, RunID: "caller-run", Roundtable: "missing"})
	if err != nil {
		t.Fatal(err)
	}
	if result.RunID != "caller-run" {
		t.Fatalf("run id = %q, want the caller's", result.RunID)
	}

	first, err := reviewer.Review(t.Context(), panel.ReviewRequest{Artifact: artifact, Roundtable: "missing"})
	if err != nil {
		t.Fatal(err)
	}
	second, err := reviewer.Review(t.Context(), panel.ReviewRequest{Artifact: artifact, Roundtable: "missing"})
	if err != nil {
		t.Fatal(err)
	}
	if first.RunID == "" || first.RunID != second.RunID {
		t.Fatalf("derived run id is not reproducible: %q vs %q", first.RunID, second.RunID)
	}
}

// The transport owns its failure taxonomy, so the panel is handed a category it
// never has to parse an error to obtain. An unclassified failure would be
// reported as a malformed reviewer rather than an unreachable plane.
func TestSeatFailureCategoriesNameTheTransportCause(t *testing.T) {
	for _, tc := range []struct {
		err  error
		want string
	}{
		{err: delegate.ErrDelegateReplayUnavailable, want: "replay_unavailable"},
		{err: delegate.ErrDelegateCostLimitUnsupported, want: "cost_limit_unsupported"},
		{err: delegate.ErrDelegateUnassignedExpired, want: "unassigned_expired"},
		{err: delegate.ErrDelegateTerminal, want: "delegate_terminal"},
		{err: context.DeadlineExceeded, want: "deadline"},
	} {
		if got := seatFailureCategory(tc.err); got != tc.want {
			t.Fatalf("seatFailureCategory(%v) = %q, want %q", tc.err, got, tc.want)
		}
	}
	result := seatResult("", "", 0, false, delegate.ErrDelegateReplayUnavailable)
	if !result.ReplayLost {
		t.Fatal("a lost replay was not marked, so the caller would park instead of recovering")
	}
	if result.FailureDetail == "" {
		t.Fatal("seat failure reached the panel with no detail to report")
	}
}

func TestSeatFailureDetailIsRedacted(t *testing.T) {
	result := seatResult("", "", 0, false,
		errorString("launch failed: authorization: bearer sk-live-secret-value"))
	if strings.Contains(result.FailureDetail, "sk-live-secret-value") {
		t.Fatalf("credential survived into a durable seat failure: %q", result.FailureDetail)
	}
}

type errorString string

func (e errorString) Error() string { return string(e) }

func asValidation(err error, target *panel.ValidationError) bool {
	validation, ok := err.(panel.ValidationError)
	if ok {
		*target = validation
	}
	return ok
}

// scriptedSeats answers every seat with one response, echoing the run and
// artifact identity unless the test deliberately supplies its own.
type scriptedSeats struct {
	mu       sync.Mutex
	requests []panel.SeatRequest
	response string
}

func (s *scriptedSeats) One(_ context.Context, run panel.Run, request panel.SeatRequest) panel.SeatResult {
	s.mu.Lock()
	s.requests = append(s.requests, request)
	s.mu.Unlock()
	response := s.response
	if response == "" {
		response = fmt.Sprintf(
			`{"run_id":%q,"artifact_hash":%q,"artifact_stage":%q,`+
				`"original_request_alignment":{"status":"aligned","summary":"implements the request"},`+
				`"verdict":"approve","findings":[]}`,
			run.ID, request.ArtifactHash, request.ArtifactStage)
	}
	return panel.SeatResult{Response: response, Participant: "opaque-seat-token"}
}

func (s *scriptedSeats) Group(ctx context.Context, run panel.Run, requests []panel.SeatRequest) []panel.SeatResult {
	out := make([]panel.SeatResult, len(requests))
	for i := range requests {
		out[i] = s.One(ctx, run, requests[i])
	}
	return out
}

func reviewerWithSeats(t *testing.T, seats panel.Delegates) *PanelReviewer {
	t.Helper()
	reviewer, err := NewPanelReviewer(testPresets(t, 1), seats)
	if err != nil {
		t.Fatal(err)
	}
	return reviewer
}

// The verdict has to be tied to the exact bytes reviewed, and to this run. A
// result that cannot be traced back to them is not evidence the artifact was
// reviewed at all.
func TestReviewBindsTheVerdictToTheRunAndArtifact(t *testing.T) {
	seats := &scriptedSeats{}
	artifact := "\n" + strings.Repeat("diff --git a/a b/a\n", 4) + "DIRECT_ARTIFACT_MARKER\n\n"
	result, err := reviewerWithSeats(t, seats).Review(t.Context(), panel.ReviewRequest{
		Artifact: artifact, OriginalRequest: "Review only the supplied direct artifact.",
		ArtifactStage: "frozen_diff", RunID: "review-pr-1828-attempt-2", Roundtable: "default",
	})
	if err != nil {
		t.Fatal(err)
	}
	wantHash := panel.Hash([]byte(artifact))
	if result.RunID != "review-pr-1828-attempt-2" || result.ArtifactHash != wantHash ||
		result.Feedback == nil || result.Feedback.ArtifactHash != wantHash {
		t.Fatalf("result identity=%+v want run and artifact %s", result, wantHash)
	}
	seats.mu.Lock()
	defer seats.mu.Unlock()
	if len(seats.requests) == 0 {
		t.Fatal("no seat was convened")
	}
	for _, request := range seats.requests {
		if !strings.Contains(request.Prompt, "DIRECT_ARTIFACT_MARKER") {
			t.Fatalf("seat received another run's artifact: %+v", request)
		}
	}
}

// A seat that echoes some other run or artifact is reporting on work this
// review did not ask about, so it cannot count toward the verdict.
func TestReviewRejectsASeatThatEchoesAnotherIdentity(t *testing.T) {
	seats := &scriptedSeats{response: `{"run_id":"another-run","artifact_hash":"stale-hash",` +
		`"artifact_stage":"frozen_diff","original_request_alignment":{"status":"aligned","summary":"looks right"},` +
		`"verdict":"approve","findings":[]}`}
	result, err := reviewerWithSeats(t, seats).Review(t.Context(), panel.ReviewRequest{
		Artifact: strings.Repeat("diff --git a/a b/a\n", 4), RunID: "review-current", Roundtable: "default",
	})
	if err != nil {
		t.Fatalf("stale identity should park, not error: %v", err)
	}
	if result.Approved || result.ParticipantsUsed != 0 || !result.Degraded {
		t.Fatalf("stale panel response was accepted: %+v", result)
	}
}

// A retry must replay the seats it already paid for, and that depends entirely
// on the run identity reaching the seat requests. If Stage or ExecutionVersion
// are dropped on the wire, every attempt keys a different durable slot and
// convenes -- and bills -- a fresh panel.
func TestReviewCarriesRunIdentityOntoEverySeat(t *testing.T) {
	seats := &scriptedSeats{}
	request := panel.ReviewRequest{
		Artifact: strings.Repeat("diff --git a/a b/a\n", 4), OriginalRequest: "review it",
		ArtifactStage: "frozen_diff", Roundtable: "default", RunID: "wi-7",
		Stage: "gate", ExecutionVersion: "v3", CostLimitUSD: 4,
	}
	if _, err := reviewerWithSeats(t, seats).Review(t.Context(), request); err != nil {
		t.Fatal(err)
	}
	seats.mu.Lock()
	defer seats.mu.Unlock()
	if len(seats.requests) == 0 {
		t.Fatal("no seat was convened")
	}
	first := seats.requests[0]
	if first.DurableSlot == "" {
		t.Fatal("seat carries no durable slot, so a retry cannot replay it")
	}
	// The slot hashes the run identity, so the same review keys the same slot
	// and a different attempt keys a different one.
	other := request
	other.ExecutionVersion = "v4"
	otherSeats := &scriptedSeats{}
	if _, err := reviewerWithSeats(t, otherSeats).Review(t.Context(), other); err != nil {
		t.Fatal(err)
	}
	otherSeats.mu.Lock()
	defer otherSeats.mu.Unlock()
	if otherSeats.requests[0].DurableSlot != first.DurableSlot {
		t.Fatal("execution version leaked into the durable slot; a replay would never match")
	}
	// The per-seat ceiling is the plane adapter's to divide, not the panel's, so
	// it is asserted where that division happens rather than through a double
	// that does not implement it.
}

// ReplayOnly is what stops a lifecycle retry from paying twice for spend that
// was already reconciled. It has to reach the seat, not stop at the boundary.
func TestReviewPropagatesReplayOnly(t *testing.T) {
	seats := &recordingRun{}
	_, err := reviewerWithSeats(t, seats).Review(t.Context(), panel.ReviewRequest{
		Artifact: strings.Repeat("diff --git a/a b/a\n", 4), Roundtable: "default",
		RunID: "wi-8", Stage: "gate", ExecutionVersion: "v1", ReplayOnly: true,
	})
	if err != nil {
		t.Fatal(err)
	}
	if !seats.seen.ReplayOnly {
		t.Fatal("replay-only was dropped; a reconciled retry would launch and bill again")
	}
	if seats.seen.Stage != "gate" || seats.seen.ExecutionVersion != "v1" {
		t.Fatalf("run identity did not reach the plane: %+v", seats.seen)
	}
	if seats.seen.CostLimitUSD != 0 {
		t.Fatalf("unset reservation became %v", seats.seen.CostLimitUSD)
	}
}

type recordingRun struct {
	mu   sync.Mutex
	seen panel.Run
}

func (r *recordingRun) One(_ context.Context, run panel.Run, request panel.SeatRequest) panel.SeatResult {
	r.mu.Lock()
	r.seen = run
	r.mu.Unlock()
	return panel.SeatResult{Response: fmt.Sprintf(
		`{"run_id":%q,"artifact_hash":%q,"artifact_stage":%q,`+
			`"original_request_alignment":{"status":"aligned","summary":"ok"},`+
			`"verdict":"approve","findings":[]}`, run.ID, request.ArtifactHash, request.ArtifactStage)}
}

func (r *recordingRun) Group(ctx context.Context, run panel.Run, requests []panel.SeatRequest) []panel.SeatResult {
	out := make([]panel.SeatResult, len(requests))
	for i := range requests {
		out[i] = r.One(ctx, run, requests[i])
	}
	return out
}
