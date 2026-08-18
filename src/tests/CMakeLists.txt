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
    ${CMAKE_CURRENT_SOURCE_DIR}/../modules/config
    ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/headers)
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
aimee_add_test(test_memory_provider test_memory_provider.c)
aimee_add_test(test_harness_memory test_harness_memory.c)
aimee_add_test(test_memory_redirect test_memory_redirect.c)
aimee_add_test(test_roundtable_brief test_roundtable_brief.c)
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../modules/config
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../modules/config
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
  ${AIMEE_SRC_DIR}/db2
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
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../modules/config)
  aimee_enable_test_asserts(test_vault_bootstrap_privilege)
  add_test(NAME test_vault_bootstrap_privilege COMMAND test_vault_bootstrap_privilege)
  set_tests_properties(test_vault_bootstrap_privilege PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

  add_executable(test_git_oauth_gh test_git_oauth_gh.c
    ../modules/git/git_oauth_gh.c)
  target_include_directories(test_git_oauth_gh PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../headers
    ${CMAKE_CURRENT_SOURCE_DIR}/../modules/config
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../modules/config
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
  ../core/event_bus/module_protocol.c)
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

aimee_add_test(test_bus_flow test_bus_flow.c ../core/event_bus/bus_attach.c ../core/event_bus/bus_host.c ../core/event_bus/bus_route.c ../core/event_bus/bus_region.c ../core/event_bus/bus_region_host.c ../core/event_bus/bus_ring.c ../core/event_bus/bus_arena.c ../core/event_bus/bus_wire.c)
target_include_directories(test_bus_flow PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core/event_bus/include)

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
aimee_add_test(test_db2_module_contract
  test_db2_module_contract.c
  ../modules/db2/client/generated.c
  ../modules/db2/module_adapter.c)
target_include_directories(test_db2_module_contract PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../core/event_bus/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../modules/db2
  ${CMAKE_CURRENT_SOURCE_DIR}/../modules/db2/include)
aimee_add_test(test_bus_db2_module
  test_bus_db2_module.c
  ../modules/db2/client/generated.c
  ../modules/db2/module_adapter.c)
target_include_directories(test_bus_db2_module PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../core/event_bus/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../modules/db2
  ${CMAKE_CURRENT_SOURCE_DIR}/../modules/db2/include)
target_link_libraries(test_bus_db2_module PRIVATE aimee-core-event-bus Threads::Threads)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(test_bus_db2_process
    test_bus_db2_process.c
    ../modules/db2/client/generated.c)
  target_include_directories(test_bus_db2_process PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/event_bus/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../modules/db2/include)
  target_link_libraries(test_bus_db2_process PRIVATE aimee-core-event-bus Threads::Threads)
endif()
aimee_add_test(test_db3_route
  test_db3_route.c
  ../modules/db2/db3_route.c)
target_include_directories(test_db3_route PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../modules/db2/include)
target_link_libraries(test_db3_route PRIVATE m)
aimee_add_test(test_bus_db3
  test_bus_db3.c
  ../modules/db2/db3_route.c)
target_include_directories(test_bus_db3 PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../core/event_bus/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../modules/db2/include)
target_link_libraries(test_bus_db3 PRIVATE aimee-core-event-bus Threads::Threads m)
aimee_add_test(test_bus_capture test_bus_capture.c ../core/event_bus/bus_capture.c ../core/event_bus/bus_client.c ../core/event_bus/bus_attach.c ../core/event_bus/bus_host.c ../core/event_bus/bus_route.c ../core/event_bus/bus_region.c ../core/event_bus/bus_region_host.c ../core/event_bus/bus_ring.c ../core/event_bus/bus_arena.c ../core/event_bus/bus_wire.c)
target_include_directories(test_bus_capture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core/event_bus/include)
target_link_libraries(test_bus_capture PRIVATE Threads::Threads)

# First real module-on-bus integration: the DB1-backed user-memory recall served
# over the bus and verified against a direct call (feature-tree memory-first slice).
# The bus sources are compiled straight into this test binary; the real DB1 memory
# path comes from aimee-core, which aimee_add_test links. A test binary only — D7
# keeps the bus out of every shipping target.
aimee_add_test(test_bus_memory_recall test_bus_memory_recall.c ../core/event_bus/bus_client.c ../core/event_bus/bus_attach.c ../core/event_bus/bus_host.c ../core/event_bus/bus_route.c ../core/event_bus/bus_region.c ../core/event_bus/bus_region_host.c ../core/event_bus/bus_ring.c ../core/event_bus/bus_arena.c ../core/event_bus/bus_wire.c)
target_include_directories(test_bus_memory_recall PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core/event_bus/include)
target_link_libraries(test_bus_memory_recall PRIVATE Threads::Threads)

# Real mutating memory op over the bus: upsert (insert + update), verified by
# reading the real store back. Same rationale as the recall test above (D7-clean).
aimee_add_test(test_bus_memory_upsert test_bus_memory_upsert.c ../core/event_bus/bus_client.c ../core/event_bus/bus_attach.c ../core/event_bus/bus_host.c ../core/event_bus/bus_route.c ../core/event_bus/bus_region.c ../core/event_bus/bus_region_host.c ../core/event_bus/bus_ring.c ../core/event_bus/bus_arena.c ../core/event_bus/bus_wire.c)
target_include_directories(test_bus_memory_upsert PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core/event_bus/include)
target_link_libraries(test_bus_memory_upsert PRIVATE Threads::Threads)

# A second, distinct module over the bus: real config_autonomy_lookup vs a direct
# call, incl. a live env-override case. Config deps come from aimee-core. D7-clean.
aimee_add_test(test_bus_config_autonomy test_bus_config_autonomy.c ../core/event_bus/bus_client.c ../core/event_bus/bus_attach.c ../core/event_bus/bus_host.c ../core/event_bus/bus_route.c ../core/event_bus/bus_region.c ../core/event_bus/bus_region_host.c ../core/event_bus/bus_ring.c ../core/event_bus/bus_arena.c ../core/event_bus/bus_wire.c)
target_include_directories(test_bus_config_autonomy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core/event_bus/include)
target_link_libraries(test_bus_config_autonomy PRIVATE Threads::Threads)

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
aimee_add_test(test_db test_db.c)
aimee_add_test(test_config test_config.c)
aimee_add_test(test_cron_config test_cron_config.c ../modules/config/config_trigger.c)
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
aimee_add_test(test_wfe_foreach test_wfe_foreach.c)
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
aimee_add_test(test_aimee_frontend test_aimee_frontend.c ../modules/translation/aimee_frontend_anthropic.c ../modules/translation/aimee_frontend_openai.c ../modules/translation/aimee_frontend_responses.c ../modules/ir/aimee_ir.c)
aimee_add_test(test_aimee_backend test_aimee_backend.c ../modules/translation/aimee_backend_anthropic.c ../modules/translation/aimee_backend_openai.c ../modules/translation/aimee_backend_responses.c ../modules/translation/aimee_frontend_anthropic.c ../modules/translation/aimee_frontend_openai.c ../modules/ir/aimee_ir.c)
aimee_add_test(test_aimee_ir_shadow test_aimee_ir_shadow.c ../server/aimee_ir_shadow.c ../modules/ir/aimee_ir_metrics.c ../modules/translation/aimee_backend_anthropic.c ../modules/translation/aimee_frontend_anthropic.c ../modules/translation/aimee_frontend_openai.c ../modules/ir/aimee_ir.c)
aimee_add_test(test_aimee_ir_serve test_aimee_ir_serve.c ../server/aimee_ir_serve.c ../modules/ir/aimee_ir_session.c ../modules/translation/aimee_backend_openai.c ../modules/translation/aimee_backend_responses.c ../modules/translation/aimee_frontend_anthropic.c ../modules/translation/aimee_frontend_responses.c ../modules/ir/aimee_ir.c ../modules/ir/aimee_ir_metrics.c)
aimee_add_test(test_aimee_ir_stream test_aimee_ir_stream.c ../modules/translation/aimee_ir_stream.c ../modules/ir/aimee_ir.c)
aimee_add_test(test_wfe_binding test_wfe_binding.c)
aimee_add_test(test_shutdown_forensics test_shutdown_forensics.c)

add_executable(test_toolset test_toolset.c)
target_include_directories(test_toolset PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../headers
    ${CMAKE_CURRENT_SOURCE_DIR}/../modules/config
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
add_executable(test_db2_pool test_db2_pool.c ../db2/db2_pool.c support/log_stub.c)
aimee_enable_test_asserts(test_db2_pool)
target_link_libraries(test_db2_pool PRIVATE Threads::Threads)
add_test(NAME test_db2_pool COMMAND test_db2_pool)
target_include_directories(test_compute_pool PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../headers
    ${CMAKE_CURRENT_SOURCE_DIR}/../modules/config
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../modules/config
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../modules/config
  ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/headers)
aimee_enable_test_asserts(test_server_session_pools)
if(WIN32)
  target_compile_definitions(test_server_session_pools PRIVATE AIMEE_WINDOWS _WIN32_WINNT=0x0A00)
endif()
target_link_libraries(test_server_session_pools PRIVATE Threads::Threads)
add_test(NAME test_server_session_pools COMMAND test_server_session_pools)
set_tests_properties(test_server_session_pools PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(test_cmd_table test_cmd_table.c)
target_include_directories(test_cmd_table PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../headers
    ${CMAKE_CURRENT_SOURCE_DIR}/../modules/config
  ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/headers)
aimee_enable_test_asserts(test_cmd_table)
target_link_libraries(test_cmd_table PRIVATE aimee-core)
add_test(NAME test_cmd_table COMMAND test_cmd_table)
set_tests_properties(test_cmd_table PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# test_prompts needs prompts.c + dstr.c, and calls working_profile_observe /
# working_profile_autoobserve_from_feedback directly — those live in aimee-agent.
add_executable(test_prompts test_prompts.c ../prompts.c ../dstr.c)
target_include_directories(test_prompts PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../headers
    ${CMAKE_CURRENT_SOURCE_DIR}/../modules/config
  ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/headers)
aimee_enable_test_asserts(test_prompts)
if(TARGET aimee-core)
  if(NOT WIN32)
    target_link_libraries(test_prompts PRIVATE
      -Wl,--start-group aimee-agent aimee-data aimee-core -Wl,--end-group)
  else()
    target_link_libraries(test_prompts PRIVATE aimee-agent aimee-data aimee-core)
  endif()
endif()
add_test(NAME test_prompts COMMAND test_prompts)
set_tests_properties(test_prompts PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(test_agent_http test_agent_http.c)
target_include_directories(test_agent_http PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../headers
    ${CMAKE_CURRENT_SOURCE_DIR}/../modules/config
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../modules/config
  ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/headers)
aimee_enable_test_asserts(test_kb_lab)
target_link_libraries(test_kb_lab PRIVATE m)
add_test(NAME test_kb_lab COMMAND test_kb_lab)
set_tests_properties(test_kb_lab PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# test_kb_mdl: MDL scoring for synthesis candidate selection.
# Needs kb_mdl.c (aimee-data) + config_learning.c (aimee-core) + zstd.
add_executable(test_kb_mdl test_kb_mdl.c ../kb/kb_mdl.c ../modules/config/config_learning.c)
target_include_directories(test_kb_mdl PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../headers
    ${CMAKE_CURRENT_SOURCE_DIR}/../modules/config
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

add_executable(test_compact_prune test_compact_prune.c)
target_include_directories(test_compact_prune PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../headers
    ${CMAKE_CURRENT_SOURCE_DIR}/../modules/config
  ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/headers)
aimee_enable_test_asserts(test_compact_prune)
if(TARGET aimee-agent)
  if(NOT WIN32)
    target_link_libraries(test_compact_prune PRIVATE
      -Wl,--start-group aimee-agent aimee-data aimee-core -Wl,--end-group)
  else()
    target_link_libraries(test_compact_prune PRIVATE aimee-agent aimee-data aimee-core)
  endif()
endif()
add_test(NAME test_compact_prune COMMAND test_compact_prune)
set_tests_properties(test_compact_prune PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(test_session_compact_focused test_session_compact_focused.c)
target_include_directories(test_session_compact_focused PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../headers
    ${CMAKE_CURRENT_SOURCE_DIR}/../modules/config
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

add_executable(test_conversation_context test_conversation_context.c)
target_include_directories(test_conversation_context PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../headers
    ${CMAKE_CURRENT_SOURCE_DIR}/../modules/config
  ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/headers)
aimee_enable_test_asserts(test_conversation_context)
if(TARGET aimee-agent)
  if(NOT WIN32)
    target_link_libraries(test_conversation_context PRIVATE
      -Wl,--start-group aimee-agent aimee-data aimee-core -Wl,--end-group)
  else()
    target_link_libraries(test_conversation_context PRIVATE aimee-agent aimee-data aimee-core)
  endif()
endif()
add_test(NAME test_conversation_context COMMAND test_conversation_context)
set_tests_properties(test_conversation_context PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
