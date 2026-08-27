# Current-stack economizer ROI pilot

This is a calibration result, not a confirmatory product claim. It exercises
the production Go economizer handler in process, then sends paired original and
reduced transcripts directly to the operator-owned local Qwen3.8 endpoint. It
does not exercise the module bus, Aimee delegation, tool recall, or durable
organizational memory.

## Valid paired pilot

- Run ID: `roi-pilot-a82427406ef64a54`
- Source pin: `f29dc522ae345d74de664492a11fd77609933e8c`
- Model: Qwen3.8-27B UD-Q4_K_XL, 65,536-token endpoint context
- Corpus: six deterministic synthetic multi-turn operations handoffs
- Pairing: economizer off versus full history-fold/compress/Coordinate-Closet
  configuration, one repeat, randomized order with seed `20260826`
- Grader: exact match on a planted identifier in the retained tail
- Provider budget: expected and hard maximum marginal spend both `$0.00`

| condition | resolved | input | cache read | output | total | total / resolved |
|---|---:|---:|---:|---:|---:|---:|
| off | 6 / 6 | 26,034 | 162 | 78 | 26,112 | 4,352 |
| full | 6 / 6 | 23,808 | 162 | 78 | 23,886 | 3,981 |

The economizer removed **2,226 provider-counted tokens**, or **8.52% of total
token volume**, with identical resolution and output-token totals. That is 371
tokens per resolved task in this narrow workload. Cache reads were identical,
so they do not explain the delta.

The economizer's internal chars-per-token forecast reported 5,250 removed
tokens across the six reduced prompts, while the provider tokenizer measured
2,226. This is why the public metric uses provider usage objects rather than
the reducer forecast.

The run averaged 7.35 seconds per off call and 6.76 seconds per full call. With
only six sequential observations, this latency difference is diagnostic and
not a performance claim.

Raw artifacts:

- `current-stack-qwen38-tail-pilot.preflight.json` is the persisted budget and
  lineage manifest written before dispatch.
- `current-stack-qwen38-tail-pilot.json` contains every call, provider response
  ID, raw usage object, activation record, exact grade, and paired summary.
- Raw artifact SHA-256:
  `ccac4bb72b1c273f6099791b36809d1d22cab6df81dd883969934325b0ee0bf0`.

## Calibration findings that are not claims

The endpoint defaults to xhigh thinking. A first uncommitted calibration capped
generation at 32 tokens, spent the entire allowance in hidden reasoning, and
returned no final answers. The valid pilot therefore pins
`chat_template_kwargs.enable_thinking=false`.

A second uncommitted calibration planted a secret-like rollback token in the
folded region. The economizer correctly redacted it into a page-back
placeholder, but this direct-provider harness has no `tool_output_get` recovery
loop. The reduced arm therefore could not answer. That calibration is not a
product-quality result; it identifies the recovery-aware workload required for
the next pilot. Both uncommitted calibration artifacts were excluded because
they could not meet the source-lineage gate.

### Excluded tokenizer micro-test

A clean-lineage, one-task Codex tokenizer micro-test is retained for runner
diagnostics only. It is **excluded from product ROI evidence**: the prompt was
synthetic, the answer was a planted identifier, the task was single-shot, and
the agent did not inspect, edit, or test a large repository.

- Run ID: `roi-codex-pilot-848b4adac3f242d6`
- Source pin: `3938b55fc9a410c1b548ea7e8b2a40d7e5f264a7`
- Contract: Codex CLI authenticated through ChatGPT; actual marginal cash cost
  `$0.00`, with API rates reported only as a price equivalent
- Outcome: both off and full resolved the exact-answer task
- Off: 17,690 input (9,984 cached), 14 output, `$0.0350976` API equivalent
- Full: 17,838 input (9,984 cached), 14 output, `$0.0356896` API equivalent

