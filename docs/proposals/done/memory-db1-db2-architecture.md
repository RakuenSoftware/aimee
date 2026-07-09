# Proposal: Memory architecture — retire `.md` memory; db1 = user, db2 = organization

- **State:** shipped — Phase 1 (db1 user store + recall merge + capture), migration Slices 1/3/4,
  and §2 Slice 5 (harness-memory subsystem removal + CI guard) are all merged on `testing`. Every
  acceptance criterion is met; §4 Phase-4 auto-population (OQ5) is deferred to a follow-up. See
  "Implementation status (as-built)" below.

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

## Migration design (R3 — `.md` retirement, roundtabled)

The `.md`-retirement migration (§2) was roundtabled (7 participants) before any code. Verdict: the
migration **cannot be safely automated end-to-end** — it is operator-gated at every write/delete.

**Hard conclusions (data-loss traps):**
- **Scope is not mechanically derivable.** `harness_memory.type ∈ {fact,index,note,scratch}` carries
  no user/org signal; the only signal is the `.md` frontmatter `metadata.type`
  (project/feedback/reference/user), and it is advisory. Auto-defaulting is silent + irreversible:
  default→db1 locks org-generalizable notes away; default→db2 leaks private notes org-wide.
  **Classification MUST be operator-reviewed per memory.**
- **Never bulk-load free-form `.md` into structured `user_memories`.** The `identity:`/`pref:` recall
  selectors won't match arbitrary bodies → *stored-but-not-surfaced* = data-loss-by-inaccessibility.
  Preserved documents need a **separate, non-recallable archival kind/table**, distinct from the
  structured identity/preference store.
- **The server db1 `harness_memory` table is canonical; the client `.md` files are a re-hydrated
  (lossy) derivative.** Reconcile against the table, not the files; never hash hydrate's re-render.
- **Define the replacement write path BEFORE removing interception.** Deleting
  `memory_redirect`/PostToolUse before agents have a new memory target drops all NEW writes silently.
- **Verification is content-hash + key-set reconciliation, not count-only**; keep the source
  read-only until an explicit per-phase reversibility gate confirms the copy.

**Sequenced slices (each gated, reversible):**
1. **Slice 1 (shipped, read-only):** `scripts/harness-memory-inventory.py` — inventories the `.md`
   store, surfaces per-memory scope-signals for classification, and (best-effort) reconciles against
   the canonical server table. Writes nothing, deletes nothing.
2. **Operator classification** — the operator labels each memory user(db1)/org(db2)/archive/drop
   (no automated scope guess).
3. **Migration writes** — copy per the classification into db1 (user), db2 (org), or a non-recallable
   archival store; verify by content-hash + key-set; **source retained**.
