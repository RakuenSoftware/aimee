# Design decisions needed: provider-general agent registration (aimee)

## Operator requirement

"Registering codex should just be registering codex, with you being able to select
the appropriate model." And: "for anything that has specific models to pick (i.e.
roundtables), we need the provider AND the model displayed, e.g. GPT-Codex-Sol."
Roundtables for codex must offer Sol, Terra and Luna as three distinct seats.

## Current state (verified in source)

- `agent_t` holds ONE `model`. Dispatch reads `agent->model` (agent_runtime.c:1105,1174).
- `agent_route*()` returns an `agent_t *`. Selection minimises `ag->cost_tier`.
- `provider_catalog_get_health(peer->name)` keys health by AGENT NAME.
- `concurrency_per_model` (admission) already keys by MODEL string.
- `agent_try_same_tier_fallback()` iterates peers at equal `ag->cost_tier`.
- `agent_result_t` already carries distinct `model`, `served_model`, `requested_model`.
- `model_capability_resolve_ref()` already parses `provider:model` refs and backs
  `aimee model show [provider:]<model>`.
- Provider registry exists: `model_provider_t` with `fetch_models`, and
  `server_provider_models_cached()` (DB1-cached, 1h TTL). `fetch_models` returns
  model IDs ONLY - price/context/caps come from the models.dev catalog.
- Capability routing now defaults ON; the gate fails upward to the most capable seat.
- Per-model price bands exist (`model_price_band_t`), as do per-agent price overrides.

## Decisions required

1. REPRESENTATION. Should a registered provider be:
   (a) ONE agent_t carrying a model SET, with routing returning (agent, model); or
   (b) N synthetic agent_t entries materialised at config load, one per model?
   (b) requires no change to routing, admission, health or fallback - all of which
   are per-agent today - but multiplies MAX_AGENTS (currently 16) and makes
   `--via <name>` ambiguous. (a) is architecturally cleaner but touches every
   routing signature. Which, and why?

2. COST_TIER. Under (a) tier cannot stay an agent property: sol/terra/luna are
   $5/$2.50/$1 input. Should tier be derived per-model from catalog price, stay an
   operator-declared per-model override, or be replaced entirely by price ordering
   now that resolved prices exist?

3. HEALTH. A failing sol must not mark terra and luna down. provider_catalog keys
   by agent name. Should health become keyed by (provider, model), and what happens
   to existing recorded health on upgrade?

4. FALLBACK. `agent_try_same_tier_fallback` means "another agent at the same tier".
   Under provider registration, what is the correct peer set - same-tier models
   within the provider, across providers, or both, and in what order?

5. ROUTABLE SET. Which of a provider's catalogued models become routable?
   `moonshotai` lists 10, several superseded. Options: all non-deprecated catalog
   entries; an operator allowlist; or the profile's existing `fallback_models`.
   Note model_capability_t already carries a `deprecated` flag that
   agent_satisfies_required_caps() already rejects on.

6. IDENTITY. Adopt `provider:model` (model_capability_resolve_ref's existing form)
   as the identity for roundtable seats, `sources` attribution, `--via`, and health
   keys? Display uses the catalog `name` ("GPT-5.6 Sol"). Any reason not to?

7. MIGRATION. Existing agents.json entries name one model each. What is the
   least-surprising upgrade path, and should provider-general registration be
   opt-in per agent or a global mode?

8. SMALLEST FIRST SLICE that delivers operator-visible value without destabilising
   routing, given capability routing is now ON by default.
