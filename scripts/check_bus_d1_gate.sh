#!/usr/bin/env bash
# check_bus_d1_gate.sh — the gate D1 of docs/dev/EVENT_BUS_DECISIONS.md names.
#
# D1 asks module-runtime to amend the wire spec's segment layout from one shared
# segment to several fd-backed regions. Slices 1 and 2 of the feature tree are
# layout-independent and may proceed while that is open; slice 3 and everything
# downstream cannot, because they build the layout the amendment decides.
#
# Rather than leave that as prose nobody runs, the amendment carries a
# machine-readable status line and this script reads it. Run it at slice-3 PR
# open.
#
#   REQUESTED  -> exit 1, slice 3 must not open
#   ACCEPTED   -> exit 0
#   DECLINED   -> exit 1, and D1/D2 must be re-run and re-issued before any
#                 fallback layout is built; it is not a one-slice change.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
spec="$repo_root/docs/proposals/pending/event-bus-wire-spec.md"

if [ ! -f "$spec" ]; then
   printf 'FAIL: cannot find the wire spec at %s\n' "$spec" >&2
   exit 1
fi

status=$(grep -oE '\*\*Amendment status: (REQUESTED|ACCEPTED|DECLINED)' "$spec" |
   head -1 | sed -E 's/.*: //') || true

case "${status:-}" in
ACCEPTED)
   echo "check_bus_d1_gate: ok — the D1 layout amendment is accepted; slice 3 may open"
   exit 0
   ;;
DECLINED)
   cat >&2 <<'MSG'
FAIL: the D1 layout amendment was DECLINED.

Slice 3 must not open against the single-segment fallback without first
re-running the roundtable on D1 and D2 and re-issuing EVENT_BUS_DECISIONS.md.
Reverting to one segment invalidates the enforced half of the v0 threat model —
it re-opens cross-client ring access and slot enumeration — so it is not a
one-slice change and must not be taken by silently widening D2.
MSG
   exit 1
   ;;
REQUESTED)
   cat >&2 <<'MSG'
FAIL: the D1 layout amendment is still REQUESTED.

Slices 1 and 2 are layout-independent and may proceed. Slice 3 and everything
downstream of it are blocked until module-runtime, which owns the wire spec,
marks the amendment ACCEPTED or DECLINED.
MSG
   exit 1
   ;;
*)
   printf 'FAIL: no amendment status line found in %s\n' "$spec" >&2
   exit 1
   ;;
esac
