package main

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"database/sql"
	"encoding/pem"
	"fmt"
	"log"
	"math/big"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"

	_ "github.com/mattn/go-sqlite3"

	"github.com/RakuenSoftware/smoothgui/auth"
)

const pamService = "aimee-webchat"

type server struct {
	cfg      *config
	db       *sql.DB
	sessions *auth.SessionStore
	rl       *auth.RateLimiter
	users    *auth.UserManager
	authH    *auth.Handler
}

func run(cfg *config) error {
	if err := initSPA(cfg); err != nil {
		return fmt.Errorf("spa: %w", err)
	}

	db, err := openDB(cfg.dbPath)
	if err != nil {
		return fmt.Errorf("open db: %w", err)
	}

	if err := applyChannelMigrations(db); err != nil {
		return fmt.Errorf("channel migrations: %w", err)
	}

	if err := applyChatSessionMigrations(db); err != nil {
		return fmt.Errorf("chat session migrations: %w", err)
	}

	sessions := auth.NewSessionStore(db, 24*time.Hour)
	rl := auth.NewRateLimiter(db, 10, 15*time.Minute)
	users := auth.NewUserManager("aimee")
	authH := auth.NewHandler(pamService, sessions, rl, users)

	s := &server{
		cfg:      cfg,
		db:       db,
		sessions: sessions,
		rl:       rl,
		users:    users,
		authH:    authH,
	}

	mux := http.NewServeMux()
	s.registerRoutes(mux)

	addr := fmt.Sprintf(":%d", cfg.port)

	if !cfg.tlsEnabled() {
		log.Printf("aimee-webchat: http on %s", addr)
		return http.ListenAndServe(addr, mux)
	}

	cert, key, err := ensureTLSCert(cfg)
	if err != nil {
		return fmt.Errorf("tls: %w", err)
	}

	tlsCert, err := tls.LoadX509KeyPair(cert, key)
	if err != nil {
		return fmt.Errorf("load cert: %w", err)
	}

	tlsCfg := &tls.Config{
		Certificates: []tls.Certificate{tlsCert},
		MinVersion:   tls.VersionTLS12,
	}

	srv := &http.Server{
		Addr:      addr,
		Handler:   mux,
		TLSConfig: tlsCfg,
	}

	log.Printf("aimee-webchat: https on %s", addr)
	return srv.ListenAndServeTLS("", "")
}

