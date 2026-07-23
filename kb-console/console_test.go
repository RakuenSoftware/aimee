package main

import (
	"io"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

// newTestServer builds a server backed by a stub kb and a temp session store.
func newTestServer(t *testing.T, kbURL string) *server {
	t.Helper()
	home := t.TempDir()
	cfg := &config{consoleHome: home, kbBaseURL: kbURL}
	ss, err := openSessionStore(filepath.Join(home, "s.db"))
	if err != nil {
		t.Fatalf("session store: %v", err)
	}
	vault := newCredentialVault(maxOIDCCredentials)
	ss.vault = vault
	return &server{
		cfg:              cfg,
		auth:             newAuthenticator(cfg),
		sessions:         ss,
		kbBearer:         "scope:console-admin:c1:secret",
		kbClient:         &http.Client{},
		oidcTokens:       vault,
		fleetOIDCEnabled: true,
		logins:           newRateLimiter(5, time.Minute),
	}
}

func TestProxyForwardsFleetWithOIDCOnly(t *testing.T) {
	var seen http.Request
	kb := stubKB(t, &seen)
	defer kb.Close()
	srv := newTestServer(t, kb.URL)
	exp := time.Now().Add(time.Hour)
	sess, _ := srv.sessions.create(&principal{iss: "https://idp", sub: "alice", expires: exp}, false)
	if err := srv.oidcTokens.put(sess.id, sess.iss, sess.sub, exp, "signed-oidc-token"); err != nil {
		t.Fatal(err)
	}
	r := httptest.NewRequest("GET", "/api/v1/servers?team=9", nil)
	r.Header.Set("Authorization", "Bearer attacker-selected")
	r.AddCookie(&http.Cookie{Name: sessionCookie, Value: sess.id})
	w := httptest.NewRecorder()
	srv.handleAPI(w, r)
	if w.Code != http.StatusOK {
		t.Fatalf("fleet proxy status = %d", w.Code)
	}
	if got := seen.Header.Get("Authorization"); got != "Bearer signed-oidc-token" {
		t.Fatalf("fleet Authorization = %q", got)
	}
}

func TestProxyFleetBreakGlassAndMissingVaultFailBeforeUpstream(t *testing.T) {
	var seen http.Request
	kb := stubKB(t, &seen)
	defer kb.Close()
	srv := newTestServer(t, kb.URL)
	bg, _ := srv.sessions.create(&principal{iss: "break-glass", sub: "break-glass"}, true)
	r := httptest.NewRequest("GET", "/api/v1/servers?team=9", nil)
	r.AddCookie(&http.Cookie{Name: sessionCookie, Value: bg.id})
	w := httptest.NewRecorder()
	srv.handleAPI(w, r)
	if w.Code != http.StatusForbidden || seen.Method != "" {
		t.Fatalf("break-glass fleet request reached upstream: status=%d method=%q", w.Code, seen.Method)
	}

	seen = http.Request{}
	exp := time.Now().Add(time.Hour)
	oidc, _ := srv.sessions.create(&principal{iss: "https://idp", sub: "alice", expires: exp}, false)
	r = httptest.NewRequest("GET", "/api/v1/servers?team=9", nil)
	r.AddCookie(&http.Cookie{Name: sessionCookie, Value: oidc.id})
	w = httptest.NewRecorder()
	srv.handleAPI(w, r)
	if w.Code != http.StatusUnauthorized || seen.Method != "" {
		t.Fatalf("missing-vault fleet request reached upstream: status=%d method=%q", w.Code, seen.Method)
	}
}

func TestProxyResponseLimitIsAtomic(t *testing.T) {
	kb := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusOK)
		_, _ = io.WriteString(w, strings.Repeat("x", maxProxyResponseBytes+1))
	}))
	defer kb.Close()
	srv := newTestServer(t, kb.URL)
	sess, _ := srv.sessions.create(&principal{iss: "i", sub: "u"}, false)
	r := httptest.NewRequest("GET", "/api/v1/console/overview", nil)
	r.AddCookie(&http.Cookie{Name: sessionCookie, Value: sess.id})
	w := httptest.NewRecorder()
	srv.handleAPI(w, r)
	if w.Code != http.StatusBadGateway || strings.Contains(w.Body.String(), "xxx") {
		t.Fatalf("overflow must be atomic 502, got status=%d body-prefix=%q", w.Code, w.Body.String()[:min(32, w.Body.Len())])
	}
}

