import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("native_artifacts", Path(__file__).resolve().parents[1] / "preserve_native_recovery_logs.py")
helper = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(helper)


class NativeRecoveryArtifacts(unittest.TestCase):
    def test_only_bounded_redacted_logs_exported_privately(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            destination = root / "artifacts"
            destination.mkdir(mode=0o700)
            home = root / "home"
            (home / ".config/aimee").mkdir(parents=True)
            (home / ".config/aimee/config.json").write_text("CONFIG_MUST_NOT_EXPORT")
            (home / ".config/aimee/server.log").write_text("attestation stage failed\nAuthorization: Bearer NEVER_EXPORT\npostgresql://user:NEVER_EXPORT@host/database\n")
            (home / "aimee-memory.log").write_text("memory module attach refused\napi_key=NEVER_EXPORT\n")
            (home / "native-ir-crash-live.log").write_text("x" * (helper.LIMIT * 2) + "\napi_key=NEVER_EXPORT\nAssertionError: actual owner refused\n")
            result = helper.preserve(destination, home, "")
            self.assertEqual(set(path.name for path in result.iterdir()), {"server.log", "native-ir-crash-live.log", "aimee-memory.log"})
            self.assertEqual(result.stat().st_mode & 0o777, 0o700)
            for path in result.iterdir():
                self.assertEqual(path.stat().st_mode & 0o777, 0o600)
                self.assertLessEqual(path.stat().st_size, helper.LIMIT)
                self.assertNotIn("NEVER_EXPORT", path.read_text())
                self.assertNotIn("CONFIG_MUST_NOT_EXPORT", path.read_text())
            self.assertIn("attestation stage failed", (result / "server.log").read_text())
            self.assertIn("memory module attach refused", (result / "aimee-memory.log").read_text())
            self.assertIn("AssertionError: actual owner refused", (result / "native-ir-crash-live.log").read_text())

    def test_shared_or_symlink_artifact_directory_refuses(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            unsafe = root / "shared"
            unsafe.mkdir(mode=0o755)
            with self.assertRaises(ValueError):
                helper.open_destination(unsafe)
            safe = root / "private"
            safe.mkdir(mode=0o700)
            alias = root / "alias"
            alias.symlink_to(safe, target_is_directory=True)
            with self.assertRaises(ValueError):
                helper.open_destination(alias)
            with self.assertRaises(ValueError):
                helper.open_destination(Path("relative"))

    def test_source_symlink_is_never_followed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            destination = root / "artifacts"
            destination.mkdir(mode=0o700)
            home = root / "home"
            home.mkdir()
            secret = root / "configuration"
            secret.write_text("SECRET_CONFIG")
            (home / "native-ir-crash-live.log").symlink_to(secret)
            with self.assertRaises(OSError):
                helper.preserve(destination, home, "")
            self.assertFalse(any(path.is_file() for path in destination.rglob("*")))


if __name__ == "__main__":
    unittest.main()
