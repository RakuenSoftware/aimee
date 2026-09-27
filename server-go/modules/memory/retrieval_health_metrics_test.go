package memory

import (
	"encoding/json"
	"math"
	"strings"
	"testing"
	"time"
)

func healthFixture() (healthPopulation, []healthInvocation) {
	at := time.Date(2026, 9, 26, 0, 0, 0, 0, time.UTC)
	p := healthPopulation{Namespace: "owner", Principal: "alice", Project: "project", Workspace: "workspace", Purpose: "task", QueryClass: "typed", Stage: "dispatched", From: at, Until: at.Add(time.Hour)}
	known, valid := true, false
	a := healthRecord{RecordID: "record-secret-a", VersionID: "a:1", Kind: "fact", Family: "family-secret", LowTrust: &known, LifecycleViolation: &valid}
	b := healthRecord{RecordID: "record-secret-b", VersionID: "b:1", Kind: "constraint"}
	e := healthInvocation{Attempt: "first", Binding: "binding", At: at, Namespace: p.Namespace, Principal: p.Principal, Project: p.Project, Workspace: p.Workspace, Purpose: p.Purpose, QueryClass: p.QueryClass, Task: "task-secret", Turn: "1", Stage: p.Stage, SamplePPM: 1000000, SamplingEpoch: "epoch", Records: []healthRecord{a, b, a}}
	next := e
	next.Attempt, next.Turn, next.PreviousTurn, next.At = "second", "2", "1", at.Add(time.Minute)
	next.Records = []healthRecord{a}
	return p, []healthInvocation{e, next}
}

func closeHealth(t *testing.T, got *float64, expected float64) {
	t.Helper()
	if got == nil || math.Abs(*got-expected) > 1e-10 {
		t.Fatalf("got %v; expected %v", got, expected)
	}
}

func TestHealthHandCalculatedPopulationAndIdentityPrivacy(t *testing.T) {
	p, events := healthFixture()
	// An identical collector retry must not create another invocation.
	r, err := aggregateHealth(append(events, events[0]), p)
	if err != nil {
		t.Fatal(err)
	}
	if r.Invocations != 2 || r.Records.Occurrences != 3 || r.Records.Distinct != 2 || r.Versions.Occurrences != 3 {
		t.Fatal(r)
	}
	closeHealth(t, r.Records.Top1Share, 2.0/3)
	closeHealth(t, r.Records.Top5Share, 1)
	closeHealth(t, r.Records.HHI, 5.0/9)
	closeHealth(t, r.Records.Simpson, 4.0/9)
	closeHealth(t, r.Records.Entropy, -(2.0/3)*math.Log2(2.0/3)-(1.0/3)*math.Log2(1.0/3))
	closeHealth(t, r.Repeat.Rate, 1)
	if r.Runs.Lengths[1] != 1 || r.Runs.Lengths[2] != 1 || r.Runs.UnknownStart != 2 || r.Runs.UnknownEnd != 1 {
		t.Fatal(r.Runs)
	}
	if r.Repeat.Denominator != 1 || r.Repeat.Unknown != 2 || r.FamilyCounts[1] != 2 || r.UnknownOrigins != 1 || r.LowTrustFanout[1] != 1 {
		t.Fatal(r)
	}
	if r.Lifecycle.Denominator != 2 || r.Lifecycle.Unknown != 1 || r.EstimatedOccurrences == nil || *r.EstimatedOccurrences != 3 || r.EstimateSE == nil || *r.EstimateSE != 0 {
		t.Fatal(r)
	}
	raw, _ := json.Marshal(r)
	for _, secret := range []string{"record-secret", "family-secret", "task-secret", "alice"} {
		if strings.Contains(string(raw), secret) {
			t.Fatal("aggregate leaks identity", secret)
		}
	}
}

func TestHealthEmptySingleRecordVersionsAndHistoricalLabels(t *testing.T) {
	p, events := healthFixture()
	r, err := aggregateHealth(nil, p)
	if err != nil || r.Records.HHI != nil || r.Records.Entropy != nil || r.Repeat.Rate != nil || r.Lifecycle.Rate != nil || r.EstimatedOccurrences != nil || r.EstimateSE != nil {
		t.Fatal(r, err)
	}
	a := events[0].Records[0]
	b := a
	b.VersionID = "a:2"
	b.Historical = true
	invalid := true
	b.LifecycleViolation = &invalid
	events[0].Records = []healthRecord{a}
	events[1].Records = []healthRecord{b}
	r, err = aggregateHealth(events, p)
	if err != nil {
		t.Fatal(err)
	}
	closeHealth(t, r.Records.HHI, 1)
	closeHealth(t, r.Records.Entropy, 0)
	if r.Records.Normalized != nil || r.Versions.Distinct != 2 || r.Lifecycle.Denominator != 1 || r.Lifecycle.Numerator != 0 {
		t.Fatal(r)
	}
}

