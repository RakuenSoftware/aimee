// Command control-web is the aimee-kb Control Plane web console: a standalone Go
// thin-client that fronts a shared aimee-kb's /v1 surface directly, so a company KB
// is administrable with no colocated aimee-server. It mirrors aimee-runtime-web's
// shape (auto-TLS HTTPS, SQLite sessions, /api/* proxy) but uses NO PAM — login is
// OIDC (with a presence-flag break-glass) and it holds a scoped console-admin
// credential whose route allowlist the kb enforces through its event-bus
// control-web module.
//
// Optional module `control-web`: ships ENABLED and binds to localhost unless told
// otherwise. Turn it off for fully scripted deployments with AIMEE_CONTROL_WEB_ENABLED=0
// (or the -disabled flag); the container then idles without a listener.
package main

import (
	"crypto/tls"
	"flag"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"
)

func main() {
	cfg := &config{}
	flag.StringVar(&cfg.addr, "addr", "127.0.0.1:8744", "listen address (default localhost)")
	flag.StringVar(&cfg.kbBaseURL, "kb", envOr("AIMEE_KB_API_URL", "https://127.0.0.1:8741"), "aimee-kb base URL")
	flag.StringVar(&cfg.credFile, "cred", os.Getenv("CONTROL_WEB_CRED_FILE"), "console-admin bearer file (mode 0600)")
	flag.StringVar(&cfg.consoleHome, "home", envOr("CONTROL_WEB_HOME", defaultHome()), "console state dir")
	flag.StringVar(&cfg.spaPath, "spa", "", "console SPA index.html (auto-discovered if empty)")
	flag.StringVar(&cfg.certFile, "cert", "", "TLS cert (auto-generated if empty)")
	flag.StringVar(&cfg.keyFile, "key", "", "TLS key")
	oidcFile := flag.String("oidc", os.Getenv("CONTROL_WEB_OIDC_FILE"), "read-only OIDC config JSON")
	insecureKB := flag.Bool("insecure-kb", false, "skip TLS verify to the kb (dev only)")
	disabled := flag.Bool("disabled", false, "start disabled: log and idle without a listener (also AIMEE_CONTROL_WEB_ENABLED=0)")
	flag.Parse()

	// Optional-module gate. control-web ships enabled; an operator turns it off for a
	// fully scripted / headless deployment via the -disabled flag or
	// AIMEE_CONTROL_WEB_ENABLED in {0,false,no,off}. When off we idle rather than exit
	// so the container stays healthy (no restart loop) but binds no listener.
	if *disabled || !controlWebEnabled() {
		log.Printf("control-web: disabled by operator (AIMEE_CONTROL_WEB_ENABLED=0 / -disabled); no listener")
		select {}
	}

	if cfg.spaPath == "" {
		cfg.spaPath = discoverSPA()
	}
	if err := os.MkdirAll(cfg.consoleHome, 0o700); err != nil {
		die("console home: %v", err)
	}
	if err := cfg.loadOIDC(*oidcFile); err != nil {
		die("oidc config: %v", err)
	}
	bearer, err := cfg.loadCred()
	if err != nil {
		// control-web ships enabled, but a scoped console-admin cred is a deployment
		// input the kb must provision. Without it, idle (healthy, no listener) with a
		// clear message rather than crash-loop a default-on container. Operators who
		// do not want the console at all set AIMEE_CONTROL_WEB_ENABLED=0.
		log.Printf("control-web: enabled but no console-admin credential (%v); idling — "+
			"provide CONTROL_WEB_CRED_FILE or set AIMEE_CONTROL_WEB_ENABLED=0", err)
		select {}
	}

	sessDB := filepath.Join(cfg.consoleHome, "sessions.db")
	sessions, err := openSessionStore(sessDB)
	if err != nil {
		die("session store: %v", err)
	}
	// The session DB holds live session tokens: enforce 0600, fail-closed.
	if err := os.Chmod(sessDB, 0o600); err != nil {
		die("session db chmod: %v", err)
	}
	if fi, err := os.Stat(sessDB); err != nil || fi.Mode().Perm()&0o077 != 0 {
		die("session db %s is not 0600", sessDB)
	}
	oidcTokens := newCredentialVault(maxOIDCCredentials)
	sessions.vault = oidcTokens

	kbClient := &http.Client{Timeout: 15 * time.Second}
	if *insecureKB {
		log.Printf("control-web: WARNING -insecure-kb set — kb TLS verification disabled (dev only, never in production)")
		kbClient.Transport = &http.Transport{TLSClientConfig: &tls.Config{InsecureSkipVerify: true}}
	}

	// If no file/env configured OIDC, pull the DB2-backed config from the kb (S2b).
	if !cfg.oidcConfigured() {
		cfg.fetchOIDCFromKB(cfg.kbBaseURL, bearer, kbClient)
	}

	srv := &server{
		cfg: cfg, auth: newAuthenticator(cfg), sessions: sessions,
		kbBearer: bearer, kbClient: kbClient,
		oidcTokens:       oidcTokens,
		fleetOIDCEnabled: cfg.fleetOIDCAligned(cfg.kbBaseURL, bearer, kbClient),
		logins:           newRateLimiter(5, time.Minute),
	}
	srv.loadSPA()

	if !isLoopback(cfg.addr) {
		log.Printf("control-web: WARNING binding non-loopback address %q — the OIDC login + console-admin proxy will be network-reachable", cfg.addr)
	}

	if err := srv.startupProbe(); err != nil {
		die("startup probe: %v", err)
	}
	if !cfg.oidcConfigured() {
		log.Printf("control-web: WARNING oidc not configured — break-glass-only until OIDC is set")
	}
	if cfg.oidcConfigured() && !srv.fleetOIDCEnabled {
		log.Printf("control-web: WARNING OIDC issuer/audience differs from kb; fleet proxy disabled")
	}

	certFile, keyFile, err := ensureTLS(cfg)
	if err != nil {
		die("tls: %v", err)
	}
	log.Printf("control-web: listening https://%s (kb=%s)", cfg.addr, cfg.kbBaseURL)
	httpSrv := &http.Server{
		Addr:              cfg.addr,
		Handler:           srv.routes(),
		ReadHeaderTimeout: 10 * time.Second,
	}
	if err := httpSrv.ListenAndServeTLS(certFile, keyFile); err != nil {
		die("serve: %v", err)
	}
}

