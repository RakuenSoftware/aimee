# Three nested gold sets: small (1k), mid (3k), large (10k+)

Design for replacing the 70-note hand-authored gold set with a tiered corpus.
Written before implementation because three of the decisions below are expensive
to reverse once numbers have been published against them.

## The measured constraint that drives everything

At 10,000 notes nobody labels the gold. Not me, not the operator, not a panel of
delegates. So **gold has to be correct by construction, not by judgement**: the
generator composes a note from facts it already knows to be true, and the triple
falls out of the composition rather than being read back off the text.

That is only possible if the facts come from somewhere that already knows the
answer. Git does:

| source | count | fact it proves | predicate |
| --- | ---: | --- | --- |
| file renames (`--diff-filter=R`) | 1,418 | X is now called Y | `also_known_as`, `supersedes` |
| file deletions (`--diff-filter=D`) | 2,257 | X no longer exists | negation (empty gold) |
| (author, repo) pairs | 597 | P contributed to R | `member_of` |
| tags | 60 | v2 follows v1 | `supersedes` |
| distinct authors | 517 | real person names | subjects |
| commits | 18,413 | classifiable assertions | governance / transient |

A rename is not an opinion. `R097 src/backend/drm/compositor/dumb.rs ->
src/backend/drm/exporter/dumb.rs` is a fact with a diff behind it, and the gold
triple is mechanical. This is the difference between a 70-note set and a
10,000-note set: at 70 a human can label, at 10,000 the source must.

## Tier structure and the nesting property

    small  1,000   ⊂  mid  3,000   ⊂  large  10,000+

Each note carries a `tier` field (1/2/3). small = tier 1; mid = tiers 1-2;
large = all. Assignment is **stratified at generation time**, so every tier holds
the same domain and category proportions, taking a prefix of a generated file
would give small a skewed mix and make the tiers incomparable.

**Nesting is a feature and a trap.** The feature: a cheap small-tier run is
directly comparable to an expensive large-tier run, because small's notes are a
literal subset. The trap, which must be stated wherever these numbers are
published: **the tiers are not independent replications.** A result that holds on
small and then holds on large has not been confirmed twice. It is the same data
with more of it. Only the *added* notes are new evidence, and a bias in small
propagates to every tier.

## What each tier is for

| tier | n | CI half-width | E2B runtime @2.2s/note | use |
| --- | ---: | ---: | ---: | --- |
| small | 1,000 | ~±0.031 | ~37 min | iteration; does this change anything? |
| mid | 3,000 | ~±0.018 | ~1.8 h | confirmation before a decision |
| large | 10,000 | ~±0.010 | ~6.1 h | publication-grade, run rarely |

CI half-widths are extrapolated from the measured ±0.12 at n=69 by the 1/sqrt(n)
relation, not measured. They will be re-measured per tier once built.

Two consequences worth being blunt about:

- **Even 10,000 notes will not resolve everything.** The E2B Q4-vs-Q6 delta was
  +0.0012; at ±0.010 that is still noise. What large DOES buy is the 0.0155
  E4B Q6-vs-Q8 class of question, which is currently unresolvable.
- **Large is not a routine run.** One model, one config, six hours. The 14-model
  ladder at large is ~85 hours of GPU. Large exists for the numbers that go in
  front of users; small and mid carry day-to-day work.

## Three domains, because that is what the product spans

aimee bridges code, business and sales. The current 70-note set has **zero**
notes containing business or sales vocabulary, so two thirds of the product's
surface is untested. Target mix, applied within every tier:

| domain | share | entities | gold provenance |
| --- | ---: | --- | --- |
| code | 40% | real: 517 authors, 27 repos, 1,418 renames, 2,257 deletions | verifiable against git |
| business | 30% | synthetic but internally consistent | correct by construction |
| sales | 30% | synthetic but internally consistent | correct by construction |

The category taxonomy (`first_person`, `multi_fact`, `negation`, `transient`,
`ambiguous`, …) applies **within** each domain, because the failure modes differ
by domain. Sales notes are dense with non-durable optimism, "might close by
Friday", "the call went really well": and over-extracting a deal that never
closed is the graph-poisoning case that `transient` exists to catch.

Business and sales entities are invented. That is an external-validity limit and
it is not the same limit as invented *labels*: the risk of authored gold is that
it encodes one person's reading of an ambiguous case, and a generated
`Acme | customer_of | us` is not ambiguous. What synthetic entities cost is
realism, not correctness.

## The failure mode this design must not walk into

10,000 template-generated notes can be **easier** than 70 hand-written ones. If
every `first_person` note is "I work for {ORG}", a model learns the shape and
every score rises, the ladder compresses, and the benchmark loses the
discrimination it exists for. That would be worse than not expanding at all,
because the numbers would look better while meaning less.

Mitigations, all mandatory:

1. Many templates per (domain, category) cell, not one.
2. Large real entity inventories so surface forms vary independently of template.
3. Paraphrase and clause-order variation within a template.
4. **Explicit diversity gates**, checked before a tier is accepted: type-token
   ratio, near-duplicate rate over note text, entity reuse distribution, and
   distinct-template coverage. A tier that fails these is regenerated, not
   shipped.
5. The 70 hand-authored notes are **retained** in tier 1 and tagged
   `provenance: hand`, as an anchor. If generated notes score much higher than
   hand-authored ones for the same model, the generator has made the task easier
   and the gates missed it.

## Gold error is estimated, not assumed

Correct-by-construction does not mean correct. The generator can be wrong
systematically, a template that phrases a fact ambiguously produces notes whose
"correct" triple is not what a careful reader would extract.

So: an **audit subsample** of ~200 notes, stratified across domains and
categories, gets independently labelled, by the operator and/or delegates from
model families not under test, and the disagreement rate is reported as the
gold error estimate. No model on the leaderboard may label, for the obvious
reason.

That converts "one annotator, no inter-rater agreement" from a permanent caveat
into a number attached to each tier.

## Build order

1. `mine_entities.py`: entity and fact inventory from git and configs. Real.
2. `synth_entities.py`: business/sales inventory. Seeded, consistent.
3. `templates/`: per (domain, category) template bank.
4. `generate_gold.py`: seeded, stratified, emits `tier` and `provenance`.
5. `check_diversity.py`: the gates above; fails loudly.
6. `validate_gold.py`: extend the existing structural checks to tiers.
7. Audit subsample and the first inter-rater number.

Nothing is published against these sets until 5, 6 and 7 have run.
