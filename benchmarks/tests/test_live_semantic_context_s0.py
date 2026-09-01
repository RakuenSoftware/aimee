import hashlib
import json
from pathlib import Path
import subprocess
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

    def test_s1_comparison_is_fully_pinned_before_candidate_code(self) -> None:
        self.assertEqual(self.contract["state"], "candidate-pinned")
        self.assertTrue(self.contract["candidate_implementation_allowed"])
        self.assertEqual(
            [arm["id"] for arm in self.contract["arms"]],
            ["production", "location_only", "batched_context"],
        )
        self.assertGreaterEqual(self.contract["task_contract"]["semantic_eligible_minimum"], 30)
        self.assertGreaterEqual(self.contract["task_contract"]["control_minimum"], 15)
        self.assertEqual(self.contract["unresolved_pins"], [])
        evidence = self.contract["s0_gate_evidence"]
        self.assertEqual(evidence["pull_request"], 2950)
        self.assertEqual(evidence["head_commit"], "a415b76208245612fbfe58b5d695285b3e2b5ee3")
        self.assertEqual(evidence["workflow_run"], 33520936934)
        self.assertEqual(evidence["macos_job"], 99900203992)
        self.assertEqual(evidence["macos_job_conclusion"], "success")
        self.assertTrue(evidence["macos_observation_baseline_matched"])
        self.assertRegex(evidence["macos_observation_artifact_digest"], r"^sha256:[0-9a-f]{64}$")
        self.assertEqual(self.contract["promotion"]["authority_isolation_failures"], 0)
        self.assertEqual(self.contract["promotion"]["false_current_results"], 0)
        self.assertEqual(self.contract["promotion"]["false_ok_empty_failures"], 0)

    def test_s1_corpus_and_model_visible_bytes_are_checked(self) -> None:
        manifest = json.loads((BASE / "s1-task-manifest.json").read_text())
        self.assertEqual(manifest["counts"], {
            "total": 45, "semantic_eligible": 30, "controls": 15,
        })
        self.assertEqual(len({task["id"] for task in manifest["tasks"]}), 45)
        pins = self.contract["content_pins"]
        for key, relative in (
            ("task_manifest", "s1-task-manifest.json"),
            ("agent_system_prompt", "prompts/s1-agent-system-v1.md"),
            ("tool_schemas", "tools/s1-tool-schemas-v1.json"),
        ):
            self.assertEqual(pins[key], relative)
            self.assertEqual(
                pins[f"{key}_sha256"], hashlib.sha256((BASE / relative).read_bytes()).hexdigest()
            )
        completed = subprocess.run(
            ["python3", str(BASE / "validate_s1_contract.py")],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_model_execution_is_exact_and_candidate_commit_is_a_run_pin(self) -> None:
        model = self.contract["model_execution_pin"]
        self.assertEqual(model["model_identifier"], "gpt-5.6-sol")
        self.assertEqual(model["reasoning"], "medium")
        self.assertEqual(model["client_version"], "0.151.0")
        self.assertRegex(model["linux_x86_64_executable_sha256"], r"^[0-9a-f]{64}$")
        self.assertEqual(
            self.contract["candidate_commit_pin"]["timing"],
            "before the first epoch-2 promotion cell, after the retained calibration exposed and verified the cross-file defect",
        )
        self.assertEqual(
            self.contract["candidate_commit_pin"]["commit"],
            "795631825e13e070b2d5d3061a3248b493f2b75b",
        )
        self.assertEqual(
            self.contract["candidate_commit_pin"]["runtime_src_tree"],
            "bd04b26c677a07f00fea043759a3a82f9625753d",
        )
        workflow = (ROOT / ".github" / "workflows" / "ci.yml").read_text()
        self.assertIn(
            "--candidate-commit 795631825e13e070b2d5d3061a3248b493f2b75b",
            workflow,
        )
        self.assertIn(
            "--candidate-src-tree bd04b26c677a07f00fea043759a3a82f9625753d",
            workflow,
        )

    def test_real_provider_job_blocks_the_protected_unit_aggregate(self) -> None:
        workflow = (ROOT / ".github" / "workflows" / "ci.yml").read_text()
        self.assertIn("lsp-real-providers:", workflow)
        self.assertIn("- os: macos-latest", workflow)
        self.assertIn("runs-on: ${{ matrix.os }}", workflow)
        self.assertIn("--assert-baseline", workflow)
        self.assertIn("run_s1_candidate_probe.py", workflow)
        self.assertIn("--assert-candidate", workflow)
        self.assertIn("lsp-s1-real-provider-${{ matrix.artifact }}-candidate", workflow)
        self.assertIn("LSP_REAL_RESULT: ${{ needs.lsp-real-providers.result }}", workflow)
        self.assertIn('[ "$LSP_REAL_RESULT" = success ]', workflow)


if __name__ == "__main__":
    unittest.main()