func TestProxyResponseExactLimitSucceeds(t *testing.T) {
	kb := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusOK)
		_, _ = io.WriteString(w, strings.Repeat("x", maxProxyResponseBytes))
	}))
	defer kb.Close()
	srv := newTestServer(t, kb.URL)
	sess, _ := srv.sessions.create(&principal{iss: "i", sub: "u"}, false)
	r := httptest.NewRequest("GET", "/api/v1/console/overview", nil)
	r.AddCookie(&http.Cookie{Name: sessionCookie, Value: sess.id})
	w := httptest.NewRecorder()
	srv.handleAPI(w, r)
	if w.Code != http.StatusOK || w.Body.Len() != maxProxyResponseBytes {
		t.Fatalf("exact-limit response status=%d bytes=%d", w.Code, w.Body.Len())
	}
}

func TestFleetUpstreamUnauthorizedInvalidatesSession(t *testing.T) {
	kb := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		writeJSON(w, http.StatusUnauthorized, map[string]string{"error": "expired"})
	}))
	defer kb.Close()
	srv := newTestServer(t, kb.URL)
	exp := time.Now().Add(time.Hour)
	sess, _ := srv.sessions.create(&principal{iss: "https://idp", sub: "alice", expires: exp}, false)
	if err := srv.oidcTokens.put(sess.id, sess.iss, sess.sub, exp, "signed-oidc-token"); err != nil {
		t.Fatal(err)
	}
	r := httptest.NewRequest("GET", "/api/v1/servers?team=9", nil)
	r.AddCookie(&http.Cookie{Name: sessionCookie, Value: sess.id})
	w := httptest.NewRecorder()
	srv.handleAPI(w, r)
	if w.Code != http.StatusUnauthorized {
		t.Fatalf("upstream status = %d", w.Code)
	}
	if _, err := srv.sessions.get(sess.id); err == nil {
		t.Fatal("401 did not invalidate session")
	}
	if _, ok := srv.oidcTokens.get(sess); ok {
		t.Fatal("401 did not cleanse vault entry")
	}
}

func TestFleetAmbiguityLatchSurvivesSessionReloadAndBlocksRedispatch(t *testing.T) {
	upstreamCalls := 0
	kb := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		upstreamCalls++
		writeJSON(w, http.StatusBadGateway, map[string]string{"error": "indeterminate"})
	}))
	defer kb.Close()
	srv := newTestServer(t, kb.URL)
	exp := time.Now().Add(time.Hour)
	sess, err := srv.sessions.create(&principal{iss: "https://idp", sub: "alice", expires: exp}, false)
	if err != nil {
		t.Fatal(err)
	}
	if err := srv.oidcTokens.put(sess.id, sess.iss, sess.sub, exp, "signed-oidc-token"); err != nil {
		t.Fatal(err)
	}
	dispatch := func() *httptest.ResponseRecorder {
		r := httptest.NewRequest(http.MethodPost, "/api/v1/servers/server-1/actions?team=9",
			strings.NewReader(`{"action":"agent.enable","agent":"agent.one"}`))
		r.Header.Set("Content-Type", "application/json")
		r.Header.Set("X-CSRF-Token", sess.csrf)
		r.AddCookie(&http.Cookie{Name: sessionCookie, Value: sess.id})
		w := httptest.NewRecorder()
		srv.handleAPI(w, r)
		return w
	}
	if w := dispatch(); w.Code != http.StatusBadGateway {
		t.Fatalf("first action status = %d", w.Code)
	}
	reloaded, err := srv.sessions.get(sess.id)
	if err != nil || !reloaded.fleetIndeterminate {
		t.Fatalf("ambiguity latch did not survive session reload: %+v err=%v", reloaded, err)
	}
	if w := dispatch(); w.Code != http.StatusConflict {
		t.Fatalf("redispatch status = %d, want 409", w.Code)
	}
	if upstreamCalls != 1 {
		t.Fatalf("ambiguous action reached upstream %d times, want 1", upstreamCalls)
	}
}

