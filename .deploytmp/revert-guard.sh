set -e
H=/var/lib/docker/volumes/aimee_aimee-server-home/_data
Y="$H/aimee.yaml"
# This edit was made on a wrong diagnosis: the agent still works inside
# .aimee/worktrees/<id> in the successful run, so the key never prevented
# worktree creation. The real fix was wiping the dirty cell. Revert it so all
# three boxes carry an identical, unmodified config.
if [ -f "$Y.bak-worktreeguard" ]; then
  mv "$Y.bak-worktreeguard" "$Y"
  chown 1000:1000 "$Y"
fi
grep -c 'require_session_worktree' "$Y" || true
echo REVERTED
