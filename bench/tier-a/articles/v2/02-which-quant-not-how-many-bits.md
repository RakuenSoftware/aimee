# Which quant beats how many bits

DRAFT. The corpus-independence limit in the last section applies to every number
above it.

The question I set out to answer was whether to run Q4 and keep the VRAM, or pay
about 1.4 GiB for Q6. I have now run that comparison, and a much bigger one, across
six models from 2B to 31B.

Bit width turned out to be the least interesting axis available. **Who packed the
four bits moved the score more than adding four more bits ever did.** And the thing
quantisation-aware training actually bought me was not accuracy at all.

## The bit-width ladders, and how little they say

Corpus v5, 1,001 notes, prompt v8, one variable at a time. Paired bootstrap,
resampling notes rather than facts:

| comparison | delta | 95% interval |
|---|---:|---|
| gemma-4-E2B, Q6 − Q4 | +0.0065 | [−0.0145, +0.0272] |
| gemma-4-E4B, Q6 − Q4 | +0.0150 | [−0.0040, +0.0333] |
| SmolLM3-3B, Q8 − Q4 | **+0.0352** | [+0.0165, +0.0543] |
| LFM2.5-2.6B, Q4 − Q8 | +0.0104 | [−0.0153, +0.0366] |

One of four clears its interval, and it is the model that scores 0.3933.

The gemma ladder at 10,000 notes rises then falls: Q4 0.6324, Q6 0.6450, Q8 0.6321.
Q8 lands 0.0003 **below** Q4. Two more bits per weight, a bigger file, nothing to
show. More bits is not a direction.

I previously reported LFM2.5-2.6B as getting *worse* with more bits. Its interval
contains zero and that claim is withdrawn.

## Eight runs, eight times the same sign

The E2B Q4-to-Q6 interval crosses zero. Under the rule most of us were taught,
that ends it.

I had run that comparison before. Not once.

**It has been measured on five corpora across eight independent runs and come out
positive every time.** Different note sets, ontology versions, prompt revisions,
hardware on two of them. If the true effect were zero each run is a coin flip on
direction, and eight flips landing the same way is p = 0.008 by a sign test, using
nothing but direction and discarding every magnitude.

One objection would sink this. If a single run is mostly noise, eight agreeing
signs might be a stable quirk of the setup. So I measured it: three independent
runs of one arm in identical configuration, days apart with server restarts
between, produced **byte-identical completions on all 1,001 notes** and the same
strict F1 to four decimal places, 0.6138, every time. Re-running an arm does not
move it, so a sign is not a coin flip on run-to-run variation.

**If you are chasing small effects on a benchmark you control, replicate across
corpora before you buy sample size.** The sign is cheap and accumulates. The
interval is expensive and does not.

**And the concession that limits it:** my eight runs are not independent. They
share a prompt lineage, a scorer, and a corpus generation procedure. A systematic
bias in any of those produces the same sign every time for reasons unrelated to
quantisation. Read p = 0.008 as an optimistic bound.

## QAT: four sizes, one result, and it is not the one I expected

Google ships gemma-4 in a quantisation-aware trained build. The weights saw
quantisation during training. Everything else here runs a post-hoc quant. Both are
about four bits.

I ran the pair at four sizes. Same corpus, same prompt, same decoding, and for the
two large pairs the same draft head:

| size | QAT − non-QAT | 95% CI | |
|---|---:|---|---|
| E2B | **+0.0389** | [+0.0152, +0.0635] | holds |
| E4B | ~0 | | indistinguishable |
| 12B | +0.0100 | [−0.0091, +0.0289] | indistinguishable |
| 31B | +0.0108 | [−0.0013, +0.0235] | indistinguishable |

**Only the smallest model shows a QAT accuracy benefit that survives its own
interval.**

The 12B and 31B deltas are +0.0100 and +0.0108, almost identical, both positive,
both straddling zero. Two same-signed nulls are weak evidence of a small real
effect rather than none, and neither can be claimed alone. The honest reading is
that quantisation error stops mattering once the model is large enough to absorb
it, and QAT's accuracy case evaporates with it.

I wanted this to be a size trend and it is nearly the opposite of one.

## The packer beat the training

Google publishes its QAT weights as a flat legacy `q4_0`. Unsloth publishes the
*same* QAT weights repacked with their dynamic quant, which assigns bit width per
tensor by sensitivity. Same trained weights. Different four-bit packing.

| 26B-A4B, n=1001 | F1 | prec | rec | parse |
|---|---:|---:|---:|---:|
| unsloth QAT + dynamic | **0.6804** | 0.6501 | 0.7136 | 958/1001 |
| google QAT, flat q4_0 | 0.6575 | 0.6398 | 0.6761 | 940/1001 |

> **+0.0229, 95% CI [+0.0022, +0.0440], significant.**

Holding quantisation-aware training constant and changing only who did the packing
moved F1 more than QAT itself moved it at 12B or 31B. Almost all of it is recall:
628 true positives against 595. The per-tensor allocation is recovering facts the
flat quant drops, not trimming false positives.

That is the second of two quant comparisons in this project that clears its own
interval, and both of them are about *which* quant rather than how many bits.

**The caveat that damages it:** the two arms ran on different hardware, and my
rented-versus-local calibration bound is ±0.019 at n=1001, which nearly swallows
the lower edge of that interval at +0.0022. This wants a same-card rerun before it
carries anything on its own.