func (s *server) registerRoutes(mux *http.ServeMux) {
	// Auth routes (no session required)
	mux.HandleFunc("/login", s.handleLogin)
	mux.HandleFunc("/logout", s.handleLogout)
	mux.HandleFunc("/api/auth/login", s.handleAuthLogin)
	mux.HandleFunc("/api/auth/logout", s.authH.Logout)
	mux.HandleFunc("/v1/model", s.handleOpenAIModels)
	mux.HandleFunc("/v1/models", s.handleOpenAIModels)
	// OpenAI inference surface, proxied to aimee-server's real /v1 endpoints
	// (streaming-capable). The webchat bearer gates access; the UDS hop is trusted.
	mux.HandleFunc("/v1/chat/completions", s.handleOpenAIChatCompletions)
	mux.HandleFunc("/v1/completions", s.openAIProxyHandler("/v1/completions"))
	mux.HandleFunc("/v1/responses", s.openAIProxyHandler("/v1/responses"))
	mux.HandleFunc("/v1/embeddings", s.openAIProxyHandler("/v1/embeddings"))

	// SPA pages (session required → redirect to login)
	mux.HandleFunc("/", s.requireAuth(s.handleRoot))
	mux.HandleFunc("/chat", s.requireAuth(s.handleSPA))
	mux.HandleFunc("/dashboard", s.requireAuth(s.handleSPA))
	mux.HandleFunc("/workflows", s.requireAuth(s.handleSPA))
	mux.HandleFunc("/projects", s.requireAuth(s.handleSPA))
	// The Editor tab is a SPA page that embeds the in-app VSCode in an iframe, so
	// the nav shell stays visible. The iframe's src is the /vscode proxy below.
	mux.HandleFunc("/editor", s.requireAuth(s.handleSPA))
	// In-app VSCode (code-server) reverse-proxy (WP-J): the iframe document and
	// all its assets/WebSocket traffic go through /vscode -> aimee-server's
	// per-user editor port. Not an /api path, so requireAuth redirects to /login
	// on an expired session (correct for the iframe).
	mux.HandleFunc("/vscode", s.requireAuth(s.handleVSCode))
	mux.HandleFunc("/vscode/", s.requireAuth(s.handleVSCode))

	// Chat API (session required)
	mux.HandleFunc("/api/chat/send", s.requireAuth(s.handleChatSend))
	mux.HandleFunc("/api/chat/session", s.requireAuth(s.handleChatSession))
	mux.HandleFunc("/api/chat/projects", s.requireAuth(s.handleChatProjects))
	mux.HandleFunc("/api/chat/bootstrap-status", s.requireAuth(s.handleBootstrapStatus))
	mux.HandleFunc("/api/chat/init-rules", s.requireAuth(s.handleChatInitRules))
	mux.HandleFunc("/api/chat/skills", s.requireAuth(s.handleChatSkills))
	mux.HandleFunc("/api/chat/skill", s.requireAuth(s.handleChatSkill))
	mux.HandleFunc("/api/chat/personas", s.requireAuth(s.handleChatPersonas))
	mux.HandleFunc("/api/chat/personas/", s.requireAuth(s.handleChatPersonaItem))
	mux.HandleFunc("/api/chat/persona", s.requireAuth(s.handleChatPersona))
	mux.HandleFunc("/api/chat/attach", s.requireAuth(s.handleChatAttach))
	mux.HandleFunc("/api/chat/detach", s.requireAuth(s.handleChatDetach))
	mux.HandleFunc("/api/chat/session-events", s.requireAuth(s.handleChatSessionEvents))
	mux.HandleFunc("/api/chat/prompt-tier", s.requireAuth(s.handleChatPromptTier))
	mux.HandleFunc("/api/chat/clear", s.requireAuth(s.handleChatClear))
	mux.HandleFunc("/api/chat/threads", s.requireAuth(s.handleChatThreads))
	// Per-user persisted session/tab list (restore after a crash). Distinct from
	// /api/chat/threads (in-tab branches).
	mux.HandleFunc("/api/chat/sessions", s.requireAuth(s.handleChatSessionsList))
	mux.HandleFunc("/api/chat/branch", s.requireAuth(s.handleChatBranch))
	mux.HandleFunc("/api/chat/switch-thread", s.requireAuth(s.handleChatSwitchThread))
	mux.HandleFunc("/api/chat/rewind", s.requireAuth(s.handleChatRewind))

	// User/auth management (session required)
	mux.HandleFunc("/api/auth/me", auth.RequireAuth(s.sessions, http.HandlerFunc(s.handleMe)).ServeHTTP)
	mux.HandleFunc("/api/auth/password", auth.RequireAuth(s.sessions, http.HandlerFunc(s.authH.ChangePassword)).ServeHTTP)
	mux.HandleFunc("/api/users", auth.RequireAuth(s.sessions, http.HandlerFunc(s.authH.ListUsers)).ServeHTTP)

	// Aggregate dashboard endpoint — all panels in one server round-trip
	mux.HandleFunc("/api/dashboard", s.requireAuth(s.handleDashboardAll))

	// Workflow visual composer (W7): proxy to aimee-server /v1/workflow/*.
	mux.HandleFunc("/api/workflow/blocks", s.requireAuth(s.handleWorkflowBlocks))
	mux.HandleFunc("/api/workflow/defs", s.requireAuth(s.handleWorkflowDefs))
	mux.HandleFunc("/api/workflow/defs/", s.requireAuth(s.handleWorkflowDefs))
	mux.HandleFunc("/api/workflow/validate", s.requireAuth(s.handleWorkflowValidate))
	mux.HandleFunc("/api/workflow/save", s.requireAuth(s.handleWorkflowSave))
	mux.HandleFunc("/api/workflow/items", s.requireAuth(s.handleWorkflowItems))
	mux.HandleFunc("/api/workflow/items/", s.requireAuth(s.handleWorkflowItems))

	// Credential vault (WP-C.2c): the user re-presents their login password to
	// unlock; calls carry the server.token bearer + X-Aimee-Webuser so
	// aimee-server resolves this user's webuser: vault.
	mux.HandleFunc("/api/vault/unlock", s.requireAuth(s.handleVaultUnlock))
	mux.HandleFunc("/api/vault/password", s.requireAuth(s.handleVaultPassword))
	mux.HandleFunc("/api/vault/credentials", s.requireAuth(s.handleVaultCredentials))

	// Git projects (webchat-git WP-F): clone + per-project ops + listing,
	// forwarded to /v1/workspace/* with the user's webuser: assertion.
	mux.HandleFunc("/api/git/projects", s.requireAuth(s.handleGitProjects))
	mux.HandleFunc("/api/git/clone", s.requireAuth(s.handleGitClone))
	mux.HandleFunc("/api/git/op", s.requireAuth(s.handleGitOp))

	// Live endpoints backed by aimee-server socket
	mux.HandleFunc("/api/agents", s.requireAuth(s.handleAgents))
	mux.HandleFunc("/api/rules", s.requireAuth(s.handleCollabRulesList))
	mux.HandleFunc("/api/rules/active", s.requireAuth(s.handleCollabRulesActive))
	mux.HandleFunc("POST /api/rules/{id}/{action}", s.requireAuth(s.handleCollabRuleAction))
	mux.HandleFunc("/api/delegations", s.requireAuth(s.handleDelegations))
	mux.HandleFunc("/api/metrics", s.requireAuth(s.handleMetrics))
	mux.HandleFunc("GET /api/sessions/workflows/channel/{channel}", s.requireAuth(s.handleWorkflowByChannel))
	mux.HandleFunc("POST /api/sessions/workflows/{id}/pause", s.requireAuth(s.handleWorkflowPause))

	// Live endpoints backed by aimee-server socket (extended dashboard)
	mux.HandleFunc("/api/traces", s.requireAuth(s.dataArrayHandler("dashboard.traces")))
	mux.HandleFunc("/api/plans", s.requireAuth(s.dataArrayHandler("dashboard.plans")))
	mux.HandleFunc("/api/logs", s.requireAuth(s.dataArrayHandler("dashboard.logs")))
	mux.HandleFunc("GET /api/plugins", s.requireAuth(s.handlePluginsList))
	mux.HandleFunc("POST /api/plugins/{name}/toggle", s.requireAuth(s.handlePluginToggle))
	mux.HandleFunc("/api/memory-stats", s.requireAuth(s.dataObjectHandler("dashboard.memory_stats")))
	mux.HandleFunc("/api/dashboard/onboard", s.requireAuth(s.dataObjectHandler("dashboard.onboard")))
	mux.HandleFunc("/api/dashboard/plugins", s.requireAuth(s.dataObjectHandler("dashboard.plugins")))
	// Channels (stored in local SQLite)
	mux.HandleFunc("GET /api/channels", s.requireAuth(s.handleChannelsList))
	mux.HandleFunc("POST /api/channels", s.requireAuth(s.handleChannelsCreate))
	mux.HandleFunc("GET /api/channels/{name}/messages", s.requireAuth(s.handleChannelMessages))
	mux.HandleFunc("POST /api/channels/{name}/messages", s.requireAuth(s.handleChannelPost))
	mux.HandleFunc("GET /api/channels/{name}/events", s.requireAuth(s.handleChannelEvents))
	// LSP diagnostics (live from aimee-server)
	mux.HandleFunc("/api/lsp/diagnostics/summary", s.requireAuth(s.handleLSPDiagnosticsSummary))
}

