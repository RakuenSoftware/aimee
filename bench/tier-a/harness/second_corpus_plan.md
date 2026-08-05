# The second corpus: what it has to be, and what it can settle

Every ladder, null and ranking in this project ran on one corpus, from one
pipeline, with one generator model. A measured effect and an artefact of that
generator are indistinguishable from inside, and no amount of GPU time on the
existing corpus closes it. This is the largest open item in the work.

It is also the one that could move a headline rather than tighten it.

## The claim at risk

Article 02's spine is a sign test: the E2B Q4-to-Q6 step has come out positive on
five corpora across eight independent runs, p = 0.008 by direction alone. That
argument already states its own limit:

> my eight runs are not independent. They share a prompt lineage, a scorer, and a
> corpus generation procedure, and a systematic bias in any of those produces the
> same sign every time for reasons unrelated to quantisation.

A sign test assumes independence. Eight corpora from one generator are closer to
one observation than to eight. If the sign flips on a genuinely independent
corpus, the shared lineage was the whole story and article 02 loses its spine.

## What "independent" has to mean here

Three things must differ. Anything less is a fifth corpus from the same family.

**A different generator model.** It must not be a model under test. Every model
in the head-to-head is disqualified, because a corpus written by a model then
graded on that model's own phrasing habits is the bias this is meant to detect.
Pick something well outside the 230M-8B field: a 30B-class instruct model on a
rented card.

**A different entity inventory.** The original drew names, hosts and
organisations from one synthesised inventory. A new corpus needs its own, with no
shared surface forms, or the name-normalisation folds in the scorer will be
tuned to it by accident.

**A different template set.** This is the one that matters most and is easiest to
get wrong. The `runs_on` defect was a single template phrasing a hostname fact as
deployment, manufacturing 28 false negatives and 23 false positives per arm and
rewarding models that read the sentence wrong. Templates are where a corpus
encodes an opinion about language. Reusing them reproduces the opinion.

One thing does NOT need to differ: the ontology and the scorer. Changing those
changes what is being measured rather than what it is measured on, and the point
is to vary the corpus alone.

## What the lost inputs mean

The original generator was seeded and deterministic, and its `--inventory` and
`--synth` files were never committed. Zero of 1001 notes reproduce from the four
surviving inventory files. That is recorded as an unfixable defect.

For this purpose it cuts the other way: the new corpus **cannot** accidentally
inherit the old inputs, because the old inputs no longer exist. Independence is
easier to achieve than it would have been, and harder to verify, since there is
no baseline to diff against.

## Size, and why small is enough

This does not need 10,000 notes. It needs enough to resolve the sign of a
0.0065-0.015 effect, and the sign is cheaper than the magnitude: that is article
02's own argument turned on itself.

**Proposal: 1,001 notes**, matching gold_small so the tier machinery and the
scorer need no changes, and so the result is directly comparable in n to the
eight runs already banked.

Stratify to the same ten categories in the same proportions, including the 32%
factless share. A corpus with a different abstention fraction would move F1 for
reasons that have nothing to do with quantisation and would confound the very
thing being tested.

## The acceptance test, registered in advance

Run exactly one pair on the new corpus: **gemma-4-E2B at UD-Q4 and UD-Q6**,
nproc=1, no MTP, prompt v8, both arms on the same card in the same session.

- **Sign positive** (Q6 > Q4): the ninth independent observation, on the first
  corpus that does not share lineage. The shared-lineage caveat weakens
  substantially and article 02 gets stronger.
- **Sign negative**: the caveat was the whole story. Article 02's sign test is
  withdrawn, not softened, and the quant recommendation reverts to "measure it on
  your own corpus" with no direction attached.
- **Indistinguishable with a wide interval**: n=1001 was not enough to read the
  sign. Report that as a failed test rather than as support.

Magnitude is not the test. Direction is.

## Cost

| step | estimate |
|---|---:|
| generation, 30B-class model, ~1000 notes with gold | 2-3 h on one rented card |
| the E2B Q4 arm | ~40 min |
| the E2B Q6 arm | ~40 min |
| **total** | **under $1 at observed rented prices** |

The expensive part is not the GPU. It is writing a template set and an inventory
that are genuinely unlike the originals, and building gold triples that are
correct. Generated gold has to be checked, and the check is manual.

## What this does NOT settle

One second corpus tests the sign on one pair for one model family. It does not
generalise the whole series, and it does not retroactively validate the six-pair
MTP ladder, the head-to-head ranking, or the abstention findings, all of which
carry the same limit and would need the same treatment.

It settles the single claim most exposed to the limit, which is the right first
thing to spend it on.

## Status

Not started. Nothing above is measured; this is a design, and the numbers in the
cost table are estimates from observed per-arm costs rather than from a run.
