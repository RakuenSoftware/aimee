# Response and orchestration stages — modules for governance, delegates, workflows

- **State:** PENDING — proposal for the next round of pluggable-stage seams; Slice 7
  (`gw_stage_registry.c`, MERGED) closed only the REQUEST-mutation side. This document
  proposes the RESPONSE-STAGE seam and the ORCHESTRATION-HOOK seam; design points flagged
  OPEN below are for the roundtable.
- **Author:** JBailes
- **Date:** 2026-07-17

## Problem

Slice 7 modularized the REQUEST side. `gw_stage_registry.c` defines `gw_request_stage_fn`,
`gw_stage_registry_build` produces an ordered, config-driven catalog (memory, tool_policing,
router, model_pin), and `gw_pipeline_run_request` walks that catalog over the raw request
cJSON. The user goal — move MEMORY, GOVERNANCE, DELEGATES, WORKFLOWS into individually
add/removable modules to shrink core — is ~1/4 done. Memory is a request stage. The rest
have **no seam**:

- **GOVERNANCE (response side).** `gateway_policy_police_parsed_response(parsed_response_t*)`
  is called **inline at 4+ sites**: `anthropic_http.c:629`, `:1058`, `:1089`, and
  `openai_chat.c:1258`. Pure duplication, no registry, not togglable. Action-side
  governance (blast_radius, TDD, action_audit) is a separate concern — it intercepts tool
  **actions**, not requests/responses, and is out of scope here.
- **DELEGATES + WORKFLOWS.** Orchestration-shaped (spawn sub-agents / advance a workflow).
  They do **not** fit `gw_request_stage_fn` (which mutates the outbound request and
  returns an intervention count) — they need a different hook shape that can observe/act
  on a turn, not transform the request payload.

The result: response-side governance is glued to four call sites, and delegates/workflows
have nowhere to register. Both must be seams before they can be modules.

## Target

Two new seams, mirroring the proven Slice-7 pattern (explicit ordered catalog,
config-gated enable/disable, deterministic order, fail-closed on a bad catalog):

1. **RESPONSE-STAGE registry** — a response-transform seam over the parsed reply.
   - First consumer: consolidate the 4+ inline `gateway_policy_police_parsed_response`
     calls into **one** registered `"governance"` stage, togglable, run by a
     response-pipeline runner. This deletes the duplication AND makes response-side
     governance a real module.
   - Initial type: `parsed_response_t*` (reuse `anthropic_response_from_parsed` /
     `emit_message_as_sse` / police unchanged). An IR-native `aimee_response_t*`
     signature is a sequenced follow-up per the canonical-IR proposal (Slice 5 proper
     + `turn_record_v1` sanctioned follow-up).
2. **ORCHESTRATION hook seam** for delegates + workflows.
   - A **distinct** interface (NOT a response transform) — an ordered set of hooks that
     can observe/act on a turn (spawn a delegate, dispatch to a workflow) and be
     enabled/disabled via config. Delegates and workflows register here, **not** in the
     request/response stage registries.

The response stage is a *transform over the reply* (mutates `parsed_response_t`,
returns an intervention count — symmetric with `gw_request_stage_fn`). The orchestration
hook is an *action on the turn* (may spawn, may short-circuit, may re-enter). Keeping
these two interfaces separate is the whole point.

## Open design points for the roundtable

- **Response-stage signature.** `int (*)(parsed_response_t*, void*)` returning
  intervention count, symmetric with `gw_request_stage_fn`? Or IR-native
  (`aimee_response_t*`)? Brief's recommendation: `parsed_response_t` **now** (reuse
  emit/police), IR-native later — but the table rules. The interface choice is
  reversible until Slice 2 wires the first consumer.
- **Where the response pipeline runs, and the 4→1 collapse.** Relative to emit
  (`anthropic_response_from_parsed` / `emit_message_as_sse`) — must run **before** the
  client-shape render so governance sees the canonical reply, not the rendered SSE.
  The four sites (`anthropic_http.c:629`, `:1058`, `:1089`; `openai_chat.c:1258`) collapse
  to one runner call per egress site, with the site supplying the `parsed_response_t*`
  it already builds.
