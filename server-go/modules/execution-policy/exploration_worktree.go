package executionpolicy

import (
	"bytes"
	"context"
	"errors"
	"os/exec"
	"path/filepath"
	"strings"
	"time"
)

// Observe a clean tracked worktree at the host boundary. Dirty, non-Git,
// oversized, timed-out and aliased roots are explicitly unavailable. This is
// not an assertion that a remote index contains this commit.
func explorationWorktreeGeneration(directory string) string {
	if !canonicalDiscoveryPath(directory) {
		return "unavailable"
	}
	real, err := filepath.EvalSymlinks(directory)
	if err != nil || real != directory {
		return "unavailable"
	}
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, "git", "-C", directory, "-c", "core.fsmonitor=false", "-c", "core.untrackedCache=false",
		"status", "--porcelain=v2", "--branch", "--untracked-files=normal", "--ignore-submodules=none")
	output := &explorationGitOutput{}
	cmd.Stdout = output
	cmd.WaitDelay = 100 * time.Millisecond
	if cmd.Run() != nil {
		return "unavailable"
	}
	oid := ""
	for _, line := range strings.Split(output.String(), "\n") {
		if strings.HasPrefix(line, "# branch.oid ") {
			oid = strings.TrimPrefix(line, "# branch.oid ")
			continue
		}
		if line != "" && !strings.HasPrefix(line, "# ") {
			return "unavailable"
		}
	}
	if len(oid) != 40 && len(oid) != 64 {
		return "unavailable"
	}
	for _, c := range oid {
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
			return "unavailable"
		}
	}
	return "git-clean:" + oid
}

type explorationGitOutput struct{ buffer bytes.Buffer }

func (b *explorationGitOutput) Write(p []byte) (int, error) {
	if b.buffer.Len()+len(p) > 65536 {
		return 0, errors.New("worktree observation limit")
	}
	return b.buffer.Write(p)
}

func (b *explorationGitOutput) String() string { return b.buffer.String() }
