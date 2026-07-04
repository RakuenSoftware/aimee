// Command kb-console is the aimee-kb web console: a standalone Go thin-client that
// fronts a shared aimee-kb's /v1 surface directly, so a company KB is administrable
// with no colocated aimee-server. It mirrors aimee-webchat's shape (auto-TLS HTTPS,
// SQLite sessions, /api/* proxy) but uses NO PAM — login is OIDC (with a
// presence-flag break-glass) and it holds a scoped console-admin credential whose
// route allowlist the kb enforces server-side (see src/kb/http/kb_route_acl.c).
//
// Default-off: it only runs when explicitly launched with a console-admin cred
// file, and binds to localhost unless told otherwise.
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
	flag.StringVar(&cfg.credFile, "cred", os.Getenv("KB_CONSOLE_CRED_FILE"), "console-admin bearer file (mode 0600)")
	flag.StringVar(&cfg.consoleHome, "home", envOr("KB_CONSOLE_HOME", defaultHome()), "console state dir")
	flag.StringVar(&cfg.spaPath, "spa", "", "console SPA index.html (auto-discovered if empty)")
	flag.StringVar(&cfg.certFile, "cert", "", "TLS cert (auto-generated if empty)")
	flag.StringVar(&cfg.keyFile, "key", "", "TLS key")
	oidcFile := flag.String("oidc", os.Getenv("KB_CONSOLE_OIDC_FILE"), "read-only OIDC config JSON")
	insecureKB := flag.Bool("insecure-kb", false, "skip TLS verify to the kb (dev only)")
	flag.Parse()

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
		die("%v", err)
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

	kbClient := &http.Client{Timeout: 15 * time.Second}
	if *insecureKB {
		log.Printf("kb-console: WARNING -insecure-kb set — kb TLS verification disabled (dev only, never in production)")
		kbClient.Transport = &http.Transport{TLSClientConfig: &tls.Config{InsecureSkipVerify: true}}
	}

	// If no file/env configured OIDC, pull the DB2-backed config from the kb (S2b).
	if !cfg.oidcConfigured() {
		cfg.fetchOIDCFromKB(cfg.kbBaseURL, bearer, kbClient)
	}

	srv := &server{
		cfg: cfg, auth: newAuthenticator(cfg), sessions: sessions,
		kbBearer: bearer, kbClient: kbClient,
		logins: newRateLimiter(5, time.Minute),
	}
	srv.loadSPA()

	if !isLoopback(cfg.addr) {
		log.Printf("kb-console: WARNING binding non-loopback address %q — the OIDC login + console-admin proxy will be network-reachable", cfg.addr)
	}

	if err := srv.startupProbe(); err != nil {
		die("startup probe: %v", err)
	}
	if !cfg.oidcConfigured() {
		log.Printf("kb-console: WARNING oidc not configured — break-glass-only until OIDC is set")
	}

	certFile, keyFile, err := ensureTLS(cfg)
	if err != nil {
		die("tls: %v", err)
	}
	log.Printf("kb-console: listening https://%s (kb=%s)", cfg.addr, cfg.kbBaseURL)
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

func defaultHome() string {
	if h, err := os.UserConfigDir(); err == nil {
		return filepath.Join(h, "aimee", "kb-console")
	}
	return "./kb-console-home"
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
	fmt.Fprintf(os.Stderr, "kb-console: "+format+"\n", a...)
	os.Exit(1)
}
