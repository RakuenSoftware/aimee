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
        value[claim_name] = predicates.get(field, f"value-{field}")
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

    def test_document_negative_fixtures_have_stable_rules(self) -> None:
        positive = (FIXTURES / "positive/module.md").read_bytes()
        cases = {
            "unicode": (positive.replace(b"Clear", "Cl\u00e9ar".encode()), "document-byte-domain"),
            "section-order": (positive.replace(
                b"## Purpose and non-goals", b"## Classification and lifecycle", 1
            ), "document-section-cardinality"),
            "placeholder": (positive.replace(b"Clear", b"TODO", 1), "document-placeholder"),
            "projection": (positive.replace(b"Sources: src/modules/memory/*.c", b"Sources: none", 1), "document-projection"),
        }
        for name, (raw, rule) in cases.items():
            with self.subTest(name=name):
                self.assert_rule(rule, lambda raw=raw: contract.parse_module_document(
                    raw, "memory", contract.DocumentProjection(("src/modules/memory/*.c",), ())
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
        policy["identities"][0]["revoked_at"] = "2026-12-31T00:00:00Z"
        self.assert_rule("trust-policy-authorization", lambda: contract.validate_trust_policy(policy, at=at))

    def test_toolchain_lock_and_publisher_result(self) -> None:
        executable = Path(shutil.which("ssh-keygen"))
        lock = {
            "schema": "aimee.sshsig-toolchain.v1",
            "image": "registry.example.invalid/aimee/sshsig@sha256:" + "a" * 64,
            "ssh_keygen_sha256": contract.sha256(executable.read_bytes()),
        }
        contract.verify_toolchain_binary(contract.validate_toolchain_lock(lock), executable)
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
            contract.verify_sshsig(subject, signature, "owner@example", canonical_key, Path(shutil.which("ssh-keygen")))
            self.assert_rule("sshsig-verify", lambda: contract.verify_sshsig(
                subject + b"x", signature, "owner@example", canonical_key, Path(shutil.which("ssh-keygen"))
            ))
            signed_sha256 = subprocess.run(
                ["ssh-keygen", "-Y", "sign", "-f", str(key), "-n", "aimee.module-doc.v1", "-O", "hashalg=sha256"],
                input=subject, check=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            )
            self.assert_rule("sshsig-hash", lambda: contract.verify_sshsig(
                subject, signed_sha256.stdout, "owner@example", canonical_key,
                Path(shutil.which("ssh-keygen")),
            ))


if __name__ == "__main__":
    unittest.main()
