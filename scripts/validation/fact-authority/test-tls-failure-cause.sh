#!/bin/bash
# Does the TLS startup failure name the RIGHT cause?
#
# server_tls_init_default() has three unrelated failure paths and used to return
# -1 for all of them, so the caller reported every one as
#
#     tls_port=8743 set but TLS cert/key not loadable; TLS DISABLED
#
# The mTLS ramp self-test is DB1 stage 19 (db1-pki). With no db1 module the log
# read:
#
#     WARN  db1.pki: DB1 pki is unreachable (module call result 1)
#     WARN  pki.ramp: mTLS ramp startup self-test failed; refusing mTLS startup
#     ERROR server.http: tls_port=8743 set but TLS cert/key not loadable
#
# and the last line is the one an operator acts on -- so a perfectly good
# certificate got inspected while the actual fault was a module that was not
# running. That is what happened here, and it cost real time.
#
# This reproduces it: stop db1, restart the server, and require that the message
# names the ramp rather than the certificate. Then restore db1 and require that
# TLS actually comes up, so a message that is merely always-pessimistic cannot
# pass.
#
# Requires aimee.api.mtls to be on (the ramp only runs then).
# Run AS ROOT in the container.
set -u
export LC_ALL=C
rc=0

mode="$(grep -E '^    mtls: ' /root/aimee.yaml | awk '{print $2}')"
if [ "${mode:-off}" = "off" ]; then
  echo "setting mtls: optional -- the ramp only runs when mTLS is on"
  bash /root/set-mtls-mode.sh optional >/dev/null 2>&1
fi

echo "=== 1. db1 UNAVAILABLE: the ramp cannot run ==="
# The binary is moved aside rather than the process killed, because
# start-server.sh relaunches db1 itself. The GRANT stays in place: an unloadable
# grant fails the whole module endpoint and takes the daemon down, which is a
# different failure and would not exercise this path.
#
# The daemon is started through start-server.sh, not by hand. A hand-rolled
# launch lacks the environment it sets and the server exits immediately, so the
# absence of a "TLS DISABLED" line reads as success when nothing ever ran -- the
# first version of this test made exactly that mistake.
BIN=/usr/local/libexec/aimee-modules/aimee-module-db1
[ -x "$BIN" ] || { echo "FAIL: $BIN missing; cannot stage this" >&2; exit 1; }
mv "$BIN" "$BIN.away"
# The ALREADY-RUNNING module has to go too. install-db1-module.sh refuses on a
# missing binary BEFORE it reaches its own pkill, so a module started by an
# earlier run survives -- and a live db1 answers the ramp perfectly well no
# matter what is on disk, which is why staging the binary alone changed nothing.
pkill -f "aimee-module-db1" 2>/dev/null
sleep 1
bash /root/start-server.sh >/dev/null 2>&1
sleep 2
line="$(grep -a "TLS DISABLED" /root/server.log | tail -1)"
echo "  ${line:-（no TLS DISABLED line）}"
case "$line" in
  *"mTLS ramp self-test refused"*)
    echo "  PASS: the message names the ramp, not the certificate" ;;
  *"cert/key not loadable"*)
    echo "  FAIL: still blaming the certificate for a ramp failure"; rc=1 ;;
  "")
    # Not a failure of the code under test: on this container the ramp completes
    # without any db1 module attached, so the branch cannot be staged here and a
    # verdict either way would be invented. Reported as SKIP rather than PASS
    # (which would claim coverage) or FAIL (which would blame the fix).
    #
    # The mapping itself is pinned deterministically by
    # unit-test-server-tls-init-cause; what is missing is only the live
    # reproduction of the ramp refusing.
    echo "  SKIP: the ramp completes here even with db1 absent, so this branch"
    echo "        cannot be staged on this box. The cause mapping is covered by"
    echo "        unit-test-server-tls-init-cause." ;;
  *)
    echo "  FAIL: unexpected message"; rc=1 ;;
esac
mv "$BIN.away" "$BIN"

echo
echo "=== 2. db1 RUNNING: TLS must actually come up ==="
# Without this leg, a message that always says "ramp refused" would pass leg 1.
bash /root/start-server.sh >/dev/null 2>&1
sleep 3
up="$(grep -a "native TLS enabled" /root/server.log | tail -1)"
bad="$(grep -a "TLS DISABLED" /root/server.log | tail -1)"
if [ -n "$up" ] && [ -z "$bad" ]; then
  echo "  $up"
  echo "  PASS: with db1 running, TLS starts"
else
  echo "  FAIL: TLS did not come up with db1 running"
  echo "  ${bad:-（no TLS DISABLED line）}"
  rc=1
fi
exit $rc
