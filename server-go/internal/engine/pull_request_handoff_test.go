package engine

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/internal/wfe"
	"github.com/JBailes/aimee/server-go/internal/workflowstore"
	"github.com/JBailes/aimee/server-go/internal/workflowstore/workflowstoretest"
)

const deduplicationProposal = `# Proposal: Document proposal-trigger blob deduplication

## Goal

Document the production behavior of the autonomous pending-proposal watcher so
operators can predict when a file is admitted, queued, or deliberately ignored.

## Scope

Update only ` + "`docs/wfe-autonomy-runbook.md`" + `. Add a concise section that explains:

- pending proposal identity is based on the complete file bytes, workflow, and
  mode, not the moving branch commit;
- advancing the watched branch without changing a proposal does not start a
  duplicate run;
- changing the proposal bytes makes it eligible for a new run;
- the live trigger admission cap can queue otherwise eligible proposals and is
  edited in Workflows → Run policy; and
- queued proposals remain eligible on later scans without manual firing.

## Acceptance criteria

1. The behavior above is stated precisely in ` + "`docs/wfe-autonomy-runbook.md`" + `.
2. The documentation does not imply that a commit SHA alone is a proposal
   identity.
3. No source code, workflow definition, generated documentation, or runtime
   configuration is changed.
4. Existing documentation checks pass.`

func TestPullRequestTitleUsesProposalMeaningInsteadOfWorkItemID(t *testing.T) {
	for _, test := range []struct {
		name    string
		request string
		want    string
	}{
		{name: "proposal heading", request: "# Proposal: appliance state-recovery runbook\n\n- **State:** pending", want: "Appliance state-recovery runbook"},
		{name: "plain request", request: "document automatic proposal admission in three implementation slices", want: "Document automatic proposal admission in three implementation slices"},
		{name: "slice packet", request: `{"packet_id":"p1","summary":"add the admission identity test"}`, want: "Add the admission identity test"},
	} {
		t.Run(test.name, func(t *testing.T) {
			got, err := pullRequestTitle(test.request)
			if err != nil || got != test.want {
				t.Fatalf("pullRequestTitle() = %q, %v; want %q", got, err, test.want)
			}
		})
	}
}

func TestPullRequestTitleRejectsBookkeepingAndBoundsProse(t *testing.T) {
	if _, err := pullRequestTitle("wi_42ca22355e8a97e3ab6bbe9f8de7702c"); err == nil {
		t.Fatal("work-item identifier was accepted as a PR title")
	}
	got, err := pullRequestTitle(strings.Repeat("meaningful words ", 30))
	if err != nil {
		t.Fatal(err)
	}
	if len([]rune(got)) > maxPullRequestTitleRunes || !strings.HasSuffix(got, "…") {
		t.Fatalf("long title was not bounded cleanly: %q", got)
	}
}

func TestPullRequestTitlePrefersConcreteGoalOverProposalLabel(t *testing.T) {
	got, err := pullRequestTitle(deduplicationProposal)
	want := "Document the production behavior of the autonomous pending-proposal watcher"
	if err != nil || got != want {
		t.Fatalf("pullRequestTitle() = %q, %v; want %q", got, err, want)
	}
}

func TestPullRequestHandoffRedactsCredentialMaterial(t *testing.T) {
	secret := "sk-live-0123456789abcdefghijklmnop"
	input := strings.Join([]string{
		"Keep this reviewer-facing context.",
		"OPENAI_API_KEY=" + secret,
		"Authorization: Bearer " + secret,
		"-----BEGIN PRIVATE KEY-----",
		"base64-private-key-material",
		"-----END PRIVATE KEY-----",
		"Keep this conclusion too.",
	}, "\n")
	redacted := redactPullRequestMarkdown(input)
	for _, leaked := range []string{secret, "base64-private-key-material", "BEGIN PRIVATE KEY"} {
		if strings.Contains(redacted, leaked) {
			t.Fatalf("generated PR markdown leaked %q:\n%s", leaked, redacted)
		}
	}
	for _, kept := range []string{"Keep this reviewer-facing context.", "Keep this conclusion too.",
		"[REDACTED CREDENTIAL", "[REDACTED PRIVATE KEY"} {
		if !strings.Contains(redacted, kept) {
			t.Fatalf("generated PR markdown missing %q:\n%s", kept, redacted)
		}
	}

	diff := "diff --git a/config b/config\n+++ b/config\n@@ -1 +1 @@\n-API_TOKEN=old-secret-token-value\n+API_TOKEN=" + secret + "\n"
	if highlights := parseDiffHighlights(diff); len(highlights) != 0 {
		t.Fatalf("credential diff became a representative edit: %#v", highlights)
	}
}

