# One sentence in my prompt turned a model's reasoning off

DRAFT. The +0.116 figure below has no interval, and that is stated where it appears
rather than at the end.

A 10,000 note extraction run finished in 34 minutes. I read that as a fact about the
hardware and moved on.

It should have taken six hours. The model was not thinking, and it was not thinking
because of a sentence I had written to make its output easier to parse.

## One sentence, applied to a channel I did not mean

The extraction prompt ended with:

    No prose, no markdown.

`gemma-4-E4B` applies that to its own reasoning channel. Across 10,000 notes of a
run whose every row recorded `thinking: true`, it emitted zero reasoning tokens.

I isolated it rather than guessing:

| system prompt | notes that reasoned |
|---|---:|
| v4, unmodified | 0/20 |
| minus `No prose, no markdown.` | 20/20 |
| minus `Return ONLY a JSON object:` | 0/20 |
| rescoped to "the answer itself must be JSON only" | 0/20 |
| v5, "Reason first if it helps; the answer that follows..." | 20/20 |

Two properties hid it. **Nothing failed:** valid JSON, clean parse, no truncation,
an F1 of 0.5947 sitting comfortably among the other models. And **E2B does not have
the behaviour**, so two arms of one sweep disagreed in a way that looked like an
ordinary model-size effect.

