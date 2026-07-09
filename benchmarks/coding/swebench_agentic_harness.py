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


# ------------------------------------------------------------ agentic loop -----
# S1 live wiring (roundtable 2026-07-05 + arm-C transport ruling 2026-07-08). A tools-using
# delegate runs its OWN explore/edit/test loop server-side under ONE dispatch; the loop bound is
# handed to the delegate as --max-turns/--timeout. The diff is taken from the provisioned
# workspace (`git diff base_commit`, byte-stable) when the workspace is co-located with the
# delegate, else from the delegate's returned unified diff (the proven PR #986 text path). Both
# paths run scan_and_redact_secrets. Attribution is by the dispatch's job_id (delegation_id ends
# in `-<job_id>`), captured here so the caller can read tokens without touching the filesystem.
_DIFF_LINE = re.compile(r"^(diff --git |index |--- |\+\+\+ |@@ |[ +\-\\]|rename |similarity |"
                        r"new file |deleted file |old mode |new mode |Binary )")


def extract_diff_from_text(text: str) -> str:
    """Pull a unified diff out of a delegate's response and STOP at the first non-diff line so
    trailing prose is never submitted as a patch. Mirrors bench_swebench_supervised._extract_diff
    (the merged single-shot extractor) so the agentic arm and the single-shot arm agree."""
    if not text:
        return ""
    m = re.search(r"```diff\s*\n(.*?)```", text, re.DOTALL)
    if m:
        return m.group(1).strip()
    lines = text.splitlines()
    start = next((i for i, ln in enumerate(lines)
                  if ln.startswith("diff --git ") or ln.startswith("--- ")), None)
    if start is None:
        return ""
    out = []
    for ln in lines[start:]:
        if ln == "" or _DIFF_LINE.match(ln):
            out.append(ln)
        else:
            break
    return "\n".join(out).strip()


def agentic_prompt(instance: dict, *, arm: str) -> str:
    """The bounded problem statement handed to a tools-using delegate. It gets the issue text and
    (when the prep extracted one) the focal file/region as a hint, and is told to edit the repo
    in its workspace and END by emitting the full unified diff (```diff fenced) so the harness can
    extract a patch even when it cannot read the delegate's server-side worktree."""
    repo = instance.get("repo", "the repository")
    problem = instance.get("problem") or instance.get("problem_statement", "")
    focal = instance.get("file", "")
    region = instance.get("region", "")
    hint = f"\n\nThe fix likely touches `{focal}`.\n```python\n{region}\n```" if region else ""
    return (f"You are fixing a bug in {repo}. Explore the checked-out repository in your "
            f"workspace, make the minimal edit that resolves the issue, and (if you can) run the "
            f"relevant tests.\n\n## Issue\n{problem}{hint}\n\nWhen done, output the COMPLETE fix as "
            f"a single unified git diff in a ```diff fenced block (a/ and b/ prefixes, correct "
            f"context lines). Do not commit; leave the working tree dirty.")


@dataclass
class AgenticResult:
    """One worker/primary agentic run: the emitted patch + attribution handles + loop telemetry."""
    patch: str
    redactions: int
    job_id: int | None
    delegation_id: str | None
    agent: str
    status: str
    api_calls: int
    error: str = ""
    patch_source: str = "none"      # "workspace" | "response_text" | "none"
    returned_diff: str = ""          # the diff text the delegate CLAIMED (diagnostic only)

    @property
    def authoritative(self) -> bool:
        """A patch is graded-authoritative ONLY when it came from the co-located workspace's actual
        filesystem state (`git diff`). A response-text diff is a CLAIM the agent may not have applied
        — grading it would decouple 'resolved' from the filesystem (the roundtable's hallucination-
        gap risk), so it is never authoritative."""
        return self.patch_source == "workspace" and bool(self.patch.strip())

    @property
    def ok(self) -> bool:
        return self.status == "done" and not self.error and bool(self.patch.strip())


def worktree_branch(instance_id: str, arm: str, worker_idx: int) -> str:
    """Deterministic per-(instance,arm,worker) worktree branch (F2 isolation naming)."""
    safe = re.sub(r"[^A-Za-z0-9_.-]", "-", f"{instance_id}-{arm}-{worker_idx}")
    return f"aimee/wi/swebench-{safe}"


def run_agentic_loop(instance: dict, env: WorkerEnv, budget: LoopBudget, *, arm: str,
                     worker: str | None = None, base_repo: str | None = None,
                     token_db: str = "", session_id: str = "", worker_idx: int = 0,
                     aimee_bin: str = "aimee", role: str = "code",
                     dispatch=None) -> AgenticResult:
    """Live per-instance agentic loop, shared by arm A (primary drives) and arm C (worker drives).

    Provisions an isolated workspace at base_commit (when `base_repo` is given and co-located with
    the delegate), dispatches ONE tools-enabled `code` delegate on a per-worker worktree branch
    (FINDING 4: tools need --worktree), polls to a bounded terminal, and extracts a canonical,
    secret-redacted patch — preferring the workspace diff, falling back to the delegate's returned
    diff text. `dispatch` is injected (default: the live transport) so CI drives a fake fleet.

    Returns an AgenticResult; a fleet/patch failure is recorded on it, never raised."""
    from benchmarks.coding import swebench_live_transport as _T
    if dispatch is None:
        dispatch = _T.dispatch_and_wait
    instance_id = instance["instance_id"]
    base_commit = instance.get("base_commit", "")
    branch = worktree_branch(instance_id, arm, worker_idx)

    if base_repo:
        try:
            provision_workspace(base_repo, base_commit, env.workspace)
        except ProvisionError as e:
            return AgenticResult("", 0, None, None, worker or "", "error", 0, error=str(e))

    outcome = dispatch(
        role, agentic_prompt(instance, arm=arm), aimee_bin=aimee_bin, via=worker,
        persona="engineer", tools=True, worktree=branch, max_turns=budget.max_turns,
        timeout_ms=int(budget.max_wall_s * 1000), token_db=token_db, session_id=session_id)

    returned_diff = extract_diff_from_text(outcome.result)   # what the delegate CLAIMED (diagnostic)
    patch, redactions, source = "", 0, "none"
    ws = Path(env.workspace)
    if base_repo:
        # Co-located mode: the ACTUAL filesystem diff is the ONLY graded source (anti hallucination-
        # gap — a response-text diff the agent never applied must never be graded). If git diff
        # yields nothing, the extraction failed; we do NOT fall back to the claimed text.
        if (ws / ".git").exists():
            try:
                patch, redactions = extract_patch(env.workspace, base_commit)
                source = "workspace" if patch.strip() else "none"
            except (PatchError, ProvisionError, subprocess.CalledProcessError):
                patch, source = "", "none"
    else:
        # Degraded/dev mode (no co-located workspace): the returned text is all there is. Flag it
        # NON-AUTHORITATIVE so the reporter can exclude it from the graded headline.
        patch, redactions = scan_and_redact_secrets(returned_diff)
        source = "response_text" if patch.strip() else "none"

    status = outcome.status if outcome.job_id is not None else "error"
    return AgenticResult(patch=patch, redactions=redactions, job_id=outcome.job_id,
                         delegation_id=outcome.delegation_id, agent=outcome.agent_name or (worker or ""),
                         status=status, api_calls=outcome.api_calls, error=outcome.error,
                         patch_source=source, returned_diff=returned_diff)
