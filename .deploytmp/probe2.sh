set -u
export AIMEE_HOME=/var/lib/docker/volumes/aimee_aimee-server-home/_data
# The earlier probe used a repo with NO remote, but the cell's worktree branch is
# based on origin/main -- so creation there may have failed for lack of an origin
# and reported "no worktree" for the wrong reason. Give the probe a real origin.
BIN="${BIN:-/usr/local/bin/aimee}"
U=$(mktemp -d)/upstream.git
mkdir -p "$U" && git init -q --bare "$U"
T=$(mktemp -d); cd "$T"
git init -q .; git config user.email t@t; git config user.name t
echo hello > a.txt; git add a.txt; git commit -qm init
git branch -M main
git remote add origin "$U"; git push -q origin main 2>/dev/null
git fetch -q origin 2>/dev/null
echo -n "config says: "; grep -h require_session_worktree "$AIMEE_HOME/aimee.yaml" 2>/dev/null || echo "(unset -> default ON)"
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"probe","version":"1"}}}' \
  | timeout 30 "$BIN" mcp-serve >/dev/null 2>&1
echo -n "$(basename $BIN) worktree: "
if [ -d "$T/.aimee/worktrees" ]; then ls "$T/.aimee/worktrees" | head -2; else echo "none"; fi
cd /; rm -rf "$T" "$(dirname $U)"
