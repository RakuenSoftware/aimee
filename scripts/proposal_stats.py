#!/usr/bin/env python3
"""proposal_stats.py: summarize docs/proposals/{pending,done} pipeline state.

Reads files only. Never writes to disk. Stdlib-only.
"""
from __future__ import annotations

import argparse
import json
import os
import sys


def _proposals_root() -> str:
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.normpath(os.path.join(here, "..", "docs", "proposals"))


def _list_files(dir_path: str) -> list[str]:
    if not os.path.isdir(dir_path):
        return []
    # Use os.scandir so we can classify entries without following symlinks;
    # following them would let a link under pending/ or done/ cause us to
    # read content from outside docs/proposals, violating the read scope.
    candidates: list[str] = []
    with os.scandir(dir_path) as it:
        for entry in it:
            if entry.is_file(follow_symlinks=False):
                candidates.append(entry.path)
    candidates.sort()
    return candidates


def _count_words(files: list[str]) -> int:
    # "words" is defined as whitespace-separated tokens (str.split() with no
    # arguments). This matches any run of whitespace as a single delimiter
    # and intentionally double-counts hyphenated tokens like "state-machine"
    # as two tokens; callers comparing pending_words across runs see the
    # same definition each time.
    total = 0
    for path in files:
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                total += len(fh.read().split())
        except OSError:
            continue
    return total


def _collect(proposals_root: str) -> dict[str, int]:
    pending_dir = os.path.join(proposals_root, "pending")
    done_dir = os.path.join(proposals_root, "done")

    pending_files = _list_files(pending_dir)
    done_files = _list_files(done_dir)

    return {
        "pending": len(pending_files),
        "done": len(done_files),
        "pending_words": _count_words(pending_files),
    }


def _format_human(stats: dict[str, int]) -> str:
    return (
        f"pending: {stats['pending']}\n"
        f"done:    {stats['done']}\n"
        f"pending_words: {stats['pending_words']}\n"
    )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Summarize the docs/proposals pipeline (pending vs done)."
    )
    parser.add_argument(
        "--json",
        action="store_true",
        dest="as_json",
        help="Emit a single JSON object on stdout.",
    )
    args = parser.parse_args(argv)

    stats = _collect(_proposals_root())

    if args.as_json:
        json.dump(stats, sys.stdout)
        sys.stdout.write("\n")
    else:
        sys.stdout.write(_format_human(stats))

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
