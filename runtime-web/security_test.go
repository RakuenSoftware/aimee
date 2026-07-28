package main

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestSecurityHeaders(t *testing.T) {
	s := &server{}
	h := s.securityHeaders(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusNoContent)
	}))
	r := httptest.NewRecorder()
	h.ServeHTTP(r, httptest.NewRequest(http.MethodGet, "/login", nil))

	want := map[string]string{
		"X-Content-Type-Options": "nosniff",
		"X-Frame-Options":        "SAMEORIGIN",
		"Referrer-Policy":        "no-referrer",
		"Permissions-Policy":     "geolocation=(), microphone=(), camera=(), usb=()",
	}
	for name, value := range want {
		if got := r.Header().Get(name); got != value {
			t.Errorf("%s=%q, want %q", name, got, value)
		}
	}
}

func TestContentSecurityPolicyPinsInlineScripts(t *testing.T) {
	csp := contentSecurityPolicy([]byte(`<html><script>safe()</script><style>x{color:red}</style></html>`))
	for _, want := range []string{
		"default-src 'self'",
		"script-src 'self' 'sha256-",
		"script-src-attr 'none'",
		"style-src 'self' 'unsafe-inline'",
		"frame-src 'self'",
		"frame-ancestors 'self'",
		"object-src 'none'",
	} {
		if !strings.Contains(csp, want) {
			t.Errorf("CSP %q does not contain %q", csp, want)
		}
	}
	if strings.Contains(csp, "script-src 'self' 'unsafe-inline'") {
		t.Fatalf("inline scripts must be hash-pinned: %s", csp)
	}
}

func TestPageHandlersSetContentSecurityPolicy(t *testing.T) {
	s := &server{
		loginCSP: contentSecurityPolicy([]byte(loginHTML)),
		spaCSP:   contentSecurityPolicy(spaHTML),
	}

	login := httptest.NewRecorder()
	s.handleLogin(login, httptest.NewRequest(http.MethodGet, "/login", nil))
	if got := login.Header().Get("Content-Security-Policy"); got == "" {
		t.Fatal("login response is missing Content-Security-Policy")
	}

	spa := httptest.NewRecorder()
	s.handleSPA(spa, httptest.NewRequest(http.MethodGet, "/chat", nil))
	if got := spa.Header().Get("Content-Security-Policy"); got == "" {
		t.Fatal("SPA response is missing Content-Security-Policy")
	}
}
