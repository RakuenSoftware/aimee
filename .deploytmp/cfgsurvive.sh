set -u
H=/var/lib/docker/volumes/aimee_aimee-server-home/_data
Y="$H/aimee.yaml"
echo -n "before restart: "; grep -E 'require_session_worktree|require_aimee_git' "$Y" | tr '\n' ' '; echo
echo -n "mtime before  : "; stat -c '%y' "$Y"
cd /opt/aimee-src
docker compose -f compose.server-managed.yaml up -d --force-recreate aimee-server >/dev/null 2>&1
sleep 12
echo -n "after restart : "; grep -E 'require_session_worktree|require_aimee_git' "$Y" | tr '\n' ' '; echo
echo -n "mtime after   : "; stat -c '%y' "$Y"
echo CFG_SURVIVE_DONE
