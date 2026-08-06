# The parallelism limit was never VRAM, and neither was the cost

DRAFT.

I run this benchmark on two consumer cards: an RTX 5080 with 16 GiB on CUDA and an
RX 7900 XTX with 24 GiB on Vulkan. For weeks I sized the number of concurrent model
processes by what I believed fitted in VRAM.

The limit was a per-process default I had never set. Eight GiB of KV cache, per
process, reserved whether the workload needed it or not.

Then the models got too big for both cards, I started renting GPUs, and the same
mistake happened again one layer up.

## The number you are watching is total VRAM

You look at a 5 GB model file and a 16 GiB card and conclude three copies fit. Then
two copies fail to start, and you conclude the model is bigger in memory than on
disk, which is true and is not what stopped you.

What binds is the context allocation per process. Set the context size to what your
prompts actually use and the arithmetic changes completely. My extraction prompts
run a few hundred to a few thousand tokens against a default sized for tens of
thousands.

## The backends do not have the same shape

Steady-state throughput in notes per minute, computed from per-request latency
times process count so that server startup is excluded:

| processes | RTX 5080, CUDA | RX 7900 XTX, Vulkan |
|---|---:|---:|
| 1 | 47.6 | 40.7 |
| 2 | 67.4 | 63.8 |
| 3 | 59.9 | 78.1 |
| 4 | 61.8 | **83.3** |

CUDA flattens after two processes. Vulkan is still climbing at four, and passes the
faster card doing it.

I ran three on both, a number chosen by what fitted in VRAM rather than by where the
returns stop. On the 5080 that is past the plateau. On the XTX it is short of it.

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

At one process the 5080 serves 359 tok/s. At three processes it serves 148 tok/s per
stream.

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

## Model size decides the card, and the quant decides the model size

Once the models passed 12B, VRAM stopped being a tuning parameter and became a gate.

| model | file | fits 5080 (15.92 GiB free) | fits XTX (24 GiB) |
|---|---:|---|---|
| gemma-4-12B QAT | 6.26 GiB | yes | yes |
| gemma-4-26B-A4B QAT | 13.27 GiB | **yes** | yes |
| gemma-4-26B-A4B non-QAT | 15.84 GiB | **no** | yes |
| gemma-4-31B QAT | 16.10 GiB | no | yes |
| Qwen3.6-27B | 16.40 GiB | no | yes |
| gemma-4-31B non-QAT | 17.53 GiB | no | yes |

Read the middle two rows together. Same model, same nominal four bits, and
quantisation-aware training moves it across a hardware boundary. On a 16 GB card the
choice is not QAT versus non-QAT. It is QAT versus a different model.

That arm then ran at **323 tok/s, the fastest in this project**, on the smaller of
my two cards, scoring 0.6804. The card I had written off as too small for large
models was running the third-best arm in the field.

## The mixture of experts does not rescue you, and then it does

An 8B model with about 1B active parameters sounds like it should behave like a 1B
model. It does not, in the way that binds here: **all experts stay resident.**
LFM2.5-8B-A1B at Q4_K_M is 5.16 GB and three copies are 15.5 GB of a 16,303 MiB card
before any KV cache. Q6 and Q8 are 20.9 and 27.0 GB for three copies and do not run.

So that arm runs at a different process count from the rest of the field, which makes
it incomparable to the ranking by construction. Process count is worth about 0.0105
F1 in this harness.

Sparsity buys you nothing on VRAM. What it buys is bandwidth, and at scale that turns
out to be the larger prize:

| model | active params | tok/s | GPU |
|---|---|---:|---|
| Qwen3.6-35B-A3B | ~3B of 35B | **234.0** | RTX 5090 |
| Qwen3.6-27B dense | 27B | 67.8 | RTX 5090 |

**3.5 times faster, same family, same quant, same card class, and the two write
almost the same amount of text.** A dense 27B at Q4 reads about 16.4 GiB of weights
per token; the MoE reads roughly a tenth of that. Plan VRAM by total parameters and
throughput by active parameters. They are different numbers and both matter.

## Renting is cheap and the meter is not where you are looking

Four completed 1,001-note arms on rented hardware:

| arm | GPU | $/hr | hours | cost |
|---|---|---:|---:|---:|
| gemma-4-31B non-QAT | RTX 5090 | 0.360 | 1.50 | **$0.54** |
| Qwen3.6-35B-A3B | RTX 5090 | 0.337 | 1.31 | $0.44 |
| gemma-4-12B QAT | RTX 3090 | 0.130 | 1.46 | $0.19 |
| gemma-4-26B google | RTX 3090 | 0.111 | 1.25 | $0.14 |

