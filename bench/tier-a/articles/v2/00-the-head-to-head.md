# Twenty-two arms, one corpus, and a fifteen-fold parameter increase that bought 0.047

DRAFT. Every arm below is 1,001 notes on corpus v5 with prompt v8, scored by the
unmodified scorer. The metric columns are recomputed from the prediction files in
one pass so the whole table is internally consistent. Nothing was re-run to write
this.

I built this benchmark to find out which local model to put in front of a fact
extractor. I started with models I could fit on a 16 GB card, got a ceiling of
0.6406, and assumed the ceiling was the corpus. Then I rented bigger GPUs and ran
the same corpus against models up to 35B.

The ceiling moved to 0.7257. But almost none of the movement came from size.

## The table

| model | quant | F1 | prec | rec | parse | abstain | spurious | reasons |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| Qwen3.6-35B-A3B | UD-Q4, MoE | **0.7257** | 0.6841 | 0.7727 | 1.00 | 0.699 | 99 | 1.00 |
| gemma-4-31B | QAT UD-Q4 | 0.6872 | 0.6022 | **0.8000** | 1.00 | 0.463 | 180 | 1.00 |
| gemma-4-12B | QAT UD-Q4 | 0.6854 | 0.6437 | 0.7330 | 0.92 | 0.702 | 97 | 1.00 |
| gemma-4-26B-A4B | QAT UD-Q4, unsloth | 0.6804 | 0.6501 | 0.7136 | 0.96 | 0.680 | 106 | 1.00 |
| gemma-4-31B | UD-Q4 | 0.6763 | 0.5882 | 0.7955 | 1.00 | 0.475 | 177 | 1.00 |
| gemma-4-12B | UD-Q4 | 0.6754 | 0.6271 | 0.7318 | 0.90 | 0.593 | 135 | 1.00 |
| gemma-4-26B-A4B | QAT q4_0, google | 0.6575 | 0.6398 | 0.6761 | 0.94 | 0.696 | 102 | 1.00 |
| gemma-4-E2B | QAT q4_0 | 0.6406 | 0.6294 | 0.6523 | 0.99 | 0.717 | 93 | 1.00 |
| gemma-4-E4B | QAT q4_0 | 0.6194 | 0.5878 | 0.6545 | 1.00 | 0.705 | 95 | 0.85 |
| gemma-4-E4B | UD-Q4 | 0.6166 | 0.5767 | 0.6625 | 1.00 | 0.568 | 143 | 1.00 |
| gemma-4-E2B | UD-Q4 | 0.6017 | 0.5840 | 0.6205 | 1.00 | 0.677 | 105 | 1.00 |
| LFM2.5-2.6B | Q4_K_M | 0.5854 | 0.5664 | 0.6057 | 1.00 | 0.630 | 124 | 1.00 |
| granite-4.1-3b | UD-Q4 | 0.5432 | 0.5501 | 0.5364 | 1.00 | **0.929** | **24** | 0.00 |
| gemma-3n-E4B | UD-Q4 | 0.5331 | 0.4918 | 0.5818 | 1.00 | 0.422 | 188 | 0.00 |
| LFM2.5-8B-A1B | Q4_K_M | 0.5198 | 0.5707 | 0.4773 | 0.98 | 0.907 | 31 | 1.00 |
| Qwen3-1.7B | UD-Q4 | 0.4618 | 0.4503 | 0.4739 | 0.99 | 0.652 | 113 | 1.00 |
| SmolLM3-3B | Q8_0 | 0.3933 | 0.3767 | 0.4114 | 0.99 | 0.354 | 214 | 0.00 |
| granite-4.0-1b | UD-Q4 | 0.3911 | 0.3836 | 0.3989 | 0.95 | 0.888 | 36 | 0.00 |
| LFM2.5-VL-1.6B | Q8_0 | 0.2725 | 0.2537 | 0.2943 | 1.00 | 0.323 | 223 | 0.00 |
| LFM2.5-1.2B | Q8_0 | 0.1671 | 0.2078 | 0.1398 | **0.73** | 0.382 | 202 | 0.00 |
| MiniCPM5-1B | Q8_0 | 0.1652 | 0.2630 | 0.1205 | **0.87** | 0.963 | 12 | 1.00 |
| LFM2.5-230M | Q8_0 | 0.1309 | 0.1289 | 0.1330 | 1.00 | 0.531 | 151 | 0.00 |

