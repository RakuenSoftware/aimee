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

- **The automated pipeline can NEVER mint a hard rule.** Only operator-entered rules may be
  `directive_type='hard'`. Every §4 write is hard-capped at `soft`, and there is no code path from
  an inferred/injected signal to the non-overridable slot (Proposal 2 R2).
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
ttl_at, state (pending|approved|rejected)}`.

- **Nothing in quarantine is recallable** — the recall selectors (db1 `db1_user_memory_list_recall`,
  db2 `db2_memory_list_recall_section`) never read the quarantine table. It surfaces ONLY via the
  operator review command.
- **Gates before quarantine**: a candidate must clear the PII gate and a confidence floor; a PII or
  low-confidence hit is quarantined-for-review or dropped, never auto-approved.
- **Scope default**: unsure → db1/private (Proposal 2 §1 "over-sharing is the harm").
- **Hard cap**: any org candidate is `directive_type='soft'` (§0). No hard-rule path exists.
- **TTL**: an unreviewed candidate expires at `ttl_at` (default e.g. 30d) — "stored forever, never
  reviewed" is impossible; expiry is logged, not silently dropped.
- **Config**: `intelligence.autopopulate.extract.enabled` (default 0). Off ⇒ no extraction runs.

### §2b Quarantine review surface

`aimee memory review` gains the auto-extract queue with explicit verbs:
- `aimee memory review list [--store db1|db2] [--limit N]` — pending candidates with provenance.
- `aimee memory review approve <id> [--edit "..."]` — release into the target store at the correct
  scope (db1 via `memory.user_capture`; db2 as a `soft` rule/fact) tagged `provenance=auto-extracted`
  so the merge places it at the **untrusted-advisory** lattice tier.
- `aimee memory review reject <id> [--reason S]` — tombstone; feeds a negative signal so the same
  candidate is not re-proposed.
- `aimee memory review edit <id> "..."` — correct before approving.
- Also surfaced in the dashboard/console review panel (reuse the existing review surface).

### §2c Feedback → durable org rules

Route the feedback signal `kb_client_rules_generate` consumes into a **persistent** db2 `rules`
write path (in addition to, or replacing, the ephemeral brief block):
- New/reinforced rules land as `directive_type='soft'`, `domain` from the feedback topic, `weight`
  from evidence strength.
- **Lifecycle**: set `expires_at` (decay) and bump `last_reinforced_at` on repeat signal, so an
  erroneous/injected rule cannot persist indefinitely across every user's brief; a stale rule
  expires, a reinforced one survives.
- A revocation path (`aimee rules -` / review reject) tombstones a bad auto-rule.
- **Config**: `intelligence.autopopulate.feedback_rules.enabled` (default 0).

### §2d Promotion → durable org facts/rules (strict gate)

Consolidate recurring, high-confidence db2 `episode` rows into durable org facts/rules:
- Runs only when `intelligence.autopopulate.promote.enabled` (default 0) AND a promotion is
  operator-gated per batch (a review step), because it mutates shared multi-user state.
- Each promotion writes a **WORM audit** row (reuse `audit_worm_*` / `kb_audit_event`) recording
  source episodes, resulting row, and approver.
- Hard-capped at `soft`; the promoted row enters recall at the advisory tier until an operator
  elevates it via explicit capture.

## §3 Phasing (each independently shippable; all default-off)

1. **§2b review surface + quarantine table + schema** — the *sink and the gate UI* first, so nothing
   can be produced without a place to review it. `aimee memory review list/approve/reject/edit`
   against a hand-seeded candidate. (No producer yet ⇒ zero risk.)
2. **§2a extraction producer (default-off)** — the gated offline pass that fills quarantine, behind
   `extract.enabled=0`; calibratable.
3. **§2c feedback→durable rules (default-off)** — the durable sink for the feedback signal, with
   decay/reinforcement lifecycle.
4. **§2d promotion (default-off, strict gate)** — last, because it mutates shared org state; gated
   per-batch with WORM audit.

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

- Quarantine isolation: a quarantined candidate NEVER appears in `aimee memory recall` (unit).
- Gate: a PII-flagged or sub-threshold candidate is quarantined/dropped, never auto-approved (unit).
- No-hard-rule invariant: every §4 write path rejects `directive_type='hard'` (unit).
- Review verbs: `approve` releases to the correct store+scope tagged advisory; `reject` tombstones +
  suppresses re-proposal; TTL expiry logs and removes an unreviewed row (unit).
- Feedback→rules lifecycle: a repeat signal bumps `last_reinforced_at`; a stale rule passes
  `expires_at` and drops from recall (unit).
- Default-off: with all `autopopulate.*` flags 0, no producer runs and no candidate is written (unit).

## Open questions for the roundtable

1. **Quarantine store**: a new db2 `memory_candidates` table, or reuse `collab_rules`'
   proposed/decided pattern generalized? One table for both db1- and db2-targeted candidates, or split
   by store?
2. **Extraction cadence + source**: offline batch over episodes only, or also transcripts? What
   confidence floor is defensible before calibration data exists — or ship the producer dark (writes
   quarantine, review surface hidden) until calibrated?
3. **Feedback→rules vs the ephemeral brief block**: additive (both) or replace the ephemeral block
   once durable rules exist? Migration of any in-flight ephemeral guidance?
4. **Promotion gate granularity**: per-row approval vs per-batch vs confidence-auto-approve with WORM
   audit + easy revoke? How does promotion interact with the KB's own typed-fact ontology
   reconciliation (avoid two systems promoting the same signal)?
5. **Review surface reuse**: extend the existing `aimee memory review` (charter pipeline) or a
   distinct `aimee memory quarantine` namespace to avoid conflating two review queues?
