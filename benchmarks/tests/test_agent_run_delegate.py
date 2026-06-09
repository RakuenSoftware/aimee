#!/usr/bin/env python3
"""Integration test for the real (non-fake) delegate path in AimeeHarness.

Exercises the full queue -> poll jobs.status -> parse loop and the
judge_majority verdict logic against a *stub* aimee client (a tiny script that
mimics `delegate execute --background` and `jobs status`). No real model, GPU,
or judge is required — this is the deterministic CI proof that the
delegate-as-judge wiring is correct.
"""

from __future__ import annotations

import os
import stat
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from benchmarks.common.harness import AimeeHarness
from benchmarks.common.llm_eval import JUDGE_SYSTEM, judge_majority

# A stub that stands in for the `aimee` client. `delegate execute` records the
# prompt and returns a pending job; `jobs status` returns a succeeded job whose
# `result` is computed from the recorded prompt (judge prompts are graded by a
# trivial substring rule; everything else echoes a canned answer).
_STUB = textwrap.dedent(
    '''\
    #!/usr/bin/env python3
    import json, os, sys
    argv = sys.argv[1:]
    state = os.environ["AIMEE_STUB_STATE"]
    pfile = os.path.join(state, "prompt.txt")

    def emit(obj):
        sys.stdout.write(json.dumps(obj))

    if "delegate" in argv and "execute" in argv:
        # The harness passes the prompt over stdin with --prompt-stdin (benchmark
        # prompts exceed the argv size limit); mirror the real client contract
        # instead of reading the positional slot, which is now the flag itself.
        prompt = sys.stdin.read() if "--prompt-stdin" in argv else argv[argv.index("execute") + 1]
        with open(pfile, "w") as fh:
            fh.write(prompt)
        emit({"job_id": 1, "job_status": "pending"})
    elif "jobs" in argv and "status" in argv:
        prompt = open(pfile).read() if os.path.exists(pfile) else ""
        if "Gold answer:" in prompt:
            gold = cand = ""
            for line in prompt.splitlines():
                if line.startswith("Gold answer: "):
                    gold = line.split(": ", 1)[1].strip().lower()
                elif line.startswith("Candidate answer: "):
                    cand = line.split(": ", 1)[1].strip().lower()
            score = 1 if gold and cand and (gold in cand or cand in gold) else 0
            result = json.dumps({"score": score})
        else:
            result = "four"
        emit({"job_id": 1, "job_status": "done",
              "job": {"status": "done", "result": result, "agent_name": "stub"}})
    else:
        emit({"error": "stub: unsupported", "argv": argv})
    '''
)


class DelegateStubTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        root = Path(self._tmp.name)
        self.state = root / "state"
        self.state.mkdir()
        self.client = root / "stub-client"
        self.client.write_text(_STUB)
        self.client.chmod(self.client.stat().st_mode | stat.S_IXUSR)
        self.home = root / "home"
        self.home.mkdir()

        self._env = {
            "AIMEE_BENCH_CLIENT": str(self.client),
            "AIMEE_BENCH_SOURCE_HOME": str(self.home),
            "AIMEE_STUB_STATE": str(self.state),
            "AIMEE_BENCH_DELEGATE_POLL": "0",
            "AIMEE_BENCH_PERSONA": "engineer",
        }
        self._saved = {k: os.environ.get(k) for k in self._env}
        os.environ.update(self._env)
        os.environ.pop("AIMEE_BENCH_FAKE_AGENT", None)
        self.harness = AimeeHarness(root=root)

    def tearDown(self) -> None:
        for k, v in self._saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        self._tmp.cleanup()

    def test_agent_run_parses_job_result(self) -> None:
        ex = self.harness.agent_run(self.home, prompt="What is 2+2?", system="answer only", max_tokens=16)
        self.assertEqual(ex.response, "four")
        self.assertEqual(ex.agent, "stub")
        self.assertGreaterEqual(ex.prompt_tokens, 1)

    def test_agent_run_without_system(self) -> None:
        # The model_only adapter calls agent_run without `system`; must not crash.
        ex = self.harness.agent_run(self.home, prompt="anything", max_tokens=16)
        self.assertEqual(ex.response, "four")

    def test_judge_majority_correct(self) -> None:
        votes, _lat, _ji, _jo, verdict = judge_majority(
            self.harness, self.home,
            question="capital?", gold_answer="Paris", candidate="The answer is Paris.",
        )
        self.assertEqual(len(votes), 3)
        self.assertEqual(votes, ["CORRECT", "CORRECT", "CORRECT"])
        self.assertEqual(verdict, "CORRECT")

    def test_judge_majority_wrong(self) -> None:
        votes, _lat, _ji, _jo, verdict = judge_majority(
            self.harness, self.home,
            question="capital?", gold_answer="Paris", candidate="London",
        )
        self.assertEqual(verdict, "WRONG")
        self.assertTrue(all(v == "WRONG" for v in votes))

    def test_failed_job_raises(self) -> None:
        # Point the stub at a state dir with no prompt file → judge branch off →
        # but force a failure by making the client emit a failed status.
        failing = Path(self._tmp.name) / "failing-client"
        failing.write_text(
            "#!/usr/bin/env python3\n"
            "import json,sys\n"
            "a=sys.argv[1:]\n"
            "print(json.dumps({'job_id':1,'job_status':'pending'}) if 'delegate' in a "
            "else json.dumps({'job':{'status':'failed','result':'boom'}}))\n"
        )
        failing.chmod(failing.stat().st_mode | stat.S_IXUSR)
        os.environ["AIMEE_BENCH_CLIENT"] = str(failing)
        harness = AimeeHarness(root=Path(self._tmp.name))
        with self.assertRaises(RuntimeError):
            harness.agent_run(self.home, prompt="x", max_tokens=16)


if __name__ == "__main__":
    unittest.main()
