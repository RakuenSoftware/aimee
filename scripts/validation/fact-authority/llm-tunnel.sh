#!/bin/bash
# Bridge the appliance's loopback-only llama-server so the validation container
# can reach it.
#
# The aimee-llm workload on the SmoothNAS box binds 127.0.0.1:8762, so nothing
# off that host can see it -- which is why every external port probe found only
# nginx on 80/443. This forwards it onto this workstation's LAN address, giving
# the container an ordinary http:// endpoint to point SYNTHESIS_ENDPOINT at.
#
# Run on the WORKSTATION (not the container). Ctrl-C or kill to tear down.
set -u
REMOTE="${REMOTE:-admin@192.168.1.254}"
PORT="${PORT:-8762}"
BIND="${BIND:-0.0.0.0}"

exec ssh -N \
  -o BatchMode=yes \
  -o ExitOnForwardFailure=yes \
  -o ServerAliveInterval=30 \
  -L "${BIND}:${PORT}:127.0.0.1:${PORT}" \
  "$REMOTE"
