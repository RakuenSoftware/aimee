# Implementation plan — replayable-evidence verification (Part A)

Plan for [replayable-verification-and-deepening-sweep.md](replayable-verification-and-deepening-sweep.md),
**scoped to Part A only** per the proposal's Phasing decision (ship A standalone,
exercise its evidence vocabulary on real runs, then plan Part B separately).
Grounded in `origin/testing`. Default-safe: the replay/verify pass is gated and,
when off, the roundtable behaves exactly as today.

**Plan-gate:** roundtabled (engineer + security lenses → both APPROVE-WITH-CHANGES).
The four security blockers — verifier tolerance (B1), `area_root` provenance (B2),
model-prose-out-of-artifact (B3), 2000-line-cap factoring (B4) — and the concerns
(verifier-role, post-replay dedup, idkey normalization, vacuous-query, fixed
rejected array, upward tier table, ast-grep validation, independent gate, audit
sink) are folded into the work packets below.

## Key existing primitives (reuse, don't rebuild)

| Primitive | Where | Use |
|-----------|-------|-----|
| `delegate_roundtable_run()` | `src/server/delegate_ensemble.c:1494` | Orchestrator — the verify pass slots in **after** item capture, **before** `assemble_review_artifact`. |
| `capture_round_review_items()` / `capture_review_items_from_text()` | `delegate_ensemble.c:944` / `:880` | Produce `roundtable_review_item_t[]` from panelist JSON — the input to the verify pass. |
| `review_items_same()` / `review_item_add_source()` | `delegate_ensemble.c:843` / `:850` | Existing dedup; extend to key on the deterministic `identity_key` from the reduced record. |
| `assemble_review_artifact()` | `delegate_ensemble.c:1446` | Final artifact; add the **rejected appendix**. |
| `panel_persona_prompt()` | `delegate_ensemble.c:1154` | Where the panelist brief is built — add the "emit a structured evidence query" instruction. |
| `roundtable_review_item_t` | `src/headers/delegate_ensemble.h:71` | Fixed-width item; gains a bounded evidence sub-struct (WP-A1). |
| Read-only code-intel surfaces | `find_symbol`, `lsp_references`, `ast_grep_search`, `search_graph` | The **only** execution path for replay (security model). |
| `run_aggregator()` / `parse_model_json_lenient()` | `delegate_ensemble.c:490` / `:78` | Pattern for a delegate sub-call + lenient JSON parse; the verifier reuses this shape. |

**File-size constraint:** `delegate_ensemble.c` is **1917/2000 lines**. The replay
engine and verifier go in **new files**, not appended here:
- `src/server/evidence_replay.{c,h}` — the non-model structured-query → reduced-record engine.
- `src/server/roundtable_verify.{c,h}` — the verifier pass (rubric + verdict).

## Design refinement settled by grounding

The proposal hedged the evidence field between "inline" and "4 KiB side buffer".
Because we **locked replay to structured-query-only**, the evidence is *bounded*,
not a free-form command — so it is a **fixed inline sub-struct**, no heap, no
lifetime management in `delegate_roundtable_result_free`:

```c
typedef enum { EV_NONE=0, EV_SYMBOL, EV_REFS, EV_SEARCH } ev_kind_t;
typedef struct {
   ev_kind_t kind;
   char target[256];        /* symbol (EV_SYMBOL/EV_REFS) or search query (EV_SEARCH) */
   char project[128];       /* index project scope ("" = all indexed projects) */
   int  expected_count;     /* the count the panelist claims */
   char expected_idkey[65]; /* sha256-hex[:64] of sorted "file:line", or "" */
   int  factual;            /* 1 = replayable claim, 0 = interpretive (caps at concern) */
} review_evidence_t;
```
Added to `roundtable_review_item_t`: `~460 B` × 128 items ≈ `+60 KB` to
`roundtable_result_t` — well within the 32 MB compute-pool worker stack.

## Implementation grounding (verified against the code)

