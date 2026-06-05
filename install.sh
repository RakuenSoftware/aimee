#!/usr/bin/env bash
set -euo pipefail

# aimee installer
# Builds the shipped binaries, installs them, and configures hooks.

INSTALL_DIR="$HOME/.local/bin"
AIMEE_BIN="$INSTALL_DIR/aimee"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$SCRIPT_DIR/src"
LOCAL_BIN="$SCRIPT_DIR/aimee"
LOCAL_SERVER="$SCRIPT_DIR/aimee-server"
LOCAL_WEBCHAT="$SCRIPT_DIR/aimee-webchat"
LOCAL_KB="$SCRIPT_DIR/aimee-kb"

# shellcheck source=distro-detect.sh
source "$SCRIPT_DIR/distro-detect.sh"
detect_pkg_manager

# Colors (if terminal supports them)
if [ -t 1 ]; then
    BOLD='\033[1m'
    GREEN='\033[0;32m'
    YELLOW='\033[0;33m'
    RESET='\033[0m'
else
    BOLD='' GREEN='' YELLOW='' RESET=''
fi

info()  { echo -e "${GREEN}>${RESET} $*"; }
warn()  { echo -e "${YELLOW}!${RESET} $*"; }
bold()  { echo -e "${BOLD}$*${RESET}"; }

INTERACTIVE_INSTALL=0
if [ -t 0 ] && [ -t 1 ] && [ -z "${CI:-}" ]; then
    INTERACTIVE_INSTALL=1
fi
NONINTERACTIVE_NOTICE_EMITTED=0

emit_noninteractive_notice() {
    if [ "$NONINTERACTIVE_NOTICE_EMITTED" -eq 0 ]; then
        info "Non-interactive install detected; using defaults and existing config values."
        NONINTERACTIVE_NOTICE_EMITTED=1
    fi
}

prompt_with_default() {
    local prompt="$1"
    local default_value="$2"
    local reply=""

    if [ "$INTERACTIVE_INSTALL" = "1" ]; then
        read -rp "$prompt" reply
    else
        emit_noninteractive_notice
    fi

    printf '%s\n' "${reply:-$default_value}"
}

prompt_secret() {
    local prompt="$1"
    local reply=""

    if [ "$INTERACTIVE_INSTALL" = "1" ]; then
        read -rsp "$prompt" reply
        echo ""
    else
        emit_noninteractive_notice
    fi

    printf '%s\n' "$reply"
}

prompt_yes_no() {
    local prompt="$1"
    local default_value="${2:-N}"
    local reply=""

    if [ "$INTERACTIVE_INSTALL" = "1" ]; then
        read -rp "$prompt" reply
    else
        emit_noninteractive_notice
    fi

    printf '%s\n' "${reply:-$default_value}"
}

# --- Prerequisites ---

# System packages and the PostgreSQL bootstrap need sudo, so they live in
# install-deps.sh. Here we only CHECK the abstract deps (shared list +
# dep_present live in distro-detect.sh) and, if any are missing, point the user
# at install-deps.sh rather than escalating privileges from this script.
missing_deps=()
for dep in "${REQUIRED_DEPS[@]}"; do
    dep_present "$dep" || missing_deps+=("$dep")
done

if [ "${#missing_deps[@]}" -gt 0 ]; then
    echo "Error: missing required dependencies: ${missing_deps[*]}"
    echo
    echo "These need a system package manager (sudo). Install them with:"
    echo "  ./install-deps.sh"
    echo "then re-run ./install.sh."
    echo
    echo "Or install them manually:"
    for dep in "${missing_deps[@]}"; do
        hint=$(pkg_install_hint "$dep")
        [ -n "$hint" ] && echo "  - $dep: $hint"
    done
    exit 1
fi

if ! sqlite3 :memory: "PRAGMA compile_options" 2>/dev/null | grep -q ENABLE_FTS5; then
    echo "Error: system SQLite does not have FTS5 enabled."
    echo "Install a SQLite build with FTS5 support (default on Debian 13+)."
    exit 1
fi

# --- Build (skip if binary is newer than all source) ---