`abstain` is how often a model correctly says nothing on the 322 notes whose
correct answer is nothing. `spurious` counts the triples it invented on those same
notes. `reasons` is the fraction of rows carrying a reasoning pass.

## One model separates. The rest of the top is a plateau.

I ran a paired bootstrap on every adjacent pair, because an adjacent pair is
exactly the ordering claim a ranked table makes.

| step | delta | 95% CI | |
|---|---:|---|---|
| 35B-A3B → 31B QAT | −0.0386 | [−0.0577, −0.0194] | **separable** |
| 31B QAT → 12B QAT | −0.0017 | [−0.0202, +0.0162] | indistinguishable |
| 12B QAT → 26B unsloth | −0.0051 | [−0.0256, +0.0154] | indistinguishable |
| 26B unsloth → 31B non-QAT | −0.0041 | [−0.0258, +0.0176] | indistinguishable |
| 31B non-QAT → 12B non-QAT | −0.0009 | [−0.0197, +0.0180] | indistinguishable |
| 12B non-QAT → 26B google | −0.0179 | [−0.0434, +0.0071] | indistinguishable |
| 26B google → E2B QAT | −0.0168 | [−0.0406, +0.0070] | indistinguishable |

**Six consecutive steps, none of them separable, spanning 2B to 31B.** The models
in that block differ by a factor of fifteen in parameters and by three quant
schemes, and this corpus cannot order any neighbouring pair of them.

Only the 35B MoE breaks out, and it breaks out cleanly.

## The trap in that table, which I nearly fell into

A chain of indistinguishable steps is not a flat region. I wrote "2B through 31B
is one band" in my notes and then tested the ends against each other directly:

| span | delta | 95% CI | |
|---|---:|---|---|
| E2B QAT → 31B QAT | +0.0465 | [+0.0220, +0.0712] | **significant** |
| 26B google → 31B QAT | +0.0297 | [+0.0079, +0.0521] | **significant** |
| E2B QAT → 35B-A3B | +0.0851 | [+0.0609, +0.1095] | **significant** |

Every adjacent step is noise and the sum of them is not. That is not a paradox,
it is what happens when you test seven hypotheses on overlapping data: each step
carries an interval of roughly ±0.020, and six of those stacked end to end have
plenty of room to hide a real 0.047.

So the honest statement is the one with the span in it. **Going from 2B to 31B is
worth about +0.047 F1, and you cannot detect it one model at a time.** If you
benchmark by climbing a size ladder and comparing each rung to the last, you will
conclude that size does nothing, and you will be wrong.

## What size actually bought

Fifteen times the parameters bought **+0.0465**. Switching architecture at the top
bought **+0.0386** more, from a model with roughly 3B active parameters.

That second number is the one worth staring at. Qwen3.6-35B-A3B is a
mixture-of-experts: 35B of weights resident, about 3B of them doing work per
token. It beat a dense 31B by more than the dense 31B beat a 2B, while reading
roughly a tenth as much memory per token.

And the throughput matches the architecture rather than the parameter count:

| model | tok/s | GPU |
|---|---:|---|
| gemma-4-26B-A4B, QAT unsloth | **323.1** | RTX 5080 |
| Qwen3.6-35B-A3B | 234.0 | RTX 5090 |
| gemma-4-12B non-QAT | 195.8 | RTX 5080 |
| gemma-4-12B QAT | 142.4 | RTX 3090 |
| gemma-4-31B non-QAT | 80.5 | RTX 5090 |
| gemma-4-31B QAT | 67.3 | RX 7900 XTX |
| Qwen3.6-27B dense (partial) | 64.7 | RTX 5090 |

