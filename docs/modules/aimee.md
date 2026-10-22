# aimee module

## Purpose and non-goals

`aimee` owns the server-specific DB1 domain model and peer-session behavior. It defines typed
families, validation, and workflow persistence while `postgres` owns generic database execution.
It does not own PostgreSQL pooling, KB data in KB_STORE, or the shared event-bus runtime.

## Public contracts

Principal `30` serves domain stages `1` through `19` as event kinds `11777` through `11795`, then
peer stages `20` through `23` as `11796` through `11799`. Typed family operations use the bounded
store wire; peer delivery, inbox, grant, and channel operations use the versioned peer wire.

## Dependencies and consumers

- `audit`: records governed peer actions and DB1 domain mutations without storing secret payloads.
- `config`: supplies bounded server settings and the DB1 connection configuration used at startup.
- `ir`: supplies the public data-only canonical session/history codec; DB1 retains storage and authorization ownership.
- `execution-policy`: authorizes action-class peer sends before the module accepts their delivery.
- `module-runtime`: authenticates the process, principal, stage grants, lifecycle, and readiness.

Server routes, workflows, session management, telemetry, and MCP peer tools consume these contracts;
they do not open DB1 directly or import `postgres` implementation details.

## Providers and readiness

The Go package under `server-go/modules/aimee` provides the handlers. Readiness requires its stage
catalog, DB1 family catalog, migration checks, and generic store client to initialize successfully;
a missing database or checksum mismatch keeps the module unavailable instead of serving partial state.

## Configuration and activation

- `runtime_toggle.supported`: `false`; DB1 and peer contracts are required server behavior and cannot disappear during a live session.

DB1 uses `AIMEE_STORE_URL` for runtime access and `AIMEE_STORE_MIGRATION_URL` for schema administration.
Operators manage those as secrets; there is no `AIMEE_DB1_PATH` or supported SQLite fallback.

## Surfaces

The module is reached through named server `/v1` routes, workflow operations, generated DB1 clients,
and the MCP `peer_send` and `peer_inbox` tools. Its bus stages are internal contracts; no caller sends
arbitrary SQL, supplies a database path, or selects an unregistered family name.

## Data and migrations

DB1 is PostgreSQL. Domain schemas and migrations live in `server-go/modules/aimee/families`, including
session, workflow, identity, audit-attribution, and working-memory rows. Migrations are ordered,
checksummed, owner-scoped, and applied through `postgres`; operators back up DB1 with `pg_dump`.
The descriptor lists embedded SQL in `go_assets`, separate from JSON event `contracts`.
Independent module exports copy these files and track them as CMake build dependencies;
complete ownership validation rejects missing assets and paths outside the module Go directory.

### Canonical session log (migration in progress)

Migration 44 adds `session_ir_events`, owned by the existing session lifecycle, with cascade
deletion on session removal/subject erasure. Session operations 28/29 append validated canonical
events and return bounded replay pages. Append locks the session row inside the family transaction;
retrying an identical producer event ID returns its original decimal sequence, while reusing
that ID with different turn, actor, or payload fails. Sequence values remain decimal strings
in transport envelopes. Authorization and authenticated actor derivation must occur in Server
before these internal operations are exposed; there is no new browser-accessible route.

Browser and log-owned delegate capture commit canonical events before delivery. Direct primary
and delegate IR execution read their canonical history from this log; vendor SDK resume/cache
activation remains unfinished. Native handles are derived metadata, never a second transcript.

Session operation 34, `session_harness_compare_swap`, compares/replaces an owner-checked native
metadata value under the existing session-row transaction lock. Empty expected text claims an
absent key; stale comparisons return `missing` and cannot overwrite another execution's claim or
completion. The host native-cache controller uses unique claim revisions before dispatch and
retains an unresolved state if later proof/handle writes fail. Completed prefix commitments live
in typed `native_checkpoint` session events. This operation introduces no browser route, schema
migration or parallel history store.

## Security and privacy

The module validates family arity and bounds before storage, derives actor identity from authenticated
context, and never accepts credentials inside workflow rows. `postgres` receives typed statements and
parameters; authorization, principal provenance, and audit disposition remain at the domain boundary.

## Supported journeys

A server start migrates DB1, verifies every registered family, and exposes health only after the store
is ready. A workflow persists its definition and run rows through `aimee`; a peer send is policy-checked,
addressed by authenticated session identity, stored durably, and drained exactly once.

## Tests and failure behavior

Tests under `server-go/modules/aimee` cover family catalogs, migrations, wire arity, SQL checksums,
peer ownership, and database failures. Unknown operations, malformed frames, stale identities, failed
transactions, and unavailable storage return typed errors; none silently falls back to memory.

## Operational diagnostics

Use module readiness, DB1 migration diagnostics, PostgreSQL connectivity, peer queue counters, and
request IDs to isolate failures. Logs name the `aimee` family or peer stage and preserve database error
classes while excluding SQL credentials, message bodies, and sensitive workflow parameters.

## Compatibility

Principal `30`, event kinds `11777` through `11799`, family identifiers, migration owners, and wire
versions are persistent compatibility contracts. The removed SQLite DB1 paths and the former principal
`31` layout are not valid 0.4.0 configuration and receive no compatibility fallback.

## Extension and removal

Add a domain operation by registering it in the `families` catalog, assigning a stable stage or typed
family opcode, adding migrations and failure tests, and updating generated clients. Removal requires a
versioned consumer migration; principal refs, event kinds, migration owners, and recorded versions are never reused.

### Server-local procedure experience

With KB mode `none`, or unset mode and no configured connection, the existing
`learning.application` route persists explicit feedback locally. Configured KB
applications retain their existing owner; an explicitly configured broken KB does
not silently fall back. The host still reads the principal-owned WORM provider
receipt and matches exact attempt, procedure revision and exposure references.
Clients supply neither exposure nor actor authority. Verified UDS users, browser
users and mTLS clients can submit feedback; model execution intent and daemon
bearers cannot establish human authority. No active-session/project requirement
is added to feedback on an existing receipt.

Learning remains the sole admission/projection owner. DB1 telemetry operations 59
and 60 load a bounded immutable cohort and append against its snapshot digest.
The host projects before append and re-projects on a concurrent update. Event IDs
are principal-scoped and immutable; identical retries are idempotent. Migration 46
owns `local_procedure_events` and payload-free `local_procedure_erasure_intents`.
These are observation metadata, independent of KB knowledge/artifact storage.
Full admitted JSON survives the typed interface without the legacy 4096-byte DTO.

Subject erasure captures surviving event-identity digests, removes local rows and
fences old snapshots through the existing erasure epoch. Startup replay also
cleans rows resurrected by content-only restores. Runtime roles cannot erase the
control metadata. No process-success observation becomes a verified task grade.
This path does not implement autonomous synthesis or independent grading.
