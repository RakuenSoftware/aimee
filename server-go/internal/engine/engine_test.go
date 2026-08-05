package engine

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	_ "modernc.org/sqlite"

	"github.com/JBailes/aimee/server-go/internal/db1"
	"github.com/JBailes/aimee/server-go/internal/wfe"
)

func execOnDB(t *testing.T, path, query string, args ...any) {
	t.Helper()
	db, err := sql.Open("sqlite", "file:"+path)
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	if _, err := db.Exec(query, args...); err != nil {
		t.Fatal(err)
	}
}

// expireBudgetLease simulates a crashed owner whose reservation lease has run
// out. Replay ownership is only transferable once the lease lapses.
func expireBudgetLease(t *testing.T, path, workItemID string) {
	t.Helper()
	db, err := sql.Open("sqlite", "file:"+path)
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	if _, err := db.Exec(`UPDATE lifecycle_work_item SET reservation_lease_until=datetime('now','-1 minute') WHERE work_item_id=?`, workItemID); err != nil {
		t.Fatal(err)
	}
}

type scriptedRunner struct {
	t          *testing.T
	call       int
	proposal   string
	firstPlan  string
	secondPlan string
	feedback   wfe.ReviewFeedback
}

func (r *scriptedRunner) Run(_ context.Context, req StepRequest) (StepResult, error) {
	r.call++
	if req.Proposal != r.proposal {
		r.t.Fatal("runner did not receive the complete proposal")
	}
	switch r.call {
	case 1:
		if req.Node.ID != "plan" {
			r.t.Fatalf("first node=%q", req.Node.ID)
		}
		return StepResult{Status: StepAdvanced, Artifact: r.firstPlan}, nil
	case 2:
		if req.Node.ID != "plan_gate" || string(req.Inputs["src"].Content) != r.firstPlan {
			r.t.Fatal("gate did not receive the complete first plan")
		}
		return StepResult{Status: StepChanges, Feedback: &r.feedback}, nil
	case 3:
		if req.Node.ID != "plan" || req.Feedback == nil ||
			req.Feedback.Findings[0].Recommendation != r.feedback.Findings[0].Recommendation {
			r.t.Fatal("refining planner did not receive complete structured feedback")
		}
		return StepResult{Status: StepAdvanced, Artifact: r.secondPlan}, nil
	case 4:
		if req.Node.ID != "plan_gate" || string(req.Inputs["src"].Content) != r.secondPlan {
			r.t.Fatal("gate did not receive the complete revised plan")
		}
		return StepResult{Status: StepAdvanced, ContentHash: req.Inputs["src"].Hash}, nil
	case 5:
		if req.Node.ID != "done" {
			r.t.Fatalf("terminal node=%q", req.Node.ID)
		}
		return StepResult{Status: StepAdvanced}, nil
	default:
		r.t.Fatalf("unexpected runner call %d", r.call)
		return StepResult{}, nil
	}
}

type artifactRoutingRunner struct {
	t    *testing.T
	call int
}

type recoveringRunner struct{ calls int }

type retryDetailRunner struct {
	t      *testing.T
	calls  int
	detail string
}

type blockingSiblingRunner struct {
	started chan StepRequest
	release chan struct{}
	costUSD float64
}

func (r *blockingSiblingRunner) Run(ctx context.Context, req StepRequest) (StepResult, error) {
	select {
	case r.started <- req:
	case <-ctx.Done():
		return StepResult{}, ctx.Err()
	}
	select {
	case <-r.release:
		return StepResult{Status: StepAdvanced, CostUSD: r.costUSD}, nil
	case <-ctx.Done():
		return StepResult{}, ctx.Err()
	}
}

func (r *recoveringRunner) Run(_ context.Context, _ StepRequest) (StepResult, error) {
	r.calls++
	if r.calls == 1 {
		return StepResult{}, errors.New("temporary runner outage")
	}
	return StepResult{Status: StepAdvanced}, nil
}

func (r *retryDetailRunner) Run(_ context.Context, req StepRequest) (StepResult, error) {
	r.calls++
	if r.calls == 1 {
		return StepResult{Status: StepChanges, Detail: r.detail}, nil
	}
	if req.RetryDetail != r.detail {
		r.t.Fatalf("retry detail=%q, want %q", req.RetryDetail, r.detail)
	}
	return StepResult{Status: StepAdvanced}, nil
}

