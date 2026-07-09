# Proposal: Memory auto-population — feedback→rules, promotion, gated extraction (Proposal 2 Phase 4)

- **State:** draft (pending — not started). Follow-up to Proposal 2, the
  [db1/db2 memory architecture](done/memory-db1-db2-architecture.md); implements its deferred §4
  Phase-4 auto-population, split by risk per that proposal's R4 roundtable.

## Thesis

[Proposal 2](done/memory-db1-db2-architecture.md) shipped the memory *substrate*: db1 = user
memory, db2 = org memory, recall merges both, and **explicit** capture (`aimee memory
identity`/`prefer` → db1, `aimee rules +` → db2) populates it by scope. What it deferred (OQ5) is
**automatic** population — turning the signal aimee already collects (feedback, recurring episodes,
transcript evidence) into durable, correctly-scoped memory without a human hand-typing every entry.

Auto-population is where memory earns its keep, but it is also where it is most dangerous: an
auto-written identity/rule becomes **high-primacy brief text** for the primary agent, and — for db2
(org) rows — for *every* user. So the design is gated, default-off, and cleanly split by blast
radius. This proposal makes that split concrete.

## Goal

1. **§4a Extraction → quarantine** (safe to ship default-off): a gated pass proposes user
   preferences/identity (→ db1) and org conventions (→ db2) into a **quarantine** queue that never
   reaches a brief until an operator approves it.
2. **§4b Promotion → durable org rules** (separate, strict operator gate): consolidate recurring
   high-confidence db2 signal into durable org `rules`, behind its own gate + audit — never enabled
   by the §4a switch.
3. **Feedback → durable org rules**: route the same feedback signal `kb_client_rules_generate`
   consumes into persistent db2 `rules` with lifecycle/decay, instead of an ephemeral brief block.
4. A single **quarantine review surface** (`aimee memory review`) with `approve`/`reject`/`edit`
   verbs and a TTL, so nothing is stored-forever-unreviewed and nothing is surfaced unreviewed.

## §0 Invariants inherited from Proposal 2 (non-negotiable)

- **The automated pipeline can NEVER mint a hard rule — enforced *structurally*, not just in code
  (R1/R2).** Every §4 write is hard-capped at `soft`, AND a DB trigger/CHECK rejects any
  auto/quarantine-provenance row with `directive_type='hard'` — **in both db2 (org rules) AND db1
  (user memory)**, so no auto-populated user identity/preference can be minted `hard` either (R2).
  Provenance is stamped by a single trusted, code-controlled quarantine-release sink (ideally a
  dedicated SQL INSERT target / role for auto-writes), so an injected code path cannot forge an
  operator provenance to reach the non-overridable slot (R2). Only operator-entered rules may be
  `hard`.
- **Precedence lattice** (Proposal 2 R2), highest authority first: hard org rules → operator user
  captures → soft org defaults → **untrusted advisory** (auto-extracted / quarantine-released rows,
  repo-file conventions). Auto-population only ever writes into the *lowest* tier.
- **Scope is a privacy boundary** (Proposal 2 §0): user data must never land in db2 (shared). When
  extraction is unsure of scope, it defaults to db1 (private) and/or quarantine — never db2.
- **Gate-failure semantics**: a PII / low-confidence hit is **quarantined** (stored, not surfaced)
  pending review — never silently redacted or silently surfaced.

## §1 What already exists (build on, don't rebuild)

- **db2 `rules`** (`src/db2/schema.sql`): `polarity`, `weight`, `domain`, `directive_type`
  (default `soft`), **`expires_at`**, **`last_reinforced_at`** — decay + reinforcement columns are
  already present. Feedback→rules and promotion reuse them.
- **Feedback→brief generator**: `kb_client_rules_generate` already synthesizes rule-like guidance
  from the feedback signal for the *ephemeral* brief block; §4 adds a durable-rules sink for the
  same signal.
