# P7-reseal-a TPM2 prepared-reseal foundation

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** delivered and disabled pending the P7-reseal-b/c/d orchestration slices.
- **Depends on:** P7 TPM2 PolicyNV anti-rollback.

## Delivered scope

The TPM2 custody helper now exposes typed `prepare`, `status`, `commit`, `abort`,
and `cleanup` operations. Default non-TPM builds expose the same ABI, zero outputs,
and return `NOT_BUILT`. No route, operator command, Postgres mutation, or production
caller enables these helpers in this slice.

Preparation creates two canonical v2 sealed objects before changing the NV counter:
a continuation capsule at generation G and the future active object at G+1. A single
bounded `.reseal.bundle` durably binds the operation ID, G/G+1, predecessor digest,
both object digests, and supplied new-KEK digest. Nested objects are unmarshalled,
generation-checked, and checked against freshly recomputed
PolicyNV+PolicyAuthValue public policy before state classification or installation.

The provider mutex and a service-owned 0600 cross-process flock serialize prepared
operations with the legacy reseal path. Active, bundle, lock, and temporary files
are bounded regular files with no symlink following; reads require service ownership,
0600 mode, and link count one. Parents must be service-owned and not group/world
writable. Publication uses `renameat2(RENAME_NOREPLACE)` and fails closed where it
is unavailable. File and directory fsync failures propagate; idempotent retries
repeat the relevant directory fsync.

`commit` revalidates the active canonical predecessor and NV G immediately before
one increment. It re-reads NV after every increment response, including errors, and
only moves forward when G+1 is observed. At G+1 it installs the already-durable,
validated future object, then revalidates generation, policy, digest, and parent
durability. Mutation failures never inherit an earlier successful status result.
All entry and exit paths clear the cached KEK and leave the provider sealed.

`abort` mutates only the exact PREPARED operation and idempotently repairs parent
durability for the same receipt after removal. `cleanup` requires the
typed terminal-completed authorization and an exact INSTALLED operation. Missing,
unsafe, malformed, foreign, or generation-inconsistent artifacts fail closed as
CORRUPT or CONFLICT; CLEANED is exact idempotent cleanup status and is not accepted
by commit.

## Validation completed

- Default server build and fail-closed stub unit test under `-Werror`.
- CT260 with real libtss2 and swtpm: provision G, prepare, status, commit G+1,
  repeated commit, unseal only the new KEK, cleanup, and CLEANED classification.
- Existing TPM2 swtpm regression suite.
- The prepared flow under ASAN+UBSAN on CT260.
- Adversarial plan and branch roundtables, including fixes extracted from recurring
  findings even when the panel headline reported no issues.

## Explicitly deferred

P7-reseal-b/c/d own the production admission barrier, privilege inventory,
Postgres DEK staging/inventory/promotion, startup reconciliation, WORM terminal
events, kill-boundary testing, and operator enablement. Stronger filesystem
component walking, a separate artifact directory, full TPM/platform identity in
the receipt, typed BUSY, atfork reinitialization, and locked/non-dumpable transient
buffers remain helper-hardening candidates before that production enablement.
