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
2. Local repository with a usable ``HEAD~1`` -> combined diff of
   ``HEAD~1..HEAD`` (committed) and ``HEAD`` vs working tree
   (staged + unstaged + untracked-eligible).  Working-tree Phase 1+
   edits are permitted when the anchors are present in the working tree
   or in the local base (``HEAD~1`` or ``HEAD`` if no parent), so the
   pre-PR development workflow can iterate on implementation once the
   anchors are in hand.  Committed-only Phase 1+ changes still require
   the anchors to be present in the local base.
3. Local repository without ``HEAD~1`` (initial commit or shallow
   clone) -> union of ``HEAD``'s tracked files and the working tree.
   Phase 1+ work is permitted only when the anchors are present at
   ``HEAD`` or in the working tree.
4. Otherwise the gate fails closed -- silently passing would defeat
   the enforcement contract.

CI mode (the ``GITHUB_ACTIONS`` env var is set) requires a non-empty
``BASE_SHA``; an unset ``BASE_SHA`` in CI mode is a configuration
failure and the gate fails closed.  The generic ``CI=true`` env var is
intentionally NOT treated as CI mode (F3 follow-up) because many
local development tools and IDEs set it and would otherwise turn
ordinary local dev runs into confusing "BASE_SHA unset" failures.

Phase 1+ surfaces are listed in ``PHASE_ONE_PREFIXES``. The set is
intentionally limited to the concrete files and narrowly scoped module paths
named by the six decisions. Broad catch-alls such as ``src/server/`` and
``docs/guardrails/`` are excluded so unrelated work and this Phase 0 packet
can merge before the anchors. The one Phase 5.0 follow-up document named by
Decision 5 is listed explicitly.
When at least one of those paths is present in the diff being
validated AND the anchor contract is not satisfied, the gate fails;
otherwise it passes.
"""
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ANCHORS = "docs/guardrails/collapse_anchors.md"

# Explicit implementation surfaces named by the six decisions. Keep this
# list narrow: unrelated files in the same source trees are not Phase 1 work.
PHASE_ONE_PREFIXES = (
    "src/server/aimee_ir_stream.c", "src/server/anthropic_http.c",
    "src/headers/aimee_ir.h", "src/server/openai_chat.c",
    "src/server/openai_shape.c", "src/server/server_http.c",
    "src/server/server_http_routes.c", "src/posix/server_compute.c",
    "src/db1/webchat_live.c", "src/posix/agent_ir_parse.c",
    "src/server/agent_runtime.c", "src/server/agent_request_build.c",
    "src/modules/roundtable/delegate_ensemble.c",
    "src/modules/workflows/wfe_live_panel.c",
    "src/db1/roundtable_pipeline.c", "src/server/model_sampling.c",
    "src/server/aimee_backend_openai.c",
    "src/server/aimee_backend_anthropic.c",
    "src/server/aimee_backend_bedrock.c", "src/db2/bandit.c",
    "src/server/server_state.c", "src/modules/audit/audit_worm.c",
    "src/modules/guardrails/", "src/modules/config/config.h",
    "src/modules/config/config.c",
    "docs/guardrails/collapse_promotion_bucketing.md",
)

_PHASE_ONE_EXEMPT = frozenset({
    "docs/guardrails/collapse_anchors.md",
    "docs/guardrails/collapse_recon.md",
    "docs/guardrails/sampling_capability_matrix.md",
    "docs/guardrails/collapse_anchor_review_closure.md",
})

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
    return the nonempty paths it reports.  Git failures are
    propagated as :class:`GateError` so the gate fails closed
    instead of silently treating a failed diff as empty (F002)."""
    out = _git_text("diff", "--name-only", *args, cwd=cwd)
    return [line for line in out.splitlines() if line]