- **Gates**: `src/memory_pii_gate.c` (per-attribute PII gating) and `src/memory_fact_gate.c`
  (typed-fact write validation) exist for the KB typed-facts path; §4a extraction reuses the PII
  gate and the confidence discipline.
- **db1 user store** (`src/db1/user_memory.c`) + **explicit capture** (server op
  `memory.user_capture`): extraction's db1 sink is the *same* upsert path, so a quarantine-release
  is one call.
- **Review-queue precedent**: `collab_rules` (proposed/decided status workflow) and the existing
  `aimee memory review` (charter-pipeline artifacts) are working models for a review queue.

## §2 Design

### §2a Extraction → quarantine (default-off)

A gated, offline LLM pass (never on the hot turn loop) reads recent transcripts/episodes and
proposes candidate memories, each tagged with a target store + scope. Candidates are written to a
**quarantine table** (NOT to the `memories`/`rules`/`user_memories` recall selectors), carrying:
`{proposed_store (db1|db2), kind, key, content, scope, confidence, evidence_ref, provenance,
sensitivity, content_hash, ttl_at, state (pending|approved|rejected)}`. `content_hash` carries a
UNIQUE constraint over pending rows so the extractor cannot flood the queue with the same signal
(R2 dedup); `sensitivity` drives PII handling (§2b).

- **Nothing in quarantine is recallable** — the recall selectors (db1 `db1_user_memory_list_recall`,
  db2 `db2_memory_list_recall_section`) never read the quarantine table. It surfaces ONLY via the
  operator review command.
- **Gates before quarantine**: a candidate must clear the PII gate and a confidence floor; a PII or
  low-confidence hit is quarantined-for-review or dropped, never auto-approved.
- **Scope default**: unsure → db1/private (Proposal 2 §1 "over-sharing is the harm") — **except a
  candidate whose subject/speaker is ambiguous** (e.g. a multi-speaker transcript) is NOT
  auto-attributed to db1; it is quarantined `scope=unknown` and the operator assigns scope on
  approval (R1: a multi-speaker candidate silently landing in one user's db1 is a mis-attribution).
- **Hard cap**: any org candidate is `directive_type='soft'` (§0). No hard-rule path exists.
- **TTL**: an unreviewed candidate expires at `ttl_at` (default e.g. 30d) — "stored forever, never
  reviewed" is impossible; expiry is logged, not silently dropped.
- **Config**: `intelligence.autopopulate.extract.enabled` (default 0). Off ⇒ no extraction runs.

### §2b Quarantine review surface

`aimee memory review` gains the auto-extract queue with explicit verbs. Every mutating verb requires
a named capability — **`CAP_MEMORY_ADMIN`** (a new server auth cap, distinct from `CAP_MEMORY_WRITE`)
— so gating is structural, not policy-by-convention (R2):
- `aimee memory review list [--store db1|db2] [--limit N]` — pending candidates with provenance.
  For a `sensitivity=pii` candidate the list renders a **redacted preview**; the raw content is
  access-gated behind a separate `--reveal` that re-checks `CAP_MEMORY_ADMIN` and audits the reveal
  (R2 — quarantine is a sensitive store, never a plain-text PII dump).
- `aimee memory review approve <id> [--edit "..."] [--scope user|org]` — release into the target
  store at the correct scope (db1 via `memory.user_capture`; db2 as a `soft` rule/fact) tagged
  `provenance=auto-extracted` so the merge places it at the **untrusted-advisory** lattice tier. A
  `scope=unknown` candidate (multi-speaker, §2a) CANNOT be approved without an explicit `--scope`
  (enforced by a DB CHECK: no release with unresolved scope) — never a silent db1 mis-attribution
  (R2).
- `aimee memory review reject <id> [--reason S]` — tombstone; feeds a negative signal so the same
  candidate is not re-proposed.
- `aimee memory review edit <id> "..."` — correct before approving.
- Also surfaced in the dashboard/console review panel (reuse the existing review surface).

### §2c Feedback → durable org rules

Route the feedback signal `kb_client_rules_generate` consumes toward **persistent** db2 `rules` —
but **through the same quarantine gate as extraction (R1: no bypass)**. A feedback-derived rule is a
db2-targeted candidate; it is written to quarantine and reaches the `rules` table only on operator
approval. It never auto-writes a durable, every-user-visible rule.
- On approval, the rule lands as `directive_type='soft'`, `domain` from the feedback topic, `weight`
  from evidence strength.
- **Lifecycle**: set `expires_at` (decay) and bump `last_reinforced_at` **only on genuinely new
  evidence** — a distinct session/source, never the rule's own prior emission. Enforced
  structurally (R2): each reinforcement carries a `source_evidence_ref`, and a reinforcement is
  rejected when that ref is a descendant of the rule's own prior emissions (an evidence-lineage
  check), so a rule can never reinforce itself and defeat decay. A rule with no *new* evidence still
  decays and expires.
