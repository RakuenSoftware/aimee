export AIMEE_HOME=/var/lib/docker/volumes/aimee_aimee-server-home/_data
# The worktree appears in the same second the cell dir is created -- i.e. during
# harness setup, before the agent runs. The harness's aimee calls at that point
# are `workspace add` and `index scan`. Test them directly.
T=$(mktemp -d); cd "$T"
git init -q .; git config user.email t@t; git config user.name t
mkdir -p src; echo 'int main(void){return 0;}' > src/a.c
git add -A; git commit -qm init
echo -n "before:            "; [ -d .aimee/worktrees ] && echo yes || echo no
timeout 120 /usr/local/bin/aimee workspace add "$T" --no-scan >/dev/null 2>&1
echo -n "after workspace add: "; [ -d .aimee/worktrees ] && ls .aimee/worktrees | head -1 || echo no
cd /; rm -rf "$T"
echo WSADD_PROBE_DONE
