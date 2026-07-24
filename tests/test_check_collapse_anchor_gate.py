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
    Phase 1+ path.

    F3 follow-up: if the developer has any Phase 1+ path dirty in the
    real worktree (staged, unstaged, or untracked) the gate is
    supposed to fail closed, and running the smoke test in that
    state would produce a confusing failure that looks like a test
    bug.  Skip the smoke test in that case so the test suite stays
    usable while a developer has Phase 1+ work in flight.
    """
    # Some environments mark the worktree as "dubious ownership" and
    # ``git status`` fails closed; register it before reading status so
    # the smoke test is usable in such environments.
    subprocess.run(
        ["git", "config", "--global", "--add", "safe.directory", str(REPO_ROOT)],
        cwd=REPO_ROOT, env={**os.environ, "HOME": "/tmp"},
        capture_output=True, text=True,
    )
    status = subprocess.run(
        ["git", "status", "--porcelain"],
        cwd=REPO_ROOT, env={**os.environ, "HOME": "/tmp"},
        capture_output=True, text=True,
    )
    if status.returncode != 0:
        pytest.skip(
            "real worktree is not a usable git repo for this runner; "
            "smoke test is run against a clean anchor-only state"
        )
    dirty = [
        line[3:].strip().split(" -> ", 1)[-1]
        for line in status.stdout.splitlines() if line.strip()
    ]
    # F002: pull the Phase 1+ prefix set directly from the gate so this
    # skip list stays in lock-step when prefixes are added or removed.
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "gate", SCRIPT_SRC,
    )
    gate = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(gate)
    prefixes = gate.phase_one_prefixes()

    def _is_phase_one(path):
        if path == "docs/guardrails/collapse_anchors.md":
            return False
        for prefix in prefixes:
            if prefix.endswith("/"):
                if path == prefix or path.startswith(prefix):
                    return True
            else:
                if path == prefix:
                    return True
        return False

    if any(_is_phase_one(path) for path in dirty):
        pytest.skip(
            "real worktree has a Phase 1+ path dirty; "
            "smoke test is run against a clean anchor-only state"
        )

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


def test_gate_does_not_treat_plain_ci_env_as_ci_mode(
    fake_worktree: Path,
) -> None:
    """F3 follow-up: a plain ``CI=true`` env var (set by many local dev
    tools and IDEs) must NOT trip the gate's CI-mode branch.  Only
    ``GITHUB_ACTIONS`` is treated as authoritative so a developer
    running the gate from such a tool does not get a confusing
    "BASE_SHA unset in CI mode" failure with no Phase 1+ change
    actually present.

    The pre-F3 behaviour was: ``CI=true`` raised GateError even on a
    valid local-mode run, breaking developer tools that set
    ``CI=true`` (Jenkins, many IDEs, and various CLI utilities).  The
    post-F3 behaviour is: ``CI=true`` is ignored, the gate runs the
    local-mode contract, and the result is the same as if the env
    var were unset.  In this test setup the local-mode base
    (HEAD~1) carries the anchors, so the gate passes -- and
    importantly it does NOT print the CI-mode BASE_SHA error.
    """
    _write_anchors(fake_worktree, list(range(1, 7)))
    _commit(fake_worktree, "Phase 0", {})
    _commit(fake_worktree, "Phase 1", {"src/server/foo.c": "x\n"})

    result = _run_gate_with_env(fake_worktree, {"CI": "true"})
    assert result.returncode == 0, (
        f"gate must pass in local mode under CI=true when base has anchors; "
        f"stdout={result.stdout!r} stderr={result.stderr!r}"
    )
    assert "BASE_SHA is unset in CI mode" not in result.stderr, (
        "plain CI=true must not be treated as CI mode (F3 follow-up); "
        f"got stderr={result.stderr!r}"
    )
    assert "present with all six decisions" in result.stdout


def test_gate_plain_ci_env_does_not_mask_missing_base_anchors(
    fake_worktree: Path,
) -> None:
    """F3 follow-up (counterpart): when ``CI=true`` is set but the
    local base genuinely lacks the anchor file, the gate must still
    fail closed via the local-mode contract -- not via the
    CI-mode BASE_SHA branch, and not silently pass.  This confirms
    the F3 fix does not weaken the merge-first contract.
    """
    # Base commit has the seed but no anchor file.  The Phase 1+
    # change alone lands a server path.
    _commit(fake_worktree, "Phase 1", {"src/server/foo.c": "x\n"})

    result = _run_gate_with_env(fake_worktree, {"CI": "true"})
    assert result.returncode != 0, (
        f"gate must fail closed when local base lacks anchors under CI=true; "
        f"stdout={result.stdout!r} stderr={result.stderr!r}"
    )
    assert "BASE_SHA is unset in CI mode" not in result.stderr, (
        "plain CI=true must not be treated as CI mode (F3 follow-up); "
        f"got stderr={result.stderr!r}"
    )
    # Local-mode failure: the anchor file is required.
    assert "required" in result.stderr or "missing" in result.stderr


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


def test_gate_rejects_phase_one_path_committed_in_initial_commit(
    tmp_path: Path,
) -> None:
    """F3 follow-up: when ``HEAD~1`` is unavailable (initial commit) and
    the working tree is clean, a Phase 1+ path committed in that
    initial commit must still trip the gate.  Pre-F3 the gate would
    see an empty diff from ``_working_tree_names`` and pass silently,
    which defeated the merge-first contract for the very first commit
    of a fresh repository.
    """
    repo = tmp_path / "initialwt"
    repo.mkdir()
    scripts_dir = repo / "scripts"
    scripts_dir.mkdir()
    shutil.copyfile(SCRIPT_SRC, scripts_dir / "check-collapse-anchor-gate.py")
    (scripts_dir / "check-collapse-anchor-gate.py").chmod(0o755)
    _git(repo, "init", "-q", "-b", "main")
    _git(repo, "config", "user.email", "test@test")
    _git(repo, "config", "user.name", "test")
    _git(repo, "config", "--global", "--add", "safe.directory", str(repo))

    # Single commit containing a Phase 1+ path but NO anchor file.
    # The merge-first contract requires the anchors to be merged
    # before Phase 1+ work; an initial commit that ships Phase 1+
    # without the anchor file must fail closed.  Pre-F3 the gate
    # would have seen an empty diff (HEAD~1 is invalid AND the
    # working tree is clean) and passed silently.
    (repo / "src" / "server").mkdir(parents=True)
    (repo / "src" / "server" / "foo.c").write_text("void foo(void) {}\n")
    _git(repo, "add", "-A")
    _git(repo, "commit", "-q", "-m", "initial: phase 1+ path only")

    result = _run_gate(repo)
    assert result.returncode == 1, (
        f"gate must fail closed when an initial commit carries a Phase 1+ path "
        f"without the anchor file; "
        f"stdout={result.stdout!r} stderr={result.stderr!r}"
    )
    assert "required" in result.stderr


def test_gate_accepts_phase_one_path_committed_in_initial_commit_with_anchors(
    tmp_path: Path,
) -> None:
    """F3 follow-up (counterpart): an initial commit that lands the
    anchor document AND the Phase 1+ path in the same commit is a
    valid merge-first: the anchors are present at HEAD (which is the
    only available base reference).  The gate must pass.
    """
    repo = tmp_path / "initialwt_ok"
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
    (repo / "docs" / "guardrails" / "collapse_anchors.md").write_text(
        "# Anchors\n\n" + "\n".join(
            f"## Decision {n} - placeholder\n" for n in range(1, 7)
        ) + "\n"
    )
    (repo / "src" / "server").mkdir(parents=True)
    (repo / "src" / "server" / "foo.c").write_text("void foo(void) {}\n")
    _git(repo, "add", "-A")
    _git(repo, "commit", "-q", "-m", "initial: phase 1+ path with anchors")

    result = _run_gate(repo)
    assert result.returncode == 0, (
        f"gate must pass when an initial commit carries both anchors and "
        f"Phase 1+ path; stdout={result.stdout!r} stderr={result.stderr!r}"
    )
    assert "present with all six decisions" in result.stdout



# ---------------------------------------------------------------------------
# F002 closure: rejection tests for one representative path from each Phase
# 1+ surface family.  These prove the gate's prefix set covers every
# implementation surface named by the six ``## Decision N`` sections of
# ``docs/guardrails/collapse_anchors.md``.  Each test sets up a clean Phase 0
# base, lands a single file under the cited surface, and asserts the gate
# fails closed.
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("phase_one_path", [
    # Decision 1/3: webchat producer (POSIX compute seam).
    "src/posix/server_compute.c",
    # Decision 1/3: webchat consumer (DB1 polled row).
    "src/db1/webchat_live.c",
    # Decision 1/3: roundtable DB1 ledger.
    "src/db1/roundtable_pipeline.c",
    # Decision 1/3: roundtable relay (workflows module).
    "src/modules/workflows/wfe_live_panel.c",
    # Decision 1/3: roundtable relay (roundtable module).
    "src/modules/roundtable/delegate_ensemble.c",
    # Decision 1/3: delegate relay (agent_runtime).
    "src/server/agent_runtime.c",
    # Decision 1/3: delegate relay (POSIX agent_ir_parse).
    "src/posix/agent_ir_parse.c",
    # Decision 1/3: Anthropic compatibility relay emitter.
    "src/server/aimee_ir_stream.c",
    # Decision 1/3: Anthropic HTTP route handler.
    "src/server/anthropic_http.c",
    # Decision 1/3: OpenAI Chat + Responses relay.
    "src/server/openai_chat.c",
    # Decision 1/3: OpenAI frame formatting.
    "src/server/openai_shape.c",
    # Decision 1/3: server_http SSE entry.
    "src/server/server_http.c",
    # Decision 1/3: server_http_routes chat-live route.
    "src/server/server_http_routes.c",
    # Decision 1/3: typed envelope header.
    "src/headers/aimee_ir.h",
    # Decision 4: per-backend OpenAI sampling plumbing.
    "src/server/aimee_backend_openai.c",
    # Decision 4: per-backend Anthropic sampling plumbing.
    "src/server/aimee_backend_anthropic.c",
    # Decision 4: per-backend Bedrock sampling plumbing.
    "src/server/aimee_backend_bedrock.c",
    # Decision 4: curated local-model sampling overlay.
    "src/server/model_sampling.c",
    # Decision 4: request builder that gates Responses max_tokens.
    "src/server/agent_request_build.c",
    # Decision 5: bandit promotion storage.
    "src/db2/bandit.c",
    # Decision 5: server_state promote handler.
    "src/server/server_state.c",
    # Decision 6: audit-store WORM API source.
    "src/modules/audit/audit_worm.c",
    # Phase 1.0 docs: a follow-up packet under docs/guardrails/ (e.g.
    # collapse_promotion_bucketing.md per Decision 5) must not be allowed
    # before the anchors are merged.
    "docs/guardrails/collapse_promotion_bucketing.md",
    # Config source of truth (Decision 2).
    "src/modules/config/config.c",
])
def test_gate_rejects_each_phase_one_surface_in_f002_recommendation(
    fake_worktree, phase_one_path,
):
    """F002 closure: each surface named by a ``## Decision N`` section must
    trip the gate on its own.  A PR that introduces collapse work on any
    one of these paths must fail closed until the anchors are merged on
    the target branch.
    """
    # Phase 0 base: full anchor document with all six decisions.
    _write_anchors(fake_worktree, list(range(1, 7)))
    _commit(fake_worktree, "Phase 0", {})

    # Remove the anchor file from the working tree so it is not picked
    # up by the next ``git add -A`` -- the F002 contract forbids the
    # anchor file from being part of the Phase 1+ change, so the file
    # must NOT appear in the diff that the gate is validating.
    (fake_worktree / "docs" / "guardrails" / "collapse_anchors.md").unlink()

    # Land exactly one file at the cited Phase 1+ surface.
    target = fake_worktree / phase_one_path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text("/* placeholder */\n")
    _git(fake_worktree, "add", "-A")
    _git(fake_worktree, "commit", "-q", "-m", "Phase 1+:" + phase_one_path)

    result = _run_gate(fake_worktree)
    assert result.returncode == 1, (
        "gate must reject Phase 1+ change at " + repr(phase_one_path) + "; "
        "stdout=" + repr(result.stdout) + " stderr=" + repr(result.stderr)
    )


def test_gate_phase_one_prefixes_cover_every_decision_named_surface(fake_worktree):
    """F002 closure (unit-level): the gate's ``PHASE_ONE_PREFIXES`` must
    include at least one prefix per ``## Decision N`` section.  This
    catches the failure mode where a decision names a new file family
    but the gate's prefix set is not updated to match.
    """
    # Phase 0 base present so the contract check can run.
    _write_anchors(fake_worktree, list(range(1, 7)))
    _commit(fake_worktree, "Phase 0", {})

    import importlib.util
    spec = importlib.util.spec_from_file_location("gate", SCRIPT_SRC)
    gate = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(gate)
    prefixes = gate.phase_one_prefixes()

    # Each decision names at least one surface that must be present in
    # the prefix set.  These are the file families the collapse plan
    # names in ``docs/guardrails/collapse_anchors.md`` and
    # ``collapse_recon.md``.
    required_surfaces = (
        # Decision 1/3: Anthropic compatibility relay.
        "src/server/aimee_ir_stream.c",
        # Decision 1/3: OpenAI Chat + Responses.
        "src/server/openai_chat.c",
        # Decision 1/3: webchat producer.
        "src/posix/server_compute.c",
        # Decision 1/3: webchat consumer.
        "src/db1/webchat_live.c",
        # Decision 1/3: delegate relay.
        "src/server/agent_runtime.c",
        # Decision 1/3: roundtable relay.
        "src/modules/roundtable/",
        # Decision 4: backend sampling.
        "src/server/aimee_backend_openai.c",
        "src/server/aimee_backend_anthropic.c",
        "src/server/aimee_backend_bedrock.c",
        # Decision 5: promotion storage.
        "src/db2/bandit.c",
        # Decision 6: audit store.
        "src/modules/audit/",
        # Decision 2: config source.
        "src/modules/config/",
    )

    def _matches(path, prefix):
        if prefix.endswith("/"):
            return path == prefix or path.startswith(prefix)
        return path == prefix

    missing = [
        surface for surface in required_surfaces
        if not any(_matches(surface, prefix) for prefix in prefixes)
    ]
    assert not missing, (
        "PHASE_ONE_PREFIXES must cover every decision-named surface; "
        "missing=" + repr(missing)
    )


def test_gate_anchor_document_exempt_from_phase_one_rejection(fake_worktree):
    """F002 closure (carve-out): the anchor document itself is the single
    exempt path.  A PR that *adds* ``docs/guardrails/collapse_anchors.md``
    must not be forbidden by the gate (otherwise the contract would be
    impossible to bootstrap).  This is the documented
    ``_PHASE_ONE_EXEMPT`` carve-out.
    """
    # Base: no anchor file, no Phase 1+ surfaces.
    _commit(fake_worktree, "base: seed", {"README.md": "x\n"})
    # PR head: add the anchor file only.
    _write_anchors(fake_worktree, list(range(1, 7)))
    _commit(fake_worktree, "PR: add anchors only", {})

    result = _run_gate(fake_worktree)
    assert result.returncode == 0, (
        "gate must accept a PR whose only change is the anchor document; "
        "stdout=" + repr(result.stdout) + " stderr=" + repr(result.stderr)
    )
    # Either "present with all six decisions" (when the gate reaches the
    # final contract_present check) or "no Phase 1+ paths changed"
    # (when the anchor-only diff is correctly exempted from the
    # Phase 1+ surface check) is acceptable.  Both indicate the gate
    # accepted the PR; the important assertion is returncode == 0 and
    # that the gate did NOT print a "required" / "missing" error.
    assert "required" not in result.stderr, (
        "gate must not print a required-error for an anchor-only PR; "
        "stderr=" + repr(result.stderr)
    )
    assert "missing" not in result.stderr, (
        "gate must not print a missing-error for an anchor-only PR; "
        "stderr=" + repr(result.stderr)
    )