The 35B is **3.6 times faster than the 27B from the same family**, and the two
produce nearly the same amount of text per note: median 1,100 completion tokens
against 1,269. So the gap is per-token cost, not verbosity. At Q4 a dense 27B
reads about 16.4 GiB of weights per token against roughly 1.5 GiB for a 3B-active
MoE, and an eleven-fold traffic ratio turns into a 3.6-fold latency ratio once
compute overlaps the reads.

Two of those rows are also worth reading as a warning: the 35B ran with **no
speculative decoding at all** and still beat the dense 12B that ran with a working
draft head at 82% acceptance. I had these arms labelled "native MTP" in my own
notes for several hours. They are not. `/props` reports `speculative: null`, and
no row in either Qwen prediction file carries a `draft_n` counter. Qwen3.6
publishes no MTP draft in this repo; I assumed a fast model must be speculating
and the assumption survived longer than it should have.

## F1 and restraint are still independent axes, and now it is worse

The top of the table hides a split that F1 does not show.

| model | F1 | recall | abstain | spurious |
|---|---:|---:|---:|---:|
| gemma-4-31B QAT | 0.6872 | **0.8000** | **0.463** | **180** |
| gemma-4-12B QAT | 0.6854 | 0.7330 | 0.702 | 97 |

**These two are statistically indistinguishable on F1 and they are not the same
model.** The 31B finds 0.80 of the facts, the best recall in the entire field. It
also invents 180 triples on the 322 notes that assert nothing, nearly twice the
12B's 97, and it correctly stays silent less than half the time.

Both 31B arms behave this way, QAT and not, so it is a property of that model
rather than of a quant. Scaling to 31B bought recall and spent restraint, and the
aggregate hid the trade completely.

At the other end, **granite-4.1-3b abstains on 93% of factless notes and invents
24 triples.** It ranks thirteenth on F1 and first on discipline by a wide margin.
LFM2.5-8B-A1B is second at 0.907 and 31.

So the choice still depends on what your pipeline does with a wrong fact. If a bad
edge is caught by a write gate and costs a review, buy recall and take the 31B. If
it lands in a graph and nothing downstream will ever find it again, buy restraint,
and granite-4.1-3b beats seven models scoring above it.

## Read the parse column before you believe a score

Four rows in this table are floors rather than capabilities.

**Both gemma-4-12B arms parse at 0.90 and 0.92** with **zero** rows hitting the
context limit. That is malformed JSON, not truncation. The 12B QAT arm's 0.6854 is
computed with 83 unreadable rows counted as failures, so its real capability is
somewhere above the number I printed, and the 31B and Qwen rows at 1.00 are not
carrying the same handicap. The gap between the 12B and the 31B is therefore
overstated by an unknown amount.

**MiniCPM5-1B parses 0.87.** Its abstention of 0.963 and 12 spurious triples look
like world-class restraint until you notice it is barely emitting anything at all.

**LFM2.5-1.2B parses 0.73.** A quarter of its output is unreadable.

And the reverse trap: **LFM2.5-230M parses 1.00 and scores 0.1309.** Nothing is
wrong with its format. It is answering, fluently, incorrectly. A clean parse rate
is not evidence of a working model.

## Half the field never reasons

| reasons on ~0% of rows | reasons on ~100% |
|---|---|
| granite-4.1-3b, granite-4.0-1b, gemma-3n-E4B, SmolLM3-3B, LFM2.5-VL-1.6B, LFM2.5-1.2B, LFM2.5-230M | every gemma-4 dense model, Qwen3.6-35B-A3B, LFM2.5-2.6B, LFM2.5-8B-A1B, Qwen3-1.7B, MiniCPM5-1B |

Seven of the field emit no reasoning pass in this harness. On gemma-4-E4B I know
why: one sentence in my prompt, `No prose, no markdown.`, suppressed reasoning
entirely across 10,000 notes while every row still recorded `thinking: true`.
Removing that sentence restored it on 770 of 770 notes.

