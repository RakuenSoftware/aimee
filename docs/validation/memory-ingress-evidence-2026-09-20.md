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
