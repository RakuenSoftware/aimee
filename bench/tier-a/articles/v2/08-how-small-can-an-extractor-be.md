# How small can a fact extractor be

ROUGH DRAFT, and the weakest of the set. The ranking that made this piece
originally ran on 69 notes. The corpus-backed arms exist and the rebuild has not
been done. Read the numbers here as a hypothesis about order until that lands.

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
model scored a 0.14 parse rate against my schema. That is a format disagreement,
not a capability measurement, and I have not re-run it with a prompt matched to
its expected output. Until I do, it has no place in a ranking.

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

**Check parse rate before you believe a low score.** Two of my sixteen scored near
zero for reasons that were not capability.

## What this piece needs before publication

1. Rebuild the ranking on 1,001 notes minimum, with a paired bootstrap on every
   claimed gap.
2. Re-run the 350M model with a matched prompt or drop it.
3. Report load time per model as a column. I have observed it only in passing and
   it is a real deployment cost.
4. Confirm or withdraw the one family inversion I saw, where a smaller sibling
   outscored a larger one. At n=1,001 that pattern already dissolved once under a
   proper interval.
