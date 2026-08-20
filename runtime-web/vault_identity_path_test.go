package main

// runuser replaces the environment with the target user's login PATH, which on
// a stock Debian login.defs is /sbin:/bin:/usr/sbin:/usr/bin -- and aimee-server
// installs to /usr/local/bin, including in the server image. Handing runuser the
// bare name therefore made the lookup depend on the aimee user's login PATH
// rather than on where the binary is, and where that PATH omits the install
// directory the webchat exits at startup with "webchat Vault export failed" --
// an error about the Vault, for a binary that was never found.

import (
	"os"
	"path/filepath"
	"runtime"
	"testing"
)

func TestAimeeServerPathResolvesAgainstOurOwnPATH(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("runuser is POSIX-only")
	}
	dir := t.TempDir()
	binary := filepath.Join(dir, "aimee-server")
	if err := os.WriteFile(binary, []byte("#!/bin/sh\nexit 0\n"), 0o755); err != nil {
		t.Fatal(err)
	}
	t.Setenv("PATH", dir)
	if got := aimeeServerPath(); got != binary {
		t.Errorf("aimeeServerPath() = %q, want the resolved %q -- runuser cannot be "+
			"relied on to find it by name", got, binary)
	}
}

// Absent from PATH entirely, the bare name is still the honest answer: runuser
// may yet find it, and inventing a path we never saw would be worse.
func TestAimeeServerPathFallsBackToTheBareName(t *testing.T) {
	t.Setenv("PATH", t.TempDir())
	if got := aimeeServerPath(); got != "aimee-server" {
		t.Errorf("aimeeServerPath() = %q, want \"aimee-server\"", got)
	}
}
