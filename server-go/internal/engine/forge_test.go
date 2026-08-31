package engine

import (
	"context"
	"encoding/json"
	"errors"
	"net"
	"net/http"
	"os/exec"
	"path/filepath"
	"strings"
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

func TestHTTPForgeIdentityUsesSealedResourcePlaneResult(t *testing.T) {
	socket := filepath.Join(t.TempDir(), "forge.sock")
	listener, err := net.Listen("unix", socket)
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	server := &http.Server{Handler: http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var request map[string]any
		if err := json.NewDecoder(r.Body).Decode(&request); err != nil {
			t.Fatal(err)
		}
		if request["op"] != "identity" || request["workdir"] != "/managed/worktree" {
			t.Fatalf("unexpected identity request: %#v", request)
		}
		_, _ = w.Write([]byte(`{"ok":true,"configured":true,"name":"Operator","email":"operator@example.test"}`))
	})}
	go server.Serve(listener)
	defer server.Close()
	forge, err := NewHTTPForge(HTTPForgeConfig{UnixSocket: socket})
	if err != nil {
		t.Fatal(err)
	}
	identity, err := forge.Identity(t.Context(), "/managed/worktree")
	if err != nil {
		t.Fatal(err)
	}
	if identity.Name != "Operator" || identity.Email != "operator@example.test" {
		t.Fatalf("identity = %+v", identity)
	}
}

func TestHTTPForgeIdentityReportsMissingPair(t *testing.T) {
	socket := filepath.Join(t.TempDir(), "forge.sock")
	listener, err := net.Listen("unix", socket)
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	server := &http.Server{Handler: http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte(`{"ok":true,"configured":false}`))
	})}
	go server.Serve(listener)
	defer server.Close()
	forge, err := NewHTTPForge(HTTPForgeConfig{UnixSocket: socket})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := forge.Identity(t.Context(), "/managed/worktree"); !errors.Is(err, ErrGitIdentityMissing) {
		t.Fatalf("identity error = %v, want ErrGitIdentityMissing", err)
	}
}

func TestHTTPForgePushRejectsUnmanagedBranchAndMismatchedOrigin(t *testing.T) {
	forge := &HTTPForge{}
	if err := forge.Push(t.Context(), t.TempDir(), t.TempDir(), "main"); err == nil ||
		!strings.Contains(err.Error(), "unmanaged branch") {
		t.Fatalf("unmanaged branch error = %v", err)
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
	if err := forge.Push(t.Context(), repo, worktree, "aimee/feat/wi_example"); err == nil ||
		err.Error() != "worktree origin does not match admitted repository" {
		t.Fatalf("mismatched origin error = %v", err)
	}
}

func TestHTTPForgeOpenCarriesCompleteDraftHandoff(t *testing.T) {
	socket := filepath.Join(t.TempDir(), "forge.sock")
	listener, err := net.Listen("unix", socket)
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	var request map[string]any
	server := &http.Server{Handler: http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if err := json.NewDecoder(r.Body).Decode(&request); err != nil {
			t.Fatal(err)
		}
		_, _ = w.Write([]byte(`{"ok":true,"url":"https://github.com/acme/one/pull/7"}`))
	})}
	go server.Serve(listener)
	defer server.Close()

	root := t.TempDir()
	repo := filepath.Join(root, "repo")
	worktree := filepath.Join(root, "worktree")
	for _, dir := range []string{repo, worktree} {
		if output, err := exec.Command("git", "init", dir).CombinedOutput(); err != nil {
			t.Fatalf("git init %s: %v: %s", dir, err, output)
		}
		if output, err := exec.Command("git", "-C", dir, "remote", "add", "origin", "https://github.com/acme/one.git").CombinedOutput(); err != nil {
			t.Fatalf("add repo origin: %v: %s", err, output)
		}
	}
	forge, err := NewHTTPForge(HTTPForgeConfig{UnixSocket: socket})
	if err != nil {
		t.Fatal(err)
	}
	spec := PullRequestSpec{Title: "Describe the completed feature", Body: "## Summary\n\nReview this.", Draft: true}
	pr, err := forge.Open(t.Context(), repo, worktree, "aimee/feat/wi_example", "testing", spec)
	if err != nil {
		t.Fatal(err)
	}
	if pr.URL != "https://github.com/acme/one/pull/7" {
		t.Fatalf("PR = %+v", pr)
	}
	for key, want := range map[string]any{"op": "open", "title": spec.Title, "body": spec.Body,
		"draft": true, "head": "aimee/feat/wi_example", "base": "testing"} {
		if request[key] != want {
			t.Fatalf("request[%q] = %#v, want %#v; request=%#v", key, request[key], want, request)
		}
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

func TestMergeErrIsConflict(t *testing.T) {
	for _, tc := range []struct {
		name string
		err  error
		want bool
	}{
		{
			// The exact string the resource plane produced in production while
			// slices g0.1/g0.2 retried an unwinnable merge every 15 seconds.
			name: "live conflict payload",
			err: errors.New(`forge resource 400: {"error":"github API (pr merge, HTTP 405): ` +
				`Pull Request has merge conflicts"}`),
			want: true,
		},
		{name: "singular", err: errors.New("Pull Request has a merge conflict"), want: true},
		{name: "mixed case", err: errors.New("Merge Conflict detected"), want: true},
		{name: "upper case", err: errors.New("MERGE CONFLICTS PRESENT"), want: true},
		{
			// A lost race: head or base moved mid-merge. Retrying wins it, so it
			// must stay retryable even though GitHub answers 405 here too.
			name: "lost race",
			err:  errors.New("Base branch was modified. Review and try the merge again."),
			want: false,
		},
		{
			// HTTP 409 is literally named "Conflict"; the bare word must not be
			// enough to reject a winnable race.
			name: "bare conflict word",
			err:  errors.New("forge resource 409: Conflict"),
			want: false,
		},
		{name: "unrelated failure", err: errors.New("forge resource plane is unavailable"), want: false},
		{name: "empty message", err: errors.New(""), want: false},
		{name: "nil error", err: nil, want: false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if got := mergeErrIsConflict(tc.err); got != tc.want {
				t.Fatalf("mergeErrIsConflict(%v) = %v, want %v", tc.err, got, tc.want)
			}
		})
	}
}
