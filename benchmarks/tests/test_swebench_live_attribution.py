#!/usr/bin/env python3
"""Unit tests for S0-live token attribution.

Runs the real attribution SQL against an in-memory sqlite fixture that mirrors the .254
`delegation_spawns` + `token_audit` schema (columns verified on the live DB). Proves the
corrected primary-vs-worker split, the realized-only filter, and the disjointness assertions.
Run: python3 -m unittest benchmarks.tests.test_swebench_live_attribution
"""
from __future__ import annotations

import sqlite3
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from benchmarks.coding import swebench_live_attribution as A


def _fixture() -> sqlite3.Connection:
    con = sqlite3.connect(":memory:")
    con.execute("CREATE TABLE delegation_spawns (id INTEGER PRIMARY KEY, delegation_id TEXT, "
                "parent_delegation_id TEXT, session_id TEXT, depth INT, role TEXT)")
    con.execute("CREATE TABLE token_audit (id INTEGER PRIMARY KEY, delegation_id TEXT, "
                "usage_kind TEXT, prompt_tokens INT, completion_tokens INT, "
                "cache_read_tokens INT, cache_write_tokens INT)")
    S = "sess-1"
    # primary dispatch (depth-1) + two worker dispatches (depth-1 siblings today)
    con.executemany("INSERT INTO delegation_spawns VALUES (?,?,?,?,?,?)", [
        (100, "deleg-P", "", S, 1, "execute"),
        (101, "deleg-W1", "", S, 1, "code"),
        (102, "deleg-W2", "", S, 1, "code"),
    ])
    con.executemany("INSERT INTO token_audit VALUES (?,?,?,?,?,?,?)", [
        # primary: 1 realized (1000 prompt, 200 cache_read -> 800 uncached, 100 out) + 1 avoided
        (1, "deleg-P", "realized", 1000, 100, 200, 0),
        (2, "deleg-P", "avoided", 5000, 0, 0, 0),
        # workers: big realized returns (priced $0 in reporting)
        (3, "deleg-W1", "realized", 40000, 3000, 0, 0),
        (4, "deleg-W2", "realized", 50000, 4000, 0, 0),
        (5, "deleg-W2", "avoided", 9999, 0, 0, 0),
    ])
    con.commit()
    return con


class TestCapture(unittest.TestCase):
    def test_capture_newest_spawn_after_marker(self):
        con = _fixture()
        # dispatched after id 100 -> newest is W2 (id 102)
        self.assertEqual(A.capture_dispatch_delegation(con, "sess-1", 100), "deleg-W2")

    def test_none_when_no_new_spawn(self):
        con = _fixture()
        self.assertIsNone(A.capture_dispatch_delegation(con, "sess-1", 999))


class TestRealizedTotals(unittest.TestCase):
    def test_uncached_headline_and_realized_only(self):
        con = _fixture()
        t = A.realized_totals(con, ["deleg-P"])
        self.assertEqual(t.input_uncached, 800)   # 1000 - 200 cache_read
        self.assertEqual(t.input_cached, 200)
        self.assertEqual(t.output, 100)
        self.assertEqual(t.rows, 1)                # the avoided row excluded
        self.assertEqual(t.total_headline, 900)

    def test_empty_delegation_set(self):
        con = _fixture()
        self.assertEqual(A.realized_totals(con, []).rows, 0)


class TestPrimaryWorkerSplit(unittest.TestCase):
    def test_split_is_by_delegation_ownership(self):
        con = _fixture()
        s = A.primary_worker_split(con, "deleg-P", ["deleg-W1", "deleg-W2"])
        self.assertEqual(s["primary_headline"], 900)     # cheap primary
        self.assertEqual(s["worker_output"], 7000)        # 3000 + 4000
        self.assertEqual(s["worker_input"], 90000)        # 40000 + 50000, avoided excluded
        self.assertEqual(s["primary_rows"], 1)

    def test_child_delegations_when_tree_exists(self):
        con = _fixture()
        # promote W1 to a child of P
        con.execute("UPDATE delegation_spawns SET parent_delegation_id='deleg-P', depth=2 "
                    "WHERE delegation_id='deleg-W1'")
        self.assertEqual(A.child_delegations(con, "deleg-P"), ["deleg-W1"])


