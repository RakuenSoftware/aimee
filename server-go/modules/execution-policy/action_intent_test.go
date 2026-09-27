package executionpolicy

import (
	"encoding/json"
	"strings"
	"testing"
	"time"
)

func actionFixture() (ActionIntent, ActionFreshness, time.Time) {
	now := time.Date(2026, 9, 27, 0, 0, 0, 0, time.UTC)
	i := ActionIntent{SchemaVersion: 1, ID: "action", Request: "request", Task: "task", Attempt: "attempt", Principal: "alice", Root: "root", Tool: "write_file", Class: "file_write", Destination: "workspace:one/file", PayloadDigest: strings.Repeat("a", 64), Purpose: "requested edit", EvidenceDigest: strings.Repeat("b", 64), ContextReceipt: strings.Repeat("c", 64), PolicyGeneration: "policy:1", RevocationGeneration: "revocation:1", IdempotencyKey: "key", WorkUnits: "9", Expires: now.Add(time.Minute)}
	f := ActionFreshness{IntentDigest: actionDigest(i), EvidenceDigest: i.EvidenceDigest, ContextReceipt: i.ContextReceipt, PolicyGeneration: i.PolicyGeneration, RevocationGeneration: i.RevocationGeneration, MemoryGuard: "owner-guard", Checked: now, Expires: now.Add(time.Second), Authorized: true, MemoryRequired: true}
	return i, f, now
}
func TestGovernedActionBindingDispatchAndCrash(t *testing.T) {
	i, f, now := actionFixture()
	var state []byte
	apply := func(op string, intent ActionIntent, lease ActionFreshness) ActionDecision {
		t.Helper()
		next, d, e := GovernedAction("alice", "root", state, op, intent, intent.Class, lease, nil, ActionCompositionPolicy{}, now)
		if e != nil {
			t.Fatal(e)
		}
		if d.Allowed {
			state = next
		}
		return d
	}
	if !apply("prepare", i, f).Allowed {
		t.Fatal("prepare")
	}
	for _, change := range []func(*ActionIntent){func(i *ActionIntent) { i.Destination = "other/file" }, func(i *ActionIntent) { i.PayloadDigest = strings.Repeat("d", 64) }, func(i *ActionIntent) { i.Attempt = "other" }, func(i *ActionIntent) { i.Tool = "git_push" }} {
		bad := i
		change(&bad)
		if d := apply("prepare", bad, f); d.Allowed || d.Reason != "idempotency_conflict" {
			t.Fatal(d)
		}
	}
	if !apply("admit", i, f).Allowed {
		t.Fatal("admit")
	}
	before := string(state)
	if !apply("admit", i, f).Allowed || string(state) != before {
		t.Fatal("double reservation")
	}
	if !apply("dispatch", i, f).DispatchAllowed {
		t.Fatal("dispatch")
	}
	if d := apply("dispatch", i, f); d.DispatchAllowed || d.Reason != "outcome_unknown" {
		t.Fatal(d)
	}
	var j ActionJournal
	json.Unmarshal(state, &j)
	if j.ReservedCalls != 1 || j.ReservedWork != 9 || len(j.Audit) != 3 {
		t.Fatal(j)
	}
	next, d, e := GovernedAction("alice", "root", append([]byte(nil), state...), "dispatch", i, i.Class, f, nil, ActionCompositionPolicy{}, now)
	if e != nil || d.DispatchAllowed || string(next) != string(state) {
		t.Fatal("crash replay acquired dispatch")
	}
}
func TestGovernedActionFreshnessAndRegistry(t *testing.T) {
	i, f, now := actionFixture()
	state, d, e := GovernedAction("alice", "root", nil, "prepare", i, i.Class, f, nil, ActionCompositionPolicy{}, now)
	if e != nil || !d.Allowed {
		t.Fatal(e, d)
	}
	for _, change := range []func(*ActionFreshness){func(f *ActionFreshness) { f.Authorized = false }, func(f *ActionFreshness) { f.MemoryGuard = "" }, func(f *ActionFreshness) { f.MemoryRequired = false }, func(f *ActionFreshness) { f.Expires = now }, func(f *ActionFreshness) { f.Checked = now.Add(time.Second) }, func(f *ActionFreshness) { f.Expires = now.Add(3 * time.Second) }, func(f *ActionFreshness) { f.EvidenceDigest = strings.Repeat("d", 64) }, func(f *ActionFreshness) { f.ContextReceipt = "other" }, func(f *ActionFreshness) { f.PolicyGeneration = "policy:2" }, func(f *ActionFreshness) { f.RevocationGeneration = "revocation:2" }} {
		bad := f
		change(&bad)
		next, d, e := GovernedAction("alice", "root", state, "admit", i, i.Class, bad, nil, ActionCompositionPolicy{}, now)
		if e != nil || d.Allowed || string(next) != string(state) {
			t.Fatal(d, e)
		}
	}
	if _, d, e := GovernedAction("alice", "root", state, "admit", i, "read_only", f, nil, ActionCompositionPolicy{}, now); e != nil || d.Allowed {
		t.Fatal("forged registry class")
	}
	if _, _, e := GovernedAction("mallory", "root", state, "admit", i, i.Class, f, nil, ActionCompositionPolicy{}, now); e == nil {
		t.Fatal("foreign principal")
	}
	if _, _, e := GovernedAction("alice", "other", state, "admit", i, i.Class, f, nil, ActionCompositionPolicy{}, now); e == nil {
		t.Fatal("foreign root")
	}
}
func TestGovernedActionUnknownAndExactVerification(t *testing.T) {
	i, f, now := actionFixture()
	var state []byte
	for _, op := range []string{"prepare", "admit", "dispatch"} {
		next, d, e := GovernedAction("alice", "root", state, op, i, i.Class, f, nil, ActionCompositionPolicy{}, now)
		if e != nil || !d.Allowed {
			t.Fatal(d, e)
		}
		state = next
	}
	unknown := &ActionOutcome{State: "outcome_unknown", EvidenceRef: "transport-timeout", Destination: i.Destination, PayloadDigest: i.PayloadDigest}
	state, d, e := GovernedAction("alice", "root", state, "outcome", i, i.Class, f, unknown, ActionCompositionPolicy{}, now)
	if e != nil || !d.Allowed || CompletionClaim(*d.Receipt) != "outcome_unknown" {
		t.Fatal(d, e)
	}
	wrong := &ActionOutcome{State: "effect_confirmed", EvidenceRef: "owner-observation", Destination: "other/file", PayloadDigest: i.PayloadDigest, ObjectVersion: "revision:4"}
	if _, d, e := GovernedAction("alice", "root", state, "reconcile", i, i.Class, f, wrong, ActionCompositionPolicy{}, now); e != nil || d.Allowed {
		t.Fatal("equal content at wrong object")
	}
	ack := &ActionOutcome{State: "acknowledged", EvidenceRef: "provider-ack", Destination: i.Destination, PayloadDigest: i.PayloadDigest}
	state, d, e = GovernedAction("alice", "root", state, "reconcile", i, i.Class, f, ack, ActionCompositionPolicy{}, now)
	if e != nil || !d.Allowed || CompletionClaim(*d.Receipt) != "provider_acknowledged" {
		t.Fatal(d, e)
	}
	confirmed := *ack
	confirmed.State = "effect_confirmed"
	if _, d, _ := GovernedAction("alice", "root", state, "reconcile", i, i.Class, f, &confirmed, ActionCompositionPolicy{}, now); d.Allowed {
		t.Fatal("unverified effect")
	}
	confirmed.ObjectVersion = "revision:4"
	state, d, e = GovernedAction("alice", "root", state, "reconcile", i, i.Class, f, &confirmed, ActionCompositionPolicy{}, now)
	if e != nil || !d.Allowed || CompletionClaim(*d.Receipt) != "effect_confirmed" {
		t.Fatal(d, e)
	}
	if _, d, _ := GovernedAction("alice", "root", state, "dispatch", i, i.Class, f, nil, ActionCompositionPolicy{}, now); d.DispatchAllowed {
		t.Fatal("confirmed effect replayed")
	}
}
func TestGovernedActionSharedCompositionAndBudgets(t *testing.T) {
	i, f, now := actionFixture()
	var state []byte
	calls, work := uint64(2), uint64(18)
	p := ActionCompositionPolicy{MaxCalls: &calls, MaxWork: &work, ForbidSensitivePublish: true, ForbidElevatedUse: true}
	apply := func(i ActionIntent) ActionDecision {
		t.Helper()
		f.IntentDigest = actionDigest(i)
		for _, op := range []string{"prepare", "admit"} {
			next, d, e := GovernedAction("alice", "root", state, op, i, i.Class, f, nil, p, now)
			if e != nil {
				t.Fatal(e)
			}
			if !d.Allowed {
				return d
			}
			state = next
		}
		return ActionDecision{Allowed: true}
	}
	i.Class = "sensitive_read"
	i.Tool = "read_file"
	if !apply(i).Allowed {
		t.Fatal("read")
	}
	publish := i
	publish.ID = "publish"
	publish.IdempotencyKey = "publish"
	publish.Task = "child"
	publish.Attempt = "delegate"
	publish.Tool = "git_push"
	publish.Class = "external_publish"
	if d := apply(publish); d.Allowed || d.Reason != "composition_restricted" {
		t.Fatal("delegate bypass", d)
	}
	next := i
	next.ID = "next"
	next.IdempotencyKey = "next"
	next.Class = "file_write"
	next.Tool = "write_file"
	if !apply(next).Allowed {
		t.Fatal("second reservation")
	}
	third := next
	third.ID = "third"
	third.IdempotencyKey = "third"
	third.Attempt = "retry"
	if d := apply(third); d.Allowed || d.Reason != "task_resource_ceiling" {
		t.Fatal("retry budget reset", d)
	}
	zero := uint64(0)
	p.MaxCalls = &zero
	state = nil
	if d := apply(i); d.Allowed || d.Reason != "task_resource_ceiling" {
		t.Fatal("zero disabled limit", d)
	}
}

