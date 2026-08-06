# Local LLMs: Speculative Decoding

All six paired runs are banked. The acceptance figures are read from the server's
own counters rather than inferred from wall clock.

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

<figure class="sg-figure"><input class="sg-figure__radio sg-figure__radio--chart" type="radio" name="fig-0" id="fig-0-chart" checked><input class="sg-figure__radio sg-figure__radio--table" type="radio" name="fig-0" id="fig-0-table"><div class="sg-figure__tabs"><label class="sg-figure__tab sg-figure__tab--chart" for="fig-0-chart">Chart</label><label class="sg-figure__tab sg-figure__tab--table" for="fig-0-table">Numbers</label></div><div class="sg-figure__panes"><div class="sg-figure__pane sg-figure__pane--chart"><svg class="sg-chart" viewBox="0 0 760 230" preserveAspectRatio="xMidYMid meet" role="img" aria-label="Differences against zero, one row per configuration"><line class="sg-chart__grid" x1="200.0" x2="200.0" y1="22" y2="192.0"/><text class="sg-chart__value" x="200.0" y="210.0" text-anchor="middle" opacity=".7">−0.006</text><line class="sg-chart__grid" x1="319.0" x2="319.0" y1="22" y2="192.0"/><text class="sg-chart__value" x="319.0" y="210.0" text-anchor="middle" opacity=".7">−0.003</text><line class="sg-chart__rule" x1="438.0" x2="438.0" y1="22" y2="192.0"/><text class="sg-chart__value" x="438.0" y="210.0" text-anchor="middle" opacity=".7">0</text><line class="sg-chart__grid" x1="557.0" x2="557.0" y1="22" y2="192.0"/><text class="sg-chart__value" x="557.0" y="210.0" text-anchor="middle" opacity=".7">+0.003</text><line class="sg-chart__grid" x1="676.0" x2="676.0" y1="22" y2="192.0"/><text class="sg-chart__value" x="676.0" y="210.0" text-anchor="middle" opacity=".7">+0.006</text><text class="sg-chart__axis" x="438.0" y="14" text-anchor="middle">NO CHANGE</text><text class="sg-chart__label" x="188" y="51.0" text-anchor="end">E2B Q4</text><line class="sg-chart__line sg-chart__line--muted" x1="438.0" x2="592.7" y1="47.0" y2="47.0"/><circle class="sg-chart__mark sg-chart__mark--1 sg-chart__ring" cx="592.7" cy="47.0" r="5"/><text class="sg-chart__value" x="686" y="51.0">+0.0039</text><text class="sg-chart__label" x="188" y="77.0" text-anchor="end">E2B Q6</text><line class="sg-chart__line sg-chart__line--muted" x1="438.0" x2="489.6" y1="73.0" y2="73.0"/><circle class="sg-chart__mark sg-chart__mark--1 sg-chart__ring" cx="489.6" cy="73.0" r="5"/><text class="sg-chart__value" x="686" y="77.0">+0.0013</text><text class="sg-chart__label" x="188" y="103.0" text-anchor="end">E2B Q8</text><line class="sg-chart__line sg-chart__line--muted" x1="438.0" x2="350.7" y1="99.0" y2="99.0"/><circle class="sg-chart__mark sg-chart__mark--1 sg-chart__ring" cx="350.7" cy="99.0" r="5"/><text class="sg-chart__value" x="686" y="103.0">−0.0022</text><text class="sg-chart__label" x="188" y="129.0" text-anchor="end">E4B Q4</text><line class="sg-chart__line sg-chart__line--muted" x1="438.0" x2="418.2" y1="125.0" y2="125.0"/><circle class="sg-chart__mark sg-chart__mark--1 sg-chart__ring" cx="418.2" cy="125.0" r="5"/><text class="sg-chart__value" x="686" y="129.0">−0.0005</text><text class="sg-chart__label" x="188" y="155.0" text-anchor="end">E4B Q6</text><line class="sg-chart__line sg-chart__line--muted" x1="438.0" x2="505.4" y1="151.0" y2="151.0"/><circle class="sg-chart__mark sg-chart__mark--1 sg-chart__ring" cx="505.4" cy="151.0" r="5"/><text class="sg-chart__value" x="686" y="155.0">+0.0017</text><text class="sg-chart__label" x="188" y="181.0" text-anchor="end">E4B Q8</text><line class="sg-chart__line sg-chart__line--muted" x1="438.0" x2="477.7" y1="177.0" y2="177.0"/><circle class="sg-chart__mark sg-chart__mark--1 sg-chart__ring" cx="477.7" cy="177.0" r="5"/><text class="sg-chart__value" x="686" y="181.0">+0.0010</text><text class="sg-chart__axis" x="438.0" y="224" text-anchor="middle">CHANGE IN SCORE, GUESSING ON MINUS OFF</text></svg></div><div class="sg-figure__pane sg-figure__pane--table"><table><thead><tr><th style="text-align:left">model</th><th style="text-align:left">quant</th><th style="text-align:right">guessing on</th><th style="text-align:right">guessing off</th><th style="text-align:right">score change</th><th style="text-align:right">steady throughput</th></tr></thead><tbody><tr><td style="text-align:left">E2B</td><td style="text-align:left">Q4</td><td style="text-align:right">0.6246</td><td style="text-align:right">0.6207</td><td style="text-align:right">+0.0039</td><td style="text-align:right">+84.0%</td></tr><tr><td style="text-align:left">E2B</td><td style="text-align:left">Q6</td><td style="text-align:right">0.6344</td><td style="text-align:right">0.6331</td><td style="text-align:right">+0.0013</td><td style="text-align:right">+91.6%</td></tr><tr><td style="text-align:left">E2B</td><td style="text-align:left">Q8</td><td style="text-align:right">0.6329</td><td style="text-align:right">0.6351</td><td style="text-align:right">−0.0022</td><td style="text-align:right">+102.5%</td></tr><tr><td style="text-align:left">E4B</td><td style="text-align:left">Q4</td><td style="text-align:right">0.6301</td><td style="text-align:right">0.6306</td><td style="text-align:right">−0.0005</td><td style="text-align:right">+110.6%</td></tr><tr><td style="text-align:left">E4B</td><td style="text-align:left">Q6</td><td style="text-align:right">0.6452</td><td style="text-align:right">0.6435</td><td style="text-align:right">+0.0017</td><td style="text-align:right">+116.2%</td></tr><tr><td style="text-align:left">E4B</td><td style="text-align:left">Q8</td><td style="text-align:right">0.6337</td><td style="text-align:right">0.6327</td><td style="text-align:right">+0.0010</td><td style="text-align:right"><strong>+131.3%</strong></td></tr></tbody></table></div></div><figcaption class="sg-figure__caption">The accuracy change from turning guessing on, for each model and quant. The sign flips three times and the largest move is 0.0039. Throughput for the same six is in the Numbers pane, where it climbs the whole way.</figcaption></figure>

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

