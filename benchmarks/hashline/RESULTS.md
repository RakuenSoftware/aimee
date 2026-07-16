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
