#!/usr/bin/env bash
set -euo pipefail

# aimee updater
# Pulls latest source from GitHub and rebuilds shipped artifacts when needed.

# --- Parse flags ---
TESTING=false
FORCE=false
for arg in "$@"; do
    case "$arg" in
        --testing) TESTING=true ;;
        --force|-f) FORCE=true ;;
        *) echo "Unknown flag: $arg"; exit 1 ;;
    esac
done

INSTALL_DIR="$HOME/.local/bin"
AIMEE_BIN="$INSTALL_DIR/aimee"
AIMEE_SERVER_BIN="$INSTALL_DIR/aimee-server"
AIMEE_WEBCHAT_BIN="$INSTALL_DIR/aimee-webchat"
AIMEE_KB_BIN="$INSTALL_DIR/aimee-kb"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$SCRIPT_DIR/src"
LOCAL_BIN="$SCRIPT_DIR/aimee"
LOCAL_SERVER="$SCRIPT_DIR/aimee-server"
LOCAL_WEBCHAT="$SCRIPT_DIR/aimee-webchat"
LOCAL_KB="$SCRIPT_DIR/aimee-kb"
DEFAULT_UPDATE_SOURCE_URL="https://github.com/RakuenSoftware/aimee.git"
DEFAULT_UPDATE_SOURCE_BRANCH="main"

# shellcheck source=distro-detect.sh
source "$SCRIPT_DIR/distro-detect.sh"
detect_pkg_manager

# Colors (if terminal supports them)
if [ -t 1 ]; then
    GREEN='\033[0;32m'
    YELLOW='\033[0;33m'
    RESET='\033[0m'
else
    GREEN='' YELLOW='' RESET=''
fi

info() { echo -e "${GREEN}>${RESET} $*"; }
warn() { echo -e "${YELLOW}!${RESET} $*"; }

tracked_branch_for() {
    local branch="$1"
    local merge_ref

    merge_ref="$(git config --get "branch.$branch.merge" 2>/dev/null || true)"
    if [ -z "$merge_ref" ]; then
        return 1
    fi

    echo "${merge_ref#refs/heads/}"
}

normalize_update_branch() {
    local branch="$1"

    case "$branch" in
        testing)
            echo "$DEFAULT_UPDATE_SOURCE_BRANCH"
            ;;
        *)
            echo "$branch"
            ;;
    esac
}

wait_for_server() {
    local timeout_secs="${1:-5}"
    local elapsed=0

    while [ "$elapsed" -lt "$timeout_secs" ]; do
        if AIMEE_NO_AUTOSTART=1 "$AIMEE_BIN" status >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done

    return 1
}

# --- Pull latest ---

cd "$SCRIPT_DIR"
OLD_HEAD=$(git rev-parse HEAD)
CURRENT_REMOTE_URL="$(git remote get-url origin 2>/dev/null || true)"
CURRENT_BRANCH="$(git rev-parse --abbrev-ref HEAD 2>/dev/null || true)"
CURRENT_TRACKED_BRANCH=""
if [ -z "$CURRENT_REMOTE_URL" ]; then
    CURRENT_REMOTE_URL="$DEFAULT_UPDATE_SOURCE_URL"
fi
if [ -z "$CURRENT_BRANCH" ] || [ "$CURRENT_BRANCH" = "HEAD" ]; then
    CURRENT_BRANCH="$DEFAULT_UPDATE_SOURCE_BRANCH"
else
    CURRENT_TRACKED_BRANCH="$(tracked_branch_for "$CURRENT_BRANCH" || true)"
fi

if [ -n "$CURRENT_TRACKED_BRANCH" ]; then
    CURRENT_BRANCH="$CURRENT_TRACKED_BRANCH"
elif [ "$CURRENT_BRANCH" != "$DEFAULT_UPDATE_SOURCE_BRANCH" ] && [ "$CURRENT_BRANCH" != "testing" ]; then
    warn "Current branch '$CURRENT_BRANCH' has no upstream; using '$DEFAULT_UPDATE_SOURCE_BRANCH' for updates"
    CURRENT_BRANCH="$DEFAULT_UPDATE_SOURCE_BRANCH"
fi

UPDATE_SOURCE_URL="${AIMEE_UPDATE_SOURCE_URL:-$CURRENT_REMOTE_URL}"
UPDATE_SOURCE_BRANCH="${AIMEE_UPDATE_SOURCE_BRANCH:-$CURRENT_BRANCH}"

