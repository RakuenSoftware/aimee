cmake_minimum_required(VERSION 3.16)
project(aimee C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Expose POSIX and GNU extensions on all platforms (MinGW needs this for
# gmtime_r, strtok_r, strcasecmp, strncasecmp, etc.)
add_compile_definitions(_GNU_SOURCE)

# Version string embedded into binaries (aimee version / --version). Mirrors the
# Makefile's GIT_VERSION injection. Release/packaging passes -DAIMEE_VERSION_STR=vX.Y.Z;
# when unset the headers fall back to the AIMEE_VERSION default in aimee_version.h.
if(DEFINED AIMEE_VERSION_STR AND NOT AIMEE_VERSION_STR STREQUAL "")
    add_compile_definitions(AIMEE_VERSION="${AIMEE_VERSION_STR}")
endif()

option(WITH_PAM "Enable PAM authentication support" ON)
option(WITH_LIBSECRET "Enable libsecret integration" ON)
option(WITH_UI "Enable embedded dashboard UI" OFF)
option(WITH_TLS "Enable in-client TLS for https:// remote aimee-servers" ON)
# Native TLS backends are part of aimee-core-connection:
#   Linux   -> OpenSSL
#   macOS   -> Secure Transport
#   Windows -> Schannel/SSPI
# Each uses the OS trust store, so the single-binary thin client needs no bundled
# CA bundle and no external TLS library.
option(AIMEE_LEAN "Lean build profile: size-optimized, no UI" OFF)
# Thin-client-only profile: configure and build ONLY the `aimee` CLI target,
# skipping the unit-test suite (aimee-server / aimee-kb / aimee-gateway /
# aimee-runtime-web are not CMake targets at all — they build via `make -C src`).
# The thin client links only Threads (+ OpenSSL for https), so this lets
# a release/packaging box build it with no Go, libpq, zstd, PAM, or libsecret
# toolchain. Composes with AIMEE_LEAN for a size-optimized, stripped binary.
option(AIMEE_THIN_CLIENT "Build only the aimee thin client (no server/kb/gateway/webchat)" OFF)

include(CTest)

# Lean profile overrides
if(AIMEE_LEAN)
    set(WITH_UI OFF CACHE BOOL "Enable embedded dashboard UI" FORCE)
endif()

# --- Dynamic linking policy ---
# Prefer shared system libraries on POSIX to keep binaries lean.
# Static linking is only used for the MinGW runtime on Windows.
if(NOT WIN32)
    if(APPLE)
        set(CMAKE_FIND_LIBRARY_SUFFIXES ".dylib" ".tbd" ".a")
    else()
        set(CMAKE_FIND_LIBRARY_SUFFIXES ".so" ".a")
    endif()
endif()

set(AIMEE_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(AIMEE_MODULE_RUNTIME_INCLUDE_DIR
    "${AIMEE_SRC_DIR}/modules/module-runtime/include")
set(AIMEE_AUDIT_INCLUDE_DIR
    "${AIMEE_SRC_DIR}/modules/audit/include")
set(AIMEE_GATEWAY_INCLUDE_DIR
    "${AIMEE_SRC_DIR}/modules/gateway/include")
set(AIMEE_IR_INCLUDE_DIR
    "${AIMEE_SRC_DIR}/modules/ir/include")
set(AIMEE_TRANSLATION_INCLUDE_DIR
    "${AIMEE_SRC_DIR}/modules/translation/include")
set(AIMEE_DELEGATES_INCLUDE_DIR
    "${AIMEE_SRC_DIR}/modules/delegates/include")
set(AIMEE_PROTOCOLS_INCLUDE_DIR
    "${AIMEE_SRC_DIR}/modules/protocols/include")
set(AIMEE_CORE_CONNECTION_INCLUDE_DIR
    "${AIMEE_SRC_DIR}/core/connection/include")
set(AIMEE_CORE_EVENT_BUS_INCLUDE_DIR
    "${AIMEE_SRC_DIR}/core/event_bus/include")

# The core-modularization layout keeps each module's public headers under
# modules/<m>/include/aimee/<m>/. The Makefile adds these as -Imodules/<m>/include;
# mirror that globally for every CMake target (they were defined above but never
# wired into any target, so <aimee/protocols/acp/acp_server.h> etc. did not resolve).
include_directories(
    ${AIMEE_MODULE_RUNTIME_INCLUDE_DIR}
    ${AIMEE_AUDIT_INCLUDE_DIR}
    ${AIMEE_GATEWAY_INCLUDE_DIR}
    ${AIMEE_IR_INCLUDE_DIR}
    ${AIMEE_TRANSLATION_INCLUDE_DIR}
    ${AIMEE_DELEGATES_INCLUDE_DIR}
    ${AIMEE_PROTOCOLS_INCLUDE_DIR}
    ${AIMEE_CORE_CONNECTION_INCLUDE_DIR}
    ${AIMEE_CORE_EVENT_BUS_INCLUDE_DIR}
    "${AIMEE_SRC_DIR}/modules/memory/include"
    "${AIMEE_SRC_DIR}/modules/learning/include"
    "${AIMEE_SRC_DIR}/modules/tools/include"
    "${AIMEE_SRC_DIR}/modules/workspace/include"
    "${AIMEE_SRC_DIR}/modules/git/include"
    "${AIMEE_SRC_DIR}/modules/response-composition/include"
    "${AIMEE_SRC_DIR}/modules/skills/include"
    "${AIMEE_SRC_DIR}/modules/routing/include"
    "${AIMEE_SRC_DIR}/modules/roundtable/include"
    "${AIMEE_SRC_DIR}/modules/benchmarks/include"
    "${AIMEE_SRC_DIR}/modules/governance/include"
    "${AIMEE_SRC_DIR}/modules/execution-policy/include"
    "${AIMEE_SRC_DIR}/modules/workflows/include"
    "${AIMEE_SRC_DIR}/modules/kb-synthesis/include"
    "${AIMEE_SRC_DIR}/modules/runtime-web/include"
    "${AIMEE_SRC_DIR}/modules/control-web/include"
    "${AIMEE_SRC_DIR}/modules/postgres/include"
    # MODULES THAT KEEP THEIR HEADERS FLAT, beside their sources, rather than
    # under an include/ subdir. src/Makefile has carried -I for every one of
    # these all along; CMake had only the include/-style modules, so the
    # thin-client build compiled these modules' sources while being unable to
    # find their headers. That is why macos-build and windows-cmake failed on
    # 'wfe_def.h' and then 'guardrails.h' (the latter reached from
    # src/headers/aimee.h, so it broke translation units that never named a
    # module at all) while the Make build stayed green.
    #
    # Listed to match src/Makefile rather than added one at a time as each new
    # missing header surfaces: an unused -I costs nothing, and the next flat
    # header should not be another red CI run.
    "${AIMEE_SRC_DIR}/modules/workflows"
    "${AIMEE_SRC_DIR}/modules/guardrails"
    "${AIMEE_SRC_DIR}/modules/economizer"
    "${AIMEE_SRC_DIR}/modules/roadmap"
    "${AIMEE_SRC_DIR}/modules/sandbox"
    "${AIMEE_SRC_DIR}/modules/lsp"
    "${AIMEE_SRC_DIR}/modules/css"
    "${AIMEE_SRC_DIR}/modules/webuser"
    "${AIMEE_SRC_DIR}/modules/governance"
    "${AIMEE_SRC_DIR}/modules/vault"
    "${AIMEE_SRC_DIR}/modules/roundtable"
    "${AIMEE_SRC_DIR}/modules/kb_client"
    "${AIMEE_SRC_DIR}/modules/kb-synthesis"
    "${AIMEE_SRC_DIR}/modules/benchmarks"
    "${AIMEE_SRC_DIR}/modules/aws"
    "${AIMEE_SRC_DIR}/pipeline")

# LTO / IPO: must be set BEFORE any target is declared so the setting takes
# effect uniformly across aimee-core/data/agent/cmd and every test binary.
# Previously this block lived further down the file, which meant the static
# libs built as plain ELF while tests added via add_subdirectory(src/tests)
# got LTO bitcode — causing undefined-reference failures at link time
# (working_profile_observe, agent_http_*) that only showed up in the test targets.
include(CheckIPOSupported)
check_ipo_supported(RESULT LTO_SUPPORTED OUTPUT LTO_ERROR)
if(LTO_SUPPORTED AND NOT WIN32)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

if(AIMEE_LEAN)
    set(AIMEE_COMMON_WARNINGS
        -Os
        -g0
        -Wall
        -Wextra
        -Werror
        -Wno-unused-parameter
        -Wno-format-truncation
        -Wno-unused-result
        -ffunction-sections
        -fdata-sections
    )
else()
    set(AIMEE_COMMON_WARNINGS
        -O2
        -g
        -Wall
        -Wextra
        -Werror
        -Wno-unused-parameter
        -Wno-format-truncation
        -Wno-unused-result
        -ffunction-sections
        -fdata-sections
    )
endif()

# --- Platform-specific source selection ---
if(WIN32)
    set(PLATFORM_CORE_SRCS
        ${AIMEE_SRC_DIR}/windows/platform_event.c
        ${AIMEE_SRC_DIR}/windows/platform_ipc.c
        ${AIMEE_SRC_DIR}/windows/platform_net.c
        ${AIMEE_SRC_DIR}/windows/platform_path.c
        ${AIMEE_SRC_DIR}/windows/platform_process.c
        ${AIMEE_SRC_DIR}/windows/platform_random.c
        ${AIMEE_SRC_DIR}/windows/secret_store.c
        ${AIMEE_SRC_DIR}/windows/util.c
        ${AIMEE_SRC_DIR}/windows/events.c
    )
    set(PLATFORM_DATA_SRCS
        ${AIMEE_SRC_DIR}/windows/history.c
        ${AIMEE_SRC_DIR}/windows/memory.c
        ${AIMEE_SRC_DIR}/windows/cmd_describe.c
    )
    set(PLATFORM_AGENT_SRCS
        ${AIMEE_SRC_DIR}/windows/agent_runtime.c
        ${AIMEE_SRC_DIR}/windows/agent_bridge.c
        ${AIMEE_SRC_DIR}/windows/web_egress.c
    )
    set(PLATFORM_CMD_SRCS
        ${AIMEE_SRC_DIR}/windows/cmd_infra.c
        ${AIMEE_SRC_DIR}/windows/cmd_agent_delegate.c
        ${AIMEE_SRC_DIR}/windows/cmd_hooks.c
    )
    if(WITH_UI)
        list(APPEND PLATFORM_CMD_SRCS
            ${AIMEE_SRC_DIR}/windows/dashboard.c
        )
    endif()
    set(PLATFORM_CLI_SRCS
        ${AIMEE_SRC_DIR}/windows/cli_client.c
        ${AIMEE_SRC_DIR}/windows/cli_main.c
    )
    set(PLATFORM_SERVER_SRCS
        ${AIMEE_SRC_DIR}/windows/server_main.c
        ${AIMEE_SRC_DIR}/windows/cli_client.c
        ${AIMEE_SRC_DIR}/windows/server_compute.c
        ${AIMEE_SRC_DIR}/windows/cmd_hooks.c
    )
elseif(APPLE)
    set(PLATFORM_CORE_SRCS
        ${AIMEE_SRC_DIR}/posix/platform_ipc.c
        ${AIMEE_SRC_DIR}/posix/platform_net.c
        ${AIMEE_SRC_DIR}/posix/platform_path.c
        ${AIMEE_SRC_DIR}/posix/platform_process.c
        ${AIMEE_SRC_DIR}/posix/platform_random.c
        ${AIMEE_SRC_DIR}/mac/platform_event.c
        ${AIMEE_SRC_DIR}/mac/platform_ipc.c
        ${AIMEE_SRC_DIR}/mac/platform_process.c
        ${AIMEE_SRC_DIR}/mac/secret_store.c
        ${AIMEE_SRC_DIR}/posix/util.c
        ${AIMEE_SRC_DIR}/posix/events.c
    )
    set(PLATFORM_DATA_SRCS
        ${AIMEE_SRC_DIR}/posix/history.c
        ${AIMEE_SRC_DIR}/posix/memory.c
        ${AIMEE_SRC_DIR}/posix/cmd_describe.c
    )
    set(PLATFORM_AGENT_SRCS
        ${AIMEE_SRC_DIR}/posix/agent_runtime.c
        ${AIMEE_SRC_DIR}/posix/agent_max_turns.c
        ${AIMEE_SRC_DIR}/posix/agent_runtime_messages.c
        ${AIMEE_SRC_DIR}/posix/agent_runtime_support.c
        ${AIMEE_SRC_DIR}/posix/agent_runtime_tmux.c
        ${AIMEE_SRC_DIR}/posix/agent_bridge.c
        ${AIMEE_SRC_DIR}/modules/tools/agent_tools.c
        ${AIMEE_SRC_DIR}/posix/agent_source_authority.c
        ${AIMEE_SRC_DIR}/modules/tools/agent_tools_dispatch.c
        ${AIMEE_SRC_DIR}/modules/tools/agent_tools_effect_contract.c
        ${AIMEE_SRC_DIR}/modules/tools/agent_tools_anchored.c
        ${AIMEE_SRC_DIR}/posix/web_egress.c
        ${AIMEE_SRC_DIR}/posix/web_read.c
        ${AIMEE_SRC_DIR}/posix/sandbox.c
    )
    list(APPEND PLATFORM_AGENT_SRCS
        ${AIMEE_SRC_DIR}/posix/agent_shell_claude.c
        ${AIMEE_SRC_DIR}/posix/agent_shell_claude_pty.c
        ${AIMEE_SRC_DIR}/posix/agent_shell_codex.c

    )
    set(PLATFORM_CMD_SRCS
        ${AIMEE_SRC_DIR}/posix/cmd_infra.c
        ${AIMEE_SRC_DIR}/posix/cmd_agent_delegate.c
        ${AIMEE_SRC_DIR}/posix/cmd_hooks.c
    )
    if(WITH_UI)
        list(APPEND PLATFORM_CMD_SRCS
            ${AIMEE_SRC_DIR}/posix/dashboard.c
        )
    endif()
    set(PLATFORM_CLI_SRCS
        ${AIMEE_SRC_DIR}/posix/cli_client.c
        ${AIMEE_SRC_DIR}/posix/cli_main.c
    )
    set(PLATFORM_SERVER_SRCS
        ${AIMEE_SRC_DIR}/posix/server_main.c
        ${AIMEE_SRC_DIR}/posix/cli_client.c
        ${AIMEE_SRC_DIR}/posix/server_compute.c
        ${AIMEE_SRC_DIR}/posix/cmd_hooks.c
    )
else()
    set(PLATFORM_CORE_SRCS
        ${AIMEE_SRC_DIR}/posix/platform_ipc.c
        ${AIMEE_SRC_DIR}/posix/platform_net.c
        ${AIMEE_SRC_DIR}/posix/platform_path.c
        ${AIMEE_SRC_DIR}/posix/platform_process.c
        ${AIMEE_SRC_DIR}/posix/platform_random.c
        ${AIMEE_SRC_DIR}/linux/platform_event.c
        ${AIMEE_SRC_DIR}/linux/platform_ipc.c
        ${AIMEE_SRC_DIR}/linux/platform_process.c
        ${AIMEE_SRC_DIR}/linux/secret_store.c
        ${AIMEE_SRC_DIR}/posix/util.c
        ${AIMEE_SRC_DIR}/posix/events.c
    )
    set(PLATFORM_DATA_SRCS
        ${AIMEE_SRC_DIR}/posix/history.c
        ${AIMEE_SRC_DIR}/posix/memory.c
        ${AIMEE_SRC_DIR}/posix/cmd_describe.c
    )
    set(PLATFORM_AGENT_SRCS
        ${AIMEE_SRC_DIR}/posix/agent_runtime.c
        ${AIMEE_SRC_DIR}/posix/agent_max_turns.c
        ${AIMEE_SRC_DIR}/posix/agent_runtime_messages.c
        ${AIMEE_SRC_DIR}/posix/agent_runtime_support.c
        ${AIMEE_SRC_DIR}/posix/agent_runtime_tmux.c
        ${AIMEE_SRC_DIR}/posix/agent_bridge.c
        ${AIMEE_SRC_DIR}/modules/tools/agent_tools.c
        ${AIMEE_SRC_DIR}/posix/agent_source_authority.c
        ${AIMEE_SRC_DIR}/modules/tools/agent_tools_dispatch.c
        ${AIMEE_SRC_DIR}/modules/tools/agent_tools_effect_contract.c
        ${AIMEE_SRC_DIR}/modules/tools/agent_tools_anchored.c
        ${AIMEE_SRC_DIR}/posix/web_egress.c
        ${AIMEE_SRC_DIR}/posix/web_read.c
        ${AIMEE_SRC_DIR}/posix/sandbox.c
    )
    list(APPEND PLATFORM_AGENT_SRCS
        ${AIMEE_SRC_DIR}/posix/agent_shell_claude.c
        ${AIMEE_SRC_DIR}/posix/agent_shell_claude_pty.c
        ${AIMEE_SRC_DIR}/posix/agent_shell_codex.c

    )
    set(PLATFORM_CMD_SRCS
        ${AIMEE_SRC_DIR}/posix/cmd_infra.c
        ${AIMEE_SRC_DIR}/posix/cmd_agent_delegate.c
        ${AIMEE_SRC_DIR}/posix/cmd_hooks.c
    )
    if(WITH_UI)
        list(APPEND PLATFORM_CMD_SRCS
            ${AIMEE_SRC_DIR}/posix/dashboard.c
        )
    endif()
    set(PLATFORM_CLI_SRCS
        ${AIMEE_SRC_DIR}/posix/cli_client.c
        ${AIMEE_SRC_DIR}/posix/cli_main.c
    )
    set(PLATFORM_SERVER_SRCS
        ${AIMEE_SRC_DIR}/posix/server_main.c
        ${AIMEE_SRC_DIR}/posix/cli_client.c
        ${AIMEE_SRC_DIR}/posix/server_compute.c
        ${AIMEE_SRC_DIR}/posix/cmd_hooks.c
    )
endif()

if(WIN32)
    set(CLIENT_SUPPORT_SRCS
        ${AIMEE_SRC_DIR}/windows/platform_ipc.c
        ${AIMEE_SRC_DIR}/windows/platform_net.c
        ${AIMEE_SRC_DIR}/windows/platform_path.c
        ${AIMEE_SRC_DIR}/windows/platform_process.c
        # cli_mcp_serve mints the session id when none is published yet.
        ${AIMEE_SRC_DIR}/windows/platform_random.c
        ${AIMEE_SRC_DIR}/windows/history.c
        ${AIMEE_SRC_DIR}/windows/cli_client.c
        ${AIMEE_SRC_DIR}/windows/cli_main.c
        ${AIMEE_SRC_DIR}/windows/util.c
        ${AIMEE_SRC_DIR}/util.c
        ${AIMEE_SRC_DIR}/windows/workspace_provider.c
        ${AIMEE_SRC_DIR}/modules/workspace/workspace_provider_detached.c
        ${AIMEE_SRC_DIR}/aimee_client.c
        # http_uds_client.c compiles to a stub on Windows (no AF_UNIX transport);
        # it provides http_uds_request so the shared CLI (cli_persona/cli_roles)
        # links. The Windows client uses the TCP/HTTP transport at runtime.
        ${AIMEE_SRC_DIR}/http_uds_client.c
    )
    set(KB_PLATFORM_EXTRA_SRCS
        ${AIMEE_SRC_DIR}/windows/cli_client.c
        ${AIMEE_SRC_DIR}/windows/memory.c
        ${AIMEE_SRC_DIR}/windows/agent_bridge.c
        ${AIMEE_SRC_DIR}/windows/web_egress.c
    )
elseif(APPLE)
    set(CLIENT_SUPPORT_SRCS
        ${AIMEE_SRC_DIR}/posix/platform_path.c
        ${AIMEE_SRC_DIR}/posix/platform_process.c
        ${AIMEE_SRC_DIR}/posix/platform_net.c
        ${AIMEE_SRC_DIR}/mac/platform_process.c
        # cli_mcp_serve mints the session id when none is published yet.
        ${AIMEE_SRC_DIR}/posix/platform_random.c
        ${AIMEE_SRC_DIR}/posix/history.c
        ${AIMEE_SRC_DIR}/posix/cli_client.c
        ${AIMEE_SRC_DIR}/posix/cli_main.c
        ${AIMEE_SRC_DIR}/posix/util.c
        ${AIMEE_SRC_DIR}/util.c
        ${AIMEE_SRC_DIR}/posix/workspace_provider.c
        ${AIMEE_SRC_DIR}/modules/workspace/workspace_provider_detached.c
        ${AIMEE_SRC_DIR}/aimee_client.c
        ${AIMEE_SRC_DIR}/http_uds_client.c
    )
    set(KB_PLATFORM_EXTRA_SRCS
        ${AIMEE_SRC_DIR}/posix/cli_client.c
        ${AIMEE_SRC_DIR}/posix/memory.c
        ${AIMEE_SRC_DIR}/posix/agent_bridge.c
    )
else()
    set(CLIENT_SUPPORT_SRCS
        ${AIMEE_SRC_DIR}/posix/platform_path.c
        ${AIMEE_SRC_DIR}/posix/platform_process.c
        ${AIMEE_SRC_DIR}/posix/platform_net.c
        ${AIMEE_SRC_DIR}/linux/platform_process.c
        # cli_mcp_serve mints the session id when none is published yet.
        ${AIMEE_SRC_DIR}/posix/platform_random.c
        ${AIMEE_SRC_DIR}/posix/history.c
        ${AIMEE_SRC_DIR}/posix/cli_client.c
        ${AIMEE_SRC_DIR}/posix/cli_main.c
        ${AIMEE_SRC_DIR}/posix/util.c
        ${AIMEE_SRC_DIR}/util.c
        ${AIMEE_SRC_DIR}/posix/workspace_provider.c
        ${AIMEE_SRC_DIR}/modules/workspace/workspace_provider_detached.c
        ${AIMEE_SRC_DIR}/aimee_client.c
        ${AIMEE_SRC_DIR}/http_uds_client.c
    )
    set(KB_PLATFORM_EXTRA_SRCS
        ${AIMEE_SRC_DIR}/posix/cli_client.c
        ${AIMEE_SRC_DIR}/posix/memory.c
        ${AIMEE_SRC_DIR}/posix/agent_bridge.c
    )
endif()

# The C store module is gone: server-go/modules/postgres serves its kinds now.
#
# What is left of it here is the secret store, which stores secrets as files
# under the config directory and opened no database at any point -- it merely
# lived in that directory.
set(DB1_SRCS
    ${AIMEE_SRC_DIR}/secrets.c
)

# The preserved C implementation now lives under module ownership. It remains
# in-process until the staged aimee-kb process-contract cutover; the later Go
# port replaces this private tree without moving PostgreSQL/pgvector out of DB2.
set(DB2_SRCS
    ${AIMEE_SRC_DIR}/modules/db2/c/db2_init.c
    ${AIMEE_SRC_DIR}/modules/db2/c/db2_reembed.c
    ${AIMEE_SRC_DIR}/modules/db2/c/db2_pool.c
    ${AIMEE_SRC_DIR}/modules/db2/c/agent_hints.c
    ${AIMEE_SRC_DIR}/modules/db2/c/agent_outcomes.c
    ${AIMEE_SRC_DIR}/modules/db2/c/canonical_index.c
    ${AIMEE_SRC_DIR}/modules/db2/c/anti_patterns.c
    ${AIMEE_SRC_DIR}/modules/db2/c/collab_rules.c
    ${AIMEE_SRC_DIR}/modules/db2/c/curiosity.c
    ${AIMEE_SRC_DIR}/modules/db2/c/decision_log.c
    ${AIMEE_SRC_DIR}/modules/db2/c/enrollments.c
    ${AIMEE_SRC_DIR}/modules/db2/c/db2_tenant.c
    ${AIMEE_SRC_DIR}/modules/db2/c/db2_hardening.c
    ${AIMEE_SRC_DIR}/modules/db2/c/team.c
    ${AIMEE_SRC_DIR}/modules/db2/c/project.c
    ${AIMEE_SRC_DIR}/modules/db2/c/membership.c
    ${AIMEE_SRC_DIR}/modules/db2/c/admin_grant.c
    ${AIMEE_SRC_DIR}/modules/db2/c/oidc_jwks.c
    ${AIMEE_SRC_DIR}/modules/db2/c/entity_edges.c
    ${AIMEE_SRC_DIR}/modules/db2/c/fact_identity.c
    ${AIMEE_SRC_DIR}/modules/db2/c/fact_identity_unicode.c
    ${AIMEE_SRC_DIR}/modules/db2/c/fact_mutation.c
    ${AIMEE_SRC_DIR}/modules/db2/c/evidence_lifecycle.c
    ${AIMEE_SRC_DIR}/modules/db2/c/entity_profiles.c
    ${AIMEE_SRC_DIR}/modules/db2/c/epistemic_directives.c
    ${AIMEE_SRC_DIR}/modules/db2/c/failed_queries.c
    ${AIMEE_SRC_DIR}/modules/db2/c/feedback.c
    ${AIMEE_SRC_DIR}/modules/db2/c/notes.c
    ${AIMEE_SRC_DIR}/modules/db2/c/prospective_memories.c
    ${AIMEE_SRC_DIR}/modules/db2/c/rules.c
    ${AIMEE_SRC_DIR}/modules/db2/c/stopwords.c
    ${AIMEE_SRC_DIR}/modules/db2/c/tasks.c
    ${AIMEE_SRC_DIR}/modules/db2/c/tool_registry.c
    ${AIMEE_SRC_DIR}/modules/db2/c/trace_mining.c
    ${AIMEE_SRC_DIR}/modules/db2/c/kind_lifecycle.c
    ${AIMEE_SRC_DIR}/modules/db2/c/kb_payload.c
    ${AIMEE_SRC_DIR}/modules/db2/c/kb_runtime_state.c
    ${AIMEE_SRC_DIR}/modules/db2/c/kb_service_backend.c
    ${AIMEE_SRC_DIR}/modules/db2/c/kb_service_backend_ingest.c
    ${AIMEE_SRC_DIR}/modules/db2/c/kb_service_backend_context.c
    ${AIMEE_SRC_DIR}/modules/db2/c/memory_briefing.c
    ${AIMEE_SRC_DIR}/modules/db2/c/memory_conflicts.c
    ${AIMEE_SRC_DIR}/modules/db2/c/memory_export.c
    ${AIMEE_SRC_DIR}/modules/db2/c/memory_health.c
    ${AIMEE_SRC_DIR}/modules/db2/c/memory_lifecycle.c
    ${AIMEE_SRC_DIR}/modules/db2/c/memory_payload.c
    ${AIMEE_SRC_DIR}/modules/db2/c/memory_promotion.c
    ${AIMEE_SRC_DIR}/modules/db2/c/memory_query.c
    ${AIMEE_SRC_DIR}/modules/db2/c/memory_query_bookkeeping.c
    ${AIMEE_SRC_DIR}/modules/db2/c/memory_entity_graph.c
    ${AIMEE_SRC_DIR}/modules/db2/c/memory_row_mapper_pg.c
    ${AIMEE_SRC_DIR}/modules/db2/c/memory_score_fields.c
    ${AIMEE_SRC_DIR}/modules/db2/c/memory_scope_query.c
    ${AIMEE_SRC_DIR}/modules/db2/c/memory_relations.c
    ${AIMEE_SRC_DIR}/modules/db2/c/memory_scenes.c
    ${AIMEE_SRC_DIR}/modules/db2/c/vector_index_ops.c
    ${AIMEE_SRC_DIR}/modules/db2/c/code_index_ops.c
    ${AIMEE_SRC_DIR}/modules/db2/c/learning.c
    ${AIMEE_SRC_DIR}/modules/db2/c/code_index.c
    ${AIMEE_SRC_DIR}/modules/db2/c/code_project_lifecycle.c
    ${AIMEE_SRC_DIR}/modules/db2/c/css_graph.c
    ${AIMEE_SRC_DIR}/modules/db2/c/typed_facts.c
    ${AIMEE_SRC_DIR}/modules/db2/c/css_migration.c
    ${AIMEE_SRC_DIR}/modules/db2/c/css_render.c
    ${AIMEE_SRC_DIR}/modules/db2/c/css_insights.c
    ${AIMEE_SRC_DIR}/modules/db2/c/db_schema.c
    ${AIMEE_SRC_DIR}/modules/db2/c/memory_lint.c
    ${AIMEE_SRC_DIR}/modules/db2/c/sketch.c
    ${AIMEE_SRC_DIR}/modules/db2/c/artifacts.c
    ${AIMEE_SRC_DIR}/modules/db2/c/corpus_structural.c
    ${AIMEE_SRC_DIR}/modules/db2/c/corpus_jobs.c
    ${AIMEE_SRC_DIR}/modules/db2/c/curator_terms.c
    ${AIMEE_SRC_DIR}/modules/db2/c/curator_gaps.c
    ${AIMEE_SRC_DIR}/modules/db2/c/cross_repo_resolver.c
    ${AIMEE_SRC_DIR}/modules/db2/c/cross_repo_classify.c
    ${AIMEE_SRC_DIR}/modules/db2/c/cross_repo_stats.c
    ${AIMEE_SRC_DIR}/modules/db2/c/cross_repo_deps.c
    ${AIMEE_SRC_DIR}/modules/db2/c/cross_repo_review.c
    ${AIMEE_SRC_DIR}/modules/db2/c/cross_repo_identity.c
    ${AIMEE_SRC_DIR}/modules/db2/c/cross_repo_route.c
    ${AIMEE_SRC_DIR}/modules/db2/c/cross_repo_build.c
    ${AIMEE_SRC_DIR}/modules/db2/c/mining.c
    ${AIMEE_SRC_DIR}/modules/db2/c/calibration.c
    ${AIMEE_SRC_DIR}/modules/db2/c/demotion.c
    ${AIMEE_SRC_DIR}/modules/db2/c/feature_rows.c
    ${AIMEE_SRC_DIR}/modules/db2/c/report_enrichments.c
    ${AIMEE_SRC_DIR}/modules/db2/c/bandit.c
    ${AIMEE_SRC_DIR}/modules/db2/c/kb_maintenance.c
    ${AIMEE_SRC_DIR}/modules/db2/c/kb_docs.c
    ${AIMEE_SRC_DIR}/modules/db2/c/kb_releases.c
    ${AIMEE_SRC_DIR}/modules/db2/c/pgvec_transport.c
    ${AIMEE_SRC_DIR}/modules/db2/c/memory_vectors.c
    ${AIMEE_SRC_DIR}/modules/db2/c/kb_vectors.c
    ${AIMEE_SRC_DIR}/modules/db2/c/vector_status.c
    ${AIMEE_SRC_DIR}/modules/db2/c/pgvec_verify.c
    ${AIMEE_SRC_DIR}/modules/db2/c/pgvec_kb_service.c
)

set(DB2_HOST_ADAPTER_SRCS
    ${AIMEE_SRC_DIR}/kb/db2_adapters/kb_service_backend_agent.c
    ${AIMEE_SRC_DIR}/kb/db2_adapters/kb_service_backend_export.c
    ${AIMEE_SRC_DIR}/kb/db2_adapters/kb_service_backend_memory.c
    ${AIMEE_SRC_DIR}/kb/db2_adapters/kb_service_backend_runtime.c
)

set(CORE_SRCS
    ${AIMEE_SRC_DIR}/config_client.c
    ${AIMEE_SRC_DIR}/config_client_contract.c
    ${AIMEE_SRC_DIR}/config_client_accessors_0.c
    ${AIMEE_SRC_DIR}/config_client_accessors_1.c
    ${AIMEE_SRC_DIR}/config_client_accessors_2.c
    ${AIMEE_SRC_DIR}/config_client_accessors_3.c
    ${AIMEE_SRC_DIR}/config_client_accessors_4.c
    ${AIMEE_SRC_DIR}/config_client_accessors_5.c
    ${AIMEE_SRC_DIR}/config_client_accessors_6.c
    ${AIMEE_SRC_DIR}/config_client_accessors_7.c
    ${AIMEE_SRC_DIR}/module_json_call.c
    ${AIMEE_SRC_DIR}/session_id.c
    ${AIMEE_SRC_DIR}/modules/db2/c/db_postgres.c
    ${AIMEE_SRC_DIR}/util.c
    ${AIMEE_SRC_DIR}/util_url.c
    ${AIMEE_SRC_DIR}/kb/kb_identity.c
    ${AIMEE_SRC_DIR}/modules/audit/audit_action.c
    ${AIMEE_SRC_DIR}/modules/audit/audit_ledger.c
    ${AIMEE_SRC_DIR}/report_enrichment.c
    ${AIMEE_SRC_DIR}/delivery_target.c
    ${AIMEE_SRC_DIR}/text.c
    ${AIMEE_SRC_DIR}/render.c
    ${AIMEE_SRC_DIR}/markdown.c
    ${AIMEE_SRC_DIR}/json_fluent.c
    ${AIMEE_SRC_DIR}/log.c
    ${AIMEE_SRC_DIR}/shutdown_forensics.c
    ${AIMEE_SRC_DIR}/dstr.c
    ${AIMEE_SRC_DIR}/yaml.c
    ${AIMEE_SRC_DIR}/diff.c
    ${AIMEE_SRC_DIR}/anchor_snapshot.c
    ${AIMEE_SRC_DIR}/edit_anchored.c
    ${AIMEE_SRC_DIR}/code_outline.c
    ${AIMEE_SRC_DIR}/sse_parser.c
    ${AIMEE_SRC_DIR}/aimee_home.c
    ${AIMEE_SRC_DIR}/shared/kb_paths.c
    ${AIMEE_SRC_DIR}/compact.c
    ${AIMEE_SRC_DIR}/platform_random.c
    ${AIMEE_SRC_DIR}/slop_detect.c
    ${AIMEE_SRC_DIR}/proxy_bootstrap.c
    ${AIMEE_SRC_DIR}/client_integrations.c
    ${AIMEE_SRC_DIR}/modules/protocols/mcp/mcp_tools.c
    ${AIMEE_SRC_DIR}/modules/protocols/mcp/mcp_skill_tools.c
    ${AIMEE_SRC_DIR}/modules/protocols/mcp/mcp_tools_gateway.c
    ${AIMEE_SRC_DIR}/modules/protocols/mcp/mcp_client.c
    ${AIMEE_SRC_DIR}/modules/protocols/mcp/mcp_client_registry.c
    ${AIMEE_SRC_DIR}/modules/memory/memory_provider.c
    ${AIMEE_SRC_DIR}/harness_memory_common.c
    ${AIMEE_SRC_DIR}/harness_memory_scope.c
    ${AIMEE_SRC_DIR}/harness_memory_audit.c
    ${AIMEE_SRC_DIR}/harness_memory_spill.c
    ${AIMEE_SRC_DIR}/modules/module-runtime/pre_llm_hook.c
    ${AIMEE_SRC_DIR}/code_collect.c
    ${AIMEE_SRC_DIR}/codex_auth.c
    ${AIMEE_SRC_DIR}/events.c
    ${AIMEE_SRC_DIR}/model_registry.c
    ${AIMEE_SRC_DIR}/models_dev.c
    ${AIMEE_SRC_DIR}/models_dev_cache.c
    ${AIMEE_SRC_DIR}/modules/lsp/lsp_client.c
    ${AIMEE_SRC_DIR}/modules/lsp/lsp_manager.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_iface.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_def.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_validate.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_canonical.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_custom.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_manager_artifacts.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_externalization.c
    ${AIMEE_SRC_DIR}/modules/workflows/tool_egress.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_native_gate.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_enforce.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_deliver.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_advance.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_router.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_router_catalog.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_autonomous_route.c
    ${AIMEE_SRC_DIR}/vendor/cJSON.c
    ${PLATFORM_CORE_SRCS}
)

set(DATA_SRCS
    ${AIMEE_SRC_DIR}/modules/memory/memory_core.c
    ${AIMEE_SRC_DIR}/modules/memory/memory_logic.c
    ${AIMEE_SRC_DIR}/modules/memory/memory_effective.c
    ${AIMEE_SRC_DIR}/modules/memory/memory_advanced.c
    ${AIMEE_SRC_DIR}/modules/memory/memory_activation.c
    ${AIMEE_SRC_DIR}/modules/memory/memory_assemble.c
    ${AIMEE_SRC_DIR}/modules/memory/memory_conflict.c
    ${AIMEE_SRC_DIR}/modules/memory/memory_context.c
    ${AIMEE_SRC_DIR}/modules/memory/memory_directives.c
    ${AIMEE_SRC_DIR}/modules/memory/memory_episodes.c
    ${AIMEE_SRC_DIR}/modules/memory/memory_graph.c
    ${AIMEE_SRC_DIR}/modules/memory/memory_health.c
    ${AIMEE_SRC_DIR}/modules/memory/memory_improve.c
    ${AIMEE_SRC_DIR}/modules/memory/memory_lifecycle.c
    ${AIMEE_SRC_DIR}/modules/memory/memory_maintenance.c
    ${AIMEE_SRC_DIR}/modules/memory/memory_prospective.c
    ${AIMEE_SRC_DIR}/modules/memory/memory_scan.c
    ${AIMEE_SRC_DIR}/trace_analysis.c
    ${AIMEE_SRC_DIR}/index.c
    ${AIMEE_SRC_DIR}/extractors.c
    ${AIMEE_SRC_DIR}/modules/css/css_analyze.c
    ${AIMEE_SRC_DIR}/modules/css/css_render_oracle.c
    ${AIMEE_SRC_DIR}/modules/css/css_render_cmd.c
    ${AIMEE_SRC_DIR}/extractors_extra.c
    ${AIMEE_SRC_DIR}/extractors_new_langs.c
    ${AIMEE_SRC_DIR}/context_discover.c
    ${AIMEE_SRC_DIR}/tasks.c
    ${AIMEE_SRC_DIR}/modules/learning/learning_router.c
    ${AIMEE_SRC_DIR}/modules/learning/learning_implicit.c
    ${AIMEE_SRC_DIR}/dogfood.c
    ${AIMEE_SRC_DIR}/working_profile.c
    ${AIMEE_SRC_DIR}/modules/guardrails/guardrails.c
    ${AIMEE_SRC_DIR}/modules/guardrails/guardrails_orchestrator.c
    ${AIMEE_SRC_DIR}/modules/guardrails/guardrails_action_audit.c
    ${AIMEE_SRC_DIR}/modules/guardrails/guardrails_tdd.c
   _learn.c
    ${AIMEE_SRC_DIR}/session_state.c
    ${AIMEE_SRC_DIR}/session_briefing.c
    ${AIMEE_SRC_DIR}/file_safety.c
    ${AIMEE_SRC_DIR}/branch_ownership.c
    ${AIMEE_SRC_DIR}/integrity_gate.c
    ${AIMEE_SRC_DIR}/conversation_context.c
    ${AIMEE_SRC_DIR}/payload_rewrite.c
    ${AIMEE_SRC_DIR}/modules/guardrails/guardrails_semantic.c
    ${AIMEE_SRC_DIR}/modules/workspace/workspace.c
    ${AIMEE_SRC_DIR}/modules/workspace/workspace_session_lookup.c
    # THE session worktree/branch key, shared with the thin client.
    ${AIMEE_SRC_DIR}/session_worktree_key.c
    ${AIMEE_SRC_DIR}/worktree_gc.c
    ${AIMEE_SRC_DIR}/modules/workspace/workspace_manifest.c
    ${AIMEE_SRC_DIR}/modules/vault/agent_credentials.c
    ${AIMEE_SRC_DIR}/modules/routing/routing.c
    ${AIMEE_SRC_DIR}/agent_tier_lint.c
    ${AIMEE_SRC_DIR}/cmd_describe.c
    ${AIMEE_SRC_DIR}/history.c
    ${AIMEE_SRC_DIR}/role_templates.c
    ${AIMEE_SRC_DIR}/modules/skills/skill.c
    ${AIMEE_SRC_DIR}/modules/skills/skill_rollback.c
    ${AIMEE_SRC_DIR}/conversation.c
    ${AIMEE_SRC_DIR}/trajectory_export.c
    ${AIMEE_SRC_DIR}/trajectory_batch.c
    ${AIMEE_SRC_DIR}/modules/memory/memory_profile_pack.c
    ${AIMEE_SRC_DIR}/wiki_render.c
    ${AIMEE_SRC_DIR}/kb/kb.c
    ${AIMEE_SRC_DIR}/kb/kb_fusion.c
    ${AIMEE_SRC_DIR}/kb/kb_neardup.c
    ${AIMEE_SRC_DIR}/kb/kb_conventions.c
    ${AIMEE_SRC_DIR}/sketch.c
    ${AIMEE_SRC_DIR}/modules/learning/learning_evidence.c
    ${AIMEE_SRC_DIR}/kb/kb_calibrate.c
    ${AIMEE_SRC_DIR}/kb/kb_demote.c
    ${AIMEE_SRC_DIR}/kb/kb_features.c
    ${AIMEE_SRC_DIR}/kb/kb_ranker.c
    ${AIMEE_SRC_DIR}/kb/kb_detect.c
    ${AIMEE_SRC_DIR}/kb/kb_reasoning.c
    ${AIMEE_SRC_DIR}/kb/kb_bandit.c
    ${AIMEE_SRC_DIR}/kb/kb_planner.c
    ${AIMEE_SRC_DIR}/kb/roadmap.c
    ${AIMEE_SRC_DIR}/kb/kb_mdl.c
    ${AIMEE_SRC_DIR}/kb/kb_lab.c
    ${PLATFORM_DATA_SRCS}
)

set(AGENT_SRCS
    ${AIMEE_SRC_DIR}/server/agent_runtime.c
    ${AIMEE_SRC_DIR}/server/agent_parallel.c
    ${AIMEE_SRC_DIR}/server/agent_runtime_liveness.c
    ${AIMEE_SRC_DIR}/server/agent_fallback.c
    ${AIMEE_SRC_DIR}/server/agent_context_budget.c
    ${AIMEE_SRC_DIR}/server/agent_bridge.c
    ${AIMEE_SRC_DIR}/server/tool_call_args.c
    ${AIMEE_SRC_DIR}/server/agent_request_shaping.c
    ${AIMEE_SRC_DIR}/server/model_sampling.c
    ${AIMEE_SRC_DIR}/server/agent_adapter.c
    ${AIMEE_SRC_DIR}/server/primary_session_adapter.c
    ${AIMEE_SRC_DIR}/server/primary_cli_ingestor.c
    ${AIMEE_SRC_DIR}/server/agent_policy.c
    ${AIMEE_SRC_DIR}/server/execution_policy_bus.c
    ${AIMEE_SRC_DIR}/server/agent_tasks.c
    ${AIMEE_SRC_DIR}/modules/benchmarks/agent_eval.c
    ${AIMEE_SRC_DIR}/modules/benchmarks/agent_eval_memory_support.c
    ${AIMEE_SRC_DIR}/modules/benchmarks/agent_eval_baseline.c
    ${AIMEE_SRC_DIR}/modules/benchmarks/agent_eval_benchmarks.c
    ${AIMEE_SRC_DIR}/server/agent_coord.c
    ${AIMEE_SRC_DIR}/server/agent_tools.c
    ${AIMEE_SRC_DIR}/server/computer_use.c
    ${AIMEE_SRC_DIR}/toolset.c
    ${AIMEE_SRC_DIR}/server/tool_args_coerce.c
    ${AIMEE_SRC_DIR}/server/tool_schema_sanitizer.c
    ${AIMEE_SRC_DIR}/server/script_runner.c
    ${AIMEE_SRC_DIR}/server/script_rpc.c
    ${AIMEE_SRC_DIR}/server/osv_check.c
    ${AIMEE_SRC_DIR}/server/failover.c
    ${AIMEE_SRC_DIR}/server/http_retry.c
    ${AIMEE_SRC_DIR}/server/web_search.c
    ${AIMEE_SRC_DIR}/server/token_tracker.c
    ${AIMEE_SRC_DIR}/server/middleware.c
    ${AIMEE_SRC_DIR}/server/agent_pipeline.c
    ${AIMEE_SRC_DIR}/server/process_mgr.c
    ${AIMEE_SRC_DIR}/server/agent_loop.c
    ${AIMEE_SRC_DIR}/server/liveness.c
    ${AIMEE_SRC_DIR}/server/cli_session.c
    ${AIMEE_SRC_DIR}/server/cli_session_pty.c
    ${AIMEE_SRC_DIR}/server/cli_codex.c
    ${AIMEE_SRC_DIR}/server/cli_claude.c

    ${AIMEE_SRC_DIR}/server/cli_mistral.c
    ${AIMEE_SRC_DIR}/server/cli_acp.c
    ${AIMEE_SRC_DIR}/server/session_compact.c
    ${AIMEE_SRC_DIR}/server/compact_prune.c
    ${AIMEE_SRC_DIR}/server/context_engine.c
    ${AIMEE_SRC_DIR}/modules/delegates/delegate_backend.c
    ${AIMEE_SRC_DIR}/modules/delegates/delegate_backend_docker.c
    ${AIMEE_SRC_DIR}/modules/delegates/delegate_driver.c
    ${AIMEE_SRC_DIR}/modules/delegates/delegate_openai.c

    ${AIMEE_SRC_DIR}/modules/delegates/delegate_xml_fallback.c
    ${AIMEE_SRC_DIR}/server/model_provider.c
    ${AIMEE_SRC_DIR}/server/openrouter_profile.c
    ${AIMEE_SRC_DIR}/server/openai_profile.c
    ${AIMEE_SRC_DIR}/server/anthropic_profile.c

    ${AIMEE_SRC_DIR}/server/ollama_profile.c
    ${AIMEE_SRC_DIR}/server/llama_native_profile.c
    ${AIMEE_SRC_DIR}/server/mistral_profile.c
    ${AIMEE_SRC_DIR}/server/minimax_profile.c
    ${AIMEE_SRC_DIR}/server/aux_router.c
    ${AIMEE_SRC_DIR}/server/otel.c
    ${AIMEE_SRC_DIR}/server/oauth_pkce.c
    ${AIMEE_SRC_DIR}/server/oauth_tokens.c
    ${AIMEE_SRC_DIR}/working_profile.c
    ${AIMEE_SRC_DIR}/server/agent_shell.c
    ${AIMEE_SRC_DIR}/server/agent_cli_shell.c
    ${AIMEE_SRC_DIR}/server/provider_cli_adapter.c
    ${PLATFORM_AGENT_SRCS}
)

set(GIT_SRCS
    ${AIMEE_SRC_DIR}/modules/git/mcp_git_query.c
    ${AIMEE_SRC_DIR}/modules/git/mcp_git_write.c
    ${AIMEE_SRC_DIR}/modules/git/mcp_git_integrate.c
    ${AIMEE_SRC_DIR}/modules/git/mcp_git_branch.c
    ${AIMEE_SRC_DIR}/modules/git/mcp_git_pr.c
    ${AIMEE_SRC_DIR}/modules/git/git_verify.c
    ${AIMEE_SRC_DIR}/modules/git/git_verify_state.c
    ${AIMEE_SRC_DIR}/modules/git/git_verify_config.c
    ${AIMEE_SRC_DIR}/modules/git/git_verify_hook.c
    ${AIMEE_SRC_DIR}/modules/git/git_verify_jobs.c
    ${AIMEE_SRC_DIR}/modules/git/git_verify_ops.c
    ${AIMEE_SRC_DIR}/modules/git/git_verify_select.c
    ${AIMEE_SRC_DIR}/modules/git/git_verify_step.c
)

set(CMD_SRCS
    ${AIMEE_SRC_DIR}/prompts.c
    ${AIMEE_SRC_DIR}/cmd_core.c
    ${AIMEE_SRC_DIR}/cmd_init.c
    ${AIMEE_SRC_DIR}/cmd_data.c
    ${AIMEE_SRC_DIR}/cmd_infra.c
    ${AIMEE_SRC_DIR}/cmd_session_history.c
    ${AIMEE_SRC_DIR}/cmd_capabilities.c
    ${AIMEE_SRC_DIR}/cmd_session_lifecycle.c
    ${AIMEE_SRC_DIR}/cmd_ensemble.c
    ${AIMEE_SRC_DIR}/cmd_memory.c
    ${AIMEE_SRC_DIR}/cmd_memory_core.c
    ${AIMEE_SRC_DIR}/cmd_memory_curiosity.c
    ${AIMEE_SRC_DIR}/cmd_memory_embed.c
    ${AIMEE_SRC_DIR}/cmd_memory_vector.c
    ${AIMEE_SRC_DIR}/cmd_index.c
    ${AIMEE_SRC_DIR}/cmd_rules.c
    ${AIMEE_SRC_DIR}/cmd_learning.c
    ${AIMEE_SRC_DIR}/cmd_dogfood.c
    ${AIMEE_SRC_DIR}/cmd_identity.c
    ${AIMEE_SRC_DIR}/cmd_onboard.c
    ${AIMEE_SRC_DIR}/cmd_diagnose.c
    ${AIMEE_SRC_DIR}/cmd_hooks.c
    ${AIMEE_SRC_DIR}/cmd_hooks_scope.c
    ${AIMEE_SRC_DIR}/modules/memory/memory_redirect.c
    ${AIMEE_SRC_DIR}/cmd_agent.c
    ${AIMEE_SRC_DIR}/cmd_agent_setup.c
    ${AIMEE_SRC_DIR}/cmd_agent_trace.c
    ${AIMEE_SRC_DIR}/cmd_agent_delegate.c
    ${AIMEE_SRC_DIR}/cmd_agent_delegate_toolset.c
    ${AIMEE_SRC_DIR}/modules/delegates/delegate_depth.c
    ${AIMEE_SRC_DIR}/modules/delegates/delegate_plan.c
    ${AIMEE_SRC_DIR}/modules/delegates/delegate_prompt.c
    ${AIMEE_SRC_DIR}/modules/delegates/delegate_run_phases.c
    ${AIMEE_SRC_DIR}/modules/delegates/delegate_routing.c
    ${AIMEE_SRC_DIR}/modules/delegates/delegate_role.c
    ${AIMEE_SRC_DIR}/modules/delegates/delegate_economics.c
    ${AIMEE_SRC_DIR}/cmd_wm.c
    ${AIMEE_SRC_DIR}/cmd_cancel.c
    ${AIMEE_SRC_DIR}/cmd_rewind.c
    ${AIMEE_SRC_DIR}/cmd_branch.c
    ${AIMEE_SRC_DIR}/cmd_util.c
    ${AIMEE_SRC_DIR}/cmd_table.c
    ${AIMEE_SRC_DIR}/cmd_doctor.c
    ${AIMEE_SRC_DIR}/cmd_toolset.c
    ${AIMEE_SRC_DIR}/cmd_slop.c
    ${AIMEE_SRC_DIR}/cmd_job.c
    ${AIMEE_SRC_DIR}/cmd_roles.c
    ${AIMEE_SRC_DIR}/cmd_skill.c
    ${AIMEE_SRC_DIR}/cmd_autopilot.c
    ${AIMEE_SRC_DIR}/cmd_run.c
    ${AIMEE_SRC_DIR}/cmd_kb.c
    ${AIMEE_SRC_DIR}/hud.c
    ${AIMEE_SRC_DIR}/turn_narration.c
    ${AIMEE_SRC_DIR}/dashboard.c
    ${AIMEE_SRC_DIR}/aimee_client.c
    ${PLATFORM_CMD_SRCS}
)
# The transport's local Unix-socket path (http_uds_client.c) is POSIX-only;
# aimee_client.c only references it under AIMEE_POSIX, so Windows omits it.
if(NOT WIN32)
    list(APPEND CMD_SRCS ${AIMEE_SRC_DIR}/http_uds_client.c)
endif()
if(NOT WITH_UI)
    list(APPEND CMD_SRCS ${AIMEE_SRC_DIR}/dashboard_stub.c)
endif()

set(CLI_SRCS
    ${AIMEE_SRC_DIR}/cli_main.c
    ${AIMEE_SRC_DIR}/cli_agent_setup.c
    ${AIMEE_SRC_DIR}/cli_session_start.c
    # cli_session_start renders the degraded-dependency notice from here; the
    # CLI links it directly, so it must be listed in BOTH build systems.
    ${AIMEE_SRC_DIR}/session_degraded_notice.c
    # Thin-client self-update (cli_main dispatch + cli_session_start drift notice
    # both call these; the CLI does not link aimee-cmd, so they build in here).
    ${AIMEE_SRC_DIR}/cmd_self_update.c
    ${AIMEE_SRC_DIR}/self_update_util.c
    # memory_redirect_check (the CLI's remote-only hooks-pre interception) calls
    # hmem_resolve_project/_scope_for_client/_audit/_spill; the standalone `aimee`
    # CLI does not link aimee-core, so their definers must build into the CLI
    # directly. Without these the macOS/Windows CMake link fails undefined hmem_*.
    ${AIMEE_SRC_DIR}/harness_memory_common.c
    ${AIMEE_SRC_DIR}/harness_memory_scope.c
    # cli_main's remote-only hooks-pre path calls memory_redirect_check; the CLI
    # doesn't link aimee-core, so its definer must build in directly (make links it
    # via CLIENT_SUPPORT_OBJS). Without it the CLI link fails undefined.
    ${AIMEE_SRC_DIR}/modules/memory/memory_redirect.c
    ${AIMEE_SRC_DIR}/harness_memory_audit.c
    ${AIMEE_SRC_DIR}/harness_memory_spill.c
    ${AIMEE_SRC_DIR}/cli_attention_guard.c
    # Per-session branch+worktree bootstrap, shared by the SessionStart hook
    # (cli_session_start.c) and the MCP proxy (cli_mcp_serve.c). The client links
    # no workspace.o, so this is its own git-subprocess implementation.
    ${AIMEE_SRC_DIR}/client_session_worktree.c
    # THE session worktree/branch key. Shared with aimee-core so the client and
    # the server agree on where a session's worktree is.
    ${AIMEE_SRC_DIR}/session_worktree_key.c
    ${AIMEE_SRC_DIR}/cli_code_audit.c
    ${AIMEE_SRC_DIR}/cli_css.c
    ${AIMEE_SRC_DIR}/cli_chat_stream.c
    ${AIMEE_SRC_DIR}/cli_client.c
    ${AIMEE_SRC_DIR}/cli_kb_smoke.c
    ${AIMEE_SRC_DIR}/cli_v1_routes.c
    ${AIMEE_SRC_DIR}/cli_v1_routes_d.c
    ${AIMEE_SRC_DIR}/cli_v1_routes_e.c
    ${AIMEE_SRC_DIR}/cli_v1_routes_c.c
    ${AIMEE_SRC_DIR}/cli_v1_routes_b.c
    ${AIMEE_SRC_DIR}/cli_v1_routes_f.c
    ${AIMEE_SRC_DIR}/cli_argspec.c
    ${AIMEE_SRC_DIR}/cli_index_bootstrap.c
    ${AIMEE_SRC_DIR}/cli_launch.c
    ${AIMEE_SRC_DIR}/cli_mcp_serve.c
    # handle_tools_list calls mcp_tool_prose_lean/mcp_compact_tool_prose to build
    # the lean catalog. Same rule as memory_redirect.c above: the client does not
    # link aimee-core, so the definer must build in directly (make links it via
    # CLIENT_SUPPORT_OBJS). Without it the CLI link fails undefined on Windows.
    ${AIMEE_SRC_DIR}/modules/protocols/mcp/mcp_tool_profile.c
    ${AIMEE_SRC_DIR}/cli_profile.c
    ${AIMEE_SRC_DIR}/client_integrations.c
    # Thin-client config reads and structured profile mutations route through
    # the server; this is the caller-only dispatcher, not a config engine.
    ${AIMEE_SRC_DIR}/client_config.c
    ${AIMEE_SRC_DIR}/cmd_profile.c
    ${AIMEE_SRC_DIR}/cmd_manuscript.c
    ${AIMEE_SRC_DIR}/cmd_optimize.c
    ${AIMEE_SRC_DIR}/manuscript.c
    ${AIMEE_SRC_DIR}/cli_persona.c
    ${AIMEE_SRC_DIR}/cli_roles.c
    ${AIMEE_SRC_DIR}/modules/workspace/cli_workspace_serve.c
    ${AIMEE_SRC_DIR}/modules/workspace/workspace_client_diff.c
    ${AIMEE_SRC_DIR}/cli_agent_keys.c
    ${AIMEE_SRC_DIR}/cli_remote.c
    ${AIMEE_SRC_DIR}/modules/delegates/delegate_plan.c
    ${AIMEE_SRC_DIR}/modules/protocols/acp/acp_server.c
    ${AIMEE_SRC_DIR}/cmd_workflow.c
)
# `workspace serve` + the reverse-channel host the detached-workspace runner so
# the client serves its working tree to a remote aimee-server. It now builds on
# Windows too (cli_workspace_serve.c is native-threaded via _beginthreadex), so
# it is compiled on every platform — no per-OS exclusion.

set(SERVER_SRCS
    ${AIMEE_SRC_DIR}/server/server_main.c
    ${AIMEE_SRC_DIR}/server/obs_bus_adapter.c
    ${AIMEE_SRC_DIR}/server/module_routing_adapter.c
    ${AIMEE_SRC_DIR}/server/vault_bootstrap_privilege.c
    ${AIMEE_SRC_DIR}/server/server.c
    ${AIMEE_SRC_DIR}/server/s2_native_gate_hook.c
    ${AIMEE_SRC_DIR}/server_provider.c
    ${AIMEE_SRC_DIR}/server_insights.c
    ${AIMEE_SRC_DIR}/server_eval.c
    ${AIMEE_SRC_DIR}/server/server_api_status.c
    server/server_error_kind.c
    ${AIMEE_SRC_DIR}/server/server_seed_config.c
    ${AIMEE_SRC_DIR}/server/server_auth.c
    ${AIMEE_SRC_DIR}/server/server_hooks.c
    ${AIMEE_SRC_DIR}/server/server_session.c
    ${AIMEE_SRC_DIR}/server/server_session_pools.c
    ${AIMEE_SRC_DIR}/server/presence.c
    ${AIMEE_SRC_DIR}/server/turn_registry.c
    ${AIMEE_SRC_DIR}/server/wfe_live_delegate.c
    ${AIMEE_SRC_DIR}/server/workflow_control_bus.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_scheduler.c
    ${AIMEE_SRC_DIR}/modules/vault/vault_principal.c
    ${AIMEE_SRC_DIR}/modules/vault/vault_crypto.c
    ${AIMEE_SRC_DIR}/modules/vault/vault_kek_cache.c
    ${AIMEE_SRC_DIR}/modules/vault/vault_store.c
    ${AIMEE_SRC_DIR}/modules/vault/vault_service.c
    ${AIMEE_SRC_DIR}/modules/vault/vault_capability.c
    ${AIMEE_SRC_DIR}/server/server_vault.c
    ${AIMEE_SRC_DIR}/server/server_state.c
    ${AIMEE_SRC_DIR}/server/server_state_freshness.c
    ${AIMEE_SRC_DIR}/server/server_audit_replay_routes.c
    ${AIMEE_SRC_DIR}/server/vault_audit_bridge.c
    ${AIMEE_SRC_DIR}/server/sandbox_audit_bridge.c
    ${AIMEE_SRC_DIR}/server/memory_audit_bridge.c
    ${AIMEE_SRC_DIR}/server/server_runner_endpoints.c
    ${AIMEE_SRC_DIR}/server/workspace_register_args.c
    ${AIMEE_SRC_DIR}/server/server_state_audit.c
    ${AIMEE_SRC_DIR}/server/server_css_query.c
    ${AIMEE_SRC_DIR}/server/server_runner_endpoints.c
    ${AIMEE_SRC_DIR}/server/server_state_audit.c
    ${AIMEE_SRC_DIR}/server/server_css_query.c
    ${AIMEE_SRC_DIR}/server/server_state.c
    ${AIMEE_SRC_DIR}/server/server_state_freshness.c
    ${AIMEE_SRC_DIR}/server/server_memory_review.c
    ${AIMEE_SRC_DIR}/server/server_audit_replay_routes.c
    ${AIMEE_SRC_DIR}/server/vault_audit_bridge.c
    ${AIMEE_SRC_DIR}/server/sandbox_audit_bridge.c
    ${AIMEE_SRC_DIR}/server/memory_audit_bridge.c
    ${AIMEE_SRC_DIR}/server/server_runner_endpoints.c
    ${AIMEE_SRC_DIR}/server/server_state_audit.c
    ${AIMEE_SRC_DIR}/server/server_css_query.c
    ${AIMEE_SRC_DIR}/server/server_runner_endpoints.c
    ${AIMEE_SRC_DIR}/server/server_state_audit.c
    ${AIMEE_SRC_DIR}/server/server_css_query.c
    ${AIMEE_SRC_DIR}/server/server_state_index.c
    ${AIMEE_SRC_DIR}/server/server_active_project.c
    ${AIMEE_SRC_DIR}/server/server_delegate_monitor.c
    ${AIMEE_SRC_DIR}/server/server_coord_dispatcher.c
    ${AIMEE_SRC_DIR}/server/server_agent.c
    ${AIMEE_SRC_DIR}/server/server_compute.c
    ${AIMEE_SRC_DIR}/server/server_compute_roundtable.c
    ${AIMEE_SRC_DIR}/server/server_compute_mailbox.c
    ${AIMEE_SRC_DIR}/server_compute_episodes.c
    ${AIMEE_SRC_DIR}/server/server_delegate_ondemand.c
    ${AIMEE_SRC_DIR}/server/server_delegate_status.c
    ${AIMEE_SRC_DIR}/server/server_compute_async.c
    ${AIMEE_SRC_DIR}/server/server_jobs_aux.c
    ${AIMEE_SRC_DIR}/server/server_skill.c
    ${AIMEE_SRC_DIR}/server/server_skill_jobs.c
    ${AIMEE_SRC_DIR}/server/server_mcp.c
    ${AIMEE_SRC_DIR}/server/server_mcp_call_table.c
    ${AIMEE_SRC_DIR}/server/server_mcp_ast_grep.c
    ${AIMEE_SRC_DIR}/server/server_mcp_tools.c
    ${AIMEE_SRC_DIR}/server/server_mcp_delegate.c
    ${AIMEE_SRC_DIR}/server/server_mcp_process.c
    ${AIMEE_SRC_DIR}/server/server_mcp_skill.c
    ${AIMEE_SRC_DIR}/server/server_mcp_learning.c
    ${AIMEE_SRC_DIR}/server/server_mcp_ensemble.c
    ${AIMEE_SRC_DIR}/server/server_workflow_api.c
    ${AIMEE_SRC_DIR}/server/server_mcp_gateway.c
    ${AIMEE_SRC_DIR}/server/session_search_tool.c
    ${AIMEE_SRC_DIR}/server/compute_pool.c
    ${AIMEE_SRC_DIR}/cli_client.c
    ${AIMEE_SRC_DIR}/cli_v1_routes.c
    ${AIMEE_SRC_DIR}/cli_v1_routes_d.c
    ${AIMEE_SRC_DIR}/cli_v1_routes_e.c
    ${AIMEE_SRC_DIR}/cli_v1_routes_c.c
    ${AIMEE_SRC_DIR}/cli_v1_routes_b.c
    ${AIMEE_SRC_DIR}/cli_v1_routes_f.c
    ${AIMEE_SRC_DIR}/cli_argspec.c
    ${AIMEE_SRC_DIR}/server/agent_admission.c
    ${AIMEE_SRC_DIR}/server/provider_catalog.c
    ${AIMEE_SRC_DIR}/server/dashboard_server.c
    ${AIMEE_SRC_DIR}/hud.c
    ${AIMEE_SRC_DIR}/cmd_identity.c
    ${AIMEE_SRC_DIR}/prompts.c
    ${AIMEE_SRC_DIR}/working_profile.c
    ${AIMEE_SRC_DIR}/modules/delegates/delegate_role.c
    ${AIMEE_SRC_DIR}/modules/delegates/delegate_depth.c
    ${AIMEE_SRC_DIR}/modules/delegates/delegate_prompt.c
    ${AIMEE_SRC_DIR}/modules/delegates/delegate_run_phases.c
    ${AIMEE_SRC_DIR}/modules/delegates/delegate_routing.c
    ${AIMEE_SRC_DIR}/modules/delegates/delegate_launch.c
    ${AIMEE_SRC_DIR}/modules/delegates/delegate_source_authority.c
    ${AIMEE_SRC_DIR}/modules/delegates/delegate_economics.c
    ${AIMEE_SRC_DIR}/modules/delegates/delegate_credentials.c
    ${AIMEE_SRC_DIR}/modules/delegates/delegate_credential_retry.c
    ${AIMEE_SRC_DIR}/modules/delegates/delegate_patch_coordinator.c
    ${AIMEE_SRC_DIR}/modules/delegates/delegate_checkout.c
    ${AIMEE_SRC_DIR}/cmd_init.c
    ${AIMEE_SRC_DIR}/server/trigger_scheduler.c
    ${AIMEE_SRC_DIR}/server/server_trigger.c
    ${AIMEE_SRC_DIR}/server/server_cron.c
    ${AIMEE_SRC_DIR}/server/server_trajectory.c
    ${AIMEE_SRC_DIR}/modules/kb_client/kb_client.c
    ${AIMEE_SRC_DIR}/modules/kb_client/kb_client_cache.c
    ${AIMEE_SRC_DIR}/modules/kb_client/kb_client_ws.c
    ${AIMEE_SRC_DIR}/modules/kb_client/kb_client_docs.c
    ${AIMEE_SRC_DIR}/kb/kb_doc_hash.c
    ${AIMEE_SRC_DIR}/modules/kb_client/kb_client_index.c
    ${AIMEE_SRC_DIR}/modules/kb_client/kb_client_index_parse.c
    ${AIMEE_SRC_DIR}/modules/kb_client/kb_client_memory.c
    ${AIMEE_SRC_DIR}/modules/kb_client/kb_client_memory_mutations.c
    ${AIMEE_SRC_DIR}/modules/kb_client/kb_client_memory_audit.c
    ${AIMEE_SRC_DIR}/modules/kb_client/kb_client_agent.c
    ${AIMEE_SRC_DIR}/modules/kb_client/kb_client_dashboard.c
    ${AIMEE_SRC_DIR}/modules/kb_client/kb_client_tasks.c
    ${AIMEE_SRC_DIR}/modules/kb_client/kb_client_data.c
    ${AIMEE_SRC_DIR}/modules/kb_client/kb_client_tool_registry.c
    ${AIMEE_SRC_DIR}/modules/kb_client/kb_client_prospective.c
    ${AIMEE_SRC_DIR}/cmd_session_lifecycle.c
    ${AIMEE_SRC_DIR}/cmd_hooks_scope.c
    ${AIMEE_SRC_DIR}/server/server_session_start_stubs.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_engine.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_advance_exec.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_block_resolve.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_bind_ingress.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_blocks.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_approval.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_verdict.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_roundtable.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_autonomy.c
    ${PLATFORM_SERVER_SRCS}
)
set(SERVER_DATA_SRCS ${DATA_SRCS})
list(REMOVE_ITEM SERVER_DATA_SRCS ${SERVER_SRCS} ${AIMEE_SRC_DIR}/modules/guardrails/guardrails_orchestrator.c)
list(APPEND SERVER_DATA_SRCS ${AIMEE_SRC_DIR}/modules/guardrails/guardrails_orchestrator.c)
set(SERVER_CORE_SRCS ${CORE_SRCS})
list(REMOVE_ITEM SERVER_CORE_SRCS ${AIMEE_SRC_DIR}/modules/db2/c/db_postgres.c)

set(KB_SRCS
    ${AIMEE_SRC_DIR}/kb/kb_main.c
    ${AIMEE_SRC_DIR}/kb/kb_obs_bus_adapter.c
    ${AIMEE_SRC_DIR}/kb/kb_background.c
    ${AIMEE_SRC_DIR}/kb/kb_service.c
    ${AIMEE_SRC_DIR}/kb/kb_service_workers.c
    ${AIMEE_SRC_DIR}/kb/kb_ingest_workers.c
    # kb_ingest_workers reconciles the webuser clone tree through
    # ws_scope_webusers_base(); the kb target must link it.
    ${AIMEE_SRC_DIR}/modules/workspace/workspace_scope.c
    ${AIMEE_SRC_DIR}/kb/kb_service_kb.c
    ${AIMEE_SRC_DIR}/kb/kb_identity_resolve.c
    ${AIMEE_SRC_DIR}/kb/kb_oidc_jwks_fleet.c
    ${AIMEE_SRC_DIR}/kb/kb_reqctx.c
    ${AIMEE_SRC_DIR}/kb/kb_service_memory.c
    ${AIMEE_SRC_DIR}/kb/kb_service_css.c
    ${AIMEE_SRC_DIR}/kb/kb_service_agent.c
    ${AIMEE_SRC_DIR}/kb/kb_service_artifacts.c
    ${AIMEE_SRC_DIR}/kb/kb_intel_payload.c
    ${AIMEE_SRC_DIR}/kb/kb_reflection.c
    ${AIMEE_SRC_DIR}/modules/kb-synthesis/kb_curator_contradictions.c
    ${AIMEE_SRC_DIR}/modules/kb-synthesis/kb_curator_custom.c
    ${AIMEE_SRC_DIR}/modules/kb-synthesis/kb_curator_drain.c
    ${AIMEE_SRC_DIR}/modules/kb-synthesis/kb_curator_extract.c
    ${AIMEE_SRC_DIR}/modules/kb-synthesis/kb_curator_extract_code.c
    ${AIMEE_SRC_DIR}/modules/kb-synthesis/kb_curator_grounding.c
    ${AIMEE_SRC_DIR}/modules/kb-synthesis/kb_curator_index_claims.c
    ${AIMEE_SRC_DIR}/modules/kb-synthesis/kb_curator_index_code_unit.c
    ${AIMEE_SRC_DIR}/modules/kb-synthesis/kb_curator_index_narrative.c
    ${AIMEE_SRC_DIR}/modules/kb-synthesis/kb_curator_judge.c
    ${AIMEE_SRC_DIR}/modules/kb-synthesis/kb_curator_link_artifacts.c
    ${AIMEE_SRC_DIR}/modules/kb-synthesis/kb_curator_llm.c
    ${AIMEE_SRC_DIR}/modules/kb-synthesis/kb_curator_notify.c
    ${AIMEE_SRC_DIR}/modules/kb-synthesis/kb_curator_pipeline.c
    ${AIMEE_SRC_DIR}/modules/kb-synthesis/kb_curator_promote.c
    ${AIMEE_SRC_DIR}/modules/kb-synthesis/kb_curator_queue.c
    ${AIMEE_SRC_DIR}/modules/kb-synthesis/kb_curator_resolve_entities.c
    ${AIMEE_SRC_DIR}/modules/kb-synthesis/kb_curator_serve.c
    ${AIMEE_SRC_DIR}/modules/kb-synthesis/kb_curator_sidecar.c
    ${AIMEE_SRC_DIR}/modules/kb-synthesis/kb_curator_synthesize.c
    ${AIMEE_SRC_DIR}/modules/kb-synthesis/kb_curator_version.c
    ${AIMEE_SRC_DIR}/kb/memory_rewrite_llm.c
    ${AIMEE_SRC_DIR}/kb/kb_mining.c
    ${AIMEE_SRC_DIR}/kb/kb_workload_provider.c
    ${AIMEE_SRC_DIR}/kb/kb_workload_helper_posix.c
    ${AIMEE_SRC_DIR}/kb/kb_workload_wire.c
    ${AIMEE_SRC_DIR}/kb/kb_workload_proof.c
    ${AIMEE_SRC_DIR}/kb/kb_workload_jwt.c
    ${AIMEE_SRC_DIR}/kb/kb_management_cert_codec.c
    ${AIMEE_SRC_DIR}/kb/kb_management_cert_crypto.c
    ${AIMEE_SRC_DIR}/kb/kb_management_cert_binding.c
    ${AIMEE_SRC_DIR}/kb/kb_management_cert_storage.c
    ${AIMEE_SRC_DIR}/kb/kb_management_cert_lifecycle.c
    ${AIMEE_SRC_DIR}/kb/http/kb_http.c
    ${AIMEE_SRC_DIR}/kb/http/kb_metrics_listener.c
    ${AIMEE_SRC_DIR}/kb/http/kb_http_content_policy.c
    ${AIMEE_SRC_DIR}/kb/http/kb_http_entry.c
    # kb_http.c reads request bodies through these scanners; both build systems
    # must compile them or the kb target links an undefined symbol.
    ${AIMEE_SRC_DIR}/kb/http/kb_http_json.c
    ${AIMEE_SRC_DIR}/kb/http/kb_ingress.c
    ${AIMEE_SRC_DIR}/kb/http/kb_http_team.c
    ${AIMEE_SRC_DIR}/kb/http/kb_route_acl.c
    ${AIMEE_SRC_DIR}/kb/http/kb_http_console.c
    ${AIMEE_SRC_DIR}/kb/http/kb_http_accounts.c
    ${AIMEE_SRC_DIR}/kb/http/kb_http_governance.c
    ${AIMEE_SRC_DIR}/kb/http/kb_http_code.c
    ${AIMEE_SRC_DIR}/kb/http/kb_http_code_scan_phase.c
    ${AIMEE_SRC_DIR}/kb/http/kb_http_ingest.c
    ${AIMEE_SRC_DIR}/kb/http/kb_http_jobs.c
    ${AIMEE_SRC_DIR}/kb/http/kb_http_ws.c
    ${AIMEE_SRC_DIR}/kb/kb_doc_hash.c
    ${AIMEE_SRC_DIR}/kb/http/kb_http_releases.c
    ${AIMEE_SRC_DIR}/kb/http/kb_http_reflections.c
    ${AIMEE_SRC_DIR}/kb/kb_ingest_normalize.c
    ${AIMEE_SRC_DIR}/kb/kb_memory_embed.c
    ${AIMEE_SRC_DIR}/kb/fact_grounding.c
)
set(KB_DATA_SRCS
    ${AIMEE_SRC_DIR}/modules/memory/memory_core.c
    ${AIMEE_SRC_DIR}/modules/memory/memory_logic.c
    ${AIMEE_SRC_DIR}/modules/memory/memory_effective.c
    ${AIMEE_SRC_DIR}/modules/memory/memory_health.c
    ${AIMEE_SRC_DIR}/modules/memory/memory_conflict.c
    ${AIMEE_SRC_DIR}/modules/memory/memory_context.c
    ${AIMEE_SRC_DIR}/modules/memory/memory_assemble.c
    ${AIMEE_SRC_DIR}/modules/memory/memory_advanced.c
    ${AIMEE_SRC_DIR}/modules/memory/memory_prospective.c
    ${AIMEE_SRC_DIR}/modules/memory/memory_lifecycle.c
    ${AIMEE_SRC_DIR}/modules/memory/memory_directives.c
    ${AIMEE_SRC_DIR}/modules/memory/memory_maintenance.c
    ${AIMEE_SRC_DIR}/modules/memory/memory_graph.c
    ${AIMEE_SRC_DIR}/modules/memory/memory_scan.c
    ${AIMEE_SRC_DIR}/modules/memory/memory_improve.c
    ${AIMEE_SRC_DIR}/modules/memory/memory_episodes.c
    ${AIMEE_SRC_DIR}/index.c
    ${AIMEE_SRC_DIR}/extractors.c
    ${AIMEE_SRC_DIR}/modules/css/css_analyze.c
    ${AIMEE_SRC_DIR}/modules/css/css_render_oracle.c
    ${AIMEE_SRC_DIR}/modules/css/css_render_cmd.c
    ${AIMEE_SRC_DIR}/extractors_extra.c
    ${AIMEE_SRC_DIR}/extractors_new_langs.c
    ${AIMEE_SRC_DIR}/dogfood.c
    ${AIMEE_SRC_DIR}/modules/learning/learning_router.c
    ${AIMEE_SRC_DIR}/modules/learning/learning_implicit.c
    ${AIMEE_SRC_DIR}/integrity_gate.c
    ${AIMEE_SRC_DIR}/session_briefing.c
    ${AIMEE_SRC_DIR}/modules/workspace/workspace.c
    ${AIMEE_SRC_DIR}/modules/workspace/workspace_session_lookup.c
    ${AIMEE_SRC_DIR}/modules/workspace/workspace_manifest.c
    ${AIMEE_SRC_DIR}/modules/memory/memory_profile_pack.c
    ${AIMEE_SRC_DIR}/wiki_render.c
    ${AIMEE_SRC_DIR}/kb/kb.c
    ${AIMEE_SRC_DIR}/kb/kb_fusion.c
    ${AIMEE_SRC_DIR}/kb/kb_neardup.c
    ${AIMEE_SRC_DIR}/kb/kb_conventions.c
    ${AIMEE_SRC_DIR}/sketch.c
    ${AIMEE_SRC_DIR}/modules/learning/learning_evidence.c
    ${AIMEE_SRC_DIR}/kb/kb_calibrate.c
    ${AIMEE_SRC_DIR}/kb/kb_demote.c
    ${AIMEE_SRC_DIR}/kb/kb_features.c
    ${AIMEE_SRC_DIR}/kb/kb_ranker.c
    ${AIMEE_SRC_DIR}/kb/kb_detect.c
    ${AIMEE_SRC_DIR}/kb/kb_reasoning.c
    ${AIMEE_SRC_DIR}/kb/kb_bandit.c
    ${AIMEE_SRC_DIR}/kb/kb_planner.c
    ${AIMEE_SRC_DIR}/kb/roadmap.c
    ${AIMEE_SRC_DIR}/kb/kb_mdl.c
    ${AIMEE_SRC_DIR}/kb/kb_lab.c
)
set(KB_CORE_SRCS ${CORE_SRCS})
list(REMOVE_ITEM KB_CORE_SRCS
)

find_package(Threads REQUIRED)
# On Windows the HTTP layer uses WinHTTP; OpenSSL is only required on POSIX.
if(WIN32)
    find_package(OpenSSL)
else()
    find_package(OpenSSL REQUIRED)
endif()

# --- Full-build static libraries + their native dependencies ----------------
# The thin `aimee` CLI target (defined below) links none of these; it needs
# only Threads (+ OpenSSL for https). Skipping them under -DAIMEE_THIN_CLIENT=ON
# means a packaging box needs no SQLite/libpq/zstd/PAM/libsecret toolchain.
if(NOT AIMEE_THIN_CLIENT)

set(AIMEE_PROJECT_INCLUDE_DIRS
    ${AIMEE_SRC_DIR}
    ${AIMEE_SRC_DIR}/headers
    ${AIMEE_SRC_DIR}/modules/economizer
    ${AIMEE_SRC_DIR}/modules/governance
    ${AIMEE_SRC_DIR}/modules/workflows
    ${AIMEE_SRC_DIR}/modules/vault
    ${AIMEE_SRC_DIR}/modules/guardrails
    ${AIMEE_SRC_DIR}/modules/roundtable
    ${AIMEE_SRC_DIR}/modules/kb_client
    ${AIMEE_SRC_DIR}/modules/agent_eval
    ${AIMEE_SRC_DIR}/modules/roadmap
    ${AIMEE_SRC_DIR}/modules/sandbox
    ${AIMEE_SRC_DIR}/modules/lsp
    ${AIMEE_SRC_DIR}/modules/skill
    ${AIMEE_SRC_DIR}/modules/css
    ${AIMEE_SRC_DIR}/modules/webuser
    ${AIMEE_SRC_DIR}/pipeline
    ${AIMEE_SRC_DIR}/vendor/headers)

add_library(aimee-core STATIC
    ${CORE_SRCS}
    ${DB1_SRCS}
    ${DB2_SRCS}
    ${DB2_HOST_ADAPTER_SRCS}
)
target_include_directories(aimee-core PUBLIC
    ${AIMEE_PROJECT_INCLUDE_DIRS}
    ${AIMEE_SRC_DIR}/db1_client
    ${AIMEE_SRC_DIR}/modules/db2/c
)
target_compile_options(aimee-core PRIVATE ${AIMEE_COMMON_WARNINGS})
target_link_libraries(aimee-core PUBLIC Threads::Threads)

add_library(aimee-data STATIC ${DATA_SRCS})
target_include_directories(aimee-data PUBLIC ${AIMEE_PROJECT_INCLUDE_DIRS})
target_compile_options(aimee-data PRIVATE ${AIMEE_COMMON_WARNINGS})
target_link_libraries(aimee-data PUBLIC aimee-core ${ZSTD_LIBRARY})

add_library(aimee-agent STATIC ${AGENT_SRCS})
target_include_directories(aimee-agent PUBLIC ${AIMEE_PROJECT_INCLUDE_DIRS})
target_compile_options(aimee-agent PRIVATE ${AIMEE_COMMON_WARNINGS})
if(OpenSSL_FOUND OR OPENSSL_FOUND)
    target_link_libraries(aimee-agent PUBLIC aimee-data aimee-core OpenSSL::SSL OpenSSL::Crypto)
else()
    target_link_libraries(aimee-agent PUBLIC aimee-data aimee-core)
endif()

add_library(aimee-git STATIC ${GIT_SRCS})
target_include_directories(aimee-git PUBLIC ${AIMEE_PROJECT_INCLUDE_DIRS})
target_compile_options(aimee-git PRIVATE ${AIMEE_COMMON_WARNINGS})
target_link_libraries(aimee-git PUBLIC aimee-agent aimee-data aimee-core)

add_library(aimee-cmd STATIC ${CMD_SRCS})
target_include_directories(aimee-cmd PUBLIC ${AIMEE_PROJECT_INCLUDE_DIRS})
target_compile_options(aimee-cmd PRIVATE ${AIMEE_COMMON_WARNINGS})
target_link_libraries(aimee-cmd PUBLIC aimee-git aimee-agent aimee-data aimee-core)
if(WITH_UI)
    target_compile_definitions(aimee-cmd PUBLIC WITH_UI)
endif()

if(WIN32)
    set(WITH_PAM OFF CACHE BOOL "Enable PAM authentication support" FORCE)
    set(WITH_LIBSECRET OFF CACHE BOOL "Enable libsecret integration" FORCE)
    target_compile_definitions(aimee-core PUBLIC AIMEE_WINDOWS _WIN32_WINNT=0x0A00)
    set(SQLITE3_AMALGAMATION "${AIMEE_SRC_DIR}/vendor/sqlite3/sqlite3.c")
    if(EXISTS "${SQLITE3_AMALGAMATION}")
        # Use vendored amalgamation (preferred for reproducible Windows builds)
        target_sources(aimee-core PRIVATE "${SQLITE3_AMALGAMATION}")
        # Suppress warnings for the third-party amalgamation (it is not our code)
        set_source_files_properties("${SQLITE3_AMALGAMATION}" PROPERTIES
            COMPILE_FLAGS "-w")
        target_include_directories(aimee-core PUBLIC ${AIMEE_SRC_DIR}/vendor/sqlite3)
    else()
        # Fall back to system SQLite (e.g. installed via package manager)
        find_package(SQLite3 REQUIRED)
        target_link_libraries(aimee-core PUBLIC SQLite::SQLite3)
        message(STATUS "SQLite amalgamation not found; using system SQLite3")
    endif()
    target_link_libraries(aimee-core PUBLIC ws2_32 bcrypt winhttp)
else()
    find_package(SQLite3 REQUIRED)
    target_link_libraries(aimee-core PUBLIC SQLite::SQLite3 m)
endif()

# --- DB2 client (libpq) ---
# DB2 is a required storage tier on POSIX builds; pkg-config discovers the
# client flags and the build fails loudly if they are missing.
# See docs/proposals/accepted/three-db-split-user-shared-vectors.md and
# docs/proposals/pending/three-db-pin-backends.md.
#
# Platform notes:
#  - Linux/macOS: pkg-config locates libpq via the distro package.
#  - Windows (MinGW/MSVC): libpq isn't reliably discoverable through
#    pkg-config, and the MinGW CI job builds without that client toolchain
#    on the runner. DB2 is disabled with AIMEE_DISABLE_POSTGRES when libpq
#    is not found there; the DB1 local tier remains available.
find_package(PkgConfig QUIET)
set(AIMEE_HAVE_LIBPQ OFF)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBPQ libpq)
    if(LIBPQ_FOUND)
        set(AIMEE_HAVE_LIBPQ ON)
    endif()
endif()
if(AIMEE_HAVE_LIBPQ)
    target_include_directories(aimee-core PUBLIC ${LIBPQ_INCLUDE_DIRS})
    target_link_directories(aimee-core PUBLIC ${LIBPQ_LIBRARY_DIRS})
    target_link_libraries(aimee-core PUBLIC ${LIBPQ_LIBRARIES})
    target_compile_options(aimee-core PRIVATE ${LIBPQ_CFLAGS_OTHER})
else()
    if(UNIX)
        message(FATAL_ERROR
            "libpq not found. Install libpq-dev (Debian/Ubuntu), "
            "libpq-devel (Fedora/RHEL), or libpq (Homebrew).")
    endif()
    message(STATUS "libpq not found; DB2 client disabled for this build.")
    target_compile_definitions(aimee-core PUBLIC AIMEE_DISABLE_POSTGRES)
endif()

if(APPLE)
    find_library(SECURITY_FRAMEWORK Security REQUIRED)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
endif()

# zstd backs MDL-guided synthesis scoring (src/kb/kb_mdl.c). Required on UNIX
# (libzstd is a standard package); optional on the MinGW/Windows portable build,
# which has no libzstd — there kb_mdl.c falls back to a length-based size proxy.
find_library(ZSTD_LIBRARY NAMES zstd libzstd)
if(NOT ZSTD_LIBRARY)
    if(UNIX)
        message(FATAL_ERROR
            "zstd not found. Install libzstd-dev (Debian/Ubuntu), "
            "libzstd-devel (Fedora/RHEL), or zstd (Homebrew).")
    endif()
    message(STATUS "zstd not found; MDL compression disabled for this build.")
    target_compile_definitions(aimee-core PUBLIC AIMEE_DISABLE_ZSTD)
    set(ZSTD_LIBRARY "")
endif()

if(UNIX AND NOT APPLE AND NOT WIN32)
    if(WITH_PAM)
        include(CheckIncludeFile)
        include(CheckLibraryExists)
        check_include_file(security/pam_appl.h HAVE_PAM_HEADER)
        check_library_exists(pam pam_start "" HAVE_PAM_LIBRARY)
        if(HAVE_PAM_HEADER AND HAVE_PAM_LIBRARY)
            target_compile_definitions(aimee-cmd PUBLIC WITH_PAM)
            target_link_libraries(aimee-cmd PUBLIC pam)
        else()
            message(STATUS "PAM not found; disabling WITH_PAM")
        endif()
    endif()

    if(WITH_LIBSECRET)
        find_package(PkgConfig)
        if(PkgConfig_FOUND)
            pkg_check_modules(LIBSECRET libsecret-1)
        endif()
        if(LIBSECRET_FOUND)
            target_compile_definitions(aimee-cmd PUBLIC WITH_LIBSECRET)
            target_include_directories(aimee-cmd PUBLIC ${LIBSECRET_INCLUDE_DIRS})
            target_link_directories(aimee-cmd PUBLIC ${LIBSECRET_LIBRARY_DIRS})
            target_link_libraries(aimee-cmd PUBLIC ${LIBSECRET_LIBRARIES})
            target_compile_options(aimee-cmd PRIVATE ${LIBSECRET_CFLAGS_OTHER})
        else()
            message(STATUS "libsecret not found; disabling WITH_LIBSECRET")
        endif()
    endif()
endif()

endif() # NOT AIMEE_THIN_CLIENT (full-build static libs + native deps)

# On Windows (MinGW), link statically so executables don't depend on
# libwinpthread-1.dll, libgcc_s_seh-1.dll, etc.
if(WIN32 AND MINGW)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
endif()

# Dead-code elimination linker flags (LTO itself is set up at the top of this
# file so it applies uniformly to libraries and test binaries alike — see
# comment there).  The lean profile additionally strips symbols (-s).
# Linux and MinGW/Windows use GNU ld's --gc-sections; macOS uses -dead_strip.
# Windows MUST be included: without it, dead client code (e.g. the unused
# is_noise_utterance in util.c) is retained and drags in undefined references
# (tokenize_for_search) that the thin client never actually calls.
if(APPLE)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-dead_strip")
else()
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--gc-sections")
endif()

# Lean build: additionally strip symbols
if(AIMEE_LEAN)
    if(NOT WIN32 AND NOT APPLE)
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -s")
    endif()
endif()

file(STRINGS "${AIMEE_SRC_DIR}/core/VERSION" AIMEE_CORE_SOURCE_VERSION LIMIT_COUNT 1)
set(AIMEE_CORE_REQUIRED_VERSION "${AIMEE_CORE_SOURCE_VERSION}" CACHE STRING
    "Pinned aimee-core package version")
option(AIMEE_USE_INSTALLED_CORE "Use a versioned installed aimee-core package" OFF)
if(AIMEE_USE_INSTALLED_CORE)
    find_package(aimee-core ${AIMEE_CORE_REQUIRED_VERSION} EXACT CONFIG REQUIRED)
    if(WITH_TLS AND NOT AIMEE_CORE_NATIVE_TLS_ENABLED)
        message(FATAL_ERROR "The pinned installed aimee-core package has native TLS disabled")
    endif()
    add_library(aimee-core-connection ALIAS aimee::aimee-core-connection)
    if(TARGET aimee::aimee-core-event-bus)
        add_library(aimee-core-event-bus ALIAS aimee::aimee-core-event-bus)
    endif()
else()
add_library(aimee-core-connection STATIC
    ${AIMEE_SRC_DIR}/core/connection/auth.c
    ${AIMEE_SRC_DIR}/core/connection/control.c
    ${AIMEE_SRC_DIR}/core/connection/endpoint.c
    ${AIMEE_SRC_DIR}/core/connection/http1.c
    ${AIMEE_SRC_DIR}/core/connection/socket.c
)
target_include_directories(aimee-core-connection PUBLIC ${AIMEE_CORE_CONNECTION_INCLUDE_DIR})
target_compile_options(aimee-core-connection PRIVATE ${AIMEE_COMMON_WARNINGS})
if(WIN32)
    target_compile_definitions(aimee-core-connection PRIVATE _WIN32_WINNT=0x0A00)
    target_link_libraries(aimee-core-connection PUBLIC ws2_32)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(aimee-core-event-bus STATIC
        ${AIMEE_SRC_DIR}/core/event_bus/bus_attach.c
        ${AIMEE_SRC_DIR}/core/event_bus/bus_arena.c
        ${AIMEE_SRC_DIR}/core/event_bus/bus_capture.c
        ${AIMEE_SRC_DIR}/core/event_bus/bus_client.c
        ${AIMEE_SRC_DIR}/core/event_bus/bus_endpoint.c
        ${AIMEE_SRC_DIR}/core/event_bus/bus_host.c
        ${AIMEE_SRC_DIR}/core/event_bus/module_client.c
        ${AIMEE_SRC_DIR}/core/event_bus/module_protocol.c
        ${AIMEE_SRC_DIR}/core/event_bus/module_runtime.c
        ${AIMEE_SRC_DIR}/core/event_bus/bus_region.c
        ${AIMEE_SRC_DIR}/core/event_bus/bus_region_host.c
        ${AIMEE_SRC_DIR}/core/event_bus/bus_ring.c
        ${AIMEE_SRC_DIR}/core/event_bus/bus_route.c
        ${AIMEE_SRC_DIR}/core/event_bus/bus_runtime.c
        ${AIMEE_SRC_DIR}/core/event_bus/bus_wire.c
    )
    target_include_directories(aimee-core-event-bus PUBLIC
        ${AIMEE_SRC_DIR}/core/event_bus/include)
    target_compile_options(aimee-core-event-bus PRIVATE ${AIMEE_COMMON_WARNINGS})
    target_link_libraries(aimee-core-event-bus PUBLIC Threads::Threads)
endif()
endif()

add_executable(aimee
    ${CLI_SRCS}
    ${CLIENT_SUPPORT_SRCS}
    ${AIMEE_SRC_DIR}/aimee_home.c
    ${AIMEE_SRC_DIR}/code_collect.c
    ${AIMEE_SRC_DIR}/codex_auth.c
    ${AIMEE_SRC_DIR}/dstr.c
    ${AIMEE_SRC_DIR}/history.c
    ${AIMEE_SRC_DIR}/markdown.c
    ${AIMEE_SRC_DIR}/vendor/cJSON.c
    ${AIMEE_SRC_DIR}/yaml.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_iface.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_def.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_validate.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_canonical.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_custom.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_manager_artifacts.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_externalization.c
    ${AIMEE_SRC_DIR}/modules/workflows/tool_egress.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_native_gate.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_enforce.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_deliver.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_advance.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_router.c
    ${AIMEE_SRC_DIR}/modules/workflows/wfe_router_catalog.c
)
target_include_directories(aimee PRIVATE
    ${AIMEE_SRC_DIR}
    ${AIMEE_SRC_DIR}/headers
    ${AIMEE_SRC_DIR}/vendor/headers
    ${AIMEE_SRC_DIR}/db1_client
    ${AIMEE_SRC_DIR}/modules/db2/c)
target_compile_options(aimee PRIVATE ${AIMEE_COMMON_WARNINGS})
target_link_libraries(aimee PRIVATE aimee-core-connection)
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
    target_sources(aimee PRIVATE ${AIMEE_SRC_DIR}/http_content_encoding.c)
    target_link_libraries(aimee PRIVATE Threads::Threads ZLIB::ZLIB)
else()
    message(STATUS "zlib unavailable: building thin client without HTTP gzip")
    target_sources(aimee PRIVATE ${AIMEE_SRC_DIR}/http_content_encoding_stub.c)
    target_link_libraries(aimee PRIVATE Threads::Threads)
endif()
if(NOT WIN32)
    # cmd_optimize.c (bandit math) and cli_attention_guard.c use sqrt/pow/log;
    # libm is implicit in libSystem on Apple but a separate link on Linux. The
    # make build links -lm unconditionally (L_CLIENT); mirror that here. Windows
    # math lives in the CRT, so no separate lib.
    target_link_libraries(aimee PRIVATE m)
endif()
if(WIN32)
    target_compile_definitions(aimee PRIVATE AIMEE_WINDOWS _WIN32_WINNT=0x0A00)
    target_link_libraries(aimee PRIVATE ws2_32)
    # LTO + --gc-sections on MinGW fails to drop the dead is_noise_utterance
    # (util.c, pulled in for opt_parse etc.) which references tokenize_for_search
    # (text.c — not part of the thin client). Disabling IPO for this target lets
    # --gc-sections strip the unreachable function, matching the make build.
    set_target_properties(aimee PROPERTIES INTERPROCEDURAL_OPTIMIZATION FALSE)
endif()
if(WITH_TLS)
    target_compile_definitions(aimee PRIVATE WITH_TLS)
endif()
if(WITH_TLS AND NOT AIMEE_USE_INSTALLED_CORE)
    target_sources(aimee-core-connection PRIVATE
        ${AIMEE_SRC_DIR}/core/connection/native_tls_identity.c
        ${AIMEE_SRC_DIR}/core/connection/native_tls_path.c)
    if(APPLE)
        # macOS: Secure Transport (system frameworks; universal-binary safe; Keychain
        # trust). Deprecated since 10.15 -> silence the deprecation warning for THIS
        # TU only (the build is -Werror); never globally.
        target_sources(aimee-core-connection PRIVATE
            ${AIMEE_SRC_DIR}/core/connection/native_tls_securetransport.c)
        set_source_files_properties(
            ${AIMEE_SRC_DIR}/core/connection/native_tls_securetransport.c
            PROPERTIES COMPILE_OPTIONS "-Wno-deprecated-declarations")
        find_library(SECURITY_FRAMEWORK Security REQUIRED)
        find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
        target_link_libraries(aimee-core-connection PUBLIC
            ${SECURITY_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK})
    elseif(WIN32)
        # Windows: Schannel/SSPI (no OpenSSL; Windows certificate store). ncrypt
        # supplies CNG (NCryptImportKey) for mTLS client-key import. Pin
        # _WIN32_WINNT>=0x0A00 at the build level too (the schannel TU also sets
        # it before any include) so the CNG/CERT_NCRYPT_KEY_SPEC symbols are
        # exposed regardless of header include order.
        target_sources(aimee-core-connection PRIVATE
            ${AIMEE_SRC_DIR}/core/connection/native_tls_schannel.c)
        target_compile_definitions(aimee-core-connection PRIVATE _WIN32_WINNT=0x0A00)
        target_link_libraries(aimee-core-connection PUBLIC secur32 crypt32 ncrypt)
    else()
        # Linux/other POSIX: OpenSSL.
        find_package(OpenSSL REQUIRED)
        target_sources(aimee-core-connection PRIVATE
            ${AIMEE_SRC_DIR}/core/connection/tls_openssl.c
            ${AIMEE_SRC_DIR}/core/connection/native_tls_openssl.c)
        target_link_libraries(aimee-core-connection PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    endif()
endif()


# Generate embedded help data from docs/agent.md
set(AGENT_HELP_HEADER "${AIMEE_SRC_DIR}/agent_help_data.h")
set(AGENT_HELP_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/gen_help_data.py")
file(WRITE "${AGENT_HELP_SCRIPT}"
"import sys\n\
data = open(sys.argv[1], 'rb').read()\n\
with open(sys.argv[2], 'w') as f:\n\
    f.write('/* Auto-generated from docs/agent.md -- do not edit */\\n')\n\
    f.write('static const unsigned char agent_help_data[] = {')\n\
    f.write(', '.join('0x{:02x}'.format(b) for b in data))\n\
    f.write('};\\n')\n\
    f.write('static const unsigned int agent_help_data_len = {};\\n'.format(len(data)))\n\
")
add_custom_command(
    OUTPUT "${AGENT_HELP_HEADER}"
    COMMAND "${Python3_EXECUTABLE}" "${AGENT_HELP_SCRIPT}" "${CMAKE_CURRENT_SOURCE_DIR}/docs/agent.md" "${AGENT_HELP_HEADER}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/docs/agent.md"
    COMMENT "Generating agent_help_data.h from docs/agent.md"
)
add_custom_target(agent_help_header DEPENDS "${AGENT_HELP_HEADER}")
# cli_main.c #includes the generated agent_help_data.h, so the thin client must
# depend on its generator — otherwise a client-only build (e.g. Windows, which
# builds only the `aimee` target) races the generated header.
add_dependencies(aimee agent_help_header)

# --- Non-thin-client targets ------------------------------------------------
# CMake builds ONLY the `aimee` thin client (+ the unit-test suite below).
# aimee-server, aimee-kb, aimee-gateway and aimee-runtime-web are Linux/Docker
# components built EXCLUSIVELY via `make -C src` (see src/Makefile); their CMake
# targets were removed because CMake's source lists are not maintained for the
# full server build (they drifted ~188 files behind src/Makefile) and no CI gate
# ever built them. The block below is the agent tool-prompt / DB schema codegen
# and the unit-test suite, which the thin-client-only profile
# (-DAIMEE_THIN_CLIENT=ON) skips.
if(NOT AIMEE_THIN_CLIENT)

set(TOOL_PROMPTS_HEADER "${AIMEE_SRC_DIR}/tool_prompts_data.h")
file(GLOB TOOL_PROMPT_FILES CONFIGURE_DEPENDS "${AIMEE_SRC_DIR}/tool_prompts/*.md")
add_custom_command(
    OUTPUT "${TOOL_PROMPTS_HEADER}"
    COMMAND "${Python3_EXECUTABLE}" "${AIMEE_SRC_DIR}/gen_tool_prompts.py" "${TOOL_PROMPTS_HEADER}"
    DEPENDS "${AIMEE_SRC_DIR}/gen_tool_prompts.py" ${TOOL_PROMPT_FILES}
    COMMENT "Generating tool_prompts_data.h from src/tool_prompts"
)
add_custom_target(tool_prompts_header DEPENDS "${TOOL_PROMPTS_HEADER}")
add_dependencies(aimee-agent tool_prompts_header)

# Generate schema_data.h from tier schema SQL. modules/db1/db_schema.c and
# modules/db2/c/db_schema.c includes this header to embed its tier schemas as C
# string constants for DB1 and DB2. Without this rule, fresh CMake checkouts
# fail to build with "schema_data.h: No such file or directory" because the
# generator only ran from the Makefile.
set(SCHEMA_DATA_HEADER "${AIMEE_SRC_DIR}/schema_data.h")
set(SCHEMA_FILES
    "${AIMEE_SRC_DIR}/modules/db2/c/schema.sql"
    "${AIMEE_SRC_DIR}/modules/db2/c/schema_sqlite.sql"
)
add_custom_command(
    OUTPUT "${SCHEMA_DATA_HEADER}"
    COMMAND "${Python3_EXECUTABLE}" "${AIMEE_SRC_DIR}/gen_schema.py" "${SCHEMA_DATA_HEADER}"
    WORKING_DIRECTORY "${AIMEE_SRC_DIR}"
    DEPENDS "${AIMEE_SRC_DIR}/gen_schema.py" ${SCHEMA_FILES}
    COMMENT "Generating schema_data.h from tier schema SQL"
)
add_custom_target(schema_data_header DEPENDS "${SCHEMA_DATA_HEADER}")
add_dependencies(aimee-core schema_data_header)



add_custom_target(module-boundary-check
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-module-boundary.py" --src-dir "${AIMEE_SRC_DIR}" --require-dirs
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-module-boundary.py" --src-dir "${AIMEE_SRC_DIR}" --plant-test
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Checking aimee-kb/server source module boundaries"
)
add_test(NAME module_boundary_check
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-module-boundary.py" --src-dir "${AIMEE_SRC_DIR}" --require-dirs
)

add_custom_target(kb-v1-coverage-check
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-kb-v1-coverage.py" --src-dir "${AIMEE_SRC_DIR}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-kb-v1-coverage.py" --plant-test
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Checking aimee-kb /v1 coverage for legacy kb RPC migration"
)
add_custom_target(kb-target-isolation-check
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-kb-target-isolation.py" --makefile "${AIMEE_SRC_DIR}/Makefile" --cmake "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-kb-target-isolation.py" --plant-test
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Checking standalone aimee-kb target isolation"
)
add_custom_target(kb-container-packaging-check
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-kb-container-packaging.py" --root "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-kb-container-packaging.py" --plant-test
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Checking aimee-kb container packaging"
)
add_test(NAME kb_v1_coverage_check
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-kb-v1-coverage.py" --src-dir "${AIMEE_SRC_DIR}"
)
add_test(NAME kb_v1_coverage_plant
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-kb-v1-coverage.py" --plant-test
)
add_test(NAME kb_target_isolation_check
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-kb-target-isolation.py" --makefile "${AIMEE_SRC_DIR}/Makefile" --cmake "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
)
add_test(NAME kb_target_isolation_plant
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-kb-target-isolation.py" --plant-test
)
add_test(NAME kb_container_packaging_check
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-kb-container-packaging.py" --root "${CMAKE_CURRENT_SOURCE_DIR}"
)
add_test(NAME kb_container_packaging_plant
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-kb-container-packaging.py" --plant-test
)

add_subdirectory(src/tests)

endif() # NOT AIMEE_THIN_CLIENT
