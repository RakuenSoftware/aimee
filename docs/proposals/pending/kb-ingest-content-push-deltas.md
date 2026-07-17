# KB ingest: content-push, default-tree indexing, and monotonic delta ordering

- **State:** PENDING — roundtable-reviewed (round 1: BLOCK → rulings folded in below).
- **Author:** JBailes
- **Date:** 2026-07-17

## Problem (root cause)

aimee-kb is **stateless and horizontally scalable** — a fresh instance serves any
tenant from Postgres with no instance-local authoritative state. But repository
ingest assumes aimee-kb shares a filesystem with the caller: `kb.build`/`kb.update`
POST a filesystem **path** to aimee-kb and expect it to read that path. In a split
deployment (aimee-server + a separate aimee-kb whose mounts exclude the caller's
per-user workspace) aimee-kb **cannot read the path and silently ingests nothing**
— observed live on .254 (`files=0`). The correct pattern already exists partially
(`index.scan`/`index.ingest` push code content; `kb.docs.push` pushes document
bytes) but was under-built (the `/v1/kb/docs/push` route was even missing).

## Invariant

**aimee-kb never reads a caller's filesystem.** Every ingest is content-push: the
workspace-owning tier (aimee-server, per end-user checkout) resolves *what* to send
and streams **bytes + metadata** to aimee-kb, keyed by **(project-identity,
item-path, item-version)**. Holds for one box or twenty stateless kb instances.

## Requirements (operator)

1. Index the **default tree** of any repo, by any end user (a first-class per-user,
   per-repo operation, not a one-off path scan).
2. **Project identity + dedup:** memories/index/embeddings key on a stable project
   identity; the **same repo is ingested once** (two users/checkouts → one project).
3. **Delta ingestion:** the default tree always changes; ingest deltas.
4. **Incremental only:** never re-ingest an entire file/repo for a small change.
5. **Monotonic delta ordering (multi-writer):** never apply a delta older than what
   is applied for an item (user A applies yesterday's delta; user B later pushes an
   *older* delta for the same repo → must not regress). Ordering by an authoritative
   version, never a client wall-clock.
6. **(Near future) Uploaded-document ingest** through the same content-push seam.

## Rulings (roundtable round 1 — these were the OPEN decisions)

### R1 — Project identity (dedup key)
Three-tier, in priority order: **(a) operator-assigned alias** (config-store);
**(b) normalized canonical remote URL** when exactly one remote exists; **(c)
local-only repos:** identity = SHA-256 of `(worktree-absolute-path, root-tree-SHA)`,
explicitly **non-dedupable across machines** (two laptops with the same local-only
repo are distinct projects until an operator alias merges them). **Forks are distinct
projects** (different remote URL). A **monorepo subtree** is its own project, identity
= parent-identity + subtree path, operator-aliasable. Memories key on this identity so
project-specific memories follow the project, not a checkout path. (Root-commit SHA is
a *version* field, never identity.)

### R2 — Version / ordering (monotonic, force-push safe)
Ordering key = a **per-project generation derived strictly from commit ancestry**:
commit A is newer than B **iff A is a descendant of B** in the repo ref graph. Reject
any delta whose base is **not a strict descendant** of the stored generation. **Commit
wall-clock time is forbidden as an ordering input** (label only). aimee-kb persists a distinct
per-project **`generation`** row (the ancestry head last applied), separate from the
per-item versions of R3. **Force-push/rebase is an explicit operator action** that
advances `generation` to the forced tip AND **resets the per-item watermarks of every
affected item to the forced version, reindexing them** — a stale per-item row never
survives a force-push.

### R3 — Watermark granularity
Watermark is **per-item `(project_id, path)` — required** for item-level monotonicity
(composite PK). A **per-project high-water-mark is an optional fast-path only** (reject
a delta whose base version < project HWM without a per-item lookup, since it cannot move
any item forward). Per-item rows are the source of truth; the per-project `generation`
(R2) and HWM are the fast-path, not the guarantee. A per-item version is only accepted
if its base is a descendant of the item's stored version (R2), evaluated against the
project `generation`.

