# Observability module and tap adapter

## Purpose

`observability` is a disabled-by-default Aimee module implemented as a reusable Go package at
`server-go/modules/observability`. Its descriptor records ownership and dependency edges, while
consumers import it from the repository's existing `server-go` Go module. It does not add another
`go.mod` or create a separately versioned dependency.

The package gives every Go service one contract for structured logs, metrics, traces, Prometheus
scraping, HTTP instrumentation, flush, and shutdown. OpenTelemetry is the provider boundary. A
service does not import Datadog, Grafana, Loki, Tempo, or a vendor-specific metrics SDK.

## Public contract

Consumers depend on the package's `observability.Telemetry` interface:

- `Logger(component)` emits bounded, enterprise-safe structured logs locally and over OTLP.
- `LocalLogger(component)` emits only locally and is the required path for prompts, content,
  credentials, filesystem paths, and errors that might embed them.
- `Meter(component)` and `Tracer(component)` return standard OpenTelemetry interfaces.
- `MetricsHandler()` exposes the same instruments in Prometheus/OpenMetrics form.
- `HTTPHandler(operation, next)` adds HTTP server metrics and traces.
- `ForceFlush(ctx)` and `Shutdown(ctx)` provide bounded lifecycle hooks.

`Runtime` is the reference implementation. It owns independent providers rather than changing
OpenTelemetry globals, so multiple services and tests can coexist in one process.

## Tap adapter

`TapObserver` converts a `bus.Event` into:

- `aimee.tap.events`, labeled only with bounded wire vocabulary;
- `aimee.tap.payload.bytes`;
- `aimee.tap.exceptions` for invalid frames, overflow, and producer reaping;
- one content-free structured event log containing kind, pattern, placement, status, sequence,
  payload length, and source handle.

The payload is never attached to telemetry. Sequence and source handle are log attributes, not
metric labels, to prevent unbounded cardinality. The adapter is suitable for the Go bus host/tap;
it does not turn a subscribed client stream into a false full-stream tap.

## Providers

Prometheus instruments are retained in a private registry, but no scrape listener is opened by
default. OTLP/HTTP logs, metrics, and traces are also disabled unless one of the standard
`OTEL_EXPORTER_OTLP*_ENDPOINT` variables is set. The OpenTelemetry Collector then owns fan-out:

- Prometheus consumes the collector's Prometheus exporter.
- Grafana reads Prometheus, Loki, and Tempo; Loki and Tempo receive native OTLP.
- Datadog uses the collector's Datadog exporter and connector.
- Another backend is an exporter configuration change, not an application code change.

Both `aimee-server` and `aimee-kb` use the same opt-in scrape-listener contract. The listener is
metrics-only and is separate from each service's control/API surface. The Go server still wraps its
API handler with OTel HTTP instrumentation, but those measurements remain process-local when no
scrape or OTLP exporter is configured. Its default structured logger is local-only. Producers
explicitly use `Logger` when a schema is reviewed as enterprise-safe.

## Configuration

The package follows standard OpenTelemetry exporter variables, notably:

```text
OTEL_EXPORTER_OTLP_ENDPOINT=https://otel-collector.example:4318
OTEL_EXPORTER_OTLP_CERTIFICATE=/path/to/ca.pem
OTEL_EXPORTER_OTLP_CLIENT_CERTIFICATE=/path/to/client-chain.pem
OTEL_EXPORTER_OTLP_CLIENT_KEY=/path/to/client-key.pem
OTEL_RESOURCE_ATTRIBUTES=deployment.environment.name=production
```

Prometheus exposure is independently configured per process with the same flag or environment
variable:

```text
--observability-listen=tcp://127.0.0.1:9464
AIMEE_OBSERVABILITY_LISTEN=unix:///run/user/1000/aimee-server-metrics.sock
```

An empty value is disabled. Accepted values are exactly `tcp://host:port` and
`unix:///absolute/path`; `/metrics` is the only served route. When server and KB run on the same
host they must receive different ports or socket paths. `AIMEE_SERVER_OBSERVABILITY_LISTEN` and
`AIMEE_KB_OBSERVABILITY_LISTEN` override the generic variable for their respective processes. A
proxy can consume either transport and publish its own TLS/authenticated endpoint.

Unix sockets are created `0600`, refuse to overwrite any existing path, and are removed on clean
shutdown only if the path still names the socket the process created. Plain TCP is accepted only on
a loopback bind. A non-loopback TCP listener fails closed unless TLS and at least one client control
(mTLS or a file-backed bearer token) are configured. Configuring both mTLS and bearer requires both.
The same variables and flags apply to `aimee-server` and `aimee-kb`; service-prefixed variables
override generic ones. TLS private keys and bearer files may not grant group/other access.

OTLP/HTTP export follows the standard global and signal-specific OTLP exporter variables and their
precedence. Remote plaintext `http://` export is rejected by policy unless the operator explicitly
sets `AIMEE_OBSERVABILITY_ALLOW_INSECURE_OTLP=true`; loopback plaintext remains available for a
local collector or proxy. Production should use `https://`, a trusted CA, and preferably mTLS.

Service name/version are supplied by the host. `Config` also supports an injected local handler,
Prometheus registry, and metric export interval for tests and embedded services.

## Security and privacy

Enterprise log messages must be constant schema vocabulary. `Logger` redacts attributes whose keys
look content- or credential-bearing, including authorization, cookies, credentials, passwords,
secrets, tokens, prompts, content, bodies, arguments, results, and responses. Key redaction is a
backstop, not permission to pass arbitrary messages. Anything that might contain user text uses
`LocalLogger`.

Tap telemetry is content-free by type and uses bounded labels. Do not add event payloads, principal
identifiers, session IDs, request IDs, source handles, or sequence values to metric attributes.

## Tests and failure behavior

The focused package tests verify provider lifecycle, disabled-by-default exposure, TCP and Unix
listeners, Unix ownership/cleanup, Prometheus export, HTTP wrapping, explicit OTLP activation,
sensitive-key redaction, tap metric production, exception accounting, and non-disclosure of payload
bytes. OTLP batches are best-effort observability: export failure cannot block tap or service
correctness. Shutdown has a caller-provided deadline and attempts all three signal providers.

## Deployment

`deploy/observability/` contains collector configurations for the local Grafana stack and Datadog,
plus a Compose reference stack and provisioned Grafana data sources. These files are operational
examples, not embedded backends. See
[`docs/runbooks/observability-security.md`](../runbooks/observability-security.md) for the complete
production configuration, trust-material, rotation, Prometheus, Collector, and verification guide.
