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

# Canonical key order for stats; shared by _collect and _format_human so
# iteration order is stable and the label column lines up regardless of
# the dict's insertion order or any future additions.
_STAT_KEYS: tuple[str, ...] = ("pending", "done", "pending_words")

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
    #
    # Words are accumulated in a per-file subtotal and only merged into the
    # running total once the entire file has been read and decoded without
    # error; this guarantees that a NUL byte, invalid UTF-8, or read error
    # encountered after some chunks have already been counted does not leak
    # partial words into the total while still being reported as skipped.
    # An open file descriptor is always closed via a top-level try/finally
    # so exceptions during fstat / fdopen / read cannot leak it.
    total = 0
    for path in files:
        subtotal = 0
        fd_owned: int | None = None
        skip_reason: str | None = None
        try:
            try:
                fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
            except OSError as exc:
                skip_reason = f"open failed: {exc}"
            else:
                fd_owned = fd
                try:
                    st_fd = os.fstat(fd)
                    st_path = os.stat(path, follow_symlinks=False)
                except OSError as exc:
                    skip_reason = f"stat failed: {exc}"
                else:
                    if (
                        not stat.S_ISREG(st_fd.st_mode)
                        or st_fd.st_ino != st_path.st_ino
                        or st_fd.st_dev != st_path.st_dev
                    ):
                        skip_reason = "not a regular file"
            if skip_reason is None:
                decoder = codecs.getincrementaldecoder("utf-8")(errors="strict")
                pending = ""
                fh = None
                try:
                    try:
                        fh = os.fdopen(fd_owned, "rb")
                    except OSError as exc:
                        skip_reason = f"fdopen failed: {exc}"
                    else:
                        try:
                            while True:
                                chunk = fh.read(_READ_CHUNK_BYTES)
                                if not chunk:
                                    try:
                                        text = decoder.decode(b"", final=True)
                                    except UnicodeDecodeError as exc:
                                        skip_reason = f"not valid utf-8: {exc}"
                                        break
                                    tokens = (pending + text).split()
                                    if tokens:
                                        subtotal += len(tokens)
                                    break
                                if b"\x00" in chunk:
                                    skip_reason = "appears to be binary"
                                    break
                                try:
                                    text = decoder.decode(chunk, final=False)
                                except UnicodeDecodeError as exc:
                                    skip_reason = f"not valid utf-8: {exc}"
                                    break
                                tokens = (pending + text).split()
                                pending = ""
                                if text and not text[-1].isspace():
                                    # Last token may continue into the next chunk.
                                    pending = tokens.pop()
                                subtotal += len(tokens)
                        except OSError as exc:
                            skip_reason = f"read failed: {exc}"
                finally:
                    if fh is not None:
                        fh.close()
        finally:
            if fd_owned is not None:
                try:
                    os.close(fd_owned)
                except OSError:
                    # os.fdopen took ownership and already closed it; the
                    # OSError from double-close is expected and safe to
                    # ignore here.
                    pass
        if skip_reason is not None:
            print(
                f"warning: skipping {path}: {skip_reason}", file=sys.stderr
            )
        else:
            total += subtotal
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
    # Iterate the canonical _STAT_KEYS order rather than the dict's own
    # insertion order so the output column alignment is stable regardless
    # of how _collect builds its dict, and raise if any expected key is
    # missing so a future refactor that drops a key fails loudly here
    # instead of silently misaligning the table.
    for key in _STAT_KEYS:
        if key not in stats:
            raise KeyError(f"missing required stat: {key}")
    label_width = max(len(k) for k in _STAT_KEYS) + 2
    lines = [
        f"{key + ':':<{label_width}}{stats[key]}" for key in _STAT_KEYS
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
