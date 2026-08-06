# How small can a fact extractor be

DRAFT. The full field table lives in the head-to-head piece. This one asks only the
size question.

If you want a local model to turn a note into structured facts, the first question is
how much model you have to buy. I measured twenty-two arms, from 230M to 35B.

The answer is smaller than I expected, and the reason it took me three attempts to
state correctly is worth more than the answer.

## Fifteen times the parameters is worth 0.047

Every arm below is 1,001 notes on the same corpus with the same prompt.

| model | params | F1 |
|---|---|---:|
| gemma-4-E2B | 2B | 0.6406 |
| gemma-4-26B-A4B | 26B total, ~4B active | 0.6804 |
| gemma-4-12B | 12B | 0.6854 |
| gemma-4-31B | 31B | 0.6872 |

> gemma-4-E2B → gemma-4-31B: **+0.0465, 95% CI [+0.0220, +0.0712]**

Real, and small. Going from 2B to 31B is worth less than half of what a *prompt
clause* was worth on one model in this same project, and about the same as what a
quant change was worth on another.

## The measurement that nearly said the opposite

I first tested it the obvious way: bootstrap each adjacent pair down the ranking.

| step | delta | 95% CI | |
|---|---:|---|---|
| 31B QAT → 12B QAT | −0.0017 | [−0.0202, +0.0162] | indistinguishable |
| 12B QAT → 26B unsloth | −0.0051 | [−0.0256, +0.0154] | indistinguishable |
| 26B unsloth → 31B non-QAT | −0.0041 | [−0.0258, +0.0176] | indistinguishable |
| 31B non-QAT → 12B non-QAT | −0.0009 | [−0.0197, +0.0180] | indistinguishable |
| 12B non-QAT → 26B google | −0.0179 | [−0.0434, +0.0071] | indistinguishable |
| 26B google → E2B QAT | −0.0168 | [−0.0406, +0.0070] | indistinguishable |

Six consecutive steps, 2B to 31B, not one of them separable. I wrote "size does
nothing on this task" in my notes and I was about to publish it.

Then I tested the ends against each other and got +0.0465 with the interval well
clear of zero.

Both results are correct. Each step carries an interval of roughly ±0.020, and six
of those stacked end to end have room to hide a real 0.047 with none of the
individual steps noticing.

**A size ladder compared rung by rung will always tell you size does nothing.** That
is the shape of the experiment, not a fact about models, and it is the default way
people run this comparison.

## What size actually changes is not the score

The 31B and the 12B are statistically identical on F1. They are not the same model.

| | F1 | recall | abstains on factless | invented triples |
|---|---:|---:|---:|---:|
| gemma-4-31B QAT | 0.6872 | **0.8000** | **0.463** | **180** |
| gemma-4-12B QAT | 0.6854 | 0.7330 | 0.702 | 97 |

The 31B finds the most facts in the entire field and invents nearly twice as many on
the 322 notes that assert nothing. Both 31B arms do this, so it is the model and not
the quant.

Scaling up bought recall and spent restraint. If your pipeline reviews what it
writes, that is a good trade. If it writes into a graph nothing will audit, it is a
bad one, and the F1 column shows neither.

## Architecture beat size, twice

The largest jump in the whole field is not a size step.

> gemma-4-31B QAT → Qwen3.6-35B-A3B: **+0.0386, 95% CI [+0.0194, +0.0577]**

That is a mixture of experts with roughly 3B active parameters beating a dense 31B by
more than the dense 31B beat a 2B.

And on throughput the same architecture choice dominates everything:

| model | active | tok/s |
|---|---|---:|
| Qwen3.6-35B-A3B | ~3B of 35B | **234.0** |
| Qwen3.6-27B dense | 27B | 67.8 |

**3.5 times faster, same family, same quant, same card class, writing the same
amount of text.**

So the size question splits in two. Total parameters decide what fits on your card.
Active parameters decide what it costs to run. A 26B MoE ran at 323 tok/s on a 16 GB
consumer card, faster than a 12B dense model on the same card.

## A mixture of experts is not a small model on disk

All experts stay resident. LFM2.5-8B-A1B at Q4_K_M is 5.16 GB and three copies do not
fit a 16 GiB card, so that arm ran at a different process count from the rest of the
field, which makes it incomparable to the ranking by construction. Process count is
worth about 0.0105 F1 here.

Sparsity buys bandwidth, not VRAM. Plan memory by total parameters and speed by
active ones.

## Below 2B, stop measuring capability and check the format

Four arms in my field score under 0.20, and they get there in different ways.

**LFM2.5-1.2B parses 0.73 and MiniCPM5-1B parses 0.87.** A quarter and an eighth of
their output is unreadable, with **zero** rows hitting the context limit, so this is
malformed JSON rather than truncation. Those scores are floors on models I have never
measured properly. That is a format disagreement, not a capability result, and until
I re-run them with a matched prompt they have no place in a ranking.

**LFM2.5-230M parses 1.00 and scores 0.1309.** Nothing is wrong with its format. It is
answering fluently and incorrectly.

A clean parse rate is not evidence of a working model, and a poor one is not evidence
of a broken one. Check which you have before you conclude anything about size.

## Half the field never reasons, and I only know why for one of them

Seven arms emit no reasoning pass at all in this harness. On gemma-4-E4B I traced it:
one sentence in my prompt, `No prose, no markdown.`, suppressed reasoning across
10,000 notes while every row still recorded `thinking: true`. Removing it restored
reasoning on 770 of 770 notes and was worth +0.116 relation-agnostic recall.

I have run that diagnostic on four models. Eighteen are unchecked. A model that
silently loses its reasoning pass scores as a worse model, so some unknown fraction
of the small end of my field is a prompt problem wearing the costume of a size
problem.

That is the largest open item in this piece, and it points the same way every time:
**before concluding a small model cannot do the task, check that your prompt let it
try.**

## Shortlist at 2B to 4B, then spend the VRAM somewhere else

**Shortlist at 2B to 4B, then check what a bigger card would buy.** In my field that
is +0.047 for fifteen times the parameters, and I would rather spend the VRAM on a
sparse 26B that runs at 323 tok/s.

**Decide on restraint, not on F1.** The size step changed invention rate by 1.9x and
F1 by 0.002.

**Budget for the ladder, not the model.** The cheapest real gains I found were a
quant change and a prompt clause, both larger than several size steps, and neither
transfers between models.

**Check parse rate and the unfloored score before believing a low one.** Four of my
arms scored near zero in three different ways: two emit valid JSON that is never the
right shape, and two extracted correctly and were emptied by a confidence gate.

**Cost spans a factor of 16 and is a separate axis.** CPU time per note runs from
2,233 ms at 230M to 35,230 ms at E4B. On GPU the largest E4B quant took 420 seconds
to load before serving its first note, and that appears in no accuracy column
anywhere.

## The sub-2B rows are the weakest thing here

1. **The sub-2B models re-run with matched prompts.** Two are floors and one is
   untested against its own format.
2. **The reasoning clause tested against the other eighteen.**
3. **Load time as a column.** I have observed it in passing and it is a real
   deployment cost.
4. **A second corpus from a different generator.** Every number here inherits one
   lineage, and a model trained on data resembling my generator has an advantage I
   cannot detect from inside.