if $TESTING; then
    warn "--testing is deprecated; using '$DEFAULT_UPDATE_SOURCE_BRANCH'"
    UPDATE_SOURCE_BRANCH="$DEFAULT_UPDATE_SOURCE_BRANCH"
fi

NORMALIZED_UPDATE_SOURCE_BRANCH="$(normalize_update_branch "$UPDATE_SOURCE_BRANCH")"
if [ "$NORMALIZED_UPDATE_SOURCE_BRANCH" != "$UPDATE_SOURCE_BRANCH" ]; then
    warn "'$UPDATE_SOURCE_BRANCH' is deprecated; using '$NORMALIZED_UPDATE_SOURCE_BRANCH' instead"
fi
UPDATE_SOURCE_BRANCH="$NORMALIZED_UPDATE_SOURCE_BRANCH"

# Convert between HTTPS and SSH GitHub URLs
to_ssh_url()   { echo "$1" | sed 's|https://github.com/|git@github.com:|'; }
to_https_url() { echo "$1" | sed 's|git@github.com:|https://github.com/|'; }

fetch_with_fallback() {
    local url="$1" branch="$2"
    local ssh_url https_url
    ssh_url="$(to_ssh_url "$url")"
    https_url="$(to_https_url "$url")"
    if git fetch "$ssh_url" "$branch" 2>/dev/null; then
        return 0
    fi
    warn "SSH fetch failed, trying HTTPS..."
    git fetch "$https_url" "$branch"
}

branch_checked_out_elsewhere() {
    # True if branch $1 is checked out in a git worktree other than this one.
    local branch="$1" here
    here="$(git rev-parse --show-toplevel 2>/dev/null || true)"
    git worktree list --porcelain 2>/dev/null | awk \
        -v want="refs/heads/$branch" -v here="$here" '
        /^worktree /{ wt = substr($0, 10) }
        /^branch /{ if ($2 == want && wt != here) found = 1 }
        END { exit(found ? 0 : 1) }'
}

info "Pulling latest from $UPDATE_SOURCE_URL:$UPDATE_SOURCE_BRANCH..."
fetch_with_fallback "$UPDATE_SOURCE_URL" "$UPDATE_SOURCE_BRANCH"
if branch_checked_out_elsewhere "$UPDATE_SOURCE_BRANCH"; then
    # The branch is checked out in another worktree, so we cannot switch to it
    # here. The build only needs the source tree at the fetched commit, so
    # update via a detached checkout instead of contending for the branch ref.
    warn "Branch '$UPDATE_SOURCE_BRANCH' is checked out in another worktree; using a detached checkout of the fetched commit"
    git checkout --detach FETCH_HEAD
elif git show-ref --verify --quiet "refs/heads/$UPDATE_SOURCE_BRANCH"; then
    git checkout "$UPDATE_SOURCE_BRANCH"
    git merge FETCH_HEAD
else
    git checkout -b "$UPDATE_SOURCE_BRANCH" FETCH_HEAD
fi

NEW_HEAD=$(git rev-parse HEAD)

# --- Check if rebuild is needed ---

needs_build() {
    # aimee-webchat (Go) is optional: only required/buildable when a Go toolchain
    # is present (see `make all`'s ALL_WEBCHAT gate). Without Go the C core still
    # builds, so a missing webchat must not force a perpetual rebuild.
    local bins=("$LOCAL_BIN" "$LOCAL_SERVER" "$LOCAL_KB")
    command -v go >/dev/null 2>&1 && bins+=("$LOCAL_WEBCHAT")
    local bin

    if $FORCE; then
        return 0
    fi

    # Any missing shipped artifact requires a rebuild.
    for bin in "${bins[@]}"; do
        [ -f "$bin" ] || return 0
    done

    # HEAD moved and source files were in the diff
    if [ "$OLD_HEAD" != "$NEW_HEAD" ]; then
        if git diff --name-only "$OLD_HEAD" "$NEW_HEAD" -- src/ | grep -qE '\.(c|h)$|Makefile'; then
            return 0
        fi
    fi

    # Any source file newer than the binary
    while IFS= read -r -d '' src; do
        for bin in "${bins[@]}"; do
            [ "$src" -nt "$bin" ] && return 0
        done
    done < <(find "$SRC_DIR" \( -name '*.c' -o -name '*.h' \) -print0)

    # Makefile itself changed
    for bin in "${bins[@]}"; do
        [ "$SRC_DIR/Makefile" -nt "$bin" ] && return 0
    done

    return 1
}