func TestHealthPopulationBoundariesAndSamplingUncertainty(t *testing.T) {
	p, events := healthFixture()
	for name, mutate := range map[string]func(*healthInvocation){
		"principal":          func(e *healthInvocation) { e.Principal = "bob" },
		"namespace":          func(e *healthInvocation) { e.Namespace = "other" },
		"project":            func(e *healthInvocation) { e.Project = "other" },
		"workspace":          func(e *healthInvocation) { e.Workspace = "other" },
		"purpose":            func(e *healthInvocation) { e.Purpose = "other" },
		"query class":        func(e *healthInvocation) { e.QueryClass = "other" },
		"unknown dispatch":   func(e *healthInvocation) { e.Stage = "unknown_dispatch" },
		"uncertain dispatch": func(e *healthInvocation) { e.Stage = "network_uncertain" },
		"assembled unsent":   func(e *healthInvocation) { e.Stage = "assembled_unsent" },
		"before window":      func(e *healthInvocation) { e.At = p.From.Add(-time.Nanosecond) },
		"exclusive end":      func(e *healthInvocation) { e.At = p.Until },
	} {
		t.Run(name, func(t *testing.T) {
			e := events[0]
			mutate(&e)
			r, err := aggregateHealth([]healthInvocation{e}, p)
			if err != nil || r.Invocations != 0 {
				t.Fatal(r, err)
			}
		})
	}
	events[0].SamplePPM = 500000
	r, err := aggregateHealth(events, p)
	if err != nil || !r.Sampled || r.EstimatedOccurrences == nil || *r.EstimatedOccurrences != 5 || r.EstimateSE == nil || math.Abs(*r.EstimateSE-math.Sqrt(8)) > 1e-10 || r.Repeat.Denominator != 0 {
		t.Fatal(r, err)
	}
	// Unknown sampling is invalid data, never an exact zero or division by zero.
	events[0].SamplePPM = 0
	if _, err = aggregateHealth(events, p); err == nil {
		t.Fatal("accepted missing sampling metadata")
	}
}

func TestHealthKeyedNamespaceFingerprintsAndConflictingRetries(t *testing.T) {
	key := []byte(strings.Repeat("k", 32))
	f := healthQueryFingerprint(key, "private-a", "yes")
	if len(f) != 64 || f != healthQueryFingerprint(key, "private-a", "yes") || f == healthQueryFingerprint(key, "private-b", "yes") || f == healthQueryFingerprint([]byte(strings.Repeat("q", 32)), "private-a", "yes") || healthQueryFingerprint(nil, "private-a", "yes") != "" {
		t.Fatal("fingerprint domain or key binding")
	}
	p, events := healthFixture()
	conflict := events[0]
	conflict.Binding = "changed"
	if _, err := aggregateHealth(append(events, conflict), p); err == nil {
		t.Fatal("accepted conflicting retry")
	}
	// A missing previous turn does not create adjacency with the last retained one.
	events[1].PreviousTurn = "missing"
	r, err := aggregateHealth(events, p)
	if err != nil || r.Repeat.Denominator != 0 || r.Repeat.Unknown != 3 {
		t.Fatal(r, err)
	}
}

func TestHealthTurnOrderAndBranchesCannotInventContinuity(t *testing.T) {
	p, events := healthFixture()
	events[0].At, events[1].At = events[1].At, events[0].At
	r, err := aggregateHealth(events, p)
	if err != nil || r.Repeat.Denominator != 0 || r.Runs.Lengths[1] != 3 {
		t.Fatal(r, err)
	}
	p, events = healthFixture()
	branch := events[1]
	branch.Attempt = "branch"
	branch.Turn = "3"
	branch.At = branch.At.Add(time.Second)
	r, err = aggregateHealth(append(events, branch), p)
	if err != nil || r.Repeat.Denominator != 0 || r.Runs.Lengths[2] != 0 {
		t.Fatal(r, err)
	}
}