### R4 — "What changed" is a persisted manifest, not a git diff
aimee-kb persists a per-project **manifest**: `path → {version, content_hash,
chunk_hashes[]}`. A **delta is the set-diff between the new tree's manifest and the
stored manifest by content_hash.** Git `name-status` is at most a **hint** to the
caller about which paths to read — never the source of truth for what changed. This
makes S5 (uploaded blobs) trivial: an uploaded doc is a one-item manifest, no git ref —
but it **still resolves an R1 project identity** (an explicit target project, or a
per-tenant `uploads` default project); no content is stored without a project identity.

### R5 — Deletes + idempotent apply
Delete writes a **tombstone `(project, path)`** carrying the current version watermark.
Apply rules: (a) a delete at version V is rejected if stored version > V; (b) an upsert
at V is rejected if a tombstone at version ≥ V exists (unless the upsert carries a
higher version); (c) upsert **replaces** (never appends) all chunks for `(project,
path)` — chunk_hashes are scoped to `(project, path)`, so fork-distinct projects (R1)
never share or resurrect each other's chunks so a re-add cannot resurrect stale embeddings. **Apply is a chunk-hash diff**
against stored `chunk_hashes` — a retry of the same version re-embeds **zero** chunks
(at-least-once delivery is free at the embedding layer). Tombstone retention: keep until
project GC.

### R6 — Push authorization (tenant isolation)
Push endpoints are **authenticated by the caller's tenant token** (not network
position). The resolved project-identity **must be authorized for that tenant** before
bytes are accepted; pushing a project the caller does not own returns **403** (not 404)
and is logged. **The same tenant-scoping gates reads** (search/list/get): a tenant
never sees another tenant's project content. (Rate-limiting / DoS is an infra concern,
out of scope here.) Required in S2 acceptance.

## Slices (reordered per R-ruling; each: pure core + tests, roundtable before PR)

1. **DONE — `/v1/kb/docs/push` route** (content-push doc ingest reachable).
   *Refs:* aimee PR #1484 (`fix(kb): wire the missing /v1/kb/docs/push route`).
2. **S3 first — Project identity + dedup + migration.** Identity resolver (R1),
   alias table, project rows keyed on identity, memories re-keyed to identity. Schema
   migration: **dual-key legacy path-forwarded rows under old + new identity for a
   deprecation window (reads union both), then GC the legacy keys at window end** — this
   is the chosen path (an operator one-shot re-embed is only a fallback for corrupt legacy
   rows). Proven **without a full re-embed** via hash-stable chunk reuse. Rollback = drop
   the new-identity keys, legacy keys still resolve. No new ingest in this slice.
3. **S2 — Index-default-tree as a content-push op** (against the *locked* S3 identity).
   aimee-server resolves the repo default branch, builds the manifest (R4), pushes
   bytes. **Tenant-scoped project authorization (R6).** **Retire `kb.build`'s path
   parameter for the doc/tree corpus → 410 Gone** (asserted in the PR diff) so the
   split-deployment bug can't be reintroduced.
4. **S4 — Delta + per-item watermark + manifest.** Per-item watermark (R3), ancestry
   generation (R2), manifest set-diff (R4), tombstones + chunk-hash apply (R5).
5. **S5 — Uploaded-document source** (same seam, blob = one-item manifest).

## Acceptance

- No ingest passes a caller filesystem path to aimee-kb. Proven in one PR: a stateless
  aimee-kb with **no workspace mount** indexes a repo end-to-end **from pushed bytes**
  (content-push populates files/chunks), AND `kb.build`'s doc/tree **path param returns
  410** (route-table diff asserted) — so the original `files=0` split-deployment bug
  cannot be reintroduced.
- Indexing the same repo from two user checkouts → **one** project; forks → two;
  project-scoped memories resolve to the project identity.
- A default-tree change re-embeds only changed chunks (touch one file → only its changed
  chunks re-embed; unchanged chunks untouched; a same-version retry re-embeds zero).
- Out-of-order two-writer test: apply newer then older delta for the same item → older is
  a **no-op** (per-item watermark rejects it). Force-push advances generation + reindexes.
- Delete then stale-replay → no resurrection; re-add at higher version → clean replace.
- Push of an unauthorized project → 403.

## Risks / open

- Ancestry generation needs the commit graph available to the caller (aimee-server has
  the checkout — fine); cost is a bounded ancestry check per delta.
- Migration dual-key window duration + GC of legacy keys.
- Manifest storage size per large monorepo (path × chunk_hashes) — bounded, indexed by
  `(project_id, path)`.
