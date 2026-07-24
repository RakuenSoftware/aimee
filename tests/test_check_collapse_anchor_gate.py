"""Behavioral tests for scripts/check-collapse-anchor-gate.py.

These tests exercise the gate against synthetic Git histories so that the
behavior is verified end-to-end, not merely by grepping the script source.

Each test builds a throwaway worktree under ``/tmp`` that mirrors the
relevant invariants of this repository:

* the script is copied verbatim from ``scripts/check-collapse-anchor-gate.py``;
* the worktree is a real Git repo, so the gate's diff-discovery path is
  exercised against a real ``git diff`` invocation;
* the ``docs/guardrails/collapse_anchors.md`` file is materialised (or
  withheld) on a per-test basis, and may carry zero to six
  ``## Decision N`` headings.

The contract:

* if no Phase 1+ path appears in the validated diff, the gate passes
  silently regardless of the anchor file's contents;
* if a Phase 1+ path appears, the anchor file must exist in the working
  tree and must carry every ``## Decision N`` heading (N = 1..6);
* the anchor file is NOT required to appear in the validated diff
  itself -- once merged, it is the *presence* of the document on disk
  that gates subsequent Phase 1+ work, not its re-modification;
* a failure to compute the diff (missing ``git`` binary, bad
  ``BASE_SHA``, missing ``HEAD~1`` AND an empty working tree) must
  fail closed with a nonzero exit code.
"""
from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPT_SRC = REPO_ROOT / "scripts" / "check-collapse-anchor-gate.py"


def _git(cwd: Path, *args: str) -> None:
    full_env = {
        **os.environ,
        "HOME": "/tmp",
        "GIT_AUTHOR_NAME": "test",
        "GIT_AUTHOR_EMAIL": "test@test",
        "GIT_COMMITTER_NAME": "test",
        "GIT_COMMITTER_EMAIL": "test@test",
    }
    subprocess.run(
        ["git", *args], cwd=cwd, env=full_env, check=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )


@pytest.fixture()
def fake_worktree(tmp_path: Path) -> Path:
    """Build a fresh Git repo with the real gate script installed and
    an initial commit so that HEAD~1 is reachable from subsequent
    commits (mirroring the Phase 0 -> Phase 1 sequence the contract
    is designed for)."""
    repo = tmp_path / "wt"
    repo.mkdir()
    scripts_dir = repo / "scripts"
    scripts_dir.mkdir()
    shutil.copyfile(SCRIPT_SRC, scripts_dir / "check-collapse-anchor-gate.py")
    (scripts_dir / "check-collapse-anchor-gate.py").chmod(0o755)
    _git(repo, "init", "-q", "-b", "main")
    _git(repo, "config", "user.email", "test@test")
    _git(repo, "config", "user.name", "test")
    _git(repo, "config", "--global", "--add", "safe.directory", str(repo))
    (repo / "docs" / "guardrails").mkdir(parents=True)
    # Seed an empty initial commit so HEAD~1 exists for the next commit.
    (repo / ".gitkeep").write_text("")
    _git(repo, "add", "-A")
    _git(repo, "commit", "-q", "--allow-empty", "-m", "seed")
    return repo


def _run_gate(repo: Path, base_sha: str | None = None,
              path_override: str | None = None) -> subprocess.CompletedProcess:
    env = os.environ.copy()
    env["HOME"] = "/tmp"
    if base_sha is not None:
        env["BASE_SHA"] = base_sha
    if path_override is not None:
        env["PATH"] = path_override
    return subprocess.run(
        ["python3", "scripts/check-collapse-anchor-gate.py"],
        cwd=repo, env=env, capture_output=True, text=True,
    )


def _write_anchors(repo: Path, decisions: list[int]) -> None:
    body = "# Anchors\n\n" + "\n".join(
        f"## Decision {n} - placeholder\n" for n in decisions
    ) + "\n"
    (repo / "docs" / "guardrails" / "collapse_anchors.md").write_text(body)


def _commit(repo: Path, message: str, files: dict[str, str]) -> None:
    for rel, content in files.items():
        target = repo / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content)
    _git(repo, "add", "-A")
    _git(repo, "commit", "-q", "-m", message)


