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

    def test_production_tree(self):
        checker.check(checker.ROOT)

    def test_retired_api_and_driver_calls_in_c_and_go(self):
        for relative, source in [
            ("src/server/rogue.c", "anthropic_stream_feed_openai(s, p);"),
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
            dispatch = ("dispatch_tool_call_ir_ctx_status" if relative == "src/posix/agent_runtime.c"
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
