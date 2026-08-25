#!/usr/bin/env python3
"""Unit tests for the changeset WORM seal gate.

The gate's whole subject is a silent omission -- a changeset that closes with
nothing in the audit chain -- so the gate failing silently would reproduce the
defect it exists to catch. These tests hold it to three things: it reports a
close whose seal was deleted, it refuses to pass when it resolves no close
sites at all, and it does not demand a seal for a close that belongs to
somebody else's changeset.
"""

from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "check_changeset_worm_seal", ROOT / "scripts/check_changeset_worm_seal.py")
assert SPEC and SPEC.loader
CHECK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECK)

SEALED = """CREATE OR REPLACE FUNCTION example_apply(p_id TEXT) RETURNS VOID
LANGUAGE plpgsql AS $$
DECLARE cid TEXT;
BEGIN
  UPDATE fact_graph_commits SET status='applied',closed_at=now_text
    WHERE commit_id=cid;
  PERFORM kb_fact_commit_worm_seal(cid, p_id);
  PERFORM set_config('aimee.changeset_id','',true);
END $$;
"""

# The revert path marks the changeset it is reverting, then closes its own.
# Only the second one is a close of its own and only it needs a seal.
FOREIGN_CLOSE = """CREATE OR REPLACE FUNCTION example_revert(p_id TEXT) RETURNS VOID
LANGUAGE plpgsql AS $$
DECLARE cid TEXT;
BEGIN
  UPDATE fact_graph_commits SET status='reverted',rolled_back_by=actor
    WHERE commit_id=p_id;
  UPDATE fact_graph_commits SET status='applied',closed_at=now_text
    WHERE commit_id=cid;
  PERFORM kb_fact_commit_worm_seal(cid, p_id);
END $$;
"""


class ChangesetWormSealTest(unittest.TestCase):
    def write(self, text: str) -> Path:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        path = Path(temporary.name) / "schema.sql"
        path.write_text(text, encoding="utf-8")
        return path

    def test_sealed_close_passes(self) -> None:
        sites, unsealed = CHECK.audit(SEALED)
        self.assertEqual([("example_apply", 5)], sites)
        self.assertEqual([], unsealed)
        self.assertEqual(0, CHECK.main(["check", str(self.write(SEALED))]))

    def test_deleted_seal_is_reported(self) -> None:
        """The negative control: remove the real call, the gate must notice."""
        broken = "\n".join(
            line for line in SEALED.splitlines()
            if "kb_fact_commit_worm_seal(cid" not in line)
        sites, unsealed = CHECK.audit(broken)
        self.assertEqual(1, len(sites))
        self.assertEqual(sites, unsealed)
        self.assertEqual(1, CHECK.main(["check", str(self.write(broken))]))

    def test_zero_sites_is_a_failure_not_a_pass(self) -> None:
        """A gate that resolves nothing is not a gate. It must exit non-zero."""
        empty = "CREATE TABLE fact_graph_commits (commit_id TEXT);\n"
        self.assertEqual(([], []), CHECK.audit(empty))
        self.assertEqual(2, CHECK.main(["check", str(self.write(empty))]))

    def test_foreign_changeset_close_needs_no_seal_of_its_own(self) -> None:
        sites, unsealed = CHECK.audit(FOREIGN_CLOSE)
        self.assertEqual([("example_revert", 7)], sites)
        self.assertEqual([], unsealed)

    def test_self_test_mode_agrees_with_the_real_schema(self) -> None:
        text = Path(CHECK.DEFAULT_SCHEMA).read_text(encoding="utf-8")
        self.assertIsNone(CHECK.self_test(text))

    def test_production_schema_has_every_close_sealed(self) -> None:
        text = Path(CHECK.DEFAULT_SCHEMA).read_text(encoding="utf-8")
        sites, unsealed = CHECK.audit(text)
        self.assertTrue(sites, "no changeset close sites resolved in schema.sql")
        self.assertEqual([], unsealed)


if __name__ == "__main__":
    unittest.main()
