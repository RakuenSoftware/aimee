package observability

import (
	"fmt"
	"net"
	"net/url"
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
)

type otlpSignal struct {
	name    string
	enabled bool
}

func envBool(name string) bool {
	value, _ := strconv.ParseBool(strings.TrimSpace(os.Getenv(name)))
	return value
}

func validateOTLPEnvironment(config Config) error {
	allowInsecure := config.AllowInsecureOTLP
	if raw := strings.TrimSpace(os.Getenv("AIMEE_OBSERVABILITY_ALLOW_INSECURE_OTLP")); raw != "" {
		parsed, err := strconv.ParseBool(raw)
		if err != nil {
			return fmt.Errorf("observability OTLP: AIMEE_OBSERVABILITY_ALLOW_INSECURE_OTLP: %w", err)
		}
		allowInsecure = parsed
	}

	signals := []otlpSignal{
		{name: "TRACES", enabled: config.OTLPEnabled || config.OTLPTracesEnabled},
		{name: "METRICS", enabled: config.OTLPEnabled || config.OTLPMetricsEnabled},
		{name: "LOGS", enabled: config.OTLPEnabled || config.OTLPLogsEnabled},
	}
	if !signals[0].enabled && !signals[1].enabled && !signals[2].enabled &&
		otlpSecurityMaterialConfigured() {
		return fmt.Errorf("observability OTLP: TLS/mTLS settings require an OTLP endpoint")
	}
	for _, signal := range signals {
		if !signal.enabled {
			continue
		}
		if err := validateOTLPSignal(signal.name, allowInsecure); err != nil {
			return err
		}
	}
	return nil
}

func otlpSecurityMaterialConfigured() bool {
	for _, signal := range []string{"", "TRACES_", "METRICS_", "LOGS_"} {
		for _, suffix := range []string{"CERTIFICATE", "CLIENT_CERTIFICATE", "CLIENT_KEY"} {
			if strings.TrimSpace(os.Getenv("OTEL_EXPORTER_OTLP_"+signal+suffix)) != "" {
				return true
			}
		}
	}
	return false
}

func validateOTLPSignal(signal string, allowInsecure bool) error {
	endpoint := signalEnv(signal, "ENDPOINT")
	if endpoint == "" {
		endpoint = "http://localhost:4318"
	}
	parsed, err := url.Parse(endpoint)
	if err != nil || parsed.Host == "" || (parsed.Scheme != "http" && parsed.Scheme != "https") {
		return fmt.Errorf("observability OTLP %s: endpoint must be an http or https URL", strings.ToLower(signal))
	}
	if parsed.User != nil || parsed.Fragment != "" {
		return fmt.Errorf("observability OTLP %s: endpoint must not contain credentials or a fragment", strings.ToLower(signal))
	}

	caFile := signalEnv(signal, "CERTIFICATE")
	clientCertificate := signalEnv(signal, "CLIENT_CERTIFICATE")
	clientKey := signalEnv(signal, "CLIENT_KEY")
	if (clientCertificate == "") != (clientKey == "") {
		return fmt.Errorf("observability OTLP %s: client certificate and key must be configured together", strings.ToLower(signal))
	}
	for purpose, path := range map[string]string{
		"CA certificate":     caFile,
		"client certificate": clientCertificate,
	} {
		if path != "" {
			if err := validateOTLPFile(path, purpose, false); err != nil {
				return fmt.Errorf("observability OTLP %s: %w", strings.ToLower(signal), err)
			}
		}
	}
	if clientKey != "" {
		if err := validateOTLPFile(clientKey, "client key", true); err != nil {
			return fmt.Errorf("observability OTLP %s: %w", strings.ToLower(signal), err)
		}
	}

	hasTLSMaterial := caFile != "" || clientCertificate != "" || clientKey != ""
	if parsed.Scheme == "http" {
		if hasTLSMaterial {
			return fmt.Errorf("observability OTLP %s: TLS material requires an https endpoint", strings.ToLower(signal))
		}
		if !otlpEndpointLoopback(parsed) && !allowInsecure {
			return fmt.Errorf("observability OTLP %s: plaintext non-loopback endpoint requires AIMEE_OBSERVABILITY_ALLOW_INSECURE_OTLP=true", strings.ToLower(signal))
		}
	}
	return nil
}

func signalEnv(signal, suffix string) string {
	if value := strings.TrimSpace(os.Getenv("OTEL_EXPORTER_OTLP_" + signal + "_" + suffix)); value != "" {
		return value
	}
	return strings.TrimSpace(os.Getenv("OTEL_EXPORTER_OTLP_" + suffix))
}

func otlpEndpointLoopback(endpoint *url.URL) bool {
	host := endpoint.Hostname()
	if strings.EqualFold(host, "localhost") {
		return true
	}
	ip := net.ParseIP(host)
	return ip != nil && ip.IsLoopback()
}

func validateOTLPFile(path, purpose string, private bool) error {
	clean := filepath.Clean(path)
	info, err := os.Stat(clean)
	if err != nil {
		return fmt.Errorf("stat %s: %w", purpose, err)
	}
	if !info.Mode().IsRegular() {
		return fmt.Errorf("%s must be a regular file", purpose)
	}
	if private && runtime.GOOS != "windows" && info.Mode().Perm()&0o077 != 0 {
		return fmt.Errorf("%s file permissions must not grant group or other access", purpose)
	}
	return nil
}
