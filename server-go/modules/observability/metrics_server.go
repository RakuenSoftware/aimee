package observability

import (
	"context"
	"crypto/subtle"
	"crypto/tls"
	"crypto/x509"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"net"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"time"
)

const (
	minBearerTokenBytes = 32
	maxBearerTokenBytes = 512
)

// MetricsServerConfig controls the optional Prometheus scrape listener. An
// empty Endpoint disables the listener and makes every other field invalid.
type MetricsServerConfig struct {
	Endpoint           string
	TLSCertificateFile string
	TLSKeyFile         string
	TLSClientCAFile    string
	BearerTokenFile    string
}

// MetricsServerConfigFromEnv reads the generic AIMEE_OBSERVABILITY_* settings,
// with optional service-specific overrides such as AIMEE_SERVER_OBSERVABILITY_*.
func MetricsServerConfigFromEnv(servicePrefix string) MetricsServerConfig {
	return MetricsServerConfig{
		Endpoint:           observabilityEnv(servicePrefix, "LISTEN"),
		TLSCertificateFile: observabilityEnv(servicePrefix, "TLS_CERTIFICATE"),
		TLSKeyFile:         observabilityEnv(servicePrefix, "TLS_KEY"),
		TLSClientCAFile:    observabilityEnv(servicePrefix, "TLS_CLIENT_CA"),
		BearerTokenFile:    observabilityEnv(servicePrefix, "BEARER_TOKEN_FILE"),
	}
}

func observabilityEnv(servicePrefix, suffix string) string {
	if servicePrefix != "" {
		if value := strings.TrimSpace(os.Getenv(servicePrefix + "_OBSERVABILITY_" + suffix)); value != "" {
			return value
		}
	}
	return strings.TrimSpace(os.Getenv("AIMEE_OBSERVABILITY_" + suffix))
}

// MetricsServer exposes a Prometheus scrape handler on an explicitly configured
// TCP or Unix socket. No listener is created when the endpoint is empty.
type MetricsServer struct {
	server     *http.Server
	listener   net.Listener
	socketPath string
	socketInfo fs.FileInfo
	done       chan error
	token      []byte
	closeOnce  sync.Once
}

