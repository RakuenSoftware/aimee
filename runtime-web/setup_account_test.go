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
	users      map[string]string
	locked     map[string]bool
	lockErr    error
	beforeLock func(username string)
}

const generatedBootstrapUsername = "aimee-012345abcdef"

func writeGeneratedBootstrapCredentials(t *testing.T, s *server, username string) []byte {
	t.Helper()
	t.Setenv("AIMEE_WEBCHAT_USER", "")
	t.Setenv("AIMEE_WEBCHAT_PASSWORD", "")
	if err := os.MkdirAll(s.setupAccountDir(), 0o700); err != nil {
		t.Fatal(err)
	}
	data := []byte("username=" + username + "\npassword=" + strings.Repeat("a", 64) + "\n")
	if err := os.WriteFile(s.setupAccountCredentials(), data, 0o600); err != nil {
		t.Fatal(err)
	}
	return data
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
	if f.beforeLock != nil {
		f.beforeLock(username)
	}
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
	dbPath := filepath.Join(t.TempDir(), "webchat.db")
	bootstrapDir := filepath.Join(filepath.Dir(dbPath), "webchat")
	if err := os.MkdirAll(bootstrapDir, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(bootstrapDir, "bootstrap-user"),
		[]byte("generated:"+defaultBootstrapUsername+"\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	db, err := openDB(dbPath)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	fake := &fakeSetupAccounts{
		users:  map[string]string{defaultBootstrapUsername: "$test$bootstrap"},
		locked: map[string]bool{},
	}
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/vault/set_server", func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			Agent  string `json:"agent"`
			Cred   string `json:"cred"`
			Secret string `json:"secret"`
		}
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil ||
			body.Agent != "webchat-login" || body.Cred != "replacement_hash" {
			http.Error(w, `{"error":"bad vault write"}`, http.StatusBadRequest)
			return
		}
		w.Write([]byte(`{"status":"ok"}`))
	})
	cfg := startFakeV1(t, mux)
	cfg.port = 8443
	cfg.dbPath = dbPath
	s := &server{
		cfg:      cfg,
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
	if err != nil || !strings.Contains(string(stored), "aimee:$test$bootstrap") ||
		strings.Contains(string(stored), "virant:$test$correct horse") {
		t.Fatalf("legacy login store was used for new verifier persistence: %q err=%v", stored, err)
	}
	if fake.users["virant"] != "$test$correct horse" {
		t.Fatalf("replacement was not committed through the Vault account abstraction")
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

func TestSetupAccountReplacesGeneratedBootstrapLogin(t *testing.T) {
	s, fake := newSetupAccountTestServer(t)
	writeGeneratedBootstrapCredentials(t, s, generatedBootstrapUsername)
	fake.beforeLock = func(username string) {
		if username != generatedBootstrapUsername {
			t.Fatalf("locking %q, want generated bootstrap", username)
		}
		if _, err := os.Stat(s.setupAccountCredentials()); err != nil {
			t.Fatalf("credentials removed before bootstrap lock: %v", err)
		}
	}
	fake.users[generatedBootstrapUsername] = "$test$generated"
	if err := os.WriteFile(s.setupAccountStore(), []byte(
		generatedBootstrapUsername+":$test$generated\nother:$test$other\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	oldToken, err := s.sessions.CreateSession(generatedBootstrapUsername)
	if err != nil {
		t.Fatal(err)
	}

	status := s.setupAccountStatus(generatedBootstrapUsername)
	if status["complete"] != false || status["required"] != true ||
		status["username"] != generatedBootstrapUsername {
		t.Fatalf("generated bootstrap status = %#v", status)
	}

	body := `{"username":"virant","password":"correct horse","password_confirmation":"correct horse"}`
	req := withUser(httptest.NewRequest(http.MethodPost, "/api/setup/account",
		strings.NewReader(body)), generatedBootstrapUsername)
	req.Header.Set("Sec-Fetch-Site", "same-origin")
	rr := httptest.NewRecorder()
	s.handleSetupAccount(rr, req)
	if rr.Code != http.StatusOK {
		t.Fatalf("replace generated account: code=%d body=%s", rr.Code, rr.Body.String())
	}
	if !fake.locked[generatedBootstrapUsername] {
		t.Fatalf("generated bootstrap was not locked: %#v", fake.locked)
	}
	if _, err := os.Stat(s.setupAccountCredentials()); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("plaintext generated credentials survived replacement: %v", err)
	}
	stored, err := os.ReadFile(s.setupAccountStore())
	if err != nil || !strings.Contains(string(stored), generatedBootstrapUsername+":") ||
		strings.Contains(string(stored), "virant:$test$correct horse") {
		t.Fatalf("legacy login store was modified: %q err=%v", stored, err)
	}
	if fake.users["virant"] != "$test$correct horse" {
		t.Fatalf("replacement was not committed through the Vault account abstraction")
	}
	if _, err := s.sessions.ValidateSession(oldToken); err == nil {
		t.Fatal("generated bootstrap session remains valid")
	}
}

func TestSetupAccountRollbackRestoresGeneratedCredentials(t *testing.T) {
	s, fake := newSetupAccountTestServer(t)
	originalCredentials := writeGeneratedBootstrapCredentials(t, s, generatedBootstrapUsername)
	fake.users[generatedBootstrapUsername] = "$test$generated"
	fake.lockErr = errors.New("lock failed")
	originalStore := []byte(generatedBootstrapUsername + ":$test$generated\n")
	if err := os.WriteFile(s.setupAccountStore(), originalStore, 0o600); err != nil {
		t.Fatal(err)
	}

	body := `{"username":"virant","password":"correct horse","password_confirmation":"correct horse"}`
	req := withUser(httptest.NewRequest(http.MethodPost, "/api/setup/account",
		strings.NewReader(body)), generatedBootstrapUsername)
	req.Header.Set("Sec-Fetch-Site", "same-origin")
	rr := httptest.NewRecorder()
	s.handleSetupAccount(rr, req)
	if rr.Code != http.StatusInternalServerError {
		t.Fatalf("code=%d body=%s", rr.Code, rr.Body.String())
	}
	credentials, err := os.ReadFile(s.setupAccountCredentials())
	if err != nil || string(credentials) != string(originalCredentials) {
		t.Fatalf("generated credentials=%q err=%v", credentials, err)
	}
	stored, err := os.ReadFile(s.setupAccountStore())
	if err != nil || string(stored) != string(originalStore) {
		t.Fatalf("login store=%q err=%v", stored, err)
	}
}

func TestReadGeneratedBootstrapUsernameRejectsMalformedFiles(t *testing.T) {
	path := filepath.Join(t.TempDir(), "bootstrap-credentials")
	for _, data := range []string{
		"username=aimee-short\npassword=" + strings.Repeat("a", 64) + "\n",
		"username=" + generatedBootstrapUsername + "\npassword=not-hex\n",
		"username=operator\npassword=" + strings.Repeat("a", 64) + "\n",
		"username=" + generatedBootstrapUsername + "\nusername=aimee-fedcba654321\npassword=" + strings.Repeat("a", 64) + "\n",
		"username=" + generatedBootstrapUsername + "\npassword=" + strings.Repeat("a", 64) + "\nunknown=value\n",
	} {
		if err := os.WriteFile(path, []byte(data), 0o600); err != nil {
			t.Fatal(err)
		}
		if username, ok := readGeneratedBootstrapUsername(path); ok {
			t.Fatalf("accepted malformed credential as %q: %q", username, data)
		}
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
		{"password below the minimum", "aimee", `{"username":"virant","password":"short","password_confirmation":"short"}`, http.StatusBadRequest},
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
	if err := os.MkdirAll(s.setupAccountDir(), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(s.setupAccountBootstrapUser(), []byte("explicit:operator\n"), 0o600); err != nil {
		t.Fatal(err)
	}
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

// The roundtable admin gate lives in the C server, which runs as `aimee`, while
// runtime-web runs as root. The replacement marker is how the two agree on who
// the appliance administrator is — so it has to be readable by the OTHER
// process. Written 0600 it was not, the gate fell through to its "admin"
// fallback, and it refused the very operator whose name was in the file.
//
// Reproduced on a live appliance: complete the wizard as `virant`, and
// POST /v1/roundtables/active returns 403; make the marker readable and the same
// request returns 200. Nothing in either process could catch that alone — both
// halves were correct in isolation.
func TestReplacementMarkerIsReadableByTheServerProcess(t *testing.T) {
	dir := t.TempDir()
	marker := filepath.Join(dir, "webchat", "bootstrap-replaced")

	rollback, err := writeReplacementMarker(marker, "virant")
	if err != nil {
		t.Fatalf("writeReplacementMarker: %v", err)
	}
	if rollback == nil {
		t.Fatal("no rollback returned")
	}

	fi, err := os.Stat(marker)
	if err != nil {
		t.Fatal(err)
	}
	if perm := fi.Mode().Perm(); perm&0o044 == 0 {
		t.Fatalf("marker mode %o is unreadable by the server process; the admin gate "+
			"cannot resolve the administrator and refuses them", perm)
	}
	// The directory has to be traversable for the same reason.
	di, err := os.Stat(filepath.Dir(marker))
	if err != nil {
		t.Fatal(err)
	}
	if perm := di.Mode().Perm(); perm&0o055 == 0 {
		t.Fatalf("marker dir mode %o blocks the server process", perm)
	}

	// It is identity metadata, not a credential: the content is a username.
	body, err := os.ReadFile(marker)
	if err != nil {
		t.Fatal(err)
	}
	if strings.TrimSpace(string(body)) != "virant" {
		t.Fatalf("marker content = %q; want the replacement username", string(body))
	}
}

// The minimum itself, from both sides. "Something short is rejected" does not say
// where the line is, so a change to setupAccountMinPassword can pass unnoticed;
// this fails until the number is updated deliberately.
//
// It gets its own server because a successful creation retires the bootstrap
// account, and every later case in a shared table then answers 409.
func TestSetupAccountPasswordMinimumBoundary(t *testing.T) {
	// Pin the NUMBER, not just the constant. Deriving both passwords from
	// setupAccountMinPassword makes the test self-consistent at any value, so it
	// would pass just as happily if the minimum drifted back to 8 and locked
	// operators out of six-character passwords again.
	if setupAccountMinPassword != 6 {
		t.Fatalf("the account minimum is 6 characters; got %d", setupAccountMinPassword)
	}
	const short = "abcde"  // 5
	const exact = "abcdef" // 6

	s, _ := newSetupAccountTestServer(t)
	body := `{"username":"virant","password":"` + short + `","password_confirmation":"` + short + `"}`
	req := withUser(httptest.NewRequest(http.MethodPost, "/api/setup/account", strings.NewReader(body)), "aimee")
	req.Header.Set("Sec-Fetch-Site", "same-origin")
	rr := httptest.NewRecorder()
	s.handleSetupAccount(rr, req)
	if rr.Code != http.StatusBadRequest {
		t.Fatalf("%d characters must be refused: code=%d body=%s", len(short), rr.Code, rr.Body.String())
	}

	s2, _ := newSetupAccountTestServer(t)
	body = `{"username":"virant","password":"` + exact + `","password_confirmation":"` + exact + `"}`
	req = withUser(httptest.NewRequest(http.MethodPost, "/api/setup/account", strings.NewReader(body)), "aimee")
	req.Header.Set("Sec-Fetch-Site", "same-origin")
	rr = httptest.NewRecorder()
	s2.handleSetupAccount(rr, req)
	if rr.Code != http.StatusOK {
		t.Fatalf("exactly %d characters must be accepted: code=%d body=%s", len(exact), rr.Code, rr.Body.String())
	}
}
