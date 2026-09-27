# MR-12 served views and claim cards: functional closeout

MR-12 explicit serving is functionally complete at candidate `5d4bcab87`.
The image is `sha256:139d2956cdab1bc707d1764ea3646cdddecdd4e29bc5d43abff70d6d3970c1dc`.
Both existing CT109 owners upgraded from the MR-11 candidate, preserved their
PostgreSQL stores and enrolled identities, and passed health checks. Production
CT100 remains on its healthy 0.4.5 server, embedder and PostgreSQL. No optional
ranking/horizon policy or automatic view injection was enabled.

## Delivered behavior

Nine named views share Go selection, scope, eligibility, dependency, coverage,
budget and invocation-receipt contracts. CLI, HTTP, MCP and native-agent tools
translate to that owner. The private placement reports unavailable historical,
procedure and contradiction adapters explicitly; it never fills them from KB.
Hard rules precede scoped constraints, followed by coherent contradictions and
current evidence. Failure candidates require recorded origin lineage. Procedure
applicability/outcomes and task sufficiency remain unknown when the owner lacks
corresponding evidence or explicit task obligations.

Read-only claim cards bind canonical revisions, authorship, lineage, calibration
uncertainty, bounded visible contradictions, and expected-version correction.
Evidence expansion reads accessible exact versions; cards have no editable
canonical store. Claim-specific contradiction filtering precedes the bound.

Caching reuses serialization only after a fresh owner transaction recomputes
selection, time applicability and collection dependencies, including empty ones.
Standby owners are rejected. Each invocation gets a fresh receipt binding exact
rendered bytes and retained sources. This explicit projection receipt is not a
provider-dispatch receipt; MR-06/source release still governs provider handoff.
Rich diagnostic fields are excluded from the minimal model-facing rendering.

## Validation

- Full PostgreSQL memory race suite: passed in 399.607 seconds; exported module
  build passed. Final owner/discovery delta: passed in 1.760 seconds.
- Native transport tests and 131 argument specifications / 1,253 differential
  samples passed. MCP adapters compiled and were exercised through the server.
- All 77 lint checks ran. The failing parity/ownership/format checks were fixed
  and rerun successfully; the generated-document check passed after commit.
- Live CT109 harness: **43 checks passed**, including private/shared fresh
  receipts on cache reuse, insert/correction invalidation, zero-byte budgets,
  claim-card uncertainty, exact CLI/HTTP/MCP rendering parity, both visible
  contradiction sides, hidden-side exclusion, whole-bundle revocation, and a
  clock-only validity transition without changing the canonical revision.
  Explicit historical coordinates bind different cache identities.
- Owner tests cover every named composition, hard-rule insertion after cached
  selection, source validity, exact accounting, task/time/budget identity,
  unknown lineage, private isolation and minimal model text.

The first full run failed stale discovery counts; the rerun above fixed them.
Earlier native fixture setup and evidence fixtures were corrected rather than
counted as passes. One evidence fixture initially assumed a SQL insert lacked an
origin event; the owner correctly creates one. The corrected test checks both
that origin and an uninterpretable upstream dependency. Exact live synthetic
records were erased after acceptance, without touching unrelated records.

[Evidence manifest](memory-mr12-evidence-2026-09-27/manifest.json) binds retained
logs, live checks, image identities and reproducible harnesses. Named views
require a client with the compiled nested-JSON marshaller; no incomplete dynamic
argument specification silently drops budgets or requirements. Hard token
limits remain explicitly unsupported. Automatic preload remains off.
