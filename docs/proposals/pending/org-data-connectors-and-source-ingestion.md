# Proposal: Org-data connectors — the source-ingestion on-ramp for the every-domain KB

- **State:** PENDING — design only, no code in this PR. Adds the missing
  *front door* to the existing ingest pipeline: a connector layer that pulls
  documents from the systems where an
  organization's non-code knowledge actually lives (issue trackers, chat, email,
  doc stores, wikis) and hands them to the existing Normalize → curator →
  promote pipeline. Builds on **Ingest Lab and Strategy-Aware Chunking** (Accepted,
  Phase 0 shipped — `aimee kb lab`), the **Corpus Staged Processing Pipeline**
  (Done — `corpus_processing_jobs`, `aimee kb pipeline`), **Structured PDF
  Ingestion** (Done), and the credential vault (**cred-vault-consolidation**,
  `git_host_cred.c`, `git_oauth_github.c` — Done). Does **not** re-propose ingest,
  chunking, the stage machine, or extraction — it feeds them.
- **Author:** JBailes
- **Date:** 2026-07-07
- **Charter roles:** Extract (Normalize/Ingest stage — the connector is the source
  half of Normalize), Classify-Score (source-kind + document-kind classification at
  intake), Enforce (per-connector auth, PII, and scope gating on the ingest hot
  path — fail closed), Gate-Promote (staging → release gating per source, reusing
  the curator's release gate), Persist (durable per-source sync cursors and
  provenance). Cites the Architecture Charter; the connector output enters the
  charter's one data pipeline at Normalize.

## Thesis

KNOWLEDGE.md's premise is a *whole-company* knowledge base — §1 promises aimee
ingests "prose, code, API specs, runbooks, design docs, tickets, meeting notes, and
policies" across "engineering, product, sales, support, operations, legal, and
finance," and §4's cross-domain reasoning ("connect a **support** pattern to the
**engineering** root cause, or a **sales** commitment to the **roadmap**") only
works if support tickets, sales notes, and roadmap docs are all *in the graph*.

Today there is **no way to get that data in**. The shipped ingest surface is
file-, PDF-, and code-oriented: Ingest Lab normalizes and chunks *what is handed
to it*, the corpus pipeline drains *documents already seeded* into
`corpus_processing_jobs`, but **nothing pulls from a source system**. A grep of the
tree confirms it: Jira, Confluence, Slack, Gmail, Google Drive, Zendesk, and Notion
appear only in prose, never in code — there is no connector, no incremental sync,
no per-source auth wiring, and no proposal that owns this gap.

The result is a real capability with an empty corpus for every non-file domain. The
mechanism (one graph, hybrid recall, cross-domain synthesis) is built; the on-ramp
is not. This proposal is that on-ramp: a small, uniform **connector contract** plus
a first set of adapters, incremental sync, and ingest-time enforcement — routing
everything into the pipeline that already exists downstream.

## Goal

1. **One connector contract** — a uniform adapter interface (`connector_kind`,
   `list_since(cursor)`, `fetch(ref)`, `to_document()`) so every source looks
   identical to the Normalize stage, and a new source is a small adapter, not a
   pipeline change.
2. **A first adapter set** that covers the domains §1 names: an issue tracker, a
   chat source, a doc/wiki store, and email — chosen for breadth of domain
   coverage, not vendor completeness.
3. **Incremental sync** — durable per-source cursors so re-runs pull only new/
   changed items, with supersession (not duplication) on edited source records.
4. **Ingest-time enforcement** — per-connector auth from the existing vault, a
   source→scope mapping (which shared/`workspace` scope a source lands in), and PII
   / poison gating on the intake path before anything reaches the curator.
5. **Zero new downstream** — connector output enters at Normalize and rides the
   shipped chunking → staged pipeline → curator → promotion path unchanged.

## §0 What already exists (so we don't rebuild it)

- **Normalize + chunking.** Ingest Lab (`aimee kb lab`) — document-kind-aware
  chunking, quality signals, stage recommendations (Phase 0 shipped). This is the
  stage a connector feeds.
- **Staged pipeline.** `corpus_processing_jobs` + `aimee kb pipeline` — the
  resumable per-document stage machine with doc-ingest seeding and deterministic
  drain (Done). Connectors seed it.
- **Structured document intake.** Structured PDF ingestion + evidence layer, corpus
  structural analysis (`document_sections`, `document_references`, staleness) —
  Done. Applies to connector-sourced docs the same way.
- **Credential vault + OAuth.** The server vault (`git_host_cred.c`), GitHub OAuth
  device flow (`git_oauth_github.c`), and cred-vault-consolidation (Done) — the
  auth-storage substrate connectors reuse for per-source tokens; the device-flow
  pattern generalizes to per-connector OAuth.
- **Ingest safety.** The **Ingest poison gate** (Done — Layer-1 deterministic
  pattern gate, five threat categories, obfuscation-aware normalizer, shadow mode)
  and the typed-fact PII sensitivity tiers — the enforcement this proposal wires
  onto the connector intake path.
- **Scope lattice.** `global > workspace > project > user` with the audited public
  access contract (Memory Public Contract, Done) — the destination scopes a
  source→scope mapping targets.

## §1 The connector contract

A connector is a small adapter implementing one interface; the pipeline never knows
which source it came from:

