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

### Codex high-coordinate calibration

A clean-lineage, one-task Codex calibration is retained as a negative result:

- Run ID: `roi-codex-pilot-848b4adac3f242d6`
- Source pin: `3938b55fc9a410c1b548ea7e8b2a40d7e5f264a7`
- Contract: Codex CLI authenticated through ChatGPT; actual marginal cash cost
  `$0.00`, with API rates reported only as a price equivalent
- Outcome: both off and full resolved the exact-answer task
- Off: 17,690 input (9,984 cached), 14 output, `$0.0350976` API equivalent
- Full: 17,838 input (9,984 cached), 14 output, `$0.0356896` API equivalent

The economized prompt was 2,880 UTF-8 bytes shorter and the economizer forecast
875 fewer tokens, yet GPT-5.6 Sol reported **148 more input tokens**. The
high-coordinate folded summary contains many conserved unique identifiers and
Unicode placeholders; those were cheaper than the original repetitive log text
under Qwen's tokenizer but not under GPT-5.6's. This is evidence that the local
chars-per-token forecast is not a provider bill and that savings are not
provider-neutral.

The result is a calibration, not a regression estimate: it has one task and one
repeat. It motivates a preregistered coordinate-density stratum and repeated
Codex pairs.

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

## What this supports

This run supports only the narrow statement that the current economizer can
reduce provider-counted input tokens on long, irrelevant history without
changing exact-answer quality when the required information remains in the
retained tail. Repeats, natural coding tasks, tool condensation and recovery,
module-process activation, delegation, and a billable provider remain required
before publication.
