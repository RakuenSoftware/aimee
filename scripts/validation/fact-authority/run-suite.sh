#!/bin/bash
# Run the whole fact-authority validation suite, in order, under the right
# configuration for each probe, and report one summary.
#
# WHY THIS EXISTS. The order and the required posture were only ever recorded in
# scattered comments and in whoever last ran them. Two probes need OPPOSITE
# mTLS settings and each refuses to run under the wrong one, several need a
# re-seed between them because they destroy the rows they act on, and a couple
# depend on modules that must be attached before the daemon reads modules.d.
# Getting any of that wrong produces a green run that proves nothing, which is
# the failure mode this whole suite exists to avoid.
#
# Each probe already owns its own controls; this only sequences them and
# reports. A probe that SKIPS is reported as a skip, never folded into pass.
#
# Usage: run-suite.sh [SERVER_BEARER]
# Run AS ROOT in the container.
set -u
export LC_ALL=C
BEARER="${1:-$(cat /root/api-bearer.txt 2>/dev/null || echo)}"
pass=0 fail=0 skip=0
failed_names=""

hdr() { printf '\n\033[1m=== %s ===\033[0m\n' "$1" 2>/dev/null || printf '\n=== %s ===\n' "$1"; }

run() { # $1 = label, $2.. = command
  local label="$1"; shift
  local out status
  out="$("$@" 2>&1)"
  status=$?
  if printf '%s' "$out" | grep -q "^ *SKIP:"; then
     skip=$((skip + 1))
     printf '  %-34s SKIP\n' "$label"
     printf '%s\n' "$out" | grep "^ *SKIP:" | head -2 | sed 's/^/      /'
  elif [ $status -eq 0 ] && ! printf '%s' "$out" | grep -q "FAIL"; then
     pass=$((pass + 1))
     printf '  %-34s PASS\n' "$label"
  else
     fail=$((fail + 1))
     failed_names="$failed_names $label"
     printf '  %-34s FAIL\n' "$label"
     printf '%s\n' "$out" | grep -E "FAIL|Assertion" | head -3 | sed 's/^/      /'
  fi
}

seed() { bash /root/seed-facts.sh >/dev/null 2>&1; }

# Bringing the kb up means its MODULES too. A probe that restarts a daemon
# orphans every module attached to its bus, and nothing restarts them -- so the
# rest of the run executes with stages missing while each module process is
# either gone or, worse, still alive against a dead socket. "state: RUNNING" is
# not "attached": the check that matters is whether a stage answers.
bring_up_kb() {
  bash /root/install-config-module.sh grants   >/dev/null 2>&1
  bash /root/install-postgres-module.sh grants >/dev/null 2>&1
  bash /root/start-kb.sh >/dev/null 2>&1
  bash /root/smm.sh >/dev/null 2>&1
  bash /root/install-postgres-module.sh >/dev/null 2>&1
  sleep 2
}

bring_up_server() {
  bash /root/install-config-module.sh grants >/dev/null 2>&1
  bash /root/install-db1-module.sh grants    >/dev/null 2>&1
  bash /root/start-server.sh >/dev/null 2>&1
  bash /root/imms.sh >/dev/null 2>&1
  sleep 2
}

hdr "bringing the stack up"
bring_up_kb
bring_up_server
echo "  daemons: kb=$(pgrep -cf /usr/local/bin/aimee-kb) server=$(pgrep -cf /usr/local/bin/aimee-server)"
echo "  kb-bus modules:     $(pgrep -cf 'aimee-modules/aimee-module-.* /root/.config/aimee/kb-module-bus.sock')"
echo "  server-bus modules: $(pgrep -cf 'aimee-modules/aimee-module-.* /root/server-module-bus.sock')"
# Mark the log so the detector at the end only counts THIS run.
capstart="$(grep -ac 'result=capability_absent' /root/kb.log 2>/dev/null || echo 0)"

hdr "schema and migration"
run "p6 migration on a populated db" bash /root/test-p6-migration.sh
# That probe restarts the kb to apply the schema, which drops every module on
# its bus. Without this the whole remainder of the suite runs with memory and
# postgres detached, and the passes below would mean less than they appear to.
bring_up_kb

hdr "authority: the account decides"
seed; run "kb, attested person"        bash /root/test-retract.sh
seed; run "server, agent vs person"    bash /root/test-server-retract.sh
seed; run "same body, both transports" bash /root/test-transport-authority.sh "$BEARER"
seed; run "memory.delete"              bash /root/test-memory-delete.sh
seed; run "provenance stamping"        bash /root/test-provenance.sh

hdr "authority: the agent's own words are not the user's"
seed; run "agent query must not retract" bash /root/test-context-block.sh

hdr "authority: gap 2, class rank on a functional relation"
run "drain must not outrank the user" bash /root/test-drain-supersede.sh

hdr "account forms"
# OIDC mints its own token: kb applies a hard age ceiling on iat, so a token
# minted at setup is refused minutes later with the same answer a forged one
# gets.
seed; run "oidc bearer"  bash /root/test-oidc-authority.sh

# The two postures are mutually exclusive and each probe refuses the wrong one:
# with mTLS optional a caller presenting no client certificate is READ-ONLY
# whatever its token says, so the identity-token probe cannot reach the authority
# decision; with mTLS off there is no certificate to be the account.
bash /root/set-mtls-mode.sh optional >/dev/null 2>&1
bring_up_server
seed; run "mtls client certificate"    bash /root/test-mtls-authority.sh "$BEARER"
run "tls failure names its cause"      bash /root/test-tls-failure-cause.sh

bash /root/set-mtls-mode.sh off >/dev/null 2>&1
bring_up_server
seed; run "account over TCP"           bash /root/test-account-tcp-authority.sh "$BEARER"

hdr "read surfaces"
run "entity profile / schema list" bash /root/test-graph-surfaces.sh
run "grant administration path"    bash /root/test-grant-admin.sh

hdr "summary"
# A stage that could not be reached makes a probe green for the wrong reason:
# every consumer reports "no answer" and carries on. Counted across the run so a
# detached module cannot hide behind a clean pass list.
capend="$(grep -ac 'result=capability_absent' /root/kb.log 2>/dev/null || echo 0)"
capnew=$(( capend - capstart ))
if [ "$capnew" -gt 0 ]; then
  echo "  WARNING: $capnew module stage call(s) were refused as capability_absent"
  grep -a 'result=capability_absent' /root/kb.log | tail -3 | sed 's/^/      /'
  echo "      A probe whose stage never ran can still print PASS, so treat the"
  echo "      results above as unproven until this is zero."
  fail=$((fail + 1))
  failed_names="$failed_names module-stages-reachable"
fi
printf '  pass %d   fail %d   skip %d\n' "$pass" "$fail" "$skip"
[ -n "$failed_names" ] && printf '  failed:%s\n' "$failed_names"
[ "$skip" -gt 0 ] && printf '  (%d probe(s) skipped -- see above; nothing is claimed for those)\n' "$skip"
exit $([ "$fail" -eq 0 ] && echo 0 || echo 1)
