# Eight ways a run scores fine and is broken

DRAFT.

Every failure below produced a number, most of them plausible. One produced a rank.

If you are ranking local models on one column, this is what that column does not
carry.

## F1 is not blind, and I published that it was

F1 is not blind. I published that it was, and it was wrong.

I found three note categories scoring exactly 0.0000 in every arm, all of them
factless notes where the correct answer is no facts, and I wrote it up as a
structural hole: a third of the corpus that can only cost points, correct
restraint worth nothing.

Two checks killed it. The scorer already emits `null` rather than 0.0 for those
categories, with a comment explaining that printing 0.0 inverts the meaning and
ranks perfect restraint as the worst result. My analysis script printed 0.0 and
reintroduced the exact bug the scorer was written to avoid.

And the rows are scored. Zeroing their false positives is worth **+0.040 to
+0.053 F1** across six arms, an order of magnitude larger than any effect I had
been arguing about.

So the metric prices over-extraction correctly. What follows is not a case against
F1. It is a case against reporting it alone.

## One: the answer arrived in a tool-call envelope and scored zero

A model wrapped its output in a tool-call structure instead of returning the
schema. The extraction inside was fine. The parser saw nothing it recognised and
scored near zero.

Read as a ranking, that model is incapable. Read as a run, the harness and the model
disagreed about the envelope.

**The tell:** parse rate collapses while completion tokens stay normal. A model that
cannot do the task produces short or empty output. A model in the wrong envelope
produces full-length output that scores nothing.

## Two: the context ran out and every guard said the run was clean

A run exhausted its context window. The truncation flag never fired, because the
flag compares completion tokens against the request maximum, and a request that
dies against the *context* limit never reaches that maximum. The condition was
unreachable by construction.

**The fix:** compare prompt tokens plus completion tokens against the context
size. Do not compare completion tokens against `max_tokens`.

## Three: a model stopped reasoning and nobody noticed

One sentence in the prompt suppressed a model's reasoning pass entirely. Its
score dropped and it read as a worse model.

The general form is worse: reasoning is a property of the run, not of the model,
and it is invisible unless something counts it. In this field granite and
gemma-3n reason on zero rows.

And it is not binary. gemma-4 E4B under QAT declines to reason on a stable 16% of
rows, 479 of 3,002, of which 204 answer `{"facts":[]}` in five tokens. Those rows
abstain at 51% against 24.5% on rows where it did reason.

**The part that refuted my explanation:** restricted to the rows where E4B did
reason, it scores 0.6238 against E2B's 0.6420. It is worse precisely where it
reasons. Reasoning starvation is real, reproducible at two tiers, and not the
cause of the gap. I do not know what is.

## Four: the score was a floor and I compared it to a capability

One arm had 40 rows fail to parse. They contributed nothing, so its F1 is a lower
bound. The arm it was compared against had zero parse failures, so that F1 is
capability.

The large-model field made this worse rather than better. Both gemma-4-12B arms parse
at **0.90 and 0.92**, with **zero** rows hitting the context limit, so that is
malformed JSON and not truncation. They sit second and sixth in a twenty-two arm
ranking whose top and bottom neighbours parse at 1.00. Between 83 and 98 unreadable
rows are being counted as failures against models carrying no such handicap, and the
published gap between them is overstated by an amount I cannot currently quantify.

**Bound the correction before arguing about the gap.** Those 40 rows contain 15
gold facts total, and 26 of the 40 have empty gold where abstaining is correct.
Perfect handling is worth **+0.0038** against an interval of ±0.013. Here the
floor is a caveat. On the context-exhaustion arms above, the same check was the
entire result.

## Five: two models tied and behaved differently

The clearest one, and the large models state it more sharply than the small ones did.

| | F1 | recall | abstains on factless | invented triples |
|---|---:|---:|---:|---:|
| gemma-4-31B QAT | 0.6872 | **0.8000** | **0.463** | **180** |
| gemma-4-12B QAT | 0.6854 | 0.7330 | 0.702 | 97 |

> 31B − 12B = **−0.0017, 95% CI [−0.0202, +0.0162]**, indistinguishable

Statistically the same model. The 31B finds the most facts in the entire field and
invents nearly twice as many on the 322 notes that assert nothing, staying correctly
silent less than half the time. Both 31B arms behave this way, QAT and not, so it is
the model rather than the quant.

