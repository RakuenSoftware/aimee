package main

import (
	"crypto/rsa"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math/big"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"sync"
	"time"

	"github.com/golang-jwt/jwt/v5"
)

// principal is the verified identity behind a console session.
type principal struct {
	iss           string
	sub           string
	viaBreakGlass bool
}

// jwksCache fetches and caches the operator-configured JWKS keyed by kid.
type jwksCache struct {
	mu       sync.Mutex
	url      string
	keys     map[string]*rsa.PublicKey
	fetched  time.Time
	ttl      time.Duration
	maxStale time.Duration
	client   *http.Client // nil = build the hardened default per fetch (tests inject)
}

func newJWKSCache(url string) *jwksCache {
	return &jwksCache{url: url, keys: map[string]*rsa.PublicKey{}, ttl: 5 * time.Minute, maxStale: time.Hour}
}

// key returns the RSA public key for kid, refreshing on a miss or when stale. On
// a refresh failure it may serve a still-cached key, but only within maxStale —
// so a rotated-out (e.g. compromised) key is not accepted indefinitely when the
// JWKS endpoint is unreachable; past maxStale it hard-fails.
func (j *jwksCache) key(kid string) (*rsa.PublicKey, error) {
	j.mu.Lock()
	defer j.mu.Unlock()
	if k, ok := j.keys[kid]; ok && time.Since(j.fetched) < j.ttl {
		return k, nil
	}
	if err := j.refresh(); err != nil {
		if k, ok := j.keys[kid]; ok && time.Since(j.fetched) < j.maxStale {
			return k, nil
		}
		return nil, err
	}
	k, ok := j.keys[kid]
	if !ok {
		return nil, fmt.Errorf("no JWKS key for kid %q", kid)
	}
	return k, nil
}

func (j *jwksCache) refresh() error {
	// HTTPS-only, no redirects, bounded body — the basic SSRF/rebind defenses.
	// S2b adds the full private-range denial + DNS-rebind re-check when the URL
	// becomes operator-editable via /v1/config/oidc.
	if u, err := url.Parse(j.url); err != nil || u.Scheme != "https" || u.Host == "" {
		return errors.New("jwks url must be an absolute https URL")
	}
	client := j.client
	if client == nil {
		client = &http.Client{
			Timeout: 5 * time.Second,
			CheckRedirect: func(*http.Request, []*http.Request) error {
				return errors.New("jwks: redirects not allowed")
			},
		}
	}
	resp, err := client.Get(j.url)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return fmt.Errorf("jwks fetch: status %d", resp.StatusCode)
	}
	var doc struct {
		Keys []struct {
			Kid string `json:"kid"`
			Kty string `json:"kty"`
			N   string `json:"n"`
			E   string `json:"e"`
		} `json:"keys"`
	}
	if err := json.NewDecoder(io.LimitReader(resp.Body, 1<<20)).Decode(&doc); err != nil {
		return err
	}
	next := map[string]*rsa.PublicKey{}
	for _, k := range doc.Keys {
		if k.Kty != "RSA" {
			continue
		}
		pk, err := rsaKeyFromNE(k.N, k.E)
		if err != nil {
			continue
		}
		next[k.Kid] = pk
	}
	if len(next) == 0 {
		return errors.New("jwks: no usable RSA keys")
	}
	j.keys = next
	j.fetched = time.Now()
	return nil
}

func rsaKeyFromNE(nB64, eB64 string) (*rsa.PublicKey, error) {
	nb, err := base64.RawURLEncoding.DecodeString(nB64)
	if err != nil {
		return nil, err
	}
	eb, err := base64.RawURLEncoding.DecodeString(eB64)
	if err != nil {
		return nil, err
	}
	n := new(big.Int).SetBytes(nb)
	e := 0
	for _, b := range eb {
		e = e<<8 | int(b)
	}
	if e == 0 {
		return nil, errors.New("bad exponent")
	}
	return &rsa.PublicKey{N: n, E: e}, nil
}

// authenticator verifies OIDC JWTs and enforces the admin-claim mapping. It is a
// stdlib + golang-jwt port of src/kb/auth_oidc.c — RS256 only, iss/aud/exp/nbf
// checked, claims mapped to admin access. A parity test locks the mapping.
type authenticator struct {
	cfg  *config
	jwks *jwksCache
}

func newAuthenticator(cfg *config) *authenticator {
	a := &authenticator{cfg: cfg}
	if cfg.oidcConfigured() {
		a.jwks = newJWKSCache(cfg.oidc.JWKSURL)
	}
	return a
}

// verifyOIDC verifies a bearer JWT and returns the principal iff it carries the
// pinned admin claim. RS256 only (reject none/HS — alg-confusion defense).
func (a *authenticator) verifyOIDC(raw string) (*principal, error) {
	if a.jwks == nil {
		return nil, errors.New("oidc not configured")
	}
	opts := []jwt.ParserOption{
		jwt.WithValidMethods([]string{"RS256"}),
		jwt.WithIssuer(a.cfg.oidc.Issuer),
		jwt.WithExpirationRequired(),
	}
	if a.cfg.oidc.Audience != "" {
		opts = append(opts, jwt.WithAudience(a.cfg.oidc.Audience))
	}
	parser := jwt.NewParser(opts...)
	claims := jwt.MapClaims{}
	_, err := parser.ParseWithClaims(raw, claims, func(t *jwt.Token) (interface{}, error) {
		kid, _ := t.Header["kid"].(string)
		if kid == "" {
			return nil, errors.New("missing kid")
		}
		return a.jwks.key(kid)
	})
	if err != nil {
		return nil, err
	}
	if !a.hasAdminClaim(claims) {
		return nil, errors.New("missing admin claim")
	}
	iss, _ := claims["iss"].(string)
	sub, _ := claims["sub"].(string)
	if sub == "" {
		return nil, errors.New("missing sub")
	}
	return &principal{iss: iss, sub: sub}, nil
}

// hasAdminClaim reports whether claims[AdminClaim] contains any configured admin
// value. Handles both a scalar-string claim and an array-of-strings claim.
func (a *authenticator) hasAdminClaim(claims jwt.MapClaims) bool {
	want := map[string]bool{}
	for _, v := range a.cfg.oidc.AdminValues {
		want[v] = true
	}
	if len(want) == 0 {
		return false
	}
	raw, ok := claims[a.cfg.oidc.AdminClaim]
	if !ok {
		return false
	}
	switch v := raw.(type) {
	case string:
		return want[v]
	case []interface{}:
		for _, item := range v {
			if s, ok := item.(string); ok && want[s] {
				return true
			}
		}
	}
	return false
}

// breakGlassPath is the presence-flag file that enables break-glass login.
func breakGlassPath(home string) string { return filepath.Join(home, ".break_glass") }

// breakGlassEnabled reports whether the operator has dropped the presence flag.
// Break-glass is OFF unless this file exists AND is a regular file with mode 0600
// (no group/world bits) — matching the cred-file contract, so a world-readable
// flag does not silently open the recovery path.
func breakGlassEnabled(home string) bool {
	fi, err := os.Stat(breakGlassPath(home))
	if err != nil || fi.IsDir() {
		return false
	}
	return fi.Mode().Perm()&0o077 == 0
}
