package engine

import (
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/internal/workflowstore"
)

func stamp(t time.Time) string { return t.UTC().Format("2006-01-02 15:04:05") }

func testScheduler() *Scheduler {
	return &Scheduler{transientPauses: append([]transientPause(nil), schedulerTransientPauses...)}
}

func TestAQuietStoreAsksForNothing(t *testing.T) {
	now := time.Date(2026, 10, 30, 12, 0, 0, 0, time.UTC)
	items := []workflowstore.WorkItem{
		{ID: "wi_done", State: "accepted", UpdatedAt: stamp(now.Add(-time.Hour))},
		{ID: "wi_live", State: "active", UpdatedAt: stamp(now)},
	}
	work := testScheduler().survey(items, nil, now)
	if len(work.transientDue) != 0 || work.wallResumable || work.wallAbandon ||
		work.fanInParents || work.orphans || !work.next.IsZero() {
		t.Fatalf("survey = %+v; a store with nothing paused needs no sweep and no timer", work)
	}
	if work.terminal != 1 {
		t.Fatalf("terminal = %d, want 1", work.terminal)
	}
}

func TestATransientPauseIsDueAtItsBackoff(t *testing.T) {
	now := time.Date(2026, 10, 30, 12, 0, 0, 0, time.UTC)
	items := []workflowstore.WorkItem{
		{ID: "wi_ci", State: "active", PauseReason: "ci_pending", UpdatedAt: stamp(now.Add(-5 * time.Second))},
		{ID: "wi_runner", State: "active", PauseReason: "runner_unavailable", UpdatedAt: stamp(now.Add(-time.Minute))},
		{ID: "wi_operator", State: "active", PauseReason: "awaiting_approval", UpdatedAt: stamp(now.Add(-time.Hour))},
	}
	work := testScheduler().survey(items, nil, now)
	if !work.transientDue["runner_unavailable"] || work.transientDue["ci_pending"] {
		t.Fatalf("due = %v; want only runner_unavailable, whose backoff has passed", work.transientDue)
	}
	// ci_pending backs off 15s from 5s ago, plus a second for the store's
	// whole-second timestamps.
	if want := now.Add(11 * time.Second); !work.next.Equal(want) {
		t.Fatalf("next = %v, want %v", work.next, want)
	}
	if work.transientDue["awaiting_approval"] {
		t.Fatal("an operator's pause is never resumed on a timer")
	}
}

func TestWallCapsFollowThePolicy(t *testing.T) {
	now := time.Date(2026, 10, 30, 12, 0, 0, 0, time.UTC)
	policy := func() RunPolicy {
		return RunPolicy{AutoResumeWall: true, MaxResumes: 2, StaleAbandon: time.Minute}
	}
	items := []workflowstore.WorkItem{
		{ID: "wi_again", State: "active", PauseReason: "wall_cap", OverrideCount: 1, UpdatedAt: stamp(now)},
		{ID: "wi_spent", State: "active", PauseReason: "wall_cap", OverrideCount: 2, UpdatedAt: stamp(now.Add(-30 * time.Second))},
	}
	work := testScheduler().survey(items, policy, now)
	if !work.wallResumable {
		t.Fatal("a wall cap with resumes left is resumed at once")
	}
	if work.wallAbandon {
		t.Fatal("an exhausted wall cap is abandoned only after its grace")
	}
	if want := now.Add(31 * time.Second); !work.next.Equal(want) {
		t.Fatalf("next = %v, want the end of the grace %v", work.next, want)
	}
}

func TestFanInAndOrphansAreSeenInTheListing(t *testing.T) {
	now := time.Date(2026, 10, 30, 12, 0, 0, 0, time.UTC)
	items := []workflowstore.WorkItem{
		{ID: "wi_p", State: "active", PauseReason: "slices_running", UpdatedAt: stamp(now)},
		{ID: "wi_p.s1.g1.a", ParentID: "wi_p", State: "active", UpdatedAt: stamp(now)},
		{ID: "wi_gone", State: "stopped", UpdatedAt: stamp(now)},
		{ID: "wi_gone.s1.g1.a", ParentID: "wi_gone", State: "active", UpdatedAt: stamp(now)},
	}
	work := testScheduler().survey(items, nil, now)
	if !work.fanInParents {
		t.Fatal("a parent waiting on its slices needs the fan-in check")
	}
	if !work.orphans {
		t.Fatal("an active child of a stopped parent is an orphan to reconcile")
	}
}

func TestATruncatedListingSweepsEverything(t *testing.T) {
	now := time.Now()
	work := pendingWork{transientDue: map[string]bool{}}
	work.sweepAll(schedulerTransientPauses, now)
	for _, pause := range schedulerTransientPauses {
		if !work.transientDue[pause.reason] {
			t.Fatalf("%s not swept", pause.reason)
		}
	}
	if !work.wallResumable || !work.wallAbandon || !work.fanInParents || !work.orphans {
		t.Fatalf("sweepAll = %+v", work)
	}
	if want := now.Add(5 * time.Second); !work.next.Equal(want) {
		t.Fatalf("next = %v, want the shortest backoff %v", work.next, want)
	}
}

func TestSleepingUntilDue(t *testing.T) {
	now := time.Now()
	if got := untilDue(time.Time{}, now); got >= 0 {
		t.Fatalf("nothing due: %v, want to sleep until told", got)
	}
	if got := untilDue(now.Add(-time.Minute), now); got != minimumRecheck {
		t.Fatalf("overdue: %v, want the %v floor, not a spin", got, minimumRecheck)
	}
	if got := untilDue(now.Add(20*time.Second), now); got != 20*time.Second {
		t.Fatalf("due in 20s: %v", got)
	}
}
