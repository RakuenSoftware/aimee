# Eighteen arms, one corpus, and the column that decides it is not F1

ROUGH DRAFT. This is the piece the project was built to produce. Every arm below
is 1,001 notes on corpus v5 with prompt v8, scored by the unmodified scorer.
Nothing here was re-run to write it.

Fourteen models. The best one scores 0.6406 and the worst usable one scores
0.1309. If you rank on that column alone you will pick the wrong model for at
least two common jobs.

## The table

| model | quant | F1 | prec | rec | parse | abstention | spurious | reasons |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| gemma-4-E2B | QAT q4_0 | **0.6406** | 0.6294 | 0.6523 | 0.99 | 0.675 | 105 | 1.00 |
| gemma-4-E4B | QAT q4_0 | 0.6194 | 0.5878 | 0.6545 | 1.00 | 0.599 | 131 | 0.85 |
| gemma-4-E4B | UD-Q4 | 0.6166 | 0.5767 | 0.6625 | 1.00 | 0.584 | 139 | 1.00 |
| gemma-4-E2B | UD-Q4 | 0.6017 | 0.5840 | 0.6205 | 1.00 | 0.661 | 110 | 1.00 |
| LFM2.5-2.6B | Q4_K_M | 0.5854 | 0.5664 | 0.6057 | 1.00 | 0.668 | 110 | 1.00 |
| LFM2.5-2.6B | Q8_0 | 0.5750 | 0.5454 | 0.6080 | 1.00 | 0.593 | 135 | 1.00 |
| granite-4.1-3b | UD-Q4 | 0.5474 | 0.5541 | 0.5409 | 1.00 | **0.786** | **71** | 0.00 |
| gemma-3n-E4B | UD-Q4 | 0.5331 | 0.4918 | 0.5818 | 1.00 | 0.321 | 220 | 0.00 |
| LFM2.5-8B-A1B | Q4_K_M | 0.5198 | 0.5707 | 0.4773 | 0.98 | 0.732 | 88 | 1.00 |
| Qwen3-1.7B | UD-Q4 | 0.4618 | 0.4503 | 0.4739 | 0.99 | 0.515 | 157 | 1.00 |
| SmolLM3-3B | Q8_0 | 0.3933 | 0.3767 | 0.4114 | 0.99 | 0.297 | 225 | 0.00 |
| granite-4.0-1b | UD-Q4 | 0.3911 | 0.3836 | 0.3989 | 0.95 | 0.289 | 230 | 0.00 |
| SmolLM3-3B | Q4_K_M | 0.3581 | 0.3363 | 0.3830 | 0.99 | 0.211 | 248 | 0.00 |
| LFM2.5-VL-1.6B | Q8_0 | 0.2725 | 0.2537 | 0.2943 | 1.00 | **0.171** | **279** | 0.00 |
| MiniCPM5-1B | Q8_0 | 0.1652 | 0.2630 | 0.1205 | **0.87** | 0.763 | 82 | 1.00 |
| LFM2.5-1.2B | Q8_0 | 0.1671 | 0.2078 | 0.1398 | **0.73** | 0.635 | 91 | 0.00 |
| MiniCPM5-1B | Q4_K_M | 0.1258 | 0.2041 | 0.0909 | **0.66** | 0.748 | 77 | 1.00 |
| LFM2.5-230M | Q8_0 | 0.1309 | 0.1289 | 0.1330 | 1.00 | 0.180 | 264 | 0.00 |

`abstention` is how often a model correctly says nothing on the 322 notes whose
correct answer is nothing. `spurious` is how many triples it invented on those same
notes. `reasons` is the fraction of rows carrying a reasoning pass.

**Read the interval before the order.** At n=1,001 a paired bootstrap gives roughly
±0.024, so the top four are one group. Any two rows within 0.024 are not ordered by
this table.

## F1 and restraint are independent axes

**granite-4.1-3b ranks seventh on F1 and first on discipline.** It abstains on 79%
of factless notes and invents 71 triples, half what the models above it invent. It
finds fewer facts and it makes up far less.

**LFM2.5-VL-1.6B is the inverse.** 0.2725 F1, abstains on 17%, invents 279
triples. It answers almost everything, including the notes that assert nothing.

Those two are 0.27 apart on F1 and four times apart on invention rate, in opposite
directions.

So the choice depends on what your pipeline does with a wrong fact. If a bad edge
is caught by a write gate and costs a review, buy recall. If it lands in a graph and
nothing downstream will ever find it again, buy restraint, and granite-4.1-3b beats
three models scoring above it.

## Three models whose score is a floor, not a capability

