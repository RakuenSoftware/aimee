# Implementation plan: roundtable panel composition

Companion to `roundtable-panel-composition.md` (State: READY). This plan is the
concrete change list. Scope = panel *composition* on the existing engine; no new
pipeline, no routing-layer rework. `ENSEMBLE_MAX_REFS = 8` is unchanged.

## Summary of the change

Make each roundtable **review** panelist run under a distinct **persona** (its
system prompt), defaulting to a diverse lineup, drawn from the persona registry
(built-in + custom), assignable per-model via config — without changing the
default panel size or the draft/aggregate paths.

## Work packets

### WP-1 — New built-in persona `reviewer-constructive`
The constructive counterpart to the contrarian `reviewer`. All additive switch
cases; existing enum values are untouched.

- `src/headers/prompts.h`: append `AIMEE_MODE_REVIEWER_CONSTRUCTIVE = 7` to
  `aimee_mode_t` (after `AIMEE_MODE_ARCHITECT = 6`).
- `src/prompts.c`:
  - name→mode resolver (~line 505): map `"reviewer-constructive"` →
    `AIMEE_MODE_REVIEWER_CONSTRUCTIVE`.
  - `prompt_principles_text` (~line 532): add the new mode to the reviewer-family
    case (shares the reviewer principles).
  - `prompt_persona_text` (~line 550): add a case returning new prose — a
    *constructive* reviewer identity: "You are a senior constructive code
    reviewer. Assess the change as written: confirm what is correct and complete,
    name what is missing or risky, and judge whether it meets its stated goal.
    You are the counterpart to the contrarian reviewer — not adversarial, but
    not a rubber stamp."
  - reviewer-family predicate (~line 651): add the new mode alongside QA /
    SECURITY / REVIEWER / ARCHITECT.
- `src/config_mode.c` (~line 25): add the `"reviewer-constructive"` name mapping
  if it has its own table (mirror prompts.c).
- `src/persona.c`:
  - `g_builtins[]` (~line 95): add
    `{"reviewer-constructive", AIMEE_MODE_REVIEWER_CONSTRUCTIVE, "Senior
    constructive code reviewer (assess as written)", "review,diagnose,validate,research",
    "", "", "readonly"}`.
  - `builtin_brief` (~line 161): return a CONSTRUCTIVE_REVIEWER_BRIEF for the new
    mode (mirror REVIEWER_BRIEF, framed as assess-as-written).

### WP-2 — Config: per-participant persona array
- `src/headers/config.h` (near `ensemble_reference_models`, ~line 1291): add
  `char ensemble_reference_personas[8][PERSONA_NAME_MAX];` and
  `int ensemble_reference_persona_count;`. (`PERSONA_NAME_MAX` from persona.h —
  include it or use a local constant equal to it.)
- `src/config_sections.c` `config_parse_ensemble_section` (~line 1162): parse a
  `reference_personas` string array exactly like `reference_models` (same `< 8`
  guard).
- `src/config_save.c` (~line 64–79): round-trip `reference_personas` when present.

### WP-3 — Engine: compose & assign personas (the core)
File: `src/server/delegate_ensemble.c` (add `#include "persona.h"`).

- New static helper:
  `static const char *panel_persona_name(const config_t *cfg, roundtable_mode_t
  mode, int model_index)`:
  - returns `NULL` unless `mode == ROUNDTABLE_REVIEW` (draft/MoA unchanged);
  - if `cfg->ensemble_reference_personas[model_index][0]` set → return it;
  - else round-robin a file-static default lineup
    `{"security","architect","qa","reviewer","reviewer-constructive"}` indexed by
    `model_index % 5`. **Keyed on `model_index` (the participant's position in the
    configured `reference_models` list), NOT the shuffled slot** — so the pairing
    is stable run-to-run.
- New static helper to compose+own the system prompt:
  `persona_compose_delegate_prompt(name, NULL, NULL)` → heap string; NULL name →
  NULL prompt (today's behavior). On compose failure, log + use NULL (non-fatal).
- `run_round_parallel` (~line 1043): add `char *personas[ENSEMBLE_MAX_REFS]`
  (memset NULL); for each `i`, compose `panel_persona_name(cfg, mode, i)` into
  `personas[i]` and set `tasks[i].system_prompt = personas[i]`. Free all
  `personas[i]` in the success path AND the `fail:` cleanup (mirror `prompts[i]`).
- `run_round_sequential` (~line 1084): `int i = order[oi]` is the stable model
  index → compose `panel_persona_name(cfg, mode, i)`, pass as the 4th arg to
  `agent_run_named` instead of `NULL`, free after the call.
- `delegate_ensemble_run` (~line 1125, MoA aggregation) and `repair_review_json`
  (~line 1008, JSON repair): leave `system_prompt = NULL` — not review lenses.
- Logging: include the persona name in the per-panelist log/label so the panel
  composition is visible (and cost stays attributed per model via the existing
  `ensemble_fold_cost`).

### WP-4 — No-tools review + small-context skip (safety)
- Verify the engine fan-out (`agent_run_named` / `agent_run_parallel`) does NOT
  enable file tools for review panelists (a doc/diff review needs none; the live
  panel proved tools-on wastes turns + trips rate limits). If tools are on by
  default for these agents, gate them off for the ensemble review path only.
  *If already tools-off, this WP is a no-op + a test asserting it.*
- When a panelist model's context window is smaller than the composed prompt,
  skip that panelist with a log rather than failing the whole round (the engine
  already tolerates partial failure via `participants_failed`; ensure a
  context-too-small model is counted as a skipped participant, not a hard error).
- Out of scope (follow-up): marking codex `tools`-capable in the global routing
  registry so the *manual* role-routed `aimee delegate review` path accepts it.

## Tests (`src/tests/`)
- `panel_persona_name`: default round-robin is stable by model index; a
  configured `reference_personas[i]` overrides; non-review mode returns NULL.
- A persona'd review still parses into review items (persona = identity only; the
  round prompt still drives the JSON output shape).
- A missing/unknown custom persona falls back (engineer) and does NOT drop the
  panelist (`persona_load` already guarantees a usable persona).
- New built-in resolves: `persona_load(..., "reviewer-constructive", ...)`
  returns its constructive prose, distinct from `reviewer`.
- Config round-trip: `reference_personas` parses and saves.

## Build / verify
- `make -j1` (parallel LTO flakes on this host), then `make test` for the
  affected suites (test_persona, test_config, the ensemble/roundtable test if
  present). `aimee git verify` before push.

## Risk notes
- Lowest-risk additive change: new enum value appended, new switch cases, new
  optional config field, NULL→persona at three review call sites only.
- The MoA and draft paths are explicitly excluded, so authoring/synthesis
  behavior is byte-unchanged.
- Default panel size unchanged → no surprise cost for existing deployments.
