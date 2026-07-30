package main

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func fixedResolver(mode string, err error) *authModeResolver {
	return &authModeResolver{
		fetch: func(context.Context) (string, error) { return mode, err },
		now:   time.Now,
	}
}

// PAM is the baseline, and every failure resolves to it. A kb that is
// unreachable or serving nonsense must not strand the operator on a login flow
// nobody can complete — PAM is the mode that always has a local answer.
func TestAuthModeFallsBackToPAM(t *testing.T) {
	for name, r := range map[string]*authModeResolver{
		"kb unreachable": fixedResolver("", errors.New("dial tcp: refused")),
		"kb reports pam": fixedResolver(authModePAM, nil),
		"kb nonsense":    fixedResolver("banana", nil),
	} {
		if got := r.Mode(context.Background()); got != authModePAM {
			t.Fatalf("%s: Mode() = %q; want pam", name, got)
		}
		if r.oidc(context.Background()) {
			t.Fatalf("%s: oidc() = true; want false", name)
		}
	}

	oidc := fixedResolver(authModeOIDC, nil)
	if !oidc.oidc(context.Background()) {
		t.Fatal("a kb reporting oidc must switch the appliance to oidc")
	}
}

// The resolver caches so the wizard is not re-querying the kb on every request,
// but the TTL has to actually expire or an operator who just connected an OIDC
// kb would keep seeing the local-account step.
func TestAuthModeCacheExpires(t *testing.T) {
	calls := 0
	now := time.Now()
	r := &authModeResolver{
		fetch: func(context.Context) (string, error) { calls++; return authModePAM, nil },
		now:   func() time.Time { return now },
	}
	r.Mode(context.Background())
	r.Mode(context.Background())
	if calls != 1 {
		t.Fatalf("calls = %d; want 1 (second read served from cache)", calls)
	}
	now = now.Add(authModeTTL + time.Second)
	r.Mode(context.Background())
	if calls != 2 {
		t.Fatalf("calls = %d; want 2 (cache must expire)", calls)
	}
}

// With OIDC the identity provider owns accounts. Creating a local one would be a
// parallel way in that bypasses the IdP's policy — the same reason the kb refuses
// password login when an issuer is configured.
func TestSetupAccountRefusedUnderOIDC(t *testing.T) {
	s := &server{authMode: fixedResolver(authModeOIDC, nil)}
	req := withUser(httptest.NewRequest(http.MethodPost, "/api/setup/account",
		strings.NewReader(`{"username":"someone","password":"hunter2hunter2"}`)), "virant")
	rr := httptest.NewRecorder()
	s.handleSetupAccount(rr, req)

	if rr.Code != http.StatusConflict {
		t.Fatalf("code = %d; want 409", rr.Code)
	}
	if !strings.Contains(rr.Body.String(), "identity provider") {
		t.Fatalf("body = %q; want it to name the identity provider", rr.Body.String())
	}
}

// And the wizard must not be told the step is required, so it disappears rather
// than offering an action the server will refuse.
func TestSetupAccountStatusIsNotRequiredUnderOIDC(t *testing.T) {
	s := &server{authMode: fixedResolver(authModeOIDC, nil)}
	rr := httptest.NewRecorder()
	s.handleSetupAccount(rr, withUser(httptest.NewRequest(http.MethodGet, "/api/setup/account", nil), "virant"))

	var got map[string]any
	if err := json.Unmarshal(rr.Body.Bytes(), &got); err != nil {
		t.Fatal(err)
	}
	if got["required"] != false || got["managed_by"] != authModeOIDC {
		t.Fatalf("status = %v; want required=false managed_by=oidc", got)
	}
}

func TestAuthModeEndpointReportsTheMode(t *testing.T) {
	s := &server{authMode: fixedResolver(authModeOIDC, nil)}
	rr := httptest.NewRecorder()
	s.handleAuthMode(rr, httptest.NewRequest(http.MethodGet, "/api/auth/mode", nil))
	if !strings.Contains(rr.Body.String(), `"mode":"oidc"`) {
		t.Fatalf("body = %q; want mode oidc", rr.Body.String())
	}
}
