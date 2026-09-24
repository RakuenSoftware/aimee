# MR-01: alert inspection excludes erased parents

Unresolved contradiction alerts checked audience but returned suppressed,
quarantined, deleted and revoked parent text. The [HTTP common fixture](memory-mr01-alert-inspection-2026-09-24/http-before-checks.json)
reproduces this on `6fab64fb4`; the [captured synthetic response](memory-mr01-alert-inspection-2026-09-24/http-before-alerts-project.json)
shows each excluded state. A [restricted runtime-role replay](memory-mr01-alert-inspection-2026-09-24/before-race.txt)
also reproduces rejected and unknown-state admission.

Alert inspection now uses an explicit lifecycle policy before ordering and limits.
It retains pending/fulfilled commitments and authorized historical versions,
including future or expired records needed for operator review. It refuses
erased, rejected, quarantined, unknown and suppressed live records, and requires
current generated-card dependencies. Both conflict parents use this gate;
stale-pending and newly-superseded channels use the same inspection policy.
This preserves overdue pending and retained historical alerts rather than
applying ordinary current-serving semantics to an operator view.

The regression checks both conflict positions, historical/pending preservation,
60 newer erased conflicts ahead of the eligible result, and suppressed pending
content. The [targeted race suite](memory-mr01-alert-inspection-2026-09-24/after-race.txt) passes in 1.255 seconds.
The first broader [runtime replay](memory-mr01-alert-inspection-2026-09-24/runtime-timeout.txt)
hit its existing deadline in a graph/cognification path after 306.706 seconds;
its log is retained and the timeout is unchanged. Full-suite rerun and corrected
HTTP validation are pending. MR-01 remains open,
including the final source-check-to-transport race.
