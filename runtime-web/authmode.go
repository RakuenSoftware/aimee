package main

import (
	"context"
	"encoding/json"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"
)

const (
	authModePAM  = "pam"
	authModeOIDC = "oidc"
)

// authModeTTL keeps the wizard responsive without re-asking the kb on every
// request. Short, because an operator who has just pointed the appliance at an
// OIDC kb should not have to wait for a long cache to expire before the account
// step disappears.
const authModeTTL = 30 * time.Second

// authModeResolver reports which login flow this appliance offers.
//
// The kb owns the answer: GET /v1/identity/auth-mode returns "oidc" when an
// issuer is configured and "pam" otherwise. Before a kb is connected there is no
// issuer to ask about, so the answer is PAM — which is what makes the wizard's
// first login possible at all.
//
// Every failure resolves to PAM. A kb that is unreachable, slow, or serving
// nonsense must not strand the operator on a login flow nobody can complete;
// PAM is the mode that always has a local answer. The kb makes the same choice
// for a configured-but-broken OIDC profile.
type authModeResolver struct {
	fetch func(ctx context.Context) (string, error)

	mu       sync.Mutex
	cached   string
	cachedAt time.Time
	now      func() time.Time
}

func newAuthModeResolver() *authModeResolver {
	return &authModeResolver{fetch: fetchKBAuthMode, now: time.Now}
}

func (r *authModeResolver) Mode(ctx context.Context) string {
	// Nil-safe: this gate sits in front of account management, and a caller that
	// holds no resolver must fall back to the baseline rather than panic a
	// request on its way to a decision.
	if r == nil {
		return authModePAM
	}
	if r.now == nil {
		r.now = time.Now
	}
	r.mu.Lock()
	if r.cached != "" && r.now().Sub(r.cachedAt) < authModeTTL {
		mode := r.cached
		r.mu.Unlock()
		return mode
	}
	r.mu.Unlock()

	mode := authModePAM
	if r.fetch != nil {
		if got, err := r.fetch(ctx); err == nil && got == authModeOIDC {
			mode = authModeOIDC
		}
	}

	r.mu.Lock()
	r.cached, r.cachedAt = mode, r.now()
	r.mu.Unlock()
	return mode
}

// oidc reports whether the connected kb owns identity, in which case this
// appliance must not offer local account management at all.
func (r *authModeResolver) oidc(ctx context.Context) bool {
	return r != nil && r.Mode(ctx) == authModeOIDC
}

func kbBaseURL() string {
	return strings.TrimRight(strings.TrimSpace(os.Getenv("AIMEE_KB_API_URL")), "/")
}

func fetchKBAuthMode(ctx context.Context) (string, error) {
	base := kbBaseURL()
	if base == "" {
		// No kb connected yet — the wizard's own first-run state.
		return authModePAM, nil
	}
	ctx, cancel := context.WithTimeout(ctx, 3*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, base+"/v1/identity/auth-mode", nil)
	if err != nil {
		return "", err
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return authModePAM, nil
	}
	var payload struct {
		Mode string `json:"mode"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&payload); err != nil {
		return "", err
	}
	if payload.Mode == authModeOIDC {
		return authModeOIDC, nil
	}
	return authModePAM, nil
}

// handleAuthMode lets the wizard ask which flow it is in, so it can hide the
// "create your account" step under OIDC rather than offering an action the
// server will refuse.
func (s *server) handleAuthMode(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]string{"mode": s.authMode.Mode(r.Context())})
}

// requireLocalAccounts refuses local account management when the kb owns
// identity. Under OIDC the IdP is the only place an account can be created, and
// a local one would be a parallel way in that bypasses the IdP's policy — the
// same reasoning that makes the kb refuse password login when OIDC is set.
// Returns true when the request was refused.
func (s *server) requireLocalAccounts(w http.ResponseWriter, r *http.Request) bool {
	if !s.authMode.oidc(r.Context()) {
		return false
	}
	writeJSONError(w, http.StatusConflict,
		"this appliance uses OIDC; accounts are managed by the identity provider")
	return true
}
