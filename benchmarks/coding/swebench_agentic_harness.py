#!/usr/bin/env python3
"""S1 — per-worker agentic harness for the agentic supervised SWE-bench benchmark (#987).

Provisions an isolated, editable repo workspace at a SWE-bench instance's `base_commit`,
runs a BOUNDED agentic loop (explore/edit/[test]/verify) in it, and emits a canonical,
byte-stable unified diff = the "model patch" the OFFICIAL SWE-bench Docker grader scores.
Used by BOTH arms (A: the primary drives the loop; C: a fleet worker drives it while the
primary supervises) — the pure loop core is shared so a transport difference can never be
mistaken for an algorithmic one.

This module is the S1 DESIGN-ROUNDTABLE's "pure surface" (2026-07-05, 5/7 panel): everything
here is unit-testable with NO live server, NO docker, NO network. The reproducibility-critical
logic lives here; the live parts (delegate transport, .254 workspace creation, per-worker
container startup, official grading) are narrow, MARKED stubs that fail honestly.

Ratified rulings baked in:
  Q1  in-loop tests default OFF (option c): iterate on git_verify+build; the official grader
      is the SOLE resolution source, run once at the end -> iteration is fully reproducible and
      decoupled from the single grader host (CT 101). resolve_test_command() supports opt-in.
  Q2  per-worker OS isolation: a per-worker Docker container is the DEFAULT isolation unit for
      any worker that executes tests/builds; venv + a port/TMPDIR/HOME/cache allocation table is
      the documented FALLBACK when Docker is unavailable (e.g. the .254 LXC2Docker shim). The
      allocation TABLE is needed either way and is implemented + tested here.
  Q3  workspaces live where the loop runs (arm C: server-side on .254; arm A: via /v1/runs);
      patch = `git -C <ws> diff <base_commit>`. The provisioning + extraction logic is shared.
  Q5  commit hygiene (forbid in-loop commits; assert HEAD==base_commit before diff), artifact
      scrub, deterministic diff flags, submodule/LFS rejection, and a secret-scan+redact pass
      over every emitted patch before it can reach a ledger/log.
"""
from __future__ import annotations

import hashlib
import os
import re
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

# ------------------------------------------------------------ resource table --
# Q2: even when the default isolation unit is a container, the harness must hand each worker a
# non-overlapping (port range, TMPDIR, HOME, cache dirs) tuple — inside the container these keep
# a crashed worker from poisoning a sibling; in the venv fallback they ARE the isolation. The
# allocation is deterministic per (instance, arm, worker) so a re-run reproduces byte-for-byte.
_PORTS_PER_WORKER = 16  # a small block; SWE-bench tests rarely bind more than a couple


@dataclass(frozen=True)
class WorkerEnv:
    key: str
    port_base: int
    port_count: int
    workspace: str
    tmpdir: str
    home: str
    cache_dir: str

    def env(self) -> dict:
        """The OS-resource env a worker's test/build commands run under (venv fallback path)."""
        return {"TMPDIR": self.tmpdir, "HOME": self.home,
                "XDG_CACHE_HOME": self.cache_dir, "PIP_CACHE_DIR": self.cache_dir,
                "CONDA_PKGS_DIRS": self.cache_dir,
                "AIMEE_BENCH_PORT_BASE": str(self.port_base)}


class EnvAllocator:
    """Hands out non-overlapping WorkerEnvs from a bounded port pool with explicit release and
    range-exhaustion errors. Deterministic slot assignment keyed by (instance, arm, worker)."""

    def __init__(self, root: str, base_port: int = 21000, max_workers: int = 256):
        self.root = Path(root)
        self.base_port = base_port
        self.max_workers = max_workers
        self._slots: dict[str, int] = {}
        self._free: list[int] = []
        self._next = 0

    def _slot_for(self, key: str) -> int:
        if key in self._slots:
            return self._slots[key]
        if self._free:
            slot = self._free.pop()
        elif self._next < self.max_workers:
            slot = self._next
            self._next += 1
        else:
            raise RuntimeError(f"port/worker pool exhausted (max_workers={self.max_workers})")
        self._slots[key] = slot
        return slot

    def allocate(self, instance_id: str, arm: str, worker_idx: int) -> WorkerEnv:
        key = f"{instance_id}:{arm}:{worker_idx}"
        slot = self._slot_for(key)
        base = self.root / key.replace(":", "__").replace("/", "_")
        return WorkerEnv(
            key=key,
            port_base=self.base_port + slot * _PORTS_PER_WORKER,
            port_count=_PORTS_PER_WORKER,
            workspace=str(base / "ws"),
            tmpdir=str(base / "tmp"),
            home=str(base / "home"),
            cache_dir=str(base / "cache"),
        )

    def release(self, instance_id: str, arm: str, worker_idx: int) -> None:
        key = f"{instance_id}:{arm}:{worker_idx}"
        slot = self._slots.pop(key, None)
        if slot is not None:
            self._free.append(slot)

    def port_ranges(self) -> list[tuple[int, int]]:
        """(start, end) inclusive for each live slot — used by tests to assert non-overlap."""
        return sorted((self.base_port + s * _PORTS_PER_WORKER,
                       self.base_port + s * _PORTS_PER_WORKER + _PORTS_PER_WORKER - 1)
                      for s in self._slots.values())