// StartMetricsServer starts a dedicated metrics-only HTTP server. Endpoint must
// be empty (disabled), tcp://host:port, or unix:///absolute/path.
//
// Plain HTTP is permitted only on a loopback TCP address or an owner-only Unix
// socket, allowing a local authenticating proxy. A non-loopback TCP listener
// requires TLS plus either mTLS or a bearer token. When mTLS and a bearer token
// are both configured, clients must satisfy both controls.
func StartMetricsServer(config MetricsServerConfig, metrics http.Handler) (*MetricsServer, error) {
	trimMetricsConfig(&config)
	if config.Endpoint == "" {
		if config.hasSecurityMaterial() {
			return nil, errors.New("observability metrics: security settings require an endpoint")
		}
		return nil, nil
	}
	if metrics == nil {
		return nil, errors.New("observability metrics: handler is required")
	}

	network, address, err := parseMetricsEndpoint(config.Endpoint)
	if err != nil {
		return nil, err
	}
	tlsConfig, err := loadMetricsTLS(config)
	if err != nil {
		return nil, err
	}
	token, err := loadBearerToken(config.BearerTokenFile)
	if err != nil {
		return nil, err
	}
	cleanupToken := true
	defer func() {
		if cleanupToken {
			clear(token)
		}
	}()

	listener, socketInfo, err := listenMetrics(network, address)
	if err != nil {
		return nil, fmt.Errorf("observability metrics listen %q: %w", config.Endpoint, err)
	}
	closeListener := true
	defer func() {
		if closeListener {
			_ = listener.Close()
			if network == "unix" {
				_ = os.Remove(address)
			}
		}
	}()

	if network == "tcp" && !listenerLoopback(listener) {
		if tlsConfig == nil {
			return nil, errors.New("observability metrics: non-loopback TCP requires TLS")
		}
		if config.TLSClientCAFile == "" && len(token) == 0 {
			return nil, errors.New("observability metrics: non-loopback TCP requires mTLS or a bearer token")
		}
	}
	if network == "unix" && tlsConfig != nil {
		return nil, errors.New("observability metrics: TLS is supported only for TCP listeners")
	}
	if tlsConfig != nil {
		listener = tls.NewListener(listener, tlsConfig)
	}

	mux := http.NewServeMux()
	mux.Handle("GET /metrics", metrics)
	var handler http.Handler = http.HandlerFunc(func(w http.ResponseWriter, request *http.Request) {
		w.Header().Set("Cache-Control", "no-store")
		w.Header().Set("X-Content-Type-Options", "nosniff")
		mux.ServeHTTP(w, request)
	})
	if len(token) != 0 {
		handler = bearerAuthHandler(token, handler)
	}

	server := &MetricsServer{
		server: &http.Server{
			Handler:           handler,
			ReadHeaderTimeout: 5 * time.Second,
			ReadTimeout:       10 * time.Second,
			WriteTimeout:      15 * time.Second,
			IdleTimeout:       30 * time.Second,
			MaxHeaderBytes:    16 * 1024,
			TLSConfig:         tlsConfig,
		},
		listener: listener,
		done:     make(chan error, 1),
		token:    token,
	}
	if network == "unix" {
		server.socketPath = address
		server.socketInfo = socketInfo
	}
	go func() {
		err := server.server.Serve(listener)
		if errors.Is(err, http.ErrServerClosed) || errors.Is(err, net.ErrClosed) {
			err = nil
		}
		server.done <- err
		close(server.done)
	}()
	cleanupToken = false
	closeListener = false
	return server, nil
}

func trimMetricsConfig(config *MetricsServerConfig) {
	config.Endpoint = strings.TrimSpace(config.Endpoint)
	config.TLSCertificateFile = strings.TrimSpace(config.TLSCertificateFile)
	config.TLSKeyFile = strings.TrimSpace(config.TLSKeyFile)
	config.TLSClientCAFile = strings.TrimSpace(config.TLSClientCAFile)
	config.BearerTokenFile = strings.TrimSpace(config.BearerTokenFile)
}

func (config MetricsServerConfig) hasSecurityMaterial() bool {
	return config.TLSCertificateFile != "" || config.TLSKeyFile != "" ||
		config.TLSClientCAFile != "" || config.BearerTokenFile != ""
}

func loadMetricsTLS(config MetricsServerConfig) (*tls.Config, error) {
	hasCertificate := config.TLSCertificateFile != ""
	hasKey := config.TLSKeyFile != ""
	if hasCertificate != hasKey {
		return nil, errors.New("observability metrics: TLS certificate and key must be configured together")
	}
	if config.TLSClientCAFile != "" && !hasCertificate {
		return nil, errors.New("observability metrics: client CA requires a TLS certificate and key")
	}
	if !hasCertificate {
		return nil, nil
	}
	if err := validateRegularFile(config.TLSCertificateFile, "TLS certificate"); err != nil {
		return nil, err
	}
	if err := validatePrivateFile(config.TLSKeyFile, "TLS key"); err != nil {
		return nil, err
	}
	certificate, err := tls.LoadX509KeyPair(config.TLSCertificateFile, config.TLSKeyFile)
	if err != nil {
		return nil, fmt.Errorf("observability metrics: load TLS identity: %w", err)
	}
	leaf, err := x509.ParseCertificate(certificate.Certificate[0])
	if err != nil {
		return nil, fmt.Errorf("observability metrics: parse TLS certificate: %w", err)
	}
	now := time.Now()
	if now.Before(leaf.NotBefore) || now.After(leaf.NotAfter) {
		return nil, errors.New("observability metrics: TLS certificate is not currently valid")
	}

	tlsConfig := &tls.Config{
		Certificates: []tls.Certificate{certificate},
		MinVersion:   tls.VersionTLS12,
		NextProtos:   []string{"h2", "http/1.1"},
	}
	if config.TLSClientCAFile != "" {
		if err := validateRegularFile(config.TLSClientCAFile, "client CA"); err != nil {
			return nil, err
		}
		caPEM, err := os.ReadFile(config.TLSClientCAFile)
		if err != nil {
			return nil, fmt.Errorf("observability metrics: read client CA: %w", err)
		}
		clientCAs := x509.NewCertPool()
		if !clientCAs.AppendCertsFromPEM(caPEM) {
			return nil, errors.New("observability metrics: client CA contains no certificates")
		}
		tlsConfig.ClientCAs = clientCAs
		tlsConfig.ClientAuth = tls.RequireAndVerifyClientCert
	}
	return tlsConfig, nil
}

