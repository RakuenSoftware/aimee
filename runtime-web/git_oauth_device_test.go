package main

import (
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

// POST /api/git/oauth/device/start forwards {provider, host} to aimee-server and
// relays the device-flow fields (user_code, verification_uri).
func TestGitOauthDeviceStartProxy(t *testing.T) {
	var gotWebuser, gotBody string
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/git/oauth/device/start", func(w http.ResponseWriter, r *http.Request) {
		gotWebuser = r.Header.Get("X-Aimee-Webuser")
		b, _ := io.ReadAll(r.Body)
		gotBody = string(b)
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`{"ok":true,"user_code":"ABCD-1234","verification_uri":"https://gitlab.com/-/device","interval":5}`))
	})
	cfg := startFakeV1(t, mux)
	writeServerToken(t, cfg)
	s := &server{cfg: cfg}

	req := withUser(httptest.NewRequest(http.MethodPost, "/api/git/oauth/device/start",
		strings.NewReader(`{"provider":"gitlab","host":"gitlab.com"}`)), "alice")
	rr := httptest.NewRecorder()
	s.handleGitOauthDeviceStart(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("device/start: code=%d body=%q", rr.Code, rr.Body.String())
	}
	if gotWebuser != "alice" || !strings.Contains(gotBody, `"provider":"gitlab"`) {
		t.Fatalf("forwarded webuser=%q body=%q", gotWebuser, gotBody)
	}
	if !strings.Contains(rr.Body.String(), "ABCD-1234") {
		t.Fatalf("relayed body missing user_code: %q", rr.Body.String())
	}
}

// device/start rejects a missing provider at the proxy.
func TestGitOauthDeviceStartRejectsMissingProvider(t *testing.T) {
	called := false
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/git/oauth/device/start", func(w http.ResponseWriter, r *http.Request) {
		called = true
		w.Write([]byte(`{}`))
	})
	cfg := startFakeV1(t, mux)
	writeServerToken(t, cfg)
	s := &server{cfg: cfg}

	req := withUser(httptest.NewRequest(http.MethodPost, "/api/git/oauth/device/start",
		strings.NewReader(`{"host":"gitlab.com"}`)), "alice")
	rr := httptest.NewRecorder()
	s.handleGitOauthDeviceStart(rr, req)
	if rr.Code != http.StatusBadRequest {
		t.Fatalf("missing provider: code=%d, want 400", rr.Code)
	}
	if called {
		t.Fatalf("aimee-server was called despite missing provider")
	}
}

// device/config GET forwards provider+host as query params; POST forwards the body.
func TestGitOauthDeviceConfigProxy(t *testing.T) {
	var getProvider, getHost, postBody string
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/git/oauth/device/config", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		if r.Method == http.MethodGet {
			getProvider = r.URL.Query().Get("provider")
			getHost = r.URL.Query().Get("host")
			w.Write([]byte(`{"configured":true,"client_id":"cid-1"}`))
			return
		}
		b, _ := io.ReadAll(r.Body)
		postBody = string(b)
		w.Write([]byte(`{"ok":true}`))
	})
	cfg := startFakeV1(t, mux)
	writeServerToken(t, cfg)
	s := &server{cfg: cfg}

	// GET
	req := withUser(httptest.NewRequest(http.MethodGet,
		"/api/git/oauth/device/config?provider=gitea&host=gitea.example.com", nil), "alice")
	rr := httptest.NewRecorder()
	s.handleGitOauthDeviceConfig(rr, req)
	if rr.Code != http.StatusOK || getProvider != "gitea" || getHost != "gitea.example.com" {
		t.Fatalf("config GET: code=%d provider=%q host=%q", rr.Code, getProvider, getHost)
	}
	if !strings.Contains(rr.Body.String(), "cid-1") {
		t.Fatalf("config GET relay missing client_id: %q", rr.Body.String())
	}

	// POST
	req = withUser(httptest.NewRequest(http.MethodPost, "/api/git/oauth/device/config",
		strings.NewReader(`{"provider":"gitea","host":"gitea.example.com","client_id":"cid-2"}`)), "alice")
	rr = httptest.NewRecorder()
	s.handleGitOauthDeviceConfig(rr, req)
	if rr.Code != http.StatusOK || !strings.Contains(postBody, `"client_id":"cid-2"`) {
		t.Fatalf("config POST: code=%d body=%q", rr.Code, postBody)
	}
}
