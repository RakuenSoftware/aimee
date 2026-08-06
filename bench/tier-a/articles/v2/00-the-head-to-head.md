# Thirty-two arms, one corpus, and a fifteen-fold parameter increase worth 0.047

Every arm is 1,001 notes on corpus v5 with prompt v8, scored by the unmodified
scorer. Metric columns are recomputed from the prediction files in one pass so the
table is internally consistent.

I built this to pick a local model for a fact extractor. I started with what fits
on a 16 GB card, got a ceiling of 0.6406, and assumed the ceiling was the corpus.
Then I rented bigger GPUs.

The ceiling moved to 0.7257. Almost none of the movement came from size.

## One note in, zero or more triples out

The model reads a single remembered note and returns subject-relation-object
triples as JSON. The prompt is the one my production system already sends, read
out of `kb_memory_facts.c` rather than written for the benchmark, so a result here
is a statement about the system I run.

    Vera Duarte joined the retrieval team last quarter.
    {"subject": "Vera Duarte", "relation": "member_of", "object": "retrieval team"}

Notes are one or two sentences, median 53 characters. The relation comes from 24
canonical predicates, or the model coins a snake_case one when none fits. It has
to keep durable state and drop everything transient, so `Hoping to get Fairweather
Chemicals over the line this quarter` is worth no facts at all: 322 of the 1,001
notes are that kind, and getting them right means returning an empty list. A
retraction is not an absence either. `Kestrel Freight is no longer a customer`
wants the original fact back with `negated` set.

F1 is strict. Subject, relation and object all have to match a gold triple, and
there are 880 of those across the 1,001 notes, spread over ten note categories
including negation, multi-fact, implicit and deliberately ambiguous.

| model | quant | F1 | prec | rec | parse | abstain | spurious | reasons |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| Qwen3.6-35B-A3B | UD-Q4, MoE | **0.7257** | 0.6841 | 0.7727 | 1.00 | 0.699 | 99 | 1.00 |
| Qwen3.6-27B dense | UD-Q4 | 0.7152 | 0.6550 | 0.7875 | 1.00 | 0.547 | 148 | 1.00 |
| gemma-4-31B | QAT UD-Q4 | 0.6872 | 0.6022 | **0.8000** | 1.00 | 0.463 | 180 | 1.00 |
| gemma-4-12B | QAT UD-Q4 | 0.6854 | 0.6437 | 0.7330 | 0.92 | 0.702 | 97 | 1.00 |
| gemma-4-26B-A4B | QAT UD-Q4, unsloth | 0.6804 | 0.6501 | 0.7136 | 0.96 | 0.680 | 106 | 1.00 |
| gemma-4-31B | UD-Q4 | 0.6763 | 0.5882 | 0.7955 | 1.00 | 0.475 | 177 | 1.00 |
| gemma-4-12B | UD-Q4 | 0.6754 | 0.6271 | 0.7318 | 0.90 | 0.593 | 135 | 1.00 |
| gemma-4-26B-A4B | QAT q4_0, google | 0.6575 | 0.6398 | 0.6761 | 0.94 | 0.696 | 102 | 1.00 |
| gemma-4-E2B | QAT q4_0 | 0.6406 | 0.6294 | 0.6523 | 0.99 | 0.717 | 93 | 1.00 |
| gemma-4-E4B | UD-Q6 | 0.6339 | 0.5976 | 0.6750 | 1.00 | 0.578 | 139 | 0.85 |
| gemma-4-E2B | UD-Q8 | 0.6226 | 0.6094 | 0.6364 | 1.00 | 0.683 | 105 | 1.00 |
| gemma-4-E4B | QAT q4_0 | 0.6194 | 0.5878 | 0.6545 | 1.00 | 0.705 | 95 | 0.85 |
| gemma-4-E2B | UD-Q6 | 0.6179 | 0.6077 | 0.6284 | 1.00 | 0.661 | 112 | 1.00 |
| gemma-4-E4B | UD-Q4 | 0.6166 | 0.5767 | 0.6625 | 1.00 | 0.568 | 143 | 1.00 |
| gemma-4-E4B | UD-Q8 | 0.6094 | 0.5744 | 0.6489 | 1.00 | 0.578 | 139 | 1.00 |
| gemma-4-E2B | UD-Q4 | 0.6017 | 0.5840 | 0.6205 | 1.00 | 0.677 | 105 | 1.00 |
| LFM2.5-2.6B | Q4_K_M | 0.5854 | 0.5664 | 0.6057 | 1.00 | 0.630 | 124 | 1.00 |
| LFM2.5-2.6B | Q6_K | 0.5795 | 0.5526 | 0.6091 | 1.00 | 0.612 | 125 | 1.00 |
| LFM2.5-2.6B | Q8_0 | 0.5750 | 0.5454 | 0.6080 | 1.00 | 0.593 | 135 | 1.00 |
| granite-4.1-3b | UD-Q4 | 0.5432 | 0.5501 | 0.5364 | 1.00 | **0.929** | **24** | 0.00 |
| gemma-3n-E4B | UD-Q4 | 0.5331 | 0.4918 | 0.5818 | 1.00 | 0.422 | 188 | 0.00 |
| LFM2.5-8B-A1B | Q4_K_M | 0.5198 | 0.5707 | 0.4773 | 0.98 | 0.907 | 31 | 1.00 |
| Qwen3-1.7B | UD-Q4 | 0.4618 | 0.4503 | 0.4739 | 0.99 | 0.652 | 113 | 1.00 |
| SmolLM3-3B | Q8_0 | 0.3933 | 0.3767 | 0.4114 | 0.99 | 0.354 | 214 | 0.00 |
| granite-4.0-1b | UD-Q4 | 0.3911 | 0.3836 | 0.3989 | 0.95 | 0.888 | 36 | 0.00 |
| LFM2.5-VL-1.6B | Q6_K | 0.2744 | 0.2569 | 0.2943 | 1.00 | 0.183 | 275 | 0.00 |
| LFM2.5-VL-1.6B | Q8_0 | 0.2725 | 0.2537 | 0.2943 | 1.00 | 0.323 | 223 | 0.00 |
| LFM2.5-1.2B | Q6_K | 0.1771 | 0.2320 | 0.1432 | **0.59** | 0.510 | 87 | 0.00 |
| LFM2.5-1.2B | Q8_0 | 0.1671 | 0.2078 | 0.1398 | **0.73** | 0.382 | 202 | 0.00 |
| MiniCPM5-1B | Q8_0 | 0.1652 | 0.2630 | 0.1205 | **0.87** | 0.963 | 12 | 1.00 |
| LFM2.5-230M | Q6_K | 0.1363 | 0.1330 | 0.1398 | 1.00 | 0.158 | 271 | 0.00 |
| LFM2.5-230M | Q8_0 | 0.1309 | 0.1289 | 0.1330 | 1.00 | 0.531 | 151 | 0.00 |

