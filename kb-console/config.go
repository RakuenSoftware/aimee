package main

import (
	"encoding/json"
	"fmt"
	"os"
	"strings"
)

// config holds the console's runtime configuration. It is assembled from flags and
// environment, then validated (cred-file permissions, OIDC file presence) before
// the server starts. The console is default-off: it only runs when explicitly
// launched with a cred file, and binds to localhost unless told otherwise.
type config struct {
	addr        string // listen address (default 127.0.0.1:8744)
	kbBaseURL   string // aimee-kb /v1 base, e.g. https://aimee-kb:8741
	credFile    string // path to the console-admin bearer (mode 0600)
	consoleHome string // state dir: session db + break-glass flag live here
	spaPath     string // built console SPA (dist-console/index.html)
	certFile    string // TLS cert (auto-generated if empty)
	keyFile     string // TLS key
	oidc        oidcConfig
}

// oidcConfig is the read-only OIDC verifier config for S0 (loaded from a file).
// S2b replaces the source of truth with a DB2-backed, editable config; the shape
// is kept identical so the parity test against auth_oidc.c holds across both.
type oidcConfig struct {
	Issuer      string   `json:"issuer"`
	Audience    string   `json:"audience"`
	JWKSURL     string   `json:"jwks_url"`
	AdminClaim  string   `json:"admin_claim"`  // claim name, e.g. "groups" or "aimee_role"
	AdminValues []string `json:"admin_values"` // any of these values grants console access
}

// loadCred reads the console-admin bearer from credFile, refusing to run if the
// file is group/world-readable (0600 contract) — a stolen or over-shared cred is
// full console-admin access, so this is fail-closed.
func (c *config) loadCred() (string, error) {
	if os.Getenv("KB_CONSOLE_CRED") != "" {
		return "", fmt.Errorf("inline KB_CONSOLE_CRED env is disallowed; use KB_CONSOLE_CRED_FILE (mode 0600)")
	}
	if c.credFile == "" {
		return "", fmt.Errorf("no console-admin credential: set KB_CONSOLE_CRED_FILE")
	}
	fi, err := os.Stat(c.credFile)
	if err != nil {
		return "", fmt.Errorf("cred file: %w", err)
	}
	if fi.Mode().Perm()&0o077 != 0 {
		return "", fmt.Errorf("cred file %s is group/world-accessible (mode %o); require 0600", c.credFile, fi.Mode().Perm())
	}
	b, err := os.ReadFile(c.credFile)
	if err != nil {
		return "", fmt.Errorf("cred file: %w", err)
	}
	tok := strings.TrimSpace(string(b))
	if tok == "" {
		return "", fmt.Errorf("cred file %s is empty", c.credFile)
	}
	return tok, nil
}

// loadOIDC reads the read-only OIDC config file if present. A missing file is not
// an error: the console then serves break-glass-only (see auth.go), which is the
// documented recovery posture before OIDC is configured.
func (c *config) loadOIDC(path string) error {
	if path == "" {
		return nil
	}
	b, err := os.ReadFile(path)
	if err != nil {
		if os.IsNotExist(err) {
			return nil
		}
		return err
	}
	return json.Unmarshal(b, &c.oidc)
}

// oidcConfigured reports whether OIDC login is available (vs break-glass-only).
// Audience is required: without it, audience validation would be silently
// skipped and a token minted for any other relying party by the same IdP would
// be accepted. A half-configured OIDC block therefore stays break-glass-only.
func (c *config) oidcConfigured() bool {
	return c.oidc.Issuer != "" && c.oidc.JWKSURL != "" && c.oidc.AdminClaim != "" &&
		c.oidc.Audience != "" && len(c.oidc.AdminValues) > 0
}