needs_build() {
    local bins=("$LOCAL_BIN" "$LOCAL_SERVER" "$LOCAL_WEBCHAT" "$LOCAL_KB")
    local bin

    for bin in "${bins[@]}"; do
        [ -f "$bin" ] || return 0
    done

    while IFS= read -r -d '' src; do
        for bin in "${bins[@]}"; do
            [ "$src" -nt "$bin" ] && return 0
        done
    done < <(find "$SRC_DIR" -name '*.c' -o -name '*.h' | tr '\n' '\0')

    while IFS= read -r -d '' src; do
        for bin in "${bins[@]}"; do
            [ "$src" -nt "$bin" ] && return 0
        done
    done < <(find "$SCRIPT_DIR/webchat" -name '*.go' -o -name 'go.mod' | tr '\n' '\0')

    for bin in "${bins[@]}"; do
        [ "$SRC_DIR/Makefile" -nt "$bin" ] && return 0
    done

    return 1
}

if needs_build; then
    info "Building shipped aimee artifacts..."
    cd "$SRC_DIR"
    make all server
    cd "$SCRIPT_DIR"
else
    info "Binaries are up to date, skipping build"
fi

# --- Stop running server before overwriting binaries ---

if pgrep -x aimee-server >/dev/null 2>&1; then
    info "Stopping aimee-server before install..."
    _server_pids="$(pgrep -x aimee-server 2>/dev/null || true)"
    pkill -x aimee-server 2>/dev/null || true
    if [ -n "$_server_pids" ]; then
        for _spid in $_server_pids; do
            pkill -TERM -P "$_spid" 2>/dev/null || true
        done
    fi
    # Wait up to 5 seconds for graceful shutdown, then force kill
    for i in 1 2 3 4 5; do
        pgrep -x aimee-server >/dev/null 2>&1 || break
        sleep 1
    done
    if pgrep -x aimee-server >/dev/null 2>&1; then
        warn "aimee-server did not exit gracefully, sending SIGKILL..."
        pkill -9 -x aimee-server 2>/dev/null || true
        sleep 1
    fi
fi

# --- Install binaries ---

mkdir -p "$INSTALL_DIR"
rm -f "$INSTALL_DIR/aimee" "$INSTALL_DIR/aimee-server" \
      "$INSTALL_DIR/aimee-webchat" "$INSTALL_DIR/aimee-kb"
cp "$LOCAL_BIN" "$INSTALL_DIR/aimee"
cp "$LOCAL_SERVER" "$INSTALL_DIR/aimee-server"
cp "$LOCAL_WEBCHAT" "$INSTALL_DIR/aimee-webchat"
cp "$LOCAL_KB" "$INSTALL_DIR/aimee-kb"
chmod +x "$INSTALL_DIR/aimee" "$INSTALL_DIR/aimee-server" \
         "$INSTALL_DIR/aimee-webchat" "$INSTALL_DIR/aimee-kb"

# Remove retired binaries. aimee-worker was folded into aimee-server's
# in-process compute pool; it is not a shipped artifact.
for legacy in "$INSTALL_DIR/aimee-mcp" "$INSTALL_DIR/aimem" "$INSTALL_DIR/aimee-worker"; do
    if [ -f "$legacy" ]; then
        info "Removing retired binary: $legacy"
        rm -f "$legacy"
    fi
done

info "Installed: $INSTALL_DIR/aimee, $INSTALL_DIR/aimee-server, $INSTALL_DIR/aimee-webchat, $INSTALL_DIR/aimee-kb"

# --- Install bundled skills ---

if [ -d "$SCRIPT_DIR/skills" ]; then
    BUNDLED_SKILLS_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/aimee/skills"
    mkdir -p "$BUNDLED_SKILLS_DIR"
    while IFS= read -r -d '' skill_src; do
        skill_name="$(basename "$skill_src")"
        skill_dst="$BUNDLED_SKILLS_DIR/$skill_name"
        if [ ! -e "$skill_dst" ]; then
            cp -R "$skill_src" "$skill_dst"
            info "Installed bundled skill: $skill_name"
        fi
    done < <(find "$SCRIPT_DIR/skills" -mindepth 1 -maxdepth 1 -type d -print0)
fi

