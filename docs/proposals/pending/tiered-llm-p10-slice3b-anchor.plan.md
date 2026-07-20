# P10/P7 slice 3b implementation plan — vault custody selection + seal barrier (P7 §2-3)

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
