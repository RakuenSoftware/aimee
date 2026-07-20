#!/usr/bin/env python3
"""Mutation tests for check_git_core_contract.py."""

from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
CHECKER_PATH = REPO_ROOT / "scripts/check_git_core_contract.py"
SPEC = importlib.util.spec_from_file_location("check_git_core_contract", CHECKER_PATH)
assert SPEC and SPEC.loader
checker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checker)


class GitCoreContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.contract_path = REPO_ROOT / checker.DEFAULT_CONTRACT
        cls.contract = checker.extract_contract(cls.contract_path)

    def fresh(self) -> dict[str, object]:
        contract = copy.deepcopy(self.contract)
        contract["lifecycle"] = {
            "status": "pending",
            "enforcement_scope": "structural-only",
            "approval_evidence": None,
        }
        return contract

    def validate_pending(self, contract: dict[str, object]) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            checker.validate_contract(
                contract,
                path=Path(tmp) / checker.DEFAULT_CONTRACT,
                config_root=Path(tmp),
                require_status="pending",
                check_git=False,
            )

    def assert_invalid(self, mutate, rule: str | None = None) -> None:
        contract = self.fresh()
        mutate(contract)
        with self.assertRaises(checker.ContractError) as caught:
            self.validate_pending(contract)
        if rule:
            self.assertIn(f"rule={rule}", str(caught.exception))

    def approved_fixture(self, contract: dict[str, object], root: Path) -> None:
        digest = checker.reviewed_contract_sha256(contract)
        evidence = {
            "schema_version": 1,
            "run_id": "oprun_test_approval_1",
            "decision": "approve",
            "decided_at": "2026-07-20T22:00:00Z",
            "reviewed_contract_sha256": digest,
            "findings": [],
            "overall": "No issues found by the panel.",
        }
        evidence_path = root / checker.EVIDENCE_PATH
        evidence_path.parent.mkdir(parents=True)
        evidence_raw = json.dumps(evidence, sort_keys=True, indent=2) + "\n"
        evidence_path.write_text(evidence_raw, encoding="utf-8")
        contract["lifecycle"] = {
            "status": "roundtable-approved",
            "enforcement_scope": "structural-only",
            "approval_evidence": {
                "run_id": evidence["run_id"],
                "artifact_path": checker.EVIDENCE_PATH.as_posix(),
                "file_sha256": hashlib.sha256(evidence_raw.encode()).hexdigest(),
                "reviewed_contract_sha256": digest,
            },
        }

    def test_repository_contract_is_valid_pending(self) -> None:
        handoff_path = REPO_ROOT / checker.DEFAULT_HANDOFF
        checker.validate_handoff(
            checker.extract_json_fence(handoff_path, "slice3-handoff"), handoff_path
        )
        status = self.contract["lifecycle"]["status"]
        checker.validate_contract(
            copy.deepcopy(self.contract),
            path=self.contract_path,
            config_root=REPO_ROOT,
            require_status=status,
            check_git=True,
        )

    def test_handoff_requires_exact_ordered_invariants(self) -> None:
        handoff_path = REPO_ROOT / checker.DEFAULT_HANDOFF
        handoff = checker.extract_json_fence(handoff_path, "slice3-handoff")
        handoff["invariants"] = handoff["invariants"][:-1]
        with self.assertRaisesRegex(checker.ContractError, "handoff-invariants"):
            checker.validate_handoff(handoff, handoff_path)

    def test_strict_json_rejects_duplicate_key(self) -> None:
        with self.assertRaisesRegex(checker.ContractError, "json-duplicate-key"):
            checker.loads_strict('{"module":"git","module":"memory"}', path="test")

    def test_strict_json_rejects_float_exponent_and_nonfinite(self) -> None:
        for raw in ('{"value":1.0}', '{"value":1e2}', '{"value":NaN}', '{"value":Infinity}'):
            with self.subTest(raw=raw), self.assertRaisesRegex(checker.ContractError, "json-number-domain"):
                checker.loads_strict(raw, path="test")

    def test_top_level_schema_and_versions_fail_closed(self) -> None:
        cases = (
            (lambda c: c.__setitem__("unknown", True), "schema"),
            (lambda c: c.pop("module"), "schema"),
            (lambda c: c.__setitem__("schema_version", "1"), "pinned-value"),
            (lambda c: c.__setitem__("schema_version", 2), "pinned-value"),
            (lambda c: c.__setitem__("contract_version", 1), "pinned-value"),
            (lambda c: c.__setitem__("contract_version", "2.0.0"), "pinned-value"),
            (lambda c: c.__setitem__("classification", "optional"), "pinned-value"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule, mutation=mutate):
                self.assert_invalid(mutate, rule)

    def test_memory_ownership_is_pinned(self) -> None:
        cases = (
            lambda c: c["ownership"].__setitem__("memory_owner", "git"),
            lambda c: c["ownership"].__setitem__("code_intelligence_exclusive_to_memory", False),
            lambda c: c["ownership"].__setitem__("git_may_persist_code_intelligence", True),
            lambda c: c["ingest_boundary"].__setitem__("writer", "git"),
            lambda c: c["ingest_boundary"].__setitem__("git_access", "write"),
        )
        for mutate in cases:
            with self.subTest(mutation=mutate):
                self.assert_invalid(mutate)

    def test_code_intelligence_namespace_forbids_git_writer(self) -> None:
        def mutate(contract):
            contract["principal"]["write_scope"].append(
                {
                    "namespace": "memory.code-intelligence.repository-records",
                    "principal": "git",
                    "access": "write",
                }
            )

        self.assert_invalid(mutate, "memory-exclusive")

    def test_code_intelligence_namespace_requires_memory_writer(self) -> None:
        self.assert_invalid(lambda c: c["principal"].__setitem__("write_scope", []), "schema")

    def test_ingest_namespace_must_be_memory_owned(self) -> None:
        self.assert_invalid(
            lambda c: c["ingest_boundary"].__setitem__("namespace", "memory.other"),
            "ingest-boundary",
        )

    def test_principal_scope_is_closed_default_deny_and_unique(self) -> None:
        cases = (
            lambda c: c["principal"].__setitem__("policy", "allow"),
            lambda c: c["principal"]["read_scope"][0].__setitem__("extra", True),
            lambda c: c["principal"]["read_scope"].append(copy.deepcopy(c["principal"]["read_scope"][0])),
        )
        for mutate in cases:
            with self.subTest(mutation=mutate):
                self.assert_invalid(mutate)

    def test_provenance_requires_both_and_distinct_key_identities(self) -> None:
        cases = (
            lambda c: c["provenance"].__setitem__("require_both", False),
            lambda c: c["provenance"].__setitem__("identity_rule", "same-key"),
            lambda c: c["provenance"]["producer"]["signature"].__setitem__("algorithms", []),
            lambda c: c["provenance"]["repository"].pop("signature"),
        )
        for mutate in cases:
            with self.subTest(mutation=mutate):
                self.assert_invalid(mutate)

    def test_redaction_is_closed_complete_and_disjoint(self) -> None:
        cases = (
            lambda c: c["redaction"].__setitem__("policy", "best-effort"),
            lambda c: c["redaction"].__setitem__("stage", "after-memory-ingest"),
            lambda c: c["redaction"]["deny_classes"].remove("tokens"),
            lambda c: c["redaction"]["applies_to"].remove("producer-attestation"),
            lambda c: c["redaction"]["allow_classes"].append("credentials"),
        )
        for mutate in cases:
            with self.subTest(mutation=mutate):
                self.assert_invalid(mutate)

    def test_non_git_workspace_remains_usable(self) -> None:
        cases = (
            lambda c: c["non_git_workspace"].__setitem__("behavior", "disabled"),
            lambda c: c["non_git_workspace"].__setitem__("git_only_operation", "error"),
        )
        for mutate in cases:
            with self.subTest(mutation=mutate):
                self.assert_invalid(mutate)

    def test_trigger_paths_are_safe_closed_and_unique(self) -> None:
        cases = (
            lambda c: c["trigger_surface"]["descriptors"][0].__setitem__("path", "../module.yaml"),
            lambda c: c["trigger_surface"]["generated_profiles"].append(copy.deepcopy(c["trigger_surface"]["generated_profiles"][0])),
            lambda c: c["trigger_surface"]["readiness_markers"][0].__setitem__("value", "available"),
            lambda c: c["trigger_surface"]["status_claim_roots"][0].__setitem__("claim", "ready"),
            lambda c: c["trigger_surface"].__setitem__("generated_builds", []),
        )
        for mutate in cases:
            with self.subTest(mutation=mutate):
                self.assert_invalid(mutate)

    def test_acceptance_kind_outcomes_are_exact(self) -> None:
        for kind, (decision, reason) in checker.EXPECTED_ACCEPTANCE.items():
            for field, bad in (("decision", "pass" if decision != "pass" else "deny"), ("reason_code", "UNKNOWN")):
                def mutate(contract, kind=kind, field=field, bad=bad):
                    entry = next(item for item in contract["acceptance"] if item["kind"] == kind)
                    entry["expected"][field] = bad

                with self.subTest(kind=kind, field=field):
                    self.assert_invalid(mutate, "pinned-value")

    def test_acceptance_missing_unknown_and_duplicate_fail(self) -> None:
        cases = (
            lambda c: c["acceptance"].pop(),
            lambda c: c["acceptance"][0].__setitem__("kind", "unknown_kind"),
            lambda c: c["acceptance"].append(copy.deepcopy(c["acceptance"][0])),
        )
        for mutate in cases:
            with self.subTest(mutation=mutate):
                self.assert_invalid(mutate)

    def test_historical_cutoff_fields_are_immutable(self) -> None:
        cases = (
            lambda c: c["historical_cutoff"].__setitem__("commit", "0" * 40),
            lambda c: c["historical_cutoff"].__setitem__("ref", "refs/heads/testing"),
            lambda c: c["historical_cutoff"].__setitem__("pinned_after_slice", 2),
            lambda c: c["historical_cutoff"].__setitem__("path", "src/git"),
        )
        for mutate in cases:
            with self.subTest(mutation=mutate):
                self.assert_invalid(mutate, "pinned-value")

    def test_pending_rejects_prepopulated_evidence(self) -> None:
        self.assert_invalid(
            lambda c: c["lifecycle"].__setitem__("approval_evidence", {}), "lifecycle"
        )

    def test_roundtable_approved_evidence_binds_contract_and_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            contract = self.fresh()
            self.approved_fixture(contract, root)
            checker.validate_contract(
                contract,
                path=root / checker.DEFAULT_CONTRACT,
                config_root=root,
                require_status="roundtable-approved",
                check_git=False,
            )

    def test_approved_rejects_substantive_post_review_mutation(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            contract = self.fresh()
            self.approved_fixture(contract, root)
            contract["ownership"]["producer"] = "git-mutated"
            with self.assertRaisesRegex(checker.ContractError, "review-digest"):
                checker.validate_contract(
                    contract,
                    path=root / checker.DEFAULT_CONTRACT,
                    config_root=root,
                    require_status="roundtable-approved",
                    check_git=False,
                )

    def test_approved_rejects_evidence_file_tampering(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            contract = self.fresh()
            self.approved_fixture(contract, root)
            evidence_path = root / checker.EVIDENCE_PATH
            evidence_path.write_text(evidence_path.read_text() + " ", encoding="utf-8")
            with self.assertRaisesRegex(checker.ContractError, "evidence-file-digest"):
                checker.validate_contract(
                    contract,
                    path=root / checker.DEFAULT_CONTRACT,
                    config_root=root,
                    require_status="roundtable-approved",
                    check_git=False,
                )

    def test_approved_rejects_blocking_finding(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            contract = self.fresh()
            self.approved_fixture(contract, root)
            evidence_path = root / checker.EVIDENCE_PATH
            evidence = json.loads(evidence_path.read_text())
            evidence["findings"] = [{"severity": "blocking", "location": "contract", "message": "no"}]
            raw = json.dumps(evidence, sort_keys=True, indent=2) + "\n"
            evidence_path.write_text(raw, encoding="utf-8")
            contract["lifecycle"]["approval_evidence"]["file_sha256"] = hashlib.sha256(raw.encode()).hexdigest()
            with self.assertRaisesRegex(checker.ContractError, "evidence-blocker"):
                checker.validate_contract(
                    contract,
                    path=root / checker.DEFAULT_CONTRACT,
                    config_root=root,
                    require_status="roundtable-approved",
                    check_git=False,
                )

    def test_config_root_containment_rejects_escape(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(checker.ContractError, "path-containment"):
                checker.resolve_under_root(Path(tmp), Path("../outside"), label="contract")


if __name__ == "__main__":
    unittest.main()
