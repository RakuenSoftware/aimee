#!/bin/sh
# aimee-kb container entrypoint.
#
# Wraps the aimee-kb binary to make the image robust to deployment
# environments whose volume semantics differ from Docker named volumes.
#
# 1. Stack rlimit. The kb's drain/ingest/watch/query worker threads need a
#    64 MB stack; the 8 MB container default overflows and SIGSEGVs (exit 139)
#    on real memory/kb queries. Compose sets `ulimits: stack: 67108864`, but
#    runtimes that don't (e.g. SmoothNAS plugins, plain `docker run`) inherit
#    the small default. The container's hard limit is unlimited, so raise the
#    soft limit here before exec.
#
# 2. Baked config seeding. The default config that selects the embedder/LLM
#    sidecar commands is baked at $AIMEE_HOME/.config/aimee/aimee.yaml. Docker
#    *named* volumes copy image content into a fresh volume, so the baked file
#    survives; *bind* mounts (including SmoothNAS "flat" plugin volumes) shadow
#    the directory with an empty host dir, dropping the config — the kb then
#    falls back to a non-functional builtin embedder (embed_ok=false). Keep the
#    canonical default outside $AIMEE_HOME (at /opt/aimee/defaults) and seed it
#    in if the config is missing, so embeddings work under any volume type.
#
#    The config path is aimee_home()/aimee.yaml; with AIMEE_HOME set,
#    aimee_home() == $AIMEE_HOME verbatim (see src/aimee_home.c), so the file
#    the kb reads is $AIMEE_HOME/aimee.yaml -- NOT $AIMEE_HOME/.config/aimee/
#    (that path only applies when AIMEE_HOME is unset and $HOME/.config is used).
set -e

# 1. Stack rlimit (64 MB == 65536 KiB == 67108864 bytes). Best-effort: some
#    runtimes forbid raising it, in which case the compose ulimit / a host
#    profile is still required.
ulimit -s 65536 2>/dev/null || true

# 2. Seed the baked default config if it is missing (fresh / bind-mounted
#    volume). Never clobber an operator-provided config.
: "${AIMEE_HOME:=/var/lib/aimee}"
cfg="$AIMEE_HOME/aimee.yaml"
default="/opt/aimee/defaults/aimee.yaml"
if [ ! -f "$cfg" ] && [ -f "$default" ]; then
    mkdir -p "$AIMEE_HOME"
    cp "$default" "$cfg"
fi

exec aimee-kb "$@"
