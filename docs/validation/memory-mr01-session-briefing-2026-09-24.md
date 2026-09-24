# MR-01: session briefing carries verified audience

The session briefing adapters retained only `limit`, dropping both supplied
audience arguments and the verified caller context before invoking Go. The
[common HTTP reproduction](memory-mr01-session-briefing-2026-09-24/http-before.json)
on `143b8d0dc` passes dashboard and directive recall checks, then fails the
project session briefing. The [credential reproduction](memory-mr01-session-briefing-2026-09-24/authority-before.json)
likewise passes the fixed dashboard boundary and fails the session briefing.

Both `session_briefing.directives` and `session_briefing.commitments` now use the
same context-preserving transport as dashboard statistics. The operation is
selected by the registered handler; request text cannot replace it. C forwards
arguments and host context, then maps Go's `block` field to the established
`body` field. It performs no memory selection, rendering or eligibility policy.
An unavailable module returns an error instead of an apparently successful
empty briefing. Global reminders retain their existing shared-store behavior.

The changed native translation units [compile with warnings as errors](memory-mr01-session-briefing-2026-09-24/native-build.txt).
C-boundary, ownership and module-bus guards pass. Go source is unchanged from
the directive candidate's passing full race suite and export. Candidate `50282c8ba` passes the [expanded 100-check common fixture](memory-mr01-common-derived-2026-09-24/checks.json)
and [28/28 authority checks plus 128 concurrent requests](memory-mr01-session-briefing-2026-09-24/authority-after/checks.json).
Both project and workspace tests attest the dashboard and session briefing
repairs, including implicit shared visibility and attempts to widen authority.

The same candidate passes the fresh deployment matrix: **1714/1714** checks,
including 1097 T2 and 617 T3 results, with both harness exit codes zero.
The raw [fresh evidence directory](memory-mr01-session-briefing-2026-09-24/fresh)
contains process receipts, nine image identities, three provider caps and
verified cleanup of the owned containers/networks. This includes placement,
module outage, scope isolation, provider-bound context and asynchronous memory
refusal/recovery checks. It does not certify MR-01's remaining release race.
