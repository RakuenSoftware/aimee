# Complete hard-rule recall and bounded packing

Production implementation and deployment harness: `f6e544e060`.
Application image:
`sha256:c9ccceb88e9aee841dc30b5e3332b01065809cb83051d3ca6f30fb2686a4e56e`.

Go recall previously fetched at most eight hard rules per turn or sixteen at
session start, then removed rules after optional rows when the bundle exceeded
its allocation. It now returns the complete stored hard-rule set or an explicit
`protected_context_overflow` with no partial recall payload. Private/shared
composition preserves that refusal. No failed packing increments directive
surfacing counters.

The database query bounds candidate count using the minimum possible serialized
rule size, fetching one beyond any possible fit. A cumulative raw-text bound
replaces oversized returned text with an overflow marker before it crosses the
database bus. Go checks exact serialized rule bytes and complete envelope size;
the marker never becomes a truncated served rule. Optional rows retain their
existing section priority and are never split. Bounded prefix search replaces
repeated serialization after each discarded row.

MCP previously discarded the owner's error while extracting `recall`, and could
return session guidance instead. It now forwards the original error before
optional guidance. HTTP asks the existing runtime-web classifier to map opaque
owner failures, producing HTTP 413 for protected overflow. Classification failure
still gives an upstream failure, not HTTP success. These native changes transport
owner decisions; the memory module remains Go and the C bus is unchanged.

## Local validation

- Go race tests pass for the complete memory package, runtime-web and bundled
  launcher, with PostgreSQL integration required.
- Restricted-role replay additionally passes the final cumulative query bound:
  more than sixteen rules, a large low-priority rule, six hundred minimal rules,
  and six hundred large rules all preserve completeness or explicitly refuse.
- Exhaustive packing comparisons cover section priority, Unicode/JSON escaping,
  estimate digit boundaries, required content and deterministic maximal prefixes.
- Private/shared composition propagates overflow without personal fallback.
- Native HTTP tests cover classified refusal and unavailable classification.
  Actual C-host/Go-process runtime-web conformance covers the new outcome.
- The standalone Go memory export builds and its recall packing regressions pass;
  exporting leaves the repository lock unchanged.
- All 77 lint checks pass, including the reviewed external-adapter ownership
  ledger and pure-Go memory/C-bus boundaries.
- All 767 benchmark-suite tests run successfully, with two existing skips. The
  exact MCP memory integration review is updated; semantic-source drift remains
  rejected. The frozen LSP candidate and performance thresholds are unchanged.

All **58 remote CI checks pass** on `2269c6c1b9`, which adds the exact reviewed
MCP integration record to production implementation `f6e544e060` without changing
production code or the deployment harness. This includes sanitizers, the complete
script suite, Linux/macOS provider probes, platform builds, all deployment jobs
and encrypted 0.4.1 upgrade/rollback coverage.
[CI receipt](memory-shared-reliability-2026-09-21/ci-2269c6c1b9.json).

## Packing microbenchmark

Three runs on Linux/amd64, Intel i7-14700K, with sixty-four optional Unicode rows
and a hard rule, fitting a 600-unit legacy allocation:

| Packer | Time | Allocated bytes | Allocations |
|---|---:|---:|---:|
| Previous row-by-row removal | 14.26–14.42 ms | 15.52–15.55 MB | 282–284 |
| Bounded prefix search | 1.281–1.307 ms | 1.387–1.396 MB | 47 |

This is about 11 times faster with about 91% fewer allocated bytes in the stress
fixture. It excludes retrieval, transport and provider work and is not a
whole-request P95 or MR-18 quality/performance result.
[Raw samples](memory-shared-reliability-2026-09-21/recall-packing-benchmark-f6e544e060.json).

## Fresh deployment evidence

Fresh isolated deployments in task-owned CT 9498 on `.253` pass
**1,261/1,261 checks: 778 T2 and 483 T3**. T2 adds fifteen checks for complete
hard-rule HTTP/CLI reads, optional evidence trimming, explicit HTTP 413, MCP
refusal with session-start guidance enabled/disabled, oversized stored text and
recovery. Both placements retain 273 provider-boundary checks with a 32 KiB
operator ceiling. Existing restart, outage, isolation, correction, semantic and
exploratory checks pass.

The actual application, PostgreSQL and embedder image identities were verified
for all nine containers, and every application has the configured deployment
ceiling. The three projects are `aimee-e2e-kb-6b28849aa7`,
`aimee-e2e-server-0ebbf93d26` and `aimee-e2e-server-db1b279c0e`.
All nine containers are stopped; volumes are retained.
Raw evidence is retained in `/opt/aimee-memory-proposals-evidence/` in CT 9498;
committed receipts omit credentials and provider prompt bodies.

- [T2 verdicts](memory-shared-reliability-2026-09-21/fresh-t2-f6e544e060.json)
- [T3 verdicts](memory-shared-reliability-2026-09-21/fresh-t3-f6e544e060.json)
- [Provider accounting](memory-shared-reliability-2026-09-21/provider-accounting-f6e544e060.json)
- [Verified images and limits](memory-shared-reliability-2026-09-21/image-identities-f6e544e060.json)

## Acceptance limits

The legacy `limit_tokens` allocation still uses bytes/4; it is not an exact
provider token counter. Preserving the existing stored hard-rule classification
does not prove rule-promotion authority or grant protection to model-labeled
memory. All-provider refusal, required user-constraint transformation checks,
source-version release binding and durable dispatch receipts remain open.
This change does not certify MR-03 or the full MR-01–MR-18 program.
