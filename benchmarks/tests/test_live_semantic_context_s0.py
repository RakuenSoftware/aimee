import hashlib
import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / "benchmarks" / "live-semantic-context"


def tree_hash(path: Path) -> str:
    digest = hashlib.sha256()
    for child in sorted(item for item in path.rglob("*") if item.is_file()):
        digest.update(child.relative_to(path).as_posix().encode())
        digest.update(b"\0")
        digest.update(child.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


class LiveSemanticContextS0Test(unittest.TestCase):
    def setUp(self) -> None:
        self.observation = json.loads((BASE / "s0-baseline-observation.json").read_text())
        self.contract = json.loads((BASE / "s1-experiment-contract.json").read_text())

    def test_real_provider_observation_is_pinned_and_reproducible(self) -> None:
        providers = {item["name"]: item for item in self.observation["providers"]}
        self.assertEqual(set(providers), {"gopls", "pyright"})
        self.assertEqual(providers["gopls"]["version"], "v0.20.0")
        self.assertEqual(providers["pyright"]["version"], "1.1.413")
        for name, fixture in (("gopls", "go"), ("pyright", "python")):
            self.assertEqual(
                providers[name]["fixture_sha256"], tree_hash(BASE / "fixtures" / fixture)
            )
            self.assertRegex(providers[name]["binary_sha256"], r"^[0-9a-f]{64}$")
            self.assertGreater(providers[name]["peak_process_tree_count"], 1)
            self.assertGreater(providers[name]["peak_process_tree_rss_kib"], 0)
        self.assertGreaterEqual(providers["gopls"]["reference_count"], 3)
        self.assertIn(providers["pyright"]["reference_count"], (0, 3))

    def test_red_baseline_cannot_be_relabelled_ready(self) -> None:
        providers = {item["name"]: item for item in self.observation["providers"]}
        self.assertEqual(providers["gopls"]["classification"], "ready")
        self.assertEqual(providers["gopls"]["cold_diagnostics"], 0)
        self.assertEqual(providers["gopls"]["cold_active_servers"], 0)
        self.assertEqual(
            providers["pyright"]["classification"],
            "location_link_unsupported_unsynchronized",
        )
        self.assertEqual(providers["pyright"]["expected_reference_counts_before_document_sync"], [0, 3])
        self.assertFalse(providers["pyright"]["definition_matched"])
        expected_gaps = {
            "cold_diagnostics_false_empty",
            "absolute_workspace_is_caller_selected",
            "post_write_freshness_is_unproved",
            "windows_live_provider_is_unsupported",
            "crash_and_timeout_share_text_shaped_failure",
            "detached_worktree_authority_is_unproved",
            "location_link_is_not_parsed",
        }
        self.assertEqual(set(self.observation["known_red_baseline"]), expected_gaps)
        self.assertTrue(all(self.observation["known_red_baseline"].values()))

    def test_s1_stays_closed_until_the_comparison_is_fully_pinned(self) -> None:
        self.assertEqual(self.contract["state"], "preregistered-design-incomplete")
        self.assertFalse(self.contract["candidate_implementation_allowed"])
        self.assertEqual(
            [arm["id"] for arm in self.contract["arms"]],
            ["production", "location_only", "batched_context"],
        )
        self.assertGreaterEqual(self.contract["task_contract"]["semantic_eligible_minimum"], 30)
        self.assertGreaterEqual(self.contract["task_contract"]["control_minimum"], 15)
        self.assertTrue(self.contract["unresolved_pins"])
        self.assertEqual(self.contract["promotion"]["authority_isolation_failures"], 0)
        self.assertEqual(self.contract["promotion"]["false_current_results"], 0)
        self.assertEqual(self.contract["promotion"]["false_ok_empty_failures"], 0)

    def test_real_provider_job_blocks_the_protected_unit_aggregate(self) -> None:
        workflow = (ROOT / ".github" / "workflows" / "ci.yml").read_text()
        self.assertIn("lsp-real-providers:", workflow)
        self.assertIn("--assert-baseline", workflow)
        self.assertIn("LSP_REAL_RESULT: ${{ needs.lsp-real-providers.result }}", workflow)
        self.assertIn('[ "$LSP_REAL_RESULT" = success ]', workflow)


if __name__ == "__main__":
    unittest.main()
