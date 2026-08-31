# P5-D1 console fleet and OIDC-preserving proxy

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** delivered; plan review converged in jobs 8902–8903 after the audience/`azp`,
  vault-compensation, canonical-team, and response-overflow corrections. Adversarial branch
  review then drove the durable mutation-result acknowledgement protocol described below.
- **Parent:** `tiered-llm-p5-oidc-control-plane.md`, §§3–4.
- **Depends on:** P5-A registry, P5-B3 live health channel, and P5-C3 action composition.
- **Followed by:** P5-D2 bounded agents/config reads and P5 close-out.

## Boundary

Expose the already-delivered fleet primitives through `kb-console` without collapsing an OIDC
operator into the console's shared `scope:console-admin` bearer. This slice adds exact console
routes for registry list, live health, and the two C3 mutations (`agent.enable` and
`agent.disable`), preserves the verified browser OIDC token only for those requests, documents
the existing kb routes in OpenAPI, and adds a Fleet UI.

This slice does not add a generic reverse proxy, arbitrary server paths, config mutation, bulk
actions, automatic retries, new token minting, a console signing key, or persisted OIDC bearer
material. It does not widen the C `console-admin` ACL. Read-only server agents/config forwarding
requires a new bounded management protocol and remains P5-D2; the Fleet drill-down in D1 shows
registry state and live health only.

## Credential and session contract

The console continues to verify the OIDC `id_token` at login with its existing RS256/issuer/
audience/admin-claim contract. D1 requires one explicit IdP resource/client registration shared by
kb and kb-console: the console's configured issuer and nonempty audience must exactly equal the
active values returned by kb's authenticated `/v1/config/oidc` at startup, or OIDC fleet proxying
stays disabled. The token's `aud` must be the single string equal to that audience, not a
multi-audience array; if `azp` is present it must equal that same registered audience. The kb still
enforces the identical issuer/audience before constructing an actor. Tests cover wrong audience,
multi-audience, and mismatched-`azp` tokens so forwarding never weakens audience validation.

Extend the verified principal with the exact JWT expiry. Create the console session with an
absolute expiry of `min(existing eight-hour bound, JWT exp)` and reject a
token whose expiry cannot be extracted or is already past. After the session row commits, place an
owned byte copy of the exact verified compact JWT in a process-local credential vault keyed by the
fresh random session id. The vault is bounded by the maximum live session count, mutex-protected,
expires entries at the token expiry, overwrites bytes before release, and never logs, serializes,
audits, returns, or stores the token in SQLite. Logout, session invalidation, and expiry remove and
cleanse the entry. Vault insertion is part of login's fail-closed compensation contract:
allocation, capacity, or concurrent insertion failure deletes the just-created session row,
cleanses any temporary token copy, emits no cookie, and returns an error. Startup has an empty
vault: an old OIDC session row therefore cannot authorize a
fleet request and is deleted with a 401 requiring login again. This intentional restart behavior
avoids creating a durable bearer-token custody system in the console.

Break-glass remains usable for every existing console-admin route, preserving the lockout recovery
path, but it is not permitted to originate fleet health or mutation requests in D1. A break-glass
fleet request returns 403 before network I/O. The shared console-admin bearer never becomes an
operator principal and is never sent to a `/v1/servers...` route. This avoids manufacturing an
owner identity from a scoped service credential. If non-OIDC fleet management is later required,
it must use an explicit individually attributable owner credential rather than silently widening
console-admin.

## Deny-by-default proxy split

Keep the existing `consoleAdminAllows(method,path)` table unchanged. Add a separate exact
`fleetAllows(method,path)` family containing only:

- `GET /v1/servers`
- `GET /v1/servers/{server_id}/health`
- `POST /v1/servers/{server_id}/actions`

The matcher retains segment-count equality, a 512-byte path ceiling, no encoded aliasing, and
method equality. `proxyAPI` classifies the path before constructing the upstream request:

1. existing console-admin route: send only the configured console-admin bearer;
2. fleet route: require a non-break-glass OIDC session and a live vault entry whose `(iss,sub,exp)`
   equals the session, then send only that exact OIDC bearer; or
3. everything else: 403 without upstream I/O.