def test_gate_passes_when_no_phase_one_paths_change(fake_worktree: Path) -> None:
    """A commit that touches only non-Phase-1+ paths must pass
    regardless of whether the anchor file is present."""
    # Anchor file missing entirely -- only acceptable if no Phase 1+ path
    # changed in the validated diff.
    _commit(fake_worktree, "docs only", {"README.md": "hello\n"})

    result = _run_gate(fake_worktree)
    assert result.returncode == 0, result.stderr
    assert "no Phase 1+ paths changed" in result.stdout


def test_gate_passes_when_anchors_already_merged_and_phase_one_touches_other_files(
    fake_worktree: Path,
) -> None:
    """Regression for F2: the gate must NOT require the anchor file to
    appear in the same diff as a Phase 1+ change.  Once the anchor file
    is merged, ordinary Phase 1+ commits that don't touch it must pass."""
    _write_anchors(fake_worktree, list(range(1, 7)))
    _commit(fake_worktree, "Phase 0: anchors", {})

    # A second commit touches src/server/ but NOT the anchors file.
    _commit(
        fake_worktree, "Phase 1: server change",
        {"src/server/foo.c": "void foo(void) {}\n"},
    )

    result = _run_gate(fake_worktree)
    assert result.returncode == 0, result.stderr
    assert "present with all six decisions" in result.stdout


def test_gate_rejects_phase_one_change_when_anchors_file_missing(
    fake_worktree: Path,
) -> None:
    _commit(
        fake_worktree, "Phase 1: server change only",
        {"src/server/foo.c": "void foo(void) {}\n"},
    )
    result = _run_gate(fake_worktree)
    assert result.returncode == 1
    assert "required" in result.stderr


def test_gate_rejects_phase_one_change_when_decisions_incomplete(
    fake_worktree: Path,
) -> None:
    _write_anchors(fake_worktree, [1, 2, 3, 4, 5])  # missing Decision 6
    _commit(fake_worktree, "Phase 1", {"src/server/foo.c": "x\n"})
    result = _run_gate(fake_worktree)
    assert result.returncode == 1
    assert "6" in result.stderr


def test_gate_fails_closed_when_diff_discovery_cannot_run(
    fake_worktree: Path,
) -> None:
    """Regression for F1: if ``git diff`` itself cannot run, the gate must
    fail with a nonzero exit code rather than silently reporting success.

    Uses a wrapper that monkeypatches ``subprocess.run`` / ``subprocess.check_output``
    in the gate process so any attempt to invoke ``git`` raises
    ``FileNotFoundError``. This is more reliable than a PATH shim because
    subprocess PATH resolution is process-local and can be defeated by
    absolute paths or PATH ordering quirks; a monkeypatch on the gate's
    own subprocess calls removes the ambiguity.
    """
    _write_anchors(fake_worktree, list(range(1, 7)))
    _commit(fake_worktree, "Phase 0", {})
    _commit(fake_worktree, "Phase 1", {"src/server/foo.c": "x\n"})

    wrapper_body = (
        "import runpy, subprocess, sys\n"
        "_orig_run = subprocess.run\n"
        "_orig_co = subprocess.check_output\n"
        "def _reject_git(*args, **kwargs):\n"
        "    argv = kwargs.get('args') or (args[0] if args else None)\n"
        "    if isinstance(argv, (list, tuple)) and argv and argv[0] == 'git':\n"
        "        raise FileNotFoundError(2, 'No such file or directory', 'git')\n"
        "    return _orig_run(*args, **kwargs)\n"
        "def _reject_git_co(*args, **kwargs):\n"
        "    argv = kwargs.get('args') or (args[0] if args else None)\n"
        "    if isinstance(argv, (list, tuple)) and argv and argv[0] == 'git':\n"
        "        raise FileNotFoundError(2, 'No such file or directory', 'git')\n"
        "    return _orig_co(*args, **kwargs)\n"
        "subprocess.run = _reject_git\n"
        "subprocess.check_output = _reject_git_co\n"
        "sys.argv = ['check-collapse-anchor-gate.py']\n"
        "runpy.run_path('scripts/check-collapse-anchor-gate.py', run_name='__main__')\n"
    )
    wrapper = fake_worktree / "_no_git_wrapper.py"
    wrapper.write_text(wrapper_body)
    env = os.environ.copy()
    env["HOME"] = "/tmp"
    env.pop("BASE_SHA", None)
    result = subprocess.run(
        ["python3", str(wrapper)],
        cwd=fake_worktree, env=env, capture_output=True, text=True,
    )
    assert result.returncode != 0, result.stdout
    assert "git" in result.stderr.lower()


