package observability

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
	"time"
)

// The exporter attaches as the principal the contract declares, the one whose
// generated grant says telemetry=yes.
func TestRingTelemetryPrincipalIsTheContracts(t *testing.T) {
	raw, err := os.ReadFile(filepath.Join("..", "..", "..", "src", "modules", "process-contracts.json"))
	if err != nil {
		t.Fatal(err)
	}
	var contracts struct {
		PrincipalClass uint32 `json:"principal_class"`
		Telemetry      struct {
			ID           string `json:"id"`
			PrincipalRef uint32 `json:"principal_ref"`
			Executable   string `json:"executable"`
		} `json:"telemetry"`
	}
	if err := json.Unmarshal(raw, &contracts); err != nil {
		t.Fatal(err)
	}
	if contracts.PrincipalClass != RingTelemetryPrincipalClass ||
		contracts.Telemetry.PrincipalRef != RingTelemetryPrincipalRef {
		t.Fatalf("contract principal %d/%d, exporter attaches as %d/%d", contracts.PrincipalClass,
			contracts.Telemetry.PrincipalRef, RingTelemetryPrincipalClass, RingTelemetryPrincipalRef)
	}
	// cmd/aimee-module dispatches on this executable name.
	if filepath.Base(contracts.Telemetry.Executable) != "aimee-module-ring-telemetry" {
		t.Fatalf("executable %q", contracts.Telemetry.Executable)
	}
}

func clearOTLPEnv(t *testing.T) {
	for _, name := range []string{"OTEL_EXPORTER_OTLP_ENDPOINT", "OTEL_EXPORTER_OTLP_LOGS_ENDPOINT",
		"OTEL_EXPORTER_OTLP_TRACES_ENDPOINT", "OTEL_EXPORTER_OTLP_METRICS_ENDPOINT",
		"AIMEE_OBSERVABILITY_ALLOW_INSECURE_OTLP"} {
		t.Setenv(name, "")
	}
}

// With no collector there is nowhere off the host to send records: the
// exporter does not attach (so it never wakes on an undrained ring) and returns
// as soon as it is stopped.
func TestRingTelemetryWithoutCollectorSleepsUnattached(t *testing.T) {
	clearOTLPEnv(t)
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- RunRingTelemetry(ctx, filepath.Join(t.TempDir(), "absent.sock"), "") }()
	select {
	case err := <-done:
		t.Fatalf("returned before it was stopped: %v", err)
	case <-time.After(100 * time.Millisecond):
	}
	cancel()
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("did not stop")
	}
}

// The ring exporter is held to the same collector policy as every exporter:
// plaintext to a remote collector is refused.
func TestRingTelemetryKeepsTheCollectorPolicy(t *testing.T) {
	clearOTLPEnv(t)
	t.Setenv("OTEL_EXPORTER_OTLP_LOGS_ENDPOINT", "http://collector.example:4318/v1/logs")
	err := RunRingTelemetry(context.Background(), filepath.Join(t.TempDir(), "absent.sock"), "")
	if err == nil {
		t.Fatal("a plaintext remote collector was accepted")
	}
}
