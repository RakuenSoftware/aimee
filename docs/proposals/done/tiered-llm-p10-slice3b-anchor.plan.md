# P10/P7 slice 3b implementation plan — vault custody selection + seal barrier (P7 §2-3)

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

Slice 3b of P10+P7. Branch off `testing` (P1, P3a, P10 s1, P10/P7 s2 merged). The
prerequisite the deferred CA-key slice revealed: **a live key (CA key, org vendor keys)
may only live in the vault under an external anchor, never bare `file` custody** (P7 §3).
This slice builds the **custody selection + seal/unseal barrier + P7 §3 dev-vs-hardened
enforcement**, testable via a **mock anchor** (real `tpm2`/`pkcs11`/`kms` need hardware —
deferred; P7's Testing section explicitly plans "tpm2/pkcs11/kms behind build flags with a
mock anchor"). Multi-instance coordination (seal-epoch-in-pg, auto-unseal, wrapped-root,
singleton lease, witness) is a further slice.

## What exists

- Custody seam `vault_custody_provider_t` (slice 1): `{name, ctx, get_kek, rotate}` +
  binder `vault_custody_set_provider()`. Default `file` provider (`vault_server_key.c`)
  derives the KEK from a 0600 master-key file (self-unsealing). No seal notion yet.
- kb binds `vault_pg_backend` at `kb_main.c:779`; the vault holds NO keys yet (slice 2).

## Design decisions

1. **Config `vault.custody`** (new kb config key; default `file`). Values: `file` |
   `mock` (test/dev anchor) | `tpm2` | `pkcs11` | `kms`. Read once at kb startup; selects
   the custody provider bound via `vault_custody_set_provider()`. `tpm2`/`pkcs11`/`kms`
   are **declared but unimplemented → fail closed at startup** with a clear typed error
   ("custody 'kms' not yet implemented; use file (dev) or mock (test)") — real anchors land
   with hardware. This keeps the config surface honest and forward-compatible.
2. **Seal state in the custody layer.** Add to the seam (or a thin `vault_seal.{c,h}`):
   `vault_seal_status() -> {SEALED, UNSEALED}`, `vault_unseal(secret,len)`,
   `vault_seal()`. Rules:
   - **`file` custody is always UNSEALED** (self-unseal from the master-key file — today's
     low-ops behavior, unchanged).
   - **A non-`file` (anchor) custody starts SEALED**: `get_kek` fails
     (`VAULT_ERR_SEALED`) until `vault_unseal` invokes the anchor. This is P7 §3's "starts
     sealed; refuses org egress until unsealed, fail-closed, no silent fallback to a
     plaintext root."
3. **Mock anchor provider** (`vault_custody_mock.{c,h}`, compiled always but only bound
   when `vault.custody=mock`; test/dev only): its `get_kek` returns a KEK derived (HKDF,
   `vault_crypto`) from a root that the mock "unwraps" ONLY after `vault_unseal` is called
   with the configured unseal secret — modelling an external anchor's `Decrypt`. Sealed →
   `get_kek` returns `VAULT_ERR_SEALED`. Proves the seam + seal/unseal barrier work with a
   non-file custody, without real hardware.
4. **P7 §3 enforcement** (`kb_vault_policy.{c,h}` or in the kb bind path):
   - `kb_vault_live_keys_allowed()` — **false under `file` custody**, true under a
     non-file custody that is currently UNSEALED. A live key (CA key / org vendor key) may
     be provisioned/loaded ONLY when this is true.
   - **Fail-closed at boot**: a `file`-custody kb that finds live-key ciphertext already in
     `org_vault_secret` (any `org:pki:*` or `org:`/`team:` secret slot) **refuses to serve
     org egress** (typed startup error) — matching §3 "fails closed at boot if such
     ciphertext is present." (It still serves non-secret paths.)
   - A non-`file` kb that comes up and cannot unseal refuses org egress until unsealed.
5. **`VAULT_ERR_SEALED`** added to `vault_status_t` (mirrors slice-2's
   `VAULT_ERR_UNSUPPORTED_OP`).

## Scope (slice 3b)

1. `vault.custody` config plumbing (kb config) + startup provider selection in the kb
   bind path (`kb_main.c` near :779), with the unimplemented-anchor fail-closed.
2. Seal/unseal state + `vault_seal.{c,h}` (or extend the custody seam); `file` always
   unsealed, anchor starts sealed. `get_kek` seal-checks.
3. `mock` anchor custody provider (test/dev).
4. `kb_vault_live_keys_allowed()` + the §3 boot fail-closed check (query `org_vault_secret`
   for live-key rows under file custody via the slice-2 definer read path).
5. `VAULT_ERR_SEALED`.
6. Tests: unit — mock anchor seal/unseal state machine (sealed → `get_kek` fails; unseal
   → succeeds; re-seal → fails again); `file` custody always unsealed; the §3 gate
   (`kb_vault_live_keys_allowed` false under file, true under unsealed mock); config
   selection incl. the unimplemented-anchor fail-closed. A real-PG-gated test that the §3
   boot check detects a live-key row under file custody and fail-closes.

