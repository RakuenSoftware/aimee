# My benchmark lied to me nine times and the evidence was always already on disk

DRAFT.

Every wrong answer I published in this project came from an instrument biased in
the direction I wanted. In eight of the nine, the data that would have caught it was
already sitting in my own output. The one that had no such column is the cheapest to
prevent and the most expensive to find.

That is the finding. Not "measure carefully". The repeatable failure is that a
benchmark computes more than it prints, and the discarded column is the one that
would have stopped you.

You are watching the model. Sixteen of them, a ladder of quants, an accuracy column.
What is moving your numbers is a process count, a startup timer, and a threshold you
borrowed from an unrelated experiment.

Nine instances, each with its correction, because a defect list without fixes is a
confession rather than a method. The last three came from renting hardware, which
did not introduce a new failure mode. It gave the existing ones a meter.

## One: throughput that grew with the variable under test

I measured throughput as rows divided by wall clock. Wall clock includes server
startup, and startup grows with process count:

| card | nproc=1 | nproc=2 | nproc=3 | nproc=4 |
|---|---:|---:|---:|---:|
| RTX 5080 | 56 s | 84 s | 107 s | 137 s |
| RX 7900 XTX | 61 s | 67 s | 83 s | 99 s |

On a 200-note sweep that is a third to a half of the measurement. I reported two
conclusions from it: throughput peaks at two processes and declines, and four
processes are slower than one. Both wrong, both in the direction the bias pointed.

**Fix:** compute throughput from per-request latency times process count. Startup
is excluded by construction. The curve plateaus instead of falling and nproc=4 is
30% to 100% faster than nproc=1.

**Already on disk:** the sweep computed the startup column and discarded it.

## Two: fifteen processes I thought I had killed

An arm was running at 8.8 notes/min. I read that as the card. Fifteen orphaned
client processes from previously killed runs were still issuing requests to the
same three ports. Every request was served correctly. It queued. The server's own
timings looked healthy, so only the client saw the cost.

Killing them took the identical in-flight arm to 38.7 notes/min.

**Fix:** the cleanup handler now reaps children on EXIT, INT and TERM. And the
diagnostic that matters is one line: `ps -eo pid,cmd | grep -c run_llamacpp.py`,
compared against the process count you expect.

**Already on disk:** a load average of 27 for six hours, on a machine whose only
job was shuttling JSON over three SSH tunnels.

## Three: a significance threshold that was somebody else's effect size

I used 0.0105 all campaign to decide whether a difference was real. It is not an
interval. It is finding 19's measured effect of process count on F1, from an
unrelated experiment, and I promoted it to a threshold because it was a number of
about the right size.

The real interval at n=1001 is near ±0.024. Every ranking claim I made with a gap
between those two figures was unsupported.

Then I compounded it by proposing ±0.024 as a new universal threshold, which is
the same mistake with a better number.

**Fix:** every comparison gets its own paired bootstrap. No shared constant. It
costs seconds.

**Already on disk:** the bootstrap tool. I wrote it, then kept using the constant.

## Four: a subset is not a run

I ran one model on 1,001 notes, then on 3,002 notes that strictly contain those
1,001. Same card, same quant, same process count, same prompt.

| | |
|---|---:|
| independently run 1,001 arm | 0.6406 |
| the same 1,001 notes inside the 3,002 arm | 0.6327 |
| **byte-identical completions** | **529 / 1001** |

47% of outputs differ on identical inputs in an identical configuration.

The obvious explanation is the preceding note. It does not survive its own test:
notes with the same predecessor churned 44.8%, notes with a different one churned
48.3%. The prompt cache holds roughly 38 entries, so the carried state is the last
38 notes rather than the last one, which predicts uniform churn. That is
consistent with the mechanism and is not a measurement of it.

**Fix, and this one surprised me:** disable the prompt cache for comparability
arms. I had recorded that as too expensive on the reasoning that the 600-token
system prompt would be re-evaluated per note. Measured, it costs nothing: 38
minutes against 41 for the same 1,001 notes. The run I declined for two days as
unaffordable was free.

**Consequence:** a table that ranks natively-run arms against arms extracted from
a larger run is comparing two different things. Mine did. The effect is −0.0079,
inside the interval, so no ranking moved. It was luck, not design.

## Five: a constant from 70 notes reached three source files

The justification for enabling the model's reasoning pass was a constant that
appeared in `kb_curator_provider.c`, in `provider_client.c`, and in the commit
messages that introduced both: **thinking is worth +0.084 F1 to E4B.**

Its provenance was 53 true positives across about 70 notes, with no interval.

Re-measured paired over 955 notes: **+0.0103, 95% interval [−0.0201, +0.0404].**
The constant was eight times its own re-measured value and only the sign survived.

**Fix:** an interval beside the number, in the source comment, or the number does
not go in the source.

**Already on disk:** nothing, and that is the point. This one had no discarded
column, because no interval was ever computed. It is the cheapest of the six to
prevent and the most expensive to find, because a number in a source file has no
provenance attached to it at all.

