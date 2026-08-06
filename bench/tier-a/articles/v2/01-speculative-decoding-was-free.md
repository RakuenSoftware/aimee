# Speculative decoding doubled throughput and cost nothing I could measure

DRAFT. All six paired runs are banked. The acceptance figures are read from the
server's own counters rather than inferred from wall clock.

A small, fast model guesses the next few words. The big model checks all of those
guesses in one pass instead of producing them one at a time, and every guess it
agrees with is a word you got for free. That is speculative decoding, and on this
extraction task it more than doubles throughput. It changes 26% of the output
text. It changes accuracy by an amount I can bound inside four thousandths of a
point on a 0 to 1 scale.

That is a free lunch, which is the kind of claim I should distrust, and I measured
it wrong twice before I measured it right.

Two words the rest of this needs. gemma-4 ships the small model the technique
requires, which llama.cpp calls multi-token prediction (MTP). The share of guesses
the big model keeps is the acceptance rate, and it is the number I should have
been reading all along.

Then I ran a model that does no guessing at all and it beat every model that does.

## The number you are watching is the wrong one

Everyone reporting speculative decoding reports a speedup multiple. I reported
5.3x, then 1.58x, and both were properties of my instrument rather than of the
feature.

The number that matters is the pair. One card, one set of notes, one process
count, the guessing model as the only difference between two runs:

| model | quant | guessing on | guessing off | score change | steady throughput |
|---|---|---:|---:|---:|---:|
| E2B | Q4 | 0.6246 | 0.6207 | +0.0039 | +84.0% |
| E2B | Q6 | 0.6344 | 0.6331 | +0.0013 | +91.6% |
| E2B | Q8 | 0.6329 | 0.6351 | −0.0022 | +102.5% |
| E4B | Q4 | 0.6301 | 0.6306 | −0.0005 | +110.6% |
| E4B | Q6 | 0.6452 | 0.6435 | +0.0017 | +116.2% |
| E4B | Q8 | 0.6337 | 0.6327 | +0.0010 | **+131.3%** |

10,000 notes per run, three processes, RX 7900 XTX. The accuracy differences
scatter around zero, the sign flips three times, and the largest is 0.0039.
Throughput climbs the whole way and never stops climbing.

For three of the six I resampled those 10,000 notes 20,000 times and scored both
settings on the same draw each time, which gives a range the true difference sits
inside:

> E4B Q4: off − on = **+0.0005**, range −0.0028 to +0.0036
> E4B Q6: off − on = **−0.0017**, range −0.0048 to +0.0013
> E4B Q8: off − on = **−0.0010**, range −0.0041 to +0.0021

None of those says "I cannot tell". Each says *the effect is smaller than five
thousandths in either direction*. Bounding an effect tightly around zero is a
stronger result than failing to find one, and it took 60,000 notes to buy three.

The gain rises with quant size inside each family, which is what you would expect
from where the time actually goes. Generating a word means reading the whole model
out of memory, and on these cards that read is slower than the arithmetic, so the
compute sits idle waiting. A bigger quant is a bigger read and more idle time for
the guessing to fill. Q8 gains most because it is the most expensive to read.

## Measure the mechanism, not its shadow

Wall clock mixes the feature up with the host, the model and the backend. The
mechanism is two counters the server already keeps: how many words the small model
proposed, and how many the big one kept. I had been reporting the shadow for
months.

Six large runs, every one with acceptance recorded:

| run | words guessed | kept |
|---|---:|---:|
| gemma-4-12B non-QAT | 1,510,235 | 82.0% |
| gemma-4-12B QAT | 1,414,986 | 81.2% |
| gemma-4-26B-A4B unsloth QAT | 1,360,556 | 79.2% |
| gemma-4-26B-A4B google q4_0 | 1,367,766 | 79.1% |
| gemma-4-31B QAT | 539,715 | 79.1% |
| gemma-4-31B non-QAT | 620,046 | 78.5% |

