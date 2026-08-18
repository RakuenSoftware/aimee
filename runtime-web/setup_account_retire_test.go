package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The state the testing appliance was actually found in: a generated bootstrap
// login recorded in bootstrap-user, real operator accounts alongside it, and NO
// replacement marker — because those accounts were created by some route other
// than this endpoint. bootstrap-credentials is absent, so the generated
// password is unrecoverable and nobody can ever sign in as that account.
//
//	/var/lib/aimee/webchat/
//	  bootstrap-user   generated:aimee-c78febb6b371
//	  identities       aimee-c78febb6b371, admin, laura
//	  bootstrap-replaced  (missing)
func newStrandedApplianceServer(t *testing.T) (*server, *fakeSetupAccounts) {
	t.Helper()
	s, fake := newSetupAccountTestServer(t)
	if err := os.WriteFile(filepath.Join(s.setupAccountDir(), "bootstrap-user"),
		[]byte("generated:"+generatedBootstrapUsername+"\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	delete(fake.users, defaultBootstrapUsername)
	fake.users[generatedBootstrapUsername] = "$test$generated"
	fake.users["laura"] = "$test$laura"
	return s, fake
}

func setupStatus(t *testing.T, s *server, caller string) map[string]any {
	t.Helper()
	req := withUser(httptest.NewRequest(http.MethodGet, "/api/setup/account", nil), caller)
	rr := httptest.NewRecorder()
	s.handleSetupAccount(rr, req)
	var out map[string]any
	if err := json.Unmarshal(rr.Body.Bytes(), &out); err != nil {
		t.Fatalf("status body %q: %v", rr.Body.String(), err)
	}
	return out
}

func retireRequest(caller, body string) *http.Request {
	req := withUser(httptest.NewRequest(http.MethodPost, "/api/setup/account",
		strings.NewReader(body)), caller)
	req.Header.Set("Sec-Fetch-Site", "same-origin")
	return req
}

// An operator signed in as their own account can close the generated login.
// Before this they could not: the endpoint demanded the bootstrap session, whose
// password no longer exists anywhere, so the wizard asked forever for a step
// with no reachable completion.
func TestEstablishedAccountRetiresStrandedBootstrapLogin(t *testing.T) {
	s, fake := newStrandedApplianceServer(t)

	status := setupStatus(t, s, "laura")
	if status["required"] != true || status["retire_only"] != true {
		t.Fatalf("a stranded appliance must offer retirement to laura: %#v", status)
	}

	rr := httptest.NewRecorder()
	s.handleSetupAccount(rr, retireRequest("laura", `{"retire_bootstrap":true}`))
	if rr.Code != http.StatusOK {
		t.Fatalf("retirement refused: code=%d body=%s", rr.Code, rr.Body.String())
	}
	if !fake.locked[generatedBootstrapUsername] {
		t.Fatal("the generated login is still a usable way in")
	}
	// laura's own account is untouched — retirement creates and deletes nothing.
	if _, ok := fake.users["laura"]; !ok {
		t.Fatal("the caller's own account was removed")
	}

	if status := setupStatus(t, s, "laura"); status["complete"] != true {
		t.Fatalf("the step must now be complete: %#v", status)
	}
	// The lockout this fixes: adminUsername() fell back to the pending bootstrap
	// user, so the administrator was an account nobody could authenticate as.
	if got := s.adminUsername(); got != "laura" {
		t.Fatalf("administrator = %q, want laura", got)
	}
}

// Retirement is opt-in. A POST that does not ask for it is refused exactly as
// before, so no other request can retire a login as a side effect.
func TestRetirementRequiresAnExplicitRequest(t *testing.T) {
	s, fake := newStrandedApplianceServer(t)
	for _, body := range []string{
		`{}`,
		`{"retire_bootstrap":false}`,
		`{"username":"laura2","password":"correct horse","password_confirmation":"correct horse"}`,
	} {
		rr := httptest.NewRecorder()
		s.handleSetupAccount(rr, retireRequest("laura", body))
		if rr.Code != http.StatusForbidden {
			t.Fatalf("body %s: code=%d body=%s", body, rr.Code, rr.Body.String())
		}
	}
	if fake.locked[generatedBootstrapUsername] {
		t.Fatal("a login was retired without being asked for")
	}
	if _, ok := fake.users["laura2"]; ok {
		t.Fatal("the retirement path created an account; it must create nothing")
	}
}

// Only an account that already exists may retire the login. Otherwise the gate
// would be "anyone requireAuth let through", and the injected username is the
// only thing distinguishing an operator from a stale or forged session.
func TestUnknownCallerCannotRetire(t *testing.T) {
	s, fake := newStrandedApplianceServer(t)
	for _, caller := range []string{"", "nobody"} {
		if s.retireOnly(caller, generatedBootstrapUsername) {
			t.Fatalf("caller %q must not be offered retirement", caller)
		}
		rr := httptest.NewRecorder()
		s.handleSetupAccount(rr, retireRequest(caller, `{"retire_bootstrap":true}`))
		if rr.Code != http.StatusForbidden {
			t.Fatalf("caller %q: code=%d body=%s", caller, rr.Code, rr.Body.String())
		}
	}
	if fake.locked[generatedBootstrapUsername] {
		t.Fatal("an unknown caller retired the bootstrap login")
	}
}

// The bootstrap account itself keeps the replacement flow: it may still trade
// its login for a real one, which retirement deliberately does not do.
func TestBootstrapSessionStillReplacesRatherThanRetires(t *testing.T) {
	s, fake := newStrandedApplianceServer(t)

	if status := setupStatus(t, s, generatedBootstrapUsername); status["retire_only"] != false {
		t.Fatalf("the bootstrap session must be offered replacement, not retirement: %#v", status)
	}
	rr := httptest.NewRecorder()
	s.handleSetupAccount(rr, retireRequest(generatedBootstrapUsername,
		`{"username":"virant","password":"correct horse","password_confirmation":"correct horse"}`))
	if rr.Code != http.StatusOK {
		t.Fatalf("replacement refused: code=%d body=%s", rr.Code, rr.Body.String())
	}
	if _, ok := fake.users["virant"]; !ok {
		t.Fatal("replacement did not create the new account")
	}
	if !fake.locked[generatedBootstrapUsername] {
		t.Fatal("replacement did not retire the bootstrap login")
	}
	if got := s.adminUsername(); got != "virant" {
		t.Fatalf("administrator = %q, want virant", got)
	}
}

// Once retired, the step stays complete: a second caller cannot re-run it and
// take the administrator slot from the operator who completed setup.
func TestRetirementIsNotRepeatable(t *testing.T) {
	s, _ := newStrandedApplianceServer(t)
	rr := httptest.NewRecorder()
	s.handleSetupAccount(rr, retireRequest("laura", `{"retire_bootstrap":true}`))
	if rr.Code != http.StatusOK {
		t.Fatalf("first retirement refused: code=%d body=%s", rr.Code, rr.Body.String())
	}
	rr = httptest.NewRecorder()
	s.handleSetupAccount(rr, retireRequest("admin", `{"retire_bootstrap":true}`))
	if rr.Code != http.StatusConflict {
		t.Fatalf("second retirement: code=%d body=%s", rr.Code, rr.Body.String())
	}
	if got := s.adminUsername(); got != "laura" {
		t.Fatalf("administrator = %q, want laura — the slot was taken after setup", got)
	}
}
