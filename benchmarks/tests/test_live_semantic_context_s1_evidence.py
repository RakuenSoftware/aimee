from __future__ import annotations

import hashlib
import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / "benchmarks" / "results" / "live-semantic-context" / "s1-epoch3"
CANDIDATE = "474bd69954237fca249eb44e942caeab4270ad5e"
CANDIDATE_TREE = "e6ba59ceba5a40323b006e834ac28ef39a2abc46"
RUNNER = "682bd5c55ff4ba6b4b131de7debef2af5e906321"
CONTRACT_SHA256 = "345610b19bad3644260d731dea1ffd225971d74b57c26f041a2d55f2cda1bee1"
MANIFEST_SHA256 = "612e64f133ed338c30fff51d52f10823dc22ee3fa5905894a1d38b087fa5dae1"
TREE_HASHES = {
    "paired": "78c89fd52214679ed179a582b253bce257ccdc9970e73cadb3c0cf5a852a8b03",
    "adversarial": "c0b4f30eeb092b3632c84310d118377ce1df9fdef5f0d5db802b1713f1e85b57",
    "providers": "619bbe11d9d1b23b8452f64f52bbb6d4b3a78d29b2ff7e9bfb2974b7149c1703",
}


def tree_hash(path: Path) -> str:
    digest = hashlib.sha256()
    for child in sorted(item for item in path.rglob("*") if item.is_file()):
        digest.update(child.relative_to(path).as_posix().encode())
        digest.update(b"\0")
        digest.update(child.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


class LiveSemanticContextS1EvidenceTest(unittest.TestCase):
    def test_every_published_evidence_byte_is_pinned(self) -> None:
        self.assertEqual(
            {name: tree_hash(BASE / name) for name in TREE_HASHES}, TREE_HASHES
        )

    def test_paired_value_artifact_is_complete_and_material(self) -> None:
        results = json.loads((BASE / "paired" / "results.json").read_text())
        summary = json.loads((BASE / "paired" / "summary.json").read_text())
        self.assertEqual(results["claim_status"], "complete")
        self.assertEqual(len(results["cells"]), 135)
        self.assertEqual(len(list((BASE / "paired" / "raw").glob("*.jsonl"))), 135)
        self.assertEqual(len(list((BASE / "paired" / "tools").glob("*.jsonl"))), 135)
        self.assertFalse(any(cell["infrastructure_failure"] for cell in results["cells"]))
        self.assertTrue(all(cell["cell_eligible"] for cell in results["cells"]))
        self.assertEqual(results["contract_sha256"], CONTRACT_SHA256)
        self.assertEqual(results["manifest_sha256"], MANIFEST_SHA256)
        self.assertEqual(results["lineage"]["candidate_commit"], CANDIDATE)
        self.assertEqual(results["lineage"]["candidate_src_tree"], CANDIDATE_TREE)
        self.assertEqual(results["lineage"]["runner_commit"], RUNNER)

        semantic = summary["paired"]["semantic"]
        self.assertEqual(semantic["complete_pairs"], 30)
        self.assertGreaterEqual(semantic["median_tool_call_reduction"]["lower_95"], 0.50)
        self.assertGreater(semantic["median_wall_time_reduction"]["lower_95"], 0.33)
        self.assertTrue(summary["material_value_gate"]["passes_either_choice"])
        self.assertEqual(summary["candidate_adoption"], {
            "denominator": 30, "rate": 29 / 30,
        })
        self.assertEqual(summary["arm_metrics"]["batched_context"]["task_success_rate"], 1.0)
        self.assertEqual(summary["arm_metrics"]["production"]["task_success_rate"], 44 / 45)
        control = summary["paired"]["control"]
        self.assertEqual(control["task_success_absolute_delta"]["estimate"], 0)
        self.assertEqual(control["median_tool_call_reduction"]["estimate"], 0)

    def test_adversarial_and_cross_platform_gates_are_complete(self) -> None:
        results = json.loads((BASE / "adversarial" / "results.json").read_text())
        summary = json.loads((BASE / "adversarial" / "summary.json").read_text())
        self.assertEqual(results["claim_status"], "complete")
        self.assertEqual(len(results["cells"]), 12)
        self.assertFalse(any(cell["infrastructure_failure"] for cell in results["cells"]))
        self.assertTrue(all(cell["cell_eligible"] for cell in results["cells"]))
        self.assertEqual(results["lineage"]["candidate_commit"], CANDIDATE)
        self.assertEqual(results["lineage"]["runner_commit"], RUNNER)
        self.assertEqual(summary["safety"], {
            "authority_citation_failures": 0,
            "false_current_results": 0,
            "false_ok_empty_count": 0,
            "typed_failure_denominator": 12,
            "typed_failure_preservation_rate": 1.0,
        })

        providers = [
            json.loads(path.read_text()) for path in sorted((BASE / "providers").glob("*.json"))
        ]
        self.assertEqual(len(providers), 2)
        for report in providers:
            self.assertTrue(report["candidate_matched"])
            self.assertEqual(report["candidate_commit"], CANDIDATE)
            self.assertEqual(report["candidate_src_tree"], CANDIDATE_TREE)
            for provider in report["providers"]:
                self.assertEqual(provider["cold_start_attempts"], 20)
                self.assertEqual(provider["cold_start_successes"], 20)
                self.assertEqual(provider["reference_recall"], 1.0)
                self.assertEqual(provider["reference_false_positive_rate"], 0.0)


if __name__ == "__main__":
    unittest.main()