# --- aimee-kb: local sidecar vs. remote endpoint ---
#
# Local: run aimee-kb on this host, backed by local PostgreSQL + pgvector
#   (provisioned by bootstrap_postgres in install-deps.sh).
# Remote: point this install at an existing aimee-kb over HTTP; no local sidecar
#   and no Postgres. The choice persists as kb_client_url / kb_client_bearer_token
#   in aimee.yaml (the file aimee-server reads via config_load); the server
#   exports them to AIMEE_KB_API_URL / AIMEE_KB_API_BEARER_TOKEN at startup
#   (every launch path), and the kb unit is only enabled in local mode.
#   AIMEE_KB_MODE is exported for install-deps.sh.
# aimee config lives in aimee.yaml (YAML), not config.json — that is the file
# config_load / aimee-server actually read. These helpers upsert/read/remove a
# top-level scalar key without a YAML library, matching aimee's own unquoted
# config_save emit (cf. db2_url with colons). Column-0-anchored so nested keys
# are never matched.
AIMEE_CONFIG_DIR="$HOME/.config/aimee"
AIMEE_YAML="$AIMEE_CONFIG_DIR/aimee.yaml"

aimee_yaml_unset() { # remove any top-level "<key>:" lines
    [ -s "$AIMEE_YAML" ] || return 0
    grep -vE "^$1:" "$AIMEE_YAML" > "$AIMEE_YAML.tmp" 2>/dev/null && mv "$AIMEE_YAML.tmp" "$AIMEE_YAML"
}
aimee_yaml_set() { # upsert "<key>: <value>"
    mkdir -p "$AIMEE_CONFIG_DIR"
    touch "$AIMEE_YAML"
    aimee_yaml_unset "$1"
    if [ -s "$AIMEE_YAML" ] && [ "$(tail -c1 "$AIMEE_YAML" | wc -l)" -eq 0 ]; then
        echo >> "$AIMEE_YAML"
    fi
    printf '%s: %s\n' "$1" "$2" >> "$AIMEE_YAML"
}
aimee_yaml_get() { # echo top-level scalar value (empty if absent)
    [ -f "$AIMEE_YAML" ] || return 0
    sed -n -E "s/^$1:[[:space:]]*(.*)\$/\1/p" "$AIMEE_YAML" | head -n1
}

KB_MODE="local"
echo ""
bold "Knowledge base (aimee-kb)"
echo "  [1] Local  — run aimee-kb on this host (needs PostgreSQL + pgvector)"
echo "  [2] Remote — point at an existing aimee-kb over HTTP"
echo ""
kb_choice=$(prompt_with_default "  Select [1/2] (default: 1): " "1")
if [ "$kb_choice" = "2" ]; then
    KB_MODE="remote"
    kb_url=$(prompt_with_default "  Remote aimee-kb URL (e.g. https://kb.example:4010): " "")
    kb_token=$(prompt_secret "  Bearer token (optional, blank for none): ")
    aimee_yaml_set kb_client_url "$kb_url"
    if [ -n "$kb_token" ]; then
        aimee_yaml_set kb_client_bearer_token "$kb_token"
    else
        aimee_yaml_unset kb_client_bearer_token
    fi
    info "Configured remote aimee-kb: $kb_url"
else
    # Local mode: clear any stale remote pointer so the local sidecar is used.
    aimee_yaml_unset kb_client_url
    aimee_yaml_unset kb_client_bearer_token
fi
export AIMEE_KB_MODE="$KB_MODE"

# --- Install systemd user units (Linux only) ---
#
# aimee-server's lifecycle is owned by systemd on Linux. The CLI no longer
# auto-spawns the server; it expects a service-managed instance. macOS
# launchd and Windows SCM units land in their own follow-up PRs.
if command -v systemctl >/dev/null 2>&1 && [ -d "$SCRIPT_DIR/systemd/user" ]; then
    USER_UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
    mkdir -p "$USER_UNIT_DIR"

    # The slice carries resource guardrails shared by the services. Install it
    # before daemon-reload so service Slice= references get the current limits.
    #
    # aimee-server and aimee-kb are independent services. The unit for
    # aimee-server orders After= aimee-kb so a fresh boot brings kb up
    # first; aimee-server's fork-and-exec fallback kicks in only when
    # neither systemd nor a remote kb is reachable. The long-term plan
    # (proposal: aimee-kb-network-listener) is to allow kb to run on a
    # separate host shared by many aimee-servers; that work removes the
    # local fallback entirely.
    # aimee-gateway.service (multi-channel ambient presence) is refreshed here
    # too, but not auto-enabled — it needs per-channel config, so the operator
    # enables it explicitly (systemctl --user enable --now aimee-gateway).
    for unit in aimee.slice aimee-kb.service aimee-server.service aimee-gateway.service; do
        src="$SCRIPT_DIR/systemd/user/$unit"
        if [ -f "$src" ]; then
            cp "$src" "$USER_UNIT_DIR/$unit"
            info "systemd user unit copied to $USER_UNIT_DIR/$unit"
        fi
    done

    if systemctl --user daemon-reload 2>/dev/null; then
        info "systemctl --user daemon-reload ok"
        if [ "$KB_MODE" = "remote" ]; then
            # Remote kb: don't run a local sidecar; stop/disable it if a prior
            # local install left it enabled, then bring up just the server.
            systemctl --user disable --now aimee-kb.service 2>/dev/null || true
            if systemctl --user enable --now aimee-server.service 2>/dev/null; then
                info "aimee-server.service enabled and started (remote aimee-kb)"
            else
                warn "systemctl --user enable failed; run: systemctl --user enable --now aimee-server"
            fi
        else
            # Local kb: enable kb FIRST so the server unit's After= ordering can
            # actually find it. enable --now is idempotent.
            if systemctl --user enable --now aimee-kb.service aimee-server.service 2>/dev/null; then
                info "aimee-kb.service + aimee-server.service enabled and started"
            else
                warn "systemctl --user enable failed; run: systemctl --user enable --now aimee-kb aimee-server"
            fi
        fi
    else
        warn "systemctl --user daemon-reload failed; check that user manager is running (loginctl enable-linger \$USER if needed)"
    fi
