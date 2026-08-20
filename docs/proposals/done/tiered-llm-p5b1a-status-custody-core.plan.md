# P5-B1a management-status custody core

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** delivered, independently fail-disabled.
- **Depends on:** P5-B status foundation and P7 KMS signed-HWM custody.

## Delivered scope

The primary database now has a fixed platform management-status key registry and
immutable key-use intent ledger. Candidate and admission functions accept only the
fixed status slot, an activated/current KMS-HWM-attested version, and the exact wire
key id. Admission revalidates and locks caller enrollment, team authority, target
registry/enrollment, revocation generation, key binding and seal epoch; it appends a
digest-bound intent and secret-free WORM event in the same transaction before returning
an encrypted envelope. Exact replay returns no envelope and changed replay inputs fail.

The online status login is NOINHERIT, NOBYPASSRLS and has only the four fixed function
grants. A separate no-login status-definer role owns exactly those functions and has the
narrow BYPASSRLS/table/helper authority required to cross the fleet's FORCE-RLS tables;
the repository-wide owner remains NOBYPASSRLS. Definition-time search paths and a real
temp-shadow gate prevent relation substitution. Direct runtime/status access and intent
mutation are denied, including by WORM triggers.

The C custody callback uses an authority-owned libpq connection, fresh signed-HWM
verification, a process-wide serialized candidate-to-guard sequence, and both durable
primary and local seal guards. The existing sensitive decrypt/use block is shared in a
mlock/MADV_DONTDUMP arena, admits exactly 32 Ed25519 seed bytes, signs only the existing
domain-separated status transcript, and non-elidably cleans every plaintext path.
Cancellation cannot strand a transaction/lock or retain a signature. Ordinary
`aimee-kb` neither links nor exposes the status custody/adapter objects.

## Validation completed

- Full lint, schema sync/alter-order, SQLite shape and planted target-isolation gates.
- `-Werror` authority-core/server builds and registered custody, explicit-connection,
  generic key-use and SQLite DB unit tests; repeated mutex/cancellation stress.
- Focused custody and explicit-connection tests under ASAN+UBSAN with leak detection.
- Fresh real PostgreSQL 17 on CT260: schema-only load, hardened role/owner/ACL/temp-shadow
  assertions, fresh admission/WORM ordering, audit rollback, digest-bound replay, and
  deterministic revoke/disable/rotation/seal concurrency.
- Updated P7 primary maintenance-barrier gate and diff whitespace checks.

## Explicitly deferred

P5-B1b owns the separately deployed required-mTLS status listener and owner-only,
overwrite-refusing KMS provisioning/bootstrap command. The status-key registry remains
empty/disabled, so this core has no reachable live signing path. Workload identity and
full challenge -> authority -> health orchestration remain P5-B2/P5-B3 respectively.
