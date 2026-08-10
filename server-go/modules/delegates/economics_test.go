package delegates

import (
	"strings"
	"testing"
)

func doneHandoff(passed int, changed, owned string) string {
	tests := `[]`
	if passed > 0 {
		tests = `[{"name":"t","status":"passed"}]`
	}
	return `{"schema_version":"delegate_result_v1","status":"done",` +
		`"changed_files":[` + changed + `],"tests":` + tests +
		`,"supervisor_actions":[],"summary":"did it"}`
}

// A cheap run whose delegates verified their own work is the shape the cost
// model is built for.
func TestEconomicsTier0HeavyIsALikelyWin(t *testing.T) {
	tasks := []EconomicsTask{
		{Status: "done", ClaimedBy: "cheap", Files: `["src/a.c"]`,
			Result: doneHandoff(1, `"src/a.c"`, "")},
		{Status: "done", ClaimedBy: "cheap", Files: `["src/b.c"]`,
			Result: doneHandoff(1, `"src/b.c"`, "")},
	}
	agents := []AgentTier{{Name: "cheap", Tier: 0}}

	r := BuildEconomicsReport(tasks, agents)
	if r.DelegateCount != 2 || r.TierCounts[0] != 2 {
		t.Fatalf("counts = %d delegates, tiers %v", r.DelegateCount, r.TierCounts)
	}
	if !r.IsTier0Heavy() {
		t.Error("two tier-0 delegates should be tier-0 heavy")
	}
	if r.ValidHandoffs != 2 || r.InvalidHandoffs != 0 {
		t.Errorf("handoffs valid=%d invalid=%d", r.ValidHandoffs, r.InvalidHandoffs)
	}
	if r.DelegatesWithFocusedTests != 2 || r.FocusedTestsRunByDelegates != 2 {
		t.Errorf("verification = %d/%d", r.DelegatesWithFocusedTests, r.FocusedTestsRunByDelegates)
	}
	if r.Verdict != "likely_net_win" {
		t.Errorf("verdict = %q", r.Verdict)
	}
}

// Expensive seats plus supervisor cleanup is the case the model exists to warn
// about.
func TestEconomicsExpensiveWithCleanupIsALikelyLoss(t *testing.T) {
	tasks := []EconomicsTask{
		{Status: "failed", ClaimedBy: "dear", Result: `{"agent":"dear"}`},
		{Status: "done", ClaimedBy: "dear", Files: `["src/a.c"]`, Result: `not a handoff at all`},
	}
	agents := []AgentTier{{Name: "dear", Tier: 3}}

	r := BuildEconomicsReport(tasks, agents)
	if r.TierCounts[3] != 2 || r.UnknownTierCount != 0 {
		t.Fatalf("tiers = %v unknown=%d", r.TierCounts, r.UnknownTierCount)
	}
	if r.InvalidHandoffs != 1 {
		t.Errorf("invalid handoffs = %d, want 1", r.InvalidHandoffs)
	}
	// The failed task and the unbelievable handoff each cost attention.
	if r.ManualIntegrationEvents != 2 || r.SupervisorActionsRequired != 1 {
		t.Errorf("manual=%d actions=%d", r.ManualIntegrationEvents, r.SupervisorActionsRequired)
	}
	if r.Verdict != "likely_net_loss" {
		t.Errorf("verdict = %q", r.Verdict)
	}
}

// An unknown tier is its own bucket. Counting it as cheap would flatter the run
// into a win it did not earn.
func TestEconomicsUnknownTierIsNotCheap(t *testing.T) {
	tasks := []EconomicsTask{
		{Status: "done", ClaimedBy: "mystery", Files: `[]`, Result: doneHandoff(1, "", "")},
	}
	r := BuildEconomicsReport(tasks, nil)
	if r.UnknownTierCount != 1 || r.TierCounts[0] != 0 {
		t.Fatalf("tiers = %v unknown = %d", r.TierCounts, r.UnknownTierCount)
	}
	if r.IsTier0Heavy() || r.Verdict == "likely_net_win" {
		t.Errorf("unknown tier counted as a win: %q", r.Verdict)
	}
}

// The tier comes from the result first, then the claimer, then the named agent.
func TestEconomicsTierPrecedence(t *testing.T) {
	reported := EconomicsTask{Status: "done", ClaimedBy: "dear", Files: `[]`,
		Result: `{"agent_cost_tier":0,"schema_version":"delegate_result_v1","status":"done",` +
			`"changed_files":[],"tests":[{"status":"passed"}],"summary":"x"}`}
	agents := []AgentTier{{Name: "dear", Tier: 3}}
	if r := BuildEconomicsReport([]EconomicsTask{reported}, agents); r.TierCounts[0] != 1 {
		t.Errorf("reported tier ignored: %v", r.TierCounts)
	}

	named := EconomicsTask{Status: "done", Files: `[]`,
		Result: `{"agent_name":"dear","schema_version":"delegate_result_v1","status":"done",` +
			`"changed_files":[],"tests":[{"status":"passed"}],"summary":"x"}`}
	if r := BuildEconomicsReport([]EconomicsTask{named}, agents); r.TierCounts[3] != 1 {
		t.Errorf("named agent's tier not used: %v", r.TierCounts)
	}
}