def test_gate_fails_closed_when_base_sha_is_invalid(
    fake_worktree: Path,
) -> None:
    """When BASE_SHA is set (CI mode) but doesn't resolve to a real
    revision, the gate must fail closed."""
    _write_anchors(fake_worktree, list(range(1, 7)))
    _commit(fake_worktree, "Phase 0", {})
    _commit(fake_worktree, "Phase 1", {"src/server/foo.c": "x\n"})

    result = _run_gate(fake_worktree, base_sha="definitely_not_a_real_sha")
    assert result.returncode != 0, result.stdout


def test_gate_uses_base_sha_when_provided(fake_worktree: Path) -> None:
    """When CI exports BASE_SHA, the gate must diff against that
    revision instead of HEAD~1, so the gate works correctly under
    shallow clones (where HEAD~1 is unavailable)."""
    _write_anchors(fake_worktree, list(range(1, 7)))
    _commit(fake_worktree, "Phase 0", {})
    initial_head = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=fake_worktree, env={**os.environ, "HOME": "/tmp"},
        check=True, capture_output=True, text=True,
    ).stdout.strip()
    _commit(fake_worktree, "Phase 1 unrelated", {"README.md": "x\n"})

    # Validate against the Phase 0 merge-base: the resulting diff must
    # NOT include Phase 1+ paths, so the gate passes.
    result = _run_gate(fake_worktree, base_sha=initial_head)
    assert result.returncode == 0, result.stderr


def test_gate_script_has_six_decision_check() -> None:
    """Smoke test preserved from the prior artifact."""
    text = SCRIPT_SRC.read_text()
    assert "range(1, 7)" in text
    assert "diff" in text


def test_gate_script_is_executable_python() -> None:
    """Smoke test preserved from the prior artifact: running the script
    against the real worktree must succeed because the worktree carries
    the merged anchor document and the prior commit does not touch a
    Phase 1+ path."""
    result = subprocess.run(
        ["python3", str(SCRIPT_SRC)],
        cwd=REPO_ROOT, capture_output=True, text=True,
        env={**os.environ, "HOME": "/tmp"},
    )
    assert result.returncode == 0, result.stderr or result.stdout


def test_gate_rejects_phase_one_change_when_base_branch_lacks_anchors(
    fake_worktree: Path,
) -> None:
    """F001 regression: integration test that validates a PR-style diff
    containing Phase 1+ changes against a target branch whose head does
    NOT carry the merged anchor document.

    The realistic scenario is: a PR is opened against ``base`` (the
    target branch) which has never merged the Phase 0 anchors.  Even if
    the PR head happens to carry the anchors file (because the same PR
    adds it), the gate must fail because the contract requires the
    anchors to be **already merged** on the target branch before
    Phase 1+ work begins.

    The test mirrors the CI event model by setting ``BASE_SHA`` to the
    target-branch head and validating the PR head diff, exactly as the
    gate's ``diff_against_base`` resolution does in CI mode.
    """
    # Target branch head: no anchor file, no Phase 1+ surfaces.
    _commit(fake_worktree, "base: readme", {"README.md": "base\n"})
    base_head = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=fake_worktree, env={**os.environ, "HOME": "/tmp"},
        check=True, capture_output=True, text=True,
    ).stdout.strip()

    # PR head branch: adds a Phase 1+ surface (src/server/foo.c) AND
    # the anchors file (because in reality the same PR can carry both).
    _commit(
        fake_worktree, "PR: adds server file and anchors",
        {
            "src/server/foo.c": "void foo(void) {}\n",
            "docs/guardrails/collapse_anchors.md":
                "# Anchors\n\n" + "\n".join(
                    f"## Decision {n} - placeholder\n" for n in range(1, 7)
                ) + "\n",
        },
    )
    result = _run_gate(fake_worktree, base_sha=base_head)
    assert result.returncode == 1, (
        f"gate must reject PR whose base branch lacks anchors; "
        f"stdout={result.stdout!r} stderr={result.stderr!r}"
    )
    assert "target branch" in result.stderr
    assert "does not yet contain" in result.stderr


