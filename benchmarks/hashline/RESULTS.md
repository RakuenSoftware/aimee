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
