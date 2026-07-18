#!/usr/bin/env python3
"""proposal_stats.py: summarize docs/proposals/{pending,done} pipeline state.

Reads files only. Never writes to disk. Stdlib-only.
"""
from __future__ import annotations

import argparse
import codecs
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
    # Use os.path.commonpath (not startswith) so /foo/barbaz cannot be
    # misread as living inside /foo/bar. realpath() on both sides closes
    # the symlink-race gap left by the is_symlink guard above.
    real_path_abs = os.path.realpath(real_path)
    real_root_abs = os.path.realpath(real_root)
    try:
        return os.path.commonpath([real_path_abs, real_root_abs]) == real_root_abs
    except ValueError:
        return False


def _list_files(dir_path: str, root: str) -> list[str]:
    # Walk strictly via lstat metadata (follow_symlinks=False), so a symlink
    # under pending/ or done/ cannot trick us into reading content from
    # outside docs/proposals. We never follow links or call isfile on a
    # realpath; symlinks and other non-regular entries are skipped with a
    # warning instead of raising. Treating an escape as non-fatal keeps the
    # script usable even if a contributor accidentally drops a dangling or
    # wrong-target symlink into the queue, which matches the acceptance
    # criterion that missing/escaped dirs be handled gracefully.
    #
    # Caveat: between the lstat/realpath validation here and the open() in
    # _count_words, an attacker who can write inside docs/proposals could
    # atomically replace a verified regular file with a symlink that points
    # outside the tree. The script does not claim to defend against that
    # TOCTOU race; this script is intended for trusted operator use, not
    # adversarial input. If untrusted proposals ever land here, switch to
    # opening via O_NOFOLLOW on a directory fd or equivalent.
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
    # memory: stream each file in chunks. We use an incremental UTF-8
    # decoder so a multibyte character split across a 64 KiB read boundary
    # does not raise UnicodeDecodeError or get double-counted, and we
    # carry the trailing partial token across chunks so a word straddling
    # the boundary is counted exactly once. NUL bytes short-circuit the
    # rest of the file as binary, matching prior behavior.
    total = 0
    for path in files:
        try:
            try:
                fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
            except OSError as exc:
                print(
                    f"warning: skipping {path}: open failed: {exc}",
                    file=sys.stderr,
                )
                binary = True
            else:
                try:
                    st_fd = os.fstat(fd)
                    st_path = os.stat(path, follow_symlinks=False)
                except OSError as exc:
                    print(
                        f"warning: skipping {path}: stat failed: {exc}",
                        file=sys.stderr,
                    )
                    os.close(fd)
                    binary = True
                else:
                    if (
                        not stat.S_ISREG(st_fd.st_mode)
                        or st_fd.st_ino != st_path.st_ino
                        or st_fd.st_dev != st_path.st_dev
                    ):
                        print(
                            f"warning: skipping {path}: not a regular file",
                            file=sys.stderr,
                        )
                        os.close(fd)
                        binary = True
                    else:
                        binary = False
            if not binary:
                decoder = codecs.getincrementaldecoder("utf-8")(
                    errors="strict"
                )
                pending = ""
                with os.fdopen(fd, "rb") as fh:
                    while True:
                        chunk = fh.read(_READ_CHUNK_BYTES)
                        if not chunk:
                            try:
                                text = decoder.decode(b"", final=True)
                            except UnicodeDecodeError as exc:
                                print(
                                    f"warning: skipping {path}: not valid utf-8: {exc}",
                                    file=sys.stderr,
                                )
                                binary = True
                                break
                            if pending or text:
                                total += len((pending + text).split())
                            break
                        if b"\x00" in chunk:
                            print(
                                f"warning: skipping {path}: appears to be binary",
                                file=sys.stderr,
                            )
                            binary = True
                            break
                        try:
                            text = decoder.decode(chunk, final=False)
                        except UnicodeDecodeError as exc:
                            print(
                                f"warning: skipping {path}: not valid utf-8: {exc}",
                                file=sys.stderr,
                            )
                            binary = True
                            break
                        # Prepend the unfinished token from the previous chunk
                        # so a word split across the boundary is one whole token.
                        tokens = (pending + text).split()
                        pending = ""
                        if text and not text[-1].isspace():
                            # Last token may continue into the next chunk.
                            pending = tokens.pop()
                        total += len(tokens)
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
        "pending": len(pending_files),
        "done": len(done_files),
        "pending_words": pending_words,
    }


def _format_human(stats: dict[str, object]) -> str:
    label_width = max(len(k) for k in stats) + 2
    lines = [
        f"{key + ':':<{label_width}}{stats[key]}"
        for key in ("pending", "done", "pending_words")
    ]
    return "\n".join(lines) + "\n"


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
