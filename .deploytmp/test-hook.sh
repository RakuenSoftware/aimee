export AIMEE_HOME=/var/lib/docker/volumes/aimee_aimee-server-home/_data
# The guard has been registered-but-inert for every measurement in this study
# (the plugin wrote "%s hooks", not "%s hooks pre"). Registration is not
# enforcement -- drive the hook directly with a PreToolUse payload for a shell
# `git diff` and read back what it decides.
echo '--- payload: shell git diff ---'
printf '%s' '{"hook_event_name":"PreToolUse","tool_name":"Bash","tool_input":{"command":"git diff --stat"}}' \
  | timeout 30 /usr/local/bin/aimee hooks pre 2>&1 | head -c 900
echo
echo '--- payload: shell git status ---'
printf '%s' '{"hook_event_name":"PreToolUse","tool_name":"Bash","tool_input":{"command":"git status --short"}}' \
  | timeout 30 /usr/local/bin/aimee hooks pre 2>&1 | head -c 900
echo
echo '--- payload: an unrelated command (must pass through) ---'
printf '%s' '{"hook_event_name":"PreToolUse","tool_name":"Bash","tool_input":{"command":"make -s lint"}}' \
  | timeout 30 /usr/local/bin/aimee hooks pre 2>&1 | head -c 400
echo
echo HOOK_TEST_DONE
