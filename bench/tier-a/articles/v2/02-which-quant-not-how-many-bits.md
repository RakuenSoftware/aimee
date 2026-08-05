# Which quant beats how many bits

ROUGH DRAFT. The corpus-independence limit in the last section is the one I
cannot close, and it applies to every number above it.

The question I set out to answer was whether to run Q4 and keep the VRAM, or pay
about 1.4 GiB for Q6. The measurement came back clean, paired, and inconclusive.
The confidence interval crossed zero.

Under the rule most of us were taught that is the end of it. Pick the cheap one,
move on. That would have been the wrong call, and why it is wrong turned out to
be worth more than the quantisation answer.

## The interval answers a question I was not asking

Corpus v5, 1,001 notes, prompt v8, three processes, one variable at a time:

| arm | strict F1 | relation-agnostic F1 | abstention | spurious triples |
|---|---:|---:|---:|---:|
| E2B Q4 | 0.6114 | 0.7728 | 0.689 | 101 |
| E2B Q6 | 0.6179 | 0.7676 | 0.661 | 112 |
| E4B Q4 | 0.6189 | 0.7421 | 0.556 | 146 |
| E4B Q6 | 0.6339 | 0.7556 | 0.578 | 139 |

Paired bootstrap, 5,000 replicates, resampling notes rather than facts:

| comparison | delta | 95% interval |
|---|---:|---|
| E2B, Q6 − Q4 | +0.0065 | [−0.0145, +0.0272] |
| E4B, Q6 − Q4 | +0.0150 | [−0.0040, +0.0333] |

Both contain zero.

A confidence interval on one run answers one question: if I re-ran this comparison
on a fresh sample from this corpus, how much would the delta move? It does not
answer whether the effect exists.

## Eight runs, eight times the same sign

I had run this comparison before. Not once.

**The E2B Q4-to-Q6 step has been measured on five corpora across eight
independent runs and has come out positive every time.** Different note sets,
different ontology versions, different prompt revisions, different hardware on
two of them.

If the true effect were zero, each run is a coin flip on direction. Eight flips
landing the same way is p = 0.008 by a sign test, using nothing but direction and
discarding every magnitude. That is stronger than any single 1,001-note interval
in the table above, and an analysis that looks at one run at a time cannot see it.

The E4B step has since replicated a ninth time at 10,000 notes, again positive.

There is one objection that would sink this if it held. If a single run is mostly
noise, eight agreeing signs might be a stable quirk of the setup rather than an
effect. I measured that rather than assuming it: three independent runs of one arm
in the identical configuration, days apart with server restarts between them,
produced **byte-identical completions on all 1,001 notes** and the same strict F1
to four decimal places, 0.6138, every time. Re-running an arm does not move it, so
a sign is not a coin flip on run-to-run variation.

**The operational point:** if you are chasing small effects on a benchmark you
control, replicate across corpora before you invest in sample size. The sign is
cheap and accumulates. The interval is expensive and does not.

**And the concession that limits it:** my eight runs are not independent. They share
a prompt lineage, a scorer, and a corpus generation procedure, and a systematic bias
in any of those produces the same sign every time for reasons unrelated to
quantisation. Read p = 0.008 as an optimistic bound. What survives without
qualification is that the direction is stable across every variation I have
introduced, and no run has reversed it.

## More bits is not a direction

Three ladders, three shapes:

| model | as bits increase |
|---|---|
| gemma-4 E4B at 10k | Q4 0.6324, Q6 0.6450, Q8 **0.6321** |
| SmolLM3-3B | Q8 − Q4 = **+0.0352** |
| LFM2.5-2.6B | spread of **−0.0104** across the ladder |

gemma-4 rises then falls. SmolLM3 rises. LFM2.5 gets worse with more bits.

The E4B Q6-to-Q8 step is −0.0129, and Q8 lands 0.0003 *below* Q4. Two more bits
per weight, a larger file, and nothing to show for it. By this article's own
argument, one run per arm settles nothing, so: **if you were planning to spend
disk on Q8, measure it first.** The step that paid was Q4 to Q6 and the step above
gave the gain straight back.

## A differently trained quant beat the whole bit-width axis

Google ships gemma-4 in a quantisation-aware trained build, legacy `q4_0`, weights
trained with quantisation in the loop. Everything else here runs unsloth's
`UD-Q4_K_XL`, a post-hoc dynamic K-quant. Both are about four bits.

Same tier, same process count, same card, same decoding setting, same prompt. The
only difference is the quant scheme:

| model | QAT | UD | delta |
|---|---:|---:|---:|
| gemma-4 E2B | 0.6406 | 0.6017 | **+0.0389** |
| gemma-4 E4B | indistinguishable | | |

