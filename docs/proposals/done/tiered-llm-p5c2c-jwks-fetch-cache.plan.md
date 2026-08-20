# Tiered LLM P5-C2c: Authenticated JWKS Fetch and Durable Server Cache

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

Status: completed; roundtable-converged and validated

## Objective and boundary

Complete the public half of the P5 management-token trust path: an enrolled aimee-server fetches the immutable C2b signed JWKS envelope from aimee-kb over the existing pinned server-to-kb mTLS channel, validates it against an independently operator-installed C2a public trust bundle, and installs it in a durable monotonic server-local cache. Add a typed unknown-`kid` verifier result and one-refresh orchestration seam for the later action composition slice.

This slice does not mint a token, use a private token key, enable `/v1/management/action`, dispatch a management mutation, or implement a generation after C2b's generation 1. If a later publisher ships generation 2, this C2c consumer continues to accept generation 1 only until a separately reviewed plan lifts the pin; it never silently gains rotation semantics. The existing unconditional action denial remains unchanged.

## Trust and authorization boundaries

The kb exposes exact `GET /v1/management/jwks` only on its mTLS listener. It is a special certificate-only route before the generic bearer router, never a plaintext or console-admin route. It rejects a query, request body, path alias, and non-GET method. The request supplies no server id or identity header. The route uses only the issuer, normalized serial, and DER fingerprint derived from the verified TLS client leaf.

A new `SECURITY DEFINER` PostgreSQL facade, owned by a dedicated `aimee_kb_jwks_runtime_definer` role, atomically requires exactly one active `kb_server_registry` client-leaf binding and its matching active, unrevoked, unexpired `p5-server-client` enrollment for that TLS triple. The role is `NOLOGIN NOINHERIT NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION BYPASSRLS`; FORCE-RLS crossing is isolated to this reader, runtime is not its member, and the role owns no tables or vault helper. It receives SELECT only on registry, enrollments, and the three publication tables plus EXECUTE on the existing owner-held vault-open helper. Reusing the status definer would mix authority compartments; keeping the publication owner would combine reader compromise with owner authority; a synthetic principal would couple machine authentication to unrelated membership policy. The facade requires the vault open, takes the fixed publication transaction lock, and returns only one exact internally consistent FINAL generation row and its bounded public metadata. It never returns EMPTY, STAGED, or CAS_DONE state. Runtime receives EXECUTE only on this facade and no table, writer, publisher-reader, provider, or private-custody authority. PUBLIC and all unrelated roles are explicitly denied. The schema change must enumerate and test the complete closure: revoke the new function from PUBLIC and the publisher/status/status-provision/token-root-provision/migration roles; retain the existing runtime revocations on all publication/root/candidate/generation/registry/permit/key-use tables, sequences, `publication_final`, inspect/roots/writers, vault/key-use, HWM, signing, and custody functions; then grant `aimee_kb_runtime` only EXECUTE on the new runtime facade. Grants occur only after all revokes in the idempotent schema and are asserted by the PG17 ACL fixture. Add the facade to the P7 barrier inventory. This corrects the initial owner-held design after the PG17 test demonstrated that `aimee_kb_owner` is intentionally `NOBYPASSRLS`; the dedicated-compartment decision was converged by runner `p5c2c-definer-decision-v2-20260722`.

Authorization failures are distinguished without leaking state: no/malformed client identity is 401, an authenticated but unregistered/revoked/expired client is 403, wrong method is 405, and primary/vault/integrity/publication unavailability is 503. Authority failures close the keep-alive connection. A successful response is `application/json` whose body is the byte-identical stored C2b envelope, with exact Content-Length and no wrapper or reserialization.

## KB runtime adapter

Add a small runtime-only DB2 adapter that calls the new facade on the primary connection and accepts exactly one row followed by DONE. It bounds generation, candidate id, validity, envelope length, and every digest; rejects embedded NUL; recomputes the envelope SHA-256; and clears all output on failure. It does not link any offline JWKS publisher, root provisioner, vault recovery, manifest signer, or HWM provider object.

The mTLS listener passes only its TLS-derived triple to this adapter. The existing per-request enrollment precheck remains defense in depth, but the new facade is the route's exact registry/enrollment authority. Both checks must allow: the precheck may fail fast before the facade, while a precheck allow never overrides a facade denial. An authenticated leaf that passes the broad issuer/serial precheck but fails exact registry/fingerprint/purpose/expiry binding is 403. Tests force disagreement in both directions and prove no route success unless the intersection allows.

## Consumer validation and trust bootstrap

