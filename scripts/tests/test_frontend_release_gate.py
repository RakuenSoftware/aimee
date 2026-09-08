#!/usr/bin/env python3
"""The browser regression lane must block the required aggregate CI status."""
import os
from pathlib import Path
import re
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]


class FrontendReleaseGateTests(unittest.TestCase):
    def test_browser_failure_blocks_required_gate(self):
        workflow = (ROOT / ".github/workflows/ci.yml").read_text()
        lane = workflow.split("  frontend-tests:\n", 1)[1].split("  go-unit-tests:\n", 1)[0]
        for command in ("npm ci", "npm test", "npm run build", "npx playwright install", "npm run test:browser"):
            self.assertIn(command, lane)
        aggregate = workflow.split("\n  unit-tests:\n", 1)[1]
        aggregate = re.split(r"\n  [a-z][a-z0-9-]*:\n", aggregate, maxsplit=1)[0]
        self.assertIn("frontend-tests", aggregate.split("\n", 1)[0])
        self.assertIn("FRONTEND_RESULT: ${{ needs.frontend-tests.result }}", aggregate)
        run = aggregate.split("        run: |\n", 1)[1]
        script = "\n".join(line[10:] for line in run.splitlines() if line.startswith("          "))
        env = dict(os.environ, CHANGE_SCOPE_RESULT="success", DOCS_ONLY="false",
                   SHARDS_RESULT="success", SHARDS_PG_RESULT="success", DB2_REPLAY_RESULT="success",
                   P1_RESULT="success", GO_RESULT="success", LSP_REAL_RESULT="success")
        for result in ("success", "failure", "cancelled", "skipped"):
            with self.subTest(result=result):
                proc = subprocess.run(["bash", "-c", script], env=dict(env, FRONTEND_RESULT=result), capture_output=True)
                self.assertEqual(proc.returncode == 0, result == "success")


if __name__ == "__main__":
    unittest.main()
