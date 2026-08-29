package db1_test

import (
	"context"
	"errors"
	"fmt"
	"math"
	"path/filepath"
	"strings"
	"sync"
	"testing"

	"github.com/JBailes/aimee/server-go/internal/db1"
	"github.com/JBailes/aimee/server-go/internal/db1/db1test"
)

// newTestStore starts a real module for this test and returns a store on it,
// plus the path that store lives at -- some tests have to reach around the
// module to move the clock, and db1test.Exec needs to know where.
// The batch size TerminalDelegateJobs claims per sweep. Spelled here because
// this is an external test package now; it must match the unexported constant,
// and the test that uses it asserts the batch is bounded, so a drift shows up
// as a failure rather than as a silently weaker assertion.
const terminalCancellationBatchSize = 8

func newTestStore(t *testing.T) (*db1.Store, string) {
	t.Helper()
	storePath := filepath.Join(t.TempDir(), "aimee.db")
	store, err := db1test.Open(t, storePath)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = store.Close() })
	return store, storePath
}

func TestTerminalDelegateJobsReturnsBoundedBatch(t *testing.T) {
	store, storePath := newTestStore(t)
	for i := 1; i <= terminalCancellationBatchSize+2; i++ {
		workItemID := fmt.Sprintf("wi_terminal_batch_%02d", i)
		if err := store.CreateWorkItem(t.Context(), db1.CreateWorkItem{ID: workItemID, Repo: "repo", ProposalPath: workItemID, WorkflowName: "build", StartStage: "impl"}); err != nil {
			t.Fatal(err)
		}
		if err := store.SaveWorkflowDelegateJob(t.Context(), workItemID+":impl:v1:hash", workItemID, i, ""); err != nil {
			t.Fatal(err)
		}
		db1test.Exec(t, storePath, `INSERT INTO agent_jobs(id,status) VALUES(?,'running')`, i)
		if _, err := store.StopTree(t.Context(), workItemID); err != nil {
			t.Fatal(err)
		}
	}
	mappings, err := store.TerminalDelegateJobs(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	if len(mappings) != terminalCancellationBatchSize {
		t.Fatalf("mappings=%d want bounded batch %d", len(mappings), terminalCancellationBatchSize)
	}
	second, err := store.TerminalDelegateJobs(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	if len(second) != terminalCancellationBatchSize || second[0].JobID != terminalCancellationBatchSize+1 || second[1].JobID != terminalCancellationBatchSize+2 {
		t.Fatalf("second batch did not rotate past failed jobs: %+v", second)
	}
}

func TestPausedParentIsNotReconciledOrCancelledAsTerminal(t *testing.T) {
	store, storePath := newTestStore(t)
	ctx := t.Context()
	if err := store.CreateWorkItem(ctx, db1.CreateWorkItem{ID: "wi_paused_parent", Repo: "repo", ProposalPath: "parent", WorkflowName: "build", StartStage: "slices"}); err != nil {
		t.Fatal(err)
	}
	if err := store.CreateWorkItem(ctx, db1.CreateWorkItem{ID: "wi_paused_child", Repo: "repo", ProposalPath: "child", WorkflowName: "slice", StartStage: "impl", ParentID: "wi_paused_parent"}); err != nil {
		t.Fatal(err)
	}
	if err := store.Park(ctx, "wi_paused_parent", "slices", "manual", 0); err != nil {
		t.Fatal(err)
	}
	if err := store.SaveWorkflowDelegateJob(ctx, "wi_paused_parent:slices:v1:hash", "wi_paused_parent", 81, ""); err != nil {
		t.Fatal(err)
	}
	db1test.Exec(t, storePath, `INSERT INTO agent_jobs(id,status) VALUES(81,'running')`)
	orphans, err := store.ReconcileOrphanedDescendants(ctx)
	if err != nil || len(orphans) != 0 {
		t.Fatalf("paused parent reconciled descendants: ids=%v err=%v", orphans, err)
	}
	mappings, err := store.TerminalDelegateJobs(ctx)
	if err != nil || len(mappings) != 0 {
		t.Fatalf("paused parent selected for cancellation: mappings=%v err=%v", mappings, err)
	}
}

func TestStopTreeStopsPausedRootAndPausedDescendant(t *testing.T) {
	store, _ := newTestStore(t)
	ctx := t.Context()
	for _, item := range []db1.CreateWorkItem{
		{ID: "wi_paused_tree", Repo: "repo", ProposalPath: "paused-root", WorkflowName: "build", StartStage: "slices"},
		{ID: "wi_paused_tree.child", Repo: "repo", ProposalPath: "paused-child", WorkflowName: "slice", StartStage: "impl", ParentID: "wi_paused_tree"},
	} {
		if err := store.CreateWorkItem(ctx, item); err != nil {
			t.Fatal(err)
		}
		if err := store.Park(ctx, item.ID, item.StartStage, "manual", 0); err != nil {
			t.Fatal(err)
		}
	}
	stopped, err := store.StopTree(ctx, "wi_paused_tree")
	if err != nil || len(stopped) != 2 {
		t.Fatalf("stop paused tree: ids=%v err=%v", stopped, err)
	}
	for _, id := range stopped {
		item, err := store.WorkItem(ctx, id)
		if err != nil || item.State != "stopped" || item.PauseReason != "" {
			t.Fatalf("paused member not stopped: item=%+v err=%v", item, err)
		}
	}
}

func TestConcurrentRootAdmissionNeverExceedsCap(t *testing.T) {
	store, _ := newTestStore(t)
	const attempts = 12
	const cap = 2
	var wg sync.WaitGroup
	var admitted int
	var mu sync.Mutex
	for i := 0; i < attempts; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			err := store.AdmitRoot(context.Background(), db1.CreateWorkItem{ID: fmt.Sprintf("wi_admit_%d", i), Repo: "repo", ProposalPath: fmt.Sprintf("p-%d", i), WorkflowName: "build", StartStage: "start"}, cap)
			if err == nil {
				mu.Lock()
				admitted++
				mu.Unlock()
				return
			}
			if !errors.Is(err, db1.ErrAdmissionFull) {
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
	store, _ := newTestStore(t)
	legacy := "git:" + strings.Repeat("a", 40) + ":docs/proposals/pending/p.md:" + strings.Repeat("b", 64) + ":build:autonomous"
	if err := store.CreateWorkItem(context.Background(), db1.CreateWorkItem{
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
	if got, want := db1.GitProposalIdentity(strings.Repeat("b", 64), "build", "autonomous"),
		"git:"+strings.Repeat("b", 64)+":build:autonomous"; got != want {
		t.Fatalf("identity=%q want=%q", got, want)
	}
}

func TestParkedRootStillConsumesAdmissionCapacity(t *testing.T) {
	store, _ := newTestStore(t)
	ctx := context.Background()
	first := db1.CreateWorkItem{ID: "wi_parked", Repo: "repo", ProposalPath: "parked", WorkflowName: "build", StartStage: "feature"}
	if err := store.AdmitRoot(ctx, first, 1); err != nil {
		t.Fatal(err)
	}
	if err := store.ParkWithDetail(ctx, first.ID, first.StartStage, "runner_unavailable", "fork/exec git: resource temporarily unavailable", 0); err != nil {
		t.Fatal(err)
	}
	second := db1.CreateWorkItem{ID: "wi_waiting", Repo: "repo", ProposalPath: "waiting", WorkflowName: "build", StartStage: "draft"}
	if err := store.AdmitRoot(ctx, second, 1); !errors.Is(err, db1.ErrAdmissionFull) {
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
	store, _ := newTestStore(t)
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

func TestRetryLimitResumeStartsFreshBudgetAndKeepsDiagnostic(t *testing.T) {
	store, _ := newTestStore(t)
	ctx := context.Background()
	if err := store.CreateWorkItem(ctx, db1.CreateWorkItem{ID: "wi_retry_resume", Repo: "repo", ProposalPath: "retry", WorkflowName: "build", StartStage: "impl"}); err != nil {
		t.Fatal(err)
	}
	if parked, err := store.RecordRetry(ctx, "wi_retry_resume", "impl", "impl", "verify failed: stale docs", 1, 0); err != nil || !parked {
		t.Fatalf("initial retry: parked=%v err=%v", parked, err)
	}
	if err := store.Resume(ctx, "wi_retry_resume"); err != nil {
		t.Fatal(err)
	}
	if attempts, err := store.StageAttemptCount(ctx, "wi_retry_resume", "impl"); err != nil || attempts != 0 {
		t.Fatalf("attempts after retry-limit resume=%d err=%v, want fresh budget", attempts, err)
	}
	if detail, err := store.LatestStageRetryDetail(ctx, "wi_retry_resume", "impl"); err != nil || detail != "verify failed: stale docs" {
		t.Fatalf("retry detail=%q err=%v", detail, err)
	}
	if parked, err := store.RecordRetry(ctx, "wi_retry_resume", "impl", "impl", "verify failed again", 3, 0); err != nil || parked {
		t.Fatalf("first retry in fresh budget: parked=%v err=%v", parked, err)
	}
	if attempts, err := store.StageAttemptCount(ctx, "wi_retry_resume", "impl"); err != nil || attempts != 1 {
		t.Fatalf("attempts after fresh failure=%d err=%v", attempts, err)
	}
	if detail, err := store.LatestStageRetryDetail(ctx, "wi_retry_resume", "impl"); err != nil || detail != "verify failed again" {
		t.Fatalf("new retry detail=%q err=%v", detail, err)
	}
	if err := store.Move(ctx, "wi_retry_resume", "impl", "review", "advance", "verified", "hash", 0); err != nil {
		t.Fatal(err)
	}
	if detail, err := store.LatestStageRetryDetail(ctx, "wi_retry_resume", "impl"); err != nil || detail != "" {
		t.Fatalf("completed-stage retry detail=%q err=%v, want stale detail cleared", detail, err)
	}
}

func TestWorkflowBudgetHeartbeatExtendsReplayReservations(t *testing.T) {
	for _, state := range []string{"actual", "unresolved"} {
		t.Run(state, func(t *testing.T) {
			store, storePath := newTestStore(t)
			createTestItem(t, store, "wi_heartbeat_"+state)
			id := "wi_heartbeat_" + state
			db1test.Exec(t, storePath, `UPDATE lifecycle_work_item
SET reservation_state=?,reservation_owner='owner',reservation_lease_until=datetime('now','-1 minute')
WHERE work_item_id=?`, state, id)
			if err := store.HeartbeatWorkflowBudget(t.Context(), id, "owner"); err != nil {
				t.Fatal(err)
			}
			if live := db1test.QueryInt(t, storePath,
				`SELECT reservation_lease_until > datetime('now') `+
					`FROM lifecycle_work_item WHERE work_item_id=?`, id); live != 1 {
				t.Fatalf("the heartbeat did not extend the lease (live=%d)", live)
			}
		})
	}
}

func TestTransientPauseIsNotRepairContext(t *testing.T) {
	store, _ := newTestStore(t)
	ctx := context.Background()
	if err := store.CreateWorkItem(ctx, db1.CreateWorkItem{ID: "wi_transient", Repo: "repo", ProposalPath: "transient", WorkflowName: "build", StartStage: "impl"}); err != nil {
		t.Fatal(err)
	}
	if err := store.ParkWithDetail(ctx, "wi_transient", "impl", "runner_unavailable", "delegate service restarting", 0); err != nil {
		t.Fatal(err)
	}
	if resumed, err := store.ResumeTransient(ctx, "runner_unavailable", 0); err != nil || resumed != 1 {
		t.Fatalf("resume transient=%d err=%v", resumed, err)
	}
	if detail, err := store.LatestStageRetryDetail(ctx, "wi_transient", "impl"); err != nil || detail != "" {
		t.Fatalf("transient pause leaked as repair detail=%q err=%v", detail, err)
	}
}

func TestWorkflowBudgetAggregatesChildrenAndParksWholeTree(t *testing.T) {
	store, _ := newTestStore(t)
	ctx := context.Background()
	if err := store.CreateWorkItem(ctx, db1.CreateWorkItem{ID: "wi_budget", Repo: "repo", ProposalPath: "budget", WorkflowName: "build", StartStage: "fanout", MaxCostUSD: 1}); err != nil {
		t.Fatal(err)
	}
	if err := store.CreateWorkItem(ctx, db1.CreateWorkItem{ID: "wi_budget.child", Repo: "repo", ProposalPath: "packet", WorkflowName: "slice", StartStage: "impl", ParentID: "wi_budget"}); err != nil {
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
	if err := store.ParkBudgetTree(ctx, root, "wi_budget.child", .30); err != nil {
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
	store, _ := newTestStore(t)
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

func TestReplayUnrecoverableCanBeResumedByOperator(t *testing.T) {
	store, _ := newTestStore(t)
	createTestItem(t, store, "wi_replay_operator")
	if err := store.Park(context.Background(), "wi_replay_operator", "plan_gate", "replay_unrecoverable", 0); err != nil {
		t.Fatal(err)
	}
	if err := store.Resume(context.Background(), "wi_replay_operator"); err != nil {
		t.Fatal(err)
	}
	item, err := store.WorkItem(context.Background(), "wi_replay_operator")
	if err != nil || item.PauseReason != "" || item.State != "active" {
		t.Fatalf("item=%+v err=%v", item, err)
	}
}

func TestBaseIntegrationConflictCanBeResumedAfterOperatorRepair(t *testing.T) {
	store, _ := newTestStore(t)
	createTestItem(t, store, "wi_base_conflict")
	if err := store.Park(context.Background(), "wi_base_conflict", "plan_gate", "base_integration_conflict", 0); err != nil {
		t.Fatal(err)
	}
	if err := store.Resume(context.Background(), "wi_base_conflict"); err != nil {
		t.Fatalf("resume repaired base integration conflict: %v", err)
	}
	item, err := store.WorkItem(context.Background(), "wi_base_conflict")
	if err != nil || item.PauseReason != "" || item.State != "active" || item.Stage != "plan_gate" {
		t.Fatalf("item=%+v err=%v", item, err)
	}
}

func TestStopTerminalizesActiveWorkflowTree(t *testing.T) {
	store, _ := newTestStore(t)
	ctx := context.Background()
	for _, in := range []db1.CreateWorkItem{
		{ID: "wi_stop_tree", Repo: "repo", ProposalPath: "root", WorkflowName: "build", StartStage: "slices"},
		{ID: "wi_stop_tree.child", Repo: "repo", ProposalPath: "child", WorkflowName: "slice", StartStage: "impl", ParentID: "wi_stop_tree"},
		{ID: "wi_stop_tree.grandchild", Repo: "repo", ProposalPath: "grandchild", WorkflowName: "slice", StartStage: "impl", ParentID: "wi_stop_tree.child"},
	} {
		if err := store.CreateWorkItem(ctx, in); err != nil {
			t.Fatal(err)
		}
	}
	stopped, err := store.StopTree(ctx, "wi_stop_tree")
	if err != nil {
		t.Fatal(err)
	}
	if got := strings.Join(stopped, ","); got != "wi_stop_tree,wi_stop_tree.child,wi_stop_tree.grandchild" {
		t.Fatalf("stopped IDs=%q", got)
	}
	for _, id := range []string{"wi_stop_tree", "wi_stop_tree.child", "wi_stop_tree.grandchild"} {
		item, err := store.WorkItem(ctx, id)
		if err != nil {
			t.Fatal(err)
		}
		if item.State != "stopped" {
			t.Fatalf("%s state=%q want stopped", id, item.State)
		}
		events, err := store.Events(ctx, id, 0, 20)
		if err != nil {
			t.Fatal(err)
		}
		last := events[len(events)-1]
		if last.Kind != "terminal" || last.Detail != "operator_stop" {
			t.Fatalf("%s last event=%+v", id, last)
		}
	}
}

func TestCreateChildRejectsTerminalParent(t *testing.T) {
	store, _ := newTestStore(t)
	ctx := context.Background()
	if err := store.CreateWorkItem(ctx, db1.CreateWorkItem{ID: "wi_terminal_parent", Repo: "repo", ProposalPath: "root", WorkflowName: "build", StartStage: "slices"}); err != nil {
		t.Fatal(err)
	}
	if err := store.Stop(ctx, "wi_terminal_parent"); err != nil {
		t.Fatal(err)
	}
	err := store.CreateWorkItem(ctx, db1.CreateWorkItem{ID: "wi_late_child", Repo: "repo", ProposalPath: "child", WorkflowName: "slice", StartStage: "impl", ParentID: "wi_terminal_parent"})
	if err == nil || !strings.Contains(err.Error(), "parent work item is not active") {
		t.Fatalf("create child under terminal parent: %v", err)
	}
	if _, err := store.WorkItem(ctx, "wi_late_child"); err == nil {
		t.Fatal("late child was persisted")
	}
}

func TestCreateChildRacingStopTreeCannotLeaveActiveOrphan(t *testing.T) {
	store, _ := newTestStore(t)
	ctx := context.Background()
	for iteration := 0; iteration < 50; iteration++ {
		rootID := fmt.Sprintf("wi_stop_race_%d", iteration)
		childID := rootID + ".child"
		if err := store.CreateWorkItem(ctx, db1.CreateWorkItem{ID: rootID, Repo: "repo", ProposalPath: rootID, WorkflowName: "build", StartStage: "slices"}); err != nil {
			t.Fatal(err)
		}
		start := make(chan struct{})
		var stopErr, createErr error
		var wg sync.WaitGroup
		wg.Add(2)
		go func() {
			defer wg.Done()
			<-start
			_, stopErr = store.StopTree(ctx, rootID)
		}()
		go func() {
			defer wg.Done()
			<-start
			createErr = store.CreateWorkItem(ctx, db1.CreateWorkItem{ID: childID, Repo: "repo", ProposalPath: childID, WorkflowName: "slice", StartStage: "impl", ParentID: rootID})
		}()
		close(start)
		wg.Wait()
		if stopErr != nil {
			t.Fatalf("iteration %d stop: %v", iteration, stopErr)
		}
		child, err := store.WorkItem(ctx, childID)
		switch {
		case createErr == nil && err != nil:
			t.Fatalf("iteration %d successful child creation missing: %v", iteration, err)
		case createErr == nil && child.State != "stopped":
			t.Fatalf("iteration %d child state=%q want stopped", iteration, child.State)
		case createErr != nil && err == nil:
			t.Fatalf("iteration %d failed child creation persisted item=%+v: %v", iteration, child, createErr)
		case createErr != nil && !strings.Contains(createErr.Error(), "parent work item is not active"):
			t.Fatalf("iteration %d unexpected child error: %v", iteration, createErr)
		}
	}
}

func TestReconcileOrphanedDescendantsStopsCorruptActiveTree(t *testing.T) {
	store, storePath := newTestStore(t)
	ctx := context.Background()
	for _, in := range []db1.CreateWorkItem{
		{ID: "wi_orphan_root", Repo: "repo", ProposalPath: "root", WorkflowName: "build", StartStage: "slices"},
		{ID: "wi_orphan_child", Repo: "repo", ProposalPath: "child", WorkflowName: "slice", StartStage: "impl", ParentID: "wi_orphan_root"},
		{ID: "wi_orphan_grandchild", Repo: "repo", ProposalPath: "grandchild", WorkflowName: "slice", StartStage: "impl", ParentID: "wi_orphan_child"},
	} {
		if err := store.CreateWorkItem(ctx, in); err != nil {
			t.Fatal(err)
		}
	}
	// Reproduce state written by the old non-cascading Stop implementation.
	db1test.Exec(t, storePath, `UPDATE lifecycle_work_item SET state='stopped' WHERE work_item_id='wi_orphan_root'`)
	stopped, err := store.ReconcileOrphanedDescendants(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if got := strings.Join(stopped, ","); got != "wi_orphan_child,wi_orphan_grandchild" {
		t.Fatalf("stopped=%q", got)
	}
	for _, id := range []string{"wi_orphan_child", "wi_orphan_grandchild"} {
		item, err := store.WorkItem(ctx, id)
		if err != nil || item.State != "stopped" {
			t.Fatalf("%s item=%+v err=%v", id, item, err)
		}
		events, err := store.Events(ctx, id, 0, 20)
		if err != nil {
			t.Fatal(err)
		}
		last := events[len(events)-1]
		if last.Kind != "terminal" || last.Detail != "ancestor_terminal" {
			t.Fatalf("%s last event=%+v", id, last)
		}
	}
}

func createTestItem(t *testing.T, store *db1.Store, id string) {
	t.Helper()
	err := store.CreateWorkItem(context.Background(), db1.CreateWorkItem{
		ID: id, Repo: "repo", ProposalPath: id + ".md", WorkflowName: "build",
		WorkflowVersion: strings.Repeat("f", 64), StartStage: "plan_gate", Mode: "autonomous",
	})
	if err != nil {
		t.Fatal(err)
	}
}

func TestMaxIterationsParksWithoutAbandoning(t *testing.T) {
	store, _ := newTestStore(t)
	createTestItem(t, store, "wi_cap")
	ctx := context.Background()

	for i := 0; i < 3; i++ {
		out, err := store.RecordRequestedChanges(ctx, "wi_cap", "plan_gate", "plan",
			"plan-"+string(rune('a'+i)), "feedback-"+string(rune('a'+i)), "subject anchor never defined", 3, 3, 0)
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
	store, _ := newTestStore(t)
	createTestItem(t, store, "wi_repeat")
	ctx := context.Background()
	for i := 0; i < 3; i++ {
		out, err := store.RecordRequestedChanges(ctx, "wi_repeat", "plan_gate", "plan",
			"same-plan", "same-feedback", "", 24, 3, 0)
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
	store, _ := newTestStore(t)
	createTestItem(t, store, "wi_progress")
	ctx := context.Background()
	cases := [][2]string{{"plan-a", "feedback-a"}, {"plan-b", "feedback-a"}, {"plan-b", "feedback-b"}}
	for _, pair := range cases {
		out, err := store.RecordRequestedChanges(ctx, "wi_progress", "plan_gate", "plan",
			pair[0], pair[1], "", 24, 3, 0)
		if err != nil {
			t.Fatal(err)
		}
		if out.Parked || out.IdenticalRepeats != 1 {
			t.Fatalf("changed review was not recognized as progress: %+v", out)
		}
	}
}

func TestStructuredBlockerSetsRequireStrictShrinkageForProgress(t *testing.T) {
	store, _ := newTestStore(t)
	createTestItem(t, store, "wi_blocker_sets")
	ctx := context.Background()
	a := strings.Repeat("a", 64)
	b := strings.Repeat("b", 64)
	c := strings.Repeat("c", 64)
	payload := func(set string) string {
		return fmt.Sprintf(`{"version":1,"mode":"enforce","summary":"unresolved","blocker_set":%q}`, set)
	}
	cases := []struct {
		set         string
		wantRepeats int
	}{
		{a + "," + b, 1}, // baseline
		{a + "," + b, 2}, // equal, despite changed hashes below
		{a, 1},           // strict subset is demonstrated progress
		{b, 2},           // swapped blocker is churn
		{b + "," + c, 3}, // superset is regression
	}
	for i, tc := range cases {
		out, err := store.RecordRequestedChanges(ctx, "wi_blocker_sets", "plan_gate", "plan",
			fmt.Sprintf("plan-%d", i), fmt.Sprintf("feedback-%d", i), payload(tc.set), 24, 9, 0)
		if err != nil {
			t.Fatal(err)
		}
		if out.IdenticalRepeats != tc.wantRepeats || out.Parked {
			t.Fatalf("round %d outcome=%+v, want repeats=%d", i+1, out, tc.wantRepeats)
		}
	}
}

func TestStructuredBlockerSetsRemainShadowOnlyInObserveMode(t *testing.T) {
	store, _ := newTestStore(t)
	createTestItem(t, store, "wi_blocker_shadow")
	ctx := context.Background()
	set := strings.Repeat("a", 64)
	payload := fmt.Sprintf(`{"version":1,"mode":"observe","summary":"unresolved","blocker_set":%q}`, set)
	for i := 0; i < 2; i++ {
		out, err := store.RecordRequestedChanges(ctx, "wi_blocker_shadow", "plan_gate", "plan",
			fmt.Sprintf("plan-%d", i), fmt.Sprintf("feedback-%d", i), payload, 24, 2, 0)
		if err != nil {
			t.Fatal(err)
		}
		if out.Parked || out.IdenticalRepeats != 1 {
			t.Fatalf("observe-only classification affected routing: %+v", out)
		}
	}
}

func TestEnforcedBlockerConvergenceFallsBackWhenStructureIsUnavailable(t *testing.T) {
	store, _ := newTestStore(t)
	createTestItem(t, store, "wi_blocker_fallback")
	ctx := context.Background()
	payload := `{"version":1,"mode":"enforce","summary":"unresolved","blocker_set":""}`
	for i := 0; i < 2; i++ {
		out, err := store.RecordRequestedChanges(ctx, "wi_blocker_fallback", "plan_gate", "plan",
			"same-plan", "same-feedback", payload, 24, 2, 0)
		if err != nil {
			t.Fatal(err)
		}
		if out.IdenticalRepeats != i+1 {
			t.Fatalf("round %d repeats=%d, want %d", i+1, out.IdenticalRepeats, i+1)
		}
		if out.Parked != (i == 1) {
			t.Fatalf("round %d parked=%v", i+1, out.Parked)
		}
	}
}

func TestConcurrentStoreOpenDoesNotClearLiveBudgetLease(t *testing.T) {
	path := filepath.Join(t.TempDir(), "aimee.db")
	first, err := db1test.Open(t, path)
	if err != nil {
		t.Fatal(err)
	}
	defer first.Close()
	item := db1.CreateWorkItem{ID: "wi_live_lease", Repo: "repo", ProposalPath: "live-lease", WorkflowName: "build", WorkflowVersion: "v1", StartStage: "work", MaxCostUSD: 1}
	if err := first.CreateWorkItem(t.Context(), item); err != nil {
		t.Fatal(err)
	}
	if _, err := first.ReserveWorkflowBudget(t.Context(), item.ID, "live-owner"); err != nil {
		t.Fatal(err)
	}
	second, err := db1test.Open(t, path)
	if err != nil {
		t.Fatal(err)
	}
	defer second.Close()
	reservation, err := second.ReserveWorkflowBudget(t.Context(), item.ID, "competing-owner")
	if err != nil || !reservation.Busy || reservation.Allowed {
		t.Fatalf("reservation=%+v err=%v", reservation, err)
	}
}

func TestExpiredPostDispatchLeaseBecomesReplayOnlyAfterRestart(t *testing.T) {
	path := filepath.Join(t.TempDir(), "aimee.db")
	first, err := db1test.Open(t, path)
	if err != nil {
		t.Fatal(err)
	}
	item := db1.CreateWorkItem{ID: "wi_expired_lease", Repo: "repo", ProposalPath: "expired-lease", WorkflowName: "build", WorkflowVersion: "v1", StartStage: "work", MaxCostUSD: .75}
	if err := first.CreateWorkItem(t.Context(), item); err != nil {
		t.Fatal(err)
	}
	initial, err := first.ReserveWorkflowBudget(t.Context(), item.ID, "dead-owner")
	if err != nil {
		t.Fatal(err)
	}
	db1test.Exec(t, path, `UPDATE lifecycle_work_item SET reservation_lease_until=CURRENT_TIMESTAMP - INTERVAL '1 minute' WHERE work_item_id=?`, item.ID)
	if err := first.Close(); err != nil {
		t.Fatal(err)
	}
	restarted, err := db1test.Open(t, path)
	if err != nil {
		t.Fatal(err)
	}
	defer restarted.Close()
	replay, err := restarted.ReserveWorkflowBudget(t.Context(), item.ID, "restart-owner")
	if err != nil || !replay.ReplayOnly || replay.Amount != initial.Amount {
		t.Fatalf("replay=%+v initial=%+v err=%v", replay, initial, err)
	}
}

func TestBudgetTreeParkPreservesAndConsumesInflightSibling(t *testing.T) {
	store, _ := newTestStore(t)
	ctx := t.Context()
	for _, item := range []db1.CreateWorkItem{
		{ID: "wi_park_root", Repo: "repo", ProposalPath: "park-root", WorkflowName: "build", StartStage: "fanout", MaxCostUSD: 1},
		{ID: "wi_park_a", Repo: "repo", ProposalPath: "park-a", WorkflowName: "slice", StartStage: "work", ParentID: "wi_park_root"},
		{ID: "wi_park_b", Repo: "repo", ProposalPath: "park-b", WorkflowName: "slice", StartStage: "work", ParentID: "wi_park_root"},
	} {
		if err := store.CreateWorkItem(ctx, item); err != nil {
			t.Fatal(err)
		}
	}
	a, err := store.ReserveWorkflowBudget(ctx, "wi_park_a", "owner-a")
	if err != nil {
		t.Fatal(err)
	}
	b, err := store.ReserveWorkflowBudget(ctx, "wi_park_b", "owner-b")
	if err != nil || a.Amount+b.Amount > 1 {
		t.Fatalf("a=%+v b=%+v err=%v", a, b, err)
	}
	if allowed, err := store.ReconcileWorkflowBudget(ctx, "wi_park_a", "owner-a", .7); err != nil || allowed {
		t.Fatalf("overspend allowed=%v err=%v", allowed, err)
	}
	if err := store.ParkBudgetTree(ctx, "wi_park_root", "wi_park_a", .7); err != nil {
		t.Fatal(err)
	}
	inflight, err := store.WorkItem(ctx, "wi_park_b")
	if err != nil || inflight.PauseReason != "" || inflight.ReservedCostUSD != b.Amount {
		t.Fatalf("in-flight sibling was destroyed: %+v err=%v", inflight, err)
	}
	if allowed, err := store.ReconcileWorkflowBudget(ctx, "wi_park_b", "owner-b", .3); err != nil || !allowed {
		t.Fatalf("sibling reconcile allowed=%v err=%v", allowed, err)
	}
	if err := store.Move(ctx, "wi_park_b", "work", "done", "advance", "", "", .3); err != nil {
		t.Fatal(err)
	}
	_, spent, _, err := store.WorkflowBudget(ctx, "wi_park_root")
	if err != nil || spent != 1 {
		t.Fatalf("spent=%v err=%v", spent, err)
	}
}

func TestBudgetTreeParkPreservesReconciledSiblingAcrossStores(t *testing.T) {
	path := filepath.Join(t.TempDir(), "aimee.db")
	first, err := db1test.Open(t, path)
	if err != nil {
		t.Fatal(err)
	}
	defer first.Close()
	second, err := db1test.Open(t, path)
	if err != nil {
		t.Fatal(err)
	}
	defer second.Close()
	ctx := t.Context()
	for _, item := range []db1.CreateWorkItem{
		{ID: "wi_xproc_root", Repo: "repo", ProposalPath: "xproc-root", WorkflowName: "build", StartStage: "fanout", MaxCostUSD: 1},
		{ID: "wi_xproc_a", Repo: "repo", ProposalPath: "xproc-a", WorkflowName: "slice", StartStage: "work", ParentID: "wi_xproc_root"},
		{ID: "wi_xproc_b", Repo: "repo", ProposalPath: "xproc-b", WorkflowName: "slice", StartStage: "work", ParentID: "wi_xproc_root"},
	} {
		if err := first.CreateWorkItem(ctx, item); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := first.ReserveWorkflowBudget(ctx, "wi_xproc_a", "owner-a"); err != nil {
		t.Fatal(err)
	}
	if _, err := second.ReserveWorkflowBudget(ctx, "wi_xproc_b", "owner-b"); err != nil {
		t.Fatal(err)
	}
	if allowed, err := first.ReconcileWorkflowBudget(ctx, "wi_xproc_a", "owner-a", .7); err != nil || allowed {
		t.Fatalf("a allowed=%v err=%v", allowed, err)
	}
	if allowed, err := second.ReconcileWorkflowBudget(ctx, "wi_xproc_b", "owner-b", .3); err != nil || !allowed {
		t.Fatalf("b allowed=%v err=%v", allowed, err)
	}
	// This is the cross-process window: B has measured and reconciled spend but
	// has not committed its lifecycle transition when A parks the capped tree.
	if err := first.ParkBudgetTree(ctx, "wi_xproc_root", "wi_xproc_a", .7); err != nil {
		t.Fatal(err)
	}
	b, err := second.WorkItem(ctx, "wi_xproc_b")
	if err != nil || b.PauseReason != "" || b.ReservedCostUSD != .3 || b.ReservationState != "actual" {
		t.Fatalf("reconciled sibling=%+v err=%v", b, err)
	}
	if err := second.Move(ctx, "wi_xproc_b", "work", "done", "advance", "", "", .3); err != nil {
		t.Fatal(err)
	}
	_, spent, _, err := first.WorkflowBudget(ctx, "wi_xproc_root")
	if err != nil || spent != 1 {
		t.Fatalf("spent=%v err=%v", spent, err)
	}
}

func TestZeroCostReconciliationRequiresReplayAfterRestart(t *testing.T) {
	path := filepath.Join(t.TempDir(), "aimee.db")
	store, err := db1test.Open(t, path)
	if err != nil {
		t.Fatal(err)
	}
	item := db1.CreateWorkItem{ID: "wi_zero_replay", Repo: "repo", ProposalPath: "zero-replay", WorkflowName: "build", StartStage: "work"}
	if err := store.CreateWorkItem(t.Context(), item); err != nil {
		t.Fatal(err)
	}
	if reservation, err := store.ReserveWorkflowBudget(t.Context(), item.ID, "first"); err != nil || !reservation.Allowed {
		t.Fatalf("reservation=%+v err=%v", reservation, err)
	}
	if allowed, err := store.ReconcileWorkflowBudget(t.Context(), item.ID, "first", 0); err != nil || !allowed {
		t.Fatalf("allowed=%v err=%v", allowed, err)
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}
	store, err = db1test.Open(t, path)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	// Replay ownership is leased. While the reconciling owner's lease is live a
	// competing invocation is refused rather than allowed to replay in parallel.
	if busy, err := store.ReserveWorkflowBudget(t.Context(), item.ID, "second"); err != nil || !busy.Busy || busy.Allowed || busy.ReplayOnly {
		t.Fatalf("busy=%+v err=%v", busy, err)
	}
	db1test.Exec(t, path, `UPDATE lifecycle_work_item SET reservation_lease_until=CURRENT_TIMESTAMP - INTERVAL '1 minute' WHERE work_item_id=?`, item.ID)
	replay, err := store.ReserveWorkflowBudget(t.Context(), item.ID, "second")
	if err != nil || !replay.ReplayOnly || replay.Amount != 0 {
		t.Fatalf("replay=%+v err=%v", replay, err)
	}
	if allowed, err := store.ReconcileWorkflowBudget(t.Context(), item.ID, "second", 0); err != nil || !allowed {
		t.Fatalf("replay allowed=%v err=%v", allowed, err)
	}
	if err := store.Move(t.Context(), item.ID, "work", "done", "advance", "", "", 0); err != nil {
		t.Fatal(err)
	}
	got, err := store.WorkItem(t.Context(), item.ID)
	if err != nil || got.ReservationState != "" || got.ReservedCostUSD != 0 {
		t.Fatalf("item=%+v err=%v", got, err)
	}
}

// A crash between a denied reconciliation and ParkBudgetTree must not let the
// restarted replay commit the over-budget transition.
func TestDeniedReconciliationStaysDeniedAcrossRestart(t *testing.T) {
	path := filepath.Join(t.TempDir(), "aimee.db")
	first, err := db1test.Open(t, path)
	if err != nil {
		t.Fatal(err)
	}
	ctx := t.Context()
	for _, item := range []db1.CreateWorkItem{
		{ID: "wi_deny_root", Repo: "repo", ProposalPath: "deny-root", WorkflowName: "build", StartStage: "fanout", MaxCostUSD: 1},
		{ID: "wi_deny_a", Repo: "repo", ProposalPath: "deny-a", WorkflowName: "slice", StartStage: "work", ParentID: "wi_deny_root"},
		{ID: "wi_deny_b", Repo: "repo", ProposalPath: "deny-b", WorkflowName: "slice", StartStage: "work", ParentID: "wi_deny_root"},
	} {
		if err := first.CreateWorkItem(ctx, item); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := first.ReserveWorkflowBudget(ctx, "wi_deny_a", "owner-a"); err != nil {
		t.Fatal(err)
	}
	if _, err := first.ReserveWorkflowBudget(ctx, "wi_deny_b", "owner-b"); err != nil {
		t.Fatal(err)
	}
	// B commits real spend; A then overshoots what the tree can still afford.
	if allowed, err := first.ReconcileWorkflowBudget(ctx, "wi_deny_b", "owner-b", .6); err != nil || !allowed {
		t.Fatalf("b allowed=%v err=%v", allowed, err)
	}
	if allowed, err := first.ReconcileWorkflowBudget(ctx, "wi_deny_a", "owner-a", .7); err != nil || allowed {
		t.Fatalf("a allowed=%v err=%v", allowed, err)
	}
	// Crash before ParkBudgetTree: the denied decision was never acted on.
	db1test.Exec(t, path, `UPDATE lifecycle_work_item SET reservation_lease_until=CURRENT_TIMESTAMP - INTERVAL '1 minute' WHERE work_item_id=?`, "wi_deny_a")
	if err := first.Close(); err != nil {
		t.Fatal(err)
	}
	restarted, err := db1test.Open(t, path)
	if err != nil {
		t.Fatal(err)
	}
	defer restarted.Close()
	replay, err := restarted.ReserveWorkflowBudget(ctx, "wi_deny_a", "restart-owner")
	if err != nil || !replay.ReplayOnly || replay.Amount != .7 {
		t.Fatalf("replay=%+v err=%v", replay, err)
	}
	if allowed, err := restarted.ReconcileWorkflowBudget(ctx, "wi_deny_a", "restart-owner", .7); err != nil || allowed {
		t.Fatalf("replayed denial allowed=%v err=%v", allowed, err)
	}
}

// Two processes must not both enter replay execution for the same reservation.
func TestConcurrentReplayAdmissionAdmitsExactlyOneOwner(t *testing.T) {
	path := filepath.Join(t.TempDir(), "aimee.db")
	first, err := db1test.Open(t, path)
	if err != nil {
		t.Fatal(err)
	}
	defer first.Close()
	second, err := db1test.Open(t, path)
	if err != nil {
		t.Fatal(err)
	}
	defer second.Close()
	ctx := t.Context()
	item := db1.CreateWorkItem{ID: "wi_replay_race", Repo: "repo", ProposalPath: "replay-race", WorkflowName: "build", WorkflowVersion: "v1", StartStage: "work", MaxCostUSD: 1}
	if err := first.CreateWorkItem(ctx, item); err != nil {
		t.Fatal(err)
	}
	if _, err := first.ReserveWorkflowBudget(ctx, item.ID, "dead-owner"); err != nil {
		t.Fatal(err)
	}
	if allowed, err := first.ReconcileWorkflowBudget(ctx, item.ID, "dead-owner", .4); err != nil || !allowed {
		t.Fatalf("allowed=%v err=%v", allowed, err)
	}
	db1test.Exec(t, path, `UPDATE lifecycle_work_item SET reservation_lease_until=CURRENT_TIMESTAMP - INTERVAL '1 minute' WHERE work_item_id=?`, item.ID)
	winner, err := first.ReserveWorkflowBudget(ctx, item.ID, "replay-one")
	if err != nil || !winner.ReplayOnly {
		t.Fatalf("winner=%+v err=%v", winner, err)
	}
	loser, err := second.ReserveWorkflowBudget(ctx, item.ID, "replay-two")
	if err != nil || !loser.Busy || loser.ReplayOnly || loser.Allowed {
		t.Fatalf("loser=%+v err=%v", loser, err)
	}
	// Only the admitted owner can consume the reservation.
	if _, err := second.ReconcileWorkflowBudget(ctx, item.ID, "replay-two", .4); err == nil {
		t.Fatal("losing owner reconciled the reservation")
	}
	if allowed, err := first.ReconcileWorkflowBudget(ctx, item.ID, "replay-one", .4); err != nil || !allowed {
		t.Fatalf("winner reconcile allowed=%v err=%v", allowed, err)
	}
}

// A non-finite runner cost must be rejected at the durable boundary, not written
// into cum_cost_usd where it would strand the whole tree in budget_cap.
func TestReconcileRejectsNonFiniteCost(t *testing.T) {
	store, _ := newTestStore(t)
	ctx := t.Context()
	item := db1.CreateWorkItem{ID: "wi_nonfinite", Repo: "repo", ProposalPath: "nonfinite", WorkflowName: "build", WorkflowVersion: "v1", StartStage: "work", MaxCostUSD: 1}
	if err := store.CreateWorkItem(ctx, item); err != nil {
		t.Fatal(err)
	}
	if _, err := store.ReserveWorkflowBudget(ctx, item.ID, "owner"); err != nil {
		t.Fatal(err)
	}
	for _, bad := range []float64{math.NaN(), math.Inf(1), math.Inf(-1), -1} {
		if _, err := store.ReconcileWorkflowBudget(ctx, item.ID, "owner", bad); err == nil {
			t.Fatalf("reconcile accepted non-finite/negative cost %v", bad)
		}
		if err := store.ParkRunnerFailure(ctx, item.ID, "work", "owner", "runner_unavailable", "d", true, false, bad); err == nil {
			t.Fatalf("park accepted non-finite/negative cost %v", bad)
		}
	}
	// The rejected reconciles left the reservation intact for a real retry.
	got, err := store.WorkItem(ctx, item.ID)
	if err != nil || got.CumulativeCostUSD != 0 {
		t.Fatalf("cum_cost polluted by rejected cost: %+v err=%v", got, err)
	}
}

// A replay whose durable result is gone recovers per reservation state: an
// 'unresolved' (interrupted) reservation is released for a fresh dispatch; an
// 'actual' (measured, reconciled) reservation commits its cost and parks for a
// human rather than silently re-billing it.
func TestRecoverLostReplayReleasesUnresolvedAndParksActual(t *testing.T) {
	ctx := t.Context()

	t.Run("unresolved_redispatches", func(t *testing.T) {
		store, storePath := newTestStore(t)
		item := db1.CreateWorkItem{ID: "wi_lost_unresolved", Repo: "r", ProposalPath: "u", WorkflowName: "build", WorkflowVersion: "v1", StartStage: "impl", MaxCostUSD: 1}
		if err := store.CreateWorkItem(ctx, item); err != nil {
			t.Fatal(err)
		}
		if _, err := store.ReserveWorkflowBudget(ctx, item.ID, "owner1"); err != nil {
			t.Fatal(err)
		}
		// Ambiguous post-dispatch failure leaves the reservation 'unresolved'.
		if err := store.ParkRunnerFailure(ctx, item.ID, "impl", "owner1", "runner_unavailable", "d", true, false, 0); err != nil {
			t.Fatal(err)
		}
		// A restart resumes: a new owner takes replay ownership.
		db1test.Exec(t, storePath, `UPDATE lifecycle_work_item SET pause_reason='', reservation_lease_until=CURRENT_TIMESTAMP - INTERVAL '1 minute' WHERE work_item_id=?`, item.ID)
		res, err := store.ReserveWorkflowBudget(ctx, item.ID, "owner2")
		if err != nil || !res.ReplayOnly {
			t.Fatalf("res=%+v err=%v", res, err)
		}
		redispatch, err := store.RecoverLostReplay(ctx, item.ID, "impl", "owner2")
		if err != nil || !redispatch {
			t.Fatalf("redispatch=%v err=%v", redispatch, err)
		}
		got, err := store.WorkItem(ctx, item.ID)
		if err != nil || got.State != "active" || got.PauseReason != "" || got.ReservationState != "" || got.ReservedCostUSD != 0 {
			t.Fatalf("item not runnable/cleared: %+v err=%v", got, err)
		}
	})

	t.Run("actual_parks_for_human", func(t *testing.T) {
		store, storePath := newTestStore(t)
		item := db1.CreateWorkItem{ID: "wi_lost_actual", Repo: "r", ProposalPath: "a", WorkflowName: "build", WorkflowVersion: "v1", StartStage: "impl", MaxCostUSD: 1}
		if err := store.CreateWorkItem(ctx, item); err != nil {
			t.Fatal(err)
		}
		if _, err := store.ReserveWorkflowBudget(ctx, item.ID, "owner1"); err != nil {
			t.Fatal(err)
		}
		if allowed, err := store.ReconcileWorkflowBudget(ctx, item.ID, "owner1", 0.4); err != nil || !allowed {
			t.Fatalf("reconcile allowed=%v err=%v", allowed, err)
		}
		db1test.Exec(t, storePath, `UPDATE lifecycle_work_item SET reservation_lease_until=CURRENT_TIMESTAMP - INTERVAL '1 minute' WHERE work_item_id=?`, item.ID)
		res, err := store.ReserveWorkflowBudget(ctx, item.ID, "owner2")
		if err != nil || !res.ReplayOnly {
			t.Fatalf("res=%+v err=%v", res, err)
		}
		redispatch, err := store.RecoverLostReplay(ctx, item.ID, "impl", "owner2")
		if err != nil || redispatch {
			t.Fatalf("redispatch=%v err=%v", redispatch, err)
		}
		got, err := store.WorkItem(ctx, item.ID)
		if err != nil || got.PauseReason != "replay_unrecoverable" || got.ReservationState != "" || got.ReservedCostUSD != 0 || got.CumulativeCostUSD != 0.4 {
			t.Fatalf("expected parked with committed cost: %+v err=%v", got, err)
		}
	})

	t.Run("wrong_owner_refused", func(t *testing.T) {
		store, _ := newTestStore(t)
		item := db1.CreateWorkItem{ID: "wi_lost_owner", Repo: "r", ProposalPath: "o", WorkflowName: "build", WorkflowVersion: "v1", StartStage: "impl", MaxCostUSD: 1}
		if err := store.CreateWorkItem(ctx, item); err != nil {
			t.Fatal(err)
		}
		if _, err := store.ReserveWorkflowBudget(ctx, item.ID, "owner1"); err != nil {
			t.Fatal(err)
		}
		if err := store.ParkRunnerFailure(ctx, item.ID, "impl", "owner1", "runner_unavailable", "d", true, false, 0); err != nil {
			t.Fatal(err)
		}
		if _, err := store.RecoverLostReplay(ctx, item.ID, "impl", "intruder"); err == nil {
			t.Fatal("recover accepted a non-owner")
		}
	})
}

// The production refinement cycle is gate --loop--> author, then
// author --advance--> gate. That re-entering advance was clearing the gate's own
// attempt counter, so the cap never accumulated and the pair looped without
// bound: a live plan gate reached 63 loops against a cap of 20. Entering a stage
// must not reset the budget that bounds it; only completing one may.
func TestGateIterationCapSurvivesTheAuthorsReentry(t *testing.T) {
	store, _ := newTestStore(t)
	createTestItem(t, store, "wi_loop")
	ctx := context.Background()

	parkedAt := -1
	for i := 0; i < 10; i++ {
		out, err := store.RecordRequestedChanges(ctx, "wi_loop", "plan_gate", "plan",
			fmt.Sprintf("plan-%d", i), fmt.Sprintf("feedback-%d", i), "", 3, 99, 0)
		if err != nil {
			t.Fatal(err)
		}
		if out.Parked {
			parkedAt = i
			break
		}
		// The author re-authors and advances back into the gate.
		if err := store.Move(ctx, "wi_loop", "plan", "plan_gate", "advance", "",
			fmt.Sprintf("plan-%d", i), 0); err != nil {
			t.Fatal(err)
		}
	}
	if parkedAt != 2 {
		t.Fatalf("gate parked at loop %d, want 2 (cap of 3); the author's re-entry reset the counter", parkedAt)
	}
}