func TestPullRequestProposalDetailsFallsBackToProblemAndAcceptance(t *testing.T) {
	request := `# Proposal: run checks for slices

## Problem

Slice pull requests currently merge without any CI check runs.

## Acceptance

- Pull requests targeting the feature namespace run the complete gate.`
	details := pullRequestProposalDetails(request, "Run checks for slices")
	if details.Goal != "Slice pull requests currently merge without any CI check runs." {
		t.Fatalf("goal = %q", details.Goal)
	}
	if !strings.Contains(details.Scope, "targeting the feature namespace") {
		t.Fatalf("scope = %q", details.Scope)
	}
}

func TestDiffHighlightsLeadWithSubstantiveMultilineEdit(t *testing.T) {
	diff := strings.Join([]string{
		"diff --git a/docs/proposals/done/example.md b/docs/proposals/done/example.md",
		"+++ b/docs/proposals/done/example.md",
		"@@ -1 +1 @@",
		"-[related](example.md)",
		"+[related](../done/example.md)",
		"diff --git a/.github/workflows/ci.yml b/.github/workflows/ci.yml",
		"+++ b/.github/workflows/ci.yml",
		"@@ -8 +8,3 @@",
		"-    branches: [main, testing]",
		"+    branches: [main, testing, 'aimee/feat/**']",
		"+    # Slice pull requests use the complete gate.",
		"+    # No separate job definitions are needed.",
		"",
	}, "\n")
	highlights := parseDiffHighlights(diff)
	if len(highlights) != 2 {
		t.Fatalf("highlights = %#v", highlights)
	}
	if !strings.Contains(highlights[0], ".github/workflows/ci.yml") ||
		!strings.Contains(highlights[0], "aimee/feat/**") {
		t.Fatalf("substantive workflow edit did not lead: %#v", highlights)
	}
	if !strings.Contains(highlights[1], "docs/proposals/done/example.md") {
		t.Fatalf("proposal housekeeping highlight missing: %#v", highlights)
	}
}