func loadBearerToken(path string) ([]byte, error) {
	if path == "" {
		return nil, nil
	}
	if err := validatePrivateFile(path, "bearer token"); err != nil {
		return nil, err
	}
	file, err := os.Open(path)
	if err != nil {
		return nil, fmt.Errorf("observability metrics: open bearer token: %w", err)
	}
	defer file.Close()
	token, err := io.ReadAll(io.LimitReader(file, maxBearerTokenBytes+2))
	if err != nil {
		return nil, fmt.Errorf("observability metrics: read bearer token: %w", err)
	}
	token = trimOneLineEnding(token)
	if len(token) < minBearerTokenBytes || len(token) > maxBearerTokenBytes {
		clear(token)
		return nil, fmt.Errorf("observability metrics: bearer token must be %d-%d bytes", minBearerTokenBytes, maxBearerTokenBytes)
	}
	for _, character := range token {
		if character < 0x21 || character > 0x7e {
			clear(token)
			return nil, errors.New("observability metrics: bearer token must contain visible ASCII without whitespace")
		}
	}
	return token, nil
}

func trimOneLineEnding(value []byte) []byte {
	if len(value) > 0 && value[len(value)-1] == '\n' {
		value = value[:len(value)-1]
		if len(value) > 0 && value[len(value)-1] == '\r' {
			value = value[:len(value)-1]
		}
	}
	return value
}

func validatePrivateFile(path, purpose string) error {
	if err := validateRegularFile(path, purpose); err != nil {
		return err
	}
	info, err := os.Stat(path)
	if err != nil {
		return fmt.Errorf("observability metrics: stat %s: %w", purpose, err)
	}
	if runtime.GOOS != "windows" && info.Mode().Perm()&0o077 != 0 {
		return fmt.Errorf("observability metrics: %s file permissions must not grant group or other access", purpose)
	}
	return nil
}

func validateRegularFile(path, purpose string) error {
	info, err := os.Stat(path)
	if err != nil {
		return fmt.Errorf("observability metrics: stat %s: %w", purpose, err)
	}
	if !info.Mode().IsRegular() {
		return fmt.Errorf("observability metrics: %s must be a regular file", purpose)
	}
	return nil
}

func bearerAuthHandler(expected []byte, next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, request *http.Request) {
		values := request.Header.Values("Authorization")
		if len(values) != 1 || !strings.HasPrefix(values[0], "Bearer ") {
			w.Header().Set("WWW-Authenticate", `Bearer realm="aimee-observability"`)
			http.Error(w, "unauthorized", http.StatusUnauthorized)
			return
		}
		presented := []byte(strings.TrimPrefix(values[0], "Bearer "))
		if len(presented) != len(expected) || subtle.ConstantTimeCompare(presented, expected) != 1 {
			w.Header().Set("WWW-Authenticate", `Bearer realm="aimee-observability"`)
			http.Error(w, "unauthorized", http.StatusUnauthorized)
			return
		}
		next.ServeHTTP(w, request)
	})
}

