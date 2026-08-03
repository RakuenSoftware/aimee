# The benchmark was fast because it was broken

DRAFT. Every measurement below is final and reproduced. Open items at the end.

A 10,000 note extraction run finished in 34 minutes. We read that as a fact about
the hardware and moved on to the next arm.

It should have taken about six hours. The run was fast because the model was not
thinking, and the model was not thinking because of a sentence we had written in
the prompt to make its output easier to parse.

Four separate defects in this benchmark are worth writing down, not because they
are interesting individually but because of what they have in common: **none of
them produced an error, and all of them produced a plausible number**.

## One sentence turned the reasoning off

The extraction prompt ended with:

    No prose, no markdown.

`gemma-4-E4B` applies that instruction to its own reasoning channel. Across all
10,000 notes of a run whose every row recorded `thinking: true`, it emitted zero
reasoning tokens.

We isolated the clause:

| system prompt | notes that reasoned |
|---|---:|
| v4, unmodified | 0/20 |
| minus `No prose, no markdown.` | 20/20 |
| minus `Return ONLY a JSON object:` | 0/20 |
| rescoped to "the answer itself must be JSON only" | 0/20 |
| v5, "Reason first if it helps; the answer that follows..." | 20/20 |

Two properties made this hard to see. **Nothing failed.** Valid JSON, clean
parse, no truncation, and an F1 of 0.5947 that sat comfortably among the other
models. And **E2B does not have the behaviour**, so the two arms of one sweep
disagreed in a way that looked like an ordinary model-size effect.