On E2B, quantisation-aware training is worth more than the entire Q4-to-Q8 range
of any ladder above. On its larger sibling it does nothing I can measure.

The two also fail differently, which a single column hides. QAT parses slightly
worse on E2B, 992 of 1,001 against UD's 1,001, while scoring substantially better.
A quant comparison tracking F1 alone shows a clean win and hides that the winner
is marginally less well formed.

I have two models. That is not a size trend and I am not going to write one. The
mechanism is unmeasured and stays unwritten. What I will say is that the cheapest
untried thing on this list is checking whether your model has a QAT build, because
on one of my two it beat three bit widths.

## When two metrics disagree, stop spending memory

**E4B agrees with itself.** Q6 wins strict F1, wins relation-agnostic F1, abstains
more appropriately, and emits 7 fewer spurious triples. Every view points the same
way.

**E2B contradicts itself.** Q6 wins strict F1 by 0.0065 and loses
relation-agnostic F1 by 0.0052. It abstains less often and emits 11 more spurious
triples.

That disagreement is diagnostic. When two views of the same predictions point
opposite ways, the effect is smaller than the difference between the metrics, and
you should not spend memory chasing it. A single reported F1 would have shown E2B
Q6 ahead by 0.0065 and said nothing about the rest.

## The inversion that did not survive more notes

E2B is architecturally a nested submodel of E4B, so E4B should dominate. Across
five head-to-head configurations it does, four times. Under QAT it went the other
way by −0.0213, 95% CI [−0.0445, +0.0015] at n=1,001. The interval barely
contained zero.

A model losing to its own submodel is usually a bug, so I ran both at 3,002 notes:

| | E2B | E4B |
|---|---:|---:|
| strict F1 | 0.6416 | 0.6374 |

> E4B − E2B = **−0.0042, 95% CI [−0.0173, +0.0088]**

Noise. The new interval excludes the old point estimate.

**Concede what that costs me:** at n=1,001 I had a result that looked like a real
inversion, and the only thing between it and publication was distrust. A ±0.024
interval manufactures a −0.021 effect regularly, and most benchmark tables at that
sample size do not print one.

## One number is two behaviours

The two QAT arms tie and are not the same model.

| | E2B | E4B |
|---|---:|---:|
| true positives | 1734 | **1800** |
| false positives | 1029 | **1206** |
| parse failures | 40 | 0 |

E4B finds 66 more real facts and invents 177 more. The same shape holds on the
10,000-note UD arms, where abstention is 0.674 for E2B and 0.578 for E4B, stable
across all three quants.

If your pipeline writes into a knowledge graph and cannot tolerate invented edges,
those models are not interchangeable, and the F1 column says they are.

## Correct for the floor before arguing about the gap

E2B's 0.6416 has 40 rows that failed to parse and contributed nothing. E4B's has
none. One number is suppressed and the other is not.

1. Score both on the 2,962 rows E2B parsed: −0.0028, CI [−0.0158, +0.0103].
2. Bound the best case. Those 40 rows hold 15 gold facts total and 26 have empty
   gold where abstaining is correct. Perfect handling is worth **+0.0038**.
3. Repair the JSON. All 40 are structurally malformed rather than wrong. Not done,
   because it means changing the scorer mid-campaign and correction 2 already
   bounds the gain below the noise.

Corrections 1 and 2 move the delta in opposite directions. Both sit inside the
interval.

**State whether each F1 is a floor or a capability, then bound the correction
before arguing.** Here the bound is a tenth of the noise. On other arms in this
project the same check was the whole result.

## The decision I made

- **E2B runs Q4.** The gain is real and consistent in sign, and it is also small,
  self-contradictory across metrics, and costs 1.4 GiB on disk (2.97 against
  4.39). Not worth it.
- **E4B runs Q6.** Roughly 2.3 times the delta, agreement across every metric, and
  better abstention.
- **Check QAT before either.** On E2B it is worth ten times the Q4-to-Q6 step.

The pairing is the point. Memory saved on the small model funds the same quant
step on the large one, where it buys more than twice as much.

Note what that rests on. Not significance, which I do not have. Direction that has
never reversed, magnitude differing by 2.3x between families, and metric agreement
in one family and not the other. Three weak signals pointed the same way,
deliberately, rather than one strong signal I was never going to get at this
budget.

## The limit I cannot close

Every ladder here ran on one corpus, from one pipeline, with one generator model.
A quant direction and a generator artifact are indistinguishable from inside.

The test is a second corpus from a different pipeline and generator, with the same
ladders re-run. If the signs hold, this piece gets stronger. If they flip, the
shared lineage was the whole story.

That is unmeasured, not disproven, and it is the largest open item in this work.