- **Replay source is the deterministic db2 code-index, not tool-output strings
  and not `search_graph`.** `search_graph` (mcp_tools.c) is the *memory/episode*
  graph, wrong for code. The code surfaces are pure C, DB-backed, return
  structured hits with `file_path`+`line`:
  - `EV_SYMBOL` → `index_find()` / `db2_code_index_term_find()` → `term_hit_t`.
  - `EV_REFS` → `index_find_callers()` / `db2_code_index_callers_find(project,
    symbol, …)` → `caller_hit_t` (the workhorse for "N call sites" claims; the
    `code_calls` table records caller/callee/line).
  - `EV_SEARCH` → `index_code_search()` → `code_search_hit_t`.
  - (`ast_grep` shells out to a binary → **deferred from v1**; `blast_radius` is
    a Part-B deletion-test source, not Part A.)
- **`area_root` containment is by construction (strengthens B2).** The index only
  holds files from scanned projects, so replay physically cannot reference an
  unindexed path (no `/etc/passwd`). Plus the `project` scope on each query.
  Reduced-record paths are still validated as project-relative for the idkey.
- **Degrade vs reject (critical robustness rule).** Distinguish
  `REPLAY_INDEX_UNAVAILABLE` (project unknown / 0 indexed files) → **degrade: keep
  the item, mark `unverified:index-unavailable`, do NOT reject** — from
  `REPLAY_CONTRADICTED` (index present, claim doesn't reproduce) → reject. Without
  this, enabling the gate on an unindexed server would drop every review item.
- **sha256 for idkey** → OpenSSL `EVP_sha256` (already used across the tree:
  forge_app_token.c, gateway/platform_webhook.c, kb/auth_oidc.c).
- **Config flag** → `config.h` field + one `config_fields.c` row
  `{"roundtable.replay_verify_enabled", offsetof(...), sizeof(int), 1, CFG_BOOL}`
  (default-on). No `config_sections.c` change needed (mirrors
  `claude_cli_delegate_enabled`).
- **Build wiring** → add `server/evidence_replay.c` + `server/roundtable_verify.c`
  to the SRCS lists in `src/Makefile` (~L347/374-378, explicit lists, not glob);
  new unit tests go in `src/tests/Rules.mk` (`TEST_TARGETS` + per-target objs).

## Work packets

### WP-A1 — Evidence sub-struct + struct plumbing
- Add `review_evidence_t evidence;` to `roundtable_review_item_t`
  (`delegate_ensemble.h`); update the `_Static_assert`/array-dim comments.
- Audit every site that `memcpy`/`memcmp`/serializes the struct —
  `git grep -n 'roundtable_review_item_t' src` — explicitly including
  **`src/server/server_compute.c`** (the other `delegate_roundtable_run` caller)
  for any wire/JSON (de)serialization, plus `review_items_same`, dedup, capture,
  and artifact. Bump each to carry the evidence sub-struct.
- No behavior change yet — field is populated in WP-A3, ignored when the gate is off.

### WP-A2 — Structured-query replay engine (`evidence_replay.{c,h}`) — security core
- `int evidence_replay(const review_evidence_t *ev, reduced_record_t *out)`.
- **`area_root` is authoritative, not a parameter (B2).** It is the server's
  project root resolved **once at config load** and held immutable for the run;
  `evidence_replay` reads it from that single source. There is no caller-supplied
  root to spoof — a path that resolves outside it is rejected at the engine, not
  trusted to the glob.
- Dispatch `ev->kind` to the matching read-only surface **only**; never a shell,
  never arbitrary argv, never `make`/`curl`. Unknown/`EV_NONE`/malformed → return
  `REPLAY_UNVERIFIABLE` (the item is dropped, not retried).
- **Vacuous-query predicate (C7):** all of `target`/`pattern`/`glob` empty after
  trim, or `expected_count <= 0` for a count claim → `UNVERIFIABLE` with a
  distinct `reason=vacuous` (so a jailbroken panelist can't smuggle a no-op query).
- Constrain `glob` to resolve under `area_root` (reject `..`, absolute escapes,
  **and symlinks/hardlinks leaving the root** — resolve, then re-check) before any
  surface call.
- **`EV_PATTERN` validation (C5):** reject unbalanced/over-nested ast-grep
  patterns and **time-box** every surface call; a pathological pattern returns
  `UNVERIFIABLE` within the deadline, never hangs the pass.
- Reduce output to `reduced_record_t { int count; char idkey[65]; }`.
  **idkey normalization (C4):** `idkey = sha256_hex( join("\n",
  sort_ascending( "<file>:<line>" )) )[:64]`, where `file` = path **relative to
  `area_root`, normalized, symlinks resolved**, `line` = decimal — so equivalent
  evidence in different path forms collides correctly.
- The full surface output is written to an **append-only audit sink with a
  per-item size cap (C1)** — a fixed path that **no prompt-construction code path
  ever reads back**; never returned to a model.
- **Dispatch model (C11):** per-item replay is **serial within the verify pass**
  for v1 (bounded, deterministic, simplest to reason about); a bounded-parallel
  variant is a later optimization, not v1.
- Pure/deterministic and unit-testable with a fake surface backend (no network).

### WP-A3 — Verifier pass (`roundtable_verify.{c,h}`)
- `int roundtable_verify_items(agent_config_t*, const config_t*, roundtable_result_t*, ...)`:
  for each captured item — call `evidence_replay`; `UNVERIFIABLE` → move the item
  to the **rejected list**.
- **Tolerance contract (B1):** severity is always derived from the **reproduced
  `actual`, never the panelist's claim**. A Part-A factual trigger is **binary** —
  the cited symbol/line either reproduces or it doesn't (no fuzzy band). Where a
  claim carries a count (relevant to Part B, not this slice), the verifier
  **re-grounds to `actual`** and logs `claimed→actual` drift in the audit sink;
  the tier follows `actual`, so inflating the claim buys nothing.
- **Rejected list is a fixed array (B4/C10/mistral#2):** add
  `rejected[ROUNDTABLE_MAX_REVIEW_ITEMS]` (each: claim summary + query + reduced
  record + reason code + stable id) to `roundtable_result_t`, mirroring
  `items[]`. **No heap, no new free-path** in `delegate_roundtable_result_free`.
- **Dedup runs post-replay (C3):** `reduced.idkey` only exists after
  `evidence_replay`, so dedup happens over **survivors**, keyed on `idkey`
  (replacing the model-derived key in `review_items_same`). Rejected items don't
  participate.
- For survivors, run **one fresh delegate call** (reuse the `run_aggregator`
  shape) by a **dedicated `verifier` persona — NOT one of the panel models (C2)** —
  that receives **only** the claims + reduced records (never raw output, never the
  proposers' reasoning). Its role is recorded in the audit sink.
- **Verdict schema carries NO model prose (B3):** the verifier returns
  `{ severity, idkey[], line_range[] }` only. The human-facing rationale is
  **code-templated from the reduced record**, never from model text — so a
  jailbroken verifier can neither escalate the enum (capped by WP-A4) nor smuggle
  a scary narrative into the artifact. (The panelist's own finding summary, captured
  pre-verification, still rides through as the finding text.)
- Apply the rubric mechanically in code (WP-A4) over the verdict — the model
  proposes, the code decides.
- Wire into `delegate_roundtable_run` between capture and `assemble_review_artifact`.

### WP-A4 — Part-A rubric + verdict logic
- Encode the severity rubric as a pure function `severity_t apply_rubric(claimed,
  reduced, factual, verdict)`: `blocker` requires a reproduced factual trigger;
  `factual==0` items **cap at `concern`**; `nit` is the floor. Verifier may
  **downgrade** (claim exceeds reproduced support) **and promote** (a reproduced
  fact meets a higher tier) — but **never escalate on interpretation alone**.
- **Explicit upward tier table (C9)** — which reproduced trigger earns which tier,
  so promotion isn't left to the model: reproduced UB / memory-safety / auth-bypass
  / data-loss → `blocker`; reproduced correctness defect without UB → `concern`;
  reproduced style/preference → `nit`. The trigger class comes from the structured
  verdict (an enum), not prose. Unit-test **each cell**.
- Fully unit-tested incl. the reproduced-blocker-rated-nit (promote) case, the
  interpretive-only-capped case, the off-by-one-in-a-test-fixture (stays `concern`,
  doesn't clamp-promote) case, and the claimed-100/actual-93 re-grounding case.

### WP-A5 — Panelist brief + config gate + artifact
- `panel_persona_prompt` (review mode): instruct panelists to emit a
  `review_evidence_t`-shaped query for every item and to mark factual vs
  interpretive. An item with no query is treated as interpretive (caps at concern)
  and is not replayed.
- **Config gate (C6):** `roundtable_replay_verify_enabled` — a bool in
  config_fields/config_sections read **independently** of any panel-mode/caller
  intent flag, validated at the public entry point; both the replay flag and
  review mode must hold. mtime-reloaded, matching existing ensemble config. Off →
  today's exact behavior (no replay, no rejected appendix). Default-on for review
  mode is a config default, not derived from caller-controlled state.
- **Keep `delegate_ensemble.c` under the 2000-line cap (B4):** the rejected
  appendix + the `verified=N rejected=M capped=K` counts are rendered by a helper
  **in `roundtable_verify.c`** — `char *roundtable_render_rejected(const
  roundtable_result_t *)`. `assemble_review_artifact` gains only a single
  `str_append(artifact, rejected_md)` + free. Net add to `delegate_ensemble.c` is
  the verify-call wiring + the brief text — budgeted to land it ≤ ~1980 with
  margin; if it doesn't, the brief text moves to `roundtable_verify.c` too.

### WP-A6 — Tests
- `test_evidence_replay`: each `ev_kind_t` against a fake surface; `..`-escape
  **and symlink-leaving-root** rejection; `area_root` is authoritative — a benign
  glob is still rejected if the resolved path leaves the stored root (B2);
  vacuous-query → `UNVERIFIABLE reason=vacuous` (C7); pathological `EV_PATTERN`
  returns `UNVERIFIABLE` within the deadline (C5); **idkey determinism across
  path-form variations** (C4); UNVERIFIABLE on malformed.
- `test_roundtable_verify`: `actual`≠`claimed` re-grounds, tier follows `actual`
  (B1); reproduced trigger promotes a nit to blocker; interpretive-only capped at
  concern; off-by-one-in-test-fixture stays concern (C9); rejected `rejected[]`
  array populated; **verdict-jailbreak**: a model verdict cannot escalate the enum
  past the rubric **and** its prose never reaches the artifact — byte-grep the
  model payload, byte-grep the artifact, assert no overlap beyond the idkey ref
  (B3); dedup runs over survivors only (C3).
- **Audit-sink isolation (C1):** assert the audit path appears in **no**
  prompt-construction function (prophylactic grep test).
- Extend `test_wfe_roundtable` / existing ensemble tests for the gate-on path;
  gate-off path is **byte-identical to today for both the artifact and the panel
  prompt bytes** (C8).
- Respect the per-file test line cap; pre-split `test_roundtable_verify` into
  `_{core,rubric,artifact}.c` if it would exceed the cap (mistral#5).

## Sequencing & ownership
- WP-A1 → A2 → A3/A4 (A4 is a pure lib A3 calls) → A5 → A6. A2 and A4 are pure
  libraries, parallelizable across delegates once A1 lands.
- Each WP is a vertical slice with its own tests; conventional commits staging
  only changed files.

## Risk / rollback
- Entirely behind `roundtable_replay_verify_enabled`; default-on for review mode
  but a single flag flip restores prior behavior. No schema/DB change in Part A
  (the `typed_facts` `architecture_settled` relation belongs to Part B).
- Biggest risk is the panelist-query quality (garbage queries → mass rejection);
  mitigated by the interpretive-cap fallback (an item with no/!factual query
  still lands as a `concern`, it is not lost) and surfaced via the
  `rejected/capped` counts so a bad panel is visible, not silent.

## Out of this plan (Part B — separate follow-up plan)
The deepening sweep, scope/areas, `typed_facts.architecture_settled`, worktree +
committer, cost caps, and work-item filing are **not** in Part A. They get their
own plan once Part A's evidence vocabulary is exercised on real roundtables.
