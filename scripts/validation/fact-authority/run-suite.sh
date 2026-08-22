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
# The count is re-baselined after every bring-up, because the readiness poll
# below IS a module stage call: `aimee status` asks event 11265, so its first
# attempt before the module attaches is itself a capability_absent. Counting the
# probe own warm-up as evidence of a broken run is a false positive of my own
# making, and it fired on the first fresh-container run.
#
# What the detector is for is stages missing WHILE PROBES RUN. Re-baselining
# after bring-up measures exactly that and nothing else.
rebaseline_caps() {
  capstart="$(grep -ac 'result=capability_absent' /root/kb.log 2>/dev/null | head -1)"
  capstart="${capstart:-0}"
}

bring_up_kb() {
  bash /root/install-config-module.sh grants   >/dev/null 2>&1
  bash /root/install-postgres-module.sh grants >/dev/null 2>&1
  bash /root/start-kb.sh >/dev/null 2>&1
  bash /root/smm.sh >/dev/null 2>&1
  bash /root/install-postgres-module.sh >/dev/null 2>&1
  # WAIT for a stage to answer, rather than sleeping a guessed interval. A module
  # process reports "state: RUNNING" as soon as it is spawned, but attaching to
  # the bus lags the daemon accepting calls -- so the first probe after a restart
  # could be served with stages missing, and it still prints PASS because every
  # consumer of an absent stage reports "no answer" and carries on. On a fresh
  # container that race is wider (nothing is warm) and it fired.
  #
  # `aimee status` reporting `store: ok` means event 11265 answered, which is a
  # module stage round trip: the cheapest honest readiness signal available.
  local i
  for i in $(seq 1 30); do
    if AIMEE_HOME=/root AIMEE_API_ENDPOINT=unix:/root/aimee-http.sock \
         /usr/local/bin/aimee status 2>/dev/null | grep -q "store: *ok"; then
      rebaseline_caps
      return 0
    fi
    sleep 2
  done
  echo "  WARNING: no module stage answered within 60s of bringing the kb up;" >&2
  echo "           probes below may run with stages missing." >&2
}

bring_up_server() {
  bash /root/install-config-module.sh grants >/dev/null 2>&1
  bash /root/install-db1-module.sh grants    >/dev/null 2>&1
  bash /root/start-server.sh >/dev/null 2>&1
  bash /root/imms.sh >/dev/null 2>&1
  sleep 2
  rebaseline_caps
}

hdr "bringing the stack up"
bring_up_kb
bring_up_server
echo "  daemons: kb=$(pgrep -cf /usr/local/bin/aimee-kb) server=$(pgrep -cf /usr/local/bin/aimee-server)"
echo "  kb-bus modules:     $(pgrep -cf 'aimee-modules/aimee-module-.* /root/.config/aimee/kb-module-bus.sock')"
echo "  server-bus modules: $(pgrep -cf 'aimee-modules/aimee-module-.* /root/server-module-bus.sock')"
# Mark the log so the detector at the end only counts THIS run.
# grep -c PRINTS 0 and EXITS 1 when there are no matches, so `|| echo 0` appends
# a second line and the arithmetic below sees "0\n0". head -1 keeps one number.
capstart="$(grep -ac 'result=capability_absent' /root/kb.log 2>/dev/null | head -1)"
capstart="${capstart:-0}"

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

hdr "live model stages (needs the chat provider and capture proxy)"
# These were NOT in this runner for its first several passes, and their absence
# is exactly the kind of thing a "13 pass" summary hides: four probes existed and
# none of them ran. They need the server's chat provider pointed at the capture
# proxy, so they SKIP rather than fail when that is not up -- a missing provider
# is a staging fact, not a defect in the code under test.
if curl -s -m 8 -o /dev/null http://127.0.0.1:8798/v1/models 2>/dev/null; then
  run "RETRIEVE via module cycling" bash /root/test-retrieve-live.sh
  run "RERANK via envelope cycling" bash /root/test-rerank-live.sh
  run "chat turn carries the envelope" bash /root/test-chat-memory-stages.sh
else
  skip=$((skip + 3))
  echo "  live model stages                  SKIP"
  echo "      SKIP: the capture proxy is not answering on :8798. Run slp.sh"
  echo "            (which also repoints the provider) to exercise these."
fi

# test-retract-remote.sh is deliberately NOT here: it must run from a peer that
# is not this container, so the host invokes it. Running it from inside would
# make every call loopback and prove the opposite of what it asks.

hdr "read surfaces"
run "entity profile / schema list" bash /root/test-graph-surfaces.sh
run "grant administration path"    bash /root/test-grant-admin.sh

hdr "embedding"
# Last, because it restarts the kb pointed at a stub embedder and leaves that
# pointing in place; nothing after it should depend on the earlier config.
run "EMBED call path" bash /root/test-embed-stage.sh
bring_up_kb

hdr "summary"
# A stage that could not be reached makes a probe green for the wrong reason:
# every consumer reports "no answer" and carries on. Counted across the run so a
# detached module cannot hide behind a clean pass list.
capend="$(grep -ac 'result=capability_absent' /root/kb.log 2>/dev/null | head -1)"
capend="${capend:-0}"
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
