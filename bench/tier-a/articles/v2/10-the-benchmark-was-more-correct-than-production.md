# I built a benchmark to rank models and it audited my production system

ROUGH DRAFT. The sample sizes here are the smallest in the series. The defects are
structural, which is why I am willing to write them up at n=70.

The benchmark ranked the models. It also turned out to encode a specification of
how a knowledge graph should behave, and my production system disagreed with it in
five places.

Production was wrong in all five.

## The wrong number is the model score

I built a scorer to compare extractors. To do that it had to decide what counts as
the same fact: when two entity names refer to one thing, what a retraction does to
a stored fact, which relations are legal, and what happens to a fact whose subject
was later renamed.

Every one of those decisions is a specification. I wrote a specification by
accident, then compared it to the running system, and the running system lost.

## One entity, filed under three names

Production stored the same organisation under three distinct entity records. The
scorer folded them, because a scorer that does not normalise names cannot compare
two models fairly.

The fold logic in the scorer was more correct than the fold logic in the system it
was built to evaluate. I found it by accident.

**A deliberate line-by-line diff of scorer against production has still not been
done.** That is the obvious next step and it is not scheduled.

## Retractions never reached storage

The extraction path produces negated facts. The write path did not carry the
negation through to the storage layer, so a retraction was recorded as an
assertion.

The benchmark exercises this properly: 1,318 of 10,000 notes are negation cases
where the correct output is no fact. Production had no equivalent test, because
the behaviour is invisible unless something checks that the fact is *absent*
afterwards.

## Polarity is the safety-critical number and it has one measurement

Across 1,738 evaluated facts, polarity was wrong once.

That is the number I would most want a second measurement of, and it is two models
on one corpus. A polarity error is not a mislabelled edge, recoverable downstream.
It is a stored fact asserting the opposite of what the note said.

**Stated as mine rather than as the world's:** I have not found a polarity failure
mode beyond that one instance. I have not looked hard, across enough models, to
tell you there is not one.

## A migration for one of these fixes stranded the facts it was fixing

The fix for the entity-fold problem shipped with a migration. The migration left
facts pointing at entity records that no longer existed.

Only real Postgres surfaced it. The test suite ran against a substitute that
accepted the orphaned rows without complaint.

**The rule that follows:** a migration touching identity gets tested against the
real engine, not a stand-in. The substitute is faster and it agreed with the
migration about something the real engine does not permit.

## The kind gate could not fire on the path that needed it

A validation gate on entity kind was unreachable from the LLM extraction path. It
guarded the older deterministic path only.

The extraction path is the one producing novel entities from free text, so it is
the path where a kind check earns its place. The gate existed, passed review, and
protected the code that needed it least.

## What to do

**Treat your scorer as a specification and diff it against production.** Mine
encoded name normalisation, negation handling, entity identity and relation
legality. Four of those disagreed with the running system.

**Test for absence, not just presence.** Retractions, negations and deletions are
invisible to a test that only asserts what should be there. A third of my corpus
tests absence, and that is the part that caught this.

**Run identity migrations against the real engine.** The substitute agreed with
the bug.

**Check every gate is reachable from the path you added last.** Mine guarded the
old path and the new one shipped without it.

## What this piece does not have

Sample size. Most of these were found during a 70-note era of the project, and the
polarity figure is two models on one corpus.

I am writing them up anyway because a structural defect does not need a large n:
a gate that cannot fire from a path fires zero times regardless of how many notes
you push through it. Where the claim is statistical rather than structural, the
polarity number, I have said so and I would not act on it alone.

The fragmentation fix has also not been re-measured. Three entity families should
consolidate after the ontology change. Nothing has confirmed that they did.
