TEST_C_FLAGS = $(C_FLAGS) -I.. -Igateway
TEST_L_FLAGS = $(L_FULL)

# Test output prefix: defaults to $(OBJDIR)/tests so any `make unit-tests`
# invocation with a non-default OBJDIR (sanitizers, coverage, build-integrity,
# ...) automatically gets an isolated test-binary directory. Prevents parallel
# verify steps from racing to link the same tests/unit-test-* paths.
TESTPREFIX ?= $(OBJDIR)/tests
TESTLINK = @mkdir -p $(dir $@) && $(CC)
TESTLINK_MIN = @mkdir -p $(dir $@) && $(CC)
# Test execution parallelism. The unit-tests target used to run binaries in a
# serial shell loop; most tests are process-isolated and safe to fan out.
# Keep it overridable so flaky-debug sessions can force TEST_RUN_JOBS=1.
TEST_RUN_JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 4)

# Common object sets for tests
TEST_CORE_OBJS = $(OBJDIR)/db1/db.o $(OBJDIR)/db1/db_schema.o $(OBJDIR)/db1/maintenance.o $(OBJDIR)/tests/aimee_pg_sqlite_shim.o $(OBJDIR)/db2/db2_test_shim.o $(OBJDIR)/config.o $(OBJDIR)/config_sections.o $(OBJDIR)/config_database.o $(OBJDIR)/config_learning.o $(OBJDIR)/config_memory.o $(OBJDIR)/config_charter.o $(OBJDIR)/config_trigger.o $(OBJDIR)/config_kb_maintenance.o $(OBJDIR)/config_kb_curator.o $(OBJDIR)/config_server_api.o $(OBJDIR)/config_skills.o $(OBJDIR)/config_save.o $(OBJDIR)/config_mode.o $(OBJDIR)/config_fields.o $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/util.o $(OBJDIR)/text.o \
                 $(OBJDIR)/platform_random.o $(PLATFORM_BASIC_OBJS) \
                 $(OBJDIR)/aimee_home.o $(OBJDIR)/shared/kb_paths.o \
                 $(OBJDIR)/log.o $(OBJDIR)/shutdown_forensics.o $(OBJDIR)/cJSON.o $(OBJDIR)/util_url.o $(OBJDIR)/report_enrichment.o $(OBJDIR)/compact.o $(OBJDIR)/coord_closet.o $(OBJDIR)/context_fold.o $(OBJDIR)/context_reduce.o $(OBJDIR)/tool_condense.o $(OBJDIR)/fold_register.o $(OBJDIR)/fold_recall.o $(OBJDIR)/slop_detect.o $(OBJDIR)/proxy_bootstrap.o \
                 $(OBJDIR)/json_fluent.o $(OBJDIR)/markdown.o
# Extended set for tests that need workspace/worktree/guardrails functions (pulls in agents).
TEST_WORKSPACE_OBJS_EXTRA = $(OBJDIR)/workspace.o $(DB1_OBJS) \
                             $(OBJDIR)/server/agent_config.o $(OBJDIR)/tests/support/vault_service_stub.o $(OBJDIR)/tests/support/oauth_tokens_stub.o $(OBJDIR)/server/agent_adapter.o $(OBJDIR)/cmd_describe.o \
                             $(OBJDIR)/posix/cmd_describe.o \
                             $(OBJDIR)/server/agent_runtime.o $(OBJDIR)/server/agent_logging.o $(OBJDIR)/server/request_context.o $(OBJDIR)/server/skill_review.o $(OBJDIR)/server/skill_curator.o $(OBJDIR)/server/agent_context_budget.o $(OBJDIR)/prompts.o $(OBJDIR)/server/provider_cli_adapter.o $(OBJDIR)/server/cli_codex.o $(OBJDIR)/server/cli_claude.o $(OBJDIR)/server/cli_gemini.o $(OBJDIR)/server/cli_mistral.o $(OBJDIR)/server/cli_acp.o $(OBJDIR)/conversation_context.o $(OBJDIR)/server/provider_catalog.o $(OBJDIR)/server/agent_bridge.o $(OBJDIR)/server/anthropic_shape.o $(OBJDIR)/server/tool_call_args.o $(OBJDIR)/server/agent_request_shaping.o $(OBJDIR)/server/agent_policy.o $(OBJDIR)/server/model_sampling.o \
                             $(OBJDIR)/server/agent_tasks.o $(OBJDIR)/server/agent_eval.o $(OBJDIR)/server/agent_eval_memory_support.o $(OBJDIR)/server/agent_eval_baseline.o \
                             $(OBJDIR)/server/agent_coord.o $(OBJDIR)/server/agent_tools.o $(OBJDIR)/server/script_runner.o $(OBJDIR)/server/script_rpc.o $(OBJDIR)/toolset.o $(OBJDIR)/server/tool_args_coerce.o $(OBJDIR)/server/tool_schema_sanitizer.o \
                             $(OBJDIR)/server/kb_client.o $(OBJDIR)/server/kb_client_cache.o $(OBJDIR)/server/kb_client_index.o $(OBJDIR)/code_collect.o $(OBJDIR)/server/kb_client_index_parse.o $(OBJDIR)/server/kb_client_memory.o $(OBJDIR)/server/kb_client_memory_mutations.o $(OBJDIR)/server/kb_client_agent.o $(OBJDIR)/server/kb_client_dashboard.o $(OBJDIR)/server/kb_client_tasks.o $(OBJDIR)/server/kb_client_data.o $(OBJDIR)/tests/server/kb_client_tool_registry.o $(OBJDIR)/server/kb_client_prospective.o $(OBJDIR)/shared/kb_paths.o $(OBJDIR)/cli_client.o $(OBJDIR)/cli_v1_routes.o $(OBJDIR)/cli_v1_routes_b.o $(OBJDIR)/cli_v1_routes_c.o $(OBJDIR)/cli_v1_routes_d.o \
                             $(OBJDIR)/server/mcp_client.o $(OBJDIR)/server/mcp_client_registry.o \
                             $(OBJDIR)/server/http_retry.o $(OBJDIR)/server/failover.o \
                             $(OBJDIR)/posix/cli_client.o $(OBJDIR)/cli_v1_routes.o $(OBJDIR)/cli_v1_routes_b.o $(OBJDIR)/cli_v1_routes_c.o $(OBJDIR)/cli_v1_routes_d.o $(OBJDIR)/aimee_tls.o $(OBJDIR)/codex_auth.o $(OBJDIR)/posix/cli_main.o \
	                             $(OBJDIR)/guardrails.o $(OBJDIR)/guardrails_orchestrator.o $(OBJDIR)/guardrails_action_audit.o $(OBJDIR)/audit_action.o $(OBJDIR)/audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o $(OBJDIR)/guardrails_tdd.o $(OBJDIR)/guardrails_semantic.o $(OBJDIR)/guardrails_blast_radius.o $(OBJDIR)/skill.o $(OBJDIR)/session_state.o $(OBJDIR)/file_safety.o $(OBJDIR)/git_verify.o $(OBJDIR)/git_verify_state.o $(OBJDIR)/git_verify_config.o $(OBJDIR)/git_verify_jobs.o $(OBJDIR)/git_verify_hook.o $(OBJDIR)/git_verify_ops.o $(OBJDIR)/git_verify_select.o $(OBJDIR)/git_verify_step.o \
                             $(OBJDIR)/branch_ownership.o \
                             $(OBJDIR)/dstr.o $(OBJDIR)/diff.o \
                             $(OBJDIR)/server/web_search.o \
                             $(OBJDIR)/server/token_tracker.o \
                             $(OBJDIR)/server/process_mgr.o \
                             $(OBJDIR)/lsp_manager.o $(OBJDIR)/lsp_client.o \
                             $(OBJDIR)/server/model_provider.o $(OBJDIR)/server/openai_profile.o \
                             $(OBJDIR)/server/anthropic_profile.o $(OBJDIR)/server/gemini_profile.o \
                             $(OBJDIR)/server/openrouter_profile.o $(OBJDIR)/server/ollama_profile.o \
                             $(OBJDIR)/server/llama_native_profile.o $(OBJDIR)/server/mistral_profile.o \
                             $(OBJDIR)/server/minimax_profile.o \
                             $(OBJDIR)/model_registry.o $(OBJDIR)/models_dev.o $(OBJDIR)/models_dev_cache.o \
                             $(OBJDIR)/tests/support/delegate_child_env_export_stub.o \
                             $(OBJDIR)/tests/support/git_cred_inject_stub.o \
                             $(OBJDIR)/gateway_delegate.o $(OBJDIR)/gateway_pipeline.o $(OBJDIR)/gateway_policy.o \
                             $(PLATFORM_AGENT_OBJS)

TEST_MCP_CLIENT_OBJS = $(OBJDIR)/server/mcp_client.o \
                       $(OBJDIR)/sse_parser.o \
                       $(OBJDIR)/tests/support/mock_agent_http.o \
                       $(OBJDIR)/cJSON.o \
                       $(PLATFORM_BASIC_OBJS)

TEST_DATA_OBJS = $(TEST_CORE_OBJS) $(OBJDIR)/rel_types.o $(OBJDIR)/memory_fact_gate.o $(OBJDIR)/memory_extract_patterns.o $(OBJDIR)/db2/rel_types_store.o $(OBJDIR)/db2/entity_registry.o $(OBJDIR)/db2/fact_lifecycle.o $(OBJDIR)/db2/ontology_evolution.o $(OBJDIR)/db2/fact_ingest.o $(OBJDIR)/db2/fact_recall.o $(OBJDIR)/memory_pii_gate.o $(OBJDIR)/learning_router.o $(OBJDIR)/learning_implicit.o $(OBJDIR)/dogfood.o $(OBJDIR)/working_profile.o $(OBJDIR)/integrity_gate.o \
                 $(OBJDIR)/memory_core.o $(OBJDIR)/memory_core_crud.o $(OBJDIR)/memory_core_helpers.o $(OBJDIR)/memory_core_helpers_b.o $(OBJDIR)/memory_core_search.o $(OBJDIR)/memory_core_search_b.o $(OBJDIR)/memory_core_search_c.o $(OBJDIR)/memory_core_scope_embed.o $(OBJDIR)/memory_core_tiers.o $(OBJDIR)/db2/kb_payload.o $(OBJDIR)/db2/memory_lifecycle.o $(OBJDIR)/db2/memory_payload.o $(OBJDIR)/db2/memory_promotion.o $(OBJDIR)/db2/memory_query.o $(OBJDIR)/db2/memory_query_bookkeeping.o $(OBJDIR)/db2/memory_entity_graph.o $(OBJDIR)/db2/memory_score_fields.o $(OBJDIR)/db2/memory_scope_query.o $(OBJDIR)/db2/memory_scenes.o $(OBJDIR)/db2/memory_briefing.o $(OBJDIR)/db2/memory_health.o $(OBJDIR)/db2/memory_row_mapper_pg.o $(OBJDIR)/db2/memory_relations.o $(OBJDIR)/db2/memory_conflicts.o $(OBJDIR)/db2/vector_index_ops.o $(OBJDIR)/db2/code_index_ops.o $(OBJDIR)/tests/support/memory_embed_stub.o $(OBJDIR)/posix/memory.o \
                 $(OBJDIR)/memory_logic.o $(OBJDIR)/memory_effective.o $(OBJDIR)/memory_health.o $(OBJDIR)/memory_conflict.o $(OBJDIR)/memory_context.o $(OBJDIR)/memory_assemble.o $(OBJDIR)/memory_advanced.o $(OBJDIR)/memory_prospective.o $(OBJDIR)/memory_lifecycle.o $(OBJDIR)/memory_directives.o $(OBJDIR)/memory_maintenance.o $(OBJDIR)/memory_graph.o $(OBJDIR)/memory_graph_fusion.o $(OBJDIR)/memory_scan.o $(OBJDIR)/memory_improve.o $(OBJDIR)/memory_episodes.o \
                 $(OBJDIR)/workflow_learn.o \
                 $(OBJDIR)/index.o $(OBJDIR)/css_analyze.o $(OBJDIR)/db2/css_graph.o $(OBJDIR)/extractors.o $(OBJDIR)/extractors_extra.o $(OBJDIR)/extractors_new_langs.o $(OBJDIR)/code_treesitter.o \
                 $(OBJDIR)/tasks.o $(OBJDIR)/render.o \
                 $(DB1_OBJS) $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o $(OBJDIR)/db2/agent_hints.o $(OBJDIR)/db2/agent_outcomes.o $(OBJDIR)/db2/anti_patterns.o $(OBJDIR)/db2/collab_rules.o $(OBJDIR)/db2/curiosity.o $(OBJDIR)/db2/decision_log.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/db2/entity_edges.o $(OBJDIR)/db2/entity_nodes.o $(OBJDIR)/db2/code_projection.o $(OBJDIR)/db2/shadow_delta.o $(OBJDIR)/server/kb_client_code_embed.o $(OBJDIR)/db2/entity_profiles.o $(OBJDIR)/db2/epistemic_directives.o $(OBJDIR)/db2/failed_queries.o $(OBJDIR)/db2/feedback.o $(OBJDIR)/db2/memory_export.o $(OBJDIR)/db2/notes.o $(OBJDIR)/db2/prospective_memories.o $(OBJDIR)/db2/rules.o $(OBJDIR)/db2/tasks.o $(OBJDIR)/db2/tool_registry.o $(OBJDIR)/db2/trace_mining.o $(OBJDIR)/db2/kind_lifecycle.o $(OBJDIR)/db2/kb_runtime_state.o $(OBJDIR)/db2/kb_service_backend.o $(OBJDIR)/db2/kb_service_backend_ingest.o $(OBJDIR)/db2/memory_scenes.o $(OBJDIR)/db2/learning.o $(OBJDIR)/db2/code_index.o $(OBJDIR)/db2/sketch.o $(OBJDIR)/db2/pgvec_transport.o $(OBJDIR)/db2/memory_vectors.o $(OBJDIR)/db2/kb_vectors.o $(OBJDIR)/db2/vector_status.o $(OBJDIR)/db2/pgvec_verify.o $(OBJDIR)/db2/pgvec_kb_service.o $(OBJDIR)/kb/kb.o $(OBJDIR)/kb/kb_fusion.o $(OBJDIR)/kb/kb_neardup.o $(OBJDIR)/kb/kb_conventions.o $(OBJDIR)/sketch.o \
                 $(OBJDIR)/workspace.o \
                 $(OBJDIR)/learning_evidence.o $(OBJDIR)/db2/learning_synth_ops.o \
                 $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o $(OBJDIR)/db2/demotion.o $(OBJDIR)/db2/calibration.o \
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

TEST_TARGETS := $(TESTPREFIX)/unit-test-util $(TESTPREFIX)/unit-test-db $(TESTPREFIX)/unit-test-harness-memory $(TESTPREFIX)/unit-test-memory-redirect $(TESTPREFIX)/unit-test-harness-memory-scope $(TESTPREFIX)/unit-test-harness-memory-spill $(TESTPREFIX)/unit-test-harness-memory-audit $(TESTPREFIX)/unit-test-roundtable-brief $(TESTPREFIX)/unit-test-db2 $(TESTPREFIX)/unit-test-schema-subst $(TESTPREFIX)/unit-test-code-index-ops $(TESTPREFIX)/unit-test-curator-version $(TESTPREFIX)/unit-test-curator-invalidate $(TESTPREFIX)/unit-test-curator-notify $(TESTPREFIX)/unit-test-curator-queue $(TESTPREFIX)/unit-test-curator-pipeline-sched $(TESTPREFIX)/unit-test-curator-custom-stages $(TESTPREFIX)/unit-test-pgvec $(TESTPREFIX)/unit-test-rules \
               $(TESTPREFIX)/unit-test-guardrails $(TESTPREFIX)/unit-test-memory $(TESTPREFIX)/unit-test-tasks \
               $(TESTPREFIX)/unit-test-cmd-hooks-scope \
               $(TESTPREFIX)/unit-test-agent $(TESTPREFIX)/unit-test-agent-repair $(TESTPREFIX)/unit-test-agent-apikey $(TESTPREFIX)/unit-test-script-runner $(TESTPREFIX)/unit-test-provider-cli-adapter $(TESTPREFIX)/unit-test-cli-acp $(TESTPREFIX)/unit-test-acp-server $(TESTPREFIX)/unit-test-workspace-provider-container $(TESTPREFIX)/unit-test-mcp-native-surface $(TESTPREFIX)/unit-test-mcp-native-dispatch $(TESTPREFIX)/unit-test-extractors \
               $(TESTPREFIX)/unit-test-text $(TESTPREFIX)/unit-test-config $(TESTPREFIX)/unit-test-roundtable-preset $(TESTPREFIX)/unit-test-roundtable-seat-resolve $(TESTPREFIX)/unit-test-audit-worm $(TESTPREFIX)/unit-test-kb-audit-worm $(TESTPREFIX)/unit-test-config-economizer $(TESTPREFIX)/unit-test-config-snapshot $(TESTPREFIX)/unit-test-msg-session-disable $(TESTPREFIX)/unit-test-gateway-mutate $(TESTPREFIX)/unit-test-gateway-mutate-wire $(TESTPREFIX)/unit-test-config-surface $(TESTPREFIX)/unit-test-tool-condense $(TESTPREFIX)/unit-test-tool-output-cap $(TESTPREFIX)/unit-test-ingress-preinject $(TESTPREFIX)/unit-test-code-span $(TESTPREFIX)/unit-test-code-match $(TESTPREFIX)/unit-test-gw-stage-memory $(TESTPREFIX)/unit-test-attention-guard $(TESTPREFIX)/unit-test-codex-auth $(TESTPREFIX)/unit-test-code-audit $(TESTPREFIX)/unit-test-code-audit-graph $(TESTPREFIX)/unit-test-db2-code-audit $(TESTPREFIX)/unit-test-cron-config $(TESTPREFIX)/unit-test-cron-runtime $(TESTPREFIX)/unit-test-feedback \
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
               $(TESTPREFIX)/unit-test-evidence-replay $(TESTPREFIX)/unit-test-git-pr-ci-grade $(TESTPREFIX)/unit-test-roundtable-verify $(TESTPREFIX)/unit-test-sweep-logic $(TESTPREFIX)/unit-test-sweep-scope $(TESTPREFIX)/unit-test-sweep-parse \
               $(TESTPREFIX)/unit-test-css-analyze $(TESTPREFIX)/unit-test-typed-facts $(TESTPREFIX)/unit-test-css-graph $(TESTPREFIX)/unit-test-css-insights $(TESTPREFIX)/unit-test-css-oracle $(TESTPREFIX)/unit-test-css-render-oracle $(TESTPREFIX)/unit-test-css-migration $(TESTPREFIX)/unit-test-css-render $(TESTPREFIX)/unit-test-css-render-cmd \
               $(TESTPREFIX)/unit-test-compute-pool $(TESTPREFIX)/unit-test-db2-pool $(TESTPREFIX)/unit-test-cli-launch \
               $(TESTPREFIX)/unit-test-server-session-pools \
               $(TESTPREFIX)/unit-test-presence \
               $(TESTPREFIX)/unit-test-cli-provider \
               $(TESTPREFIX)/unit-test-context-assembly $(TESTPREFIX)/unit-test-workspace-memory \
               $(TESTPREFIX)/unit-test-dashboard \
               $(TESTPREFIX)/unit-test-log $(TESTPREFIX)/unit-test-server-dispatch \
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
               $(TESTPREFIX)/unit-test-wfe-random-delegate \
               $(TESTPREFIX)/unit-test-wfe-gate-reject \
               $(TESTPREFIX)/unit-test-wfe-gate-apply \
               $(TESTPREFIX)/unit-test-wfe-submitter \
               $(TESTPREFIX)/unit-test-wfe-manager-blocks \
               $(TESTPREFIX)/unit-test-wfe-manager-artifacts \
               $(TESTPREFIX)/unit-test-wfe-externalization \
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
               $(TESTPREFIX)/unit-test-aimee-ir-metrics \
               $(TESTPREFIX)/unit-test-aimee-frontend \
               $(TESTPREFIX)/unit-test-aimee-backend \
               $(TESTPREFIX)/unit-test-aimee-ir-shadow \
               $(TESTPREFIX)/unit-test-aimee-ir-serve \
               $(TESTPREFIX)/unit-test-aimee-ir-stream \
               $(TESTPREFIX)/unit-test-workflow-gate-caps \
               $(TESTPREFIX)/unit-test-wfe-webapi \
               $(TESTPREFIX)/unit-test-cli-profile \
               $(TESTPREFIX)/unit-test-cmd-profile \
               $(TESTPREFIX)/unit-test-kb-client-index \
               $(TESTPREFIX)/unit-test-kb-client-index-remote \
               $(TESTPREFIX)/unit-test-kb-client-docs \
               $(TESTPREFIX)/unit-test-kb-client-search \
               $(TESTPREFIX)/unit-test-kb-client-memory \
               $(TESTPREFIX)/unit-test-kb-graph \
               $(TESTPREFIX)/unit-test-kb-rrf \
               $(TESTPREFIX)/unit-test-kb-graph-analytics $(TESTPREFIX)/unit-test-lessons-cite-tracker $(TESTPREFIX)/unit-test-lessons-reflect $(TESTPREFIX)/unit-test-lessons-actuate $(TESTPREFIX)/unit-test-lessons-session-capture $(TESTPREFIX)/unit-test-kb-doc-hash \
               $(TESTPREFIX)/unit-test-prompt-sanitizer \
               $(TESTPREFIX)/unit-test-guardrails-blast-radius \
               $(TESTPREFIX)/unit-test-code-collect \
               $(TESTPREFIX)/unit-test-server-compute \
               $(TESTPREFIX)/unit-test-server-memory-benchmark \
               $(TESTPREFIX)/unit-test-server-jobs-aux \
               $(TESTPREFIX)/unit-test-compute-concurrency \
               $(TESTPREFIX)/unit-test-provider-catalog \
               $(TESTPREFIX)/unit-test-trace-analysis \
               $(TESTPREFIX)/unit-test-cmd-branch \
               $(TESTPREFIX)/unit-test-cmd-core $(TESTPREFIX)/unit-test-cmd-work \
               $(TESTPREFIX)/unit-test-client-integrations $(TESTPREFIX)/unit-test-mcp-git \
               $(TESTPREFIX)/unit-test-git-verify-select \
               $(TESTPREFIX)/unit-test-git-verify-contract \
               $(TESTPREFIX)/unit-test-cli-mcp-serve \
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
               $(TESTPREFIX)/unit-test-cli-http-transport \
               $(TESTPREFIX)/unit-test-delegate-xml-fallback \
               $(TESTPREFIX)/unit-test-http-retry \
               $(TESTPREFIX)/unit-test-cmd-doctor \
               $(TESTPREFIX)/unit-test-diff \
               $(TESTPREFIX)/unit-test-workspace-provider \
               $(TESTPREFIX)/unit-test-workspace-handle \
               $(TESTPREFIX)/unit-test-forge-credentials \
               $(TESTPREFIX)/unit-test-forge-app-token \
               $(TESTPREFIX)/unit-test-workspace-mirror \
               $(TESTPREFIX)/unit-test-workspace-provider-detached \
               $(TESTPREFIX)/unit-test-workspace-runner-queue \
               $(TESTPREFIX)/unit-test-workspace-scope \
               $(TESTPREFIX)/unit-test-webuser-runtime \
               $(TESTPREFIX)/unit-test-workspace-runner-registry \
               $(TESTPREFIX)/unit-test-workspace-turn \
               $(TESTPREFIX)/unit-test-notes \
               $(TESTPREFIX)/unit-test-cmd-cancel \
               $(TESTPREFIX)/unit-test-cmd-delegate \
               $(TESTPREFIX)/unit-test-delegate-plan \
               $(TESTPREFIX)/unit-test-delegate-role \
               $(TESTPREFIX)/unit-test-sse-parser \
               $(TESTPREFIX)/unit-test-anthropic-ingress \
               $(TESTPREFIX)/unit-test-anthropic-http \
               $(TESTPREFIX)/unit-test-anthropic-http-p2c \
               $(TESTPREFIX)/unit-test-anthropic-http-streaming-p2c \
               $(TESTPREFIX)/unit-test-gateway-policy \
               $(TESTPREFIX)/unit-test-gateway-pipeline \
               $(TESTPREFIX)/unit-test-gateway-p4-delegate \
               $(TESTPREFIX)/unit-test-hud \
               $(TESTPREFIX)/unit-test-coord-jobs \
               $(TESTPREFIX)/unit-test-plan-waves \
               $(TESTPREFIX)/unit-test-history \
               $(TESTPREFIX)/unit-test-events \
               $(TESTPREFIX)/unit-test-file-ref \
               $(TESTPREFIX)/unit-test-role-templates \
               $(TESTPREFIX)/unit-test-skill \
               $(TESTPREFIX)/unit-test-web-search \
               $(TESTPREFIX)/unit-test-tdd \
               $(TESTPREFIX)/unit-test-compact \
               $(TESTPREFIX)/unit-test-coord-closet \
               $(TESTPREFIX)/unit-test-fold-budget \
               $(TESTPREFIX)/unit-test-context-fold \
               $(TESTPREFIX)/unit-test-fold-register \
               $(TESTPREFIX)/unit-test-fold-recall \
               $(TESTPREFIX)/unit-test-task-rail \
               $(TESTPREFIX)/unit-test-episode-seal \
               $(TESTPREFIX)/unit-test-token-audit \
               $(TESTPREFIX)/unit-test-token-audit-load \
               $(TESTPREFIX)/unit-test-windows \
               $(TESTPREFIX)/unit-test-token-tracker \
               $(TESTPREFIX)/unit-test-context-reduce \
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
               $(TESTPREFIX)/unit-test-memory-fact-gate \
               $(TESTPREFIX)/unit-test-memory-embed-dim-guard \
               $(TESTPREFIX)/unit-test-rel-types-store \
               $(TESTPREFIX)/unit-test-entity-registry \
               $(TESTPREFIX)/unit-test-fact-lifecycle \
               $(TESTPREFIX)/unit-test-embedding-dim \
               $(TESTPREFIX)/unit-test-ontology-evolution \
               $(TESTPREFIX)/unit-test-extract-patterns \
               $(TESTPREFIX)/unit-test-fact-ingest $(TESTPREFIX)/unit-test-decision-log \
               $(TESTPREFIX)/unit-test-fact-recall \
               $(TESTPREFIX)/unit-test-pii-gate \
               $(TESTPREFIX)/unit-test-sandbox \
               $(TESTPREFIX)/unit-test-slop-detect \
               $(TESTPREFIX)/unit-test-vault-principal \
               $(TESTPREFIX)/unit-test-vault-crypto \
               $(TESTPREFIX)/unit-test-vault-kek-cache \
               $(TESTPREFIX)/unit-test-vault-store \
               $(TESTPREFIX)/unit-test-vault-service \
               $(TESTPREFIX)/unit-test-vault-master-rotate \
               $(TESTPREFIX)/unit-test-git-forge-vault \
               $(TESTPREFIX)/unit-test-git-host-resolve \
               $(TESTPREFIX)/unit-test-git-cred-inject \
               $(TESTPREFIX)/unit-test-git-ssh-agent \
               $(TESTPREFIX)/unit-test-webchat-git-leak \
               $(TESTPREFIX)/unit-test-git-project \
               $(TESTPREFIX)/unit-test-git-ops \
               $(TESTPREFIX)/unit-test-webuser-editor \
               $(TESTPREFIX)/unit-test-vault-bootstrap \
               $(TESTPREFIX)/unit-test-pki \
               $(TESTPREFIX)/unit-test-aimee-tls-clientcert \
               $(TESTPREFIX)/unit-test-aimee-tls-pin \
               $(TESTPREFIX)/unit-test-vault-server-key \
               $(TESTPREFIX)/unit-test-vault-capability \
               $(TESTPREFIX)/unit-test-agent-key-import \
               $(TESTPREFIX)/unit-test-vault-audit \
               $(TESTPREFIX)/unit-test-prompts \
               $(TESTPREFIX)/unit-test-cmd-session \
               $(TESTPREFIX)/unit-test-model-registry \
               $(TESTPREFIX)/unit-test-models-dev \
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
               $(TESTPREFIX)/unit-test-delegate-backend-local \
               $(TESTPREFIX)/unit-test-delegate-backend-ssh \
               $(TESTPREFIX)/unit-test-delegate-backend-docker \
               $(TESTPREFIX)/unit-test-session-compact \
               $(TESTPREFIX)/unit-test-session-compact-focused \
               $(TESTPREFIX)/unit-test-compact-prune \
               $(TESTPREFIX)/unit-test-otel \
               $(TESTPREFIX)/unit-test-clarify \
               $(TESTPREFIX)/unit-test-collab-rules \
               $(TESTPREFIX)/unit-test-oauth-pkce \
               $(TESTPREFIX)/unit-test-oauth-reauth \
               $(TESTPREFIX)/unit-test-mcp-client \
               $(TESTPREFIX)/unit-test-mcp-client-sse \
               $(TESTPREFIX)/unit-test-mcp-client-integration \
               $(TESTPREFIX)/unit-test-mcp-client-registry \
               $(TESTPREFIX)/unit-test-osv-check \
               $(TESTPREFIX)/unit-test-mcp-osv-cache \
               $(TESTPREFIX)/unit-test-plugin \
               $(TESTPREFIX)/unit-test-plugin-loader \
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
               $(TESTPREFIX)/unit-test-memory-recall-pivot \
               $(TESTPREFIX)/unit-test-memory-filter \
               $(TESTPREFIX)/unit-test-memory-profiles \
               $(TESTPREFIX)/unit-test-wiki-render \
               $(TESTPREFIX)/unit-test-integrity-gate \
               $(TESTPREFIX)/unit-test-conversation-context \
               $(TESTPREFIX)/unit-test-payload-rewrite \
               $(TESTPREFIX)/unit-test-payload-rewrite-state \
               $(TESTPREFIX)/unit-test-guardrails-semantic \
               $(TESTPREFIX)/unit-test-guardrails-computer-use \
               $(TESTPREFIX)/unit-test-kb-http-routes \
               $(TESTPREFIX)/unit-test-kb-scope \
               $(TESTPREFIX)/unit-test-kb-route-acl \
               $(TESTPREFIX)/unit-test-kb-enroll \
               $(TESTPREFIX)/unit-test-kb-verifier \
               $(TESTPREFIX)/unit-test-kb-auth-oidc \
               $(TESTPREFIX)/unit-test-kb-pki \
               $(TESTPREFIX)/unit-test-kb-tls \
               $(TESTPREFIX)/unit-test-kb-releases-db \
               $(TESTPREFIX)/unit-test-kb-ingest-format \
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
               $(TESTPREFIX)/unit-test-server-delegate-monitor \
               $(TESTPREFIX)/unit-test-db1-delegation-recursive-cancel \
               $(TESTPREFIX)/unit-test-tool-args-coerce \
               $(TESTPREFIX)/unit-test-tool-schema-sanitizer \
               $(TESTPREFIX)/unit-test-toolset \
               $(TESTPREFIX)/unit-test-db1-cost-fold \
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
               $(TESTPREFIX)/unit-test-substrate-fixtures
