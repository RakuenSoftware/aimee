package economizer

import (
	"encoding/json"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

// The gateway seam's apply/bypass verdict is decided in the module, so these
// pin the WIRING (does the stage decide at all, with the structural check
// actually connected) rather than the decision logic, which gateway_test.go
// already covers directly.

func TestReduceStageDecidesOnTheGatewaySeam(t *testing.T) {
	resp, status := invoke(t, ReduceRequest{
		Messages:      rawMessages(t, 20),
		SystemPrompt:  "sys",
		Seam:          "gateway",
		HistoryFold:   true,
		ClosetEnabled: true,
	})
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !resp.Mutated || resp.Reason != "reduced" {
		t.Fatalf("expected a genuine reduction to decide on: %+v", resp)
	}
	// A clean reduction that shrank is the one case worth mutating for.
	if resp.Bypass != "none" {
		t.Errorf("bypass = %q, want \"none\"", resp.Bypass)
	}
}

// The delegate seam has no apply/bypass decision, so the field must stay empty
// rather than carrying a verdict its caller would have no business reading.
func TestReduceStageLeavesTheDelegateSeamUndecided(t *testing.T) {
	resp, status := invoke(t, ReduceRequest{
		Messages:      rawMessages(t, 20),
		SystemPrompt:  "sys",
		Seam:          "delegate",
		HistoryFold:   true,
		ClosetEnabled: true,
	})
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if resp.Bypass != "" {
		t.Errorf("bypass = %q on the delegate seam, want empty", resp.Bypass)
	}
}

// A measure-only pass changes nothing, so there is nothing to apply.
func TestReduceStageBypassesWhenNothingWasApplied(t *testing.T) {
	resp, status := invoke(t, ReduceRequest{
		Messages:     rawMessages(t, 20),
		SystemPrompt: "sys",
		Seam:         "gateway",
		HistoryFold:  true,
		MeasureOnly:  true,
	})
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if resp.Mutated {
		t.Fatalf("measure-only must not mutate: %+v", resp)
	}
	if resp.Bypass != "no_op" {
		t.Errorf("bypass = %q, want \"no_op\"", resp.Bypass)
	}
}

// The structural check must be REACHABLE from the stage, not merely present.
// GWShouldApply skips the check entirely when the port is nil, and a nil port
// is indistinguishable from a clean transcript by verdict alone — both give
// "none". So this drives a transcript whose reduced view is structurally
// broken and asserts the stage catches it; it fails if the port is ever
// unwired.
func TestReduceStageRunsTheStructuralCheck(t *testing.T) {
	// A tool result whose matching call is not in the array is an orphan: the
	// exact shape the guard exists to keep away from a provider.
	arr := NewArray()
	for i := 0; i < 12; i++ {
		appendTurn(arr, i)
	}
	orphan := NewObject()
	orphan.Set("role", NewString("tool"))
	orphan.Set("tool_call_id", NewString("call_missing"))
	orphan.Set("content", NewString("a result for a call that is not here"))
	arr.Append(orphan)

	// Pin the premise: this transcript IS structurally broken by the same
	// function the stage is expected to use. If this stops holding, the test
	// below is proving nothing.
	if MessageHistoryRepair(arr.Clone()) == 0 {
		t.Skip("fixture is no longer structurally broken; rewrite it")
	}

	body, err := json.Marshal(ReduceRequest{
		Messages:      json.RawMessage(PrintJSONUnformatted(arr)),
		SystemPrompt:  "sys",
		Seam:          "gateway",
		HistoryFold:   true,
		ClosetEnabled: true,
	})
	if err != nil {
		t.Fatal(err)
	}
	out, status := NewHandler()(bus.ModuleInvocation{StageID: StageReduce}, body)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	var resp ReduceResponse
	if err := json.Unmarshal(out, &resp); err != nil {
		t.Fatal(err)
	}
	if !resp.Mutated {
		t.Skip("no reduction was applied, so there is no view to check")
	}
	if resp.Bypass == "none" {
		t.Errorf("a structurally broken reduced view was cleared for dispatch "+
			"(bypass=%q) — the structural check is not wired", resp.Bypass)
	}
}
