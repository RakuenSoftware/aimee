# Ten drafts

Rough drafts, one per distinct subject the measurement body supports. Written
against `/home/virant/dev/voice-guide/VOICE.md` (Part I and Part III).

The previous six drafts are untouched in `articles/`. Nothing there has been
deleted or superseded yet; that is a decision, not a cleanup.

| # | subject | evidence | state |
|---|---|---|---|
| 01 | speculative decoding, throughput against accuracy | 4 of 6 paired 10k arms, 1 bootstrap, per-category split | strongest; 2 arms outstanding |
| 02 | which quant beats how many bits | 3 ladders, QAT vs UD at 2 tiers, mid-tier resolution | strong |
| 03 | how a benchmark lies to its author | 5 defects, each with correction and the discarded column | strong, and self-implicating |
| 04 | repeatable, not identical | 5 identity measurements, cache cost measured | discriminating run in flight |
| 05 | six ways a run scores fine and is broken | 6 pathologies, 1 retracted | strong |
| 06 | reasoning is a property of the run | 3 shapes, the +0.24 cancellation | the +0.116 has no interval |
| 07 | the parallelism limit was never VRAM | 2 backends, 4 process counts, startup table | good; >4 processes untested |
| 08 | how small can an extractor be | 16 models | weakest; ranking needs rebuilding at n>=1001 |
| 09 | the corpus decides what you can find | composition, tiers, ontology, one bad template | strong; corpus not reproducible |
| 10 | the benchmark audited production | 5 structural defects | smallest n; defects are structural |

## What every draft carries

The corpus-independence limit. Every ladder, null and ranking here ran on one
corpus from one pipeline with one generator model, so a measured effect and a
generator artifact are indistinguishable from inside. Drafts 02 and 09 state it
as a limit on their own conclusions. It is the largest open item in the work.

## Known gaps in the drafts

- 01 needs E4B Q6 and Q8, plus bootstraps on five of six pairs.
- 04's mechanism claim is registered as a prediction, not a result, until the
  cache-off arms land.
- 06's largest figure, +0.116 recall, has no interval; the bootstrap tool scores
  strict F1 only.
- 08 should not publish until its ranking is rebuilt with per-gap bootstraps.
- 10's polarity figure is one error in 1,738, two models, one corpus.
