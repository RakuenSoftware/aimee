#!/usr/bin/env python3
"""Enforce the Phase 0 anchor contract for changes to Phase 1+ surfaces.

The contract is the presence of ``docs/guardrails/collapse_anchors.md``
containing all six ``## Decision N`` headings (N = 1..6) on the **target
branch** that a Phase 1+ change is being proposed against.

On a ``pull_request`` run, the CI runner sets ``BASE_SHA`` to the head
of the base branch (the branch the PR targets) and the working tree
contains the PR head.  The contract is that the anchor document is
already merged on the target branch, so the gate must verify the
document's presence at ``BASE_SHA`` -- not just at the working tree.
Verifying only the working tree would let a PR whose target branch
does not yet carry the anchors pass, which is exactly the scenario
the Phase 0 contract forbids ("Phase 1 implementation does not start
until the anchors are merged").

Resolution of the contract:

1. ``BASE_SHA`` env var (CI ``pull_request`` events) -> diff is
   ``BASE_SHA..HEAD`` and the anchor MUST be present at ``BASE_SHA``
   (the target branch head).  This is the explicit PR-base/PR-head
   semantics required by F001.
2. Local repository with a usable ``HEAD~1`` -> diff is ``HEAD~1..HEAD``
   and the anchor MUST be present in the working tree (last-commit
   form).  This is the pre-PR dev workflow.
3. Local repository without ``HEAD~1`` (initial commit or shallow
   clone) -> diff is the working tree against ``HEAD`` and the anchor
   MUST be present in the working tree.
4. Otherwise the gate fails closed -- silently passing would defeat the
   enforcement contract.

Phase 1+ surfaces are listed in ``PHASE_ONE_PREFIXES``.  When at least
one of those paths is present in the diff being validated AND the
anchor contract is not satisfied, the gate fails; otherwise it passes.
"""
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ANCHORS = "docs/guardrails/collapse_anchors.md"
PHASE_ONE_PREFIXES = (
    "src/server/",
    "src/modules/guardrails/",
    "src/headers/aimee_ir.h",
    "src/modules/config/",
    "src/modules/audit/",
    "scripts/check-collapse-anchor-gate.py",
)


class GateError(RuntimeError):
    """Diff discovery or contract verification failed."""


def _git_text(*args: str, cwd: Path) -> str:
    """Run ``git`` with the given arguments and return stdout as text.

    Raises ``GateError`` on any non-zero exit or invocation failure so
    the gate fails closed when the diff cannot be computed.
    """
    try:
        return subprocess.check_output(
            ["git", *args], cwd=cwd, text=True,
        )
    except subprocess.CalledProcessError as exc:
        raise GateError(
            f"git {' '.join(args)} failed: {exc.stderr or exc}"
        ) from exc
    except OSError as exc:
        raise GateError(f"could not invoke git: {exc}") from exc


def diff_against_base(cwd: Path) -> list[str]:
    """Return the list of files changed in the diff being validated.

    Resolution order:

    1. ``BASE_SHA`` env var (set by CI for ``pull_request`` events) ->
       diff from ``BASE_SHA`` to ``HEAD`` (the explicit PR-base/PR-head
       semantics required by F001).
    2. Local repository with a usable ``HEAD~1`` -> diff from
       ``HEAD~1`` to ``HEAD`` (last-commit form).
    3. Local repository without ``HEAD~1`` (initial commit or shallow
       clone) -> diff from the working tree against ``HEAD``, so staged
       + unstaged edits are still validated.
    4. Otherwise the gate fails closed -- silently passing would
       defeat the enforcement contract.
    """
    base_sha = os.environ.get("BASE_SHA", "").strip()
    if base_sha:
        out = _git_text("diff", "--name-only", base_sha, "HEAD", cwd=cwd)
        return [line for line in out.splitlines() if line]

    try:
        has_parent = subprocess.run(
            ["git", "rev-parse", "--verify", "--quiet", "HEAD~1"],
            cwd=cwd, text=True,
        ).returncode == 0
    except OSError as exc:
        raise GateError(f"could not invoke git: {exc}") from exc
    if has_parent:
        out = _git_text("diff", "--name-only", "HEAD~1", "HEAD", cwd=cwd)
        return [line for line in out.splitlines() if line]

    out = _git_text("diff", "--name-only", "HEAD", cwd=cwd)
    return [line for line in out.splitlines() if line]