- A revocation path (`aimee rules -` / review reject) tombstones a bad auto-rule.
- **Config**: `intelligence.autopopulate.feedback_rules.enabled` (default 0). The existing ephemeral
  brief block is unchanged until durable rules are calibrated — additive, not a replacement (OQ3).

### §2d Promotion → durable org facts/rules (strict gate)

Consolidate recurring, high-confidence db2 `episode` rows into durable org facts/rules:
- Runs only when `intelligence.autopopulate.promote.enabled` (default 0) AND each promotion is
  approved **per row** (never per batch — R2), because it mutates shared multi-user state.
- Each promotion writes a **WORM audit** row (reuse `audit_worm_*` / `kb_audit_event`) recording
  source episodes, resulting row, and approver.
- **KB-ontology reconciliation (R2)**: before promoting, check the candidate against the KB's own
  typed-fact ontology reconciliation so the two systems don't diverge (a promoted rule that
  conflicts with an existing typed fact is flagged, not silently written).
- Hard-capped at `soft`; the promoted row enters recall at the advisory tier until an operator
  elevates it via explicit capture.

## §3 Phasing (each independently shippable; all default-off)

1. **§2b review surface + quarantine table + schema** — the *sink and the gate UI* first, so nothing
   can be produced without a place to review it. `aimee memory review list/approve/reject/edit`
   against a hand-seeded candidate. **Low blast radius, not "zero risk" (R1):** `approve` is a live
   write into recall, so the review command itself is operator-gated + audited, and the quarantine
   table is structurally outside the recall selectors (a guard test asserts recall never reads it).
2. **§2a extraction producer (default-off)** — the gated offline pass that fills quarantine, behind
   `extract.enabled=0`; calibratable.
3. **§2c feedback→durable rules (default-off)** — the durable sink for the feedback signal, with
   decay/reinforcement lifecycle.
4. **§2d promotion (default-off, strict gate)** — last, because it mutates shared org state; gated
   **per-row** with WORM audit.

## Non-goals

- Turning any of this on by default. Every stage ships `enabled=0`; calibration + operator enable is
  a separate decision.
- A new rollout/bandit system — reuse the shipped calibration machinery for confidence thresholds.
- Re-scoping db1 within one instance, or changing recall retrieval/ranking (separate tracks).
- Minting hard rules automatically (structurally impossible by §0).

## Risks / honest limits

- **Prompt-injection into high-primacy text**: transcript-derived candidates are untrusted; the
  quarantine + operator-approval + advisory-tier placement is the mitigation. Never auto-surface.
- **Cross-user leak via db2**: only org-scoped candidates go to db2, only after review; unsure ⇒
  db1/quarantine. The PII gate runs before quarantine.
- **Reviewer fatigue / silent backlog**: the TTL + expiry logging prevents a stored-forever queue;
  the review surface shows counts so the operator sees backlog growth.
- **Calibration unknowns (OQ5)**: extraction precision is unproven; shipping default-off with the
  quarantine gate means a low-precision producer costs review time, not brief pollution.