unit-tests: $(BINARY) $(TEST_TARGETS)
	@jobs="$(TEST_RUN_JOBS)"; \
	if [ "$$jobs" -le 1 ]; then \
	  for t in $(TEST_TARGETS); do \
	    log="$$(mktemp /tmp/aimee-test-run.XXXXXX)"; \
	    echo "  $$t"; \
	    "./$$t" >"$$log" 2>&1; \
	    rc="$$?"; \
	    cat "$$log"; \
	    rm -f "$$log"; \
	    [ "$$rc" -eq 0 ] || exit "$$rc"; \
	  done; \
	else \
	  printf '%s\0' $(TEST_TARGETS) | \
	    xargs -0 -n1 -P "$$jobs" sh -c 't="$$1"; log="$$(mktemp /tmp/aimee-test-run.XXXXXX)"; echo "  $$t"; "./$$t" >"$$log" 2>&1; rc="$$?"; cat "$$log"; rm -f "$$log"; if [ "$$rc" -ne 0 ]; then echo "FAILED: $$t" >&2; fi; exit "$$rc"' _ || exit 1; \
	fi
	@echo "All tests passed."

$(TESTPREFIX)/unit-test-util: $(OBJDIR)/tests/test_util.o $(OBJDIR)/util.o $(OBJDIR)/text.o \
                     $(OBJDIR)/cJSON.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-db: $(OBJDIR)/tests/test_db.o $(OBJDIR)/db1/db.o $(OBJDIR)/db1/db_schema.o $(OBJDIR)/db2/db_schema.o $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db1_write.o $(OBJDIR)/db1/db1_trigger.o $(OBJDIR)/db1/db1_cron_jobs.o $(OBJDIR)/db1/model_catalog.o $(OBJDIR)/db1/maintenance.o $(OBJDIR)/db1/eval.o $(OBJDIR)/tests/aimee_pg_sqlite_shim.o $(OBJDIR)/db2/db2_test_shim.o $(OBJDIR)/config.o $(OBJDIR)/config_sections.o $(OBJDIR)/config_database.o $(OBJDIR)/config_learning.o $(OBJDIR)/config_memory.o $(OBJDIR)/config_charter.o $(OBJDIR)/config_trigger.o $(OBJDIR)/config_kb_maintenance.o $(OBJDIR)/config_kb_curator.o $(OBJDIR)/config_server_api.o $(OBJDIR)/config_skills.o $(OBJDIR)/config_save.o \
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

$(TESTPREFIX)/unit-test-memory-redirect: $(OBJDIR)/tests/test_memory_redirect.o $(OBJDIR)/memory_redirect.o $(OBJDIR)/harness_memory_scope.o $(OBJDIR)/aimee_home.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-harness-memory: $(OBJDIR)/tests/test_harness_memory.o $(OBJDIR)/db1/user_memory.o $(OBJDIR)/harness_memory_common.o $(OBJDIR)/db1/db.o $(OBJDIR)/db1/db_schema.o $(OBJDIR)/db2/db_schema.o $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db1_write.o $(OBJDIR)/db1/db1_trigger.o $(OBJDIR)/db1/db1_cron_jobs.o $(OBJDIR)/db1/model_catalog.o $(OBJDIR)/db1/maintenance.o $(OBJDIR)/db1/eval.o $(OBJDIR)/tests/aimee_pg_sqlite_shim.o $(OBJDIR)/db2/db2_test_shim.o $(OBJDIR)/config.o $(OBJDIR)/config_sections.o $(OBJDIR)/config_database.o $(OBJDIR)/config_learning.o $(OBJDIR)/config_memory.o $(OBJDIR)/config_charter.o $(OBJDIR)/config_trigger.o $(OBJDIR)/config_kb_maintenance.o $(OBJDIR)/config_kb_curator.o $(OBJDIR)/config_server_api.o $(OBJDIR)/config_skills.o $(OBJDIR)/config_save.o \
                    $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o \
                    $(OBJDIR)/util.o $(OBJDIR)/text.o $(OBJDIR)/platform_random.o \
                    $(OBJDIR)/log.o $(PLATFORM_BASIC_OBJS) $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-webchat-claude-sessions: $(OBJDIR)/tests/test_webchat_claude_sessions.o $(OBJDIR)/db1/webchat_claude_sessions.o $(OBJDIR)/db1/db.o $(OBJDIR)/db1/db_schema.o $(OBJDIR)/db2/db_schema.o $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db1_write.o $(OBJDIR)/db1/db1_trigger.o $(OBJDIR)/db1/db1_cron_jobs.o $(OBJDIR)/db1/model_catalog.o $(OBJDIR)/db1/maintenance.o $(OBJDIR)/db1/eval.o $(OBJDIR)/tests/aimee_pg_sqlite_shim.o $(OBJDIR)/db2/db2_test_shim.o $(OBJDIR)/config.o $(OBJDIR)/config_sections.o $(OBJDIR)/config_database.o $(OBJDIR)/config_learning.o $(OBJDIR)/config_memory.o $(OBJDIR)/config_charter.o $(OBJDIR)/config_trigger.o $(OBJDIR)/config_kb_maintenance.o $(OBJDIR)/config_kb_curator.o $(OBJDIR)/config_server_api.o $(OBJDIR)/config_skills.o $(OBJDIR)/config_save.o \
                    $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o \
                    $(OBJDIR)/util.o $(OBJDIR)/text.o $(OBJDIR)/platform_random.o \
                    $(OBJDIR)/log.o $(PLATFORM_BASIC_OBJS) $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-db2: $(OBJDIR)/tests/test_db2.o $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o \
                            $(OBJDIR)/db2/entity_edges.o
	$(TESTLINK) -o $@ $^ $(L_CORE)

$(TESTPREFIX)/unit-test-schema-subst: $(OBJDIR)/tests/test_schema_subst.o \
                                      $(OBJDIR)/db2/db_schema.o
	$(TESTLINK) -o $@ $^ $(L_CORE)

$(TESTPREFIX)/unit-test-code-index-ops: \
                                       $(OBJDIR)/tests/test_code_index_ops.o \
                                       $(OBJDIR)/db2/code_index_ops.o \
                                       $(OBJDIR)/db2/code_index.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

# Cross-repo dependency graph S3: DB stats layer over the sqlite shim (portable
# SQL). Links the db2 init/pool/schema + the shim core like code-index-ops.
$(TESTPREFIX)/unit-test-cross-repo-stats: \
                                       $(OBJDIR)/tests/test_cross_repo_stats.o \
                                       $(OBJDIR)/db2/cross_repo_stats.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o \
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
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o \
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
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-cross-repo-route: \
                                       $(OBJDIR)/tests/test_cross_repo_route.o \
                                       $(OBJDIR)/db2/cross_repo_route.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-cross-repo-build: \
                                       $(OBJDIR)/tests/test_cross_repo_build.o \
                                       $(OBJDIR)/db2/cross_repo_build.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o \
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
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

# S4b review queue + adjudication over the sqlite shim.
$(TESTPREFIX)/unit-test-cross-repo-review: \
                                       $(OBJDIR)/tests/test_cross_repo_review.o \
                                       $(OBJDIR)/db2/cross_repo_review.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

# auditable-correctness P3 fidelity storage substrate over the sqlite shim.
$(TESTPREFIX)/unit-test-fidelity: \
                                       $(OBJDIR)/tests/test_fidelity.o \
                                       $(OBJDIR)/db2/fidelity.o \
                                       $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

# auditable-correctness P3 fidelity-check eligibility (fail-closed gate). The
# helper only reads config_t fields, so it links standalone.
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
                                       $(OBJDIR)/css_analyze.o \
                                       $(OBJDIR)/db2/code_index.o \
                                       $(OBJDIR)/db2/cross_repo_resolver.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

# CSS analysis signals (!important / specificity / unused vars / token candidates).
$(TESTPREFIX)/unit-test-css-insights: \
                                       $(OBJDIR)/tests/test_css_insights.o \
                                       $(OBJDIR)/db2/css_insights.o \
                                       $(OBJDIR)/db2/css_graph.o \
                                       $(OBJDIR)/db2/kb_runtime_state.o \
                                       $(OBJDIR)/css_analyze.o \
                                       $(OBJDIR)/db2/code_index.o \
                                       $(OBJDIR)/db2/cross_repo_resolver.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

# Typed-fact store + write gate over the sqlite shim.
$(TESTPREFIX)/unit-test-typed-facts: \
                                       $(OBJDIR)/tests/test_typed_facts.o \
                                       $(OBJDIR)/db2/typed_facts.o \
                                       $(OBJDIR)/db2/rel_types_store.o \
                                       $(OBJDIR)/db2/fact_recall.o \
                                       $(OBJDIR)/db2/entity_edges.o \
                                       $(OBJDIR)/db2/entity_registry.o \
                                       $(OBJDIR)/db2/ontology_evolution.o \
                                       $(OBJDIR)/db2/fact_lifecycle.o \
                                       $(OBJDIR)/memory_fact_gate.o \
                                       $(OBJDIR)/rel_types.o \
                                       $(OBJDIR)/memory_pii_gate.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o \
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
                                       $(OBJDIR)/css_analyze.o \
                                       $(OBJDIR)/db2/code_index.o \
                                       $(OBJDIR)/db2/cross_repo_resolver.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

# CSS rendered-oracle storage + evaluation (#4-full slice 2): db2/css_render.o
# + the pure css_render_oracle.o, over the migration-unit + graph spine.
$(TESTPREFIX)/unit-test-css-render: \
                                       $(OBJDIR)/tests/test_css_render.o \
                                       $(OBJDIR)/db2/css_render.o \
                                       $(OBJDIR)/css_render_oracle.o \
                                       $(OBJDIR)/db2/css_migration.o \
                                       $(OBJDIR)/db2/css_graph.o \
                                       $(OBJDIR)/db2/kb_runtime_state.o \
                                       $(OBJDIR)/db2/typed_facts.o \
                                       $(OBJDIR)/css_analyze.o \
                                       $(OBJDIR)/db2/code_index.o \
                                       $(OBJDIR)/db2/cross_repo_resolver.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-curator-version: \
                                       $(OBJDIR)/tests/test_curator_version.o \
                                       $(OBJDIR)/kb/kb_curator_version.o \
                                       $(OBJDIR)/db2/kb_runtime_state.o \
                                       $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                                       $(OBJDIR)/db2/kb_payload.o \
                                       $(OBJDIR)/db2/feature_rows.o \
                                       $(OBJDIR)/kb/kb_mdl.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-curator-invalidate: \
                                       $(OBJDIR)/tests/test_curator_invalidate.o \
                                       $(OBJDIR)/db2/kb_payload.o \
                                       $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                                       $(OBJDIR)/db2/feature_rows.o \
                                       $(OBJDIR)/kb/kb_mdl.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-curator-notify: $(OBJDIR)/tests/test_curator_notify.o $(OBJDIR)/kb/kb_curator_notify.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-curator-pipeline-sched: $(OBJDIR)/tests/test_curator_pipeline_sched.o $(OBJDIR)/kb/kb_curator_pipeline.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-curator-custom-stages: $(OBJDIR)/tests/test_curator_custom_stages.o $(OBJDIR)/kb/kb_curator_custom.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-curator-queue: \
                                       $(OBJDIR)/tests/test_curator_queue.o \
                                       $(OBJDIR)/kb/kb_curator_queue.o \
                                       $(OBJDIR)/kb/kb_curator_extract.o \
                                       $(OBJDIR)/kb/kb_memory_facts.o \
                                       $(OBJDIR)/kb/kb_curator_llm.o \
                                       $(OBJDIR)/kb/kb_curator_sidecar.o \
                                       $(OBJDIR)/kb_curator_provider.o \
                                       $(OBJDIR)/provider_client.o \
                                       $(OBJDIR)/tests/support/mock_agent_http.o \
                                       $(OBJDIR)/db2/typed_facts.o \
                                       $(OBJDIR)/db2/rel_types_store.o \
                                       $(OBJDIR)/db2/fact_recall.o \
                                       $(OBJDIR)/db2/fact_ingest.o \
                                       $(OBJDIR)/db2/fact_lifecycle.o \
                                       $(OBJDIR)/db2/entity_edges.o \
                                       $(OBJDIR)/db2/entity_registry.o \
                                       $(OBJDIR)/db2/ontology_evolution.o \
                                       $(OBJDIR)/db2/memory_query.o \
                                       $(OBJDIR)/db2/memory_row_mapper_pg.o \
                                       $(OBJDIR)/memory_extract_patterns.o \
                                       $(OBJDIR)/memory_fact_gate.o \
                                       $(OBJDIR)/memory_pii_gate.o \
                                       $(OBJDIR)/rel_types.o \
                                       $(OBJDIR)/index.o \
                                       $(OBJDIR)/db2/code_index.o \
                                       $(OBJDIR)/db2/kb_payload.o \
                                       $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                                       $(OBJDIR)/db2/feature_rows.o \
                                       $(OBJDIR)/kb/kb_mdl.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-pgvec: $(OBJDIR)/tests/test_pgvec.o \
                    $(OBJDIR)/db2/pgvec_transport.o $(OBJDIR)/db2/memory_vectors.o $(OBJDIR)/db2/kb_vectors.o \
                    $(OBJDIR)/db2/vector_status.o $(OBJDIR)/db2/pgvec_verify.o $(OBJDIR)/db2/pgvec_kb_service.o \
                    $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                    $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# memory/KB vector upsert dim guard (rejects builtin-384 vs halfvec(1024)/(2560)).
$(TESTPREFIX)/unit-test-memory-embed-dim-guard: $(OBJDIR)/tests/test_memory_embed_dim_guard.o \
                    $(OBJDIR)/db2/pgvec_transport.o $(OBJDIR)/db2/memory_vectors.o $(OBJDIR)/db2/kb_vectors.o \
                    $(OBJDIR)/db2/vector_status.o $(OBJDIR)/db2/pgvec_verify.o $(OBJDIR)/db2/pgvec_kb_service.o \
                    $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                    $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-rules: $(OBJDIR)/tests/test_rules.o $(DB1_OBJS) $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o $(OBJDIR)/db2/rules.o $(OBJDIR)/db2/stopwords.o $(OBJDIR)/db2/tool_registry.o $(OBJDIR)/db1/db.o $(OBJDIR)/db1/db_schema.o $(OBJDIR)/tests/aimee_pg_sqlite_shim.o $(OBJDIR)/db2/db2_test_shim.o \
                       $(OBJDIR)/config.o $(OBJDIR)/config_sections.o $(OBJDIR)/config_database.o $(OBJDIR)/config_learning.o $(OBJDIR)/config_memory.o $(OBJDIR)/config_charter.o $(OBJDIR)/config_trigger.o $(OBJDIR)/config_kb_maintenance.o $(OBJDIR)/config_kb_curator.o $(OBJDIR)/config_server_api.o $(OBJDIR)/config_skills.o $(OBJDIR)/config_save.o $(OBJDIR)/config_mode.o $(OBJDIR)/config_fields.o $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/util.o $(OBJDIR)/text.o \
                       $(OBJDIR)/platform_random.o $(OBJDIR)/log.o $(PLATFORM_BASIC_OBJS) $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-context-discover: $(OBJDIR)/tests/test_context_discover.o $(OBJDIR)/context_discover.o \
                       $(OBJDIR)/db1/db.o $(OBJDIR)/db1/db_schema.o $(OBJDIR)/tests/aimee_pg_sqlite_shim.o $(OBJDIR)/db2/db2_test_shim.o $(OBJDIR)/config.o $(OBJDIR)/config_sections.o $(OBJDIR)/config_database.o $(OBJDIR)/config_learning.o $(OBJDIR)/config_memory.o $(OBJDIR)/config_charter.o $(OBJDIR)/config_trigger.o $(OBJDIR)/config_kb_maintenance.o $(OBJDIR)/config_kb_curator.o $(OBJDIR)/config_server_api.o $(OBJDIR)/config_skills.o $(OBJDIR)/config_save.o $(OBJDIR)/config_mode.o $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o \
                       $(OBJDIR)/util.o $(OBJDIR)/text.o $(OBJDIR)/platform_random.o $(OBJDIR)/log.o \
                       $(PLATFORM_BASIC_OBJS) $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-cli-mcp-serve: $(OBJDIR)/tests/test_cli_mcp_serve.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

# aimee_client.o resolves the remote-target accessors cli_v1_client_endpoint now
# calls (it synthesizes a tcp: endpoint from a --server/AIMEE_SERVER_URL target).
# --gc-sections drops the rest of the transport, which this marshalling test
# never calls.
$(TESTPREFIX)/unit-test-cli-v1-delegate: $(OBJDIR)/tests/test_cli_v1_delegate.o \
                                  $(OBJDIR)/cJSON.o $(OBJDIR)/posix/util.o $(OBJDIR)/aimee_client.o \
                                  $(OBJDIR)/codex_auth.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-cli-server-compat: $(OBJDIR)/tests/test_cli_server_compat.o \
                                  $(OBJDIR)/cJSON.o $(OBJDIR)/posix/util.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)


