#!/usr/bin/env python3
"""Tests for deterministic descriptor-owned C text embedding."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
GENERATOR = REPO_ROOT / "scripts/generate_c_embedded_header.py"
SPEC = importlib.util.spec_from_file_location("generate_c_embedded_header", GENERATOR)
assert SPEC and SPEC.loader
generator = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(generator)


class EmbeddedHeaderTests(unittest.TestCase):
    def test_generation_is_exact_reproducible_and_c_escaped(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            alpha = root / "alpha.sql"
            beta = root / "beta.sql"
            output = root / "generated/schema_data.h"
            alpha.write_text("select 'a';\n", encoding="utf-8")
            beta.write_text("line one\nline \"two\"\\end\n", encoding="utf-8")
            entries = [("ALPHA_SQL", alpha), ("BETA_SQL", beta)]
            generator.generate(output, entries)
            first = output.read_bytes()
            generator.generate(output, entries)
            self.assertEqual(output.read_bytes(), first)
            text = first.decode("utf-8")
            self.assertIn('ALPHA_SQL __attribute__((unused)) = "select \'a\';\\n";', text)
            self.assertIn('BETA_SQL __attribute__((unused)) = "line one\\nline \\\"two\\\"\\\\end\\n";', text)

    def test_invalid_duplicate_unsorted_and_symlink_inputs_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "input.sql"
            source.write_text("select 1;", encoding="utf-8")
            output = root / "out.h"
            cases = (
                [("BAD=1", source)],
                [("B_SQL", source), ("A_SQL", source)],
                [("A_SQL", source), ("A_SQL", source)],
            )
            for entries in cases:
                with self.subTest(entries=entries), self.assertRaises(generator.GenerationError):
                    generator.generate(output, entries)

            link = root / "link.sql"
            link.symlink_to(source)
            with self.assertRaisesRegex(generator.GenerationError, "regular file"):
                generator.generate(output, [("LINK_SQL", link)])


if __name__ == "__main__":
    unittest.main()
