package engine

import (
	"context"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/internal/db1"
	"github.com/JBailes/aimee/server-go/internal/db1/db1test"
	"github.com/JBailes/aimee/server-go/internal/wfe"
)

// gitRun runs a git command in dir, failing the test on error.
func gitRun(t *testing.T, dir string, args ...string) string {
	t.Helper()
	cmd := exec.Command("git", append([]string{"-C", dir}, args...)...)
	cmd.Env = append(os.Environ(),
		"GIT_AUTHOR_NAME=t", "GIT_AUTHOR_EMAIL=t@e",
		"GIT_COMMITTER_NAME=t", "GIT_COMMITTER_EMAIL=t@e")
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("git %v: %v: %s", args, err, out)
	}
	return string(out)
}

// setupSliceRepo builds a repo with a feature branch aimee/feat/wi_parent and a
// slice worktree on aimee/wi/wi_child cut from it. Returns (repo, slicedir).
func setupSliceRepo(t *testing.T) (string, string) {
	t.Helper()
	repo := t.TempDir()
	gitRun(t, repo, "init", "-b", "trunk")
	if err := os.WriteFile(filepath.Join(repo, "README"), []byte("x\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, repo, "add", "README")
	gitRun(t, repo, "commit", "-m", "init")
	gitRun(t, repo, "branch", "aimee/feat/wi_parent", "trunk")
	slicedir := filepath.Join(t.TempDir(), "slice")
	gitRun(t, repo, "worktree", "add", slicedir, "-b", "aimee/wi/wi_child", "aimee/feat/wi_parent")
	return repo, slicedir
}

// advanceFeature commits a file onto aimee/feat/wi_parent in the main repo,
// simulating a sibling slice merging into the feature branch.
func advanceFeature(t *testing.T, repo, name, content string) {
	t.Helper()
	gitRun(t, repo, "checkout", "aimee/feat/wi_parent")
	if err := os.WriteFile(filepath.Join(repo, name), []byte(content), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, repo, "add", name)
	gitRun(t, repo, "commit", "-m", "sibling: "+name)
	gitRun(t, repo, "checkout", "trunk")
}

func TestIntegrateFeatureBasePicksUpSiblingMerge(t *testing.T) {
	repo, slicedir := setupSliceRepo(t)
	// A sibling slice merges a prerequisite into the feature branch AFTER our
	// worktree was cut.
	advanceFeature(t, repo, "prereq.go", "package prereq\n")

	// The prerequisite is not visible in the slice worktree yet.
	if _, err := os.Stat(filepath.Join(slicedir, "prereq.go")); !os.IsNotExist(err) {
		t.Fatalf("prereq.go should be absent before integration, stat err=%v", err)
	}

	park, err := integrateFeatureBase(context.Background(), slicedir, "wi_parent")
	if err != nil {
		t.Fatalf("integrateFeatureBase: %v", err)
	}
	if park != "" {
		t.Fatalf("expected clean integration, got park=%q", park)
	}
	// The prerequisite is now present — the dependent slice can proceed.
	if _, err := os.Stat(filepath.Join(slicedir, "prereq.go")); err != nil {
		t.Fatalf("prereq.go should be present after integration: %v", err)
	}
}

func TestNativeRunnerIntegrateFeatureBaseUsesResourcePlaneIdentity(t *testing.T) {
	t.Setenv("AIMEE_GIT_AUTHOR_NAME", "")
	t.Setenv("AIMEE_GIT_AUTHOR_EMAIL", "")
	repo, slicedir := setupSliceRepo(t)
	if err := os.WriteFile(filepath.Join(slicedir, "slice.txt"), []byte("slice\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, slicedir, "add", "slice.txt")
	gitRun(t, slicedir, "commit", "-m", "slice change")
	advanceFeature(t, repo, "sibling.txt", "sibling\n")

	runner := &NativeRunner{forge: fixedIdentityForge{}}
	park, err := runner.integrateFeatureBase(t.Context(), slicedir, "wi_parent")
	if err != nil || park != "" {
		t.Fatalf("integrateFeatureBase: park=%q err=%v", park, err)
	}
	author := gitRun(t, slicedir, "show", "-s", "--format=%an <%ae>")
	if strings.TrimSpace(author) != "Vault Operator <vault@example.test>" {
		t.Fatalf("merge author = %q", author)
	}
}

// A slice freeze must review only that slice's delta over the latest feature
// tip. Forge merges advance origin/aimee/feat/<parent>, not the stale local ref;
// using the local ref makes later slice artifacts include every landed sibling
// and causes strict scope review to reject unrelated prerequisite work as drift.
func TestFreezeUsesMergedRemoteFeatureTip(t *testing.T) {
	root := t.TempDir()
	origin := filepath.Join(root, "origin.git")
	repo := filepath.Join(root, "repo")
	gitRun(t, root, "init", "--bare", "-b", "trunk", origin)
	gitRun(t, root, "clone", origin, repo)
	if err := os.WriteFile(filepath.Join(repo, "README"), []byte("root\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, repo, "add", "README")
	gitRun(t, repo, "commit", "-m", "init")
	gitRun(t, repo, "push", "-u", "origin", "trunk")

	feature := "aimee/feat/wi_parent"
	gitRun(t, repo, "branch", feature)
	gitRun(t, repo, "push", "origin", feature)

	store, err := db1test.Open(t, filepath.Join(root, "db.sqlite"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = store.Close() })
	ctx := t.Context()
	for _, in := range []db1.CreateWorkItem{
		{ID: "wi_parent", Repo: repo, ProposalPath: "p", WorkflowName: "build", StartStage: "feature"},
		{ID: "wi_child", Repo: repo, ProposalPath: "c", WorkflowName: "slice", StartStage: "freeze", ParentID: "wi_parent"},
	} {
		if err := store.CreateWorkItem(ctx, in); err != nil {
			t.Fatal(err)
		}
	}
	manager, err := NewWorktreeManager(store, filepath.Join(root, "trees"))
	if err != nil {
		t.Fatal(err)
	}
	child, err := store.WorkItem(ctx, "wi_child")
	if err != nil {
		t.Fatal(err)
	}
	workdir, _, err := manager.Ensure(ctx, child, false)
	if err != nil {
		t.Fatal(err)
	}

	// Simulate a sibling PR merging after this child worktree was created.
	landed := filepath.Join(root, "landed")
	gitRun(t, root, "clone", "-b", feature, origin, landed)
	if err := os.WriteFile(filepath.Join(landed, "sibling.txt"), []byte("landed\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, landed, "add", "sibling.txt")
	gitRun(t, landed, "commit", "-m", "sibling")
	gitRun(t, landed, "push", "origin", feature)
	if park, err := integrateFeatureBase(ctx, workdir, "wi_parent"); err != nil || park != "" {
		t.Fatalf("integrateFeatureBase: park=%q err=%v", park, err)
	}

	if err := os.WriteFile(filepath.Join(workdir, "child.txt"), []byte("child\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, workdir, "add", "child.txt")
	gitRun(t, workdir, "commit", "-m", "child")

	runner := &NativeRunner{db: store, worktrees: manager}
	result, err := runner.freeze(ctx, StepRequest{WorkItem: child})
	if err != nil {
		t.Fatal(err)
	}
	if result.Status != StepAdvanced {
		t.Fatalf("freeze status=%q detail=%q", result.Status, result.Detail)
	}
	if strings.Contains(result.Artifact, "sibling.txt") {
		t.Fatalf("freeze leaked previously merged sibling work:\n%s", result.Artifact)
	}
	if !strings.Contains(result.Artifact, "child.txt") {
		t.Fatalf("freeze omitted this slice's change:\n%s", result.Artifact)
	}
}

func TestIntegrateFeatureBaseNoopWhenAlreadyCurrent(t *testing.T) {
	repo, slicedir := setupSliceRepo(t)
	_ = repo
	// Base == HEAD's ancestor already: integration is a no-op, no park.
	park, err := integrateFeatureBase(context.Background(), slicedir, "wi_parent")
	if err != nil || park != "" {
		t.Fatalf("expected clean no-op, got park=%q err=%v", park, err)
	}
}

func TestIntegrateFeatureBaseConflictAbortsCleanly(t *testing.T) {
	repo, slicedir := setupSliceRepo(t)

	// The slice edits shared.go one way...
	if err := os.WriteFile(filepath.Join(slicedir, "shared.go"), []byte("package a // slice\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, slicedir, "add", "shared.go")
	gitRun(t, slicedir, "commit", "-m", "slice edit")

	// ...while a sibling edits the same file the other way on the feature branch.
	advanceFeature(t, repo, "shared.go", "package a // sibling\n")

	park, err := integrateFeatureBase(context.Background(), slicedir, "wi_parent")
	if err != nil {
		t.Fatalf("integrateFeatureBase returned err: %v", err)
	}
	if park != "base_integration_conflict" {
		t.Fatalf("expected base_integration_conflict, got %q", park)
	}
	// The worktree must be left clean — never half-merged.
	if status := gitRun(t, slicedir, "status", "--porcelain"); status != "" {
		t.Fatalf("worktree not clean after conflict abort: %q", status)
	}
	// No merge must be in progress (MERGE_HEAD absent).
	cmd := exec.Command("git", "-C", slicedir, "rev-parse", "-q", "--verify", "MERGE_HEAD")
	if err := cmd.Run(); err == nil {
		t.Fatal("MERGE_HEAD still present: merge was not aborted")
	}
	// The slice's own commit survives.
	if _, err := os.Stat(filepath.Join(slicedir, "shared.go")); err != nil {
		t.Fatalf("slice's own shared.go should survive the abort: %v", err)
	}
}

// A delegate that reports partial and writes nothing must fail its slice, not
// advance it. Observed on wi_3d5de168: every implement job came back
//
//	partial — "named file 'docs/runbooks/appliance-state-recovery.md' was not
//	created by delegate ... The task remains unimplemented"
//
// and the engine advanced anyway. freeze then saw an empty diff, read it as "the
// work is already in the base", and accepted the slice — so the run reached
// done=5 with no commits, no file and no PR, recorded as success.
type partialNoCommitAgents struct{}

func (partialNoCommitAgents) Delegate(_ context.Context, _ DelegateRequest) (DelegateResult, error) {
	return DelegateResult{
		Response: "named file 'docs/runbooks/x.md' was not created by delegate; the task remains unimplemented",
		Partial:  true,
	}, nil
}

type documentedNoopAgents struct{}

func (documentedNoopAgents) Delegate(_ context.Context, _ DelegateRequest) (DelegateResult, error) {
	return DelegateResult{
		Response: "Partial result; delegate ended with error: delegate code: no owned files changed; " +
			"result treated as incomplete\n\nNo changes made. Documentation is already complete.",
		Partial: true,
	}, nil
}

func (documentedNoopAgents) DelegateGroup(_ context.Context, requests []DelegateRequest) []DelegateGroupResult {
	return make([]DelegateGroupResult, len(requests))
}

type implementationSatisfiedNoopAgents struct{}

func (implementationSatisfiedNoopAgents) Delegate(_ context.Context, _ DelegateRequest) (DelegateResult, error) {
	return DelegateResult{
		Response: "Partial result; delegate ended with error: delegate code: no owned files changed; " +
			"result treated as incomplete\n\nTask already complete on the current branch. No changes made.",
		Partial: true,
	}, nil
}

func (implementationSatisfiedNoopAgents) DelegateGroup(_ context.Context, requests []DelegateRequest) []DelegateGroupResult {
	return make([]DelegateGroupResult, len(requests))
}

func (a partialNoCommitAgents) DelegateGroup(_ context.Context, requests []DelegateRequest) []DelegateGroupResult {
	out := make([]DelegateGroupResult, len(requests))
	for i := range requests {
		out[i] = DelegateGroupResult{Response: "partial"}
	}
	return out
}

type rejectDelegateAgents struct {
	calls int
	last  DelegateRequest
}

func (a *rejectDelegateAgents) Delegate(_ context.Context, request DelegateRequest) (DelegateResult, error) {
	a.calls++
	a.last = request
	return DelegateResult{}, errors.New("delegate must not run for an already-repaired reviewed tree")
}

type recordingVerifier struct {
	calls int
	err   error
}

func (v *recordingVerifier) Verify(context.Context, string) error {
	v.calls++
	return v.err
}

func TestReviewResumeRefreezesHumanRepairWithoutMeaninglessDelegateEdit(t *testing.T) {
	root := t.TempDir()
	repo := filepath.Join(root, "repo")
	if err := os.MkdirAll(repo, 0o755); err != nil {
		t.Fatal(err)
	}
	gitRun(t, repo, "init", "-b", "trunk")
	if err := os.WriteFile(filepath.Join(repo, "README"), []byte("base\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, repo, "add", "README")
	gitRun(t, repo, "commit", "-m", "init")
	gitRun(t, repo, "remote", "add", "origin", repo)
	gitRun(t, repo, "update-ref", "refs/remotes/origin/trunk", "HEAD")
	gitRun(t, repo, "symbolic-ref", "refs/remotes/origin/HEAD", "refs/remotes/origin/trunk")

	store, err := db1test.Open(t, filepath.Join(root, "db.sqlite"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = store.Close() })
	ctx := t.Context()
	if err := store.CreateWorkItem(ctx, db1.CreateWorkItem{ID: "wi_root", Repo: repo,
		ProposalPath: "p", WorkflowName: "build", StartStage: "document"}); err != nil {
		t.Fatal(err)
	}
	manager, err := NewWorktreeManager(store, filepath.Join(root, "trees"))
	if err != nil {
		t.Fatal(err)
	}
	item, _ := store.WorkItem(ctx, "wi_root")
	workdir, branch, err := manager.Ensure(ctx, item, true)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(workdir, "runbook.md"), []byte("reviewed\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, workdir, "add", "runbook.md")
	gitRun(t, workdir, "commit", "-m", "document")
	reviewedDiff := gitRun(t, workdir, "--no-pager", "diff", "origin/trunk...HEAD")
	reviewedHash := wfe.Hash([]byte(strings.TrimSpace(reviewedDiff)))

	if err := os.WriteFile(filepath.Join(workdir, "runbook.md"), []byte("reviewed and human-repaired\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	item, _ = store.WorkItem(ctx, "wi_root")
	item.ContentHash = reviewedHash
	item.Worktree = workdir
	agents := &rejectDelegateAgents{}
	runner := &NativeRunner{agents: agents, worktrees: manager, db: store}
	request := StepRequest{WorkItem: item, Node: wfe.Node{ID: "document"},
		Feedback: &wfe.ReviewFeedback{ArtifactHash: reviewedHash}}
	if _, err := runner.mutate(ctx, request, true); err == nil || agents.calls != 1 {
		t.Fatalf("dirty repair bypassed delegate: calls=%d err=%v", agents.calls, err)
	}

	gitRun(t, workdir, "add", "runbook.md")
	gitRun(t, workdir, "commit", "-m", "human repair")
	agents.calls = 0
	verifier := &recordingVerifier{err: errors.New("repair still fails verification")}
	runner.verifier = verifier
	implementationRequest := StepRequest{WorkItem: item, Node: wfe.Node{ID: "impl"},
		Feedback: &wfe.ReviewFeedback{ArtifactHash: reviewedHash}}
	result, err := runner.mutate(ctx, implementationRequest, false)
	if err != nil || result.Status != StepChanges ||
		!strings.Contains(result.Detail, "repair still fails verification") {
		t.Fatalf("unverified implementation repair result=%+v err=%v", result, err)
	}
	if agents.calls != 0 || verifier.calls != 1 {
		t.Fatalf("unverified repair delegate calls=%d verifier calls=%d", agents.calls, verifier.calls)
	}

	verifier.err = nil
	result, err = runner.mutate(ctx, implementationRequest, false)
	if err != nil {
		t.Fatal(err)
	}
	if agents.calls != 0 || verifier.calls != 2 {
		t.Fatalf("verified repair delegate calls=%d verifier calls=%d", agents.calls, verifier.calls)
	}
	if result.Status != StepAdvanced || result.Artifact != branch ||
		!strings.Contains(result.Detail, "verified and re-freezing exact repair") {
		t.Fatalf("implementation repair result=%+v", result)
	}

	// A failed mechanical repair verification loops back to implementation.
	// That retry must call a delegate; otherwise the fast path just reruns the
	// identical failing verifier until retry_limit without giving anything a
	// chance to fix the checkout.
	if err := store.Move(ctx, item.ID, "document", "impl", "advance", "", reviewedHash, 0); err != nil {
		t.Fatal(err)
	}
	if parked, err := store.RecordRetry(ctx, item.ID, "impl", "impl", "verify failed", 3, 0); err != nil || parked {
		t.Fatalf("record implementation retry: parked=%v err=%v", parked, err)
	}
	item, _ = store.WorkItem(ctx, item.ID)
	item.ContentHash = reviewedHash
	item.Worktree = workdir
	agents.calls = 0
	verifierCalls := verifier.calls
	_, err = runner.mutate(ctx, StepRequest{WorkItem: item, Node: wfe.Node{ID: "impl"},
		Feedback: &wfe.ReviewFeedback{ArtifactHash: reviewedHash}, RetryDetail: "verify failed: clang-format violation"}, false)
	if err == nil || agents.calls != 1 {
		t.Fatalf("failed verification retry bypassed delegate: calls=%d err=%v", agents.calls, err)
	}
	if !strings.Contains(agents.last.Prompt, "PREVIOUS ATTEMPT FAILURE TO FIX:\nverify failed: clang-format violation") {
		t.Fatalf("repair delegate did not receive verifier failure: %q", agents.last.Prompt)
	}
	if verifier.calls != verifierCalls {
		t.Fatalf("failed verification retry replayed verifier: calls=%d, want %d", verifier.calls, verifierCalls)
	}

	agents.calls = 0
	result, err = runner.mutate(ctx, StepRequest{WorkItem: item, Node: wfe.Node{ID: "document"},
		Feedback: &wfe.ReviewFeedback{ArtifactHash: reviewedHash}}, true)
	if err != nil {
		t.Fatal(err)
	}
	if agents.calls != 0 {
		t.Fatalf("delegate calls=%d, want 0", agents.calls)
	}
	if result.Status != StepAdvanced || result.Artifact != branch ||
		!strings.Contains(result.Detail, "re-freezing exact repair") {
		t.Fatalf("result=%+v", result)
	}
	if head := strings.TrimSpace(gitRun(t, workdir, "rev-parse", "HEAD")); result.ContentHash != head {
		t.Fatalf("content hash=%q, want repaired HEAD %q", result.ContentHash, head)
	}

	// Removing the entire reviewed diff is not a repair to re-freeze. The normal
	// delegate path must handle it rather than letting freeze accept an empty root
	// diff as a no-op without another roundtable review.
	gitRun(t, workdir, "rm", "runbook.md")
	gitRun(t, workdir, "commit", "-m", "remove reviewed document")
	agents.calls = 0
	if _, err := runner.mutate(ctx, StepRequest{WorkItem: item, Node: wfe.Node{ID: "document"},
		Feedback: &wfe.ReviewFeedback{ArtifactHash: reviewedHash}}, true); err == nil || agents.calls != 1 {
		t.Fatalf("empty repair bypassed delegate: calls=%d err=%v", agents.calls, err)
	}
}

func TestPartialImplementWithNoCommitDoesNotAdvance(t *testing.T) {
	root := t.TempDir()
	repo := filepath.Join(root, "repo")
	if err := os.MkdirAll(repo, 0o755); err != nil {
		t.Fatal(err)
	}
	gitRun(t, repo, "init", "-b", "trunk")
	if err := os.WriteFile(filepath.Join(repo, "README"), []byte("x\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, repo, "add", "README")
	gitRun(t, repo, "commit", "-m", "init")
	gitRun(t, repo, "remote", "add", "origin", repo)
	gitRun(t, repo, "update-ref", "refs/remotes/origin/trunk", "HEAD")
	gitRun(t, repo, "symbolic-ref", "refs/remotes/origin/HEAD", "refs/remotes/origin/trunk")

	store, err := db1test.Open(t, filepath.Join(root, "db.sqlite"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = store.Close() })
	ctx := t.Context()
	for _, in := range []db1.CreateWorkItem{
		{ID: "wi_parent", Repo: repo, ProposalPath: "p", WorkflowName: "build", StartStage: "feature"},
		{ID: "wi_child", Repo: repo, ProposalPath: "c", WorkflowName: "slice", StartStage: "impl", ParentID: "wi_parent"},
	} {
		if err := store.CreateWorkItem(ctx, in); err != nil {
			t.Fatal(err)
		}
	}
	manager, err := NewWorktreeManager(store, filepath.Join(root, "trees"))
	if err != nil {
		t.Fatal(err)
	}
	parent, _ := store.WorkItem(ctx, "wi_parent")
	if _, _, err := manager.Ensure(ctx, parent, true); err != nil {
		t.Fatal(err)
	}
	child, _ := store.WorkItem(ctx, "wi_child")

	runner := &NativeRunner{agents: partialNoCommitAgents{}, worktrees: manager, db: store}
	// docs=true: the verifier is skipped on a documentation slice, which is how
	// this reached freeze with nothing in the worktree.
	out, err := runner.mutate(ctx, StepRequest{WorkItem: child, Node: wfe.Node{ID: "impl"}}, true)
	if err != nil {
		t.Fatalf("mutate: %v", err)
	}
	if out.Status == StepAdvanced {
		t.Fatal("a partial delegate that produced no commit must not advance the slice")
	}
	if out.Status != StepChanges {
		t.Fatalf("expected StepChanges, got %q", out.Status)
	}
	// The delegate already said exactly what was wrong; that is the finding.
	if !strings.Contains(out.Detail, "was not created by delegate") {
		t.Fatalf("delegate's own diagnosis must survive into the step detail: %q", out.Detail)
	}

	// The write-role guard also reports a legitimate sibling-satisfied no-op as
	// partial. The implementation prompt requires the delegate to state that the
	// task is already complete; accept only that explicit report, and still run
	// the mechanical verifier before advancing the unchanged HEAD.
	verifier := &recordingVerifier{}
	satisfiedRunner := &NativeRunner{agents: implementationSatisfiedNoopAgents{},
		worktrees: manager, db: store, verifier: verifier}
	out, err = satisfiedRunner.mutate(ctx, StepRequest{WorkItem: child, Node: wfe.Node{ID: "impl"}}, false)
	if err != nil {
		t.Fatalf("sibling-satisfied mutate: %v", err)
	}
	if out.Status != StepAdvanced {
		t.Fatalf("explicit completed no-op status=%q detail=%q, want advanced", out.Status, out.Detail)
	}
	if verifier.calls != 1 {
		t.Fatalf("explicit completed no-op verifier calls=%d, want 1", verifier.calls)
	}

	// Existing branch work must not excuse a later partial correction attempt
	// when the exact reviewed artifact still carries blocking feedback. Without
	// this check the old commit made branchHasWorkOverBase true, so the unchanged
	// retry advanced to freeze and immediately re-served the same findings.
	workdir, _, err := manager.Ensure(ctx, child, false)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(workdir, "implementation.go"), []byte("package implementation\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, workdir, "add", "implementation.go")
	gitRun(t, workdir, "commit", "-m", "initial reviewed implementation")
	child, _ = store.WorkItem(ctx, "wi_child")
	reviewedDiff, err := frozenWorktreeDiff(ctx, child, workdir)
	if err != nil {
		t.Fatal(err)
	}
	reviewedHash := wfe.Hash([]byte(reviewedDiff))
	verifier = &recordingVerifier{}
	runner.verifier = verifier
	blockingFeedback := &wfe.ReviewFeedback{ArtifactHash: reviewedHash, Findings: []wfe.Finding{{
		ID: "still-broken", Severity: "blocking", Summary: "the reviewed implementation is still broken",
	}}}
	out, err = runner.mutate(ctx, StepRequest{WorkItem: child, Node: wfe.Node{ID: "impl"},
		Feedback: blockingFeedback}, false)
	if err != nil {
		t.Fatalf("review correction mutate: %v", err)
	}
	if out.Status != StepChanges {
		t.Fatalf("partial correction of unchanged reviewed artifact status=%q, want changes", out.Status)
	}
	if verifier.calls != 0 {
		t.Fatalf("unchanged partial correction reached verifier: calls=%d", verifier.calls)
	}

	// The inverse is equally important: a prior correction attempt may have
	// committed the requested repair before its delegate result was lost or
	// downgraded to partial. Once the frozen diff differs from the reviewed
	// artifact, the retry may advance after mechanical verification even if it
	// makes no additional commit of its own.
	if err := os.WriteFile(filepath.Join(workdir, "implementation.go"),
		[]byte("package implementation\n\nconst repaired = true\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, workdir, "add", "implementation.go")
	gitRun(t, workdir, "commit", "-m", "repair reviewed implementation")
	repairedHead := strings.TrimSpace(gitRun(t, workdir, "rev-parse", "HEAD"))
	child, _ = store.WorkItem(ctx, "wi_child")
	out, err = runner.mutate(ctx, StepRequest{WorkItem: child, Node: wfe.Node{ID: "impl"},
		Feedback: blockingFeedback}, false)
	if err != nil {
		t.Fatalf("changed review correction mutate: %v", err)
	}
	if out.Status != StepAdvanced || out.ContentHash != repairedHead {
		t.Fatalf("partial correction after changed reviewed artifact result=%+v, want advanced HEAD %s", out, repairedHead)
	}
	if verifier.calls != 1 {
		t.Fatalf("changed partial correction verifier calls=%d, want 1", verifier.calls)
	}
}

func TestDocumentPartialNoChangeAdvancesUnchangedHead(t *testing.T) {
	root := t.TempDir()
	repo := filepath.Join(root, "repo")
	if err := os.MkdirAll(repo, 0o755); err != nil {
		t.Fatal(err)
	}
	gitRun(t, repo, "init", "-b", "trunk")
	if err := os.WriteFile(filepath.Join(repo, "README"), []byte("base\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, repo, "add", "README")
	gitRun(t, repo, "commit", "-m", "init")
	gitRun(t, repo, "remote", "add", "origin", repo)
	gitRun(t, repo, "update-ref", "refs/remotes/origin/trunk", "HEAD")
	gitRun(t, repo, "symbolic-ref", "refs/remotes/origin/HEAD", "refs/remotes/origin/trunk")

	store, err := db1test.Open(t, filepath.Join(root, "db.sqlite"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = store.Close() })
	ctx := t.Context()
	if err := store.CreateWorkItem(ctx, db1.CreateWorkItem{ID: "wi_root", Repo: repo,
		ProposalPath: "p", WorkflowName: "build", StartStage: "document"}); err != nil {
		t.Fatal(err)
	}
	manager, err := NewWorktreeManager(store, filepath.Join(root, "trees"))
	if err != nil {
		t.Fatal(err)
	}
	item, _ := store.WorkItem(ctx, "wi_root")
	workdir, branch, err := manager.Ensure(ctx, item, true)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(workdir, "docs.md"), []byte("already documented\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	gitRun(t, workdir, "add", "docs.md")
	gitRun(t, workdir, "commit", "-m", "accepted implementation")
	head := strings.TrimSpace(gitRun(t, workdir, "rev-parse", "HEAD"))
	item, _ = store.WorkItem(ctx, "wi_root")
	reviewedDiff, err := frozenWorktreeDiff(ctx, item, workdir)
	if err != nil {
		t.Fatal(err)
	}

	runner := &NativeRunner{agents: documentedNoopAgents{}, worktrees: manager, db: store}
	out, err := runner.mutate(ctx, StepRequest{WorkItem: item, Node: wfe.Node{ID: "document"},
		Proposal: "Document the accepted implementation.",
		Feedback: &wfe.ReviewFeedback{ArtifactHash: wfe.Hash([]byte(reviewedDiff)), Findings: []wfe.Finding{{
			ID: "optional-polish", Severity: "suggestion", Summary: "consider optional wording polish",
		}}}}, true)
	if err != nil {
		t.Fatalf("mutate: %v", err)
	}
	if out.Status != StepAdvanced || out.Artifact != branch || out.ContentHash != head {
		t.Fatalf("result=%+v, want unchanged advanced HEAD %s", out, head)
	}
	if got := strings.TrimSpace(gitRun(t, workdir, "status", "--porcelain")); got != "" {
		t.Fatalf("document no-op dirtied the worktree: %q", got)
	}
}
