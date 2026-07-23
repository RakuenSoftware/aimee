# P5-D2a management-read trust core and bounded agents projection

- **State:** proposed for roundtable review.
- **Parent:** `tiered-llm-p5-oidc-control-plane.md`, §§2–4.
- **Depends on:** P5-B3 management mTLS/status channel, P5-C2 token authority, P5-C3
  checkpoint/action composition, and P5-D1 OIDC-preserving fleet console.
- **Followed by:** P5-D2b explicit safe config projection and P5 close-out.

## Boundary

Add one read-only per-server fleet surface without proxying the server's generic data-plane API:

- external: `GET /v1/servers/{server_id}/agents?team=<canonical positive int64>`;
- management listener: an exact challenge plus one exact agents-read operation; and
- console: an OIDC-only per-server agents drill-down.

D2a establishes the management-read authorization primitive and returns a closed, bounded agent
projection. It does not expose `/v1/agents`, `/v1/agent/list`, `/v1/config`, config mutation,
agent mutation, arbitrary management paths, bulk/fan-out reads, credentials, endpoints, command
templates, environment references, raw `agents.json`, or secret-shaped placeholders. D2b owns
the separately frozen config allowlist because config exposure is a distinct data-loss boundary.

## Decisions to review

The recommended contract is binding unless the plan roundtable identifies a concrete flaw:

1. Introduce a distinct `remote_reads` management capability. Never reuse `remote_writes`, and
   never represent a read as a fake enable/disable action.
2. Authorize only an org-admin or an active team lead for the target team. Ordinary membership is
   sufficient for the existing registry/health view but not for detailed server configuration.
3. Add a primary-backed immutable management-read intent admitted by the isolated token authority.
   An ordinary kb process must not self-select token claims merely because the operation is a read.
4. Require the B3 nonce-bound status proof, the C3 primary checkpoint immediately before response,
   and durable server-local JTI consumption. This preserves next-request revocation and replay
   semantics rather than creating a weaker parallel protocol.
5. Reads remain independent of `remote_writes`; they fail disabled when the management-read
   runtime is absent or explicitly disabled. D2a adds no general-purpose read toggle exposed to an
   untrusted caller.

## External kb and console contract

Register only `GET /v1/servers/{server_id}/agents`. Require exactly one `team` query value in
canonical positive signed-64-bit decimal form. Reject missing, duplicate, empty, signed,
zero/leading-zero, overflow, percent-aliased, and extra query parameters before authorization or
network I/O. Retain the existing 512-byte path bound and exact segment equality.

The handler authenticates the composite actor and performs one primary-authoritative snapshot
that proves:

- actor is org-admin or active lead of the requested team;
- target server is active and belongs to that team;
- enrollment and management server certificate remain active/unrevoked;
- endpoint and enrolled `mgmt_cert_cn` are the same snapshot; and
- this kb instance has an admitted live management client identity.

Cross-team, inactive grant, revoked target/cert, replica-only, skewed snapshot, or unavailable
authority fails before dialing. The kb handler returns a closed JSON envelope and a bounded error;
it never relays arbitrary server headers. Keep the existing strict HTTPS, address policy,
connect-time DNS revalidation, certificate pin, no redirects, one deadline, and response cap.

Add the external route to the C kb route ACL for the authenticated management-read authority. Add
the matching `/api/v1/servers/{id}/agents` route only to `fleetAllows`: it uses the verified OIDC
credential preserved by D1. It is never added to `consoleAdminAllows`; break-glass and a missing
OIDC vault entry fail before upstream I/O. The D1 mutation latch/ACK protocol does not apply to
this idempotent GET.

## Read-intent and token-authority contract

Add an immutable primary table for read intents, or extend the existing authority admission with
an equally narrow typed source while preserving its current action-intent closure. The preferred
row binds a fresh correlation/JTI, team, composite actor, target server, selector `agents`, exact
request digest, capability `remote_reads`, target certificate/generation snapshot, issuance
deadline, and terminal admission state. RLS and grants give runtime kb only the exact insert/read
operations; the isolated token authority performs the atomic pending-to-issued admission.

The request digest binds a canonical versioned envelope, not merely an empty-body hash:
`v=1`, external method/path, selector, target server id, team id, challenge nonce, and relevant
certificate/status generations. Unknown fields, alternate order/encoding, or path aliases fail.

