# P5-D2a management-read trust core and bounded agents projection

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** completed 2026-07-23; roundtable-converged and validated on PostgreSQL 17 plus the
  CT260-to-CT262 production mTLS topology.
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

Add a distinct primary table, `kb_management_read_intent`; do not extend or reinterpret the action
intent table. Each row immutably binds a fresh correlation/JTI, team, composite actor, target
server, selector (CHECK exactly `agents`), capability (CHECK exactly `remote_reads`), canonical
external method/path, the 32-byte challenge nonce, request digest, actual kb peer certificate
issuer/serial, enrolled target certificate issuer/serial, revocation generation, token-publication
generation, exact audience and issuer, and issuance deadline. Add the database-enforced shared
`kb_management_token_intent_namespace` table with UNIQUE `correlation_id`, UNIQUE `jti`, UNIQUE
`(correlation_id,jti,kind)`, and a kind CHECK of exactly `action` or `read`. Each typed intent has a
constant kind column checked to exactly its type and an explicit foreign key on the complete
`(correlation_id,jti,kind)` tuple. In one transaction, the exact action/read creation function uses
`INSERT ... ON CONFLICT DO NOTHING` to reserve its namespace row, requires exactly one returned
row, and then inserts the typed row. Zero returned rows or SQLSTATE `23505` maps only to the public
`conflict` result; every other SQL error rolls the transaction back and maps to `unavailable`.
Migrate existing action intents into the namespace and require every future
action creation to reserve it too. No preflight query substitutes for the unique constraints;
concurrent cross-kind inserts for either identifier yield exactly one reservation and one closed
conflict. Authority admission joins exactly one namespace row to exactly one matching typed row and
rejects missing, mismatched, or multiple sources.

The closed state machine is `pending -> signing -> issued`, with `expired` terminal. Runtime creates
only `pending` rows through an exact definer function and cannot update them. That function freezes
every JWT claim before insertion, including version, issuer, audience, subject, team, capability,
JTI, correlation, digest, peer issuer/serial/fingerprint, kid and key generation, `iat`, and `exp`;
`iat`, `exp`, and the issuance deadline derive from primary database time, not runtime input.

Only these authority-definer transitions exist, guarded with locked rows and database time:

- `pending -> signing` when `now < issuance_deadline`, recording a random lease owner and bounded
  `lease_until`, with `now < lease_until <= issuance_deadline`; `pending -> expired` when
  `now >= issuance_deadline` through the authority expiry
  function;
- `signing -> signing` only as lease recovery when `lease_until <= now < issuance_deadline`, changing
  only lease metadata and preserving every claim; `signing -> expired` whenever
  `now >= issuance_deadline`, even if a lease just expired; and
- `signing -> issued` only for the current lease owner while `now < issuance_deadline`, the frozen
  `exp > now`, the selected key id/generation still exactly matches, and the supplied token parses
  back to the frozen claims.

`issued` and `expired` have no outgoing transitions. An issued row remains issued after JWT expiry;
expiry is enforced from its frozen `exp`. Authority admission invokes the expiration transition
before any claim or signing readback. At the equality boundary (`now == issuance_deadline`) expiry
wins and signing/finalization is forbidden.

Every authority-definer call first locks the typed row with `SELECT ... FOR UPDATE`, then assigns
`v_now := clock_timestamp()` exactly once after the lock is acquired, and uses that same post-lock
`v_now` for lease, issuance-deadline, and frozen `exp` comparisons through commit. Lease recovery,
expiry, and finalization therefore serialize on the row lock; after waiting, each evaluates state
and all time predicates against one fresh pinned time and cannot authorize a transition using a
pre-wait timestamp or enter the equality boundary twice.

After signing, one authority transaction atomically stores the exact bounded JWT bytes in the
authority-owned row, stores their SHA-256, appends the WORM key-use record, and moves the row to
`issued`; there is no committed state with the audit/token hash but no issued token. The token is
returned only by exact post-commit authority readback of those retained bytes, never by rebuilding
it. A crash before that transaction commits leaves `signing` with no durable token/audit and lease
recovery signs the same frozen claims with the same frozen key generation; a crash after commit
returns the byte-identical retained token and never signs again. Ordinary kb roles have no direct
access to retained bearer bytes. Racing admissions yield exactly one issued result and closed
conflicts for the rest. Reuse the C2d admit/use/readback/finalize discipline where applicable,
without weakening the new table's type separation.

