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
2. Local repository with a usable ``HEAD~1`` -> diff is the union of
   ``HEAD~1..HEAD`` (committed) and ``HEAD`` vs working tree
   (staged + unstaged + untracked-eligible), so pending edits are
   covered even before the developer commits.  This is the pre-PR
   dev workflow.
3. Local repository without ``HEAD~1`` (initial commit or shallow
   clone) -> diff is the union of ``HEAD``'s tracked files and the
   working tree, so a Phase 1+ path committed in the initial commit
   (invisible to a HEAD~1..HEAD diff) still trips the gate.  This is
   the F3 follow-up: a clean initial commit that introduces Phase 1+
   paths must not pass silently.
4. Otherwise the gate fails closed -- silently passing would defeat the
   enforcement contract.

CI mode (the ``GITHUB_ACTIONS`` env var is set) requires a non-empty
``BASE_SHA``; an unset ``BASE_SHA`` in CI mode is a configuration
failure and the gate fails closed.  This is the F001 fix: the
base-reference contract is mandatory whenever the runner signals it
is operating in CI mode.  The generic ``CI=true`` env var is
intentionally NOT treated as CI mode (F3 follow-up) because many
local development tools and IDEs set it and would otherwise turn
ordinary local dev runs into confusing "BASE_SHA unset" failures.

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


def _git_text_ok(*args: str, cwd: Path) -> str:
    """Like ``_git_text`` but returns the empty string on non-zero
    exit rather than raising.  Used for best-effort diff queries that
    combine multiple sources."""
    try:
        return subprocess.check_output(
            ["git", *args], cwd=cwd, text=True,
        )
    except subprocess.CalledProcessError:
        return ""
    except OSError as exc:
        raise GateError(f"could not invoke git: {exc}") from exc


def _local_base_ref(cwd: Path) -> str | None:
    """Return the local-mode base ref (``HEAD~1``) when it exists.

    In CI mode the runner supplies ``BASE_SHA`` and we must use that
    exact revision; in local mode (no ``BASE_SHA``, not CI) the natural
    base for the ``HEAD~1..HEAD`` diff is ``HEAD~1`` -- the merge-base
    of the current commit and its parent.  Returning ``None`` when
    ``HEAD~1`` is unavailable (initial commit / shallow clone) lets the
    caller fall back to the working-tree diff only, which is the
    behaviour described in the docstring for that case.
    """
    try:
        sha = subprocess.check_output(
            ["git", "rev-parse", "--verify", "HEAD~1"],
            cwd=cwd, text=True,
        ).strip()
    except subprocess.CalledProcessError:
        return None
    except OSError as exc:
        raise GateError(f"could not invoke git: {exc}") from exc
    return sha or None


def _ci_mode() -> bool:
    """True iff the surrounding environment looks like a GitHub Actions
    runner.

    The gate must validate the base-reference contract whenever the
    runner signals CI mode, because that is the only mode in which a
    target-branch / PR-head split exists.  An unset ``BASE_SHA`` under
    CI mode is treated as a configuration failure (F001).

    Only ``GITHUB_ACTIONS`` is treated as authoritative: the generic
    ``CI=true`` env var is set by many local development tools and IDEs
    and would otherwise turn a local dev run into a "fail closed with
    no BASE_SHA" surprise (F3 follow-up).  Anyone running the gate
    against a non-GitHub CI should set ``GITHUB_ACTIONS`` explicitly
    rather than rely on the loose ``CI`` env var.
    """
    return bool(os.environ.get("GITHUB_ACTIONS", "").strip())


def _diff_names(*args: str, cwd: Path) -> list[str]:
    """Run ``git diff --name-only`` with the given arguments and
    return the nonempty paths it reports.  Returns an empty list on
    a non-zero exit (used for the combined local-mode diff)."""
    out = _git_text_ok("diff", "--name-only", *args, cwd=cwd)
    return [line for line in out.splitlines() if line]


def _working_tree_names(cwd: Path) -> list[str]:
    """Return the names of files modified, staged, or untracked in the
    working tree, so a Phase 1+ edit that the developer has not yet
    committed is still visible to the gate (F002).

    Combines ``git diff --name-only HEAD`` (tracked edits + staged
    additions) with ``git ls-files --others --modified --exclude-standard``
    (untracked files outside standard ignore lists).  ``git add -N``
    would also fold untracked files into the diff output, but the
    ``ls-files`` source is sufficient on its own and avoids mutating
    the index.
    """
    diff_out = _git_text_ok("diff", "--name-only", "HEAD", cwd=cwd)
    ls_out = _git_text_ok(
        "ls-files", "--others", "--modified", "--exclude-standard", cwd=cwd,
    )
    names: list[str] = []
    seen: set[str] = set()
    for line in (*diff_out.splitlines(), *ls_out.splitlines()):
        if not line or line in seen:
            continue
        seen.add(line)
        names.append(line)
    return names


