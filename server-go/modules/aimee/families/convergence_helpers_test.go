package families

import (
	"strings"
	"testing"
)

func fingerprint(ch byte) string { return strings.Repeat(string(ch), 64) }

func TestConvergenceFieldsPreservesLegacyAndRejectsMalformedSets(t *testing.T) {
	if summary, set, mode := convergenceFields("legacy summary"); summary != "legacy summary" || set != "" || mode != "legacy" {
		t.Fatalf("legacy=(%q,%q,%q)", summary, set, mode)
	}
	raw := `{"version":1,"mode":"enforce","summary":"human detail","blocker_set":"` + fingerprint('a') + `"}`
	if summary, set, mode := convergenceFields(raw); summary != "human detail" || set != fingerprint('a') || mode != "enforce" {
		t.Fatalf("versioned=(%q,%q,%q)", summary, set, mode)
	}
	bad := `{"version":1,"summary":"human detail","blocker_set":"ABC"}`
	if summary, set, mode := convergenceFields(bad); summary != "human detail" || set != "" || mode != "observe" {
		t.Fatalf("malformed=(%q,%q,%q)", summary, set, mode)
	}
}

func TestBlockerSetRelationship(t *testing.T) {
	a, b, c := fingerprint('a'), fingerprint('b'), fingerprint('c')
	tests := []struct {
		name, previous, current, want string
	}{
		{"equal", a + "," + b, a + "," + b, "stalled"},
		{"strict subset", a + "," + b, a, "progress"},
		{"superset", a, a + "," + b, "regression"},
		{"swap", a + "," + b, b + "," + c, "churn"},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			if got := blockerSetRelationship(tc.previous, tc.current); got != tc.want {
				t.Fatalf("got %q, want %q", got, tc.want)
			}
		})
	}
}

func TestCanonicalBlockerSetRequiresSortedUniqueLowerHex(t *testing.T) {
	a, b := fingerprint('a'), fingerprint('b')
	for _, good := range []string{a, a + "," + b} {
		if !canonicalBlockerSet(good) {
			t.Errorf("rejected canonical set %q", good)
		}
	}
	for _, bad := range []string{"", b + "," + a, a + "," + a, strings.ToUpper(a), "abc"} {
		if canonicalBlockerSet(bad) {
			t.Errorf("accepted malformed set %q", bad)
		}
	}
}
