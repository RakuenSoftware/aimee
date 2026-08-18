#!/usr/bin/env python3
"""Generate C string constants from descriptor-declared UTF-8 text inputs."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import sys
import tempfile
from typing import NoReturn


C_IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


class GenerationError(ValueError):
    """A deterministic embedded-header input is malformed."""


def fail(message: str) -> NoReturn:
    raise GenerationError(message)


def generate(output: Path, entries: list[tuple[str, Path]]) -> None:
    if not entries:
        fail("at least one SYMBOL PATH entry is required")
    keys = [(symbol, path.as_posix()) for symbol, path in entries]
    if keys != sorted(set(keys)):
        fail("entries must be sorted and unique by symbol and path")
    symbols = [symbol for symbol, _path in entries]
    if len(symbols) != len(set(symbols)):
        fail("entry symbols must be unique")

    lines = ["/* Auto-generated from descriptor build inputs; do not edit. */"]
    for symbol, source in entries:
        if not C_IDENTIFIER.fullmatch(symbol):
            fail(f"invalid C identifier: {symbol!r}")
        try:
            if source.is_symlink() or not source.is_file():
                fail(f"input is not a regular file: {source}")
            value = source.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as exc:
            fail(f"cannot read {source}: {exc}")
        lines.append(
            f"static const char *{symbol} __attribute__((unused)) = "
            f"{json.dumps(value, ensure_ascii=True)};"
        )

    output.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=f".{output.name}.", dir=output.parent)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            stream.write("\n".join(lines) + "\n")
        os.replace(temporary, output)
    except BaseException:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--entry", action="append", nargs=2, metavar=("SYMBOL", "PATH"),
                        default=[])
    args = parser.parse_args(argv)
    try:
        generate(args.output, [(symbol, Path(path)) for symbol, path in args.entry])
    except GenerationError as exc:
        print(f"generate_c_embedded_header: error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
