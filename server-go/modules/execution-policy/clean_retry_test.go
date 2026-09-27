package executionpolicy

import (
	"encoding/json"
	"strings"
	"testing"
	"time"
)

func retryFixture() (RetryPolicy, time.Time) {
	return RetryPolicy{Enabled: true, MaxAttempts: 3, MaxRepeatedFailures: 2, MaxWallSeconds: 600, MaxProviderBytes: 1000, NanodollarsPerByte: 2, MaxNanodollars: 2000, RetainInputSeconds: 60}, time.Date(2026, 9, 27, 0, 0, 0, 0, time.UTC)
}
func TestCleanRetryIsolationAndSharedLimits(t *testing.T) {
	p, now := retryFixture()
	var raw []byte
	call := func(r RetryRequest) RetryDecision {
		t.Helper()
		next, d, e := CleanRetry("alice", "root", raw, nil, r, p, "policy", now)
		if e != nil {
			t.Fatal(e)
		}
		if d.Allowed {
			raw = next
		}
		return d
	}
	first := call(RetryRequest{Operation: "begin", Request: "one", Input: "Original user constraints"})
	if !first.Allowed {
		t.Fatal(first)
	}
	if d := call(RetryRequest{Operation: "send", Attempt: first.Attempt, Call: RetryCall{ID: "wire1", Bytes: 600, Payload: strings.Repeat("a", 64)}}); !d.Allowed {
		t.Fatal(d)
	}
	if d := call(RetryRequest{Operation: "send", Attempt: first.Attempt, Call: RetryCall{ID: "wire1", Bytes: 600, Payload: strings.Repeat("a", 64)}}); d.Allowed {
		t.Fatal("resend reused reservation")
	}
	if d := call(RetryRequest{Operation: "finish", Attempt: first.Attempt, Failure: "execution_failure"}); !d.Allowed {
		t.Fatal(d)
	}
	retry := RetryRequest{Operation: "begin", Request: "two", Previous: first.Attempt, Input: "Current complete replacement constraints", ReplaceConstraints: true}
	d := call(retry)
	if !d.Allowed || d.Attempt == first.Attempt || d.RemainingAttempts != 1 || d.RemainingBytes != 400 || d.WorkspaceRestored || !d.FreshAssemblyRequired || strings.Contains(d.Summary, "Original user constraints") {
		t.Fatal(d)
	}
	if d := call(RetryRequest{Operation: "send", Attempt: d.Attempt, Call: RetryCall{ID: "wire2", Bytes: 401, Payload: strings.Repeat("b", 64)}}); d.Allowed || d.Reason != "task_provider_byte_ceiling" {
		t.Fatal(d)
	}
	if d := call(RetryRequest{Operation: "finish", Attempt: d.Attempt, Failure: "execution_failure"}); !d.Allowed {
		t.Fatal(d)
	}
	retry.Request = "three"
	retry.Previous = d.Attempt
	if d := call(retry); d.Allowed || d.Reason != "repeated_failure_ceiling" {
		t.Fatal(d)
	}
	// Copying persisted bytes simulates a new reducer process; spent allowance
	// remains unchanged, and corruption cannot mint a fresh allowance.
	var j RetryJournal
	json.Unmarshal(raw, &j)
	j.Bytes = 0
	corrupt, _ := json.Marshal(j)
	if _, _, e := CleanRetry("alice", "root", corrupt, nil, retry, p, "policy", now); e == nil {
		t.Fatal("counter corruption accepted")
	}
	if _, _, e := CleanRetry("mallory", "root", raw, nil, retry, p, "policy", now); e == nil {
		t.Fatal("foreign owner accepted")
	}
}
func TestCleanRetryBaselineAndBudgetGaps(t *testing.T) {
	p, now := retryFixture()
	raw, d, e := CleanRetry("alice", "root", nil, nil, RetryRequest{Operation: "begin", Request: "one", Input: "baseline"}, p, "policy", now)
	if e != nil || !d.Allowed {
		t.Fatal(e, d)
	}
	id := d.Attempt
	raw, d, e = CleanRetry("alice", "root", raw, nil, RetryRequest{Operation: "finish", Attempt: id, Failure: "context_refused"}, p, "policy", now)
	if e != nil || !d.Allowed {
		t.Fatal(e, d)
	}
	retry := RetryRequest{Operation: "begin", Request: "two", Previous: id, Input: "new", ReplaceConstraints: true}
	for _, test := range []struct {
		name    string
		offset  time.Duration
		policy  RetryPolicy
		replace bool
		want    string
	}{
		{"expired", 61 * time.Second, p, true, "baseline_unavailable_fresh_plan_required"},
		{"wall", 601 * time.Second, p, true, "task_wall_ceiling"},
		{"constraints", 0, p, false, "current_complete_constraints_required"},
	} {
		t.Run(test.name, func(t *testing.T) {
			r := retry
			r.ReplaceConstraints = test.replace
			_, d, e := CleanRetry("alice", "root", raw, nil, r, test.policy, "policy2", now.Add(test.offset))
			if e != nil || d.Allowed || d.Reason != test.want {
				t.Fatal(e, d)
			}
		})
	}
	zero := uint64(0)
	p.MaxTokens = &zero
	if _, d, _ := CleanRetry("alice", "root", raw, nil, retry, p, "policy", now); d.Allowed || d.Reason != "exact_token_counter_unavailable" {
		t.Fatal(d)
	}
	p.MaxTokens = nil
	p.MaxNanodollars = 1
	if _, d, _ := CleanRetry("alice", "root", nil, nil, RetryRequest{Operation: "begin", Request: "new", Input: "current"}, p, "policy", now); !d.Allowed {
		t.Fatal(d)
	}
	var j RetryJournal
	json.Unmarshal(raw, &j)
	j.Attempts[0].Input = "tampered"
	bad, _ := json.Marshal(j)
	if _, _, e := CleanRetry("alice", "root", bad, nil, retry, p, "policy", now); e == nil {
		t.Fatal("corrupt baseline accepted")
	}
}
func TestCleanRetryPreservesUnknownAndVerifiedEffects(t *testing.T) {
	i, f, now := actionFixture()
	var actions []byte
	for _, op := range []string{"prepare", "admit", "dispatch"} {
		next, d, e := GovernedAction("alice", "root", actions, op, i, i.Class, f, nil, ActionCompositionPolicy{}, now)
		if e != nil || !d.Allowed {
			t.Fatal(e, d)
		}
		actions = next
	}
	p, _ := retryFixture()
	raw, d, _ := CleanRetry("alice", "root", nil, actions, RetryRequest{Operation: "begin", Request: "one", Input: "baseline"}, p, "policy", now)
	id := d.Attempt
	raw, _, _ = CleanRetry("alice", "root", raw, actions, RetryRequest{Operation: "finish", Attempt: id, Failure: "execution_failure"}, p, "policy", now)
	r := RetryRequest{Operation: "begin", Request: "two", Previous: id, Input: "current instructions", ReplaceConstraints: true}
	if _, d, _ := CleanRetry("alice", "root", raw, actions, r, p, "policy", now); d.Allowed || d.Reason != "unknown_effect_reconciliation_required" {
		t.Fatal(d)
	}
	verified := &ActionOutcome{State: "effect_confirmed", EvidenceRef: "owner", Destination: i.Destination, PayloadDigest: i.PayloadDigest, ObjectVersion: "observed-version"}
	actions, _, _ = GovernedAction("alice", "root", actions, "outcome", i, i.Class, f, verified, ActionCompositionPolicy{}, now)
	next, d, e := CleanRetry("alice", "root", raw, actions, r, p, "new-policy", now)
	if e != nil || !d.Allowed || !strings.Contains(d.Summary, "observed-version") || !strings.Contains(d.Summary, i.Destination) || d.WorkspaceRestored {
		t.Fatal(e, d)
	}
	var j RetryJournal
	json.Unmarshal(next, &j)
	if j.Attempts[1].Policy != "new-policy" {
		t.Fatal("old policy resurrected")
	}
}

func TestCleanRetryDisablePreservesInFlightOutcome(t *testing.T) {
	p, now := retryFixture()
	raw, d, e := CleanRetry("alice", "root", nil, nil, RetryRequest{Operation: "begin", Request: "one", Input: "current"}, p, "policy", now)
	if e != nil || !d.Allowed {
		t.Fatal(e, d)
	}
	next, done, e := CleanRetry("alice", "root", raw, nil, RetryRequest{Operation: "finish", Attempt: d.Attempt, Failure: "execution_failure"}, RetryPolicy{}, "disabled", now)
	if e != nil || !done.Allowed {
		t.Fatal(e, done)
	}
	_, blocked, e := CleanRetry("alice", "root", next, nil, RetryRequest{Operation: "begin", Request: "two", Input: "current"}, RetryPolicy{}, "disabled", now)
	if e != nil || blocked.Allowed || blocked.Reason != "retry_disabled" {
		t.Fatal(e, blocked)
	}
}
