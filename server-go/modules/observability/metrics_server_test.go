package observability

import (
	"context"
	"errors"
	"io"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"
)

func TestMetricsServerDisabledByDefault(t *testing.T) {
	server, err := StartMetricsServer(MetricsServerConfig{}, http.NotFoundHandler())
	if err != nil {
		t.Fatal(err)
	}
	if server != nil {
		t.Fatal("empty endpoint started a listener")
	}
}

func TestMetricsServerRejectsSecurityWithoutEndpoint(t *testing.T) {
	_, err := StartMetricsServer(MetricsServerConfig{BearerTokenFile: "/not/read"}, http.NotFoundHandler())
	if err == nil {
		t.Fatal("security settings without endpoint were accepted")
	}
}

func TestMetricsServerTCP(t *testing.T) {
	server, err := StartMetricsServer(MetricsServerConfig{Endpoint: "tcp://127.0.0.1:0"}, http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_, _ = io.WriteString(w, "aimee_test_total 1\n")
	}))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = server.Shutdown(context.Background()) })

	response, err := http.Get("http://" + server.Addr().String() + "/metrics")
	if err != nil {
		t.Fatal(err)
	}
	defer response.Body.Close()
	body, err := io.ReadAll(response.Body)
	if err != nil {
		t.Fatal(err)
	}
	if response.StatusCode != http.StatusOK || !strings.Contains(string(body), "aimee_test_total 1") {
		t.Fatalf("metrics response status=%d body=%q", response.StatusCode, body)
	}
	notFound, err := http.Get("http://" + server.Addr().String() + "/")
	if err != nil {
		t.Fatal(err)
	}
	_ = notFound.Body.Close()
	if notFound.StatusCode != http.StatusNotFound {
		t.Fatalf("non-metrics route status = %d, want 404", notFound.StatusCode)
	}
}

func TestMetricsServerUnixSocketLifecycle(t *testing.T) {
	path := filepath.Join(t.TempDir(), "nested", "metrics.sock")
	server, err := StartMetricsServer(MetricsServerConfig{Endpoint: "unix://" + path}, http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_, _ = io.WriteString(w, "ok")
	}))
	if err != nil {
		t.Fatal(err)
	}
	info, err := os.Stat(path)
	if err != nil {
		t.Fatal(err)
	}
	if got := info.Mode().Perm(); got != 0o600 {
		t.Fatalf("socket mode = %o, want 600", got)
	}

	transport := &http.Transport{DialContext: func(ctx context.Context, _, _ string) (net.Conn, error) {
		return (&net.Dialer{}).DialContext(ctx, "unix", path)
	}}
	client := &http.Client{Transport: transport, Timeout: 2 * time.Second}
	response, err := client.Get("http://unix/metrics")
	if err != nil {
		t.Fatal(err)
	}
	_ = response.Body.Close()
	transport.CloseIdleConnections()

	if err := server.Shutdown(context.Background()); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(path); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("socket remains after shutdown: %v", err)
	}
}

func TestMetricsServerBearerAuthentication(t *testing.T) {
	token := "0123456789abcdef0123456789abcdef"
	tokenFile := filepath.Join(t.TempDir(), "metrics.token")
	if err := os.WriteFile(tokenFile, []byte(token+"\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	server, err := StartMetricsServer(MetricsServerConfig{
		Endpoint:        "tcp://127.0.0.1:0",
		BearerTokenFile: tokenFile,
	}, http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_, _ = io.WriteString(w, "ok")
	}))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = server.Shutdown(context.Background()) })

	endpoint := "http://" + server.Addr().String() + "/metrics"
	response, err := http.Get(endpoint)
	if err != nil {
		t.Fatal(err)
	}
	_ = response.Body.Close()
	if response.StatusCode != http.StatusUnauthorized {
		t.Fatalf("unauthenticated status = %d, want 401", response.StatusCode)
	}

	request, err := http.NewRequest(http.MethodGet, endpoint, nil)
	if err != nil {
		t.Fatal(err)
	}
	request.Header.Set("Authorization", "Bearer "+token)
	response, err = http.DefaultClient.Do(request)
	if err != nil {
		t.Fatal(err)
	}
	_ = response.Body.Close()
	if response.StatusCode != http.StatusOK {
		t.Fatalf("authenticated status = %d, want 200", response.StatusCode)
	}
}

func TestMetricsServerRejectsPublicPlaintext(t *testing.T) {
	_, err := StartMetricsServer(MetricsServerConfig{Endpoint: "tcp://0.0.0.0:0"}, http.NotFoundHandler())
	if err == nil || !strings.Contains(err.Error(), "non-loopback TCP requires TLS") {
		t.Fatalf("public plaintext error = %v", err)
	}
}

func TestMetricsServerRejectsPermissiveTokenFile(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("Unix file permissions are not available")
	}
	path := filepath.Join(t.TempDir(), "token")
	if err := os.WriteFile(path, []byte("0123456789abcdef0123456789abcdef"), 0o644); err != nil {
		t.Fatal(err)
	}
	_, err := StartMetricsServer(MetricsServerConfig{
		Endpoint: "tcp://127.0.0.1:0", BearerTokenFile: path,
	}, http.NotFoundHandler())
	if err == nil || !strings.Contains(err.Error(), "permissions") {
		t.Fatalf("permissive token file error = %v", err)
	}
}

func TestMetricsServerConfigServiceEnvironmentOverridesGeneric(t *testing.T) {
	t.Setenv("AIMEE_OBSERVABILITY_LISTEN", "tcp://127.0.0.1:9464")
	t.Setenv("AIMEE_SERVER_OBSERVABILITY_LISTEN", "unix:///tmp/aimee-metrics.sock")
	t.Setenv("AIMEE_OBSERVABILITY_TLS_CERTIFICATE", "/generic/cert")
	t.Setenv("AIMEE_SERVER_OBSERVABILITY_TLS_CERTIFICATE", "/server/cert")
	config := MetricsServerConfigFromEnv("AIMEE_SERVER")
	if config.Endpoint != "unix:///tmp/aimee-metrics.sock" || config.TLSCertificateFile != "/server/cert" {
		t.Fatalf("service overrides not applied: %+v", config)
	}
}

func TestMetricsServerRefusesNonSocketUnixPath(t *testing.T) {
	path := filepath.Join(t.TempDir(), "metrics.sock")
	if err := os.WriteFile(path, []byte("keep"), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := StartMetricsServer(MetricsServerConfig{Endpoint: "unix://" + path}, http.NotFoundHandler()); err == nil {
		t.Fatal("non-socket path was replaced")
	}
	contents, err := os.ReadFile(path)
	if err != nil || string(contents) != "keep" {
		t.Fatalf("existing file changed: contents=%q err=%v", contents, err)
	}
}

func TestMetricsEndpointValidation(t *testing.T) {
	for _, endpoint := range []string{
		"http://127.0.0.1:9464", "tcp://127.0.0.1:9464/metrics",
		"unix://relative.sock", "tcp://user:secret@127.0.0.1:9464",
	} {
		if _, _, err := parseMetricsEndpoint(endpoint); err == nil {
			t.Errorf("endpoint %q accepted", endpoint)
		}
	}
}
