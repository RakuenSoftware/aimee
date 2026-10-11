#!/usr/bin/env python3
"""Snapshot Hillock release-candidate branch tips for immutable CI qualification."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

REMOTE = "https://github.com/roandejager/Hillock.git"
BRANCH = re.compile(r"rc/v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)")
REVISION = re.compile(r"[0-9a-f]{40}")


def candidates(listing, selected=""):
    if selected and not BRANCH.fullmatch(selected):
        raise ValueError("branch must use rc/v<major>.<minor>.<patch>")
    found = {}
    seen = set()
    for line in listing.splitlines():
        fields = line.split()
        if len(fields) != 2:
            raise ValueError("malformed upstream branch listing")
        revision, ref = fields
        branch = ref.removeprefix("refs/heads/")
        if not ref.startswith("refs/heads/") or not BRANCH.fullmatch(branch):
            continue
        if not REVISION.fullmatch(revision) or revision == "0" * 40 or branch in seen:
            raise ValueError("invalid or duplicate upstream branch tip")
        seen.add(branch)
        if not selected or selected == branch:
            found[branch] = {"branch": branch, "revision": revision,
                             "branch_key": hashlib.sha256(branch.encode()).hexdigest()[:16]}
    if selected and not found:
        raise ValueError("requested Hillock release-candidate branch does not exist")
    if len(found) > 256:
        raise ValueError("Hillock candidates exceed the GitHub matrix limit; select a branch")
    return sorted(found.values(), key=lambda row: tuple(map(int, BRANCH.fullmatch(row["branch"]).groups())))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--branch", default="", help="optional single rc/vX.Y.Z branch")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--github-output", type=Path)
    args = parser.parse_args()
    if args.branch and not BRANCH.fullmatch(args.branch):
        parser.error("branch must use rc/v<major>.<minor>.<patch>")
    listing = subprocess.check_output(
        ["git", "ls-remote", "--heads", REMOTE, "refs/heads/rc/*"],
        text=True, timeout=60)
    rows = candidates(listing, args.branch)
    result = {"repository": REMOTE, "matrix": {"include": rows}, "count": len(rows)}
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    if args.github_output:
        with args.github_output.open("a") as out:
            out.write("matrix=" + json.dumps(result["matrix"], separators=(",", ":")) + "\n")
            out.write(f"count={len(rows)}\n")
    print(json.dumps(result))


if __name__ == "__main__":
    main()
