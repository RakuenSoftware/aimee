# Proposal: Memory architecture — retire `.md` memory; db1 = user, db2 = organization

## Thesis

Aimee's durable memory today is fragmented and mis-scoped:

- The operator's real curated knowledge lives as **`~/.claude/.../memory/*.md` files** — a
  file-based "central agent-memory interception" subsystem (`harness_memory_*` ×7 +
  `memory_redirect` + `db1/harness_memory` + `server/harness_memory_routes`) that mirrors agent
  `.md` writes and hydrates them into a local dir. It is brittle, host-coupled, un-queryable as
  structured memory, and does **not** feed the session-start brief.
- The structured store (**db2**, central Postgres) holds *everything else* — the `memories` table
  (identity/preferences/facts via recall selectors), the `rules` table, provenance, and the code
  graph — with **no scoping** between what is personal to a user and what is shared org knowledge.
- Result: recall's curated categories are empty (on `.254`, ~all 52 rows are `tier=L1,
  kind=episode` delegation feedback), so the brief has nothing to show even when delivered.

**Decision (operator, 2026-07-04):** retire the `.md` memory mechanism entirely, and split durable
memory by **scope**:

- **db1 (local SQLite) = user-specific memory** — who this user is and how they work: identity,
  personal preferences, personal commitments/reminders. Private to the user's instance.
- **db2 (central Postgres) = generalizable / organization memory** — knowledge applicable across
  the company: rules, coding conventions, shared project facts, decisions, the code graph.

The session-start brief then assembles from **both** (user ⊕ org), and ingestion routes each new
memory to the correct store by scope.

*(Companion proposal `remote-first-session-start.md` fixes brief **delivery**; this fixes what the
brief is built **from**. They compose but ship independently.)*

## Goal

1. **Retire** the `.md`/harness-memory subsystem — no more file-based agent memory.
2. **db1 = user memory, db2 = org memory**, with a clear classification of every memory kind.
3. Session-start recall reads **both** db1 (user) and db2 (org) and merges them.
4. Ingestion **routes by scope**, so identity/preferences land in db1 and rules/conventions/facts
   land in db2 — automatically, not by hand.

## §0 Deployment model (the invariant everything rests on)

- **aimee-server is 1 user : 1 instance** — never multi-tenant. db1 (its local SQLite) is therefore
  **per-user by construction**; no tenancy, partitioning, or access-control is needed on it. db1 =
  *this* user's private memory.
- **aimee-kb is many users : 1 instance** — the shared knowledge service that owns **db2**
  (Postgres). db2 is therefore the **organization-shared** store, read by every user's aimee-server.
- The session-start brief is assembled **on the user's own aimee-server**, merging its per-user db1
  with the shared kb/db2. There is no cross-client db1 access and no thin-client-side db — the
  server the client talks to *is* that user's instance.

Direct consequence: **user-specific data must never be written to db2/kb** — it is a shared,
multi-tenant store, so anything personal put there leaks into other users' briefs. Scope is a
correctness/privacy boundary, not just organization.

## §0.1 What already exists (so we don't rebuild it)

- **db2** already has the structured `memories` table (tier/kind/key/content/lifecycle/…), the
  `rules` table (`src/db2/schema.sql:15` — org rules with `directive_type`, `weight`, `domain`),
  provenance, conflicts, and recall selectors (`db2_memory_list_recall_section`,
  `src/db2/memory_score_fields.c:1355`).
- **db1** already has a `harness_memory` table (`src/db1/harness_memory.c`) — but it is the `.md`
  mirror, not a first-class user-memory store. It also holds session_state, token_audit, etc.
- Recall currently queries **db2 only** (`src/memory_context.c:835`+).
- The `.md` subsystem to retire: `harness_memory_hydrate/common/spill/audit/scope/watch`,
  `memory_redirect`, `db1/harness_memory`, `server/harness_memory_routes`, and the local-dir
  hydration + `MEMORY.md` index.

## §1 Scope model — which memory kind goes where

**Scope is a per-memory property, not a fixed per-category rule.** Review consensus (8 reviewers)
was that "Active Context" and "Open Commitments" are each user *or* org depending on subject and
audience — so each memory carries an explicit `scope ∈ {user, org}` tag that decides its store, and
a recall category can draw from **both** db1 and db2 at assembly time.

| Recall category | Typical scope | Store | Notes |
|---|---|---|---|
| **Identity** (who the user is) | user | **db1** | always personal |
| **Preferences** (how they work) | user | **db1** | always personal |
| **Always-On Rules / Directives** | org | **db2** | shared conventions (`rules` table already here) |
| **Key Facts / decisions / code graph** | org | **db2** | shared org knowledge |
| **Active Context** | **per-item** | db1 *or* db2 | "*my* current branch/task" → db1 (user); a generalizable project fact → db2 (org) |
| **Open Commitments / Reminders** | **per-item** | db1 *or* db2 | a personal reminder → db1; a shared/team commitment ("land PR X before freeze") → db2 |

The default dividing test when scope is ambiguous: *"would another engineer benefit, with no privacy
cost?"* → db2 (org); *"is this about **this** person, their machine, or their in-progress work?"* →
db1 (user). When unsure, **default to db1 (user/private)** — over-sharing into the multi-tenant
db2 is the harmful failure (§0).

### Precedence when db1 (user) conflicts with db2 (org)

Not a flat "user wins." Org rules split into two classes:
- **Hard org rules** (security / compliance, `directive_type='hard'`) are **non-overridable** — a
  user preference can never suppress "always run tests" or a compliance directive.
- **Soft org defaults** yield to a conflicting user preference.

The merge marks each surfaced rule's class so the primary agent can see which are inviolable.

## §2 Retire the `.md` memory subsystem

Remove the file-based interception + hydration path and the local `~/.claude/.../memory/*.md`
store. Replace agent memory writes with direct db1/db2 stores (§4). Migrate any still-valuable
content out of the existing `.md` files into db1/db2 by scope as a one-time import, then delete the
subsystem and its CI/tests. *(This is a deprecation with a migration step — sequence carefully so
no durable knowledge is lost in the cutover.)*

## §3 db1 becomes a first-class user-memory store; recall reads both

- Give db1 a proper user-memory schema (repurpose/replace `harness_memory`): identity, preference,
  commitment/reminder kinds with the same tier/lifecycle discipline db2 uses.
- Add db1 recall queries mirroring the db2 recall selectors for the **user** categories.
- `memory_context.c` merges db1 (user) + db2 (org) sections into one recall bundle for the brief.
- Ordering/precedence and de-dup across the two stores decided in review (user identity/preferences
  should generally win over a conflicting org default).

## §4 Ingestion routes by scope

Every durable memory write is classified user-vs-org and routed to db1 or db2:

- **Explicit capture** (first-class, low-friction): `aimee identity set …` / `aimee prefer "…"` →
  db1; `aimee rule add "…"` / convention capture → db2. Idempotent (dedupe on key).
- **Feedback → durable org rules:** route the same feedback signal `kb_client_rules_generate`
  consumes into persistent db2 `rules`/directives, not just an ephemeral brief block.
- **Extraction (default-off, gated):** an LLM pass proposes user preferences / identity (→ db1) and
  org conventions (→ db2) from transcripts, behind confidence + PII gates
  (`memory_fact_gate` / `memory_pii_gate`).
- **Promotion:** recurring high-confidence db2 `episode` rows consolidate into org facts; stable
  personal patterns consolidate into db1 preferences.
- **Tier gate:** curated captures land at the tier recall requires (L2+) by policy, so they surface
  immediately (removes the `--tier L2` friction seen in the manual seed).

## Phasing (each independently shippable; behavioural steps default-off)

1. **§3 (schema + read-merge)** + **§4 explicit capture + tier policy** — db1 user store exists,
   recall reads both, `aimee identity/prefer/rule` populate the right db. Seed the operator's known
   durable set. *(Makes the brief non-empty with correctly-scoped content.)*
2. **§2 migration** — import valuable `.md` content into db1/db2 by scope.
3. **§2 removal** — delete the `.md`/harness subsystem, routes, and tests.
4. **§4 feedback→rules + promotion + gated extraction** — automatic population, calibrated,
   default-off with an operator enable.

## Non-goals

- Standing up a co-located server (topology is remote-only).
- Re-embedding / re-ranking changes to retrieval (separate track).
- Multi-user scoping *within* one db1 (db1 = this user's instance; cross-user is a later concern).

## Risks / honest limits

- **Cutover data loss:** retiring `.md` must not drop durable knowledge — the §2 migration must run
  and be verified before removal (phased: migrate, then delete).
- **Mis-classification:** an org rule wrongly stored in db1 (or vice-versa) mis-scopes the brief;
  the explicit-capture commands make scope a deliberate choice, and extraction is gated/default-off.
- **Prompt-injection / provenance:** auto-extracted identity/rules become high-primacy brief text —
  must pass provenance + PII gates before influencing the primary agent. Default-off until
  calibrated.
- **db1 durability/location:** db1 is local SQLite in the server instance; "user-specific" assumes
  one instance per user. If instances are ever shared, db1 needs per-user scoping (out of scope
  here, flagged).
- **Two-store conflicts:** a user preference contradicting an org rule needs a precedence rule (§3).

## Tests

- Scope routing: `aimee identity set` writes db1; `aimee rule add` writes db2 (unit).
- Recall merge: a db1 identity + a db2 rule both appear in one recall bundle, correctly sectioned.
- Precedence: a user preference overrides a conflicting org default in the merged brief.
- Migration: a sample `.md` set imports into db1/db2 by scope with no content loss.
- Removal: after §2, no code path reads/writes the `.md` store; CI guard enforces it.
- Tier policy: a freshly-captured preference surfaces in recall without a manual `--tier` override.

## Open questions for the roundtable

1. **db1 user-memory schema:** repurpose `harness_memory` or a clean new table? What tier/lifecycle
   discipline does db1 inherit from db2?
2. **Active Context scope:** project facts are org (db2), but "what I'm working on right now" is
   arguably user (db1) — one category or split?
3. **Migration fidelity:** which existing `.md` files are worth importing vs dropping, and who
   classifies them (heuristic vs one-time LLM pass vs operator review)?
4. **Precedence:** exact rule when a db1 user preference conflicts with a db2 org rule.
5. **Extraction default:** ship §4 extraction/promotion default-off with an operator enable, or
   hold until calibration data exists?

## Review revisions (R1)

From the design roundtable (7 participants, 58 findings). Folded in:

- **§0 Deployment model added** — resolves the most-cited finding (db1 tenancy, flagged 5×):
  aimee-server is 1:1 per user so db1 is per-user by construction; aimee-kb is many:1 so db2 is the
  shared org store. User data in db2 is now explicitly a **privacy leak**, not just a mis-scope.
- **§1 is now per-item scope** (8× consensus) — Active Context and Open Commitments carry an explicit
  `scope` tag and draw from both stores; default-to-db1-when-unsure (over-sharing is the harm).
- **Precedence refined** (4×) — hard org rules (security/compliance) are non-overridable; only soft
  org defaults yield to user preferences. Prevents a user "skip tests" preference suppressing an org
  rule.
- **Migration protocol (§2), made concrete** (6×): the `.md` cutover runs
  **migrate → verify → operator-classify → delete**, gated on acceptance criteria and reversible:
  - *Verify* = deterministic row-count + content-hash reconciliation between the `.md` source set and
    the db1/db2 rows it produced; the delete phase is blocked until reconciliation passes.
  - *Classify* = an operator-reviewed pass decides user-vs-org per file (OQ3); no silent heuristic
    deletion.
  - *Rollback* = the `.md` store is retained (read-only) until an explicit post-migration
    confirmation, so the deletion phase is reversible.
  - *Removal completeness* = the delete list explicitly includes the subsystem's **tests, CI jobs,
    and docs**, enforced by a CI guard that no code path reads/writes the `.md` store.
- **Provenance / trust boundary (§4), specified** (5× — prompt-injection theme): repo-file
  conventions (`AGENTS.md`/`.aimee-rules`) and any auto-extracted memory are marked **untrusted
  data, not instructions**, at a lower trust tier than operator-entered captures; only explicit
  operator captures (§4 commands) enter at full primacy. Gate failure semantics are defined:
  a PII/low-confidence hit is **quarantined** (stored, not surfaced) pending review — never silently
  redacted or surfaced.
- **Auto-generated org rules get a lifecycle** — feedback→db2 rules carry decay + a review/revocation
  path (reuse the `rules` table's `expires_at`/`last_reinforced_at`), so an erroneous or injected
  rule cannot persist indefinitely across every user's brief.

Deferred to implementation / remain open questions: the db1 user-memory schema (OQ1), exact
per-file migration classification (OQ3), and the extraction default (OQ5).

## Review revisions (R2)

Second-pass roundtable confirmed the three R1 blocking themes (db1 tenancy, phasing, trust boundary)
are closed, and surfaced two **new** blockers introduced by the R1 additions, plus refinements.
Folded in:

- **Unified precedence lattice (closes the new blocker; supersedes the two separate orderings).**
  One total order governs every rule/fact that reaches the brief, highest authority first:
  1. **Hard org rules** (security/compliance, `directive_type='hard'`) — inviolable, operator/policy
     sourced only.
  2. **Operator-entered user captures** (db1 identity/preferences via §4 commands).
  3. **Soft org defaults** (db2).
  4. **Untrusted advisory** — repo-file conventions (`AGENTS.md`/`.aimee-rules`) and
     auto-extracted / quarantine-released memories. Advisory context, never instructions; loses to
     every tier above.
  P1's repo-file trust tier and this lattice are the **same** ordering — the merge in both proposals
  resolves conflicts by this single lattice. (Rationale: user preference outranks a *soft* org
  default but never a *hard* one; anything untrusted is always lowest.)
- **The automated pipeline can never mint a hard rule (closes the injection blocker).** Only
  operator-entered rules may be `directive_type='hard'`. Feedback→rules, extraction, and promotion
  are **hard-capped at `soft`** and pass through quarantine/review; there is no code path from an
  inferred/injected signal to the non-overridable slot.
- **Quarantine review surface defined** (the gate was non-functional without it): a review queue with
  explicit `approve` / `reject` / `edit` verbs, surfaced to the operator (e.g. in the dashboard and
  a `aimee memory review` command); quarantined rows have a TTL so "stored forever, never reviewed"
  is impossible — an unreviewed row expires rather than silently persisting or bulk-approving.
- **Merge dedup key specified:** de-dup across db1 ⊕ db2 (and, in P1, server-half ⊕ client-half) keys
  on `(normalized-scope-independent-key, kind)`; on collision the higher lattice tier wins and the
  loser is dropped, so a repo convention can't duplicate a captured rule.
- **Migration reconciliation granularity pinned:** the delete-gate hash is over the **structured**
  projection `(key, scope, kind, normalized-payload)` of each produced row, plus a **key-set-equality**
  check between the `.md` source set and the db1/db2 rows — not a prose-blob hash. Pass requires both.
- **Migration concurrency handled:** the classify/verify phase requires a **detach/drain step** —
  no still-attached agent may write the `.md` store while it is read-only; late writes are rejected
  with a clear error, not silently dropped, so nothing lands un-migrated.
- **db1 schema decision pulled into Phase 1:** §4 explicit-capture commands can't ship on an undecided
  schema, so OQ1 (repurpose `harness_memory` vs new table) is **resolved as the first step of
  Phase 1**, before the commands.
