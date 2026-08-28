# observability module

## Purpose and non-goals

`observability` provides one Go contract for structured logs, metrics, traces, Prometheus exposure,
HTTP instrumentation, flush, and shutdown. OpenTelemetry is its provider boundary. It does not make
authorization decisions, store audit evidence, export payload content, or force a vendor SDK on services.

## Public contracts

Consumers use `observability.Telemetry` for bounded `Logger`, `LocalLogger`, `Meter`, `Tracer`,
`MetricsHandler`, `HTTPHandler`, `ForceFlush`, and `Shutdown` behavior. `TapObserver` maps bus metadata
to content-free instruments; payload bytes are counted but never attached to logs or traces.

## Dependencies and consumers

- `module-runtime`: declares lifecycle, optional activation, ownership, and readiness for the reusable Go package.

Go server and KB services import the package for local or OTLP telemetry. The event-bus host can use
`TapObserver`; services retain control of listener placement and do not depend on a global provider.

## Providers and readiness

`server-go/modules/observability` implements independent OpenTelemetry providers and a private Prometheus
registry. Readiness requires valid exporter and metrics-listener configuration. Local structured logging
remains available when a remote collector is unreachable, while required TLS configuration fails closed.

## Configuration and activation

- `runtime_toggle.supported`: `true`; the package is disabled by default and can be enabled in images that expose an approved telemetry destination.

OTLP endpoint, protocol, headers, TLS, service identity, sampling, and metrics listener settings are
deployment-owned. Configure secrets through approved custody; a blank exporter keeps telemetry local.

## Surfaces

The operator surfaces are local structured logs, an optional `MetricsHandler` or managed metrics server,
and configured OTLP export. Application code uses the package interface. There is no generic event-body
browser, payload export route, or module-specific vendor configuration hidden from deployment review.

## Data and migrations

The module owns no durable application database and has no schema migrations. Metrics aggregate in
memory; spans and bounded logs are exported or dropped according to provider policy. Audit durability
belongs to `audit`, not this package. Restart resets process-local counters and exporter queues.

## Security and privacy

Use `LocalLogger` for prompts, credentials, content, paths, and errors that may contain them. Tap labels
come only from bounded wire vocabulary; sequence and source handle are log attributes, not metric labels.
TLS protects remote export, and headers are redacted from config and diagnostic output.

## Supported journeys

A service constructs one `Runtime`, requests component loggers, meters, and tracers, wraps HTTP handlers,
then flushes and shuts down within bounded contexts. A bus event reaches `TapObserver`, which records kind,
pattern, status, sequence, source, and payload length without copying the payload itself.

## Tests and failure behavior

Tests under `server-go/modules/observability` cover providers, OTLP configuration, tap mapping, metrics
servers, and TLS. Invalid endpoints, headers, certificates, or listener settings return errors. Exporter
failure is visible and bounded; it cannot crash request handling or redirect sensitive events to logs.

## Operational diagnostics

Inspect exporter readiness, queue/drop counts, last OTLP error, scrape health, TLS certificate state,
and `aimee.tap.exceptions`. Compare local logs with collector reception using trace IDs. Never enable
payload logging to debug export; use bounded metadata and the separate governed capture path.

## Compatibility

Telemetry interface methods, instrument names such as `aimee.tap.events`, bounded label vocabulary, and
privacy classification are compatibility contracts. Provider internals may change without consumers.
New labels require cardinality and privacy review; existing names are not silently repurposed.

## Extension and removal

Add an `OpenTelemetry` instrument with ownership, units, bounded attributes, privacy review, provider tests, and operator
documentation. Add exporters behind OpenTelemetry rather than vendor imports in consumers. Removing the
package requires migrating every import and preserving dashboards that depend on stable instrument names.
