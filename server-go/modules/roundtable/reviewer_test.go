package roundtable

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/modules/delegates/plane"
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
	// A client pointed at a loopback address it will never reach: these cases
	// are decided before any seat is convened.
	client, err := plane.NewHTTPAgentClient(plane.AgentHTTPConfig{BaseURL: "http://127.0.0.1:1"})
	if err != nil {
		t.Fatal(err)
	}
	reviewer, err := NewPanelReviewer(testPresets(t, seats), client)
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
		{err: plane.ErrDelegateReplayUnavailable, want: "replay_unavailable"},
		{err: plane.ErrDelegateUnassignedExpired, want: "unassigned_expired"},
		{err: plane.ErrDelegateTerminal, want: "delegate_terminal"},
		{err: context.DeadlineExceeded, want: "deadline"},
	} {
		if got := seatFailureCategory(tc.err); got != tc.want {
			t.Fatalf("seatFailureCategory(%v) = %q, want %q", tc.err, got, tc.want)
		}
	}
	result := seatResult("", "", 0, false, plane.ErrDelegateReplayUnavailable)
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