## Tests

*(Acceptance is prose here — this is a design proposal; the concrete
`make unit-tests TEST=...` acceptance block lands with the first implementation slice, §3.1.)*

- Quarantine isolation: a quarantined candidate NEVER appears in `aimee memory recall` (unit), AND
  a source guard asserts the recall selectors (`db1_user_memory_list_recall`,
  `db2_memory_list_recall_section`) contain no reference to the quarantine table (structural, R2).
- Capability gate: a review mutating verb without `CAP_MEMORY_ADMIN` is refused (unit).
- Dedup: two extractions of the same signal collapse to one pending candidate via `content_hash` (unit).
- Scope enforcement: approving a `scope=unknown` candidate without `--scope` is rejected (unit).
- Gate: a PII-flagged or sub-threshold candidate is quarantined/dropped, never auto-approved (unit).
- No-hard-rule invariant: every §4 write path rejects `directive_type='hard'` (unit).
- Review verbs: `approve` releases to the correct store+scope tagged advisory; `reject` tombstones +
  suppresses re-proposal; TTL expiry logs and removes an unreviewed row (unit).
- Feedback→rules lifecycle: a repeat signal bumps `last_reinforced_at`; a stale rule passes
  `expires_at` and drops from recall (unit).
- Default-off: with all `autopopulate.*` flags 0, no producer runs and no candidate is written (unit).

## Review revisions (R1)

From the design roundtable (5 participants: MiniMax-M3, codex, mimo-v2.5-pro, GLM-5.2,
mistral-medium-3-5). The panel converged — near-unanimously — on these blocking findings, all folded
in above:

- **Feedback→rules must not bypass quarantine** (5/5). §2c now routes feedback-derived rules through
  the *same* quarantine+approval gate as extraction; nothing auto-writes a durable every-user rule.
- **The no-hard-rule invariant must be enforced structurally** (5/5), not only code-capped: a db2
  trigger/CHECK rejects an auto/quarantine-provenance `rules` row with `directive_type='hard'` (§0).
- **Quarantine is a sensitive (PII) store** (5/5): PII-flagged candidates carry a `sensitivity` tag,
  are operator-only (never surfaced), and have a hard retention TTL; raw PII content is access-gated,
  not shown in the plain review list.
- **Rejected candidates need fingerprint tombstones** (4/5): a `reject` records a content/semantic
  fingerprint; the extractor checks the tombstone set and will not re-propose a rejected fingerprint
  (with decay, so a genuinely changed fact can eventually re-surface) — no re-proposal loops.
- **Reinforcement must not self-feed** (4/5): `last_reinforced_at` bumps only on genuinely *new*
  evidence (distinct session/source), never a rule's own prior emission; decay still applies (§2c).
- **Multi-speaker scope leak** (4/5): an ambiguous-subject candidate is quarantined `scope=unknown`
  for operator scope-assignment, never auto-attributed to a user's db1 (§2a).
- **Quarantine isolation is structural** (2/5): the quarantine table sits physically outside the
  recall selectors; a guard test asserts recall never reads it (§2b).
- **Promotion is per-row, audited** (1, strong): per-row operator approval (or confidence-auto with a
  per-row WORM audit row + easy revoke), never per-batch bulk; a conflict check against the KB's own
  typed-fact ontology reconciliation avoids two systems promoting the same signal (§2d).
- **Phase 1 is low-blast-radius, not "zero risk"** (1): `approve` is a live write path, so the review
  command is itself operator-gated + audited (§3.1).

## Review revisions (R2)

Second roundtable pass (same 5-provider panel). It confirmed the architecture but flagged that R1
stated its invariants as prose without the *structural* mechanism that makes them hold, plus one
direct contradiction. All folded in above:

- **§2d contradiction fixed**: promotion is **per-row** (approval + WORM audit), never "per batch" —
  the earlier §2d/§3 "per batch" wording contradicted R1 and is removed.
