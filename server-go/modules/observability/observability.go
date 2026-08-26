// Package observability provides the shared telemetry contract for Aimee Go
// services. OpenTelemetry is the provider boundary; Prometheus is exposed as an
// http.Handler and OTLP carries logs, metrics, and traces to a collector.
package observability

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"strings"
	"time"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promhttp"
	"go.opentelemetry.io/contrib/bridges/otelslog"
	"go.opentelemetry.io/contrib/instrumentation/net/http/otelhttp"
	"go.opentelemetry.io/otel/attribute"
	"go.opentelemetry.io/otel/exporters/otlp/otlplog/otlploghttp"
	"go.opentelemetry.io/otel/exporters/otlp/otlpmetric/otlpmetrichttp"
	"go.opentelemetry.io/otel/exporters/otlp/otlptrace/otlptracehttp"
	otelprom "go.opentelemetry.io/otel/exporters/prometheus"
	otellog "go.opentelemetry.io/otel/log"
	"go.opentelemetry.io/otel/metric"
	logsdk "go.opentelemetry.io/otel/sdk/log"
	metricsdk "go.opentelemetry.io/otel/sdk/metric"
	"go.opentelemetry.io/otel/sdk/resource"
	tracesdk "go.opentelemetry.io/otel/sdk/trace"
	"go.opentelemetry.io/otel/semconv/v1.40.0"
	"go.opentelemetry.io/otel/trace"
)

// Telemetry is the lifecycle and instrumentation surface implemented by every
// Aimee observability provider. Services depend on this interface, not on a
// Datadog, Grafana, or Prometheus SDK.
type Telemetry interface {
	Logger(component string) *slog.Logger
	LocalLogger(component string) *slog.Logger
	Meter(component string) metric.Meter
	Tracer(component string) trace.Tracer
	MetricsHandler() http.Handler
	HTTPHandler(operation string, next http.Handler) http.Handler
	ForceFlush(context.Context) error
	Shutdown(context.Context) error
}

// Config is intentionally backend-neutral. Endpoint, headers, compression,
// certificates, and timeouts use the standard OTEL_EXPORTER_OTLP_* environment
// variables understood by the OTLP/HTTP exporters.
type Config struct {
	ServiceName        string
	ServiceNamespace   string
	ServiceVersion     string
	Environment        string
	OTLPEnabled        bool
	OTLPTracesEnabled  bool
	OTLPMetricsEnabled bool
	OTLPLogsEnabled    bool
	AllowInsecureOTLP  bool
	MetricInterval     time.Duration
	LocalHandler       slog.Handler
	PrometheusRegistry *prometheus.Registry
}

// ConfigFromEnv applies stable Aimee resource defaults and enables OTLP only
// when a standard global or signal-specific endpoint is present.
func ConfigFromEnv(serviceName, serviceVersion string) Config {
	return Config{
		ServiceName:        serviceName,
		ServiceNamespace:   "aimee",
		ServiceVersion:     serviceVersion,
		Environment:        os.Getenv("AIMEE_ENVIRONMENT"),
		OTLPEnabled:        envConfigured("OTEL_EXPORTER_OTLP_ENDPOINT"),
		OTLPTracesEnabled:  envConfigured("OTEL_EXPORTER_OTLP_TRACES_ENDPOINT"),
		OTLPMetricsEnabled: envConfigured("OTEL_EXPORTER_OTLP_METRICS_ENDPOINT"),
		OTLPLogsEnabled:    envConfigured("OTEL_EXPORTER_OTLP_LOGS_ENDPOINT"),
		AllowInsecureOTLP:  envBool("AIMEE_OBSERVABILITY_ALLOW_INSECURE_OTLP"),
		MetricInterval:     15 * time.Second,
	}
}

func envConfigured(name string) bool {
	return strings.TrimSpace(os.Getenv(name)) != ""
}

// Runtime is the reference Telemetry implementation.
type Runtime struct {
	resource *resource.Resource
	traces   *tracesdk.TracerProvider
	metrics  *metricsdk.MeterProvider
	logs     *logsdk.LoggerProvider
	registry *prometheus.Registry
	local    slog.Handler
	shutdown []func(context.Context) error
}

var _ Telemetry = (*Runtime)(nil)