func TestFinalPullRequestHandoffExplainsProposalAndActualDiff(t *testing.T) {
	root := t.TempDir()
	repo := filepath.Join(root, "repo")
	runGit := func(args ...string) {
		t.Helper()
		command := exec.Command("git", args...)
		command.Dir = repo
		command.Env = append(os.Environ(), "GIT_AUTHOR_NAME=test", "GIT_AUTHOR_EMAIL=t@example.test",
			"GIT_COMMITTER_NAME=test", "GIT_COMMITTER_EMAIL=t@example.test")
		if output, err := command.CombinedOutput(); err != nil {
			t.Fatalf("git %v: %v: %s", args, err, output)
		}
	}
	if err := os.MkdirAll(filepath.Join(repo, "docs", "proposals", "pending"), 0o755); err != nil {
		t.Fatal(err)
	}
	runGit("init")
	runGit("checkout", "-b", "trunk")
	pending := filepath.Join(repo, "docs", "proposals", "pending", "automatic-wfe-trigger-blob-dedup-runbook.md")
	if err := os.WriteFile(pending, []byte(deduplicationProposal), 0o600); err != nil {
		t.Fatal(err)
	}
	runbook := filepath.Join(repo, "docs", "wfe-autonomy-runbook.md")
	if err := os.WriteFile(runbook, []byte("The cap is edited in **Edit Workflows → Run policy**.\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	runGit("add", ".")
	runGit("commit", "-m", "initial documentation")
	runGit("checkout", "-b", "aimee/feat/wi_dedup")
	doneDir := filepath.Join(repo, "docs", "proposals", "done")
	if err := os.MkdirAll(doneDir, 0o755); err != nil {
		t.Fatal(err)
	}
	done := filepath.Join(doneDir, filepath.Base(pending))
	if err := os.Rename(pending, done); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(runbook, []byte("The cap is edited in **Workflows → Run policy**.\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	runGit("add", ".")
	runGit("commit", "-m", "document proposal watcher behavior")

	store, err := workflowstoretest.Open(t, filepath.Join(root, "workflow.sqlite"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	itemInput := workflowstore.CreateWorkItem{
		ID: "wi_dedup", Repo: repo, ProposalPath: "proposal:dedup", WorkflowName: "build-triggered",
		WorkflowVersion: "v1", StartStage: "pr", SourcePath: "docs/proposals/pending/automatic-wfe-trigger-blob-dedup-runbook.md",
	}
	if err := store.CreateWorkItem(context.Background(), itemInput); err != nil {
		t.Fatal(err)
	}
	artifacts, err := wfe.NewArtifactStore(filepath.Join(root, "artifacts"))
	if err != nil {
		t.Fatal(err)
	}
	if err := artifacts.PutPlan(itemInput.ID, []byte("1. Correct the run-policy navigation label.\n2. Verify documentation.")); err != nil {
		t.Fatal(err)
	}
	item, err := store.WorkItem(context.Background(), itemInput.ID)
	if err != nil {
		t.Fatal(err)
	}
	runner := &NativeRunner{db: store, artifacts: artifacts}
	spec, err := runner.pullRequestSpec(t.Context(), StepRequest{
		WorkItem: item, Node: wfe.Node{ID: "pr", Block: "pr.open"}, Proposal: deduplicationProposal,
	}, item, repo, "aimee/feat/wi_dedup", "trunk")
	if err != nil {
		t.Fatal(err)
	}
	if !spec.Draft {
		t.Fatal("final handoff was not a draft")
	}
	if want := "Document the production behavior of the autonomous pending-proposal watcher"; spec.Title != want {
		t.Fatalf("title = %q, want %q", spec.Title, want)
	}
	for _, marker := range []string{
		"## What this proposal does",
		"operators can predict when a file is admitted, queued, or deliberately ignored",
		"pending proposal identity is based on the complete file bytes",
		"## What changed",
		"- Updated `docs/wfe-autonomy-runbook.md`.",
		"Archived `docs/proposals/pending/automatic-wfe-trigger-blob-dedup-runbook.md` as `docs/proposals/done/automatic-wfe-trigger-blob-dedup-runbook.md` without changing its contents",
		"changed `The cap is edited in **Edit Workflows → Run policy**.` to `The cap is edited in **Workflows → Run policy**.`",
		"## Verification",
		"## Human review boundary",
		"must not mark it ready, approve it, or merge it",
	} {
		if !strings.Contains(spec.Body, marker) {
			t.Fatalf("final PR body missing %q:\n%s", marker, spec.Body)
		}
	}
	if strings.Contains(spec.Body, "## Summary") || strings.Contains(spec.Body, "terminal handoff") {
		t.Fatalf("generic workflow boilerplate still leads the PR body:\n%s", spec.Body)
	}
	boundary := strings.Index(spec.Body, "## Human review boundary")
	trace := strings.Index(spec.Body, "<summary>Workflow trace</summary>")
	if boundary < 0 || trace < boundary {
		t.Fatalf("workflow bookkeeping appears before the human handoff contract:\n%s", spec.Body)
	}
}

func TestRefreshPullRequestBaseUsesCurrentRemoteTipBeforeHandoff(t *testing.T) {
	root := t.TempDir()
	bare := filepath.Join(root, "origin.git")
	repo := filepath.Join(root, "repo")
	updater := filepath.Join(root, "updater")
	run := func(dir string, args ...string) string {
		t.Helper()
		command := exec.Command("git", args...)
		command.Dir = dir
		command.Env = append(os.Environ(), "GIT_AUTHOR_NAME=test", "GIT_AUTHOR_EMAIL=t@example.test",
			"GIT_COMMITTER_NAME=test", "GIT_COMMITTER_EMAIL=t@example.test")
		output, err := command.CombinedOutput()
		if err != nil {
			t.Fatalf("git %v: %v: %s", args, err, output)
		}
		return strings.TrimSpace(string(output))
	}
	run(root, "init", "--bare", bare)
	run(root, "clone", bare, repo)
	run(repo, "checkout", "-b", "testing")
	if err := os.WriteFile(filepath.Join(repo, "base.txt"), []byte("initial\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	run(repo, "add", "base.txt")
	run(repo, "commit", "-m", "initial base")
	run(repo, "push", "-u", "origin", "testing")
	run(repo, "checkout", "-b", "aimee/feat/wi_refresh")
	if err := os.WriteFile(filepath.Join(repo, "proposal.txt"), []byte("proposal\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	run(repo, "add", "proposal.txt")
	run(repo, "commit", "-m", "proposal change")

	run(root, "clone", bare, updater)
	run(updater, "checkout", "testing")
	if err := os.WriteFile(filepath.Join(updater, "base.txt"), []byte("current remote\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	run(updater, "add", "base.txt")
	run(updater, "commit", "-m", "advance integration branch")
	run(updater, "push", "origin", "testing")

	conflict, detail, err := refreshPullRequestBase(t.Context(), repo, "testing")
	if err != nil || conflict {
		t.Fatalf("refreshPullRequestBase() = conflict %v, detail %q, err %v", conflict, detail, err)
	}
	run(repo, "merge-base", "--is-ancestor", "refs/remotes/origin/testing", "HEAD")
	if changed := run(repo, "diff", "--name-only", "refs/remotes/origin/testing...HEAD"); changed != "proposal.txt" {
		t.Fatalf("handoff diff includes stale-base noise: %q", changed)
	}
}
