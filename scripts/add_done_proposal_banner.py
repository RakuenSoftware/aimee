#!/usr/bin/env python3
"""Stamp the archival banner onto every proposal in docs/proposals/done/.

A proposal in done/ is a record of a design as it was agreed, not a description
of how the system behaves now. Read as documentation it is actively misleading
wherever the implementation has since moved on -- and several have. The banner
says so at the top of the file, where anyone (or any agent) starting from a
proposal will see it before building a mental model from it.

Idempotent: files that already carry the banner are left alone. Run from the
repository root.
"""

import pathlib
import sys

DONE_DIR = pathlib.Path("docs/proposals/done")

MARKER = "> **Archived proposal.**"
BANNER = (
    "> **Archived proposal.** This records the design as it was agreed, not the\n"
    "> system as it behaves today; parts of it have since diverged. For current\n"
    "> behaviour see `docs/`, or the code.\n"
)


def stamp(path: pathlib.Path) -> bool:
    """Insert the banner after the title heading. Returns True if changed."""
    text = path.read_text(encoding="utf-8")
    if MARKER in text:
        return False

    lines = text.splitlines(keepends=True)

    # Insert after the first ATX heading so the banner sits under the title
    # rather than above it. A file with no heading gets the banner at the top.
    insert_at = 0
    for i, line in enumerate(lines):
        if line.startswith("# "):
            insert_at = i + 1
            break

    block = []
    # Keep exactly one blank line on each side of the banner.
    if insert_at > 0 and not (insert_at < len(lines) and lines[insert_at].strip() == ""):
        block.append("\n")
    elif insert_at > 0:
        insert_at += 1
    block.append("\n" if insert_at == 0 else "")
    block.append(BANNER)
    block.append("\n")

    lines[insert_at:insert_at] = [b for b in block if b]
    path.write_text("".join(lines), encoding="utf-8")
    return True


def main() -> int:
    if not DONE_DIR.is_dir():
        print(f"{DONE_DIR} not found -- run from the repository root", file=sys.stderr)
        return 1

    changed = skipped = 0
    for path in sorted(DONE_DIR.glob("*.md")):
        if stamp(path):
            changed += 1
        else:
            skipped += 1

    print(f"banner: {changed} stamped, {skipped} already carried it")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