def test_gate_accepts_phase_one_change_when_base_branch_has_anchors(
    fake_worktree: Path,
) -> None:
    """F001 closure: companion to the rejection test above.  When the
    target branch already carries the merged anchor document AND the
    PR head touches a Phase 1+ surface, the gate must pass (the
    anchors file is already on the target branch, satisfying the
    merge-first contract)."""
    _write_anchors(fake_worktree, list(range(1, 7)))
    _commit(fake_worktree, "base: anchors", {})
    base_head = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=fake_worktree, env={**os.environ, "HOME": "/tmp"},
        check=True, capture_output=True, text=True,
    ).stdout.strip()

    # PR head: Phase 1+ change only (anchor file not touched again).
    _commit(
        fake_worktree, "PR: server change only",
        {"src/server/foo.c": "void foo(void) {}\n"},
    )

    result = _run_gate(fake_worktree, base_sha=base_head)
    assert result.returncode == 0, (
        f"gate must pass when target branch has anchors; "
        f"stdout={result.stdout!r} stderr={result.stderr!r}"
    )
    assert "present with all six decisions" in result.stdout


def test_gate_rejects_phase_one_change_when_base_branch_decisions_incomplete(
    fake_worktree: Path,
) -> None:
    """F001 closure: when the target branch carries the anchor file but
    it is missing one of the six ``## Decision N`` headings, the gate
    must reject the PR even though the file is present."""
    _write_anchors(fake_worktree, [1, 2, 3, 4, 5])  # missing Decision 6
    _commit(fake_worktree, "base: incomplete anchors", {})
    base_head = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=fake_worktree, env={**os.environ, "HOME": "/tmp"},
        check=True, capture_output=True, text=True,
    ).stdout.strip()

    _commit(
        fake_worktree, "PR: server change",
        {"src/server/foo.c": "void foo(void) {}\n"},
    )

    result = _run_gate(fake_worktree, base_sha=base_head)
    assert result.returncode == 1, (
        f"gate must reject PR whose base branch has incomplete anchors; "
        f"stdout={result.stdout!r} stderr={result.stderr!r}"
    )
    assert "missing on the target branch" in result.stderr
    assert "6" in result.stderr


def _run_gate_with_env(
    repo: Path, env_overrides: dict[str, str],
) -> subprocess.CompletedProcess:
    """Run the gate with ``env_overrides`` honoring the conventional
    HOME setup.  BASE_SHA is cleared unless explicitly set so the
    caller can drive the local-vs-CI branching deliberately."""
    env = os.environ.copy()
    env["HOME"] = "/tmp"
    env["GIT_AUTHOR_NAME"] = "test"
    env["GIT_AUTHOR_EMAIL"] = "test@test"
    env["GIT_COMMITTER_NAME"] = "test"
    env["GIT_COMMITTER_EMAIL"] = "test@test"
    # Clear every key the override targets so env_overrides wins.
    for key in env_overrides:
        env.pop(key, None)
    env.update(env_overrides)
    return subprocess.run(
        [str(repo / "scripts" / "check-collapse-anchor-gate.py")],
        cwd=repo, env=env, capture_output=True, text=True,
    )


def _stage(repo: Path, rel: str, content: str) -> None:
    """Write ``content`` to ``rel`` and stage it (no commit)."""
    target = repo / rel
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(content)
    _git(repo, "add", "--", rel)


def _write_unstaged(repo: Path, rel: str, content: str) -> None:
    """Write ``content`` to ``rel`` without staging."""
    target = repo / rel
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(content)


def test_gate_rejects_phase_one_change_when_uncommitted_in_local_mode(
    fake_worktree: Path,
) -> None:
    """F002 closure: in local mode (no BASE_SHA, no CI env), a Phase 1+
    file that exists in the working tree but has not been committed
    must still trip the gate.  ``HEAD~1..HEAD`` would miss it; the
    combined working-tree diff must catch it."""
    _write_anchors(fake_worktree, list(range(1, 7)))
    _commit(fake_worktree, "Phase 0", {})

    # Phase 1+ file modified in the working tree but NOT committed.
    _write_unstaged(fake_worktree, "src/server/foo.c", "void foo(void) {}\n")

    result = _run_gate(fake_worktree)
    assert result.returncode == 1, (
        f"gate must catch uncommitted Phase 1+ changes in local mode; "
        f"stdout={result.stdout!r} stderr={result.stderr!r}"
    )


