#!/bin/bash
# What the fleet actually costs right now, per instance and in total.
# The /instances/ list endpoint intermittently returns an empty array even when
# instances are running, so this walks the contracts named in the pool log.
set -u
cd "$(dirname "$0")/.." || exit 1
: "${VAST_API_KEY:?set VAST_API_KEY}"
TOT=0
echo "contract   status     \$/hr     up(min)  arm"
for C in $(grep -oE 'contract [0-9]+' results/vast/vast.log | awk '{print $2}' | sort -u); do
  ARM=$(grep -m1 "contract $C" results/vast/vast.log | sed -E 's/.*--- ([^ ]+) on .*/\1/')
  L=$(timeout 15 curl -s -H "Authorization: Bearer $VAST_API_KEY" \
        "https://console.vast.ai/api/v0/instances/$C/" 2>/dev/null | python3 -c "
import sys,json,time
try:
    d=json.load(sys.stdin).get('instances') or {}
    if not d: raise SystemExit
    print('%s %.4f %.1f'%(d.get('actual_status'),d.get('dph_total') or 0,(time.time()-(d.get('start_date') or 0))/60))
except Exception: pass" 2>/dev/null)
  [ -z "$L" ] && continue
  set -- $L
  printf "%-10s %-10s %7s %9s  %s\n" "$C" "$1" "$2" "$3" "${ARM:-?}"
  TOT=$(python3 -c "print(round($TOT + $2, 4))")
done
echo "-----------------------------------------------"
printf "TOTAL \$%s/hr\n" "$TOT"
timeout 15 curl -s -H "Authorization: Bearer $VAST_API_KEY" https://console.vast.ai/api/v0/users/current/ \
 | python3 -c "
import sys,json
d=json.load(sys.stdin); c=d.get('credit')
print('credit: %s'%('unreadable -- do NOT assume zero' if c is None else '\$%.3f'%c))"
