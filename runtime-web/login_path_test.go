package main

import (
	"errors"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/RakuenSoftware/smoothgui/auth"
)

// stubIdentity stands in for pamAccounts so the WIRING is under test rather than
// PAM itself: whether a successful check actually yields a usable session, and
// whether a broken authenticator is distinguishable from a wrong password.
type stubIdentity struct {
	password string
	err      error
}

func (s stubIdentity) Authenticate(username, password string) (bool, error) {
	if s.err != nil {
		return false, s.err
	}
	return username == "virant" && password == s.password, nil
}
func (s stubIdentity) UpdatePassword(_, _, _ string) error { return nil }
func (s stubIdentity) List() ([]string, error)             { return []string{"virant"}, nil }

func newLoginTestServer(t *testing.T, identity webchatIdentityStore) *server {
	t.Helper()
	db, err := openDB(filepath.Join(t.TempDir(), "webchat.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { db.Close() })
	if err := applyChatSessionMigrations(db); err != nil {
		t.Fatal(err)
	}
	sessions, err := newSignedSessionStore(db, &fakeWebchatVault{}, time.Hour)
	if err != nil {
		t.Fatal(err)
	}
	return &server{
		cfg:      &config{dbPath: filepath.Join(t.TempDir(), "webchat.db")},
		db:       db,
		sessions: sessions,
		rl:       auth.NewRateLimiter(db, 10, 15*time.Minute),
		identity: identity,
		authMode: fixedResolver(authModePAM, nil),
	}
}

func login(t *testing.T, s *server, username, password string) *httptest.ResponseRecorder {
	t.Helper()
	body := `{"username":"` + username + `","password":"` + password + `"}`
	req := httptest.NewRequest(http.MethodPost, "/api/auth/login", strings.NewReader(body))
	rr := httptest.NewRecorder()
	s.handleAuthLogin(rr, req)
	return rr
}

// The whole login path, end to end: a correct credential must yield a session
// cookie that actually authenticates a subsequent request.
//
// Every piece of this was unit-tested in isolation and the seam between them
// still broke once — PAMAuthenticate re-executes the binary as __pam_auth and
// main() never dispatched it, so every real login would have failed while the
// identity tests stayed green. This is the test that would have caught it.
func TestLoginPathIssuesAUsableSession(t *testing.T) {
	s := newLoginTestServer(t, stubIdentity{password: "correct-horse"})

	rr := login(t, s, "virant", "correct-horse")
	if rr.Code != http.StatusOK {
		t.Fatalf("login code = %d, body = %q; want 200", rr.Code, rr.Body.String())
	}
	var session *http.Cookie
	for _, c := range rr.Result().Cookies() {
		if c.Name == "session" {
			session = c
		}
	}
	if session == nil || session.Value == "" {
		t.Fatal("login issued no session cookie")
	}
	if !session.HttpOnly || session.SameSite != http.SameSiteStrictMode {
		t.Fatalf("session cookie is not HttpOnly+SameSite=Strict: %+v", session)
	}

	// The cookie must authenticate a protected route, as the browser would use it.
	reached := false
	protected := s.requireAuth(func(w http.ResponseWriter, r *http.Request) {
		reached = true
		if got := currentUser(r); got != "virant" {
			t.Fatalf("currentUser = %q; want virant", got)
		}
	})
	req := httptest.NewRequest(http.MethodGet, "/api/chat/sessions", nil)
	req.AddCookie(session)
	rr2 := httptest.NewRecorder()
	protected(rr2, req)
	if !reached || rr2.Code == http.StatusUnauthorized {
		t.Fatalf("session did not authenticate: reached=%v code=%d", reached, rr2.Code)
	}
}

func TestLoginPathRejectsAWrongPassword(t *testing.T) {
	s := newLoginTestServer(t, stubIdentity{password: "correct-horse"})
	rr := login(t, s, "virant", "wrong")
	if rr.Code != http.StatusUnauthorized {
		t.Fatalf("code = %d; want 401", rr.Code)
	}
	for _, c := range rr.Result().Cookies() {
		if c.Name == "session" && c.Value != "" {
			t.Fatal("a failed login issued a session cookie")
		}
	}
}

// A broken PAM stack must not read as a wrong password. 503 tells an operator to
// look at the authenticator; 401 sends them hunting a credential problem that
// does not exist — which is how the appliance's identity bug stayed hidden.
func TestLoginPathDistinguishesBrokenAuthFromBadCredentials(t *testing.T) {
	s := newLoginTestServer(t, stubIdentity{err: errors.New("PAM is unavailable")})
	rr := login(t, s, "virant", "anything")
	if rr.Code != http.StatusServiceUnavailable {
		t.Fatalf("code = %d, body = %q; want 503", rr.Code, rr.Body.String())
	}
}

// A session must stop working once it is deleted, so logout genuinely ends it.
func TestLoginPathSessionIsRevocable(t *testing.T) {
	s := newLoginTestServer(t, stubIdentity{password: "correct-horse"})
	rr := login(t, s, "virant", "correct-horse")
	var token string
	for _, c := range rr.Result().Cookies() {
		if c.Name == "session" {
			token = c.Value
		}
	}
	if token == "" {
		t.Fatal("no session issued")
	}
	if _, err := s.sessions.ValidateSession(token); err != nil {
		t.Fatalf("fresh session did not validate: %v", err)
	}
	if err := s.sessions.DeleteSession(token); err != nil {
		t.Fatal(err)
	}
	if _, err := s.sessions.ValidateSession(token); err == nil {
		t.Fatal("a deleted session still validates")
	}
}