func TestGovernedUnknownOutcomeCannotChangeAttemptToReplay(t *testing.T) {
	i, f, now := actionFixture()
	var state []byte
	for _, op := range []string{"prepare", "admit", "dispatch"} {
		next, d, e := GovernedAction("alice", "root", state, op, i, i.Class, f, nil, ActionCompositionPolicy{}, now)
		if e != nil || !d.Allowed {
			t.Fatal(op, d, e)
		}
		state = next
	}
	second := i
	second.ID = "second"
	second.IdempotencyKey = "second"
	second.Attempt = "retry"
	state, d, e := GovernedAction("alice", "root", state, "prepare", second, second.Class, f, nil, ActionCompositionPolicy{}, now)
	if e != nil || !d.Allowed {
		t.Fatal(d, e)
	}
	f.IntentDigest = actionDigest(second)
	_, d, e = GovernedAction("alice", "root", state, "admit", second, second.Class, f, nil, ActionCompositionPolicy{}, now)
	if e != nil || d.Allowed || d.Reason != "destination_outcome_unresolved" {
		t.Fatal("new attempt bypassed reconciliation", d, e)
	}
}

func TestGovernedJournalCorruptionAndDuplicateID(t *testing.T) {
	i, f, now := actionFixture()
	state, d, e := GovernedAction("alice", "root", nil, "prepare", i, i.Class, f, nil, ActionCompositionPolicy{}, now)
	if e != nil || !d.Allowed {
		t.Fatal(d, e)
	}
	duplicate := i
	duplicate.IdempotencyKey = "different"
	if _, d, e = GovernedAction("alice", "root", state, "prepare", duplicate, duplicate.Class, f, nil, ActionCompositionPolicy{}, now); e != nil || d.Allowed || d.Reason != "action_identity_conflict" {
		t.Fatal(d, e)
	}
	state, d, e = GovernedAction("alice", "root", state, "admit", i, i.Class, f, nil, ActionCompositionPolicy{}, now)
	if e != nil || !d.Allowed {
		t.Fatal(d, e)
	}
	for _, mutate := range []func(*ActionJournal){
		func(j *ActionJournal) { j.ReservedCalls = 0 },
		func(j *ActionJournal) { j.ReservedWork = 0 },
		func(j *ActionJournal) { j.Audit[1].State = "prepared" },
		func(j *ActionJournal) { j.Sequence = 0 },
		func(j *ActionJournal) {
			r := j.Actions[i.IdempotencyKey]
			r.Intent.PayloadDigest = strings.Repeat("d", 64)
			j.Actions[i.IdempotencyKey] = r
		},
	} {
		var j ActionJournal
		json.Unmarshal(state, &j)
		mutate(&j)
		corrupt, _ := json.Marshal(j)
		if _, _, e = GovernedAction("alice", "root", corrupt, "dispatch", i, i.Class, f, nil, ActionCompositionPolicy{}, now); e == nil {
			t.Fatal("corrupt journal accepted")
		}
	}
}