func TestFleetMutationClaimAllowsOnlyOneConcurrentDispatch(t *testing.T) {
	entered := make(chan struct{})
	release := make(chan struct{})
	var upstreamCalls atomic.Int32
	kb := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		if upstreamCalls.Add(1) == 1 {
			close(entered)
		}
		<-release
		writeJSON(w, http.StatusBadGateway, map[string]string{"error": "indeterminate"})
	}))
	defer kb.Close()
	srv := newTestServer(t, kb.URL)
	exp := time.Now().Add(time.Hour)
	sess, err := srv.sessions.create(&principal{iss: "https://idp", sub: "alice", expires: exp}, false)
	if err != nil {
		t.Fatal(err)
	}
	if err := srv.oidcTokens.put(sess.id, sess.iss, sess.sub, exp, "signed-oidc-token"); err != nil {
		t.Fatal(err)
	}
	dispatch := func(done chan<- int) {
		r := httptest.NewRequest(http.MethodPost, "/api/v1/servers/server-1/actions?team=9",
			strings.NewReader(`{"action":"agent.enable","agent":"agent.one"}`))
		r.Header.Set("Content-Type", "application/json")
		r.Header.Set("X-CSRF-Token", sess.csrf)
		r.AddCookie(&http.Cookie{Name: sessionCookie, Value: sess.id})
		w := httptest.NewRecorder()
		srv.handleAPI(w, r)
		done <- w.Code
	}
	done := make(chan int, 2)
	go dispatch(done)
	select {
	case <-entered:
	case <-time.After(2 * time.Second):
		t.Fatal("first mutation did not reach upstream")
	}
	go dispatch(done)
	select {
	case status := <-done:
		if status != http.StatusConflict {
			t.Fatalf("competing mutation status = %d, want 409", status)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("competing mutation did not fail promptly")
	}
	close(release)
	if status := <-done; status != http.StatusBadGateway {
		t.Fatalf("dispatched mutation status = %d, want 502", status)
	}
	if calls := upstreamCalls.Load(); calls != 1 {
		t.Fatalf("concurrent mutations reached upstream %d times, want 1", calls)
	}
}

func TestFleetMutationLatchClearsAfterDefiniteDenial(t *testing.T) {
	upstreamCalls := 0
	kb := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		upstreamCalls++
		writeJSON(w, http.StatusForbidden, map[string]string{"error": "policy denied"})
	}))
	defer kb.Close()
	srv := newTestServer(t, kb.URL)
	exp := time.Now().Add(time.Hour)
	sess, err := srv.sessions.create(&principal{iss: "https://idp", sub: "alice", expires: exp}, false)
	if err != nil {
		t.Fatal(err)
	}
	if err := srv.oidcTokens.put(sess.id, sess.iss, sess.sub, exp, "signed-oidc-token"); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 2; i++ {
		r := httptest.NewRequest(http.MethodPost, "/api/v1/servers/server-1/actions?team=9",
			strings.NewReader(`{"action":"agent.enable","agent":"agent.one"}`))
		r.Header.Set("Content-Type", "application/json")
		r.Header.Set("X-CSRF-Token", sess.csrf)
		r.AddCookie(&http.Cookie{Name: sessionCookie, Value: sess.id})
		w := httptest.NewRecorder()
		srv.handleAPI(w, r)
		if w.Code != http.StatusForbidden {
			t.Fatalf("definite denial %d status = %d", i, w.Code)
		}
		latched, err := srv.sessions.get(sess.id)
		if err != nil || !latched.fleetIndeterminate {
			t.Fatalf("definite response was not held for browser acknowledgement: %+v err=%v", latched, err)
		}
		ack := httptest.NewRequest(http.MethodPost, "/api/fleet/ack", nil)
		ack.Header.Set("X-CSRF-Token", sess.csrf)
		ack.AddCookie(&http.Cookie{Name: sessionCookie, Value: sess.id})
		ackW := httptest.NewRecorder()
		srv.handleFleetAck(ackW, ack)
		if ackW.Code != http.StatusOK {
			t.Fatalf("definite denial %d acknowledgement status = %d", i, ackW.Code)
		}
	}
	if upstreamCalls != 2 {
		t.Fatalf("definite denials reached upstream %d times, want 2", upstreamCalls)
	}
	reloaded, err := srv.sessions.get(sess.id)
	if err != nil || reloaded.fleetIndeterminate {
		t.Fatalf("definite response left mutation latch set: %+v err=%v", reloaded, err)
	}
}

