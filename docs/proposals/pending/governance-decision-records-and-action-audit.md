# Proposal: Governance surface — decision records + per-action policy-verdict audit

**Status:** DRAFT (roundtable-reviewed 2026-07-02, findings incorporated — see Disposition).
**Owner:** memory / guardrails

> **Framing.** This came out of surveying a governance-layer product
> ([sechroom.ai](https://sechroom.ai/)) that "sits between AI assistants and business tools"
> and sells four things: a single governed MCP interface, **policy enforced at the point of
> action** ("every governed write is checked against your policy at the point of action and
> recorded with its verdict"), **per-action audit + replay** ("who acted, which tools, what
> changed, what policy applied … so a sequence can be replayed"), and **curated memory** with
> dedup + human approval. Its one explicitly-named primitive is the **Decision record**
> (`status / rationale / alternatives / revisit-date / supersedes / author / linked-policies /
> version history`).
>
> **aimee already implements four of the five pillars** — as internal agent plumbing, not as an
> inspectable governance surface. This proposal takes only the two genuine deltas. After
> grounding the design against the code (§Grounding), **both deltas reduce to extending storage
> and call sites that already exist** — no new table for the audit, no new scheduler, no new
> ontology, no hot-path write.

## What already exists (do NOT rebuild)

| Governance pillar | aimee's existing structure |
| --- | --- |
| Single governed MCP interface, scoped tool exposure | `src/mcp_tool_profile.c` + `src/toolset.c` + `src/gateway_policy.c` |
| Policy enforced at the point of action | `src/guardrails_orchestrator.c::pre_tool_check()` — the per-tool-call choke point that already computes an allow/block verdict **and already calls `audit_log()` with a stable event key at each block site** |
| Human sign-off on sensitive actions | `src/workflow/wfe_approval.c` — HMAC-SHA256-signed, non-forgeable human gates |
| Curated memory + dedup + human approval | `src/memory_fact_gate.c` + `src/integrity_gate.c` + `src/kb_neardup.*` + curator (`src/kb_curator_provider.c`) |

## Grounding (verified against the code, 2026-07-02)

These three facts are load-bearing and were verified before finalizing:

- **G1 — `pre_tool_check()` already audits at block sites.** `src/headers/guardrails.h:134` documents
  the `0=allow / 1=rewrite / 2=block` return contract; the human message is free-form prose in
  `msg_buf`, **but** each block site also calls `audit_log("<stable_key>", …)` with a bounded
  event key — `read_before_write` (`guardrails_orchestrator.c:1783`), `truncating_write` (`:1813`),
  `stale_edit` (`:1845`), `subagent_blocked` (`:1615`), `antipattern_blocked` (`:1673`). `audit_log()`
  (`src/log.c`) writes JSON lines to a mode-0600, rotated `audit.log`. So a **bounded reason code and
  a fire-and-forget audit sink already exist at exactly this call site.**
- **G2 — a `decision_log` table already exists.** `src/db2/schema.sql:29`:
  `decision_log(options, chosen, rationale, assumptions, outcome, created_at)` — the decision-record
  shape is already purpose-built. Relation *types* are runtime-registerable via the `rel_types` table
  (`schema.sql:1067`, `db2_rel_types_stage_provisional()` in `rel_types_store.c:132`) — a plain
  `INSERT`, **no migration**. Agent-memory rows (`memories`, `schema.sql:24`) carry a free-text
  `kind` column and `ttl_at` / `valid_until` expiry columns.
- **G3 — revisit hooks already exist, no scheduler.** The curator drain runs a periodic poll loop
  (`kb_curator_drain.c` `drain_thread_main`, `DRAIN_POLL_SECS=5`); the recall path already runs a
  lazy expiry sweep — `recall_fill_reminders()` calls `memory_directive_sweep_expired()`
  (`memory_context.c:920`) using the existing `ttl_at`/`valid_until` columns.

## Delta 1 — Decision record (reuse+extend `decision_log`, not a new store or the ontology)

**Problem.** aimee stores durable "we decided X because Y" as freeform `feedback`/`project`
memories, carrying rationale but not the fields that make a decision *governable over time*:
what it **supersedes**, **when to revisit**, and which **policy** it binds. A decision with no
revisit trigger rots silently — the exact "forgets your context every morning" failure the
agent-memory thesis exists to kill.

**Build (on existing storage).**
- **Record body → extend the existing `decision_log` table** (§G2). Add the governance columns it
  lacks: `status` (`active`/`superseded`/`revisit_due`), `revisit_when` (reuse the `ttl_at` date
  convention), `supersedes_id` (self-FK), `author`, `linked_policy_id`. This is a column add to a
  **purpose-built existing table**, not a new table and not per-edge metadata — which is the
  cleaner model the panel asked for (§Disposition D1a). `options`/`chosen`/`rationale` already
  cover alternatives + choice + why.
- **Graph linkage → runtime-registerable relation types** (§G2), no migration: register
  `supersedes`, `linked-policy`, `decided-by` as `rel_types` rows so a decision participates in the
  memory graph (recall, contradiction detection) without overloading the ontology with metadata.
- **Version chain + invariant.** `supersedes_id` + `status` + `created_at` give the full temporal
  chain (answers "what was decided on date X"). Invariant: **at most one `active` decision per
  scope**, enforced by the write gate (`memory_fact_gate` verdict path) — flip the prior to
  `superseded` in the same write.
- **`revisit_when` → reuse the existing expiry machinery, NOT conflict detection** (§G3,
  §Disposition D1b). A due decision surfaces two ways, both on paths that already run: **lazily on
  recall** (extend the `memory_directive_sweep_expired()` sweep in `recall_fill_reminders()` to flip
  `status=revisit_due` and surface it), and **opportunistically** in the existing `kb_curator_drain`
  poll. No cron, no poller, no new thread.

**Not building:** a new `decision_records` table, per-edge metadata bags, or `memory_conflict`
involvement.

## Delta 2 — Per-action policy-verdict audit (enrich the audit call that already fires)

**Problem.** The only genuinely *partial* pillar. `db1_lifecycle_event_add()` records
*workflow-stage* decisions with attribution; `trajectory_export.c` replays interactions — but the
general governed **tool-call** stream is only coarsely logged. Sechroom's "checked at the point of
action and **recorded with its verdict** … so a sequence can be replayed" holds for workflow stages,
not per tool call.

**Build (enrich the existing `audit_log()` call, single sink, off the enforcement path).**
- The block sites in `pre_tool_check()` **already call `audit_log(stable_key, …)`** (§G1). Enrich
  that existing JSON row with the structured fields: `{ts, actor, tool, args_hash, mode,
  reason_code, verdict}`. Add **one** `audit_log()` call on the allow path (`return 0`,
  `guardrails_orchestrator.c:1999`) so allows are audited too. **One sink** = the existing
  `audit.log` (already JSON lines, 0600, rotated) — no DB1 write, no second surface (§Disposition
  D2c, D2e).
- **`reason_code`, not prose** = the stable event key that already exists (`read_before_write`,
  `stale_edit`, …). Free-form `msg_buf` is **never persisted** — kills the reason-PII finding
  (§Disposition D2b).
- **Fail-open by construction.** `audit_log()` is side-effect-only logging invoked *after* the int
  verdict is decided; a log failure cannot alter the returned `0/1/2`. State this as an invariant:
  **audit loss is acceptable, enforcement drift is not** (§Disposition D2a). No hot-path
  transactional write → the perf finding is moot (§Disposition D2d).
- **`args_hash` = keyed HMAC over a pinned canonicalization**, not a bare digest of low-entropy
  args: reuse the server-secret HMAC that `wfe_approval` already uses; canonical form = sorted-key
  JSON with a static volatile-field redaction allow-list (drop timestamps/request-ids/tokens);
  version the scheme (`args_hash_v1`). Raw args and any arg-bearing prose never enter the row
  (§Disposition D2f).
- **`mode`/`reason_code` are stable ids** — the `guardrail_mode` enum value and the fixed event key,
  not display strings, so exported trajectories survive policy changes.
- **Replay.** `audit.log` *is* the canonical governed-action ledger (structured JSON, ordered by
  `ts`); `trajectory_export.c` gains an optional reader that interleaves it by timestamp. No new
  DB1 kind.

**Not building:** action *re-execution* replay (inspect-the-sequence is what Sechroom means and what
we already have), a second audit store, or persisted free-form reasoning.

## Non-goals

- **Policy consolidation refactor.** Collapsing aimee's policy (`guardrail_mode` + toolsets +
  gateway flags + wfe gate registration) into one declarative surface is a real refactor for a
  presentation benefit — **out of scope** until a concrete need appears.
- **No new MCP interface / tool-exposure / human-gate mechanism** — all three exist and are
  untouched.

## Phases & sequencing

P1 and P2 are **independent** (§Disposition Seq). Ship **P2 first** (pure-additive passive audit,
lowest risk); a decision write from P1, like any governed write, is then captured by P2 for free —
no hard dependency.

- **P2 — Per-action audit (do first).** Enrich the existing `audit_log()` rows at the
  `pre_tool_check()` block sites + add the allow-path call; define `args_hash_v1` (HMAC +
  canonicalization); teach `trajectory_export.c` to read `audit.log`. **Roll out reader-before-writer:**
  land the enriched-schema tolerance in the export reader first, then enable the enriched emit
  behind a config flag (audit is observational; default-on once the reader ships). Tests: a blocked
  action writes a row with `verdict=block` + `reason_code`; an allow writes `verdict=allow`; the row
  round-trips through the export reader; a simulated `audit_log` failure does **not** change the
  enforcement verdict.
- **P1 — Decision record.** `ALTER decision_log` add the governance columns; register the
  `supersedes`/`linked-policy`/`decided-by` `rel_types` rows; enforce the one-active-per-scope
  invariant in the write gate; extend the recall expiry sweep + curator poll to flip
  `status=revisit_due`. Tests: a decision write with `supersedes_id` deactivates the prior and keeps
  the chain queryable; a second active decision for the same scope is rejected; a decision past
  `revisit_when` surfaces as `revisit_due` on the next recall.

## Roundtable disposition (2026-07-02, 4/7 panelists: minimax, mistral, codex, glm-5.2; not degraded)

Every convergent finding was resolved by grounding on an existing seam:

- **D1a** (decision-as-edge = ontology pollution; prefer a table — mistral, glm, verifier) →
  **fixed**: reuse+extend the existing `decision_log` table (§G2); edges only for graph linkage.
- **D1b** (`revisit_when` via `memory_conflict` is a semantic mismatch / would need a cron = new
  subsystem — glm, verifier) → **fixed**: dropped `memory_conflict`; reuse the existing recall
  expiry sweep + curator poll (§G3).
- **D1-supersede** (active/inactive loses the version chain; need a one-active invariant — minimax,
  verifier) → **fixed**: `supersedes_id`+`status`+`created_at` chain + write-gate invariant.
- **D2a** (audit emit must not flip allow→block; state fail-open — minimax, glm, verifier
  *lynchpin*) → **fixed**: single post-verdict fire-and-forget `audit_log()`; invariant stated.
- **D2b** (persisting free-form `reason` leaks PII — minimax, codex) → **fixed**: persist the stable
  `reason_code` event key only; prose never stored.
- **D2c/D2e** (two sinks fork replay — minimax, codex) → **fixed**: single sink = `audit.log`; no DB1
  write.
- **D2d** (sync DB1 write per tool call = hot-path cost — minimax, glm) → **fixed**: no DB1 write;
  `audit_log()` is already the fire-and-forget path.
- **D2f** (bare `args_hash` is dictionary-attackable — minimax, mistral, codex, glm; 4×) → **fixed**:
  keyed HMAC + pinned canonicalization + redaction allow-list, versioned.
- **D2-policy_applied/mode** (must be stable versioned ids — codex) → **fixed**: enum value + fixed
  event key, not display names. (Renamed the field to `reason_code`.)
- **Seq** (P1/P2 ordering undefined; add DB kind before emitting, reader tolerates unknowns —
  codex, mistral) → **fixed**: declared independent, P2-first, reader-before-writer rollout.
- **Terminology nit** (model wording drifts — codex) → **fixed**: one storage model (table body +
  graph edges) stated once in §Delta 1.

The verification pass **contradicted 14 of the harsher items** as overblown (e.g. "emit might alter
enforcement" — refuted once the emit is fire-and-forget post-verdict); those are not carried.

## Notes

Both deltas are additive and default-safe: Delta 1 adds an opt-in decision shape on an existing
table; Delta 2 enriches an audit row that already fires. Neither changes an enforcement decision —
they make existing decisions *recorded and revisitable*, which is the whole governance value. Ships
slice-by-slice off `origin/testing`; roundtable the code before each PR per standing practice.
