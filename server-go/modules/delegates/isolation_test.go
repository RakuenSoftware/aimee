package delegates

import (
	"strings"
	"testing"
)

func TestJudgeIsolationFailsClosed(t *testing.T) {
	for _, probe := range []IsolationProbe{IsolationBreached, IsolationUnknown} {
		verdict := JudgeIsolation(probe)
		if !verdict.Refuse || verdict.Reason == "" {
			t.Fatalf("probe %v did not refuse: %+v", probe, verdict)
		}
	}
	if reason := JudgeIsolation(IsolationBreached).Reason; !strings.Contains(reason, "bypass the egress proxy") {
		t.Fatalf("breach reason does not explain impact: %q", reason)
	}
	if verdict := JudgeIsolation(IsolationConfirmed); verdict.Refuse || verdict.Reason != "" {
		t.Fatalf("confirmed isolation refused: %+v", verdict)
	}
}

func TestParseIsolationProbe(t *testing.T) {
	cases := []struct {
		report string
		failed bool
		want   IsolationProbe
	}{
		{"", false, IsolationConfirmed}, {"none=;", false, IsolationConfirmed},
		{`{"none":{"IPAddress":"","GlobalIPv6Address":""}}`, false, IsolationConfirmed},
		{`{"bridge":{"IPAddress":"172.17.0.2"}}`, false, IsolationBreached},
		{"bridge=;", false, IsolationBreached}, {"none=172.17.0.2;", false, IsolationBreached},
		{"something unexpected", false, IsolationUnknown}, {"none=;", true, IsolationUnknown},
	}
	for _, c := range cases {
		if got := ParseIsolationProbe(c.report, c.failed); got != c.want {
			t.Errorf("report %q failed=%v: got %v want %v", c.report, c.failed, got, c.want)
		}
	}
}
