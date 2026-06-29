package main

import (
	"crypto/tls"
	"net/http"
	"net/http/httptest"
	"testing"
)

// sameOriginVSCode is the cross-origin gate guarding the /vscode reverse proxy.
// A cookie alone is CSRF-able, so the proxy must reject any cross-origin request
// and, for WebSocket upgrades (the editor terminal), refuse a missing Origin too
// (browsers always send one on an upgrade). Same-origin iframe traffic — asset
// GETs without an Origin, and XHR/WS whose Origin matches our host — must pass.
func TestSameOriginVSCodeGate(t *testing.T) {
	mkReq := func(method, host, origin string, tls_ bool, xfproto, secFetch string, ws bool) *http.Request {
		r := httptest.NewRequest(method, "http://"+host+"/vscode/", nil)
		r.Host = host
		if tls_ {
			r.TLS = &tls.ConnectionState{}
		}
		if origin != "" {
			r.Header.Set("Origin", origin)
		}
		if xfproto != "" {
			r.Header.Set("X-Forwarded-Proto", xfproto)
		}
		if secFetch != "" {
			r.Header.Set("Sec-Fetch-Site", secFetch)
		}
		if ws {
			r.Header.Set("Upgrade", "websocket")
			r.Header.Set("Connection", "keep-alive, Upgrade")
		}
		return r
	}

	const host = "aimee.example:8443" // explicit non-default port
	cases := []struct {
		name                          string
		method, host, origin, xfproto string
		secFetch                      string
		tls_, ws, want                bool
	}{
		// Same-origin iframe traffic must pass.
		{"asset GET no origin", "GET", host, "", "", "", true, false, true},
		{"XHR same origin", "GET", host, "https://" + host, "", "", true, false, true},
		{"XHR same origin case-folded scheme", "GET", host, "HTTPS://" + host, "", "", true, false, true},
		{"WS same origin", "GET", host, "https://" + host, "", "", true, true, true},
		// Default-port normalization: Host carries :443, Origin omits it.
		{"XHR same origin default 443 stripped", "GET", "aimee.example:443", "https://aimee.example", "", "", true, false, true},
		{"WS same origin default 443 stripped", "GET", "aimee.example:443", "https://aimee.example", "", "", true, true, true},
		{"XHR same origin default 80 stripped", "GET", "aimee.example:80", "http://aimee.example", "", "", false, false, true},
		// TLS-terminating proxy: r.TLS nil, X-Forwarded-Proto: https.
		{"behind TLS proxy, XHR https origin", "GET", "aimee.example", "https://aimee.example", "https", "", false, false, true},
		{"behind TLS proxy, WS https origin", "GET", "aimee.example", "https://aimee.example", "https, http", "", false, true, true},
		// Cross-origin must be cut.
		{"XHR cross origin", "GET", host, "https://evil.example", "", "", true, false, false},
		{"WS cross origin", "GET", host, "https://evil.example", "", "", true, true, false},
		{"WS missing origin refused", "GET", host, "", "", "", true, true, false},
		{"WS wrong scheme", "GET", host, "http://" + host, "", "", true, true, false},
		{"XHR same host wrong port", "GET", "aimee.example:8443", "https://aimee.example:9999", "", "", true, false, false},
		// Empty-Origin: safe methods pass; non-safe pass only with Sec-Fetch-Site same-origin.
		{"empty origin POST refused", "POST", host, "", "", "", true, false, false},
		{"empty origin POST sec-fetch same-origin", "POST", host, "", "", "same-origin", true, false, true},
		{"empty origin POST sec-fetch cross-site", "POST", host, "", "", "cross-site", true, false, false},
		{"empty origin OPTIONS allowed", "OPTIONS", host, "", "", "", true, false, true},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			r := mkReq(tc.method, tc.host, tc.origin, tc.tls_, tc.xfproto, tc.secFetch, tc.ws)
			if got := sameOriginVSCode(r); got != tc.want {
				t.Fatalf("sameOriginVSCode = %v, want %v", got, tc.want)
			}
		})
	}
}

// isWebSocketUpgrade must detect the upgrade only when both the Upgrade token and
// a "upgrade" entry in Connection are present (the latter may be one of several
// comma-separated tokens), and must not be fooled by a bare Connection header.
func TestIsWebSocketUpgrade(t *testing.T) {
	cases := []struct {
		upgrade, conn string
		want          bool
	}{
		{"websocket", "Upgrade", true},
		{"websocket", "keep-alive, Upgrade", true},
		{"WebSocket", "upgrade", true},
		{"websocket", "keep-alive", false},
		{"", "Upgrade", false},
		{"h2c", "Upgrade", false},
	}
	for _, tc := range cases {
		r := httptest.NewRequest(http.MethodGet, "/vscode/", nil)
		if tc.upgrade != "" {
			r.Header.Set("Upgrade", tc.upgrade)
		}
		r.Header.Set("Connection", tc.conn)
		if got := isWebSocketUpgrade(r); got != tc.want {
			t.Errorf("isWebSocketUpgrade(Upgrade=%q,Connection=%q)=%v want %v",
				tc.upgrade, tc.conn, got, tc.want)
		}
	}
}
