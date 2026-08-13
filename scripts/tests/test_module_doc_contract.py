#!/usr/bin/env python3
"""Tests for provider-neutral module-document attestation contracts."""

from __future__ import annotations

import base64
import contextlib
import os
import signal
from contextlib import contextmanager
from datetime import datetime, timezone
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import struct
import tempfile
import threading
import time
import unittest
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = REPO_ROOT / "scripts/module_doc_contract.py"
SPEC = importlib.util.spec_from_file_location("module_doc_contract", MODULE_PATH)
assert SPEC and SPEC.loader
contract = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(contract)
FIXTURES = REPO_ROOT / "tests/fixtures/module-doc-contract"


def _proc_stat_fields(pid: int) -> list[str] | None:
    """Fields of /proc/<pid>/stat after the comm, or None if the pid is gone.

    Splitting on the LAST ")" is deliberate: comm is an arbitrary program name in
    parentheses and may itself contain them.
    """
    try:
        stat = Path(f"/proc/{pid}/stat").read_text()
    except (FileNotFoundError, ProcessLookupError):
        return None
    _, separator, tail = stat.rpartition(")")
    fields = tail.split()
    if not separator or not fields:
        raise AssertionError(f"Malformed /proc/{pid}/stat")
    return fields


def process_starttime(pid: int) -> str | None:
    """The process's start time in clock ticks, or None if it is already gone.

    A pid is a NUMBER, and Linux hands numbers out again. Start time is what
    makes it an identity: a recycled pid names a process that began later, so
    (pid, starttime) is stable where pid alone is not.
    """
    fields = _proc_stat_fields(pid)
    if fields is None:
        return None
    # /proc/<pid>/stat field 22 is starttime; `fields` begins at field 3 (state).
    return fields[19] if len(fields) > 19 else None


def process_is_gone_or_zombie(pid: int, starttime: str | None = None) -> bool:
    """Return whether the process we meant is vanished, a zombie, or replaced.

    Pass the `starttime` captured when the pid was learned. Without it this
    answers a question about a NUMBER rather than about a process, and on a busy
    machine the number can belong to something else by the time it is asked --
    which reads as "still alive" and is how this reported a working kill as a
    failure.
    """
    fields = _proc_stat_fields(pid)
    if fields is None:
        return True
    if fields[0] == "Z":
        return True
    if starttime is not None and (len(fields) <= 19 or fields[19] != starttime):
        # The pid was recycled: ours is gone and this is somebody else's process.
        return True
    return False


def describe_process(pid: int) -> str:
    """What /proc says about a pid, for a failure message that can be acted on."""
    fields = _proc_stat_fields(pid)
    if fields is None:
        return f"pid {pid}: gone"
    try:
        cmdline = Path(f"/proc/{pid}/cmdline").read_bytes().replace(b"\0", b" ").decode(
            "utf-8", "replace"
        ).strip()
    except OSError:
        cmdline = "<unreadable>"
    state = fields[0]
    start = fields[19] if len(fields) > 19 else "?"
    return f"pid {pid}: state={state} starttime={start} cmdline={cmdline!r}"


def locked_toolchain():
    executable_text = shutil.which("ssh-keygen")
    if executable_text is None:
        raise AssertionError("ssh-keygen is required by the module-doc contract suite")
    executable = Path(executable_text)
    lock = {
        "schema": "aimee.sshsig-toolchain.v1",
        "image": "registry.example.invalid/aimee/sshsig@sha256:" + "a" * 64,
        "ssh_keygen_sha256": contract.sha256(executable.read_bytes()),
    }
    return contract.load_locked_toolchain(lock, executable)


def oidc_profile() -> dict[str, object]:
    mappings = {
        field: f"ci_{field}" for field in contract.CLAIM_MAPPED_WORKLOAD_FIELDS
    }
    return {
        "schema": "aimee.ci-oidc-profile.v1",
        "name": "protected-ci",
        "issuer": "https://issuer.example.invalid",
        "audience": "aimee-module-doc-verifier",
        "jwks": {
            "uri": "https://issuer.example.invalid/jwks",
            "tls_spki_sha256": ["A" * 43 + "="],
            "jwk_thumbprints_sha256": ["C" * 43],
            "max_age_seconds": 3600,
        },
        "allowed_algorithms": ["ES256", "RS256"],
        "max_token_lifetime_seconds": 600,
        "clock_skew_seconds": 60,
        "claim_mappings": mappings,
        "predicates": {
            "repository_identity": "repository-17",
            "workflow_identity": "module-doc-trigger",
            "workflow_revision": "0123456789abcdef0123456789abcdef01234567",
            "event_type": "pull-request-target",
        },
        "repository_api": {
            "base_url": "https://repository.example.invalid/api",
            "tls_spki_sha256": ["B" * 43 + "="],
            "response_schema": "aimee.candidate-target.v1",
        },
    }


def claims(profile: dict[str, object], now: int = 1_800_000_000) -> dict[str, object]:
    value: dict[str, object] = {
        "iss": profile["issuer"], "sub": "workload:42", "aud": profile["audience"],
        "iat": now, "nbf": now - 1, "exp": now + 300, "jti": "token-1",
    }
    predicates = profile["predicates"]
    mappings = profile["claim_mappings"]
    assert isinstance(predicates, dict) and isinstance(mappings, dict)
    for field, claim_name in mappings.items():
        value[claim_name] = predicates.get(field, "1" if field == "attempt" else f"value-{field}")
    return value


