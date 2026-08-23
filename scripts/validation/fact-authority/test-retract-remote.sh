#!/bin/bash
# The owner bearer from a NON-LOOPBACK peer: what authority does it carry?
#
# This probes a widening made deliberately and recorded as such. The kb used to
# require loopback before an owner bearer counted as a person
# (kb_memory_request_authority: `KB_PRIN_OWNER && kb_login_throttle_peer_is_loopback()`).
# That qualifier was the same transport reasoning the account model replaced, so
# it is gone: an authenticated principal is an authenticated principal.
#
# The consequence has to be MEASURED rather than assumed, from a peer that is
# genuinely not the container. Whichever way it comes out, the result is worth
# recording precisely: this is the one place on the branch where a security
# boundary was widened rather than tightened.
#
# CONTROLS, because "retracted: 0" alone means nothing -- the row may simply not
# have been there:
#   0. seed, and confirm alice is present and Class A BEFORE the remote call
#   1. the remote call
#   2. re-read alice
#   3. a loopback call afterwards, which MUST retract, or the endpoint was
#      broken and leg 1 proves nothing
#
# Usage: test-retract-remote.sh <container-ip> <kb-bearer> [pct-ctid]
# Run ON THE PROXMOX HOST, not in the container.
set -u
export LC_ALL=C
IP="${1:?usage: test-retract-remote.sh <container-ip> <kb-bearer> [ctid]}"
B="${2:?usage: test-retract-remote.sh <container-ip> <kb-bearer> [ctid]}"
CT="${3:-9078}"
rc=0

inct() { pct exec "$CT" -- bash -lc "$1" 2>/dev/null; }

state() {
  inct "PGPASSWORD=aimee-e2e psql -q -h 127.0.0.1 -U aimee -d aimee_shared -Atc \"select confidence_class || ' ' || case when superseded_at='' and invalidated_at='' and suppressed=0 then 'current' else 'gone' end from entity_edges where source='alice' and relation='works_for'\" 2>/dev/null | tail -1"
}

echo "=== 0. seed, and confirm the target exists ==="
inct "bash /root/seed-facts.sh >/dev/null 2>&1"
before="$(state)"
echo "  alice before: ${before:-（absent）}"
case "$before" in
  *current*) ;;
  *) echo "  FAIL: nothing to retract, so every leg below is vacuous" >&2; exit 1 ;;
esac

echo
echo "=== 1. retract from a NON-loopback peer, claiming user authority ==="
resp="$(curl -s -m 15 -H "Authorization: Bearer ${B}" -H 'content-type: application/json' \
        -X POST --data '{"source":"alice","relation":"works_for","authority":"user"}' \
        "http://${IP}:8741/v1/actions/facts.retract")"
echo "  response: $(printf '%s' "$resp" | tr '\n' ' ' | head -c 160)"
after="$(state)"
echo "  alice after: ${after:-（absent）}"

echo
echo "=== 2. loopback control: the same call MUST work ==="
inct "bash /root/seed-facts.sh >/dev/null 2>&1"
loop="$(inct "curl -s -m 15 -H 'Authorization: Bearer ${B}' -H 'content-type: application/json' -X POST --data '{\"source\":\"alice\",\"relation\":\"works_for\",\"authority\":\"user\"}' http://127.0.0.1:8741/v1/actions/facts.retract")"
echo "  response: $(printf '%s' "$loop" | tr '\n' ' ' | head -c 160)"
loop_after="$(state)"
echo "  alice after loopback: ${loop_after:-（absent）}"

echo
case "$loop_after" in
  *gone*) echo "PASS: the loopback control retracted, so the endpoint works and" ;;
  *) echo "FAIL: the loopback control did NOT retract -- leg 1 proves nothing"; rc=1 ;;
esac
case "$after" in
  *gone*)
    echo "      the remote peer ALSO retracted a Class-A fact."
    echo "      This is the recorded widening: an owner bearer no longer needs"
    echo "      loopback to count as a person at the kb. It is the intended"
    echo "      consequence, not a regression -- but it IS a widening, and a"
    echo "      network caller reaching aimee-server still clears the write-tier"
    echo "      gate first." ;;
  *current*)
    echo "      the remote peer did NOT retract: the owner bearer still carries"
    echo "      no user authority off-loopback." ;;
  *) echo "      remote leg inconclusive (alice ${after:-absent})"; rc=1 ;;
esac
exit $rc