// stubKB records the last request it saw and echoes a canned 200.
func stubKB(t *testing.T, seen *http.Request) *httptest.Server {
	t.Helper()
	return httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		*seen = *r
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(200)
		_, _ = io.WriteString(w, `{"ok":true}`)
	}))
}

func TestProxyDenyByDefault(t *testing.T) {
	var seen http.Request
	kb := stubKB(t, &seen)
	defer kb.Close()
	srv := newTestServer(t, kb.URL)
	sess, _ := srv.sessions.create(&principal{iss: "i", sub: "u"}, false)

	// A GET to a non-allowlisted route must be 403 and must NOT reach the kb.
	seen = http.Request{}
	r := httptest.NewRequest("GET", "/api/v1/memory", nil)
	r.AddCookie(&http.Cookie{Name: sessionCookie, Value: sess.id})
	w := httptest.NewRecorder()
	srv.handleAPI(w, r)
	if w.Code != http.StatusForbidden {
		t.Fatalf("expected 403 for non-allowlisted route, got %d", w.Code)
	}
	if seen.Method != "" {
		t.Fatalf("denied request must not reach the kb")
	}
}

func TestProxyRejectsEncodedPathBeforeUpstream(t *testing.T) {
	var seen http.Request
	kb := stubKB(t, &seen)
	defer kb.Close()
	srv := newTestServer(t, kb.URL)
	sess, _ := srv.sessions.create(&principal{iss: "i", sub: "u"}, false)
	r := httptest.NewRequest("GET", "/api/v1/%63onsole/overview", nil)
	r.AddCookie(&http.Cookie{Name: sessionCookie, Value: sess.id})
	w := httptest.NewRecorder()
	srv.handleAPI(w, r)
	if w.Code != http.StatusForbidden || seen.Method != "" {
		t.Fatalf("encoded path reached upstream: status=%d method=%q raw=%q", w.Code, seen.Method, r.URL.RawPath)
	}
}

func TestProxyForwardsAllowlisted(t *testing.T) {
	var seen http.Request
	kb := stubKB(t, &seen)
	defer kb.Close()
	srv := newTestServer(t, kb.URL)
	sess, _ := srv.sessions.create(&principal{iss: "i", sub: "u"}, false)

	r := httptest.NewRequest("GET", "/api/v1/console/overview", nil)
	r.AddCookie(&http.Cookie{Name: sessionCookie, Value: sess.id})
	w := httptest.NewRecorder()
	srv.handleAPI(w, r)
	if w.Code != 200 {
		t.Fatalf("expected 200 for allowlisted route, got %d", w.Code)
	}
	if seen.URL == nil || seen.URL.Path != "/v1/console/overview" {
		t.Fatalf("kb did not receive the remapped path; got %v", seen.URL)
	}
	if got := seen.Header.Get("Authorization"); got != "Bearer "+srv.kbBearer {
		t.Fatalf("kb must receive the console-admin bearer, got %q", got)
	}
}