<figure class="sg-figure"><input class="sg-figure__radio sg-figure__radio--chart" type="radio" name="fig-1" id="fig-1-chart" checked><input class="sg-figure__radio sg-figure__radio--table" type="radio" name="fig-1" id="fig-1-table"><div class="sg-figure__tabs"><label class="sg-figure__tab sg-figure__tab--chart" for="fig-1-chart">Chart</label><label class="sg-figure__tab sg-figure__tab--table" for="fig-1-table">Numbers</label></div><div class="sg-figure__panes"><div class="sg-figure__pane sg-figure__pane--chart"><svg class="sg-chart" viewBox="0 0 760 222" preserveAspectRatio="xMidYMid meet" role="img" aria-label="Values on a zoomed scale, one row per run"><line class="sg-chart__grid" x1="250.0" x2="250.0" y1="18" y2="186"/><text class="sg-chart__value" x="250.0" y="202" text-anchor="middle" opacity=".7">77</text><line class="sg-chart__grid" x1="359.0" x2="359.0" y1="18" y2="186"/><text class="sg-chart__value" x="359.0" y="202" text-anchor="middle" opacity=".7">78.5</text><line class="sg-chart__grid" x1="468.0" x2="468.0" y1="18" y2="186"/><text class="sg-chart__value" x="468.0" y="202" text-anchor="middle" opacity=".7">80</text><line class="sg-chart__grid" x1="577.0" x2="577.0" y1="18" y2="186"/><text class="sg-chart__value" x="577.0" y="202" text-anchor="middle" opacity=".7">81.5</text><line class="sg-chart__grid" x1="686.0" x2="686.0" y1="18" y2="186"/><text class="sg-chart__value" x="686.0" y="202" text-anchor="middle" opacity=".7">83</text><text class="sg-chart__label" x="238" y="43.0" text-anchor="end">gemma-4-12B non-QAT</text><circle class="sg-chart__mark sg-chart__mark--1 sg-chart__ring" cx="613.3" cy="39.0" r="5"/><text class="sg-chart__value" x="696.0" y="43.0">82.0%</text><text class="sg-chart__label" x="238" y="69.0" text-anchor="end">gemma-4-12B QAT</text><circle class="sg-chart__mark sg-chart__mark--1 sg-chart__ring" cx="555.2" cy="65.0" r="5"/><text class="sg-chart__value" x="696.0" y="69.0">81.2%</text><text class="sg-chart__label" x="238" y="95.0" text-anchor="end">gemma-4-26B-A4B unsloth QAT</text><circle class="sg-chart__mark sg-chart__mark--muted sg-chart__ring" cx="409.9" cy="91.0" r="5"/><text class="sg-chart__value" x="696.0" y="95.0">79.2%</text><text class="sg-chart__label" x="238" y="121.0" text-anchor="end">gemma-4-26B-A4B google q4_0</text><circle class="sg-chart__mark sg-chart__mark--muted sg-chart__ring" cx="402.6" cy="117.0" r="5"/><text class="sg-chart__value" x="696.0" y="121.0">79.1%</text><text class="sg-chart__label" x="238" y="147.0" text-anchor="end">gemma-4-31B QAT</text><circle class="sg-chart__mark sg-chart__mark--muted sg-chart__ring" cx="402.6" cy="143.0" r="5"/><text class="sg-chart__value" x="696.0" y="147.0">79.1%</text><text class="sg-chart__label" x="238" y="173.0" text-anchor="end">gemma-4-31B non-QAT</text><circle class="sg-chart__mark sg-chart__mark--muted sg-chart__ring" cx="359.0" cy="169.0" r="5"/><text class="sg-chart__value" x="696.0" y="173.0">78.5%</text><text class="sg-chart__axis" x="468.0" y="216" text-anchor="middle">SHARE OF GUESSES KEPT (%)</text></svg><div class="sg-figure__legend"><span><i style="background:var(--sg-chart-1)"></i>12B</span><span><i style="background:var(--sg-chart-muted)"></i>26B and 31B</span></div></div><div class="sg-figure__pane sg-figure__pane--table"><table><thead><tr><th style="text-align:left">run</th><th style="text-align:right">words guessed</th><th style="text-align:right">kept</th></tr></thead><tbody><tr><td style="text-align:left">gemma-4-12B non-QAT</td><td style="text-align:right">1,510,235</td><td style="text-align:right">82.0%</td></tr><tr><td style="text-align:left">gemma-4-12B QAT</td><td style="text-align:right">1,414,986</td><td style="text-align:right">81.2%</td></tr><tr><td style="text-align:left">gemma-4-26B-A4B unsloth QAT</td><td style="text-align:right">1,360,556</td><td style="text-align:right">79.2%</td></tr><tr><td style="text-align:left">gemma-4-26B-A4B google q4_0</td><td style="text-align:right">1,367,766</td><td style="text-align:right">79.1%</td></tr><tr><td style="text-align:left">gemma-4-31B QAT</td><td style="text-align:right">539,715</td><td style="text-align:right">79.1%</td></tr><tr><td style="text-align:left">gemma-4-31B non-QAT</td><td style="text-align:right">620,046</td><td style="text-align:right">78.5%</td></tr></tbody></table></div></div><figcaption class="sg-figure__caption">How many guesses the big model kept. Drawn on a zoomed scale, and as dots rather than bars, because the whole range is six points wide and a bar chart starting anywhere but zero would overstate it.</figcaption></figure>

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

