
# The daemon no longer links DB1's connection lifecycle -- see DB1_SRCS in the
# Makefile for why. Tests still need it: they open ":memory:" and link the
# domain objects directly, which is the right way to test a store without
# standing a module up beside every case.
DB1_TEST_OBJS = $(DB1_OBJS) $(OBJDIR)/modules/db1/db1_init.o \
                $(OBJDIR)/modules/db1/db1_write.o $(OBJDIR)/modules/db1/diagnostics.o

TEST_C_FLAGS = $(C_FLAGS) -I.. -Igateway
TEST_L_FLAGS = $(L_FULL) $(DB2_TEST_BACKEND_LIB)

# Which object supplies the aimee_pg_* surface to test binaries.
#
# The sqlite shim and the real libpq layer export the SAME 33 aimee_pg_* symbols,
# so the backend is a LINK-time choice, not a runtime one -- a binary carrying
# both would not link. Every test recipe therefore names this variable rather
# than either object directly.
#
# Default: the sqlite shim, which translates DB2's SQL and needs no server.
# AIMEE_TEST_PG=1: the real libpq layer, so the same tests exercise postgres.
# Nothing else about a recipe changes, which is the point -- there is one set of
# test binaries, built against one backend or the other.
DB2_TEST_BACKEND_OBJ = $(OBJDIR)/tests/aimee_pg_sqlite_shim.o
DB2_TEST_BACKEND_LIB =
ifeq ($(AIMEE_TEST_PG),1)
# entity_edges.o rides along because db2_init calls db2_entity_edge_build_unique_index
# on the not-a-shim path. Under the sqlite shim aimee_pg_is_shim() folds to a
# constant and LTO drops that branch with its callee, so most recipes never had to
# name the object; against real libpq the branch is live and 54 of them stop
# linking. Carrying it with the backend keeps the choice to this one place instead
# of editing every recipe.
DB2_TEST_BACKEND_OBJ = $(OBJDIR)/db2/db_postgres.o $(OBJDIR)/db2/entity_edges.o
DB2_TEST_BACKEND_LIB = $(PQ_LIB)
# Lets the shim tell "you forgot AIMEE_TEST_DB2_TEMPLATE_URL" apart from "you
# wanted sqlite", which are otherwise the same code path and abort identically.
# Target-specific so only the shim sees it, rather than redefining every object.
$(OBJDIR)/db2/db2_test_shim.o: C_FLAGS += -DAIMEE_TEST_PG_BACKEND=1
endif

# db2_test_shim.o's flags depend on AIMEE_TEST_PG, and a flag change is invisible to
# make: its source and headers are untouched, so switching modes in the same OBJDIR
# reuses the object built for the OTHER backend. Everything then links and 90-odd
# binaries abort at startup with "built with AIMEE_TEST_PG=1 ... but
# AIMEE_TEST_DB2_TEMPLATE_URL is unset" -- a wall of failures with nothing wrong in
# the tree. Depend on a stamp named after the mode: flipping modes deletes the other
# stamp and creates this one, which is then newer than the object and forces the
# rebuild (and the relink that follows it). Same mode twice in a row rebuilds nothing.
DB2_TEST_BACKEND_STAMP = \
    $(OBJDIR)/tests/.db2-test-backend-$(if $(filter 1,$(AIMEE_TEST_PG)),pg,sqlite)
$(DB2_TEST_BACKEND_STAMP):
	@mkdir -p $(dir $@) && rm -f $(OBJDIR)/tests/.db2-test-backend-* && touch $@
$(OBJDIR)/db2/db2_test_shim.o: $(DB2_TEST_BACKEND_STAMP)

# Test output prefix: defaults to $(OBJDIR)/tests so any `make unit-tests`
# invocation with a non-default OBJDIR (sanitizers, coverage, build-integrity,
# ...) automatically gets an isolated test-binary directory. Prevents parallel
# verify steps from racing to link the same tests/unit-test-* paths.
TESTPREFIX ?= $(OBJDIR)/tests

# Native unit tests talk to a deterministic in-process peer implementing the
# same JSON event contract as the independently running Go module. This is a bus
# double, not a second configuration engine: the golden defaults remain owned
# by and loaded from the pinned external Go module's defaults.json.
CONFIG_CLIENT_ACCESSOR_OBJS = $(OBJDIR)/config_client_accessors_0.o \
                              $(OBJDIR)/config_client_accessors_1.o \
                              $(OBJDIR)/config_client_accessors_2.o \
                              $(OBJDIR)/config_client_accessors_3.o \
                              $(OBJDIR)/config_client_accessors_4.o \
                              $(OBJDIR)/config_client_accessors_5.o \
                              $(OBJDIR)/config_client_accessors_6.o \
                              $(OBJDIR)/config_client_accessors_7.o
CONFIG_CLIENT_TEST_OBJS = $(OBJDIR)/config_client.o $(OBJDIR)/config_client_contract.o \
                          $(CONFIG_CLIENT_ACCESSOR_OBJS) \
                          $(OBJDIR)/tests/support/config_module_stub.o $(OBJDIR)/session_id.o
CONFIG_ACCESSOR_OBJS = $(CONFIG_CLIENT_TEST_OBJS)
TESTLINK = @mkdir -p $(dir $@) && $(CC)
TESTLINK_MIN = @mkdir -p $(dir $@) && $(CC)
# Test execution parallelism. The unit-tests target used to run binaries in a
# serial shell loop; most tests are process-isolated and safe to fan out.
# Keep it overridable so flaky-debug sessions can force TEST_RUN_JOBS=1.
TEST_RUN_JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 4)

# obs_bus now exposes the authenticated external-module listener. Focused tests
# link the bus as loose objects, so keep its two new implementation objects in
# one fixture rather than duplicating them in every target below.
OBS_BUS_LINK_OBJS = $(OBJDIR)/modules/audit/obs_bus.o \
                    $(OBJDIR)/core/event_bus/bus_runtime.o \
                    $(OBJDIR)/core/event_bus/bus_endpoint.o \
                    $(OBJDIR)/core/event_bus/module_client.o \
                    $(OBJDIR)/core/event_bus/module_protocol.o

.PHONY: unit-test-server-management-tls
unit-test-server-management-tls: $(TESTPREFIX)/unit-test-server-management-tls
	$<

$(TESTPREFIX)/unit-test-server-management-tls: $(OBJDIR)/tests/test_server_management_tls.o \
                                                $(OBJDIR)/server/server_tls.o \
                                                $(OBJDIR)/cJSON.o \
                                                $(CORE_CONNECTION_LIB)
	$(TESTLINK_MIN) -o $@ $^ $(EXTRA_L_FLAGS) -lssl -lcrypto -lpthread

.PHONY: unit-test-server-management-listener-live
unit-test-server-management-listener-live: $(TESTPREFIX)/unit-test-server-management-listener-live
	$<

P5B3C_LIVE_SERVER_OBJS = $(filter-out $(OBJDIR)/server/server_main.o,$(SERVER_OBJS)) \
    $(OBS_BUS_LINK_OBJS) $(OBJDIR)/modules/audit/audit_replay.o \
    $(OBJDIR)/core/event_bus/bus_client.o $(OBJDIR)/core/event_bus/bus_attach.o $(OBJDIR)/core/event_bus/bus_host.o \
    $(OBJDIR)/core/event_bus/bus_route.o $(OBJDIR)/core/event_bus/bus_region.o $(OBJDIR)/core/event_bus/bus_region_host.o \
    $(OBJDIR)/core/event_bus/bus_ring.o $(OBJDIR)/core/event_bus/bus_arena.o \
    $(OBJDIR)/core/event_bus/bus_wire.o $(OBJDIR)/core/event_bus/bus_capture.o \
                           $(SERVER_KB_CLIENT_OBJS) $(AGENT_OBJS) $(SERVER_DATA_OBJS) \
                           $(SERVER_CMD_OBJS) $(CORE_OBJS) $(DB1_TEST_OBJS) $(PLATFORM_OBJS) \
                           $(MCP_GIT_OBJS) $(OBJDIR)/aimee_client.o $(OBJDIR)/http_uds_client.o

# No log_stub.o here: this target links the real server objects, and those
# already carry log.o. Having both put two definitions of aimee_log in one link
# and the target had not built AT ALL -- it is absent from UNIT_TEST_TARGETS, so
# nothing ran it and nothing reported it. The stub cannot replace log.o either;
# it defines aimee_log and nothing else, while the server needs log_init,
# log_set_level, audit_last_event and friends from the same object.
# The two core libraries are prerequisites for the same reason they are on the
# $(SERVER) rule: nothing else here builds them, so on a clean tree the link
# failed with "cannot find build/obj/libaimee-core-connection.a". It only ever
# appeared to work when a previous server build happened to leave them behind.
$(TESTPREFIX)/unit-test-server-management-listener-live: \
    $(OBJDIR)/tests/test_server_management_listener_live.o $(P5B3C_LIVE_SERVER_OBJS) \
    $(CORE_EVENT_BUS_LIB) $(CORE_CONNECTION_LIB)
	$(TESTLINK) -o $@ $^ $(L_SERVER)

# Common object sets for tests
TEST_CORE_OBJS = $(OBJDIR)/modules/db1/db.o $(OBJDIR)/modules/db1/db_schema.o $(OBJDIR)/modules/db1/maintenance.o $(DB2_TEST_BACKEND_OBJ) $(OBJDIR)/db2/db2_test_shim.o $(CONFIG_CLIENT_TEST_OBJS) $(OBJDIR)/client_config.o $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/util.o $(OBJDIR)/text.o $(OBJDIR)/interaction_event_names.o \
                 $(OBJDIR)/platform_random.o $(PLATFORM_BASIC_OBJS) \
                 $(OBJDIR)/aimee_home.o $(OBJDIR)/shared/kb_paths.o \
                 $(OBJDIR)/log.o $(OBJDIR)/shutdown_forensics.o $(OBJDIR)/cJSON.o $(OBJDIR)/util_url.o $(OBJDIR)/report_enrichment.o $(OBJDIR)/compact.o $(OBJDIR)/wire_fence.o $(OBJDIR)/slop_detect.o $(OBJDIR)/proxy_bootstrap.o \
                 $(OBJDIR)/json_fluent.o $(OBJDIR)/markdown.o $(OBJDIR)/modules/vault/runtime_secret.o
TEST_CORE_OBJS += $(OBJDIR)/http_content_encoding.o
# Extended set for tests that need workspace/worktree/guardrails functions (pulls in agents).
# The two DB1 domains the daemon stopped linking: their callers reach them over
# the bus now, so DB1_SRCS no longer names them. The implementations still exist
# and the module process still compiles them, and these tests want a real
# working-memory store rather than a bus round trip -- so they name them here.
#
# Deliberately defined in the TEST makefile. src/Makefile is what the catalog
# reads to decide whether a retired source is still linked into the daemon, and
# a test-only object list there reads exactly like a reverted cutover.
DB1_MIGRATED_OBJS = $(OBJDIR)/modules/db1/wm.o $(OBJDIR)/modules/db1/payload_rewrite_state.o \
                    $(OBJDIR)/modules/db1/cognify_jobs.o \
                    $(OBJDIR)/modules/db1/conv_context.o \
                    $(OBJDIR)/modules/db1/agent_log.o \
                    $(OBJDIR)/modules/db1/windows.o \
                    $(OBJDIR)/modules/db1/db1_trigger.o \
                    $(OBJDIR)/modules/db1/user_memory.o \
                    $(OBJDIR)/modules/db1/coord_jobs.o \
                    $(OBJDIR)/modules/db1/db1_cron_jobs.o \
                    $(OBJDIR)/modules/db1/delegations.o \
                    $(OBJDIR)/modules/db1/delegate_reservation.o \
                    $(OBJDIR)/modules/db1/delegation_checkpoint.o \
                    $(OBJDIR)/modules/db1/agent_jobs.o $(OBJDIR)/agent_job_release.o \
                    $(OBJDIR)/modules/db1/delegate_learning.o \
                    $(OBJDIR)/modules/db1/server_sessions.o \
                    $(OBJDIR)/modules/db1/primary_sessions.o $(OBJDIR)/primary_session_release.o \
                    $(OBJDIR)/modules/db1/session_paths.o \
                    $(OBJDIR)/modules/db1/webchat_claude_sessions.o \
                    $(OBJDIR)/modules/db1/webchat_live.o \
                    $(OBJDIR)/modules/db1/runtime_state.o \
                    $(OBJDIR)/modules/db1/project_clones.o \
                    $(OBJDIR)/modules/db1/local_operator.o \
                    $(OBJDIR)/modules/db1/env.o \
                    $(OBJDIR)/modules/db1/model_catalog.o $(OBJDIR)/model_catalog_release.o \
                    $(OBJDIR)/modules/db1/model_pricing.o \
                    $(OBJDIR)/modules/db1/working_profile_local.o \
                    $(OBJDIR)/modules/db1/tool_local_availability.o \
                    $(OBJDIR)/modules/db1/caches.o \
                    $(OBJDIR)/modules/db1/execution_trace.o \
                    $(OBJDIR)/modules/db1/wfe_binding.o \
                    $(OBJDIR)/modules/db1/pipelines.o \
                    $(OBJDIR)/modules/db1/roadmap_runtime.o \
                    $(OBJDIR)/modules/db1/checkpoints.o \
                    $(OBJDIR)/modules/db1/execution_plans.o $(OBJDIR)/execution_plan_list.o \
                    $(OBJDIR)/modules/db1/roundtable_pipeline.o \
                    $(OBJDIR)/wfe_store_lists.o \
                    $(OBJDIR)/modules/db1/remote_client_grant.o $(OBJDIR)/remote_client_claim.o \
                    $(OBJDIR)/modules/db1/web_page_cache.o \
                    $(OBJDIR)/modules/db1/fsnap.o \
                    $(OBJDIR)/modules/db1/decisions.o \
                    $(OBJDIR)/modules/db1/mcp_osv_cache.o \
                    $(OBJDIR)/modules/db1/token_audit.o \
                    $(OBJDIR)/modules/db1/cost_fold.o \
                    $(OBJDIR)/modules/db1/interaction_events.o \
                    $(OBJDIR)/modules/db1/guardrail_events.o \
                    $(OBJDIR)/modules/db1/eval.o \
                    $(OBJDIR)/modules/db1/diagnose.o \
                    $(OBJDIR)/modules/db1/clarify.o \
                    $(OBJDIR)/modules/db1/session_state.o $(OBJDIR)/delegate_exit_classify.o

TEST_WORKSPACE_OBJS_EXTRA = $(OBJDIR)/modules/workspace/workspace.o $(OBJDIR)/session_worktree_key.o $(OBJDIR)/modules/workspace/workspace_manifest.o $(OBJDIR)/modules/workspace/workspace_turn.o $(DB1_TEST_OBJS) $(DB1_MIGRATED_OBJS) $(OBJDIR)/user_memory_merge.o \
                            $(OBJDIR)/modules/routing/agent_config.o $(OBJDIR)/modules/vault/agent_credentials.o $(OBJDIR)/modules/routing/agent_registry.o $(OBJDIR)/modules/routing/routing.o $(OBJDIR)/tests/support/vault_service_stub.o $(OBJDIR)/tests/support/oauth_tokens_stub.o $(OBJDIR)/server/agent_adapter.o $(OBJDIR)/cmd_describe.o \
                             $(OBJDIR)/posix/cmd_describe.o \
                             $(OBJDIR)/server/agent_runtime.o $(OBJDIR)/modules/economizer/economizer_module_client.o $(OBJDIR)/server/agent_request_build.o $(OBJDIR)/tests/support/ir_shadow_stubs.o $(OBJDIR)/server/agent_logging.o $(OBJDIR)/server/request_context.o $(OBJDIR)/server/agent_context_budget.o $(OBJDIR)/prompts.o $(OBJDIR)/server/provider_cli_adapter.o $(OBJDIR)/server/cli_codex.o $(OBJDIR)/server/cli_claude.o $(OBJDIR)/server/cli_mistral.o $(OBJDIR)/server/cli_acp.o $(OBJDIR)/conversation_context.o $(OBJDIR)/server/provider_catalog.o $(OBJDIR)/server/agent_bridge.o $(OBJDIR)/server/anthropic_shape.o $(OBJDIR)/server/tool_call_args.o $(OBJDIR)/server/agent_request_shaping.o $(OBJDIR)/server/agent_policy.o $(OBJDIR)/server/model_sampling.o \
                             $(OBJDIR)/server/agent_tasks.o $(OBJDIR)/modules/benchmarks/agent_eval.o $(OBJDIR)/modules/benchmarks/agent_eval_memory_support.o $(OBJDIR)/modules/benchmarks/agent_eval_baseline.o \
                             $(OBJDIR)/server/agent_coord.o $(OBJDIR)/server/agent_tools.o $(OBJDIR)/modules/sandbox/sandbox_learned.o $(OBJDIR)/module_json_call.o $(OBJDIR)/posix/workspace_provider.o $(OBJDIR)/server/script_runner.o $(OBJDIR)/server/script_rpc.o $(OBJDIR)/toolset.o $(OBJDIR)/server/tool_args_coerce.o $(OBJDIR)/server/tool_schema_sanitizer.o \
                             $(OBJDIR)/modules/kb_client/kb_client.o $(OBJDIR)/modules/kb_client/kb_client_cache.o $(OBJDIR)/modules/kb_client/kb_client_index.o $(OBJDIR)/code_collect.o $(OBJDIR)/modules/kb_client/kb_client_index_parse.o $(OBJDIR)/modules/kb_client/kb_client_memory.o $(OBJDIR)/modules/kb_client/kb_client_memory_audit.o $(OBJDIR)/modules/kb_client/kb_client_memory_mutations.o $(OBJDIR)/modules/kb_client/kb_client_agent.o $(OBJDIR)/modules/kb_client/kb_client_dashboard.o $(OBJDIR)/modules/kb_client/kb_client_tasks.o $(OBJDIR)/modules/kb_client/kb_client_data.o $(OBJDIR)/tests/modules/kb_client/kb_client_tool_registry.o $(OBJDIR)/modules/kb_client/kb_client_prospective.o $(OBJDIR)/shared/kb_paths.o $(OBJDIR)/cli_client.o $(OBJDIR)/cli_v1_routes.o $(OBJDIR)/cli_v1_routes_b.o $(OBJDIR)/modules/workspace/workspace_client_diff.o $(OBJDIR)/cli_v1_routes_c.o $(OBJDIR)/cli_v1_routes_d.o $(OBJDIR)/aimee_client.o $(OBJDIR)/http_uds_client.o $(OBJDIR)/aimee_tls.o $(OBJDIR)/codex_auth.o $(OBJDIR)/cli_v1_routes_e.o $(OBJDIR)/cli_v1_routes_f.o \
                             $(OBJDIR)/modules/protocols/mcp/mcp_client.o $(OBJDIR)/modules/protocols/mcp/mcp_client_registry.o \
                             $(OBJDIR)/server/http_retry.o $(OBJDIR)/server/failover.o \
                             $(OBJDIR)/posix/cli_client.o $(OBJDIR)/cli_v1_routes.o $(OBJDIR)/cli_v1_routes_b.o $(OBJDIR)/modules/workspace/workspace_client_diff.o $(OBJDIR)/cli_v1_routes_c.o $(OBJDIR)/cli_v1_routes_d.o $(OBJDIR)/cli_v1_routes_e.o $(OBJDIR)/cli_v1_routes_f.o $(OBJDIR)/aimee_tls.o $(OBJDIR)/codex_auth.o $(OBJDIR)/posix/cli_main.o \
	                             $(OBJDIR)/modules/guardrails/guardrails.o $(OBJDIR)/modules/guardrails/guardrails_orchestrator.o $(OBJDIR)/modules/guardrails/guardrails_action_audit.o $(OBJDIR)/modules/audit/audit_action.o $(OBJDIR)/modules/audit/audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o $(OBJDIR)/modules/guardrails/guardrails_tdd.o $(OBJDIR)/modules/guardrails/guardrails_semantic.o $(OBJDIR)/modules/guardrails/guardrails_blast_radius.o $(OBJDIR)/modules/skills/skill.o $(OBJDIR)/session_state.o $(OBJDIR)/file_safety.o $(OBJDIR)/modules/git/git_verify.o $(OBJDIR)/modules/git/git_verify_state.o $(OBJDIR)/modules/git/git_verify_config.o $(OBJDIR)/modules/git/git_verify_jobs.o $(OBJDIR)/modules/git/git_verify_hook.o $(OBJDIR)/modules/git/git_verify_ops.o $(OBJDIR)/modules/git/git_verify_select.o $(OBJDIR)/modules/git/git_verify_step.o \
                             $(OBJDIR)/branch_ownership.o $(OBJDIR)/modules/db1/git_ownership.o \
                             $(OBJDIR)/dstr.o $(OBJDIR)/diff.o $(OBJDIR)/anchor_snapshot.o $(OBJDIR)/edit_anchored.o \
                             $(OBJDIR)/code_outline.o $(OBJDIR)/modules/tools/agent_tools_anchored.o \
                             $(OBJDIR)/posix/web_read.o \
                             $(OBJDIR)/server/web_search.o \
                                    $(OBJDIR)/server/web_search_fuse.o $(OBJDIR)/server/web_search_breaker.o $(OBJDIR)/rrf.o \
                             $(OBJDIR)/server/token_tracker.o \
                             $(OBJDIR)/server/process_mgr.o \
                             $(OBJDIR)/modules/lsp/lsp_manager.o $(OBJDIR)/modules/lsp/lsp_client.o \
                             $(OBJDIR)/server/model_provider.o $(OBJDIR)/server/openai_profile.o \
                             $(OBJDIR)/server/anthropic_profile.o                             $(OBJDIR)/server/openrouter_profile.o $(OBJDIR)/server/ollama_profile.o \
                             $(OBJDIR)/server/llama_native_profile.o $(OBJDIR)/server/mistral_profile.o \
                             $(OBJDIR)/server/minimax_profile.o \
                             $(OBJDIR)/model_registry.o $(OBJDIR)/models_dev.o $(OBJDIR)/models_dev_cache.o \
                             $(OBJDIR)/tests/support/delegate_child_env_export_stub.o \
                             $(OBJDIR)/tests/support/git_cred_inject_stub.o \
                             $(OBJDIR)/modules/gateway/gateway_delegate.o $(OBJDIR)/modules/gateway/gateway_pipeline.o $(OBJDIR)/modules/gateway/gateway_policy.o \
                             $(OBJDIR)/modules/ir/aimee_ir.o \
                             $(PLATFORM_AGENT_OBJS)

# This bundle carries guardrails_action_audit.o and guardrails_semantic.o, which
# reference obs_bus_emit / obs_bus_emit_guardrail — so every test linking it
# also needs the bus objects. Test binaries list them individually for focused
# fixtures; shipping daemons consume only libaimee-core-event-bus.a. guardrail_events.o /
# aimee_home.o / log.o are already in TEST_DATA_OBJS.
BUS_TEST_OBJS = $(OBS_BUS_LINK_OBJS) \
                $(OBJDIR)/core/event_bus/bus_client.o $(OBJDIR)/core/event_bus/bus_attach.o $(OBJDIR)/core/event_bus/bus_host.o \
                $(OBJDIR)/core/event_bus/bus_route.o $(OBJDIR)/core/event_bus/bus_region.o $(OBJDIR)/core/event_bus/bus_region_host.o \
                $(OBJDIR)/core/event_bus/bus_ring.o $(OBJDIR)/core/event_bus/bus_arena.o \
                $(OBJDIR)/core/event_bus/bus_wire.o $(OBJDIR)/core/event_bus/bus_capture.o
TEST_WORKSPACE_OBJS_EXTRA += $(BUS_TEST_OBJS)

TEST_MCP_CLIENT_OBJS = $(OBJDIR)/modules/protocols/mcp/mcp_client.o \
                       $(OBJDIR)/sse_parser.o \
                       $(OBJDIR)/tests/support/mock_agent_http.o \
                       $(OBJDIR)/cJSON.o \
                       $(PLATFORM_BASIC_OBJS)

TEST_FACT_MUTATION_MIN_OBJS = $(OBJDIR)/db2/fact_mutation.o $(OBJDIR)/db2/kb_audit_worm.o \
                              $(OBJDIR)/modules/audit/audit_worm_chain.o \
                              $(OBJDIR)/modules/workflows/wfe_canonical.o \
                              $(OBJDIR)/aimee_sha256.o

TEST_DATA_OBJS = $(TEST_CORE_OBJS) $(TEST_FACT_MUTATION_MIN_OBJS) $(OBJDIR)/rel_types.o $(OBJDIR)/agent_job_release.o $(OBJDIR)/delegate_exit_classify.o $(OBJDIR)/diagnose_render.o $(OBJDIR)/clarify_render.o $(OBJDIR)/interaction_event_names.o $(OBJDIR)/modules/memory/memory_fact_gate.o $(OBJDIR)/modules/memory/memory_extract_patterns.o $(OBJDIR)/db2/rel_types_store.o $(OBJDIR)/db2/entity_registry.o $(OBJDIR)/db2/fact_lifecycle.o $(OBJDIR)/db2/ontology_evolution.o $(OBJDIR)/db2/fact_ingest.o $(OBJDIR)/db2/fact_recall.o $(OBJDIR)/modules/memory/memory_pii_gate.o $(OBJDIR)/modules/learning/learning_router.o $(OBJDIR)/modules/learning/learning_regret.o $(OBJDIR)/modules/learning/learning_implicit.o $(OBJDIR)/dogfood.o $(OBJDIR)/working_profile.o $(OBJDIR)/integrity_gate.o \
                 $(OBJDIR)/modules/memory/memory_core.o $(OBJDIR)/modules/memory/memory_core_crud.o $(OBJDIR)/modules/memory/memory_core_helpers.o $(OBJDIR)/modules/memory/memory_core_helpers_b.o $(OBJDIR)/modules/memory/memory_core_search.o $(OBJDIR)/modules/memory/memory_core_search_b.o $(OBJDIR)/modules/memory/memory_core_search_c.o $(OBJDIR)/modules/memory/memory_core_scope_embed.o $(OBJDIR)/modules/memory/memory_core_tiers.o $(OBJDIR)/db2/kb_payload.o $(OBJDIR)/db2/memory_lifecycle.o $(OBJDIR)/db2/memory_payload.o $(OBJDIR)/db2/memory_promotion.o $(OBJDIR)/db2/memory_query.o $(OBJDIR)/db2/memory_query_bookkeeping.o $(OBJDIR)/db2/memory_entity_graph.o $(OBJDIR)/db2/memory_score_fields.o $(OBJDIR)/db2/memory_scope_query.o $(OBJDIR)/db2/memory_scenes.o $(OBJDIR)/db2/memory_briefing.o $(OBJDIR)/db2/memory_health.o $(OBJDIR)/db2/memory_row_mapper_pg.o $(OBJDIR)/db2/memory_relations.o $(OBJDIR)/db2/memory_conflicts.o $(OBJDIR)/db2/vector_index_ops.o $(OBJDIR)/db2/code_index_ops.o $(OBJDIR)/tests/support/memory_embed_stub.o $(OBJDIR)/posix/memory.o \
                 $(OBJDIR)/modules/memory/memory_logic.o $(OBJDIR)/modules/memory/memory_effective.o $(OBJDIR)/modules/memory/memory_health.o $(OBJDIR)/db2/fact_lifecycle.o $(OBJDIR)/modules/memory/memory_conflict.o $(OBJDIR)/modules/memory/memory_context.o $(OBJDIR)/modules/memory/memory_assemble.o $(OBJDIR)/modules/memory/memory_advanced.o $(OBJDIR)/modules/memory/memory_prospective.o $(OBJDIR)/modules/memory/memory_lifecycle.o $(OBJDIR)/modules/memory/memory_directives.o $(OBJDIR)/modules/memory/memory_maintenance.o $(OBJDIR)/modules/memory/memory_graph.o $(OBJDIR)/modules/memory/memory_graph_fusion.o $(OBJDIR)/modules/memory/memory_scan.o $(OBJDIR)/modules/memory/memory_improve.o $(OBJDIR)/modules/memory/memory_episodes.o \
                 $(OBJDIR)/workflow_learn.o \
                 $(OBJDIR)/index.o $(OBJDIR)/cochange.o $(OBJDIR)/modules/css/css_analyze.o $(OBJDIR)/db2/css_graph.o $(OBJDIR)/extractors.o $(OBJDIR)/extractors_extra.o $(OBJDIR)/extractors_new_langs.o $(OBJDIR)/code_treesitter.o \
                 $(OBJDIR)/tasks.o $(OBJDIR)/render.o \
                 $(DB1_TEST_OBJS) $(DB1_MIGRATED_OBJS) $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o $(OBJDIR)/db2/agent_hints.o $(OBJDIR)/db2/agent_outcomes.o $(OBJDIR)/db2/anti_patterns.o $(OBJDIR)/modules/db1/approach_failures.o $(OBJDIR)/approach_store.o $(OBJDIR)/db2/collab_rules.o $(OBJDIR)/db2/curiosity.o $(OBJDIR)/db2/decision_log.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/db2/entity_edges.o $(OBJDIR)/db2/entity_nodes.o $(OBJDIR)/db2/code_projection.o $(OBJDIR)/db2/shadow_delta.o $(OBJDIR)/modules/kb_client/kb_client_code_embed.o $(OBJDIR)/db2/entity_profiles.o $(OBJDIR)/db2/epistemic_directives.o $(OBJDIR)/db2/failed_queries.o $(OBJDIR)/db2/feedback.o $(OBJDIR)/db2/memory_export.o $(OBJDIR)/db2/notes.o $(OBJDIR)/db2/prospective_memories.o $(OBJDIR)/db2/rules.o $(OBJDIR)/db2/tasks.o $(OBJDIR)/db2/tool_registry.o $(OBJDIR)/db2/trace_mining.o $(OBJDIR)/db2/kind_lifecycle.o $(OBJDIR)/db2/kb_runtime_state.o $(OBJDIR)/db2/kb_service_backend.o $(OBJDIR)/db2/kb_service_backend_ingest.o $(OBJDIR)/db2/memory_scenes.o $(OBJDIR)/db2/learning.o $(OBJDIR)/db2/code_index.o $(OBJDIR)/db2/sketch.o $(OBJDIR)/db2/pgvec_transport.o $(OBJDIR)/db2/memory_vectors.o $(OBJDIR)/db2/kb_vectors.o $(OBJDIR)/db2/vector_status.o $(OBJDIR)/db2/pgvec_verify.o $(OBJDIR)/db2/pgvec_kb_service.o $(OBJDIR)/kb/kb.o $(OBJDIR)/kb/kb_fusion.o $(OBJDIR)/kb/kb_neardup.o $(OBJDIR)/kb/kb_conventions.o $(OBJDIR)/sketch.o \
                 $(OBJDIR)/modules/workspace/workspace.o $(OBJDIR)/session_worktree_key.o $(OBJDIR)/modules/workspace/workspace_manifest.o \
                 $(OBJDIR)/modules/learning/learning_evidence.o $(OBJDIR)/db2/learning_synth_ops.o \
                 $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o $(OBJDIR)/db2/demotion.o $(OBJDIR)/db2/calibration.o \
                 $(OBJDIR)/db2/feature_rows.o $(OBJDIR)/kb/kb_features.o $(OBJDIR)/kb/kb_ranker.o $(OBJDIR)/kb/kb_detect.o \
                 $(OBJDIR)/kb/kb_reasoning.o \
                 $(OBJDIR)/db2/bandit.o $(OBJDIR)/kb/kb_bandit.o $(OBJDIR)/kb/kb_bandit_registry.o \
                 $(OBJDIR)/kb/kb_mdl.o \
                 $(OBJDIR)/server/computer_use.o

# Same as TEST_DATA_OBJS but with the agent_http_* mock appended. Used by
# targets that exercise the embedding/vector path without pulling in the
# real posix/agent_bridge.o (which lives in TEST_WORKSPACE_OBJS_EXTRA).
# Combined targets ($(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)) must
# NOT use this — the real agent_bridge provides agent_http_* there, and
# linking both would produce duplicate-symbol errors.
TEST_DATA_OBJS_MOCK = $(TEST_DATA_OBJS) $(OBJDIR)/tests/support/mock_agent_http.o

TEST_TARGETS := $(TESTPREFIX)/unit-test-util $(TESTPREFIX)/unit-test-db $(TESTPREFIX)/unit-test-harness-memory $(TESTPREFIX)/unit-test-memory-redirect $(TESTPREFIX)/unit-test-harness-memory-scope $(TESTPREFIX)/unit-test-harness-memory-spill $(TESTPREFIX)/unit-test-harness-memory-audit $(TESTPREFIX)/unit-test-roundtable-brief $(TESTPREFIX)/unit-test-db2 $(TESTPREFIX)/unit-test-pg-prepare-classification $(TESTPREFIX)/unit-test-schema-subst $(TESTPREFIX)/unit-test-code-index-ops $(TESTPREFIX)/unit-test-code-project-lifecycle $(TESTPREFIX)/unit-test-curator-version $(TESTPREFIX)/unit-test-curator-invalidate $(TESTPREFIX)/unit-test-curator-notify $(TESTPREFIX)/unit-test-curator-queue $(TESTPREFIX)/unit-test-curator-pipeline-sched $(TESTPREFIX)/unit-test-curator-custom-stages $(TESTPREFIX)/unit-test-pgvec $(TESTPREFIX)/unit-test-pgvec-neardup $(TESTPREFIX)/unit-test-rules $(TESTPREFIX)/unit-test-delegate-sandbox-image \
               $(TESTPREFIX)/unit-test-guardrails $(TESTPREFIX)/unit-test-session-degraded-notice $(TESTPREFIX)/unit-test-kb-http-json $(TESTPREFIX)/unit-test-memory $(TESTPREFIX)/unit-test-tasks \
               $(TESTPREFIX)/unit-test-cmd-hooks-scope \
               $(TESTPREFIX)/unit-test-agent $(TESTPREFIX)/unit-test-agent-repair $(TESTPREFIX)/unit-test-agent-apikey $(TESTPREFIX)/unit-test-script-runner $(TESTPREFIX)/unit-test-provider-cli-adapter $(TESTPREFIX)/unit-test-cli-acp $(TESTPREFIX)/unit-test-acp-server $(TESTPREFIX)/unit-test-toolset-thread-scope $(TESTPREFIX)/unit-test-workspace-provider-container $(TESTPREFIX)/unit-test-mcp-native-surface $(TESTPREFIX)/unit-test-mcp-native-dispatch $(TESTPREFIX)/unit-test-extractors \
               $(TESTPREFIX)/unit-test-text $(TESTPREFIX)/unit-test-roundtable-preset $(TESTPREFIX)/unit-test-roundtable-seat-resolve $(TESTPREFIX)/unit-test-audit-worm $(TESTPREFIX)/unit-test-audit-worm-chain $(TESTPREFIX)/unit-test-kb-audit-worm $(TESTPREFIX)/unit-test-msg-session-key $(TESTPREFIX)/unit-test-gateway-mutate $(TESTPREFIX)/unit-test-gateway-mutate-wire $(TESTPREFIX)/unit-test-tool-output-cap $(TESTPREFIX)/unit-test-ingress-preinject $(TESTPREFIX)/unit-test-code-span $(TESTPREFIX)/unit-test-code-match $(TESTPREFIX)/unit-test-gw-stage-memory $(TESTPREFIX)/unit-test-attention-guard $(TESTPREFIX)/unit-test-client-session-worktree $(TESTPREFIX)/unit-test-session-worktree-key $(TESTPREFIX)/unit-test-codex-auth $(TESTPREFIX)/unit-test-code-audit $(TESTPREFIX)/unit-test-code-audit-graph $(TESTPREFIX)/unit-test-cochange $(TESTPREFIX)/unit-test-db2-code-audit $(TESTPREFIX)/unit-test-cron-runtime $(TESTPREFIX)/unit-test-feedback \
               $(TESTPREFIX)/unit-test-render $(TESTPREFIX)/unit-test-index $(TESTPREFIX)/unit-test-manuscript $(TESTPREFIX)/unit-test-persona $(TESTPREFIX)/unit-test-server-http $(TESTPREFIX)/unit-test-openai-shape $(TESTPREFIX)/unit-test-openai-chat-policed $(TESTPREFIX)/unit-test-openai-responses-store \
               $(TESTPREFIX)/unit-test-feedback-shadow $(TESTPREFIX)/unit-test-graph-fusion $(TESTPREFIX)/unit-test-code-vectors $(TESTPREFIX)/unit-test-graph-scoring $(TESTPREFIX)/unit-test-code-projection $(TESTPREFIX)/unit-test-entity-nodes $(TESTPREFIX)/unit-test-memory-advanced $(TESTPREFIX)/unit-test-memory-health \
               $(TESTPREFIX)/unit-test-memory-ranker-boundary \
               $(TESTPREFIX)/unit-test-memory-lanes \
               $(TESTPREFIX)/unit-test-workspace \
               $(TESTPREFIX)/unit-test-cross-repo-deps \
               $(TESTPREFIX)/unit-test-cross-repo-stats \
               $(TESTPREFIX)/unit-test-cross-repo-deps-orch \
               $(TESTPREFIX)/unit-test-cross-repo-acceptance \
               $(TESTPREFIX)/unit-test-cross-repo-identity \
               $(TESTPREFIX)/unit-test-cross-repo-route \
               $(TESTPREFIX)/unit-test-cross-repo-build \
               $(TESTPREFIX)/unit-test-cross-repo-review \
               $(TESTPREFIX)/unit-test-primary-session-adapter \
               $(TESTPREFIX)/unit-test-webchat-claude-sessions \
               $(TESTPREFIX)/unit-test-turn-registry \
               $(TESTPREFIX)/unit-test-session-search-tool \
               $(TESTPREFIX)/unit-test-working-memory $(TESTPREFIX)/unit-test-working-memory-mock $(TESTPREFIX)/unit-test-local-resolution $(TESTPREFIX)/unit-test-cognify-jobs $(TESTPREFIX)/unit-test-extractors-extra \
               $(TESTPREFIX)/unit-test-evidence-replay $(TESTPREFIX)/unit-test-git-pr-ci-grade $(TESTPREFIX)/unit-test-roundtable-verify $(TESTPREFIX)/unit-test-roundtable-chair $(TESTPREFIX)/unit-test-sweep-logic $(TESTPREFIX)/unit-test-sweep-scope $(TESTPREFIX)/unit-test-sweep-parse \
               $(TESTPREFIX)/unit-test-css-analyze $(TESTPREFIX)/unit-test-typed-facts $(TESTPREFIX)/unit-test-css-graph $(TESTPREFIX)/unit-test-css-insights $(TESTPREFIX)/unit-test-css-oracle $(TESTPREFIX)/unit-test-css-render-oracle $(TESTPREFIX)/unit-test-css-migration $(TESTPREFIX)/unit-test-css-render $(TESTPREFIX)/unit-test-css-render-cmd \
               $(TESTPREFIX)/unit-test-compute-pool $(TESTPREFIX)/unit-test-db2-pool $(TESTPREFIX)/unit-test-db2-conn-bounds $(TESTPREFIX)/unit-test-db2-conn-open $(TESTPREFIX)/unit-test-cli-launch \
               $(TESTPREFIX)/unit-test-server-session-pools \
               $(TESTPREFIX)/unit-test-presence \
               $(TESTPREFIX)/unit-test-cli-provider \
               $(TESTPREFIX)/unit-test-context-assembly $(TESTPREFIX)/unit-test-workspace-memory \
               $(TESTPREFIX)/unit-test-dashboard \
               $(TESTPREFIX)/unit-test-log $(TESTPREFIX)/unit-test-server-error-kind $(TESTPREFIX)/unit-test-server-dispatch \
               $(TESTPREFIX)/unit-test-aimee-home \
               $(TESTPREFIX)/unit-test-workflow \
               $(TESTPREFIX)/unit-test-wfe-engine \
               $(TESTPREFIX)/unit-test-wfe-blocks \
               $(TESTPREFIX)/unit-test-wfe-approval \
               $(TESTPREFIX)/unit-test-wfe-roundtable \
               $(TESTPREFIX)/unit-test-wfe-sliced-build \
               $(TESTPREFIX)/unit-test-wfe-foreach \
               $(TESTPREFIX)/unit-test-wfe-foreach-spawn \
               $(TESTPREFIX)/unit-test-wfe-panel-roundtable \
               $(TESTPREFIX)/unit-test-wfe-replay-worktree \
               $(TESTPREFIX)/unit-test-wfe-autonomy \
               $(TESTPREFIX)/unit-test-wfe-custom \
               $(TESTPREFIX)/unit-test-wfe-safety \
               $(TESTPREFIX)/unit-test-wfe-failure-taxonomy \
               $(TESTPREFIX)/unit-test-wfe-delegate-seam \
               $(TESTPREFIX)/unit-test-wfe-scheduler \
               $(TESTPREFIX)/unit-test-wfe-gate-reject \
               $(TESTPREFIX)/unit-test-wfe-gate-apply \
               $(TESTPREFIX)/unit-test-wfe-submitter \
               $(TESTPREFIX)/unit-test-wfe-manager-blocks \
               $(TESTPREFIX)/unit-test-wfe-manager-artifacts \
               $(TESTPREFIX)/unit-test-wfe-externalization \
               $(TESTPREFIX)/unit-test-tool-egress \
               $(TESTPREFIX)/unit-test-cli-claude-allowlist \
               $(TESTPREFIX)/unit-test-web-read-spans \
               $(TESTPREFIX)/unit-test-web-egress \
               $(TESTPREFIX)/unit-test-web-page-cache \
               $(TESTPREFIX)/unit-test-web-search-fusion \
               $(TESTPREFIX)/unit-test-web-search-fuse \
               $(TESTPREFIX)/unit-test-web-search-breaker \
               $(TESTPREFIX)/unit-test-dependency-breaker \
               $(TESTPREFIX)/unit-test-kb-rrf-purity \
               $(TESTPREFIX)/unit-test-wfe-deliver \
               $(TESTPREFIX)/unit-test-wfe-manager-flow \
               $(TESTPREFIX)/unit-test-wfe-router \
               $(TESTPREFIX)/unit-test-wfe-router-catalog \
               $(TESTPREFIX)/unit-test-wfe-autonomous-route \
               $(TESTPREFIX)/unit-test-wfe-native-gate \
               $(TESTPREFIX)/unit-test-wfe-enforce \
               $(TESTPREFIX)/unit-test-wfe-advance \
               $(TESTPREFIX)/unit-test-wfe-advance-exec \
               $(TESTPREFIX)/unit-test-wfe-block-resolve \
               $(TESTPREFIX)/unit-test-wfe-bind-ingress \
               $(TESTPREFIX)/unit-test-primary-cli-ingestor \
               $(TESTPREFIX)/unit-test-wfe-binding \
               $(TESTPREFIX)/unit-test-aimee-ir \
               $(TESTPREFIX)/unit-test-ir-legacy-parity \
               $(TESTPREFIX)/unit-test-agent-request-build \
               $(TESTPREFIX)/unit-test-ir-crossproto-egress \
               $(TESTPREFIX)/unit-test-aimee-ir-rescue \
               $(TESTPREFIX)/unit-test-agent-ir-parse \
               $(TESTPREFIX)/unit-test-responses-parity \
               $(TESTPREFIX)/unit-test-ir-shadow-response \
               $(TESTPREFIX)/unit-test-shadow-mirror \
               $(TESTPREFIX)/unit-test-aimee-ir-metrics \
               $(TESTPREFIX)/unit-test-aimee-frontend \
               $(TESTPREFIX)/unit-test-aimee-backend \
               $(TESTPREFIX)/unit-test-aimee-backend-bedrock \
               $(TESTPREFIX)/unit-test-kb-bedrock-dispatch \
               $(TESTPREFIX)/unit-test-kb-http-client \
               $(TESTPREFIX)/unit-test-vault-kms \
               $(TESTPREFIX)/unit-test-aimee-ir-shadow \
               $(TESTPREFIX)/unit-test-aimee-ir-serve \
               $(TESTPREFIX)/unit-test-aimee-ir-session \
               $(TESTPREFIX)/unit-test-aimee-ir-stream \
               $(TESTPREFIX)/unit-test-aimee-converse-stream \
               $(TESTPREFIX)/unit-test-workflow-gate-caps \
               $(TESTPREFIX)/unit-test-wfe-webapi \
               $(TESTPREFIX)/unit-test-cli-profile \
               $(TESTPREFIX)/unit-test-cmd-profile \
               $(TESTPREFIX)/unit-test-index-investigate-status \
               $(TESTPREFIX)/unit-test-kb-client-index \
               $(TESTPREFIX)/unit-test-kb-client-index-remote \
               $(TESTPREFIX)/unit-test-kb-client-docs \
               $(TESTPREFIX)/unit-test-kb-client-search \
               $(TESTPREFIX)/unit-test-kb-client-memory \
               $(TESTPREFIX)/unit-test-kb-graph \
               $(TESTPREFIX)/unit-test-kb-rrf \
               $(TESTPREFIX)/unit-test-kb-graph-analytics $(TESTPREFIX)/unit-test-lessons-cite-tracker $(TESTPREFIX)/unit-test-lessons-reflect $(TESTPREFIX)/unit-test-lessons-actuate $(TESTPREFIX)/unit-test-lessons-session-capture $(TESTPREFIX)/unit-test-kb-doc-hash \
               $(TESTPREFIX)/unit-test-prompt-sanitizer \
               $(TESTPREFIX)/unit-test-bus-wire \
               $(TESTPREFIX)/unit-test-module-protocol \
               $(TESTPREFIX)/unit-test-bus-ring \
               $(TESTPREFIX)/unit-test-bus-region \
               $(TESTPREFIX)/unit-test-bus-arena \
               $(TESTPREFIX)/unit-test-bus-host \
               $(TESTPREFIX)/unit-test-bus-route \
               $(TESTPREFIX)/unit-test-bus-flow \
               $(TESTPREFIX)/unit-test-bus-client \
               $(TESTPREFIX)/unit-test-bus-endpoint \
               $(TESTPREFIX)/unit-test-bus-runtime \
               $(TESTPREFIX)/unit-test-module-runtime \
               $(TESTPREFIX)/unit-test-module-json-call \
               $(TESTPREFIX)/unit-test-economizer-module-client \
               $(TESTPREFIX)/unit-test-sandbox-pkg-proxy-adapter \
               $(TESTPREFIX)/unit-test-sandbox-learned-observe \
               $(TESTPREFIX)/unit-test-routing-module \
               $(TESTPREFIX)/unit-test-bus-capture \
               $(TESTPREFIX)/unit-test-guardrails-blast-radius \
               $(TESTPREFIX)/unit-test-code-collect \
               $(TESTPREFIX)/unit-test-server-conn-accept \
               $(TESTPREFIX)/unit-test-server-compute \
               $(TESTPREFIX)/unit-test-server-memory-benchmark \
               $(TESTPREFIX)/unit-test-server-jobs-aux \
               $(TESTPREFIX)/unit-test-agent-admission \
               $(TESTPREFIX)/unit-test-provider-catalog \
               $(TESTPREFIX)/unit-test-trace-analysis \
               $(TESTPREFIX)/unit-test-cmd-branch \
               $(TESTPREFIX)/unit-test-cmd-core \
               $(TESTPREFIX)/unit-test-client-integrations $(TESTPREFIX)/unit-test-mcp-git \
               $(TESTPREFIX)/unit-test-git-verify-select \
               $(TESTPREFIX)/unit-test-git-verify-contract \
               $(TESTPREFIX)/unit-test-cli-mcp-serve \
               $(TESTPREFIX)/unit-test-cli-index-bootstrap \
               $(TESTPREFIX)/unit-test-server-mcp-roundtable \
               $(TESTPREFIX)/unit-test-cli-v1-delegate \
               $(TESTPREFIX)/unit-test-cli-server-compat \
               $(TESTPREFIX)/unit-test-platform-process \
               $(TESTPREFIX)/unit-test-shutdown-forensics \
               $(TESTPREFIX)/unit-test-dstr \
               $(TESTPREFIX)/unit-test-aimee-client \
               $(TESTPREFIX)/unit-test-cli-remote \
               $(TESTPREFIX)/unit-test-util-url \
               $(TESTPREFIX)/unit-test-delivery-target \
               $(TESTPREFIX)/unit-test-gateway \
               $(TESTPREFIX)/unit-test-gateway-telegram \
               $(TESTPREFIX)/unit-test-gateway-ntfy-webhook \
               $(TESTPREFIX)/unit-test-gateway-stt-pairing \
               $(TESTPREFIX)/unit-test-mcp-gateway-tools \
               $(TESTPREFIX)/unit-test-report-enrichment \
               $(TESTPREFIX)/unit-test-hardware-probe \
               $(TESTPREFIX)/unit-test-curator-profile \
               $(TESTPREFIX)/unit-test-kb-client-cache \
               $(TESTPREFIX)/unit-test-openai-runs-store \
               $(TESTPREFIX)/unit-test-cli-argspec \
               $(TESTPREFIX)/unit-test-cli-http-transport \
               $(TESTPREFIX)/unit-test-http-retry \
               $(TESTPREFIX)/unit-test-cmd-doctor \
               $(TESTPREFIX)/unit-test-diff \
               $(TESTPREFIX)/unit-test-anchor-snapshot \
               $(TESTPREFIX)/unit-test-edit-anchored \
               $(TESTPREFIX)/unit-test-hashline-gate \
               $(TESTPREFIX)/unit-test-workspace-provider \
               $(TESTPREFIX)/unit-test-workspace-handle \
               $(TESTPREFIX)/unit-test-forge-credentials \
               $(TESTPREFIX)/unit-test-forge-app-token \
               $(TESTPREFIX)/unit-test-workspace-mirror \
               $(TESTPREFIX)/unit-test-workspace-client-base \
               $(TESTPREFIX)/unit-test-workspace-provider-detached \
               $(TESTPREFIX)/unit-test-cli-kb-smoke \
               $(TESTPREFIX)/unit-test-kb-sidecar-identity \
               $(TESTPREFIX)/unit-test-synthesis-mtls-client \
               $(TESTPREFIX)/unit-test-workspace-scope \
               $(TESTPREFIX)/unit-test-workspace-migration \
               $(TESTPREFIX)/unit-test-webuser-runtime \
               $(TESTPREFIX)/unit-test-workspace-turn \
               $(TESTPREFIX)/unit-test-notes \
               $(TESTPREFIX)/unit-test-cmd-cancel \
               $(TESTPREFIX)/unit-test-cmd-delegate \
               $(TESTPREFIX)/unit-test-delegate-plan \
               $(TESTPREFIX)/unit-test-delegate-role \
               $(TESTPREFIX)/unit-test-delegate-permissions \
               $(TESTPREFIX)/unit-test-sse-parser \
               $(TESTPREFIX)/unit-test-anthropic-ingress \
               $(TESTPREFIX)/unit-test-anthropic-http \
               $(TESTPREFIX)/unit-test-anthropic-http-p2c \
               $(TESTPREFIX)/unit-test-anthropic-http-streaming-p2c \
               $(TESTPREFIX)/unit-test-gateway-policy \
               $(TESTPREFIX)/unit-test-gateway-pipeline \
               $(TESTPREFIX)/unit-test-gw-stage-registry \
               $(TESTPREFIX)/unit-test-gw-response-registry $(TESTPREFIX)/unit-test-response-completion-stage \
               $(TESTPREFIX)/unit-test-response-governance-stage \
               $(TESTPREFIX)/unit-test-gw-orchestration-seam \
               $(TESTPREFIX)/unit-test-gw-orch-delegates \
               $(TESTPREFIX)/unit-test-gw-orch-workflows \
               $(TESTPREFIX)/unit-test-gateway-p4-delegate \
               $(TESTPREFIX)/unit-test-hud \
               $(TESTPREFIX)/unit-test-coord-jobs \
               $(TESTPREFIX)/unit-test-deploy-apply \
               $(TESTPREFIX)/unit-test-cli-v1-subcommands \
               $(TESTPREFIX)/unit-test-cli-v1-poll-deadline \
               $(TESTPREFIX)/unit-test-plan-waves \
               $(TESTPREFIX)/unit-test-history \
               $(TESTPREFIX)/unit-test-events \
               $(TESTPREFIX)/unit-test-file-ref \
               $(TESTPREFIX)/unit-test-role-templates \
               $(TESTPREFIX)/unit-test-skill \
               $(TESTPREFIX)/unit-test-web-search \
               $(TESTPREFIX)/unit-test-tdd \
               $(TESTPREFIX)/unit-test-compact \
               $(TESTPREFIX)/unit-test-wire-fence \
               $(TESTPREFIX)/unit-test-economizer-live-surface \
               $(TESTPREFIX)/unit-test-token-audit \
               $(TESTPREFIX)/unit-test-token-audit-load \
               $(TESTPREFIX)/unit-test-windows \
               $(TESTPREFIX)/unit-test-token-tracker \
               $(TESTPREFIX)/unit-test-model-pricing \
               $(TESTPREFIX)/unit-test-provider-client \
               $(TESTPREFIX)/unit-test-kb-curator-provider \
               $(TESTPREFIX)/unit-test-kb-curator-llm \
               $(TESTPREFIX)/unit-test-reasoning-cap \
               $(TESTPREFIX)/unit-test-request-context \
               $(TESTPREFIX)/unit-test-response-dedup \
               $(TESTPREFIX)/unit-test-anthropic-shape \
               $(TESTPREFIX)/unit-test-tool-prompts \
               $(TESTPREFIX)/unit-test-delegate-token-budget \
               $(TESTPREFIX)/unit-test-delegate-context-shed \
               $(TESTPREFIX)/unit-test-agent-error-retryable \
               $(TESTPREFIX)/unit-test-delegate-ephemeral-ws \
               $(TESTPREFIX)/unit-test-delegate-handoff \
               $(TESTPREFIX)/unit-test-delegate-economics \
               $(TESTPREFIX)/unit-test-delegate-patch-coordinator \
               $(TESTPREFIX)/unit-test-delegate-ensemble \
               $(TESTPREFIX)/unit-test-rel-types \
               $(TESTPREFIX)/unit-test-memory-facts-grounding \
               $(TESTPREFIX)/unit-test-memory-fact-gate \
               $(TESTPREFIX)/unit-test-memory-embed-dim-guard \
               $(TESTPREFIX)/unit-test-memory-embed-http-auth \
               $(TESTPREFIX)/unit-test-memory-embed-batch \
               $(TESTPREFIX)/unit-test-rel-types-store \
               $(TESTPREFIX)/unit-test-entity-registry \
               $(TESTPREFIX)/unit-test-fact-lifecycle \
               $(TESTPREFIX)/unit-test-mcp-memory-gate \
               $(TESTPREFIX)/unit-test-embedding-dim \
               $(TESTPREFIX)/unit-test-embedder-probe-register \
               $(TESTPREFIX)/unit-test-ontology-evolution \
               $(TESTPREFIX)/unit-test-extract-patterns \
               $(TESTPREFIX)/unit-test-fact-ingest $(TESTPREFIX)/unit-test-decision-log \
               $(TESTPREFIX)/unit-test-fact-recall \
               $(TESTPREFIX)/unit-test-pii-gate \
               $(TESTPREFIX)/unit-test-sandbox \
               $(TESTPREFIX)/unit-test-slop-detect \
               $(TESTPREFIX)/unit-test-vault-principal \
               $(TESTPREFIX)/unit-test-vault-crypto \
               $(TESTPREFIX)/unit-test-vault-kek-check \
               $(TESTPREFIX)/unit-test-vault-reseal-receipt \
               $(TESTPREFIX)/unit-test-vault-witness-record \
               $(TESTPREFIX)/unit-test-vault-witness-merkle \
               $(TESTPREFIX)/unit-test-vault-witness-checkpoint \
               $(TESTPREFIX)/unit-test-vault-witness-export \
               $(TESTPREFIX)/unit-test-vault-witness-verify \
               $(TESTPREFIX)/unit-test-vault-witness-signer \
               $(TESTPREFIX)/unit-test-vault-witness-proof \
               $(TESTPREFIX)/unit-test-vault-witness-offline \
               $(TESTPREFIX)/unit-test-witness-tamper-scenarios \
               $(TESTPREFIX)/unit-test-witness-offline-fuzz \
               $(TESTPREFIX)/unit-test-witness-gate-race \
               $(TESTPREFIX)/unit-test-vault-mutation-budget \
               $(TESTPREFIX)/unit-test-vault-reseal-orchestrator \
               $(TESTPREFIX)/unit-test-org-vault-rewrap \
               $(TESTPREFIX)/unit-test-vault-kek-cache \
               $(TESTPREFIX)/unit-test-vault-store \
               $(TESTPREFIX)/unit-test-vault-seam \
               $(TESTPREFIX)/unit-test-vault-local-status \
               $(TESTPREFIX)/unit-test-vault-operator-status-runtime \
               $(TESTPREFIX)/unit-test-kb-vault-operator-status \
               $(TESTPREFIX)/unit-test-kb-vault-operator-mutation \
               $(TESTPREFIX)/unit-test-kb-vault-operator-choreography \
               $(TESTPREFIX)/unit-test-kb-vault-operator-runtime \
               $(TESTPREFIX)/unit-test-kb-vault-protected-secret \
               $(TESTPREFIX)/unit-test-kb-vault-activation-latch \
               $(TESTPREFIX)/unit-test-kb-vault-tpm-runtime-lock \
               $(TESTPREFIX)/unit-test-vault-maintenance-guard \
               $(TESTPREFIX)/unit-test-vault-d3b-custody \
               $(TESTPREFIX)/unit-test-vault-provider-credential \
               $(TESTPREFIX)/unit-test-kb-vault-key-use \
               $(TESTPREFIX)/unit-test-kb-vault-key-use-live \
               $(TESTPREFIX)/unit-test-kb-vault-rotation \
               $(TESTPREFIX)/unit-test-kb-vault-rotation-ops \
               $(TESTPREFIX)/unit-test-kb-vault-rotation-ops-live \
               $(TESTPREFIX)/unit-test-vault-kms-hwm-live \
               $(TESTPREFIX)/unit-test-kb-vault-rotation-live \
               $(TESTPREFIX)/unit-test-vault-service \
               $(TESTPREFIX)/unit-test-vault-master-rotate \
               $(TESTPREFIX)/unit-test-vault-seal \
               $(TESTPREFIX)/unit-test-vault-tpm2-stub \
               $(TESTPREFIX)/unit-test-git-forge-vault \
               $(TESTPREFIX)/unit-test-git-host-resolve \
               $(TESTPREFIX)/unit-test-git-pr-stage \
               $(TESTPREFIX)/unit-test-git-cred-inject \
               $(TESTPREFIX)/unit-test-git-ssh-agent \
               $(TESTPREFIX)/unit-test-webchat-git-leak \
               $(TESTPREFIX)/unit-test-git-project \
               $(TESTPREFIX)/unit-test-git-ops \
               $(TESTPREFIX)/unit-test-webuser-editor \
               $(TESTPREFIX)/unit-test-vault-bootstrap \
               $(TESTPREFIX)/unit-test-vault-bootstrap-privilege \
               $(TESTPREFIX)/unit-test-pki $(TESTPREFIX)/unit-test-remote-client-grant \
               $(TESTPREFIX)/unit-test-aimee-tls-clientcert \
               $(TESTPREFIX)/unit-test-aimee-tls-pin \
               $(TESTPREFIX)/unit-test-vault-server-key \
               $(TESTPREFIX)/unit-test-vault-capability \
               $(TESTPREFIX)/unit-test-agent-key-import \
               $(TESTPREFIX)/unit-test-vault-audit \
               $(TESTPREFIX)/unit-test-server-vault-gate \
               $(TESTPREFIX)/unit-test-server-cert-grant \
               $(TESTPREFIX)/unit-test-prompts \
               $(TESTPREFIX)/unit-test-cmd-session \
               $(TESTPREFIX)/unit-test-model-registry \
               $(TESTPREFIX)/unit-test-models-dev $(TESTPREFIX)/unit-test-agent-tier-lint \
               $(TESTPREFIX)/unit-test-p3b-spend \
               $(TESTPREFIX)/unit-test-model-provider \
               $(TESTPREFIX)/unit-test-delegate-driver \
               $(TESTPREFIX)/unit-test-agent-http \
               $(TESTPREFIX)/unit-test-middleware \
               $(TESTPREFIX)/unit-test-verify-hook \
               $(TESTPREFIX)/unit-test-pipeline \
               $(TESTPREFIX)/unit-test-process-mgr \
               $(TESTPREFIX)/unit-test-proxy-bootstrap \
               $(TESTPREFIX)/unit-test-cmd-run \
               $(TESTPREFIX)/unit-test-conversation \
               $(TESTPREFIX)/unit-test-agent-loop \
               $(TESTPREFIX)/unit-test-agent-max-turns \
               $(TESTPREFIX)/unit-test-provider-settable \
               $(TESTPREFIX)/unit-test-file-snapshot \
               $(TESTPREFIX)/unit-test-execution-trace \
               $(TESTPREFIX)/unit-test-diagnose \
               $(TESTPREFIX)/unit-test-json-fluent \
               $(TESTPREFIX)/unit-test-cmd-config \
               $(TESTPREFIX)/unit-test-cmd-table \
               $(TESTPREFIX)/unit-test-tool-validation \
               $(TESTPREFIX)/unit-test-turn-narration \
               $(TESTPREFIX)/unit-test-markdown \
               $(TESTPREFIX)/unit-test-kb \
               $(TESTPREFIX)/unit-test-kb-export \
               $(TESTPREFIX)/unit-test-agent-runtime-messages \
               $(TESTPREFIX)/unit-test-minimax-tool-call-args \
               $(TESTPREFIX)/unit-test-delegate-liveness \
               $(TESTPREFIX)/unit-test-agent-parallel \
               $(TESTPREFIX)/unit-test-server-cli-oauth \
               $(TESTPREFIX)/unit-test-workspace-manifest \
               $(TESTPREFIX)/unit-test-lsp \
               $(TESTPREFIX)/unit-test-memory-retrieval-eval \
               $(TESTPREFIX)/unit-test-context-discover \
               $(TESTPREFIX)/unit-test-ensemble \
               $(TESTPREFIX)/unit-test-cli-session \
               $(TESTPREFIX)/unit-test-cli-session-pty \
               $(TESTPREFIX)/unit-test-cli-codex \
               $(TESTPREFIX)/unit-test-delegate-backend \
               $(TESTPREFIX)/unit-test-delegate-backend-docker \
               $(TESTPREFIX)/unit-test-session-compact \
               $(TESTPREFIX)/unit-test-embedder-catalog \
               $(TESTPREFIX)/unit-test-agent-list-handler \
               $(TESTPREFIX)/unit-test-rounds-to-resume \
               $(TESTPREFIX)/unit-test-session-compact-focused \
               $(TESTPREFIX)/unit-test-compact-prune \
               $(TESTPREFIX)/unit-test-otel \
               $(TESTPREFIX)/unit-test-clarify \
               $(TESTPREFIX)/unit-test-collab-rules \
               $(TESTPREFIX)/unit-test-oauth-pkce \
               $(TESTPREFIX)/unit-test-subject-grammar \
               $(TESTPREFIX)/unit-test-kb-http-grants \
               $(TESTPREFIX)/unit-test-kb-oidc-login \
               $(TESTPREFIX)/unit-test-kb-oidc-login-store \
               $(TESTPREFIX)/unit-test-kb-oidc-token-exchange \
               $(TESTPREFIX)/unit-test-kb-oidc-login-flow \
               $(TESTPREFIX)/unit-test-kb-http-identity-login \
               $(TESTPREFIX)/unit-test-oauth-reauth \
               $(TESTPREFIX)/unit-test-mcp-client \
               $(TESTPREFIX)/unit-test-mcp-client-sse \
               $(TESTPREFIX)/unit-test-mcp-client-integration \
               $(TESTPREFIX)/unit-test-mcp-client-registry \
               $(TESTPREFIX)/unit-test-osv-check \
               $(TESTPREFIX)/unit-test-mcp-osv-cache \
               $(TESTPREFIX)/unit-test-plugin-c-hook \
               $(TESTPREFIX)/unit-test-memory-provider \
               $(TESTPREFIX)/unit-test-context-engine \
               $(TESTPREFIX)/unit-test-dogfood \
               $(TESTPREFIX)/unit-test-working-profile \
               $(TESTPREFIX)/unit-test-cmd-onboard \
               $(TESTPREFIX)/unit-test-curiosity \
               $(TESTPREFIX)/unit-test-cmd-identity \
               $(TESTPREFIX)/unit-test-session-briefing \
               $(TESTPREFIX)/unit-test-session-start-util \
               $(TESTPREFIX)/unit-test-memory-assemble-util \
               $(TESTPREFIX)/unit-test-session-brief \
               $(TESTPREFIX)/unit-test-learning-metrics \
               $(TESTPREFIX)/unit-test-learning-eval-synthesis \
               $(TESTPREFIX)/unit-test-learning-regret \
               $(TESTPREFIX)/unit-test-approach-memory \
               $(TESTPREFIX)/unit-test-learning-attribution \
               $(TESTPREFIX)/unit-test-policy-arms \
               $(TESTPREFIX)/unit-test-curiosity-resolve \
               $(TESTPREFIX)/unit-test-eval-candidates \
               $(TESTPREFIX)/unit-test-memory-recall-pivot \
               $(TESTPREFIX)/unit-test-memory-filter \
               $(TESTPREFIX)/unit-test-memory-profiles \
               $(TESTPREFIX)/unit-test-wiki-render \
               $(TESTPREFIX)/unit-test-integrity-gate \
               $(TESTPREFIX)/unit-test-conversation-context \
               $(TESTPREFIX)/unit-test-payload-rewrite \
               $(TESTPREFIX)/unit-test-payload-rewrite-state \
               $(TESTPREFIX)/unit-test-http-content-encoding \
               $(TESTPREFIX)/unit-test-guardrails-semantic \
               $(TESTPREFIX)/unit-test-guardrails-computer-use \
               $(TESTPREFIX)/unit-test-kb-http-routes \
               $(TESTPREFIX)/unit-test-kb-scope \
               $(TESTPREFIX)/unit-test-kb-identity \
               $(TESTPREFIX)/unit-test-kb-identity-resolve \
               $(TESTPREFIX)/unit-test-kb-ingress \
               $(TESTPREFIX)/unit-test-kb-oidc-jwks \
               $(TESTPREFIX)/unit-test-db2-hardening \
               $(TESTPREFIX)/unit-test-kb-tenancy-shim-guard \
               $(TESTPREFIX)/unit-test-kb-models-validate \
               $(TESTPREFIX)/unit-test-kb-route-acl \
               $(TESTPREFIX)/unit-test-kb-enroll \
               $(TESTPREFIX)/unit-test-kb-verifier \
               $(TESTPREFIX)/unit-test-kb-auth-oidc \
               $(TESTPREFIX)/unit-test-kb-pki \
               $(TESTPREFIX)/unit-test-managed-server-identity \
               $(TESTPREFIX)/unit-test-kb-tls \
               $(TESTPREFIX)/unit-test-kb-releases-db \
               $(TESTPREFIX)/unit-test-kb-ingest-format \
               $(TESTPREFIX)/unit-test-kb-ingest-worker-cap \
               $(TESTPREFIX)/unit-test-kb-dense-vector-scope \
               $(TESTPREFIX)/unit-test-mcp-roundtable-contract \
               $(TESTPREFIX)/unit-test-mcp-delegate-contract \
               $(TESTPREFIX)/unit-test-workspace-prune-dead \
               $(TESTPREFIX)/unit-test-workspace-add-idempotent \
               $(TESTPREFIX)/unit-test-kb-doc-pdf \
               $(TESTPREFIX)/unit-test-kb-http-ingest \
               $(TESTPREFIX)/unit-test-kb-releases \
               $(TESTPREFIX)/unit-test-sketch \
               $(TESTPREFIX)/unit-test-kb-fusion \
               $(TESTPREFIX)/unit-test-kb-lab \
               $(TESTPREFIX)/unit-test-artifacts \
               $(TESTPREFIX)/unit-test-evidence-embed \
               $(TESTPREFIX)/unit-test-learning-bundle \
               $(TESTPREFIX)/unit-test-learning-synth \
               $(TESTPREFIX)/unit-test-learning-version \
               $(TESTPREFIX)/unit-test-calibration \
               $(TESTPREFIX)/unit-test-demotion \
               $(TESTPREFIX)/unit-test-fidelity \
               $(TESTPREFIX)/unit-test-fidelity-check \
               $(TESTPREFIX)/unit-test-features \
               $(TESTPREFIX)/unit-test-ranker-fit \
               $(TESTPREFIX)/unit-test-retrieval-outcome-bridge \
               $(TESTPREFIX)/unit-test-td-search-render \
               $(TESTPREFIX)/unit-test-report-enrichments \
               $(TESTPREFIX)/unit-test-reasoning \
               $(TESTPREFIX)/unit-test-bandit \
               $(TESTPREFIX)/unit-test-planner \
               $(TESTPREFIX)/unit-test-roadmap \
               $(TESTPREFIX)/unit-test-roadmap-decompose \
               $(TESTPREFIX)/unit-test-roadmap-auto \
               $(TESTPREFIX)/unit-test-kb-mdl \
               $(TESTPREFIX)/unit-test-trigger \
               $(TESTPREFIX)/unit-test-trigger-e2e \
               $(TESTPREFIX)/unit-test-kb-mining \
               $(TESTPREFIX)/unit-test-corpus-structural \
               $(TESTPREFIX)/unit-test-corpus-jobs \
               $(TESTPREFIX)/unit-test-corpus-terms-gaps \
               $(TESTPREFIX)/unit-test-kb-maintenance \
               $(TESTPREFIX)/unit-test-agent-policy-intercept \
               $(TESTPREFIX)/unit-test-delegate-dispatch-reliability \
               $(TESTPREFIX)/unit-test-curator-code-unit \
               $(TESTPREFIX)/unit-test-curator-resolve-entities \
               $(TESTPREFIX)/unit-test-curator-index-narrative \
               $(TESTPREFIX)/unit-test-curator-index-claims \
               $(TESTPREFIX)/unit-test-curator-contradictions \
               $(TESTPREFIX)/unit-test-curator-index-code-unit \
               $(TESTPREFIX)/unit-test-curator-link-artifacts \
               $(TESTPREFIX)/unit-test-curator-serve \
               $(TESTPREFIX)/unit-test-curator-pipeline \
               $(TESTPREFIX)/unit-test-curator-judge \
               $(TESTPREFIX)/unit-test-kb-surprising-judge \
               $(TESTPREFIX)/unit-test-curator-synthesize \
               $(TESTPREFIX)/unit-test-kb-reflection \
               $(TESTPREFIX)/unit-test-curator-promote \
               $(TESTPREFIX)/unit-test-db1-write-retry \
               $(TESTPREFIX)/unit-test-db1-agent-job-heartbeat \
               $(TESTPREFIX)/unit-test-db1-delegate-reservation \
               $(TESTPREFIX)/unit-test-obs-bus-module-concurrency \
               $(TESTPREFIX)/unit-test-db1-agent-job-cancel-unassigned \
               $(TESTPREFIX)/unit-test-server-delegate-monitor \
               $(TESTPREFIX)/unit-test-db1-delegation-recursive-cancel \
               $(TESTPREFIX)/unit-test-tool-args-coerce \
               $(TESTPREFIX)/unit-test-tool-schema-sanitizer \
               $(TESTPREFIX)/unit-test-toolset \
               $(TESTPREFIX)/unit-test-db1-cost-fold \
               $(TESTPREFIX)/unit-test-db1-module-stage \
               $(TESTPREFIX)/unit-test-db1-git-ownership-client \
               $(TESTPREFIX)/unit-test-db1-conversation-client \
               $(TESTPREFIX)/unit-test-db1-agent-work-client \
               $(TESTPREFIX)/unit-test-db1-module-bus \
               $(TESTPREFIX)/unit-test-bus-memory-recall \
               $(TESTPREFIX)/unit-test-bus-memory-upsert \
               $(TESTPREFIX)/unit-test-bus-audit-replay \
               $(TESTPREFIX)/unit-test-bus-audit-replay-tool \
               $(TESTPREFIX)/unit-test-bus-audit-retention \
               $(TESTPREFIX)/unit-test-bus-config-autonomy \
               $(TESTPREFIX)/unit-test-bus-guardrail-durability \
               $(TESTPREFIX)/unit-test-bus-shutdown-race \
               $(TESTPREFIX)/unit-test-git-verify-state-bus \
               $(TESTPREFIX)/unit-test-bus-audit-durability \
               $(TESTPREFIX)/unit-test-bus-memory-audit \
               $(TESTPREFIX)/unit-test-bus-sandbox-audit \
               $(TESTPREFIX)/unit-test-bus-tool-completion \
               $(TESTPREFIX)/unit-test-bus-vault-audit \
               $(TESTPREFIX)/unit-test-db1-roundtable-pipeline \
               $(TESTPREFIX)/unit-test-roundtable-pipeline-eval \
               $(TESTPREFIX)/unit-test-roundtable-pipeline-chunk \
               $(TESTPREFIX)/unit-test-roundtable-pipeline-ctl \
               $(TESTPREFIX)/unit-test-roundtable-pipeline-capture \
               $(TESTPREFIX)/unit-test-db1-session-paths \
               $(TESTPREFIX)/unit-test-interaction-events \
               $(TESTPREFIX)/unit-test-trajectory \
               $(TESTPREFIX)/unit-test-trajectory-batch \
               $(TESTPREFIX)/unit-test-delegate-credentials \
               $(TESTPREFIX)/unit-test-curator-fixtures \
               $(TESTPREFIX)/unit-test-substrate-fixtures \
               $(TESTPREFIX)/unit-test-org-telemetry \
               $(TESTPREFIX)/unit-test-aws-auth \
               $(TESTPREFIX)/unit-test-org-model-catalog-target \
               $(TESTPREFIX)/unit-test-kb-mgmt-endpoint \
               $(TESTPREFIX)/unit-test-kb-mgmt-status \
               $(TESTPREFIX)/unit-test-kb-mgmt-status-listener \
               $(TESTPREFIX)/unit-test-server-mgmt-status \
               $(TESTPREFIX)/unit-test-server-mgmt-token \
               $(TESTPREFIX)/unit-test-server-mgmt-endpoint \
               $(TESTPREFIX)/unit-test-server-mgmt-read \
               $(TESTPREFIX)/unit-test-server-mgmt-read-source \
               $(TESTPREFIX)/unit-test-server-mgmt-read-endpoint \
               $(TESTPREFIX)/unit-test-server-mgmt-checkpoint-client \
               $(TESTPREFIX)/unit-test-kb-mgmt-token \
               $(TESTPREFIX)/unit-test-kb-identity-token \
               $(TESTPREFIX)/unit-test-workspace-scan-indexed \
               $(TESTPREFIX)/unit-test-server-active-project \
               $(TESTPREFIX)/unit-test-kb-login-throttle \
               $(TESTPREFIX)/unit-test-server-identity-token \
               $(TESTPREFIX)/unit-test-server-write-tier-db1 \
               $(TESTPREFIX)/unit-test-server-identity-jti \
               $(TESTPREFIX)/unit-test-server-management-jti \
               $(TESTPREFIX)/unit-test-server-management-tls \
               $(TESTPREFIX)/unit-test-kb-mgmt-status-authority \
               $(TESTPREFIX)/unit-test-kb-management-health-exchange \
               $(TESTPREFIX)/unit-test-kb-mgmt-status-client \
               $(TESTPREFIX)/unit-test-kb-management-runtime \
               $(TESTPREFIX)/unit-test-kb-http-servers-health \
               $(TESTPREFIX)/unit-test-kb-management-action \
               $(TESTPREFIX)/unit-test-aws-eventstream
TEST_TARGETS += $(TESTPREFIX)/unit-test-command-registry
TEST_TARGETS +=
TEST_TARGETS += $(TESTPREFIX)/unit-test-kb-mgmt-status-custody \
                $(TESTPREFIX)/unit-test-management-status-key-ctx \
                $(TESTPREFIX)/unit-test-kb-mgmt-status-provision \
                $(TESTPREFIX)/unit-test-management-status-runtime
TEST_TARGETS += $(TESTPREFIX)/unit-test-kb-workload-wire \
                $(TESTPREFIX)/unit-test-kb-workload-proof \
                $(TESTPREFIX)/unit-test-kb-workload-jwt \
                $(TESTPREFIX)/unit-test-kb-workload-helper-posix \
                $(TESTPREFIX)/unit-test-kb-workload-provider
TEST_TARGETS += $(TESTPREFIX)/unit-test-management-client-instance
TEST_TARGETS += $(TESTPREFIX)/unit-test-management-action-journal
TEST_TARGETS += $(TESTPREFIX)/unit-test-management-identity-journal
TEST_TARGETS += $(TESTPREFIX)/unit-test-kb-management-cert-lifecycle
TEST_TARGETS += $(TESTPREFIX)/unit-test-kb-mgmt-token-roots-provision
TEST_TARGETS += $(TESTPREFIX)/unit-test-kb-mgmt-token-authority
TEST_TARGETS += $(TESTPREFIX)/unit-test-server-write-tier
TEST_TARGETS += $(TESTPREFIX)/unit-test-kb-identity-token-authority
TEST_TARGETS += $(TESTPREFIX)/unit-test-kb-mgmt-token-authority-ipc
TEST_TARGETS += $(TESTPREFIX)/unit-test-kb-mgmt-jwks-publication
TEST_TARGETS += $(TESTPREFIX)/unit-test-server-mgmt-jwks-cache
TEST_TARGETS += $(TESTPREFIX)/unit-test-kb-mgmt-offline-hardening
TEST_TARGETS += $(TESTPREFIX)/unit-test-communication
TEST_TARGETS += $(TESTPREFIX)/unit-test-process-module-handlers
TEST_TARGETS += $(TESTPREFIX)/unit-test-db2-module-contract \
                $(TESTPREFIX)/unit-test-bus-db2-module \
                $(TESTPREFIX)/unit-test-db2-module-init \
                $(TESTPREFIX)/unit-test-db2-cert-serial-support \
                $(TESTPREFIX)/unit-test-db2-cjson-support \
                $(TESTPREFIX)/unit-test-db2-cochange-support \
                $(TESTPREFIX)/unit-test-db2-code-audit-graph-support \
                $(TESTPREFIX)/unit-test-db2-code-import-support \
                $(TESTPREFIX)/unit-test-db2-code-match-support \
                $(TESTPREFIX)/unit-test-db2-dstr-support \
                $(TESTPREFIX)/unit-test-db2-extractor-support \
                $(TESTPREFIX)/unit-test-db2-log-support \
                $(TESTPREFIX)/unit-test-db2-management-read-support \
                $(TESTPREFIX)/unit-test-db2-model-validation-support \
                $(TESTPREFIX)/unit-test-db2-node-kind-text-support \
                $(TESTPREFIX)/unit-test-db2-pii-classifier-support \
                $(TESTPREFIX)/unit-test-db2-pii-inject-gate-support \
                $(TESTPREFIX)/unit-test-db2-random-support \
                $(TESTPREFIX)/unit-test-db2-rel-enum-text-support \
                $(TESTPREFIX)/unit-test-db2-rel-seed-support \
                $(TESTPREFIX)/unit-test-db2-rel-type-support \
                $(TESTPREFIX)/unit-test-db2-runtime-config-support \
                $(TESTPREFIX)/unit-test-db2-sketch-support \
                $(TESTPREFIX)/unit-test-db2-text-support \
                $(TESTPREFIX)/unit-test-db2-time-support \
                $(TESTPREFIX)/unit-test-db3-route \
                $(TESTPREFIX)/unit-test-bus-db3

MODULE_HANDLER_TEST_OBJS = \
   $(OBJDIR)/tests/module_handlers/memory.o \
   $(OBJDIR)/tests/module_handlers/learning.o \
   $(OBJDIR)/tests/module_handlers/delegates.o \
   $(OBJDIR)/tests/module_handlers/tools.o \
   $(OBJDIR)/tests/module_handlers/workspace.o \
   $(OBJDIR)/tests/module_handlers/git.o \
   $(OBJDIR)/tests/module_handlers/skills.o \
   $(OBJDIR)/tests/module_handlers/governance.o \
   $(OBJDIR)/tests/module_handlers/workflows.o \
   $(OBJDIR)/tests/module_handlers/roundtable.o \
   $(OBJDIR)/tests/module_handlers/kb_synthesis.o \
   $(OBJDIR)/tests/module_handlers/runtime_web.o \
   $(OBJDIR)/tests/module_handlers/control_web.o \
   $(OBJDIR)/tests/module_handlers/benchmarks.o \
   $(OBJDIR)/tests/module_handlers/providers.o

define module_handler_test_object
$(OBJDIR)/tests/module_handlers/$(1).o: modules/$(2)/module_adapter.c
	@mkdir -p $$(dir $$@)
	$$(CC) $$(TEST_C_FLAGS) -Daimee_module_handler=aimee_$(1)_module_handler -c -o $$@ $$<
endef
$(eval $(call module_handler_test_object,memory,memory))
$(eval $(call module_handler_test_object,learning,learning))
$(eval $(call module_handler_test_object,delegates,delegates))
$(eval $(call module_handler_test_object,tools,tools))
$(eval $(call module_handler_test_object,workspace,workspace))
$(eval $(call module_handler_test_object,git,git))
$(eval $(call module_handler_test_object,skills,skills))
$(eval $(call module_handler_test_object,governance,governance))
$(eval $(call module_handler_test_object,workflows,workflows))
$(eval $(call module_handler_test_object,roundtable,roundtable))
$(eval $(call module_handler_test_object,kb_synthesis,kb-synthesis))
$(eval $(call module_handler_test_object,runtime_web,runtime-web))
$(eval $(call module_handler_test_object,control_web,control-web))
$(eval $(call module_handler_test_object,benchmarks,benchmarks))
$(eval $(call module_handler_test_object,providers,providers))

$(TESTPREFIX)/unit-test-process-module-handlers: \
   $(OBJDIR)/tests/test_process_module_handlers.o $(MODULE_HANDLER_TEST_OBJS) \
   $(OBJDIR)/modules/skills/skill_trigger_policy.o \
   $(OBJDIR)/modules/learning/learning_signal_policy.o
	$(TESTLINK_MIN) -o $@ $^ $(EXTRA_L_FLAGS) -lm

# The shared connection archive is the only implementation of endpoint,
# credential, and OpenSSL mTLS primitives. Tests may link it through L_CORE,
# L_CLIENT, L_SERVER, or L_KB; the order-only edge guarantees the archive exists
# before any selected test link starts.
$(TEST_TARGETS): | $(CORE_CONNECTION_LIB)

.PHONY: unit-test-communication
unit-test-communication: $(TESTPREFIX)/unit-test-communication
	$<

$(TESTPREFIX)/unit-test-communication: $(OBJDIR)/tests/test_communication.o \
                                        $(CORE_CONNECTION_LIB)
	$(TESTLINK_MIN) -o $@ $^ -lssl -lcrypto

# CI can split the suite across independent runners without changing the local
# contract: the defaults still build and execute every test in one invocation.
# Round-robin selection keeps adjacent families from piling into one shard and
# preserves process isolation -- every selected target is still the same binary
# used by the full suite.
UNIT_TEST_SHARD_COUNT ?= 1
UNIT_TEST_SHARD_INDEX ?= 0
UNIT_TEST_SKIP_P1 ?= 0
ifeq ($(UNIT_TEST_SHARD_INDEX),0)
UNIT_TEST_AUX_TARGETS = $(TESTPREFIX)/unit-test-db2-dstr-support-sanitize \
                        $(TESTPREFIX)/unit-test-db2-cert-serial-support-sanitize \
                        $(TESTPREFIX)/unit-test-db2-cjson-support-sanitize \
                        $(TESTPREFIX)/unit-test-db2-cochange-support-sanitize \
                        $(TESTPREFIX)/unit-test-db2-code-audit-graph-support-sanitize \
                        $(TESTPREFIX)/unit-test-db2-code-import-support-sanitize \
                        $(TESTPREFIX)/unit-test-db2-code-match-support-sanitize \
                        $(TESTPREFIX)/unit-test-db2-extractor-support-sanitize \
                        $(TESTPREFIX)/unit-test-db2-log-support-sanitize \
                        $(TESTPREFIX)/unit-test-db2-management-read-support-sanitize \
                        $(TESTPREFIX)/unit-test-db2-model-validation-support-sanitize \
                        $(TESTPREFIX)/unit-test-db2-node-kind-text-support-sanitize \
                        $(TESTPREFIX)/unit-test-db2-pii-classifier-support-sanitize \
                        $(TESTPREFIX)/unit-test-db2-pii-inject-gate-support-sanitize \
                        $(TESTPREFIX)/unit-test-db2-random-support-sanitize \
                        $(TESTPREFIX)/unit-test-db2-rel-enum-text-support-sanitize \
                        $(TESTPREFIX)/unit-test-db2-rel-seed-support-sanitize \
                        $(TESTPREFIX)/unit-test-db2-rel-type-support-sanitize \
                        $(TESTPREFIX)/unit-test-db2-runtime-config-support-sanitize \
                        $(TESTPREFIX)/unit-test-db2-sketch-support-sanitize \
                        $(TESTPREFIX)/unit-test-db2-text-support-sanitize \
                        $(TESTPREFIX)/unit-test-db2-time-support-sanitize
else
UNIT_TEST_AUX_TARGETS =
endif
ifeq ($(UNIT_TEST_SHARD_COUNT),1)
UNIT_TEST_TARGETS := $(TEST_TARGETS)
else
UNIT_TEST_TARGETS := $(shell printf '%s\n' $(TEST_TARGETS) | \
	awk -v count='$(UNIT_TEST_SHARD_COUNT)' -v shard='$(UNIT_TEST_SHARD_INDEX)' \
	    'count > 0 && shard >= 0 && shard < count && slot == shard { print } \
	     { slot = slot + 1; slot %= count }')
endif

ifeq ($(UNIT_TEST_SKIP_P1),1)
UNIT_TEST_P1_PREREQ :=
else
UNIT_TEST_P1_PREREQ := p1-rls-gate-check
endif

# The verify ledger lives in the git module now, so a suite that exercises the
# verify gate brings the real module up on a real bus
# (tests/support/git_module_fixture.c). Build it here and name it in the
# environment, so no suite has to grow an argv contract to find it.
unit-tests: $(UNIT_TEST_P1_PREREQ) $(BINARY) $(OBJDIR)/aimee-module $(OBJDIR)/aimee-module-config \
            $(UNIT_TEST_TARGETS) $(UNIT_TEST_AUX_TARGETS)
	@if ! printf '%s:%s\n' "$(UNIT_TEST_SHARD_COUNT)" "$(UNIT_TEST_SHARD_INDEX)" | \
	     awk -F: '$$1 ~ /^[0-9]+$$/ && $$2 ~ /^[0-9]+$$/ && $$1 > 0 && $$2 < $$1 { ok=1 } END { exit !ok }'; then \
	  echo "invalid unit-test shard $(UNIT_TEST_SHARD_INDEX)/$(UNIT_TEST_SHARD_COUNT)" >&2; \
	  exit 2; \
	fi
	@echo "Unit-test shard $(UNIT_TEST_SHARD_INDEX)/$(UNIT_TEST_SHARD_COUNT): $(words $(UNIT_TEST_TARGETS)) binaries"
	@# Point the run's HOME at a throwaway dir so a test that does NOT isolate its
	@# own environment defaults to $$th/.config/aimee, never the developer's real
	@# ~/.config/aimee — the dir a running aimee-server reads agents.json, config
	@# and the vault from. The suite and a server share it whenever AIMEE_HOME is
	@# unset, so an unisolated test could read or overwrite live state. (No current
	@# test was proven to do so — a full run under isolation writes nothing there —
	@# but "none does today" is luck, not a boundary.)
	@#
	@# HOME only, NOT AIMEE_HOME: aimee_home() checks AIMEE_HOME before HOME, so
	@# exporting AIMEE_HOME would OVERRIDE the many tests that set HOME themselves
	@# to steer the config dir (e.g. test_session_brief). Exporting HOME leaves the
	@# default safe while a test's own setenv still wins inside its process.
	@# TMPDIR as well as HOME: platform_tmpdir() honours it, so the directories
	@# tests create with platform_mkdtemp() land inside $$th and go with it on
	@# EXIT. 64 of the 112 tests that make one never remove it, and they used to
	@# pile up in /tmp across every run until tmpfs ran out of INODES (40k dirs,
	@# 857k inodes, with 45GB still free) and nothing could create a file.
	@# Short name on purpose. Every test temp dir now hangs off this prefix, so
	@# its length is added to every path the suite creates. At least one test —
	@# test_parent_write_guard_readonly_large_find — lists 300 files through
	@# tool_bash, whose head+tail compaction drops the middle; the longer the
	@# paths, the more bytes and the tighter the cut, and with
	@# "aimee-unit-home.XXXXXX" (22 chars over /tmp) its final entry fell out of
	@# the preserved tail and the assertion failed. That test is fragile about
	@# output size — its own comment admits as much — but a short prefix costs
	@# nothing and keeps this change from perturbing it.
	@# The outer `aimee git verify` process holds the host-wide verifier lock
	@# while this suite runs. A few white-box tests invoke handle_git_verify again;
	@# give those nested test invocations their own lock instead of deadlocking on
	@# their parent process, while still serializing them with one another.
	@# The failure markers live in their OWN directory, NOT in $$th.
	@# $$th is the tests' HOME and TMPDIR, and (per the note above) most tests
	@# that create anything create it there; a test that removes or recreates its
	@# HOME takes every other parallel job's marker with it. The runner then
	@# printed "Unit test failures:" followed by NOTHING, which is the least
	@# useful possible output: a red suite that will not say what went red, so the
	@# whole run has to be repeated serially just to recover the name.
	@th="$$(mktemp -d /tmp/aut.XXXXXX)"; \
	fd="$$(mktemp -d /tmp/autf.XXXXXX)"; \
	export HOME="$$th" TMPDIR="$$th" AIMEE_VERIFY_LOCK_FILE="$$th/nested-verify.lock"; \
	unset AIMEE_HOME AIMEE_API_REMOTE_WRITES AIMEE_API_MTLS AIMEE_API_BEARER_TOKEN \
	  AIMEE_SERVER_HTTP_BIND AIMEE_WORKSPACES_DIR AIMEE_KB_API_URL \
	  AIMEE_KB_API_BEARER_TOKEN AIMEE_WFE_ENGINE AIMEE_WFE_HTTP_SOCKET; \
	trap 'rm -rf "$$th" "$$fd"' EXIT; \
	export AIMEE_TEST_FAILURE_DIR="$$fd"; \
	export AIMEE_TEST_MODULE_BIN="$(CURDIR)/$(OBJDIR)/aimee-module"; \
	export AIMEE_CONFIG_TEST_DEFAULTS="$(CONFIG_TEST_DEFAULTS)"; \
	export AIMEE_CONFIG_TEST_MODULE="$(CURDIR)/$(OBJDIR)/aimee-module-config"; \
	export AIMEE_CONFIG_TEST_HOST_HOME="$$HOME"; \
	jobs="$(TEST_RUN_JOBS)"; \
	if [ "$$jobs" -le 1 ]; then \
	  for t in $(UNIT_TEST_TARGETS); do \
	    log="$$(mktemp /tmp/aimee-test-run.XXXXXX)"; \
	    echo "  $$t"; \
	    "./$$t" >"$$log" 2>&1; \
	    rc="$$?"; \
	    cat "$$log"; \
	    rm -f "$$log"; \
	    [ "$$rc" -eq 0 ] || exit "$$rc"; \
	  done; \
	else \
	  if ! printf '%s\0' $(UNIT_TEST_TARGETS) | \
	    xargs -0 -n1 -P "$$jobs" sh -c 't="$$1"; log="$$(mktemp /tmp/aimee-test-run.XXXXXX)"; echo "  $$t"; "./$$t" >"$$log" 2>&1; rc="$$?"; cat "$$log"; rm -f "$$log"; if [ "$$rc" -ne 0 ]; then echo "FAILED: $$t" >&2; printf "FAILED: %s\\n" "$$t" >"$$AIMEE_TEST_FAILURE_DIR/failure.$$$$"; fi; exit "$$rc"' _; then \
	    echo "Unit test failures:" >&2; \
	    if ! cat "$$fd"/failure.* >&2 2>/dev/null; then \
	      echo "  (no marker written: a test was killed by a signal before it could" >&2; \
	      echo "   record itself, or xargs itself failed - re-run with TEST_RUN_JOBS=1)" >&2; \
	    fi; \
	    exit 1; \
	  fi; \
	fi
	@for t in $(UNIT_TEST_AUX_TARGETS); do \
	  echo "  $$t"; \
	  "./$$t"; \
	done
	@echo "All tests passed."

$(TESTPREFIX)/unit-test-util: $(OBJDIR)/tests/test_util.o $(OBJDIR)/util.o $(OBJDIR)/text.o \
                     $(OBJDIR)/cJSON.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-db: $(OBJDIR)/tests/test_db.o $(OBJDIR)/modules/db1/db.o $(OBJDIR)/modules/db1/db_schema.o $(OBJDIR)/db2/db_schema.o $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db1_write.o $(OBJDIR)/modules/db1/db1_trigger.o $(OBJDIR)/modules/db1/db1_cron_jobs.o $(OBJDIR)/modules/db1/model_catalog.o $(OBJDIR)/model_catalog_release.o $(OBJDIR)/modules/db1/maintenance.o $(OBJDIR)/modules/db1/eval.o $(DB2_TEST_BACKEND_OBJ) $(OBJDIR)/db2/db2_test_shim.o $(CONFIG_CLIENT_TEST_OBJS) \
                    $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o \
                    $(OBJDIR)/util.o $(OBJDIR)/text.o $(OBJDIR)/platform_random.o \
                    $(OBJDIR)/log.o $(PLATFORM_BASIC_OBJS) $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-roundtable-brief: $(OBJDIR)/tests/test_roundtable_brief.o $(OBJDIR)/server/server_compute_roundtable.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-harness-memory-spill: $(OBJDIR)/tests/test_harness_memory_spill.o $(OBJDIR)/harness_memory_spill.o $(OBJDIR)/harness_memory_common.o $(OBJDIR)/aimee_home.o $(OBJDIR)/posix/platform_path.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-harness-memory-audit: $(OBJDIR)/tests/test_harness_memory_audit.o $(OBJDIR)/harness_memory_audit.o $(OBJDIR)/aimee_home.o $(OBJDIR)/posix/platform_path.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-harness-memory-scope: $(OBJDIR)/tests/test_harness_memory_scope.o $(OBJDIR)/harness_memory_scope.o $(OBJDIR)/aimee_home.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-memory-redirect: $(OBJDIR)/tests/test_memory_redirect.o $(OBJDIR)/modules/memory/memory_redirect.o $(OBJDIR)/harness_memory_scope.o $(OBJDIR)/aimee_home.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-harness-memory: $(OBJDIR)/tests/test_harness_memory.o $(OBJDIR)/modules/db1/user_memory.o $(OBJDIR)/user_memory_merge.o $(OBJDIR)/harness_memory_common.o $(OBJDIR)/modules/db1/db.o $(OBJDIR)/modules/db1/db_schema.o $(OBJDIR)/db2/db_schema.o $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db1_write.o $(OBJDIR)/modules/db1/db1_trigger.o $(OBJDIR)/modules/db1/db1_cron_jobs.o $(OBJDIR)/modules/db1/model_catalog.o $(OBJDIR)/model_catalog_release.o $(OBJDIR)/modules/db1/maintenance.o $(OBJDIR)/modules/db1/eval.o $(DB2_TEST_BACKEND_OBJ) $(OBJDIR)/db2/db2_test_shim.o $(CONFIG_CLIENT_TEST_OBJS) \
                    $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o \
                    $(OBJDIR)/util.o $(OBJDIR)/text.o $(OBJDIR)/platform_random.o \
                    $(OBJDIR)/log.o $(PLATFORM_BASIC_OBJS) $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-webchat-claude-sessions: $(OBJDIR)/tests/test_webchat_claude_sessions.o $(OBJDIR)/modules/db1/webchat_claude_sessions.o $(OBJDIR)/modules/db1/db.o $(OBJDIR)/modules/db1/db_schema.o $(OBJDIR)/db2/db_schema.o $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db1_write.o $(OBJDIR)/modules/db1/db1_trigger.o $(OBJDIR)/modules/db1/db1_cron_jobs.o $(OBJDIR)/modules/db1/model_catalog.o $(OBJDIR)/model_catalog_release.o $(OBJDIR)/modules/db1/maintenance.o $(OBJDIR)/modules/db1/eval.o $(DB2_TEST_BACKEND_OBJ) $(OBJDIR)/db2/db2_test_shim.o $(CONFIG_CLIENT_TEST_OBJS) \
                    $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o \
                    $(OBJDIR)/util.o $(OBJDIR)/text.o $(OBJDIR)/platform_random.o \
                    $(OBJDIR)/log.o $(PLATFORM_BASIC_OBJS) $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-db2: $(OBJDIR)/tests/test_db2.o $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o \
                            $(OBJDIR)/db2/entity_edges.o
	$(TESTLINK) -o $@ $^ $(L_CORE)

# Names the sqlite shim object directly rather than $(DB2_TEST_BACKEND_OBJ): this is
# a white-box test OF the shim, not of DB2 through it. It hands aimee_pg_prepare_ex a
# raw sqlite3* and drives the RESOURCE classification with sqlite3_hard_heap_limit64,
# neither of which means anything to libpq. Under AIMEE_TEST_PG=1 the backend swap
# would point it at db_postgres.o and every assertion would be about the wrong
# implementation.
$(TESTPREFIX)/unit-test-pg-prepare-classification: \
                                      $(OBJDIR)/tests/test_pg_prepare_classification.o \
                                      $(OBJDIR)/tests/aimee_pg_sqlite_shim.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lsqlite3 -lm

$(TESTPREFIX)/unit-test-schema-subst: $(OBJDIR)/tests/test_schema_subst.o \
                                      $(OBJDIR)/db2/db_schema.o
	$(TESTLINK) -o $@ $^ $(L_CORE)

# P9a telemetry pure-helper tests (Prometheus render/escape, metric_name
# validation, token sha256 + constant-time compare, content-free structural
# check). Links only the dependency-light org_telemetry_fmt.o (+ OpenSSL for
# sha256 via TEST_L_FLAGS); the DB-backed paths live in the real-PG gate.
$(OBJDIR)/tests/test_org_telemetry.o: schema_data.h
$(TESTPREFIX)/unit-test-org-telemetry: $(OBJDIR)/tests/test_org_telemetry.o \
                                       $(OBJDIR)/db2/org_telemetry_fmt.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# P6a AWS-auth core pure tests: SigV4 (published aws-sig-v4-test-suite vectors),
# STS body/parse + web-identity JWT signature verify, bedrock least-privilege
# session policy, and STS session-cache isolation. Links only the kb-only
# modules/aws/*.o + cJSON (JWT claim parse) + OpenSSL (HMAC/SHA256/JWT verify) via
# TEST_L_FLAGS — no DB, no network (pure/offline).
$(TESTPREFIX)/unit-test-aws-auth: $(OBJDIR)/tests/test_aws_auth.o \
                                  $(OBJDIR)/modules/aws/aws_sigv4.o \
                                  $(OBJDIR)/modules/aws/aws_sts.o \
                                  $(OBJDIR)/modules/aws/bedrock_policy.o \
                                  $(OBJDIR)/modules/aws/sts_cache.o \
                                  $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-org-model-catalog-target: \
                                  $(OBJDIR)/tests/test_org_model_catalog_target.o \
                                  $(OBJDIR)/db2/org_model_catalog.o \
                                  $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# P6b AWS eventstream framing decoder pure test: CRC32 vectors, valid decode,
# the NEED_MORE/ERROR bounds matrix, exception/error frames, concatenated +
# rolling-buffer resume, BE->host integer swap, and the deterministic fuzz
# sweep (memory-safety gate). Links ONLY aws_eventstream.o — CRC32 is
# self-contained, so no OpenSSL/zlib/cJSON.
$(TESTPREFIX)/unit-test-aws-eventstream: $(OBJDIR)/tests/test_aws_eventstream.o \
                                         $(OBJDIR)/tests/support/aws_eventstream_fixture.o \
                                         $(OBJDIR)/modules/aws/aws_eventstream.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-code-index-ops: \
                                       $(OBJDIR)/tests/test_code_index_ops.o \
                                       $(OBJDIR)/db2/code_index_ops.o \
                                       $(OBJDIR)/db2/code_index.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-code-project-lifecycle: \
                                       $(OBJDIR)/tests/test_code_project_lifecycle.o \
                                       $(OBJDIR)/db2/code_index.o \
	$(OBJDIR)/db2/code_project_lifecycle.o \
	$(OBJDIR)/db2/cross_repo_resolver.o \
	$(OBJDIR)/db2/kb_audit_worm.o \
                                       $(OBJDIR)/modules/audit/audit_worm_chain.o \
                                       $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

# Cross-repo dependency graph S3: DB stats layer over the sqlite shim (portable
# SQL). Links the db2 init/pool/schema + the shim core like code-index-ops.
$(TESTPREFIX)/unit-test-cross-repo-stats: \
                                       $(OBJDIR)/tests/test_cross_repo_stats.o \
                                       $(OBJDIR)/db2/cross_repo_stats.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

# S4a orchestration over the sqlite shim (portable candidate-gen + pure core).
$(TESTPREFIX)/unit-test-cross-repo-deps-orch: \
                                       $(OBJDIR)/tests/test_cross_repo_deps_orch.o \
                                       $(OBJDIR)/db2/cross_repo_deps.o \
                                       $(OBJDIR)/db2/cross_repo_stats.o \
                                       $(OBJDIR)/db2/cross_repo_resolver.o \
                                       $(OBJDIR)/db2/cross_repo_classify.o \
                                       $(OBJDIR)/db2/cross_repo_review.o \
                                       $(OBJDIR)/db2/cross_repo_identity.o \
                                       $(OBJDIR)/db2/cross_repo_route.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-cross-repo-identity: \
                                       $(OBJDIR)/tests/test_cross_repo_identity.o \
                                       $(OBJDIR)/db2/cross_repo_identity.o \
                                       $(OBJDIR)/db2/cross_repo_deps.o \
                                       $(OBJDIR)/db2/cross_repo_stats.o \
                                       $(OBJDIR)/db2/cross_repo_resolver.o \
                                       $(OBJDIR)/db2/cross_repo_classify.o \
                                       $(OBJDIR)/db2/cross_repo_review.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-cross-repo-route: \
                                       $(OBJDIR)/tests/test_cross_repo_route.o \
                                       $(OBJDIR)/db2/cross_repo_route.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-cross-repo-build: \
                                       $(OBJDIR)/tests/test_cross_repo_build.o \
                                       $(OBJDIR)/db2/cross_repo_build.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-cross-repo-acceptance: \
                                       $(OBJDIR)/tests/test_cross_repo_acceptance.o \
                                       $(OBJDIR)/db2/cross_repo_deps.o \
                                       $(OBJDIR)/db2/cross_repo_stats.o \
                                       $(OBJDIR)/db2/cross_repo_resolver.o \
                                       $(OBJDIR)/db2/cross_repo_classify.o \
                                       $(OBJDIR)/db2/cross_repo_review.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

# S4b review queue + adjudication over the sqlite shim.
$(TESTPREFIX)/unit-test-cross-repo-review: \
                                       $(OBJDIR)/tests/test_cross_repo_review.o \
                                       $(OBJDIR)/db2/cross_repo_review.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

# auditable-correctness P3 fidelity storage substrate over the sqlite shim.
$(TESTPREFIX)/unit-test-fidelity: \
                                       $(OBJDIR)/tests/test_fidelity.o \
                                       $(OBJDIR)/db2/fidelity.o \
                                       $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

# auditable-correctness P3 fidelity-check eligibility (fail-closed gate). The
# helper only reads legacy_config_record fields, so it links standalone.
$(TESTPREFIX)/unit-test-fidelity-check: \
                                       $(OBJDIR)/tests/test_fidelity_check.o \
                                       $(OBJDIR)/server/fidelity_check.o \
                                       $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# CSS style-graph persistence (WP-B) over the sqlite shim.
$(TESTPREFIX)/unit-test-css-graph: \
                                       $(OBJDIR)/tests/test_css_graph.o \
                                       $(OBJDIR)/db2/css_graph.o \
                                       $(OBJDIR)/db2/kb_runtime_state.o \
                                       $(OBJDIR)/modules/css/css_analyze.o \
                                       $(OBJDIR)/db2/code_index.o \
                                       $(OBJDIR)/db2/cross_repo_resolver.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

# CSS analysis signals (!important / specificity / unused vars / token candidates).
$(TESTPREFIX)/unit-test-css-insights: \
                                       $(OBJDIR)/tests/test_css_insights.o \
                                       $(OBJDIR)/db2/css_insights.o \
                                       $(OBJDIR)/db2/css_graph.o \
                                       $(OBJDIR)/db2/kb_runtime_state.o \
                                       $(OBJDIR)/modules/css/css_analyze.o \
                                       $(OBJDIR)/db2/code_index.o \
                                       $(OBJDIR)/db2/cross_repo_resolver.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

# Typed-fact store + write gate over the sqlite shim.
$(TESTPREFIX)/unit-test-typed-facts: \
                                       $(OBJDIR)/tests/test_typed_facts.o \
                                       $(OBJDIR)/db2/typed_facts.o \
                                       $(TEST_FACT_MUTATION_MIN_OBJS) \
                                       $(OBJDIR)/db2/rel_types_store.o \
                                       $(OBJDIR)/db2/fact_recall.o \
                                       $(OBJDIR)/db2/entity_edges.o \
                                       $(OBJDIR)/db2/entity_registry.o \
                                       $(OBJDIR)/db2/ontology_evolution.o \
                                       $(OBJDIR)/db2/fact_lifecycle.o \
                                       $(OBJDIR)/modules/memory/memory_fact_gate.o \
                                       $(OBJDIR)/rel_types.o \
                                       $(OBJDIR)/modules/memory/memory_pii_gate.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

# CSS migration pipeline driver (WP-F) over the sqlite shim.
$(TESTPREFIX)/unit-test-css-migration: \
                                       $(OBJDIR)/tests/test_css_migration.o \
                                       $(OBJDIR)/db2/css_migration.o \
                                       $(OBJDIR)/db2/css_graph.o \
                                       $(OBJDIR)/db2/kb_runtime_state.o \
                                       $(OBJDIR)/db2/typed_facts.o \
                                       $(TEST_FACT_MUTATION_MIN_OBJS) \
                                       $(OBJDIR)/rel_types.o \
                                       $(OBJDIR)/modules/css/css_analyze.o \
                                       $(OBJDIR)/db2/code_index.o \
                                       $(OBJDIR)/db2/cross_repo_resolver.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

# CSS rendered-oracle storage + evaluation (#4-full slice 2): db2/css_render.o
# + the pure css_render_oracle.o, over the migration-unit + graph spine.
$(TESTPREFIX)/unit-test-css-render: \
                                       $(OBJDIR)/tests/test_css_render.o \
                                       $(OBJDIR)/db2/css_render.o \
                                       $(OBJDIR)/modules/css/css_render_oracle.o \
                                       $(OBJDIR)/db2/css_migration.o \
                                       $(OBJDIR)/db2/css_graph.o \
                                       $(OBJDIR)/db2/kb_runtime_state.o \
                                       $(OBJDIR)/db2/typed_facts.o \
                                       $(TEST_FACT_MUTATION_MIN_OBJS) \
                                       $(OBJDIR)/rel_types.o \
                                       $(OBJDIR)/modules/css/css_analyze.o \
                                       $(OBJDIR)/db2/code_index.o \
                                       $(OBJDIR)/db2/cross_repo_resolver.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-curator-version: \
                                       $(OBJDIR)/tests/test_curator_version.o \
                                       $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_version.o \
                                       $(OBJDIR)/db2/kb_runtime_state.o \
                                       $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                                       $(OBJDIR)/db2/kb_payload.o \
                                       $(OBJDIR)/db2/feature_rows.o \
                                       $(OBJDIR)/kb/kb_mdl.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-curator-invalidate: \
                                       $(OBJDIR)/tests/test_curator_invalidate.o \
                                       $(OBJDIR)/db2/kb_payload.o \
                                       $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                                       $(OBJDIR)/db2/feature_rows.o \
                                       $(OBJDIR)/kb/kb_mdl.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-curator-notify: $(OBJDIR)/tests/test_curator_notify.o $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_notify.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-curator-pipeline-sched: $(OBJDIR)/tests/test_curator_pipeline_sched.o $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_pipeline.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-curator-custom-stages: $(OBJDIR)/tests/test_curator_custom_stages.o $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_custom.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-curator-queue: \
                                       $(OBJDIR)/tests/test_curator_queue.o \
                                       $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_queue.o \
                                       $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_extract.o \
                                       $(OBJDIR)/db2/db2_tenant.o $(OBJDIR)/kb/kb_identity.o \
                                       $(OBJDIR)/kb/kb_memory_facts.o $(OBJDIR)/kb/fact_grounding.o \
                                       $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_llm.o \
                                       $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_sidecar.o \
                                       $(OBJDIR)/kb_curator_provider.o \
                                       $(OBJDIR)/provider_client.o \
                                       $(OBJDIR)/tests/support/mock_agent_http.o \
                                       $(OBJDIR)/db2/typed_facts.o \
                                       $(TEST_FACT_MUTATION_MIN_OBJS) \
                                       $(OBJDIR)/db2/rel_types_store.o \
                                       $(OBJDIR)/db2/fact_recall.o \
                                       $(OBJDIR)/db2/fact_ingest.o \
                                       $(OBJDIR)/db2/fact_lifecycle.o \
                                       $(OBJDIR)/db2/entity_edges.o \
                                       $(OBJDIR)/db2/entity_registry.o \
                                       $(OBJDIR)/db2/ontology_evolution.o \
                                       $(OBJDIR)/db2/memory_query.o \
                                       $(OBJDIR)/db2/memory_row_mapper_pg.o \
                                       $(OBJDIR)/modules/memory/memory_extract_patterns.o \
                                       $(OBJDIR)/modules/memory/memory_fact_gate.o \
                                       $(OBJDIR)/modules/memory/memory_pii_gate.o \
                                       $(OBJDIR)/rel_types.o \
                                       $(OBJDIR)/index.o $(OBJDIR)/cochange.o \
                                       $(OBJDIR)/db2/code_index.o \
                                       $(OBJDIR)/db2/kb_payload.o \
                                       $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                                       $(OBJDIR)/db2/feature_rows.o \
                                       $(OBJDIR)/kb/kb_mdl.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

# Real libpq, NOT the sqlite shim. Every other unit test links
# aimee_pg_sqlite_shim.o, which is why no test has ever executed pgvector SQL:
# operators like <=> do not exist in sqlite, so a shimmed test cannot tell a
# working vector query from a broken one. This one target swaps the shim for
# db_postgres.o so the query runs where halfvec actually lives.
TEST_CORE_OBJS_PG = $(filter-out $(DB2_TEST_BACKEND_OBJ) $(OBJDIR)/db2/db2_test_shim.o,$(TEST_CORE_OBJS)) \
                    $(OBJDIR)/db2/db_postgres.o $(OBJDIR)/db2/entity_edges.o

$(TESTPREFIX)/unit-test-pgvec-neardup: $(OBJDIR)/tests/test_pgvec_neardup.o \
                    $(OBJDIR)/db2/pgvec_transport.o $(OBJDIR)/db2/memory_scope_query.o $(OBJDIR)/db2/memory_vectors.o $(OBJDIR)/db2/kb_vectors.o \
                    $(OBJDIR)/db2/vector_status.o $(OBJDIR)/db2/pgvec_verify.o $(OBJDIR)/db2/pgvec_kb_service.o \
                    $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                    $(TEST_CORE_OBJS_PG)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) $(PQ_LIB)

$(TESTPREFIX)/unit-test-pgvec: $(OBJDIR)/tests/test_pgvec.o \
                    $(OBJDIR)/db2/pgvec_transport.o $(OBJDIR)/db2/memory_scope_query.o $(OBJDIR)/db2/memory_vectors.o $(OBJDIR)/db2/kb_vectors.o \
                    $(OBJDIR)/db2/vector_status.o $(OBJDIR)/db2/pgvec_verify.o $(OBJDIR)/db2/pgvec_kb_service.o \
                    $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                    $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# memory/KB vector upsert dim guard (rejects builtin-384 vs halfvec(1024)/(2560)).
$(TESTPREFIX)/unit-test-memory-embed-dim-guard: $(OBJDIR)/tests/test_memory_embed_dim_guard.o \
                    $(OBJDIR)/db2/pgvec_transport.o $(OBJDIR)/db2/memory_scope_query.o $(OBJDIR)/db2/memory_vectors.o $(OBJDIR)/db2/kb_vectors.o \
                    $(OBJDIR)/db2/vector_status.o $(OBJDIR)/db2/pgvec_verify.o $(OBJDIR)/db2/pgvec_kb_service.o \
                    $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                    $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-memory-embed-http-auth: \
                    $(OBJDIR)/tests/test_memory_embed_http_auth.o \
                    $(OBJDIR)/modules/memory/memory_core_helpers_b.o \
                    $(OBJDIR)/tests/support/mock_agent_http.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# One embedder round trip per batch: the request COUNT is the behaviour under test.
$(TESTPREFIX)/unit-test-memory-embed-batch: \
                    $(OBJDIR)/tests/test_memory_embed_batch.o \
                    $(OBJDIR)/modules/memory/memory_core_helpers_b.o \
                    $(OBJDIR)/tests/support/mock_agent_http.o \
                    $(OBJDIR)/cJSON.o $(OBJDIR)/log.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-rules: $(OBJDIR)/tests/test_rules.o $(DB1_TEST_OBJS) $(DB1_MIGRATED_OBJS) $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o $(OBJDIR)/db2/rules.o $(OBJDIR)/db2/stopwords.o $(OBJDIR)/db2/tool_registry.o $(OBJDIR)/modules/db1/db.o $(OBJDIR)/modules/db1/db_schema.o $(DB2_TEST_BACKEND_OBJ) $(OBJDIR)/db2/db2_test_shim.o \
                       $(CONFIG_CLIENT_TEST_OBJS) $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/util.o $(OBJDIR)/text.o \
                       $(OBJDIR)/platform_random.o $(OBJDIR)/log.o $(PLATFORM_BASIC_OBJS) $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-context-discover: $(OBJDIR)/tests/test_context_discover.o $(OBJDIR)/context_discover.o \
                       $(OBJDIR)/modules/db1/db.o $(OBJDIR)/modules/db1/db_schema.o $(DB2_TEST_BACKEND_OBJ) $(OBJDIR)/db2/db2_test_shim.o $(CONFIG_CLIENT_TEST_OBJS) $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o \
                       $(OBJDIR)/util.o $(OBJDIR)/text.o $(OBJDIR)/platform_random.o $(OBJDIR)/log.o \
                       $(PLATFORM_BASIC_OBJS) $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# mcp_tool_profile.o: cli_mcp_serve trims tools/list prose on the way out, so this
# test links the same object the shipped client does.
$(TESTPREFIX)/unit-test-cli-mcp-serve: $(OBJDIR)/tests/test_cli_mcp_serve.o $(OBJDIR)/cJSON.o \
                     $(OBJDIR)/modules/protocols/mcp/mcp_tool_profile.o \
                     $(OBJDIR)/posix/platform_random.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

# Includes cli_index_bootstrap.c directly so transport/index calls can be
# controlled without linking the full thin-client closure.
$(TESTPREFIX)/unit-test-cli-index-bootstrap: $(OBJDIR)/tests/test_cli_index_bootstrap.o \
                     $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-server-mcp-roundtable: \
                    $(OBJDIR)/tests/test_server_mcp_roundtable.o \
                    $(OBJDIR)/server/server_mcp_roundtable.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

# aimee_client.o resolves the remote-target accessors cli_v1_client_endpoint now
# calls (it synthesizes a tcp: endpoint from a --server/AIMEE_SERVER_URL target).
# platform_path.o backs the delegate --output branch reached by the shared
# production response finisher that this test now calls directly.
# --gc-sections still drops the rest of the transport closure that this
# marshalling/response-contract test never invokes.
# cli_v1_route_for_method now FETCHES the route map from the server, so this
# test needs the HTTP transport (cli_http_request) linked in even though it
# never reaches a server: the lookup calls through it. The route sources
# themselves are #included by the test, so their objects stay out.
$(TESTPREFIX)/unit-test-cli-v1-delegate: $(OBJDIR)/tests/test_cli_v1_delegate.o \
                                  $(OBJDIR)/modules/workspace/workspace_client_diff.o \
                                  $(OBJDIR)/cJSON.o $(OBJDIR)/cli_argspec.o $(OBJDIR)/posix/util.o $(OBJDIR)/aimee_client.o $(OBJDIR)/http_uds_client.o \
                                  $(OBJDIR)/codex_auth.o $(OBJDIR)/posix/platform_path.o \
                                  $(OBJDIR)/posix/cli_client.o $(OBJDIR)/cli_client.o \
                                  $(OBJDIR)/aimee_tls.o \
                                  $(CORE_CONNECTION_LIB)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# The test INCLUDES cli_v1_routes_b.c to reach a static marshaller, so that
# object must not also be linked here or every symbol in it is duplicate.
TEST_TARGETS += $(TESTPREFIX)/unit-test-workspace-add-noscan
$(TESTPREFIX)/unit-test-workspace-add-noscan: $(OBJDIR)/tests/test_workspace_add_noscan.o \
                                  $(OBJDIR)/modules/workspace/workspace_client_diff.o \
                                  $(OBJDIR)/cli_v1_routes.o \
                                  $(OBJDIR)/cli_v1_routes_c.o $(OBJDIR)/cli_v1_routes_d.o $(OBJDIR)/cli_v1_routes_e.o $(OBJDIR)/cli_v1_routes_f.o \
                                  $(OBJDIR)/cli_client.o $(OBJDIR)/posix/cli_client.o \
                                  $(OBJDIR)/aimee_client.o $(OBJDIR)/http_uds_client.o $(OBJDIR)/aimee_tls.o $(OBJDIR)/codex_auth.o \
                                  $(OBJDIR)/aimee_home.o $(OBJDIR)/cJSON.o $(OBJDIR)/cli_argspec.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-cli-v1-poll-deadline: $(OBJDIR)/tests/test_cli_v1_poll_deadline.o \
                                  $(OBJDIR)/cli_v1_routes.o $(OBJDIR)/cli_v1_routes_b.o $(OBJDIR)/cli_argspec.o $(OBJDIR)/modules/workspace/workspace_client_diff.o \
                                  $(OBJDIR)/cli_v1_routes_c.o $(OBJDIR)/cli_v1_routes_d.o $(OBJDIR)/cli_v1_routes_e.o $(OBJDIR)/cli_v1_routes_f.o \
                                  $(OBJDIR)/cli_client.o $(OBJDIR)/posix/cli_client.o \
                                  $(OBJDIR)/aimee_client.o $(OBJDIR)/http_uds_client.o $(OBJDIR)/aimee_tls.o $(OBJDIR)/codex_auth.o \
                                  $(OBJDIR)/aimee_home.o $(OBJDIR)/cJSON.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-cli-v1-subcommands: $(OBJDIR)/tests/test_cli_v1_subcommands.o \
                                  $(OBJDIR)/cli_v1_routes.o $(OBJDIR)/cli_v1_routes_b.o $(OBJDIR)/cli_argspec.o $(OBJDIR)/modules/workspace/workspace_client_diff.o \
                                  $(OBJDIR)/cli_v1_routes_c.o $(OBJDIR)/cli_v1_routes_d.o $(OBJDIR)/cli_v1_routes_e.o $(OBJDIR)/cli_v1_routes_f.o \
                                  $(OBJDIR)/cli_client.o $(OBJDIR)/posix/cli_client.o \
                                  $(OBJDIR)/aimee_client.o $(OBJDIR)/http_uds_client.o $(OBJDIR)/aimee_tls.o $(OBJDIR)/codex_auth.o \
                                  $(OBJDIR)/aimee_home.o $(OBJDIR)/cJSON.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-cli-server-compat: $(OBJDIR)/tests/test_cli_server_compat.o \
                                  $(OBJDIR)/cJSON.o $(OBJDIR)/posix/util.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)


$(TESTPREFIX)/unit-test-guardrails: $(OBJDIR)/tests/test_guardrails.o $(OBJDIR)/tests/support/git_module_fixture.o \
                            $(OBJDIR)/server/obs_bus_adapter.o \
                            $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-cmd-hooks-scope: $(OBJDIR)/tests/test_cmd_hooks_scope.o \
                             $(OBJDIR)/cmd_hooks_scope.o $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-memory: $(CONFIG_ACCESSOR_OBJS) $(OBJDIR)/tests/test_memory.o $(OBJDIR)/kb/kb_bandit.o $(OBJDIR)/kb/kb_bandit_registry.o $(OBJDIR)/db2/bandit.o $(OBJDIR)/db2/demotion.o $(OBJDIR)/modules/memory/memory_core.o $(OBJDIR)/modules/memory/memory_core_crud.o $(OBJDIR)/modules/memory/memory_core_helpers.o $(OBJDIR)/modules/memory/memory_core_helpers_b.o $(OBJDIR)/modules/memory/memory_core_search.o $(OBJDIR)/modules/memory/memory_core_search_b.o $(OBJDIR)/modules/memory/memory_core_search_c.o $(OBJDIR)/modules/memory/memory_core_scope_embed.o $(OBJDIR)/modules/memory/memory_core_tiers.o \
                        $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o $(OBJDIR)/db2/kb_payload.o $(OBJDIR)/db2/kb_service_backend.o $(OBJDIR)/db2/kb_service_backend_ingest.o $(OBJDIR)/db2/memory_lifecycle.o $(OBJDIR)/db2/memory_payload.o $(OBJDIR)/db2/memory_promotion.o $(OBJDIR)/db2/memory_query.o $(OBJDIR)/db2/memory_query_bookkeeping.o $(OBJDIR)/db2/memory_entity_graph.o $(OBJDIR)/db2/memory_score_fields.o $(OBJDIR)/db2/memory_scope_query.o $(OBJDIR)/db2/memory_scenes.o $(OBJDIR)/db2/memory_briefing.o $(OBJDIR)/db2/memory_health.o $(OBJDIR)/db2/memory_row_mapper_pg.o $(OBJDIR)/db2/memory_relations.o $(OBJDIR)/db2/memory_conflicts.o $(OBJDIR)/db2/vector_index_ops.o $(OBJDIR)/db2/code_index_ops.o $(OBJDIR)/db2/rules.o $(OBJDIR)/db2/stopwords.o $(OBJDIR)/db2/tool_registry.o $(OBJDIR)/db2/feedback.o $(OBJDIR)/db2/notes.o $(OBJDIR)/db2/anti_patterns.o $(OBJDIR)/modules/db1/approach_failures.o $(OBJDIR)/approach_store.o $(OBJDIR)/db2/curiosity.o $(OBJDIR)/db2/entity_edges.o $(OBJDIR)/db2/entity_profiles.o $(OBJDIR)/db2/epistemic_directives.o $(OBJDIR)/db2/failed_queries.o $(OBJDIR)/db2/kind_lifecycle.o $(OBJDIR)/db2/calibration.o $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o $(OBJDIR)/db2/pgvec_transport.o $(OBJDIR)/db2/memory_vectors.o $(OBJDIR)/db2/kb_vectors.o $(OBJDIR)/db2/vector_status.o $(OBJDIR)/db2/pgvec_verify.o $(OBJDIR)/db2/pgvec_kb_service.o \
                        $(OBJDIR)/tests/support/mock_agent_http.o \
                        $(OBJDIR)/tests/support/memory_embed_stub.o $(OBJDIR)/tests/support/kb_client_test_stub.o \
                        $(OBJDIR)/posix/memory.o \
                        $(OBJDIR)/modules/memory/memory_logic.o $(OBJDIR)/modules/memory/memory_health.o $(OBJDIR)/db2/fact_lifecycle.o $(TEST_FACT_MUTATION_MIN_OBJS) $(OBJDIR)/modules/memory/memory_conflict.o $(OBJDIR)/modules/memory/memory_context.o $(OBJDIR)/modules/memory/memory_assemble.o \
                        $(OBJDIR)/modules/memory/memory_advanced.o $(OBJDIR)/modules/memory/memory_prospective.o $(OBJDIR)/modules/memory/memory_lifecycle.o $(OBJDIR)/modules/memory/memory_directives.o $(OBJDIR)/modules/memory/memory_maintenance.o $(OBJDIR)/modules/memory/memory_graph.o $(OBJDIR)/modules/memory/memory_graph_fusion.o $(OBJDIR)/modules/memory/memory_scan.o $(OBJDIR)/modules/memory/memory_improve.o $(OBJDIR)/modules/memory/memory_episodes.o $(OBJDIR)/modules/db1/db.o $(OBJDIR)/modules/db1/db_schema.o $(DB2_TEST_BACKEND_OBJ) $(OBJDIR)/db2/db2_test_shim.o $(CONFIG_CLIENT_TEST_OBJS) \
                        $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/modules/vault/runtime_secret.o \
                        $(OBJDIR)/util.o $(OBJDIR)/text.o $(OBJDIR)/platform_random.o \
                        $(OBJDIR)/aimee_home.o \
                        $(OBJDIR)/log.o $(PLATFORM_BASIC_OBJS) \
                        $(OBJDIR)/tests/support/learning_implicit_stub.o $(OBJDIR)/dogfood.o $(OBJDIR)/working_profile.o \
                        $(OBJDIR)/tasks.o $(OBJDIR)/index.o $(OBJDIR)/cochange.o $(OBJDIR)/modules/css/css_analyze.o $(OBJDIR)/db2/css_graph.o $(OBJDIR)/extractors.o \
                        $(OBJDIR)/extractors_extra.o $(OBJDIR)/extractors_new_langs.o $(OBJDIR)/code_treesitter.o \
                        $(OBJDIR)/kb/kb.o $(OBJDIR)/db2/code_index.o $(OBJDIR)/kb/kb_neardup.o $(OBJDIR)/kb/kb_conventions.o $(OBJDIR)/kb/kb_mdl.o \
                        $(OBJDIR)/db2/feature_rows.o \
                        $(OBJDIR)/modules/workspace/workspace.o $(OBJDIR)/session_worktree_key.o $(OBJDIR)/modules/workspace/workspace_manifest.o $(OBJDIR)/util_url.o $(OBJDIR)/report_enrichment.o $(DB1_TEST_OBJS) \
                        $(OBJDIR)/render.o $(OBJDIR)/json_fluent.o $(OBJDIR)/cJSON.o $(DB1_MIGRATED_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-tasks: $(OBJDIR)/tests/test_tasks.o $(OBJDIR)/tasks.o $(OBJDIR)/modules/db1/db.o $(OBJDIR)/modules/db1/db_schema.o $(DB2_TEST_BACKEND_OBJ) $(OBJDIR)/db2/db2_test_shim.o \
                       $(CONFIG_CLIENT_TEST_OBJS) $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/modules/vault/runtime_secret.o $(OBJDIR)/util.o $(OBJDIR)/text.o \
                       $(OBJDIR)/aimee_home.o \
                       $(OBJDIR)/platform_random.o $(OBJDIR)/log.o $(PLATFORM_BASIC_OBJS) \
                       $(OBJDIR)/modules/memory/memory_core.o $(OBJDIR)/modules/memory/memory_core_crud.o $(OBJDIR)/modules/memory/memory_core_helpers.o $(OBJDIR)/modules/memory/memory_core_helpers_b.o $(OBJDIR)/modules/memory/memory_core_search.o $(OBJDIR)/modules/memory/memory_core_search_b.o $(OBJDIR)/modules/memory/memory_core_search_c.o $(OBJDIR)/modules/memory/memory_core_scope_embed.o $(OBJDIR)/modules/memory/memory_core_tiers.o $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o $(OBJDIR)/db2/kb_payload.o $(OBJDIR)/db2/kb_service_backend.o $(OBJDIR)/db2/kb_service_backend_ingest.o $(OBJDIR)/db2/memory_lifecycle.o $(OBJDIR)/db2/memory_payload.o $(OBJDIR)/db2/memory_promotion.o $(OBJDIR)/db2/memory_query.o $(OBJDIR)/db2/memory_query_bookkeeping.o $(OBJDIR)/db2/memory_entity_graph.o $(OBJDIR)/db2/memory_score_fields.o $(OBJDIR)/db2/memory_scope_query.o $(OBJDIR)/db2/memory_scenes.o $(OBJDIR)/db2/memory_briefing.o $(OBJDIR)/db2/memory_health.o $(OBJDIR)/db2/memory_row_mapper_pg.o $(OBJDIR)/db2/memory_relations.o $(OBJDIR)/db2/memory_conflicts.o $(OBJDIR)/db2/vector_index_ops.o $(OBJDIR)/db2/code_index_ops.o $(OBJDIR)/db2/rules.o $(OBJDIR)/db2/tasks.o $(OBJDIR)/db2/tool_registry.o $(OBJDIR)/db2/feedback.o $(OBJDIR)/db2/notes.o $(OBJDIR)/db2/anti_patterns.o $(OBJDIR)/modules/db1/approach_failures.o $(OBJDIR)/approach_store.o $(OBJDIR)/db2/curiosity.o $(OBJDIR)/db2/decision_log.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/db2/pgvec_transport.o $(OBJDIR)/db2/memory_vectors.o $(OBJDIR)/db2/kb_vectors.o $(OBJDIR)/db2/vector_status.o $(OBJDIR)/db2/pgvec_verify.o $(OBJDIR)/db2/pgvec_kb_service.o $(OBJDIR)/tests/support/mock_agent_http.o $(OBJDIR)/tests/support/memory_embed_stub.o $(OBJDIR)/tests/support/kb_client_test_stub.o $(OBJDIR)/posix/memory.o \
                       $(OBJDIR)/modules/memory/memory_logic.o $(OBJDIR)/modules/memory/memory_health.o $(OBJDIR)/db2/fact_lifecycle.o $(TEST_FACT_MUTATION_MIN_OBJS) $(OBJDIR)/modules/memory/memory_conflict.o $(OBJDIR)/modules/memory/memory_context.o $(OBJDIR)/modules/memory/memory_assemble.o \
                       $(OBJDIR)/modules/memory/memory_advanced.o $(OBJDIR)/modules/memory/memory_prospective.o $(OBJDIR)/modules/memory/memory_lifecycle.o $(OBJDIR)/modules/memory/memory_directives.o $(OBJDIR)/modules/memory/memory_maintenance.o $(OBJDIR)/modules/memory/memory_graph.o $(OBJDIR)/modules/memory/memory_graph_fusion.o $(OBJDIR)/modules/memory/memory_scan.o $(OBJDIR)/modules/memory/memory_improve.o $(OBJDIR)/modules/memory/memory_episodes.o $(OBJDIR)/tests/support/learning_implicit_stub.o $(OBJDIR)/dogfood.o $(OBJDIR)/working_profile.o \
                       $(OBJDIR)/index.o $(OBJDIR)/cochange.o $(OBJDIR)/modules/css/css_analyze.o $(OBJDIR)/db2/css_graph.o $(OBJDIR)/extractors.o \
                       $(OBJDIR)/extractors_extra.o $(OBJDIR)/extractors_new_langs.o $(OBJDIR)/code_treesitter.o \
                       $(OBJDIR)/kb/kb_mdl.o $(OBJDIR)/db2/feature_rows.o \
                       $(OBJDIR)/render.o $(OBJDIR)/cJSON.o $(DB1_TEST_OBJS) $(DB1_MIGRATED_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-agent: $(OBJDIR)/tests/test_agent.o $(OBJDIR)/tests/test_agent_caps.o \
                     $(OBJDIR)/db1_store_ready.o \
                      $(OBJDIR)/tests/support/delegate_role_seam_stub.o \
                      $(OBJDIR)/tests/support/role_template_toolset_stub.o \
                      $(OBJDIR)/modules/execution-policy/execution_policy.o \
                      $(OBJDIR)/modules/workflows/tool_egress.o \
                      $(OBJDIR)/tests/test_agent_responses.o \
                      $(OBJDIR)/posix/agent_ir_parse.o $(OBJDIR)/modules/translation/aimee_backend_responses.o \
                      $(OBJDIR)/modules/translation/aimee_backend_anthropic.o $(OBJDIR)/modules/translation/aimee_backend_openai.o \
                      $(OBJDIR)/modules/ir/aimee_ir.o $(OBJDIR)/modules/delegates/aimee_ir_rescue.o \
                      $(OBJDIR)/modules/ir/aimee_ir_metrics.o \
                      $(OBJDIR)/tests/test_agent_delegate_root.o $(OBJDIR)/server/agent_cli_shell.o \
                      $(OBJDIR)/modules/audit/audit_action.o $(OBJDIR)/modules/audit/audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                      $(OBJDIR)/server/tool_call_args.o \
                      $(OBJDIR)/server/session_compact.o $(OBJDIR)/server/rounds_to_resume.o $(OBJDIR)/server/compact_prune.o $(OBJDIR)/modules/delegates/delegate_driver.o \
                      $(OBJDIR)/modules/delegates/delegate_openai.o                      $(OBJDIR)/modules/delegates/delegate_xml_fallback.o $(OBJDIR)/modules/delegates/delegate_role.o \
                      $(OBJDIR)/model_registry.o $(OBJDIR)/models_dev.o \
                      $(OBJDIR)/models_dev_cache.o $(OBJDIR)/payload_rewrite.o \
                      $(OBJDIR)/server/middleware.o $(OBJDIR)/server/liveness.o \
                      $(OBJDIR)/server/cli_session.o $(OBJDIR)/server/agent_policy_intercept.o \
                      $(OBJDIR)/server/model_provider.o $(OBJDIR)/server/openai_profile.o \
                      $(OBJDIR)/server/anthropic_profile.o $(OBJDIR)/server/minimax_profile.o \
                      $(OBJDIR)/server/mistral_profile.o $(OBJDIR)/server/openrouter_profile.o \
                      $(OBJDIR)/server/ollama_profile.o $(OBJDIR)/server/llama_native_profile.o \
                      $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# message_history_repair tests split out of test_agent.c (2000-line limit).
# Mirrors unit-test-agent's link line so message_history_repair (agent_bridge.o,
# pulled via the shared object set) and cJSON resolve; gc-sections drops the rest.
$(TESTPREFIX)/unit-test-agent-repair: $(OBJDIR)/tests/test_agent_repair.o \
                      $(OBJDIR)/server/agent_cli_shell.o \
                      $(OBJDIR)/server/tool_call_args.o \
                      $(OBJDIR)/server/session_compact.o $(OBJDIR)/server/rounds_to_resume.o $(OBJDIR)/server/compact_prune.o $(OBJDIR)/modules/delegates/delegate_driver.o \
                      $(OBJDIR)/modules/delegates/delegate_openai.o                      $(OBJDIR)/modules/delegates/delegate_xml_fallback.o $(OBJDIR)/modules/delegates/delegate_role.o \
                      $(OBJDIR)/model_registry.o $(OBJDIR)/models_dev.o \
                      $(OBJDIR)/models_dev_cache.o $(OBJDIR)/payload_rewrite.o \
                      $(OBJDIR)/server/middleware.o $(OBJDIR)/server/liveness.o \
                      $(OBJDIR)/server/cli_session.o $(OBJDIR)/server/agent_policy_intercept.o \
                      $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# agents.json secret-serialization test split out of test_agent.c (2000-line
# limit). Mirrors unit-test-agent's link line.
$(TESTPREFIX)/unit-test-agent-apikey: $(OBJDIR)/tests/test_agent_apikey.o \
                     $(OBJDIR)/db1_store_ready.o \
                      $(OBJDIR)/tests/support/role_template_toolset_stub.o \
                      $(OBJDIR)/modules/execution-policy/execution_policy.o \
                      $(OBJDIR)/server/agent_cli_shell.o \
                      $(OBJDIR)/modules/audit/audit_action.o $(OBJDIR)/modules/audit/audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                      $(OBJDIR)/server/tool_call_args.o \
                      $(OBJDIR)/server/session_compact.o $(OBJDIR)/server/rounds_to_resume.o $(OBJDIR)/server/compact_prune.o $(OBJDIR)/modules/delegates/delegate_driver.o \
                      $(OBJDIR)/modules/delegates/delegate_openai.o                      $(OBJDIR)/modules/delegates/delegate_xml_fallback.o $(OBJDIR)/modules/delegates/delegate_role.o \
                      $(OBJDIR)/model_registry.o $(OBJDIR)/models_dev.o \
                      $(OBJDIR)/models_dev_cache.o $(OBJDIR)/payload_rewrite.o \
                      $(OBJDIR)/server/middleware.o $(OBJDIR)/server/liveness.o \
                      $(OBJDIR)/server/cli_session.o $(OBJDIR)/server/agent_policy_intercept.o \
                      $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-script-runner: $(OBJDIR)/tests/test_script_runner.o \
                      $(OBJDIR)/server/script_runner.o $(OBJDIR)/server/script_rpc.o $(OBJDIR)/toolset.o \
                      $(OBJDIR)/platform_random.o $(OBJDIR)/posix/platform_random.o \
                      $(CONFIG_CLIENT_TEST_OBJS) $(OBJDIR)/dstr.o $(OBJDIR)/log.o $(OBJDIR)/aimee_home.o \
                      $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-provider-cli-adapter: $(OBJDIR)/tests/test_provider_cli_adapter.o \
                      $(OBJDIR)/tests/support/delegate_child_env_export_stub.o \
                      $(OBJDIR)/tests/support/git_cred_inject_stub.o \
                      $(OBJDIR)/server/provider_cli_adapter.o $(OBJDIR)/server/cli_codex.o \
                      $(OBJDIR)/server/cli_claude.o $(OBJDIR)/server/cli_mistral.o \
                      $(OBJDIR)/server/cli_acp.o $(OBJDIR)/posix/workspace_provider.o \
                      $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-cli-acp: $(OBJDIR)/tests/test_cli_acp.o \
                      $(OBJDIR)/tests/support/delegate_child_env_export_stub.o \
                      $(OBJDIR)/tests/support/git_cred_inject_stub.o \
                      $(OBJDIR)/server/provider_cli_adapter.o $(OBJDIR)/server/cli_codex.o \
                      $(OBJDIR)/server/cli_claude.o $(OBJDIR)/server/cli_mistral.o \
                      $(OBJDIR)/server/cli_acp.o $(OBJDIR)/posix/workspace_provider.o \
                      $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-extractors: $(OBJDIR)/tests/test_extractors.o $(OBJDIR)/extractors.o \
                           $(OBJDIR)/extractors_extra.o $(OBJDIR)/extractors_new_langs.o $(OBJDIR)/code_treesitter.o $(OBJDIR)/index.o $(OBJDIR)/cochange.o $(OBJDIR)/modules/css/css_analyze.o $(OBJDIR)/db2/css_graph.o $(OBJDIR)/modules/db1/db.o $(OBJDIR)/modules/db1/db_schema.o $(DB2_TEST_BACKEND_OBJ) $(OBJDIR)/db2/db2_test_shim.o \
                           $(CONFIG_CLIENT_TEST_OBJS) $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/util.o $(OBJDIR)/text.o \
                           $(OBJDIR)/platform_random.o $(OBJDIR)/log.o $(PLATFORM_BASIC_OBJS) \
                           $(OBJDIR)/modules/memory/memory_core.o $(OBJDIR)/modules/memory/memory_core_crud.o $(OBJDIR)/modules/memory/memory_core_helpers.o $(OBJDIR)/modules/memory/memory_core_helpers_b.o $(OBJDIR)/modules/memory/memory_core_search.o $(OBJDIR)/modules/memory/memory_core_search_b.o $(OBJDIR)/modules/memory/memory_core_search_c.o $(OBJDIR)/modules/memory/memory_core_scope_embed.o $(OBJDIR)/modules/memory/memory_core_tiers.o $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o $(OBJDIR)/db2/kb_payload.o $(OBJDIR)/db2/kb_service_backend.o $(OBJDIR)/db2/kb_service_backend_ingest.o $(OBJDIR)/db2/memory_lifecycle.o $(OBJDIR)/db2/memory_payload.o $(OBJDIR)/db2/memory_promotion.o $(OBJDIR)/db2/memory_query.o $(OBJDIR)/db2/memory_query_bookkeeping.o $(OBJDIR)/db2/memory_entity_graph.o $(OBJDIR)/db2/memory_score_fields.o $(OBJDIR)/db2/memory_scope_query.o $(OBJDIR)/db2/memory_scenes.o $(OBJDIR)/db2/memory_briefing.o $(OBJDIR)/db2/memory_health.o $(OBJDIR)/db2/memory_row_mapper_pg.o $(OBJDIR)/db2/memory_relations.o $(OBJDIR)/db2/memory_conflicts.o $(OBJDIR)/db2/vector_index_ops.o $(OBJDIR)/db2/code_index_ops.o $(OBJDIR)/db2/rules.o $(OBJDIR)/db2/stopwords.o $(OBJDIR)/db2/tool_registry.o $(OBJDIR)/db2/feedback.o $(OBJDIR)/db2/notes.o $(OBJDIR)/db2/anti_patterns.o $(OBJDIR)/modules/db1/approach_failures.o $(OBJDIR)/approach_store.o $(OBJDIR)/db2/curiosity.o $(OBJDIR)/db2/entity_edges.o $(OBJDIR)/db2/entity_profiles.o $(OBJDIR)/db2/epistemic_directives.o $(OBJDIR)/db2/failed_queries.o $(OBJDIR)/db2/kind_lifecycle.o $(OBJDIR)/db2/pgvec_transport.o $(OBJDIR)/db2/memory_vectors.o $(OBJDIR)/db2/kb_vectors.o $(OBJDIR)/db2/vector_status.o $(OBJDIR)/db2/pgvec_verify.o $(OBJDIR)/db2/pgvec_kb_service.o $(OBJDIR)/tests/support/mock_agent_http.o $(OBJDIR)/tests/support/memory_embed_stub.o $(OBJDIR)/tests/support/kb_client_test_stub.o $(OBJDIR)/posix/memory.o \
                           $(OBJDIR)/modules/memory/memory_logic.o $(OBJDIR)/modules/memory/memory_health.o $(OBJDIR)/db2/fact_lifecycle.o $(TEST_FACT_MUTATION_MIN_OBJS) $(OBJDIR)/modules/memory/memory_conflict.o $(OBJDIR)/modules/memory/memory_context.o $(OBJDIR)/modules/memory/memory_assemble.o \
                            $(OBJDIR)/modules/memory/memory_advanced.o $(OBJDIR)/modules/memory/memory_prospective.o $(OBJDIR)/modules/memory/memory_lifecycle.o $(OBJDIR)/modules/memory/memory_directives.o $(OBJDIR)/modules/memory/memory_maintenance.o $(OBJDIR)/modules/memory/memory_graph.o $(OBJDIR)/modules/memory/memory_graph_fusion.o $(OBJDIR)/modules/memory/memory_scan.o $(OBJDIR)/modules/memory/memory_improve.o $(OBJDIR)/modules/memory/memory_episodes.o \
                           $(OBJDIR)/tests/support/learning_implicit_stub.o $(OBJDIR)/dogfood.o $(OBJDIR)/working_profile.o $(OBJDIR)/tasks.o \
                           $(OBJDIR)/kb/kb_mdl.o $(OBJDIR)/db2/feature_rows.o \
                           $(OBJDIR)/render.o $(OBJDIR)/json_fluent.o $(OBJDIR)/cJSON.o $(DB1_TEST_OBJS) $(DB1_MIGRATED_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# --- New tests ---

# CSS analyzer (WP-A) — pure leaf, depends only on css_analyze.o + libc.
$(TESTPREFIX)/unit-test-css-analyze: $(OBJDIR)/tests/test_css_analyze.o $(OBJDIR)/modules/css/css_analyze.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# CSS interim static oracle (WP-E) — leaf: css_oracle.o + css_analyze.o + libc.
$(TESTPREFIX)/unit-test-css-oracle: $(OBJDIR)/tests/test_css_oracle.o $(OBJDIR)/modules/css/css_oracle.o $(OBJDIR)/modules/css/css_analyze.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# CSS rendered computed-style oracle core (#4-full) — leaf: css_render_oracle.o + cJSON + libc.
$(TESTPREFIX)/unit-test-css-render-oracle: $(OBJDIR)/tests/test_css_render_oracle.o $(OBJDIR)/modules/css/css_render_oracle.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# CSS command-driven render backend (#4-full slice 3) — adapter over platform_exec_pipe.
$(TESTPREFIX)/unit-test-css-render-cmd: $(OBJDIR)/tests/test_css_render_cmd.o \
                                       $(OBJDIR)/modules/css/css_render_cmd.o \
                                       $(OBJDIR)/modules/css/css_render_oracle.o \
                                       $(OBJDIR)/log.o \
                                       $(OBJDIR)/posix/platform_process.o \
                                       $(OBJDIR)/linux/platform_process.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-text: $(OBJDIR)/tests/test_text.o $(OBJDIR)/util.o $(OBJDIR)/text.o \
                     $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-ingress-preinject: $(OBJDIR)/tests/test_ingress_preinject.o \
                     $(OBJDIR)/server/ingress_preinject.o $(OBJDIR)/server/request_context.o \
                     $(OBJDIR)/log.o $(OBJDIR)/cJSON.o $(OBJDIR)/dstr.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-code-span: $(OBJDIR)/tests/test_code_span.o \
                     $(OBJDIR)/server/code_span.o $(OBJDIR)/kb/kb_doc_hash.o \
                     $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-code-match: $(OBJDIR)/tests/test_code_match.o $(OBJDIR)/code_match.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-gw-stage-memory: $(OBJDIR)/tests/test_gw_stage_memory.o \
                     $(OBJDIR)/modules/memory/gw_stage_memory.o $(OBJDIR)/pipeline/gw_stage_registry.o $(OBJDIR)/server/ingress_preinject.o \
                     $(OBJDIR)/server/request_context.o $(OBJDIR)/log.o \
                     $(OBJDIR)/modules/ir/aimee_ir.o $(OBJDIR)/modules/ir/aimee_ir_session.o \
                     $(OBJDIR)/cJSON.o $(OBJDIR)/dstr.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-codex-auth: $(OBJDIR)/tests/test_codex_auth.o \
                     $(OBJDIR)/codex_auth.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-attention-guard: $(OBJDIR)/tests/test_attention_guard.o \
                     $(OBJDIR)/cli_attention_guard.o $(OBJDIR)/client_config.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm

# THE worktree/branch key. Pure; links only its own TU.
$(TESTPREFIX)/unit-test-session-worktree-key: $(OBJDIR)/tests/test_session_worktree_key.o \
                     $(OBJDIR)/session_worktree_key.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Exercises real `git` against throwaway repos: the bug this guards (a session
# inheriting the shared checkout's branch) is invisible to a mocked git.
$(TESTPREFIX)/unit-test-client-session-worktree: $(OBJDIR)/tests/test_client_session_worktree.o \
                     $(OBJDIR)/client_session_worktree.o $(OBJDIR)/cli_attention_guard.o \
                     $(OBJDIR)/client_config.o $(OBJDIR)/session_worktree_key.o $(OBJDIR)/aimee_home.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm

$(TESTPREFIX)/unit-test-code-audit: $(OBJDIR)/tests/test_code_audit.o \
                     $(OBJDIR)/cli_code_audit.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-code-audit-graph: $(OBJDIR)/tests/test_code_audit_graph.o \
                     $(OBJDIR)/code_audit_graph.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-cochange: $(OBJDIR)/tests/test_cochange.o \
                     $(OBJDIR)/cochange.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-db2-code-audit: $(OBJDIR)/tests/test_db2_code_audit.o \
                     $(OBJDIR)/db2/code_audit.o $(OBJDIR)/db2/entity_nodes.o \
                     $(OBJDIR)/code_audit_graph.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-server-memory-benchmark: \
                     $(OBJDIR)/tests/test_server_memory_benchmark.o \
                     $(OBJDIR)/server/server_memory_benchmark.o \
                     $(OBJDIR)/tests/module_handlers/benchmarks.o \
                     $(OBJDIR)/json_fluent.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm

$(TESTPREFIX)/unit-test-delegate-sandbox-image: $(OBJDIR)/tests/test_delegate_sandbox_image.o \
                      $(OBJDIR)/modules/delegates/delegate_sandbox_image.o $(OBJDIR)/modules/sandbox/sandbox_learned.o $(OBJDIR)/module_json_call.o $(OBJDIR)/modules/delegates/delegate_launch_args.o \
                      $(OBJDIR)/modules/delegates/delegate_launch_args.o \
                      $(OBJDIR)/modules/guardrails/guardrails_tdd.o \
                      $(OBJDIR)/harness_memory_common.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-roundtable-preset: $(OBJDIR)/tests/test_roundtable_preset.o $(OBJDIR)/modules/roundtable/roundtable_preset.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-roundtable-seat-resolve: $(OBJDIR)/tests/test_roundtable_seat_resolve.o \
                      $(OBJDIR)/modules/roundtable/roundtable_seat_resolve.o \
                      $(OBJDIR)/modules/routing/agent_config.o $(OBJDIR)/modules/vault/agent_credentials.o $(OBJDIR)/modules/routing/agent_registry.o $(OBJDIR)/modules/routing/routing.o \
                      $(OBJDIR)/tests/support/vault_service_stub.o \
                      $(OBJDIR)/tests/support/oauth_tokens_stub.o \
                      $(OBJDIR)/tests/support/provider_cli_adapter_stub.o \
                      $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-audit-worm: $(OBJDIR)/tests/test_audit_worm.o $(OBJDIR)/modules/audit/audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o \
                      $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Pure-primitive drift/golden test for the shared WORM chain (no storage engine):
# the single source of truth for the cross-engine hash contract.
$(TESTPREFIX)/unit-test-audit-worm-chain: $(OBJDIR)/tests/test_audit_worm_chain.o $(OBJDIR)/modules/audit/audit_worm_chain.o \
                      $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-msg-session-key: $(OBJDIR)/tests/test_msg_session_key.o \
                     $(OBJDIR)/server/msg_session_key.o \
                     $(OBJDIR)/harness_memory_common.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lpthread

$(TESTPREFIX)/unit-test-gateway-mutate: $(OBJDIR)/tests/test_gateway_mutate.o \
                     $(OBJDIR)/modules/economizer/gateway_mutate.o $(OBJDIR)/server/agent_bridge.o \
                     $(OBJDIR)/server/session_compact.o $(OBJDIR)/server/rounds_to_resume.o $(OBJDIR)/server/tool_call_args.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm -lpthread

$(TESTPREFIX)/unit-test-gateway-mutate-wire: $(OBJDIR)/tests/test_gateway_mutate_wire.o \
                     $(OBJDIR)/modules/economizer/gateway_mutate_wire.o $(OBJDIR)/modules/economizer/economizer_module_client.o $(OBJDIR)/module_json_call.o $(OBJDIR)/modules/economizer/gateway_mutate.o \
                     $(BUS_TEST_OBJS) \
                     $(OBJDIR)/server/msg_session_key.o \
                     $(OBJDIR)/server/agent_bridge.o $(OBJDIR)/server/session_compact.o $(OBJDIR)/server/rounds_to_resume.o \
                     $(OBJDIR)/server/tool_call_args.o $(OBJDIR)/server/token_tracker.o \
                     $(OBJDIR)/harness_memory_common.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm -lpthread

# Configurable tool-output cap: tests the header-inline clamp resolver
# (agent_tool_output_cap_clamp). No extra objects — the clamp is pure.
$(TESTPREFIX)/unit-test-tool-output-cap: $(OBJDIR)/tests/test_tool_output_cap.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Learning detector replay (citation heuristics). Runs the real
# dogfood_classify_next_turn() over implicit-signal fixtures and emits a
# predictions jsonl for benchmarks/learning/learning_replay.py — binds the
# learning-router rollout metric to the real build instead of a Python re-impl.
$(TESTPREFIX)/learning-implicit-replay: $(OBJDIR)/tests/learning_implicit_replay.o \
		$(OBJDIR)/dogfood.o $(OBJDIR)/cJSON.o $(OBJDIR)/util.o $(OBJDIR)/text.o $(OBJDIR)/dstr.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# One-command reproduce of the citation-detector grade (PASS/FAIL via exit code).
.PHONY: learning-citation-eval
learning-citation-eval: $(TESTPREFIX)/learning-implicit-replay
	$(TESTPREFIX)/learning-implicit-replay ../benchmarks/learning/implicit-signal/labelled.jsonl \
		> $(OBJDIR)/learning_citation_preds.jsonl
	python3 ../benchmarks/learning/learning_replay.py \
		../benchmarks/learning/implicit-signal/labelled.jsonl \
		--heuristics citation_then_repair,citation_then_continuation \
		--predictions $(OBJDIR)/learning_citation_preds.jsonl

$(TESTPREFIX)/unit-test-feedback: $(OBJDIR)/tests/test_feedback.o $(TEST_DATA_OBJS_MOCK) \
	$(OBJDIR)/modules/learning/learning_signal_policy.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-render: $(OBJDIR)/tests/test_render.o $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-markdown: $(OBJDIR)/tests/test_markdown.o $(OBJDIR)/markdown.o \
                                   $(OBJDIR)/cJSON.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-index: $(OBJDIR)/tests/test_index.o $(TEST_DATA_OBJS_MOCK) \
                               $(OBJDIR)/db2/canonical_index.o \
                               $(OBJDIR)/db2/cross_repo_resolver.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-memory-advanced: $(OBJDIR)/tests/test_memory_advanced.o $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-feedback-shadow: $(OBJDIR)/tests/test_feedback_shadow.o \
    $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm

$(TESTPREFIX)/unit-test-graph-fusion: $(OBJDIR)/tests/test_graph_fusion.o \
    $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm

$(TESTPREFIX)/unit-test-code-vectors: $(OBJDIR)/tests/test_code_vectors.o \
    $(OBJDIR)/kb/kb_service_code_embed.o $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-graph-scoring: $(OBJDIR)/tests/test_graph_scoring.o \
    $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm

$(TESTPREFIX)/unit-test-code-projection: $(OBJDIR)/tests/test_code_projection.o \
    $(OBJDIR)/db2/code_projection.o $(OBJDIR)/db2/entity_nodes.o $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-entity-nodes: $(OBJDIR)/tests/test_entity_nodes.o \
    $(OBJDIR)/db2/entity_nodes.o $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_postgres.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-memory-health: $(OBJDIR)/tests/test_memory_health.o $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-memory-ranker-boundary: $(OBJDIR)/tests/test_memory_ranker_boundary.o $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm

$(TESTPREFIX)/unit-test-memory-lanes: $(OBJDIR)/tests/test_memory_lanes.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-workspace: $(OBJDIR)/tests/test_workspace.o \
                          $(OBJDIR)/worktree_gc.o \
                          $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-primary-session-adapter: $(OBJDIR)/tests/test_primary_session_adapter.o \
                               $(OBJDIR)/server/primary_session_adapter.o $(OBJDIR)/server/agent_adapter.o \
                               $(OBJDIR)/server/ingress_preinject.o \
                               $(OBJDIR)/server/session_compact.o $(OBJDIR)/server/rounds_to_resume.o $(OBJDIR)/server/compact_prune.o $(OBJDIR)/server/agent_bridge.o $(OBJDIR)/server/anthropic_shape.o $(OBJDIR)/server/tool_call_args.o \
                               $(OBJDIR)/server/agent_request_shaping.o \
                               $(OBJDIR)/server/context_engine.o \
                               $(OBJDIR)/tests/support/mock_agent_http.o \
                               $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/primary_sessions.o $(OBJDIR)/primary_session_release.o \
                               $(OBJDIR)/model_registry.o $(OBJDIR)/models_dev.o $(OBJDIR)/models_dev_cache.o \
                               $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-session-search-tool: $(OBJDIR)/tests/test_session_search_tool.o \
                               $(OBJDIR)/server/session_search_tool.o \
                               $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/primary_sessions.o $(OBJDIR)/primary_session_release.o \
                               $(OBJDIR)/modules/db1/server_sessions.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-working-memory: $(OBJDIR)/tests/test_working_memory.o \
                               $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o $(OBJDIR)/modules/db1/db_schema.o $(OBJDIR)/modules/db1/wm.o \
                               $(OBJDIR)/util.o $(OBJDIR)/platform_random.o \
                               $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Mock-pattern demonstrator: exercises the same wm.h contract using the
# in-memory implementation. Same test source, different backing.
$(TESTPREFIX)/unit-test-working-memory-mock: $(OBJDIR)/tests/test_working_memory.o \
                               $(OBJDIR)/tests/support/db1_init_mock.o $(OBJDIR)/tests/support/wm_mock.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-local-resolution: $(OBJDIR)/tests/test_local_resolution.o \
                               $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o $(OBJDIR)/modules/db1/project_clones.o \
                               $(OBJDIR)/modules/db1/tool_local_availability.o \
                               $(OBJDIR)/modules/db1/local_operator.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-cognify-jobs: $(OBJDIR)/tests/test_cognify_jobs.o \
                               $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o $(OBJDIR)/modules/db1/cognify_jobs.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-extractors-extra: $(OBJDIR)/tests/test_extractors_extra.o $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-compute-pool: $(OBJDIR)/tests/test_compute_pool.o $(OBJDIR)/server/compute_pool.o $(OBJDIR)/log.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

# The bounds every pooled connection carries. Built with postgres disabled so the
# policy is exercised without a backend.
$(OBJDIR)/tests/test_db2_conn_bounds.o: C_FLAGS += -Idb2
# libpq is linked so the suite can assert against PQconninfoParse — the real
# parser is the only thing that distinguishes "the bounds appear in the string"
# from "the bounds are real options", which is exactly the distinction the URI bug
# slipped through. The tests still need no running backend.
$(TESTPREFIX)/unit-test-db2-conn-bounds: $(OBJDIR)/tests/test_db2_conn_bounds.o $(OBJDIR)/db2/db_postgres.o $(OBJDIR)/log.o
	$(TESTLINK_MIN) -o $@ $^ $(PQ_LIB) -lpthread $(EXTRA_L_FLAGS)

# aimee_pg_open's own contract. The test defines the libpq entry points that
# function calls; a definition in an object file wins over the same symbol in a
# shared library, so these land on the fakes while the rest of libpq stays real.
# That is what makes "SET statement_timeout fails" reachable — a live backend
# cannot be asked to reject it on demand. LTO is left on (the rest of the build
# uses it, and libpq is a shared library with no IR, so nothing can be inlined
# across the seam regardless).
$(OBJDIR)/tests/test_db2_conn_open.o: C_FLAGS += -Idb2
$(TESTPREFIX)/unit-test-db2-conn-open: $(OBJDIR)/tests/test_db2_conn_open.o $(OBJDIR)/db2/db_postgres.o $(OBJDIR)/log.o
	$(TESTLINK_MIN) -o $@ $^ $(PQ_LIB) -lpthread $(EXTRA_L_FLAGS)

$(TESTPREFIX)/unit-test-db2-pool: $(OBJDIR)/tests/test_db2_pool.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/log.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-server-session-pools: $(OBJDIR)/tests/test_server_session_pools.o \
	                               $(OBJDIR)/server/server_session_pools.o $(OBJDIR)/server/compute_pool.o \
	                               $(OBJDIR)/log.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-presence: $(OBJDIR)/tests/test_presence.o \
	                               $(OBJDIR)/server/presence.o $(OBJDIR)/delivery_target.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-turn-registry: $(OBJDIR)/tests/test_turn_registry.o \
	                               $(OBJDIR)/server/turn_registry.o $(OBJDIR)/tests/support/log_stub.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-cli-launch: $(OBJDIR)/tests/test_cli_launch.o $(OBJDIR)/cli_launch.o \
                            $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-cli-provider: $(OBJDIR)/tests/test_cli_provider.o $(OBJDIR)/posix/cli_main.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-context-assembly: $(OBJDIR)/tests/test_context_assembly.o \
                                 $(OBJDIR)/tests/support/delegate_role_policy_stub.o \
                                 $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-workspace-memory: $(OBJDIR)/tests/test_workspace_memory.o $(TEST_DATA_OBJS_MOCK) \
                                 $(OBJDIR)/modules/workspace/workspace.o $(OBJDIR)/session_worktree_key.o $(OBJDIR)/dashboard.o $(OBJDIR)/dashboard_kb.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-dashboard: $(OBJDIR)/tests/test_dashboard.o \
                          $(OBJDIR)/dashboard.o $(OBJDIR)/dashboard_kb.o $(OBJDIR)/server/dashboard_server.o $(OBJDIR)/modules/kb_client/kb_client.o $(OBJDIR)/modules/kb_client/kb_client_cache.o $(OBJDIR)/modules/kb_client/kb_client_index.o $(OBJDIR)/code_collect.o $(OBJDIR)/modules/kb_client/kb_client_memory.o $(OBJDIR)/modules/kb_client/kb_client_memory_audit.o $(OBJDIR)/modules/kb_client/kb_client_memory_mutations.o $(OBJDIR)/modules/kb_client/kb_client_agent.o $(OBJDIR)/modules/kb_client/kb_client_dashboard.o $(OBJDIR)/modules/kb_client/kb_client_tasks.o $(OBJDIR)/modules/kb_client/kb_client_data.o \
                          $(OBJDIR)/cli_client.o $(OBJDIR)/cli_v1_routes.o $(OBJDIR)/cli_v1_routes_b.o $(OBJDIR)/cli_argspec.o $(OBJDIR)/modules/workspace/workspace_client_diff.o $(OBJDIR)/cli_v1_routes_c.o $(OBJDIR)/cli_v1_routes_d.o $(OBJDIR)/aimee_client.o $(OBJDIR)/http_uds_client.o $(OBJDIR)/aimee_tls.o $(OBJDIR)/codex_auth.o $(OBJDIR)/cli_v1_routes_e.o $(OBJDIR)/cli_v1_routes_f.o $(OBJDIR)/posix/cli_client.o $(OBJDIR)/aimee_tls.o $(OBJDIR)/codex_auth.o $(DB1_TEST_OBJS) $(DB1_MIGRATED_OBJS) \
                          $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o $(OBJDIR)/db2/decision_log.o $(OBJDIR)/db2/kb_audit_worm.o \
                          $(CONFIG_CLIENT_TEST_OBJS) \
                          $(OBJDIR)/yaml.o $(OBJDIR)/modules/db1/db.o $(OBJDIR)/modules/db1/db_schema.o $(DB2_TEST_BACKEND_OBJ) $(OBJDIR)/db2/db2_test_shim.o \
                          $(OBJDIR)/util.o $(OBJDIR)/text.o $(OBJDIR)/dstr.o \
                          $(OBJDIR)/modules/vault/runtime_secret.o \
                          $(OBJDIR)/tests/support/mock_agent_http.o \
                          $(OBJDIR)/aimee_home.o $(OBJDIR)/shared/kb_paths.o \
                          $(OBJDIR)/platform_random.o $(OBJDIR)/log.o $(OBJDIR)/cJSON.o \
                          $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-aimee-home: $(OBJDIR)/tests/test_aimee_home.o $(OBJDIR)/aimee_home.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-cli-profile: $(OBJDIR)/tests/test_cli_profile.o $(OBJDIR)/cli_profile.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-cmd-profile: $(OBJDIR)/tests/test_cmd_profile.o $(OBJDIR)/cmd_profile.o \
                            $(OBJDIR)/client_config.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-log: $(OBJDIR)/tests/test_log.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-acp-server: $(OBJDIR)/tests/test_acp_server.o \
                           $(OBJDIR)/modules/protocols/acp/acp_server.o \
                           $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

# vault_provider_has_credential only calls vault_service_list, which the test
# stubs, so agent_credentials.o is the sole real object needed here.
$(TESTPREFIX)/unit-test-vault-provider-credential: $(OBJDIR)/tests/test_vault_provider_credential.o \
                      $(OBJDIR)/modules/vault/agent_credentials.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-server-dispatch: $(OBJDIR)/tests/test_server_dispatch.o $(OBJDIR)/server/server.o $(OBJDIR)/server/server_error_kind.o $(OBJDIR)/server/server_seed_config.o $(OBJDIR)/server/server_api_status.o $(OBJDIR)/server_provider.o $(OBJDIR)/server/provider_settable.o $(OBJDIR)/server/agent_adapter.o $(OBJDIR)/server_insights.o $(OBJDIR)/server_eval.o \
	$(OBJDIR)/server/s2_native_gate_hook.o $(OBJDIR)/modules/workflows/wfe_native_gate.o $(OBJDIR)/modules/workflows/wfe_externalization.o $(OBJDIR)/modules/workflows/tool_egress.o \
	$(OBJDIR)/modules/db1/wfe_binding.o $(OBJDIR)/modules/db1/wfe_store.o $(OBJDIR)/modules/workflows/wfe_enforce.o \
	$(OBJDIR)/wfe_store_lists.o \
                      $(OBJDIR)/harness_memory_common.o $(OBJDIR)/modules/delegates/delegate_sandbox_image.o $(OBJDIR)/modules/sandbox/sandbox_learned.o $(OBJDIR)/module_json_call.o $(OBJDIR)/modules/delegates/delegate_launch_args.o \
                      $(OBJDIR)/modules/memory/memory_redirect.o $(OBJDIR)/harness_memory_scope.o $(OBJDIR)/harness_memory_audit.o \
                      $(OBJDIR)/tests/support/delegate_child_env_export_stub.o \
	                                $(OBJDIR)/server/server_config.o \
	                                $(OBJDIR)/tests/support/skill_jobs_stub.o \
	                                $(OBJDIR)/server/server_hooks.o $(OBJDIR)/server/server_http.o $(OBJDIR)/server/server_http_surfaces.o $(OBJDIR)/command_registry.o $(OBJDIR)/server/server_bearer_auth.o $(OBJDIR)/server/server_http_management.o $(OBJDIR)/server/server_http_routes.o $(OBJDIR)/server/server_http_mgmt_read_routes.o $(OBJDIR)/server/shadow_mirror.o $(OBJDIR)/server/server_http_routes_git.o $(OBJDIR)/server/server_dev_submit.o $(OBJDIR)/server/server_ci_route.o $(OBJDIR)/server/server_http_config_routes.o $(OBJDIR)/server/cli_command_defs.o $(OBJDIR)/server/cli_dispatch_defs.o $(OBJDIR)/server/cli_marshal_defs.o $(OBJDIR)/server/cli_argspec_defs.o $(OBJDIR)/server/server_http_conn_worker.o $(OBJDIR)/server/server_http_response.o $(OBJDIR)/server/server_http_sse.o $(OBJDIR)/tests/support/git_route_stub.o $(OBJDIR)/server/server_http_reqctx.o $(OBJDIR)/server/server_http_identity.o $(OBJDIR)/server/server_http_authz.o $(OBJDIR)/modules/vault/vault_principal.o \
	                                $(OBJDIR)/tests/support/workflow_api_stub.o \
	                                $(OBJDIR)/tests/support/vault_handlers_stub.o \
	                                $(OBJDIR)/modules/vault/runtime_secret.o \
	                                $(CONFIG_CLIENT_TEST_OBJS) \
	                                $(OBJDIR)/tests/support/toolset_stub.o \
	                                $(OBJDIR)/cJSON.o $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
	                                $(OBJDIR)/modules/db1/model_catalog.o $(OBJDIR)/model_catalog_release.o \
	                                $(OBJDIR)/cmd_init.o \
	                                $(OBJDIR)/server/server_trigger.o $(OBJDIR)/tests/support/trigger_proposals_stub.o $(OBJDIR)/modules/db1/db1_trigger.o \
	                                $(OBJDIR)/modules/db1/pipelines.o $(OBJDIR)/modules/db1/token_audit.o \
	                                $(OBJDIR)/modules/delegates/delegate_backend.o 	                                $(OBJDIR)/modules/delegates/delegate_backend_docker.o \
	                                $(OBJDIR)/server/model_provider.o $(OBJDIR)/server/openai_profile.o \
	                                $(OBJDIR)/server/anthropic_profile.o	                                $(OBJDIR)/server/openrouter_profile.o $(OBJDIR)/server/ollama_profile.o \
	                                $(OBJDIR)/server/llama_native_profile.o $(OBJDIR)/server/mistral_profile.o \
	                                $(OBJDIR)/server/minimax_profile.o \
	                                $(OBJDIR)/modules/delegates/delegate_credentials.o $(OBJDIR)/model_registry.o \
	                                $(OBJDIR)/models_dev.o $(OBJDIR)/models_dev_cache.o \
	                                $(OBJDIR)/aimee_home.o \
	                                $(OBJDIR)/tests/support/mock_agent_http.o \
	                                $(OBJDIR)/posix/platform_path.o $(OBJDIR)/posix/platform_random.o \
	                                $(OBJDIR)/platform_random.o \
	                                $(OBJDIR)/linux/secret_store.o $(OBJDIR)/posix/util.o \
	                                $(OBJDIR)/json_fluent.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-server-error-kind: $(OBJDIR)/tests/test_server_error_kind.o \
                     $(OBJDIR)/server/server_error_kind.o \
                     $(OBJDIR)/tests/module_handlers/runtime_web.o \
                     $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-client-index: $(OBJDIR)/tests/test_kb_client_index.o \
	                                 $(OBJDIR)/modules/kb_client/kb_client_index_parse.o \
	                                 $(OBJDIR)/json_fluent.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-client-index-remote: $(OBJDIR)/tests/test_kb_client_index_remote.o \
	                                 $(OBJDIR)/modules/kb_client/kb_client_index.o $(OBJDIR)/code_collect.o \
	                                 $(OBJDIR)/modules/kb_client/kb_client_index_parse.o \
	                                 $(OBJDIR)/json_fluent.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-index-investigate-status: $(OBJDIR)/tests/test_index_investigate_status.o \
	                                  $(OBJDIR)/server/server_state_index.o \
	                                  $(OBJDIR)/json_fluent.o $(OBJDIR)/cJSON.o \
	                                  $(OBJDIR)/tests/support/log_stub.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-client-docs: $(OBJDIR)/tests/test_kb_client_docs.o \
	                                 $(OBJDIR)/modules/kb_client/kb_client_docs.o \
	                                 $(OBJDIR)/kb/kb_doc_hash.o \
	                                 $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-client-search: $(OBJDIR)/tests/test_kb_client_search.o \
	                                  $(OBJDIR)/modules/kb_client/kb_client.o \
	                                  $(OBJDIR)/modules/kb_client/kb_client_cache.o \
	                                  $(OBJDIR)/modules/kb_client/kb_client_code_graph.o \
	                                  $(OBJDIR)/modules/kb_client/kb_client_index.o $(OBJDIR)/code_collect.o \
	                                  $(OBJDIR)/modules/kb_client/kb_client_index_parse.o \
	                                  $(OBJDIR)/cli_client.o $(OBJDIR)/cli_v1_routes.o $(OBJDIR)/cli_v1_routes_b.o $(OBJDIR)/cli_argspec.o $(OBJDIR)/modules/workspace/workspace_client_diff.o $(OBJDIR)/cli_v1_routes_c.o $(OBJDIR)/cli_v1_routes_d.o $(OBJDIR)/aimee_client.o $(OBJDIR)/http_uds_client.o $(OBJDIR)/aimee_tls.o $(OBJDIR)/codex_auth.o $(OBJDIR)/cli_v1_routes_e.o $(OBJDIR)/cli_v1_routes_f.o $(OBJDIR)/posix/cli_client.o $(OBJDIR)/aimee_tls.o $(OBJDIR)/codex_auth.o \
	                                  $(OBJDIR)/tests/support/mock_agent_http.o \
	                                  $(OBJDIR)/aimee_home.o $(OBJDIR)/shared/kb_paths.o $(OBJDIR)/cJSON.o \
	                                  $(PLATFORM_BASIC_OBJS) $(OBJDIR)/tests/support/log_stub.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-client-memory: $(OBJDIR)/tests/test_kb_client_memory.o \
	                                  $(OBJDIR)/modules/kb_client/kb_client.o \
	                                  $(OBJDIR)/modules/kb_client/kb_client_cache.o \
	                                  $(OBJDIR)/modules/kb_client/kb_client_memory.o $(OBJDIR)/modules/kb_client/kb_client_memory_audit.o $(OBJDIR)/modules/kb_client/kb_client_memory_mutations.o \
	                                  $(OBJDIR)/modules/kb_client/kb_client_index.o $(OBJDIR)/code_collect.o \
	                                  $(OBJDIR)/modules/kb_client/kb_client_index_parse.o \
	                                  $(OBJDIR)/cli_client.o $(OBJDIR)/cli_v1_routes.o $(OBJDIR)/cli_v1_routes_b.o $(OBJDIR)/cli_argspec.o $(OBJDIR)/modules/workspace/workspace_client_diff.o $(OBJDIR)/cli_v1_routes_c.o $(OBJDIR)/cli_v1_routes_d.o $(OBJDIR)/aimee_client.o $(OBJDIR)/http_uds_client.o $(OBJDIR)/aimee_tls.o $(OBJDIR)/codex_auth.o $(OBJDIR)/cli_v1_routes_e.o $(OBJDIR)/cli_v1_routes_f.o $(OBJDIR)/posix/cli_client.o $(OBJDIR)/aimee_tls.o $(OBJDIR)/codex_auth.o \
	                                  $(OBJDIR)/tests/support/mock_agent_http.o \
	                                  $(OBJDIR)/aimee_home.o $(OBJDIR)/shared/kb_paths.o $(OBJDIR)/cJSON.o \
	                                  $(PLATFORM_BASIC_OBJS) $(OBJDIR)/tests/support/log_stub.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-server-compute: $(OBJDIR)/tests/test_server_compute.o $(OBJDIR)/tests/delegate_permissions_stub.o $(OBJDIR)/modules/db1/db.o $(OBJDIR)/modules/db1/db_schema.o $(DB2_TEST_BACKEND_OBJ) $(OBJDIR)/db2/db2_test_shim.o \
                               $(OBJDIR)/tests/support/delegate_role_policy_stub.o \
                               $(OBJDIR)/tests/support/toolset_stub.o \
                               $(OBJDIR)/tests/support/agent_source_authority_stub.o \
                               $(OBJDIR)/tests/support/provider_cli_adapter_stub.o \
                               $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o $(OBJDIR)/modules/db1/delegations.o $(OBJDIR)/modules/db1/agent_jobs.o $(OBJDIR)/agent_job_release.o $(OBJDIR)/modules/db1/session_paths.o $(OBJDIR)/modules/db1/cost_fold.o $(OBJDIR)/modules/db1/token_audit.o $(OBJDIR)/modules/delegates/delegate_credentials.o $(OBJDIR)/modules/delegates/delegate_credential_retry.o $(OBJDIR)/modules/db1/delegate_learning.o $(OBJDIR)/delegate_exit_classify.o $(OBJDIR)/modules/db1/interaction_events.o \
                               $(OBJDIR)/modules/db1/execution_plans.o $(OBJDIR)/modules/db1/coord_jobs.o $(OBJDIR)/interaction_event_names.o \
		                               $(OBJDIR)/modules/delegates/delegate_launch.o $(OBJDIR)/modules/delegates/delegate_source_authority.o $(OBJDIR)/modules/delegates/delegate_economics.o $(OBJDIR)/server/server_coord_dispatcher.o \
		                               $(OBJDIR)/modules/delegates/gw_orch_delegates.o $(OBJDIR)/pipeline/gw_orchestration_seam.o \
		                               $(OBJDIR)/modules/delegates/delegate_routing.o $(OBJDIR)/modules/delegates/delegate_launch_args.o \
		                               $(OBJDIR)/model_registry.o \
		                               $(OBJDIR)/models_dev.o $(OBJDIR)/models_dev_cache.o \
		                               $(OBJDIR)/server/server_delegate_status.o \
		                               $(OBJDIR)/server/provider_catalog.o $(OBJDIR)/modules/delegates/delegate_prompt.o $(OBJDIR)/modules/delegates/delegate_ephemeral_ws.o $(OBJDIR)/modules/delegates/delegate_run_phases.o $(OBJDIR)/modules/delegates/delegate_checkout.o $(OBJDIR)/server/liveness.o $(OBJDIR)/modules/delegates/delegate_launch_args.o \
                               $(CONFIG_CLIENT_TEST_OBJS) $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/util.o $(OBJDIR)/text.o \
                               $(OBJDIR)/aimee_home.o \
                               $(OBJDIR)/model_registry.o $(OBJDIR)/models_dev.o $(OBJDIR)/models_dev_cache.o \
                               $(OBJDIR)/platform_random.o $(OBJDIR)/log.o $(PLATFORM_BASIC_OBJS) $(OBJDIR)/cJSON.o $(OBJDIR)/server/presence.o $(OBJDIR)/server/turn_registry.o $(OBJDIR)/tests/support/agent_cancel_stub.o $(OBJDIR)/delivery_target.o \
                               $(OBJDIR)/modules/workspace/workspace_turn.o $(OBJDIR)/modules/delegates/delegate_sandbox_image.o $(OBJDIR)/modules/sandbox/sandbox_learned.o $(OBJDIR)/module_json_call.o $(OBJDIR)/harness_memory_common.o $(OBJDIR)/modules/workspace/workspace_provider_container.o $(OBJDIR)/modules/delegates/delegate_backend.o $(OBJDIR)/tests/support/git_cred_inject_stub.o $(OBJDIR)/modules/workspace/workspace_provider_detached.o $(OBJDIR)/modules/delegates/delegate_launch_args.o \
                               $(OBJDIR)/modules/workspace/workspace_runner_registry.o $(OBJDIR)/modules/workspace/workspace_runner_queue.o \
                               $(OBJDIR)/modules/workspace/workspace_mirror.o $(OBJDIR)/modules/git/forge_credentials.o $(OBJDIR)/modules/git/git_host_resolve.o \
	                               $(OBJDIR)/posix/workspace_provider.o $(OBJDIR)/json_fluent.o \
	                               $(OBJDIR)/server/token_tracker.o \
	                               $(OBJDIR)/server/request_context.o $(OBJDIR)/modules/db1/delegate_reservation.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# The embedder catalog is deliberately free of server deps so it links against cJSON
# alone — the point of splitting it out of server_state.c.
$(TESTPREFIX)/unit-test-embedder-catalog: $(OBJDIR)/tests/test_embedder_catalog.o \
                               $(OBJDIR)/server/embedder_catalog.o \
                               $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-agent-list-handler: $(OBJDIR)/tests/test_agent_list_handler.o \
                               $(OBJDIR)/server/server_agent.o $(OBJDIR)/modules/routing/agent_config.o $(OBJDIR)/modules/vault/agent_credentials.o $(OBJDIR)/modules/routing/agent_registry.o $(OBJDIR)/modules/routing/routing.o \
                               $(OBJDIR)/agent_tier_lint.o \
                               $(OBJDIR)/server/model_provider.o $(OBJDIR)/server/openai_profile.o \
                               $(OBJDIR)/server/anthropic_profile.o $(OBJDIR)/server/minimax_profile.o \
                               $(OBJDIR)/server/mistral_profile.o $(OBJDIR)/server/openrouter_profile.o \
                               $(OBJDIR)/server/ollama_profile.o $(OBJDIR)/server/llama_native_profile.o \
                               $(OBJDIR)/model_registry.o $(OBJDIR)/models_dev_cache.o $(OBJDIR)/models_dev.o \
                               $(OBJDIR)/tests/support/vault_service_stub.o \
                               $(OBJDIR)/tests/support/oauth_tokens_stub.o \
                               $(OBJDIR)/tests/support/provider_cli_adapter_stub.o \
                               $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-server-jobs-aux: $(OBJDIR)/tests/test_server_jobs_aux.o \
                               $(OBJDIR)/tests/support/delegate_role_seam_stub.o \
                               $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                               $(OBJDIR)/modules/db1/agent_jobs.o $(OBJDIR)/agent_job_release.o $(OBJDIR)/modules/db1/execution_plans.o \
                               $(OBJDIR)/modules/db1/coord_jobs.o $(OBJDIR)/modules/delegates/delegate_role.o \
                               $(OBJDIR)/role_templates.o \
                               $(OBJDIR)/json_fluent.o \
                               $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-agent-admission: $(OBJDIR)/tests/test_agent_admission.o $(OBJDIR)/server/agent_admission.o
	$(TESTLINK) -o $@ $^ -lpthread

# Workflow engine W1: pure (yaml/cJSON/dstr only), no DB/config needed.
$(TESTPREFIX)/unit-test-workflow: $(OBJDIR)/tests/test_workflow.o \
                                  $(OBJDIR)/modules/workflows/wfe_def.o $(OBJDIR)/modules/workflows/wfe_iface.o \
                                  $(OBJDIR)/modules/workflows/wfe_validate.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o $(OBJDIR)/modules/workflows/wfe_custom.o \
                                  $(OBJDIR)/aimee_home.o \
                                  $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Workflow engine W2: state machine + engine (DB1-backed).
$(TESTPREFIX)/unit-test-wfe-engine: $(OBJDIR)/tests/test_wfe_engine.o \
                                    $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                                    $(OBJDIR)/modules/db1/wfe_store.o $(OBJDIR)/modules/workflows/wfe_engine.o \
                                    $(OBJDIR)/wfe_store_lists.o \
                                    $(OBJDIR)/modules/workflows/wfe_def.o $(OBJDIR)/modules/workflows/wfe_iface.o \
                                    $(OBJDIR)/modules/workflows/wfe_validate.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o $(OBJDIR)/modules/workflows/wfe_custom.o \
                                    $(OBJDIR)/aimee_home.o $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o \
                                    $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Workflow engine W3: block executors (freeze is real git; others integration-gated).
$(TESTPREFIX)/unit-test-wfe-blocks: $(OBJDIR)/tests/test_wfe_blocks.o \
                                    $(OBJDIR)/modules/workflows/wfe_blocks.o $(OBJDIR)/modules/workflows/wfe_engine.o $(OBJDIR)/tests/support/config_autonomy_stub.o \
                                    $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                                    $(OBJDIR)/modules/db1/wfe_store.o $(OBJDIR)/modules/workflows/wfe_def.o \
                                    $(OBJDIR)/wfe_store_lists.o \
                                    $(OBJDIR)/modules/workflows/wfe_iface.o $(OBJDIR)/modules/workflows/wfe_validate.o \
                                    $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o $(OBJDIR)/modules/workflows/wfe_custom.o $(OBJDIR)/aimee_home.o \
                                    $(OBJDIR)/modules/workflows/wfe_deliver.o $(OBJDIR)/modules/workflows/wfe_manager_artifacts.o \
                                    $(OBJDIR)/util.o $(OBJDIR)/posix/util.o $(OBJDIR)/yaml.o \
                                    $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# primary-as-manager (S0): the def-parse-based tests need the def/validate/canonical
# chain (no engine/db1); the schema + externalization tests are pure.
$(TESTPREFIX)/unit-test-wfe-manager-blocks: $(OBJDIR)/tests/test_wfe_manager_blocks.o \
                                    $(OBJDIR)/modules/workflows/wfe_def.o $(OBJDIR)/modules/workflows/wfe_iface.o \
                                    $(OBJDIR)/modules/workflows/wfe_validate.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                                    $(OBJDIR)/modules/workflows/wfe_custom.o $(OBJDIR)/aimee_home.o \
                                    $(OBJDIR)/util.o $(OBJDIR)/posix/util.o $(OBJDIR)/yaml.o \
                                    $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-wfe-manager-artifacts: $(OBJDIR)/tests/test_wfe_manager_artifacts.o \
                                    $(OBJDIR)/modules/workflows/wfe_manager_artifacts.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-rrf-purity: $(OBJDIR)/tests/test_kb_rrf_purity.o $(OBJDIR)/rrf.o
	$(TESTLINK) -o $@ $^ $(L_CORE) -lm

$(TESTPREFIX)/unit-test-web-search-fusion: $(OBJDIR)/tests/test_web_search_fusion.o \
                                    $(OBJDIR)/server/web_search.o \
                                    $(OBJDIR)/server/web_search_fuse.o $(OBJDIR)/server/web_search_breaker.o $(OBJDIR)/rrf.o \
                                    $(OBJDIR)/posix/web_read.o \
                                    $(OBJDIR)/dstr.o $(OBJDIR)/util.o $(OBJDIR)/log.o \
                                    $(OBJDIR)/aimee_home.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(L_CORE)

$(TESTPREFIX)/unit-test-web-search-fuse: $(OBJDIR)/tests/test_web_search_fuse.o \
                                    $(OBJDIR)/server/web_search_fuse.o \
                                    $(OBJDIR)/server/web_search.o \
                                    $(OBJDIR)/posix/web_read.o \
                                    $(OBJDIR)/rrf.o \
                                    $(OBJDIR)/modules/db1/web_page_cache.o $(OBJDIR)/modules/db1/db1_init.o \
                                    $(OBJDIR)/modules/db1/db_schema.o $(OBJDIR)/modules/db1/db1_write.o \
                                    $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(L_CORE) -lm

$(TESTPREFIX)/unit-test-web-search-breaker: $(OBJDIR)/tests/test_web_search_breaker.o \
                                    $(OBJDIR)/server/web_search_breaker.o
	$(TESTLINK) -o $@ $^ $(L_CORE)

$(TESTPREFIX)/unit-test-dependency-breaker: $(OBJDIR)/tests/test_dependency_breaker.o
	$(TESTLINK) -o $@ $^ $(L_CORE)

$(TESTPREFIX)/unit-test-web-page-cache: $(OBJDIR)/tests/test_web_page_cache.o \
                                    $(OBJDIR)/modules/db1/web_page_cache.o $(OBJDIR)/modules/db1/db1_init.o \
                                    $(OBJDIR)/modules/db1/db_schema.o $(OBJDIR)/modules/db1/db1_write.o \
                                    $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(L_CORE)

$(TESTPREFIX)/unit-test-web-egress: $(OBJDIR)/tests/test_web_egress.o \
                                    $(OBJDIR)/posix/web_egress.o \
                                    $(OBJDIR)/dstr.o $(OBJDIR)/util.o $(OBJDIR)/log.o \
                                    $(OBJDIR)/aimee_home.o $(OBJDIR)/vendor/cJSON.o
	$(TESTLINK) -o $@ $^ $(L_CORE)

$(TESTPREFIX)/unit-test-web-read-spans: $(OBJDIR)/tests/test_web_read_spans.o \
                                    $(OBJDIR)/dstr.o $(OBJDIR)/util.o $(OBJDIR)/log.o \
                                    $(OBJDIR)/aimee_home.o $(OBJDIR)/vendor/cJSON.o
	$(CC) $(L_FLAGS) -o $@ $^

$(TESTPREFIX)/unit-test-cli-claude-allowlist: $(OBJDIR)/tests/test_cli_claude_allowlist.o \
                                    $(OBJDIR)/server/cli_claude.o \
                                    $(OBJDIR)/modules/workflows/wfe_externalization.o \
                                    $(OBJDIR)/modules/workflows/wfe_native_gate.o \
                                    $(OBJDIR)/modules/workflows/tool_egress.o
	$(TESTLINK) -o $@ $^ $(L_CORE)

$(TESTPREFIX)/unit-test-tool-egress: $(OBJDIR)/tests/test_tool_egress.o \
                                    $(OBJDIR)/modules/workflows/tool_egress.o \
                                    $(OBJDIR)/modules/workflows/wfe_externalization.o \
                                    $(OBJDIR)/modules/workflows/wfe_native_gate.o
	$(CC) $(L_FLAGS) -o $@ $^

$(TESTPREFIX)/unit-test-wfe-externalization: $(OBJDIR)/tests/test_wfe_externalization.o \
                                    $(OBJDIR)/modules/workflows/wfe_externalization.o $(OBJDIR)/modules/workflows/tool_egress.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-wfe-deliver: $(OBJDIR)/tests/test_wfe_deliver.o \
                                    $(OBJDIR)/modules/workflows/wfe_deliver.o $(OBJDIR)/modules/workflows/wfe_def.o \
                                    $(OBJDIR)/modules/workflows/wfe_iface.o $(OBJDIR)/modules/workflows/wfe_validate.o \
                                    $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o $(OBJDIR)/modules/workflows/wfe_custom.o \
                                    $(OBJDIR)/modules/workflows/wfe_deliver.o $(OBJDIR)/modules/workflows/wfe_manager_artifacts.o \
                                    $(OBJDIR)/aimee_home.o $(OBJDIR)/util.o $(OBJDIR)/posix/util.o \
                                    $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# S1 router pure core (no engine/DB/LLM deps).
$(TESTPREFIX)/unit-test-wfe-router: $(OBJDIR)/tests/test_wfe_router.o \
                                    $(OBJDIR)/modules/workflows/wfe_router.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# S4 autonomous-parity routing policy (pure; no engine/DB deps).
$(OBJDIR)/tests/test_wfe_autonomous_route.o: C_FLAGS += -Icore/event_bus/include
$(TESTPREFIX)/unit-test-wfe-autonomous-route: $(OBJDIR)/tests/test_wfe_autonomous_route.o \
                                    $(OBJDIR)/modules/workflows/wfe_autonomous_route.o \
                                    $(OBJDIR)/module_json_call.o \
                                    $(OBJDIR)/tests/support/module_bus_stub.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# The C side of the git-forge-request seam: the stage reply is canned through
# module_bus_stub, so this pins the translation into the caller's return codes
# without a forge, a credential or a repository. The HTTP entry points are
# stubbed inside the test TU (and abort), which is why no agent_exec object is
# linked here.
$(TESTPREFIX)/unit-test-git-pr-stage: $(OBJDIR)/tests/test_git_pr_stage.o \
                                    $(OBJDIR)/modules/git/git_pr_api.o \
                                    $(OBJDIR)/modules/git/git_pr_ci_grade.o \
                                    $(OBJDIR)/module_json_call.o \
                                    $(OBJDIR)/tests/support/module_bus_stub.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# S2 enforcement pure cores (no engine/DB deps).
$(TESTPREFIX)/unit-test-wfe-native-gate: $(OBJDIR)/tests/test_wfe_native_gate.o \
                                    $(OBJDIR)/modules/workflows/wfe_native_gate.o $(OBJDIR)/modules/workflows/wfe_externalization.o $(OBJDIR)/modules/workflows/tool_egress.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-wfe-enforce: $(OBJDIR)/tests/test_wfe_enforce.o \
                                    $(OBJDIR)/modules/workflows/wfe_enforce.o $(OBJDIR)/modules/workflows/wfe_externalization.o $(OBJDIR)/modules/workflows/tool_egress.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# S2 sub-slice 3: advance_request parser + event-bus decision seam (provider faked here).
$(TESTPREFIX)/unit-test-wfe-advance: $(OBJDIR)/tests/test_wfe_advance.o \
                                    $(OBJDIR)/modules/workflows/wfe_advance.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# S2 binding seam: auth-token->sid parser + idempotent interactive bind.
$(TESTPREFIX)/unit-test-wfe-bind-ingress: $(OBJDIR)/tests/test_wfe_bind_ingress.o \
                                    $(OBJDIR)/modules/workflows/wfe_bind_ingress.o $(OBJDIR)/log.o $(OBJDIR)/modules/workflows/wfe_enforce.o \
                                    $(OBJDIR)/modules/workflows/wfe_externalization.o $(OBJDIR)/modules/workflows/tool_egress.o $(OBJDIR)/modules/db1/wfe_binding.o \
                                    $(OBJDIR)/modules/workflows/wfe_router.o $(OBJDIR)/modules/workflows/wfe_router_catalog.o \
                                    $(OBJDIR)/modules/workflows/wfe_blocks.o $(OBJDIR)/modules/workflows/wfe_engine.o $(OBJDIR)/tests/support/config_autonomy_stub.o \
                                    $(OBJDIR)/modules/workflows/wfe_manager_artifacts.o $(OBJDIR)/modules/workflows/wfe_deliver.o \
                                    $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                                    $(OBJDIR)/modules/db1/wfe_store.o $(OBJDIR)/modules/workflows/wfe_def.o \
                                    $(OBJDIR)/wfe_store_lists.o \
                                    $(OBJDIR)/modules/workflows/wfe_iface.o $(OBJDIR)/modules/workflows/wfe_validate.o \
                                    $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o $(OBJDIR)/modules/workflows/wfe_custom.o \
                                    $(OBJDIR)/aimee_home.o $(OBJDIR)/util.o $(OBJDIR)/posix/util.o \
                                    $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Primary-CLI-ingestor S2 seam (Slice 2): gate + enforce-before-send. Same dep
# closure as unit-test-wfe-bind-ingress (it calls wfe_bind_interactive) + the
# ingestor object.
$(TESTPREFIX)/unit-test-primary-cli-ingestor: $(OBJDIR)/tests/test_primary_cli_ingestor.o \
                                    $(OBJDIR)/server/primary_cli_ingestor.o $(OBJDIR)/log.o \
                                    $(OBJDIR)/modules/workflows/wfe_bind_ingress.o $(OBJDIR)/modules/workflows/wfe_enforce.o \
                                    $(OBJDIR)/modules/workflows/wfe_externalization.o $(OBJDIR)/modules/workflows/tool_egress.o $(OBJDIR)/modules/db1/wfe_binding.o \
                                    $(OBJDIR)/modules/workflows/wfe_router.o $(OBJDIR)/modules/workflows/wfe_router_catalog.o \
                                    $(OBJDIR)/modules/workflows/wfe_blocks.o $(OBJDIR)/modules/workflows/wfe_engine.o $(OBJDIR)/tests/support/config_autonomy_stub.o \
                                    $(OBJDIR)/modules/workflows/wfe_manager_artifacts.o $(OBJDIR)/modules/workflows/wfe_deliver.o \
                                    $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                                    $(OBJDIR)/modules/db1/wfe_store.o $(OBJDIR)/modules/workflows/wfe_def.o \
                                    $(OBJDIR)/wfe_store_lists.o \
                                    $(OBJDIR)/modules/workflows/wfe_iface.o $(OBJDIR)/modules/workflows/wfe_validate.o \
                                    $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o $(OBJDIR)/modules/workflows/wfe_custom.o \
                                    $(OBJDIR)/aimee_home.o $(OBJDIR)/util.o $(OBJDIR)/posix/util.o \
                                    $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# S2 sub-slice 4: per-block resolver + dispatch-time externalization guard.
$(TESTPREFIX)/unit-test-wfe-block-resolve: $(OBJDIR)/tests/test_wfe_block_resolve.o \
                                    $(OBJDIR)/modules/workflows/wfe_block_resolve.o $(OBJDIR)/modules/workflows/wfe_enforce.o \
                                    $(OBJDIR)/modules/workflows/wfe_externalization.o $(OBJDIR)/modules/workflows/tool_egress.o $(OBJDIR)/modules/db1/wfe_binding.o \
                                    $(OBJDIR)/modules/workflows/wfe_blocks.o $(OBJDIR)/modules/workflows/wfe_engine.o $(OBJDIR)/tests/support/config_autonomy_stub.o \
                                    $(OBJDIR)/modules/workflows/wfe_manager_artifacts.o $(OBJDIR)/modules/workflows/wfe_deliver.o \
                                    $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                                    $(OBJDIR)/modules/db1/wfe_store.o $(OBJDIR)/modules/workflows/wfe_def.o \
                                    $(OBJDIR)/wfe_store_lists.o \
                                    $(OBJDIR)/modules/workflows/wfe_iface.o $(OBJDIR)/modules/workflows/wfe_validate.o \
                                    $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o $(OBJDIR)/modules/workflows/wfe_custom.o \
                                    $(OBJDIR)/aimee_home.o $(OBJDIR)/util.o $(OBJDIR)/posix/util.o \
                                    $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# S2 sub-slice 3: interactive-driver executor (binding + engine + audit).
$(TESTPREFIX)/unit-test-wfe-advance-exec: $(OBJDIR)/tests/test_wfe_advance_exec.o \
                                    $(OBJDIR)/modules/workflows/wfe_advance_exec.o $(OBJDIR)/modules/workflows/wfe_advance.o \
                                    $(OBJDIR)/modules/workflows/wfe_enforce.o $(OBJDIR)/modules/workflows/wfe_externalization.o $(OBJDIR)/modules/workflows/tool_egress.o \
                                    $(OBJDIR)/modules/db1/wfe_binding.o \
                                    $(OBJDIR)/modules/workflows/wfe_blocks.o $(OBJDIR)/modules/workflows/wfe_engine.o $(OBJDIR)/tests/support/config_autonomy_stub.o \
                                    $(OBJDIR)/modules/workflows/wfe_manager_artifacts.o $(OBJDIR)/modules/workflows/wfe_deliver.o \
                                    $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                                    $(OBJDIR)/modules/db1/wfe_store.o $(OBJDIR)/modules/workflows/wfe_def.o \
                                    $(OBJDIR)/wfe_store_lists.o \
                                    $(OBJDIR)/modules/workflows/wfe_iface.o $(OBJDIR)/modules/workflows/wfe_validate.o \
                                    $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o $(OBJDIR)/modules/workflows/wfe_custom.o \
                                    $(OBJDIR)/aimee_home.o $(OBJDIR)/util.o $(OBJDIR)/posix/util.o \
                                    $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# S2 session<->work-item binding (DB1-backed).
$(TESTPREFIX)/unit-test-wfe-binding: $(OBJDIR)/tests/test_wfe_binding.o \
                                    $(OBJDIR)/modules/db1/wfe_binding.o $(OBJDIR)/modules/db1/wfe_store.o \
                                    $(OBJDIR)/wfe_store_lists.o \
                                    $(OBJDIR)/modules/db1/db1_init.o \
                                    $(OBJDIR)/modules/db1/db1_write.o $(OBJDIR)/modules/db1/db1_trigger.o \
                                    $(OBJDIR)/modules/db1/db1_cron_jobs.o $(OBJDIR)/modules/db1/model_catalog.o $(OBJDIR)/model_catalog_release.o \
                                    $(OBJDIR)/modules/db1/eval.o $(OBJDIR)/db2/db_schema.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Slice 0: the canonical IR (pure — cJSON only).
$(TESTPREFIX)/unit-test-aimee-ir: $(OBJDIR)/tests/test_aimee_ir.o \
                                 $(OBJDIR)/modules/ir/aimee_ir.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Slice 0: IR shadow metrics (pure).
$(TESTPREFIX)/unit-test-aimee-ir-metrics: $(OBJDIR)/tests/test_aimee_ir_metrics.o \
                                         $(OBJDIR)/modules/ir/aimee_ir_metrics.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Slice 1: frontend parse adapters (pure — cJSON only).
$(TESTPREFIX)/unit-test-aimee-frontend: $(OBJDIR)/tests/test_aimee_frontend.o \
                                       $(OBJDIR)/modules/translation/aimee_frontend_anthropic.o \
                                       $(OBJDIR)/modules/translation/aimee_frontend_openai.o \
                                       $(OBJDIR)/modules/translation/aimee_frontend_responses.o \
                                       $(OBJDIR)/modules/translation/aimee_backend_responses.o \
                                       $(OBJDIR)/modules/ir/aimee_ir.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Slice 2: backend build/parse adapters (pure — cJSON only).
$(TESTPREFIX)/unit-test-aimee-backend: $(OBJDIR)/tests/test_aimee_backend.o \
                                      $(OBJDIR)/modules/translation/aimee_backend_anthropic.o \
                                      $(OBJDIR)/modules/translation/aimee_backend_openai.o \
                                      $(OBJDIR)/modules/translation/aimee_backend_responses.o \
                                      $(OBJDIR)/modules/translation/aimee_frontend_anthropic.o \
                                      $(OBJDIR)/modules/translation/aimee_frontend_openai.o \
                                      $(OBJDIR)/modules/ir/aimee_ir.o $(OBJDIR)/cJSON.o \
                                      $(OBJDIR)/text.o $(OBJDIR)/util.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Slice P6c-ir: Bedrock Converse backend build/parse (pure — cJSON only).
$(TESTPREFIX)/unit-test-aimee-backend-bedrock: $(OBJDIR)/tests/test_aimee_backend_bedrock.o \
                                      $(OBJDIR)/modules/translation/aimee_backend_bedrock.o \
                                      $(OBJDIR)/modules/ir/aimee_ir.o $(OBJDIR)/cJSON.o \
                                      $(OBJDIR)/text.o $(OBJDIR)/util.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-bedrock-dispatch: $(OBJDIR)/tests/test_kb_bedrock_dispatch.o \
                                      $(OBJDIR)/tests/support/aws_eventstream_fixture.o \
                                      $(OBJDIR)/kb/kb_bedrock_egress.o \
                                      $(OBJDIR)/modules/translation/aimee_backend_bedrock.o \
                                      $(OBJDIR)/modules/translation/aimee_ir_stream.o \
                                      $(OBJDIR)/modules/ir/aimee_ir.o $(OBJDIR)/cJSON.o \
                                      $(OBJDIR)/modules/aws/aws_sigv4.o \
                                      $(OBJDIR)/modules/aws/aws_eventstream.o \
                                      $(OBJDIR)/modules/aws/bedrock_policy.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-http-client: $(OBJDIR)/tests/test_kb_http_client.o \
                                      $(OBJDIR)/kb/http/kb_http_client.o | $(KB_RESOLVER)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-bedrock-live: $(OBJDIR)/tests/test_kb_bedrock_live.o \
                              $(filter-out $(OBJDIR)/kb/kb_main.o,$(KB_OBJS)) $(OBJDIR)/dashboard_kb.o \
                              $(OBJDIR)/server/oauth_pkce.o $(OBJDIR)/server/embedder_probe.o \
                              $(OBJDIR)/modules/translation/aimee_backend_bedrock.o \
                              $(OBJDIR)/modules/translation/aimee_ir_stream.o $(OBJDIR)/modules/ir/aimee_ir.o \
                              $(KB_DATA_OBJS) $(KB_CORE_OBJS) $(KB_DB2_PG_OBJS) $(KB_DB2_OBJS) \
                              $(KB_VAULT_OBJS) $(KB_PLATFORM_OBJS) $(TS_VENDOR_OBJS) | $(KB_RESOLVER)
	$(TESTLINK) -o $@ $^ $(L_KB)

$(TESTPREFIX)/unit-test-kb-mgmt-live: $(OBJDIR)/tests/test_kb_mgmt_live.o \
                                      $(OBJDIR)/kb/kb_mgmt_client.o \
                                      $(OBJDIR)/kb/kb_mgmt_endpoint.o \
                                      $(OBJDIR)/kb/http/kb_tls.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-mgmt-endpoint: $(OBJDIR)/tests/test_kb_mgmt_endpoint.o \
                                          $(OBJDIR)/kb/kb_mgmt_endpoint.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-mgmt-status: $(OBJDIR)/tests/test_kb_mgmt_status.o \
                                        $(OBJDIR)/kb/kb_mgmt_status.o \
                                        $(OBJDIR)/util.o \
                                        $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-mgmt-status-listener: \
    $(OBJDIR)/tests/test_kb_mgmt_status_listener.o \
    $(OBJDIR)/kb/kb_mgmt_status_listener.o $(OBJDIR)/kb/kb_mgmt_status_peer.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-server-mgmt-status: $(OBJDIR)/tests/test_server_mgmt_status.o \
                                            $(OBJDIR)/server/server_mgmt_status.o \
                                            $(OBJDIR)/modules/db1/mgmt_nonce.o \
                                            $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db.o \
                                            $(OBJDIR)/modules/db1/db_schema.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-server-mgmt-token: $(OBJDIR)/tests/test_server_mgmt_token.o \
                                           $(OBJDIR)/shared/auth_token_verify.o \
                                           $(OBJDIR)/util.o \
                                        $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-server-mgmt-endpoint: $(OBJDIR)/tests/test_server_mgmt_endpoint.o \
                                              $(OBJDIR)/server/server_mgmt_endpoint.o \
                                              $(OBJDIR)/util.o \
                                        $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lcrypto

$(TESTPREFIX)/unit-test-server-mgmt-read: $(OBJDIR)/tests/test_server_mgmt_read.o \
                                          $(OBJDIR)/shared/management_read.o \
                                          $(OBJDIR)/util.o \
                                        $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lcrypto

$(TESTPREFIX)/unit-test-server-mgmt-read-source: \
    $(OBJDIR)/tests/test_server_mgmt_read_source.o \
    $(OBJDIR)/server/server_mgmt_read_source.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lcrypto

$(TESTPREFIX)/unit-test-server-mgmt-read-endpoint: \
    $(OBJDIR)/tests/test_server_mgmt_read_endpoint.o \
    $(OBJDIR)/server/server_mgmt_read_endpoint.o \
    $(OBJDIR)/shared/management_read.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lcrypto

$(TESTPREFIX)/unit-test-server-mgmt-checkpoint-client: \
    $(OBJDIR)/tests/test_server_mgmt_checkpoint_client.o \
    $(OBJDIR)/server/server_mgmt_checkpoint_client.o $(OBJDIR)/server/server_mgmt_status.o \
    $(OBJDIR)/modules/db1/mgmt_nonce.o \
    $(OBJDIR)/modules/vault/runtime_secret.o \
    $(OBJDIR)/kb/kb_mgmt_status.o $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db.o \
    $(OBJDIR)/modules/db1/db_schema.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lcrypto

$(TESTPREFIX)/unit-test-kb-mgmt-token: $(OBJDIR)/tests/test_kb_mgmt_token.o \
                                       $(OBJDIR)/kb/kb_mgmt_token.o \
                                       $(OBJDIR)/shared/auth_token_verify.o \
                                       $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-workspace-scan-indexed: $(OBJDIR)/tests/test_workspace_scan_indexed.o \
                                            $(OBJDIR)/server/server_workspace_scan.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

# Resolution order only: the workspace and kb calls are driven from the test, so
# this links the one TU under test and nothing else.
$(TESTPREFIX)/unit-test-server-active-project: $(OBJDIR)/tests/test_server_active_project.o \
                                            $(OBJDIR)/server/server_active_project.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-kb-login-throttle: $(OBJDIR)/tests/test_kb_login_throttle.o \
                                            $(OBJDIR)/kb/kb_login_throttle.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-kb-identity-token: $(OBJDIR)/tests/test_kb_identity_token.o \
                                           $(OBJDIR)/kb/kb_identity_token.o \
                                           $(OBJDIR)/server/oauth_pkce.o \
                                           $(OBJDIR)/util.o \
                                        $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-server-write-tier: $(OBJDIR)/tests/test_server_write_tier.o \
                                           $(OBJDIR)/server/server_write_tier.o \
                                           $(OBJDIR)/shared/auth_token_verify.o \
                                           $(OBJDIR)/kb/kb_identity_token.o \
                                           $(OBJDIR)/server/oauth_pkce.o \
                                           $(OBJDIR)/util.o \
                                        $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-server-identity-token: $(OBJDIR)/tests/test_server_identity_token.o \
                                               $(OBJDIR)/shared/auth_token_verify.o \
                                               $(OBJDIR)/kb/kb_caller_token.o \
                                               $(OBJDIR)/kb/kb_identity_token.o \
                                               $(OBJDIR)/server/oauth_pkce.o \
                                               $(OBJDIR)/util.o \
                                        $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-server-write-tier-db1: \
    $(OBJDIR)/tests/test_server_write_tier_db1.o \
    $(OBJDIR)/server/server_write_tier_db1.o $(OBJDIR)/server/server_runtime_identity.o \
    $(OBJDIR)/modules/db1/server_identity_jti.o \
    $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db.o $(OBJDIR)/modules/db1/db_schema.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-server-identity-jti: \
    $(OBJDIR)/tests/test_server_identity_jti.o \
    $(OBJDIR)/modules/db1/server_identity_jti.o $(OBJDIR)/modules/db1/db1_init.o \
    $(OBJDIR)/modules/db1/db.o $(OBJDIR)/modules/db1/db_schema.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-server-management-jti: \
    $(OBJDIR)/tests/test_server_management_jti.o \
    $(OBJDIR)/modules/db1/server_management_jti.o $(OBJDIR)/modules/db1/db1_init.o \
    $(OBJDIR)/modules/db1/db.o $(OBJDIR)/modules/db1/db_schema.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-mgmt-status-authority: \
    $(OBJDIR)/tests/test_kb_mgmt_status_authority.o \
    $(OBJDIR)/kb/kb_mgmt_status_authority.o $(OBJDIR)/kb/kb_mgmt_status.o \
    $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-management-health-exchange: \
    $(OBJDIR)/tests/test_kb_management_health_exchange.o \
    $(OBJDIR)/kb/kb_management_health_exchange.o \
    $(OBJDIR)/kb/kb_mgmt_status_authority.o $(OBJDIR)/kb/kb_mgmt_status.o \
    $(OBJDIR)/kb/kb_mgmt_endpoint.o $(OBJDIR)/kb/kb_mgmt_client.o \
    $(OBJDIR)/kb/http/kb_tls.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-management-action: \
    $(OBJDIR)/tests/test_kb_management_action.o \
    $(OBJDIR)/kb/kb_management_action.o $(OBJDIR)/kb/kb_management_health_exchange.o \
    $(OBJDIR)/kb/kb_mgmt_status_authority.o $(OBJDIR)/kb/kb_mgmt_status.o \
    $(OBJDIR)/kb/kb_mgmt_endpoint.o $(OBJDIR)/kb/kb_mgmt_client.o \
    $(OBJDIR)/kb/http/kb_tls.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-mgmt-status-client: \
    $(OBJDIR)/tests/test_kb_mgmt_status_client.o \
    $(OBJDIR)/kb/kb_mgmt_status_client.o $(OBJDIR)/kb/kb_mgmt_client.o \
    $(OBJDIR)/kb/kb_mgmt_endpoint.o $(OBJDIR)/kb/http/kb_tls.o \
    $(OBJDIR)/kb/pki.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-management-runtime: \
    $(OBJDIR)/tests/test_kb_management_runtime.o \
    $(OBJDIR)/kb/kb_management_runtime.o $(OBJDIR)/kb/kb_mgmt_status.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-http-servers-health: \
    $(OBJDIR)/tests/test_kb_http_servers_health.o \
    $(OBJDIR)/kb/http/kb_http_servers.o $(OBJDIR)/kb/kb_management_action.o \
    $(OBJDIR)/kb/kb_reqctx.o \
    $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-mgmt-status-custody: \
    $(OBJDIR)/tests/test_kb_mgmt_status_custody.o \
    $(OBJDIR)/kb/kb_mgmt_status_custody.o $(OBJDIR)/kb/kb_mgmt_status.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-management-status-key-ctx: \
    $(OBJDIR)/tests/test_management_status_key_ctx.o \
    $(OBJDIR)/db2/management_status_key.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-kb-mgmt-status-provision: \
    $(OBJDIR)/tests/test_kb_mgmt_status_provision.o \
    $(OBJDIR)/kb/kb_mgmt_status_provision.o \
    $(OBJDIR)/modules/vault/vault_crypto.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-mgmt-token-roots-provision: \
    $(OBJDIR)/tests/test_kb_mgmt_token_roots_provision.o \
    $(OBJDIR)/kb/kb_mgmt_token_roots_provision.o \
    $(OBJDIR)/kb/kb_mgmt_token_public.o \
    $(OBJDIR)/modules/vault/vault_crypto.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-mgmt-token-authority: \
    $(OBJDIR)/tests/test_kb_mgmt_token_authority.o \
    $(OBJDIR)/modules/db2/c/management_token_authority.o \
    $(OBJDIR)/modules/db2/c/db_postgres.o \
    $(OBJDIR)/kb/kb_mgmt_token_authority.o $(OBJDIR)/kb/kb_mgmt_token.o \
    $(OBJDIR)/kb/kb_mgmt_token_public.o \
    $(OBJDIR)/modules/vault/vault_crypto.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-identity-token-authority: \
    $(OBJDIR)/tests/test_kb_identity_token_authority.o \
    $(OBJDIR)/modules/db2/c/management_token_authority.o \
    $(OBJDIR)/modules/db2/c/db_postgres.o \
    $(OBJDIR)/kb/kb_mgmt_token_authority.o $(OBJDIR)/kb/kb_mgmt_token.o \
    $(OBJDIR)/kb/kb_identity_token.o $(OBJDIR)/kb/kb_mgmt_token_public.o \
    $(OBJDIR)/shared/auth_token_verify.o $(OBJDIR)/server/oauth_pkce.o \
    $(OBJDIR)/modules/vault/vault_crypto.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-mgmt-token-authority-ipc: \
    $(OBJDIR)/tests/test_kb_mgmt_token_authority_ipc.o \
    $(OBJDIR)/kb/kb_mgmt_token_authority_ipc.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-mgmt-jwks-publication: \
    $(OBJDIR)/tests/test_kb_mgmt_jwks_publication.o \
    $(OBJDIR)/kb/kb_mgmt_jwks_publication.o \
    $(OBJDIR)/kb/kb_mgmt_token_roots_provision.o \
    $(OBJDIR)/kb/kb_mgmt_token_public.o \
    $(OBJDIR)/modules/vault/vault_crypto.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-server-mgmt-jwks-cache: \
    $(OBJDIR)/tests/test_server_mgmt_jwks_cache.o \
    $(OBJDIR)/server/server_mgmt_jwks_cache.o \
    $(OBJDIR)/modules/db1/mgmt_jwks_cache.o \
    $(OBJDIR)/kb/kb_mgmt_jwks_publication.o \
    $(OBJDIR)/kb/kb_mgmt_token_roots_provision.o \
    $(OBJDIR)/kb/kb_mgmt_token_public.o \
    $(OBJDIR)/modules/vault/vault_crypto.o \
    $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db.o $(OBJDIR)/modules/db1/db_schema.o \
    $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-mgmt-offline-hardening: \
    $(OBJDIR)/tests/test_kb_mgmt_offline_hardening.o \
    $(OBJDIR)/kb/kb_mgmt_offline_hardening.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-management-status-runtime: \
    $(OBJDIR)/tests/test_management_status_runtime.o \
    $(OBJDIR)/db2/management_status_runtime.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-management-client-instance: \
    $(OBJDIR)/tests/test_management_client_instance.o \
    $(OBJDIR)/db2/management_client_instance.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto

$(TESTPREFIX)/unit-test-management-action-journal: \
    $(OBJDIR)/tests/test_management_action_journal.o \
    $(OBJDIR)/db2/management_action_journal.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

# The identity journal links management_action_journal.o only for the shared
# SQLSTATE classifier; the pg layer and tenant scope are mocked in the test.
$(TESTPREFIX)/unit-test-management-identity-journal: \
    $(OBJDIR)/tests/test_management_identity_journal.o \
    $(OBJDIR)/db2/management_identity_journal.o \
    $(OBJDIR)/db2/management_action_journal.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-kb-management-cert-lifecycle: \
    $(OBJDIR)/tests/test_kb_management_cert_lifecycle.o \
    $(OBJDIR)/kb/kb_management_cert_codec.o \
    $(OBJDIR)/kb/kb_management_cert_crypto.o \
    $(OBJDIR)/kb/kb_management_cert_binding.o \
    $(OBJDIR)/tests/p5b2c/kb_management_cert_lifecycle.o \
    $(OBJDIR)/tests/p5b2c/kb_management_cert_storage.o \
    $(OBJDIR)/kb/pki.o \
    $(OBJDIR)/kb/kb_workload_provider.o \
    $(OBJDIR)/kb/kb_workload_helper_posix.o \
    $(OBJDIR)/kb/kb_workload_wire.o \
    $(OBJDIR)/kb/kb_workload_proof.o \
    $(OBJDIR)/kb/kb_workload_jwt.o \
    $(OBJDIR)/modules/aws/aws_sts.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(OBJDIR)/tests/test_kb_management_cert_lifecycle.o: C_FLAGS += -DAIMEE_MANAGEMENT_CERT_TESTING

$(OBJDIR)/tests/p5b2c/kb_management_cert_lifecycle.o: kb/kb_management_cert_lifecycle.c
	@mkdir -p $(dir $@)
	$(CC) -c $(C_FLAGS) -DAIMEE_MANAGEMENT_CERT_TESTING -o $@ $<

$(OBJDIR)/tests/p5b2c/kb_management_cert_storage.o: kb/kb_management_cert_storage.c
	@mkdir -p $(dir $@)
	$(CC) -c $(C_FLAGS) -DAIMEE_MANAGEMENT_CERT_TESTING -o $@ $<

$(TESTPREFIX)/unit-test-kb-workload-wire: $(OBJDIR)/tests/test_kb_workload_wire.o \
                                            $(OBJDIR)/kb/kb_workload_wire.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-kb-workload-proof: $(OBJDIR)/tests/test_kb_workload_proof.o \
                                             $(OBJDIR)/kb/kb_workload_proof.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-workload-jwt: $(OBJDIR)/tests/test_kb_workload_jwt.o \
                                           $(OBJDIR)/kb/kb_workload_jwt.o \
                                           $(OBJDIR)/modules/aws/aws_sts.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-workload-helper-posix: \
    $(OBJDIR)/tests/test_kb_workload_helper_posix.o \
    $(OBJDIR)/kb/kb_workload_helper_posix.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Provider orchestration is tested with deterministic lower-level seams. The
# production object above still links the real checked-fd/JWT/proof objects into
# aimee-kb; only this isolated test object rewrites their symbol names.
$(OBJDIR)/tests/kb_workload_provider_mocked.o: kb/kb_workload_provider.c
	@mkdir -p $(dir $@)
	$(CC) -c $(TEST_C_FLAGS) \
	  -Dkb_workload_checked_root_file_open=mock_checked_root_file_open \
	  -Dkb_workload_helper_invoke=mock_helper_invoke \
	  -Dkb_workload_proof_key_load_der=mock_proof_key_load \
	  -Dkb_workload_proof_key_close=mock_proof_key_close \
	  -Dkb_workload_proof_anchor_id=mock_proof_anchor \
	  -Dkb_workload_proof_verify=mock_proof_verify \
	  -Dkb_workload_jwt_validate_ex=mock_jwt_validate_ex -o $@ $<

$(TESTPREFIX)/unit-test-kb-workload-provider: \
    $(OBJDIR)/tests/test_kb_workload_provider.o \
    $(OBJDIR)/tests/kb_workload_provider_mocked.o $(OBJDIR)/kb/kb_workload_wire.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-vault-kms: $(OBJDIR)/tests/test_vault_kms.o \
                                  $(OBJDIR)/modules/vault/vault_custody_kms.o \
                                  $(OBJDIR)/modules/vault/vault_hwm.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-vault-kms-hwm-live: $(OBJDIR)/tests/test_vault_kms_hwm_live.o \
                                  $(OBJDIR)/modules/vault/vault_custody_kms.o \
                                  $(OBJDIR)/modules/vault/vault_hwm.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Slice 3: IR shadow observer (pure — cJSON only).
$(TESTPREFIX)/unit-test-aimee-ir-shadow: $(OBJDIR)/tests/test_aimee_ir_shadow.o \
                                        $(OBJDIR)/server/aimee_ir_shadow.o \
                                        $(OBJDIR)/modules/ir/aimee_ir_metrics.o \
                                        $(OBJDIR)/modules/translation/aimee_backend_anthropic.o \
                                        $(OBJDIR)/modules/translation/aimee_frontend_anthropic.o \
                                        $(OBJDIR)/modules/translation/aimee_frontend_openai.o \
                                        $(OBJDIR)/modules/ir/aimee_ir.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Slice 5: IR live request-build (pure — cJSON only).
$(TESTPREFIX)/unit-test-aimee-ir-session: $(OBJDIR)/tests/test_aimee_ir_session.o \
                                          $(OBJDIR)/modules/ir/aimee_ir_session.o \
                                          $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-aimee-ir-serve: $(OBJDIR)/tests/test_aimee_ir_serve.o \
                                       $(OBJDIR)/modules/ir/aimee_ir_session.o \
                                       $(OBJDIR)/server/aimee_ir_serve.o \
                                       $(OBJDIR)/modules/translation/aimee_backend_openai.o \
                                       $(OBJDIR)/modules/translation/aimee_backend_responses.o \
                                       $(OBJDIR)/modules/translation/aimee_frontend_anthropic.o \
                                       $(OBJDIR)/modules/translation/aimee_frontend_openai.o \
                                       $(OBJDIR)/modules/translation/aimee_frontend_responses.o \
                                       $(OBJDIR)/modules/ir/aimee_ir.o \
                                       $(OBJDIR)/tests/support/ir_seam_memory_stub.o \
                                       $(OBJDIR)/modules/ir/aimee_ir_metrics.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Slice 4: IR-delta streaming (pure — cJSON only).
$(TESTPREFIX)/unit-test-aimee-ir-stream: $(OBJDIR)/tests/test_aimee_ir_stream.o \
                                        $(OBJDIR)/modules/translation/aimee_ir_stream.o \
                                        $(OBJDIR)/modules/ir/aimee_ir.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Slice P6c-stream: Bedrock ConverseStream event -> IR delta parser (pure — cJSON
# only; links aimee_backend_bedrock.o for the shared converse_stop_reason mapping).
$(TESTPREFIX)/unit-test-aimee-converse-stream: $(OBJDIR)/tests/test_aimee_converse_stream.o \
                                        $(OBJDIR)/modules/translation/aimee_ir_stream.o \
                                        $(OBJDIR)/modules/translation/aimee_backend_bedrock.o \
                                        $(OBJDIR)/modules/ir/aimee_ir.o $(OBJDIR)/cJSON.o \
                                        $(OBJDIR)/text.o $(OBJDIR)/util.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# S1 router catalog I/O (enumerates $AIMEE_HOME workflows + built-in lanes).
$(TESTPREFIX)/unit-test-wfe-router-catalog: $(OBJDIR)/tests/test_wfe_router_catalog.o \
                                    $(OBJDIR)/modules/workflows/wfe_router_catalog.o $(OBJDIR)/modules/workflows/wfe_router.o \
                                    $(OBJDIR)/yaml.o $(OBJDIR)/cJSON.o $(OBJDIR)/aimee_home.o \
                                    $(OBJDIR)/dstr.o $(OBJDIR)/posix/platform_path.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Integration: the manager executors driven through the real engine (DB1-backed).
$(TESTPREFIX)/unit-test-wfe-manager-flow: $(OBJDIR)/tests/test_wfe_manager_flow.o \
                                    $(OBJDIR)/modules/workflows/wfe_blocks.o $(OBJDIR)/modules/workflows/wfe_engine.o $(OBJDIR)/tests/support/config_autonomy_stub.o \
                                    $(OBJDIR)/modules/workflows/wfe_manager_artifacts.o $(OBJDIR)/modules/workflows/wfe_deliver.o \
                                    $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                                    $(OBJDIR)/modules/db1/wfe_store.o $(OBJDIR)/modules/workflows/wfe_def.o \
                                    $(OBJDIR)/wfe_store_lists.o \
                                    $(OBJDIR)/modules/workflows/wfe_iface.o $(OBJDIR)/modules/workflows/wfe_validate.o \
                                    $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o $(OBJDIR)/modules/workflows/wfe_custom.o \
                                    $(OBJDIR)/modules/workflows/wfe_deliver.o $(OBJDIR)/modules/workflows/wfe_manager_artifacts.o \
                                    $(OBJDIR)/aimee_home.o $(OBJDIR)/util.o $(OBJDIR)/posix/util.o \
                                    $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Config-extensible blocks + safety blocks share the blocks/engine/registry deps.
$(TESTPREFIX)/unit-test-wfe-custom: $(OBJDIR)/tests/test_wfe_custom.o \
                                    $(OBJDIR)/modules/workflows/wfe_blocks.o $(OBJDIR)/modules/workflows/wfe_engine.o $(OBJDIR)/tests/support/config_autonomy_stub.o \
                                    $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                                    $(OBJDIR)/modules/db1/wfe_store.o $(OBJDIR)/modules/workflows/wfe_def.o \
                                    $(OBJDIR)/wfe_store_lists.o \
                                    $(OBJDIR)/modules/workflows/wfe_iface.o $(OBJDIR)/modules/workflows/wfe_validate.o \
                                    $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o $(OBJDIR)/modules/workflows/wfe_custom.o \
                                    $(OBJDIR)/modules/workflows/wfe_deliver.o $(OBJDIR)/modules/workflows/wfe_manager_artifacts.o \
                                    $(OBJDIR)/aimee_home.o $(OBJDIR)/util.o $(OBJDIR)/posix/util.o \
                                    $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-wfe-safety: $(OBJDIR)/tests/test_wfe_safety.o \
                                    $(OBJDIR)/modules/workflows/wfe_blocks.o $(OBJDIR)/modules/workflows/wfe_engine.o $(OBJDIR)/tests/support/config_autonomy_stub.o \
                                    $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                                    $(OBJDIR)/modules/db1/wfe_store.o $(OBJDIR)/modules/workflows/wfe_def.o \
                                    $(OBJDIR)/wfe_store_lists.o \
                                    $(OBJDIR)/modules/workflows/wfe_iface.o $(OBJDIR)/modules/workflows/wfe_validate.o \
                                    $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o $(OBJDIR)/modules/workflows/wfe_custom.o \
                                    $(OBJDIR)/modules/workflows/wfe_deliver.o $(OBJDIR)/modules/workflows/wfe_manager_artifacts.o \
                                    $(OBJDIR)/aimee_home.o $(OBJDIR)/util.o $(OBJDIR)/posix/util.o \
                                    $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-wfe-failure-taxonomy: $(OBJDIR)/tests/test_wfe_failure_taxonomy.o \
                                    $(OBJDIR)/modules/workflows/wfe_iface.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm

$(TESTPREFIX)/unit-test-wfe-delegate-seam: $(OBJDIR)/tests/test_wfe_delegate_seam.o \
                                    $(OBJDIR)/modules/workflows/wfe_blocks.o $(OBJDIR)/modules/workflows/wfe_engine.o $(OBJDIR)/tests/support/config_autonomy_stub.o \
                                    $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                                    $(OBJDIR)/modules/db1/wfe_store.o $(OBJDIR)/modules/workflows/wfe_def.o \
                                    $(OBJDIR)/wfe_store_lists.o \
                                    $(OBJDIR)/modules/workflows/wfe_iface.o $(OBJDIR)/modules/workflows/wfe_validate.o \
                                    $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o $(OBJDIR)/modules/workflows/wfe_custom.o \
                                    $(OBJDIR)/modules/workflows/wfe_deliver.o $(OBJDIR)/modules/workflows/wfe_manager_artifacts.o \
                                    $(OBJDIR)/aimee_home.o $(OBJDIR)/util.o $(OBJDIR)/posix/util.o \
                                    $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-wfe-scheduler: $(OBJDIR)/tests/test_wfe_scheduler.o \
                                    $(OBJDIR)/modules/workflows/wfe_scheduler.o $(OBJDIR)/modules/workflows/wfe_autonomy.o \
                                    $(OBJDIR)/tests/support/log_stub.o \
                                    $(OBJDIR)/modules/workflows/wfe_blocks.o $(OBJDIR)/modules/workflows/wfe_engine.o $(OBJDIR)/tests/support/config_autonomy_stub.o \
                                    $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                                    $(OBJDIR)/modules/db1/wfe_store.o $(OBJDIR)/modules/workflows/wfe_def.o \
                                    $(OBJDIR)/wfe_store_lists.o \
                                    $(OBJDIR)/modules/workflows/wfe_iface.o $(OBJDIR)/modules/workflows/wfe_validate.o \
                                    $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o $(OBJDIR)/modules/workflows/wfe_custom.o \
                                    $(OBJDIR)/modules/workflows/wfe_roundtable.o $(OBJDIR)/modules/workflows/wfe_approval.o \
                                    $(OBJDIR)/modules/workflows/wfe_verdict.o $(OBJDIR)/module_json_call.o $(OBJDIR)/tests/support/module_bus_stub.o \
                                    $(OBJDIR)/aimee_home.o $(OBJDIR)/util.o $(OBJDIR)/posix/util.o \
                                    $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Human-gate reject routing (retry_on_reject): pure def-query, no DB.
$(TESTPREFIX)/unit-test-wfe-gate-reject: $(OBJDIR)/tests/test_wfe_gate_reject.o \
                                    $(OBJDIR)/modules/workflows/wfe_def.o $(OBJDIR)/modules/workflows/wfe_iface.o \
                                    $(OBJDIR)/modules/workflows/wfe_validate.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                                    $(OBJDIR)/modules/workflows/wfe_custom.o $(OBJDIR)/aimee_home.o \
                                    $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Atomic guarded human-gate transition (DB1-backed).
$(TESTPREFIX)/unit-test-wfe-gate-apply: $(OBJDIR)/tests/test_wfe_gate_apply.o \
                                    $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                                    $(OBJDIR)/modules/db1/wfe_store.o $(OBJDIR)/aimee_home.o \
                                    $(OBJDIR)/wfe_store_lists.o \
                                    $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# intake-auth: per-principal submitter binding + concurrency/rate count helpers.
$(TESTPREFIX)/unit-test-wfe-submitter: $(OBJDIR)/tests/test_wfe_submitter.o \
                                    $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                                    $(OBJDIR)/modules/db1/wfe_store.o $(OBJDIR)/aimee_home.o \
                                    $(OBJDIR)/wfe_store_lists.o \
                                    $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Capability invariant for the gate route (compile-time _Static_assert).
$(TESTPREFIX)/unit-test-workflow-gate-caps: $(OBJDIR)/tests/test_workflow_gate_caps.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

# Workflow visual composer (W7): /v1/workflow read+author handlers.
# wf_api_triggers reads trigger rules through the module caller contract.
$(TESTPREFIX)/unit-test-wfe-webapi: $(OBJDIR)/tests/test_wfe_webapi.o \
                                    $(OBJDIR)/server/server_workflow_api.o \
                                    $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                                    $(OBJDIR)/modules/db1/wfe_store.o $(OBJDIR)/modules/workflows/wfe_def.o \
                                    $(OBJDIR)/wfe_store_lists.o \
                                    $(OBJDIR)/modules/workflows/wfe_iface.o $(OBJDIR)/modules/workflows/wfe_validate.o \
                                    $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o $(OBJDIR)/modules/workflows/wfe_custom.o \
                                    $(CONFIG_CLIENT_TEST_OBJS) $(OBJDIR)/util.o $(OBJDIR)/text.o \
                                    $(OBJDIR)/platform_random.o \
                                    $(OBJDIR)/aimee_home.o $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Workflow engine W4: HMAC approval signer + gate.human.
$(TESTPREFIX)/unit-test-wfe-approval: $(OBJDIR)/tests/test_wfe_approval.o \
                                      $(OBJDIR)/modules/workflows/wfe_approval.o $(OBJDIR)/modules/workflows/wfe_engine.o \
                                      $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                                      $(OBJDIR)/modules/db1/wfe_store.o $(OBJDIR)/modules/workflows/wfe_def.o \
                                      $(OBJDIR)/wfe_store_lists.o \
                                      $(OBJDIR)/modules/workflows/wfe_iface.o $(OBJDIR)/modules/workflows/wfe_validate.o \
                                      $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o $(OBJDIR)/modules/workflows/wfe_custom.o $(OBJDIR)/aimee_home.o \
                                      $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Workflow engine W5: roundtable verdict rule + gate.roundtable (mock panel).
$(TESTPREFIX)/unit-test-wfe-roundtable: $(OBJDIR)/tests/test_wfe_roundtable.o \
                                        $(OBJDIR)/modules/workflows/wfe_roundtable.o \
                                        $(OBJDIR)/modules/workflows/wfe_verdict.o $(OBJDIR)/module_json_call.o $(OBJDIR)/tests/support/module_bus_stub.o $(OBJDIR)/modules/workflows/wfe_engine.o \
                                        $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                                        $(OBJDIR)/modules/db1/wfe_store.o $(OBJDIR)/modules/workflows/wfe_def.o \
                                        $(OBJDIR)/wfe_store_lists.o \
                                        $(OBJDIR)/modules/workflows/wfe_iface.o $(OBJDIR)/modules/workflows/wfe_validate.o \
                                        $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o $(OBJDIR)/modules/workflows/wfe_custom.o $(OBJDIR)/aimee_home.o \
                                        $(OBJDIR)/util.o $(OBJDIR)/posix/util.o $(OBJDIR)/tests/support/log_stub.o \
                                        $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# foreach.workflow fan-in aggregation (DB parent<->child linkage) via the engine +
# a mock child-spawn provider.
$(TESTPREFIX)/unit-test-wfe-foreach: $(OBJDIR)/tests/test_wfe_foreach.o \
                                    $(OBJDIR)/modules/workflows/wfe_blocks.o $(OBJDIR)/modules/workflows/wfe_engine.o $(OBJDIR)/tests/support/config_autonomy_stub.o \
                                    $(OBJDIR)/modules/workflows/wfe_autonomy.o $(OBJDIR)/modules/workflows/wfe_approval.o \
                                    $(OBJDIR)/modules/workflows/wfe_roundtable.o $(OBJDIR)/modules/workflows/wfe_verdict.o $(OBJDIR)/module_json_call.o $(OBJDIR)/tests/support/module_bus_stub.o \
                                    $(OBJDIR)/tests/support/log_stub.o \
                                    $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                                    $(OBJDIR)/modules/db1/wfe_store.o $(OBJDIR)/modules/workflows/wfe_def.o \
                                    $(OBJDIR)/wfe_store_lists.o \
                                    $(OBJDIR)/modules/workflows/wfe_iface.o $(OBJDIR)/modules/workflows/wfe_validate.o \
                                    $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o $(OBJDIR)/modules/workflows/wfe_custom.o \
                                    $(OBJDIR)/modules/workflows/wfe_deliver.o $(OBJDIR)/modules/workflows/wfe_manager_artifacts.o \
                                    $(OBJDIR)/aimee_home.o $(OBJDIR)/util.o $(OBJDIR)/posix/util.o \
                                    $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Live panel: verified roundtable items -> per-lens wfe verdicts (pure mapper).
$(TESTPREFIX)/unit-test-wfe-panel-roundtable: $(OBJDIR)/tests/test_wfe_panel_roundtable.o \
                                    $(OBJDIR)/modules/workflows/wfe_panel_roundtable.o \
                                    $(OBJDIR)/modules/workflows/wfe_verdict.o $(OBJDIR)/module_json_call.o $(OBJDIR)/tests/support/module_bus_stub.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

# Live panel: the worktree-grounded evidence-replay backend.
$(TESTPREFIX)/unit-test-wfe-replay-worktree: $(OBJDIR)/tests/test_wfe_replay_worktree.o \
                              $(OBJDIR)/modules/workflows/wfe_replay_worktree.o $(OBJDIR)/server/evidence_replay.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto

# Live foreach spawner: a split packet-plan -> child slice work items (parent linkage).
$(TESTPREFIX)/unit-test-wfe-foreach-spawn: $(OBJDIR)/tests/test_wfe_foreach_spawn.o \
                                    $(OBJDIR)/modules/workflows/wfe_live_foreach.o \
                                    $(OBJDIR)/modules/workflows/wfe_blocks.o $(OBJDIR)/modules/workflows/wfe_engine.o $(OBJDIR)/tests/support/config_autonomy_stub.o \
                                    $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                                    $(OBJDIR)/modules/db1/wfe_store.o $(OBJDIR)/modules/workflows/wfe_def.o \
                                    $(OBJDIR)/wfe_store_lists.o \
                                    $(OBJDIR)/modules/workflows/wfe_iface.o $(OBJDIR)/modules/workflows/wfe_validate.o \
                                    $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o $(OBJDIR)/modules/workflows/wfe_custom.o \
                                    $(OBJDIR)/modules/workflows/wfe_deliver.o $(OBJDIR)/modules/workflows/wfe_manager_artifacts.o \
                                    $(OBJDIR)/aimee_home.o $(OBJDIR)/log.o $(OBJDIR)/util.o $(OBJDIR)/posix/util.o \
                                    $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Sliced-lifecycle build workflow: branch.open / foreach.workflow catalog typing +
# parent/child graph validation + version stability (pure def/validator; no engine).
$(TESTPREFIX)/unit-test-wfe-sliced-build: $(OBJDIR)/tests/test_wfe_sliced_build.o \
                                        $(OBJDIR)/modules/workflows/wfe_def.o $(OBJDIR)/modules/workflows/wfe_iface.o \
                                        $(OBJDIR)/modules/workflows/wfe_validate.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                                        $(OBJDIR)/modules/workflows/wfe_custom.o $(OBJDIR)/aimee_home.o \
                                        $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Workflow engine W6: autonomy driver + gate-override.
$(TESTPREFIX)/unit-test-wfe-autonomy: $(OBJDIR)/tests/test_wfe_autonomy.o \
                                      $(OBJDIR)/modules/workflows/wfe_autonomy.o $(OBJDIR)/modules/workflows/wfe_approval.o \
                                      $(OBJDIR)/tests/support/log_stub.o \
                                      $(OBJDIR)/tests/support/config_autonomy_stub.o \
                                      $(OBJDIR)/modules/workflows/wfe_roundtable.o $(OBJDIR)/modules/workflows/wfe_verdict.o $(OBJDIR)/module_json_call.o $(OBJDIR)/tests/support/module_bus_stub.o \
                                      $(OBJDIR)/modules/workflows/wfe_engine.o $(OBJDIR)/modules/db1/db1_init.o \
                                      $(OBJDIR)/modules/db1/db_schema.o $(OBJDIR)/modules/db1/wfe_store.o \
                                      $(OBJDIR)/wfe_store_lists.o \
                                      $(OBJDIR)/modules/workflows/wfe_def.o $(OBJDIR)/modules/workflows/wfe_iface.o \
                                      $(OBJDIR)/modules/workflows/wfe_validate.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o $(OBJDIR)/modules/workflows/wfe_custom.o \
                                      $(OBJDIR)/aimee_home.o $(OBJDIR)/util.o $(OBJDIR)/posix/util.o \
                                      $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o \
                                      $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-provider-catalog: $(OBJDIR)/tests/test_provider_catalog.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-trace-analysis: $(OBJDIR)/tests/test_trace_analysis.o $(TEST_DATA_OBJS_MOCK) \
                                $(OBJDIR)/trace_analysis.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-cmd-branch: $(OBJDIR)/tests/test_cmd_branch.o $(OBJDIR)/cmd_branch.o \
                           $(OBJDIR)/cmd_util.o $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA) \
                           $(OBJDIR)/modules/git/mcp_git_query.o $(OBJDIR)/tests/support/git_cred_inject_stub.o $(OBJDIR)/modules/git/forge_credentials.o $(OBJDIR)/modules/git/git_host_resolve.o $(OBJDIR)/modules/git/mcp_git_write.o $(OBJDIR)/modules/git/mcp_git_integrate.o \
                           $(OBJDIR)/modules/git/mcp_git_branch.o $(OBJDIR)/modules/git/mcp_git_pr.o $(OBJDIR)/tests/support/git_pr_api_stub.o $(OBJDIR)/modules/git/git_pr_ci_grade.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-cmd-core: $(OBJDIR)/tests/test_cmd_core.o $(TEST_DATA_OBJS) \
                         $(TEST_WORKSPACE_OBJS_EXTRA) \
                         $(OBJDIR)/cmd_util.o \
                         $(OBJDIR)/cmd_infra.o \
                         $(OBJDIR)/cmd_init.o \
                         $(OBJDIR)/modules/git/mcp_git_query.o $(OBJDIR)/tests/support/git_cred_inject_stub.o $(OBJDIR)/modules/git/forge_credentials.o $(OBJDIR)/modules/git/git_host_resolve.o $(OBJDIR)/modules/git/mcp_git_write.o $(OBJDIR)/modules/git/mcp_git_integrate.o \
                         $(OBJDIR)/modules/git/mcp_git_branch.o $(OBJDIR)/modules/git/mcp_git_pr.o $(OBJDIR)/tests/support/git_pr_api_stub.o $(OBJDIR)/modules/git/git_pr_ci_grade.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-client-integrations: $(OBJDIR)/tests/test_client_integrations.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-mcp-git: $(OBJDIR)/tests/test_mcp_git.o $(OBJDIR)/tests/support/git_module_fixture.o $(OBJDIR)/modules/git/mcp_git_query.o $(OBJDIR)/tests/support/git_cred_inject_stub.o $(OBJDIR)/modules/git/forge_credentials.o $(OBJDIR)/modules/git/git_host_resolve.o $(OBJDIR)/modules/git/mcp_git_write.o $(OBJDIR)/modules/git/mcp_git_integrate.o \
                        $(OBJDIR)/modules/git/mcp_git_branch.o $(OBJDIR)/modules/git/mcp_git_pr.o $(OBJDIR)/tests/support/git_pr_api_stub.o $(OBJDIR)/modules/git/git_pr_ci_grade.o $(OBJDIR)/modules/git/git_verify.o $(OBJDIR)/modules/git/git_verify_state.o $(OBJDIR)/modules/git/git_verify_config.o \
                        $(OBJDIR)/modules/git/git_verify_jobs.o $(OBJDIR)/modules/git/git_verify_hook.o $(OBJDIR)/modules/git/git_verify_ops.o \
                        $(OBJDIR)/modules/git/git_verify_select.o $(OBJDIR)/modules/git/git_verify_step.o $(OBJDIR)/server/compute_pool.o \
                        $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-git-verify-select: $(OBJDIR)/tests/test_git_verify_select.o \
                        $(OBJDIR)/modules/git/git_verify_select.o $(OBJDIR)/modules/git/git_verify.o $(OBJDIR)/modules/git/git_verify_state.o $(OBJDIR)/modules/git/git_verify_config.o \
                        $(OBJDIR)/modules/git/git_verify_jobs.o $(OBJDIR)/modules/git/git_verify_hook.o $(OBJDIR)/modules/git/git_verify_ops.o \
                        $(OBJDIR)/modules/git/git_verify_step.o $(OBJDIR)/server/compute_pool.o \
                        $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-git-verify-contract: $(OBJDIR)/tests/test_git_verify_contract.o \
                        $(OBJDIR)/modules/git/git_verify.o $(OBJDIR)/modules/git/git_verify_state.o $(OBJDIR)/modules/git/git_verify_config.o \
                        $(OBJDIR)/modules/git/git_verify_jobs.o $(OBJDIR)/modules/git/git_verify_hook.o $(OBJDIR)/modules/git/git_verify_ops.o \
                        $(OBJDIR)/modules/git/git_verify_select.o $(OBJDIR)/modules/git/git_verify_step.o $(OBJDIR)/server/compute_pool.o \
                        $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-interaction-events: $(OBJDIR)/tests/test_interaction_events.o \
                               $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                               $(OBJDIR)/modules/db1/maintenance.o $(OBJDIR)/modules/db1/interaction_events.o \
                               $(OBJDIR)/log.o $(OBJDIR)/util.o $(OBJDIR)/cJSON.o \
                               $(PLATFORM_BASIC_OBJS) $(OBJDIR)/interaction_event_names.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-trajectory: $(OBJDIR)/tests/test_trajectory.o \
                               $(OBJDIR)/trajectory_export.o $(OBJDIR)/modules/audit/audit_ledger.o \
                               $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                               $(OBJDIR)/modules/db1/maintenance.o $(OBJDIR)/modules/db1/interaction_events.o \
                               $(OBJDIR)/posix/memory.o \
                               $(CONFIG_CLIENT_TEST_OBJS) $(OBJDIR)/aimee_home.o \
                               $(OBJDIR)/log.o $(OBJDIR)/util.o $(OBJDIR)/text.o $(OBJDIR)/cJSON.o \
                               $(PLATFORM_BASIC_OBJS) $(OBJDIR)/interaction_event_names.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-trajectory-batch: $(OBJDIR)/tests/test_trajectory_batch.o \
                               $(OBJDIR)/trajectory_batch.o $(OBJDIR)/trajectory_export.o $(OBJDIR)/modules/audit/audit_ledger.o \
                               $(CONFIG_CLIENT_TEST_OBJS) \
                               $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                               $(OBJDIR)/modules/db1/maintenance.o $(OBJDIR)/modules/db1/interaction_events.o \
                               $(OBJDIR)/posix/memory.o \
                               $(OBJDIR)/log.o $(OBJDIR)/util.o $(OBJDIR)/text.o $(OBJDIR)/yaml.o $(OBJDIR)/aimee_home.o $(OBJDIR)/cJSON.o \
                               $(PLATFORM_BASIC_OBJS) $(OBJDIR)/interaction_event_names.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-platform-process: $(OBJDIR)/tests/test_platform_process.o \
                                  $(OBJDIR)/posix/platform_process.o \
                                  $(OBJDIR)/linux/platform_process.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-shutdown-forensics: $(OBJDIR)/tests/test_shutdown_forensics.o \
                                  $(OBJDIR)/shutdown_forensics.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-dstr: $(OBJDIR)/tests/test_dstr.o $(OBJDIR)/dstr.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

# The aimee-client test compiles its in-process TLS mock only in WITH_TLS builds.
$(OBJDIR)/tests/test_aimee_client.o: C_FLAGS += $(TLS_FLAGS)
$(OBJDIR)/tests/test_kb_graph.o: C_FLAGS += -Ikb
$(OBJDIR)/tests/test_kb_graph_analytics.o: C_FLAGS += -Ikb
$(OBJDIR)/tests/test_lessons_cite_tracker.o: C_FLAGS += -Ikb
$(OBJDIR)/tests/test_lessons_reflect.o: C_FLAGS += -Ikb
$(OBJDIR)/tests/test_lessons_actuate.o: C_FLAGS += -Ikb
$(OBJDIR)/tests/test_lessons_session_capture.o: C_FLAGS += -Ikb
$(OBJDIR)/tests/test_prompt_sanitizer.o: C_FLAGS += -Ikb

$(TESTPREFIX)/unit-test-kb-graph: $(OBJDIR)/tests/test_kb_graph.o \
                                  $(OBJDIR)/kb/kb_service_graph.o \
                                  $(OBJDIR)/kb/kb_graph_analytics.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(L_CORE)

# Reciprocal Rank Fusion core (§5 hybrid retrieval scoring model). Pure: no DB.
$(TESTPREFIX)/unit-test-kb-rrf: $(OBJDIR)/tests/test_kb_rrf.o $(OBJDIR)/rrf.o
	$(TESTLINK) -o $@ $^ $(L_CORE) -lm

# Graph analytics: degree-centrality hub ranking (§4). Pure: no DB.
$(TESTPREFIX)/unit-test-kb-graph-analytics: $(OBJDIR)/tests/test_kb_graph_analytics.o \
                                            $(OBJDIR)/kb/kb_graph_analytics.o
	$(TESTLINK) -o $@ $^ $(L_CORE)

# graph-feedback §3 / S3a-api: the auto-`useful` cite-again proxy. Pure: no DB.
$(TESTPREFIX)/unit-test-lessons-cite-tracker: $(OBJDIR)/tests/test_lessons_cite_tracker.o \
                                              $(OBJDIR)/kb/lessons_cite_tracker.o
	$(TESTLINK) -o $@ $^ $(L_CORE)

$(TESTPREFIX)/unit-test-lessons-reflect: $(OBJDIR)/tests/test_lessons_reflect.o \
                                         $(OBJDIR)/kb/lessons_reflect.o
	$(TESTLINK) -o $@ $^ $(L_CORE) -lm

$(TESTPREFIX)/unit-test-lessons-actuate: $(OBJDIR)/tests/test_lessons_actuate.o \
                                         $(OBJDIR)/kb/lessons_actuate.o
	$(TESTLINK) -o $@ $^ $(L_CORE)

$(TESTPREFIX)/unit-test-lessons-session-capture: $(OBJDIR)/tests/test_lessons_session_capture.o \
                                                 $(OBJDIR)/kb/lessons_session_capture.o \
                                                 $(OBJDIR)/kb/lessons_cite_tracker.o
	$(TESTLINK) -o $@ $^ $(L_CORE) -lpthread

$(TESTPREFIX)/unit-test-kb-doc-hash: $(OBJDIR)/tests/test_kb_doc_hash.o \
                                     $(OBJDIR)/kb/kb_doc_hash.o
	$(TESTLINK) -o $@ $^ $(L_CORE) -lcrypto

# Event-bus wire codec (feature tree slice 1). Pure: no DB, no shared memory.
# BUS_VECTOR_DIR points at the committed golden vectors so the binary can run
# from any cwd; those bytes are the cross-language conformance authority (D8).
# The main Makefile scopes -Icore/event_bus/include to the bus objects. Test
# translation units that exercise the public contract opt in below.
$(OBJDIR)/tests/test_bus_wire.o: C_FLAGS += -Icore/event_bus/include -DBUS_VECTOR_DIR=\"$(CURDIR)/tests/fixtures/bus\"
$(TESTPREFIX)/unit-test-bus-wire: $(OBJDIR)/tests/test_bus_wire.o \
                                  $(OBJDIR)/core/event_bus/bus_wire.o
	$(TESTLINK_MIN) -o $@ $^ $(EXTRA_L_FLAGS)

$(OBJDIR)/tests/test_module_protocol.o: C_FLAGS += -Icore/event_bus/include
$(TESTPREFIX)/unit-test-module-protocol: $(OBJDIR)/tests/test_module_protocol.o \
                                         $(OBJDIR)/core/event_bus/module_protocol.o
	$(TESTLINK_MIN) -o $@ $^ $(EXTRA_L_FLAGS)

# Event-bus SPSC ring (feature tree slice 2). Pure: no DB, no shared memory —
# the ring lives in caller-supplied memory, which is what lets it land before
# the region layout is settled.
$(OBJDIR)/tests/test_bus_ring.o: C_FLAGS += -Icore/event_bus/include
$(TESTPREFIX)/unit-test-bus-ring: $(OBJDIR)/tests/test_bus_ring.o \
                                  $(OBJDIR)/core/event_bus/bus_ring.o
	$(TESTLINK_MIN) -o $@ $^ $(EXTRA_L_FLAGS) -lpthread

# Event-bus region layout (feature tree slice 3). Uses memfd_create + mmap; no
# DB. Links the ring (slice 2) since a queue-pair region contains two rings.
$(OBJDIR)/tests/test_bus_region.o: C_FLAGS += -Icore/event_bus/include
$(TESTPREFIX)/unit-test-bus-region: $(OBJDIR)/tests/test_bus_region.o \
                                    $(OBJDIR)/core/event_bus/bus_region.o $(OBJDIR)/core/event_bus/bus_region_host.o \
                                    $(OBJDIR)/core/event_bus/bus_ring.o
	$(TESTLINK_MIN) -o $@ $^ $(EXTRA_L_FLAGS)

# Event-bus arena lease allocator (feature tree slice 4). Host-private; no shared
# memory of its own, no DB — it manages spans over a caller-supplied buffer.
$(OBJDIR)/tests/test_bus_arena.o: C_FLAGS += -Icore/event_bus/include
$(TESTPREFIX)/unit-test-bus-arena: $(OBJDIR)/tests/test_bus_arena.o \
                                   $(OBJDIR)/core/event_bus/bus_arena.o
	$(TESTLINK_MIN) -o $@ $^ $(EXTRA_L_FLAGS)

# Event-bus host: admission, fd passing, reaping (feature tree slice 5). Uses
# memfd + socketpair; no DB. Links region, ring and arena.
$(OBJDIR)/tests/test_bus_host.o: C_FLAGS += -Icore/event_bus/include
$(TESTPREFIX)/unit-test-bus-host: $(OBJDIR)/tests/test_bus_host.o \
                                  $(OBJDIR)/core/event_bus/bus_attach.o \
                                  $(OBJDIR)/core/event_bus/bus_host.o \
                                  $(OBJDIR)/core/event_bus/bus_route.o \
                                  $(OBJDIR)/core/event_bus/bus_region.o $(OBJDIR)/core/event_bus/bus_region_host.o \
                                  $(OBJDIR)/core/event_bus/bus_ring.o \
                                  $(OBJDIR)/core/event_bus/bus_arena.o \
                                  $(OBJDIR)/core/event_bus/bus_wire.o
	$(TESTLINK_MIN) -o $@ $^ $(EXTRA_L_FLAGS)

# Event-bus routing + tap (feature tree slice 6).
$(OBJDIR)/tests/test_bus_route.o: C_FLAGS += -Icore/event_bus/include
$(TESTPREFIX)/unit-test-bus-route: $(OBJDIR)/tests/test_bus_route.o \
                                   $(OBJDIR)/core/event_bus/bus_attach.o \
                                   $(OBJDIR)/core/event_bus/bus_host.o \
                                   $(OBJDIR)/core/event_bus/bus_route.o \
                                   $(OBJDIR)/core/event_bus/bus_region.o $(OBJDIR)/core/event_bus/bus_region_host.o \
                                   $(OBJDIR)/core/event_bus/bus_ring.o \
                                   $(OBJDIR)/core/event_bus/bus_arena.o \
                                   $(OBJDIR)/core/event_bus/bus_wire.o
	$(TESTLINK_MIN) -o $@ $^ $(EXTRA_L_FLAGS)

# Event-bus flow control (feature tree slice 7).
$(OBJDIR)/tests/test_bus_flow.o: C_FLAGS += -Icore/event_bus/include
$(TESTPREFIX)/unit-test-bus-flow: $(OBJDIR)/tests/test_bus_flow.o \
                                  $(OBJDIR)/core/event_bus/bus_attach.o \
                                  $(OBJDIR)/core/event_bus/bus_host.o \
                                  $(OBJDIR)/core/event_bus/bus_route.o \
                                  $(OBJDIR)/core/event_bus/bus_region.o $(OBJDIR)/core/event_bus/bus_region_host.o \
                                  $(OBJDIR)/core/event_bus/bus_ring.o \
                                  $(OBJDIR)/core/event_bus/bus_arena.o \
                                  $(OBJDIR)/core/event_bus/bus_wire.o
	$(TESTLINK_MIN) -o $@ $^ $(EXTRA_L_FLAGS)

# Event-bus C reference client (feature tree slice 8).
$(OBJDIR)/tests/test_bus_client.o: C_FLAGS += -Icore/event_bus/include
$(TESTPREFIX)/unit-test-bus-client: $(OBJDIR)/tests/test_bus_client.o \
                                    $(OBJDIR)/core/event_bus/bus_client.o \
                                    $(OBJDIR)/core/event_bus/bus_attach.o \
                                    $(OBJDIR)/core/event_bus/bus_host.o \
                                    $(OBJDIR)/core/event_bus/bus_route.o \
                                    $(OBJDIR)/core/event_bus/bus_region.o $(OBJDIR)/core/event_bus/bus_region_host.o \
                                    $(OBJDIR)/core/event_bus/bus_ring.o \
                                    $(OBJDIR)/core/event_bus/bus_arena.o \
                                    $(OBJDIR)/core/event_bus/bus_wire.o
	$(TESTLINK_MIN) -o $@ $^ $(EXTRA_L_FLAGS) -lpthread

$(OBJDIR)/tests/test_bus_endpoint.o: C_FLAGS += -Icore/event_bus/include
$(TESTPREFIX)/unit-test-bus-endpoint: $(OBJDIR)/tests/test_bus_endpoint.o \
                                      $(OBJDIR)/core/event_bus/bus_endpoint.o
	$(TESTLINK_MIN) -o $@ $^ $(EXTRA_L_FLAGS)

.PHONY: unit-test-bus-endpoint
unit-test-bus-endpoint: $(TESTPREFIX)/unit-test-bus-endpoint
	$<

$(OBJDIR)/tests/test_bus_runtime.o: C_FLAGS += -Icore/event_bus/include
$(TESTPREFIX)/unit-test-bus-runtime: $(OBJDIR)/tests/test_bus_runtime.o \
                                     $(OBJDIR)/core/event_bus/bus_runtime.o \
                                     $(OBJDIR)/core/event_bus/bus_endpoint.o \
                                     $(OBJDIR)/core/event_bus/bus_client.o \
                                     $(OBJDIR)/core/event_bus/bus_attach.o \
                                     $(OBJDIR)/core/event_bus/bus_host.o \
                                     $(OBJDIR)/core/event_bus/bus_route.o \
                                     $(OBJDIR)/core/event_bus/bus_region.o \
                                     $(OBJDIR)/core/event_bus/bus_region_host.o \
                                     $(OBJDIR)/core/event_bus/bus_ring.o \
                                     $(OBJDIR)/core/event_bus/bus_arena.o \
                                     $(OBJDIR)/core/event_bus/bus_wire.o
	$(TESTLINK_MIN) -o $@ $^ $(EXTRA_L_FLAGS) -lpthread

.PHONY: unit-test-bus-runtime
unit-test-bus-runtime: $(TESTPREFIX)/unit-test-bus-runtime
	$<

$(OBJDIR)/tests/test_module_runtime.o: C_FLAGS += -Icore/event_bus/include
$(TESTPREFIX)/unit-test-module-runtime: $(OBJDIR)/tests/test_module_runtime.o \
                                        $(OBJDIR)/core/event_bus/module_client.o \
                                        $(OBJDIR)/core/event_bus/module_runtime.o \
                                        $(OBJDIR)/core/event_bus/module_protocol.o \
                                        $(OBJDIR)/core/event_bus/bus_runtime.o \
                                        $(OBJDIR)/core/event_bus/bus_endpoint.o \
                                        $(OBJDIR)/core/event_bus/bus_client.o \
                                        $(OBJDIR)/core/event_bus/bus_attach.o \
                                        $(OBJDIR)/core/event_bus/bus_host.o \
                                        $(OBJDIR)/core/event_bus/bus_route.o \
                                        $(OBJDIR)/core/event_bus/bus_region.o \
                                        $(OBJDIR)/core/event_bus/bus_region_host.o \
                                        $(OBJDIR)/core/event_bus/bus_ring.o \
                                        $(OBJDIR)/core/event_bus/bus_arena.o \
                                        $(OBJDIR)/core/event_bus/bus_wire.o
	$(TESTLINK_MIN) -o $@ $^ $(EXTRA_L_FLAGS) -lpthread

$(OBJDIR)/tests/test_sandbox_learned_observe.o: C_FLAGS += -Icore/event_bus/include -Imodules/sandbox/include
$(OBJDIR)/tests/test_sandbox_pkg_proxy_adapter.o: C_FLAGS += -Icore/event_bus/include -Imodules/sandbox/include
$(OBJDIR)/tests/test_module_json_call.o: C_FLAGS += -Icore/event_bus/include
$(TESTPREFIX)/unit-test-module-json-call: $(OBJDIR)/tests/test_module_json_call.o \
                                        $(OBJDIR)/module_json_call.o \
                                        $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(EXTRA_L_FLAGS) -lpthread

unit-test-module-json-call: $(TESTPREFIX)/unit-test-module-json-call
	$<

$(TESTPREFIX)/unit-test-sandbox-pkg-proxy-adapter: \
                                        $(OBJDIR)/tests/test_sandbox_pkg_proxy_adapter.o \
                                        $(OBJDIR)/modules/sandbox/sandbox_pkg_proxy.o \
                                        $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(EXTRA_L_FLAGS)

.PHONY: unit-test-sandbox-pkg-proxy-adapter
unit-test-sandbox-pkg-proxy-adapter: $(TESTPREFIX)/unit-test-sandbox-pkg-proxy-adapter
	$<

$(OBJDIR)/tests/test_economizer_module_client.o: C_FLAGS += -Icore/event_bus/include
$(TESTPREFIX)/unit-test-economizer-module-client: \
                                        $(OBJDIR)/tests/test_economizer_module_client.o \
                                        $(OBJDIR)/modules/economizer/economizer_module_client.o \
                                        $(OBJDIR)/module_json_call.o $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(EXTRA_L_FLAGS) -lpthread

unit-test-economizer-module-client: $(TESTPREFIX)/unit-test-economizer-module-client
	$<

$(TESTPREFIX)/unit-test-sandbox-learned-observe: $(OBJDIR)/tests/test_sandbox_learned_observe.o \
                                        $(OBJDIR)/modules/sandbox/sandbox_learned.o $(OBJDIR)/module_json_call.o \
                                        $(OBJDIR)/modules/guardrails/guardrails_tdd.o \
                                        $(OBJDIR)/util.o \
                                        $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(EXTRA_L_FLAGS) -lpthread

.PHONY: unit-test-sandbox-learned-observe
unit-test-sandbox-learned-observe: $(TESTPREFIX)/unit-test-sandbox-learned-observe
	$<

.PHONY: unit-test-module-runtime
unit-test-module-runtime: $(TESTPREFIX)/unit-test-module-runtime
	$<

$(OBJDIR)/tests/test_routing_module.o: C_FLAGS += -Icore/event_bus/include -Imodules/routing/include
$(OBJDIR)/modules/routing/module_adapter.o: C_FLAGS += -Icore/event_bus/include -Imodules/routing/include
$(TESTPREFIX)/unit-test-routing-module: $(OBJDIR)/tests/test_routing_module.o \
                                        $(OBJDIR)/modules/routing/module_adapter.o
	$(TESTLINK_MIN) -o $@ $^ $(EXTRA_L_FLAGS)

.PHONY: unit-test-routing-module
unit-test-routing-module: $(TESTPREFIX)/unit-test-routing-module
	$<

$(OBJDIR)/tests/test_db2_module_contract.o: C_FLAGS += -Icore/event_bus/include \
                                                        -Imodules/db2 \
                                                        -Imodules/db2/include
$(OBJDIR)/modules/db2/module_adapter.o: C_FLAGS += -Icore/event_bus/include \
                                                   -Imodules/db2/include
$(TESTPREFIX)/unit-test-db2-module-contract: \
                                        $(OBJDIR)/tests/test_db2_module_contract.o \
                                        $(OBJDIR)/modules/db2/client/generated.o \
                                        $(OBJDIR)/modules/db2/module_adapter.o
	$(TESTLINK_MIN) -o $@ $^ $(EXTRA_L_FLAGS)

.PHONY: unit-test-db2-module-contract
unit-test-db2-module-contract: $(TESTPREFIX)/unit-test-db2-module-contract
	$<

$(OBJDIR)/tests/test_db2_module_init.o: C_FLAGS += -Imodules/db2
$(OBJDIR)/modules/db2/module_init.o: C_FLAGS += -Imodules/db2 -Imodules/db2/c -Iheaders
$(TESTPREFIX)/unit-test-db2-module-init: $(OBJDIR)/tests/test_db2_module_init.o \
                                        $(OBJDIR)/modules/db2/module_init.o
	$(TESTLINK_MIN) -o $@ $^ $(EXTRA_L_FLAGS)

.PHONY: unit-test-db2-module-init
unit-test-db2-module-init: $(TESTPREFIX)/unit-test-db2-module-init
	$<

$(OBJDIR)/tests/test_bus_db2_module.o: C_FLAGS += -Icore/event_bus/include \
                                                   -Imodules/db2 \
                                                   -Imodules/db2/include
$(OBJDIR)/modules/db2/client/generated.o: C_FLAGS += -Icore/event_bus/include \
                                                        -Imodules/db2/include
$(TESTPREFIX)/unit-test-bus-db2-module: \
                                        $(OBJDIR)/tests/test_bus_db2_module.o \
                                        $(OBJDIR)/modules/db2/client/generated.o \
                                        $(OBJDIR)/modules/db2/module_adapter.o \
                                        $(OBJDIR)/core/event_bus/module_client.o \
                                        $(OBJDIR)/core/event_bus/module_runtime.o \
                                        $(OBJDIR)/core/event_bus/module_protocol.o \
                                        $(OBJDIR)/core/event_bus/bus_runtime.o \
                                        $(OBJDIR)/core/event_bus/bus_endpoint.o \
                                        $(OBJDIR)/core/event_bus/bus_client.o \
                                        $(OBJDIR)/core/event_bus/bus_attach.o \
                                        $(OBJDIR)/core/event_bus/bus_host.o \
                                        $(OBJDIR)/core/event_bus/bus_route.o \
                                        $(OBJDIR)/core/event_bus/bus_region.o \
                                        $(OBJDIR)/core/event_bus/bus_region_host.o \
                                        $(OBJDIR)/core/event_bus/bus_ring.o \
                                        $(OBJDIR)/core/event_bus/bus_arena.o \
                                        $(OBJDIR)/core/event_bus/bus_wire.o
	$(TESTLINK_MIN) -o $@ $^ $(EXTRA_L_FLAGS) -lpthread

.PHONY: unit-test-bus-db2-module
unit-test-bus-db2-module: $(TESTPREFIX)/unit-test-bus-db2-module
	$<

$(OBJDIR)/tests/test_bus_db2_process.o: C_FLAGS += -Icore/event_bus/include \
                                                    -Imodules/db2/include
$(TESTPREFIX)/unit-test-bus-db2-process: \
                                        $(OBJDIR)/tests/test_bus_db2_process.o \
                                        $(OBJDIR)/modules/db2/client/generated.o \
                                        $(OBJDIR)/core/event_bus/module_client.o \
                                        $(OBJDIR)/core/event_bus/module_protocol.o \
                                        $(OBJDIR)/core/event_bus/bus_runtime.o \
                                        $(OBJDIR)/core/event_bus/bus_endpoint.o \
                                        $(OBJDIR)/core/event_bus/bus_client.o \
                                        $(OBJDIR)/core/event_bus/bus_attach.o \
                                        $(OBJDIR)/core/event_bus/bus_host.o \
                                        $(OBJDIR)/core/event_bus/bus_route.o \
                                        $(OBJDIR)/core/event_bus/bus_region.o \
                                        $(OBJDIR)/core/event_bus/bus_region_host.o \
                                        $(OBJDIR)/core/event_bus/bus_ring.o \
                                        $(OBJDIR)/core/event_bus/bus_arena.o \
                                        $(OBJDIR)/core/event_bus/bus_wire.o
	$(TESTLINK_MIN) -o $@ $^ $(EXTRA_L_FLAGS) -lpthread

.PHONY: db2-replay
db2-replay: $(TESTPREFIX)/unit-test-bus-db2-process $(OBJDIR)/aimee-module-db2-replay
	@test -n "$$AIMEE_DB2_URL" || { echo "db2-replay requires AIMEE_DB2_URL" >&2; exit 1; }
	$< $(abspath $(OBJDIR)/aimee-module-db2-replay)

# --- Postgres-backed unit tests -------------------------------------------
#
# The sqlite shim translates DB2's SQL rather than executing it, so engine-level
# behaviour (affected-row counts, scope-macro expansion, postgres-only syntax) is
# unverified by a normal `make unit-tests`. Point AIMEE_TEST_DB2_TEMPLATE_URL at a
# postgres database and db2_test_shim_open* clones it per test binary instead of
# opening sqlite -- the same tests, the real engine.
#
#   make db2-test-template AIMEE_TEST_DB2_TEMPLATE_URL=postgresql://.../aimee_test_tpl
#   make unit-tests-pg     AIMEE_TEST_DB2_TEMPLATE_URL=postgresql://.../aimee_test_tpl
$(TESTPREFIX)/db2-test-template: $(OBJDIR)/tests/db2_test_template.o \
                            $(DB2_OBJS) $(OBJDIR)/db2/db_postgres.o \
                            $(OBJDIR)/log.o $(OBJDIR)/util.o
	$(TESTLINK_MIN) -o $@ $^ $(PQ_LIB) -lpthread -lm $(EXTRA_L_FLAGS)

.PHONY: db2-test-template
db2-test-template: $(TESTPREFIX)/db2-test-template
	@test -n "$$AIMEE_TEST_DB2_TEMPLATE_URL" || \
	  { echo "db2-test-template requires AIMEE_TEST_DB2_TEMPLATE_URL" >&2; exit 1; }
	$< "$$AIMEE_TEST_DB2_TEMPLATE_URL" tests/db2_test_reset.sql

# Rebuild the template, then run the suite against it. The template is rebuilt
# every time on purpose: a schema change that never reached the template would
# otherwise be tested against a stale copy, which is the failure mode this whole
# mode exists to close.
#
# AIMEE_TEST_PG=1 is set here, not left to the caller. The template URL alone only
# tells the shim to clone; the BACKEND is the link-time choice above, so without it
# the suite would build against the sqlite shim and then hand it a postgres URL to
# open as a file. Passing it as a command-line variable to the sub-make overrides
# whatever the environment says, so `make unit-tests-pg URL=...` means one thing.
# Shard variables reach the inner unit-tests through MAKEFLAGS as usual.
.PHONY: unit-tests-pg
unit-tests-pg:
	@test -n "$$AIMEE_TEST_DB2_TEMPLATE_URL" || \
	  { echo "unit-tests-pg requires AIMEE_TEST_DB2_TEMPLATE_URL" >&2; exit 1; }
	$(MAKE) AIMEE_TEST_PG=1 db2-test-template
	$(MAKE) AIMEE_TEST_PG=1 unit-tests

$(OBJDIR)/tests/test_db3_route.o: C_FLAGS += -Imodules/db2/include
$(OBJDIR)/modules/db2/db3_route.o: C_FLAGS += -Imodules/db2/include
$(TESTPREFIX)/unit-test-db3-route: $(OBJDIR)/tests/test_db3_route.o \
                                      $(OBJDIR)/modules/db2/db3_route.o
	$(TESTLINK_MIN) -o $@ $^ $(EXTRA_L_FLAGS) -lm

.PHONY: unit-test-db3-route
unit-test-db3-route: $(TESTPREFIX)/unit-test-db3-route
	$<

$(OBJDIR)/tests/test_bus_db3.o: C_FLAGS += -Icore/event_bus/include -Imodules/db2/include
$(TESTPREFIX)/unit-test-bus-db3: $(OBJDIR)/tests/test_bus_db3.o \
                                $(OBJDIR)/modules/db2/db3_route.o \
                                $(OBJDIR)/core/event_bus/bus_runtime.o \
                                $(OBJDIR)/core/event_bus/bus_endpoint.o \
                                $(OBJDIR)/core/event_bus/bus_client.o \
                                $(OBJDIR)/core/event_bus/bus_attach.o \
                                $(OBJDIR)/core/event_bus/bus_host.o \
                                $(OBJDIR)/core/event_bus/bus_route.o \
                                $(OBJDIR)/core/event_bus/bus_region.o \
                                $(OBJDIR)/core/event_bus/bus_region_host.o \
                                $(OBJDIR)/core/event_bus/bus_ring.o \
                                $(OBJDIR)/core/event_bus/bus_arena.o \
                                $(OBJDIR)/core/event_bus/bus_wire.o
	$(TESTLINK_MIN) -o $@ $^ $(EXTRA_L_FLAGS) -lpthread -lm

.PHONY: unit-test-bus-db3
unit-test-bus-db3: $(TESTPREFIX)/unit-test-bus-db3
	$<

# Event-bus conformance host harness (feature tree slice 10). A test binary that
# exposes the C host on a Unix socket so the Go reference client can interoperate
# with it across a process boundary. Never linked into a shipping target.
$(OBJDIR)/tests/bus_conformance_host.o: C_FLAGS += -Icore/event_bus/include
$(TESTPREFIX)/bus-conformance-host: $(OBJDIR)/tests/bus_conformance_host.o \
                                    $(OBJDIR)/core/event_bus/bus_client.o \
                                    $(OBJDIR)/core/event_bus/bus_attach.o \
                                    $(OBJDIR)/core/event_bus/bus_host.o \
                                    $(OBJDIR)/core/event_bus/bus_route.o \
                                    $(OBJDIR)/core/event_bus/bus_region.o $(OBJDIR)/core/event_bus/bus_region_host.o \
                                    $(OBJDIR)/core/event_bus/bus_ring.o \
                                    $(OBJDIR)/core/event_bus/bus_arena.o \
                                    $(OBJDIR)/core/event_bus/bus_wire.o
	$(TESTLINK_MIN) -o $@ $^ $(EXTRA_L_FLAGS) -lpthread

.PHONY: bus-conformance-host
bus-conformance-host: $(TESTPREFIX)/bus-conformance-host

# Strict C host for the Go DB3 provider/router interoperability test. The host
# owns transport only; every provider, selection, fallback, and idempotency
# decision in the test runs in Go over authenticated grants.
$(OBJDIR)/tests/test_bus_db3_go_host.o: C_FLAGS += -Icore/event_bus/include -Imodules/db2/include
$(TESTPREFIX)/db3-go-host: $(OBJDIR)/tests/test_bus_db3_go_host.o \
                            $(OBJDIR)/core/event_bus/bus_runtime.o \
                            $(OBJDIR)/core/event_bus/bus_endpoint.o \
                            $(OBJDIR)/core/event_bus/bus_attach.o \
                            $(OBJDIR)/core/event_bus/bus_host.o \
                            $(OBJDIR)/core/event_bus/bus_route.o \
                            $(OBJDIR)/core/event_bus/bus_region.o \
                            $(OBJDIR)/core/event_bus/bus_region_host.o \
                            $(OBJDIR)/core/event_bus/bus_ring.o \
                            $(OBJDIR)/core/event_bus/bus_arena.o \
                            $(OBJDIR)/core/event_bus/bus_wire.o
	$(TESTLINK_MIN) -o $@ $^ $(EXTRA_L_FLAGS) -lpthread

.PHONY: db3-go-host
db3-go-host: $(TESTPREFIX)/db3-go-host

# Event-bus capture + observational replay (feature tree slice 11).
$(OBJDIR)/tests/test_bus_capture.o: C_FLAGS += -Icore/event_bus/include
$(TESTPREFIX)/unit-test-bus-capture: $(OBJDIR)/tests/test_bus_capture.o \
                                     $(OBJDIR)/core/event_bus/bus_capture.o \
                                     $(OBJDIR)/core/event_bus/bus_client.o \
                                     $(OBJDIR)/core/event_bus/bus_attach.o \
                                     $(OBJDIR)/core/event_bus/bus_host.o \
                                     $(OBJDIR)/core/event_bus/bus_route.o \
                                     $(OBJDIR)/core/event_bus/bus_region.o $(OBJDIR)/core/event_bus/bus_region_host.o \
                                     $(OBJDIR)/core/event_bus/bus_ring.o \
                                     $(OBJDIR)/core/event_bus/bus_arena.o \
                                     $(OBJDIR)/core/event_bus/bus_wire.o
	$(TESTLINK_MIN) -o $@ $^ $(EXTRA_L_FLAGS) -lpthread

# Event-bus dispatch benchmark (feature tree slice 12). Built -O2 for a realistic
# measurement; a test binary, never linked into a shipping target.
$(OBJDIR)/tests/bus_bench.o: C_FLAGS += -Icore/event_bus/include -O2
$(TESTPREFIX)/bus-bench: $(OBJDIR)/tests/bus_bench.o \
                        $(OBJDIR)/core/event_bus/bus_client.o \
                        $(OBJDIR)/core/event_bus/bus_attach.o \
                        $(OBJDIR)/core/event_bus/bus_host.o \
                        $(OBJDIR)/core/event_bus/bus_route.o \
                        $(OBJDIR)/core/event_bus/bus_region.o $(OBJDIR)/core/event_bus/bus_region_host.o \
                        $(OBJDIR)/core/event_bus/bus_ring.o \
                        $(OBJDIR)/core/event_bus/bus_arena.o \
                        $(OBJDIR)/core/event_bus/bus_wire.o
	$(TESTLINK_MIN) -o $@ $^ $(EXTRA_L_FLAGS) -lpthread

.PHONY: bus-bench
bus-bench: $(TESTPREFIX)/bus-bench

# First real module-on-bus integration (memory recall over the bus). Links the
# real DB1 user-memory path (against the pg/sqlite test shim, like the memory
# harness) plus the bus. A test/integration binary; not a shipping target.
BUS_MEM_OBJS = $(OBJDIR)/modules/db1/user_memory.o $(OBJDIR)/modules/db1/db.o $(OBJDIR)/modules/db1/db_schema.o \
               $(OBJDIR)/db2/db_schema.o $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db1_write.o \
               $(OBJDIR)/modules/db1/db1_trigger.o $(OBJDIR)/modules/db1/db1_cron_jobs.o \
               $(OBJDIR)/modules/db1/guardrail_events.o \
               $(OBJDIR)/modules/db1/model_catalog.o $(OBJDIR)/model_catalog_release.o $(OBJDIR)/modules/db1/maintenance.o $(OBJDIR)/modules/db1/eval.o \
               $(DB2_TEST_BACKEND_OBJ) $(OBJDIR)/db2/db2_test_shim.o \
               $(CONFIG_CLIENT_TEST_OBJS) \
               $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/util.o $(OBJDIR)/text.o \
               $(OBJDIR)/platform_random.o $(PLATFORM_BASIC_OBJS) $(OBJDIR)/log.o $(OBJDIR)/cJSON.o
$(OBJDIR)/tests/test_bus_memory_recall.o: C_FLAGS += -Icore/event_bus/include
$(TESTPREFIX)/unit-test-bus-memory-recall: $(OBJDIR)/tests/test_bus_memory_recall.o \
                                           $(OBJDIR)/core/event_bus/bus_client.o \
                                           $(OBJDIR)/core/event_bus/bus_attach.o \
                                           $(OBJDIR)/core/event_bus/bus_host.o \
                                           $(OBJDIR)/core/event_bus/bus_route.o \
                                           $(OBJDIR)/core/event_bus/bus_region.o $(OBJDIR)/core/event_bus/bus_region_host.o \
                                           $(OBJDIR)/core/event_bus/bus_ring.o \
                                           $(OBJDIR)/core/event_bus/bus_arena.o \
                                           $(OBJDIR)/core/event_bus/bus_wire.o \
                                           $(BUS_MEM_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lpthread

.PHONY: unit-test-bus-memory-recall
unit-test-bus-memory-recall: $(TESTPREFIX)/unit-test-bus-memory-recall
	$<

# The verify ledger across the real bus, against the real Go module.
#
# git_verify_state.c is now a seam: it asks the git module rather than touching
# the repository. Every other suite that links it uses module_bus_stub, whose
# default is "no module attached" -- which proves the seam fails closed and
# proves nothing about whether it is right. This target builds the Go multicall
# binary, hosts the daemon's authenticated module endpoint, and drives the
# ordinary C entry points against a real attached process.
#
# It needs Go. A missing toolchain fails the target rather than skipping it:
# skipping would silently retire the only correctness coverage the ledger has.
$(OBJDIR)/tests/test_git_verify_state_bus.o: C_FLAGS += -Icore/event_bus/include

$(OBJDIR)/aimee-module: $(wildcard ../server-go/modules/git/*.go) $(wildcard ../server-go/cmd/aimee-module/*.go)
	@test -n "$(GO)" || { echo "aimee-module: ERROR go is required for the bus fixture"; exit 1; }
	@mkdir -p $(@D)
	cd ../server-go && CGO_ENABLED=0 $(GO) build -o "$(CURDIR)/$@" ./cmd/aimee-module

$(OBJDIR)/aimee-module-config: ../server-go/go.mod ../server-go/go.sum
	@test -n "$(GO)" || { echo "aimee-module-config: ERROR go is required for the config fixture"; exit 1; }
	@mkdir -p $(dir $@)
	cd ../server-go && CGO_ENABLED=0 $(GO) build -o "$(CURDIR)/$@" \
		github.com/RakuenSoftware/aimee-module-config/runtime

$(TESTPREFIX)/unit-test-git-verify-state-bus: | $(OBJDIR)/aimee-module
$(TESTPREFIX)/unit-test-git-verify-state-bus: $(OBJDIR)/tests/test_git_verify_state_bus.o \
                                           $(OBJDIR)/modules/git/git_verify_state.o \
                                           $(OBJDIR)/modules/git/git_verify.o \
                                           $(OBJDIR)/modules/git/git_verify_config.o \
                                           $(OBJDIR)/modules/git/git_verify_jobs.o \
                                           $(OBJDIR)/modules/git/git_verify_hook.o \
                                           $(OBJDIR)/modules/git/git_verify_ops.o \
                                           $(OBJDIR)/modules/git/git_verify_select.o \
                                           $(OBJDIR)/modules/git/git_verify_step.o \
                                           $(OBJDIR)/module_json_call.o $(OBJDIR)/cJSON.o \
                                           $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lpthread

.PHONY: unit-test-git-verify-state-bus
unit-test-git-verify-state-bus: $(TESTPREFIX)/unit-test-git-verify-state-bus $(OBJDIR)/aimee-module
	$< $(OBJDIR)/aimee-module

# Real mutating memory op over the bus (upsert insert + update, verified by a
# direct read-back of the store). Same DB1 path + bus objects as the recall test.
$(OBJDIR)/tests/test_bus_memory_upsert.o: C_FLAGS += -Icore/event_bus/include
$(TESTPREFIX)/unit-test-bus-memory-upsert: $(OBJDIR)/tests/test_bus_memory_upsert.o \
                                           $(OBJDIR)/core/event_bus/bus_client.o \
                                           $(OBJDIR)/core/event_bus/bus_attach.o \
                                           $(OBJDIR)/core/event_bus/bus_host.o \
                                           $(OBJDIR)/core/event_bus/bus_route.o \
                                           $(OBJDIR)/core/event_bus/bus_region.o $(OBJDIR)/core/event_bus/bus_region_host.o \
                                           $(OBJDIR)/core/event_bus/bus_ring.o \
                                           $(OBJDIR)/core/event_bus/bus_arena.o \
                                           $(OBJDIR)/core/event_bus/bus_wire.o \
                                           $(BUS_MEM_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lpthread

.PHONY: unit-test-bus-memory-upsert
unit-test-bus-memory-upsert: $(TESTPREFIX)/unit-test-bus-memory-upsert
	$<

# Native caller-side contract: exact event/stage/request envelopes sent to the
# external pure-Go config module, including atomic mutations and reload versioning.
$(OBJDIR)/tests/test_bus_config_autonomy.o: C_FLAGS += -Icore/event_bus/include
$(TESTPREFIX)/unit-test-bus-config-autonomy: $(OBJDIR)/tests/test_bus_config_autonomy.o \
                                             $(OBJDIR)/config_client.o \
                                             $(OBJDIR)/config_client_contract.o \
                                             $(CONFIG_CLIENT_ACCESSOR_OBJS) \
                                             $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lpthread

.PHONY: unit-test-bus-config-autonomy
unit-test-bus-config-autonomy: $(TESTPREFIX)/unit-test-bus-config-autonomy
	$<

# The audit-on-bus DURABILITY test: the first module whose real path is being
# migrated onto the bus. Emits N rows through the real obs_bus producer/consumer
# and requires the real ledger to hold exactly N, each once, zero drops. Links
# obs_bus + audit_ledger + the bus + the shared support objects. Test binary only.
$(OBJDIR)/tests/test_bus_audit_durability.o: C_FLAGS += -Icore/event_bus/include
$(TESTPREFIX)/unit-test-bus-audit-durability: $(OBJDIR)/tests/test_bus_audit_durability.o \
                                              $(OBS_BUS_LINK_OBJS) \
                                              $(OBJDIR)/modules/audit/audit_ledger.o \
                                              $(OBJDIR)/aimee_home.o \
                                              $(OBJDIR)/core/event_bus/bus_client.o \
                                              $(OBJDIR)/core/event_bus/bus_attach.o \
                                              $(OBJDIR)/core/event_bus/bus_host.o \
                                              $(OBJDIR)/core/event_bus/bus_route.o \
                                              $(OBJDIR)/core/event_bus/bus_region.o $(OBJDIR)/core/event_bus/bus_region_host.o \
                                              $(OBJDIR)/core/event_bus/bus_ring.o \
                                              $(OBJDIR)/core/event_bus/bus_arena.o \
                                              $(OBJDIR)/core/event_bus/bus_wire.o \
                                              $(OBJDIR)/core/event_bus/bus_capture.o \
                                              $(BUS_MEM_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lpthread

$(TESTPREFIX)/unit-test-obs-bus-module-concurrency: $(OBJDIR)/tests/test_obs_bus_module_concurrency.o \
                                              $(OBS_BUS_LINK_OBJS) \
                                              $(OBJDIR)/modules/audit/audit_ledger.o \
                                              $(OBJDIR)/aimee_home.o \
                                              $(OBJDIR)/core/event_bus/bus_client.o \
                                              $(OBJDIR)/core/event_bus/bus_attach.o \
                                              $(OBJDIR)/core/event_bus/bus_host.o \
                                              $(OBJDIR)/core/event_bus/bus_route.o \
                                              $(OBJDIR)/core/event_bus/bus_region.o $(OBJDIR)/core/event_bus/bus_region_host.o \
                                              $(OBJDIR)/core/event_bus/bus_ring.o \
                                              $(OBJDIR)/core/event_bus/bus_arena.o \
                                              $(OBJDIR)/core/event_bus/bus_wire.o \
                                              $(OBJDIR)/core/event_bus/bus_capture.o \
                                              $(BUS_MEM_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lpthread

.PHONY: unit-test-bus-audit-durability
unit-test-bus-audit-durability: $(TESTPREFIX)/unit-test-bus-audit-durability
	$<

# audit-on-bus RECORD+REPLAY: the reason audit is on the bus (auditability, not
# speed). Emits N rows, then reads the real capture file back and requires every
# governed-action row to replay in order, reconstructed field-for-field. Same
# link set as durability plus bus_capture.o. Test binary only.
$(OBJDIR)/tests/test_bus_audit_replay.o: C_FLAGS += -Icore/event_bus/include
$(TESTPREFIX)/unit-test-bus-audit-replay: $(OBJDIR)/tests/test_bus_audit_replay.o \
                                          $(OBS_BUS_LINK_OBJS) \
                                          $(OBJDIR)/modules/audit/audit_ledger.o \
                                          $(OBJDIR)/aimee_home.o \
                                          $(OBJDIR)/core/event_bus/bus_client.o \
                                          $(OBJDIR)/core/event_bus/bus_attach.o \
                                          $(OBJDIR)/core/event_bus/bus_host.o \
                                          $(OBJDIR)/core/event_bus/bus_route.o \
                                          $(OBJDIR)/core/event_bus/bus_region.o $(OBJDIR)/core/event_bus/bus_region_host.o \
                                          $(OBJDIR)/core/event_bus/bus_ring.o \
                                          $(OBJDIR)/core/event_bus/bus_arena.o \
                                          $(OBJDIR)/core/event_bus/bus_wire.o \
                                          $(OBJDIR)/core/event_bus/bus_capture.o \
                                          $(BUS_MEM_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lpthread

.PHONY: unit-test-bus-audit-replay
unit-test-bus-audit-replay: $(TESTPREFIX)/unit-test-bus-audit-replay
	$<

# audit-on-bus capture RETENTION: a restart retains prior sessions' replay streams
# but the files stay bounded. Same link set as replay/durability.
$(OBJDIR)/tests/test_bus_audit_retention.o: C_FLAGS += -Icore/event_bus/include
$(TESTPREFIX)/unit-test-bus-audit-retention: $(OBJDIR)/tests/test_bus_audit_retention.o \
                                             $(OBS_BUS_LINK_OBJS) \
                                             $(OBJDIR)/modules/audit/audit_ledger.o \
                                             $(OBJDIR)/aimee_home.o \
                                             $(OBJDIR)/core/event_bus/bus_client.o \
                                             $(OBJDIR)/core/event_bus/bus_attach.o \
                                             $(OBJDIR)/core/event_bus/bus_host.o \
                                             $(OBJDIR)/core/event_bus/bus_route.o \
                                             $(OBJDIR)/core/event_bus/bus_region.o $(OBJDIR)/core/event_bus/bus_region_host.o \
                                             $(OBJDIR)/core/event_bus/bus_ring.o \
                                             $(OBJDIR)/core/event_bus/bus_arena.o \
                                             $(OBJDIR)/core/event_bus/bus_wire.o \
                                             $(OBJDIR)/core/event_bus/bus_capture.o \
                                             $(BUS_MEM_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lpthread

.PHONY: unit-test-bus-audit-retention
unit-test-bus-audit-retention: $(TESTPREFIX)/unit-test-bus-audit-retention
	$<

# The operator replay TOOL (audit_replay.c, behind aimee-server --audit-replay):
# render a capture file's governed-action rows. Adds audit_replay.o to the set.
$(OBJDIR)/tests/test_bus_audit_replay_tool.o: C_FLAGS += -Icore/event_bus/include
$(TESTPREFIX)/unit-test-bus-audit-replay-tool: $(OBJDIR)/tests/test_bus_audit_replay_tool.o \
                                               $(OBS_BUS_LINK_OBJS) \
                                               $(OBJDIR)/modules/audit/audit_replay.o \
                                               $(OBJDIR)/modules/audit/audit_ledger.o \
                                               $(OBJDIR)/aimee_home.o \
                                               $(OBJDIR)/core/event_bus/bus_client.o \
                                               $(OBJDIR)/core/event_bus/bus_attach.o \
                                               $(OBJDIR)/core/event_bus/bus_host.o \
                                               $(OBJDIR)/core/event_bus/bus_route.o \
                                               $(OBJDIR)/core/event_bus/bus_region.o $(OBJDIR)/core/event_bus/bus_region_host.o \
                                               $(OBJDIR)/core/event_bus/bus_ring.o \
                                               $(OBJDIR)/core/event_bus/bus_arena.o \
                                               $(OBJDIR)/core/event_bus/bus_wire.o \
                                               $(OBJDIR)/core/event_bus/bus_capture.o \
                                               $(BUS_MEM_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lpthread

.PHONY: unit-test-bus-audit-replay-tool
unit-test-bus-audit-replay-tool: $(TESTPREFIX)/unit-test-bus-audit-replay-tool
	$<

# Second module on the bus: the guardrail-semantic event. Emits N over the bus and
# requires the real db1 guardrail_events table to hold exactly N. guardrail_events.o
# is in BUS_MEM_OBJS; obs_bus dispatches this kind to db1.
$(OBJDIR)/tests/test_bus_guardrail_durability.o: C_FLAGS += -Icore/event_bus/include
$(TESTPREFIX)/unit-test-bus-guardrail-durability: $(OBJDIR)/tests/test_bus_guardrail_durability.o \
                                                  $(OBJDIR)/server/obs_bus_adapter.o \
                                                  $(OBS_BUS_LINK_OBJS) \
                                                  $(OBJDIR)/modules/audit/audit_ledger.o \
                                                  $(OBJDIR)/aimee_home.o \
                                                  $(OBJDIR)/core/event_bus/bus_client.o \
                                                  $(OBJDIR)/core/event_bus/bus_attach.o \
                                                  $(OBJDIR)/core/event_bus/bus_host.o \
                                                  $(OBJDIR)/core/event_bus/bus_route.o \
                                                  $(OBJDIR)/core/event_bus/bus_region.o $(OBJDIR)/core/event_bus/bus_region_host.o \
                                                  $(OBJDIR)/core/event_bus/bus_ring.o \
                                                  $(OBJDIR)/core/event_bus/bus_arena.o \
                                                  $(OBJDIR)/core/event_bus/bus_wire.o \
                                                  $(OBJDIR)/core/event_bus/bus_capture.o \
                                                  $(BUS_MEM_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lpthread

.PHONY: unit-test-bus-guardrail-durability
unit-test-bus-guardrail-durability: $(TESTPREFIX)/unit-test-bus-guardrail-durability
	$<

# Vault credential-access audit, end-to-end through the REAL server bridge
# (vault_audit_bridge.o) -> obs_bus -> ledger. Same bus link set as the audit
# durability test, plus the bridge, the vault service + its crypto/store/cache
# deps, and audit_action.o (audit_args_hash + audit_ensure_key).
$(TESTPREFIX)/unit-test-bus-vault-audit: $(OBJDIR)/tests/test_bus_vault_audit.o \
                                         $(OBJDIR)/server/vault_audit_bridge.o \
                                         $(OBS_BUS_LINK_OBJS) \
                                         $(OBJDIR)/modules/audit/audit_ledger.o \
                                         $(OBJDIR)/modules/audit/audit_action.o \
                                         $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                                         $(OBJDIR)/modules/vault/vault_service.o \
                                         $(OBJDIR)/modules/vault/vault_store.o \
                                         $(OBJDIR)/modules/vault/vault_kek_check.o \
                                         $(OBJDIR)/modules/vault/vault_crypto.o \
                                         $(OBJDIR)/modules/vault/vault_kek_cache.o \
                                         $(OBJDIR)/modules/vault/vault_server_key.o \
                                         $(OBJDIR)/aimee_home.o \
                                         $(OBJDIR)/core/event_bus/bus_client.o \
                                         $(OBJDIR)/core/event_bus/bus_attach.o \
                                         $(OBJDIR)/core/event_bus/bus_host.o \
                                         $(OBJDIR)/core/event_bus/bus_route.o \
                                         $(OBJDIR)/core/event_bus/bus_region.o $(OBJDIR)/core/event_bus/bus_region_host.o \
                                         $(OBJDIR)/core/event_bus/bus_ring.o \
                                         $(OBJDIR)/core/event_bus/bus_arena.o \
                                         $(OBJDIR)/core/event_bus/bus_wire.o \
                                         $(OBJDIR)/core/event_bus/bus_capture.o \
                                         $(BUS_MEM_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lpthread

.PHONY: unit-test-bus-vault-audit
unit-test-bus-vault-audit: $(TESTPREFIX)/unit-test-bus-vault-audit
	$<

# NOT in TEST_TARGETS: like the other bus tests it needs a special bus link set
# the standard unit-tests build does not assemble, so the bench gate
# (check_bus_perf_gate.sh) force-builds + runs it via this .PHONY target instead.

# Sandbox degraded-isolation audit, end-to-end through the REAL server bridge
# (sandbox_audit_bridge.o) -> obs_bus -> ledger. Same bus link set as the audit
# durability test, plus the bridge, posix/sandbox.o, and audit_action.o.
$(TESTPREFIX)/unit-test-bus-sandbox-audit: $(OBJDIR)/tests/test_bus_sandbox_audit.o \
                                           $(OBJDIR)/server/sandbox_audit_bridge.o \
                                           $(OBJDIR)/posix/sandbox.o \
                                           $(OBS_BUS_LINK_OBJS) \
                                           $(OBJDIR)/modules/audit/audit_ledger.o \
                                           $(OBJDIR)/modules/audit/audit_action.o \
                                           $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                                           $(OBJDIR)/aimee_home.o \
                                           $(OBJDIR)/core/event_bus/bus_client.o \
                                           $(OBJDIR)/core/event_bus/bus_attach.o \
                                           $(OBJDIR)/core/event_bus/bus_host.o \
                                           $(OBJDIR)/core/event_bus/bus_route.o \
                                           $(OBJDIR)/core/event_bus/bus_region.o $(OBJDIR)/core/event_bus/bus_region_host.o \
                                           $(OBJDIR)/core/event_bus/bus_ring.o \
                                           $(OBJDIR)/core/event_bus/bus_arena.o \
                                           $(OBJDIR)/core/event_bus/bus_wire.o \
                                           $(OBJDIR)/core/event_bus/bus_capture.o \
                                           $(BUS_MEM_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lpthread

.PHONY: unit-test-bus-sandbox-audit
unit-test-bus-sandbox-audit: $(TESTPREFIX)/unit-test-bus-sandbox-audit
	$<

# Tool-call completion audit, through the REAL server bridge
# (tool_completion_audit_bridge.o) -> obs_bus -> ledger. Links the dep-free hook
# TU (agent_tools_completion.o) — NOT the whole tool dispatcher — and drives it
# via that TU's test seam.
$(TESTPREFIX)/unit-test-bus-tool-completion: $(OBJDIR)/tests/test_bus_tool_completion.o \
                                             $(OBJDIR)/server/tool_completion_audit_bridge.o \
                                             $(OBJDIR)/modules/tools/agent_tools_completion.o \
                                             $(OBS_BUS_LINK_OBJS) \
                                             $(OBJDIR)/modules/audit/audit_ledger.o \
                                             $(OBJDIR)/modules/audit/audit_action.o \
                                             $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                                             $(OBJDIR)/aimee_home.o \
                                             $(OBJDIR)/core/event_bus/bus_client.o \
                                             $(OBJDIR)/core/event_bus/bus_attach.o \
                                             $(OBJDIR)/core/event_bus/bus_host.o \
                                             $(OBJDIR)/core/event_bus/bus_route.o \
                                             $(OBJDIR)/core/event_bus/bus_region.o $(OBJDIR)/core/event_bus/bus_region_host.o \
                                             $(OBJDIR)/core/event_bus/bus_ring.o \
                                             $(OBJDIR)/core/event_bus/bus_arena.o \
                                             $(OBJDIR)/core/event_bus/bus_wire.o \
                                             $(OBJDIR)/core/event_bus/bus_capture.o \
                                             $(BUS_MEM_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lpthread

.PHONY: unit-test-bus-tool-completion
unit-test-bus-tool-completion: $(TESTPREFIX)/unit-test-bus-tool-completion
	$<

# Server-side memory-mutation audit, end-to-end through the REAL bridge
# (memory_audit_bridge.o) -> obs_bus -> ledger. Links the dep-free hook TU
# (kb_client_memory_audit.o) — NOT the whole kb_client RPC stack.
$(TESTPREFIX)/unit-test-bus-memory-audit: $(OBJDIR)/tests/test_bus_memory_audit.o \
                                          $(OBJDIR)/server/memory_audit_bridge.o \
                                          $(OBJDIR)/modules/kb_client/kb_client_memory_audit.o \
                                          $(OBS_BUS_LINK_OBJS) \
                                          $(OBJDIR)/modules/audit/audit_ledger.o \
                                          $(OBJDIR)/modules/audit/audit_action.o \
                                          $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                                          $(OBJDIR)/aimee_home.o \
                                          $(OBJDIR)/core/event_bus/bus_client.o \
                                          $(OBJDIR)/core/event_bus/bus_attach.o \
                                          $(OBJDIR)/core/event_bus/bus_host.o \
                                          $(OBJDIR)/core/event_bus/bus_route.o \
                                          $(OBJDIR)/core/event_bus/bus_region.o $(OBJDIR)/core/event_bus/bus_region_host.o \
                                          $(OBJDIR)/core/event_bus/bus_ring.o \
                                          $(OBJDIR)/core/event_bus/bus_arena.o \
                                          $(OBJDIR)/core/event_bus/bus_wire.o \
                                          $(OBJDIR)/core/event_bus/bus_capture.o \
                                          $(BUS_MEM_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lpthread

.PHONY: unit-test-bus-memory-audit
unit-test-bus-memory-audit: $(TESTPREFIX)/unit-test-bus-memory-audit
	$<

# Shutdown race: concurrent producers vs obs_bus_stop() (regression test for the
# in-flight-emit-vs-teardown UAF). Same link set as the guardrail durability test.
$(OBJDIR)/tests/test_bus_shutdown_race.o: C_FLAGS += -Icore/event_bus/include
$(TESTPREFIX)/unit-test-bus-shutdown-race: $(OBJDIR)/tests/test_bus_shutdown_race.o \
                                           $(OBJDIR)/server/obs_bus_adapter.o \
                                           $(OBS_BUS_LINK_OBJS) \
                                           $(OBJDIR)/modules/audit/audit_ledger.o \
                                           $(OBJDIR)/aimee_home.o \
                                           $(OBJDIR)/core/event_bus/bus_client.o \
                                           $(OBJDIR)/core/event_bus/bus_attach.o \
                                           $(OBJDIR)/core/event_bus/bus_host.o \
                                           $(OBJDIR)/core/event_bus/bus_route.o \
                                           $(OBJDIR)/core/event_bus/bus_region.o $(OBJDIR)/core/event_bus/bus_region_host.o \
                                           $(OBJDIR)/core/event_bus/bus_ring.o \
                                           $(OBJDIR)/core/event_bus/bus_arena.o \
                                           $(OBJDIR)/core/event_bus/bus_wire.o \
                                           $(OBJDIR)/core/event_bus/bus_capture.o \
                                           $(BUS_MEM_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lpthread

.PHONY: unit-test-bus-shutdown-race
unit-test-bus-shutdown-race: $(TESTPREFIX)/unit-test-bus-shutdown-race
	$<

.PHONY: unit-test-bus-capture
unit-test-bus-capture: $(TESTPREFIX)/unit-test-bus-capture
	$<

.PHONY: unit-test-bus-client
unit-test-bus-client: $(TESTPREFIX)/unit-test-bus-client
	$<

.PHONY: unit-test-bus-flow
unit-test-bus-flow: $(TESTPREFIX)/unit-test-bus-flow
	$<

.PHONY: unit-test-bus-route
unit-test-bus-route: $(TESTPREFIX)/unit-test-bus-route
	$<

.PHONY: unit-test-bus-host
unit-test-bus-host: $(TESTPREFIX)/unit-test-bus-host
	$<

.PHONY: unit-test-bus-arena
unit-test-bus-arena: $(TESTPREFIX)/unit-test-bus-arena
	$<

.PHONY: unit-test-bus-region
unit-test-bus-region: $(TESTPREFIX)/unit-test-bus-region
	$<

.PHONY: unit-test-bus-ring
unit-test-bus-ring: $(TESTPREFIX)/unit-test-bus-ring
	$<

.PHONY: unit-test-bus-wire
unit-test-bus-wire: $(TESTPREFIX)/unit-test-bus-wire
	$<

.PHONY: unit-test-module-protocol
unit-test-module-protocol: $(TESTPREFIX)/unit-test-module-protocol
	$<

# The bus tests are deliberately NOT in TEST_TARGETS -- each needs a special bus
# link set the standard unit-tests build does not assemble, so the bench gate
# (check_bus_perf_gate.sh) force-builds them via their .PHONY targets. That also
# means none of them inherited the blanket `$(TEST_TARGETS): | $(CORE_CONNECTION_LIB)`
# earlier in this file, so the ones that pull the archive through L_CORE died on a
# clean tree with "cannot find build/obj/libaimee-core-connection.a" -- exactly the
# gap already fixed for unit-test-code-treesitter below and for the auxiliary
# drivers in src/Makefile. Listing all of them keeps the guarantee independent of
# which ones happen to link L_CORE today.
#
# Order-only: the archive must EXIST before these link, but relinking it must not
# force every bus test to relink.
BUS_TEST_TARGETS := $(addprefix $(TESTPREFIX)/, \
   unit-test-bus-endpoint unit-test-bus-runtime unit-test-bus-memory-recall \
   unit-test-git-verify-state-bus \
   unit-test-bus-memory-upsert unit-test-bus-config-autonomy \
   unit-test-bus-audit-durability unit-test-bus-audit-replay \
   unit-test-bus-audit-retention unit-test-bus-audit-replay-tool \
   unit-test-bus-guardrail-durability unit-test-bus-vault-audit \
   unit-test-bus-sandbox-audit unit-test-bus-tool-completion \
   unit-test-bus-memory-audit unit-test-bus-shutdown-race unit-test-bus-capture \
   unit-test-bus-client unit-test-bus-flow unit-test-bus-route unit-test-bus-host \
   unit-test-bus-arena unit-test-bus-region unit-test-bus-ring unit-test-bus-wire)

$(BUS_TEST_TARGETS): | $(CORE_CONNECTION_LIB)


# Render-boundary prompt sanitizer (graph-feedback §4 / P0). Pure: no DB.
$(TESTPREFIX)/unit-test-prompt-sanitizer: $(OBJDIR)/tests/test_prompt_sanitizer.o \
                                          $(OBJDIR)/kb/prompt_sanitizer.o
	$(TESTLINK) -o $@ $^ $(L_CORE)

# Blast-radius advisory: structural §7 actuation. Hermetic — legacy_config_read and the
# kb_client_index_* sidecar calls are stubbed in the test, so no DB/sidecar.
$(TESTPREFIX)/unit-test-guardrails-blast-radius: $(OBJDIR)/tests/test_guardrails_blast_radius.o \
                                                 $(OBJDIR)/modules/guardrails/guardrails_blast_radius.o
	$(TESTLINK) -o $@ $^ $(L_CORE)

# Code collector source selection (git default branch vs working tree). Drives
# the real collector against throwaway git repos materialized under TMPDIR.
$(TESTPREFIX)/unit-test-code-collect: $(OBJDIR)/tests/test_code_collect.o \
                                      $(OBJDIR)/code_collect.o $(OBJDIR)/cJSON.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(L_CORE)

# Accepted-connection fds must be close-on-exec: an inherited fd keeps the
# client blocked in read() until the child exits (see server_conn_io.c).
$(TESTPREFIX)/unit-test-server-conn-accept: $(OBJDIR)/tests/test_server_conn_accept.o \
                                           $(OBJDIR)/server/server_conn_io.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(L_CORE)

# §2 tree-sitter front-end test — opt-in only (links the fetched runtime + grammar).
# Default `make unit-tests` (CI) never builds it: the target + its TEST_TARGETS entry are
# gated on AIMEE_TREESITTER, so the vendored objects are required only when enabled.
ifdef AIMEE_TREESITTER
TEST_TARGETS += $(TESTPREFIX)/unit-test-code-treesitter
$(OBJDIR)/code_treesitter.o: C_FLAGS += -DAIMEE_TREESITTER -Ivendor/tree-sitter/lib/include
# Order-only dep on a fetch target so a cold checkout fetches tree_sitter/api.h before
# this object (which includes it) is compiled (see the same note in src/Makefile).
$(OBJDIR)/code_treesitter.o: | vendor/tree-sitter/lib/src/lib.c
# The blanket `$(TEST_TARGETS): | $(CORE_CONNECTION_LIB)` earlier in this file
# expands TEST_TARGETS where it is written, and this target appends itself
# BELOW that line -- so it never inherited the dependency. It still links the
# archive through L_CORE, so a clean tree failed with "cannot find
# build/obj/libaimee-core-connection.a" at link time.
$(TESTPREFIX)/unit-test-code-treesitter: | $(CORE_CONNECTION_LIB)
$(TESTPREFIX)/unit-test-code-treesitter: $(OBJDIR)/tests/test_code_treesitter.o \
                                         $(OBJDIR)/code_treesitter.o \
                                         $(OBJDIR)/extractors.o \
                                         $(OBJDIR)/extractors_extra.o \
                                         $(OBJDIR)/extractors_new_langs.o \
                                         $(TS_VENDOR_OBJS) \
                                         $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(L_CORE)
endif

# Cross-repo dependency graph S2a/S2b: pure resolver core (import resolution +
# distinctiveness) and tier classification (multiplicity + pipeline). DB-free, so
# it links only the resolver + classify objects. The TEST_TARGETS membership is
# declared in the initial := block above (before the unit-tests rule) so the
# binary is built, not just run.
$(TESTPREFIX)/unit-test-cross-repo-deps: $(OBJDIR)/tests/test_cross_repo_deps.o \
                                         $(OBJDIR)/db2/cross_repo_resolver.o \
                                         $(OBJDIR)/db2/cross_repo_classify.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-aimee-client: $(OBJDIR)/tests/test_aimee_client.o $(OBJDIR)/aimee_client.o \
                                      $(OBJDIR)/posix/platform_net.o $(OBJDIR)/http_uds_client.o \
                                      $(OBJDIR)/aimee_home.o $(OBJDIR)/http_content_encoding.o \
                                      $(TLS_OBJS) $(CORE_CONNECTION_LIB)
	$(TESTLINK) -o $@ $^ $(L_MINIMAL) $(TLS_LIBS) -lz

$(TESTPREFIX)/unit-test-cli-remote: $(OBJDIR)/tests/test_cli_remote.o $(OBJDIR)/cli_remote.o \
                                    $(OBJDIR)/aimee_client.o $(OBJDIR)/posix/platform_net.o \
                                    $(OBJDIR)/http_uds_client.o $(OBJDIR)/aimee_home.o $(OBJDIR)/cJSON.o \
                                    $(OBJDIR)/http_content_encoding.o $(TLS_OBJS) $(CORE_CONNECTION_LIB)
	$(TESTLINK) -o $@ $^ $(L_MINIMAL) $(TLS_LIBS) -lz

$(TESTPREFIX)/unit-test-util-url: $(OBJDIR)/tests/test_util_url.o $(OBJDIR)/util_url.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-delivery-target: $(OBJDIR)/tests/test_delivery_target.o \
                                         $(OBJDIR)/delivery_target.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-gateway: $(OBJDIR)/tests/test_gateway.o \
                                 $(OBJDIR)/gateway/delivery_router.o \
                                 $(OBJDIR)/gateway/platform_registry.o \
                                 $(OBJDIR)/gateway/platform_telegram.o \
                                 $(OBJDIR)/gateway/platform_ntfy.o \
                                 $(OBJDIR)/gateway/platform_webhook.o \
                                 $(OBJDIR)/gateway/gateway_ctx.o $(OBJDIR)/cli_v1_routes.o $(OBJDIR)/cli_v1_routes_b.o $(OBJDIR)/cli_argspec.o $(OBJDIR)/modules/workspace/workspace_client_diff.o $(OBJDIR)/cli_v1_routes_c.o $(OBJDIR)/cli_v1_routes_d.o $(OBJDIR)/aimee_client.o $(OBJDIR)/http_uds_client.o $(OBJDIR)/aimee_tls.o $(OBJDIR)/codex_auth.o $(OBJDIR)/cli_v1_routes_e.o $(OBJDIR)/cli_v1_routes_f.o \
                                 $(OBJDIR)/gateway/gateway_pairing.o \
                                 $(OBJDIR)/aimee_home.o \
                                 $(OBJDIR)/gateway/session_key.o \
                                 $(OBJDIR)/gateway/pairing.o \
                                 $(OBJDIR)/gateway/mirror.o \
                                 $(OBJDIR)/gateway/stt.o \
                                 $(OBJDIR)/gateway/tts.o \
                                 $(OBJDIR)/delivery_target.o \
                                 $(OBJDIR)/posix/agent_bridge.o \
                                 $(OBJDIR)/proxy_bootstrap.o \
                                 $(OBJDIR)/cJSON.o \
                                 $(OBJDIR)/log.o \
                                 $(OBJDIR)/platform_random.o \
                                 $(OBJDIR)/modules/vault/runtime_secret.o \
                                 $(GATEWAY_PLATFORM_OBJS) \
                                 $(CORE_CONNECTION_LIB)
	$(TESTLINK) -o $@ $^ $(L_GATEWAY)

$(TESTPREFIX)/unit-test-gateway-telegram: $(OBJDIR)/tests/test_gateway_telegram.o \
                                          $(OBJDIR)/gateway/platform_telegram.o \
                                          $(OBJDIR)/gateway/platform_registry.o \
                                          $(OBJDIR)/gateway/platform_ntfy.o \
                                          $(OBJDIR)/gateway/platform_webhook.o \
                                          $(OBJDIR)/gateway/gateway_ctx.o $(OBJDIR)/cli_v1_routes.o $(OBJDIR)/cli_v1_routes_b.o $(OBJDIR)/cli_argspec.o $(OBJDIR)/modules/workspace/workspace_client_diff.o $(OBJDIR)/cli_v1_routes_c.o $(OBJDIR)/cli_v1_routes_d.o $(OBJDIR)/aimee_client.o $(OBJDIR)/http_uds_client.o $(OBJDIR)/aimee_tls.o $(OBJDIR)/codex_auth.o $(OBJDIR)/cli_v1_routes_e.o $(OBJDIR)/cli_v1_routes_f.o \
                                          $(OBJDIR)/gateway/gateway_pairing.o \
                                          $(OBJDIR)/aimee_home.o \
                                          $(OBJDIR)/gateway/delivery_router.o \
                                          $(OBJDIR)/gateway/session_key.o \
                                          $(OBJDIR)/gateway/pairing.o \
                                          $(OBJDIR)/gateway/mirror.o \
                                          $(OBJDIR)/gateway/stt.o \
                                          $(OBJDIR)/gateway/tts.o \
                                          $(OBJDIR)/delivery_target.o \
                                          $(OBJDIR)/posix/agent_bridge.o \
                                          $(OBJDIR)/posix/cli_client.o $(OBJDIR)/cli_v1_routes.o $(OBJDIR)/cli_v1_routes_b.o $(OBJDIR)/modules/workspace/workspace_client_diff.o $(OBJDIR)/cli_v1_routes_c.o $(OBJDIR)/cli_v1_routes_d.o $(OBJDIR)/cli_v1_routes_e.o $(OBJDIR)/cli_v1_routes_f.o $(OBJDIR)/aimee_tls.o $(OBJDIR)/codex_auth.o \
                                          $(OBJDIR)/proxy_bootstrap.o \
                                          $(OBJDIR)/util.o \
                                        $(OBJDIR)/cJSON.o \
                                          $(OBJDIR)/log.o \
                                          $(OBJDIR)/platform_random.o \
                                          $(OBJDIR)/modules/vault/runtime_secret.o \
                                          $(GATEWAY_PLATFORM_OBJS) \
                                          $(CORE_CONNECTION_LIB)
	$(TESTLINK) -o $@ $^ $(L_GATEWAY)

$(TESTPREFIX)/unit-test-gateway-ntfy-webhook: $(OBJDIR)/tests/test_gateway_ntfy_webhook.o \
                                              $(OBJDIR)/gateway/platform_ntfy.o \
                                              $(OBJDIR)/gateway/platform_webhook.o \
                                              $(OBJDIR)/gateway/platform_telegram.o \
                                              $(OBJDIR)/gateway/platform_registry.o \
                                              $(OBJDIR)/gateway/gateway_ctx.o $(OBJDIR)/cli_v1_routes.o $(OBJDIR)/cli_v1_routes_b.o $(OBJDIR)/cli_argspec.o $(OBJDIR)/modules/workspace/workspace_client_diff.o $(OBJDIR)/cli_v1_routes_c.o $(OBJDIR)/cli_v1_routes_d.o $(OBJDIR)/aimee_client.o $(OBJDIR)/http_uds_client.o $(OBJDIR)/aimee_tls.o $(OBJDIR)/codex_auth.o $(OBJDIR)/cli_v1_routes_e.o $(OBJDIR)/cli_v1_routes_f.o \
                                              $(OBJDIR)/gateway/gateway_pairing.o \
                                              $(OBJDIR)/aimee_home.o \
                                              $(OBJDIR)/gateway/delivery_router.o \
                                              $(OBJDIR)/gateway/session_key.o \
                                              $(OBJDIR)/gateway/pairing.o \
                                              $(OBJDIR)/gateway/mirror.o \
                                              $(OBJDIR)/gateway/stt.o \
                                              $(OBJDIR)/gateway/tts.o \
                                              $(OBJDIR)/delivery_target.o \
                                              $(OBJDIR)/posix/agent_bridge.o \
                                              $(OBJDIR)/posix/cli_client.o $(OBJDIR)/cli_v1_routes.o $(OBJDIR)/cli_v1_routes_b.o $(OBJDIR)/modules/workspace/workspace_client_diff.o $(OBJDIR)/cli_v1_routes_c.o $(OBJDIR)/cli_v1_routes_d.o $(OBJDIR)/cli_v1_routes_e.o $(OBJDIR)/cli_v1_routes_f.o $(OBJDIR)/aimee_tls.o $(OBJDIR)/codex_auth.o \
                                              $(OBJDIR)/proxy_bootstrap.o \
                                              $(OBJDIR)/util.o \
                                        $(OBJDIR)/cJSON.o \
                                              $(OBJDIR)/log.o \
                                              $(OBJDIR)/platform_random.o \
                                              $(OBJDIR)/modules/vault/runtime_secret.o \
                                              $(GATEWAY_PLATFORM_OBJS) \
                                              $(CORE_CONNECTION_LIB)
	$(TESTLINK) -o $@ $^ $(L_GATEWAY)

$(TESTPREFIX)/unit-test-mcp-gateway-tools: $(OBJDIR)/tests/test_mcp_gateway_tools.o \
                                            $(OBJDIR)/modules/protocols/mcp/mcp_tools_gateway.o \
                                            $(OBJDIR)/server/server_mcp_gateway.o \
                                            $(OBJDIR)/delivery_target.o \
                                            $(OBJDIR)/util.o \
                                        $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-gateway-stt-pairing: $(OBJDIR)/tests/test_gateway_stt_pairing.o \
                                             $(OBJDIR)/gateway/stt.o \
                                             $(OBJDIR)/gateway/pairing.o \
                                             $(OBJDIR)/gateway/mirror.o \
                                             $(OBJDIR)/log.o \
                                             $(OBJDIR)/platform_random.o \
                                             $(OBJDIR)/posix/agent_bridge.o \
                                             $(OBJDIR)/proxy_bootstrap.o \
                                             $(OBJDIR)/util.o \
                                        $(OBJDIR)/cJSON.o \
                                             $(OBJDIR)/modules/vault/runtime_secret.o \
                                             $(GATEWAY_PLATFORM_OBJS) \
                                             $(CORE_CONNECTION_LIB)
	$(TESTLINK) -o $@ $^ $(L_GATEWAY)

$(TESTPREFIX)/unit-test-cron-runtime: $(OBJDIR)/tests/test_cron_runtime.o \
                                      $(OBJDIR)/server/server_cron.o \
                                      $(OBJDIR)/util.o \
                                      $(OBJDIR)/server/trigger_scheduler.o \
                                      $(OBJDIR)/modules/workflows/gw_orch_workflows.o \
                                      $(OBJDIR)/pipeline/gw_orchestration_seam.o \
                                      $(OBJDIR)/delivery_target.o \
                                      $(OBJDIR)/json_fluent.o \
                                      $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-report-enrichment: $(OBJDIR)/tests/test_report_enrichment.o \
                                           $(OBJDIR)/report_enrichment.o $(OBJDIR)/util_url.o \
                                           $(OBJDIR)/util.o \
                                        $(OBJDIR)/cJSON.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-hardware-probe: $(OBJDIR)/tests/test_hardware_probe.o \
                                        $(OBJDIR)/hardware_probe.o $(DB1_TEST_OBJS) $(DB1_MIGRATED_OBJS) \
                                        $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-curator-profile: $(OBJDIR)/tests/test_curator_profile.o \
                                         $(OBJDIR)/curator_profile.o \
                                         $(OBJDIR)/hardware_probe.o $(DB1_TEST_OBJS) $(DB1_MIGRATED_OBJS) \
                                         $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-client-cache: $(OBJDIR)/tests/test_kb_client_cache.o \
                                         $(OBJDIR)/modules/kb_client/kb_client_cache.o \
                                         $(OBJDIR)/log.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-openai-runs-store: $(OBJDIR)/tests/test_openai_runs_store.o $(OBJDIR)/modules/governance/gw_stage_completion.o \
                                           $(OBJDIR)/server/openai_runs_store.o \
                                           $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# The differential proof that a served argument spec builds the same body the
# compiled marshaller does. Links the real cli_v1_routes*.o so it compares
# against the SHIPPING marshallers, not a copy of them.
$(TESTPREFIX)/unit-test-cli-argspec: $(OBJDIR)/tests/test_cli_argspec.o \
                                     $(OBJDIR)/cli_argspec.o \
                                     $(OBJDIR)/posix/cli_client.o $(OBJDIR)/cli_v1_routes.o $(OBJDIR)/cli_v1_routes_b.o $(OBJDIR)/modules/workspace/workspace_client_diff.o $(OBJDIR)/cli_v1_routes_c.o $(OBJDIR)/cli_v1_routes_d.o $(OBJDIR)/aimee_client.o $(OBJDIR)/http_uds_client.o $(OBJDIR)/aimee_tls.o $(OBJDIR)/codex_auth.o $(OBJDIR)/cli_v1_routes_e.o $(OBJDIR)/cli_v1_routes_f.o \
                                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-cli-http-transport: $(OBJDIR)/tests/test_cli_http_transport.o \
                                            $(OBJDIR)/posix/cli_client.o $(OBJDIR)/cli_v1_routes.o $(OBJDIR)/cli_v1_routes_b.o $(OBJDIR)/cli_argspec.o $(OBJDIR)/modules/workspace/workspace_client_diff.o $(OBJDIR)/cli_v1_routes_c.o $(OBJDIR)/cli_v1_routes_d.o $(OBJDIR)/aimee_client.o $(OBJDIR)/http_uds_client.o $(OBJDIR)/aimee_tls.o $(OBJDIR)/codex_auth.o $(OBJDIR)/cli_v1_routes_e.o $(OBJDIR)/cli_v1_routes_f.o $(OBJDIR)/aimee_tls.o $(OBJDIR)/codex_auth.o \
                                            $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# The tool-call rescue parser and its golden corpus moved with the rule to
# server-go/modules/delegates (rescue*.go, testdata/xml_fallback_golden.json).
# The generator that used to refresh that corpus from the C parser is gone with
# it: C no longer implements the dialects, so regenerating would have quietly
# emptied the very corpus the port is pinned against.

$(TESTPREFIX)/unit-test-agent-policy-intercept: $(OBJDIR)/tests/test_agent_policy_intercept.o \
                                                $(OBJDIR)/server/agent_policy_intercept.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-http-retry: $(OBJDIR)/tests/test_http_retry.o $(OBJDIR)/server/http_retry.o $(OBJDIR)/server/failover.o \
                            $(OBJDIR)/modules/db1/interaction_events.o \
                            $(OBJDIR)/server/model_provider.o $(OBJDIR)/server/openai_profile.o \
                            $(OBJDIR)/server/anthropic_profile.o                            $(OBJDIR)/server/openrouter_profile.o $(OBJDIR)/server/ollama_profile.o \
                            $(OBJDIR)/server/llama_native_profile.o $(OBJDIR)/server/mistral_profile.o \
                            $(OBJDIR)/server/minimax_profile.o \
                            $(OBJDIR)/server/agent_bridge.o $(OBJDIR)/server/anthropic_shape.o $(OBJDIR)/server/tool_call_args.o $(OBJDIR)/server/agent_request_shaping.o \
                            $(OBJDIR)/posix/agent_bridge.o $(TEST_CORE_OBJS) $(OBJDIR)/interaction_event_names.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-cmd-doctor: $(OBJDIR)/tests/test_cmd_doctor.o $(OBJDIR)/cmd_doctor.o $(OBJDIR)/agent_tier_lint.o \
                      $(OBJDIR)/model_registry.o $(OBJDIR)/models_dev.o $(OBJDIR)/models_dev_cache.o \
                            $(OBJDIR)/hardware_probe.o \
                            $(DB2_OBJS) \
                            $(OBJDIR)/cmd_util.o $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA) \
                            $(OBJDIR)/client_integrations.o $(OBJDIR)/modules/db1/secrets.o \
                            $(OBJDIR)/modules/kb_client/kb_client.o $(OBJDIR)/modules/kb_client/kb_client_cache.o $(OBJDIR)/modules/kb_client/kb_client_index.o $(OBJDIR)/code_collect.o $(OBJDIR)/modules/kb_client/kb_client_memory.o $(OBJDIR)/modules/kb_client/kb_client_memory_audit.o $(OBJDIR)/modules/kb_client/kb_client_memory_mutations.o $(OBJDIR)/modules/kb_client/kb_client_agent.o $(OBJDIR)/modules/kb_client/kb_client_dashboard.o $(OBJDIR)/modules/kb_client/kb_client_tasks.o $(OBJDIR)/modules/kb_client/kb_client_data.o $(OBJDIR)/cli_client.o $(OBJDIR)/cli_v1_routes.o $(OBJDIR)/cli_v1_routes_b.o $(OBJDIR)/cli_argspec.o $(OBJDIR)/modules/workspace/workspace_client_diff.o $(OBJDIR)/cli_v1_routes_c.o $(OBJDIR)/cli_v1_routes_d.o $(OBJDIR)/aimee_client.o $(OBJDIR)/http_uds_client.o $(OBJDIR)/aimee_tls.o $(OBJDIR)/codex_auth.o $(OBJDIR)/cli_v1_routes_e.o $(OBJDIR)/cli_v1_routes_f.o $(OBJDIR)/posix/cli_client.o $(OBJDIR)/aimee_tls.o $(OBJDIR)/codex_auth.o \
                            $(OBJDIR)/modules/git/mcp_git_query.o $(OBJDIR)/tests/support/git_cred_inject_stub.o $(OBJDIR)/modules/git/forge_credentials.o $(OBJDIR)/modules/git/git_host_resolve.o $(OBJDIR)/modules/git/mcp_git_write.o $(OBJDIR)/modules/git/mcp_git_integrate.o \
                            $(OBJDIR)/modules/git/mcp_git_branch.o $(OBJDIR)/modules/git/mcp_git_pr.o $(OBJDIR)/tests/support/git_pr_api_stub.o $(OBJDIR)/modules/git/git_pr_ci_grade.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-cmd-onboard: $(OBJDIR)/tests/test_cmd_onboard.o \
                      $(OBJDIR)/model_registry.o $(OBJDIR)/models_dev.o $(OBJDIR)/models_dev_cache.o \
                            $(OBJDIR)/cmd_onboard.o $(OBJDIR)/cmd_core.o $(OBJDIR)/cmd_init.o \
                            $(OBJDIR)/cmd_doctor.o $(OBJDIR)/agent_tier_lint.o $(OBJDIR)/hardware_probe.o $(OBJDIR)/cmd_util.o \
                            $(DB2_OBJS) \
                            $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA) \
                            $(OBJDIR)/client_integrations.o $(OBJDIR)/modules/db1/secrets.o \
                            $(OBJDIR)/modules/git/mcp_git_query.o $(OBJDIR)/tests/support/git_cred_inject_stub.o $(OBJDIR)/modules/git/forge_credentials.o $(OBJDIR)/modules/git/git_host_resolve.o $(OBJDIR)/modules/git/mcp_git_write.o $(OBJDIR)/modules/git/mcp_git_integrate.o \
                            $(OBJDIR)/modules/git/mcp_git_branch.o $(OBJDIR)/modules/git/mcp_git_pr.o $(OBJDIR)/tests/support/git_pr_api_stub.o $(OBJDIR)/modules/git/git_pr_ci_grade.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-diff: $(OBJDIR)/tests/test_diff.o $(OBJDIR)/diff.o $(OBJDIR)/dstr.o \
                      $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-anchor-snapshot: $(OBJDIR)/tests/test_anchor_snapshot.o \
                      $(OBJDIR)/anchor_snapshot.o $(OBJDIR)/dstr.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-edit-anchored: $(OBJDIR)/tests/test_edit_anchored.o \
                      $(OBJDIR)/edit_anchored.o $(OBJDIR)/anchor_snapshot.o $(OBJDIR)/dstr.o \
                      $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-workspace-provider: $(OBJDIR)/tests/test_workspace_provider.o \
                      $(OBJDIR)/posix/workspace_provider.o $(OBJDIR)/posix/util.o $(OBJDIR)/util.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-workspace-handle: $(OBJDIR)/tests/test_workspace_handle.o \
                      $(OBJDIR)/modules/workspace/workspace_handle.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-forge-credentials: $(OBJDIR)/tests/test_forge_credentials.o \
                      $(OBJDIR)/modules/git/forge_credentials.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

# Forge App installation-token provider: JWT shape, token-response parse,
# refresh decision, and mint/cache/refresh against a mock agent_http_post.
$(TESTPREFIX)/unit-test-forge-app-token: $(OBJDIR)/tests/test_forge_app_token.o \
                      $(OBJDIR)/forge_app_token.o \
                      $(OBJDIR)/server/oauth_pkce.o \
                      $(OBJDIR)/tests/support/mock_agent_http.o \
                      $(OBJDIR)/cJSON.o \
                      $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-workspace-mirror: $(OBJDIR)/tests/test_workspace_mirror.o \
                      $(OBJDIR)/modules/workspace/workspace_mirror.o $(OBJDIR)/aimee_home.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-workspace-client-base: $(OBJDIR)/tests/test_workspace_client_base.o \
                      $(OBJDIR)/modules/workspace/workspace_client_diff.o $(OBJDIR)/posix/util.o \
                      $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-workspace-provider-detached: \
                      $(OBJDIR)/tests/test_workspace_provider_detached.o \
                      $(OBJDIR)/modules/workspace/workspace_provider_detached.o \
                      $(OBJDIR)/posix/workspace_provider.o $(OBJDIR)/posix/util.o $(OBJDIR)/util.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-webuser-runtime: \
                      $(OBJDIR)/tests/test_webuser_runtime.o \
                      $(OBJDIR)/modules/webuser/webuser_runtime.o \
                      $(OBJDIR)/tests/support/webuser_name_validator.o \
                      $(OBJDIR)/tests/module_handlers/workspace.o \
                      $(OBJDIR)/aimee_home.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-cli-kb-smoke: \
                      $(OBJDIR)/tests/test_cli_kb_smoke.o \
                      $(OBJDIR)/cli_kb_smoke.o \
                      $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-workspace-scope: \
                      $(OBJDIR)/tests/test_workspace_scope.o \
                      $(OBJDIR)/modules/workspace/workspace_scope.o \
                      $(OBJDIR)/tests/module_handlers/workspace.o \
                      $(OBJDIR)/aimee_home.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-workspace-migration: \
                      $(OBJDIR)/tests/test_workspace_migration.o \
                      $(OBJDIR)/modules/workspace/workspace_scope.o \
                      $(OBJDIR)/aimee_home.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-workspace-turn: $(OBJDIR)/tests/test_workspace_turn.o \
                      $(OBJDIR)/modules/workspace/workspace_turn.o $(OBJDIR)/modules/workspace/workspace_provider_container.o $(OBJDIR)/modules/delegates/delegate_backend.o $(OBJDIR)/tests/support/git_cred_inject_stub.o \
                      $(OBJDIR)/modules/workspace/workspace_provider_detached.o \
                      $(OBJDIR)/modules/workspace/workspace_runner_registry.o $(OBJDIR)/tests/support/obs_bus_module_call_stub.o \
                      $(OBJDIR)/modules/workspace/workspace_runner_queue.o \
                      $(OBJDIR)/modules/workspace/workspace_mirror.o $(OBJDIR)/modules/git/forge_credentials.o $(OBJDIR)/modules/git/git_host_resolve.o \
                      $(OBJDIR)/posix/workspace_provider.o $(OBJDIR)/posix/util.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-manuscript: $(OBJDIR)/tests/test_manuscript.o $(OBJDIR)/manuscript.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-notes: $(OBJDIR)/tests/test_notes.o \
                       $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-cmd-cancel: $(OBJDIR)/tests/test_cmd_cancel.o \
                            $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA) \
                            $(OBJDIR)/cmd_cancel.o $(OBJDIR)/cmd_util.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-cmd-delegate: $(OBJDIR)/tests/test_cmd_delegate.o \
                      $(OBJDIR)/tests/support/delegate_role_seam_stub.o \
                             $(OBJDIR)/modules/delegates/delegate_depth.o $(OBJDIR)/modules/delegates/delegate_role.o \
                             $(OBJDIR)/role_templates.o \
                             $(OBJDIR)/modules/delegates/delegate_prompt.o $(OBJDIR)/modules/delegates/delegate_routing.o $(OBJDIR)/modules/delegates/delegate_launch_args.o \
                             $(OBJDIR)/modules/delegates/delegate_checkout.o $(OBJDIR)/cJSON.o \
                             $(OBJDIR)/tests/module_handlers/delegates.o \
                             $(OBJDIR)/util.o $(OBJDIR)/posix/platform_process.o $(OBJDIR)/posix/util.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-delegate-plan: $(OBJDIR)/tests/test_delegate_plan.o \
                             $(OBJDIR)/modules/delegates/delegate_plan.o $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-delegate-role: $(OBJDIR)/tests/test_delegate_role.o \
                             $(OBJDIR)/modules/delegates/delegate_role.o $(OBJDIR)/role_templates.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

# The C side of the resolved permission set: reading the module's answer into a
# fixed-size held set. Minimal by design — the seam and the wire, nothing else.
$(TESTPREFIX)/unit-test-delegate-permissions: $(OBJDIR)/tests/test_delegate_permissions.o \
                             $(OBJDIR)/modules/delegates/delegate_launch_args.o \
                             $(OBJDIR)/log.o $(OBJDIR)/aimee_home.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

# Core panel-provider boundary (#1845). Part of the roundtable-profiles CI gate's
# "required core without roundtable" set; rule restored after the module
# restructuring dropped it.
$(TESTPREFIX)/unit-test-panel-provider: $(OBJDIR)/tests/test_panel_provider.o \
                                       $(OBJDIR)/modules/delegates/panel_provider.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Trigger end-to-end: the REAL scan_proposals (textually included, real git
# subprocesses + real DB1) files a work item from a committed pending proposal
# and the real autonomy scheduler drives it to terminal (stub executors only).
$(TESTPREFIX)/unit-test-trigger-e2e: $(OBJDIR)/tests/test_trigger_e2e.o \
                                    $(OBJDIR)/modules/workflows/gw_orch_workflows.o $(OBJDIR)/pipeline/gw_orchestration_seam.o \
                                    $(OBJDIR)/modules/workflows/wfe_scheduler.o $(OBJDIR)/modules/workflows/wfe_autonomy.o \
                                    $(OBJDIR)/tests/support/log_stub.o \
                                    $(OBJDIR)/modules/workflows/wfe_blocks.o $(OBJDIR)/modules/workflows/wfe_engine.o $(OBJDIR)/tests/support/config_autonomy_stub.o \
                                    $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                                    $(OBJDIR)/modules/db1/wfe_store.o $(OBJDIR)/modules/workflows/wfe_def.o \
                                    $(OBJDIR)/wfe_store_lists.o \
                                    $(OBJDIR)/modules/workflows/wfe_iface.o $(OBJDIR)/modules/workflows/wfe_validate.o \
                                    $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o $(OBJDIR)/modules/workflows/wfe_custom.o \
                                    $(OBJDIR)/modules/workflows/wfe_roundtable.o $(OBJDIR)/modules/workflows/wfe_approval.o \
                                    $(OBJDIR)/modules/workflows/wfe_verdict.o $(OBJDIR)/module_json_call.o $(OBJDIR)/tests/support/module_bus_stub.o $(OBJDIR)/modules/workflows/wfe_deliver.o \
                                    $(OBJDIR)/modules/workflows/wfe_manager_artifacts.o \
                                    $(OBJDIR)/aimee_home.o $(OBJDIR)/util.o $(OBJDIR)/posix/util.o \
                                    $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)
$(OBJDIR)/tests/test_trigger_e2e.o: tests/test_trigger_e2e.c server/trigger_scheduler.c
	@mkdir -p $(dir $@)
	$(CC) -c $(TEST_C_FLAGS) -I. -o $@ $<

$(TESTPREFIX)/unit-test-trigger: $(OBJDIR)/tests/test_trigger.o $(OBJDIR)/modules/workflows/gw_orch_workflows.o $(OBJDIR)/pipeline/gw_orchestration_seam.o $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)
$(OBJDIR)/tests/test_trigger.o: tests/test_trigger.c server/trigger_scheduler.c
	@mkdir -p $(dir $@)
	$(CC) -c $(TEST_C_FLAGS) -I. -o $@ $<


$(TESTPREFIX)/unit-test-kb-maintenance: $(OBJDIR)/tests/test_kb_maintenance.o \
                             $(OBJDIR)/db2/kb_maintenance.o \
                             $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                             $(OBJDIR)/db2/feature_rows.o $(OBJDIR)/kb/kb_mdl.o \
                             $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                             $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm -lzstd

$(TESTPREFIX)/unit-test-kb-mining: $(OBJDIR)/tests/test_kb_mining.o \
                             $(OBJDIR)/kb/kb_mining.o $(OBJDIR)/kb/kb_background.o \
                             $(OBJDIR)/kb/kb_mdl.o \
                             $(OBJDIR)/kb/kb_reasoning.o \
                             $(OBJDIR)/db2/mining.o $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                             $(OBJDIR)/db2/feature_rows.o \
                             $(OBJDIR)/modules/learning/learning_evidence.o $(OBJDIR)/db2/learning_synth_ops.o \
                             $(OBJDIR)/db2/learning.o \
                             $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                             $(OBJDIR)/db2/feedback.o \
                             $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm -lzstd

$(TESTPREFIX)/unit-test-sse-parser: $(OBJDIR)/tests/test_sse_parser.o $(OBJDIR)/sse_parser.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-anthropic-ingress: $(OBJDIR)/tests/test_anthropic_ingress.o $(OBJDIR)/server/anthropic_ingress.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-anthropic-http: $(OBJDIR)/tests/test_anthropic_http.o $(OBJDIR)/modules/governance/gw_stage_completion.o $(OBJDIR)/modules/memory/gw_stage_memory.o $(OBJDIR)/modules/ir/aimee_ir.o $(OBJDIR)/pipeline/gw_stage_registry.o $(OBJDIR)/server/anthropic_ingress.o $(OBJDIR)/sse_parser.o $(OBJDIR)/json_fluent.o $(OBJDIR)/cJSON.o $(OBJDIR)/modules/gateway/gateway_pipeline.o $(OBJDIR)/tests/support/ir_ingress_stubs.o $(OBJDIR)/wire_fence.o $(OBJDIR)/modules/translation/aimee_ir_stream.o $(OBJDIR)/modules/ir/aimee_ir_metrics.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL) -lcrypto

# P2c (response-side tool policing) integration test: same source as
# unit-test-anthropic-http but linked against the REAL modules/gateway/gateway_policy.o
# so the production police function runs against the driver's parsed
# response. The shape tests stub the request-side policy helpers so they
# don't have to deal with guardrails dependencies; this test exercises the
# full wiring end-to-end.
$(TESTPREFIX)/unit-test-anthropic-http-p2c: $(OBJDIR)/tests/test_anthropic_http_p2c.o $(OBJDIR)/modules/governance/gw_stage_completion.o $(OBJDIR)/modules/memory/gw_stage_memory.o $(OBJDIR)/modules/ir/aimee_ir.o $(OBJDIR)/modules/governance/gw_stage_governance.o $(OBJDIR)/pipeline/gw_response_registry.o $(OBJDIR)/pipeline/gw_stage_registry.o $(OBJDIR)/server/anthropic_ingress.o $(OBJDIR)/sse_parser.o $(OBJDIR)/json_fluent.o $(OBJDIR)/cJSON.o $(OBJDIR)/modules/gateway/gateway_pipeline.o $(OBJDIR)/modules/gateway/gateway_policy.o $(OBJDIR)/tests/support/ir_ingress_stubs.o $(OBJDIR)/wire_fence.o $(OBJDIR)/modules/translation/aimee_ir_stream.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL) -lcrypto

# P2c streaming integration test: linked against the REAL modules/gateway/gateway_policy.o
# so the streaming policy branch in messages_stream runs as in production.
# Same minimal-link pattern as the buffered P2c test above; the SSE replay
# helper + police function exercise the buffered-fetch + replay flow when
# `gateway_prevent_subagents` is ON.
$(TESTPREFIX)/unit-test-anthropic-http-streaming-p2c: $(OBJDIR)/tests/test_anthropic_http_streaming_p2c.o $(OBJDIR)/modules/governance/gw_stage_completion.o $(OBJDIR)/modules/memory/gw_stage_memory.o $(OBJDIR)/modules/ir/aimee_ir.o $(OBJDIR)/modules/governance/gw_stage_governance.o $(OBJDIR)/pipeline/gw_response_registry.o $(OBJDIR)/pipeline/gw_stage_registry.o $(OBJDIR)/server/anthropic_ingress.o $(OBJDIR)/sse_parser.o $(OBJDIR)/json_fluent.o $(OBJDIR)/cJSON.o $(OBJDIR)/modules/gateway/gateway_pipeline.o $(OBJDIR)/modules/gateway/gateway_policy.o $(OBJDIR)/tests/support/ir_ingress_stubs.o $(OBJDIR)/wire_fence.o $(OBJDIR)/modules/translation/aimee_ir_stream.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL) -lcrypto

$(TESTPREFIX)/unit-test-gateway-policy: $(OBJDIR)/tests/test_gateway_policy.o $(OBJDIR)/modules/gateway/gateway_policy.o $(OBJDIR)/json_fluent.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-gateway-pipeline: $(OBJDIR)/tests/test_gateway_pipeline.o $(OBJDIR)/modules/gateway/gateway_pipeline.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)


# Slice 7: config-driven stage registry, proven through the shared pipeline.
$(TESTPREFIX)/unit-test-gw-stage-registry: $(OBJDIR)/tests/test_gw_stage_registry.o $(OBJDIR)/pipeline/gw_stage_registry.o $(OBJDIR)/modules/gateway/gateway_pipeline.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)


# Response-stage registry + runner (Slice 1 of the response seam), self-contained.
$(TESTPREFIX)/unit-test-gw-response-registry: $(OBJDIR)/tests/test_gw_response_registry.o $(OBJDIR)/pipeline/gw_response_registry.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)


# Response governance stage (Slice 2): toggle proven via a counting police stub.
$(TESTPREFIX)/unit-test-response-governance-stage: $(OBJDIR)/tests/test_response_governance_stage.o $(OBJDIR)/modules/governance/gw_stage_governance.o $(OBJDIR)/pipeline/gw_response_registry.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

# Orchestration-hook seam + runner (Slice 3 of the response/orchestration seam), self-contained.
$(TESTPREFIX)/unit-test-gw-orchestration-seam: $(OBJDIR)/tests/test_gw_orchestration_seam.o $(OBJDIR)/pipeline/gw_orchestration_seam.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

# Delegates orchestration module (first port): toggle + module runner over the seam, proven
# with a fake capability (no real delegate thread). Self-contained.
$(TESTPREFIX)/unit-test-gw-orch-delegates: $(OBJDIR)/tests/test_gw_orch_delegates.o $(OBJDIR)/modules/delegates/gw_orch_delegates.o $(OBJDIR)/pipeline/gw_orchestration_seam.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

# Workflows orchestration module (second port): toggle + module runner over the seam, proven
# with a fake capability (no real work item created). Self-contained.
$(TESTPREFIX)/unit-test-gw-orch-workflows: $(OBJDIR)/tests/test_gw_orch_workflows.o $(OBJDIR)/modules/workflows/gw_orch_workflows.o $(OBJDIR)/pipeline/gw_orchestration_seam.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-gateway-p4-delegate: $(OBJDIR)/tests/test_gateway_p4_delegate.o $(OBJDIR)/modules/gateway/gateway_delegate.o $(OBJDIR)/modules/gateway/gateway_policy.o $(OBJDIR)/modules/gateway/gateway_pipeline.o $(OBJDIR)/json_fluent.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(OBJDIR)/tests/%.o: tests/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $(TEST_C_FLAGS) -o $@ $<

# Dependency tracking for test objects. The top-level DEPS = $(ALL_OBJS:.o=.d)
# only covers production objects; test objects are built by these rules, so
# their .d files must be -included here too. These .d files (generated by
# -MMD -MP in TEST_C_FLAGS) capture both #included .inc fixtures and any .c
# sources a test cross-compiles, so editing e.g. a tests/*.inc fixture rebuilds
# the test object that #includes it. Wildcard so missing files (first build,
# before any .d exists) are simply skipped.
-include $(wildcard $(OBJDIR)/tests/*.d)
-include $(wildcard $(OBJDIR)/tests/support/*.d)
-include $(wildcard $(OBJDIR)/tests/server/*.d)

$(OBJDIR)/tests/modules/kb_client/kb_client_tool_registry.o: modules/kb_client/kb_client_tool_registry.c
	@mkdir -p $(dir $@)
	$(CC) -c $(TEST_C_FLAGS) -o $@ $<


$(TESTPREFIX)/unit-test-hud: $(OBJDIR)/tests/test_hud.o $(OBJDIR)/hud.o \
                      $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-history: $(OBJDIR)/tests/test_history.o $(OBJDIR)/history.o $(OBJDIR)/cJSON.o \
                         $(OBJDIR)/util.o $(OBJDIR)/text.o \
                         $(OBJDIR)/modules/db1/db.o $(OBJDIR)/modules/db1/db_schema.o $(DB2_TEST_BACKEND_OBJ) $(OBJDIR)/db2/db2_test_shim.o $(CONFIG_CLIENT_TEST_OBJS) \
                         $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/platform_random.o \
                         $(OBJDIR)/log.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-events: $(OBJDIR)/tests/test_events.o $(OBJDIR)/events.o \
                                $(OBJDIR)/delivery_target.o $(TEST_CORE_OBJS) \
                                $(OBJDIR)/posix/events.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-roadmap-auto: $(OBJDIR)/tests/test_roadmap_auto.o \
                            $(OBJDIR)/modules/roadmap/roadmap_milestone.o \
                            $(OBJDIR)/modules/roadmap/roadmap_reassess.o \
                            $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-coord-jobs: $(OBJDIR)/tests/test_coord_jobs.o \
                            $(OBJDIR)/modules/delegates/delegate_economics.o \
                            $(OBJDIR)/modules/delegates/delegate_prompt.o \
                            $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# deploy_apply.c is #included by the test for its statics, and the two config
# symbols it calls are stubbed there — so this links nothing else but cJSON,
# which deploy_apply.c uses to scope `docker compose ps` to the managed services.
$(TESTPREFIX)/unit-test-deploy-apply: $(OBJDIR)/tests/test_deploy_apply.o \
                                      $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lpthread

$(TESTPREFIX)/unit-test-plan-waves: $(OBJDIR)/tests/test_plan_waves.o \
                            $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-file-ref: $(OBJDIR)/tests/test_file_ref.o \
                           $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-role-templates: $(OBJDIR)/tests/test_role_templates.o \
                               $(OBJDIR)/role_templates.o $(TEST_CORE_OBJS) \
                               $(OBJDIR)/dstr.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-skill: $(OBJDIR)/tests/test_skill.o \
                               $(OBJDIR)/modules/skills/skill.o $(OBJDIR)/modules/skills/skill_rollback.o \
                               $(OBJDIR)/modules/skills/skill_trigger_policy.o $(TEST_CORE_OBJS) \
                               $(OBJDIR)/dstr.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-web-search: $(OBJDIR)/tests/test_web_search.o \
                            $(OBJDIR)/server/web_search.o $(TEST_CORE_OBJS) \
                                    $(OBJDIR)/server/web_search_fuse.o $(OBJDIR)/server/web_search_breaker.o $(OBJDIR)/rrf.o \
                            $(OBJDIR)/dstr.o $(OBJDIR)/server/agent_bridge.o $(OBJDIR)/server/anthropic_shape.o $(OBJDIR)/server/tool_call_args.o \
                            $(OBJDIR)/server/agent_request_shaping.o \
                            $(OBJDIR)/posix/agent_bridge.o $(OBJDIR)/server/http_retry.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-tdd: $(OBJDIR)/tests/test_tdd.o \
                     $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-compact: $(OBJDIR)/tests/test_compact.o $(OBJDIR)/compact.o \
                                  $(OBJDIR)/cJSON.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-wire-fence: \
                                  $(OBJDIR)/tests/test_wire_fence.o \
                                  $(OBJDIR)/wire_fence.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-economizer-live-surface: tests/test_economizer_live_surface.sh
	@mkdir -p $(dir $@)
	cp $< $@
	chmod +x $@

$(TESTPREFIX)/unit-test-compact-prune: $(OBJDIR)/tests/test_compact_prune.o \
                                          $(OBJDIR)/server/compact_prune.o \
                                          $(OBJDIR)/server/session_compact.o $(OBJDIR)/server/rounds_to_resume.o $(OBJDIR)/server/compact_prune.o \
                                          $(OBJDIR)/server/agent_bridge.o $(OBJDIR)/server/anthropic_shape.o $(OBJDIR)/server/tool_call_args.o \
                                          $(OBJDIR)/server/agent_request_shaping.o \
                                          $(OBJDIR)/modules/delegates/delegate_driver.o \
                                          $(OBJDIR)/modules/delegates/delegate_openai.o \
                                          $(OBJDIR)/modules/delegates/delegate_xml_fallback.o \
                                          $(OBJDIR)/model_registry.o $(OBJDIR)/models_dev.o $(OBJDIR)/models_dev_cache.o \
                                          $(OBJDIR)/server/agent_tools.o \
                                          $(OBJDIR)/modules/delegates/delegate_role.o \
                                          $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-session-compact-focused: $(OBJDIR)/tests/test_session_compact_focused.o \
                                          $(OBJDIR)/server/session_compact.o $(OBJDIR)/server/rounds_to_resume.o $(OBJDIR)/server/compact_prune.o \
                                          $(OBJDIR)/server/agent_bridge.o $(OBJDIR)/server/anthropic_shape.o $(OBJDIR)/server/tool_call_args.o \
                                          $(OBJDIR)/server/agent_request_shaping.o \
                                          $(OBJDIR)/modules/delegates/delegate_driver.o \
                                          $(OBJDIR)/modules/delegates/delegate_openai.o \
                                          $(OBJDIR)/modules/delegates/delegate_xml_fallback.o \
                                          $(OBJDIR)/model_registry.o $(OBJDIR)/models_dev.o $(OBJDIR)/models_dev_cache.o \
                                          $(OBJDIR)/server/agent_tools.o \
                                          $(OBJDIR)/modules/delegates/delegate_role.o \
                                          $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-session-compact: $(OBJDIR)/tests/test_session_compact.o \
                                          $(OBJDIR)/server/session_compact.o $(OBJDIR)/server/rounds_to_resume.o $(OBJDIR)/server/compact_prune.o \
                                          $(OBJDIR)/server/agent_bridge.o $(OBJDIR)/server/anthropic_shape.o $(OBJDIR)/server/tool_call_args.o \
                                          $(OBJDIR)/server/agent_request_shaping.o \
                                          $(OBJDIR)/modules/delegates/delegate_driver.o \
                                          $(OBJDIR)/modules/delegates/delegate_openai.o \
                                          $(OBJDIR)/modules/delegates/delegate_xml_fallback.o \
                                          $(OBJDIR)/model_registry.o $(OBJDIR)/models_dev.o $(OBJDIR)/models_dev_cache.o \
                                          $(OBJDIR)/server/agent_tools.o \
                                          $(OBJDIR)/modules/delegates/delegate_role.o \
                                          $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-rounds-to-resume: $(OBJDIR)/tests/test_rounds_to_resume.o \
                                          $(OBJDIR)/server/rounds_to_resume.o \
                                          $(OBJDIR)/server/session_compact.o $(OBJDIR)/server/rounds_to_resume.o $(OBJDIR)/server/compact_prune.o \
                                          $(OBJDIR)/server/agent_bridge.o $(OBJDIR)/server/anthropic_shape.o $(OBJDIR)/server/tool_call_args.o \
                                          $(OBJDIR)/server/agent_request_shaping.o \
                                          $(OBJDIR)/modules/delegates/delegate_driver.o \
                                          $(OBJDIR)/modules/delegates/delegate_openai.o \
                                          $(OBJDIR)/modules/delegates/delegate_xml_fallback.o \
                                          $(OBJDIR)/model_registry.o $(OBJDIR)/models_dev.o $(OBJDIR)/models_dev_cache.o \
                                          $(OBJDIR)/server/agent_tools.o \
                                          $(OBJDIR)/modules/delegates/delegate_role.o \
                                          $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-token-tracker: $(OBJDIR)/tests/test_token_tracker.o \
                               $(OBJDIR)/server/token_tracker.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-reasoning-cap: $(OBJDIR)/tests/test_reasoning_cap.o \
                               $(OBJDIR)/reasoning_cap.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# typed-fact P1: pure ontology + write-gate (no DB), so a minimal link.
$(TESTPREFIX)/unit-test-rel-types: $(OBJDIR)/tests/test_rel_types.o \
                               $(OBJDIR)/rel_types.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

# Grounding gate: pure text check, no DB, so a minimal link like rel_types.
$(TESTPREFIX)/unit-test-memory-facts-grounding: $(OBJDIR)/tests/test_memory_facts_grounding.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-memory-fact-gate: $(OBJDIR)/tests/test_memory_fact_gate.o \
                               $(OBJDIR)/modules/memory/memory_fact_gate.o $(OBJDIR)/rel_types.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

# typed-fact P1b: DB2 store + commit path, against the sqlite shim.
$(TESTPREFIX)/unit-test-rel-types-store: $(OBJDIR)/tests/test_rel_types_store.o \
                               $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# typed-fact P2a: entity registry / alias resolution, against the sqlite shim.
$(TESTPREFIX)/unit-test-entity-registry: $(OBJDIR)/tests/test_entity_registry.o \
                               $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# typed-fact P3: confidence classes (§5) + correction/retraction (§4), shim.
# The capability grading behind the MCP `mutate` and `memory_maintain` gates.
# server_mcp_memory_gate.o is pure (macros + strcmp), so this links with only
# server_auth.o for server_capability_for_method -- deliberately, so the
# security-critical decisions are testable without linking the MCP call table.
$(TESTPREFIX)/unit-test-mcp-memory-gate: $(OBJDIR)/tests/test_mcp_memory_gate.o \
                               $(OBJDIR)/server/server_mcp_memory_gate.o \
                               $(OBJDIR)/server/server_auth.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-fact-lifecycle: $(OBJDIR)/tests/test_fact_lifecycle.o \
                               $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# embedder-runtime-fetch-autodim §2: kb_meta dim record + refuse-on-mismatch, shim.
$(TESTPREFIX)/unit-test-embedding-dim: $(OBJDIR)/tests/test_embedding_dim.o \
                               $(OBJDIR)/db2/db_schema.o $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Which probes embedder_probe_register installs for which embed command. Guards a
# registration decision, not a computation: skipping the serving-identity probe along
# with the dim probe is what made the builtin -> model switch undetectable.
$(TESTPREFIX)/unit-test-embedder-probe-register: $(OBJDIR)/tests/test_embedder_probe_register.o \
                               $(OBJDIR)/server/embedder_probe.o \
                               $(OBJDIR)/modules/memory/memory_core_helpers_b.o $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# typed-fact P4: self-extending ontology promotion pipeline (§2), shim.
$(TESTPREFIX)/unit-test-ontology-evolution: $(OBJDIR)/tests/test_ontology_evolution.o \
                               $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# typed-fact P5: pattern-first extraction (§6) + retraction scan (§4). Pure.
$(TESTPREFIX)/unit-test-extract-patterns: $(OBJDIR)/tests/test_extract_patterns.o \
                               $(OBJDIR)/modules/memory/memory_extract_patterns.o $(OBJDIR)/rel_types.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# typed-fact P5: pattern-first ingest pipeline (§6 -> §1), shim.
$(TESTPREFIX)/unit-test-fact-ingest: $(OBJDIR)/tests/test_fact_ingest.o \
                               $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-audit-worm: $(OBJDIR)/tests/test_kb_audit_worm.o \
                               $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                               $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(OBJDIR)/tests/test_kb_audit_worm.o \
$(OBJDIR)/tests/test_kb_audit_worm_pg.o \
$(OBJDIR)/tests/test_code_project_lifecycle.o: C_FLAGS += -Imodules/db2/include

$(TESTPREFIX)/unit-test-decision-log: $(OBJDIR)/tests/test_decision_log.o \
                               $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-doc-pdf: $(OBJDIR)/tests/test_kb_doc_pdf.o \
                               $(OBJDIR)/kb/kb_doc_pdf.o \
                               $(OBJDIR)/kb/kb_tsr_sidecar.o \
                               $(OBJDIR)/kb/kb_ocr_sidecar.o \
                               $(OBJDIR)/kb/kb_blob_store.o \
                               $(OBJDIR)/kb/kb_blob_reconcile.o \
                               $(OBJDIR)/kb/kb_doc_hash.o \
                               $(OBJDIR)/kb/http/kb_http_pdf.o \
                               $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# typed-fact P5: typed-fact recall + §7 PII gating into the envelope, shim.
$(TESTPREFIX)/unit-test-fact-recall: $(OBJDIR)/tests/test_fact_recall.o \
                               $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# typed-fact P5: per-attribute PII recall gating (§7). Pure.
$(TESTPREFIX)/unit-test-pii-gate: $(OBJDIR)/tests/test_pii_gate.o \
                               $(OBJDIR)/modules/memory/memory_pii_gate.o $(OBJDIR)/rel_types.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-request-context: $(OBJDIR)/tests/test_request_context.o \
                               $(OBJDIR)/server/request_context.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-response-dedup: $(OBJDIR)/tests/test_response_dedup.o \
                               $(OBJDIR)/server/response_dedup.o \
                               $(OBJDIR)/modules/response-composition/module_adapter.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-anthropic-shape: $(OBJDIR)/tests/test_anthropic_shape.o \
                               $(OBJDIR)/server/anthropic_shape.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-token-audit: $(OBJDIR)/tests/test_token_audit.o \
                              $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-token-audit-load: $(OBJDIR)/tests/test_token_audit_load.o \
                              $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-windows: $(OBJDIR)/tests/test_windows.o \
                          $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-tool-prompts: $(OBJDIR)/tests/test_tool_prompts.o \
                              $(OBJDIR)/server/agent_policy.o $(OBJDIR)/dstr.o \
                              $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-delegate-token-budget: $(OBJDIR)/tests/test_delegate_token_budget.o \
                                       $(OBJDIR)/server/agent_coord.o \
                                       $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-delegate-context-shed: $(OBJDIR)/tests/test_delegate_context_shed.o \
                                       $(OBJDIR)/modules/delegates/delegate_prompt.o \
                                       $(OBJDIR)/modules/delegates/delegate_launch_args.o \
                                       $(OBJDIR)/modules/delegates/delegate_role.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-agent-error-retryable: $(OBJDIR)/tests/test_agent_error_retryable.o \
                                       $(OBJDIR)/server/agent_fallback.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-delegate-ephemeral-ws: $(OBJDIR)/tests/test_delegate_ephemeral_ws.o \
                                       $(OBJDIR)/modules/delegates/delegate_ephemeral_ws.o \
                                       $(OBJDIR)/aimee_home.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-delegate-handoff: $(OBJDIR)/tests/test_delegate_handoff.o \
                                       $(OBJDIR)/modules/delegates/delegate_prompt.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-delegate-dispatch-reliability: \
                                       $(OBJDIR)/tests/test_delegate_dispatch_reliability.o \
                                       $(OBJDIR)/modules/delegates/delegate_prompt.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-curator-code-unit: \
                                       $(OBJDIR)/tests/test_curator_code_unit.o \
                                       $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_queue.o \
                                       $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_extract_code.o \
                                       $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_extract.o \
                                       $(OBJDIR)/db2/db2_tenant.o $(OBJDIR)/kb/kb_identity.o \
                                       $(OBJDIR)/kb_curator_provider.o \
                                       $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_sidecar.o \
                                       $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_grounding.o \
                                       $(OBJDIR)/tests/module_handlers/kb_synthesis.o \
                                       $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                                       $(OBJDIR)/db2/feature_rows.o \
                                       $(OBJDIR)/kb/kb_mdl.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-curator-resolve-entities: \
                                       $(OBJDIR)/tests/test_curator_resolve_entities.o \
                                       $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_resolve_entities.o \
                                       $(OBJDIR)/kb_curator_provider.o \
                                       $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                                       $(OBJDIR)/db2/feature_rows.o \
                                       $(OBJDIR)/kb/kb_mdl.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-curator-index-narrative: \
                                       $(OBJDIR)/tests/test_curator_index_narrative.o \
                                       $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_index_narrative.o \
                                       $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                                       $(OBJDIR)/db2/feature_rows.o \
                                       $(OBJDIR)/kb/kb_mdl.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-curator-index-claims: \
                                       $(OBJDIR)/tests/test_curator_index_claims.o \
                                       $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_index_claims.o \
                                       $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                                       $(OBJDIR)/db2/feature_rows.o \
                                       $(OBJDIR)/kb/kb_mdl.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-curator-contradictions: \
                                       $(OBJDIR)/tests/test_curator_contradictions.o \
                                       $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_contradictions.o \
                                       $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                                       $(OBJDIR)/db2/feature_rows.o \
                                       $(OBJDIR)/kb/kb_mdl.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-curator-index-code-unit: \
                                       $(OBJDIR)/tests/test_curator_index_code_unit.o \
                                       $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_index_code_unit.o \
                                       $(OBJDIR)/db2/kb_runtime_state.o \
                                       $(OBJDIR)/tests/support/kb_txn_stub.o \
                                       $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                                       $(OBJDIR)/db2/feature_rows.o \
                                       $(OBJDIR)/kb/kb_mdl.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-curator-pipeline: \
                                       $(OBJDIR)/tests/test_curator_pipeline.o \
                                       $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_resolve_entities.o \
                                       $(OBJDIR)/kb_curator_provider.o \
                                       $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_index_code_unit.o \
                                       $(OBJDIR)/db2/kb_runtime_state.o \
                                       $(OBJDIR)/tests/support/kb_txn_stub.o \
                                       $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_link_artifacts.o \
                                       $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_serve.o \
                                       $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                                       $(OBJDIR)/db2/feature_rows.o \
                                       $(OBJDIR)/kb/kb_mdl.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-curator-serve: \
                                       $(OBJDIR)/tests/test_curator_serve.o \
                                       $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_serve.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-curator-link-artifacts: \
                                       $(OBJDIR)/tests/test_curator_link_artifacts.o \
                                       $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_link_artifacts.o \
                                       $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                                       $(OBJDIR)/db2/feature_rows.o \
                                       $(OBJDIR)/kb/kb_mdl.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

# LLM judge sidecar — request build / invoke / parse round-trip via tiny shell
# "sidecars". Only depends on cJSON; no DB link.
$(TESTPREFIX)/unit-test-curator-judge: $(OBJDIR)/text.o $(OBJDIR)/tests/support/curator_config_stub.o \
                                       \
                                       $(OBJDIR)/tests/test_curator_judge.o \
                                       $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_judge.o \
                                       $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_sidecar.o \
                                       $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_llm.o \
                                       $(OBJDIR)/kb_curator_provider.o \
                                       $(OBJDIR)/provider_client.o \
                                       $(OBJDIR)/tests/support/mock_agent_http.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# §4 surprising-links judge: real request-build + verdict-parse; DB accessors stubbed
# in the test, the LLM faked via the curator sidecar seam (cfg=NULL + printf judge_cmd).
$(TESTPREFIX)/unit-test-kb-surprising-judge: $(OBJDIR)/text.o $(OBJDIR)/tests/support/curator_config_stub.o \
                                       \
                                       $(OBJDIR)/tests/test_kb_surprising_judge.o \
                                       $(OBJDIR)/kb/kb_surprising_judge.o \
                                       $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_sidecar.o \
                                       $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_llm.o \
                                       $(OBJDIR)/kb_curator_provider.o \
                                       $(OBJDIR)/provider_client.o \
                                       $(OBJDIR)/tests/support/mock_agent_http.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# synthesize_topic — graceful drain entry under the sqlite shim (gated off).
$(TESTPREFIX)/unit-test-curator-synthesize: \
                                       $(OBJDIR)/tests/test_curator_synthesize.o \
                                       $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_synthesize.o \
                                       $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_sidecar.o \
                                       $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_llm.o \
                                       $(OBJDIR)/kb_curator_provider.o \
                                       $(OBJDIR)/provider_client.o \
                                       $(OBJDIR)/tests/support/mock_agent_http.o \
                                       $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                                       $(OBJDIR)/db2/feature_rows.o \
                                       $(OBJDIR)/kb/kb_mdl.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

# idle-reflection synthesis write-gate (§3/§4): reflection routes through the
# shared curator LLM path; the LLM is faked via the sidecar command seam
# (printf fallback, no provider). Includes kb_reflection.c to reach the static
# run_synthesis_pass; graph/feature/background deps are stubbed in the test.
$(TESTPREFIX)/unit-test-kb-reflection: \
                                       $(OBJDIR)/tests/test_kb_reflection.o \
                                       $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_sidecar.o \
                                       $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_llm.o \
                                       $(OBJDIR)/kb_curator_provider.o \
                                       $(OBJDIR)/provider_client.o \
                                       $(OBJDIR)/tests/support/mock_agent_http.o \
                                       $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                                       $(OBJDIR)/db2/feature_rows.o \
                                       $(OBJDIR)/kb/kb_mdl.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

# promote_entity — pure scope-lattice step + graceful drain entry (gated off).
$(TESTPREFIX)/unit-test-curator-promote: \
                                       $(OBJDIR)/tests/test_curator_promote.o \
                                       $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_promote.o \
                                       $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                                       $(OBJDIR)/db2/feature_rows.o \
                                       $(OBJDIR)/kb/kb_mdl.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

# Curator fixture corpus (benchmarks/curator/) — schema + behavioral grounding
# checks. The fixture dir is absolute so the test runs from any cwd.
$(OBJDIR)/tests/test_curator_fixtures.o: C_FLAGS += -DCURATOR_FIXTURE_DIR=\"$(CURDIR)/../benchmarks/curator/fixtures\"
$(TESTPREFIX)/unit-test-curator-fixtures: \
                                       $(OBJDIR)/tests/test_curator_fixtures.o \
                                       $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_grounding.o \
                                       $(OBJDIR)/tests/module_handlers/kb_synthesis.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Cross-source learning substrate fixture corpus (benchmarks/learning/substrate/).
$(OBJDIR)/tests/test_substrate_fixtures.o: C_FLAGS += -DSUBSTRATE_FIXTURE_DIR=\"$(CURDIR)/../benchmarks/learning/substrate\"
$(TESTPREFIX)/unit-test-substrate-fixtures: \
                                       $(OBJDIR)/tests/test_substrate_fixtures.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-db1-write-retry: \
                                       $(OBJDIR)/tests/test_db1_write_retry.o \
                                       $(OBJDIR)/modules/db1/db1_write.o \
                                       $(OBJDIR)/modules/db1/db_schema.o \
                                       $(OBJDIR)/modules/db1/db.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-db1-agent-job-heartbeat: \
                                       $(OBJDIR)/tests/test_db1_agent_job_heartbeat.o \
                                       $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                                       $(OBJDIR)/modules/db1/agent_jobs.o $(OBJDIR)/agent_job_release.o $(OBJDIR)/modules/db1/agent_log.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-db1-agent-job-cancel-unassigned: \
                                       $(OBJDIR)/tests/test_db1_agent_job_cancel_unassigned.o \
                                       $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                                       $(OBJDIR)/modules/db1/agent_jobs.o $(OBJDIR)/agent_job_release.o $(OBJDIR)/modules/db1/agent_log.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lpthread

$(TESTPREFIX)/unit-test-db1-delegate-reservation: \
                                       $(OBJDIR)/tests/test_db1_delegate_reservation.o \
                                       $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                                       $(OBJDIR)/modules/db1/delegate_reservation.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-server-delegate-monitor: \
                                       $(OBJDIR)/tests/test_server_delegate_monitor.o \
                                       $(OBJDIR)/server/server_delegate_monitor.o \
                                       $(OBJDIR)/server/agent_admission.o \
                                       $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                                       $(OBJDIR)/modules/db1/agent_jobs.o $(OBJDIR)/agent_job_release.o $(OBJDIR)/log.o \
                                       $(OBJDIR)/dstr.o $(OBJDIR)/util.o $(OBJDIR)/text.o \
                                       $(OBJDIR)/platform_random.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-db1-delegation-recursive-cancel: \
                                       $(OBJDIR)/tests/test_db1_delegation_recursive_cancel.o \
                                       $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                                       $(OBJDIR)/modules/db1/delegations.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-tool-args-coerce: \
                                       $(OBJDIR)/tests/test_tool_args_coerce.o \
                                       $(OBJDIR)/server/tool_args_coerce.o \
                                       $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-tool-schema-sanitizer: \
                                       $(OBJDIR)/tests/test_tool_schema_sanitizer.o \
                                       $(OBJDIR)/server/tool_schema_sanitizer.o \
                                       $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-workspace-provider-container: \
                                       $(OBJDIR)/tests/test_workspace_provider_container.o \
                                       $(OBJDIR)/modules/workspace/workspace_provider_container.o \
                                       $(OBJDIR)/posix/workspace_provider.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-toolset-thread-scope: \
                      $(OBJDIR)/tests/support/role_template_toolset_stub.o \
                                       $(OBJDIR)/tests/test_toolset_thread_scope.o \
                                       $(OBJDIR)/server/agent_tools.o \
                                       $(OBJDIR)/modules/delegates/delegate_role.o \
                                       $(OBJDIR)/toolset.o \
                                       $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-toolset: \
                                       $(OBJDIR)/tests/test_toolset.o \
                                       $(OBJDIR)/toolset.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# The module adapter carries the stage handler; the bus objects come with
# module_runtime.h's declarations. db1_init/db_schema/checkpoints give it a real
# store to round-trip through, because a handler that only parses proves nothing
# about what a caller gets back.
# The bus client the daemon links in place of the domain. Stubs the bus: the
# framing and the result mapping are what this pins, and the module that answers
# these frames is covered by unit-test-db1-module-stage.
$(TESTPREFIX)/unit-test-db1-git-ownership-client: \
                                       $(OBJDIR)/tests/test_db1_git_ownership_client.o \
                                       $(OBJDIR)/db1_client/git_ownership.o \
                                       $(OBJDIR)/module_json_call.o $(OBJDIR)/log.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

# The composition, not either half: the real module binary, the real bus, and
# the ordinary generated clients the daemon links. Deliberately links
# DB1_CLIENT_OBJS and NOT the DB1 domain -- linking the domain would answer
# every call in-process and prove nothing about the module.
$(TESTPREFIX)/unit-test-db1-module-bus: \
                                       $(OBJDIR)/tests/test_db1_module_bus.o \
                                       $(DB1_CLIENT_OBJS) \
                                       $(OBJDIR)/agent_job_release.o \
                                       $(OBJDIR)/primary_session_release.o \
                                       $(OBJDIR)/model_catalog_release.o \
                                       $(OBJDIR)/ensemble_view.o \
                                       $(OBJDIR)/execution_plan_list.o \
                                       $(OBJDIR)/remote_client_claim.o \
                                       $(OBJDIR)/jti_replay_consume.o \
                                       $(OBJDIR)/wfe_store_lists.o \
                                       $(OBS_BUS_LINK_OBJS) \
                                       $(OBJDIR)/core/event_bus/bus_client.o \
                                       $(OBJDIR)/core/event_bus/bus_attach.o \
                                       $(OBJDIR)/core/event_bus/bus_host.o \
                                       $(OBJDIR)/core/event_bus/bus_route.o \
                                       $(OBJDIR)/core/event_bus/bus_region.o \
                                       $(OBJDIR)/core/event_bus/bus_region_host.o \
                                       $(OBJDIR)/core/event_bus/bus_ring.o \
                                       $(OBJDIR)/core/event_bus/bus_arena.o \
                                       $(OBJDIR)/core/event_bus/bus_wire.o \
                                       $(OBJDIR)/core/event_bus/bus_capture.o \
                                       $(OBJDIR)/module_json_call.o $(OBJDIR)/log.o \
                                       $(CONFIG_CLIENT_TEST_OBJS) \
                                       $(OBJDIR)/platform_random.o \
                                       $(OBJDIR)/posix/platform_random.o \
                                       $(OBJDIR)/aimee_home.o \
                                       $(OBJDIR)/posix/platform_path.o \
                                       | $(OBJDIR)/aimee-module-db1
	$(TESTLINK_MIN) -o $@ $^ $(EXTRA_L_FLAGS) -lpthread

.PHONY: unit-test-db1-module-bus
unit-test-db1-module-bus: $(TESTPREFIX)/unit-test-db1-module-bus $(OBJDIR)/aimee-module-db1
	$< $(OBJDIR)/aimee-module-db1

# The module binary is produced by the bundle exporter, not this Makefile: the
# process contract owns which sources a module compiles, and duplicating that
# list here would let the two drift.
#
# The prerequisites are deliberately a wildcard rather than the descriptor's
# source list, for the same reason: naming the sources here is the duplication
# the comment above refuses. A wildcard rebuilds more often than strictly
# needed and never less, which is the safe direction. With no prerequisites at
# all -- as this rule stood -- the binary was built once and never again, so
# every later run of the fixture tested a stale module and reported PASS for
# code that was not in it.
# The module's schema is an input to the module, and leaving it out meant a
# schema change rebuilt the daemon and not the process that actually applies it:
# the store came up on the old tables and the only symptom was a column that was
# not there.
DB1_MODULE_INPUTS = $(wildcard modules/db1/*.c) $(wildcard modules/db1/*.h) \
                    $(wildcard modules/db1/*.sql) \
                    modules/db1/module.yaml \
                    modules/db1/eventcontract/operations.json \
                    vendor/cJSON.c \
                    ../scripts/export_c_repositories.py \
                    ../scripts/build_c_module_runtime_bundle.py

$(OBJDIR)/aimee-module-db1: $(DB1_MODULE_INPUTS)
	@rm -rf $(OBJDIR)/module-bundle
	@python3 ../scripts/export_c_repositories.py --runtime-bundle $(abspath $(OBJDIR))/module-bundle >/dev/null
	@python3 ../scripts/build_c_module_runtime_bundle.py --bundle $(abspath $(OBJDIR))/module-bundle --output $(abspath $(OBJDIR)) --placement server >/dev/null

# DB2 is granted but not started by default in the KB image. This target proves
# that its complete descriptor-owned source set still produces the executable
# the grant names. The descriptor generates schema_data.h in the bundle build
# directory, so this proof does not depend on a prior monolithic build.
$(OBJDIR)/aimee-module-db2:
	@rm -rf $(OBJDIR)/db2-module-bundle
	@python3 ../scripts/export_c_repositories.py --runtime-bundle $(abspath $(OBJDIR))/db2-module-bundle >/dev/null
	@python3 ../scripts/build_c_module_runtime_bundle.py --bundle $(abspath $(OBJDIR))/db2-module-bundle --output $(abspath $(OBJDIR)) --placement kb >/dev/null

# Replay cataloged inactive-family operations against the exact exported DB2
# source closure without adding their grants to the production process contract.
$(OBJDIR)/aimee-module-db2-replay: tests/support/db2_module_replay_main.c
	@rm -rf $(OBJDIR)/db2-replay-bundle $(OBJDIR)/db2-replay-output
	@python3 ../scripts/export_c_repositories.py --runtime-bundle $(abspath $(OBJDIR))/db2-replay-bundle >/dev/null
	@cp $< $(OBJDIR)/db2-replay-bundle/src/aimee-module-db2.c
	@python3 ../scripts/build_c_module_runtime_bundle.py --bundle $(abspath $(OBJDIR))/db2-replay-bundle --output $(abspath $(OBJDIR))/db2-replay-output --placement kb >/dev/null
	@cp $(OBJDIR)/db2-replay-output/aimee-module-db2 $@

.PHONY: check-db2-module-runtime
check-db2-module-runtime: $(OBJDIR)/aimee-module-db2
	@log="$$(mktemp)"; trap 'rm -f "$$log"' EXIT; \
	if env -u AIMEE_DB2_URL $(abspath $<) /tmp/aimee-db2-no-bus.sock > /dev/null 2>"$$log"; then \
	  echo "DB2 module attached without AIMEE_DB2_URL" >&2; exit 1; \
	fi; \
	grep -q '^db2: AIMEE_DB2_URL is unset; refusing to serve$$' "$$log"; \
	if AIMEE_DB2_URL='postgresql://db2:sentinel-secret@127.0.0.1:1/aimee?connect_timeout=1' \
	     $(abspath $<) /tmp/aimee-db2-no-bus.sock > /dev/null 2>"$$log"; then \
	  echo "DB2 module attached after failed initialization" >&2; exit 1; \
	fi; \
	grep -q '^db2: database initialization failed; refusing to serve$$' "$$log"; \
	if grep -q 'sentinel-secret' "$$log"; then \
	  echo "DB2 module leaked its DSN" >&2; exit 1; \
	fi; \
	echo "DB2 module runtime: complete link and fail-closed initialization ok"

$(TESTPREFIX)/unit-test-db1-agent-work-client: \
                                       $(OBJDIR)/tests/test_db1_agent_work_client.o \
                                       $(OBJDIR)/db1_client/agent_work.o \
                                       $(OBJDIR)/module_json_call.o $(OBJDIR)/log.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-db1-conversation-client: \
                                       $(OBJDIR)/tests/test_db1_conversation_client.o \
                                       $(OBJDIR)/db1_client/conversation.o \
                                       $(OBJDIR)/module_json_call.o $(OBJDIR)/log.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-db1-module-stage: \
                                       $(OBJDIR)/tests/test_db1_module_stage.o \
                                       $(OBJDIR)/modules/db1/module_adapter.o \
                                       $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                                       $(OBJDIR)/modules/db1/checkpoints.o \
                                       $(OBJDIR)/modules/db1/git_ownership.o \
                                       $(OBJDIR)/modules/db1/git_ownership_stage.o \
                                       $(OBJDIR)/modules/db1/conversation_stage.o \
                                       $(OBJDIR)/modules/db1/clarify.o \
                                       $(OBJDIR)/modules/db1/payload_rewrite_state.o \
                                       $(OBJDIR)/modules/db1/wm.o $(OBJDIR)/util.o \
                                       $(OBJDIR)/modules/db1/agent_work_stage.o \
                                       $(OBJDIR)/modules/db1/delegation_stage.o \
                                       $(OBJDIR)/modules/db1/sessions_stage.o \
                                       $(OBJDIR)/modules/db1/runtime_stage.o \
                                       $(OBJDIR)/modules/db1/telemetry_stage.o $(OBJDIR)/modules/db1/approach_failures.o \
                                       $(OBJDIR)/modules/db1/guardrail_state_stage.o \
                                       $(OBJDIR)/modules/db1/session_state.o \
                                       $(OBJDIR)/modules/db1/ensemble_stage.o \
                                       $(OBJDIR)/modules/db1/ensemble.o \
                                       $(OBJDIR)/modules/db1/workflow_stage.o \
                                       $(OBJDIR)/modules/db1/execution_trace.o \
                                       $(OBJDIR)/modules/db1/wfe_binding.o \
                                       $(OBJDIR)/modules/db1/pipelines.o \
                                       $(OBJDIR)/modules/db1/roadmap_runtime.o \
                                       $(OBJDIR)/modules/db1/execution_plans.o \
                                       $(OBJDIR)/modules/db1/roundtable_pipeline.o \
                                       $(OBJDIR)/modules/db1/roundtable_stage.o \
                                       $(OBJDIR)/modules/db1/identity_stage.o \
                                       $(OBJDIR)/modules/db1/checkpoints_stage.o \
                                       $(OBJDIR)/modules/db1/jti_replay_stage.o \
                                       $(OBJDIR)/modules/db1/lifecycle_stage.o \
                                       $(OBJDIR)/modules/db1/wfe_engine_store.o \
                                       $(OBJDIR)/modules/db1/wfe_budget_store.o \
                                       $(OBJDIR)/modules/db1/wfe_transition_store.o \
                                       $(OBJDIR)/modules/db1/wfe_tree_store.o \
                                       $(OBJDIR)/modules/db1/mgmt_jwks_stage.o \
                                       $(OBJDIR)/modules/db1/mgmt_jwks_cache.o \
                                       $(OBJDIR)/modules/db1/mgmt_nonce_stage.o \
                                       $(OBJDIR)/modules/db1/pki_stage.o \
                                       $(OBJDIR)/modules/db1/pki_store.o \
                                       $(OBJDIR)/modules/db1/mgmt_nonce.o \
                                       $(OBJDIR)/modules/db1/server_identity_jti.o \
                                       $(OBJDIR)/modules/db1/wfe_store.o $(OBJDIR)/wfe_store_lists.o \
                     $(OBJDIR)/modules/db1/server_management_jti.o \
                                       $(OBJDIR)/modules/db1/economizer_state.o \
                                       $(OBJDIR)/modules/db1/remote_client_grant.o \
                                       $(OBJDIR)/modules/db1/wfe_store.o \
                                       $(OBJDIR)/wfe_store_lists.o \
                                       $(OBJDIR)/dstr.o \
                                       $(OBJDIR)/modules/db1/token_audit.o \
                                       $(OBJDIR)/modules/db1/cost_fold.o \
                                       $(OBJDIR)/modules/db1/interaction_events.o \
                                       $(OBJDIR)/modules/db1/guardrail_events.o \
                                       $(OBJDIR)/modules/db1/eval.o \
                                       $(OBJDIR)/modules/db1/diagnose.o \
                                       $(OBJDIR)/log.o $(OBJDIR)/posix/platform_path.o \
                                       $(OBJDIR)/modules/db1/interaction_events.o \
                                       $(OBJDIR)/modules/db1/runtime_state.o \
                                       $(OBJDIR)/modules/db1/project_clones.o \
                                       $(OBJDIR)/modules/db1/local_operator.o \
                                       $(OBJDIR)/modules/db1/env.o \
                                       $(OBJDIR)/modules/db1/model_catalog.o $(OBJDIR)/model_catalog_release.o \
                                       $(OBJDIR)/modules/db1/model_pricing.o \
                                       $(OBJDIR)/modules/db1/working_profile_local.o \
                                       $(OBJDIR)/modules/db1/tool_local_availability.o \
                                       $(OBJDIR)/modules/db1/caches.o \
                                       $(OBJDIR)/modules/db1/web_page_cache.o \
                                       $(OBJDIR)/modules/db1/fsnap.o \
                                       $(OBJDIR)/modules/db1/decisions.o \
                                       $(OBJDIR)/modules/db1/mcp_osv_cache.o \
                                       $(OBJDIR)/modules/db1/server_sessions.o \
                                       $(OBJDIR)/modules/db1/primary_sessions.o \
                                       $(OBJDIR)/modules/db1/session_paths.o \
                                       $(OBJDIR)/modules/db1/webchat_claude_sessions.o \
                                       $(OBJDIR)/modules/db1/webchat_live.o \
                                       $(OBJDIR)/modules/db1/cognify_jobs.o \
                                       $(OBJDIR)/modules/db1/conv_context.o \
                                       $(OBJDIR)/modules/db1/agent_log.o \
                                       $(OBJDIR)/modules/db1/windows.o \
                                       $(OBJDIR)/modules/db1/db1_trigger.o \
                                       $(OBJDIR)/modules/db1/user_memory.o \
                                       $(OBJDIR)/modules/db1/coord_jobs.o \
                                       $(OBJDIR)/modules/db1/db1_cron_jobs.o \
                                       $(OBJDIR)/modules/db1/delegations.o \
                                       $(OBJDIR)/modules/db1/delegate_reservation.o \
                                       $(OBJDIR)/modules/db1/delegation_checkpoint.o \
                                       $(OBJDIR)/modules/db1/agent_jobs.o $(OBJDIR)/agent_job_release.o \
                                       $(OBJDIR)/modules/db1/delegate_learning.o $(OBJDIR)/delegate_exit_classify.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(OBJDIR)/core/event_bus/module_runtime.o \
                                       $(OBJDIR)/core/event_bus/module_protocol.o \
                                       $(OBJDIR)/core/event_bus/bus_attach.o \
                                       $(OBJDIR)/core/event_bus/bus_client.o \
                                       $(OBJDIR)/core/event_bus/bus_endpoint.o \
                                       $(OBJDIR)/core/event_bus/bus_region.o \
                                       $(OBJDIR)/core/event_bus/bus_ring.o \
                                       $(OBJDIR)/core/event_bus/bus_wire.o $(OBJDIR)/interaction_event_names.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-db1-cost-fold: \
                                       $(OBJDIR)/tests/test_db1_cost_fold.o \
                                       $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                                       $(OBJDIR)/modules/db1/cost_fold.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-db1-roundtable-pipeline: \
                                       $(OBJDIR)/tests/test_db1_roundtable_pipeline.o \
                                       $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                                       $(OBJDIR)/modules/db1/roundtable_pipeline.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-roundtable-pipeline-eval: \
                                       $(OBJDIR)/tests/test_roundtable_pipeline_eval.o \
                                       $(OBJDIR)/modules/roundtable/roundtable_pipeline_eval.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-roundtable-pipeline-chunk: \
                                       $(OBJDIR)/tests/test_roundtable_pipeline_chunk.o \
                                       $(OBJDIR)/modules/roundtable/roundtable_pipeline_chunk.o $(OBJDIR)/module_json_call.o $(OBJDIR)/tests/support/module_bus_stub.o $(OBJDIR)/cJSON.o \
                                       $(OBJDIR)/modules/roundtable/roundtable_pipeline_eval.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-roundtable-pipeline-ctl: \
                                       $(OBJDIR)/tests/test_roundtable_pipeline_ctl.o \
                                       $(OBJDIR)/server/server_pipeline.o $(OBJDIR)/server/server_pipeline_merge.o \
                                       $(OBJDIR)/modules/git/git_pr_ci_grade.o \
                                       $(OBJDIR)/modules/roundtable/roundtable_pipeline_eval.o \
                                       $(OBJDIR)/modules/roundtable/roundtable_pipeline_chunk.o $(OBJDIR)/module_json_call.o $(OBJDIR)/tests/support/module_bus_stub.o $(OBJDIR)/cJSON.o \
                                       $(OBJDIR)/modules/db1/roundtable_pipeline.o \
                                       $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                                       $(OBJDIR)/modules/db1/local_operator.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-roundtable-pipeline-capture: \
                                       $(OBJDIR)/tests/test_roundtable_pipeline_capture.o \
                                       $(OBJDIR)/modules/roundtable/roundtable_pipeline_capture.o \
                                       $(OBJDIR)/modules/roundtable/roundtable_pipeline_eval.o \
                                       $(OBJDIR)/modules/db1/roundtable_pipeline.o \
                                       $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                                       $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-db1-session-paths: \
                                       $(OBJDIR)/tests/test_db1_session_paths.o \
                                       $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                                       $(OBJDIR)/modules/db1/session_paths.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-delegate-credentials: \
                                       $(OBJDIR)/tests/test_delegate_credentials.o \
                                       $(OBJDIR)/modules/delegates/delegate_credentials.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-delegate-economics: $(OBJDIR)/tests/test_delegate_economics.o \
                                       $(OBJDIR)/modules/delegates/delegate_economics.o \
                                       $(OBJDIR)/modules/delegates/delegate_prompt.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-delegate-patch-coordinator: $(OBJDIR)/tests/test_delegate_patch_coordinator.o \
                                       $(OBJDIR)/modules/delegates/delegate_patch_coordinator.o \
                                       $(OBJDIR)/modules/delegates/delegate_prompt.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-delegate-ensemble: $(OBJDIR)/tests/test_delegate_ensemble.o \
                                       $(OBJDIR)/modules/roundtable/delegate_ensemble.o $(OBJDIR)/modules/roundtable/delegate_ensemble_review.o \
                                       $(OBJDIR)/modules/roundtable/roundtable_verify.o \
                                       $(OBJDIR)/modules/roundtable/roundtable_chair.o \
                                       $(OBJDIR)/server/evidence_replay.o \
                                       $(OBJDIR)/server/token_tracker.o \
                                       $(OBJDIR)/server/token_tracker_registry.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-delegate-liveness: $(OBJDIR)/tests/test_delegate_liveness.o \
                                    $(OBJDIR)/server/liveness.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-agent-parallel: $(OBJDIR)/tests/test_agent_parallel.o \
                                    $(OBJDIR)/server/agent_parallel.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-server-cli-oauth: $(OBJDIR)/tests/test_server_cli_oauth.o \
                                    $(OBJDIR)/server/server_cli_oauth.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-agent-runtime-messages: $(OBJDIR)/tests/test_agent_runtime_messages.o \
                                    $(OBJDIR)/posix/agent_runtime_messages.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-minimax-tool-call-args: $(OBJDIR)/tests/test_minimax_tool_call_args.o \
                                    $(OBJDIR)/server/agent_bridge.o $(OBJDIR)/server/anthropic_shape.o $(OBJDIR)/server/tool_call_args.o \
                                    $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-workspace-add-idempotent: $(OBJDIR)/tests/test_workspace_add_idempotent.o \
                                     $(CONFIG_CLIENT_TEST_OBJS) $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o \
                                     $(OBJDIR)/dstr.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-workspace-prune-dead: $(OBJDIR)/tests/test_workspace_prune_dead.o \
                                     $(CONFIG_CLIENT_TEST_OBJS) $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o \
                                     $(OBJDIR)/dstr.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-workspace-manifest: $(OBJDIR)/tests/test_workspace_manifest.o \
                                     $(OBJDIR)/modules/workspace/workspace_manifest.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-lsp: $(OBJDIR)/tests/test_lsp.o \
                              $(OBJDIR)/modules/lsp/lsp_client.o \
                              $(OBJDIR)/modules/lsp/lsp_manager.o \
                              $(OBJDIR)/modules/db1/db.o $(OBJDIR)/modules/db1/db_schema.o $(OBJDIR)/modules/db1/maintenance.o $(DB2_TEST_BACKEND_OBJ) $(OBJDIR)/db2/db2_test_shim.o \
                              $(CONFIG_CLIENT_TEST_OBJS) $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o \
                              $(OBJDIR)/aimee_home.o \
                              $(OBJDIR)/util.o $(OBJDIR)/text.o \
                              $(OBJDIR)/platform_random.o $(OBJDIR)/log.o \
                              $(PLATFORM_BASIC_OBJS) \
                              $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-sandbox: $(OBJDIR)/tests/test_sandbox.o \
                         $(OBJDIR)/posix/sandbox.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-slop-detect: $(OBJDIR)/tests/test_slop_detect.o \
                              $(OBJDIR)/slop_detect.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-vault-principal: $(OBJDIR)/tests/test_vault_principal.o \
                              $(OBJDIR)/modules/vault/vault_principal.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-vault-crypto: $(OBJDIR)/tests/test_vault_crypto.o \
                              $(OBJDIR)/modules/vault/vault_crypto.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto

$(TESTPREFIX)/unit-test-vault-kek-check: $(OBJDIR)/tests/test_vault_kek_check.o \
                              $(OBJDIR)/modules/vault/vault_kek_check.o \
                              $(OBJDIR)/modules/vault/vault_crypto.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto

$(TESTPREFIX)/unit-test-vault-reseal-receipt: $(OBJDIR)/tests/test_vault_reseal_receipt.o \
                              $(OBJDIR)/modules/vault/vault_reseal_receipt.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto

$(TESTPREFIX)/unit-test-vault-witness-record: $(OBJDIR)/tests/test_vault_witness_record.o \
                              $(OBJDIR)/modules/vault/vault_witness_record.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto

$(TESTPREFIX)/unit-test-vault-witness-merkle: $(OBJDIR)/tests/test_vault_witness_merkle.o \
                              $(OBJDIR)/modules/vault/vault_witness_merkle.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto

$(TESTPREFIX)/unit-test-vault-witness-checkpoint: $(OBJDIR)/tests/test_vault_witness_checkpoint.o \
                              $(OBJDIR)/modules/vault/vault_witness_checkpoint.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto

$(TESTPREFIX)/unit-test-vault-witness-export: $(OBJDIR)/tests/test_vault_witness_export.o \
                              $(OBJDIR)/modules/vault/vault_witness_export.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto

$(TESTPREFIX)/unit-test-vault-witness-verify: $(OBJDIR)/tests/test_vault_witness_verify.o \
                              $(OBJDIR)/modules/vault/vault_witness_verify.o \
                              $(OBJDIR)/modules/vault/vault_witness_record.o \
                              $(OBJDIR)/modules/vault/vault_witness_checkpoint.o \
                              $(OBJDIR)/modules/vault/vault_witness_merkle.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto

$(TESTPREFIX)/unit-test-vault-witness-signer: $(OBJDIR)/tests/test_vault_witness_signer.o \
                              $(OBJDIR)/modules/vault/vault_witness_signer.o \
                              $(OBJDIR)/modules/vault/vault_witness_checkpoint.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto

$(TESTPREFIX)/unit-test-vault-witness-proof: $(OBJDIR)/tests/test_vault_witness_proof.o \
                              $(OBJDIR)/modules/vault/vault_witness_proof.o \
                              $(OBJDIR)/modules/vault/vault_witness_merkle.o \
                              $(OBJDIR)/modules/vault/vault_witness_record.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto

$(TESTPREFIX)/unit-test-witness-gate-race: $(OBJDIR)/tests/test_witness_gate_race.o \
                              $(OBJDIR)/kb/kb_witness_gate_state.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-vault-witness-offline: $(OBJDIR)/tests/test_vault_witness_offline.o \
                              $(OBJDIR)/modules/vault/vault_witness_offline.o \
                              $(OBJDIR)/modules/vault/vault_witness_verify.o \
                              $(OBJDIR)/modules/vault/vault_witness_record.o \
                              $(OBJDIR)/modules/vault/vault_witness_merkle.o \
                              $(OBJDIR)/modules/vault/vault_witness_checkpoint.o \
                              $(OBJDIR)/modules/vault/vault_witness_export.o \
                              $(OBJDIR)/modules/vault/vault_witness_proof.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto

# Deterministic mutation property test over the offline verifier: 60k fixed-seed
# bit-flips of a VALID stream must never verify clean and must never crash. Kept as
# a CI unit test (not a corpus fuzzer) so any future change letting a mutated stream
# pass is caught. Fast (~1s) and self-contained (no args, fixed LCG seed).
$(TESTPREFIX)/unit-test-witness-offline-fuzz: $(OBJDIR)/tests/test_witness_offline_fuzz.o \
                              $(OBJDIR)/modules/vault/vault_witness_offline.o \
                              $(OBJDIR)/modules/vault/vault_witness_verify.o \
                              $(OBJDIR)/modules/vault/vault_witness_record.o \
                              $(OBJDIR)/modules/vault/vault_witness_merkle.o \
                              $(OBJDIR)/modules/vault/vault_witness_checkpoint.o \
                              $(OBJDIR)/modules/vault/vault_witness_export.o \
                              $(OBJDIR)/modules/vault/vault_witness_proof.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto

$(TESTPREFIX)/unit-test-witness-tamper-scenarios: $(OBJDIR)/tests/test_witness_tamper_scenarios.o \
                              $(OBJDIR)/modules/vault/vault_witness_offline.o \
                              $(OBJDIR)/modules/vault/vault_witness_verify.o \
                              $(OBJDIR)/modules/vault/vault_witness_record.o \
                              $(OBJDIR)/modules/vault/vault_witness_merkle.o \
                              $(OBJDIR)/modules/vault/vault_witness_checkpoint.o \
                              $(OBJDIR)/modules/vault/vault_witness_export.o \
                              $(OBJDIR)/modules/vault/vault_witness_proof.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto

$(TESTPREFIX)/unit-test-vault-mutation-budget: \
                              $(OBJDIR)/tests/test_vault_mutation_budget.o \
                              $(OBJDIR)/modules/vault/vault_mutation_budget.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-vault-reseal-orchestrator: \
                              $(OBJDIR)/tests/test_vault_reseal_orchestrator.o \
                              $(OBJDIR)/modules/vault/vault_reseal_orchestrator.o \
                              $(OBJDIR)/modules/vault/vault_mutation_budget.o \
                              $(OBJDIR)/modules/vault/vault_reseal_receipt.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto

$(TESTPREFIX)/unit-test-org-vault-rewrap: $(OBJDIR)/tests/test_org_vault_rewrap.o \
                              $(OBJDIR)/db2/org_vault_rewrap.o \
                              $(OBJDIR)/modules/vault/vault_reseal_receipt.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto -lpthread

$(TESTPREFIX)/unit-test-evidence-replay: $(OBJDIR)/tests/test_evidence_replay.o \
                              $(OBJDIR)/server/evidence_replay.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto

# Live forge: the pure check-runs/combined-status -> CI verdict aggregation.
$(OBJDIR)/tests/test_git_pr_ci_grade.o: C_FLAGS += -Icore/event_bus/include
$(TESTPREFIX)/unit-test-git-pr-ci-grade: $(OBJDIR)/tests/test_git_pr_ci_grade.o \
                              $(OBJDIR)/modules/git/git_pr_ci_grade.o \
                              $(OBJDIR)/module_json_call.o $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-roundtable-verify: $(OBJDIR)/tests/test_roundtable_verify.o \
                              $(OBJDIR)/modules/roundtable/roundtable_verify.o $(OBJDIR)/server/evidence_replay.o \
                              $(OBJDIR)/dstr.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto

$(TESTPREFIX)/unit-test-roundtable-chair: $(OBJDIR)/tests/test_roundtable_chair.o \
                              $(OBJDIR)/modules/roundtable/roundtable_chair.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-sweep-logic: $(OBJDIR)/tests/test_sweep_logic.o \
                              $(OBJDIR)/server/sweep_exclude.o $(OBJDIR)/server/sweep_score.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-sweep-scope: $(OBJDIR)/tests/test_sweep_scope.o \
                              $(OBJDIR)/server/sweep_scope.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-sweep-parse: $(OBJDIR)/tests/test_sweep_parse.o \
                              $(OBJDIR)/server/sweep_parse.o $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-vault-kek-cache: $(OBJDIR)/tests/test_vault_kek_cache.o \
                              $(OBJDIR)/modules/vault/vault_kek_cache.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto

$(TESTPREFIX)/unit-test-vault-store: $(OBJDIR)/tests/test_vault_store.o \
                              $(OBJDIR)/modules/vault/vault_store.o $(OBJDIR)/modules/vault/vault_kek_check.o $(OBJDIR)/modules/vault/vault_crypto.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-vault-seam: $(OBJDIR)/tests/test_vault_seam.o \
                              $(OBJDIR)/modules/vault/vault_store.o $(OBJDIR)/modules/vault/vault_kek_check.o $(OBJDIR)/modules/vault/vault_crypto.o \
                              $(OBJDIR)/modules/vault/vault_kek_cache.o \
                              $(OBJDIR)/modules/vault/vault_server_key.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-vault-local-status: $(OBJDIR)/tests/test_vault_local_status.o \
                              $(OBJDIR)/modules/vault/vault_store.o $(OBJDIR)/modules/vault/vault_kek_check.o $(OBJDIR)/modules/vault/vault_crypto.o \
                              $(OBJDIR)/modules/vault/vault_kek_cache.o \
                              $(OBJDIR)/modules/vault/vault_server_key.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-vault-operator-status-runtime: \
                              $(OBJDIR)/tests/test_vault_operator_status_runtime.o \
                              $(OBJDIR)/db2/vault_operator_status_runtime.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lpq -lpthread

$(TESTPREFIX)/unit-test-kb-vault-operator-status: \
                              $(OBJDIR)/tests/test_kb_vault_operator_status.o \
                              $(OBJDIR)/kb/kb_vault_operator_status.o \
                              $(OBJDIR)/kb/kb_vault_protected_secret.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lpthread

$(TESTPREFIX)/unit-test-kb-vault-operator-mutation: \
                              $(OBJDIR)/tests/test_kb_vault_operator_mutation.o \
                              $(OBJDIR)/kb/kb_vault_operator_status.o \
                              $(OBJDIR)/kb/kb_vault_protected_secret.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lpthread

$(TESTPREFIX)/unit-test-kb-vault-operator-choreography: \
                              $(OBJDIR)/tests/test_kb_vault_operator_choreography.o \
                              $(OBJDIR)/kb/kb_vault_operator_mutation.o \
                              $(OBJDIR)/modules/vault/vault_mutation_budget.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto

$(TESTPREFIX)/unit-test-kb-vault-operator-runtime: \
                              $(OBJDIR)/tests/test_kb_vault_operator_runtime.o \
                              $(OBJDIR)/kb/kb_vault_operator_runtime.o \
                              $(OBJDIR)/kb/kb_vault_operator_mutation.o \
                              $(OBJDIR)/modules/vault/vault_mutation_budget.o \
                              $(OBJDIR)/kb/kb_vault_protected_secret.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto -lpthread

$(TESTPREFIX)/unit-test-kb-vault-protected-secret: \
                              $(OBJDIR)/tests/test_kb_vault_protected_secret.o \
                              $(OBJDIR)/kb/kb_vault_protected_secret.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-kb-vault-activation-latch: \
                              $(OBJDIR)/tests/test_kb_vault_activation_latch.o \
                              $(OBJDIR)/kb/kb_vault_activation_latch.o \
                              $(OBJDIR)/kb/kb_vault_operator_status.o \
                              $(OBJDIR)/kb/kb_vault_protected_secret.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lpthread

$(TESTPREFIX)/unit-test-kb-vault-tpm-runtime-lock: \
                              $(OBJDIR)/tests/test_kb_vault_tpm_runtime_lock.o \
                              $(OBJDIR)/kb/kb_vault_tpm_runtime_lock.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lpthread

$(TESTPREFIX)/unit-test-vault-maintenance-guard: \
                              $(OBJDIR)/tests/test_vault_maintenance_guard.o \
                              $(OBJDIR)/modules/vault/vault_store.o $(OBJDIR)/modules/vault/vault_kek_check.o \
                              $(OBJDIR)/modules/vault/vault_crypto.o \
                              $(OBJDIR)/modules/vault/vault_kek_cache.o \
                              $(OBJDIR)/modules/vault/vault_server_key.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-vault-d3b-custody: \
                              $(OBJDIR)/tests/test_vault_d3b_custody.o \
                              $(OBJDIR)/modules/vault/vault_store.o $(OBJDIR)/modules/vault/vault_kek_check.o \
                              $(OBJDIR)/modules/vault/vault_crypto.o \
                              $(OBJDIR)/modules/vault/vault_kek_cache.o \
                              $(OBJDIR)/modules/vault/vault_server_key.o \
                              $(OBJDIR)/modules/vault/vault_custody_tpm2.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-vault-rotation: $(OBJDIR)/tests/test_kb_vault_rotation.o \
                              $(OBJDIR)/kb/kb_vault_rotation.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto

$(TESTPREFIX)/unit-test-kb-vault-key-use: $(OBJDIR)/tests/test_kb_vault_key_use.o \
                              $(OBJDIR)/kb/kb_vault_key_use.o \
                              $(OBJDIR)/kb/kb_vault_protected_use.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto

$(TESTPREFIX)/unit-test-kb-vault-key-use-live: $(OBJDIR)/tests/test_kb_vault_key_use_live.o \
                              $(filter-out $(OBJDIR)/kb/kb_main.o,$(KB_OBJS)) $(OBJDIR)/dashboard_kb.o \
                              $(OBJDIR)/server/oauth_pkce.o $(OBJDIR)/server/embedder_probe.o \
                              $(KB_DATA_OBJS) $(KB_CORE_OBJS) $(KB_DB2_PG_OBJS) $(KB_DB2_OBJS) \
                              $(KB_VAULT_OBJS) $(KB_PLATFORM_OBJS) $(TS_VENDOR_OBJS)
	$(TESTLINK) -o $@ $^ $(L_KB)

$(TESTPREFIX)/unit-test-kb-p2b-egress-live: $(OBJDIR)/tests/test_kb_p2b_egress_live.o \
                              $(filter-out $(OBJDIR)/kb/kb_main.o,$(KB_OBJS)) $(OBJDIR)/dashboard_kb.o \
                              $(OBJDIR)/server/oauth_pkce.o $(OBJDIR)/server/embedder_probe.o \
                              $(OBJDIR)/modules/translation/aimee_backend_bedrock.o \
                              $(OBJDIR)/modules/translation/aimee_frontend_openai.o \
                              $(OBJDIR)/modules/translation/aimee_ir_stream.o $(OBJDIR)/modules/ir/aimee_ir.o \
                              $(KB_DATA_OBJS) $(KB_CORE_OBJS) $(KB_DB2_PG_OBJS) $(KB_DB2_OBJS) \
                              $(KB_VAULT_OBJS) $(KB_PLATFORM_OBJS) $(TS_VENDOR_OBJS)
	$(TESTLINK) -o $@ $^ $(L_KB)

$(TESTPREFIX)/unit-test-kb-vault-rotation-ops: $(OBJDIR)/tests/test_kb_vault_rotation_ops.o \
                              $(OBJDIR)/kb/kb_vault_rotation_ops.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto

$(TESTPREFIX)/unit-test-kb-vault-rotation-ops-live: \
                              $(OBJDIR)/tests/test_kb_vault_rotation_ops_live.o \
                              $(filter-out $(OBJDIR)/kb/kb_main.o,$(KB_OBJS)) $(OBJDIR)/dashboard_kb.o \
                              $(OBJDIR)/server/oauth_pkce.o $(OBJDIR)/server/embedder_probe.o \
                              $(KB_DATA_OBJS) $(KB_CORE_OBJS) $(KB_DB2_PG_OBJS) $(KB_DB2_OBJS) \
                              $(KB_VAULT_OBJS) $(KB_PLATFORM_OBJS) $(TS_VENDOR_OBJS)
	$(TESTLINK) -o $@ $^ $(L_KB)

# P10 kb vault Postgres backend. REAL-PG test: links the KB object closure (real libpq
# via db_postgres.o, the vault core + db2/vault_pg.o) so that on a box with
# AIMEE_TEST_PG_URL it actually connects to Postgres; without it the test SKIPs (exit 0).
# Modeled on the negation-eval binary (full KB minus kb_main + a test main), which is
# the established pattern for a real-libpq test target.
$(TESTPREFIX)/unit-test-vault-pg: $(OBJDIR)/tests/test_vault_pg.o \
                              $(filter-out $(OBJDIR)/kb/kb_main.o,$(KB_OBJS)) $(OBJDIR)/dashboard_kb.o \
                              $(OBJDIR)/server/oauth_pkce.o $(OBJDIR)/server/embedder_probe.o \
                              $(KB_DATA_OBJS) $(KB_CORE_OBJS) $(KB_DB2_PG_OBJS) $(KB_DB2_OBJS) \
                              $(KB_VAULT_OBJS) $(KB_PLATFORM_OBJS) $(TS_VENDOR_OBJS)
	$(TESTLINK) -o $@ $^ $(L_KB)

$(TESTPREFIX)/unit-test-kb-vault-rotation-live: $(OBJDIR)/tests/test_kb_vault_rotation_live.o \
                              $(filter-out $(OBJDIR)/kb/kb_main.o,$(KB_OBJS)) $(OBJDIR)/dashboard_kb.o \
                              $(OBJDIR)/server/oauth_pkce.o $(OBJDIR)/server/embedder_probe.o \
                              $(KB_DATA_OBJS) $(KB_CORE_OBJS) $(KB_DB2_PG_OBJS) $(KB_DB2_OBJS) \
                              $(KB_VAULT_OBJS) $(KB_PLATFORM_OBJS) $(TS_VENDOR_OBJS)
	$(TESTLINK) -o $@ $^ $(L_KB)

# P7-reseal-c owner-only mock driver. Explicit standalone target: it is intentionally
# absent from TEST_TARGETS because it requires an otherwise empty real-PG scratch vault.
$(TESTPREFIX)/p7-vault-rewrap-live: $(OBJDIR)/tests/test_kb_vault_rewrap_live.o \
                              $(filter-out $(OBJDIR)/kb/kb_main.o,$(KB_OBJS)) $(OBJDIR)/dashboard_kb.o \
                              $(OBJDIR)/server/oauth_pkce.o $(OBJDIR)/server/embedder_probe.o \
                              $(KB_DATA_OBJS) $(KB_CORE_OBJS) $(KB_DB2_PG_OBJS) $(KB_DB2_OBJS) \
                              $(KB_VAULT_OBJS) $(KB_PLATFORM_OBJS) $(TS_VENDOR_OBJS)
	$(TESTLINK) -o $@ $^ $(L_KB)

# P2a atomic-audit proof. REAL-PG test (SKIPs without AIMEE_TEST_PG_URL): builds a mixed
# [C, SQL, C] kb_audit_event chain and asserts the C verifier accepts it + the SQL row
# hashes byte-identically in C. Same KB object closure as unit-test-vault-pg (real libpq
# via db_postgres.o + the kb db2 layer that carries kb_audit_worm.o/schema.sql).
# Content-scope referent + predicate (slice 1). REAL-PG test: SKIPs cleanly
# without AIMEE_TEST_PG_URL, because RLS and current_setting mean nothing on the
# SQLite shim.
$(TESTPREFIX)/unit-test-content-scope-pg: $(OBJDIR)/tests/test_content_scope_pg.o \
                              $(filter-out $(OBJDIR)/kb/kb_main.o,$(KB_OBJS)) $(OBJDIR)/dashboard_kb.o \
                              $(OBJDIR)/server/oauth_pkce.o $(OBJDIR)/server/embedder_probe.o \
                              $(KB_DATA_OBJS) $(KB_CORE_OBJS) $(KB_DB2_PG_OBJS) $(KB_DB2_OBJS) \
                              $(KB_VAULT_OBJS) $(KB_PLATFORM_OBJS) $(TS_VENDOR_OBJS)
	$(TESTLINK) -o $@ $^ $(L_KB)

$(TESTPREFIX)/unit-test-kb-audit-worm-pg: $(OBJDIR)/tests/test_kb_audit_worm_pg.o \
                              $(filter-out $(OBJDIR)/kb/kb_main.o,$(KB_OBJS)) $(OBJDIR)/dashboard_kb.o \
                              $(OBJDIR)/server/oauth_pkce.o $(OBJDIR)/server/embedder_probe.o \
                              $(KB_DATA_OBJS) $(KB_CORE_OBJS) $(KB_DB2_PG_OBJS) $(KB_DB2_OBJS) \
                              $(KB_VAULT_OBJS) $(KB_PLATFORM_OBJS) $(TS_VENDOR_OBJS)
	$(TESTLINK) -o $@ $^ $(L_KB)

$(TESTPREFIX)/unit-test-witness-checkpoint-produce-pg: $(OBJDIR)/tests/test_witness_checkpoint_produce_pg.o \
                              $(filter-out $(OBJDIR)/kb/kb_main.o,$(KB_OBJS)) $(OBJDIR)/dashboard_kb.o \
                              $(OBJDIR)/server/oauth_pkce.o $(OBJDIR)/server/embedder_probe.o \
                              $(KB_DATA_OBJS) $(KB_CORE_OBJS) $(KB_DB2_PG_OBJS) $(KB_DB2_OBJS) \
                              $(KB_VAULT_OBJS) $(KB_PLATFORM_OBJS) $(TS_VENDOR_OBJS)
	$(TESTLINK) -o $@ $^ $(L_KB)

$(TESTPREFIX)/unit-test-witness-emit-pg: $(OBJDIR)/tests/test_witness_emit_pg.o \
                              $(filter-out $(OBJDIR)/kb/kb_main.o,$(KB_OBJS)) $(OBJDIR)/dashboard_kb.o \
                              $(OBJDIR)/server/oauth_pkce.o $(OBJDIR)/server/embedder_probe.o \
                              $(KB_DATA_OBJS) $(KB_CORE_OBJS) $(KB_DB2_PG_OBJS) $(KB_DB2_OBJS) \
                              $(KB_VAULT_OBJS) $(KB_PLATFORM_OBJS) $(TS_VENDOR_OBJS)
	$(TESTLINK) -o $@ $^ $(L_KB)

$(TESTPREFIX)/aimee-witness-boot-tpm-harness: $(OBJDIR)/tools/witness_boot_tpm_harness.o \
                              $(filter-out $(OBJDIR)/kb/kb_main.o,$(KB_OBJS)) $(OBJDIR)/dashboard_kb.o \
                              $(OBJDIR)/server/oauth_pkce.o $(OBJDIR)/server/embedder_probe.o \
                              $(KB_DATA_OBJS) $(KB_CORE_OBJS) $(KB_DB2_PG_OBJS) $(KB_DB2_OBJS) \
                              $(KB_VAULT_OBJS) $(KB_PLATFORM_OBJS) $(TS_VENDOR_OBJS)
	$(TESTLINK) -o $@ $^ $(L_KB) $(KB_TPM2_LDLIBS)

$(TESTPREFIX)/aimee-witness-cadence-harness: $(OBJDIR)/tools/witness_cadence_harness.o \
                              $(filter-out $(OBJDIR)/kb/kb_main.o,$(KB_OBJS)) $(OBJDIR)/dashboard_kb.o \
                              $(OBJDIR)/server/oauth_pkce.o $(OBJDIR)/server/embedder_probe.o \
                              $(KB_DATA_OBJS) $(KB_CORE_OBJS) $(KB_DB2_PG_OBJS) $(KB_DB2_OBJS) \
                              $(KB_VAULT_OBJS) $(KB_PLATFORM_OBJS) $(TS_VENDOR_OBJS)
	$(TESTLINK) -o $@ $^ $(L_KB)

$(TESTPREFIX)/unit-test-witness-canary-pg: $(OBJDIR)/tests/test_witness_canary_pg.o \
                              $(filter-out $(OBJDIR)/kb/kb_main.o,$(KB_OBJS)) $(OBJDIR)/dashboard_kb.o \
                              $(OBJDIR)/server/oauth_pkce.o $(OBJDIR)/server/embedder_probe.o \
                              $(KB_DATA_OBJS) $(KB_CORE_OBJS) $(KB_DB2_PG_OBJS) $(KB_DB2_OBJS) \
                              $(KB_VAULT_OBJS) $(KB_PLATFORM_OBJS) $(TS_VENDOR_OBJS)
	$(TESTLINK) -o $@ $^ $(L_KB)

$(TESTPREFIX)/unit-test-witness-recovery-pg: $(OBJDIR)/tests/test_witness_recovery_pg.o \
                              $(filter-out $(OBJDIR)/kb/kb_main.o,$(KB_OBJS)) $(OBJDIR)/dashboard_kb.o \
                              $(OBJDIR)/server/oauth_pkce.o $(OBJDIR)/server/embedder_probe.o \
                              $(KB_DATA_OBJS) $(KB_CORE_OBJS) $(KB_DB2_PG_OBJS) $(KB_DB2_OBJS) \
                              $(KB_VAULT_OBJS) $(KB_PLATFORM_OBJS) $(TS_VENDOR_OBJS)
	$(TESTLINK) -o $@ $^ $(L_KB)

$(TESTPREFIX)/unit-test-witness-tamper-pg: $(OBJDIR)/tests/test_witness_tamper_pg.o \
                              $(filter-out $(OBJDIR)/kb/kb_main.o,$(KB_OBJS)) $(OBJDIR)/dashboard_kb.o \
                              $(OBJDIR)/server/oauth_pkce.o $(OBJDIR)/server/embedder_probe.o \
                              $(KB_DATA_OBJS) $(KB_CORE_OBJS) $(KB_DB2_PG_OBJS) $(KB_DB2_OBJS) \
                              $(KB_VAULT_OBJS) $(KB_PLATFORM_OBJS) $(TS_VENDOR_OBJS)
	$(TESTLINK) -o $@ $^ $(L_KB)

$(TESTPREFIX)/unit-test-git-ops: $(OBJDIR)/tests/test_git_ops.o \
                              $(OBJDIR)/tests/module_handlers/workspace.o \
                              $(OBJDIR)/modules/git/git_ops.o $(OBJDIR)/tests/support/git_pr_api_stub.o $(OBJDIR)/modules/git/git_cred_inject.o $(OBJDIR)/modules/git/git_ssh_agent.o $(OBJDIR)/modules/webuser/webuser_runtime.o \
                              $(OBJDIR)/modules/git/git_forge_vault.o $(OBJDIR)/modules/git/git_host_cred.o $(OBJDIR)/modules/git/git_host_resolve.o $(OBJDIR)/modules/workspace/workspace_scope.o \
                              $(OBJDIR)/modules/git/forge_credentials.o $(CONFIG_CLIENT_TEST_OBJS) $(OBJDIR)/aimee_home.o $(OBJDIR)/util_url.o \
                              $(OBJDIR)/posix/util.o $(OBJDIR)/util.o \
                              $(OBJDIR)/modules/vault/vault_service.o $(OBJDIR)/modules/vault/vault_store.o $(OBJDIR)/modules/vault/vault_kek_check.o \
                              $(OBJDIR)/modules/vault/vault_crypto.o $(OBJDIR)/modules/vault/vault_kek_cache.o \
                              $(OBJDIR)/modules/vault/vault_server_key.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-git-project: $(OBJDIR)/tests/test_git_project.o \
                              $(OBJDIR)/tests/support/gp_local_index_stub.o \
                              $(OBJDIR)/tests/module_handlers/workspace.o \
                              $(OBJDIR)/modules/git/git_project.o $(OBJDIR)/server/ws_registry.o $(OBJDIR)/modules/git/git_cred_inject.o $(OBJDIR)/modules/git/git_ssh_agent.o $(OBJDIR)/modules/webuser/webuser_runtime.o \
                              $(OBJDIR)/modules/git/git_forge_vault.o $(OBJDIR)/modules/git/git_host_cred.o $(OBJDIR)/modules/git/git_host_resolve.o $(OBJDIR)/modules/workspace/workspace_scope.o \
                              $(OBJDIR)/modules/git/forge_credentials.o $(OBJDIR)/util_url.o $(CONFIG_CLIENT_TEST_OBJS) $(OBJDIR)/aimee_home.o \
                              $(OBJDIR)/posix/util.o $(OBJDIR)/util.o \
                              $(OBJDIR)/modules/vault/vault_service.o $(OBJDIR)/modules/vault/vault_store.o $(OBJDIR)/modules/vault/vault_kek_check.o \
                              $(OBJDIR)/modules/vault/vault_crypto.o $(OBJDIR)/modules/vault/vault_kek_cache.o \
                              $(OBJDIR)/modules/vault/vault_server_key.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-webchat-git-leak: $(OBJDIR)/tests/test_webchat_git_leak.o \
                              $(OBJDIR)/modules/git/git_cred_inject.o $(OBJDIR)/modules/git/git_ssh_agent.o $(OBJDIR)/modules/webuser/webuser_runtime.o $(OBJDIR)/tests/support/webuser_name_validator.o $(OBJDIR)/tests/module_handlers/workspace.o $(OBJDIR)/modules/git/git_forge_vault.o \
                              $(OBJDIR)/modules/git/git_host_cred.o $(OBJDIR)/modules/git/git_host_resolve.o $(OBJDIR)/util_url.o $(OBJDIR)/posix/util.o $(OBJDIR)/util.o \
                              $(OBJDIR)/modules/git/forge_credentials.o $(CONFIG_CLIENT_TEST_OBJS) $(OBJDIR)/aimee_home.o \
                              $(OBJDIR)/modules/vault/vault_service.o $(OBJDIR)/modules/vault/vault_store.o $(OBJDIR)/modules/vault/vault_kek_check.o \
                              $(OBJDIR)/modules/vault/vault_crypto.o $(OBJDIR)/modules/vault/vault_kek_cache.o \
                              $(OBJDIR)/modules/vault/vault_server_key.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-git-ssh-agent: $(OBJDIR)/tests/test_git_ssh_agent.o \
                              $(OBJDIR)/modules/git/git_ssh_agent.o $(OBJDIR)/modules/git/git_forge_vault.o \
                              $(OBJDIR)/modules/webuser/webuser_runtime.o $(OBJDIR)/tests/support/webuser_name_validator.o $(OBJDIR)/tests/module_handlers/workspace.o \
                              $(CONFIG_CLIENT_TEST_OBJS) $(OBJDIR)/aimee_home.o \
                              $(OBJDIR)/modules/vault/vault_service.o $(OBJDIR)/modules/vault/vault_store.o $(OBJDIR)/modules/vault/vault_kek_check.o \
                              $(OBJDIR)/modules/vault/vault_crypto.o $(OBJDIR)/modules/vault/vault_kek_cache.o \
                              $(OBJDIR)/modules/vault/vault_server_key.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-git-cred-inject: $(OBJDIR)/tests/test_git_cred_inject.o \
                              $(OBJDIR)/modules/git/git_cred_inject.o $(OBJDIR)/modules/git/git_ssh_agent.o $(OBJDIR)/modules/webuser/webuser_runtime.o $(OBJDIR)/tests/support/webuser_name_validator.o $(OBJDIR)/tests/module_handlers/workspace.o $(OBJDIR)/modules/git/git_forge_vault.o \
                              $(OBJDIR)/modules/git/git_host_cred.o $(OBJDIR)/modules/git/git_host_resolve.o $(OBJDIR)/util_url.o $(OBJDIR)/posix/util.o $(OBJDIR)/util.o \
                              $(OBJDIR)/modules/git/forge_credentials.o $(CONFIG_CLIENT_TEST_OBJS) $(OBJDIR)/aimee_home.o \
                              $(OBJDIR)/modules/vault/vault_service.o $(OBJDIR)/modules/vault/vault_store.o $(OBJDIR)/modules/vault/vault_kek_check.o \
                              $(OBJDIR)/modules/vault/vault_crypto.o $(OBJDIR)/modules/vault/vault_kek_cache.o \
                              $(OBJDIR)/modules/vault/vault_server_key.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-webuser-editor: $(OBJDIR)/tests/test_webuser_editor.o \
                              $(OBJDIR)/modules/webuser/webuser_editor.o $(OBJDIR)/modules/git/git_cred_inject.o \
                              $(OBJDIR)/modules/git/git_ssh_agent.o $(OBJDIR)/modules/webuser/webuser_runtime.o \
                              $(OBJDIR)/modules/workspace/workspace_scope.o $(OBJDIR)/modules/git/git_forge_vault.o \
                              $(OBJDIR)/modules/git/git_host_cred.o $(OBJDIR)/modules/git/git_host_resolve.o $(OBJDIR)/util_url.o $(OBJDIR)/posix/util.o $(OBJDIR)/util.o \
                              $(OBJDIR)/modules/git/forge_credentials.o $(CONFIG_CLIENT_TEST_OBJS) $(OBJDIR)/aimee_home.o \
                              $(OBJDIR)/modules/vault/vault_service.o $(OBJDIR)/modules/vault/vault_store.o $(OBJDIR)/modules/vault/vault_kek_check.o \
                              $(OBJDIR)/modules/vault/vault_crypto.o $(OBJDIR)/modules/vault/vault_kek_cache.o \
                              $(OBJDIR)/modules/vault/vault_server_key.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-git-forge-vault: $(OBJDIR)/tests/test_git_forge_vault.o \
                              $(OBJDIR)/modules/git/git_forge_vault.o \
                              $(OBJDIR)/modules/vault/vault_service.o $(OBJDIR)/modules/vault/vault_store.o $(OBJDIR)/modules/vault/vault_kek_check.o \
                              $(OBJDIR)/modules/vault/vault_crypto.o $(OBJDIR)/modules/vault/vault_kek_cache.o \
                              $(OBJDIR)/modules/vault/vault_server_key.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-git-host-resolve: $(OBJDIR)/tests/test_git_host_resolve.o \
                              $(OBJDIR)/modules/git/git_host_resolve.o \
                              $(OBJDIR)/posix/util.o $(OBJDIR)/util.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-vault-service: $(OBJDIR)/tests/test_vault_service.o \
                              $(OBJDIR)/modules/vault/vault_service.o $(OBJDIR)/modules/vault/vault_store.o $(OBJDIR)/modules/vault/vault_kek_check.o \
                              $(OBJDIR)/modules/vault/vault_crypto.o $(OBJDIR)/modules/vault/vault_kek_cache.o \
                              $(OBJDIR)/modules/vault/vault_server_key.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-vault-master-rotate: $(OBJDIR)/tests/test_vault_master_rotate.o \
                              $(OBJDIR)/modules/vault/vault_service.o $(OBJDIR)/modules/vault/vault_store.o $(OBJDIR)/modules/vault/vault_kek_check.o \
                              $(OBJDIR)/modules/vault/vault_crypto.o $(OBJDIR)/modules/vault/vault_kek_cache.o \
                              $(OBJDIR)/modules/vault/vault_server_key.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# P10/P7 slice 3b: the custody seal/unseal barrier + kb §3 live-key gate. Links the
# seam (vault_server_key), every provider referenced by the kb-only policy, and the
# KEK cache (seal flushes it) — all OpenSSL-only, no PG needed (3b stores no keys).
VAULT_POLICY_PROVIDER_OBJS = $(OBJDIR)/modules/vault/vault_custody_mock.o \
                              $(OBJDIR)/modules/vault/vault_custody_tpm2.o \
                              $(OBJDIR)/modules/vault/vault_reseal_receipt.o \
                              $(OBJDIR)/modules/vault/vault_custody_kms.o \
                              $(OBJDIR)/modules/vault/vault_custody_pkcs11.o \
                              $(OBJDIR)/modules/vault/vault_hwm.o

$(TESTPREFIX)/unit-test-vault-seal: $(OBJDIR)/tests/test_vault_seal.o \
                              $(OBJDIR)/kb/kb_vault_policy.o \
                              $(VAULT_POLICY_PROVIDER_OBJS) \
                              $(OBJDIR)/modules/vault/vault_server_key.o \
                              $(OBJDIR)/modules/vault/vault_store.o $(OBJDIR)/modules/vault/vault_kek_check.o \
                              $(OBJDIR)/modules/vault/vault_crypto.o \
                              $(OBJDIR)/modules/vault/vault_kek_cache.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Default-build (no libtss2) stub contract for the tpm2 custody provider. The kb
# policy seam pulls in kb_vault_policy.o which now references the tpm2 provider,
# so this links vault_custody_tpm2.o (the stub in a default build).
$(TESTPREFIX)/unit-test-vault-tpm2-stub: $(OBJDIR)/tests/test_vault_tpm2_stub.o \
                              $(OBJDIR)/kb/kb_vault_policy.o \
                              $(VAULT_POLICY_PROVIDER_OBJS) \
                              $(OBJDIR)/modules/vault/vault_server_key.o \
                              $(OBJDIR)/modules/vault/vault_store.o $(OBJDIR)/modules/vault/vault_kek_check.o \
                              $(OBJDIR)/modules/vault/vault_crypto.o \
                              $(OBJDIR)/modules/vault/vault_kek_cache.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# P7-tpm2a swtpm integration harness. Built ON DEMAND ONLY (NOT in TEST_TARGETS,
# so the default no-libtss2 build never touches it) by the CT gate:
#   make WITH_TPM2=1 $(OBJDIR)/tests/p7-tpm2-harness
# It links the KB-PREFIXED vault_custody_tpm2.o — the one the kb pattern rule
# compiles WITH -DWITH_TPM2 (the real ESAPI provider) — plus the tss2 libraries
# ($(KB_TPM2_LDLIBS)). test_vault_tpm2.c itself only uses the public provider API
# (no tss2 headers), so this object compiles with the ordinary tests rule.
# scripts/p7_tpm2_swtpm_test.sh builds + drives this binary against swtpm.
$(TESTPREFIX)/p7-tpm2-harness: $(OBJDIR)/tests/test_vault_tpm2.o \
                              $(OBJDIR)/kb/kb_vault_policy.o \
                              $(OBJDIR)/kb/modules/vault/vault_custody_tpm2.o \
                              $(OBJDIR)/kb/modules/vault/vault_reseal_receipt.o \
                              $(OBJDIR)/modules/vault/vault_custody_mock.o \
                              $(OBJDIR)/modules/vault/vault_custody_kms.o \
                              $(OBJDIR)/modules/vault/vault_custody_pkcs11.o \
                              $(OBJDIR)/modules/vault/vault_hwm.o \
                              $(OBJDIR)/modules/vault/vault_server_key.o \
                              $(OBJDIR)/modules/vault/vault_store.o $(OBJDIR)/modules/vault/vault_kek_check.o \
                              $(OBJDIR)/modules/vault/vault_crypto.o \
                              $(OBJDIR)/modules/vault/vault_kek_cache.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) $(KB_TPM2_LDLIBS)

# PKCS#11 (SoftHSM2) vault-custody harness (#1905). Built + driven on demand by
# scripts/p7_pkcs11_softhsm_test.sh (the vault-pkcs11-token CI gate) and by the
# unit-tests job. Rule restored after the core-modularization restructuring
# dropped it. The generic $(OBJDIR)/modules/vault/vault_custody_pkcs11.o is
# compiled WITHOUT -DWITH_PKCS11, so it is the stub provider whose unseal fails;
# the harness must link the real provider, so it uses a dedicated object built
# with -DWITH_PKCS11 + the p11-kit headers and links -ldl for dlopen().
P11KIT_HARNESS_CFLAGS := -DWITH_PKCS11 $(shell pkg-config --cflags p11-kit-1 2>/dev/null || echo -I/usr/include/p11-kit-1)
$(OBJDIR)/tests/vault_custody_pkcs11_hsm.o: modules/vault/vault_custody_pkcs11.c
	@mkdir -p $(dir $@)
	$(CC) -c $(C_FLAGS) $(P11KIT_HARNESS_CFLAGS) -o $@ $<
# Appended to TEST_TARGETS below the blanket order-only rule (or not at all), so
# it never inherited the core-archive dependency it links through TEST_L_FLAGS.
$(TESTPREFIX)/p7-pkcs11-harness: | $(CORE_CONNECTION_LIB)
$(TESTPREFIX)/p7-pkcs11-harness: $(OBJDIR)/tests/test_vault_custody_pkcs11.o \
                              $(OBJDIR)/tests/vault_custody_pkcs11_hsm.o \
                              $(OBJDIR)/modules/vault/vault_crypto.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -ldl

# P7-reseal-d2b real PostgreSQL + swtpm integration harness.  On demand only:
# the default unit suite must remain independent of libtss2 and a scratch PG DB.
# The full KB closure intentionally supplies the KB-prefixed WITH_TPM2 provider,
# D2a Postgres adapter, and D2b default orchestrator adapter.
$(TESTPREFIX)/p7-reseal-d2b-live: \
                              $(OBJDIR)/tests/test_vault_reseal_orchestrator_live.o \
                              $(filter-out $(OBJDIR)/kb/kb_main.o,$(KB_OBJS)) $(OBJDIR)/dashboard_kb.o \
                              $(OBJDIR)/server/oauth_pkce.o $(OBJDIR)/server/embedder_probe.o \
                              $(KB_DATA_OBJS) $(KB_CORE_OBJS) $(KB_DB2_PG_OBJS) $(KB_DB2_OBJS) \
                              $(KB_VAULT_OBJS) $(KB_PLATFORM_OBJS) $(TS_VENDOR_OBJS)
	$(TESTLINK) -o $@ $^ $(L_KB) $(KB_TPM2_LDLIBS)

$(TESTPREFIX)/unit-test-agent-key-import: $(OBJDIR)/tests/test_agent_key_import.o \
                              $(OBJDIR)/cli_agent_keys.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-vault-bootstrap: $(OBJDIR)/tests/test_vault_bootstrap.o \
                              $(OBJDIR)/server/server_vault_bootstrap.o \
                              $(OBJDIR)/modules/vault/vault_env_bootstrap.o \
                              $(OBJDIR)/modules/vault/vault_service.o $(OBJDIR)/modules/vault/vault_store.o $(OBJDIR)/modules/vault/vault_kek_check.o \
                              $(OBJDIR)/modules/vault/vault_crypto.o $(OBJDIR)/modules/vault/vault_kek_cache.o \
                              $(OBJDIR)/modules/vault/vault_server_key.o \
                              $(OBJDIR)/modules/vault/vault_capability.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-vault-bootstrap-privilege: \
                              $(OBJDIR)/tests/test_vault_bootstrap_privilege.o \
                              $(OBJDIR)/server/vault_bootstrap_privilege.o
	$(TESTLINK_MIN) -o $@ $^ $(EXTRA_L_FLAGS)

$(TESTPREFIX)/unit-test-pki: $(OBJDIR)/tests/test_pki.o $(OBJDIR)/server/pki.o \
$(OBJDIR)/modules/db1/pki_store.o \
                              $(OBJDIR)/modules/vault/vault_service.o $(OBJDIR)/modules/vault/vault_store.o $(OBJDIR)/modules/vault/vault_kek_check.o \
                              $(OBJDIR)/modules/vault/vault_crypto.o $(OBJDIR)/modules/vault/vault_kek_cache.o \
                              $(OBJDIR)/modules/vault/vault_server_key.o $(OBJDIR)/modules/vault/vault_principal.o \
                              $(DB1_TEST_OBJS) $(DB1_MIGRATED_OBJS) \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-remote-client-grant: $(OBJDIR)/tests/test_remote_client_grant.o \
                              $(DB1_TEST_OBJS) $(DB1_MIGRATED_OBJS) \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-vault-server-key: $(OBJDIR)/tests/test_vault_server_key.o \
                              $(OBJDIR)/modules/vault/vault_server_key.o \
                              $(OBJDIR)/modules/vault/vault_crypto.o \
                              $(OBJDIR)/modules/vault/vault_kek_cache.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-aimee-tls-clientcert: $(OBJDIR)/tests/test_aimee_tls_clientcert.o \
                              $(OBJDIR)/aimee_tls.o $(OBJDIR)/aimee_home.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-aimee-tls-pin: $(OBJDIR)/tests/test_aimee_tls_pin.o \
                              $(OBJDIR)/aimee_tls.o $(OBJDIR)/kb/pki.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-vault-capability: $(OBJDIR)/tests/test_vault_capability.o \
                              $(OBJDIR)/modules/vault/vault_capability.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-vault-audit: $(OBJDIR)/tests/test_vault_audit.o \
                              $(OBJDIR)/server/server_vault.o \
                              $(OBJDIR)/modules/vault/vault_service.o $(OBJDIR)/modules/vault/vault_store.o $(OBJDIR)/modules/vault/vault_kek_check.o \
                              $(OBJDIR)/modules/vault/vault_crypto.o $(OBJDIR)/modules/vault/vault_kek_cache.o \
                              $(OBJDIR)/modules/vault/vault_server_key.o $(OBJDIR)/modules/vault/vault_capability.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-server-vault-gate: $(OBJDIR)/tests/test_server_vault_gate.o \
                              $(OBJDIR)/server/server_vault.o \
                              $(OBJDIR)/modules/vault/vault_service.o $(OBJDIR)/modules/vault/vault_store.o $(OBJDIR)/modules/vault/vault_kek_check.o \
                              $(OBJDIR)/modules/vault/vault_crypto.o $(OBJDIR)/modules/vault/vault_kek_cache.o \
                              $(OBJDIR)/modules/vault/vault_server_key.o $(OBJDIR)/modules/vault/vault_capability.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-server-cert-grant: $(OBJDIR)/tests/test_server_cert_grant.o \
                              $(OBJDIR)/server/server_cert.o \
                              $(OBJDIR)/modules/vault/vault_capability.o \
                              $(OBJDIR)/modules/vault/vault_principal.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-server-ready: $(OBJDIR)/tests/test_server_ready.o \
                           $(OBJDIR)/server/server_ready.o $(OBJDIR)/log.o $(OBJDIR)/db1_store_ready.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-prompts: $(OBJDIR)/tests/test_prompts.o \
                           $(OBJDIR)/prompts.o $(OBJDIR)/dstr.o $(OBJDIR)/working_profile.o \
                           $(DB1_TEST_OBJS) $(DB1_MIGRATED_OBJS) \
                           $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-persona: $(OBJDIR)/tests/test_persona.o \
                           $(OBJDIR)/persona.o $(OBJDIR)/prompts.o $(OBJDIR)/dstr.o \
                           $(OBJDIR)/working_profile.o \
                           $(DB1_TEST_OBJS) $(DB1_MIGRATED_OBJS) \
                           $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-server-http: $(OBJDIR)/tests/test_server_http.o \
                     $(OBJDIR)/modules/db1/mgmt_jwks_cache.o \
                     $(OBJDIR)/server/server_mgmt_jwks_cache.o \
                     $(OBJDIR)/modules/db1/wfe_store.o $(OBJDIR)/wfe_store_lists.o \
                     $(OBJDIR)/modules/db1/server_management_jti.o \
                     $(OBJDIR)/modules/db1/server_identity_jti.o \
                     $(OBJDIR)/modules/delegates/delegate_launch_args.o \
                     $(OBJDIR)/tests/delegate_permissions_stub.o \
                      $(OBJDIR)/model_registry.o $(OBJDIR)/models_dev.o $(OBJDIR)/models_dev_cache.o \
                           $(OBJDIR)/server/server_http.o $(OBJDIR)/server/server_http_surfaces.o $(OBJDIR)/command_registry.o $(OBJDIR)/server/server_bearer_auth.o $(OBJDIR)/server/server_http_keepalive.o $(OBJDIR)/server/server_http_management.o $(OBJDIR)/server/server_http_routes.o $(OBJDIR)/server/workspace_register_args.o $(OBJDIR)/server/workflow_control_bus.o \
                     $(OBJDIR)/server/server_runtime_identity.o \
                     $(OBJDIR)/server/server_http_mgmt_read_routes.o $(OBJDIR)/server/shadow_mirror.o $(OBJDIR)/server/server_http_routes_git.o $(OBJDIR)/server/server_dev_submit.o $(OBJDIR)/server/server_ci_route.o $(OBJDIR)/server/server_http_config_routes.o $(OBJDIR)/server/cli_command_defs.o $(OBJDIR)/server/cli_dispatch_defs.o $(OBJDIR)/server/cli_marshal_defs.o $(OBJDIR)/server/cli_argspec_defs.o $(OBJDIR)/server/server_http_conn_worker.o $(OBJDIR)/server/server_http_response.o $(OBJDIR)/server/server_http_sse.o $(OBJDIR)/server/server_http_reqctx.o $(OBJDIR)/server/request_context.o $(OBJDIR)/server/server_http_identity.o $(OBJDIR)/server/server_http_authz.o $(OBJDIR)/tests/support/git_route_stub.o $(OBJDIR)/tests/support/workflow_api_stub.o $(OBJDIR)/tests/support/router_advise_stub.o $(OBJDIR)/modules/vault/vault_principal.o $(OBJDIR)/server/presence.o \
                           $(OBJDIR)/server/server_mgmt_status.o $(OBJDIR)/server/server_mgmt_endpoint.o $(OBJDIR)/shared/management_read.o $(OBJDIR)/server/server_mgmt_read_endpoint.o $(OBJDIR)/server/server_mgmt_read_source.o $(OBJDIR)/server/server_mgmt_audit.o $(OBJDIR)/shared/auth_token_verify.o $(OBJDIR)/kb/kb_mgmt_status.o $(OBJDIR)/kb/kb_mgmt_endpoint.o \
                           $(OBJDIR)/modules/db1/mgmt_nonce.o \
                           $(OBJDIR)/server/cli_session_pty.o $(OBJDIR)/server/cli_session.o $(OBJDIR)/posix/workspace_provider.o \
                           $(OBJDIR)/modules/workspace/workspace_runner_registry.o $(OBJDIR)/modules/workspace/workspace_runner_queue.o \
                           $(OBJDIR)/modules/git/forge_credentials.o \
                           $(OBJDIR)/delivery_target.o \
                           $(OBJDIR)/server/openai_shape.o \
                           $(OBJDIR)/server/openai_runs_store.o $(OBJDIR)/server/server_auth.o \
                           $(OBJDIR)/server/compute_pool.o \
                           $(OBJDIR)/modules/routing/agent_config.o $(OBJDIR)/modules/vault/agent_credentials.o $(OBJDIR)/modules/routing/agent_registry.o $(OBJDIR)/modules/routing/routing.o $(OBJDIR)/tests/support/provider_cli_adapter_stub.o $(OBJDIR)/tests/support/model_provider_stub.o $(OBJDIR)/tests/support/vault_service_stub.o $(OBJDIR)/tests/support/oauth_tokens_stub.o \
                           $(OBJDIR)/persona.o $(OBJDIR)/prompts.o \
                           $(OBJDIR)/modules/roundtable/roundtable_preset.o \
                           $(OBJDIR)/role_templates.o \
                           $(OBJDIR)/dstr.o $(OBJDIR)/working_profile.o \
                           $(OBJDIR)/modules/roundtable/roundtable_pipeline_capture.o \
                           $(OBJDIR)/modules/roundtable/roundtable_pipeline_eval.o \
                           $(DB1_TEST_OBJS) $(DB1_MIGRATED_OBJS) \
                           $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-openai-shape: $(OBJDIR)/tests/test_openai_shape.o $(OBJDIR)/modules/governance/gw_stage_completion.o \
                           $(OBJDIR)/server/openai_shape.o \
                           $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-openai-chat-policed: $(OBJDIR)/tests/test_openai_chat_policed.o $(OBJDIR)/modules/governance/gw_stage_completion.o \
                           $(OBJDIR)/server/openai_shape.o \
                           $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-command-registry: $(OBJDIR)/tests/test_command_registry.o \
                           $(OBJDIR)/command_registry.o $(OBJDIR)/log.o $(OBJDIR)/dstr.o \
                           $(OBJDIR)/modules/protocols/mcp/mcp_group_tool.o \
                           $(OBJDIR)/modules/protocols/mcp/mcp_tools.o \
                           $(OBJDIR)/modules/protocols/mcp/mcp_tools_extended.o \
                           $(OBJDIR)/modules/protocols/mcp/mcp_skill_tools.o \
                           $(OBJDIR)/modules/protocols/mcp/mcp_tools_gateway.o \
                           $(OBJDIR)/modules/protocols/mcp/mcp_tool_profile.o \
                           $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-openai-responses-store: $(OBJDIR)/tests/test_openai_responses_store.o $(OBJDIR)/modules/governance/gw_stage_completion.o \
                           $(OBJDIR)/server/openai_responses_store.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-cmd-session: $(OBJDIR)/tests/test_cmd_session.o \
                             $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-model-registry: $(OBJDIR)/tests/test_model_registry.o \
                                $(OBJDIR)/model_registry.o $(OBJDIR)/models_dev.o \
                                $(OBJDIR)/models_dev_cache.o $(OBJDIR)/aimee_home.o \
                                $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-agent-tier-lint: $(OBJDIR)/tests/test_agent_tier_lint.o \
                                $(OBJDIR)/agent_tier_lint.o \
                                $(OBJDIR)/model_registry.o $(OBJDIR)/models_dev.o \
                                $(OBJDIR)/models_dev_cache.o $(OBJDIR)/aimee_home.o \
                                $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-models-dev: $(OBJDIR)/tests/test_models_dev.o \
                                $(OBJDIR)/model_registry.o $(OBJDIR)/models_dev.o \
                                $(OBJDIR)/models_dev_cache.o $(OBJDIR)/aimee_home.o \
                                $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-model-provider: $(OBJDIR)/tests/test_model_provider.o \
                                $(OBJDIR)/server/model_provider.o \
                                $(OBJDIR)/server/openai_profile.o \
                                $(OBJDIR)/server/anthropic_profile.o \
                                $(OBJDIR)/server/openrouter_profile.o \
                                $(OBJDIR)/server/ollama_profile.o \
                                $(OBJDIR)/server/llama_native_profile.o \
                                $(OBJDIR)/server/mistral_profile.o \
                                $(OBJDIR)/server/minimax_profile.o \
                                $(OBJDIR)/tests/support/mock_agent_http.o \
                                $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-delegate-driver: $(OBJDIR)/tests/test_delegate_driver.o \
                                 $(OBJDIR)/modules/delegates/delegate_driver.o \
                                 $(OBJDIR)/server/agent_request_shaping.o \
                                 $(OBJDIR)/modules/delegates/delegate_openai.o \
                                 $(OBJDIR)/modules/delegates/delegate_xml_fallback.o \
                                 $(OBJDIR)/model_registry.o $(OBJDIR)/models_dev.o $(OBJDIR)/models_dev_cache.o \
                                 $(OBJDIR)/server/agent_tools.o \
                                 $(OBJDIR)/modules/delegates/delegate_role.o \
                                 $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-agent-http: $(OBJDIR)/tests/test_agent_http.o \
                                 $(OBJDIR)/models_dev.o $(OBJDIR)/models_dev_cache.o \
                                $(OBJDIR)/posix/agent_ir_parse.o $(OBJDIR)/modules/translation/aimee_backend_openai.o \
                                $(OBJDIR)/modules/translation/aimee_backend_anthropic.o $(OBJDIR)/modules/ir/aimee_ir.o \
                                $(OBJDIR)/modules/delegates/aimee_ir_rescue.o $(OBJDIR)/modules/ir/aimee_ir_metrics.o \
                                $(OBJDIR)/server/agent_bridge.o $(OBJDIR)/server/anthropic_shape.o $(OBJDIR)/server/tool_call_args.o \
                                $(OBJDIR)/server/agent_request_shaping.o \
                                $(OBJDIR)/modules/delegates/delegate_driver.o \
                                $(OBJDIR)/modules/delegates/delegate_openai.o \
                                $(OBJDIR)/modules/delegates/delegate_xml_fallback.o \
                                $(OBJDIR)/model_registry.o \
                                $(OBJDIR)/server/agent_tools.o \
                                $(OBJDIR)/modules/delegates/delegate_role.o \
                                $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Links the real dispatch TU against the shared test object sets — no stubs needed
# (TEST_DATA_OBJS + TEST_WORKSPACE_OBJS_EXTRA already cover every td_* handler's
# dependencies). The provider itself is faked in the test: the point under test is
# the routing, not any MCP handler.
$(TESTPREFIX)/unit-test-mcp-native-dispatch: \
                     $(OBJDIR)/db1_store_ready.o \
                      $(OBJDIR)/tests/support/role_template_toolset_stub.o \
                                       $(OBJDIR)/tests/test_mcp_native_dispatch.o \
                                       $(OBJDIR)/modules/tools/agent_tools_dispatch.o \
                                       $(OBJDIR)/db1_store_ready.o \
                                       $(OBJDIR)/modules/tools/agent_tools_completion.o \
                                       $(OBJDIR)/server/agent_tools.o \
                                       $(OBJDIR)/modules/delegates/delegate_role.o \
                                       $(OBJDIR)/toolset.o \
                                       $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-mcp-native-surface: $(OBJDIR)/tests/test_mcp_native_surface.o \
                                 $(OBJDIR)/models_dev.o $(OBJDIR)/models_dev_cache.o \
                                $(OBJDIR)/server/agent_bridge.o $(OBJDIR)/server/anthropic_shape.o $(OBJDIR)/server/tool_call_args.o \
                                $(OBJDIR)/server/agent_request_shaping.o \
                                $(OBJDIR)/modules/delegates/delegate_driver.o \
                                $(OBJDIR)/modules/delegates/delegate_openai.o \
                                $(OBJDIR)/modules/delegates/delegate_xml_fallback.o \
                                $(OBJDIR)/model_registry.o \
                                $(OBJDIR)/server/agent_tools.o \
                                $(OBJDIR)/modules/delegates/delegate_role.o \
                                $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-middleware: $(OBJDIR)/tests/test_middleware.o $(OBJDIR)/log.o \
                                    $(OBJDIR)/model_registry.o $(OBJDIR)/aimee_home.o $(OBJDIR)/vendor/cJSON.o $(OBJDIR)/models_dev.o $(OBJDIR)/models_dev_cache.o \
                                    $(PLATFORM_BASIC_OBJS)
	$(TESTLINK_MIN) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-verify-hook: $(OBJDIR)/tests/test_verify_hook.o \
	                                     $(OBJDIR)/modules/git/git_verify.o $(OBJDIR)/modules/git/git_verify_state.o $(OBJDIR)/modules/git/git_verify_config.o $(OBJDIR)/modules/git/git_verify_jobs.o $(OBJDIR)/modules/git/git_verify_hook.o $(OBJDIR)/modules/git/git_verify_ops.o $(OBJDIR)/modules/git/git_verify_select.o $(OBJDIR)/modules/git/git_verify_step.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-pipeline: $(OBJDIR)/tests/test_pipeline.o \
                                   $(OBJDIR)/server/agent_pipeline.o \
	                                   $(OBJDIR)/modules/git/git_verify.o $(OBJDIR)/modules/git/git_verify_state.o $(OBJDIR)/modules/git/git_verify_config.o $(OBJDIR)/modules/git/git_verify_jobs.o $(OBJDIR)/modules/git/git_verify_hook.o $(OBJDIR)/modules/git/git_verify_ops.o $(OBJDIR)/modules/git/git_verify_select.o $(OBJDIR)/modules/git/git_verify_step.o \
                                   $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-process-mgr: $(OBJDIR)/tests/test_process_mgr.o \
                                      $(OBJDIR)/server/server_mcp_process.o \
                                      $(OBJDIR)/server/process_mgr.o $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lpthread

$(TESTPREFIX)/unit-test-proxy-bootstrap: $(OBJDIR)/tests/test_proxy_bootstrap.o \
                                          $(OBJDIR)/proxy_bootstrap.o $(OBJDIR)/log.o \
                                          $(OBJDIR)/util.o $(PLATFORM_BASIC_OBJS) $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-cmd-run: $(OBJDIR)/tests/test_cmd_run.o \
                                  $(OBJDIR)/cmd_run.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-conversation: $(OBJDIR)/tests/test_conversation.o \
                                       $(OBJDIR)/conversation.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-provider-settable: $(OBJDIR)/tests/test_provider_settable.o \
                                         $(OBJDIR)/server/provider_settable.o \
                                         $(OBJDIR)/server/agent_adapter.o \
                                         $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-agent-max-turns: $(OBJDIR)/tests/test_agent_max_turns.o \
                                         $(OBJDIR)/posix/agent_max_turns.o $(OBJDIR)/cJSON.o \
                                         $(OBJDIR)/log.o $(OBJDIR)/util.o $(OBJDIR)/platform_random.o \
                                         $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-agent-loop: $(OBJDIR)/tests/test_agent_loop.o \
                                     $(OBJDIR)/server/agent_loop.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o \
                                     $(OBJDIR)/util.o $(OBJDIR)/text.o $(OBJDIR)/log.o \
                                     $(OBJDIR)/platform_random.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-file-snapshot: $(OBJDIR)/tests/test_file_snapshot.o \
                                        $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o $(OBJDIR)/modules/db1/wm.o $(OBJDIR)/modules/db1/fsnap.o \
                                        $(OBJDIR)/log.o $(OBJDIR)/util.o $(OBJDIR)/platform_random.o \
                                        $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-execution-trace: $(OBJDIR)/tests/test_execution_trace.o \
                                          $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o $(OBJDIR)/modules/db1/execution_trace.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-diagnose: $(OBJDIR)/tests/test_diagnose.o \
                                   $(OBJDIR)/diagnose_render.o \
                                   $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o $(OBJDIR)/modules/db1/diagnose.o \
                                   $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm

$(TESTPREFIX)/unit-test-clarify: $(OBJDIR)/tests/test_clarify.o \
                                 $(OBJDIR)/clarify_render.o \
                                  $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o $(OBJDIR)/modules/db1/clarify.o \
                                  $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# DB1 server-owned per-model price table (ingress-cost-accounting §2).
$(TESTPREFIX)/unit-test-model-pricing: $(OBJDIR)/tests/test_model_pricing.o \
                                  $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                                  $(OBJDIR)/modules/db1/model_pricing.o \
                                  $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-provider-client: $(OBJDIR)/tests/test_provider_client.o \
                                  $(OBJDIR)/provider_client.o $(OBJDIR)/cJSON.o \
                                  $(OBJDIR)/text.o \
                                  $(OBJDIR)/tests/support/mock_agent_http.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# The focused accessor fixture supplies the curator provider settings.
$(TESTPREFIX)/unit-test-kb-curator-provider: $(OBJDIR)/tests/support/curator_config_stub.o \
                                       $(OBJDIR)/tests/test_kb_curator_provider.o \
                                  $(OBJDIR)/kb_curator_provider.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-curator-llm: $(OBJDIR)/tests/support/curator_config_stub.o \
                                       $(OBJDIR)/tests/test_kb_curator_llm.o \
                                  $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_llm.o $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_sidecar.o \
                                  $(OBJDIR)/kb_curator_provider.o $(OBJDIR)/provider_client.o \
                                  $(OBJDIR)/text.o \
                                  $(OBJDIR)/cJSON.o $(OBJDIR)/tests/support/mock_agent_http.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-collab-rules: $(OBJDIR)/tests/test_collab_rules.o $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-json-fluent: $(OBJDIR)/tests/test_json_fluent.o $(OBJDIR)/json_fluent.o \
                                      $(OBJDIR)/modules/db1/db.o $(OBJDIR)/modules/db1/db_schema.o \
                                      $(DB2_TEST_BACKEND_OBJ) $(OBJDIR)/db2/db2_test_shim.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-cmd-config: $(OBJDIR)/tests/test_cmd_config.o $(OBJDIR)/cmd_data.o \
                                     $(OBJDIR)/cmd_util.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-cmd-table: $(OBJDIR)/tests/test_cmd_table.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-tool-validation: $(OBJDIR)/tests/test_tool_validation.o \
                                          $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-turn-narration: $(OBJDIR)/tests/test_turn_narration.o \
                                         $(OBJDIR)/turn_narration.o $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-kb: $(OBJDIR)/tests/test_kb.o $(OBJDIR)/kb/kb.o $(OBJDIR)/db2/code_index.o $(OBJDIR)/db2/kb_runtime_state.o $(OBJDIR)/kb/kb_ingest_workers.o $(OBJDIR)/kb/kb_bandit.o $(OBJDIR)/kb/kb_bandit_registry.o $(OBJDIR)/db2/bandit.o $(OBJDIR)/kb/modules/kb-synthesis/kb_curator_notify.o $(OBJDIR)/kb/kb_fusion.o $(OBJDIR)/kb/kb_neardup.o $(OBJDIR)/kb/kb_conventions.o \
                             $(OBJDIR)/sketch.o $(OBJDIR)/db2/sketch.o \
                             $(OBJDIR)/modules/memory/memory_core.o $(OBJDIR)/modules/memory/memory_core_crud.o $(OBJDIR)/modules/memory/memory_core_helpers.o $(OBJDIR)/modules/memory/memory_core_helpers_b.o $(OBJDIR)/modules/memory/memory_core_search.o $(OBJDIR)/modules/memory/memory_core_search_b.o $(OBJDIR)/modules/memory/memory_core_search_c.o $(OBJDIR)/modules/memory/memory_core_scope_embed.o $(OBJDIR)/modules/memory/memory_core_tiers.o $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db2_tenant.o $(OBJDIR)/kb/kb_identity.o $(OBJDIR)/db2/db_schema.o $(OBJDIR)/db2/kb_payload.o $(OBJDIR)/db2/kb_service_backend.o $(OBJDIR)/db2/kb_service_backend_ingest.o $(OBJDIR)/db2/memory_lifecycle.o $(OBJDIR)/db2/memory_payload.o $(OBJDIR)/db2/memory_promotion.o $(OBJDIR)/db2/memory_query.o $(OBJDIR)/db2/memory_query_bookkeeping.o $(OBJDIR)/db2/memory_entity_graph.o $(OBJDIR)/db2/memory_score_fields.o $(OBJDIR)/db2/memory_scope_query.o $(OBJDIR)/db2/memory_scenes.o $(OBJDIR)/db2/memory_briefing.o $(OBJDIR)/db2/memory_health.o $(OBJDIR)/db2/memory_row_mapper_pg.o $(OBJDIR)/db2/memory_relations.o $(OBJDIR)/db2/memory_conflicts.o $(OBJDIR)/db2/vector_index_ops.o $(OBJDIR)/db2/code_index_ops.o $(OBJDIR)/db2/rules.o $(OBJDIR)/db2/stopwords.o $(OBJDIR)/db2/tool_registry.o $(OBJDIR)/db2/feedback.o $(OBJDIR)/db2/notes.o $(OBJDIR)/db2/anti_patterns.o $(OBJDIR)/modules/db1/approach_failures.o $(OBJDIR)/approach_store.o $(OBJDIR)/db2/curiosity.o $(OBJDIR)/db2/entity_edges.o $(OBJDIR)/db2/entity_profiles.o $(OBJDIR)/db2/epistemic_directives.o $(OBJDIR)/db2/failed_queries.o $(OBJDIR)/db2/kind_lifecycle.o $(OBJDIR)/db2/pgvec_transport.o $(OBJDIR)/db2/memory_vectors.o $(OBJDIR)/db2/kb_vectors.o $(OBJDIR)/db2/vector_status.o $(OBJDIR)/db2/pgvec_verify.o $(OBJDIR)/db2/pgvec_kb_service.o $(OBJDIR)/tests/support/mock_agent_http.o $(OBJDIR)/tests/support/memory_embed_stub.o $(OBJDIR)/tests/support/kb_client_test_stub.o $(OBJDIR)/tests/support/kb_ws_stub.o $(OBJDIR)/posix/memory.o \
                             $(OBJDIR)/modules/memory/memory_logic.o $(OBJDIR)/modules/memory/memory_health.o $(OBJDIR)/db2/fact_lifecycle.o $(TEST_FACT_MUTATION_MIN_OBJS) $(OBJDIR)/modules/memory/memory_conflict.o $(OBJDIR)/modules/memory/memory_context.o $(OBJDIR)/modules/memory/memory_assemble.o \
                              $(OBJDIR)/modules/memory/memory_advanced.o $(OBJDIR)/modules/memory/memory_prospective.o $(OBJDIR)/modules/memory/memory_lifecycle.o $(OBJDIR)/modules/memory/memory_directives.o $(OBJDIR)/modules/memory/memory_maintenance.o $(OBJDIR)/modules/memory/memory_graph.o $(OBJDIR)/modules/memory/memory_graph_fusion.o $(OBJDIR)/modules/memory/memory_scan.o $(OBJDIR)/modules/memory/memory_improve.o $(OBJDIR)/modules/memory/memory_episodes.o \
                             $(OBJDIR)/tests/support/learning_implicit_stub.o $(OBJDIR)/dogfood.o \
                             $(OBJDIR)/kb/kb_features.o $(OBJDIR)/kb/kb_ranker.o $(OBJDIR)/kb/kb_detect.o $(OBJDIR)/kb/kb_mdl.o \
                             $(OBJDIR)/db2/feature_rows.o $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                             $(OBJDIR)/db2/calibration.o \
                             $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm -lzstd

$(TESTPREFIX)/unit-test-memory-retrieval-eval: $(OBJDIR)/kb/kb_bandit.o $(OBJDIR)/kb/kb_bandit_registry.o $(OBJDIR)/db2/bandit.o \
                             $(OBJDIR)/tests/test_memory_retrieval_eval.o \
                             $(OBJDIR)/modules/benchmarks/agent_eval.o $(OBJDIR)/modules/benchmarks/agent_eval_memory_support.o $(OBJDIR)/modules/benchmarks/agent_eval_baseline.o \
                             $(OBJDIR)/modules/memory/memory_core.o $(OBJDIR)/modules/memory/memory_core_crud.o $(OBJDIR)/modules/memory/memory_core_helpers.o $(OBJDIR)/modules/memory/memory_core_helpers_b.o $(OBJDIR)/modules/memory/memory_core_search.o $(OBJDIR)/modules/memory/memory_core_search_b.o $(OBJDIR)/modules/memory/memory_core_search_c.o $(OBJDIR)/modules/memory/memory_core_scope_embed.o $(OBJDIR)/modules/memory/memory_core_tiers.o $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o $(OBJDIR)/db2/kb_payload.o $(OBJDIR)/db2/kb_service_backend.o $(OBJDIR)/db2/kb_service_backend_ingest.o $(OBJDIR)/db2/memory_lifecycle.o $(OBJDIR)/db2/memory_payload.o $(OBJDIR)/db2/memory_promotion.o $(OBJDIR)/db2/memory_query.o $(OBJDIR)/db2/memory_query_bookkeeping.o $(OBJDIR)/db2/memory_entity_graph.o $(OBJDIR)/db2/memory_score_fields.o $(OBJDIR)/db2/memory_scope_query.o $(OBJDIR)/db2/memory_scenes.o $(OBJDIR)/db2/memory_briefing.o $(OBJDIR)/db2/memory_health.o $(OBJDIR)/db2/memory_row_mapper_pg.o $(OBJDIR)/db2/memory_relations.o $(OBJDIR)/db2/memory_conflicts.o $(OBJDIR)/db2/vector_index_ops.o $(OBJDIR)/db2/code_index_ops.o $(OBJDIR)/db2/rules.o $(OBJDIR)/db2/stopwords.o $(OBJDIR)/db2/tool_registry.o $(OBJDIR)/db2/feedback.o $(OBJDIR)/db2/notes.o $(OBJDIR)/db2/anti_patterns.o $(OBJDIR)/modules/db1/approach_failures.o $(OBJDIR)/approach_store.o $(OBJDIR)/db2/curiosity.o $(OBJDIR)/db2/entity_edges.o $(OBJDIR)/db2/entity_profiles.o $(OBJDIR)/db2/epistemic_directives.o $(OBJDIR)/db2/failed_queries.o $(OBJDIR)/db2/kind_lifecycle.o $(OBJDIR)/db2/pgvec_transport.o $(OBJDIR)/db2/memory_vectors.o $(OBJDIR)/db2/kb_vectors.o $(OBJDIR)/db2/vector_status.o $(OBJDIR)/db2/pgvec_verify.o $(OBJDIR)/db2/pgvec_kb_service.o $(OBJDIR)/tests/support/mock_agent_http.o $(OBJDIR)/tests/support/memory_embed_stub.o $(OBJDIR)/tests/support/kb_client_test_stub.o $(OBJDIR)/posix/memory.o \
                             $(OBJDIR)/modules/memory/memory_logic.o $(OBJDIR)/modules/memory/memory_health.o $(OBJDIR)/db2/fact_lifecycle.o $(TEST_FACT_MUTATION_MIN_OBJS) $(OBJDIR)/modules/memory/memory_conflict.o $(OBJDIR)/modules/memory/memory_context.o $(OBJDIR)/modules/memory/memory_assemble.o \
                              $(OBJDIR)/modules/memory/memory_advanced.o $(OBJDIR)/modules/memory/memory_prospective.o $(OBJDIR)/modules/memory/memory_lifecycle.o $(OBJDIR)/modules/memory/memory_directives.o $(OBJDIR)/modules/memory/memory_maintenance.o $(OBJDIR)/modules/memory/memory_graph.o $(OBJDIR)/modules/memory/memory_graph_fusion.o $(OBJDIR)/modules/memory/memory_scan.o $(OBJDIR)/modules/memory/memory_improve.o $(OBJDIR)/modules/memory/memory_episodes.o \
                             $(OBJDIR)/kb/kb.o $(OBJDIR)/db2/code_index.o $(OBJDIR)/kb/kb_neardup.o $(OBJDIR)/kb/kb_conventions.o $(OBJDIR)/kb/kb_mdl.o $(OBJDIR)/sketch.o $(OBJDIR)/db2/sketch.o \
                             $(OBJDIR)/tests/support/learning_implicit_stub.o $(OBJDIR)/dogfood.o $(DB1_TEST_OBJS) $(DB1_MIGRATED_OBJS) \
                             $(OBJDIR)/kb/kb_features.o $(OBJDIR)/kb/kb_ranker.o $(OBJDIR)/kb/kb_detect.o \
                             $(OBJDIR)/db2/feature_rows.o $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                             $(OBJDIR)/db2/calibration.o \
                             $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm -lzstd


$(TESTPREFIX)/unit-test-ensemble: $(OBJDIR)/tests/test_ensemble.o \
                     $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o $(OBJDIR)/modules/db1/ensemble.o \
                     $(OBJDIR)/ensemble_render.o $(OBJDIR)/ensemble_view.o \
                     $(CONFIG_CLIENT_TEST_OBJS) \
                     $(OBJDIR)/aimee_home.o \
                     $(OBJDIR)/yaml.o \
                     $(OBJDIR)/dstr.o \
                     $(OBJDIR)/util.o \
                     $(OBJDIR)/text.o \
                     $(OBJDIR)/log.o \
                     $(OBJDIR)/cJSON.o \
                     $(OBJDIR)/platform_random.o \
                     $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-cli-session: $(OBJDIR)/tests/test_cli_session.o \
                     $(OBJDIR)/server/cli_session.o \
                     $(OBJDIR)/posix/workspace_provider.o \
                     $(OBJDIR)/util.o \
                     $(OBJDIR)/text.o \
                     $(OBJDIR)/cJSON.o \
                     $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-cli-session-pty: $(OBJDIR)/tests/test_cli_session_pty.o \
                     $(OBJDIR)/server/cli_session_pty.o \
                     $(OBJDIR)/server/cli_session.o \
                     $(OBJDIR)/posix/workspace_provider.o \
                     $(OBJDIR)/util.o \
                     $(OBJDIR)/text.o \
                     $(OBJDIR)/cJSON.o \
                     $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-cli-codex: $(OBJDIR)/tests/test_cli_codex.o \
                     $(OBJDIR)/server/cli_codex.o \
                     $(OBJDIR)/cJSON.o \
                     $(OBJDIR)/log.o \
                     $(OBJDIR)/dstr.o \
                     $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-delegate-backend: $(OBJDIR)/tests/test_delegate_backend.o \
                     $(OBJDIR)/modules/delegates/delegate_backend.o \
                     $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-delegate-backend-docker: $(OBJDIR)/tests/test_delegate_backend_docker.o \
                     $(OBJDIR)/modules/delegates/delegate_backend.o \
                     $(OBJDIR)/modules/delegates/delegate_backend_docker.o $(OBJDIR)/log.o \
                     $(OBJDIR)/modules/delegates/delegate_launch_args.o \
                     $(OBJDIR)/aimee_home.o \
                     $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-otel: $(OBJDIR)/tests/test_otel.o \
                     $(OBJDIR)/server/otel.o \
                     $(OBJDIR)/server/agent_bridge.o $(OBJDIR)/server/anthropic_shape.o $(OBJDIR)/server/tool_call_args.o \
                     $(OBJDIR)/server/agent_request_shaping.o \
                     $(OBJDIR)/server/http_retry.o \
                     $(OBJDIR)/modules/gateway/gateway_delegate.o $(OBJDIR)/modules/gateway/gateway_pipeline.o $(OBJDIR)/modules/gateway/gateway_policy.o \
                     $(TEST_CORE_OBJS) \
                     $(PLATFORM_AGENT_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-oauth-pkce: $(OBJDIR)/tests/test_oauth_pkce.o \
                     $(OBJDIR)/server/oauth_pkce.o \
                     $(OBJDIR)/server/oauth_tokens.o \
                     $(OBJDIR)/modules/db1/secrets.o \
                     $(OBJDIR)/modules/vault/vault_service.o \
                     $(OBJDIR)/modules/vault/vault_store.o $(OBJDIR)/modules/vault/vault_kek_check.o \
                     $(OBJDIR)/modules/vault/vault_crypto.o \
                     $(OBJDIR)/modules/vault/vault_kek_cache.o \
                     $(OBJDIR)/modules/vault/vault_server_key.o \
                     $(OBJDIR)/platform_random.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-oauth-reauth: $(OBJDIR)/tests/test_oauth_reauth.o \
                     $(OBJDIR)/server/oauth_pkce.o \
                     $(OBJDIR)/server/oauth_tokens.o \
                     $(OBJDIR)/modules/db1/secrets.o \
                     $(OBJDIR)/modules/vault/vault_service.o \
                     $(OBJDIR)/modules/vault/vault_store.o $(OBJDIR)/modules/vault/vault_kek_check.o \
                     $(OBJDIR)/modules/vault/vault_crypto.o \
                     $(OBJDIR)/modules/vault/vault_kek_cache.o \
                     $(OBJDIR)/modules/vault/vault_server_key.o \
                     $(OBJDIR)/platform_random.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-enroll: $(OBJDIR)/tests/test_kb_enroll.o \
                     $(OBJDIR)/kb/enroll.o \
                     $(OBJDIR)/kb/pki.o \
                     $(OBJDIR)/modules/vault/vault_service.o \
                     $(OBJDIR)/modules/vault/vault_store.o \
                     $(OBJDIR)/modules/vault/vault_kek_check.o \
                     $(OBJDIR)/modules/vault/vault_server_key.o \
                     $(OBJDIR)/modules/vault/vault_crypto.o \
                     $(OBJDIR)/modules/vault/vault_kek_cache.o \
                     $(OBJDIR)/server/oauth_pkce.o \
                     $(OBJDIR)/platform_random.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-verifier: $(OBJDIR)/tests/test_kb_verifier.o \
                     $(OBJDIR)/kb/verifier.o \
                     $(OBJDIR)/kb/kb_scope.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-tls: $(OBJDIR)/tests/test_kb_tls.o \
                     $(OBJDIR)/kb/http/kb_tls.o \
                     $(OBJDIR)/kb/pki.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-auth-oidc: $(OBJDIR)/tests/test_kb_auth_oidc.o \
                     $(OBJDIR)/kb/auth_oidc.o \
                     $(OBJDIR)/kb/verifier.o \
                     $(OBJDIR)/kb/kb_scope.o \
                     $(OBJDIR)/server/oauth_pkce.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# The login core is pure, so it links only the OIDC verifier (for the nonce
# claim reader), the identity-key builder, and the PKCE primitives. The test
# overrides platform_random_bytes, so platform objects are deliberately absent.
$(TESTPREFIX)/unit-test-kb-oidc-login: $(OBJDIR)/tests/test_kb_oidc_login.o \
                     $(OBJDIR)/kb/kb_oidc_login.o \
                     $(OBJDIR)/kb/auth_oidc.o \
                     $(OBJDIR)/kb/kb_identity.o \
                     $(OBJDIR)/server/oauth_pkce.o \
                     $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto

$(TESTPREFIX)/unit-test-kb-oidc-login-store: $(OBJDIR)/tests/test_kb_oidc_login_store.o \
                     $(OBJDIR)/kb/kb_oidc_login_store.o \
                     $(OBJDIR)/kb/kb_oidc_login.o \
                     $(OBJDIR)/kb/auth_oidc.o \
                     $(OBJDIR)/kb/kb_identity.o \
                     $(OBJDIR)/server/oauth_pkce.o \
                     $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto

$(TESTPREFIX)/unit-test-kb-oidc-token-exchange: $(OBJDIR)/tests/test_kb_oidc_token_exchange.o \
                     $(OBJDIR)/kb/kb_oidc_token_exchange.o \
                     $(OBJDIR)/kb/kb_oidc_login.o \
                     $(OBJDIR)/kb/auth_oidc.o \
                     $(OBJDIR)/kb/kb_identity.o \
                     $(OBJDIR)/server/oauth_pkce.o \
                     $(OBJDIR)/util.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto

# Composition test: all four increment-4a units linked together, so a seam
# mismatch between them fails here rather than at a real login.
$(TESTPREFIX)/unit-test-kb-oidc-login-flow: $(OBJDIR)/tests/test_kb_oidc_login_flow.o \
                     $(OBJDIR)/kb/kb_oidc_login.o \
                     $(OBJDIR)/kb/kb_oidc_login_store.o \
                     $(OBJDIR)/kb/kb_oidc_token_exchange.o \
                     $(OBJDIR)/kb/auth_oidc.o \
                     $(OBJDIR)/kb/kb_identity.o \
                     $(OBJDIR)/server/oauth_pkce.o \
                     $(OBJDIR)/util.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto

# The db2 seam is stubbed IN THE TEST TU: it needs Postgres, and its own behaviour is
# covered by the P1 RLS gate. What this pins is the routing and validation layer — which
# requests reach the seam at all, and with what arguments.
$(TESTPREFIX)/unit-test-kb-http-grants: $(OBJDIR)/tests/test_kb_http_grants.o \
                     $(OBJDIR)/kb/http/kb_http_grants.o \
                     $(OBJDIR)/kb/kb_identity.o \
                     $(OBJDIR)/kb/kb_identity_token.o \
                     $(OBJDIR)/util.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o $(OBJDIR)/log.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto

$(TESTPREFIX)/unit-test-kb-http-identity-login: \
                     $(OBJDIR)/tests/test_kb_http_identity_login.o \
                     $(OBJDIR)/kb/http/kb_http_identity_login.o \
                     $(OBJDIR)/kb/kb_login_throttle.o \
                     $(OBJDIR)/kb/kb_oidc_token_exchange.o \
                     $(OBJDIR)/kb/kb_oidc_login.o \
                     $(OBJDIR)/kb/kb_oidc_login_store.o \
                     $(OBJDIR)/kb/auth_oidc.o \
                     $(OBJDIR)/kb/verifier.o \
                     $(OBJDIR)/kb/kb_scope.o \
                     $(OBJDIR)/kb/kb_identity.o \
                     $(OBJDIR)/kb/kb_reqctx.o \
                     $(OBJDIR)/server/oauth_pkce.o \
                     $(OBJDIR)/util.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o $(OBJDIR)/log.o \
                     $(CORE_CONNECTION_LIB)
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto

# Both C copies of the subject grammar, against tests/subject_corpus.h. Links the
# server's token unit for its predicate; the db2 one is header-only inline.
$(TESTPREFIX)/unit-test-subject-grammar: $(OBJDIR)/tests/test_subject_grammar.o \
                     $(OBJDIR)/shared/auth_token_verify.o \
                     $(OBJDIR)/kb/kb_mgmt_token_authority.o \
                     $(OBJDIR)/kb/kb_mgmt_token_public.o \
                     $(OBJDIR)/kb/kb_identity_token.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-sidecar-identity: \
                     $(OBJDIR)/tests/test_kb_sidecar_identity.o \
                     $(OBJDIR)/kb/kb_sidecar_identity.o \
                     $(OBJDIR)/kb/pki.o \
                     $(OBJDIR)/kb/modules/vault/vault_crypto.o \
                     $(OBJDIR)/kb/modules/vault/vault_server_key.o \
                     $(OBJDIR)/kb/modules/vault/vault_store.o $(OBJDIR)/kb/modules/vault/vault_kek_check.o \
                     $(OBJDIR)/kb/modules/vault/vault_kek_cache.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# The kb's own HTTP client against a peer that demands a client certificate. Links
# the real agent_bridge, because the defect was in which SSL_CTX it chose.
$(TESTPREFIX)/unit-test-synthesis-mtls-client: \
                     $(OBJDIR)/tests/test_synthesis_mtls_client.o \
                     $(OBJDIR)/posix/agent_bridge.o \
                     $(OBJDIR)/proxy_bootstrap.o \
                     $(OBJDIR)/kb/kb_sidecar_identity.o \
                     $(OBJDIR)/kb/pki.o \
                     $(OBJDIR)/kb/modules/vault/vault_crypto.o \
                     $(OBJDIR)/kb/modules/vault/vault_server_key.o \
                     $(OBJDIR)/kb/modules/vault/vault_store.o $(OBJDIR)/kb/modules/vault/vault_kek_check.o \
                     $(OBJDIR)/kb/modules/vault/vault_kek_cache.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-pki: $(OBJDIR)/tests/test_kb_pki.o \
                     $(OBJDIR)/kb/pki.o \
                     $(OBJDIR)/kb/modules/vault/vault_crypto.o \
                     $(OBJDIR)/kb/modules/vault/vault_server_key.o \
                     $(OBJDIR)/kb/modules/vault/vault_store.o $(OBJDIR)/kb/modules/vault/vault_kek_check.o \
                     $(OBJDIR)/kb/modules/vault/vault_kek_cache.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-managed-server-identity: \
                     $(OBJDIR)/tests/test_managed_server_identity.o \
                     $(OBJDIR)/kb/managed_server_identity.o \
                     $(OBJDIR)/kb/pki.o \
                     $(OBJDIR)/kb/modules/vault/vault_crypto.o \
                     $(OBJDIR)/kb/modules/vault/vault_server_key.o \
                     $(OBJDIR)/kb/modules/vault/vault_store.o \
                     $(OBJDIR)/kb/modules/vault/vault_kek_check.o \
                     $(OBJDIR)/kb/modules/vault/vault_kek_cache.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-context-engine: $(OBJDIR)/tests/test_context_engine.o \
                     $(OBJDIR)/server/context_engine.o \
                     $(OBJDIR)/server/session_compact.o $(OBJDIR)/server/rounds_to_resume.o $(OBJDIR)/server/compact_prune.o \
                     $(OBJDIR)/server/agent_bridge.o $(OBJDIR)/server/anthropic_shape.o $(OBJDIR)/server/tool_call_args.o \
                     $(OBJDIR)/server/agent_request_shaping.o \
                     $(OBJDIR)/modules/delegates/delegate_driver.o \
                     $(OBJDIR)/modules/delegates/delegate_openai.o \
                     $(OBJDIR)/modules/delegates/delegate_xml_fallback.o \
                     $(OBJDIR)/model_registry.o $(OBJDIR)/models_dev.o $(OBJDIR)/models_dev_cache.o \
                     $(OBJDIR)/server/agent_tools.o \
                     $(OBJDIR)/modules/delegates/delegate_role.o \
                     $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-mcp-client: $(OBJDIR)/tests/test_mcp_client.o \
                     $(TEST_MCP_CLIENT_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(OBJDIR)/tests/support/mock_mcp_server.o: tests/support/mock_mcp_server.c
	@mkdir -p $(dir $@) && $(CC) $(TEST_C_FLAGS) -o $@ -c $<

$(OBJDIR)/tests/support/mock_agent_http.o: tests/support/mock_agent_http.c tests/support/mock_agent_http.h
	@mkdir -p $(dir $@) && $(CC) $(TEST_C_FLAGS) -o $@ -c $<

$(OBJDIR)/tests/support/ir_ingress_stubs.o: tests/support/ir_ingress_stubs.c
	@mkdir -p $(dir $@) && $(CC) $(TEST_C_FLAGS) -o $@ -c $<

$(OBJDIR)/tests/support/config_autonomy_stub.o: tests/support/config_autonomy_stub.c
	@mkdir -p $(dir $@) && $(CC) $(TEST_C_FLAGS) -o $@ -c $<

$(OBJDIR)/tests/support/router_advise_stub.o: tests/support/router_advise_stub.c
	@mkdir -p $(dir $@) && $(CC) $(TEST_C_FLAGS) -o $@ -c $<

$(OBJDIR)/tests/test_mcp_client_integration.o: C_FLAGS += -DMCP_MOCK_SERVER_PATH=\"$(TESTPREFIX)/mock-mcp-server\"
$(OBJDIR)/tests/test_mcp_client_registry.o: C_FLAGS += -DMCP_MOCK_SERVER_PATH=\"$(TESTPREFIX)/mock-mcp-server\"

$(TESTPREFIX)/mock-mcp-server: $(OBJDIR)/tests/support/mock_mcp_server.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-mcp-client-integration: $(OBJDIR)/tests/test_mcp_client_integration.o \
                     $(TEST_MCP_CLIENT_OBJS) \
                     $(TESTPREFIX)/mock-mcp-server
	$(TESTLINK) -o $@ $(OBJDIR)/tests/test_mcp_client_integration.o $(TEST_MCP_CLIENT_OBJS) $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-mcp-client-sse: $(OBJDIR)/tests/test_mcp_client_sse.o \
                     $(TEST_MCP_CLIENT_OBJS)
	$(TESTLINK) -o $@ $(OBJDIR)/tests/test_mcp_client_sse.o $(TEST_MCP_CLIENT_OBJS) $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-memory-provider: $(OBJDIR)/tests/test_memory_provider.o \
                     $(OBJDIR)/modules/memory/memory_provider.o \
                     $(OBJDIR)/log.o \
                     $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-plugin-c-hook: $(OBJDIR)/tests/test_plugin_c_hook.o \
                     $(OBJDIR)/modules/module-runtime/pre_llm_hook.o \
                     $(OBJDIR)/log.o \
                     $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)




$(TESTPREFIX)/unit-test-dogfood: $(OBJDIR)/tests/test_dogfood.o \
                     $(OBJDIR)/tests/support/learning_implicit_stub.o $(OBJDIR)/dogfood.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-working-profile: $(OBJDIR)/tests/test_working_profile.o \
                     $(OBJDIR)/working_profile.o \
                     $(OBJDIR)/db2/calibration.o \
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                     $(OBJDIR)/db2/feature_rows.o \
                     $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(DB1_TEST_OBJS) $(DB1_MIGRATED_OBJS) \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-curiosity: $(OBJDIR)/tests/test_curiosity.o $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# KB store-side memory-audit hook + end-to-end onto aimee-kb's obs_bus/ledger:
# memory_core_crud (db2 shim) fires the hook, the REAL KB bridge maps it, and the
# row lands in the ledger. Links the db2-shim memory set plus the bus stack.
$(TESTPREFIX)/unit-test-memory-audit-hook: $(OBJDIR)/tests/test_memory_audit_hook.o \
                                           $(OBJDIR)/kb/kb_memory_audit_bridge.o \
                                           $(OBS_BUS_LINK_OBJS) \
                                           $(OBJDIR)/modules/audit/audit_ledger.o \
                                           $(OBJDIR)/core/event_bus/bus_client.o \
                                           $(OBJDIR)/core/event_bus/bus_attach.o \
                                           $(OBJDIR)/core/event_bus/bus_host.o \
                                           $(OBJDIR)/core/event_bus/bus_route.o \
                                           $(OBJDIR)/core/event_bus/bus_region.o $(OBJDIR)/core/event_bus/bus_region_host.o \
                                           $(OBJDIR)/core/event_bus/bus_ring.o \
                                           $(OBJDIR)/core/event_bus/bus_arena.o \
                                           $(OBJDIR)/core/event_bus/bus_wire.o \
                                           $(OBJDIR)/core/event_bus/bus_capture.o \
                                           $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lpthread

# NOT in TEST_TARGETS: it links the bus objects, which the standard unit-tests
# build does not assemble — so the bench gate (check_bus_perf_gate.sh) force-builds
# and runs it via this .PHONY target, like the other bus tests.
.PHONY: unit-test-memory-audit-hook
unit-test-memory-audit-hook: $(TESTPREFIX)/unit-test-memory-audit-hook
	$<

$(TESTPREFIX)/unit-test-cmd-identity: $(OBJDIR)/tests/test_cmd_identity.o \
                     $(OBJDIR)/cmd_identity.o $(OBJDIR)/working_profile.o \
                     $(DB1_TEST_OBJS) $(DB1_MIGRATED_OBJS) \
                     $(OBJDIR)/cmd_util.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-session-briefing: $(OBJDIR)/tests/test_session_briefing.o \
                     $(OBJDIR)/session_briefing.o $(OBJDIR)/modules/skills/skill.o $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Pure helpers extracted from session_start_emit; the header is static-inline so
# only cJSON.o is needed at link time.
$(TESTPREFIX)/unit-test-session-start-util: $(OBJDIR)/tests/test_session_start_util.o \
                     $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Pure string helpers extracted from memory_assemble.c; header is static-inline
# so the test links nothing extra.
$(TESTPREFIX)/unit-test-memory-assemble-util: $(OBJDIR)/tests/test_memory_assemble_util.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-session-brief: $(OBJDIR)/tests/test_session_brief.o \
                     $(OBJDIR)/cmd_session_history.o $(OBJDIR)/cmd_util.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-learning-metrics: $(OBJDIR)/tests/test_learning_metrics.o \
                     $(OBJDIR)/modules/learning/learning_router.o $(OBJDIR)/modules/learning/learning_regret.o $(OBJDIR)/modules/learning/learning_implicit.o \
                     $(OBJDIR)/modules/learning/learning_signal_policy.o \
                     $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-curiosity-resolve: $(OBJDIR)/tests/test_curiosity_resolve.o \
                     $(OBJDIR)/curiosity_resolve.o \
                     $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-policy-arms: $(OBJDIR)/tests/test_policy_arms.o \
                     $(OBJDIR)/modules/learning/learning_policy_arms.o \
                     $(OBJDIR)/modules/learning/learning_approach_memory.o \
                     $(OBJDIR)/modules/learning/learning_endogeneity.o \
                     $(OBJDIR)/modules/db1/approach_failures.o $(OBJDIR)/approach_store.o \
                     $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-learning-attribution: $(OBJDIR)/tests/test_learning_attribution.o \
                     $(OBJDIR)/modules/learning/learning_attribution.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-approach-memory: $(OBJDIR)/tests/test_approach_memory.o \
                     $(OBJDIR)/modules/learning/learning_approach_memory.o $(OBJDIR)/modules/learning/learning_policy_arms.o \
                     $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-learning-regret: $(OBJDIR)/tests/test_learning_regret.o \
                     $(OBJDIR)/modules/learning/learning_router.o \
                     $(OBJDIR)/modules/learning/learning_implicit.o \
                     $(OBJDIR)/modules/learning/learning_signal_policy.o \
                     $(OBJDIR)/modules/learning/learning_regret.o \
                     $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-learning-eval-synthesis: $(OBJDIR)/tests/test_learning_eval_synthesis.o \
                     $(OBJDIR)/modules/learning/learning_eval_synthesis.o \
                     $(OBJDIR)/modules/learning/learning_endogeneity.o \
                     $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-eval-candidates: $(OBJDIR)/tests/test_eval_candidates.o \
                     $(OBJDIR)/eval_synthesis.o \
                     $(OBJDIR)/modules/learning/learning_eval_synthesis.o \
                     $(OBJDIR)/modules/learning/learning_endogeneity.o \
                     $(OBJDIR)/modules/db1/eval.o \
                     $(OBJDIR)/modules/db1/agent_jobs.o \
                     $(OBJDIR)/posix/agent_runtime_support.o \
                     $(OBJDIR)/modules/learning/learning_approach_memory.o $(OBJDIR)/modules/learning/learning_policy_arms.o \
                     $(OBJDIR)/modules/db1/approach_failures.o $(OBJDIR)/approach_store.o \
                     $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-memory-recall-pivot: $(OBJDIR)/tests/test_memory_recall_pivot.o \
                     $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-memory-filter: $(OBJDIR)/tests/test_memory_filter.o $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm

$(TESTPREFIX)/unit-test-memory-profiles: $(OBJDIR)/tests/test_memory_profiles.o \
                     $(OBJDIR)/modules/memory/memory_profile_pack.o \
                     $(OBJDIR)/aimee_home.o \
                     $(OBJDIR)/cJSON.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-wiki-render: $(OBJDIR)/tests/test_wiki_render.o \
                     $(OBJDIR)/wiki_render.o $(TEST_DATA_OBJS_MOCK) \
                     $(OBJDIR)/tests/support/kb_client_test_stub.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-integrity-gate: $(OBJDIR)/tests/test_integrity_gate.o \
                     $(OBJDIR)/integrity_gate.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-conversation-context: $(OBJDIR)/tests/test_conversation_context.o \
                     $(OBJDIR)/conversation_context.o \
                     $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o $(OBJDIR)/modules/db1/conv_context.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Manual-inspection harness for the virtual-context rollout (AC#2/AC#3). Drives
# the real record->flush->assemble->search->expand path with the flag on and
# prints reviewer evidence. Source lives under tests/eval/ (outside src/tests).
$(OBJDIR)/tests/inspect_real_session.o: ../tests/eval/agentic_context_virtualization/inspect_real_session.c
	@mkdir -p $(dir $@) && $(CC) $(TEST_C_FLAGS) -Imodules/db1 -c -o $@ $<

$(TESTPREFIX)/virtual-context-inspect: $(OBJDIR)/tests/inspect_real_session.o \
                     $(OBJDIR)/conversation_context.o \
                     $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o $(OBJDIR)/modules/db1/conv_context.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-payload-rewrite-state: $(OBJDIR)/tests/test_payload_rewrite_state.o \
                     $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o $(OBJDIR)/modules/db1/payload_rewrite_state.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-payload-rewrite: $(OBJDIR)/tests/test_payload_rewrite.o \
                     $(OBJDIR)/payload_rewrite.o \
                     $(OBJDIR)/modules/db1/db1_init.o \
                     $(OBJDIR)/modules/db1/payload_rewrite_state.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-scope: $(OBJDIR)/tests/test_kb_scope.o \
                     $(OBJDIR)/kb/kb_scope.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-identity: $(OBJDIR)/tests/test_kb_identity.o \
                     $(OBJDIR)/kb/kb_identity.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-identity-resolve: $(OBJDIR)/tests/test_kb_identity_resolve.o \
                     $(OBJDIR)/kb/kb_identity.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-ingress: $(OBJDIR)/tests/test_kb_ingress.o \
                     $(OBJDIR)/kb/http/kb_ingress.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-oidc-jwks: $(OBJDIR)/tests/test_kb_oidc_jwks.o \
                     $(OBJDIR)/kb/kb_oidc_jwks_fleet.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-db2-hardening: $(OBJDIR)/tests/test_db2_hardening.o \
                     $(OBJDIR)/db2/db2_hardening.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-tenancy-shim-guard: $(OBJDIR)/tests/test_kb_tenancy_shim_guard.o \
                     $(OBJDIR)/db2/team.o $(OBJDIR)/db2/project.o $(OBJDIR)/db2/membership.o \
                     $(OBJDIR)/db2/admin_grant.o $(OBJDIR)/db2/write_tier_grant.o \
                     $(OBJDIR)/db2/oidc_jwks.o \
                     $(OBJDIR)/db2/db2_tenant.o $(OBJDIR)/kb/kb_identity.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-route-acl: $(OBJDIR)/tests/test_kb_route_acl.o \
                     $(OBJDIR)/kb/http/kb_route_acl.o \
                     $(OBJDIR)/tests/module_handlers/control_web.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# P2a: pure route-admission validators (wire whitelist + name bound). No storage/PG
# dependency — the entitled-resolution + admin-gate are proven by the real-PG RLS gate.
$(TESTPREFIX)/unit-test-kb-models-validate: $(OBJDIR)/tests/test_kb_models_validate.o \
                     $(OBJDIR)/kb/http/kb_models_validate.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-p3b-spend: $(OBJDIR)/tests/test_p3b_spend.o \
                     $(OBJDIR)/kb/http/kb_insights_util.o $(OBJDIR)/cJSON.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-mcp-roundtable-contract: $(OBJDIR)/tests/test_mcp_roundtable_contract.o \
                                     $(OBJDIR)/modules/protocols/mcp/mcp_tools.o \
                                     $(OBJDIR)/cJSON.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-mcp-delegate-contract: $(OBJDIR)/tests/test_mcp_delegate_contract.o \
                                     $(OBJDIR)/modules/protocols/mcp/mcp_tools.o \
                                     $(OBJDIR)/cJSON.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-dense-vector-scope: $(OBJDIR)/tests/test_kb_dense_vector_scope.o \
                                     $(OBJDIR)/kb/kb.o $(KB_TEST_OBJS) $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-ingest-worker-cap: $(OBJDIR)/tests/test_kb_ingest_worker_cap.o \
                     $(OBJDIR)/kb/kb_ingest_workers.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-ingest-format: $(OBJDIR)/tests/test_kb_ingest_format.o \
                     $(OBJDIR)/kb/kb_ingest_normalize.o $(OBJDIR)/log.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-session-degraded-notice: $(OBJDIR)/tests/test_session_degraded_notice.o $(OBJDIR)/session_degraded_notice.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-kb-http-json: $(OBJDIR)/tests/test_kb_http_json.o $(OBJDIR)/kb/http/kb_http_json.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-kb-http-routes: $(OBJDIR)/tests/test_kb_http_routes.o $(OBJDIR)/kb/http/kb_http_json.o \
                     $(OBJDIR)/tests/support/corpus_jobs_http_stub.o \
                     $(OBJDIR)/tests/support/pdf_route_stubs.o \
                     $(OBJDIR)/kb/http/kb_http_grants.o \
                     $(OBJDIR)/kb/kb_identity_token.o \
                     $(OBJDIR)/tests/support/kb_http_route_stubs.o \
                     $(OBJDIR)/kb/http/kb_http.o $(OBJDIR)/command_registry.o \
                     $(OBJDIR)/kb/http/kb_http_content_policy.o \
                     $(OBJDIR)/kb/http/kb_http_entry.o \
                     $(OBJDIR)/kb/http/kb_http_listener.o \
                     $(OBJDIR)/kb/http/kb_http_conn.o \
                     $(OBJDIR)/tests/support/kb_ws_stub.o \
                     $(OBJDIR)/kb/http/kb_http_search.o \
                     $(OBJDIR)/kb/http/kb_route_acl.o \
                     $(OBJDIR)/tests/module_handlers/control_web.o \
                     $(OBJDIR)/kb/http/kb_http_console.o \
                     $(OBJDIR)/kb/http/kb_http_accounts.o $(OBJDIR)/kb/http/kb_http_bootstrap.o $(OBJDIR)/kb/http/kb_http_identity_login.o $(OBJDIR)/kb/kb_oidc_login.o $(OBJDIR)/kb/kb_oidc_login_store.o $(OBJDIR)/kb/kb_login_throttle.o \
                     $(OBJDIR)/kb/http/kb_http_governance.o \
                     $(OBJDIR)/util.o \
                     $(OBJDIR)/kb/kb_scope.o \
                     $(OBJDIR)/kb/verifier.o \
                     $(OBJDIR)/kb/enroll.o \
                     $(OBJDIR)/kb/pki.o \
                     $(OBJDIR)/kb/http/kb_tls.o \
                     $(OBJDIR)/kb/http/kb_tls_serve.o \
                     $(OBJDIR)/shared/auth_token_verify.o \
                     $(OBJDIR)/kb/kb_caller_token.o \
                     $(OBJDIR)/db2/management_jwks_runtime.o \
                     $(OBJDIR)/modules/kb_client/kb_client_mtls.o \
                     $(OBJDIR)/modules/vault/runtime_secret.o \
                     $(OBJDIR)/server/oauth_pkce.o \
                     $(OBJDIR)/shared/kb_paths.o \
                     $(OBJDIR)/aimee_home.o \
                     $(OBJDIR)/kb/kb_intel_payload.o \
                     $(OBJDIR)/kb/kb_bandit_registry.o \
                     $(OBJDIR)/kb/http/kb_http_code.o \
                     $(OBJDIR)/kb/http/kb_http_code_lifecycle.o \
                     $(OBJDIR)/kb/http/kb_http_code_context.o \
                     $(OBJDIR)/kb/http/kb_http_code_graphfb.o $(OBJDIR)/kb/lessons_reflect.o \
                                    $(OBJDIR)/kb/lessons_session_capture.o $(OBJDIR)/kb/lessons_cite_tracker.o \
                     $(OBJDIR)/rrf.o \
                     $(OBJDIR)/kb/kb_graph_analytics.o \
                     $(OBJDIR)/kb/prompt_sanitizer.o \
                     $(OBJDIR)/kb/http/kb_http_pdf.o \
                     $(OBJDIR)/kb/http/kb_http_jobs.o \
                     $(OBJDIR)/posix/td_search_render.o $(OBJDIR)/dstr.o \
                     $(OBJDIR)/kb/http/kb_http_team.o $(OBJDIR)/kb/http/kb_ingress.o \
                     $(OBJDIR)/kb/kb_reqctx.o $(OBJDIR)/kb/kb_identity.o \
                     $(OBJDIR)/kb/kb_identity_resolve.o \
                     $(OBJDIR)/kb/auth_oidc.o \
                     $(OBJDIR)/db2/db2_tenant.o $(OBJDIR)/db2/team.o \
                     $(OBJDIR)/db2/project.o $(OBJDIR)/db2/membership.o \
                     $(OBJDIR)/cJSON.o \
                     $(OBJDIR)/kb/modules/vault/vault_crypto.o \
                     $(OBJDIR)/kb/modules/vault/vault_server_key.o \
                     $(OBJDIR)/kb/modules/vault/vault_store.o $(OBJDIR)/kb/modules/vault/vault_kek_check.o \
                     $(OBJDIR)/kb/modules/vault/vault_kek_cache.o \
                     $(OBJDIR)/log.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-http-ingest: $(OBJDIR)/tests/test_kb_http_ingest.o \
                     $(OBJDIR)/kb/http/kb_http_ingest.o \
                     $(OBJDIR)/tests/support/kb_ws_stub.o \
                     $(OBJDIR)/kb/kb_doc_hash.o \
                     $(CONFIG_ACCESSOR_OBJS) \
                     $(OBJDIR)/cJSON.o \
                     $(OBJDIR)/log.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-releases: $(OBJDIR)/tests/test_kb_releases.o \
                     $(OBJDIR)/kb/http/kb_http_releases.o \
                     $(OBJDIR)/tests/support/kb_ws_stub.o \
                     $(OBJDIR)/log.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-sketch: $(OBJDIR)/tests/test_sketch.o \
                     $(OBJDIR)/sketch.o \
                     $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm

DB2_CJSON_TEST_FLAGS = -Imodules/db2/support

$(OBJDIR)/tests/test_db2_cjson_support.o: tests/test_db2_cjson_support.c \
                                           modules/db2/support/cJSON.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_CJSON_TEST_FLAGS) -c -o $@ $<

$(OBJDIR)/tests/db2_cjson_support_impl.o: modules/db2/support/cjson.c \
                                          modules/db2/support/cJSON.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_CJSON_TEST_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-cjson-support: \
                     $(OBJDIR)/tests/test_db2_cjson_support.o \
                     $(OBJDIR)/tests/db2_cjson_support_impl.o
	$(TESTLINK_MIN) -o $@ $^ $(TEST_L_FLAGS) -lm

DB2_CJSON_SANITIZE_DIR = $(OBJDIR)/tests/db2-cjson-support-sanitize

$(DB2_CJSON_SANITIZE_DIR)/test.o: tests/test_db2_cjson_support.c \
                                        modules/db2/support/cJSON.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_CJSON_TEST_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) \
	      -c -o $@ $<

$(DB2_CJSON_SANITIZE_DIR)/support.o: modules/db2/support/cjson.c \
                                           modules/db2/support/cJSON.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_CJSON_TEST_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) \
	      -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-cjson-support-sanitize: \
                     $(DB2_CJSON_SANITIZE_DIR)/test.o \
                     $(DB2_CJSON_SANITIZE_DIR)/support.o
	$(CC) $(DB2_SUPPORT_SANITIZE_FLAGS) -o $@ $^ -lm

.PHONY: unit-test-db2-cjson-support unit-test-db2-cjson-support-sanitize
unit-test-db2-cjson-support: $(TESTPREFIX)/unit-test-db2-cjson-support
	$<

unit-test-db2-cjson-support-sanitize: $(TESTPREFIX)/unit-test-db2-cjson-support-sanitize
	$<

DB2_RANDOM_TEST_FLAGS = -Imodules/db2/support -Iheaders -pthread
DB2_RANDOM_SUPPORT_RENAMES = \
   -Dplatform_random_bytes=db2_support_platform_random_bytes \
   -Dplatform_random_hex=db2_support_platform_random_hex
DB2_RANDOM_LEGACY_RENAMES = \
   -Dplatform_random_bytes=legacy_platform_random_bytes \
   -Dplatform_random_hex=legacy_platform_random_hex
DB2_RANDOM_IO_SEAMS = -include tests/support/db2_random_io_seam.h

$(OBJDIR)/tests/test_db2_random_support.o: tests/test_db2_random_support.c \
                                             modules/db2/support/db2_random.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_RANDOM_TEST_FLAGS) -c -o $@ $<

$(OBJDIR)/tests/db2_random_support_real.o: modules/db2/support/random_primitives.c \
                                               modules/db2/support/db2_random.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_RANDOM_TEST_FLAGS) -c -o $@ $<

$(OBJDIR)/tests/db2_random_support_impl.o: modules/db2/support/random_primitives.c \
                                               modules/db2/support/db2_random.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_RANDOM_TEST_FLAGS) $(DB2_RANDOM_SUPPORT_RENAMES) \
	      $(DB2_RANDOM_IO_SEAMS) -c -o $@ $<

$(OBJDIR)/tests/db2_random_legacy_common.o: platform_random.c headers/platform_random.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_RANDOM_TEST_FLAGS) $(DB2_RANDOM_LEGACY_RENAMES) \
	      -c -o $@ $<

$(OBJDIR)/tests/db2_random_legacy_platform.o: posix/platform_random.c headers/platform_random.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_RANDOM_TEST_FLAGS) $(DB2_RANDOM_LEGACY_RENAMES) \
	      $(DB2_RANDOM_IO_SEAMS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-random-support: \
                     $(OBJDIR)/tests/test_db2_random_support.o \
                     $(OBJDIR)/tests/db2_random_support_real.o \
                     $(OBJDIR)/tests/db2_random_support_impl.o \
                     $(OBJDIR)/tests/db2_random_legacy_common.o \
                     $(OBJDIR)/tests/db2_random_legacy_platform.o
	$(TESTLINK_MIN) -o $@ $^ $(TEST_L_FLAGS) -pthread

DB2_RANDOM_SANITIZE_DIR = $(OBJDIR)/tests/db2-random-support-sanitize

$(DB2_RANDOM_SANITIZE_DIR)/test.o: tests/test_db2_random_support.c \
                                          modules/db2/support/db2_random.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_RANDOM_TEST_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) \
	      -c -o $@ $<

$(DB2_RANDOM_SANITIZE_DIR)/real.o: modules/db2/support/random_primitives.c \
                                          modules/db2/support/db2_random.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_RANDOM_TEST_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) \
	      -c -o $@ $<

$(DB2_RANDOM_SANITIZE_DIR)/support.o: modules/db2/support/random_primitives.c \
                                             modules/db2/support/db2_random.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_RANDOM_TEST_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) \
	      $(DB2_RANDOM_SUPPORT_RENAMES) $(DB2_RANDOM_IO_SEAMS) -c -o $@ $<

$(DB2_RANDOM_SANITIZE_DIR)/legacy-common.o: platform_random.c headers/platform_random.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_RANDOM_TEST_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) \
	      $(DB2_RANDOM_LEGACY_RENAMES) -c -o $@ $<

$(DB2_RANDOM_SANITIZE_DIR)/legacy-platform.o: posix/platform_random.c \
                                                 headers/platform_random.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_RANDOM_TEST_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) \
	      $(DB2_RANDOM_LEGACY_RENAMES) $(DB2_RANDOM_IO_SEAMS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-random-support-sanitize: \
                     $(DB2_RANDOM_SANITIZE_DIR)/test.o \
                     $(DB2_RANDOM_SANITIZE_DIR)/real.o \
                     $(DB2_RANDOM_SANITIZE_DIR)/support.o \
                     $(DB2_RANDOM_SANITIZE_DIR)/legacy-common.o \
                     $(DB2_RANDOM_SANITIZE_DIR)/legacy-platform.o
	$(CC) $(DB2_SUPPORT_SANITIZE_FLAGS) -o $@ $^ -pthread

.PHONY: unit-test-db2-random-support unit-test-db2-random-support-sanitize
unit-test-db2-random-support: $(TESTPREFIX)/unit-test-db2-random-support
	$<

unit-test-db2-random-support-sanitize: $(TESTPREFIX)/unit-test-db2-random-support-sanitize
	$<

DB2_CERT_SERIAL_SUPPORT_RENAMES = -DAIMEE_DB2_CERT_SERIAL_PREFIX
DB2_CERT_SERIAL_SECTION_FLAGS = -ffunction-sections -fdata-sections

$(OBJDIR)/tests/db2_cert_serial_support_impl.o: \
                     modules/db2/support/cert_serial_primitives.c \
                     modules/db2/support/db2_cert_serial.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_CERT_SERIAL_SUPPORT_RENAMES) \
	      $(DB2_CERT_SERIAL_SECTION_FLAGS) -c -o $@ $<

$(OBJDIR)/tests/db2_cert_serial_monolith.o: kb/kb_identity.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_CERT_SERIAL_SECTION_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-cert-serial-support: \
                     $(OBJDIR)/tests/test_db2_cert_serial_support.o \
                     $(OBJDIR)/tests/db2_cert_serial_support_impl.o \
                     $(OBJDIR)/tests/db2_cert_serial_monolith.o
	$(TESTLINK_MIN) -Wl,--gc-sections -o $@ $^ $(TEST_L_FLAGS)

DB2_CERT_SERIAL_SANITIZE_DIR = $(OBJDIR)/tests/db2-cert-serial-support-sanitize

$(DB2_CERT_SERIAL_SANITIZE_DIR)/test.o: tests/test_db2_cert_serial_support.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_CERT_SERIAL_SANITIZE_DIR)/support.o: \
                     modules/db2/support/cert_serial_primitives.c \
                     modules/db2/support/db2_cert_serial.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_CERT_SERIAL_SUPPORT_RENAMES) \
	      $(DB2_CERT_SERIAL_SECTION_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_CERT_SERIAL_SANITIZE_DIR)/monolith.o: kb/kb_identity.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_CERT_SERIAL_SECTION_FLAGS) \
	      $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-cert-serial-support-sanitize: \
                     $(DB2_CERT_SERIAL_SANITIZE_DIR)/test.o \
                     $(DB2_CERT_SERIAL_SANITIZE_DIR)/support.o \
                     $(DB2_CERT_SERIAL_SANITIZE_DIR)/monolith.o
	$(CC) $(DB2_SUPPORT_SANITIZE_FLAGS) -Wl,--gc-sections -o $@ $^

.PHONY: unit-test-db2-cert-serial-support unit-test-db2-cert-serial-support-sanitize
unit-test-db2-cert-serial-support: $(TESTPREFIX)/unit-test-db2-cert-serial-support
	$<

unit-test-db2-cert-serial-support-sanitize: \
                     $(TESTPREFIX)/unit-test-db2-cert-serial-support-sanitize
	$<

DB2_COCHANGE_SUPPORT_RENAMES = -DAIMEE_DB2_COCHANGE_PREFIX
DB2_COCHANGE_SECTION_FLAGS = -ffunction-sections -fdata-sections

$(OBJDIR)/tests/db2_cochange_support_impl.o: \
                     modules/db2/support/cochange_primitives.c \
                     modules/db2/support/db2_cochange.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_COCHANGE_SUPPORT_RENAMES) \
	      $(DB2_COCHANGE_SECTION_FLAGS) -c -o $@ $<

$(OBJDIR)/tests/db2_cochange_monolith.o: cochange.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_COCHANGE_SECTION_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-cochange-support: \
                     $(OBJDIR)/tests/test_db2_cochange_support.o \
                     $(OBJDIR)/tests/db2_cochange_support_impl.o \
                     $(OBJDIR)/tests/db2_cochange_monolith.o
	$(TESTLINK_MIN) -Wl,--gc-sections -o $@ $^ $(TEST_L_FLAGS)

DB2_COCHANGE_SANITIZE_DIR = $(OBJDIR)/tests/db2-cochange-support-sanitize

$(DB2_COCHANGE_SANITIZE_DIR)/test.o: tests/test_db2_cochange_support.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_COCHANGE_SANITIZE_DIR)/support.o: \
                     modules/db2/support/cochange_primitives.c \
                     modules/db2/support/db2_cochange.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_COCHANGE_SUPPORT_RENAMES) \
	      $(DB2_COCHANGE_SECTION_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_COCHANGE_SANITIZE_DIR)/monolith.o: cochange.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_COCHANGE_SECTION_FLAGS) \
	      $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-cochange-support-sanitize: \
                     $(DB2_COCHANGE_SANITIZE_DIR)/test.o \
                     $(DB2_COCHANGE_SANITIZE_DIR)/support.o \
                     $(DB2_COCHANGE_SANITIZE_DIR)/monolith.o
	$(CC) $(DB2_SUPPORT_SANITIZE_FLAGS) -Wl,--gc-sections -o $@ $^

.PHONY: unit-test-db2-cochange-support unit-test-db2-cochange-support-sanitize
unit-test-db2-cochange-support: $(TESTPREFIX)/unit-test-db2-cochange-support
	$<

unit-test-db2-cochange-support-sanitize: \
                     $(TESTPREFIX)/unit-test-db2-cochange-support-sanitize
	$<

DB2_CODE_MATCH_SUPPORT_RENAMES = -DAIMEE_DB2_CODE_MATCH_PREFIX
DB2_CODE_MATCH_SECTION_FLAGS = -ffunction-sections -fdata-sections

$(OBJDIR)/tests/db2_code_match_support_impl.o: \
                     modules/db2/support/code_match_primitives.c \
                     modules/db2/support/db2_code_match.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_CODE_MATCH_SUPPORT_RENAMES) \
	      $(DB2_CODE_MATCH_SECTION_FLAGS) -c -o $@ $<

$(OBJDIR)/tests/db2_code_match_monolith.o: code_match.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_CODE_MATCH_SECTION_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-code-match-support: \
                     $(OBJDIR)/tests/test_db2_code_match_support.o \
                     $(OBJDIR)/tests/db2_code_match_support_impl.o \
                     $(OBJDIR)/tests/db2_code_match_monolith.o
	$(TESTLINK_MIN) -Wl,--gc-sections -o $@ $^ $(TEST_L_FLAGS)

DB2_CODE_MATCH_SANITIZE_DIR = $(OBJDIR)/tests/db2-code-match-support-sanitize

$(DB2_CODE_MATCH_SANITIZE_DIR)/test.o: tests/test_db2_code_match_support.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_CODE_MATCH_SANITIZE_DIR)/support.o: \
                     modules/db2/support/code_match_primitives.c \
                     modules/db2/support/db2_code_match.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_CODE_MATCH_SUPPORT_RENAMES) \
	      $(DB2_CODE_MATCH_SECTION_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_CODE_MATCH_SANITIZE_DIR)/monolith.o: code_match.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_CODE_MATCH_SECTION_FLAGS) \
	      $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-code-match-support-sanitize: \
                     $(DB2_CODE_MATCH_SANITIZE_DIR)/test.o \
                     $(DB2_CODE_MATCH_SANITIZE_DIR)/support.o \
                     $(DB2_CODE_MATCH_SANITIZE_DIR)/monolith.o
	$(CC) $(DB2_SUPPORT_SANITIZE_FLAGS) -Wl,--gc-sections -o $@ $^

.PHONY: unit-test-db2-code-match-support unit-test-db2-code-match-support-sanitize
unit-test-db2-code-match-support: $(TESTPREFIX)/unit-test-db2-code-match-support
	$<

unit-test-db2-code-match-support-sanitize: \
                     $(TESTPREFIX)/unit-test-db2-code-match-support-sanitize
	$<

DB2_DSTR_SUPPORT_RENAMES = \
   -Ddstr_appendf=db2_support_dstr_appendf \
   -Ddstr_init=db2_support_dstr_init \
   -Ddstr_steal=db2_support_dstr_steal
DB2_DSTR_TEST_ALLOCATOR = -Drealloc=db2_test_realloc

$(OBJDIR)/tests/db2_dstr_support_impl.o: modules/db2/support/dstr_primitives.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_DSTR_SUPPORT_RENAMES) \
	      $(DB2_DSTR_TEST_ALLOCATOR) -c -o $@ $<

$(OBJDIR)/tests/db2_dstr_monolith.o: dstr.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_DSTR_TEST_ALLOCATOR) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-dstr-support: \
                     $(OBJDIR)/tests/test_db2_dstr_support.o \
                     $(OBJDIR)/tests/db2_dstr_support_impl.o \
                     $(OBJDIR)/tests/db2_dstr_monolith.o
	$(TESTLINK_MIN) -o $@ $^ $(TEST_L_FLAGS)

DB2_DSTR_SANITIZE_DIR = $(OBJDIR)/tests/db2-dstr-support-sanitize

$(DB2_DSTR_SANITIZE_DIR)/test.o: tests/test_db2_dstr_support.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_DSTR_SANITIZE_DIR)/support.o: modules/db2/support/dstr_primitives.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_DSTR_SUPPORT_RENAMES) \
	      $(DB2_DSTR_TEST_ALLOCATOR) $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_DSTR_SANITIZE_DIR)/monolith.o: dstr.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_DSTR_TEST_ALLOCATOR) \
	      $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-dstr-support-sanitize: \
                     $(DB2_DSTR_SANITIZE_DIR)/test.o \
                     $(DB2_DSTR_SANITIZE_DIR)/support.o \
                     $(DB2_DSTR_SANITIZE_DIR)/monolith.o
	$(CC) $(DB2_SUPPORT_SANITIZE_FLAGS) -o $@ $^

.PHONY: unit-test-db2-dstr-support unit-test-db2-dstr-support-sanitize
unit-test-db2-dstr-support: $(TESTPREFIX)/unit-test-db2-dstr-support
	$<

unit-test-db2-dstr-support-sanitize: $(TESTPREFIX)/unit-test-db2-dstr-support-sanitize
	$<

DB2_MGMT_READ_SUPPORT_RENAMES = \
   -Dserver_mgmt_read_selector_name=db2_support_server_mgmt_read_selector_name
DB2_MGMT_READ_SECTION_FLAGS = -ffunction-sections -fdata-sections

$(OBJDIR)/tests/db2_management_read_support_impl.o: \
                     modules/db2/support/management_read_primitives.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_MGMT_READ_SUPPORT_RENAMES) \
	      $(DB2_MGMT_READ_SECTION_FLAGS) -c -o $@ $<

$(OBJDIR)/tests/db2_management_read_monolith.o: shared/management_read.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_MGMT_READ_SECTION_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-management-read-support: \
                     $(OBJDIR)/tests/test_db2_management_read_support.o \
                     $(OBJDIR)/tests/db2_management_read_support_impl.o \
                     $(OBJDIR)/tests/db2_management_read_monolith.o
	$(TESTLINK_MIN) -Wl,--gc-sections -o $@ $^ $(TEST_L_FLAGS)

DB2_MGMT_READ_SANITIZE_DIR = $(OBJDIR)/tests/db2-management-read-support-sanitize

$(DB2_MGMT_READ_SANITIZE_DIR)/test.o: tests/test_db2_management_read_support.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_MGMT_READ_SANITIZE_DIR)/support.o: \
                     modules/db2/support/management_read_primitives.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_MGMT_READ_SUPPORT_RENAMES) \
	      $(DB2_MGMT_READ_SECTION_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_MGMT_READ_SANITIZE_DIR)/monolith.o: shared/management_read.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_MGMT_READ_SECTION_FLAGS) \
	      $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-management-read-support-sanitize: \
                     $(DB2_MGMT_READ_SANITIZE_DIR)/test.o \
                     $(DB2_MGMT_READ_SANITIZE_DIR)/support.o \
                     $(DB2_MGMT_READ_SANITIZE_DIR)/monolith.o
	$(CC) $(DB2_SUPPORT_SANITIZE_FLAGS) -Wl,--gc-sections -o $@ $^

.PHONY: unit-test-db2-management-read-support \
        unit-test-db2-management-read-support-sanitize
unit-test-db2-management-read-support: $(TESTPREFIX)/unit-test-db2-management-read-support
	$<

unit-test-db2-management-read-support-sanitize: \
                     $(TESTPREFIX)/unit-test-db2-management-read-support-sanitize
	$<

DB2_MODEL_VALIDATION_SUPPORT_RENAMES = -DAIMEE_DB2_MODEL_VALIDATION_PREFIX
DB2_MODEL_VALIDATION_SECTION_FLAGS = -ffunction-sections -fdata-sections

$(OBJDIR)/tests/db2_model_validation_support_impl.o: \
                     modules/db2/support/model_validation_primitives.c \
                     modules/db2/support/db2_model_validation.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_MODEL_VALIDATION_SUPPORT_RENAMES) \
	      $(DB2_MODEL_VALIDATION_SECTION_FLAGS) -c -o $@ $<

$(OBJDIR)/tests/db2_model_validation_monolith.o: kb/http/kb_models_validate.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_MODEL_VALIDATION_SECTION_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-model-validation-support: \
                     $(OBJDIR)/tests/test_db2_model_validation_support.o \
                     $(OBJDIR)/tests/db2_model_validation_support_impl.o \
                     $(OBJDIR)/tests/db2_model_validation_monolith.o
	$(TESTLINK_MIN) -Wl,--gc-sections -o $@ $^ $(TEST_L_FLAGS)

DB2_MODEL_VALIDATION_SANITIZE_DIR = $(OBJDIR)/tests/db2-model-validation-support-sanitize

$(DB2_MODEL_VALIDATION_SANITIZE_DIR)/test.o: tests/test_db2_model_validation_support.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_MODEL_VALIDATION_SANITIZE_DIR)/support.o: \
                     modules/db2/support/model_validation_primitives.c \
                     modules/db2/support/db2_model_validation.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_MODEL_VALIDATION_SUPPORT_RENAMES) \
	      $(DB2_MODEL_VALIDATION_SECTION_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_MODEL_VALIDATION_SANITIZE_DIR)/monolith.o: kb/http/kb_models_validate.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_MODEL_VALIDATION_SECTION_FLAGS) \
	      $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-model-validation-support-sanitize: \
                     $(DB2_MODEL_VALIDATION_SANITIZE_DIR)/test.o \
                     $(DB2_MODEL_VALIDATION_SANITIZE_DIR)/support.o \
                     $(DB2_MODEL_VALIDATION_SANITIZE_DIR)/monolith.o
	$(CC) $(DB2_SUPPORT_SANITIZE_FLAGS) -Wl,--gc-sections -o $@ $^

.PHONY: unit-test-db2-model-validation-support \
        unit-test-db2-model-validation-support-sanitize
unit-test-db2-model-validation-support: \
                     $(TESTPREFIX)/unit-test-db2-model-validation-support
	$<

unit-test-db2-model-validation-support-sanitize: \
                     $(TESTPREFIX)/unit-test-db2-model-validation-support-sanitize
	$<

DB2_TEXT_SUPPORT_RENAMES = -Dtext_sanitize_utf8=db2_support_text_sanitize_utf8
DB2_TEXT_SECTION_FLAGS = -ffunction-sections -fdata-sections

$(OBJDIR)/tests/db2_text_support_impl.o: modules/db2/support/text_primitives.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_TEXT_SUPPORT_RENAMES) \
	      $(DB2_TEXT_SECTION_FLAGS) -c -o $@ $<

$(OBJDIR)/tests/db2_text_monolith.o: text.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_TEXT_SECTION_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-text-support: \
                     $(OBJDIR)/tests/test_db2_text_support.o \
                     $(OBJDIR)/tests/db2_text_support_impl.o \
                     $(OBJDIR)/tests/db2_text_monolith.o
	$(TESTLINK_MIN) -Wl,--gc-sections -o $@ $^ $(TEST_L_FLAGS)

DB2_TEXT_SANITIZE_DIR = $(OBJDIR)/tests/db2-text-support-sanitize

$(DB2_TEXT_SANITIZE_DIR)/test.o: tests/test_db2_text_support.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_TEXT_SANITIZE_DIR)/support.o: modules/db2/support/text_primitives.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_TEXT_SUPPORT_RENAMES) $(DB2_TEXT_SECTION_FLAGS) \
	      $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_TEXT_SANITIZE_DIR)/monolith.o: text.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_TEXT_SECTION_FLAGS) \
	      $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-text-support-sanitize: \
                     $(DB2_TEXT_SANITIZE_DIR)/test.o \
                     $(DB2_TEXT_SANITIZE_DIR)/support.o \
                     $(DB2_TEXT_SANITIZE_DIR)/monolith.o
	$(CC) $(DB2_SUPPORT_SANITIZE_FLAGS) -Wl,--gc-sections -o $@ $^

.PHONY: unit-test-db2-text-support unit-test-db2-text-support-sanitize
unit-test-db2-text-support: $(TESTPREFIX)/unit-test-db2-text-support
	$<

unit-test-db2-text-support-sanitize: $(TESTPREFIX)/unit-test-db2-text-support-sanitize
	$<

DB2_EXTRACTOR_SECTION_FLAGS = -ffunction-sections -fdata-sections

$(OBJDIR)/tests/db2_extractor_support_impl.o: modules/db2/support/extractor_primitives.c \
                                            modules/db2/support/db2_extractors.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) -DAIMEE_DB2_EXTRACTOR_PREFIX $(DB2_EXTRACTOR_SECTION_FLAGS) \
	      -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-extractor-support: \
                     $(OBJDIR)/tests/test_db2_extractor_support.o \
                     $(OBJDIR)/tests/db2_extractor_support_impl.o \
                     $(OBJDIR)/extractors.o $(OBJDIR)/extractors_extra.o \
                     $(OBJDIR)/extractors_new_langs.o $(OBJDIR)/code_treesitter.o
	$(TESTLINK_MIN) -Wl,--gc-sections -o $@ $^ $(TEST_L_FLAGS)

DB2_EXTRACTOR_SANITIZE_DIR = $(OBJDIR)/tests/db2-extractor-support-sanitize

$(DB2_EXTRACTOR_SANITIZE_DIR)/test.o: tests/test_db2_extractor_support.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_EXTRACTOR_SANITIZE_DIR)/support.o: modules/db2/support/extractor_primitives.c \
                                               modules/db2/support/db2_extractors.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) -DAIMEE_DB2_EXTRACTOR_PREFIX $(DB2_EXTRACTOR_SECTION_FLAGS) \
	      $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_EXTRACTOR_SANITIZE_DIR)/extractors.o: extractors.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_EXTRACTOR_SECTION_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) \
	      -c -o $@ $<

$(DB2_EXTRACTOR_SANITIZE_DIR)/extra.o: extractors_extra.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_EXTRACTOR_SECTION_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) \
	      -c -o $@ $<

$(DB2_EXTRACTOR_SANITIZE_DIR)/new-langs.o: extractors_new_langs.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_EXTRACTOR_SECTION_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) \
	      -c -o $@ $<

$(DB2_EXTRACTOR_SANITIZE_DIR)/tree-sitter.o: code_treesitter.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_EXTRACTOR_SECTION_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) \
	      -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-extractor-support-sanitize: \
                     $(DB2_EXTRACTOR_SANITIZE_DIR)/test.o \
                     $(DB2_EXTRACTOR_SANITIZE_DIR)/support.o \
                     $(DB2_EXTRACTOR_SANITIZE_DIR)/extractors.o \
                     $(DB2_EXTRACTOR_SANITIZE_DIR)/extra.o \
                     $(DB2_EXTRACTOR_SANITIZE_DIR)/new-langs.o \
                     $(DB2_EXTRACTOR_SANITIZE_DIR)/tree-sitter.o
	$(CC) $(DB2_SUPPORT_SANITIZE_FLAGS) -Wl,--gc-sections -o $@ $^

.PHONY: unit-test-db2-extractor-support unit-test-db2-extractor-support-sanitize
unit-test-db2-extractor-support: $(TESTPREFIX)/unit-test-db2-extractor-support
	$<

unit-test-db2-extractor-support-sanitize: \
                     $(TESTPREFIX)/unit-test-db2-extractor-support-sanitize
	$<

DB2_CODE_IMPORT_SUPPORT_RENAMES = \
   -Dcode_path_import_identity=db2_support_code_path_import_identity \
   -Dcode_import_identity=db2_support_code_import_identity \
   -Dcode_import_resolves_path=db2_support_code_import_resolves_path
DB2_CODE_IMPORT_SECTION_FLAGS = -ffunction-sections -fdata-sections

$(OBJDIR)/tests/db2_code_import_support_impl.o: \
                     modules/db2/support/code_import_primitives.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_CODE_IMPORT_SUPPORT_RENAMES) \
	      $(DB2_CODE_IMPORT_SECTION_FLAGS) -c -o $@ $<

$(OBJDIR)/tests/db2_code_import_monolith.o: extractors.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_CODE_IMPORT_SECTION_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-code-import-support: \
                     $(OBJDIR)/tests/test_db2_code_import_support.o \
                     $(OBJDIR)/tests/db2_code_import_support_impl.o \
                     $(OBJDIR)/tests/db2_code_import_monolith.o
	$(CC) -Wl,--gc-sections -o $@ $^

DB2_CODE_IMPORT_SANITIZE_DIR = $(OBJDIR)/tests/db2-code-import-support-sanitize

$(DB2_CODE_IMPORT_SANITIZE_DIR)/test.o: tests/test_db2_code_import_support.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_CODE_IMPORT_SANITIZE_DIR)/support.o: \
                     modules/db2/support/code_import_primitives.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_CODE_IMPORT_SUPPORT_RENAMES) \
	      $(DB2_CODE_IMPORT_SECTION_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_CODE_IMPORT_SANITIZE_DIR)/monolith.o: extractors.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_CODE_IMPORT_SECTION_FLAGS) \
	      $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-code-import-support-sanitize: \
                     $(DB2_CODE_IMPORT_SANITIZE_DIR)/test.o \
                     $(DB2_CODE_IMPORT_SANITIZE_DIR)/support.o \
                     $(DB2_CODE_IMPORT_SANITIZE_DIR)/monolith.o
	$(CC) $(DB2_SUPPORT_SANITIZE_FLAGS) -Wl,--gc-sections -o $@ $^

.PHONY: unit-test-db2-code-import-support unit-test-db2-code-import-support-sanitize
unit-test-db2-code-import-support: $(TESTPREFIX)/unit-test-db2-code-import-support
	$<

unit-test-db2-code-import-support-sanitize: \
                     $(TESTPREFIX)/unit-test-db2-code-import-support-sanitize
	$<

DB2_CODE_AUDIT_GRAPH_SUPPORT_RENAMES = \
   -Dcode_audit_dead_exports=db2_support_code_audit_dead_exports \
   -Dcode_audit_find_cycles=db2_support_code_audit_find_cycles
DB2_CODE_AUDIT_GRAPH_SECTION_FLAGS = -ffunction-sections -fdata-sections

$(OBJDIR)/tests/db2_code_audit_graph_support_impl.o: \
                     modules/db2/support/code_audit_graph_primitives.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_CODE_AUDIT_GRAPH_SUPPORT_RENAMES) \
	      $(DB2_CODE_AUDIT_GRAPH_SECTION_FLAGS) -c -o $@ $<

$(OBJDIR)/tests/db2_code_audit_graph_monolith.o: code_audit_graph.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_CODE_AUDIT_GRAPH_SECTION_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-code-audit-graph-support: \
                     $(OBJDIR)/tests/test_db2_code_audit_graph_support.o \
                     $(OBJDIR)/tests/db2_code_audit_graph_support_impl.o \
                     $(OBJDIR)/tests/db2_code_audit_graph_monolith.o
	$(CC) -Wl,--gc-sections -o $@ $^

DB2_CODE_AUDIT_GRAPH_SANITIZE_DIR = \
                     $(OBJDIR)/tests/db2-code-audit-graph-support-sanitize

$(DB2_CODE_AUDIT_GRAPH_SANITIZE_DIR)/test.o: \
                     tests/test_db2_code_audit_graph_support.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_CODE_AUDIT_GRAPH_SANITIZE_DIR)/support.o: \
                     modules/db2/support/code_audit_graph_primitives.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_CODE_AUDIT_GRAPH_SUPPORT_RENAMES) \
	      $(DB2_CODE_AUDIT_GRAPH_SECTION_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_CODE_AUDIT_GRAPH_SANITIZE_DIR)/monolith.o: code_audit_graph.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_CODE_AUDIT_GRAPH_SECTION_FLAGS) \
	      $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-code-audit-graph-support-sanitize: \
                     $(DB2_CODE_AUDIT_GRAPH_SANITIZE_DIR)/test.o \
                     $(DB2_CODE_AUDIT_GRAPH_SANITIZE_DIR)/support.o \
                     $(DB2_CODE_AUDIT_GRAPH_SANITIZE_DIR)/monolith.o
	$(CC) $(DB2_SUPPORT_SANITIZE_FLAGS) -Wl,--gc-sections -o $@ $^

.PHONY: unit-test-db2-code-audit-graph-support \
        unit-test-db2-code-audit-graph-support-sanitize
unit-test-db2-code-audit-graph-support: \
                     $(TESTPREFIX)/unit-test-db2-code-audit-graph-support
	$<

unit-test-db2-code-audit-graph-support-sanitize: \
                     $(TESTPREFIX)/unit-test-db2-code-audit-graph-support-sanitize
	$<

DB2_TIME_SUPPORT_RENAMES = \
   -Dnow_utc=db2_support_now_utc \
   -Dparse_utc_ts=db2_support_parse_utc_ts
DB2_TIME_SECTION_FLAGS = -ffunction-sections -fdata-sections

$(OBJDIR)/tests/db2_time_support_impl.o: modules/db2/support/time_primitives.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_TIME_SUPPORT_RENAMES) \
	      $(DB2_TIME_SECTION_FLAGS) -c -o $@ $<

$(OBJDIR)/tests/db2_time_monolith.o: util.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_TIME_SECTION_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-time-support: \
                     $(OBJDIR)/tests/test_db2_time_support.o \
                     $(OBJDIR)/tests/db2_time_support_impl.o \
                     $(OBJDIR)/tests/db2_time_monolith.o
	$(TESTLINK_MIN) -Wl,--gc-sections -o $@ $^ $(TEST_L_FLAGS)

DB2_TIME_SANITIZE_DIR = $(OBJDIR)/tests/db2-time-support-sanitize

$(DB2_TIME_SANITIZE_DIR)/test.o: tests/test_db2_time_support.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_TIME_SANITIZE_DIR)/support.o: modules/db2/support/time_primitives.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_TIME_SUPPORT_RENAMES) $(DB2_TIME_SECTION_FLAGS) \
	      $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_TIME_SANITIZE_DIR)/monolith.o: util.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_TIME_SECTION_FLAGS) \
	      $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-time-support-sanitize: \
                     $(DB2_TIME_SANITIZE_DIR)/test.o \
                     $(DB2_TIME_SANITIZE_DIR)/support.o \
                     $(DB2_TIME_SANITIZE_DIR)/monolith.o
	$(CC) $(DB2_SUPPORT_SANITIZE_FLAGS) -Wl,--gc-sections -o $@ $^

.PHONY: unit-test-db2-time-support unit-test-db2-time-support-sanitize
unit-test-db2-time-support: $(TESTPREFIX)/unit-test-db2-time-support
	$<

unit-test-db2-time-support-sanitize: $(TESTPREFIX)/unit-test-db2-time-support-sanitize
	$<

DB2_REL_ENUM_TEXT_SUPPORT_RENAMES = \
   -Dcorrection_behavior_to_text=db2_support_correction_behavior_to_text \
   -Drel_sensitivity_to_text=db2_support_rel_sensitivity_to_text

$(OBJDIR)/tests/db2_rel_enum_text_support_impl.o: modules/db2/support/rel_enum_text_primitives.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_REL_ENUM_TEXT_SUPPORT_RENAMES) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-rel-enum-text-support: \
                     $(OBJDIR)/tests/test_db2_rel_enum_text_support.o \
                     $(OBJDIR)/tests/db2_rel_enum_text_support_impl.o \
                     $(OBJDIR)/tests/db2_rel_type_monolith.o
	$(TESTLINK_MIN) -Wl,--gc-sections -o $@ $^ $(TEST_L_FLAGS)

DB2_REL_ENUM_TEXT_SANITIZE_DIR = $(OBJDIR)/tests/db2-rel-enum-text-support-sanitize

$(DB2_REL_ENUM_TEXT_SANITIZE_DIR)/test.o: tests/test_db2_rel_enum_text_support.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_REL_ENUM_TEXT_SANITIZE_DIR)/support.o: modules/db2/support/rel_enum_text_primitives.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_REL_ENUM_TEXT_SUPPORT_RENAMES) \
	      $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-rel-enum-text-support-sanitize: \
                     $(DB2_REL_ENUM_TEXT_SANITIZE_DIR)/test.o \
                     $(DB2_REL_ENUM_TEXT_SANITIZE_DIR)/support.o \
                     $(OBJDIR)/tests/db2-rel-type-support-sanitize/monolith.o
	$(CC) $(DB2_SUPPORT_SANITIZE_FLAGS) -Wl,--gc-sections -o $@ $^

.PHONY: unit-test-db2-rel-enum-text-support unit-test-db2-rel-enum-text-support-sanitize
unit-test-db2-rel-enum-text-support: $(TESTPREFIX)/unit-test-db2-rel-enum-text-support
	$<

unit-test-db2-rel-enum-text-support-sanitize: \
      $(TESTPREFIX)/unit-test-db2-rel-enum-text-support-sanitize
	$<

DB2_NODE_KIND_TEXT_SUPPORT_RENAMES = \
   -Dmemory_ontology_node_kind_to_text=db2_support_memory_ontology_node_kind_to_text

$(OBJDIR)/tests/db2_node_kind_text_support_impl.o: \
                     modules/db2/support/node_kind_text_primitives.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_NODE_KIND_TEXT_SUPPORT_RENAMES) -c -o $@ $<

$(OBJDIR)/tests/db2_memory_episodes_monolith.o: modules/memory/memory_episodes.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-node-kind-text-support: \
                     $(OBJDIR)/tests/test_db2_node_kind_text_support.o \
                     $(OBJDIR)/tests/db2_node_kind_text_support_impl.o \
                     $(OBJDIR)/tests/db2_memory_episodes_monolith.o
	$(TESTLINK_MIN) -Wl,--gc-sections -o $@ $^ $(TEST_L_FLAGS)

DB2_NODE_KIND_TEXT_SANITIZE_DIR = $(OBJDIR)/tests/db2-node-kind-text-support-sanitize

$(DB2_NODE_KIND_TEXT_SANITIZE_DIR)/test.o: tests/test_db2_node_kind_text_support.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_NODE_KIND_TEXT_SANITIZE_DIR)/support.o: \
                     modules/db2/support/node_kind_text_primitives.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_NODE_KIND_TEXT_SUPPORT_RENAMES) \
	      $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_NODE_KIND_TEXT_SANITIZE_DIR)/monolith.o: modules/memory/memory_episodes.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-node-kind-text-support-sanitize: \
                     $(DB2_NODE_KIND_TEXT_SANITIZE_DIR)/test.o \
                     $(DB2_NODE_KIND_TEXT_SANITIZE_DIR)/support.o \
                     $(DB2_NODE_KIND_TEXT_SANITIZE_DIR)/monolith.o
	$(CC) $(DB2_SUPPORT_SANITIZE_FLAGS) -Wl,--gc-sections -o $@ $^

.PHONY: unit-test-db2-node-kind-text-support unit-test-db2-node-kind-text-support-sanitize
unit-test-db2-node-kind-text-support: $(TESTPREFIX)/unit-test-db2-node-kind-text-support
	$<

unit-test-db2-node-kind-text-support-sanitize: \
      $(TESTPREFIX)/unit-test-db2-node-kind-text-support-sanitize
	$<

DB2_PII_INJECT_GATE_SUPPORT_RENAMES = \
   -Dmemory_pii_should_inject=db2_support_memory_pii_should_inject
DB2_PII_INJECT_GATE_SECTION_FLAGS = -ffunction-sections -fdata-sections

$(OBJDIR)/tests/db2_pii_inject_gate_support_impl.o: \
                     modules/db2/support/pii_inject_gate_primitives.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_PII_INJECT_GATE_SUPPORT_RENAMES) \
	      $(DB2_PII_INJECT_GATE_SECTION_FLAGS) -c -o $@ $<

$(OBJDIR)/tests/db2_memory_pii_gate_monolith.o: modules/memory/memory_pii_gate.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_PII_INJECT_GATE_SECTION_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-pii-inject-gate-support: \
                     $(OBJDIR)/tests/test_db2_pii_inject_gate_support.o \
                     $(OBJDIR)/tests/db2_pii_inject_gate_support_impl.o \
                     $(OBJDIR)/tests/db2_memory_pii_gate_monolith.o
	$(CC) -Wl,--gc-sections -o $@ $^

DB2_PII_INJECT_GATE_SANITIZE_DIR = \
                     $(OBJDIR)/tests/db2-pii-inject-gate-support-sanitize

$(DB2_PII_INJECT_GATE_SANITIZE_DIR)/test.o: \
                     tests/test_db2_pii_inject_gate_support.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_PII_INJECT_GATE_SANITIZE_DIR)/support.o: \
                     modules/db2/support/pii_inject_gate_primitives.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_PII_INJECT_GATE_SUPPORT_RENAMES) \
	      $(DB2_PII_INJECT_GATE_SECTION_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) \
	      -c -o $@ $<

$(DB2_PII_INJECT_GATE_SANITIZE_DIR)/monolith.o: \
                     modules/memory/memory_pii_gate.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_PII_INJECT_GATE_SECTION_FLAGS) \
	      $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-pii-inject-gate-support-sanitize: \
                     $(DB2_PII_INJECT_GATE_SANITIZE_DIR)/test.o \
                     $(DB2_PII_INJECT_GATE_SANITIZE_DIR)/support.o \
                     $(DB2_PII_INJECT_GATE_SANITIZE_DIR)/monolith.o
	$(CC) $(DB2_SUPPORT_SANITIZE_FLAGS) -Wl,--gc-sections -o $@ $^

.PHONY: unit-test-db2-pii-inject-gate-support \
        unit-test-db2-pii-inject-gate-support-sanitize
unit-test-db2-pii-inject-gate-support: \
      $(TESTPREFIX)/unit-test-db2-pii-inject-gate-support
	$<

unit-test-db2-pii-inject-gate-support-sanitize: \
      $(TESTPREFIX)/unit-test-db2-pii-inject-gate-support-sanitize
	$<

DB2_PII_CLASSIFIER_SUPPORT_RENAMES = \
   -Dmemory_pii_register_turn_classifier=db2_support_memory_pii_register_turn_classifier \
   -Dmemory_pii_turn_requests_sensitive=db2_support_memory_pii_turn_requests_sensitive \
   -Dmemory_pii_rel_sensitivity=db2_support_memory_pii_rel_sensitivity \
   -Dmemory_pii_register_sensitivity_batch=db2_support_memory_pii_register_sensitivity_batch \
   -Dmemory_pii_rel_sensitivity_batch=db2_support_memory_pii_rel_sensitivity_batch
DB2_PII_CLASSIFIER_SECTION_FLAGS = -ffunction-sections -fdata-sections

$(OBJDIR)/tests/db2_pii_classifier_support_impl.o: \
                     modules/db2/support/pii_classifier_primitives.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_PII_CLASSIFIER_SUPPORT_RENAMES) \
	      $(DB2_PII_CLASSIFIER_SECTION_FLAGS) -c -o $@ $<

$(OBJDIR)/tests/db2_pii_classifier_monolith.o: modules/memory/memory_pii_gate.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_PII_CLASSIFIER_SECTION_FLAGS) -c -o $@ $<

$(OBJDIR)/tests/db2_pii_classifier_rel_type.o: modules/db2/support/rel_type_primitives.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_PII_CLASSIFIER_SECTION_FLAGS) -c -o $@ $<

$(OBJDIR)/tests/db2_pii_classifier_rel_seed.o: modules/db2/support/rel_seed_primitives.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_PII_CLASSIFIER_SECTION_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-pii-classifier-support: \
                     $(OBJDIR)/tests/test_db2_pii_classifier_support.o \
                     $(OBJDIR)/tests/db2_pii_classifier_support_impl.o \
                     $(OBJDIR)/tests/db2_pii_classifier_monolith.o \
                     $(OBJDIR)/tests/db2_pii_classifier_rel_type.o \
                     $(OBJDIR)/tests/db2_pii_classifier_rel_seed.o
	$(CC) -Wl,--gc-sections -o $@ $^

DB2_PII_CLASSIFIER_SANITIZE_DIR = $(OBJDIR)/tests/db2-pii-classifier-support-sanitize

$(DB2_PII_CLASSIFIER_SANITIZE_DIR)/test.o: tests/test_db2_pii_classifier_support.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_PII_CLASSIFIER_SANITIZE_DIR)/support.o: \
                     modules/db2/support/pii_classifier_primitives.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_PII_CLASSIFIER_SUPPORT_RENAMES) \
	      $(DB2_PII_CLASSIFIER_SECTION_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_PII_CLASSIFIER_SANITIZE_DIR)/monolith.o: modules/memory/memory_pii_gate.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_PII_CLASSIFIER_SECTION_FLAGS) \
	      $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_PII_CLASSIFIER_SANITIZE_DIR)/rel-type.o: \
                     modules/db2/support/rel_type_primitives.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_PII_CLASSIFIER_SECTION_FLAGS) \
	      $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_PII_CLASSIFIER_SANITIZE_DIR)/rel-seed.o: \
                     modules/db2/support/rel_seed_primitives.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_PII_CLASSIFIER_SECTION_FLAGS) \
	      $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-pii-classifier-support-sanitize: \
                     $(DB2_PII_CLASSIFIER_SANITIZE_DIR)/test.o \
                     $(DB2_PII_CLASSIFIER_SANITIZE_DIR)/support.o \
                     $(DB2_PII_CLASSIFIER_SANITIZE_DIR)/monolith.o \
                     $(DB2_PII_CLASSIFIER_SANITIZE_DIR)/rel-type.o \
                     $(DB2_PII_CLASSIFIER_SANITIZE_DIR)/rel-seed.o
	$(CC) $(DB2_SUPPORT_SANITIZE_FLAGS) -Wl,--gc-sections -o $@ $^

.PHONY: unit-test-db2-pii-classifier-support unit-test-db2-pii-classifier-support-sanitize
unit-test-db2-pii-classifier-support: \
                     $(TESTPREFIX)/unit-test-db2-pii-classifier-support
	$<

unit-test-db2-pii-classifier-support-sanitize: \
                     $(TESTPREFIX)/unit-test-db2-pii-classifier-support-sanitize
	$<

$(OBJDIR)/tests/db2_runtime_config_support_impl.o: \
                     modules/db2/support/runtime_config_primitives.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-runtime-config-support: \
                     $(OBJDIR)/tests/test_db2_runtime_config_support.o \
                     $(OBJDIR)/tests/db2_runtime_config_support_impl.o
	$(TESTLINK_MIN) -o $@ $^ $(TEST_L_FLAGS)

DB2_RUNTIME_CONFIG_SANITIZE_DIR = $(OBJDIR)/tests/db2-runtime-config-support-sanitize

$(DB2_RUNTIME_CONFIG_SANITIZE_DIR)/test.o: tests/test_db2_runtime_config_support.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_RUNTIME_CONFIG_SANITIZE_DIR)/support.o: \
                     modules/db2/support/runtime_config_primitives.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-runtime-config-support-sanitize: \
                     $(DB2_RUNTIME_CONFIG_SANITIZE_DIR)/test.o \
                     $(DB2_RUNTIME_CONFIG_SANITIZE_DIR)/support.o
	$(CC) $(DB2_SUPPORT_SANITIZE_FLAGS) -o $@ $^

.PHONY: unit-test-db2-runtime-config-support \
        unit-test-db2-runtime-config-support-sanitize
unit-test-db2-runtime-config-support: $(TESTPREFIX)/unit-test-db2-runtime-config-support
	$<

unit-test-db2-runtime-config-support-sanitize: \
                     $(TESTPREFIX)/unit-test-db2-runtime-config-support-sanitize
	$<

$(OBJDIR)/tests/db2_log_support_impl.o: modules/db2/support/log_primitives.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-log-support: \
                     $(OBJDIR)/tests/test_db2_log_support.o \
                     $(OBJDIR)/tests/db2_log_support_impl.o
	$(TESTLINK_MIN) -o $@ $^ $(TEST_L_FLAGS)

DB2_LOG_SANITIZE_DIR = $(OBJDIR)/tests/db2-log-support-sanitize

$(DB2_LOG_SANITIZE_DIR)/test.o: tests/test_db2_log_support.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_LOG_SANITIZE_DIR)/support.o: modules/db2/support/log_primitives.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-log-support-sanitize: \
                     $(DB2_LOG_SANITIZE_DIR)/test.o \
                     $(DB2_LOG_SANITIZE_DIR)/support.o
	$(CC) $(DB2_SUPPORT_SANITIZE_FLAGS) -o $@ $^

.PHONY: unit-test-db2-log-support unit-test-db2-log-support-sanitize
unit-test-db2-log-support: $(TESTPREFIX)/unit-test-db2-log-support
	$<

unit-test-db2-log-support-sanitize: $(TESTPREFIX)/unit-test-db2-log-support-sanitize
	$<

DB2_REL_TYPE_SUPPORT_RENAMES = \
   -Drel_type_is_functional=db2_support_rel_type_is_functional \
   -Drel_type_normalize=db2_support_rel_type_normalize
DB2_REL_TYPE_SECTION_FLAGS = -ffunction-sections -fdata-sections

$(OBJDIR)/tests/db2_rel_type_support_impl.o: modules/db2/support/rel_type_primitives.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_REL_TYPE_SUPPORT_RENAMES) \
	      $(DB2_REL_TYPE_SECTION_FLAGS) -c -o $@ $<

$(OBJDIR)/tests/db2_rel_type_monolith.o: rel_types.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_REL_TYPE_SECTION_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-rel-type-support: \
                     $(OBJDIR)/tests/test_db2_rel_type_support.o \
                     $(OBJDIR)/tests/db2_rel_type_support_impl.o \
                     $(OBJDIR)/tests/db2_rel_type_monolith.o
	$(TESTLINK_MIN) -Wl,--gc-sections -o $@ $^ $(TEST_L_FLAGS)

DB2_REL_TYPE_SANITIZE_DIR = $(OBJDIR)/tests/db2-rel-type-support-sanitize

$(DB2_REL_TYPE_SANITIZE_DIR)/test.o: tests/test_db2_rel_type_support.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_REL_TYPE_SANITIZE_DIR)/support.o: modules/db2/support/rel_type_primitives.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_REL_TYPE_SUPPORT_RENAMES) $(DB2_REL_TYPE_SECTION_FLAGS) \
	      $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_REL_TYPE_SANITIZE_DIR)/monolith.o: rel_types.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_REL_TYPE_SECTION_FLAGS) \
	      $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-rel-type-support-sanitize: \
                     $(DB2_REL_TYPE_SANITIZE_DIR)/test.o \
                     $(DB2_REL_TYPE_SANITIZE_DIR)/support.o \
                     $(DB2_REL_TYPE_SANITIZE_DIR)/monolith.o
	$(CC) $(DB2_SUPPORT_SANITIZE_FLAGS) -Wl,--gc-sections -o $@ $^

.PHONY: unit-test-db2-rel-type-support unit-test-db2-rel-type-support-sanitize
unit-test-db2-rel-type-support: $(TESTPREFIX)/unit-test-db2-rel-type-support
	$<

unit-test-db2-rel-type-support-sanitize: $(TESTPREFIX)/unit-test-db2-rel-type-support-sanitize
	$<

DB2_REL_SEED_SUPPORT_RENAMES = \
   -Drel_type_normalize=db2_support_rel_type_normalize \
   -Drel_types_seed_at=db2_support_rel_types_seed_at \
   -Drel_types_seed_count=db2_support_rel_types_seed_count \
   -Drel_types_seed_lookup=db2_support_rel_types_seed_lookup

$(OBJDIR)/tests/db2_rel_seed_support_impl.o: modules/db2/support/rel_seed_primitives.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_REL_SEED_SUPPORT_RENAMES) \
	      $(DB2_REL_TYPE_SECTION_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-rel-seed-support: \
                     $(OBJDIR)/tests/test_db2_rel_seed_support.o \
                     $(OBJDIR)/tests/db2_rel_seed_support_impl.o \
                     $(OBJDIR)/tests/db2_rel_type_support_impl.o \
                     $(OBJDIR)/tests/db2_rel_type_monolith.o
	$(TESTLINK_MIN) -Wl,--gc-sections -o $@ $^ $(TEST_L_FLAGS)

DB2_REL_SEED_SANITIZE_DIR = $(OBJDIR)/tests/db2-rel-seed-support-sanitize

$(DB2_REL_SEED_SANITIZE_DIR)/test.o: tests/test_db2_rel_seed_support.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_REL_SEED_SANITIZE_DIR)/support.o: modules/db2/support/rel_seed_primitives.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_REL_SEED_SUPPORT_RENAMES) $(DB2_REL_TYPE_SECTION_FLAGS) \
	      $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-rel-seed-support-sanitize: \
                     $(DB2_REL_SEED_SANITIZE_DIR)/test.o \
                     $(DB2_REL_SEED_SANITIZE_DIR)/support.o \
                     $(DB2_REL_TYPE_SANITIZE_DIR)/support.o \
                     $(DB2_REL_TYPE_SANITIZE_DIR)/monolith.o
	$(CC) $(DB2_SUPPORT_SANITIZE_FLAGS) -Wl,--gc-sections -o $@ $^

.PHONY: unit-test-db2-rel-seed-support unit-test-db2-rel-seed-support-sanitize
unit-test-db2-rel-seed-support: $(TESTPREFIX)/unit-test-db2-rel-seed-support
	$<

unit-test-db2-rel-seed-support-sanitize: $(TESTPREFIX)/unit-test-db2-rel-seed-support-sanitize
	$<

DB2_SKETCH_SUPPORT_RENAMES = \
   -Dsketch_bloom_init=db2_support_sketch_bloom_init \
   -Dsketch_count_min_init=db2_support_sketch_count_min_init \
   -Dsketch_fnv1a=db2_support_sketch_fnv1a \
   -Dsketch_hll_add_hash=db2_support_sketch_hll_add_hash \
   -Dsketch_hll_init=db2_support_sketch_hll_init \
   -Dsketch_lsh_band_hash=db2_support_sketch_lsh_band_hash \
   -Dsketch_minhash_init=db2_support_sketch_minhash_init

$(OBJDIR)/tests/db2_sketch_support_impl.o: modules/db2/support/sketch_primitives.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_SKETCH_SUPPORT_RENAMES) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-sketch-support: \
                     $(OBJDIR)/tests/test_db2_sketch_support.o \
                     $(OBJDIR)/tests/db2_sketch_support_impl.o \
                     $(OBJDIR)/sketch.o
	$(TESTLINK_MIN) -o $@ $^ $(TEST_L_FLAGS) -lm

DB2_SKETCH_SANITIZE_DIR = $(OBJDIR)/tests/db2-sketch-support-sanitize
DB2_SUPPORT_SANITIZE_FLAGS = -O1 -g -fno-lto -fsanitize=address,undefined \
                             -fno-omit-frame-pointer -U_FORTIFY_SOURCE \
                             -D_FORTIFY_SOURCE=3

$(DB2_SKETCH_SANITIZE_DIR)/test.o: tests/test_db2_sketch_support.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_SKETCH_SANITIZE_DIR)/support.o: modules/db2/support/sketch_primitives.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_SKETCH_SUPPORT_RENAMES) \
	      $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(DB2_SKETCH_SANITIZE_DIR)/monolith.o: sketch.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_C_FLAGS) $(DB2_SUPPORT_SANITIZE_FLAGS) -c -o $@ $<

$(TESTPREFIX)/unit-test-db2-sketch-support-sanitize: \
                     $(DB2_SKETCH_SANITIZE_DIR)/test.o \
                     $(DB2_SKETCH_SANITIZE_DIR)/support.o \
                     $(DB2_SKETCH_SANITIZE_DIR)/monolith.o
	$(CC) $(DB2_SUPPORT_SANITIZE_FLAGS) -o $@ $^ -lm

.PHONY: unit-test-db2-sketch-support
unit-test-db2-sketch-support: $(TESTPREFIX)/unit-test-db2-sketch-support
	$<

.PHONY: unit-test-db2-sketch-support-sanitize
unit-test-db2-sketch-support-sanitize: $(TESTPREFIX)/unit-test-db2-sketch-support-sanitize
	$<

$(TESTPREFIX)/unit-test-kb-lab: $(OBJDIR)/tests/test_kb_lab.o \
                     $(OBJDIR)/kb/kb_lab.o \
                     $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-fusion: $(OBJDIR)/tests/test_kb_fusion.o \
                     $(OBJDIR)/kb/kb_fusion.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm

$(TESTPREFIX)/unit-test-kb-export: $(OBJDIR)/tests/test_kb_export.o \
                     $(OBJDIR)/kb_export_obsidian.o $(OBJDIR)/kb_export_json.o \
                     $(OBJDIR)/db2/kb_service_backend_export.o \
                     $(OBJDIR)/cJSON.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-artifacts: $(OBJDIR)/tests/test_artifacts.o \
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                     $(OBJDIR)/db2/feature_rows.o \
                     $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/modules/learning/learning_evidence.o $(OBJDIR)/db2/learning_synth_ops.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o $(OBJDIR)/db2/feedback.o \
                     $(OBJDIR)/db2/anti_patterns.o $(OBJDIR)/modules/db1/approach_failures.o $(OBJDIR)/approach_store.o \
                     $(OBJDIR)/db2/workflow_patterns.o \
                     $(OBJDIR)/db2/rules.o $(OBJDIR)/db2/stopwords.o \
                     $(OBJDIR)/db2/epistemic_directives.o \
                     $(OBJDIR)/db2/entity_nodes.o \
                     $(OBJDIR)/db2/evidence_vectors.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-evidence-embed: $(OBJDIR)/tests/test_evidence_embed.o \
                     $(OBJDIR)/kb/kb_evidence_embed.o \
                     $(OBJDIR)/db2/evidence_vectors.o \
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                     $(OBJDIR)/db2/feature_rows.o \
                     $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-learning-bundle: $(OBJDIR)/tests/test_learning_bundle.o \
                     $(OBJDIR)/modules/learning/learning_bundle.o \
                     $(OBJDIR)/db2/evidence_vectors.o \
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                     $(OBJDIR)/db2/feature_rows.o \
                     $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm -lzstd

$(TESTPREFIX)/unit-test-learning-synth: $(OBJDIR)/tests/test_learning_synth.o \
                     $(OBJDIR)/kb/kb_learning_synth.o \
                     $(OBJDIR)/modules/learning/learning_bundle.o \
                     $(OBJDIR)/db2/evidence_vectors.o \
                     $(OBJDIR)/db2/learning_synth_ops.o \
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                     $(OBJDIR)/db2/feature_rows.o \
                     $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(OBJDIR)/posix/platform_process.o \
                     $(OBJDIR)/linux/platform_process.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm -lzstd

$(TESTPREFIX)/unit-test-learning-version: $(OBJDIR)/tests/test_learning_version.o \
                     $(OBJDIR)/kb/kb_learning_version.o \
                     $(OBJDIR)/db2/evidence_vectors.o \
                     $(OBJDIR)/db2/learning_synth_ops.o \
                     $(OBJDIR)/db2/kb_runtime_state.o \
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                     $(OBJDIR)/db2/feature_rows.o \
                     $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm -lzstd

$(TESTPREFIX)/unit-test-corpus-structural: $(OBJDIR)/tests/test_corpus_structural.o \
                     $(OBJDIR)/db2/corpus_structural.o \
                     $(OBJDIR)/db2/corpus_jobs.o \
                     $(OBJDIR)/db2/kb_docs.o \
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                     $(OBJDIR)/db2/feature_rows.o $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-corpus-jobs: $(OBJDIR)/tests/test_corpus_jobs.o \
                     $(OBJDIR)/db2/corpus_jobs.o \
                     $(OBJDIR)/db2/curator_terms.o \
                     $(OBJDIR)/db2/curator_gaps.o \
                     $(OBJDIR)/db2/corpus_structural.o \
                     $(OBJDIR)/db2/curiosity.o \
                     $(OBJDIR)/db2/kb_docs.o \
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                     $(OBJDIR)/db2/feature_rows.o $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-corpus-terms-gaps: $(OBJDIR)/tests/test_corpus_terms_gaps.o \
                     $(OBJDIR)/db2/curator_terms.o \
                     $(OBJDIR)/db2/curator_gaps.o \
                     $(OBJDIR)/db2/corpus_structural.o \
                     $(OBJDIR)/db2/corpus_jobs.o \
                     $(OBJDIR)/db2/curiosity.o \
                     $(OBJDIR)/db2/kb_docs.o \
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                     $(OBJDIR)/db2/feature_rows.o $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-calibration: $(OBJDIR)/tests/test_calibration.o \
                     $(OBJDIR)/kb/kb_calibrate.o \
                     $(OBJDIR)/db2/calibration.o \
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                     $(OBJDIR)/db2/feature_rows.o \
                     $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-demotion: $(OBJDIR)/tests/test_demotion.o \
                     $(OBJDIR)/db2/demotion.o \
                     $(OBJDIR)/db2/memory_payload.o \
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                     $(OBJDIR)/db2/feature_rows.o \
                     $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm -lzstd

$(TESTPREFIX)/unit-test-features: $(OBJDIR)/tests/test_features.o \
                     $(OBJDIR)/kb/kb_features.o \
                     $(OBJDIR)/kb/kb_ranker.o \
                     $(OBJDIR)/kb/kb_detect.o \
                     $(OBJDIR)/db2/feature_rows.o \
                     $(OBJDIR)/db2/sketch.o \
                     $(OBJDIR)/sketch.o \
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                     $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/db2/calibration.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm -lzstd

$(TESTPREFIX)/unit-test-ranker-fit: $(OBJDIR)/tests/test_ranker_fit.o \
                     $(OBJDIR)/kb/kb_ranker_fit.o \
                     $(OBJDIR)/kb/kb_ranker.o \
                     $(OBJDIR)/kb/kb_features.o \
                     $(OBJDIR)/kb/kb_detect.o \
                     $(OBJDIR)/db2/feature_rows.o \
                     $(OBJDIR)/db2/sketch.o \
                     $(OBJDIR)/sketch.o \
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                     $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm -lzstd

# Bridge logic in isolation: the test stubs legacy_config_read + the KB client, so no
# DB/network objects are needed — just the bridge TU.
$(TESTPREFIX)/unit-test-retrieval-outcome-bridge: $(OBJDIR)/tests/test_retrieval_outcome_bridge.o \
                     $(OBJDIR)/server/retrieval_outcome_bridge.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(OBJDIR)/tests/test_artifacts.o: C_FLAGS += -Imodules/db2/include

# Pure render/extract helpers behind the kb_search tool — cJSON + dstr only.
$(TESTPREFIX)/unit-test-td-search-render: $(OBJDIR)/tests/test_td_search_render.o \
                     $(OBJDIR)/posix/td_search_render.o \
                     $(OBJDIR)/cJSON.o $(OBJDIR)/dstr.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-report-enrichments: $(OBJDIR)/tests/test_report_enrichments.o \
                     $(OBJDIR)/db2/report_enrichments.o \
                     $(OBJDIR)/report_enrichment.o $(OBJDIR)/util_url.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-reasoning: $(OBJDIR)/tests/test_reasoning.o \
                     $(OBJDIR)/kb/kb_reasoning.o \
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                     $(OBJDIR)/db2/feature_rows.o \
                     $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-bandit: $(OBJDIR)/tests/test_bandit.o \
                     $(OBJDIR)/kb/kb_bandit.o \
                     $(OBJDIR)/kb/kb_bandit_registry.o \
                     $(OBJDIR)/db2/bandit.o \
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                     $(OBJDIR)/db2/feature_rows.o \
                     $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-planner: $(OBJDIR)/tests/test_planner.o \
                     $(OBJDIR)/kb/kb_planner.o \
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                     $(OBJDIR)/db2/feature_rows.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-roadmap: $(OBJDIR)/tests/test_roadmap.o \
                     $(OBJDIR)/kb/roadmap.o \
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                     $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/db2/feature_rows.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-kb-releases-db: $(OBJDIR)/tests/test_kb_releases_db.o \
                     $(OBJDIR)/db2/kb_releases.o \
                     $(OBJDIR)/db2/kb_runtime_state.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-roadmap-decompose: $(OBJDIR)/tests/test_roadmap_decompose.o \
                     $(OBJDIR)/modules/roadmap/roadmap_decompose.o \
                     $(OBJDIR)/kb/roadmap.o \
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                     $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/db2/feature_rows.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-kb-mdl: $(OBJDIR)/tests/test_kb_mdl.o \
                     $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/db2/feature_rows.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_hardening.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

# guardrails_semantic.o -> obs_bus_emit_guardrail, so this test now links the
# bus (gsem_record records over it) plus the bus's own deps.
$(OBJDIR)/tests/test_guardrails_semantic.o: C_FLAGS += -Icore/event_bus/include
$(TESTPREFIX)/unit-test-guardrails-semantic: $(OBJDIR)/tests/test_guardrails_semantic.o \
                     $(OBJDIR)/modules/guardrails/guardrails_semantic.o \
                     $(OBJDIR)/server/obs_bus_adapter.o \
                     $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o $(OBJDIR)/modules/db1/guardrail_events.o \
                     $(OBS_BUS_LINK_OBJS) $(OBJDIR)/modules/audit/audit_ledger.o \
                     $(OBJDIR)/aimee_home.o \
                     $(OBJDIR)/core/event_bus/bus_client.o $(OBJDIR)/core/event_bus/bus_attach.o $(OBJDIR)/core/event_bus/bus_host.o \
                     $(OBJDIR)/core/event_bus/bus_route.o $(OBJDIR)/core/event_bus/bus_region.o $(OBJDIR)/core/event_bus/bus_region_host.o \
                     $(OBJDIR)/core/event_bus/bus_ring.o $(OBJDIR)/core/event_bus/bus_arena.o \
                     $(OBJDIR)/core/event_bus/bus_wire.o $(OBJDIR)/core/event_bus/bus_capture.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lpthread

# Links the config closure rather than L_MINIMAL: computer_use_policy_from_config
# reads its policy through the accessors now, so the
# minimal link can no longer resolve it.
$(TESTPREFIX)/unit-test-guardrails-computer-use: $(OBJDIR)/tests/test_guardrails_computer_use.o \
                     $(OBJDIR)/server/computer_use.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-osv-check: $(OBJDIR)/tests/test_osv_check.o \
                     $(OBJDIR)/server/osv_check.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-mcp-osv-cache: $(OBJDIR)/tests/test_mcp_osv_cache.o \
                     $(OBJDIR)/modules/db1/db1_init.o $(OBJDIR)/modules/db1/db_schema.o \
                     $(OBJDIR)/modules/db1/mcp_osv_cache.o $(OBJDIR)/modules/db1/interaction_events.o \
                     $(OBJDIR)/modules/db1/maintenance.o $(OBJDIR)/log.o $(OBJDIR)/util.o \
                     $(OBJDIR)/cJSON.o $(PLATFORM_BASIC_OBJS) $(OBJDIR)/interaction_event_names.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-mcp-client-registry: $(OBJDIR)/tests/test_mcp_client_registry.o \
                     $(OBJDIR)/modules/protocols/mcp/mcp_tools.o $(OBJDIR)/modules/protocols/mcp/mcp_tool_profile.o $(OBJDIR)/modules/protocols/mcp/mcp_tools_extended.o $(OBJDIR)/modules/protocols/mcp/mcp_skill_tools.o $(OBJDIR)/modules/protocols/mcp/mcp_tools_gateway.o \
                     $(OBJDIR)/server/session_search_tool.o \
                     $(CONFIG_CLIENT_TEST_OBJS) $(OBJDIR)/platform_random.o \
                     $(OBJDIR)/yaml.o \
                     $(OBJDIR)/modules/db1/db.o \
                     $(OBJDIR)/modules/db1/db_schema.o $(OBJDIR)/modules/db1/maintenance.o $(DB2_TEST_BACKEND_OBJ) $(OBJDIR)/db2/db2_test_shim.o \
                     $(OBJDIR)/log.o \
                     $(OBJDIR)/aimee_home.o \
                     $(OBJDIR)/server/osv_check.o \
                     $(OBJDIR)/modules/vault/runtime_secret.o \
                     $(OBJDIR)/modules/protocols/mcp/mcp_client_registry.o \
                     $(TEST_MCP_CLIENT_OBJS) \
                     $(TESTPREFIX)/mock-mcp-server
	$(TESTLINK) -o $@ $(OBJDIR)/tests/test_mcp_client_registry.o $(OBJDIR)/modules/protocols/mcp/mcp_tools.o $(OBJDIR)/modules/protocols/mcp/mcp_tool_profile.o $(OBJDIR)/modules/protocols/mcp/mcp_tools_extended.o $(OBJDIR)/modules/protocols/mcp/mcp_skill_tools.o $(OBJDIR)/modules/protocols/mcp/mcp_tools_gateway.o $(OBJDIR)/server/session_search_tool.o $(CONFIG_CLIENT_TEST_OBJS) $(OBJDIR)/platform_random.o $(OBJDIR)/aimee_home.o $(OBJDIR)/yaml.o $(OBJDIR)/modules/db1/db.o $(OBJDIR)/modules/db1/db_schema.o $(OBJDIR)/modules/db1/maintenance.o $(DB2_TEST_BACKEND_OBJ) $(OBJDIR)/db2/db2_test_shim.o $(OBJDIR)/log.o $(OBJDIR)/server/osv_check.o $(OBJDIR)/modules/vault/runtime_secret.o $(OBJDIR)/modules/protocols/mcp/mcp_client_registry.o $(TEST_MCP_CLIENT_OBJS) $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-agent-request-build: $(OBJDIR)/tests/test_agent_request_build.o \
                                       $(OBJDIR)/server/agent_request_build.o \
                                       $(OBJDIR)/modules/translation/aimee_backend_openai.o \
                                       $(OBJDIR)/modules/translation/aimee_backend_responses.o \
                                       $(OBJDIR)/modules/translation/aimee_backend_anthropic.o \
                                       $(OBJDIR)/modules/ir/aimee_ir.o \
                                       $(OBJDIR)/modules/ir/aimee_ir_metrics.o \
                                       $(OBJDIR)/server/model_sampling.o \
                                       $(OBJDIR)/server/anthropic_shape.o \
                                       $(OBJDIR)/server/agent_bridge.o \
                                       $(OBJDIR)/server/agent_request_shaping.o \
                                       $(OBJDIR)/server/provider_catalog.o \
                                       $(TEST_CORE_OBJS) $(OBJDIR)/platform_random.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-ir-crossproto-egress: $(OBJDIR)/tests/test_ir_crossproto_egress.o \
                                       $(OBJDIR)/modules/translation/aimee_backend_anthropic.o \
                                       $(OBJDIR)/modules/translation/aimee_frontend_anthropic.o \
                                       $(OBJDIR)/modules/translation/aimee_frontend_openai.o \
                                       $(OBJDIR)/modules/ir/aimee_ir.o \
                                       $(OBJDIR)/modules/ir/aimee_ir_metrics.o \
                                       $(OBJDIR)/text.o $(OBJDIR)/util.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-ir-legacy-parity: $(OBJDIR)/tests/test_ir_legacy_parity.o \
                                       $(OBJDIR)/server/aimee_ir_serve.o $(OBJDIR)/modules/ir/aimee_ir_session.o \
                                       $(OBJDIR)/modules/translation/aimee_backend_openai.o \
                                       $(OBJDIR)/modules/translation/aimee_backend_responses.o \
                                       $(OBJDIR)/modules/translation/aimee_frontend_anthropic.o \
                                       $(OBJDIR)/modules/translation/aimee_frontend_openai.o \
                                       $(OBJDIR)/modules/translation/aimee_frontend_responses.o \
                                       $(OBJDIR)/modules/ir/aimee_ir.o \
                                       $(OBJDIR)/tests/support/ir_seam_memory_stub.o \
                                       $(OBJDIR)/modules/ir/aimee_ir_metrics.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-aimee-ir-rescue: $(OBJDIR)/tests/test_aimee_ir_rescue.o \
                                         $(OBJDIR)/modules/delegates/aimee_ir_rescue.o \
                                         $(OBJDIR)/modules/ir/aimee_ir.o \
                                         $(OBJDIR)/modules/ir/aimee_ir_metrics.o \
                                         $(OBJDIR)/modules/delegates/delegate_xml_fallback.o \
                                         $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-ir-shadow-response: $(OBJDIR)/tests/test_ir_shadow_response.o \
                                            $(OBJDIR)/server/aimee_ir_shadow.o \
                                            $(OBJDIR)/modules/translation/aimee_backend_anthropic.o \
                                            $(OBJDIR)/modules/translation/aimee_backend_openai.o \
                                            $(OBJDIR)/modules/translation/aimee_backend_responses.o \
                                            $(OBJDIR)/modules/translation/aimee_frontend_anthropic.o \
                                            $(OBJDIR)/modules/translation/aimee_frontend_openai.o \
                                            $(OBJDIR)/modules/translation/aimee_frontend_responses.o \
                                            $(OBJDIR)/modules/ir/aimee_ir.o \
                                            $(OBJDIR)/modules/ir/aimee_ir_metrics.o \
                                            $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-shadow-mirror: $(OBJDIR)/tests/test_shadow_mirror.o \
                                       $(OBJDIR)/server/shadow_mirror.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-agent-ir-parse: $(OBJDIR)/tests/test_agent_ir_parse.o \
                                        $(OBJDIR)/posix/agent_ir_parse.o \
                                        $(OBJDIR)/modules/delegates/aimee_ir_rescue.o \
                                        $(OBJDIR)/modules/delegates/delegate_xml_fallback.o \
                                        $(OBJDIR)/modules/translation/aimee_backend_anthropic.o \
                                        $(OBJDIR)/modules/translation/aimee_backend_openai.o \
                                        $(OBJDIR)/modules/ir/aimee_ir.o \
                                        $(OBJDIR)/modules/ir/aimee_ir_metrics.o \
                                        $(OBJDIR)/server/tool_call_args.o \
                                        $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-hashline-gate: $(OBJDIR)/tests/test_hashline_gate.o $(OBJDIR)/server/agent_cli_shell.o \
                      $(OBJDIR)/modules/audit/audit_action.o $(OBJDIR)/modules/audit/audit_worm.o $(OBJDIR)/modules/audit/audit_worm_chain.o $(OBJDIR)/modules/workflows/wfe_canonical.o $(OBJDIR)/aimee_sha256.o \
                      $(OBJDIR)/server/tool_call_args.o \
                      $(OBJDIR)/server/session_compact.o $(OBJDIR)/server/rounds_to_resume.o $(OBJDIR)/server/compact_prune.o $(OBJDIR)/modules/delegates/delegate_driver.o \
                      $(OBJDIR)/modules/delegates/delegate_openai.o $(OBJDIR)/modules/delegates/delegate_xml_fallback.o $(OBJDIR)/modules/delegates/delegate_role.o \
                      $(OBJDIR)/model_registry.o $(OBJDIR)/models_dev.o \
                      $(OBJDIR)/models_dev_cache.o $(OBJDIR)/payload_rewrite.o \
                      $(OBJDIR)/server/middleware.o $(OBJDIR)/server/liveness.o \
                      $(OBJDIR)/server/cli_session.o $(OBJDIR)/server/agent_policy_intercept.o \
                      $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Differential parity: legacy agent_parse_response_responses vs IR agent_ir_parse_responses
# on identical codex SSE bytes. Links agent_bridge.o (legacy parser + SSE extractor) with
# the full IR backend chain and NO weak stubs, so both parsers run for real.
$(TESTPREFIX)/unit-test-responses-parity: $(OBJDIR)/tests/test_responses_parity.o \
                                          $(OBJDIR)/server/agent_bridge.o \
                                          $(OBJDIR)/posix/agent_ir_parse.o \
                                          $(OBJDIR)/modules/translation/aimee_backend_responses.o \
                                          $(OBJDIR)/modules/translation/aimee_backend_anthropic.o \
                                          $(OBJDIR)/modules/translation/aimee_backend_openai.o \
                                          $(OBJDIR)/modules/ir/aimee_ir.o \
                                          $(OBJDIR)/modules/delegates/aimee_ir_rescue.o \
                                          $(OBJDIR)/modules/ir/aimee_ir_metrics.o \
                                          $(OBJDIR)/modules/delegates/delegate_xml_fallback.o \
                                          $(OBJDIR)/server/tool_call_args.o \
                                          $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-http-content-encoding: \
    $(OBJDIR)/tests/test_http_content_encoding.o $(OBJDIR)/http_content_encoding.o
	$(TESTLINK_MIN) -o $@ $^ -lz


# Runtime credentials are a low-level process service used by config and client
# code. Keep every standard unit binary link-complete even when a narrow test
# enumerates those objects instead of using TEST_CORE_OBJS; LTO removes the
# cache from binaries that do not reference it.
TEST_KB_RUNTIME_TARGETS = \
  $(TESTPREFIX)/unit-test-kb-audit-worm-pg \
  $(TESTPREFIX)/unit-test-content-scope-pg \
  $(TESTPREFIX)/unit-test-kb-bedrock-live \
  $(TESTPREFIX)/unit-test-kb-p2b-egress-live \
  $(TESTPREFIX)/unit-test-kb-vault-key-use-live \
  $(TESTPREFIX)/unit-test-kb-vault-rotation-live \
  $(TESTPREFIX)/unit-test-kb-vault-rotation-ops-live \
  $(TESTPREFIX)/unit-test-vault-pg \
  $(TESTPREFIX)/unit-test-witness-canary-pg \
  $(TESTPREFIX)/unit-test-witness-checkpoint-produce-pg \
  $(TESTPREFIX)/unit-test-witness-emit-pg \
  $(TESTPREFIX)/unit-test-witness-recovery-pg \
  $(TESTPREFIX)/unit-test-witness-tamper-pg
$(filter-out $(TEST_KB_RUNTIME_TARGETS),$(TEST_TARGETS)): \
  $(OBJDIR)/modules/vault/runtime_secret.o

# ---------------------------------------------------------------- benchmark probes
# Compaction retention probe: measures how much load-bearing detail survives a
# compaction boundary under each summary derivation. Deliberately NOT in
# TEST_TARGETS -- it is a measurement, not a gate, and a derivation scoring badly
# is a result to read rather than a build failure. Run it explicitly:
#   make -C src compaction-retention-probe
#   src/build/obj/tests/compaction-retention-probe benchmarks/compaction-quality/corpus.json
$(OBJDIR)/tests/retention_probe.o: ../benchmarks/compaction-quality/retention_probe.c
	@mkdir -p $(dir $@)
	$(CC) -c $(TEST_C_FLAGS) -o $@ $<

$(TESTPREFIX)/compaction-retention-probe: $(OBJDIR)/tests/retention_probe.o \
                                          $(OBJDIR)/server/token_tracker.o \
                                          $(OBJDIR)/server/session_compact.o $(OBJDIR)/server/rounds_to_resume.o $(OBJDIR)/server/compact_prune.o \
                                          $(OBJDIR)/server/agent_bridge.o $(OBJDIR)/server/anthropic_shape.o $(OBJDIR)/server/tool_call_args.o \
                                          $(OBJDIR)/server/agent_request_shaping.o \
                                          $(OBJDIR)/modules/delegates/delegate_driver.o \
                                          $(OBJDIR)/modules/delegates/delegate_openai.o \
                                          $(OBJDIR)/modules/delegates/delegate_xml_fallback.o \
                                          $(OBJDIR)/model_registry.o $(OBJDIR)/models_dev.o $(OBJDIR)/models_dev_cache.o \
                                          $(OBJDIR)/server/agent_tools.o \
                                          $(OBJDIR)/modules/delegates/delegate_role.o \
                                          $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

.PHONY: compaction-retention-probe
compaction-retention-probe: $(TESTPREFIX)/compaction-retention-probe

# Bounded model-neutral completion intervention over the response seam.
$(TESTPREFIX)/unit-test-response-completion-stage: $(OBJDIR)/tests/test_response_completion_stage.o $(OBJDIR)/modules/governance/gw_stage_completion.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)
