package panel

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"sync"
	"testing"
)

type unavailableDelegates struct {
	category       string
	waitForContext bool
}

type categorizedDelegates []string

func (d categorizedDelegates) Group(_ context.Context, _ Run, requests []SeatRequest) []SeatResult {
	out := make([]SeatResult, len(requests))
	for i := range out {
		category := d[i]
		out[i] = SeatResult{Err: errors.New(category), FailureCategory: category, FailureDetail: category}
	}
	return out
}

func (d categorizedDelegates) One(ctx context.Context, run Run, request SeatRequest) SeatResult {
	return d.Group(ctx, run, []SeatRequest{request})[0]
}

func (d unavailableDelegates) Group(ctx context.Context, _ Run, requests []SeatRequest) []SeatResult {
	if d.waitForContext {
		<-ctx.Done()
	}
	out := make([]SeatResult, len(requests))
	for i := range out {
		out[i] = SeatResult{Err: errors.New(d.category), FailureCategory: d.category,
			FailureDetail: d.category + " while seating reviewer"}
	}
	return out
}

func (d unavailableDelegates) One(ctx context.Context, run Run, request SeatRequest) SeatResult {
	return d.Group(ctx, run, []SeatRequest{request})[0]
}

func capacityTestRun(id string) Run {
	content := "a complete implementation plan that is long enough for panel review"
	return Run{ID: id, OriginalRequest: "implement the requested capacity behavior",
		Reviewed: Artifact{Stage: "plan", Content: content, Hash: Hash([]byte(content))}}
}

func capacityTestPanel(deadlineMS int) Panel {
	return Panel{Seats: []Seat{{Persona: "qa"}, {Persona: "architect"}},
		MinSuccessful: 2, DeadlineMS: deadlineMS, Acquired: true}
}

func TestSaturatedPanelReportsRetryableCapacityInsteadOfUnreachable(t *testing.T) {
	result, err := Convene(t.Context(), unavailableDelegates{category: "capacity_backpressure"},
		capacityTestRun("capacity"), capacityTestPanel(0), "")
	if err != nil {
		t.Fatal(err)
	}
	if result.Status != StatusPending || result.PauseReason != "panel_capacity" || result.DeadlineHit {
		t.Fatalf("saturated panel lost its capacity state: %+v", result)
	}
	if result.ParticipantsFailed != 2 || len(result.ParticipantFailures) != 2 ||
		!strings.Contains(result.Detail, "capacity") {
		t.Fatalf("capacity diagnostics are incomplete: %+v", result)
	}
}

func TestCapacityWaitDeadlineIsDistinctFromDelegateExecutionDeadline(t *testing.T) {
	capacity, err := Convene(t.Context(), unavailableDelegates{category: "capacity_backpressure", waitForContext: true},
		capacityTestRun("capacity-deadline"), capacityTestPanel(5), "")
	if err != nil {
		t.Fatal(err)
	}
	execution, err := Convene(t.Context(), unavailableDelegates{category: "deadline"},
		capacityTestRun("execution-deadline"), capacityTestPanel(0), "")
	if err != nil {
		t.Fatal(err)
	}
	if capacity.PauseReason != "panel_capacity_deadline" || !capacity.DeadlineHit {
		t.Fatalf("capacity deadline was not typed: %+v", capacity)
	}
	if execution.PauseReason != "panel_deadline" || !execution.DeadlineHit {
		t.Fatalf("delegate execution deadline was not distinct: %+v", execution)
	}
}

func TestMixedCapacityAndExecutionDeadlineRemainsExecutionDeadline(t *testing.T) {
	for _, categories := range []categorizedDelegates{
		{"capacity_backpressure", "deadline"},
		{"capacity_deadline", "deadline"},
	} {
		result, err := Convene(t.Context(), categories,
			capacityTestRun("mixed-deadline"), capacityTestPanel(0), "")
		if err != nil {
			t.Fatal(err)
		}
		if result.PauseReason != "panel_deadline" || !result.DeadlineHit {
			t.Fatalf("execution timeout was mislabeled as a capacity-wait deadline: %+v", result)
		}
	}
}

type overlappingCapacityDelegates struct {
	started chan struct{}
	release chan struct{}
}

func (d overlappingCapacityDelegates) Group(_ context.Context, _ Run, requests []SeatRequest) []SeatResult {
	d.started <- struct{}{}
	<-d.release
	out := make([]SeatResult, len(requests))
	for i := range out {
		out[i] = SeatResult{Err: errors.New("saturated"), FailureCategory: "capacity_backpressure",
			FailureDetail: "eligible pool saturated"}
	}
	return out
}

func (d overlappingCapacityDelegates) One(ctx context.Context, run Run, request SeatRequest) SeatResult {
	return d.Group(ctx, run, []SeatRequest{request})[0]
}

// The original defect appeared only when several roundtables overlapped. Keep
// ten panels in flight at the same barrier so a generic panel_unreachable can
// never hide behind one lucky serial run.
func TestTenOverlappingCapacityCampaignsNeverReportPanelUnreachable(t *testing.T) {
	const campaigns = 10
	delegates := overlappingCapacityDelegates{started: make(chan struct{}, campaigns), release: make(chan struct{})}
	results := make(chan RunResult, campaigns)
	errs := make(chan error, campaigns)
	var wg sync.WaitGroup
	for i := 0; i < campaigns; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			result, err := Convene(t.Context(), delegates, capacityTestRun(fmt.Sprintf("campaign-%d", i)), capacityTestPanel(0), "")
			results <- result
			errs <- err
		}(i)
	}
	for i := 0; i < campaigns; i++ {
		<-delegates.started
	}
	close(delegates.release)
	wg.Wait()
	close(results)
	close(errs)
	for err := range errs {
		if err != nil {
			t.Fatal(err)
		}
	}
	for result := range results {
		if result.PauseReason != "panel_capacity" {
			t.Fatalf("overlapping campaign reported %q instead of panel_capacity: %+v", result.PauseReason, result)
		}
	}
}