## What QAT actually bought me

Not accuracy. A hardware tier.

| model | QAT | non-QAT |
|---|---:|---:|
| gemma-4-26B-A4B UD-Q4 | **13.27 GiB** | 15.84 GiB |
| gemma-4-31B UD-Q4 | 16.10 GiB | 17.53 GiB |

My RTX 5080 has 15.92 GiB free after the server starts. The QAT 26B fits with room
for its draft head. **The non-QAT build of the same model does not fit that card at
all.** So on 16 GB the choice is not QAT versus non-QAT, it is QAT versus a
different model.

And it ran at **323 tok/s, the fastest arm in this entire project**, scoring
0.6804, indistinguishable from models three rows above it in the head-to-head.

QAT is also faster at equal size. Both 12B builds, sequentially, on one card, same
binary, same context, same draft:

| | tok/s | draft accepted |
|---|---:|---:|
| QAT | **285.7** | 85.4% |
| non-QAT | 233.1 | 84.2% |

**+22.6%**, with acceptance a point *higher* on the slower arm, so speculation
efficiency is not the cause. The QAT file is 6.26 GiB against 6.86, a 9% difference
that does not explain 23%. The candidate mechanism is that a dynamic quant assigns
different tensor types to QAT weights under the same `UD-Q4_K_XL` label, and
i-quant tensors dequantise more slowly than K-quants. Unverified.

That 1.7x I saw earlier between rented boxes was two thirds host variance. The same
arm measured 84.4 to 131.9 tok/s across five placements, which is why this had to
be run on one card to mean anything.

## Quant scheme does not touch speculative decoding

| arm | drafted tokens | accepted |
|---|---:|---:|
| 12B non-QAT | 1,510,235 | 82.0% |
| 12B QAT | 1,414,986 | 81.2% |
| 26B google q4_0 | 1,367,766 | 79.1% |
| 26B unsloth QAT+UD | 1,360,556 | 79.2% |
| 31B non-QAT | 620,046 | 78.5% |
| 31B QAT | 539,715 | 79.1% |

Acceptance tracks the model, not the quant. Every pair is within a point of itself
across schemes that differ by 0.023 in F1. **MTP and quantisation compose.** Pick
the quant on accuracy and file size, then turn speculation on separately.

## When two metrics disagree, stop spending memory

**E4B agrees with itself.** Q6 wins strict F1, wins relation-agnostic F1, abstains
more appropriately, emits 7 fewer spurious triples.

**E2B contradicts itself.** Q6 wins strict F1 by 0.0065 and loses relation-agnostic
F1 by 0.0052. It abstains less often and emits 11 more spurious triples.

When two views of the same predictions point opposite ways, the effect is smaller
than the difference between the metrics. A single reported F1 would have shown E2B
Q6 ahead by 0.0065 and said nothing about the rest.

## Correct for the floor before arguing about the gap

Two of the arms in this article have unreadable rows counted as failures, and it
changes what the numbers mean.

Both gemma-4-12B arms parse at 0.90 and 0.92 with **zero** rows at the context
limit. That is malformed JSON, not truncation. So the 12B QAT delta of +0.0100 is
computed between two floors, and I do not know that both floors sit the same
distance below their capability.

The procedure that works, from the E2B case where I did run it:

1. Score both arms on only the rows the worse one parsed. Moved the delta −0.0028.
2. Bound the best case. Those 40 rows held 15 gold facts and 26 had empty gold
   where abstaining is correct, so perfect handling was worth **+0.0038**.
3. Repair the JSON. Not done, because step 2 already bounded the gain below the
   noise.

Corrections 1 and 2 moved the delta in opposite directions and both sat inside the
interval. **State whether each F1 is a floor or a capability, then bound the
correction before arguing.** Here the bound was a tenth of the noise. On other arms
in this project the same check was the whole result.

## Check for a QAT build before you argue about bit width

- **Check for a QAT build before choosing a bit width.** On E2B it beat every
  bit-width step in this project. On a 16 GB card it decides which models exist.
- **Prefer a dynamic repack of QAT weights over the publisher's flat quant.**
  +0.0229 at 26B, the only large-model quant effect I can show.
- **E2B runs Q4.** The Q6 gain is real in sign and small, self-contradictory across
  metrics, and costs 1.4 GiB on disk. Not worth it.
- **E4B runs Q6.** Roughly 2.3 times the delta, agreement across every metric,
  better abstention.
- **At 12B and above, stop optimising the quant for accuracy.** Nothing there
  cleared an interval. Optimise it for what fits.

Note what that rests on. Not significance, which I mostly do not have. Direction
that has never reversed, magnitude differing between families, and metric agreement
in one family and not the other.

## One corpus, and three pairs split across two machines

**One corpus.** Every ladder ran on one pipeline with one generator model. A quant
direction and a generator artifact are indistinguishable from inside. The test is a
second corpus from a different generator with the same ladders re-run. Unmeasured,
not disproven, and the largest open item in this work.

**Cross-hardware pairs.** The 12B, 31B and 26B pairs each split across two machines.
Both QAT pairs are re-running now at n=3002 with each pair confined to one card,
which removes that term and should narrow the interval by about √3. The registered
prediction, written before the run finished: if the 31B point estimate holds, its
interval becomes roughly [+0.0036, +0.0180] and the null resolves. If it does not
hold, QAT does nothing at 31B and I will say so here.
