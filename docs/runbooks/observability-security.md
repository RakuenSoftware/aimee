# Production observability security

This runbook covers the two independent observability paths:

1. A Prometheus-compatible `/metrics` listener on each Aimee process.
2. OTLP/HTTP export of logs, metrics, and traces from Go services to a collector.

Both paths are disabled by default. Enabling a scrape listener does not enable OTLP, and setting an
OTLP endpoint does not open a scrape listener. Grafana, Datadog, Elastic, and other backends should
normally consume from Prometheus or an OpenTelemetry Collector rather than from a vendor-specific
API in Aimee.

## Security contract

| Bind/export path | Required controls | Intended use |
| --- | --- | --- |
| No endpoint | None; no listener/exporter exists | Default |
| `unix:///absolute/path` scrape | Socket is created `0600` | Same-user local proxy |
| Loopback `tcp://127.0.0.1:PORT` or `[::1]` scrape | Plain HTTP allowed; TLS/auth optional | Local Prometheus or proxy |
| Non-loopback TCP scrape | TLS server identity plus mTLS, bearer, or both | Remote Prometheus |
| Loopback `http://` OTLP | Allowed | Local collector/sidecar |
| Remote `https://` OTLP | Server certificate validation; mTLS recommended | Remote collector |
| Remote `http://` OTLP | Explicit insecure override required | Exceptional trusted-network migration only |

The process refuses to start the relevant listener/exporter when configuration is incomplete,
unsafe, or attached to a disabled endpoint. TLS uses a minimum of TLS 1.2. If both a client CA and
a bearer file are configured for a scrape listener, a client must pass both checks.

## Scrape-listener settings

Each generic variable can be overridden per process by inserting `SERVER_` or `KB_` after
`AIMEE_`. For example, `AIMEE_SERVER_OBSERVABILITY_TLS_KEY` overrides
`AIMEE_OBSERVABILITY_TLS_KEY` in `aimee-server`.

| Generic environment variable | Command-line flag | Meaning |
| --- | --- | --- |
| `AIMEE_OBSERVABILITY_LISTEN` | `--observability-listen=` | Empty, `tcp://host:port`, or `unix:///absolute/path` |
| `AIMEE_OBSERVABILITY_TLS_CERTIFICATE` | `--observability-tls-certificate=` | PEM server certificate chain |
| `AIMEE_OBSERVABILITY_TLS_KEY` | `--observability-tls-key=` | Matching PEM private key |
| `AIMEE_OBSERVABILITY_TLS_CLIENT_CA` | `--observability-tls-client-ca=` | PEM CA used to require and verify client certificates |
| `AIMEE_OBSERVABILITY_BEARER_TOKEN_FILE` | `--observability-bearer-token-file=` | Raw bearer secret file |

Use separate listen addresses when server and KB share a host. Never pass a raw secret on the
command line. The token file must be a regular file, contain 32–512 visible ASCII bytes with at
most one final line ending, and grant no group/other permissions. The TLS private key has the same
permission restriction. Certificate and CA files may be world-readable if organizational policy
allows it.

The KB also accepts the existing `telemetry.metrics_token` SHA-256 hash. A token file is preferred
for a new deployment because it gives server and KB the same interface. If both are configured,
the token file must hash to that configured value. Rotate by atomically replacing mounted files and
restarting the process; listener credentials are intentionally loaded once at startup.

Create a bearer secret without putting it in shell history:

```bash
umask 077
openssl rand -base64 48 > /run/secrets/aimee-observability-token
chmod 0600 /run/secrets/aimee-observability-token
```

### HTTPS plus bearer

```bash
export AIMEE_SERVER_OBSERVABILITY_LISTEN=tcp://0.0.0.0:9464
export AIMEE_SERVER_OBSERVABILITY_TLS_CERTIFICATE=/run/pki/server-chain.pem
export AIMEE_SERVER_OBSERVABILITY_TLS_KEY=/run/pki/server-key.pem
export AIMEE_SERVER_OBSERVABILITY_BEARER_TOKEN_FILE=/run/secrets/aimee-observability-token
```

The certificate must be currently valid and contain the DNS name or IP used by Prometheus in its
subject alternative names. Protect the private key with mode `0400` or `0600`.

Prometheus scrape configuration:

```yaml
scrape_configs:
  - job_name: aimee-server
    scheme: https
    static_configs:
      - targets: [aimee-server.example:9464]
    authorization:
      type: Bearer
      credentials_file: /run/secrets/aimee-observability-token
    tls_config:
      ca_file: /run/pki/observability-ca.pem
      server_name: aimee-server.example
      min_version: TLS12
```

### Mutual TLS

