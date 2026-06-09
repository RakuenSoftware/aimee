# Proposal: ingest restoration and recall contract

- **State:** done
- **Completed:** 2026-06-09
- **Author:** JBailes
- **Date:** 2026-06-08
- **Charter role(s):** knowledge-base ingest, Synthesize, Recall

## Shipped work

This proposal is complete. Damaged-ingest restoration now has a concrete
default-off queue and provenance contract, and recall answers expose whether
their evidence is stored verbatim or synthesised.

Implemented behavior:

- `kb_lab` remains a standalone read-only detector; restoration queueing lives in
  the corpus/job layer.
- `db2_corpus_job_mark_restoration_candidate` records a
  `restoration_candidate` stage event and moves the document to
  `stage='restore'`, `stage_status='pending'`.
- The corpus stage runner treats `restore` as a queued/external stage rather
  than trying to repair content inside `kb_lab`.
- `kb_curator_restore_fragment_record` persists a single-pass `restoration`
  artifact with `evidence_mode: "synthesised"`.
- Restored artifacts cite the source document and optional base artifact, link
  `restored_from` / `restores`, and write an `audit_events` row for
  `corpus.restore`.
- The restoration payload records the prompt version, fragment id, confidence,
  and `[unknown]` sentinel status so gaps are explicit instead of fabricated.
- `memory.ask` results now include `evidence_mode`, defaulting normal stored hits
  to `verbatim` and marking L5/synthesis/restoration provenance as
  `synthesised`.
- The KB RPC, server client parser, and MCP structured `memory_ask` response
  carry the evidence mode.

## Verification evidence

- `src/db2/corpus_jobs.c`
- `src/db2/corpus_jobs.h`
- `src/kb/kb_curator_synthesize.c`
- `src/kb/kb_curator_synthesize.h`
- `src/headers/memory.h`
- `src/memory_core_search.inc`
- `src/db2/kb_service_backend_memory.c`
- `src/server/kb_client_memory.c`
- `src/server/server_mcp.c`
- `src/tests/test_corpus_jobs.c`
- `src/tests/test_curator_synthesize.c`
- `src/tests/test_memory_cases_b.inc`
