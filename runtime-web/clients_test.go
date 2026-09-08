package main

import (
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestClientManagementCannotClaimTemporaryBootstrapOwner(t *testing.T) {
	s := newLoginTestServer(t, stubIdentity{password: "secret"})
	s.cfg.dbPath = filepath.Join(t.TempDir(), "webchat.db")
	if err := os.MkdirAll(s.setupAccountDir(), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(s.setupAccountBootstrapUser(), []byte("generated:aimee-012345abcdef\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	w := httptest.NewRecorder()
	s.handleClients(w, httptest.NewRequest(http.MethodPost, "/api/clients", strings.NewReader(`{"name":"Laptop"}`)))
	if w.Code != http.StatusConflict {
		t.Fatalf("temporary account could claim device ownership: status %d", w.Code)
	}
}

func TestClientManagementRequiresLogin(t *testing.T) {
	s := newLoginTestServer(t, stubIdentity{password: "secret"})
	for _, path := range []string{"/api/clients", "/api/clients/revoke"} {
		w := httptest.NewRecorder()
		r := httptest.NewRequest(http.MethodPost, path, strings.NewReader(`{}`))
		s.requireAuth(s.handleClients)(w, r)
		if w.Code != http.StatusUnauthorized {
			t.Fatalf("%s status %d", path, w.Code)
		}
	}
}

func TestClientManagementRejectsUnsupportedMethodsAndOversizedBodies(t *testing.T) {
	var s server
	for _, tc := range []struct {
		method, path, body string
		status             int
	}{
		{http.MethodDelete, "/api/clients", "", 405},
		{http.MethodGet, "/api/clients/revoke", "", 405},
		{http.MethodPost, "/api/clients", strings.Repeat("x", 5000), 400},
	} {
		w := httptest.NewRecorder()
		s.handleClients(w, httptest.NewRequest(tc.method, tc.path, strings.NewReader(tc.body)))
		if w.Code != tc.status {
			t.Fatalf("%s %s returned %d", tc.method, tc.path, w.Code)
		}
	}
}