The request digest is SHA-256 over this exact length-prefixed binary encoding. It begins with the
19 bytes `aimee-mgmt-read-v1\0`, followed in order by: external method `GET`, canonical external
path first formed by byte-concatenating `/v1/servers/`, the concrete validated `server_id`, and
`/agents` (there are no literal braces), then encoded as one length-prefixed string (the three
substrings are not separately encoded), selector `agents`, server id, team as unsigned big-endian
64-bit, the raw 32-byte nonce, kb peer certificate issuer, kb peer certificate serial, enrolled
server certificate issuer, enrolled server certificate serial, revocation generation as unsigned
big-endian 64-bit, and token-publication generation as unsigned big-endian 64-bit. Each string is
raw canonical UTF-8 prefixed by an unsigned big-endian 16-bit byte length; strings containing NUL,
invalid UTF-8, noncanonical identifiers, or exceeding 65535 bytes are rejected rather than
normalized. Integers have no textual encoding. There are no optional fields, separators, JSON,
escaping, or Unicode normalization. A checked-in known-answer vector freezes every input byte and
the resulting digest; alternate order, encoding, path alias, or unknown field fails.

Certificate issuer is the existing enrollment/status canonical slash-form byte string obtained by
`X509_NAME_oneline(X509_get_issuer_name(cert), ...)`, persisted and compared byte-for-byte with no
case folding, whitespace change, or re-rendering. Certificate serial is the non-negative ASN.1
INTEGER converted by `ASN1_INTEGER_to_BN` plus `BN_bn2hex`, then lowercase ASCII hex with no `0x`,
separator, whitespace, or added leading zero beyond `BN_bn2hex`'s even-length representation;
empty or negative serials fail. The live peer and
primary snapshot must already compare equal in these canonical forms before hashing.

The normative known-answer inputs are method `GET`, path `/v1/servers/server-a/agents`, selector
`agents`, server id `server-a`, team 42, nonce bytes `00 01 ... 1f`, kb issuer `/CN=kb-ca`, kb serial
`01af`, server issuer `/CN=server-ca`, server serial `10be`, revocation generation 7, and publication
generation 9. The complete 165 encoded bytes in hex are:
`61696d65652d6d676d742d726561642d7631000003474554001b2f76312f736572766572732f7365727665722d612f6167656e747300066167656e747300087365727665722d61000000000000002a000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f00092f434e3d6b622d6361000430316166000d2f434e3d7365727665722d636100043130626500000000000000070000000000000009`.
Their SHA-256 is `c66354428fbcdb9648b532b8de71b748e0d058711cfb441c608f5460564efcbf`.

Mint the existing short-lived management JWT shape with a new closed capability
`remote_reads`, audience equal to the exact stable registry `server_id` (never endpoint, DNS name,
or certificate name), issuer equal to the configured pinned issuer string, operator composite
subject, unique JTI,
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

The challenge is admitted using the base management mTLS chain, dedicated clientAuth EKU/policy,
certificate syntax/pin, and local generation/high-water checks. It does not and cannot require the
fresh nonce-bound proof that it exists to enable. Its exact response is no more than 512 bytes and
contains no extra fields:
`{"nonce":"<43-character unpadded base64url encoding of 32 bytes>","purpose":"management.read.v1","expires_at":<unix-seconds>}`.
The nonce has a 15-second lifetime, is bound to that peer/session and one read request, and is
single-use. Receiving it confers no authorization and exposes no fleet data. Entropy failure,
clock failure, malformed body, replay, expiry, session change, or capacity exhaustion fails closed
with the bounded error schema below.

The server generates the challenge `purpose` from a closed enum containing exactly
`management.read.v1`; it accepts no caller-supplied purpose. The kb challenge parser requires the
exact field set and exact purpose before constructing an intent. The signed status proof carries
the same closed purpose and the server verifies it byte-for-byte; an unknown or action purpose is
`integrity`. The JWT is purpose-separated by its exact `remote_reads` capability plus bound method,
path, selector, nonce digest, and audience; do not add an advisory token-purpose field.

