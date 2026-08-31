package panel

import (
	"context"
	"strings"
	"testing"
	"time"
)

func TestRoundtableStageGuidanceCoversEverySupportedStage(t *testing.T) {
	tests := map[string]string{
		"intent":      "acceptance criteria faithfully capture",
		"plan":        "goal-only restatement",
		"frozen_diff": "negative or unavailable lookup evidence",
	}
	for stage, marker := range tests {
		if normalized, ok := normalizeRoundtableStage(stage); !ok || normalized != stage || !strings.Contains(roundtableStageGuidance(normalized), marker) {
			t.Fatalf("stage %q lacks its guidance marker %q", stage, marker)
		}
	}
}

func TestRoundtableRepairPreservesNonBlockingApprovalFindings(t *testing.T) {
	prompt := panelResponseRepairPrompt("run", "hash", "frozen_diff", "invalid")
	if !strings.Contains(prompt, "may carry suggestion or nit findings") || !strings.Contains(prompt, `"verdict":"approve|changes|blocked"`) || strings.Contains(prompt, "approve only with an empty findings array") {
		t.Fatalf("repair prompt contradicts the panel verdict contract: %s", prompt)
	}
}

func TestPanelSeatDurableSlotCannotAliasDelimitedIdentifiers(t *testing.T) {
	left := Run{ID: "a:b", Stage: "c"}
	right := Run{ID: "a", Stage: "b:c"}
	if got, other := seatDurableSlot(left, 1, 0), seatDurableSlot(right, 1, 0); got == other {
		t.Fatalf("structured identities aliased: %q", got)
	}
}

// Suggestions and nits must not gate an artifact: the panel's severity taxonomy
// exists to separate work that cannot ship from advisory polish. Gating on every
// finding made any multi-seat gate unpassable.
func TestBlockingFindingCountIgnoresAdvisorySeverities(t *testing.T) {
	cases := []struct {
		name     string
		findings []Finding
		want     int
	}{
		{"empty", nil, 0},
		{"only advisory", []Finding{{Severity: "suggestion"}, {Severity: "nit"}, {Severity: "NIT"}, {Severity: " Suggestion "}}, 0},
		{"blocking and foundational", []Finding{{Severity: "blocking"}, {Severity: "foundational"}}, 2},
		{"mixed", []Finding{{Severity: "nit"}, {Severity: "blocking"}, {Severity: "suggestion"}}, 1},
		{"unclassified is blocking", []Finding{{Severity: ""}, {Severity: "weird"}}, 2},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if got := blockingFindingCount(tc.findings); got != tc.want {
				t.Fatalf("blockingFindingCount=%d want %d", got, tc.want)
			}
		})
	}
}

// The chairman is a separate step: it gets the configured deadline in full,
// measured from the step context, however long the seats took. Sharing the
// panel's context starved it to zero whenever they ran long, and it failed on
// the POST that merely launches its job.
func TestChairmanGetsItsOwnFullDeadline(t *testing.T) {
	const deadlineMS = 600_000
	budget := time.Duration(deadlineMS) * time.Millisecond
	step := t.Context()

	ctx, done := chairmanDeadline(step, deadlineMS)
	defer done()
	deadline, ok := ctx.Deadline()
	if !ok {
		t.Fatal("chairman ran with no deadline at all")
	}
	// Its budget is the configured one, not a remainder, so it must be close to
	// the full value rather than some fraction of it.
	if remaining := time.Until(deadline); remaining < budget-time.Minute {
		t.Fatalf("chairman budget=%v, want the configured %v", remaining, budget)
	}

	t.Run("an exhausted analysis phase does not shorten it", func(t *testing.T) {
		exhausted, cancel := context.WithTimeout(step, time.Millisecond)
		defer cancel()
		<-exhausted.Done()
		ctx, done := chairmanDeadline(step, deadlineMS)
		defer done()
		if err := ctx.Err(); err != nil {
			t.Fatalf("chairman inherited a spent budget: %v", err)
		}
		deadline, _ := ctx.Deadline()
		if remaining := time.Until(deadline); remaining < budget-time.Minute {
			t.Fatalf("chairman budget=%v after slow seats, want %v", remaining, budget)
		}
	})

	t.Run("no configured deadline is left alone", func(t *testing.T) {
		ctx, done := chairmanDeadline(step, 0)
		defer done()
		if ctx != step {
			t.Fatal("an unbounded roundtable must stay unbounded")
		}
	})
}

// Four runs of the same proposal burned their entire round budget rediscovering
// that the REQUEST was unimplementable: it asked the lint to fire when a
// "declared subject" stopped resolving, and no such declaration exists. The gate
// could only say "changes", so the author rewrote a plan that could never satisfy
// it, until convergence_limit parked with no recorded reason. A reviewer must be
// able to say the request itself is the problem.
func TestBlockedIsAUsableVerdictAndDemandsFindings(t *testing.T) {
	blocked := panelResponse{Verdict: "blocked"}
	if panelVerdictError(blocked) == nil {
		t.Fatal("blocked without findings must be rejected: it names no reason a human could act on")
	}
	blocked.Findings = []panelFinding{{Severity: "foundational", Summary: "the request depends on a declaration that does not exist"}}
	if err := panelVerdictError(blocked); err != nil {
		t.Fatalf("blocked with a finding must be usable: %v", err)
	}
}
