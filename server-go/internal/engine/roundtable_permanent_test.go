package engine

import (
	"context"
	"errors"
	"fmt"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/internal/db1"
	"github.com/JBailes/aimee/server-go/internal/wfe"
	roundtablecfg "github.com/JBailes/aimee/server-go/modules/roundtable/panel"
)

type statusReviewer struct{ status bus.ModuleStatus }

func (r statusReviewer) Review(context.Context, roundtablecfg.ReviewRequest) (roundtablecfg.RunResult, error) {
	return roundtablecfg.RunResult{}, fmt.Errorf("roundtable review over the bus: %w",
		&bus.ModuleCallStatusError{Status: r.status})
}

// Parking exists for a runner that might come back. A request the panel refuses
// as invalid is refused identically every time, so the step has to fail rather
// than be resubmitted every few seconds until a human notices.
func TestRoundtableFailsTheStepWhenThePanelRefusesTheRequest(t *testing.T) {
	for _, tc := range []struct {
		name   string
		status bus.ModuleStatus
		failed bool
	}{
		{"invalid request is permanent", bus.ModuleStatusInvalidRequest, true},
		{"internal may be transient", bus.ModuleStatusInternal, false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			runner := &NativeRunner{agents: &recordingAgents{}}
			runner.reviews = statusReviewer{status: tc.status}
			reviewed := wfe.Artifact{Type: "plan",
				Content: []byte("content of the artifact under review, long enough to be reviewable")}
			result, err := runner.roundtable(context.Background(), StepRequest{
				WorkItem: db1.WorkItem{Repo: t.TempDir(), Worktree: t.TempDir()},
				Node: wfe.Node{Params: map[string]any{"roundtable": "default",
					"panel": map[string]any{"required": []any{"original-request", "reviewer"}}}},
				Proposal: "request",
				Inputs:   map[string]wfe.Artifact{"src": reviewed},
			})
			if tc.failed {
				if err != nil {
					t.Fatalf("err = %v, want the step to fail without an error to retry on", err)
				}
				if result.Status != StepFailed {
					t.Fatalf("status = %q, want %q", result.Status, StepFailed)
				}
				return
			}
			if err == nil {
				t.Fatalf("a possibly-transient failure returned no error, so the step would never retry")
			}
			if errors.Is(err, nil) {
				t.Fatal("unreachable")
			}
		})
	}
}