needs_install() {
    if $FORCE; then
        return 0
    fi

    local bins=("$AIMEE_BIN" "$AIMEE_SERVER_BIN" "$AIMEE_KB_BIN")
    # webchat only counts toward "needs install" when a local build exists.
    [ -f "$LOCAL_WEBCHAT" ] && bins+=("$AIMEE_WEBCHAT_BIN")
    for bin in "${bins[@]}"; do
        [ -f "$bin" ] || return 0
    done

    return 1
}

# universal-ctags powers code indexing. Installing it needs a package manager
# (sudo), which lives in install-deps.sh — update.sh stays unprivileged, so just
# warn if it's missing. Indexing degrades without it; the update still proceeds.
if ! command -v ctags &>/dev/null; then
    warn "universal-ctags not found; code indexing will be limited."
    warn "Install it with ./install-deps.sh (or: $(pkg_install_hint universal-ctags))."
fi

BUILD_NEEDED=false
INSTALL_NEEDED=false
REFRESH_BINARIES=false
if needs_build; then
    BUILD_NEEDED=true
fi
if needs_install; then
    INSTALL_NEEDED=true
fi
if $BUILD_NEEDED || $INSTALL_NEEDED; then
    REFRESH_BINARIES=true
else
    info "Binaries already up to date; refreshing installed support files"
fi

# --- Build ---

if $BUILD_NEEDED; then
    if $FORCE; then
        info "Forcing rebuild and reinstall..."
    else
        info "Source files changed, rebuilding..."
    fi
    cd "$SRC_DIR"
    if $FORCE; then
        make -B all
    else
        make all
    fi
    cd "$SCRIPT_DIR"
elif $REFRESH_BINARIES; then
    info "Local binaries are current; refreshing installed binaries"
else
    info "Skipping binary reinstall"
fi

# Detect a remote aimee-kb: when kb_client_url is configured, this host points
# at a remote kb over HTTP and runs no local sidecar, so all local-kb start
# paths below are skipped (the systemd path already no-ops because the unit is
# left disabled in remote installs).
KB_REMOTE=false
# config_load reads aimee.yaml (YAML), not config.json; match a non-empty
# top-level kb_client_url scalar (column-0 anchored).
KB_CONFIG_FILE="${XDG_CONFIG_HOME:-$HOME/.config}/aimee/aimee.yaml"
if [ -f "$KB_CONFIG_FILE" ] && \
   grep -qE '^kb_client_url:[[:space:]]*[^[:space:]]' "$KB_CONFIG_FILE" 2>/dev/null; then
    KB_REMOTE=true
    info "aimee-kb: remote endpoint configured; skipping local sidecar management"
fi

# --- Stop aimee-kb if running (must happen before install to avoid "Text file busy") ---

KB_WAS_RUNNING=false
if $REFRESH_BINARIES && pgrep -x aimee-kb >/dev/null 2>&1; then
    info "Stopping aimee-kb..."
    pkill -x aimee-kb 2>/dev/null || true
    for i in 1 2 3 4 5; do
        pgrep -x aimee-kb >/dev/null 2>&1 || break
        sleep 1
    done
    if pgrep -x aimee-kb >/dev/null 2>&1; then
        warn "aimee-kb did not exit gracefully, sending SIGKILL..."
        pkill -9 -x aimee-kb 2>/dev/null || true
        sleep 1
    fi
    KB_WAS_RUNNING=true
fi

# --- Stop server if running (must happen before install to avoid "Text file busy") ---

SERVER_WAS_RUNNING=false
if $REFRESH_BINARIES && pgrep -x aimee-server >/dev/null 2>&1; then
    info "Stopping aimee-server..."
    # Capture server PIDs before signalling so we can kill child processes
    # (active claude sessions) that block compute threads in waitpid.
    _server_pids="$(pgrep -x aimee-server 2>/dev/null || true)"
    pkill -x aimee-server 2>/dev/null || true
    # Signal child processes of aimee-server (forked claude sessions) so
    # compute threads unblock from waitpid and the server can exit cleanly.
    if [ -n "$_server_pids" ]; then
        for _spid in $_server_pids; do
            pkill -TERM -P "$_spid" 2>/dev/null || true
        done
    fi
    # Wait up to 30 seconds for graceful shutdown, then force kill.
    # Active claude sessions need up to ~5 s to handle SIGTERM and finish
    # their compute-thread cleanup before the server can exit cleanly.
    for i in $(seq 1 30); do
        pgrep -x aimee-server >/dev/null 2>&1 || break
        sleep 1
    done
    if pgrep -x aimee-server >/dev/null 2>&1; then
        warn "aimee-server did not exit gracefully, sending SIGKILL..."
        pkill -9 -x aimee-server 2>/dev/null || true
        sleep 1
    fi
    SERVER_WAS_RUNNING=true