Set the client CA in addition to the server identity:

```bash
export AIMEE_KB_OBSERVABILITY_LISTEN=tcp://0.0.0.0:9465
export AIMEE_KB_OBSERVABILITY_TLS_CERTIFICATE=/run/pki/kb-chain.pem
export AIMEE_KB_OBSERVABILITY_TLS_KEY=/run/pki/kb-key.pem
export AIMEE_KB_OBSERVABILITY_TLS_CLIENT_CA=/run/pki/prometheus-client-ca.pem
```

Prometheus then presents a client identity:

```yaml
scrape_configs:
  - job_name: aimee-kb
    scheme: https
    static_configs:
      - targets: [aimee-kb.example:9465]
    tls_config:
      ca_file: /run/pki/observability-ca.pem
      cert_file: /run/pki/prometheus-client-chain.pem
      key_file: /run/pki/prometheus-client-key.pem
      server_name: aimee-kb.example
      min_version: TLS12
```

Add `authorization.credentials_file` to this job if both mTLS and bearer are configured. Aimee
verifies certificate chain and validity through the configured CA; issue dedicated client
certificates with a client-auth extended key usage and short validity.

### Unix socket or loopback proxy

```bash
export AIMEE_KB_OBSERVABILITY_LISTEN=unix:///run/aimee/kb-metrics.sock
curl --fail --unix-socket /run/aimee/kb-metrics.sock http://localhost/metrics
```

Aimee creates missing parent directories with mode `0700`, refuses to replace an existing path,
and only removes the socket it created. Run the proxy under the same OS identity (or deliberately
arrange access before startup), then apply TLS and authentication at the proxy's remote boundary.
For a container sidecar, `tcp://127.0.0.1:9464` is usually simpler when both processes share a
network namespace.

## OTLP/HTTP exporter settings

Aimee uses the OpenTelemetry OTLP/HTTP exporters and the standard global and signal-specific
configuration precedence. A signal-specific variable wins over the global form.

| Setting | Global variable | Signal-specific example |
| --- | --- | --- |
| Endpoint | `OTEL_EXPORTER_OTLP_ENDPOINT` | `OTEL_EXPORTER_OTLP_TRACES_ENDPOINT` |
| Trusted server CA | `OTEL_EXPORTER_OTLP_CERTIFICATE` | `OTEL_EXPORTER_OTLP_LOGS_CERTIFICATE` |
| Client certificate | `OTEL_EXPORTER_OTLP_CLIENT_CERTIFICATE` | `OTEL_EXPORTER_OTLP_METRICS_CLIENT_CERTIFICATE` |
| Client private key | `OTEL_EXPORTER_OTLP_CLIENT_KEY` | `OTEL_EXPORTER_OTLP_TRACES_CLIENT_KEY` |
| Headers | `OTEL_EXPORTER_OTLP_HEADERS` | `OTEL_EXPORTER_OTLP_LOGS_HEADERS` |
| Compression | `OTEL_EXPORTER_OTLP_COMPRESSION` | `OTEL_EXPORTER_OTLP_METRICS_COMPRESSION` |
| Timeout | `OTEL_EXPORTER_OTLP_TIMEOUT` | `OTEL_EXPORTER_OTLP_TRACES_TIMEOUT` |

Setting a global endpoint enables all three signals. Setting only a signal endpoint enables only
that signal. For an OTLP/HTTP base endpoint, the SDK appends `/v1/traces`, `/v1/metrics`, or
`/v1/logs`; a signal-specific endpoint is used as supplied by the SDK.

Recommended mTLS configuration:

```bash
export OTEL_EXPORTER_OTLP_ENDPOINT=https://otel-collector.example:4318
export OTEL_EXPORTER_OTLP_CERTIFICATE=/run/pki/collector-ca.pem
export OTEL_EXPORTER_OTLP_CLIENT_CERTIFICATE=/run/pki/aimee-client-chain.pem
export OTEL_EXPORTER_OTLP_CLIENT_KEY=/run/pki/aimee-client-key.pem
export OTEL_EXPORTER_OTLP_COMPRESSION=gzip
export OTEL_EXPORTER_OTLP_TIMEOUT=10000
export OTEL_RESOURCE_ATTRIBUTES='deployment.environment.name=production'
```

The client certificate and key must be supplied together; the key may not grant group/other
access. TLS files on an `http://` endpoint are rejected. A remote plaintext endpoint is also
rejected unless `AIMEE_OBSERVABILITY_ALLOW_INSECURE_OTLP=true` is explicitly set. That override
does not add encryption or authentication and should not be used in production.

