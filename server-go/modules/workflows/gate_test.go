package workflows

import "testing"

// Ported one-for-one from the C gate suite (src/tests/test_wfe_roundtable.c), so
// the migration is pinned to the behaviour it replaces. Several of these encode
// a specific past failure and are named accordingly.

func mk(persona string, kind VerdictKind, hash string, blockers int) Verdict {
	return Verdict{
		Persona:             persona,
		SchemaVersion:       VerdictSchema,
		ReviewedContentHash: hash,
		Kind:                kind,
		HighSevBlockers:     blockers,
	}
}

func TestEffectiveQuorumSingleLensFloor(t *testing.T) {
	cases := []struct{ requested, nreq, want int }{
		{0, 4, 4},
		{1, 1, 2}, // floor of 2
		{3, 2, 3},
		{5, 4, 5},
	}
	for _, c := range cases {
		if got := EffectiveQuorum(c.requested, c.nreq); got != c.want {
			t.Fatalf("EffectiveQuorum(%d,%d) = %d, want %d", c.requested, c.nreq, got, c.want)
		}
	}
}

func TestGateDecisionMatrix(t *testing.T) {
	req2 := []string{"security", "architect"}

	cases := []struct {
		name     string
		verdicts []Verdict
		required []string
		quorum   int
		want     GateDecision
	}{
		{
			"both approve, no blockers",
			[]Verdict{mk("security", VerdictApprove, "H", 0), mk("architect", VerdictApprove, "H", 0)},
			req2, 2, GateApprove,
		},
		{
			"a required persona is missing",
			[]Verdict{mk("security", VerdictApprove, "H", 0)},
			req2, 2, GateDegraded,
		},
		{
			"a high-severity blocker loops even with quorum met",
			[]Verdict{mk("security", VerdictApprove, "H", 0), mk("architect", VerdictApprove, "H", 1)},
			req2, 2, GateChanges,
		},
		{
			// comment is non-blocking, so approve+comment meets quorum 2
			"comment counts toward quorum",
			[]Verdict{mk("security", VerdictApprove, "H", 0), mk("architect", VerdictComment, "H", 0)},
			req2, 2, GateApprove,
		},
		{
			// ...but at least one lens must explicitly approve
			"comments alone never pass",
			[]Verdict{mk("security", VerdictComment, "H", 0), mk("architect", VerdictComment, "H", 0)},
			req2, 2, GateChanges,
		},
		{
			"any request_changes loops, even with quorum-many non-blocking",
			[]Verdict{
				mk("security", VerdictApprove, "H", 0),
				mk("architect", VerdictComment, "H", 0),
				mk("qa", VerdictRequestChanges, "H", 0),
			},
			req2, 2, GateChanges,
		},
		{
			// A required persona that reviewed a DIFFERENT artifact never validly
			// reviewed this one: an integrity failure, not a definitive CHANGES.
			"tampered hash on a required persona degrades",
			[]Verdict{mk("security", VerdictApprove, "WRONG", 0), mk("architect", VerdictApprove, "H", 0)},
			req2, 2, GateDegraded,
		},
		{
			"malformed required persona degrades",
			[]Verdict{mk("security", VerdictApprove, "H", 0), mk("architect", VerdictMalformed, "H", 0)},
			req2, 2, GateDegraded,
		},
		{
			// REGRESSION: an untrustworthy REQUIRED verdict must not be papered
			// over by other panelists meeting quorum.
			"untrustworthy required verdict is not covered by others' quorum",
			[]Verdict{
				mk("security", VerdictMalformed, "H", 0),
				mk("architect", VerdictApprove, "H", 0),
				mk("qa", VerdictApprove, "H", 0),
			},
			[]string{"security"}, 2, GateDegraded,
		},
	}

	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			got := GateDecide(c.verdicts, c.required, c.quorum, "H")
			if got.Decision != c.want {
				t.Fatalf("decision = %q (%s), want %q", got.Decision, got.Reason, c.want)
			}
			if got.Reason == "" {
				t.Fatal("every ruling must carry a reason")
			}
		})
	}
}

// An unknown schema version is not merely ignored: it is untrustworthy, so it
// fails closed exactly like a malformed verdict.
func TestUnknownSchemaFailsClosed(t *testing.T) {
	bad := mk("security", VerdictApprove, "H", 0)
	bad.SchemaVersion = VerdictSchema + 1
	got := GateDecide([]Verdict{bad, mk("architect", VerdictApprove, "H", 0)},
		[]string{"security", "architect"}, 2, "H")
	if got.Decision != GateDegraded {
		t.Fatalf("decision = %q, want degraded", got.Decision)
	}
}

// With no artifact hash to compare against, hash mismatch cannot be detected and
// must not be invented -- the other integrity rules still apply.
func TestEmptyArtifactHashSkipsHashCheck(t *testing.T) {
	got := GateDecide(
		[]Verdict{mk("security", VerdictApprove, "anything", 0), mk("architect", VerdictApprove, "other", 0)},
		[]string{"security", "architect"}, 2, "")
	if got.Decision != GateApprove {
		t.Fatalf("decision = %q (%s), want approve", got.Decision, got.Reason)
	}
}