class ModuleDocContractTests(unittest.TestCase):
    def test_process_state_probe_distinguishes_exit_from_live_processes(self):
        with mock.patch.object(Path, "read_text", side_effect=FileNotFoundError):
            self.assertTrue(process_is_gone_or_zombie(123))
        with mock.patch.object(Path, "read_text", side_effect=ProcessLookupError):
            self.assertTrue(process_is_gone_or_zombie(123))
        with mock.patch.object(
            Path, "read_text", return_value="123 (name with spaces) Z 1 2 3"
        ):
            self.assertTrue(process_is_gone_or_zombie(123))
        with mock.patch.object(
            Path, "read_text", return_value="123 (name with spaces) S 1 2 3"
        ):
            self.assertFalse(process_is_gone_or_zombie(123))
        with mock.patch.object(Path, "read_text", return_value="malformed"):
            with self.assertRaisesRegex(AssertionError, "Malformed"):
                process_is_gone_or_zombie(123)

    def assert_rule(self, rule: str, callback) -> None:
        with self.assertRaisesRegex(contract.ContractError, f"rule={rule}"):
            callback()

    def test_positive_document_fixture(self) -> None:
        raw = (FIXTURES / "positive/module.md").read_bytes()
        contract.parse_module_document(
            raw, "memory", contract.DocumentProjection(("src/modules/memory/*.c",), ())
        )

    def test_protected_diagnostics_are_complete(self) -> None:
        with self.assertRaises(contract.ContractError) as captured:
            with contract.diagnostic_context("memory", "a" * 40, "docs/modules/memory.md"):
                contract.fail("document-test", "bad record", line=7)
        message = str(captured.exception)
        for field in (
            "rule=document-test", "module=memory", f"candidate={'a' * 40}",
            "path=docs/modules/memory.md", "line=7", "expected=document-test-contract",
            "actual=bad record",
        ):
            self.assertIn(field, message)

    def test_workflow_activation_is_dormant_then_fail_closed(self) -> None:
        workflow = (
            REPO_ROOT / ".github/workflows/module-doc-attestation-trigger.yml"
        ).read_text(encoding="utf-8")
        self.assertIn("if: ${{ vars.MODULE_DOC_ATTESTATION_ENABLED == 'true' }}", workflow)
        self.assertIn("MODULE_DOC_VERIFIER_SPKI", workflow)
        self.assertIn("--pinnedpubkey", workflow)
        self.assertNotIn("actions/checkout", workflow)
        inventory_workflow = (
            REPO_ROOT / ".github/workflows/module-inventory.yml"
        ).read_text(encoding="utf-8")
        for executable in ("ssh-keygen", "ssh-agent", "ssh-add"):
            self.assertIn(f"command -v {executable}", inventory_workflow)

    def test_document_negative_fixtures_have_stable_rules(self) -> None:
        positive = (FIXTURES / "positive/module.md").read_bytes()
        cases = {
            "unicode": (positive.replace(b"Clear", "Cl\u00e9ar".encode()), "document-byte-domain"),
            "section-order": (positive.replace(
                b"## Purpose and non-goals", b"## Classification and lifecycle", 1
            ), "document-section-cardinality"),
            "placeholder": (positive.replace(b"Clear", b"TODO", 1), "document-placeholder"),
            "projection": (positive.replace(b"Sources: src/modules/memory/*.c", b"Sources: none", 1), "document-projection"),
            "record-order": (positive.replace(
                b"Clear module behavior documents concrete contracts dependencies activation failure handling diagnostics security privacy compatibility migration ownership and removal rules for operators maintainers reviewers implementers callers services deployments tests workflows and supported user journeys while identifying current evidence boundaries known gaps operational consequences stable interfaces data handling readiness states and safe extension expectations.\n- Evidence: path:scripts/module_doc_contract.py#L1",
                b"- Evidence: path:scripts/module_doc_contract.py#L1\nClear module behavior documents concrete contracts dependencies activation failure handling diagnostics security privacy compatibility migration ownership and removal rules for operators maintainers reviewers implementers callers services deployments tests workflows and supported user journeys while identifying current evidence boundaries known gaps operational consequences stable interfaces data handling readiness states and safe extension expectations.",
                1,
            ), "document-record-order"),
            "none-consecutive": (positive.replace(
                b"State: present\nSources:",
                b"State: none\nReason: Concrete reason.\n\nImplication: Concrete implication.\nEvidence: path:scripts/module_doc_contract.py#L1\nSources:",
                1,
            ), "document-none-block"),
            "preamble": (positive.replace(
                b"# memory module\n\n", b"# memory module\n\nUnparsed preamble.\n", 1
            ), "document-preamble"),
            "duplicate-h1": (positive.replace(
                b"# memory module\n\n", b"# memory module\n\n# memory module\n", 1
            ), "document-preamble"),
            "extra-h1-blank": (positive.replace(
                b"# memory module\n\n", b"# memory module\n\n\n", 1
            ), "document-preamble"),
        }
        for name, (raw, rule) in cases.items():
            with self.subTest(name=name):
                self.assert_rule(rule, lambda raw=raw: contract.parse_module_document(
                    raw, "memory", contract.DocumentProjection(("src/modules/memory/*.c",), ())
                ))
        none = positive.replace(
            b"State: present\nSources:",
            b"State: none\nReason: Concrete reason.\nImplication: Concrete implication.\nEvidence: path:scripts/module_doc_contract.py#L1\nSources:",
            1,
        )
        contract.parse_module_document(
            none, "memory", contract.DocumentProjection(("src/modules/memory/*.c",), ())
        )
        unknown_module = positive.replace(
            b"path:scripts/module_doc_contract.py#L1", b"module:absent", 1
        )
        def resolve(reference: str) -> None:
            if reference == "module:absent":
                contract.fail("document-evidence-module", "unknown evidence module")
        self.assert_rule("document-evidence-module", lambda: contract.parse_module_document(
            unknown_module, "memory",
            contract.DocumentProjection(("src/modules/memory/*.c",), ()),
            resolve_reference=resolve,
        ))
        evidence = b"- Evidence: path:scripts/module_doc_contract.py#L1\n"
        excessive = positive.replace(evidence, evidence * 52, 1)
        self.assert_rule("document-evidence-budget", lambda: contract.parse_module_document(
            excessive, "memory",
            contract.DocumentProjection(("src/modules/memory/*.c",), ()),
        ))

    def test_profile_is_provider_neutral_and_closed(self) -> None:
        profile = oidc_profile()
        self.assertEqual(contract.validate_oidc_profile(profile), profile)
        example_raw = (
            REPO_ROOT / ".github/module-doc-attestation/issuer-profile.example.json"
        ).read_bytes()
        example = contract.strict_json_bytes(example_raw)
        self.assertEqual(contract.validate_oidc_profile(example), example)
        self.assertNotIn(b"github", example_raw.lower())
        trailing_issuer = json.loads(json.dumps(profile))
        trailing_issuer["issuer"] += "/"
        self.assertEqual(contract.validate_oidc_profile(trailing_issuer), trailing_issuer)
        unknown = dict(profile)
        unknown["provider"] = "github"
        self.assert_rule("oidc-profile-shape", lambda: contract.validate_oidc_profile(unknown))
        claimed_trigger = json.loads(json.dumps(profile))
        claimed_trigger["claim_mappings"]["trigger_check_identity"] = "ci_trigger_check"
        self.assert_rule(
            "oidc-claim-mappings",
            lambda: contract.validate_oidc_profile(claimed_trigger),
        )
        no_pin = json.loads(json.dumps(profile))
        no_pin["jwks"]["tls_spki_sha256"] = []
        self.assert_rule("oidc-jwks-pins", lambda: contract.validate_oidc_profile(no_pin))
        missing_predicate = json.loads(json.dumps(profile))
        del missing_predicate["predicates"]["workflow_revision"]
        self.assert_rule("oidc-predicates", lambda: contract.validate_oidc_profile(missing_predicate))
        self.assert_rule("json-size", lambda: contract.strict_json_bytes(
            b" " * (contract.MAX_JSON_BYTES + 1)
        ))
        self.assert_rule("json-integer-range", lambda: contract.strict_json_bytes(
            b'{"value":123456789012345678901}'
        ))
        malformed = (
            ("issuer", "https://user@example.invalid", "oidc-issuer"),
            ("issuer", "https:///missing-host", "oidc-issuer"),
            ("issuer", "https://issuer.example.invalid/#fragment", "oidc-issuer"),
            ("issuer", "https://Issuer.example.invalid", "oidc-issuer"),
            ("issuer", "https://issuer.example.invalid\\evil", "oidc-issuer"),
            ("jwks.uri", "https://issuer.example.invalid/jwks#fragment", "oidc-jwks-uri"),
            ("repository_api.base_url", "https://repository.example.invalid/api/", "repository-api-url"),
        )
        for field, value, rule in malformed:
            invalid = json.loads(json.dumps(profile))
            if "." in field:
                parent, child = field.split(".")
                invalid[parent][child] = value
            else:
                invalid[field] = value
            with self.subTest(url_field=field, value=value):
                self.assert_rule(rule, lambda invalid=invalid: contract.validate_oidc_profile(invalid))

    def test_verified_claim_normalization_and_binding(self) -> None:
        profile = contract.validate_oidc_profile(oidc_profile())
        token_claims = claims(profile)
        workload = contract.normalize_verified_oidc_claims(token_claims, profile, wall_time=1_800_000_000)
        self.assertNotIn("ci_trigger_check_identity", token_claims)
        self.assertEqual(
            workload["trigger_check_identity"],
            contract.derive_trigger_check_identity(
                workload["repository_identity"],
                workload["run_identity"],
                workload["attempt"],
            ),
        )
        self.assertRegex(workload["trigger_check_identity"], r"^sha256:[0-9a-f]{64}$")
        target = contract.validate_candidate_target({
            "schema": "aimee.candidate-target.v1",
            "repository_identity": workload["repository_identity"],
            "pull_request": 1708,
            "protected_base_revision": "a" * 40,
            "candidate_revision": "b" * 40,
            "base_ref": "feature/core-modularization",
            "head_ref": "slice/module-doc-contracts",
            "workflow_identity": workload["workflow_identity"],
            "workflow_revision": workload["workflow_revision"],
            "run_identity": workload["run_identity"],
            "attempt": workload["attempt"],
            "trigger_check_identity": workload["trigger_check_identity"],
        })
        contract.bind_workload_to_target(workload, target, pull_request=1708)
        self.assertRegex(contract.replay_binding(workload, target), r"^[0-9a-f]{64}$")
        self.assertRegex(contract.replay_token_identity(workload), r"^[0-9a-f]{64}$")
        original_binding = contract.replay_binding(workload, target)
        for field, value in workload.items():
            changed = dict(workload)
            changed[field] = value + 1 if type(value) is int else f"{value}-changed"
            with self.subTest(replay_workload_field=field):
                self.assertNotEqual(original_binding, contract.replay_binding(changed, target))
        for field, value in target.items():
            changed = dict(target)
            changed[field] = value + 1 if type(value) is int else f"{value}-changed"
            with self.subTest(replay_target_field=field):
                self.assertNotEqual(original_binding, contract.replay_binding(workload, changed))
        left = dict(workload, subject="a\0b", audience="c")
        right = dict(workload, subject="a", audience="b\0c")
        self.assertNotEqual(
            contract.replay_binding(left, target), contract.replay_binding(right, target)
        )
        for field, rule in (("run_identity", "target-run-binding"), ("attempt", "target-run-binding")):
            mismatch = dict(target)
            mismatch[field] = "different"
            with self.subTest(binding=field):
                self.assert_rule(rule, lambda mismatch=mismatch: contract.bind_workload_to_target(
                    workload, mismatch, pull_request=1708
                ))
        bad = dict(token_claims)
        bad["iss"] = "https://other.example.invalid"
        self.assert_rule("oidc-issuer-mismatch", lambda: contract.normalize_verified_oidc_claims(
            bad, profile, wall_time=1_800_000_000
        ))
        stale = dict(token_claims)
        stale["iat"] = 1_799_999_000
        self.assert_rule("oidc-issued-at", lambda: contract.normalize_verified_oidc_claims(
            stale, profile, wall_time=1_800_000_000
        ))
        expires_now = dict(token_claims)
        expires_now["exp"] = 1_800_000_000
        self.assert_rule("oidc-validity-window", lambda: contract.normalize_verified_oidc_claims(
            expires_now, profile, wall_time=1_800_000_000
        ))
        starts_now = dict(token_claims)
        starts_now["nbf"] = 1_800_000_000
        contract.normalize_verified_oidc_claims(
            starts_now, profile, wall_time=1_800_000_000
        )
        bad_attempt = dict(token_claims)
        bad_attempt[profile["claim_mappings"]["attempt"]] = "01"
        self.assert_rule("oidc-attempt", lambda: contract.normalize_verified_oidc_claims(
            bad_attempt, profile, wall_time=1_800_000_000
        ))

    def test_trigger_identity_is_derived_from_the_complete_run_attempt(self) -> None:
        identity = contract.derive_trigger_check_identity("repository-17", "run-42", "2")
        self.assertEqual(
            identity,
            contract.derive_trigger_check_identity("repository-17", "run-42", "2"),
        )
        for repository, run, attempt in (
            ("repository-18", "run-42", "2"),
            ("repository-17", "run-43", "2"),
            ("repository-17", "run-42", "3"),
        ):
            with self.subTest(repository=repository, run=run, attempt=attempt):
                self.assertNotEqual(
                    identity,
                    contract.derive_trigger_check_identity(repository, run, attempt),
                )
        self.assertEqual(
            contract.derive_trigger_check_identity(
                "repository-é", 'run-"42"', "2"
            ),
            "sha256:d56311b051331f765ff4651d87d88ce9f760aaed7443bd635d0a414d40771821",
        )
        for attempt in ("0", "01", "-1", ""):
            with self.subTest(attempt=attempt):
                self.assert_rule(
                    "oidc-trigger-identity",
                    lambda attempt=attempt: contract.derive_trigger_check_identity(
                        "repository-17", "run-42", attempt
                    ),
                )

    def test_trigger_request_has_no_candidate_coordinates(self) -> None:
        request = {"schema": "aimee.module-doc-trigger.v1", "pull_request": 17, "oidc_token": "a.b.c"}
        self.assertEqual(contract.validate_trigger_request(request), (17, "a.b.c"))
        request["candidate_sha"] = "f" * 40
        self.assert_rule("trigger-request-shape", lambda: contract.validate_trigger_request(request))

    def test_descriptor_v2_metadata_is_closed_and_deferred(self) -> None:
        descriptor = {
            "descriptor_version": 2,
            "id": "memory",
            "dependencies": ["config"],
            "runtime_toggle": {"supported": False},
            "docs": "docs/modules/memory.md",
            "sources": ["src/modules/memory/*.c", "src/modules/memory/private/*.h"],
            "public_headers": [],
            "surfaces": {"routes": [], "commands": [], "protocols": [], "stages": []},
        }
        self.assertEqual(contract.validate_v2_metadata(descriptor, optional=False), descriptor)
        claimed = json.loads(json.dumps(descriptor))
        claimed["surfaces"]["routes"] = ["GET /v1/memory"]
        self.assert_rule("v2-surfaces-deferred", lambda: contract.validate_v2_metadata(claimed, optional=False))
        crossed = json.loads(json.dumps(descriptor))
        crossed["sources"] = ["src/modules/config/*.c"]
        self.assert_rule("v2-source-pattern", lambda: contract.validate_v2_metadata(crossed, optional=False))
        bad_dependencies = json.loads(json.dumps(descriptor))
        bad_dependencies["dependencies"] = ["unknown", "config"]
        self.assert_rule("v2-dependencies", lambda: contract.validate_v2_metadata(
            bad_dependencies, optional=False, known_ids={"config", "memory"}
        ))
        bad_runtime = json.loads(json.dumps(descriptor))
        bad_runtime["runtime_toggle"] = {"supported": "false"}
        self.assert_rule("v2-runtime-toggle", lambda: contract.validate_v2_metadata(
            bad_runtime, optional=False
        ))

    def test_git_reader_uses_immutable_mode_checked_objects(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            subprocess.run(["git", "init", "-q"], cwd=repo, check=True)
            subprocess.run(["git", "config", "user.email", "fixture@example.invalid"], cwd=repo, check=True)
            subprocess.run(["git", "config", "user.name", "Fixture"], cwd=repo, check=True)
            evidence = repo / "evidence.txt"
            evidence.write_text("one\ntwo\n", encoding="ascii")
            subprocess.run(["git", "add", "evidence.txt"], cwd=repo, check=True)
            subprocess.run(["git", "commit", "-qm", "fixture"], cwd=repo, check=True)
            commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()
            reader = contract.GitBlobReader(repo, commit)
            self.assertEqual(reader.read_blob("evidence.txt"), b"one\ntwo\n")
            operations = reader.budget.git_operations
            reader.resolve_reference("path:evidence.txt#L2")
            reader.resolve_reference("path:evidence.txt#L2")
            self.assertEqual(reader.budget.git_operations, operations)
            self.assert_rule("document-evidence-line", lambda: reader.resolve_reference("path:evidence.txt#L3"))
            evidence.unlink()
            evidence.symlink_to("target")
            subprocess.run(["git", "add", "evidence.txt"], cwd=repo, check=True)
            subprocess.run(["git", "commit", "-qm", "symlink"], cwd=repo, check=True)
            symlink_commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()
            symlink_reader = contract.GitBlobReader(repo, symlink_commit)
            self.assert_rule("git-mode", lambda: symlink_reader.read_blob("evidence.txt"))

    def test_git_operation_timeout_is_a_contract_failure(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            subprocess.run(["git", "init", "-q"], cwd=repo, check=True)
            previous = contract.GIT_OPERATION_TIMEOUT_SECONDS
            contract.GIT_OPERATION_TIMEOUT_SECONDS = 0
            try:
                self.assert_rule(
                    "git-timeout", lambda: contract.GitBlobReader(repo, "a" * 40)
                )
            finally:
                contract.GIT_OPERATION_TIMEOUT_SECONDS = previous

    def test_decision_budget_boundaries_fail_closed(self) -> None:
        budget = contract.GitDecisionBudget()
        budget.git_operations = contract.MAX_DECISION_GIT_OPERATIONS
        self.assert_rule("git-operation-budget", budget.consume_git_operation)
        budget.evidence_references = contract.MAX_DECISION_EVIDENCE_REFERENCES
        self.assert_rule("document-evidence-budget", budget.consume_evidence_reference)
        budget.distinct_blob_bytes = contract.MAX_DECISION_DISTINCT_BLOB_BYTES
        self.assert_rule(
            "git-byte-budget", lambda: budget.cache_blob(("repo", "a" * 40, "x"), b"x")
        )

    def test_snapshot_close_drains_leases_and_serializes_accounting(self) -> None:
        budget = contract.GitDecisionBudget()
        started = threading.Event()
        release = threading.Event()
        closed = threading.Event()

        def operation() -> None:
            with budget.operation():
                started.set()
                release.wait(timeout=2)

        worker = threading.Thread(target=operation)
        worker.start()
        self.assertTrue(started.wait(timeout=1))

        def close_budget() -> None:
            budget.close()
            closed.set()

        closer = threading.Thread(target=close_budget)
        closer.start()
        self.assertFalse(closed.wait(timeout=0.05))
        deadline = time.monotonic() + 1
        while True:
            try:
                with budget.operation():
                    pass
            except contract.ContractError as exc:
                self.assertIn("rule=git-snapshot-lifetime", str(exc))
                break
            self.assertLess(time.monotonic(), deadline)
        release.set()
        worker.join(timeout=1)
        closer.join(timeout=1)
        self.assertFalse(worker.is_alive())
        self.assertFalse(closer.is_alive())
        self.assertTrue(closed.is_set())

        concurrent = contract.GitDecisionBudget()
        concurrent.git_operations = contract.MAX_DECISION_GIT_OPERATIONS - 5
        outcomes: list[str] = []
        outcome_lock = threading.Lock()

        def consume() -> None:
            try:
                concurrent.consume_git_operation()
                result = "accepted"
            except contract.ContractError:
                result = "rejected"
            with outcome_lock:
                outcomes.append(result)

        threads = [threading.Thread(target=consume) for _ in range(10)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(timeout=1)
        self.assertEqual(outcomes.count("accepted"), 5)
        self.assertEqual(outcomes.count("rejected"), 5)

        cached = contract.GitDecisionBudget()
        cache_threads = [
            threading.Thread(
                target=lambda: cached.cache_blob(("repo", "a" * 40, "x"), b"shared")
            )
            for _ in range(10)
        ]
        for thread in cache_threads:
            thread.start()
        for thread in cache_threads:
            thread.join(timeout=1)
        self.assertEqual(cached.distinct_blob_bytes, len(b"shared"))

    def test_decision_deadline_kills_git_and_prevents_late_spawn(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            subprocess.run(["git", "init", "-q"], cwd=repo, check=True)
            subprocess.run(["git", "config", "user.email", "fixture@example.invalid"], cwd=repo, check=True)
            subprocess.run(["git", "config", "user.name", "Fixture"], cwd=repo, check=True)
            (repo / "x").write_text("x\n", encoding="ascii")
            subprocess.run(["git", "add", "x"], cwd=repo, check=True)
            subprocess.run(["git", "commit", "-qm", "fixture"], cwd=repo, check=True)
            commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()
            reader = contract.GitBlobReader(repo, commit)
            real_popen = contract.subprocess.Popen
            reader.budget.git_deadline = time.monotonic() + 0.02
            def delayed_popen(*args, **kwargs):
                time.sleep(0.04)
                return real_popen(*args, **kwargs)
            with mock.patch.object(
                contract.subprocess, "Popen", side_effect=delayed_popen
            ):
                self.assert_rule("git-timeout", lambda: reader._git_bounded(
                    1024, "rev-parse", "HEAD"
                ))
            reader.budget.git_deadline = time.monotonic() + 0.1
            started = time.monotonic()
            self.assert_rule("git-timeout", lambda: reader._git_bounded(
                1024, "-c", "alias.wait=!sleep 2", "wait"
            ))
            self.assertLess(time.monotonic() - started, 1.0)
            child_pid_path = repo / "orphan.pid"
            reader.budget.git_deadline = time.monotonic() + 0.1
            self.assert_rule("git-timeout", lambda: reader._git_bounded(
                1024, "-c",
                f"alias.orphan=!sh -c 'sleep 600 & echo $! > {child_pid_path}'",
                "orphan",
            ))
            child_pid = int(child_pid_path.read_text(encoding="ascii").strip())
            # The orphan MUST outlive the observation window, or this assertion is
            # vacuous: with `sleep 5` the child exited on its own at five seconds,
            # so any wait longer than that passed whether or not the process-group
            # kill did anything. That is why the wait was ~1s -- and a 1s budget is
            # not enough for the reap to be observable on a loaded runner, which
            # failed CI here with the kill working correctly ("Git descendant
            # survived process-group timeout", 2026-08-11).
            #
            # sleep 600 makes the two failure modes distinguishable: if the kill
            # works the child is gone in milliseconds, and if it does not the child
            # is still there when the deadline expires, so waiting longer costs
            # nothing on success and cannot manufacture one.
            # Capture the identity BEFORE waiting. After this point the pid may
            # be recycled, and comparing start times is what tells "our child is
            # gone" apart from "that number now belongs to someone else".
            child_start = process_starttime(child_pid)

            # ONE evaluation decides the outcome. The previous shape asked twice
            # -- once to break the loop, once to set the result -- so a pid
            # recycled between the two answered "gone" and then "alive", failing
            # a kill that had worked, in well under a second. That is what this
            # test did in CI on 2026-08-13.
            reaped = False
            deadline = time.monotonic() + 30.0
            while time.monotonic() < deadline:
                if process_is_gone_or_zombie(child_pid, child_start):
                    reaped = True
                    break
                time.sleep(0.01)
            if not reaped:
                # Never leave a ten-minute sleep behind for the next test or the
                # runner to inherit -- the failure is the assertion below, not a
                # leaked process.
                with contextlib.suppress(ProcessLookupError, PermissionError):
                    os.kill(child_pid, signal.SIGKILL)
            self.assertTrue(
                reaped,
                "Git descendant survived process-group timeout: "
                + describe_process(child_pid),
            )
            reader.budget.git_deadline = time.monotonic() - 1
            with mock.patch.object(contract.subprocess, "Popen") as popen:
                self.assert_rule(
                    "git-timeout", lambda: reader._git_bounded(1024, "rev-parse", "HEAD")
                )
                popen.assert_not_called()
            self.assertGreater(
                contract.GitDecisionBudget().git_deadline, time.monotonic()
            )

    def test_git_output_ceilings_and_cross_reader_cache(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            subprocess.run(["git", "init", "-q"], cwd=repo, check=True)
            subprocess.run(["git", "config", "user.email", "fixture@example.invalid"], cwd=repo, check=True)
            subprocess.run(["git", "config", "user.name", "Fixture"], cwd=repo, check=True)
            (repo / "evidence.txt").write_text("shared\n", encoding="ascii")
            subprocess.run(["git", "add", "evidence.txt"], cwd=repo, check=True)
            subprocess.run(["git", "commit", "-qm", "fixture"], cwd=repo, check=True)
            commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()
            budget = contract.GitDecisionBudget()
            token = contract._ACTIVE_GIT_BUDGET.set(budget)
            try:
                first = contract.GitBlobReader(repo, commit)
                second = contract.GitBlobReader(repo, commit)
                self.assertEqual(first.read_blob("evidence.txt"), b"shared\n")
                operations = budget.git_operations
                self.assertEqual(second.read_blob("evidence.txt"), b"shared\n")
                self.assertEqual(budget.git_operations, operations)
                self.assert_rule(
                    "git-output-size", lambda: first._git_bounded(1, "rev-parse", "HEAD")
                )
                previous = contract.MAX_GIT_STDERR_BYTES
                contract.MAX_GIT_STDERR_BYTES = 1
                try:
                    self.assert_rule(
                        "git-stderr-size", lambda: first._git_bounded(1024, "invalid-command")
                    )
                finally:
                    contract.MAX_GIT_STDERR_BYTES = previous
            finally:
                contract._ACTIVE_GIT_BUDGET.reset(token)

    def test_git_blob_size_boundary_precedes_read(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            subprocess.run(["git", "init", "-q"], cwd=repo, check=True)
            subprocess.run(["git", "config", "user.email", "fixture@example.invalid"], cwd=repo, check=True)
            subprocess.run(["git", "config", "user.name", "Fixture"], cwd=repo, check=True)
            path = repo / "sized.bin"
            path.write_bytes(b"1234")
            subprocess.run(["git", "add", "sized.bin"], cwd=repo, check=True)
            subprocess.run(["git", "commit", "-qm", "four"], cwd=repo, check=True)
            commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()
            self.assertEqual(contract.GitBlobReader(repo, commit).read_blob("sized.bin", max_bytes=4), b"1234")
            path.write_bytes(b"12345")
            subprocess.run(["git", "add", "sized.bin"], cwd=repo, check=True)
            subprocess.run(["git", "commit", "-qm", "five"], cwd=repo, check=True)
            commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()
            self.assert_rule("git-object-size", lambda: contract.GitBlobReader(
                repo, commit
            ).read_blob("sized.bin", max_bytes=4))

    def test_git_tree_entry_ceiling(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            subprocess.run(["git", "init", "-q"], cwd=repo, check=True)
            subprocess.run(["git", "config", "user.email", "fixture@example.invalid"], cwd=repo, check=True)
            subprocess.run(["git", "config", "user.name", "Fixture"], cwd=repo, check=True)
            directory = repo / "many"
            directory.mkdir()
            for index in range(3):
                (directory / f"{index}.txt").write_text("x\n", encoding="ascii")
            subprocess.run(["git", "add", "many"], cwd=repo, check=True)
            subprocess.run(["git", "commit", "-qm", "many"], cwd=repo, check=True)
            commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()
            previous = contract.MAX_TREE_ENTRIES
            contract.MAX_TREE_ENTRIES = 2
            try:
                self.assert_rule("git-tree-entries", lambda: contract.GitBlobReader(
                    repo, commit
                ).list_directory("many"))
            finally:
                contract.MAX_TREE_ENTRIES = previous

    def test_external_decision_orders_crypto_resolution_and_git_reads(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            subprocess.run(["git", "init", "-q"], cwd=repo, check=True)
            subprocess.run(["git", "config", "user.email", "fixture@example.invalid"], cwd=repo, check=True)
            subprocess.run(["git", "config", "user.name", "Fixture"], cwd=repo, check=True)
            (repo / "evidence.txt").write_text("governed\n", encoding="ascii")
            subprocess.run(["git", "add", "evidence.txt"], cwd=repo, check=True)
            subprocess.run(["git", "commit", "-qm", "fixture"], cwd=repo, check=True)
            commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()
            profile = oidc_profile()
            token_claims = claims(profile)
            events: list[str] = []
            retained_readers = []
            retained_validations = []

            def verify(token: str, selected: dict[str, object]) -> dict[str, object]:
                self.assertEqual(token, "signed.jwt.value")
                self.assertEqual(selected, profile)
                events.append("crypto")
                return token_claims

            def target_for(workload: dict[str, object], pull_request: int):
                return {
                    "schema": "aimee.candidate-target.v1",
                    "repository_identity": workload["repository_identity"],
                    "pull_request": pull_request,
                    "protected_base_revision": commit,
                    "candidate_revision": commit,
                    "base_ref": "feature/core-modularization",
                    "head_ref": "slice/module-doc-contracts",
                    "workflow_identity": workload["workflow_identity"],
                    "workflow_revision": workload["workflow_revision"],
                    "run_identity": workload["run_identity"],
                    "attempt": workload["attempt"],
                    "trigger_check_identity": workload["trigger_check_identity"],
                }

            @contextmanager
            def owned_candidate(target):
                events.append("enter")
                try:
                    yield contract.ResolvedCandidate(target, repo)
                finally:
                    events.append("exit")

            def resolve(workload: dict[str, object], pull_request: int):
                self.assertEqual(events, ["crypto"])
                events.append("resolve")
                return owned_candidate(target_for(workload, pull_request))

            def validate(validation, workload, target) -> str:
                self.assertEqual(events, ["crypto", "resolve", "enter"])
                events.append("candidate")
                candidate = validation.candidate
                base = validation.base
                self.assertFalse(hasattr(validation, "reader"))
                for hidden in ("repository", "budget", "blobs"):
                    self.assertFalse(hasattr(candidate, hidden))
                self.assertEqual(candidate.read_blob("evidence.txt"), b"governed\n")
                self.assertEqual(base.read_blob("evidence.txt"), b"governed\n")
                retained_readers.append(candidate)
                retained_validations.append(validation)
                workload["repository_identity"] = "callback-mutation"
                target["candidate_revision"] = "c" * 40
                return "All governed artifacts passed."

            request = json.dumps({
                "schema": "aimee.module-doc-trigger.v1",
                "pull_request": 1708,
                "oidc_token": "signed.jwt.value",
            }, separators=(",", ":")).encode("ascii")
            profile_raw = (
                json.dumps(profile, separators=(",", ":")) + "\n"
            ).encode("ascii")

            def decide(*, resolver=resolve, validator=validate):
                return contract.evaluate_trigger(
                    request,
                    profile_raw,
                    wall_time=1_800_000_000,
                    verify_jwt=verify,
                    resolve_target=resolver,
                    validate_candidate=validator,
                )

            decision = decide()
            self.assertEqual(
                events, ["crypto", "resolve", "enter", "candidate", "exit"]
            )

            events.clear()
            read_started = threading.Event()
            release_read = threading.Event()
            reader_threads = []
            reader_errors = []
            original_read_blob = contract.GitBlobReader.read_blob

            def blocking_read_blob(reader, path, *, max_bytes=contract.MAX_GOVERNED_BLOB_BYTES):
                read_started.set()
                release_read.wait(timeout=2)
                try:
                    return original_read_blob(reader, path, max_bytes=max_bytes)
                finally:
                    events.append("read-finished")

            def concurrent_validate(validation, workload, target):
                events.append("candidate")

                def read() -> None:
                    try:
                        validation.candidate.read_blob("evidence.txt")
                    except BaseException as exc:
                        reader_errors.append(exc)

                thread = threading.Thread(target=read)
                reader_threads.append(thread)
                thread.start()
                self.assertTrue(read_started.wait(timeout=1))
                threading.Timer(0.05, release_read.set).start()
                return "Concurrent read drained before cleanup."

            with mock.patch.object(
                contract.GitBlobReader, "read_blob", new=blocking_read_blob
            ):
                decide(validator=concurrent_validate)
            for thread in reader_threads:
                thread.join(timeout=1)
            self.assertEqual(reader_errors, [])
            self.assertEqual(
                events,
                [
                    "crypto", "resolve", "enter", "candidate", "read-finished", "exit"
                ],
            )

            events.clear()

            def mismatched_resolve(workload: dict[str, object], pull_request: int):
                target = target_for(workload, pull_request)
                target["repository_identity"] = "different-repository"
                return owned_candidate(target)

            with self.assertRaisesRegex(
                contract.ContractError, "rule=target-repository-binding"
            ):
                decide(resolver=mismatched_resolve)
            self.assertEqual(events, ["crypto", "enter", "exit"])

            events.clear()

            def malformed_resolve(workload: dict[str, object], pull_request: int):
                target = target_for(workload, pull_request)
                target["candidate_revision"] = "abbreviated"
                return owned_candidate(target)

            with self.assertRaisesRegex(
                contract.ContractError, "rule=candidate-target-revision"
            ):
                decide(resolver=malformed_resolve)
            self.assertEqual(events, ["crypto", "enter", "exit"])

            events.clear()

            def explode(validation, workload, target) -> str:
                events.append("candidate")
                raise RuntimeError("unexpected validator failure")

            class AdversarialContext:
                def __init__(self, target, *, cleanup_error=False):
                    self.target = target
                    self.cleanup_error = cleanup_error

                def __enter__(self):
                    events.append("enter")
                    return contract.ResolvedCandidate(self.target, repo)

                def __exit__(self, exc_type, exc, traceback):
                    events.append("exit")
                    if self.cleanup_error:
                        raise RuntimeError("cleanup replacement")
                    return True

            def adversarial_resolve(workload, pull_request):
                return AdversarialContext(target_for(workload, pull_request))

            with self.assertRaisesRegex(RuntimeError, "unexpected validator failure"):
                decide(resolver=adversarial_resolve, validator=explode)
            self.assertEqual(events, ["crypto", "enter", "candidate", "exit"])

            events.clear()

            def replacing_resolve(workload, pull_request):
                return AdversarialContext(
                    target_for(workload, pull_request), cleanup_error=True
                )

            with self.assertRaisesRegex(RuntimeError, "unexpected validator failure"):
                decide(resolver=replacing_resolve, validator=explode)
            self.assertEqual(events, ["crypto", "enter", "candidate", "exit"])

            events.clear()

            def accept(validation, workload, target):
                events.append("candidate")
                return "Accepted before cleanup."

            self.assert_rule(
                "resolver-cleanup",
                lambda: decide(resolver=replacing_resolve, validator=accept),
            )
            self.assertEqual(events, ["crypto", "enter", "candidate", "exit"])

            events.clear()
            partial = repo / "partial-acquisition"

            def failed_acquisition(workload, pull_request):
                @contextmanager
                def acquisition():
                    partial.mkdir()
                    try:
                        raise RuntimeError("acquisition failed")
                        yield
                    finally:
                        partial.rmdir()

                return acquisition()

            with self.assertRaisesRegex(RuntimeError, "acquisition failed"):
                decide(resolver=failed_acquisition)
            self.assertFalse(partial.exists())
            self.assert_rule(
                "git-snapshot-lifetime",
                lambda: retained_readers[0].read_blob("evidence.txt"),
            )
            self.assert_rule(
                "git-snapshot-lifetime",
                lambda: retained_validations[0].base.read_blob("evidence.txt"),
            )
            self.assertFalse(hasattr(retained_validations[0], "reader"))
            for name, callback in (
                ("commit", lambda: retained_readers[0].commit),
                ("list", lambda: retained_readers[0].list_directory(".")),
                (
                    "reference",
                    lambda: retained_readers[0].resolve_reference(
                        "path:evidence.txt#L1"
                    ),
                ),
                (
                    "source-pattern",
                    lambda: retained_readers[0].expand_source_pattern(
                        "src/modules/memory/*.c", "memory"
                    ),
                ),
            ):
                with self.subTest(revoked_operation=name):
                    self.assert_rule("git-snapshot-lifetime", callback)
            self.assertEqual(decision.publisher_result["candidate_revision"], commit)
            self.assertEqual(
                decision.workload["repository_identity"], "repository-17"
            )
            self.assertRegex(decision.token_replay_key, r"^[0-9a-f]{64}$")
            self.assertRegex(decision.decision_replay_key, r"^[0-9a-f]{64}$")
            events.clear()

            def reject(validation, workload, target) -> str:
                events.append("candidate")
                contract.fail("candidate-rejected", "fixture rejection")

            rejected = decide(validator=reject)
            self.assertEqual(
                events, ["crypto", "resolve", "enter", "candidate", "exit"]
            )
            self.assertEqual(rejected.publisher_result["conclusion"], "failure")
            self.assertIn("rule=candidate-rejected", rejected.publisher_result["summary"])

            events.clear()
            rejected = decide(resolver=replacing_resolve, validator=reject)
            self.assertEqual(events, ["crypto", "enter", "candidate", "exit"])
            self.assertEqual(rejected.publisher_result["conclusion"], "failure")
            self.assertIn("rule=candidate-rejected", rejected.publisher_result["summary"])

            events.clear()

            with self.assertRaisesRegex(RuntimeError, "unexpected validator failure"):
                decide(validator=explode)
            self.assertEqual(
                events, ["crypto", "resolve", "enter", "candidate", "exit"]
            )

    def test_atomic_candidate_validator_with_two_real_signers(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            subprocess.run(["git", "init", "-q"], cwd=repo, check=True)
            subprocess.run(["git", "config", "user.email", "fixture@example.invalid"], cwd=repo, check=True)
            subprocess.run(["git", "config", "user.name", "Fixture"], cwd=repo, check=True)
            modules = {"config": False, "runtime-web": True}
            for module_id, optional in modules.items():
                path = repo / "src/modules" / module_id / "module.yaml"
                path.parent.mkdir(parents=True)
                base_descriptor = {
                    "descriptor_version": 1, "id": module_id, "dependencies": [],
                    "runtime_toggle": {"supported": optional},
                }
                if optional:
                    base_descriptor["enabled_by_default"] = True
                path.write_text(json.dumps(base_descriptor, indent=2) + "\n", encoding="ascii")
            (repo / "evidence.txt").write_text("evidence\n", encoding="ascii")
            subprocess.run(["git", "add", "."], cwd=repo, check=True)
            subprocess.run(["git", "commit", "-qm", "base"], cwd=repo, check=True)
            base_commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()

            key_paths = {"owner@example": repo / "owner", "reviewer@example": repo / "reviewer"}
            public_keys: dict[str, str] = {}
            for identity, key_path in key_paths.items():
                subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", str(key_path)], check=True)
                public_keys[identity] = " ".join(key_path.with_suffix(".pub").read_text(encoding="ascii").split()[:2])
            trust = {
                "schema": "aimee.module-doc-trust.v1", "epoch": 1,
                "identities": [
                    {"identity": "owner@example", "role": "owner", "public_key": public_keys["owner@example"],
                     "not_before": "2026-01-01T00:00:00Z", "not_after": "2028-01-01T00:00:00Z", "revoked_at": None},
                    {"identity": "reviewer@example", "role": "reviewer", "public_key": public_keys["reviewer@example"],
                     "not_before": "2026-01-01T00:00:00Z", "not_after": "2028-01-01T00:00:00Z", "revoked_at": None},
                ],
            }
            at = datetime(2027, 1, 1, tzinfo=timezone.utc)
            trust_raw = contract.canonical_trust_policy(trust, at=at)
            attestations = repo / "docs/modules/attestations"
            attestations.mkdir(parents=True)
            expected_index: list[dict[str, str]] = []
            template = (FIXTURES / "positive/module.md").read_bytes()
            template = template.replace(b"Sources: src/modules/memory/*.c", b"Sources: none")
            template = template.replace(b"path:scripts/module_doc_contract.py#L1", b"path:evidence.txt#L1")
            for module_id, optional in modules.items():
                descriptor_path = repo / "src/modules" / module_id / "module.yaml"
                descriptor = json.loads(descriptor_path.read_text(encoding="ascii"))
                descriptor.update({
                    "descriptor_version": 2,
                    "docs": f"docs/modules/{module_id}.md",
                    "sources": [], "public_headers": [],
                    "surfaces": {"routes": [], "commands": [], "protocols": [], "stages": []},
                })
                descriptor_path.write_text(json.dumps(descriptor, indent=2) + "\n", encoding="ascii")
                document = template.replace(b"# memory module", f"# {module_id} module".encode("ascii"), 1)
                document_path = repo / "docs/modules" / f"{module_id}.md"
                document_path.parent.mkdir(parents=True, exist_ok=True)
                document_path.write_bytes(document)
                subject = {
                    "descriptor_sha256": contract.sha256(descriptor_path.read_bytes()),
                    "document_sha256": contract.sha256(document),
                    "module_id": module_id,
                    "owner_identity": "owner@example", "reviewer_identity": "reviewer@example",
                    "schema": "aimee.module-doc-attestation.v1", "signed_at": "2027-01-01T00:00:00Z",
                }
                subject_raw = contract.canonical_subject(subject)
                (attestations / f"{module_id}.subject.json").write_bytes(subject_raw)
                for role, identity in (("owner", "owner@example"), ("reviewer", "reviewer@example")):
                    signed = subprocess.run(
                        ["ssh-keygen", "-Y", "sign", "-f", str(key_paths[identity]),
                         "-n", "aimee.module-doc.v1"], input=subject_raw, check=True,
                        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                    )
                    (attestations / f"{module_id}.{role}.sig").write_bytes(signed.stdout)
                expected_index.append({"module_id": module_id, "subject_sha256": contract.sha256(subject_raw)})
            expected_index.sort(key=lambda item: item["module_id"])
            (attestations / "index.json").write_bytes(contract.canonical_attestation_index(expected_index))
            subprocess.run(["git", "add", "."], cwd=repo, check=True)
            subprocess.run(["git", "commit", "-qm", "candidate"], cwd=repo, check=True)
            candidate_commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()
            summary = contract.validate_module_candidate(
                contract.GitBlobReader(repo, candidate_commit),
                contract.GitBlobReader(repo, base_commit),
                required_ids={"config"}, optional_ids={"runtime-web"}, trust_policy_raw=trust_raw,
                oidc_iat=int(at.timestamp()), toolchain=locked_toolchain(),
            )
            self.assertIn("2 descriptor-v2 documents", summary)
            (attestations / "extra.sig").write_text("not governed\n", encoding="ascii")
            subprocess.run(["git", "add", "."], cwd=repo, check=True)
            subprocess.run(["git", "commit", "-qm", "extra"], cwd=repo, check=True)
            extra_commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()
            self.assert_rule("attestation-files", lambda: contract.validate_module_candidate(
                contract.GitBlobReader(repo, extra_commit),
                contract.GitBlobReader(repo, base_commit),
                required_ids={"config"}, optional_ids={"runtime-web"}, trust_policy_raw=trust_raw,
                oidc_iat=int(at.timestamp()), toolchain=locked_toolchain(),
            ))

    def test_canonical_subject_matches_golden_vector(self) -> None:
        value = json.loads((FIXTURES / "positive/subject.json").read_text(encoding="ascii"))
        actual = contract.canonical_subject(value)
        self.assertEqual(actual, (FIXTURES / "positive/subject.json").read_bytes())
        reordered = {key: value[key] for key in reversed(list(value))}
        self.assertEqual(contract.canonical_subject(reordered), actual)

    def test_trust_policy_roles_and_verification_time(self) -> None:
        at = datetime(2027, 1, 1, tzinfo=timezone.utc)
        def public_key(byte: int) -> str:
            key_type = b"ssh-ed25519"
            blob = struct.pack(">I", len(key_type)) + key_type + struct.pack(">I", 32) + bytes([byte]) * 32
            return "ssh-ed25519 " + base64.b64encode(blob).decode("ascii")
        key_one = public_key(1)
        key_two = public_key(2)
        policy = {
            "schema": "aimee.module-doc-trust.v1", "epoch": 1,
            "identities": [
                {"identity": "owner@example", "role": "owner", "public_key": key_one,
                 "not_before": "2026-01-01T00:00:00Z", "not_after": "2028-01-01T00:00:00Z", "revoked_at": None},
                {"identity": "reviewer@example", "role": "reviewer", "public_key": key_two,
                 "not_before": "2026-01-01T00:00:00Z", "not_after": "2028-01-01T00:00:00Z", "revoked_at": None},
            ],
        }
        identities = contract.validate_trust_policy(policy, at=at)
        self.assertEqual(set(identities), {"owner@example", "reviewer@example"})
        canonical = contract.canonical_trust_policy(policy, at=at)
        self.assertEqual(set(contract.load_canonical_trust_policy(canonical, at=at)), set(identities))
        self.assert_rule("trust-policy-canonical", lambda: contract.load_canonical_trust_policy(
            canonical + b"\n", at=at
        ))
        contract.validate_trust_epoch(1, 2)
        self.assert_rule("trust-policy-epoch", lambda: contract.validate_trust_epoch(2, 2))
        subject = json.loads((FIXTURES / "positive/subject.json").read_text(encoding="ascii"))
        contract.authorize_subject(subject, identities, oidc_iat=int(at.timestamp()))
        swapped = dict(subject)
        swapped["owner_identity"] = "reviewer@example"
        swapped["reviewer_identity"] = "owner@example"
        self.assert_rule("subject-role", lambda: contract.authorize_subject(
            swapped, identities, oidc_iat=int(at.timestamp())
        ))
        later = datetime(2027, 1, 1, 12, tzinfo=timezone.utc)
        enrolled_late = json.loads(json.dumps(policy))
        enrolled_late["identities"][0]["not_before"] = "2027-01-01T01:00:00Z"
        late_identities = contract.validate_trust_policy(enrolled_late, at=later)
        self.assert_rule("subject-signing-authorization", lambda: contract.authorize_subject(
            subject, late_identities, oidc_iat=int(later.timestamp())
        ))
        policy["identities"][0]["revoked_at"] = "2026-12-31T00:00:00Z"
        self.assert_rule("trust-policy-authorization", lambda: contract.validate_trust_policy(policy, at=at))

    def test_toolchain_lock_and_publisher_result(self) -> None:
        executable = Path(shutil.which("ssh-keygen"))
        lock = {
            "schema": "aimee.sshsig-toolchain.v1",
            "image": "registry.example.invalid/aimee/sshsig@sha256:" + "a" * 64,
            "ssh_keygen_sha256": contract.sha256(executable.read_bytes()),
        }
        self.assertEqual(contract.load_locked_toolchain(lock, executable).ssh_keygen, executable)
        wrong_hash = dict(lock)
        wrong_hash["ssh_keygen_sha256"] = "0" * 64
        self.assert_rule("toolchain-binary", lambda: contract.load_locked_toolchain(
            wrong_hash, executable
        ))
        bad = dict(lock)
        bad["image"] = "registry.example.invalid/aimee/sshsig:latest"
        self.assert_rule("toolchain-image", lambda: contract.validate_toolchain_lock(bad))
        with tempfile.TemporaryDirectory() as tmp:
            copied = Path(tmp) / "ssh-keygen"
            shutil.copy2(executable, copied)
            copied_lock = dict(lock)
            copied_lock["ssh_keygen_sha256"] = contract.sha256(copied.read_bytes())
            loaded = contract.load_locked_toolchain(copied_lock, copied)
            copied.write_bytes(copied.read_bytes() + b"changed")
            self.assert_rule("toolchain-binary", loaded.executable)
        target = {"candidate_revision": "b" * 40, "trigger_check_identity": "trigger-9"}
        result = {
            "schema": "aimee.module-doc-check.v1", "name": "module-doc-attestation",
            "candidate_revision": "b" * 40, "external_id": "trigger-9",
            "conclusion": "success", "summary": "All governed artifacts passed.",
        }
        self.assertEqual(contract.validate_publisher_result(result, target), result)

    def test_real_ed25519_sshsig_verification(self) -> None:
        subject = (FIXTURES / "positive/subject.json").read_bytes()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            key = root / "key"
            subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", str(key)], check=True)
            signed = subprocess.run(
                ["ssh-keygen", "-Y", "sign", "-f", str(key), "-n", "aimee.module-doc.v1"],
                input=subject, check=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            )
            public_key = key.with_suffix(".pub").read_text(encoding="ascii").split()
            canonical_key = " ".join(public_key[:2])
            signature = signed.stdout
            toolchain = locked_toolchain()
            contract.verify_sshsig(subject, signature, "owner@example", canonical_key, toolchain)
            other_key = root / "other-key"
            subprocess.run(
                ["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", str(other_key)],
                check=True,
            )
            other_public = " ".join(
                other_key.with_suffix(".pub").read_text(encoding="ascii").split()[:2]
            )
            self.assert_rule("sshsig-verify", lambda: contract.verify_sshsig(
                subject, signature, "owner@example", other_public, toolchain
            ))
            wrong_namespace = subprocess.run(
                ["ssh-keygen", "-Y", "sign", "-f", str(key), "-n", "other.namespace"],
                input=subject, check=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            )
            self.assert_rule("sshsig-namespace", lambda: contract.verify_sshsig(
                subject, wrong_namespace.stdout, "owner@example", canonical_key, toolchain
            ))
            self.assert_rule("sshsig-verify", lambda: contract.verify_sshsig(
                subject + b"x", signature, "owner@example", canonical_key, toolchain
            ))
            signed_sha256 = subprocess.run(
                ["ssh-keygen", "-Y", "sign", "-f", str(key), "-n", "aimee.module-doc.v1", "-O", "hashalg=sha256"],
                input=subject, check=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            )
            self.assert_rule("sshsig-hash", lambda: contract.verify_sshsig(
                subject, signed_sha256.stdout, "owner@example", canonical_key,
                toolchain,
            ))
            self.assert_rule("sshsig-size", lambda: contract.verify_sshsig(
                subject, b"x" * (contract.MAX_SIGNATURE_BYTES + 1), "owner@example",
                canonical_key, toolchain,
            ))
            self.assert_rule("toolchain-lock", lambda: contract.verify_sshsig(
                subject, signature, "owner@example", canonical_key, key,
            ))


if __name__ == "__main__":
    unittest.main()
