# Aimee Cost-Savings Benchmark — Results

**Date:** 2026-07-18 · **Primary agent:** `codex` (model `gpt-5.6-sol`) · **Mode:** single-shot
**Pricing:** frontier-equivalent **$1.25 / 1M input**, **$10.00 / 1M output** tokens
**Fleet:** aimee-server on 192.168.1.254 · tokens attributed exactly from the live `token_audit` ledger

## Headline

Across **60 real coding tasks** (10 reddit + 50 SWE-bench-lite), routing work through aimee's
delegate pool — cheap worker models generate candidates, the frontier primary only supervises/selects —
cut **primary-model token spend by 61.6%** and **cost by 48.7%**, while also reducing wall-clock time.

| Combined (60 instances) | Tokens (in / out) | Cost |
|---|---:|---:|
| Default (no aimee) | 569,506 (520,168 / 49,338) | $1.1435 |
| Aimee economized (no delegates) | 569,506 | $1.1435 |
| **Aimee + delegates** | **218,597** (182,794 / 35,803) | **$0.5861** |
| **Delegate saving** | **350,909 (61.6%)** | **$0.5574 (48.7%)** |

Combined wall-clock (sum of the two runs): default **381.5s** → with delegates **301.0s**.

## Methodology

Three measurements, two live arms on the real fleet:

1. **Default (no aimee)** and 2. **Aimee economized (no delegates)** — **Arm P**: the primary (`codex`)
   solves each task single-shot. The economizer's realized/avoided split makes measurements 1 and 2
   identical here (see caveats), so the economizer row is 0% by construction.
3. **Aimee + delegates** — **Arm S**: supervised best-of-N. A pool of cheap worker models generates
   `n` candidate solutions; the primary model only supervises/selects. We count **only primary-model
   (`gpt-5.6-sol`) tokens** — the frontier spend — in both arms; the cheap workers are what shift the load.

**Token attribution.** The two arms run **sequentially**; we snapshot `MAX(id)` of the fleet
`token_audit` table immediately before and after each arm and sum the rows in that id window filtered to
the primary model. This is exact because we are the sole tenant during the run. A per-run cache-busting
nonce is prepended to every problem so the server-side `draft` result cache never returns a cached diff
(which would write no ledger row and undercount real spend).

**Per-instance attribution.** Within each arm the primary emits exactly one row per instance; sorting
those rows by the submission timestamp embedded in their `delegation_id` recovers the submission order,
which equals the instances-list order — letting each row be labelled with its real instance name and
each instance's default cost be paired with its delegate cost. (Verified: both arms produced exactly
one primary row per instance for both runs.)

## Per-run summaries

### reddit10 (10 instances)

| Measurement | Tokens | Cost |
|---|---:|---:|
| Default (no aimee) | 105,254 | $0.1542 |
| Aimee economized (no delegates) | 105,254 | $0.1542 |
| **Aimee + delegates** | **26,191** | **$0.0498** |

| Saving | Tokens | Cost | Reduction |
|---|---:|---:|---:|
| Economizer (vs default) | 0 | $0.0000 | 0.0% |
| **Delegates (vs default)** | **79,063** | **$0.1044** | **75.1% tok / 67.7% cost** |

| Time | Seconds |
|---|---:|
| Default | 36.5 |
| Aimee + delegates | 36.7 |

### SWE-bench-lite (lite:50) (50 instances)

| Measurement | Tokens | Cost |
|---|---:|---:|
| Default (no aimee) | 464,252 | $0.9893 |
| Aimee economized (no delegates) | 464,252 | $0.9893 |
| **Aimee + delegates** | **192,406** | **$0.5363** |

| Saving | Tokens | Cost | Reduction |
|---|---:|---:|---:|
| Economizer (vs default) | 0 | $0.0000 | 0.0% |
| **Delegates (vs default)** | **271,846** | **$0.4530** | **58.6% tok / 45.8% cost** |

| Time | Seconds |
|---|---:|
| Default | 345.0 |
| Aimee + delegates | 264.3 |


## Per-instance detail — reddit10