func TestRetryPassesPreviousFailureDetailToRunner(t *testing.T) {
	root := t.TempDir()
	workflowDir := filepath.Join(root, "workflows")
	if err := os.MkdirAll(workflowDir, 0o700); err != nil {
		t.Fatal(err)
	}
	definition := []byte("name: retry-detail\nstart: work\nnodes:\n  - id: work\n    block: author.proposal\n    on_fail: work\n    params:\n      max_rounds: 3\n")
	if err := os.WriteFile(filepath.Join(workflowDir, "retry-detail.yaml"), definition, 0o600); err != nil {
		t.Fatal(err)
	}
	def, err := wfe.ParseDefinition(definition)
	if err != nil {
		t.Fatal(err)
	}
	store, err := db1.Open(filepath.Join(root, "aimee.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	artifacts, err := wfe.NewArtifactStore(filepath.Join(root, "artifacts"))
	if err != nil {
		t.Fatal(err)
	}
	item := db1.CreateWorkItem{ID: "wi_retry_detail", Repo: "repo", ProposalPath: "proposal", WorkflowName: "retry-detail", WorkflowVersion: def.Version, StartStage: "work"}
	if err := store.CreateWorkItem(t.Context(), item); err != nil {
		t.Fatal(err)
	}
	if err := artifacts.PutProposal(item.ID, []byte("proposal")); err != nil {
		t.Fatal(err)
	}
	runner := &retryDetailRunner{t: t, detail: "verify failed: clang-format violation"}
	workflowEngine, err := New(store, artifacts, workflowDir, runner)
	if err != nil {
		t.Fatal(err)
	}
	if result, err := workflowEngine.Advance(t.Context(), item.ID); err != nil || result.Parked || result.NextStage != "work" {
		t.Fatalf("first advance result=%+v err=%v", result, err)
	}
	if result, err := workflowEngine.Advance(t.Context(), item.ID); err != nil || !result.Terminal || result.State != "accepted" {
		t.Fatalf("retry advance result=%+v err=%v", result, err)
	}
}

func testSiblingStepsRunConcurrently(t *testing.T, maxUSD, stepCostUSD float64) {
	root := t.TempDir()
	workflowDir := filepath.Join(root, "workflows")
	if err := os.MkdirAll(workflowDir, 0o700); err != nil {
		t.Fatal(err)
	}
	definition := []byte("name: one\nstart: work\nnodes:\n  - id: work\n    block: author.proposal\n")
	if err := os.WriteFile(filepath.Join(workflowDir, "one.yaml"), definition, 0o600); err != nil {
		t.Fatal(err)
	}
	def, err := wfe.ParseDefinition(definition)
	if err != nil {
		t.Fatal(err)
	}
	store, err := db1.Open(filepath.Join(root, "aimee.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	artifacts, err := wfe.NewArtifactStore(filepath.Join(root, "artifacts"))
	if err != nil {
		t.Fatal(err)
	}
	items := []db1.CreateWorkItem{
		{ID: "wi_parallel", Repo: "repo", ProposalPath: "root", WorkflowName: "one", WorkflowVersion: def.Version, StartStage: "work", MaxCostUSD: maxUSD},
		{ID: "wi_parallel.a", Repo: "repo", ProposalPath: "a", WorkflowName: "one", WorkflowVersion: def.Version, StartStage: "work", ParentID: "wi_parallel"},
		{ID: "wi_parallel.b", Repo: "repo", ProposalPath: "b", WorkflowName: "one", WorkflowVersion: def.Version, StartStage: "work", ParentID: "wi_parallel"},
	}
	for _, item := range items {
		if err := store.CreateWorkItem(t.Context(), item); err != nil {
			t.Fatal(err)
		}
		if err := artifacts.PutProposal(item.ID, []byte("proposal")); err != nil {
			t.Fatal(err)
		}
	}
	runner := &blockingSiblingRunner{started: make(chan StepRequest, 2), release: make(chan struct{}), costUSD: stepCostUSD}
	workflowEngine, err := New(store, artifacts, workflowDir, runner)
	if err != nil {
		t.Fatal(err)
	}
	var wg sync.WaitGroup
	errs := make(chan error, 2)
	for _, id := range []string{"wi_parallel.a", "wi_parallel.b"} {
		wg.Add(1)
		go func() {
			defer wg.Done()
			_, err := workflowEngine.Advance(t.Context(), id)
			errs <- err
		}()
	}
	var admitted []StepRequest
	for range 2 {
		select {
		case req := <-runner.started:
			admitted = append(admitted, req)
		case <-time.After(2 * time.Second):
			close(runner.release)
			t.Fatal("uncapped sibling was serialized behind another runner call")
		}
	}
	if maxUSD > 0 {
		var limits float64
		for _, req := range admitted {
			if req.CostLimitUSD <= 0 {
				close(runner.release)
				t.Fatalf("capped runner received no enforceable limit: %+v", req)
			}
			limits += req.CostLimitUSD
		}
		if limits > maxUSD {
			close(runner.release)
			t.Fatalf("runner limits=%v exceed max=%v", limits, maxUSD)
		}
		var reserved float64
		for _, id := range []string{"wi_parallel.a", "wi_parallel.b"} {
			item, err := store.WorkItem(t.Context(), id)
			if err != nil || item.ReservedCostUSD <= 0 {
				close(runner.release)
				t.Fatalf("item=%+v err=%v", item, err)
			}
			reserved += item.ReservedCostUSD
		}
		if reserved > maxUSD {
			close(runner.release)
			t.Fatalf("reserved=%v exceeds max=%v", reserved, maxUSD)
		}
	}
	close(runner.release)
	wg.Wait()
	close(errs)
	for err := range errs {
		if err != nil {
			t.Fatal(err)
		}
	}
	if maxUSD > 0 {
		_, spent, budgetMax, err := store.WorkflowBudget(t.Context(), "wi_parallel")
		if err != nil || spent != 2*stepCostUSD || budgetMax != maxUSD || spent > budgetMax {
			t.Fatalf("spent=%v max=%v err=%v", spent, budgetMax, err)
		}
		for _, id := range []string{"wi_parallel.a", "wi_parallel.b"} {
			item, err := store.WorkItem(t.Context(), id)
			if err != nil || item.ReservedCostUSD != 0 {
				t.Fatalf("item=%+v err=%v", item, err)
			}
		}
	}
}

func TestSiblingStepsRunConcurrentlyWithAndWithoutBudgetCap(t *testing.T) {
	t.Run("uncapped", func(t *testing.T) { testSiblingStepsRunConcurrently(t, 0, 0) })
	t.Run("capped", func(t *testing.T) { testSiblingStepsRunConcurrently(t, 1, .1) })
}

type breakArtifactStoreAfterSpendRunner struct {
	artifactRoot string
	cost         float64
}

func (r breakArtifactStoreAfterSpendRunner) Run(_ context.Context, _ StepRequest) (StepResult, error) {
	if err := os.RemoveAll(r.artifactRoot); err != nil {
		return StepResult{}, err
	}
	if err := os.WriteFile(r.artifactRoot, []byte("not a directory"), 0o600); err != nil {
		return StepResult{}, err
	}
	return StepResult{Status: StepAdvanced, Artifact: "completed", CostUSD: r.cost}, nil
}

func TestReconciledSpendSurvivesPostSpendArtifactFailure(t *testing.T) {
	root := t.TempDir()
	workflowDir := filepath.Join(root, "workflows")
	if err := os.MkdirAll(workflowDir, 0o700); err != nil {
		t.Fatal(err)
	}
	definition := []byte("name: one\nstart: work\nnodes:\n  - id: work\n    block: author.proposal\n    next: done\n  - id: done\n    block: author.proposal\n")
	if err := os.WriteFile(filepath.Join(workflowDir, "one.yaml"), definition, 0o600); err != nil {
		t.Fatal(err)
	}
	def, err := wfe.ParseDefinition(definition)
	if err != nil {
		t.Fatal(err)
	}
	store, err := db1.Open(filepath.Join(root, "aimee.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	artifactRoot := filepath.Join(root, "artifacts")
	artifacts, err := wfe.NewArtifactStore(artifactRoot)
	if err != nil {
		t.Fatal(err)
	}
	item := db1.CreateWorkItem{ID: "wi_commit_failure", Repo: "repo", ProposalPath: "proposal", WorkflowName: "one", WorkflowVersion: def.Version, StartStage: "work", MaxCostUSD: 1}
	if err := store.CreateWorkItem(t.Context(), item); err != nil {
		t.Fatal(err)
	}
	if err := artifacts.PutProposal(item.ID, []byte("proposal")); err != nil {
		t.Fatal(err)
	}
	workflowEngine, err := New(store, artifacts, workflowDir, breakArtifactStoreAfterSpendRunner{artifactRoot: artifactRoot, cost: .25})
	if err != nil {
		t.Fatal(err)
	}
	advance, advanceErr := workflowEngine.Advance(t.Context(), item.ID)
	if advanceErr != nil || !advance.Ran {
		t.Fatalf("advance=%+v err=%v", advance, advanceErr)
	}
	got, err := store.WorkItem(t.Context(), item.ID)
	if err != nil {
		t.Fatal(err)
	}
	if got.ReservedCostUSD != 0 || got.ReservationState != "" || got.CumulativeCostUSD != .25 || got.Stage != "work" || got.PauseReason != "artifact_write_failed" {
		t.Fatalf("actual spend was not retained across commit failure: %+v", got)
	}
}

func TestReconciledSpendReopensAndCommitsExactlyOnce(t *testing.T) {
	path := filepath.Join(t.TempDir(), "aimee.db")
	store, err := db1.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	item := db1.CreateWorkItem{ID: "wi_replay", Repo: "repo", ProposalPath: "replay", WorkflowName: "one", WorkflowVersion: "v1", StartStage: "work", MaxCostUSD: 1}
	if err := store.CreateWorkItem(t.Context(), item); err != nil {
		t.Fatal(err)
	}
	reservation, err := store.ReserveWorkflowBudget(t.Context(), item.ID, "first")
	if err != nil || reservation.Amount <= 0 {
		t.Fatalf("reservation=%+v err=%v", reservation, err)
	}
	if allowed, err := store.ReconcileWorkflowBudget(t.Context(), item.ID, "first", .25); err != nil || !allowed {
		t.Fatalf("allowed=%v err=%v", allowed, err)
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}
	store, err = db1.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	// While the reconciling owner's lease is live, replay is refused outright:
	// two invocations must never replay the same reservation concurrently.
	if busy, err := store.ReserveWorkflowBudget(t.Context(), item.ID, "second"); err != nil || !busy.Busy || busy.ReplayOnly {
		t.Fatalf("busy=%+v err=%v", busy, err)
	}
	expireBudgetLease(t, path, item.ID)
	replay, err := store.ReserveWorkflowBudget(t.Context(), item.ID, "second")
	if err != nil || !replay.ReplayOnly || replay.Amount != .25 {
		t.Fatalf("replay=%+v err=%v", replay, err)
	}
	if allowed, err := store.ReconcileWorkflowBudget(t.Context(), item.ID, "second", .25); err != nil || !allowed {
		t.Fatalf("replay reconcile allowed=%v err=%v", allowed, err)
	}
	if err := store.Move(t.Context(), item.ID, "work", "done", "advance", "replayed result", "", .25); err != nil {
		t.Fatal(err)
	}
	got, err := store.WorkItem(t.Context(), item.ID)
	if err != nil || got.CumulativeCostUSD != .25 || got.ReservedCostUSD != 0 {
		t.Fatalf("item=%+v err=%v", got, err)
	}
}

func TestUncappedDuplicateAdvanceCannotShareOneInvocation(t *testing.T) {
	root := t.TempDir()
	workflowDir := filepath.Join(root, "workflows")
	if err := os.MkdirAll(workflowDir, 0o700); err != nil {
		t.Fatal(err)
	}
	definition := []byte("name: one\nstart: work\nnodes:\n  - id: work\n    block: author.proposal\n")
	if err := os.WriteFile(filepath.Join(workflowDir, "one.yaml"), definition, 0o600); err != nil {
		t.Fatal(err)
	}
	def, err := wfe.ParseDefinition(definition)
	if err != nil {
		t.Fatal(err)
	}
	store, err := db1.Open(filepath.Join(root, "aimee.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	artifacts, err := wfe.NewArtifactStore(filepath.Join(root, "artifacts"))
	if err != nil {
		t.Fatal(err)
	}
	item := db1.CreateWorkItem{ID: "wi_duplicate", Repo: "repo", ProposalPath: "duplicate", WorkflowName: "one", WorkflowVersion: def.Version, StartStage: "work"}
	if err := store.CreateWorkItem(t.Context(), item); err != nil {
		t.Fatal(err)
	}
	if err := artifacts.PutProposal(item.ID, []byte("proposal")); err != nil {
		t.Fatal(err)
	}
	runner := &blockingSiblingRunner{started: make(chan StepRequest, 2), release: make(chan struct{}), costUSD: .1}
	workflowEngine, err := New(store, artifacts, workflowDir, runner)
	if err != nil {
		t.Fatal(err)
	}
	firstDone := make(chan error, 1)
	go func() {
		_, err := workflowEngine.Advance(t.Context(), item.ID)
		firstDone <- err
	}()
	select {
	case <-runner.started:
	case <-time.After(time.Second):
		t.Fatal("first invocation did not enter runner")
	}
	second, err := workflowEngine.Advance(t.Context(), item.ID)
	if err != nil || second.Ran || second.Parked {
		t.Fatalf("duplicate advance was not deferred: result=%+v err=%v", second, err)
	}
	select {
	case duplicate := <-runner.started:
		t.Fatalf("duplicate invocation entered runner: %+v", duplicate)
	default:
	}
	close(runner.release)
	if err := <-firstDone; err != nil {
		t.Fatal(err)
	}
}

type postDispatchFailureRunner struct{}

func (postDispatchFailureRunner) Run(context.Context, StepRequest) (StepResult, error) {
	return StepResult{}, &DelegateExecutionError{Err: errors.New("status polling failed after provider dispatch"), Dispatched: true}
}

type replayMeasuredRunner struct{ t *testing.T }

func (r replayMeasuredRunner) Run(_ context.Context, req StepRequest) (StepResult, error) {
	if !req.ReplayOnly {
		r.t.Fatal("restart launched replacement provider work instead of durable replay")
	}
	return StepResult{Status: StepAdvanced, Artifact: "replayed", CostUSD: .1}, nil
}

func TestPostDispatchFailureChargesReservationAcrossRestart(t *testing.T) {
	root := t.TempDir()
	workflowDir := filepath.Join(root, "workflows")
	if err := os.MkdirAll(workflowDir, 0o700); err != nil {
		t.Fatal(err)
	}
	definition := []byte("name: one\nstart: work\nnodes:\n  - id: work\n    block: author.proposal\n")
	if err := os.WriteFile(filepath.Join(workflowDir, "one.yaml"), definition, 0o600); err != nil {
		t.Fatal(err)
	}
	def, err := wfe.ParseDefinition(definition)
	if err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(root, "aimee.db")
	store, err := db1.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	artifacts, err := wfe.NewArtifactStore(filepath.Join(root, "artifacts"))
	if err != nil {
		t.Fatal(err)
	}
	item := db1.CreateWorkItem{ID: "wi_billed_error", Repo: "repo", ProposalPath: "failure", WorkflowName: "one", WorkflowVersion: def.Version, StartStage: "work", MaxCostUSD: .4}
	if err := store.CreateWorkItem(t.Context(), item); err != nil {
		t.Fatal(err)
	}
	if err := artifacts.PutProposal(item.ID, []byte("proposal")); err != nil {
		t.Fatal(err)
	}
	workflowEngine, err := New(store, artifacts, workflowDir, postDispatchFailureRunner{})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := workflowEngine.Advance(t.Context(), item.ID); err != nil {
		t.Fatal(err)
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}
	store, err = db1.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	got, err := store.WorkItem(t.Context(), item.ID)
	if err != nil || got.CumulativeCostUSD != 0 || got.ReservedCostUSD != .4 || got.ReservationState != "unresolved" || got.PauseReason != "runner_unavailable" {
		t.Fatalf("item=%+v err=%v", got, err)
	}
	if resumed, err := store.ResumeTransient(t.Context(), "runner_unavailable", 0); err != nil || resumed != 1 {
		t.Fatalf("resume count=%d err=%v", resumed, err)
	}
	artifacts, err = wfe.NewArtifactStore(filepath.Join(root, "artifacts"))
	if err != nil {
		t.Fatal(err)
	}
	workflowEngine, err = New(store, artifacts, workflowDir, replayMeasuredRunner{t: t})
	if err != nil {
		t.Fatal(err)
	}
	if result, err := workflowEngine.Advance(t.Context(), item.ID); err != nil || !result.Terminal {
		t.Fatalf("replay result=%+v err=%v", result, err)
	}
	got, err = store.WorkItem(t.Context(), item.ID)
	if err != nil || got.CumulativeCostUSD != .1 || got.ReservedCostUSD != 0 || got.ReservationState != "" {
		t.Fatalf("reconciled item=%+v err=%v", got, err)
	}
}

func TestPreDispatchRunnerFailureDoesNotConsumeBudget(t *testing.T) {
	root := t.TempDir()
	workflowDir := filepath.Join(root, "workflows")
	if err := os.MkdirAll(workflowDir, 0o700); err != nil {
		t.Fatal(err)
	}
	definition := []byte("name: one\nstart: work\nnodes:\n  - id: work\n    block: author.proposal\n")
	if err := os.WriteFile(filepath.Join(workflowDir, "one.yaml"), definition, 0o600); err != nil {
		t.Fatal(err)
	}
	def, err := wfe.ParseDefinition(definition)
	if err != nil {
		t.Fatal(err)
	}
	store, err := db1.Open(filepath.Join(root, "aimee.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	artifacts, err := wfe.NewArtifactStore(filepath.Join(root, "artifacts"))
	if err != nil {
		t.Fatal(err)
	}
	item := db1.CreateWorkItem{ID: "wi_predispatch", Repo: "repo", ProposalPath: "predispatch", WorkflowName: "one", WorkflowVersion: def.Version, StartStage: "work", MaxCostUSD: .4}
	if err := store.CreateWorkItem(t.Context(), item); err != nil {
		t.Fatal(err)
	}
	if err := artifacts.PutProposal(item.ID, []byte("proposal")); err != nil {
		t.Fatal(err)
	}
	workflowEngine, err := New(store, artifacts, workflowDir, &recoveringRunner{})
	if err != nil {
		t.Fatal(err)
	}
	if result, err := workflowEngine.Advance(t.Context(), item.ID); err != nil || !result.Parked {
		t.Fatalf("result=%+v err=%v", result, err)
	}
	got, err := store.WorkItem(t.Context(), item.ID)
	if err != nil || got.CumulativeCostUSD != 0 || got.ReservedCostUSD != 0 || got.ReservationState != "" {
		t.Fatalf("item=%+v err=%v", got, err)
	}
}

func TestTransientParkRecoversAndReleasesAdmissionCapacity(t *testing.T) {
	root := t.TempDir()
	workflowDir := filepath.Join(root, "workflows")
	if err := os.MkdirAll(workflowDir, 0o700); err != nil {
		t.Fatal(err)
	}
	definition := []byte("name: one\nstart: work\nnodes:\n  - id: work\n    block: author.proposal\n")
	if err := os.WriteFile(filepath.Join(workflowDir, "one.yaml"), definition, 0o600); err != nil {
		t.Fatal(err)
	}
	def, err := wfe.ParseDefinition(definition)
	if err != nil {
		t.Fatal(err)
	}
	store, err := db1.Open(filepath.Join(root, "aimee.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	artifacts, err := wfe.NewArtifactStore(filepath.Join(root, "artifacts"))
	if err != nil {
		t.Fatal(err)
	}
	if err := artifacts.PutProposal("wi_recover", []byte("proposal")); err != nil {
		t.Fatal(err)
	}
	first := db1.CreateWorkItem{ID: "wi_recover", Repo: "repo", ProposalPath: "recover", WorkflowName: "one", WorkflowVersion: def.Version, StartStage: "work"}
	if err := store.AdmitRoot(t.Context(), first, 1); err != nil {
		t.Fatal(err)
	}
	runner := &recoveringRunner{}
	workflowEngine, err := New(store, artifacts, workflowDir, runner)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := workflowEngine.Advance(t.Context(), first.ID); err != nil {
		t.Fatal(err)
	}
	parked, err := store.WorkItem(t.Context(), first.ID)
	if err != nil || parked.State != "active" || parked.PauseReason != "runner_unavailable" {
		t.Fatalf("parked item=%+v err=%v", parked, err)
	}
	events, err := store.Events(t.Context(), first.ID, 0, 10)
	if err != nil || len(events) < 2 || events[len(events)-1].Kind != "pause" ||
		!strings.Contains(events[len(events)-1].Detail, "temporary runner outage") {
		t.Fatalf("events=%+v err=%v", events, err)
	}
	blocked := db1.CreateWorkItem{ID: "wi_blocked", Repo: "repo", ProposalPath: "blocked", WorkflowName: "one", WorkflowVersion: def.Version, StartStage: "work"}
	if err := store.AdmitRoot(t.Context(), blocked, 1); !errors.Is(err, db1.ErrAdmissionFull) {
		t.Fatalf("admission while transiently parked: %v", err)
	}
	if resumed, err := store.ResumeTransient(t.Context(), "runner_unavailable", 0); err != nil || resumed != 1 {
		t.Fatalf("resumed=%d err=%v", resumed, err)
	}
	if _, err := workflowEngine.Advance(t.Context(), first.ID); err != nil {
		t.Fatal(err)
	}
	item, err := store.WorkItem(t.Context(), first.ID)
	if err != nil || item.State != "accepted" {
		t.Fatalf("item=%+v err=%v", item, err)
	}
	if err := artifacts.PutProposal(blocked.ID, []byte("next")); err != nil {
		t.Fatal(err)
	}
	if err := store.AdmitRoot(t.Context(), blocked, 1); err != nil {
		t.Fatalf("admission after recovery: %v", err)
	}
}

func TestSafeDiagnosticRedactsSecretsWithoutTruncating(t *testing.T) {
	tail := strings.Repeat("diagnostic detail λ ", 100_000) + "END"
	got := safeDiagnostic(`Authorization: Bearer abc123 https://user:pass@example.test/path token=hidden password="my secret phrase" "access_token":"two words with \"quote\"" secret='three words' Cookie: session=four-words` + "\n" +
		"AKIAIOSFODNN7EXAMPLE eyJhbGciOiJIUzI1NiJ9.cGF5bG9hZA.c2lnbmF0dXJl -----BEGIN PRIVATE KEY-----\nprivate material\n-----END PRIVATE KEY----- " + tail)
	for _, secret := range []string{"abc123", "user:pass", "hidden", "my secret phrase", "two words", "three words", "four-words", "AKIAIOSFODNN7EXAMPLE", "cGF5bG9hZA", "private material"} {
		if strings.Contains(got, secret) {
			t.Fatalf("diagnostic leaked %q", secret)
		}
	}
	if !strings.HasSuffix(got, tail) {
		t.Fatal("diagnostic tail was truncated or changed")
	}
	unterminated := safeDiagnostic("password=\"my secret phrase\\\nnext diagnostic intact\nsecret='two words\\\nlast diagnostic intact")
	for _, secret := range []string{"my secret phrase", "secret phrase", "two words", "words"} {
		if strings.Contains(unterminated, secret) {
			t.Fatalf("unterminated diagnostic leaked %q", secret)
		}
	}
	for _, preserved := range []string{"next diagnostic intact", "last diagnostic intact"} {
		if !strings.Contains(unterminated, preserved) {
			t.Fatalf("unterminated redaction removed %q", preserved)
		}
	}
}

func (r *artifactRoutingRunner) Run(_ context.Context, req StepRequest) (StepResult, error) {
	r.call++
	switch req.Node.ID {
	case "freeze":
		return StepResult{Status: StepAdvanced, ArtifactType: "frozen_diff", Artifact: "complete diff"}, nil
	case "accept_gate":
		if got := string(req.Inputs["src"].Content); got != "complete diff" {
			r.t.Fatalf("acceptance gate reviewed %q, want complete diff", got)
		}
		return StepResult{Status: StepChanges, Feedback: &wfe.ReviewFeedback{Findings: []wfe.Finding{{
			ID: "fix", Persona: "qa", Severity: "blocking", Summary: "fix code", Recommendation: "revise",
		}}}}, nil
	default:
		r.t.Fatalf("unexpected node %q", req.Node.ID)
		return StepResult{}, nil
	}
}

type ciPassRunner struct{}

func (ciPassRunner) Run(context.Context, StepRequest) (StepResult, error) {
	return StepResult{Status: StepAdvanced, ArtifactType: "verdict", Artifact: "ci_passed"}, nil
}

// A gate.ci that passes must advance to its on_pass stage (merge), exactly like
// gate.roundtable — not fall through to next=="" and finish the slice terminal.
// The latter left every green slice PR unmerged at "ci accepted".
func TestGateCIAdvancesToMergeOnPass(t *testing.T) {
	root := t.TempDir()
	workflowDir := filepath.Join(root, "workflows")
	if err := os.MkdirAll(workflowDir, 0o700); err != nil {
		t.Fatal(err)
	}
	definition := []byte(`name: slice
start: freeze
nodes:
  - id: source
    block: understand
  - id: impl
    block: implement
    in: {plan: source.out}
  - id: freeze
    block: freeze
    in: {branch: impl.out}
    next: pr
  - id: pr
    block: pr.open
    in: {src: freeze.out}
    next: ci
    on_fail: pr
  - id: ci
    block: gate.ci
    in: {pr: pr.out}
    on_pass: merge
    on_fail: impl
  - id: merge
    block: merge
    in: {pr: pr.out}
`)
	if err := os.WriteFile(filepath.Join(workflowDir, "slice.yaml"), definition, 0o600); err != nil {
		t.Fatal(err)
	}
	def, err := wfe.ParseDefinition(definition)
	if err != nil {
		t.Fatal(err)
	}
	store, err := db1.Open(filepath.Join(root, "aimee.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	artifacts, err := wfe.NewArtifactStore(filepath.Join(root, "artifacts"))
	if err != nil {
		t.Fatal(err)
	}
	if err := artifacts.PutProposal("wi_ci", []byte("proposal")); err != nil {
		t.Fatal(err)
	}
	if _, err := artifacts.PutNodeArtifact("wi_ci", "impl", "branch", []byte("head")); err != nil {
		t.Fatal(err)
	}
	if err := store.CreateWorkItem(context.Background(), db1.CreateWorkItem{ID: "wi_ci", Repo: "repo", ProposalPath: "p", WorkflowName: "slice", WorkflowVersion: def.Version, StartStage: "freeze"}); err != nil {
		t.Fatal(err)
	}
	eng, err := New(store, artifacts, workflowDir, ciPassRunner{})
	if err != nil {
		t.Fatal(err)
	}
	// freeze -> pr, then pr -> ci; the third advance runs gate.ci.
	var out AdvanceResult
	for i := 0; i < 3; i++ {
		out, err = eng.Advance(context.Background(), "wi_ci")
		if err != nil {
			t.Fatalf("advance %d: %v", i, err)
		}
	}
	// gate.ci passed: it must advance to merge (its on_pass), not finish the slice
	// terminal at ci with the PR left unmerged.
	if out.Terminal || out.NextStage != "merge" {
		t.Fatalf("gate.ci pass should advance to merge, got %+v", out)
	}
}

type acceptedRunner struct{}

func (acceptedRunner) Run(context.Context, StepRequest) (StepResult, error) {
	return StepResult{Status: StepAccepted, Detail: "no-op: empty diff vs base"}, nil
}

// A step returning StepAccepted completes the item as an accepted no-op and does
// NOT advance to a remaining stage. This is what freeze does on an empty diff so a
// no-op slice cannot loop through review to convergence_no_progress.
func TestStepAcceptedFinishesItemWithoutAdvancing(t *testing.T) {
	root := t.TempDir()
	workflowDir := filepath.Join(root, "workflows")
	if err := os.MkdirAll(workflowDir, 0o700); err != nil {
		t.Fatal(err)
	}
	definition := []byte(`name: slice
start: freeze
nodes:
  - id: source
    block: understand
  - id: impl
    block: implement
    in: {plan: source.out}
  - id: freeze
    block: freeze
    in: {branch: impl.out}
    next: pr
  - id: pr
    block: pr.open
    in: {src: freeze.out}
    next: ci
  - id: ci
    block: gate.ci
    in: {pr: pr.out}
    on_pass: merge
    on_fail: impl
  - id: merge
    block: merge
    in: {pr: pr.out}
`)
	if err := os.WriteFile(filepath.Join(workflowDir, "slice.yaml"), definition, 0o600); err != nil {
		t.Fatal(err)
	}
	def, err := wfe.ParseDefinition(definition)
	if err != nil {
		t.Fatal(err)
	}
	store, err := db1.Open(filepath.Join(root, "aimee.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	artifacts, err := wfe.NewArtifactStore(filepath.Join(root, "artifacts"))
	if err != nil {
		t.Fatal(err)
	}
	if err := artifacts.PutProposal("wi_noop", []byte("proposal")); err != nil {
		t.Fatal(err)
	}
	if _, err := artifacts.PutNodeArtifact("wi_noop", "impl", "branch", []byte("head")); err != nil {
		t.Fatal(err)
	}
	if err := store.CreateWorkItem(context.Background(), db1.CreateWorkItem{ID: "wi_noop", Repo: "repo", ProposalPath: "p", WorkflowName: "slice", WorkflowVersion: def.Version, StartStage: "freeze"}); err != nil {
		t.Fatal(err)
	}
	eng, err := New(store, artifacts, workflowDir, acceptedRunner{})
	if err != nil {
		t.Fatal(err)
	}
	out, err := eng.Advance(context.Background(), "wi_noop")
	if err != nil {
		t.Fatalf("advance: %v", err)
	}
	if !out.Terminal || out.State != "accepted" {
		t.Fatalf("StepAccepted must finish the item accepted, got terminal=%v state=%q", out.Terminal, out.State)
	}
	if out.NextStage != "" {
		t.Fatalf("no-op accept must not advance to a next stage, got %q", out.NextStage)
	}
}

func TestGateReviewsItsBoundArtifactNotThePlanShortcut(t *testing.T) {
	root := t.TempDir()
	workflowDir := filepath.Join(root, "workflows")
	if err := os.MkdirAll(workflowDir, 0o700); err != nil {
		t.Fatal(err)
	}
	definition := []byte(`name: build
start: freeze
nodes:
  - id: source
    block: understand
  - id: impl
    block: implement
    in: {plan: source.out}
  - id: freeze
    block: freeze
    in: {branch: impl.out}
    next: accept_gate
  - id: accept_gate
    block: gate.roundtable
    in: {src: freeze.out}
    params: {roundtable: default}
    on_pass: done
    on_fail: impl
  - id: done
    block: pr.open
    in: {src: freeze.out}
`)
	if err := os.WriteFile(filepath.Join(workflowDir, "build.yaml"), definition, 0o600); err != nil {
		t.Fatal(err)
	}
	def, err := wfe.ParseDefinition(definition)
	if err != nil {
		t.Fatal(err)
	}
	store, err := db1.Open(filepath.Join(root, "aimee.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	artifacts, err := wfe.NewArtifactStore(filepath.Join(root, "artifacts"))
	if err != nil {
		t.Fatal(err)
	}
	if err := artifacts.PutProposal("wi_artifacts", []byte("proposal")); err != nil {
		t.Fatal(err)
	}
	if _, err := artifacts.PutNodeArtifact("wi_artifacts", "impl", "branch", []byte("head")); err != nil {
		t.Fatal(err)
	}
	if err := artifacts.PutPlan("wi_artifacts", []byte("unrelated plan")); err != nil {
		t.Fatal(err)
	}
	if err := store.CreateWorkItem(context.Background(), db1.CreateWorkItem{ID: "wi_artifacts", Repo: "repo", ProposalPath: "p", WorkflowName: "build", WorkflowVersion: def.Version, StartStage: "freeze"}); err != nil {
		t.Fatal(err)
	}
	runner := &artifactRoutingRunner{t: t}
	eng, err := New(store, artifacts, workflowDir, runner)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := eng.Advance(context.Background(), "wi_artifacts"); err != nil {
		t.Fatal(err)
	}
	out, err := eng.Advance(context.Background(), "wi_artifacts")
	if err != nil {
		t.Fatal(err)
	}
	if out.NextStage != "impl" || out.Parked {
		t.Fatalf("review transition=%+v", out)
	}
}

func TestPlanGateRefinementIsLosslessEndToEnd(t *testing.T) {
	root := t.TempDir()
	workflowDir := filepath.Join(root, "workflows")
	if err := os.MkdirAll(workflowDir, 0o700); err != nil {
		t.Fatal(err)
	}
	definition := []byte(`
name: build
start: plan
nodes:
  - id: plan
    block: author.plan
    in: {proposal: source.out}
    next: plan_gate
  - id: source
    block: author.proposal
    next: plan
  - id: plan_gate
    block: gate.roundtable
    in: {src: plan.out}
    params: {roundtable: default, max_rounds: 24}
    on_pass: done
    on_fail: plan
  - id: done
    block: gate.deliver
    in: {verdict: plan_gate.out}
`)
	// The graph validator allows a producer before/after its consumer; execution
	// starts at plan because this test seeds an already-approved proposal.
	if err := os.WriteFile(filepath.Join(workflowDir, "build.yaml"), definition, 0o600); err != nil {
		t.Fatal(err)
	}
	def, err := wfe.ParseDefinition(definition)
	if err != nil {
		t.Fatal(err)
	}
	store, err := db1.Open(filepath.Join(root, "aimee.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	artifacts, err := wfe.NewArtifactStore(filepath.Join(root, "artifacts"))
	if err != nil {
		t.Fatal(err)
	}
	proposal := strings.Repeat("proposal criterion λ\n", 120_000) + "PROPOSAL_END"
	firstPlan := strings.Repeat("initial plan detail 漢字\n", 120_000) + "FIRST_PLAN_END"
	secondPlan := strings.Repeat("revised plan detail 🚀\n", 120_000) + "SECOND_PLAN_END"
	feedback := wfe.ReviewFeedback{SchemaVersion: 1, Findings: []wfe.Finding{{
		ID: "complete-tail", Persona: "qa", Severity: "blocking",
		Summary:        strings.Repeat("observed; ", 100_000) + "SUMMARY_END",
		Recommendation: strings.Repeat("repair; ", 100_000) + "RECOMMENDATION_END",
	}}}
	if err := artifacts.PutProposal("wi_e2e", []byte(proposal)); err != nil {
		t.Fatal(err)
	}
	if _, err := artifacts.PutNodeArtifact("wi_e2e", "source", "proposal", []byte(proposal)); err != nil {
		t.Fatal(err)
	}
	if err := store.CreateWorkItem(context.Background(), db1.CreateWorkItem{
		ID: "wi_e2e", Repo: "repo", ProposalPath: "imported", WorkflowName: "build",
		WorkflowVersion: def.Version, StartStage: "plan", Mode: "autonomous",
	}); err != nil {
		t.Fatal(err)
	}
	runner := &scriptedRunner{t: t, proposal: proposal, firstPlan: firstPlan,
		secondPlan: secondPlan, feedback: feedback}
	engine, err := New(store, artifacts, workflowDir, runner)
	if err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 5; i++ {
		if _, err := engine.Advance(context.Background(), "wi_e2e"); err != nil {
			t.Fatalf("advance %d: %v", i+1, err)
		}
	}
	item, err := store.WorkItem(context.Background(), "wi_e2e")
	if err != nil {
		t.Fatal(err)
	}
	if item.State != "accepted" || runner.call != 5 {
		t.Fatalf("item=%+v calls=%d", item, runner.call)
	}
}

type unmeasuredSpendRunner struct{ reported float64 }

func (r unmeasuredSpendRunner) Run(_ context.Context, _ StepRequest) (StepResult, error) {
	return StepResult{Status: StepAdvanced, Artifact: "completed", CostUSD: r.reported, CostUnknown: true}, nil
}

// A delegate whose audit row is missing reports no measurement. Committing its
// reported zero would let real provider spend look free and silently drain the
// tree budget, so the engine charges the authorization it granted instead.
func TestUnmeasuredSpendIsChargedAtTheReservation(t *testing.T) {
	root := t.TempDir()
	workflowDir := filepath.Join(root, "workflows")
	if err := os.MkdirAll(workflowDir, 0o700); err != nil {
		t.Fatal(err)
	}
	definition := []byte("name: one\nstart: work\nnodes:\n  - id: work\n    block: author.proposal\n    next: done\n  - id: done\n    block: author.proposal\n")
	if err := os.WriteFile(filepath.Join(workflowDir, "one.yaml"), definition, 0o600); err != nil {
		t.Fatal(err)
	}
	def, err := wfe.ParseDefinition(definition)
	if err != nil {
		t.Fatal(err)
	}
	store, err := db1.Open(filepath.Join(root, "aimee.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	artifacts, err := wfe.NewArtifactStore(filepath.Join(root, "artifacts"))
	if err != nil {
		t.Fatal(err)
	}
	item := db1.CreateWorkItem{ID: "wi_unmeasured", Repo: "repo", ProposalPath: "unmeasured", WorkflowName: "one", WorkflowVersion: def.Version, StartStage: "work", MaxCostUSD: 1}
	if err := store.CreateWorkItem(t.Context(), item); err != nil {
		t.Fatal(err)
	}
	if err := artifacts.PutProposal(item.ID, []byte("proposal")); err != nil {
		t.Fatal(err)
	}
	workflowEngine, err := New(store, artifacts, workflowDir, unmeasuredSpendRunner{reported: 0})
	if err != nil {
		t.Fatal(err)
	}
	if advance, err := workflowEngine.Advance(t.Context(), item.ID); err != nil || !advance.Ran {
		t.Fatalf("advance=%+v err=%v", advance, err)
	}
	got, err := store.WorkItem(t.Context(), item.ID)
	if err != nil {
		t.Fatal(err)
	}
	// The whole uncommitted root budget was this invocation's reservation.
	if got.CumulativeCostUSD != 1 {
		t.Fatalf("unmeasured spend committed as %v, want the full 1.00 reservation: %+v", got.CumulativeCostUSD, got)
	}
	if got.ReservationState != "" || got.ReservedCostUSD != 0 {
		t.Fatalf("reservation was not consumed: %+v", got)
	}
}

// A measured zero is genuinely free and must stay zero.
func TestMeasuredZeroCostIsNotInflated(t *testing.T) {
	root := t.TempDir()
	workflowDir := filepath.Join(root, "workflows")
	if err := os.MkdirAll(workflowDir, 0o700); err != nil {
		t.Fatal(err)
	}
	definition := []byte("name: one\nstart: work\nnodes:\n  - id: work\n    block: author.proposal\n    next: done\n  - id: done\n    block: author.proposal\n")
	if err := os.WriteFile(filepath.Join(workflowDir, "one.yaml"), definition, 0o600); err != nil {
		t.Fatal(err)
	}
	def, err := wfe.ParseDefinition(definition)
	if err != nil {
		t.Fatal(err)
	}
	store, err := db1.Open(filepath.Join(root, "aimee.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	artifacts, err := wfe.NewArtifactStore(filepath.Join(root, "artifacts"))
	if err != nil {
		t.Fatal(err)
	}
	item := db1.CreateWorkItem{ID: "wi_measured_zero", Repo: "repo", ProposalPath: "measured-zero", WorkflowName: "one", WorkflowVersion: def.Version, StartStage: "work", MaxCostUSD: 1}
	if err := store.CreateWorkItem(t.Context(), item); err != nil {
		t.Fatal(err)
	}
	if err := artifacts.PutProposal(item.ID, []byte("proposal")); err != nil {
		t.Fatal(err)
	}
	workflowEngine, err := New(store, artifacts, workflowDir, breakArtifactStoreAfterSpendRunner{artifactRoot: filepath.Join(root, "unused"), cost: 0})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := workflowEngine.Advance(t.Context(), item.ID); err != nil {
		t.Fatal(err)
	}
	got, err := store.WorkItem(t.Context(), item.ID)
	if err != nil {
		t.Fatal(err)
	}
	if got.CumulativeCostUSD != 0 {
		t.Fatalf("measured zero was inflated to %v: %+v", got.CumulativeCostUSD, got)
	}
}

type replayUnavailableRunner struct{ t *testing.T }

func (r replayUnavailableRunner) Run(_ context.Context, req StepRequest) (StepResult, error) {
	if !req.ReplayOnly {
		r.t.Fatal("replay-unavailable runner invoked without ReplayOnly")
	}
	return StepResult{}, &DelegateExecutionError{Err: ErrDelegateReplayUnavailable, Dispatched: true, CostKnown: false}
}

// A replay-only invocation whose durable result is gone must recover, not loop:
// an interrupted (unresolved) reservation re-dispatches; a lost reconciled
// (actual) one parks for a human.
func TestLostReplayRecoversInsteadOfLooping(t *testing.T) {
	setup := func(t *testing.T, id string) (*db1.Store, *Engine, string) {
		root := t.TempDir()
		workflowDir := filepath.Join(root, "workflows")
		if err := os.MkdirAll(workflowDir, 0o700); err != nil {
			t.Fatal(err)
		}
		definition := []byte("name: one\nstart: work\nnodes:\n  - id: work\n    block: author.proposal\n")
		if err := os.WriteFile(filepath.Join(workflowDir, "one.yaml"), definition, 0o600); err != nil {
			t.Fatal(err)
		}
		def, err := wfe.ParseDefinition(definition)
		if err != nil {
			t.Fatal(err)
		}
		store, err := db1.Open(filepath.Join(root, "aimee.db"))
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { _ = store.Close() })
		artifacts, err := wfe.NewArtifactStore(filepath.Join(root, "artifacts"))
		if err != nil {
			t.Fatal(err)
		}
		item := db1.CreateWorkItem{ID: id, Repo: "repo", ProposalPath: id, WorkflowName: "one", WorkflowVersion: def.Version, StartStage: "work", MaxCostUSD: 1}
		if err := store.CreateWorkItem(t.Context(), item); err != nil {
			t.Fatal(err)
		}
		if err := artifacts.PutProposal(item.ID, []byte("proposal")); err != nil {
			t.Fatal(err)
		}
		eng, err := New(store, artifacts, workflowDir, replayUnavailableRunner{t: t})
		if err != nil {
			t.Fatal(err)
		}
		return store, eng, filepath.Join(root, "aimee.db")
	}

	t.Run("unresolved_redispatches", func(t *testing.T) {
		store, eng, dbPath := setup(t, "wi_replay_lost_unresolved")
		if _, err := store.ReserveWorkflowBudget(t.Context(), "wi_replay_lost_unresolved", "o1"); err != nil {
			t.Fatal(err)
		}
		if err := store.ParkRunnerFailure(t.Context(), "wi_replay_lost_unresolved", "work", "o1", "runner_unavailable", "d", true, false, 0); err != nil {
			t.Fatal(err)
		}
		execOnDB(t, dbPath, `UPDATE lifecycle_work_item SET pause_reason='', reservation_lease_until=datetime('now','-1 minute') WHERE work_item_id=?`, "wi_replay_lost_unresolved")
		out, err := eng.Advance(t.Context(), "wi_replay_lost_unresolved")
		if err != nil || out.Parked || out.Ran {
			t.Fatalf("expected a silent redispatch: out=%+v err=%v", out, err)
		}
		got, err := store.WorkItem(t.Context(), "wi_replay_lost_unresolved")
		if err != nil || got.State != "active" || got.PauseReason != "" || got.ReservationState != "" {
			t.Fatalf("item not left runnable: %+v err=%v", got, err)
		}
	})

	t.Run("actual_parks_for_human", func(t *testing.T) {
		store, eng, dbPath := setup(t, "wi_replay_lost_actual")
		if _, err := store.ReserveWorkflowBudget(t.Context(), "wi_replay_lost_actual", "o1"); err != nil {
			t.Fatal(err)
		}
		if allowed, err := store.ReconcileWorkflowBudget(t.Context(), "wi_replay_lost_actual", "o1", 0.3); err != nil || !allowed {
			t.Fatalf("reconcile allowed=%v err=%v", allowed, err)
		}
		execOnDB(t, dbPath, `UPDATE lifecycle_work_item SET reservation_lease_until=datetime('now','-1 minute') WHERE work_item_id=?`, "wi_replay_lost_actual")
		out, err := eng.Advance(t.Context(), "wi_replay_lost_actual")
		if err != nil || !out.Parked || out.PauseReason != "replay_unrecoverable" {
			t.Fatalf("expected park replay_unrecoverable: out=%+v err=%v", out, err)
		}
		got, err := store.WorkItem(t.Context(), "wi_replay_lost_actual")
		if err != nil || got.PauseReason != "replay_unrecoverable" || got.CumulativeCostUSD != 0.3 || got.ReservationState != "" {
			t.Fatalf("expected committed cost + human park: %+v err=%v", got, err)
		}
	})
}

type alwaysFailingRunner struct{ calls int }

func (r *alwaysFailingRunner) Run(context.Context, StepRequest) (StepResult, error) {
	r.calls++
	return StepResult{}, &DelegateExecutionError{Err: ErrDelegateTerminal, Dispatched: true, CostKnown: true}
}

// A stage that keeps failing without advancing must stop auto-resuming and park
// for a human rather than retrying on the transient backoff forever.
func TestPersistentRunnerFailureParksForHuman(t *testing.T) {
	root := t.TempDir()
	workflowDir := filepath.Join(root, "workflows")
	if err := os.MkdirAll(workflowDir, 0o700); err != nil {
		t.Fatal(err)
	}
	definition := []byte("name: one\nstart: work\nnodes:\n  - id: work\n    block: author.proposal\n")
	if err := os.WriteFile(filepath.Join(workflowDir, "one.yaml"), definition, 0o600); err != nil {
		t.Fatal(err)
	}
	def, err := wfe.ParseDefinition(definition)
	if err != nil {
		t.Fatal(err)
	}
	store, err := db1.Open(filepath.Join(root, "aimee.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	artifacts, err := wfe.NewArtifactStore(filepath.Join(root, "artifacts"))
	if err != nil {
		t.Fatal(err)
	}
	item := db1.CreateWorkItem{ID: "wi_persistent_fail", Repo: "repo", ProposalPath: "pf", WorkflowName: "one", WorkflowVersion: def.Version, StartStage: "work", MaxCostUSD: 100}
	if err := store.CreateWorkItem(t.Context(), item); err != nil {
		t.Fatal(err)
	}
	if err := artifacts.PutProposal(item.ID, []byte("proposal")); err != nil {
		t.Fatal(err)
	}
	runner := &alwaysFailingRunner{}
	eng, err := New(store, artifacts, workflowDir, runner)
	if err != nil {
		t.Fatal(err)
	}
	var last AdvanceResult
	for attempt := 0; attempt < maxRunnerFailuresWithoutProgress+2; attempt++ {
		out, err := eng.Advance(t.Context(), item.ID)
		if err != nil {
			t.Fatalf("advance %d: %v", attempt, err)
		}
		last = out
		if out.PauseReason == "delegate_failed" {
			break
		}
		// Simulate the scheduler's transient resume so the next attempt runs.
		if _, err := store.ResumeTransient(t.Context(), "runner_unavailable", 0); err != nil {
			t.Fatalf("resume %d: %v", attempt, err)
		}
	}
	if last.PauseReason != "delegate_failed" {
		t.Fatalf("persistent failure never parked for human: %+v after %d calls", last, runner.calls)
	}
	got, err := store.WorkItem(t.Context(), item.ID)
	if err != nil || got.PauseReason != "delegate_failed" {
		t.Fatalf("item not parked delegate_failed: %+v err=%v", got, err)
	}
}

type rateLimitedRunner struct{ calls int }

func (r *rateLimitedRunner) Run(context.Context, StepRequest) (StepResult, error) {
	r.calls++
	return StepResult{}, &DelegateExecutionError{
		Err:        fmt.Errorf("vault credential for agent 'MiniMax-M3' is rate-limited; retry in 30s"),
		Dispatched: false,
	}
}

// Provider capacity refusals are self-clearing and advertise their own
// retry-after, so waiting them out must not consume the runner-failure bound
// that parks an item for a human. Before this was distinguished, a throttled
// credential parked a live run permanently within a minute.
func TestCapacityBackpressureDoesNotParkForHuman(t *testing.T) {
	root := t.TempDir()
	workflowDir := filepath.Join(root, "workflows")
	if err := os.MkdirAll(workflowDir, 0o700); err != nil {
		t.Fatal(err)
	}
	definition := []byte("name: one\nstart: work\nnodes:\n  - id: work\n    block: author.proposal\n")
	if err := os.WriteFile(filepath.Join(workflowDir, "one.yaml"), definition, 0o600); err != nil {
		t.Fatal(err)
	}
	def, err := wfe.ParseDefinition(definition)
	if err != nil {
		t.Fatal(err)
	}
	store, err := db1.Open(filepath.Join(root, "aimee.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	artifacts, err := wfe.NewArtifactStore(filepath.Join(root, "artifacts"))
	if err != nil {
		t.Fatal(err)
	}
	item := db1.CreateWorkItem{ID: "wi_capacity", Repo: "repo", ProposalPath: "pf", WorkflowName: "one", WorkflowVersion: def.Version, StartStage: "work", MaxCostUSD: 100}
	if err := store.CreateWorkItem(t.Context(), item); err != nil {
		t.Fatal(err)
	}
	if err := artifacts.PutProposal(item.ID, []byte("proposal")); err != nil {
		t.Fatal(err)
	}
	runner := &rateLimitedRunner{}
	eng, err := New(store, artifacts, workflowDir, runner)
	if err != nil {
		t.Fatal(err)
	}
	for attempt := 0; attempt < maxRunnerFailuresWithoutProgress+4; attempt++ {
		out, err := eng.Advance(t.Context(), item.ID)
		if err != nil {
			t.Fatalf("advance %d: %v", attempt, err)
		}
		if out.PauseReason != "capacity_backpressure" {
			t.Fatalf("attempt %d: capacity refusal parked %q, want capacity_backpressure", attempt, out.PauseReason)
		}
		if _, err := store.ResumeTransient(t.Context(), "capacity_backpressure", 0); err != nil {
			t.Fatalf("resume %d: %v", attempt, err)
		}
	}
}

// A stage parked for a human must be releasable by a human once the underlying
// delegate problem is fixed.
func TestDelegateFailedIsOperatorResumable(t *testing.T) {
	root := t.TempDir()
	store, err := db1.Open(filepath.Join(root, "aimee.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	item := db1.CreateWorkItem{ID: "wi_parked", Repo: "repo", ProposalPath: "pf", WorkflowName: "one", WorkflowVersion: "v1", StartStage: "work", MaxCostUSD: 100}
	if err := store.CreateWorkItem(t.Context(), item); err != nil {
		t.Fatal(err)
	}
	if err := store.Park(t.Context(), item.ID, "work", "delegate_failed", 0); err != nil {
		t.Fatal(err)
	}
	if err := store.Resume(t.Context(), item.ID); err != nil {
		t.Fatalf("operator cannot release a park meant for a human: %v", err)
	}
	got, err := store.WorkItem(t.Context(), item.ID)
	if err != nil || got.PauseReason != "" {
		t.Fatalf("item still paused: %+v err=%v", got, err)
	}
}

// max_rounds is the per-node repeat budget. It replaced max_iters, which named
// the same thing less clearly; a node declaring only max_rounds used to fall
// through to the default and loop far past its declared cap.
func TestMaxRoundsIsTheNodeRepeatBudget(t *testing.T) {
	for _, tc := range []struct {
		name   string
		params map[string]any
		want   int
	}{
		{"declared", map[string]any{"max_rounds": 6}, 6},
		{"absent falls back to the default", map[string]any{}, 20},
		{"zero is not a budget", map[string]any{"max_rounds": 0}, 20},
		{"negative is not a budget", map[string]any{"max_rounds": -3}, 20},
		{"retired max_iters no longer caps", map[string]any{"max_iters": 3}, 20},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if got := maxIterations(wfe.Node{ID: "gate", Params: tc.params}); got != tc.want {
				t.Fatalf("maxIterations=%d, want %d", got, tc.want)
			}
		})
	}
}

// The runner's context carries the step deadline. A park happens AFTER the
// delegate ran and its spend was reconciled, so inheriting that cancellation
// loses the transition and strands the reservation in 'actual' — the exact
// state the next replay can only park as replay_unrecoverable, which no
// operator can resume. Observed in wi_ee32a2e3: "begin park transition:
// context deadline exceeded", then an unrecoverable park one round later.
func TestParkSurvivesACancelledStepContext(t *testing.T) {
	root := t.TempDir()
	workflowDir := filepath.Join(root, "workflows")
	if err := os.MkdirAll(workflowDir, 0o700); err != nil {
		t.Fatal(err)
	}
	definition := []byte("name: one\nstart: draft\nnodes:\n  - id: draft\n    block: author.proposal\n    next: plan\n  - id: plan\n    block: author.plan\n    in:\n      proposal: draft.out\n")
	if err := os.WriteFile(filepath.Join(workflowDir, "one.yaml"), definition, 0o600); err != nil {
		t.Fatal(err)
	}
	def, err := wfe.ParseDefinition(definition)
	if err != nil {
		t.Fatal(err)
	}
	store, err := db1.Open(filepath.Join(root, "aimee.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = store.Close() })
	artifacts, err := wfe.NewArtifactStore(filepath.Join(root, "artifacts"))
	if err != nil {
		t.Fatal(err)
	}
	item := db1.CreateWorkItem{ID: "wi_park_cancelled", Repo: "repo", ProposalPath: "p",
		WorkflowName: "one", WorkflowVersion: def.Version, StartStage: "draft", MaxCostUSD: 1}
	if err := store.CreateWorkItem(t.Context(), item); err != nil {
		t.Fatal(err)
	}
	if err := artifacts.PutProposal(item.ID, []byte("proposal")); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(t.Context())
	eng, err := New(store, artifacts, workflowDir, cancelBeforeParkRunner{cancel: cancel})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := eng.Advance(ctx, "wi_park_cancelled"); err != nil {
		t.Fatalf("advance draft: %v", err)
	}
	if _, err := eng.Advance(ctx, "wi_park_cancelled"); err != nil {
		t.Fatalf("advance plan: %v", err)
	}
	got, err := store.WorkItem(t.Context(), "wi_park_cancelled")
	if err != nil {
		t.Fatal(err)
	}
	if got.PauseReason != "plan_missing" {
		t.Fatalf("park lost to the cancelled step context: pause_reason=%q", got.PauseReason)
	}
	if got.ReservationState != "" || got.ReservedCostUSD != 0 {
		t.Fatalf("reservation stranded by a lost park: state=%q reserved=%v",
			got.ReservationState, got.ReservedCostUSD)
	}
}

// Cancels the step context the way an expiring deadline would, then returns a
// result that forces the engine down its park-after-spend path.
type cancelBeforeParkRunner struct{ cancel context.CancelFunc }

func (r cancelBeforeParkRunner) Run(_ context.Context, req StepRequest) (StepResult, error) {
	if req.Node.Block != "author.plan" {
		return StepResult{Status: StepAdvanced, Artifact: "a proposal"}, nil
	}
	r.cancel()
	return StepResult{Status: StepAdvanced, CostUSD: 0.2}, nil
}

// A gate that spends its whole round budget is evidence about the plan or the
// request, but the park recorded only "convergence_limit" -- a spent budget with
// no statement of what was never fixed. Observed on wi_79e96261: ten rounds, and
// the operator had to open the feedback artifact to learn the gate had been stuck
// on a subject declaration the proposal never defined.
func TestUnresolvedBlockersSummarisesWhatHeldTheGate(t *testing.T) {
	if got := unresolvedBlockers(nil); got != "" {
		t.Fatalf("no feedback must summarise to nothing, got %q", got)
	}
	feedback := &wfe.ReviewFeedback{Findings: []wfe.Finding{
		{Severity: "suggestion", Persona: "qa", Summary: "could be tidier"},
		{Severity: "blocking", Persona: "architect", Summary: "the sweep has no acceptance criterion",
			Recommendation: "list the files to move"},
		{Severity: "nit", Persona: "qa", Summary: "typo"},
		{Severity: "foundational", Persona: "chairman", Summary: "the declared subject is never defined",
			Recommendation: "amend the proposal to define it"},
	}}
	got := unresolvedBlockers(feedback)
	// The chairman's verdict overrides the seats', so its reason must lead: it is
	// the run's stated explanation of why the work failed.
	if !strings.HasPrefix(got, "[chairman] the declared subject is never defined") {
		t.Fatalf("chairman's review must come first: %q", got)
	}
	if !strings.Contains(got, "-> amend the proposal to define it") {
		t.Fatalf("recommendation must survive so the reader knows what to do: %q", got)
	}
	if !strings.Contains(got, "the sweep has no acceptance criterion") {
		t.Fatalf("other blocking findings must still appear: %q", got)
	}
	// Suggestions and nits never held the gate, so naming them would misdirect
	// whoever reads the park.
	if strings.Contains(got, "could be tidier") || strings.Contains(got, "typo") {
		t.Fatalf("non-blocking findings must not appear: %q", got)
	}
}

// The detail lands on an append-only event row, so an unbounded summary would
// bloat every park. Three findings characterise the blockage; the rest stay in
// the feedback artifact.
func TestUnresolvedBlockersIsBounded(t *testing.T) {
	feedback := &wfe.ReviewFeedback{}
	for i := 0; i < 6; i++ {
		feedback.Findings = append(feedback.Findings, wfe.Finding{
			Severity: "blocking", Summary: strings.Repeat("x", 400),
			Recommendation: strings.Repeat("y", 400)})
	}
	got := unresolvedBlockers(feedback)
	if strings.Count(got, " | ") != 2 {
		t.Fatalf("expected at most three findings, got %d separators", strings.Count(got, " | "))
	}
	if strings.Contains(got, strings.Repeat("x", 220)) || strings.Contains(got, strings.Repeat("y", 140)) {
		t.Fatal("summary and recommendation must each be truncated")
	}
}

// conflictMergeRunner stands in for the merge step meeting a genuine content
// conflict: the discriminator in forge.go classified the forge error as
// terminal, so the runner reports StepFailed instead of StepPending.
type conflictMergeRunner struct{}

func (conflictMergeRunner) Run(context.Context, StepRequest) (StepResult, error) {
	return StepResult{Status: StepFailed,
		Detail: "merge conflict needs a content decision, no retry can resolve it"}, nil
}

// A merge that hits a content conflict must END the item, not park it. Before
// this was wired up, every merge failure became StepPending/"merge_pending",
// which the scheduler re-queues on a 15s backoff forever — so an unmergeable
// slice held the single active-root slot indefinitely and the whole run
// deadlocked. StepFailed is the status that releases it, and nothing covered
// that mapping.
func TestStepFailedFinishesItemRejectedSoTheSlotIsReleased(t *testing.T) {
	root := t.TempDir()
	workflowDir := filepath.Join(root, "workflows")
	if err := os.MkdirAll(workflowDir, 0o700); err != nil {
		t.Fatal(err)
	}
	definition := []byte(`name: slice
start: merge
nodes:
  - id: source
    block: understand
  - id: impl
    block: implement
    in: {plan: source.out}
  - id: freeze
    block: freeze
    in: {branch: impl.out}
    next: pr
  - id: pr
    block: pr.open
    in: {src: freeze.out}
    next: ci
  - id: ci
    block: gate.ci
    in: {pr: pr.out}
    on_pass: merge
    on_fail: impl
  - id: merge
    block: merge
    in: {pr: pr.out}
`)
	if err := os.WriteFile(filepath.Join(workflowDir, "slice.yaml"), definition, 0o600); err != nil {
		t.Fatal(err)
	}
	def, err := wfe.ParseDefinition(definition)
	if err != nil {
		t.Fatal(err)
	}
	store, err := db1.Open(filepath.Join(root, "aimee.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	artifacts, err := wfe.NewArtifactStore(filepath.Join(root, "artifacts"))
	if err != nil {
		t.Fatal(err)
	}
	if err := artifacts.PutProposal("wi_conflict", []byte("proposal")); err != nil {
		t.Fatal(err)
	}
	if _, err := artifacts.PutNodeArtifact("wi_conflict", "pr", "pr", []byte("https://github.com/acme/repo/pull/42")); err != nil {
		t.Fatal(err)
	}
	if err := store.CreateWorkItem(context.Background(), db1.CreateWorkItem{ID: "wi_conflict",
		Repo: "repo", ProposalPath: "p", WorkflowName: "slice",
		WorkflowVersion: def.Version, StartStage: "merge"}); err != nil {
		t.Fatal(err)
	}
	eng, err := New(store, artifacts, workflowDir, conflictMergeRunner{})
	if err != nil {
		t.Fatal(err)
	}
	out, err := eng.Advance(context.Background(), "wi_conflict")
	if err != nil {
		t.Fatalf("advance: %v", err)
	}
	if !out.Terminal || out.State != "rejected" {
		t.Fatalf("a merge conflict must finish the item rejected, got terminal=%v state=%q",
			out.Terminal, out.State)
	}
	if out.Parked || out.PauseReason != "" {
		t.Fatalf("a merge conflict must not park for retry, got parked=%v reason=%q",
			out.Parked, out.PauseReason)
	}
	item, err := store.WorkItem(context.Background(), "wi_conflict")
	if err != nil {
		t.Fatal(err)
	}
	if item.State != "rejected" {
		t.Fatalf("persisted state = %q, want rejected", item.State)
	}
}
