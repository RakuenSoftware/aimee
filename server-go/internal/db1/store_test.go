package db1

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"path/filepath"
	"strings"
	"sync"
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

func TestOpenMigratesPreGoWorkflowSchema(t *testing.T) {
	path := filepath.Join(t.TempDir(), "old.db")
	db, err := sql.Open("sqlite", "file:"+path)
	if err != nil {
		t.Fatal(err)
	}
	_, err = db.Exec(`CREATE TABLE lifecycle_work_item (id INTEGER PRIMARY KEY, work_item_id TEXT UNIQUE, repo TEXT DEFAULT '', proposal_path TEXT DEFAULT '', workflow_name TEXT DEFAULT 'build', workflow_version TEXT DEFAULT '', current_stage TEXT DEFAULT '', state TEXT DEFAULT 'active', mode TEXT DEFAULT 'autonomous', pause_reason TEXT DEFAULT '', paused_state TEXT DEFAULT '', content_hash TEXT DEFAULT '', pr_ref TEXT DEFAULT '', submitter TEXT DEFAULT '', cum_cost_usd REAL DEFAULT 0, override_count INTEGER DEFAULT 0, created_at TEXT DEFAULT CURRENT_TIMESTAMP, updated_at TEXT DEFAULT CURRENT_TIMESTAMP, UNIQUE(repo, proposal_path))`)
	if err != nil {
		t.Fatal(err)
	}
	_ = db.Close()
	store, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	if err := store.CreateWorkItem(context.Background(), CreateWorkItem{ID: "wi_migrated", Repo: "r", ProposalPath: "p", WorkflowName: "build", WorkflowVersion: strings.Repeat("a", 64), StartStage: "start", SourcePath: "docs/proposals/pending/p.md"}); err != nil {
		t.Fatal(err)
	}
	item, err := store.WorkItem(context.Background(), "wi_migrated")
	if err != nil {
		t.Fatal(err)
	}
	if item.SourcePath == "" {
		t.Fatal("source_path migration missing")
	}
}

func TestConcurrentRootAdmissionNeverExceedsCap(t *testing.T) {
	store := newTestStore(t)
	const attempts = 12
	const cap = 2
	var wg sync.WaitGroup
	var admitted int
	var mu sync.Mutex
	for i := 0; i < attempts; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			err := store.AdmitRoot(context.Background(), CreateWorkItem{ID: fmt.Sprintf("wi_admit_%d", i), Repo: "repo", ProposalPath: fmt.Sprintf("p-%d", i), WorkflowName: "build", StartStage: "start"}, cap)
			if err == nil {
				mu.Lock()
				admitted++
				mu.Unlock()
				return
			}
			if !errors.Is(err, ErrAdmissionFull) {
				t.Errorf("admit: %v", err)
			}
		}(i)
	}
	wg.Wait()
	if admitted != cap {
		t.Fatalf("admitted=%d want=%d", admitted, cap)
	}
	count, err := store.ActiveRootCount(context.Background())
	if err != nil || count != cap {
		t.Fatalf("count=%d err=%v", count, err)
	}
}

func TestWorkItemByGitProposalMatchesCommitQualifiedLegacyIdentity(t *testing.T) {
	store := newTestStore(t)
	legacy := "git:" + strings.Repeat("a", 40) + ":docs/proposals/pending/p.md:" + strings.Repeat("b", 64) + ":build:autonomous"
	if err := store.CreateWorkItem(context.Background(), CreateWorkItem{
		ID: "wi_legacy_git", Repo: "repo", ProposalPath: legacy,
		WorkflowName: "build", StartStage: "draft",
	}); err != nil {
		t.Fatal(err)
	}
	item, err := store.WorkItemByGitProposal(context.Background(), "repo", strings.Repeat("b", 64), "build", "autonomous")
	if err != nil || item.ID != "wi_legacy_git" {
		t.Fatalf("legacy lookup item=%+v err=%v", item, err)
	}
}

func TestGitProposalIdentityPinsBlobWorkflowAndMode(t *testing.T) {
	if got, want := GitProposalIdentity(strings.Repeat("b", 64), "build", "autonomous"),
		"git:"+strings.Repeat("b", 64)+":build:autonomous"; got != want {
		t.Fatalf("identity=%q want=%q", got, want)
	}
}

