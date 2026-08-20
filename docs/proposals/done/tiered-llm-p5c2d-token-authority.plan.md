# P5-C2d online management-token authority

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** implemented and validated. Two completed reviewer passes were incorporated;
  the final multi-seat convergence call was attempted twice with distinct runner
  `p5c2d-token-authority-plan-v10-ensemble-20260722` and timed out at the bounded service gate.
- **Parent:** `tiered-llm-p5-oidc-control-plane.md`, §3.
- **Depends on:** P5-C1b canonical mint core, P5-C1c primary/WORM action journal,
  P5-C2a custody roots, P5-C2b final signed publication, and P5-C2c consumer contract.
- **Followed by:** P5-C3 management-action dispatch composition and P5-D OIDC propagation.

## Boundary

Make the C2a RSA-3072 token root usable online for the first time, but only inside a dedicated
minimal `aimee-kb-token-authority` process. Given an exact committed C1c correlation/JTI tuple,
the authority revalidates all mutable admission snapshots on the primary,
binds the journaled `kid` to the exact current FINAL C2b publication and enabled C2a custody key,
durably records one pre-use signing admission, opens the platform-scoped version-2 vault envelope
inside the protected arena, and invokes C1b internally to return the exact RS256 JWT.

The ordinary kb process never receives the token-root envelope, links the token-root DB adapter, or
executes a plaintext-key callback. It invokes the authority daemon over a fixed local checked-fd protocol
that accepts only correlation id and JTI and returns either one bounded JWT or a closed error. The
daemon runs as a dedicated unprivileged `aimee-token-authority` OS identity which is never shared
with kb, owns its private configuration and vault/HWM descriptors, and listens on an absolute,
root-owned Unix socket with a fixed group and mode. The client requires the listener's kernel
`SO_PEERCRED` UID of the root-created socket-activation listener (root is outside the threat
boundary; authority-created listeners are rejected), and the daemon admits only the configured kb
service UID verified with `SO_PEERCRED`; frames are length-delimited, one request is processed at a time, and
token bytes never enter argv or environment. The authority sets `PR_SET_DUMPABLE=0`, disables core
dumps, closes every non-allowlisted descriptor, and cannot be ptraced or inspected through `/proc`
by the ordinary unprivileged kb identity. No public or production API accepts a signing input, digest, PEM,
private key, arbitrary claims, or signer callback. The C1b callback is private to the authority
binary and reached only after database admission. Thus compromise of ordinary kb does not expose a
generic protected-use signing seam.
It does not dial a server, obtain a nonce/status staple, append an action outcome, enable
`/v1/management/action`, or expose a console route. Those remain C3.

## Primary authority and linearization

Add immutable `kb_management_token_key_use_intent`, keyed by the 64-hex C1c correlation id and
unique JTI. Store only the team, actor, target, request digest, kid, token-root custody key/version,
FINAL JWKS generation/candidate/manifest digests, vault seal epoch, HWM attestation digest, and
purpose `management.token.sign.v1`. Guard UPDATE/DELETE/TRUNCATE and revoke all direct table,
sequence, and guard access from PUBLIC, runtime, publisher, root provisioner, status roles, and
migration roles.

Add one hardened `SECURITY DEFINER` facade owned by a dedicated
`aimee_kb_token_authority_definer` role. The role is NOLOGIN/NOINHERIT/non-superuser with
BYPASSRLS only because it must cross the FORCE-RLS C1c journal while retaining a separate authority
compartment. A separate NOLOGIN/NOINHERIT/NOBYPASSRLS
`aimee_kb_token_authority_store_owner` owns only the new immutable intent table, its sequence, and
guards; the global migration/owner inheritance chain is not a member. The definer owns no table,
vault helper, publisher, root provisioner, or generic WORM function.
Grant it the exact SELECT/INSERT closure and EXECUTE only on vault-open and the narrow owner-held
WORM append wrapper needed by this facade. PostgreSQL requires a locking reader using
`SELECT ... FOR SHARE/UPDATE` to hold UPDATE privilege on a selected table, so give the definer
only the minimum column-level UPDATE privilege on one inert immutable/key column per locked table;
no facade accepts a mutation value, and PG17 asserts both the lock works and no mutation is
reachable. A distinct LOGIN/NOINHERIT/NOBYPASSRLS
`aimee_kb_token_authority_runtime` role is used only by the authority daemon and receives EXECUTE
only on the facade, finalize, and exact readback functions. `PUBLIC`, ordinary
`aimee_kb_runtime`, and every publisher/provisioner/status/migration role receive no execution on
those functions and no direct access to their tables, sequences, guards, vault rows, or helper
functions. Revoke default and explicit `PUBLIC` EXECUTE before making the narrow runtime grant.

