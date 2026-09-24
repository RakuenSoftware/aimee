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
the directive candidate's passing full race suite and export. Corrected common
and scoped-credential HTTP validation is pending. The fixture now has 62 common
checks and 28 authority checks plus 128 concurrent requests.
