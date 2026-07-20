# P7-reseal-a TPM2 prepared-reseal foundation

- **State:** proposed implementation slice.
- **Depends on:** P7 TPM2 PolicyNV anti-rollback.

## Scope

Replace the TPM helper's unrecoverable increment-before-blob-write window with a
canonical, inspectable, idempotent prepared-reseal protocol. This slice changes no
Postgres schema and has no production orchestration caller. Its public operations
remain disabled infrastructure for the later barrier/staging/orchestration slices.

## API and canonical receipt

Add `vault_custody_tpm2_reseal_prepare`, `_status`, `_commit`, and `_cleanup` plus
typed status/result values. The caller supplies a fixed 16-byte random operation
ID, expected old generation, new KEK, and operator secret. The receipt is a
versioned canonical byte encoding, never a serialized C struct, and binds:

- operation ID, old and new generation (exactly G and G+1; reject overflow);
- predecessor active-blob SHA-256 and future-blob SHA-256;
- TPM manufacturer/firmware identity, persistent-primary Name, NV index Name,
  NV public attributes, policy digest, algorithm suite, and configured active-path
  identity; and
- a digest over the complete canonical manifest transcript.

All parsers reject noncanonical lengths, trailing bytes, duplicate fields, unknown
versions/algorithms, size overflow, and identity/policy disagreement. Default
non-TPM builds expose the same ABI and fail closed.

## Artifact protocol

Use a sibling 0700 directory owned by the effective service UID, opened once with
`O_DIRECTORY|O_NOFOLLOW`. All file operations are relative to that validated
directory FD; reject unsafe owner/mode, non-regular files, link counts other than
one, cross-device layout, symlinked path components, and active/artifact paths on
different filesystems. Leaf files are 0600 and bounded.

For operation O at NV G, `prepare` validates that the active v2 blob is exactly at
G and captures its digest. It creates a future active TPM blob containing the new
KEK and canonical receipt identity under PolicyNV(G+1). It also creates a temporary
continuation capsule for the same KEK under PolicyNV(G), solely so a pre-increment
restart can reproduce/verify staging in later slices. Each file is written with
no-replace semantics, fsynced, published by rename, and followed by directory
fsync. A canonical manifest binding both digests is published last. Existing
artifacts are accepted only when every canonical byte and digest matches.

The continuation capsule is not a post-increment recovery mechanism and is never
called an active blob. Once NV advances it is deliberately unusable; forward
recovery relies only on the already durable future blob. No API in this slice
returns the capsule plaintext KEK. A later staging slice may add an internal
use-in-place consumer under protected memory.

## Locking and state classification

Hold both the provider mutex and a provider-scoped `flock` for every prepare,
status, commit, cleanup, legacy reseal, and unseal reconciliation operation. The
lock covers final NV read, possible increment, active installation, and installed
verification. Re-read all state only after acquiring it. This prevents two
processes from independently advancing G.

`status` enumerates the cross-product rather than inferring success from NV:

- NV G + no manifest/artifacts: `ABSENT`;
- NV G + complete matching capsule/future/manifest: `PREPARED`;
- NV G + incomplete unpublished files: `INCOMPLETE_PREPARE` (safe cleanup/retry);
- NV G+1 + complete matching future not installed: `NV_ADVANCED`;
- NV G+1 + matching future installed as active: `INSTALLED`;
- any digest/identity mismatch, manifest without required artifacts, foreign active
  blob, NV below G or above G+1: `CONFLICT` or `CORRUPT`, fail closed.

Every classification compares operation ID, receipt/manifest transcript,
predecessor digest, future/installed digest, TPM/NV identity, generation, file
metadata, and policy. A future-installed crash before directory fsync is repaired
by verifying the active bytes and fsyncing the directory before `INSTALLED`.

## Commit and ambiguity handling

`commit` requires `PREPARED`. Immediately before increment it revalidates the
predecessor active blob, full manifest, future artifact, TPM/NV identity, and NV G.
It invokes one NV increment. After success, error, lost response, or context loss,
it reinitializes as necessary and queries NV before deciding: G is safe retry; G+1
is forward-only installation; any other value is conflict. It never issues a
second increment once G+1 is observed.

At G+1, install the already durable future artifact only while holding the lock and
only if its digest equals the manifest receipt. Rename over the active file,
fsync the directory, re-read the active blob, and verify the complete receipt,
generation, policy, and digest. Clear cached KEK and mark the provider sealed on
entry and on every exit, including failures after NV advancement.

`cleanup` is legal only for an exact receipt whose active blob is verified
installed at G+1 and only when the caller supplies an explicit terminal-completed
authorization flag; this flag is infrastructure-only until the DB slice binds it
to durable state. It deletes only matching continuation artifacts, fsyncs each
deletion set, never removes the active blob, and is idempotent for every subset.
Before NV advancement, a separate abort-cleanup is allowed only after re-reading
NV=G and verifying the predecessor remains active. Legacy one-shot reseal takes the
same locks and refuses while any prepared manifest exists.

## Validation

- Default build unit test: every new function fails closed and zeroes outputs.
- Parser/filesystem unit tests: malformed/noncanonical receipt and manifest,
  integer overflow, trailing bytes, wrong identity/policy/digest, partial artifact
  subsets, wrong owner/mode/type/link count, leaf and parent symlinks, hard links,
  no-replace races, directory substitution, and cross-device refusal.
- Real libtss2+swtpm on CT260: provision G, prepare G+1, commit, reset both process
  and swtpm, unseal only the new blob, and prove old-blob replay fails PolicyNV.
  Repeat prepare/status/commit/cleanup and race two processes plus a SIGSTOP/stale
  process; exactly one NV increment occurs.
- Kill or force command failure after every durable boundary: capsule write/fsync/
  publish, future write/fsync/publish, manifest write/fsync/publish, final pre-NV
  validation, command submission, TPM execution with response loss, NV increment,
  active rename, active fsync, directory fsync, and cleanup subsets. Restart from
  only swtpm state and the artifact directory. The exact operation reaches
  `INSTALLED`, remains safely retryable at G, or fails sealed with a typed conflict;
  it never needs the plaintext new KEK after `prepare` returns.
- ASAN/UBSAN all parser and helper exits; scan artifacts/logs for raw old/new KEK
  canaries; verify buffers use `OPENSSL_cleanse`, `mlock`, and `MADV_DONTDUMP` where
  plaintext is temporarily constructed and fail closed if protections fail.

## Deferred and disabled

No database barrier, wrap staging/promotion, startup DB reconciliation, WORM event,
route, CLI, or operator action is added here. The prepared API is not evidence that
whole-vault rotation is safe until P7-reseal-b/c/d merge.