The authority runtime role, not the ordinary kb runtime role, invokes the facade. It takes correlation id and
JTI and derives team/actor solely from the immutable journal row; no claimed principal crosses the
local wire. The facade uses a fixed row-lock order: action intent/outcome, actor membership/grant,
target registry/enrollment, local management installation/enrollment, revocation singleton,
publication registry/generation/candidate, token root, vault current/secret/rotation. Every mutable
writer is audited to take the naturally conflicting row lock. A REPEATABLE READ transaction and
these `FOR SHARE/UPDATE` locks prevent mixed snapshots; advisory locks are only supplemental
same-operation serialization and are never treated as authority fencing. After all locks it uses
`clock_timestamp()` and atomically requires:

1. exactly one C1c intent, no outcome, exact team/actor/JTI, `iat <= now < exp`, and the persisted
   actor is still an active tenant member and still a platform admin or active lead for that team;
2. the target registry and target `p5-server-management` enrollment are still active, unrevoked,
   unexpired, and byte-identical to the journaled enrollment/issuer/serial/fingerprint;
3. the selected local B2 management installation/current issue and
   `p5-kb-management` enrollment remain active and byte-identical to the journaled lineage;
4. the primary revocation generation equals the journaled snapshot (conservatively invalidating
   every pre-revocation intent rather than guessing whether a changed generation is relevant);
5. one internally consistent FINAL generation-1 C2b publication is currently valid at primary time,
   its sole RSA JWK kid equals the journaled kid, and all candidate/registry/root public and HWM
   public-key and envelope digests agree; the publication HWM and token-custody HWM remain
   distinct custody domains and are each checked against their own persisted binding;
6. the enabled token root is version 2, its platform-scoped `org:p5-token/management/rs256`
   current vault row and activated rotation agree, the vault is open, and its HWM attestation
   exactly matches the token root and activated token rotation binding.

Issuance has two explicit transactions. First, for a new correlation, insert the immutable key-use intent and append a bounded canonical
`vault.key_use` WORM event in the same transaction before any private-key operation. Exact replay
returns the identical admitted envelope with `newly_admitted=false`; any mismatch is conflict.
The authority refuses replay and never signs twice. Second, after that commit, begin a fresh
REPEATABLE READ use transaction, reacquire the complete row-lock set and repeat every current
authority/key/time check. Keep that transaction and all locks open across HWM verification,
protected decrypt, C1b mint and self-verification. Before releasing bytes, call a narrow finalize
function in the same transaction which takes one fresh `t = clock_timestamp()`, rechecks no
outcome and every time-dependent predicate against `t` while the locks remain held: intent
`iat <= t < exp`, active actor membership and grant intervals, target registry and target/local
enrollment expiry, local installation/current-issue validity, publication
`valid_from <= t < valid_until`, and every applicable certificate, installation, rotation, and
root validity interval. It also rechecks the complete still-locked identity/digest/version tuple
and commits. Only an acknowledged finalize commit permits the authority to
write the JWT response. Revocation/outcome writers therefore serialize before or after the signing
linearization point rather than racing through it.

A process crash after admission but before response leaves a durable *pre-dispatch unresolved*
record. It is never automatically retried. The record intentionally cannot and does not claim
whether RSA signing completed; it proves only that no token was safely returned for dispatch. C3's
later recovery reader may append LOCAL_FAILURE for the absence of a dispatchable result without
asserting a signing outcome. This prefers availability loss over duplicate private-key use.

