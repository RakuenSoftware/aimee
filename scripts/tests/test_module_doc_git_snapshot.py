#!/usr/bin/env python3
"""Offline tests for provider-neutral immutable Git snapshot acquisition."""

from __future__ import annotations

import importlib.util
import fcntl
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[2]


def load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


contract = load("module_doc_contract", REPO_ROOT / "scripts/module_doc_contract.py")
snapshot = load(
    "module_doc_git_snapshot", REPO_ROOT / "scripts/module_doc_git_snapshot.py"
)


class GitSnapshotTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.fixture_root = self.root / "fixtures"
        self.fixture_root.mkdir()
        self.remote = self.fixture_root / "remote.git"
        self.work = self.root / "work"
        self.snapshots = self.root / "snapshots"
        self.snapshots.mkdir()
        subprocess.run(["git", "init", "-q", "--bare", str(self.remote)], check=True)
        subprocess.run(["git", "init", "-q", str(self.work)], check=True)
        subprocess.run(
            ["git", "config", "user.email", "fixture@example.invalid"],
            cwd=self.work, check=True,
        )
        subprocess.run(
            ["git", "config", "user.name", "Fixture"], cwd=self.work, check=True
        )
        (self.work / "evidence.txt").write_text("base\n", encoding="ascii")
        subprocess.run(["git", "add", "evidence.txt"], cwd=self.work, check=True)
        subprocess.run(["git", "commit", "-qm", "base"], cwd=self.work, check=True)
        self.base = self.git("rev-parse", "HEAD")
        (self.work / "evidence.txt").write_text("candidate\n", encoding="ascii")
        subprocess.run(["git", "commit", "-qam", "candidate"], cwd=self.work, check=True)
        self.candidate = self.git("rev-parse", "HEAD")
        subprocess.run(
            [
                "git", "push", str(self.remote),
                f"{self.base}:refs/heads/base",
                f"{self.candidate}:refs/heads/candidate",
            ], cwd=self.work, check=True, stdout=subprocess.DEVNULL,
        )
        self.specs = (
            snapshot.FetchSpec("refs/heads/base", "refs/aimee-snapshot/base", self.base),
            snapshot.FetchSpec(
                "refs/heads/candidate", "refs/aimee-snapshot/candidate", self.candidate
            ),
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def git(self, *arguments: str) -> str:
        return subprocess.check_output(
            ["git", *arguments], cwd=self.work, text=True
        ).strip()

    def acquire(self, authorization: bytearray, **kwargs):
        return snapshot._acquire_test_snapshot(
            self.remote,
            self.fixture_root,
            self.specs,
            authorization,
            temp_parent=self.snapshots,
            **kwargs,
        )

    def assert_rule(self, rule: str, callback) -> None:
        with self.assertRaisesRegex(contract.ContractError, f"rule={rule}"):
            callback()

    def test_acquires_exact_read_only_snapshot_without_secret_residue(self) -> None:
        secret = b"Authorization: Basic SECRET-VALUE-XXXXXXXX"
        authorization = bytearray(secret)
        observed = {}

        def observe(arguments, env, pass_fds):
            rendered = repr((arguments, env)).encode("utf-8")
            self.assertNotIn(secret, rendered)
            self.assertEqual(
                set(env),
                {
                    "GIT_CONFIG_GLOBAL", "GIT_CONFIG_NOSYSTEM", "GIT_TERMINAL_PROMPT",
                    "HOME", "LANG", "LC_ALL", "PATH",
                },
            )
            self.assertEqual(len(pass_fds), 1)
            fd = pass_fds[0]
            observed["fd"] = fd
            self.assertEqual(stat.S_IMODE(os.fstat(fd).st_mode), 0o600)
            required_seals = (
                fcntl.F_SEAL_SEAL | fcntl.F_SEAL_SHRINK
                | fcntl.F_SEAL_GROW | fcntl.F_SEAL_WRITE
            )
            self.assertEqual(fcntl.fcntl(fd, fcntl.F_GET_SEALS), required_seals)
            os.lseek(fd, 0, os.SEEK_SET)
            config = os.read(fd, snapshot.MAX_HEADER_BYTES + 1024)
            self.assertIn(secret, config)
            self.assertIn(self.remote.as_uri().encode("ascii"), config)
            self.assertNotIn(b"[http]\n", config)
            os.lseek(fd, 0, os.SEEK_SET)

        handle = snapshot._acquire_test_snapshot(
            self.remote,
            self.fixture_root,
            self.specs,
            authorization,
            temp_parent=self.snapshots,
            observer=observe,
        )
        self.assertEqual(authorization, bytearray(len(secret)))
        self.assertFalse(Path(f"/proc/self/fd/{observed['fd']}").exists())
        repository = handle.repository
        self.assertFalse(handle._root.stat().st_mode & stat.S_IWUSR)
        self.assertFalse(repository.stat().st_mode & stat.S_IWUSR)
        self.assertEqual(
            contract.GitBlobReader(repository, self.base).read_blob("evidence.txt"),
            b"base\n",
        )
        self.assertEqual(
            contract.GitBlobReader(repository, self.candidate).read_blob("evidence.txt"),
            b"candidate\n",
        )
        refs = subprocess.check_output(
            ["git", "for-each-ref", "--format=%(refname)", "refs/aimee-snapshot"],
            cwd=repository, text=True,
        ).splitlines()
        self.assertEqual(
            refs, ["refs/aimee-snapshot/base", "refs/aimee-snapshot/candidate"]
        )
        self.assertEqual(
            subprocess.check_output(["git", "remote"], cwd=repository), b""
        )
        handle.close()
        handle.close()
        self.assertFalse(repository.exists())
        self.assertEqual(list(self.snapshots.iterdir()), [])

    def test_wrong_or_missing_ref_fails_atomically_and_redacts(self) -> None:
        secret = b"Authorization: Basic SECRET-WRONG-SHA"
        authorization = bytearray(secret)
        wrong = (
            snapshot.FetchSpec(
                "refs/heads/base", "refs/aimee-snapshot/base", "f" * 40
            ),
        )
        with self.assertRaises(contract.ContractError) as captured:
            snapshot._acquire_test_snapshot(
                self.remote, self.fixture_root, wrong, authorization,
                temp_parent=self.snapshots,
            )
        self.assertNotIn(secret.decode(), str(captured.exception))
        self.assertEqual(authorization, bytearray(len(secret)))
        self.assertEqual(list(self.snapshots.iterdir()), [])

        missing_secret = bytearray(b"Authorization: Basic SECRET-MISSING")
        missing = self.specs + (
            snapshot.FetchSpec(
                "refs/heads/absent", "refs/aimee-snapshot/absent", "a" * 40
            ),
        )
        self.assert_rule(
            "snapshot-git",
            lambda: snapshot._acquire_test_snapshot(
                self.remote, self.fixture_root, missing, missing_secret,
                temp_parent=self.snapshots,
            ),
        )
        self.assertEqual(list(self.snapshots.iterdir()), [])

    def test_blob_ref_is_not_accepted_as_a_commit(self) -> None:
        blob = subprocess.check_output(
            ["git", "hash-object", "-w", "evidence.txt"], cwd=self.work, text=True
        ).strip()
        subprocess.run(
            ["git", "update-ref", "refs/test/blob", blob], cwd=self.work, check=True
        )
        subprocess.run(
            ["git", "push", str(self.remote), "refs/test/blob:refs/test/blob"],
            cwd=self.work, check=True, stdout=subprocess.DEVNULL,
        )
        specs = (
            snapshot.FetchSpec("refs/test/blob", "refs/aimee-snapshot/blob", blob),
        )
        self.assert_rule(
            "snapshot-git",
            lambda: snapshot._acquire_test_snapshot(
                self.remote, self.fixture_root, specs,
                bytearray(b"Authorization: Basic SECRET-BLOB"),
                temp_parent=self.snapshots,
            ),
        )

    def test_invalid_inputs_fail_before_subprocess_or_repository_creation(self) -> None:
        cases = (
            (
                "http://example.invalid/repo", self.specs,
                bytearray(b"Authorization: Bearer x"), "snapshot-url",
            ),
            (
                "https://example.invalid/repo", self.specs,
                bytearray(b"Authorization: x\nEvil: y"), "snapshot-credential",
            ),
            (
                "https://example.invalid/repo",
                (snapshot.FetchSpec("refs/heads/base", "refs/heads/bad", self.base),),
                bytearray(b"Authorization: Bearer x"),
                "snapshot-ref",
            ),
            (
                "https://example.invalid/repo",
                (snapshot.FetchSpec("refs/heads/base", "refs/aimee-snapshot/base", "abc"),),
                bytearray(b"Authorization: Bearer x"),
                "snapshot-commit",
            ),
        )
        for url, specs, authorization, rule in cases:
            original_length = len(authorization)
            with self.subTest(rule=rule), mock.patch.object(snapshot.subprocess, "Popen") as popen:
                self.assert_rule(
                    rule,
                    lambda url=url, specs=specs, auth=authorization: (
                        snapshot.acquire_snapshot(
                            url, specs, auth,
                            allowed_origins=("https://example.invalid",),
                            temp_parent=self.snapshots,
                        )
                    ),
                )
                popen.assert_not_called()
                self.assertEqual(authorization, bytearray(original_length))
        self.assertEqual(list(self.snapshots.iterdir()), [])

    def test_fixture_transport_rejects_escape_and_symlink(self) -> None:
        outside = self.root / "outside.git"
        outside.mkdir()
        self.assert_rule(
            "snapshot-test-remote",
            lambda: snapshot._validate_fixture_remote(outside, self.fixture_root),
        )
        link = self.fixture_root / "link.git"
        link.symlink_to(self.remote, target_is_directory=True)
        self.assert_rule(
            "snapshot-test-remote",
            lambda: snapshot._validate_fixture_remote(link, self.fixture_root),
        )
        root_link = self.root / "fixture-link"
        root_link.symlink_to(self.fixture_root, target_is_directory=True)
        self.assert_rule(
            "snapshot-test-remote",
            lambda: snapshot._validate_fixture_remote(self.remote, root_link),
        )

    def test_production_url_is_canonical_and_origin_is_explicitly_allowed(self) -> None:
        rejected = (
            "https://EXAMPLE.invalid/repo",
            "https://example.invalid:443/repo",
            "https://example.invalid/a/../repo",
            "https://example.invalid/a%2frepo",
            "https://example.invalid/repo ",
            "https://example.invalid:0/repo",
            "https://user@example.invalid/repo",
            'https://example.invalid/repo"name',
        )
        for url in rejected:
            with self.subTest(url=url):
                self.assert_rule("snapshot-url", lambda url=url: snapshot._validate_https_url(url))

        url, origin = snapshot._validate_https_url(
            "https://git.example.invalid:8443/repository"
        )
        self.assertEqual(url, "https://git.example.invalid:8443/repository")
        self.assertEqual(origin, "https://git.example.invalid:8443")
        snapshot._validate_allowed_origins(origin, (origin,))
        self.assert_rule(
            "snapshot-origin",
            lambda: snapshot._validate_allowed_origins(
                "https://169.254.169.254", ("https://git.example.invalid",)
            ),
        )
        self.assert_rule(
            "snapshot-origin",
            lambda: snapshot._validate_allowed_origins(
                origin, ("https://" + "a" * snapshot.MAX_ORIGIN_BYTES,)
            ),
        )

    def test_short_memfd_write_fails_closed_and_cleanup_is_retryable(self) -> None:
        authorization = bytearray(b"Authorization: Bearer SECRET-SHORT-WRITE")
        with mock.patch.object(snapshot.os, "write", return_value=0):
            self.assert_rule("snapshot-credential", lambda: self.acquire(authorization))
        self.assertEqual(authorization, bytearray(len(authorization)))
        self.assertEqual(list(self.snapshots.iterdir()), [])

        real_write = snapshot.os.write
        calls = 0

        def fragmented_write(fd, value):
            nonlocal calls
            calls += 1
            if calls == 1:
                return real_write(fd, value[:7])
            if calls == 2:
                raise InterruptedError
            return real_write(fd, value)

        with mock.patch.object(snapshot.os, "write", side_effect=fragmented_write):
            fragmented = self.acquire(
                bytearray(b"Authorization: Bearer SECRET-FRAGMENTED")
            )
        fragmented.close()
        self.assertGreaterEqual(calls, 3)

        handle = self.acquire(bytearray(b"Authorization: Bearer SECRET-CLEANUP"))
        repository = handle.repository
        with mock.patch.object(snapshot.shutil, "rmtree", side_effect=OSError("busy")) as remove:
            self.assert_rule("snapshot-cleanup", handle.close)
            self.assertEqual(remove.call_count, 2)
        self.assertTrue(repository.exists())
        handle.close()
        self.assertFalse(repository.exists())

    def test_platform_output_timeout_and_disk_bounds(self) -> None:
        with mock.patch.object(snapshot.os, "memfd_create", None):
            authorization = bytearray(b"Authorization: Basic SECRET-PLATFORM")
            self.assert_rule("snapshot-platform", lambda: self.acquire(authorization))
            self.assertEqual(authorization, bytearray(len(authorization)))

        env = snapshot._base_env(self.root)
        self.assert_rule(
            "snapshot-output",
            lambda: snapshot._run(
                ["/usr/bin/python3", "-c", f"print('x'*{snapshot.MAX_OUTPUT_BYTES + 1})"],
                cwd=self.root, env=env, timeout_seconds=2,
            ),
        )
        self.assert_rule(
            "snapshot-timeout",
            lambda: snapshot._run(
                ["/usr/bin/python3", "-c", "import time; time.sleep(5)"],
                cwd=self.root, env=env, timeout_seconds=1,
            ),
        )
        started = time.monotonic()
        self.assert_rule(
            "snapshot-timeout",
            lambda: snapshot._run(
                [
                    "/usr/bin/python3", "-c",
                    "import os,time; p=os.fork(); "
                    "os._exit(0) if p else time.sleep(5)",
                ],
                cwd=self.root, env=env, timeout_seconds=1,
            ),
        )
        self.assertLess(time.monotonic() - started, 2)
        large = self.root / "large"
        large.mkdir()
        (large / "data").write_bytes(b"x" * (snapshot.MIN_DISK_BYTES + 1))
        self.assert_rule(
            "snapshot-disk",
            lambda: snapshot._run(
                ["/usr/bin/python3", "-c", "import time; time.sleep(.2)"],
                cwd=self.root, env=env, timeout_seconds=2,
                monitor_root=large, max_disk_bytes=snapshot.MIN_DISK_BYTES,
            ),
        )

    def test_tree_scan_is_bounded_by_entries_not_by_the_clock(self) -> None:
        """The scan budget must be deterministic.

        The entry cap is the bound that decides a verdict, and the same tree must get
        the same answer everywhere. The clock is only a stuck-filesystem backstop: at
        0.1s it failed a few-dozen-file fixture on a contended CI runner and reported
        it as rule=snapshot-disk, i.e. a contract violation, on two unrelated branches
        while the identical tree passed locally.
        """
        tree = self.root / "scan"
        tree.mkdir()
        for index in range(8):
            (tree / f"f{index}").write_bytes(b"x" * 16)

        # A healthy tree is measured, not rejected -- and the clock does not decide it.
        self.assertEqual(snapshot._tree_bytes(tree), 8 * 16)
        with mock.patch.object(snapshot, "MAX_TREE_SCAN_SECONDS", 0.0):
            self.assertEqual(
                snapshot._tree_bytes(tree),
                8 * 16,
                "an already-expired clock must not fail a tree well under the entry cap",
            )

        # The deterministic cap still fires, and still as snapshot-disk.
        with mock.patch.object(snapshot, "MAX_TREE_ENTRIES", 4):
            self.assert_rule("snapshot-disk", lambda: snapshot._tree_bytes(tree))

    def test_sensitive_output_is_discarded_and_failure_is_fixed(self) -> None:
        secret = "Authorization: Bearer REFLECTED-SECRET"
        env = snapshot._base_env(self.root)
        output = snapshot._run(
            ["/usr/bin/python3", "-c", f"print({secret!r})"],
            cwd=self.root, env=env, timeout_seconds=2, sensitive_output=True,
        )
        self.assertEqual(output, b"")
        with self.assertRaises(contract.ContractError) as captured:
            snapshot._run(
                [
                    "/usr/bin/python3", "-c",
                    f"import sys; print({secret!r}, file=sys.stderr); sys.exit(1)",
                ],
                cwd=self.root, env=env, timeout_seconds=2, sensitive_output=True,
            )
        self.assertEqual(
            str(captured.exception),
            "rule=snapshot-git: authenticated Git operation failed",
        )
        self.assertNotIn("REFLECTED-SECRET", str(captured.exception))

    def test_initialization_failures_and_unreachable_objects_fail_closed(self) -> None:
        authorization = bytearray(b"Authorization: Bearer SECRET-CHMOD")
        with mock.patch.object(snapshot.Path, "chmod", side_effect=OSError("mode")):
            with self.assertRaisesRegex(OSError, "mode"):
                self.acquire(authorization)
        self.assertEqual(authorization, bytearray(len(authorization)))
        self.assertEqual(list(self.snapshots.iterdir()), [])

        authorization = bytearray(b"Authorization: Bearer SECRET-MKDIR")
        with mock.patch.object(snapshot.Path, "mkdir", side_effect=OSError("mkdir")):
            with self.assertRaisesRegex(OSError, "mkdir"):
                self.acquire(authorization)
        self.assertEqual(authorization, bytearray(len(authorization)))
        self.assertEqual(list(self.snapshots.iterdir()), [])

        real_run = snapshot._run
        injected = False

        def inject_unreachable(arguments, **kwargs):
            nonlocal injected
            if "rev-parse" in arguments and not injected:
                injected = True
                subprocess.run(
                    ["git", "hash-object", "-w", "--stdin"],
                    cwd=kwargs["cwd"], input=b"reflected credential", check=True,
                    stdout=subprocess.DEVNULL,
                )
            return real_run(arguments, **kwargs)

        with mock.patch.object(snapshot, "_run", side_effect=inject_unreachable):
            self.assert_rule(
                "snapshot-object-closure",
                lambda: self.acquire(
                    bytearray(b"Authorization: Bearer SECRET-UNREACHABLE")
                ),
            )
        self.assertTrue(injected)
        self.assertEqual(list(self.snapshots.iterdir()), [])

    def test_subprocess_environment_is_a_fixed_allowlist(self) -> None:
        with mock.patch.dict(
            os.environ,
            {
                "GIT_DIR": "/attacker",
                "GIT_OBJECT_DIRECTORY": "/attacker/objects",
                "GIT_TRACE": "1",
                "HTTPS_PROXY": "https://attacker.invalid",
            },
        ):
            env = snapshot._base_env(self.root)
        self.assertEqual(
            set(env),
            {
                "GIT_CONFIG_GLOBAL", "GIT_CONFIG_NOSYSTEM", "GIT_TERMINAL_PROMPT",
                "HOME", "LANG", "LC_ALL", "PATH",
            },
        )


    def test_context_body_failure_cleans_repository(self) -> None:
        handle = self.acquire(bytearray(b"Authorization: Basic SECRET-BODY"))
        repository = handle.repository
        with self.assertRaisesRegex(RuntimeError, "body failed"):
            with handle:
                raise RuntimeError("body failed")
        self.assertFalse(repository.exists())
        self.assertEqual(list(self.snapshots.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
