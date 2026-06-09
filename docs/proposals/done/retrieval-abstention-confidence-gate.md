# Proposal: Recall abstention confidence gate

- **State:** done
- **Completed:** 2026-06-09
- **Author:** JBailes
- **Date:** 2026-06-09
- **Charter roles:** Recall, Calibrate / Evaluate-Optimize, Gate-Promote,
  Extract

## Shipped work

This proposal is complete for the intended rollout slice: a default-off,
deterministic answerability gate with a structured evidence trace and a shared
no-answer contract across recall surfaces.

Implemented behavior:

- `memory.abstain.enabled`, `memory.abstain.gate`, and the chunk confidence
  floor are parsed through config and surfaced in generated configuration docs.
- `memory_answerable` evaluates retrieved evidence and writes a bounded
  `memory_answer_evidence_t` trace with decision, reason, score, threshold,
  anchor, and support metadata.
- `memory.ask` refuses weak evidence as `no_answer` when the abstention gate is
  enabled, while preserving the existing default-off behavior for normal users.
- Citation-required failures share the same no-answer path instead of bypassing
  the structured trace.
- Curated L4/L5 anchors remain exempt from weak-evidence abstention.
- MCP, KB RPC, and client paths serialize and render the no-answer/evidence
  contract consistently.
- The context-assembly path withholds weak memory evidence behind the same
  switch and emits the `## Memory Answerability` sentinel instead of relying on
  the old soft low-confidence marker.
- Runtime state records abstention events for later evaluation.
- DB2-disabled and stub builds keep an explicit no-op contract instead of
  failing to compile when answerability fields are present.

Calibration to a default-on threshold remains intentionally outside this done
record: it requires labeled ask-outcome data and bench acceptance, not another
code surface.

## Verification evidence

- `src/headers/memory.h`
- `src/headers/config.h`
- `src/config.c`
- `src/config_sections.c`
- `src/config_fields.c`
- `src/memory_core_search.inc`
- `src/memory_assemble.c`
- `src/server/server_mcp.c`
- `src/server/kb_client_memory.c`
- `src/db2/kb_service_backend_memory.c`
- `src/kb/kb_service_memory.c`
- `src/tests/test_memory_advanced.c`
- `docs/gen/configuration.md`