`abstain` is how often a model correctly returns nothing on those 322 factless
notes. `spurious` counts the triples it invents on them instead. `reasons` is the
fraction of rows carrying a reasoning pass.

## The top is a tie, and six of the steps below it are not real

A paired bootstrap on every adjacent pair, because an adjacent pair is exactly the
ordering claim a ranked table makes.

| step | delta | 95% CI | |
|---|---:|---|---|
| 35B-A3B → 27B dense | −0.0106 | [−0.0294, +0.0088] | indistinguishable |
| 27B dense → 31B QAT | −0.0280 | [−0.0456, −0.0106] | **separable** |
| 31B QAT → 12B QAT | −0.0017 | [−0.0202, +0.0162] | indistinguishable |
| 12B QAT → 26B unsloth | −0.0051 | [−0.0256, +0.0154] | indistinguishable |
| 26B unsloth → 31B non-QAT | −0.0041 | [−0.0258, +0.0176] | indistinguishable |
| 31B non-QAT → 12B non-QAT | −0.0009 | [−0.0197, +0.0180] | indistinguishable |
| 12B non-QAT → 26B google | −0.0179 | [−0.0434, +0.0071] | indistinguishable |
| 26B google → E2B QAT | −0.0168 | [−0.0406, +0.0070] | indistinguishable |

Six consecutive steps, 2B to 31B, and this corpus cannot order any neighbouring
pair. The break is one rung higher than the ranking suggests: two models sit above
it, and they cannot be ordered against each other either.

That second fact cost me the claim I had written first. I had the 35B down as the
only separably best model in the field, on a table where the 27B had no F1 yet.
It has one now, and the two are a tie.

I nearly published the rest as "2B through 31B is one flat band". Then I tested
the ends against each other.

| span | delta | 95% CI | |
|---|---:|---|---|
| E2B QAT → 31B QAT | **+0.0465** | [+0.0220, +0.0712] | significant |
| E2B QAT → 35B-A3B | **+0.0851** | [+0.0609, +0.1095] | significant |

Every step is noise. The sum is not. Six intervals of about ±0.020 stacked end to
end leave plenty of room to hide a real 0.047.

