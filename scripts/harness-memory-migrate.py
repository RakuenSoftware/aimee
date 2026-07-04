#!/usr/bin/env python3
"""Migrate the .md harness-memory store into db1 as private, non-recallable
archival rows (Proposal 2 memory-arch — .md retirement, Slice 3).

SAFE by construction, per the migration design roundtable:
  - Every .md memory is imported to db1 with kind='archive' (tier L1) via the
    `aimee memory archive` command. kind='archive'/tier-L1 are outside the
    L2/fact|preference recall selectors, so nothing pollutes structured recall.
  - NOTHING is written to db2 (org). Defaulting all to db1/private means no
    private note can leak org-wide — the operator promotes to org/structured
    later, from the archive, deliberately.
  - The .md source and the harness_memory table are RETAINED (this tool never
    deletes). Subsystem removal is a separate, later, gated slice.
  - DRY-RUN by default. --apply performs the writes.

Idempotent: re-running updates the same archive row (ON CONFLICT(kind,key)).

Usage:
  harness-memory-migrate.py [--memory-dir DIR] [--aimee ./aimee] [--apply] [--limit N]
"""
import argparse
import os
import subprocess
import sys


def md_memories(memory_dir):
    """Yield (name, body) for each .md memory (name = path minus .md, matching
    the harness_memory `name`), skipping the MEMORY index."""
    for root, _dirs, files in os.walk(memory_dir):
        for f in sorted(files):
            if not f.endswith(".md"):
                continue
            path = os.path.join(root, f)
            name = os.path.relpath(path, memory_dir)[:-3]
            if name == "MEMORY":
                continue
            try:
                body = open(path, encoding="utf-8", errors="replace").read()
            except OSError as e:
                print(f"  SKIP {name}: {e}", file=sys.stderr)
                continue
            yield name, body


def main():
    ap = argparse.ArgumentParser(description="Migrate .md harness memory -> db1 archive (safe).")
    ap.add_argument("--memory-dir", default=os.path.expanduser(
        "~/.claude/projects/-home-virant-dev-aimee/memory"))
    ap.add_argument("--aimee", default="./aimee", help="path to the aimee CLI")
    ap.add_argument("--apply", action="store_true",
                    help="actually write to db1 (default is a dry run)")
    ap.add_argument("--limit", type=int, default=0, help="cap number migrated (0 = all)")
    a = ap.parse_args()

    if not os.path.isdir(a.memory_dir):
        print(f"migrate: no memory dir at {a.memory_dir}", file=sys.stderr)
        return 1

    items = list(md_memories(a.memory_dir))
    if a.limit > 0:
        items = items[:a.limit]

    mode = "APPLY" if a.apply else "DRY-RUN"
    print(f"=== .md -> db1 archive migration ({mode}) — {len(items)} memories ===")
    print("destination: db1 user_memories, kind='archive', tier L1 (NON-recallable, private).")
    print("db2 (org): nothing. source .md / harness_memory: RETAINED (no deletion).\n")

    ok = fail = 0
    for name, body in items:
        if not a.apply:
            print(f"  would archive: {name}  ({len(body.encode('utf-8'))} bytes)")
            ok += 1
            continue
        # key='archive:'+name, kind='archive'. Body goes via --content= (verbatim
        # after the first '='), so arbitrary bodies — including frontmatter '---'
        # and newlines — survive CLI flag parsing.
        try:
            r = subprocess.run([a.aimee, "memory", "archive", name, "--content=" + body],
                               capture_output=True, text=True, timeout=30)
        except (OSError, subprocess.SubprocessError) as e:
            print(f"  FAIL {name}: {e}", file=sys.stderr)
            fail += 1
            continue
        if r.returncode == 0:
            ok += 1
        else:
            fail += 1
            print(f"  FAIL {name}: {(r.stderr or r.stdout).strip()[:120]}", file=sys.stderr)

    print(f"\n{mode}: {ok} ok, {fail} failed of {len(items)}.")
    if not a.apply:
        print("Re-run with --apply to write. Nothing was changed.")
    else:
        print("Source retained. Verify with `aimee memory list --kind archive` (or the inventory),"
              " then classify/promote from the archive before any subsystem removal.")
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