**Acceptance tracks the model and ignores the quant.** Each pair is within a point
of itself across quant schemes that differ by up to 0.023 in score. So the two
choices are independent: pick the quant on accuracy and file size, then turn
guessing on separately, and neither decision constrains the other.

Acceptance also falls slowly with size, 82% at 12B to 78.5% at 31B, which is the
opposite of the wall-clock story. The 31B gains *more* wall clock from guessing
than the 12B while keeping *fewer* of the guesses, because its bigger read leaves
more idle time to reclaim. Reporting speedup alone would have shown one number and
hidden both.

## It is not output-identical, and that is the interesting part

Speculative decoding is supposed to change nothing. Every guess is checked against
the big model, so a guess that is kept is the word the big model would have
produced anyway.

Measured on 100 notes, with the randomness turned off and a fresh server each
time:

| | identical to the one-at-a-time run |
|---|---:|
| guessing off | 100/100 |
| guessing on | **74/100** |

Checking several guesses at once pushes them through the big model together rather
than one by one. That changes the shape of the arithmetic, which changes the order
the numbers are added in, and where two candidate words were nearly tied the
winner flips. Twenty-six notes in a hundred.

So the question is not whether the output moved. It moved. Whether it got worse is
what the table above answers, and the answer is no.

It also moves the **same way every time**. Two guessing runs against each other,
fresh server each: 100/100 on E4B and 100/100 on E2B. The arithmetic shape is set
by how many words are guessed at a time, not by anything outside the run. I checked E2B rather than assuming
it, because `--model` is only a label and a stale server would have loaded E4B
twice and produced a meaningless pass. `/props` confirmed the quant, and a median
latency of 1345 ms against E4B's 2548 ms confirmed it independently.

I checked one more way, because an average of zero can be two opposite effects
cancelling out. On a different question in this project an average of zero over
these same notes turned out to be +0.24 on one subset and −0.02 on another. So I
split all four pairs by note category. Largest single movement: +0.0220 on
implicit, over 723 notes, inside the ±0.024 that many notes can resolve. No
category moves further than its own range allows. The zero holds all the way down.

## A model that does no guessing beat every model that does

Qwen3.6-35B-A3B ran at **234 words/s with no speculative decoding at all.** The dense
gemma-4-12B, running a draft head at 82% acceptance on a comparable card, managed
195.8.

I had both Qwen runs labelled as guessing in my own notes for several hours,
because 234 words a second on a 35B model looked impossible without it. The server
reports it as off. No row in either output file carries a count of guessed words.
Qwen3.6 ships no guessing model in that repository. I inferred a mechanism from a number
and the inference outlived three status reports before I checked the field that
was sitting in every row.

The real mechanism is architecture. A mixture of experts (MoE) keeps all 35B in
memory but only runs about 3B of it for any given word, so it reads about 1.5 GiB
per word against a card that can move 1.79 TB every second. Its own dense sibling,
which runs all of itself every time, makes the point with no guessing involved on
either side:

| Qwen3.6, same family, same quant, same card class | words/s | median completion |
|---|---:|---:|
| 35B-A3B, mixture of experts | **234.0** | 1,100 words |
| 27B dense | 67.8 | 1,256 words |

**3.5 times faster, writing the same amount of text.** The dense 27B reads about
16.4 GiB per word; the sparse one reads roughly a tenth of that. It costs nothing
in accuracy: on these notes the two are a tie, −0.0106, with a range of −0.0294 to
+0.0088 that comfortably contains zero.

So the ranking is: guessing is worth about 2x, and picking a sparse architecture is
worth 3.5x. If you are optimising throughput and you can choose
the model, choose the model first. Speculation is what you turn on afterwards, on
whatever you chose.

## The 5.3x was two numbers with different denominators

