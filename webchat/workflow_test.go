package main

import (
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The Workflow Actions lifecycle controls (Start/Pause/Stop/Delete) call
// /api/workflow/items/<id>/{pause,resume,stop} (POST) and DELETE
// /api/workflow/items/<id>. Regression: these once fell through the item proxy's
// switch to the default "bad path" 400 because the switch predated the /v1
// lifecycle routes. Each must reach its matching /v1 path under the webuser
// identity, not be rejected.
func TestWorkflowItemsLifecycleRouting(t *testing.T) {
	cases := []struct {
		name    string
		method  string
		apiPath string
		wantV1  string
	}{
		{"pause", http.MethodPost, "/api/workflow/items/wi123/pause", "/v1/workflow/items/wi123/pause"},
		{"resume", http.MethodPost, "/api/workflow/items/wi123/resume", "/v1/workflow/items/wi123/resume"},
		{"stop", http.MethodPost, "/api/workflow/items/wi123/stop", "/v1/workflow/items/wi123/stop"},
		{"delete", http.MethodDelete, "/api/workflow/items/wi123", "/v1/workflow/items/wi123"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			var gotMethod, gotPath, gotWebuser string
			var gotLen int64
			mux := http.NewServeMux()
			mux.HandleFunc(tc.wantV1, func(w http.ResponseWriter, r *http.Request) {
				gotMethod, gotPath = r.Method, r.URL.Path
				gotWebuser = r.Header.Get("X-Aimee-Webuser")
				gotLen = r.ContentLength
				w.Header().Set("Content-Type", "application/json")
				w.Write([]byte(`{"status":"ok"}`))
			})
			cfg := startFakeV1(t, mux)
			if err := os.WriteFile(filepath.Join(filepath.Dir(cfg.socketPath), "server.token"),
				[]byte("sekret-token\n"), 0600); err != nil {
				t.Fatalf("write server.token: %v", err)
			}
			s := &server{cfg: cfg}

			req := withUser(httptest.NewRequest(tc.method, tc.apiPath, nil), "alice")
			rr := httptest.NewRecorder()
			s.handleWorkflowItems(rr, req)

			if rr.Code != http.StatusOK || !strings.Contains(rr.Body.String(), `"status":"ok"`) {
				t.Fatalf("%s: code=%d body=%q (regressed to bad path?)", tc.name, rr.Code, rr.Body.String())
			}
			if gotMethod != tc.method || gotPath != tc.wantV1 {
				t.Fatalf("%s: proxied %s %s, want %s %s", tc.name, gotMethod, gotPath, tc.method, tc.wantV1)
			}
			if gotWebuser != "alice" {
				t.Fatalf("%s: X-Aimee-Webuser = %q, want alice", tc.name, gotWebuser)
			}
			// Body-less mutations: nothing should be forwarded (v1RequestWebuser
			// sends no body / no Content-Type when body==nil).
			if gotLen > 0 {
				t.Fatalf("%s: forwarded Content-Length=%d, want 0 (body-less)", tc.name, gotLen)
			}
		})
	}
}

// Any (method, suffix) pair the item proxy doesn't map must be refused as
// "bad path" rather than silently forwarded — including a wrong method on an
// otherwise-known lifecycle suffix and a mutating method on the bare id path.
// The fake /v1 backend has no handlers, so a forwarded request would surface as
// a non-"bad path" response (proving nothing leaked through).
func TestWorkflowItemsRejectsUnmapped(t *testing.T) {
	cases := []struct {
		name    string
		method  string
		apiPath string
	}{
		{"post-unknown-suffix", http.MethodPost, "/api/workflow/items/wi123/bogus"},
		{"delete-on-lifecycle-suffix", http.MethodDelete, "/api/workflow/items/wi123/pause"},
		{"put-on-lifecycle-suffix", http.MethodPut, "/api/workflow/items/wi123/resume"},
		{"get-on-lifecycle-suffix", http.MethodGet, "/api/workflow/items/wi123/stop"},
		{"post-on-bare-id", http.MethodPost, "/api/workflow/items/wi123"},
		{"put-on-bare-id", http.MethodPut, "/api/workflow/items/wi123"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			s := &server{cfg: startFakeV1(t, http.NewServeMux())}
			req := withUser(httptest.NewRequest(tc.method, tc.apiPath, nil), "alice")
			rr := httptest.NewRecorder()
			s.handleWorkflowItems(rr, req)
			if rr.Code != http.StatusBadRequest || !strings.Contains(rr.Body.String(), "bad path") {
				t.Fatalf("%s: code=%d body=%q, want 400 bad path", tc.name, rr.Code, rr.Body.String())
			}
		})
	}
}
