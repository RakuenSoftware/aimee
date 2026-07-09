#!/usr/bin/env python3
"""Unit tests for swebench_live_transport — the live delegate transport's pure surface.

No live server: argv construction, status/dispatch JSON parsing, and the full dispatch->poll
loop driven by an INJECTED fake fleet runner (so the multi-turn control flow is exercised in CI).
Run: python3 -m unittest benchmarks.tests.test_swebench_live_transport
"""
from __future__ import annotations

import sqlite3
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from benchmarks.coding import swebench_live_transport as T
from benchmarks.coding import swebench_live_attribution as LA


class TestArgv(unittest.TestCase):
    def test_tools_requires_worktree(self):
        with self.assertRaises(ValueError):
            T.build_delegate_argv("code", "fix it", via="GLM-5.2", tools=True)

    def test_tools_with_worktree_ok(self):
        argv = T.build_delegate_argv("code", "fix it", via="GLM-5.2", tools=True,
                                     worktree="aimee/wi/x", persona="engineer")
        self.assertIn("--tools", argv)
        self.assertEqual(argv[argv.index("--worktree") + 1], "aimee/wi/x")
        self.assertEqual(argv[argv.index("--via") + 1], "GLM-5.2")
        self.assertEqual(argv[:4], ["aimee", "delegate", "code", "fix it"])

    def test_supervisor_no_tools_no_worktree(self):
        # Arm-C supervisor: NO tools -> no worktree required (structural "no raw code").
        argv = T.build_delegate_argv("review", "pick best", via="codex", tools=False)
        self.assertNotIn("--tools", argv)
        self.assertNotIn("--worktree", argv)

    def test_durable_and_json_default(self):
        argv = T.build_delegate_argv("explain", "hi")
        self.assertIn("--json", argv)
        self.assertIn("--durable", argv)


class TestParsing(unittest.TestCase):
    def test_parse_status_done(self):
        s = ('{"job_id":26,"job_status":"done","role":"explain","agent_name":"GLM-5.2",'
             '"result":"PONG","api_call_count":3}')
        st = T.parse_status(s)
        self.assertEqual(st.job_id, 26)
        self.assertTrue(st.terminal)
        self.assertTrue(st.ok)
        self.assertEqual(st.result, "PONG")
        self.assertEqual(st.api_calls, 3)

    def test_parse_status_failed_is_terminal_not_ok(self):
        st = T.parse_status('{"job_id":1,"job_status":"failed","result":"needs persona"}')
        self.assertTrue(st.terminal)
        self.assertFalse(st.ok)

    def test_parse_status_pending(self):
        st = T.parse_status('{"job_id":1,"job_status":"pending"}')
        self.assertFalse(st.terminal)

    def test_parse_dispatch(self):
        self.assertEqual(T.parse_dispatch('{"job_id":25,"job_status":"pending"}'), 25)

    def test_json_with_leading_log_line(self):
        st = T.parse_status('INFO some log\n{"job_id":9,"job_status":"done","result":"x"}\n')
        self.assertEqual(st.job_id, 9)
        self.assertEqual(st.result, "x")

    def test_garbage_is_empty(self):
        self.assertEqual(T.parse_dispatch("not json at all"), None)


class _FakeFleet:
    """A scripted fleet: the dispatch returns a job_id, then N pending polls, then a terminal."""
    def __init__(self, job_id=42, pending=2, status="done", result="diff --git a/x b/x\n+ok\n",
                 agent="GLM-5.2"):
        self.job_id, self.pending, self.status, self.result, self.agent = \
            job_id, pending, status, result, agent
        self._polls = 0
        self.dispatched_argv = None

    def __call__(self, argv):
        if argv[2] == "status":
            self._polls += 1
            if self._polls <= self.pending:
                return T.CompletedRun(0, f'{{"job_id":{self.job_id},"job_status":"running"}}')
            return T.CompletedRun(0, f'{{"job_id":{self.job_id},"job_status":"{self.status}",'
                                     f'"agent_name":"{self.agent}","result":{_j(self.result)},'
                                     f'"api_call_count":4}}')
        self.dispatched_argv = argv
        return T.CompletedRun(0, f'{{"job_id":{self.job_id},"job_status":"pending"}}')


def _j(s):
    import json
    return json.dumps(s)