$(TESTPREFIX)/unit-test-guardrails: $(OBJDIR)/tests/test_guardrails.o \
                            $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-cmd-hooks-scope: $(OBJDIR)/tests/test_cmd_hooks_scope.o \
                             $(OBJDIR)/cmd_hooks_scope.o $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-memory: $(OBJDIR)/tests/test_memory.o $(OBJDIR)/kb/kb_bandit.o $(OBJDIR)/kb/kb_bandit_registry.o $(OBJDIR)/db2/bandit.o $(OBJDIR)/db2/demotion.o $(OBJDIR)/memory_core.o $(OBJDIR)/memory_core_crud.o $(OBJDIR)/memory_core_helpers.o $(OBJDIR)/memory_core_helpers_b.o $(OBJDIR)/memory_core_search.o $(OBJDIR)/memory_core_search_b.o $(OBJDIR)/memory_core_search_c.o $(OBJDIR)/memory_core_scope_embed.o $(OBJDIR)/memory_core_tiers.o \
                        $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o $(OBJDIR)/db2/kb_payload.o $(OBJDIR)/db2/kb_service_backend.o $(OBJDIR)/db2/kb_service_backend_ingest.o $(OBJDIR)/db2/memory_lifecycle.o $(OBJDIR)/db2/memory_payload.o $(OBJDIR)/db2/memory_promotion.o $(OBJDIR)/db2/memory_query.o $(OBJDIR)/db2/memory_query_bookkeeping.o $(OBJDIR)/db2/memory_entity_graph.o $(OBJDIR)/db2/memory_score_fields.o $(OBJDIR)/db2/memory_scope_query.o $(OBJDIR)/db2/memory_scenes.o $(OBJDIR)/db2/memory_briefing.o $(OBJDIR)/db2/memory_health.o $(OBJDIR)/db2/memory_row_mapper_pg.o $(OBJDIR)/db2/memory_relations.o $(OBJDIR)/db2/memory_conflicts.o $(OBJDIR)/db2/vector_index_ops.o $(OBJDIR)/db2/code_index_ops.o $(OBJDIR)/db2/rules.o $(OBJDIR)/db2/stopwords.o $(OBJDIR)/db2/tool_registry.o $(OBJDIR)/db2/feedback.o $(OBJDIR)/db2/notes.o $(OBJDIR)/db2/anti_patterns.o $(OBJDIR)/db2/curiosity.o $(OBJDIR)/db2/entity_edges.o $(OBJDIR)/db2/entity_profiles.o $(OBJDIR)/db2/epistemic_directives.o $(OBJDIR)/db2/failed_queries.o $(OBJDIR)/db2/kind_lifecycle.o $(OBJDIR)/db2/calibration.o $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o $(OBJDIR)/db2/pgvec_transport.o $(OBJDIR)/db2/memory_vectors.o $(OBJDIR)/db2/kb_vectors.o $(OBJDIR)/db2/vector_status.o $(OBJDIR)/db2/pgvec_verify.o $(OBJDIR)/db2/pgvec_kb_service.o \
                        $(OBJDIR)/tests/support/mock_agent_http.o \
                        $(OBJDIR)/tests/support/memory_embed_stub.o $(OBJDIR)/tests/support/kb_client_test_stub.o \
                        $(OBJDIR)/posix/memory.o \
                        $(OBJDIR)/memory_logic.o $(OBJDIR)/memory_health.o $(OBJDIR)/memory_conflict.o $(OBJDIR)/memory_context.o $(OBJDIR)/memory_assemble.o \
                         \
                        $(OBJDIR)/memory_advanced.o $(OBJDIR)/memory_prospective.o $(OBJDIR)/memory_lifecycle.o $(OBJDIR)/memory_directives.o $(OBJDIR)/memory_maintenance.o $(OBJDIR)/memory_graph.o $(OBJDIR)/memory_graph_fusion.o $(OBJDIR)/memory_scan.o $(OBJDIR)/memory_improve.o $(OBJDIR)/memory_episodes.o $(OBJDIR)/db1/db.o $(OBJDIR)/db1/db_schema.o $(OBJDIR)/tests/aimee_pg_sqlite_shim.o $(OBJDIR)/db2/db2_test_shim.o $(OBJDIR)/config.o $(OBJDIR)/config_sections.o $(OBJDIR)/config_database.o $(OBJDIR)/config_learning.o $(OBJDIR)/config_memory.o $(OBJDIR)/config_charter.o $(OBJDIR)/config_trigger.o $(OBJDIR)/config_kb_maintenance.o $(OBJDIR)/config_kb_curator.o $(OBJDIR)/config_server_api.o $(OBJDIR)/config_skills.o $(OBJDIR)/config_save.o \
                        $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o \
                        $(OBJDIR)/util.o $(OBJDIR)/text.o $(OBJDIR)/platform_random.o \
                        $(OBJDIR)/aimee_home.o \
                        $(OBJDIR)/log.o $(PLATFORM_BASIC_OBJS) \
                        $(OBJDIR)/tests/support/learning_implicit_stub.o $(OBJDIR)/dogfood.o $(OBJDIR)/working_profile.o \
                        $(OBJDIR)/tasks.o $(OBJDIR)/index.o $(OBJDIR)/css_analyze.o $(OBJDIR)/db2/css_graph.o $(OBJDIR)/extractors.o \
                        $(OBJDIR)/extractors_extra.o $(OBJDIR)/extractors_new_langs.o $(OBJDIR)/code_treesitter.o \
                        $(OBJDIR)/kb/kb.o $(OBJDIR)/kb/kb_neardup.o $(OBJDIR)/kb/kb_conventions.o $(OBJDIR)/kb/kb_mdl.o \
                        $(OBJDIR)/db2/feature_rows.o \
                        $(OBJDIR)/workspace.o $(OBJDIR)/util_url.o $(OBJDIR)/report_enrichment.o $(DB1_OBJS) \
                        $(OBJDIR)/render.o $(OBJDIR)/json_fluent.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-tasks: $(OBJDIR)/tests/test_tasks.o $(OBJDIR)/tasks.o $(OBJDIR)/db1/db.o $(OBJDIR)/db1/db_schema.o $(OBJDIR)/tests/aimee_pg_sqlite_shim.o $(OBJDIR)/db2/db2_test_shim.o \
                       $(OBJDIR)/config.o $(OBJDIR)/config_sections.o $(OBJDIR)/config_database.o $(OBJDIR)/config_learning.o $(OBJDIR)/config_memory.o $(OBJDIR)/config_charter.o $(OBJDIR)/config_trigger.o $(OBJDIR)/config_kb_maintenance.o $(OBJDIR)/config_kb_curator.o $(OBJDIR)/config_server_api.o $(OBJDIR)/config_skills.o $(OBJDIR)/config_save.o $(OBJDIR)/config_mode.o $(OBJDIR)/config_fields.o $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/util.o $(OBJDIR)/text.o \
                       $(OBJDIR)/aimee_home.o \
                       $(OBJDIR)/platform_random.o $(OBJDIR)/log.o $(PLATFORM_BASIC_OBJS) \
                       $(OBJDIR)/memory_core.o $(OBJDIR)/memory_core_crud.o $(OBJDIR)/memory_core_helpers.o $(OBJDIR)/memory_core_helpers_b.o $(OBJDIR)/memory_core_search.o $(OBJDIR)/memory_core_search_b.o $(OBJDIR)/memory_core_search_c.o $(OBJDIR)/memory_core_scope_embed.o $(OBJDIR)/memory_core_tiers.o $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o $(OBJDIR)/db2/kb_payload.o $(OBJDIR)/db2/kb_service_backend.o $(OBJDIR)/db2/kb_service_backend_ingest.o $(OBJDIR)/db2/memory_lifecycle.o $(OBJDIR)/db2/memory_payload.o $(OBJDIR)/db2/memory_promotion.o $(OBJDIR)/db2/memory_query.o $(OBJDIR)/db2/memory_query_bookkeeping.o $(OBJDIR)/db2/memory_entity_graph.o $(OBJDIR)/db2/memory_score_fields.o $(OBJDIR)/db2/memory_scope_query.o $(OBJDIR)/db2/memory_scenes.o $(OBJDIR)/db2/memory_briefing.o $(OBJDIR)/db2/memory_health.o $(OBJDIR)/db2/memory_row_mapper_pg.o $(OBJDIR)/db2/memory_relations.o $(OBJDIR)/db2/memory_conflicts.o $(OBJDIR)/db2/vector_index_ops.o $(OBJDIR)/db2/code_index_ops.o $(OBJDIR)/db2/rules.o $(OBJDIR)/db2/tasks.o $(OBJDIR)/db2/tool_registry.o $(OBJDIR)/db2/feedback.o $(OBJDIR)/db2/notes.o $(OBJDIR)/db2/anti_patterns.o $(OBJDIR)/db2/curiosity.o $(OBJDIR)/db2/decision_log.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/db2/pgvec_transport.o $(OBJDIR)/db2/memory_vectors.o $(OBJDIR)/db2/kb_vectors.o $(OBJDIR)/db2/vector_status.o $(OBJDIR)/db2/pgvec_verify.o $(OBJDIR)/db2/pgvec_kb_service.o $(OBJDIR)/tests/support/mock_agent_http.o $(OBJDIR)/tests/support/memory_embed_stub.o $(OBJDIR)/tests/support/kb_client_test_stub.o $(OBJDIR)/posix/memory.o \
                       $(OBJDIR)/memory_logic.o $(OBJDIR)/memory_health.o $(OBJDIR)/memory_conflict.o $(OBJDIR)/memory_context.o $(OBJDIR)/memory_assemble.o \
                        \
                       $(OBJDIR)/memory_advanced.o $(OBJDIR)/memory_prospective.o $(OBJDIR)/memory_lifecycle.o $(OBJDIR)/memory_directives.o $(OBJDIR)/memory_maintenance.o $(OBJDIR)/memory_graph.o $(OBJDIR)/memory_graph_fusion.o $(OBJDIR)/memory_scan.o $(OBJDIR)/memory_improve.o $(OBJDIR)/memory_episodes.o $(OBJDIR)/tests/support/learning_implicit_stub.o $(OBJDIR)/dogfood.o $(OBJDIR)/working_profile.o \
                       $(OBJDIR)/index.o $(OBJDIR)/css_analyze.o $(OBJDIR)/db2/css_graph.o $(OBJDIR)/extractors.o \
                       $(OBJDIR)/extractors_extra.o $(OBJDIR)/extractors_new_langs.o $(OBJDIR)/code_treesitter.o \
                       $(OBJDIR)/kb/kb_mdl.o $(OBJDIR)/db2/feature_rows.o \
                       $(OBJDIR)/render.o $(OBJDIR)/cJSON.o $(DB1_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-agent: $(OBJDIR)/tests/test_agent.o $(OBJDIR)/tests/test_agent_caps.o \
                      $(OBJDIR)/tests/test_agent_responses.o \
                      $(OBJDIR)/tests/test_agent_delegate_root.o $(OBJDIR)/server/agent_cli_shell.o \
                      $(OBJDIR)/audit_action.o $(OBJDIR)/audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                      $(OBJDIR)/server/tool_call_args.o \
                      $(OBJDIR)/server/session_compact.o $(OBJDIR)/server/compact_prune.o $(OBJDIR)/server/delegate_driver.o \
                      $(OBJDIR)/server/delegate_openai.o $(OBJDIR)/server/delegate_gemini.o \
                      $(OBJDIR)/server/delegate_xml_fallback.o $(OBJDIR)/server/delegate_role.o \
                      $(OBJDIR)/model_registry.o $(OBJDIR)/models_dev.o \
                      $(OBJDIR)/models_dev_cache.o $(OBJDIR)/payload_rewrite.o \
                      $(OBJDIR)/server/middleware.o $(OBJDIR)/server/liveness.o \
                      $(OBJDIR)/server/cli_session.o $(OBJDIR)/server/agent_policy_intercept.o \
                      $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# message_history_repair tests split out of test_agent.c (2000-line limit).
# Mirrors unit-test-agent's link line so message_history_repair (agent_bridge.o,
# pulled via the shared object set) and cJSON resolve; gc-sections drops the rest.
$(TESTPREFIX)/unit-test-agent-repair: $(OBJDIR)/tests/test_agent_repair.o \
                      $(OBJDIR)/server/agent_cli_shell.o \
                      $(OBJDIR)/server/tool_call_args.o \
                      $(OBJDIR)/server/session_compact.o $(OBJDIR)/server/compact_prune.o $(OBJDIR)/server/delegate_driver.o \
                      $(OBJDIR)/server/delegate_openai.o $(OBJDIR)/server/delegate_gemini.o \
                      $(OBJDIR)/server/delegate_xml_fallback.o $(OBJDIR)/server/delegate_role.o \
                      $(OBJDIR)/model_registry.o $(OBJDIR)/models_dev.o \
                      $(OBJDIR)/models_dev_cache.o $(OBJDIR)/payload_rewrite.o \
                      $(OBJDIR)/server/middleware.o $(OBJDIR)/server/liveness.o \
                      $(OBJDIR)/server/cli_session.o $(OBJDIR)/server/agent_policy_intercept.o \
                      $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# agents.json secret-serialization test split out of test_agent.c (2000-line
# limit). Mirrors unit-test-agent's link line.
$(TESTPREFIX)/unit-test-agent-apikey: $(OBJDIR)/tests/test_agent_apikey.o \
                      $(OBJDIR)/server/agent_cli_shell.o \
                      $(OBJDIR)/audit_action.o $(OBJDIR)/audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                      $(OBJDIR)/server/tool_call_args.o \
                      $(OBJDIR)/server/session_compact.o $(OBJDIR)/server/compact_prune.o $(OBJDIR)/server/delegate_driver.o \
                      $(OBJDIR)/server/delegate_openai.o $(OBJDIR)/server/delegate_gemini.o \
                      $(OBJDIR)/server/delegate_xml_fallback.o $(OBJDIR)/server/delegate_role.o \
                      $(OBJDIR)/model_registry.o $(OBJDIR)/models_dev.o \
                      $(OBJDIR)/models_dev_cache.o $(OBJDIR)/payload_rewrite.o \
                      $(OBJDIR)/server/middleware.o $(OBJDIR)/server/liveness.o \
                      $(OBJDIR)/server/cli_session.o $(OBJDIR)/server/agent_policy_intercept.o \
                      $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-script-runner: $(OBJDIR)/tests/test_script_runner.o \
                      $(OBJDIR)/server/script_runner.o $(OBJDIR)/server/script_rpc.o $(OBJDIR)/toolset.o \
                      $(OBJDIR)/platform_random.o $(OBJDIR)/posix/platform_random.o \
                      $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/log.o $(OBJDIR)/aimee_home.o \
                      $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-provider-cli-adapter: $(OBJDIR)/tests/test_provider_cli_adapter.o \
                      $(OBJDIR)/tests/support/delegate_child_env_export_stub.o \
                      $(OBJDIR)/tests/support/git_cred_inject_stub.o \
                      $(OBJDIR)/server/provider_cli_adapter.o $(OBJDIR)/server/cli_codex.o \
                      $(OBJDIR)/server/cli_claude.o $(OBJDIR)/server/cli_gemini.o $(OBJDIR)/server/cli_mistral.o \
                      $(OBJDIR)/server/cli_acp.o $(OBJDIR)/posix/workspace_provider.o \
                      $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-cli-acp: $(OBJDIR)/tests/test_cli_acp.o \
                      $(OBJDIR)/tests/support/delegate_child_env_export_stub.o \
                      $(OBJDIR)/tests/support/git_cred_inject_stub.o \
                      $(OBJDIR)/server/provider_cli_adapter.o $(OBJDIR)/server/cli_codex.o \
                      $(OBJDIR)/server/cli_claude.o $(OBJDIR)/server/cli_gemini.o $(OBJDIR)/server/cli_mistral.o \
                      $(OBJDIR)/server/cli_acp.o $(OBJDIR)/posix/workspace_provider.o \
                      $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-extractors: $(OBJDIR)/tests/test_extractors.o $(OBJDIR)/extractors.o \
                           $(OBJDIR)/extractors_extra.o $(OBJDIR)/extractors_new_langs.o $(OBJDIR)/code_treesitter.o $(OBJDIR)/index.o $(OBJDIR)/css_analyze.o $(OBJDIR)/db2/css_graph.o $(OBJDIR)/db1/db.o $(OBJDIR)/db1/db_schema.o $(OBJDIR)/tests/aimee_pg_sqlite_shim.o $(OBJDIR)/db2/db2_test_shim.o \
                           $(OBJDIR)/config.o $(OBJDIR)/config_sections.o $(OBJDIR)/config_database.o $(OBJDIR)/config_learning.o $(OBJDIR)/config_memory.o $(OBJDIR)/config_charter.o $(OBJDIR)/config_trigger.o $(OBJDIR)/config_kb_maintenance.o $(OBJDIR)/config_kb_curator.o $(OBJDIR)/config_server_api.o $(OBJDIR)/config_skills.o $(OBJDIR)/config_save.o $(OBJDIR)/config_mode.o $(OBJDIR)/config_fields.o $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/util.o $(OBJDIR)/text.o \
                           $(OBJDIR)/platform_random.o $(OBJDIR)/log.o $(PLATFORM_BASIC_OBJS) \
                           $(OBJDIR)/memory_core.o $(OBJDIR)/memory_core_crud.o $(OBJDIR)/memory_core_helpers.o $(OBJDIR)/memory_core_helpers_b.o $(OBJDIR)/memory_core_search.o $(OBJDIR)/memory_core_search_b.o $(OBJDIR)/memory_core_search_c.o $(OBJDIR)/memory_core_scope_embed.o $(OBJDIR)/memory_core_tiers.o $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o $(OBJDIR)/db2/kb_payload.o $(OBJDIR)/db2/kb_service_backend.o $(OBJDIR)/db2/kb_service_backend_ingest.o $(OBJDIR)/db2/memory_lifecycle.o $(OBJDIR)/db2/memory_payload.o $(OBJDIR)/db2/memory_promotion.o $(OBJDIR)/db2/memory_query.o $(OBJDIR)/db2/memory_query_bookkeeping.o $(OBJDIR)/db2/memory_entity_graph.o $(OBJDIR)/db2/memory_score_fields.o $(OBJDIR)/db2/memory_scope_query.o $(OBJDIR)/db2/memory_scenes.o $(OBJDIR)/db2/memory_briefing.o $(OBJDIR)/db2/memory_health.o $(OBJDIR)/db2/memory_row_mapper_pg.o $(OBJDIR)/db2/memory_relations.o $(OBJDIR)/db2/memory_conflicts.o $(OBJDIR)/db2/vector_index_ops.o $(OBJDIR)/db2/code_index_ops.o $(OBJDIR)/db2/rules.o $(OBJDIR)/db2/stopwords.o $(OBJDIR)/db2/tool_registry.o $(OBJDIR)/db2/feedback.o $(OBJDIR)/db2/notes.o $(OBJDIR)/db2/anti_patterns.o $(OBJDIR)/db2/curiosity.o $(OBJDIR)/db2/entity_edges.o $(OBJDIR)/db2/entity_profiles.o $(OBJDIR)/db2/epistemic_directives.o $(OBJDIR)/db2/failed_queries.o $(OBJDIR)/db2/kind_lifecycle.o $(OBJDIR)/db2/pgvec_transport.o $(OBJDIR)/db2/memory_vectors.o $(OBJDIR)/db2/kb_vectors.o $(OBJDIR)/db2/vector_status.o $(OBJDIR)/db2/pgvec_verify.o $(OBJDIR)/db2/pgvec_kb_service.o $(OBJDIR)/tests/support/mock_agent_http.o $(OBJDIR)/tests/support/memory_embed_stub.o $(OBJDIR)/tests/support/kb_client_test_stub.o $(OBJDIR)/posix/memory.o \
                           $(OBJDIR)/memory_logic.o $(OBJDIR)/memory_health.o $(OBJDIR)/memory_conflict.o $(OBJDIR)/memory_context.o $(OBJDIR)/memory_assemble.o \
                            \
                            $(OBJDIR)/memory_advanced.o $(OBJDIR)/memory_prospective.o $(OBJDIR)/memory_lifecycle.o $(OBJDIR)/memory_directives.o $(OBJDIR)/memory_maintenance.o $(OBJDIR)/memory_graph.o $(OBJDIR)/memory_graph_fusion.o $(OBJDIR)/memory_scan.o $(OBJDIR)/memory_improve.o $(OBJDIR)/memory_episodes.o \
                           $(OBJDIR)/tests/support/learning_implicit_stub.o $(OBJDIR)/dogfood.o $(OBJDIR)/working_profile.o $(OBJDIR)/tasks.o \
                           $(OBJDIR)/kb/kb_mdl.o $(OBJDIR)/db2/feature_rows.o \
                           $(OBJDIR)/render.o $(OBJDIR)/json_fluent.o $(OBJDIR)/cJSON.o $(DB1_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# --- New tests ---

# CSS analyzer (WP-A) — pure leaf, depends only on css_analyze.o + libc.
$(TESTPREFIX)/unit-test-css-analyze: $(OBJDIR)/tests/test_css_analyze.o $(OBJDIR)/css_analyze.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# CSS interim static oracle (WP-E) — leaf: css_oracle.o + css_analyze.o + libc.
$(TESTPREFIX)/unit-test-css-oracle: $(OBJDIR)/tests/test_css_oracle.o $(OBJDIR)/css_oracle.o $(OBJDIR)/css_analyze.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# CSS rendered computed-style oracle core (#4-full) — leaf: css_render_oracle.o + cJSON + libc.
$(TESTPREFIX)/unit-test-css-render-oracle: $(OBJDIR)/tests/test_css_render_oracle.o $(OBJDIR)/css_render_oracle.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# CSS command-driven render backend (#4-full slice 3) — adapter over platform_exec_pipe.
$(TESTPREFIX)/unit-test-css-render-cmd: $(OBJDIR)/tests/test_css_render_cmd.o \
                                       $(OBJDIR)/css_render_cmd.o \
                                       $(OBJDIR)/css_render_oracle.o \
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
                     $(OBJDIR)/server/gw_stage_memory.o $(OBJDIR)/server/ingress_preinject.o \
                     $(OBJDIR)/server/request_context.o $(OBJDIR)/log.o \
                     $(OBJDIR)/cJSON.o $(OBJDIR)/dstr.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-codex-auth: $(OBJDIR)/tests/test_codex_auth.o \
                     $(OBJDIR)/codex_auth.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-attention-guard: $(OBJDIR)/tests/test_attention_guard.o \
                     $(OBJDIR)/cli_attention_guard.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm

$(TESTPREFIX)/unit-test-code-audit: $(OBJDIR)/tests/test_code_audit.o \
                     $(OBJDIR)/cli_code_audit.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-code-audit-graph: $(OBJDIR)/tests/test_code_audit_graph.o \
                     $(OBJDIR)/code_audit_graph.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-db2-code-audit: $(OBJDIR)/tests/test_db2_code_audit.o \
                     $(OBJDIR)/db2/code_audit.o $(OBJDIR)/db2/entity_nodes.o \
                     $(OBJDIR)/code_audit_graph.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-server-memory-benchmark: \
                     $(OBJDIR)/tests/test_server_memory_benchmark.o \
                     $(OBJDIR)/server/server_memory_benchmark.o \
                     $(OBJDIR)/json_fluent.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm

$(TESTPREFIX)/unit-test-config: $(OBJDIR)/tests/test_config.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-roundtable-preset: $(OBJDIR)/tests/test_roundtable_preset.o $(OBJDIR)/roundtable_preset.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-roundtable-seat-resolve: $(OBJDIR)/tests/test_roundtable_seat_resolve.o \
                      $(OBJDIR)/server/roundtable_seat_resolve.o \
                      $(OBJDIR)/server/agent_config.o \
                      $(OBJDIR)/tests/support/vault_service_stub.o \
                      $(OBJDIR)/tests/support/oauth_tokens_stub.o \
                      $(OBJDIR)/tests/support/provider_cli_adapter_stub.o \
                      $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-audit-worm: $(OBJDIR)/tests/test_audit_worm.o $(OBJDIR)/audit_worm.o $(OBJDIR)/audit_worm_chain.o \
                      $(OBJDIR)/workflow/wfe_canonical.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-config-economizer: $(OBJDIR)/tests/test_config_economizer.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-config-snapshot: $(OBJDIR)/tests/test_config_snapshot.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-msg-session-disable: $(OBJDIR)/tests/test_msg_session_disable.o \
                     $(OBJDIR)/server/msg_session_disable.o $(OBJDIR)/server/gw_mutate_stats.o \
                     $(OBJDIR)/harness_memory_common.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lpthread

$(TESTPREFIX)/unit-test-gateway-mutate: $(OBJDIR)/tests/test_gateway_mutate.o \
                     $(OBJDIR)/server/gateway_mutate.o $(OBJDIR)/server/agent_bridge.o \
                     $(OBJDIR)/server/session_compact.o $(OBJDIR)/server/tool_call_args.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm -lpthread

$(TESTPREFIX)/unit-test-gateway-mutate-wire: $(OBJDIR)/tests/test_gateway_mutate_wire.o \
                     $(OBJDIR)/server/gateway_mutate_wire.o $(OBJDIR)/server/gateway_mutate.o \
                     $(OBJDIR)/server/msg_session_disable.o $(OBJDIR)/server/gw_mutate_stats.o \
                     $(OBJDIR)/server/agent_bridge.o $(OBJDIR)/server/session_compact.o \
                     $(OBJDIR)/server/tool_call_args.o $(OBJDIR)/server/token_tracker.o \
                     $(OBJDIR)/harness_memory_common.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm -lpthread

$(TESTPREFIX)/unit-test-config-surface: $(OBJDIR)/tests/test_config_surface.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

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

$(TESTPREFIX)/unit-test-feedback: $(OBJDIR)/tests/test_feedback.o $(TEST_DATA_OBJS_MOCK)
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
    $(OBJDIR)/db2/entity_nodes.o $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_postgres.o
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
                               $(OBJDIR)/server/session_compact.o $(OBJDIR)/server/compact_prune.o $(OBJDIR)/server/agent_bridge.o $(OBJDIR)/server/anthropic_shape.o $(OBJDIR)/server/tool_call_args.o \
                               $(OBJDIR)/server/agent_request_shaping.o \
                               $(OBJDIR)/server/context_engine.o \
                               $(OBJDIR)/tests/support/mock_agent_http.o \
                               $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/primary_sessions.o \
                               $(OBJDIR)/model_registry.o \
                               $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-session-search-tool: $(OBJDIR)/tests/test_session_search_tool.o \
                               $(OBJDIR)/server/session_search_tool.o \
                               $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/primary_sessions.o \
                               $(OBJDIR)/db1/server_sessions.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-working-memory: $(OBJDIR)/tests/test_working_memory.o \
                               $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o $(OBJDIR)/db1/db_schema.o $(OBJDIR)/db1/wm.o \
                               $(OBJDIR)/util.o $(OBJDIR)/platform_random.o \
                               $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Mock-pattern demonstrator: exercises the same wm.h contract using the
# in-memory implementation. Same test source, different backing.
$(TESTPREFIX)/unit-test-working-memory-mock: $(OBJDIR)/tests/test_working_memory.o \
                               $(OBJDIR)/db1/db1_init_mock.o $(OBJDIR)/db1/wm_mock.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-local-resolution: $(OBJDIR)/tests/test_local_resolution.o \
                               $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o $(OBJDIR)/db1/project_clones.o \
                               $(OBJDIR)/db1/tool_local_availability.o \
                               $(OBJDIR)/db1/local_operator.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-cognify-jobs: $(OBJDIR)/tests/test_cognify_jobs.o \
                               $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o $(OBJDIR)/db1/cognify_jobs.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-extractors-extra: $(OBJDIR)/tests/test_extractors_extra.o $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-compute-pool: $(OBJDIR)/tests/test_compute_pool.o $(OBJDIR)/server/compute_pool.o $(OBJDIR)/log.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

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
                                 $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-workspace-memory: $(OBJDIR)/tests/test_workspace_memory.o $(TEST_DATA_OBJS_MOCK) \
                                 $(OBJDIR)/workspace.o $(OBJDIR)/dashboard.o $(OBJDIR)/dashboard_kb.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-dashboard: $(OBJDIR)/tests/test_dashboard.o \
                          $(OBJDIR)/dashboard.o $(OBJDIR)/dashboard_kb.o $(OBJDIR)/server/dashboard_server.o $(OBJDIR)/plugin.o $(OBJDIR)/server/kb_client.o $(OBJDIR)/server/kb_client_cache.o $(OBJDIR)/server/kb_client_index.o $(OBJDIR)/code_collect.o $(OBJDIR)/server/kb_client_memory.o $(OBJDIR)/server/kb_client_memory_mutations.o $(OBJDIR)/server/kb_client_agent.o $(OBJDIR)/server/kb_client_dashboard.o $(OBJDIR)/server/kb_client_tasks.o $(OBJDIR)/server/kb_client_data.o \
                          $(OBJDIR)/cli_client.o $(OBJDIR)/cli_v1_routes.o $(OBJDIR)/cli_v1_routes_b.o $(OBJDIR)/cli_v1_routes_c.o $(OBJDIR)/cli_v1_routes_d.o $(OBJDIR)/posix/cli_client.o $(OBJDIR)/aimee_tls.o $(OBJDIR)/codex_auth.o $(DB1_OBJS) \
                          $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o $(OBJDIR)/db2/decision_log.o $(OBJDIR)/db2/kb_audit_worm.o \
                          $(OBJDIR)/config.o $(OBJDIR)/config_sections.o $(OBJDIR)/config_database.o $(OBJDIR)/config_learning.o $(OBJDIR)/config_memory.o $(OBJDIR)/config_charter.o $(OBJDIR)/config_trigger.o $(OBJDIR)/config_kb_maintenance.o $(OBJDIR)/config_kb_curator.o $(OBJDIR)/config_server_api.o $(OBJDIR)/config_skills.o $(OBJDIR)/config_save.o \
                          $(OBJDIR)/yaml.o $(OBJDIR)/db1/db.o $(OBJDIR)/db1/db_schema.o $(OBJDIR)/tests/aimee_pg_sqlite_shim.o $(OBJDIR)/db2/db2_test_shim.o \
                          $(OBJDIR)/util.o $(OBJDIR)/text.o $(OBJDIR)/dstr.o \
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
                            $(OBJDIR)/aimee_home.o $(OBJDIR)/posix/platform_path.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-log: $(OBJDIR)/tests/test_log.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-acp-server: $(OBJDIR)/tests/test_acp_server.o \
                           $(OBJDIR)/acp_server.o \
                           $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-server-dispatch: $(OBJDIR)/tests/test_server_dispatch.o $(OBJDIR)/server/server.o $(OBJDIR)/server/server_seed_config.o $(OBJDIR)/server/server_api_status.o $(OBJDIR)/server_provider.o $(OBJDIR)/server_insights.o $(OBJDIR)/server_eval.o $(OBJDIR)/server_provider_slots.o \
	$(OBJDIR)/server/s2_native_gate_hook.o $(OBJDIR)/workflow/wfe_native_gate.o $(OBJDIR)/workflow/wfe_externalization.o \
	$(OBJDIR)/db1/wfe_binding.o $(OBJDIR)/db1/wfe_store.o $(OBJDIR)/workflow/wfe_enforce.o \
                      $(OBJDIR)/harness_memory_common.o \
                      $(OBJDIR)/memory_redirect.o $(OBJDIR)/harness_memory_scope.o $(OBJDIR)/harness_memory_audit.o \
                      $(OBJDIR)/tests/support/delegate_child_env_export_stub.o \
	                                $(OBJDIR)/server/server_config.o $(OBJDIR)/config_fields.o \
	                                $(OBJDIR)/server/skill_review.o $(OBJDIR)/tests/support/skill_jobs_stub.o \
	                                $(OBJDIR)/server/server_hooks.o $(OBJDIR)/server/server_http.o $(OBJDIR)/server/server_http_routes.o $(OBJDIR)/server/server_http_routes_git.o $(OBJDIR)/server/server_dev_submit.o $(OBJDIR)/server/server_ci_route.o $(OBJDIR)/server/server_http_config_routes.o $(OBJDIR)/server/server_http_conn_worker.o $(OBJDIR)/server/server_http_response.o $(OBJDIR)/server/server_http_sse.o $(OBJDIR)/tests/support/git_route_stub.o $(OBJDIR)/server/server_http_reqctx.o $(OBJDIR)/server/server_http_identity.o $(OBJDIR)/server/vault_principal.o \
	                                $(OBJDIR)/tests/support/workflow_api_stub.o \
	                                $(OBJDIR)/tests/support/vault_handlers_stub.o \
	                                $(OBJDIR)/tests/support/toolset_stub.o \
	                                $(OBJDIR)/cJSON.o $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o \
	                                $(OBJDIR)/db1/model_catalog.o \
	                                $(OBJDIR)/cmd_init.o \
	                                $(OBJDIR)/server/server_trigger.o $(OBJDIR)/db1/db1_trigger.o \
	                                $(OBJDIR)/db1/pipelines.o $(OBJDIR)/db1/token_audit.o \
	                                $(OBJDIR)/server/delegate_backend.o $(OBJDIR)/server/delegate_backend_local.o \
	                                $(OBJDIR)/server/delegate_backend_ssh.o $(OBJDIR)/server/delegate_backend_docker.o \
	                                $(OBJDIR)/server/model_provider.o $(OBJDIR)/server/openai_profile.o \
	                                $(OBJDIR)/server/anthropic_profile.o $(OBJDIR)/server/gemini_profile.o \
	                                $(OBJDIR)/server/openrouter_profile.o $(OBJDIR)/server/ollama_profile.o \
	                                $(OBJDIR)/server/llama_native_profile.o $(OBJDIR)/server/mistral_profile.o \
	                                $(OBJDIR)/server/minimax_profile.o \
	                                $(OBJDIR)/server/delegate_credentials.o $(OBJDIR)/model_registry.o \
	                                $(OBJDIR)/models_dev.o $(OBJDIR)/models_dev_cache.o \
	                                $(OBJDIR)/aimee_home.o \
	                                $(OBJDIR)/tests/support/mock_agent_http.o \
	                                $(OBJDIR)/posix/platform_path.o $(OBJDIR)/posix/platform_random.o \
	                                $(OBJDIR)/platform_random.o \
	                                $(OBJDIR)/linux/secret_store.o $(OBJDIR)/posix/util.o \
	                                $(OBJDIR)/json_fluent.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-client-index: $(OBJDIR)/tests/test_kb_client_index.o \
	                                 $(OBJDIR)/server/kb_client_index_parse.o \
	                                 $(OBJDIR)/json_fluent.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-client-index-remote: $(OBJDIR)/tests/test_kb_client_index_remote.o \
	                                 $(OBJDIR)/server/kb_client_index.o $(OBJDIR)/code_collect.o \
	                                 $(OBJDIR)/server/kb_client_index_parse.o \
	                                 $(OBJDIR)/json_fluent.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-client-docs: $(OBJDIR)/tests/test_kb_client_docs.o \
	                                 $(OBJDIR)/server/kb_client_docs.o \
	                                 $(OBJDIR)/kb/kb_doc_hash.o \
	                                 $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-client-search: $(OBJDIR)/tests/test_kb_client_search.o \
	                                  $(OBJDIR)/server/kb_client.o \
	                                  $(OBJDIR)/server/kb_client_cache.o \
	                                  $(OBJDIR)/server/kb_client_index.o $(OBJDIR)/code_collect.o \
	                                  $(OBJDIR)/server/kb_client_index_parse.o \
	                                  $(OBJDIR)/cli_client.o $(OBJDIR)/cli_v1_routes.o $(OBJDIR)/cli_v1_routes_b.o $(OBJDIR)/cli_v1_routes_c.o $(OBJDIR)/cli_v1_routes_d.o $(OBJDIR)/posix/cli_client.o $(OBJDIR)/aimee_tls.o $(OBJDIR)/codex_auth.o \
	                                  $(OBJDIR)/tests/support/mock_agent_http.o \
	                                  $(OBJDIR)/aimee_home.o $(OBJDIR)/shared/kb_paths.o $(OBJDIR)/cJSON.o \
	                                  $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-client-memory: $(OBJDIR)/tests/test_kb_client_memory.o \
	                                  $(OBJDIR)/server/kb_client.o \
	                                  $(OBJDIR)/server/kb_client_cache.o \
	                                  $(OBJDIR)/server/kb_client_memory.o \
	                                  $(OBJDIR)/server/kb_client_index.o $(OBJDIR)/code_collect.o \
	                                  $(OBJDIR)/server/kb_client_index_parse.o \
	                                  $(OBJDIR)/cli_client.o $(OBJDIR)/cli_v1_routes.o $(OBJDIR)/cli_v1_routes_b.o $(OBJDIR)/cli_v1_routes_c.o $(OBJDIR)/cli_v1_routes_d.o $(OBJDIR)/posix/cli_client.o $(OBJDIR)/aimee_tls.o $(OBJDIR)/codex_auth.o \
	                                  $(OBJDIR)/tests/support/mock_agent_http.o \
	                                  $(OBJDIR)/aimee_home.o $(OBJDIR)/shared/kb_paths.o $(OBJDIR)/cJSON.o \
	                                  $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-server-compute: $(OBJDIR)/tests/test_server_compute.o $(OBJDIR)/db1/db.o $(OBJDIR)/db1/db_schema.o $(OBJDIR)/tests/aimee_pg_sqlite_shim.o $(OBJDIR)/db2/db2_test_shim.o \
                               $(OBJDIR)/tests/support/delegate_role_policy_stub.o \
                               $(OBJDIR)/tests/support/toolset_stub.o \
                               $(OBJDIR)/tests/support/agent_source_authority_stub.o \
                               $(OBJDIR)/tests/support/provider_cli_adapter_stub.o \
                               $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o $(OBJDIR)/db1/delegations.o $(OBJDIR)/db1/agent_jobs.o $(OBJDIR)/db1/session_paths.o $(OBJDIR)/db1/cost_fold.o $(OBJDIR)/db1/token_audit.o $(OBJDIR)/server/delegate_credentials.o $(OBJDIR)/server/delegate_credential_retry.o $(OBJDIR)/db1/delegate_learning.o $(OBJDIR)/db1/interaction_events.o \
                               $(OBJDIR)/db1/execution_plans.o $(OBJDIR)/db1/coord_jobs.o \
		                               $(OBJDIR)/server/delegate_launch.o $(OBJDIR)/server/delegate_source_authority.o $(OBJDIR)/server/delegate_economics.o $(OBJDIR)/server/server_coord_dispatcher.o \
		                               $(OBJDIR)/server/delegate_routing.o \
		                               $(OBJDIR)/model_registry.o \
		                               $(OBJDIR)/models_dev.o $(OBJDIR)/models_dev_cache.o \
		                               $(OBJDIR)/server/server_delegate_status.o \
		                               $(OBJDIR)/server/provider_catalog.o $(OBJDIR)/server/delegate_prompt.o $(OBJDIR)/server/delegate_ephemeral_ws.o $(OBJDIR)/server/delegate_run_phases.o $(OBJDIR)/server/delegate_checkout.o $(OBJDIR)/server/liveness.o \
                               $(OBJDIR)/config.o $(OBJDIR)/config_sections.o $(OBJDIR)/config_database.o $(OBJDIR)/config_learning.o $(OBJDIR)/config_memory.o $(OBJDIR)/config_charter.o $(OBJDIR)/config_trigger.o $(OBJDIR)/config_kb_maintenance.o $(OBJDIR)/config_kb_curator.o $(OBJDIR)/config_server_api.o $(OBJDIR)/config_skills.o $(OBJDIR)/config_save.o $(OBJDIR)/config_mode.o $(OBJDIR)/config_fields.o $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/util.o $(OBJDIR)/text.o \
                               $(OBJDIR)/aimee_home.o \
                               $(OBJDIR)/model_registry.o $(OBJDIR)/models_dev.o $(OBJDIR)/models_dev_cache.o \
                               $(OBJDIR)/platform_random.o $(OBJDIR)/log.o $(PLATFORM_BASIC_OBJS) $(OBJDIR)/cJSON.o $(OBJDIR)/server/presence.o $(OBJDIR)/server/turn_registry.o $(OBJDIR)/tests/support/agent_cancel_stub.o $(OBJDIR)/delivery_target.o \
                               $(OBJDIR)/server/workspace_turn.o $(OBJDIR)/tests/support/git_cred_inject_stub.o $(OBJDIR)/server/workspace_provider_detached.o \
                               $(OBJDIR)/server/workspace_runner_registry.o $(OBJDIR)/server/workspace_runner_queue.o \
                               $(OBJDIR)/workspace_mirror.o $(OBJDIR)/forge_credentials.o $(OBJDIR)/server/git_host_resolve.o \
                               $(OBJDIR)/posix/workspace_provider.o $(OBJDIR)/json_fluent.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-server-jobs-aux: $(OBJDIR)/tests/test_server_jobs_aux.o \
                               $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o \
                               $(OBJDIR)/db1/agent_jobs.o $(OBJDIR)/db1/execution_plans.o \
                               $(OBJDIR)/db1/coord_jobs.o $(OBJDIR)/server/delegate_role.o \
                               $(OBJDIR)/role_templates.o \
                               $(OBJDIR)/json_fluent.o \
                               $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-compute-concurrency: $(OBJDIR)/tests/test_compute_concurrency.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Workflow engine W1: pure (yaml/cJSON/dstr only), no DB/config needed.
$(TESTPREFIX)/unit-test-workflow: $(OBJDIR)/tests/test_workflow.o \
                                  $(OBJDIR)/workflow/wfe_def.o $(OBJDIR)/workflow/wfe_iface.o \
                                  $(OBJDIR)/workflow/wfe_validate.o $(OBJDIR)/workflow/wfe_canonical.o $(OBJDIR)/workflow/wfe_custom.o \
                                  $(OBJDIR)/aimee_home.o \
                                  $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Workflow engine W2: state machine + engine (DB1-backed).
$(TESTPREFIX)/unit-test-wfe-engine: $(OBJDIR)/tests/test_wfe_engine.o \
                                    $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o \
                                    $(OBJDIR)/db1/wfe_store.o $(OBJDIR)/workflow/wfe_engine.o \
                                    $(OBJDIR)/workflow/wfe_def.o $(OBJDIR)/workflow/wfe_iface.o \
                                    $(OBJDIR)/workflow/wfe_validate.o $(OBJDIR)/workflow/wfe_canonical.o $(OBJDIR)/workflow/wfe_custom.o \
                                    $(OBJDIR)/aimee_home.o $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o \
                                    $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Workflow engine W3: block executors (freeze is real git; others integration-gated).
$(TESTPREFIX)/unit-test-wfe-blocks: $(OBJDIR)/tests/test_wfe_blocks.o \
                                    $(OBJDIR)/workflow/wfe_blocks.o $(OBJDIR)/workflow/wfe_engine.o $(OBJDIR)/tests/support/config_autonomy_stub.o \
                                    $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o \
                                    $(OBJDIR)/db1/wfe_store.o $(OBJDIR)/workflow/wfe_def.o \
                                    $(OBJDIR)/workflow/wfe_iface.o $(OBJDIR)/workflow/wfe_validate.o \
                                    $(OBJDIR)/workflow/wfe_canonical.o $(OBJDIR)/workflow/wfe_custom.o $(OBJDIR)/aimee_home.o \
                                    $(OBJDIR)/workflow/wfe_deliver.o $(OBJDIR)/workflow/wfe_manager_artifacts.o \
                                    $(OBJDIR)/util.o $(OBJDIR)/posix/util.o $(OBJDIR)/yaml.o \
                                    $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# primary-as-manager (S0): the def-parse-based tests need the def/validate/canonical
# chain (no engine/db1); the schema + externalization tests are pure.
$(TESTPREFIX)/unit-test-wfe-manager-blocks: $(OBJDIR)/tests/test_wfe_manager_blocks.o \
                                    $(OBJDIR)/workflow/wfe_def.o $(OBJDIR)/workflow/wfe_iface.o \
                                    $(OBJDIR)/workflow/wfe_validate.o $(OBJDIR)/workflow/wfe_canonical.o \
                                    $(OBJDIR)/workflow/wfe_custom.o $(OBJDIR)/aimee_home.o \
                                    $(OBJDIR)/util.o $(OBJDIR)/posix/util.o $(OBJDIR)/yaml.o \
                                    $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-wfe-manager-artifacts: $(OBJDIR)/tests/test_wfe_manager_artifacts.o \
                                    $(OBJDIR)/workflow/wfe_manager_artifacts.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-wfe-externalization: $(OBJDIR)/tests/test_wfe_externalization.o \
                                    $(OBJDIR)/workflow/wfe_externalization.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-wfe-deliver: $(OBJDIR)/tests/test_wfe_deliver.o \
                                    $(OBJDIR)/workflow/wfe_deliver.o $(OBJDIR)/workflow/wfe_def.o \
                                    $(OBJDIR)/workflow/wfe_iface.o $(OBJDIR)/workflow/wfe_validate.o \
                                    $(OBJDIR)/workflow/wfe_canonical.o $(OBJDIR)/workflow/wfe_custom.o \
                                    $(OBJDIR)/workflow/wfe_deliver.o $(OBJDIR)/workflow/wfe_manager_artifacts.o \
                                    $(OBJDIR)/aimee_home.o $(OBJDIR)/util.o $(OBJDIR)/posix/util.o \
                                    $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# S1 router pure core (no engine/DB/LLM deps).
$(TESTPREFIX)/unit-test-wfe-router: $(OBJDIR)/tests/test_wfe_router.o \
                                    $(OBJDIR)/workflow/wfe_router.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# S4 autonomous-parity routing policy (pure; no engine/DB deps).
$(TESTPREFIX)/unit-test-wfe-autonomous-route: $(OBJDIR)/tests/test_wfe_autonomous_route.o \
                                    $(OBJDIR)/workflow/wfe_autonomous_route.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# S2 enforcement pure cores (no engine/DB deps).
$(TESTPREFIX)/unit-test-wfe-native-gate: $(OBJDIR)/tests/test_wfe_native_gate.o \
                                    $(OBJDIR)/workflow/wfe_native_gate.o $(OBJDIR)/workflow/wfe_externalization.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-wfe-enforce: $(OBJDIR)/tests/test_wfe_enforce.o \
                                    $(OBJDIR)/workflow/wfe_enforce.o $(OBJDIR)/workflow/wfe_externalization.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# S2 sub-slice 3: advance_request pure core (parser + CAS/replay decision; cJSON only).
$(TESTPREFIX)/unit-test-wfe-advance: $(OBJDIR)/tests/test_wfe_advance.o \
                                    $(OBJDIR)/workflow/wfe_advance.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# S2 binding seam: auth-token->sid parser + idempotent interactive bind.
$(TESTPREFIX)/unit-test-wfe-bind-ingress: $(OBJDIR)/tests/test_wfe_bind_ingress.o \
                                    $(OBJDIR)/workflow/wfe_bind_ingress.o $(OBJDIR)/log.o $(OBJDIR)/workflow/wfe_enforce.o \
                                    $(OBJDIR)/workflow/wfe_externalization.o $(OBJDIR)/db1/wfe_binding.o \
                                    $(OBJDIR)/workflow/wfe_router.o $(OBJDIR)/workflow/wfe_router_catalog.o \
                                    $(OBJDIR)/workflow/wfe_blocks.o $(OBJDIR)/workflow/wfe_engine.o $(OBJDIR)/tests/support/config_autonomy_stub.o \
                                    $(OBJDIR)/workflow/wfe_manager_artifacts.o $(OBJDIR)/workflow/wfe_deliver.o \
                                    $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o \
                                    $(OBJDIR)/db1/wfe_store.o $(OBJDIR)/workflow/wfe_def.o \
                                    $(OBJDIR)/workflow/wfe_iface.o $(OBJDIR)/workflow/wfe_validate.o \
                                    $(OBJDIR)/workflow/wfe_canonical.o $(OBJDIR)/workflow/wfe_custom.o \
                                    $(OBJDIR)/aimee_home.o $(OBJDIR)/util.o $(OBJDIR)/posix/util.o \
                                    $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Primary-CLI-ingestor S2 seam (Slice 2): gate + enforce-before-send. Same dep
# closure as unit-test-wfe-bind-ingress (it calls wfe_bind_interactive) + the
# ingestor object.
$(TESTPREFIX)/unit-test-primary-cli-ingestor: $(OBJDIR)/tests/test_primary_cli_ingestor.o \
                                    $(OBJDIR)/server/primary_cli_ingestor.o $(OBJDIR)/log.o \
                                    $(OBJDIR)/workflow/wfe_bind_ingress.o $(OBJDIR)/workflow/wfe_enforce.o \
                                    $(OBJDIR)/workflow/wfe_externalization.o $(OBJDIR)/db1/wfe_binding.o \
                                    $(OBJDIR)/workflow/wfe_router.o $(OBJDIR)/workflow/wfe_router_catalog.o \
                                    $(OBJDIR)/workflow/wfe_blocks.o $(OBJDIR)/workflow/wfe_engine.o $(OBJDIR)/tests/support/config_autonomy_stub.o \
                                    $(OBJDIR)/workflow/wfe_manager_artifacts.o $(OBJDIR)/workflow/wfe_deliver.o \
                                    $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o \
                                    $(OBJDIR)/db1/wfe_store.o $(OBJDIR)/workflow/wfe_def.o \
                                    $(OBJDIR)/workflow/wfe_iface.o $(OBJDIR)/workflow/wfe_validate.o \
                                    $(OBJDIR)/workflow/wfe_canonical.o $(OBJDIR)/workflow/wfe_custom.o \
                                    $(OBJDIR)/aimee_home.o $(OBJDIR)/util.o $(OBJDIR)/posix/util.o \
                                    $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# S2 sub-slice 4: per-block resolver + dispatch-time externalization guard.
$(TESTPREFIX)/unit-test-wfe-block-resolve: $(OBJDIR)/tests/test_wfe_block_resolve.o \
                                    $(OBJDIR)/workflow/wfe_block_resolve.o $(OBJDIR)/workflow/wfe_enforce.o \
                                    $(OBJDIR)/workflow/wfe_externalization.o $(OBJDIR)/db1/wfe_binding.o \
                                    $(OBJDIR)/workflow/wfe_blocks.o $(OBJDIR)/workflow/wfe_engine.o $(OBJDIR)/tests/support/config_autonomy_stub.o \
                                    $(OBJDIR)/workflow/wfe_manager_artifacts.o $(OBJDIR)/workflow/wfe_deliver.o \
                                    $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o \
                                    $(OBJDIR)/db1/wfe_store.o $(OBJDIR)/workflow/wfe_def.o \
                                    $(OBJDIR)/workflow/wfe_iface.o $(OBJDIR)/workflow/wfe_validate.o \
                                    $(OBJDIR)/workflow/wfe_canonical.o $(OBJDIR)/workflow/wfe_custom.o \
                                    $(OBJDIR)/aimee_home.o $(OBJDIR)/util.o $(OBJDIR)/posix/util.o \
                                    $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# S2 sub-slice 3: interactive-driver executor (binding + engine + audit).
$(TESTPREFIX)/unit-test-wfe-advance-exec: $(OBJDIR)/tests/test_wfe_advance_exec.o \
                                    $(OBJDIR)/workflow/wfe_advance_exec.o $(OBJDIR)/workflow/wfe_advance.o \
                                    $(OBJDIR)/workflow/wfe_enforce.o $(OBJDIR)/workflow/wfe_externalization.o \
                                    $(OBJDIR)/db1/wfe_binding.o \
                                    $(OBJDIR)/workflow/wfe_blocks.o $(OBJDIR)/workflow/wfe_engine.o $(OBJDIR)/tests/support/config_autonomy_stub.o \
                                    $(OBJDIR)/workflow/wfe_manager_artifacts.o $(OBJDIR)/workflow/wfe_deliver.o \
                                    $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o \
                                    $(OBJDIR)/db1/wfe_store.o $(OBJDIR)/workflow/wfe_def.o \
                                    $(OBJDIR)/workflow/wfe_iface.o $(OBJDIR)/workflow/wfe_validate.o \
                                    $(OBJDIR)/workflow/wfe_canonical.o $(OBJDIR)/workflow/wfe_custom.o \
                                    $(OBJDIR)/aimee_home.o $(OBJDIR)/util.o $(OBJDIR)/posix/util.o \
                                    $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# S2 session<->work-item binding (DB1-backed).
$(TESTPREFIX)/unit-test-wfe-binding: $(OBJDIR)/tests/test_wfe_binding.o \
                                    $(OBJDIR)/db1/wfe_binding.o $(OBJDIR)/db1/wfe_store.o \
                                    $(OBJDIR)/db1/db1_init.o \
                                    $(OBJDIR)/db1/db1_write.o $(OBJDIR)/db1/db1_trigger.o \
                                    $(OBJDIR)/db1/db1_cron_jobs.o $(OBJDIR)/db1/model_catalog.o \
                                    $(OBJDIR)/db1/eval.o $(OBJDIR)/db2/db_schema.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Slice 0: the canonical IR (pure — cJSON only).
$(TESTPREFIX)/unit-test-aimee-ir: $(OBJDIR)/tests/test_aimee_ir.o \
                                 $(OBJDIR)/server/aimee_ir.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Slice 0: IR shadow metrics (pure).
$(TESTPREFIX)/unit-test-aimee-ir-metrics: $(OBJDIR)/tests/test_aimee_ir_metrics.o \
                                         $(OBJDIR)/server/aimee_ir_metrics.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Slice 1: frontend parse adapters (pure — cJSON only).
$(TESTPREFIX)/unit-test-aimee-frontend: $(OBJDIR)/tests/test_aimee_frontend.o \
                                       $(OBJDIR)/server/aimee_frontend_anthropic.o \
                                       $(OBJDIR)/server/aimee_frontend_openai.o \
                                       $(OBJDIR)/server/aimee_frontend_responses.o \
                                       $(OBJDIR)/server/aimee_ir.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Slice 2: backend build/parse adapters (pure — cJSON only).
$(TESTPREFIX)/unit-test-aimee-backend: $(OBJDIR)/tests/test_aimee_backend.o \
                                      $(OBJDIR)/server/aimee_backend_anthropic.o \
                                      $(OBJDIR)/server/aimee_backend_openai.o \
                                      $(OBJDIR)/server/aimee_backend_responses.o \
                                      $(OBJDIR)/server/aimee_frontend_anthropic.o \
                                      $(OBJDIR)/server/aimee_frontend_openai.o \
                                      $(OBJDIR)/server/aimee_ir.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Slice 3: IR shadow observer (pure — cJSON only).
$(TESTPREFIX)/unit-test-aimee-ir-shadow: $(OBJDIR)/tests/test_aimee_ir_shadow.o \
                                        $(OBJDIR)/server/aimee_ir_shadow.o \
                                        $(OBJDIR)/server/aimee_ir_metrics.o \
                                        $(OBJDIR)/server/aimee_backend_anthropic.o \
                                        $(OBJDIR)/server/aimee_frontend_anthropic.o \
                                        $(OBJDIR)/server/aimee_frontend_openai.o \
                                        $(OBJDIR)/server/aimee_ir.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Slice 5: IR live request-build (pure — cJSON only).
$(TESTPREFIX)/unit-test-aimee-ir-serve: $(OBJDIR)/tests/test_aimee_ir_serve.o \
                                       $(OBJDIR)/server/aimee_ir_serve.o \
                                       $(OBJDIR)/server/aimee_backend_openai.o \
                                       $(OBJDIR)/server/aimee_backend_responses.o \
                                       $(OBJDIR)/server/aimee_frontend_anthropic.o \
                                       $(OBJDIR)/server/aimee_frontend_openai.o \
                                       $(OBJDIR)/server/aimee_frontend_responses.o \
                                       $(OBJDIR)/server/aimee_ir.o \
                                       $(OBJDIR)/server/aimee_ir_metrics.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Slice 4: IR-delta streaming (pure — cJSON only).
$(TESTPREFIX)/unit-test-aimee-ir-stream: $(OBJDIR)/tests/test_aimee_ir_stream.o \
                                        $(OBJDIR)/server/aimee_ir_stream.o \
                                        $(OBJDIR)/server/aimee_ir.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# S1 router catalog I/O (enumerates $AIMEE_HOME workflows + built-in lanes).
$(TESTPREFIX)/unit-test-wfe-router-catalog: $(OBJDIR)/tests/test_wfe_router_catalog.o \
                                    $(OBJDIR)/workflow/wfe_router_catalog.o $(OBJDIR)/workflow/wfe_router.o \
                                    $(OBJDIR)/yaml.o $(OBJDIR)/cJSON.o $(OBJDIR)/aimee_home.o \
                                    $(OBJDIR)/dstr.o $(OBJDIR)/posix/platform_path.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Integration: the manager executors driven through the real engine (DB1-backed).
$(TESTPREFIX)/unit-test-wfe-manager-flow: $(OBJDIR)/tests/test_wfe_manager_flow.o \
                                    $(OBJDIR)/workflow/wfe_blocks.o $(OBJDIR)/workflow/wfe_engine.o $(OBJDIR)/tests/support/config_autonomy_stub.o \
                                    $(OBJDIR)/workflow/wfe_manager_artifacts.o $(OBJDIR)/workflow/wfe_deliver.o \
                                    $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o \
                                    $(OBJDIR)/db1/wfe_store.o $(OBJDIR)/workflow/wfe_def.o \
                                    $(OBJDIR)/workflow/wfe_iface.o $(OBJDIR)/workflow/wfe_validate.o \
                                    $(OBJDIR)/workflow/wfe_canonical.o $(OBJDIR)/workflow/wfe_custom.o \
                                    $(OBJDIR)/workflow/wfe_deliver.o $(OBJDIR)/workflow/wfe_manager_artifacts.o \
                                    $(OBJDIR)/aimee_home.o $(OBJDIR)/util.o $(OBJDIR)/posix/util.o \
                                    $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Config-extensible blocks + safety blocks share the blocks/engine/registry deps.
$(TESTPREFIX)/unit-test-wfe-custom: $(OBJDIR)/tests/test_wfe_custom.o \
                                    $(OBJDIR)/workflow/wfe_blocks.o $(OBJDIR)/workflow/wfe_engine.o $(OBJDIR)/tests/support/config_autonomy_stub.o \
                                    $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o \
                                    $(OBJDIR)/db1/wfe_store.o $(OBJDIR)/workflow/wfe_def.o \
                                    $(OBJDIR)/workflow/wfe_iface.o $(OBJDIR)/workflow/wfe_validate.o \
                                    $(OBJDIR)/workflow/wfe_canonical.o $(OBJDIR)/workflow/wfe_custom.o \
                                    $(OBJDIR)/workflow/wfe_deliver.o $(OBJDIR)/workflow/wfe_manager_artifacts.o \
                                    $(OBJDIR)/aimee_home.o $(OBJDIR)/util.o $(OBJDIR)/posix/util.o \
                                    $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-wfe-safety: $(OBJDIR)/tests/test_wfe_safety.o \
                                    $(OBJDIR)/workflow/wfe_blocks.o $(OBJDIR)/workflow/wfe_engine.o $(OBJDIR)/tests/support/config_autonomy_stub.o \
                                    $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o \
                                    $(OBJDIR)/db1/wfe_store.o $(OBJDIR)/workflow/wfe_def.o \
                                    $(OBJDIR)/workflow/wfe_iface.o $(OBJDIR)/workflow/wfe_validate.o \
                                    $(OBJDIR)/workflow/wfe_canonical.o $(OBJDIR)/workflow/wfe_custom.o \
                                    $(OBJDIR)/workflow/wfe_deliver.o $(OBJDIR)/workflow/wfe_manager_artifacts.o \
                                    $(OBJDIR)/aimee_home.o $(OBJDIR)/util.o $(OBJDIR)/posix/util.o \
                                    $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-wfe-failure-taxonomy: $(OBJDIR)/tests/test_wfe_failure_taxonomy.o \
                                    $(OBJDIR)/workflow/wfe_iface.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm

$(TESTPREFIX)/unit-test-wfe-delegate-seam: $(OBJDIR)/tests/test_wfe_delegate_seam.o \
                                    $(OBJDIR)/workflow/wfe_blocks.o $(OBJDIR)/workflow/wfe_engine.o $(OBJDIR)/tests/support/config_autonomy_stub.o \
                                    $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o \
                                    $(OBJDIR)/db1/wfe_store.o $(OBJDIR)/workflow/wfe_def.o \
                                    $(OBJDIR)/workflow/wfe_iface.o $(OBJDIR)/workflow/wfe_validate.o \
                                    $(OBJDIR)/workflow/wfe_canonical.o $(OBJDIR)/workflow/wfe_custom.o \
                                    $(OBJDIR)/workflow/wfe_deliver.o $(OBJDIR)/workflow/wfe_manager_artifacts.o \
                                    $(OBJDIR)/aimee_home.o $(OBJDIR)/util.o $(OBJDIR)/posix/util.o \
                                    $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-wfe-scheduler: $(OBJDIR)/tests/test_wfe_scheduler.o \
                                    $(OBJDIR)/server/wfe_scheduler.o $(OBJDIR)/workflow/wfe_autonomy.o \
                                    $(OBJDIR)/tests/support/log_stub.o \
                                    $(OBJDIR)/workflow/wfe_blocks.o $(OBJDIR)/workflow/wfe_engine.o $(OBJDIR)/tests/support/config_autonomy_stub.o \
                                    $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o \
                                    $(OBJDIR)/db1/wfe_store.o $(OBJDIR)/workflow/wfe_def.o \
                                    $(OBJDIR)/workflow/wfe_iface.o $(OBJDIR)/workflow/wfe_validate.o \
                                    $(OBJDIR)/workflow/wfe_canonical.o $(OBJDIR)/workflow/wfe_custom.o \
                                    $(OBJDIR)/workflow/wfe_roundtable.o $(OBJDIR)/workflow/wfe_approval.o \
                                    $(OBJDIR)/workflow/wfe_verdict.o \
                                    $(OBJDIR)/aimee_home.o $(OBJDIR)/util.o $(OBJDIR)/posix/util.o \
                                    $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-wfe-random-delegate: $(OBJDIR)/tests/test_wfe_random_delegate.o \
                                    $(OBJDIR)/server/wfe_delegate_resolve.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

# Human-gate reject routing (retry_on_reject): pure def-query, no DB.
$(TESTPREFIX)/unit-test-wfe-gate-reject: $(OBJDIR)/tests/test_wfe_gate_reject.o \
                                    $(OBJDIR)/workflow/wfe_def.o $(OBJDIR)/workflow/wfe_iface.o \
                                    $(OBJDIR)/workflow/wfe_validate.o $(OBJDIR)/workflow/wfe_canonical.o \
                                    $(OBJDIR)/workflow/wfe_custom.o $(OBJDIR)/aimee_home.o \
                                    $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Atomic guarded human-gate transition (DB1-backed).
$(TESTPREFIX)/unit-test-wfe-gate-apply: $(OBJDIR)/tests/test_wfe_gate_apply.o \
                                    $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o \
                                    $(OBJDIR)/db1/wfe_store.o $(OBJDIR)/aimee_home.o \
                                    $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# intake-auth: per-principal submitter binding + concurrency/rate count helpers.
$(TESTPREFIX)/unit-test-wfe-submitter: $(OBJDIR)/tests/test_wfe_submitter.o \
                                    $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o \
                                    $(OBJDIR)/db1/wfe_store.o $(OBJDIR)/aimee_home.o \
                                    $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Capability invariant for the gate route (compile-time _Static_assert).
$(TESTPREFIX)/unit-test-workflow-gate-caps: $(OBJDIR)/tests/test_workflow_gate_caps.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

# Workflow visual composer (W7): /v1/workflow read+author handlers.
$(TESTPREFIX)/unit-test-wfe-webapi: $(OBJDIR)/tests/test_wfe_webapi.o \
                                    $(OBJDIR)/server/server_workflow_api.o \
                                    $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o \
                                    $(OBJDIR)/db1/wfe_store.o $(OBJDIR)/workflow/wfe_def.o \
                                    $(OBJDIR)/workflow/wfe_iface.o $(OBJDIR)/workflow/wfe_validate.o \
                                    $(OBJDIR)/workflow/wfe_canonical.o $(OBJDIR)/workflow/wfe_custom.o \
                                    $(OBJDIR)/aimee_home.o $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Workflow engine W4: HMAC approval signer + gate.human.
$(TESTPREFIX)/unit-test-wfe-approval: $(OBJDIR)/tests/test_wfe_approval.o \
                                      $(OBJDIR)/workflow/wfe_approval.o $(OBJDIR)/workflow/wfe_engine.o \
                                      $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o \
                                      $(OBJDIR)/db1/wfe_store.o $(OBJDIR)/workflow/wfe_def.o \
                                      $(OBJDIR)/workflow/wfe_iface.o $(OBJDIR)/workflow/wfe_validate.o \
                                      $(OBJDIR)/workflow/wfe_canonical.o $(OBJDIR)/workflow/wfe_custom.o $(OBJDIR)/aimee_home.o \
                                      $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Workflow engine W5: roundtable verdict rule + gate.roundtable (mock panel).
$(TESTPREFIX)/unit-test-wfe-roundtable: $(OBJDIR)/tests/test_wfe_roundtable.o \
                                        $(OBJDIR)/workflow/wfe_roundtable.o \
                                        $(OBJDIR)/workflow/wfe_verdict.o $(OBJDIR)/workflow/wfe_engine.o \
                                        $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o \
                                        $(OBJDIR)/db1/wfe_store.o $(OBJDIR)/workflow/wfe_def.o \
                                        $(OBJDIR)/workflow/wfe_iface.o $(OBJDIR)/workflow/wfe_validate.o \
                                        $(OBJDIR)/workflow/wfe_canonical.o $(OBJDIR)/workflow/wfe_custom.o $(OBJDIR)/aimee_home.o \
                                        $(OBJDIR)/util.o $(OBJDIR)/posix/util.o $(OBJDIR)/tests/support/log_stub.o \
                                        $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# foreach.workflow fan-in aggregation (DB parent<->child linkage) via the engine +
# a mock child-spawn provider.
$(TESTPREFIX)/unit-test-wfe-foreach: $(OBJDIR)/tests/test_wfe_foreach.o \
                                    $(OBJDIR)/workflow/wfe_blocks.o $(OBJDIR)/workflow/wfe_engine.o $(OBJDIR)/tests/support/config_autonomy_stub.o \
                                    $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o \
                                    $(OBJDIR)/db1/wfe_store.o $(OBJDIR)/workflow/wfe_def.o \
                                    $(OBJDIR)/workflow/wfe_iface.o $(OBJDIR)/workflow/wfe_validate.o \
                                    $(OBJDIR)/workflow/wfe_canonical.o $(OBJDIR)/workflow/wfe_custom.o \
                                    $(OBJDIR)/workflow/wfe_deliver.o $(OBJDIR)/workflow/wfe_manager_artifacts.o \
                                    $(OBJDIR)/aimee_home.o $(OBJDIR)/util.o $(OBJDIR)/posix/util.o \
                                    $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Live panel: verified roundtable items -> per-lens wfe verdicts (pure mapper).
$(TESTPREFIX)/unit-test-wfe-panel-roundtable: $(OBJDIR)/tests/test_wfe_panel_roundtable.o \
                                    $(OBJDIR)/server/wfe_panel_roundtable.o \
                                    $(OBJDIR)/workflow/wfe_verdict.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

# Live panel: the worktree-grounded evidence-replay backend.
$(TESTPREFIX)/unit-test-wfe-replay-worktree: $(OBJDIR)/tests/test_wfe_replay_worktree.o \
                              $(OBJDIR)/server/wfe_replay_worktree.o $(OBJDIR)/server/evidence_replay.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto

# Live foreach spawner: a split packet-plan -> child slice work items (parent linkage).
$(TESTPREFIX)/unit-test-wfe-foreach-spawn: $(OBJDIR)/tests/test_wfe_foreach_spawn.o \
                                    $(OBJDIR)/server/wfe_live_foreach.o \
                                    $(OBJDIR)/workflow/wfe_blocks.o $(OBJDIR)/workflow/wfe_engine.o $(OBJDIR)/tests/support/config_autonomy_stub.o \
                                    $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o \
                                    $(OBJDIR)/db1/wfe_store.o $(OBJDIR)/workflow/wfe_def.o \
                                    $(OBJDIR)/workflow/wfe_iface.o $(OBJDIR)/workflow/wfe_validate.o \
                                    $(OBJDIR)/workflow/wfe_canonical.o $(OBJDIR)/workflow/wfe_custom.o \
                                    $(OBJDIR)/workflow/wfe_deliver.o $(OBJDIR)/workflow/wfe_manager_artifacts.o \
                                    $(OBJDIR)/aimee_home.o $(OBJDIR)/log.o $(OBJDIR)/util.o $(OBJDIR)/posix/util.o \
                                    $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Sliced-lifecycle build workflow: branch.open / foreach.workflow catalog typing +
# parent/child graph validation + version stability (pure def/validator; no engine).
$(TESTPREFIX)/unit-test-wfe-sliced-build: $(OBJDIR)/tests/test_wfe_sliced_build.o \
                                        $(OBJDIR)/workflow/wfe_def.o $(OBJDIR)/workflow/wfe_iface.o \
                                        $(OBJDIR)/workflow/wfe_validate.o $(OBJDIR)/workflow/wfe_canonical.o \
                                        $(OBJDIR)/workflow/wfe_custom.o $(OBJDIR)/aimee_home.o \
                                        $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Workflow engine W6: autonomy driver + gate-override.
$(TESTPREFIX)/unit-test-wfe-autonomy: $(OBJDIR)/tests/test_wfe_autonomy.o \
                                      $(OBJDIR)/workflow/wfe_autonomy.o $(OBJDIR)/workflow/wfe_approval.o \
                                      $(OBJDIR)/tests/support/log_stub.o \
                                      $(OBJDIR)/workflow/wfe_roundtable.o $(OBJDIR)/workflow/wfe_verdict.o \
                                      $(OBJDIR)/workflow/wfe_engine.o $(OBJDIR)/db1/db1_init.o \
                                      $(OBJDIR)/db1/db_schema.o $(OBJDIR)/db1/wfe_store.o \
                                      $(OBJDIR)/workflow/wfe_def.o $(OBJDIR)/workflow/wfe_iface.o \
                                      $(OBJDIR)/workflow/wfe_validate.o $(OBJDIR)/workflow/wfe_canonical.o $(OBJDIR)/workflow/wfe_custom.o \
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
                           $(OBJDIR)/mcp_git_query.o $(OBJDIR)/tests/support/git_cred_inject_stub.o $(OBJDIR)/forge_credentials.o $(OBJDIR)/server/git_host_resolve.o $(OBJDIR)/mcp_git_write.o \
                           $(OBJDIR)/mcp_git_branch.o $(OBJDIR)/mcp_git_pr.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-cmd-core: $(OBJDIR)/tests/test_cmd_core.o $(TEST_DATA_OBJS) \
                         $(TEST_WORKSPACE_OBJS_EXTRA) \
                         $(OBJDIR)/cmd_util.o $(OBJDIR)/cmd_work.o \
                         $(OBJDIR)/cmd_infra.o \
                         $(OBJDIR)/cmd_init.o \
                         $(OBJDIR)/mcp_git_query.o $(OBJDIR)/tests/support/git_cred_inject_stub.o $(OBJDIR)/forge_credentials.o $(OBJDIR)/server/git_host_resolve.o $(OBJDIR)/mcp_git_write.o \
                         $(OBJDIR)/mcp_git_branch.o $(OBJDIR)/mcp_git_pr.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-cmd-work: $(OBJDIR)/tests/test_cmd_work.o $(TEST_DATA_OBJS_MOCK) \
                          $(OBJDIR)/cmd_work.o $(OBJDIR)/cmd_util.o \
                          $(OBJDIR)/tests/support/kb_client_test_stub.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-client-integrations: $(OBJDIR)/tests/test_client_integrations.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-mcp-git: $(OBJDIR)/tests/test_mcp_git.o $(OBJDIR)/mcp_git_query.o $(OBJDIR)/tests/support/git_cred_inject_stub.o $(OBJDIR)/forge_credentials.o $(OBJDIR)/server/git_host_resolve.o $(OBJDIR)/mcp_git_write.o \
                        $(OBJDIR)/mcp_git_branch.o $(OBJDIR)/mcp_git_pr.o $(OBJDIR)/git_verify.o $(OBJDIR)/git_verify_state.o $(OBJDIR)/git_verify_config.o \
                        $(OBJDIR)/git_verify_jobs.o $(OBJDIR)/git_verify_hook.o $(OBJDIR)/git_verify_ops.o \
                        $(OBJDIR)/git_verify_select.o $(OBJDIR)/git_verify_step.o $(OBJDIR)/server/compute_pool.o \
                        $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-git-verify-select: $(OBJDIR)/tests/test_git_verify_select.o \
                        $(OBJDIR)/git_verify_select.o $(OBJDIR)/git_verify.o $(OBJDIR)/git_verify_state.o $(OBJDIR)/git_verify_config.o \
                        $(OBJDIR)/git_verify_jobs.o $(OBJDIR)/git_verify_hook.o $(OBJDIR)/git_verify_ops.o \
                        $(OBJDIR)/git_verify_step.o $(OBJDIR)/server/compute_pool.o \
                        $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-git-verify-contract: $(OBJDIR)/tests/test_git_verify_contract.o \
                        $(OBJDIR)/git_verify.o $(OBJDIR)/git_verify_state.o $(OBJDIR)/git_verify_config.o \
                        $(OBJDIR)/git_verify_jobs.o $(OBJDIR)/git_verify_hook.o $(OBJDIR)/git_verify_ops.o \
                        $(OBJDIR)/git_verify_select.o $(OBJDIR)/git_verify_step.o $(OBJDIR)/server/compute_pool.o \
                        $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-interaction-events: $(OBJDIR)/tests/test_interaction_events.o \
                               $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o \
                               $(OBJDIR)/db1/maintenance.o $(OBJDIR)/db1/interaction_events.o \
                               $(OBJDIR)/log.o $(OBJDIR)/util.o $(OBJDIR)/cJSON.o \
                               $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-trajectory: $(OBJDIR)/tests/test_trajectory.o \
                               $(OBJDIR)/trajectory_export.o $(OBJDIR)/audit_ledger.o \
                               $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o \
                               $(OBJDIR)/db1/maintenance.o $(OBJDIR)/db1/interaction_events.o \
                               $(OBJDIR)/posix/memory.o \
                               $(OBJDIR)/config.o $(OBJDIR)/config_sections.o $(OBJDIR)/config_database.o $(OBJDIR)/config_learning.o $(OBJDIR)/config_memory.o $(OBJDIR)/config_charter.o $(OBJDIR)/config_trigger.o $(OBJDIR)/config_kb_maintenance.o $(OBJDIR)/config_kb_curator.o $(OBJDIR)/config_server_api.o $(OBJDIR)/config_skills.o $(OBJDIR)/config_save.o $(OBJDIR)/aimee_home.o \
                               $(OBJDIR)/log.o $(OBJDIR)/util.o $(OBJDIR)/text.o $(OBJDIR)/cJSON.o \
                               $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-trajectory-batch: $(OBJDIR)/tests/test_trajectory_batch.o \
                               $(OBJDIR)/trajectory_batch.o $(OBJDIR)/trajectory_export.o $(OBJDIR)/audit_ledger.o \
                               $(OBJDIR)/config.o $(OBJDIR)/config_sections.o $(OBJDIR)/config_database.o $(OBJDIR)/config_learning.o $(OBJDIR)/config_memory.o $(OBJDIR)/config_charter.o $(OBJDIR)/config_trigger.o $(OBJDIR)/config_kb_maintenance.o $(OBJDIR)/config_kb_curator.o $(OBJDIR)/config_server_api.o $(OBJDIR)/config_skills.o $(OBJDIR)/config_save.o \
                               $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o \
                               $(OBJDIR)/db1/maintenance.o $(OBJDIR)/db1/interaction_events.o \
                               $(OBJDIR)/posix/memory.o \
                               $(OBJDIR)/log.o $(OBJDIR)/util.o $(OBJDIR)/text.o $(OBJDIR)/yaml.o $(OBJDIR)/aimee_home.o $(OBJDIR)/cJSON.o \
                               $(PLATFORM_BASIC_OBJS)
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
$(OBJDIR)/tests/test_kb_rrf.o: C_FLAGS += -Ikb
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
$(TESTPREFIX)/unit-test-kb-rrf: $(OBJDIR)/tests/test_kb_rrf.o $(OBJDIR)/kb/kb_rrf.o
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

# Render-boundary prompt sanitizer (graph-feedback §4 / P0). Pure: no DB.
$(TESTPREFIX)/unit-test-prompt-sanitizer: $(OBJDIR)/tests/test_prompt_sanitizer.o \
                                          $(OBJDIR)/kb/prompt_sanitizer.o
	$(TESTLINK) -o $@ $^ $(L_CORE)

# Blast-radius advisory: structural §7 actuation. Hermetic — config_load and the
# kb_client_index_* sidecar calls are stubbed in the test, so no DB/sidecar.
$(TESTPREFIX)/unit-test-guardrails-blast-radius: $(OBJDIR)/tests/test_guardrails_blast_radius.o \
                                                 $(OBJDIR)/guardrails_blast_radius.o
	$(TESTLINK) -o $@ $^ $(L_CORE)

# Code collector source selection (git default branch vs working tree). Drives
# the real collector against throwaway git repos materialized under TMPDIR.
$(TESTPREFIX)/unit-test-code-collect: $(OBJDIR)/tests/test_code_collect.o \
                                      $(OBJDIR)/code_collect.o $(OBJDIR)/cJSON.o $(PLATFORM_BASIC_OBJS)
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
                                      $(OBJDIR)/aimee_home.o $(TLS_OBJS)
	$(TESTLINK) -o $@ $^ $(L_MINIMAL) $(TLS_LIBS)

$(TESTPREFIX)/unit-test-cli-remote: $(OBJDIR)/tests/test_cli_remote.o $(OBJDIR)/cli_remote.o \
                                    $(OBJDIR)/aimee_client.o $(OBJDIR)/posix/platform_net.o \
                                    $(OBJDIR)/http_uds_client.o $(OBJDIR)/aimee_home.o $(OBJDIR)/cJSON.o \
                                    $(TLS_OBJS)
	$(TESTLINK) -o $@ $^ $(L_MINIMAL) $(TLS_LIBS)

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
                                 $(OBJDIR)/gateway/gateway_ctx.o $(OBJDIR)/cli_v1_routes.o $(OBJDIR)/cli_v1_routes_b.o $(OBJDIR)/cli_v1_routes_c.o $(OBJDIR)/cli_v1_routes_d.o \
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
                                 $(GATEWAY_PLATFORM_OBJS)
	$(TESTLINK) -o $@ $^ $(L_GATEWAY)

$(TESTPREFIX)/unit-test-gateway-telegram: $(OBJDIR)/tests/test_gateway_telegram.o \
                                          $(OBJDIR)/gateway/platform_telegram.o \
                                          $(OBJDIR)/gateway/platform_registry.o \
                                          $(OBJDIR)/gateway/platform_ntfy.o \
                                          $(OBJDIR)/gateway/platform_webhook.o \
                                          $(OBJDIR)/gateway/gateway_ctx.o $(OBJDIR)/cli_v1_routes.o $(OBJDIR)/cli_v1_routes_b.o $(OBJDIR)/cli_v1_routes_c.o $(OBJDIR)/cli_v1_routes_d.o \
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
                                          $(OBJDIR)/posix/cli_client.o $(OBJDIR)/cli_v1_routes.o $(OBJDIR)/cli_v1_routes_b.o $(OBJDIR)/cli_v1_routes_c.o $(OBJDIR)/cli_v1_routes_d.o $(OBJDIR)/aimee_tls.o $(OBJDIR)/codex_auth.o \
                                          $(OBJDIR)/proxy_bootstrap.o \
                                          $(OBJDIR)/cJSON.o \
                                          $(OBJDIR)/log.o \
                                          $(OBJDIR)/platform_random.o \
                                          $(GATEWAY_PLATFORM_OBJS)
	$(TESTLINK) -o $@ $^ $(L_GATEWAY)

$(TESTPREFIX)/unit-test-gateway-ntfy-webhook: $(OBJDIR)/tests/test_gateway_ntfy_webhook.o \
                                              $(OBJDIR)/gateway/platform_ntfy.o \
                                              $(OBJDIR)/gateway/platform_webhook.o \
                                              $(OBJDIR)/gateway/platform_telegram.o \
                                              $(OBJDIR)/gateway/platform_registry.o \
                                              $(OBJDIR)/gateway/gateway_ctx.o $(OBJDIR)/cli_v1_routes.o $(OBJDIR)/cli_v1_routes_b.o $(OBJDIR)/cli_v1_routes_c.o $(OBJDIR)/cli_v1_routes_d.o \
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
                                              $(OBJDIR)/posix/cli_client.o $(OBJDIR)/cli_v1_routes.o $(OBJDIR)/cli_v1_routes_b.o $(OBJDIR)/cli_v1_routes_c.o $(OBJDIR)/cli_v1_routes_d.o $(OBJDIR)/aimee_tls.o $(OBJDIR)/codex_auth.o \
                                              $(OBJDIR)/proxy_bootstrap.o \
                                              $(OBJDIR)/cJSON.o \
                                              $(OBJDIR)/log.o \
                                              $(OBJDIR)/platform_random.o \
                                              $(GATEWAY_PLATFORM_OBJS)
	$(TESTLINK) -o $@ $^ $(L_GATEWAY)

$(TESTPREFIX)/unit-test-mcp-gateway-tools: $(OBJDIR)/tests/test_mcp_gateway_tools.o \
                                            $(OBJDIR)/mcp_tools_gateway.o \
                                            $(OBJDIR)/server/server_mcp_gateway.o \
                                            $(OBJDIR)/delivery_target.o \
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
                                             $(OBJDIR)/cJSON.o \
                                             $(GATEWAY_PLATFORM_OBJS)
	$(TESTLINK) -o $@ $^ $(L_GATEWAY)

$(TESTPREFIX)/unit-test-cron-config: $(OBJDIR)/tests/test_cron_config.o \
                                     $(OBJDIR)/config_trigger.o $(OBJDIR)/platform_random.o \
                                     $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-cron-runtime: $(OBJDIR)/tests/test_cron_runtime.o \
                                      $(OBJDIR)/server/server_cron.o \
                                      $(OBJDIR)/server/trigger_scheduler.o \
                                      $(OBJDIR)/delivery_target.o \
                                      $(OBJDIR)/json_fluent.o \
                                      $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-report-enrichment: $(OBJDIR)/tests/test_report_enrichment.o \
                                           $(OBJDIR)/report_enrichment.o $(OBJDIR)/util_url.o \
                                           $(OBJDIR)/cJSON.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-hardware-probe: $(OBJDIR)/tests/test_hardware_probe.o \
                                        $(OBJDIR)/hardware_probe.o $(DB1_OBJS) \
                                        $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-curator-profile: $(OBJDIR)/tests/test_curator_profile.o \
                                         $(OBJDIR)/curator_profile.o \
                                         $(OBJDIR)/hardware_probe.o $(DB1_OBJS) \
                                         $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-client-cache: $(OBJDIR)/tests/test_kb_client_cache.o \
                                         $(OBJDIR)/server/kb_client_cache.o \
                                         $(OBJDIR)/log.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-openai-runs-store: $(OBJDIR)/tests/test_openai_runs_store.o \
                                           $(OBJDIR)/server/openai_runs_store.o \
                                           $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-cli-http-transport: $(OBJDIR)/tests/test_cli_http_transport.o \
                                            $(OBJDIR)/posix/cli_client.o $(OBJDIR)/cli_v1_routes.o $(OBJDIR)/cli_v1_routes_b.o $(OBJDIR)/cli_v1_routes_c.o $(OBJDIR)/cli_v1_routes_d.o $(OBJDIR)/aimee_tls.o $(OBJDIR)/codex_auth.o \
                                            $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-delegate-xml-fallback: $(OBJDIR)/tests/test_delegate_xml_fallback.o \
                                               $(OBJDIR)/server/delegate_xml_fallback.o \
                                               $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-agent-policy-intercept: $(OBJDIR)/tests/test_agent_policy_intercept.o \
                                                $(OBJDIR)/server/agent_policy_intercept.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-http-retry: $(OBJDIR)/tests/test_http_retry.o $(OBJDIR)/server/http_retry.o $(OBJDIR)/server/failover.o \
                            $(OBJDIR)/db1/interaction_events.o \
                            $(OBJDIR)/server/model_provider.o $(OBJDIR)/server/openai_profile.o \
                            $(OBJDIR)/server/anthropic_profile.o $(OBJDIR)/server/gemini_profile.o \
                            $(OBJDIR)/server/openrouter_profile.o $(OBJDIR)/server/ollama_profile.o \
                            $(OBJDIR)/server/llama_native_profile.o $(OBJDIR)/server/mistral_profile.o \
                            $(OBJDIR)/server/minimax_profile.o \
                            $(OBJDIR)/server/agent_bridge.o $(OBJDIR)/server/anthropic_shape.o $(OBJDIR)/server/tool_call_args.o $(OBJDIR)/server/agent_request_shaping.o \
                            $(OBJDIR)/posix/agent_bridge.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-cmd-doctor: $(OBJDIR)/tests/test_cmd_doctor.o $(OBJDIR)/cmd_doctor.o \
                      $(OBJDIR)/model_registry.o $(OBJDIR)/models_dev.o $(OBJDIR)/models_dev_cache.o \
                            $(OBJDIR)/hardware_probe.o \
                            $(DB2_OBJS) \
                            $(OBJDIR)/cmd_util.o $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA) \
                            $(OBJDIR)/client_integrations.o $(OBJDIR)/db1/secrets.o \
                            $(OBJDIR)/server/kb_client.o $(OBJDIR)/server/kb_client_cache.o $(OBJDIR)/server/kb_client_index.o $(OBJDIR)/code_collect.o $(OBJDIR)/server/kb_client_memory.o $(OBJDIR)/server/kb_client_memory_mutations.o $(OBJDIR)/server/kb_client_agent.o $(OBJDIR)/server/kb_client_dashboard.o $(OBJDIR)/server/kb_client_tasks.o $(OBJDIR)/server/kb_client_data.o $(OBJDIR)/cli_client.o $(OBJDIR)/cli_v1_routes.o $(OBJDIR)/cli_v1_routes_b.o $(OBJDIR)/cli_v1_routes_c.o $(OBJDIR)/cli_v1_routes_d.o $(OBJDIR)/posix/cli_client.o $(OBJDIR)/aimee_tls.o $(OBJDIR)/codex_auth.o \
                            $(OBJDIR)/mcp_git_query.o $(OBJDIR)/tests/support/git_cred_inject_stub.o $(OBJDIR)/forge_credentials.o $(OBJDIR)/server/git_host_resolve.o $(OBJDIR)/mcp_git_write.o \
                            $(OBJDIR)/mcp_git_branch.o $(OBJDIR)/mcp_git_pr.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-cmd-onboard: $(OBJDIR)/tests/test_cmd_onboard.o \
                      $(OBJDIR)/model_registry.o $(OBJDIR)/models_dev.o $(OBJDIR)/models_dev_cache.o \
                            $(OBJDIR)/cmd_onboard.o $(OBJDIR)/cmd_core.o $(OBJDIR)/cmd_init.o \
                            $(OBJDIR)/cmd_doctor.o $(OBJDIR)/hardware_probe.o $(OBJDIR)/cmd_util.o \
                            $(DB2_OBJS) \
                            $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA) \
                            $(OBJDIR)/client_integrations.o $(OBJDIR)/db1/secrets.o \
                            $(OBJDIR)/mcp_git_query.o $(OBJDIR)/tests/support/git_cred_inject_stub.o $(OBJDIR)/forge_credentials.o $(OBJDIR)/server/git_host_resolve.o $(OBJDIR)/mcp_git_write.o \
                            $(OBJDIR)/mcp_git_branch.o $(OBJDIR)/mcp_git_pr.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-diff: $(OBJDIR)/tests/test_diff.o $(OBJDIR)/diff.o $(OBJDIR)/dstr.o \
                      $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-tool-condense: $(OBJDIR)/tests/test_tool_condense.o $(OBJDIR)/tool_condense.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-workspace-provider: $(OBJDIR)/tests/test_workspace_provider.o \
                      $(OBJDIR)/posix/workspace_provider.o $(OBJDIR)/posix/util.o $(OBJDIR)/util.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-workspace-handle: $(OBJDIR)/tests/test_workspace_handle.o \
                      $(OBJDIR)/workspace_handle.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-forge-credentials: $(OBJDIR)/tests/test_forge_credentials.o \
                      $(OBJDIR)/forge_credentials.o
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
                      $(OBJDIR)/workspace_mirror.o $(OBJDIR)/aimee_home.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