| Instance | Default tok | Default $ | Default s | +Delegates tok | +Delegates $ | +Delegates s | Δ tokens | Δ % |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `sympy__sympy-20212` | 12,324 | $0.0189 | 9.7 | 2,068 | $0.0039 | 4.5 | 10,256 | +83% |
| `scikit-learn__scikit-learn-13439` | 11,975 | $0.0160 | 3.4 | 2,349 | $0.0040 | 3.3 | 9,626 | +80% |
| `sympy__sympy-13480` | 10,790 | $0.0148 | 4.9 | 2,184 | $0.0060 | 11.6 | 8,606 | +80% |
| `pytest-dev__pytest-11143` | 15,306 | $0.0208 | 5.7 | 3,274 | $0.0054 | 4.3 | 12,032 | +79% |
| `scikit-learn__scikit-learn-13584` | 9,478 | $0.0186 | 18.2 | 2,105 | $0.0051 | 9.0 | 7,373 | +78% |
| `django__django-12908` | 9,809 | $0.0133 | 3.2 | 2,183 | $0.0038 | 3.3 | 7,626 | +78% |
| `django__django-15814` | 13,800 | $0.0186 | 3.8 | 3,118 | $0.0073 | 8.1 | 10,682 | +77% |
| `django__django-11179` | 6,766 | $0.0097 | 6.4 | 1,804 | $0.0035 | 4.0 | 4,962 | +73% |
| `pytest-dev__pytest-6116` | 7,094 | $0.0097 | 3.4 | 1,899 | $0.0032 | 3.4 | 5,195 | +73% |
| `django__django-13447` | 7,912 | $0.0138 | 9.4 | 5,207 | $0.0076 | 3.9 | 2,705 | +34% |

## Per-instance detail — SWE-bench-lite (50)

