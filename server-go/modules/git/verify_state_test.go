package git

import (
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

// newRepo makes a real repository with one commit. The ledger keys on tree
// hashes git actually produced, so a fixture that fakes them would not prove
// the lookup works.
func newRepo(t *testing.T) string {
	t.Helper()
	root := t.TempDir()
	for _, args := range [][]string{
		{"init", "-q", "-b", "main"},
		{"config", "user.email", "test@example.com"},
		{"config", "user.name", "test"},
		{"commit", "-q", "--allow-empty", "-m", "one"},
	} {
		command := exec.Command("git", append([]string{"-C", root}, args...)...)
		if out, err := command.CombinedOutput(); err != nil {
			t.Skipf("git unavailable: %v: %s", err, out)
		}
	}
	return root
}

func call(t *testing.T, request VerifyStateRequest) (VerifyStateResponse, bus.ModuleStatus) {
	t.Helper()
	encoded, err := json.Marshal(request)
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}
	body, status := Handle(bus.ModuleInvocation{StageID: StageVerifyRun}, encoded)
	var response VerifyStateResponse
	if status == bus.ModuleStatusOK {
		if err := json.Unmarshal(body, &response); err != nil {
			t.Fatalf("unmarshal: %v", err)
		}
	}
	return response, status
}

func TestVerifyStateHashesAndDirtiness(t *testing.T) {
	root := newRepo(t)

	tree, status := call(t, VerifyStateRequest{Op: "tree-hash", ProjectRoot: root})
	if status != bus.ModuleStatusOK || !tree.OK || len(tree.Hash) != 40 {
		t.Fatalf("tree-hash = %#v status %d", tree, status)
	}
	commit, _ := call(t, VerifyStateRequest{Op: "commit-hash", ProjectRoot: root})
	if !commit.OK || commit.Hash == tree.Hash {
		t.Fatalf("commit-hash = %#v, must differ from the tree hash", commit)
	}

	clean, _ := call(t, VerifyStateRequest{Op: "worktree-dirty", ProjectRoot: root})
	if !clean.OK || clean.Dirty {
		t.Fatalf("fresh checkout reported dirty: %#v", clean)
	}
	if err := os.WriteFile(filepath.Join(root, "new.txt"), []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	dirty, _ := call(t, VerifyStateRequest{Op: "worktree-dirty", ProjectRoot: root})
	if !dirty.OK || !dirty.Dirty {
		t.Fatalf("untracked file not reported dirty: %#v", dirty)
	}

	// A path that is not a repository must answer "unknown", not "clean" --
	// the gate treats a clean tree as safe to push.
	missing, _ := call(t, VerifyStateRequest{Op: "tree-hash", ProjectRoot: filepath.Join(root, "absent")})
	if missing.OK {
		t.Fatalf("non-repository reported a hash: %#v", missing)
	}
}

func TestVerifyStateRoundTripAndWindow(t *testing.T) {
	root := newRepo(t)

	empty, _ := call(t, VerifyStateRequest{Op: "state-read", ProjectRoot: root})
	if !empty.OK || len(empty.Entries) != 0 {
		t.Fatalf("absent ledger = %#v, want ok with no entries", empty)
	}

	written, _ := call(t, VerifyStateRequest{
		Op: "state-write", ProjectRoot: root, Timestamp: 1000, Hash: strings.Repeat("a", 40),
		Failed: 1, Total: 3, StepResults: "lint:0,build:1",
	})
	if !written.OK {
		t.Fatal("state-write failed")
	}
	read, _ := call(t, VerifyStateRequest{Op: "state-read", ProjectRoot: root})
	if len(read.Entries) != 1 {
		t.Fatalf("entries = %#v", read.Entries)
	}
	got := read.Entries[0]
	if got.Timestamp != 1000 || got.Failed != 1 || got.Total != 3 || got.StepResults != "lint:0,build:1" {
		t.Fatalf("round trip lost data: %#v", got)
	}

	// Re-verifying the same tree replaces its entry rather than accumulating,
	// otherwise a repeatedly verified branch evicts every other one.
	call(t, VerifyStateRequest{Op: "state-write", ProjectRoot: root, Timestamp: 2000, Hash: strings.Repeat("a", 40)})
	again, _ := call(t, VerifyStateRequest{Op: "state-read", ProjectRoot: root})
	if len(again.Entries) != 1 || again.Entries[0].Timestamp != 2000 {
		t.Fatalf("same tree not replaced: %#v", again.Entries)
	}

	for index := 0; index < StateMax+4; index++ {
		call(t, VerifyStateRequest{
			Op: "state-write", ProjectRoot: root, Timestamp: int64(index),
			Hash: strings.Repeat(string(rune('0'+index%10)), 39) + string(rune('a'+index%20)),
		})
	}
	windowed, _ := call(t, VerifyStateRequest{Op: "state-read", ProjectRoot: root})
	if len(windowed.Entries) != StateMax {
		t.Fatalf("window = %d entries, want %d", len(windowed.Entries), StateMax)
	}
}

func TestVerifyStateReadsLegacyLayout(t *testing.T) {
	root := newRepo(t)
	path := statePath(root)
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	hash := strings.Repeat("b", 40)
	if err := os.WriteFile(path, []byte("1700000000\n"+hash+"\nfailed=2/total=5\n"), 0o644); err != nil {
		t.Fatal(err)
	}

	read, _ := call(t, VerifyStateRequest{Op: "state-read", ProjectRoot: root})
	if len(read.Entries) != 1 || read.Entries[0].Hash != hash ||
		read.Entries[0].Timestamp != 1700000000 || read.Entries[0].Failed != 2 || read.Entries[0].Total != 5 {
		t.Fatalf("legacy ledger = %#v", read.Entries)
	}

	// The next write upgrades the file in place.
	call(t, VerifyStateRequest{Op: "state-write", ProjectRoot: root, Timestamp: 1, Hash: strings.Repeat("c", 40)})
	contents, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if lines := strings.Split(strings.TrimSpace(string(contents)), "\n"); len(lines) != 2 ||
		!strings.Contains(lines[1], hash) {
		t.Fatalf("upgraded ledger = %q", contents)
	}
}

func TestVerifyStateSharesOneLedgerAcrossWorktrees(t *testing.T) {
	root := newRepo(t)
	linked := filepath.Join(t.TempDir(), "wt")
	command := exec.Command("git", "-C", root, "worktree", "add", "-q", "-b", "side", linked)
	if out, err := command.CombinedOutput(); err != nil {
		t.Skipf("git worktree unavailable: %v: %s", err, out)
	}

	// Recorded in the linked worktree, it must be visible from the main
	// checkout -- that is where the pre-push hook reads the gate.
	hash := strings.Repeat("d", 40)
	if written, _ := call(t, VerifyStateRequest{Op: "state-write", ProjectRoot: linked, Timestamp: 7, Hash: hash}); !written.OK {
		t.Fatal("write from linked worktree failed")
	}
	read, _ := call(t, VerifyStateRequest{Op: "state-read", ProjectRoot: root})
	if len(read.Entries) != 1 || read.Entries[0].Hash != hash {
		t.Fatalf("main checkout did not see the linked worktree's entry: %#v", read.Entries)
	}
}

func TestVerifyStateRefusesBadRequests(t *testing.T) {
	if _, status := Handle(bus.ModuleInvocation{StageID: StageVerifyRun}, []byte("{")); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("malformed JSON accepted")
	}
	if _, status := call(t, VerifyStateRequest{Op: "no-such-op"}); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("unknown op accepted")
	}
	// A write with no tree to key on would produce an entry nothing can match.
	if _, status := call(t, VerifyStateRequest{Op: "state-write", ProjectRoot: t.TempDir()}); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("hashless write accepted")
	}
}
