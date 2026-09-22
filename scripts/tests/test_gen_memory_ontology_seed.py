#!/usr/bin/env python3
"""The Go owner generates schema and temporary native ontology data."""
from __future__ import annotations
import hashlib
import importlib.util
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
REPO = Path(__file__).resolve().parents[2]
OUTPUTS = ["src/modules/db2/c/schema_sqlite.sql", "src/rel_types.c", "src/modules/db2/c/schema.sql", "src/modules/db2/support/rel_seed_primitives.c", "server-go/modules/memory/testdata/ontology_seed.tsv"]

class OntologySeedGeneratorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="go-ontology-generator-")
        cls.generator = Path(cls.temp.name) / "seed"
        subprocess.run(["go", "build", "-o", str(cls.generator), "./modules/memory/cmd/aimee-memory-seed"], cwd=REPO / "server-go", env={**os.environ, "CGO_ENABLED": "0"}, check=True)
    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()
    def copy_outputs(self, root):
        for path in OUTPUTS:
            dst = root / path
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(REPO / path, dst)
    def run_generator(self, *args):
        return subprocess.run([str(self.generator), *map(str,args)], capture_output=True, text=True)
    def test_all_checked_in_outputs_match_go_owner(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            self.copy_outputs(root)
            result = self.run_generator("--root",root)
            self.assertEqual(result.returncode,0,result.stderr)
            for path in OUTPUTS:
                self.assertEqual((root/path).read_bytes(),(REPO/path).read_bytes(),path)
            result = self.run_generator("--root",root,"--check")
            self.assertEqual(result.returncode,0,result.stderr)
            spec = importlib.util.spec_from_file_location("closure",REPO/"scripts/check_db2_link_closure.py")
            checker = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(checker)
            policy = next(row for row in checker.SUPPORT_UNITS if row["path"]=="src/modules/db2/support/rel_seed_primitives.c")
            self.assertEqual(policy["source_sha256"],hashlib.sha256((root/policy["path"]).read_bytes()).hexdigest())
    def test_drift_is_detected_without_writing(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            self.copy_outputs(root)
            path = root/"src/modules/db2/c/schema.sql"
            path.write_text(path.read_text().replace("'works_for','person'","'works_for','other'",1))
            before = path.read_bytes()
            result = self.run_generator("--root",root,"--check")
            self.assertEqual(result.returncode,1)
            self.assertIn("generated ontology drift",result.stderr)
            self.assertEqual(path.read_bytes(),before)
    def test_invalid_region_fails_before_writes(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            self.copy_outputs(root)
            path = root/"src/rel_types.c"
            path.write_text(path.read_text().replace("/* END GO MEMORY ONTOLOGY SEED */","",1))
            before = {p:(root/p).read_bytes() for p in OUTPUTS}
            result = self.run_generator("--root",root)
            self.assertEqual(result.returncode,1)
            for p in OUTPUTS:
                self.assertEqual(before[p],(root/p).read_bytes())
    def test_standalone_db2_output_and_failure(self):
        with tempfile.TemporaryDirectory() as raw:
            path = Path(raw)/"seed.c"
            result = self.run_generator("--db2-output",path)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(path.read_bytes(),(REPO/"src/modules/db2/support/rel_seed_primitives.c").read_bytes())
            result = self.run_generator("--db2-output",Path(raw)/"missing"/"seed.c")
            self.assertEqual(result.returncode,1)
            result = self.run_generator("unexpected")
            self.assertEqual(result.returncode,2)

if __name__ == "__main__":
    unittest.main()
