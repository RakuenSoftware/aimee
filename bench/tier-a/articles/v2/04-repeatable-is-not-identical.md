# Every configuration reproduces itself exactly and no two agree

DRAFT. The discriminating run for the last mechanism is in flight.

A benchmark arm that takes 44 minutes is one you run once a night. I wanted a
six-arm ladder over 10,000 notes, which at that rate is 44 hours, so I went looking
for speed.

Every option I found changed the model's output. The rule I was using to judge them
was wrong, and it took the fastest option to show me.

## The question is not whether it matches sequential

It is whether a configuration reproduces **itself**.

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

## Two configurations, each perfectly repeatable, permanently 0.0105 apart

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
process count and prompt gives different text depending on which corpus it was
embedded in: 529 of 1,001 identical between a 1,001-note run and a 3,002-note run
containing it.

It took three hypotheses and two retractions to find the cause, and each test was
registered in writing before it ran.

| hypothesis | test | result |
|---|---|---|
| the preceding note | split churn by predecessor identity | 44.8% vs 48.3%, **refuted** |
| prompt-cache history | re-run both corpora with `--cache-ram 0` | 49.9% vs 52.8%, **refuted** |
| **sequence position** | same notes, seeded shuffle, cache off | **confirmed** |

| | byte-identical |
|---|---:|
| same notes, same order, cache off | **1001/1001** |
| same notes, **shuffled**, cache off | **524/1001** |
| same notes, inside a 3,002-note corpus | 499/1001 |

Shuffling reproduces the cross-corpus churn to within 25 notes. **A subset is not
a run because its notes sit somewhere else in the queue.**

That implies state carries between requests even with the prompt cache disabled:
`llama-server` holds a live KV context per slot, and `--cache-ram` governs the
prompt cache rather than that context. It does not identify which state, and after
two explanations that fitted the data and died on their own tests, I am not naming
a fourth without another registered one.

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

## Require self-reproduction three ways before you trust an arm

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

## The bound I now have, and the one I still do not

Identity is a property of a configuration, and hardware is part of the configuration.
I measured one crossing: a rented RTX 3090 against my local RTX 5080, identical
settings, same corpus, same prompt.

| | |
|---|---:|
| delta | **+0.0057 F1** |
| 95% CI | [−0.0136, +0.0251] |
| byte-identical completions | 640/1001 |

So two CUDA cards running the same build agree to within about **±0.019 at n=1001**
and disagree on a third of their output text. That is the same pattern as every other
mechanism in this piece: repeatable within a configuration, not identical across one.

The crossing I have never measured is my own two cards. The XTX runs Vulkan on a
different llama.cpp build, and nothing in this project has ever put the same arm on
both and compared. Every cross-card number I have published carries an unquantified
term because of it, which is why the two quant pairs now running keep each pair on a
single card rather than splitting halves across the field.

## Long generations, and the division I cannot do

Both identity measurements used the standard extraction prompt, a few hundred
tokens. A configuration that drifts only on long generations would not appear in
either, and I have not run that.

And the throughput comparison you would most want from this piece, isolated
processes against single-process speculative decoding, does not exist: the two
figures were measured on different cards and I cannot divide them.