# ------------------------------------------------------------ test commands ----
# Q1: default OFF, but when in-loop testing is opted in the command is resolved deterministically
# from the repo (SWE-bench's four repo families). This is the repo's own runner, NOT the grader.
_TEST_COMMANDS = {
    "django/django": "python tests/runtests.py --parallel=1 --verbosity=0",
    "sympy/sympy": "python -m pytest -q",
    "scikit-learn/scikit-learn": "python -m pytest -q",
    "pytest-dev/pytest": "python -m pytest -q",
}


def resolve_test_command(repo: str) -> str | None:
    """The in-loop test command for a repo, or None if unknown (loop falls back to build/verify)."""
    return _TEST_COMMANDS.get(repo)


# ------------------------------------------------------------ provisioning -----
class ProvisionError(RuntimeError):
    pass


def _git(ws, *args, check=True, capture=True):
    return subprocess.run(["git", "-C", str(ws), *args], check=check,
                          capture_output=capture, text=True)


def assert_no_submodule_or_lfs(base_repo: str) -> None:
    """Q5: reject submodule/LFS instances up front rather than silently mis-diffing them. The
    four target repo families are pure-python (none), so this is a guard, not a common path."""
    p = Path(base_repo)
    if (p / ".gitmodules").exists():
        raise ProvisionError(f"{base_repo}: has .gitmodules; submodule instances are rejected "
                             "(use --submodule=diff handling before enabling)")
    attrs = p / ".gitattributes"
    if attrs.exists() and "filter=lfs" in attrs.read_text(errors="replace"):
        raise ProvisionError(f"{base_repo}: uses git-lfs; LFS instances are rejected")


def provision_workspace(base_repo: str, base_commit: str, dest: str) -> str:
    """Create an isolated, editable checkout of base_repo at base_commit under dest.

    Q5 hygiene: force a clean detached checkout at base_commit and scrub ALL untracked/ignored
    state (`git checkout -f` + `git clean -fdx`) so no leftover build artifact or prior-run edit
    can pollute the eventual diff. Returns the workspace path. Uses a local worktree off the base
    clone (cheap, shares the object store) and asserts the result is exactly at base_commit.
    """
    assert_no_submodule_or_lfs(base_repo)
    dest_p = Path(dest)
    dest_p.parent.mkdir(parents=True, exist_ok=True)
    if not dest_p.exists():
        # A detached worktree off the base clone: isolated working tree, shared objects.
        r = _git(base_repo, "worktree", "add", "--detach", "--force", str(dest_p), base_commit,
                 check=False)
        if r.returncode != 0:
            raise ProvisionError(f"worktree add failed: {r.stderr.strip()}")
    else:
        _git(dest_p, "checkout", "-f", "--detach", base_commit)
    _git(dest_p, "clean", "-fdx")
    head = _git(dest_p, "rev-parse", "HEAD").stdout.strip()
    if not head.startswith(base_commit) and not base_commit.startswith(head):
        raise ProvisionError(f"workspace HEAD {head} != base_commit {base_commit}")
    return str(dest_p)


# ------------------------------------------------------------ patch extraction -
# Q5: a canonical, byte-stable patch. `git add -A` stages tracked edits AND untracked NEW source
# files (the agent's creations) while .gitignore keeps build artifacts out; `git diff --cached`
# against base_commit then emits the full model patch. Deterministic flags kill host-dependent
# ordering/renames/external-diff. Standard a/ b/ prefixes are KEPT (the grader applies with
# `git apply`, which expects them). We assert HEAD==base_commit so an accidental in-loop commit
# is caught rather than silently producing an empty/partial diff.
_DIFF_FLAGS = ["--no-ext-diff", "--no-color", "--no-renames", "--submodule=diff",
               "--src-prefix=a/", "--dst-prefix=b/"]


class PatchError(RuntimeError):
    pass


def extract_patch(workspace: str, base_commit: str, *, redact_secrets: bool = True) -> tuple[str, int]:
    """Return (canonical_patch, redaction_count) for all changes since base_commit.

    Forbids in-loop commits (asserts HEAD==base_commit). Stages everything not gitignored, diffs
    against base_commit with deterministic flags, then secret-scans+redacts before the patch can
    reach any ledger/log."""
    head = _git(workspace, "rev-parse", "HEAD").stdout.strip()
    if not (head.startswith(base_commit) or base_commit.startswith(head)):
        raise PatchError(f"HEAD {head} != base_commit {base_commit}: worker committed in-loop "
                         "(forbidden — leave the tree dirty)")
    _git(workspace, "add", "-A")
    env = dict(os.environ, GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_SYSTEM="/dev/null")
    r = subprocess.run(["git", "-C", str(workspace), "diff", "--cached", *_DIFF_FLAGS, base_commit],
                       check=True, capture_output=True, text=True, env=env)
    patch = r.stdout
    n = 0
    if redact_secrets:
        patch, n = scan_and_redact_secrets(patch)
    return patch, n


