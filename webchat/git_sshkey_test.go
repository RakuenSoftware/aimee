package main

import (
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The SSH-key panel POSTs {ssh_key} to /api/git/sshkey; the backend must forward
// it to aimee-server's /v1/git/sshkey carrying the webuser trust headers, and
// must never echo the key back to the browser. DELETE clears it (no body).
func TestGitSSHKeyProxy(t *testing.T) {
	var gotMethod, gotWebuser, gotAuth, gotBody string
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/git/sshkey", func(w http.ResponseWriter, r *http.Request) {
		gotMethod = r.Method
		gotWebuser = r.Header.Get("X-Aimee-Webuser")
		gotAuth = r.Header.Get("Authorization")
		b := make([]byte, r.ContentLength)
		r.Body.Read(b)
		gotBody = string(b)
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`{"ok":true}`))
	})
	cfg := startFakeV1(t, mux)
	if err := os.WriteFile(filepath.Join(filepath.Dir(cfg.socketPath), "server.token"),
		[]byte("sekret-token\n"), 0600); err != nil {
		t.Fatalf("write server.token: %v", err)
	}
	s := &server{cfg: cfg}

	const key = "-----BEGIN OPENSSH PRIVATE KEY-----\nABCDEF\n-----END OPENSSH PRIVATE KEY-----"
	req := withUser(httptest.NewRequest(http.MethodPost, "/api/git/sshkey",
		strings.NewReader(`{"ssh_key":`+jsonString(key)+`}`)), "alice")
	rr := httptest.NewRecorder()
	s.handleGitSSHKey(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("POST sshkey: code=%d body=%q", rr.Code, rr.Body.String())
	}
	if gotMethod != http.MethodPost || gotWebuser != "alice" || gotAuth != "Bearer sekret-token" {
		t.Fatalf("forwarded method=%q webuser=%q auth=%q", gotMethod, gotWebuser, gotAuth)
	}
	if !strings.Contains(gotBody, "ABCDEF") {
		t.Fatalf("forwarded body missing key material: %q", gotBody)
	}
	// The key must never be echoed back to the browser.
	if strings.Contains(rr.Body.String(), "ABCDEF") {
		t.Fatalf("response leaked key material to the browser: %q", rr.Body.String())
	}

	// DELETE clears it: no body, forwarded as DELETE.
	gotMethod, gotBody = "", "x"
	req = withUser(httptest.NewRequest(http.MethodDelete, "/api/git/sshkey", nil), "alice")
	rr = httptest.NewRecorder()
	s.handleGitSSHKey(rr, req)
	if rr.Code != http.StatusOK || gotMethod != http.MethodDelete {
		t.Fatalf("DELETE sshkey: code=%d forwardedMethod=%q", rr.Code, gotMethod)
	}
}

// A missing/empty key is rejected at the proxy without calling aimee-server.
func TestGitSSHKeyRejectsEmpty(t *testing.T) {
	called := false
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/git/sshkey", func(w http.ResponseWriter, r *http.Request) {
		called = true
		w.Write([]byte(`{"ok":true}`))
	})
	cfg := startFakeV1(t, mux)
	if err := os.WriteFile(filepath.Join(filepath.Dir(cfg.socketPath), "server.token"),
		[]byte("t\n"), 0600); err != nil {
		t.Fatalf("write server.token: %v", err)
	}
	s := &server{cfg: cfg}

	req := withUser(httptest.NewRequest(http.MethodPost, "/api/git/sshkey",
		strings.NewReader(`{"ssh_key":""}`)), "alice")
	rr := httptest.NewRecorder()
	s.handleGitSSHKey(rr, req)
	if rr.Code != http.StatusBadRequest {
		t.Fatalf("empty key: code=%d, want 400", rr.Code)
	}
	if called {
		t.Fatalf("aimee-server was called for an empty key")
	}
}

// jsonString quotes s as a JSON string literal (small helper to avoid importing
// encoding/json just for the test body).
func jsonString(s string) string {
	var b strings.Builder
	b.WriteByte('"')
	for _, r := range s {
		switch r {
		case '"':
			b.WriteString(`\"`)
		case '\\':
			b.WriteString(`\\`)
		case '\n':
			b.WriteString(`\n`)
		default:
			b.WriteRune(r)
		}
	}
	b.WriteByte('"')
	return b.String()
}
