export AIMEE_HOME=/var/lib/docker/volumes/aimee_aimee-server-home/_data
# Both guards live in the same `hooks pre` handler. Confirm the one we disabled
# is off and the one under test is still on, by driving the hook -- not by
# reading the config file back, which only proves the file was written.
echo '--- git redirect (must still DENY) ---'
printf '%s' '{"hook_event_name":"PreToolUse","tool_name":"Bash","tool_input":{"command":"git diff --stat"}}' \
  | timeout 30 /usr/local/bin/aimee hooks pre 2>&1 | head -c 200
echo
echo '--- write outside a session worktree (must now PASS) ---'
printf '%s' '{"hook_event_name":"PreToolUse","tool_name":"Bash","tool_input":{"command":"sed -n 1,40p TICKET.txt"}}' \
  | timeout 30 /usr/local/bin/aimee hooks pre 2>&1 | head -c 300
echo
echo '--- an ordinary build (must PASS) ---'
printf '%s' '{"hook_event_name":"PreToolUse","tool_name":"Bash","tool_input":{"command":"make -s build/obj/tests/unit-test-config"}}' \
  | timeout 30 /usr/local/bin/aimee hooks pre 2>&1 | head -c 300
echo
echo GUARD_VERIFY_DONE
