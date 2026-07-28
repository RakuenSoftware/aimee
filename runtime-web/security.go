package main

import (
	"crypto/sha256"
	"encoding/base64"
	"net/http"
	"regexp"
	"strings"
)

var inlineScript = regexp.MustCompile(`(?s)<script[^>]*>(.*?)</script>`)

// contentSecurityPolicy pins the executable content in the single-file pages.
// Styles remain inline-compatible because rendered chat markdown and several UI
// components intentionally apply style attributes at runtime.
func contentSecurityPolicy(html []byte) string {
	script := "'self'"
	for _, match := range inlineScript.FindAllSubmatch(html, -1) {
		sum := sha256.Sum256(match[1])
		script += " 'sha256-" + base64.StdEncoding.EncodeToString(sum[:]) + "'"
	}
	return strings.Join([]string{
		"default-src 'self'",
		"script-src " + script,
		"script-src-attr 'none'",
		"style-src 'self' 'unsafe-inline'",
		"img-src 'self' data: blob:",
		"font-src 'self' data:",
		"connect-src 'self'",
		"frame-src 'self'",
		"frame-ancestors 'self'",
		"form-action 'self'",
		"base-uri 'self'",
		"object-src 'none'",
	}, "; ")
}

// securityHeaders applies response hardening that is safe for every route,
// including the same-origin /vscode iframe and WebSocket proxy. HSTS is omitted
// deliberately: the appliance defaults to a self-signed certificate and may be
// served by a TLS-terminating proxy; that proxy owns HSTS for its public name.
func (s *server) securityHeaders(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("X-Content-Type-Options", "nosniff")
		w.Header().Set("X-Frame-Options", "SAMEORIGIN")
		w.Header().Set("Referrer-Policy", "no-referrer")
		w.Header().Set("Permissions-Policy", "geolocation=(), microphone=(), camera=(), usb=()")
		next.ServeHTTP(w, r)
	})
}
