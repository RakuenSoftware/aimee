package main

import (
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestRoundtableMutationsRequireAdministrator(t *testing.T) {
	cases := []struct {
		name    string
		method  string
		apiPath string
		body    string
		item    bool
	}{
		{"create", http.MethodPost, "/api/roundtables", `{"name":"review"}`, false},
		{"edit", http.MethodPut, "/api/roundtables/review", `{"seats":[]}`, true},
		{"delete", http.MethodDelete, "/api/roundtables/review", "", true},
		{"select default", http.MethodPost, "/api/roundtables/active", `{"name":"review"}`, true},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			s := &server{}
			req := withUser(httptest.NewRequest(tc.method, tc.apiPath, strings.NewReader(tc.body)), "alice")
			rr := httptest.NewRecorder()
			if tc.item {
				s.handleRoundtableItem(rr, req)
			} else {
				s.handleRoundtables(rr, req)
			}
			if rr.Code != http.StatusForbidden || !strings.Contains(rr.Body.String(), "administrator access required") {
				t.Fatalf("code=%d body=%q, want administrator 403", rr.Code, rr.Body.String())
			}
		})
	}
}

func TestRoundtableAdministratorMutationsCarryAttestedIdentity(t *testing.T) {
	cases := []struct {
		name    string
		method  string
		apiPath string
		v1Path  string
		body    string
		item    bool
	}{
		{"create", http.MethodPost, "/api/roundtables", "/v1/roundtables", `{"name":"review"}`, false},
		{"edit", http.MethodPut, "/api/roundtables/review", "/v1/roundtables/review", `{"seats":[]}`, true},
		{"delete", http.MethodDelete, "/api/roundtables/review", "/v1/roundtables/review", "", true},
		{"select default", http.MethodPost, "/api/roundtables/active", "/v1/roundtables/active", `{"name":"review"}`, true},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			var gotMethod, gotWebuser, gotAuthorization string
			mux := http.NewServeMux()
			mux.HandleFunc(tc.v1Path, func(w http.ResponseWriter, r *http.Request) {
				gotMethod = r.Method
				gotWebuser = r.Header.Get("X-Aimee-Webuser")
				gotAuthorization = r.Header.Get("Authorization")
				w.Header().Set("Content-Type", "application/json")
				_, _ = w.Write([]byte(`{"ok":true}`))
			})
			cfg := startFakeV1(t, mux)
			s := &server{cfg: cfg}
			req := withUser(httptest.NewRequest(tc.method, tc.apiPath, strings.NewReader(tc.body)), "admin")
			rr := httptest.NewRecorder()
			if tc.item {
				s.handleRoundtableItem(rr, req)
			} else {
				s.handleRoundtables(rr, req)
			}
			if rr.Code != http.StatusOK {
				t.Fatalf("code=%d body=%q, want 200", rr.Code, rr.Body.String())
			}
			if gotMethod != tc.method || gotWebuser != "admin" || gotAuthorization != "" {
				t.Fatalf("forwarded method=%q webuser=%q auth=%q", gotMethod, gotWebuser, gotAuthorization)
			}
		})
	}
}

// The appliance administrator is the account that REPLACED the generated
// bootstrap login, recorded in webchat/bootstrap-replaced. Both gates used to
// compare against the literal name "admin", which on a set-up appliance is the
// one account that is definitively not the administrator — so the real operator
// got "administrator access required" on every preset mutation, including
// "save as default" (POST /api/roundtables/active).
func TestRoundtableAdminIsTheReplacementAccount(t *testing.T) {
	tmp := t.TempDir()
	if err := os.MkdirAll(filepath.Join(tmp, "webchat"), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(tmp, "webchat", "bootstrap-replaced"),
		[]byte("virant\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	s := &server{cfg: &config{dbPath: filepath.Join(tmp, "webchat.db")}}

	if got := s.adminUsername(); got != "virant" {
		t.Fatalf("adminUsername()=%q, want virant", got)
	}
	if !s.isAdmin(withUser(httptest.NewRequest(http.MethodGet, "/", nil), "virant")) {
		t.Fatal("the replacement account must be the administrator")
	}
	// and the pre-replacement name must no longer be privileged
	if s.isAdmin(withUser(httptest.NewRequest(http.MethodGet, "/", nil), "admin")) {
		t.Fatal("\"admin\" must not be privileged once it has been replaced")
	}
	if s.isAdmin(withUser(httptest.NewRequest(http.MethodGet, "/", nil), "")) {
		t.Fatal("an empty identity must never be the administrator")
	}

	// "save as default" must now reach the v1 layer rather than 403.
	mux := http.NewServeMux()
	reached := false
	mux.HandleFunc("/v1/roundtables/active", func(w http.ResponseWriter, r *http.Request) {
		reached = true
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"ok":true}`))
	})
	cfg := startFakeV1(t, mux)
	cfg.dbPath = filepath.Join(tmp, "webchat.db")
	s = &server{cfg: cfg}
	req := withUser(httptest.NewRequest(http.MethodPost, "/api/roundtables/active",
		strings.NewReader(`{"name":"default"}`)), "virant")
	rr := httptest.NewRecorder()
	s.handleRoundtableItem(rr, req)
	if rr.Code != http.StatusOK || !reached {
		t.Fatalf("save-as-default: code=%d reached=%v body=%q", rr.Code, reached, rr.Body.String())
	}
}

// With no bootstrap record at all the gate keeps its previous shape.
func TestRoundtableAdminFallsBackToAdminWithoutRecords(t *testing.T) {
	s := &server{cfg: &config{dbPath: filepath.Join(t.TempDir(), "webchat.db")}}
	if got := s.adminUsername(); got != "admin" {
		t.Fatalf("adminUsername()=%q, want admin", got)
	}
}