fi

# --- Clean up retired binaries ---

LEGACY_MCP="$INSTALL_DIR/aimee-mcp"
if [ -f "$LEGACY_MCP" ]; then
   info "Removing legacy aimee-mcp binary..."
   rm -f "$LEGACY_MCP"
fi
for legacy in "$INSTALL_DIR/aimee-worker" "$INSTALL_DIR/aimem"; do
    if [ -f "$legacy" ]; then
        info "Removing retired binary: $legacy"
        rm -f "$legacy"
    fi
done

# --- Install binary ---

if $REFRESH_BINARIES; then
    mkdir -p "$INSTALL_DIR"
    rm -f "$AIMEE_BIN" "$AIMEE_SERVER_BIN" "$AIMEE_WEBCHAT_BIN" "$AIMEE_KB_BIN"
    cp "$LOCAL_BIN" "$AIMEE_BIN"
    cp "$LOCAL_SERVER" "$AIMEE_SERVER_BIN"
    cp "$LOCAL_KB" "$AIMEE_KB_BIN"
    chmod +x "$AIMEE_BIN" "$AIMEE_SERVER_BIN" "$AIMEE_KB_BIN"
    # webchat is optional (built only when Go is available); install if present.
    if [ -f "$LOCAL_WEBCHAT" ]; then
        cp "$LOCAL_WEBCHAT" "$AIMEE_WEBCHAT_BIN"
        chmod +x "$AIMEE_WEBCHAT_BIN"
    fi
fi

# --- Refresh systemd user units (Linux only) ---
#
# aimee-server and aimee-kb are independent services on Linux post-#1660.
# Each gets its own user unit; aimee-server.service orders After=aimee-kb.
# Sync the shared slice and both unit files, daemon-reload, restart whichever
# services are enabled.
# On non-systemd boxes we fall back to fork-and-exec restart paths below.
#
# Every step prints an info line. The previous revision was silent on
# success, which made it impossible to tell from the operator side whether
# this block ran at all — caught us this iteration when an old unit file
# lingered across multiple update.sh runs without the cp ever firing.
SYSTEMD_RESTARTED=false
KB_SYSTEMD_RESTARTED=false
if command -v systemctl >/dev/null 2>&1 && [ -d "$SCRIPT_DIR/systemd/user" ]; then
    USER_UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
    mkdir -p "$USER_UNIT_DIR"

    # aimee-gateway.service is refreshed alongside the core units (not enabled
    # here; the operator enables it once per-channel config exists).
    for unit in aimee.slice aimee-kb.service aimee-server.service aimee-gateway.service; do
        src="$SCRIPT_DIR/systemd/user/$unit"
        dst="$USER_UNIT_DIR/$unit"
        if [ -f "$src" ]; then
            if [ ! -f "$dst" ] || ! cmp -s "$src" "$dst"; then
                cp "$src" "$dst"
                info "refreshed $dst"
            else
                info "$unit unchanged"
            fi
        fi
    done

    if systemctl --user daemon-reload 2>/dev/null; then
        info "systemctl --user daemon-reload ok"

        # Restart kb first so server's After=aimee-kb ordering picks up
        # the new kb binary by the time server's restart fires.
        if systemctl --user is-enabled --quiet aimee-kb.service 2>/dev/null; then
            if systemctl --user restart aimee-kb.service 2>/dev/null; then
                info "aimee-kb restarted via systemd"
                KB_WAS_RUNNING=false
                KB_SYSTEMD_RESTARTED=true
            else
                warn "systemctl --user restart aimee-kb failed"
            fi
        fi

        if systemctl --user is-enabled --quiet aimee-server.service 2>/dev/null; then
            if systemctl --user restart aimee-server.service 2>/dev/null; then
                info "aimee-server restarted via systemd"
                SERVER_WAS_RUNNING=false
                SYSTEMD_RESTARTED=true
            else
                warn "systemctl --user restart aimee-server failed"
            fi
        else
            info "aimee-server.service not enabled; leaving fork-and-exec fallback active"
        fi
    else
        warn "systemctl --user daemon-reload failed (user manager may not be running)"
    fi
