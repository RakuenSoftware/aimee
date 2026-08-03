# Your benchmark needs repeatable, not identical

DRAFT. All identity measurements below are final. One throughput comparison is
missing because the two configurations ran on different cards, which is stated
where it applies rather than hidden. Open items at the end.

A benchmark arm that takes 44 minutes is a benchmark you run once a night. We
wanted to run a six-arm ladder over 10,000 notes, which at that rate is 44 hours
of GPU time, so we went looking for speed.

Every option we found changed the model's output. Working out which changes were
acceptable turned out to be the more useful result, because the rule we started
with was wrong.

## Greedy decoding is deterministic, with an asterisk

Temperature zero, one slot, one request at a time. Run the same 20 notes twice
and you get the same bytes twice. We assumed this and, for once, it held:

| comparison | identical raw output |
|---|---:|
| banked arm against fresh run 1 | 20/20 |
| banked arm against fresh run 3 | 20/20 |
| fresh run 1 against fresh run 3 | 20/20 |

Bit-reproducible across independent server restarts, days apart.

Now the asterisk. Against a server that has **already served requests**, the same
20 notes give a different answer:

| comparison | identical raw output |
|---|---:|
| banked arm against run 2, warm server | 14/20 |
| run 3 against run 4, warm server | 14/20 |

Exactly 6 of 20 drift, and they drift the same way each time. The mechanism is
prompt-cache reuse: llama.cpp keeps a cached KV prefix per slot, every request in
this benchmark shares the same 600-token system prompt, and whether a given
request recomputes that prefix or reuses cached state depends on what ran before
it. The two paths do not produce bit-identical logits.

The practical consequence is narrow and sharp. **Spot-checking a handful of notes
against a server that is already running is not a valid check.** It manufactures
disagreements that have nothing to do with what you changed. Every arm in this
benchmark restarts its server, which is the only reason the banked results are
comparable at all.

## The control I did not run

We measured 32 parallel slots at 4.54 times faster, and 197 of 1001 notes
extracting different facts. The obvious reading is that concurrency changed the
output.

That reading had no control, and it was pointed out rather than noticed. Going
from 1 slot to 32 changes concurrency **and** the cache-reuse pattern at the same
time. One slot reuses one prefix. Thirty-two slots hold varying state across
requests. The warm-server effect above is already worth 6 in 20 on its own.

So 197 is an upper bound on the concurrency effect, not a measurement of it. The
number stays in the notes with that label attached.

If you take one process point from this article: when you change a knob and the
output moves, check whether the knob moved anything else.

## Multi-token prediction: fast, wrong, and usable

Speculative decoding drafts several tokens with a small head and verifies them
against the full model. Under greedy sampling it should be output-identical,
because a drafted token is accepted only when it equals the target's argmax.

It is not.

| configuration | identical to the banked sequential arm | wall |
|---|---:|---:|
| plain, no MTP | 100/100 | 263 s |
| MTP | 74/100 | 144 s |

The theory misses that verification feeds several tokens through the target in
one forward pass instead of one. The batch shape changes, the floating-point
reduction order changes with it, and near-ties flip. Twenty-six notes in a
hundred is not a rounding curiosity you can wave away.

Then the measurement that changes what to do about it. Two MTP runs against each
other, fresh server each time:

| model | run 1 against run 2 |
|---|---:|
| E4B UD-Q4_K_XL | 100/100 |
| E2B UD-Q4_K_XL | 100/100 |

MTP perturbs the output relative to a sequential run, and it perturbs it **the
same way every time**. The batch shapes are fixed by the draft length, not by
anything external.

We checked E2B rather than assuming it, because `--model` is only a label and a
stale server would have loaded E4B twice and produced a meaningless pass.
`/props` confirmed the E2B quant, and median latency of 1345 ms against E4B's
2548 ms confirmed it independently.

The speedup is a property of the model, not of the feature:

| model | sequential | with MTP | ratio |
|---|---:|---:|---:|
| E4B UD-Q4_K_XL | 22.9 notes/min | 41.9 | 1.83x |
| E2B UD-Q4_K_XL | 27.0 notes/min | 43.0 | 1.59x |

Speculation reclaims compute that sits idle while the card waits on memory. A
smaller model is less bandwidth-bound at batch size 1, so there is less idle
compute to reclaim and less to gain. Quoting one number for "MTP speedup" would
be wrong.

## Thirty-two slots fails the only test that matters

We expected slots and speculation to compound. Both halves of that were wrong.

| configuration | speedup |
|---|---:|
| MTP alone | 1.83x |
| 32 slots alone | 4.54x |
| 32 slots and MTP together | 4.34x |

The combination is marginally slower than slots alone. They spend the same
resource. At batch size 1 the GPU is bandwidth-bound with compute to spare, and
both features exist to fill that gap. With 32 sequences in flight there is
nothing left for drafting to claim, so verification is added work with no idle
capacity to hide in.

The disqualifying result is not the speed:

| comparison, 32 slots and MTP | identical |
|---|---:|
| run 1 against run 2, raw completions | 63/100 |
| run 1 against run 2, extracted facts | 75/100 |

**Twenty-five notes in a hundred extract different facts between two runs of the
same configuration on the same hardware.** Wall time varied too, 71 s against
61 s, and that is the mechanism: with 32 requests in flight, which requests share
a batch depends on arrival and scheduling timing, and that is not reproducible.

## The rule we were using was the wrong one