# Gated live integration test (NOT in unit-tests / verify): drives the broker
# against a real authenticated git remote. Build + run via `make forge-cred-integration`.
$(TESTPREFIX)/forge-cred-live: $(OBJDIR)/tests/test_forge_credentials_live.o \
                      $(OBJDIR)/forge_credentials.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-workspace-provider-detached: \
                      $(OBJDIR)/tests/test_workspace_provider_detached.o \
                      $(OBJDIR)/server/workspace_provider_detached.o \
                      $(OBJDIR)/posix/workspace_provider.o $(OBJDIR)/posix/util.o $(OBJDIR)/util.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-webuser-runtime: \
                      $(OBJDIR)/tests/test_webuser_runtime.o \
                      $(OBJDIR)/server/webuser_runtime.o \
                      $(OBJDIR)/server/workspace_scope.o \
                      $(OBJDIR)/aimee_home.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-workspace-scope: \
                      $(OBJDIR)/tests/test_workspace_scope.o \
                      $(OBJDIR)/server/workspace_scope.o \
                      $(OBJDIR)/aimee_home.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-workspace-runner-queue: \
                      $(OBJDIR)/tests/test_workspace_runner_queue.o \
                      $(OBJDIR)/server/workspace_runner_queue.o \
                      $(OBJDIR)/server/workspace_provider_detached.o \
                      $(OBJDIR)/posix/workspace_provider.o $(OBJDIR)/posix/util.o $(OBJDIR)/util.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-workspace-runner-registry: \
                      $(OBJDIR)/tests/test_workspace_runner_registry.o \
                      $(OBJDIR)/server/workspace_runner_registry.o \
                      $(OBJDIR)/server/workspace_runner_queue.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-workspace-turn: $(OBJDIR)/tests/test_workspace_turn.o \
                      $(OBJDIR)/server/workspace_turn.o $(OBJDIR)/server/workspace_provider_container.o $(OBJDIR)/server/delegate_backend.o $(OBJDIR)/tests/support/git_cred_inject_stub.o \
                      $(OBJDIR)/server/workspace_provider_detached.o \
                      $(OBJDIR)/server/workspace_runner_registry.o \
                      $(OBJDIR)/server/workspace_runner_queue.o \
                      $(OBJDIR)/workspace_mirror.o $(OBJDIR)/forge_credentials.o $(OBJDIR)/server/git_host_resolve.o \
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
                             $(OBJDIR)/server/delegate_depth.o $(OBJDIR)/server/delegate_role.o \
                             $(OBJDIR)/role_templates.o \
                             $(OBJDIR)/server/delegate_prompt.o $(OBJDIR)/server/delegate_routing.o \
                             $(OBJDIR)/server/delegate_checkout.o $(OBJDIR)/cJSON.o \
                             $(OBJDIR)/util.o $(OBJDIR)/posix/platform_process.o $(OBJDIR)/posix/util.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-delegate-plan: $(OBJDIR)/tests/test_delegate_plan.o \
                             $(OBJDIR)/server/delegate_plan.o $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-delegate-role: $(OBJDIR)/tests/test_delegate_role.o \
                             $(OBJDIR)/server/delegate_role.o $(OBJDIR)/role_templates.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

