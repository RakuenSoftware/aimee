package main

import (
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestWorkflowGlobalConfigRequiresAdministrator(t *testing.T) {
	s := &server{}
	req := withUser(httptest.NewRequest(http.MethodPost, "/api/workflow/config/set", strings.NewReader(`{"key":"trigger.max_concurrent","value":2}`)), "alice")
	rr := httptest.NewRecorder()
	s.handleWorkflowConfigSet(rr, req)
	if rr.Code != http.StatusForbidden {
		t.Fatalf("status=%d, want 403", rr.Code)
	}
}

func TestWorkflowGlobalConfigRejectsWrongMethod(t *testing.T) {
	s := &server{}
	req := withUser(httptest.NewRequest(http.MethodPut, "/api/workflow/config/set",
		strings.NewReader(`{"key":"trigger.max_concurrent","value":2}`)), "admin")
	rec := httptest.NewRecorder()
	s.handleWorkflowConfigSet(rec, req)
	if rec.Code != http.StatusMethodNotAllowed {
		t.Fatalf("status=%d body=%s", rec.Code, rec.Body.String())
	}
}

func TestWorkflowGlobalDefinitionWritesRequireAdministrator(t *testing.T) {
	s := &server{}
	for _, tc := range []struct {
		name, method, target, body string
		handler                    func(http.ResponseWriter, *http.Request)
	}{
		{"save", http.MethodPost, "/api/workflow/save", `{}`, s.handleWorkflowSave},
		{"put-block", http.MethodPut, "/api/workflow/blocks/custom.test", `{}`, s.handleWorkflowBlockItem},
		{"delete-block", http.MethodDelete, "/api/workflow/blocks/custom.test", ``, s.handleWorkflowBlockItem},
	} {
		t.Run(tc.name, func(t *testing.T) {
			req := withUser(httptest.NewRequest(tc.method, tc.target, strings.NewReader(tc.body)), "alice")
			rec := httptest.NewRecorder()
			tc.handler(rec, req)
			if rec.Code != http.StatusForbidden {
				t.Fatalf("status=%d body=%s", rec.Code, rec.Body.String())
			}
		})
	}
}

func TestWorkflowBlocksAndWritesCarryAdministratorCapability(t *testing.T) {
	var calls []string
	mux := http.NewServeMux()
	for _, route := range []string{"/v1/workflow/blocks", "/v1/workflow/blocks/custom.test", "/v1/workflow/save"} {
		route := route
		mux.HandleFunc(route, func(w http.ResponseWriter, r *http.Request) {
			calls = append(calls, r.Method+":"+r.URL.Path+":"+r.Header.Get("X-Aimee-Webuser")+":"+r.Header.Get("X-Aimee-Workflow-Operator"))
			_, _ = w.Write([]byte(`{"ok":true,"blocks":[]}`))
		})
	}
	s := &server{cfg: startFakeV1(t, mux)}
	for _, tc := range []struct {
		method, target, user, body string
		handler                    func(http.ResponseWriter, *http.Request)
	}{
		{http.MethodGet, "/api/workflow/blocks", "alice", ``, s.handleWorkflowBlocks},
		{http.MethodGet, "/api/workflow/blocks", "admin", ``, s.handleWorkflowBlocks},
		{http.MethodPut, "/api/workflow/blocks/custom.test", "admin", `{}`, s.handleWorkflowBlockItem},
		{http.MethodPost, "/api/workflow/save", "admin", `{}`, s.handleWorkflowSave},
	} {
		req := withUser(httptest.NewRequest(tc.method, tc.target, strings.NewReader(tc.body)), tc.user)
		rec := httptest.NewRecorder()
		tc.handler(rec, req)
		if rec.Code != http.StatusOK {
			t.Fatalf("%s %s status=%d body=%s", tc.method, tc.target, rec.Code, rec.Body.String())
		}
	}
	want := []string{
		"GET:/v1/workflow/blocks:alice:",
		"GET:/v1/workflow/blocks:admin:true",
		"PUT:/v1/workflow/blocks/custom.test:admin:true",
		"POST:/v1/workflow/save:admin:true",
	}
	if len(calls) != len(want) {
		t.Fatalf("calls=%v, want %v", calls, want)
	}
	for i := range want {
		if calls[i] != want[i] {
			t.Fatalf("calls=%v, want %v", calls, want)
		}
	}
}

func TestWorkflowOperatorItemRoutesRejectOrdinaryUsersBeforeProxy(t *testing.T) {
	proxied := false
	mux := http.NewServeMux()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		proxied = true
		w.WriteHeader(http.StatusOK)
	})
	s := &server{cfg: startFakeV1(t, mux)}
	for _, tc := range []struct {
		method, target, body string
	}{
		{http.MethodGet, "/api/workflow/items/all", ``},
		{http.MethodPost, "/api/workflow/items/wi123/gate", `{"decision":"approve"}`},
	} {
		req := withUser(httptest.NewRequest(tc.method, tc.target, strings.NewReader(tc.body)), "alice")
		rec := httptest.NewRecorder()
		s.handleWorkflowItems(rec, req)
		if rec.Code != http.StatusForbidden {
			t.Fatalf("%s status=%d body=%s", tc.target, rec.Code, rec.Body.String())
		}
	}
	if proxied {
		t.Fatal("ordinary-user operator request reached the control plane")
	}
}

