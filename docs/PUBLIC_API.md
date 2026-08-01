# Public API

aimee exposes two versioned HTTP surfaces:

- `aimee-server /v1` for clients, browsers, sessions, agents, tools, workflows, config, and
  OpenAI/Anthropic-compatible ingress;
- `aimee-kb /v1` for memory, documents, code, retrieval, curation, and KB administration.

The generated references are authoritative:

- [Server and KB routes](gen/api-v1.md)
- [`aimee-kb` OpenAPI](../api/openapi-v1.yaml)
- [`aimee-server` OpenAPI](../api/openapi-server-v1.yaml)
- [Route descriptor](gen/v1-route-descriptor.json)

## Stability

- A stable route is prefixed with `/v1` and has a stable operation ID.
- New routes, optional request fields, and response fields may be added within v1.
- Clients must ignore unknown response fields.
- A breaking request or response change uses a new major prefix with an overlap period.
- `/v1/internal/*`, explicitly experimental operations, browser proxy routes, and diagnostic fields
  are not public compatibility promises.

The generic `POST /v1/rpc` endpoint is retired. Use named operations.

## Transport

Local clients use the server Unix socket. Remote clients use HTTPS. KB traffic is HTTP inside a
declared deployment boundary or HTTPS across hosts.

JSON is the normal body format. Upload routes accept bounded raw or multipart content as described
by their OpenAPI operation. Streaming routes declare their framing in the generated reference.

Every response has a status code. Errors return a small JSON object with a stable machine signal;
human wording may change. When present, `X-Request-ID` ties the response to logs and downstream
calls.

Use cursors where an operation exposes pagination. Do not infer numeric offsets.

## Authentication

| Caller | Authentication |
| --- | --- |
| local thin client | Unix-socket filesystem ownership |
| remote thin client | TLS pin plus bearer or mTLS client identity |
| browser | authenticated web session and CSRF protection |
| server → KB | configured service bearer and TLS where required |
| workflow → resource plane | supervised local peer and narrow internal route |

Authentication identifies the principal. Each route then checks capabilities, scope, and write tier.
The shared bearer is read-only; writes use a KB-signed identity and exact subject grant.

Send remote bearer credentials as:

```http
Authorization: Bearer <token>
```

Do not put tokens in query strings.

## Scope and writes

KB operations can be global, workspace, project, or user scoped. The target scope comes from the
operation's typed fields, not from a free-form path. A scoped principal cannot cross into another
scope even when it knows an object ID.

Remote writes are split into data and full authority. Data covers memory, documents, and index
ingestion. Full also covers runner and workspace mutation. The route descriptor declares the class
for each operation.

File commands from a thin client upload bytes. A server route must never interpret a client path as
a local path.

## Discovery and health

Use the documented health, version, capability, and OpenAPI operations for feature detection. Do not
derive server capability from a version string alone.

The running service may serve its OpenAPI document. CI checks route descriptors, handlers, and specs
for drift. `make docs-gen-check` checks the human reference.

## SDKs

SDKs are generated from OpenAPI for C, C++, C#, Go, Java, Python, Rust, and TypeScript. They are build
artifacts, not hand-maintained clients.

```bash
scripts/gen-sdks.sh
scripts/gen-sdks.sh python go
make -C src sdk-parity-check
make -C src api-conformance-check
```

The generator and toolchain requirements live in `scripts/gen-sdks.sh`. Run live SDK smoke tests
against the deployment before publishing an SDK package.

## Event bus is not public HTTP

The shared-memory event bus is an intra-daemon module contract. It does not accept remote clients,
replace route authorization, or expose capture files over `/v1`. A future external bus adapter still
needs its own admission and public compatibility contract.