# Trigger end-to-end: the REAL scan_proposals (textually included, real git
# subprocesses + real DB1) files a work item from a committed pending proposal
# and the real autonomy scheduler drives it to terminal (stub executors only).
$(TESTPREFIX)/unit-test-trigger-e2e: $(OBJDIR)/tests/test_trigger_e2e.o \
                                    $(OBJDIR)/server/wfe_scheduler.o $(OBJDIR)/workflow/wfe_autonomy.o \
                                    $(OBJDIR)/tests/support/log_stub.o \
                                    $(OBJDIR)/workflow/wfe_blocks.o $(OBJDIR)/workflow/wfe_engine.o $(OBJDIR)/tests/support/config_autonomy_stub.o \
                                    $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o \
                                    $(OBJDIR)/db1/wfe_store.o $(OBJDIR)/workflow/wfe_def.o \
                                    $(OBJDIR)/workflow/wfe_iface.o $(OBJDIR)/workflow/wfe_validate.o \
                                    $(OBJDIR)/workflow/wfe_canonical.o $(OBJDIR)/workflow/wfe_custom.o \
                                    $(OBJDIR)/workflow/wfe_roundtable.o $(OBJDIR)/workflow/wfe_approval.o \
                                    $(OBJDIR)/workflow/wfe_verdict.o $(OBJDIR)/workflow/wfe_deliver.o \
                                    $(OBJDIR)/workflow/wfe_manager_artifacts.o \
                                    $(OBJDIR)/aimee_home.o $(OBJDIR)/util.o $(OBJDIR)/posix/util.o \
                                    $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)
