package main

import (
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

// The WFE is an internal control plane behind an owner-only Unix socket. Keep
// credential material out of its long-lived argv/environment and do not grow a
// second TCP authentication surface here.
func TestControlPlaneRemainsUnixSocketOnlyAndCredentialFree(t *testing.T) {
	_, testFile, _, ok := runtime.Caller(0)
	if !ok {
		t.Fatal("locate test source")
	}
	source, err := os.ReadFile(filepath.Join(filepath.Dir(testFile), "main.go"))
	if err != nil {
		t.Fatal(err)
	}
	text := string(source)
	for _, forbidden := range []string{
		`flag.String("listen"`,
		`agent-service-bearer`,
		`bearer-token`,
		`AIMEE_AGENT_SERVICE_BEARER`,
		`AIMEE_API_BEARER_TOKEN`,
		`net.Listen("tcp"`,
	} {
		if strings.Contains(text, forbidden) {
			t.Fatalf("WFE control plane reintroduced forbidden credential/TCP surface %q", forbidden)
		}
	}
	if !strings.Contains(text, `net.Listen("unix", *socket)`) ||
		!strings.Contains(text, `os.Chmod(*socket, 0o600)`) {
		t.Fatal("WFE control plane must bind an owner-only Unix socket")
	}
}