Mint the existing short-lived management JWT shape with a new closed capability
`remote_reads`, audience equal to the target server, operator composite subject, unique JTI,
request digest, target/team binding, and actual kb management client-certificate binding. Extend
the token builder, verifier, authority snapshot/readback, schema CHECKs, and key-use admission as
one closed typed change. Existing `remote_writes` tokens remain byte-for-byte and behaviorally
unchanged. WORM-audit metadata records authorization, actor, team, target, selector, correlation,
JTI, digest, key id/generation, and result status, never the returned agent bytes.

## Management-listener protocol

Add only these routes to the distinct required-mTLS management listener:

- `POST /v1/management/read/challenge` with exact empty body, returning a bounded fresh nonce for
  purpose `management.read.v1`; and
- `GET /v1/management/read/agents` with no query or body.

The agents request carries only the short-lived bearer token and exact nonce-bound signed status
proof required by B3. The server validates listener/peer EKU and policy, live peer status,
purpose/nonce/generation/freshness, token signature/kid/generation/audience/cert binding/team/
capability/path/digest/expiry, and durably consumes the JTI before rendering. It performs the C3
primary checkpoint after authentication and immediately before returning data. Authority/JWKS/
status/checkpoint outage fails closed. Generic data-plane agent/config routes are never registered
on the management listener.

## Bounded agents projection

Load the authoritative local agent configuration through a dedicated read-only projector. A load
or validation failure returns 503; it must never masquerade as an empty successful fleet. Return
at most 16 agents, sorted by canonical agent name, with an exact object schema:

- `name`: existing agent identifier grammar and length bound;
- `provider`: bounded public provider identifier;
- `model`: bounded public model identifier;
- `enabled`: boolean;
- `delegate_available`: boolean;
- `primary_only`: boolean; and
- `max_parallel`: bounded non-negative integer.

The envelope contains only `server_id`, `team`, and `agents`. Unknown internal fields are ignored
by construction because the projector creates fresh objects from the allowlist; it does not clone
or redact raw JSON. Never emit API keys, key commands, endpoints/base URLs, executable commands,
environment references, headers, auth config, personas/roles/instructions, tool policy, filesystem
paths, or raw config fragments. Bound object count, every string, nesting, and total encoded body;
use strict UTF-8/JSON and fail rather than truncate or partially return.

## Console UI

Extend Fleet selection with a read-only `Load agents` drill-down. It uses the already validated
team and selected server, clears stale results when either changes, disables duplicate in-flight
loads, and renders only the closed projection. It performs no action POST and no fleet result ACK.
Show authorization/unavailable/load-failure states without suggesting that console-admin or
break-glass can bypass them.

## Verification

Unit and integration coverage must include:

- exact path/method/query/body matching, encoded/trailing/sibling/extra argument denial;
- OIDC-only console credential selection, caller-header stripping, break-glass/console-admin
  denial before I/O, missing/restarted vault reauthentication, and 1 MiB atomic response bound;
- org-admin/team-lead success; ordinary member, inactive lead, cross-team, revoked enrollment/
  cert, stale/read-replica snapshot, and wrong target denial;
- read-intent immutability, FORCE RLS/least privilege, multi-instance concurrent admission, closed
  `remote_reads` capability, action/read type confusion denial, and WORM/key-use audit closure;
- mTLS peer/EKU/pin, token audience/team/path/digest/cert/capability/expiry/JTI, unknown-kid single
  refresh, signed generation rollback, nonce purpose/replay, checkpoint race, revocation at every
  challenge/dispatch/response boundary, no redirect/reconnect, and runtime unregister races;
- projection canaries proving raw secrets/endpoints/commands/env references never appear; exact
  count/string/integer/body boundaries; load failure != empty; duplicate/unknown/wrong-type/NUL/
  invalid UTF-8 rejection; fuzz corpus plus ASAN/UBSAN; and
- frontend rendering, canonical team/server reset, degraded errors, in-flight dedupe, and proof
  that agents reads issue neither mutation nor ACK.

Run real PostgreSQL 17 schema/RLS/grant/concurrency gates, production kb/server builds, focused C
unit/sanitizer/fuzz gates, Go race tests, frontend tests/build, docs generation, and kb/server API
conformance.

On the approved throwaway topology, use CT260 for real PG/token/status/kb/console and CT262 for a
production server if its current dedicated P5 role is intact; otherwise allocate a new throwaway
only after recording the complete `pct list`. Log in through a real RS256 mock OIDC issuer and
read agents end to end. Prove exact OIDC composite attribution, safe projection, secret canary
absence, and ordinary-member/cross-team/break-glass/console-admin/wrong-cert/revoked-cert/
authority-outage/generic-route failure. Prove reads remain available with `remote_writes=off` and
fail when the read runtime is absent. Finish with an adversarial frozen branch review and merge to
`testing` before beginning D2b.