class TestDispatchLoop(unittest.TestCase):
    def test_dispatch_and_wait_success(self):
        fleet = _FakeFleet(pending=2, status="done")
        out = T.dispatch_and_wait("code", "fix", runner=fleet, sleep=lambda *_: None,
                                  via="GLM-5.2", tools=True, worktree="aimee/wi/x")
        self.assertTrue(out.ok)
        self.assertEqual(out.job_id, 42)
        self.assertEqual(out.polls, 3)               # 2 running + 1 terminal
        self.assertIn("+ok", out.result)
        self.assertEqual(out.agent_name, "GLM-5.2")

    def test_dispatch_failure_recorded_not_raised(self):
        fleet = _FakeFleet(status="failed", result="no persona")
        out = T.dispatch_and_wait("code", "fix", runner=fleet, sleep=lambda *_: None,
                                  via="x", tools=True, worktree="w")
        self.assertFalse(out.ok)
        self.assertEqual(out.status, "failed")

    def test_no_job_id_is_error(self):
        out = T.dispatch_and_wait("code", "fix", runner=lambda a: T.CompletedRun(1, "boom", "err"),
                                  sleep=lambda *_: None, via="x", tools=True, worktree="w")
        self.assertFalse(out.ok)
        self.assertEqual(out.status, "error")

    def test_poll_budget_exhausted(self):
        fleet = _FakeFleet(pending=999)
        out = T.dispatch_and_wait("code", "fix", runner=fleet, sleep=lambda *_: None, max_polls=3,
                                  via="x", tools=True, worktree="w")
        self.assertFalse(out.ok)
        self.assertIn("budget", out.error)


class TestDelegationCaptureAndSplit(unittest.TestCase):
    def _db(self):
        con = sqlite3.connect(":memory:")
        con.execute(f"CREATE TABLE {LA.SPAWNS}(id INTEGER PRIMARY KEY, delegation_id TEXT, "
                    "parent_delegation_id TEXT, session_id TEXT, depth INT, role TEXT)")
        con.execute(f"CREATE TABLE {LA.AUDIT}(id INTEGER PRIMARY KEY, delegation_id TEXT, "
                    "usage_kind TEXT, prompt_tokens INT, completion_tokens INT, "
                    "cache_read_tokens INT, cache_write_tokens INT, tool_name TEXT)")
        return con

    def test_read_split_partitions_by_delegation_id(self):
        con = self._db()
        con.execute(f"INSERT INTO {LA.AUDIT} VALUES(1,'p','realized',1000,200,300,0,'')")
        con.execute(f"INSERT INTO {LA.AUDIT} VALUES(2,'w1','realized',5000,80,0,0,'')")
        con.execute(f"INSERT INTO {LA.AUDIT} VALUES(3,'p','avoided',9999,9999,0,0,'')")  # excluded
        con.commit()
        import tempfile, os
        fd, path = tempfile.mkstemp(suffix=".db")
        os.close(fd)
        # Re-open on-disk so read_split (which opens by path) sees the rows.
        disk = sqlite3.connect(path)
        con.backup(disk)
        disk.close()
        split = T.read_split(path, "p", ["w1"])
        os.unlink(path)
        self.assertEqual(split["primary_input_uncached"], 700)   # 1000 - 300 cache_read
        self.assertEqual(split["primary_output"], 200)
        self.assertEqual(split["worker_output"], 80)

    def test_exact_delegation_read_ignores_lookalike_ids(self):
        # Exact delegation_id matching must NOT pull in a different id that merely shares a suffix
        # (the fragility the LIKE '%-<job_id>' heuristic risks).
        con = self._db()
        con.execute(f"INSERT INTO {LA.AUDIT} VALUES(1,'deleg-a-b-3','realized',1000,100,200,0,'')")
        con.execute(f"INSERT INTO {LA.AUDIT} VALUES(2,'deleg-a-b-13','realized',9999,9999,0,0,'')")
        con.commit()
        import tempfile, os
        fd, path = tempfile.mkstemp(suffix=".db"); os.close(fd)
        disk = sqlite3.connect(path); con.backup(disk); disk.close()
        uncached, cached, output, rows = T.read_realized_by_delegations(path, ["deleg-a-b-3"])
        os.unlink(path)
        self.assertEqual(rows, 1)                    # only the exact id, not deleg-a-b-13
        self.assertEqual(uncached, 800)              # 1000 - 200 cache_read
        self.assertEqual(output, 100)

    def test_supervisor_tool_call_rows_zero_when_no_tools(self):
        con = self._db()
        con.execute(f"INSERT INTO {LA.AUDIT} VALUES(1,'sup','realized',100,20,0,0,'')")
        con.commit()
        import tempfile, os
        fd, path = tempfile.mkstemp(suffix=".db"); os.close(fd)
        disk = sqlite3.connect(path); con.backup(disk); disk.close()
        self.assertEqual(T.supervisor_tool_call_rows(path, "sup"), 0)
        os.unlink(path)


if __name__ == "__main__":
    unittest.main()
