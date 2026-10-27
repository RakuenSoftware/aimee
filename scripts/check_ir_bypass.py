#!/usr/bin/env python3
"""Keep retired native paths out and migrated IR consumers free of wire readers.

Native entry/egress adapters remain explicit boundaries. This guard expands with
consumer cutovers; its scope is not a claim that every consumer has migrated.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import io
import tokenize
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
LEDGER = Path("tests/baselines/ir-bypass-exceptions.json")
RETIRED_OWNERS = ('scripts/llm-chat.py', 'scripts/curator-extract.py',
                  'scripts/curator-synthesize.py', 'scripts/learning-synthesize.py',
                  'scripts/llm-rewrite.py', 'src/modules/gateway/gateway_pipeline.c','src/modules/gateway/include/aimee/gateway/gateway_pipeline.h','src/pipeline/gw_stage_registry.c','src/pipeline/gw_stage_registry.h', 'integrations/discord/prompt_budget.py', 'integrations/discord/test_prompt_budget.py','src/cmd_branch.c', 'src/cmd_core.c', 'src/cmd_data.c', 'src/cmd_doctor.c', 'src/cmd_infra.c', 'src/cmd_onboard.c', 'src/cmd_run.c', 'src/cmd_session_history.c', 'src/dashboard.c', 'src/headers/cmd_branch.h', 'src/headers/cmd_run.h', 'src/server/aimee_ir_shadow.c', 'src/headers/aimee_ir_shadow.h', 'server-go/modules/economizer/gateway_wire.go', 'server-go/modules/economizer/planner_openai.go', 'server-go/modules/economizer/planner_anthropic.go', 'server-go/modules/economizer/proof.go')
RETIRED = (
    "probeParts", "probeUsage", "probeStop", "probeResponsesStream",
    "probeResponseOutput", "probeMetadata",
    "gw_pipeline_run_request", "gw_stage_registry_build", "gw_request_t",
    "gw_mem_target_t", "gw_api_t", "gw_stage_t", "gw_stage_slot_t",
    "aimee_ir_shadow_observe_request", "gateway_policy_apply_request",
    "gateway_policy_strip_tools", "gateway_policy_pin_model", "message_history_repair",
    "server_native_runtime_open", "server_native_runtime_open_request",
    "agent_build_exec_context", "agent_build_exec_context_ex",
    "agent_build_exec_context_for_role", "agent_build_exec_context_checked",
    "responses_backend_sse_object",
    "gateway_handle_message", "rpc_chat_send_stream",
    "dispatch_tool_call", "dispatch_tool_call_ctx", "dispatch_tool_call_ctx_status",
    "agent_build_request", "agent_build_request_ir", "agent_prepare_request_ir",
    "plugin_chook_apply_pre_llm",
    "platform_delegate_run_background", "platform_trace_run_task",
    "cron_build_job_prompt", "cron_run_llm_prompt",
    "roadmap_milestone_review_prompt", "roadmap_reassess_build_prompt",
    "roadmap_decompose_build_prompt",
    "roundtable_chair_build_prompt",
    "agent_tools_dispatch_owned",
    "cli_http_request_stream", "turn_narration_t", "turn_narration_reset",
    "turn_narration_add", "turn_narration_inc_iter", "turn_narration_build",
    "TURN_NAR_MAX_CALLS", "TURN_NAR_NAME_LEN", "TURN_NAR_ARG_LEN",
    "agent_collect_tool_prompts", "router_advise_turn", "delegation_mailbox_t", "mailbox_acquire", "mailbox_find",
    "mailbox_wait", "mailbox_reply", "mailbox_release", "mailbox_put", "tl_mailbox",
    "agent_vote", "delegate_token_budget_for_agents",
    'cli_session_set_stream_cb',
    'cli_session_get_stream_cb',
    'cli_session_set_cancel_check',
    'cli_session_get_cancel_check',
    'cli_session_set_heartbeat_cb',
    'cli_session_get_heartbeat_cb',
    'cli_session_set_error_grace_ms',
    'cli_session_get_error_grace_ms',
    'cli_session_set_kind',
    'cli_session_mark_baseline',
    'cli_session_capture_cmd',
    'cli_session_prepare_claude',
    'cli_session_isolated_claude_home',
    'cli_session_isolated_codex_home',
    'cli_session_resolve_cwd',
    'cli_session_send',
    'cli_session_capture',
    'cli_session_extract_response',
    'cli_session_recv',
    'cli_session_make_execution_name',
    'cli_session_strip_ansi',
    'cli_session_delta',
    'cli_session_harness_home',
    'cli_oauth_vault_materialize_get',
    'compact_prune_tool_results',
    'compact_prune_estimate_savings',
    'COMPACT_PRUNE_THRESHOLD_BYTES',
    'normalize_roundtable_brief',
    'add_roundtable_arrays',
    'normalized_roundtable_brief_t',

    "delegate_child_export_context_env",
    "agent_source_authority_export_env",
    'openai_request_double',
    'openai_request_int',
    'openai_format_chat_completion',
    'openai_format_chat_completion_reason',
    'openai_format_response',
    'openai_format_response_status',
    'openai_format_run',
    'openai_format_responses_delta',
    'openai_format_responses_completed',
    'openai_format_responses_incomplete',
    'openai_responses_message_item',
    'openai_responses_function_call_item',
    'openai_format_responses_msg_item_added',
    'openai_format_responses_msg_item_done',
    'openai_format_responses_fc_item_added',
    'openai_format_responses_fc_item_done',
    'openai_format_responses_fc_args_delta',
    'openai_format_responses_fc_args_done',
    'openai_format_responses_completed_items',

    "agent_coordinate", "agent_execute_with_plan", "agent_job_resume",
    "agent_plan_execute", "agent_plan_rollback_step", "agent_plan_verify", "plan_assign_waves",
    "agent_loop_init", "agent_loop_free", "agent_loop_run", "agent_loop_parse_completion",
    "agent_loop_t",
    "provider_cli_spawn_argv", "acp_turn_consume", "acp_turn_state_t", "acp_tools",
    "host_model_capture",
    "cli_claude_allowed_tools", "cli_claude_allowed_tools_count", "claude_build_argv",
    "claude_spawn", "claude_execute", "CLAUDE_ALLOWED_TOOLS", "CLAUDE_ARG_MAX",
    "cli_codex_parse_server_request_method", "cli_codex_approval_decision",
    "cli_codex_command_restarts_aimee_server", "cli_codex_json_has_server_restart",
    "cli_codex_method_is_shell_approval", "cli_codex_send_approval_decision",
    "cli_codex_send_permissions_decision", "cli_codex_send_user_input_decision",
    "cli_codex_send_elicitation_decision", "CLI_CODEX_APPROVAL_POLICY_CONFIG",
    "CLI_CODEX_SANDBOX_READ_ONLY_CONFIG", "CLI_CODEX_SANDBOX_WORKSPACE_WRITE_CONFIG",
    "CLI_CODEX_SANDBOX_DANGER_CONFIG",
    "builtin_chat_send_ex", "builtin_chat_stream_t", "builtin_chat_stream_control_t",
    "chat_stream_on_open", "chat_stream_finish", "g_acp_tool_seq",
    "db1_primary_session_save", "db1_primary_session_load", "db1_primary_session_delete",
    "db1_primary_session_alloc_recent", "db1_primary_session_alloc_search",
    "db1_webchat_claude_session_get", "db1_webchat_claude_session_owned_by_other",
    "db1_webchat_claude_session_bind",
    "agent_responses_sse_response_object", "agent_tools_builtin_catalog",
    "provider_cli_adapter_execute", "agent_execute_cli_codex", "agent_execute_cli_codex_at_cwd",
    "provider_client_complete", "provider_client_build_openai", "provider_import_request",
    "build_tools_array", "build_tools_array_responses", "build_tools_array_anthropic",
    "agent_tools_filter_for_role", "agent_tools_filter_anthropic_for_role",
    "agent_tools_append_delegate_respond_tool", "delegate_respond_spec",
    "agent_tools_sanitize_for_provider", "agent_tools_sanitize_for_agent",
    "agent_build_request_openai", "agent_build_request_responses", "agent_build_request_anthropic",
    "agent_anthropic_set_system", "model_sampling_apply_openai", "model_sampling_apply_anthropic",
    "agent_request_shape_openai_messages",
    "RunProbeWorker", "newProbeWorker", "runConfiguredProbe", "splitProbeCommand", "validProbeOutput",
    "externalTextObservation", "CLIText", "RunWatchdog",
    "shadow_mirror_dispatch",
    "kb_curator_sidecar_run", "kb_curator_shell_quote", "kb_curator_describe_wait_status",
    "kb_curator_append_sidecar_error", "kb_curator_pick_sidecar_command",
    "kb_curator_resolve_sidecar_command",
    "gw_stage_router", "ingress_preinject_query_from_messages",
    "ingress_preinject_last_assistant_from_messages",
    "messages_compact_consecutive",
    "agent_store_feedback",
    "provider_cli_collect_child",
    "provider_cli_parse_output",
    "aimee_ir_path_enabled", "aimee_ir_stream_relay_enabled",
    "aimee_ir_build_provider_body", "aimee_ir_shadow_compare_bodies", "aimee_ir_shadow_enabled",
    "aimee_ir_prepend_active_persona_text", "aimee_ir_prepend_persona_text",
    "cli_event_t", "CLI_EVENT_NONE", "CLI_EVENT_TEXT_DELTA", "CLI_EVENT_TOOL_START",
    "CLI_EVENT_TOOL_COMPLETE", "CLI_EVENT_TURN_COMPLETE", "CLI_EVENT_ERROR",
    "provider_cli_default_build_prompt", "provider_cli_format_json_tool_result",
    "provider_cli_event_is_write", "provider_cli_parse_json_line_common",
    "cli_codex_chat_request_t", "cli_codex_chat_result_t", "cli_codex_chat_event_cb",
    "cli_codex_chat_stream", "server_native_codex_chat_stream",
    "cli_codex_parse_agent_message",
    "acp_serve_client_request",
    "acp_read_file_bounded",
    "acp_resolve_in_workdir",
    "tool_side_effect", "tool_validate",
    "tool_call_normalize_assistant_arguments", "tool_call_sanitize_assistant_arguments",
    "agent_execute_cli_shell_driver", "agent_shell_driver_get", "agent_shell_driver_register",
    "agent_shell_driver_t", "SHELL_EVENT_TEXT_DELTA", "SHELL_EVENT_TOOL_START",
    "SHELL_EVENT_TOOL_COMPLETE", "SHELL_EVENT_SESSION_ID", "SHELL_EVENT_TURN_COMPLETE",
    "SHELL_EVENT_ERROR",
    "openai_parse_chat_request", "openai_parse_completion_request",
    "openai_parse_responses_request", "openai_parse_responses_to_chat",
    "openai_split_chat_request",
    "raw_responses", "build_provider_body", "translate_request",
    "agent_resolve_auth", "agent_build_extra_headers",
    "parsed_response_t", "parsed_tool_call_t", "agent_free_parsed_response",
    "gateway_policy_police_parsed_response", "aimee_ir_shadow_compare_response",
    "openai_format_chat_completion_tool_calls", "openai_format_chat_chunk_tool_calls",
    "openai_responses_emit_policed",
    "gateway_delegate_tool_shape", "agent_tool_result_usable", "GW_TOOL_SHAPE_NAMED",
    "GW_TOOL_SHAPE_FUNCTION_NESTED", "GW_TOOL_SHAPE_UNSUPPORTED",
    "agent_ir_parse_json_response", "agent_ir_parse_responses",
    "agent_build_openai_assistant_message_from_calls",
    "delegate_respond_message", "agent_tools_strip_delegate_respond",
    "anthropic_system_to_text", "anthropic_messages_to_openai",
    "anthropic_tools_to_openai", "anthropic_tools_to_responses",
    "anthropic_response_from_parsed", "anthropic_stream_begin",
    "anthropic_stream_feed_openai", "anthropic_stream_finish",
    "anthropic_stream_get_usage", "anthropic_stream_free", "emit_message_as_sse",
)
RULES = {
    "discord-model-endpoint": re.compile(r"\bconfig\.endpoint\b|/v1/(?:chat/completions|responses|messages)\b"),
    "gateway-display-conversation": re.compile(r"\b(?:cli_chat_request_stream|cli_chat_stream|cli_ir_user_message)\s*\("),
    "public-context-reset": re.compile(
        r'\bmemset\s*\(\s*&[^,;\n]*\.context\s*,\s*0\s*,|'
        r'\.context\s*=\s*\(\s*aimee_context_meta_t\s*\)\s*\{\s*0\s*\}'),
    "first-party-text-message": re.compile(
        r'\bcJSON_AddStringToObject\s*\([^,]+,\s*"(?:message|prompt|system_prompt|original_request)"\s*,'),
    "native-control-reparse": re.compile(r"\b(?:anthropic_backend_parse|anthropic_frontend_parse|anthropic_stream_to_deltas)\s*\("),
    "native-control-history": re.compile(r"\b(?:message|delta|harness_output_decode)\s*\("),
    "acp-ambient-launch": re.compile(r"\b(?:execlp|execvp|setenv|unsetenv)\s*\("),
    "codex-ambient-launch": re.compile(r"\b(?:execlp|execvp|setenv|unsetenv)\s*\("),
    "delegate-model-bypass": re.compile(r"\b(?:exec\.Command(?:Context)?|[A-Za-z_]\w*\.TextResponse)\s*\("),
    "model-command-bypass": re.compile(r"\b(?:platform_exec_pipe|kb_curator_sidecar_run|popen|subprocess\.(?:run|Popen|check_output)|asyncio\.create_subprocess_(?:exec|shell))\s*\("),
    "legacy-transcript-write": re.compile(r"\bdb1_primary_session_save\s*\("),
    "legacy-session-search": re.compile(r"\bdb1_primary_session_(?:alloc_search|alloc_recent|get_latest)\b|\b(?:FROM|JOIN)\s+primary_sessions\b", re.IGNORECASE),
    "unscoped-approach-prompt": re.compile(r"\bapproach_store_(?:retry_context|recall|render)\s*\("),
    "unqualified-learning-prompt": re.compile(r"\bdb1_delegate_learning_inject_prompt\s*\("),
    "provider-probe-exec": re.compile(r"\bexec\.(?:Command|CommandContext)\s*\("),
    "retired-api": re.compile(r"\b(?:" + "|".join(RETIRED) + r")\b"),
    "raw-model-contract": re.compile(
        r"\(\s*\*\s*skill_trial_runner_fn\s*\)\s*\(\s*void\s*\*\s*\w+\s*,\s*const\s+char\s*\*|"
        r"\baux_call\s*\([^)]*const\s+char\s*\*\s*(?:prompt|input)\b|"
        r"\(\s*\*\s*roadmap_model_fn\s*\)\s*\(\s*const\s+char\s*\*|"
        r"\b(?:delegate_(?:ensemble|roundtable)_run|aimee_panel_(?:run|aggregate))\s*\([^)]*const\s+char\s*\*\s*(?:prompt|task)\b"),
    "untyped-exec-context": re.compile(r"\bagent_build_exec_context(?:_checked)?\s*\("),
    "raw-steer-contract": re.compile(
        r"\b(?:chat_steer_set|server_chat_ir_(?:replace|reject)_steer)\s*\([^)]*const\s+char\s*\*\s*message\b|"
        r"\bchat_steer_take\s*\([^)]*char\s*\*\s*\*\s*out_message\b"),
    "driver-bypass": re.compile(r"->\s*(?:parse_response|build_request)\s*\("),
    "unclassified-tool-dispatch": re.compile(r"\bdispatch_tool_call(?:_ctx)?\s*\("),
    "native-tool-reparse": re.compile(r"\b(?:openai_ir_parse_tool|anthropic_ir_parse_tool)\s*\("),
    "dead-tool-builder-abi": re.compile(r"(?:->|\.)\s*build_tools\b|\(\s*\*\s*build_tools\s*\)"),
    "native-profile-rebuild": re.compile(r"\bagent_build_request_(?:openai|anthropic|responses)\s*\("),
    "post-render-profile": re.compile(r"\b(?:model_sampling_apply_(?:openai|anthropic)|agent_anthropic_set_system|agent_request_shape_openai_messages)\s*\("),
    "canonical-dispatch-cache": re.compile(r"\bdispatch_tool_call_ctx_status\s*\("),
    "native-schema-lookup": re.compile(r"\b(?:tool_validate|kb_client_tool_registry_lookup)\s*\("),
    "native-hook-field": re.compile(r'"(?:tool_name|tool_input|tool_response|tool_use_id|tool_call_id|tool_namespace)"'),
    "native-history-field": re.compile(
        r'"(?:tool_calls|function_call|function_call_output|tool_call_id|tool_use_id|'
        r'call_id|choices|reasoning_content|input_text|output_text|input_schema)"'),
}
# Only these migrated model consumers join the guard, not Python fixtures,
# Discord delivery, or the distinct KB/external service clients.
DISCORD_MODEL_FILES = {
    "integrations/discord/bridge.py", "integrations/discord/model_client.py",
    "integrations/discord/model_ir.py", "integrations/discord/workflow_chat.py",
}
PYTHON_HISTORY_FIELD = re.compile('(?P<quote>["\'])' +
    RULES["native-history-field"].pattern[1:-1] + r"(?P=quote)")

# These consumers have completed their history/algorithm cutover. Provider
# adapters are intentionally outside this set; newly migrated consumers join it.
CANONICAL_FILES = DISCORD_MODEL_FILES | {
    "src/server/server_acp_execution.c", "src/server/server_acp_resources.c",
    "src/server/server_provider_cli_execute.c",
    "src/posix/agent_runtime.c", "src/posix/server_compute.c", "src/server/session_compact.c",
    "src/server/router_advise.c", "src/server/server_compute.c",
    "src/server/aux_router.c", "src/server/server_cron.c",
    "src/modules/roundtable/delegate_ensemble.c", "src/modules/roundtable/ensemble_request.c",
    "src/modules/delegates/delegate_prompt.c",
    "src/server/script_tool_execution.c",
    "src/server/agent_request_build.c",
    "src/server/server_session_log.c", "src/server/server_chat_ir.c",
    "src/server/primary_session_adapter.c", "src/server/server_native_cache.c",
    "src/server/server_native_execution.c", "src/server/session_search_tool.c",
    "server-go/modules/aimee/families/session_ir_search.go",
}
# Business JSON is allowed after canonical terminal admission. These model
# consumers cannot obtain model results from arbitrary command stdout.
CANONICAL_MODEL_FILES = DISCORD_MODEL_FILES | {
    "src/kb/kb_memory_facts.c", "src/kb/kb_reflection.c",
    "src/kb/kb_surprising_judge.c", "src/kb/kb_learning_synth.c",
    "src/modules/kb-synthesis/kb_curator_llm.c",
    "src/modules/kb-synthesis/kb_curator_extract.c",
    "src/modules/kb-synthesis/kb_curator_extract_code.c",
    "src/modules/kb-synthesis/kb_curator_judge.c",
    "src/modules/kb-synthesis/kb_curator_synthesize.c",
}
CANONICAL_HOOK_FILES = {"src/cmd_hooks.c", "src/server/server_hooks.c"}
CANONICAL_TREES = ("server-go/modules/economizer/", "server-go/modules/memory/",
                   "server-go/modules/aimee/peer/", "server-go/modules/roundtable/panel/",
                   "server-go/internal/engine/", "server-go/delegate/")
# String-only tool entries are retired throughout production C.
CANONICAL_PROFILE_FUNCTIONS = {
    "prepare_host_profile", "agent_prepare_seeded_request_ir", "agent_build_seeded_request_ir",
    "agent_openai_profile_ir", "agent_responses_profile_ir",
    "agent_anthropic_profile_ir",
}

# Match strings first so comment markers inside literal data remain literal data.
LEXEMES = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|`[^`]*`|/\*[\s\S]*?\*/|//[^\n]*')


def without_comments(source: str) -> str:
    return LEXEMES.sub(
        lambda match: re.sub(r"[^\n]", " ", match[0])
        if match[0].startswith(("/*", "//")) else match[0], source)


def without_python_comments(source: str) -> str:
    # tokenize preserves single/double/triple-quoted literals, escaped quotes and
    # URL comment markers. Mask comments without moving diagnostic line numbers.
    lines = source.splitlines(keepends=True)
    try:
        for token in tokenize.generate_tokens(io.StringIO(source).readline):
            if token.type == tokenize.COMMENT:
                row, start = token.start
                end = token.end[1]
                line = lines[row - 1]
                lines[row - 1] = line[:start] + " " * (end - start) + line[end:]
    except (tokenize.TokenError, IndentationError) as error:
        raise ValueError("invalid migrated Python source") from error
    return "".join(lines)


def profile_function_spans(source: str, names=None) -> list[tuple[int, int]]:
    # Ignore literal/comment braces when finding the already migrated function
    # bodies. Compatibility output APIs elsewhere in this file remain boundaries.
    code = LEXEMES.sub(lambda match: re.sub(r"[^\n]", " ", match[0]), source)
    pattern = re.compile(r"\b(?:" + "|".join(sorted(CANONICAL_PROFILE_FUNCTIONS if names is None else names)) +
                         r")\s*\([^)]*\)\s*\{")
    spans = []
    for match in pattern.finditer(code):
        depth = 1
        end = match.end()
        while end < len(code) and depth:
            depth += (code[end] == "{") - (code[end] == "}")
            end += 1
        spans.append((match.end(), end))
    return spans


def sources(root: Path):
    for relative in sorted(DISCORD_MODEL_FILES):
        path = root / relative
        if path.is_file():
            yield relative, path
    for tree in ("src", "server-go", "runtime-web", "frontend/src"):
        for path in sorted((root / tree).rglob("*")):
            relative = path.relative_to(root).as_posix()
            if path.suffix not in {".c", ".h", ".go", ".ts", ".tsx"} or not path.is_file():
                continue
            parts = path.relative_to(root).parts
            if any(part in {"tests", "testdata", "vendor", "build", "tmp"} for part in parts) or path.name.endswith(("_test.go", ".test.ts", ".test.tsx")):
                continue
            yield relative, path


def findings(root: Path) -> list[dict]:
    result = [{"file": path, "rule": "retired-owner", "line": 1, "match": path}
              for path in RETIRED_OWNERS if (root / path).exists()]
    for relative, path in sources(root):
        original = path.read_text(encoding="utf-8")
        source = without_python_comments(original) if relative in DISCORD_MODEL_FILES else without_comments(original)
        canonical = relative in CANONICAL_FILES or relative.startswith(CANONICAL_TREES)
        profile_spans = profile_function_spans(source) if relative == "src/server/agent_bridge.c" else []
        # Public delegate result/view metadata uses a numeric tool_calls counter,
        # not provider-native assistant/tool history. The worker remains guarded.
        view_spans = (profile_function_spans(source, {
            "compute_terminal_outcome", "delegate_build_result_response"})
            if relative == "src/server/server_compute.c" else [])
        native_control_spans = (profile_function_spans(source, {
            "aimee_claude_frame_ir_decode", "aimee_codex_frame_ir_decode", "aimee_codex_reply_ir_decode",
            "aimee_claude_frame_ir", "aimee_codex_frame_ir", "aimee_codex_reply_ir"})
            if relative == "src/modules/translation/harness_ir.c" else [])
        for rule, pattern in RULES.items():
            if rule == "discord-model-endpoint" and relative not in DISCORD_MODEL_FILES:
                continue
            if rule == "native-history-field" and relative in DISCORD_MODEL_FILES:
                pattern = PYTHON_HISTORY_FIELD
            if rule == "gateway-display-conversation" and not relative.startswith("src/gateway/"):
                continue
            if rule == "public-context-reset" and relative not in {
                    "src/posix/server_compute.c", "src/server/server_agent.c"}:
                continue
            if rule == "first-party-text-message" and relative not in {
                    "src/cli_chat_stream.c", "src/cli_v1_routes.c",
                    "src/cli_v1_routes_e.c", "src/gateway/gateway_ctx.c"}:
                continue
            if rule == "untyped-exec-context" and (
                    not relative.endswith(".c") or relative == "src/server/agent_runtime.c"):
                continue
            if rule in {"native-control-reparse", "native-control-history"} and relative != "src/modules/translation/harness_ir.c":
                continue
            if rule == "acp-ambient-launch" and relative != "src/server/cli_acp.c":
                continue
            if rule == "codex-ambient-launch" and relative != "src/server/cli_codex.c":
                continue
            if rule == "provider-probe-exec" and Path(relative).parent.as_posix() != "server-go/modules/providers":
                continue
            if rule == "delegate-model-bypass" and Path(relative).parent.as_posix() != "server-go/modules/delegates":
                continue
            if rule == "model-command-bypass" and relative not in CANONICAL_MODEL_FILES:
                continue
            if rule == "legacy-transcript-write" and relative not in {
                    "src/server/server_auth.c", "src/posix/server_compute.c",
                    "src/windows/server_compute.c"}:
                continue
            if rule == "legacy-session-search" and relative not in {
                    "src/server/session_search_tool.c",
                    "server-go/modules/aimee/families/session_ir_search.go"}:
                continue
            # Legacy storage/read-only administration is not prompt admission.
            # The retry-context API may exist, but no production caller can use
            # it; direct recall/render also cannot bypass canonical execution
            # context in the automatic delegate assembly owner.
            if rule == "unscoped-approach-prompt":
                if relative in {"src/approach_store.c", "src/headers/approach_store.h"}:
                    continue
                if relative not in {"src/server/server_compute.c", "src/server_eval.c", "src/server/server_learning_approaches.c"}:
                    pattern = re.compile(r"\bapproach_store_retry_context\s*\(")
            if rule == "unqualified-learning-prompt" and relative in {"src/db1_client/delegation.c", "src/db1_client/delegate_learning.h"}:
                continue
            if rule == "dead-tool-builder-abi" and relative != "src/modules/delegates/include/aimee/delegates/delegate_driver.h" and not relative.startswith("src/modules/delegates/"):
                continue
            if rule == "native-tool-reparse" and relative not in {"src/server/agent_bridge.c", "src/server/agent_tools.c", "src/posix/agent_runtime.c", "src/server/server_native_execution.c"}:
                continue
            if rule == "post-render-profile" and relative != "src/server/agent_request_build.c":
                continue
            if rule in {"canonical-dispatch-cache", "unclassified-tool-dispatch"} and not relative.endswith(".c"):
                continue
            if rule == "native-schema-lookup" and not canonical:
                continue
            if rule == "native-hook-field" and relative not in CANONICAL_HOOK_FILES:
                continue
            if rule == "native-history-field" and not canonical:
                continue
            for match in pattern.finditer(source):
                if rule == "native-control-history" and not any(
                        start <= match.start() < end for start, end in native_control_spans):
                    continue
                if rule == "native-history-field" and any(
                        start <= match.start() < end for start, end in view_spans):
                    continue
                if rule == "native-tool-reparse" and relative == "src/server/agent_bridge.c" and not any(
                        start <= match.start() < end for start, end in profile_spans):
                    continue
                if rule == "native-profile-rebuild" and not any(
                        start <= match.start() < end for start, end in profile_spans):
                    continue
                line = source.count("\n", 0, match.start()) + 1
                literal = original.splitlines()[line - 1].strip()
                result.append({"file": relative, "rule": rule, "line": line,
                               "sha256": hashlib.sha256(literal.encode()).hexdigest()})
    return result


def check(root: Path, today: dt.date | None = None) -> None:
    today = today or dt.date.today()
    ledger = json.loads((root / LEDGER).read_text(encoding="utf-8"))
    if set(ledger) != {"version", "exceptions"} or ledger["version"] != 1 or not isinstance(ledger["exceptions"], list):
        raise ValueError("invalid IR bypass ledger")
    actual = findings(root)
    for exception in ledger["exceptions"]:
        required = {"file", "rule", "line", "sha256", "owner", "reason", "expires", "retirement"}
        if set(exception) != required or exception["rule"] not in RULES:
            raise ValueError("incomplete IR bypass exception")
        if any(not isinstance(exception[key], str) or not exception[key].strip()
               for key in required - {"line"}) or type(exception["line"]) is not int:
            raise ValueError("invalid IR bypass exception fields")
        if dt.date.fromisoformat(exception["expires"]) <= today:
            raise ValueError(f"expired IR bypass exception: {exception['file']}")
        identity = {key: exception[key] for key in ("file", "rule", "line", "sha256")}
        if identity not in actual:
            raise ValueError(f"unused or changed IR bypass exception: {exception['file']}")
        actual.remove(identity)
    if actual:
        raise ValueError("IR bypasses:\n" + "\n".join(
            f"{row['file']}:{row['line']} {row['rule']}" for row in actual))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args()
    try:
        check(args.root)
    except (ValueError, KeyError, OSError) as error:
        raise SystemExit(f"ir-bypass: {error}") from error
    print("ir-bypass: retired APIs and migrated C/Go/Python consumers clean; no native fallback")


if __name__ == "__main__":
    main()
