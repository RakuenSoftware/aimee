# Proposal: P7 — Hardened ("hardcore") vault for aimee-kb

- **State:** proposed (pending — not started). Part of `tiered-llm-offering.md`.
- **Author:** JBailes (drafted by the engineer agent, 2026-07-17).
- **Depends on:** **P10 (shared vault core)** — P7 is the **kb *hardening profile*** on
  that core, not a separate vault. The assurance-neutral machinery (envelope crypto,
  use-in-place, rotation + `hwm_read`/`hwm_cas` anti-rollback, memory hygiene) and the
  custody/storage seams live in P10; this proposal specifies the kb profile's *mandatory*
  policy (external custody, seal/auto-unseal, WORM non-disableable, per-slot isolation,
  Postgres ciphertext store, stateless KEK cache). **Must land before or with P2b** — the
  first slice that handles a real key. Only **P2a** (catalog-only, no keys) may precede it;
  no key-handling or credential-provisioning route ships without it, so there is no window
  where P2 holds a live org key before this exists.

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
  slot, blocking substitution; note AAD does **not** prevent version *rollback* by
  itself — that needs the external epoch of §8). Fail-closed, `OPENSSL_cleanse`
  throughout.
- `vault_store.c` — reuse the **envelope record format** (`kek_check` verifier,
  dual-wrap fields), but **not its file persistence**. At scale the authoritative
  store is Postgres, so P7 defines a **Postgres vault repository** (atomic
  ciphertext+version writes, AAD and `team|provider` tenant predicates, rotation via
  compare-and-swap) in place of the 0600 tmp+rename+fsync file. The file backend
  remains only for the single-instance `file`-custody dev box.
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

Link the `vault_crypto` envelope core (and the `vault_store` record format) into
aimee-kb and instantiate a kb-owned vault whose principals are org-scoped (`org:` /
`team:<id>` / `provider:<name>`). Each org vendor key is stored under its own DEK,
AAD-bound to `team|provider|cred`. **Ciphertext + wrap metadata are always authoritative
in the database vault repository (§0)** — invariants #9/#10 hold in *every* mode; the
single-instance dev box runs its own local DB, it does **not** get a file-based ciphertext
store. The only thing `file` custody changes is **where the KEK/root anchor lives** (a
local file vs. an external anchor) — never where ciphertext is persisted. Writes are
atomic (per-key **immutable version rows**, tenant predicates); a use-in-place read
resolves the version the **custody anchor attests as current** (§8) — verifying its
signed `(key_id, version)` token — and decrypts under *that* version. Rotation never
rewrites a row in place: it **stages an immutable `N+1` row** beside `N` and advances
the anchor's **attested current version** from `N` to `N+1` (§8). Because version rows
are immutable and egress binds to the attested current version inside the admission
transaction (§6), a use can never observe a torn read or a mid-rotation,
partially-rewrapped state — it reads either the fully-staged `N` or the fully-staged
`N+1`, never a hybrid. Use the dual-wrap self-start model so the kb
daemon can restart unattended in non-sealed deployments, and the seal barrier (§3)
in hardened ones.

