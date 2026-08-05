# Speculative decoding doubled throughput and cost nothing I could measure

ROUGH DRAFT. Four of six paired arms banked. E4B Q6 and Q8 land tonight.

Multi-token prediction on gemma-4 more than doubles throughput on this
extraction task. It changes 26% of the output text. It changes accuracy by an
amount I can bound inside four thousandths of an F1 point.

That is a free lunch, which is the kind of claim I should distrust, and I
measured it wrong twice before I measured it right.

## The number you are watching is the wrong one

Everyone reporting speculative decoding reports a speedup multiple. I reported
5.3x, then 1.58x, and both were properties of my instrument rather than of the
feature.

The number that matters is the pair. One card, one corpus, one process count,
the draft model as the only difference between two arms:

| model | quant | MTP | no-MTP | ΔF1 | steady throughput |
|---|---|---:|---:|---:|---:|
| E2B | Q4 | 0.6246 | 0.6207 | +0.0039 | +84.0% |
| E2B | Q6 | 0.6344 | 0.6331 | +0.0013 | +91.6% |
| E2B | Q8 | 0.6329 | 0.6351 | −0.0022 | +102.5% |
| E4B | Q4 | 0.6301 | 0.6306 | −0.0005 | +110.6% |

10,000 notes per arm, three processes, RX 7900 XTX. The accuracy deltas scatter
around zero and the sign flips twice. Throughput climbs the whole way.

On the one pair with a paired bootstrap, 20,000 replicates over the same 10,000
notes:

> no-MTP − MTP = **+0.0005, 95% CI [−0.0028, +0.0036]**

That is not "I cannot tell". It is *the effect is smaller than four thousandths
of an F1 point in either direction*. A precise null is a stronger statement than
an indistinguishable one, and it took 10,000 notes to buy.

## It is not output-identical, and that is the interesting part

Speculative decoding is supposed to be lossless. The draft is verified against
the target, so an accepted token is the token the target would have produced.

Measured on 100 notes, greedy, fresh servers:

| | identical to the sequential arm |
|---|---:|
| plain, no MTP | 100/100 |
| MTP | **74/100** |

Verification pushes several tokens through the target in one forward pass. The
batch shape changes, the floating-point reduction order changes with it, and
near-ties flip. Twenty-six notes in a hundred.

So the question is not whether the output moved. It moved. The question is whether
it got worse, and the table above says no.

It also moves the **same way every time**. Two speculative runs against each other,
fresh server each: 100/100 on E4B and 100/100 on E2B. The batch shapes are fixed by
the draft length rather than by anything external. I checked E2B rather than
assuming it, because `--model` is only a label and a stale server would have loaded
E4B twice and produced a meaningless pass: `/props` confirmed the quant, and a
median latency of 1345 ms against E4B's 2548 ms confirmed it independently.

I checked one more way, because an aggregate null can be two opposite effects
cancelling. On a different question in this project, reasoning on against
reasoning off, an aggregate null over this same corpus turned out to be +0.24 F1
on one subset and −0.02 on another. So I split all four pairs by note category.
Largest single movement: +0.0220 on implicit, n=723, which is inside the ±0.024
that sample size supports. No category exceeds its own interval. The null is a
null all the way down.

## The 5.3x was two numbers with different denominators

The first figure came from dividing 68.5 notes/min, a completed MTP arm, by about
13 notes/min, a no-MTP arm sampled while it was still starting up.

Worse, the denominator was contaminated. Fifteen orphaned client processes from
runs I had killed earlier were still issuing requests to the same three ports the
live arm was using. Every request was served correctly. It simply queued. The
server's own timings looked healthy and only the client saw the cost. Killing the
orphans took the identical in-flight arm from 8.8 to 38.7 notes/min.

The tell had been visible for six hours: a load average of 27 on a machine whose
only job was shuttling JSON over three SSH tunnels. Nothing in my harness looks
at load, and no diagnostic printed the client count. `ps | grep -c` would have
answered it in one second.

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

On a 200-note run that is a third to a half of the wall clock, and the bias
pointed the same way as the hypothesis. It produced two confident wrong
conclusions that I reported before catching them: that aggregate throughput peaks
at two processes and declines, and that four processes are slower than one.

Compute throughput from per-request latency and process count instead, which
excludes startup by construction, and the curve plateaus rather than falling.
nproc=4 is 30% to 100% faster than nproc=1.

## Before dividing two numbers, check the denominators are the same thing

That rule would have caught all three attempts. It is not a statistical rule.

In each case the data needed to catch the error already existed. The 5.3x needed
a process count. The sweep needed its own startup column, which it was computing
and discarding. And when I multiplied a single-stream figure by three to project
a three-process rate, the correction factor was in the sweep's own output:
per-stream throughput falls from 359 to 148 tok/s between one process and three,
on that card, measured that afternoon.

## What to do with it

Turn it on. On this task, on these two models, it is worth roughly a doubling of
throughput for no accuracy cost that 10,000 notes can detect.

Two limits, both load-bearing. It is repeatable but not identical, so an arm run
with MTP cannot be compared against an arm run without it. Those are different
configurations, not two measurements of one thing. And the speedup belongs to the model
and the backend rather than to the feature:

| model | sequential | with MTP | ratio |
|---|---:|---:|---:|
| E4B UD-Q4_K_XL | 22.9 notes/min | 41.9 | **1.83x** |
| E2B UD-Q4_K_XL | 27.0 notes/min | 43.0 | **1.59x** |

Speculation reclaims compute that sits idle while the card waits on memory. A
smaller model is less bandwidth-bound at batch size 1, so there is less idle compute
to reclaim and less to gain. Quoting one number for "MTP speedup" would be wrong.

The same mechanism explains why it does not compound with concurrency. Thirty-two
slots alone is 4.54x; thirty-two slots and speculation together is **4.34x**, which
is marginally slower. They spend the same resource. With 32 sequences in flight
there is no idle capacity left for drafting to claim, so verification is added work
with nowhere to hide.

I can only vouch for gemma-4. It is the only family in this field that publishes
an MTP draft, so I have no second family to check, and that is a limit on the
claim rather than a gap I plan to fill.

## Still open

1. **E4B Q6 and Q8.** Two of six pairs. If either breaks the null this piece
   changes.
2. **Bootstrap intervals on the other five pairs.** Only E4B Q4 has one. The
   rest are point estimates, and I have already been burned once by treating a
   fixed number as a significance threshold when it was a measured effect from an
   unrelated experiment.
