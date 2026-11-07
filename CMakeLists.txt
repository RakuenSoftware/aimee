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

# CMake builds only the aimee thin client and its shared transport library, on
# Linux, macOS and Windows. aimee proper (aimee-server, aimee-kb, the ring host,
# module processes) is Linux-only and builds with `make -C src`.
option(WITH_TLS "Enable in-client TLS for https:// remote aimee-servers" ON)
# Native TLS backends are part of aimee-core-connection:
#   Linux   -> OpenSSL
#   macOS   -> Secure Transport
#   Windows -> Schannel/SSPI
# Each uses the OS trust store, so the single-binary thin client needs no bundled
# CA bundle and no external TLS library.
option(AIMEE_LEAN "Lean build profile: size-optimized" OFF)

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
    "${AIMEE_SRC_DIR}/core/codec/include"

    "${AIMEE_SRC_DIR}/core/types/include"
    "${AIMEE_SRC_DIR}/core/connection/include")

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
    "${AIMEE_SRC_DIR}/modules/learning/include"
    "${AIMEE_SRC_DIR}/modules/tools/include"
    "${AIMEE_SRC_DIR}/modules/workspace/include"
    "${AIMEE_SRC_DIR}/modules/git/include"
    "${AIMEE_SRC_DIR}/modules/egress/include"
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
    "${AIMEE_SRC_DIR}/modules/providers/include"
    "${AIMEE_SRC_DIR}/modules/sandbox/include"
    "${AIMEE_SRC_DIR}/core/types/include"
    "${AIMEE_SRC_DIR}/core/turn_integrity/include"
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
# effect uniformly across every library and executable.
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
        -Wformat=2
        -Wformat-security
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
        -Wformat=2
        -Wformat-security
        -Wno-unused-result
        -ffunction-sections
        -fdata-sections
    )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_compile_options(-D_FORTIFY_SOURCE=3 -fstack-protector-strong
                        -fstack-clash-protection -fPIE)
    # -fcf-protection enables Intel CET and is rejected by GCC on AArch64.
    # Gate it on the target processor (not the build host, so cross-compiles
    # select the same flags as native builds).
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|i[3-6]86)$")
        add_compile_options(-fcf-protection=full)
    endif()
    # -pie belongs to executables only; on the shared client library it links
    # crt1 and fails for want of main.
    add_link_options(-Wl,-z,relro,-z,now -Wl,-z,noexecstack)
    add_link_options($<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:-pie>)
endif()

