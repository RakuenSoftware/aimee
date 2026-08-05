# Every configuration reproduces itself exactly and no two agree

ROUGH DRAFT. The discriminating run for the last mechanism is in flight.

A benchmark arm that takes 44 minutes is one you run once a night. I wanted a
six-arm ladder over 10,000 notes, which at that rate is 44 hours, so I went
looking for speed.

Every option I found changed the model's output. Working out which changes were
acceptable turned out to be the useful result, because the rule I started with was
wrong.

## Identical to what

The question people ask about a fast configuration is whether it reproduces the
sequential baseline. That is the wrong question, and it took the 32-slot result to
show me.

The right question is whether a configuration reproduces **itself**.

| configuration | speed | matches sequential | repeats itself |
|---|---:|---:|---|
| sequential | 1.00x | identical by definition | yes, 4 confirmations |
| speculative decoding | 1.83x | 74/100 | **yes, 100/100 on both models** |
| 32 slots | 4.54x | 804/1001 | **no** |
| 32 slots and MTP | 4.34x | 64/100 | **no, 75/100 against itself** |
| 3 isolated processes, MTP | not on matched hardware | not measured | yes, 1001/1001 |

Speculative decoding fails the first test and passes the second. Thirty-two slots
fails both. A benchmark compares arms to each other, so it needs one configuration
held fixed across every arm. It does not need that configuration to agree with one
it is not using.

## Thirty-two slots is fast and disqualified

Twenty-five notes in a hundred extract **different facts between two runs of the
same configuration on the same hardware**:

| comparison, 32 slots and MTP | identical |
|---|---:|
| run 1 against run 2, raw completions | 63/100 |
| run 1 against run 2, extracted facts | 75/100 |

Wall time varied too, 71 s against 61 s, and that is the mechanism. With 32
requests in flight, which requests share a batch depends on arrival and scheduling
timing, and that is not reproducible.

Slots batch requests into a **shared forward pass**, so one sequence's logits
depend on which other requests are in flight beside it. Separate processes have
separate contexts and never share a matrix multiply. Contention between them
changes timing, which changes nothing arithmetic.

| parallelism | identical between two runs |
|---|---:|
| 32 slots in one process | 44/60 |
| 2 isolated processes | 60/60 |
| 3 isolated processes, full corpus | **1001/1001, three ways** |
| 1 process, full corpus | 1001/1001 |

Three independent runs of the same three-process arm, same 1,001 notes, same card,
days apart with server restarts between them, produced byte-identical completions
on every note in all three pairwise comparisons and the same strict F1 to four
decimals, 0.6138, each time.

The three-way check matters more than a second run would. Two identical runs can
happen because something was cached or copied. A third, launched from a different
script on a different day, is harder to explain that way.

## The title, in one comparison

That same three-process arm does **not** agree with a one-process run of the same
model, quant, prompt and decoding setting. Process count is the only difference.

| | raw completions matching | strict F1 |
|---|---:|---:|
| 3 processes against 1 process | 652/1001 | 0.6138 against 0.6033 |

And the one-process configuration is not the sloppy one. Run it twice and it is
also byte-identical at 0.6033 both times. Neither drifts. They disagree with each
other permanently, by **0.0105 F1**, which is larger than the quant steps this
benchmark was built to detect (0.0065 to 0.015).

Comparing an arm run at one process against an arm run at three is not a
comparison, and sample size does not help.

## Four more ways output moves without accuracy moving

**Warm servers.** Cold, the same 20 notes give the same bytes: 20/20 across
independent restarts days apart. Against a server that has already served
requests, 14/20. Exactly 6 drift, the same 6 each time. The prompt cache keeps a
KV prefix per slot, every request shares the same 600-token system prompt, and
whether a request recomputes or reuses depends on what ran before it.

The consequence is narrow and sharp: **spot-checking a few notes against a running
server is not a valid check.** It manufactures disagreements unrelated to what you
changed.

**Verification batching.** Speculative decoding pushes several tokens through the
target in one forward pass. Batch shape changes, floating-point reduction order
changes with it, near-ties flip. 26 notes in a hundred, and the same 26 every time.

**Corpus composition.** The one I did not expect. The same note, model, quant,
process count and prompt produces different text depending on which corpus it was
embedded in: 529 of 1,001 identical between a 1,001-note run and a 3,002-note run
that strictly contains it.

The obvious cause fails its own test. Same predecessor, 44.8% churn. Different
predecessor, 48.3%. The cache holds roughly 38 entries, so what carries is the last
38 notes, and almost every note has a different 38-note history between corpora.
That predicts uniform churn and uniform churn is what appears. **Consistent with
the mechanism, not a measurement of it.** The discriminating run, both corpora with
the cache disabled, is on the card as I write, with the prediction registered
before it started: near 1001/1001 confirms the cache, anything near 79% withdraws
the explanation.

**Cache setting.** Cache on against cache off, same corpus: 792/1001.

Every one of those rewrote between 21% and 47% of the output text. None moved F1
outside its own interval.

## The control I did not run

I measured 32 slots at 4.54x with 197 of 1,001 notes extracting different facts,
and read that as the concurrency effect.

It had no control, and it was pointed out rather than noticed. Going from 1 slot to
32 changes concurrency **and** the cache-reuse pattern together, and the warm-server
effect alone is worth 6 in 20.

So 197 is an upper bound on the concurrency effect, not a measurement of it.

**When you change a knob and the output moves, check whether the knob moved
anything else.**

## A run's slot count is part of its identity

I nearly published a worse version of this. My first single-server reference was an
older banked arm at 0.6114, which made a tidy 0.0024 gap. Its device record says
`total_slots : 4`. It was never a single-slot run.

It looks respectable from the outside: it sits between the two honest numbers and
moves 645 of 1,001 notes against one and 688 against the other, which is exactly the
profile of a third configuration nobody labelled as one.

The slot count was recorded in a `device.txt` next to the predictions and read by
nobody until a number disagreed. A recorded signal that nothing consumes is the
recurring defect class in this project.

## Turning the cache off costs nothing, and I assumed otherwise for two days

I recorded the comparability run as unaffordable. The reasoning was sound: the
600-token system prompt is served from the cache, so disabling it re-evaluates that
prefix per note.

Measured, the same 1,001 notes take 38 minutes with the cache off against 41 with it
on. Prefilling 600 tokens is noise next to two seconds of generation.

I wrote "that is the article owner's call" into a defect entry rather than spending
40 minutes finding out.

## What to require

**Self-reproduction, per configuration, three ways.** Run one arm three times before
you trust any arm. Two can agree because something was cached.

**No comparison across a configuration boundary.** Speculative against sequential,
warm against cold, one process against three, a native run against a subset of a
larger one. Those are different configurations, not two measurements of one thing.

**The slot count, process count and cache setting recorded and read.** All three were
in my output before they were in my analysis.

**Cache off for any cross-corpus comparison.** It costs about 7% of wall clock,
which I can say because I measured it rather than reasoned about it.

**Do not read output churn as accuracy movement.** Everything above moved 21% to 47%
of the text and none of it moved the score outside its interval.

## What I have not measured

Both identity measurements used the standard extraction prompt, a few hundred
tokens. A configuration that drifts only on long generations would not appear in
either, and I have not run that.

And the throughput comparison you would most want from this piece, isolated
processes against single-process speculative decoding, does not exist: the two
figures were measured on different cards and I cannot divide them.
