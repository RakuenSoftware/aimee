#!/usr/bin/env python3
"""Enforce the Phase 0 anchor contract for changes to Phase 1+ surfaces.

The contract is the presence of ``docs/guardrails/collapse_anchors.md``
containing all six ``## Decision N`` headings (N = 1..6).  Once that
document is merged, every subsequent change that touches a Phase 1+
surface must continue to find the document in the working tree, but it
must NOT be required to re-modify the document in the same commit --
doing so would defeat the request's "Phase 1 implementation does not
start until the anchors are merged" condition by enforcing repeated
modification of the merged contract file.

Phase 1+ surfaces are listed in ``PHASE_ONE_PREFIXES``.  When at least
one of those paths is present in the diff being validated, the gate
checks the contract; otherwise it passes.
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


def diff_against_base(cwd: Path) -> list[str]:
    """Return the list of files changed in the diff being validated.

    Resolution order:

    1. ``BASE_SHA`` env var (set by CI for ``pull_request`` events) ->
       diff from ``BASE_SHA`` to ``HEAD``.
    2. Local repository with a usable ``HEAD~1`` -> diff from
       ``HEAD~1`` to ``HEAD`` (last-commit form).
    3. Local repository without ``HEAD~1`` (initial commit or shallow
       clone) -> diff from the index/working tree against ``HEAD``,
       so staged + unstaged edits are still validated.
    4. Otherwise the gate fails closed -- silently passing would
       defeat the enforcement contract.
    """
    base_sha = os.environ.get("BASE_SHA", "").strip()
    if base_sha:
        try:
            out = subprocess.check_output(
                ["git", "diff", "--name-only", base_sha, "HEAD"],
                cwd=cwd, text=True,
            )
        except subprocess.CalledProcessError as exc:
            raise GateError(
                f"git diff {base_sha} HEAD failed: {exc.stderr or exc}"
            ) from exc
        except OSError as exc:
            raise GateError(f"could not invoke git: {exc}") from exc
        return [line for line in out.splitlines() if line]

    try:
        has_parent = subprocess.run(
            ["git", "rev-parse", "--verify", "--quiet", "HEAD~1"],
            cwd=cwd, text=True,
        ).returncode == 0
    except OSError as exc:
        raise GateError(f"could not invoke git: {exc}") from exc
    if has_parent:
        try:
            out = subprocess.check_output(
                ["git", "diff", "--name-only", "HEAD~1", "HEAD"],
                cwd=cwd, text=True,
            )
        except subprocess.CalledProcessError as exc:
            raise GateError(
                f"git diff HEAD~1 HEAD failed: {exc.stderr or exc}"
            ) from exc
        except OSError as exc:
            raise GateError(f"could not invoke git: {exc}") from exc
        return [line for line in out.splitlines() if line]

    try:
        out = subprocess.check_output(
            ["git", "diff", "--name-only", "HEAD"],
            cwd=cwd, text=True,
        )
    except subprocess.CalledProcessError as exc:
        raise GateError(
            f"git diff HEAD failed (no usable comparison base): "
            f"{exc.stderr or exc}"
        ) from exc
    except OSError as exc:
        raise GateError(f"could not invoke git: {exc}") from exc
    return [line for line in out.splitlines() if line]


def phase_one_touched(paths: list[str]) -> bool:
    return any(path.startswith(PHASE_ONE_PREFIXES) for path in paths)


def contract_present(cwd: Path) -> tuple[bool, list[str]]:
    """Return (ok, missing-decision-list).  The anchor file must exist
    and must contain every ``## Decision N`` heading for N in 1..6."""
    anchor = cwd / ANCHORS
    if not anchor.is_file():
        return False, [str(n) for n in range(1, 7)]
    text = anchor.read_text(encoding="utf-8")
    missing = [str(n) for n in range(1, 7) if f"## Decision {n}" not in text]
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
        f"({ANCHORS} present with all six decisions on disk)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
