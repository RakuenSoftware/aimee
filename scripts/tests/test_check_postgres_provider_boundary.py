#!/usr/bin/env python3
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("boundary", Path(__file__).resolve().parents[1] / "check_postgres_provider_boundary.py")
BOUNDARY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BOUNDARY)

class ProviderBoundaryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.contract = {"components": [{"id": "postgres", "runtime": "go", "execution": "process", "placements": ["server", "kb"], "stages": [{"id": i, "event_kind": 11264+i} for i in (1, 2, 3)]}]}
        self.write_contract()
        self.write("src/Makefile", "L_SERVER = -lpthread\nL_KB = -lpthread\n")
    def write(self, name, value):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(value)
    def write_contract(self):
        self.write("src/modules/process-contracts.json", json.dumps(self.contract))
    def test_provider_and_independent_fixtures_are_allowed(self):
        self.write("server-go/modules/postgres/sql.go", "pgxpool.New(ctx, dsn)")
        self.write("src/tests/fixture.c", "PQconnectdb(dsn);")
        self.assertEqual([], BOUNDARY.check(self.root))
    def test_retired_tree_fails(self):
        self.write("src/modules/db2/renamed.c", "")
        self.assertTrue(BOUNDARY.check(self.root))
    def test_native_driver_fails_even_outside_modules(self):
        self.write("src/hidden.c", "PQconnectdb(dsn);")
        self.assertTrue(BOUNDARY.check(self.root))
    def test_go_domain_cannot_open_pool(self):
        self.write("server-go/modules/kb/hidden.go", "pgxpool.NewWithConfig(ctx, cfg)")
        self.assertTrue(BOUNDARY.check(self.root))
    def test_role_cannot_omit_provider(self):
        self.contract["components"][0]["placements"] = ["server"]
        self.write_contract()
        self.assertTrue(BOUNDARY.check(self.root))
    def test_legacy_contract_fails(self):
        self.contract["components"].append({"id": "db2"})
        self.write_contract()
        self.assertTrue(BOUNDARY.check(self.root))
    def test_native_link_cannot_restore_driver(self):
        self.write("src/Makefile", "L_SERVER = $(PQ_LIB)\n")
        self.assertTrue(BOUNDARY.check(self.root))
    def test_cmake_cannot_restore_native_driver(self):
        self.write("CMakeLists.txt", "find_package(PostgreSQL REQUIRED)\n")
        self.assertTrue(BOUNDARY.check(self.root))

if __name__ == "__main__":
    unittest.main()
