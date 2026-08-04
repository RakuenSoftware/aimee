export AIMEE_HOME=/var/lib/docker/volumes/aimee_aimee-server-home/_data
# Does `aimee mcp serve` still relocate the session into .aimee/worktrees now that
# require_session_worktree is false? Drive it in a throwaway git repo and look at
# what appears on disk. Config written back only proves the write.
T=$(mktemp -d)
cd "$T"
git init -q .
git config user.email t@t; git config user.name t
echo hello > a.txt
git add a.txt && git commit -qm init
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"probe","version":"1"}}}' \
  | timeout 25 /usr/local/bin/aimee mcp-serve >/dev/null 2>&1
echo -n "session worktree created: "
if [ -d "$T/.aimee/worktrees" ]; then ls "$T/.aimee/worktrees" | head -3; else echo "NO (good)"; fi
rm -rf "$T"
echo PROBE_DONE