fi

# --- Install macOS LaunchAgent plists (Darwin only) ---
#
# aimee-server's lifecycle is owned by launchd on macOS. The CLI no longer
# auto-spawns the server; it expects a service-managed instance. This
# mirrors the Linux systemd block above: kb is installed and loaded first
# so the server's connect-on-start finds it, server second.
if [ "$(uname)" = "Darwin" ] && [ -d "$SCRIPT_DIR/service" ]; then
    LA_DIR="$HOME/Library/LaunchAgents"
    mkdir -p "$LA_DIR" "$HOME/Library/Logs/aimee"

    # Remote kb: load only the server agent (no local sidecar).
    mac_labels="com.aimee.kb com.aimee.server"
    [ "$KB_MODE" = "remote" ] && mac_labels="com.aimee.server"
    for label in $mac_labels; do
        src="$SCRIPT_DIR/service/$label.plist"
        dst="$LA_DIR/$label.plist"
        if [ -f "$src" ]; then
            sed "s|__HOME__|$HOME|g" "$src" > "$dst"
            info "LaunchAgent plist installed at $dst"
            launchctl unload "$dst" 2>/dev/null || true
            if launchctl load "$dst" 2>/dev/null; then
                info "$label loaded via launchctl"
            else
                warn "launchctl load $dst failed; run: launchctl load $dst"
            fi
        fi
    done
fi

# --- Install ast-grep (sg) for structural code search ---

SG_BIN="$INSTALL_DIR/sg"
SG_VERSION="0.38.0"

install_ast_grep() {
    local arch
    arch=$(uname -m)
    local sg_arch
    case "$arch" in
        x86_64)  sg_arch="x86_64-unknown-linux-musl" ;;
        aarch64) sg_arch="aarch64-unknown-linux-musl" ;;
        *)
            warn "ast-grep: unsupported architecture '$arch'. Skipping."
            return
            ;;
    esac

    if ! command -v curl >/dev/null 2>&1; then
        warn "curl not found; skipping ast-grep download. Install curl to enable ast_grep_search."
        return
    fi

    local url="https://github.com/ast-grep/ast-grep/releases/download/${SG_VERSION}/sg-${sg_arch}.tar.gz"
    local tmp_dir
    tmp_dir=$(mktemp -d)
    info "Downloading ast-grep ${SG_VERSION}..."
    if curl -fsSL "$url" -o "$tmp_dir/sg.tar.gz" 2>/dev/null; then
        tar -xf "$tmp_dir/sg.tar.gz" -C "$tmp_dir" 2>/dev/null
        if [ -f "$tmp_dir/sg" ]; then
            cp "$tmp_dir/sg" "$SG_BIN"
            chmod +x "$SG_BIN"
            info "Installed ast-grep: $SG_BIN"
        else
            warn "ast-grep archive did not contain 'sg' binary. ast_grep_search tool will be unavailable."
        fi
    else
        warn "Failed to download ast-grep. ast_grep_search tool will report binary not found."
    fi
    rm -rf "$tmp_dir"
}