fi

# --- Refresh macOS LaunchAgent plists (Darwin only) ---
#
# Mirrors the Linux systemd refresh: re-render plists into LaunchAgents,
# kickstart whichever labels are loaded so the new binary is picked up.
# Clear *_WAS_RUNNING so the fork-and-exec restart further down does not
# double-launch.
if [ "$(uname)" = "Darwin" ] && [ -d "$SCRIPT_DIR/service" ]; then
    LA_DIR="$HOME/Library/LaunchAgents"
    mkdir -p "$LA_DIR" "$HOME/Library/Logs/aimee"

    for label in com.aimee.kb com.aimee.server; do
        src="$SCRIPT_DIR/service/$label.plist"
        dst="$LA_DIR/$label.plist"
        if [ -f "$src" ]; then
            tmp="$dst.new"
            sed "s|__HOME__|$HOME|g" "$src" > "$tmp"
            if [ ! -f "$dst" ] || ! cmp -s "$tmp" "$dst"; then
                mv "$tmp" "$dst"
                info "refreshed $dst"
            else
                rm -f "$tmp"
                info "$label.plist unchanged"
            fi
        fi
    done

    UID_NUM="$(id -u)"
    if launchctl list com.aimee.kb >/dev/null 2>&1; then
        if launchctl kickstart -k "gui/$UID_NUM/com.aimee.kb" 2>/dev/null; then
            info "aimee-kb restarted via launchctl"
            KB_WAS_RUNNING=false
        else
            warn "launchctl kickstart com.aimee.kb failed"
        fi
    fi
    if launchctl list com.aimee.server >/dev/null 2>&1; then
        if launchctl kickstart -k "gui/$UID_NUM/com.aimee.server" 2>/dev/null; then
            info "aimee-server restarted via launchctl"
            SERVER_WAS_RUNNING=false
        else
            warn "launchctl kickstart com.aimee.server failed"
        fi
    fi
fi

if $SERVER_WAS_RUNNING && ! $SYSTEMD_RESTARTED; then
    info "Restarting aimee-server..."
    # aimee-server's worker threads need a 64 MB stack; the systemd unit sets
    # LimitSTACK=67108864 for exactly this reason. This fork-and-exec fallback
    # runs when systemd isn't managing the server, and would otherwise inherit
    # the shell's default soft stack limit (often 8 MB), which overflows and
    # SIGSEGVs the server at startup. Raise it to match the unit before exec.
    ( ulimit -s 65536 2>/dev/null || true; exec "$AIMEE_SERVER_BIN" >/dev/null 2>&1 ) &
    if wait_for_server 5; then
        info "aimee-server restarted"
    else
        warn "aimee-server did not come back within 5 seconds"
    fi
fi

# Local aimee-kb fork-and-exec fallback. Start the sidecar when it is NOT
# currently running — not only when it was running before the update — so a
# down kb recovers on deploy (cold-start). Skipped for remote installs (no
# local sidecar) and when systemd already managed the restart.
if ! $KB_REMOTE && ! $KB_SYSTEMD_RESTARTED && [ -x "$AIMEE_KB_BIN" ] && \
   ! pgrep -x aimee-kb >/dev/null 2>&1; then
    info "Starting aimee-kb..."
    # Match the systemd unit / launchd plist: aimee-kb serves /v1 only and EXITS
    # without a port (kb_api_http_port defaults to 0), so pass --http-port=8741.
    # Also give worker threads a 64 MB stack like the unit's LimitSTACK, or the
    # drain/ingest/watch threads can overflow the default 8 MB and SIGSEGV.
    ( ulimit -s 65536 2>/dev/null || true; exec "$AIMEE_KB_BIN" --http-port=8741 >/dev/null 2>&1 ) &
fi

bash "$SCRIPT_DIR/configure-hooks.sh"

if [ -f "$AIMEE_WEBCHAT_BIN" ]; then
    info "Updated: $AIMEE_BIN, $AIMEE_SERVER_BIN, $AIMEE_WEBCHAT_BIN, $AIMEE_KB_BIN"
else
    info "Updated: $AIMEE_BIN, $AIMEE_SERVER_BIN, $AIMEE_KB_BIN (aimee-webchat skipped: no Go toolchain)"
fi
