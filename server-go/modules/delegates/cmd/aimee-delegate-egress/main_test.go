package main

import (
	"context"
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/modules/delegates"
)

func verifiedRunner(_ context.Context, _ string, args ...string) (string, error) {
	if len(args) == 0 || args[0] != "inspect" {
		return "", nil
	}
	switch args[2] {
	case `{{json .NetworkSettings.Networks}}`:
		return `{"none":{"IPAddress":""}}`, nil
	case `{{range .Mounts}}{{.Source}}|{{.Destination}}|{{.RW}};{{end}}`:
		return "/srv/repo|/srv/repo|true;/run/aimee/aimee-http.sock|/run/aimee/aimee-http.sock|true;", nil
	case `{{range .Config.Env}}{{println .}}{{end}}`:
		return "AIMEE_API_ENDPOINT=" + delegates.ControlEndpoint + "\nGIT_CONFIG_COUNT=1\nGIT_CONFIG_KEY_0=safe.directory\nGIT_CONFIG_VALUE_0=*\nPATH=/usr/bin:/bin\n", nil
	}
	return "", nil
}

func TestAcquireRequiresUnixSocketAndReturnsVerifiedContainer(t *testing.T) {
	tmp := t.TempDir()
	socket := filepath.Join(tmp, delegates.ControlSocketBasename)
	listener, err := net.Listen("unix", socket)
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	name, err := acquire(context.Background(), []string{"--task", "d1", "--image", "ubuntu:22.04",
		"--workdir", "/srv/repo", "--repo-root", "/srv/repo", "--worktree", "/srv/repo",
		"--socket-check", socket, "--socket-source", "/run/aimee/aimee-http.sock", "--writes-allowed"}, verifiedRunner)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasPrefix(name, "aimee-delegate-d1-") {
		t.Fatalf("unexpected name %q", name)
	}
}

func TestAcquireRejectsPlainFileAndMissingWorkspace(t *testing.T) {
	path := filepath.Join(t.TempDir(), delegates.ControlSocketBasename)
	if err := os.WriteFile(path, []byte("not a socket"), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := acquire(context.Background(), []string{"--task", "d1", "--image", "ubuntu:22.04",
		"--socket-check", path, "--socket-source", "/run/aimee/aimee-http.sock"}, verifiedRunner); err == nil {
		t.Fatal("plain file or empty workspace was accepted")
	}
}

func TestMachineRunnerSeparatesSuccessfulStdoutFromWarnings(t *testing.T) {
	output, err := commandRunner(context.Background(), "/bin/sh", "-c", "printf machine-receipt; printf warning >&2")
	if err != nil || output != "machine-receipt" {
		t.Fatal("warnings corrupted the successful machine receipt", output, err)
	}
	output, err = commandRunner(context.Background(), "/bin/sh", "-c", "printf 'No such container' >&2; exit 1")
	if err == nil || !strings.Contains(output, "No such container") {
		t.Fatal("failed cleanup lost its diagnostic", output, err)
	}
}

func TestVerifierRequiresLaunchingParentIdentity(t *testing.T) {
	for _, expected := range []int{0, -1, os.Getpid()} {
		if _, _, err := verifierParentContext(context.Background(), expected); err == nil {
			t.Fatalf("unowned parent %d accepted", expected)
		}
	}
	owned, join, err := verifierParentContext(context.Background(), os.Getppid())
	if err != nil {
		t.Fatal(err)
	}
	join()
	if owned.Err() != context.Canceled {
		t.Fatal("parent watcher was not joined")
	}
}
