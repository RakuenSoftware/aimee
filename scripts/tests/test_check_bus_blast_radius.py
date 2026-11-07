from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class BusBlastRadiusTests(unittest.TestCase):
    def fixture(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        (root / "scripts").mkdir()
        (root / "src/tests").mkdir(parents=True)
        shutil.copyfile(ROOT / "scripts/check_bus_blast_radius.sh",
                        root / "scripts/check_bus_blast_radius.sh")
        (root / "src/Makefile").write_text(
            "OURO_RING_LIB = provided-ring.a\n"
            "$(BINARY): $(CORE_CONNECTION_LIB)\n"
            "$(SERVER): $(CORE_CONNECTION_LIB) $(OURO_RING_LIB)\n"
            "$(KB): $(CORE_CONNECTION_LIB) $(OURO_RING_LIB)\n"
            "../write-tier-enforce-live: $(CORE_CONNECTION_LIB) $(OURO_RING_LIB)\n")
        (root / "CMakeLists.txt").write_text("")
        return root

    def check(self, root):
        return subprocess.run(["bash", str(root / "scripts/check_bus_blast_radius.sh"),
                               "--allow-unbuilt"], text=True, capture_output=True)

    def test_exact_cancellation_peer_client_headers_allowed(self):
        root = self.fixture()
        (root / "src/tests/module_command_cancellation_cases.c").write_text(
            "#include <ouroboros/client.h>\n#include <ouroboros/endpoint.h>\n")
        result = self.check(root)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_cancellation_peer_host_or_arena_access_rejected(self):
        root = self.fixture()
        peer = root / "src/tests/module_command_cancellation_cases.c"
        for header in ("host", "arena"):
            with self.subTest(header=header):
                peer.write_text(f"#include <ouroboros/{header}.h>\n")
                result = self.check(root)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("ring header escaped", result.stderr)
        peer.write_text('#include <ouroboros/host.h> /* #include <ouroboros/client.h> */\n')
        result = self.check(root)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ring header escaped", result.stderr)

    def test_nearby_test_is_not_exempt(self):
        root = self.fixture()
        (root / "src/tests/module_command_other_cases.c").write_text(
            "#include <ouroboros/client.h>\n")
        result = self.check(root)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ring header escaped", result.stderr)

    def test_client_library_graph_remains_confined(self):
        root = self.fixture()
        makefile = root / "src/Makefile"
        makefile.write_text(makefile.read_text().replace(
            "$(BINARY): $(CORE_CONNECTION_LIB)",
            "$(BINARY): $(CORE_CONNECTION_LIB) $(OURO_RING_LIB)"))
        result = self.check(root)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must not consume", result.stderr)


if __name__ == "__main__":
    unittest.main()
