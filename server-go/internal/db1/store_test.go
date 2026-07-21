package db1

import (
	"context"
	"path/filepath"
	"strings"
	"testing"
)

func newTestStore(t *testing.T) *Store {
	t.Helper()
	store, err := Open(filepath.Join(t.TempDir(), "aimee.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = store.Close() })
	return store
}

func createTestItem(t *testing.T, store *Store, id string) {
	t.Helper()
	err := store.CreateWorkItem(context.Background(), CreateWorkItem{
		ID: id, Repo: "repo", ProposalPath: id + ".md", WorkflowName: "build",
		WorkflowVersion: strings.Repeat("f", 64), StartStage: "plan_gate", Mode: "autonomous",
	})
	if err != nil {
		t.Fatal(err)
	}
}

func TestMaxIterationsParksWithoutAbandoning(t *testing.T) {
	store := newTestStore(t)
	createTestItem(t, store, "wi_cap")
	ctx := context.Background()

	for i := 0; i < 3; i++ {
		out, err := store.RecordRequestedChanges(ctx, "wi_cap", "plan_gate", "plan",
			"plan-"+string(rune('a'+i)), "feedback-"+string(rune('a'+i)), 3, 3)
		if err != nil {
			t.Fatal(err)
		}
		if out.Parked != (i == 2) {
			t.Fatalf("attempt %d parked=%v", i+1, out.Parked)
		}
	}
	item, err := store.WorkItem(ctx, "wi_cap")
	if err != nil {
		t.Fatal(err)
	}
	if item.State != "active" || item.PauseReason != "convergence_limit" {
		t.Fatalf("state=%q pause=%q, want active/convergence_limit", item.State, item.PauseReason)
	}
}

func TestIdenticalPlanAndFeedbackParksAsNoProgress(t *testing.T) {
	store := newTestStore(t)
	createTestItem(t, store, "wi_repeat")
	ctx := context.Background()
	for i := 0; i < 3; i++ {
		out, err := store.RecordRequestedChanges(ctx, "wi_repeat", "plan_gate", "plan",
			"same-plan", "same-feedback", 24, 3)
		if err != nil {
			t.Fatal(err)
		}
		if i == 2 && (!out.Parked || out.PauseReason != "convergence_no_progress") {
			t.Fatalf("third identical review outcome: %+v", out)
		}
	}
	item, err := store.WorkItem(ctx, "wi_repeat")
	if err != nil {
		t.Fatal(err)
	}
	if item.State != "active" || item.PauseReason != "convergence_no_progress" {
		t.Fatalf("state=%q pause=%q", item.State, item.PauseReason)
	}
}

func TestChangedPlanOrFeedbackIsPositiveProgress(t *testing.T) {
	store := newTestStore(t)
	createTestItem(t, store, "wi_progress")
	ctx := context.Background()
	cases := [][2]string{{"plan-a", "feedback-a"}, {"plan-b", "feedback-a"}, {"plan-b", "feedback-b"}}
	for _, pair := range cases {
		out, err := store.RecordRequestedChanges(ctx, "wi_progress", "plan_gate", "plan",
			pair[0], pair[1], 24, 3)
		if err != nil {
			t.Fatal(err)
		}
		if out.Parked || out.IdenticalRepeats != 1 {
			t.Fatalf("changed review was not recognized as progress: %+v", out)
		}
	}
}
