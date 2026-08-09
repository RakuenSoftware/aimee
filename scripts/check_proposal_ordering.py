#!/usr/bin/env python3
"""Require the approved Git child contract to precede every Git migration signal."""

from __future__ import annotations

import argparse
import difflib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
from typing import NoReturn


REPO_ROOT = Path(__file__).resolve().parent.parent
SLICE2_ANCHOR = "a3c4d413b6ce5f674994a6e6c4589ae2383819a4"
# Git-contract signals that neither existed at Slice 2 approval nor descend from
# it: work branched before the anchor and merged after without being rebased onto
# the approved contract. They are real ordering violations, and they are frozen
# history -- the pinned `historical_cutoff` cannot be advanced to cover them
# because it is part of the drift-checked discovery view.
#
# Recording them is the only honest option left: waiving them silently would hide
# the violation, and failing on them forever would make the gate unpassable and so
# get it switched off. The set is closed. A new non-descendant signal fails, and a
# waived commit that stops being a signal must be deleted, so it only shrinks.
PRE_APPROVAL_SIGNALS = frozenset({
    "98ea378111cac8b1173c5719c05a6472ec2ba052",  # 2026-07-20 Merge PR #1676 fix/mcp-git-long-lived
    "bed04cbd132ae0c5e8045e8eb558586aa5660b6f",  # 2026-07-20 Merge testing into claude/tiered-llm
    "c1a6925b6e7d69d8131722411a672d421cff9457",  # 2026-07-20 fix(git): allow advanced long-lived PR branches
    "f20ed22893e3e6c62fddfb32a7d2d4d79f9e3f3e",  # 2026-07-20 Merge testing into claude/tiered-llm
    "04f458d4ddd404d1a3eb8916deae62045bfa0856",  # 2026-07-21 tighten WFE forge operation contracts
    "09610d34630aa5276724c105e4efbcca90137f93",  # 2026-07-21 format WFE forge resource code
    "21974f9d8ac36212f721176c1cd7df4bf8c6876b",  # 2026-07-21 fix(wfe): suppress squash commit trailers (#1683)
    "26196ebbfb333272ac262afee05783034de46ef8",  # 2026-07-21 Merge testing into claude/tiered-llm
    "308252ea1d4b85b5143d963e6750e3b6303c8ea4",  # 2026-07-21 route Go WFE forge ops through credential plane
    "68413cd4bb8c764b8b5c8140d0ec77183a81d4cd",  # 2026-07-21 accept and confine dotted WFE slice ids
    "8ee53bf464348c564bd5abd7d40f6070f27fc12a",  # 2026-07-21 close WFE forge boundary review findings
    "a6a72c5f473b5d7e614f5ac2d2ea3fa6d809c9e5",  # 2026-07-21 harden WFE forge execution boundary
    "b02a99f62e173fbfe95b3799093b25431b03775b",  # 2026-07-21 Merge testing into fix/server-*
    "fdf5eee3ff287e926a11e71b7b74e41470b21b19",  # 2026-07-21 inspect included local git configuration
    "07be17808892da8c1bfe09b2c196ac010a7a7e87",  # 2026-07-22 Merge PR #1709 rewrite/go-server
    "4cb97581cd5d003d09f3dd3184a133de942b9da7",  # 2026-07-22 Merge testing into agent/economizer
    "5618ca8dcc624ef52e2690b3b00ff1b337f81615",  # 2026-07-22 Merge PR #1774 agent/transport-performance
    "58a287d007ade81710dedbf04a27ab0dc673910d",  # 2026-07-22 Merge PR #1816 from testing
    "92742168850cf19b0464717023b37aedec51f259",  # 2026-07-22 feat: start mTLS transport performance
    "be13d1bb262b712a82ef7779f497597dada46ea5",  # 2026-07-22 Merge testing into claude/tiered-llm
    "d9111213e40785d85a8ee3252483b55546c7dfba",  # 2026-07-22 Merge testing into agent/core-substrate-proposal
    "acf1e9621cff770ece49c57205664099723af941",  # 2026-07-25 Core modularization current with testing
    "b209ac32ee444e85b9cfb260434ac93572320f57",  # 2026-07-25 Merge testing into feat/bus-arena-routing
    "6d01547cf4d2c42c66bbaa4a8e1f35c6ea6f1859",  # 2026-07-27 Merge PR #2068 from testing
})
ANCHOR_CONTRACT_PATH = "docs/proposals/pending/git-core-contract.md"
CONTRACT_PATH = "docs/proposals/done/git-core-contract.md"
EVIDENCE_PATH = "docs/validation/roundtable/git-core-contract.json"
HANDOFF_PATH = "docs/validation/core-modularization-slice-2.md"
CHECKER_PATH = "scripts/check_git_core_contract.py"
FEATURE_BRANCH = "feature/core-modularization"
# The branches this gate is allowed to run against. It must stay equal to the
# base-branch filters in .github/workflows/module-inventory.yml: a branch the
# workflow triggers on but this rejects fails every run with rule=event-base,
# and a branch this accepts but the workflow never triggers on is dead weight.
# Git-contract work now merges to testing, so testing is gated; the integration
# bases are here because sub-PRs stack onto them before reaching it.
GATED_BASES = ("main", "testing", FEATURE_BRANCH)
GATED_BASE_PREFIXES = ("aimee/feat/", "agent/")
GIT = shutil.which("git", path="/usr/bin:/bin") or "/usr/bin/git"
PYTHON = shutil.which("python3", path="/usr/bin:/bin") or "/usr/bin/python3"
TRIGGER_GROUPS = (
    "descriptors",
    "generated_builds",
    "generated_profiles",
    "readiness_markers",
    "status_claim_roots",
)
HANDOFF = {
    "schema_version": 1,
    "receiver": "slice-3-proposal-ordering-gate",
    "contract_file": CONTRACT_PATH,
    "evidence_file": EVIDENCE_PATH,
    "invariants_source": "git-core-contract.invariants",
    "ordering_script_baseline": "6ce37f53e1f627c19e15fc01f68959f546a5eded",
    "trigger_surface_source": "git-core-contract",
}
PINNED_HANDOFF = {**HANDOFF, "contract_file": ANCHOR_CONTRACT_PATH}


