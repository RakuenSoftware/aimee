# MR-01: dashboard transport retains audience and authority

The advertised `dashboard.memory_stats` KB route discarded request arguments
and invoked the memory runtime without verified caller context. The
[actual HTTP reproduction](memory-mr01-dashboard-transport-2026-09-24/http-before.json)
on `b866f1ac0` returned [11 records and 20 conflict endpoints](memory-mr01-dashboard-transport-2026-09-24/dashboard-before.json)
instead of the project audience's 9 records and 16 endpoints. A second
[scoped-credential reproduction](memory-mr01-dashboard-transport-2026-09-24/authority-before.json)
on `7a73b3bc0` also failed the dashboard boundary while the preceding ordinary
memory routes correctly enforced it.

The C handler now forwards request arguments and verifier-owned context as
separate fields to the existing Go runtime command. It pins the operation to
`stats-dashboard` and preserves the public `payload` envelope. Eligibility,
audience binding and aggregation stay in Go. It refuses an unavailable module
without a database fallback. The external adapter ownership digest was reviewed
and refreshed. Both changed C translation units [compile with warnings as errors](memory-mr01-dashboard-transport-2026-09-24/native-build.txt),
and ownership, C-boundary, module-bus, descriptor and inventory checks pass.
The existing Go statistics regression and full memory suite remain applicable;
this follow-up changes only the transport. The [next transport reproduction](memory-mr01-session-briefing-2026-09-24/http-before.json)
on `143b8d0dc` passes both dashboard audience checks; the
[credential run](memory-mr01-session-briefing-2026-09-24/authority-before.json)
passes project implicit-scope, include-all and forged-authority checks before
failing the separate session briefing. Complete candidate validation follows
the session transport repair.

The common fixture now tests the advertised dashboard for both audiences.
The credential fixture additionally checks implicit scope, attempted
`include_all` widening and forged authority for project/workspace credentials.
This repair does not close the remaining dashboard families or MR-01 as a whole.
