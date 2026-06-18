# aimee-kb Public API: Stability Contract

The **aimee-kb `/v1` HTTP API** is the sole, public, versioned interface to the
aimee knowledge base. aimee-server is its primary client; third parties consume
the same endpoints with no access to aimee source.

- **Source of truth:** [`api/openapi-v1.yaml`](../api/openapi-v1.yaml)
  (OpenAPI 3.1). The running service serves the identical document at
  `GET /v1/openapi.json` and `GET /v1/openapi.yaml`; CI checks byte-for-byte
  parity.
- **Human reference:** [`docs/gen/api-v1.md`](gen/api-v1.md), generated from the
  spec (`make docs-gen`).
- **Client SDKs:** generated under [`api/sdks/`](../api/sdks/) for eight
  languages; see [SDKs](#sdks).

This document is the **stability contract**. It applies from the first tagged
release; pre-release `/v1` may still iterate freely.

---

## Versioning

- **URL-prefixed.** Every endpoint lives under `/v1`. The version is visible to
  clients, routers, and proxies.
- **Additive within a major.** New endpoints, new optional request fields, and
  new response fields may be added to `/v1` at any time; these are non-breaking
  by construction. Clients MUST ignore unknown response fields.
- **Breaking changes get a new prefix.** A backward-incompatible change is
  introduced as `/v2` alongside `/v1` during an overlap window. The deprecation
  cycle for a retired major is **≥ 6 months**.
- **Stable identifiers.** `operationId`s in the spec are stable and drive the
  generated SDK method names; they are not renamed within a major version.

What is **not** covered by the stability contract: `/v1/internal/*` routes,
unversioned aliases, and any endpoint explicitly marked experimental in the
spec.

---

## Transport

- HTTP/1.1 and HTTP/2. JSON request/response bodies; `multipart/form-data` for
  uploads (`POST /v1/docs`). WebSocket for live job/invalidation streams
  (`GET /v1/jobs/{id}/stream`).
- **TLS is required for any non-loopback bind.** Loopback / Unix-socket
  deployments may run without TLS.
- The service is enabled by setting `kb.api.http_port` in aimee-kb config;
  bearer auth is enabled by setting `kb.api.bearer_token`.

### WebSocket streams (Phase-2)

Two `/v1` endpoints upgrade to a WebSocket (RFC 6455) instead of returning a
single JSON body. Both authenticate with the same bearer as REST routes; server
frames are unmasked text JSON.

| Endpoint | Stream |
|----------|--------|
| `GET /v1/jobs/{id}/stream` | live job-status frames until the job is terminal (done/failed), then a close frame |
| `GET /v1/events` | a `{"type":"subscribed"}` greeting, then `{"type":"invalidation","kind":...,"scope_kind":...,"scope_id":...,"ts":...}` events whenever cached retrieval state changes (a **release** is promoted/rolled back, or a **doc** is ingested) |

The invalidation stream lets aimee-server drop cached search/entity/index
results on signal rather than waiting for TTL expiry (the cache itself is a
separate aimee-server concern). A client reconnects by re-opening the stream;
the kb closes streams cleanly on shutdown. `make v1-ws-test` (a stdlib-only
client, `src/tests/test_v1_ws.py`) exercises both against a live kb.

---

## Authentication & authorization

There are exactly two trust boundaries (charter §Service Topology):

| Boundary | Transport | Auth |
|----------|-----------|------|
| clients ↔ aimee-server | Unix socket / loopback | filesystem permissions on the socket |
| aimee-server ↔ aimee-kb | UDS (same host) or TLS (remote) | none on UDS; **bearer token** on TLS |

### Bearer tokens (remote)

Send `Authorization: Bearer <token>`. A configured token may be
**self-describing**:

```
scope:<kind>:<id>:<secret>     scoped token   (e.g. scope:project:foo:s3cr3t)
<secret>                       unscoped/admin token (full access)
```

- `<kind>` is one of `global` / `workspace` / `project` / `user`.
- **Authentication** (does the presented secret match the configured token) is
  separate from **authorization** (may a token scoped `<kind>:<id>` touch a
  resource at a different scope).
- A scoped token is denied with **403** on cross-scope access. Example: a token
  scoped `project:X` cannot read or write artifacts at `workspace:Y`. Admin
  (unscoped) tokens bypass the scope check.
- The request's target scope is taken from `scope=` / `project=` / `workspace=`
  query parameters or the `scope_kind` + `scope_id` / `scope_user` body fields.

The pure decision logic lives in `src/kb/kb_scope.c` and is unit-tested in
`src/tests/test_kb_scope.c`.

### Token lifecycle (v1, intentionally minimal)

- Opaque 256-bit tokens, base64url-encoded. aimee-kb stores only
  `sha256(token)` + metadata; operators keep cleartext in a root-owned
  `kb_token_file` on each aimee-server host.
- `aimee-kb token issue` prints once; `token rotate` supports a grace window;
  `token revoke` flips `active=false`.
- OIDC and refresh flows are deferred to a later distributed-mode auth proposal.

---

## Request & response conventions

- **Errors:** standard HTTP status plus a JSON body
  `{"error": "<message>"}`. The status code is the stable signal; the `message`
  text may change. The per-request correlation id is returned in the
  `X-Request-ID` response header (see below), not the body.
- **Pagination:** opaque cursors only; responses carry `next_cursor`, pass it
  back as `?cursor=<value>`. There are **no** numeric offsets.
- **Observability:** every response echoes `X-Request-ID` (client-supplied or
  server-generated as `<pid>-<counter>`), and the service logs it at INFO with
  method, path, and status. `GET /v1/health` reports dependency status.
- **Rate limiting:** none in v1. The scaffold exists for future per-token
  quotas.

### Discovery endpoints

| Endpoint | Purpose |
|----------|---------|
| `GET /v1/health` | liveness + dependency status |
| `GET /v1/version` | `api_version`, `model_version`, `prompt_version` |
| `GET /v1/capabilities` | configured extractors, normalizers, scope levels |
| `GET /v1/openapi.json` / `GET /v1/openapi.yaml` | the live OpenAPI document |

---

## SDKs

Day-one SDKs are **generated from the spec**, never hand-written. They are
**not committed** — `api/sdks/<lang>/` is gitignored and regenerated on demand
(locally or in CI, see [`.github/workflows/sdk-gen.yml`](../.github/workflows/sdk-gen.yml)):

```
c  cpp  csharp  go  java  python  rust  typescript
```

Regenerate them with:

```sh
scripts/gen-sdks.sh            # all languages
scripts/gen-sdks.sh python go  # a subset
```

The script is self-bootstrapping: it uses a system `java` if present, otherwise
it downloads a portable Temurin JRE and the pinned `openapi-generator-cli` jar
into `~/.cache/aimee-sdkgen` (no root required). Regenerating from an unchanged
spec is a **byte-for-byte no-op**, so regeneration is deterministic.

Two gates keep SDKs honest:

- `scripts/check-sdk-parity.py`: every `operationId` in the spec is covered by
  every generated SDK (`make sdk-parity-check`). Run after `make gen-sdks` in the
  SDK-gen CI workflow (the SDKs are no longer committed, so this is not part of
  the fast `make lint` target).
- `scripts/check-api-conformance.py`: every spec path is routed by the
  aimee-kb server (`make api-conformance-check`, wired into `make lint`).

Running each SDK against a live aimee-kb is automated by `scripts/sdk-smoke.sh`
(`make v1-sdk-smoke`): it builds a minimal consumer per language that calls the
service through the generated client and reports PASS/SKIP/FAIL, skipping
languages whose toolchain/deps aren't present and self-skipping when no kb is
reachable. Run it on any host with the toolchains installed (the per-language
list is in the script header) and point it at a deployed kb with `KB_BASE_URL` /
`KB_BEARER_TOKEN`; no CI service required.

Validated live on a Debian-12 host with all toolchains: **go, typescript,
python, java, c, cpp, csharp PASS**; rust requires a newer cargo than Debian 12
ships (the runner is correct and passes on cargo ≥ 1.74). The generated
cpp-restsdk client has no bearer-auth support, so cpp is exercised against an
unauthenticated kb. See also [Integration testing](#integration-testing).

---

## Integration testing

A language-agnostic smoke test that uses only `curl` + `jq` exercises the
core client journey (ingest → search → fetch artifact → subscribe to the
invalidation/job stream) against a running service:

```sh
KB_BASE_URL=http://127.0.0.1:8090/v1 \
KB_BEARER_TOKEN=<token-if-remote> \
  src/tests/test_v1_third_party.sh
```

It skips gracefully (exit 0, SKIP) when no service is reachable, so it is safe
in CI; point it at a deployed aimee-kb to validate the stability contract.

---

## Deploy modes

The same `/v1` contract serves all three charter deploy modes; only the
transport differs.

| mode | aimee-kb | server ↔ kb |
|------|----------|-------------|
| install-today (default) | localhost | Unix socket |
| services-split | remote | TLS + bearer |
| full-distributed | remote Postgres too | TLS + bearer |

Switching aimee-server from local to remote kb is a **config change, not a code
change**: set `kb_client_url` to the remote kb's `https://` **origin**
(`scheme://host:port`, without a `/v1` suffix; the client appends the versioned
path) and `kb_client_bearer_token` to a token the remote kb accepts.
aimee-server exports these into
`AIMEE_KB_API_URL` / `AIMEE_KB_API_BEARER_TOKEN`, which the kb_client transport
honors. See [`config/aimee-server.yaml.example`](../config/aimee-server.yaml.example)
and [`config/aimee-kb.yaml.example`](../config/aimee-kb.yaml.example).

---

## References

- [`api/openapi-v1.yaml`](../api/openapi-v1.yaml): the contract.
- [`docs/gen/api-v1.md`](gen/api-v1.md): generated reference.
- [`docs/proposa../done/aimee-kb-service-and-public-api.md`](proposa../done/aimee-kb-service-and-public-api.md)
  is the owning proposal.
- [`docs/proposals/done/memory-public-contract.md`](proposals/done/memory-public-contract.md)
  is the caller-facing contract reused across CLI / MCP / `/v1`.
