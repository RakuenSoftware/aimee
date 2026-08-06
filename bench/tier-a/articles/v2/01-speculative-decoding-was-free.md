# Speculative decoding doubled throughput and cost nothing I could measure

DRAFT. All six paired arms are banked. The acceptance figures are read from
`timings.draft_n` rather than inferred from wall clock.

Multi-token prediction on gemma-4 more than doubles throughput on this extraction
task. It changes 26% of the output text. It changes accuracy by an amount I can
bound inside four thousandths of an F1 point.

That is a free lunch, which is the kind of claim I should distrust, and I measured
it wrong twice before I measured it right.

Then I ran a model that does not speculate at all and it beat every model that
does.

## The number you are watching is the wrong one

Everyone reporting speculative decoding reports a speedup multiple. I reported
5.3x, then 1.58x, and both were properties of my instrument rather than of the
feature.

The number that matters is the pair. One card, one corpus, one process count, the
draft model as the only difference between two arms:

| model | quant | MTP | no-MTP | ΔF1 | steady throughput |
|---|---|---:|---:|---:|---:|
| E2B | Q4 | 0.6246 | 0.6207 | +0.0039 | +84.0% |
| E2B | Q6 | 0.6344 | 0.6331 | +0.0013 | +91.6% |
| E2B | Q8 | 0.6329 | 0.6351 | −0.0022 | +102.5% |
| E4B | Q4 | 0.6301 | 0.6306 | −0.0005 | +110.6% |
| E4B | Q6 | 0.6452 | 0.6435 | +0.0017 | +116.2% |
| E4B | Q8 | 0.6337 | 0.6327 | +0.0010 | **+131.3%** |

10,000 notes per arm, three processes, RX 7900 XTX. The accuracy deltas scatter
around zero, the sign flips three times, and the largest is 0.0039. Throughput
climbs the whole way and never stops climbing.

Three of the six carry a paired bootstrap, 20,000 replicates over the same 10,000
notes:

> E4B Q4: no-MTP − MTP = **+0.0005, 95% CI [−0.0028, +0.0036]**
> E4B Q6: no-MTP − MTP = **−0.0017, 95% CI [−0.0048, +0.0013]**
> E4B Q8: no-MTP − MTP = **−0.0010, 95% CI [−0.0041, +0.0021]**

None of those is "I cannot tell". Each says *the effect is smaller than five
thousandths of an F1 point in either direction*. A precise null is a stronger
statement than an indistinguishable one, and it took 60,000 notes to buy three.

The gain rises with quant size inside each family, which is what bandwidth-bound
decoding predicts. A heavier target spends more time waiting on memory, so there
is more idle compute for speculation to reclaim. Q8 gains most because it is the
most expensive to read.

## Measure the mechanism, not its shadow

Wall clock confounds the feature with the host, the model and the backend. The
mechanism is `timings.draft_n` and `draft_n_accepted`: how many tokens the draft
proposed and how many the target kept. I had been reporting the shadow for months.

Six large arms, every one with acceptance recorded:

| arm | drafted tokens | accepted |
|---|---:|---:|
| gemma-4-12B non-QAT | 1,510,235 | 82.0% |
| gemma-4-12B QAT | 1,414,986 | 81.2% |
| gemma-4-26B-A4B unsloth QAT | 1,360,556 | 79.2% |
| gemma-4-26B-A4B google q4_0 | 1,367,766 | 79.1% |
| gemma-4-31B QAT | 539,715 | 79.1% |
| gemma-4-31B non-QAT | 620,046 | 78.5% |

**Acceptance tracks the model and ignores the quant.** Each pair is within a point
of itself across quant schemes that differ by up to 0.023 in F1. So MTP and
quantisation compose: choose the quant on accuracy and file size, then turn
speculation on separately, and neither decision constrains the other.

Acceptance also falls slowly with size, 82% at 12B to 78.5% at 31B, which is the
opposite of the wall-clock story. The 31B gains *more* wall clock from speculation
than the 12B and accepts *fewer* drafted tokens, because it is more
bandwidth-bound. Reporting speedup alone would have shown one number and hidden
both.

## It is not output-identical, and that is the interesting part

Speculative decoding is supposed to be lossless. The draft is verified against the
target, so an accepted token is the token the target would have produced.

Measured on 100 notes, greedy, fresh servers:

| | identical to the sequential arm |
|---|---:|
| plain, no MTP | 100/100 |
| MTP | **74/100** |

Verification pushes several tokens through the target in one forward pass. The
batch shape changes, the floating-point reduction order changes with it, and
near-ties flip. Twenty-six notes in a hundred.

So the question is not whether the output moved. It moved. Whether it got worse is
what the table above answers, and the answer is no.

It also moves the **same way every time**. Two speculative runs against each other,
fresh server each: 100/100 on E4B and 100/100 on E2B. Batch shapes are fixed by
draft length rather than by anything external. I checked E2B rather than assuming
it, because `--model` is only a label and a stale server would have loaded E4B
twice and produced a meaningless pass. `/props` confirmed the quant, and a median
latency of 1345 ms against E4B's 2548 ms confirmed it independently.

I checked one more way, because an aggregate null can be two opposite effects
cancelling. On a different question in this project an aggregate null over this
same corpus turned out to be +0.24 F1 on one subset and −0.02 on another. So I
split all four pairs by note category. Largest single movement: +0.0220 on
implicit, n=723, inside the ±0.024 that sample size supports. No category exceeds
its own interval. The null is a null all the way down.

