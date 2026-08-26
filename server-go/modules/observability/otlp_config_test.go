package observability

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func clearOTLPEnvironment(t *testing.T) {
	t.Helper()
	for _, name := range []string{
		"OTEL_EXPORTER_OTLP_ENDPOINT", "OTEL_EXPORTER_OTLP_TRACES_ENDPOINT",
		"OTEL_EXPORTER_OTLP_METRICS_ENDPOINT", "OTEL_EXPORTER_OTLP_LOGS_ENDPOINT",
		"OTEL_EXPORTER_OTLP_CERTIFICATE", "OTEL_EXPORTER_OTLP_CLIENT_CERTIFICATE",
		"OTEL_EXPORTER_OTLP_CLIENT_KEY", "OTEL_EXPORTER_OTLP_TRACES_CERTIFICATE",
		"OTEL_EXPORTER_OTLP_TRACES_CLIENT_CERTIFICATE", "OTEL_EXPORTER_OTLP_TRACES_CLIENT_KEY",
		"OTEL_EXPORTER_OTLP_METRICS_CERTIFICATE", "OTEL_EXPORTER_OTLP_METRICS_CLIENT_CERTIFICATE",
		"OTEL_EXPORTER_OTLP_METRICS_CLIENT_KEY", "OTEL_EXPORTER_OTLP_LOGS_CERTIFICATE",
		"OTEL_EXPORTER_OTLP_LOGS_CLIENT_CERTIFICATE", "OTEL_EXPORTER_OTLP_LOGS_CLIENT_KEY",
		"AIMEE_OBSERVABILITY_ALLOW_INSECURE_OTLP",
	} {
		t.Setenv(name, "")
	}
}

func TestOTLPDefaultLoopbackEndpointIsAllowed(t *testing.T) {
	clearOTLPEnvironment(t)
	if err := validateOTLPEnvironment(Config{OTLPEnabled: true}); err != nil {
		t.Fatal(err)
	}
}

func TestOTLPRejectsSecurityMaterialWithoutEndpoint(t *testing.T) {
	clearOTLPEnvironment(t)
	t.Setenv("OTEL_EXPORTER_OTLP_CERTIFICATE", "/not/read")
	err := validateOTLPEnvironment(Config{})
	if err == nil || !strings.Contains(err.Error(), "require an OTLP endpoint") {
		t.Fatalf("security material without endpoint error = %v", err)
	}
}

func TestOTLPRejectsPlaintextRemoteEndpoint(t *testing.T) {
	clearOTLPEnvironment(t)
	t.Setenv("OTEL_EXPORTER_OTLP_ENDPOINT", "http://collector.example:4318")
	err := validateOTLPEnvironment(Config{OTLPEnabled: true})
	if err == nil || !strings.Contains(err.Error(), "ALLOW_INSECURE_OTLP") {
		t.Fatalf("plaintext remote endpoint error = %v", err)
	}
	if err := validateOTLPEnvironment(Config{OTLPEnabled: true, AllowInsecureOTLP: true}); err != nil {
		t.Fatalf("explicit insecure endpoint rejected: %v", err)
	}
}

func TestOTLPAcceptsHTTPSAndSignalSpecificEndpoint(t *testing.T) {
	clearOTLPEnvironment(t)
	t.Setenv("OTEL_EXPORTER_OTLP_LOGS_ENDPOINT", "https://collector.example/v1/logs")
	if err := validateOTLPEnvironment(Config{OTLPLogsEnabled: true}); err != nil {
		t.Fatal(err)
	}
}

func TestOTLPRequiresClientCertificatePair(t *testing.T) {
	clearOTLPEnvironment(t)
	t.Setenv("OTEL_EXPORTER_OTLP_ENDPOINT", "https://collector.example")
	t.Setenv("OTEL_EXPORTER_OTLP_CLIENT_CERTIFICATE", "/client.crt")
	err := validateOTLPEnvironment(Config{OTLPEnabled: true})
	if err == nil || !strings.Contains(err.Error(), "configured together") {
		t.Fatalf("unpaired mTLS material error = %v", err)
	}
}

func TestOTLPRejectsPermissiveClientKey(t *testing.T) {
	clearOTLPEnvironment(t)
	directory := t.TempDir()
	certificate := filepath.Join(directory, "client.crt")
	key := filepath.Join(directory, "client.key")
	if err := os.WriteFile(certificate, []byte("certificate is validated by exporter"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(key, []byte("private key is validated by exporter"), 0o644); err != nil {
		t.Fatal(err)
	}
	t.Setenv("OTEL_EXPORTER_OTLP_ENDPOINT", "https://collector.example")
	t.Setenv("OTEL_EXPORTER_OTLP_CLIENT_CERTIFICATE", certificate)
	t.Setenv("OTEL_EXPORTER_OTLP_CLIENT_KEY", key)
	err := validateOTLPEnvironment(Config{OTLPEnabled: true})
	if err == nil || !strings.Contains(err.Error(), "permissions") {
		t.Fatalf("permissive client key error = %v", err)
	}
}

func TestOTLPRejectsInvalidInsecureOverride(t *testing.T) {
	clearOTLPEnvironment(t)
	t.Setenv("AIMEE_OBSERVABILITY_ALLOW_INSECURE_OTLP", "sometimes")
	err := validateOTLPEnvironment(Config{OTLPEnabled: true})
	if err == nil || !strings.Contains(err.Error(), "invalid syntax") {
		t.Fatalf("invalid policy value error = %v", err)
	}
}
