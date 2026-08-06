# Renting GPUs for this benchmark: what broke, what is fixed, what is not

Everything here was learned in one afternoon of running eight arms on vast.ai.
The failures are recorded because every one of them looked like a host problem
and most of them were mine.

## What the rented path is

The rented box runs **only** `llama-server`. The corpus, the client, the prompt
and the scorer all stay local, and `run_llamacpp.py --base-url` drives the remote
server over HTTP. The GPU is then the single variable between a rented arm and a
local one, which is the property that makes the results comparable at all.

Calibration measured that variable rather than assuming it: rented RTX 3090
against the local 5080, identical configuration, **+0.0057 F1, 95% CI [-0.0136,
+0.0251]**, byte identity 640/1001, with tp and fn identical and the entire
difference in fp. So rented arms are comparable to the local field **to within
about +/-0.019 at n=1001**. State that bound; do not say "the same".

## Fixed, and why each one bit

**Offer id is not machine identity.** Several offers can be slots on one physical
box. Placing arms concurrently by walking a price-ordered list double-books a
single GPU: the first container takes it, the rest die with `failed to set up
container`. Five of six arms died this way in one round. The pool now claims
`machine_id` per arm and releases it on failure. This was invisible at
`--parallel 4` and deterministic at 8, so widening the fleet did not cause the
bug, it exposed one that had been costing arms quietly.

**A fixed pull deadline is the wrong instrument.** One host sat on `Retrying in 2
seconds` for fifteen minutes, and I encoded the fifteen minutes instead of the
stuck. At 420s it abandoned healthy hosts; raised to 900s it abandoned every
6-21 GiB arm, and since the pool re-places on timeout, each big arm burned a full
deadline of billing and restarted the download elsewhere, forever. Download time
scales with model size, so **the bias grew with the variable under test** -- the
same shape as defect 35, where server startup grew with process count and skewed
a throughput result in the direction the hypothesis wanted. Now: abandon a host
only when `status_msg` stops changing for 600s, with a 3600s hard cap.

**A docker daemon error is terminal, not slow.** Waiting out the stall window on
one costs ten minutes of billing to learn what the first poll already said. Fails
immediately now.

**Killing a pool used to leak its rentals.** SIGTERM leaves worker threads
mid-`run_arm`, so the `finally` that destroys the instance never runs. Ten
instances were found alive after their pools were gone, four of them orphans
billing against nothing. That is the orphaned-client defect from the local host,
one layer up and with a meter attached. The pool now tracks what it created and
reaps on SIGTERM/SIGINT.

**The logged price was the offer price.** `dph_total` on an offer excludes storage
and, on a bid rental, the bid premium. Logging it understated real burn by roughly
3x across a ten-instance fleet, always in the reassuring direction. The pool now
reads the billed rate back from the instance after start and uses it for the
per-arm cost line.

**`--parallel` defaulted to 4.** On an eight-arm list that left half the fleet
queued while it was described as running. Default is now one worker per job; the
real constraints are offer availability, budget, and one local HTTP client per
arm, none of which is a fixed number.

**The client needs an explicit thinking flag.** `run_llamacpp.py` requires
`--thinking` or `--no-thinking` and has no default, deliberately, so an arm cannot
silently inherit the wrong one. Omitting it in the pool cost an idle instance.

**Preemption is handled and has now actually fired.** The watchdog polls the
instance while the client runs, because a preempted arm otherwise hangs on a dead
endpoint until the client's 3600s timeout. A preempted arm is **discarded and
requeued whole** unless the job declares `resumable`, in which case the partial is
kept and resume narrows the CORPUS to unanswered ids rather than changing the
client.

**A stray signal to the launching shell tore down the fleet.** The pool is
started with `nohup setsid ... &`, but when the launching shell was killed -- a
tool timeout is enough -- the signal reached the whole process group, the pool's
own SIGTERM handler fired, and it dutifully destroyed every instance it had just
rented and exited. It looked like a crash. The reaper added to stop orphaned
rentals had made the pool fragile to exactly the thing that happens most often.
The pool now calls `os.setsid()` itself at startup and ignores SIGHUP, so
group-directed signals cannot reach it while a deliberate `kill <pid>` still can:
reap on purpose, survive by accident. It also writes a pidfile, because `pgrep`
has repeatedly matched the caller's own command line.

**The stall detector then fired on healthy hosts, for the same reason a third
time.** `status_msg` reaches its terminal value the moment the container starts;
llama.cpp then spends 10-30 minutes pulling a 16-21 GiB GGUF from HuggingFace and
vast narrates none of it. Watching status_msg for change therefore abandons every
large-model host at exactly `stall` seconds after container start. Eleven of the
twenty-seven re-placements in one hour were this, against twelve genuine container
failures. The wait has now been wrong three ways -- 420s, 900s, and
600s-from-container-start -- and each time the error was a clock measured from an
event unrelated to the work finishing. The stall timer now polices only the phases
vast actually narrates (created, loading); once the instance is `running` the sole
bound is `hard_cap` and the real completion signal is `/health` answering.

