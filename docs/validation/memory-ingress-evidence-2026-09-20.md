# Accepted ingress evidence validation

Automatic ingress previously recorded all retrieved memory and code candidates
before Go budget packing and before the host integrity decision. An omitted memory
could therefore receive outcome feedback, and a rejected envelope could still
publish retrieval evidence.

The Go owner now returns retained code indices and retained memory IDs/previews
from the same selected entries that render the envelope. Previews are the exact
single-line, clipped text in the envelope. The host copies those selections only
after successful assembly, integrity acceptance and envelope allocation. Scoped
transport remains active through emission and is cleared on every exit. Evidence
remains optional; the existing envelope bytes, retrieval order and limits are
unchanged. Selection stays Go; the external host transports it. The C bus is
unchanged.

This establishes the **assembled-envelope boundary only**. It does not certify
provider admission, dispatch, acknowledgement, source-version freshness or complete
coverage of typed facts/task-context/audit entries. Durable MR-06 stage receipts
and final provider-request byte/token accounting remain open. The existing ranker
document-search bridge is a separate surface; this patch does not change it.

## Local validation

The real Go owner/native host fixture covers full-width int64 IDs, partial memory
packing, oversized code exclusion without displacing smaller memories, exact
clipped feedback, assembly failure, empty context, integrity rejection and scope
cleanup on event-ID failure. The unchanged envelope golden and existing task,
facts, temporal and compression cases pass. Focused Go tests cover source-index
mapping with a preceding task block and skipped code entry, multibyte clipping,
and empty retained lists. Native ownership/boundary checks and all 14 frozen
semantic-context comparison tests pass. No new whole-request latency claim is made.

The complete required PostgreSQL-backed race suites pass: memory in 57.382 seconds
and families in 1.442 seconds.

Full CI passed on predecessor `9bd72bfcd0`
([run 35529494801](https://github.com/RakuenSoftware/aimee/actions/runs/35529494801)).
This is separate from CI on the ingress correction itself.

## Fresh deployment regression evidence

Application and harness `40039465c2` pass **638/638** checks in new owned
`.253` CT 9498 deployments:

- T2: **429/429** — 168 private, 208 shared, 29 correction-review, six identity
  and 18 topology checks.
- T3: **209/209** — 168 private, 15 real-model semantic, 21 exploratory and
  five topology checks.

The [T2 receipt](memory-shared-reliability-2026-09-20/fresh-t2-40039465c2.json)
and [T3 receipt](memory-shared-reliability-2026-09-20/fresh-t3-40039465c2.json)
retain only check names and verdicts. This matrix verifies deployment and memory
regressions, including concurrency, exact IDs, isolation, restart, owner outage,
semantic recovery and review/retry behavior. The ingress-specific omission and
integrity assertions are covered by the native-host/real-Go fixture described
above; this matrix does not establish provider dispatch receipts.

All nine containers were independently verified against these image IDs:

- Application: `sha256:88baef181850b8cc298cf6620a198ace5b7931bc6a5e36e7e7ef5b8c1bbf6410`.
- PostgreSQL: `sha256:b6209cde68c9a7a65c562b8a4ca45682f138b4a2de5b5dbcfe7ca04ec48e962f`.
- Embedder: `sha256:f1286af7de10cf058a9bec14c45326d64de73e3878db19c732db86cda1f9d979`.

Raw results remain in `/opt/aimee-memory-proposals-evidence/t2-40039465c2`
and `t3-40039465c2`. Test containers are stopped; evidence and volumes remain.
The documentation/evidence follow-up does not change application or harness code.