def _working_tree_names(cwd: Path) -> list[str]:
    """Return the names of files modified, staged, or untracked in the
    working tree, so a Phase 1+ edit that the developer has not yet
    committed is still visible to the gate (F002).

    Combines ``git diff --name-only HEAD`` (tracked edits + staged
    additions) with ``git ls-files --others --modified --exclude-standard``
    (untracked files outside standard ignore lists).  Git failures are
    propagated as :class:`GateError` so the gate fails closed rather
    than silently treating a failed query as an empty diff (F002).
    """
    diff_out = _git_text("diff", "--name-only", "HEAD", cwd=cwd)
    ls_out = _git_text(
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
    contract forbids.  The initial commit IS the base in this scenario,
    so the gate must inspect HEAD's tree as well.  Git failures are
    propagated as :class:`GateError` so the gate fails closed rather
    than silently treating a failed tree query as an empty diff (F002).
    """
    out = _git_text("ls-tree", "-r", "--name-only", "HEAD", cwd=cwd)
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
       change staged but not committed must still be visible to the gate.
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
        #   edited but not yet committed.  These are permitted when the
        #   anchors are present in the working tree or in the local
        #   base (HEAD~1), so the pre-PR workflow can iterate locally
        #   on top of the merged anchors.
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


def _matches_prefix(path: str, prefix: str) -> bool:
    """True iff ``path`` lies under ``prefix``.

    For prefix strings that end with ``/`` (directory prefixes) the
    test is a strict path-component check: ``path == prefix`` or
    ``path.startswith(prefix)`` is sufficient.  For prefix strings
    that name a single file (no trailing ``/``) the test is exact
    equality, so a PR that renames or co-locates a Phase 1+ file
    does not accidentally fall outside the gate's scope.
    """
    if prefix.endswith("/"):
        return path == prefix or path.startswith(prefix)
    return path == prefix


def phase_one_touched(paths: list[str]) -> bool:
    """True iff at least one path in ``paths`` is a Phase 1+ surface.

    The ``_PHASE_ONE_EXEMPT`` carve-out lets a PR whose purpose is to
    introduce the anchor document itself land without the anchors
    already being merged (otherwise the contract would be impossible
    to bootstrap).
    """
    return _phase_one_match(paths) is not None


def _phase_one_match(paths: list[str]) -> str | None:
    """Return the first Phase 1+ prefix matched by ``paths``, or None.

    Exempt paths are skipped.  The matched prefix is surfaced in error
    messages so a failure points to the decision/surface family that
    tripped the gate.
    """
    for path in paths:
        if path in _PHASE_ONE_EXEMPT:
            continue
        for prefix in PHASE_ONE_PREFIXES:
            if _matches_prefix(path, prefix):
                return prefix
    return None


def phase_one_prefixes() -> tuple[str, ...]:
    """Public view of ``PHASE_ONE_PREFIXES`` for tests and downstream
    tooling.  Returning a copy prevents callers from mutating the
    module-level tuple."""
    return tuple(PHASE_ONE_PREFIXES)


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


def _contract_present_at_ref(ref: str, cwd: Path) -> tuple[bool, list[str]]:
    """Verify the anchor contract at an arbitrary git revision.

    Returns ``(True, [])`` if ``collapse_anchors.md`` exists at ``ref``
    and contains every ``## Decision N`` heading for N in 1..6.
    Otherwise returns ``(False, missing-decisions)``.
    """
    if not _anchor_path_exists_at(ref, cwd):
        return False, [str(n) for n in range(1, 7)]
    headings = _anchor_decisions_at(ref, cwd)
    missing = [str(n) for n in (str(i) for i in range(1, 7)) if n not in headings]
    return not missing, missing


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
    """Verify the anchor contract at the base revision.

    In CI mode the base revision is ``BASE_SHA`` (the target branch
    head).  In local mode with a usable ``HEAD~1`` the base is
    ``HEAD~1``.  In local mode without ``HEAD~1`` (initial commit or
    shallow clone) the base is ``HEAD`` (F001 closure).  In all three
    cases the base-reference contract is mandatory when a Phase 1+
    path is present; the function only returns ``(True, [])`` when the
    anchor file at the resolved base has every decision.

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
        # clone local workflow.
        #
        # F001 closure: when ``HEAD~1`` is unavailable (initial commit
        # or shallow clone), the only available base reference is
        # ``HEAD``.  The gate is only asked to verify the base contract
        # when a Phase 1+ path is present, so deferring to the working
        # tree alone would let an initial commit that introduces a
        # Phase 1+ path without anchors pass silently.  Verify the
        # anchors at ``HEAD`` instead; if they are missing, fail closed.
        local_base = _local_base_ref(cwd)
        if local_base is None:
            base_sha = "HEAD"
        else:
            base_sha = local_base

    return _contract_present_at_ref(base_sha, cwd)


def main() -> int:
    try:
        paths = diff_against_base(ROOT)
    except GateError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    match = _phase_one_match(paths)
    if match is None:
        print("collapse anchor gate: OK (no Phase 1+ paths changed)")
        return 0

    # F002: distinguish the bootstrap prerequisite from ordinary local
    # dirty-tree validation.  In CI mode the target branch must carry
    # the anchors.  In local mode, working-tree Phase 1+ changes are
    # permitted when the anchors are already present in the working
    # tree or in the local base, so a developer can iterate on Phase 1+
    # implementation before committing.  Committed Phase 1+ changes still
    # require the anchors to be present in the local base.
    working_tree_paths = _working_tree_names(ROOT)
    working_tree_match = _phase_one_match(working_tree_paths)

    base_sha = os.environ.get("BASE_SHA", "").strip()
    if base_sha or _ci_mode():
        # Target-branch semantics: the supplied BASE_SHA is the head of
        # the branch being targeted (e.g. a PR base).  The anchors must
        # already be merged there before Phase 1+ implementation begins.
        # This branch is used whenever BASE_SHA is explicitly set, even in
        # local test harnesses, so the target-branch contract can be
        # exercised without GITHUB_ACTIONS.
        if not base_sha:
            print(
                "error: BASE_SHA is unset in CI mode; refusing to "
                "validate. The Phase 0 anchor contract requires the "
                "target branch head to be supplied so the gate can "
                "verify that collapse_anchors.md is already merged "
                "on the target branch before Phase 1+ implementation "
                "begins.",
                file=sys.stderr,
            )
            return 2
        try:
            base_ok, base_missing = _contract_present_at_ref(base_sha, ROOT)
        except GateError as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2
        if not base_ok:
            if base_missing and len(base_missing) == 6:
                print(
                    f"error: Phase 1+ paths changed against a target branch "
                    f"({base_sha}) that does not yet contain the merged "
                    f"anchor document ({ANCHORS}). The Phase 0 anchor "
                    "contract requires collapse_anchors.md to be merged "
                    "on the target branch before Phase 1+ implementation "
                    "begins; "
                    f"{ANCHORS} is required when Phase 1+ paths change "
                    f"(matched prefix: {match}).",
                    file=sys.stderr,
                )
            else:
                print(
                    "error: collapse anchor decisions missing on the target "
                    "branch: " + ", ".join(base_missing),
                    file=sys.stderr,
                )
            return 1
        print(
            "collapse anchor gate: OK "
            f"({ANCHORS} present with all six decisions on the target branch "
            f"{base_sha})"
        )
        return 0

    # Local mode.
    if working_tree_match is not None:
        # If the anchors are present in the working tree, the developer
        # is iterating on Phase 1+ implementation on top of the anchors.
        ok, _ = contract_present(ROOT)
        if ok:
            print(
                "collapse anchor gate: OK (local Phase 1+ work on top of "
                f"present {ANCHORS})"
            )
            return 0
        # Anchors are not in the working tree; check the local base so
        # a developer can keep the anchors in an earlier commit and
        # iterate on Phase 1+ files in the working tree.
        local_base = _local_base_ref(ROOT)
        base_ref = local_base if local_base is not None else "HEAD"
        try:
            base_ok, _ = _contract_present_at_ref(base_ref, ROOT)
        except GateError as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2
        if base_ok:
            print(
                "collapse anchor gate: OK (local Phase 1+ work on top of "
                f"{ANCHORS} at base {base_ref})"
            )
            return 0
        # Bootstrap: Phase 1+ paths in the working tree before any
        # anchor document exists.
        print(
            f"error: Phase 1+ paths are present in the working tree "
            f"(staged, unstaged, or untracked) before the anchor document "
            f"is present; matched prefix: {working_tree_match}. "
            f"Create {ANCHORS} with all six decisions first, then re-stage "
            "your Phase 1+ change on top of it.",
            file=sys.stderr,
        )
        return 1

    # Only committed Phase 1+ changes.  Verify the anchors are present
    # in the local base (HEAD~1 or HEAD if no parent).
    local_base = _local_base_ref(ROOT)
    base_ref = local_base if local_base is not None else "HEAD"
    try:
        base_ok, base_missing = _contract_present_at_ref(base_ref, ROOT)
    except GateError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    if not base_ok:
        if base_missing and len(base_missing) == 6:
            print(
                f"error: Phase 1+ paths changed against a local base "
                f"({base_ref}) that does not yet contain the merged anchor "
                f"document ({ANCHORS}). The Phase 0 anchor contract requires "
                "collapse_anchors.md to be merged on the target branch before "
                "Phase 1+ implementation begins; "
                f"{ANCHORS} is required when Phase 1+ paths change "
                f"(matched prefix: {match}).",
                file=sys.stderr,
            )
        else:
            print(
                "error: collapse anchor decisions missing on the local base "
                "branch: " + ", ".join(base_missing),
                file=sys.stderr,
            )
        return 1

    # The base has anchors; also ensure the working tree still carries
    # a complete copy (defensive: if the developer deleted the anchor
    # file in the working tree while editing Phase 1+ files, the local
    # dev workflow should still satisfy the literal contract).
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
        f"({ANCHORS} present with all six decisions on the local base "
        "and in the working tree)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
