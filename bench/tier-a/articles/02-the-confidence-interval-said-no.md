# The confidence interval said no and the effect was real anyway

DRAFT. The 1001-note numbers below are final. The 10,000-note replication is
partially complete: two arms banked, four still running. Open items at the end.

We wanted a small question answered. Given a fixed VRAM budget for a local fact
extractor, is it better to run Q4 and keep the memory, or pay about 1.4 GiB for
Q6?

The measurement came back clean, paired, and inconclusive. The confidence
interval crossed zero. Under the rule most of us were taught, that is the end of
it: no significant difference, pick the cheap one, move on.

That would have been the wrong call, and the reason is worth more than the
quantisation answer.

## The measurement

Corpus v5, 1001 notes, prompt v8, every arm under multi-token prediction at
concurrency 1, same card, one variable changed at a time.

| arm | strict F1 | relation-agnostic F1 | abstention | spurious triples |
|---|---:|---:|---:|---:|
| E2B Q4 | 0.6114 | 0.7728 | 0.689 | 101 |
| E2B Q6 | 0.6179 | 0.7676 | 0.661 | 112 |
| E4B Q4 | 0.6189 | 0.7421 | 0.556 | 146 |
| E4B Q6 | 0.6339 | 0.7556 | 0.578 | 139 |

Paired bootstrap over 5000 replicates, resampling notes rather than facts:

| comparison | delta | 95% interval |
|---|---:|---|
| E2B, Q6 minus Q4 | +0.0065 | [-0.0145, +0.0272] |
| E4B, Q6 minus Q4 | +0.0150 | [-0.0040, +0.0333] |

Both intervals contain zero. On this run, at this sample size, neither quant
step is distinguishable from noise.

## What the interval is actually telling you

A confidence interval on one run answers one question: if we re-ran this same
comparison on a fresh sample of notes from this same corpus, how much would the
delta move? For E4B it says the delta could plausibly land anywhere from -0.004
to +0.033. The measurement is compatible with no effect.

It does not answer the question we care about, which is whether the effect
exists. Those are different questions, and the difference is not pedantic.

We had already run this comparison before. Not once.

**The E2B Q4-to-Q6 step has now been measured on 5 different corpora across 8
independent runs, and it has come out positive every time.** Different note
sets, different ontology versions, different prompt revisions, different
hardware on two of them. Eight for eight, same sign.

If the true effect were zero, each run would be a coin flip on direction. Eight
flips landing the same way is p = 0.008 by a sign test, using nothing but the
direction and discarding every magnitude. That is stronger evidence than any
single 1001-note interval in the table above, and an analysis that only ever
looks at one run cannot see it at all.

The E4B step has since replicated a 9th time at 10,000 notes, again positive.

## The trap, stated plainly

A per-run interval answers **could this run have come out the other way**.

Replication answers **does this effect exist**.

For an effect whose true size is around 0.01 F1, a 1000-note run does not have
the resolution to answer the second question, and it will keep reporting "not
significant" no matter how many times you run it, because each run is judged
alone. You can spend a great deal of GPU time growing n and still learn nothing,
when running the same comparison against a new corpus and recording only the
sign would have settled it for a fraction of the cost.

This is the operational point of the article. If you are evaluating small
effects on a benchmark you control, replicate across corpora before you invest
in sample size. The sign is cheap and it accumulates. The interval is expensive
and it does not.

There is a real caveat attached, and it limits the claim rather than decorating
it. Our 8 runs are not fully independent. They share a prompt lineage, a scorer,
and a generation procedure for the corpora. A systematic bias in any of those
would produce the same sign every time for a reason that has nothing to do with
quantisation. A sign test assumes independence and ours is imperfect, so read
p = 0.008 as the optimistic bound rather than the number. What we can say
without qualification is that the direction is stable across every variation we
have introduced so far, and that no run has ever reversed it.

## Bigger quant is not uniformly better

The two families do not behave the same way, and this is where the decision
stops being a single number.

**E4B agrees with itself.** Q6 wins on strict F1, wins on relation-agnostic F1,
abstains more appropriately, and emits 7 fewer spurious triples. Every view of
the data points the same direction.

**E2B contradicts itself.** Q6 wins strict F1 by 0.0065, and loses
relation-agnostic F1 by 0.0052. It abstains less often and emits 11 more
spurious triples. The metrics disagree.

That disagreement is diagnostic. When two views of the same predictions point
opposite ways, the honest reading is that the effect is smaller than the
difference between the metrics, and you should not spend memory chasing it. A
single reported F1 would have hidden this completely: it would have shown E2B Q6
ahead by 0.0065 and said nothing about the fact that the same predictions look
worse under a slightly different lens.

## The decision we made

- **E2B runs Q4.** The gain is real and consistent in sign, and it is also
  small, self-contradictory across metrics, and costs 1.4 GiB on disk (2.97
  against 4.39). Not worth it here.
- **E4B runs Q6.** Roughly 2.3 times the delta, agreement across every metric,
  and better abstention behaviour.

The pairing is the point. Memory saved on the small model funds the same quant
step on the large one, where it buys more than twice as much.

Note what that decision rests on. Not significance, which we do not have.
Direction that has never once reversed, magnitude that differs by 2.3x between
families, and metric agreement in one family and not the other. Three weak
signals pointing the same way, used deliberately, rather than one strong signal
we were never going to get at this budget.

## What still needs measuring

1. **Finish the 10,000-note ladder.** E4B Q4 (0.6324) and E4B Q6 (0.6450) are
   banked and confirm the E4B direction a 9th time. E4B Q8 and all three E2B
   arms are still running. Until E2B Q4 and Q6 land at 10k, the E2B decision
   rests on 1000-note runs only.
2. **Test independence properly.** Generate a corpus with a different pipeline
   and a different generator model, then re-run the E2B pair. If the sign holds
   there, the shared-lineage caveat above weakens considerably. If it flips, the
   caveat was the whole story.
3. **Q8 on both families.** Currently unmeasured at 10k. If Q6 to Q8 is flat
   while Q4 to Q6 is not, the curve has a knee worth reporting.
4. **Report intervals and sign counts side by side** in the results tables. We
   drew the wrong conclusion from this data once before catching it, and the
   table format was part of why.