- **Orchestration hook shape.** Is a stage-shaped interface even right for
  delegates/workflows? They likely need a richer context (turn state, the ability to
  short-circuit / re-enter) than a `gw_request_stage_fn` allows. **OPEN.** Goal: the
  minimal hook that captures both without over-abstracting. Candidate: a turn-scoped
  context (turn id, request ir, response ir-or-NULL, continuation token) + a return
  that can request "continue / short-circuit / re-enter after N".
- **Ordering + failure semantics.** Per the Slice-7 ruling (fail-closed on a bad
  catalog). Confirm whether response stages adopt the same default and whether the
  orchestration seam needs different semantics (an orchestration hook that crashes may
  block a delegate spawn — likely still fail-closed).
- **Config surface.** Env toggles in the style of `AIMEE_STAGE_MEMORY`, or the
  config-store? Per-stage names + duplicate/unknown validation (re-use the Slice-7
  catalog validator). **OPEN.**

## Slices (each: pure core + tests first, then wire, roundtable before PR — mirror Slice 7)

1. **Response-stage registry + runner.** Mirror `gw_stage_registry` /
   `gw_pipeline_run_request` for the response side: `gw_response_stage_fn`, ordered
   catalog built by name, `gw_pipeline_run_response(parsed_response_t*, void* ctx)`,
   config-gated enable/disable, duplicate/unknown validation, fail-closed on bad
   catalog. Pure + unit-tested before any wire.
2. **Port response governance.** One `"governance"` response stage wrapping
   `gateway_policy_police_parsed_response`; replace the 4+ inline call sites with a
   single runner call per egress site; config toggle (`AIMEE_STAGE_GOVERNANCE` or
   equivalent); prove through the pipeline that enabled polices and disabled does
   not — same shape as the memory toggle test.
3. **Orchestration hook seam — SEPARATE, larger.** Design + registry for
   delegates/workflows as its own sub-track once 1–2 land. Scope: interface +
   registry + a clear first-port plan; full ports are sequenced follow-ups (mirrors
   how the canonical-IR proposal sequences `turn_record_v1` as a sanctioned
   follow-up, not a loose end).

## Acceptance

- `gateway_policy_police_parsed_response` is invoked via a registered, togglable
  response stage through a response-pipeline runner, **not** inline at 4+ sites
  (`anthropic_http.c:629`, `:1058`, `:1089`; `openai_chat.c:1258`).
- Disabling the governance stage via config demonstrably **skips** policing (tested
  through the runner); enabling **runs** it — same shape as the memory toggle.
- The delegates/workflows orchestration seam is **DESIGNED** (interface + registry)
  with a clear first-port plan; full ports are sequenced follow-ups.
- Response-stage ordering is deterministic; bad catalogs are fail-closed (per the
  Slice-7 ruling).

## Risks / open questions

- **Interface drift.** If the response-stage signature moves from `parsed_response_t*`
  to `aimee_response_t*` (per the canonical-IR proposal's follow-ups), every registered
  stage needs re-wiring. Sequencing Slice 1–2 on `parsed_response_t` keeps the first
  port honest; the IR-native variant is a separate slice behind its own enablement
  criteria (shadow parity, per Slice 5 of the canonical-IR proposal).
- **Orchestration seam scope creep.** Delegates and workflows are not symmetric — a
  delegate spawn may be fire-and-forget while a workflow advance may need to re-enter
  the turn. The minimal hook that captures both without over-abstracting is the
  highest-risk design call here. Lean toward the smaller interface and let the
  follow-up slices prove whether it was enough.
- **Action-side governance is out of scope.** blast_radius / TDD / action_audit
  intercept tool **actions**, not requests/responses. They are a different seam and a
  separate proposal; conflating them with the response-stage seam would muddy both
  interfaces.
```yaml acceptance
- {id: 1, tier: mechanical, check: "make unit-tests TEST=test_gw_response_registry"}
- {id: 2, tier: mechanical, check: "make unit-tests TEST=test_response_governance_stage"}
- {id: 3, tier: integration, check: "grep -c gateway_policy_police_parsed_response src/server/anthropic_http.c src/server/openai_chat.c"}
```
