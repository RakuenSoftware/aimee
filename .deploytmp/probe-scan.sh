set -u
export AIMEE_HOME=/var/lib/docker/volumes/aimee_aimee-server-home/_data
BIN="${BIN:-/usr/local/bin/aimee}"
# workspace add was clean; index scan is the other aimee call the harness makes
# during cell setup, and the worktree appears in that same second.
U=$(mktemp -d)/up.git; mkdir -p "$U"; git init -q --bare "$U"
T=$(mktemp -d); cd "$T"
git init -q .; git config user.email t@t; git config user.name t
mkdir -p src; echo 'int f(void){return 1;}' > src/a.c
git add -A; git commit -qm init; git branch -M main
git remote add origin "$U"; git push -q origin main 2>/dev/null; git fetch -q origin 2>/dev/null
chown -R 1000:1000 "$T" 2>/dev/null || true
P="probe$$"
timeout 300 "$BIN" workspace add "$T" --no-scan >/dev/null 2>&1
echo -n "after workspace add: "; [ -d "$T/.aimee/worktrees" ] && ls "$T/.aimee/worktrees" | head -1 || echo none
timeout 600 "$BIN" index scan "$P" "$T" --force >/dev/null 2>&1
echo -n "after index scan   : "; [ -d "$T/.aimee/worktrees" ] && ls "$T/.aimee/worktrees" | head -1 || echo none
cd /; rm -rf "$T" "$(dirname $U)"
echo SCAN_PROBE_DONE