<figure class="sg-figure"><input class="sg-figure__radio sg-figure__radio--chart" type="radio" name="fig-2" id="fig-2-chart" checked><input class="sg-figure__radio sg-figure__radio--table" type="radio" name="fig-2" id="fig-2-table"><div class="sg-figure__tabs"><label class="sg-figure__tab sg-figure__tab--chart" for="fig-2-chart">Chart</label><label class="sg-figure__tab sg-figure__tab--table" for="fig-2-table">Numbers</label></div><div class="sg-figure__panes"><div class="sg-figure__pane sg-figure__pane--chart"><svg class="sg-chart" viewBox="0 0 760 128" preserveAspectRatio="xMidYMid meet" role="img" aria-label="Magnitude per run"><line class="sg-chart__grid" x1="336.6" x2="336.6" y1="16" y2="96"/><text class="sg-chart__value" x="336.6" y="112" text-anchor="middle" opacity=".7">30</text><line class="sg-chart__grid" x1="463.2" x2="463.2" y1="16" y2="96"/><text class="sg-chart__value" x="463.2" y="112" text-anchor="middle" opacity=".7">60</text><line class="sg-chart__grid" x1="589.8" x2="589.8" y1="16" y2="96"/><text class="sg-chart__value" x="589.8" y="112" text-anchor="middle" opacity=".7">90</text><text class="sg-chart__label" x="198" y="45.0" text-anchor="end">guessing off</text><rect class="sg-chart__mark sg-chart__mark--1" x="210.0" y="36.5" width="422.0" height="9" rx="4"/><text class="sg-chart__value" x="641.0" y="45.0">100/100</text><text class="sg-chart__label" x="198" y="79.0" text-anchor="end">guessing on</text><rect class="sg-chart__mark sg-chart__mark--2" x="210.0" y="70.5" width="312.3" height="9" rx="4"/><text class="sg-chart__value" x="531.3" y="79.0">**74/100**</text><text class="sg-chart__axis" x="421.0" y="122" text-anchor="middle">NOTES IDENTICAL TO THE ONE-AT-A-TIME RUN, OUT OF 100</text></svg></div><div class="sg-figure__pane sg-figure__pane--table"><table><thead><tr><th style="text-align:left"></th><th style="text-align:right">identical to the one-at-a-time run</th></tr></thead><tbody><tr><td style="text-align:left">guessing off</td><td style="text-align:right">100/100</td></tr><tr><td style="text-align:left">guessing on</td><td style="text-align:right"><strong>74/100</strong></td></tr></tbody></table></div></div><figcaption class="sg-figure__caption">Guessing is supposed to change nothing. It changes twenty-six notes in a hundred, because checking several guesses at once changes the order the arithmetic happens in.</figcaption></figure>

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
gemma-4-12B, guessing with 82% of its guesses kept on a comparable card, managed
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