class TestVerifyLiveAttribution(unittest.TestCase):
    def test_passes_on_clean_split(self):
        con = _fixture()
        v = A.verify_live_attribution(con, "deleg-P", ["deleg-W1", "deleg-W2"])
        self.assertTrue(v["passed"], v)
        self.assertTrue(v["L1_primary_measured"]["ok"])

    def test_cross_bill_detected(self):
        con = _fixture()
        # a worker id equal to the primary id would be cross-billing
        v = A.verify_live_attribution(con, "deleg-P", ["deleg-P"])
        self.assertFalse(v["L2_worker_disjoint"]["ok"])
        self.assertFalse(v["passed"])


class TestSplitByJobs(unittest.TestCase):
    def _db(self):
        con = sqlite3.connect(":memory:")
        con.execute(f"CREATE TABLE {A.AUDIT}(delegation_id TEXT, usage_kind TEXT, prompt_tokens INT,"
                    " completion_tokens INT, cache_read_tokens INT)")
        return con

    def test_split_partitions_by_trailing_job_id(self):
        con = self._db()
        con.execute(f"INSERT INTO {A.AUDIT} VALUES('deleg-1-2-7','realized',1000,100,200)")
        con.execute(f"INSERT INTO {A.AUDIT} VALUES('deleg-9-9-8','realized',5000,40,0)")
        con.execute(f"INSERT INTO {A.AUDIT} VALUES('deleg-1-2-7','avoided',9999,9999,0)")  # excluded
        con.commit()
        s = A.split_by_jobs(con, 7, [8])
        self.assertEqual(s["primary_input_uncached"], 800)   # 1000 - 200 cache_read
        self.assertEqual(s["primary_output"], 100)
        self.assertEqual(s["worker_output"], 40)


class TestRunLiveMatrix(unittest.TestCase):
    """Drive the S0-live runner with a fake fleet + an on-disk fixture DB: assert L1-L4 pass when
    the primary and worker land distinct realized rows keyed by job_id."""

    def test_passes_on_disjoint_jobs(self):
        import tempfile, os
        from benchmarks.coding import swebench_live_transport as LT
        fd, path = tempfile.mkstemp(suffix=".db"); os.close(fd)
        con = sqlite3.connect(path)
        con.execute(f"CREATE TABLE {A.AUDIT}(delegation_id TEXT, usage_kind TEXT, prompt_tokens INT,"
                    " completion_tokens INT, cache_read_tokens INT)")
        con.execute(f"INSERT INTO {A.AUDIT} VALUES('deleg-a-b-31','realized',800,90,100)")   # primary
        con.execute(f"INSERT INTO {A.AUDIT} VALUES('deleg-a-b-32','realized',400,20,0)")     # worker
        con.commit(); con.close()

        jobs = iter([31, 32])
        def fake_dispatch(role, prompt, **kw):
            jid = next(jobs)
            return LT.DispatchOutcome(jid, "done", "```diff\n+x\n```", kw.get("via"),
                                      f"deleg-a-b-{jid}", 2, 1)
        res = A.run_live_matrix(db_path=path, primary_agent="codex", worker="GLM-5.2",
                                dispatch=fake_dispatch)
        os.unlink(path)
        self.assertTrue(res["passed"], res)
        self.assertEqual(res["primary_job_id"], 31)
        self.assertEqual(res["split"]["primary_input_uncached"], 700)

    def test_reports_error_when_db_missing(self):
        from benchmarks.coding import swebench_live_transport as LT
        fake = lambda role, prompt, **kw: LT.DispatchOutcome(1, "done", "", kw.get("via"),
                                                            "d-1", 1, 1)
        res = A.run_live_matrix(db_path="/no/such.db", dispatch=fake)
        self.assertFalse(res["passed"])
        self.assertIn("not readable", res["error"])


if __name__ == "__main__":
    unittest.main()
