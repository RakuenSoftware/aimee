export AIMEE_HOME=/var/lib/docker/volumes/aimee_aimee-server-home/_data
# The cell has a session id; the probes above did not. client_session_worktree_ensure
# keys on it, so its absence may be why the probe never reproduced the cell.
export AIMEE_SESSION_ID=019fc900-0000-7000-8000-0000000000ff
# `mcp-serve` does not create a worktree with require_session_worktree:false, yet
# the cell gets one. The other aimee process in a cell is the PreToolUse hook,
# which runs on EVERY tool call. Drive it in a throwaway repo and look.
T=$(mktemp -d); cd "$T"
git init -q .; git config user.email t@t; git config user.name t
echo hello > a.txt; git add a.txt; git commit -qm init
printf '%s' '{"hook_event_name":"PreToolUse","tool_name":"Bash","tool_input":{"command":"sed -n 1,5p a.txt"}}' \
  | timeout 30 /usr/local/bin/aimee hooks pre >/dev/null 2>&1
echo -n "after hooks pre: "
if [ -d "$T/.aimee/worktrees" ]; then ls "$T/.aimee/worktrees"; else echo "no worktree"; fi
printf '%s' '{"hook_event_name":"SessionStart","source":"startup"}' \
  | timeout 30 /usr/local/bin/aimee hooks pre >/dev/null 2>&1
echo -n "after SessionStart: "
if [ -d "$T/.aimee/worktrees" ]; then ls "$T/.aimee/worktrees"; else echo "no worktree"; fi
cd /; rm -rf "$T"
echo HOOK_PROBE_DONE
