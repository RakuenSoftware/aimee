# Native memory repairs — 2026-10-08

Native is not required to implement the external memory integration contract. These changes repair native's own correctness and evaluation behavior. Cognee, Hillock and future external integrations must comply with their declared contract; native may retain its own SQL, APIs and optimizations.

## Repairs

- **Shared subject erasure:** select captured authorship in `memory_fact_actors` as well as legacy ownership and source sessions. Freeze actor changes while discovering the erase set, then run the existing transitive deletion/retained-intent path. The existing NOLOGIN privacy definer receives table privileges needed for this lock; request-serving roles receive no additional erasure authority. The regression now creates an authored source with empty `owner_principal`, follows copied descendants/cycles and verifies an unrelated author survives.
- **Recovery admission deadline:** admission consumes the recovery work budget. A timeout before receiving its reservation receipt produces an incomplete `time_exhausted` result and performs no canonical read. A possibly committed pending reservation remains intact, so subsequent requests cannot repeat an unknown round. Outer cancellation and early ledger failures still propagate. Tests cover a saturated admission pool, lost commit receipt, duplicate refusal and an actual locked PostgreSQL admission table.
- **Evaluation database cleanup:** rollback and drop have separate deadlines. Drop only the owner's randomly named disposable database with `FORCE`, terminating orphaned clients; retry an idempotent drop after deadline/unknown receipt or database-in-use failure, at most three times. Fatal errors remain failures. Cleanup does not use the live store URL or terminate sibling databases. Tests leave an orphaned locked transaction, then verify removal and sibling/admin availability.
- **UTF-8 evaluation bootstrap:** the earlier patch explicitly creates UTF-8 databases from SQL_ASCII template0 and verifies encoding in the isolation test.
- **JIT in raw replay fixtures:** the runtime replay's raw administrator SQL now uses transaction-local `jit=off`, matching the native production owner's existing bounded-query setting. Tests no longer require a global login JIT override. This is a fixture correction, not an external memory contract requirement or a claim of unlimited production throughput.

## Verification and deployment

Guest: CT9212 on `192.168.1.253`, address `192.168.0.158`, actual PostgreSQL 18.6, Go 1.26.7. Retain the original [failed acceptance evidence](memory-backends-lxc-253-2026-10-08.md) alongside this follow-up. Private credentials are excluded.

All native repair gates passed:

| Check | Result |
|---|---|
| Targeted race and real-PostgreSQL regressions, three iterations | Pass |
| Full required native owner/evaluation replay | Pass, 398.57 s |
| Memory ownership/C boundary/egress checks | Pass |
| Rebuilt shipping private/shared API baseline | 12/12 pass |
| Actual-author erasure and restart verification | 4/4 pass |
| Full guest reboot: exact corrected private/shared payloads and retained SQL erasure intents | 3/3 pass |

The fixes were built and installed in CT9212, including the restricted-role erasure function/grants. The previously abandoned CT9212 evaluation database was removed after verifying it was owned by the disposable test administrator. Existing identities, Vaults and memory stores were retained. Discord CT9211 and the external provider guests were not redeployed.

The first post-upgrade baseline reruns correctly refused private fixture payloads identical to content erased in the earlier destructive acceptance runs. Retained payload-digest intents were confirmed. The baseline harness now puts its run nonce in each new fixture payload, preserving erasure protection rather than clearing tombstones to make tests pass. The resulting fresh baseline passes 12/12. This is fixture hygiene; it does not qualify re-admitting erased content or change the native erasure policy.

[Follow-up machine-readable evidence](native-memory-fixes-2026-10-08/). Original failed acceptance results remain preserved. Native's own regression success does not certify external contract compliance or close the Cognee/Hillock shipping gaps.

## Hillock-specific versus shared findings

| Finding | Classification | Evidence/impact |
|---|---|---|
| Missing chatbot recall, KB briefing and verified public export | Shared Aimee external-adapter/host gaps | Both shipping guests failed these routes. |
| Permanent author tombstones reject new writes after erasure | Shared compatibility-catalog gap | Both external guests return the same conflict. |
| 256 eligible-record retrieval ceiling | Shared adapter bound | Both adapter packages define `MaxRecords=256`; both guests refuse record 257 without losing durable records. This is not uniquely a Hillock upstream limitation. |
| Native recovery deadlines/evaluation cleanup | Shared Aimee native regression failures observed across lab guests | Neither originates in the provider. |
| Relation re-ingestion overwrites original `source_doc` | Confirmed on pinned Hillock graph | Its triple primary key and replacement write keep one reporter; deployed stateless ranking avoids this path. Equivalent Cognee graph behavior was not tested. |
| Name-component entity linking returns person and mountains | Confirmed on pinned Hillock entity linker | `Kibukx`/`Kibukx_mountains` reproduction. Deployed sidecar does not use this linker. Equivalent Cognee entity resolution was not tested. |
| Greeting calls missing `query_ollama_stream` | Hillock-specific source bug | Pinned non-streaming chat branch fails. Aimee owns replies and does not use this renderer. |
| Subword/HYDRA retrieval profile | Hillock-specific implementation | Lexical/morphological ranking; negated height query still retrieves the positive claim. This alone does not establish Cognee's semantic superiority or absence of the same answer error. Cognee used deterministic local inference fixtures in these tests. |
| Extra token/vocabulary/CPU limits | Hillock-sidecar-specific limits | 64 query tokens, 256 tokens/record, 4096 distinct snapshot tokens, 128 UTF-8 bytes/token, 10 s compute deadline. Cognee shares the record/body bounds but uses synchronous indexing and its own deadlines. |

Only the last two rows affect the currently deployed Hillock retrieval path specifically. The graph/linker/greeting defects are confirmed upstream findings which matter if those optional paths are enabled; they did not cause the shared shipping failures. “Confirmed only in Hillock” does not mean Cognee was proven free of an analogous defect. Full real-model answer-quality comparison remains unqualified.
