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


def _real_within(real_path: str, real_root: str) -> bool:
    return real_path == real_root or real_path.startswith(real_root + os.sep)


def _ensure_real_dir(dir_path: str, root: str) -> None:
    # Refuse to traverse a symlinked state directory. Following it would let
    # docs/proposals/pending -> /etc cause us to enumerate and read files
    # outside the canonical proposals root, violating read scope.
    if os.path.islink(dir_path):
        raise RuntimeError(
            f"refusing to read {dir_path}: state directory is a symlink"
        )
    if not os.path.isdir(dir_path):
        return
    real_dir = os.path.realpath(dir_path)
    real_root = os.path.realpath(root)
    if not _real_within(real_dir, real_root):
        raise RuntimeError(
            f"refusing to read {dir_path}: resolves outside proposals root "
            f"{root}"
        )


def _list_files(dir_path: str, root: str) -> list[str]:
    _ensure_real_dir(dir_path, root)
    if not os.path.isdir(dir_path):
        return []
    # Use os.scandir with follow_symlinks=False so each entry is classified by
    # the link itself, not its target. A link under pending/ or done/ would
    # otherwise let us read content from outside docs/proposals.
    real_root = os.path.realpath(root)
    candidates: list[str] = []
    with os.scandir(dir_path) as it:
        for entry in it:
            if entry.is_file(follow_symlinks=False):
                real_path = os.path.realpath(entry.path)
                if not _real_within(real_path, real_root):
                    raise RuntimeError(
                        f"refusing to read {entry.path}: resolves outside "
                        f"proposals root {root}"
                    )
                candidates.append(entry.path)
    candidates.sort()
    return candidates


def _count_words(files: list[str], proposals_root: str) -> int:
    real_root = os.path.realpath(proposals_root)
    total = 0
    for path in files:
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                total += len(fh.read().split())
        except OSError as exc:
            raise RuntimeError(
                f"failed to read {path}: {exc}"
            ) from exc
    return total


def _collect(proposals_root: str) -> dict[str, int]:
    pending_dir = os.path.join(proposals_root, "pending")
    done_dir = os.path.join(proposals_root, "done")

    pending_files = _list_files(pending_dir, proposals_root)
    done_files = _list_files(done_dir, proposals_root)

    return {
        "pending": len(pending_files),
        "done": len(done_files),
        "pending_words": _count_words(pending_files, proposals_root),
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

    try:
        stats = _collect(_proposals_root())
    except RuntimeError as exc:
        sys.stderr.write(f"proposal_stats: {exc}\n")
        return 1

    if args.as_json:
        json.dump(stats, sys.stdout)
        sys.stdout.write("\n")
    else:
        sys.stdout.write(_format_human(stats))

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