<figure class="sg-figure"><input class="sg-figure__radio sg-figure__radio--chart" type="radio" name="fig-3" id="fig-3-chart" checked><input class="sg-figure__radio sg-figure__radio--table" type="radio" name="fig-3" id="fig-3-table"><div class="sg-figure__tabs"><label class="sg-figure__tab sg-figure__tab--chart" for="fig-3-chart">Chart</label><label class="sg-figure__tab sg-figure__tab--table" for="fig-3-table">Numbers</label></div><div class="sg-figure__panes"><div class="sg-figure__pane sg-figure__pane--chart"><svg class="sg-chart" viewBox="0 0 760 168" preserveAspectRatio="xMidYMid meet" role="img" aria-label="Two runs across several metrics, each on its own scale"><text class="sg-chart__axis" x="0" y="32">WORDS PER SECOND</text><text class="sg-chart__label" x="168" y="52.0" text-anchor="end" font-size="11">35B-A3B, mixture of experts</text><rect class="sg-chart__mark sg-chart__mark--1" x="178" y="43.5" width="404.4" height="9" rx="4"/><text class="sg-chart__value" x="591.4" y="52.0">234.0</text><text class="sg-chart__label" x="168" y="69.0" text-anchor="end" font-size="11">27B dense</text><rect class="sg-chart__mark sg-chart__mark--2" x="178" y="60.5" width="117.2" height="9" rx="4"/><text class="sg-chart__value" x="304.2" y="69.0">67.8</text><text class="sg-chart__axis" x="0" y="94">MEDIAN WORDS WRITTEN</text><text class="sg-chart__label" x="168" y="114.0" text-anchor="end" font-size="11">35B-A3B, mixture of experts</text><rect class="sg-chart__mark sg-chart__mark--1" x="178" y="105.5" width="339.4" height="9" rx="4"/><text class="sg-chart__value" x="526.4" y="114.0">1100</text><text class="sg-chart__label" x="168" y="131.0" text-anchor="end" font-size="11">27B dense</text><rect class="sg-chart__mark sg-chart__mark--2" x="178" y="122.5" width="387.6" height="9" rx="4"/><text class="sg-chart__value" x="574.6" y="131.0">1256</text></svg></div><div class="sg-figure__pane sg-figure__pane--table"><table><thead><tr><th style="text-align:left">Qwen3.6, same family, same quant, same card class</th><th style="text-align:right">words/s</th><th style="text-align:right">median completion</th></tr></thead><tbody><tr><td style="text-align:left">35B-A3B, mixture of experts</td><td style="text-align:right"><strong>234.0</strong></td><td style="text-align:right">1,100 words</td></tr><tr><td style="text-align:left">27B dense</td><td style="text-align:right">67.8</td><td style="text-align:right">1,256 words</td></tr></tbody></table></div></div><figcaption class="sg-figure__caption">The same family, same quant, same class of card, with no guessing on either side. Each measure on its own scale, because the units differ.</figcaption></figure>

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