- **No-hard-rule now covers db1 too** (was db2-only): the structural trigger/CHECK rejects a
  `hard` auto/quarantine row in **both** stores, and provenance is stamped by a single trusted,
  code-controlled release sink (ideally a dedicated SQL INSERT role for auto-writes) so an injected
  path cannot forge operator provenance (§0).
- **PII made structural**: the candidate schema gains a `sensitivity` column; the review `list`
  renders redacted previews for `pii` rows and gates raw content behind an audited `--reveal` (§2a/§2b).
- **Reinforcement self-feed made structural**: a `source_evidence_ref` + evidence-lineage check
  rejects a reinforcement whose evidence descends from the rule's own prior emissions (§2c).
- **Multi-speaker scope enforced**: a `scope=unknown` candidate cannot be released without an
  explicit `--scope`, enforced by a DB CHECK — no silent db1 mis-attribution (§2a/§2b).
- **`approve`/mutating verbs need a named capability**: a new `CAP_MEMORY_ADMIN` server auth cap
  (distinct from `CAP_MEMORY_WRITE`) gates the review verbs structurally (§2b).
- **Promotion ↔ KB-ontology reconciliation**: promotion checks the candidate against the KB's own
  typed-fact reconciliation so the two systems don't diverge (§2d).
- **Dedup / anti-flood**: a UNIQUE `content_hash` over pending candidates prevents the extractor from
  flooding the queue with the same signal (§2a).
- **Structural isolation is a named guard test**: a test asserts the recall selectors
  (`db1_user_memory_list_recall`, `db2_memory_list_recall_section`) contain no reference to the
  quarantine table — belt-and-suspenders on non-membership (Tests).
- **Namespace**: the auto-population queue lives under a **distinct** surface (`aimee memory
  quarantine`, or `aimee memory review --queue autopopulate`) to avoid conflating it with the
  charter-pipeline review (OQ3).
- **db1 decay symmetry (noted)**: whether auto-extracted db1 rows carry `expires_at`/reinforcement
  like db2 rules — to avoid stale identity drift — is called out for the schema slice (OQ4).

## Review revisions (R3 — converged)

Third roundtable pass. The panel **converged**: codex and mimo-v2.5-pro reported CONVERGED, mistral
and gpu-mid marked every R1/R2 blocker resolved. The only residual "blocking" votes (MiniMax-M3) ask
for deeper *implementation* concreteness — the exact confidence floor, the precise evidence-lineage
algorithm, the trusted-sink implementation, and the KB-reconciliation mechanics. These are
design-complete here (the mechanisms and their enforcement points are named) and their exact
DDL/thresholds land with the schema/implementation slice (§3.1), consistent with the design-vs-code
boundary; the open items are captured below. One small robustness point folded: the feedback→
quarantine path is **fail-closed** — if the candidate write fails, the signal is dropped, never
written as a durable rule.

## Open questions (post-R3)

Design-complete; these are implementation-slice decisions (not design blockers):

1. **Quarantine store shape**: the panel favoured a **single** `memory_candidates` table for both
   db1- and db2-targeted candidates (discriminated by `proposed_store`), rather than splitting or
   reusing `collab_rules`. Confirm columns + indexes at the schema slice (§3.1).
2. **Extraction confidence floor + dark-mode**: ship the producer **dark** (writes quarantine, review
   surface gated) over **episodes** first (not raw transcripts) until calibration data exists; the
   defensible confidence floor is set from that calibration. What initial floor is safe pre-data?
3. **Review namespace**: a **distinct** `aimee memory quarantine` (or a clearly separated
   `aimee memory review --queue autopopulate`) to avoid conflating the auto-extract queue with the
   existing charter-pipeline review — final naming.
4. **Retention/PII policy specifics**: exact TTLs (quarantine, tombstone), and whether raw PII
   content is stored encrypted vs. reference-only, pending the operator's data-handling policy.
