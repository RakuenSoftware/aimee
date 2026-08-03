# The benchmark was more correct than production

DRAFT. All measurements below are final. One structural caveat about corpus
reproducibility is stated in its own section rather than at the end. Open items
follow.

We built a benchmark to rank extraction models. It ranked them. It also turned
out to encode a specification of how a knowledge graph should behave, and that
specification disagreed with the system we were actually running.

On most of the disagreements we chased, the benchmark was right and production
was wrong.

That is not a happy accident. A benchmark's scorer has to decide when two answers
mean the same thing, and those decisions are a design document nobody thinks to
review. Ours had been accumulating good decisions for months while the production
path had not.

One section below breaks the pattern, and it is the one worth reading closely.
On the ontology, both sides were wrong in different directions: production
defined too few relations, and the gold set was not self-consistent either,
using `owns` and `owns_account` for the same meaning. "The benchmark is the
better specification" is a useful prior, not a rule, and the way you find out
which case you are in is by diffing them.

## Three names for one entity

`entity_name_normalize()` in production lower-cased the string and collapsed
whitespace. Nothing else.

So `Sunshine`, `Sunshine team` and `sunshine_team` were three distinct
`canonical_id` values, and a fact stored under any one of them was invisible to a
query about either of the others.

The benchmark's scorer had been folding separators, articles, honorifics and edge
punctuation for months, because otherwise it produced false negatives that were
obviously false. And the corpus was cloned **from production data**, so those
folds are not a convenience for grading. They describe how names actually vary in
the real note stream.

Which leads to the sentence that reframed the work: **tier-A extraction was
measuring cleaner than the graph it produced.** The model emitted a correct fact,
the scorer folded the name and credited it, and production filed the same fact
under a name nothing would ever resolve to. The benchmark could not see the
failure because the benchmark had already fixed it, for itself, locally.

The fix went into production, not the benchmark. The folding rule is deliberately
narrow, and the exclusions are the interesting part:

`van`, `gateway`, `router`, `server`, `box` and `project` are **not** stripped as
trailing descriptors, because product names contain those words. `Girder Gateway
van` and `Ingot Router` are entities, not entities plus noise.

The asymmetry is what justifies the caution. A fold you miss leaves two nodes,
and an alias can join them later. A fold you get wrong welds two real entities
into one, and there is nothing left in the data to undo it with. When in doubt,
under-fold.

## The migration that lost the memory it was fixing

Renormalising the entity registry required a migration, so we wrote one. It
merged the registry and stopped there.

`entity_edges` stores its endpoints as **text**, and recall matches them
literally. Both `db2_fact_recall_block` and `db2_fact_current_count` run
`WHERE source = ?` with no canonicalisation. A fact written under a display name
that lost the merge stayed filed under a name that no longer resolved to
anything: **present in the table, invisible to every query.**

Silent memory loss, shipped by a migration whose entire purpose was to stop
losing memory.

The shim test passed the whole time. It seeded a legacy alias row and no legacy
edge, so the exact condition the bug needs was never constructed. A Postgres
integration test written to seed precisely that failed on its first run.

Second time in one session that a real-backend test caught something the sqlite
shim could not. We now treat the shim as a syntax check rather than a behaviour
check for anything touching migrations.

## The ontology made the model guess a word

The seed ontology defined 17 relations. **19% of the gold set's own triples, 167
of 880 across 12 predicates, used relations it did not define**: `owns_account`
39 times, `subscription_tier` 39, `customer_of` 26, `purchased` 17.

The benchmark was making the model invent a predicate name and then grading it on
whether it invented the same one. The gold was not even self-consistent with
itself; both `owns` and `owns_account` appear in it.

The models mirrored the gap faithfully. Across two 1000-note runs, 22 to 24
percent of extracted facts used a non-seed predicate, spread over 89 distinct
names, 54 of which appeared exactly once.

The first thing we concluded about this was wrong. These facts were never
stranded: a NOVEL verdict still writes the edge, and recall filters on
`superseded_at` and `suppressed` rather than on relation class. The real cost is
fragmentation, which is slower and harder to notice:

| meaning | facts | split across |
|---|---:|---|
| hosting and deployment | 112 | runs_on 45, has_hostname 46, operates 16, hosts 5 |
| ownership | 89 | owns 59, acquired 30 |
| membership | 396 | works_for 205, member_of 167, contributes_to 24 |