`OTEL_EXPORTER_OTLP_HEADERS` can carry an authorization header supported by a collector
authenticator. Environment variables are often visible to privileged host tooling, so mTLS or a
secret-injection mechanism should be preferred. Do not put Datadog, Grafana Cloud, or Elastic API
keys in the Aimee process when a collector can own those credentials.

## Collector production boundary

[`deploy/observability/otel-collector-production.yaml`](../../deploy/observability/otel-collector-production.yaml)
is a fail-closed mTLS receiver and mTLS upstream template. The operator must mount and supply:

- Collector server chain/key and the CA that issues Aimee client certificates.
- Upstream CA and collector client chain/key.
- A backend OTLP endpoint, writable storage directory and byte limit, retention policy, capacity
  limits, and alert routing.

Required template variables are:

```text
OTEL_RECEIVER_GRPC_ENDPOINT=0.0.0.0:4317
OTEL_RECEIVER_HTTP_ENDPOINT=0.0.0.0:4318
OTEL_RECEIVER_TLS_CERTIFICATE=/run/pki/collector-chain.pem
OTEL_RECEIVER_TLS_KEY=/run/pki/collector-key.pem
OTEL_RECEIVER_TLS_CLIENT_CA=/run/pki/aimee-client-ca.pem
OTEL_COLLECTOR_STORAGE_DIRECTORY=/var/lib/otelcol/aimee
OTEL_COLLECTOR_STORAGE_MAX_BYTES=10737418240
OTEL_UPSTREAM_ENDPOINT=https://backend.example:4318
OTEL_UPSTREAM_TLS_CA=/run/pki/backend-ca.pem
OTEL_UPSTREAM_TLS_CERTIFICATE=/run/pki/collector-client-chain.pem
OTEL_UPSTREAM_TLS_KEY=/run/pki/collector-client-key.pem
```

Use a secret store or read-only secret volume, mode private keys `0400`/`0600`, and ensure the
collector process can read them. The template deliberately contains no insecure fallback and no
embedded secret. A backend can be an Elastic-compatible OTLP endpoint, Grafana Alloy/Tempo/Loki,
Datadog via its Collector exporter, or another OTLP consumer. Backend availability does not alter
Aimee's instrumentation contract.

## Certificate and secret lifecycle

- Use separate server and client identities; do not reuse a general web-server key.
- Constrain CA issuance and use server-auth/client-auth extended key usages appropriately.
- Use short-lived certificates where automation permits and alert before expiry.
- Mount private files read-only and keep group/other permission bits clear.
- Rotate scrape and Aimee exporter credentials with a rolling process restart.
- The Collector supports certificate reload settings, but verify the exact deployed Collector
  version before relying on reload behavior.
- Revoke compromised certificates at the CA/mesh boundary and replace bearer secrets everywhere
  they are mounted. Aimee never logs raw bearer values.

## Verification and failure tests

Before opening a firewall or service object, verify locally:

```bash
# TLS server identity and protocol
openssl s_client -connect aimee-server.example:9464 \
  -servername aimee-server.example -CAfile /run/pki/observability-ca.pem -tls1_2

# mTLS scrape
curl --fail --cacert /run/pki/observability-ca.pem \
  --cert /run/pki/prometheus-client-chain.pem \
  --key /run/pki/prometheus-client-key.pem \
  https://aimee-kb.example:9465/metrics

# Bearer scrape without exposing the token in argv
curl --fail --cacert /run/pki/observability-ca.pem \
  --config <(printf 'header = "Authorization: Bearer %s"\n' "$(tr -d '\r\n' </run/secrets/aimee-observability-token)") \
  https://aimee-server.example:9464/metrics

promtool check config /etc/prometheus/prometheus.yml
otelcol-contrib validate --config deploy/observability/otel-collector-production.yaml
```

Also prove negative cases: no client certificate, wrong CA, missing/wrong bearer, expired
certificate, permissive key/token permissions, plaintext remote bind, and security settings with no
listener. Each must fail. Restrict network reachability even when application authentication is
enabled, monitor scrape/export failures, and keep the local JSON logs available because OTLP export
is intentionally best-effort and never part of request correctness.

## Normative and operator references

- [OpenTelemetry OTLP exporter specification](https://opentelemetry.io/docs/specs/otel/protocol/exporter/)
- [OpenTelemetry Collector TLS and mTLS configuration](https://github.com/open-telemetry/opentelemetry-collector/blob/main/config/configtls/README.md)
- [OpenTelemetry Collector security guidance](https://opentelemetry.io/docs/security/config-best-practices/)
- [Prometheus scrape authentication and TLS configuration](https://prometheus.io/docs/prometheus/latest/configuration/configuration/)