$(OBJDIR)/tests/test_trigger_e2e.o: tests/test_trigger_e2e.c server/trigger_scheduler.c
	@mkdir -p $(dir $@)
	$(CC) -c $(TEST_C_FLAGS) -I. -o $@ $<

$(TESTPREFIX)/unit-test-trigger: $(OBJDIR)/tests/test_trigger.o $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)
$(OBJDIR)/tests/test_trigger.o: tests/test_trigger.c server/trigger_scheduler.c
	@mkdir -p $(dir $@)
	$(CC) -c $(TEST_C_FLAGS) -I. -o $@ $<


$(TESTPREFIX)/unit-test-kb-maintenance: $(OBJDIR)/tests/test_kb_maintenance.o \
                             $(OBJDIR)/db2/kb_maintenance.o \
                             $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                             $(OBJDIR)/db2/feature_rows.o $(OBJDIR)/kb/kb_mdl.o \
                             $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                             $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm -lzstd

$(TESTPREFIX)/unit-test-kb-mining: $(OBJDIR)/tests/test_kb_mining.o \
                             $(OBJDIR)/kb/kb_mining.o $(OBJDIR)/kb/kb_background.o \
                             $(OBJDIR)/kb/kb_mdl.o \
                             $(OBJDIR)/kb/kb_reasoning.o \
                             $(OBJDIR)/db2/mining.o $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                             $(OBJDIR)/db2/feature_rows.o \
                             $(OBJDIR)/learning_evidence.o $(OBJDIR)/db2/learning_synth_ops.o \
                             $(OBJDIR)/db2/learning.o \
                             $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                             $(OBJDIR)/db2/feedback.o \
                             $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm -lzstd

$(TESTPREFIX)/unit-test-sse-parser: $(OBJDIR)/tests/test_sse_parser.o $(OBJDIR)/sse_parser.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-anthropic-ingress: $(OBJDIR)/tests/test_anthropic_ingress.o $(OBJDIR)/server/anthropic_ingress.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-anthropic-http: $(OBJDIR)/tests/test_anthropic_http.o $(OBJDIR)/server/gw_stage_memory.o $(OBJDIR)/server/anthropic_ingress.o $(OBJDIR)/sse_parser.o $(OBJDIR)/json_fluent.o $(OBJDIR)/cJSON.o $(OBJDIR)/gateway_pipeline.o $(OBJDIR)/tests/support/ir_ingress_stubs.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

# P2c (response-side tool policing) integration test: same source as
# unit-test-anthropic-http but linked against the REAL gateway_policy.o
# so the production police function runs against the driver's parsed
# response. The shape tests stub the request-side policy helpers so they
# don't have to deal with guardrails dependencies; this test exercises the
# full wiring end-to-end.
$(TESTPREFIX)/unit-test-anthropic-http-p2c: $(OBJDIR)/tests/test_anthropic_http_p2c.o $(OBJDIR)/server/gw_stage_memory.o $(OBJDIR)/server/anthropic_ingress.o $(OBJDIR)/sse_parser.o $(OBJDIR)/json_fluent.o $(OBJDIR)/cJSON.o $(OBJDIR)/gateway_pipeline.o $(OBJDIR)/gateway_policy.o $(OBJDIR)/tests/support/ir_ingress_stubs.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

# P2c streaming integration test: linked against the REAL gateway_policy.o
# so the streaming policy branch in messages_stream runs as in production.
# Same minimal-link pattern as the buffered P2c test above; the SSE replay
# helper + police function exercise the buffered-fetch + replay flow when
# `gateway_prevent_subagents` is ON.
$(TESTPREFIX)/unit-test-anthropic-http-streaming-p2c: $(OBJDIR)/tests/test_anthropic_http_streaming_p2c.o $(OBJDIR)/server/gw_stage_memory.o $(OBJDIR)/server/anthropic_ingress.o $(OBJDIR)/sse_parser.o $(OBJDIR)/json_fluent.o $(OBJDIR)/cJSON.o $(OBJDIR)/gateway_pipeline.o $(OBJDIR)/gateway_policy.o $(OBJDIR)/tests/support/ir_ingress_stubs.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-gateway-policy: $(OBJDIR)/tests/test_gateway_policy.o $(OBJDIR)/gateway_policy.o $(OBJDIR)/json_fluent.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-gateway-pipeline: $(OBJDIR)/tests/test_gateway_pipeline.o $(OBJDIR)/gateway_pipeline.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-gateway-p4-delegate: $(OBJDIR)/tests/test_gateway_p4_delegate.o $(OBJDIR)/gateway_delegate.o $(OBJDIR)/gateway_policy.o $(OBJDIR)/gateway_pipeline.o $(OBJDIR)/json_fluent.o $(OBJDIR)/cJSON.o
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

$(OBJDIR)/tests/server/kb_client_tool_registry.o: server/kb_client_tool_registry.c
	@mkdir -p $(dir $@)
	$(CC) -c $(TEST_C_FLAGS) -o $@ $<


$(TESTPREFIX)/unit-test-hud: $(OBJDIR)/tests/test_hud.o $(OBJDIR)/hud.o \
                      $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-history: $(OBJDIR)/tests/test_history.o $(OBJDIR)/history.o $(OBJDIR)/cJSON.o \
                         $(OBJDIR)/util.o $(OBJDIR)/text.o \
                         $(OBJDIR)/db1/db.o $(OBJDIR)/db1/db_schema.o $(OBJDIR)/tests/aimee_pg_sqlite_shim.o $(OBJDIR)/db2/db2_test_shim.o $(OBJDIR)/config.o $(OBJDIR)/config_sections.o $(OBJDIR)/config_database.o $(OBJDIR)/config_learning.o $(OBJDIR)/config_memory.o $(OBJDIR)/config_charter.o $(OBJDIR)/config_trigger.o $(OBJDIR)/config_kb_maintenance.o $(OBJDIR)/config_kb_curator.o $(OBJDIR)/config_server_api.o $(OBJDIR)/config_skills.o $(OBJDIR)/config_save.o \
                         $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o $(OBJDIR)/platform_random.o \
                         $(OBJDIR)/log.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-events: $(OBJDIR)/tests/test_events.o $(OBJDIR)/events.o \
                                $(OBJDIR)/delivery_target.o $(TEST_CORE_OBJS) \
                                $(OBJDIR)/posix/events.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-roadmap-auto: $(OBJDIR)/tests/test_roadmap_auto.o \
                            $(OBJDIR)/roadmap_milestone.o \
                            $(OBJDIR)/roadmap_reassess.o \
                            $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-coord-jobs: $(OBJDIR)/tests/test_coord_jobs.o \
                            $(OBJDIR)/server/delegate_economics.o \
                            $(OBJDIR)/server/delegate_prompt.o \
                            $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

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
                               $(OBJDIR)/skill.o $(OBJDIR)/skill_rollback.o $(TEST_CORE_OBJS) \
                               $(OBJDIR)/dstr.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-web-search: $(OBJDIR)/tests/test_web_search.o \
                            $(OBJDIR)/server/web_search.o $(TEST_CORE_OBJS) \
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

$(TESTPREFIX)/unit-test-coord-closet: $(OBJDIR)/tests/test_coord_closet.o $(OBJDIR)/coord_closet.o \
                                  $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-fold-budget: $(OBJDIR)/tests/test_fold_budget.o $(OBJDIR)/fold_budget.o \
                                  $(OBJDIR)/model_registry.o $(OBJDIR)/models_dev.o \
                                  $(OBJDIR)/models_dev_cache.o $(OBJDIR)/cJSON.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-context-fold: $(OBJDIR)/tests/test_context_fold.o $(OBJDIR)/context_fold.o $(OBJDIR)/fold_register.o \
                                  $(OBJDIR)/coord_closet.o $(OBJDIR)/compact.o $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o \
                                  $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-fold-register: $(OBJDIR)/tests/test_fold_register.o $(OBJDIR)/fold_register.o \
                                  $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-fold-recall: $(OBJDIR)/tests/test_fold_recall.o $(OBJDIR)/fold_recall.o \
                                  $(OBJDIR)/dstr.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-task-rail: $(OBJDIR)/tests/test_task_rail.o $(OBJDIR)/task_rail.o \
                                  $(OBJDIR)/cJSON.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-episode-seal: $(OBJDIR)/tests/test_episode_seal.o $(OBJDIR)/episode_seal.o \
                                  $(OBJDIR)/cJSON.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-compact-prune: $(OBJDIR)/tests/test_compact_prune.o \
                                          $(OBJDIR)/server/compact_prune.o \
                                          $(OBJDIR)/server/session_compact.o $(OBJDIR)/server/compact_prune.o \
                                          $(OBJDIR)/server/agent_bridge.o $(OBJDIR)/server/anthropic_shape.o $(OBJDIR)/server/tool_call_args.o \
                                          $(OBJDIR)/server/agent_request_shaping.o \
                                          $(OBJDIR)/server/delegate_driver.o \
                                          $(OBJDIR)/server/delegate_openai.o \
                                          $(OBJDIR)/server/delegate_gemini.o \
                                          $(OBJDIR)/server/delegate_xml_fallback.o \
                                          $(OBJDIR)/model_registry.o \
                                          $(OBJDIR)/server/agent_tools.o \
                                          $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-session-compact-focused: $(OBJDIR)/tests/test_session_compact_focused.o \
                                          $(OBJDIR)/server/session_compact.o $(OBJDIR)/server/compact_prune.o \
                                          $(OBJDIR)/server/agent_bridge.o $(OBJDIR)/server/anthropic_shape.o $(OBJDIR)/server/tool_call_args.o \
                                          $(OBJDIR)/server/agent_request_shaping.o \
                                          $(OBJDIR)/server/delegate_driver.o \
                                          $(OBJDIR)/server/delegate_openai.o \
                                          $(OBJDIR)/server/delegate_gemini.o \
                                          $(OBJDIR)/server/delegate_xml_fallback.o \
                                          $(OBJDIR)/model_registry.o \
                                          $(OBJDIR)/server/agent_tools.o \
                                          $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-session-compact: $(OBJDIR)/tests/test_session_compact.o \
                                          $(OBJDIR)/server/session_compact.o $(OBJDIR)/server/compact_prune.o \
                                          $(OBJDIR)/server/agent_bridge.o $(OBJDIR)/server/anthropic_shape.o $(OBJDIR)/server/tool_call_args.o \
                                          $(OBJDIR)/server/agent_request_shaping.o \
                                          $(OBJDIR)/server/delegate_driver.o \
                                          $(OBJDIR)/server/delegate_openai.o \
                                          $(OBJDIR)/server/delegate_gemini.o \
                                          $(OBJDIR)/server/delegate_xml_fallback.o \
                                          $(OBJDIR)/model_registry.o \
                                          $(OBJDIR)/server/agent_tools.o \
                                          $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-token-tracker: $(OBJDIR)/tests/test_token_tracker.o \
                               $(OBJDIR)/server/token_tracker.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-context-reduce: $(OBJDIR)/tests/test_context_reduce.o \
                               $(OBJDIR)/context_reduce.o $(OBJDIR)/server/token_tracker.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-reasoning-cap: $(OBJDIR)/tests/test_reasoning_cap.o \
                               $(OBJDIR)/reasoning_cap.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# typed-fact P1: pure ontology + write-gate (no DB), so a minimal link.
$(TESTPREFIX)/unit-test-rel-types: $(OBJDIR)/tests/test_rel_types.o \
                               $(OBJDIR)/rel_types.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-memory-fact-gate: $(OBJDIR)/tests/test_memory_fact_gate.o \
                               $(OBJDIR)/memory_fact_gate.o $(OBJDIR)/rel_types.o
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
$(TESTPREFIX)/unit-test-fact-lifecycle: $(OBJDIR)/tests/test_fact_lifecycle.o \
                               $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# embedder-runtime-fetch-autodim §2: kb_meta dim record + refuse-on-mismatch, shim.
$(TESTPREFIX)/unit-test-embedding-dim: $(OBJDIR)/tests/test_embedding_dim.o \
                               $(OBJDIR)/db2/db_schema.o $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# typed-fact P4: self-extending ontology promotion pipeline (§2), shim.
$(TESTPREFIX)/unit-test-ontology-evolution: $(OBJDIR)/tests/test_ontology_evolution.o \
                               $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# typed-fact P5: pattern-first extraction (§6) + retraction scan (§4). Pure.
$(TESTPREFIX)/unit-test-extract-patterns: $(OBJDIR)/tests/test_extract_patterns.o \
                               $(OBJDIR)/memory_extract_patterns.o $(OBJDIR)/rel_types.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# typed-fact P5: pattern-first ingest pipeline (§6 -> §1), shim.
$(TESTPREFIX)/unit-test-fact-ingest: $(OBJDIR)/tests/test_fact_ingest.o \
                               $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-audit-worm: $(OBJDIR)/tests/test_kb_audit_worm.o \
                               $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                               $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

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
                               $(OBJDIR)/memory_pii_gate.o $(OBJDIR)/rel_types.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-request-context: $(OBJDIR)/tests/test_request_context.o \
                               $(OBJDIR)/server/request_context.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-response-dedup: $(OBJDIR)/tests/test_response_dedup.o \
                               $(OBJDIR)/server/response_dedup.o
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
                                       $(OBJDIR)/server/delegate_prompt.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-agent-error-retryable: $(OBJDIR)/tests/test_agent_error_retryable.o \
                                       $(OBJDIR)/server/agent_fallback.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-delegate-ephemeral-ws: $(OBJDIR)/tests/test_delegate_ephemeral_ws.o \
                                       $(OBJDIR)/server/delegate_ephemeral_ws.o \
                                       $(OBJDIR)/aimee_home.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-delegate-handoff: $(OBJDIR)/tests/test_delegate_handoff.o \
                                       $(OBJDIR)/server/delegate_prompt.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-delegate-dispatch-reliability: \
                                       $(OBJDIR)/tests/test_delegate_dispatch_reliability.o \
                                       $(OBJDIR)/server/delegate_prompt.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-curator-code-unit: \
                                       $(OBJDIR)/tests/test_curator_code_unit.o \
                                       $(OBJDIR)/kb/kb_curator_queue.o \
                                       $(OBJDIR)/kb/kb_curator_extract_code.o \
                                       $(OBJDIR)/kb/kb_curator_extract.o \
                                       $(OBJDIR)/kb/kb_curator_sidecar.o \
                                       $(OBJDIR)/kb/kb_curator_grounding.o \
                                       $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                                       $(OBJDIR)/db2/feature_rows.o \
                                       $(OBJDIR)/kb/kb_mdl.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-curator-resolve-entities: \
                                       $(OBJDIR)/tests/test_curator_resolve_entities.o \
                                       $(OBJDIR)/kb/kb_curator_resolve_entities.o \
                                       $(OBJDIR)/kb_curator_provider.o \
                                       $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                                       $(OBJDIR)/db2/feature_rows.o \
                                       $(OBJDIR)/kb/kb_mdl.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-curator-index-narrative: \
                                       $(OBJDIR)/tests/test_curator_index_narrative.o \
                                       $(OBJDIR)/kb/kb_curator_index_narrative.o \
                                       $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                                       $(OBJDIR)/db2/feature_rows.o \
                                       $(OBJDIR)/kb/kb_mdl.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-curator-index-claims: \
                                       $(OBJDIR)/tests/test_curator_index_claims.o \
                                       $(OBJDIR)/kb/kb_curator_index_claims.o \
                                       $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                                       $(OBJDIR)/db2/feature_rows.o \
                                       $(OBJDIR)/kb/kb_mdl.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-curator-contradictions: \
                                       $(OBJDIR)/tests/test_curator_contradictions.o \
                                       $(OBJDIR)/kb/kb_curator_contradictions.o \
                                       $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                                       $(OBJDIR)/db2/feature_rows.o \
                                       $(OBJDIR)/kb/kb_mdl.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-curator-index-code-unit: \
                                       $(OBJDIR)/tests/test_curator_index_code_unit.o \
                                       $(OBJDIR)/kb/kb_curator_index_code_unit.o \
                                       $(OBJDIR)/db2/kb_runtime_state.o \
                                       $(OBJDIR)/tests/support/kb_txn_stub.o \
                                       $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                                       $(OBJDIR)/db2/feature_rows.o \
                                       $(OBJDIR)/kb/kb_mdl.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-curator-pipeline: \
                                       $(OBJDIR)/tests/test_curator_pipeline.o \
                                       $(OBJDIR)/kb/kb_curator_resolve_entities.o \
                                       $(OBJDIR)/kb_curator_provider.o \
                                       $(OBJDIR)/kb/kb_curator_index_code_unit.o \
                                       $(OBJDIR)/db2/kb_runtime_state.o \
                                       $(OBJDIR)/tests/support/kb_txn_stub.o \
                                       $(OBJDIR)/kb/kb_curator_link_artifacts.o \
                                       $(OBJDIR)/kb/kb_curator_serve.o \
                                       $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                                       $(OBJDIR)/db2/feature_rows.o \
                                       $(OBJDIR)/kb/kb_mdl.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-curator-serve: \
                                       $(OBJDIR)/tests/test_curator_serve.o \
                                       $(OBJDIR)/kb/kb_curator_serve.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-curator-link-artifacts: \
                                       $(OBJDIR)/tests/test_curator_link_artifacts.o \
                                       $(OBJDIR)/kb/kb_curator_link_artifacts.o \
                                       $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                                       $(OBJDIR)/db2/feature_rows.o \
                                       $(OBJDIR)/kb/kb_mdl.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

# LLM judge sidecar — request build / invoke / parse round-trip via tiny shell
# "sidecars". Only depends on cJSON; no DB link.
$(TESTPREFIX)/unit-test-curator-judge: \
                                       $(OBJDIR)/tests/test_curator_judge.o \
                                       $(OBJDIR)/kb/kb_curator_judge.o \
                                       $(OBJDIR)/kb/kb_curator_sidecar.o \
                                       $(OBJDIR)/kb/kb_curator_llm.o \
                                       $(OBJDIR)/kb_curator_provider.o \
                                       $(OBJDIR)/provider_client.o \
                                       $(OBJDIR)/tests/support/mock_agent_http.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# §4 surprising-links judge: real request-build + verdict-parse; DB accessors stubbed
# in the test, the LLM faked via the curator sidecar seam (cfg=NULL + printf judge_cmd).
$(TESTPREFIX)/unit-test-kb-surprising-judge: \
                                       $(OBJDIR)/tests/test_kb_surprising_judge.o \
                                       $(OBJDIR)/kb/kb_surprising_judge.o \
                                       $(OBJDIR)/kb/kb_curator_sidecar.o \
                                       $(OBJDIR)/kb/kb_curator_llm.o \
                                       $(OBJDIR)/kb_curator_provider.o \
                                       $(OBJDIR)/provider_client.o \
                                       $(OBJDIR)/tests/support/mock_agent_http.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# synthesize_topic — graceful drain entry under the sqlite shim (gated off).
$(TESTPREFIX)/unit-test-curator-synthesize: \
                                       $(OBJDIR)/tests/test_curator_synthesize.o \
                                       $(OBJDIR)/kb/kb_curator_synthesize.o \
                                       $(OBJDIR)/kb/kb_curator_sidecar.o \
                                       $(OBJDIR)/kb/kb_curator_llm.o \
                                       $(OBJDIR)/kb_curator_provider.o \
                                       $(OBJDIR)/provider_client.o \
                                       $(OBJDIR)/tests/support/mock_agent_http.o \
                                       $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                                       $(OBJDIR)/db2/feature_rows.o \
                                       $(OBJDIR)/kb/kb_mdl.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o \
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
                                       $(OBJDIR)/kb/kb_curator_sidecar.o \
                                       $(OBJDIR)/kb/kb_curator_llm.o \
                                       $(OBJDIR)/kb_curator_provider.o \
                                       $(OBJDIR)/provider_client.o \
                                       $(OBJDIR)/tests/support/mock_agent_http.o \
                                       $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                                       $(OBJDIR)/db2/feature_rows.o \
                                       $(OBJDIR)/kb/kb_mdl.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

# promote_entity — pure scope-lattice step + graceful drain entry (gated off).
$(TESTPREFIX)/unit-test-curator-promote: \
                                       $(OBJDIR)/tests/test_curator_promote.o \
                                       $(OBJDIR)/kb/kb_curator_promote.o \
                                       $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                                       $(OBJDIR)/db2/feature_rows.o \
                                       $(OBJDIR)/kb/kb_mdl.o \
                                       $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o \
                                       $(OBJDIR)/db2/db_schema.o \
                                       $(OBJDIR)/cJSON.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

# Curator fixture corpus (benchmarks/curator/) — schema + behavioral grounding
# checks. The fixture dir is absolute so the test runs from any cwd.
$(OBJDIR)/tests/test_curator_fixtures.o: C_FLAGS += -DCURATOR_FIXTURE_DIR=\"$(CURDIR)/../benchmarks/curator/fixtures\"
$(TESTPREFIX)/unit-test-curator-fixtures: \
                                       $(OBJDIR)/tests/test_curator_fixtures.o \
                                       $(OBJDIR)/kb/kb_curator_grounding.o \
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
                                       $(OBJDIR)/db1/db1_write.o \
                                       $(OBJDIR)/db1/db_schema.o \
                                       $(OBJDIR)/db1/db.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-db1-agent-job-heartbeat: \
                                       $(OBJDIR)/tests/test_db1_agent_job_heartbeat.o \
                                       $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o \
                                       $(OBJDIR)/db1/agent_jobs.o $(OBJDIR)/db1/agent_log.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-server-delegate-monitor: \
                                       $(OBJDIR)/tests/test_server_delegate_monitor.o \
                                       $(OBJDIR)/server/server_delegate_monitor.o \
                                       $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o \
                                       $(OBJDIR)/db1/agent_jobs.o $(OBJDIR)/log.o \
                                       $(OBJDIR)/dstr.o $(OBJDIR)/util.o $(OBJDIR)/text.o \
                                       $(OBJDIR)/platform_random.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-db1-delegation-recursive-cancel: \
                                       $(OBJDIR)/tests/test_db1_delegation_recursive_cancel.o \
                                       $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o \
                                       $(OBJDIR)/db1/delegations.o
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
                                       $(OBJDIR)/server/workspace_provider_container.o \
                                       $(OBJDIR)/posix/workspace_provider.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-toolset: \
                                       $(OBJDIR)/tests/test_toolset.o \
                                       $(OBJDIR)/toolset.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-db1-cost-fold: \
                                       $(OBJDIR)/tests/test_db1_cost_fold.o \
                                       $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o \
                                       $(OBJDIR)/db1/cost_fold.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-db1-roundtable-pipeline: \
                                       $(OBJDIR)/tests/test_db1_roundtable_pipeline.o \
                                       $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o \
                                       $(OBJDIR)/db1/roundtable_pipeline.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-roundtable-pipeline-eval: \
                                       $(OBJDIR)/tests/test_roundtable_pipeline_eval.o \
                                       $(OBJDIR)/server/roundtable_pipeline_eval.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-roundtable-pipeline-chunk: \
                                       $(OBJDIR)/tests/test_roundtable_pipeline_chunk.o \
                                       $(OBJDIR)/server/roundtable_pipeline_chunk.o \
                                       $(OBJDIR)/server/roundtable_pipeline_eval.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-roundtable-pipeline-ctl: \
                                       $(OBJDIR)/tests/test_roundtable_pipeline_ctl.o \
                                       $(OBJDIR)/server/server_pipeline.o $(OBJDIR)/server/server_pipeline_merge.o \
                                       $(OBJDIR)/server/git_pr_ci_grade.o \
                                       $(OBJDIR)/server/roundtable_pipeline_eval.o \
                                       $(OBJDIR)/server/roundtable_pipeline_chunk.o \
                                       $(OBJDIR)/db1/roundtable_pipeline.o \
                                       $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o \
                                       $(OBJDIR)/db1/local_operator.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-roundtable-pipeline-capture: \
                                       $(OBJDIR)/tests/test_roundtable_pipeline_capture.o \
                                       $(OBJDIR)/server/roundtable_pipeline_capture.o \
                                       $(OBJDIR)/server/roundtable_pipeline_eval.o \
                                       $(OBJDIR)/db1/roundtable_pipeline.o \
                                       $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o \
                                       $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-db1-session-paths: \
                                       $(OBJDIR)/tests/test_db1_session_paths.o \
                                       $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o \
                                       $(OBJDIR)/db1/session_paths.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-delegate-credentials: \
                                       $(OBJDIR)/tests/test_delegate_credentials.o \
                                       $(OBJDIR)/server/delegate_credentials.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-delegate-economics: $(OBJDIR)/tests/test_delegate_economics.o \
                                       $(OBJDIR)/server/delegate_economics.o \
                                       $(OBJDIR)/server/delegate_prompt.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-delegate-patch-coordinator: $(OBJDIR)/tests/test_delegate_patch_coordinator.o \
                                       $(OBJDIR)/server/delegate_patch_coordinator.o \
                                       $(OBJDIR)/server/delegate_prompt.o \
                                       $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-delegate-ensemble: $(OBJDIR)/tests/test_delegate_ensemble.o \
                                       $(OBJDIR)/server/delegate_ensemble.o $(OBJDIR)/server/delegate_ensemble_review.o \
                                       $(OBJDIR)/server/roundtable_verify.o \
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

