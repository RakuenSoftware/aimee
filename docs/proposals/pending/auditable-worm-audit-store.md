# Proposal: Per-service auditable, verifiable WORM metrics-and-logs store

## Thesis

aimee's and its agents' actions must be **fully and verifiably** auditable. Today
they are not: the per-action guardrail audit is a flat append file (`audit.log`)
re-parsed in full on every read with a 20k in-memory cap (band-aided in #1092),
metrics/analytics live in **mutable** operational tables (`agent_log`,
`token_audit`, `guardrail_events`), and nothing is cryptographically
tamper-evident. Replace this with a dedicated, append-only (**WORM**),
hash-chained + MAC-checkpointed audit store **per service** — one for aimee-server,
one for aimee-kb — that is the single record of what happened and can be **proven**
intact or shown, at a named row, to have been altered.

## Goal

For **each** service independently:

1. A dedicated WORM store holding the **full record** of governed actions plus
   periodic **metric snapshots**.
2. **Cryptographic tamper-evidence**: a hash-chain over every record + periodic
   MAC-signed checkpoints, so any edit, deletion, reorder, or truncation is
   detectable and *provable* by `verify`.
3. **OS-level immutability**: sealed segments are made kernel-immutable so even a
   privileged local process cannot rewrite history in place (degrading to
   crypto-only where the filesystem can't).
4. Reads become **indexed queries** (server-side pagination / filter / aggregate),
   superseding #1092's full-file reader.

## §0 Deployment model & threat model (the invariant everything rests on)

- **aimee-server** is SQLite-based, single-process, in its own container
  (`AIMEE_HOME=/var/lib/aimee`). Its WORM store is **SQLite segments** on the
  server's own volume, sealed into kernel-immutable files.
- **aimee-kb** is Postgres-based (db2). Its WORM store is a **dedicated Postgres
  role + schema** with `UPDATE`/`DELETE` revoked, sealed by consistent snapshots
  exported to immutable storage.
- The two **do not share** a store (different trust domains, engines, lifecycles).
  The record model, canonicalization, chain, and checkpoint format are **byte-for-
  byte identical** (shared code + test vectors); only the storage/immutability
  substrate differs.
- **Threat model, stated honestly.** The guarantee is **detection**: the hash-chain
  + MAC checkpoints let anyone holding the verify key *prove* that history was
  edited, reordered, or truncated. OS immutability (`chattr +i`) *raises the bar*
  for silent in-place rewrite but is bypassable by root/`CAP_LINUX_IMMUTABLE`, so
  it is hardening, not the guarantee. **Absolute prevention against a fully
  compromised host is out of scope; tamper-evidence is not.** Tamper-evidence holds
  only while the **verify key stays secret** and an **out-of-band trust anchor**
  (§7) survives host compromise — both are explicit design elements, not
  assumptions.

## §0.1 What already exists (reuse, don't rebuild)

- **HMAC-SHA256 primitive + key loading**: `src/audit_action.c` (`hmac_sha256`,
  `audit_load_key`, `AUDIT_KEY_LEN`) — reuse the *primitive*, with a **separate
  chain key** (§2).
- **SHA-256 raw**: `wfe_sha256_raw` for the chain hash.
- **The action seam already fires on every governed call**:
  `guardrails_action_audit.c` → `audit_action_log(...)` (server),
  `db2_audit_event_write(...)` (kb). These become the WORM writer.
- **The paginated reader** (`audit_ledger_read`, `dashboard.audit`) is the
  migration fallback the indexed reader replaces.

## §1 Record model (identical across services)

Append-only table `audit_event`:

| column      | meaning |
|-------------|---------|
| `seq`       | monotonic 1..N, **gap-free** (the ordering + chain index; §2) |
| `ts`        | RFC3339-UTC, writer clock — **advisory only**, NOT in `row_hash`, NOT an ordering authority (§2) |
| `actor`     | **structured** `{role, principal_id}` (R2-10) — `role` ∈ closed set `primary`\|`delegate`\|`operator`\|`system`\|`curator`; `principal_id` is the authenticated identity (SSO sub / deployer JWT / machine id / delegate name), so "operator" isn't collapsed across humans/CI/monitoring. Names are `[a-z0-9._-]+`, NFKC-lowercased at write (R2-11). |
| `action`    | dotted verb from a closed registry — `tool.<name>`, `agent.set`, `delegate.dispatch`, `gate.approve`, `chain.checkpoint`, `kb.query`, `kb.ingest`, `kb.curator.promote`, `metric.snapshot`, … |
| `subject`   | the thing acted on (tool args-hash, agent name, work-item id, doc id); sensitive subjects are **HMAC-hashed** (R2-12), never plain SHA-256 |
| `verdict`   | closed enum: `allow`\|`block`\|`rewrite`\|`approval_required`\|`ok`\|`error`\|`na` (forward-compat is via `version`/`algo`, not a `reserved` slot; R2-22) |
| `detail`    | JSON per a **per-action allowlisted schema** (§6); ≤ **16 KB**; money as integer minor-units, no binary floats (R2-18); oversized/blob fields spilled by hash reference (§6) |
| `key_id`    | id of the chain key in force when the row was written (rotation; §2) |
| `prev_hash` | SHA-256 of the previous row's `row_hash`; for `seq=1` (genesis) it is **exactly 32 zero bytes**, with a genesis test vector (R2-1) |
| `row_hash`  | `SHA256(domain_tag ‖ prev_hash ‖ canonical(record))` |

**Canonicalization (`canonical`) is a spec, not prose (R1-1):** **RFC 8785 (JSON
Canonicalization Scheme)** over the record's `{version, algo, seq, actor, action,
subject, verdict, detail, key_id}` in a fixed field order, UTF-8, RFC3339 `ts`
profile *excluded from the hash*, with an explicit `version`/`algo`/`domain_tag`
so the format is upgradable without breaking old rows. The `detail` schema forbids
non-deterministic fields (server timestamps, random ids) inside the hashed body.
**Shared test vectors** (fixed inputs → expected `row_hash`) are part of the code
and both the SQLite and Postgres writers/verifiers must reproduce them, so a hash
computed on one engine verifies on the other.

**Metric snapshots** are ordinary rows (`action="metric.snapshot"`, `detail =
{tokens, cost_usd, verdict_counts, agent_stats,…}`) appended on a timer (§5) — a
verifiable metrics-over-time history; live/rollup metrics still come from the
operational tables for speed.

## §2 Verifiability — hash-chain + MAC checkpoints (+ keys)

- **Chain + gap-free seq (R1-5 / R2-7).** `seq = head.seq + 1` is allocated
  **inside the same serialized transaction** as the insert — SQLite: a process
  mutex + a single-row `head` table (`PRAGMA synchronous=FULL`); Postgres:
  **`pg_advisory_xact_lock`** (transaction-scoped, auto-released on session death)
  + `head` row update, allocation + insert in one transaction at `SERIALIZABLE`,
  with `NOWAIT`/bounded-retry then fail-closed to avoid deadlock/priority-inversion.
  **DB SERIAL/sequences are forbidden** (they gap on rollback). One writer per store
  total-orders the chain.
- **Checkpoints are first-class chain rows (R2-6).** A checkpoint is itself an
  `audit_event` (`action="chain.checkpoint"`, `detail={head_hash, mac}`) written by
  a **single timer-driven path** (not "N events OR T seconds" racing) so it is
  seq-ordered, unique, and gap-detectable like any other row. `mac =
  HMAC-SHA256(chain_key[key_id], domain_tag ‖ head_hash ‖ seq ‖ key_id)` — binds
  `head_hash`+`seq`+`key_id` (no `ts`; R1-10). Committing to a `seq` makes
  **truncation/rollback detectable**.
- **The uncheckpointed tail is provisional, and verify says so (R2-1, was
  BLOCKING).** Rows after the newest checkpoint are chained but **not yet
  MAC-attested** — an actor with live-file write access (but no chain key) could
  rewrite that tail and still satisfy the hash-chain. Therefore: (a) `verify`
  reports "**trusted through checkpoint C (seq S); K uncheckpointed rows are chained-
  but-unattested**" and exits **non-green (amber)** whenever head is beyond the
  newest trusted checkpoint; (b) the checkpoint cadence bounds K (small T, plus a
  checkpoint forced before each **seal** and on shutdown); (c) the newest checkpoint
  is mirrored to the **signed off-host anchor** (§7) so the tail's exposure window is
  the anchor-update interval, explicitly stated, not silent.
- **Keys (R1-3).** A **dedicated chain key per service**, distinct from the
  approval-signing key; `key_id` in every row and checkpoint; rotation introduces a
  new `key_id` at a checkpoint boundary and the rotation checkpoint is
  **cross-signed** by old+new so pre-rotation history stays verifiable. Key custody:
  a `0600` key file under `AIMEE_HOME` (or a KMS/HSM handle) owned by the service,
  documented, generated from a CSPRNG. **Honest naming:** the checkpoint is a
  **MAC**, not a signature — a verifier who can check it can also forge it; for
  independently-provable checkpoints (auditor/support who must verify but not
  forge), an **asymmetric-signature / HSM public-verify mode** is a documented
  upgrade (a public key verifies; the private signer stays server-side/HSM).
- **`aimee audit verify` (R1-9 / R2-13).** Runs against a **consistent snapshot**
  (SQLite: a deferred read txn; Postgres: `REPEATABLE READ`) so it never races the
  writer; "max trusted checkpoint" is bounded by that snapshot's head. Default: walk
  from the **last known-good checkpoint** (the signed off-host anchor, §7) to head —
  O(recent) — recompute each `row_hash`, assert `prev_hash` linkage + gap-free
  `seq`, verify each checkpoint MAC, and **validate the anchor** (present, signature
  valid, not stale). `--report-all` does the full O(N) forensic pass and reports
  every divergence. Exit codes: **green** (verified to a fresh anchor), **amber**
  (chain intact but head beyond newest trusted checkpoint / anchor stale),
  **red** (a break, with the first/all offending `seq`). Usable as a monitoring
  probe and a release gate.

## §3 WORM enforcement — layered, with honest guarantees

1. **Application:** the store exposes only `append()` + `read`/`verify`. No update
   or delete code path exists anywhere.
2. **Database (accidental-write protection, NOT adversarial WORM; R1-7).**
   - SQLite: `BEFORE UPDATE/DELETE … RAISE(ABORT,'WORM')` triggers on both tables.
     Stated plainly: a process with **write access to the live DB file** can drop
     these triggers, so DB-level WORM stops bugs/casual edits, **not** an adversary
     — the adversarial guarantee is the crypto chain (§2) + OS immutability (below).
   - Postgres: a writer role granted `INSERT, SELECT` only, `REVOKE UPDATE, DELETE`,
     with row-level `BEFORE UPDATE/DELETE` triggers as defense-in-depth; DDL owned
     by a separate admin role the app never uses.
3. **OS immutability (R1-7 / R1-12).**
   - **aimee-server (SQLite), precise layout:** the **live tail is a normal mutable
     SQLite DB** (SQLite rewrites pages + WAL, so `chattr +i/+a` on the *live* DB is
     invalid). On rotation (size/time) the tail is checkpointed (WAL truncated),
     `VACUUM INTO` copies it to a **sealed segment** `audit-<lo>-<hi>.db`, a final
     sealing checkpoint is written whose `prev_hash` links the next segment's
     genesis, the segment **and its directory** are fsync'd, then `chattr +i`.
     **`chattr +i` success is a precondition** for advancing the tail (the next
     `seq` reservation blocks on seal-complete); a chattr failure is a **hard error
     → roll the segment back to mutable + alert**, never a silent half-seal. On a
     filesystem without immutable-flag support (checked at startup: NFS/tmpfs/non-
     Linux), the store **degrades to crypto-only** with a loud, audited warning.
   - **aimee-kb (Postgres), one pinned mechanism:** at each checkpoint, a
     **consistent logical snapshot** (`COPY`/`pg_dump` of the sealed `seq` range,
     not raw WAL) of the sealed segment is exported to **object-lock / `+i`
     storage**; the live tail relies on the advisory-lock chain + triggers, with WAL
     archived best-effort to *separate* append-only storage. (Raw-WAL-only is
     rejected: WAL records physical mutation, not a consistent audit snapshot.)
   - Verification spans sealed (immutable) segments + the live tail; the chain
     stitches across boundaries via each seal's `prev_hash`.

## §4 Capture — completeness is enforced, not conventional (R1-4)

- One writer API: `audit_event(actor, action, subject, verdict, detail)`. Capture
  at the **narrowest governed-state mutation boundary**, not scattered UI/API sites.
- A published **inventory of governed transitions** (tool actions, agent/config
  mutations, delegate dispatch, gate/approval, autonomous-run lifecycle, vault
  credential ops; kb: query/ingest/curation/governance) is the checklist for
  completeness.
- A **CI/lint guard** forbids direct writes to the audited domains outside the seam
  (a denylist of mutation entry points that must route through `audit_event`).
- **Tests mutate every audited domain object and assert a corresponding WORM row**,
  so a new bypassing call site fails CI rather than silently escaping the record.

## §5 Reads — Logs page + Guardrail pane query the store

`dashboard.audit` (and a kb equivalent) query `audit_event` directly:
`… WHERE (filters) ORDER BY seq DESC LIMIT ? OFFSET ?` (keyset by `seq` for deep
pages) — real server-side pagination + **server-side filtering** (verdict/actor/
tool, today client-side) — and the Guardrail mix becomes `GROUP BY verdict`.
Indexes on `(seq)`, `(verdict)`, `(actor)`, `(action)`, `(ts)`. This retires the
#1092 file reader to a migration fallback only.

## §6 Data classification & redaction (immutable ⇒ a privacy constraint; R1-8)

Because rows are immutable **forever**, `detail` is not a free-form blob:

- **Per-action allowlisted schemas** — each `action` declares the exact fields it
  may record; anything else is dropped before write.
- **Hard 16 KB cap**; oversized or sensitive-by-type fields are **content-addressed**
  — `detail` stores `sha256:<hex>` and the payload goes to a separate,
  *non-WORM*, deletable blob store. This keeps the chain intact while allowing
  **crypto-shred deletion** (drop the blob; the hash reference and chain survive) to
  satisfy privacy/legal deletion requests.
- **Secret scanning + subject hashing** — credentials/tokens/PII are redacted or
  hashed at the seam; the audited `subject` for sensitive inputs is a hash, not the
  raw value.

## §7 Trust anchor, retention & migration

- **Out-of-band anchor (R1-9).** The **newest trusted checkpoint** (a
  `{seq, head_hash, key_id, mac}` tag) is stored **outside the WORM volume** (a
  deploy-time/operator-held anchor). `verify` compares head against it, so a
  restore-from-backup, rollback, or fork is detectable even though external
  transparency-log anchoring is a non-goal for v1.
- **Retention (R1-11).** Default **90d online, 7y offline**; at seal time a segment
  is moved/copied to **object storage with object-lock** (or `+i` media); `verify`
  spans the archive; the schedule (post-seal move vs. nightly batch), archive
  target, and who runs it are configured, not left implicit.
- **Migration — authoritative WORM, fail-closed (R1-6).** The **WORM store is
  authoritative**. A one-shot **legacy→genesis import** builds a synthetic chain
  over existing `audit.log` so history is continuous, then new writes go WORM-first
  with the file write best-effort/derived during the window, guarded by an
  **idempotency key** and a **parity check** before default-on. If the authoritative
  sink cannot accept a write, the governed action **fails closed** (§Durability).
  The Logs reader reads WORM, falling back to `audit_ledger_read` only until the
  legacy files age out.

## Durability (R1-2 / R2-3)

An audited action does **not** return success until its `audit_event` row is
**fsync-durable** in the store. There is **no async batch writer and no
group-commit** in v1 (both introduce a window where a crash loses a "successful"
action's row or reorders it, breaking §1/§2). On writer failure or a would-block,
the action **fails closed** (rejected) rather than proceeding unaudited. Durability
is pinned at the engine: **SQLite `PRAGMA synchronous=FULL` + WAL + fsync of the DB
file and its directory**; **Postgres `synchronous_commit=on`** (and, if a replica
is ever added, `remote_apply` or an explicit justification). Cost is mitigated by
one SHA-256 per row and the bounded `detail`; the **latency budget and lock
contention are benchmarked, not assumed**. A bounded group-commit is a *possible
later optimization* only if benchmarked and only with the invariant that every
member of a commit group is fsync-durable and seq-ordered before any of them
returns success — out of scope for v1.

## Phasing (each independently shippable; behavioural steps default-off)

- **S0** — record model + canonicalization spec + shared test vectors +
  hash-chain single-writer (transaction-scoped gap-free `seq`) + SQLite store +
  WORM triggers + **synchronous fail-closed writer**, behind `audit.worm.enabled`
  (default-off), dual-writing the file. **A minimal kb chain + triggers lands in
  parallel** so the two engines share the record/canonicalization code from day one
  (R1-13).
- **S1** — dedicated chain key + MAC checkpoints + rotation + `aimee audit verify`
  (incremental + `--report-all`) + out-of-band anchor + a `doctor` health check.
- **S2** — segment rotation + sealing + OS immutability (chattr precondition,
  FS-support check + crypto-only degrade) + cross-segment chain stitching.
- **S3** — full capture (inventory + lint guard + per-domain tests); `dashboard.audit`
  reads/filters/paginates from the store; retire the #1092 reader to fallback.
- **S4** — data-classification schemas + redaction/content-addressed spill +
  metric snapshots on a timer.
- **S5** — aimee-kb WORM store completed: Postgres role/schema, revoke UPDATE/DELETE,
  triggers, chain, checkpoints, `verify`, snapshot-to-object-lock sealing.
- **S6** — kb capture (query/ingest/curation/governance) + kb Logs/metrics surface.
- **Close-out** — full kb WORM is a **release gate** for advertising "verifiable";
  flip defaults on after live verify; retention archive wired; file to done/.

## Non-goals

- A shared/cross-service store (explicitly per-service).
- Absolute prevention against a fully-compromised host (we guarantee **detection**).
- Replacing the operational tables that back live dashboards (kept for speed).
- External transparency-log anchoring and forward-secure HMAC ratcheting (future
  upgrades noted in §2/§7).

## Risks / honest limits

- **Hot-path cost**: synchronous fsync per audited action; benchmarked, with
  group-commit as the fallback shape (never async/best-effort).
- **Key/anchor custody is the crux**: tamper-evidence holds only while the chain key
  is secret and the out-of-band anchor survives host compromise — both are explicit
  (§2/§7), with HSM/asymmetric as the stronger upgrade.
- **OS immutability needs privilege + supporting FS**: documented capability model;
  degrades to crypto-only with a loud warning where unavailable.
- **Two engines**: shared record/canonicalization/chain code; engine-specific
  storage/immutability adapters.
- **Retention vs. WORM growth**: archive+seal off the hot volume; verification spans
  the archive; crypto-shred handles deletion without breaking the chain.

## Tests

- **Verify** detects each tamper class on a seeded store: row edit, delete, reorder,
  tail truncation, forged/missing checkpoint, key mismatch, rollback-vs-anchor.
- **Shared canonicalization test vectors** produce identical `row_hash` on SQLite
  and Postgres.
- WORM triggers/grants reject `UPDATE`/`DELETE`; a dropped-trigger case is *detected
  by verify* (proving DB-WORM is not the guarantee).
- Cross-segment chain stitches through a seal; sealed segment is `+i` and
  unmodifiable; FS-without-immutability degrades to crypto-only with the warning.
- **Capture completeness**: mutating every audited domain object produces a WORM row;
  the lint guard fails a bypassing write.
- Durability: kill-9 mid-write leaves the chain consistent (no torn/lost committed
  row); fail-closed rejects an action when the sink is unavailable.
- Migration: legacy→genesis import parity; fallback reader still renders old
  `audit.log`.
- Redaction: an oversized/secret-bearing `detail` is capped/spilled/redacted;
  crypto-shredding a blob leaves `verify` still passing.
- Hot-path throughput benchmark (append+fsync+hash) vs. the current file append.

## R2 — additional design decisions (roundtabled; precise specs)

Body sections above already carry the design-level R2 fixes (durability drops
group-commit; checkpoints are first-class rows; the uncheckpointed tail is
provisional and `verify` reports amber; structured `actor`; genesis = 32 zero
bytes; consistent-snapshot verify). The remaining rulings, pinned here for the
implementation slices:

- **R2-2 Postgres WORM covers TRUNCATE/MERGE + grant hygiene.** Add a `BEFORE
  TRUNCATE ON audit_event` trigger that raises; refuse `MERGE` in code/trigger;
  audit the grant set so **no superuser/`BYPASSRLS`** grantee can touch the schema;
  DDL owned by a separate admin role the app never assumes.
- **R2-4 Genesis is "chained, not origin-attested."** The legacy→genesis import
  proves only internal consistency of pre-existing `audit.log` (which had no
  write-time evidence). Record a **notarized hash of `audit.log` at import time**,
  bind it into the genesis checkpoint + the off-host anchor, and label genesis rows
  as origin-unattested in `verify` output.
- **R2-5 Sealing runs in a SIDECAR, not the audited service.** A separate
  process/host-cron holds `CAP_LINUX_IMMUTABLE`, has **no write access to the live
  tail**, and exposes a narrow "seal `<path>`" API. The audited container **never
  holds the capability** (so a compromise of the service can't unseal).
- **R2-8 The off-host anchor is a signed artifact with a custody protocol.** The
  newest-trusted-checkpoint anchor is `sign(ed25519, {seq, head_hash, key_id, mac})`
  with the signing key held **off-host / in a separate administrative domain**
  (write-only API or a second host — a compromised audited host cannot silently
  update it). Update cadence = per-checkpoint (or per-N with the gap stated);
  `verify` validates the signature + freshness and an auditor can verify offline.
- **R2-9 Fail-closed needs a call-site audit + lint.** Making the writer synchronous
  changes existing best-effort call sites; **inventory every audit call site for
  swallowed errors** and add a **lint rule forbidding catching audit-write errors**
  outside the seam's own retry/fail-closed policy, before flipping durability on.
- **R2-14 The single-writer invariant is enforced, not assumed.** A startup **lock
  file** under `AIMEE_HOME`; a second instance opening the store is **rejected**
  (documented behavior). An HA/failover topology must not silently run two writers.
- **R2-16 Precise SQLite seal state machine + quiesce.** Pause new writes on the
  writer mutex; force a `chain.checkpoint`; `PRAGMA wal_checkpoint(TRUNCATE)`;
  `VACUUM INTO` the sealed segment; fsync segment file **and directory**; `chattr
  +i` (precondition); link the next segment's genesis `prev_hash` to the seal;
  resume writes. Sealing requires **exclusive access to the live tail**; tested with
  concurrent writers.
- **R2-17 Chain-key lifecycle.** Old `key_id`s are **retained for the full audit
  retention** (or bridged into the asymmetric public-verify format before
  retirement); backup/escrow via KMS/HSM versioning; a documented compromise
  response (what `verify` can still trust after a key leak — the pre-leak,
  anchored prefix).
- **R2-20 Blob integrity for content-addressed spill.** The blob's `sha256` lives in
  the (immutable) WORM row; `verify`/read checks the blob against it; the blob store
  is deletable (crypto-shred) but its retention is **aligned to** WORM retention
  (deletion only via the documented privacy/legal path, which leaves the chain
  intact).
- **R2-21 Key-rotation cross-sign format.** The rotation checkpoint carries
  `mac_old` + `mac_new` over the same payload with `key_id_old`/`key_id_new`; test
  vectors cover the boundary so rotation never breaks verifiability.
- **R2-19 Keyset pagination is the primary read API.** `WHERE seq < ? ORDER BY seq
  DESC LIMIT ?` (not `OFFSET` for deep pages); composite indexes `(verdict, seq)`,
  `(actor_role, seq)`, `(action, seq)` for filtered views; index write-amplification
  is benchmarked and index maintenance is defined across segment rotation.
- **R2-23 Canonicalization vectors from an independent oracle.** Source the JCS test
  vectors from a separate RFC 8785 reference (not our own writer), plus a
  property-based test that mutates JSON inputs and re-canonicalizes to catch
  non-determinism.
- **R2-24 `metric.snapshot` is pinned.** `subject = "tenant:<id>"|"global"`; a
  **single in-process timer** (no external scheduler) with jitter bounds; a
  per-snapshot-kind `detail` schema (units, scope, currency-as-minor-units); a
  sizing budget (`metric.snapshot` ≤ ~5% of seq allocation) that feeds the seal
  cadence.
- **R2 (CAS)** Content-addressed spill uses a **dedicated deletable CAS** with its
  own retention (not the WORM volume).

## Revision history

- **R1** (roundtabled, 63 items / 6 panelists): synchronous fail-closed durability,
  dedicated chain key + honest MAC naming, RFC 8785 canonicalization + shared test
  vectors, transaction-scoped gap-free `seq`, authoritative-WORM dual-write with
  legacy→genesis import, precise SQLite live-tail→sealed-segment layout with
  chattr-precondition + FS-degrade, DB-triggers-not-adversarial honesty,
  data-classification/redaction section, retention section, out-of-band anchor,
  incremental anchored verify, kb WORM as a release gate.
- **R2** (roundtabled, 39 items / 4 panelists; 1 blocking resolved): uncheckpointed-
  tail semantics (`verify` amber) + checkpoints as first-class rows; Postgres
  TRUNCATE/MERGE/grant hardening + `pg_advisory_xact_lock`; sealer sidecar; signed
  off-host anchor with a custody protocol; structured `actor{role,principal_id}`;
  genesis = 32 zero bytes + origin-unattested labeling + notarized legacy hash;
  HMAC subject hashing; enforced single-writer lock; precise seal state machine +
  quiesce; engine durability PRAGMAs; key lifecycle + cross-sign format; blob
  integrity; keyset pagination + composite indexes; numeric determinism; independent
  canonicalization vectors; `metric.snapshot` spec; dedicated deletable CAS.
- **R3** (security + engineer persona reviews, used as the panel substitute because
  the fan-out panel truncated on the grown proposal — the bug fixed in aimee PR
  #1094): both reviewed the full document and found **no new blocking issues**; the
  security persona's verdict was repeatedly "sound design for detection" / "very
  high quality," and the only "blocking"-tagged mentions were re-confirmations that
  the R2-1 uncheckpointed-tail resolution is sound. Design considered converged and
  ready for implementation sign-off; a confirmatory full-panel run is available once
  the roundtable fix (#1094) is deployed to the panel server.
