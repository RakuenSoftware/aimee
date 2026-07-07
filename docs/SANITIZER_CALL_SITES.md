# Prompt-sanitizer call-site register

The single render-boundary sanitizer for corpus-derived strings that flow into an
agent's prompt is `sanitize_for_prompt(field, kind, out, out_len, out_reason)` in
`src/kb/prompt_sanitizer.c` (proposal `graph-feedback-self-audit-and-learning` §4 /
P0). This document is the **audited register** of every place untrusted,
corpus-derived text is rendered into an agent-visible surface, and which sanitizer
kind guards it. It is enforced by `scripts/check-sanitizer-callsites.py` (run from
`make lint`): the register and the code must stay in lockstep.

## The boundary contract

- **One boundary.** Untrusted fields (file paths, symbol labels, community names,
  memory-fact bodies, lessons, corrections, model-generated captions/transcripts,
  Markdown bodies) are sanitized *once*, at the point they are placed into an
  agent-visible string, via `sanitize_for_prompt`. No ad-hoc escaping elsewhere.
- **Two layers.** Structured kinds (`SANITIZE_FILE_PATH`, `SANITIZE_SYMBOL_LABEL`,
  `SANITIZE_SOURCE_LOCATION`, `SANITIZE_COMMUNITY_NAME`) are strict-validated and
  **rejected** on control/markup. A caller must fail closed on `SANITIZE_REJECTED`.
  Free-text kinds are defanged in place and never rejected.
- **Status is load-bearing.** A security-sensitive renderer must branch on the
  returned `sanitize_status_t` (`OK` / `TRUNCATED` / `REJECTED`), not ignore it.

## Enumerated injection markers neutralized

Kept in sync with `kMarkers[]` in `prompt_sanitizer.c` and the categorized attack
corpus in `src/tests/test_prompt_sanitizer.c`:

- **Special-token delimiters:** `<|` … `|>` (e.g. `<|im_start|>`, `<|im_end|>`,
  `<|endoftext|>`).
- **Role tags:** `<system>`, `<assistant>`, `<user>`, `<tool>`, `<human>` (and
  their closing forms).
- **Instruction headers:** `### Instruction:`, `### System`, `### Response`,
  `### Human`, `### Assistant`.
- **Bracket tags / fabricated log lines:** `[graphify]`, `[system]`,
  `[assistant]`, `[inst]`, `[/inst]`.
- **Control/escape (all kinds):** ANSI CSI/OSC escapes, C0/DEL controls
  (newline/tab preserved only in free text), UTF-8-encoded C1 controls.

## Call-site register

Every non-test, non-impl call to `sanitize_for_prompt(` in `src/` MUST appear here
with the file and the kind it applies. The guard fails if a code call is
undocumented, or a documented file no longer calls the sanitizer.

| File | Render surface | Kind(s) | Slice |
|------|----------------|---------|-------|
| `src/kb/http/kb_http_code_graphfb.c` | `GET /v1/code/graph/audit` findings (cycle file paths, orphan/bridge/unverified symbol labels, community names) **and** `GET /v1/code/graph/diff` entries, **and** `GET /v1/code/lessons` artifact (node keys + community labels) rendered into the agent-visible JSON. | `SANITIZE_FILE_PATH`, `SANITIZE_SYMBOL_LABEL`, `SANITIZE_COMMUNITY_NAME` | S1, S2 |

> The audit route renders corpus-derived node/file/community strings; each goes
> through `audit_safe()` → `sanitize_for_prompt` (fail-closed to `[unsafe-label]`
> on `SANITIZE_REJECTED`). When S3 (lessons) or S5/S6 (grammars / media captions)
> render corpus text, add a row here in the same PR that adds the
> `sanitize_for_prompt` call, or `make lint` fails.