I have run that diagnostic on four models. The other eighteen are unchecked, and a
model that silently loses its reasoning pass scores as a worse model. Some fraction
of the bottom half of this table is a prompt problem wearing the costume of a
capability problem, and I still cannot tell you which rows.

One model reasons *partially*: gemma-4-E4B under QAT, on 85% of rows. I do not
know why.

## What I would run today

**If it fits: Qwen3.6-35B-A3B.** It is the only model in the field that is
separably best, it parses everything, its restraint is good at 0.699 abstention,
and at 234 tok/s it is faster than every dense model here except one. 16.4 GiB at
UD-Q4, so it needs a 24 GB card.

**On a 16 GB card: gemma-4-26B-A4B at QAT UD-Q4.** 0.6804, indistinguishable from
models three rows above it, 0.96 parse, 0.680 abstention, and **323 tok/s, the
fastest arm in this entire project.** It fits in 13.27 GiB because QAT shrinks it,
which is the single most useful thing QAT did in this whole benchmark. The
non-QAT build of the same model is 15.84 GiB and does not fit that card at all.

**If invented facts are expensive: granite-4.1-3b.** 0.18 F1 behind the leader and
it invents a quarter as many triples as the model directly above it.

**If you need recall: gemma-4-31B.** 0.8000, the best in the field, and you pay for
it in precision and in restraint.

**Do not deploy below about 1.2B on this prompt.** Everything under it either fails
to parse or answers confidently and wrongly.

## What this table is not

**It is not a level measurement.** It orders models on one corpus generated by one
pipeline with one generator model. A model tuned on data resembling my generator
has an advantage I cannot detect from inside. This is the limit I cannot close
without a second corpus from a different lineage.

**Not every arm ran on the same GPU.** The large models were run wherever they fit,
across a local RTX 5080, a local RX 7900 XTX and rented RTX 3090s and 5090s. I
calibrated that: a rented 3090 against the local 5080, identical configuration,
came back **+0.0057 F1, CI [−0.0136, +0.0251]**, byte identity 640/1001. So rented
arms match the local field to within about **±0.019 at n=1001**, which is wider
than several deltas in this article. Where a comparison crosses hardware I have
said so, and none of the adjacent-pair verdicts change if you widen them by that
amount, because they were already indistinguishable.

That bound was measured CUDA to CUDA. The XTX runs Vulkan on a different
llama.cpp build and I have never measured it against the 5080 at all. The 31B QAT
row is the one this touches, and it is why the 31B QAT-versus-non-QAT comparison
in the quant article is being re-run with both halves on one card.

**Three rows are not native runs.** gemma-3n-E4B, Qwen3-1.7B and granite-4.1-3b
were extracted from 10,000-note arms rather than run at this tier. The same notes
score differently depending on which corpus they ran inside, by −0.0079 with 47%
of output text differing. Inside the interval, but not the same measurement.

**One row is a different configuration.** LFM2.5-8B-A1B could not run at three
processes: Q4_K_M is 5.16 GB and three copies exceed a 16 GiB card. Process count
alone is worth about 0.0105 F1 in this harness, so its 0.5198 is not directly
comparable to the rows above it.

**One arm is still running.** Qwen3.6-27B dense is at roughly a fifth of the
corpus. Its throughput is settled across three readings at 64.5, 64.5 and 64.2
tok/s, so the speed claim above is safe, but it has no F1 in this table yet and I
have not written one in.

## What would change my recommendation

1. **The 12B parse failures diagnosed.** 83 to 98 unreadable rows on a model
   sitting near the top of the table is the largest single source of understatement
   here, and it is a format problem I have not opened.
2. **The prompt clause tested against the other eighteen models.** Seven never
   reason and I have checked four.
3. **The 3k pairs now running.** Both QAT pairs at n=3002 with each pair confined to
   one card, which removes the cross-hardware term and narrows the interval by
   about √3.
4. **A second corpus from a different generator.** Everything above inherits one
   lineage.
