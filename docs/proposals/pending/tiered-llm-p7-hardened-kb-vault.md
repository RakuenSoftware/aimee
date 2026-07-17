# Proposal: P7 — Hardened ("hardcore") vault for aimee-kb

- **State:** proposed (pending — not started). Part of `tiered-llm-offering.md`.
- **Author:** JBailes (drafted by the engineer agent, 2026-07-17).
- **Depends on:** the server vault crypto primitives (reused). **Must land with or
  immediately after P2** — do not entrust real org keys to kb before this exists.

## Thesis

Once P2 lands, aimee-kb holds **every org vendor key for every team** in one place —
the single highest-value secret store in the whole system, and the exact target the
commercial gateways spend real infrastructure protecting. Today **aimee-kb has no
vault at all**: its most sensitive secret, the enrollment **CA private key**, is an
*unencrypted* PKCS#8 PEM at mode 0600 (`src/kb/pki.c:49-69`). This packet gives kb a
hardened vault by reusing the server's mature envelope-encryption core and adding
the layer it lacks: an external root of trust, a seal barrier, use-in-place
semantics (never emit plaintext), memory locking, and audited key use.

## Goal

A kb vault that: encrypts every org vendor key (and kb's own CA key) at rest under
envelope encryption; roots its master key in an **external** trust anchor
(KMS/HSM/TPM), not a local file; **starts sealed**; **uses** keys in place for
egress rather than returning plaintext; locks key memory; **audits every key use**
to the WORM ledger; and isolates keys per team/provider to bound blast radius.

## §0 What already exists — reuse vs. missing

**Reuse as-is (mature, tested, pure — directly linkable from kb):**
- `src/server/vault_crypto.{c,h}` — KEK = HKDF-SHA256 (or scrypt for password
  roots), DEK = random 32 B **AES-KW (RFC 3394)** wrapped under the KEK, secret =
  **AES-256-GCM** with **AAD = "principal|agent|cred"** (binds ciphertext to its
  slot; blocks substitution/rollback). Fail-closed, `OPENSSL_cleanse` throughout.
- `vault_store.c` envelope file format (0600, atomic tmp+rename+fsync, `kek_check`
  verifier, dual-wrap fields so a daemon can self-start after restart).
- `vault_kek_cache.c` (RAM-only, 64 slots, 900 s TTL, reject-don't-evict, cleanse).
- `vault_server_key.c` master-key **rotation** (mint new → re-wrap all principals →
  backup → probe-verify → commit/restore).

**Missing for "hardcore" (this packet adds each):** ① no seal/unseal barrier (the
master key auto-decrypts from a local 0600 file at start); ② no external root of
trust (no KMS/HSM/TPM/PKCS#11 hook — the root is always a local file); ③ no
memory locking (`mlock`/`MADV_DONTDUMP` absent — keys are swappable and
core-dumpable); ④ **fetch, not use-in-place** — `vault_service_get`/
`_inject_api_key` copy plaintext into the caller's buffer; ⑤ **no access audit** —
vault reads are never recorded in the WORM ledger; ⑥ kb's CA key is plaintext;
⑦ rotation is manual and master-only.

## §1 kb vault on the reused core

Link `vault_crypto`, `vault_store`, and `vault_kek_cache` into aimee-kb and
instantiate a kb-owned vault whose principals are org-scoped (`org:` /
`team:<id>` / `provider:<name>`). Each org vendor key is stored under its own DEK,
AAD-bound to `team|provider|cred`. Use the dual-wrap self-start model so the kb
daemon can restart unattended in non-sealed deployments, and the seal barrier (§3)
in hardened ones.

## §2 External root of trust (the core "hardcore" upgrade)

Introduce a **KEK-custody provider seam**: in hardened mode, the master/root key
is never a bare local file; it is unwrapped by an external anchor at unseal:
- `file` (default; preserves today's behaviour and keeps low-ops single-box
  installs working),
- `tpm2` (seal the root to the box's TPM PCRs),
- `pkcs11` (HSM / YubiHSM),
- `kms` (AWS KMS / Cloud KMS `Decrypt` of a wrapped root — pairs naturally with
  the P6 AWS integration).

One narrow interface (`kek_custody_unwrap(wrapped_root) → root`) sits in front of
all providers. The on-disk artifact becomes a *wrapped* root blob, useless
without the anchor.

## §3 Seal / unseal barrier

The vault **starts sealed**: it cannot decrypt any org key until unsealed. Unseal
invokes the §2 custody provider — KMS auto-unseal for hands-off ops; TPM or HSM;
or an operator unseal key / Shamir quorum for the highest-assurance posture. A
sealed kb serves everything non-secret but refuses org egress with a clear typed
error until unsealed. Fail-closed, with no silent fallback to a plaintext root.

## §4 Use-in-place (never emit plaintext) — the key property for an org key store

Add a **use-not-fetch** primitive: the vault attaches the org key to an outbound
egress request *inside the vault boundary* and returns only the result, so the
plaintext key never crosses the `/v1` API or lands in a request struct the rest
of kb can read. The P2 `/v1/llm/egress` path calls this primitive; no route ever
returns an org key. (Contrast today's `vault_service_inject_api_key`, which
copies plaintext into the caller's buffer.) This is what makes "kb holds every
org key" tolerable: a bug elsewhere in kb cannot exfiltrate the keys because
they are never handed out.

## §5 Memory hygiene

Close the gap: `mlock` the pages holding KEKs, DEKs, and plaintext keys, and
`madvise(MADV_DONTDUMP)` them, so org keys are not swapped to disk or written to
a core dump. Keep the existing `OPENSSL_cleanse`-on-free discipline.

## §6 WORM-audited key use

Every decrypt/use of an org key writes an entry to the existing WORM audit ledger
(`src/audit_ledger.c`, `src/db2/kb_audit_worm.c`): identity/team, timestamp,
`provider:cred`, and request id — **never the secret or a fingerprint of it**.
This lets an operator answer "who used vendor key X and when," and it feeds the
P5 operator-audit surface. Today, vault reads are completely unaudited.

## §7 Move kb's CA key behind the vault

Stop persisting the enrollment CA private key as plaintext PKCS#8
(`pki.c:49-69`). Store it as a vault credential (`org:pki:ca-key`), decrypted
only in memory at sign time and cleansed after. This is the single highest-risk
at-rest secret in kb today and the first thing the vault should protect.

## §8 Rotation

Extend rotation beyond the master key: per-DEK rotation, org-key **value**
rotation (swap the upstream vendor key without downtime, re-wrap in place), and
an optional scheduled cadence. Reuse the backup → probe-verify → commit/restore
discipline from `vault_server_key_rotate`.

## §9 Per-team / per-provider isolation

Each org key is its own DEK in its own principal slot (`team:<id>` /
`provider:<name>`), AAD-bound. Compromise or rotation of one key never exposes
another, bounding blast radius across teams and vendors.

## Acceptance criteria

- kb stores an org vendor key as AES-256-GCM, AAD-bound; the on-disk artifact
  contains no plaintext, KEK, or DEK.
- In hardened mode the vault starts **sealed** and refuses org egress until
  unsealed via the configured custody provider; `file` mode still self-starts
  for low-ops boxes.
- No `/v1` route ever returns an org key; egress works via the use-in-place
  primitive (the plaintext never appears in any API response or log).
- Every org-key use produces exactly one WORM ledger entry with identity/team/
  provider/request-id and **no secret material**.
- The kb CA key is no longer a plaintext file; enrollment still issues certs.
- Rotating an org key's value keeps egress working with the new key and renders
  the old ciphertext undecryptable.
- Key pages are `mlock`ed and `MADV_DONTDUMP` (verified); a forced core dump
  contains no key bytes.

## Testing

Unit: envelope round-trip + AAD-mismatch rejection (reuse vault_crypto tests),
seal/unseal state machine, each custody provider (file always; tpm2/pkcs11/kms
behind build flags with a mock anchor), use-in-place returns result-not-secret,
WORM entry shape (no secret), rotation re-wrap + old-ciphertext-fails.
Integration: sealed-kb refuses egress → unseal → egress works; CA-key-in-vault
enrollment end-to-end; core-dump scan asserts no key bytes.

## Non-goals

No mandatory external HSM for low-ops single-box installs (`file` custody stays
the default; hardened anchors are opt-in). Not a general-purpose secrets manager
for arbitrary app secrets — scope is org vendor keys + the kb CA key. No change
to the server's per-user vault (this is the kb tier).
