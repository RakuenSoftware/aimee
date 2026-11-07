#!/usr/bin/env python3
"""Reject assistant identities in raw author/committer fields of new commits."""
from __future__ import annotations

import argparse
import re
import subprocess
import sys

AI_NAME = re.compile(r"(?:claude(?: code)?|codex|chatgpt|(?:github )?copilot|copilot-swe-agent)(?:\[bot\]|[- ]bot|[- ]agent)?", re.I)
AI_EMAILS = {"noreply@anthropic.com", "claude@anthropic.com", "codex@openai.com", "noreply@openai.com"}
AI_NOREPLY = re.compile(r"(?:\d+\+)?(?:claude|claude-code|codex|chatgpt|copilot|copilot-swe-agent)(?:\[bot\])?@users\.noreply\.github\.com", re.I)


def assistant_identity(name: str, email: str) -> bool:
    return bool(AI_NAME.fullmatch(name.strip()) or email.strip().casefold() in AI_EMAILS
                or AI_NOREPLY.fullmatch(email.strip()))


def check_range(base: str, head: str) -> list[str]:
    # Resolve refs before constructing a revision range, and read the raw fields:
    # mailmap substitutions (%aN/%aE) must not hide a forbidden Git identity.
    def git(*args: str) -> str:
        return subprocess.check_output(["git", *args], text=True, encoding="utf-8", errors="replace")
    base_sha = git("rev-parse", "--verify", "--end-of-options", base + "^{commit}").strip()
    head_sha = git("rev-parse", "--verify", "--end-of-options", head + "^{commit}").strip()
    problems = []
    for sha in git("rev-list", base_sha + ".." + head_sha).splitlines():
        fields = git("show", "-s", "--format=%an%x00%ae%x00%cn%x00%ce", sha).rstrip("\n").split("\0")
        if len(fields) != 4:
            raise ValueError(f"cannot read author/committer fields for {sha}")
        for label, name, email in (("author", *fields[:2]), ("committer", *fields[2:])):
            if assistant_identity(name, email):
                problems.append(f"Commit {sha} has an AI {label} identity: {name} <{email}>")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base")
    parser.add_argument("head")
    args = parser.parse_args()
    try:
        problems = check_range(args.base, args.head)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"::error::Cannot verify commit identities: {error}", file=sys.stderr)
        return 1
    for problem in problems:
        print(f"::error::{problem}", file=sys.stderr)
    if problems:
        return 1
    print("no AI author or committer identities in new commits — ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
