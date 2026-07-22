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
			if err := os.WriteFile(filepath.Join(filepath.Dir(cfg.socketPath), "server.token"),
				[]byte("roundtable-secret\n"), 0600); err != nil {
				t.Fatalf("write server.token: %v", err)
			}
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
			if gotMethod != tc.method || gotWebuser != "admin" || gotAuthorization != "Bearer roundtable-secret" {
				t.Fatalf("forwarded method=%q webuser=%q auth=%q", gotMethod, gotWebuser, gotAuthorization)
			}
		})
	}
}
