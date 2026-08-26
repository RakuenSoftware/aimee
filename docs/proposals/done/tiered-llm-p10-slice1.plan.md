# P10 slice 1 implementation plan: vault seams in place (behavior-preserving)

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

Slice 1 of P10 (shared vault core). Branch off `testing` (P1 + P3a merged).

## Key correction to the proposal's premise (verified against the tree)

P10 §0/§1 say "there is no `src/modules/`" and frame the work as *moving* the vault
out of `src/server/` into a new top-level `src/vault/`. **That premise is stale.** The
vault already lives as a clean directory-as-module at **`src/modules/vault/`**,
`vault_crypto`, `vault_store`, `vault_kek_cache`, `vault_principal`, `vault_server_key`,
`vault_service`, `vault_capability`, and `src/modules/` is now the tree's established
module home (`delegates/`, `git/`, `audit/`, `config/`, `vault/`). Only `server_vault.c`
(HTTP handlers) and `server_vault_bootstrap.c` (boot provisioning) sit under `src/server/`
(correctly; they are server glue, which the proposal says stays in the service layer).

Consequences that reshape slice 1:

- **Do NOT physically move to `src/vault/`.** A literal move would churn the
  `-Imodules/vault` include path and rewrite ~10 `src/tests/Rules.mk` link recipes and
  cross-module test recipes that reference `$(OBJDIR)/modules/vault/*.o` by path, pure
  churn, no behavior change, higher risk. Keeping `src/modules/vault/` honors "prefer
  existing patterns, keep changes minimal" and matches every other module.
- **The DB-free core is already isolated.** `vault_crypto` and `vault_principal` have
  zero non-libc/OpenSSL deps; `vault_kek_cache` only depends on `vault_principal.h`.
  `vault_store`/`vault_server_key`/`vault_capability` depend only on `config_default_dir()`
  (from `modules/config/config.c`, which already links into BOTH binaries via `CORE_SRCS`
  surviving the KB filter) plus `platform_path`/`cJSON`/`log`. None touch sqlite/libpq/db1_.
- **Linking is already satisfiable.** `L_SERVER` and `L_KB` both link `-lssl -lcrypto`, so
  a shared vault-core needs no new library on either side.

So the real, valuable P10 intent that does NOT yet exist is the **two swappable seams**,
today `vault_store` is hardwired to one jsonfile format and `vault_server_key` is hardwired
to `file` custody. Slice 1 introduces those seams **in place**, behavior-preserving.

## Scope (slice 1 only)

1. **Storage-backend seam.** Add `modules/vault/vault_internal.h` (private) defining
   `vault_store_backend_t`, a vtable of fn-pointers matching today's `vault_store_*`
   contract (`get_or_create_salt`, `salt_readonly`, `unlock_check`, `set`, `set_dual`,
   `set_server`, `get_server`, `add_server_wraps`, `get`, `has_entry`, `list`, `delete`,
   `rekey`, `rekey_field`, `list_principals`) + a `const char *name`. Refactor the existing
   jsonfile logic in `vault_store.c` into a static `jsonfile_backend` behind that vtable;
   the public `vault_store.h` API stays byte-for-byte identical and becomes a thin facade
   dispatching to the bound backend (default = `jsonfile`). No format change, no signature
   change, `test_vault_store` passes verbatim.
2. **Custody-provider seam.** Define `vault_custody_provider_t` in the same private header:
   `unwrap(wrapped_root,len,out_root)` plus the P7 anti-rollback hooks `hwm_read(key_id) ->
   (version, attestation)` and `hwm_cas(key_id, expected, new) -> token|conflict` declared
   as **interface-only** in slice 1 (the `file` provider implements `unwrap` from today's
   master-key logic and returns `VAULT_CUSTODY_UNSUPPORTED` for hwm_*. Those land with the
   external anchors in a later slice). Refactor `vault_server_key.c` into the static
   `file_custody` provider behind the vtable; `vault_server_kek()` / `_rotate()` keep their
   exact behavior. `test_vault_server_key` + `test_vault_master_rotate` pass verbatim.
3. **Facade + profile handle (minimal).** A `vault_profile_t` = `{const vault_store_backend_t
   *store; const vault_custody_provider_t *custody; policy flags}` and a `vault_default_profile()`
   returning `{jsonfile, file, server-defaults}`. Slice 1 does NOT thread a `vault_t*` through
   every caller (that is a larger, separate refactor); it establishes the seam types and the
   default composition so P7's kb profile is a new `vault_profile_t`, not a fork. The existing
   module-global functions keep working against the default profile (no consumer signature
   changes → all 15+ consumers and 10 unit tests compile and pass unchanged).

