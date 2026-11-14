# Conditional immutable Vault custody

This generic, opt-in Vault contract is independent of application memory policy. Existing unmarked credentials retain their mutable setter and getter behavior. The new API is a trusted in-process host API, not a model tool, network-authentication mechanism or capability granted by a reference string.

`vault_store_create_server_once` and `vault_service_create_server_wrap_once` return closed Created, Replay, Conflict or refusal results. They accept a bounded secret string (at most 64 KiB), per-principal slot and caller-owned generation buffer. Only Created and Replay return an opaque generation. An existing unmarked slot conflicts; it is never silently converted.

The JSON provider holds the existing process/interprocess write lock across root loading, creation or authenticated value comparison, and durable file replacement. A missing principal root, salt and new encrypted entry are committed together. Exact same-value Replay and different-value Conflict leave file bytes unchanged. Comparison happens inside custody; no plaintext or value-derived fingerprint is returned or audited. An unsupported backend must implement the optional atomic callback or refuse; the facade never substitutes sequential read and upsert.

A newly created entry contains one `immutable_generation_v1` field: a nonzero random 128-bit identifier written as 32 lowercase hexadecimal characters. The generation is authenticated in domain-separated AES-GCM additional data together with the principal and slot. Removing or modifying it cannot turn the ciphertext into a valid legacy entry. Every mutable setter and batch upsert rejects marked entries, including same-value mutable writes. Authorized deletion replaces the entry with a no-value Retired tombstone carrying only slot names and its generation. It retains no ciphertext, wrapped keys, nonce or tag. Same-ID create retries return Retired, regardless of supplied value; neither mutable setters nor batch upserts can revive it. A new logical write requires a fresh opaque slot ID and generation. Repeated deletion preserves the tombstone without rewriting it. Retired entries are excluded from live `has`/`list` and raw getters/snapshots; generation-checked use returns explicit Retired.

Raw getters and snapshots refuse marked entries. New generation-checked provider reads and `vault_service_use_server_wrap_generation` require the exact current marker and generation before use. The service invokes its trusted callback once and cleanses its temporary plaintext. Missing incarnations return a hard generation refusal, never the legacy environment-fallback result. A callback error may occur after consumption or effects: it is not proof that nothing happened and must not trigger automatic replay. A snapshot check does not provide a durable external guard for later effects, authenticate the caller, or replace current permission checks. The caller must derive and authorize the principal and destination/purpose from verified host state; the callback must not retain secret bytes.

The reserved instance/server principal is explicitly unsupported by the new per-principal server-wrap API. Existing instance credentials use a different primary-wrap contract and retain their old APIs. Current per-principal key rewrap preserves the immutable marker and generation. User-password rotation verifies the existing user-key check and skips valid no-value tombstones and recognizable server-only wraps; malformed present user wraps and missing server-wrap metadata refuse without committing a new verifier.

A failed durable commit can have crossed rename before directory fsync failed. Its result is unresolved, not proof of absence. Retrying the same tuple and value must recover Replay with the already stored generation; no fresh overwrite is allowed. This contract does not prevent privileged file/backup rollback or establish irreversible deletion across backups. External reference users still need current authorization and any required erasure/rollback protections.

Qualification uses real encrypted JSON files, exact replay/conflict bytes, six-process same/different contenders, mutable-API refusal, generation/marker tampering, delete/recreate, key rotation, unsupported/malformed providers, and bounded fault injection around real file operations. There is no hardware-provider, general no-plaintext-to-parent or application protection claim.

Legacy-principal migration cannot copy marked or retired values into the deployment-wide namespace: its raw getter refuses marked entries, while listing hides retirements. This is tested through actual unlock/migration. The immutable purpose is cryptographically domain-separated from ordinary v1 credentials and bound to the principal, agent namespace, slot and generation; adding a generation marker to ordinary ciphertext fails authentication. No value-derived fingerprint is exposed.

## Additive provider-fenced generation use (isolated candidate)

`vault_service_use_server_wrap_generation_fenced` and the corresponding store
operation are separate from the existing snapshot callback API. The optional
backend method must retain its retirement/write fence across exact live generation
validation, decryption and one trusted synchronous callback. The JSON provider
holds its existing process mutex and interprocess `.write.lock` through callback
exit. Missing provider support returns Unsupported with no snapshot-get fallback.
The new retained capability is `aimee_vault_generation_fenced_use_contract_v1 = 1`;
immutable/Retired v2 stays unchanged.

The facade owns a 64 KiB+1 buffer, validates the backend passes that exact buffer
with a matching bounded NUL/length, invokes the caller at most once, and cleanses
its storage. A duplicate callback attempt, malformed buffer or backend error
cannot cause a second invocation. Callback error or backend failure after callback
entry reports an unknown effect (`VAULT_STORE_USE_FAILED` / service IO), never
permission to replay. Cancellation is disabled across owned buffers and provider
locks, then restored after cleanup; the trusted callback must return promptly and
must not fork, exit its thread, longjmp, change cancellation state, retain bytes
or perform asynchronous work.
There is no hard timed-IO or preemption guarantee.

The callback MUST NOT reenter Vault/key/native APIs. Known same-thread entry and
key-lock paths reject reentry before taking conflicting locks, including key
rotation/seal, mutable storage and nested fenced use. No native RPC belongs under
the Vault lock. Entry-fence release follows callback exit, before any subsequent
native completion. The existing master-key rotation remains offline; this API
does not establish online whole-directory rotation, seal-epoch authorization,
native ACL eligibility, destination permission, antirollback or exactly-once
external effects. Where required, compose the existing admitted seal-use guard
outside this entry fence. No plaintext getter, model tool or HTTP route is added.


## Independent local seal-lifetime admission

`vault_local_use_begin(expected_local_epoch, kek)` (retained capability
`aimee_vault_local_use_contract_v1 = 1`) validates the actual current local
custody epoch and unsealed provider and holds the existing seal read guard until
`vault_use_end()`. Cancellation is disabled across that new guard and restored
after unlock. The caller owns the returned key buffer and MUST cleanse it before
end, because restoring cancellation may deliver a pending cancellation immediately.
Nested use, active same-thread maintenance, fork-child reuse and entry-callback
reentry refuse and cleanse the requested output. Seal waits for the reader, then
advances the epoch so old admissions fail. No hard lock-wait deadline, persistent restart epoch or antirollback guarantee is promised.

This generic local primitive does not initialize, synchronize or replace a
primary epoch. `vault_use_begin(local, admitted_primary, kek)` retains its existing
mandatory initialized/exact-primary gate. Organization KeyUse MUST continue to use
that primary-authorized path. Local custody admission is not authorization to any
entry/value or destination; callers must independently establish that authority.
For local protected use, acquire native authorization before the local seal
guard, then the generation/retirement fence, perform only bounded trusted work,
and release the entry fence before any native completion. No native/Vault RPC
belongs inside the entry callback.
