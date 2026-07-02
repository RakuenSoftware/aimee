# Implementation Plan: governance decision records + per-action policy-verdict audit

Companion to `governance-decision-records-and-action-audit.md` (merged to `testing`, PR #945).
Grounded against the code 2026-07-02. **P2 (audit) ships before P1 (decision record)** per the
proposal's reader-before-writer, lowest-risk-first sequencing.

## Verification substrate (confirmed working)

- **Inner loop — local unit tests.** `cc`/`gcc`/`make` + `cmake`/`ctest` present; `build/` exists;
  `aimee-core` static lib + test targets build and run in seconds (verified: `test_util` passes).
  35 ctest targets. This is the per-slice fast loop.
- **Production build** — `make -C src ../aimee-server` (the CMake `aimee-server` target is
  known-stale; do not rely on it — [[aimee-two-build-systems]]).
- **Integration loop — PVE `root@192.168.1.253`.** CT 101 `aimee-docker` running; spin a fresh CT
  for any slice that needs a live DB2/postgres + running server (S2 audit-file assertion, S4/S5/S6
  decision_log DDL + recall). Watch the docker-lxc Env/PATH bug ([[lxc2docker-image-env-path-bug]]):
  pass `-e PATH=...` when building in a non-login shell.
- **CI gates** — no co-author trailers, no Claude attribution ([[aimee-ci-no-coauthor-trailers]],
  [[no-claude-attribution-in-prs]]); schema/docs-gen/lint/Windows gates trip on C/DB changes
  ([[aimee-pr-ci-gates]]).

## Per-slice protocol

Each slice: branch off `origin/testing` → implement → local ctest (+ .253 integration if it touches
DB/server/audit-file) → **roundtable review the CODE** ([[always-roundtable-review-before-pr]]) →
address findings → PR → merge to `testing`. Slices are independently shippable.

## Grounded seams (exact)

- `audit_log(event_type, fmt, …)` (`src/log.c:143`) writes `{"ts","event","detail"}` JSON to a
  0600 rotated `audit.log`. **Add a sibling `audit_action_log(...)`** writing the enriched governed-
  action row to the **same** `audit_fp` (single sink, same mutex/rotation) — existing callers
  untouched.
- `pre_tool_check()` (`src/guardrails_orchestrator.c:1196`) — block sites already call
  `audit_log("<stable_key>", …)` (`read_before_write`:1783, `truncating_write`:1813,
  `stale_edit`:1845, `subagent_blocked`:1615, `antipattern_blocked`:1673); allow terminal is
  `return 0` (:1999). Verdict contract `0/1/2` (`src/headers/guardrails.h:134`).
- `hmac_sha256()` + `wfe_sha256_raw()` (`src/workflow/wfe_approval.c:90`) — reuse for `args_hash`.
- `decision_log` table (`src/db2/schema.sql:29`) + full API `db2_decision_log_insert/get/
  set_outcome/list` + `db2_decision_log_row_t` (`src/db2/decision_log.h`) + client wrappers
  (`kb_client_decision_log_*`). Extend additively.
- `rel_types` table (`schema.sql:1067`) + `db2_rel_types_stage_provisional()`
  (`src/db2/rel_types_store.c:132`) — runtime `INSERT`, no migration.
- Recall expiry sweep — `memory_directive_sweep_expired()` in `recall_fill_reminders()`
  (`src/memory_context.c:920`); client wrapper `kb_client_memory_directive_sweep_expired_json()`.
  Periodic drain — `kb_curator_drain.c` `drain_thread_main` (`DRAIN_POLL_SECS=5`).

---

## P2 — Per-action audit (ship first)

### S1 — `args_hash_v1` (pure helper + unit tests)
- New `src/audit_action.{c,h}`: `audit_args_hash(const char *tool, const char *args_json, char out[…])`
  → `"v1:<hex>"`. Keyed HMAC-SHA256 (reuse `wfe_sha256_raw`) over a **canonical projection**:
  sorted-key JSON with a static volatile-field redaction allow-list (drop `timestamp`/`ts`/
  `request_id`/`session_id`/ephemeral tokens). Version-pinned.
- **Key source** — see Q1 (roundtable). Default assumption pending answer: a dedicated
  `$AIMEE_HOME/.audit-key` (same 0600 provisioning as `.approval-key`), NOT the approval key, to
  avoid coupling the audit digest to the approval trust root.
- Tests: determinism; key-sensitivity (different key → different hash); volatile-field redaction;
  key-order independence; empty/oversized args bounded.
- Pure, no DB/server → **local ctest only**.

### S2 — structured governed-action audit row + wire into `pre_tool_check`
- `audit_action_log(actor, tool, args_hash, mode, reason_code, verdict, detail)` in `log.c` →
  `{"ts","event":"tool_action","actor","tool","args_hash","mode","reason_code","verdict","detail"}`
  to the same `audit.log`.
- Wire: at each block site pass the existing stable key as `reason_code`, `verdict="block"` (or
  `"approval_required"` where the prose prefix already distinguishes it); add ONE call on the allow
  path (`return 0`) with `verdict="allow"`. `mode` = `guardrail_mode` enum value (stable id).
  `reason_code`/prose split: **only the stable key is persisted**, never `msg_buf` prose.
- **Fail-open invariant**: `audit_action_log` runs post-verdict, side-effect-only; a write/format
  failure never mutates the returned `0/1/2`.
- Rollout: emit behind a config flag; default-on only after S3's reader ships (reader-before-writer).
- Tests: local — block→`verdict=block`+`reason_code`; allow→`verdict=allow`; a forced audit-emit
  failure leaves the verdict unchanged. **.253 integration** — assert the JSON line lands in
  `audit.log` under a live server driving a blocked Write.

### S3 — `trajectory_export` reads `audit.log`
- Extend `src/trajectory_export.c` with an optional reader parsing `audit.log` JSON lines,
  interleaving `event:"tool_action"` rows by `ts` into the exported trajectory. Config-gated;
  tolerate legacy/unknown lines (`{ts,event,detail}` and future kinds) — reader ships before the
  writer defaults on.
- Tests: a `tool_action` row round-trips into the export; a legacy `audit_log` line is ignored
  without error.

---

## P1 — Decision record

### S4 — `decision_log` governance columns + `rel_types`
- Additive `ALTER TABLE decision_log ADD COLUMN IF NOT EXISTS` (match the repo's migration
  convention — confirm whether `schema.sql` is apply-once `CREATE IF NOT EXISTS` + a migrations
  path, or re-run idempotent): `status TEXT DEFAULT 'active'`, `revisit_when TEXT`,
  `supersedes_id BIGINT`, `author TEXT`, `linked_policy_id BIGINT`. Extend `db2_decision_log_row_t`
  + `insert`/`get`/`list` + client wrappers.
- Register `rel_types` rows `supersedes`, `linked-policy`, `decided-by` via
  `db2_rel_types_stage_provisional()` (runtime INSERT, no migration).
- Tests: new columns round-trip through insert/get; rel_types rows present after registration.

### S5 — decision write path + one-active-per-scope invariant
- Write sets `status='active'`; when `supersedes_id` is set, flip the prior row to `'superseded'`
  in the same transaction. `supersedes_id`+`status`+`created_at` give the version chain.
- **Invariant**: at most one `active` decision per *scope* — see Q2 (roundtable) for the scope key.
  Enforce via a partial unique index and/or the `memory_fact_gate` verdict path.
- Tests: supersede deactivates prior + chain queryable; a second `active` for the same scope is
  rejected.

### S6 — `revisit_when` surfacing (no new scheduler)
- Lazy-at-recall: extend the `memory_directive_sweep_expired()` path so a decision past
  `revisit_when` flips to `status='revisit_due'` and surfaces on recall.
- Periodic: add one per-poll scan block in `kb_curator_drain` for due decisions.
- Tests: a decision past `revisit_when` surfaces as `revisit_due` on the next recall; the periodic
  scan flips it without a recall.

---

## Decisions — RESOLVED by roundtable (2026-07-02, 5/7 healthy, verified=7, not degraded)

1. **`args_hash` key source → dedicated `$AIMEE_HOME/.audit-key`.** Isolates the audit tamper-
   evidence trust root from the `wfe_approval` authorization root (different threat model, different
   rotation). Mirror `wfe_approval_ensure_key()` (atomic `O_EXCL` + `/dev/urandom`, 0600).
   **Fail-closed at startup** (provision the key when the server starts); **fail-open at hash time**
   (if the key is somehow absent, skip the audit row — never HMAC-over-empty "integrity theater",
   and never block the tool).
2. **Decision "scope" → `(subject, linked_policy_id)`** with an **explicit new `subject` column**
   (do NOT overload `task_id`). NULL `linked_policy_id` handled by `COALESCE(linked_policy_id,-1)`
   in the partial unique index (Postgres treats NULLs as distinct, which would silently void the
   invariant for policy-less decisions).
3. **`audit_action_log` → new sibling function** to the same `audit.log` (existing `audit_log`
   callers untouched). Confirmed.
4. **Default-on timing → flag-gated until S3 merges, then a dedicated flip slice (S7).** Confirmed.

## Roundtable disposition — revisions folded into the slices above

- **Migration (was BLOCKING) → resolved.** `schema.sql` is re-run idempotently with inline
  `ALTER TABLE … ADD COLUMN IF NOT EXISTS` (precedent: `memories` at `schema.sql:224`, `terms` at
  `:21`). S4 adds ALTERs to `schema.sql`; **no migrations dir, no separate migration file.**
- **S1 redaction → INVERT to a per-tool allowlist** (denylist-by-omission would leak future
  PII/secret fields into a tamper-evident append-only log). `args_hash` HMACs a per-tool allowlist
  of decision-relevant fields (`file_path`/`command`/`old_string`/`new_string`/`url`…); an
  **unknown tool hashes tool-name only** (never its values). Contract documented in the header.
- **S1 bounds** — hard cap 64 KiB / bounded depth+key-count; oversize → HMAC a truncation marker +
  bounded prefix (stable, verifiable). Reuse cJSON for parse; deterministic whitespace-free
  sorted-key serializer as a small audited helper with golden vectors. Version tag `v1-`.
- **S2 exactly-once emit → structural, not convention.** Refactor: rename the body to
  `pre_tool_check_inner()` returning the verdict + a small `{verdict, reason_code}` out-struct set at
  each block/allow site; a thin outer `pre_tool_check()` calls inner, emits **one**
  `audit_action_log()` post-verdict, returns unchanged. Kills the "many `return 0` sites" bug the
  verifier flagged. `guardrail_mode` is already a `pre_tool_check` param → in scope at the wrapper.
- **S2/S3 shape discriminator → new top-level `"kind":"tool_action"`** (do NOT overload `event`);
  legacy `audit_log` rows keep `event`. The audit row is **stand-alone — no FK to `decision_log`**
  (so P2 ships before P1). Reader discriminates on `kind`.
- **Config plumbing → owned by S1.** Define `audit.action_enabled` (default false) + one accessor;
  S2 (writer) and S3 (reader) both read it. No independent config reach-ins.
- **S3 rotation/ties** — read current + rotated logs for the window; deterministic tie-break by
  (file order, byte offset). Rate-limited warn on unparseable lines (don't silently swallow).
- **S6 → dedicated idempotent `decision_log_mark_revisit_due(now)`** owning the transition; called
  by BOTH the recall site and the drain (do not overload `memory_directive_sweep_expired`, which is
  named for a different row type). Index `revisit_when` (avoid O(N) scans every 5 s).
- **S7 (NEW) — default-on flip.** After S3 merges: flip `audit.action_enabled` default to true;
  verify on a live `.253`/`.254` server that `trajectory_export` consumes `tool_action` rows.
  Prevents a silently-written, never-read dead feature.

## Slice ledger
- [ ] S1 args_hash_v1 + config knob · [ ] S2 inner/outer refactor + action-audit row · [ ] S3 export reader
- [ ] S4 decision_log cols (+subject) + rel_types · [ ] S5 write path + partial-unique invariant · [ ] S6 revisit sweep API · [ ] S7 default-on flip + live verify
