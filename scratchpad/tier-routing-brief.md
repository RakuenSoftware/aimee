# Design brief for review: deterministic delegate tier routing (multi-provider)

## Goal

Route delegate work to the cheapest model that will actually complete the packet, across
Anthropic (Fable / Opus / Sonnet / Haiku), Codex (sol / terra / luna), OpenRouter, local
LLMs (llama.cpp / Ollama), MiniMax, and Kimi. Decide whether this needs a learned routing
model or can be fully deterministic.

## Existing machinery in aimee (verified by reading source)

- `src/headers/agent_types.h:237` — each agent carries integer `cost_tier`.
- `src/server/agent_config.c:1695` `agent_route()` — selects the minimum enabled `cost_tier`
  that supports the role and is available for routing; prefers tmux/stateful backends at that
  tier; balances within tier via `agent_pick_balanced()`.
- `src/server/agent_config.c:1802` `agent_route_with_caps_inner()` — same as above but first
  filters candidates by required capability flags and minimum context window. Gated on
  `sys_cfg->model_meta_capability_routing` (default 0, `src/modules/config/config.c:899`).
- `src/modules/delegates/delegate_routing.c:11` `delegate_infer_capability_requirements()` —
  pure deterministic heuristics on prompt text: file extensions -> VISION/PDF/AUDIO,
  `strlen/4` token estimate -> `min_context`. No LLM involved.
- `src/model_registry.c` — capability catalog (`MODEL_CAP_REASONING|TOOLS|VISION|PDF|AUDIO|
  STREAMING`), context windows, `deprecated` flag, models.dev refresh, local overrides.
- `src/server/agent_fallback.c:66` `agent_try_same_tier_fallback()` — on retryable provider
  error or `AGENT_RC_AT_LIMIT`, retries a *same-tier* peer; skips CATALOG_HEALTH_DOWN.
- `src/server/agent_admission.c` — global fail-closed admission (default 14 total concurrent
  agent sessions, 5 per model).
- `src/server/aux_router.c` — separate cheap-model aux path with transient/no-config cooldowns.

Conclusion drawn so far: the *mechanism* for cheapest-capable routing already exists. Adding
new providers is largely a registration + `cost_tier` assignment + capability-catalog problem,
not new routing code.

## The gap (CORRECTED after round 1 — the original framing was wrong)

Round 1 of review raised a blocking correctness finding against this section, and it is
confirmed against source. The original brief assumed delegates default to an expensive tier
that an allowlist could downgrade. That is false.

`agent_route()` (`src/server/agent_config.c:1705-1719`) computes `min_tier` across all enabled
role-supporting available agents and routes there. `agent_route_with_caps_inner()` does the
same after capability filtering (`:1825`). Lower `cost_tier` wins. New agents default to tier 1
(`src/cmd_agent_setup.c:179`); the built-in mistral-plan normalizes to tier 0
(`src/server/agent_config.c:196-202`).

**Aimee already routes cheapest-first.** There is no downgrade to perform. The allowlist-
downgrade design of round 1 is therefore vacuous — it proposes an operation the router has
already been performing unconditionally since before this brief.

The actual gap is the opposite one. The ONLY quality gates today are `agent_supports_role()`,
the capability flags, and the context-window minimum. There is no way to say "this packet class
must not be served below tier N." Cheapness is unbounded from below: register a cheap local
model, mark it as supporting `engineer`, and every engineer packet silently routes to it.

## Reframed proposal: a per-class tier FLOOR, not a downgrade allowlist

The missing primitive is a floor — a declared minimum capability tier per (role x packet
class) — plus honest `cost_tier` assignment for the expanded provider set.

Consequences of the reframing:

- The feature is quality protection, not cost saving. It makes some packets MORE expensive
  than today's unbounded cheapest-first, never less.
- The economizer-safety-spec conflict largely dissolves. A floor cannot be a cost intervention
  in the spec's sense: it never lowers spend, so it can never fail the spec's "must be strictly
  lower" inequality — that inequality only binds interventions that claim savings. This needs
  confirmation from the panel, but the sign of the effect is now clearly safe.
- The multi-provider work (Fable/Opus/Sonnet/Haiku, Codex sol/terra/luna, OpenRouter, local,
  MiniMax, Kimi) becomes primarily a *registration + honest tier assignment + capability
  catalog* problem. Registering cheap providers WITHOUT floors is actively dangerous under
  cheapest-first: it silently redirects existing work to the new cheap model.
- Questions 1 and 2 below are largely mooted: there is no difficulty prediction to make and no
  downgrade decision to earn. A floor is an operator declaration, not an inference.

## Proposed direction (the thing under review)

### 1. No learned routing model in v1

Rationale to be challenged:
- A classifier accurate enough to separate "Haiku-able" from "needs Opus" must do much of the
  reading the task itself requires; on short packets the router costs more than it saves.
- No labeled corpus exists mapping (task -> cheapest model that succeeded). The deterministic
  escalating router must ship first to generate that data. A learned model is a later phase.
- Every other admission/routing decision in the repo is deterministic and auditable. A
  probabilistic router makes "why did this go to Haiku?" unanswerable.

### 2. Default high, downgrade only on a proven-safe allowlist. NO speculative escalation.

