#!/usr/bin/env python3
"""Tests for provider-neutral module-document attestation contracts."""

from __future__ import annotations

import base64
from datetime import datetime, timezone
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import struct
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = REPO_ROOT / "scripts/module_doc_contract.py"
SPEC = importlib.util.spec_from_file_location("module_doc_contract", MODULE_PATH)
assert SPEC and SPEC.loader
contract = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(contract)
FIXTURES = REPO_ROOT / "tests/fixtures/module-doc-contract"


def locked_toolchain():
    executable_text = shutil.which("ssh-keygen")
    if executable_text is None:
        raise unittest.SkipTest("ssh-keygen is required")
    executable = Path(executable_text)
    lock = {
        "schema": "aimee.sshsig-toolchain.v1",
        "image": "registry.example.invalid/aimee/sshsig@sha256:" + "a" * 64,
        "ssh_keygen_sha256": contract.sha256(executable.read_bytes()),
    }
    return contract.load_locked_toolchain(lock, executable)


def oidc_profile() -> dict[str, object]:
    mappings = {field: f"ci_{field}" for field in contract.WORKLOAD_FIELDS}
    return {
        "schema": "aimee.ci-oidc-profile.v1",
        "name": "protected-ci",
        "issuer": "https://issuer.example.invalid",
        "audience": "aimee-module-doc-verifier",
        "jwks": {
            "uri": "https://issuer.example.invalid/jwks",
            "tls_spki_sha256": ["1" * 64],
            "key_thumbprints_sha256": ["3" * 64],
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
            "tls_spki_sha256": ["2" * 64],
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
    def assert_rule(self, rule: str, callback) -> None:
        with self.assertRaisesRegex(contract.ContractError, f"rule={rule}"):
            callback()

    def test_positive_document_fixture(self) -> None:
        raw = (FIXTURES / "positive/module.md").read_bytes()
        contract.parse_module_document(
            raw, "memory", contract.DocumentProjection(("src/modules/memory/*.c",), ())
        )

    def test_workflow_activation_is_dormant_then_fail_closed(self) -> None:
        workflow = (
            REPO_ROOT / ".github/workflows/module-doc-attestation-trigger.yml"
        ).read_text(encoding="utf-8")
        self.assertIn("if: ${{ vars.MODULE_DOC_ATTESTATION_ENABLED == 'true' }}", workflow)
        self.assertIn("MODULE_DOC_VERIFIER_AUDIENCE and MODULE_DOC_VERIFIER_URL must be configured", workflow)
        self.assertNotIn("actions/checkout", workflow)

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

    def test_profile_is_provider_neutral_and_closed(self) -> None:
        profile = oidc_profile()
        self.assertEqual(contract.validate_oidc_profile(profile), profile)
        example_raw = (
            REPO_ROOT / ".github/module-doc-attestation/issuer-profile.example.json"
        ).read_bytes()
        example = contract.strict_json_bytes(example_raw)
        self.assertEqual(contract.validate_oidc_profile(example), example)
        self.assertNotIn(b"github", example_raw.lower())
        unknown = dict(profile)
        unknown["provider"] = "github"
        self.assert_rule("oidc-profile-shape", lambda: contract.validate_oidc_profile(unknown))
        no_pin = json.loads(json.dumps(profile))
        no_pin["jwks"]["tls_spki_sha256"] = []
        self.assert_rule("oidc-jwks-pins", lambda: contract.validate_oidc_profile(no_pin))
        missing_predicate = json.loads(json.dumps(profile))
        del missing_predicate["predicates"]["workflow_revision"]
        self.assert_rule("oidc-predicates", lambda: contract.validate_oidc_profile(missing_predicate))
        self.assert_rule("json-size", lambda: contract.strict_json_bytes(
            b" " * (contract.MAX_JSON_BYTES + 1)
        ))
        malformed = (
            ("issuer", "https://user@example.invalid", "oidc-issuer"),
            ("issuer", "https:///missing-host", "oidc-issuer"),
            ("issuer", "https://issuer.example.invalid/#fragment", "oidc-issuer"),
            ("issuer", "https://Issuer.example.invalid", "oidc-issuer"),
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
        bad_attempt = dict(token_claims)
        bad_attempt[profile["claim_mappings"]["attempt"]] = "01"
        self.assert_rule("oidc-attempt", lambda: contract.normalize_verified_oidc_claims(
            bad_attempt, profile, wall_time=1_800_000_000
        ))

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
            reader.resolve_reference("path:evidence.txt#L2")
            self.assert_rule("document-evidence-line", lambda: reader.resolve_reference("path:evidence.txt#L3"))
            evidence.unlink()
            evidence.symlink_to("target")
            subprocess.run(["git", "add", "evidence.txt"], cwd=repo, check=True)
            subprocess.run(["git", "commit", "-qm", "symlink"], cwd=repo, check=True)
            symlink_commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()
            symlink_reader = contract.GitBlobReader(repo, symlink_commit)
            self.assert_rule("git-mode", lambda: symlink_reader.read_blob("evidence.txt"))

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

            def verify(token: str, selected: dict[str, object]) -> dict[str, object]:
                self.assertEqual(token, "signed.jwt.value")
                self.assertEqual(selected, profile)
                events.append("crypto")
                return token_claims

            def resolve(workload: dict[str, object], pull_request: int) -> dict[str, object]:
                self.assertEqual(events, ["crypto"])
                events.append("resolve")
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

            def validate(reader, workload, target) -> str:
                self.assertEqual(events, ["crypto", "resolve"])
                events.append("candidate")
                self.assertEqual(reader.read_blob("evidence.txt"), b"governed\n")
                return "All governed artifacts passed."

            request = json.dumps({
                "schema": "aimee.module-doc-trigger.v1",
                "pull_request": 1708,
                "oidc_token": "signed.jwt.value",
            }, separators=(",", ":")).encode("ascii")
            decision = contract.evaluate_trigger(
                request,
                (json.dumps(profile, separators=(",", ":")) + "\n").encode("ascii"),
                wall_time=1_800_000_000,
                verify_jwt=verify,
                resolve_target=resolve,
                validate_candidate=validate,
                repository=repo,
            )
            self.assertEqual(events, ["crypto", "resolve", "candidate"])
            self.assertEqual(decision.publisher_result["candidate_revision"], commit)

    @unittest.skipUnless(shutil.which("ssh-keygen"), "ssh-keygen is required")
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
        target = {"candidate_revision": "b" * 40, "trigger_check_identity": "trigger-9"}
        result = {
            "schema": "aimee.module-doc-check.v1", "name": "module-doc-attestation",
            "candidate_revision": "b" * 40, "external_id": "trigger-9",
            "conclusion": "success", "summary": "All governed artifacts passed.",
        }
        self.assertEqual(contract.validate_publisher_result(result, target), result)

    @unittest.skipUnless(shutil.which("ssh-keygen"), "ssh-keygen is required")
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
