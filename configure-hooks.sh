#!/usr/bin/env bash
set -euo pipefail

# aimee hook configurator
# Detects and configures hooks for AI coding tools.
# Called by install.sh and update.sh.

INSTALL_DIR="$HOME/.local/bin"
AIMEE_BIN="$INSTALL_DIR/aimee"
HOME_DIR="$HOME"
CONFIGURED=0
REFRESHED=0

# Redirect-grep hook: install alongside the aimee binary so any machine that
# runs configure-hooks.sh gets the script at a known path.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REDIRECT_GREP_SRC="$SCRIPT_DIR/scripts/hooks/redirect_grep.py"
REDIRECT_SCRIPT="$HOME/.local/share/aimee/hooks/redirect_grep.py"
if [ -f "$REDIRECT_GREP_SRC" ]; then
    mkdir -p "$(dirname "$REDIRECT_SCRIPT")"
    cp "$REDIRECT_GREP_SRC" "$REDIRECT_SCRIPT"
    chmod +x "$REDIRECT_SCRIPT"
fi

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

configure_json_hooks() {
    local name="$1"
    local config_path="$2"
    local pre_event="$3"
    local post_event="$4"
    local session_event="$5"
    local pre_matcher="$6"
    local post_matcher="$7"
    local mcp_path="${8:-$config_path}"
    local client_id="${9:-generic}"

    local settings="{}"
    if [ -f "$config_path" ]; then
        settings=$(cat "$config_path")
    fi

    local mcp_settings="{}"
    if [ "$mcp_path" != "$config_path" ] && [ -f "$mcp_path" ]; then
        mcp_settings=$(cat "$mcp_path")
    fi

    local new_settings
    new_settings=$(AIMEE_SETTINGS_JSON="$settings" AIMEE_MCP_SETTINGS_JSON="$mcp_settings" AIMEE_REDIRECT_SCRIPT="$REDIRECT_SCRIPT" AIMEE_HOOK_CLIENT_ID="$client_id" python3 -c "
import json, os

settings = json.loads(os.environ.get('AIMEE_SETTINGS_JSON', '{}'))
mcp_settings = json.loads(os.environ.get('AIMEE_MCP_SETTINGS_JSON', '{}')) if '$mcp_path' != '$config_path' else settings

hooks = settings.get('hooks', {})
mcp = mcp_settings.get('mcpServers', {})
bin_path = '$AIMEE_BIN'
mcp_bin = bin_path
redirect_script = os.environ.get('AIMEE_REDIRECT_SCRIPT', '')
client_id = os.environ.get('AIMEE_HOOK_CLIENT_ID', 'generic')

def client_env_cmd(command):
    if client_id:
        return f'AIMEE_HOOK_CLIENT={client_id} {command}'
    return command

def pre_hook_cmd():
    cmd = client_env_cmd(bin_path + ' hooks pre')
    if client_id == 'codex':
        return cmd + ' || true'
    return cmd

def add_hook(event, matcher, command, extra_commands=None):
    entries = hooks.get(event, [])
    # Remove existing aimee hooks for this event to avoid duplicates/stale paths
    entries = [e for e in entries
               if not any('aimee' in h.get('command', '')
                          for h in e.get('hooks', []))]
    hook_list = [{'type': 'command', 'command': command}]
    for ec in (extra_commands or []):
        hook_list.append({'type': 'command', 'command': ec})
    entries.append({'matcher': matcher, 'hooks': hook_list})
    hooks[event] = entries

redirect_cmd = None
if redirect_script and os.path.exists(redirect_script):
    if client_id == 'codex':
        redirect_cmd = f'AIMEE_HOOK_CLIENT=codex python3 {redirect_script} || true'
    else:
        redirect_cmd = f'AIMEE_HOOK_CLIENT={client_id} python3 {redirect_script} 2>/dev/null || true'

if '$session_event' != 'NONE':
    add_hook('$session_event', 'startup|resume|compact', bin_path + ' session-start')
if '$pre_event' != 'NONE':
    add_hook('$pre_event', '$pre_matcher', pre_hook_cmd(),
             extra_commands=[redirect_cmd] if redirect_cmd else None)
if '$post_event' != 'NONE':
    add_hook('$post_event', '$post_matcher', client_env_cmd(bin_path + ' hooks post'))

settings['hooks'] = hooks

# Add Aimee MCP server
mcp['aimee'] = {'command': mcp_bin, 'args': ['mcp-serve']}
if '$mcp_path' == '$config_path':
    settings['mcpServers'] = mcp
else:
    mcp_settings['mcpServers'] = mcp
    print('---MCP---')
    print(json.dumps(mcp_settings, indent=2))
    print('---CONFIG---')

print(json.dumps(settings, indent=2))
")

    # Detect whether aimee was already registered before applying the new config
    local was_configured=0
    local check_path="$config_path"
    if [ "$mcp_path" != "$config_path" ] && [ -f "$mcp_path" ]; then
        check_path="$mcp_path"
    fi
    if [ -f "$check_path" ]; then
        if python3 -c "
import json, sys
try:
    with open('$check_path') as f:
        s = json.load(f)
    sys.exit(0 if 'aimee' in s.get('mcpServers', {}) else 1)
except Exception:
    sys.exit(1)
" 2>/dev/null; then
            was_configured=1
        fi
    fi

    mkdir -p "$(dirname "$config_path")"
    if echo "$new_settings" | grep -qF -- "---MCP---"; then
        echo "$new_settings" | sed -n '/---MCP---/,/---CONFIG---/p' | sed '1d;$d' > "$mcp_path"
        echo "$new_settings" | sed -n '/---CONFIG---/,$p' | sed '1d' > "$config_path"
    else
        echo "$new_settings" > "$config_path"
    fi

    if [ "$was_configured" -eq 1 ]; then
        info "Refreshed $name (already configured): $config_path"
        if [ "$mcp_path" != "$config_path" ]; then
            info "Refreshed $name MCP: $mcp_path"
        fi
        REFRESHED=$((REFRESHED + 1))
    else
        info "Configured $name: $config_path"
        if [ "$mcp_path" != "$config_path" ]; then
            info "Configured $name MCP: $mcp_path"
        fi
        CONFIGURED=$((CONFIGURED + 1))
    fi
}

# --- Detect and configure AI coding tools ---

# Claude Code
if [ -d "$HOME_DIR/.claude" ] || command -v claude &>/dev/null; then
    CLAUDE_SETTINGS="$HOME_DIR/.claude/settings.json"
    configure_json_hooks "Claude Code" \
        "$CLAUDE_SETTINGS" \
        "PreToolUse" "PostToolUse" "SessionStart" \
        "Edit|Write|MultiEdit|Bash|Read|Glob|Grep|Task" "Edit|Write|MultiEdit" \
        "$CLAUDE_SETTINGS" "claude"

    # Ensure Gemini is in the auto-allow list if permissions are already set
    python3 -c "
import json
import os
import sys

config_path = '$CLAUDE_SETTINGS'
if not os.path.exists(config_path):
    sys.exit(0)

with open(config_path) as f:
    settings = json.load(f)

if 'permissions' in settings and 'allow' in settings['permissions']:
    allow = settings['permissions']['allow']
    if 'Gemini(gemini:*)' not in allow:
        # Insert after Bash(git:*) or at the end
        try:
            idx = allow.index('Bash(git:*)')
            allow.insert(idx + 1, 'Gemini(gemini:*)')
        except ValueError:
            allow.append('Gemini(gemini:*)')
        
        with open(config_path, 'w') as f:
            json.dump(settings, f, indent=2)
            f.write('\n')
" 2>/dev/null
fi

# Claude Desktop (MCP only, no hooks)
CLAUDE_DESKTOP_CONFIG=""
if [[ "$OSTYPE" == "darwin"* ]]; then
    CLAUDE_DESKTOP_CONFIG="$HOME_DIR/Library/Application Support/Claude/claude_desktop_config.json"
elif [[ "$OSTYPE" == "linux-gnu"* ]]; then
    CLAUDE_DESKTOP_CONFIG="$HOME_DIR/.config/Claude/claude_desktop_config.json"
fi

if [ -n "$CLAUDE_DESKTOP_CONFIG" ] && [ -d "$(dirname "$CLAUDE_DESKTOP_CONFIG")" ]; then
    configure_json_hooks "Claude Desktop" \
        "$CLAUDE_DESKTOP_CONFIG" \
        "NONE" "NONE" "NONE" "" "" \
        "$CLAUDE_DESKTOP_CONFIG" "claude-desktop"
fi

# Gemini CLI
if [ -d "$HOME_DIR/.gemini" ] || command -v gemini &>/dev/null; then
    configure_json_hooks "Gemini CLI" \
        "$HOME_DIR/.gemini/settings.json" \
        "BeforeTool" "AfterTool" "SessionStart" \
        "write_file|replace|shell" "write_file|replace" \
        "$HOME_DIR/.gemini/settings.json" "gemini"
fi

# Codex CLI
if [ -d "$HOME_DIR/.codex" ] || command -v codex &>/dev/null; then
    configure_json_hooks "Codex CLI" \
        "$HOME_DIR/.codex/hooks.json" \
        "PreToolUse" "PostToolUse" "SessionStart" \
        "Bash" "Bash" \
        "$HOME_DIR/.codex/mcp-config.json" "codex"
fi

# GitHub Copilot
if [ -d "$HOME_DIR/.copilot" ] || command -v copilot &>/dev/null; then
    COPILOT_CONFIG="$HOME_DIR/.copilot/config.json"
    # GitHub Copilot CLI uses hooks in config.json and MCP in mcp-config.json
    configure_json_hooks "GitHub Copilot" \
        "$COPILOT_CONFIG" \
        "PreToolUse" "PostToolUse" "SessionStart" \
        "Bash|Edit|Write" "Edit|Write" \
        "$HOME_DIR/.copilot/mcp-config.json" "copilot"
fi

# VS Code (MCP only, no hooks). VS Code reads MCP servers from a user-level
# mcp.json using the "servers" key (NOT the "mcpServers" key the CLI tools use),
# so this needs its own merge rather than configure_json_hooks. Covers stable
# VS Code, Insiders, and VSCodium.
configure_vscode_mcp() {
    local name="$1"
    local mcp_path="$2"

    local existing="{}"
    [ -f "$mcp_path" ] && existing=$(cat "$mcp_path")

    local was_configured=0
    if [ -f "$mcp_path" ] && python3 -c "
import json, sys
try:
    s = json.load(open('$mcp_path'))
    sys.exit(0 if 'aimee' in s.get('servers', {}) else 1)
except Exception:
    sys.exit(1)
" 2>/dev/null; then
        was_configured=1
    fi

    mkdir -p "$(dirname "$mcp_path")"
    AIMEE_VSCODE_MCP_JSON="$existing" python3 -c "
import json, os
s = json.loads(os.environ.get('AIMEE_VSCODE_MCP_JSON', '{}')) or {}
servers = s.get('servers', {})
servers['aimee'] = {'type': 'stdio', 'command': '$AIMEE_BIN', 'args': ['mcp-serve']}
s['servers'] = servers
print(json.dumps(s, indent=2))
" > "$mcp_path"

    if [ "$was_configured" -eq 1 ]; then
        info "Refreshed $name (already configured): $mcp_path"
        REFRESHED=$((REFRESHED + 1))
    else
        info "Configured $name: $mcp_path"
        CONFIGURED=$((CONFIGURED + 1))
    fi
}

if [[ "$OSTYPE" == "darwin"* ]]; then
    VSCODE_USER_BASE="$HOME_DIR/Library/Application Support"
else
    VSCODE_USER_BASE="$HOME_DIR/.config"
fi
# variant label -> User config dir
configure_vscode_variant() {
    local label="$1" dir="$2" cmd="$3"
    if [ -d "$dir" ] || command -v "$cmd" &>/dev/null; then
        configure_vscode_mcp "$label" "$dir/mcp.json"
    fi
}
configure_vscode_variant "VS Code"          "$VSCODE_USER_BASE/Code/User"            code
configure_vscode_variant "VS Code Insiders" "$VSCODE_USER_BASE/Code - Insiders/User" code-insiders
configure_vscode_variant "VSCodium"         "$VSCODE_USER_BASE/VSCodium/User"        codium

# Print a summary only when this script is run directly (not sourced by install.sh).
# install.sh sources this file and prints its own summary using the CONFIGURED/REFRESHED values.
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    TOTAL_ACTIVE=$((CONFIGURED + REFRESHED))
    if [ "$TOTAL_ACTIVE" -eq 0 ]; then
        warn "No supported AI coding tools detected (checked for Claude Code, Gemini CLI, Codex CLI, GitHub Copilot, VS Code)."
    elif [ "$CONFIGURED" -gt 0 ] && [ "$REFRESHED" -eq 0 ]; then
        info "aimee configured for $CONFIGURED new tool(s)."
    elif [ "$REFRESHED" -gt 0 ] && [ "$CONFIGURED" -eq 0 ]; then
        info "aimee hooks refreshed for $REFRESHED tool(s) (already configured)."
    else
        info "aimee configured for $CONFIGURED new tool(s), refreshed $REFRESHED existing tool(s)."
    fi
fi