def _head_tree_names(cwd: Path) -> list[str]:
    """Return the names of every file tracked at ``HEAD``.

    Used as the initial-commit fallback: when ``HEAD~1`` is unavailable
    there is no prior commit to diff against, but a Phase 1+ path
    present in the initial commit itself is exactly the scenario the
    contract forbids.  ``git ls-tree -r HEAD`` is best-effort: on a
    corrupt tree it returns the empty string, and the caller treats the
    result as an empty Phase 1+ list (which is no worse than the
    pre-existing behaviour).
    """
    out = _git_text_ok("ls-tree", "-r", "--name-only", "HEAD", cwd=cwd)
    return [line for line in out.splitlines() if line]


def diff_against_base(cwd: Path) -> list[str]:
    """Return the list of files changed in the diff being validated.

    Resolution order:

    1. ``BASE_SHA`` env var (set by CI for ``pull_request`` events) ->
       diff from ``BASE_SHA`` to ``HEAD`` (the explicit PR-base/PR-head
       semantics required by F001).  Failure to resolve ``BASE_SHA``
       raises :class:`GateError` so the gate fails closed.
    2. Local repository with a usable ``HEAD~1`` -> combined diff of
       ``HEAD~1..HEAD`` (committed) and ``HEAD`` vs the working tree
       (staged + unstaged).  The union is required by F002: a Phase 1+
       change staged but not committed must still trip the gate.
    3. Local repository without ``HEAD~1`` (initial commit or shallow
       clone) -> union of ``HEAD``'s tracked files and the working
       tree, so a Phase 1+ path committed in the initial commit
       (which is invisible to a HEAD~1..HEAD diff) still trips the
       gate.  This is the F3 follow-up: a clean initial commit that
       introduces Phase 1+ paths must not pass silently.
    4. Otherwise the gate fails closed -- silently passing would
       defeat the enforcement contract.
    """
    base_sha = os.environ.get("BASE_SHA", "").strip()
    if base_sha:
        # Use _git_text (not _git_text_ok) so an invalid BASE_SHA
        # fails closed with a GateError instead of silently producing
        # an empty diff.
        out = _git_text("diff", "--name-only", base_sha, "HEAD", cwd=cwd)
        return [line for line in out.splitlines() if line]

    # F001: in CI mode BASE_SHA is mandatory.  Without it the gate
    # cannot know what target branch the PR is being proposed against,
    # so silently passing would defeat the merge-first contract.
    if _ci_mode():
        raise GateError(
            "BASE_SHA is unset in CI mode; refusing to validate. "
            "Set BASE_SHA to the target branch head (e.g. "
            "github.event.pull_request.base.sha) so the gate can "
            "verify that the anchor document is already merged on "
            "the target branch."
        )

    try:
        # capture_output=True so the SHA / error from ``git rev-parse``
        # does not leak into the gate's stdout (the gate's stdout is
        # asserted on by tests).
        has_parent = subprocess.run(
            ["git", "rev-parse", "--verify", "--quiet", "HEAD~1"],
            cwd=cwd, text=True, capture_output=True,
        ).returncode == 0
    except OSError as exc:
        raise GateError(f"could not invoke git: {exc}") from exc
    if has_parent:
        # The local-mode diff is the union of two sources so the gate
        # catches both flavours of Phase 1+ work before it lands on
        # the target branch:
        #
        # * ``HEAD~1..HEAD`` -- Phase 1+ changes already committed
        #   but not yet on the target branch.  When these exist the
        #   gate must verify the anchor document is already merged at
        #   ``HEAD~1`` (the local approximation of the target branch
        #   head); otherwise the Phase 1+ commit sneaks ahead of the
        #   Phase 0 anchors.
        # * working tree vs ``HEAD`` -- Phase 1+ changes staged or
        #   edited but not yet committed.  The F002 closure requires
        #   these to trip the gate even when ``HEAD`` itself already
        #   carries the anchors, because in-flight Phase 1+ work
        #   before the anchors are merged on the target branch is
        #   exactly the scenario the contract forbids.
        committed = _diff_names("HEAD~1", "HEAD", cwd=cwd)
        working_tree = _working_tree_names(cwd)
        seen: set[str] = set()
        combined: list[str] = []
        for path in [*committed, *working_tree]:
            if path not in seen:
                seen.add(path)
                combined.append(path)
        return combined

    # F3 follow-up: when HEAD~1 is unavailable, the working tree diff
    # alone misses Phase 1+ paths that were committed in the initial
    # commit itself.  The initial commit IS the base in this scenario,
    # so the gate must inspect HEAD's tree as well.
    head_tree = _head_tree_names(cwd)
    working_tree = _working_tree_names(cwd)
    seen: set[str] = set()
    combined: list[str] = []
    for path in [*head_tree, *working_tree]:
        if path not in seen:
            seen.add(path)
            combined.append(path)
    return combined


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
    heading for N in 1..6.

    NOTE -- working-tree vs CI asymmetry (F4 review finding): this
    working-tree check is intentionally local-only.  CI mode skips
    it because the CI checkout represents the PR head, and the
    merge-first contract is enforced separately against ``BASE_SHA``
    by :func:`contract_present_at_base`.  Re-verifying the working
    tree in CI would be redundant: the PR head always carries the
    pending Phase 1+ diff that we are trying to gate, so the only
    authoritative question is whether ``BASE_SHA`` (the target
    branch) already merged the anchor document.
    """
    anchor = cwd / ANCHORS
    if not anchor.is_file():
        return False, [str(n) for n in range(1, 7)]
    text = anchor.read_text(encoding="utf-8")
    missing = [str(n) for n in range(1, 7) if f"## Decision {n}" not in text]
    return not missing, missing


def contract_present_at_base(cwd: Path) -> tuple[bool, list[str]]:
    """Verify the anchor contract at the configured ``BASE_SHA``.

    Returns ``(True, [])`` early when no ``BASE_SHA`` is configured
    AND we are not in CI mode.  When ``BASE_SHA`` is set OR the
    surrounding environment is a CI runner, the base-reference
    contract is mandatory (F001) and the function only returns
    ``(True, [])`` when the file at ``BASE_SHA`` has every decision.

    The caller in :func:`main` only invokes this function when
    ``phase_one_touched(paths)`` is true, so the function is never
    asked to "verify" in the no-Phase-1-changes case.
    """
    base_sha = os.environ.get("BASE_SHA", "").strip()
    if not base_sha:
        # F001: in CI mode the base-reference contract is mandatory;
        # an unset BASE_SHA is a configuration failure rather than a
        # satisfied contract.
        if _ci_mode():
            return False, [str(n) for n in range(1, 7)]
        # F002: in local mode the natural base is ``HEAD~1`` -- the
        # local approximation of the target branch head.  ``HEAD~1``
        # is the last commit before HEAD, so verifying the anchors
        # are merged there is what "anchors are merged on the target
        # branch before Phase 1+ starts" reduces to in a single-
        # clone local workflow.  When ``HEAD~1`` is unavailable
        # (initial commit), the gate defers to the working-tree
        # contract check only.
        local_base = _local_base_ref(cwd)
        if local_base is None:
            return True, []
        base_sha = local_base

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

    # F002: in-flight (working-tree) Phase 1+ changes are forbidden
    # outright, regardless of HEAD's merge state.  Only Phase 1+
    # changes that are already committed at HEAD are eligible for the
    # base-reference contract check.  ``_working_tree_names`` returns
    # the union of staged, unstaged, and pure-untracked paths, so a
    # single Phase 1+ path present there is sufficient to trip the
    # gate.
    working_tree_paths = _working_tree_names(ROOT)
    if any(path.startswith(PHASE_ONE_PREFIXES) for path in working_tree_paths):
        print(
            "error: Phase 1+ paths are present in the working tree "
            "(staged, unstaged, or untracked) without being committed. "
            f"{ANCHORS} is required to be merged on the target branch "
            "before Phase 1+ implementation begins; run with the working tree clean "
            "(e.g. git stash) to validate committed work only; commit the Phase 0 "
            "anchors on the target branch first, then re-stage your "
            "Phase 1+ change on top of that merge.",
            file=sys.stderr,
        )
        return 1

    try:
        base_ok, base_missing = contract_present_at_base(ROOT)
    except GateError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    if not base_ok:
        if base_missing and len(base_missing) == 6:
            base_sha = os.environ.get("BASE_SHA", "").strip()
            if _ci_mode() and not base_sha:
                print(
                    "error: BASE_SHA is unset in CI mode; refusing to "
                    "validate. The Phase 0 anchor contract requires the "
                    "target branch head to be supplied so the gate can "
                    "verify that collapse_anchors.md is already merged "
                    "on the target branch before Phase 1+ implementation "
                    "begins.",
                    file=sys.stderr,
                )
            else:
                print(
                    "error: Phase 1+ paths changed against a target branch "
                    f"({base_sha}) that does not yet contain the merged "
                    f"anchor document ({ANCHORS}). The Phase 0 anchor "
                    "contract requires collapse_anchors.md to be merged "
                    "on the target branch before Phase 1+ implementation "
                    "begins; "
                    f"{ANCHORS} is required when Phase 1+ paths change.",
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
