# P5-B1b management-status key bootstrap and online DB boundary

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** completed; roundtable-converged and merged-ready.
- **Depends on:** P5-B1a status custody core and P7 KMS signed-HWM custody.
- **Followed by:** P5-B1c required-mTLS status authority.

## Boundary decision

The design roundtable converged on separating the first-key bootstrap from the
HTTP/TLS listener. This slice delivers an offline owner provisioner and the exact
online PostgreSQL connection boundary. It adds no listener, socket, HTTP route,
runtime image, enrollment, or challenge/health orchestration. Ordinary `aimee-kb`
still cannot link the authority custody or provisioner. Therefore the provisioned
key remains unreachable by any shipped online process until P5-B1c lands.

## Fixed bootstrap protocol

- Support only the fixed `org:p5-status / management / ed25519` platform slot and
  KMS custody with a cryptographically verified signed-HWM value of exactly 1.
  The custody key id must have no status registry, slot, secret, current pointer,
  or rotation history. Any other state fails closed rather than being repaired.
- Generate one 32-byte Ed25519 seed in the existing mlock/MADV_DONTDUMP protected
  arena. Derive the public key and deterministic wire id
  `p5-status-v1-<first-32-lowercase-hex-of-sha256-public-key>` there. Collision or
  changed-binding state fails. All seed, KEK, DEK, AAD and temporary envelope
  buffers are non-elidably cleansed on every success, error and cancellation exit.
- Under the fixed slot transaction advisory lock and an open durable seal epoch,
  stage an inert encrypted version 1, an independently encrypted candidate version
  2, a disabled singleton registry row, and one fixed 1-to-2 activation record.
  The staged row records immutable custody id, wire id, public-key digest, exact
  envelope digests and initial seal epoch so a rerun cannot regenerate or substitute.
- Outside PostgreSQL, call the provider's atomic signed-HWM CAS from 1 to 2. A
  definite compare failure is accepted only when a fresh signed read proves exact
  version 2. That reread must verify under the configured signer/domain and the
  immutable staged custody key id; finalize revalidates the custody/wire/public-key
  topology and records the verified attestation digest in its WORM event. Any other
  result is uncertain/fatal and leaves the registry disabled.
- In a second locked owner transaction, verify the complete staged binding and the
  provider-signed version-2 attestation, persist that attestation on exactly the
  version-2 secret, advance current to 2, mark the fixed rotation activated, enable
  the registry, and append a secret-free WORM activation event atomically.
- Rerun recognizes only the exact disabled staged state or the exact enabled final
  state. It decrypts the persisted version-2 candidate inside the protected arena,
  re-derives and compares the public key/wire id/envelope digests, then performs the
  missing CAS or finalization. The exact CAS-done/current-v1 state is finalized under
  the same global-then-slot-then-registry/rotation/secret/current lock order. A DB
  transaction cannot be partially committed: the unique rotation plus activated
  replay check prevents duplicate rows and the WORM event is appended only in the
  transaction that enables the registry. Same-operation recovery is deterministic; changed
  rows, binding, ciphertext, receipt, seal epoch or HWM fail closed. An already
  enabled registry returns conflict and never outputs or changes key material.
- The provisioner prints one canonical JSON object containing only custody key id,
  wire key id and base64url public key on the fresh successful finalization, with no
  timestamp, host, run id, epoch or other field. Recovery
  and already-enabled invocations do not re-emit material. Logs/errors use fixed
  classes and never contain envelopes, attestations, key bytes or provider output.

## PostgreSQL privilege boundary

- Add owner-only fixed stage/status/finalize functions with definition-time
  `pg_catalog,pg_temp` search paths and fully qualified `public.` application
  objects. Revoke CREATE on schema public from PUBLIC and assert it remains revoked.
  Revoke the functions from PUBLIC, runtime,
  `aimee_kb_status`, `aimee_kb_status_login`, and the status definer. Only the
  out-of-band migration/owner path may execute them. No generic vault provision API
  is granted to an online role.
- Make bootstrap state overwrite-resistant: immutable identity/envelope columns,
  enumerated phase transitions, WORM guards after activation, exact singleton and
  slot constraints, and one transaction-scoped advisory-lock namespace shared by
  stage, status, finalize, seal and any later maintenance path.
- Add `aimee_kb_status_login` as NOLOGIN, NOINHERIT, NOBYPASSRLS, NOSUPERUSER,
  NOCREATEDB, NOCREATEROLE, NOREPLICATION, with membership only in
  `aimee_kb_status`. Deployment alone enables LOGIN and supplies its credential.
  It owns nothing and has no direct table/sequence/function privileges.
