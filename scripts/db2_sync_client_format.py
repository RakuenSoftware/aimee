"""Reconcile the DB2 client-source template with clang-format's preference.

Every operation added to the DB2 event bus used to cost one round trip: write
the call wrapper into gen_db2_contract.py, run the generator, discover
clang-format wraps the signature differently from what was typed, hand-edit the
template, run the generator again. The wrapping depends on identifier length, so
it is not predictable while writing.

The template for src/modules/db2/client/generated.c is a plain triple-quoted
string with no interpolation, which makes the fix mechanical: format the
generated file, and where the formatter changed a definition, put the formatted
text back into the template it came from.

    python3 scripts/gen_db2_contract.py --write
    python3 scripts/db2_sync_client_format.py
    python3 scripts/gen_db2_contract.py --write

The second generator run emits what the formatter already agreed to. Nothing
else is touched: the other generated C is built from f-strings where braces are
escaped, so a naive sync-back would corrupt them, and in practice the drift has
all been in this one file.
"""
import subprocess
import sys
from pathlib import Path

GENERATOR = Path("scripts/gen_db2_contract.py")
GENERATED = Path("src/modules/db2/client/generated.c")
FORMATTER = "clang-format-19"


def _formatted(text: str) -> str:
    # Format through a file inside the tree so the repository's .clang-format
    # applies; the formatter resolves its configuration by path.
    scratch = GENERATED.with_name("_sync_client_format_tmp.c")
    scratch.write_text(text, encoding="utf-8")
    try:
        subprocess.run([FORMATTER, "-i", str(scratch)], check=True)
        return scratch.read_text(encoding="utf-8")
    finally:
        scratch.unlink(missing_ok=True)


def _definitions(text: str) -> list[str]:
    """Split C into top-level definitions, the granularity that moves."""
    blocks, current = [], []
    for line in text.split("\n"):
        current.append(line)
        if line == "}":
            blocks.append("\n".join(current))
            current = []
    if current:
        blocks.append("\n".join(current))
    return blocks


def main() -> int:
    if not GENERATED.is_file() or not GENERATOR.is_file():
        print("run from the repository root", file=sys.stderr)
        return 2

    source = GENERATED.read_text(encoding="utf-8")
    wanted = _formatted(source)
    if wanted == source:
        print("db2_sync_client_format: already formatted")
        return 0

    generator = GENERATOR.read_text(encoding="utf-8")
    before, after = _definitions(source), _definitions(wanted)
    if len(before) != len(after):
        print("db2_sync_client_format: formatting changed the definition structure; "
              "reconcile by hand", file=sys.stderr)
        return 1

    patched = 0
    for old, new in zip(before, after):
        if old == new:
            continue
        occurrences = generator.count(old)
        if occurrences != 1:
            print(f"db2_sync_client_format: {occurrences} matches in the template for a "
                  "changed definition; reconcile by hand", file=sys.stderr)
            return 1
        generator = generator.replace(old, new)
        patched += 1

    GENERATOR.write_text(generator, encoding="utf-8")
    print(f"db2_sync_client_format: reconciled {patched} definition(s); "
          "re-run the generator to emit them")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
