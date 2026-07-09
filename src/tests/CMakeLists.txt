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
add_executable(test_plugin_loader test_plugin_loader.c)
target_include_directories(test_plugin_loader PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../headers
  ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/headers)
aimee_enable_test_asserts(test_plugin_loader)
if(TARGET aimee-agent)
  if(NOT WIN32)
    target_link_libraries(test_plugin_loader PRIVATE
      -Wl,--start-group aimee-agent aimee-data aimee-core -Wl,--end-group)
  else()
    target_link_libraries(test_plugin_loader PRIVATE aimee-agent aimee-data aimee-core)
  endif()
elseif(TARGET aimee-core)
  target_link_libraries(test_plugin_loader PRIVATE aimee-core)
endif()
add_test(NAME test_plugin_loader COMMAND test_plugin_loader)
set_tests_properties(test_plugin_loader PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
aimee_add_test(test_memory_provider test_memory_provider.c)
aimee_add_test(test_harness_memory test_harness_memory.c)
aimee_add_test(test_memory_redirect test_memory_redirect.c)
aimee_add_test(test_roundtable_brief test_roundtable_brief.c)
aimee_add_test(test_harness_memory_scope test_harness_memory_scope.c)
aimee_add_test(test_harness_memory_spill test_harness_memory_spill.c)
aimee_add_test(test_harness_memory_audit test_harness_memory_audit.c)
aimee_add_test(test_plugin_c_hook test_plugin_c_hook.c)
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
aimee_add_test(test_audit_action test_audit_action.c)
aimee_add_test(test_audit_action_log test_audit_action_log.c)
aimee_add_test(test_audit_ledger test_audit_ledger.c)
aimee_add_test(test_db test_db.c)
aimee_add_test(test_config test_config.c)
aimee_add_test(test_cron_config test_cron_config.c ../config_trigger.c)
aimee_add_test(test_json_fluent test_json_fluent.c)
aimee_add_test(test_render test_render.c)
aimee_add_test(test_text test_text.c)
aimee_add_test(test_report_enrichment test_report_enrichment.c)
aimee_add_test(test_delivery_target test_delivery_target.c)
aimee_add_test(test_delegate_plan test_delegate_plan.c ../server/delegate_plan.c)
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
aimee_add_test(test_aimee_ir test_aimee_ir.c ../server/aimee_ir.c)
aimee_add_test(test_aimee_ir_metrics test_aimee_ir_metrics.c ../server/aimee_ir_metrics.c)
aimee_add_test(test_aimee_frontend test_aimee_frontend.c ../server/aimee_frontend_anthropic.c ../server/aimee_frontend_openai.c ../server/aimee_frontend_responses.c ../server/aimee_ir.c)
aimee_add_test(test_aimee_backend test_aimee_backend.c ../server/aimee_backend_anthropic.c ../server/aimee_backend_openai.c ../server/aimee_backend_responses.c ../server/aimee_frontend_anthropic.c ../server/aimee_frontend_openai.c ../server/aimee_ir.c)
aimee_add_test(test_aimee_ir_shadow test_aimee_ir_shadow.c ../server/aimee_ir_shadow.c ../server/aimee_ir_metrics.c ../server/aimee_backend_anthropic.c ../server/aimee_frontend_anthropic.c ../server/aimee_frontend_openai.c ../server/aimee_ir.c)
aimee_add_test(test_aimee_ir_serve test_aimee_ir_serve.c ../server/aimee_ir_serve.c ../server/aimee_backend_openai.c ../server/aimee_backend_responses.c ../server/aimee_frontend_anthropic.c ../server/aimee_frontend_responses.c ../server/aimee_ir.c ../server/aimee_ir_metrics.c)
aimee_add_test(test_aimee_ir_stream test_aimee_ir_stream.c ../server/aimee_ir_stream.c ../server/aimee_ir.c)
aimee_add_test(test_wfe_binding test_wfe_binding.c)
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
add_executable(test_db2_pool test_db2_pool.c ../db2/db2_pool.c support/log_stub.c)
aimee_enable_test_asserts(test_db2_pool)
target_link_libraries(test_db2_pool PRIVATE Threads::Threads)
add_test(NAME test_db2_pool COMMAND test_db2_pool)
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

add_executable(test_cmd_table test_cmd_table.c)
target_include_directories(test_cmd_table PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../headers
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
add_executable(test_kb_mdl test_kb_mdl.c ../kb/kb_mdl.c ../config_learning.c)
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

add_executable(test_compact_prune test_compact_prune.c)
target_include_directories(test_compact_prune PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../headers
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