The browser cannot select the credential class. Incoming `Authorization`, cookie, forwarding,
hop-by-hop, or identity headers are never copied. Only the existing bounded body and Content-Type
are forwarded, with CSRF required for the action POST. Preserve the query string byte-for-byte;
the kb remains authoritative for the C3 exact `team` query grammar and request body. Bound upstream
responses at 1 MiB. Buffer at most limit-plus-one before writing any downstream status or byte; an
overflow closes the upstream body and returns exact 502 JSON, never a partial or truncated success.
Propagate only Content-Type plus the status. An upstream 401 invalidates the session and vault entry so an expired/revoked OIDC token
cannot be retried through an eight-hour console cookie.

## KB and API contract

No new C authorization mechanism is introduced. The existing additive OIDC verifier authenticates
the forwarded JWT, builds the issuer-qualified `(iss,sub)` principal, and the P5 registry/health/
action handlers apply team/RLS/capability policy. Add the two currently-undocumented surfaces to
`api/openapi-v1.yaml`:

- `GET /servers?team=<canonical positive decimal>` returning the bounded registry list; and
- `POST /servers/{server_id}/actions?team=<canonical positive decimal>` accepting the exact closed
  `agent.enable|agent.disable` body and returning the existing result/error statuses.

Keep the existing live-health operation. Regenerate generated API documentation and route
descriptors. Add conformance tests proving every documented route is routed and no generic server
proxy route exists.

## Fleet UI

Add a `Fleet` navigation page. The operator enters/selects one team id matching C3's canonical
grammar: `[1-9][0-9]*`, with no sign or leading zero and within signed 64-bit range before any
list, health, or action request. D1 does not infer it from registry metadata. The page lists server id, registry status, last reported
health/version, and management availability from `GET /v1/servers`. Selecting a server may request
live health. For one agent name matching `[A-Za-z0-9._-]{1,63}`, expose explicit Enable and Disable
buttons with confirmation, CSRF-backed POST, in-flight disabling, and exact success/denied/
unavailable feedback. Never automatically retry a mutation and never issue a second request after
an ambiguous network failure. Surface `remote_writes` denial without suggesting that kb overrides
the server policy.

## Verification

Go tests cover route-family separation, OIDC-vs-console-admin Authorization selection, no caller
header forwarding, issuer/audience/`azp` parity, break-glass denial before I/O, token expiry, vault
cleanup, capacity/concurrent insertion compensation, restart/missing-vault reauthentication, CSRF,
exact-limit and limit-plus-one response behavior, and upstream 401 invalidation. Existing console-admin ACL
drift tests remain unchanged and new fleet ACL tests prove encoded/trailing/sibling/wrong-method
denial.

Frontend tests cover registry rendering, canonical team validation including zero/sign/leading-zero/
overflow cases, live-health degradation,
enable/disable confirmation, one in-flight mutation, no retry, and policy-denied display. Run
`go test ./...`, the console and production kb/server builds, frontend typecheck/build/tests,
OpenAPI/docs/conformance checks, sanitizers for touched C boundaries, and relevant P5 unit gates.

On CT260, run a two-node kb/server topology plus kb-console and a local mock OIDC issuer. Log in
with a real RS256 token, list the team fleet, fetch live health, enable then disable a real agent,
and prove the server and kb WORM records contain the OIDC composite actor rather than
`console-admin`. Prove a different-team token is denied, `remote_writes=off` prevents the side
effect, the console-admin bearer cannot reach fleet routes, token expiry forces re-login, and no
automatic redispatch occurs. Complete with an adversarial full-branch roundtable, converge, merge
to `testing`, update the delivery table, and store the next final tally.

## Delivery record

Delivered on 2026-07-23. The implementation preserves verified OIDC credentials in a bounded,
process-local cleansing vault; keeps console-admin and fleet ACL/credential classes disjoint;
adds tenant-scoped fleet registry reads, live health, explicit enable/disable actions, OpenAPI
contracts, and the Fleet UI; and uses a durable SQLite mutation state machine. Each mutation
atomically claims state with a fresh random result token, ambiguous outcomes remain locked, a
definite response is locked before downstream delivery, and only a CSRF-protected browser ACK
carrying that exact token releases the session. Stale ACKs cannot release later results.

Validation passed locally and from the exact archived head on CT260: Go unit/race/build, 101
frontend tests and production build, production kb/server builds, docs generation, 75-route kb
and 309-route server conformance, concurrent single-dispatch, and the live RS256 OIDC mock-kb
flow (registry, health, enable, disable, cross-team denial, policy denial, and result ACKs). The
mock proved every fleet request used the operator OIDC bearer and never console-admin.
