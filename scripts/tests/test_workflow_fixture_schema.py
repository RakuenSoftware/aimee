import importlib.util
from pathlib import Path
import unittest

PATH = Path(__file__).resolve().parents[1] / "validation/workflow-fixture-schema.py"
spec = importlib.util.spec_from_file_location("workflow_fixture_schema", PATH)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class WorkflowSchemaOracle(unittest.TestCase):
    def environment(self, suffix=""):
        return {"AIMEE_STORE_URL": "postgres://runtime:fixture@localhost/db" + suffix,
                "AIMEE_STORE_MIGRATION_URL": "postgres://migrator:fixture@localhost/db" + suffix}

    def snapshot(self, schema="tenant"):
        return {"schema": schema, "columns": [
            ["lifecycle_work_item", "id", 1, "text", "text", "NO", None],
            ["session_ir_events", "session_id", 1, "text", "text", "NO", None]]}

    def test_uri_scope_without_environment_is_preserved(self):
        env, expected = module.connection(self.environment("?search_path=tenant"))
        self.assertEqual(expected, "tenant")
        self.assertIn("-c search_path=tenant", env["PGOPTIONS"])
        self.assertIn("default_transaction_read_only=on", env["PGOPTIONS"])
        self.assertNotIn("search_path=", env["PGDATABASE"])
        self.assertEqual(module.admit_snapshot(self.snapshot(), expected), self.snapshot())

    def test_no_scope_does_not_fabricate_public(self):
        env, expected = module.connection(self.environment())
        self.assertIsNone(expected)
        self.assertNotIn("search_path", env["PGOPTIONS"])
        module.admit_snapshot(self.snapshot("actual_role_default"), expected)

    def test_environment_is_crosschecked_with_uri_and_actual_schema(self):
        env = self.environment("?search_path=tenant")
        env["AIMEE_STORE_SCHEMA"] = "public"
        with self.assertRaises(ValueError):
            module.connection(env)
        env["AIMEE_STORE_SCHEMA"] = "tenant"
        _, expected = module.connection(env)
        with self.assertRaises(ValueError):
            module.admit_snapshot(self.snapshot("public"), expected)

    def test_ambiguous_or_disagreeing_scopes_refuse(self):
        for suffix in ["?search_path=one&search_path=two", "?search_path=one,public",
                       "?options=-csearch_path%3Dother", "?search_path=" + "x" * 64]:
            with self.subTest(suffix=suffix), self.assertRaises(ValueError):
                module.connection(self.environment(suffix))
        env = self.environment("?search_path=one")
        env["AIMEE_STORE_MIGRATION_URL"] = env["AIMEE_STORE_MIGRATION_URL"].replace("one", "two")
        with self.assertRaises(ValueError):
            module.connection(env)

    def test_empty_or_unmigrated_snapshots_never_compare_as_proof(self):
        for value in [None, {}, {"schema": "tenant", "columns": []},
                      {"schema": "", "columns": self.snapshot()["columns"]},
                      {"schema": "tenant", "columns": [["unrelated", "id", 1, "text", "text", "NO", None]]}]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                module.admit_snapshot(value, "tenant")


if __name__ == "__main__":
    unittest.main()
