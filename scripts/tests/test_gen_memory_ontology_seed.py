#!/usr/bin/env python3
"""Regeneration contract for the shared memory/DB2 ontology seed walk."""

from __future__ import annotations

import hashlib
import importlib.util
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]


def load_closure_checker():
    path = REPO / "scripts/check_db2_link_closure.py"
    spec = importlib.util.spec_from_file_location("check_db2_link_closure", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class OntologySeedGeneratorTests(unittest.TestCase):
    def build_generator(self, directory: Path, seed_source: Path | None = None) -> Path:
        output = directory / "gen-ontology-seed"
        seed_source = seed_source or REPO / "src/rel_types.c"
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            f"-I{REPO / 'src'}", f"-I{REPO / 'src/headers'}",
            "-o", str(output),
            str(REPO / "scripts/gen-memory-ontology-seed.c"),
            str(seed_source),
        ], check=True)
        return output

    def test_all_checked_in_outputs_match_one_compiled_table_walk(self) -> None:
        with tempfile.TemporaryDirectory(prefix="ontology-seed-generator-") as raw:
            directory = Path(raw)
            generator = self.build_generator(directory)
            generated_go = directory / "ontology_seed.go"
            generated_tsv = directory / "ontology_seed.tsv"
            generated_db2 = directory / "rel_seed_primitives.c"
            subprocess.run([
                str(generator), str(generated_go), str(generated_tsv), str(generated_db2),
            ], check=True)
            expected = {
                generated_go: REPO / "server-go/modules/memory/ontology_seed.go",
                generated_tsv: REPO / "server-go/modules/memory/testdata/ontology_seed.tsv",
                generated_db2: REPO / "src/modules/db2/support/rel_seed_primitives.c",
            }
            for generated, checked_in in expected.items():
                self.assertEqual(generated.read_bytes(), checked_in.read_bytes(), checked_in)

            checker = load_closure_checker()
            policy = next(
                row for row in checker.SUPPORT_UNITS
                if row["path"] == "src/modules/db2/support/rel_seed_primitives.c"
            )
            self.assertEqual(
                hashlib.sha256(generated_db2.read_bytes()).hexdigest(),
                policy["source_sha256"],
            )
            self.assertEqual(
                hashlib.sha256(
                    (REPO / "src/modules/db2/support/db2_rel_seed.h").read_bytes()
                ).hexdigest(),
                policy["header_sha256"],
            )

    def test_two_output_memory_invocation_remains_supported(self) -> None:
        with tempfile.TemporaryDirectory(prefix="ontology-seed-generator-") as raw:
            directory = Path(raw)
            generator = self.build_generator(directory)
            generated_go = directory / "ontology_seed.go"
            generated_tsv = directory / "ontology_seed.tsv"
            subprocess.run([str(generator), str(generated_go), str(generated_tsv)], check=True)
            self.assertEqual(
                generated_go.read_bytes(),
                (REPO / "server-go/modules/memory/ontology_seed.go").read_bytes(),
            )
            self.assertEqual(
                generated_tsv.read_bytes(),
                (REPO / "server-go/modules/memory/testdata/ontology_seed.tsv").read_bytes(),
            )

    def test_invalid_arity_fails_without_creating_outputs(self) -> None:
        with tempfile.TemporaryDirectory(prefix="ontology-seed-generator-") as raw:
            directory = Path(raw)
            generator = self.build_generator(directory)
            output = directory / "unexpected"
            completed = subprocess.run(
                [str(generator), str(output)], capture_output=True, text=True, check=False,
            )
            self.assertEqual(completed.returncode, 2)
            self.assertIn("usage:", completed.stderr)
            self.assertFalse(output.exists())

            too_many = subprocess.run(
                [str(generator), "a", "b", "c", "d"],
                cwd=directory, capture_output=True, text=True, check=False,
            )
            self.assertEqual(too_many.returncode, 2)
            self.assertIn("usage:", too_many.stderr)

    def test_unwritable_db2_output_fails_on_the_new_output_path(self) -> None:
        with tempfile.TemporaryDirectory(prefix="ontology-seed-generator-") as raw:
            directory = Path(raw)
            generator = self.build_generator(directory)
            missing_parent = directory / "missing" / "rel_seed_primitives.c"
            completed = subprocess.run([
                str(generator), str(directory / "seed.go"), str(directory / "seed.tsv"),
                str(missing_parent),
            ], capture_output=True, text=True, check=False)
            self.assertEqual(completed.returncode, 1)
            self.assertIn(str(missing_parent), completed.stderr)
            self.assertFalse(missing_parent.exists())

    def test_non_printable_seed_text_fails_before_creating_outputs(self) -> None:
        with tempfile.TemporaryDirectory(prefix="ontology-seed-generator-") as raw:
            directory = Path(raw)
            seed_source = directory / "non_printable_seed.c"
            seed_source.write_text(
                '#include "rel_types.h"\n'
                'static const rel_type_def_t ROW = {\n'
                ' .rel_type = "bad\\001name", .head_kinds = { NODE_OTHER },\n'
                ' .head_kind_count = 1, .tail_kinds = { NODE_OTHER },\n'
                ' .tail_kind_count = 1, .category = "test" };\n'
                'int rel_types_seed_count(void) { return 1; }\n'
                'const rel_type_def_t *rel_types_seed_at(int i) { return i == 0 ? &ROW : 0; }\n',
                encoding="utf-8",
            )
            generator = self.build_generator(directory, seed_source)
            outputs = [directory / "seed.go", directory / "seed.tsv", directory / "seed.c"]
            completed = subprocess.run(
                [str(generator), *(str(path) for path in outputs)],
                capture_output=True, text=True, check=False,
            )
            self.assertEqual(completed.returncode, 1)
            self.assertIn("non-printable byte 0x01", completed.stderr)
            self.assertTrue(all(not path.exists() for path in outputs))


if __name__ == "__main__":
    unittest.main()