| Instance | Default tok | Default $ | Default s | +Delegates tok | +Delegates $ | +Delegates s | Δ tokens | Δ % |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `psf__requests-2317` | 11,199 | $0.0187 | 12.8 | 1,466 | $0.0029 | 3.8 | 9,733 | +87% |
| `django__django-11001` | 12,316 | $0.0167 | 8.1 | 1,887 | $0.0036 | 3.7 | 10,429 | +85% |
| `pylint-dev__pylint-7114` | 18,240 | $0.0306 | 20.6 | 3,170 | $0.0061 | 6.8 | 15,070 | +83% |
| `mwaskom__seaborn-2848` | 12,722 | $0.0180 | 6.2 | 2,262 | $0.0068 | 10.9 | 10,460 | +82% |
| `scikit-learn__scikit-learn-10949` | 15,422 | $0.0220 | 7.7 | 3,011 | $0.0061 | 6.7 | 12,411 | +80% |
| `pallets__flask-4045` | 7,498 | $0.0231 | 33.5 | 1,688 | $0.0037 | 5.1 | 5,810 | +78% |
| `matplotlib__matplotlib-18869` | 11,223 | $0.0155 | 5.1 | 2,535 | $0.0053 | 6.6 | 8,688 | +77% |
| `sympy__sympy-12171` | 12,495 | $0.0303 | 37.7 | 2,861 | $0.0087 | 15.9 | 9,634 | +77% |
| `matplotlib__matplotlib-22711` | 8,195 | $0.0115 | 3.8 | 1,937 | $0.0052 | 7.0 | 6,258 | +76% |
| `astropy__astropy-14365` | 7,810 | $0.0131 | 8.0 | 1,866 | $0.0037 | 4.3 | 5,944 | +76% |
| `sphinx-doc__sphinx-10451` | 13,870 | $0.0432 | 71.9 | 3,319 | $0.0086 | 11.5 | 10,551 | +76% |
| `sphinx-doc__sphinx-11445` | 11,980 | $0.0191 | 12.0 | 2,877 | $0.0066 | 11.0 | 9,103 | +76% |
| `sympy__sympy-11870` | 13,241 | $0.0237 | 19.3 | 3,200 | $0.0063 | 6.0 | 10,041 | +76% |
| `psf__requests-1963` | 12,950 | $0.0189 | 8.8 | 3,297 | $0.0069 | 7.6 | 9,653 | +74% |
| `sphinx-doc__sphinx-10325` | 13,232 | $0.0473 | 102.1 | 3,613 | $0.0067 | 5.4 | 9,619 | +73% |
| `sympy__sympy-11897` | 9,403 | $0.0220 | 26.3 | 2,658 | $0.0056 | 6.3 | 6,745 | +72% |
| `astropy__astropy-14995` | 8,465 | $0.0147 | 9.7 | 2,436 | $0.0055 | 8.4 | 6,029 | +71% |
| `django__django-11019` | 9,391 | $0.0130 | 3.7 | 2,786 | $0.0086 | 18.7 | 6,605 | +70% |
| `pydata__xarray-3364` | 9,323 | $0.0149 | 12.7 | 2,893 | $0.0076 | 11.1 | 6,430 | +69% |
| `pydata__xarray-4094` | 13,114 | $0.0277 | 45.7 | 4,443 | $0.0088 | 8.1 | 8,671 | +66% |
| `matplotlib__matplotlib-23299` | 8,160 | $0.0138 | 10.2 | 3,030 | $0.0107 | 16.1 | 5,130 | +63% |
| `psf__requests-2674` | 10,637 | $0.0369 | 66.7 | 3,963 | $0.0150 | 30.4 | 6,674 | +63% |
| `pylint-dev__pylint-5859` | 7,983 | $0.0204 | 22.7 | 3,023 | $0.0059 | 6.5 | 4,960 | +62% |
| `pallets__flask-4992` | 6,371 | $0.0128 | 11.1 | 2,480 | $0.0051 | 8.1 | 3,891 | +61% |
| `django__django-10914` | 10,135 | $0.0287 | 45.8 | 3,948 | $0.0090 | 10.8 | 6,187 | +61% |
| `pydata__xarray-4493` | 11,456 | $0.0231 | 19.3 | 4,511 | $0.0131 | 16.9 | 6,945 | +61% |
| `django__django-11039` | 12,525 | $0.0227 | 26.7 | 5,004 | $0.0269 | 56.5 | 7,521 | +60% |
| `mwaskom__seaborn-3010` | 8,120 | $0.0214 | 27.2 | 3,299 | $0.0096 | 12.7 | 4,821 | +59% |
| `astropy__astropy-14182` | 6,618 | $0.0097 | 4.0 | 2,784 | $0.0049 | 4.8 | 3,834 | +58% |
| `pydata__xarray-4248` | 8,622 | $0.0131 | 6.6 | 3,934 | $0.0170 | 28.9 | 4,688 | +54% |
| `astropy__astropy-6938` | 8,910 | $0.0292 | 39.3 | 4,193 | $0.0112 | 20.4 | 4,717 | +53% |
| `psf__requests-2148` | 8,416 | $0.0296 | 46.0 | 4,018 | $0.0097 | 13.4 | 4,398 | +52% |
| `mwaskom__seaborn-3407` | 9,182 | $0.0126 | 4.2 | 4,494 | $0.0073 | 4.8 | 4,688 | +51% |
| `pytest-dev__pytest-5103` | 9,364 | $0.0226 | 29.7 | 4,887 | $0.0103 | 10.2 | 4,477 | +48% |
| `mwaskom__seaborn-3190` | 12,173 | $0.0209 | 13.5 | 6,495 | $0.0218 | 37.1 | 5,678 | +47% |
| `scikit-learn__scikit-learn-10508` | 5,678 | $0.0128 | 13.7 | 3,042 | $0.0084 | 12.5 | 2,636 | +46% |
| `sympy__sympy-11400` | 8,176 | $0.0272 | 49.5 | 4,423 | $0.0229 | 47.3 | 3,753 | +46% |
| `scikit-learn__scikit-learn-10297` | 10,708 | $0.0167 | 8.6 | 5,944 | $0.0339 | 99.1 | 4,764 | +44% |
| `pylint-dev__pylint-6506` | 5,699 | $0.0169 | 29.9 | 3,172 | $0.0069 | 8.0 | 2,527 | +44% |
| `pytest-dev__pytest-11143` | 8,774 | $0.0189 | 19.4 | 4,955 | $0.0072 | 3.1 | 3,819 | +44% |
| `pylint-dev__pylint-7080` | 3,911 | $0.0062 | 4.0 | 2,309 | $0.0053 | 13.2 | 1,602 | +41% |
| `sphinx-doc__sphinx-7686` | 9,190 | $0.0321 | 59.0 | 6,206 | $0.0242 | 45.7 | 2,984 | +32% |
| `scikit-learn__scikit-learn-11040` | 3,746 | $0.0062 | 6.4 | 2,674 | $0.0088 | 16.7 | 1,072 | +29% |
| `pytest-dev__pytest-5221` | 4,264 | $0.0098 | 14.0 | 3,055 | $0.0109 | 19.5 | 1,209 | +28% |
| `astropy__astropy-12907` | 3,698 | $0.0122 | 20.4 | 3,613 | $0.0071 | 6.3 | 85 | +2% |
| `matplotlib__matplotlib-23314` | 4,096 | $0.0064 | 4.1 | 4,346 | $0.0105 | 13.6 | -250 | -6% |
| `pallets__flask-5063` | 5,739 | $0.0129 | 18.6 | 6,224 | $0.0381 | 84.7 | -485 | -8% |
| `django__django-10924` | 2,743 | $0.0052 | 4.8 | 2,985 | $0.0052 | 4.9 | -242 | -9% |
| `matplotlib__matplotlib-22835` | 8,709 | $0.0327 | 46.5 | 9,505 | $0.0233 | 25.1 | -796 | -9% |
| `pytest-dev__pytest-11148` | 8,360 | $0.0116 | 3.5 | 15,685 | $0.0228 | 10.4 | -7,325 | -88% |