class OrderingError(ValueError):
    """A fail-closed ordering error with a stable rule name."""


def fail(rule: str, message: str) -> NoReturn:
    raise OrderingError(f"rule={rule}: {message}")


def git_run(repo: Path, *args: str) -> subprocess.CompletedProcess[bytes]:
    try:
        return subprocess.run(
            [GIT, *args],
            cwd=repo,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    except OSError as exc:
        fail("git-exec", f"cannot execute {GIT}: {exc}")


def git(repo: Path, *args: str) -> bytes:
    result = git_run(repo, *args)
    if result.returncode != 0:
        stderr = result.stderr.decode("utf-8", "replace").strip()
        fail("git-command", f"git {' '.join(args)} failed ({result.returncode}): {stderr}")
    return result.stdout


def git_text(repo: Path, *args: str) -> str:
    try:
        return git(repo, *args).decode("utf-8").strip()
    except UnicodeDecodeError as exc:
        fail("git-output", f"git {' '.join(args)} returned invalid UTF-8: {exc}")


def require_repository(repo: Path) -> None:
    result = git_run(repo, "rev-parse", "--show-toplevel")
    if result.returncode != 0:
        fail("config-root", f"{repo} is not a Git repository")
    try:
        top = result.stdout.decode("utf-8").strip()
    except UnicodeDecodeError as exc:
        fail("config-root", f"repository root is not UTF-8: {exc}")
    if Path(os.path.realpath(top)) != repo:
        fail("config-root", f"{repo} is not the repository root")


def _no_duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    value: dict[str, object] = {}
    for key, item in pairs:
        if key in value:
            fail("json-duplicate-key", f"duplicate object key {key!r}")
        value[key] = item
    return value


def _reject_number(value: str) -> NoReturn:
    fail("json-number-domain", f"floating, exponent, or non-finite number forbidden: {value}")


def loads_strict(raw: str, *, label: str) -> dict[str, object]:
    try:
        value = json.loads(
            raw,
            object_pairs_hook=_no_duplicate_keys,
            parse_float=_reject_number,
            parse_constant=_reject_number,
        )
    except json.JSONDecodeError as exc:
        fail("json-parse", f"{label}: {exc.msg} at {exc.lineno}:{exc.colno}")
    if not isinstance(value, dict):
        fail("contract-shape", f"{label} must be a JSON object")
    return value


def extract_json_fence_text(raw: str, name: str, *, label: str) -> dict[str, object]:
    opening = re.compile(rf"^```json {re.escape(name)}[ \t]*$", re.MULTILINE)
    matches = list(opening.finditer(raw))
    if len(matches) != 1:
        fail("contract-fence", f"{label}: expected exactly one {name} JSON fence")
    body = raw[matches[0].end() :]
    if body.startswith("\r\n"):
        body = body[2:]
    elif body.startswith("\n"):
        body = body[1:]
    closing = re.search(r"^```[ \t]*$", body, re.MULTILINE)
    if closing is None:
        fail("contract-fence", f"{label}: unterminated {name} JSON fence")
    return loads_strict(body[: closing.start()].rstrip("\r\n"), label=label)


def read_live_text(repo: Path, relative: str) -> str:
    candidate = repo / relative
    resolved = Path(os.path.realpath(candidate))
    try:
        resolved.relative_to(repo)
    except ValueError:
        fail("path-containment", f"{relative} escapes repository root")
    if candidate.is_symlink():
        fail("input-symlink", f"{relative} must not be a symlink")
    try:
        if not stat.S_ISREG(candidate.stat(follow_symlinks=False).st_mode):
            fail("input-not-regular", f"{relative} must be a regular file")
        return resolved.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        fail("input", f"cannot read {relative}: {exc}")


def anchor_text(repo: Path, relative: str, rule: str) -> str:
    result = git_run(repo, "show", f"{SLICE2_ANCHOR}:{relative}")
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", "replace").strip()
        fail(rule, f"cannot read {relative} from Slice 2 anchor: {detail}")
    try:
        return result.stdout.decode("utf-8")
    except UnicodeDecodeError as exc:
        fail(rule, f"{relative} at Slice 2 anchor is not UTF-8: {exc}")


def validate_handoff(
    value: dict[str, object], expected: dict[str, object], *, label: str
) -> None:
    if value != expected:
        fail("handoff-shape", f"{label} differs from the exact Slice 2 handoff")


def validate_trusted_contract(repo: Path) -> None:
    checker = git(repo, "show", f"{SLICE2_ANCHOR}:{CHECKER_PATH}")
    with tempfile.TemporaryDirectory(prefix="aimee-ordering-") as directory:
        checker_path = Path(directory) / "check_git_core_contract.py"
        checker_path.write_bytes(checker)
        checker_path.chmod(0o400)
        # The immutable Slice-2 checker predates proposal archival and therefore hardcodes the
        # original pending/ contract and handoff paths. Validate the current contract with that
        # trusted checker in a disposable shared clone containing only this path-compatibility
        # projection; never reintroduce the archived proposal into the live tree.
        compatibility_root = Path(directory) / "repo"
        clone = subprocess.run(
            [GIT, "clone", "--quiet", "--shared", "--no-checkout", str(repo), str(compatibility_root)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if clone.returncode != 0:
            fail(
                "checker-compat-clone",
                clone.stderr.decode("utf-8", "replace").strip() or "shared clone failed",
            )
        compatibility_contract = compatibility_root / ANCHOR_CONTRACT_PATH
        compatibility_contract.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(repo / CONTRACT_PATH, compatibility_contract)
        compatibility_handoff = compatibility_root / HANDOFF_PATH
        compatibility_handoff.parent.mkdir(parents=True, exist_ok=True)
        compatibility_handoff.write_bytes(git(repo, "show", f"{SLICE2_ANCHOR}:{HANDOFF_PATH}"))
        compatibility_evidence = compatibility_root / EVIDENCE_PATH
        compatibility_evidence.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(repo / EVIDENCE_PATH, compatibility_evidence)
        try:
            result = subprocess.run(
                [
                    PYTHON,
                    "-I",
                    "-S",
                    str(checker_path),
                    "--config-root",
                    str(compatibility_root),
                    "--require-status",
                    "roundtable-approved",
                ],
                cwd=repo,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
        except OSError as exc:
            fail("checker-exec", f"cannot execute trusted contract checker: {exc}")
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", "replace").strip()
        fail("contract-validation", f"trusted Slice 2 checker rejected HEAD: {detail}")


def canonical_metadata(repo: Path) -> tuple[dict[str, object], dict[str, object]]:
    contract = extract_json_fence_text(
        anchor_text(repo, ANCHOR_CONTRACT_PATH, "anchor-contract"),
        "git-core-contract",
        label=f"{SLICE2_ANCHOR}:{ANCHOR_CONTRACT_PATH}",
    )
    evidence = git_run(repo, "cat-file", "-e", f"{SLICE2_ANCHOR}:{EVIDENCE_PATH}")
    if evidence.returncode != 0:
        fail("anchor-evidence", f"{EVIDENCE_PATH} is absent from the Slice 2 anchor")
    handoff = extract_json_fence_text(
        anchor_text(repo, HANDOFF_PATH, "anchor-handoff"),
        "slice3-handoff",
        label=f"{SLICE2_ANCHOR}:{HANDOFF_PATH}",
    )
    validate_handoff(handoff, PINNED_HANDOFF, label="pinned handoff")
    return contract, handoff


def live_metadata(repo: Path) -> tuple[dict[str, object], dict[str, object]]:
    contract = extract_json_fence_text(
        read_live_text(repo, CONTRACT_PATH), "git-core-contract", label=CONTRACT_PATH
    )
    handoff = extract_json_fence_text(
        read_live_text(repo, HANDOFF_PATH), "slice3-handoff", label=HANDOFF_PATH
    )
    validate_handoff(handoff, HANDOFF, label="live handoff")
    return contract, handoff


def discovery_view(contract: dict[str, object]) -> dict[str, object]:
    try:
        return {
            "historical_cutoff": contract["historical_cutoff"],
            "invariants": contract["invariants"],
            "trigger_surface": contract["trigger_surface"],
        }
    except KeyError as exc:
        fail("contract-shape", f"contract lacks discovery field {exc.args[0]!r}")


def path_metadata(contract: dict[str, object]) -> tuple[set[str], list[tuple[str, str]]]:
    trigger = contract.get("trigger_surface")
    if not isinstance(trigger, dict):
        fail("contract-shape", "trigger_surface must be an object")
    missing = set(TRIGGER_GROUPS) - set(trigger)
    unknown = set(trigger) - set(TRIGGER_GROUPS)
    if missing or unknown:
        fail(
            "contract-shape",
            f"trigger_surface keys mismatch; missing={sorted(missing)}, unknown={sorted(unknown)}",
        )
    exact: set[str] = set()
    try:
        for group in ("descriptors", "generated_builds", "generated_profiles"):
            exact.update(str(item["path"]).rstrip("/") for item in trigger[group])
        exact.update(str(item["path"]).rstrip("/") for item in trigger["readiness_markers"])
        root_claims = [
            (str(item["path"]).rstrip("/"), str(item["claim"]))
            for item in trigger["status_claim_roots"]
        ]
    except (KeyError, TypeError) as exc:
        fail("contract-shape", f"malformed trigger_surface record: {exc}")
    if "" in exact or any(not root or not claim for root, claim in root_claims):
        fail("contract-shape", "trigger_surface paths and claims must not be empty")
    return exact, root_claims


def validate_discovery(
    live: dict[str, object],
    pinned: dict[str, object],
    live_handoff: dict[str, object],
    pinned_handoff: dict[str, object],
) -> None:
    if discovery_view(live) != discovery_view(pinned):
        fail("discovery-drift", "HEAD discovery metadata differs from Slice 2 anchor")
    live_paths, live_roots = path_metadata(live)
    pinned_paths, pinned_roots = path_metadata(pinned)
    if live_paths != pinned_paths or live_roots != pinned_roots:
        fail("discovery-drift", "HEAD trigger paths or claim roots differ from Slice 2 anchor")
    live_handoff_without_path = {k: v for k, v in live_handoff.items() if k != "contract_file"}
    pinned_handoff_without_path = {k: v for k, v in pinned_handoff.items() if k != "contract_file"}
    if json.dumps(live_handoff_without_path, sort_keys=True) != json.dumps(
        pinned_handoff_without_path, sort_keys=True
    ):
        fail("discovery-drift", "HEAD handoff differs from Slice 2 anchor")


def source_or_exact_signal(path: str, exact_paths: set[str]) -> bool:
    return (
        path == "src/modules/git"
        or path.startswith("src/modules/git/")
        or path in exact_paths
    )


def parse_name_status(raw: bytes) -> list[tuple[str, tuple[str, ...]]]:
    fields = raw.split(b"\0")
    if fields and fields[-1] == b"":
        fields.pop()
    records: list[tuple[str, tuple[str, ...]]] = []
    index = 0
    while index < len(fields):
        try:
            status = fields[index].decode("ascii")
        except UnicodeDecodeError as exc:
            fail("name-status", f"non-ASCII status: {exc}")
        index += 1
        path_count = 2 if status.startswith(("R", "C")) else 1
        if index + path_count > len(fields):
            fail("name-status", f"truncated record for status {status!r}")
        # Git pathnames are arbitrary non-NUL bytes, not guaranteed UTF-8. Preserve
        # them losslessly so a historical filename cannot disable the ordering gate.
        paths = tuple(
            field.decode("utf-8", errors="surrogateescape")
            for field in fields[index : index + path_count]
        )
        index += path_count
        records.append((status, paths))
    return records


def under_root(path: str, root: str) -> bool:
    return path == root or path.startswith(root + "/")


def claim_patterns(claim: str, suffix: str) -> tuple[re.Pattern[str], ...]:
    escaped = re.escape(claim)
    if suffix in {".yaml", ".yml"}:
        return (
            re.compile(rf"^[ \t]*{escaped}[ \t]*:[ \t]*true(?:[ \t]+#.*)?[ \t]*$"),
        )
    if suffix == ".json":
        return (re.compile(rf'^[ \t]*"{escaped}"[ \t]*:[ \t]*true[ \t]*,?[ \t]*$'),)
    return ()


def blob(repo: Path, revision: str, path: str) -> bytes | None:
    result = git_run(repo, "show", f"{revision}:{path}")
    if result.returncode != 0:
        return None
    return result.stdout


def yaml_scalar_lines(lines: list[str]) -> set[int]:
    """Return line indexes that are content of a YAML block scalar."""
    scalar_lines: set[int] = set()
    scalar_indent: int | None = None
    opening = re.compile(r"^[ \t]*[^#][^:]*:[ \t]*[|>][+-]?[1-9]?[ \t]*(?:#.*)?$")
    for index, line in enumerate(lines):
        expanded = line.expandtabs(8)
        indent = len(expanded) - len(expanded.lstrip(" "))
        if scalar_indent is not None:
            if not line.strip() or indent > scalar_indent:
                scalar_lines.add(index)
                continue
            scalar_indent = None
        if opening.fullmatch(line):
            scalar_indent = indent
    return scalar_lines


def claim_signal_in_diff(before: bytes, after: bytes, claim: str, suffix: str) -> bool:
    """Detect new or changed-to-true claims in a structured UTF-8 blob."""
    try:
        old_lines = before.decode("utf-8").splitlines()
        new_lines = after.decode("utf-8").splitlines()
    except UnicodeDecodeError:
        return False
    patterns = claim_patterns(claim, suffix)
    scalar_lines = yaml_scalar_lines(new_lines) if suffix in {".yaml", ".yml"} else set()
    matcher = difflib.SequenceMatcher(a=old_lines, b=new_lines, autojunk=False)
    for tag, _old_start, _old_end, new_start, new_end in matcher.get_opcodes():
        if tag == "equal":
            continue
        for index in range(new_start, new_end):
            if index not in scalar_lines and any(
                pattern.fullmatch(new_lines[index]) for pattern in patterns
            ):
                return True
    return False


def commit_signals(
    repo: Path,
    parent: str,
    commit: str,
    *,
    exact_paths: set[str],
    root_claims: list[tuple[str, str]],
) -> list[tuple[str, str]]:
    raw = git(
        repo,
        "diff",
        "--name-status",
        "-z",
        "-M",
        "-C",
        "--find-copies-harder",
        parent,
        commit,
        "--",
    )
    return records_signals(
        repo,
        parent,
        commit,
        parse_name_status(raw),
        exact_paths=exact_paths,
        root_claims=root_claims,
    )


def records_signals(
    repo: Path,
    parent: str,
    commit: str,
    records: list[tuple[str, tuple[str, ...]]],
    *,
    exact_paths: set[str],
    root_claims: list[tuple[str, str]],
) -> list[tuple[str, str]]:
    """The signal rules, over name-status records from whichever git command."""
    signals: list[tuple[str, str]] = []
    for status, paths in records:
        if any(source_or_exact_signal(path, exact_paths) for path in paths):
            signals.append(("path", f"{status}:{' -> '.join(paths)}"))

        if status.startswith("D"):
            continue
        destination = paths[-1]
        suffix = Path(destination).suffix.lower()
        if suffix not in {".json", ".yaml", ".yml"}:
            continue
        after = blob(repo, commit, destination)
        if after is None:
            fail("git-blob", f"cannot read {commit}:{destination}")
        for root, claim in root_claims:
            if not under_root(destination, root):
                continue
            source = paths[0]
            source_in_root = under_root(source, root)
            before = None
            if not status.startswith("C") and source_in_root:
                before = blob(repo, parent, source)
            if claim_signal_in_diff(before or b"", after, claim, suffix):
                signals.append(("status-claim", f"{root}:{claim}:{destination}"))
    return signals


def first_parent(repo: Path, commit: str) -> str:
    line = git_text(repo, "rev-list", "--parents", "-n", "1", commit).split()
    if len(line) < 2:
        fail("git-history", f"commit {commit} has no first parent")
    return line[1]


def batched_history(repo: Path, cutoff: str, head: str) -> dict[str, tuple[str, list]]:
    """Every commit's first parent and first-parent diff, in one git process.

    Asking per commit costs two processes each, and the range is thousands of
    commits: ~2 minutes per run, times the several runs this gate performs. One
    `git log` answers all of it. `--diff-merges=first-parent` is exactly the
    `diff <first parent> <commit>` the per-commit form took, so a merge still
    reports what it brought to its target branch.

    The stream is `\\x1e<sha> <parents>\\0\\n` followed by the commit's -z
    name-status records, so the record separator cannot collide with a pathname
    (git forbids \\x1e in neither, but it cannot appear unescaped in the header
    the format emits, and paths are consumed positionally by parse_name_status).
    """
    raw = git(
        repo,
        "log",
        "-z",
        "--format=%x1e%H %P",
        "--name-status",
        "--diff-merges=first-parent",
        "-M",
        "-C",
        "--find-copies-harder",
        f"{cutoff}..{head}",
    )
    history: dict[str, tuple[str, list]] = {}
    for chunk in raw.split(b"\x1e")[1:]:
        header, separator, rest = chunk.partition(b"\0")
        if not separator:
            fail("git-history", "batched log record has no header terminator")
        try:
            fields = header.decode("ascii").split()
        except UnicodeDecodeError as exc:
            fail("git-history", f"non-ASCII commit header: {exc}")
        if not fields:
            fail("git-history", "batched log record has an empty header")
        commit, parents = fields[0], fields[1:]
        # Strip exactly the newline the format line ends with. lstrip would eat a
        # pathname that legitimately begins with one.
        body = rest[1:] if rest.startswith(b"\n") else rest
        history[commit] = (parents[0] if parents else "", parse_name_status(body))
    return history


def scan_history(
    repo: Path,
    cutoff: str,
    head: str,
    *,
    exact_paths: set[str],
    root_claims: list[tuple[str, str]],
) -> list[tuple[str, str, list[tuple[str, str]]]]:
    commits_text = git_text(repo, "rev-list", "--topo-order", "--reverse", f"{cutoff}..{head}")
    commits = commits_text.splitlines() if commits_text else []
    history = batched_history(repo, cutoff, head)
    found: list[tuple[str, str, list[tuple[str, str]]]] = []
    for commit in commits:
        parent, records = history.get(commit, (None, None))
        if records is None:
            # A commit rev-list yielded but the batched log did not describe is a
            # disagreement between two git commands, not something to skip past.
            fail("git-history", f"commit {commit} is missing from the batched log")
        if not parent:
            parent = first_parent(repo, commit)
        signals = records_signals(
            repo,
            parent,
            commit,
            records,
            exact_paths=exact_paths,
            root_claims=root_claims,
        )
        if signals:
            found.append((commit, parent, signals))
    return found


def is_ancestor(repo: Path, ancestor: str, descendant: str) -> bool:
    result = git_run(repo, "merge-base", "--is-ancestor", ancestor, descendant)
    if result.returncode not in (0, 1):
        fail("git-ancestry", f"cannot compare {ancestor} to {descendant}")
    return result.returncode == 0


def enforce_waiver_is_live(
    signals: list[tuple[str, str, list[tuple[str, str]]]]
) -> None:
    """Every waived commit must still be a signal, so the set only shrinks.

    Scoped to the real scan rather than to enforce_signal_precedence, which is a
    pure function over whatever signals it is handed and is exercised against
    synthetic repositories that contain none of these commits.
    """
    stale = sorted(PRE_APPROVAL_SIGNALS - {commit for commit, _, _ in signals})
    if stale:
        fail(
            "pre-approval-waiver",
            f"waived commits are no longer Git-contract signals: {stale}; delete them",
        )


def descends_from(repo: Path, ancestor: str, descendant: str) -> bool:
    """Ancestry, treating an unresolvable ref as 'does not descend'.

    `is_ancestor` fails closed on a ref git cannot resolve, which is right for
    the anchor checks in validate_ordering -- it runs `cat-file -e` on the
    anchor first, so an unresolvable ref there is a real misconfiguration. Here
    the answer wanted is about this one signal, so an unknown ref is simply not
    an ancestor and the caller reports the ordering violation with its evidence.
    """
    result = git_run(repo, "merge-base", "--is-ancestor", ancestor, descendant)
    return result.returncode == 0


def anchor_descendants(repo: Path, anchor: str) -> set[str]:
    """Every commit between the anchor and HEAD, in one command.

    Asking `merge-base --is-ancestor` per signal is one process per commit, and
    this job runs the checker four times over a hundred-plus signals: enough to
    exceed the job's five-minute budget on its own. `rev-list --ancestry-path`
    answers the same question once. The range excludes the anchor, so a parent
    equal to it is correctly absent.
    """
    head = git_run(repo, "rev-parse", "HEAD")
    if head.returncode != 0:
        return set()
    # git_run yields bytes; interpolating them raw produces a b'...' rev range
    # that git rejects, and an empty set silently fails every signal at once.
    revision = f"{anchor}..{head.stdout.decode('ascii', 'replace').strip()}"
    result = git_run(repo, "rev-list", "--ancestry-path", revision)
    if result.returncode != 0:
        return set()
    return set(result.stdout.decode("ascii", "replace").split())


def on_first_parent_chain(repo: Path, ancestor: str, descendant: str) -> bool:
    current = descendant
    while True:
        if current == ancestor:
            return True
        line = git_text(repo, "rev-list", "--parents", "-n", "1", current).split()
        if len(line) < 2:
            return False
        current = line[1]


def enforce_signal_precedence(
    repo: Path, anchor: str, signals: list[tuple[str, str, list[tuple[str, str]]]]
) -> None:
    """Every Git-contract signal must build on the approved Slice 2 contract.

    "Builds on" is ancestry, not first-parent-chain membership. Requiring the
    latter is a claim about *how* work merges, not about what it follows: under
    a merge-based pull-request flow a topic commit's first parent is a topic
    commit, which is never on the integration branch's first-parent line. That
    reading rejected 74 of the 95 signals in this repository -- every ordinary
    post-approval Git-module change, including the merges that delivered them.

    Ancestry keeps the property the gate exists for. A signal whose parent
    descends from the anchor demonstrably came after the approved contract; one
    whose parent does not, did not. `parent == anchor` still fails, so approval
    and the first change it authorizes stay distinct commits.
    """
    descendants = anchor_descendants(repo, anchor)
    for commit, parent, evidence in signals:
        if commit in PRE_APPROVAL_SIGNALS:
            continue
        if parent == anchor or parent not in descendants:
            rendered = ", ".join(f"{kind}:{value}" for kind, value in evidence)
            fail(
                "git-contract-ordering",
                f"signal at {commit} does not follow approved Slice 2 contract: {rendered}",
            )


def is_gated_base(name: str) -> bool:
    """Whether this branch is one the module gates run against."""
    return name in GATED_BASES or any(
        name.startswith(prefix) and len(name) > len(prefix)
        for prefix in GATED_BASE_PREFIXES
    )


def validate_event(head: str) -> None:
    keys = (
        "GITHUB_EVENT_NAME",
        "GITHUB_REF",
        "GITHUB_SHA",
        "GITHUB_BASE_REF",
        "GITHUB_HEAD_REF",
    )
    context = {key: os.environ.get(key, "") for key in keys}
    if not any(context.values()):
        return
    event = context["GITHUB_EVENT_NAME"]
    ref = context["GITHUB_REF"]
    event_sha = context["GITHUB_SHA"]
    if not event:
        fail("event-name", "GITHUB_EVENT_NAME is required when GitHub context is present")
    if not ref:
        fail("event-ref", "GITHUB_REF is required when GitHub context is present")
    if not event_sha or event_sha != head:
        fail("event-head", f"GITHUB_SHA {event_sha!r} differs from checked-out HEAD {head}")
    if event == "pull_request":
        if not re.fullmatch(r"refs/pull/[0-9]+/merge", ref):
            fail("event-ref", f"unsupported pull_request ref {ref!r}")
        if not is_gated_base(context["GITHUB_BASE_REF"]):
            fail("event-base", f"pull request must target a gated base, not {context['GITHUB_BASE_REF']!r}")
        if not context["GITHUB_HEAD_REF"]:
            fail("event-head-ref", "GITHUB_HEAD_REF is required for pull requests")
        return
    if event in {"push", "workflow_dispatch"} and ref.startswith("refs/heads/"):
        if is_gated_base(ref[len("refs/heads/"):]):
            return
    fail("event-ref", f"unsupported GitHub event/ref pair {event!r}/{ref!r}")


def require_clean_metadata(repo: Path) -> None:
    for args in (
        ("diff", "--quiet", "--", CONTRACT_PATH, HANDOFF_PATH),
        ("diff", "--cached", "--quiet", "--", CONTRACT_PATH, HANDOFF_PATH),
    ):
        result = git_run(repo, *args)
        if result.returncode == 1:
            fail("live-contract-dirty", "contract or handoff has uncommitted changes")
        if result.returncode != 0:
            fail("git-command", f"git {' '.join(args)} failed ({result.returncode})")


def validate_ordering(repo: Path) -> int:
    require_repository(repo)
    head = git_text(repo, "rev-parse", "HEAD")
    validate_event(head)
    git(repo, "cat-file", "-e", f"{SLICE2_ANCHOR}^{{commit}}")
    if not is_ancestor(repo, SLICE2_ANCHOR, head):
        fail("slice2-anchor", "approved Slice 2 anchor is not an ancestor of HEAD")

    require_clean_metadata(repo)
    validate_trusted_contract(repo)
    pinned, pinned_handoff = canonical_metadata(repo)
    live, live_handoff = live_metadata(repo)
    validate_discovery(live, pinned, live_handoff, pinned_handoff)

    try:
        cutoff = str(pinned["historical_cutoff"]["commit"])
    except (KeyError, TypeError) as exc:
        fail("contract-shape", f"malformed historical cutoff: {exc}")
    if not is_ancestor(repo, cutoff, SLICE2_ANCHOR):
        fail("slice2-anchor", "historical cutoff does not precede the Slice 2 anchor")
    exact_paths, root_claims = path_metadata(pinned)
    signals = scan_history(
        repo,
        cutoff,
        head,
        exact_paths=exact_paths,
        root_claims=root_claims,
    )
    enforce_signal_precedence(repo, SLICE2_ANCHOR, signals)
    enforce_waiver_is_live(signals)
    return len(signals)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config-root", type=Path)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    root_value = args.config_root or os.environ.get("AIMEE_CONFIG_ROOT") or REPO_ROOT
    repo = Path(os.path.realpath(root_value))
    if not repo.is_dir():
        print(
            f"check_proposal_ordering: error: rule=config-root: {repo} is not a directory",
            file=sys.stderr,
        )
        return 1
    try:
        signals_after_anchor = validate_ordering(repo)
    except (OrderingError, OSError, subprocess.SubprocessError, UnicodeError, ValueError) as exc:
        print(f"check_proposal_ordering: error: {exc}", file=sys.stderr)
        return 1
    print(
        "check_proposal_ordering: ok "
        f"({signals_after_anchor} post-cutoff signal commit(s); {repo})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
