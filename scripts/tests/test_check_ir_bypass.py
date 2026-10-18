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
            ("src/server/rogue.c", "driver->parse_response(body);"),
            ("src/server/rogue.c", "driver->build_request(input);"),
        ]:
            with self.subTest(relative=relative, source=source):
                path = self.source(relative, source)
                with self.assertRaisesRegex(ValueError, "IR bypasses"):
                    checker.check(self.root)
                path.unlink()

    def test_native_shapes_refused_in_migrated_c_and_go_consumers(self):
        for relative in ["src/server/session_compact.c", "server-go/modules/economizer/new.go",
                         "server-go/modules/memory/new.go"]:
            for field in ["tool_calls", "function_call", "tool_call_id", "reasoning_content"]:
                path = self.source(relative, f'lookup(message, "{field}");')
                with self.assertRaisesRegex(ValueError, "native-history-field"):
                    checker.check(self.root)
                path.unlink()

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
