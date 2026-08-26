# Proposal: Capability-thresholded delegate routing

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

> **Archived delivered scope (2026-07-26).** This proposal is retained as the historical
> specification for work already delivered. Remaining work is tracked in
> [`capability-thresholded-delegate-routing-residual.md`](../pending/capability-thresholded-delegate-routing-residual.md).

- **State:** DONE. Delivered scope archived 2026-07-26.
- **Author:** JBailes
- **Date:** 2026-07-22
- **Supersedes:** nothing. Extends the existing `agent_route_with_caps()` path.
- **Related:** `standing-benchmark-cadence.md` (competence data source),
  `agentic-supervised-swebench.md` (per-model agentic score),
  `provider-neutral-economizer-safety-spec.md` (normative gate; see §7)

## 0. Target operator experience

> The operator inputs a **provider**. aimee automatically detects its **models, costs, and
> capabilities**, and lists **provider+model** wherever a specific model must be selected or
> attributed.

Everything below serves that. Routing then picks the cheapest model whose capability meets the
packet's requirement (§1), and any surface that names a model, roundtable seats, `--via`,
review attribution, names it as `provider:model` with a catalog display name (§5b.1).

The data to do this already exists and aimee already ingests it. models.dev supplies, per model:
context window, input/output/cache price, capability flags, deprecation, and a display name.
The provider registry (`model_provider_t.fetch_models`, `server_provider_models_cached()`)
supplies the live model list per provider. **The one thing standing between those two sources
and the experience above is the join key**: capability lookup is keyed on `(provider, model)`,
and the live config supplies a wire-protocol name (`anthropic`) where the catalog expects a
vendor name (`minimax`, `moonshotai`), §2.6. Fix that join and auto-detection of models, costs
and capabilities becomes reading data aimee already has.

## 1. Objective

> Route each delegate packet to the **cheapest agent whose capability meets the packet's
> requirement**. Capability is a *filter*; price is the *ordering* among survivors.

Stated as a rule: threshold, then minimize. Deliberately NOT a scalar value score. See §3.2.

## 2. Verified current state

All claims in this section were read from source or from the live server, not inferred.

### 2.1 The router already implements the objective, but it is disabled

`agent_route_with_caps_inner()` (`src/server/agent_config.c:1802`) filters candidates by
capability flags and minimum context window, then routes to `min_tier` among survivors. That is
exactly threshold-then-minimize. But its first statement is:

```c
if (!sys_cfg || !sys_cfg->model_meta_capability_routing)
   return agent_route(cfg, role);          /* agent_config.c:1808 */
```

`model_meta_capability_routing` is documented at `src/modules/config/config.h:1943` as
"0 = cost-tier only (default), 1 = filter by capability flags", and `config.c` does not
initialize it (the surrounding defaults block ends at `model_meta_refresh_minutes = 60`).
**Default is off.** Production therefore runs `agent_route()`: cheapest tier that merely
*supports the role*, where role support is a name match against a config list
(`agent_supports_role()`). No capability is consulted.

### 2.2 `cost_tier` semantics: lower = cheaper (confirmed against the live server)

`aimee agent list --json` against 192.168.1.254:8743 returns four enabled agents:

| name | model | cost_tier | roles | context_window |
|---|---|---|---|---|
| `claude` | `claude-opus-4-8` | **1** | code, review, explain, refactor, draft, execute, all | 200000 |
| `codex` | `gpt-5.6-sol` | **0** | all | 272000 |
| `MiniMax-M3` | `MiniMax-M3` | **0** | all | *(absent)* |
| `kimi-k2.7-code` | `kimi-k2.7-code` | **0** | all | *(absent)* |

Opus at tier 1, the three cheap agents at tier 0 confirms lower = cheaper, consistent with
`agent_route()` selecting `min_tier` and with the built-in cheap planner normalizing to tier 0
(`agent_config.c:196-202`).

### 2.3 Consequence: the premium agent is currently unreachable by role routing

`agent_route()` computes `min_tier` over eligible agents and then, in its second pass, only
considers agents where `ag->cost_tier == min_tier` (`agent_config.c:1729`). The
`primary_default` early-return is *inside* that filtered loop, so a default agent above
`min_tier` is never returned.

With the live config, `min_tier == 0` for every role, because all three tier-0 agents declare
`roles: ["all"]`. Therefore **`claude` / `claude-opus-4-8` is never selected by role-based
routing for any role**, including `review` and `code`. It is reachable only via an explicit
`--via claude`, `--tier 1`, or a provider/model override
(`delegate_apply_route_overrides`, `delegate_routing.c:271`).

This is the "cheap silently eats everything" failure, already live.
It is also why the feature must be framed as capability protection: the system is already
maximally cheap and has no floor.

### 2.4 A coarse learned router already exists

`src/server/server_compute.c:837-856` samples a two-arm DB2 bandit named `delegate_routing`
with arms `{"cheapest", "premium"}`, gated on `bandit_live_decision_enabled`. A `premium` draw
is translated to a concrete tier via `delegate_max_cost_tier()` (`delegate_routing.c:215`) and
applied as `tier_override`. The decision is closed at `server_compute.c:1635-1645` with a
reward incorporating realized spend, "so the bandit prefers cheaper arms at comparable
quality."

**Implication: do not build a new learned router.** The learning substrate exists. It is
capability-blind and has only two coarse arms, and it can currently only move *up* to
`delegate_max_cost_tier`. It cannot express "cheapest agent that is good enough."

### 2.5 What "capability" currently means

`MODEL_CAP_REASONING|TOOLS|VISION|PDF|AUDIO|STREAMING` plus context window and a `deprecated`
flag (`src/headers/model_registry.h:25-39`). These are **modality and plumbing** facts: can the
model physically accept this request. Requirements are inferred deterministically from the
prompt by `delegate_infer_capability_requirements()` (`delegate_routing.c:11`), file
extensions map to VISION/PDF/AUDIO, `strlen/4` gives a token estimate.

Nothing expresses **competence**: whether the model is good enough at the task. Haiku and Opus
both satisfy `MODEL_CAP_TOOLS`. `MODEL_CAP_REASONING` is a boolean. It says a model exposes
reasoning, not that it reasons well enough for this packet.

### 2.6 The context windows ARE available: the config conflates wire protocol with vendor

models.dev carries both context windows and prices for every live agent, and aimee already
ingests it (`src/models_dev.c`, `https://models.dev/api.json`, 24h TTL cache at
`~/.cache/aimee/models_dev.json`, plus a bundled snapshot and operator overrides).
Fetched 2026-07-22:

| models.dev key | context | $/Mtok in | $/Mtok out | cache read |
|---|---|---|---|---|
| `minimax/MiniMax-M3` | **1,000,000** | 0.30 | 1.20 | 0.06 |
| `moonshotai/kimi-k2.7-code` | **262,144** | 0.95 | 4.00 | 0.19 |
| `anthropic/claude-opus-4-8` | 1,000,000 | 5.00 | 25.00 | 0.50 |
| `openai/gpt-5.6-sol` | 1,050,000 | 5.00 | 30.00 | 0.50 |
| `openai/gpt-5.6-terra` | 1,050,000 | 2.50 | 15.00 | 0.25 |
| `openai/gpt-5.6-luna` | 1,050,000 | 1.00 | 6.00 | 0.10 |

The lookup misses because `model_capability_get()` matches on provider with `strcasecmp` in
every path, operator overrides, the dynamic cache, the static table, and
`models_dev_cache_lookup()` (`model_registry.c:852-899`). The live agents are registered with
`provider: "anthropic"` because they speak the **Anthropic wire format**, but models.dev keys
them by **model vendor**: `minimax` and `moonshotai`.

