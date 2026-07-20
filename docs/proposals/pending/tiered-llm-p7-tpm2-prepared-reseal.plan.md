# P7-reseal-a TPM2 prepared-reseal foundation

- **State:** proposed implementation slice, revised after adversarial review.
- **Depends on:** P7 TPM2 PolicyNV anti-rollback.

## Scope

Replace the TPM helper's unrecoverable increment-before-blob-write window with a
canonical, inspectable, idempotent prepared-reseal protocol. This slice changes no
Postgres schema and has no production orchestration caller. Its operations remain
disabled infrastructure for the later barrier/staging/orchestration slices.

## API and canonical encoding

Add `vault_custody_tpm2_reseal_prepare`, `_status`, `_commit`, `_abort`, and
`_cleanup` with typed result/status values. The caller supplies a fixed 16-byte
random operation ID, expected old generation, new KEK, and operator secret.

The format is acyclic. TPM objects contain only a fixed payload-version tag, KEK,
and generation. Their SHA-256 hashes are computed after creation. A fixed-order
manifest contains magic/version/total length, operation ID, G/G+1, algorithm and
TPM/NV/policy identities, predecessor/capsule/future hashes, and tail magic. The
public receipt contains the same fixed header/hashes plus
`SHA256("p7.reseal.manifest.v1" || manifest_bytes)`. Neither object embeds the
receipt or a digest depending on its own bytes.

Integers are big-endian; byte strings are fixed-size or explicitly big-endian
length-prefixed. Parsers reject noncanonical lengths, trailing bytes, unknown
versions/algorithms, size overflow, generation overflow, and identity/policy
disagreement. The receipt binds operation ID, G/G+1, predecessor/future/capsule
digests, TPM manufacturer/firmware identity, persistent-primary Name, NV Name and
public attributes, both policy digests, algorithm suite, and active-path identity.
It is never a serialized C struct. Default builds expose the ABI, return typed
`NOT_BUILT`, zero every output receipt/status, and fail closed.

## Secure singleton artifact protocol

Use a sibling 0700 artifact directory and independently validated 0700 active-file
parent, owned by the service UID and opened per operation with
`O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC`. All operations are relative to those FDs.
Reject unsafe owner/mode, non-regular files, link count other than one, symlinked
path components, and different `st_dev` values. Files are 0600 and bounded. This
is a single-host local-journaled-filesystem protocol with atomic same-directory
rename and durable file/directory fsync; hostile root/service-UID directory
replacement is outside the threat model.

The pending namespace is fixed and singleton: `pending.capsule`,
`pending.future`, and `pending.manifest`. For O at NV G, `prepare` verifies the
active v2 blob at G and captures its digest. It creates:

1. a continuation capsule for the new KEK under the existing PolicyNV(G) template;
2. a future active object for the same KEK under a trial-computed PolicyNV(G+1)
   template (object creation does not evaluate the future policy; unseal later does);
3. the canonical manifest binding both objects to O, G/G+1, predecessor, TPM/NV
   identity and policies.

Each artifact is written via an `O_EXCL|O_NOFOLLOW` temporary, fsynced, published
by same-directory no-replace rename, then directory-fsynced. Manifest publishes
last. A complete existing set is accepted only on exact canonical equality. At NV
G, a capsule/future subset without a manifest is `INCOMPLETE_PREPARE`; retry for
the same O unseals the capsule, compares the supplied KEK in protected memory, and
rebuilds randomized future/manifest bytes rather than trusting an unverifiable
partial future. A different O or KEK conflicts. No API returns capsule plaintext.
After NV advances the capsule is intentionally unusable; forward recovery relies
only on the already durable future blob.

## Locking and normative states

Hold the provider mutex then a provider-scoped cross-process flock for prepare,
status, commit, abort, cleanup, legacy reseal, and unseal reconciliation. The
stable `provider.lock` is 0600, service-owned, regular with link count one, never
deleted, opened per acquisition with `O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC`, and
locked `LOCK_EX`; release flock before mutex. Fork is forbidden while held. Lock
contention returns typed `BUSY`, never success. All final NV reads, increments,
installation and verification occur while locked.

`status` maps the reachable cross-product deterministically:

- NV G, no pending files/tombstones: `ABSENT`;
- NV G, capsule and/or future without manifest: `INCOMPLETE_PREPARE`;
- NV G, exact capsule+future+manifest and predecessor: `PREPARED`;
- NV G+1, exact pending set and predecessor still active: `NV_ADVANCED`;
- NV G+1, exact future also installed active: `INSTALLED`;
- NV G+1, installed active plus matching cleanup tombstone/subset: `CLEANED`;
- manifest without required artifacts, receipt/digest/identity disagreement,
  foreign operation, foreign active blob, NV below G or above G+1: `CONFLICT` or
  `CORRUPT`, always sealed.

Every status compares operation ID, manifest transcript, predecessor/future/
installed digests, TPM/NV identity, generation, metadata, and policy. A crash after
active rename but before directory fsync is repaired by verifying active bytes and
fsyncing its parent before `INSTALLED`. `INSTALLED` proves only TPM/artifact state;
later slices must separately quiesce processes already holding an old plaintext KEK.

## Commit and TPM command ambiguity

`commit` accepts `PREPARED`, `NV_ADVANCED`, or `INSTALLED`. `INSTALLED` is exact
idempotent success. From `PREPARED`, it revalidates predecessor, manifest, future,
TPM/NV identity and NV G immediately before issuing one increment. It queries NV
after success, error, or lost response while holding the same lock. G permits at
most one bounded retry; G+1 is forward-only installation; all other values are
conflict. Client response loss never triggers guessed TPM startup or a blind second
increment. A fresh call starting `NV_ADVANCED` installs without incrementing.

At G+1, preserve `pending.future` as recovery evidence. Copy its verified bytes to
an `O_EXCL` 0600 temporary inside the active parent, fsync it, same-directory rename
over the active name, fsync the active parent, re-read active, and verify generation,
policy and digest before returning `INSTALLED`. No cross-directory rename occurs.
Cached KEK is cleared and the provider is marked sealed on entry and every exit,
including post-increment failures.

## Abort, cleanup, and secrets

`abort` is legal only at NV G with the exact predecessor still active. It writes
and fsyncs a fixed abort tombstone, removes matching pending artifacts with
directory fsync, then removes/fsyncs the tombstone; restart completes any subset.
`cleanup` requires exact installed receipt plus typed
`VAULT_TPM2_CLEANUP_TERMINAL_COMPLETED`; any other value is rejected before disk
mutation. It similarly journals deletion of capsule/future/manifest. Neither path
ever removes active or lock files. Legacy one-shot reseal uses the same locks and
refuses while any partial/complete pending state or tombstone exists.

The operator secret remains solely the existing TPM NV/object authValue; it is not
repurposed as an HMAC key. Internal derived auth/session buffers are always
cleansed, never logged or persisted. Caller-owned `const` secret/KEK buffers remain
the caller's cleanse responsibility. Internal plaintext KEK copies require
`mlock` and `MADV_DONTDUMP`, are never inherited across fork, and use
`OPENSSL_cleanse` on every exit; inability to establish protection fails closed.

## Validation

- Default build: every new call returns `NOT_BUILT` and zeroes output structs.
- Parser/filesystem unit and ASAN/UBSAN: noncanonical receipt/manifest, overflow,
  trailing bytes, wrong identity/policy/digest, every partial artifact/tombstone
  subset, unsafe owner/mode/type/link count, leaf/parent symlink, hard link,
  no-replace race, directory substitution, and cross-device `st_dev` refusal.
- CT260 real libtss2+swtpm: provision G, prepare/commit G+1, reset both process and
  swtpm, unseal only new, and prove old replay fails PolicyNV. Repeat every call;
  race two processes and a SIGSTOP/stale peer; exactly one increment occurs.
- Kill/fail after every durable boundary: capsule/future/manifest write, fsync and
  publish; final validation; command submission; successful TPM execution with
  response dropped; NV increment; active temp/fsync/rename/dir-fsync; and every
  abort/cleanup tombstone/deletion subset. Restart from only swtpm state and files.
  The same O reaches `INSTALLED`, remains retryable at G, or stays sealed with a
  typed conflict; it never requires plaintext new KEK after manifest publication.
- Scan artifacts/logs for old/new KEK canaries and assert protected buffers are
  cleansed on every helper/parser exit. The response-drop test uses a TCTI proxy
  fault, not a generic process kill.

## Deferred and disabled

No Postgres barrier, wrap staging/promotion, startup DB reconciliation, WORM event,
route, CLI, or operator action is added. The prepared API is not evidence that
whole-vault rotation is safe until P7-reseal-b/c/d merge.