- `connector_kind` — stable id (`jira`, `slack`, `confluence`, `imap`, …).
- `list_since(cursor) → [ref, …]` — enumerate items changed since the durable
  cursor (§3); no full re-scan on re-run.
- `fetch(ref) → raw` — pull one item's content + metadata.
- `to_document(raw) → document` — map to the pipeline's document shape (body,
  source metadata, author, timestamps, stable `source_uri`), so Normalize and every
  downstream stage treat it identically to a file or PDF.

The adapter is intentionally thin: no extraction, no chunking, no dedup — those are
downstream and shipped. This keeps "add a source" cheap and keeps all intelligence
in one place (the curator), per the charter's single-pipeline rule.

## §2 First adapter set (domain breadth, not vendor completeness)

Four adapters, each covering a domain §1 names that has no on-ramp today:

- **Issue tracker** (e.g. Jira/GitHub Issues) — product/engineering tickets and
  their decisions.
- **Chat** (e.g. Slack) — operations/support tacit knowledge; thread-aware
  `to_document` so a thread is one document, not N fragments.
- **Doc / wiki** (e.g. Confluence/Notion) — policies, runbooks, design docs.
- **Email** (IMAP) — sales/legal/finance correspondence; the lowest-common-
  denominator source, provable without a vendor account.

Selection criterion is **domain coverage** so §4's cross-domain examples become
demonstrable end-to-end; additional vendors are later adapters against the same
contract, not new proposals.

## §3 Incremental sync + supersession

- **Durable cursor per source** (`connector_sync_cursors` in DB2) — last-seen
  watermark (timestamp or opaque token) so `list_since` pulls only the delta.
- **Supersession, not duplication** — an edited source record maps to the same
  `source_uri`; re-ingest supersedes the prior document (reusing the corpus
  pipeline's existing supersession/versioning), so recall doesn't accumulate stale
  copies. Aligns with the MinHash-LSH near-dup / supersession machinery already in
  the sketch layer (Done).
- **Resumable** — a sync run is itself a job so an interrupted pull resumes, matching
  the corpus pipeline's resumability discipline.

## §4 Ingest-time enforcement (fail closed)

Every connector item passes three gates *before* the curator sees it:

1. **Auth** — per-connector credentials from the server vault; OAuth via the shipped
   device-flow pattern where the source supports it. No plaintext tokens outside the
   vault.
2. **Source → scope mapping** — an operator declares which scope a source lands in
   (e.g. a support Slack channel → `workspace`; a personal mailbox → `user`).
   Unmapped source ⇒ most-restrictive scope by default, never `global`. This is the
   privacy boundary the multi-tenant KB depends on (cf. the db1/db2 scope-correctness
   rule in `memory-db1-db2-architecture.md`).
3. **PII + poison** — the shipped ingest poison gate runs on connector intake in
   shadow first, then enforcing; PII sensitivity tiers apply so a connector can be
   pinned to withhold personal facts from shared scopes. Unknown/learned relations
   fail closed to the restrictive tier, as the typed-fact layer already does.

## §5 Operator surface

- `aimee kb connect <kind> …` — register a source (auth + source→scope mapping),
  stored like a git-host credential.
- `aimee kb sync <source>` — run/refresh a pull; `--dry-run` previews the delta and
  enforcement decisions through Ingest Lab without seeding the pipeline.
- `aimee kb sources` — list configured sources, cursors, last-sync, item counts.
- A `sync` can be scheduled by the existing routine/cron surface — no new scheduler.

## Acceptance criteria

1. **Contract.** The connector interface is documented; two adapters from §2
   implement it with no pipeline change beyond registration.
2. **End-to-end.** A fixture source (e.g. a local IMAP/maildir and a canned issue
   export) ingests through connector → Normalize → staged pipeline → curator, and
   the items are recallable, with `source_uri`/provenance intact.
3. **Incremental.** A second sync over an unchanged source pulls zero items; an
   edited item supersedes (not duplicates) its prior document.
4. **Enforcement.** An unmapped source lands in the restrictive scope, never
   `global`; the poison gate blocks a planted adversarial item; a PII-tiered fact
   is withheld from a shared-scope recall in a fixture test.
5. **Auth hygiene.** Credentials live only in the vault; a `--dry-run` never writes
   to the pipeline.
6. **Cross-domain demo (validation-pending).** With an issue-tracker source and a
   code corpus both ingested, a §4-style query ("connect this support/issue pattern
   to the code that implements it") returns a citation-backed cross-source answer.
   Real-org quality is a dogfood deliverable and reported as *validation-pending*,
   not done — this proposal ships the on-ramp and fixtures.

## Explicitly out of scope / does not re-propose

- Normalize/chunking, the staged pipeline, extraction, promotion, PII tiers, poison
  gate, and the vault — all shipped; reused verbatim.
- Making curator extraction/reflection actually synthesize over this wider corpus —
  that is the sibling proposal
  `llm-sidecar-productionization-curator-and-reflection.md`. This proposal makes the
  corpus *wide*; that one makes extraction *real*. They compose but ship
  independently, and both are needed before §4's whole-company reasoning is true in
  the build.
- Vendor-exhaustive connector coverage — beyond the first four, each new source is a
  thin adapter against the §1 contract, not a new proposal.