<figure class="sg-figure"><input class="sg-figure__radio sg-figure__radio--chart" type="radio" name="fig-4" id="fig-4-chart" checked><input class="sg-figure__radio sg-figure__radio--table" type="radio" name="fig-4" id="fig-4-table"><div class="sg-figure__tabs"><label class="sg-figure__tab sg-figure__tab--chart" for="fig-4-chart">Chart</label><label class="sg-figure__tab sg-figure__tab--table" for="fig-4-table">Numbers</label></div><div class="sg-figure__panes"><div class="sg-figure__pane sg-figure__pane--chart"><svg class="sg-chart" viewBox="0 0 760 356" preserveAspectRatio="xMidYMid meet" role="img" aria-label="Series compared across categories"><text class="sg-chart__axis" x="0" y="36">1 PROCESS</text><text class="sg-chart__label" x="140" y="54.0" text-anchor="end" font-size="11">RTX 5080</text><rect class="sg-chart__mark sg-chart__mark--1" x="150" y="45.5" width="212.8" height="9" rx="4"/><text class="sg-chart__value" x="371.8" y="54.0">56 s</text><text class="sg-chart__label" x="140" y="80.0" text-anchor="end" font-size="11">RX 7900 XTX</text><rect class="sg-chart__mark sg-chart__mark--2" x="150" y="71.5" width="231.8" height="9" rx="4"/><text class="sg-chart__value" x="390.8" y="80.0">61 s</text><text class="sg-chart__axis" x="0" y="106">2 PROCESSES</text><text class="sg-chart__label" x="140" y="124.0" text-anchor="end" font-size="11">RTX 5080</text><rect class="sg-chart__mark sg-chart__mark--1" x="150" y="115.5" width="319.2" height="9" rx="4"/><text class="sg-chart__value" x="478.2" y="124.0">84 s</text><text class="sg-chart__label" x="140" y="150.0" text-anchor="end" font-size="11">RX 7900 XTX</text><rect class="sg-chart__mark sg-chart__mark--2" x="150" y="141.5" width="254.6" height="9" rx="4"/><text class="sg-chart__value" x="413.6" y="150.0">67 s</text><text class="sg-chart__axis" x="0" y="176">3 PROCESSES</text><text class="sg-chart__label" x="140" y="194.0" text-anchor="end" font-size="11">RTX 5080</text><rect class="sg-chart__mark sg-chart__mark--1" x="150" y="185.5" width="406.6" height="9" rx="4"/><text class="sg-chart__value" x="565.6" y="194.0">107 s</text><text class="sg-chart__label" x="140" y="220.0" text-anchor="end" font-size="11">RX 7900 XTX</text><rect class="sg-chart__mark sg-chart__mark--2" x="150" y="211.5" width="315.4" height="9" rx="4"/><text class="sg-chart__value" x="474.4" y="220.0">83 s</text><text class="sg-chart__axis" x="0" y="246">4 PROCESSES</text><text class="sg-chart__label" x="140" y="264.0" text-anchor="end" font-size="11">RTX 5080</text><rect class="sg-chart__mark sg-chart__mark--1" x="150" y="255.5" width="520.6" height="9" rx="4"/><text class="sg-chart__value" x="679.6" y="264.0">137 s</text><text class="sg-chart__label" x="140" y="290.0" text-anchor="end" font-size="11">RX 7900 XTX</text><rect class="sg-chart__mark sg-chart__mark--2" x="150" y="281.5" width="376.2" height="9" rx="4"/><text class="sg-chart__value" x="535.2" y="290.0">99 s</text><text class="sg-chart__axis" x="435.0" y="348" text-anchor="middle">SERVER STARTUP, SECONDS</text></svg></div><div class="sg-figure__pane sg-figure__pane--table"><table><thead><tr><th style="text-align:left">card</th><th style="text-align:right">1 process</th><th style="text-align:right">2 processes</th><th style="text-align:right">3 processes</th><th style="text-align:right">4 processes</th></tr></thead><tbody><tr><td style="text-align:left">RTX 5080</td><td style="text-align:right">56 s</td><td style="text-align:right">84 s</td><td style="text-align:right">107 s</td><td style="text-align:right">137 s</td></tr><tr><td style="text-align:left">RX 7900 XTX</td><td style="text-align:right">61 s</td><td style="text-align:right">67 s</td><td style="text-align:right">83 s</td><td style="text-align:right">99 s</td></tr></tbody></table></div></div><figcaption class="sg-figure__caption">Startup time grew with the variable under test, which is what made the 1.58x wrong: it was inside the throughput measurement.</figcaption></figure>

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