$(TESTPREFIX)/unit-test-workspace-manifest: $(OBJDIR)/tests/test_workspace_manifest.o \
                                     $(OBJDIR)/workspace_manifest.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-lsp: $(OBJDIR)/tests/test_lsp.o \
                              $(OBJDIR)/lsp_client.o \
                              $(OBJDIR)/lsp_manager.o \
                              $(OBJDIR)/db1/db.o $(OBJDIR)/db1/db_schema.o $(OBJDIR)/db1/maintenance.o $(OBJDIR)/tests/aimee_pg_sqlite_shim.o $(OBJDIR)/db2/db2_test_shim.o \
                              $(OBJDIR)/config.o $(OBJDIR)/config_sections.o $(OBJDIR)/config_database.o $(OBJDIR)/config_learning.o $(OBJDIR)/config_memory.o $(OBJDIR)/config_charter.o $(OBJDIR)/config_trigger.o $(OBJDIR)/config_kb_maintenance.o $(OBJDIR)/config_kb_curator.o $(OBJDIR)/config_server_api.o $(OBJDIR)/config_skills.o $(OBJDIR)/config_save.o $(OBJDIR)/config_mode.o $(OBJDIR)/yaml.o $(OBJDIR)/dstr.o \
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
                              $(OBJDIR)/server/vault_principal.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-vault-crypto: $(OBJDIR)/tests/test_vault_crypto.o \
                              $(OBJDIR)/server/vault_crypto.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto

$(TESTPREFIX)/unit-test-evidence-replay: $(OBJDIR)/tests/test_evidence_replay.o \
                              $(OBJDIR)/server/evidence_replay.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto

# Live forge: the pure check-runs/combined-status -> CI verdict aggregation.
$(TESTPREFIX)/unit-test-git-pr-ci-grade: $(OBJDIR)/tests/test_git_pr_ci_grade.o \
                              $(OBJDIR)/server/git_pr_ci_grade.o $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-roundtable-verify: $(OBJDIR)/tests/test_roundtable_verify.o \
                              $(OBJDIR)/server/roundtable_verify.o $(OBJDIR)/server/evidence_replay.o \
                              $(OBJDIR)/dstr.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto

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
                              $(OBJDIR)/server/vault_kek_cache.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL) -lcrypto

$(TESTPREFIX)/unit-test-vault-store: $(OBJDIR)/tests/test_vault_store.o \
                              $(OBJDIR)/server/vault_store.o $(OBJDIR)/server/vault_crypto.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-git-ops: $(OBJDIR)/tests/test_git_ops.o \
                              $(OBJDIR)/server/git_ops.o $(OBJDIR)/tests/support/git_pr_api_stub.o $(OBJDIR)/server/git_cred_inject.o $(OBJDIR)/server/git_ssh_agent.o $(OBJDIR)/server/webuser_runtime.o \
                              $(OBJDIR)/server/git_forge_vault.o $(OBJDIR)/server/git_host_cred.o $(OBJDIR)/server/git_host_resolve.o $(OBJDIR)/server/workspace_scope.o \
                              $(OBJDIR)/forge_credentials.o $(OBJDIR)/config.o $(OBJDIR)/aimee_home.o $(OBJDIR)/util_url.o \
                              $(OBJDIR)/posix/util.o $(OBJDIR)/util.o \
                              $(OBJDIR)/server/vault_service.o $(OBJDIR)/server/vault_store.o \
                              $(OBJDIR)/server/vault_crypto.o $(OBJDIR)/server/vault_kek_cache.o \
                              $(OBJDIR)/server/vault_server_key.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-git-project: $(OBJDIR)/tests/test_git_project.o \
                              $(OBJDIR)/tests/support/kb_purge_stub.o \
                              $(OBJDIR)/server/git_project.o $(OBJDIR)/server/ws_registry.o $(OBJDIR)/server/git_cred_inject.o $(OBJDIR)/server/git_ssh_agent.o $(OBJDIR)/server/webuser_runtime.o \
                              $(OBJDIR)/server/git_forge_vault.o $(OBJDIR)/server/git_host_cred.o $(OBJDIR)/server/git_host_resolve.o $(OBJDIR)/server/workspace_scope.o \
                              $(OBJDIR)/forge_credentials.o $(OBJDIR)/util_url.o $(OBJDIR)/config.o $(OBJDIR)/aimee_home.o \
                              $(OBJDIR)/posix/util.o $(OBJDIR)/util.o \
                              $(OBJDIR)/server/vault_service.o $(OBJDIR)/server/vault_store.o \
                              $(OBJDIR)/server/vault_crypto.o $(OBJDIR)/server/vault_kek_cache.o \
                              $(OBJDIR)/server/vault_server_key.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-webchat-git-leak: $(OBJDIR)/tests/test_webchat_git_leak.o \
                              $(OBJDIR)/server/git_cred_inject.o $(OBJDIR)/server/git_ssh_agent.o $(OBJDIR)/server/webuser_runtime.o $(OBJDIR)/server/workspace_scope.o $(OBJDIR)/server/git_forge_vault.o \
                              $(OBJDIR)/server/git_host_cred.o $(OBJDIR)/server/git_host_resolve.o $(OBJDIR)/util_url.o $(OBJDIR)/posix/util.o $(OBJDIR)/util.o \
                              $(OBJDIR)/forge_credentials.o $(OBJDIR)/config.o $(OBJDIR)/aimee_home.o \
                              $(OBJDIR)/server/vault_service.o $(OBJDIR)/server/vault_store.o \
                              $(OBJDIR)/server/vault_crypto.o $(OBJDIR)/server/vault_kek_cache.o \
                              $(OBJDIR)/server/vault_server_key.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-git-ssh-agent: $(OBJDIR)/tests/test_git_ssh_agent.o \
                              $(OBJDIR)/server/git_ssh_agent.o $(OBJDIR)/server/git_forge_vault.o \
                              $(OBJDIR)/server/webuser_runtime.o $(OBJDIR)/server/workspace_scope.o \
                              $(OBJDIR)/config.o $(OBJDIR)/aimee_home.o \
                              $(OBJDIR)/server/vault_service.o $(OBJDIR)/server/vault_store.o \
                              $(OBJDIR)/server/vault_crypto.o $(OBJDIR)/server/vault_kek_cache.o \
                              $(OBJDIR)/server/vault_server_key.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-git-cred-inject: $(OBJDIR)/tests/test_git_cred_inject.o \
                              $(OBJDIR)/server/git_cred_inject.o $(OBJDIR)/server/git_ssh_agent.o $(OBJDIR)/server/webuser_runtime.o $(OBJDIR)/server/workspace_scope.o $(OBJDIR)/server/git_forge_vault.o \
                              $(OBJDIR)/server/git_host_cred.o $(OBJDIR)/server/git_host_resolve.o $(OBJDIR)/util_url.o $(OBJDIR)/posix/util.o $(OBJDIR)/util.o \
                              $(OBJDIR)/forge_credentials.o $(OBJDIR)/config.o $(OBJDIR)/aimee_home.o \
                              $(OBJDIR)/server/vault_service.o $(OBJDIR)/server/vault_store.o \
                              $(OBJDIR)/server/vault_crypto.o $(OBJDIR)/server/vault_kek_cache.o \
                              $(OBJDIR)/server/vault_server_key.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-webuser-editor: $(OBJDIR)/tests/test_webuser_editor.o \
                              $(OBJDIR)/server/webuser_editor.o $(OBJDIR)/server/git_cred_inject.o \
                              $(OBJDIR)/server/git_ssh_agent.o $(OBJDIR)/server/webuser_runtime.o \
                              $(OBJDIR)/server/workspace_scope.o $(OBJDIR)/server/git_forge_vault.o \
                              $(OBJDIR)/server/git_host_cred.o $(OBJDIR)/server/git_host_resolve.o $(OBJDIR)/util_url.o $(OBJDIR)/posix/util.o $(OBJDIR)/util.o \
                              $(OBJDIR)/forge_credentials.o $(OBJDIR)/config.o $(OBJDIR)/aimee_home.o \
                              $(OBJDIR)/server/vault_service.o $(OBJDIR)/server/vault_store.o \
                              $(OBJDIR)/server/vault_crypto.o $(OBJDIR)/server/vault_kek_cache.o \
                              $(OBJDIR)/server/vault_server_key.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-git-forge-vault: $(OBJDIR)/tests/test_git_forge_vault.o \
                              $(OBJDIR)/server/git_forge_vault.o \
                              $(OBJDIR)/server/vault_service.o $(OBJDIR)/server/vault_store.o \
                              $(OBJDIR)/server/vault_crypto.o $(OBJDIR)/server/vault_kek_cache.o \
                              $(OBJDIR)/server/vault_server_key.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-git-host-resolve: $(OBJDIR)/tests/test_git_host_resolve.o \
                              $(OBJDIR)/server/git_host_resolve.o \
                              $(OBJDIR)/posix/util.o $(OBJDIR)/util.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-vault-service: $(OBJDIR)/tests/test_vault_service.o \
                              $(OBJDIR)/server/vault_service.o $(OBJDIR)/server/vault_store.o \
                              $(OBJDIR)/server/vault_crypto.o $(OBJDIR)/server/vault_kek_cache.o \
                              $(OBJDIR)/server/vault_server_key.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-vault-master-rotate: $(OBJDIR)/tests/test_vault_master_rotate.o \
                              $(OBJDIR)/server/vault_service.o $(OBJDIR)/server/vault_store.o \
                              $(OBJDIR)/server/vault_crypto.o $(OBJDIR)/server/vault_kek_cache.o \
                              $(OBJDIR)/server/vault_server_key.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-agent-key-import: $(OBJDIR)/tests/test_agent_key_import.o \
                              $(OBJDIR)/cli_agent_keys.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-vault-bootstrap: $(OBJDIR)/tests/test_vault_bootstrap.o \
                              $(OBJDIR)/server/server_vault_bootstrap.o \
                              $(OBJDIR)/server/vault_service.o $(OBJDIR)/server/vault_store.o \
                              $(OBJDIR)/server/vault_crypto.o $(OBJDIR)/server/vault_kek_cache.o \
                              $(OBJDIR)/server/vault_server_key.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-pki: $(OBJDIR)/tests/test_pki.o $(OBJDIR)/server/pki.o \
                              $(OBJDIR)/server/vault_service.o $(OBJDIR)/server/vault_store.o \
                              $(OBJDIR)/server/vault_crypto.o $(OBJDIR)/server/vault_kek_cache.o \
                              $(OBJDIR)/server/vault_server_key.o $(OBJDIR)/server/vault_principal.o \
                              $(DB1_OBJS) \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-vault-server-key: $(OBJDIR)/tests/test_vault_server_key.o \
                              $(OBJDIR)/server/vault_server_key.o $(OBJDIR)/server/vault_crypto.o \
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
                              $(OBJDIR)/server/vault_capability.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-vault-audit: $(OBJDIR)/tests/test_vault_audit.o \
                              $(OBJDIR)/server/server_vault.o \
                              $(OBJDIR)/server/vault_service.o $(OBJDIR)/server/vault_store.o \
                              $(OBJDIR)/server/vault_crypto.o $(OBJDIR)/server/vault_kek_cache.o \
                              $(OBJDIR)/server/vault_server_key.o $(OBJDIR)/server/vault_capability.o \
                              $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-prompts: $(OBJDIR)/tests/test_prompts.o \
                           $(OBJDIR)/prompts.o $(OBJDIR)/dstr.o $(OBJDIR)/working_profile.o \
                           $(DB1_OBJS) \
                           $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-persona: $(OBJDIR)/tests/test_persona.o \
                           $(OBJDIR)/persona.o $(OBJDIR)/prompts.o $(OBJDIR)/dstr.o \
                           $(OBJDIR)/working_profile.o \
                           $(DB1_OBJS) \
                           $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-server-http: $(OBJDIR)/tests/test_server_http.o \
                      $(OBJDIR)/model_registry.o $(OBJDIR)/models_dev.o $(OBJDIR)/models_dev_cache.o \
                           $(OBJDIR)/server/server_http.o $(OBJDIR)/server/server_http_routes.o $(OBJDIR)/server/server_http_routes_git.o $(OBJDIR)/server/server_dev_submit.o $(OBJDIR)/server/server_ci_route.o $(OBJDIR)/server/server_http_config_routes.o $(OBJDIR)/server/server_http_conn_worker.o $(OBJDIR)/server/server_http_response.o $(OBJDIR)/server/server_http_sse.o $(OBJDIR)/server/server_http_reqctx.o $(OBJDIR)/server/server_http_identity.o $(OBJDIR)/tests/support/git_route_stub.o $(OBJDIR)/tests/support/workflow_api_stub.o $(OBJDIR)/tests/support/router_advise_stub.o $(OBJDIR)/server/vault_principal.o $(OBJDIR)/server/presence.o \
                           $(OBJDIR)/server/cli_session_pty.o $(OBJDIR)/server/cli_session.o $(OBJDIR)/posix/workspace_provider.o \
                           $(OBJDIR)/server/workspace_runner_registry.o $(OBJDIR)/server/workspace_runner_queue.o \
                           $(OBJDIR)/forge_credentials.o \
                           $(OBJDIR)/delivery_target.o \
                           $(OBJDIR)/server/openai_shape.o \
                           $(OBJDIR)/server/openai_runs_store.o $(OBJDIR)/server/server_auth.o \
                           $(OBJDIR)/server/compute_pool.o \
                           $(OBJDIR)/server/agent_config.o $(OBJDIR)/tests/support/vault_service_stub.o $(OBJDIR)/tests/support/oauth_tokens_stub.o \
                           $(OBJDIR)/persona.o $(OBJDIR)/prompts.o \
                           $(OBJDIR)/roundtable_preset.o \
                           $(OBJDIR)/role_templates.o \
                           $(OBJDIR)/dstr.o $(OBJDIR)/working_profile.o \
                           $(OBJDIR)/server/roundtable_pipeline_capture.o \
                           $(OBJDIR)/server/roundtable_pipeline_eval.o \
                           $(DB1_OBJS) \
                           $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-openai-shape: $(OBJDIR)/tests/test_openai_shape.o \
                           $(OBJDIR)/server/openai_shape.o \
                           $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-openai-chat-policed: $(OBJDIR)/tests/test_openai_chat_policed.o \
                           $(OBJDIR)/server/openai_shape.o \
                           $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-openai-responses-store: $(OBJDIR)/tests/test_openai_responses_store.o \
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

$(TESTPREFIX)/unit-test-models-dev: $(OBJDIR)/tests/test_models_dev.o \
                                $(OBJDIR)/model_registry.o $(OBJDIR)/models_dev.o \
                                $(OBJDIR)/models_dev_cache.o $(OBJDIR)/aimee_home.o \
                                $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-model-provider: $(OBJDIR)/tests/test_model_provider.o \
                                $(OBJDIR)/server/model_provider.o \
                                $(OBJDIR)/server/openai_profile.o \
                                $(OBJDIR)/server/anthropic_profile.o \
                                $(OBJDIR)/server/gemini_profile.o \
                                $(OBJDIR)/server/openrouter_profile.o \
                                $(OBJDIR)/server/ollama_profile.o \
                                $(OBJDIR)/server/llama_native_profile.o \
                                $(OBJDIR)/server/mistral_profile.o \
                                $(OBJDIR)/server/minimax_profile.o \
                                $(OBJDIR)/tests/support/mock_agent_http.o \
                                $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-delegate-driver: $(OBJDIR)/tests/test_delegate_driver.o \
                                 $(OBJDIR)/server/delegate_driver.o \
                                 $(OBJDIR)/server/agent_request_shaping.o \
                                 $(OBJDIR)/server/delegate_openai.o \
                                 $(OBJDIR)/server/delegate_gemini.o \
                                 $(OBJDIR)/server/delegate_xml_fallback.o \
                                 $(OBJDIR)/model_registry.o \
                                 $(OBJDIR)/server/agent_tools.o \
                                 $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-agent-http: $(OBJDIR)/tests/test_agent_http.o \
                                 $(OBJDIR)/models_dev.o $(OBJDIR)/models_dev_cache.o \
                                $(OBJDIR)/server/agent_bridge.o $(OBJDIR)/server/anthropic_shape.o $(OBJDIR)/server/tool_call_args.o \
                                $(OBJDIR)/server/agent_request_shaping.o \
                                $(OBJDIR)/server/delegate_driver.o \
                                $(OBJDIR)/server/delegate_openai.o \
                                $(OBJDIR)/server/delegate_gemini.o \
                                $(OBJDIR)/server/delegate_xml_fallback.o \
                                $(OBJDIR)/model_registry.o \
                                $(OBJDIR)/server/agent_tools.o \
                                $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Links the real dispatch TU against the shared test object sets — no stubs needed
# (TEST_DATA_OBJS + TEST_WORKSPACE_OBJS_EXTRA already cover every td_* handler's
# dependencies). The provider itself is faked in the test: the point under test is
# the routing, not any MCP handler.
$(TESTPREFIX)/unit-test-mcp-native-dispatch: \
                                       $(OBJDIR)/tests/test_mcp_native_dispatch.o \
                                       $(OBJDIR)/posix/agent_tools_dispatch.o \
                                       $(OBJDIR)/server/agent_tools.o \
                                       $(OBJDIR)/toolset.o \
                                       $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-mcp-native-surface: $(OBJDIR)/tests/test_mcp_native_surface.o \
                                 $(OBJDIR)/models_dev.o $(OBJDIR)/models_dev_cache.o \
                                $(OBJDIR)/server/agent_bridge.o $(OBJDIR)/server/anthropic_shape.o $(OBJDIR)/server/tool_call_args.o \
                                $(OBJDIR)/server/agent_request_shaping.o \
                                $(OBJDIR)/server/delegate_driver.o \
                                $(OBJDIR)/server/delegate_openai.o \
                                $(OBJDIR)/server/delegate_gemini.o \
                                $(OBJDIR)/server/delegate_xml_fallback.o \
                                $(OBJDIR)/model_registry.o \
                                $(OBJDIR)/server/agent_tools.o \
                                $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-middleware: $(OBJDIR)/tests/test_middleware.o $(OBJDIR)/log.o \
                                    $(OBJDIR)/model_registry.o \
                                    $(PLATFORM_BASIC_OBJS)
	$(TESTLINK_MIN) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-verify-hook: $(OBJDIR)/tests/test_verify_hook.o \
	                                     $(OBJDIR)/git_verify.o $(OBJDIR)/git_verify_state.o $(OBJDIR)/git_verify_config.o $(OBJDIR)/git_verify_jobs.o $(OBJDIR)/git_verify_hook.o $(OBJDIR)/git_verify_ops.o $(OBJDIR)/git_verify_select.o $(OBJDIR)/git_verify_step.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-pipeline: $(OBJDIR)/tests/test_pipeline.o \
                                   $(OBJDIR)/server/agent_pipeline.o \
	                                   $(OBJDIR)/git_verify.o $(OBJDIR)/git_verify_state.o $(OBJDIR)/git_verify_config.o $(OBJDIR)/git_verify_jobs.o $(OBJDIR)/git_verify_hook.o $(OBJDIR)/git_verify_ops.o $(OBJDIR)/git_verify_select.o $(OBJDIR)/git_verify_step.o \
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
                                        $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o $(OBJDIR)/db1/wm.o $(OBJDIR)/db1/fsnap.o \
                                        $(OBJDIR)/log.o $(OBJDIR)/util.o $(OBJDIR)/platform_random.o \
                                        $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-execution-trace: $(OBJDIR)/tests/test_execution_trace.o \
                                          $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o $(OBJDIR)/db1/execution_trace.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-diagnose: $(OBJDIR)/tests/test_diagnose.o \
                                   $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o $(OBJDIR)/db1/diagnose.o \
                                   $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm

$(TESTPREFIX)/unit-test-clarify: $(OBJDIR)/tests/test_clarify.o \
                                  $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o $(OBJDIR)/db1/clarify.o \
                                  $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# DB1 server-owned per-model price table (ingress-cost-accounting §2).
$(TESTPREFIX)/unit-test-model-pricing: $(OBJDIR)/tests/test_model_pricing.o \
                                  $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o \
                                  $(OBJDIR)/db1/model_pricing.o \
                                  $(OBJDIR)/dstr.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-provider-client: $(OBJDIR)/tests/test_provider_client.o \
                                  $(OBJDIR)/provider_client.o $(OBJDIR)/cJSON.o \
                                  $(OBJDIR)/tests/support/mock_agent_http.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-curator-provider: $(OBJDIR)/tests/test_kb_curator_provider.o \
                                  $(OBJDIR)/kb_curator_provider.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-curator-llm: $(OBJDIR)/tests/test_kb_curator_llm.o \
                                  $(OBJDIR)/kb/kb_curator_llm.o $(OBJDIR)/kb/kb_curator_sidecar.o \
                                  $(OBJDIR)/kb_curator_provider.o $(OBJDIR)/provider_client.o \
                                  $(OBJDIR)/cJSON.o $(OBJDIR)/tests/support/mock_agent_http.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-collab-rules: $(OBJDIR)/tests/test_collab_rules.o $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-json-fluent: $(OBJDIR)/tests/test_json_fluent.o $(OBJDIR)/json_fluent.o \
                                      $(OBJDIR)/db1/db.o $(OBJDIR)/db1/db_schema.o \
                                      $(OBJDIR)/tests/aimee_pg_sqlite_shim.o $(OBJDIR)/db2/db2_test_shim.o $(OBJDIR)/cJSON.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-cmd-config: $(OBJDIR)/tests/test_cmd_config.o $(OBJDIR)/cmd_data.o \
                                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-cmd-table: $(OBJDIR)/tests/test_cmd_table.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-tool-validation: $(OBJDIR)/tests/test_tool_validation.o \
                                          $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-turn-narration: $(OBJDIR)/tests/test_turn_narration.o \
                                         $(OBJDIR)/turn_narration.o $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-kb: $(OBJDIR)/tests/test_kb.o $(OBJDIR)/kb/kb.o $(OBJDIR)/db2/kb_runtime_state.o $(OBJDIR)/kb/kb_ingest_workers.o $(OBJDIR)/kb/kb_bandit.o $(OBJDIR)/kb/kb_bandit_registry.o $(OBJDIR)/db2/bandit.o $(OBJDIR)/kb/kb_curator_notify.o $(OBJDIR)/kb/kb_fusion.o $(OBJDIR)/kb/kb_neardup.o $(OBJDIR)/kb/kb_conventions.o \
                             $(OBJDIR)/sketch.o $(OBJDIR)/db2/sketch.o \
                             $(OBJDIR)/memory_core.o $(OBJDIR)/memory_core_crud.o $(OBJDIR)/memory_core_helpers.o $(OBJDIR)/memory_core_helpers_b.o $(OBJDIR)/memory_core_search.o $(OBJDIR)/memory_core_search_b.o $(OBJDIR)/memory_core_search_c.o $(OBJDIR)/memory_core_scope_embed.o $(OBJDIR)/memory_core_tiers.o $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o $(OBJDIR)/db2/kb_payload.o $(OBJDIR)/db2/kb_service_backend.o $(OBJDIR)/db2/kb_service_backend_ingest.o $(OBJDIR)/db2/memory_lifecycle.o $(OBJDIR)/db2/memory_payload.o $(OBJDIR)/db2/memory_promotion.o $(OBJDIR)/db2/memory_query.o $(OBJDIR)/db2/memory_query_bookkeeping.o $(OBJDIR)/db2/memory_entity_graph.o $(OBJDIR)/db2/memory_score_fields.o $(OBJDIR)/db2/memory_scope_query.o $(OBJDIR)/db2/memory_scenes.o $(OBJDIR)/db2/memory_briefing.o $(OBJDIR)/db2/memory_health.o $(OBJDIR)/db2/memory_row_mapper_pg.o $(OBJDIR)/db2/memory_relations.o $(OBJDIR)/db2/memory_conflicts.o $(OBJDIR)/db2/vector_index_ops.o $(OBJDIR)/db2/code_index_ops.o $(OBJDIR)/db2/rules.o $(OBJDIR)/db2/stopwords.o $(OBJDIR)/db2/tool_registry.o $(OBJDIR)/db2/feedback.o $(OBJDIR)/db2/notes.o $(OBJDIR)/db2/anti_patterns.o $(OBJDIR)/db2/curiosity.o $(OBJDIR)/db2/entity_edges.o $(OBJDIR)/db2/entity_profiles.o $(OBJDIR)/db2/epistemic_directives.o $(OBJDIR)/db2/failed_queries.o $(OBJDIR)/db2/kind_lifecycle.o $(OBJDIR)/db2/pgvec_transport.o $(OBJDIR)/db2/memory_vectors.o $(OBJDIR)/db2/kb_vectors.o $(OBJDIR)/db2/vector_status.o $(OBJDIR)/db2/pgvec_verify.o $(OBJDIR)/db2/pgvec_kb_service.o $(OBJDIR)/tests/support/mock_agent_http.o $(OBJDIR)/tests/support/memory_embed_stub.o $(OBJDIR)/tests/support/kb_client_test_stub.o $(OBJDIR)/tests/support/kb_ws_stub.o $(OBJDIR)/posix/memory.o \
                             $(OBJDIR)/memory_logic.o $(OBJDIR)/memory_health.o $(OBJDIR)/memory_conflict.o $(OBJDIR)/memory_context.o $(OBJDIR)/memory_assemble.o \
                              $(OBJDIR)/memory_advanced.o $(OBJDIR)/memory_prospective.o $(OBJDIR)/memory_lifecycle.o $(OBJDIR)/memory_directives.o $(OBJDIR)/memory_maintenance.o $(OBJDIR)/memory_graph.o $(OBJDIR)/memory_graph_fusion.o $(OBJDIR)/memory_scan.o $(OBJDIR)/memory_improve.o $(OBJDIR)/memory_episodes.o \
                             $(OBJDIR)/tests/support/learning_implicit_stub.o $(OBJDIR)/dogfood.o \
                             $(OBJDIR)/kb/kb_features.o $(OBJDIR)/kb/kb_ranker.o $(OBJDIR)/kb/kb_detect.o $(OBJDIR)/kb/kb_mdl.o \
                             $(OBJDIR)/db2/feature_rows.o $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                             $(OBJDIR)/db2/calibration.o \
                             $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm -lzstd