## Not fixed. Do these before the next campaign.

**1. Cap failures per JOB, not per host.** Defect 41 states the rule and nothing
implements it. `MiniCPM5-1B` failed identically on three hosts and the pool kept
re-placing, because each attempt looks like a fresh host problem in the log.
Three hosts showing one symptom is evidence about the model. Stop after the
second and mark the job unrunnable.

**2. Choose the GPU shape by bandwidth per dollar, and allow a fallback list.**
Generation here is memory-bandwidth-bound, so the metric is `$/hr per TB/s`:

| GPU | $/hr | VRAM | GB/s | $/hr per TB/s |
|---|---:|---:|---:|---:|
| RTX 3090 | 0.0896 | 24G | 936 | **0.0958** |
| RTX A5000 | 0.0742 | 24G | 768 | 0.0966 |
| RTX 5090 | 0.2289 | 32G | 1792 | 0.1277 |
| A100 SXM4 | 0.2681 | 40G | 2039 | 0.1315 |
| RTX 4090 | 0.1356 | 24G | 1008 | 0.1345 |

The 3090 is the best value; the 5090 is 1.9x the bandwidth for 2.6x the price,
which is the right trade when wall clock matters more than spend. `--gpu` takes
one name today, so a shape with no cheap offers fails to place instead of falling
back. It should take an ordered list.

**3. A spend ceiling with a kill switch.** Nothing stops a pool running all night.
It should take a budget, track billed rate x elapsed, and reap everything on
breach.

**4. The stall detector can be fooled** by a retry message containing a changing
counter or timestamp, which would look like progress until the hard cap. If that
appears, compare on the stable part of the message rather than reaching for a
shorter clock.

**5. `GET /api/v0/instances/` is DEPRECATED, not "intermittently empty".** I wrote
the intermittent version above after seeing it return an empty array while
instances were demonstrably running. That diagnosis was wrong in the same way as
the pull timeouts: I recorded the symptom. Asking it with a query string returns
the real answer, `{"success":false,"error":"deprecated_endpoint","msg":"... Use
/api/v1/instances/ instead"}`; asking it bare returns `{"instances": []}`, a
successful-looking empty fleet. **Use `/api/v1/instances/` to list.**

Per-contract `/api/v0/instances/<cid>/` still works and is what the pool uses, so
`endpoint`, `alive` and `billed_rate` were never affected. What WAS affected is my
ability to see the fleet at all, and believing the empty array hid 25 live
instances billing $2.68/hr while two of them were doing work. An endpoint that
reports "you own nothing" is the most expensive possible failure mode, because
nothing about it looks like a failure.

**A leaked instance is invisible from the harness side.** Nothing in the local
state, the locks, the pidfiles or the prediction files reveals an instance that no
pool owns. The only way to see one is to list the fleet and compare `start_date`
against the start time of the oldest live pool: an instance created before any
running pool cannot be owned by one. That comparison is evidence, not a clock, and
it is the check that should run every time the fleet is inspected.

**6. Image pull is the dominant per-arm cost for short work.** It runs 2-20
minutes and is billed. One host pulled the llama.cpp CUDA image in 84 seconds and
another advertising comparable bandwidth took fifteen. For a batch of short arms,
run several arms of one model per instance rather than one instance per arm; the
pool supports this via a job's `prompts` list.

**7. Record draft acceptance on every MTP arm.** `timings.draft_n` and
`draft_n_accepted` are what speculative decoding's speedup actually IS. Every MTP
figure in this project is wall clock, which confounds the mechanism with the host,
the model and the backend. The field exists now; nothing has consumed it yet.

## The recurring shape

Six of the failures above were pattern-matching failures: I saw one instance of a
problem and encoded the symptom rather than the cause. A host stuck for fifteen
minutes became a fifteen-minute timeout. An offer became a machine. A price became
the price. In each case the correct signal was already in the data being logged
and nothing read it, which is the same defect class this whole benchmark is about.

**Prediction files were deleted before being committed.** A fleet relaunch ran
`rm -f results/vast/*.mtp.live.pred.jsonl` "to be clean", destroying the arms
behind an already-committed finding. They had never been committed, so nothing was
recoverable. The pool truncates on restart anyway, so the deletion achieved
nothing at all.

**Rule: commit prediction files as soon as an arm produces rows, and never delete
a partial arm.** A partial arm is evidence; the harness knows how to ignore it
(the skip check compares row count against the gold tier) and a later reader can
be told what it is. Deleting it to keep a directory tidy trades a permanent loss
for a cosmetic gain.
