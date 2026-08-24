#!/bin/bash
# Why does the TCP leg get 401 on the FIRST suite run after prepare-suite?
#
# Observed: `same body, both transports` fails with "refused at the auth wall"
# on the run immediately following prepare-suite.sh, and passes on every run
# after that. Run alone it also passes. So something prepare-suite leaves behind
# is consumed by the first request and not the second.
#
# This makes the state visible instead of guessed at: what bearer the file holds,
# what the server thinks it has, and what the server logs when the call lands.
# Run AS ROOT in the container.
set -u
export LC_ALL=C
B="$(cat /root/api-bearer.txt)"

echo "=== bearer on disk ==="
echo "  ${B:0:12}... (${#B} chars)"

echo
echo "=== extras sealed in the vault ==="
for i in 0 1 2 3 4 5; do
  v="$(grep -ao "AIMEE_API_BEARER_TOKEN_EXTRA_$i" /root/aimee.db 2>/dev/null | head -1)"
  [ -n "$v" ] && echo "  $v present"
done

echo
echo "=== remote_client_grants ==="
sqlite3 -header -column /root/aimee.db \
  "SELECT principal,tier,substr(cert_serial,1,12) AS serial FROM remote_client_grants" 2>/dev/null

echo
echo "=== TCP call, verbatim ==="
mark="$(wc -l < /root/server.log 2>/dev/null || echo 0)"
out="$(curl -s -m 30 -o /tmp/tcp.out -w '%{http_code}' \
        -H "Authorization: Bearer $B" -H 'content-type: application/json' \
        -X POST --data '{"source":"user","relation":"age","authority":"user"}' \
        http://127.0.0.1:8740/v1/facts/retract)"
echo "  http: $out"
echo "  body: $(head -c 220 /tmp/tcp.out)"

echo
echo "=== server log since the call ==="
tail -n +"$((mark + 1))" /root/server.log 2>/dev/null | tail -8 | sed 's/^/  /'

echo
echo "=== same call again, immediately ==="
out2="$(curl -s -m 30 -o /tmp/tcp2.out -w '%{http_code}' \
        -H "Authorization: Bearer $B" -H 'content-type: application/json' \
        -X POST --data '{"source":"user","relation":"age","authority":"user"}' \
        http://127.0.0.1:8740/v1/facts/retract)"
echo "  http: $out2"
echo "  body: $(head -c 220 /tmp/tcp2.out)"

echo
if [ "$out" != "$out2" ]; then
  echo "REPRODUCED: the same call answered $out then $out2 -- the first request"
  echo "            differs from the second, so state is being consumed or settled."
else
  echo "both calls agreed ($out); the difference is not request-to-request here"
fi