4. **Replacement write path** — new agent memory routes to db1/db2 (extends S2's capture) so
   interception can be removed without dropping writes.
5. **Subsystem removal** — delete `harness_memory_*`/`memory_redirect`/routes/tests/docs, gated on
   (2)+(3)+(4) confirmed, after a read-only compatibility window.

## Implementation status (as-built — 2026-07-08)

Verified against `origin/testing` @ `d76cdfef`. The proposal is substantially shipped; the sole
remaining acceptance criterion is **Removal** (§2 Slice 5), which is operator-gated by design.

| Proposal item | Status | Where |
|---|---|---|
| §3 db1 user-memory store + schema (OQ1) | **Shipped** | new `user_memories` table + `src/db1/user_memory.c`; PR #1040. OQ1 resolved: a **clean new table** (not a repurposed `harness_memory`), mirroring db2's tier/kind/lifecycle discipline. |
| §3 recall reads both db1 ⊕ db2, merged | **Shipped** | `kb_client_memory_recall_json_ex` merges db1 identity/preferences on top of the kb (db2) bundle in aimee-server (the 1:1 per-user seam); `db1_user_memory_merge_into_array`. PR #1040. |
| §4 explicit capture routes by scope | **Shipped** | `aimee memory identity` / `aimee memory prefer` → db1 (key `identity:`/`pref:`, tier L2); `aimee rules +` → db2. PR #1045. |
| §4 tier-gate (curated captures surface without `--tier`) | **Shipped** | capture writes tier L2, inside the recall selector band. PR #1045. |
| Precedence (§3 / R2 lattice) — user capture overrides a conflicting soft org default | **Shipped (identity/prefs)** | merge de-dups on `key` and inserts the db1 row first ("db1 wins"), tagging `scope:user`. Hard org rules remain a separate always-on section (`memory_context.c` §7), so they are never suppressed. |
| §2 Slice 1 — migration inventory | **Shipped** | `scripts/harness-memory-inventory.py` (read-only). PR #1046. |
| §2 Slice 3 — `.md` → db1 archive migration | **Shipped** | `scripts/harness-memory-migrate.py` (safe default: all → db1 private `archive`/L1, source retained, dry-run default) + `aimee memory archive`. PR #1047. |
| §2 Slice 4 — replacement write path (interception → db1) | **Shipped, default-ON** | config `memory_md_retire` (**default-on**): a memory-`.md` Write is denied and stored as a private non-recallable db1 `archive:<project>/<name>` row; session-start hydrate skipped; brief steers the agent to aimee memory. PRs #1048, later default-on flip. |
| **§2 Slice 5 — subsystem removal + CI guard** | **Shipped** | deleted the `harness_memory` mirror table, `_hydrate`/`_watch`, `server/harness_memory_routes.c`, the `harness_memory.*` RPC ops + routes + auth caps, and the `memory_md_retire` config flag (retirement is now unconditional); the replacement path (`memory_redirect` → db1 archive) and `user_memory` are retained, as are the shared `harness_memory_{common,scope,audit,spill}` interception helpers. Added `scripts/check-md-store-retired.py` (wired into `make lint`). |
| §4 Phase 4 — feedback→durable db2 rules, promotion, gated extraction, quarantine review | **NOT done (default-off scope)** | OQ5 (ship default-off now vs. hold for calibration) remains open. |

### Open questions — resolution
- **OQ1 (db1 schema):** RESOLVED — a clean `user_memories` table, not a repurposed `harness_memory`.
- **OQ2 (Active Context scope):** the shipped merge covers identity + preferences (always user/db1);
  Active Context remains per-item as designed and is not yet split into a db1 half.
- **OQ3 (migration fidelity):** RESOLVED by the safe default — migration sends **all** `.md` to db1
  private `archive` (non-recallable), source retained; operator promotes to org later. No automated
  scope guess, no data loss.
- **OQ4 (precedence):** RESOLVED for the shipped scope — user capture wins over a same-`key` org row;
  hard org rules are a separate, non-overridable section.
- **OQ5 (Phase-4 extraction default):** **OPEN** — operator decision.

### §2 Slice 5 subsystem-removal — as executed

Removal was the last acceptance criterion, executed on operator direction after the live `.254`
check confirmed the canonical `harness_memory` table is empty there (0 rows — nothing to
classify/lose). What was done (and what any other deployment should still run before upgrading):

1. **Confirm migration on the live db1** — run `scripts/harness-memory-migrate.py --apply` (safe:
   all → db1 `archive`, source retained), then verify by content-hash + key-set that every
   `harness_memory` / `.md` row produced a db1 archive row. `.md` source files are retained (never
   deleted) until step 5.
2. **Separate the replacement write path from the legacy subsystem FIRST.**
   `src/memory_redirect.{c,h}` is **NOT** legacy — it is the shipped replacement interception/write
   path (#1048) that stores intercepted memory into db1. It must be **preserved**. Before any
   deletion, split it so its `md_retire`-on db1-archive path has no compile/link dependency on the
   legacy hydration/spill/watch modules (today it pulls in `harness_memory_common`/`_audit` for
   `hmem_audit`/`hmem_resolve_project` — move those helpers it still needs into a small retained
   unit, or inline them, so the legacy files can be deleted without breaking the replacement path).
3. **Delete the legacy subsystem code** (reachable only when `memory_md_retire` is off, now the
   deprecated legacy mode): `src/db1/harness_memory.{c,h}`,
   `src/harness_memory_{scope,spill,hydrate,watch}.{c,h}` (and `_common`/`_audit` once the
   replacement path no longer needs them), `src/server/harness_memory_routes.c`; drop the
   `harness_memory.*` RPC ops (`server/server.c` dispatch table) and their auth caps — retiring an
   RPC via a `GONE` contract (respond `method gone` for one release) rather than a bare removal so
   an older client gets a clear error, not a silent 404; make `memory_md_retire` unconditional in
   `memory_redirect.c` (drop the `md_retire`-off branch); drop the hydrate-skip conditional in
   `cli_session_start.c`.
4. **Remove the build + test entries**: the legacy `harness_memory*` glue in `src/Makefile`
   (`DB1_SRCS`, `CLI_SRCS`, `SERVER_SRCS`) — **keep `user_memory.c` and `memory_redirect.c`**; the
   `unit-test-harness-memory*` targets in `src/tests/Rules.mk`, the CMake registrations, and the six
   `test_harness_memory*` files (keep the merge/precedence assertions that live in
   `test_harness_memory.c` by moving them to a retained `test_user_memory.c`).
5. **Add the CI guard** (`scripts/check-md-store-retired.py`, wired into a `*-check` Makefile target
   like the existing `check-*.py` guards). Use an **allowlist** contract: the guard fails if any file
   outside the known replacement path (`memory_redirect.c`) calls a `.md`-materializing entry point
   (`harness_memory_hydrate`/`_watch`/`_spill` re-render), so the retirement can't silently regress
   and the removal stays enforced — the "CI guard enforces it" half of the criterion.
6. **Read-only compatibility window, then delete `.md` sources** — only after steps 1–5 are confirmed
   on the live deployment, per the reversibility gate.

Verify each step with `cd src && make -j all server` and the memory unit tests (all green at time of
writing). Full end-to-end validation of the interception change requires the live `.254` stack
(**validation-pending** until then). `scripts/harness-memory-inventory.py --worklist` produces the
operator classification worklist (path / frontmatter-type / scope-signal / suggested-disposition +
a blank operator-decision column) that gates step 1.

### §4 Phase 4 — split by risk (roundtable R4)

The design roundtable flagged that shipping §4 as a single default-off switch under-gates
cross-user state mutation. Split it:

- **§4a — extraction → quarantine (safe to ship default-off).** The gated LLM pass proposes user
  preferences/identity (→ db1) and org conventions (→ db2) into a **quarantine** queue only; nothing
  reaches a brief until an operator `approve`s it via the review surface. Extraction is hard-capped at
  `soft`, passes the confidence/PII gates, and each row carries a TTL so an unreviewed proposal
  expires rather than lingering. This is calibratable behind the operator enable.
- **§4b — promotion → durable org rules (requires a separate, strict operator gate + audit trail).**
  Consolidating recurring db2 episodes into durable org `rules` mutates shared, multi-user state, so
  it stays behind its own operator gate with a WORM audit entry per promotion — never auto-enabled by
  the §4a switch.

### Acceptance-criteria status checklist

- [x] Scope routing: `aimee memory identity`→db1, `aimee rules +`→db2 (unit-tested).
- [x] Recall merge: db1 identity + db2 rule appear in one bundle, correctly sectioned (unit-tested).
- [x] Precedence: a user capture overrides a same-key org row in the merged brief (unit-tested);
      hard org rules stay in a separate, non-overridable section. *(Roundtable R2: extend explicit
      lattice/dedup unit coverage — see `test_harness_memory.c` merge assertions.)*
- [x] Migration: a sample `.md` set imports into db1 archive by the safe default with no content loss.
- [x] Tier policy: a freshly-captured preference surfaces in recall without a manual `--tier`.
- [x] **Removal: after §2, no code path reads/writes the `.md` store; CI guard enforces it.**
      *(Legacy subsystem deleted; `scripts/check-md-store-retired.py` enforces it, wired into
      `make lint`.)*
