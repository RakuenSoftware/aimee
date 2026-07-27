package main

import (
	"path/filepath"
	"testing"
)

func TestWFEOwnedRoutesUseGoControlPlaneSocket(t *testing.T) {
	root := t.TempDir()
	configured := filepath.Join(root, "go-wfe.sock")
	t.Setenv("AIMEE_WFE_ENGINE", "go")
	t.Setenv("AIMEE_WFE_HTTP_SOCKET", configured)
	s := &server{cfg: &config{socketPath: filepath.Join(root, "aimee.sock")}}
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
	t.Setenv("AIMEE_WFE_ENGINE", "c")
	if got := s.aimeeHTTPSockPathFor("/v1/workflow/items"); got == configured {
		t.Fatal("legacy engine mode was sent to Go WFE socket")
	}
}
