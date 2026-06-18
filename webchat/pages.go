package main

import (
	"fmt"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"time"
)

var spaHTML []byte

const fallbackSPA = `<!DOCTYPE html><html><head><meta charset="utf-8"><title>aimee</title>
<style>body{font-family:system-ui;background:#111;color:#aaa;display:flex;align-items:center;justify-content:center;height:100vh;margin:0}</style>
</head><body><p>SPA not built. Run <code>cd frontend && npm run build</code> then restart aimee-webchat.</p></body></html>`

// initSPA loads the React SPA from cfg.spaPath or auto-discovers it.
func initSPA(cfg *config) error {
	path := cfg.spaPath
	if path == "" {
		// Try adjacent to binary first, then repo-relative locations.
		exe, _ := os.Executable()
		candidates := []string{
			filepath.Join(filepath.Dir(exe), "frontend", "dist", "index.html"),
			filepath.Join(filepath.Dir(exe), "..", "frontend", "dist", "index.html"),
		}
		for _, c := range candidates {
			if _, err := os.Stat(c); err == nil {
				path = c
				break
			}
		}
	}

	if path == "" {
		spaHTML = []byte(fallbackSPA)
		log.Printf("aimee-webchat: SPA not found, serving fallback page (use --spa to specify path)")
		return nil
	}

	b, err := os.ReadFile(path)
	if err != nil {
		spaHTML = []byte(fallbackSPA)
		log.Printf("aimee-webchat: SPA read error (%v), serving fallback", err)
		return nil
	}
	spaHTML = b
	log.Printf("aimee-webchat: loaded SPA from %s (%d bytes)", path, len(b))
	return nil
}

const loginHTML = `<!DOCTYPE html><html><head><meta charset='utf-8'>
<title>aimee - login</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui;background:#111;color:#eee;display:flex;
justify-content:center;align-items:center;min-height:100vh}
.card{background:#1a1a2e;border-radius:12px;padding:32px;width:360px;
border:1px solid #333}
h1{font-size:22px;color:#8cf;margin-bottom:24px;text-align:center}
label{display:block;font-size:13px;color:#888;margin-bottom:4px}
input{width:100%;padding:10px 12px;background:#111;color:#eee;border:1px solid #444;
border-radius:6px;font-size:14px;margin-bottom:16px}
input:focus{outline:none;border-color:#8cf}
button{width:100%;padding:10px;background:#234;color:#8cf;border:1px solid #456;
border-radius:6px;font-size:14px;cursor:pointer}
button:hover{background:#345}
.error{color:#f44;font-size:13px;margin-bottom:12px;text-align:center}
</style></head><body>
<div class='card'>
<h1>aimee</h1>
<div id='err' class='error'></div>
<form id='f'>
<label>Username</label>
<input name='username' type='text' autocomplete='username' autofocus required>
<label>Password</label>
<input name='password' type='password' autocomplete='current-password' required>
<button type='submit'>Sign in</button>
</form></div>
<script>
if(location.search.includes('error=1'))
  document.getElementById('err').textContent='Invalid credentials';
document.getElementById('f').addEventListener('submit',async function(e){
  e.preventDefault();
  const fd=new FormData(this);
  const r=await fetch('/api/auth/login',{method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({username:fd.get('username'),password:fd.get('password')})
  });
  if(r.ok){location.href='/chat';}
  else{document.getElementById('err').textContent='Invalid credentials';}
});
</script></body></html>`

func (s *server) handleLogin(w http.ResponseWriter, r *http.Request) {
	// Already logged in → redirect to chat.
	if cookie, err := r.Cookie("session"); err == nil {
		if _, err := s.sessions.ValidateSession(cookie.Value); err == nil {
			http.Redirect(w, r, "/chat", http.StatusFound)
			return
		}
	}

	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	fmt.Fprint(w, loginHTML)
}

func (s *server) handleLogout(w http.ResponseWriter, r *http.Request) {
	if cookie, err := r.Cookie("session"); err == nil {
		s.sessions.DeleteSession(cookie.Value)
	}

	http.SetCookie(w, &http.Cookie{
		Name:     "session",
		Value:    "",
		Path:     "/",
		HttpOnly: true,
		Secure:   s.cfg.tlsEnabled(),
		SameSite: http.SameSiteStrictMode,
		Expires:  time.Unix(0, 0),
		MaxAge:   -1,
	})

	http.Redirect(w, r, "/login", http.StatusFound)
}

// handleSPA serves the React SPA. The dist is a single inlined index.html (all
// JS/CSS embedded), so the browser caches THIS document — and without no-cache it
// kept serving a stale bundle after a deploy (e.g. a fixed page still looked
// broken until the cache was manually cleared). Force revalidation so a redeploy
// is always picked up on the next navigation.
func (s *server) handleSPA(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Cache-Control", "no-cache, no-store, must-revalidate")
	w.Header().Set("Pragma", "no-cache")
	w.Header().Set("Expires", "0")
	w.Write(spaHTML)
}

// handleAuthLogin delegates JSON login to the SmoothGUI handler and sets a
// secure cookie appropriate for this server's TLS mode.
func (s *server) handleAuthLogin(w http.ResponseWriter, r *http.Request) {
	// Wrap the response writer to intercept the Set-Cookie header so we can
	// rewrite Secure based on our TLS setting.
	if !s.cfg.tlsEnabled() {
		w = &insecureCookieWriter{ResponseWriter: w}
	}
	s.authH.Login(w, r)
}

// insecureCookieWriter strips the Secure flag from Set-Cookie when TLS is off.
// Both WriteHeader and Write are overridden because net/http's implicit
// WriteHeader(200) (triggered by the first Write call) bypasses the interface
// and goes straight to the concrete ResponseWriter, so we must intercept Write
// to ensure the cookie rewrite runs before headers are flushed.
type insecureCookieWriter struct {
	http.ResponseWriter
	headerWritten bool
}

func (iw *insecureCookieWriter) WriteHeader(code int) {
	if iw.headerWritten {
		return
	}
	iw.headerWritten = true
	h := iw.ResponseWriter.Header()
	cookies := h["Set-Cookie"]
	h.Del("Set-Cookie")
	for _, c := range cookies {
		h.Add("Set-Cookie", removeSecureFlag(c))
	}
	iw.ResponseWriter.WriteHeader(code)
}

func (iw *insecureCookieWriter) Write(b []byte) (int, error) {
	if !iw.headerWritten {
		iw.WriteHeader(http.StatusOK)
	}
	return iw.ResponseWriter.Write(b)
}

func removeSecureFlag(cookie string) string {
	out := ""
	for _, part := range splitCookieParts(cookie) {
		if part == "Secure" {
			continue
		}
		if out != "" {
			out += "; "
		}
		out += part
	}
	return out
}

func splitCookieParts(s string) []string {
	var parts []string
	start := 0
	for i := 0; i <= len(s); i++ {
		if i == len(s) || s[i] == ';' {
			part := s[start:i]
			for len(part) > 0 && (part[0] == ' ' || part[0] == '\t') {
				part = part[1:]
			}
			if part != "" {
				parts = append(parts, part)
			}
			start = i + 1
		}
	}
	return parts
}
