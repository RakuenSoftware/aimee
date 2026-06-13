package main

import (
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The unlock handler must reach aimee-server's /v1/vault/unlock carrying BOTH
// the server.token bearer AND X-Aimee-Webuser (the trust headers aimee-server
// requires to resolve the webuser: vault), plus the password body.
func TestVaultUnlockSendsWebuserAndBearer(t *testing.T) {
	var gotAuth, gotWebuser, gotBody string
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/vault/unlock", func(w http.ResponseWriter, r *http.Request) {
		gotAuth = r.Header.Get("Authorization")
		gotWebuser = r.Header.Get("X-Aimee-Webuser")
		b := make([]byte, r.ContentLength)
		r.Body.Read(b)
		gotBody = string(b)
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`{"status":"ok","principal":"webuser:alice"}`))
	})
	cfg := startFakeV1(t, mux)
	if err := os.WriteFile(filepath.Join(filepath.Dir(cfg.socketPath), "server.token"),
		[]byte("sekret-token\n"), 0600); err != nil {
		t.Fatalf("write server.token: %v", err)
	}
	s := &server{cfg: cfg}

	req := withUser(httptest.NewRequest(http.MethodPost, "/api/vault/unlock",
		strings.NewReader(`{"password":"hunter2"}`)), "alice")
	rr := httptest.NewRecorder()
	s.handleVaultUnlock(rr, req)

	if rr.Code != http.StatusOK || !strings.Contains(rr.Body.String(), `"status":"ok"`) {
		t.Fatalf("unlock: code=%d body=%q", rr.Code, rr.Body.String())
	}
	if gotAuth != "Bearer sekret-token" {
		t.Fatalf("Authorization = %q, want Bearer sekret-token", gotAuth)
	}
	if gotWebuser != "alice" {
		t.Fatalf("X-Aimee-Webuser = %q, want alice", gotWebuser)
	}
	if !strings.Contains(gotBody, "hunter2") {
		t.Fatalf("body missing password: %q", gotBody)
	}
}

// Fail-closed: with no server.token the backend must NOT call aimee-server and
// must return an auth error (no webuser assertion is sent without the secret).
func TestVaultUnlockFailsClosedWithoutServerToken(t *testing.T) {
	called := false
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/vault/unlock", func(w http.ResponseWriter, r *http.Request) {
		called = true
		w.Write([]byte(`{"status":"ok"}`))
	})
	cfg := startFakeV1(t, mux) // note: no server.token written
	s := &server{cfg: cfg}

	req := withUser(httptest.NewRequest(http.MethodPost, "/api/vault/unlock",
		strings.NewReader(`{"password":"hunter2"}`)), "alice")
	rr := httptest.NewRecorder()
	s.handleVaultUnlock(rr, req)

	if called {
		t.Fatalf("aimee-server was called without a server.token (should fail closed)")
	}
	if rr.Code == http.StatusOK {
		t.Fatalf("expected a non-200 fail-closed status, got 200: %q", rr.Body.String())
	}
}

// Defense in depth: even if aimee-server (wrongly) echoes a secret/value/KEK in
// a vault response, the webchat boundary must NOT relay it to the browser.
func TestVaultResponsesNeverLeakSecrets(t *testing.T) {
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/vault/list", func(w http.ResponseWriter, r *http.Request) {
		// A deliberately-leaky upstream: a secret/value/wrapped_dek alongside names.
		w.Write([]byte(`{"status":"ok","credentials":[{"agent":"claude","cred":"api_key","secret":"sk-LEAKED","value":"sk-LEAKED2","wrapped_dek":"WRAPKEYLEAK"}]}`))
	})
	mux.HandleFunc("/v1/vault/unlock", func(w http.ResponseWriter, r *http.Request) {
		w.Write([]byte(`{"status":"ok","kek":"RAWKEKLEAK","root_key":"sk-LEAKED3"}`))
	})
	cfg := startFakeV1(t, mux)
	if err := os.WriteFile(filepath.Join(filepath.Dir(cfg.socketPath), "server.token"),
		[]byte("tok\n"), 0600); err != nil {
		t.Fatalf("write server.token: %v", err)
	}
	s := &server{cfg: cfg}

	// list must surface only names.
	rr := httptest.NewRecorder()
	s.handleVaultCredentials(rr, withUser(httptest.NewRequest(http.MethodGet, "/api/vault/credentials", nil), "eve"))
	body := rr.Body.String()
	for _, leak := range []string{"sk-LEAKED", "sk-LEAKED2", "WRAPKEYLEAK"} {
		if strings.Contains(body, leak) {
			t.Fatalf("list leaked %q to the browser: %s", leak, body)
		}
	}
	if !strings.Contains(body, `"agent":"claude"`) || !strings.Contains(body, `"cred":"api_key"`) {
		t.Fatalf("list dropped the names: %s", body)
	}

	// unlock must surface only {status:ok}, never the upstream key fields.
	rr2 := httptest.NewRecorder()
	s.handleVaultUnlock(rr2, withUser(httptest.NewRequest(http.MethodPost, "/api/vault/unlock", strings.NewReader(`{"password":"p"}`)), "eve"))
	b2 := rr2.Body.String()
	for _, leak := range []string{"RAWKEKLEAK", "sk-LEAKED3"} {
		if strings.Contains(b2, leak) {
			t.Fatalf("unlock leaked %q to the browser: %s", leak, b2)
		}
	}
}

// The credentials endpoint maps GET->vault.list, POST->vault.set, DELETE->vault.delete.
func TestVaultCredentialsRoutesByMethod(t *testing.T) {
	var listed, set, deleted bool
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/vault/list", func(w http.ResponseWriter, r *http.Request) {
		listed = true
		w.Write([]byte(`{"status":"ok","credentials":[]}`))
	})
	mux.HandleFunc("/v1/vault/set", func(w http.ResponseWriter, r *http.Request) {
		set = true
		w.Write([]byte(`{"status":"ok"}`))
	})
	mux.HandleFunc("/v1/vault/delete", func(w http.ResponseWriter, r *http.Request) {
		deleted = true
		w.Write([]byte(`{"status":"ok"}`))
	})
	cfg := startFakeV1(t, mux)
	if err := os.WriteFile(filepath.Join(filepath.Dir(cfg.socketPath), "server.token"),
		[]byte("tok\n"), 0600); err != nil {
		t.Fatalf("write server.token: %v", err)
	}
	s := &server{cfg: cfg}

	do := func(method, body string) int {
		req := withUser(httptest.NewRequest(method, "/api/vault/credentials", strings.NewReader(body)), "bob")
		rr := httptest.NewRecorder()
		s.handleVaultCredentials(rr, req)
		return rr.Code
	}
	do(http.MethodGet, "")
	do(http.MethodPost, `{"agent":"claude","cred":"api_key","secret":"sk-x"}`)
	do(http.MethodDelete, `{"agent":"claude","cred":"api_key"}`)
	if !listed || !set || !deleted {
		t.Fatalf("routing: list=%v set=%v delete=%v", listed, set, deleted)
	}
}