API equivalents use the [official GPT-5.6 Sol model
rates](https://developers.openai.com/api/docs/models/gpt-5.6-sol) accessed on
2026-08-26: `$4.00/M` uncached input, `$0.40/M` cached input, and `$20.00/M`
output below the long-context threshold.

The transformed prompt was 2,880 UTF-8 bytes shorter and the economizer forecast
875 fewer tokens, yet GPT-5.6 Sol reported **148 more input tokens**. The
high-coordinate folded summary contains many conserved unique identifiers and
Unicode placeholders; those were cheaper than the original repetitive log text
under Qwen's tokenizer but not under GPT-5.6's. The only supported inference is
that the local chars-per-token forecast is not a provider bill. This tiny test
cannot estimate Aimee's effect on a real agent trajectory, because it has no
repository exploration, compiler output, test output, retries, cross-language
coordination, or accumulated tool history.

The result is not a negative product finding or a regression estimate. It is a
tokenizer-specific mechanism check with one task and one repeat. Large-repository
agentic experiments supersede it for ROI evaluation.

- Raw artifact: `current-stack-codex-sol-tail-calibration.json`
- Preflight: `current-stack-codex-sol-tail-calibration.preflight.json`
- Raw artifact SHA-256:
  `63e477561a147cedbd90ee10b187e508efbdfb9fa8584126f006e21d4fa9fc5f`

The one-pair low-coordinate mechanism check held model, answer, and grader
constant while replacing unique key/value log lines with verbose natural
language:

- Run ID: `roi-codex-pilot-35035e1ec10e4e36`
- Source pin: `bf0c089a28f2bce3d4114e71ba324073333ecb79`
- Off: 17,130 input (9,984 cached), 14 output, `$0.0328576` API equivalent
- Full: 15,701 input (9,984 cached), 14 output, `$0.0271416` API equivalent
- Both exact answers resolved

That is 1,429 fewer total input tokens (8.34%), 1,429 fewer uncached input
tokens (20.0%), and `$0.005716` lower API-price equivalent for the task. The
opposite signs in the two coordinate-density strata confirm that provider-side
tokenization and content shape are material factors; neither one-pair result is
a population estimate.

- Raw artifact: `current-stack-codex-sol-low-coordinate-calibration.json`
- Preflight: `current-stack-codex-sol-low-coordinate-calibration.preflight.json`
- Raw artifact SHA-256:
  `60f564eae6b8450371960aab7c18eb32fb41ec5607c960a175aadb9b04733c13`

The first three-repeat run reproduced the provider-token effect exactly in all
three pairs: off was 17,130 input tokens and full was 15,701 every time, with
3/3 exact resolutions in both conditions. Aggregate input fell from 51,390 to
47,103, a reduction of 4,287 tokens (8.34%).

It also exposed a cache-isolation defect in the runner. The final full call
reused 13,056 cached tokens while the other calls reused 9,984 because identical
treatment prompts could share cache state across repeats. Total input still
includes cached tokens, so the 4,287-token delta is intact; the aggregate
API-price-equivalent delta is not a clean causal estimate and is rejected.

- Run ID: `roi-codex-pilot-b030d48865ad406d`
- Source pin: `dc1a1d88a239d39b84123f799d380f2668b356cb`
- Raw artifact: `current-stack-codex-sol-low-coordinate-k3.json`
- Raw artifact SHA-256:
  `36d3b43f4aefb43f25ccd12c35d5f82336bf41483cff29dc8f7de5c1cd78ada1`

The cache-isolated rerun added one shared nonce per repeat before the condition
histories. Cache reads then matched within every pair: 9,984 / 9,984, 9,984 /
9,984, and 12,032 / 12,032.

| condition | resolved | input | cache read | uncached input | output | API equivalent |
|---|---:|---:|---:|---:|---:|---:|
| off | 3 / 3 | 51,474 | 32,000 | 19,474 | 42 | $0.091536 |
| full | 3 / 3 | 47,187 | 32,000 | 15,187 | 42 | $0.074388 |

Across the three repeats, economization removed 4,287 total input tokens
(8.33%) and 4,287 uncached input tokens (22.0%), while outputs and exact-answer
quality were identical. At the pinned GPT-5.6 Sol API rates, the price
equivalent fell by `$0.017148` (18.7%). The actual marginal cash charge remained
`$0.00` because the CLI was authenticated through ChatGPT.

This is repeatability evidence for one synthetic task, not three independent
tasks and not a population estimate.

- Run ID: `roi-codex-pilot-299e09879a4b4eb8`
- Source pin: `09fa898d95fe8aa9d6da5efee38d61223c8de177`
- Raw artifact: `current-stack-codex-sol-low-coordinate-k3-cache-isolated.json`
- Preflight: `current-stack-codex-sol-low-coordinate-k3-cache-isolated.preflight.json`
- Raw artifact SHA-256:
  `4176a7edcda71555669ca83c8c1255561a46f7d2c7df569cc0bfc0830fca0219`

## What this supports

This run supports only the narrow statement that the current economizer can
reduce provider-counted input tokens on long, irrelevant history without
changing exact-answer quality when the required information remains in the
retained tail. Repeats, natural coding tasks, tool condensation and recovery,
module-process activation, delegation, and a billable provider remain required
before publication.

## Large-repository crossover stage

The tiny synthetic checks above are not the headline product test. A separate
paired runner now targets four real defects in Aimee itself, including prior
high-context trajectories and a C/Go/JSON boundary change. Each cell starts
from the buggy parent revision in a disposable full-repository worktree. The
plain and Aimee conditions use the same Qwen3.8 model, prompt, tools, maximum
turns, and hidden child-era grader; only current production economization of the
accumulated conversation differs.

The runner records provider-reported billable token buckets on every turn,
exact context-limit errors, economizer activation and warm-boundary reuse, full
recoverable tool output, patch contents, and hidden-test results. It also
measures test authorship rather than merely asking for tests: an authored test
is marked regression-sensitive only when the candidate's visible grader is
green and the test-only diff turns red when reapplied to the original buggy
parent.

A public capacity statement requires an observed pair where the plain agent is
rejected by the provider for exceeding context and the Aimee condition passes
the hidden grader. A plain failure for any other reason is a completion
crossover, not a context-capacity crossover.

### Trust-bundle exploratory pair: more runway, no completion

The first real-repository pair is a valid adverse calibration result, not a
product ROI win.

- Source pin: `79a2ee0df751b7f34b1acb5b17999f79925bb6e0`
- Task: `trust_bundle_readiness`, buggy parent
  `358bab1f7fa4da58b349e1847c0b685a4c5686f0`
- Model/context: Qwen3.8-27B UD-Q4_K_XL, 65,536 tokens
- Limits: 30 turns, 2,048 output tokens per turn, one exploratory repeat
- Marginal provider spend: `$0.00` on the operator-owned local endpoint

| condition | provider responses | terminal reason | input | cache read | output | total | patch | authored tests | hidden grade |
|---|---:|---|---:|---:|---:|---:|---|---|---|
| off | 14 | context limit on turn 15 | 575,923 | 512,806 | 1,291 | 577,214 | none | none | fail |
| Aimee | 30 | maximum turns | 873,612 | 104,072 | 4,321 | 877,933 | none | none | fail |

The plain request reached 64,438 provider-reported input tokens, then its next
natural request was rejected at 68,217 tokens against the 65,536-token window.
Aimee kept every request at or below 43,232 input tokens and completed all 30
allowed provider turns. That establishes additional context runway on this
trajectory, but it is **not** the desired context-capacity crossover: neither
condition produced a patch, authored a test, or passed the hidden grader.

The treatment also cost more total tokens and wall time because it ran more than
twice as many turns. Its 45-minute wall time versus 6.3 minutes for control was
amplified by low prefix-cache reuse: 104,072 cached input tokens for Aimee versus
512,806 for control. More available turns are not ROI when the additional work
does not converge.

The artifact exposed a specific economizer defect. The treatment reported 25
mutated turns, but every mutation was body compression; whole-history folding
never activated, the frozen boundary was reused on 0 turns, and the Coordinate
Closet overflowed on 24 turns. Autonomous OpenAI tool loops have no later plain
user message, while the fold accepted only plain-user boundaries.

Commit `840b8a4ab4` adds a structurally safe boundary immediately before a
complete assistant tool-call cycle. An offline replay of the exact canonical
30-turn transcript through that patched production handler—not a new provider
run—changes the mechanism diagnostics as follows:

| diagnostic | recorded treatment | patched offline replay |
|---|---:|---:|
| true history-fold turns | 0 | 25 |
| byte-stable boundary reuses | 0 | 19 |
| Coordinate Closet overflow turns | 24 | 0 |
| final folded / retained messages | n/a | 62 / 19 |
| internal reduced-token forecast | 633,195 | 562,257 |

The forecast is not provider billing and is not reported as savings. A fresh
paired provider rerun is required to learn whether the fix reduces billed input,
improves cache behavior, stops the rereading loop, or enables completion.

Raw artifacts:

- `large-repo-qwen38-trust-pilot.preflight.json`
- `large-repo-qwen38-trust-pilot.json`
- Raw artifact SHA-256:
  `c72c59ae5f93dc0d7842bdb7729c0ab7aae7095c94bada1985d05fc05412106d`