## Explicitly deferred (further slices)

Real `tpm2`/`pkcs11`/`kms` providers (need hardware); multi-instance seal-epoch-in-Postgres
+ per-instance auto-unseal via workload identity + wrapped-root-in-pg (§3 scaled path);
singleton-lease fencing (§3); WORM-audited key use + witness outbox (§6); use-in-place
(§4); mlock (§5); rotation + `hwm_read`/`hwm_cas` anti-rollback (§8); the CA-key-in-vault
(rides on this slice's anchor + unseal) and org vendor keys (P2b).

## Gate

- `make -j kb` + `make -j server` link clean; `make lint` (kb-target-isolation,
  module-boundary) green. Server profile unaffected (still `file`, always unsealed).
- New seal/unseal + mock-anchor + §3-gate unit tests pass; the real-PG §3 boot-check test
  passes on CT103. Existing vault unit + seam tests unchanged.

## Non-goals (slice 3b)

No real hardware anchor; no multi-instance seal coordination; no key moved into the vault
yet (that is the CA-key slice, next, now §3-compliant); no use-in-place/WORM/rotation.

## v2 refinements (roundtable-converged; security-critical)

Panel found no blocking issue; these repeated signals reshape the slice:

- **`mock` NEVER satisfies the §3 live-key gate.** `kb_vault_live_keys_allowed()` requires
  custody ∈ {`tpm2`,`pkcs11`,`kms`} (a REAL external anchor) AND unsealed. `file` AND
  `mock` both → **false**. `mock` is a **test/dev provider only** — it exists to exercise
  the seal state machine, and is compiled in but can NEVER be the basis for holding a live
  key. (Closes "a hardened kb bypasses §3 with vault.custody=mock".)
- **Seal flushes + zeroizes the KEK.** `vault_seal()` (or the provider's seal) calls
  `vault_kek_cache_clear()` and zeroizes any cached/derived KEK, so no post-seal cached
  KEK survives (P7 §3 "sealing flushes the KEK cache"). The mock zeroizes its derived KEK
  on re-seal; a subsequent `get_kek` re-derives only after a fresh `unseal`.
- **Seal state lives in the custody PROVIDER instance, not a shared global.** Add optional
  `seal`/`unseal`/`is_sealed` to `vault_custody_provider_t` (or the provider owns the state
  in its `ctx`). The `file` provider is a no-op always-unsealed. The **server profile
  (file custody) never seals** and never sees `VAULT_ERR_SEALED`. No mutable seal flag in
  shared module state.
- **Config validation at parse.** `vault.custody` is validated against the known enum at
  config-load (reject `tpm`, `TPM2`, `kmss`, … with a clear error), not deferred to bind.
  `tpm2`/`pkcs11`/`kms` parse OK but **fail closed at provider-bind** ("not yet
  implemented; use file (dev) or mock (test)").
- **Unseal is provider-specific, not a universal passphrase.** The mock unseals from a
  config secret; the real anchors (deferred) define their own unseal (workload-identity
  `Decrypt` / TPM policy session / PKCS#11 login). The seam's unseal is an opaque
  provider callback — do NOT bake `vault_unseal(secret,len)` as THE contract.
- **`VAULT_ERR_SEALED` scoping.** Returned only by an anchor provider's `get_kek` when
  sealed; the file provider never returns it (server unaffected).

### Tightened scope (defer live-key mechanics to the CA-key slice)

Slice 3b stores NO keys, so the §3 boot-scan + the `kind`-column classifier + per-use
live-key enforcement ride with the **CA-key slice** (where a live key first exists). 3b
ships: (1) `vault.custody` config + parse validation + provider selection (unimpl anchors
fail-closed); (2) per-provider seal/unseal state (file always-unsealed; a sealed provider's
`get_kek` → `VAULT_ERR_SEALED`; seal flushes+zeroizes the KEK cache); (3) the `mock` anchor
provider (test-only, never live-key-eligible); (4) `kb_vault_live_keys_allowed()` (requires
a real unsealed anchor); (5) `VAULT_ERR_SEALED`. The CA-key slice then adds the `kind`
column, the boot fail-closed, and the central per-use enforcement at the kb_vault live-key
choke point.

### Tests (add the negative + zeroize checks)

Mock seal state machine: sealed → `get_kek` = `VAULT_ERR_SEALED`; unseal → succeeds;
re-seal → KEK cache cleared, next `get_kek` fails until re-unseal. `file` always unsealed.
`kb_vault_live_keys_allowed()` false for file AND mock (unit). Config parse rejects
typos + unimpl anchors fail at bind. Negative: a `file`-custody kb with no live keys builds
+ starts clean (server profile path unaffected).
