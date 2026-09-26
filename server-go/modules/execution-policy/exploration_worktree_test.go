package executionpolicy

import (
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestExplorationWorktreeObservation(t *testing.T) {
	root := t.TempDir()
	git := func(args ...string) {
		t.Helper()
		command := append([]string{"-C", root}, args...)
		if output, err := exec.Command("git", command...).CombinedOutput(); err != nil {
			t.Fatalf("git: %v: %s", err, output)
		}
	}
	if explorationWorktreeGeneration(root) != "unavailable" {
		t.Fatal("non-repository attested")
	}
	git("init", "-b", "main")
	if explorationWorktreeGeneration(root) != "unavailable" {
		t.Fatal("unborn HEAD attested")
	}
	file := filepath.Join(root, "sample.txt")
	if err := os.WriteFile(file, []byte("first\n"), 0600); err != nil {
		t.Fatal(err)
	}
	git("add", ".")
	git("-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "commit", "-m", "fixture")
	clean := explorationWorktreeGeneration(root)
	if !strings.HasPrefix(clean, "git-clean:") {
		t.Fatal(clean)
	}
	home := t.TempDir()
	t.Setenv("AIMEE_HOME", home)
	if err := os.WriteFile(filepath.Join(home, "policy.json"), []byte(`{"exploration":{"raw_scans":2},"adaptive_exploration":{"enabled":true,"raw_scans":0}}`), 0600); err != nil {
		t.Fatal(err)
	}
	now := time.Now()
	c := testContract(now)
	c.Binding.WorkingDirectory = root
	offer := &sessionExplorationOffer{MemoryOwner: c.Binding.MemoryOwner, IndexGeneration: c.Binding.IndexGeneration,
		PlanDigest: c.PlanDigest, SourceVersionsDigest: c.SourceVersionsDigest, QueryClass: c.QueryClass,
		ConfidenceProvenance: c.ConfidenceProvenance, Expires: c.Expires}
	raw, _ := json.Marshal(sessionExplorationRequest{Operation: "prepare", Binding: c.Binding, Offer: offer})
	state, reply, err := SessionExploration("alice", "session", nil, raw, now)
	if err != nil {
		t.Fatal(err)
	}
	var issued struct {
		Contract explorationContract `json:"contract"`
	}
	json.Unmarshal(reply, &issued)
	if issued.Contract.Binding.WorktreeGeneration != clean {
		t.Fatal("issuance did not use observed generation")
	}
	check := func(id string) map[string]any {
		t.Helper()
		raw, _ := json.Marshal(sessionExplorationRequest{Operation: "check", Binding: issued.Contract.Binding, AttemptID: id,
			Tool: "grep", SideEffect: "filesystem", Arguments: json.RawMessage(`{"path":".","pattern":"sample"}`)})
		next, reply, err := SessionExploration("alice", "session", state, raw, now)
		if err != nil {
			t.Fatal(err)
		}
		state = next
		var result map[string]any
		json.Unmarshal(reply, &result)
		return result
	}
	if check("clean")["exploration"].(map[string]any)["would_restrict"] != true {
		t.Fatal("clean contract not observed")
	}

	if err := os.WriteFile(file, []byte("changed\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if explorationWorktreeGeneration(root) != "unavailable" {
		t.Fatal("dirty tracked file attested")
	}
	dirty := check("dirty")["exploration"].(map[string]any)
	if dirty["reason"] != "contract_invalidated" || dirty["would_restrict"] != false {
		t.Fatal("dirty worktree retained adaptive control", dirty)
	}
	if check("operator-ceiling")["allowed"] != false {
		t.Fatal("invalidation reset operator ceiling")
	}

	git("checkout", "--", "sample.txt")
	untracked := filepath.Join(root, "new.txt")
	if err := os.WriteFile(untracked, []byte("new"), 0600); err != nil {
		t.Fatal(err)
	}
	if explorationWorktreeGeneration(root) != "unavailable" {
		t.Fatal("untracked work attested")
	}
	if err := os.Remove(untracked); err != nil {
		t.Fatal(err)
	}
	if explorationWorktreeGeneration(root) != clean {
		t.Fatal("clean observation unstable")
	}
	git("-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "commit", "--allow-empty", "-m", "next")
	if explorationWorktreeGeneration(root) == clean {
		t.Fatal("new commit did not invalidate observation")
	}
	alias := filepath.Join(t.TempDir(), "alias")
	if err := os.Symlink(root, alias); err != nil {
		t.Fatal(err)
	}
	if explorationWorktreeGeneration(alias) != "unavailable" {
		t.Fatal("aliased root attested")
	}
}
