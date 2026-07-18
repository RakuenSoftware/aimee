# The aimee Economizer

The economizer reduces LLM token **cost** without degrading the model's context.
It has **one** user-facing control — a three-way tier — and applies a
**provider-aware** strategy underneath, because Anthropic and OpenAI reward
completely different economization.

```yaml
economizer: safe        # off | safe | aggressive   (default: safe)
```

```sh
aimee config set economizer aggressive
```

---

## The core invariant (why this is provider-aware)

**Anthropic prompt-caches on the exact bytes of the request prefix.** A cache *read*
is ~10× cheaper than re-sending the tokens; a cache *miss* costs the full price plus
a one-time *write* surcharge. So on Anthropic the cheapest thing aimee can do is
**not change the bytes** turn-over-turn.

This gives the single rule everything else follows from:

> **Once content has been sent to the Anthropic API, it is frozen** — every later
> turn must replay it **byte-identically**, or the cache misses and the "savings"
> cost *more*. **Before** content is first sent, aimee may transform it freely.

That is "freeze-on-first-send." It means aimee **can** compress, condense, or fold
Anthropic-bound context — it just has to do it **before the first send** and then
**freeze** the result. A tool output that is condensed the moment it is produced,
and thereafter replayed identically, shrinks every future turn's token count *and*
keeps the cache intact.

OpenAI's caching is automatic and far more forgiving of prefix changes, so on the
OpenAI family the economizer optimizes for **fewer tokens outright** rather than
byte-stability, and can apply lossier reductions.

Cross-protocol byte-identity (an `openai-wire` client whose request is translated to
Anthropic egress produces the *same* bytes an `anthropic-wire` client would) is
guaranteed by the canonical IR egress and is a prerequisite for all of the above.

---

## The three tiers

| Tier | Anthropic-bound context | OpenAI-family-bound context |
|---|---|---|
| **off** | no caching, no reduction — verbatim passthrough | no reduction |
| **safe** | prompt caching **+** deterministic, freeze-on-first-send **tool-output condensation** (lossless-on-demand: the full output is recall-restorable) | `tool_condense` **+** non-destructive fold-with-recall (only where fully restorable) |
| **aggressive** | prompt caching **+** everything in safe **+** more aggressive, still-frozen reduction (history fold + body compression) applied before first send | **+** lossy body compression **+** live `/v1` request mutation |

Rules that hold on **every** tier:

- **Anthropic egress is always byte-deterministic.** Whatever reduction a tier
  applies is applied *before* first send and *frozen* thereafter, so the cached
  prefix never shifts. A reduction that cannot be frozen cache-favorably (its
  savings over the freeze horizon do not beat the one-time cache-write cost) is
  **not** applied — the freeze-cost guardrail decides this per conversation.
- **Reduction never crosses providers.** OpenAI-only lossy passes never touch an
  Anthropic-bound request, and vice-versa; the strategy is keyed on the **egress
  provider**.
- **off is a true bypass.** No cache markers, no condensation — the request is the
  canonical egress of the verbatim context.

### What "condensation" vs "fold" vs "compression" mean

- **Tool-output condensation** — a recognized command's output (e.g. a directory
  listing, a large file read) is replaced by a compact, deterministic summary with
  the full body **recall-restorable on demand**. Lossless in effect; cache-safe
  because the same output always condenses to the same bytes.
- **History fold** — older turns are rolled into a compact skeleton. Lossy unless
  recall-restorable. On Anthropic it is only ever applied *before first send* and
  frozen (never re-folded into different bytes later).
- **Body compression** — boundary-free shrinking of large tool bodies. Aggressive
  tier only.
- **Live gateway mutation** — applies the reduced form to the live inbound `/v1`
  request. Aggressive tier, **OpenAI-family only.**

---

## Observability

Every economization is recorded so its impact is visible, never silent:

- **Ledger rows** in the token audit (`source="economizer"`, `usage_kind="avoided"`)
  carry the tokens removed, the baseline/reduced sizes, the reason, and the
  freeze/reuse decision — tagged with the active **`tier`**; the row's **`model`**
  identifies the egress provider. Surfaced together at `GET /v1/economizer/stats`.
- **Cache accounting** — `cache_read_tokens` / `cache_write_tokens` on each turn
  show the caching payoff directly.
- **Spend** excludes avoided tokens from the billable total and reports them as
  `avoided $X`, so the saving is legible.
- A conversation whose reduction was *suppressed* (guardrail said "not
  cache-favorable") is logged, so an operator can see the economizer chose not to
  act rather than silently doing nothing.

---

## Choosing a tier

- **off** — debugging, or a workload where you must guarantee the model sees the
  byte-exact verbatim context and pay full price.
- **safe** (default) — lossless. Anthropic gets prompt caching + recall-restorable
  tool condensation; OpenAI gets recall-restorable reduction. No information is ever
  lost to the model; cost drops with zero behavioral risk.
- **aggressive** — maximum cost reduction. Adds lossy folding/compression and, on
  OpenAI, live request mutation. Use when cost matters more than keeping the full
  verbatim history addressable.

---

## Migration from the old config

The previous `reduce.*` flags and the `economizer.enabled` / `economizer.aggressive`
two-tier switches are **removed**. Map old settings to the tier:

| Old | New |
|---|---|
| `economizer.enabled: false` | `economizer: off` |
| `economizer.enabled: true`, `economizer.aggressive: false` | `economizer: safe` |
| `economizer.enabled: true`, `economizer.aggressive: true`, `reduce.gateway_mutate: true` | `economizer: aggressive` |

The reduction tuning that used to live in `reduce.*` / `fold.*` / `closet.*` is now
an internal preset selected by the tier; there are no per-lever knobs.
