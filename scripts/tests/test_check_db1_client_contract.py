#!/usr/bin/env python3
"""Named timeout descriptors must retain catalog contract coverage."""

import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


CHECKER = Path(__file__).resolve().parents[1] / "check-db1-client-contract.py"
SPEC = importlib.util.spec_from_file_location("db1_client_contract", CHECKER)
assert SPEC and SPEC.loader
checker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checker)


class NamedDescriptorContractTests(unittest.TestCase):
    def check(self, descriptor, request, reply, known=True):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "client.c").write_text(
                f"db1_call(&{descriptor}, AIMEE_DB1_OP_PROBE, fields, "
                f"{request}, values, caps, {reply}, NULL);\n"
            )
            catalog = {"probe": {"request": 1, "reply": 0,
                                  "family": "sessions", "variadic": False}} if known else {}
            output = io.StringIO()
            with patch.object(checker, "CLIENT_DIR", root), \
                    patch.object(checker, "MODULE_API", root / "absent.h"), \
                    patch.object(checker, "load_catalog", return_value=(catalog, {})), \
                    contextlib.redirect_stdout(output):
                result = checker.main()
            return result, output.getvalue()

    def test_named_descriptors_are_checked(self):
        for descriptor in ("family", "history_family", "source_family", "bounded_read"):
            with self.subTest(descriptor=descriptor):
                result, output = self.check(descriptor, 1, 0)
                self.assertEqual(result, 0)
                self.assertIn("1 call sites", output)

    def test_wrong_request_and_reply_cannot_hide_behind_descriptor_name(self):
        for descriptor in ("family", "history_family", "source_family"):
            for request, reply in ((2, 0), (1, 1)):
                with self.subTest(descriptor=descriptor, request=request, reply=reply):
                    result, output = self.check(descriptor, request, reply)
                    self.assertEqual(result, 1, output)

    def test_unknown_operation_is_rejected_for_named_descriptor(self):
        result, output = self.check("source_family", 1, 0, known=False)
        self.assertEqual(result, 1, output)
        self.assertIn("not in the catalog", output)


class OwnedSessionContractTests(unittest.TestCase):
    def test_owned_request_and_reply_shape_remain_catalog_checked(self):
        for request, reply, expected in ((1, 10, 0), (2, 10, 1), (1, 11, 1)):
            with self.subTest(request=request, reply=reply), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                (root / "sessions.c").write_text(
                    f"session_record_read(AIMEE_DB1_OP_SERVER_SESSION_GET, fields, "
                    f"{request}, {reply}, rows, 1, 1);\n")
                catalog = {"server_session_get": {"request": 1, "reply": 10,
                              "family": "sessions", "variadic": False}}
                with patch.object(checker, "CLIENT_DIR", root), \
                        patch.object(checker, "MODULE_API", root / "absent.h"), \
                        patch.object(checker, "load_catalog", return_value=(catalog, {})), \
                        contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(checker.main(), expected)


if __name__ == "__main__":
    unittest.main()
