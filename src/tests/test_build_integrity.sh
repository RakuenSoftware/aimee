#!/bin/bash
# Build integrity tests: catch common Makefile and source breakage early.
# Run from the src/ directory.
set -uo pipefail

MODE="${1:-default}"

FAIL=0
pass() { echo "  PASS: $1"; }
fail() { echo "  FAIL: $1"; FAIL=1; }

case "$MODE" in
    default) echo "build-integrity:" ;;
    --build-variants) echo "build-variants:" ;;
    *)
        echo "usage: $0 [--build-variants]" >&2
        exit 2
        ;;
esac

# 1. No duplicate variable assignments in Makefile (catches overwritten vars)
# Skip lines inside else..endif blocks: those are conditional alternatives, not duplicates.
dupes=$(awk '/^else/{skip=1;next}/^endif/{skip=0;next}skip{next}/^[A-Z_]+ =/{print $1}' Makefile | sort | uniq -d)
if [ -z "$dupes" ]; then
    pass "no duplicate Makefile variable assignments"
else
    fail "duplicate Makefile variable assignments: $dupes"
fi

# 2. Every .c in CORE/DATA/AGENT/CMD_SRCS actually exists
for var in CORE_SRCS DATA_SRCS AGENT_SRCS CMD_SRCS CLI_SRCS SERVER_SRCS; do
    files=$(make -p 2>/dev/null | grep "^$var = " | sed "s/^$var = //" | tr ' ' '\n' | grep '\.c$')
    missing=""
    for f in $files; do
        [ -f "$f" ] || missing="$missing $f"
    done
    if [ -z "$missing" ]; then
        pass "$var: all source files exist"
    else
        fail "$var: missing files:$missing"
    fi
done

# 3. Every test source listed in TEST_TARGETS has a corresponding .c file
targets=$(make -p 2>/dev/null | grep "^TEST_TARGETS" | sed "s/^TEST_TARGETS[: ]*= //" | tr ' ' '\n')
missing_tests=""
for t in $targets; do
    # Targets look like "<prefix>/unit-test-foo" where prefix varies with OBJDIR.
    # Drop the prefix, strip "unit-test-", and map hyphens to underscores:
    #   .../unit-test-foo-bar -> tests/test_foo_bar.c
    name=$(basename "$t" | sed 's|^unit-test-||; s|-|_|g')
    src="tests/test_${name}.c"
    if [ ! -f "$src" ]; then
        # Some variant targets intentionally reuse the base test source with
        # different backing objects, e.g. unit-test-working-memory-mock.
        alt="${src%_mock.c}.c"
        [ -f "$alt" ] || missing_tests="$missing_tests $src"
    fi
done
if [ -z "$missing_tests" ]; then
    pass "all TEST_TARGETS have source files"
else
    fail "missing test sources:$missing_tests"
fi

# 4. Rules.mk: TEST_TARGETS continuation lines (detect missing backslash)
# Every non-last line of a multi-line variable must end with backslash
in_targets=0
line_num=0
bad_lines=""
while IFS= read -r line; do
    line_num=$((line_num + 1))
    if echo "$line" | grep -q "^TEST_TARGETS"; then
        in_targets=1
    fi
    if [ "$in_targets" = "1" ]; then
        # If line has content and does NOT end with \ but the next line
        # is indented (continuation), that is a missing backslash
        if echo "$line" | grep -qE '^\s+.*unit-test-' && ! echo "$line" | grep -q '\\$'; then
            in_targets=0  # this should be the last line
        fi
    fi
done < tests/Rules.mk

# 5. Every test target linking config.o must also link platform_random.o
# Join continuation lines, then check each target rule
bad_targets=$(sed ':a; /\\$/N; s/\\\n//; ta' tests/Rules.mk | grep 'unit-test-' | while IFS= read -r rule; do
    target=$(echo "$rule" | cut -d: -f1 | tr -d ' ')
    deps=$(echo "$rule" | cut -d: -f2-)
    if echo "$deps" | grep -q 'config\.o' && \
       ! echo "$deps" | grep -q 'platform_random\.o' && \
       ! echo "$deps" | grep -q 'TEST_CORE_OBJS\|TEST_DATA_OBJS\|CORE_OBJS\|PLATFORM_BASIC_OBJS'; then
        echo "$target"
    fi
done | tr '\n' ' ')
if [ -z "$bad_targets" ]; then
    pass "all test targets with config.o also link platform_random.o"
else
    fail "config.o without platform_random.o in: $bad_targets"
fi

# 6. Every .PHONY target has a corresponding rule (catches missing targets)
phony_targets=$(grep '^\.PHONY:' Makefile tests/Rules.mk 2>/dev/null | \
    sed 's/^.*\.PHONY://' | tr ' ' '\n' | sort -u | grep -v '^$')
missing_rules=""
for target in $phony_targets; do
    # make -n (dry run) exits non-zero if the target has no rule
    if ! make -n "$target" >/dev/null 2>&1; then
        missing_rules="$missing_rules $target"
    fi
done
if [ -z "$missing_rules" ]; then
    pass "all .PHONY targets have rules"
else
    fail ".PHONY targets with no rule:$missing_rules"
fi

# 7. Scripts don't reference non-existent make targets
# Only match lines where 'make' is the command (start of line or after && / ; / |)
bad_script_targets=""
for script in ../update.sh ../install.sh ../setup.sh; do
    [ -f "$script" ] || continue
    # Extract lines where make is invoked as a command, then pull targets
    targets=$(grep -E '(^|[;&|]\s*)make\s' "$script" 2>/dev/null \
        | sed 's/.*make //' \
        | tr ' ' '\n' \
        | grep -vE '^-|^\$|^$|^>|^2>' \
        || true)
    for target in $targets; do
        if ! make -n "$target" >/dev/null 2>&1; then
            bad_script_targets="$bad_script_targets $script:$target"
        fi
    done
done
if [ -z "$bad_script_targets" ]; then
    pass "scripts reference only valid make targets"
else
    fail "scripts reference missing make targets:$bad_script_targets"
fi

# 7a. PreToolUse grep redirect hook must keep targeted file inspection unblocked.
if python3 ../scripts/test-redirect-grep-hook.py >/dev/null 2>&1; then
    pass "redirect grep hook classifier"
else
    fail "redirect grep hook classifier regression"
fi

# 7a2. Claude-style clients can report non-zero PreToolUse exits as hook
# failures, so Aimee guardrail denials must be structured hook output.
if grep -q 'permissionDecision", "deny"' ../src/cli_main.c &&
   grep -q 'permissionDecision", "deny"' ../src/cmd_hooks.c &&
   grep -q 'cli_hook_client_uses_pretool_json' ../src/cli_main.c &&
   grep -q 'hook_client_uses_pretool_json()) ? 0 : rc' ../src/cmd_hooks.c; then
    pass "PreToolUse guardrail denials are structured"
else
    fail "PreToolUse guardrail denial regression"
fi

hook_payload=$(printf '{"tool_name":"spawn_agent","tool_input":{"prompt":"x"},"cwd":"%s"}' "$(pwd)")
set +e
hook_out=$(printf '%s' "$hook_payload" | AIMEE_HOOK_CLIENT=claude ../aimee hooks pre 2>/dev/null)
hook_rc=$?
set -e
if [ "$hook_rc" -eq 0 ] &&
   echo "$hook_out" | grep -q '"hookEventName":"PreToolUse"' &&
   echo "$hook_out" | grep -q '"permissionDecision":"deny"'; then
    pass "PreToolUse denials exit cleanly for Claude-style hooks"