func (s *server) handleRoot(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}
	http.Redirect(w, r, "/chat", http.StatusFound)
}

func (s *server) handleStubArray(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	fmt.Fprintf(w, "[]")
}

func (s *server) handleStubObject(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	fmt.Fprintf(w, "{}")
}

func (s *server) handleLSPStub(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	fmt.Fprintf(w, `{"errors":0,"warnings":0,"active_servers":0}`)
}

func openDB(dbPath string) (*sql.DB, error) {
	if err := os.MkdirAll(filepath.Dir(dbPath), 0o700); err != nil {
		return nil, err
	}

	db, err := sql.Open("sqlite3", dbPath+"?_journal=WAL&_busy_timeout=5000")
	if err != nil {
		return nil, err
	}

	for _, m := range auth.Migrations {
		if _, err := db.Exec(m); err != nil {
			return nil, fmt.Errorf("migrate: %w", err)
		}
	}

	return db, nil
}

func ensureTLSCert(cfg *config) (cert, key string, err error) {
	if cfg.certFile != "" && cfg.keyFile != "" {
		return cfg.certFile, cfg.keyFile, nil
	}

	dir := filepath.Dir(cfg.dbPath)
	certPath := filepath.Join(dir, "webchat.crt")
	keyPath := filepath.Join(dir, "webchat.key")

	if _, err := os.Stat(certPath); err == nil {
		if _, err := os.Stat(keyPath); err == nil {
			return certPath, keyPath, nil
		}
	}

	if err := generateSelfSignedCert(certPath, keyPath); err != nil {
		return "", "", err
	}
	return certPath, keyPath, nil
}