// startupProbe confirms the console-admin cred reaches its own allowlisted route
// on the kb (cred + ACL + route all wired). Fail-fast otherwise.
func (s *server) startupProbe() error {
	req, _ := http.NewRequest("GET", strings.TrimRight(s.cfg.kbBaseURL, "/")+"/v1/console/overview", nil)
	req.Header.Set("Authorization", "Bearer "+s.kbBearer)
	resp, err := s.kbClient.Do(req)
	if err != nil {
		return fmt.Errorf("kb unreachable at %s: %w", s.cfg.kbBaseURL, err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("console-admin cred failed on /v1/console/overview: status %d", resp.StatusCode)
	}
	return nil
}

// isLoopback reports whether a listen address binds only the loopback interface.
func isLoopback(addr string) bool {
	host, _, err := net.SplitHostPort(addr)
	if err != nil {
		host = addr
	}
	if host == "" || host == "localhost" {
		return true
	}
	ip := net.ParseIP(host)
	return ip != nil && ip.IsLoopback()
}

func envOr(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}

// controlWebEnabled reports the module toggle: enabled unless AIMEE_CONTROL_WEB_ENABLED
// is explicitly falsy (0/false/no/off, any case). Unset/empty leaves it ON — control-web
// is a first-class surface that ships enabled and must be explicitly disabled. Mirrors
// aimee-runtime-web's webchat_is_enabled entrypoint gate.
func controlWebEnabled() bool {
	switch strings.ToLower(strings.TrimSpace(os.Getenv("AIMEE_CONTROL_WEB_ENABLED"))) {
	case "0", "false", "no", "off":
		return false
	default:
		return true
	}
}

func defaultHome() string {
	if h, err := os.UserConfigDir(); err == nil {
		return filepath.Join(h, "aimee", "control-web")
	}
	return "./control-web-home"
}

// discoverSPA finds the built console SPA relative to the binary.
func discoverSPA() string {
	exe, err := os.Executable()
	if err != nil {
		return ""
	}
	dir := filepath.Dir(exe)
	for _, cand := range []string{
		filepath.Join(dir, "frontend", "dist-console", "console.html"),
		filepath.Join(dir, "..", "frontend", "dist-console", "console.html"),
		filepath.Join(dir, "..", "..", "frontend", "dist-console", "console.html"),
	} {
		if fileExists(cand) {
			return cand
		}
	}
	return ""
}

func die(format string, a ...any) {
	fmt.Fprintf(os.Stderr, "control-web: "+format+"\n", a...)
	os.Exit(1)
}