Challenge issuance and consumption reuse the existing server-local SQLite `server_mgmt_nonce`
store and `server_mgmt_nonce_issue_purpose`/`consume_purpose` transactions, extending their closed
purpose allowlist with exactly `management.read.v1`. Issuance atomically stores the raw nonce plus
peer issuer/serial/fingerprint, TLS channel binding, target server, purpose, and expiry. At step 6,
status verification calls `consume_purpose` before dispatch; its `BEGIN IMMEDIATE` lookup, binding
checks, unconditional delete on every identifiable attempt, high-water update, and commit form one
atomic consume. Concurrent reuse yields one result and then `conflict`; binding/purpose/rollback or
invalid proof is `integrity`; storage/busy/commit failure is `unavailable`. Process restart clears
the outstanding-nonce table during management-status initialization, so every pre-restart
challenge becomes invalid rather than reusable. Expired rows are boundedly collected on issuance.
No reconnect or fresh JTI can make a consumed, failed, expired, or restart-invalidated nonce valid.

The kb/server sequence is fixed:

1. Authenticate the operator and obtain one primary target/actor/certificate snapshot.
2. Establish pinned mTLS and obtain the bounded challenge.
3. Build the canonical request digest including that nonce.
4. Insert the immutable `pending` read intent.
5. Have the isolated authority admit, sign, audit, and finalize the token.
6. Obtain a fresh primary-backed nonce-bound status proof and dispatch the exact read on the same
   pinned connection without redirect, reconnect, or retry.
7. Validate the complete binding and atomically consume the JTI before loading configuration.
8. Load, validate, sort, and encode the entire projection in memory.
9. Perform the final primary-backed read checkpoint immediately before the first response byte.
10. Emit the already-complete response once.

Failure at any step emits no data. Once JTI consumption succeeds it remains consumed even if load,
projection, checkpoint, or response delivery fails; a fresh external request is required. The
token's client certificate issuer/serial must match the actual mTLS peer and intent snapshot. Its
server audience, enrolled certificate identity, revocation/status generation, publication
generation, status proof, and checkpoint must all refer to the same request snapshot. The purpose
is closed everywhere to `management.read.v1`; action-purpose artifacts cannot satisfy it.

Step 7 reuses the existing server-local SQLite `server_management_jti` table and
`server_management_jti_consume` transaction, extending its capability CHECK to the closed pair
`remote_writes`/`remote_reads` without changing action rows. Its primary key on `jti`,
`BEGIN IMMEDIATE`, bounded garbage collection, and insert-before-commit provide durable at-most-once
consumption across threads and process restart. Replay/unique collision is `conflict`; invalid
verified-claim material is `integrity`; database unavailable, busy, commit failure, or capacity is
`unavailable`. No data loads before commit returns OK, and no later failure deletes the row.

Status freshness is the existing B3 bound (proof issue/expiry fields and high-water generation),
further limited by the 15-second read nonce; both must be valid at dispatch. All identity strings
use the canonical issuer/serial rules above, fingerprints and digests use exact lowercase hex, and
identifiers use exact ASCII bytes. Compare, in order: intent kb identity to live peer; intent
snapshot to status peer/target/cert fingerprints and revocation/publication generations; stable
registry server id to token audience; enrolled `mgmt_cert_cn`/identity to the pinned server peer;
and status snapshot to the final checkpoint. Any byte/generation mismatch or rollback is
`integrity`; explicit revoked/inactive state is `forbidden`; authority transport/primary outage is
`unavailable`. The final checkpoint returns the current primary generation/snapshot and must equal
the intent/status generation tuple. A rotation or generation bump between dispatch and checkpoint
therefore emits `integrity` and zero response bytes.

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

The precise lexical bounds are: `name` matches `[A-Za-z0-9._-]{1,63}`, `provider` matches
`[A-Za-z0-9._-]{1,15}`, and `model` matches `[A-Za-z0-9._:/+-]{1,127}`. `enabled`,
`delegate_available`, and `primary_only` are JSON booleans; `max_parallel` is an integer in
`0..1024`. Sort by unsigned ASCII bytes of `name`, independent of locale. Duplicate names,
invalid UTF-8, wrong types, out-of-range values, or more than 16 agents fail the whole request.
The completely encoded envelope is capped at 32 KiB; it is never truncated or streamed.

