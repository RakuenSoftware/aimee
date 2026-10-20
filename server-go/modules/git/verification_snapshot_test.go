package git

import (
	"context"
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

func snapshotFixture(t *testing.T) verificationSnapshotRequest {
	t.Helper()
	root := newRepo(t)
	if err := os.WriteFile(filepath.Join(root, "source.txt"), []byte("committed\n"), 0600); err != nil {
		t.Fatal(err)
	}
	for _, args := range [][]string{{"add", "source.txt"}, {"commit", "-qm", "source"}} {
		if out, err := exec.Command("git", append([]string{"-C", root}, args...)...).CombinedOutput(); err != nil {
			t.Fatal(err, string(out))
		}
	}
	commit, ok := gitOutput(root, "rev-parse", "HEAD")
	if !ok {
		t.Fatal("source commit unavailable")
	}
	tree, ok := gitOutput(root, "rev-parse", "HEAD^{tree}")
	if !ok {
		t.Fatal("source tree unavailable")
	}
	info, err := os.Stat(root)
	if err != nil {
		t.Fatal(err)
	}
	stat := info.Sys().(*syscall.Stat_t)
	return verificationSnapshotRequest{Op: "snapshot-create", ProjectRoot: root,
		Device: strconv.FormatUint(uint64(stat.Dev), 10), Inode: strconv.FormatUint(uint64(stat.Ino), 10),
		Commit: commit, Tree: tree}
}

func snapshotCall(t *testing.T, request verificationSnapshotRequest) verificationSnapshotResponse {
	t.Helper()
	raw, _ := json.Marshal(request)
	body, status := Handle(bus.ModuleInvocation{StageID: StageVerifyRun}, raw)
	if status != bus.ModuleStatusOK {
		t.Fatal("snapshot stage refused", status)
	}
	var response verificationSnapshotResponse
	if err := json.Unmarshal(body, &response); err != nil {
		t.Fatal(err)
	}
	return response
}

func TestVerificationSnapshotUsesCommittedObjectsAndOwnedLease(t *testing.T) {
	request := snapshotFixture(t)
	// Dirty and untracked caller files are not an immutable source snapshot.
	os.WriteFile(filepath.Join(request.ProjectRoot, "source.txt"), []byte("caller dirty\n"), 0600)
	os.WriteFile(filepath.Join(request.ProjectRoot, "untracked.txt"), []byte("not source"), 0600)
	response := snapshotCall(t, request)
	if !response.OK || !response.Clean || response.Tree != request.Tree || !snapshotHex(response.Lease, 32) {
		t.Fatal("no private committed snapshot", response)
	}
	t.Cleanup(func() { snapshotCall(t, verificationSnapshotRequest{Op: "snapshot-release", Lease: response.Lease}) })
	content, err := os.ReadFile(filepath.Join(response.Path, "source.txt"))
	if err != nil || string(content) != "committed\n" {
		t.Fatal("copied mutable caller content", string(content), err)
	}
	if _, err = os.Stat(filepath.Join(response.Path, "untracked.txt")); !os.IsNotExist(err) {
		t.Fatal("imported untracked files", err)
	}
	if info, err := os.Stat(filepath.Join(response.Path, ".git")); err != nil || !info.IsDir() {
		t.Fatal("snapshot has no independent Git metadata", err)
	}
	if _, err = os.Stat(filepath.Join(response.Path, ".git", "objects", "info", "alternates")); !os.IsNotExist(err) {
		t.Fatal("snapshot still depends on caller objects", err)
	}
	os.WriteFile(filepath.Join(request.ProjectRoot, "source.txt"), []byte("caller changed again\n"), 0600)
	state := snapshotCall(t, verificationSnapshotRequest{Op: "snapshot-state", Lease: response.Lease})
	if !state.OK || !state.Clean || state.Tree != request.Tree {
		t.Fatal("caller change affected the private snapshot", state)
	}
	os.WriteFile(filepath.Join(response.Path, "source.txt"), []byte("verifier changed tracked source\n"), 0600)
	state = snapshotCall(t, verificationSnapshotRequest{Op: "snapshot-state", Lease: response.Lease})
	if !state.OK || state.Clean {
		t.Fatal("verifier mutation was read as an unchanged snapshot", state)
	}
	if !snapshotCall(t, verificationSnapshotRequest{Op: "snapshot-release", Lease: response.Lease}).OK {
		t.Fatal("owned cleanup failed")
	}
	if _, err = os.Stat(response.Path); !os.IsNotExist(err) {
		t.Fatal("released snapshot still exists", err)
	}
	if snapshotCall(t, verificationSnapshotRequest{Op: "snapshot-state", Lease: response.Lease}).OK {
		t.Fatal("released lease still grants access")
	}
}

func TestVerificationSnapshotRefusesForeignSourceAndPathSubstitution(t *testing.T) {
	request := snapshotFixture(t)
	foreign := request
	foreign.Inode = "0"
	if snapshotCall(t, foreign).OK {
		t.Fatal("unheld source identity accepted")
	}
	foreign = request
	foreign.Tree = strings.Repeat("0", len(request.Tree))
	if snapshotCall(t, foreign).OK {
		t.Fatal("commit/tree mismatch accepted")
	}
	response := snapshotCall(t, request)
	if !response.OK {
		t.Fatal("fixture snapshot unavailable")
	}
	moved := response.Path + "-moved"
	t.Cleanup(func() { os.RemoveAll(response.Path); os.RemoveAll(moved) })
	if err := os.Rename(response.Path, moved); err != nil {
		t.Fatal(err)
	}
	if err := os.Mkdir(response.Path, 0700); err != nil {
		t.Fatal(err)
	}
	sentinel := filepath.Join(response.Path, "foreign.txt")
	os.WriteFile(sentinel, []byte("preserve replacement"), 0600)
	if snapshotCall(t, verificationSnapshotRequest{Op: "snapshot-state", Lease: response.Lease}).OK {
		t.Fatal("replacement inherited the held resource")
	}
	if snapshotCall(t, verificationSnapshotRequest{Op: "snapshot-release", Lease: response.Lease}).OK {
		t.Fatal("replacement reported as owned cleanup")
	}
	if content, err := os.ReadFile(sentinel); err != nil || string(content) != "preserve replacement" {
		t.Fatal("cleanup deleted a replacement", err)
	}
}

func TestVerificationSnapshotRequiresHostWireAndJoinsCancelledGit(t *testing.T) {
	request := snapshotFixture(t)
	raw, _ := json.Marshal(request)
	for _, invocation := range []bus.ModuleInvocation{
		{StageID: StageVerifyRun, PrincipalRef: 1}, {StageID: StageVerifyRun, DeadlineNS: 1},
	} {
		if _, status := Handle(invocation, raw); status == bus.ModuleStatusOK {
			t.Fatal("foreign or expired snapshot request accepted")
		}
	}
	tools := t.TempDir()
	marker := filepath.Join(tools, "late-effect")
	script := "#!/bin/sh\n(/bin/sleep 1; printf escaped > '" + marker + "') &\nwait\n"
	if err := os.WriteFile(filepath.Join(tools, "git"), []byte(script), 0700); err != nil {
		t.Fatal(err)
	}
	t.Setenv("PATH", tools+string(os.PathListSeparator)+os.Getenv("PATH"))
	ctx, cancel := context.WithTimeout(context.Background(), 50*time.Millisecond)
	defer cancel()
	start := time.Now()
	if _, ok := snapshotGit(ctx, tools, true, "ignored"); ok || time.Since(start) > 2*time.Second {
		t.Fatal("cancelled Git did not join within its budget")
	}
	time.Sleep(1100 * time.Millisecond)
	if _, err := os.Stat(marker); !os.IsNotExist(err) {
		t.Fatal("a cancelled Git descendant performed a late effect", err)
	}
}

func TestVerificationSnapshotLostReplyExpiresWithoutDeletingRenewedCustody(t *testing.T) {
	response := snapshotCall(t, snapshotFixture(t))
	if !response.OK {
		t.Fatal("snapshot unavailable")
	}
	t.Cleanup(func() { snapshotCall(t, verificationSnapshotRequest{Op: "snapshot-release", Lease: response.Lease}) })
	verificationSnapshots.Lock()
	resource := verificationSnapshots.leases[response.Lease]
	verificationSnapshots.Unlock()
	// A timer already queued before a successful renewal cannot retire the lease.
	expireVerificationSnapshot(response.Lease, resource)
	if !snapshotCall(t, verificationSnapshotRequest{Op: "snapshot-state", Lease: response.Lease}).OK {
		t.Fatal("stale timer revoked current custody")
	}
	verificationSnapshots.Lock()
	resource.expires = time.Now().Add(-time.Second)
	verificationSnapshots.Unlock()
	expireVerificationSnapshot(response.Lease, resource)
	if snapshotCall(t, verificationSnapshotRequest{Op: "snapshot-state", Lease: response.Lease}).OK {
		t.Fatal("expired custody still grants access")
	}
	if _, err := os.Stat(response.Path); !os.IsNotExist(err) {
		t.Fatal("expired private snapshot remained", err)
	}
}

func TestVerificationSnapshotRenewKeepsCustodyWithoutMeasuringActiveTests(t *testing.T) {
	response := snapshotCall(t, snapshotFixture(t))
	if !response.OK {
		t.Fatal("fixture snapshot unavailable")
	}
	t.Cleanup(func() { snapshotCall(t, verificationSnapshotRequest{Op: "snapshot-release", Lease: response.Lease}) })
	if err := os.WriteFile(filepath.Join(response.Path, "source.txt"), []byte("active test mutation"), 0600); err != nil {
		t.Fatal(err)
	}
	verificationSnapshots.Lock()
	resource := verificationSnapshots.leases[response.Lease]
	resource.expires = time.Now().Add(20 * time.Millisecond)
	resource.timer.Reset(20 * time.Millisecond)
	verificationSnapshots.Unlock()
	tools := t.TempDir()
	marker := filepath.Join(tools, "git-invoked")
	if err := os.WriteFile(filepath.Join(tools, "git"), []byte("#!/bin/sh\nprintf forbidden > '"+marker+"'\nexit 1\n"), 0700); err != nil {
		t.Fatal(err)
	}
	t.Setenv("PATH", tools+string(os.PathListSeparator)+os.Getenv("PATH"))
	renewed := snapshotCall(t, verificationSnapshotRequest{Op: "snapshot-renew", Lease: response.Lease})
	if !renewed.OK || renewed.Clean || renewed.Commit != "" || renewed.Tree != "" {
		t.Fatal("heartbeat claimed a source measurement", renewed)
	}
	time.Sleep(40 * time.Millisecond)
	if _, err := os.Stat(response.Path); err != nil {
		t.Fatal("old TTL deleted renewed active snapshot", err)
	}
	if _, err := os.Stat(marker); !os.IsNotExist(err) {
		t.Fatal("renewal ran Git during active tests", err)
	}
}

func TestVerificationSnapshotRenewRefusesExpiredAndSubstitutedResources(t *testing.T) {
	response := snapshotCall(t, snapshotFixture(t))
	if !response.OK {
		t.Fatal("fixture snapshot unavailable")
	}
	moved := response.Path + "-moved"
	t.Cleanup(func() {
		snapshotCall(t, verificationSnapshotRequest{Op: "snapshot-release", Lease: response.Lease})
		os.RemoveAll(response.Path)
		os.RemoveAll(moved)
	})
	if err := os.Rename(response.Path, moved); err != nil {
		t.Fatal(err)
	}
	if err := os.Mkdir(response.Path, 0700); err != nil {
		t.Fatal(err)
	}
	if snapshotCall(t, verificationSnapshotRequest{Op: "snapshot-renew", Lease: response.Lease}).OK {
		t.Fatal("replacement inherited renewal authority")
	}
	expired := snapshotCall(t, snapshotFixture(t))
	if !expired.OK {
		t.Fatal("fixture snapshot unavailable")
	}
	verificationSnapshots.Lock()
	verificationSnapshots.leases[expired.Lease].expires = time.Now().Add(-time.Second)
	verificationSnapshots.Unlock()
	if snapshotCall(t, verificationSnapshotRequest{Op: "snapshot-renew", Lease: expired.Lease}).OK {
		t.Fatal("expired lease was resurrected")
	}
	if _, err := os.Stat(expired.Path); !os.IsNotExist(err) {
		t.Fatal("expired resource was not cleaned up", err)
	}
}

func TestVerificationSourceStateBindsOriginalIdentityAndDisablesRepositoryMonitors(t *testing.T) {
	request := snapshotFixture(t)
	request.Op = "snapshot-source-state"
	monitor := filepath.Join(t.TempDir(), "monitor")
	marker := monitor + "-called"
	if err := os.WriteFile(monitor, []byte("#!/bin/sh\nprintf forbidden > '"+marker+"'\nprintf '\\0'\n"), 0700); err != nil {
		t.Fatal(err)
	}
	if output, err := exec.Command("git", "-C", request.ProjectRoot, "config", "core.fsmonitor", monitor).CombinedOutput(); err != nil {
		t.Fatal(err, string(output))
	}
	state := snapshotCall(t, request)
	if !state.OK || !state.Clean || state.Commit != request.Commit || state.Tree != request.Tree || state.Lease != "" || state.Path != "" {
		t.Fatal("missing original source measurement", state)
	}
	if _, err := os.Stat(marker); !os.IsNotExist(err) {
		t.Fatal("repository-selected monitor executed", err)
	}
	if err := os.WriteFile(filepath.Join(request.ProjectRoot, "source.txt"), []byte("mutated source\n"), 0600); err != nil {
		t.Fatal(err)
	}
	state = snapshotCall(t, request)
	if !state.OK || state.Clean {
		t.Fatal("changed original source measured clean", state)
	}
	foreignWorktree := t.TempDir()
	if err := os.WriteFile(filepath.Join(foreignWorktree, "source.txt"), []byte("committed\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if output, err := exec.Command("git", "-C", request.ProjectRoot, "config", "core.worktree", foreignWorktree).CombinedOutput(); err != nil {
		t.Fatal(err, string(output))
	}
	state = snapshotCall(t, request)
	if !state.OK || state.Clean {
		t.Fatal("repository core.worktree redirected source measurement", state)
	}
	foreign := request
	foreign.Inode = "0"
	if snapshotCall(t, foreign).OK {
		t.Fatal("foreign original identity accepted")
	}
	foreign = request
	foreign.Commit = strings.Repeat("0", len(request.Commit))
	if snapshotCall(t, foreign).OK {
		t.Fatal("different original revision accepted")
	}
	foreign = request
	foreign.Tree = strings.Repeat("0", len(request.Tree))
	if snapshotCall(t, foreign).OK {
		t.Fatal("different original tree accepted")
	}
	alias := request.ProjectRoot + "-alias"
	if err := os.Symlink(request.ProjectRoot, alias); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { os.Remove(alias) })
	foreign = request
	foreign.ProjectRoot = alias
	if snapshotCall(t, foreign).OK {
		t.Fatal("path alias became original source identity")
	}
}

func TestVerificationSnapshotPlacementAndLeaseMarkerPreserveCleanSource(t *testing.T) {
	for _, linked := range []bool{false, true} {
		name := "main"
		if linked {
			name = "linked"
		}
		t.Run(name, func(t *testing.T) {
			original := snapshotFixture(t)
			request := original
			if linked {
				request.ProjectRoot = filepath.Join(t.TempDir(), "linked")
				if output, err := exec.Command("git", "-C", original.ProjectRoot, "worktree", "add", "--detach", request.ProjectRoot, original.Commit).CombinedOutput(); err != nil {
					t.Fatal(err, string(output))
				}
				info, err := os.Stat(request.ProjectRoot)
				if err != nil {
					t.Fatal(err)
				}
				identity := info.Sys().(*syscall.Stat_t)
				request.Device = strconv.FormatUint(uint64(identity.Dev), 10)
				request.Inode = strconv.FormatUint(uint64(identity.Ino), 10)
			}
			metadata, ok := gitOutput(request.ProjectRoot, "rev-parse", "--absolute-git-dir")
			if !ok {
				t.Fatal("source Git metadata unavailable")
			}
			metadata, err := filepath.EvalSymlinks(metadata)
			if err != nil {
				t.Fatal(err)
			}
			response := snapshotCall(t, request)
			if !response.OK {
				t.Fatal("private snapshot unavailable", response)
			}
			t.Cleanup(func() { snapshotCall(t, verificationSnapshotRequest{Op: "snapshot-release", Lease: response.Lease}) })
			if filepath.Dir(response.Path) != metadata {
				t.Fatal("snapshot escaped mapped source metadata", response.Path, metadata)
			}
			info, err := os.Stat(response.Path)
			if err != nil || info.Mode().Perm() != 0700 {
				t.Fatal("private snapshot directory permissions", info, err)
			}
			marker := filepath.Join(response.Path, ".git", "aimee-verification-lease")
			content, err := os.ReadFile(marker)
			if err != nil || string(content) != response.Lease+"\n" {
				t.Fatal("namespace marker does not bind exact lease", string(content), err)
			}
			info, err = os.Lstat(marker)
			if err != nil || !info.Mode().IsRegular() || info.Mode().Perm() != 0400 {
				t.Fatal("lease marker is not a private read-only regular file", info, err)
			}
			if _, err = os.Stat(filepath.Join(response.Path, ".git", "objects", "info", "alternates")); !os.IsNotExist(err) {
				t.Fatal("snapshot shares source object storage", err)
			}
			for _, source := range []verificationSnapshotRequest{original, request} {
				source.Op = "snapshot-source-state"
				state := snapshotCall(t, source)
				if !state.OK || !state.Clean || state.Commit != source.Commit || state.Tree != source.Tree {
					t.Fatal("private snapshot polluted source state", state)
				}
				dirty, ok := gitOutput(source.ProjectRoot, "status", "--porcelain", "--untracked-files=all")
				if !ok || dirty != "" {
					t.Fatal("Git sees private metadata as source changes", dirty)
				}
			}
			state := snapshotCall(t, verificationSnapshotRequest{Op: "snapshot-state", Lease: response.Lease})
			if !state.OK || !state.Clean {
				t.Fatal("namespace marker polluted snapshot source", state)
			}
		})
	}
}