Deleting the sentence is not the fix. Removing it restores reasoning and brings
back fenced ` ```json ` output on 14 of 20 notes. The clause was doing real work
and the production parser's first-brace-to-last-brace scan was quietly absorbing
the cost. The fix was to rescope it, and the rescoping had to be tested, because
the first two attempts did nothing at all.

The obvious objection was the quantisation. I tested it on two independent builds
with **different chat templates**, Unsloth UD-Q4_K_XL and stock ggml-org Q8_0.
Both suppress. It is the model.

## The evidence was in all ten thousand rows

The first instinct was completion length. Median 27 tokens sounds broken. It is
not: `{"facts":[]}` is 5 tokens and a single triple is about 30, so a p10 of 5, a
median of 27 and a p90 of 49 is what a healthy extractor produces on a corpus that
is a third factless. `parse_ok` was 10000 of 10000.

The answer channel was never unhealthy. Only the reasoning channel was, and the
answer channel is the one every tool looks at.

The real evidence was in every row:

```json
{"thinking": true, "reasoning_chars": 0, "parse_ok": true, "truncated": false}
```

That row contradicts itself. The run was configured to think, produced no thought,
and said so ten thousand times, in a field added during an earlier investigation
and never consumed by anything.

| | v4 as banked | thinking restored |
|---|---:|---:|
| median completion tokens | 27 | ~390 |
| median latency | 214 ms | ~1790 ms |
| notes that reasoned | 0/10000 | 20/20 |
| throughput | 280/min | 27/min |

The tenfold speed difference was the most visible symptom and the one I explained
away first.

**Recording a signal is not checking it.** This was the fourth instance of that
defect class in this codebase. The fix is a gate rather than a note: the scorer now
refuses to score a run whose rows claim thinking and contain no reasoning anywhere.

## The constant came from 70 notes and outlived them

While fixing the above I went looking for why thinking was enabled at all. The
justification was a constant that appeared in `kb_curator_provider.c`, in
`provider_client.c`, and in the commit messages that introduced both:

**"Thinking is worth +0.084 F1 to E4B."**

Its provenance was 53 true positives across about 70 notes, with no interval. It
had become a design decision.

Re-measured paired over 955 notes, same model, quant, card and corpus:

| | strict F1 | precision | recall |
|---|---:|---:|---:|
| thinking suppressed | 0.5990 | 0.6607 | 0.5478 |
| thinking restored | 0.6093 | 0.6175 | 0.6014 |

**+0.0103, 95% interval [−0.0201, +0.0404]**, 5,000 paired replicates. The constant
was eight times its own re-measured value and the sign was the only part that
survived.

## Stopping there would have been the same error in reverse

That is the part of this worth taking away.

An audit of the errors found that 68 of the 93 extra false positives introduced by
thinking are reconcilable by `rel_type_canonicalize()` and the entity graph,
machinery production already runs. Only about 24 are genuinely spurious. Scored on
entity pairs while ignoring how the predicate was named:

| | relation-agnostic F1 | precision | recall |
|---|---:|---:|---:|
| thinking suppressed | 0.7783 | 0.8585 | 0.7118 |
| thinking restored | 0.8390 | 0.8503 | 0.8280 |

**Recall up 0.116 at flat precision**, fabrication rate 0.0 in both arms. It does
cost abstention, 0.907 down to 0.870.

Thinking finds materially more real facts and names them more variably. Strict F1
charges that variance twice, once as a miss and once as a false positive, so a
change that is clearly good under the metric production cares about looks like
noise under the metric the benchmark reports.

I have no interval on the relation-agnostic delta. The bootstrap tool scores strict
F1 only. That is the same defect as the +0.084 constant, one level up, and it is
the largest unbounded number in this project.

## Reasoning is a property of the run, not the model

Three shapes appear across fourteen models.

**Suppressed by prompt.** E4B loses its reasoning pass to a clause. E2B, same
family, does not.

**Absent entirely.** Seven of fourteen models in my head-to-head emit no reasoning
pass at all: both granite models, gemma-3n-E4B, SmolLM3-3B, and three of the LFM2.5
family. Not reduced. Zero.

**Partial and stable.** gemma-4 E4B under QAT declines to reason on 479 of 3,002
rows, 16%, of which 204 answer `{"facts":[]}` in five tokens. The rate reproduces at
two corpus sizes. Those rows abstain at 51% against 24.5% on rows where it did
reason.

I tested it rather than leaving it open, with a positive control so a null would
mean something. Two variants derived from the live template: `v4clause` restores
the original suppressing wording, `noclause` removes the output constraint
entirely.

| model | live | `v4clause` | `noclause` |
|---|---:|---:|---:|
| gemma-4-E4B | 100% | **0/1001** | **770/770** |
| granite-4.1-3b | 0% | | 0/1001 |
| SmolLM3-3B | 0% | | 0/798 |
| LFM2.5-230M | 0% | | 0/570 |

**It is that sentence, not the presence of a constraint.** Removing the constraint
entirely changes nothing for E4B; restoring the original wording kills reasoning
outright. So the rescoping fix works rather than merely coinciding with recovery.

And with the control firing, the zeros mean what they appear to mean: granite,
SmolLM3 and LFM2.5-230M reason on nothing with no constraint at all. Their zero is
capability, not my prompt. The bottom half of the ranking is not a prompt
artefact.

## The explanation I had was wrong and its own test killed it

The obvious reading of the 16% is that starvation causes E4B's deficit against its
own submodel.

Restricted to the 2,523 rows where E4B **did** reason, it scores 0.6238 against
E2B's 0.6420. The gap is wider where it reasons.

So the starvation is real, reproducible, and not the cause of anything I can
attribute to it. Not context length, not truncation, not an output-envelope
problem: all three are zero across that arm. That is an open question, not a
caveat.

## An aggregate null hid a +0.24

Reasoning on against reasoning off aggregated to approximately nothing across the
corpus. Split by note category it is **+0.24 F1 on one subset and −0.02 on
another**, cancelling.

A single number over a heterogeneous corpus can be the average of two effects
pointing opposite ways, and mine is heterogeneous by design: ten categories, from
notes carrying three facts to notes carrying none.

I have since split the speculative-decoding pairs the same way and there the null
survives, no category exceeding its own interval. That contrast is the useful part.
Aggregate nulls are not all the same kind, and you cannot tell which you have
without splitting.

## Every large model in the field reasons, and that proves less than it looks like

The six large arms I have since run all emit a reasoning pass on 100% of rows:
gemma-4 at 12B, 26B and 31B in both quants, and Qwen3.6-35B-A3B. Seven models in the
small field emit one on none.

The tempting reading is that reasoning is a capability that arrives with scale. My own
diagnostic says do not take it. On gemma-4-E4B the pass was suppressed by a sentence
in my prompt and restored by deleting it, with no change to the model at all, and the
same prompt is in front of every arm above. A model that silently loses its reasoning
pass scores as a worse model, and the correlation between size and reasoning in my
table is as consistent with larger models resisting a bad clause as it is with smaller
ones lacking the capability.

I have run the diagnostic on four of twenty-two. Until it runs on the rest, the split
in that column is a hypothesis about my prompt as much as about the models.

## Make the reasoning count a gate, not a field

**Count reasoning rows and print the percentage.** Not a flag, a percentage, per
arm. Mine was one field in the prediction row and nothing read it for weeks.

**Make it a gate, not a field.** A check that can refuse the run is worth more than
a value in the output.

**Treat an unexpectedly fast run as a bug report.** Six hours became 34 minutes and
I filed it as good news.

**Put an interval beside any constant that reaches your source code**, or keep the
constant out of the source. Mine was off by a factor of eight and shaped a design
decision in two files.

**Split every null by whatever strata your corpus has.** If it has none, fix that
before trusting any aggregate.