The projector reads only through a new frozen getter that copies the seven typed allowlisted fields
from each validated `agent_t` into a dedicated projection record. It receives neither the raw JSON
object nor the remaining `agent_t` fields. Adding fields to the loader or `agent_t` cannot widen
this getter without changing its type and focused fixtures; an `api_key_ref`/endpoint/command
canary present in the source object must remain unreachable and absent through JSON round-trip fuzz.

The envelope contains only `server_id`, `team`, and `agents`. Unknown internal fields are ignored
by construction because the projector creates fresh objects from the allowlist; it does not clone
or redact raw JSON. Never emit API keys, key commands, endpoints/base URLs, executable commands,
environment references, headers, auth config, personas/roles/instructions, tool policy, filesystem
paths, or raw config fragments. Bound object count, every string, nesting, and total encoded body;
use strict UTF-8/JSON and fail rather than truncate or partially return.

"Unknown internal fields" means additional fields already accepted in an authoritative local
`agent_t` object but absent from this projection; the projector never reads or emits them. Unknown
external selectors, query keys, JSON/envelope members, token/status purposes or capabilities,
projection enum values, and fields in any newly parsed management-read wire object are rejected.
This does not make the projector a generic raw-config parser and does not change existing agent
configuration compatibility.

Every external and management-listener failure uses the closed shape
`{"error":{"code":"<enum>","message":"<fixed text>","correlation_id":"<43-character unpadded base64url>"}}`.
The exact mapping is:

- HTTP 400, `invalid_request`, `Invalid request.`
- HTTP 403, `forbidden`, `Forbidden.`
- HTTP 404, `not_found`, `Not found.`
- HTTP 409, `conflict`, `Request conflict.`
- HTTP 502, `integrity`, `Integrity verification failed.`
- HTTP 503, `unavailable`, `Service unavailable.`

Checks stop at the first failing protocol step in the fixed ten-step sequence above. Within a step,
the executable sub-check order below is authoritative: an earlier result wins and no later check
may replace it. Cross-team and unauthorized callers
always receive `forbidden`, not target existence. A missing target is `not_found` only after actor
authorization for that team. Cryptographic mismatch, rollback, binding failure, or malformed
authenticated upstream proof is `integrity`; an already-consumed nonce or JTI is `conflict`;
dependency timeout/outage/capacity is `unavailable`.
Never copy upstream response text or headers, parser detail, certificate/path/endpoint data, SQL
detail, or internal identifiers.

The executable sub-check order and mapping is also closed:

1. Parse method/path/query/body (`invalid_request`); authenticate actor (`forbidden`); obtain a
   primary snapshot (`unavailable`); authorize admin/active lead and team ownership (`forbidden`);
   test authorized-team target existence (`not_found`); test active/revoked state (`forbidden`);
   then validate snapshot self-consistency and generation monotonicity (`integrity`).
2. Resolve the snapshotted endpoint and open the one pinned mTLS connection (transport outage is
   `unavailable`; any completed peer/pin/identity mismatch is `integrity`).
3. Parse the exact challenge envelope and purpose, then construct/recompute the KAT-defined digest;
   any mismatch is `integrity`, while entropy/challenge service outage is `unavailable`.
4. Reserve namespace and insert the exact immutable intent (`conflict` for the specified unique
   loss; `integrity` for readback mismatch; otherwise database failure is `unavailable`).
5. Admit/finalize/read back the token (`conflict` for state/lease loss, `integrity` for claim/token
   mismatch, `unavailable` for authority/key/database outage).
6. Parse enough of the bounded status envelope to identify a nonce; malformed/unidentifiable input
   is `integrity`. For an identifiable nonce, atomically consume it on every attempt while passing
   the complete signature/shape result into `consume_purpose`: already absent/consumed/expired is
   `conflict`, binding/purpose/rollback or invalid proof is `integrity`, and storage failure is
   `unavailable`. Only after a valid trusted proof may its explicit revoked state produce
   `forbidden`; then compare remaining identities/generations (`integrity`) and dispatch. Thus an
   invalid proof cannot assert revocation, while a valid signed revoked proof deterministically
   yields `forbidden`.
7. Verify token and all bindings (`integrity`), then consume JTI (`conflict` for replay,
   `unavailable` for storage/capacity).
8. Load and project the frozen getter; any load, validation, count, or encoding failure is
   `unavailable` and produces no partial object.