**The config conflates transport shape with vendor identity.** That single conflation causes
every defect in §2.7. `agent_t` needs to carry both: a wire/API shape (which determines the
request builder) and a catalog identity (which determines capability lookup).

### 2.7 What the misregistration actually causes: and a correction

An earlier draft of this proposal claimed enabling capability routing would strand these agents
**fail-closed**. That was wrong. Verified behavior is the opposite and worse:

`model_capability_get()` never returns 0 for a named model. It falls through to
`model_capability_get_heuristic()` (`model_registry.c:251`), which always returns 1. So the
fail-closed branch in `agent_satisfies_required_caps()` (`agent_config.c:1770`) is unreachable
for any named model. Consequences with the live config:

1. **Capability inference is actively suppressed.** The heuristic has a `minimax` branch
   granting `REASONING|TOOLS|STREAMING` (`model_registry.c:304`), dead code for these agents,
   because their provider reads `anthropic`. They instead take the Anthropic branch, match no
   `claude-*` prefix and neither "opus" nor "sonnet", and so resolve to `TOOLS|STREAMING`
   only: **no REASONING, no VISION, no PDF**.
2. **MiniMax-M3 gets a silently wrong context window.** `model_context_window()` prefix-matches
   the stale `{"minimax", 200000}` fallback (`model_registry.c:196`), yielding **200,000**
   against a true **1,000,000**. A 5x understatement that would exclude a 1M-context model
   from exactly the long-context packets it is best suited to.
3. **kimi-k2.7-code resolves to context window 0.** No `kimi`/`moonshot` entry exists in
   `g_ctx_windows`. And the context gate is **fail-open on zero**:
   `if (min_context > 0 && effective_ctx > 0 && effective_ctx < min_context) return 0;`
   (`agent_config.c:1789`). An unknown context window skips the check entirely, so kimi can be
   handed a prompt exceeding its real 262,144 ceiling.

4. **Reasoning timeouts are wrong today, independent of routing.** `agent_config.c:688` derives
   the per-call timeout from `model_capability_get(ag->provider, ag->model, ...)`: a
   `MODEL_CAP_REASONING` model gets `AGENT_REASONING_TIMEOUT_MS`, everything else gets
   `AGENT_DEFAULT_TIMEOUT_MS`. Both MiniMax-M3 and kimi-k2.7-code are reasoning models per
   models.dev, but resolve without the flag (item 1), so **both currently run on the short
   default timeout**. The comment at that site names the exact symptom this causes: slow
   multi-minute completions "cut off and retried as spurious read failures."

So the migration hazard is that **capability routing would run on wrong data and fail open**, which is the worse failure: it looks like it is working. And item 4 is a
live operational defect right now, with capability routing still off.

### 2.9 Root cause is a too-narrow guard on an existing normalization

The intent already exists in the codebase. `agent_config.c:664-666`:

```c
if (strcmp(ag->provider, "openai") == 0 &&
    (strstr(ag->endpoint, "api.minimax.") || strstr(ag->model, "MiniMax-M2")))
   snprintf(ag->provider, sizeof(ag->provider), "%s", "minimax");
```

This is exactly the wire-shape-to-vendor remap §2.6 calls for, but its guard misses the live
config twice over: it fires only when `provider == "openai"` (live agents are `"anthropic"`)
and it hardcodes the model substring `MiniMax-M2` (live model is `MiniMax-M3`). There is no
moonshotai/kimi equivalent at all.

This is therefore a **bug in an existing mechanism**, not a missing feature, which makes
Slice 0 considerably smaller than a new abstraction. Note `agent_t.provider` is
`char[16]` (`agent_types.h:238`); `"moonshotai"` fits, but the field is tight enough that any
vendor-identity work must check bounds rather than assume headroom.

### 2.8 `cost_tier` is factually inverted for at least one agent

Per §2.2, `codex` / `gpt-5.6-sol` sits at `cost_tier` **0**, the cheapest tier, alongside
MiniMax ($0.30/$1.20) and kimi ($0.95/$4.00). Its actual price is **$5.00 / $30.00 per Mtok**:
the most expensive model in the fleet, output-pricier than `claude-opus-4-8` at $5.00/$25.00,
which sits at tier **1**.

The tier ordering the router minimizes over does not correspond to cost. `sol` is the premium
Codex tier; `terra` ($2.50/$15) and `luna` ($1.00/$6) are the cheaper variants and are not
registered at all.

This must be corrected before any competence work, because it means today's "cheapest-first"
router is minimizing a hand-entered integer that disagrees with the published price sheet, and not
price at all.

### 2.8.1 A principled tier ladder exists in the catalog

models.dev prices the exact fleet under discussion into a clean total order
(fetched 2026-07-22, $/Mtok):

| proposed tier | in | out | models | context |
|---|---|---|---|---|
| 0 | 0.30 | 1.20 | `minimax/MiniMax-M3` | 1,000,000 |
| 1 | 0.95–1.00 | 4–6 | `moonshotai/kimi-k2.7-code`, `anthropic/claude-haiku-4-5`, `openai/gpt-5.6-luna` | 200k–1.05M |
| 2 | 2.00–2.50 | 10–15 | `anthropic/claude-sonnet-5`, `openai/gpt-5.6-terra` | 1M–1.05M |
| 3 | 5.00 | 25–30 | `anthropic/claude-opus-4-8`, `openai/gpt-5.6-sol` | 1M–1.05M |
| 4 | 10.00 | 50 | `anthropic/claude-fable-5` | 1,000,000 |

Live tiers vs. this ladder: `MiniMax-M3` 0 → 0 (correct); `kimi-k2.7-code` 0 → 1;
`claude-opus-4-8` 1 → 3; **`gpt-5.6-sol` 0 → 3**, a three-tier error on the most expensive
model in the fleet.

Two observations that matter for §3:

- Input-price order and output-price order agree at tier granularity (the only ties are
  haiku/luna at $1 input and opus/sol at $5 input), so a single integer tier is a defensible
  *cheapness ordering* even though it is not a defensible *quality* ordering. This is the
  §3.2 split working as intended.
- **Every model in the table reports `reasoning: true`.** `MODEL_CAP_REASONING` therefore
  discriminates nothing across this fleet, direct evidence for §2.5 that the existing
  capability flags cannot express competence, and that Slice 2 needs a real measured axis
  rather than another boolean.

### 2.10 CORRECTION: aimee downloads models.dev but cannot read it

§0 and §2.6 of earlier drafts asserted "aimee already ingests models.dev". That is **false**,
verified 2026-07-22. It downloads it and the reader cannot parse what was downloaded.

**Format mismatch.** `models_dev_refresh()` (`src/models_dev.c:58-113`) shells out to
`curl -s -o <cache> https://models.dev/api.json` and atomically renames the result. No
transform. The live api.json is NESTED:

```json
{"minimax": {"models": {"MiniMax-M3": {"limit": {"context": 1000000},
                                        "cost": {"input": 0.3, "output": 1.2}}}}}
```

But the reader (`src/models_dev_cache.c:78-84`, `fill_cap_from_json` at `:14-45`) expects a
FLAT dict keyed `"provider/model"` with camelCase fields:

```json
{"anthropic/claude-opus-4-7": {"contextWindow": 200000, "maxTokens": 32000,
                               "inputCost": 15.0, "outputCost": 75.0, "tools": true}}
```

`cJSON_GetObjectItemCaseSensitive(root, "minimax/MiniMax-M3")` against a nested root returns
NULL. **The downloaded cache can never resolve a single model.** Consistent with
`models_dev_capability_get()` being an explicit stub returning 0
(`models_dev.c:115-122`, comment: "full ingestion from models.dev cache is future work").