def test_gate_rejects_phase_one_change_when_staged_in_local_mode(
    fake_worktree: Path,
) -> None:
    """F002 closure: a Phase 1+ file staged (added to the index) but
    not committed must still trip the gate in local mode."""
    _write_anchors(fake_worktree, list(range(1, 7)))
    _commit(fake_worktree, "Phase 0", {})

    _stage(fake_worktree, "src/server/foo.c", "void foo(void) {}\n")

    result = _run_gate(fake_worktree)
    assert result.returncode == 1, (
        f"gate must catch staged Phase 1+ changes in local mode; "
        f"stdout={result.stdout!r} stderr={result.stderr!r}"
    )


def test_gate_combines_committed_and_working_tree_changes_in_local_mode(
    fake_worktree: Path,
) -> None:
    """F002 closure: a Phase 1+ change spread across the last commit
    AND the working tree must still trip the gate; the union of both
    diffs is required for the contract to hold."""
    _write_anchors(fake_worktree, list(range(1, 7)))
    _commit(fake_worktree, "Phase 0", {})
    # Last commit: a non-Phase 1+ file only.
    _commit(fake_worktree, "docs", {"README.md": "x\n"})
    # Working tree: a Phase 1+ file as an unstaged edit.
    _write_unstaged(fake_worktree, "src/server/foo.c", "void foo(void) {}\n")

    result = _run_gate(fake_worktree)
    assert result.returncode == 1, (
        f"gate must catch Phase 1+ changes that only appear in the "
        f"working tree; stdout={result.stdout!r} stderr={result.stderr!r}"
    )


def test_gate_fails_closed_in_ci_mode_without_base_sha(
    fake_worktree: Path,
) -> None:
    """F001 closure: when the surrounding environment is a CI runner
    (``GITHUB_ACTIONS=true``) but ``BASE_SHA`` is unset, the gate
    must fail closed.  Silently passing would let a PR whose target
    branch does not yet carry the anchors pass, which is exactly the
    scenario the Phase 0 contract forbids."""
    _write_anchors(fake_worktree, list(range(1, 7)))
    _commit(fake_worktree, "Phase 0", {})
    _commit(fake_worktree, "Phase 1", {"src/server/foo.c": "x\n"})

    result = _run_gate_with_env(
        fake_worktree,
        {"GITHUB_ACTIONS": "true"},
    )
    assert result.returncode != 0, (
        f"gate must fail closed under CI mode without BASE_SHA; "
        f"stdout={result.stdout!r} stderr={result.stderr!r}"
    )
    assert "BASE_SHA" in result.stderr


def test_gate_fails_closed_in_ci_mode_via_ci_env_var(
    fake_worktree: Path,
) -> None:
    """F001 closure: the generic ``CI`` env var is also a CI signal,
    not only ``GITHUB_ACTIONS``.  This mirrors the matrix the
    workflow needs to cover."""
    _write_anchors(fake_worktree, list(range(1, 7)))
    _commit(fake_worktree, "Phase 0", {})
    _commit(fake_worktree, "Phase 1", {"src/server/foo.c": "x\n"})

    result = _run_gate_with_env(fake_worktree, {"CI": "true"})
    assert result.returncode != 0, (
        f"gate must fail closed under CI=true without BASE_SHA; "
        f"stdout={result.stdout!r} stderr={result.stderr!r}"
    )
    assert "BASE_SHA" in result.stderr


def test_gate_passes_in_local_mode_without_ci_when_no_phase_one_change(
    fake_worktree: Path,
) -> None:
    """F001 closure: the no-CI / no-BASE_SHA local path is preserved
    when no Phase 1+ path is touched (so the gate still passes on
    ordinary docs-only changes)."""
    _write_anchors(fake_worktree, list(range(1, 7)))
    _commit(fake_worktree, "Phase 0", {})
    _commit(fake_worktree, "docs", {"README.md": "x\n"})

    result = _run_gate(fake_worktree)
    assert result.returncode == 0, (
        f"gate must pass in local mode when no Phase 1+ change; "
        f"stdout={result.stdout!r} stderr={result.stderr!r}"
    )
    assert "no Phase 1+ paths changed" in result.stdout