$(TESTPREFIX)/unit-test-memory-retrieval-eval: $(OBJDIR)/kb/kb_bandit.o $(OBJDIR)/kb/kb_bandit_registry.o $(OBJDIR)/db2/bandit.o \
                             $(OBJDIR)/tests/test_memory_retrieval_eval.o \
                             $(OBJDIR)/server/agent_eval.o $(OBJDIR)/server/agent_eval_memory_support.o $(OBJDIR)/server/agent_eval_baseline.o \
                             $(OBJDIR)/memory_core.o $(OBJDIR)/memory_core_crud.o $(OBJDIR)/memory_core_helpers.o $(OBJDIR)/memory_core_helpers_b.o $(OBJDIR)/memory_core_search.o $(OBJDIR)/memory_core_search_b.o $(OBJDIR)/memory_core_search_c.o $(OBJDIR)/memory_core_scope_embed.o $(OBJDIR)/memory_core_tiers.o $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o $(OBJDIR)/db2/kb_payload.o $(OBJDIR)/db2/kb_service_backend.o $(OBJDIR)/db2/kb_service_backend_ingest.o $(OBJDIR)/db2/memory_lifecycle.o $(OBJDIR)/db2/memory_payload.o $(OBJDIR)/db2/memory_promotion.o $(OBJDIR)/db2/memory_query.o $(OBJDIR)/db2/memory_query_bookkeeping.o $(OBJDIR)/db2/memory_entity_graph.o $(OBJDIR)/db2/memory_score_fields.o $(OBJDIR)/db2/memory_scope_query.o $(OBJDIR)/db2/memory_scenes.o $(OBJDIR)/db2/memory_briefing.o $(OBJDIR)/db2/memory_health.o $(OBJDIR)/db2/memory_row_mapper_pg.o $(OBJDIR)/db2/memory_relations.o $(OBJDIR)/db2/memory_conflicts.o $(OBJDIR)/db2/vector_index_ops.o $(OBJDIR)/db2/code_index_ops.o $(OBJDIR)/db2/rules.o $(OBJDIR)/db2/stopwords.o $(OBJDIR)/db2/tool_registry.o $(OBJDIR)/db2/feedback.o $(OBJDIR)/db2/notes.o $(OBJDIR)/db2/anti_patterns.o $(OBJDIR)/db2/curiosity.o $(OBJDIR)/db2/entity_edges.o $(OBJDIR)/db2/entity_profiles.o $(OBJDIR)/db2/epistemic_directives.o $(OBJDIR)/db2/failed_queries.o $(OBJDIR)/db2/kind_lifecycle.o $(OBJDIR)/db2/pgvec_transport.o $(OBJDIR)/db2/memory_vectors.o $(OBJDIR)/db2/kb_vectors.o $(OBJDIR)/db2/vector_status.o $(OBJDIR)/db2/pgvec_verify.o $(OBJDIR)/db2/pgvec_kb_service.o $(OBJDIR)/tests/support/mock_agent_http.o $(OBJDIR)/tests/support/memory_embed_stub.o $(OBJDIR)/tests/support/kb_client_test_stub.o $(OBJDIR)/posix/memory.o \
                             $(OBJDIR)/memory_logic.o $(OBJDIR)/memory_health.o $(OBJDIR)/memory_conflict.o $(OBJDIR)/memory_context.o $(OBJDIR)/memory_assemble.o \
                              $(OBJDIR)/memory_advanced.o $(OBJDIR)/memory_prospective.o $(OBJDIR)/memory_lifecycle.o $(OBJDIR)/memory_directives.o $(OBJDIR)/memory_maintenance.o $(OBJDIR)/memory_graph.o $(OBJDIR)/memory_graph_fusion.o $(OBJDIR)/memory_scan.o $(OBJDIR)/memory_improve.o $(OBJDIR)/memory_episodes.o \
                             $(OBJDIR)/kb/kb.o $(OBJDIR)/kb/kb_neardup.o $(OBJDIR)/kb/kb_conventions.o $(OBJDIR)/kb/kb_mdl.o $(OBJDIR)/sketch.o $(OBJDIR)/db2/sketch.o \
                             $(OBJDIR)/tests/support/learning_implicit_stub.o $(OBJDIR)/dogfood.o $(DB1_OBJS) \
                             $(OBJDIR)/kb/kb_features.o $(OBJDIR)/kb/kb_ranker.o $(OBJDIR)/kb/kb_detect.o \
                             $(OBJDIR)/db2/feature_rows.o $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                             $(OBJDIR)/db2/calibration.o \
                             $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm -lzstd


$(TESTPREFIX)/unit-test-ensemble: $(OBJDIR)/tests/test_ensemble.o \
                     $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o $(OBJDIR)/db1/ensemble.o \
                     $(OBJDIR)/config.o $(OBJDIR)/config_sections.o $(OBJDIR)/config_database.o $(OBJDIR)/config_learning.o $(OBJDIR)/config_memory.o $(OBJDIR)/config_charter.o $(OBJDIR)/config_trigger.o $(OBJDIR)/config_kb_maintenance.o $(OBJDIR)/config_kb_curator.o $(OBJDIR)/config_server_api.o $(OBJDIR)/config_skills.o $(OBJDIR)/config_save.o \
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
                     $(OBJDIR)/server/delegate_backend.o \
                     $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-delegate-backend-local: $(OBJDIR)/tests/test_delegate_backend_local.o \
                      $(OBJDIR)/tests/support/delegate_child_env_export_stub.o \
                     $(OBJDIR)/server/delegate_backend.o \
                     $(OBJDIR)/server/delegate_backend_local.o \
                     $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-delegate-backend-ssh: $(OBJDIR)/tests/test_delegate_backend_ssh.o \
                     $(OBJDIR)/server/delegate_backend.o \
                     $(OBJDIR)/server/delegate_backend_ssh.o \
                     $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-delegate-backend-docker: $(OBJDIR)/tests/test_delegate_backend_docker.o \
                     $(OBJDIR)/server/delegate_backend.o \
                     $(OBJDIR)/server/delegate_backend_docker.o \
                     $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-otel: $(OBJDIR)/tests/test_otel.o \
                     $(OBJDIR)/server/otel.o \
                     $(OBJDIR)/server/agent_bridge.o $(OBJDIR)/server/anthropic_shape.o $(OBJDIR)/server/tool_call_args.o \
                     $(OBJDIR)/server/agent_request_shaping.o \
                     $(OBJDIR)/server/http_retry.o \
                     $(OBJDIR)/gateway_delegate.o $(OBJDIR)/gateway_pipeline.o $(OBJDIR)/gateway_policy.o \
                     $(TEST_CORE_OBJS) \
                     $(PLATFORM_AGENT_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-oauth-pkce: $(OBJDIR)/tests/test_oauth_pkce.o \
                     $(OBJDIR)/server/oauth_pkce.o \
                     $(OBJDIR)/server/oauth_tokens.o \
                     $(OBJDIR)/db1/secrets.o \
                     $(OBJDIR)/server/vault_service.o \
                     $(OBJDIR)/server/vault_store.o \
                     $(OBJDIR)/server/vault_crypto.o \
                     $(OBJDIR)/server/vault_kek_cache.o \
                     $(OBJDIR)/server/vault_server_key.o \
                     $(OBJDIR)/platform_random.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-oauth-reauth: $(OBJDIR)/tests/test_oauth_reauth.o \
                     $(OBJDIR)/server/oauth_pkce.o \
                     $(OBJDIR)/server/oauth_tokens.o \
                     $(OBJDIR)/db1/secrets.o \
                     $(OBJDIR)/server/vault_service.o \
                     $(OBJDIR)/server/vault_store.o \
                     $(OBJDIR)/server/vault_crypto.o \
                     $(OBJDIR)/server/vault_kek_cache.o \
                     $(OBJDIR)/server/vault_server_key.o \
                     $(OBJDIR)/platform_random.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-enroll: $(OBJDIR)/tests/test_kb_enroll.o \
                     $(OBJDIR)/kb/enroll.o \
                     $(OBJDIR)/kb/pki.o \
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

$(TESTPREFIX)/unit-test-kb-pki: $(OBJDIR)/tests/test_kb_pki.o \
                     $(OBJDIR)/kb/pki.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-context-engine: $(OBJDIR)/tests/test_context_engine.o \
                     $(OBJDIR)/server/context_engine.o \
                     $(OBJDIR)/server/session_compact.o $(OBJDIR)/server/compact_prune.o \
                     $(OBJDIR)/server/agent_bridge.o $(OBJDIR)/server/anthropic_shape.o $(OBJDIR)/server/tool_call_args.o \
                     $(OBJDIR)/server/agent_request_shaping.o \
                     $(OBJDIR)/server/delegate_driver.o \
                     $(OBJDIR)/server/delegate_openai.o \
                     $(OBJDIR)/server/delegate_gemini.o \
                     $(OBJDIR)/server/delegate_xml_fallback.o \
                     $(OBJDIR)/model_registry.o \
                     $(OBJDIR)/server/agent_tools.o \
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
                     $(OBJDIR)/memory_provider.o \
                     $(OBJDIR)/log.o \
                     $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-plugin-c-hook: $(OBJDIR)/tests/test_plugin_c_hook.o \
                     $(OBJDIR)/plugin_c_hook.o \
                     $(OBJDIR)/log.o \
                     $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-plugin-loader: $(OBJDIR)/tests/test_plugin_loader.o \
                     $(OBJDIR)/plugin_loader.o $(OBJDIR)/plugin.o $(OBJDIR)/plugin_ctx.o \
                     $(OBJDIR)/memory_provider.o $(OBJDIR)/server/context_engine.o \
                     $(OBJDIR)/server/session_compact.o $(OBJDIR)/server/compact_prune.o \
                     $(OBJDIR)/server/agent_bridge.o $(OBJDIR)/server/anthropic_shape.o $(OBJDIR)/server/tool_call_args.o \
                     $(OBJDIR)/server/agent_request_shaping.o \
                     $(OBJDIR)/server/delegate_driver.o \
                     $(OBJDIR)/server/delegate_openai.o \
                     $(OBJDIR)/server/delegate_gemini.o \
                     $(OBJDIR)/server/delegate_xml_fallback.o \
                     $(OBJDIR)/model_registry.o \
                     $(OBJDIR)/server/agent_tools.o \
                     $(OBJDIR)/config.o $(OBJDIR)/config_sections.o $(OBJDIR)/config_database.o $(OBJDIR)/config_learning.o $(OBJDIR)/config_memory.o $(OBJDIR)/config_charter.o $(OBJDIR)/config_trigger.o $(OBJDIR)/config_kb_maintenance.o $(OBJDIR)/config_kb_curator.o $(OBJDIR)/config_server_api.o $(OBJDIR)/config_skills.o $(OBJDIR)/config_save.o \
                     $(OBJDIR)/aimee_home.o \
                     $(OBJDIR)/yaml.o \
                     $(OBJDIR)/db1/db.o \
                     $(OBJDIR)/db1/db_schema.o $(OBJDIR)/tests/aimee_pg_sqlite_shim.o $(OBJDIR)/db2/db2_test_shim.o \
                     $(OBJDIR)/util.o $(OBJDIR)/text.o $(OBJDIR)/dstr.o \
                     $(OBJDIR)/platform_random.o \
                     $(OBJDIR)/log.o $(OBJDIR)/cJSON.o \
                     $(TEST_DATA_OBJS) $(TEST_WORKSPACE_OBJS_EXTRA)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-plugin: $(OBJDIR)/tests/test_plugin.o \
                     $(OBJDIR)/plugin.o \
                     $(OBJDIR)/config.o $(OBJDIR)/config_sections.o $(OBJDIR)/config_database.o $(OBJDIR)/config_learning.o $(OBJDIR)/config_memory.o $(OBJDIR)/config_charter.o $(OBJDIR)/config_trigger.o $(OBJDIR)/config_kb_maintenance.o $(OBJDIR)/config_kb_curator.o $(OBJDIR)/config_server_api.o $(OBJDIR)/config_skills.o $(OBJDIR)/config_save.o \
                     $(OBJDIR)/aimee_home.o \
                     $(OBJDIR)/yaml.o \
                     $(OBJDIR)/db1/db.o \
                     $(OBJDIR)/db1/db_schema.o $(OBJDIR)/tests/aimee_pg_sqlite_shim.o $(OBJDIR)/db2/db2_test_shim.o \
                     $(OBJDIR)/util.o $(OBJDIR)/text.o $(OBJDIR)/dstr.o \
                     $(OBJDIR)/platform_random.o \
                     $(OBJDIR)/log.o $(OBJDIR)/cJSON.o \
                     $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)


$(TESTPREFIX)/unit-test-dogfood: $(OBJDIR)/tests/test_dogfood.o \
                     $(OBJDIR)/tests/support/learning_implicit_stub.o $(OBJDIR)/dogfood.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-working-profile: $(OBJDIR)/tests/test_working_profile.o \
                     $(OBJDIR)/working_profile.o \
                     $(OBJDIR)/db2/calibration.o \
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                     $(OBJDIR)/db2/feature_rows.o \
                     $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(DB1_OBJS) \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-curiosity: $(OBJDIR)/tests/test_curiosity.o $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-cmd-identity: $(OBJDIR)/tests/test_cmd_identity.o \
                     $(OBJDIR)/cmd_identity.o $(OBJDIR)/working_profile.o \
                     $(DB1_OBJS) \
                     $(OBJDIR)/cmd_util.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-session-briefing: $(OBJDIR)/tests/test_session_briefing.o \
                     $(OBJDIR)/session_briefing.o $(OBJDIR)/skill.o $(TEST_DATA_OBJS_MOCK)
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
                     $(OBJDIR)/learning_router.o $(OBJDIR)/learning_implicit.o \
                     $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-memory-recall-pivot: $(OBJDIR)/tests/test_memory_recall_pivot.o \
                     $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-memory-filter: $(OBJDIR)/tests/test_memory_filter.o $(TEST_DATA_OBJS_MOCK)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm

$(TESTPREFIX)/unit-test-memory-profiles: $(OBJDIR)/tests/test_memory_profiles.o \
                     $(OBJDIR)/memory_profile_pack.o \
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
                     $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o $(OBJDIR)/db1/conv_context.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Manual-inspection harness for the virtual-context rollout (AC#2/AC#3). Drives
# the real record->flush->assemble->search->expand path with the flag on and
# prints reviewer evidence. Source lives under tests/eval/ (outside src/tests).
$(OBJDIR)/tests/inspect_real_session.o: ../tests/eval/agentic_context_virtualization/inspect_real_session.c
	@mkdir -p $(dir $@) && $(CC) $(TEST_C_FLAGS) -Idb1 -c -o $@ $<

$(TESTPREFIX)/virtual-context-inspect: $(OBJDIR)/tests/inspect_real_session.o \
                     $(OBJDIR)/conversation_context.o \
                     $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o $(OBJDIR)/db1/conv_context.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-payload-rewrite-state: $(OBJDIR)/tests/test_payload_rewrite_state.o \
                     $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o $(OBJDIR)/db1/payload_rewrite_state.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-payload-rewrite: $(OBJDIR)/tests/test_payload_rewrite.o \
                     $(OBJDIR)/payload_rewrite.o \
                     $(OBJDIR)/db1/db1_init.o \
                     $(OBJDIR)/db1/payload_rewrite_state.o $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-scope: $(OBJDIR)/tests/test_kb_scope.o \
                     $(OBJDIR)/kb/kb_scope.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-route-acl: $(OBJDIR)/tests/test_kb_route_acl.o \
                     $(OBJDIR)/kb/http/kb_route_acl.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-ingest-format: $(OBJDIR)/tests/test_kb_ingest_format.o \
                     $(OBJDIR)/kb/kb_ingest_normalize.o $(OBJDIR)/log.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-http-routes: $(OBJDIR)/tests/test_kb_http_routes.o \
                     $(OBJDIR)/tests/support/corpus_jobs_http_stub.o \
                     $(OBJDIR)/tests/support/pdf_route_stubs.o \
                     $(OBJDIR)/kb/http/kb_http.o \
                     $(OBJDIR)/kb/http/kb_http_conn.o \
                     $(OBJDIR)/tests/support/kb_ws_stub.o \
                     $(OBJDIR)/kb/http/kb_http_search.o \
                     $(OBJDIR)/kb/http/kb_route_acl.o \
                     $(OBJDIR)/kb/http/kb_http_console.o \
                     $(OBJDIR)/kb/http/kb_http_accounts.o \
                     $(OBJDIR)/kb/http/kb_http_governance.o \
                     $(OBJDIR)/util.o \
                     $(OBJDIR)/kb/kb_scope.o \
                     $(OBJDIR)/kb/verifier.o \
                     $(OBJDIR)/kb/enroll.o \
                     $(OBJDIR)/kb/pki.o \
                     $(OBJDIR)/kb/http/kb_tls.o \
                     $(OBJDIR)/kb/http/kb_tls_serve.o \
                     $(OBJDIR)/server/kb_client_mtls.o \
                     $(OBJDIR)/server/oauth_pkce.o \
                     $(OBJDIR)/shared/kb_paths.o \
                     $(OBJDIR)/aimee_home.o \
                     $(OBJDIR)/kb/kb_intel_payload.o \
                     $(OBJDIR)/kb/kb_bandit_registry.o \
                     $(OBJDIR)/kb/http/kb_http_code.o \
                     $(OBJDIR)/kb/http/kb_http_code_graphfb.o $(OBJDIR)/kb/lessons_reflect.o \
                                    $(OBJDIR)/kb/lessons_session_capture.o $(OBJDIR)/kb/lessons_cite_tracker.o \
                     $(OBJDIR)/kb/kb_rrf.o \
                     $(OBJDIR)/kb/kb_graph_analytics.o \
                     $(OBJDIR)/kb/prompt_sanitizer.o \
                     $(OBJDIR)/kb/http/kb_http_pdf.o \
                     $(OBJDIR)/kb/http/kb_http_jobs.o \
                     $(OBJDIR)/posix/td_search_render.o $(OBJDIR)/dstr.o \
                     $(OBJDIR)/cJSON.o \
                     $(OBJDIR)/log.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-kb-http-ingest: $(OBJDIR)/tests/test_kb_http_ingest.o \
                     $(OBJDIR)/kb/http/kb_http_ingest.o \
                     $(OBJDIR)/tests/support/kb_ws_stub.o \
                     $(OBJDIR)/kb/kb_doc_hash.o \
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
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                     $(OBJDIR)/db2/feature_rows.o \
                     $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/learning_evidence.o $(OBJDIR)/db2/learning_synth_ops.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o $(OBJDIR)/db2/feedback.o \
                     $(OBJDIR)/db2/anti_patterns.o \
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
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                     $(OBJDIR)/db2/feature_rows.o \
                     $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-learning-bundle: $(OBJDIR)/tests/test_learning_bundle.o \
                     $(OBJDIR)/learning_bundle.o \
                     $(OBJDIR)/db2/evidence_vectors.o \
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                     $(OBJDIR)/db2/feature_rows.o \
                     $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm -lzstd

$(TESTPREFIX)/unit-test-learning-synth: $(OBJDIR)/tests/test_learning_synth.o \
                     $(OBJDIR)/kb/kb_learning_synth.o \
                     $(OBJDIR)/learning_bundle.o \
                     $(OBJDIR)/db2/evidence_vectors.o \
                     $(OBJDIR)/db2/learning_synth_ops.o \
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                     $(OBJDIR)/db2/feature_rows.o \
                     $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(OBJDIR)/posix/platform_process.o \
                     $(OBJDIR)/linux/platform_process.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm -lzstd

$(TESTPREFIX)/unit-test-learning-version: $(OBJDIR)/tests/test_learning_version.o \
                     $(OBJDIR)/kb/kb_learning_version.o \
                     $(OBJDIR)/db2/evidence_vectors.o \
                     $(OBJDIR)/db2/learning_synth_ops.o \
                     $(OBJDIR)/db2/kb_runtime_state.o \
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                     $(OBJDIR)/db2/feature_rows.o \
                     $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm -lzstd

$(TESTPREFIX)/unit-test-corpus-structural: $(OBJDIR)/tests/test_corpus_structural.o \
                     $(OBJDIR)/db2/corpus_structural.o \
                     $(OBJDIR)/db2/corpus_jobs.o \
                     $(OBJDIR)/db2/kb_docs.o \
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                     $(OBJDIR)/db2/feature_rows.o $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-corpus-jobs: $(OBJDIR)/tests/test_corpus_jobs.o \
                     $(OBJDIR)/db2/corpus_jobs.o \
                     $(OBJDIR)/db2/curator_terms.o \
                     $(OBJDIR)/db2/curator_gaps.o \
                     $(OBJDIR)/db2/corpus_structural.o \
                     $(OBJDIR)/db2/curiosity.o \
                     $(OBJDIR)/db2/kb_docs.o \
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                     $(OBJDIR)/db2/feature_rows.o $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-corpus-terms-gaps: $(OBJDIR)/tests/test_corpus_terms_gaps.o \
                     $(OBJDIR)/db2/curator_terms.o \
                     $(OBJDIR)/db2/curator_gaps.o \
                     $(OBJDIR)/db2/corpus_structural.o \
                     $(OBJDIR)/db2/corpus_jobs.o \
                     $(OBJDIR)/db2/curiosity.o \
                     $(OBJDIR)/db2/kb_docs.o \
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                     $(OBJDIR)/db2/feature_rows.o $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-calibration: $(OBJDIR)/tests/test_calibration.o \
                     $(OBJDIR)/kb/kb_calibrate.o \
                     $(OBJDIR)/db2/calibration.o \
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                     $(OBJDIR)/db2/feature_rows.o \
                     $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-demotion: $(OBJDIR)/tests/test_demotion.o \
                     $(OBJDIR)/db2/demotion.o \
                     $(OBJDIR)/db2/memory_payload.o \
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                     $(OBJDIR)/db2/feature_rows.o \
                     $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm -lzstd

$(TESTPREFIX)/unit-test-features: $(OBJDIR)/tests/test_features.o \
                     $(OBJDIR)/kb/kb_features.o \
                     $(OBJDIR)/kb/kb_ranker.o \
                     $(OBJDIR)/kb/kb_detect.o \
                     $(OBJDIR)/db2/feature_rows.o \
                     $(OBJDIR)/db2/sketch.o \
                     $(OBJDIR)/sketch.o \
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                     $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/db2/calibration.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
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
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                     $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lm -lzstd

# Bridge logic in isolation: the test stubs config_load + the KB client, so no
# DB/network objects are needed — just the bridge TU.
$(TESTPREFIX)/unit-test-retrieval-outcome-bridge: $(OBJDIR)/tests/test_retrieval_outcome_bridge.o \
                     $(OBJDIR)/server/retrieval_outcome_bridge.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

# Pure render/extract helpers behind the kb_search tool — cJSON + dstr only.
$(TESTPREFIX)/unit-test-td-search-render: $(OBJDIR)/tests/test_td_search_render.o \
                     $(OBJDIR)/posix/td_search_render.o \
                     $(OBJDIR)/cJSON.o $(OBJDIR)/dstr.o
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-report-enrichments: $(OBJDIR)/tests/test_report_enrichments.o \
                     $(OBJDIR)/db2/report_enrichments.o \
                     $(OBJDIR)/report_enrichment.o $(OBJDIR)/util_url.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-reasoning: $(OBJDIR)/tests/test_reasoning.o \
                     $(OBJDIR)/kb/kb_reasoning.o \
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                     $(OBJDIR)/db2/feature_rows.o \
                     $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-bandit: $(OBJDIR)/tests/test_bandit.o \
                     $(OBJDIR)/kb/kb_bandit.o \
                     $(OBJDIR)/kb/kb_bandit_registry.o \
                     $(OBJDIR)/db2/bandit.o \
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                     $(OBJDIR)/db2/feature_rows.o \
                     $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-planner: $(OBJDIR)/tests/test_planner.o \
                     $(OBJDIR)/kb/kb_planner.o \
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                     $(OBJDIR)/db2/feature_rows.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-roadmap: $(OBJDIR)/tests/test_roadmap.o \
                     $(OBJDIR)/kb/roadmap.o \
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                     $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/db2/feature_rows.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-kb-releases-db: $(OBJDIR)/tests/test_kb_releases_db.o \
                     $(OBJDIR)/db2/kb_releases.o \
                     $(OBJDIR)/db2/kb_runtime_state.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-roadmap-decompose: $(OBJDIR)/tests/test_roadmap_decompose.o \
                     $(OBJDIR)/roadmap_decompose.o \
                     $(OBJDIR)/kb/roadmap.o \
                     $(OBJDIR)/db2/artifacts.o $(OBJDIR)/db2/kb_audit_worm.o $(OBJDIR)/audit_worm_chain.o $(OBJDIR)/workflow/wfe_canonical.o \
                     $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/db2/feature_rows.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-kb-mdl: $(OBJDIR)/tests/test_kb_mdl.o \
                     $(OBJDIR)/kb/kb_mdl.o \
                     $(OBJDIR)/db2/feature_rows.o \
                     $(OBJDIR)/db2/db2_init.o $(OBJDIR)/db2/db2_pool.o $(OBJDIR)/db2/db_schema.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS) -lzstd

$(TESTPREFIX)/unit-test-guardrails-semantic: $(OBJDIR)/tests/test_guardrails_semantic.o \
                     $(OBJDIR)/guardrails_semantic.o \
                     $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o $(OBJDIR)/db1/guardrail_events.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-guardrails-computer-use: $(OBJDIR)/tests/test_guardrails_computer_use.o \
                     $(OBJDIR)/server/computer_use.o $(OBJDIR)/cJSON.o
	$(TESTLINK_MIN) -o $@ $^ $(L_MINIMAL)

$(TESTPREFIX)/unit-test-osv-check: $(OBJDIR)/tests/test_osv_check.o \
                     $(OBJDIR)/server/osv_check.o \
                     $(TEST_CORE_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-mcp-osv-cache: $(OBJDIR)/tests/test_mcp_osv_cache.o \
                     $(OBJDIR)/db1/db1_init.o $(OBJDIR)/db1/db_schema.o \
                     $(OBJDIR)/db1/mcp_osv_cache.o $(OBJDIR)/db1/interaction_events.o \
                     $(OBJDIR)/db1/maintenance.o $(OBJDIR)/log.o $(OBJDIR)/util.o \
                     $(OBJDIR)/cJSON.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(TEST_L_FLAGS)

$(TESTPREFIX)/unit-test-mcp-client-registry: $(OBJDIR)/tests/test_mcp_client_registry.o \
                     $(OBJDIR)/mcp_tools.o $(OBJDIR)/mcp_tool_profile.o $(OBJDIR)/mcp_tools_extended.o $(OBJDIR)/mcp_skill_tools.o $(OBJDIR)/mcp_tools_gateway.o \
                     $(OBJDIR)/server/session_search_tool.o \
                     $(OBJDIR)/plugin.o \
                     $(OBJDIR)/config.o $(OBJDIR)/config_sections.o $(OBJDIR)/config_database.o $(OBJDIR)/config_learning.o $(OBJDIR)/config_memory.o $(OBJDIR)/config_charter.o $(OBJDIR)/config_trigger.o $(OBJDIR)/config_kb_maintenance.o $(OBJDIR)/config_kb_curator.o $(OBJDIR)/config_server_api.o $(OBJDIR)/config_skills.o $(OBJDIR)/platform_random.o $(OBJDIR)/config_save.o \
                     $(OBJDIR)/yaml.o \
                     $(OBJDIR)/db1/db.o \
                     $(OBJDIR)/db1/db_schema.o $(OBJDIR)/db1/maintenance.o $(OBJDIR)/tests/aimee_pg_sqlite_shim.o $(OBJDIR)/db2/db2_test_shim.o \
                     $(OBJDIR)/log.o \
                     $(OBJDIR)/aimee_home.o \
                     $(OBJDIR)/server/osv_check.o \
                     $(OBJDIR)/server/mcp_client_registry.o \
                     $(TEST_MCP_CLIENT_OBJS) \
                     $(TESTPREFIX)/mock-mcp-server
	$(TESTLINK) -o $@ $(OBJDIR)/tests/test_mcp_client_registry.o $(OBJDIR)/mcp_tools.o $(OBJDIR)/mcp_tool_profile.o $(OBJDIR)/mcp_tools_extended.o $(OBJDIR)/mcp_skill_tools.o $(OBJDIR)/mcp_tools_gateway.o $(OBJDIR)/server/session_search_tool.o $(OBJDIR)/plugin.o $(OBJDIR)/config.o $(OBJDIR)/config_sections.o $(OBJDIR)/config_database.o $(OBJDIR)/config_learning.o $(OBJDIR)/config_memory.o $(OBJDIR)/config_charter.o $(OBJDIR)/config_trigger.o $(OBJDIR)/config_kb_maintenance.o $(OBJDIR)/config_kb_curator.o $(OBJDIR)/config_server_api.o $(OBJDIR)/config_skills.o $(OBJDIR)/platform_random.o $(OBJDIR)/config_save.o $(OBJDIR)/aimee_home.o $(OBJDIR)/yaml.o $(OBJDIR)/db1/db.o $(OBJDIR)/db1/db_schema.o $(OBJDIR)/db1/maintenance.o $(OBJDIR)/tests/aimee_pg_sqlite_shim.o $(OBJDIR)/db2/db2_test_shim.o $(OBJDIR)/log.o $(OBJDIR)/server/osv_check.o $(OBJDIR)/server/mcp_client_registry.o $(TEST_MCP_CLIENT_OBJS) $(TEST_L_FLAGS)