Under a dollar to run a 31B model over a thousand notes. That is not the number that
cost me money.

I woke up to **25 rented instances billing $2.68 an hour, of which two were doing
work.** The rest were leaks from overnight: containers that failed to start, hosts
that exited, and one still serving a model whose arm had finished and banked hours
earlier.

The reason I could not see them is the useful part. `GET /api/v0/instances/` returns
`{"instances": []}`. Not an error. A successful-looking empty fleet. Ask the same
endpoint with a query string and it says what it actually means:
`deprecated_endpoint, use /api/v1/instances/`. I had recorded the empty array in my
own notes as "intermittently returns empty" and moved on, which is the same failure
as every timeout in this project: I wrote down the symptom.

**An endpoint that reports "you own nothing" is the most expensive failure mode
available, because nothing about it looks like a failure.**

The check that finds a leak does not need the provider to cooperate. An instance
whose `start_date` precedes the start time of the oldest running orchestrator process
cannot be owned by one. That is evidence, not a clock, and it identified four
ten-hour-old instances immediately.

## Do not put a clock on a download

Four times I decided a rented host was stuck because it had taken too long: 420
seconds, then 900, then 600 from container start, then a 3,600 second cap. Every one
of them abandoned working hosts, and the false-positive rate **grew with model size**,
because download time scales with the file and my deadline did not. Since the pool
re-places on timeout, each large arm burned a full deadline of billing and then
restarted the download somewhere else, forever.

The signals that actually distinguish a dead host from a slow one are all
categorical: the instance reports `exited`, the container reports a docker daemon
error, the instance disappears from the API, or `disk_util` sits at −1 with no
status message while a sibling placed later is already reporting progress. None of
those is a duration.

## Load time is a real budget line

On GPU, the largest quantisation of E4B took **420 seconds** to load before serving
its first note. Run several models in sequence and that is a meaningful part of an
overnight campaign, and it appears in no accuracy column anywhere.

## Two sessions, one GPU

Two automated sessions targeting the same card do not queue. They contend, both slow
to a crawl, and neither reports anything wrong, because from each side every request
is being served correctly.

The same failure with a longer fuse: killing a run by pattern kills the driver and
leaves its client processes alive. Fifteen of those, still issuing requests to ports
a later run reused, held a live arm at 8.8 notes/min instead of 38.7. The only
visible symptom was a load average of 27.

And the version I hit most recently, which is the local twin of the rented leak: a
finished arm's server stayed resident on the 5080 holding 14,828 of 16,303 MiB. The
next arm's server asked for 6,390 MiB, died with `cudaMalloc failed: out of memory`,
and my launcher sat printing `sizing`. **A card at high VRAM and zero utilisation
with no client attached means the same thing locally that it means on a rented box.**

**The fixes are cheap.** Reap children on EXIT, INT and TERM rather than by pattern.
Whatever starts a server stops it, in the script that started it. Clearing a port
range proves nothing about a card.

## Set the context size yourself, and measure your own process curve

**Set your context size explicitly.** The default is sized for a workload that is
probably not yours, and it is reserved per process.

**Measure the process-count curve on your own backend.** CUDA and Vulkan diverge past
two processes on my hardware, and the faster card loses.

**Compute throughput from latency times process count.** Never rows over wall clock,
unless your arms are long enough that startup is rounding.

**Size VRAM by total parameters and speed by active parameters**, and check whether a
QAT build moves your model down a tier before you buy a bigger card.

**Rent by the hour and check the bill against the fleet, not against your intent.**
Renting a 31B arm costs under a dollar. Forgetting one costs more than running it.

## Where the returns stop, and the Vulkan gap I never closed

Where the returns actually stop. Everything above is one to four processes, and the
six-process cap is inherited from an assumption.

The cross-backend accuracy bound. I calibrated a rented RTX 3090 against my local
5080 at **+0.0057 F1, CI [−0.0136, +0.0251]**, byte identity 640/1001. That is CUDA
to CUDA. The XTX runs Vulkan on a different llama.cpp build, and I have never
measured it against either. Every number in this article that crosses that boundary
inherits an unquantified term, which is why the two quant pairs now running keep each
pair on a single card.
