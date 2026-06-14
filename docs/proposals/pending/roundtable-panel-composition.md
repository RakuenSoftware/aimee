# Proposal: Roundtable panel composition — diverse personas, wide fan-out

- **State:** draft, pending review
- **Author:** JBailes
- **Date:** 2026-06-14
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

## Open questions
- Neutral-reviewer persona: new built-in vs shipped custom persona?
- Persona→model assignment: pinned (codex→contrarian, claude→architect, …) vs
  round-robin vs operator-configured?
- Panel-size cap and cost: wide fan-out multiplies token spend — gate by
  `ensemble.max_cost_usd` (now optional/uncapped by default) and surface the
  per-panelist cost.
- claude-via-CLI is primary-only by default (not delegate-eligible) — leave out
  of the auto-panel unless `claude_cli_delegate_enabled`.

## Notes
This rides on the existing roundtable/ensemble engine (`delegate_ensemble.c`) and
the persona system (`persona.c`, `persona_compose_delegate_prompt`); it's panel
*composition*, not a new pipeline. Verify each model's `tools`/context
capabilities at panel-build time and skip-with-a-log rather than fail silently.