And the auto-promotion rule, which makes a novel relation permanent once it
recurs three times, would have set this in concrete. Twenty-three of the 89
qualified. The ontology was on course to reach roughly 40 mostly-synonymous
entries with no way back.

We seeded seven relations and 15 aliases. What we refused to fold matters as much
as what we added: `owns` is too generic to be a target, `operates` and `runs`
describe running a **business** rather than running **on** a host, and
`contributes_to` is not membership. Each of those would have traded a
fragmentation problem for a precision problem.

Early result at n=223 under the expanded ontology: novel-predicate rate fell from
23.5% to 10.0%. Provisional, because that arm was interrupted before it
completed.

## Negation was information and we were throwing it away

`db2_fact_retract()` has existed since an early milestone, with bitemporal
supersede, refusal on immutable edges, and an authority guard so a model cannot
delete a fact a user stated directly. It is tested.

Nothing on the LLM path called it.

`fact_ingest.c` invoked it only from the pattern extractor, and only for
first-person attributes of the user. So "I no longer work at X" was handled by a
regular expression, while "Kestrel Freight isn't a customer any more", which is
the shape most third-party facts take, was dropped on the floor by a prompt that
told the model a retraction had nothing durable to record.

`member_of` is multi-valued, so nothing superseded it either. The edge stayed
active no matter how many notes said the relationship had ended.

The models were already doing the hard part. On the negation slice they either
emitted the correct triple with the polarity silently dropped, giving
`Kestrel Freight member_of customer`, or invented a negative predicate like
`removed_from` or `deleted_from`. Both outcomes were discarded.

The fix was to stop asking for a special retraction shape and instead ask for
**the original fact with a polarity flag**. That maps one-to-one onto the
existing API, and keeping the object is the point: `target` scopes the retraction
to a single edge, where a NULL target would retract every value of
`(source, relation)`.

Measured on two models, corpus v5, 1001 notes:

| | retractions flagged | usable by `db2_fact_retract` | polarity errors per 869 ordinary notes |
|---|---:|---:|---:|
| E4B | 115/132 | 92 | 0 |
| E2B | 85/132 | 85, every one it flagged | 1 |

Different failure profiles and both safe. E4B flags more and converts fewer. E2B
flags fewer and converts all of them. One error in 1738 non-retraction notes
across both, which is the number that mattered, because the risk of a polarity
flag is that it fires when it should not and deletes a true fact.

Relocations emit both halves correctly paired, retracting the old value and
asserting the new one, 85% of the time.

## The caveat that limits all of this

The corpus generator is seeded and deterministic, and its README says so. Its
`--inventory` and `--synth` input files were never tracked in git.

We tried all four surviving inventory files against the recorded seed. **Zero of
1001 notes reproduce.**

So corpus v5 was not regenerated from v4. It was derived by relabelling: 368
relation labels changed, zero note-text differences, zero id differences. That
happens to be useful, because stable ids and stable note text mean a v4
prediction file can be scored against v5 gold, which is how several comparisons
in this series were possible at all.

It is still a benchmark whose input artefact cannot be rebuilt from source. Every
number in these five articles inherits that limitation. We can re-score, and we
cannot regenerate.

## The pattern

Four production defects, all found by building a benchmark, none of them found by
running production.

The reason is that a scorer is forced to be explicit. To grade an answer, it has
to state when two names are the same name, when two predicates mean the same
thing, and what a negated fact is. Production never had to state any of that, so
it never did, and the gaps stayed invisible because nothing compared the two.

If you have a benchmark and a production path for the same task, the useful
exercise is not just running it. It is **diffing your scorer against your
pipeline**. Every normalisation the scorer performs and the pipeline does not is
a place where you are measuring a system you did not ship.

## What still needs measuring

1. **Finish the expanded-ontology arm.** The 23.5% to 10.0% novel-predicate
   result is n=223 from an interrupted run. It needs a full 1000-note arm before
   it belongs anywhere but a note.
2. **Track the corpus inputs.** Commit the inventory and synth files, or accept
   permanently that the corpus is a binary artefact and version it as one.
3. **Re-measure fragmentation after the ontology change.** The three families
   above should consolidate. Nothing has confirmed that they did.
4. **Polarity on more models.** Two models, one corpus. The one error in 1738 is
   the safety-critical number and it deserves a wider base.
5. **Audit the scorer for remaining folds production lacks.** We found the name
   normalisation gap by accident. A deliberate line-by-line diff has not been
   done.
