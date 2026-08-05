# One sentence in my prompt turned a model's reasoning off

ROUGH DRAFT. The +0.116 figure below has no interval. That is stated where it
appears rather than at the end.

A 10,000-note extraction run finished in 34 minutes. I read that as a fact about
the hardware.

It should have taken about six hours. The model was not thinking. One sentence in
my system prompt had suppressed its reasoning pass, and the benchmark got faster
and worse at the same time.

## Speed is a diagnostic, and I was reading it as a result

The wrong number here is wall clock treated as a property of your setup. A run
that comes in far under estimate is telling you something about the run.

The thing that binds is the reasoning pass. On gemma-4 E4B, turning it back on
was worth **+0.116 relation-agnostic recall**. That figure carries no confidence
interval, because my bootstrap tool scores strict F1 only and I never extended it.
It is a single measurement on one model, and it is the number that decided the
whole project's prompt.

Flagging that: the largest single effect I have measured is also the one I have
been laziest about.

## Reasoning is a property of the run, not the model

Three shapes appear in this field of sixteen models.

**Suppressed by prompt.** E4B loses its reasoning pass to a prompt clause. E2B,
the same family, does not. I know of no way to predict which from the model card.

**Absent entirely.** granite and gemma-3n reason on zero rows in this harness.
Not reduced. Zero.

**Partial, and stable.** gemma-4 E4B under quantisation-aware training declines to
reason on 479 of 3,002 rows, 16%. 204 of those answer `{"facts":[]}` in five
tokens. The rate reproduces at two corpus sizes, so it is not a small-sample
artifact.

Those partial rows abstain at 51% against 24.5% on rows where the model did
reason. A model that silently loses its reasoning pass on a sixth of your input
scores as a worse model.

## The explanation I had was wrong, and its own test killed it

The obvious reading of the 16% is that reasoning starvation causes the deficit:
E4B scores below its own submodel under QAT, and here are the rows where it
skipped the work.

Restricted to the 2,523 rows where E4B **did** reason, it scores 0.6238 against
E2B's 0.6420. The gap is wider where it reasons, not narrower.

So the starvation is real, reproducible, and not the cause of anything I can
attribute to it. I do not know what drives those rows. It is not context length,
truncation, or an output-envelope problem: all three are zero across that arm.

That is an open question, not a caveat.

## An aggregate null hid a +0.24

The reason I now split every null by category comes from this subject.

Reasoning on against reasoning off aggregated to approximately nothing across the
corpus. Split by note category, it is **+0.24 F1 on one subset and −0.02 on
another**, cancelling.

A single number over a heterogeneous corpus can be the average of two effects
that point in opposite directions, and the corpus here is heterogeneous by design:
ten categories, from notes carrying three facts to notes carrying none.

I have since split the speculative-decoding pairs the same way, and there the null
survives: no category exceeds its own interval. That is the useful contrast.
Aggregate nulls are not all the same, and you cannot tell which kind you have
without splitting.

## What to do

**Count reasoning rows and print the percentage.** Not a flag. A percentage, per
arm. Mine is one field in the prediction row and I did not look at it for weeks.

**Treat an unexpectedly fast run as a bug report.** Six hours became 34 minutes
and I filed it as good news.

**Split every null by whatever strata your corpus has.** If it has none, that is
worth fixing before you trust any aggregate.

**Test your prompt against every model you rank, not the one you developed
against.** I know E4B suppresses and E2B does not. The other fourteen in my field
are unchecked, and a model that quietly loses its reasoning pass looks like a
worse model rather than a misconfigured one.

## What I have not done

The +0.116 needs an interval, which means extending the bootstrap tool to
relation-agnostic scoring. Until then it is one number from one model on one
corpus, and it is carrying more weight in this project than anything else with
that provenance.
