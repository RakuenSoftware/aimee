# The parallelism limit was never VRAM

ROUGH DRAFT.

I run this benchmark on two consumer cards: an RTX 5080 with 16 GiB on CUDA and an
RX 7900 XTX with 24 GiB on Vulkan. For weeks I sized the number of concurrent
model processes by what I believed fitted in VRAM.

The limit was a per-process default I had never set. Eight GiB of KV cache, per
process, reserved whether or not the workload needed it.

## The number you are watching is total VRAM

You look at a 5 GB model file and a 16 GiB card and conclude three copies fit.
Then two copies fail to start, and you conclude the model is bigger in memory than
on disk, which is true and is not what stopped you.

What binds is the context allocation per process. Set the context size to what
your prompts actually use and the arithmetic changes completely. My extraction
prompts run a few hundred to a few thousand tokens against a default sized for
tens of thousands.

## The backends do not have the same shape

Steady-state throughput in notes per minute, computed from per-request latency
times process count so that server startup is excluded:

| processes | RTX 5080 (CUDA) | RX 7900 XTX (Vulkan) |
|---|---:|---:|
| 1 | 47.6 | 40.7 |
| 2 | 67.4 | 63.8 |
| 3 | 59.9 | 78.1 |
| 4 | 61.8 | **83.3** |

CUDA flattens after two processes. Vulkan is still climbing at four, and passes
the faster card doing it.

I run three on both, a number chosen by what fitted in VRAM rather than by where
the returns stop. On the 5080 that is past the plateau. On the XTX it is short of
it.

**The cap is untested above four.** My runner refuses more than six on the
assumption that the card is bandwidth-saturated by then. That assumption has never
been measured and I would not defend it.

## The fast configuration is the one you cannot use

Thirty-two slots in one process is 4.54x, far beyond anything process isolation
reaches. It is also not reproducible: two runs of that configuration agree on 63 of
100 raw completions and 75 of 100 extracted fact sets.

Slots batch requests into a shared forward pass, so a sequence's logits depend on
which other requests happen to be in flight beside it. Isolated processes do not
share a matrix multiply, so contention between them costs time and changes no
arithmetic.

That is why this benchmark runs N single-slot servers rather than one server with N
slots, and pays roughly half the available speed for it. The corpus is split
round-robin rather than in blocks, because it is ordered by domain and a contiguous
split would hand one shard every negation note and another every infrastructure
note, and they would finish hours apart.

## Per-stream throughput falls, and that is the number people project with

At one process the 5080 serves 359 tok/s. At three processes it serves 148 tok/s
per stream.

Multiply a single-stream benchmark figure by your process count and you will
overstate by more than a factor of two. I did this, in public, to project a
three-process rate from a one-process measurement, and the correction factor was
sitting in the same output file.

## Startup is not free and it scales the wrong way

| card | nproc=1 | nproc=2 | nproc=3 | nproc=4 |
|---|---:|---:|---:|---:|
| RTX 5080 | 56 s | 84 s | 107 s | 137 s |
| RX 7900 XTX | 61 s | 67 s | 83 s | 99 s |

About 30 seconds per server. On a 10,000-note arm that is rounding. On a 200-note
sweep it is a third to a half of the wall clock, and if you measure throughput as
rows over wall clock you have built a bias that grows with the variable you are
testing. I did, and it produced two wrong conclusions before I caught it.

## The mixture of experts does not rescue you

An 8B model with about 1B active parameters sounds like it should behave like a
1B model. It does not, in the way that matters here: all experts stay resident.
Q4_K_M is 5.16 GB and three copies are 15.5 GB of a 16,303 MiB card before any KV
cache at all. Q6 and Q8 are 20.9 and 27.0 GB for three copies and do not run.

So that arm runs at a different process count from the rest of the field, which
makes it incomparable to the ranking table by construction. Process count is worth
about 0.0105 F1 in this harness. I say so where the number appears rather than
letting it sit in a ranking it does not belong in.

## Load time is a real budget line

On GPU, the largest quantisation of E4B took **420 seconds** to load before serving
its first note. Run several models in sequence and that is a meaningful part of an
overnight campaign, and it appears in no accuracy column anywhere.

## Two sessions, one GPU

Two automated sessions targeting the same card do not queue. They contend, both
slow to a crawl, and neither reports anything wrong, because from each side every
request is being served correctly.

The same failure with a longer fuse: killing a run by pattern kills the driver and
leaves its client processes alive. Fifteen of those, still issuing requests to
ports a later run reused, held a live arm at 8.8 notes/min instead of 38.7. The
only visible symptom was a load average of 27.

**Both fixes are cheap.** Reap children on EXIT, INT and TERM rather than by
pattern. And before every run, count clients and compare against what you expect:
`ps -eo pid,cmd | grep -c run_llamacpp.py`.

## What to do

**Set your context size explicitly.** The default is sized for a workload that is
probably not yours, and it is reserved per process.

**Measure the process-count curve on your own backend.** CUDA and Vulkan diverge
past two processes on my hardware, and the faster card loses.

**Compute throughput from latency times process count.** Never rows over wall
clock, unless your arms are long enough that startup is rounding.

**Count your clients before and after every run.** One line of shell. It is the
diagnostic I most wish I had had six hours earlier.

## What I have not measured

Where the returns actually stop. Everything above is one to four processes; the
six-process cap is inherited from an assumption. And the whole table is two cards
from two vendors on one workload, which is enough to show the backends differ and
not enough to tell you what yours will do.
