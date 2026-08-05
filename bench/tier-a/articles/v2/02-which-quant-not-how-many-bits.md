# Which quant beats how many bits

ROUGH DRAFT. Every figure below is measured. The corpus-independence limit in the
last section is the one I cannot close.

The question I set out to answer was whether to run Q4 and keep the VRAM, or pay
about 1.4 GiB for Q6. I got an answer, and then three other models made the
question look wrong.

More bits is not a direction. It is a property of the model you happen to be
holding.

## Three ladders, three shapes

| model | direction as bits increase |
|---|---|
| gemma-4 E4B | Q4 0.6301, Q6 0.6452, Q8 0.6337 |
| SmolLM3-3B | Q8 − Q4 = **+0.0352** |
| LFM2.5-2.6B | spread of **−0.0104** across the ladder |

gemma-4 rises then falls. SmolLM3 rises. LFM2.5 gets worse with more bits. Same
harness, same corpus, same prompt, same scorer.

If you take one thing: the ladder is cheap and your model's shape is not
predictable from anyone else's. Run it.

## A differently trained quant beat both framings

Google ships gemma-4 in a quantisation-aware trained build, legacy `q4_0`, weights
trained with quantisation in the loop. Everything else in this benchmark runs
unsloth's `UD-Q4_K_XL`, a post-hoc dynamic K-quant. Both are about four bits.

Same tier, same process count, same card, same MTP setting, same prompt. The only
difference is the quant scheme:

| model | QAT | UD | delta |
|---|---:|---:|---:|
| gemma-4 E2B | 0.6406 | 0.6017 | **+0.0389** |
| gemma-4 E4B | indistinguishable | | |

On the smaller model, quantisation-aware training is worth more than the entire
Q4-to-Q8 range of either ladder above. On its larger sibling it does nothing I can
measure.

I have two models. That is not a size trend, and I am not going to write one. The
mechanism is unmeasured and stays unwritten.

## The result that did not make sense, and did not survive more notes

E2B is architecturally a nested submodel of E4B, so E4B should dominate. Across
five head-to-head configurations it does, four times. Under QAT it went the other
way by −0.0213, with a paired bootstrap at n=1001 giving 95% CI [−0.0445,
+0.0015]. The interval barely contained zero.

A model losing to its own submodel is the kind of finding that is usually a bug,
so I ran both arms at 3,002 notes to tighten the interval:

| | E2B | E4B |
|---|---:|---:|
| strict F1 | 0.6416 | 0.6374 |

> E4B − E2B = **−0.0042, 95% CI [−0.0173, +0.0088]**

The anomaly was noise. The new interval excludes the old point estimate. Nothing
needs explaining, because there is no longer anything anomalous to explain.

**Concede the part that damages this:** at n=1001 I had a result that looked like
a real inversion, and the only thing separating it from a published finding was
that I distrusted it enough to spend three hours of GPU time. A ±0.024 interval
will manufacture a −0.021 effect regularly, and most benchmark tables at that
sample size do not print one.

## One number is two behaviours

The two QAT arms tie on F1 and are not the same model.

| | E2B | E4B |
|---|---:|---:|
| true positives | 1734 | **1800** |
| false positives | 1029 | **1206** |
| parse failures | 40 | 0 |

E4B finds 66 more real facts and invents 177 more. Higher recall, worse
precision, netted to zero by the metric.

The same shape holds on the 10,000-note UD arms, where the abstention rate
(how often a model correctly says nothing when the correct answer is nothing) is
0.674 for E2B and 0.578 for E4B, stable across all three quants.

If your pipeline writes into a knowledge graph and cannot tolerate invented
edges, those two models are not interchangeable, and the F1 column says they are.

## Correct for the floor before you argue about the gap

E2B's 0.6416 has 40 rows that failed to parse and contributed nothing. E4B's
0.6374 has none. One number is suppressed and the other is not, so comparing them
raw is comparing a floor to a capability.

Three corrections, all cheap:

1. Score both arms on the 2,962 rows E2B parsed: delta −0.0028, CI [−0.0158,
   +0.0103].
2. Bound the best case. Those 40 rows contain 15 gold facts in total and 26 of
   them have empty gold, so abstaining is correct. Perfect handling is worth
   **+0.0038**.
3. Repair the JSON. All 40 are structurally malformed rather than wrong, one
   missing a closing brace, one with an extra. I did not do this, because it
   means changing the scorer mid-campaign and correction 2 already bounds the
   gain below the noise.

Corrections 1 and 2 move the delta in opposite directions. Both sit inside the
interval. The conclusion holds three ways.

The general rule is the useful part: **state whether each F1 is a floor or a
capability, then bound the correction before arguing.** Here the bound is a tenth
of the noise. On other arms in this project the same check was the whole result.

## What to do

Run the ladder on your model and your corpus. It costs a few hours and the answer
does not transfer from anyone else's model, including a model from the same
family.

If your model has a QAT build, test it first. It was worth more than three bit
widths on one of my two models, and it is the cheapest thing on this list to try.

Print precision, recall and abstention next to F1. A tie in the last column can be
two models with different failure modes, and choosing between them is a decision
about which failure your system survives.

## The limit I cannot close

Every ladder above ran on one corpus, generated by one pipeline with one
generator model. The direction of a quant effect and the biases of that corpus
share a lineage, and I cannot separate them from inside.

The test is a second corpus built by a different pipeline and a different
generator, then the same ladders re-run. If the signs hold there, this piece gets
stronger. If they flip, the shared lineage was the whole story.

That is unmeasured, not disproven, and it is the largest open item in this work.