## Six: I fitted a story to my own house theme

Splitting the paired arms by category, three categories scored exactly 0.0000 in
every arm. I wrote that up as a structural blind spot in F1: a third of the
corpus unscoreable, correct restraint worth nothing.

Two checks against my own source killed it. The scorer already emits `null`
rather than 0.0 for factless categories, with a comment explaining that printing
0.0 inverts the meaning. My analysis script printed 0.0 and reintroduced exactly
the bug the scorer was written to avoid. And the rows are not invisible to F1 at
all: zeroing their false positives is worth **+0.040 to +0.053**.

By then I had five findings of the form "F1 is blind to X". The shape was
familiar, the data fit it, and I published before checking. Pattern-matching to
your own house theme is faster than the checks that catch it.

**Fix:** the metric I thought I was introducing has been in every score file all
along, under `over_extraction`. I rediscovered a number that was already there,
which is the fourth time in this project.

## Seven: a successful response that said I owned nothing

`GET /api/v0/instances/` returned `{"instances": []}`. Not an error. A parseable,
200-status, empty fleet.

I recorded that in my own operational notes as "intermittently returns empty while
instances are demonstrably running" and moved on, because I had rentals I could see
working and a list endpoint that disagreed with them.

Ask the same endpoint with a query string and it stops lying:

    {"success":false,"error":"deprecated_endpoint",
     "msg":"/api/v0/instances/ is deprecated. Use /api/v1/instances/ instead."}

The v1 endpoint reported **25 running instances billing $2.68 an hour, two of which
were doing work.** The rest were overnight leaks: containers that failed to start,
hosts that had exited, and one still serving a model whose arm had finished and been
committed hours earlier.

**Fix:** the check that finds a leak does not need the provider to cooperate. An
instance whose `start_date` precedes the start time of the oldest running
orchestrator process cannot be owned by one. That is a comparison between two facts,
not a timeout, and it identified four ten-hour-old instances immediately.

**Already on disk:** the deprecation notice, behind a query string, in the same
endpoint I was already calling. I diagnosed the symptom, wrote the symptom into my
notes as if it were the cause, and then trusted my own note for eleven hours.

## Eight: four timeouts, each wrong in the same direction

I decided a rented host was stuck four separate times, at 420 seconds, then 900, then
600 from container start, then a 3,600 second cap. Every one of them abandoned hosts
that were working.

The bias has a shape. Download time scales with model size and my deadline did not,
so **the false-positive rate grew with the variable under test.** That is the same
defect as the throughput timer in section one, one layer up and with a bill attached:
because the pool re-places on timeout, each large arm burned a full deadline of
billing and then restarted the download somewhere else, forever.

**Fix:** none of the signals that actually distinguish a dead host from a slow one is
a duration. The instance reports `exited`. The container reports a docker daemon
error. The instance disappears from the API. Or `disk_util` sits at −1 with no status
message while a sibling placed thirty minutes later is already reporting progress.
Those are categorical, and the last one is a comparison rather than a clock.

**Already on disk:** every one of those fields, in the same API response I was
timing.

## Nine: I inferred a mechanism from a number

Qwen3.6-35B-A3B ran at 234 tok/s. I decided a 35B model could not do that without
speculative decoding, labelled both Qwen arms "native MTP" in my notes, and reported
it that way three times.

`/props` says `speculative: null`. No row in either prediction file carries a
`draft_n` counter. Qwen3.6 publishes no MTP draft in that repository at all. The real
mechanism is that roughly 3B of its 35B parameters are active per token, so it reads
about a tenth as much memory as a dense model of its size.

**Fix:** I had already added `draft_n` and `draft_n_accepted` to every prediction row,
specifically because wall clock confounds the feature with the host and the model.
Then I read the throughput column anyway and explained it with a feature the model
does not have. Recording a mechanism is not the same as consulting it.

**Already on disk:** in every row of both files, in the field I added for this exact
purpose.

## Print the denominators your harness already has

A benchmark computes more than it prints. The discarded column is the one that
catches you, and in eight of the nine cases here it was already being computed. The
one exception had no interval to discard, which is worse.

So the practice is not "be careful". It is:

**Print the denominators.** Process count, startup time, sample size, client count,
slot count, draft acceptance. Most of these were a number the harness had and did not
show.

**Never share a threshold across experiments.** A number that was an effect size
in one place is not a significance bound in another, however similar its
magnitude.

**Read your own scorer before writing about its limits.** Twice now the tool had
already handled the case I was about to report as unhandled, and had a comment
saying why.

**A subset is not a run**, unless you have turned the cache off and checked.

**Never encode a duration as a diagnosis.** Four times I turned "this is taking a
while" into a constant. A clock cannot tell a slow host from a dead one, and the
signals that can are all categorical.

**When a response says you own nothing, prove it before believing it.** A successful
empty answer is the most expensive failure mode available, because nothing about it
looks like a failure.

Each of those is one line of output or one hour of work. I spent about a week, and
roughly forty dollars of rented GPU time, on the errors they would have prevented.