9. Verify checkpoint signature/bindings/generation (`integrity`) after a successful lookup;
   lookup/primary outage is `unavailable`.
10. Write the already-complete success bytes once; a socket failure cannot be converted to a later
    public error and never restores nonce/JTI state.

Tests inject combined observable faults, including a malformed proof carrying a revoked bit, a
valid signed revoked proof with a later generation mismatch, revoked certificate plus replica
outage, and namespace collision plus malformed readback, and assert the ordered result rather than
whichever dependency returns last.

## Database isolation and failure semantics

Preserve the four-role authority split: `aimee_kb_token_authority_definer`,
`aimee_kb_token_authority_runtime`, `aimee_kb_token_authority_store_owner`, and ordinary
`aimee_kb_runtime`. They are NOINHERIT, have no cross-membership or usable SET ROLE path, and
owner/definer roles are NOLOGIN where applicable. Ordinary runtime can execute only the exact
publication-generation and read-intent creation functions; it cannot update the table, execute
authority admission/readback, retrieve retained bearer bytes, or read authority key material.
Authority runtime can execute only fixed admission/finalization/readback functions. Definer
functions perform exact typed reads, row locks, and allowed transitions, set a
safe search path, and are owned/granted narrowly. The table uses ENABLE and FORCE RLS with no
broad direct DML grants.

Concurrent requests for one correlation/JTI produce at most one signed/issued token. An issued row
is never reissued. Concurrent listener attempts consume a JTI at most once. Authority death before
durable `issued` plus WORM/key-use audit exposes no token; lease recovery signs only the same
immutable claim set. Projection or checkpoint failure after JTI consumption does not roll it back.
PostgreSQL gates must negatively prove role membership, SET ROLE, direct DML, authority-function,
cross-kind source, multi-row source, and type-confusion denial in addition to successful recovery.
They also race lease recovery against expiry at `lease_until`/deadline boundaries using the pinned
database time and race two server listener threads on one JTI, including SQLite busy/storage death;
exactly one transition/consume may succeed and no agent bytes may be loaded or emitted on failure.

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
- byte-for-byte recomputation of the complete 165-byte digest known-answer vector and expected
  SHA-256, plus one-byte-at-a-time field mutations that must change the digest;
- mTLS peer/EKU/pin, token audience/team/path/digest/cert/capability/expiry/JTI, unknown-kid single
  refresh, signed generation rollback, nonce purpose/replay, checkpoint race, revocation at every
  challenge/dispatch/response boundary, no redirect/reconnect, and runtime unregister races;
- concurrent same-nonce consumption and restart invalidation, plus an authority row-lock wait that
  crosses the issuance deadline and must expire rather than recover/finalize;
- projection canaries proving raw secrets/endpoints/commands/env references never appear; exact
  count/string/integer/body boundaries; load failure != empty; duplicate names, unknown external
  selectors/wire members, wrong-type/NUL/invalid UTF-8 rejection; accepted extra internal agent
  fields ignored and absent from output; fuzz corpus plus ASAN/UBSAN; and
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

## Delivery evidence

- The frozen digest/token/status/checkpoint composition, strict agent projection, external kb
  route, server management-listener route, OIDC-only console drill-down, OpenAPI surfaces, and
  least-privilege PostgreSQL authority landed as one bounded branch.
- A successful runtime composition test exercises challenge → authoritative publication
  generation → immutable intent → isolated token authority → signed status → final primary
  snapshot → exact management read → closed projection. Adversarial cases cover read-token/write
  separation, duplicate projection members, rejected-response isolation, authority-result mapping,
  route grammar, and unregister races.
- A fresh real PostgreSQL 17 database passed schema installation, FORCE-RLS/grant boundaries,
  authoritative publication generation, retained-bearer isolation, read intent, authority
  admission/finalization/readback, and WORM key-use checks.
- Exact archived candidate builds passed on CT260 and CT262. The production management listener on
  CT262 passed CT260 mTLS challenge issuance and fail-closed invalid-token dispatch with random
  correlation identifiers. Production server/kb builds, focused C tests, Go tests, frontend tests,
  API conformance, and diff hygiene passed.
- Four logical adversarial branch roundtables were resolved; the focused correction review
  converged with `No issues found`.
