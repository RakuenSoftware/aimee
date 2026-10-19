#!/usr/bin/env python3
"""The committed event-bus vector tables are exactly what their generators
produce.

The C, Go and Rust clients are each held to src/tests/fixtures/bus/
wire_vectors.tsv and layout_vectors.tsv. Those tests catch a client drifting
from a table; this one catches a table drifting from its generator: a
hand-edited row, or a generator change committed without regenerating. Either
would let all three clients agree on something the specification does not say.
"""

from __future__ import annotations

from pathlib import Path
import subprocess
import sys
import unittest


REPO = Path(__file__).resolve().parents[2]
FIXTURES = REPO / "src/tests/fixtures/bus"
TABLES = {
    "wire_vectors.tsv": REPO / "scripts/gen_bus_wire_vectors.py",
    "layout_vectors.tsv": REPO / "scripts/gen_bus_layout_vectors.py",
}


def generate(generator: Path) -> str:
    out = subprocess.run(
        [sys.executable, "-I", str(generator)],
        check=True, capture_output=True, text=True, cwd=REPO,
    )
    return out.stdout


class BusVectorsCurrentTests(unittest.TestCase):
    def test_committed_tables_match_their_generators(self) -> None:
        for name, generator in TABLES.items():
            with self.subTest(table=name):
                committed = (FIXTURES / name).read_text(encoding="utf-8")
                fresh = generate(generator)
                if committed != fresh:
                    old, new = committed.splitlines(), fresh.splitlines()
                    changed = [f"- {line}" for line in old if line not in new]
                    changed += [f"+ {line}" for line in new if line not in old]
                    self.fail(
                        f"{name} differs from {generator.name}; regenerate with "
                        f"`{generator.relative_to(REPO)} > "
                        f"{(FIXTURES / name).relative_to(REPO)}` and review:\n"
                        + "\n".join(changed[:40])
                    )

    def test_tables_are_not_empty_and_names_are_unique(self) -> None:
        for name in TABLES:
            with self.subTest(table=name):
                rows = [
                    line.split("\t", 1)[0]
                    for line in (FIXTURES / name).read_text(encoding="utf-8").splitlines()
                    if line and not line.startswith("#")
                ]
                self.assertGreater(len(rows), 10)
                self.assertEqual(len(rows), len(set(rows)), f"duplicate rows in {name}")

    def test_layout_generator_refuses_a_structure_that_does_not_fill(self) -> None:
        # The generator's struct() check is what makes each size row agree with
        # its offsets; prove it fires rather than trusting it.
        source = TABLES["layout_vectors.tsv"].read_text(encoding="utf-8")
        broken = source.replace('("magic", 4), ("reserved", 4), ("size", 8)',
                                '("magic", 4), ("reserved", 2), ("size", 8)', 1)
        self.assertNotEqual(source, broken, "arena struct declaration not found")
        out = subprocess.run([sys.executable, "-I", "-c", broken],
                             capture_output=True, text=True, cwd=REPO)
        self.assertNotEqual(out.returncode, 0)
        self.assertIn("arena: fields fill 14 bytes, declared 16", out.stderr)


if __name__ == "__main__":
    unittest.main()