An escalation ladder (try cheap -> try mid -> try expensive) is explicitly REJECTED as the
design. Its worst case costs strictly more than dispatching the capable model once, it burns
tokens on attempts known to be discarded, and its expected value depends on a cheap-tier
success rate nobody has measured. It is not under consideration; do not propose it back.

The design instead defaults to the CURRENT routing behavior (the role's established tier) and
downgrades only when the packet matches a narrow, enumerated, deterministically-recognized
task class that has been demonstrated safe at the lower tier. Unmatched packets are not
downgraded. The failure mode of a wrong decision is therefore "we did not save money on this
packet", never "we spent double".

Properties this buys:
- Cost is monotonically non-increasing versus today. No packet gets more expensive.
- One dispatch per packet. No discarded attempts, no retry amplification.
- The decision is a table lookup, auditable and explainable after the fact.

Retry/fallback on PROVIDER FAILURE (existing `agent_try_same_tier_fallback`, health, cooldown,
`AGENT_RC_AT_LIMIT`) is unchanged and out of scope — that is failure handling, not tier
climbing, and it stays same-tier.

Open question for the panel: how does a task class earn its way onto the allowlist? Options
include operator declaration only, or an offline (non-production) evaluation harness run
against recorded packets, or standing benchmarks. Whatever the mechanism, it must not consume
production tokens on speculative attempts.

### 3. Deterministic downgrade-eligibility inputs (all already available, none requiring inference)

- role (`review` / `engineer` / `qa` / `security` / `architect`) — already the routing key
- write-capable vs read-only (`delegate_role_is_write()`)
- prompt token estimate (existing `strlen/4` in `delegate_infer_capability_requirements`)
- blast radius / files touched (existing `aimee-blast-radius`)
- required capability flags (existing catalog)

A static per-role x per-blast-radius table of DOWNGRADE-ELIGIBLE classes. Default: no
downgrade. Conservative by construction — a class must be enumerated to qualify.

Candidate starting allowlist (to be challenged): mechanical//bounded packets such as
single-file formatting or lint fixes, commit-message and changelog drafting, docstring or
comment generation over an already-written diff, structured extraction/summarization with a
fixed output schema, and mechanical rename/import rewrites where the acceptance check is
exact. Explicitly NOT eligible: anything write-capable across multiple files, security or
architecture roles, and any packet whose blast radius or context exceeds a declared bound.

### 4. Cross-provider tier assignment

Requires a normalized `cost_tier` across heterogeneous providers whose prices, context windows,
tool-calling fidelity, and rate limits differ by orders of magnitude, including local models
whose marginal token cost is ~0 but whose latency and quality are worse.

## Known constraint that must be reconciled

`docs/proposals/pending/provider-neutral-economizer-safety-spec.md` is APPROVED and normative.
Its baseline explicitly pins "the same model, endpoint, account/project routing, service tier,
region, and API shape", and its governing rule is:

> Wherever Aimee intervenes, the user's charge and the authoritative provider cost must both be
> strictly lower than they would have been without the economizer. If that cannot be proven for
> the individual request and its complete task lifecycle, Aimee does not intervene.

Note this is a large part of WHY the escalation ladder is rejected: a failed cheap attempt plus
a retry at a higher tier costs strictly MORE than dispatching the higher tier once, so it could
never satisfy the spec's inequality. The allowlist-downgrade design at least has the right
sign — one dispatch, at a tier less than or equal to today's.

But "cheaper per dispatch" is still not the same as the spec's demand for a proven strictly
lower cost for the COMPLETE TASK: if a downgraded packet produces work a human or a later
delegate must redo, the task-level cost rises even though the single request was cheaper. The
proposed reconciliation is to ship this as explicit user/operator-selected routing policy
(declared allowlist, opt-in, off by default) rather than as an Aimee-initiated cost
intervention. The panel should judge whether that framing genuinely sits outside the spec's
scope or is a relabeling of the exact intervention the spec forbids.

## Questions for the panel

1. Is the "no learned model in v1" conclusion correct, or is there a cheap deterministic-enough
   difficulty signal (or a tiny local classifier) that changes the calculus?
2. The escalation ladder is rejected (section 2) — do not argue for it. Instead: is the
   allowlist-downgrade design sound, and what is the right mechanism for a task class to earn
   allowlist membership WITHOUT spending production tokens on speculative attempts? What is
   the smallest allowlist that is worth shipping at all — i.e. is the achievable saving large
   enough to justify any new routing surface, or is the honest answer "not worth it yet"?
3. Is the economizer-safety-spec reconciliation sound, or does routing-as-user-policy still
   violate the approved gate? Is a spec amendment required before any of this can ship?
4. How should `cost_tier` be normalized across Anthropic / Codex / OpenRouter / local /
   MiniMax / Kimi when price, quality, latency and rate limits do not co-vary? Is a single
   integer tier the wrong abstraction?
5. What breaks in the existing router if the agent population grows to ~15-20 agents across
   6+ providers — admission control, health/cooldown, `agent_pick_balanced` fairness,
   fallback-chain interaction, capability-catalog coverage for non-models.dev providers?
6. What is the smallest shippable first slice, and what are its acceptance criteria?
