package engine

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	configcontract "github.com/JBailes/aimee/server-go/config"
	"github.com/JBailes/aimee/server-go/internal/api"
	"github.com/JBailes/aimee/server-go/internal/wfe"
	"github.com/JBailes/aimee/server-go/internal/workflowstore/workflowstoretest"
	appconfig "github.com/RakuenSoftware/aimee-module-config/server-go/modules/config"
)

type externalConfigStore struct{ *appconfig.Store }

func (s externalConfigStore) TriggerRules() ([]configcontract.TriggerRule, error) {
	rules, err := s.Store.TriggerRules()
	if err != nil {
		return nil, err
	}
	out := make([]configcontract.TriggerRule, len(rules))
	for i, rule := range rules {
		out[i] = configcontract.TriggerRule{
			Source: rule.Source, Event: rule.Event, Schedule: rule.Schedule, Mode: rule.Mode,
			Pipeline: configcontract.TriggerPipeline{Template: rule.Pipeline.Template,
				Workspace: rule.Pipeline.Workspace, MaxSpendUSD: rule.Pipeline.MaxSpendUSD},
		}
	}
	return out, nil
}

type e2eAgents struct {
	mu       sync.Mutex
	codeRuns int
}

func (a *e2eAgents) Delegate(_ context.Context, request DelegateRequest) (DelegateResult, error) {
	switch request.Role {
	case "review":
		return DelegateResult{Response: fmt.Sprintf(`{"run_id":%q,"artifact_hash":%q,"artifact_stage":%q,"original_request_alignment":{"status":"aligned","summary":"implements the request"},"verdict":"approve","findings":[]}`,
			request.WorkItemID, request.ArtifactHash, request.ArtifactStage)}, nil
	case "draft":
		if strings.Contains(request.Prompt, "PACKET PLAN") || strings.Contains(request.Prompt, "Decompose the complete approved plan") {
			return DelegateResult{Response: `{"schema_version":1,"packets":[{"packet_id":"p1","summary":"implement feature","target_blocks":["implement"],"dependencies":[],"acceptance_criteria":["feature exists"]}]}`}, nil
		}
		if strings.Contains(request.Prompt, "Scope the engineering task") {
			return DelegateResult{Response: `{"schema_version":1,"status":"unconfirmed","summary":"implement feature","rationale":"proposal","acceptance_criteria":["feature exists"]}`}, nil
		}
		return DelegateResult{Response: "1. Implement feature.\n2. Verify it.\n"}, nil
	case "code":
		a.mu.Lock()
		a.codeRuns++
		n := a.codeRuns
		a.mu.Unlock()
		name := "feature.txt"
		if strings.Contains(request.Prompt, "Document the complete") {
			name = "docs/feature.md"
		}
		path := filepath.Join(request.Workdir, name)
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			return DelegateResult{}, err
		}
		if err := os.WriteFile(path, []byte(fmt.Sprintf("run %d\n", n)), 0o600); err != nil {
			return DelegateResult{}, err
		}
		return DelegateResult{Response: "done"}, nil
	default:
		return DelegateResult{}, fmt.Errorf("unexpected role %s", request.Role)
	}
}

func (a *e2eAgents) DelegateGroup(ctx context.Context, requests []DelegateRequest) []DelegateGroupResult {
	return testDelegateGroup(ctx, requests, a.Delegate)
}

type passVerifier struct{}

func (passVerifier) Verify(context.Context, string) error { return nil }

type transientGateRunner struct {
	mu       sync.Mutex
	next     Runner
	failures []string
}

func (r *transientGateRunner) Run(ctx context.Context, request StepRequest) (StepResult, error) {
	r.mu.Lock()
	if request.Node.ID == "plan_gate" && len(r.failures) > 0 {
		reason := r.failures[0]
		r.failures = r.failures[1:]
		r.mu.Unlock()
		return StepResult{Status: StepPending, PauseReason: reason, Detail: "injected transient " + reason}, nil
	}
	r.mu.Unlock()
	return r.next.Run(ctx, request)
}

type e2eForge struct {
	mu    sync.Mutex
	opens []PullRequestSpec
}

func (*e2eForge) Push(ctx context.Context, _, workdir, branch string) error {
	_, err := gitText(ctx, workdir, "push", "-u", "origin", branch)
	return err
}

func (f *e2eForge) Open(_ context.Context, _ string, _ string, head, base string, spec PullRequestSpec) (PullRequest, error) {
	f.mu.Lock()
	f.opens = append(f.opens, spec)
	f.mu.Unlock()
	return PullRequest{Ref: "pr:" + head, URL: "pr:" + head, Head: head, Base: base}, nil
}
func (*e2eForge) CI(context.Context, string, string) (CIState, error) { return CIPassed, nil }
func (*e2eForge) Merge(ctx context.Context, workdir, _ string, base string) error {
	_, err := gitText(ctx, workdir, "push", "origin", "HEAD:refs/heads/"+base)
	return err
}

