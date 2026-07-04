package main

import (
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"os"
	"regexp"
	"strings"
)

const sessionCookie = "kbc_session"

type server struct {
	cfg      *config
	auth     *authenticator
	sessions *sessionStore
	kbBearer string
	kbClient *http.Client
	spa      []byte // cached SPA bytes (read once at startup)
	spaCSP   string // CSP for the SPA route, with inline-content hashes
	logins   *rateLimiter
}

var (
	reInlineScript = regexp.MustCompile(`(?s)<script[^>]*>(.*?)</script>`)
	reInlineStyle  = regexp.MustCompile(`(?s)<style[^>]*>(.*?)</style>`)
)

// loadSPA caches the single-file SPA and precomputes its CSP. The bundle inlines
// its JS/CSS, so a plain `script-src 'self'` would block it; instead we pin the
// exact inline-content sha256 hashes (no HTML mutation, no 'unsafe-inline').
func (s *server) loadSPA() {
	b, err := os.ReadFile(s.cfg.spaPath)
	if err != nil {
		s.spa = nil
		s.spaCSP = s.cspHeader(nil, nil)
		return
	}
	s.spa = b
	s.spaCSP = s.cspHeader(inlineHashes(b, reInlineScript), inlineHashes(b, reInlineStyle))
}

func inlineHashes(html []byte, re *regexp.Regexp) []string {
	var hs []string
	for _, m := range re.FindAllSubmatch(html, -1) {
		sum := sha256.Sum256(m[1])
		hs = append(hs, "'sha256-"+base64.StdEncoding.EncodeToString(sum[:])+"'")
	}
	return hs
}

// cspHeader builds the CSP. The only non-self entries are the IdP origin (for the
// OIDC flow) and the inline script/style hashes. Never '*', never 'unsafe-inline'.
func (s *server) cspHeader(scriptHashes, styleHashes []string) string {
	idp := oidcOrigin(s.cfg.oidc.Issuer)
	connect, frame, form := "'self'", "'none'", "'self'"
	if idp != "" {
		connect += " " + idp
		form += " " + idp
		frame = idp
	}
	script := "'self'"
	if len(scriptHashes) > 0 {
		script += " " + strings.Join(scriptHashes, " ")
	}
	style := "'self'"
	if len(styleHashes) > 0 {
		style += " " + strings.Join(styleHashes, " ")
	}
	return strings.Join([]string{
		"default-src 'self'",
		"script-src " + script,
		"style-src " + style,
		"img-src 'self' data:",
		"connect-src " + connect,
		"frame-src " + frame,
		"form-action " + form,
		"base-uri 'self'",
		"object-src 'none'",
	}, "; ")
}

func (s *server) routes() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/api/login", s.handleLogin)
	mux.HandleFunc("/api/logout", s.handleLogout)
	mux.HandleFunc("/api/session", s.handleSession)
	mux.HandleFunc("/api/", s.handleAPI)
	mux.HandleFunc("/", s.handleSPA)
	return s.securityHeaders(mux)
}

// securityHeaders applies the base hardening headers plus a strict default CSP
// (no inline) for non-SPA responses; handleSPA overrides the CSP with the
// hash-pinned SPA policy. HSTS is intentionally omitted (self-signed cert +
// localhost default); set it at a fronting proxy for a real deployment.
func (s *server) securityHeaders(next http.Handler) http.Handler {
	defaultCSP := s.cspHeader(nil, nil)
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Security-Policy", defaultCSP)
		w.Header().Set("X-Content-Type-Options", "nosniff")
		w.Header().Set("X-Frame-Options", "DENY")
		w.Header().Set("Referrer-Policy", "no-referrer")
		w.Header().Set("Permissions-Policy", "geolocation=(), microphone=(), camera=(), usb=()")
		next.ServeHTTP(w, r)
	})
}

func (s *server) requireSession(r *http.Request) (*session, error) {
	c, err := r.Cookie(sessionCookie)
	if err != nil {
		return nil, err
	}
	return s.sessions.get(c.Value)
}

