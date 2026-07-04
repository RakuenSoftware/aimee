package main

import (
	"io"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
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
	return &server{
		cfg:      cfg,
		auth:     newAuthenticator(cfg),
		sessions: ss,
		kbBearer: "scope:console-admin:c1:secret",
		kbClient: &http.Client{},
		logins:   newRateLimiter(5, time.Minute),
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