func TestProxyCSRFRequiredOnMutation(t *testing.T) {
	var seen http.Request
	kb := stubKB(t, &seen)
	defer kb.Close()
	srv := newTestServer(t, kb.URL)
	sess, _ := srv.sessions.create(&principal{iss: "i", sub: "u"}, false)

	// POST without the CSRF header -> 403, no kb call.
	seen = http.Request{}
	r := httptest.NewRequest("POST", "/api/v1/enrollments/x/revoke", strings.NewReader("{}"))
	r.AddCookie(&http.Cookie{Name: sessionCookie, Value: sess.id})
	w := httptest.NewRecorder()
	srv.handleAPI(w, r)
	if w.Code != http.StatusForbidden {
		t.Fatalf("expected 403 without CSRF token, got %d", w.Code)
	}
	if seen.Method != "" {
		t.Fatalf("CSRF-rejected request must not reach the kb")
	}

	// With the correct CSRF token -> forwarded.
	r = httptest.NewRequest("POST", "/api/v1/enrollments/x/revoke", strings.NewReader("{}"))
	r.AddCookie(&http.Cookie{Name: sessionCookie, Value: sess.id})
	r.Header.Set("X-CSRF-Token", sess.csrf)
	w = httptest.NewRecorder()
	srv.handleAPI(w, r)
	if w.Code != 200 {
		t.Fatalf("expected 200 with valid CSRF token, got %d", w.Code)
	}
}

func TestNoSessionUnauthorized(t *testing.T) {
	srv := newTestServer(t, "http://127.0.0.1:1")
	r := httptest.NewRequest("GET", "/api/v1/console/overview", nil)
	w := httptest.NewRecorder()
	srv.handleAPI(w, r)
	if w.Code != http.StatusUnauthorized {
		t.Fatalf("expected 401 without a session, got %d", w.Code)
	}
}

func TestSessionBinding(t *testing.T) {
	srv := newTestServer(t, "http://127.0.0.1:1")
	a, _ := srv.sessions.create(&principal{iss: "idp-a", sub: "alice"}, false)
	got, err := srv.sessions.get(a.id)
	if err != nil || got.iss != "idp-a" || got.sub != "alice" {
		t.Fatalf("session not bound to (iss,sub): %+v err=%v", got, err)
	}
	// Revoking alice@idp-a invalidates her sessions but not a same-sub other-idp.
	b, _ := srv.sessions.create(&principal{iss: "idp-b", sub: "alice"}, false)
	srv.sessions.invalidateSub("idp-a", "alice")
	if _, err := srv.sessions.get(a.id); err == nil {
		t.Fatalf("invalidateSub should drop idp-a/alice")
	}
	if _, err := srv.sessions.get(b.id); err != nil {
		t.Fatalf("invalidateSub must not drop idp-b/alice (distinct issuer)")
	}
}

func TestBreakGlassOffByDefault(t *testing.T) {
	srv := newTestServer(t, "http://127.0.0.1:1")
	// No presence flag -> break-glass login rejected even with the right bearer.
	body := `{"break_glass_bearer":"` + srv.kbBearer + `"}`
	r := httptest.NewRequest("POST", "/api/login", strings.NewReader(body))
	w := httptest.NewRecorder()
	srv.handleLogin(w, r)
	if w.Code != http.StatusUnauthorized {
		t.Fatalf("break-glass must be off without the presence flag, got %d", w.Code)
	}
}

func TestOIDCOrigin(t *testing.T) {
	if got := oidcOrigin("https://idp.example.com/realms/x"); got != "https://idp.example.com" {
		t.Fatalf("oidcOrigin = %q", got)
	}
	if got := oidcOrigin("not a url"); got != "" {
		t.Fatalf("oidcOrigin(bad) = %q", got)
	}
}
