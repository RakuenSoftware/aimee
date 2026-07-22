# Proposal: P10 — Shared vault core (one vault, two profiles)

- **State:** proposed (pending — not started). Part of `tiered-llm-offering.md`.
- **Author:** JBailes (drafted by the engineer agent, 2026-07-18).
- **Depends on:** nothing new — it is an **extraction** of the already-built aimee-server
  credential vault (WP-C) into a self-contained module. It is a **prerequisite of P7**:
  P7 becomes the *kb hardening profile* on top of this core. Sequencing: P10's
  behavior-preserving server refactor may land anytime; the **kb profile** must be in
  place before/with P2b (same gate P7 already carries).

## Thesis

After P2/P7 the system has **two** credential vaults: the existing per-user server vault
(`src/server/vault_*.c`, holding each user's personal vendor keys) and the new hardened
kb org vault (P7, holding every org's vendor keys, multi-tenant, stateless-at-scale).
They already share DNA — P7's own header says *"depends on the server vault crypto
primitives (reused)."* Two vaults that share crypto but are otherwise built and evolved
separately is precisely the shape that drifts: a fix or hardening applied to one silently
skips the other, and the AAD/wrap/rotation invariants fork.

**Extract one vault module — linked into both binaries — instantiated as two profiles.**
A single assurance-neutral core
(envelope crypto, use-in-place, rotation + anti-rollback, memory hygiene) sits behind two
narrow seams — **custody** (root-of-trust for the KEK) and **storage** (where ciphertext
lives). The **server profile** selects `{file custody, local file/SQLite storage,
single-user}`; the **kb profile** selects `{external-anchor custody, Postgres storage,
multi-tenant, sealed, WORM-audited}`. Assurance is chosen by **profile composition, not by
a pile of caller booleans** — the kb profile's mandatory-ness (seal, WORM, non-`file`
custody for live keys, mlock) is enforced *inside the profile*, so no wrong flag can
silently produce a weak multi-tenant vault.

## §0 What already exists

The server vault (WP-C) is already cleanly layered — this proposal mostly *moves* it, it
does not invent it:

- **Crypto core** — `src/server/vault_crypto.{c,h}`. Pure OpenSSL, stateless. KEK =
  HKDF-SHA256 (or scrypt N=2¹⁷ for the webuser/password path); DEK = fresh random 32B;
  secret = AES-256-GCM under DEK with **AAD = `principal|agent|cred`** (binds ciphertext to
  its slot); DEK is AES-KW (RFC 3394) wrapped under the KEK. `OPENSSL_cleanse` on every
  path. **This is the shared core, verbatim.**
- **Store** — `src/server/vault_store.{c,h}`. Today: one `<b64url(principal)>.json` 0600
  file per principal. API (`vault_store_get_or_create_salt`, `_unlock_check`, `_set`,
  `_set_dual`, `_get`, `_list`, `_delete`, `_rekey`, `_list_principals`) is already a
  storage-shaped interface — a natural seam.
- **KEK cache** — `src/server/vault_kek_cache.{c,h}`. RAM-only, principal-keyed, TTL'd,
  cleanse-on-evict, reject-don't-evict at capacity. **Gap:** no `mlock`/`MADV_DONTDUMP`
  (the KEK sits in ordinary heap) — fine for single-user, **required** by the kb profile.
- **Principal** — `src/server/vault_principal.{c,h}`. Identity from *attested transport
  only* (`attested_transport_t`: `UDS_PEERCRED`/`MTLS_CLIENT`/…), never the request body;
  principals `uid:<n>` / `cert:<CN>` / `webuser:<name>` / shared `server`. Fail-closed.
- **Custody of the root** — `src/server/vault_server_key.{c,h}`. Server KEK = HKDF over a
  0600 auto-minted `.vault/.server-master.key` file. **A file-key root of trust, no
  KMS/TPM** — exactly P7's "`file` custody = dev/low-ops" tier. Includes full rotate.
- **Service / HTTP / capability / bootstrap** — `vault_service.c` (`_unlock`, `_set` dual-wrap,
  `_get`, **`_inject_api_key`** = the use-path), `server_vault.c` (`/v1/vault/*`),
  `vault_capability.c` (the `vault:write:server` 0600 allow-list), `server_vault_bootstrap.c`
  (boot-seals delegate keys, cleanses env). A second, simpler store also exists —
  `src/db1/secrets.c`, a **pluggable backend facade** (OS keyring | plaintext file) — which
  is both a custody option (`keyring`) and the module template below.
- **kb org vault** — **does not exist**; P7 is proposal-only. So the kb side is a *new*
  build on the extracted core, not a merge of two shipped things (lower risk).
- **Module pattern** — there is no `src/modules/`; the tree's real pattern is
  **directory-as-module with a facade header + a private internal header carrying a vtable**,
  and `src/db1/secrets` (public `secrets.h`, private `secrets_internal.h` with
  `db1_secret_backend_t`, platform impls in `src/mac`/`src/linux`) is the cleanest template.
  `src/shared/` is the *declared* cross-service boundary for source shared by server and kb.

## §1 The extraction — one module, linked into both binaries

`aimee-server` and `aimee-kb` are **separate persistent processes with a compile-enforced
boundary**: server links `-lsqlite3` and is built `-DAIMEE_DB2_DISABLED` (**never** libpq);
kb links `-lpq` (**never** sqlite); a build-integrity check enforces "standalone aimee-kb
target isolation." The vault is a **first-class module linked into both** — like the
existing `aimee-core`/`aimee-data` static libs — **not** a source snippet copied into each,
and **not** a separate vault daemon (it runs in-process in whichever service loads it).

That boundary makes the two seams **load-bearing, not merely tidy**: a single shared vault
module is only possible if the shared part carries **zero DB dependencies**. So the module
is split by link-time reach:

- **`aimee-vault-core`** (static lib, links into **both** binaries) — `vault_crypto`,
  use-in-place, rotation + `hwm_read`/`hwm_cas`, memory hygiene, the principal/scope logic,
  and the **seam *interfaces*** (`vault_custody_provider_t`, `vault_store_backend_t`). It
  pulls in OpenSSL only — **no libpq, no sqlite3, no `config.o`-style server/kb singletons.**
- **Storage backends** are separate objects linked per service: `jsonfile`/`sqlite` →
  **aimee-server only**; `postgres` → **aimee-kb only**. Neither service ever links the
  other's backend, so the vault cannot smuggle libpq into the server or sqlite into kb.
- **Custody providers** likewise: `file`/`keyring` link server-side; `tpm2`/`pkcs11`/`kms`
  link kb-side (a single box may opt the server up to `tpm2`).

Concretely: create **`src/vault/`** (facade `vault/vault.h` + private
`vault/vault_internal.h` carrying the two vtables), following the `db1/secrets` template;
the core's public contract lives at the `src/shared/` boundary since both services depend
on it. Move `vault_crypto`, `vault_kek_cache`, `vault_principal` in largely verbatim;
`vault_store` becomes the storage-seam interface + the `jsonfile` backend; `vault_server_key`
becomes the `file` custody provider; `db1/secrets`' keyring becomes the `keyring` provider.
Server/kb glue (HTTP handlers, bootstrap) stays in `src/server/` and `src/kb/` and calls the
facade. A `vault_t` is opened against a **profile** binding {custody provider, storage
backend, policy}; all callers use the same facade regardless of profile.

## §2 The core (assurance-neutral — identical in both profiles)

- **Envelope crypto** — `vault_crypto` unchanged: HKDF/scrypt KEK, random DEK, AES-256-GCM
  with AAD binding, AES-KW DEK wrap, fail-closed + cleanse.
- **Use-in-place** — the one operation that never returns plaintext across the API: the
  vault decrypts a DEK, uses the secret **inside** the module (inject into an outbound
  request / SigV4-sign / OAuth-refresh), and cleanses. Generalize the existing
  `vault_service_inject_api_key` into `vault_use(vault, slot, use_cb)`; **no `vault_get`
  that returns a raw org key exists in the kb profile** (invariant #1).
- **Rotation + anti-rollback** — per-key **immutable versioned rows** (no in-place replace)
  plus the custody seam's attested high-water: `hwm_read(key_id) → (version, attestation)`
  and `hwm_cas(key_id, expected, new) → signed_token | conflict`; activation *is* the
  `hwm_cas(N→N+1)` commit point (spec'd in P7 §8, now a core operation both profiles get).
- **Memory hygiene** — cleanse-on-evict everywhere; `mlock` + `MADV_DONTDUMP` on the KEK
  cache offered by the core and **required by the kb profile** (closes the server-cache gap).

## §3 Custody seam (root of trust for the KEK)

A vtable `vault_custody_provider_t`: `unwrap(wrapped_root) → root`, plus `hwm_read` /
`hwm_cas` (§2). Providers:

| provider | root of trust | profile use |
|---|---|---|
| `file` | 0600 master-key file (today's `vault_server_key`) | server default; **kb dev-only, keyless** |
| `keyring` | OS keyring (`db1/secrets`) | server (desktop) |
| `tpm2` | TPM NV-sealed + NV counter/quote | kb single pinned box (low-ops live keys) |
| `pkcs11` | HSM unwrap + monotonic counter | kb, highest assurance |
| `kms` | KMS `Decrypt` of wrapped root (+ external counter — KMS has no counter) | kb scaled |

The provider is the *only* place custody differs; the core is oblivious to which one is
bound. **Live-key rule (from invariant #6):** a `file` provider holding a live key fails
closed — `file` is keyless dev mode; live keys require `tpm2`/`pkcs11`/`kms`.

## §4 Storage seam (where ciphertext lives)

A vtable `vault_store_backend_t` generalizing today's `vault_store_*` (get/set/list/delete
/rekey/salt/unlock-check). Backends:

- **`jsonfile`** — today's per-principal 0600 JSON (server default; zero migration).
- **`sqlite`** — optional server backend (DB1) for callers already on SQLite.
- **`postgres`** — kb backend (DB2): **envelope ciphertext only**, AAD-bound, so a full DB
  (incl. replicas/backups/WAL) compromise yields no usable key (invariant #10). Tenant
  isolation (team-scoped predicate / RLS) is enforced at this backend for the kb profile.

Storage holds **only ciphertext + wrap metadata**; the KEK/root never enters it (it lives
behind the custody seam), so custody and storage stay independently substitutable.

## §5 Identity / scope

The principal generalizes: server principals (`uid:`/`cert:`/`webuser:`/`server`) and the
kb scope `(org, team, provider)` are both "a namespaced slot key." The **AAD generalizes
with it** — server `principal|agent|cred`, kb `org|team|provider|version` — so per-team /
per-provider DEK isolation (P7 §9) is just the kb profile's slot-scoping, using the same
AAD-binding mechanism, not a new crypto path.

## §6 The two profiles

Assurance is a profile, enforced internally:

| requirement | **server profile** | **kb profile** |
|---|---|---|
| custody | `file` (or `keyring`) | external anchor req'd for live keys; `file`=dev-keyless |
| storage | `jsonfile` (or `sqlite`), local | `postgres`, ciphertext-only, tenant-isolated |
| scope | single user principal | `(org, team, provider)`, per-slot DEK isolation |
| statelessness | single process | N stateless instances; per-instance KEK cache only |
| seal / unseal | optional | **required**; auto-unseal via workload identity at scale |
| audit | optional (existing server-write audit sink) | **WORM default-on, non-disableable**, hash-chain, fail-closed at boot |
| memory hygiene | cleanse (mlock optional) | cleanse **+ mlock + MADV_DONTDUMP** |
| use-in-place / envelope crypto / rotation | shared core | shared core |

The kb profile **refuses to construct** if a mandatory property is unmet (live key under
`file` custody, WORM off, non-sealed boot on a scaled deployment) — mandatory-ness lives in
the profile constructor, never in a caller flag. The server profile keeps today's low-ops
defaults unchanged. A single-box operator who wants hardening can opt the *server* profile
up to `tpm2` custody without touching the core — a free benefit of the extraction.

## §7 Sequencing & migration

1. **Behavior-preserving server refactor** — move server vault into `src/vault/` behind the
   facade with the `{file, jsonfile}` profile; on-disk format, `/v1/vault/*`, and all
   consumers (`agent_config.c`, `oauth_tokens.c`, `git_forge_vault.c`, `pki.c`, …) unchanged.
   Pure refactor, its own PR, gated by the existing server vault unit tests passing verbatim.
   *Independent — may land anytime.*
2. **kb profile** — P7's hardening (seal/unseal, external custody, WORM, per-slot isolation,
   Postgres ciphertext store, stateless KEK cache, auto-unseal) implemented **as the kb
   profile of this core**. This carries P7's existing gate: in place **before/with P2b**.

## Non-goals

- Not a rewrite of the server vault's semantics or on-disk format — step 1 is
  behavior-preserving; any format change is a separate, later migration.
- Not a general-purpose secrets manager — scope is vendor/OAuth credentials + the kb CA key.
- Does not force the server to adopt hardening — TPM/seal/WORM stay opt-in for the server
  profile; only the kb profile mandates them.

## Acceptance criteria

- The server vault, refactored onto the core, passes its **existing** unit tests unchanged;
  `/v1/vault/*` and every consumer behave identically (golden-file on-disk round-trip).
- One `vault_crypto` / one use-in-place / one rotation path is exercised by **both** profiles
  in tests (a change to core behavior shows up in both profiles' tests — the anti-drift proof).
- The kb profile **fails to construct** under each mandatory violation (live key on `file`
  custody; WORM disabled; non-sealed boot when scaled) — a negative test per case.
- No API on the kb profile returns a raw org key (static + runtime assertion, mirrors P2b's
  "no org key on the server" guard).
- Custody and storage are independently swappable: a core-level test drives the facade
  against **mock** custody + storage backends (and each real backend against its own seam)
  to prove neither seam leaks into the other.
- **Link-boundary preserved (build-integrity):** `aimee-server` links `aimee-vault-core`
  with **no libpq** and builds under `-DAIMEE_DB2_DISABLED`; `aimee-kb` links it with **no
  sqlite3**; `make build-integrity` (incl. the standalone-aimee-kb-target-isolation check)
  passes with the vault module present — the `postgres` backend object appears only in the
  kb link line, `jsonfile`/`sqlite` only in the server's, and `aimee-vault-core` in both.
