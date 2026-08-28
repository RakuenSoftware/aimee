# egress module

## Purpose and non-goals

`egress` is the sole governed HTTP and SSE transport for network calls initiated by Go process modules.
It binds method, destination, purpose, caller identity, DNS result, limits, and credential presence before
dialing. It does not proxy arbitrary user traffic or replace PostgreSQL and sandbox transports.

## Public contracts

Principal `32` serves seven ledger stages at event kinds `12289` through `12295`: authorization,
bounded unary HTTP, and stream open, lifecycle, and frame operations. Requests and responses use the
versioned bounded wire in `aimee/egress/module_api.h`; redirects require a fresh authorization decision.

## Dependencies and consumers

- `audit`: receives content-free authorization and transfer outcomes for the WORM evidence path.
- `module-runtime`: attests principal `32`, caller identities, executable ownership, lifecycle, and stage grants.

Memory embedding, forge operations, review artifacts, and remote MCP SSE consume the service. Each uses
a distinct request-only bus identity so one consumer cannot borrow another consumer's destination policy.

## Providers and readiness

The Go process under `server-go/modules/egress` provides credential resolution, pinned HTTP dialing, and
SSE framing. Readiness requires its policy catalog, resolver, and Vault helper contract. External endpoint
failure affects the call, while a missing policy or unsafe helper keeps the relevant stage unavailable.

## Configuration and activation

- `runtime_toggle.supported`: `false`; all declared Go-module Internet access must pass through the required governed transport.

Destination and credential policy comes from reviewed process contracts and narrowly scoped environment
secrets such as `AIMEE_MCP_<principal>_TOKEN`. Ordinary callers cannot add a host or widen an allowlist at runtime.

## Surfaces

There is no public proxy URL or CLI tunnel. Callers use authenticated `egress` bus stages; operators see health,
metrics, and audit outcomes. Forge credentials arrive as short-lived encrypted envelopes, while MCP
credentials are resolved by exact principal. External response content returns only to the requesting caller.

## Data and migrations

`egress` owns no durable content database and requires no schema migration. Policy lives in versioned
source contracts; short-lived credential envelopes and SSE state remain in memory and vanish on restart.
The audit sink stores bounded metadata such as destination class, byte counts, status, and outcome.

## Security and privacy

Every DNS answer must be allowed and the `egress` dial uses the validated numeric address. Redirects, private
addresses, credential scope, response size, and stream lifetime are checked independently. Raw headers,
bearers, request bodies, external responses, and secret-bearing errors never enter capture or WORM.

## Supported journeys

A forge caller submits a scoped request, `egress` validates its attested identity and GitHub origin,
decrypts a 30-second envelope, adds the bearer, pins DNS, and returns bounded bytes. An MCP stream uses
the same authorization seam, then receives lifecycle and frame events until closure or limit expiry.

## Tests and failure behavior

Tests under `server-go/modules/egress` cover policy, DNS pinning, redirects, credentials, HTTP bounds,
and SSE lifecycle. Unknown callers, disallowed destinations, mixed public/private answers, expired
envelopes, oversized bodies, and helper failures fail closed without opening or retaining a connection.

## Operational diagnostics

Inspect principal `32` readiness, caller principal, purpose, destination class, DNS refusal, HTTP status,
byte counters, and stream termination reason. Correlate the ledger event with its request ID. Diagnostics
must never print authorization headers, Vault output, encrypted-envelope plaintext, or response content.

## Compatibility

Principal `32`, events `12289` through `12295`, stage meanings, caller identities, and policy purpose
strings are stable compatibility boundaries. Adding a destination does not relax existing callers.
Direct network primitives outside declared owners remain a lint and runtime contract violation.

## Extension and removal

Add a consumer by assigning an attested caller identity, exact `purpose` values, destinations, credential rules,
bounds, audit disposition, and denial tests. Extend the wire additively with fixtures. Removal requires
migrating every caller to another governed owner and retaining historical purpose semantics for audit.
