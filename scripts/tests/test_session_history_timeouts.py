#!/usr/bin/env python3
"""Every session history operation gets the history budget on both sides.

A delegate's history checkpoint is one write of its whole transcript. The C
client sent the IR and tool-source history saves with the ordinary 2-second
sessions budget, so a long run finished its work and then failed with
"canonical run checkpoint failed" (live 1.1.2, job 77: 76 turns, the save hit
deadline_exceeded). The store side gave one of them 30 seconds and the other
its default. Both sides must name every *_HISTORY_* operation as a history call.
"""

from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
C_CLIENT = ROOT / "src/db1_client/sessions.c"
GO_FAMILY = ROOT / "server-go/modules/aimee/families/sessions.go"


class SessionHistoryTimeouts(unittest.TestCase):
    def test_c_client_sends_history_ops_with_history_budget(self):
        src = C_CLIENT.read_text()
        switch = re.search(r"sessions_call_timeout_ms\(uint32_t op\)\s*\{(.*?)\n\}", src, re.S)
        self.assertIsNotNone(switch, "sessions_call_timeout_ms not found")
        long_ops = set(re.findall(r"case (AIMEE_DB1_OP_SESSION_\w+):", switch.group(1)))
        calls = re.findall(r"db1_call\(\s*&(\w+)\s*,\s*(AIMEE_DB1_OP_SESSION_\w*HISTORY\w*)", src)
        self.assertTrue(calls, "no history db1_call found; the scan is broken")
        for family, op in calls:
            with self.subTest(op=op):
                self.assertEqual(family, "history_family", f"{op} is sent with the 2 s budget")
        history_ops = {op for _, op in calls} | set(
            re.findall(r"primary_read\(\s*(AIMEE_DB1_OP_SESSION_\w*HISTORY\w*)", src))
        for op in sorted(history_ops):
            with self.subTest(op=op):
                self.assertIn(op, long_ops, f"{op} is missing from sessions_call_timeout_ms")

    def test_store_gives_history_ops_the_canonical_budget(self):
        src = GO_FAMILY.read_text()
        rows = re.findall(r'\{Name: "(session_\w*history\w*)",([^}]*)\}', src)
        self.assertTrue(rows, "no history op found; the scan is broken")
        for name, rest in rows:
            with self.subTest(op=name):
                self.assertIn("Timeout: store.CanonicalHistoryTimeout", rest,
                              f"{name} runs with the default store budget")


if __name__ == "__main__":
    unittest.main()
