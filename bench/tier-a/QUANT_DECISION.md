# Quantisation: Q6 for E4B (measured), Q4 for E2B (superseded call)

Decision of 2026-08-01. Records the evidence, the part of it that is a
measurement, and the part that is a judgement, so a later reader can tell them
apart.

> **SUPERSEDED IN PART, 2026-08-03.** The shipped images are E4B at UD-Q6_K_XL and
> **E2B at UD-Q4_K_XL** — see `scripts/synthesis-model-table.sh`, which is the source
> of truth. The E4B half of this document still stands and is the measured half. The
> E2B half recommended Q6 as an explicitly JUDGED call on a directional prior, and
> the operator has since taken Q4 for E2B instead: the gap is undecidable at n=69
> (+0.0012, CI [-0.063, +0.069]), and on the small box E2B exists to serve, Q4 costs
> 1.70 GB VRAM / 2.68 GB RSS against 2.30 / 3.86 for Q6.
>
> Nothing measured here changed. What changed is which way an undecidable tie was
> broken, and the reasoning below is left intact so the two calls can be compared
> rather than one quietly overwritten. "What would change this" still applies: the
> E2B half needs a larger gold set, not more runs.

## The measurement

Six arms, Unsloth Dynamic quants, thinking ON, `--no-mmproj`, `-c 8192`, all on
`.254`'s 7900 XTX under RADV Vulkan. One variable per comparison: the quant.
Strict F1 on the 69-note gold set. Paired 95% CI from `harness/bootstrap_ci.py`
(same notes resampled for both arms, 5000 replicates).

| model | quant | F1 | tp | fp | fn |
| --- | --- | ---: | ---: | ---: | ---: |
| E2B | UD-Q4_K_XL | 0.7206 | 49 | 20 | 18 |
| E2B | UD-Q6_K_XL | 0.7218 | 48 | 18 | 19 |
| E2B | UD-Q8_K_XL | 0.6763 | 47 | 25 | 20 |
| E4B | UD-Q4_K_XL | 0.7200 | 45 | 13 | 22 |
| E4B | UD-Q6_K_XL | 0.8062 | 52 | 10 | 15 |
| E4B | UD-Q8_K_XL | 0.8217 | 53 |  9 | 14 |

Q6 − Q4:

| comparison | delta | 95% CI | verdict |
| --- | ---: | --- | --- |
| E4B | +0.0862 | [+0.0101, +0.1812] | **significant** |
| E2B | +0.0012 | [-0.0633, +0.0690] | indistinguishable |
| pooled (138 notes) | +0.0431 | [-0.0080, +0.0984] | 94.6% in favour |

## Where the curve stops: Q8 buys nothing

| comparison | delta | 95% CI | verdict |
| --- | ---: | --- | --- |
| E4B Q8 - Q4 | +0.1017 | [+0.0263, +0.1917] | **significant** |
| E4B Q8 - Q6 | +0.0155 | [+0.0000, +0.0396] | indistinguishable |

The Q4->Q6 step is worth 0.086 F1 and the Q6->Q8 step is worth 0.016, with an
interval whose lower bound sits on zero. Read that interval carefully: it never
goes NEGATIVE, so Q8 is probably very slightly better than Q6 — just not by
enough to measure, and not by enough to pay 0.45 GB of host RSS for. A step that
is real but too small to price is the strongest available form of "stop here",
and it is why Q6 is a sweet spot rather than a compromise.

E2B's Q8 arm goes the OTHER way: 0.6763, below both its own Q4 and Q6, while
E4B's Q8 is the highest arm measured anywhere in the lane. Same quant family,
same host, same day, opposite sign. That is what noise looks like at a model
size whose comparisons all sit inside their own intervals, and it is a reason to
trust the E4B numbers rather than the E2B ones when the two disagree.

Together, the E4B Q6-Q4 and Q8-Q4 results are the only comparisons in this entire
benchmark effort that cleared the significance bar. Everything else measured —
runtime, thinking on a fixed prompt, quantisation at E2B — came back inside its
own interval.

## Memory, measured rather than inferred from file size

Peak VRAM from `/sys/class/drm/card0/device/mem_info_vram_used` (card0 is the
XTX; card1 is the Phoenix iGPU and reads near zero), peak host RSS from
`/proc/<pid>/status` `VmHWM`, both sampled every 10s alongside the run.