func TestGovernedCancellationRetainsSpentBudget(t *testing.T) {
	for _, admitted := range []bool{false, true} {
		i, f, now := actionFixture()
		state, d, e := GovernedAction("alice", "root", nil, "prepare", i, i.Class, f, nil, ActionCompositionPolicy{}, now)
		if e != nil || !d.Allowed {
			t.Fatal(d, e)
		}
		if admitted {
			state, d, e = GovernedAction("alice", "root", state, "admit", i, i.Class, f, nil, ActionCompositionPolicy{}, now)
			if e != nil || !d.Allowed {
				t.Fatal(d, e)
			}
		}
		state, d, e = GovernedAction("alice", "root", state, "cancel", i, i.Class, f, nil, ActionCompositionPolicy{}, now)
		if e != nil || !d.Allowed || d.Receipt.State != "failed_before_effect" {
			t.Fatal(d, e)
		}
		var j ActionJournal
		json.Unmarshal(state, &j)
		if j.validate() != nil || (admitted && j.ReservedCalls != 1) || (!admitted && j.ReservedCalls != 0) {
			t.Fatal("cancel changed reservation", j)
		}
		if _, d, e = GovernedAction("alice", "root", state, "dispatch", i, i.Class, f, nil, ActionCompositionPolicy{}, now); e != nil || d.DispatchAllowed {
			t.Fatal("cancelled dispatch")
		}
	}
	i, f, now := actionFixture()
	var state []byte
	for _, op := range []string{"prepare", "admit", "dispatch"} {
		next, d, e := GovernedAction("alice", "root", state, op, i, i.Class, f, nil, ActionCompositionPolicy{}, now)
		if e != nil || !d.Allowed {
			t.Fatal(d, e)
		}
		state = next
	}
	next, d, e := GovernedAction("alice", "root", state, "cancel", i, i.Class, f, nil, ActionCompositionPolicy{}, now)
	if e != nil || d.Allowed || d.Reason != "cancellation_requires_reconciliation" || string(next) != string(state) {
		t.Fatal("started effect silently cancelled", d, e)
	}
}