**Across N stateless kb instances (invariant #9):** the encrypted key material and
wrap metadata are authoritative in **shared Postgres**, not on any instance's local
disk; each instance holds only a **per-instance KEK cache** (RAM, cleansed) and
unseals **independently** against the shared external anchor (§2/§3). The cache holds
the KEK derived (HKDF, `vault_crypto`) from the anchor-unwrapped root; the per-key
**DEKs live wrapped in Postgres** and are unwrapped under that KEK into RAM only at
use. Every instance derives the *same* KEK from the same root, so any instance can
unwrap any team's DEK — no instance-specific KEK diverges. **Postgres
holds only AAD-bound ciphertext** — never a plaintext org key, never the KEK/root
(invariant #10): the envelope encryption runs before the row is written, so a dump
of the primary, a read replica, or a backup yields no usable key; the unwrap anchor
lives outside Postgres. kb↔Postgres is always TLS under a least-privilege role. This is a
further reason a bare `file` root cannot serve a scaled kb: a local-file root cannot
be shared or coordinated across an arbitrary, changing set of instances — the
external anchor (KMS/HSM) is exactly what lets any instance unseal on its own. The
`file`-default therefore applies only to the single-instance single-tenant case.

## §2 External root of trust (the core "hardcore" upgrade)

Introduce a **KEK-custody provider seam**: in hardened mode, the master/root key
is never a bare local file; it is unwrapped by an external anchor at unseal:
- `file` (default; preserves today's behaviour and keeps low-ops single-box
  installs working),
- `tpm2` (seal the root to the box's TPM PCRs) — **valid only for a single, pinned
  machine** (a TPM-sealed blob cannot be unwrapped by a fresh instance on a different
  host, so this is a single-instance / dev-box option, not a scaled one; a scaled kb
  uses a *networked* anchor below, or TPM *remote attestation* to such a service, never
  local PCR sealing),
- `pkcs11` (HSM / YubiHSM),
- `kms` (AWS KMS / Cloud KMS `Decrypt` of a wrapped root — pairs naturally with
  the P6 AWS integration).

The custody seam exposes `kek_custody_unwrap(wrapped_root) → root` **plus a per-key
attested high-water API** — not a bare monotonic tick, because §8's anti-rollback
requires *reading* and *atomically advancing* an anchor-attested per-key current
version, which a single `epoch()` primitive cannot express:
- **`hwm_read(key_id) → (version, attestation)`** — an authoritative read of the
  key's current attested version, returning a **signed attestation bound to
  `(vault-identity, key_id, version, deployment-domain)`** so a reader can prove which
  version the anchor considers current (and cannot be replayed against another vault or key).
- **`hwm_cas(key_id, expected_version, new_version) → signed_token | conflict`** — an
  **atomic compare-and-set advance** that moves the anchor's current version from
  `expected` to `new` only if `expected` is still current, returning the custodian's
  signed `(key_id, new_version)` token, else `conflict` (a concurrent rotation won).
  Advance is never a blind increment.

Each backend realizes **both** operations: **TPM** (per-key NV-indexed counter + a
signed attestation quote), **HSM** (per-key monotonic counter + signing key), or an
**external high-water service** (a conditional-write counter — DynamoDB/etcd — fronted
by a signer). A bare `kms` `Decrypt` provides neither read-CAS nor attestation and
**must** be paired with one of these. The on-disk artifact becomes a *wrapped* root
blob, useless without the anchor.

## §3 Seal / unseal barrier

The vault **starts sealed**: it cannot decrypt any org key until unsealed. Unseal
invokes the §2 custody provider — KMS auto-unseal for hands-off ops; TPM or HSM;
or an operator unseal key / Shamir quorum for the highest-assurance posture. A
sealed kb serves everything non-secret but refuses org egress with a clear typed
error until unsealed. Fail-closed, with no silent fallback to a plaintext root.

**Seal affects admission only.** Sealing flushes the KEK cache and refuses *new*
org-egress requests; a call already dispatched to the vendor (its org key already
attached inside the vault boundary) runs to completion — because no new decrypt is
possible post-seal, no in-flight call can re-fetch a key, and seal never aborts an
in-flight vendor stream mid-response. (An operator wanting to kill in-flight calls
uses egress cancellation, not seal.)

**Seal is a shared decision, honored per instance.** With many stateless instances,
seal state lives in Postgres (a `sealed` flag / epoch, invariant #9): an operator
seal flips it, and every instance observes it, refuses new org egress, and flushes
its own KEK cache; unseal re-arms all instances (each re-derives its KEK via the
anchor). The admission-only rule above then applies per instance to its own
in-flight calls.

**Use-admission is epoch-checked on the primary — a cached KEK is not enough.**
Every decrypt/use is admitted in a **single Postgres-primary transaction** that both
(a) appends the WORM admission event (§6) and (b) verifies the **current seal
epoch**; the short-lived use-capability it returns is **bound to that epoch**. So an
instance still holding a stale cached KEK cannot admit a use after another instance
sealed the vault — its epoch no longer matches, the atomic admission fails, and it
flushes. Notify/poll may accelerate cache flush, but correctness rests on the atomic
epoch check in the admission transaction, never on flush latency.

**How a fresh instance consumes keys at scale (auto-unseal).** Horizontal scaling is
unbounded, so unseal must be **unattended and per-instance** — no operator step, no
shared unseal secret, and nothing decryptable baked into the image. The wrapped-root blob lives in
Postgres (ciphertext, useless without the anchor). A new aimee-kb connects to Postgres
with its **platform-provisioned DB credential** (verify-full TLS) to read the blob —
that credential is delivered by the platform's own secret store (cloud IAM DB auth /
workload-identity DB token / a mounted rotated secret), **not baked into the image**,
and is scoped to the least-privilege runtime role; it can read only ciphertext and
cannot decrypt anything —
then unwraps it by authenticating to the external anchor **as itself** — DB access and
vault unseal are **independent trust paths** (the DB credential cannot decrypt the
root; the anchor grant cannot read the DB). The Postgres client credential is
**platform-provisioned and is *not* itself sealed in the vault** — that would be
circular (you'd need the vault to open the DB that holds the wrapped root). kb unwraps
using a **platform workload
identity** it obtains at boot **independent of aimee's own PKI** — a cloud IAM role,
SPIFFE identity, or TPM attestation. It is deliberately **not** a kb-CA-issued
certificate: the kb CA key lives *inside* the vault being unsealed (§7) and is used
**only after unseal** to issue/verify certs — unseal itself never needs the CA (it uses
the platform workload identity), so there is no circularity between §3 and §7. So
bootstrapping unseal off a kb-issued cert would be circular. The anchor's access
policy grants only the **kb service's platform identity** permission to
`Decrypt`/unwrap the root — **not** any enrollment `cert:CN` — so a leaked server or
thin-client cert cannot unseal, and a stolen Postgres dump cannot either (it holds
only ciphertext and has no anchor grant, invariant #10). This is
standard envelope auto-unseal (KMS `Decrypt`, HSM/PKCS#11, or TPM): the trust
bootstrap is the instance's authenticated identity, not a secret in the image.
Revoking a compromised instance = revoke its workload identity at the anchor, which
stops it — and any impersonator — from unsealing; the org-key ciphertext in Postgres
is unaffected. The `file` root cannot do any of this (no per-instance attestation,
no shared unwrap) — one more reason it is single-instance only.

**Multi-tenant kb must start sealed under an external anchor — and bare `file` custody
is keyless dev mode.** The bare `file` custody default and unattended non-sealed
self-start (§2, §1) are permitted **only for a single-instance, single-org
(dev/personal) box that holds no live org keys**: a `file`-custody kb **refuses to
provision or load any org vendor credential or the kb CA private key**, and **fails
closed at boot** if such ciphertext is present. The moment a box holds live keys it
must select a non-`file` anchor — **TPM** for the low-ops single pinned single-org
box, or **KMS/HSM** for any **scaled** deployment (more than one kb instance — the
production norm) or any deployment holding **more than one organization's** keys;
both start sealed (a `file` root cannot even distribute across instances, §1).
Invariant #6 is binding, not opt-in; a kb that comes up non-sealed on a bare `file`
root — or one holding live-key ciphertext under `file` custody — fails closed for
org egress.

**Single-instance is enforced by an atomic lease, not by trust.** A `file`-custody kb
must acquire a unique **singleton lease** at boot — a **single durable lease row** in
Postgres (owner id + heartbeat/TTL) taken under `SELECT … FOR UPDATE`, not a
per-connection session advisory lock (which invariant #9 rules out) — and a second
instance starting on the same `file` root cannot acquire it and **refuses to start**.
The lease carries a **monotonic fencing token**: every write or key-use guarded by it
includes the token, and Postgres rejects a stale one via CAS — so an original instance
that pauses past lease expiry and later resumes is **fenced out** (its old token loses
to the new holder's), and two instances can never both act as the singleton across an
expiry/resume. So two instances can never each believe they are
the allowed singleton — any attempt to scale a `file`-custody deployment is caught
atomically and forces the move to an external anchor.

## §4 Use-in-place (never emit plaintext) — the key property for an org key store

Add a **use-not-fetch** primitive: the vault attaches the org key to an outbound
egress request *inside the vault boundary* and returns only the result, so the
plaintext key never crosses the `/v1` API or lands in a request struct the rest
of kb can read. The P2 `/v1/llm/egress` path calls this primitive; no route ever
returns an org key. (Contrast today's `vault_service_inject_api_key`, which
copies plaintext into the caller's buffer.) This is what makes "kb holds every
org key" tolerable: a bug elsewhere in kb cannot exfiltrate the keys **through any
API response, log, or persisted/long-lived struct**, because they are never handed
out there.

**What use-in-place does and does not guarantee (honest scope).** In-process
use-in-place is **not a hard isolation boundary**: the outbound TLS/HTTP stack must
receive the credential in process memory to sign or authenticate the vendor call,
and `mlock`/`MADV_DONTDUMP` (§5) prevent swap and core-dump capture but do not stop
another code path *in the same process* from reading those bytes. The guarantee
this primitive delivers is therefore scoped and enforceable, not absolute: the
plaintext key is never returned over any API, never persisted, never logged, and
never lands in a long-lived struct the rest of kb reads.

To make that scope real rather than aspirational, the credential-bearing outbound path
uses a **controlled request builder over a secure, `mlock`ed, non-dumpable arena**: the
key is placed only into buffers the signer owns and cleanses, and the request is handed
to the transport with the credential already in an owned buffer — kb does **not** hand the
raw key to a general HTTP library's internal allocator. Where a specific transport cannot
guarantee that (its internals copy the header into unmanaged buffers), that key/provider
is routed through the **stronger boundary instead**, which for the highest-value store is
the *recommended production posture*, not a "future upgrade": a **separate egress-broker
process** (the raw key never enters the main kb address space), an **HSM/KMS-backed signing
operation**, or a **provider-issued short-lived credential handle**. So the contradiction is
resolved by construction: we do not simultaneously claim "every copy is cleansed" and "we
can't control library copies" — we either fully control the copies (owned arena) or we do
not let the raw key reach that path at all (broker/HSM). Tests instrument the **actual
outbound path** and assert no key bytes survive after the call or in a forced core dump;
a transport that fails that assertion is disqualified from the in-process path.

## §5 Memory hygiene

`mlock`/`MADV_DONTDUMP` **fail closed**: if either protection cannot be established at
startup (e.g. a restrictive `RLIMIT_MEMLOCK`), a key-holding kb **refuses to start** (or
refuses org egress) rather than run with swappable/core-dumpable key memory — never a
silent degrade. Close the gap: `mlock` the pages holding KEKs, DEKs, and plaintext keys, and
`madvise(MADV_DONTDUMP)` them, so org keys are not swapped to disk or written to
a core dump. Keep the existing `OPENSSL_cleanse`-on-free discipline.

## §6 WORM-audited key use

Every decrypt/use of an org key writes an entry to the existing WORM audit ledger
(`src/audit_ledger.c`, `src/db2/kb_audit_worm.c`) — **one shared Postgres ledger
across all instances** (invariant #9), so key-use history is complete regardless of
which instance served the call: identity/team, timestamp,
`provider:cred`, and request id — **never the secret or a fingerprint of it**.
This lets an operator answer "who used vendor key X and when," and it feeds the
P5 operator-audit surface. Today, vault reads are completely unaudited.

**Audit admission gates key use (fail-closed).** A successful, durable WORM append
is a **prerequisite** to using the key: if the audit write fails, egress is refused
and the key is not used — no org key is ever used unaudited. The protocol is **append-only**:
a durable **pre-use admission** event is appended *before* the key is used
(idempotent on request/use id — a retry appends nothing new), and an optional
**outcome** event (success/failure) is appended *afterward*, linked by the same id.
Nothing is ever mutated in place; "audited key use" means **exactly one admission
event** exists per use, with zero-or-one linked outcome events.

**Concurrent appends are totally ordered; tamper is detectable independently of
Postgres.** With many instances appending, the hash-chain needs a single total
order, so appends serialize through a **durable** single-writer point — a monotonic
Postgres sequence assigned inside a `SERIALIZABLE` transaction that reads-and-extends
the current head — and each entry links the unique prior hash. (The serialization is
the transaction + sequence, both durable DB state — **not** a per-connection session
advisory lock, which invariant #9 rules out as authoritative.) Because a sufficiently privileged Postgres actor could
rewrite rows, integrity does not rest on Postgres alone: **each entry's hash is
enqueued to a durable *shared* witness-outbox (a Postgres table — never
instance-local state, which invariant #9 forbids and which would vanish on autoscale
teardown), and any instance drains that outbox to the off-host append-only sink
asynchronously, in batches** — so a key use is not blocked on a synchronous off-host
round-trip (that would be a fleet-wide throughput choke), yet the pending evidence
survives any single instance's death and **must drain**. The in-DB hash chain provides
**integrity ordering but not durability of evidence** — a full DB compromise can
rewrite both the chain and the outbox, which is exactly why the off-host witness is
authoritative for tamper detection. The backlog threshold is a **build-time constant on
any key-holding kb** (not an operator config knob that could be set to infinity to
defeat fail-closed). If the sink is unreachable and the unwitnessed backlog in the shared outbox exceeds a
bounded threshold, new key use is **gated (fail-closed)** rather than proceeding
without recoverable tamper evidence — witnessing is **never silently skipped** for a
key-holding kb; the only tunable is the backlog bound, not whether to witness. A
periodically **signed head** is the cheap backstop, and per-entry witnessing (not just
the head) closes the rewrite-after-checkpoint window. The order-assigning sequence is a **single
shared Postgres sequence** (not per-instance). To avoid a fleet-wide hotspot at ~20+
instances, the chain is **pre-sharded into per-(tenant, provider) sub-chains** — each
ordered by an **atomic per-shard counter *row*** (`UPDATE … SET seq = seq + 1 …
RETURNING`), **not** a runtime-created Postgres `SEQUENCE` object (which would be
runtime DDL the no-DDL invariant forbids); the shard counter row is created lazily by
DML upsert, so no runtime DDL is needed and each shard still has its **own total order**
(sufficient for that tenant's tamper-evidence). There is **no single global total order
across all entries** — that would re-serialize every append on one head; instead a
**periodic global checkpoint** signs and links the current sub-chain heads on a defined
cadence (**every N entries or T seconds, whichever first**), giving a verifiable
cross-shard anchor without a global write per entry — the checkpoint interval bounds the
cross-shard reordering an attacker could hide. The unwitnessed-evidence bound is
**global, not per-instance**, and is **reserved atomically inside the admission
transaction** (like every other concurrency-sensitive op, invariant #9) — not a
check-then-act: admission does `UPDATE witness_budget SET unwitnessed = unwitnessed + 1
WHERE unwitnessed < :absolute_ceiling RETURNING …`, and if it updates zero rows the
budget is exhausted and admission **refuses** new key use (fail-closed) — it does not
silently degrade to read-only. The reserved slot is released when the entry drains to the
off-host witness. Because instance count is unbounded, a per-instance or concurrency-scaled
threshold could not bound total unwitnessed evidence, so the ceiling is absolute and shared. The ceiling combines a
**per-shard allowance with the global cap**, so one saturated `(tenant, provider)` shard
does not starve a different tenant's legitimate burst (and one tenant cannot consume the
whole global budget of unwitnessed evidence).

**At-most-once dispatch via a durable intent record** (true exactly-once is *not*
claimed against an external vendor). A durable dispatch record
(`admitted → dispatching → settled`) is written *before* the vendor call, keyed by a
**`(tenant, authenticated_origin, request_id)`** primary key — *not* a bare global
`request_id` — with the row also persisting a **canonical request hash** plus the
provider/model/credential identity; admission is an `INSERT … ON CONFLICT DO NOTHING
RETURNING`, and on a conflict kb **verifies the incoming request's immutable metadata
matches the stored row and rejects any conflict whose canonical hash differs** — so an
untrusted caller cannot reuse an id for a *different* operation to ride a
previously-admitted request, and conflict handling never exposes another tenant's
state. Exactly one instance wins the admit and any legitimate retry or second instance
reads the existing committed row (that is how an instance decides "past admitted"
without ambiguity across N stateless instances). The **`admitted → dispatching`
transition is itself an atomic CAS** — `UPDATE … SET state='dispatching', owner=me
WHERE state='admitted' RETURNING` — so exactly one instance wins the *dispatch* (not
merely the insert): a second instance that observed the `admitted` row updates zero
rows and does **not** call the vendor. Settlement (T2) takes `SELECT … FOR UPDATE` on
that row — two instances can never both dispatch. A retry
never re-dispatches once the record is past `admitted`. **Recovery ownership is
atomic:** the dispatch row carries an `owner` + lease/heartbeat, and a recovering
instance claims a stuck `dispatching` row only via `UPDATE … SET owner = me WHERE
state = 'dispatching' AND lease_expired … RETURNING` (compare-and-set), so exactly one
instance takes over settlement of a crashed request — recoverers cannot race. Where the vendor supports an
**idempotency key**, kb sends the request/use id as that key so even a network-level
retry is de-duplicated vendor-side. Where it does not, a crash *after* the request is
on the wire is **resolved conservatively**: the call is marked uncertain, its
reservation settles at reserved-max, and kb does **not** auto-re-dispatch — surfaced
as a failed call rather than risk a double charge (the outcome event records
`uncertain`). So one admission maps to **at most one** vendor dispatch; genuine
exactly-once holds only where vendor-side idempotency is available (which kb uses when
it is). The admission transaction — WORM append + dispatch record + seal-epoch check —
**commits on the primary before** the signer callback attaches the key and the vendor
call is made; the key is never attached until admission has durably committed. This is
**fail-closed on integrity**: if the primary or the WORM-append path is unavailable,
admission cannot commit and org egress is **refused, not allowed unaudited** — a
deliberate integrity-over-availability choice for org-key use (org egress pauses;
personal `direct` egress and non-key reads are unaffected). The pause is **bounded by
Postgres HA failover** — the deployment runs a replicated primary with automatic
failover, so "primary unavailable" is a failover window, not an indefinite outage; the
proposal assumes HA Postgres, it does not treat a single primary as a permanent SPOF.
The admission **use-capability is bound to `(seal_epoch, key_version)`** and re-checked
at key-attach time, so a capability admitted just before a seal or a version advance
cannot attach a stale key — it fails the re-check. The residual check-to-attach window
(between the re-check and the actual attach) is **exactly the documented "seal affects
admission only; a call already dispatched completes" boundary** — a seal landing in that
sub-millisecond window is treated as the call being already in-flight, which is the
accepted semantics, not an unclosed hole; nothing *new* decrypts after seal. **Honest residual:** a DB compromise
that erases both the admission row and its not-yet-drained outbox entry before the
off-host witness drains leaves that one use unwitnessed; the fail-closed backlog bound
(§6) caps how many uses can sit undrained, and the highest-assurance posture drains
synchronously — the window is bounded, not zero, and the proposal says so.

**Default-on, and non-disableable on a key-holding kb.** The WORM ledger and its
**tamper-evidence** (the hash-chained entries and the chain-verification/
evidence-integrity checks) are **enabled by default** — not an opt-in flag. Any kb
that holds live org keys (i.e. any multi-tenant or scaled deployment) **refuses to
start or to serve org egress with WORM or its chain verification disabled**:
config validation fails closed at boot, so a key-holding kb can never silently run
unaudited or with tamper-evidence off. Turning it off is a single-instance,
key-less dev-box affordance only — and even there it defaults on. Chain
verification runs continuously (append-time link check plus a periodic full-chain
verify), and a detected break raises a typed integrity alert rather than being
swallowed. Retention and immutability
are the ledger's actual WORM/DB2 properties (verified), not merely its name.

## §7 Move kb's CA key behind the vault

Stop persisting the enrollment CA private key as plaintext PKCS#8
(`pki.c:49-69`). Store it as a vault credential (`org:pki:ca-key`), decrypted
only in memory at sign time and cleansed after. This is the single highest-risk
at-rest secret in kb today and the first thing the vault should protect.

## §8 Rotation

Extend rotation beyond the master key: per-DEK rotation, org-key **value**
rotation (store the new vendor key's ciphertext and **revoke the old key at the
vendor** — that upstream revocation, not deletion of bytes, is what makes a
superseded value useless; a copy of the old ciphertext plus its DEK would still
decrypt, so the security rests on revocation), and an optional scheduled cadence.
**Anti-rollback:** because AES-GCM AAD binds a ciphertext to its *slot* but not to
*time*, a restored old ciphertext row would otherwise resurrect a rotated-out key;
bind each stored version to a **per-key monotonic version** checked against *that
key's own* high-water mark — **not a single global epoch**, which would invalidate
every other key's still-current version whenever any one key rotates. The
non-rollbackable high-water storage is the custody seam's per-key attested high-water
API (`hwm_read`/`hwm_cas` over TPM NV / HSM counter / external high-water-mark, §2;
**not** a bare KMS `Decrypt`, which has neither counter nor attestation). Crucially the
high-water is **not merely a Postgres integer** (a full DB compromise could rewrite both
it and the ciphertext): `hwm_cas` returns a **signed token over `(key_id, new_version)`**
and `hwm_read` a signed attestation of the current version, stored beside the ciphertext
row, and on use kb **verifies that token against the custodian's public/attestation key**
before accepting the version — so a DB rewrite cannot forge a rolled-back version. A
stored version below a key's own current high-water is refused, so restoring an old
backup row cannot roll *that* key back while leaving unrelated keys untouched.
**Activation is exactly the `hwm_cas(key_id, N → N+1)` advance (§2), the single commit
point:** a crash *before* it leaves `N` current with a harmless staged `N+1` row (retried
idempotently); a crash *after* it leaves `N+1` current and the old `N` refused — there is
no window in which both or neither is current, and a `conflict` return is never treated as
success.

**Rotation is a multi-version, crash-safe state machine** (there is **no CAS that
replaces `N`** — versions are immutable rows that coexist). A durable primary-backed
`rotation(key_id, state)` row drives it: **(1) provision** the new vendor credential at
the vendor; **(2) stage** — insert `N+1` as an **immutable staged row** (does not touch
`N`) with the custodian's signed `(key_id, N+1)` token; **(3) probe** the staged version
with a validation call; **(4) activate** — atomically advance the **anchor's attested
current version** from `N` to `N+1`; **(5) revoke** the old credential at the vendor;
**(6) retire** — `N` is kept only as the anti-rollback floor. Each step is idempotent and
resumable from the `rotation` row after a crash. **Acceptance is defined by the anchor,
not Postgres:** egress uses the version the **custody anchor attests as current** (verified
via its signed token), never the "max committed" row a DB compromise could set — so
restoring an older ciphertext `M <` current is refused (its token is below the attested
current). Because `N+1` is inert-but-committed until step (4), between staging and
activation the attested current is still `N` (fully usable) and there is **neither** a
window where the attested version is unavailable **nor** one where a retired version is
used; a crash before step (4) leaves the anchor attesting `N` (safe), and a resume
completes activation. **In-flight capabilities are version-bound** (re-checked against the
attested current inside the admission transaction, §6), and a **compromise-driven**
rotation additionally seals the key across the brief activation so `N` cannot be used at
all. Reads **fail closed** if the anchor is unavailable; per-key identifiers are stable.
Reuse the backup → probe-verify → commit/restore discipline from `vault_server_key_rotate`.

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
- The kb org-key vault exposes **no plaintext-returning accessor at any layer** —
  not just no `/v1` route. There is no `vault_service_get`/`_inject_api_key`-style
  symbol over org principals; the use-in-place primitive (§4) is the *only* code
  path that ever touches an org-key plaintext, and it returns the egress result,
  never the key. A CI symbol/grep guard asserts no org-principal read path copies
  plaintext into a caller buffer.
- Every org-key use produces **exactly one append-only WORM admission entry** (plus
  zero-or-one linked outcome entries) with identity/team/provider/request-id and
  **no secret material**; no entry is ever updated in place, and use is refused if
  the admission append does not durably commit (fail-closed).
- WORM + its hash-chain tamper-evidence are **on by default**; a config that
  disables WORM or chain verification on a **key-holding** kb **fails boot
  validation** (the kb refuses to run unaudited), and a deliberately corrupted
  ledger entry is detected by chain verification and raises a typed integrity alert.
- The kb CA key is no longer a plaintext file; enrollment still issues certs.
- Rotating an org key's value keeps egress working with the new key; the old vendor
  key is **revoked upstream**, and an attempt to roll back to a superseded ciphertext
  version is refused by the external-epoch check. (The design does not claim retained
  old ciphertext becomes cryptographically undecryptable — it claims the old *key* is
  revoked at the vendor and version rollback is prevented.)
- Key pages are `mlock`ed and `MADV_DONTDUMP` (verified); a **forced core dump greps
  clean of org-key bytes** (acceptance, not just testing), and a test exercises the
  **outbound HTTP library's buffer lifecycle** (not just kb's own vault buffer),
  asserting the signer callback cleanses every copy after use.
- A dump of the Postgres primary (or a read replica, or a backup) contains only
  AAD-bound ciphertext — no plaintext org key and no KEK/root; kb↔Postgres refuses a
  non-TLS connection and the runtime role cannot perform DDL or read another schema.
- A freshly-started kb instance with a valid workload identity **auto-unseals**
  against the anchor and serves org egress with no operator step; an instance
  lacking that identity — or anything holding only a Postgres dump — cannot unwrap
  any key. Revoking the workload identity at the anchor blocks further unseal.

## Testing

Unit: envelope round-trip + AAD-mismatch rejection (reuse vault_crypto tests),
seal/unseal state machine, each custody provider (file always; tpm2/pkcs11/kms
behind build flags with a mock anchor), use-in-place returns result-not-secret,
WORM entry shape (no secret), rotation re-wrap + old-ciphertext-fails.
Integration also exercises the **multi-instance races and crash points** the design
rests on: two instances racing the dispatch `admitted→dispatching` CAS (exactly one
dispatches); a crash between rotation CAS and anchor advance (no unavailable/rolled-back
version); seal landing mid-admission (capability re-check refuses); singleton-lease
fencing (a resumed stale instance is fenced); and witness-outbox drain under a sink
outage (fail-closed at the backlog bound). Integration: sealed-kb refuses egress → unseal → egress works; CA-key-in-vault
enrollment end-to-end; core-dump scan asserts no key bytes.

## Non-goals

No mandatory external **HSM** for a **single-instance, single-org (dev/personal)**
box: bare `file` custody stays the default there **but only as keyless dev mode** —
no live org key or CA key may be provisioned or loaded under it (§3); a single-org
box that holds **live** keys uses **TPM**, not a full HSM. This default does **not**
extend to any scaled (multi-instance) or multi-org deployment, which must use a
KMS/HSM anchor and start sealed (§3). Not a general-purpose secrets manager
for arbitrary app secrets — scope is org vendor keys + the kb CA key. No change
to the server's per-user vault (this is the kb tier). No hard in-process isolation
boundary in this packet (use-in-place is scoped-access, not memory isolation, §4);
a separate egress-broker process / HSM-backed signing is a possible future upgrade,
out of scope here.
