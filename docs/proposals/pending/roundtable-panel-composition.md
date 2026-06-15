# Proposal: Roundtable panel composition — diverse personas, wide fan-out

- **State:** reviewed — READY (review convergence 2026-06-15: self multi-lens
  review [security · architecture · QA · correctness · contrarian] resolved the
  open questions below; a live delegate panel was attempted [architect/mistral,
  security/minimax, qa/mimo] but degraded — mistral hit HTTP 429, the tool-less
  models exhausted turns wandering with file tools enabled. That degradation is
  itself first-hand evidence for the no-tools-review decision in §5.)
- **Author:** JBailes
- **Date:** 2026-06-14 (reviewed 2026-06-15)
- **Motivation:** the roundtable review is the project's quality gate, but today
  it under-uses what aimee already has (personas, multiple delegate models). Make
  the panel genuinely multi-lens and as wide as the available models allow.

## Problems with the panel today

1. **Panelists get no persona.** The engine fans the reference models with
   `agent_run_named(acfg, model, "review", NULL, prompt, …)` (delegate_ensemble.c
   parallel + sequential paths) — the system-prompt/persona arg is `NULL`. Every
   panelist runs the same `review` role with no persona framing and no
   per-participant differentiation.
2. **One lens, even when driven manually.** Running the panel by hand
   (`aimee delegate review --persona reviewer --via <model>`) applies a real
   persona, but the same one to every model → N models, one viewpoint.
3. **`reviewer` is contrarian-only.** The built-in `reviewer` persona is
   literally *"Senior contrarian code reviewer"* (persona.c). A panel of only
   contrarians misses what a constructive/standard code review catches. There is
   no built-in *neutral* reviewer persona (the neutral framing lives in the
   `review` role template, not a persona).
4. **Fixed small panel.** Effectively 3 reference models; no easy way to fan out
   across every capable delegate.
5. **Capability gates exclude models silently.** The `review` role requires
   `caps=tools`; **codex** (gpt-5.5) advertises no `tools` capability, so it
   can't join a review ("no configured model supports caps=tools"). Large review
   prompts (~5k+ tokens) also trip a `min_context` gate on some models
   (observed: mimo-2.5 on a 17 KB two-proposal prompt).

## Design

### 1. Per-participant personas (engine)
The roundtable engine composes a **persona per panelist** (via
`persona_compose_delegate_prompt`, as the manual delegate path already does) and
passes it as the system prompt instead of `NULL`. Default to a **diverse spread**
rather than N copies of one lens.

### 2. A diverse default lineup
Assign distinct lenses across the panel — e.g. **security, architect, QA,
contrarian `reviewer`, and a *standard/neutral* reviewer**. The neutral reviewer
needs either a new built-in persona ("constructive code reviewer") or a shipped
custom persona — pairing it with the contrarian one is the point: one tries to
break it, one assesses it as written.

### 3. Custom personas in the panel
Let the panel draw from **user/custom personas** (`~/.config/aimee/personas/`),
not just built-ins — so a deployment can add domain reviewers (e.g. a "DB schema
reviewer", a "release-safety reviewer") and have them seated automatically.

### 4. Wide fan-out (N participants)
Make the panel size configurable and let it **fan out across every enabled,
capable delegate model** (minimax, mistral, mimo-2.5, codex, … ), not a fixed 3.
`ensemble.reference_models` already lists them; the panel should default to "all
capable" with a cap, and map personas across them (round-robin or pinned).

### 5. Make codex (and tool-less models) usable as reviewers
A document/proposal review needs no file tools. Either:
- give the **review path a no-tools mode** (so a model without a `tools`
  capability — like codex — can still review), and/or
- mark codex tools-capable where appropriate.
Plus raise/relax the `min_context` gate (or chunk the prompt) so large reviews
don't silently drop capable models like mimo-2.5.

## Resolved decisions (review convergence)

These close the open questions and pin down the parts the review found
under-specified. They are the implementation contract.

1. **Persona→participant assignment — operator-configured array, with a stable
   round-robin default.** Add an optional parallel config array
   `ensemble.reference_personas[8][PERSONA_NAME_MAX]` alongside
   `ensemble.reference_models`. If `reference_personas[i]` is set, it pairs with
   `reference_models[i]`. If it is empty/unset, the engine round-robins a built-in
   default lineup (`security, architect, qa, reviewer, reviewer-constructive`)
   across the configured models. **The pairing binds to the model NAME, computed
   before the sequential-mode `shuffle_indices`** — so persona↔model stays stable
   across runs regardless of shuffle order or the per-index temperature jitter
   (reproducible reviews).

2. **The default panel size does NOT change.** Persona assignment *overlays* the
   models already in `reference_models`; it does not grow the panel. "Wide
   fan-out" stays operator-driven — list more models in `reference_models` (the
   `ENSEMBLE_MAX_REFS = 8` hard cap stands; 8 diverse lenses is the documented
   ceiling, not resized in this change). So no existing deployment sees a surprise
   cost increase from this proposal alone; cost stays bounded by the existing
   `ensemble.max_cost_usd` cap, and each panelist's cost is already folded per
   model via `ensemble_fold_cost` — the log line now also carries the persona.

