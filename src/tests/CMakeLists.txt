# CMake test integration for aimee
# Root CMakeLists.txt should call add_subdirectory(src/tests) after core targets
# are defined. As Windows support expands, mirror the full src/tests/Rules.mk set
# here with one executable per C test and register each via add_test().

enable_testing()

function(aimee_enable_test_asserts target_name)
  if(MSVC)
    target_compile_options(${target_name} PRIVATE /U NDEBUG)
  else()
    target_compile_options(${target_name} PRIVATE -UNDEBUG)
  endif()
endfunction()

function(aimee_add_test test_name)
  add_executable(${test_name} ${ARGN})
  target_include_directories(${test_name} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../headers
    ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/headers)
  target_link_libraries(${test_name} PRIVATE aimee-core-codec)
  aimee_enable_test_asserts(${test_name})
  if(WIN32)
    target_compile_definitions(${test_name} PRIVATE AIMEE_WINDOWS _WIN32_WINNT=0x0A00)
  endif()
  if(TARGET aimee-core)
    target_link_libraries(${test_name} PRIVATE aimee-core)
  endif()
  add_test(NAME ${test_name} COMMAND ${test_name})
  set_tests_properties(${test_name} PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endfunction()

# Portable unit tests that build cleanly under both POSIX and the current
# Windows CMake target set.
aimee_add_test(test_server_mcp_project test_server_mcp_project.c
               ../server/server_mcp_project.c ../server/server_active_project.c)
aimee_add_test(test_server_active_project test_server_active_project.c
               ../server/server_active_project.c)
aimee_add_test(test_harness_memory_scope test_harness_memory_scope.c)
aimee_add_test(test_harness_memory_spill test_harness_memory_spill.c)
aimee_add_test(test_harness_memory_audit test_harness_memory_audit.c)
aimee_add_test(test_plugin_c_hook test_plugin_c_hook.c)

# test_git_org_repos: provider detection + repo-list JSON parsing + paginated
# enumeration (HTTP + host-cred stubbed in the test). Self-contained — compiles
# the module + cJSON directly rather than linking aimee-core, so the test's
# agent_http_get/git_host_cred_get stubs don't clash with the real symbols.
add_executable(test_git_org_repos test_git_org_repos.c
  ../modules/git/git_org_repos.c ../vendor/cJSON.c)
target_include_directories(test_git_org_repos PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../headers
    ${CMAKE_CURRENT_SOURCE_DIR}/../modules/git
  ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/headers)
aimee_enable_test_asserts(test_git_org_repos)
add_test(NAME test_git_org_repos COMMAND test_git_org_repos)
set_tests_properties(test_git_org_repos PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# test_git_oauth_device: provider-name parsing + endpoint construction for the
# GitLab/Gitea device flow. HTTP + vault externals stubbed in the test (never
# invoked by the pure functions under test).
add_executable(test_git_oauth_device test_git_oauth_device.c
  ../modules/git/git_oauth_device.c ../vendor/cJSON.c)
target_include_directories(test_git_oauth_device PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../headers
    ${CMAKE_CURRENT_SOURCE_DIR}/../modules/git
    ${CMAKE_CURRENT_SOURCE_DIR}/../modules/vault
  ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/headers)
aimee_enable_test_asserts(test_git_oauth_device)
add_test(NAME test_git_oauth_device COMMAND test_git_oauth_device)
set_tests_properties(test_git_oauth_device PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# test_git_oauth_github: completing a sign-in requires the token to be STORED.
# Walks start -> poll against scripted HTTP/vault stubs, so the store failure is
# reachable without a network. Not linked against aimee-core: the stubs here
# deliberately replace externals that aimee-core also defines.
add_executable(test_git_oauth_github test_git_oauth_github.c
  ../modules/git/git_oauth_github.c ../vendor/cJSON.c)
target_include_directories(test_git_oauth_github PRIVATE
  ${AIMEE_PROJECT_INCLUDE_DIRS}
  ${AIMEE_SRC_DIR}/db1
  ${AIMEE_SRC_DIR}/kb_store
  ${CMAKE_CURRENT_SOURCE_DIR}/../modules/git
  ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/headers)
target_link_libraries(test_git_oauth_github PRIVATE Threads::Threads)
aimee_enable_test_asserts(test_git_oauth_github)
add_test(NAME test_git_oauth_github COMMAND test_git_oauth_github)
set_tests_properties(test_git_oauth_github PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# test_git_oauth_gh: PTY-output scrubbing + one-time-code extraction for the
# gh-CLI GitHub sign-in fallback. Exec/vault externals stubbed in the test
# (never invoked by the pure functions under test). POSIX-only (<pty.h>).
if(NOT WIN32)
  add_executable(test_vault_bootstrap_privilege
    test_vault_bootstrap_privilege.c
    ../server/vault_bootstrap_privilege.c)
  target_include_directories(test_vault_bootstrap_privilege PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..)
  aimee_enable_test_asserts(test_vault_bootstrap_privilege)
  add_test(NAME test_vault_bootstrap_privilege COMMAND test_vault_bootstrap_privilege)
  set_tests_properties(test_vault_bootstrap_privilege PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

  add_executable(test_git_oauth_gh test_git_oauth_gh.c
    ../modules/git/git_oauth_gh.c)
  target_include_directories(test_git_oauth_gh PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../headers
    ${CMAKE_CURRENT_SOURCE_DIR}/../modules/git
    ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/headers)
  target_link_libraries(test_git_oauth_gh PRIVATE Threads::Threads)
  aimee_enable_test_asserts(test_git_oauth_gh)
  add_test(NAME test_git_oauth_gh COMMAND test_git_oauth_gh)
  set_tests_properties(test_git_oauth_gh PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
add_executable(test_context_engine test_context_engine.c)
target_include_directories(test_context_engine PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../headers
  ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/headers)
aimee_enable_test_asserts(test_context_engine)
if(TARGET aimee-agent)
  if(NOT WIN32)
    target_link_libraries(test_context_engine PRIVATE
      -Wl,--start-group aimee-agent aimee-data aimee-core -Wl,--end-group)
  else()
    target_link_libraries(test_context_engine PRIVATE aimee-agent aimee-data aimee-core)
  endif()
endif()
add_test(NAME test_context_engine COMMAND test_context_engine)
set_tests_properties(test_context_engine PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
aimee_add_test(test_util test_util.c)

# Event-bus wire codec (feature tree slice 1). Self-contained: bus_wire.c has no
# dependency beyond libc, so the test links it directly rather than via a lib.
if(TARGET aimee-core-event-bus)
set(AIMEE_TEST_OBS_BUS_RUNTIME_SOURCES
  ../core/event_bus/bus_runtime.c
  ../core/event_bus/bus_endpoint.c
  ../core/event_bus/module_client.c
  ../core/event_bus/module_protocol.c
  ../aimee_sha256.c)
aimee_add_test(test_bus_wire test_bus_wire.c ../core/event_bus/bus_wire.c)
target_include_directories(test_bus_wire PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core/event_bus/include)
target_compile_definitions(test_bus_wire PRIVATE
  BUS_VECTOR_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bus")
aimee_add_test(test_module_protocol test_module_protocol.c ../core/event_bus/module_protocol.c)
target_include_directories(test_module_protocol PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../core/event_bus/include)
aimee_add_test(test_bus_ring test_bus_ring.c ../core/event_bus/bus_ring.c)
target_include_directories(test_bus_ring PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core/event_bus/include)
find_package(Threads REQUIRED)
target_link_libraries(test_bus_ring PRIVATE Threads::Threads)
aimee_add_test(test_bus_region test_bus_region.c ../core/event_bus/bus_region.c ../core/event_bus/bus_region_host.c ../core/event_bus/bus_ring.c)
target_include_directories(test_bus_region PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core/event_bus/include)
aimee_add_test(test_bus_arena test_bus_arena.c ../core/event_bus/bus_arena.c)
target_include_directories(test_bus_arena PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core/event_bus/include)
aimee_add_test(test_bus_host test_bus_host.c ../core/event_bus/bus_attach.c ../core/event_bus/bus_host.c ../core/event_bus/bus_route.c ../core/event_bus/bus_region.c ../core/event_bus/bus_region_host.c ../core/event_bus/bus_ring.c ../core/event_bus/bus_arena.c ../core/event_bus/bus_wire.c)
target_include_directories(test_bus_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core/event_bus/include)

aimee_add_test(test_bus_route test_bus_route.c ../core/event_bus/bus_attach.c ../core/event_bus/bus_host.c ../core/event_bus/bus_route.c ../core/event_bus/bus_region.c ../core/event_bus/bus_region_host.c ../core/event_bus/bus_ring.c ../core/event_bus/bus_arena.c ../core/event_bus/bus_wire.c)
target_include_directories(test_bus_route PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core/event_bus/include)

aimee_add_test(test_bus_flow test_bus_flow.c ../modules/audit/obs_bus.c ${AIMEE_TEST_OBS_BUS_RUNTIME_SOURCES} ../core/event_bus/bus_client.c ../core/event_bus/bus_attach.c ../core/event_bus/bus_host.c ../core/event_bus/bus_route.c ../core/event_bus/bus_region.c ../core/event_bus/bus_region_host.c ../core/event_bus/bus_ring.c ../core/event_bus/bus_arena.c ../core/event_bus/bus_wire.c ../core/event_bus/bus_capture.c ../modules/audit/audit_worm.c ../modules/audit/audit_worm_chain.c ../modules/workflows/wfe_canonical.c)
target_include_directories(test_bus_flow PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core/event_bus/include ${CMAKE_CURRENT_SOURCE_DIR}/../modules/audit)
target_link_libraries(test_bus_flow PRIVATE Threads::Threads)

aimee_add_test(test_bus_client test_bus_client.c ../core/event_bus/bus_client.c ../core/event_bus/bus_attach.c ../core/event_bus/bus_host.c ../core/event_bus/bus_route.c ../core/event_bus/bus_region.c ../core/event_bus/bus_region_host.c ../core/event_bus/bus_ring.c ../core/event_bus/bus_arena.c ../core/event_bus/bus_wire.c)
target_include_directories(test_bus_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core/event_bus/include)
find_package(Threads REQUIRED)
target_link_libraries(test_bus_client PRIVATE Threads::Threads)
aimee_add_test(test_bus_endpoint test_bus_endpoint.c ../core/event_bus/bus_endpoint.c)
target_include_directories(test_bus_endpoint PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../core/event_bus/include)
aimee_add_test(test_bus_runtime test_bus_runtime.c)
target_include_directories(test_bus_runtime PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../core/event_bus/include)
target_link_libraries(test_bus_runtime PRIVATE aimee-core-event-bus Threads::Threads)
aimee_add_test(test_module_runtime test_module_runtime.c)
target_include_directories(test_module_runtime PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../core/event_bus/include)
target_link_libraries(test_module_runtime PRIVATE aimee-core-event-bus Threads::Threads)
aimee_add_test(test_routing_module test_routing_module.c ../modules/routing/module_adapter.c)
target_include_directories(test_routing_module PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../core/event_bus/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../modules/routing/include)
aimee_add_test(test_bus_capture test_bus_capture.c ../core/event_bus/bus_capture.c ../core/event_bus/bus_client.c ../core/event_bus/bus_attach.c ../core/event_bus/bus_host.c ../core/event_bus/bus_route.c ../core/event_bus/bus_region.c ../core/event_bus/bus_region_host.c ../core/event_bus/bus_ring.c ../core/event_bus/bus_arena.c ../core/event_bus/bus_wire.c)
target_include_directories(test_bus_capture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core/event_bus/include)
target_link_libraries(test_bus_capture PRIVATE Threads::Threads)

# Retired native DB1 recall/upsert fixtures are replaced by the Go store and
# private-memory integration suites; do not restore in-process store fallbacks.

# Native caller-side wire contract; the pure-Go handler has the reciprocal
# contract vectors in the external module repository.
aimee_add_test(test_bus_config_autonomy test_bus_config_autonomy.c)

# audit-on-bus durability: the first module being migrated onto the bus. Emits N
# rows through the real producer/consumer and requires the real ledger to hold
# exactly N, each once, zero drops. obs_bus + bus compiled in; the ledger reader
# and file writer come from aimee-core. Test binary only.
aimee_add_test(test_bus_audit_durability test_bus_audit_durability.c ../modules/audit/obs_bus.c ${AIMEE_TEST_OBS_BUS_RUNTIME_SOURCES} ../core/event_bus/bus_client.c ../core/event_bus/bus_attach.c ../core/event_bus/bus_host.c ../core/event_bus/bus_route.c ../core/event_bus/bus_region.c ../core/event_bus/bus_region_host.c ../core/event_bus/bus_ring.c ../core/event_bus/bus_arena.c ../core/event_bus/bus_wire.c ../core/event_bus/bus_capture.c)
target_include_directories(test_bus_audit_durability PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core/event_bus/include ${CMAKE_CURRENT_SOURCE_DIR}/../modules/audit)
target_link_libraries(test_bus_audit_durability PRIVATE Threads::Threads)

# audit-on-bus record+replay: the auditability payoff. Emits rows, reads the real
# capture file back, requires faithful ordered replay of every governed-action row.
aimee_add_test(test_bus_audit_replay test_bus_audit_replay.c ../modules/audit/obs_bus.c ${AIMEE_TEST_OBS_BUS_RUNTIME_SOURCES} ../core/event_bus/bus_client.c ../core/event_bus/bus_attach.c ../core/event_bus/bus_host.c ../core/event_bus/bus_route.c ../core/event_bus/bus_region.c ../core/event_bus/bus_region_host.c ../core/event_bus/bus_ring.c ../core/event_bus/bus_arena.c ../core/event_bus/bus_wire.c ../core/event_bus/bus_capture.c)
target_include_directories(test_bus_audit_replay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core/event_bus/include ${CMAKE_CURRENT_SOURCE_DIR}/../modules/audit)
target_link_libraries(test_bus_audit_replay PRIVATE Threads::Threads)

# audit-on-bus capture retention: prior sessions kept, files bounded.
aimee_add_test(test_bus_audit_retention test_bus_audit_retention.c ../modules/audit/obs_bus.c ${AIMEE_TEST_OBS_BUS_RUNTIME_SOURCES} ../core/event_bus/bus_client.c ../core/event_bus/bus_attach.c ../core/event_bus/bus_host.c ../core/event_bus/bus_route.c ../core/event_bus/bus_region.c ../core/event_bus/bus_region_host.c ../core/event_bus/bus_ring.c ../core/event_bus/bus_arena.c ../core/event_bus/bus_wire.c ../core/event_bus/bus_capture.c)
target_include_directories(test_bus_audit_retention PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core/event_bus/include ${CMAKE_CURRENT_SOURCE_DIR}/../modules/audit)
target_link_libraries(test_bus_audit_retention PRIVATE Threads::Threads)

aimee_add_test(test_bus_capture_gaps test_bus_capture_gaps.c ../modules/audit/obs_bus.c ${AIMEE_TEST_OBS_BUS_RUNTIME_SOURCES} ../core/event_bus/bus_client.c ../core/event_bus/bus_attach.c ../core/event_bus/bus_host.c ../core/event_bus/bus_route.c ../core/event_bus/bus_region.c ../core/event_bus/bus_region_host.c ../core/event_bus/bus_ring.c ../core/event_bus/bus_arena.c ../core/event_bus/bus_wire.c ../core/event_bus/bus_capture.c ../modules/audit/audit_worm.c ../modules/audit/audit_worm_chain.c ../modules/workflows/wfe_canonical.c)
target_include_directories(test_bus_capture_gaps PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core/event_bus/include ${CMAKE_CURRENT_SOURCE_DIR}/../modules/audit)
target_link_libraries(test_bus_capture_gaps PRIVATE Threads::Threads)

aimee_add_test(test_execution_policy_bus test_execution_policy_bus.c ../server/execution_policy_bus.c)
target_include_directories(test_execution_policy_bus PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../modules/execution-policy/include)

# The operator replay tool (aimee-server --audit-replay): render a capture file.
aimee_add_test(test_bus_audit_replay_tool test_bus_audit_replay_tool.c ../modules/audit/obs_bus.c ../modules/audit/audit_replay.c ${AIMEE_TEST_OBS_BUS_RUNTIME_SOURCES} ../core/event_bus/bus_client.c ../core/event_bus/bus_attach.c ../core/event_bus/bus_host.c ../core/event_bus/bus_route.c ../core/event_bus/bus_region.c ../core/event_bus/bus_region_host.c ../core/event_bus/bus_ring.c ../core/event_bus/bus_arena.c ../core/event_bus/bus_wire.c ../core/event_bus/bus_capture.c)
target_include_directories(test_bus_audit_replay_tool PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core/event_bus/include ${CMAKE_CURRENT_SOURCE_DIR}/../modules/audit)
target_link_libraries(test_bus_audit_replay_tool PRIVATE Threads::Threads)

# Second module on the bus: the guardrail-semantic event -> db1 guardrail_events.
aimee_add_test(test_bus_guardrail_durability test_bus_guardrail_durability.c ../modules/audit/obs_bus.c ${AIMEE_TEST_OBS_BUS_RUNTIME_SOURCES} ../core/event_bus/bus_client.c ../core/event_bus/bus_attach.c ../core/event_bus/bus_host.c ../core/event_bus/bus_route.c ../core/event_bus/bus_region.c ../core/event_bus/bus_region_host.c ../core/event_bus/bus_ring.c ../core/event_bus/bus_arena.c ../core/event_bus/bus_wire.c ../core/event_bus/bus_capture.c)
target_include_directories(test_bus_guardrail_durability PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core/event_bus/include ${CMAKE_CURRENT_SOURCE_DIR}/../modules/audit)
target_link_libraries(test_bus_guardrail_durability PRIVATE Threads::Threads)

# Shutdown race: concurrent producers vs obs_bus_stop() (UAF regression).
aimee_add_test(test_bus_shutdown_race test_bus_shutdown_race.c ../modules/audit/obs_bus.c ${AIMEE_TEST_OBS_BUS_RUNTIME_SOURCES} ../core/event_bus/bus_client.c ../core/event_bus/bus_attach.c ../core/event_bus/bus_host.c ../core/event_bus/bus_route.c ../core/event_bus/bus_region.c ../core/event_bus/bus_region_host.c ../core/event_bus/bus_ring.c ../core/event_bus/bus_arena.c ../core/event_bus/bus_wire.c ../core/event_bus/bus_capture.c)
target_include_directories(test_bus_shutdown_race PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core/event_bus/include ${CMAKE_CURRENT_SOURCE_DIR}/../modules/audit)
target_link_libraries(test_bus_shutdown_race PRIVATE Threads::Threads)
endif()






aimee_add_test(test_self_update test_self_update.c ../self_update_util.c)
aimee_add_test(test_audit_action test_audit_action.c)
aimee_add_test(test_audit_action_log test_audit_action_log.c)
aimee_add_test(test_audit_ledger test_audit_ledger.c)
aimee_add_test(test_anchor_snapshot test_anchor_snapshot.c)
aimee_add_test(test_agent_admission test_agent_admission.c ../server/agent_admission.c)
aimee_add_test(test_edit_anchored test_edit_anchored.c)
aimee_add_test(test_json_fluent test_json_fluent.c)
aimee_add_test(test_render test_render.c)
aimee_add_test(test_text test_text.c)
aimee_add_test(test_report_enrichment test_report_enrichment.c)
aimee_add_test(test_delivery_target test_delivery_target.c)
aimee_add_test(test_delegate_plan test_delegate_plan.c ../modules/delegates/delegate_plan.c)
aimee_add_test(test_wfe_manager_blocks test_wfe_manager_blocks.c)
aimee_add_test(test_wfe_sliced_build test_wfe_sliced_build.c)
aimee_add_test(test_wfe_manager_artifacts test_wfe_manager_artifacts.c)
aimee_add_test(test_wfe_externalization test_wfe_externalization.c)
aimee_add_test(test_wfe_deliver test_wfe_deliver.c)
aimee_add_test(test_wfe_router test_wfe_router.c)
aimee_add_test(test_wfe_autonomous_route test_wfe_autonomous_route.c)
aimee_add_test(test_wfe_router_catalog test_wfe_router_catalog.c)
aimee_add_test(test_wfe_enforce test_wfe_enforce.c)
aimee_add_test(test_wfe_advance test_wfe_advance.c)
aimee_add_test(test_aimee_ir test_aimee_ir.c ../modules/ir/aimee_ir.c)
aimee_add_test(test_aimee_ir_metrics test_aimee_ir_metrics.c ../modules/ir/aimee_ir_metrics.c)
aimee_add_test(test_aimee_frontend test_aimee_frontend.c
  ../modules/translation/aimee_frontend_anthropic.c
  ../modules/translation/aimee_frontend_openai.c
  ../modules/translation/aimee_frontend_responses.c
  ../modules/translation/aimee_backend_anthropic.c
  ../modules/translation/aimee_backend_openai.c
  ../modules/translation/aimee_backend_responses.c
  ../modules/ir/aimee_ir.c ../aimee_sha256.c)
aimee_add_test(test_aimee_backend test_aimee_backend.c
  ../modules/translation/aimee_backend_anthropic.c
  ../modules/translation/aimee_backend_openai.c
  ../modules/translation/aimee_backend_responses.c
  ../modules/translation/aimee_frontend_anthropic.c
  ../modules/translation/aimee_frontend_openai.c
  ../modules/ir/aimee_ir.c ../aimee_sha256.c)
aimee_add_test(test_aimee_ir_serve test_aimee_ir_serve.c
  support/ir_seam_memory_stub.c ../server/aimee_ir_serve.c
  ../modules/ir/aimee_ir_session.c ../modules/ir/aimee_ir.c
  ../modules/ir/session_event.c ../modules/ir/aimee_ir_metrics.c
  ../modules/translation/wire_codec.c ../modules/translation/history_ir.c
  ../modules/translation/aimee_backend_openai.c
  ../modules/translation/aimee_backend_anthropic.c
  ../modules/translation/aimee_backend_responses.c
  ../modules/translation/aimee_frontend_anthropic.c
  ../modules/translation/aimee_frontend_openai.c
  ../modules/translation/aimee_frontend_responses.c
  ../core/turn_integrity/turn_integrity.c ../aimee_sha256.c)
target_include_directories(test_aimee_ir_serve PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../core/turn_integrity/include)
target_link_libraries(test_aimee_ir_serve PRIVATE Threads::Threads)
foreach(ir_test test_aimee_frontend test_aimee_backend test_aimee_ir_serve)
  target_link_libraries(${ir_test} PRIVATE OpenSSL::Crypto)
endforeach()
aimee_add_test(test_aimee_ir_stream test_aimee_ir_stream.c ../modules/translation/aimee_ir_stream.c ../modules/ir/aimee_ir.c)
aimee_add_test(test_shutdown_forensics test_shutdown_forensics.c)

add_executable(test_toolset test_toolset.c)
target_include_directories(test_toolset PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../headers
  ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/headers)
aimee_enable_test_asserts(test_toolset)
if(TARGET aimee-agent)
  if(NOT WIN32)
    target_link_libraries(test_toolset PRIVATE
      -Wl,--start-group aimee-agent aimee-data aimee-core -Wl,--end-group)
  else()
    target_link_libraries(test_toolset PRIVATE aimee-agent aimee-data aimee-core)
  endif()
endif()
add_test(NAME test_toolset COMMAND test_toolset)
set_tests_properties(test_toolset PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(test_compute_pool test_compute_pool.c ../server/compute_pool.c support/log_stub.c)

target_include_directories(test_compute_pool PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../headers
  ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/headers)
aimee_enable_test_asserts(test_compute_pool)
if(WIN32)
  target_compile_definitions(test_compute_pool PRIVATE AIMEE_WINDOWS _WIN32_WINNT=0x0A00)
endif()
target_link_libraries(test_compute_pool PRIVATE Threads::Threads)
add_test(NAME test_compute_pool COMMAND test_compute_pool)
set_tests_properties(test_compute_pool PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# Per-turn cancel registry (server-owned turn lifecycle). Built standalone with a
# stub presence_session_owner so it does not pull aimee-core (which would clash
# with its own copy of the symbol).
add_executable(test_turn_registry test_turn_registry.c ../server/turn_registry.c support/log_stub.c)
target_include_directories(test_turn_registry PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../headers
  ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/headers)
aimee_enable_test_asserts(test_turn_registry)
target_link_libraries(test_turn_registry PRIVATE Threads::Threads)
add_test(NAME test_turn_registry COMMAND test_turn_registry)
set_tests_properties(test_turn_registry PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(test_server_session_pools test_server_session_pools.c ../server/server_session_pools.c
  ../server/compute_pool.c support/log_stub.c)
target_include_directories(test_server_session_pools PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../headers
  ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/headers)
aimee_enable_test_asserts(test_server_session_pools)
if(WIN32)
  target_compile_definitions(test_server_session_pools PRIVATE AIMEE_WINDOWS _WIN32_WINNT=0x0A00)
endif()
target_link_libraries(test_server_session_pools PRIVATE Threads::Threads)
add_test(NAME test_server_session_pools COMMAND test_server_session_pools)
set_tests_properties(test_server_session_pools PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(test_agent_http test_agent_http.c)
target_include_directories(test_agent_http PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../headers
  ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/headers)
aimee_enable_test_asserts(test_agent_http)
if(TARGET aimee-agent)
  if(NOT WIN32)
    target_link_libraries(test_agent_http PRIVATE
      -Wl,--start-group aimee-agent aimee-data aimee-core -Wl,--end-group)
  else()
    target_link_libraries(test_agent_http PRIVATE aimee-agent aimee-data aimee-core)
  endif()
endif()
add_test(NAME test_agent_http COMMAND test_agent_http)
set_tests_properties(test_agent_http PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# test_kb_lab: Ingest lab — standalone chunk preview and quality audit.
add_executable(test_kb_lab test_kb_lab.c ../kb/kb_lab.c)
target_include_directories(test_kb_lab PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../headers
  ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/headers)
aimee_enable_test_asserts(test_kb_lab)
target_link_libraries(test_kb_lab PRIVATE m)
add_test(NAME test_kb_lab COMMAND test_kb_lab)
set_tests_properties(test_kb_lab PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# test_kb_mdl: MDL scoring for synthesis candidate selection.
# Needs kb_mdl.c (aimee-data) + config_learning.c (aimee-core) + zstd.
add_executable(test_kb_mdl test_kb_mdl.c ../kb/kb_mdl.c)
target_include_directories(test_kb_mdl PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../headers
  ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/headers)
aimee_enable_test_asserts(test_kb_mdl)
if(TARGET aimee-core)
  target_link_libraries(test_kb_mdl PRIVATE aimee-core)
endif()
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(ZSTD QUIET libzstd)
endif()
if(ZSTD_FOUND)
  target_link_libraries(test_kb_mdl PRIVATE ${ZSTD_LIBRARIES})
else()
  target_link_libraries(test_kb_mdl PRIVATE zstd)
endif()
add_test(NAME test_kb_mdl COMMAND test_kb_mdl)
set_tests_properties(test_kb_mdl PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(test_session_compact_focused test_session_compact_focused.c)
target_include_directories(test_session_compact_focused PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../headers
  ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/headers)
aimee_enable_test_asserts(test_session_compact_focused)
if(TARGET aimee-agent)
  if(NOT WIN32)
    target_link_libraries(test_session_compact_focused PRIVATE
      -Wl,--start-group aimee-agent aimee-data aimee-core -Wl,--end-group)
  else()
    target_link_libraries(test_session_compact_focused PRIVATE aimee-agent aimee-data aimee-core)
  endif()
endif()
add_test(NAME test_session_compact_focused COMMAND test_session_compact_focused)
set_tests_properties(test_session_compact_focused PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# Native harness conversion and the POSIX print-mode lifecycle.
if(NOT WIN32)
  add_executable(test_server_learning_approaches test_server_learning_approaches.c
    ../server/server_learning_approaches.c ../vendor/cJSON.c)
  target_include_directories(test_server_learning_approaches PRIVATE ${AIMEE_PROJECT_INCLUDE_DIRS}
    ${AIMEE_SRC_DIR}/modules/kb/c)
  aimee_enable_test_asserts(test_server_learning_approaches)
  add_test(NAME test_server_learning_approaches COMMAND test_server_learning_approaches)
  add_executable(test_server_learning_recovery test_server_learning_recovery.c
    ../server/server_learning_recovery.c ../server/server_learning_outcome.c
    support/log_stub.c ../interaction_event_names.c ../vendor/cJSON.c)
  target_include_directories(test_server_learning_recovery PRIVATE ${AIMEE_PROJECT_INCLUDE_DIRS})
  target_link_libraries(test_server_learning_recovery PRIVATE Threads::Threads)
  aimee_enable_test_asserts(test_server_learning_recovery)
  add_test(NAME test_server_learning_recovery COMMAND test_server_learning_recovery)
  add_executable(test_server_learning_application test_server_learning_application.c
    ../server/server_learning_application.c ../vendor/cJSON.c)
  target_include_directories(test_server_learning_application PRIVATE ${AIMEE_PROJECT_INCLUDE_DIRS}
    ${AIMEE_SRC_DIR}/modules/kb/c)
  aimee_enable_test_asserts(test_server_learning_application)
  add_test(NAME test_server_learning_application COMMAND test_server_learning_application)
  add_executable(test_server_mcp_git_scope test_server_mcp_git_scope.c
    ../server/server_mcp.c ../vendor/cJSON.c)
  target_include_directories(test_server_mcp_git_scope PRIVATE ${AIMEE_PROJECT_INCLUDE_DIRS}
    ${AIMEE_SRC_DIR}/modules/kb/c ${AIMEE_SRC_DIR}/db1_client)
  target_compile_options(test_server_mcp_git_scope PRIVATE -ffunction-sections -fdata-sections)
  if(APPLE)
    target_link_options(test_server_mcp_git_scope PRIVATE -Wl,-dead_strip)
  else()
    target_link_options(test_server_mcp_git_scope PRIVATE -Wl,--gc-sections)
  endif()
  target_link_libraries(test_server_mcp_git_scope PRIVATE Threads::Threads)
  aimee_enable_test_asserts(test_server_mcp_git_scope)
  add_test(NAME test_server_mcp_git_scope COMMAND test_server_mcp_git_scope)
  add_executable(test_server_native_replay test_server_native_replay.c
    ../server/server_native_replay.c ../modules/ir/session_event.c ../modules/ir/aimee_ir.c
    ../aimee_sha256.c ../platform_random.c ../posix/platform_random.c ../vendor/cJSON.c)
  target_include_directories(test_server_native_replay PRIVATE ${AIMEE_PROJECT_INCLUDE_DIRS}
    ${AIMEE_SRC_DIR}/modules/kb/c)
  target_link_libraries(test_server_native_replay PRIVATE Threads::Threads OpenSSL::Crypto)
  aimee_enable_test_asserts(test_server_native_replay)
  add_test(NAME test_server_native_replay COMMAND test_server_native_replay)
  add_executable(test_server_native_tool_admission test_server_native_tool_admission.c
    ../server/server_native_tool_admission.c ../server/request_context.c ../vendor/cJSON.c)
  target_include_directories(test_server_native_tool_admission PRIVATE ${AIMEE_PROJECT_INCLUDE_DIRS}
    ${AIMEE_SRC_DIR}/modules/kb/c ${AIMEE_SRC_DIR}/db1_client)
  aimee_enable_test_asserts(test_server_native_tool_admission)
  add_test(NAME test_server_native_tool_admission COMMAND test_server_native_tool_admission)
  add_executable(test_cli_transcript_archive test_cli_transcript_archive.c ../vendor/cJSON.c)
  set_property(TARGET test_cli_transcript_archive PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
  target_include_directories(test_cli_transcript_archive PRIVATE ${AIMEE_PROJECT_INCLUDE_DIRS})
  target_compile_options(test_cli_transcript_archive PRIVATE -ffunction-sections -fdata-sections)
  if(APPLE)
    target_link_options(test_cli_transcript_archive PRIVATE -Wl,-dead_strip)
  else()
    target_link_options(test_cli_transcript_archive PRIVATE -Wl,--gc-sections)
  endif()
  aimee_enable_test_asserts(test_cli_transcript_archive)
  add_test(NAME test_cli_transcript_archive COMMAND test_cli_transcript_archive)
  add_executable(test_server_session_identity test_server_session_identity.c
    ../server/server_auth.c ../posix/platform_random.c ../vendor/cJSON.c
    ../modules/translation/history_ir.c ../modules/translation/aimee_frontend_anthropic.c
    ../modules/translation/aimee_frontend_openai.c ../modules/translation/aimee_frontend_responses.c
    ../modules/translation/wire_codec.c ../modules/ir/aimee_ir.c ../modules/ir/session_event.c
    ../aimee_sha256.c ../util.c ../text.c
    ../modules/translation/aimee_backend_anthropic.c ../modules/translation/aimee_backend_responses.c)
  set_property(TARGET test_server_session_identity PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
  target_link_libraries(test_server_session_identity PRIVATE OpenSSL::Crypto m)
  target_include_directories(test_server_session_identity PRIVATE ${AIMEE_PROJECT_INCLUDE_DIRS}
    ${AIMEE_SRC_DIR}/modules/kb/c ${AIMEE_SRC_DIR}/db1_client)
  target_compile_options(test_server_session_identity PRIVATE -ffunction-sections -fdata-sections)
  if(APPLE)
    target_link_options(test_server_session_identity PRIVATE -Wl,-dead_strip)
  else()
    target_link_options(test_server_session_identity PRIVATE -Wl,--gc-sections)
  endif()
  aimee_enable_test_asserts(test_server_session_identity)
  add_test(NAME test_server_session_identity COMMAND test_server_session_identity)
  add_executable(test_server_native_launch test_server_native_launch.c
    ../server/server_native_launch.c ../modules/ir/aimee_ir.c ../aimee_sha256.c ../vendor/cJSON.c)
  target_include_directories(test_server_native_launch PRIVATE ${AIMEE_PROJECT_INCLUDE_DIRS})
  target_link_libraries(test_server_native_launch PRIVATE OpenSSL::Crypto)
  aimee_enable_test_asserts(test_server_native_launch)
  add_test(NAME test_server_native_launch COMMAND test_server_native_launch)
  add_executable(test_server_native_execution test_server_native_execution.c ../server/server_provider_cli_execute.c
    ../server/server_native_execution.c ../server/server_native_cache_lease.c ../platform_random.c ../server/server_native_launch.c ../server/cli_claude_native.c ../server/agent_request_build.c ../server/cli_session.c ../server/agent_adapter.c
    ../modules/ir/session_event.c ../modules/ir/aimee_ir.c ../aimee_sha256.c
    ../posix/workspace_provider.c ../util.c ../text.c ../aimee_home.c ../log.c
    ../vendor/cJSON.c ${PLATFORM_CORE_SRCS})
  target_include_directories(test_server_native_execution PRIVATE ${AIMEE_PROJECT_INCLUDE_DIRS}
    ${AIMEE_SRC_DIR}/modules/kb/c ${AIMEE_SRC_DIR}/db1_client)
  target_compile_options(test_server_native_execution PRIVATE -ffunction-sections -fdata-sections)
  # LTO must retain the same per-function sections at its final compile/link.
  target_link_options(test_server_native_execution PRIVATE -ffunction-sections -fdata-sections)
  if(APPLE)
    target_link_options(test_server_native_execution PRIVATE -Wl,-dead_strip)
  else()
    target_link_options(test_server_native_execution PRIVATE -Wl,--gc-sections)
  endif()
  target_link_libraries(test_server_native_execution PRIVATE
    aimee-core-connection Threads::Threads OpenSSL::Crypto m)
  aimee_enable_test_asserts(test_server_native_execution)
  add_test(NAME test_server_native_execution COMMAND test_server_native_execution)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(test_server_verification test_server_verification.c
      ../aimee_sha256.c ../util.c ../text.c ../vendor/cJSON.c)
    target_include_directories(test_server_verification PRIVATE ${AIMEE_PROJECT_INCLUDE_DIRS}
      ${AIMEE_SRC_DIR}/modules/kb/c ${AIMEE_SRC_DIR}/db1_client)
    target_compile_options(test_server_verification PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_server_verification PRIVATE -Wl,--gc-sections)
    target_link_libraries(test_server_verification PRIVATE aimee-core-codec Threads::Threads OpenSSL::Crypto)
    aimee_enable_test_asserts(test_server_verification)
    add_test(NAME test_server_verification COMMAND test_server_verification)
  endif()
  # The fixture includes all cli_v1_routes_*.c directly to test static marshallers;
  # link their extracted native client owner without duplicating those sources.
  add_executable(test_cli_v1_delegate test_cli_v1_delegate.c
    ../cli_native_bridge.c ../code_collect.c ../modules/workspace/workspace_client_diff.c
    ../vendor/cJSON.c ../cli_argspec.c ../posix/util.c ../aimee_client.c ../http_uds_client.c
    ../codex_auth.c ../posix/platform_path.c ../posix/platform_random.c
    ../posix/cli_client.c ../cli_client.c ../aimee_tls.c)
  target_include_directories(test_cli_v1_delegate PRIVATE ${AIMEE_PROJECT_INCLUDE_DIRS}
    ${AIMEE_SRC_DIR}/db1_client ${AIMEE_SRC_DIR}/modules/kb/c)
  target_compile_options(test_cli_v1_delegate PRIVATE -ffunction-sections -fdata-sections)
  if(APPLE)
    target_link_options(test_cli_v1_delegate PRIVATE -Wl,-dead_strip)
  else()
    target_link_options(test_cli_v1_delegate PRIVATE -Wl,--gc-sections)
  endif()
  target_link_libraries(test_cli_v1_delegate PRIVATE aimee-core-connection
    Threads::Threads OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB m)
  aimee_enable_test_asserts(test_cli_v1_delegate)
  add_test(NAME test_cli_v1_delegate COMMAND test_cli_v1_delegate)
  add_executable(test_server_native_cli test_server_native_cli.c ../server/server_native_cli.c
    ../modules/protocols/mcp/mcp_tools_extended.c ../aimee_sha256.c ../platform_random.c
    ../posix/platform_random.c ../vendor/cJSON.c)
  target_include_directories(test_server_native_cli PRIVATE ${AIMEE_PROJECT_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_SOURCE_DIR}/../headers)
  target_compile_options(test_server_native_cli PRIVATE -ffunction-sections -fdata-sections)
  if(APPLE)
    target_link_options(test_server_native_cli PRIVATE -Wl,-dead_strip)
  else()
    target_link_options(test_server_native_cli PRIVATE -Wl,--gc-sections)
  endif()
  target_link_libraries(test_server_native_cli PRIVATE aimee-core-connection Threads::Threads OpenSSL::Crypto)
  aimee_enable_test_asserts(test_server_native_cli)
  add_test(NAME test_server_native_cli COMMAND test_server_native_cli)
  add_executable(test_server_native_runtime test_server_native_runtime.c support/log_stub.c
    ../server/agent_tools.c ../posix/agent_source_authority.c ../modules/ir/aimee_ir.c
    ../server/server_native_runtime.c ../server/server_native_cli.c ../modules/protocols/mcp/mcp_tools_extended.c ../server/server_native_history.c ../server/server_native_tool_admission.c ../server/server_session_question.c ../server/server_native_tool_source.c ../server/server_session_capture.c ../server/server_session_control.c
    ../modules/ir/session_event.c ../cli_proxy.c ../server/request_context.c
    ../modules/translation/wire_codec.c
    ../modules/translation/aimee_frontend_anthropic.c ../modules/translation/aimee_frontend_openai.c
    ../modules/translation/aimee_frontend_responses.c ../modules/translation/aimee_backend_anthropic.c
    ../modules/translation/aimee_backend_openai.c ../modules/translation/aimee_backend_responses.c
    ../aimee_sha256.c ../platform_random.c ../posix/platform_random.c ../vendor/cJSON.c)
  target_include_directories(test_server_native_runtime PRIVATE ${AIMEE_PROJECT_INCLUDE_DIRS}
    ${AIMEE_SRC_DIR}/modules/kb/c
    ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_SOURCE_DIR}/../headers
    ${CMAKE_CURRENT_SOURCE_DIR}/../db1_client ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/headers)
  # Match Make's progress-owner fixture: only durable-store calls are peers.
  add_library(test_server_native_job_progress OBJECT ../server/agent_tasks.c)
  target_include_directories(test_server_native_job_progress PRIVATE
    $<TARGET_PROPERTY:test_server_native_runtime,INCLUDE_DIRECTORIES>)
  target_compile_definitions(test_server_native_job_progress PRIVATE
    db1_agent_job_get=fixture_job_get
    db1_agent_job_free=fixture_job_free
    db1_agent_job_heartbeat_ext=fixture_job_heartbeat_ext)
  target_compile_options(test_server_native_job_progress PRIVATE -ffunction-sections -fdata-sections)
  target_sources(test_server_native_runtime PRIVATE ../wire_fence.c
    $<TARGET_OBJECTS:test_server_native_job_progress>)
  target_link_libraries(test_server_native_runtime PRIVATE
    aimee-core-connection Threads::Threads OpenSSL::Crypto m)
  target_compile_options(test_server_native_runtime PRIVATE -ffunction-sections -fdata-sections)
  # LTO must retain the same per-function sections at its final compile/link.
  target_link_options(test_server_native_runtime PRIVATE -ffunction-sections -fdata-sections)
  if(APPLE)
    target_link_options(test_server_native_runtime PRIVATE -Wl,-dead_strip)
  else()
    target_link_options(test_server_native_runtime PRIVATE -Wl,--gc-sections)
  endif()
  aimee_enable_test_asserts(test_server_native_runtime)
  add_test(NAME test_server_native_runtime COMMAND test_server_native_runtime)
endif()

aimee_add_test(test_harness_ir test_harness_ir.c
  ../modules/translation/harness_ir.c ../modules/translation/aimee_ir_stream.c
  ../modules/translation/aimee_backend_anthropic.c ../modules/translation/aimee_frontend_anthropic.c
  ../modules/ir/session_event.c ../modules/ir/aimee_ir.c ../aimee_sha256.c)
target_link_libraries(test_harness_ir PRIVATE OpenSSL::Crypto)
if(NOT WIN32)
  aimee_add_test(test_claude_headless test_claude_headless.c ../server/claude_headless.c ../server/cli_claude_native.c ../server/server_session_control.c
    ../modules/translation/harness_ir.c ../modules/translation/aimee_ir_stream.c
    ../modules/translation/aimee_backend_anthropic.c ../modules/translation/aimee_frontend_anthropic.c
    ../modules/ir/session_event.c ../modules/ir/aimee_ir.c ../aimee_sha256.c)
  target_link_libraries(test_claude_headless PRIVATE OpenSSL::Crypto)
endif()

if(NOT WIN32)
  aimee_add_test(test_server_chat_ir test_server_chat_ir.c ../server/server_chat_ir.c ../server/server_session_capture.c ../server/server_session_control.c
    ../server/server_learning_outcome.c support/learning_outcome_stub.c support/log_stub.c
    ../interaction_event_names.c ../modules/ir/session_event.c ../modules/ir/aimee_ir.c)
  target_link_libraries(test_server_chat_ir PRIVATE OpenSSL::Crypto)
endif()

# Owner-checked canonical primary-history fixtures.
if(NOT WIN32)
  add_executable(test_primary_session_adapter
    test_primary_session_adapter.c ../server/primary_session_adapter.c
    ../server/agent_adapter.c support/provider_cli_adapter_stub.c support/log_stub.c
    ../modules/translation/history_ir.c
    ../modules/translation/aimee_frontend_anthropic.c
    ../modules/translation/aimee_backend_anthropic.c ../aimee_sha256.c
    ../modules/translation/aimee_frontend_openai.c
    ../modules/translation/aimee_frontend_responses.c
    ../modules/translation/aimee_backend_responses.c
    ../modules/ir/aimee_ir.c ../modules/ir/session_event.c
    ../modules/ir/aimee_ir_metrics.c ../text.c ../util.c ../vendor/cJSON.c)
  add_executable(test_primary_history_client test_primary_history_client.c
    ../db1_client/transport.c ../db1_client/sessions.c ../primary_session_release.c
    ../module_json_call.c ../log.c)
  foreach(history_test test_primary_session_adapter test_primary_history_client)
    target_include_directories(${history_test} PRIVATE ${AIMEE_PROJECT_INCLUDE_DIRS}
      ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_SOURCE_DIR}/../headers
      ${CMAKE_CURRENT_SOURCE_DIR}/../modules/kb/c
      ${CMAKE_CURRENT_SOURCE_DIR}/../db1_client ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/headers)
    target_compile_options(${history_test} PRIVATE -ffunction-sections -fdata-sections)
    if(APPLE)
      target_link_options(${history_test} PRIVATE -Wl,-dead_strip)
    else()
      target_link_options(${history_test} PRIVATE -Wl,--gc-sections)
    endif()
    target_link_libraries(${history_test} PRIVATE Threads::Threads OpenSSL::Crypto m)
    aimee_enable_test_asserts(${history_test})
    add_test(NAME ${history_test} COMMAND ${history_test})
    set_tests_properties(${history_test} PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  endforeach()
endif()

if(NOT WIN32)
  add_executable(test_server_session_capture test_server_session_capture.c
    ../server/server_session_capture.c ../server/server_session_control.c
    ../modules/ir/session_event.c ../modules/ir/aimee_ir.c ../vendor/cJSON.c)
  target_include_directories(test_server_session_capture PRIVATE ${AIMEE_PROJECT_INCLUDE_DIRS}
    ${AIMEE_SRC_DIR}/modules/kb/c ${AIMEE_SRC_DIR}/db1_client)
  target_link_libraries(test_server_session_capture PRIVATE Threads::Threads m)
  aimee_enable_test_asserts(test_server_session_capture)
  add_test(NAME test_server_session_capture COMMAND test_server_session_capture)
  add_executable(test_server_mcp_delegate test_server_mcp_delegate.c
    ../server/server_mcp_delegate.c ../vendor/cJSON.c)
  target_include_directories(test_server_mcp_delegate PRIVATE ${AIMEE_PROJECT_INCLUDE_DIRS}
    ${AIMEE_SRC_DIR}/modules/kb/c ${AIMEE_SRC_DIR}/db1_client)
  aimee_enable_test_asserts(test_server_mcp_delegate)
  add_test(NAME test_server_mcp_delegate COMMAND test_server_mcp_delegate)
  add_executable(test_server_session_question test_server_session_question.c
    ../server/server_session_question.c ../server/server_session_control.c
    ../server/server_compute_mailbox.c
    ../modules/ir/session_event.c ../modules/ir/aimee_ir.c
    ../platform_random.c ../posix/platform_random.c ../vendor/cJSON.c)
  target_include_directories(test_server_session_question PRIVATE ${AIMEE_PROJECT_INCLUDE_DIRS}
    ${AIMEE_SRC_DIR} ${AIMEE_SRC_DIR}/headers ${AIMEE_SRC_DIR}/vendor/headers
    ${AIMEE_SRC_DIR}/modules/kb/c ${AIMEE_SRC_DIR}/db1_client)
  target_link_libraries(test_server_session_question PRIVATE Threads::Threads m)
  if(APPLE)
    target_compile_options(test_server_session_question PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_server_session_question PRIVATE -Wl,-dead_strip)
  else()
    target_compile_options(test_server_session_question PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_server_session_question PRIVATE -Wl,--gc-sections)
  endif()
  aimee_enable_test_asserts(test_server_session_question)
  add_test(NAME test_server_session_question COMMAND test_server_session_question)
  aimee_add_test(test_server_session_control test_server_session_control.c
    ../server/server_session_control.c ../modules/ir/session_event.c
    ../modules/ir/aimee_ir.c ../vendor/cJSON.c)
  target_link_libraries(test_server_session_control PRIVATE Threads::Threads)
  aimee_add_test(test_server_session_log test_server_session_log.c
    ../server/server_session_log.c ../server/server_session_capture.c ../server/server_session_control.c
    ../server/server_session_question.c ../platform_random.c ../posix/platform_random.c
    ../server/server_learning_outcome.c support/learning_outcome_stub.c support/log_stub.c
    ../interaction_event_names.c ../modules/ir/session_event.c ../modules/ir/aimee_ir.c
    ../aimee_sha256.c)
  target_link_libraries(test_server_session_log PRIVATE Threads::Threads OpenSSL::Crypto)
endif()
# Tenant entry is stubbed to exercise every HTTP failure mapping without a DB.
add_executable(test_kb_http_common test_kb_http_common.c
  ../kb/http/kb_http_common.c ../vendor/cJSON.c)
target_include_directories(test_kb_http_common PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../headers
  ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/headers)
aimee_enable_test_asserts(test_kb_http_common)
add_test(NAME test_kb_http_common COMMAND test_kb_http_common)

aimee_add_test(test_core_base64 test_core_base64.c)

aimee_add_test(test_aimee_responses_stream test_aimee_responses_stream.c
    ../modules/translation/aimee_responses_frontend_stream.c ../modules/translation/aimee_frontend_openai.c ../modules/translation/aimee_frontend_responses.c
    ../modules/translation/aimee_backend_responses.c
    ../modules/ir/aimee_ir.c)

aimee_add_test(test_server_responses_stream test_server_responses_stream.c
    ../server/server_responses_stream.c ../sse_parser.c
    ../modules/translation/aimee_backend_responses.c
    ../modules/translation/aimee_responses_frontend_stream.c ../modules/translation/aimee_frontend_openai.c
    ../modules/translation/aimee_frontend_responses.c ../modules/ir/aimee_ir.c)

if(NOT WIN32)
  aimee_add_test(test_server_native_cache test_server_native_cache.c
      ../server/server_native_cache.c ../server/server_native_history.c ../modules/ir/session_event.c ../modules/ir/aimee_ir.c
      ../aimee_sha256.c ../platform_random.c ../posix/platform_random.c)
  target_include_directories(test_server_native_cache PRIVATE ${AIMEE_SRC_DIR}/modules/kb/c)
  target_link_libraries(test_server_native_cache PRIVATE OpenSSL::Crypto)
  aimee_add_test(test_server_native_cache_lease test_server_native_cache_lease.c
      ../server/server_native_cache_lease.c ../platform_random.c ../posix/platform_random.c)
endif()

if(NOT WIN32)
  aimee_add_test(test_agent_result_cache test_agent_result_cache.c
    ../server/agent_result_cache.c ../server/agent_runtime_liveness.c ../server/liveness.c
    ../modules/ir/response_codec.c ../modules/ir/session_event.c ../modules/ir/aimee_ir.c
    ../modules/ir/aimee_ir_metrics.c ../aimee_sha256.c)
  target_link_libraries(test_agent_result_cache PRIVATE OpenSSL::Crypto)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  aimee_add_test(test_server_native_containment test_server_native_containment.c
    ../server/server_native_containment.c ../aimee_sha256.c
    ../platform_random.c ../posix/platform_random.c)
  target_link_libraries(test_server_native_containment PRIVATE OpenSSL::Crypto Threads::Threads)
endif()

if(NOT WIN32)
  aimee_add_test(test_server_native_history test_server_native_history.c
    ../server/server_native_history.c ../modules/ir/session_event.c ../modules/ir/aimee_ir.c ../aimee_sha256.c)
  target_include_directories(test_server_native_history PRIVATE ${AIMEE_SRC_DIR}/db1_client)
  target_link_libraries(test_server_native_history PRIVATE OpenSSL::Crypto)
  aimee_add_test(test_server_unknown_effect_policy test_server_unknown_effect_policy.c
    ../server/server_unknown_effect_policy.c ../server/server_session_control.c
    ../modules/ir/session_event.c ../modules/ir/aimee_ir.c ../aimee_sha256.c)
  target_link_libraries(test_server_unknown_effect_policy PRIVATE OpenSSL::Crypto Threads::Threads)
endif()

if(NOT WIN32)
  aimee_add_test(test_server_unknown_effect_action test_server_unknown_effect_action.c
    ../server/server_unknown_effect_policy.c ../server/server_governed_action.c ../server/server_session_control.c
    ../modules/ir/session_event.c ../modules/ir/aimee_ir.c ../aimee_sha256.c)
  target_link_libraries(test_server_unknown_effect_action PRIVATE OpenSSL::Crypto Threads::Threads)
endif()

if(NOT WIN32)
  add_executable(test_coord_dispatcher_startup test_coord_dispatcher_startup.c
    support/log_stub.c ../vendor/cJSON.c)
  add_executable(test_runtime_state_lookup test_runtime_state_lookup.c)
  add_executable(test_delegate_startup test_delegate_startup.c)
  foreach(target test_coord_dispatcher_startup test_runtime_state_lookup test_delegate_startup)
    target_include_directories(${target} PRIVATE ${AIMEE_PROJECT_INCLUDE_DIRS}
      ${AIMEE_SRC_DIR} ${AIMEE_SRC_DIR}/headers ${AIMEE_SRC_DIR}/vendor/headers
      ${AIMEE_SRC_DIR}/db1_client ${AIMEE_SRC_DIR}/modules/kb/c)
    target_compile_options(${target} PRIVATE -ffunction-sections -fdata-sections)
    if(APPLE)
      target_link_options(${target} PRIVATE -Wl,-dead_strip)
    else()
      target_link_options(${target} PRIVATE -Wl,--gc-sections)
    endif()
    aimee_enable_test_asserts(${target})
    add_test(NAME ${target} COMMAND ${target})
  endforeach()
  target_link_libraries(test_coord_dispatcher_startup PRIVATE Threads::Threads m)
  target_link_libraries(test_delegate_startup PRIVATE Threads::Threads)
endif()