Tripling the parameters bought recall and spent restraint, and F1 netted the two to
0.0017.

The same shape held in the small field, across three quants each:

| | abstention rate | spurious facts |
|---|---:|---:|
| E2B Q4 / Q6 / Q8 | 0.674 / 0.658 / 0.659 | 1083 / 1145 / 1142 |
| E4B Q4 / Q6 / Q8 | **0.578 / 0.576 / 0.565** | 1410 / 1413 / 1456 |

Their F1 spread is under 0.015. E4B stays silent on 58% of factless notes where
E2B stays silent on 67%.

F1 does not hide this. It **nets** it. At Q4, E4B leads by +0.0055 as scored and
would lead by +0.0163 if both abstained perfectly. E4B is the stronger extractor
paying a point back in over-extraction.

That is a single number doing its job. It is also useless to someone choosing a model
for a pipeline that writes into a knowledge graph and cannot tolerate invented edges.
Those models are not interchangeable and the ranking says they are.

The extreme case is thirteen rows down. **granite-4.1-3b abstains on 93% of factless
notes and invents 24 triples**, against 180 for the model at the top of the table. It
gives up 0.14 F1 and invents a seventh as much. If a wrong edge is expensive, that
trade is not close, and nothing in an F1 ranking will ever show it to you.

## Six: a gate discarded every fact the model found

`Qwen3-0.6B` scored 0.000. It scores 0.403 without the confidence floor.

The model extracted facts, wrote a confidence of 0.0 beside them, and a gate meant
to protect precision discarded all of them. `granite-4.0-350m` shows the same
pattern, 0.000 floored against 0.206 unfloored.

Across all sixteen models the self-reported confidence carries almost no signal.
Most write exactly 0.0 or exactly 0.9 and nothing between. The floor was retired
and replaced with a groundedness check that both endpoints trace back to the note
text.

It also inverted a size comparison. `granite-4.0-1b` against `granite-4.1-3b`
inverts **only under the floor**: floored, the 1B wins 0.600 to 0.571; unfloored,
the 3B wins 0.648 to 0.592. The floor discarded 13 of the 3B's facts against the
1B's 2. That pair measures the gate, not the parameter count, and it sat in a
ranking table looking like a size result.

**A small model that scores zero on your benchmark may be extracting correctly and
failing your gate.** Check the unfloored score before concluding anything.

## Seven: two models scored zero for opposite reasons

`SmolLM2-360M` and `gemma-3-270m` both score 0.000 with a JSON parse rate of 1.00
and a schema rate of 0.00. They answer, the answer parses, and it is never the
`{"facts":[...]}` wrapper the task asks for. No downstream gate recovers that.

`Qwen3-0.6B` also scores 0.000, with a parse rate of 0.99 and a schema rate of
0.97. Its output is fine and a gate ate it.

Same number, opposite diagnosis: one pair is unusable at this prompt, the other was
misconfigured. Parse rate and schema rate separate them in one line.

## Eight: the guard refused a model that was fine

A model-identity check derived the expected family name from the repository name.
Google names its file `gemma-4-E2B_q4_0-it.gguf`, which does not contain the stem
that check builds. The guard blocked a valid arm.

**The general failure:** a guard with no override, on a naming convention you do
not control. Mine now takes an explicit override, used where it is documented why.

## Six columns your harness already computes and does not show

**Parse rate and schema rate.** Separates "cannot do it" from "wrong envelope".

**Prompt plus completion tokens against context size.** The truncation flag will
not tell you.

**Reasoning-row count.** Not a boolean. A percentage.

**Abstention rate.** One line, already computed by the scorer in my case, and the
difference between "these models are equivalent" and "one invents facts a third
more often".

**Precision and recall separately.** A tie in F1 is frequently two failure modes
netting out.

**The unfloored score, beside the floored one.** A gate that protects precision can
take a working model to zero, and it did, twice.

Every one of those was already being computed somewhere in my harness before I
printed it. The cost of this list is display, not measurement.

## E4B stops reasoning on 16% of rows and I cannot say why

I do not know why E4B declines to reason on 16% of rows. Not context length, not
truncation, not the tool-call envelope: all three are zero across that arm. That
is unexplained rather than explained away.
