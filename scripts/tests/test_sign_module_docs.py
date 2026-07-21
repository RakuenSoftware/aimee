#!/usr/bin/env python3
"""Tests for the human module-document signing helper."""

from __future__ import annotations

import base64
import importlib.util
from pathlib import Path
import struct
import sys
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
CONTRACT_SPEC = importlib.util.spec_from_file_location(
    "module_doc_contract", REPO_ROOT / "scripts/module_doc_contract.py"
)
assert CONTRACT_SPEC and CONTRACT_SPEC.loader
contract = importlib.util.module_from_spec(CONTRACT_SPEC)
sys.modules["module_doc_contract"] = contract
CONTRACT_SPEC.loader.exec_module(contract)
SIGN_SPEC = importlib.util.spec_from_file_location(
    "sign_module_docs", REPO_ROOT / "scripts/sign_module_docs.py"
)
assert SIGN_SPEC and SIGN_SPEC.loader
signer = importlib.util.module_from_spec(SIGN_SPEC)
SIGN_SPEC.loader.exec_module(signer)


def public_key(byte: int = 1) -> str:
    key_type = b"ssh-ed25519"
    blob = struct.pack(">I", len(key_type)) + key_type + struct.pack(">I", 32) + bytes([byte]) * 32
    return "ssh-ed25519 " + base64.b64encode(blob).decode("ascii")


class SignModuleDocsTests(unittest.TestCase):
    def assert_rule(self, rule: str, callback) -> None:
        with self.assertRaisesRegex((signer.SigningError, contract.ContractError), f"rule={rule}"):
            callback()

    def test_committed_inventory_is_the_only_signing_inventory(self) -> None:
        required, optional = signer._inventory(REPO_ROOT)
        self.assertEqual(len(required), 18)
        self.assertEqual(len(optional), 8)
        self.assertFalse(required & optional)

    def test_public_key_input_cannot_be_a_private_key(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            valid = root / "owner.pub"
            valid.write_text(public_key() + "\n", encoding="ascii")
            self.assertEqual(signer._public_key(valid), public_key())
            private = root / "owner"
            private.write_text(
                "-----BEGIN OPENSSH PRIVATE KEY-----\nnot-a-public-key\n"
                "-----END OPENSSH PRIVATE KEY-----\n",
                encoding="ascii",
            )
            self.assert_rule("public-key", lambda: signer._public_key(private))

    def test_existing_partial_directory_is_rejected_before_replacement(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / "attestations"
            target.mkdir()
            (target / "index.json").write_text("{}\n", encoding="ascii")
            marker = (target / "index.json").read_bytes()
            self.assert_rule("existing-attestations", lambda: signer._existing_attestations_are_well_formed(
                target,
                {"memory"},
                owner_identity="owner@example",
                owner_key=public_key(1),
                reviewer_identity="reviewer@example",
                reviewer_key=public_key(2),
                toolchain=object(),
            ))
            self.assertEqual((target / "index.json").read_bytes(), marker)

    def test_repository_files_reject_symlink_ancestors(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp) / "repo"
            outside = Path(tmp) / "outside"
            repo.mkdir()
            outside.mkdir()
            (outside / "secret").write_text("secret", encoding="ascii")
            (repo / "linked").symlink_to(outside, target_is_directory=True)
            self.assert_rule("repository-path", lambda: signer._repository_file(
                repo, Path("linked/secret"), max_bytes=1024
            ))

    def test_directory_exchange_is_atomic_when_supported(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            staged = root / "staged"
            target = root / "target"
            staged.mkdir()
            target.mkdir()
            (staged / "new").write_text("new", encoding="ascii")
            (target / "old").write_text("old", encoding="ascii")
            try:
                signer._rename_exchange(staged, target)
            except signer.SigningError as exc:
                if "rule=atomic-replace" in str(exc):
                    self.skipTest(str(exc))
                raise
            self.assertEqual((target / "new").read_text(encoding="ascii"), "new")
            self.assertEqual((staged / "old").read_text(encoding="ascii"), "old")

    def test_helper_never_invokes_a_shell_or_accepts_private_key_argument(self) -> None:
        source = (REPO_ROOT / "scripts/sign_module_docs.py").read_text(encoding="utf-8")
        self.assertNotIn("shell=True", source)
        self.assertNotIn("private-key", source)
        self.assertIn('"SSH_AUTH_SOCK": auth_sock', source)
        self.assertIn('public_path.write_text(public_key + "\\n"', source)


if __name__ == "__main__":
    unittest.main()
