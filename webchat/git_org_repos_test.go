package main

import (
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// writeServerToken drops the shared bearer token the proxy sends to aimee-server.
func writeServerToken(t *testing.T, cfg *config) {
	t.Helper()
	if err := os.WriteFile(filepath.Join(filepath.Dir(cfg.socketPath), "server.token"),
		[]byte("tok\n"), 0600); err != nil {
		t.Fatalf("write server.token: %v", err)
	}
}

// GET /api/git/org-repos forwards host+owner to /v1/workspace/org-repos with the
// webuser identity, and relays the {provider, repos} enumeration.
func TestGitOrgReposProxy(t *testing.T) {
	var gotWebuser, gotHost, gotOwner string
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/workspace/org-repos", func(w http.ResponseWriter, r *http.Request) {
		gotWebuser = r.Header.Get("X-Aimee-Webuser")
		gotHost = r.URL.Query().Get("host")
		gotOwner = r.URL.Query().Get("owner")
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`{"provider":"github","repos":[` +
			`{"name":"repo-a","clone_url":"https://github.com/RakuenSoftware/repo-a.git","ssh_url":"git@github.com:RakuenSoftware/repo-a.git","private":false}]}`))
	})
	cfg := startFakeV1(t, mux)
	writeServerToken(t, cfg)
	s := &server{cfg: cfg}

	req := withUser(httptest.NewRequest(http.MethodGet,
		"/api/git/org-repos?host=github.com&owner=RakuenSoftware", nil), "alice")
	rr := httptest.NewRecorder()
	s.handleGitOrgRepos(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("org-repos: code=%d body=%q", rr.Code, rr.Body.String())
	}
	if gotWebuser != "alice" || gotHost != "github.com" || gotOwner != "RakuenSoftware" {
		t.Fatalf("forwarded webuser=%q host=%q owner=%q", gotWebuser, gotHost, gotOwner)
	}
	body := rr.Body.String()
	if !strings.Contains(body, `"provider":"github"`) || !strings.Contains(body, `"repo-a"`) {
		t.Fatalf("relayed body missing provider/repos: %q", body)
	}
}

// Missing host or owner is rejected at the proxy without calling aimee-server.
func TestGitOrgReposRejectsMissingParams(t *testing.T) {
	called := false
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/workspace/org-repos", func(w http.ResponseWriter, r *http.Request) {
		called = true
		w.Write([]byte(`{}`))
	})
	cfg := startFakeV1(t, mux)
	writeServerToken(t, cfg)
	s := &server{cfg: cfg}

	for _, q := range []string{"", "?host=github.com", "?owner=RakuenSoftware"} {
		req := withUser(httptest.NewRequest(http.MethodGet, "/api/git/org-repos"+q, nil), "alice")
		rr := httptest.NewRecorder()
		s.handleGitOrgRepos(rr, req)
		if rr.Code != http.StatusBadRequest {
			t.Fatalf("query %q: code=%d, want 400", q, rr.Code)
		}
	}
	if called {
		t.Fatalf("aimee-server was called despite missing params")
	}
}

// POST /api/git/clone-org forwards {host, owner, repos} and relays {results}.
func TestGitCloneOrgProxy(t *testing.T) {
	var gotWebuser, gotBody string
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/workspace/clone-org", func(w http.ResponseWriter, r *http.Request) {
		gotWebuser = r.Header.Get("X-Aimee-Webuser")
		b, _ := io.ReadAll(r.Body)
		gotBody = string(b)
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`{"results":[` +
			`{"name":"repo-a","ok":true,"project":"repo-a","error":null},` +
			`{"name":"repo-b","ok":false,"project":null,"error":"already exists"}]}`))
	})
	cfg := startFakeV1(t, mux)
	writeServerToken(t, cfg)
	s := &server{cfg: cfg}

	reqBody := `{"host":"github.com","owner":"RakuenSoftware","repos":[` +
		`{"name":"repo-a","clone_url":"https://github.com/RakuenSoftware/repo-a.git"},` +
		`{"name":"repo-b","clone_url":"https://github.com/RakuenSoftware/repo-b.git"}]}`
	req := withUser(httptest.NewRequest(http.MethodPost, "/api/git/clone-org",
		strings.NewReader(reqBody)), "alice")
	rr := httptest.NewRecorder()
	s.handleGitCloneOrg(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("clone-org: code=%d body=%q", rr.Code, rr.Body.String())
	}
	if gotWebuser != "alice" {
		t.Fatalf("forwarded webuser=%q", gotWebuser)
	}
	if !strings.Contains(gotBody, `"clone_url":"https://github.com/RakuenSoftware/repo-a.git"`) {
		t.Fatalf("forwarded body missing repo clone_url: %q", gotBody)
	}
	body := rr.Body.String()
	if !strings.Contains(body, `"results"`) || !strings.Contains(body, `already exists`) {
		t.Fatalf("relayed body missing results: %q", body)
	}
}

// clone-org rejects an empty repos list and an over-limit list at the proxy.
func TestGitCloneOrgRejectsBadInput(t *testing.T) {
	called := false
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/workspace/clone-org", func(w http.ResponseWriter, r *http.Request) {
		called = true
		w.Write([]byte(`{"results":[]}`))
	})
	cfg := startFakeV1(t, mux)
	writeServerToken(t, cfg)
	s := &server{cfg: cfg}

	// Empty repos.
	req := withUser(httptest.NewRequest(http.MethodPost, "/api/git/clone-org",
		strings.NewReader(`{"host":"github.com","owner":"o","repos":[]}`)), "alice")
	rr := httptest.NewRecorder()
	s.handleGitCloneOrg(rr, req)
	if rr.Code != http.StatusBadRequest {
		t.Fatalf("empty repos: code=%d, want 400", rr.Code)
	}

	// Over the 100-repo cap.
	var b strings.Builder
	b.WriteString(`{"host":"github.com","owner":"o","repos":[`)
	for i := 0; i < 101; i++ {
		if i > 0 {
			b.WriteByte(',')
		}
		b.WriteString(`{"name":"r","clone_url":"https://github.com/o/r.git"}`)
	}
	b.WriteString(`]}`)
	req = withUser(httptest.NewRequest(http.MethodPost, "/api/git/clone-org",
		strings.NewReader(b.String())), "alice")
	rr = httptest.NewRecorder()
	s.handleGitCloneOrg(rr, req)
	if rr.Code != http.StatusBadRequest {
		t.Fatalf("over-limit repos: code=%d, want 400", rr.Code)
	}

	if called {
		t.Fatalf("aimee-server was called for a rejected clone-org request")
	}
}
