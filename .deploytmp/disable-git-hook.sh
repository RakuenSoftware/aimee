set -e
H=/var/lib/docker/volumes/aimee_aimee-server-home/_data
Y="$H/aimee.yaml"
# The git redirect is the only thing that changed between r5 (aimee produced real
# graded patches) and now (0 LOC, work stranded in a session worktree). Its own
# deny message documents this opt-out. Turn it off so the batching/investigate
# work can be measured without the confound; the hook interaction is a separate
# bug, recorded, not yet root-caused.
grep -q '^require_aimee_git:' "$Y" \
  && sed -i 's|^require_aimee_git:.*|require_aimee_git: false|' "$Y" \
  || printf '\nrequire_aimee_git: false\n' >> "$Y"
chown 1000:1000 "$Y"
grep -nE 'require_aimee_git|require_session_worktree' "$Y"
echo GIT_HOOK_DISABLED