**MiniCPM5-1B parses 87% at Q8 and 66% at Q4.** A third of its output is not
readable at Q4. Its F1 of 0.1258 is a lower bound on a model that abstains
correctly 75% of the time when you can read it at all. That is a format problem
wearing the costume of a capability problem.

**LFM2.5-1.2B parses 73%.** Same shape.

Before you conclude a model cannot do the task, check the parse rate. Two of my
fourteen score in the bottom third for reasons that are not capability, and the
fix is a prompt matched to the model rather than a bigger model.

**And the reverse trap.** LFM2.5-230M parses 100% and scores 0.1309 at an abstention
rate of 0.180. Nothing is wrong with its format. It is answering, at length,
incorrectly. A clean parse rate is not evidence of a working model.

## Half the field never reasons

| reasons on ~0% of rows | reasons on ~100% |
|---|---|
| granite-4.1-3b, granite-4.0-1b, gemma-3n-E4B, SmolLM3-3B, LFM2.5-VL-1.6B, LFM2.5-1.2B, LFM2.5-230M | gemma-4 (both, both quants), LFM2.5-2.6B, LFM2.5-8B-A1B, Qwen3-1.7B, MiniCPM5-1B |

Seven of fourteen emit no reasoning pass in this harness. That is a property of the
run rather than of the model: on gemma-4 E4B, one sentence in my prompt
(`No prose, no markdown.`) suppressed reasoning entirely across 10,000 notes while
every row recorded `thinking: true`, and restoring it was worth +0.116
relation-agnostic recall on that model.

I know E4B suppresses and E2B does not. **The other twelve are unchecked**, and a
model that silently loses its reasoning pass scores as a worse model. Some fraction
of the bottom half of that table may be a prompt problem rather than a model
problem, and I cannot currently tell you which.

One model reasons *partially*: gemma-4 E4B under QAT, on 85% of rows. The missing
15% abstain at twice the rate of the rest. I do not know why.

## The quant is worth more than the model, twice

| pair | delta |
|---|---:|
| SmolLM3-3B, Q8 − Q4 | **+0.0352** |
| LFM2.5-2.6B, Q4 − Q8 | **+0.0104** |
| gemma-4 E2B, QAT − UD | **+0.0389** |

SmolLM3 gets better with more bits. LFM2.5-2.6B gets worse. gemma-4 E2B gains more
from a differently *trained* four-bit quant than from any bit-width step in this
project.

That last one is the largest single-model gain in the table and it moves E2B above
its own larger sibling. If you take one action from this piece, run your candidate
at more than one quant before you rank it against anything.

## What I would run today

**Default: gemma-4-E2B at QAT q4_0.** Top of the table, second-best restraint among
usable models, 100% reasoning, and it is the smaller of the two gemma-4 models so
it costs less to serve.

**If invented facts are expensive: granite-4.1-3b.** 0.093 F1 behind, and it invents
a third fewer triples than anything near it.

**If you need recall: gemma-4-E4B.** Highest recall in the field at 0.6625, and you
pay for it in precision and in restraint.

**Do not deploy below about 1.2B on this prompt.** Everything under it either fails
to parse or answers confidently and wrongly.

## What this table is not

**It is not a level measurement.** It orders models on one corpus generated by one
pipeline with one generator model. A model tuned on data resembling my generator
has an advantage I cannot detect from inside.

**Two rows are not native runs.** gemma-3n-E4B and Qwen3-1.7B were extracted from
10,000-note arms rather than run at this tier. The same notes score −0.0079
differently depending on which corpus they ran inside, with 47% of output text
differing. That is inside the interval, so nothing here moves, but it is not the
same measurement.

**One row is a different configuration.** LFM2.5-8B-A1B could not run at three
processes: Q4_K_M is 5.16 GB and three copies exceed a 16 GiB card. Process count
alone is worth about 0.0105 F1 in this harness, so its 0.5198 is not comparable to
the rows above it.

**Every gap under 0.024 needs its own bootstrap.** I have not run one per pair. I
spent most of this project using 0.0105 as a significance threshold, which is not
an interval at all: it is the measured effect of process count from an unrelated
experiment. Every ordering claim in this article is stated at ±0.024 and the
per-pair intervals are outstanding.

## What would change my recommendation

1. **Per-pair bootstraps across the whole table.** Analysis, not GPU time.
2. **The prompt clause tested against all fourteen models.** Seven never reason and
   I have checked two of them.
3. **MiniCPM5-1B and LFM2.5-1.2B re-run with a matched prompt.** Both are floors.
4. **A second corpus from a different generator.** The whole table inherits one
   lineage, and that is the limit I cannot close from inside.