## Provenance

| Field | reddit10 | lite:50 |
|---|---|---|
| Instances | 10 | 50 |
| Server image | (pre-concurrency testing build) | `ghcr.io/rakuensoftware/aimee-server:testing-1d809bb` (vtesting-1d809bb) |
| Driver commit | `38d1516ef005` | `4b46f973a5a6` |
| Primary | `codex` / `gpt-5.6-sol` | `codex` / `gpt-5.6-sol` |
| Worker pool (n=2) | `MiniMax-M3`, `claude` | `MiniMax-M3`, `mimo-v2.5-pro`, `kimi-k2.7-code` |
| Ledger window (primary) | [30396, 30406] | [31182, 31237] |
| Ledger window (supervised) | [30406, 30416] | [31237, 31287] |

## Caveats

- **Economizer row is 0% by design.** Measurements 1 and 2 are the same single-shot primary run; the
  cache-busting nonce forces real spend (no cache reuse), so `avoided` tokens are 0 and economized ==
  default. The economizer's realized/avoided divergence only manifests with cache reuse / multi-turn
  work, which these single-shot runs deliberately exclude to measure true model spend. The delegate arm
  is where the savings land.
- **Time reflects the codex per-model cap of 4** at run time; the primary arm is the wall-clock
  bottleneck (50 solves ÷ 4). Raising the codex session cap would reduce these times.
- **Cost reduction (48.7%) < token reduction (61.6%)** because the supervisor's remaining work is
  output-heavy, and output tokens are priced 8× input.
- A minority of instances cost *more* under delegates (worst: `pytest-dev__pytest-11148`, −88%) — when
  the supervised best-of-N does not compress the primary's work; the aggregate remains strongly positive.

## Reproduce

```
python3 benchmarks/coding/bench_cost_savings.py \
  --regions benchmarks/results/swebench_supervised/regions_lite50 \
  --primary codex --primary-model gpt-5.6-sol \
  --pool MiniMax-M3,mimo-v2.5-pro,kimi-k2.7-code --n 3 \
  --output benchmarks/results/cost_savings/lite50.json
```
Raw per-arm results: `reddit10.json`, `lite50.json`. Per-instance data: `*.perinstance.json`.
