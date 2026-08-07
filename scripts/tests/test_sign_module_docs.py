#!/usr/bin/env python3
"""Tests for the human module-document signing helper."""

from __future__ import annotations

import base64
import importlib.util
import json
import os
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from argparse import Namespace
from datetime import datetime, timezone


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
        # Sizes come from the committed inventory, not literals. A literal here is
        # a second copy of a number nothing keeps in step: adding `sandbox` as an
        # eighth optional module broke this test rather than the signer.
        baseline = json.loads(
            (REPO_ROOT / "tests/baselines/modules/canonical-inventory.yaml").read_text(
                encoding="utf-8"
            )
        )
        required, optional = signer._inventory(REPO_ROOT)
        self.assertEqual(len(required), len(baseline["required"]))
        self.assertEqual(len(optional), len(baseline["optional"]))
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

    def test_staging_directory_is_bound_to_open_parent(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            parent = root / "modules"
            parent.mkdir()
            parent_fd = os.open(
                parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
            )
            try:
                name, staging_fd, staging = signer._staging_directory(parent_fd)
                self.assertEqual(stat.S_IMODE(staging.stat().st_mode), 0o700)
                moved = root / "moved-modules"
                parent.rename(moved)
                attacker = root / "attacker"
                attacker.mkdir()
                parent.symlink_to(attacker, target_is_directory=True)
                (staging / "pinned").write_text("yes", encoding="ascii")
                self.assertTrue((moved / name / "pinned").is_file())
                self.assertFalse((attacker / name).exists())
                os.close(staging_fd)
            finally:
                os.close(parent_fd)

    def test_pinned_directory_detects_name_substitution(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            parent = Path(tmp)
            target = parent / "attestations"
            target.mkdir()
            (target / "marker").write_text("validated", encoding="ascii")
            parent_fd = os.open(
                parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
            )
            target_fd = signer._open_pinned_directory(
                parent_fd, "attestations", missing_ok=False
            )
            assert target_fd is not None
            expected = os.fstat(target_fd)
            try:
                target.rename(parent / "validated-away")
                target.mkdir()
                (target / "marker").write_text("substitution", encoding="ascii")
                self.assertEqual(
                    (Path(f"/proc/self/fd/{target_fd}") / "marker").read_text(
                        encoding="ascii"
                    ),
                    "validated",
                )
                self.assert_rule("target-race", lambda: signer._assert_entry_inode(
                    parent_fd, "attestations", expected, rule="target-race"
                ))
            finally:
                os.close(target_fd)
                os.close(parent_fd)

    def test_end_to_end_agent_signing_and_malformed_prior_preservation(self) -> None:
        for command in ("ssh-keygen", "ssh-agent", "ssh-add"):
            self.assertIsNotNone(shutil.which(command), f"{command} is required")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            repo = root / "repo"
            repo.mkdir()
            inventory = repo / signer.INVENTORY_PATH
            inventory.parent.mkdir(parents=True)
            inventory.write_text(json.dumps({
                "schema_version": 1, "required": ["config"], "optional": ["runtime-web"]
            }, indent=2) + "\n", encoding="ascii")
            (repo / "evidence.txt").write_text("evidence\n", encoding="ascii")
            template = (
                REPO_ROOT / "tests/fixtures/module-doc-contract/positive/module.md"
            ).read_bytes()
            template = template.replace(
                b"Sources: src/modules/memory/*.c", b"Sources: none"
            ).replace(
                b"path:scripts/module_doc_contract.py#L1", b"path:evidence.txt#L1"
            )
            for module_id, optional in (("config", False), ("runtime-web", True)):
                descriptor = {
                    "descriptor_version": 2,
                    "id": module_id,
                    "dependencies": [],
                    "runtime_toggle": {"supported": optional},
                    "docs": f"docs/modules/{module_id}.md",
                    "sources": [],
                    "public_headers": [],
                    "surfaces": {"routes": [], "commands": [], "protocols": [], "stages": []},
                }
                if optional:
                    descriptor["enabled_by_default"] = True
                descriptor_path = repo / "src/modules" / module_id / "module.yaml"
                descriptor_path.parent.mkdir(parents=True)
                descriptor_path.write_text(
                    json.dumps(descriptor, indent=2) + "\n", encoding="ascii"
                )
                document_path = repo / "docs/modules" / f"{module_id}.md"
                document_path.parent.mkdir(parents=True, exist_ok=True)
                document_path.write_bytes(
                    template.replace(
                        b"# memory module", f"# {module_id} module".encode("ascii"), 1
                    )
                )

            keys = root / "keys"
            keys.mkdir()
            private_paths: list[Path] = []
            public_paths: list[Path] = []
            for label in ("owner", "reviewer"):
                private = keys / label
                subprocess.run(
                    ["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", str(private)],
                    check=True,
                )
                canonical = " ".join(
                    private.with_suffix(".pub").read_text(encoding="ascii").split()[:2]
                )
                public = keys / f"canonical-{label}.pub"
                public.write_text(canonical + "\n", encoding="ascii")
                private_paths.append(private)
                public_paths.append(public)

            executable = Path(shutil.which("ssh-keygen"))
            lock_path = root / "toolchain.json"
            lock_path.write_text(json.dumps({
                "schema": "aimee.sshsig-toolchain.v1",
                "image": "registry.example.invalid/aimee/sshsig@sha256:" + "a" * 64,
                "ssh_keygen_sha256": contract.sha256(executable.read_bytes()),
            }, indent=2) + "\n", encoding="ascii")
            socket_path = root / "agent.sock"
            agent = subprocess.Popen(
                ["ssh-agent", "-D", "-a", str(socket_path)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            try:
                for _ in range(200):
                    if socket_path.exists():
                        break
                    time.sleep(0.01)
                self.assertTrue(socket_path.exists(), "ssh-agent socket did not appear")
                agent_env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "SSH_AUTH_SOCK": str(socket_path)}
                for private in private_paths:
                    subprocess.run(
                        ["ssh-add", str(private)], env=agent_env, check=True,
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                    )
                previous_sock = os.environ.get("SSH_AUTH_SOCK")
                os.environ["SSH_AUTH_SOCK"] = str(socket_path)
                try:
                    args = Namespace(
                        repo=repo,
                        owner_identity="owner@example",
                        owner_public_key=public_paths[0],
                        reviewer_identity="reviewer@example",
                        reviewer_public_key=public_paths[1],
                        signed_at=datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                        toolchain_lock=lock_path,
                        ssh_keygen=executable,
                    )
                    signer.build_attestations(args)
                    target = repo / signer.ATTESTATION_PATH
                    self.assertEqual(len(list(target.iterdir())), 7)
                    tampered = target / "config.owner.sig"
                    tampered.write_bytes(tampered.read_bytes() + b"x")
                    snapshot = {
                        entry.name: entry.read_bytes() for entry in target.iterdir()
                    }
                    self.assert_rule("sshsig-armor", lambda: signer.build_attestations(args))
                    self.assertEqual(
                        {entry.name: entry.read_bytes() for entry in target.iterdir()},
                        snapshot,
                    )
                finally:
                    if previous_sock is None:
                        os.environ.pop("SSH_AUTH_SOCK", None)
                    else:
                        os.environ["SSH_AUTH_SOCK"] = previous_sock
            finally:
                agent.terminate()
                agent.wait(timeout=5)

    def test_helper_never_invokes_a_shell_or_accepts_private_key_argument(self) -> None:
        source = (REPO_ROOT / "scripts/sign_module_docs.py").read_text(encoding="utf-8")
        self.assertNotIn("shell=True", source)
        self.assertNotIn("private-key", source)
        self.assertIn('"SSH_AUTH_SOCK": auth_sock', source)
        self.assertIn('"HOME": os.fspath(isolated_home)', source)
        self.assertIn('public_path.write_text(public_key + "\\n"', source)


if __name__ == "__main__":
    unittest.main()