## Explicitly deferred (later P10/P7 slices)

- Linking the vault-core object group into `aimee-kb` + a filtered `VAULT_CORE_SRCS`/KB list
  (slice 2, with the `postgres` storage backend which is kb-only). Slice 1 stays server-only,
  so the target-isolation checks are unaffected.
- `postgres` storage backend, `tpm2`/`pkcs11`/`kms` custody, seal/unseal, WORM-audited
  use-in-place, `mlock`, rotation state machine, per-slot org scoping (all P7).
- Generalizing the AAD from `principal|agent|cred` to include the kb `org|team|provider|version`
  slot scoping (P7 §5/§9). The seam makes this a profile concern, implemented with P7.

## Anti-drift proof (the P10 acceptance criterion this slice can already assert)

Once the seams exist, a core-behavior change is exercised through the seam by both the real
jsonfile backend and a **mock** backend in tests, so slice 1 adds a `test_vault_seam.c`
driving the facade against a mock `vault_store_backend_t` + mock `vault_custody_provider_t`,
proving neither seam leaks concrete-backend assumptions into the core. This is the
"custody and storage independently swappable" acceptance test, available now.

## Gate

- Every existing vault unit test passes **verbatim** (unchanged source): `test_vault_crypto`,
  `_principal`, `_kek_cache`, `_store`, `_service`, `_master_rotate`, `_bootstrap`,
  `_server_key`, `_capability`, `_audit`, plus the cross-module tests that link vault objects
  (`test_git_forge_vault`, `test_pki`, `test_oauth_reauth`, `test_server_compute`, …).
- `make lint` (incl. `kb-target-isolation-check`, `module-boundary-check`) + `make
  check-linking` + `make build-integrity` stay green (slice 1 doesn't touch the KB link line).
- New `test_vault_seam` (mock-backend anti-drift) passes.

## Non-goals (slice 1)

Not a file move; not a behavior or on-disk-format change; not the kb profile; not threading a
`vault_t*` through callers. Pure structural seam introduction, behavior-preserving.

## v2 refinements (roundtable-converged; baked into implementation)

Panel found no blocking issues; these recurring design signals are folded in:

- **Vtables carry `void *ctx` (self) as the first parameter of every fn.** A stateless
  vtable would force P7's `postgres` storage backend and `kms`/`tpm2`/`pkcs11` custody
  providers to keep connection/handle/config state in globals. `jsonfile`/`file` pass
  `NULL` (they use `config_default_dir()` and are effectively stateless), but the SHAPE
  now supports per-instance state. No churn when P7 lands.
- **Custody seam = KEK-lifecycle operations, not raw root export.** The vtable is
  `{ name; ctx; int get_kek(ctx, uint8_t kek[VAULT_KEK_LEN]); int rotate(ctx, …) }`,
  mirroring today's `vault_server_kek()` (which derives and returns a KEK internally) and
  `vault_server_key_rotate()`. This fits a non-exportable HSM/KMS provider (which never
  exports the root. It derives/unwraps internally and returns only the KEK), unlike an
  `unwrap(wrapped_root)->root` seam. The `hwm_read`/`hwm_cas` anti-rollback API is
  **deferred to the external-anchor slice** (its attestation/token/CAS contract is not yet
  concrete enough to encode a stable interface, declaring it now would bake in an
  unusable contract).
- **No `vault_profile_t` policy flags in slice 1.** Each seam has a file-static default
  backend pointer (`g_store_backend = &jsonfile_backend`, `g_custody = &file_custody`);
  P7's kb profile swaps the pointers in its own constructor. Policy (WORM, seal, mlock)
  is a kb-profile concern and lands with P7, not conflated into a slice-1 struct. This
  also removes any risk of the kb binary inheriting a by-value server profile.
- **`test_vault_seam` drives the vtables directly**: it binds a mock
  `vault_store_backend_t` and asserts (a) the facade dispatches to it and (b) the real
  `jsonfile_backend` is reachable and behaves identically through the same vtable, no
  mutable-global test override needed.
- **Gate wording:** existing tests pass unchanged because `jsonfile` remains the default
  backend and the executed code path is byte-identical (reached through one fn-pointer);
  the tests are not modified.