func TestParkedRootStillConsumesAdmissionCapacity(t *testing.T) {
	store := newTestStore(t)
	ctx := context.Background()
	first := CreateWorkItem{ID: "wi_parked", Repo: "repo", ProposalPath: "parked", WorkflowName: "build", StartStage: "feature"}
	if err := store.AdmitRoot(ctx, first, 1); err != nil {
		t.Fatal(err)
	}
	if err := store.ParkWithDetail(ctx, first.ID, first.StartStage, "runner_unavailable", "fork/exec git: resource temporarily unavailable", 0); err != nil {
		t.Fatal(err)
	}
	second := CreateWorkItem{ID: "wi_waiting", Repo: "repo", ProposalPath: "waiting", WorkflowName: "build", StartStage: "draft"}
	if err := store.AdmitRoot(ctx, second, 1); !errors.Is(err, ErrAdmissionFull) {
		t.Fatalf("admit with parked active root: %v", err)
	}
	count, err := store.ActiveRootCount(ctx)
	if err != nil || count != 1 {
		t.Fatalf("count=%d err=%v", count, err)
	}
	events, err := store.Events(ctx, first.ID, 0, 10)
	if err != nil {
		t.Fatal(err)
	}
	last := events[len(events)-1]
	if last.Kind != "pause" || last.Detail != "fork/exec git: resource temporarily unavailable" {
		t.Fatalf("pause event=%+v", last)
	}
	item, err := store.WorkItem(ctx, first.ID)
	if err != nil || item.PauseReason != "runner_unavailable" {
		t.Fatalf("item=%+v err=%v", item, err)
	}
}

func TestExecutedTurnCountExcludesAdministrativeEvents(t *testing.T) {
	store := newTestStore(t)
	ctx := context.Background()
	createTestItem(t, store, "wi_turns")
	if err := store.Move(ctx, "wi_turns", "plan_gate", "plan", "advance", "approved", "", 0); err != nil {
		t.Fatal(err)
	}
	if err := store.Park(ctx, "wi_turns", "plan", "turn_cap", 0); err != nil {
		t.Fatal(err)
	}
	if err := store.Resume(ctx, "wi_turns"); err != nil {
		t.Fatal(err)
	}
	if parked, err := store.RecordRetry(ctx, "wi_turns", "plan", "plan", "refine", 3, 0); err != nil || parked {
		t.Fatalf("record retry: parked=%v err=%v", parked, err)
	}
	turns, err := store.ExecutedTurnCount(ctx, "wi_turns")
	if err != nil {
		t.Fatal(err)
	}
	if turns != 2 {
		t.Fatalf("turns=%d want=2 (advance + loop only)", turns)
	}
}

func TestWorkflowBudgetAggregatesChildrenAndParksWholeTree(t *testing.T) {
	store := newTestStore(t)
	ctx := context.Background()
	if err := store.CreateWorkItem(ctx, CreateWorkItem{ID: "wi_budget", Repo: "repo", ProposalPath: "budget", WorkflowName: "build", StartStage: "fanout", MaxCostUSD: 1}); err != nil {
		t.Fatal(err)
	}
	if err := store.CreateWorkItem(ctx, CreateWorkItem{ID: "wi_budget.child", Repo: "repo", ProposalPath: "packet", WorkflowName: "slice", StartStage: "impl", ParentID: "wi_budget"}); err != nil {
		t.Fatal(err)
	}
	if err := store.Move(ctx, "wi_budget.child", "impl", "review", "advance", "", "", .75); err != nil {
		t.Fatal(err)
	}
	root, spent, max, err := store.WorkflowBudget(ctx, "wi_budget.child")
	if err != nil {
		t.Fatal(err)
	}
	if root != "wi_budget" || spent != .75 || max != 1 {
		t.Fatalf("root=%s spent=%v max=%v", root, spent, max)
	}
	if err := store.Park(ctx, "wi_budget.child", "review", "human_gate", 0); err != nil {
		t.Fatal(err)
	}
	if err := store.ParkBudgetTree(ctx, root, .30); err != nil {
		t.Fatal(err)
	}
	for _, id := range []string{"wi_budget"} {
		item, err := store.WorkItem(ctx, id)
		if err != nil {
			t.Fatal(err)
		}
		if item.PauseReason != "budget_cap" {
			t.Fatalf("%s=%+v", id, item)
		}
	}
	child, err := store.WorkItem(ctx, "wi_budget.child")
	if err != nil || child.PauseReason != "human_gate" {
		t.Fatalf("pre-existing child pause was overwritten: %+v err=%v", child, err)
	}
	_, spent, _, _ = store.WorkflowBudget(ctx, root)
	if spent < 1.049 || spent > 1.051 {
		t.Fatalf("spent=%v", spent)
	}
}

