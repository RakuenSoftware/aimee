"""Contract tests for the frozen 10k Gemma baseline and resumable runners."""

from __future__ import annotations

import contextlib
import hashlib
import importlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
MODULES = ROOT / "benchmarks/gemma4_baseline"
sys.path.insert(0, str(MODULES))

builder = importlib.import_module("build_254_fixtures")
reranker = importlib.import_module("run_reranking_ab")
eurobert_controller = importlib.import_module("run_254_eurobert_rerankers")
eurobert_server = importlib.import_module("serve_cross_encoder")
eurobert_smoke = importlib.import_module("smoke_eurobert_runtime")
eurobert_trainer = importlib.import_module("train_eurobert_reranker")
comparison = importlib.import_module("compare_ab")
reranker_reports = importlib.import_module("reranker_pairwise_reports")
remaining_chain = importlib.import_module("run_254_remaining_chain")
result_validator = importlib.import_module("validate_result_checkpoint")
synthesis = importlib.import_module("run_synthesis_ab")
try:
    embedding = importlib.import_module("run_embedding_ab")
except ModuleNotFoundError as exc:
    if exc.name != "numpy":
        raise
    embedding = None
sweep = importlib.import_module("run_254_sweep")
validator = importlib.import_module("validate_fixtures")


class GemmaBaselineContractTests(unittest.TestCase):
    def test_published_checkpoints_pass_fail_closed_result_validation(self) -> None:
        bundle = ROOT / "benchmarks/fixtures/gemma4-unified/ab-v1"
        results = ROOT / "benchmarks/results/gemma4-unified/ab-v1"
        checks = (
            ("ettin68m", "reranking", 10_014),
            ("ettin400m", "reranking", 10_204),
            ("gemma4_e2b", "synthesis", 10_000),
            ("gemma4_e2b", "embedding", 10_000),
            ("gemma4_e4b", "embedding", 10_000),
            ("gemma4_12b", "synthesis", 10_013),
        )
        for label, view, raw_rows in checks:
            with self.subTest(label=label, view=view):
                evidence = result_validator.validate_checkpoint(bundle, results / label, label, view)
                self.assertEqual(evidence["latest_rows"], 10_000)
                self.assertEqual(evidence["raw_rows"], raw_rows)
                self.assertEqual(evidence["secret_scan"], "pass")
                committed = json.loads(
                    (results / label / f"validation_{view}.json").read_text(encoding="utf-8")
                )
                self.assertEqual(committed, evidence)

    def test_result_validation_evidence_is_written_atomically(self) -> None:
        bundle = ROOT / "benchmarks/fixtures/gemma4-unified/ab-v1"
        result = ROOT / "benchmarks/results/gemma4-unified/ab-v1/gemma4_12b"
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "validation_synthesis.json"
            evidence = result_validator.validate_and_write_checkpoint(
                bundle,
                result,
                "gemma4_12b",
                "synthesis",
                output,
            )
            self.assertEqual(json.loads(output.read_text(encoding="utf-8")), evidence)
            self.assertFalse(output.with_suffix(".json.tmp").exists())

    def test_full_sweeps_validate_completed_views_but_smokes_do_not(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            result = Path(directory)
            with mock.patch.object(sweep, "validate_and_write_checkpoint") as validate:
                output = sweep.validate_completed_view(ROOT, result, "model", "embedding", 0)
                self.assertEqual(output, result / "validation_embedding.json")
                validate.assert_called_once_with(
                    ROOT / "benchmarks/fixtures/gemma4-unified/ab-v1",
                    result,
                    "model",
                    "embedding",
                    result / "validation_embedding.json",
                )
            with mock.patch.object(sweep, "validate_and_write_checkpoint") as validate:
                self.assertIsNone(sweep.validate_completed_view(ROOT, result, "model", "embedding", 2))
                validate.assert_not_called()

    def test_published_12b_synthesis_checkpoint_reduces_to_exact_summary(self) -> None:
        bundle = ROOT / "benchmarks/fixtures/gemma4-unified/ab-v1"
        result = ROOT / "benchmarks/results/gemma4-unified/ab-v1/gemma4_12b"
        cases = synthesis.load_jsonl(bundle / "synthesis.jsonl")
        raw_path = result / "raw_gemma4_12b.jsonl"
        rows = synthesis.load_jsonl(raw_path)
        latest = {row["case_id"]: row for row in rows}
        self.assertEqual(len(rows), 10_013)
        self.assertEqual(set(latest), {case["case_id"] for case in cases})
        self.assertTrue(all(latest[case["case_id"]].get("ok") for case in cases))
        expected = json.loads((result / "summary_gemma4_12b.json").read_text(encoding="utf-8"))
        actual = synthesis.summarize(
            [latest[case["case_id"]] for case in cases],
            "gemma4_12b",
            "gemma4_12b",
            validator.file_sha256(bundle / "manifest.json"),
        )
        self.assertEqual(actual, expected)
        builder.assert_no_obvious_secrets([raw_path])

    def test_published_e4b_embedding_checkpoint_reproduces_summary(self) -> None:
        bundle = ROOT / "benchmarks/fixtures/gemma4-unified/ab-v1"
        result = ROOT / "benchmarks/results/gemma4-unified/ab-v1/gemma4_e4b"
        cases = synthesis.load_jsonl(bundle / "embedding.jsonl")
        raw_path = result / "raw_embedding_gemma4_e4b.jsonl"
        rows = synthesis.load_jsonl(raw_path)
        self.assertEqual(len(rows), 10_000)
        self.assertEqual({row["case_id"] for row in rows}, {case["case_id"] for case in cases})
        summary = json.loads((result / "summary_embedding_gemma4_e4b.json").read_text(encoding="utf-8"))
        self.assertEqual(summary["suite_manifest_sha256"], validator.file_sha256(bundle / "manifest.json"))
        self.assertEqual(summary["native_dimensions"], 2_560)
        for metric in ("recall_at_1", "recall_at_5", "recall_at_10", "mrr_at_10", "ndcg_at_10"):
            actual = sum(float(row["metrics"]["native"][metric]) for row in rows) / len(rows)
            self.assertAlmostEqual(actual, float(summary["dimensions"]["native"][metric]), places=15)
        builder.assert_no_obvious_secrets([raw_path])

    def test_eurobert_reranker_extension_is_matched_and_pinned(self) -> None:
        path = MODULES / "eurobert_rerankers.json"
        manifest = json.loads(path.read_text(encoding="utf-8"))
        self.assertEqual(
            [model["label"] for model in manifest["models"]],
            ["eurobert210m_reranker", "eurobert610m_reranker"],
        )
        self.assertEqual(
            [model["revision"] for model in manifest["models"]],
            [
                "39b51e15dd1f1a06f58b5cbf6a8a188cec60bd0e",
                "d9af784ed20db6c2096e335ec6a67dd4a219924c",
            ],
        )
        self.assertEqual(
            [(model["expected_hidden_size"], model["expected_layers"]) for model in manifest["models"]],
            [(768, 12), (1152, 26)],
        )
        training = manifest["training"]
        self.assertEqual(training["dataset_revision"], "7f07e8686db233d934eacde4bf47a9995f73811e")
        self.assertEqual(len(training["configs"]) * training["examples_per_config"], 576_000)
        self.assertEqual(training["effective_batch_size"], 16)
        self.assertEqual(training["max_length"], 512)
        self.assertEqual(manifest["serving"]["max_length"], training["max_length"])
        self.assertEqual(training["loss"], "MSELoss with identity activation over teacher scores")
        dockerfile = (MODULES / "Dockerfile.eurobert-reranker").read_text(encoding="utf-8")
        self.assertIn(manifest["runtime"]["base_image"], dockerfile)
        for package, version in manifest["runtime"]["packages"].items():
            self.assertIn(version, dockerfile, package)

    def test_eurobert_extension_does_not_mutate_the_frozen_model_view_matrix(self) -> None:
        result = validator.validate(ROOT / "benchmarks/fixtures/gemma4-unified/ab-v1")
        self.assertNotIn("eurobert210m_reranker", result["baseline_model_views"])
        self.assertNotIn("eurobert610m_reranker", result["baseline_model_views"])

    def test_eurobert_training_provenance_rejects_drift(self) -> None:
        manifest_path = MODULES / "eurobert_rerankers.json"
        manifest, model = eurobert_trainer.load_spec(manifest_path, "eurobert210m_reranker")
        expected = eurobert_trainer.expected_provenance(manifest_path, manifest, model)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "training_provenance.json"
            path.write_text(json.dumps(expected), encoding="utf-8")
            eurobert_trainer.assert_compatible_provenance(path, expected)
            changed = {**expected, "training_examples": expected["training_examples"] + 1}
            with self.assertRaisesRegex(RuntimeError, "incompatible training directory"):
                eurobert_trainer.assert_compatible_provenance(path, changed)

    def test_eurobert_training_completion_requires_verified_artifacts(self) -> None:
        manifest_path = MODULES / "eurobert_rerankers.json"
        manifest, model = eurobert_trainer.load_spec(manifest_path, "eurobert210m_reranker")
        expected = eurobert_trainer.expected_provenance(manifest_path, manifest, model)
        with tempfile.TemporaryDirectory() as directory:
            output_dir = Path(directory)
            final_dir = output_dir / "final"
            final_dir.mkdir()
            for name in (
                "config.json",
                "config_sentence_transformers.json",
                "modules.json",
                "tokenizer_config.json",
                "model.safetensors",
            ):
                (final_dir / name).write_text(name, encoding="utf-8")
            artifacts = eurobert_trainer.collect_final_artifacts(final_dir)
            provenance = {**expected, "status": "complete", "final_artifacts": artifacts}
            path = output_dir / "training_provenance.json"
            eurobert_trainer.write_json_atomic(path, provenance)
            self.assertEqual(
                eurobert_trainer.assert_completed_training_dir(output_dir, expected),
                provenance,
            )
            (final_dir / "model.safetensors").write_text("truncated", encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "artifact verification failed"):
                eurobert_trainer.assert_completed_training_dir(output_dir, expected)

    def test_eurobert_runtime_smoke_is_pinned_and_fail_closed(self) -> None:
        manifest_path = MODULES / "eurobert_rerankers.json"
        manifest, model = eurobert_trainer.load_spec(manifest_path, "eurobert610m_reranker")
        expected = eurobert_smoke.expected_smoke_provenance(manifest_path, manifest, model)
        self.assertIn("trainer_sha256", expected)
        self.assertIn("smoke_script_sha256", expected)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "runtime_smoke.json"
            completed = {**expected, "status": "complete"}
            eurobert_trainer.write_json_atomic(path, completed)
            self.assertEqual(eurobert_smoke.assert_completed_smoke(path, expected), completed)
            changed = {**completed, "smoke_script_sha256": "0" * 64}
            eurobert_trainer.write_json_atomic(path, changed)
            with self.assertRaisesRegex(RuntimeError, "provenance does not match"):
                eurobert_smoke.assert_completed_smoke(path, expected)

    def test_cross_encoder_server_validates_aligned_string_pairs(self) -> None:
        pairs = [["query", "document"], ["second", "candidate"]]
        self.assertEqual(eurobert_server.validate_pairs(pairs), pairs)
        for invalid in ({}, [["query"]], [["query", 1]]):
            with self.assertRaises(ValueError):
                eurobert_server.validate_pairs(invalid)

    def test_eurobert_controller_requests_exclusive_rocm_devices(self) -> None:
        command = eurobert_controller.gpu_container_prefix(
            "/docker.sock",
            "server",
            Path("/bench"),
            Path("/repo"),
            "image",
            detach=True,
        )
        self.assertEqual(command[:5], ["docker", "-H", "unix:///docker.sock", "run", "--detach"])
        self.assertEqual(command.count("--rm"), 1)
        self.assertIn("/dev/kfd:/dev/kfd", command)
        self.assertIn("/dev/dri:/dev/dri", command)
        self.assertEqual(eurobert_controller.sweep_lock_flags(True), eurobert_controller.fcntl.LOCK_EX)
        self.assertEqual(
            eurobert_controller.sweep_lock_flags(False),
            eurobert_controller.fcntl.LOCK_EX | eurobert_controller.fcntl.LOCK_NB,
        )
        with tempfile.TemporaryDirectory() as directory:
            state_path = Path(directory) / "RUN_STATE.json"
            state_path.write_text(
                json.dumps({"status": "complete", "production_restored": True}),
                encoding="utf-8",
            )
            self.assertEqual(eurobert_controller.assert_restored_handoff(state_path)["status"], "complete")
            state_path.write_text(json.dumps({"status": "failed"}), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "did not complete"):
                eurobert_controller.assert_restored_handoff(state_path)

    def test_eurobert_controller_verifies_all_pairwise_report_artifacts(self) -> None:
        labels = ["eurobert210m_reranker", "eurobert610m_reranker"]
        expected_pairs = list(
            eurobert_controller.itertools.combinations(["ettin68m", "ettin400m", *labels], 2)
        )
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            reports = []
            for left, right in expected_pairs:
                path = output / f"reranking_{left}_vs_{right}.json"
                path.write_text(json.dumps({"left": left, "right": right}), encoding="utf-8")
                reports.append({
                    "left": left,
                    "right": right,
                    "file": path.name,
                    "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                })
            index_path = output / "INDEX.json"
            index_path.write_text(
                json.dumps({"pair_count": len(reports), "reports": reports}), encoding="utf-8"
            )
            actual = eurobert_controller.assert_completed_pairwise_index(index_path, labels)
            self.assertEqual(actual["pair_count"], 6)
            (output / reports[-1]["file"]).write_text("corrupt", encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "artifact verification failed"):
                eurobert_controller.assert_completed_pairwise_index(index_path, labels)

    def test_sweep_requires_both_ettin_execution_profiles(self) -> None:
        self.assertEqual(sweep.ETTIN_CONTROLS, (
            {"label": "ettin68m", "tier": "cpu", "ngl": "0", "execution": "cpu"},
            {"label": "ettin400m", "tier": "mid", "ngl": "99", "execution": "gpu"},
        ))
        self.assertEqual(sweep.ETTIN_LOAD_PROFILE, {
            "workers": 8,
            "pairs_per_request": 4,
            "parallel_slots": 32,
            "context_tokens": 65536,
            "logical_batch_tokens": 2048,
            "physical_batch_tokens": 2048,
        })

    def test_sweep_uses_explicit_model_load_profiles(self) -> None:
        self.assertEqual(set(sweep.MODEL_LOAD_PROFILES), {
            "gemma4_e2b", "gemma4_e4b", "gemma4_12b", "gemma4_26b_a4b",
            "gemma4_31b", "qwen36_35b_a3b",
        })
        e2b = sweep.MODEL_LOAD_PROFILES["gemma4_e2b"]["synthesis"]
        self.assertEqual(e2b["workers"], 64)
        self.assertEqual(e2b["parallel_slots"], 64)
        self.assertGreaterEqual(e2b["context_tokens"] // e2b["parallel_slots"], 2048)
        for profiles in sweep.MODEL_LOAD_PROFILES.values():
            synthesis_profile = profiles["synthesis"]
            self.assertEqual(synthesis_profile["workers"], synthesis_profile["parallel_slots"])
            self.assertGreaterEqual(
                synthesis_profile["context_tokens"] // synthesis_profile["parallel_slots"],
                2048,
            )
            embedding_profile = profiles["embedding"]
            self.assertGreaterEqual(
                embedding_profile["context_tokens"] // embedding_profile["parallel_slots"],
                2048,
            )
        e2b_embedding = sweep.MODEL_LOAD_PROFILES["gemma4_e2b"]["embedding"]
        self.assertEqual(e2b_embedding["parallel_slots"], 64)
        self.assertEqual(e2b_embedding["batch_size"], 64)
        self.assertEqual(sweep.MODEL_LOAD_PROFILES["gemma4_e4b"]["synthesis"]["parallel_slots"], 64)
        self.assertEqual(sweep.MODEL_LOAD_PROFILES["gemma4_12b"]["synthesis"]["parallel_slots"], 32)
        self.assertEqual(sweep.MODEL_LOAD_PROFILES["gemma4_12b"]["embedding"], {
            "parallel_slots": 16,
            "context_tokens": 32768,
            "physical_batch_tokens": 2048,
            "batch_size": 16,
        })

    def test_sweep_mode_selection_supports_isolated_recovery(self) -> None:
        self.assertEqual(sweep.parse_modes("synthesis"), ("synthesis",))
        self.assertEqual(sweep.parse_modes("embedding,synthesis"), ("embedding", "synthesis"))
        for invalid in ("", "reranking", "synthesis,synthesis"):
            with self.assertRaises(ValueError):
                sweep.parse_modes(invalid)

    def test_remaining_chain_preserves_required_stage_order_and_scope(self) -> None:
        root = Path("/bench")
        repo = Path("/repo")
        plan = remaining_chain.stage_plan(root, repo)
        self.assertEqual(
            [stage["name"] for stage in plan],
            ["eurobert_rerankers", "gemma4_e4b_synthesis_recovery", "remaining_model_views"],
        )
        eurobert, e4b, remaining = (stage["command"] for stage in plan)
        self.assertIn("--wait-for-lock", eurobert)
        self.assertEqual(eurobert[eurobert.index("--handoff-state") + 1], "/bench/results/RUN_STATE.json")
        self.assertEqual(e4b[e4b.index("--modes") + 1], "synthesis")
        self.assertEqual(e4b[e4b.index("--labels") + 1], "gemma4_e4b")
        self.assertEqual(
            remaining[remaining.index("--labels") + 1],
            "gemma4_26b_a4b,gemma4_31b,qwen36_35b_a3b",
        )

    def test_remaining_chain_is_resumable_and_stops_on_failure(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "root"
            repo = Path(directory) / "repo"
            root.mkdir()
            argv = ["run_254_remaining_chain.py", "--root", str(root), "--repo", str(repo)]
            succeeded = mock.Mock(returncode=0)
            with mock.patch.object(sys, "argv", argv), mock.patch.object(
                remaining_chain.subprocess, "run", return_value=succeeded
            ) as run:
                self.assertEqual(remaining_chain.main(), 0)
                self.assertEqual(run.call_count, 3)
            state_path = root / "remaining_chain_state.json"
            state = json.loads(state_path.read_text(encoding="utf-8"))
            self.assertEqual(state["status"], "complete")
            self.assertEqual(len(state["completed"]), 3)
            with mock.patch.object(sys, "argv", argv), mock.patch.object(
                remaining_chain.subprocess, "run", side_effect=AssertionError("repeated")
            ):
                self.assertEqual(remaining_chain.main(), 0)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "root"
            repo = Path(directory) / "repo"
            root.mkdir()
            argv = ["run_254_remaining_chain.py", "--root", str(root), "--repo", str(repo)]
            with mock.patch.object(sys, "argv", argv), mock.patch.object(
                remaining_chain.subprocess, "run", return_value=mock.Mock(returncode=1)
            ) as run:
                with self.assertRaisesRegex(RuntimeError, "eurobert_rerankers exited 1"):
                    remaining_chain.main()
                run.assert_called_once()
            state = json.loads((root / "remaining_chain_state.json").read_text(encoding="utf-8"))
            self.assertEqual(state["status"], "failed")
            self.assertEqual(state["active"], "eurobert_rerankers")

    def test_frozen_bundle_is_exact_and_paired(self) -> None:
        result = validator.validate(ROOT / "benchmarks/fixtures/gemma4-unified/ab-v1")
        self.assertEqual(result["case_rows_per_view"], 10_000)
        self.assertEqual(result["shared_case_ids"], 10_000)
        self.assertEqual(set(result["baseline_model_views"]), {
            "gemma4_e2b", "gemma4_e4b", "gemma4_12b", "gemma4_26b_a4b",
            "gemma4_31b", "qwen36_35b_a3b", "ettin68m", "ettin400m",
        })

    def test_serialized_secret_scan_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.jsonl"
            path.write_text('{"value":"api_key=123456789abcdef"}\n', encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "secret/de-identification scan failed"):
                builder.assert_no_obvious_secrets([path])

    def test_reranker_bound_is_exact_and_keeps_head_and_tail(self) -> None:
        value = "A" * 900 + "B" * 900
        bounded = reranker.bounded_text(value, 1_024)
        self.assertEqual(len(bounded), 1_024)
        self.assertTrue(bounded.startswith("A"))
        self.assertTrue(bounded.endswith("B"))
        self.assertIn("[...truncated...]", bounded)

    def test_embedding_request_retries_transient_timeout(self) -> None:
        if embedding is None:
            self.skipTest("numpy is not installed")

        class Response:
            def __enter__(self) -> "Response":
                return self

            def __exit__(self, *_args: object) -> None:
                return None

            def read(self, *_args: object) -> bytes:
                return json.dumps({
                    "data": [{"index": 0, "embedding": [1.0, 2.0]}],
                    "usage": {"prompt_tokens": 1},
                }).encode()

        with mock.patch.object(embedding.urllib.request, "urlopen", side_effect=[TimeoutError("slow"), Response()]) as urlopen:
            with mock.patch.object(embedding.time, "sleep") as sleep:
                vectors, timing = embedding.request_embeddings("http://unused", "test", ["text"], 1, False)
        self.assertEqual(urlopen.call_count, 2)
        sleep.assert_called_once_with(1)
        self.assertEqual(vectors.tolist(), [[1.0, 2.0]])
        self.assertEqual(timing["attempts"], 2)

    def test_embedding_document_batches_resume_from_partial_cache(self) -> None:
        if embedding is None:
            self.skipTest("numpy is not installed")
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            ids = ["a", "b", "c"]
            corpus = {key: key for key in ids}
            responses = [
                (embedding.np.asarray([[1.0, 0.0], [0.0, 1.0]], dtype=embedding.np.float32), {"latency_s": 1.0}),
                (embedding.np.asarray([[1.0, 1.0]], dtype=embedding.np.float32), {"latency_s": 2.0}),
            ]
            with mock.patch.object(embedding, "request_embeddings", side_effect=responses) as request:
                matrix, telemetry = embedding.embed_document_cache(
                    "http://unused", "test", ids, corpus, output, "resume", 2, 1, False
                )
            self.assertEqual(request.call_count, 2)
            self.assertEqual(matrix.shape, (3, 2))
            self.assertEqual([row["latency_s"] for row in telemetry], [1.0, 2.0])

            (output / "doc_vectors_resume.npy").unlink()
            (output / "doc_ids_resume.json").unlink()
            (output / "doc_telemetry_resume.json").unlink()
            with mock.patch.object(embedding, "request_embeddings", side_effect=AssertionError("repeated")):
                resumed, resumed_telemetry = embedding.embed_document_cache(
                    "http://unused", "test", ids, corpus, output, "resume", 2, 1, False
                )
            self.assertEqual(resumed.tolist(), matrix.tolist())
            self.assertEqual(resumed_telemetry, telemetry)

    def test_reranker_resume_retries_failed_case(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bundle, output = root / "bundle", root / "results"
            bundle.mkdir()
            output.mkdir()
            (bundle / "manifest.json").write_text('{"suite":"test"}\n', encoding="utf-8")
            (bundle / "corpus.jsonl").write_text(
                json.dumps({"doc_id": "doc", "content": "candidate"}) + "\n", encoding="utf-8"
            )
            case = {
                "case_id": "case", "query": "query", "candidate_doc_ids": ["doc"],
                "relevance": {"doc": 1},
            }
            (bundle / "reranking.jsonl").write_text(json.dumps(case) + "\n", encoding="utf-8")
            raw = output / "raw_reranking_resume.jsonl"
            raw.write_text(json.dumps({"case_id": "case", "ok": False}) + "\n", encoding="utf-8")
            successful = {
                "case_id": "case", "ok": True, "attempts": 1, "latency_s": 0.1,
                "request_chunks": 1, "scores": [1.0], "ranked_doc_ids": ["doc"],
                "metrics": reranker.ranking_metrics(["doc"], {"doc": 1}),
            }
            argv = [
                "run_reranking_ab.py", "--endpoint", "http://unused", "--label", "resume",
                "--bundle", str(bundle), "--output-dir", str(output), "--workers", "8",
            ]
            with mock.patch.object(sys, "argv", argv), mock.patch.object(reranker, "call", return_value=successful) as call:
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(reranker.main(), 0)
            call.assert_called_once()
            self.assertEqual(json.loads(raw.read_text(encoding="utf-8").splitlines()[-1])["ok"], True)
            summary = json.loads((output / "summary_reranking_resume.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["load_profile"]["workers"], 8)
            self.assertEqual(summary["load_profile"]["maximum_inflight_pairs"], 32)

    def test_pairwise_loader_reduces_append_only_rows_to_latest(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "raw.jsonl"
            path.write_text(
                "\n".join(
                    (
                        json.dumps({"case_id": "a", "ok": False}),
                        json.dumps({"case_id": "b", "ok": True, "value": 1}),
                        json.dumps({"case_id": "a", "ok": True, "value": 2}),
                    )
                ) + "\n",
                encoding="utf-8",
            )
            self.assertEqual(comparison.load(path)["a"]["value"], 2)
            path.write_text(json.dumps({"case_id": "a", "ok": False}) + "\n", encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "latest case rows are unsuccessful"):
                comparison.load(path)

    def test_cross_family_reranker_report_discloses_training_asymmetry(self) -> None:
        context = reranker_reports.comparison_context("ettin400m", "eurobert610m_reranker", 576_000)
        self.assertIs(context["training_budget_equal"], False)
        self.assertEqual(context["ettin_training_examples"], 143_393_475)
        self.assertEqual(context["eurobert_training_examples"], 576_000)
        self.assertIn("diagnostic_only", context["latency_qualification"])
        eurobert_only = reranker_reports.comparison_context(
            "eurobert210m_reranker", "eurobert610m_reranker", 576_000
        )
        self.assertIn("qualified", eurobert_only["latency_qualification"])

    def test_synthesis_resume_does_not_repeat_completed_case(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bundle, output = root / "bundle", root / "results"
            bundle.mkdir()
            (bundle / "manifest.json").write_text('{"suite":"test"}\n', encoding="utf-8")
            (bundle / "corpus.jsonl").write_text(
                json.dumps({"doc_id": "doc", "content": "source"}) + "\n", encoding="utf-8"
            )
            case = {
                "case_id": "case", "task": "entity", "instruction": "extract",
                "source_doc_id": "doc", "expected": {"name": "x", "entity_kind": "test", "context": "source"},
            }
            (bundle / "synthesis.jsonl").write_text(json.dumps(case) + "\n", encoding="utf-8")
            result = {
                "case_id": "case", "task": "entity", "ok": True, "attempts": 1, "latency_s": 0.1,
                "raw_parse": True, "empty": False, "truncated": False,
                "usage": {"completion_tokens": 1, "prompt_tokens": 1}, "timings": {},
                "metrics": {"schema_valid": True, "required_field_recall": 1.0, "content_f1": 1.0},
            }
            argv = [
                "run_synthesis_ab.py", "--endpoint", "http://unused", "--model", "test", "--label", "resume",
                "--bundle", str(bundle), "--output-dir", str(output),
            ]
            with mock.patch.object(sys, "argv", argv), mock.patch.object(synthesis, "call", return_value=result), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(synthesis.main(), 0)
            with mock.patch.object(sys, "argv", argv), mock.patch.object(synthesis, "call", side_effect=AssertionError("repeated")), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(synthesis.main(), 0)

    def test_synthesis_resume_retries_failed_case(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bundle, output = root / "bundle", root / "results"
            bundle.mkdir()
            output.mkdir()
            (bundle / "manifest.json").write_text('{"suite":"test"}\n', encoding="utf-8")
            (bundle / "corpus.jsonl").write_text(
                json.dumps({"doc_id": "doc", "content": "source"}) + "\n", encoding="utf-8"
            )
            case = {
                "case_id": "case", "task": "entity", "instruction": "extract",
                "source_doc_id": "doc", "expected": {"name": "x", "entity_kind": "test", "context": "source"},
            }
            (bundle / "synthesis.jsonl").write_text(json.dumps(case) + "\n", encoding="utf-8")
            raw = output / "raw_resume.jsonl"
            raw.write_text(json.dumps({"case_id": "case", "task": "entity", "ok": False}) + "\n", encoding="utf-8")
            result = {
                "case_id": "case", "task": "entity", "ok": True, "attempts": 1, "latency_s": 0.1,
                "raw_parse": True, "empty": False, "truncated": False,
                "usage": {"completion_tokens": 1, "prompt_tokens": 1}, "timings": {},
                "metrics": {"schema_valid": True, "required_field_recall": 1.0, "content_f1": 1.0},
            }
            argv = [
                "run_synthesis_ab.py", "--endpoint", "http://unused", "--model", "test", "--label", "resume",
                "--bundle", str(bundle), "--output-dir", str(output),
            ]
            with mock.patch.object(sys, "argv", argv), mock.patch.object(synthesis, "call", return_value=result) as call:
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(synthesis.main(), 0)
            call.assert_called_once()
            self.assertTrue(json.loads(raw.read_text(encoding="utf-8").splitlines()[-1])["ok"])

    def test_synthesis_fails_closed_when_retry_still_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bundle, output = root / "bundle", root / "results"
            bundle.mkdir()
            (bundle / "manifest.json").write_text('{"suite":"test"}\n', encoding="utf-8")
            (bundle / "corpus.jsonl").write_text(
                json.dumps({"doc_id": "doc", "content": "source"}) + "\n", encoding="utf-8"
            )
            case = {
                "case_id": "case", "task": "entity", "instruction": "extract",
                "source_doc_id": "doc", "expected": {"name": "x", "entity_kind": "test", "context": "source"},
            }
            (bundle / "synthesis.jsonl").write_text(json.dumps(case) + "\n", encoding="utf-8")
            failed = {
                "case_id": "case", "task": "entity", "ok": False, "attempts": 3, "latency_s": 0.0,
                "raw_parse": False, "empty": True, "truncated": False,
                "metrics": {"schema_valid": False, "required_field_recall": 0.0, "content_f1": 0.0},
            }
            argv = [
                "run_synthesis_ab.py", "--endpoint", "http://unused", "--model", "test", "--label", "resume",
                "--bundle", str(bundle), "--output-dir", str(output),
            ]
            with mock.patch.object(sys, "argv", argv), mock.patch.object(synthesis, "call", return_value=failed):
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(synthesis.main(), 1)

    def test_synthesis_redacts_generated_secret_like_text_after_scoring(self) -> None:
        row = {
            "case_id": "case",
            "ok": True,
            "metrics": {"content_f1": 1.0},
            "response": '{"signature":"set_token: credential-shaped-placeholder"}',
        }
        persisted = synthesis.persisted_row(row)
        self.assertEqual(persisted["metrics"], row["metrics"])
        self.assertEqual(persisted["response"], "<REDACTED_GENERATED_RESPONSE>")
        self.assertTrue(persisted["response_redacted"])
        self.assertEqual(persisted["response_sha256"], synthesis.hashlib.sha256(row["response"].encode()).hexdigest())
        self.assertEqual(row["response"], '{"signature":"set_token: credential-shaped-placeholder"}')


if __name__ == "__main__":
    unittest.main()
