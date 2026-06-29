# Implementation plan: Deterministic context folding

Companion to `deterministic-context-folding.md` (PR #881). This plan grounds each
proposal phase in the actual `testing`-branch code, names the seams, the config
wiring, the tests, and the per-slice PR/roundtable gate. **Nothing here changes the
design** — it makes it buildable and flags the one scoping decision the roundtable
must rule on before P2.

- **State:** DRAFT — awaiting roundtable approval of this plan.
- **Author:** JBailes
- **Date:** 2026-06-29
- **Base:** every slice branches off `origin/testing` (the session checkout is far
  behind and shared by other sessions — worktrees only, per house rule).

## §A Codebase anchors (verified against `testing`)

- **Tool-result compaction.** `compact_tool_result(raw, raw_len, cfg, tool_name,
  context_used, context_budget)` (`src/compact.c`, hdr `src/headers/compact.h:46`).
  Strategies: pass-through, `compact_json_summary` (≈`compact.c:120`), head/tail
  `compact_plaintext` (≈`compact.c:146`); strategy selection ≈`compact.c:239`.
  Invoked via `agent_compress_tool_result()` (`src/server/agent_policy.c:1085`) from
  the bash tool (`src/posix/agent_tools.c:981`) and the generic dispatcher
  (`src/posix/agent_tools_dispatch.c:1614`).
- **Cache-prefix machinery.** `payload_rewrite_prefix_hash(system_prompt,
  static_context, out, len)` — FNV-1a-64 over **system_prompt + static_context only,
  no messages** (`src/payload_rewrite.c:57`). `payload_rewrite_should_defer` /
  `payload_rewrite_track_request` (`payload_rewrite.c:77`, `:?`). DB1 state
  `payload_rewrite_state_t` (`src/db1/payload_rewrite_state.h:7`). Config family
  `cache_aware_rewrite_*` (master gate, hard-context-threshold 0.85, max-defer-turns
  20, segment-check-turns 5, min-savings-tokens 500).
- **Two payload pipelines (the key fact):**
  - *Delegate path* — `agent_execute_with_tools_internal()`
    (`src/posix/agent_runtime.c`): `maybe_compact_before_request()` (`:676`) →
    **[clean fold seam]** → `track_anthropic_payload_rewrite()` (`:698`) → request
    build (`:693`–`706`). **payload_rewrite runs here.**
  - *Ingress proxy path* — `src/server/anthropic_http.c`:
    `messages_run_request_pipeline()` (`:361`) → `translate_request()` (`:369`) →
    `build_provider_body()` (`:254`). **payload_rewrite does NOT run here.**
- **Model registry.** `model_context_window(model_id)` prefix-match table
  (`src/model_registry.c:202`); `model_capability_t` (`src/headers/model_registry.h:41`)
  with `context_window`, `max_output`, costs, flags. A fold-budget resolver layers on
  top — no schema change.
- **Memory kinds.** `KIND_*` string defines + `KIND_COUNT` (`src/headers/aimee.h`);
  `memory_units` table in **DB2** with `is_episode_card` flag
  (`src/db2/schema_sqlite.sql:77`, `src/db2/schema.sql`). Episode cards:
  `memory_episode_card_generate` / `memory_episode_cards_query`
  (`src/memory_episodes.c:114`,`:239`).
- **Plan/snapshot storage (DB1).** `checkpoints(snapshot TEXT)`,
  `file_snapshots`, `file_snapshot_entries`, plus `execution_plans` / `plan_steps`
  (`src/db1/schema.sql`).

## §B Decision required before P2 — which pipeline does the fold target?

§3 fold-freeze "builds on `payload_rewrite`", but `payload_rewrite` runs **only on
the delegate path**, not the ingress proxy path that external clients (e.g. a
Claude-Code-style transcript) flow through. Options:

- **B-1 (recommended): delegate path first.** Land §1/§3 where the cache machinery
  already exists (`agent_runtime.c` seam). This is exactly aimee's own long
  autonomous agent loops — the highest-value, lowest-risk target. Ingress-path
  integration becomes an explicit later slice (needs `payload_rewrite` threaded into
  `build_provider_body`, signature change).
- **B-2: ingress path first.** Larger blast radius (touches the stateless proxy +
  golden envelope), and there is no prefix-state owner there yet — we would build
  one. Higher value for external clients, higher risk.
- **B-3: both at once.** Largest single change; rejected — violates the slice
  discipline.

This plan assumes **B-1** unless the roundtable rules otherwise. The §2 Coordinate
Closet (P1) is pipeline-independent and proceeds regardless.

## §C Implementation slices

Each slice is one PR off `origin/testing`, **default-off**, with a code roundtable
before merge (per house rule), the local CI recipe in §D green, and golden/schema
gates regenerated where touched.

### P1 — Coordinate Closet (§2) — *pipeline-independent, lands first*

- **New:** `src/coord_closet.c` + `src/headers/coord_closet.h`.
- **API (deterministic, no model/clock/rand):**
  - `coord_closet_nominate(const char *raw, size_t len, coord_lane_t lane, coord_set_t *out)`
    — extract uuids, ≥7-hex shas, abs/repo paths, digit-bearing `key=value`,
    issue/PR refs, `handle:`/`memory:` tokens.
  - `coord_closet_render(const coord_set_t *set, size_t budget, dstr *out, coord_evict_t *why)`
    — emit `Coordinate Closet (conserved from folded turns): …`; canonical NFC +
    JSON-canonicalization; total order `(lane, label, first-occurrence offset)`,
    tie-broken by `(turn_id, tool_call_id, result_index)`.
  - No-silent-loss: if a nominated coordinate would not fit, return
    `COORD_EVICT_FAIL` so the caller sizes-up / spills (P4) / refuses to fold —
    never drops silently.
- **Provenance/injection guard:** `COORD_LANE_USER` entries render untrusted, never
  mint an agent-trusted label nor auto-promote; secret/PII filter hook before
  persist/recall.
- **Wiring (minimal in P1):** call `coord_closet_nominate` inside
  `compact_tool_result` *before* strategy selection (≈`compact.c:239`) so identifiers
  are captured before truncation; surface the conserved block through the existing
  compaction return. No transcript surgery yet.
- **Config:** `coord_closet_enabled` (bool, default off), `coord_closet_budget_tokens`
  (int). Wired across the 5 config files (§D).
- **Tests:** `src/tests/test_coord_closet.c` — nomination coverage, byte-identical
  render under shuffled input + repeated runs (determinism gate), eviction→FAIL,
  user-lane quarantine. Register in `src/tests/Rules.mk`.
- **Accept:** 0 lost coordinates on a fixture corpus; deterministic bytes; lint +
  unit-tests green.

### P1.5 — Pipeline-order spike + per-model budget resolver (§7) — *prereq for P2*

- **New:** `src/fold_budget.c` + `src/headers/fold_budget.h`:
  `fold_budget_resolve(const char *model_id, const config_t *cfg, fold_budget_t *out)`
  — pure function of `(model, config)`; fields: `context_window`,
  `retained_band_tokens`, `tail_cap_tokens`, `pressure_ceiling_tokens`,
  `prefix_saturation_tokens`, `closet_budget_tokens`, eviction policy. Seeds from
  `model_capability_get` + a per-model override table; explicit fallback for unknown
  models.
- **Determinism rule:** any per-tool override (`memory_context_budget_*`, `compact_*`)
  that affects a fold is frozen for the freeze TTL or folded into the fold-input hash.
- **Spike deliverable:** a short design note (committed in the PR) pinning the §3
  five-stage pipeline order and the exact `agent_runtime.c` seam (per §B-1), plus the
  `payload_rewrite` span-registry refactor sketch (below) — reviewed before P2 code.
- **Config:** `fold_budget_*` overrides table (string/int). **Tests:**
  `test_fold_budget.c` — known + unknown models, override-freeze determinism.

### P2 — Rolling fold (§1) + fold-freeze (§3) — *core economic win*

- **New:** `src/context_fold.c` + `src/headers/context_fold.h`:
  `context_fold_view(cJSON *messages, const char *sys, const fold_budget_t *b,
  const config_t *cfg, fold_result_t *out)` — produces the synthetic skeleton
  assistant/user pair + closet (reusing P1), **non-destructive** (never mutates
  `messages`).
- **Atomic tool pairs:** a `tool_use`+`tool_result` fold as one unit or not at all;
  folded regions render as plain non-tool text. Multi/parallel/failed-call turns
  collapse whole.
- **Seam:** insert between `maybe_compact_before_request()` (`agent_runtime.c:677`)
  and `track_anthropic_payload_rewrite()` (`:698`) — sees assembled history, runs
  before the cache hash, before request build (per §B-1).
- **Freeze + single owner:** refactor `fnv1a_update` to public; add a span registry
  to `payload_rewrite.c` (`payload_rewrite_register_span(name, ptr, len)`); the fold
  registers its frozen span and the prefix is hashed **once** by `payload_rewrite`.
  No second writer. Config: `fold_freeze_enabled`, `fold_freeze_ttl_ms`,
  `fold_tail_cap_tokens`.
- **Tests + golden:** `test_context_fold.c` (skeletonization, atomic-pair invariant,
  byte-identical freeze across turns, provider-shape rendering for Anthropic/OpenAI/
  Gemini). Regenerate `test_mcp_tools_golden.inc` if the envelope shape shifts
  (`DUMP_TOOLS=1`, §D).
- **Accept:** measured provider cache-read share ↑ on a captured long session
  (validated on the pve testbed, §E); end-to-end token cost ≤ truncation baseline;
  determinism + provider-validity gates green.

### P3 — Register grammar (§6)

- **New:** `src/fold_register.c` + hdr: `fold_register_parse(turn) -> register_t`
  (in-progress/executing/verdict/hazard/blocked). Soft: untagged ⇒ in-progress; fold
  stays correct without it.
- **Wire:** retained-band selection in `context_fold.c` prefers verdict/hazard;
  episode-seal harvest (P5) gates on settled registers. Config:
  `fold_register_enabled`. **Tests:** `test_fold_register.c`.

### P4 — Fold recall (§4)

- **New:** `src/fold_recall.c` + hdr: in-memory page table mapping folded content →
  path/handle/id; trigger on re-touch/citation appends a bounded recall card to the
  tail (re-folds next epoch); residency TTL anti-thrash. Reuses existing resolvers
  `code_span_read()` (`src/headers/code_span.h`) and `memory_get`. Also backs the §2
  coordinate spill tier. Config: `fold_recall_enabled`, residency TTL.
  **Tests:** `test_fold_recall.c`.

### P5 — Sealed episodes (§5) + task rail (§8)

- **Episodes:** add `KIND_EPISODE_SEAL "episode_seal"` (`aimee.h`, bump
  `KIND_COUNT`); DB2 helper mirroring `db2_memory_unit_episode_card_insert` (+
  `is_episode_seal` flag or distinct `unit_type`); file-inventory index; auto-recall
  on file re-touch. Schema change ⇒ update **both** `db2/schema.sql` and
  `schema_sqlite.sql` and pass `make schema-sync-check`.
- **Task rail:** serialize plan FSM to DB1 `checkpoints.snapshot` (or reuse
  `execution_plans`/`plan_steps`); `serialize`/`restore`; hard-epoch wake seed
  derived from rail+closet+seals. Config: `fold_episodes_enabled`,
  `task_rail_enabled`. **Tests:** `test_episode_seal.c`, `test_task_rail.c`.

## §D Cross-cutting recipes

- **Config flag (5 files, lockstep):** declare in `config_t` (`src/headers/config.h`);
  default + parse in `src/config.c`; allowlist row in `src/config_fields.c`
  (`{name, offsetof, sizeof, 0, CFG_BOOL|CFG_INT|...}`); schema row in
  `src/config_schema.inc` (`{name, SCHEMA_BOOL|SCHEMA_INT, 0}`); save in
  `src/config_save.c` (top-level bool/int auto-handled). `make lint` checks
  config-field consistency.
- **Unit test:** add `src/tests/test_<name>.c`; add target to `TEST_TARGETS` and a
  build rule in `src/tests/Rules.mk` (copy `test_compact` pattern); run
  `cd src && make -j$(nproc) unit-tests`.
- **Golden:** regenerate with
  `DUMP_TOOLS=1 ./build/obj/tests/unit-test-mcp-client-registry` and paste the block
  into `src/tests/test_mcp_tools_golden.inc`, updating `MCP_TOOLS_GOLDEN_COUNT`.
- **Local CI gate (run before every PR):**
  `cd src && make lint && make -j$(nproc) all server && make build-integrity &&
  make -j$(nproc) unit-tests && make schema-sync-check`.
- **Commit hygiene:** no `Co-Authored-By` / Claude-attribution trailers (repo gate).

## §E Test environment (pve)

For P2+ behavioral/cache validation we need a live aimee server with an LLM backend.
Plan: provision a throwaway CT on pve (`root@192.168.1.253`), deploy the
`testing`+slice build, replay a captured long session through the delegate path, and
read measured provider cache-read tokens + net token cost. **Tear the CT down when
the slice is validated.** Unit-level determinism/atomic-pair/closet gates run locally
and need no server.

## §F Open items for the roundtable

1. **§B pipeline decision** (delegate-first vs ingress-first vs both) — the one
   blocking ruling needed before P2.
2. **Closet wiring depth in P1** — surface the conserved block only through
   `compact_tool_result`'s return now, or also persist it for cross-turn reuse in
   P1? (Leaning: return-only in P1, persistence in P2 with the fold.)
3. **Episode-seal storage** — new `is_episode_seal` column vs a distinct `unit_type`
   value on the existing `memory_units` row (avoids a schema migration).
4. **Convergence with `ingress-compression-and-cache-alignment`** — one PR series or
   two; who owns the span registry.
5. **Default-off → default-on** criteria per phase (off-reasons discipline), and
   which phases ever flip on by default.