if [ -f "$SG_BIN" ] && "$SG_BIN" --version >/dev/null 2>&1; then
    info "ast-grep already installed: $SG_BIN"
elif command -v sg >/dev/null 2>&1; then
    info "ast-grep (sg) found in PATH"
else
    install_ast_grep
fi

# Postgres database setup runs in install-deps.sh (it needs sudo). The aimee-kb
# daemon's startup auto-bootstrap (kb_bootstrap_db2_resolve) also retries those
# steps the first time it launches, so a user who skipped install-deps.sh still
# converges once a reachable PostgreSQL exists.

# --- Choose primary AI CLI ---

CONFIG_DIR="$AIMEE_CONFIG_DIR" # key files live here; config persists to aimee.yaml

# Read current provider from aimee.yaml (default: claude)
CURRENT_PROVIDER="$(aimee_yaml_get provider)"
[ -n "$CURRENT_PROVIDER" ] || CURRENT_PROVIDER="claude"

echo ""
bold "Choose your primary AI CLI"
echo "  This is the AI coding tool that launches when you run ${BOLD}aimee${RESET}."
echo ""
echo "  1) Claude   (claude)"
echo "  2) Codex    (codex)"
echo "  3) Gemini   (gemini)"
echo "  4) OpenAI-compatible (any OpenAI-compatible API)"
echo ""

# Map current provider to a number for the default prompt
case "$CURRENT_PROVIDER" in
    claude) DEFAULT_NUM=1 ;;
    codex)  DEFAULT_NUM=2 ;;
    gemini) DEFAULT_NUM=3 ;;
    openai) DEFAULT_NUM=4 ;;
    *)      DEFAULT_NUM=1 ;;
esac

cli_choice=$(prompt_with_default "  Select [1-4] (current: $DEFAULT_NUM): " "$DEFAULT_NUM")

case "$cli_choice" in
    1) CHOSEN_PROVIDER="claude" ;;
    2) CHOSEN_PROVIDER="codex"  ;;
    3) CHOSEN_PROVIDER="gemini" ;;
    4) CHOSEN_PROVIDER="openai" ;;
    *)
        warn "Invalid choice. Keeping current provider: $CURRENT_PROVIDER"
        CHOSEN_PROVIDER="$CURRENT_PROVIDER"
        ;;
esac

# Save provider to aimee.yaml (the file aimee-server reads).
aimee_yaml_set provider "$CHOSEN_PROVIDER"

info "Primary CLI set to: $CHOSEN_PROVIDER"

# For non-openai providers, ask: native CLI or built-in CLI?
USE_BUILTIN=0
if [ "$CHOSEN_PROVIDER" != "openai" ]; then
    echo ""
    bold "How should aimee launch $CHOSEN_PROVIDER?"
    echo "  a) Use the $CHOSEN_PROVIDER CLI directly (requires $CHOSEN_PROVIDER to be installed)"
    echo "  b) Use Aimee's built-in chat (routes through the $CHOSEN_PROVIDER API)"
    echo ""
    cli_mode=$(prompt_with_default "  Select [a/b] (default: a): " "a")

    if [ "$cli_mode" = "b" ] || [ "$cli_mode" = "B" ]; then
        USE_BUILTIN=1
    fi
fi

if [ "$CHOSEN_PROVIDER" != "openai" ] && [ "$USE_BUILTIN" = "0" ]; then
    # Native CLI mode: check that it's installed
    if ! command -v "$CHOSEN_PROVIDER" &>/dev/null; then
        warn "$CHOSEN_PROVIDER is not installed. Install it before launching through aimee."
    fi

    # Ensure use_builtin_cli is false
    aimee_yaml_set use_builtin_cli false
fi