The envelope and binding metadata exist only inside the authority process and its live use
transaction. Clear every output on denial, conflict, corruption, sealed vault, replica/read-only
access, or ambiguous commit. A lost admission COMMIT acknowledgement must be resolved by exact
readback as admitted-or-absent before any use transaction. Proven absence may retry the same
insert. An admitted row cannot prove whether this invocation inserted it or replayed an older
admission, so it is terminally unresolved and never signs. A lost finalize COMMIT acknowledgement
never releases the JWT and is likewise terminally unresolved.

## Online C authority

Add an authority-only DB2 adapter with fixed-capacity records and typed
`OK|DENIED|CONFLICT|EXPIRED|SEALED|INTEGRITY|UNAVAILABLE|COMMIT_AMBIGUOUS` results. It opens tenant
scope, calls only the narrow facade, requires exactly one row then DONE, validates every scalar and
digest, retains BYTEA fields before later BYTEA reads, commits, and publishes no output until commit
is known successful.

Add an authority-internal `kb_mgmt_token_authority_issue(correlation_id, jti, out)` and a separate
ordinary-kb checked-fd client wrapper. The authority validates and clears
output, obtains the admitted record, rejects replay, reads/verifies the custody HWM
anchor against the admitted version and attestation, reconstructs the immutable version-2
token-root AAD with the shared public codec, snapshots the live vault epoch, and calls the
no-fallback `kb_vault_protected_use_with_aad` entry point. Inside the protected callback:

- decode exactly one PKCS#8 RSA private key, require RSA-3072/e=65537, no trailing bytes, and derive
  its modulus/public digest/JWK kid to constant-time match the admitted C2a/C2b bindings;
- reconstruct the complete C1b claims only from the admitted C1c row;
- invoke `kb_mgmt_token_build` with a callback limited to RSASSA-PKCS1-v1_5/SHA-256;
- immediately verify the produced JWT against the admitted public JWK and exact audience, actor,
  team, capability, JTI, correlation, request digest, peer certificate, and time tuple; and
- return bytes only after that self-check.

The dedicated authority binary links only its fixed protocol, authority DB adapter, C1b builder,
vault/HWM protected-use core, crypto, and minimal platform support; isolation and plant checks fail
if generic HTTP, console, provider, publisher, provisioner, or action-dispatch objects enter it.
The protected arena and all EVP objects, decrypted DER, signature scratch, claims scratch, JWT
scratch on failure, envelope, and HWM buffers are cleansed on every exit. Cancellation remains
disabled across admission-to-use. No logs, metrics, audit detail, or errors contain token/private
bytes. Parallel calls for one correlation yield at most one protected callback.

## Failure and retry contract

Invalid/unauthorized/stale/revoked/expired/sealed/inconsistent inputs fail before protected use.
Database admission is the irreversible point: after it commits, any HWM, unseal, decode, sign,
self-verify, finalize, or output failure is terminally pre-dispatch-unresolved for that correlation.
Exact caller retries report already-used with no private operation. A different correlation/JTI cannot reuse the
journal intent or key-use record. Seal epoch or HWM change between admission and protected use
fails closed through `vault_use_begin`.

C2d emits no action outcome because it cannot know whether C3 dispatched and its durable admission
does not claim whether signing completed. C3 must append a LOCAL_FAILURE/pre-dispatch-unresolved
outcome if issuance fails after its intent exists and must never dispatch without an exact C2d OK
token. Tokens remain bounded to C1b's 90-second maximum and C2d does not extend time.

## Verification

Focused tests cover exact C1b round-trip, private/public/kid/publication binding, one callback,
32-thread/process single use, replay/conflict, output clearing, PKCS#8 trailing data, wrong key
size/exponent, wrong modulus/kid, malformed vault envelope, HWM rollback, seal race, and every
post-admission fault. ASAN/UBSAN and fuzz the bounded adapter/private-key decoder and authority
input; prove no raw signing API or token-root object enters server/publisher/provisioner targets.
Process-boundary tests run kb and authority under distinct real UIDs and prove the kb UID cannot
open authority configuration/vault descriptors or `/proc/<authority>/{mem,environ,fd}`, cannot
`ptrace`/`process_vm_readv` the daemon, cannot connect without the permitted peer credential, and
cannot substitute the root-owned socket. Root remains outside this unprivileged-compromise threat
boundary.