def phase_one_touched(paths: list[str]) -> bool:
    return any(path.startswith(PHASE_ONE_PREFIXES) for path in paths)


def _anchor_path_exists_at(ref: str, cwd: Path) -> bool:
    """Return True iff ``docs/guardrails/collapse_anchors.md`` exists in
    the given git revision (the target branch head on CI)."""
    try:
        _git_text("cat-file", "-e", f"{ref}:{ANCHORS}", cwd=cwd)
        return True
    except GateError:
        return False


def _anchor_decisions_at(ref: str, cwd: Path) -> list[str]:
    """Return the ``## Decision N`` headings present at ``ref``.

    Returns an empty list if the file is absent at ``ref`` (the gate
    distinguishes that case from the file-present-but-incomplete case
    so the error message is unambiguous).
    """
    try:
        text = _git_text("show", f"{ref}:{ANCHORS}", cwd=cwd)
    except GateError:
        return []
    return [
        n for n in (str(i) for i in range(1, 7))
        if f"## Decision {n}" in text
    ]


def contract_present(cwd: Path) -> tuple[bool, list[str]]:
    """Return (ok, missing-decision-list).  The anchor file must exist
    in the working tree and must contain every ``## Decision N``
    heading for N in 1..6."""
    anchor = cwd / ANCHORS
    if not anchor.is_file():
        return False, [str(n) for n in range(1, 7)]
    text = anchor.read_text(encoding="utf-8")
    missing = [str(n) for n in range(1, 7) if f"## Decision {n}" not in text]
    return not missing, missing


def contract_present_at_base(cwd: Path) -> tuple[bool, list[str]]:
    """Verify the anchor contract at the configured ``BASE_SHA``.

    The ``BASE_SHA`` mode (CI pull_request events) is the only mode
    that exposes a target-branch / PR-head split, so it is the only
    mode that needs to verify the contract at the base ref.  In
    every other mode the working tree is authoritative.
    """
    base_sha = os.environ.get("BASE_SHA", "").strip()
    if not base_sha:
        return True, []

    if not _anchor_path_exists_at(base_sha, cwd):
        return False, [str(n) for n in range(1, 7)]

    headings = _anchor_decisions_at(base_sha, cwd)
    missing = [
        str(n) for n in (str(i) for i in range(1, 7))
        if n not in headings
    ]
    return not missing, missing


def main() -> int:
    try:
        paths = diff_against_base(ROOT)
    except GateError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    if not phase_one_touched(paths):
        print("collapse anchor gate: OK (no Phase 1+ paths changed)")
        return 0

    try:
        base_ok, base_missing = contract_present_at_base(ROOT)
    except GateError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    if not base_ok:
        if base_missing and len(base_missing) == 6:
            print(
                "error: Phase 1+ paths changed against a target branch "
                f"({os.environ.get('BASE_SHA', '')}) that does not yet "
                "contain the merged anchor document "
                f"({ANCHORS}). The Phase 0 anchor contract requires "
                "collapse_anchors.md to be merged on the target branch "
                "before Phase 1+ implementation begins.",
                file=sys.stderr,
            )
        else:
            print(
                "error: collapse anchor decisions missing on the target "
                "branch: " + ", ".join(base_missing),
                file=sys.stderr,
            )
        return 1

    ok, missing = contract_present(ROOT)
    if not ok:
        if missing and len(missing) == 6:
            print(
                f"error: {ANCHORS} is required when Phase 1+ paths change",
                file=sys.stderr,
            )
        else:
            print(
                "error: collapse anchor decisions missing: "
                + ", ".join(missing),
                file=sys.stderr,
            )
        return 1

    print(
        "collapse anchor gate: OK "
        f"({ANCHORS} present with all six decisions on the target branch "
        "and in the working tree)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
