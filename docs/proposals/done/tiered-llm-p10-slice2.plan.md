# P10/P7 slice 2 implementation plan — kb vault foundation (Postgres store)

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

Slice 2 of P10+P7. Branch off `testing` (P1, P3a, P10-slice-1 merged). Goal: give
aimee-kb a **working credential vault** — the vault core linked into the kb binary, a
Postgres storage backend, and kb binding it at startup — so later slices (CA-key-in-
vault, seal/unseal, use-in-place, WORM audit, rotation) have a real vault to harden.
This slice is the **file-custody, Postgres-store** kb vault: functional, not yet
hardened. It handles **no org vendor keys yet** (that is P2b); slice 3 moves the kb CA
key in as the first real consumer.

## Verified ground truth (from the integration map)

- `vault_store.c` **can link into aimee-kb as-is**: its only non-libc/OpenSSL deps are
  `config_default_dir()` and `cJSON`, both already in the kb binary
  (`CORE_SRCS`/`KB_CORE_OBJS`). No `db1_`/`sqlite3_` symbols; `modules/vault/` is not a
  forbidden prefix; `--gc-sections` drops the unreferenced `jsonfile_*` code once the
  pg backend is bound. **No jsonfile split required.** `server/server_vault*.c` stay
  server-only (the `server/` prefix is forbidden in the kb link — correct, they're glue).
- kb serving init: `db2_init()` loop at `kb/kb_main.c:747`; one-time post-DB2 wiring runs
  right after (`db2_rel_types_ensure_seed()` at `:769`). The vault bind goes there.
- db2 boundary: `db2_conn()` (opaque `PGconn*`) + `aimee_pg_exec/prepare/bind_*/step/
  column_*` (`db2/db_postgres.h`); canonical use in `db2/db2_tenant.c:45-85` (lease_begin
  → conn → exec/prepare/step → lease_end). Schema applied by `db_apply_schema_postgres`
  from `db2/schema.sql`.
- No existing db2 vault/secret table — genuinely new schema + code.

## Design decisions (the seam serves both profiles; kb uses a single-KEK model)

1. **Principal generalizes to the org slot key.** P10 §5: server `uid:/cert:/webuser:`
   and kb `org:/team:<id>/provider:<name>` are both "a namespaced slot key" fed to the
   same `vault_store_*` interface. The pg backend AAD-binds `principal|agent|cred` exactly
   like jsonfile, and additionally derives a `team_id` column (from the principal, when it
   is `team:<n>`) for RLS tenant scoping. `org:pki:ca-key` (slice 3) is a platform-scoped
   slot (team_id 0 / NULL).
2. **Single-KEK model, KEK from custody — not per-principal password KDF.** The server
   profile derives a per-principal KEK from a stored salt + password/root. The kb org
   vault (P7 §1) derives **one** KEK from the custody anchor and wraps every per-key DEK
   under it. So in the kb profile the KEK is supplied by `vault_custody_provider_t.get_kek()`
   (slice 2 = the existing **file** custody provider; the external anchor + seal/unseal is
   a later slice), cached in `vault_kek_cache` under a fixed org principal. The caller (a
   thin kb vault service) obtains the KEK from custody and passes it into the
   `vault_store_*` ops exactly as today. `get_or_create_salt` returns the org-wide salt row
   (one per deployment); `unlock_check` uses the existing `kek_check` verifier.
3. **The pg backend implements the kb-relevant ops; server-dual-wrap ops are unsupported.**
   `get_or_create_salt, salt_readonly, unlock_check, set, get, has_entry, list, delete,
   rekey, rekey_field, list_principals` — full envelope round-trip against Postgres. The
   **server-autonomous dual-wrap ops** (`set_dual, set_server, get_server,
   add_server_wraps`) return `VAULT_ERR`/unsupported: they encode the server's "server can
   read a user credential without the user unlocking" model, which the org vault does not
   use (its single KEK is anchor-derived, not a second server-KEK wrap). A `NULL` slot in
   the vtable is not an option (facade would crash), so they are explicit stubs returning a
   typed unsupported error. Documented in the backend.
4. **Envelope crypto is byte-identical to jsonfile** — same `vault_crypto` (random DEK,
   AES-KW wrap under KEK, AES-256-GCM secret with AAD, `kek_check` sentinel). Only the
   PERSISTENCE differs (Postgres rows vs a JSON file). So a change to core crypto is
   exercised by both backends — the P10 anti-drift property.

## Scope (slice 2)

1. **db2 schema** (`db2/schema.sql`): `org_vault_secret` — the kb ciphertext store.
   Columns: `id`, `principal` (slot key, NOT NULL), `team_id BIGINT` (FK kb_team, NULL =
   platform-scoped like the CA key), `agent`, `cred`, `version BIGINT` (immutable version
   rows for the future rotation/anti-rollback slice — slice 2 always writes version 1 and
   upserts), `wrapped_dek BYTEA`, `nonce BYTEA`, `ciphertext BYTEA`, `tag BYTEA`,
   `kek_check BYTEA` (per-deployment verifier row), `created_at`. Unique `(principal, agent,
   cred)` (slice 2 single-version); a separate `org_vault_salt(principal PK, salt BYTEA)`
   for the salt/verifier. **RLS** (invariant #10): `FORCE ROW LEVEL SECURITY`, tenant read
   scoped to `team_id ∈ principal's teams OR team_id IS NULL` (platform slots) via the P1
   `set_tenant_context`/`aimee.principal` pattern; writes admin/writer-gated. **Ciphertext
   only** — never a plaintext key, never the KEK (the KEK lives behind custody, off-DB).
2. **Postgres storage backend** `db2/vault_pg.c` (+ header) implementing
   `vault_store_backend_t`, using `db2_conn()` + `aimee_pg_*` (the db2_tenant.c pattern),
   with a `ctx` carrying nothing in slice 2 (uses the process db2 connection). kb-only
   source (joins the KB/DB2 link, never `SERVER_SRCS`).
3. **Link the vault core into aimee-kb**: a `VAULT_CORE_SRCS` object group (vault_crypto,
   principal, kek_cache, store, service, server_key, capability) appended to the `$(KB)`
   link line, kept in `SERVER_SRCS` for the server. `db2/vault_pg.c` joins the kb/DB2 side.
4. **kb binds the backend**: after `db2_init` succeeds in `kb_main.c` (~:761), call
   `vault_store_set_backend(&vault_pg_backend)`. File custody stays the default (get_kek
   works for a dev kb). A thin `kb_vault` accessor exposes set/get for slice-3's CA key.
5. **Update target-isolation allow-list** if needed (the check already permits
   `modules/vault/`; `db2/vault_pg.c` is a `db2/` source, allowed on the kb side).
6. **Tests**: unit (pg backend envelope round-trip + AAD-mismatch reject, reusing the
   vault_crypto discipline) behind the real-PG gate on CT103; RLS gate extended to prove
   cross-team `org_vault_secret` read isolation; anti-drift assert that the SAME
   `vault_crypto` path runs under the pg backend.

## Explicitly deferred (later slices)

External-anchor custody (kms/tpm/pkcs11) + seal/unseal (§2-3); use-in-place primitive
(§4); mlock/MADV_DONTDUMP (§5); WORM-audited key use + hash-chain/witness (§6); rotation
state machine + `hwm_read`/`hwm_cas` anti-rollback + immutable multi-version rows (§8);
per-team/provider DEK isolation refinement (§9); moving the CA key in (slice 3);
org vendor keys (P2b).

## Gate

- All existing vault unit tests + the seam tests still pass unchanged.
- New pg-backend test + the extended RLS isolation assertions pass on real PG17 (CT103).
- `make lint` (kb-target-isolation-check, module-boundary-check) + `make check-linking`
  (aimee-kb: no `db1_`/`sqlite3_` symbols, libpq present) + `make build-integrity` green.
- `aimee-server` and `aimee-kb` both link clean; the vault core object group appears in
  both link lines, `db2/vault_pg.o` only in kb's, `server/server_vault*.o` only in server's.

## Non-goals (slice 2)

Not hardened (no seal/anchor/use-in-place/WORM/rotation); handles no org vendor keys; not
a rewrite of the server profile; the server-dual-wrap ops are intentionally unsupported on
the pg backend.

## v2 corrections (roundtable-converged; two critical + forward-compat)

Panel found no blocking issue, but these repeated signals are baked in:

- **CRITICAL — platform-scoped rows are NEVER a tenant read.** The v1 predicate
  `team_id ∈ principal's teams OR team_id IS NULL` leaks every `team_id IS NULL` row
  (the CA key) to every tenant. Corrected RLS:
  - tenant SELECT policy: `team_id IS NOT NULL AND team_id ∈ principal's teams`
    (via the P1 membership subquery on `aimee.principal`).
  - platform-scoped (`team_id IS NULL`) rows are readable ONLY by
    `kb_principal_is_admin()` and written/read by the SECURITY DEFINER vault-service
    path — never by an ordinary tenant session. The CA key (slice 3) is such a row and
    is thus invisible to tenants at the DB layer.
  - `org_vault_secret` uses `FORCE ROW LEVEL SECURITY`; writes go through a SECURITY
    DEFINER writer function (like P3a's metering fns), no direct tenant DML.
- **CRITICAL — sequencing / P7 §3.** Slice 2 stores **no keys** — it is plumbing +
  tests only. Storing the kb CA key (slice 3) or any org vendor key under **bare file
  custody** is what P7 §3 forbids on a key-holding kb. Therefore: (a) the file-custody
  kb vault is explicitly **single-instance dev mode**; (b) slice 3's CA-key-in-vault
  **fails closed** on a hardened/multi-instance deployment until the external-anchor +
  seal slice lands, keeping today's plaintext-PEM path only on an explicit dev box; (c)
  the external-anchor + seal/unseal slice is reordered to land **before or with** the
  CA-key-in-vault activation on any non-dev deployment. Slice 2 merging a keyless vault
  is a coherent intermediate under this gate.
- **Version-pointer schema (no rotation migration).** Adopt the proven P3a pattern
  instead of `UNIQUE(principal,agent,cred)` + upsert-in-place (which the panel flagged
  as mutable-in-place forcing a disruptive rotation migration):
  `org_vault_secret UNIQUE(principal, agent, cred, version)` (immutable rows) +
  `org_vault_current(principal, agent, cred → version)` pointer advanced atomically.
  Slice 2 writes version 1 and reads via the pointer; rotation later is purely additive.
- **AAD binds version.** AAD = `principal|agent|cred|version` (not just
  `principal|agent|cred`), so a restored older-version ciphertext cannot be presented as
  current — the AAD itself refuses it. Cheap now, essential for the rotation/anti-rollback
  slice.
- **Single `kek_check` verifier, not per-secret.** The per-deployment KEK verifier lives
  as ONE row in `org_vault_salt(principal, salt, kek_check)` (or a dedicated verifier
  row), not duplicated per `org_vault_secret` row — removes the guess-a-KEK oracle the
  panel flagged.
- **Typed unsupported error.** The four server-dual-wrap ops return
  `VAULT_ERR_UNSUPPORTED_OP` (a distinct code), so the kb vault service fails loud/clear,
  not conflated with a crypto/storage error.
- **kb_vault accessor** (`modules/vault/kb_vault.{c,h}` or `kb/kb_vault.*`) enforces the
  principal→team_id mapping: it refuses to `set` a `team:<N>` principal whose team_id
  diverges from the active tenant context. Named + specified in slice 3.
- **Build check named:** `server/server_vault*.o` staying out of the kb link is enforced
  by `kb-target-isolation-check` (the `server/` FORBIDDEN prefix) + `check-linking`.
