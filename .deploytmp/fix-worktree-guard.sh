set -e
H=/var/lib/docker/volumes/aimee_aimee-server-home/_data
Y="$H/aimee.yaml"
cp -a "$Y" "$Y.bak-worktreeguard"

# Registering `hooks pre` switched on BOTH guards the hook implements, not just
# the git redirect we wanted to measure. Session-worktree isolation moves the
# agent into .aimee/worktrees/<id>/main INSIDE the cell -- but the code index is
# registered against the cell root, so every index lookup stops resolving and the
# agent silently falls back to shell. Measured: MCP calls 25 -> 3, tool output
# +49%, and nine calls burned hunting for the working directory.
#
# Isolation exists to stop two sessions sharing one checkout from colliding on a
# single git HEAD. A benchmark cell is a disposable one-session checkout, so the
# hazard cannot occur and the cost is total. Turn it off; keep require_aimee_git
# ON, which is the behaviour under test.
grep -q '^require_session_worktree:' "$Y" \
  && sed -i 's|^require_session_worktree:.*|require_session_worktree: false|' "$Y" \
  || printf '\n# benchmark cells are disposable single-session checkouts\nrequire_session_worktree: false\n' >> "$Y"

chown 1000:1000 "$Y"
grep -nE 'require_session_worktree|require_aimee_git' "$Y" || true
echo GUARD_FIXED
