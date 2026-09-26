package memory

import (
	"reflect"
	"testing"
	"time"
)

func horizonFixture() (horizonRecord, horizonPolicy, time.Time) {
	now := time.Date(2026, 9, 23, 12, 0, 0, 0, time.UTC)
	v := MemoryRecordVersion{SchemaVersion: 1, OwnerID: "00000000-0000-0000-0000-000000000001", RecordID: "42", RecordRevision: "7"}
	r := horizonRecord{Version: v, Kind: "task_state", Domain: "debugging", BaseEligible: true, Created: horizonAnchor{Version: v, EventID: "created:42", At: now.Add(-time.Hour).Format(time.RFC3339)}}
	p := horizonPolicy{SchemaVersion: 1, Revision: "horizon-v1", TransientKinds: map[string]bool{"task_state": true}, Kinds: map[string]horizonRule{"task_state": {ID: "task-hour", DurationSeconds: 3600, Anchor: "created"}}, UnknownRule: "exclude"}
	return r, p, now
}
func TestUtilityHorizonBoundaryAndPurpose(t *testing.T) {
	r, p, now := horizonFixture()
	before := evaluateUtilityHorizon(r, p, "current", now.Add(-time.Nanosecond))
	exact := evaluateUtilityHorizon(r, p, "current", now)
	after := evaluateUtilityHorizon(r, p, "current", now.Add(time.Nanosecond))
	if before.Elapsed || before.WouldExclude || !exact.Elapsed || !exact.WouldExclude || !after.WouldExclude {
		t.Fatal(before, exact, after)
	}
	if exact.Mode != "shadow" || exact.Reason != "utility_horizon_elapsed" || exact.Deadline != now.Format(time.RFC3339) {
		t.Fatal(exact)
	}
	for _, purpose := range []string{"historical", "diagnostic"} {
		d := evaluateUtilityHorizon(r, p, purpose, now)
		if !d.Elapsed || d.WouldExclude {
			t.Fatal(d)
		}
		r.BaseEligible = false
		d = evaluateUtilityHorizon(r, p, purpose, now)
		if d.Status != "blocked" || !d.WouldExclude {
			t.Fatal("inspection bypassed base eligibility", d)
		}
		r.BaseEligible = true
	}
}
func TestUtilityHorizonRulePrecedenceAndDurableKinds(t *testing.T) {
	r, p, now := horizonFixture()
	p.Domains = map[string]horizonRule{"debugging": {ID: "domain", DurationSeconds: 7200, Anchor: "created"}}
	if d := evaluateUtilityHorizon(r, p, "current", now); d.RuleSource != "domain" || d.WouldExclude {
		t.Fatal(d)
	}
	r.Override = &horizonRule{ID: "admitted", DurationSeconds: 0, Anchor: "created"}
	if d := evaluateUtilityHorizon(r, p, "current", now); d.RuleSource != "record_override" || !d.WouldExclude {
		t.Fatal(d)
	}
	p.Safety = map[string]horizonRule{"task_state": {ID: "safety", DurationSeconds: 7200, Anchor: "created"}}
	if d := evaluateUtilityHorizon(r, p, "current", now); d.RuleSource != "system_safety" || d.WouldExclude {
		t.Fatal(d)
	}
	for _, kind := range []string{"constraint", "preference", "procedure", "decision"} {
		r.Kind = kind
		if d := evaluateUtilityHorizon(r, p, "current", now); d.Status != "not_applicable" || d.WouldExclude {
			t.Fatal("unconfigured durable kind expired", d)
		}
	}
	r.Kind = "task_state"
	p.TransientKinds = nil
	if d := evaluateUtilityHorizon(r, p, "current", now); d.WouldExclude {
		t.Fatal(d)
	}
}
func TestUtilityHorizonUnknownAnchorsAndPolicyIdentity(t *testing.T) {
	for _, mutate := range []func(*horizonRecord){
		func(r *horizonRecord) { r.Created.At = "" }, func(r *horizonRecord) { r.Created.At = "yesterday" },
		func(r *horizonRecord) { r.Created.At = "2027-01-01T00:00:00Z" },
		func(r *horizonRecord) { r.Created.Version.RecordRevision = "6" },
		func(r *horizonRecord) { r.Created.Version.OwnerID = "00000000-0000-0000-0000-000000000002" },
		func(r *horizonRecord) { r.Created.EventID = "" },
	} {
		r, p, now := horizonFixture()
		mutate(&r)
		d := evaluateUtilityHorizon(r, p, "current", now)
		if d.Status != "unknown" || !d.WouldExclude || d.Deadline != "" {
			t.Fatal(d)
		}
		p.UnknownRule = "allow"
		if d := evaluateUtilityHorizon(r, p, "current", now); d.Status != "unknown" || d.WouldExclude {
			t.Fatal(d)
		}
	}
	r, p, now := horizonFixture()
	first := evaluateUtilityHorizon(r, p, "current", now)
	if second := evaluateUtilityHorizon(r, p, "current", now); !reflect.DeepEqual(first, second) {
		t.Fatal("nondeterministic", first, second)
	}
	p.Revision = "horizon-v2"
	changed := evaluateUtilityHorizon(r, p, "current", now)
	if first.PolicyDigest == changed.PolicyDigest || first.PolicyRevision == changed.PolicyRevision {
		t.Fatal("policy identity reused")
	}
	r, p, now = horizonFixture()
	saved := r
	for i := 0; i < 50; i++ {
		evaluateUtilityHorizon(r, p, "current", now.Add(time.Duration(i)*time.Second))
	}
	if !reflect.DeepEqual(r, saved) {
		t.Fatal("serving renewed anchor")
	}
	p.Kinds["task_state"] = horizonRule{ID: "confirmation", DurationSeconds: 3600, Anchor: "confirmed"}
	if d := evaluateUtilityHorizon(r, p, "current", now); d.Status != "unknown" {
		t.Fatal(d)
	}
	r.Confirmed = &horizonAnchor{Version: r.Version, EventID: "confirm:42:7", At: now.Format(time.RFC3339)}
	if d := evaluateUtilityHorizon(r, p, "current", now); d.Status != "eligible" || d.WouldExclude {
		t.Fatal(d)
	}
}

func TestUtilityHorizonExplicitDeadlineIsACap(t *testing.T) {
	r, p, now := horizonFixture()
	rule := p.Kinds[r.Kind]
	rule.DurationSeconds = 7200
	rule.Deadline = now.Format(time.RFC3339Nano)
	p.Kinds[r.Kind] = rule
	if d := evaluateUtilityHorizon(r, p, "current", now); !d.Elapsed || d.Deadline != rule.Deadline {
		t.Fatal(d)
	}
	rule.Deadline = now.Add(10 * time.Hour).Format(time.RFC3339Nano)
	p.Kinds[r.Kind] = rule
	if d := evaluateUtilityHorizon(r, p, "current", now); d.Elapsed || d.Deadline != now.Add(time.Hour).Format(time.RFC3339Nano) {
		t.Fatal("explicit deadline extended duration", d)
	}
	rule.Deadline = "tomorrow"
	p.Kinds[r.Kind] = rule
	if d := evaluateUtilityHorizon(r, p, "current", now); d.Status != "unknown" || !d.WouldExclude {
		t.Fatal(d)
	}
	_, p, _ = horizonFixture()
	p.UnknownRule = "allow"
	if d := evaluateUtilityHorizon(r, p, "invented-purpose", now); !d.WouldExclude {
		t.Fatal("unsupported purpose failed open", d)
	}
}
