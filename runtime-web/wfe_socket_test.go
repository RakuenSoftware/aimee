package main

import (
	"path/filepath"
	"testing"
)

func TestWFEOwnedRoutesUseGoControlPlaneSocket(t *testing.T) {
	root := t.TempDir()
	configured := filepath.Join(root, "go-wfe.sock")
	s := &server{cfg: &config{
		socketPath: filepath.Join(root, "aimee.sock"),
		wfeEngine:  "go",
		wfeSocket:  configured,
	}}
	for _, path := range []string{"/v1/workflow/items", "/v1/workflow/config/set", "/v1/trigger/fire", "/v1/dev/submit"} {
		if got := s.aimeeHTTPSockPathFor(path); got != configured {
			t.Fatalf("%s routed to %s, want Go WFE socket %s", path, got, configured)
		}
	}
	if got := s.aimeeHTTPSockPathFor("/v1/delegate/run"); got == configured {
		t.Fatal("agent resource-plane route was sent to WFE control plane")
	}
	if got := s.aimeeHTTPSockPathFor("/v1/workflowX"); got == configured {
		t.Fatal("lookalike route was sent to WFE control plane")
	}
	s.cfg.wfeEngine = "c"
	if got := s.aimeeHTTPSockPathFor("/v1/workflow/items"); got == configured {
		t.Fatal("legacy engine mode was sent to Go WFE socket")
	}
}

func TestWFERoutingConfigurationIsCapturedAtStartup(t *testing.T) {
	configured := filepath.Join(t.TempDir(), "configured.sock")
	t.Setenv("AIMEE_WFE_ENGINE", "go")
	t.Setenv("AIMEE_WFE_HTTP_SOCKET", configured)
	cfg, err := newConfig(8080, "", "", filepath.Join(t.TempDir(), "aimee.sock"), filepath.Join(t.TempDir(), "webchat.db"), "")
	if err != nil {
		t.Fatal(err)
	}
	s := &server{cfg: cfg}

	// A running server must not be redirected by ambient environment mutation.
	t.Setenv("AIMEE_WFE_HTTP_SOCKET", filepath.Join(t.TempDir(), "hostile.sock"))
	if got := s.aimeeHTTPSockPathFor("/v1/workflow/items"); got != configured {
		t.Fatalf("workflow route changed after startup: got %s, want %s", got, configured)
	}
}
