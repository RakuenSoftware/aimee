#!/bin/sh
# css-render-ctl.sh: on-demand lifecycle for the aimee css-render sidecar so a
# headless-Chromium container only runs while you are doing CSS-migration work
# (not 24/7). Three patterns — pick per the README "On-demand" section:
#
#   1. Session-scoped (recommended, no privilege change to aimee-kb):
#        css-render-ctl.sh up        # before a migration session
#        ... run `aimee css render-capture` / render-verify ...
#        css-render-ctl.sh down      # after
#      `css_render_command` points at the running sidecar over HTTP (curl).
#
#   2. Lazy-start + idle-stop: as (1) plus a cron that reaps after idle:
#        */5 * * * *  css-render-ctl.sh reap 900   # stop after 15 min idle
#
#   3. Per-render ephemeral (zero idle, needs container-launch access wherever
#      css_render_command runs):
#        css_render_command: ".../css-render-ctl.sh render"   # stdin -> stdout
#
# Runtime-agnostic: set DOCKER (default `docker`) and/or DOCKER_HOST for a
# non-default container runtime (e.g. the smoothnas LXC2Docker socket:
#   DOCKER_HOST=unix:///run/smoothnas-runtime/docker.sock).
set -eu

IMAGE="${CSS_RENDER_IMAGE:-aimee-css-render}"
NAME="${CSS_RENDER_NAME:-aimee-css-render}"
PORT="${CSS_RENDER_PORT:-8780}"
DOCKER="${DOCKER:-docker}"
MARKER="${CSS_RENDER_MARKER:-/tmp/.css-render-last}"

is_running() { $DOCKER ps --format '{{.Names}}' 2>/dev/null | grep -qx "$NAME"; }
exists()     { $DOCKER ps -a --format '{{.Names}}' 2>/dev/null | grep -qx "$NAME"; }

up() {
  if is_running; then echo "css-render: already running"; return 0; fi
  if exists; then $DOCKER start "$NAME" >/dev/null; echo "css-render: started"; return 0; fi
  # Isolated: read-only rootfs, writable /tmp only, no extra caps. Runs UNTRUSTED
  # markup — keep it off internal networks (proposal §8).
  $DOCKER run -d --name "$NAME" --read-only --tmpfs /tmp \
    -e "CSS_RENDER_MARKER=$MARKER" -p "$PORT:8780" "$IMAGE" >/dev/null
  echo "css-render: created on :$PORT"
}

down() { $DOCKER stop "$NAME" >/dev/null 2>&1 || true; echo "css-render: stopped"; }

status() {
  if is_running; then $DOCKER ps --filter "name=^/$NAME$" --format '{{.Names}}  {{.Status}}';
  else echo "css-render: not running"; fi
}

# reap IDLE_SECS: stop the sidecar if no render in the last IDLE_SECS.
reap() {
  idle="${1:?usage: css-render-ctl.sh reap <idle-seconds>}"
  is_running || { echo "css-render: not running"; return 0; }
  last="$($DOCKER exec "$NAME" cat "$MARKER" 2>/dev/null || echo 0)"
  now_ms="$(( $(date +%s) * 1000 ))"
  if [ "$last" = "0" ] || [ "$(( now_ms - last ))" -gt "$(( idle * 1000 ))" ]; then
    echo "css-render: idle > ${idle}s -> stopping"; down
  else
    echo "css-render: active, keeping"
  fi
}

# render: one-shot ephemeral container; pipes stdin ({html,css}) to stdout
# (snapshot). The zero-idle css_render_command target.
render() { exec $DOCKER run --rm -i "$IMAGE" node /opt/css-render/oneshot.js; }

case "${1:-}" in
  up) up ;;
  down) down ;;
  status) status ;;
  reap) shift; reap "${1:-}" ;;
  render) render ;;
  *) echo "usage: $0 {up|down|status|reap <idle-seconds>|render}" >&2; exit 2 ;;
esac
