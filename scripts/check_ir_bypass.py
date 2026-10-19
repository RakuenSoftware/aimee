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
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
LEDGER = Path("tests/baselines/ir-bypass-exceptions.json")
RETIRED = (
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
    "retired-api": re.compile(r"\b(?:" + "|".join(RETIRED) + r")\b"),
    "driver-bypass": re.compile(r"->\s*(?:parse_response|build_request)\s*\("),
    "unclassified-tool-dispatch": re.compile(r"\bdispatch_tool_call(?:_ctx)?\s*\("),
    "native-profile-rebuild": re.compile(r"\bagent_build_request_(?:openai|anthropic|responses)\s*\("),
    "post-render-profile": re.compile(r"\b(?:model_sampling_apply_(?:openai|anthropic)|agent_anthropic_set_system|agent_request_shape_openai_messages)\s*\("),
    "native-hook-field": re.compile(r'"(?:tool_name|tool_input|tool_response|tool_use_id|tool_call_id|tool_namespace)"'),
    "native-history-field": re.compile(
        r'"(?:tool_calls|function_call|function_call_output|tool_call_id|tool_use_id|'
        r'call_id|choices|reasoning_content|input_text|output_text|input_schema)"'),
}
# These consumers have completed their history/algorithm cutover. Provider
# adapters are intentionally outside this set; newly migrated consumers join it.
CANONICAL_FILES = {
    "src/posix/agent_runtime.c", "src/server/session_compact.c",
    "src/server/agent_request_build.c",
    "src/server/server_session_log.c", "src/server/server_chat_ir.c",
    "src/server/primary_session_adapter.c", "src/server/server_native_cache.c",
}
CANONICAL_HOOK_FILES = {"src/cmd_hooks.c", "src/server/server_hooks.c"}
CANONICAL_TREES = ("server-go/modules/economizer/", "server-go/modules/memory/")
# These call sites consume the tool/receipt owner's semantic outcome. The other
# native output/compatibility boundaries are not declared globally migrated.
CLASSIFIED_DISPATCH_FILES = {
    "src/posix/agent_runtime.c", "src/server/script_rpc.c",
    "src/server/server_compute_async.c",
}
CANONICAL_PROFILE_FUNCTIONS = {
    "agent_build_request_ir", "agent_openai_profile_ir", "agent_responses_profile_ir",
    "agent_anthropic_profile_ir",
}

# Match strings first so comment markers inside literal data remain literal data.
LEXEMES = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|`[^`]*`|/\*[\s\S]*?\*/|//[^\n]*')


def without_comments(source: str) -> str:
    return LEXEMES.sub(
        lambda match: re.sub(r"[^\n]", " ", match[0])
        if match[0].startswith(("/*", "//")) else match[0], source)


def profile_function_spans(source: str) -> list[tuple[int, int]]:
    # Ignore literal/comment braces when finding the already migrated function
    # bodies. Compatibility output APIs elsewhere in this file remain boundaries.
    code = LEXEMES.sub(lambda match: re.sub(r"[^\n]", " ", match[0]), source)
    pattern = re.compile(r"\b(?:" + "|".join(sorted(CANONICAL_PROFILE_FUNCTIONS)) +
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
    for tree in ("src", "server-go"):
        for path in sorted((root / tree).rglob("*")):
            relative = path.relative_to(root).as_posix()
            if path.suffix not in {".c", ".h", ".go"} or not path.is_file():
                continue
            parts = path.relative_to(root).parts
            if any(part in {"tests", "testdata", "vendor", "build", "tmp"} for part in parts) or path.name.endswith("_test.go"):
                continue
            yield relative, path


def findings(root: Path) -> list[dict]:
    result = []
    for relative, path in sources(root):
        original = path.read_text(encoding="utf-8")
        source = without_comments(original)
        canonical = relative in CANONICAL_FILES or relative.startswith(CANONICAL_TREES)
        profile_spans = profile_function_spans(source) if relative == "src/server/agent_bridge.c" else []
        for rule, pattern in RULES.items():
            if rule == "post-render-profile" and relative != "src/server/agent_request_build.c":
                continue
            if rule == "native-hook-field" and relative not in CANONICAL_HOOK_FILES:
                continue
            if rule == "native-history-field" and not canonical:
                continue
            if rule == "unclassified-tool-dispatch" and relative not in CLASSIFIED_DISPATCH_FILES:
                continue
            for match in pattern.finditer(source):
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
    print("ir-bypass: retired APIs and migrated C/Go consumers clean; no native fallback")


if __name__ == "__main__":
    main()