We had been asking whether a fast configuration reproduces the sequential
baseline. That is the wrong question, and it took the 32-slot result to see it.

The right question is whether a configuration reproduces **itself**.

MTP fails the first test at 74/100 and passes the second at 100/100. Thirty-two
slots fails both. A benchmark's job is to compare arms to each other, so what it
needs is one configuration held fixed across every arm. It does not need that
configuration to agree with some other configuration it is not using.

| configuration | speed | matches sequential | repeatable |
|---|---:|---:|---|
| sequential | 1.00x | identical by definition | yes, 4 confirmations |
| MTP | 1.83x | 74/100 | yes, 100/100 on both models |
| 32 slots | 4.54x | 804/1001 | no |
| 32 slots and MTP | 4.34x | 64/100 | no, 75/100 against itself |
| 3 isolated processes, MTP | not on matched hardware | not measured | yes, 1001/1001 |

The bottom row is the configuration this benchmark actually runs, and it is the
clearest case of the rule. Its "matches sequential" cell is empty on purpose: we
never ran plain sequential at 1001 notes, so the comparison does not exist. What
we do have is the same arm on **one** single-slot server against **three**,
holding model, quant, prompt and MTP fixed and varying only the process count —
652 of 1001. That is the subject of the next section.

The quant deltas we are chasing are around 0.01 F1. The configuration noise in
those bottom two rows moves 20 to 26 percent of notes. Mixing configurations
across arms would have buried the signal under the instrument.

## Isolated processes get both

The reason 32 slots is not repeatable is that slots batch requests into a
**shared forward pass**, so one sequence's logits depend on which other requests
happen to be in flight beside it. Separate processes have separate contexts and
never share a matrix multiply. Contention between them changes timing, which
changes nothing arithmetic.

| parallelism | identical between two runs |
|---|---:|
| 32 slots in one process | 44/60 |
| 2 isolated processes | 60/60 |
| 3 isolated processes, full corpus | 1001/1001, three ways |

The last row is the one that settles it. **Three** independent runs of the same
three-process arm, on the same 1001 notes and the same card, spread over days
with server restarts between them, produced byte-identical raw completions on
every single note in all three pairwise comparisons, and the same strict F1 to
four decimal places, 0.6138, every time. Sixty out of sixty was suggestive. Three
runs agreeing on a thousand and one notes out of a thousand and one is the
property itself.

The three-way check matters more than a second run would have. Two identical runs
can happen because something is cached or copied; a third, launched from a
different script on a different day, is much harder to explain that way.

And in the same breath it earns the article's title. That arm agrees with itself
perfectly and does **not** agree with the one-server run of the same model, same
quant, same prompt and same MTP setting — the process count is the only thing
that differs. 652 of 1001 raw completions match, and strict F1 lands at 0.6033
against 0.6138. Isolation reproduces itself exactly while differing from a
one-process run of the identical arm by 0.0105 F1. Repeatable, not identical,
measured on the full corpus rather than argued from 60 notes.

That 0.0105 deserves its own warning, because it dwarfs the effects this
benchmark chases. A quant step we care about is worth roughly 0.0065 to 0.015
F1. Changing nothing but the number of server processes moves the score by
0.0105. Comparing an arm run at one process against an arm run at three is
therefore not a comparison at all, and sample size does not help.

One caution on that number, because we nearly published a worse version of it.
Our first single-server reference was an older banked arm scoring 0.6114, which
made for a tidy 0.0024 gap. Its device record says `total_slots : 4`. It was
never a single-slot run at all — it belongs with the multi-slot configurations
above, where a shared forward pass makes results non-reproducible by
construction, and it is not comparable to anything in this section. The 0.6033
figure is the honest one because `-np 1` is asserted in the harness on both
sides. Whether the one-process configuration reproduces *itself* is a separate
question, and a second single-server run is in flight to answer it. Until it
lands, read 0.0105 as the distance between two configurations rather than as a
settled constant.

So the production configuration is N
single-slot servers, each with its own draft head, corpus split round-robin
between them. Round-robin rather than in blocks, because the corpus is ordered by
domain and a contiguous split would hand one shard every negation note and
another every infrastructure note, and they would finish hours apart.

Here is the caveat, and it is a real limit on this section. Our sharded 10,000
note arms run at roughly 58 to 61 notes per minute on three processes, and the
41.9 notes per minute MTP figure above was measured **on a different card**. We
cannot divide those two numbers. What process isolation costs or saves in
throughput against single-process MTP on matched hardware is not something this
work has measured, and the speedup claim you would most want from this section is
the one we cannot make yet.

What we can say is that the repeatability result stands on its own, it was
measured directly, and it is the property the benchmark actually needed.

## What still needs measuring

1. **Sharded throughput on matched hardware.** Run 1, 2, 3 and 4 isolated MTP
   processes on the same card, same corpus slice, and report notes per minute for
   each. Until this exists, the number of processes is chosen by what fits in
   VRAM rather than by where the returns stop.
2. **Where the returns stop.** The runner caps at 6 processes on the assumption
   that the card is bandwidth-saturated beyond that. The assumption is untested.
3. **Isolate concurrency from cache reuse.** Re-run the 32-slot comparison with
   the prompt cache disabled, which should separate the two effects that 197 of
   1001 currently confounds.
4. **Whether MTP self-consistency survives longer outputs.** Both 100/100 results
   used the standard extraction prompt. A configuration that drifts only on long
   generations would not show up here.