The server accepts a C2a public trust bundle only from an explicit bounded absolute operator-configured path. It validates that path and file as a root-owned, non-group/world-writable, regular single-link file opened without symlink following. The fetch transport cannot supply or replace this trust root. The bundle pins the manifest wire id and Ed25519 public key, the initial RSA JWK/kid, and the publication identity digest; the cache is bound to the bundle digest so changing the deployment pin cannot reinterpret an old cache.

Extract or share only neutral public-bundle decoding primitives if doing so preserves the existing publisher/root-provisioner target-isolation gates; otherwise implement a server-local strict decoder and cross-component byte fixtures. No private provisioner object enters the server target.

Validate the fetched outer envelope as exact canonical bytes, not a permissively parsed/re-serialized equivalent: strict UTF-8; no duplicate or unknown members; exact member order and spelling; canonical unsigned JSON numbers in the safe integer range; canonical unpadded base64url; exact generation 1 and zero previous-manifest sentinel; exact validity ordering; RSA-3072/e=65537 C2a JWK; all SHA-256 bindings; canonical payload reconstruction; manifest transcript; Ed25519 signature under the pinned manifest root; manifest id/root/publication-identity equality; and exact frozen C2a/C2b fixture parity. The extracted verifier input is only the standalone canonical `{"keys":[...]}` bytes.

Generation 1 is the only accepted generation in this slice. Same-generation replay must be byte-identical. Lower, higher, conflicting, malformed, not-yet-valid, and expired artifacts cannot replace the current cache. Multi-generation publication and chain advancement are deferred until a publisher can produce them.

## Durable server cache and refresh policy

Use DB1 as the server-local authority so cache bytes and the monotonic generation/HWM commit atomically with the existing FULLMUTEX/WAL transaction gate and backup lifecycle. Add an immutable generation row plus singleton current pointer bound to generation, validity, exact JWKS/payload/envelope bytes and digests, previous/manifest digests, manifest id/public-root digest, trust-bundle digest, and fetch time. A `BEGIN IMMEDIATE` transaction validates current state, inserts only an absent byte-identical generation, and advances the singleton pointer atomically. Commit ambiguity is unavailable; it never authorizes from presumed state. DB1 unavailable, busy, corrupt, inconsistent, or constraint failure denies and clears output.

At startup, after DB1 initialization and before listener acceptance, validate the configured trust bundle and any cached row. One authenticated refresh is allowed when configured. Consumer authorization uses the exact half-open interval `valid_from <= now && now < valid_until`; it grants no clock-skew extension beyond signed times. A refresh should be scheduled before expiry, but failure may continue using the artifact only until `valid_until`. Startup outage is nonfatal only if the cache is already cryptographically valid and currently in that interval; an empty or stale cache leaves management authorization unavailable and emits a bounded operator warning/availability metric without artifact or token bytes. This slice does not make the server data plane unavailable, and the existing unconditional `/v1/management/action` 503 remains unchanged regardless of cache state.

Expose a cache lookup/refresh orchestrator with a single-flight mutex/condition. A known `kid` uses a currently valid cache without network. An empty cache or a strictly well-formed cache missing an otherwise valid token header `kid` makes exactly one authenticated mTLS fetch; concurrent waiters share that attempt; the verifier retries once and then rejects. A malformed JWKS, token header, signature, or claims is INVALID and never triggers network access. Transient fetch failure never deletes or replaces the last valid cache, but an expired/not-yet-valid cache cannot authorize.

Add `server_mgmt_token_verify_ex` with typed `OK`, `UNKNOWN_KID`, and `INVALID` results while retaining the boolean compatibility wrapper only for legacy callers and tests. The new cache/refresh orchestrator must call the typed API directly and must never make a refresh decision through the boolean wrapper. `UNKNOWN_KID` is possible only after strict parsing of the token header and the entire canonical JWKS, including unique kids. Focused tests prove the wrapper maps only `OK` to success, and prove malformed input returns `INVALID` with zero fetches while a strict absent kid returns `UNKNOWN_KID` with at most one fetch. There remains no production call that can execute an action in C2c.

Use the existing pooled `kb_client_mtls_request` path through a bounded `kb_client_mtls_management_jwks` wrapper. It accepts only status 200 and an envelope-sized response and follows no redirects or fallback endpoint. C2c must document and test process restart using fixed enrolled TLS material or add a narrowly scoped durable enrolled-identity facility; it must not claim that the current consumed one-shot enrollment environment token survives a fresh process.

## Verification

