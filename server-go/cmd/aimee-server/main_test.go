package main

import (
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

func TestConfiguredForgeHandlesURLAndSocketIndependently(t *testing.T) {
	if forge, err := configuredForge("", "/tmp/aimee-forge.sock"); err != nil || forge == nil {
		t.Fatalf("socket-only forge = %T, %v", forge, err)
	}
	if forge, err := configuredForge("https://forge.invalid", ""); err == nil || forge != nil ||
		!strings.Contains(err.Error(), "requires a Unix socket") {
		t.Fatalf("URL-only forge = %T, %v", forge, err)
	}
	if forge, err := configuredForge("", ""); err != nil || forge != nil {
		t.Fatalf("unconfigured forge = %T, %v", forge, err)
	}
}

// Workflow control is admitted by the host and carried only by the private
// module stage. No caller-controlled HTTP identity headers may bypass it.
func TestControlPlaneUsesOnlyAuthenticatedModuleIngress(t *testing.T) {
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
		`flag.String("bearer-token"`,
		`observability-bearer-token",`,
		`AIMEE_AGENT_SERVICE_BEARER`,
		`AIMEE_API_BEARER_TOKEN`,
		`net.Listen(`,
		`http.Server{`,
		`flag.String("socket"`,
	} {
		if strings.Contains(text, forbidden) {
			t.Fatalf("WFE control plane reintroduced forbidden HTTP/credential surface %q", forbidden)
		}
	}
	if !strings.Contains(text, `err = ring.RunModuleProcess(rootCtx, ring.ModuleProcessConfig{`) ||
		!strings.Contains(text, `Handler: workflows.NewHandlerContext(rootCtx, handler)`) ||
		!strings.Contains(text, `signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)`) {
		t.Fatal("workflow control must serve and drain on the signal-bound module lifecycle")
	}
	if strings.Contains(text, `Handle("GET /metrics"`) {
		t.Fatal("metrics must not be mounted on the WFE control-plane socket")
	}
	if !strings.Contains(text, `MetricsServerConfigFromEnv("AIMEE_SERVER")`) ||
		!strings.Contains(text, `observability.StartMetricsServer`) {
		t.Fatal("metrics exposure must use the shared opt-in observability listener")
	}
}
