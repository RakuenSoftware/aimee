# Aimee observability deployment

When OTLP is enabled, the application emits one OpenTelemetry stream. The collector owns vendor
routing; changing from Grafana to Datadog does not change service code.

## Local Grafana stack

```bash
export GRAFANA_ADMIN_PASSWORD='replace-me'
docker compose -f deploy/observability/docker-compose.yaml up -d
export OTEL_EXPORTER_OTLP_ENDPOINT=http://127.0.0.1:4318
export OTEL_EXPORTER_OTLP_PROTOCOL=http/protobuf
```

Restart the Go service after setting the endpoint. Prometheus is available on loopback port 9090
and Grafana on loopback port 3000. Loki stores enterprise-safe structured logs and Tempo stores
traces. The reference stack keeps only 24 hours and is not a production HA design.

No service scrape listener is enabled by default. The Compose stack receives metrics over OTLP and
scrapes the collector's Prometheus exporter.

## Direct Prometheus scrape

Both `aimee-server` and `aimee-kb` accept the same per-process listener setting:

```bash
# Give each process its own address when they share a host.
export AIMEE_SERVER_OBSERVABILITY_LISTEN=tcp://127.0.0.1:9464
export AIMEE_KB_OBSERVABILITY_LISTEN=tcp://127.0.0.1:9465
```

Prometheus can then scrape each process at `/metrics`. Loopback plaintext is intended for a local
Prometheus or proxy only. The generic
`AIMEE_OBSERVABILITY_LISTEN` variable is also accepted when process environments are configured
separately. The equivalent command-line option is
`--observability-listen=tcp://127.0.0.1:9464`.

For an owner-only Unix socket:

```bash
export AIMEE_OBSERVABILITY_LISTEN=unix:///run/user/1000/aimee-metrics.sock
curl --unix-socket /run/user/1000/aimee-metrics.sock http://localhost/metrics
```

Prometheus does not need to understand that Unix socket directly: a small local proxy can consume
it and provide the TCP/TLS/authentication policy appropriate to the deployment. Aimee refuses to
replace an existing socket path.

Both services implement the same native security policy. Non-loopback TCP requires a TLS server
certificate plus either a verified client certificate, a raw bearer from an owner-only file, or
both. Unix sockets rely on `0600` ownership unless a bearer file is additionally configured. See
[`docs/runbooks/observability-security.md`](../../docs/runbooks/observability-security.md) for all
variables, Prometheus examples, certificate requirements, and verification steps.

## Datadog

Run the OpenTelemetry Collector Contrib distribution with
`otel-collector-datadog.yaml` and inject credentials through the environment:

```bash
export DD_API_KEY='...'
export DD_SITE='datadoghq.com'
export OTEL_RECEIVER_GRPC_ENDPOINT=0.0.0.0:4317
export OTEL_RECEIVER_HTTP_ENDPOINT=0.0.0.0:4318
export OTEL_RECEIVER_TLS_CERTIFICATE=/run/pki/collector-chain.pem
export OTEL_RECEIVER_TLS_KEY=/run/pki/collector-key.pem
export OTEL_RECEIVER_TLS_CLIENT_CA=/run/pki/aimee-client-ca.pem
otelcol-contrib --config deploy/observability/otel-collector-datadog.yaml
```

Point Aimee at that collector using an `https://` `OTEL_EXPORTER_OTLP_ENDPOINT` and the matching
OTLP CA/client certificate variables. The Datadog connector
derives trace metrics and the exporter forwards logs, metrics, and traces. Never put the API key in
the YAML file or application environment.

## Production boundary

`otel-collector.yaml` and `docker-compose.yaml` are explicitly local-development examples: the
receiver binds container interfaces so sibling containers can reach it, while Compose publishes
OTLP and UI ports on host loopback only. Do not expose that receiver remotely.

For production, start with `otel-collector-production.yaml`. It requires mTLS on both OTLP receiver
protocols, uses a persistent sending queue, and verifies an mTLS upstream. The operator must supply
all endpoints, certificates, private keys, the storage directory, backend retention/legal-hold
policy, alerts, and capacity limits. `prometheus-secure.yaml.example` demonstrates HTTPS bearer and
mTLS scrape jobs. Validate the templates with the exact deployed Collector and Prometheus versions
before rollout.
