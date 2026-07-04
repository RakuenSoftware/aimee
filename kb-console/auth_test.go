package main

import (
	"crypto/rand"
	"crypto/rsa"
	"encoding/base64"
	"encoding/json"
	"math/big"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/golang-jwt/jwt/v5"
)

// oidcHarness stands up a JWKS test server and mints signed tokens, so we can
// exercise verifyOIDC end to end (the "parity/security test" the docs claim).
type oidcHarness struct {
	key    *rsa.PrivateKey
	kid    string
	jwks   *httptest.Server
	issuer string
	aud    string
}

func newOIDCHarness(t *testing.T) *oidcHarness {
	t.Helper()
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatal(err)
	}
	h := &oidcHarness{key: key, kid: "test-kid", issuer: "https://idp.test", aud: "aimee-kb-console"}
	h.jwks = httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		n := base64.RawURLEncoding.EncodeToString(key.PublicKey.N.Bytes())
		e := base64.RawURLEncoding.EncodeToString(big.NewInt(int64(key.PublicKey.E)).Bytes())
		_ = json.NewEncoder(w).Encode(map[string]any{
			"keys": []map[string]string{{"kty": "RSA", "kid": h.kid, "n": n, "e": e}},
		})
	}))
	t.Cleanup(h.jwks.Close)
	return h
}

func (h *oidcHarness) authenticator() *authenticator {
	cfg := &config{oidc: oidcConfig{
		Issuer: h.issuer, Audience: h.aud, JWKSURL: h.jwks.URL,
		AdminClaim: "groups", AdminValues: []string{"aimee-admins"},
	}}
	a := newAuthenticator(cfg)
	a.jwks.client = h.jwks.Client() // trust the httptest self-signed cert
	return a
}

func (h *oidcHarness) sign(t *testing.T, method jwt.SigningMethod, claims jwt.MapClaims, setKid bool) string {
	t.Helper()
	tok := jwt.NewWithClaims(method, claims)
	if setKid {
		tok.Header["kid"] = h.kid
	}
	var (
		s   string
		err error
	)
	if method == jwt.SigningMethodHS256 {
		s, err = tok.SignedString([]byte("attacker-hmac-key"))
	} else {
		s, err = tok.SignedString(h.key)
	}
	if err != nil {
		t.Fatal(err)
	}
	return s
}

func (h *oidcHarness) goodClaims() jwt.MapClaims {
	return jwt.MapClaims{
		"iss": h.issuer, "aud": h.aud, "sub": "alice",
		"exp": time.Now().Add(time.Hour).Unix(), "nbf": time.Now().Add(-time.Minute).Unix(),
		"groups": []any{"aimee-admins"},
	}
}

func TestVerifyOIDC(t *testing.T) {
	h := newOIDCHarness(t)
	cases := []struct {
		name   string
		method jwt.SigningMethod
		mutate func(jwt.MapClaims)
		setKid bool
		wantOK bool
	}{
		{"valid", jwt.SigningMethodRS256, nil, true, true},
		{"alg-confusion-hs256", jwt.SigningMethodHS256, nil, true, false},
		{"wrong-issuer", jwt.SigningMethodRS256, func(c jwt.MapClaims) { c["iss"] = "https://evil" }, true, false},
		{"wrong-audience", jwt.SigningMethodRS256, func(c jwt.MapClaims) { c["aud"] = "other-rp" }, true, false},
		{"expired", jwt.SigningMethodRS256, func(c jwt.MapClaims) { c["exp"] = time.Now().Add(-time.Hour).Unix() }, true, false},
		{"missing-admin-claim", jwt.SigningMethodRS256, func(c jwt.MapClaims) { c["groups"] = []any{"users"} }, true, false},
		{"missing-sub", jwt.SigningMethodRS256, func(c jwt.MapClaims) { delete(c, "sub") }, true, false},
		{"missing-kid", jwt.SigningMethodRS256, nil, false, false},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			claims := h.goodClaims()
			if tc.mutate != nil {
				tc.mutate(claims)
			}
			raw := h.sign(t, tc.method, claims, tc.setKid)
			p, err := h.authenticator().verifyOIDC(raw)
			if tc.wantOK && (err != nil || p == nil || p.sub != "alice") {
				t.Fatalf("expected valid, got p=%v err=%v", p, err)
			}
			if !tc.wantOK && err == nil {
				t.Fatalf("expected rejection, got principal %v", p)
			}
		})
	}
}

func TestAdminClaimScalarAndArray(t *testing.T) {
	h := newOIDCHarness(t)
	a := h.authenticator()
	if !a.hasAdminClaim(jwt.MapClaims{"groups": "aimee-admins"}) {
		t.Fatal("scalar admin claim should match")
	}
	if !a.hasAdminClaim(jwt.MapClaims{"groups": []any{"x", "aimee-admins"}}) {
		t.Fatal("array admin claim should match")
	}
	if a.hasAdminClaim(jwt.MapClaims{"groups": []any{"users"}}) {
		t.Fatal("non-admin should not match")
	}
}
