package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

// TestCliOauthHandlerProxies checks the subscription-OAuth proxy forwards the
// browser's {vendor,session,code} to the matching agent.cli_oauth_* op and relays
// the server's response (URL + session + device code) back verbatim.
func TestCliOauthHandlerProxies(t *testing.T) {
	var gotBody map[string]any
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/agent/cli_oauth_start", func(w http.ResponseWriter, r *http.Request) {
		_ = json.NewDecoder(r.Body).Decode(&gotBody)
		fmt.Fprint(w, `{"status":"ok","vendor":"claude","url":"https://auth.example/x","session":"s1","needs_code_back":true}`)
	})
	s := &server{cfg: startFakeV1(t, mux)}

	rr := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodPost, "/api/agents/oauth/start", strings.NewReader(`{"vendor":"claude"}`))
	s.cliOauthHandler("agent.cli_oauth_start", 5*time.Second)(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("code=%d body=%q", rr.Code, rr.Body.String())
	}
	if gotBody["vendor"] != "claude" {
		t.Fatalf("forwarded vendor=%v want claude", gotBody["vendor"])
	}
	if gotBody["method"] != "agent.cli_oauth_start" {
		t.Fatalf("forwarded method=%v", gotBody["method"])
	}
	var out map[string]any
	if err := json.Unmarshal(rr.Body.Bytes(), &out); err != nil {
		t.Fatalf("decode relay: %v", err)
	}
	if out["url"] != "https://auth.example/x" || out["session"] != "s1" {
		t.Fatalf("relay dropped fields: %v", out)
	}
	if out["needs_code_back"] != true {
		t.Fatalf("relay needs_code_back=%v want true", out["needs_code_back"])
	}
}

// TestCliOauthHandlerForwardsSessionAndCode confirms the optional session/code
// fields are forwarded (the claude paste-code-back hop) and omitted when blank.
func TestCliOauthHandlerForwardsSessionAndCode(t *testing.T) {
	var gotBody map[string]any
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/agent/cli_oauth_code", func(w http.ResponseWriter, r *http.Request) {
		_ = json.NewDecoder(r.Body).Decode(&gotBody)
		fmt.Fprint(w, `{"status":"ok"}`)
	})
	s := &server{cfg: startFakeV1(t, mux)}

	rr := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodPost, "/api/agents/oauth/code",
		strings.NewReader(`{"vendor":"claude","session":"s1","code":"ABC-123"}`))
	s.cliOauthHandler("agent.cli_oauth_code", 5*time.Second)(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("code=%d body=%q", rr.Code, rr.Body.String())
	}
	if gotBody["session"] != "s1" || gotBody["code"] != "ABC-123" {
		t.Fatalf("session/code not forwarded: %v", gotBody)
	}
}

// TestCliOauthHandlerRejectsMissingVendor: no vendor is a 400 before any upstream
// call (the op requires a vendor and there is nothing to proxy without it).
func TestCliOauthHandlerRejectsMissingVendor(t *testing.T) {
	called := false
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/agent/cli_oauth_start", func(http.ResponseWriter, *http.Request) { called = true })
	s := &server{cfg: startFakeV1(t, mux)}

	rr := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodPost, "/api/agents/oauth/start", strings.NewReader(`{}`))
	s.cliOauthHandler("agent.cli_oauth_start", 5*time.Second)(rr, req)

	if rr.Code != http.StatusBadRequest {
		t.Fatalf("code=%d want 400", rr.Code)
	}
	if called {
		t.Fatalf("upstream should not be called without a vendor")
	}
}
