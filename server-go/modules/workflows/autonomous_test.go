package workflows

import "testing"

// Ported one-for-one from src/tests/test_wfe_autonomous_route.c, which locks the
// S4 roundtable rulings (2026-07-03). Each case below corresponds to an
// assertion there.

func TestAutonomousFloorIsFullSpine(t *testing.T) {
	// The floor is a full-spine enforced workflow, not the weaker "build".
	if AutonomousFloor != "managed-change" {
		t.Fatalf("floor = %q, want managed-change", AutonomousFloor)
	}
	// Sweep is pinned to the human gate, and that gate is itself never an
	// auto-selectable lane: unvetted candidates can never reach an auto-executing
	// workflow.
	if SweepWorkflowFloor != "manual-review" {
		t.Fatalf("sweep floor = %q, want manual-review", SweepWorkflowFloor)
	}
	if AutonomousSelectable(SweepWorkflowFloor, false) {
		t.Fatal("the sweep floor must not be auto-selectable")
	}
}

func TestAutonomousSelectable(t *testing.T) {
	cases := []struct {
		name     string
		id       string
		enforced bool
		want     bool
	}{
		{"enforced full-spine lane", "managed-change", true, true},
		{"another enforced lane", "hotfix", true, true},
		// enforced:false lanes (build, anything pre-gate.deliver) are not auto-selectable
		{"build is not enforced", "build", false, false},
		{"enforced flag off", "managed-change", false, false},
		// read-only lanes are rejected even if mis-marked enforced (defence in depth)
		{"read-only converse, mis-marked enforced", "converse", true, false},
		{"read-only research, mis-marked enforced", "research", true, false},
		{"read-only converse, not enforced", "converse", false, false},
		// degenerate input fails closed
		{"empty id", "", true, false},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			if got := AutonomousSelectable(c.id, c.enforced); got != c.want {
				t.Fatalf("AutonomousSelectable(%q,%v) = %v, want %v", c.id, c.enforced, got, c.want)
			}
		})
	}
}

func TestAutonomousClamp(t *testing.T) {
	// A selectable id passes through unchanged and is not reported clamped.
	got := AutonomousClamp("hotfix", true)
	if got.Workflow != "hotfix" || got.Clamped || !got.Selectable {
		t.Fatalf("selectable id = %+v", got)
	}

	// Read-only, non-enforced and empty ids are all lifted to the floor.
	for _, c := range []struct {
		id       string
		enforced bool
	}{
		{"research", false},
		{"build", false},
		{"", false},
		{"converse", true},
	} {
		got := AutonomousClamp(c.id, c.enforced)
		if got.Workflow != AutonomousFloor || !got.Clamped || got.Selectable {
			t.Fatalf("AutonomousClamp(%q,%v) = %+v, want the floor", c.id, c.enforced, got)
		}
	}
}
