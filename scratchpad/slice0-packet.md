# Packet: Slice 0 — capability-catalog identity correctness

## Problem (all verified against source + the live server, do not re-litigate)

Live agents (`aimee agent list --json`): `claude`/`claude-opus-4-8` tier 1; `codex`/`gpt-5.6-sol`,
`MiniMax-M3`, `kimi-k2.7-code` all tier 0. MiniMax and kimi are registered with
`provider: "anthropic"` because they speak the Anthropic WIRE FORMAT, but models.dev keys them
by VENDOR (`minimax`, `moonshotai`). `model_capability_get()` (`src/model_registry.c:852`)
matches provider with `strcasecmp` on every lookup path, so all capability lookups for those two
agents miss the catalog and fall through to `model_capability_get_heuristic()`
(`src/model_registry.c:251`), which takes the **anthropic** branch.

Resulting live defects:

1. Both agents resolve to `TOOLS|STREAMING` only — no `MODEL_CAP_REASONING` — although
   models.dev reports both as reasoning models.
2. Because of (1), `src/server/agent_config.c:688` gives both the short
   `AGENT_DEFAULT_TIMEOUT_MS` instead of `AGENT_REASONING_TIMEOUT_MS`. The comment at that site
   names the symptom: slow completions cut off and retried as spurious read failures.
3. `MiniMax-M3` resolves context window **200000** via the stale prefix entry
   `{"minimax", 200000}` (`src/model_registry.c:196`). True value per models.dev is
   **1000000**.
4. `kimi-k2.7-code` resolves context window **0** (no kimi/moonshot entry in `g_ctx_windows`).
   True value is **262144**.
5. The `min_context` gate is **fail-open on zero**:
   `if (min_context > 0 && effective_ctx > 0 && effective_ctx < min_context) return 0;`
   (`src/server/agent_config.c:1789`, mirrored in
   `src/modules/delegates/delegate_routing.c` `agent_meets_filter`). A zero/unknown context
   window skips the check entirely.

Root cause for 1-4 is a too-narrow guard on an EXISTING normalization at
`src/server/agent_config.c:664-666`:

```c
if (strcmp(ag->provider, "openai") == 0 &&
    (strstr(ag->endpoint, "api.minimax.") || strstr(ag->model, "MiniMax-M2")))
   snprintf(ag->provider, sizeof(ag->provider), "%s", "minimax");
```

It fires only for `provider == "openai"` (live agents are `"anthropic"`) and hardcodes
`MiniMax-M2` (live model is `MiniMax-M3`). No moonshotai/kimi equivalent exists.

## Required changes

1. **Widen the vendor normalization** so an Anthropic-wire third-party endpoint resolves to its
   catalog vendor. Must cover at least `api.minimax.` -> `minimax` and `api.kimi.com` /
   `moonshot` -> the models.dev key that actually resolves (`moonshotai`; verify against the
   cached catalog, `kimi-for-coding` also exists — pick the one that resolves
   `kimi-k2.7-code`). Match on endpoint host and/or model-id family, NOT on a single hardcoded
   model version — `MiniMax-M2` must not be the discriminator, or M3/M4 break again.
   `agent_t.provider` is `char[16]` (`src/headers/agent_types.h:238`) — check bounds.

   CRITICAL — this decision is already made, do not re-open it. You MUST introduce a SEPARATE
   catalog/vendor identity field and use it ONLY at capability-lookup sites. Do NOT remap
   `ag->provider` in place. Verified reason: `provider == "anthropic"` drives three live
   behaviors that would silently break:
     - `agent_config.c:411` injects `anthropic-version: 2023-06-01`
     - `agent_config.c:2183` coerces `auth_type` to `x-api-key`
     - `agent_config.c:171` (`agent_provider_env_vars`) selects `anthropic_env_vars`
   The existing `openai` -> `minimax` remap at `:664-666` is only safe because `openai` has
   none of those branches. The request builder, wire shape, auth, and headers for these agents
   must remain byte-identical.

   Suggested shape: a `catalog_provider[16]` (or similar) field on `agent_t`, populated at
   config load from endpoint host / model family, defaulting to `provider` when unset, and read
   by the capability-lookup call sites listed in item 1b below.

1b. **Capability-lookup call sites** that must use the new vendor identity rather than
   `ag->provider`:
     - `agent_config.c:688` (reasoning-based timeout derivation)
     - `agent_config.c:738` (known-model derivation)
     - `agent_config.c:1779` (`agent_satisfies_required_caps`)
     - `delegate_routing.c` `agent_meets_filter` (`model_capability_get(ag->provider, ...)`)
   Grep for `model_capability_get(` and audit every call that passes an agent's provider.

2. **Close the zero-context fail-open** at `agent_config.c:1789` and the mirrored check in
   `delegate_routing.c`. An effective context window of 0 must NOT satisfy a positive
   `min_context`. Keep the per-agent `middleware.context_window` override as the operator
   escape hatch.

3. **Fix the stale context table** in `src/model_registry.c`: the `{"minimax", 200000}` prefix
   fallback silently under-reports MiniMax-M3 by 5x. Prefer catalog resolution; if the prefix
   fallback stays, it must not shadow a correct catalog value.

## Out of scope — do NOT do these

- Do NOT change `cost_tier` values or add tier/price reconciliation (separate packet).
- Do NOT enable `model_meta_capability_routing` (it stays default 0 in this packet).
- Do NOT add a competence/quality axis.
- Do NOT change routing policy or `agent_route()` semantics in any way.

## Acceptance criteria

- `model_capability_get()` resolves `MiniMax-M3` -> context 1000000 with `REASONING|TOOLS`,
  and `kimi-k2.7-code` -> context 262144 with `REASONING|TOOLS`, for agents registered with the
  live config's shape (Anthropic wire, third-party endpoint).
- Both agents consequently receive `AGENT_REASONING_TIMEOUT_MS` at `agent_config.c:688`.
- Anthropic-wire request construction for those agents is unchanged: same headers (including
  `anthropic-version`), auth, and body. Demonstrate this, don't assert it.
- A test asserts effective context window 0 does not pass a positive `min_context` gate.
- Tests added to `src/tests/test_agent_caps.c` and/or `src/tests/test_model_registry*` covering
  each of the above; existing tests still pass.
- With `model_meta_capability_routing` off (the default), routing behavior is unchanged.

## Verification required before you report done

Run the focused production build and the relevant test targets. Report exact commands and
output. If you cannot run something, say "validation-pending" explicitly and name what is
unverified — do not claim done on an unrun gate.