<figure class="sg-figure"><input class="sg-figure__radio sg-figure__radio--chart" type="radio" name="fig-5" id="fig-5-chart" checked><input class="sg-figure__radio sg-figure__radio--table" type="radio" name="fig-5" id="fig-5-table"><div class="sg-figure__tabs"><label class="sg-figure__tab sg-figure__tab--chart" for="fig-5-chart">Chart</label><label class="sg-figure__tab sg-figure__tab--table" for="fig-5-table">Numbers</label></div><div class="sg-figure__panes"><div class="sg-figure__pane sg-figure__pane--chart"><svg class="sg-chart" viewBox="0 0 760 168" preserveAspectRatio="xMidYMid meet" role="img" aria-label="Two runs across several metrics, each on its own scale"><text class="sg-chart__axis" x="0" y="32">ONE AT A TIME</text><text class="sg-chart__label" x="168" y="52.0" text-anchor="end" font-size="11">E4B UD-Q4_K_XL</text><rect class="sg-chart__mark sg-chart__mark--1" x="178" y="43.5" width="197.9" height="9" rx="4"/><text class="sg-chart__value" x="384.9" y="52.0">22.9</text><text class="sg-chart__label" x="168" y="69.0" text-anchor="end" font-size="11">E2B UD-Q4_K_XL</text><rect class="sg-chart__mark sg-chart__mark--2" x="178" y="60.5" width="233.3" height="9" rx="4"/><text class="sg-chart__value" x="420.3" y="69.0">27.0</text><text class="sg-chart__axis" x="0" y="94">WITH GUESSING</text><text class="sg-chart__label" x="168" y="114.0" text-anchor="end" font-size="11">E4B UD-Q4_K_XL</text><rect class="sg-chart__mark sg-chart__mark--1" x="178" y="105.5" width="362.0" height="9" rx="4"/><text class="sg-chart__value" x="549.0" y="114.0">41.9</text><text class="sg-chart__label" x="168" y="131.0" text-anchor="end" font-size="11">E2B UD-Q4_K_XL</text><rect class="sg-chart__mark sg-chart__mark--2" x="178" y="122.5" width="371.5" height="9" rx="4"/><text class="sg-chart__value" x="558.5" y="131.0">43.0</text></svg></div><div class="sg-figure__pane sg-figure__pane--table"><table><thead><tr><th style="text-align:left">model</th><th style="text-align:right">one at a time</th><th style="text-align:right">with guessing</th><th style="text-align:right">ratio</th></tr></thead><tbody><tr><td style="text-align:left">E4B UD-Q4_K_XL</td><td style="text-align:right">22.9 notes/min</td><td style="text-align:right">41.9</td><td style="text-align:right"><strong>1.83x</strong></td></tr><tr><td style="text-align:left">E2B UD-Q4_K_XL</td><td style="text-align:right">27.0 notes/min</td><td style="text-align:right">43.0</td><td style="text-align:right"><strong>1.59x</strong></td></tr></tbody></table></div></div><figcaption class="sg-figure__caption">The speedup belongs to the model, not to the feature. Notes per minute, each measure on its own scale.</figcaption></figure>

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