**The bundled snapshot is flat but stale and tiny.** `data/models_dev_snapshot.json` has **10
entries**: `claude-opus-4-7`, `claude-sonnet-4-6`, `claude-haiku-4-5-20251001`, `gpt-4o`, and
six others. **Not one live fleet model.** `claude-opus-4-8` (the operator's primary) is
absent, as are `gpt-5.6-*`, `MiniMax-M3`, and `kimi-k2.7-code`.

So for every model in the live fleet, `models_dev_cache_lookup()` returns 0 from both sources,
and every capability resolution falls through to the static `g_capabilities` table or
`model_capability_get_heuristic()`.

### 2.10.1 What this means for the slices

- **Slice 0 remains correct and delivered.** It made the heuristic and the static prefix table
  resolve under the right vendor identity, which is exactly why its tests assert heuristic and
  static values rather than catalog values.
- **`cost_in_per_mtok` / `cost_out_per_mtok` are 0 for every live model.** They are populated
  only from the static table or operator overrides. **Slice 0b's price lint therefore has no
  data and cannot be built as specified.**
- **`MODEL_CAP_REASONING` is never set from catalog data at all**: `fill_cap_from_json` reads
  `tools`, `vision`, `pdf`, `deprecated` and no reasoning key. Today it comes solely from
  per-vendor heuristic branches.
- §0's promise ("aimee auto-detects models, costs, and capabilities") is blocked on this, not
  only on the join key. Fixing the join key was necessary and insufficient.

**New Slice 0b-pre (blocks 0b and 0c): make models.dev ingestion actually work.** Preferred
shape: teach the reader BOTH schemas, flat key first (preserves the bundled snapshot and
`model_overrides.json`, which share the flat format), then a nested traversal for the
downloaded api.json, and capture `reasoning`/`tool_call` while there. This avoids rewriting
downloaded bytes, keeps the atomic-rename download, and leaves the override format untouched.

### 2.11 Catalog context window is NOT the routing ceiling (operator correction)

Operator correction, 2026-07-23. Earlier drafts implied the per-agent
`middleware.context_window` values in the live config were stale under-reports to be
"corrected" against the catalog. **They are deliberate policy and must not be overridden:**

| agent | catalog max | configured ceiling | why the ceiling is intentional |
|---|---|---|---|
| `claude` / claude-opus-4-8 | 1,000,000 | **200,000** | premium charges apply above 200k |
| `codex` / gpt-5.6-sol | 1,050,000 | **272,000** | the product expects requests to stay within 272k |

So two distinct quantities were being conflated:

- **Capability context window**: what the model *can* accept. Catalog data. Used to reject a
  packet the model physically cannot hold.
- **Routing/serving ceiling**: what the operator *permits*, which may be lower for cost or
  product reasons. Operator config.

Treating the catalog value as authoritative would push requests across a provider price cliff.
The existing precedence is therefore correct and must be preserved: an explicit
`middleware.context_window` always wins over the catalog
(`agent_config.c` `agent_satisfies_required_caps`, and `agent_meets_filter` in
`delegate_routing.c`). Slice 0 preserves it.

This also matters to §7: crossing 200k on Claude changes the price band, which is a genuine
cost cliff a routing decision can trigger. A future competence/price model must treat price as
varying WITHIN a model by context band, not as one number per model.

### 2.11.1 Regression this creates in prompt budgeting: OPEN

`agent_exec_context_budget_chars()` (`src/server/agent_context_budget.c`) reserves the model's
output ceiling from the window:

```c
int output_tokens = agent->max_tokens > 0 ? agent->max_tokens
                                          : model_max_output(agent_catalog_provider(agent), agent->model);
int prompt_tokens = agent->middleware.context_window - output_tokens;
```

No live agent pins `max_tokens`, so the fallback applies. Correcting the catalog identity raised
`model_max_output` for `claude` from **8192 to 128000**, so with the deliberate 200k ceiling the
prompt budget falls from ~191,800 to ~72,000 tokens, a ~62% reduction. `codex` is affected the
same way (272k ceiling, 128k reserved).

Reserving a model's *theoretical maximum* output from a *policy-capped* window is too
conservative: these models will not emit 128k tokens on a typical delegate turn. Options, none
yet chosen:

1. Reserve `min(model_max_output, ceiling / 4)`, bounded, no config change.
2. Reserve the agent's configured `max_tokens` and default it explicitly per agent.
3. Add a separate `reserved_output_tokens` knob distinct from the model ceiling.

This is a behavioural consequence of an already-committed change and must be resolved before
these commits are relied on in production.

### 2.12 Pricing: resolved by default, operator-overridable, three axes

Operator direction, 2026-07-23: default to catalog-resolved pricing and let the operator set
their own in the GUI/CLI. Implemented as `agent_resolved_price()`, override first, catalog
second, per axis, which is now the single source of truth for "what does this agent cost us".

This supersedes `tier_price_exempt` as the primary mechanism for the subscription case. Stating
the real marginal price is strictly more informative than opting out of the comparison: the
lint keeps working, and the number is visible to anything else that wants it.

**Three billed axes, not one.** Prices are cached-read / input / output per million tokens.
Cache read is roughly an order of magnitude below input, so on any prompt-caching workload it
dominates real spend and cannot be approximated by the input rate. Verified against the live
catalog 2026-07-23:

| model | in | out | cached |
|---|---|---|---|
| `minimax/MiniMax-M3` | 0.30 | 1.20 | **0.060** |
| `moonshotai/kimi-k2.7-code` | 0.95 | 4.00 | **0.190** |
| `anthropic/claude-haiku-4-5` | 1.00 | 5.00 | **0.100** |
| `anthropic/claude-opus-4-8` | 5.00 | 25.00 | **0.500** |
| `openai/gpt-5.6-luna` | 1.00 | 6.00 | **0.100** |
| `openai/gpt-5.6-sol` | 5.00 | 30.00 | **0.500** |

Cached price is OPTIONAL: many providers publish none, and its absence must never make an
otherwise-priced agent look unpriced, nor be read as free. It is reported separately and
omitted from JSON entirely when unknown.

Surfaces: `aimee agent --price-in/--price-out/--price-cached`, persisted in agents.json, and
emitted by `aimee agent list --json` alongside `price_overridden` so the GUI can show whether a
figure is the operator's or the catalog's.

### 2.12.1 The catalog encodes CONTEXT-BAND pricing: unexploited

While adding the cached axis, the live catalog turned out to publish per-context-band prices,
which is exactly the §2.11 price cliff in machine-readable form:

```json
"openai/gpt-5.6-sol": {"cost": {"input": 5, "output": 30, "cache_read": 0.5,
  "tiers": [{"input": 10, "output": 45, "cache_read": 1,
             "tier": {"type": "context", "size": 272000}}]}}
"minimax/MiniMax-M3": {"cost": {"input": 0.3, ...,
  "context_over_200k": {"input": 0.6, "output": 2.4, "cache_read": 0.12}}}
```

So `gpt-5.6-sol` **doubles** to $10/$45 above 272k, and MiniMax-M3 doubles above 200k. This
confirms the operator's constraint is not a preference but a real billing cliff, and it means
price is a function of (model, context band), not one number per model.

**RESOLVED 2026-07-23**, bands are now modelled; what follows was the gap.
Correction: the MiniMax-M3 threshold is **512,000**, not 200,000. The registry's
`context_over_200k` key name does not encode the real threshold (sol's is 272,000),
so only the structured `cost.tiers[].tier.size` is authoritative.

Superseded gap description: `model_capability_t` held a single price triple. Implications for later
slices, a routing decision that pushes a packet across a band changes the price by 2x, so any
cost comparison must know which band the packet lands in; and §7's economizer interaction is
sharper, since a context-reducing transform that crosses a band downward is a genuine, provable
saving of exactly the kind v2 authorises (its class 4, "long-context threshold avoidance").

## 3. Design

### 3.1 Capability becomes a total predicate

Extend the capability predicate with a **competence** axis alongside the existing plumbing
axes. A packet class declares a minimum competence; an agent declares its measured competence;
the router filters on `agent_competence >= packet_requirement` exactly as it already filters on
`effective_ctx >= min_context` in `agent_satisfies_required_caps()`
(`agent_config.c:1758-1793`).

Price ordering is untouched: `cost_tier` remains the integer cheapness ordering and is used
only to pick `min_tier` among survivors.

### 3.2 Rejected: scalar value scores

A `capability / cost` score, or any weighted sum of quality and price, is rejected. Worked
counterexample: capability 3 at cost 1 scores 3.0; capability 10 at cost 5 scores 2.0. The
score selects the capability-3 agent for a packet requiring capability 10.

Any scalarization trades competence for cheapness at some exchange rate, and there is no
exchange rate at which an agent that cannot do the task is worth its discount. **Competence is
a constraint, not a term in an objective.**

This also resolves the round-1 review finding that a single integer `cost_tier` cannot
faithfully order price, latency, quality, tool fidelity and rate limits across Anthropic /
Codex / OpenRouter / local / MiniMax / Kimi. Under threshold-then-minimize those dimensions
never have to share a scale: the non-price dimensions are filters, price alone orders.

### 3.3 Rejected: escalation ladders

Dispatch-cheap-then-retry-higher is rejected. Its worst case costs strictly more than
dispatching the capable agent once, it spends tokens on attempts known to be discarded, and it
can never satisfy the economizer spec's inequality (§7). One dispatch per packet.

### 3.3b Operator-stated invariants (these outrank cost)

Stated by the operator 2026-07-22, in priority order above any cost objective:

1. **No outages.** Availability is paramount. Routing must not fail a request because no agent
   qualified.
2. **User-facing sessions must be coherent.** A user must never be handed a model too weak to
   hold a reasonable interaction. This is about the capability of the seat the user talks to,
   not about model stability across turns.
3. **The managing agent**: the orchestrator that decomposes work and dispatches other agents,
must also run on the most capable tier. Its errors multiply across every packet it creates.
4. **Never route a packet to a model that cannot complete it** (the operator's phrasing: do not
   send to Luna what needs Sol).

Cheapest-with-capability (§1) therefore applies to **bounded delegate packets**, not to the
user-facing turn and not to the orchestrator.

#### The floor table

| context | floor | mechanism |
|---|---|---|
| user-facing turn | primary / highest available tier | `agent_routing_primary_turn()` already exists |
| orchestrating/managing agent | primary / highest available tier | needs a marker; see below |
| bounded delegate packet | cheapest qualifying (§1) | `agent_route_with_caps()` |

#### The mechanism for (2) already exists and is currently broken

`agent_routing_primary_turn()` (`src/server/agent_config.c:1475`) is a thread-local marker set
for a primary chat turn, and `agent_route()` consults it at `:1698`:

```c
if (agent_routing_primary_turn() && cfg->default_agent[0])
   primary_default = agent_find(cfg, cfg->default_agent);
```

But `primary_default` is only *returned* from inside the second pass, which filters
`ag->cost_tier == min_tier` (`:1729`). With `claude` at tier 1 and three agents at tier 0
(§2.2), the primary user-facing turn cannot reach Opus. **Invariant (2) is already the design
intent, already has a mechanism, and is already defeated by the same `min_tier` filter
described in §2.3.**

Fix: on a primary turn, the default agent is selected regardless of tier. This is small,
self-contained, and directly serves the operator's highest-value invariant. It belongs in
Slice 0a.

#### The orchestrator has no equivalent marker

No `agent_routing_primary_turn()` analogue exists for "this agent is managing other agents".
`primary_only` (`server.c:1957`) is an *exclusion* (it removes an agent from delegation) not
a floor. Establishing invariant (3) requires a new marker on the orchestration path. This is
listed as work, not assumed present.

### 3.3c Fail upward, not open and not closed

Invariant (1) plus invariant (4) determine what happens when the capability filter admits
nobody, or when an agent's capability cannot be established:

> Unknown or unverifiable capability disqualifies an agent from **cheap** routing; the packet
> routes to the primary instead. Never reject the request, and never silently accept an
> unqualified agent.

This supersedes the earlier §4 recommendation to close the zero-context gate strictly. Strict
fail-closed violates invariant (1); the current fail-open (§2.7 item 3) violates invariant (4).
Fail-upward satisfies both: the failure mode becomes "we spent more than necessary", visible,
recoverable, and never an outage or a garbage result.

It also gives §3.1's filter a defined answer for the empty case instead of an error, and it is
the reason the escalation ladder of §3.3 is not needed: escalation *discovers* incapability by
paying for a failure, whereas fail-upward *assumes* it whenever capability is unproven.

### 3.4 Competence data source

Competence must be **measured offline on a cadence, spending no production tokens**. Two
pending proposals already produce exactly this and should be the source of record:

- `standing-benchmark-cadence.md`: scheduled full LoCoMo / LongMemEval / memory / code-vector
  runs with persisted, retained, drift-checked scores. Its own thesis is that aimee cites
  parity numbers nobody measures.
- `agentic-supervised-swebench.md`: a graded, tool-using, per-model agentic score against the
  official SWE-bench Docker grader.

Until those land, competence is **operator-declared per agent** with the declaration recorded
and auditable. An undeclared agent is treated per the fail-closed rule in §4.

## 4. Fix catalog identity before enabling anything

Per §2.6-2.8 the required precondition is **not** hand-populating context-window overrides. The
data already exists in a source aimee already ingests; it is being looked up under the wrong
key. Manual overrides would paper over the conflation and go stale on the next model.

The precondition is, in order:

1. **Separate wire shape from catalog identity** on `agent_t`. `provider` currently does double
   duty. Capability lookup must use the vendor identity (`minimax`, `moonshotai`), while the
   request builder keeps using the Anthropic wire shape.
2. **Replace the fail-open hole with fail-upward** (§3.3c) at `agent_config.c:1789`: an
   effective context window of 0 must not qualify an agent for *cheap* selection, but must
   route to the primary rather than reject. A strict rejection would violate the operator's
   no-outage invariant. Superseded wording follows for history: an effective context window of 0 must
   not skip the `min_context` gate. Decide explicitly whether unknown-context means reject
   (fail closed) or requires an operator override, but it must not silently pass.
3. **Fix `cost_tier`** to agree with published price (§2.8), or derive it from the
   `cost_in_per_mtok` / `cost_out_per_mtok` fields that `model_capability_t` already carries
   and `capability_copy_from_json()` already ingests (`model_registry.c:678-690`).
4. **Retire or correct the stale `{"minimax", 200000}` prefix fallback**
   (`model_registry.c:196`), which silently under-reports MiniMax-M3 by 5x.

Only then does enabling `model_meta_capability_routing` produce correct decisions rather than
confident wrong ones.

## 5. Slices

### Slice 0: Catalog identity correctness (NEW, blocks everything else)

**Scope:** §4 items 1-4. Separate wire shape from catalog identity; close the zero-context
fail-open; correct `cost_tier` against published price; retire the stale minimax prefix.
No routing-policy change. This only makes the existing inputs true.

**Acceptance criteria:**
- `model_capability_get()` resolves `MiniMax-M3` to context 1,000,000 and `kimi-k2.7-code` to
  262,144 from the models.dev cache, with `REASONING|TOOLS` flags, while both continue to
  dispatch over the Anthropic wire shape unchanged.
- A test asserts an effective context window of 0 does not pass a positive `min_context` gate.
- `cost_tier` ordering across the live fleet agrees with `cost_in/out_per_mtok`; a lint fails
  when a tier ordering contradicts the catalog price ordering.
- No behavioral change while `model_meta_capability_routing` is off (it still returns
  `agent_route()`), so this slice is independently shippable.

### Slice 1: Capability-routing enablement

**Scope:** flip `model_meta_capability_routing` to default 1; keep the config key as an escape
hatch. Depends on Slice 0.

**Acceptance criteria:**
- A dry-run route report shows, for each (role x representative packet class), the candidate
  set before and after the flag flip, with no agent silently stranded and no agent admitted on
  a zero/unknown context window.
- `src/tests/test_agent_caps.c` extended; flag default asserted in `test_config.c`.
- No change to `agent_route()` semantics when the flag is off.

**Validation-pending:** the dry-run must be executed against the live server config; it cannot
be certified from the worktree alone.

### Slice 2: Competence as a declared capability axis

**Scope:** add a competence field to the agent record and to `model_capability_t`; extend
`agent_satisfies_required_caps()` with the threshold comparison; add a per-(role x packet
class) required-competence table with a conservative default.

**Acceptance criteria:**
- With a competence floor declared for `code`, a tier-0 agent below the floor is excluded and
  `claude` (tier 1) is selected, i.e. §2.3 is fixed and provably so in a test.
- With no floor declared, routing is byte-identical to Slice 1 behavior.
- Competence declarations are auditable: the route decision log records the threshold, each
  candidate's competence, and the exclusion reason.

### Slice 3: Provider breadth

**Scope:** register the remaining providers (Fable / Sonnet / Haiku, Codex terra / luna,
OpenRouter, local llama.cpp / Ollama) with honest `cost_tier`, catalog entries, and competence
declarations.

**Precondition:** Slices 1 and 2 merged. Registering cheap agents *before* a competence floor
exists reproduces §2.3 at greater scale, every new tier-0 agent immediately captures traffic
from every role it claims.

**Acceptance criteria:**
- Each new agent's competence declaration cites a benchmark run or an explicit operator
  declaration with a date.
- Admission behavior verified under saturation: the global fail-closed cap is 14 concurrent /
  5 per model (`agent_admission.c`); a 15–20 agent population must be shown not to deadlock or
  starve a tier.
- Dominated agents (cheaper AND more capable than a peer) are reported by a config lint so the
  registry gets pruned rather than accumulating unreachable entries.

### Slice 4: Reconcile the existing bandit

**Scope:** make the `delegate_routing` bandit (§2.4) capability-aware, arms must select among
*qualified* candidates only, so a `cheapest` draw can never select below the competence floor.

**Acceptance criteria:**
- The bandit cannot produce a route that the capability predicate would reject.
- Reward remains spend-aware; no new production token spend is introduced by the bandit itself.

## 5b. Slice 0c: provider-general registration (operator-requested shape)

**Requested behavior:** registering `codex` registers *codex*, one agent. aimee selects the
appropriate model (`sol` / `terra` / `luna`) per dispatch. Not three agent entries.

### What already exists

- Provider registry + GUI surface: `aimee provider list|show|models|test|quota` ->
  `/v1/provider.*` (`src/cli_v1_routes.c:322-329`), `model_provider_t` with `default_model`,
  `default_aux_model`, `fallback_models`, `fetch_models` (`src/headers/model_provider.h`).
- Model enumeration with caching: `server_provider_models_cached()`
  (`src/server_provider.c:104`). DB1-backed, 1h TTL, falls back to cache when the live fetch
  fails.
- An auto-select already exists at registration: `ag_probe_models()` (`src/cmd_agent.c:175`)
  takes the **first id returned by the provider** when no model is requested
  (`cmd_agent.c:218-222`). That is arbitrary, not appropriate, for `moonshotai` the list
  includes deprecated previews.
- Per-call model reporting is already modeled: `agent_result_t` carries `model`,
  `served_model`, and `requested_model` as distinct fields (`agent_types.h:344-358`), with a
  comment explicitly distinguishing what aimee served from what the provider echoed.

### What does not exist: per-dispatch model selection

Dispatch keys off `agent->model` as a fixed per-agent value
(`src/server/agent_runtime.c:1105-1106`, `:1174`). `openai_chat.c` takes `model` as a
parameter, but callers fill it from `agent->model`. So the *reporting* side is ready and the
*selection* side is not.

Making the provider the registered unit means routing must return **(agent, model)** rather
than an agent. That is a structural change to the core routing loop, with four consequences
that must be designed, not discovered:

1. **`cost_tier` stops being an agent property.** Price varies by model *within* a provider
   (§2.8.1: sol $5.00, terra $2.50, luna $1.00). `agent_route()` minimizes `ag->cost_tier`
   (`agent_config.c:1710`); under provider registration there is no single tier for `codex`.
   Tier must move to the model, and the router must minimize over (agent, model) pairs.
2. **Health is keyed by agent name.** `provider_catalog_get_health(peer->name)`
   (`agent_fallback.c:89`) uses the agent name. With one agent per provider, a failing `sol`
   would mark all of `codex` DOWN, including healthy `luna`. Health must become per-model, or
   per (provider, model).
3. **Admission is already per-model. This part is fine.** `concurrency_per_model` is keyed by
   model string (`ag_set_model_concurrency`, `cmd_agent.c:278`), so per-model caps keep working.
   But `max_parallel` is per-agent and would become a shared provider-wide ceiling across all
   its models. That is the *correct* semantic (a provider quota), but it is a
   behavior change and must be stated.
4. **Same-tier fallback becomes ill-defined.** `agent_try_same_tier_fallback()` iterates peers
   at equal `ag->cost_tier` (`agent_fallback.c:78-85`). With tier on the model, "same tier"
   must mean same-tier *models*, possibly within the same provider.

### Why this depends on Slices 0 and 0b

Under provider-general registration no human enters a `cost_tier` or checks a capability
lookup at registration time. Every model discovered by `fetch_models` gets its tier and
capabilities derived from the catalog automatically. Therefore a mis-keyed vendor (§2.6) or a
stale prefix entry (§2.7) propagates directly into routing with no operator in the loop.

Concretely: registering `codex` today discovers `sol`, `terra`, `luna` and, with no price basis
wired in, lands all three at a default tier, reproducing the `sol`-at-tier-0 error of §2.8
three times over.

This also **reorders the proposal**. Slice 3 (provider breadth) previously assumed manual
registration. Provider-general registration makes breadth arrive automatically, so correct
tier/capability derivation moves from "worth doing" to **blocking**.

### 5b.1 Model identity must stay first-class and addressable

Provider-general *registration* does not mean provider-granular *identity*. Anything that picks
or attributes a specific model must continue to name the model, and must display it. For a
registered `codex`, a roundtable must offer and display **Sol, Terra and Luna as three distinct
seats**, not one "codex" seat.

This is a referencing concern, not only a display one. Sites that identify a model today:

- **Roundtable attribution.** `roundtable_result_t.sources[256]` (`roundtable_types.h:96`) is a
  comma-joined name list; a live run in this session produced `"sources":"codex, MiniMax-M3"`.
  Under provider registration `codex` alone is ambiguous. `participants_total` is documented as
  "reference models per round" (`:121`), so the intended granularity is already *model*.
- **Operator pinning.** `--via AGENT` (`delegate_apply_route_overrides`) names an agent.
- **Health.** `provider_catalog_get_health(peer->name)` keys on agent name (§5b consequence 2).
- **Admission.** `concurrency_per_model` already keys on the model string, already correct.

**The canonical form already exists.** `model_capability_resolve_ref()`
(`src/model_registry.c:386`) parses `provider:model` refs, resolves bare aliases, and falls back
to `model_detect_provider()` for an unqualified id. It already backs
`aimee model show [provider:]<model>` (`cli_v1_routes_b.c:746`). Adopting `provider:model`
(e.g. `codex:gpt-5.6-sol`) as the identity for seats, `sources`, `--via`, and health keys is
therefore reuse, not new invention.

**Display names are also already in the catalog.** models.dev carries a per-model `name`:
`gpt-5.6-sol` -> `"GPT-5.6 Sol"`, plus per-provider names (`OpenAI`, `MiniMax (minimax.io)`,
`Moonshot AI`). `model_capability_t` does not currently carry a display name,
`capability_copy_from_json()` (`model_registry.c:643-701`) reads provider, model, context,
max_output, costs, flags, cutoff, open_weights and deprecated, but no name. Adding one
additive field yields operator-facing labels like `GPT-5.6 Sol` without hand-maintained
strings.

**Interaction with `$random` seat selection.** Operator guidance is that roundtable seats use
`$random` so the global admission controller balances load, and that over-pinning a contended
model causes fail-closed degraded panels. Model-granular seats *help* here: the per-model cap
(default 5) applies to more distinct keys, so a provider contributing three models spreads
across three caps instead of contending on one. `$random` must sample over
(provider, model) pairs rather than agents.

### Open design question

Which models does a registered provider expose as routable? `moonshotai` alone lists 10,
including `kimi-k2-0711-preview` and other superseded entries. Options: all non-deprecated
catalog entries; an operator allowlist per provider; or the `fallback_models` list the profile
already carries. Note `model_capability_t` already has a `deprecated` flag and
`agent_satisfies_required_caps()` already rejects on it (`agent_config.c:1791`), so
deprecation filtering is largely free once §2.6 is fixed.

## 5c. Slice 2 refined: role granularity as the competence axis

**Operator direction (2026-07-22):** split roles into specific actions (e.g. `code_simple` vs
`code_complex`, likely several code roles), track how well each model performs per role, and
use that to decide which roles a model may be assigned.

This resolves §6 open question 1. Competence is **not** a scalar and **not** per-domain. It is
per **(model, role)**. That is the right shape, and it fits the existing architecture: roles are
already the routing key (`agent_supports_role()` is consulted in every routing loop,
`agent_config.c:1661, 1688, 1708, 1727, 1751, 1816, 1847`). Per-(model, role) competence
therefore refines a dimension the router already has, rather than adding a new one.

### CORRECTED 2026-07-23: role filtering is not bypassed: the DEFAULT is permissive

An earlier version of this section claimed role filtering was "bypassed twice" and that a role
split therefore required a code change first. **That was wrong.** Operator correction, verified
against source and pinned by `test_declared_roles_route_precisely`.

`agent_is_exec_role()` (`agent_config.c`) consults the 18-role default set **only while an agent
declares no `exec_roles`**:

```c
if (agent->exec_role_count > 0) { /* exact match against the declared list */ return 0; }
/* ...otherwise fall back to default_exec_roles */
```

So a non-empty `exec_roles` REPLACES the permissive default rather than adding to it. Two
declarations give precise routing today, with no code change:

- `roles` must not contain the `"all"` wildcard, which matches every role;
- `exec_roles` must be declared, which makes exec eligibility exact.

Verified: two specialists declaring `code_simple` / `code_complex` each receive exactly their
own role, **including the dearer one, which cheapest-first would never select if role
filtering were inert**, and a built-in role neither declares reaches neither.

**Consequence for Slice 2:** splitting a broad role into specific ones is a CONFIGURATION
action, not a blocked code change. What remains genuinely missing is the competence *data* to
decide which model earns which role, not the routing mechanism to honour it.

The live fleet is permissive only because three of four agents declare `roles: ["all"]` and
none declares `exec_roles`. That is a config posture, not a defect.

### Historical note: what the earlier claim got right

1. **The `all` wildcard.** `agent_has_role()` (`agent_config.c:1353-1362`) treats a literal
   `"all"` entry as matching every role. Per §2.2 the live config has **three of four agents
   declaring `roles: ["all"]`**, `codex`, `MiniMax-M3`, `kimi-k2.7-code`. Role-based filtering
   is therefore inert for exactly the agents that win routing, and this is a direct cause of
   §2.3.
2. **The exec-role fallback.** `agent_supports_role()` returns 1 for any exec role
   (`:1391`), and `agent_is_exec_role()` (`:1894`) falls back to a built-in default set when an
   agent declares no `exec_roles`. So an agent is eligible for default exec roles regardless of
   its declared `roles` list.

Splitting `code` into `code_simple` / `code_complex` while `roles: ["all"]` persists yields
fine-grained roles and unchanged routing. **Narrowing role declarations is a prerequisite, not
a follow-up.**

### Who classifies a packet into a fine-grained role?

Fine-grained roles reintroduce the difficulty-estimation problem this proposal avoided in §3:
something must decide a packet is `code_simple` rather than `code_complex`.

It does not need a classifier. **The role is chosen by the orchestrator when it creates the
packet**. That is already how delegation works (`aimee delegate <role> ...`). And per §3.3b
invariant (3), the orchestrator is pinned to the most capable tier. So the labelling is
performed by the most capable model in the fleet, at the moment it decomposes the work, as a
side effect of an action it already takes. No inference layer, no learned router, no extra
dispatch.

This is the cleanest resolution available and should be stated as the design: *the capable
model labels the packet; the cheap model executes the labelled packet.*

### Measurement burden is the real cost

Per-(model, role) competence is a matrix. At the live fleet size and current role list
(`code, review, explain, refactor, draft, execute, summarize, format, search, diagnose,
validate` — 11 roles, per `aimee delegate --list-roles`), that is already ~44 cells for four
agents. Splitting `code` three ways and registering the full provider set of §2.8.1 pushes it
past 150.

Consequences that must be designed, not discovered:

- **Sparse matrices are the normal case.** Most cells will have no measurement. The default for
  an unmeasured (model, role) cell must follow §3.3c: unproven competence disqualifies the
  model from *cheap* selection for that role and routes upward. It must not default to
  "capable".
- **Measurement cost scales with the split.** Every new role multiplies the benchmark surface.
  This is the strongest argument for splitting roles *sparingly* and only where routing would
  actually differ. A role split that never changes which model is selected is pure cost.
- **`roles[]` becomes derived, not declared.** The operator direction "if they can be assigned
  them" means the assignment list should ultimately follow measured competence rather than
  hand-editing. That is the endpoint; the interim is an operator declaration with a recorded
  basis and date (§3.4).

### Sequencing

This is Slice 2 and it depends on Slice 0 (catalog identity) and on a competence data source
(§3.4). The role split itself, narrowing `roles[]`, constraining the `all` wildcard, closing
the exec-role fallback, is separable and can land earlier, since it is a correctness fix to
role filtering independent of any competence measurement.

## 6. Open questions

1. What is the competence scale? A single ordinal, or per-domain (code / review / reasoning /
   long-context)? Per-domain is more honest but multiplies the declaration burden.
2. How does a packet class get its required-competence value without an LLM classifying it?
   Current proposal: static per-(role x blast-radius) table. Is that granular enough to be
   useful, or so coarse it collapses to a per-role constant?
3. Should local models participate in competence-gated routing at all before any local-model
   benchmark exists? Round-1 review flagged that "local cost ~0 but quality worse" is asserted
   without measurement.
4. Are per-model admission caps the right key when agents span providers with asymmetric
   quotas, or should caps be per-provider?

## 7. Relationship to the economizer safety spec

**This section was rewritten 2026-07-22 after discovering the spec had been superseded
mid-analysis.** Earlier drafts argued against `aimee-economizer-safety-v1` (off-only), quoting a
baseline clause pinning "the same model, endpoint, account/project routing, service tier,
region, and API shape"; that clause no longer exists; the live file is now:

- `provider-neutral-economizer-safety-spec.md`, retitled **"Provider-specific, proof-gated
  economizer safety specification"**
- **Version:** `aimee-economizer-safety-v2`, **State:** APPROVED, CONVERGED, dated 2026-07-22
- **Scope:** "OpenAI GPT-5.6-family and Anthropic Claude **request paths**"

Every argument in earlier drafts of this section, both the "operator-selected policy" framing
rejected at round 1 and the "sign of the cost effect" framing that replaced it, was made
against text that is no longer normative. Neither is retained.

### What v2 actually regulates

V2 authorizes economization only through "a provider-specific planner that proves the candidate
request has a strictly lower cost than sending the untouched request", with pass-through when
the proof cannot be completed. Its governing invariant is:

```text
candidate_call_cost_upper_bound + safety_margin < baseline_call_cost_lower_bound
```

computed "for the exact provider, **model snapshot or documented model family**, endpoint, cache
mode, breakpoint layout, account-visible pricing configuration, and tokenizer version", with
both sides using "the same pinned model snapshot and tokenizer identifier."

Its four authorized intervention classes are all **content transforms of a request**: reducing
newly produced tool output before first dispatch; reducing the mutable suffix after an explicit
breakpoint; emitting a deterministic representation; and reducing a request below a long-context
pricing threshold.

### Why delegate routing is outside that scope

The model is an **input to v2's proof, held fixed on both sides of the inequality**, not
something the economizer may vary. V2 regulates what aimee does *to a request once the model is
chosen*. Delegate routing decides *which agent handles a work packet*, upstream of request
construction, and produces no candidate-versus-untouched pair for the same model.

This is a scope argument, not a cost-direction argument. It does not depend on the competence
floor raising spend, and so it does not inherit the objection that this proposal's own §1
objective ("cheapest agent whose capability meets the requirement") is manifestly
cost-motivated. A cost-motivated design can still fall outside a spec that governs a different
mechanism.

### What remains genuinely open

1. Does the operator intend v2's authority to extend to model/agent selection in a future
   version? V2's title change from "provider-neutral" to "provider-specific" and its scope line
   ("request paths") both suggest not, but this proposal should not assume the answer.
2. If §5b's provider-general registration lands, a single registered provider spans several
   model snapshots. V2 requires proofs pinned to "the same pinned model snapshot". The
   economizer must therefore see the *resolved* model for a dispatch, not the provider-level
   agent. This is a real interaction between the two designs and belongs in Slice 0c's design.
3. Whether a competence floor that overrides a cheaper qualifying model needs to be recorded
   anywhere v2's accounting can see, so cost reporting does not misattribute the difference.

**Gate:** this proposal no longer treats §7 as blocking, because the superseded clause it was
blocking on does not exist. Item 2 above is a genuine design dependency and must be resolved
before Slice 0c, not before Slice 0.

### Process note

The spec was rewritten by another session while this analysis was in progress. Any future review
of this proposal must re-verify the spec version before relying on §7. The round-2 panel flagged
exactly this (it reported quote-accuracy as unverified because it could not read the spec file)
and that flag was correct even though the finding was discarded at replay verification.

## 8. Review history

- **Round 1 (2026-07-22, codex + MiniMax-M3, degraded: 0/2 seats used repository tools):**
  reviewed an earlier draft proposing allowlist-based *downgrade*. Blocking finding: the
  downgrade operation was vacuous because the router already routes cheapest-first. Confirmed
  against source and against the live server; the proposal was inverted in response. Two further
  blocking findings, scalar `cost_tier` inadequacy (addressed in §3.2) and the economizer-spec
  relabeling (§7, still open). Because no seat successfully used repository tools, no round-1
  finding certifies shipped behavior; the load-bearing one was independently verified by hand.
- **Round 2:** not yet run. Required before this leaves PENDING.


## 9. Delivery record (2026-07-23)

Implemented on branch `rewrite/go-server-wfe`, each slice reviewed by roundtable and
fixed before the next. Unit targets green throughout; ASAN/UBSAN/leak clean on the routing
and pricing code.

### Delivered

| area | what landed |
|---|---|
| Catalog identity | `agent_t.catalog_provider` separates vendor from wire shape; host-label URI parsing; CLI provider-name aliases (`claude`->`anthropic`, `chatgpt`/`codex`->`openai`) |
| models.dev | nested api.json reader (the downloaded cache previously resolved NOTHING); regenerated snapshot, 526 models; `lint` guard against rot |
| Pricing | three axes (cached/input/output), operator override per axis, context-band schedule, `cost_tier` vs price lint in `aimee doctor` |
| Routing | primary turn reaches its default seat; capability routing ON by default; fail-upward escalation; same-registration fallback preference |
| Provider-general | `"models": [...]` or `"models": "auto"` expands one registration into per-model targets with derived tiers |
| Identity | `provider:model` refs and catalog display names in roundtable attribution, the server projection (GUI) and the CLI |
| Scope ceiling | `max_scope` per agent + `--scope` per packet, forwarded and enforced server-side; routing never relaxes it (`agent_route_with_caps_scoped`) |
| Health demotion | prefer-healthy-over-degraded routing + trips-driven exponential breaker backoff (60s→30m), the fix for the six-day out-of-quota `codex` outage where a failing seat kept winning selection |
| Escalation | automatic verifier-driven re-dispatch RETIRED (see §11): now advisory: reports `escalation_warranted` + `suggested_escalation_target`, never re-dispatches |
| Docs / help | generated config reference for the `routing` section and the new agent fields; `DELEGATES.md` Routing section; `delegate --scope` in CLI help |

### Live defects found and fixed along the way

These were present before this work and are the reason it was worth doing:

- Every capability lookup for MiniMax and Kimi missed, so both lost
  `MODEL_CAP_REASONING` and ran on the SHORT per-call timeout, whose symptom the code
  itself documents as "slow completions cut off and retried as spurious read failures".
- `claude` and `codex` resolved **no capability flags at all** and an **8192-token output
  ceiling** (true: 128000), because their provider strings are CLI/product names, not
  catalog vendors.
- MiniMax-M3 resolved a 200k context window against a true 1M; kimi resolved **0**, which
  the gate then treated as passing.
- `agent_route()` made the premium default **unreachable for every role**, including on a
  user-facing primary turn.
- The bundled offline snapshot held 10 stale entries and not one current model.

### Not delivered

- **Slice 2 (competence axis.** NOT blocked on routing: §5c is corrected) declaring narrow
  `roles` plus an explicit `exec_roles` gives precise per-role routing today, proven by
  `test_declared_roles_route_precisely`. The live fleet is permissive by CONFIG (three of four
  agents declare `roles: ["all"]`, none declares `exec_roles`), not by defect. What is missing
  is the competence DATA to decide which model earns which role.
- **Slice 4, bandit reconciliation.** The existing `cheapest`/`premium` DB2 bandit
  (`server_compute.c`) is still capability-blind.
- **Registration-scoped health keys.** Health keys on the agent name, which is now unique
  per model, so a failing sol cannot mark terra down. Two registrations of the SAME
  provider with different credentials would still share model-level health.
- **Context bands are read but not consumed by cost decisions** beyond the lint.

### Validation gaps: stated plainly

- Full CI has never run on this branch.
- **Nothing is verified against the live server.** `aimee doctor` and `aimee agent list`
  both dispatch through `/v1`, so the tier lint and the server JSON projection are
  compile- and unit-verified only.
- `~/.cache/aimee/models_dev.json` does not exist on the development host, so a real
  refresh populating the cache has never been observed; the offline snapshot path is
  verified instead.
- The delegate-fallback path has no unit coverage (it dispatches for real); the
  same-registration ordering is verified through its exported prefix helper.


## 10. Role taxonomy: cull done, redesign sequenced (2026-07-23)

### Evidence

Sampled 40 delegate jobs across the full job-id range (2026-07-18..07-23, not just the current
session). Only FOUR role values appear in six days of real use:

| role | n | what the prompts actually ask |
|---|---|---|
| `review` | 31 | almost entirely one shape: *"Review the complete artifact against the complete original request; ARTIFACT STAGE: plan\|frozen_diff"* |
| `draft` | 5 | SWE-bench style bounded bug fixes, and best-of-N patch selection: not prose drafting |
| `code` | 2 | *"Implement the complete approved task in this worktree, run the repository verification"* |
| `roundtable` | 1 | adversarial panel review: **declared nowhere**, works by falling through |

Caveat: 40 jobs, one operator, six days. Enough to show what IS used; not enough to prove
what is never needed.

### Done

Culled the six persona-shaped roles (`prose`, `line-edit`, `lyric`, `hook`, `prosody`,
`songform`) and the two alias entries (`test`, `implement`). See the commit for the
persona-vs-role reasoning. `continuity` and `beat-check` kept: the novel persona genuinely
delegates them.

### NOT done, with reasons

An architect panel recommended culling `explain`, `execute`, `summarize`, `format`, `search`,
`diagnose` and folding `refactor` into `code`. **I did not act on that**, because the panel's
own precondition, "search all personas, agent declarations, callers, tests and configuration
before removal": fails when actually run: every one of those roles has 10–49 live code
references, and `refactor`, `execute`, `search`, `diagnose` and `validate` are declared by
built-in personas.

It also recommended culling `deploy` as having "no template, no route". **That is wrong**:
`cmd_hooks.c:485-491` assigns `role = "deploy"` from hook input, so it is routed. Kept.

### Sequenced redesign (not started)

1. **`artifact_gate` with a structured `stage`.** The strongest data-backed change: 31 of 40
   observed jobs are a staged artifact-vs-request gate whose stage lives in PROMPT TEXT, so
   routing cannot see the most informative signal in the sample. Must roll out atomically.
Add the role to every agent intended to receive it, THEN switch callers, or exact role
   filtering leaves jobs unroutable.
2. **`roundtable` becomes an orchestration MODE, not a role.** It describes how several agents
   are coordinated, not a capability one agent supplies. It is an undeclared production
   dependency today; migrate the invocation before removing the fall-through.
3. **A `scope` axis (`bounded` | `whole_task`) as routing METADATA, not roles.** The panel
   explicitly rejected `code_simple`/`code_complex`: scope is more stable and auditable than
   subjective difficulty (a single-file bug can be hard; a repo-wide mechanical change can be
   easy), and difficulty roles multiply combinatorially while coupling tiers to prompt labels.
   The observed data supports the distinction, bounded benchmark fixes versus whole-worktree
   implementation, but is too small to calibrate tier thresholds. Collect telemetry first.

Each step changes agent ELIGIBILITY, so none is a template-only edit, and none can be verified
from this worktree against the live server.

---

## 11. Transport reality: where scope and verify actually run

Found by trying to exercise escalation end to end against the live server, which is the only
way this surfaced. Every unit test passes with or without the defect.

**The deployed topology is a thin client talking to a remote server over `/v1`.** The thin
`aimee` binary links `CLI_SRCS`, which does NOT include `cmd_agent_delegate.c`; it carries no
delegate engine, no provider transports and no credentials, by design. `marshal_delegate`
forwards a fixed allowlist and silently drops everything else.

Consequences, all verified rather than inferred:

- `--verify` and `--scope` were **silently dropped** on every routed run. A caller got a normal,
  successful-looking result while the verifier never executed and the scope ceiling never bound.
- `agent_route_with_caps_scoped()` had **no production caller passing a real scope**,
`server_compute.c` read `required_caps`/`min_context` off the request and then called the
  *unscoped* router. The one place a ceiling could apply was the one variant nobody called.
- `verify_escalation_warranted()` / `agent_route_escalation_target()` are referenced **only** by
  `cmd_agent_delegate.c`, which no supported deployment invokes.

### Resolution (roundtable jobs 10864, 10885)

The two flags are structurally different and were wrongly conflated onto one transport path.

**`scope` is routing policy**. It names a ceiling, carries no caller-supplied code, and the
server already chooses the seat. Forwarded and enforced server-side; the effective ceiling is
logged with the placement so a ceiling that failed to bind is distinguishable from one that bound
and admitted the seat. Constraints accepted from the panel: caller scope may only NARROW what
operator policy permits, an unsatisfiable scope is an explicit error rather than silent widening,
and scope is evaluated together with capability gating so a ceiling cannot force a non-capable
placement.

**`verify` is caller-supplied code whose exit status is the sole evidence used to decide a model
was inadequate.** Honouring it server-side would both execute caller-supplied shell on a shared
server and hand whoever passes the flag control of escalation, and therefore of spend. Running it
client-side is not available: there is no delegate engine there. It is therefore **refused** on the
routed path rather than accepted and ignored.

### Decision: generic auto-escalation is retired (operator-approved)

The panel's recommendation, now actioned:

> Drop generic verifier-driven auto-escalation from the public `--verify` contract. Route
> correctly first; make retries explicit.

The argument: a verifier failure has many causes a dearer model will not fix, invalid tests,
environment problems, ambiguous requirements, an impossible task, so "the output failed a test"
does not establish "the seat was badly chosen", and automatic spend escalation is a poor default
response to that ambiguity. It also let whoever supplied the verify command decide when to spend.
This is consistent with the operator's own rule that over-selecting beats laddering: the scope
ceiling plus capability gating are what choose a sufficient seat on the first attempt, and a
failure afterwards is evidence the routing policy needs correcting.

**What changed.** `cmd_agent_delegate.c` no longer re-dispatches. On an attributable failure it
now REPORTS:

- `escalation_warranted`: the failure is attributable to the work product, so the placement is
  worth investigating. Routing telemetry, not an instruction.
- `suggested_escalation_target`: the seat a retry *should* use: genuinely dearer AND still
  eligible under this packet's scope and capability requirements. Absent when no such seat exists,
  which is itself the answer: the placement cannot be corrected by spending more.

`escalated` was removed rather than pinned false. Not for tidiness, `verify_outcome` and
`escalation_warranted` are both emitted unconditionally, so always-present is the house style, but
because it could only ever have been produced by the same in-process path, so no deployed caller
has ever read it and there is nothing to break. `delegate_snapshot_worktree()` went with it: it existed only to make a failed
automatic re-dispatch recoverable, and with nothing re-dispatching there is no partial-worktree
hazard to guard against. `worktree_may_be_partial` and `pre_escalation_snapshot` are gone for the
same reason.

**What was kept, deliberately.** `agent_route_escalation_target()`, the judgement of which seat
is genuinely dearer and still eligible, is the part worth keeping, and is what now backs
`suggested_escalation_target`. `verify_classify()` still separates an attributable work-product
failure from an unusable verifier, because that distinction governs whether the placement warning
is honest. `verify_escalation_warranted()`'s `already_escalated` argument was CUT: it existed only to stop the
automatic re-dispatch forming a ladder, and retaining it for a hypothetical future retry loop would
be exactly the speculative generality this codebase avoids.

**These advisory fields have no in-tree consumer today, and that is stated in the code.** They are
produced only by an in-process run with `--verify`, and that flag is refused on the server-routed
path, which is how every supported deployment invokes delegates. They exist so a human reading the
JSON, or a future operator-owned retry policy, can see the placement judgement without re-deriving
it. Naming that plainly is the point: the alternative is a field the next reader mistakes for a
contract something acts on.

If automated escalation returns, it should be an operator-owned recovery policy enabled narrowly
for specific task and failure classes, never a consequence of a caller-supplied flag.