func TestWorkflowOperatorCapabilityCannotBeSpoofedByBrowserHeader(t *testing.T) {
	gotOperator := "unset"
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/workflow/items/wi123", func(w http.ResponseWriter, r *http.Request) {
		gotOperator = r.Header.Get("X-Aimee-Workflow-Operator")
		_, _ = w.Write([]byte(`{"id":"wi123"}`))
	})
	s := &server{cfg: startFakeV1(t, mux)}
	req := withUser(httptest.NewRequest(http.MethodGet, "/api/workflow/items/wi123", nil), "alice")
	req.Header.Set("X-Aimee-Workflow-Operator", "true")
	rec := httptest.NewRecorder()
	s.handleWorkflowItems(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("status=%d body=%s", rec.Code, rec.Body.String())
	}
	if gotOperator != "" {
		t.Fatalf("spoofed operator capability was forwarded: %q", gotOperator)
	}
}

func TestWorkflowTriggersReadCarriesWebuserIdentity(t *testing.T) {
	// A deployed test process inherits the production WFE environment. The fake
	// server's explicit config must win so this test cannot mutate live state.
	t.Setenv("AIMEE_WFE_ENGINE", "go")
	t.Setenv("AIMEE_WFE_HTTP_SOCKET", filepath.Join(t.TempDir(), "hostile-live.sock"))
	var gotMethod, gotUser, gotOperator string
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/workflow/triggers", func(w http.ResponseWriter, r *http.Request) {
		gotMethod = r.Method
		gotUser = r.Header.Get("X-Aimee-Webuser")
		gotOperator = r.Header.Get("X-Aimee-Workflow-Operator")
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"editable":true,"triggers":[]}`))
	})
	s := &server{cfg: startFakeV1(t, mux)}
	req := withUser(httptest.NewRequest(http.MethodGet, "/api/workflow/triggers", nil), "admin")
	rr := httptest.NewRecorder()
	s.handleWorkflowTriggers(rr, req)
	if rr.Code != http.StatusOK || gotMethod != http.MethodGet || gotUser != "admin" || gotOperator != "true" {
		t.Fatalf("code=%d method=%q user=%q operator=%q body=%s", rr.Code, gotMethod, gotUser, gotOperator, rr.Body.String())
	}
}

func TestWorkflowPolicyMapsReplacementAdministratorToControlPlaneCapability(t *testing.T) {
	root := t.TempDir()
	if err := os.MkdirAll(filepath.Join(root, "webchat"), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, "webchat", "bootstrap-replaced"), []byte("virant\n"), 0o600); err != nil {
		t.Fatal(err)
	}

	var calls []string
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/workflow/triggers", func(w http.ResponseWriter, r *http.Request) {
		calls = append(calls, r.Method+":"+r.Header.Get("X-Aimee-Webuser")+":"+r.Header.Get("X-Aimee-Workflow-Operator"))
		_, _ = w.Write([]byte(`{"editable":true,"triggers":[]}`))
	})
	mux.HandleFunc("/v1/workflow/config/set", func(w http.ResponseWriter, r *http.Request) {
		calls = append(calls, r.Method+":"+r.Header.Get("X-Aimee-Webuser")+":"+r.Header.Get("X-Aimee-Workflow-Operator"))
		_, _ = w.Write([]byte(`{"ok":true,"key":"trigger_rules"}`))
	})
	mux.HandleFunc("/v1/workflow/items/all", func(w http.ResponseWriter, r *http.Request) {
		calls = append(calls, r.Method+":"+r.Header.Get("X-Aimee-Webuser")+":"+r.Header.Get("X-Aimee-Workflow-Operator"))
		_, _ = w.Write([]byte(`{"items":[]}`))
	})
	mux.HandleFunc("/v1/workflow/items/wi1/gate", func(w http.ResponseWriter, r *http.Request) {
		calls = append(calls, r.Method+":"+r.Header.Get("X-Aimee-Webuser")+":"+r.Header.Get("X-Aimee-Workflow-Operator"))
		_, _ = w.Write([]byte(`{"id":"wi1"}`))
	})
	mux.HandleFunc("/v1/workflow/items/wi1", func(w http.ResponseWriter, r *http.Request) {
		calls = append(calls, r.Method+":"+r.Header.Get("X-Aimee-Webuser")+":"+r.Header.Get("X-Aimee-Workflow-Operator"))
		_, _ = w.Write([]byte(`{"id":"wi1"}`))
	})
	cfg := startFakeV1(t, mux)
	cfg.dbPath = filepath.Join(root, "webchat.db")
	s := &server{cfg: cfg}

	read := withUser(httptest.NewRequest(http.MethodGet, "/api/workflow/triggers", nil), "virant")
	readRecorder := httptest.NewRecorder()
	s.handleWorkflowTriggers(readRecorder, read)
	write := withUser(httptest.NewRequest(http.MethodPost, "/api/workflow/config/set",
		strings.NewReader(`{"key":"trigger_rules","value":[],"previous_version":"v1"}`)), "virant")
	writeRecorder := httptest.NewRecorder()
	s.handleWorkflowConfigSet(writeRecorder, write)
	all := withUser(httptest.NewRequest(http.MethodGet, "/api/workflow/items/all", nil), "virant")
	allRecorder := httptest.NewRecorder()
	s.handleWorkflowItems(allRecorder, all)
	gate := withUser(httptest.NewRequest(http.MethodPost, "/api/workflow/items/wi1/gate",
		strings.NewReader(`{"decision":"approve"}`)), "virant")
	gateRecorder := httptest.NewRecorder()
	s.handleWorkflowItems(gateRecorder, gate)
	// Once the bootstrap administrator is named virant, an unrelated literal
	// "admin" identity must remain an ordinary workflow owner.
	literalAdmin := withUser(httptest.NewRequest(http.MethodGet, "/api/workflow/items/wi1", nil), "admin")
	literalAdminRecorder := httptest.NewRecorder()
	s.handleWorkflowItems(literalAdminRecorder, literalAdmin)

	if readRecorder.Code != http.StatusOK || writeRecorder.Code != http.StatusOK || allRecorder.Code != http.StatusOK || gateRecorder.Code != http.StatusOK || literalAdminRecorder.Code != http.StatusOK {
		t.Fatalf("read=%d %s write=%d %s all=%d %s gate=%d %s literal-admin=%d %s", readRecorder.Code, readRecorder.Body.String(), writeRecorder.Code, writeRecorder.Body.String(), allRecorder.Code, allRecorder.Body.String(), gateRecorder.Code, gateRecorder.Body.String(), literalAdminRecorder.Code, literalAdminRecorder.Body.String())
	}
	want := []string{"GET:virant:true", "POST:virant:true", "GET:virant:true", "POST:virant:true", "GET:admin:"}
	if len(calls) != len(want) || calls[0] != want[0] || calls[1] != want[1] || calls[2] != want[2] || calls[3] != want[3] || calls[4] != want[4] {
		t.Fatalf("control-plane calls=%v, want %v", calls, want)
	}
}

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

// Child-slice work-item ids are "<parent>.s<N>" — they contain a dot. The proxy
// must route them through (detail GET, /gate, /events, /proposal), not reject the
// dot as traversal. Regression: a "/.%" guard rejected every slice id as "bad
// path", so a slice parked at a human gate could not be opened or approved.
func TestWorkflowItemsSliceIDRouting(t *testing.T) {
	cases := []struct {
		name    string
		method  string
		apiPath string
		wantV1  string
		user    string
	}{
		{"detail", http.MethodGet, "/api/workflow/items/wi123.s0", "/v1/workflow/items/wi123.s0", "alice"},
		{"gate", http.MethodPost, "/api/workflow/items/wi123.s0/gate", "/v1/workflow/items/wi123.s0/gate", "admin"},
		{"events", http.MethodGet, "/api/workflow/items/wi123.s0/events", "/v1/workflow/items/wi123.s0/events", "alice"},
		{"proposal", http.MethodGet, "/api/workflow/items/wi123.s0/proposal", "/v1/workflow/items/wi123.s0/proposal", "alice"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			var gotPath string
			mux := http.NewServeMux()
			mux.HandleFunc(tc.wantV1, func(w http.ResponseWriter, r *http.Request) {
				gotPath = r.URL.Path
				w.Header().Set("Content-Type", "application/json")
				w.Write([]byte(`{"status":"ok"}`))
			})
			cfg := startFakeV1(t, mux)
			s := &server{cfg: cfg}
			var body []byte
			if tc.method == http.MethodPost {
				body = []byte(`{"decision":"approve"}`)
			}
			req := withUser(httptest.NewRequest(tc.method, tc.apiPath, strings.NewReader(string(body))), tc.user)
			rr := httptest.NewRecorder()
			s.handleWorkflowItems(rr, req)
			if rr.Code != http.StatusOK || !strings.Contains(rr.Body.String(), `"status":"ok"`) {
				t.Fatalf("%s: code=%d body=%q (slice id rejected as bad path?)", tc.name, rr.Code, rr.Body.String())
			}
			if gotPath != tc.wantV1 {
				t.Fatalf("%s: proxied to %s, want %s", tc.name, gotPath, tc.wantV1)
			}
		})
	}
}

// A ".." traversal sequence (or a '/'/'%' separator/encoding) must still be
// refused even though a single '.' is now allowed for slice ids.
func TestWorkflowItemsRejectsTraversal(t *testing.T) {
	for _, apiPath := range []string{
		"/api/workflow/items/wi123..evil",
		"/api/workflow/items/..",
		"/api/workflow/items/wi123..",
		"/api/workflow/items/..wi.s0", // leading traversal mixed with legit dots
		"/api/workflow/items/wi%25s0", // %25 -> '%' in the id: percent still rejected
		"/api/workflow/items/a%2Fb",   // %2F -> '/': a smuggled separator is refused
	} {
		s := &server{cfg: startFakeV1(t, http.NewServeMux())}
		req := withUser(httptest.NewRequest(http.MethodGet, apiPath, nil), "alice")
		rr := httptest.NewRecorder()
		s.handleWorkflowItems(rr, req)
		if rr.Code != http.StatusBadRequest || !strings.Contains(rr.Body.String(), "bad path") {
			t.Fatalf("%s: code=%d body=%q, want 400 bad path", apiPath, rr.Code, rr.Body.String())
		}
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