// certSANs builds the Subject Alternative Names for the auto-generated cert so
// it can actually be trusted for how the browser reaches webchat. A cert with
// only DNS:localhost cannot validate for https://<LAN-IP>:8443, which (besides
// the usual warning) makes browsers refuse to register VSCode's service worker
// ("An SSL certificate error occurred when fetching the script") so the editor's
// webviews break. We include localhost, every non-loopback interface IP, the
// hostname, and any operator-supplied AIMEE_WEBCHAT_TLS_SANS (comma-separated
// IPs/DNS names) so an operator who trusts this cert gets a fully valid context.
func certSANs() (dnsNames []string, ipAddrs []net.IP) {
	dnsSeen := map[string]bool{}
	ipSeen := map[string]bool{}
	addDNS := func(n string) {
		n = strings.TrimSpace(n)
		if n != "" && !dnsSeen[n] {
			dnsSeen[n] = true
			dnsNames = append(dnsNames, n)
		}
	}
	addIP := func(ip net.IP) {
		if ip != nil && !ipSeen[ip.String()] {
			ipSeen[ip.String()] = true
			ipAddrs = append(ipAddrs, ip)
		}
	}

	addDNS("localhost")
	addIP(net.ParseIP("127.0.0.1"))
	addIP(net.ParseIP("::1"))
	if h, err := os.Hostname(); err == nil {
		addDNS(h)
	}
	if addrs, err := net.InterfaceAddrs(); err == nil {
		for _, a := range addrs {
			if ipnet, ok := a.(*net.IPNet); ok && !ipnet.IP.IsLoopback() {
				addIP(ipnet.IP)
			}
		}
	}
	// Operator override for names/IPs not derivable here (e.g. a reverse-proxy
	// hostname): AIMEE_WEBCHAT_TLS_SANS="aimee.lan,10.0.0.5".
	for _, s := range strings.Split(os.Getenv("AIMEE_WEBCHAT_TLS_SANS"), ",") {
		s = strings.TrimSpace(s)
		if s == "" {
			continue
		}
		if ip := net.ParseIP(s); ip != nil {
			addIP(ip)
		} else {
			addDNS(s)
		}
	}
	return dnsNames, ipAddrs
}

func generateSelfSignedCert(certPath, keyPath string) error {
	priv, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return err
	}

	dnsNames, ipAddrs := certSANs()

	serial, _ := rand.Int(rand.Reader, new(big.Int).Lsh(big.NewInt(1), 128))
	tmpl := &x509.Certificate{
		SerialNumber:          serial,
		Subject:               pkix.Name{CommonName: "aimee-webchat"},
		NotBefore:             time.Now().Add(-time.Minute),
		NotAfter:              time.Now().Add(10 * 365 * 24 * time.Hour),
		KeyUsage:              x509.KeyUsageDigitalSignature | x509.KeyUsageCertSign,
		ExtKeyUsage:           []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		BasicConstraintsValid: true,
		IsCA:                  true,
		DNSNames:              dnsNames,
		IPAddresses:           ipAddrs,
	}

	certDER, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &priv.PublicKey, priv)
	if err != nil {
		return err
	}

	cf, err := os.OpenFile(certPath, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0o600)
	if err != nil {
		return err
	}
	defer cf.Close()
	if err := pem.Encode(cf, &pem.Block{Type: "CERTIFICATE", Bytes: certDER}); err != nil {
		return err
	}

	kf, err := os.OpenFile(keyPath, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0o600)
	if err != nil {
		return err
	}
	defer kf.Close()
	privDER, err := x509.MarshalECPrivateKey(priv)
	if err != nil {
		return err
	}
	return pem.Encode(kf, &pem.Block{Type: "EC PRIVATE KEY", Bytes: privDER})
}