# ------------------------------------------------------------ secret scan ------
# Q5 security: agents edit config files and may paste credentials into source; the emitted patch
# is persisted to the token_audit ledger / analysis artifacts. Redact known secret shapes and
# high-entropy assignments with a STABLE marker (so the patch stays byte-stable) and count them.
_SECRET_PATTERNS = [
    re.compile(r"AKIA[0-9A-Z]{16}"),                                  # AWS access key id
    re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH |DSA |PGP )?PRIVATE KEY-----"),
    re.compile(r"gh[pousr]_[A-Za-z0-9]{36,}"),                        # GitHub tokens
    re.compile(r"sk-[A-Za-z0-9]{20,}"),                               # OpenAI-style keys
    re.compile(r"xox[baprs]-[A-Za-z0-9-]{10,}"),                      # Slack tokens
]
# KEY = "high-entropy value" assignments (env/config lines the agent might add).
_ASSIGN_RE = re.compile(
    r'((?:secret|token|password|passwd|api[_-]?key|access[_-]?key)\s*[=:]\s*["\']?)'
    r'([A-Za-z0-9+/_\-]{16,})(["\']?)', re.IGNORECASE)
_REDACTED = "AIMEE_REDACTED_SECRET"


def _shannon_entropy(s: str) -> float:
    if not s:
        return 0.0
    import math
    counts = {c: s.count(c) for c in set(s)}
    return -sum((n / len(s)) * math.log2(n / len(s)) for n in counts.values())


def scan_and_redact_secrets(text: str) -> tuple[str, int]:
    """Replace secret-shaped substrings with a stable marker; return (redacted_text, count)."""
    count = 0

    def _mark(_m):
        nonlocal count
        count += 1
        return _REDACTED

    out = text
    for pat in _SECRET_PATTERNS:
        out = pat.sub(_mark, out)

    def _assign(m):
        nonlocal count
        val = m.group(2)
        # Only redact if the value looks high-entropy (avoid nuking ordinary identifiers).
        if _shannon_entropy(val) >= 3.2:
            count += 1
            return f"{m.group(1)}{_REDACTED}{m.group(3)}"
        return m.group(0)

    out = _ASSIGN_RE.sub(_assign, out)
    return out, count


# ------------------------------------------------------------ loop bounds ------
@dataclass
class LoopBudget:
    """Bounds the agentic loop (mirrors the merged F1a/F1b turn/wall/USD caps at the bench level).
    stop_on_verify_pass ends the loop as soon as the mechanical gate is green."""
    max_turns: int = 12
    max_wall_s: float = 900.0
    max_usd: float = 1.0
    stop_on_verify_pass: bool = True

    def should_continue(self, *, turns: int, wall_s: float, usd: float, verify_passed: bool) -> bool:
        if self.stop_on_verify_pass and verify_passed:
            return False
        return turns < self.max_turns and wall_s < self.max_wall_s and usd < self.max_usd

    def stop_reason(self, *, turns: int, wall_s: float, usd: float, verify_passed: bool) -> str:
        if self.stop_on_verify_pass and verify_passed:
            return "verify_passed"
        if turns >= self.max_turns:
            return "max_turns"
        if wall_s >= self.max_wall_s:
            return "max_wall"
        if usd >= self.max_usd:
            return "max_usd"
        return "running"


# ------------------------------------------------------------ provenance -------
def workspace_fingerprint(repo: str, base_commit: str, patch: str) -> str:
    """A stable id for an emitted solution (repo + base_commit + canonical patch) for audit."""
    h = hashlib.sha256()
    h.update(f"{repo}\n{base_commit}\n".encode())
    h.update(patch.encode())
    return h.hexdigest()[:16]


# ------------------------------------------------------------ LIVE stubs -------
# The following need a live server / .254 workspace / container / grader and are NOT exercised in
# CI. They fail honestly with the exact wiring to do, mirroring S0's stub discipline.
def run_agentic_loop(instance: dict, env: WorkerEnv, budget: LoopBudget, *, arm: str,
                     worker: str | None = None) -> str:
    raise NotImplementedError(
        "live agentic loop not wired. arm C: dispatch a tools-enabled `code`/`execute` delegate "
        "against a server-side .254 workspace (Q3 option a), poll to a bounded terminal, extract "
        "`git -C <ws> diff <base_commit>`. arm A: drive the same loop via the S0 /v1/runs "
        "transport. In-loop tests default OFF (Q1 c); isolation via per-worker container or the "
        "venv+EnvAllocator fallback (Q2).")