Deleting the sentence is not the fix. Removing it restores reasoning and brings
back fenced ` ```json ` output on 14 of 20 notes. The clause was doing real work;
the production parser's first-brace-to-last-brace scan was quietly absorbing the
cost. The fix was to rescope it, and the rescoping had to be tested, because the
first two attempts at rescoping did nothing at all.

The obvious objection was that the quantisation was responsible. We tested it
rather than argued about it, on two independent builds with **different chat
templates**: Unsloth UD-Q4_K_XL and stock ggml-org Q8_0. Both suppress. It is the
model.

## The signal was in every row and nothing read it

This is the part worth generalising.

The first instinct was to look at the completion length. Median 27 tokens sounds
broken. It is not: `{"facts":[]}` is 5 tokens and a single triple is about 30, so
a p10 of 5, median of 27 and p90 of 49 is exactly what a healthy extractor
produces on a corpus that is a third factless. `parse_ok` was 10000 of 10000. The
answer channel was never unhealthy. Only the reasoning channel was, and the
answer channel is the one every tool looks at.

The real evidence was present in all ten thousand rows:

```json
{"thinking": true, "reasoning_chars": 0, "parse_ok": true, "truncated": false}
```

That row contradicts itself. The run was configured to think and produced no
thought, and it said so, ten thousand times over, in a field that had been added
during an earlier investigation and never consumed by `score.py` or
`summarize.py`.

**Recording a signal is not checking it.** This was the fourth instance of that
defect in this codebase, and the two before it are documented in the same file
under the heading "It recorded the field. Nothing read it."

The fix is a gate rather than a note: the scorer now refuses to score a run whose
rows claim thinking and contain no reasoning anywhere. A check that runs is worth
more than a field that exists.

| | v4 as banked | thinking restored |
|---|---:|---:|
| median completion tokens | 27 | ~390 |
| median latency | 214 ms | ~1790 ms |
| notes that reasoned | 0/10000 | 20/20 |
| throughput | 280/min | 27/min |

The tenfold speed difference was the most visible symptom and the one we
explained away first.

## The constant came from 70 notes

While fixing the above we went looking for why thinking was enabled at all. The
justification was a constant that appeared in `kb_curator_provider.c`, in
`provider_client.c`, and in the commit messages that introduced both:

**"Thinking is worth +0.084 F1 to E4B."**

Its provenance was 53 true positives across about 70 notes, with no interval
attached. It had become a design decision.

Re-measured paired over 955 notes, same model, quant, card and corpus:

| | strict F1 | precision | recall |
|---|---:|---:|---:|
| thinking suppressed | 0.5990 | 0.6607 | 0.5478 |
| thinking restored | 0.6093 | 0.6175 | 0.6014 |

**+0.0103, with a 95% interval of [-0.0201, +0.0404].** Indistinguishable, over
5000 paired replicates. The constant was eight times its own re-measured value
and the sign was the only part that survived.

Stopping there would have repeated the original error in the opposite direction,
which is the trap this section is really about. An audit of the errors found that
68 of the 93 extra false positives introduced by thinking are reconcilable by
`rel_type_canonicalize()` and the entity graph, machinery production already
runs. Only about 24 are genuinely spurious. Scored on entity pairs while ignoring
how the predicate was named:

| | relation-agnostic F1 | precision | recall |
|---|---:|---:|---:|
| thinking suppressed | 0.7783 | 0.8585 | 0.7118 |
| thinking restored | 0.8390 | 0.8503 | 0.8280 |

**Recall up 0.116 at flat precision**, with a fabrication rate of 0.0 in both
arms.

Thinking finds materially more real facts and names them more variably. Strict F1
charges that variance twice, once as a miss and once as a false positive, so a
change that is clearly good under the metric production actually cares about
looks like noise under the metric the benchmark reports. It does cost abstention,
0.907 down to 0.870.

We have no interval on the relation-agnostic delta. The bootstrap tool scores
strict F1 only. That is an honest gap and it is listed at the end.

## The corpus penalised the correct answer

The gold set contains a relation `has_hostname`. Twenty-eight of its 51 gold
triples came from notes generated by a template that phrases the fact as "X runs
on Y".

| note phrasing | n | model answers has_hostname | model answers runs_on |
|---|---:|---:|---:|
| "X has hostname Y" | 23 | 23/23 | 0 |
| "X runs on Y" | 28 | 0/28 | 23 |

Identical in both runs we checked. The model is perfect on one phrasing and
scores zero on the other, because it reads "runs on" as deployment. Which is what
it means.

One corpus template was manufacturing 28 false negatives and 23 false positives
per arm, and it was penalising precisely the models that read the sentence
correctly. A model that answered `has_hostname` to "X runs on Y" would have
scored better and understood less.

They are different facts at different levels of the same chain:

```
service --runs_on--> host --has_hostname--> "wol-realm-dev-9"
```

Getting this right in the corpus is not cosmetic. The whole benchmark exists to
rank models by how well they read a sentence, and this template rewarded reading
it wrong.

## What these four have in common

None of them threw an exception. None produced a malformed file. Every one of
them produced a number that fell in a believable range next to the other numbers
in its table, which is exactly why they survived.

The defect classes are worth naming separately, because the countermeasures
differ:

- **A prompt instruction with side effects on a channel you were not thinking
  about.** Countermeasure: test the clause in isolation, on more than one build.
- **A recorded signal that nothing consumes.** Countermeasure: make it a gate
  that can refuse the run, not a field in the output.
- **A measured constant that outlives its sample size.** Countermeasure: an
  interval beside the number, in the source comment, or the number does not go
  in the source.
- **A corpus that encodes an opinion about language.** Countermeasure: check
  per-template scores, not just per-model scores. This one showed up as a
  bimodal distribution within a single relation.

A benchmark is code. It has bugs at the same rate as the rest of your code. The
difference is that its bugs come out looking like results, and a result does not
announce that it is wrong.

## What still needs measuring

1. **Intervals on relation-agnostic F1.** The bootstrap tool scores strict only,
   so the +0.116 recall figure that carried the thinking decision has no interval
   beside it. That is the same defect as the +0.084 constant, one level up.
2. **Re-run every banked arm on the corrected corpus.** Every number predating
   corpus v5 was scored against the `runs_on` template. The rankings in our
   earliest comparison are hypotheses about order until this lands.
3. **Audit the remaining corpus templates the same way.** We found this one by
   noticing a bimodal score inside one relation. Nothing has checked the others.
4. **Test the prompt clause against every model in the ladder.** We know E4B
   suppresses and E2B does not. The other fourteen are unchecked, and a model
   that silently loses its reasoning pass scores as a worse model.