// New constructs independent providers without changing OpenTelemetry's global
// providers. This lets tests and several services coexist in one process.
func New(ctx context.Context, cfg Config) (*Runtime, error) {
	if strings.TrimSpace(cfg.ServiceName) == "" {
		return nil, errors.New("observability: service name is required")
	}
	if err := validateOTLPEnvironment(cfg); err != nil {
		return nil, err
	}
	if cfg.ServiceNamespace == "" {
		cfg.ServiceNamespace = "aimee"
	}
	if cfg.MetricInterval <= 0 {
		cfg.MetricInterval = 15 * time.Second
	}
	if cfg.LocalHandler == nil {
		cfg.LocalHandler = slog.NewJSONHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelInfo})
	}
	if cfg.PrometheusRegistry == nil {
		cfg.PrometheusRegistry = prometheus.NewRegistry()
	}

	attrs := []attribute.KeyValue{
		semconv.ServiceNameKey.String(cfg.ServiceName),
		semconv.ServiceNamespaceKey.String(cfg.ServiceNamespace),
	}
	if cfg.ServiceVersion != "" {
		attrs = append(attrs, semconv.ServiceVersionKey.String(cfg.ServiceVersion))
	}
	if cfg.Environment != "" {
		attrs = append(attrs, semconv.DeploymentEnvironmentNameKey.String(cfg.Environment))
	}
	res, err := resource.Merge(resource.Default(), resource.NewSchemaless(attrs...))
	if err != nil {
		return nil, fmt.Errorf("observability resource: %w", err)
	}

	runtime := &Runtime{resource: res, registry: cfg.PrometheusRegistry, local: cfg.LocalHandler}
	promReader, err := otelprom.New(otelprom.WithRegisterer(cfg.PrometheusRegistry))
	if err != nil {
		return nil, fmt.Errorf("observability prometheus: %w", err)
	}
	metricOptions := []metricsdk.Option{
		metricsdk.WithResource(res),
		metricsdk.WithReader(promReader),
	}
	traceOptions := []tracesdk.TracerProviderOption{tracesdk.WithResource(res)}
	logOptions := []logsdk.LoggerProviderOption{logsdk.WithResource(res)}

	var exporterCleanup []func(context.Context) error
	cleanupExporters := func() {
		for _, cleanup := range exporterCleanup {
			_ = cleanup(ctx)
		}
	}
	if cfg.OTLPEnabled || cfg.OTLPTracesEnabled {
		traceExporter, exportErr := otlptracehttp.New(ctx)
		if exportErr != nil {
			return nil, fmt.Errorf("observability OTLP traces: %w", exportErr)
		}
		exporterCleanup = append(exporterCleanup, traceExporter.Shutdown)
		traceOptions = append(traceOptions, tracesdk.WithBatcher(traceExporter))
	}
	if cfg.OTLPEnabled || cfg.OTLPMetricsEnabled {
		metricExporter, exportErr := otlpmetrichttp.New(ctx)
		if exportErr != nil {
			cleanupExporters()
			return nil, fmt.Errorf("observability OTLP metrics: %w", exportErr)
		}
		exporterCleanup = append(exporterCleanup, metricExporter.Shutdown)
		metricOptions = append(metricOptions, metricsdk.WithReader(
			metricsdk.NewPeriodicReader(metricExporter, metricsdk.WithInterval(cfg.MetricInterval))))
	}
	if cfg.OTLPEnabled || cfg.OTLPLogsEnabled {
		logExporter, exportErr := otlploghttp.New(ctx)
		if exportErr != nil {
			cleanupExporters()
			return nil, fmt.Errorf("observability OTLP logs: %w", exportErr)
		}
		exporterCleanup = append(exporterCleanup, logExporter.Shutdown)
		logOptions = append(logOptions, logsdk.WithProcessor(logsdk.NewBatchProcessor(logExporter)))
	}

	runtime.traces = tracesdk.NewTracerProvider(traceOptions...)
	runtime.metrics = metricsdk.NewMeterProvider(metricOptions...)
	runtime.logs = logsdk.NewLoggerProvider(logOptions...)
	runtime.shutdown = []func(context.Context) error{
		runtime.logs.Shutdown,
		runtime.metrics.Shutdown,
		runtime.traces.Shutdown,
	}
	return runtime, nil
}

// Logger returns a local and OTLP logger for content-free, bounded structured
// events. Sensitive-looking attribute keys are redacted before OTLP export.
func (r *Runtime) Logger(component string) *slog.Logger {
	exported := redactHandler{next: otelslog.NewHandler(component, otelslog.WithLoggerProvider(r.logs))}
	return slog.New(fanoutHandler{r.local, exported})
}

// LocalLogger never exports. Use it for prompts, user content, filesystem paths,
// credentials, or errors that may embed any of those values.
func (r *Runtime) LocalLogger(_ string) *slog.Logger { return slog.New(r.local) }

func (r *Runtime) Meter(component string) metric.Meter  { return r.metrics.Meter(component) }
func (r *Runtime) Tracer(component string) trace.Tracer { return r.traces.Tracer(component) }

func (r *Runtime) MetricsHandler() http.Handler {
	return promhttp.HandlerFor(r.registry, promhttp.HandlerOpts{EnableOpenMetrics: true})
}

func (r *Runtime) HTTPHandler(operation string, next http.Handler) http.Handler {
	return otelhttp.NewHandler(next, operation,
		otelhttp.WithTracerProvider(r.traces),
		otelhttp.WithMeterProvider(r.metrics))
}

func (r *Runtime) ForceFlush(ctx context.Context) error {
	return errors.Join(r.logs.ForceFlush(ctx), r.metrics.ForceFlush(ctx), r.traces.ForceFlush(ctx))
}

func (r *Runtime) Shutdown(ctx context.Context) error {
	var errs []error
	for _, shutdown := range r.shutdown {
		errs = append(errs, shutdown(ctx))
	}
	return errors.Join(errs...)
}

// LoggerProvider exposes the standard OTel interface for adapters that need to
// bridge another logging facade without depending on Runtime internals.
func (r *Runtime) LoggerProvider() otellog.LoggerProvider { return r.logs }
