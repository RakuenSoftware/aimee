package economizer

import "testing"

// Ported from src/tests/test_task_rail.c.

func TestRailSerializeRestoreRoundTrip(t *testing.T) {
	var r TaskRail
	_ = r.Start("obj/with:coords #1", []string{"s0", "s1", "s2"})
	_ = r.Ack(0, "ev0")
	_ = r.Reserve(1)

	s1, err := r.Serialize()
	if err != nil || s1 == "" {
		t.Fatalf("serialize: %v", err)
	}

	var r2 TaskRail
	if err := RestoreRail(&r2, s1); err != nil {
		t.Fatalf("restore: %v", err)
	}
	if len(r2.Steps) != 3 || !r2.Locked {
		t.Fatalf("restored shape wrong: steps=%d locked=%v", len(r2.Steps), r2.Locked)
	}
	if r2.Objective != "obj/with:coords #1" {
		t.Errorf("objective = %q", r2.Objective)
	}
	if r2.Steps[0].State != StepDone || r2.Steps[0].Evidence != "ev0" {
		t.Errorf("step 0 = %+v", r2.Steps[0])
	}
	if r2.Steps[1].State != StepReserved || r2.Steps[2].State != StepPending {
		t.Errorf("states did not round-trip: %+v %+v", r2.Steps[1], r2.Steps[2])
	}

	s2, err := r2.Serialize()
	if err != nil || s2 != s1 {
		t.Error("round-trip must be deterministic")
	}
}

// A malformed blob leaves the caller's LIVE plan untouched.
func TestRailRestoreAllOrNothing(t *testing.T) {
	for _, bad := range []string{"{not json", "", "[1]", `{"steps":5}`} {
		r := TaskRail{Objective: "keep", Locked: true, Steps: []RailStep{{Title: "t"}}}
		if err := RestoreRail(&r, bad); err == nil {
			t.Errorf("RestoreRail(%q) should have failed", bad)
		}
		if r.Objective != "keep" || len(r.Steps) != 1 {
			t.Errorf("failed restore clobbered the live rail: %+v", r)
		}
	}
}

// An out-of-range state normalizes to PENDING — the safe direction, since
// treating an unreadable state as DONE would silently drop work.
func TestRailRestoreNormalizesBadState(t *testing.T) {
	var r TaskRail
	if err := RestoreRail(&r, `{"objective":"o","locked":true,"steps":[{"title":"a","state":99}]}`); err != nil {
		t.Fatal(err)
	}
	if len(r.Steps) != 1 || r.Steps[0].State != StepPending {
		t.Errorf("bad state should normalize to PENDING, got %+v", r.Steps)
	}
}

// Evidence is omitted when absent, so a pending step carries no field implying a
// result.
func TestRailSerializeOmitsEmptyEvidence(t *testing.T) {
	var r TaskRail
	_ = r.Start("o", []string{"a"})
	blob, err := r.Serialize()
	if err != nil {
		t.Fatal(err)
	}
	if blob != `{"objective":"o","locked":true,"steps":[{"title":"a","state":0}]}` {
		t.Errorf("unexpected serialization: %s", blob)
	}
}

func TestRailNilSafety(t *testing.T) {
	var nilRail *TaskRail
	if nilRail.Next() != -1 || nilRail.DoneCount() != 0 {
		t.Error("nil rail accessors must be safe")
	}
	if _, err := nilRail.Serialize(); err == nil {
		t.Error("serializing a nil rail should fail")
	}
	if err := nilRail.Start("x", nil); err == nil {
		t.Error("starting a nil rail should fail")
	}
}
