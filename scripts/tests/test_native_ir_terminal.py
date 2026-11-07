"""Native fixture completion follows the typed parent turn, not stream position."""
import importlib.util
from pathlib import Path
import unittest


CONTROL_PATH = Path(__file__).resolve().parents[2] / "src/tests/test_native_ir_browser_control.py"
SPEC = importlib.util.spec_from_file_location("native_browser_control", CONTROL_PATH)
CONTROL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CONTROL)


def record(kind, status=None, turn="parent"):
    return {"turn_id": turn, "event": {"kind": kind, "payload": {"status": status}}}


class NativeTerminalTest(unittest.TestCase):
    def test_checkpoint_and_child_notice_after_parent_completion(self):
        CONTROL.assert_completed_parent([
            record("execution", "running"), record("message"),
            record("execution", "completed"), record("native_checkpoint"),
            record("child_execution", "running", "child"),
        ])

    def test_parent_failure_cannot_be_hidden_by_metadata_or_child_success(self):
        for status in ("failed", "cancelled"):
            with self.subTest(status=status), self.assertRaises(AssertionError):
                CONTROL.assert_completed_parent([
                    record("execution", "running"), record("execution", status),
                    record("native_checkpoint"), record("execution", "completed", "child"),
                ])

    def test_missing_or_duplicate_parent_terminal_refuses(self):
        for ending in ([], [record("execution", "completed", "child")],
                       [record("execution", "completed"), record("execution", "failed")]):
            with self.subTest(ending=ending), self.assertRaises(AssertionError):
                CONTROL.assert_completed_parent([record("execution", "running"), *ending])


if __name__ == "__main__":
    unittest.main()
