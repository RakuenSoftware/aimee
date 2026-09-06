#!/bin/sh
# A parallel test may recreate its HOME. Give each binary its own HOME/TMPDIR
# so that cannot remove another test's repositories, sockets, or configuration.
set -u
case_home=$(mktemp -d /tmp/aut.XXXXXX) || exit 1
trap 'rm -rf "$case_home"' EXIT HUP INT TERM
env HOME="$case_home" TMPDIR="$case_home" AIMEE_CONFIG_TEST_HOST_HOME="$case_home" "$@"
