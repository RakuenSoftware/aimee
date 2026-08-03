#!/bin/bash
# Keep the XTX serving ports forwarded, restoring any tunnel that dies.
#
# XTX serving ports are firewalled off-host, so the driver reaches them through
# `ssh -N -L`. Those forwards are established once, at arm start, and nothing
# watches them. When one died two hours into the E2B Q4 10k arm it stayed dead:
# 1,644 rows (16%, every one on shard 0) came back "Connection reset by peer"
# while the llama-server on the far side carried on serving normally. The arm
# was correctly discarded by the errored-row gate, and two hours were gone.
#
# Run this alongside any XTX arm. It re-establishes a forward within ~15s of it
# dropping; the client's retry (see run_llamacpp.py) covers the gap.
#
#   PORTS="8400 8401 8402 8403" nohup setsid bash harness/tunnel_keeper.sh &
set -u
HOST=${HOST:-admin@192.168.1.254}
PORTS=${PORTS:-"8300 8301 8302 8400 8401 8402 8403"}
LOG=${LOG:-results/tunnel_keeper.log}
say() { echo "[$(date -u +%H:%M:%SZ)] $*" >> "$LOG"; }

say "keeper up for: $PORTS -> $HOST"
declare -A down_since
while true; do
  for p in $PORTS; do
    # Is the far side actually answering through the forward?
    if curl -sf --max-time 6 "http://127.0.0.1:$p/health" >/dev/null 2>&1; then
      if [ -n "${down_since[$p]:-}" ]; then
        say "port $p restored"
        unset "down_since[$p]"
      fi
      continue
    fi
    # Nothing listening remotely means the arm simply is not using this port.
    if ! ssh -n -o ConnectTimeout=8 "$HOST" \
         "ss -ltnH \"sport = :$p\" 2>/dev/null | grep -q ." 2>/dev/null; then
      continue
    fi
    # Remote server is up but the forward is not carrying: rebuild it.
    [ -n "${down_since[$p]:-}" ] || { down_since[$p]=1; say "port $p DOWN, remote server alive, rebuilding forward"; }
    pkill -f "ssh -N -L $p:" 2>/dev/null
    sleep 1
    setsid nohup ssh -N -o ExitOnForwardFailure=yes -o ServerAliveInterval=20 \
      -o ServerAliveCountMax=3 -o ConnectTimeout=10 \
      -L "$p:127.0.0.1:$p" "$HOST" >/dev/null 2>&1 </dev/null &
  done
  sleep 15
done
