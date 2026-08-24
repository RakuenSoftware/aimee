#!/bin/sh
# The whole run: bring the system up, then isolate the store module's blocker.
#
# e2e-252-full.sh leaves the daemons and the fleet running when it succeeds is
# NOT true -- it tears down. So the store probe needs the system standing, and
# this drives both in one pass with the environment shared.
set -u
sh /tmp/e2e-full-keep.sh
echo
echo "############ store module probe ############"
sh /tmp/store-probe.sh
echo
echo "############ teardown ############"
pkill -f aimee-module 2>/dev/null
pkill -f module-supervisor 2>/dev/null
pkill -f aimee-server 2>/dev/null
pkill -f aimee-kb 2>/dev/null
sleep 2
echo "done"