func TestNativeSchedulerDrivesConfiguredBuildThroughSliceToFinalPR(t *testing.T) {
	root := t.TempDir()
	bare := filepath.Join(root, "origin.git")
	repo := filepath.Join(root, "repo")
	run := func(dir string, args ...string) {
		cmd := exec.Command("git", args...)
		cmd.Dir = dir
		cmd.Env = append(os.Environ(), "GIT_AUTHOR_NAME=test", "GIT_AUTHOR_EMAIL=t@example", "GIT_COMMITTER_NAME=test", "GIT_COMMITTER_EMAIL=t@example")
		if out, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("git %v: %v: %s", args, err, out)
		}
	}
	run(root, "init", "--bare", bare)
	run(root, "clone", bare, repo)
	run(repo, "checkout", "-b", "trunk")
	if err := os.WriteFile(filepath.Join(repo, "README.md"), []byte("root\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(repo, "docs/proposals/pending"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(repo, "docs/proposals/pending/feature.md"), []byte("build feature"), 0o600); err != nil {
		t.Fatal(err)
	}
	run(repo, "add", ".")
	run(repo, "commit", "-m", "init")
	run(repo, "push", "-u", "origin", "trunk")
	run(repo, "remote", "set-head", "origin", "trunk")
	workflowDir := filepath.Join(root, "workflows")
	registry, err := wfe.NewRegistry(workflowDir)
	if err != nil {
		t.Fatal(err)
	}
	build := []byte(`name: build
start: draft
nodes:
  - id: draft
    block: author.proposal
    next: feature
  - id: feature
    block: branch.open
    params: {base: trunk}
    next: plan
  - id: plan
    block: author.plan
    in: {proposal: draft.out}
    next: plan_gate
  - id: plan_gate
    block: gate.roundtable
    in: {src: plan.out}
    params: {roundtable: default, panel: {required: [architect]}, quorum: 1}
    on_pass: split
    on_fail: plan
  - id: split
    block: split
    in: {plan: plan.out}
    next: slices
    on_fail: split
  - id: slices
    block: foreach.workflow
    in: {packets: split.out, feature: feature.out}
    params: {workflow: slice}
    next: freeze
  - id: freeze
    block: freeze
    in: {branch: slices.out}
    next: gate
  - id: gate
    block: gate.roundtable
    in: {src: freeze.out}
    params: {roundtable: default, panel: {required: [qa]}, quorum: 1}
    on_pass: document
    on_fail: split
  - id: document
    block: document
    in: {branch: slices.out}
    next: docfreeze
    on_fail: document
  - id: docfreeze
    block: freeze
    in: {branch: document.out}
    next: archive
  - id: archive
    block: source.archive
    in: {branch: document.out}
    next: final_pr
  - id: final_pr
    block: pr.open
    in: {src: docfreeze.out}
    params: {base: trunk}
`)
	slice := []byte(`name: slice
start: scope
nodes:
  - id: scope
    block: understand
    next: impl
  - id: impl
    block: implement
    in: {plan: scope.out}
    next: freeze
    on_fail: impl
  - id: freeze
    block: freeze
    in: {branch: impl.out}
    next: gate
  - id: gate
    block: gate.roundtable
    in: {src: freeze.out}
    params: {roundtable: default, panel: {required: [qa]}, quorum: 1}
    on_pass: pr
    on_fail: impl
  - id: pr
    block: pr.open
    in: {src: freeze.out}
    params: {base: feature}
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
	_, err = registry.Save("build", build, "")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := registry.Save("slice", slice, ""); err != nil {
		t.Fatal(err)
	}
	store, err := workflowstoretest.Open(t, filepath.Join(root, "db.sqlite"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	artifacts, err := wfe.NewArtifactStore(filepath.Join(root, "artifacts"))
	if err != nil {
		t.Fatal(err)
	}
	configPath := filepath.Join(root, "aimee.yaml")
	configText := "trigger:\n  max_concurrent: 2\ntrigger_rules:\n  - source: watch-dir\n    event: docs/proposals/pending\n    mode: autonomous\n    pipeline:\n      template: build\n      workspace: " + fmt.Sprintf("%q", repo) + "\n"
	if err := os.WriteFile(configPath, []byte(configText), 0o600); err != nil {
		t.Fatal(err)
	}
	configStore, err := appconfig.NewStore(configPath)
	if err != nil {
		t.Fatal(err)
	}
	handler, err := api.New(store, artifacts, workflowDir)
	if err != nil {
		t.Fatal(err)
	}
	handler.SetConfigStore(externalConfigStore{Store: configStore})
	handler.ScanTriggers(context.Background())
	items, err := store.WorkItems(context.Background())
	if err != nil || len(items) != 1 {
		t.Fatalf("scanner items=%v err=%v", items, err)
	}
	rootID := items[0].ID
	worktrees, err := NewWorktreeManager(store, filepath.Join(root, "trees"))
	if err != nil {
		t.Fatal(err)
	}
	forge := &e2eForge{}
	runner, err := NewNativeRunner(store, worktrees, &e2eAgents{}, passVerifier{}, artifacts, registry, forge)
	if err != nil {
		t.Fatal(err)
	}
	// Every gate in this workflow names "default", and a named roundtable must
	// resolve to a saved preset, so the end-to-end run needs a real one.
	withPanel(runner, unpinnedTestRoundtable(t, "architect"))
	recoveringRunner := &transientGateRunner{next: runner, failures: []string{"roundtable_discussion", "roundtable_chairman"}}
	eng, err := New(store, artifacts, workflowDir, recoveringRunner)
	if err != nil {
		t.Fatal(err)
	}
	scheduler := NewScheduler(store, eng, 2, nil)
	ctx, cancel := context.WithCancel(context.Background())
	scheduler.pollEvery = 20 * time.Millisecond
	scheduler.transientPauses = []transientPause{
		{reason: "roundtable_discussion", backoff: 100 * time.Millisecond},
		{reason: "roundtable_chairman", backoff: 100 * time.Millisecond},
	}
	done := make(chan struct{})
	go func() { scheduler.Run(ctx); close(done) }()
	shutdown := func() { cancel(); <-done }
	defer shutdown()
	// The production scheduler intentionally gives transient fan-in recovery a
	// five-second backoff. This end-to-end path needs two such recovery passes
	// (child completion, then parent continuation), so leave deterministic headroom.
	deadline := time.Now().Add(20 * time.Second)
	for time.Now().Before(deadline) {
		item, err := store.WorkItem(context.Background(), rootID)
		if err != nil {
			t.Fatal(err)
		}
		if item.State == "accepted" {
			if item.PRRef == "" {
				t.Fatal("final PR ref was not persisted")
			}
			recoveringRunner.mu.Lock()
			remainingFailures := len(recoveringRunner.failures)
			recoveringRunner.mu.Unlock()
			if remainingFailures != 0 {
				t.Fatalf("%d transient roundtable failures were not exercised", remainingFailures)
			}
			forge.mu.Lock()
			opens := append([]PullRequestSpec(nil), forge.opens...)
			forge.mu.Unlock()
			if len(opens) != 2 {
				t.Fatalf("opened %d PRs, want slice + final: %+v", len(opens), opens)
			}
			if opens[0].Draft || opens[0].Title != "Implement feature" {
				t.Fatalf("slice handoff = %+v, want meaningful non-draft slice PR", opens[0])
			}
			final := opens[1]
			if !final.Draft || final.Title != "Build feature" {
				t.Fatalf("final handoff = %+v, want meaningful draft PR", final)
			}
			for _, marker := range []string{"## Human review boundary", "intentionally a draft",
				"## What this proposal does", "## What changed", "Original request",
				"Approved implementation plan", rootID} {
				if !strings.Contains(final.Body, marker) {
					t.Fatalf("final PR body missing %q:\n%s", marker, final.Body)
				}
			}
			events, err := store.Events(context.Background(), rootID, 0, 1000)
			if err != nil {
				t.Fatal(err)
			}
			seen := map[string]bool{}
			for _, event := range events {
				if event.Kind == "pause" {
					seen[event.Detail] = true
				}
			}
			for _, reason := range []string{"roundtable_discussion: injected transient roundtable_discussion", "roundtable_chairman: injected transient roundtable_chairman"} {
				if !seen[reason] {
					t.Fatalf("missing transient recovery event %q: %+v", reason, events)
				}
			}
			shutdown = func() {}
			cancel()
			<-done
			return
		}
		if item.State != "active" {
			t.Fatalf("parent ended %+v", item)
		}
		time.Sleep(20 * time.Millisecond)
	}
	item, _ := store.WorkItem(context.Background(), rootID)
	children, _ := store.Children(context.Background(), rootID)
	events, _ := store.Events(context.Background(), rootID, 0, 1000)
	t.Fatalf("workflow timed out: parent=%+v children=%+v events=%+v", item, children, events)
}