// A handoff carried as a `response` field is still the handoff.
func TestEconomicsFindsHandoffInResponseField(t *testing.T) {
	inner := doneHandoff(1, `"src/a.c"`, "")
	asObject := EconomicsTask{Status: "done", ClaimedBy: "cheap", Files: `["src/a.c"]`,
		Result: `{"response":` + inner + `}`}
	if r := BuildEconomicsReport([]EconomicsTask{asObject}, nil); r.ValidHandoffs != 1 {
		t.Errorf("object response: valid=%d invalid=%d", r.ValidHandoffs, r.InvalidHandoffs)
	}
}

// Supervisor actions a delegate asked for are attention it spent, and make the
// run an integration event even when the handoff was believable.
func TestEconomicsCountsRequestedSupervisorActions(t *testing.T) {
	result := `{"schema_version":"delegate_result_v1","status":"done","changed_files":[],` +
		`"tests":[{"status":"passed"}],"supervisor_actions":["review the migration","rerun ci"],` +
		`"summary":"needs a look"}`
	r := BuildEconomicsReport([]EconomicsTask{
		{Status: "done", ClaimedBy: "cheap", Files: `[]`, Result: result},
	}, []AgentTier{{Name: "cheap", Tier: 0}})

	if r.SupervisorActionsRequired != 2 || r.ManualIntegrationEvents != 1 {
		t.Errorf("actions=%d manual=%d", r.SupervisorActionsRequired, r.ManualIntegrationEvents)
	}
	if r.ValidHandoffs != 1 {
		t.Errorf("a believable handoff was rejected: %+v", r)
	}
}

// Queued work is not a delegate run and must not dilute the counts.
func TestEconomicsIgnoresUnclaimedPendingTasks(t *testing.T) {
	r := BuildEconomicsReport([]EconomicsTask{{Status: "pending"}}, nil)
	if r.DelegateCount != 0 || r.Verdict != "unclear" {
		t.Errorf("report = %+v", r)
	}
}

// Token totals only accumulate from results that reported any.
func TestEconomicsTokenTotals(t *testing.T) {
	tasks := []EconomicsTask{
		{Status: "done", ClaimedBy: "cheap", Files: `[]`,
			Result: `{"prompt_tokens":100,"completion_tokens":50,"schema_version":"delegate_result_v1",` +
				`"status":"done","changed_files":[],"tests":[{"status":"passed"}],"summary":"x"}`},
		{Status: "done", ClaimedBy: "cheap", Files: `[]`, Result: doneHandoff(1, "", "")},
	}
	r := BuildEconomicsReport(tasks, []AgentTier{{Name: "cheap", Tier: 0}})
	if r.PromptTokensTotal != 100 || r.CompletionTokensTotal != 50 ||
		r.DelegateTokensEstimated != 150 || r.TokenizedDelegateResults != 1 {
		t.Errorf("tokens = %d/%d/%d over %d results", r.PromptTokensTotal,
			r.CompletionTokensTotal, r.DelegateTokensEstimated, r.TokenizedDelegateResults)
	}
	// 2 delegates * 300, no manual events, no invalid handoffs.
	if r.SupervisorPromptTokensEstimated != 600 {
		t.Errorf("supervisor estimate = %d, want 600", r.SupervisorPromptTokensEstimated)
	}
}

// Ported from test_delegate_economics.c: cheap seats do NOT buy a win when the
// supervisor had to step in for most of them. The recommendation still speaks
// to the tier-0 shape, because that is what the run was -- the verdict and the
// advice answer different questions.
func TestEconomicsTier0HeavyWithHighManualIsUnclear(t *testing.T) {
	withAction := func(file, action string) string {
		return `{"schema_version":"delegate_result_v1","status":"done","changed_files":["` +
			file + `"],"tests":[{"name":"unit","status":"passed"}],` +
			`"supervisor_actions":["` + action + `"],"summary":"done"}`
	}
	tasks := []EconomicsTask{
		{Status: "done", ClaimedBy: "free-a", Files: `["src/a.c"]`,
			Result: withAction("src/a.c", "resolve API naming")},
		{Status: "done", ClaimedBy: "free-b", Files: `["src/b.c"]`,
			Result: withAction("src/b.c", "integrate overlapping changes")},
	}
	agents := []AgentTier{{Name: "free-a", Tier: 0}, {Name: "free-b", Tier: 0}}

	r := BuildEconomicsReport(tasks, agents)
	if !r.IsTier0Heavy() {
		t.Fatal("two tier-0 delegates should be tier-0 heavy")
	}
	if r.ManualIntegrationEvents != 2 {
		t.Errorf("manual events = %d, want 2", r.ManualIntegrationEvents)
	}
	if r.Verdict != "unclear" {
		t.Errorf("verdict = %q, want unclear", r.Verdict)
	}
	if !strings.Contains(r.Recommendation, "broader delegation") {
		t.Errorf("recommendation = %q", r.Recommendation)
	}
}

// A job with no delegates estimates no supervisor cost at all.
func TestEconomicsZeroDelegateJob(t *testing.T) {
	r := BuildEconomicsReport(nil, nil)
	if r.DelegateCount != 0 || r.Verdict != "unclear" ||
		r.SupervisorPromptTokensEstimated != 0 {
		t.Errorf("report = %+v", r)
	}
}

func TestEconomicsVerdictText(t *testing.T) {
	for verdict, want := range map[string]string{
		"likely_net_win":  "likely net supervisor-token win",
		"likely_net_loss": "likely net supervisor-token loss",
		"unclear":         "unclear supervisor-token outcome",
		"nonsense":        "unclear supervisor-token outcome",
	} {
		if got := EconomicsVerdictText(verdict); got != want {
			t.Errorf("%q -> %q, want %q", verdict, got, want)
		}
	}
}
