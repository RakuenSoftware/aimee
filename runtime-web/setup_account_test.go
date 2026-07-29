package main

import (
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/RakuenSoftware/smoothgui/auth"
)

type fakeSetupAccounts struct {
	users   map[string]string
	locked  map[string]bool
	lockErr error
}

func (f *fakeSetupAccounts) Exists(username string) bool {
	_, ok := f.users[username]
	return ok
}

func (f *fakeSetupAccounts) Create(username, password string) error {
	f.users[username] = "$test$" + password
	return nil
}

func (f *fakeSetupAccounts) Delete(username string) error {
	delete(f.users, username)
	return nil
}

func (f *fakeSetupAccounts) ShadowHash(username string) (string, error) {
	return f.users[username], nil
}

func (f *fakeSetupAccounts) Lock(username string) error {
	if f.lockErr != nil {
		return f.lockErr
	}
	f.locked[username] = true
	return nil
}

func TestSetupAccountRollsBackDurableStateWhenLockFails(t *testing.T) {
	s, fake := newSetupAccountTestServer(t)
	fake.lockErr = errors.New("lock failed")
	if err := os.MkdirAll(s.setupAccountDir(), 0o700); err != nil {
		t.Fatal(err)
	}
	original := []byte("aimee:$test$bootstrap\nother:$test$other\n")
	if err := os.WriteFile(s.setupAccountStore(), original, 0o600); err != nil {
		t.Fatal(err)
	}
	body := `{"username":"virant","password":"correct horse","password_confirmation":"correct horse"}`
	req := withUser(httptest.NewRequest(http.MethodPost, "/api/setup/account", strings.NewReader(body)), defaultBootstrapUsername)
	req.Header.Set("Sec-Fetch-Site", "same-origin")
	rr := httptest.NewRecorder()
	s.handleSetupAccount(rr, req)
	if rr.Code != http.StatusInternalServerError {
		t.Fatalf("code=%d body=%s", rr.Code, rr.Body.String())
	}
	if fake.Exists("virant") {
		t.Fatal("failed replacement user was not removed")
	}
	if _, err := os.Stat(s.setupAccountMarker()); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("retirement marker survived rollback: %v", err)
	}
	stored, err := os.ReadFile(s.setupAccountStore())
	if err != nil || string(stored) != string(original) {
		t.Fatalf("login store=%q err=%v", stored, err)
	}
}

func newSetupAccountTestServer(t *testing.T) (*server, *fakeSetupAccounts) {
	t.Helper()
	t.Setenv("AIMEE_WEBCHAT_USER", defaultBootstrapUsername)
	t.Setenv("AIMEE_WEBCHAT_PASSWORD", defaultBootstrapPassword)
	dbPath := filepath.Join(t.TempDir(), "webchat.db")
	db, err := openDB(dbPath)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	fake := &fakeSetupAccounts{
		users:  map[string]string{defaultBootstrapUsername: "$test$bootstrap"},
		locked: map[string]bool{},
	}
	s := &server{
		cfg:      &config{port: 8443, dbPath: dbPath},
		db:       db,
		sessions: auth.NewSessionStore(db, time.Hour),
		accounts: fake,
	}
	return s, fake
}

