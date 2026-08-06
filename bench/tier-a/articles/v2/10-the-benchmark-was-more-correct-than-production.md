# I built a benchmark to rank models and it audited my production system

DRAFT. The sample sizes here are the smallest in the series. Most of the defects are
structural, which is why I am willing to write them at this n.

The benchmark ranked the models. It also encoded a specification of how a knowledge
graph should behave, and my production path disagreed with it in four places.

Production was wrong in all four. One section breaks the pattern and it is the one
worth reading closely.

## A scorer is a design document nobody reviews

To grade an answer, a scorer must decide when two names are the same name, when two
predicates mean the same thing, and what a negated fact is. Production never had to
state any of that, so it never did. That is not a happy accident: it is what happens
when one side of a system is forced to be explicit and the other is not.

## Three names for one entity

`entity_name_normalize()` in production lower-cased the string and collapsed
whitespace. Nothing else.

So `Sunshine`, `Sunshine team` and `sunshine_team` were three distinct
`canonical_id` values, and a fact stored under any one was invisible to a query
about either of the others.

The benchmark's scorer had been folding separators, articles, honorifics and edge
punctuation for months, because otherwise it produced false negatives that were
obviously false. And the corpus was cloned **from production data**, so those folds
are not a grading convenience. They describe how names actually vary in the real
note stream.

Which is the sentence that reframed the work: **extraction was measuring cleaner
than the graph it produced.** The model emitted a correct fact, the scorer folded
the name and credited it, and production filed it under a name nothing would ever
resolve to. The benchmark could not see the failure because it had already fixed
it, locally, for itself.

The fix went into production rather than into the benchmark, and the exclusions are
the interesting part. `van`, `gateway`, `router`, `server`, `box` and `project` are
**not** stripped as trailing descriptors, because product names contain those
words. `Girder Gateway van` and `Ingot Router` are entities, not entities plus
noise.

The asymmetry justifies the caution. A fold you miss leaves two nodes, and an alias
can join them later. A fold you get wrong welds two real entities into one and
there is nothing left in the data to undo it with. **When in doubt, under-fold.**

## The migration for that fix lost the memory it was fixing

Renormalising the entity registry needed a migration. It merged the registry and
stopped there.

`entity_edges` stores its endpoints as **text**, and recall matches them literally.
Both `db2_fact_recall_block` and `db2_fact_current_count` run `WHERE source = ?`
with no canonicalisation. A fact written under a display name that lost the merge
stayed filed under a name that no longer resolved to anything: **present in the
table, invisible to every query.**

Silent memory loss, shipped by a migration whose entire purpose was to stop losing
memory.

The shim test passed throughout. It seeded a legacy alias row and no legacy edge, so
the exact condition the bug needs was never constructed. A Postgres integration test
written to seed precisely that failed on its first run.

That was the second time in one session a real-backend test caught something the
sqlite shim could not. The shim is now a syntax check rather than a behaviour check
for anything touching migrations.

## Negation was information and the pipeline threw it away

`db2_fact_retract()` had existed since an early milestone, with bitemporal
supersede, refusal on immutable edges, and an authority guard so a model cannot
delete a fact a user stated directly. It is tested.

Nothing on the LLM path called it.

`fact_ingest.c` invoked it only from the pattern extractor, and only for
first-person attributes of the user. So "I no longer work at X" was handled by a
regular expression, while "Kestrel Freight isn't a customer any more", which is the
shape most third-party facts take, was dropped by a prompt that told the model a
retraction had nothing durable to record.

`member_of` is multi-valued, so nothing superseded it either. The edge stayed active
no matter how many notes said the relationship had ended.

**The models were already doing the hard part.** On the negation slice they either
emitted the correct triple with the polarity silently dropped, or invented a
negative predicate like `removed_from`. Both outcomes were discarded downstream.