## A model with no draft head beat every model with one

Qwen3.6-35B-A3B ran at **234 tok/s with no speculative decoding at all.** The dense
gemma-4-12B, running a draft head at 82% acceptance on a comparable card, managed
195.8.

I had both Qwen arms labelled "native MTP" in my own notes for several hours,
because 234 tok/s on a 35B model looked impossible without it. `/props` reports
`speculative: null`. No row in either prediction file carries a `draft_n` counter.
Qwen3.6 publishes no MTP draft in that repo. I inferred a mechanism from a number
and the inference outlived three status reports before I checked the field that
was sitting in every row.

The real mechanism is architecture. 35B resident, roughly 3B active per token, so
about 1.5 GiB of weights read per token against a card doing 1.79 TB/s. Its own
dense sibling makes the point without any speculation involved on either side:

| Qwen3.6, same family, same quant, same card class | tok/s | median completion |
|---|---:|---:|
| 35B-A3B, mixture of experts | **234.0** | 1,100 tok |
| 27B dense | 67.8 | 1,272 tok |

**3.5 times faster, writing the same amount of text.** A dense 27B at Q4 reads
about 16.4 GiB per token; the MoE reads roughly a tenth of that.

So the ranking is: speculation is worth about 2x, and picking a sparse
architecture is worth 3.5x. If you are optimising throughput and you can choose
the model, choose the model first. Speculation is what you turn on afterwards, on
whatever you chose.

## The 5.3x was two numbers with different denominators

The first figure came from dividing 68.5 notes/min, a completed MTP arm, by about
13 notes/min, a no-MTP arm sampled while it was still starting up.

Worse, the denominator was contaminated. Fifteen orphaned client processes from
runs I had killed earlier were still issuing requests to the same three ports the
live arm was using. Every request was served correctly. It simply queued. The
server's own timings looked healthy and only the client saw the cost. Killing the
orphans took the identical in-flight arm from 8.8 to 38.7 notes/min.

The tell had been visible for six hours: a load average of 27 on a machine whose
only job was shuttling JSON over three SSH tunnels. Nothing in my harness looks at
load, and no diagnostic printed the client count.

## The 1.58x measured startup and called it throughput

The second attempt was a real experiment. Two eight-configuration sweeps, one per
card, 200 notes each, process counts 1 through 4, with and without MTP.

Its throughput metric was rows divided by wall clock, and wall clock includes
server startup. Startup is about 30 seconds per server, so it grew with the
variable under test:

| card | nproc=1 | nproc=2 | nproc=3 | nproc=4 |
|---|---:|---:|---:|---:|
| RTX 5080 | 56 s | 84 s | 107 s | 137 s |
| RX 7900 XTX | 61 s | 67 s | 83 s | 99 s |

On a 200-note run that is a third to a half of the wall clock, and the bias pointed
the same way as the hypothesis. It produced two confident wrong conclusions I
reported before catching them: that aggregate throughput peaks at two processes and
declines, and that four processes are slower than one.

Compute throughput from per-request latency and process count instead, which
excludes startup by construction, and the curve plateaus rather than falling.
nproc=4 is 30% to 100% faster than nproc=1.

## Before dividing two numbers, check the denominators are the same thing

That rule would have caught both wrong answers, and it is not a statistical one.

Each time, the data that would have caught me already existed. The 5.3x needed a
process count. The sweep needed its own startup column, which it computed and
discarded. And when I multiplied a single-stream figure by three to project a
three-process rate, the correction factor was in that sweep's output: per-stream
throughput falls from 359 to 148 tok/s between one process and three, on that card,
that afternoon.

The Qwen mislabelling is the same failure with a different surface. `draft_n` was
in every row of both files. I read the throughput column instead and explained it
with a feature the model does not have.

## Turn it on, then stop quoting a single speedup for it

Turn it on. On this task, across two model families and four sizes, it is worth
roughly a doubling of throughput for no accuracy cost that 10,000 notes can detect,
and it does not interact with your quant choice.

Three limits, all load-bearing.

**It is repeatable but not identical**, so an arm run with MTP cannot be compared
against an arm run without it. Those are different configurations, not two
measurements of one thing.

**The speedup belongs to the model and the backend, not to the feature:**

| model | sequential | with MTP | ratio |
|---|---:|---:|---:|
| E4B UD-Q4_K_XL | 22.9 notes/min | 41.9 | **1.83x** |
| E2B UD-Q4_K_XL | 27.0 notes/min | 43.0 | **1.59x** |

A smaller model is less bandwidth-bound at batch size 1, so there is less idle
compute to reclaim and less to gain. Quoting one number for "MTP speedup" would be
wrong.

**It does not compound with concurrency.** Thirty-two slots alone is 4.54x;
thirty-two slots and speculation together is **4.34x**, marginally slower. They
spend the same resource. With 32 sequences in flight there is no idle capacity for
drafting to claim, so verification is added work with nowhere to hide.

I can only vouch for gemma-4. It is still the only family in this field publishing
an MTP draft: I checked Qwen3.6 directly after mislabelling it, and the repo has
none.

## The null is bounded, not explained

1. **Acceptance against accuracy at the note level.** I have acceptance per arm and
   F1 per arm. Whether the notes where drafting fails are the notes where the model
   is wrong is unmeasured, and it is the question that would explain the null rather
   than just bound it.
2. **A second family with a draft head.** One family is a limit on the claim, not a
   gap I can close by running more gemma.
