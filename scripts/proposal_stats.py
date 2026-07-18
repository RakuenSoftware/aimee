#!/usr/bin/env python3
"""proposal_stats.py: summarize docs/proposals/{pending,done} pipeline state.

Reads files only. Never writes to disk. Stdlib-only.
"""
from __future__ import annotations

import argparse
import json
import os
import stat
import sys


# Read pending files in 64 KiB chunks so very large proposals don't blow up
# memory. Word counts must include all text regardless of file size.
_READ_CHUNK_BYTES = 64 * 1024


def _proposals_root() -> str:
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.normpath(os.path.join(here, "..", "docs", "proposals"))


def _real_within(real_path: str, real_root: str) -> bool:
    return real_path == real_root or real_path.startswith(real_root + os.sep)


def _list_files(dir_path: str, root: str) -> list[str]:
    # Walk strictly via lstat metadata (follow_symlinks=False), so a symlink
    # under pending/ or done/ cannot trick us into reading content from
    # outside docs/proposals. We never follow links or call isfile on a
    # realpath; symlinks and other non-regular entries are skipped with a
    # warning instead of raising. Treating an escape as non-fatal keeps the
    # script usable even if a contributor accidentally drops a dangling or
    # wrong-target symlink into the queue, which matches the acceptance
    # criterion that missing/escaped dirs be handled gracefully.
    if not os.path.isdir(dir_path):
        return []
    real_root = os.path.realpath(root)
    candidates: list[str] = []
    with os.scandir(dir_path) as it:
        for entry in it:
            try:
                st = entry.stat(follow_symlinks=False)
            except OSError as exc:
                print(
                    f"warning: skipping {entry.path}: lstat failed: {exc}",
                    file=sys.stderr,
                )
                continue
            if entry.is_symlink():
                print(
                    f"warning: skipping {entry.path}: symlinks are not "
                    f"followed under {dir_path}",
                    file=sys.stderr,
                )
                continue
            if not stat.S_ISREG(st.st_mode):
                print(
                    f"warning: skipping {entry.path}: not a regular file",
                    file=sys.stderr,
                )
                continue
            real_path = os.path.realpath(entry.path)
            if not _real_within(real_path, real_root):
                print(
                    f"warning: skipping {entry.path}: resolves outside "
                    f"proposals root {root}",
                    file=sys.stderr,
                )
                continue
            candidates.append(entry.path)
    candidates.sort()
    return candidates


def _count_words(files: list[str]) -> int:
    # Count words across all pending files regardless of size. Bounded
    # memory: stream each file in chunks and split on whitespace so very
    # large files do not blow up RSS.
    total = 0
    for path in files:
        try:
            with open(path, "rb") as fh:
                while True:
                    chunk = fh.read(_READ_CHUNK_BYTES)
                    if not chunk:
                        break
                    if b"\x00" in chunk:
                        print(
                            f"warning: skipping {path}: appears to be binary",
                            file=sys.stderr,
                        )
                        break
                    try:
                        text = chunk.decode("utf-8")
                    except UnicodeDecodeError as exc:
                        print(
                            f"warning: skipping {path}: not valid utf-8: {exc}",
                            file=sys.stderr,
                        )
                        break
                    total += len(text.split())
        except OSError as exc:
            print(
                f"warning: skipping {path}: read failed: {exc}", file=sys.stderr
            )
    return total


def _collect(root: str) -> dict[str, object]:
    pending_dir = os.path.join(root, "pending")
    done_dir = os.path.join(root, "done")
    pending_files = _list_files(pending_dir, root)
    done_files = _list_files(done_dir, root)
    pending_words = _count_words(pending_files)
    return {
        "pending_count": len(pending_files),
        "done_count": len(done_files),
        "pending_words": pending_words,
    }


def _format_human(stats: dict[str, object]) -> str:
    return (
        f"pending_count: {stats['pending_count']}\n"
        f"done_count:    {stats['done_count']}\n"
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