// handleLogin accepts an OIDC id_token (primary) or, when the presence-flag is
// set, a break-glass bearer. On success it rotates in a fresh session.
func (s *server) handleLogin(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeJSON(w, http.StatusMethodNotAllowed, map[string]string{"error": "method not allowed"})
		return
	}
	ipKey := clientIP(r) + "|login"
	if !s.logins.allow(ipKey) {
		writeJSON(w, http.StatusTooManyRequests, map[string]string{"error": "too many attempts; try again later"})
		return
	}

	var req struct {
		IDToken          string `json:"id_token"`
		BreakGlassBearer string `json:"break_glass_bearer"`
	}
	if err := json.NewDecoder(io.LimitReader(r.Body, 1<<16)).Decode(&req); err != nil && !errors.Is(err, io.EOF) {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "malformed request body"})
		return
	}
	// Ambiguous intent: reject rather than silently prefer one path.
	if req.IDToken != "" && req.BreakGlassBearer != "" {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "provide exactly one of id_token or break_glass_bearer"})
		return
	}

	var p *principal
	breakGlass := false
	switch {
	case req.IDToken != "" && s.cfg.oidcConfigured():
		pp, err := s.auth.verifyOIDC(req.IDToken)
		if err != nil {
			s.logins.fail(ipKey)
			writeJSON(w, http.StatusUnauthorized, map[string]string{"error": "oidc verification failed"})
			return
		}
		p = pp
	case req.BreakGlassBearer != "" && breakGlassEnabled(s.cfg.consoleHome):
		if !constEq(req.BreakGlassBearer, s.kbBearer) {
			s.logins.fail(ipKey)
			writeJSON(w, http.StatusUnauthorized, map[string]string{"error": "break-glass denied"})
			return
		}
		p = &principal{iss: "break-glass", sub: "break-glass", viaBreakGlass: true}
		breakGlass = true
	default:
		s.logins.fail(ipKey)
		writeJSON(w, http.StatusUnauthorized, map[string]string{"error": "no usable credential (oidc not configured / break-glass off)"})
		return
	}

	sess, err := s.sessions.create(p, breakGlass)
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "session create failed"})
		return
	}
	s.logins.reset(ipKey)
	http.SetCookie(w, &http.Cookie{
		Name: sessionCookie, Value: sess.id, Path: "/",
		HttpOnly: true, Secure: true, SameSite: http.SameSiteStrictMode,
	})
	action := "login"
	if breakGlass {
		action = "break_glass_login"
	}
	recordAudit(auditEvent{Actor: p.sub, Iss: p.iss, Action: action, SourceIP: clientIP(r)})
	writeJSON(w, http.StatusOK, map[string]any{"csrf": sess.csrf, "break_glass": breakGlass})
}

func (s *server) handleLogout(w http.ResponseWriter, r *http.Request) {
	if c, err := r.Cookie(sessionCookie); err == nil {
		s.sessions.del(c.Value)
	}
	http.SetCookie(w, &http.Cookie{Name: sessionCookie, Value: "", Path: "/", MaxAge: -1, HttpOnly: true, Secure: true, SameSite: http.SameSiteStrictMode})
	writeJSON(w, http.StatusOK, map[string]string{"status": "logged out"})
}

// handleSession returns the current session's csrf token + break-glass flag for
// the SPA to bootstrap; 401 if there is no valid session.
func (s *server) handleSession(w http.ResponseWriter, r *http.Request) {
	sess, err := s.requireSession(r)
	if err != nil {
		writeJSON(w, http.StatusUnauthorized, map[string]string{"error": "unauthorized"})
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"csrf": sess.csrf, "break_glass": sess.breakGlass})
}

func (s *server) handleAPI(w http.ResponseWriter, r *http.Request) {
	sess, err := s.requireSession(r)
	if err != nil {
		writeJSON(w, http.StatusUnauthorized, map[string]string{"error": "unauthorized"})
		return
	}
	s.proxyAPI(w, r, sess)
}

// handleSPA serves the cached single-file console SPA for all non-API routes,
// with the hash-pinned CSP.
func (s *server) handleSPA(w http.ResponseWriter, r *http.Request) {
	if strings.HasPrefix(r.URL.Path, "/api/") {
		http.NotFound(w, r)
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	if s.spa == nil {
		_, _ = io.WriteString(w, `<!doctype html><meta charset=utf-8><title>aimee-kb console</title>`+
			`<p>SPA not built. Run <code>cd frontend &amp;&amp; npm run build:console</code>.</p>`)
		return
	}
	w.Header().Set("Content-Security-Policy", s.spaCSP)
	w.Header().Set("Cache-Control", "no-cache")
	_, _ = w.Write(s.spa)
}