| model | quant | peak VRAM | peak host RSS | GGUF on disk |
| --- | --- | ---: | ---: | ---: |
| E2B | UD-Q4_K_XL | 1.70 GB | 2.68 GB | 2.97 GB |
| E2B | UD-Q6_K_XL | 2.30 GB | 3.86 GB | 4.39 GB |
| E2B | UD-Q8_K_XL | 2.83 GB | 4.98 GB | 4.92 GB |
| E4B | UD-Q4_K_XL | 3.36 GB | 4.16 GB | 4.77 GB |
| E4B | UD-Q6_K_XL | 4.54 GB | 6.97 GB | 6.95 GB |
| E4B | UD-Q8_K_XL | 5.71 GB | 7.42 GB | 8.11 GB |

At E4B, Q6 costs 2.81 GB of host RSS over Q4 and buys a measured 0.086 F1. That
is the one place in this benchmark where paying memory demonstrably buys quality.
The next 0.45 GB, to Q8, buys 0.0155 F1 that the dataset cannot resolve.

Resident is consistently BELOW the file size — mmap — so quoting a disk-size
delta as a RAM saving overstates it. The KV cache at a fixed `-c` is identical
across quants and does not shrink with the quant.

## What is measured and what is judged

MEASURED: Q6 beats Q4 at E4B by 0.086 F1, and that survives a paired bootstrap.
Q8 then adds only 0.0155 more, which does not. E4B is the default model, so
those two together settle the default from both sides: Q4 is measurably worse,
and Q8 is not measurably better.

JUDGED: Q6 for E2B. Its own comparison is +0.0012 — the smallest gap between any
two runs anywhere in this benchmark, and firmly undecidable at n=69. Three things
carry the call instead:

  1. Direction. Pooled over both models, P(Q6 > Q4) = 0.946.
  2. Bounded downside. The pooled interval's lower bound is -0.008: the worst
     case this data supports is Q6 being a tenth of a triple worse. The upside
     runs to +0.098. An asymmetric bet does not need p < 0.05 to be worth taking.
  3. The operator reports having seen the same Q4->Q6 direction on earlier passes
     through this matrix, in sessions not recorded in this repo.

That third point is normally the weakest kind of evidence and here it is not,
because THIS HARNESS IS DETERMINISTIC. Two independent llama.cpp runs of E2B at
identical settings produced byte-identical output on all 70 notes, and the paired
bootstrap of that pair returned [0.0000, 0.0000]. Greedy decoding, temperature 0.
So "run-to-run variation" was never an available explanation for a repeated
observation: every sighting is an independent sample of gold-set sampling error,
not of model noise. Repeated directional agreement across passes is therefore
stronger evidence than it would be under a stochastic decoder, not weaker.

## What would change this

The E2B half rests on a family prior, not on E2B's own numbers. Resolving it on
its own evidence needs a LARGER GOLD SET, not more runs — re-running a
deterministic harness reproduces the same 0.0012 delta indefinitely. CI width
falls with the square root of n: about 280 notes to resolve 0.05, about 1,100 to
resolve 0.025. The existing 70 also have a single annotator and no inter-rater
agreement figure, which more notes alone will not fix.

## Caveats that apply to every number here

- n = 69 scored notes (one of 70 is flagged `excluded` in the gold set).
- One annotator, no inter-rater agreement.
- The scorer's normalisation and alias rules were fitted against this same data
  and are worth 6-13% of F1 on their own.
- This lane is Vulkan/RADV on `.254`. Compare within it; the `.253` lanes are
  CUDA. Accuracy transfers (the same GGUF answers the same wherever it is
  served); speed does not.
- All six arms are complete and clean: 70/70 rows each, zero truncations, zero
  transport errors. The non-parsing rows (12 at E2B Q6, 6 at E2B Q8, 0 elsewhere)
  are ALL notes whose gold is empty, where the model wrote a bare `[]` instead of
  `{"facts":[]}`. Empty gold plus empty prediction contributes nothing to tp, fp
  or fn, so those cost no F1 and the parse-rate column is a formatting tic rather
  than a quality signal. It varies non-monotonically with the quant (0/12/6),
  which is itself a sign of that.
- Separately, and for the product rather than the benchmark: `mf_commit_facts`
  treats a bare `[]` as a parse failure, so in production a CORRECT abstention is
  indistinguishable in the logs from a malformed response. That is a diagnostics
  defect, not a scoring one.