else
    fail "PreToolUse denial hook output regression"
fi

check_updated_input_gate() {
    local file="$1"
    local gate="$2"
    awk -v gate="$gate" '
        index($0, gate) { seen_gate=1 }
        index($0, "cJSON_AddItemToObject(hook_out, \"updatedInput\"") {
            if (!seen_gate) exit 1
            seen_emit=1
        }
        END { exit (seen_gate && seen_emit) ? 0 : 1 }
    ' "$file"
}

if check_updated_input_gate ../src/cmd_hooks.c 'hook_client_supports_updated_input()' &&
   check_updated_input_gate ../src/cli_main.c 'cli_hook_client_supports_updated_input()' &&
   grep -q 'hook_client_supports_updated_input' ../src/cmd_hooks.c &&
   grep -q 'cli_hook_client_supports_updated_input' ../src/cli_main.c &&
   grep -q 'strcmp(client, "claude") == 0' ../src/cmd_hooks.c &&
   grep -q 'strcmp(client, "claude") == 0' ../src/cli_main.c &&
   grep -q 'emit_pretool_rewrite_unsupported_json' ../src/cmd_hooks.c &&
   grep -q 'emit_pretool_rewrite_unsupported_json' ../src/cli_main.c; then
    pass "PreToolUse updatedInput is gated to supported clients in all hook entry paths"
else
    fail "PreToolUse updatedInput client gating regression"
fi

if grep -q "if client_id == 'codex'" ../configure-hooks.sh &&
   grep -q "return cmd + ' || true'" ../configure-hooks.sh; then
    pass "Codex PreToolUse hook is advisory"
else
    fail "Codex PreToolUse hook may surface failed-hook noise"
fi

# 7b. Installer non-interactive prompts stay wrapped behind helper functions
if ./tests/test_install_noninteractive.sh >/dev/null 2>&1; then
    pass "install.sh non-interactive prompts are centralized"
else
    fail "install.sh non-interactive prompt regression"
fi