The first figure came from dividing 68.5 notes a minute, a finished run with
guessing on, by about 13 notes a minute, a run with it off that I sampled while it
was still starting up.

Worse, the denominator was contaminated. Fifteen orphaned client processes from
runs I had killed earlier were still issuing requests to the same three ports the
live run was using. Every request was served correctly. It simply queued. The
server's own timings looked healthy and only the client saw the cost. Killing the
orphans took the identical in-flight run from 8.8 to 38.7 notes/min.

The tell had been visible for six hours: a load average of 27 on a machine whose
only job was shuttling JSON over three SSH tunnels. Nothing in my harness looks at
load, and no diagnostic printed the client count.

## The 1.58x measured startup and called it throughput

The second attempt was a real experiment. Two eight-configuration sweeps, one per
card, 200 notes each, process counts 1 through 4, with guessing on and off.

Its throughput metric was rows divided by wall clock, and wall clock includes
server startup. Startup is about 30 seconds per server, so it grew with the
variable under test:

| card | 1 process | 2 processes | 3 processes | 4 processes |
|---|---:|---:|---:|---:|
| RTX 5080 | 56 s | 84 s | 107 s | 137 s |
| RX 7900 XTX | 61 s | 67 s | 83 s | 99 s |

On a 200-note run that is a third to a half of the wall clock, and the bias pointed
the same way as the hypothesis. It produced two confident wrong conclusions I
reported before catching them: that aggregate throughput peaks at two processes and
declines, and that four processes are slower than one.

Compute throughput from per-request latency and process count instead, which
excludes startup by construction, and the curve plateaus rather than falling.
Four processes is 30% to 100% faster than one.

## Before dividing two numbers, check the denominators are the same thing

That rule would have caught both wrong answers, and it is not a statistical one.

Each time, the data that would have caught me already existed. The 5.3x needed a
process count. The sweep needed its own startup column, which it computed and
discarded. And when I multiplied a single-stream figure by three to project a
three-process rate, the correction factor was in that sweep's output: per-stream
throughput falls from 359 to 148 words/s between one process and three, on that card,
that afternoon.

The Qwen mislabelling is the same failure with a different surface. The count of
guessed words was in every row of both files. I read the throughput column instead
and explained it with a feature the model does not have.

## Turn it on, then stop quoting a single speedup for it

Turn it on. On this task, across two model families and four sizes, it is worth
roughly a doubling of throughput for no accuracy cost that 10,000 notes can
detect, and it does not interact with your quant choice.

Three limits, all load-bearing.

**It is repeatable but not identical**, so a run with guessing on cannot be
compared against a run with it off. Those are different configurations, not two
measurements of one thing.

**The speedup belongs to the model and the backend, not to the feature:**

| model | one at a time | with guessing | ratio |
|---|---:|---:|---:|
| E4B UD-Q4_K_XL | 22.9 notes/min | 41.9 | **1.83x** |
| E2B UD-Q4_K_XL | 27.0 notes/min | 43.0 | **1.59x** |

A smaller model has a smaller read, so less of the card sits idle and there is less
for the guessing to reclaim. Quoting one number as "the speedup" would be wrong.

**It does not stack with running many requests at once.** Thirty-two at a time is
4.54x on its own; thirty-two at a time with guessing is **4.34x**, marginally
slower. Both fill the same idle capacity, and once thirty-two requests are in
flight there is none left, so the checking is added work with nowhere to hide.

I can only vouch for gemma-4. It is still the only family in this field that ships
a guessing model: I checked Qwen3.6 directly after mislabelling it, and the
repository has none.

## The zero is bounded, not explained

1. **Acceptance against accuracy, note by note.** I have an acceptance rate and a
   score for each run, but not whether the notes where the guessing fails are the
   notes where the model is wrong. That is the question that would explain the
   zero rather than merely bound it.
2. **A second family that ships a guessing model.** One family is a limit on the
   claim, not a gap I can close by running more gemma.
