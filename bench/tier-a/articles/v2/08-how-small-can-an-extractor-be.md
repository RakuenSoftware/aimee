# How small can a fact extractor be

ROUGH DRAFT. The full field table lives in the head-to-head piece; this one asks
only the size question. The original ranking behind it ran on 69 notes, and the
corpus-backed rebuild has not been done, so read any ordering here as a hypothesis.

If you want a local model to turn a note into structured facts, the first question
is how much model you have to buy. I measured sixteen, from 230M to 8B.

The answer is smaller than I expected, and the reason the answer is unreliable is
more interesting than the answer.

## The wrong number is parameter count

The field does not sort by size. A 2B model and its 4B sibling, where the smaller
is architecturally a nested submodel of the larger, land within 0.005 F1 of each
other at 3,002 notes. A 3B model gains **+0.0352** from a quant change, which is
larger than most of the size gaps I care about.

What binds is not how many parameters. It is whether the model reasons, whether
your prompt lets it, whether the quant you picked suits it, and whether it knows
when to say nothing.

## The 69-note table and what it could resolve

The original ranking spanned 0.000 to 0.748 across sixteen models. At n=69 the
interval is near ±0.12, derived from the ±0.03 measured at 1,001 notes and scaled
by the square root of n.

Resolvable at that size: the gap between models producing no usable output (0.00 to
0.21) and models above 0.55; the schema-rate failures, which are categorical; and
the CPU cost spread, which is throughput rather than accuracy.

Not resolvable: any ordering among the five models between 0.50 and 0.65, and both
apparent size inversions.

I know that limit is real rather than theoretical because I walked into it. A
separate claim on this benchmark, that the reasoning pass is worth +0.084 F1, came
from a 70-note measurement with no interval. It became a constant quoted in three
source files and shaped a design decision. Re-measured at 955 notes it was +0.0103,
interval spanning zero.

## What I can defend right now

**Small works.** Models in the 2B to 4B range do this task at a level that is
useful, and the gap to anything larger in my field is smaller than the gap between
two quants of the same model.

**A mixture of experts is not a small model operationally.** 8B total with about
1B active still resides in full: 5.16 GB at Q4, three copies of which do not fit a
16 GiB card. It runs at a different process count from the rest of the field,
which makes it incomparable to the ranking by construction, and process count is
worth about 0.0105 F1 here.

**Sub-1B needs a matched prompt before it needs a verdict.** One 350M extraction
model scored 0.070 at a 0.14 JSON parse rate against my schema. That is a format
disagreement, not a capability measurement, and I have not re-run it with a matched
prompt. Until I do it has no place in a ranking.

**One family inversion survived and I cannot explain it.** Qwen3.5 at 0.8B scores
0.438 and at 2B scores 0.324, inside a single family at identical settings. It
appeared consistently across the conditions I ran. It is also well inside the ±0.12
that 69 notes supports, so it is reported rather than claimed.

Two nearby comparisons look like the same finding and are not, and separating them
matters more than the headline. granite-4.0-1b at 0.592 against granite-4.0-h-1b at
0.507 is not a size result: both are 1B and the `h` is an architecture variant, so
that gap answers which 1B to pick. And granite-4.0-1b against granite-4.1-3b inverts
**only under the confidence floor**, which discarded 13 of the 3B's facts against
the 1B's 2. That pair measures the gate.

## Why the order is a hypothesis

Three separate problems, and I would not publish the table without fixing all
three.

**The sample size.** The original ranking is 69 notes. The interval at that size
swallows most of the field. I now have arms at 1,001 and 10,000 notes, and the
rebuild is analysis rather than GPU time.

**The threshold.** I spent a campaign using 0.0105 to decide whether a gap was
real. That number is a measured effect of process count from an unrelated
experiment, not an interval. The real interval at n=1,001 is near ±0.024. Every
ranking gap between those two figures is unsupported until it gets its own paired
bootstrap.

**The comparison class.** Some arms were run natively at 1,001 notes and ranked
against a field extracted from 10,000-note runs. Those are not the same
measurement: the same notes score −0.0079 differently depending on which corpus
they ran inside, with 47% of the output text differing. The effect is inside the
interval, so nothing moved. That was luck.

## The part of the table I trust least

Every number here predates fixes to the benchmark itself. A corpus template that
mislabelled a deployment relation. An ontology that did not define 19% of its own
gold's predicates. A prompt clause that suppressed one model's reasoning pass
entirely, worth +0.116 recall on that model.

Each of those changed scores. None of them changed scores uniformly across models,
which is the problem: a fix that helps reasoning models and not others reorders
the field.

## What to do until the rebuild lands

**Shortlist on size, decide on your own corpus.** Two to four billion parameters
is where to look. Which one is not a question my table can answer for you yet, and
possibly not at all, because the quant and prompt effects here are larger than the
model effects.

**Budget for the ladder, not the model.** The cheapest real gain I found was a
quant change, and it does not transfer between models in the same family.

**Check parse rate and the unfloored score before you believe a low score.** Four of
my sixteen scored zero, and they did it in three different ways: two emit valid JSON
that is never the right shape, and two extracted correctly and were emptied by a
confidence gate.

**Cost is the other axis, and it spans a factor of 16.** CPU time per note runs from
2,233 ms at 230M to 35,230 ms at E4B. granite-4.0-1b reaches 0.592 at 13,568 ms and
gemma-4-E4B reaches 0.748 at 35,230 ms: 2.6 times the CPU for 0.156 more F1, and the
0.156 is the number in that sentence with a wide interval on it. A second cost
appears in no accuracy column at all, and on GPU the largest E4B quant took 420
seconds to load before serving its first note.

## What this piece needs before publication

1. Rebuild the ranking on 1,001 notes minimum, with a paired bootstrap on every
   claimed gap.
2. Re-run the 350M model with a matched prompt or drop it.
3. Report load time per model as a column. I have observed it only in passing and
   it is a real deployment cost.
4. Confirm or withdraw the one family inversion I saw, where a smaller sibling
   outscored a larger one. At n=1,001 that pattern already dissolved once under a
   proper interval.
