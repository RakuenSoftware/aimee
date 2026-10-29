#!/usr/bin/env python3
"""Tests for exhaustive standalone C-process export generation."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[2]
EXPORTER = REPO_ROOT / "scripts/export_c_repositories.py"
sys.path.insert(0, str(REPO_ROOT / "scripts"))
SPEC = importlib.util.spec_from_file_location("export_c_repositories", EXPORTER)
assert SPEC and SPEC.loader
exporter = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(exporter)


class ModuleExportTests(unittest.TestCase):
    def test_delegate_entrypoint_has_no_retired_command_watchdog(self) -> None:
        main = exporter.go_module_main("delegates", 18, [])
        self.assertNotIn("RunWatchdog", main)
        self.assertIn("handler.NewProcessHandler(ctx, os.Args[1])", main)

    @unittest.skipUnless(shutil.which("go"), "go is not installed")
    def test_delegates_export_builds_canonical_host_only_entrypoint(self) -> None:
        self.assertIn("server-go/modules/egress/egress.go", exporter.go_process_shared_sources("delegates"))
        contract = exporter.process_contracts.validate()["delegates"]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            exporter.export_module(root, "delegates", "required", contract,
                                   exporter.source_timestamp(),
                                   exporter.CORE_VERSION_FILE.read_text().strip())
            module = root / "aimee-module-delegates"
            main = (module / "runtime/main.go").read_text()
            self.assertNotIn("RunWatchdog", main)
            self.assertFalse((module / "server-go/modules/delegates/external_text.go").exists())
            self.assertTrue((module / "server-go/modules/delegates/generic_retirement_test.go").exists())
            binary = module / "delegates"
            build = subprocess.run(["go", "build", "-o", str(binary), "./runtime"],
                                   cwd=module, capture_output=True, text=True, timeout=180)
            self.assertEqual(build.returncode, 0, build.stderr)
            usage = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(usage.returncode, 2)
            self.assertIn("DAEMON_MODULE_BUS_SOCKET", usage.stderr)

    @unittest.skipUnless(shutil.which("go"), "go is not installed")
    def test_providers_export_builds_with_owned_probe_ir(self) -> None:
        contract = exporter.process_contracts.validate()["providers"]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            exporter.export_module(root, "providers", "required", contract,
                                   exporter.source_timestamp(), exporter.CORE_VERSION_FILE.read_text().strip())
            module = root / "aimee-module-providers"
            self.assertTrue((module / "server-go/ir/request.go").is_file())
            self.assertTrue((module / "server-go/ir/response.go").is_file())
            self.assertTrue((module / "server-go/modules/providers/probe_ir.go").is_file())
            self.assertTrue((module / "server-go/modules/translation/client.go").is_file())
            self.assertNotIn("RunProbeWorker", (module / "runtime/main.go").read_text())
            self.assertFalse((module / "server-go/modules/providers/probe_worker_linux.go").exists())
            build = subprocess.run(["go", "build", "-o", str(module / "providers"), "./runtime"],
                                   cwd=module, capture_output=True, text=True, timeout=180)
            self.assertEqual(build.returncode, 0, build.stderr)

    def test_shared_database_and_domain_contracts_follow_consumers(self) -> None:
        for module in ("aimee", "memory", "postgres"):
            with self.subTest(module=module):
                sources = exporter.go_process_shared_sources(module)
                for filename in ("db.go", "store_client.go", "store_wire.go"):
                    self.assertIn(f"server-go/db/{filename}", sources)
                self.assertFalse(any(path.endswith("_test.go") for path in sources))
                self.assertTrue(all((REPO_ROOT / path).is_file() for path in sources))
        for module in ("aimee", "economizer"):
            with self.subTest(module=module):
                sources = exporter.go_process_shared_sources(module)
                self.assertIn("server-go/aimee/client.go", sources)
                self.assertFalse(any(path.startswith("server-go/db1/") for path in sources))
        for module in ("aimee", "memory", "economizer", "delegates", "governance"):
            with self.subTest(ir_consumer=module):
                sources = exporter.go_process_shared_sources(module)
                self.assertIn("server-go/ir/session.go", sources)
                self.assertIn("server-go/ir/request.go", sources)
                self.assertNotIn("server-go/ir/contract_test.go", sources)
        self.assertIn("server-go/modules/egress/egress.go", exporter.go_process_shared_sources("memory"))
        self.assertIn("server-go/modules/audit/action.go", exporter.go_process_shared_sources("memory"))

    @unittest.skipUnless(shutil.which("go"), "go is not installed")
    def test_aimee_export_builds_with_its_embedded_schema_and_process(self) -> None:
        # Exercise the actual independent repository, not merely generator text:
        # the previous generic entry point referred to a nonexistent Handle and
        # the descriptor silently omitted every go:embed migration input.
        contract = exporter.process_contracts.validate()["aimee"]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            exporter.export_module(root, "aimee", "required", contract,
                                   exporter.source_timestamp(),
                                   exporter.CORE_VERSION_FILE.read_text().strip())
            module = root / "aimee-module-aimee"
            sql_files = list((REPO_ROOT / "server-go/modules/aimee/families").glob("schema_*.sql"))
            self.assertTrue(sql_files)
            cmake = (module / "CMakeLists.txt").read_text()
            for source in sql_files:
                relative = source.relative_to(REPO_ROOT)
                self.assertEqual((module / relative).read_bytes(), source.read_bytes())
                self.assertIn(str(relative), cmake)
            binary = module / "aimee-module-aimee"
            build = subprocess.run(["go", "build", "-o", str(binary), "./runtime"],
                                   cwd=module, capture_output=True, text=True, timeout=180)
            self.assertEqual(build.returncode, 0, build.stderr)
            usage = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(usage.returncode, 2)
            self.assertIn("DAEMON_MODULE_BUS_SOCKET", usage.stderr)
            absent = subprocess.run([str(binary), str(root / "absent.sock")],
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(absent.returncode, 1)
            self.assertIn("no store backend", absent.stderr)

    @unittest.skipUnless(shutil.which("go"), "go is not installed")
    def test_postgres_export_serves_private_sessions(self) -> None:
        import struct
        import time
        contract = exporter.process_contracts.validate()["postgres"]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            exporter.export_module(root, "postgres", "required", contract,
                                   exporter.source_timestamp(),
                                   exporter.CORE_VERSION_FILE.read_text().strip())
            module = root / "aimee-module-postgres"
            binary = module / "postgres"
            build = subprocess.run(["go", "build", "-o", str(binary), "./runtime"],
                                   cwd=module, capture_output=True, text=True, timeout=180)
            self.assertEqual(build.returncode, 0, build.stderr)
            # Invalid op is refused before a connection. The framed reply proves
            # that the independent executable supports the private provider ABI.
            credential = struct.pack("<I", 0) + b"postgres://fixture@127.0.0.1:1/fixture?sslmode=disable"
            request = struct.pack("<QIQ", time.monotonic_ns() + 2_000_000_000, 0, 0)
            wire = b"".join(struct.pack("<I", len(part)) + part for part in (credential, request))
            result = subprocess.run([str(binary), "__aimee_postgres_local_session"],
                                    input=wire, capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertGreater(len(result.stdout), 12)
            self.assertEqual(struct.unpack_from("<I", result.stdout)[0], len(result.stdout) - 4)
            self.assertIn(b"08006", result.stdout)

    def test_audit_publication_is_not_an_arbitrary_request_grant(self) -> None:
        client = {"id": "memory-postgres", "principal_ref": 73,
                  "executable": "/module", "placements": ["server", "kb"],
                  "request": [11266], "publish": [3000]}
        exporter.process_contracts.validate_clients([client], {11266}, {7})
        for publish in ([11266], [3000, 3000], [True], [{}], "3000"):
            with self.subTest(publish=publish), self.assertRaises(exporter.process_contracts.ContractError):
                exporter.process_contracts.validate_clients(
                    [{**client, "publish": publish}], {11266}, {7})

    @unittest.skipUnless(shutil.which("go"), "go is not installed")
    def test_memory_export_builds_with_all_owner_sources(self) -> None:
        contract = exporter.process_contracts.validate()["memory"]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            exporter.export_module(root, "memory", "required", contract,
                                   exporter.source_timestamp(),
                                   exporter.CORE_VERSION_FILE.read_text().strip())
            module = root / "aimee-module-memory"
            for source in (REPO_ROOT / "server-go/modules/memory").rglob("*.go"):
                relative = source.relative_to(REPO_ROOT)
                self.assertEqual((module / relative).read_bytes(), source.read_bytes())
            build = subprocess.run(["go", "build", "-o", str(module / "memory"), "./runtime"],
                                   cwd=module, capture_output=True, text=True, timeout=180)
            self.assertEqual(build.returncode, 0, build.stderr)
            tests = subprocess.run(["go", "test", "./server-go/modules/memory",
                                    "-run", "TestLifecycleVersionValidation|TestObservedRankingTrace|TestEvidenceCoverage|TestEvidenceRequirements|TestEvidenceRecovery|TestEvidenceTemporal|TestEvidenceComparison|TestEvidenceProcedure|TestMR05FrozenCoverage|TestUtilityHorizon|TestRequestCopySharesCodeInitialization", "-count=1"],
                                   cwd=module, capture_output=True, text=True, timeout=180)
            self.assertEqual(tests.returncode, 0, tests.stdout + tests.stderr)

    @unittest.skipUnless(shutil.which("go"), "go is not installed")
    def test_governance_export_builds_with_canonical_identity_contract(self) -> None:
        contract = exporter.process_contracts.validate()["governance"]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            exporter.export_module(root, "governance", "optional", contract,
                                   exporter.source_timestamp(),
                                   exporter.CORE_VERSION_FILE.read_text().strip())
            module = root / "aimee-module-governance"
            build = subprocess.run(["go", "build", "-o", str(module / "governance"), "./runtime"],
                                   cwd=module, capture_output=True, text=True, timeout=180)
            self.assertEqual(build.returncode, 0, build.stderr)
            tests = subprocess.run(["go", "test", "./server-go/modules/governance", "-count=1"],
                                   cwd=module, capture_output=True, text=True, timeout=180)
            self.assertEqual(tests.returncode, 0, tests.stdout + tests.stderr)

    def test_discovery_stage_keeps_its_reserved_slot(self) -> None:
        contract = json.loads(exporter.process_contracts.CONTRACTS.read_text())
        exporter.process_contracts.validate()
        memory = next(c for c in contract["components"] if c["id"] == "memory")
        discovery = memory["stages"][-1]
        self.assertEqual(discovery["id"], 255)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "contracts.json"
            for stage_id, name in ((254, "public-command-discovery"), (255, "other-stage")):
                discovery.update(id=stage_id, name=name, event_kind=4096 + 7 * 256 + stage_id)
                path.write_text(json.dumps(contract))
                with mock.patch.object(exporter.process_contracts, "CONTRACTS", path), \
                        self.assertRaises(exporter.process_contracts.ContractError):
                    exporter.process_contracts.validate()

    def descriptor(self) -> dict[str, object]:
        return {
            "sources": [
                "src/modules/kb/c/db_postgres.c",
                "src/modules/kb/c/kb_store_init.c",
            ],
            "go_sources": ["server-go/modules/kb/kb.go"],
        }

    def test_owned_files_include_contract_sources(self) -> None:
        descriptor = self.descriptor()
        descriptor["contracts"] = ["src/modules/kb/eventcontract/operations.json"]
        owned = exporter.module_owned_files("kb", descriptor)
        self.assertIn("src/modules/kb/eventcontract/operations.json", owned)
        descriptor["contracts"] = "src/modules/kb/eventcontract/operations.json"
        with self.assertRaisesRegex(exporter.ExportError, "contracts must be a string array"):
            exporter.module_owned_files("kb", descriptor)

    def test_external_module_pin_tracks_the_canonical_go_dependency(self) -> None:
        descriptor = exporter.load_json(REPO_ROOT / "src/modules/config/module.yaml")
        contract = exporter.process_contracts.validate()["config"]
        pin = exporter.external_module_pin("config", "required", descriptor, contract)
        self.assertIsNotNone(pin)
        assert pin is not None
        dependency = exporter.go_dependency_version(
            "github.com/RakuenSoftware/aimee-module-config"
        )
        self.assertEqual(pin["repository"],
                         "https://github.com/RakuenSoftware/aimee-module-config.git")
        self.assertEqual(pin["ref"], dependency)
        self.assertEqual(pin["version"], dependency)
        self.assertEqual(pin["commit"], dependency.rsplit("-", 1)[-1])

    def test_exported_roles_and_infrastructure_preserve_bootstrap_modes(self) -> None:
        for role in ("server", "kb"):
            main = exporter.go_module_main(role, 34, [])
            self.assertIn("identity.Bootstrap(os.Args)", main)
            self.assertIn('handler.NewHandler(os.Getenv("AIMEE_HOME"))', main)
            self.assertIn("handler.Supervise(ctx", main)
            self.assertNotIn("handler.Handle", main)
        self.assertIn("storage.Bootstrap(os.Args)", exporter.go_module_main("postgres", 26, []))
        self.assertIn("handler.ModelServicesBootstrap(os.Args)", exporter.go_module_main("providers", 17, []))

    def test_exported_admission_process_uses_owner_poll_ceiling(self) -> None:
        main = exporter.go_module_main("economizer", 27, [])
        self.assertIn("MaxIdlePollInterval: handler.RequestAdmissionPollInterval", main)
        self.assertIn("Handler: handler.NewHandler()", main)
        self.assertNotIn("MaxIdlePollInterval", exporter.go_module_main("memory", 7, []))

    def test_final_request_admission_is_started_and_granted(self) -> None:
        # Exercise the shipping descriptors, rather than merely proving the
        # multicall binary knows a stage that no deployed process will serve.
        with tempfile.TemporaryDirectory() as directory:
            bundle = Path(directory) / "bundle"
            exporter.export_runtime_bundle(bundle)
            row = "economizer\t/usr/local/libexec/aimee-modules/aimee-module-economizer"
            self.assertIn(row, (bundle / "server.modules").read_text().splitlines())
            self.assertNotIn(row, (bundle / "kb.modules").read_text().splitlines())
            grant = (bundle / "grants/server/economizer.grant").read_text()
            serve = next(line.removeprefix("serve=") for line in grant.splitlines()
                         if line.startswith("serve="))
            self.assertIn("11016", serve.split(","))
            lock = json.loads(exporter.LOCK.read_text())
            pin = next(module for module in lock["modules"] if module["id"] == "economizer")
            self.assertEqual(pin["serve"], [int(kind) for kind in serve.split(",")])

    def runtime_bundle(self, runtime: str, temporary: str) -> Path:
        root = Path(temporary) / "repo"
        bundle = Path(temporary) / "bundle"
        descriptor_path = root / "src/modules/kb/module.yaml"
        descriptor_path.parent.mkdir(parents=True)
        descriptor_path.write_text(json.dumps({"id": "kb", **self.descriptor()}),
                                   encoding="utf-8")
        inventory = root / "inventory.json"
        inventory.write_text(json.dumps({"required": ["kb"], "optional": []}), encoding="utf-8")
        contracts_path = root / "process-contracts.json"
        contracts_path.write_text(json.dumps({"clients": [{
            "id": "memory-postgres", "principal_ref": 73,
            "executable": "/module", "placements": ["server", "kb"],
            "request": [11266], "publish": [3000],
        }]}), encoding="utf-8")
        contract = {
            "execution": "process",
            "runtime": runtime,
            "principal_ref": 29,
            "placements": ["kb"],
            "stages": [{"id": 1, "name": "lifecycle", "event_kind": 11521}],
        }
        with mock.patch.object(exporter, "ROOT", root), \
                mock.patch.object(exporter, "INVENTORY", inventory), \
                mock.patch.object(exporter.process_contracts, "CONTRACTS", contracts_path), \
                mock.patch.object(exporter.process_contracts, "validate",
                                  return_value={"kb": contract}):
            self.assertEqual(exporter.export_runtime_bundle(bundle), 1)
        return bundle

    def test_runtime_bundle_spawns_go_processes_and_grants_clients(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            bundle = self.runtime_bundle("go", temporary)
            for placement in ("server", "kb"):
                grant = (bundle / "grants" / placement / "memory-postgres.grant").read_text()
                self.assertIn("publish=3000\n", grant)
                self.assertIn("request=11266\n", grant)
                self.assertIn("subscribe=\n", grant)
                self.assertIn("serve=\n", grant)
            self.assertEqual((bundle / "go.modules").read_text(encoding="utf-8"), "kb\n")
            self.assertIn("kb\t/usr/local/libexec/aimee-modules/aimee-module-kb",
                          (bundle / "kb.modules").read_text(encoding="utf-8"))
            self.assertFalse((bundle / "src").exists())
            self.assertFalse((bundle / "c-build.json").exists())

    def test_runtime_bundle_refuses_a_c_module_process(self) -> None:
        # Every module process is Go; the C module-process build was retired
        # with the last C adapter, so a contract asking for one fails closed.
        with tempfile.TemporaryDirectory() as temporary, \
                self.assertRaisesRegex(exporter.ExportError, "unsupported process runtime 'c'"):
            self.runtime_bundle("c", temporary)

if __name__ == "__main__":
    unittest.main()