// Addr returns the bound address. It is useful when tcp://127.0.0.1:0 is used
// in tests or by an embedding process that publishes the selected port itself.
func (s *MetricsServer) Addr() net.Addr {
	if s == nil || s.listener == nil {
		return nil
	}
	return s.listener.Addr()
}

// Done reports an unexpected serving failure. It yields nil after Shutdown.
func (s *MetricsServer) Done() <-chan error {
	if s == nil {
		closed := make(chan error)
		close(closed)
		return closed
	}
	return s.done
}

// Shutdown stops serving, clears bearer material, and removes a Unix socket
// only when the path still identifies the socket this server created.
func (s *MetricsServer) Shutdown(ctx context.Context) error {
	if s == nil {
		return nil
	}
	var shutdownErr error
	s.closeOnce.Do(func() {
		shutdownErr = s.server.Shutdown(ctx)
		if shutdownErr != nil {
			shutdownErr = errors.Join(shutdownErr, s.server.Close())
		}
		serveErr := <-s.done
		shutdownErr = errors.Join(shutdownErr, serveErr, s.removeOwnedSocket())
		clear(s.token)
		s.token = nil
	})
	return shutdownErr
}

func parseMetricsEndpoint(endpoint string) (network, address string, err error) {
	parsed, err := url.Parse(endpoint)
	if err != nil {
		return "", "", fmt.Errorf("observability metrics endpoint: %w", err)
	}
	if parsed.User != nil || parsed.RawQuery != "" || parsed.Fragment != "" {
		return "", "", errors.New("observability metrics endpoint must not contain credentials, query, or fragment")
	}
	switch parsed.Scheme {
	case "tcp":
		if parsed.Host == "" || parsed.Path != "" {
			return "", "", errors.New("observability metrics TCP endpoint must be tcp://host:port")
		}
		if _, err := net.ResolveTCPAddr("tcp", parsed.Host); err != nil {
			return "", "", fmt.Errorf("observability metrics TCP endpoint: %w", err)
		}
		return "tcp", parsed.Host, nil
	case "unix":
		if parsed.Host != "" || !filepath.IsAbs(parsed.Path) {
			return "", "", errors.New("observability metrics Unix endpoint must be unix:///absolute/path")
		}
		return "unix", parsed.Path, nil
	default:
		return "", "", errors.New("observability metrics endpoint scheme must be tcp or unix")
	}
}

func listenMetrics(network, address string) (net.Listener, fs.FileInfo, error) {
	if network == "tcp" {
		listener, err := net.Listen("tcp", address)
		return listener, nil, err
	}
	if err := os.MkdirAll(filepath.Dir(address), 0o700); err != nil {
		return nil, nil, err
	}
	if info, err := os.Lstat(address); err == nil {
		return nil, nil, fmt.Errorf("refusing to replace existing path %s (%s)", address, info.Mode())
	} else if !errors.Is(err, os.ErrNotExist) {
		return nil, nil, err
	}
	listener, err := net.Listen("unix", address)
	if err != nil {
		return nil, nil, err
	}
	if err := os.Chmod(address, 0o600); err != nil {
		_ = listener.Close()
		_ = os.Remove(address)
		return nil, nil, err
	}
	info, err := os.Lstat(address)
	if err != nil {
		_ = listener.Close()
		_ = os.Remove(address)
		return nil, nil, err
	}
	return listener, info, nil
}

func listenerLoopback(listener net.Listener) bool {
	address, ok := listener.Addr().(*net.TCPAddr)
	return ok && address.IP.IsLoopback()
}

func (s *MetricsServer) removeOwnedSocket() error {
	if s.socketPath == "" || s.socketInfo == nil {
		return nil
	}
	current, err := os.Lstat(s.socketPath)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	if !os.SameFile(s.socketInfo, current) {
		return nil
	}
	return os.Remove(s.socketPath)
}
