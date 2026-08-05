# Your benchmark needs repeatable, not identical

ROUGH DRAFT. The discriminating run for the last section is in flight.

A six-arm ladder over 10,000 notes at 44 minutes an arm is 44 hours. At three
processes with speculative decoding it is closer to nine. I took the nine, which
means I gave up bit-exact reproduction of a sequential run.

That is the right trade, and the reason is that identical was never the property
I needed.

## Identical to what

The question people ask about a nondeterministic-looking harness is whether it
reproduces. The question that decides whether your numbers mean anything is
narrower: does this configuration reproduce **itself**?

Three runs of the same three-process arm over the same 1,001 notes agreed
byte-for-byte on every note. That is the property a benchmark needs. Two arms
differing only in the model can then be attributed to the model.

Bit-identity with a *different* configuration is a separate property, and I do not
have it:

| comparison | byte-identical |
|---|---:|
| same arm, run three times | 1001/1001 |
| sequential, no MTP, fresh servers | 100/100 |
| with MTP | **74/100** |
| prompt cache on against off, same corpus | **792/1001** |
| same notes inside a 1,001 corpus and a 3,002 corpus | **529/1001** |

Every row below the first is a configuration change, and every one of them moves
output text. None of them moved F1 by more than its own interval.

## Three mechanisms, all boring

**Verification batching.** Speculative decoding pushes several tokens through the
target model in one forward pass instead of one. The batch shape changes, the
floating-point reduction order changes with it, and near-ties flip. 26 notes in a
hundred.

**Server warmth.** Fresh servers reproduce. Warm ones do not, because the prompt
cache carries state between requests that a cold server does not have.

**Corpus composition.** This is the one I did not expect. The same note, same
model, same quant, same process count, same prompt, produces different text
depending on which corpus it was embedded in. 47% of shared notes differ between
a 1,001-note run and a 3,002-note run that strictly contains it.

The obvious cause is the preceding note. It fails its own test: 44.8% churn with
the same predecessor, 48.3% with a different one. The cache holds roughly 38
entries, so what carries is the last 38 notes rather than the last one, and almost
every note has a different 38-note history between the two corpora. Uniform churn
is what that predicts and uniform churn is what appears.

**That is consistent with the mechanism and it is not a measurement of it.** The
run that discriminates, both corpora with the cache disabled, is on the card as I
write this, with the prediction registered before it started: near 1001/1001
agreement confirms the cache, anything near 79% withdraws the explanation.

## The cost of turning it off is zero, and I assumed otherwise for two days

I recorded the comparability run as unaffordable. The reasoning was sound: the
600-token system prompt is served from the cache, so disabling it re-evaluates
that prefix on every note.

Measured, the same 1,001 notes take 38 minutes with the cache off against 41 with
it on. Prefilling 600 tokens is noise next to two seconds of generation.

**Concede what this costs me:** I wrote "that is the article owner's call" into a
defect entry rather than spending 40 minutes finding out. The expensive-sounding
experiment was the cheap one, and I never checked.

## What to actually require

**Require self-reproduction, per configuration.** Run one arm three times before
you trust any arm. If it does not agree with itself, nothing downstream is a
measurement.

**Never compare across a configuration boundary.** An MTP figure and a sequential
figure are different configurations, not two measurements of one thing. Same for
a warm server against a cold one, and same for a 1,001-note run against 1,001
notes lifted out of a 10,000-note run.

**If you must compare across corpora, turn the cache off.** It costs about 7% of
wall clock, which I can now say because I measured it rather than reasoned about
it.

**Do not read output churn as accuracy movement.** Every configuration change
above rewrote between 21% and 47% of the text and none of them moved the score
outside its interval. Text churn and score churn are close to unrelated here, and
if you watch the wrong one you will chase noise for a week.

## The part I have not tested

Both identity measurements used the standard extraction prompt, which produces a
few hundred tokens. A configuration that drifts only on long generations would not
appear in either. I have not run that, so I cannot tell you it does not happen.
