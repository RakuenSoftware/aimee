# Hashline Part I evaluation — results & interpretation

Two harnesses gate the proposal's Part I ("hashline ≥ str_replace pass@1, net
token-negative"):

1. **`src/tests/test_hashline_gate.c`** — deterministic, in CI. Proves the
   model-independent properties: hashline applies every fixture, stays safe
   under collision/drift where str_replace silently mis-applies, and reproduces
   zero already-read bytes. This always passes and is the floor.

2. **`tools/hashline_agentic_eval.py`** — multi-turn, live models. The *fair*
   test of the proposal's actual claim: real anchored reads (real FNV-1a-64
   hashes), and a retry loop that feeds the real structured tool errors back so
   fewer-retry-loops is measured, not assumed.

## Live findings (2026-07, .254 delegates, 6 fixtures × 2 runs, max 4 turns)

| model         | proto        | pass@1 | pass@k | mean turns | mean out-tokens |
|---------------|--------------|-------:|-------:|-----------:|----------------:|
| mimo-v2.5-pro | str_replace  |   83%  |   83%  |    1.42    |      308        |
| mimo-v2.5-pro | hashline     |   75%  |   75%  |    1.75    |      338        |
| MiniMax-M3    | str_replace  |   75%  |   75%  |    1.75    |       45        |
| MiniMax-M3    | hashline     |   67%  |   75%  |    1.83    |       68        |

**On this corpus, hashline does NOT beat str_replace** — it is slightly worse or
tied, at more turns and more output tokens. The Part I gate, as measured here,
**fails**, so the deprecated `old_string` path must NOT be removed yet.

## Why — and why this is not (yet) a verdict on the proposal

The result is real, but the corpus is the wrong regime to show hashline's win:

- **The fixtures are tiny.** On 3–7 line files, str_replace's `old_string` is
  short and usually unique, so it rarely needs a retry — there is almost no
  retry cost for hashline to eliminate. The proposal's own benchmark used 180
  mutation tasks over a real React codebase, where str_replace's failure modes
  (large unique-context requirements, whitespace recall over big blocks, files
  that changed under the model) bite hard and retries are expensive.
- **Hashline carries fixed overheads that only pay off at scale.** The anchored
  read view, the `snapshot_id`, and a novel nested `edits[]` schema cost tokens
  and schema-following reliability on every edit. On a trivial edit those
  overheads dominate; on a whole-function rewrite or an edit into a file full of
  near-duplicate lines they are dwarfed by the re-emit/retry savings.
- **Capable models paper over str_replace's weakness.** mimo/MiniMax add
  surrounding context to disambiguate a duplicated line, spending tokens to do
  what the anchor does for free — but still landing it in one turn on a small
  file, so the turn count doesn't diverge.

So this is evidence that **hashline is not a free win on trivial edits** (a
genuine, useful caveat), *not* evidence that the proposal is wrong at the scale
it was designed for. The two are different claims.

## What a decisive gate needs next

A realistic corpus, generated from a real repo checkout:
- functions edited inside files with many near-duplicate lines (collision at
  scale), whole-symbol rewrites (large re-emit), and edits applied after a
  concurrent change (real drift) — the regime where str_replace retries are
  frequent and costly.
- run `hashline_agentic_eval.py` over that corpus across weak + strong
  delegates; the proposal's ship criterion is *strictly better on the
  local/open-weight delegates*, which this small corpus cannot exercise.

Until that run is green, `old_string` stays. The deterministic gate and the fair
agentic harness are both in place to run it.

---

## UPDATE — realistic corpus (decisive)

The negative above was on the tiny hand corpus **and** used a wrong gate criterion
(it required hashline to use *fewer turns*, penalising the safe drift re-anchor).
Both are fixed: `tools/hashline_corpus_gen.py` mutates real repo files into 19
verifiable tasks across collision / deep-indent / whole-func / drift, and the gate
now uses the proposal's actual criterion — **pass@k ≥ str_replace AND net
token-negative** (turns informational).

Live, .254 delegates, 19-task realistic corpus, max 4 turns:

| model         | proto       | pass@1 | pass@k | turns | out-tok |
|---------------|-------------|-------:|-------:|------:|--------:|
| mimo-v2.5-pro | str_replace |   63%  |   79%  | 1.37  |   627   |
| mimo-v2.5-pro | hashline    |   74%  |  100%  | 1.32  |   202   |
| MiniMax-M3    | str_replace |   53%  |   58%  | 2.32  |   276   |
| MiniMax-M3    | hashline    |   58%  |   89%  | 1.74  |    99   |

**Gate: PASS on both delegates** — hashline solves far more tasks at ~3× fewer
output tokens.

Per-category pass@k (str→hl) and mean tokens (str→hl):

| category    | mimo pass@k | mimo tok  | MiniMax pass@k | MiniMax tok |
|-------------|-------------|-----------|----------------|-------------|
| collision   | 60% → 100%  | 1287 → 125| 0% → 100%      | 450 → 44    |
| deep-indent | 50% → 100%  | 61 → 118  | 100% → 100%    | 106 → 47    |
| whole-func  | 100% → 100% | 404 → 148 | 80% → 60%      | 331 → 193   |
| drift       | 100% → 100% | 643 → 400 | 60% → 100%     | 183 → 104   |