func TestGenericResumeCannotBypassLifecycleOwnedPause(t *testing.T) {
	store := newTestStore(t)
	createTestItem(t, store, "wi_owned_pause")
	if err := store.Park(context.Background(), "wi_owned_pause", "plan_gate", "human_gate", 0); err != nil {
		t.Fatal(err)
	}
	if err := store.Resume(context.Background(), "wi_owned_pause"); err == nil {
		t.Fatal("generic resume bypassed human gate")
	}
	item, _ := store.WorkItem(context.Background(), "wi_owned_pause")
	if item.PauseReason != "human_gate" {
		t.Fatalf("item=%+v", item)
	}
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
			"plan-"+string(rune('a'+i)), "feedback-"+string(rune('a'+i)), 3, 3, 0)
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
			"same-plan", "same-feedback", 24, 3, 0)
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
			pair[0], pair[1], 24, 3, 0)
		if err != nil {
			t.Fatal(err)
		}
		if out.Parked || out.IdenticalRepeats != 1 {
			t.Fatalf("changed review was not recognized as progress: %+v", out)
		}
	}
}

func TestCancelUnassignedDelegateJobIsAtomicAndAssignmentSafe(t *testing.T) {
	store := newTestStore(t)
	_, err := store.db.Exec(`CREATE TABLE agent_jobs (
		id INTEGER PRIMARY KEY, agent_name TEXT NOT NULL DEFAULT '', status TEXT NOT NULL,
		cancelled_at TEXT DEFAULT '', cancel_reason TEXT DEFAULT '', updated_at TEXT DEFAULT '')`)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := store.db.Exec(`INSERT INTO agent_jobs(id,status,agent_name) VALUES
		(41,'pending',''),(42,'pending','codex'),(43,'running','codex'),(44,'pending',' '),
		(45,'running',''),(46,'done',''),(47,'failed',''),(48,'cancelled','')`); err != nil {
		t.Fatal(err)
	}
	for _, id := range []int{41, 44, 45} {
		cancelled, err := store.CancelUnassignedDelegateJob(t.Context(), id, "lease expired")
		if err != nil || !cancelled {
			t.Fatalf("cancel unassigned job %d: cancelled=%v err=%v", id, cancelled, err)
		}
	}
	for _, id := range []int{42, 43, 46, 47, 48} {
		cancelled, err := store.CancelUnassignedDelegateJob(t.Context(), id, "lease expired")
		if err != nil || cancelled {
			t.Fatalf("job %d assignment/terminal guard: cancelled=%v err=%v", id, cancelled, err)
		}
	}
	for _, id := range []int{41, 44, 45} {
		var status, reason string
		if err := store.db.QueryRow(`SELECT status,cancel_reason FROM agent_jobs WHERE id=?`, id).Scan(&status, &reason); err != nil {
			t.Fatal(err)
		}
		if status != "cancelled" || reason != "lease expired" {
			t.Fatalf("job %d status=%q reason=%q", id, status, reason)
		}
	}
}

func TestForgetDelegateJobIfMatchesCannotEraseNewerRetry(t *testing.T) {
	store := newTestStore(t)
	const key = "same-logical-seat"
	if err := store.SaveDelegateJob(t.Context(), key, 51); err != nil {
		t.Fatal(err)
	}
	if err := store.SaveDelegateJob(t.Context(), key, 52); err != nil {
		t.Fatal(err)
	}
	forgot, err := store.ForgetDelegateJobIfMatches(t.Context(), key, 51)
	if err != nil || forgot {
		t.Fatalf("stale cleanup forgot=%v err=%v", forgot, err)
	}
	if jobID, err := store.DelegateJob(t.Context(), key); err != nil || jobID != 52 {
		t.Fatalf("newer retry mapping job=%d err=%v", jobID, err)
	}
	forgot, err = store.ForgetDelegateJobIfMatches(t.Context(), key, 52)
	if err != nil || !forgot {
		t.Fatalf("matching cleanup forgot=%v err=%v", forgot, err)
	}
	if _, err := store.DelegateJob(t.Context(), key); err == nil {
		t.Fatal("matching cleanup retained mapping")
	}
}