Focused unit and fixture tests cover the frozen C2a bundle/C2b envelope; every envelope/member/order/UTF-8/base64/number/time/digest/signature/JWK/root mutation; exact output clearing; known/unknown/malformed `kid`; exactly-one retry; and unchanged unconditional action denial. Fuzz the bounded bundle/envelope consumer and cache-state decoder under ASAN/UBSAN.

Temp-DB1 tests cover install, exact replay, conflict, rollback/future-generation denial, restart persistence, trust-pin change, validity boundaries, busy/commit failure, schema corruption, and 32-thread single-flight/concurrent install. Crash/fault tests prove an interrupted transaction leaves the old valid cache or no cache, never a partially authoritative row.

Real PostgreSQL 17 tests cover facade owner/prosecdef/proconfig and ACL closure; exact active triple success; every issuer/serial/fingerprint mutation; wrong enrollment purpose; pending/revoked registry; revoked/expired enrollment; sealed vault; future/expired publication; EMPTY/STAGED/CAS_DONE non-disclosure; FINAL exact byte equality; table/join tampering; public-schema/temp shadowing; runtime's lack of table/publisher/provider access; and fetch/revoke linearization with the following request denied.

On the isolated CT260/CT262 two-node topology, run production kb on CT260 and the server cache consumer on CT262. Prove authenticated fetch/install, exact response bytes, offline currently-valid restart use, expiry plus outage denial, and one unknown-kid refresh. Prove no cert, wrong CA/hostname/EKU/pin, revoked client on a reused connection, authority outage, spoof identity header, wrong method/query/body/path alias, plaintext/bearer access, malformed/rollback response, and truncated durable cache all fail closed without replacing the last valid row. Use only CT260/CT262 unless a full `pct list` is reread before allocating another throwaway guest.

Run hardened server/kb/status/root-provisioner/publisher builds, target-isolation and publisher-isolation (including plant) checks, schema sync/alter-order/SQLite-shape checks, focused release and sanitizer/fuzz tests, the full real-PG gate, and an adversarial full-branch roundtable. After convergence, move this plan to done, update the offering delivery table and tally memory, create a PR against `testing`, merge with admin, and verify GitHub CI.

## Implementation and validation record

Implemented the certificate-only kb route and its dedicated BYPASSRLS reader compartment, the runtime DB2 adapter, strict C2a/C2b consumer, durable DB1 cache, single-flight refresh policy, typed unknown-`kid` verifier, and server startup wiring. The real topology exposed and fixed a `db_postgres` decoded-BYTEA lifetime bug by retaining the envelope before reading later BYTEA columns. The real PG gate exposed and corrected the original owner-held facade design because the table owner is intentionally NOBYPASSRLS; roundtable runner `p5c2c-definer-decision-v2-20260722` converged on the dedicated, non-login, exact-closure definer role now implemented.

Validation completed:

- Local hardened server and kb builds, focused cache/token tests, schema sync, target/publisher isolation and plant checks, full lint, ASAN/UBSAN cache/token tests, and token fuzz corpus.
- Full CT103 PostgreSQL 17 gate, including exact facade ownership/attributes/ACL closure and active, mismatched, pending, revoked, expired, sealed, and non-final fixtures.
- CT260 root-run trust-file hardening and cache tests, including wrong mode, hard link, and symlink rejection.
- Production CT260 kb to CT262 client topology with real mTLS enrollment/registry binding and byte-exact signed-envelope fetch.

Generation advancement, background pre-expiry scheduling, action dispatch composition, and the broader negative topology matrix remain explicitly in C2d/C3 rather than silently expanding this slice.

Branch-review runner `p5c2c-branch-v9-20260722` identified one valid slice-local race: the facade captured its validity timestamp before waiting on the publication lock. The implementation now captures time after acquiring that lock. Four other reported regressions (mTLS worker stack, DB2 lease release, `agent_jobs.participant_token`, and `roundtable.review`) were confirmed as reverse-diff noise from 20 newer commits already on `origin/testing`; the branch is merged forward before convergence review so those base fixes remain intact.

After merging forward to `origin/testing` at `cedae488`, the complete branch diff was reviewed again by runner `p5c2c-branch-v10-20260722`. The only eligible isolated review seat found no issues and the reasoning result was aligned with zero surviving items. The run is conservatively recorded as degraded because the isolated review environment had one eligible participant; repeated shared-service retries returned an unrelated deployment's missing runner rather than a slice result. No claim of multi-seat convergence is made.

Post-merge-forward validation regenerated the schema, rebuilt hardened server and kb targets, passed the focused cache and token tests, passed the complete lint suite, and passed the full CT103 PostgreSQL 17 RLS/schema gate including the P5-C2c JWKS fixture. The branch was then published for admin merge into `testing`.