<figure>
<svg class="vz" viewBox="0 0 760 384" width="100%" height="auto" preserveAspectRatio="xMidYMid meet" role="img" aria-label="Paired F1 differences between adjacent leaderboard rows, with 95% intervals"><style>.vz{--sig:#2a78d6;--mut:#b0aea7;--cnt:#eb6834}@media (prefers-color-scheme:dark){.vz{--sig:#3987e5;--mut:#6b6a64;--cnt:#d95926}}.vz .gd{stroke:currentColor;opacity:.16}.vz .zr{stroke:currentColor;opacity:.45}.vz .tx{fill:currentColor;font-family:ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;font-size:11.5px}.vz .tm{fill:currentColor;opacity:.62;font-family:ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;font-size:11px}.vz .ax{fill:currentColor;opacity:.62;font-family:ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;font-size:10px;letter-spacing:.08em}</style><line class="gd" x1="236.0" x2="236.0" y1="28" y2="344" stroke-width="1"/><text class="tm" x="236.0" y="362" text-anchor="middle">−0.04</text><line class="zr" x1="347.0" x2="347.0" y1="28" y2="344" stroke-width="1"/><text class="tm" x="347.0" y="362" text-anchor="middle">0</text><line class="gd" x1="458.0" x2="458.0" y1="28" y2="344" stroke-width="1"/><text class="tm" x="458.0" y="362" text-anchor="middle">+0.04</text><line class="gd" x1="569.0" x2="569.0" y1="28" y2="344" stroke-width="1"/><text class="tm" x="569.0" y="362" text-anchor="middle">+0.08</text><line class="gd" x1="680.0" x2="680.0" y1="28" y2="344" stroke-width="1"/><text class="tm" x="680.0" y="362" text-anchor="middle">+0.12</text><text class="ax" x="347.0" y="20" text-anchor="middle">NO DIFFERENCE</text><text class="tx" x="172" y="61.0" text-anchor="end">35B-A3B → 27B dense</text><line x1="265.4" x2="371.4" y1="57.0" y2="57.0" stroke="var(--mut)" stroke-width="2" stroke-linecap="round"/><line x1="265.4" x2="265.4" y1="52.5" y2="61.5" stroke="var(--mut)" stroke-width="2" stroke-linecap="round"/><line x1="371.4" x2="371.4" y1="52.5" y2="61.5" stroke="var(--mut)" stroke-width="2" stroke-linecap="round"/><circle cx="317.5" cy="57.0" r="4.5" fill="var(--mut)" stroke="Canvas" stroke-width="2"/><text class="tm" x="692" y="61.0">−0.0106</text><text class="tx" x="172" y="91.0" text-anchor="end">27B dense → 31B QAT</text><line x1="220.4" x2="317.5" y1="87.0" y2="87.0" stroke="var(--sig)" stroke-width="2" stroke-linecap="round"/><line x1="220.4" x2="220.4" y1="82.5" y2="91.5" stroke="var(--sig)" stroke-width="2" stroke-linecap="round"/><line x1="317.5" x2="317.5" y1="82.5" y2="91.5" stroke="var(--sig)" stroke-width="2" stroke-linecap="round"/><circle cx="269.3" cy="87.0" r="4.5" fill="var(--sig)" stroke="Canvas" stroke-width="2"/><text class="tx" x="692" y="91.0">−0.0280</text><text class="tx" x="172" y="121.0" text-anchor="end">31B QAT → 12B QAT</text><line x1="290.9" x2="391.9" y1="117.0" y2="117.0" stroke="var(--mut)" stroke-width="2" stroke-linecap="round"/><line x1="290.9" x2="290.9" y1="112.5" y2="121.5" stroke="var(--mut)" stroke-width="2" stroke-linecap="round"/><line x1="391.9" x2="391.9" y1="112.5" y2="121.5" stroke="var(--mut)" stroke-width="2" stroke-linecap="round"/><circle cx="342.2" cy="117.0" r="4.5" fill="var(--mut)" stroke="Canvas" stroke-width="2"/><text class="tm" x="692" y="121.0">−0.0017</text><text class="tx" x="172" y="151.0" text-anchor="end">12B QAT → 26B unsloth</text><line x1="275.9" x2="389.7" y1="147.0" y2="147.0" stroke="var(--mut)" stroke-width="2" stroke-linecap="round"/><line x1="275.9" x2="275.9" y1="142.5" y2="151.5" stroke="var(--mut)" stroke-width="2" stroke-linecap="round"/><line x1="389.7" x2="389.7" y1="142.5" y2="151.5" stroke="var(--mut)" stroke-width="2" stroke-linecap="round"/><circle cx="332.8" cy="147.0" r="4.5" fill="var(--mut)" stroke="Canvas" stroke-width="2"/><text class="tm" x="692" y="151.0">−0.0051</text><text class="tx" x="172" y="181.0" text-anchor="end">26B unsloth → 31B</text><line x1="275.4" x2="395.8" y1="177.0" y2="177.0" stroke="var(--mut)" stroke-width="2" stroke-linecap="round"/><line x1="275.4" x2="275.4" y1="172.5" y2="181.5" stroke="var(--mut)" stroke-width="2" stroke-linecap="round"/><line x1="395.8" x2="395.8" y1="172.5" y2="181.5" stroke="var(--mut)" stroke-width="2" stroke-linecap="round"/><circle cx="335.6" cy="177.0" r="4.5" fill="var(--mut)" stroke="Canvas" stroke-width="2"/><text class="tm" x="692" y="181.0">−0.0041</text><text class="tx" x="172" y="211.0" text-anchor="end">31B → 12B (non-QAT)</text><line x1="292.3" x2="396.9" y1="207.0" y2="207.0" stroke="var(--mut)" stroke-width="2" stroke-linecap="round"/><line x1="292.3" x2="292.3" y1="202.5" y2="211.5" stroke="var(--mut)" stroke-width="2" stroke-linecap="round"/><line x1="396.9" x2="396.9" y1="202.5" y2="211.5" stroke="var(--mut)" stroke-width="2" stroke-linecap="round"/><circle cx="344.5" cy="207.0" r="4.5" fill="var(--mut)" stroke="Canvas" stroke-width="2"/><text class="tm" x="692" y="211.0">−0.0009</text><text class="tx" x="172" y="241.0" text-anchor="end">12B non-QAT → 26B goog</text><line x1="226.5" x2="366.7" y1="237.0" y2="237.0" stroke="var(--mut)" stroke-width="2" stroke-linecap="round"/><line x1="226.5" x2="226.5" y1="232.5" y2="241.5" stroke="var(--mut)" stroke-width="2" stroke-linecap="round"/><line x1="366.7" x2="366.7" y1="232.5" y2="241.5" stroke="var(--mut)" stroke-width="2" stroke-linecap="round"/><circle cx="297.3" cy="237.0" r="4.5" fill="var(--mut)" stroke="Canvas" stroke-width="2"/><text class="tm" x="692" y="241.0">−0.0179</text><text class="tx" x="172" y="271.0" text-anchor="end">26B google → E2B QAT</text><line x1="234.3" x2="366.4" y1="267.0" y2="267.0" stroke="var(--mut)" stroke-width="2" stroke-linecap="round"/><line x1="234.3" x2="234.3" y1="262.5" y2="271.5" stroke="var(--mut)" stroke-width="2" stroke-linecap="round"/><line x1="366.4" x2="366.4" y1="262.5" y2="271.5" stroke="var(--mut)" stroke-width="2" stroke-linecap="round"/><circle cx="300.3" cy="267.0" r="4.5" fill="var(--mut)" stroke="Canvas" stroke-width="2"/><text class="tm" x="692" y="271.0">−0.0168</text><line class="gd" x1="10" x2="750" y1="279.0" y2="279.0" stroke-width="1"/><text class="tx" x="172" y="301.0" text-anchor="end">E2B QAT → 31B QAT</text><line x1="408.0" x2="544.6" y1="297.0" y2="297.0" stroke="var(--sig)" stroke-width="2" stroke-linecap="round"/><line x1="408.0" x2="408.0" y1="292.5" y2="301.5" stroke="var(--sig)" stroke-width="2" stroke-linecap="round"/><line x1="544.6" x2="544.6" y1="292.5" y2="301.5" stroke="var(--sig)" stroke-width="2" stroke-linecap="round"/><circle cx="476.0" cy="297.0" r="4.5" fill="var(--sig)" stroke="Canvas" stroke-width="2"/><text class="tx" x="692" y="301.0">+0.0465</text><text class="tx" x="172" y="331.0" text-anchor="end">E2B QAT → 35B-A3B</text><line x1="516.0" x2="650.9" y1="327.0" y2="327.0" stroke="var(--sig)" stroke-width="2" stroke-linecap="round"/><line x1="516.0" x2="516.0" y1="322.5" y2="331.5" stroke="var(--sig)" stroke-width="2" stroke-linecap="round"/><line x1="650.9" x2="650.9" y1="322.5" y2="331.5" stroke="var(--sig)" stroke-width="2" stroke-linecap="round"/><circle cx="583.1" cy="327.0" r="4.5" fill="var(--sig)" stroke="Canvas" stroke-width="2"/><text class="tx" x="692" y="331.0">+0.0851</text></svg>
<figcaption>Difference in strict F1 between each neighbouring pair, with a 95% interval. An interval crossing zero means the ranking could be either way round. The last two rows compare non-adjacent ends of the same stretch.</figcaption>
</figure>

**Compare a size ladder rung by rung and you will conclude size does nothing.**
That is the most expensive mistake available in this kind of benchmark, and it is
the default way people run them.

## What size actually bought

Fifteen times the parameters: **+0.0465**. Changing architecture at the top:
**+0.0280** more, from a model with roughly 3B active parameters.

Qwen3.6-35B-A3B is a mixture of experts. 35B resident, about 3B working per token.
It beat a dense 31B by more than the dense 31B beat a 2B, reading roughly a tenth
as much memory to do it.

The sharper version of that is inside one family. The 35B and the dense 27B are
the same lineage at the same quant on the same card class, and on accuracy they
are a tie: −0.0106, CI [−0.0294, +0.0088]. The MoE reaches that score at 3.5 times
the throughput. Sparsity did not buy points here. It bought the same points for a
tenth of the memory read per token, which is the better deal and the harder one to
see from a leaderboard.

Throughput follows the same line:

| model | tok/s | GPU |
|---|---:|---|
| gemma-4-26B-A4B QAT | **323.1** | RTX 5080 |
| Qwen3.6-35B-A3B | 234.0 | RTX 5090 |
| gemma-4-12B non-QAT | 195.8 | RTX 5080 |
| gemma-4-12B QAT | 142.4 | RTX 3090 |
| gemma-4-31B non-QAT | 80.5 | RTX 5090 |
| gemma-4-31B QAT | 67.3 | RX 7900 XTX |
| Qwen3.6-27B dense | 67.8 | RTX 5090 |

The 35B is **3.5 times faster than the 27B from its own family**, and the two write
almost the same amount: median 1,100 completion tokens against 1,256. So the gap is
per-token cost. A dense 27B at Q4 reads about 16.4 GiB of weights per token against
roughly 1.5 GiB for a 3B-active MoE.

The 35B also ran with **no speculative decoding at all** and still beat a dense 12B
running a draft head at 82% acceptance. I had both Qwen arms labelled "native MTP"
in my own notes for several hours. `/props` says `speculative: null` and no row in
either file carries a `draft_n` counter. I assumed a fast model must be
speculating, and the assumption outlived three status reports.

## F1 hides the thing you actually care about

| model | F1 | recall | abstain | spurious |
|---|---:|---:|---:|---:|
| gemma-4-31B QAT | 0.6872 | **0.8000** | **0.463** | **180** |
| gemma-4-12B QAT | 0.6854 | 0.7330 | 0.702 | 97 |

Indistinguishable on F1. Not the same model.

The 31B finds 0.80 of the facts, best in the field. It also invents 180 triples on
the 322 notes that assert nothing, nearly twice the 12B, and stays correctly silent
less than half the time. Both 31B arms do this, QAT and not, so it is the model and
not the quant. Scaling to 31B bought recall and spent restraint. The aggregate hid
the whole trade.

At the other end, **granite-4.1-3b abstains on 93% of factless notes and invents
24 triples.** Twentieth on F1, first on discipline, by a distance.

<figure>
<svg class="vz" viewBox="0 0 760 440" width="100%" height="auto" preserveAspectRatio="xMidYMid meet" role="img" aria-label="Strict F1 against invented triples for 32 model arms"><style>.vz{--sig:#2a78d6;--mut:#b0aea7;--cnt:#eb6834}@media (prefers-color-scheme:dark){.vz{--sig:#3987e5;--mut:#6b6a64;--cnt:#d95926}}.vz .gd{stroke:currentColor;opacity:.16}.vz .zr{stroke:currentColor;opacity:.45}.vz .tx{fill:currentColor;font-family:ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;font-size:11.5px}.vz .tm{fill:currentColor;opacity:.62;font-family:ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;font-size:11px}.vz .ax{fill:currentColor;opacity:.62;font-family:ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;font-size:10px;letter-spacing:.08em}</style><line class="gd" x1="58" x2="730" y1="388.0" y2="388.0" stroke-width="1"/><text class="tm" x="48" y="392.0" text-anchor="end">0</text><line class="gd" x1="58" x2="730" y1="329.0" y2="329.0" stroke-width="1"/><text class="tm" x="48" y="333.0" text-anchor="end">50</text><line class="gd" x1="58" x2="730" y1="270.0" y2="270.0" stroke-width="1"/><text class="tm" x="48" y="274.0" text-anchor="end">100</text><line class="gd" x1="58" x2="730" y1="211.0" y2="211.0" stroke-width="1"/><text class="tm" x="48" y="215.0" text-anchor="end">150</text><line class="gd" x1="58" x2="730" y1="152.0" y2="152.0" stroke-width="1"/><text class="tm" x="48" y="156.0" text-anchor="end">200</text><line class="gd" x1="58" x2="730" y1="93.0" y2="93.0" stroke-width="1"/><text class="tm" x="48" y="97.0" text-anchor="end">250</text><line class="gd" x1="58" x2="730" y1="34.0" y2="34.0" stroke-width="1"/><text class="tm" x="48" y="38.0" text-anchor="end">300</text><text class="tm" x="67.6" y="408" text-anchor="middle">0.1</text><text class="tm" x="163.6" y="408" text-anchor="middle">0.2</text><text class="tm" x="259.6" y="408" text-anchor="middle">0.3</text><text class="tm" x="355.6" y="408" text-anchor="middle">0.4</text><text class="tm" x="451.6" y="408" text-anchor="middle">0.5</text><text class="tm" x="547.6" y="408" text-anchor="middle">0.6</text><text class="tm" x="643.6" y="408" text-anchor="middle">0.7</text><text class="ax" x="394.0" y="428" text-anchor="middle">STRICT F1</text> <text class="ax" x="12" y="18">INVENTED TRIPLES</text><circle cx="668.3" cy="271.2" r="6" fill="var(--sig)" stroke="Canvas" stroke-width="2"><title>Qwen3.6-35B-A3B UD-Q4, MoE — F1 0.7257, invents 99, abstains 0.699</title></circle><circle cx="658.2" cy="213.4" r="6" fill="var(--sig)" stroke="Canvas" stroke-width="2"><title>Qwen3.6-27B dense UD-Q4 — F1 0.7152, invents 148, abstains 0.547</title></circle><circle cx="631.3" cy="175.6" r="6" fill="var(--sig)" stroke="Canvas" stroke-width="2"><title>gemma-4-31B QAT UD-Q4 — F1 0.6872, invents 180, abstains 0.463</title></circle><circle cx="629.6" cy="273.5" r="5" fill="var(--mut)" stroke="Canvas" stroke-width="2"><title>gemma-4-12B QAT UD-Q4 — F1 0.6854, invents 97, abstains 0.702</title></circle><circle cx="624.8" cy="262.9" r="5" fill="var(--mut)" stroke="Canvas" stroke-width="2"><title>gemma-4-26B-A4B QAT UD-Q4, unsloth — F1 0.6804, invents 106, abstains 0.680</title></circle><circle cx="620.8" cy="179.1" r="5" fill="var(--mut)" stroke="Canvas" stroke-width="2"><title>gemma-4-31B UD-Q4 — F1 0.6763, invents 177, abstains 0.475</title></circle><circle cx="620.0" cy="228.7" r="5" fill="var(--mut)" stroke="Canvas" stroke-width="2"><title>gemma-4-12B UD-Q4 — F1 0.6754, invents 135, abstains 0.593</title></circle><circle cx="602.8" cy="267.6" r="5" fill="var(--mut)" stroke="Canvas" stroke-width="2"><title>gemma-4-26B-A4B QAT q4_0, google — F1 0.6575, invents 102, abstains 0.696</title></circle><circle cx="586.6" cy="278.3" r="5" fill="var(--mut)" stroke="Canvas" stroke-width="2"><title>gemma-4-E2B QAT q4_0 — F1 0.6406, invents 93, abstains 0.717</title></circle><circle cx="580.1" cy="224.0" r="5" fill="var(--mut)" stroke="Canvas" stroke-width="2"><title>gemma-4-E4B UD-Q6 — F1 0.6339, invents 139, abstains 0.578</title></circle><circle cx="569.3" cy="264.1" r="5" fill="var(--mut)" stroke="Canvas" stroke-width="2"><title>gemma-4-E2B UD-Q8 — F1 0.6226, invents 105, abstains 0.683</title></circle><circle cx="566.2" cy="275.9" r="5" fill="var(--mut)" stroke="Canvas" stroke-width="2"><title>gemma-4-E4B QAT q4_0 — F1 0.6194, invents 95, abstains 0.705</title></circle><circle cx="564.8" cy="255.8" r="5" fill="var(--mut)" stroke="Canvas" stroke-width="2"><title>gemma-4-E2B UD-Q6 — F1 0.6179, invents 112, abstains 0.661</title></circle><circle cx="563.5" cy="219.3" r="5" fill="var(--mut)" stroke="Canvas" stroke-width="2"><title>gemma-4-E4B UD-Q4 — F1 0.6166, invents 143, abstains 0.568</title></circle><circle cx="556.6" cy="224.0" r="5" fill="var(--mut)" stroke="Canvas" stroke-width="2"><title>gemma-4-E4B UD-Q8 — F1 0.6094, invents 139, abstains 0.578</title></circle><circle cx="549.2" cy="264.1" r="5" fill="var(--mut)" stroke="Canvas" stroke-width="2"><title>gemma-4-E2B UD-Q4 — F1 0.6017, invents 105, abstains 0.677</title></circle><circle cx="533.6" cy="241.7" r="5" fill="var(--mut)" stroke="Canvas" stroke-width="2"><title>LFM2.5-2.6B Q4_K_M — F1 0.5854, invents 124, abstains 0.630</title></circle><circle cx="527.9" cy="240.5" r="5" fill="var(--mut)" stroke="Canvas" stroke-width="2"><title>LFM2.5-2.6B Q6_K — F1 0.5795, invents 125, abstains 0.612</title></circle><circle cx="523.6" cy="228.7" r="5" fill="var(--mut)" stroke="Canvas" stroke-width="2"><title>LFM2.5-2.6B Q8_0 — F1 0.5750, invents 135, abstains 0.593</title></circle><circle cx="493.1" cy="359.7" r="6" fill="var(--cnt)" stroke="Canvas" stroke-width="2"><title>granite-4.1-3b UD-Q4 — F1 0.5432, invents 24, abstains 0.929</title></circle><circle cx="483.4" cy="166.2" r="5" fill="var(--mut)" stroke="Canvas" stroke-width="2"><title>gemma-3n-E4B UD-Q4 — F1 0.5331, invents 188, abstains 0.422</title></circle><circle cx="470.6" cy="351.4" r="5" fill="var(--mut)" stroke="Canvas" stroke-width="2"><title>LFM2.5-8B-A1B Q4_K_M — F1 0.5198, invents 31, abstains 0.907</title></circle><circle cx="414.9" cy="254.7" r="5" fill="var(--mut)" stroke="Canvas" stroke-width="2"><title>Qwen3-1.7B UD-Q4 — F1 0.4618, invents 113, abstains 0.652</title></circle><circle cx="349.2" cy="135.5" r="5" fill="var(--mut)" stroke="Canvas" stroke-width="2"><title>SmolLM3-3B Q8_0 — F1 0.3933, invents 214, abstains 0.354</title></circle><circle cx="347.1" cy="345.5" r="5" fill="var(--mut)" stroke="Canvas" stroke-width="2"><title>granite-4.0-1b UD-Q4 — F1 0.3911, invents 36, abstains 0.888</title></circle><circle cx="235.0" cy="63.5" r="5" fill="var(--mut)" stroke="Canvas" stroke-width="2"><title>LFM2.5-VL-1.6B Q6_K — F1 0.2744, invents 275, abstains 0.183</title></circle><circle cx="233.2" cy="124.9" r="5" fill="var(--mut)" stroke="Canvas" stroke-width="2"><title>LFM2.5-VL-1.6B Q8_0 — F1 0.2725, invents 223, abstains 0.323</title></circle><circle cx="141.6" cy="285.3" r="5" fill="var(--mut)" stroke="Canvas" stroke-width="2"><title>LFM2.5-1.2B Q6_K — F1 0.1771, invents 87, abstains 0.510</title></circle><circle cx="132.0" cy="149.6" r="5" fill="var(--mut)" stroke="Canvas" stroke-width="2"><title>LFM2.5-1.2B Q8_0 — F1 0.1671, invents 202, abstains 0.382</title></circle><circle cx="130.2" cy="373.8" r="5" fill="var(--mut)" stroke="Canvas" stroke-width="2"><title>MiniCPM5-1B Q8_0 — F1 0.1652, invents 12, abstains 0.963</title></circle><circle cx="102.4" cy="68.2" r="5" fill="var(--mut)" stroke="Canvas" stroke-width="2"><title>LFM2.5-230M Q6_K — F1 0.1363, invents 271, abstains 0.158</title></circle><circle cx="97.3" cy="209.8" r="5" fill="var(--mut)" stroke="Canvas" stroke-width="2"><title>LFM2.5-230M Q8_0 — F1 0.1309, invents 151, abstains 0.531</title></circle><text class="tx" x="679.3" y="262.2" text-anchor="start">Qwen3.6-35B-A3B</text><text class="tx" x="669.2" y="218.4" text-anchor="start">Qwen3.6-27B dense</text><text class="tx" x="642.3" y="180.6" text-anchor="start">gemma-4-31B</text><text class="tx" x="482.1" y="363.7" text-anchor="end">granite-4.1-3b</text></svg>
<figcaption>Every arm by its F1 and by how many triples it invents on the 322 notes whose correct answer is an empty list. Two arms can score the same and write very different amounts of false data downstream.</figcaption>
</figure>

So the decision is about what your pipeline does with a wrong fact. Caught by a
write gate and costs a review: buy recall, take the 31B. Lands in a graph where
nothing will ever find it again: buy restraint, and granite-4.1-3b invents fewer
triples than all nineteen arms scoring above it.

## Read the parse column first

**Both gemma-4-12B arms parse at 0.90 and 0.92, with zero rows at the context
limit.** That is malformed JSON, not truncation. The 12B QAT's 0.6854 counts 83
unreadable rows as failures. Its real capability is above the number I printed, the
31B and Qwen rows at 1.00 carry no such handicap, and the gap between them is
overstated by an amount I cannot currently quantify.

**MiniCPM5-1B parses 0.87.** Its 0.963 abstention and 12 spurious triples look like
the best discipline in the table until you notice it is barely emitting anything.

**LFM2.5-1.2B parses 0.73.**

And the reverse trap. **LFM2.5-230M parses 1.00 and scores 0.1309.** Nothing is
wrong with its format. It is answering fluently and incorrectly. A clean parse rate
is not evidence of a working model.

## A third of the field never reasons

| reasons on ~0% of rows | reasons on ~100% |
|---|---|
| granite-4.1-3b, granite-4.0-1b, gemma-3n-E4B, SmolLM3-3B, LFM2.5-VL-1.6B, LFM2.5-1.2B, LFM2.5-230M | every gemma-4 arm except the two E4B partials below, both Qwen3.6 arms, LFM2.5-2.6B, LFM2.5-8B-A1B, Qwen3-1.7B, MiniCPM5-1B |

On gemma-4-E4B I know why. One sentence in my prompt, `No prose, no markdown.`,
suppressed reasoning across 10,000 notes while every row still recorded
`thinking: true`. Removing it restored reasoning on 770 of 770 notes.

I have run that diagnostic on four models. Twenty-eight arms are unchecked. A model that
silently loses its reasoning pass scores as a worse model, so some fraction of the
bottom half of this table is a prompt problem wearing the costume of a capability
problem, and I cannot tell you which rows.

Two arms reason partially, both gemma-4-E4B, both on 85% of rows: the QAT q4_0
build and UD-Q6. I had written that up as a QAT effect when QAT was the only arm
showing it. UD-Q6 is not a QAT build, so whatever this is, it is not that. The
same model at UD-Q4 and UD-Q8 reasons on every row, which rules out a plain
size-of-quant story too. I do not know what it is.

## What this is not

**Not a level measurement.** One corpus, one generation pipeline, one generator
model. A model trained on data resembling my generator has an advantage I cannot
detect from inside. That is the limit I cannot close without a second corpus from a
different lineage.

**Not one GPU.** Large models ran wherever they fit: local RTX 5080, local RX 7900
XTX, rented RTX 3090s and 5090s. I calibrated it. A rented 3090 against the local
5080, identical configuration, came back **+0.0057 F1, CI [−0.0136, +0.0251]**,
byte identity 640/1001. So rented arms match the local field to within about
**±0.019 at n=1001**, wider than several deltas here. None of the adjacent-pair
verdicts change if you widen them by that much, because they were already
indistinguishable.

That bound is CUDA to CUDA. The XTX runs Vulkan on a different llama.cpp build and
I have never measured it against the 5080. It touches the 31B QAT row, which is why
the 31B quant pair is being re-run with both halves on one card.

**Four rows are not native runs.** gemma-3n-E4B, Qwen3-1.7B, granite-4.1-3b and
granite-4.0-1b were extracted from 10,000-note arms. The same notes score −0.0079
differently depending on which corpus they ran inside, with 47% of output text
differing. I had this down as three rows until I traced every figure in this piece
to its artifact and found the fourth.

**One row is a different configuration.** LFM2.5-8B-A1B could not run at three
processes: Q4_K_M is 5.16 GB and three copies exceed a 16 GiB card. Process count
alone is worth about 0.0105 F1 here.

**One throughput figure was corrected.** The 27B dense arm was two thirds through
when this table was first written, and I read its speed as 64.7 tok/s off three
samples. The finished run says 67.8 across 1,001 notes, with every hundred-note
slice between 67.7 and 68.0. The earlier figure was 4.6% low and every ratio here
uses the new one.

## What I would run today

**If it fits: Qwen3.6-35B-A3B.** Tied at the top with the dense 27B and separable
from everything below it, parses everything, 0.699 abstention, 234 tok/s. 16.4 GiB
at UD-Q4, so a 24 GB card.

Take it over the 27B on the tie-break rather than the score. Same family, same
quant, statistically the same F1, and the MoE runs 3.5 times faster while
abstaining on 0.699 of the factless notes against the 27B's 0.547. The dense 27B
invents 148 triples where the 35B invents 99. There is no accuracy argument
between them and two practical ones, both pointing the same way.

**On a 16 GB card: gemma-4-26B-A4B at QAT UD-Q4.** 0.6804, indistinguishable from
models three rows above it, and **323 tok/s, the fastest arm in this project.** It
fits in 13.27 GiB because QAT shrinks it. The non-QAT build of the same model is
15.84 GiB and does not fit that card at all.

**If invented facts are expensive: granite-4.1-3b.** A quarter of the invention rate
of the model directly above it.

**If you need recall: gemma-4-31B.** 0.8000, and you pay in precision and restraint.

**Nothing below about 1.2B on this prompt.** It either fails to parse or answers
confidently and wrongly.

## Four things would move this table

1. **The 12B parse failures diagnosed.** 83 to 98 unreadable rows on a model near
   the top is the largest single understatement in this table, and I have not opened
   it.
2. **The prompt clause tested against the other eighteen models.**
3. **The 3k pairs now running**, both QAT pairs at n=3002 with each pair confined to
   one card, which removes the cross-hardware term and narrows the interval by about
   √3.
4. **A second corpus from a different generator.**