# 7c. Install and update paths must refresh the same shipped systemd user units.
missing_systemd_units=""
for unit_path in ../systemd/user/*; do
    [ -f "$unit_path" ] || continue
    unit=$(basename "$unit_path")
    for script in ../install.sh ../update.sh; do
        if ! grep -q "$unit" "$script"; then
            missing_systemd_units="$missing_systemd_units $script:$unit"
        fi
    done
done
if [ -z "$missing_systemd_units" ]; then
    pass "install/update scripts refresh all systemd user units"
else
    fail "install/update scripts miss systemd user units:$missing_systemd_units"
fi

# 7c2. update.sh must refresh hooks/support files even when binaries are current.
if awk '
    /configure-hooks\.sh/ { hook_seen = 1 }
    !hook_seen && /Already up to date|Binaries already up to date/ { current_msg = 1 }
    !hook_seen && current_msg && /^[[:space:]]*exit[[:space:]]+0([[:space:]]|$)/ { bad = 1 }
    END { exit bad ? 1 : 0 }
' ../update.sh; then
    pass "update.sh refreshes support files when binaries are current"
else
    fail "update.sh exits before refreshing support files"
fi

# 7d. Bootstrap scripts must not call hidden legacy client commands. The thin
# client only exposes routed/local commands, so install/update/setup should not
# depend on unadvertised init/setup paths.
hidden_bootstrap_calls=$(grep -REn \
    'aimee[[:space:]]+(init|setup)\b|init --quiet|Database initialized|memory stats' \
    ../install.sh ../update.sh ../setup.sh 2>/dev/null || true)
if [ -z "$hidden_bootstrap_calls" ]; then
    pass "bootstrap scripts avoid hidden client init/setup commands"
else
    fail "bootstrap scripts call hidden client commands:$hidden_bootstrap_calls"
fi

# 7e. CMake shipped targets must mirror the Makefile artifact boundaries. The
# legacy helper libraries intentionally still exist for tests, but DB-free
# client/webchat and DB1-only server targets must not link them transitively.
cmake_file="../CMakeLists.txt"
cmake_target_links() {
    awk -v target="$1" '
        $0 ~ "target_link_libraries\\(" target "[ \t)]" {
            in_block=1
        }
        in_block {
            print
        }
        in_block && /\)/ {
            in_block=0
        }
    ' "$cmake_file"
}
cmake_client_links=$(cmake_target_links aimee)
cmake_webchat_links=$(cmake_target_links aimee-webchat)
cmake_server_links=$(cmake_target_links aimee-server)
cmake_boundary_failures=""
for target_block in client webchat; do
    block_var="cmake_${target_block}_links"
    block="${!block_var}"
    if echo "$block" | grep -Eq 'aimee-(cmd|git|agent|data|core)|SQLite::SQLite3|LIBPQ|libpq'; then
        cmake_boundary_failures="$cmake_boundary_failures aimee-$target_block"
    fi
done
if echo "$cmake_server_links" | grep -Eq 'aimee-(cmd|git|agent|data|core)|LIBPQ|libpq'; then
    cmake_boundary_failures="$cmake_boundary_failures aimee-server"
fi
if [ -z "$cmake_boundary_failures" ]; then
    pass "CMake shipped targets avoid legacy DB-bearing static libraries"
else
    fail "CMake shipped target boundary regressions:$cmake_boundary_failures"
fi

# 7f. Makefile shipped targets must not keep DB2/libpq implementation objects
# in core, and KB's DB2 objects must compile with the shipped KB profile.
makefile_file="Makefile"
make_var_block() {
    awk -v var="$1" '
        $0 ~ "^" var "[[:space:]]*=" {
            in_block=1
        }
        in_block {
            print
        }
        in_block && $0 !~ /\\[[:space:]]*$/ {
            exit
        }
    ' "$makefile_file"
}
make_core_srcs=$(make_var_block CORE_SRCS)
make_server_data_objs=$(make_var_block SERVER_DATA_OBJS)
make_kb_target=$(grep -F '$(KB):' "$makefile_file" || true)
make_kb_compile_rule=$(grep -A2 -F '$(OBJDIR)/kb/%.o:' "$makefile_file" || true)
make_boundary_failures=""
if echo "$make_core_srcs" | grep -Fq 'db2/db_postgres.c'; then
    make_boundary_failures="$make_boundary_failures core-has-db2-postgres"
fi
if echo "$make_server_data_objs" | grep -Fq '$(DATA_OBJS)'; then
    make_boundary_failures="$make_boundary_failures server-links-generic-data-objs"
fi
if ! echo "$make_server_data_objs" | grep -Fq '$(OBJDIR)/server/'; then
    make_boundary_failures="$make_boundary_failures server-data-not-db2-disabled"
fi
if echo "$make_kb_target" | grep -Fq '$(DB2_OBJS)'; then
    make_boundary_failures="$make_boundary_failures kb-links-generic-db2-objs"
fi
if ! echo "$make_kb_target" | grep -Fq '$(KB_DB2_PG_OBJS)'; then
    make_boundary_failures="$make_boundary_failures kb-missing-kb-db2-postgres-objs"
fi
if ! echo "$make_kb_target" | grep -Fq '$(KB_DB2_OBJS)'; then
    make_boundary_failures="$make_boundary_failures kb-missing-kb-db2-objs"
fi
if ! echo "$make_kb_compile_rule" | grep -Fq 'AIMEE_DISABLE_DB2_SQLITE_SHIM'; then
    make_boundary_failures="$make_boundary_failures kb-db2-sqlite-shim-enabled"
fi
if [ -z "$make_boundary_failures" ]; then
    pass "Makefile DB objects are target-owned and shim-disabled"
else
    fail "Makefile DB boundary regressions:$make_boundary_failures"
fi

# 7g. The KB service split must keep explicit module-boundary directories and
# container packaging for the headless aimee-kb deployment shape.
split_failures=""
for d in kb server shared; do
    [ -d "$d" ] || split_failures="$split_failures missing-src-$d"
    [ -f "$d/README.md" ] || split_failures="$split_failures missing-src-$d-readme"
    find "$d" -maxdepth 1 -name '*.h' | grep -q . ||
        split_failures="$split_failures missing-src-$d-header"
done
[ -d kb/http ] || split_failures="$split_failures missing-src-kb-http"
[ -f kb/http/README.md ] || split_failures="$split_failures missing-src-kb-http-readme"
find kb/http -maxdepth 1 -name '*.h' | grep -q . ||
    split_failures="$split_failures missing-src-kb-http-header"
if [ -f ../Dockerfile ]; then
    if ! grep -Fq 'make -C src ../aimee-kb' ../Dockerfile; then
        split_failures="$split_failures dockerfile-not-building-aimee-kb"
    fi
    if grep -Eq 'aimee-server|DB1|db1/' ../Dockerfile; then
        split_failures="$split_failures dockerfile-links-server-or-db1"
    fi
else
    split_failures="$split_failures missing-dockerfile"
fi
if [ -f ../compose.yaml ]; then
    if ! grep -Eq '^[[:space:]]+aimee-kb:' ../compose.yaml; then
        split_failures="$split_failures compose-missing-aimee-kb-service"
    fi
    if ! grep -Eq '^[[:space:]]+postgres:' ../compose.yaml; then
        split_failures="$split_failures compose-missing-postgres-service"
    fi
    if ! grep -Eq 'AIMEE_DB2_URL[=:]' ../compose.yaml; then
        split_failures="$split_failures compose-missing-db2-url"
    fi
else
    split_failures="$split_failures missing-compose-yaml"
fi
if [ -z "$split_failures" ]; then
    pass "aimee-kb split module directories and container packaging exist"
else
    fail "aimee-kb split packaging regressions:$split_failures"
fi

# 7h. The retired Codex remote frontend bridge must stay removed.
codex_frontend_refs=$(find . ../CMakeLists.txt -type f \
    \( -name '*.c' -o -name '*.h' -o -name '*.inc' -o -name 'Makefile' -o -name 'CMakeLists.txt' \) \
    ! -path './tests/test_build_integrity.sh' \
    -print0 | xargs -0 grep -En \
    'cli_tui_codex|codex_exec_tui|codex_ui_|AIMEE_CODEX_(FRONTEND|NATIVE)_BIN|codex-frontend|codex-branded' \
    2>/dev/null || true)
if [ -z "$codex_frontend_refs" ]; then
    pass "Retired Codex chat frontend bridge is absent"
else
    fail "Retired Codex chat frontend bridge references remain:$codex_frontend_refs"
fi
if grep -q 'opencode_exec_tui' ./cli_tui.c ./cli_tui_opencode_v2.inc 2>/dev/null; then
    pass "OpenCode chat frontend adapter is wired"
else
    fail "OpenCode chat frontend adapter is missing"
fi
if grep -q 'opencode_v2_extract_prompt' ./cli_tui_opencode_v2.inc &&
   grep -q 'cJSON_GetObjectItemCaseSensitive(root, "prompt")' ./cli_tui_opencode_v2.inc &&
   grep -q 'cJSON_GetObjectItemCaseSensitive(prompt, "text")' ./cli_tui_opencode_v2.inc; then
    pass "OpenCode v2 prompt text is extracted"
else
    fail "OpenCode v2 prompt.text extraction is missing"
fi
if grep -q 'opencode_v2_prompt_job_t' ./cli_tui_opencode_v2.inc &&
   grep -q 'opencode_v2_prompt_job_main' ./cli_tui_opencode_v2.inc &&
   grep -q 'response_mode == 2' ./cli_tui_opencode_v2.inc &&
   grep -q 'pthread_create(&tid, NULL, opencode_v2_prompt_job_main, worker)' ./cli_tui_opencode_v2.inc &&
   grep -q 'while ((b->busy || opencode_v2_has_prior_unstarted_turn_locked(b, turn)) && !b->closing)' ./cli_tui_opencode_v2.inc; then
    pass "OpenCode async prompt acknowledgements are queued behind active turns"
else
    fail "OpenCode async prompt acknowledgement queue is missing"
fi
if grep -q 'opencode_v2_queue_count_locked' ./cli_tui_opencode_v2.inc &&
   grep -q 'opencode_v2_publish_user_turn_locked' ./cli_tui_opencode_v2.inc &&
   grep -q 'opencode_v2_create_turn_locked' ./cli_tui_opencode_v2.inc &&
   grep -q 'message_id' ./cli_tui_opencode_v2.inc &&
   grep -q 'turn->user_published = 1' ./cli_tui_opencode_v2.inc &&
   grep -q 'turn->prompt_published = 1' ./cli_tui_opencode_v2.inc &&
   grep -q 'opencode_v2_publish_user_turn_locked(b, turn);' ./cli_tui_opencode_v2.inc &&
   grep -q '"idle"' ./cli_tui_opencode_v2.inc &&
   grep -q 'cJSON_AddNumberToObject(status, "queued", opencode_v2_queue_count_locked(b))' ./cli_tui_opencode_v2.inc; then
    pass "OpenCode queued prompts render before their turn starts"
else
    fail "OpenCode queued prompt rendering is missing"
fi
if ! grep -q 'queued:%d' ./cli_tui_legacy_aimee_fullscreen.inc 2>/dev/null &&
   ! grep -q 'Queued input\|Replaced queued input' ./cli_tui_legacy_aimee_fullscreen.inc 2>/dev/null; then
    pass "Legacy TUI does not advertise queued message state"
else
    fail "Legacy TUI still advertises queued message state"
fi
if grep -q 'OpenCode TUI exited during startup; falling back to native TUI' ./cli_tui_opencode_v2.inc 2>/dev/null \
   && ! grep -q 'if (forced || default_launch)' ./cli_tui_opencode_v2.inc 2>/dev/null; then
    pass "OpenCode auto frontend falls back to native TUI"
else
    fail "OpenCode auto frontend fallback is missing"
fi
if grep -q 'opencode_v2_ascending_id_locked' ./cli_tui_opencode_v2.inc 2>/dev/null &&
   grep -q 'opencode_v2_extract_message_id' ./cli_tui_opencode_v2.inc 2>/dev/null &&
   ! grep -q 'opencode_v2_hash_id(turn->user_id' ./cli_tui_opencode_v2.inc 2>/dev/null; then
    pass "OpenCode v2 chat message IDs preserve TUI queue ordering"
else
    fail "OpenCode v2 chat message IDs must be ascending and preserve submitted messageID"
fi
if grep -q '"message.updated"' ./cli_tui_opencode_v2.inc 2>/dev/null && \
   grep -q '"message.part.updated"' ./cli_tui_opencode_v2.inc 2>/dev/null && \
   grep -q '"session.next.text.delta"' ./cli_tui_opencode_v2.inc 2>/dev/null; then
    pass "OpenCode chat frontend publishes message and text events"
else
    fail "OpenCode chat frontend is missing response-rendering events"
fi
if grep -q 'opencode_v2_legacy_message_props_locked' ./cli_tui_opencode_v2.inc 2>/dev/null &&
   grep -q 'opencode_v2_legacy_part_props_locked' ./cli_tui_opencode_v2.inc 2>/dev/null &&
   grep -q 'opencode_v2_legacy_delta_props_locked' ./cli_tui_opencode_v2.inc 2>/dev/null &&
   grep -q '"message.updated"' ./cli_tui_opencode_v2.inc 2>/dev/null &&
   grep -q '"message.part.updated"' ./cli_tui_opencode_v2.inc 2>/dev/null &&
   grep -q '"message.part.delta"' ./cli_tui_opencode_v2.inc 2>/dev/null; then
    pass "OpenCode v2 live path publishes rendered message events"
else
    fail "OpenCode v2 live path must publish message.updated/part events for the TUI"
fi
if grep -q 'opencode_v2_global_event_body' ./cli_tui_opencode_v2.inc 2>/dev/null &&
   grep -q 'cJSON_AddStringToObject(root, "project", "aimee")' ./cli_tui_opencode_v2.inc 2>/dev/null &&
   grep -q 'cJSON_AddItemToObject(root, "payload", payload)' ./cli_tui_opencode_v2.inc 2>/dev/null &&
   grep -q 'opencode_v2_stream_events(b, fd, 0)' ./cli_tui_opencode_v2.inc 2>/dev/null &&
   grep -q 'opencode_v2_stream_events(b, fd, 1)' ./cli_tui_opencode_v2.inc 2>/dev/null; then
    pass "OpenCode v2 global events use TUI GlobalEvent envelope"
else
    fail "OpenCode v2 /global/event must wrap events for the TUI"
fi
if grep -q 'opencode_v2_prompt_props_locked' ./cli_tui_opencode_v2.inc 2>/dev/null &&
   grep -q 'cJSON_AddItemToObject(prompt, "files", cJSON_CreateArray())' ./cli_tui_opencode_v2.inc 2>/dev/null &&
   grep -q 'cJSON_AddItemToObject(prompt, "agents", cJSON_CreateArray())' ./cli_tui_opencode_v2.inc 2>/dev/null &&
   grep -q 'cJSON_AddItemToObject(prompt, "references", cJSON_CreateArray())' ./cli_tui_opencode_v2.inc 2>/dev/null &&
   ! grep -A2 '"session.next.prompted"' ./cli_tui_opencode_v2.inc 2>/dev/null | grep -q 'opencode_v2_text_props_locked'; then
    pass "OpenCode v2 prompted event carries prompt object"
else
    fail "OpenCode v2 prompted event must carry properties.prompt for TUI rendering"
fi
if grep -q '"aggregateID"' ./cli_tui_opencode_v2.inc 2>/dev/null &&
   ! grep -q '"aggregate_id"' ./cli_tui_opencode_v2.inc 2>/dev/null; then
    pass "OpenCode v2 sync history uses aggregateID"
else
    fail "OpenCode v2 sync history must use aggregateID"
fi

route_drift=""
for platform_client in posix/cli_client.c windows/cli_client.c; do
    if ! grep -q '#include "../cli_rpc_routes.inc"' "$platform_client"; then
        route_drift="$route_drift $platform_client:missing-shared-routes"
    fi
    if grep -q 'rpc_routes\[\]' "$platform_client"; then
        route_drift="$route_drift $platform_client:local-route-table"
    fi
done
if [ -z "$route_drift" ]; then
    pass "platform clients share one RPC route table"
else
    fail "platform client RPC route drift:$route_drift"
fi

# 10. Source file line-count policy (replaces per-file cap allowlist):
#   <= 1000 lines : ideal
#   > 1500 lines  : warning — acceptable but should be addressed
#   > 2000 lines  : error   — must fix before merge
#
# LINE_EXEMPT: files permitted to exceed the hard limit. Consolidated
# subsystems (memory_logic.c, memory_advanced.c per memory-consolidation
# proposal) intentionally exceed 2000 lines.
WARN_LINES=1500
ERROR_LINES=2000
LINE_EXEMPT="memory_logic.c memory_advanced.c"
oversized=""
warned=""
for f in *.c posix/*.c linux/*.c mac/*.c windows/*.c tests/*.c; do
    [ -f "$f" ] || continue
    base=$(basename "$f")
    skip=0
    for e in $LINE_EXEMPT; do
        if [ "$base" = "$e" ]; then skip=1; break; fi
    done
    [ "$skip" = "1" ] && continue
    lines=$(wc -l < "$f")
    if [ "$lines" -gt "$ERROR_LINES" ]; then
        oversized="$oversized $f($lines>$ERROR_LINES)"
    elif [ "$lines" -gt "$WARN_LINES" ]; then
        warned="$warned $f($lines)"
    fi
done
if [ -n "$warned" ]; then
    echo "  NOTE: files over $WARN_LINES lines (aim to reduce):$warned"
fi
if [ -z "$oversized" ]; then
    pass "all source files within $ERROR_LINES-line hard limit"
else
    fail "source files exceed $ERROR_LINES-line hard limit:$oversized"
fi

# 11. Layer boundary enforcement: lower layers must not include higher-layer headers.
# Architecture: Layer 0 (core) -> Layer 1 (data) -> Layer 2 (agent) -> Layer 3 (cmd/UI)
L1_HDRS="memory\.h|index\.h|extractors_extra\.h|rules\.h|tasks\.h|feedback\.h|guardrails\.h|worktree\.h|branch_ownership\.h|workspace\.h|working_memory\.h|agent_config\.h|trace_analysis\.h"
L2_HDRS="agent\.h|agent_protocol\.h|agent_exec\.h|agent_types\.h|agent_tools\.h|agent_eval\.h|agent_plan\.h|agent_coord\.h|agent_jobs\.h|agent_tunnel\.h|http_retry\.h|failover\.h"
L3_HDRS="commands\.h|dashboard\.h|cmd_branch\.h"

# Existing violations tracked for reduction (file:header).
# Sanctioned cross-layer dependencies: the verify gate (git_verify_ops.c, layer 0)
# loads the guardrails session_state_t to scope verification to the current
# project/session (#24). session_state_t is defined in guardrails.h, so reading it
# here is an intentional, reviewed dependency rather than new tech debt; relocating
# the struct out of guardrails is a separate refactor.
declare -A LAYER_EXEMPT=(
    ["git_verify_ops.c:guardrails.h"]=1
)

LAYER0_FILES="db.c db_migrations.c config.c util.c text.c render.c log.c dstr.c platform_random.c client_integrations.c mcp_tools.c git_verify.c git_verify_ops.c \
posix/platform_ipc.c posix/platform_path.c posix/platform_process.c posix/platform_random.c posix/util.c \
linux/platform_event.c linux/platform_ipc.c linux/platform_process.c \
mac/platform_event.c mac/platform_ipc.c mac/platform_process.c \
windows/platform_event.c windows/platform_ipc.c windows/platform_path.c windows/platform_process.c windows/platform_random.c windows/util.c"
LAYER1_FILES="memory.c memory_promote.c memory_context.c memory_scan.c memory_graph.c memory_advanced.c trace_analysis.c index.c extractors.c extractors_extra.c rules.c tasks.c feedback.c guardrails.c branch_ownership.c workspace.c working_memory.c agent_config.c"
LAYER2_FILES="agent.c agent_protocol.c agent_policy.c agent_context.c agent_plan.c agent_eval.c agent_eval_memory_support.c agent_coord.c agent_jobs.c agent_tools.c agent_tools_defs.c agent_http.c agent_fallback.c http_retry.c failover.c agent_tunnel.c"

layer_violations=""
check_layer_includes() {
    local file="$1" forbidden="$2"
    [ -f "$file" ] || return 0
    local bad
    bad=$(grep -oP '#include\s+"(headers/)?\K[^"]+' "$file" | grep -E "^($forbidden)$" || true)
    for h in $bad; do
        if [ -z "${LAYER_EXEMPT[$file:$h]:-}" ]; then
            layer_violations="$layer_violations $file->$h"
        fi
    done
}

for f in $LAYER0_FILES; do check_layer_includes "$f" "$L1_HDRS|$L2_HDRS|$L3_HDRS"; done
for f in $LAYER1_FILES; do check_layer_includes "$f" "$L2_HDRS|$L3_HDRS"; done
for f in $LAYER2_FILES; do check_layer_includes "$f" "$L3_HDRS"; done

if [ -z "$layer_violations" ]; then
    pass "no layer boundary violations (${#LAYER_EXEMPT[@]} exempt)"
else
    fail "layer boundary violations:$layer_violations"
fi

# ────── Parallel build groups ──────────────────────────────────────────────
# Checks 8, 9, 9b, 9c, 12, 13 each use an isolated OBJDIR with no shared
# filesystem state. Run them concurrently; buffer each group's output and
# replay in order so the log is deterministic.

PAR_TMPDIR=$(mktemp -d)
trap 'rm -rf "$PAR_TMPDIR"' EXIT

_par_run() {
    local name="$1"; shift
    (
        FAIL=0
        fail() { echo "  FAIL: $1"; FAIL=1; }
        pass() { echo "  PASS: $1"; }
        "$@"
        exit $FAIL
    ) > "$PAR_TMPDIR/$name.out" 2>&1 &
    echo $! > "$PAR_TMPDIR/$name.pid"
}

_par_collect() {
    local name="$1"
    wait "$(cat "$PAR_TMPDIR/$name.pid")"
    local rc=$?
    cat "$PAR_TMPDIR/$name.out"
    [ "$rc" = "0" ] || FAIL=1
}

_check_existing_shipped_artifacts() {
    local INTEG_BINARY="../aimee"
    local INTEG_WEBCHAT="../aimee-webchat"
    local INTEG_SERVER="../aimee-server"
    local INTEG_KB="../aimee-kb"
    local INTEG_GATEWAY="../aimee-gateway"
    local INTEG_BINARY_ABS
    INTEG_BINARY_ABS="$(pwd)/$INTEG_BINARY"

    local missing_shipped=""
    for f in "$INTEG_BINARY" "$INTEG_WEBCHAT" "$INTEG_SERVER" "$INTEG_KB" "$INTEG_GATEWAY"; do
        [ -x "$f" ] || missing_shipped="$missing_shipped $f"
    done
    if [ -z "$missing_shipped" ]; then
        pass "existing shipped artifacts are present"
    else
        fail "existing shipped artifacts missing:$missing_shipped (run make check-linking first)"
        return
    fi

    local legacy_artifacts=""
    for f in ../aimee-client ../aimee-worker ../aimee-mcp ../aimem \
             ../aimee-client.exe ../aimee-worker.exe ../aimee-mcp.exe ../aimem.exe; do
        [ -e "$f" ] && legacy_artifacts="$legacy_artifacts $f"
    done
    if [ -z "$legacy_artifacts" ]; then
        pass "legacy root artifacts are retired"
    else
        fail "legacy root artifacts remain:$legacy_artifacts"
    fi

    local client_help_leaks
    client_help_leaks=$("$INTEG_BINARY" help --all 2>&1 | \
        grep -E '\b(init|setup|config|verify|doctor|kb|database|DB[123]|aimee-kb|db|dashboard|migrate|export|import|eval|branch)\b' || true)
    if [ -z "$client_help_leaks" ]; then
        pass "client help exposes only routed storage-neutral commands"
    else
        fail "client help exposes unported/storage-internal terms: $client_help_leaks"
    fi

    local help_tmp help_output help_rc
    help_tmp=$(mktemp -d)
    help_output=$(cd "$help_tmp" && "$INTEG_BINARY_ABS" identity snapshot --help 2>&1)
    help_rc=$?
    if [ "$help_rc" -eq 0 ] &&
       grep -q "identity" <<< "$help_output" &&
       [ ! -e "$help_tmp/benchmarks/identity" ]; then
        pass "server-routed identity snapshot help has no side effects"
    else
        fail "identity snapshot --help should print help without writing a snapshot"
    fi
    rm -rf "$help_tmp"

    local provider_help_output provider_help_rc
    provider_help_output=$("$INTEG_BINARY_ABS" provider --help 2>&1)
    provider_help_rc=$?
    if [ "$provider_help_rc" -eq 0 ] &&
       grep -q "provider" <<< "$provider_help_output" &&
       ! grep -q "Unknown command" <<< "$provider_help_output"; then
        pass "server-routed provider help is client-side"
    else
        fail "provider --help should print client help instead of server errors"
    fi

    local storage_string_leaks=""
    local leaks
    leaks=$(strings "$INTEG_BINARY" 2>/dev/null | \
        grep -E 'DB[12]|(^|[^[:alnum:]_])db[12]([^[:alnum:]_]|$)|db[12]_|database|postgres|sqlite|Hybrid DB|db status|db check|db backup|db recover|db pragma' || true)
    if [ -n "$leaks" ]; then
        storage_string_leaks="$storage_string_leaks $INTEG_BINARY:$leaks"
    fi
    leaks=$(strings "$INTEG_WEBCHAT" 2>/dev/null | \
        grep -E 'aimee_db_|kb_client_|db1_|DB[12]_DISABLED' || true)
    if [ -n "$leaks" ]; then
        storage_string_leaks="$storage_string_leaks $INTEG_WEBCHAT:$leaks"
    fi
    if [ -z "$storage_string_leaks" ]; then
        pass "client/webchat binaries expose no DB-tier storage strings"
    else
        fail "client/webchat storage string leaks:$storage_string_leaks"
    fi

    local retired_artifact_string_leaks=""
    for bin in "$INTEG_BINARY" "$INTEG_WEBCHAT"; do
        leaks=$(strings "$bin" 2>/dev/null | \
            grep -E "Run 'aimee'|aimee-worker|aimee-mcp|aimem" || true)
        if [ -n "$leaks" ]; then
            retired_artifact_string_leaks="$retired_artifact_string_leaks $bin:$leaks"
        fi
    done
    if [ -z "$retired_artifact_string_leaks" ]; then
        pass "client/webchat binaries expose no retired artifact instructions"
    else
        fail "client/webchat retired artifact string leaks:$retired_artifact_string_leaks"
    fi

    local retired_command_string_leaks=""
    for bin in "$INTEG_SERVER" "$INTEG_KB"; do
        leaks=$(strings "$bin" 2>/dev/null | \
            grep -E "aimee agent reference|aimee agent test|aimee memory antipattern|aimee autopilot resume|Local commands \\(memory, index, rules, db\\)|aimee-worker|aimee-mcp|aimem" || true)
        if [ -n "$leaks" ]; then
            retired_command_string_leaks="$retired_command_string_leaks $bin:$leaks"
        fi
    done
    if [ -z "$retired_command_string_leaks" ]; then
        pass "server/kb binaries expose no retired command instructions"
    else
        fail "server/kb retired command string leaks:$retired_command_string_leaks"
    fi

    local unrouted_repair_string_leaks=""
    for bin in "$INTEG_BINARY" "$INTEG_WEBCHAT" "$INTEG_SERVER" "$INTEG_KB"; do
        leaks=$(strings "$bin" 2>/dev/null | \
            grep -E 'aimee (doctor --fix|kb repair|memory repair --all)' || true)
        if [ -n "$leaks" ]; then
            unrouted_repair_string_leaks="$unrouted_repair_string_leaks $bin:$leaks"
        fi
    done
    if [ -z "$unrouted_repair_string_leaks" ]; then
        pass "shipped binaries avoid unrouted repair command guidance"
    else
        fail "shipped binaries suggest unrouted repair commands:$unrouted_repair_string_leaks"
    fi
}

_check_branchswitch_objdir_recreate() {
    # Regression check for Makefile branch-switch cleanup. Build one nested
    # object before and after a fake branch change instead of rebuilding all
    # shipped artifacts.
    local BSOBJ=build/obj-branchswitch-check
    local BSBRANCH=build/branchswitch-check-branch.txt
    local BSTARGET="$BSOBJ/posix/platform_path.o"
    rm -rf "$BSOBJ" "$BSBRANCH"
    if make "$BSTARGET" OBJDIR="$BSOBJ" BRANCH_FILE="$BSBRANCH" >/dev/null 2>&1; then
        echo "fake-previous-branch" > "$BSBRANCH"
        if make "$BSTARGET" OBJDIR="$BSOBJ" BRANCH_FILE="$BSBRANCH" >/dev/null 2>&1; then
            pass "branch-switch OBJDIR subdirectories are recreated"
        else
            fail "branch-switch object rebuild failed after OBJDIR cleanup"
        fi
    else
        fail "branch-switch object build failed"
    fi
    rm -rf "$BSOBJ" "$BSBRANCH"
}

_group_integ() {
    # 8. Clean build succeeds (compilation + link)
    # Use isolated OBJDIR/BINARY/SERVER to avoid clobbering parallel builds.
    INTEG_OBJDIR=build/obj-integrity
    INTEG_BINARY=build/aimee-integrity
    INTEG_SERVER=build/aimee-server-integrity
    INTEG_WEBCHAT=build/aimee-webchat-integrity
    INTEG_KB=build/aimee-kb-integrity
    INTEG_GATEWAY=build/aimee-gateway-integrity
    rm -rf "$INTEG_OBJDIR" "$INTEG_BINARY" "$INTEG_SERVER" "$INTEG_WEBCHAT" "$INTEG_KB" "$INTEG_GATEWAY"
    if make all OBJDIR=$INTEG_OBJDIR BINARY=$INTEG_BINARY SERVER=$INTEG_SERVER \
            WEBCHAT=$INTEG_WEBCHAT KB=$INTEG_KB GATEWAY=$INTEG_GATEWAY >/dev/null 2>&1; then
        pass "clean build succeeds"
    else
        fail "clean build failed"
    fi
    missing_shipped=""
    for f in "$INTEG_BINARY" "$INTEG_WEBCHAT" "$INTEG_SERVER" "$INTEG_KB" "$INTEG_GATEWAY"; do
        [ -x "$f" ] || missing_shipped="$missing_shipped $f"
    done
    if [ -z "$missing_shipped" ]; then
        pass "make all builds all shipped artifacts"
    else
        fail "make all missing shipped artifacts:$missing_shipped"
    fi
    INTEG_BINARY_ABS="$(pwd)/$INTEG_BINARY"
    legacy_artifacts=""
    for f in ../aimee-client ../aimee-worker ../aimee-mcp ../aimem \
             ../aimee-client.exe ../aimee-worker.exe ../aimee-mcp.exe ../aimem.exe; do
        [ -e "$f" ] && legacy_artifacts="$legacy_artifacts $f"
    done
    if [ -z "$legacy_artifacts" ]; then
        pass "legacy root artifacts are retired"
    else
        fail "legacy root artifacts remain:$legacy_artifacts"
    fi

    client_help_leaks=$("$INTEG_BINARY" help --all 2>&1 | \
        grep -E '\b(init|setup|config|verify|doctor|kb|database|DB[123]|aimee-kb|db|dashboard|migrate|export|import|eval|branch)\b' || true)
    if [ -z "$client_help_leaks" ]; then
        pass "client help exposes only routed storage-neutral commands"
    else
        fail "client help exposes unported/storage-internal terms: $client_help_leaks"
    fi

    help_tmp=$(mktemp -d)
    help_output=$(cd "$help_tmp" && "$INTEG_BINARY_ABS" identity snapshot --help 2>&1)
    help_rc=$?
    if [ "$help_rc" -eq 0 ] &&
       grep -q "identity" <<< "$help_output" &&
       [ ! -e "$help_tmp/benchmarks/identity" ]; then
        pass "server-routed identity snapshot help has no side effects"
    else
        fail "identity snapshot --help should print help without writing a snapshot"
    fi
    rm -rf "$help_tmp"

    provider_help_output=$("$INTEG_BINARY_ABS" provider --help 2>&1)
    provider_help_rc=$?
    if [ "$provider_help_rc" -eq 0 ] &&
       grep -q "provider" <<< "$provider_help_output" &&
       ! grep -q "Unknown command" <<< "$provider_help_output"; then
        pass "server-routed provider help is client-side"
    else
        fail "provider --help should print client help instead of server errors"
    fi

    doc_client_contract_leaks=$(grep -REn \
        'aimee[[:space:]]+(\+|doctor|init|setup|plan|implement|usage|work|workspace)\b|aimee[[:space:]]+memory[[:space:]]+(embed|history|reembed|repair|stats|supersede|maintain|verify|review)\b|aimee[[:space:]]+index[[:space:]]+(overview|blast-radius|scan)\b' \
        ../README.md ../docs/COMMANDS.md ../docs/agent.md ../docs/WORKSPACES.md ../docs/DELEGATES.md \
        ../docs/BENCHMARKS.md ../docs/embedder-sweep.md ../docs/STATUS.md ../src/README.md \
        ../benchmarks/locomo/EVAL_CONFIG.md ../benchmarks/longmemeval/EVAL_CONFIG.md \
        ../benchmarks/lora/README.md \
        2>/dev/null || true)
    if [ -z "$doc_client_contract_leaks" ]; then
        pass "user docs advertise only routed client commands"
    else
        fail "user docs advertise unported client commands:$doc_client_contract_leaks"
    fi

    doc_retired_artifact_leaks=$(grep -REn \
        '`aimee[[:space:]]+(doctor|init|setup|kb|migrate|db|eval|branch)\b|aimee-worker|aimee-mcp|aimem|aimee-client' \
        ../README.md ../docs/COMMANDS.md ../docs/agent.md ../docs/WORKSPACES.md ../docs/DELEGATES.md \
        ../docs/BENCHMARKS.md ../docs/embedder-sweep.md ../docs/STATUS.md ../src/README.md \
        ../benchmarks/locomo/EVAL_CONFIG.md ../benchmarks/longmemeval/EVAL_CONFIG.md \
        ../benchmarks/lora/README.md \
        2>/dev/null || true)
    if [ -z "$doc_retired_artifact_leaks" ]; then
        pass "user docs avoid retired aimee command artifacts"
    else
        fail "user docs reference retired command artifacts:$doc_retired_artifact_leaks"
    fi

    # The CLI client (aimee) is a DB-free thin wrapper — no DB libraries allowed.
    # aimee-webchat is now a full HTTP server process with its own SQLite session
    # store (PAM auth sessions, rate-limit state); it may link sqlite but must not
    # contain aimee DB1/DB2/DB3 API strings (aimee_db_, kb_client, etc.).
    storage_string_leaks=""
    for bin in "$INTEG_BINARY"; do
        leaks=$(strings "$bin" 2>/dev/null | \
            grep -E 'DB[12]|(^|[^[:alnum:]_])db[12]([^[:alnum:]_]|$)|db[12]_|database|postgres|sqlite|Hybrid DB|db status|db check|db backup|db recover|db pragma' || true)
        if [ -n "$leaks" ]; then
            storage_string_leaks="$storage_string_leaks $bin:$leaks"
        fi
    done
    # Webchat may use sqlite for session storage but must not expose aimee DB APIs.
    webchat_aimee_db_leaks=$(strings "$INTEG_WEBCHAT" 2>/dev/null | \
        grep -E 'aimee_db_|kb_client_|db1_|DB[12]_DISABLED' || true)
    if [ -n "$webchat_aimee_db_leaks" ]; then
        storage_string_leaks="$storage_string_leaks $INTEG_WEBCHAT:$webchat_aimee_db_leaks"
    fi
    if [ -z "$storage_string_leaks" ]; then
        pass "client/webchat binaries expose no DB-tier storage strings"
    else
        fail "client/webchat storage string leaks:$storage_string_leaks"
    fi

    retired_artifact_string_leaks=""
    for bin in "$INTEG_BINARY" "$INTEG_WEBCHAT"; do
        leaks=$(strings "$bin" 2>/dev/null | \
            grep -E "Run 'aimee'|aimee-worker|aimee-mcp|aimem" || true)
        if [ -n "$leaks" ]; then
            retired_artifact_string_leaks="$retired_artifact_string_leaks $bin:$leaks"
        fi
    done
    if [ -z "$retired_artifact_string_leaks" ]; then
        pass "client/webchat binaries expose no retired artifact instructions"
    else
        fail "client/webchat retired artifact string leaks:$retired_artifact_string_leaks"
    fi

    retired_command_string_leaks=""
    for bin in "$INTEG_SERVER" "$INTEG_KB"; do
        leaks=$(strings "$bin" 2>/dev/null | \
            grep -E "aimee agent reference|aimee agent test|aimee memory antipattern|aimee autopilot resume|Local commands \\(memory, index, rules, db\\)|aimee-worker|aimee-mcp|aimem" || true)
        if [ -n "$leaks" ]; then
            retired_command_string_leaks="$retired_command_string_leaks $bin:$leaks"
        fi
    done
    if [ -z "$retired_command_string_leaks" ]; then
        pass "server/kb binaries expose no retired command instructions"
    else
        fail "server/kb retired command string leaks:$retired_command_string_leaks"
    fi

    unrouted_repair_string_leaks=""
    for bin in "$INTEG_BINARY" "$INTEG_WEBCHAT" "$INTEG_SERVER" "$INTEG_KB"; do
        leaks=$(strings "$bin" 2>/dev/null | \
            grep -E 'aimee (doctor --fix|kb repair|memory repair --all)' || true)
        if [ -n "$leaks" ]; then
            unrouted_repair_string_leaks="$unrouted_repair_string_leaks $bin:$leaks"
        fi
    done
    if [ -z "$unrouted_repair_string_leaks" ]; then
        pass "shipped binaries avoid unrouted repair command guidance"
    else
        fail "shipped binaries suggest unrouted repair commands:$unrouted_repair_string_leaks"
    fi

    rm -rf "$INTEG_OBJDIR" "$INTEG_BINARY" "$INTEG_SERVER" "$INTEG_WEBCHAT" "$INTEG_KB" "$INTEG_GATEWAY"
}

_group_lean() {
    # 9b. Lean build succeeds and meets size limit
    LEAN_BIN=build/aimee-lean-integrity
    LEAN_SRV=build/aimee-server-lean-integrity
    LEAN_WEB=build/aimee-webchat-lean-integrity
    LEAN_KB=build/aimee-kb-lean-integrity
    LEAN_GW=build/aimee-gateway-lean-integrity
    LEAN_OBJ=build/obj-lean-integrity
    rm -rf "$LEAN_OBJ" "$LEAN_BIN" "$LEAN_SRV" "$LEAN_WEB" "$LEAN_KB" "$LEAN_GW"
    if make all server OBJDIR=$LEAN_OBJ BINARY=$LEAN_BIN SERVER=$LEAN_SRV \
            WEBCHAT=$LEAN_WEB KB=$LEAN_KB GATEWAY=$LEAN_GW \
            EXTRA_C_FLAGS="-Os -g0 -ffunction-sections -fdata-sections" \
            EXTRA_L_FLAGS="-s -Wl,--gc-sections" >/dev/null 2>&1; then
        lean_size=$(stat -c%s "$LEAN_SRV" 2>/dev/null || stat -f%z "$LEAN_SRV" 2>/dev/null)
        limit=$((1500 * 1024))
        if [ "$lean_size" -le "$limit" ]; then
            pass "lean build succeeds and server binary ${lean_size} bytes <= 1.5MB"
        else
            fail "lean server binary ${lean_size} bytes exceeds 1.5MB limit"
        fi
    else
        fail "lean build failed"
    fi
    rm -rf "$LEAN_OBJ" "$LEAN_BIN" "$LEAN_SRV" "$LEAN_WEB" "$LEAN_KB" "$LEAN_GW"
}

_group_dynlink() {
    # 9c. Dynamic linking policy: system libraries must be dynamically linked.
    # Only checked on Linux (ldd) — macOS uses otool and Windows is intentionally static.
    if ! command -v ldd >/dev/null 2>&1; then
        pass "dynamic linking check: skipped (no ldd — non-Linux platform)"
        return
    fi
    # Rebuild with a dedicated OBJDIR so we have binaries to inspect
    DLOBJ=build/obj-dynlink
    DLBIN=build/aimee-dynlink
    DLSRV=build/aimee-server-dynlink
    DLWEB=build/aimee-webchat-dynlink
    DLKB=build/aimee-kb-dynlink
    DLGW=build/aimee-gateway-dynlink
    rm -rf "$DLOBJ" "$DLBIN" "$DLSRV" "$DLWEB" "$DLKB" "$DLGW"
    if make all server "$DLKB" OBJDIR=$DLOBJ BINARY=$DLBIN SERVER=$DLSRV \
            WEBCHAT=$DLWEB KB=$DLKB GATEWAY=$DLGW >/dev/null 2>&1; then
        dl_fail=0
        for bin in "$DLBIN" "$DLWEB" "$DLSRV" "$DLKB"; do
            if ldd "$bin" 2>&1 | grep -q 'not a dynamic executable'; then
                fail "$bin is statically linked (policy requires dynamic system libs)"
                dl_fail=1
            fi
        done
        # CLI client must not link any DB backend libraries.
        if ldd "$DLBIN" | grep -q 'libsqlite3'; then
            fail "$DLBIN: libsqlite3 linked into DB-free client binary"
            dl_fail=1
        fi
        if ldd "$DLBIN" | grep -q 'libpq'; then
            fail "$DLBIN: libpq linked into DB-free client binary"
            dl_fail=1
        fi
        # Webchat (full server) may use libsqlite3 for session storage but not libpq.
        if ldd "$DLWEB" 2>/dev/null | grep -q 'libpq'; then
            fail "$DLWEB: libpq linked into webchat binary"
            dl_fail=1
        fi
        # Server owns DB1 only: sqlite3 yes, libpq no.
        if ! ldd "$DLSRV" | grep -q 'libsqlite3'; then
            fail "aimee-server: libsqlite3 not dynamically linked"
            dl_fail=1
        fi
        if ldd "$DLSRV" | grep -q 'libpq'; then
            fail "aimee-server: libpq linked into DB1-only server"
            dl_fail=1
        fi
        if command -v readelf >/dev/null 2>&1 && readelf -Ws "$DLSRV" | grep -Eq 'db2_|PQ[A-Z]'; then
            fail "aimee-server: DB2 symbols present in DB1-only server"
            dl_fail=1
        fi
        if command -v nm >/dev/null 2>&1; then
            for server_db_free_obj in memory_maintenance.o memory_prospective.o memory_lifecycle.o memory_directives.o memory_health.o memory_context.o memory_graph.o memory_scan.o memory_episodes.o memory_improve.o index.o learning_router.o memory_conflict.o memory_logic.o memory_assemble.o kb.o memory_advanced.o memory_core.o; do
                if nm --undefined-only "$DLOBJ/server/$server_db_free_obj" 2>/dev/null | grep -Eq ' db2_'; then
                    fail "aimee-server: $server_db_free_obj references DB2"
                    dl_fail=1
                fi
            done
        fi
        # KB owns DB2 only (incl. pgvector): libpq yes, sqlite3 no.
        if ldd "$DLKB" | grep -q 'libsqlite3'; then
            fail "aimee-kb: libsqlite3 linked into DB2-only kb"
            dl_fail=1
        fi
        if ! ldd "$DLKB" | grep -q 'libpq'; then
            fail "aimee-kb: libpq not dynamically linked"
            dl_fail=1
        fi
        if command -v readelf >/dev/null 2>&1 && readelf -Ws "$DLKB" | grep -Eq 'db1_|sqlite3_'; then
            fail "aimee-kb: DB1/sqlite symbols present in DB2-only kb"
            dl_fail=1
        fi
        # Server must dynamically link ssl and crypto
        if ! ldd "$DLSRV" | grep -q 'libssl'; then
            fail "aimee-server: libssl not dynamically linked"
            dl_fail=1
        fi
        if ! ldd "$DLSRV" | grep -q 'libcrypto'; then
            fail "aimee-server: libcrypto not dynamically linked"
            dl_fail=1
        fi
        if [ "$dl_fail" = "0" ]; then
            pass "dynamic linking policy: client/webchat DB-free, server DB1-only, kb DB2-only"
        fi
    else
        fail "dynamic linking check: build failed"
    fi
    rm -rf "$DLOBJ" "$DLBIN" "$DLSRV" "$DLWEB" "$DLKB" "$DLGW"
}

_group_branchswitch() {
    # 12. Branch-switch build: OBJDIR subdirectories must be recreated after a branch change.
    # The branch-switch logic deletes OBJDIR; _DUMMY must run AFTER that block to recreate subdirs.
    # Without this ordering, gcc fails writing .d dependency files into non-existent directories.
    BSOBJ=build/obj-branchswitch
    BSBIN=build/aimee-branchswitch
    BSSRV=build/aimee-server-branchswitch
    BSWEB=build/aimee-webchat-branchswitch
    BSKB=build/aimee-kb-branchswitch
    BSGW=build/aimee-gateway-branchswitch
    BSBRANCH=build/branchswitch-branch.txt
    rm -rf "$BSOBJ" "$BSBIN" "$BSSRV" "$BSWEB" "$BSKB" "$BSGW" "$BSBRANCH"
    if make all server OBJDIR=$BSOBJ BINARY=$BSBIN SERVER=$BSSRV WEBCHAT=$BSWEB \
            KB=$BSKB GATEWAY=$BSGW BRANCH_FILE=$BSBRANCH >/dev/null 2>&1; then
        # Fake a prior branch so the next make triggers the branch-switch cleanup + re-mkdir
        echo "fake-previous-branch" > "$BSBRANCH"
        if make all server OBJDIR=$BSOBJ BINARY=$BSBIN SERVER=$BSSRV WEBCHAT=$BSWEB \
                KB=$BSKB GATEWAY=$BSGW BRANCH_FILE=$BSBRANCH >/dev/null 2>&1; then
            pass "build succeeds after simulated branch switch (subdirectories recreated)"
        else
            fail "build fails after simulated branch switch (missing OBJDIR subdirectories — _DUMMY must run after branch-switch block)"
        fi
    else
        fail "branch-switch test: initial build failed"
    fi
    rm -rf "$BSOBJ" "$BSBIN" "$BSSRV" "$BSWEB" "$BSKB" "$BSGW" "$BSBRANCH"
}

if [ "$MODE" = "--build-variants" ]; then
    _par_run integ       _group_integ
    _par_run lean        _group_lean
    _par_run dynlink     _group_dynlink
    _par_run bs          _group_branchswitch

    _par_collect integ
    _par_collect lean
    _par_collect dynlink
    _par_collect bs
else
    _check_existing_shipped_artifacts
    _check_branchswitch_objdir_recreate
fi

echo ""
if [ "$FAIL" = "0" ]; then
    if [ "$MODE" = "--build-variants" ]; then
        echo "All build variant checks passed."
    else
        echo "All build integrity checks passed."
    fi
else
    if [ "$MODE" = "--build-variants" ]; then
        echo "Build variant checks FAILED."
    else
        echo "Build integrity checks FAILED."
    fi
    exit 1
fi