func TestSetupAccountReplacesAndPersistsBootstrapLogin(t *testing.T) {
	s, fake := newSetupAccountTestServer(t)
	if err := os.MkdirAll(s.setupAccountDir(), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(s.setupAccountStore(), []byte("aimee:$test$bootstrap\nother:$test$other\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	oldToken, err := s.sessions.CreateSession(defaultBootstrapUsername)
	if err != nil {
		t.Fatal(err)
	}

	body := `{"username":"virant","password":"correct horse","password_confirmation":"correct horse"}`
	req := withUser(httptest.NewRequest(http.MethodPost, "/api/setup/account", strings.NewReader(body)), defaultBootstrapUsername)
	req.Header.Set("Sec-Fetch-Site", "same-origin")
	rr := httptest.NewRecorder()
	s.handleSetupAccount(rr, req)
	if rr.Code != http.StatusOK {
		t.Fatalf("replace account: code=%d body=%s", rr.Code, rr.Body.String())
	}
	if !fake.Exists("virant") || !fake.locked[defaultBootstrapUsername] {
		t.Fatalf("replacement user/lock not applied: users=%v locked=%v", fake.users, fake.locked)
	}
	stored, err := os.ReadFile(s.setupAccountStore())
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(string(stored), "aimee:") || !strings.Contains(string(stored), "virant:$test$correct horse") ||
		!strings.Contains(string(stored), "other:$test$other") {
		t.Fatalf("unexpected durable login store: %q", stored)
	}
	marker, err := os.ReadFile(s.setupAccountMarker())
	if err != nil || strings.TrimSpace(string(marker)) != "virant" {
		t.Fatalf("marker=%q err=%v", marker, err)
	}
	if _, err := s.sessions.ValidateSession(oldToken); err == nil {
		t.Fatal("bootstrap session remains valid")
	}
	cookies := rr.Result().Cookies()
	if len(cookies) != 1 || cookies[0].Name != "session" || !cookies[0].Secure {
		t.Fatalf("replacement session cookie = %#v", cookies)
	}
	if username, err := s.sessions.ValidateSession(cookies[0].Value); err != nil || username != "virant" {
		t.Fatalf("replacement session username=%q err=%v", username, err)
	}

	statusReq := withUser(httptest.NewRequest(http.MethodGet, "/api/setup/account", nil), "virant")
	statusRR := httptest.NewRecorder()
	s.handleSetupAccount(statusRR, statusReq)
	var status map[string]any
	if err := json.Unmarshal(statusRR.Body.Bytes(), &status); err != nil {
		t.Fatal(err)
	}
	if status["complete"] != true || status["required"] != false || status["username"] != "virant" {
		t.Fatalf("status = %#v", status)
	}
}

func TestSetupAccountValidatesBootstrapCallerAndCredentials(t *testing.T) {
	s, _ := newSetupAccountTestServer(t)
	tests := []struct {
		name string
		user string
		body string
		code int
	}{
		{"bootstrap caller required", "someone", `{"username":"virant","password":"long enough","password_confirmation":"long enough"}`, http.StatusForbidden},
		{"new username required", "aimee", `{"username":"aimee","password":"long enough","password_confirmation":"long enough"}`, http.StatusBadRequest},
		{"password minimum", "aimee", `{"username":"virant","password":"short","password_confirmation":"short"}`, http.StatusBadRequest},
		{"password confirmation", "aimee", `{"username":"virant","password":"long enough","password_confirmation":"different"}`, http.StatusBadRequest},
		{"password newline", "aimee", "{\"username\":\"virant\",\"password\":\"long enough\\nroot:hijack\",\"password_confirmation\":\"long enough\\nroot:hijack\"}", http.StatusBadRequest},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			req := withUser(httptest.NewRequest(http.MethodPost, "/api/setup/account", strings.NewReader(tc.body)), tc.user)
			req.Header.Set("Sec-Fetch-Site", "same-origin")
			rr := httptest.NewRecorder()
			s.handleSetupAccount(rr, req)
			if rr.Code != tc.code {
				t.Fatalf("code=%d want=%d body=%s", rr.Code, tc.code, rr.Body.String())
			}
		})
	}
}

func TestSetupAccountRejectsCrossOriginMutation(t *testing.T) {
	s, _ := newSetupAccountTestServer(t)
	body := `{"username":"virant","password":"long enough","password_confirmation":"long enough"}`
	req := withUser(httptest.NewRequest(http.MethodPost, "/api/setup/account", strings.NewReader(body)), defaultBootstrapUsername)
	req.Header.Set("Origin", "https://evil.example")
	req.Host = "aimee.example"
	rr := httptest.NewRecorder()
	s.handleSetupAccount(rr, req)
	if rr.Code != http.StatusForbidden {
		t.Fatalf("code=%d body=%s", rr.Code, rr.Body.String())
	}
}

func TestSetupAccountIsCompleteForOperatorConfiguredLogin(t *testing.T) {
	s, _ := newSetupAccountTestServer(t)
	t.Setenv("AIMEE_WEBCHAT_USER", "operator")
	t.Setenv("AIMEE_WEBCHAT_PASSWORD", "not-the-published-default")
	rr := httptest.NewRecorder()
	s.handleSetupAccount(rr, withUser(httptest.NewRequest(http.MethodGet, "/api/setup/account", nil), "operator"))
	var status map[string]any
	if err := json.Unmarshal(rr.Body.Bytes(), &status); err != nil {
		t.Fatal(err)
	}
	if status["complete"] != true || status["required"] != false || status["username"] != "operator" {
		t.Fatalf("status = %#v", status)
	}
}