3. **Personas apply to REVIEW mode only.** The engine has three fan-out call
   sites: `run_round_parallel` / `run_round_sequential` (roundtable, both
   `ROUNDTABLE_REVIEW` and `ROUNDTABLE_DRAFT`) and `delegate_ensemble_run` (the
   MoA aggregation). The diverse **reviewer** lineup is assigned **only when
   `mode == ROUNDTABLE_REVIEW`**. Draft mode and the MoA ensemble keep today's
   behavior (no reviewer persona — drafting/aggregating are not reviewing). This
   prevents mis-framing the authoring and synthesis paths.

4. **Neutral reviewer = a new built-in persona** `reviewer-constructive`
   ("Senior constructive code reviewer (assess as written; find what works and
   what's missing)"), `readonly`, roles `review,diagnose,validate,research`.
   Seated next to the contrarian `reviewer`: one tries to break it, one assesses
   it as written. A built-in (not a shipped file) so it is always available with
   no deploy step.

5. **Persona resolution is non-fatal.** `persona_load` already falls back to the
   engineer persona and never hard-fails (it returns 0 with a usable persona), so
   a missing/unreadable custom persona **logs a warning and uses the fallback —
   it never drops a panelist**. `persona_compose_delegate_prompt` returning NULL
   (OOM) also falls back to a NULL system prompt (today's behavior) rather than
   skipping the participant.

6. **The round prompt stays authoritative for output shape.** The persona is the
   panelist's *identity/lens* (system prompt); `build_round_prompt` still carries
   the review-JSON contract (severity/category/location/summary) in the user
   prompt. A test asserts a persona'd review still parses into review items.

7. **codex / tool-less models and the caps gate (clarifies §5).** The engine
   fan-out calls `agent_run_named` (routes by *name* via `agent_find`), which
   **does not pass through the role-based `caps=tools` / `min_context` gate** —
   that gate lives only on the `agent_route_with_caps` path
   (`server_compute.c`, `delegate_routing.c`) used by the *manual* `aimee delegate
   review` route. So a tool-less model like **codex is already invocable as a
   panelist simply by listing it in `reference_models`** — no routing change is
   required for the panel. What the panel *does* need: (a) run reviews with **file
   tools OFF** (a document/diff review needs none — and the live panel above
   wasted its turns and tripped a 429 precisely because tools were on), and (b)
   when a model's context window is smaller than the composed prompt, **skip that
   panelist with a log** rather than fail the whole run. Marking codex
   `tools`-capable in the global routing registry (so the *manual* role-routed
   path accepts it too) is a **follow-up, out of scope here** — it touches model
   routing broadly and risks regressions; this change keeps to panel composition.

8. **claude-via-CLI stays primary-only** — not seated in the auto-panel unless
   `claude_cli_delegate_enabled`. Unchanged.

## Notes
This rides on the existing roundtable/ensemble engine (`delegate_ensemble.c`) and
the persona system (`persona.c`, `persona_compose_delegate_prompt`); it's panel
*composition*, not a new pipeline. Verify each model's `tools`/context
capabilities at panel-build time and skip-with-a-log rather than fail silently.