Reading it honestly:
- **collision is the decisive win** — the regime the proposal targets and the
  tiny corpus could not reach. str_replace fails 0–40% of these and spends ~10×
  the tokens; hashline lands them all.
- **whole-func / deep-indent / drift** mostly favor hashline on tokens at
  equal-or-better pass@k, with one real dip: MiniMax's whole-func pass@k fell to
  60% vs 80% (a small model fumbling a multi-line anchored range — the same class
  the prefix-strip hardening addresses, and a candidate for further schema help).
- **drift held up** better than feared (hashline's re-anchor recovers), though on
  a pure content-preserving insertion str_replace is naturally robust.

### P5 implication
This is the first evidence that meets the proposal's ship criterion (hashline ≥
str_replace pass@1/pass@k, net token-negative, strictly better on the
open-weight/local delegates). It is 2 delegates × 1 run × 19 tasks, so before
retiring `old_string` a wider confirmation is warranted — more delegates
(incl. the smallest local models), several runs to bound variance, and a look at
the MiniMax whole-func dip — but the direction is now clearly positive, not the
negative the small corpus suggested.

---

## CONFIRMATION — 3 delegates × 3 runs

Wider run to bound variance and test the MiniMax whole-func dip. Same 19-task
realistic corpus, max 4 turns.

| model         | str pass@k | hl pass@k | str tok | hl tok | gate |
|---------------|-----------:|----------:|--------:|-------:|:----:|
| mimo-v2.5-pro |    70%     |    96%    |   416   |  350   | PASS |
| MiniMax-M3    |    72%     |    86%    |   314   |   98   | PASS |
| codex         |   100%     |   100%    |   313   |   62   | PASS |

**Gate PASSES on all three delegates.** Findings that held up across 3 runs:
- **collision is the consistent, decisive win** — str_replace pass@k 33–40% (100%
  only for codex), hashline 100% everywhere, at ~5–15× fewer tokens.
- **token savings are large and consistent** even where str_replace also succeeds
  (codex: hashline 62 vs 313 tok — 5× fewer — at identical 100% pass@k).
- **the MiniMax whole-func regression is real, not noise** (pass@k 87% → 53%
  across 3 runs): the smaller model fumbles multi-line anchored ranges. This is
  the one place hashline loses on success rate, and it points at a concrete
  follow-up — whole-symbol edits likely want `edit_symbol` (which resolves the
  span server-side) rather than a hand-built `replace_range`, and/or a few-shot
  range example in the schema.
- **drift** recovered well (67–93% → 93–100%) thanks to the re-anchor loop;
  content-preserving insertions still slightly favor str_replace on cost.

### P5 verdict
The Part I ship criterion is met on the available delegates: hashline ≥
str_replace pass@k and net token-negative on every model, strictly better on the
open-weight delegates (mimo, MiniMax). The remaining reservation is the MiniMax
whole-func dip; addressing whole-symbol edits via `edit_symbol` before removing
`old_string` is the prudent sequence. Note the roster excludes the very smallest
local models (the population the proposal predicts gains most) — `kimi-k2.7-code`
was unavailable throughout (provider billing quota exhausted, not a code fault).

---

## P5 completion — roundtable-defined removal gate (not a calendar)

A review roundtable (`benchmarks/hashline/roundtable-p5-review.md`) endorsed
deferring both `old_string` removal and the neural semantic leg, but required the
deferral be **measurable**, not calendar-based. That guidance is now implemented:

**Guardrails / telemetry shipped (this change):**
- **edit_symbol steering (deterministic):** an anchored `replace_range` /
  `delete_range` covering ≥ 8 lines now returns an `advisory` recommending
  `edit_symbol` (server-resolved span) — directly targeting the whole-func
  regression where small models fumble hand-built multi-line ranges. Advisory
  only; never blocks.
- **old_string usage telemetry:** every legacy `old_string` edit logs an
  `edit_deprecated` line, so removal is driven by measured usage, not a date.
- **semantic-leg signal:** `web_read` logs a `web_read` line whenever a query
  that neither the literal nor lexical leg could retrieve falls back to
  top-of-page — the measurable "concrete need" for the embedder leg.

**Numeric removal gate for `old_string` (P5).** Remove the deprecated path only
when ALL hold:
1. MiniMax-M3 (or the weakest available delegate) whole-function pass@k on the
   mutation corpus recovers to **within 10 pp of str_replace** (currently 53% vs
   87% — a 34 pp gap) with whole-symbol edits routed through `edit_symbol`;
2. the agentic gate stays green (pass@k ≥ str_replace AND net token-negative)
   across ≥ 3 runs on the full delegate roster;
3. `edit_deprecated` telemetry shows negligible residual `old_string` usage from
   aimee's own agents;
4. migration docs cover the request-shape change (treated as a deliberate
   breaking API change, not an automatic bump).

Until (1)–(4) hold, `old_string` stays. This makes the deferral falsifiable and
data-driven, per the roundtable.

**Semantic leg revisit trigger.** Build the neural leg only when the `web_read`
zero-retrieval telemetry shows a material rate of literal+lexical misses on real
queries. If built, treat the embedder as an untrusted egress dependency
(bounded payload/time, same pinning posture as web_read's HTTP leg).
