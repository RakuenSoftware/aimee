package observability

import (
	"context"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"

	"github.com/prometheus/client_golang/prometheus"
	"go.opentelemetry.io/otel/metric"
)

type captureHandler struct {
	mu      *sync.Mutex
	records *[]slog.Record
	attrs   []slog.Attr
}

func newCaptureHandler() *captureHandler {
	records := make([]slog.Record, 0)
	return &captureHandler{mu: &sync.Mutex{}, records: &records}
}

func (h *captureHandler) Enabled(context.Context, slog.Level) bool { return true }
func (h *captureHandler) Handle(_ context.Context, record slog.Record) error {
	copy := slog.NewRecord(record.Time, record.Level, record.Message, record.PC)
	copy.AddAttrs(h.attrs...)
	record.Attrs(func(attr slog.Attr) bool { copy.AddAttrs(attr); return true })
	h.mu.Lock()
	*h.records = append(*h.records, copy)
	h.mu.Unlock()
	return nil
}
func (h *captureHandler) WithAttrs(attrs []slog.Attr) slog.Handler {
	next := *h
	next.attrs = append(append([]slog.Attr{}, h.attrs...), attrs...)
	return &next
}
func (h *captureHandler) WithGroup(string) slog.Handler { return h }

func TestRuntimeMetricsLogsAndLifecycle(t *testing.T) {
	ctx := context.Background()
	local := newCaptureHandler()
	registry := prometheus.NewRegistry()
	runtime, err := New(ctx, Config{
		ServiceName: "test-service", LocalHandler: local, PrometheusRegistry: registry,
	})
	if err != nil {
		t.Fatal(err)
	}

	counter, err := runtime.Meter("test").Int64Counter("aimee.test.requests",
		metric.WithDescription("test requests"))
	if err != nil {
		t.Fatal(err)
	}
	counter.Add(ctx, 2)
	runtime.Logger("test").InfoContext(ctx, "safe event", "status", "ok")
	runtime.LocalLogger("test").InfoContext(ctx, "local event", "prompt", "private")

	recorder := httptest.NewRecorder()
	runtime.MetricsHandler().ServeHTTP(recorder, httptest.NewRequest(http.MethodGet, "/metrics", nil))
	if recorder.Code != http.StatusOK {
		t.Fatalf("metrics status = %d", recorder.Code)
	}
	if body := recorder.Body.String(); !strings.Contains(body, "aimee_test_requests_total{") ||
		!strings.Contains(body, "} 2") {
		t.Fatalf("missing counter in metrics:\n%s", body)
	}
	if got := len(*local.records); got != 2 {
		t.Fatalf("local log records = %d, want 2", got)
	}
	if err := runtime.ForceFlush(ctx); err != nil {
		t.Fatal(err)
	}
	if err := runtime.Shutdown(ctx); err != nil {
		t.Fatal(err)
	}
}

func TestHTTPHandlerWrapsService(t *testing.T) {
	runtime, err := New(context.Background(), Config{
		ServiceName: "http-test", LocalHandler: slog.NewTextHandler(io.Discard, nil),
	})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = runtime.Shutdown(context.Background()) })

	handler := runtime.HTTPHandler("aimee.http", http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusNoContent)
	}))
	recorder := httptest.NewRecorder()
	handler.ServeHTTP(recorder, httptest.NewRequest(http.MethodGet, "/v1/health", nil))
	if recorder.Code != http.StatusNoContent {
		t.Fatalf("status = %d", recorder.Code)
	}
}

func TestSensitiveKeysAreRedacted(t *testing.T) {
	tests := []string{"authorization", "api_token", "user.prompt", "tool_args", "response.body"}
	for _, key := range tests {
		attr := redactAttr(slog.String(key, "do-not-export"))
		if got := attr.Value.String(); got != "[REDACTED]" {
			t.Errorf("%s = %q", key, got)
		}
	}
	if attr := redactAttr(slog.String("event.kind", "3000")); attr.Value.String() != "3000" {
		t.Errorf("safe attribute was redacted")
	}
}

func TestConfigFromEnvRequiresExplicitEndpoint(t *testing.T) {
	for _, name := range []string{
		"OTEL_EXPORTER_OTLP_ENDPOINT", "OTEL_EXPORTER_OTLP_TRACES_ENDPOINT",
		"OTEL_EXPORTER_OTLP_METRICS_ENDPOINT", "OTEL_EXPORTER_OTLP_LOGS_ENDPOINT",
	} {
		t.Setenv(name, "")
	}
	if ConfigFromEnv("aimee", "test").OTLPEnabled {
		t.Fatal("OTLP enabled without an endpoint")
	}
	t.Setenv("OTEL_EXPORTER_OTLP_ENDPOINT", "http://collector:4318")
	if !ConfigFromEnv("aimee", "test").OTLPEnabled {
		t.Fatal("OTLP disabled with an endpoint")
	}
	t.Setenv("OTEL_EXPORTER_OTLP_ENDPOINT", "")
	t.Setenv("OTEL_EXPORTER_OTLP_LOGS_ENDPOINT", "http://collector:4318/v1/logs")
	config := ConfigFromEnv("aimee", "test")
	if config.OTLPEnabled || !config.OTLPLogsEnabled || config.OTLPTracesEnabled ||
		config.OTLPMetricsEnabled {
		t.Fatalf("signal-specific endpoint enabled the wrong exporters: %+v", config)
	}
}
