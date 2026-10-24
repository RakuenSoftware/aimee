import datetime as dt
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("ir_bypass", Path(__file__).resolve().parents[1] / "check_ir_bypass.py")
checker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checker)


class IRBypassTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.ledger = self.root / checker.LEDGER
        self.ledger.parent.mkdir(parents=True)
        self.write_ledger([])

    def write_ledger(self, exceptions):
        self.ledger.write_text(json.dumps({"version": 1, "exceptions": exceptions}))

    def source(self, relative, text):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        return path

    def test_parallel_openai_success_shapers_cannot_return(self):
        for name in ('openai_request_double', 'openai_request_int', 'openai_format_chat_completion', 'openai_format_chat_completion_reason', 'openai_format_response', 'openai_format_response_status', 'openai_format_run', 'openai_format_responses_delta', 'openai_format_responses_completed', 'openai_format_responses_incomplete', 'openai_responses_message_item', 'openai_responses_function_call_item', 'openai_format_responses_msg_item_added', 'openai_format_responses_msg_item_done', 'openai_format_responses_fc_item_added', 'openai_format_responses_fc_item_done', 'openai_format_responses_fc_args_delta', 'openai_format_responses_fc_args_done', 'openai_format_responses_completed_items'):
            for relative in ("src/server/reintroduced_shape.c", "src/headers/reintroduced_shape.h"):
                path = self.source(relative, f"int {name}(void);\n")
                with self.subTest(name=name, relative=relative), self.assertRaisesRegex(ValueError, "retired-api"):
                    checker.check(self.root)
                path.unlink()

    def test_dormant_parallel_c_execution_cannot_return(self):
        for name in ('agent_coordinate', 'agent_execute_with_plan', 'agent_job_resume', 'agent_plan_execute', 'agent_plan_rollback_step', 'agent_plan_verify', 'plan_assign_waves', 'agent_loop_init', 'agent_loop_free', 'agent_loop_run', 'agent_loop_parse_completion', 'agent_loop_t'):
            for relative in ("src/server/reintroduced_runner.c", "src/headers/reintroduced_runner.h"):
                path = self.source(relative, f"int {name}(void);\n")
                with self.subTest(name=name, relative=relative), self.assertRaisesRegex(ValueError, "retired-api"):
                    checker.check(self.root)
                path.unlink()
        self.source("src/headers/active_plan.h", "plan_t retained; plan_step_t step; void agent_set_durable_job(int); int agent_get_durable_job_id(void); int agent_loop_total_timeout_ms(int,int);")
        checker.check(self.root)

    def test_retired_first_party_chat_projection(self):
        for name in ["builtin_chat_send_ex", "builtin_chat_stream_t", "builtin_chat_stream_control_t",
                     "chat_stream_on_open", "chat_stream_finish", "g_acp_tool_seq"]:
            target = self.source("src/retired_chat.c", f"void {name}(void);\n")
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "retired-api"):
                checker.check(self.root)
            target.unlink()

    def test_unowned_sdk_policy_stays_retired(self):
        for name in ("cli_claude_allowed_tools", "cli_claude_allowed_tools_count", "claude_build_argv",
                     "claude_spawn", "claude_execute", "CLAUDE_ALLOWED_TOOLS", "CLAUDE_ARG_MAX",
                     "cli_codex_parse_server_request_method", "cli_codex_approval_decision",
                     "cli_codex_command_restarts_aimee_server", "cli_codex_json_has_server_restart",
                     "cli_codex_method_is_shell_approval", "cli_codex_send_approval_decision",
                     "cli_codex_send_permissions_decision", "cli_codex_send_user_input_decision",
                     "cli_codex_send_elicitation_decision", "CLI_CODEX_APPROVAL_POLICY_CONFIG",
                     "CLI_CODEX_SANDBOX_READ_ONLY_CONFIG", "CLI_CODEX_SANDBOX_WORKSPACE_WRITE_CONFIG",
                     "CLI_CODEX_SANDBOX_DANGER_CONFIG"):
            path = self.source("src/server/renamed_codex.c", name + "(frame);\n")
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "retired-api"):
                checker.check(self.root)
            path.unlink()

    def test_renamed_codex_ambient_launch_stays_retired(self):
        for call in ('execlp("codex", "codex", "app-server", 0)',
                     'execvp(command, arguments)', 'setenv("HOME", borrowed, 1)',
                     'unsetenv("OPENAI_API_KEY")'):
            path = self.source("src/server/cli_codex.c", "void renamed(void) { " + call + "; }")
            with self.subTest(call=call), self.assertRaisesRegex(ValueError, "codex-ambient-launch"):
                checker.check(self.root)
            path.unlink()
        self.source("src/server/cli_codex.c", "void owned(void) { fexecve(fd, argv, environment); }")
        checker.check(self.root)

    def test_unowned_acp_state_and_spawn_stay_retired(self):
        for symbol in ("provider_cli_spawn_argv", "acp_turn_consume", "acp_turn_state_t", "acp_tools"):
            path = self.source("src/server/renamed_acp.c", symbol + "(frame);\n")
            with self.subTest(symbol=symbol), self.assertRaisesRegex(ValueError, "retired-api"):
                checker.check(self.root)
            path.unlink()

    def test_renamed_acp_worker_cannot_restore_ambient_launch(self):
        for invocation in ('execvp(command, arguments)', 'execlp("agent", "agent", 0)',
                           'setenv("HOME", borrowed, 1)', 'unsetenv("AIMEE_VAULT_TOKEN")'):
            path = self.source("src/server/cli_acp.c", "void renamed(void) { " + invocation + "; }")
            with self.subTest(invocation=invocation), self.assertRaisesRegex(ValueError, "acp-ambient-launch"):
                checker.check(self.root)
            path.unlink()
        self.source("src/server/cli_acp.c", "void owned(void) { fexecve(fd, argv, environment); }")
        checker.check(self.root)

    def test_native_control_adapter_cannot_reparse_model_history(self):
        for call in ('anthropic_backend_parse(frame, response, error, size)',
                     'anthropic_frontend_parse(frame, request, error, size)',
                     'anthropic_stream_to_deltas(kind, frame, state, delta, 1)'):
            path = self.source("src/modules/translation/harness_ir.c", "int renamed(void) { return " + call + "; }")
            with self.subTest(call=call), self.assertRaisesRegex(ValueError, "native-control-reparse"):
                checker.check(self.root)
            path.unlink()
        for function in ("aimee_claude_frame_ir_decode", "aimee_codex_frame_ir_decode", "aimee_codex_reply_ir"):
            for call in ("message(id, role, blocks, count)", "delta(id, d)", "harness_output_decode(frame, state, decode)"):
                path = self.source("src/modules/translation/harness_ir.c", "int " + function + "(void) { return " + call + "; }")
                with self.subTest(function=function, call=call), self.assertRaisesRegex(ValueError, "native-control-history"):
                    checker.check(self.root)
                path.unlink()
        self.source("src/modules/translation/harness_ir.c", "int acp_chunk(void) { return message(id, role, blocks, count); }")
        checker.check(self.root)  # ACP remains the actual external model boundary.

    def test_native_alternate_history_mode_stays_retired(self):
        self.source("src/modules/translation/renamed.c", "state.host_model_capture = 0;")
        with self.assertRaisesRegex(ValueError, "retired-api"):
            checker.check(self.root)

    def test_production_tree(self):
        checker.check(checker.ROOT)

    def test_unmanaged_native_probe_cannot_return(self):
        for symbol in ("RunProbeWorker", "newProbeWorker", "runConfiguredProbe", "splitProbeCommand", "validProbeOutput"):
            with self.subTest(symbol=symbol):
                path = self.source("server-go/modules/providers/renamed_probe.go", "package providers\nfunc bad() { " + symbol + "() }")
                with self.assertRaisesRegex(ValueError, "retired-api"):
                    checker.check(self.root)
                path.unlink()

    def test_renamed_unmanaged_probe_cannot_execute(self):
        for invocation in ('exec.Command("model")', 'exec.CommandContext(ctx, "model")'):
            path = self.source("server-go/modules/providers/renamed_probe.go", "package providers\nfunc diagnostic() { " + invocation + " }")
            with self.assertRaisesRegex(ValueError, "provider-probe-exec"):
                checker.check(self.root)
            path.unlink()

    def test_internal_tool_catalog_stays_canonical(self):
        for symbol in ("build_tools_array", "build_tools_array_responses", "build_tools_array_anthropic",
                       "agent_tools_filter_for_role", "agent_tools_filter_anthropic_for_role",
                       "agent_tools_append_delegate_respond_tool", "delegate_respond_spec",
                       "agent_tools_sanitize_for_provider", "agent_tools_sanitize_for_agent"):
            with self.subTest(symbol=symbol):
                path=self.source("src/server/new_catalog.c",symbol+"(catalog);")
                with self.assertRaisesRegex(ValueError,"retired-api"): checker.check(self.root)
                path.unlink()
        for symbol in ("openai_ir_parse_tool", "anthropic_ir_parse_tool"):
            path=self.source("src/server/agent_bridge.c","int agent_prepare_request_ir(void) { return "+symbol+"(tool); }")
            with self.assertRaisesRegex(ValueError,"native-tool-reparse"): checker.check(self.root)
            path.unlink()
        path=self.source("src/modules/delegates/driver.c","driver.build_tools = renamed_builder;")
        with self.assertRaisesRegex(ValueError,"dead-tool-builder-abi"): checker.check(self.root)
        path.unlink()
        self.source("src/modules/translation/aimee_frontend_openai.c","openai_ir_parse_tool(native,tool);")
        checker.check(self.root) # explicit external ingress remains supported

    def test_secondary_flat_cli_transport_cannot_be_reintroduced(self):
        for text in (
            "cli_event_t event;", "event.type = CLI_EVENT_TEXT_DELTA;",
            "provider_cli_parse_json_line_common(line, &event);",
            "provider_cli_format_json_tool_result(name, result, buffer, size);",
            "cli_codex_chat_request_t request;", "cli_codex_chat_stream(&request, cb, ctx, out);",
            "server_native_codex_chat_stream(agent, &request, cb, ctx, out);",
            "cli_codex_parse_agent_message(line, &text);",
        ):
            with self.subTest(text=text):
                path = self.source("src/server/retired_adapter.c", text)
                with self.assertRaisesRegex(ValueError, "retired-api"):
                    checker.check(self.root)
                path.unlink()

    def test_retired_request_builders_cannot_return_under_another_consumer(self):
        symbols = ("agent_build_request_openai", "agent_build_request_responses",
                   "agent_build_request_anthropic", "agent_anthropic_set_system",
                   "model_sampling_apply_openai", "model_sampling_apply_anthropic",
                   "agent_request_shape_openai_messages")
        for symbol in symbols:
            for relative in ("src/server/new.c", "src/headers/new.h",
                             "server-go/modules/rogue/rogue.go"):
                with self.subTest(symbol=symbol, relative=relative):
                    path = self.source(relative, symbol + "(request);")
                    with self.assertRaisesRegex(ValueError, "retired-api"):
                        checker.check(self.root)
                    path.unlink()

    def test_migration_fallback_and_flat_persona_shims_stay_retired(self):
        for symbol in (
            "aimee_ir_path_enabled", "aimee_ir_stream_relay_enabled",
            "aimee_ir_build_provider_body", "aimee_ir_shadow_compare_bodies",
            "aimee_ir_shadow_enabled", "aimee_ir_prepend_active_persona_text",
            "aimee_ir_prepend_persona_text",
        ):
            with self.subTest(symbol=symbol):
                path = self.source("src/server/retired_migration.c", "void caller(void) { " + symbol + "(); }")
                with self.assertRaisesRegex(ValueError, "retired-api"):
                    checker.check(self.root)
                path.unlink()

    def test_untyped_provider_entries_stay_retired(self):
        for symbol in ("provider_client_complete", "provider_client_build_openai", "provider_import_request",
                       "agent_responses_sse_response_object", "agent_tools_builtin_catalog",
                       "provider_cli_adapter_execute", "agent_execute_cli_codex",
                       "agent_execute_cli_codex_at_cwd"):
            with self.subTest(symbol=symbol):
                path = self.source("src/server/new_model.c", symbol + "(profile, messages, schema, output, err, cap);")
                with self.assertRaisesRegex(ValueError, "retired-api"):
                    checker.check(self.root)
                path.unlink()

    def test_generic_model_stdout_adapters_stay_retired(self):
        for call in ("externalTextObservation(stdout)", "delegate.CLIText(request.Input)", "RunWatchdog(args)"):
            with self.subTest(call=call):
                path = self.source("server-go/modules/delegates/rogue.go", "package delegates\nfunc rogue() { " + call + " }")
                with self.assertRaisesRegex(ValueError, "retired-api"):
                    checker.check(self.root)
                path.unlink()

    def test_renamed_delegate_model_bypass_is_rejected(self):
        for call in ('exec.Command("model")', 'exec.CommandContext(ctx, "model")', 'canonical.TextResponse(stdout)', 'renamed.TextResponse(stdout)'):
            with self.subTest(call=call):
                path = self.source("server-go/modules/delegates/new_adapter.go", "package delegates\nfunc newAdapter() { " + call + " }")
                with self.assertRaisesRegex(ValueError, "delegate-model-bypass"):
                    checker.check(self.root)
                path.unlink()
        self.source("server-go/modules/tools/command.go", 'package tools\nfunc tool() { exec.Command("domain-tool") }')
        checker.check(self.root)

    def test_model_consumers_cannot_trust_shell_output(self):
        # Deterministic solver/index adapters remain explicit non-model protocols.
        self.source("src/kb/kb_planner.c", "platform_exec_pipe(cmd, request, n, &out, &len);")
        checker.check(self.root)
        for relative in checker.CANONICAL_MODEL_FILES:
            for call in ("platform_exec_pipe(cmd, request, n, &out, &len)",
                         "kb_curator_sidecar_run(cmd, request, cap, err, len)",
                         'popen(cmd, "r")'):
                with self.subTest(relative=relative, call=call):
                    path = self.source(relative, call + ";")
                    with self.assertRaisesRegex(ValueError, "model-command-bypass"):
                        checker.check(self.root)
                    path.unlink()

    def test_session_search_cannot_restore_unscoped_legacy_transcript_reads(self):
        self.source("src/db1_client/sessions.c", "db1_primary_session_get_latest(id, row);")
        checker.check(self.root)  # The retained one-time import owns this source read.
        for relative, text in [
            ("src/server/session_search_tool.c", "db1_primary_session_alloc_search(query, rows, 4);"),
            ("src/server/session_search_tool.c", "db1_primary_session_get_latest(id, row);"),
            ("server-go/modules/aimee/families/session_ir_search.go", 'const sql = `SELECT messages_json FROM primary_sessions`'),
        ]:
            with self.subTest(relative=relative, text=text):
                path = self.source(relative, text)
                with self.assertRaisesRegex(ValueError, "legacy-session-search"):
                    checker.check(self.root)
                path.unlink()

    def test_raw_router_and_ingress_history_readers_cannot_return(self):
        self.source("src/server/router_advise.c", "void router_advise_ir(const aimee_request_t *request); ")
        checker.check(self.root)
        for symbol in ("gw_stage_router", "ingress_preinject_query_from_messages",
                       "ingress_preinject_last_assistant_from_messages"):
            for relative in ("src/server/new.c", "src/headers/new.h",
                             "server-go/modules/rogue/rogue.go"):
                with self.subTest(symbol=symbol, relative=relative):
                    path = self.source(relative, symbol + "(request);")
                    with self.assertRaisesRegex(ValueError, "retired-api"):
                        checker.check(self.root)
                    path.unlink()
        self.source("src/server/router_advise.c", 'read(request, "tool_calls");')
        with self.assertRaisesRegex(ValueError, "native-history-field"):
            checker.check(self.root)

    def test_hook_archive_cannot_restore_legacy_transcript_write(self):
        path = self.source("src/db1_client/sessions.c", "int db1_primary_session_save(void);")
        with self.assertRaisesRegex(ValueError, "retired-api"):
            checker.check(self.root)
        path.unlink()
        self.source("src/server/server_auth.c", "db1_session_ir_history_save(session, digest, scope, actor, json);")
        checker.check(self.root)
        self.source("src/server/server_auth.c", "db1_primary_session_save(session, client, provider, json);")
        with self.assertRaisesRegex(ValueError, "legacy-transcript-write"):
            checker.check(self.root)

    def test_chat_workers_cannot_restore_provider_transcript_writer(self):
        for relative in ("src/posix/server_compute.c", "src/windows/server_compute.c"):
            with self.subTest(relative=relative):
                path = self.source(relative, "db1_primary_session_save(session, agent, provider, json);")
                with self.assertRaisesRegex(ValueError, "legacy-transcript-write"):
                    checker.check(self.root)
                path.unlink()

    def test_chat_worker_cannot_rebuild_native_wire_history(self):
        self.source("src/posix/server_compute.c", 'read(history, "tool_calls");')
        with self.assertRaisesRegex(ValueError, "native-history-field"):
            checker.check(self.root)

    def test_retired_api_and_driver_calls_in_c_and_go(self):
        for relative, source in [
            ("src/server/rogue.c", "anthropic_stream_feed_openai(s, p);"),
            ("src/server/rogue.c", "shadow_mirror_dispatch(path, raw, length, 0);"),
            ("server-go/modules/rogue/rogue.go", "raw_responses := x"),
            ("src/server/rogue.c", "gateway_delegate_tool_shape(1, 0);"),
            ("src/server/rogue.c", "agent_tool_result_usable(text);"),
            ("src/server/rogue.c", "agent_resolve_auth(agent, token, capacity);"),
            ("src/server/rogue.c", "parsed_response_t result;"),
            ("src/server/rogue.c", "gateway_policy_police_parsed_response(result);"),
            ("src/server/rogue.c", "aimee_ir_shadow_compare_response(result, native, wire);"),
            ("src/server/rogue.c", "openai_responses_emit_policed(result, id, model, time, emit, ctx);"),
            ("src/server/rogue.c", "agent_build_extra_headers(agent, headers, capacity);"),
            ("src/server/rogue.c", "agent_execute_cli_shell_driver(agent, prompt);"),
            ("src/server/rogue.c", "agent_shell_driver_get(name);"),
            ("src/server/rogue.c", "acp_serve_client_request(frame, cwd);"),
            ("src/server/rogue.c", "provider_cli_collect_child(config, result);"),
            ("src/server/rogue.c", "agent_shell_driver_t terminal;"),
            ("src/server/rogue.c", "tool_side_effect(name);"),
            ("src/server/rogue.c", "emit(SHELL_EVENT_TEXT_DELTA);"),
            ("src/server/rogue.c", "driver->parse_response(body);"),
            ("src/server/rogue.c", "driver->build_request(input);"),
        ]:
            with self.subTest(relative=relative, source=source):
                path = self.source(relative, source)
                with self.assertRaisesRegex(ValueError, "IR bypasses"):
                    checker.check(self.root)
                path.unlink()

    def test_legacy_learning_wire_tombstone_cannot_become_prompt_authority(self):
        self.source("src/db1_client/delegation.c", "char *db1_delegate_learning_inject_prompt(void); ")
        self.source("src/db1_client/delegate_learning.h", "char *db1_delegate_learning_inject_prompt(void); ")
        checker.check(self.root)
        self.source("src/server/new.c", "prompt = db1_delegate_learning_inject_prompt(role, prompt, 3);")
        with self.assertRaisesRegex(ValueError, "unqualified-learning-prompt"):
            checker.check(self.root)

    def test_unscoped_approaches_cannot_enter_automatic_delegate_context(self):
        self.source("src/approach_store.c", "char *approach_store_retry_context(const char *goal) { return 0; }")
        self.source("src/headers/approach_store.h", "char *approach_store_retry_context(const char *goal);")
        self.source("src/admin/legacy_approach_inspect.c", "approach_store_recall(goal, results, 5); approach_store_render(results, 5, out, 1024);")
        self.source("src/server/server_compute.c", "db1_execution_test_grade_recall(owner, project, NULL);")
        checker.check(self.root)
        for relative in ("src/server/server_compute.c", "src/server_eval.c",
                         "src/server/server_learning_approaches.c"):
            for call in ("approach_store_retry_context(goal)", "approach_store_recall(goal, out, 5)",
                         "approach_store_render(out, 5, prompt, 1024)"):
                with self.subTest(relative=relative, call=call):
                    path = self.source(relative, call + ";")
                    with self.assertRaisesRegex(ValueError, "unscoped-approach-prompt"):
                        checker.check(self.root)
                    path.unlink()
        self.source("src/server/server_compute.c", "db1_execution_test_grade_recall(owner, project, NULL);")
        self.source("src/server/new.c", "approach_store_retry_context(goal);")
        with self.assertRaisesRegex(ValueError, "unscoped-approach-prompt"):
            checker.check(self.root)

    def test_native_shapes_refused_in_migrated_c_and_go_consumers(self):
        for relative in [*sorted(checker.CANONICAL_FILES),
                         "server-go/modules/economizer/new.go", "server-go/modules/memory/new.go"]:
            for field in ["tool_calls", "function_call", "tool_call_id", "reasoning_content"]:
                path = self.source(relative, f'lookup(message, "{field}");')
                with self.assertRaisesRegex(ValueError, "native-history-field"):
                    checker.check(self.root)
                path.unlink()

    def test_migrated_dispatch_consumers_require_owner_status(self):
        for relative in checker.CLASSIFIED_DISPATCH_FILES:
            for name in ("dispatch_tool_call", "dispatch_tool_call_ctx"):
                path = self.source(relative, name + '(tool, args, timeout);')
                with self.assertRaisesRegex(ValueError, "unclassified-tool-dispatch"):
                    checker.check(self.root)
                path.unlink()
            dispatch = ("dispatch_tool_call_ir_ctx_status" if relative in {"src/posix/agent_runtime.c", "src/server/server_compute_async.c", "src/server/script_rpc.c", "src/server/script_tool_execution.c"}
                        else "dispatch_tool_call_ctx_status")
            self.source(relative, dispatch + '(tool, args, timeout, &failed);')
        checker.check(self.root)

    def test_canonical_dispatch_cannot_recoerce_against_cached_schema(self):
        self.source("src/posix/agent_runtime.c", 'dispatch_tool_call_ctx_status(name, args, timeout, &failed);')
        with self.assertRaisesRegex(ValueError, "canonical-dispatch-cache"):
            checker.check(self.root)
        self.source("src/posix/agent_runtime.c", 'dispatch_tool_call_ir_ctx_status(admitted, args, timeout, &failed);')
        checker.check(self.root)

    def test_plain_consumer_cannot_patch_native_profiles(self):
        for function in ("model_sampling_apply_openai", "model_sampling_apply_anthropic",
                         "agent_anthropic_set_system", "agent_request_shape_openai_messages"):
            self.source("src/server/agent_request_build.c", function + "(agent, request);")
            with self.assertRaisesRegex(ValueError, "post-render-profile"):
                checker.check(self.root)
        self.source("src/server/agent_request_build.c", "sampling = model_sampling_resolve(agent, 0.5);")
        checker.check(self.root)

    def test_canonical_execution_cannot_reload_native_schema_authority(self):
        for name in ("tool_validate", "kb_client_tool_registry_lookup"):
            path = self.source("src/posix/agent_runtime.c", name + '(name, args);')
            with self.assertRaisesRegex(ValueError, "retired-api|native-schema-lookup"):
                checker.check(self.root)
            path.unlink()
        self.source("src/posix/agent_runtime.c", "tool_validate_ir(admitted, args, error, capacity);")
        self.source("src/server/agent_policy.c", "kb_client_tool_registry_lookup(name, enabled);")
        checker.check(self.root)

    def test_native_mcp_cannot_reintroduce_kb_read_fallback(self):
        self.source("src/server/server_native_tool_admission.c", "tool_side_effect(tool);")
        with self.assertRaisesRegex(ValueError, "retired-api"):
            checker.check(self.root)
        self.source("src/server/server_native_tool_admission.c", "tool_policy_metadata(tool, metadata);")
        checker.check(self.root)

    def test_canonical_host_profiles_cannot_rebuild_native_requests(self):
        for function in checker.CANONICAL_PROFILE_FUNCTIONS:
            for wire in ("openai", "anthropic", "responses"):
                with self.subTest(function=function, wire=wire):
                    self.source("src/server/agent_bridge.c", f"""
                    cJSON *{function}(void) {{
                        if (condition) {{ log("literal close brace: }}"); }}
                        /* }} */ return agent_build_request_{wire}(agent);
                    }}
                    """)
                    with self.assertRaisesRegex(ValueError, "native-profile-rebuild"):
                        checker.check(self.root)
        self.source("src/server/agent_bridge.c", """
            cJSON *agent_build_request_ir(void) { return render(ir); }
            cJSON *compatibility_boundary(void) {
                return agent_build_request_openai(agent);
            }
        """)
        with self.assertRaisesRegex(ValueError, "retired-api"):
            checker.check(self.root)

    def test_core_repair_cannot_detect_provider_history(self):
        for field in ("tool_calls", "function_call", "function_call_output", "tool_use_id"):
            with self.subTest(field=field):
                self.source("src/server/agent_bridge.c", f"""
                    int message_history_repair(cJSON *history) {{
                        return lookup(history, "{field}");
                    }}
                """)
                with self.assertRaisesRegex(ValueError, "native-history-field"):
                    checker.check(self.root)
        self.source("src/server/agent_bridge.c", """
            int message_history_repair(cJSON *history) { return repair_ir(history); }
            cJSON *compatibility_output(void) { return lookup(native, "tool_calls"); }
        """)
        checker.check(self.root)
        for symbol in ("tool_call_normalize_assistant_arguments", "tool_call_sanitize_assistant_arguments"):
            self.source("src/server/rogue.c", symbol + "(message);")
            with self.assertRaisesRegex(ValueError, "retired-api"):
                checker.check(self.root)

    def test_native_hook_envelopes_stop_at_entry_adapter(self):
        for relative in checker.CANONICAL_HOOK_FILES:
            path = self.source(relative, 'lookup(payload, "tool_input");')
            with self.assertRaisesRegex(ValueError, "native-hook-field"):
                checker.check(self.root)
            path.unlink()
        self.source("src/modules/translation/hook_ir.c", 'lookup(native, "tool_input");')
        checker.check(self.root)

    def test_explicit_native_adapters_and_fixture_data_remain_valid(self):
        self.source("src/modules/translation/aimee_backend_openai.c", 'lookup(native, "tool_calls");')
        self.source("src/tests/test_native.c", 'anthropic_stream_feed_openai(s, p);')
        self.source("server-go/modules/memory/example_test.go", 'lookup(native, "tool_calls")')
        checker.check(self.root)

    def test_comments_and_literal_comment_markers(self):
        self.source("src/server/session_compact.c", '// "tool_calls"\n/* raw_responses */\n"https://example.org";')
        checker.check(self.root)
        self.source("src/server/session_compact.c", '"/* literal */"; lookup(x, "tool_calls");')
        with self.assertRaisesRegex(ValueError, "native-history-field"):
            checker.check(self.root)

    def test_exact_exception_expires_and_cannot_move_or_expand(self):
        path = self.source("src/server/session_compact.c", 'lookup(x, "tool_calls");\n')
        finding = checker.findings(self.root)[0]
        exception = {**finding, "owner": "history", "reason": "bounded migration seam",
                     "expires": "2026-11-01", "retirement": "replace this exact reader"}
        self.write_ledger([exception])
        checker.check(self.root, dt.date(2026, 10, 18))
        with self.assertRaisesRegex(ValueError, "expired"):
            checker.check(self.root, dt.date(2026, 11, 1))
        path.write_text('\nlookup(x, "tool_calls");\n')
        with self.assertRaisesRegex(ValueError, "unused or changed"):
            checker.check(self.root, dt.date(2026, 10, 18))
        path.write_text('lookup(x, "choices");\n')
        with self.assertRaisesRegex(ValueError, "unused or changed"):
            checker.check(self.root, dt.date(2026, 10, 18))


if __name__ == "__main__":
    unittest.main()