- On an explicit `aimee_pg_open` connection, assert the exact session-role name,
  attributes, role-membership set, SET option, database/schema privileges and safe
  pg_catalog-first search path before `SET ROLE aimee_kb_status`; refuse superuser,
  BYPASSRLS, `row_security=off`, changed session authorization, owned application
  objects, unexpected default privileges, or PUBLIC schema CREATE. Then reassert the
  exact current/session roles, attributes, memberships, row-security setting and
  search path after SET ROLE. Any pre/post drift exits nonzero. The platform status
  tables remain protected by zero direct online ACLs plus fixed definer functions;
  they intentionally do not acquire tenant RLS policies.
- Move `kb_management_status_lookup` onto this explicit connection; the authority
  binary must never call ambient `db2_conn`. Extend the fixed startup function to
  return seal state plus enabled registry custody/wire binding without returning an
  envelope. A valid sealed primary may be reported, but key use stays unavailable.

## Files

- Add `db2/management_status_provision.{c,h}` and
  `kb/kb_mgmt_status_provision.{c,h}` plus a minimal
  `kb/kb_mgmt_status_provision_main.c` target.
- Extend `db2/management_status_key.{c,h}`, `schema.sql`, `schema_roles.sql` and
  `schema_grants.sql`; keep schema-only loads valid without hardened roles.
- Register focused unit/PG17/ASAN gates and extend target-isolation checks. Do not
  package the provisioner into any runtime container or install it as a service.

## Gates

- Unit tests cover deterministic public identity, protected-buffer cleanup, envelope
  construction/recovery, exact replay and every typed HWM/DB/provider exit.
- Provisioner initialization sets `PR_SET_DUMPABLE=0`, `RLIMIT_CORE=0`, refuses a
  failed mlock/MADV_DONTDUMP/MADV_WIPEONFORK setup, and gives one cleanup owner every
  decrypted-seed/KEK/DEK buffer across all recovery and cancellation paths. Postgres
  retains only AAD-bound ciphertext; storage/WAL encryption and zero online SELECT
  ACLs are its compensating at-rest boundary.
- Real PG17 covers fresh stage/finalize, kill before CAS, kill after CAS, concurrent
  provisioners, changed binding/ciphertext/receipt, already-enabled overwrite refusal,
  sealed state, audit rollback, WORM behavior, temp shadowing and exact ACL/ownership.
- Include a distinct kill after CAS success but before the finalize connection begins;
  rerun must produce one activated rotation, one publish WORM event and no regeneration.
- Envelope binding digests are SHA-256 over fixed format/version, canonical AAD,
  wrapped DEK, nonce, ciphertext and tag; AAD or byte changes produce a typed fatal
  mismatch before any auto-repair. No path overwrites or heuristically repairs staged
  immutable columns.
- Provider tests cover signed HWM 1, CAS success, compare ambiguity resolved only by
  signed read 2, rollback, forged/stale attestation and outage.
- Online DB tests cover exact login/session/current-role assertions, unexpected
  memberships/options/privileges, explicit-context lookup allow/deny/error, sealed
  startup and configured registry-binding mismatch.
- Run lint, server/authority/provisioner builds, focused ASAN+UBSAN+leak tests and
  real PostgreSQL 17 plus KMS signed-HWM validation on CT260. Plant symbol/dependency
  checks proving ordinary KB lacks provision/custody objects and no runtime artifact
  contains `aimee-kb-status-provision`.

## Deferred

P5-B1c owns the strict HTTP reader, management-client certificate profile verifier,
bounded required-mTLS listener, online authority binary/container/systemd unit and
real TLS issuance/kill matrix. P5-B2/B3 still own workload identity and the complete
challenge -> authority -> server-health topology.

## Delivery evidence

- Focused strict and ASAN/UBSAN/leak unit tests pass for the provisioner crypto/state
  machine and explicit runtime PostgreSQL adapter. Full server/KB builds, schema sync,
  target-isolation plant checks, home-path checks, and build-integrity gates pass.
- Real PostgreSQL 17 plus the CT260 KMS signed-HWM helper passed fresh HWM 1->2
  activation, canonical fresh-only JSON, enabled-row revalidation/conflict, and a
  kill-after-CAS-before-finalize recovery with one activation audit row. Symlinked
  helper paths are rejected before database or custody work.
- The final adversarial branch review converged with no actionable findings after
  adding stored-HWM2/current-HWM2/v2-binding validation for enabled rows, trusted
  root-owned path ancestry, and the provisioner's minimal vault/platform link closure.
