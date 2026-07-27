package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestFleetOIDCAlignmentRequiresKBMatch(t *testing.T) {
	kb := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "Bearer console-secret" {
			t.Fatalf("config fetch Authorization = %q", r.Header.Get("Authorization"))
		}
		_ = json.NewEncoder(w).Encode(map[string]any{
			"configured": true, "issuer": "https://idp", "audience": "aimee-kb",
			"jwks_url": "https://idp/jwks", "admin_claim": "groups",
			"admin_values": []string{"admins"},
		})
	}))
	defer kb.Close()
	cfg := &config{oidc: oidcConfig{Issuer: "https://idp", Audience: "aimee-kb",
		JWKSURL: "https://idp/jwks", AdminClaim: "groups", AdminValues: []string{"admins"}}}
	if !cfg.fleetOIDCAligned(kb.URL, "console-secret", kb.Client()) {
		t.Fatal("matching kb OIDC contract was disabled")
	}
	cfg.oidc.Audience = "console-only"
	if cfg.fleetOIDCAligned(kb.URL, "console-secret", kb.Client()) {
		t.Fatal("mismatched audience enabled fleet proxy")
	}
}

func TestFetchOIDCConfigRejectsTrailingAndOversizedResponses(t *testing.T) {
	for name, body := range map[string]string{
		"trailing":  `{"configured":true,"issuer":"i","audience":"a"} junk`,
		"oversized": strings.Repeat(" ", (1<<20)+1),
	} {
		t.Run(name, func(t *testing.T) {
			kb := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
				_, _ = w.Write([]byte(body))
			}))
			defer kb.Close()
			if _, ok := fetchOIDCConfigFromKB(kb.URL, "secret", kb.Client()); ok {
				t.Fatal("invalid bounded response accepted")
			}
		})
	}
}