if(WIN32)
    set(CLIENT_SUPPORT_SRCS
        ${AIMEE_SRC_DIR}/windows/platform_ipc.c
        ${AIMEE_SRC_DIR}/shared/platform_net.c
        ${AIMEE_SRC_DIR}/windows/platform_path.c
        ${AIMEE_SRC_DIR}/windows/platform_process.c
        # cli_mcp_serve mints the session id when none is published yet.
        ${AIMEE_SRC_DIR}/windows/platform_random.c
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
elseif(APPLE)
    set(CLIENT_SUPPORT_SRCS
        ${AIMEE_SRC_DIR}/posix/platform_path.c
        ${AIMEE_SRC_DIR}/posix/platform_process.c
        ${AIMEE_SRC_DIR}/shared/platform_net.c
        ${AIMEE_SRC_DIR}/mac/platform_process.c
        # cli_mcp_serve mints the session id when none is published yet.
        ${AIMEE_SRC_DIR}/posix/platform_random.c
        ${AIMEE_SRC_DIR}/posix/cli_client.c
        ${AIMEE_SRC_DIR}/posix/cli_main.c
        ${AIMEE_SRC_DIR}/posix/util.c
        ${AIMEE_SRC_DIR}/util.c
        ${AIMEE_SRC_DIR}/posix/workspace_provider.c
        ${AIMEE_SRC_DIR}/modules/workspace/workspace_provider_detached.c
        ${AIMEE_SRC_DIR}/aimee_client.c
        ${AIMEE_SRC_DIR}/http_uds_client.c
    )
else()
    set(CLIENT_SUPPORT_SRCS
        ${AIMEE_SRC_DIR}/posix/platform_path.c
        ${AIMEE_SRC_DIR}/posix/platform_process.c
        ${AIMEE_SRC_DIR}/shared/platform_net.c
        ${AIMEE_SRC_DIR}/linux/platform_process.c
        # cli_mcp_serve mints the session id when none is published yet.
        ${AIMEE_SRC_DIR}/posix/platform_random.c
        ${AIMEE_SRC_DIR}/posix/cli_client.c
        ${AIMEE_SRC_DIR}/posix/cli_main.c
        ${AIMEE_SRC_DIR}/posix/util.c
        ${AIMEE_SRC_DIR}/util.c
        ${AIMEE_SRC_DIR}/posix/workspace_provider.c
        ${AIMEE_SRC_DIR}/modules/workspace/workspace_provider_detached.c
        ${AIMEE_SRC_DIR}/aimee_client.c
        ${AIMEE_SRC_DIR}/http_uds_client.c
    )
endif()

set(IR_BASE_SRCS
    ${AIMEE_SRC_DIR}/modules/ir/aimee_ir.c
    ${AIMEE_SRC_DIR}/modules/ir/session_event.c
    ${AIMEE_SRC_DIR}/modules/ir/request_codec.c
    ${AIMEE_SRC_DIR}/modules/ir/response_codec.c
)
set(HOOK_IR_SRCS
    ${AIMEE_SRC_DIR}/modules/translation/hook_ir.c
)
set(CLIENT_TRANSPORT_SRCS ${AIMEE_SRC_DIR}/cli_client.c)
set(CLI_SRCS
    ${HOOK_IR_SRCS}
    ${IR_BASE_SRCS}
    ${AIMEE_SRC_DIR}/cli_main.c
    ${AIMEE_SRC_DIR}/cli_agent_setup.c
    ${AIMEE_SRC_DIR}/cli_session_start.c
    ${AIMEE_SRC_DIR}/hook_session_token.c
    # hook_session_token_mint uses the portable hex wrapper; the platform
    # backend in CLIENT_SUPPORT_SRCS supplies platform_random_bytes only.
    ${AIMEE_SRC_DIR}/platform_random.c
    # cli_session_start renders the degraded-dependency notice from here; the
    # CLI links it directly, so it must be listed in BOTH build systems.
    ${AIMEE_SRC_DIR}/session_degraded_notice.c
    # Thin-client self-update (cli_main dispatch + cli_session_start drift notice
    # both call these; the CLI links these directly, so they build in here).
    ${AIMEE_SRC_DIR}/cmd_self_update.c
    ${AIMEE_SRC_DIR}/self_update_util.c
    # Hook project resolution is still performed client-side so a remote server
    # receives the real workspace identity.
    ${AIMEE_SRC_DIR}/harness_memory_common.c
    ${AIMEE_SRC_DIR}/harness_memory_scope.c
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
    ${CLIENT_TRANSPORT_SRCS}
    ${AIMEE_SRC_DIR}/cli_kb_smoke.c
    ${AIMEE_SRC_DIR}/cli_v1_routes.c
    ${AIMEE_SRC_DIR}/cli_v1_routes_d.c
    ${AIMEE_SRC_DIR}/cli_native_bridge.c
    ${AIMEE_SRC_DIR}/cli_v1_routes_e.c
    ${AIMEE_SRC_DIR}/cli_v1_routes_c.c
    ${AIMEE_SRC_DIR}/cli_v1_routes_b.c
    ${AIMEE_SRC_DIR}/cli_v1_routes_f.c
    ${AIMEE_SRC_DIR}/cli_argspec.c
    ${AIMEE_SRC_DIR}/cli_index_bootstrap.c
    ${AIMEE_SRC_DIR}/cli_launch.c
    ${AIMEE_SRC_DIR}/cli_proxy.c
    ${AIMEE_SRC_DIR}/cli_proxy_cmd.c
    ${AIMEE_SRC_DIR}/cli_mcp_serve.c
    # handle_tools_list calls mcp_tool_prose_lean/mcp_compact_tool_prose to build
    # the lean catalog. The client does not
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

find_package(Threads REQUIRED)
# The HTTP transport uses WinHTTP on Windows, but executable workflow artifacts
# still require OpenSSL for SHA-256 and signed-manifest verification.
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

# The thin client consumes the portable codec.
add_library(aimee-core-codec STATIC ${AIMEE_SRC_DIR}/core/codec/base64.c)
target_compile_features(aimee-core-codec PUBLIC c_std_11)
target_include_directories(aimee-core-codec PUBLIC ${AIMEE_SRC_DIR}/core/codec/include)


# On Windows (MinGW), link statically so executables don't depend on
# libwinpthread-1.dll, libgcc_s_seh-1.dll, etc.
if(WIN32 AND MINGW)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
endif()

# Dead-code elimination linker flags (LTO itself is set up at the top of this
# file so it applies uniformly to every target — see comment there).  The lean profile additionally strips symbols (-s).
# Linux and MinGW/Windows use GNU ld's --gc-sections; macOS uses -dead_strip.
# Windows MUST be included: without it, dead client code (e.g. the unused
# is_noise_utterance in util.c) is retained and drags in undefined references
# (tokenize_for_search) that the thin client never actually calls.
if(APPLE)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-dead_strip")
else()
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--gc-sections")
endif()

# Preserve symbols in the built artifact for crash forensics. Packaging may
# publish detached symbols, but link-time stripping cannot be the only copy.

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

endif()

add_executable(aimee
    ${CLI_SRCS}
    ${CLIENT_SUPPORT_SRCS}
    ${AIMEE_SRC_DIR}/aimee_home.c
    ${AIMEE_SRC_DIR}/code_collect.c
    ${AIMEE_SRC_DIR}/codex_auth.c
    ${AIMEE_SRC_DIR}/artifact_trust.c
    ${AIMEE_SRC_DIR}/dstr.c
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
if(NOT WIN32)
    target_sources(aimee PRIVATE
        ${AIMEE_SRC_DIR}/modules/workspace/workspace_identity.c
        ${AIMEE_SRC_DIR}/modules/workspace/workspace_manifest.c
        ${AIMEE_SRC_DIR}/report_enrichment.c
        ${AIMEE_SRC_DIR}/util_url.c)
endif()
target_include_directories(aimee PRIVATE
    ${AIMEE_SRC_DIR}
    ${AIMEE_SRC_DIR}/headers
    ${AIMEE_SRC_DIR}/vendor/headers
    ${AIMEE_SRC_DIR}/db1_client
    ${AIMEE_SRC_DIR}/modules/kb/c)
target_compile_definitions(aimee PRIVATE AIMEE_ARTIFACT_TRUST_NO_AUDIT=1)
target_compile_options(aimee PRIVATE ${AIMEE_COMMON_WARNINGS})
target_link_libraries(aimee PRIVATE aimee-core-connection aimee-core-codec OpenSSL::Crypto)
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
    target_link_libraries(aimee PRIVATE ws2_32 bcrypt)
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


# One thinclient transport implementation, linked by the CLI and consumed by
# in-process browser/Discord language bindings. No server/domain dependencies.
if(NOT AIMEE_USE_INSTALLED_CORE)
    set_target_properties(aimee-core-connection PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
add_library(aimee-client SHARED
    ${CLIENT_TRANSPORT_SRCS}
    ${AIMEE_SRC_DIR}/cli_chat_stream.c
    ${AIMEE_SRC_DIR}/aimee_home.c
    ${AIMEE_SRC_DIR}/aimee_sha256.c
    ${AIMEE_SRC_DIR}/vendor/cJSON.c
    ${IR_BASE_SRCS})
target_include_directories(aimee-client PUBLIC ${AIMEE_SRC_DIR}/headers
    PRIVATE ${AIMEE_SRC_DIR} ${AIMEE_SRC_DIR}/vendor/headers)
if(NOT WIN32)
    target_compile_options(aimee-client PRIVATE -fPIC)
    if(NOT AIMEE_USE_INSTALLED_CORE)
        target_compile_options(aimee-core-connection PRIVATE -fPIC)
    endif()
endif()
target_link_libraries(aimee-client PRIVATE aimee-core-connection Threads::Threads OpenSSL::Crypto)
target_compile_definitions(aimee-client PRIVATE AIMEE_CLIENT_LIBRARY)
if(WITH_TLS)
    target_compile_definitions(aimee-client PRIVATE WITH_TLS)
endif()
set_target_properties(aimee-client PROPERTIES OUTPUT_NAME aimee-client)
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/pkgconfig/aimee-client.pc" CONTENT
"prefix=${CMAKE_BINARY_DIR}\nlibdir=\${prefix}\nincludedir=${AIMEE_SRC_DIR}/headers\n\nName: aimee-client\nDescription: Shared Aimee thinclient transport\nVersion: 1.0.0\nLibs: -L\${libdir} -laimee-client\nCflags: -I\${includedir}\n")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/aimee-client-install.pc" CONTENT
"prefix=\${pcfiledir}/../..\nlibdir=\${prefix}/lib\nincludedir=\${prefix}/include/aimee-client\n\nName: aimee-client\nDescription: Shared Aimee thinclient transport\nVersion: 1.0.0\nLibs: -L\${libdir} -laimee-client\nCflags: -I\${includedir}\n")
install(TARGETS aimee-client LIBRARY DESTINATION lib COMPONENT ThinClientSDK RUNTIME DESTINATION bin COMPONENT ThinClientSDK)
install(FILES ${AIMEE_SRC_DIR}/headers/cli_client.h DESTINATION include/aimee-client COMPONENT ThinClientSDK)
install(FILES "${CMAKE_BINARY_DIR}/aimee-client-install.pc" DESTINATION lib/pkgconfig RENAME aimee-client.pc COMPONENT ThinClientSDK)
