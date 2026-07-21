package engine

import (
	"context"
	"encoding/json"
	"net"
	"net/http"
	"os/exec"
	"path/filepath"
	"testing"
)

func TestHTTPForgeExecuteUsesUnixResourcePlane(t *testing.T) {
	socket := filepath.Join(t.TempDir(), "forge.sock")
	listener, err := net.Listen("unix", socket)
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	server := &http.Server{Handler: http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/v1/internal/forge/execute" {
			t.Fatalf("unexpected path %q", r.URL.Path)
		}
		var request map[string]any
		if err := json.NewDecoder(r.Body).Decode(&request); err != nil {
			t.Fatal(err)
		}
		if request["op"] != "ci" {
			t.Fatalf("unexpected operation %v", request["op"])
		}
		_, _ = w.Write([]byte(`{"ok":true,"state":"passed"}`))
	})}
	go server.Serve(listener)
	defer server.Close()
	forge, err := NewHTTPForge(HTTPForgeConfig{UnixSocket: socket})
	if err != nil {
		t.Fatal(err)
	}
	var result struct {
		State CIState `json:"state"`
	}
	if err := forge.execute(context.Background(), map[string]any{"op": "ci"}, &result); err != nil {
		t.Fatal(err)
	}
	if result.State != CIPassed {
		t.Fatalf("unexpected state %q", result.State)
	}
}

func TestHTTPForgePushRejectsUnmanagedBranchAndMismatchedOrigin(t *testing.T) {
	forge := &HTTPForge{}
	if err := forge.Push(t.Context(), t.TempDir(), t.TempDir(), "main"); err == nil {
		t.Fatal("unmanaged branch was accepted")
	}
	repo := filepath.Join(t.TempDir(), "repo")
	worktree := filepath.Join(t.TempDir(), "worktree")
	for _, dir := range []string{repo, worktree} {
		if output, err := exec.Command("git", "init", dir).CombinedOutput(); err != nil {
			t.Fatalf("git init %s: %v: %s", dir, err, output)
		}
	}
	if output, err := exec.Command("git", "-C", repo, "remote", "add", "origin", "https://github.com/acme/one.git").CombinedOutput(); err != nil {
		t.Fatalf("add repo origin: %v: %s", err, output)
	}
	if output, err := exec.Command("git", "-C", worktree, "remote", "add", "origin", "https://github.com/acme/two.git").CombinedOutput(); err != nil {
		t.Fatalf("add worktree origin: %v: %s", err, output)
	}
	if err := forge.Push(t.Context(), repo, worktree, "aimee/feat/wi_example"); err == nil {
		t.Fatal("mismatched worktree origin was accepted")
	}
}

func TestPullNumber(t *testing.T) {
	for _, input := range []string{"42", "https://github.com/acme/repo/pull/42"} {
		number, err := pullNumber(input)
		if err != nil || number != 42 {
			t.Fatalf("pullNumber(%q) = %d, %v", input, number, err)
		}
	}
	if _, err := pullNumber("https://github.com/acme/repo/pull/not-a-number"); err == nil {
		t.Fatal("invalid pull reference accepted")
	}
}
