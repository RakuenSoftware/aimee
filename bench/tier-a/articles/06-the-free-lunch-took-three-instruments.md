# Speculative decoding was free, and it took three instruments to prove it

DRAFT. Four of six paired 10,000-note arms are banked; E4B Q6 and Q8 are running.
Every number below carries the interval it was measured with. Two earlier answers
to the same question are reported here as withdrawn rather than deleted, because
how they were wrong is most of the point.

Multi-token prediction more than doubles throughput on this extraction task and
changes accuracy by an amount bounded within **±0.004 F1**. That is a free lunch,
which is exactly the kind of claim that should be distrusted, and it was measured
wrong twice before it was measured right.

## The result

Six paired arms, 10,000 notes each, on one card, at three processes, with the
draft model as the only difference between the two sides:

| model | quant | MTP | no-MTP | ΔF1 | throughput |
|---|---|---:|---:|---:|---:|
| E2B | Q4 | 0.6246 | 0.6207 | +0.0039 | +84.0% |
| E2B | Q6 | 0.6344 | 0.6331 | +0.0013 | +91.6% |
| E2B | Q8 | 0.6329 | 0.6351 | −0.0022 | +102.5% |
| E4B | Q4 | 0.6301 | 0.6306 | −0.0005 | +110.6% |
| E4B | Q6 | *running* | | | |
| E4B | Q8 | *running* | | | |

The accuracy deltas scatter around zero and the sign flips twice. On the arm with
a paired bootstrap — E4B Q4, 20,000 replicates over the same 10,000 notes —

> no-MTP − MTP = **+0.0005, 95% CI [−0.0028, +0.0036]**

which is not "we cannot tell". It is *the effect is smaller than four thousandths
of an F1 point in either direction*. A precise null is a much stronger statement
than an indistinguishable one, and it took 10,000 notes to earn.

Throughput moves the other way and keeps climbing: 84%, 92%, 102%, 111%.

## It is not output-identical, and that is a different article

Speculative decoding is supposed to be lossless: the draft is verified against
the target, so an accepted token is the token the target would have produced.
Measured on 100 notes, greedy, fresh servers, it is not:

| | identical to the sequential arm |
|---|---:|
| plain, no MTP | 100/100 |
| MTP | **74/100** |

Verification feeds several tokens through the target in one forward pass instead
of one. The batch shape changes, the floating-point reduction order changes with
it, and near-ties flip. Twenty-six notes in a hundred.

Article 3 argues that this is fine — that a benchmark needs a configuration which
reproduces *itself*, not one that matches sequential — and it stands on this
measurement. This article asks the question article 3 does not: those 26 notes
changed, but did they get **worse**? The table above is the answer, and it is no.

## Answer one: 5.3×, withdrawn

The first figure was that MTP was worth 5.3× throughput. It came from dividing
68.5 notes/min (a completed 10,000-note MTP arm) by ~13 notes/min (a no-MTP arm
sampled while it was still early).

Two numbers, neither wrong on its own, and a ratio between them that was a
property of neither.

Worse, the denominator was contaminated. Fifteen orphaned client processes from
previously-killed runs were still issuing requests to the same three ports the
live arm was using. Every request was served correctly — it simply queued — so
the server's own timings looked healthy and only the client saw the cost. Killing
the orphans took the identical in-flight arm from 8.8 to 38.7 notes/min.

The tell had been visible for six hours: a load average of 27 on a machine whose
only job was shuttling JSON over three SSH tunnels. Nothing in the harness looks
at load, and no diagnostic printed the client count. `ps | grep -c` would have
answered it in one second.

## Answer two: 1.58–1.91×, corrected

The second attempt was a proper experiment: two eight-configuration sweeps, one
per card, 200 notes each, process counts 1 through 4, with and without MTP.

Its throughput metric was rows ÷ wall clock. Wall clock includes server startup.
Startup is about 30 seconds per server, so it **grew with the variable under
test**:

| card | nproc=1 | nproc=2 | nproc=3 | nproc=4 |
|---|---:|---:|---:|---:|
| 5080 | 56 s | 84 s | 107 s | 137 s |
| XTX | 61 s | 67 s | 83 s | 99 s |

On a 200-note run that is a third to a half of the wall clock, and the bias
pointed the same way as the hypothesis. It produced two confident, wrong
conclusions that were reported before being caught: that aggregate throughput
peaks at two processes and declines, and that four processes are slower than one.
Computing throughput from per-request latency and process count instead —
startup excluded by construction — the curve plateaus rather than falling, and
nproc=4 is 30–100% *faster* than nproc=1.

What survived the correction is a real finding, and it is backend-shaped:

| nproc | 5080 (CUDA) | XTX (Vulkan) |
|---|---:|---:|
| 1 | 47.6 | 40.7 |
| 2 | 67.4 | 63.8 |
| 3 | 59.9 | 78.1 |
| 4 | 61.8 | 83.3 |

CUDA flattens after two processes; Vulkan is still climbing at four. This project
runs three on both cards, a number chosen by what fits in VRAM rather than by
where throughput peaks.

## What the three attempts have in common

Each wrong answer came from an instrument that was lying in the direction the
hypothesis wanted, and in each case the data needed to catch it already existed.

The 5.3× needed a process count. The sweep needed its own startup column, which
it was already computing and discarding. And when the sweep's single-stream
figure was multiplied by three to project a three-process rate, the correction
factor was sitting in the sweep's own output — per-stream throughput falls from
359 to 148 tok/s between one process and three, on that very card, measured that
very afternoon.

The rule that would have caught all three is not statistical. It is: **before
dividing two numbers, check that their denominators are the same thing.**

## What to do with it

On this task, on these two models, MTP is worth roughly a doubling of throughput
for no accuracy cost that 10,000 notes can detect. It perturbs 26% of outputs and
they do not get worse.

Two caveats, both load-bearing. It is **repeatable but not identical**, so arms
run with it cannot be compared against arms run without it — an MTP figure and a
sequential figure are different configurations, not different measurements of one
thing. And the speedup is a property of the model and the backend, not of the
feature: 1.59× for E2B and 1.83× for E4B at one process, and a curve that keeps
climbing on Vulkan where it flattens on CUDA.

## What still needs measuring

1. **E4B Q6 and Q8.** Two of six pairs outstanding. If either breaks the null the
   headline changes.
2. **Bootstrap intervals on the other five pairs.** Only E4B Q4 has one. The rest
   are point estimates, and this project has already been burned by treating a
   fixed number as a significance threshold when it was a measured effect from an
   unrelated experiment.
3. **Whether the null is an average of two opposite effects.** On a related
   question — reasoning versus not — an aggregate null over this corpus turned out
   to hide +0.24 F1 on one subset and −0.02 on another, cancelling. Nobody has
   split the MTP pairs by note category to check whether the same thing is
   happening here.
4. **A third model family.** Both models tested are gemma-4, and only gemma-4
   publishes an MTP draft in this field.