# Built-in CLI configuration (for openai or use_builtin_cli)
if [ "$CHOSEN_PROVIDER" = "openai" ] || [ "$USE_BUILTIN" = "1" ]; then
    # Determine provider-specific defaults
    case "$CHOSEN_PROVIDER" in
        claude)
            DEFAULT_ENDPOINT="https://api.anthropic.com/v1"
            DEFAULT_MODEL="claude-sonnet-4-20250514"
            KEY_ENV_HINT="ANTHROPIC_API_KEY"
            KEY_FILE_NAME="claude.key"
            ;;
        gemini)
            DEFAULT_ENDPOINT="https://generativelanguage.googleapis.com/v1beta/openai"
            DEFAULT_MODEL="gemini-2.5-pro"
            KEY_ENV_HINT="GEMINI_API_KEY"
            KEY_FILE_NAME="gemini.key"
            ;;
        codex)
            DEFAULT_ENDPOINT="https://api.openai.com/v1"
            DEFAULT_MODEL="o3"
            KEY_ENV_HINT="OPENAI_API_KEY"
            KEY_FILE_NAME="openai.key"
            ;;
        *)
            DEFAULT_ENDPOINT="https://api.openai.com/v1"
            DEFAULT_MODEL="gpt-4o"
            KEY_ENV_HINT="OPENAI_API_KEY"
            KEY_FILE_NAME="openai.key"
            ;;
    esac

    echo ""
    bold "Configure ${CHOSEN_PROVIDER} API"

    # Read current values
    CURRENT_ENDPOINT="$DEFAULT_ENDPOINT"
    CURRENT_MODEL="$DEFAULT_MODEL"
    SAVED_ENDPOINT="$(aimee_yaml_get openai_endpoint)"
    SAVED_MODEL="$(aimee_yaml_get openai_model)"
    # Use saved values if they're not the old defaults
    if [ -n "$SAVED_ENDPOINT" ] && [ "$SAVED_ENDPOINT" != "https://api.openai.com/v1" ]; then
        CURRENT_ENDPOINT="$SAVED_ENDPOINT"
    fi
    if [ -n "$SAVED_MODEL" ] && [ "$SAVED_MODEL" != "gpt-4o" ]; then
        CURRENT_MODEL="$SAVED_MODEL"
    fi

    echo ""
    openai_endpoint=$(prompt_with_default "  API endpoint [$CURRENT_ENDPOINT]: " "$CURRENT_ENDPOINT")

    openai_model=$(prompt_with_default "  Model [$CURRENT_MODEL]: " "$CURRENT_MODEL")

    echo ""
    echo "  Enter your API key (leave blank to keep existing or use $KEY_ENV_HINT env var):"
    api_key=$(prompt_secret "  API key: ")

    OPENAI_KEY_CMD=""
    if [ -n "$api_key" ]; then
        KEY_FILE="$CONFIG_DIR/$KEY_FILE_NAME"
        echo -n "$api_key" > "$KEY_FILE"
        chmod 0600 "$KEY_FILE"
        OPENAI_KEY_CMD="cat $KEY_FILE"
        info "API key saved to $KEY_FILE"
    elif [ -f "$CONFIG_DIR/$KEY_FILE_NAME" ]; then
        OPENAI_KEY_CMD="cat $CONFIG_DIR/$KEY_FILE_NAME"
    fi

    aimee_yaml_set use_builtin_cli true
    aimee_yaml_set openai_endpoint "$openai_endpoint"
    aimee_yaml_set openai_model "$openai_model"
    [ -n "$OPENAI_KEY_CMD" ] && aimee_yaml_set openai_key_cmd "$OPENAI_KEY_CMD"
    info "Built-in CLI configured: $openai_model @ $openai_endpoint"
fi

# --- Optional: Local AI delegate ---

echo ""
bold "Set up a local AI delegate? (CPU-only, free)"
echo "  Offloads coding tasks (review, refactor, explain, draft)"
echo "  to a local model. Saves API costs and works offline."
echo ""
local_answer=$(prompt_yes_no "  Set up local delegate? [y/N] " "N")
if [[ "$local_answer" =~ ^[Yy] ]]; then
    AIMEE_BIN="$AIMEE_BIN" bash "$SCRIPT_DIR/add-local-delegate.sh" || warn "Local delegate setup failed (non-fatal)"
fi

# --- Detect and configure AI coding tools ---

# Source configure-hooks.sh so CONFIGURED and REFRESHED are available for the summary below
CONFIGURED=0
REFRESHED=0
# shellcheck source=configure-hooks.sh
source "$SCRIPT_DIR/configure-hooks.sh"

# --- Summary ---

TOTAL_ACTIVE=$((CONFIGURED + REFRESHED))
echo ""
if [ "$TOTAL_ACTIVE" -eq 0 ]; then
    warn "No AI coding tools detected (checked for Claude Code, Claude Desktop, Gemini CLI, Codex CLI, GitHub Copilot, VS Code)."
    warn "Install one of them, then re-run this script."
else
    bold "aimee installed and configured for $TOTAL_ACTIVE tool(s)."
    echo "Start a new session in your AI coding tool to activate."
fi