The fix was to stop asking for a special retraction shape and ask for **the original
fact with a polarity flag**, which maps one-to-one onto the existing API. Keeping
the object is the point: `target` scopes the retraction to a single edge, where a
NULL target would retract every value of `(source, relation)`.

Measured on two models, corpus v5, 1,001 notes:

| | retractions flagged | usable by `db2_fact_retract` | polarity errors per 869 ordinary notes |
|---|---:|---:|---:|
| E4B | 115/132 | 92 | 0 |
| E2B | 85/132 | 85, every one it flagged | 1 |

Different failure profiles, both safe. E4B flags more and converts fewer. E2B flags
fewer and converts all of them.

**One error in 1,738 non-retraction notes** is the number that matters, because the
risk of a polarity flag is that it fires when it should not and deletes a true fact.
Relocations emit both halves correctly paired 85% of the time.

Stated as mine rather than as the world's: I have not found a polarity failure mode
beyond that one instance, and I have not looked across enough models to tell you
there is not one.

## The section where the benchmark was also wrong

The seed ontology defined 17 relations. **19% of the gold set's own triples, 167 of
880 across 12 predicates, used relations it did not define**: `owns_account` 39
times, `subscription_tier` 39, `customer_of` 26, `purchased` 17.

The benchmark was making the model invent a predicate name and then grading it on
whether it invented the same one. And the gold was not self-consistent: both `owns`
and `owns_account` appear in it.

So both sides were wrong in different directions. Production defined too few
relations; the gold set was not coherent either. **"The benchmark is the better
specification" is a useful prior, not a rule**, and the way you find out which case
you are in is by diffing them.

My first conclusion about this was also wrong. I assumed those facts were stranded.
They were not: a NOVEL verdict still writes the edge, and recall filters on
`superseded_at` and `suppressed` rather than on relation class. The real cost is
fragmentation, which is slower and harder to notice:

| meaning | facts | split across |
|---|---:|---|
| hosting and deployment | 112 | runs_on 45, has_hostname 46, operates 16, hosts 5 |
| ownership | 89 | owns 59, acquired 30 |
| membership | 396 | works_for 205, member_of 167, contributes_to 24 |

And the auto-promotion rule, which makes a novel relation permanent once it recurs
three times, would have set that in concrete. Twenty-three of 89 novel names
qualified. The ontology was on course to about 40 mostly-synonymous entries with no
way back.

I seeded seven relations and 15 aliases. What I refused to fold matters as much:
`owns` is too generic to be a target, `operates` and `runs` describe running a
**business** rather than running **on** a host, and `contributes_to` is not
membership. Each would have traded a fragmentation problem for a precision problem.

Early result at n=223 under the expanded ontology: novel-predicate rate fell from
23.5% to 10.0%. Provisional, because that arm was interrupted.

## Read the scorer as the specification it already is

**Diff your scorer against your pipeline, line by line.** Every normalisation the
scorer performs and the pipeline does not is a place where you are measuring a
system you did not ship. I found my first one by accident and the deliberate diff
still has not been done.

**Test for absence.** Retractions, negations and deletions are invisible to a test
that only asserts what should be there. A third of my corpus tests absence and that
is the part that caught the retraction gap.

**Run identity migrations against the real engine.** The substitute agreed with the
bug.

**Check your gold against your own ontology before ranking anything on it.** A
predicate you did not define is a model penalty you did not intend, and 19% of mine
were undefined.

**When you fold, under-fold.** The error you can undo is better than the error you
cannot.

## A structural defect does not need a large n, and one claim here is not structural

Sample size. Most of these were found during a 70-note era, and the polarity figure
is two models on one corpus.

I am writing them up anyway because a structural defect does not need a large n: a
gate that cannot fire from a path fires zero times regardless of how many notes you
push through it. Where the claim is statistical rather than structural, the polarity
number, I have said so and would not act on it alone.

The fragmentation fix has also not been re-measured. Those three families should
consolidate after the ontology change. Nothing has confirmed that they did.
