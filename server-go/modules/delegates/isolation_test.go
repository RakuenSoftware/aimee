package delegates

import (
	"strings"
	"testing"
)

// A runtime that did not honour isolation leaves the delegate able to reach the
// network directly, so the egress allowlist is not in force at all.
func TestJudgeIsolationBreachAlwaysReported(t *testing.T) {
	required := JudgeIsolation(IsolationBreached, true)
	if !required.Refuse {
		t.Error("a confirmed breach did not refuse when isolation is required")
	}
	if !strings.Contains(required.Reason, "bypass the egress proxy") {
		t.Errorf("reason does not say what is at stake: %q", required.Reason)
	}

	// Without the requirement it still must not pass silently.
	optional := JudgeIsolation(IsolationBreached, false)
	if optional.Refuse {
		t.Error("refused despite isolation not being required")
	}
	if !optional.Warn {
		t.Error("a confirmed breach was neither refused nor warned about")
	}
}

// The point of the setting: "the probe failed" and "the sandbox is open" are
// indistinguishable from here, so an operator who requires isolation gets a
// refusal rather than a guess.
func TestJudgeIsolationUnknownRefusesOnlyWhenRequired(t *testing.T) {
	required := JudgeIsolation(IsolationUnknown, true)
	if !required.Refuse {
		t.Error("an unverifiable container ran while isolation was required")
	}
	if !strings.Contains(required.Reason, "cannot be proven isolated") {
		t.Errorf("reason does not explain the refusal: %q", required.Reason)
	}

	optional := JudgeIsolation(IsolationUnknown, false)
	if optional.Refuse {
		t.Error("an unverifiable container was refused without the requirement")
	}
	if !optional.Warn {
		t.Error("an unverifiable container passed with no warning at all")
	}
}

func TestJudgeIsolationConfirmedRuns(t *testing.T) {
	for _, required := range []bool{true, false} {
		v := JudgeIsolation(IsolationConfirmed, required)
		if v.Refuse || v.Warn {
			t.Errorf("required=%v: a properly isolated container was not allowed: %+v",
				required, v)
		}
	}
}

// A named network means attachment and an address means reachability; either
// alone is a breach.
func TestParseIsolationProbe(t *testing.T) {
	cases := []struct {
		name   string
		report string
		want   IsolationProbe
	}{
		{"no networks at all", "", IsolationConfirmed},
		{"only the none network", "none=;", IsolationConfirmed},
		{"none network with whitespace", "  none=  ;\n", IsolationConfirmed},
		{"named network attached", "bridge=;", IsolationBreached},
		{"none network but has an address", "none=172.17.0.2;", IsolationBreached},
		{"named network with an address", "bridge=172.17.0.2;", IsolationBreached},
		{"one clean one attached", "none=;bridge=172.17.0.3;", IsolationBreached},
		// Anything that does not match the contract is not evidence of safety.
		{"unparseable", "something unexpected", IsolationUnknown},
		{"partial entry", "bridge", IsolationUnknown},
	}
	for _, c := range cases {
		if got := ParseIsolationProbe(c.report, false); got != c.want {
			t.Errorf("%s: %q -> %v, want %v", c.name, c.report, got, c.want)
		}
	}
}

// A failed probe is never isolated. Reading silence as safety is the mistake
// this check exists to prevent.
func TestParseIsolationProbeFailureIsNeverIsolated(t *testing.T) {
	for _, report := range []string{"", "none=;", "bridge=172.17.0.2;"} {
		if got := ParseIsolationProbe(report, true); got != IsolationUnknown {
			t.Errorf("a failed probe reporting %q returned %v, want unknown", report, got)
		}
	}
}

// End to end: an unreachable runtime must not silently produce a running
// delegate when the operator required isolation.
func TestIsolationFailedProbeUnderRequirementRefuses(t *testing.T) {
	probe := ParseIsolationProbe("", true)
	if v := JudgeIsolation(probe, true); !v.Refuse {
		t.Errorf("failed probe + required isolation did not refuse: %+v", v)
	}
}