The real PostgreSQL 17 gate proves role attributes and exact ACL closure, FORCE-RLS crossing only
through the facade, actor/team isolation, all target/local/revocation/publication/root/vault
mutations, expiry boundaries, replica/read-only denial, WORM atomicity, immutable guards,
concurrent admission, and commit ambiguity. CT260 uses the real C2a RSA root, swtpm/KMS HWM and
production vault path to issue a token that the production C2c/server verifier accepts; sealed,
wrong-HWM, revoked, expired, tampered-publication, and 32-process races fail closed.

Run hardened kb/server/status/root-provisioner/publisher builds, target/publisher isolation and
plant checks, schema sync/alter-order/SQLite-shape checks, focused release and sanitizer/fuzz
tests, the full CT103 gate, CT260 exact-head validation, and an adversarial full-branch roundtable.
Keep `/v1/management/action` unconditionally 503.

Implementation validation found and fixed two integration-only boundary defects. A root-created
socket-activation listener reports root as the kernel listener peer even after the daemon drops to
its dedicated UID, so the client now requires that root listener credential while the daemon
still authenticates the kb UID independently. Provisioning encrypts the token root under the P5
token-root AAD domain rather than the generic tenant-vault domain, so the shared leaf codec now
owns that byte-exact encoding and the authority uses an explicit-AAD protected path with no legacy
fallback. The focused AAD design roundtable (job 8722) approved this separation conditionally on
version provenance, known-answer coverage, generic-fallback regression coverage, and link
isolation; the admitted envelope version is immutable and constrained to 2, and all requested
coverage was added.

CT260 then issued an RS256 token from the real C2a root through the dedicated unprivileged daemon,
PG17 authority role, KMS/HWM helper, protected decrypt and production C2c verifier. Exact replay
returned `ALREADY_USED`, the admission and WORM audit each remained singular, and the ordinary kb
UID could inspect neither the authority process nor its private HWM fixture.

The split adversarial branch review found three further process-boundary defects and two SQL
hardening gaps. Process hardening now runs before the first custody-provider operation; SIGINT and
SIGTERM set a durable `sig_atomic_t` stop request that the bounded listener loop observes even
after serving an in-flight request; and every transport failure after any request byte is sent is
reported as `COMMIT_AMBIGUOUS`, while a proven zero-byte send remains `UNAVAILABLE`. Both ends now
document and enforce the root-created socket-activation contract. Permanent focused regressions
cover the partial-send distinction, missing/truncated/malformed responses, output clearing,
pre-custody hardening, descriptor closure, and bounded signal shutdown.

The snapshot SECURITY DEFINER function now pins `TimeZone=UTC` before interpreting legacy
timezone-less certificate timestamps. Idempotent role provisioning removes every direct
membership edge into or out of the authority runtime, definer, and store-owner roles, and the
PG17 gate proves both the closed membership graph and correct authorization under a hostile
caller timezone. CT260 passed that gate after deliberately seeding and repairing inbound and
outbound membership edges.

The first exact-head rerun then exposed a checked-fd interaction between those fixes and the KMS
provider: after the authority intentionally closed stdio, `pipe()` could allocate descriptors 0
and 1, making the helper child's `dup2(write_fd, 1)` a no-op before cleanup closed descriptor 1.
Both KMS helper paths now create CLOEXEC pipes, raise both endpoints above stdio before `fork`, and
check the remap. Permanent decrypt and signed-HWM regressions close all three stdio descriptors
before provider use and prove the helper still returns exactly its bounded response.

The final remediation review also hardened recovery and its tests: role-edge repair uses
dependency-safe `REVOKE ... CASCADE` ordering so delegated `ADMIN OPTION` graphs cannot abort
idempotent provisioning, and PG17 proves no authority role survives as granted role, member, or
grantor. Regression builds explicitly undefine `NDEBUG` and fail compilation if assertions are
disabled. The signal test now delivers SIGTERM on the daemon thread during an accepted in-flight
issue and proves the response is completed before the listener exits without another accept.

Explicitly revoke EXECUTE on every new SECURITY DEFINER function from PUBLIC and every unrelated
role before granting the authority role, and assert the full closure in PG17. The ordinary
`aimee_kb_runtime` role receives no execution on these functions and no access to encrypted
token-root rows.
