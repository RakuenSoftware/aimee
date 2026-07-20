#!/usr/bin/env python3
"""Require the approved Git child contract to precede every Git migration signal."""

from __future__ import annotations

import argparse
import importlib.util
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from typing import NoReturn


REPO_ROOT = Path(__file__).resolve().parent.parent
CONTRACT_CHECKER = REPO_ROOT / "scripts/check_git_core_contract.py"
SLICE2_ANCHOR = "a3c4d413b6ce5f674994a6e6c4589ae2383819a4"
CONTRACT_PATH = "docs/proposals/pending/git-core-contract.md"
EVIDENCE_PATH = "docs/validation/roundtable/git-core-contract.json"
HANDOFF_PATH = "docs/validation/core-modularization-slice-2.md"
GIT = shutil.which("git", path="/usr/bin:/bin") or "/usr/bin/git"


class OrderingError(ValueError):
    """A fail-closed ordering error with a stable rule name."""


def fail(rule: str, message: str) -> NoReturn:
    raise OrderingError(f"rule={rule}: {message}")


def load_contract_checker():
    expected = REPO_ROOT / "scripts/check_git_core_contract.py"
    try:
        resolved = CONTRACT_CHECKER.resolve(strict=True)
    except OSError as exc:
        fail("checker-load", f"cannot resolve contract checker: {exc}")
    if CONTRACT_CHECKER.is_symlink() or resolved != expected:
        fail(
            "checker-load",
            f"contract checker path is not the trusted repository file: {resolved}",
        )
    spec = importlib.util.spec_from_file_location("check_git_core_contract", CONTRACT_CHECKER)
    if spec is None or spec.loader is None:
        fail("checker-load", f"cannot load {CONTRACT_CHECKER}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def git(repo: Path, *args: str, check: bool = True) -> bytes:
    try:
        result = subprocess.run(
            [GIT, *args],
            cwd=repo,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    except OSError as exc:
        fail("git-exec", f"cannot execute {GIT}: {exc}")
    if check and result.returncode != 0:
        stderr = result.stderr.decode("utf-8", "replace").strip()
        fail("git-command", f"git {' '.join(args)} failed ({result.returncode}): {stderr}")
    return result.stdout


def git_text(repo: Path, *args: str) -> str:
    try:
        return git(repo, *args).decode("utf-8").strip()
    except UnicodeDecodeError as exc:
        fail("git-output", f"git {' '.join(args)} returned invalid UTF-8: {exc}")


def canonical_metadata(repo: Path, checker) -> tuple[dict[str, object], dict[str, object]]:
    for path in (CONTRACT_PATH, EVIDENCE_PATH, HANDOFF_PATH):
        git(repo, "cat-file", "-e", f"{SLICE2_ANCHOR}:{path}")
    raw = git(repo, "show", f"{SLICE2_ANCHOR}:{CONTRACT_PATH}")
    handoff_raw = git(repo, "show", f"{SLICE2_ANCHOR}:{HANDOFF_PATH}")
    try:
        text = raw.decode("utf-8")
        handoff_text = handoff_raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        fail("anchor-contract", f"pinned metadata is not UTF-8: {exc}")
    contract = checker.extract_json_fence_text(text, "git-core-contract", path=CONTRACT_PATH)
    handoff = checker.extract_json_fence_text(
        handoff_text, "slice3-handoff", path=HANDOFF_PATH
    )
    return contract, handoff


def discovery_view(contract: dict[str, object]) -> dict[str, object]:
    return {
        "historical_cutoff": contract["historical_cutoff"],
        "invariants": contract["invariants"],
        "trigger_surface": contract["trigger_surface"],
    }


def validate_discovery(
    live: dict[str, object],
    pinned: dict[str, object],
    live_handoff: dict[str, object],
    pinned_handoff: dict[str, object],
) -> None:
    if discovery_view(live) != discovery_view(pinned):
        fail("discovery-drift", "HEAD contract discovery metadata differs from Slice 2 anchor")
    if live_handoff != pinned_handoff:
        fail("discovery-drift", "HEAD handoff differs from Slice 2 anchor")


def path_metadata(
    contract: dict[str, object],
) -> tuple[set[str], list[tuple[str, str]]]:
    trigger = contract["trigger_surface"]
    exact: set[str] = set()
    for group in ("descriptors", "generated_builds", "generated_profiles"):
        exact.update(item["path"] for item in trigger[group])
    exact.update(item["path"] for item in trigger["readiness_markers"])
    root_claims = [
        (item["path"].rstrip("/"), item["claim"])
        for item in trigger["status_claim_roots"]
    ]
    return exact, root_claims


def source_or_exact_signal(path: str, exact_paths: set[str]) -> bool:
    return path == "src/modules/git" or path.startswith("src/modules/git/") or path in exact_paths


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
        paths: list[str] = []
        for field in fields[index : index + path_count]:
            try:
                paths.append(field.decode("utf-8"))
            except UnicodeDecodeError as exc:
                fail("name-status", f"non-UTF-8 path: {exc}")
        index += path_count
        records.append((status, tuple(paths)))
    return records


def claim_patterns(claims: set[str]) -> tuple[re.Pattern[str], ...]:
    patterns: list[re.Pattern[str]] = []
    for claim in sorted(claims):
        escaped = re.escape(claim)
        patterns.append(re.compile(rf"^[ \t]*{escaped}[ \t]*:[ \t]*true(?:[ \t]+#.*)?[ \t]*$"))
        patterns.append(re.compile(rf'^[ \t]*"{escaped}"[ \t]*:[ \t]*true[ \t]*,?[ \t]*$'))
    return tuple(patterns)


def added_claim_signal(diff: bytes, patterns: tuple[re.Pattern[str], ...]) -> bool:
    try:
        text = diff.decode("utf-8")
    except UnicodeDecodeError:
        return False
    structured_file = False
    for line in text.splitlines():
        if line.startswith("+++ "):
            changed_path = line[4:]
            if changed_path.startswith("b/"):
                changed_path = changed_path[2:]
            structured_file = changed_path.endswith((".json", ".yaml", ".yml"))
            continue
        if not structured_file:
            continue
        if not line.startswith("+") or line.startswith("+++"):
            continue
        candidate = line[1:]
        if any(pattern.fullmatch(candidate) for pattern in patterns):
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
    signals: list[tuple[str, str]] = []
    for status, paths in parse_name_status(raw):
        for path in paths:
            if source_or_exact_signal(path, exact_paths):
                signals.append(("path", f"{status}:{path}"))
                break
    for root, claim in root_claims:
        claim_diff = git(
            repo,
            "diff",
            "--unified=0",
            "--no-color",
            "--no-ext-diff",
            parent,
            commit,
            "--",
            root,
        )
        if added_claim_signal(claim_diff, claim_patterns({claim})):
            signals.append(("status-claim", f"{root}:{claim}"))
    return signals


def scan_history(
    repo: Path,
    cutoff: str,
    head: str,
    *,
    exact_paths: set[str],
    root_claims: list[tuple[str, str]],
) -> list[tuple[str, str, list[tuple[str, str]]]]:
    commits_text = git_text(
        repo, "rev-list", "--first-parent", "--reverse", f"{cutoff}..{head}"
    )
    commits = commits_text.splitlines() if commits_text else []
    found: list[tuple[str, str, list[tuple[str, str]]]] = []
    for commit in commits:
        parent = git_text(repo, "rev-parse", f"{commit}^1")
        signals = commit_signals(
            repo,
            parent,
            commit,
            exact_paths=exact_paths,
            root_claims=root_claims,
        )
        if signals:
            found.append((commit, parent, signals))
    return found


def is_ancestor(repo: Path, ancestor: str, descendant: str) -> bool:
    try:
        result = subprocess.run(
            [GIT, "merge-base", "--is-ancestor", ancestor, descendant],
            cwd=repo,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
    except OSError as exc:
        fail("git-exec", f"cannot execute {GIT}: {exc}")
    if result.returncode not in (0, 1):
        fail("git-ancestry", f"cannot compare {ancestor} to {descendant}")
    return result.returncode == 0


def enforce_signal_precedence(
    repo: Path, anchor: str, signals: list[tuple[str, str, list[tuple[str, str]]]]
) -> None:
    for commit, parent, evidence in signals:
        if not is_ancestor(repo, anchor, parent):
            fail(
                "git-contract-ordering",
                f"signal at {commit} does not follow approved Slice 2 contract: "
                f"{evidence[0][0]}:{evidence[0][1]}",
            )


def validate_event(head: str) -> None:
    event = os.environ.get("GITHUB_EVENT_NAME")
    ref = os.environ.get("GITHUB_REF")
    event_sha = os.environ.get("GITHUB_SHA")
    if not event:
        return
    if not event_sha:
        fail("event-head", "GITHUB_SHA is required for GitHub events")
    if not ref:
        fail("event-ref", "GITHUB_REF is required for GitHub events")
    if event_sha != head:
        fail("event-head", f"GITHUB_SHA {event_sha} differs from checked-out HEAD {head}")
    allowed = (
        event == "push" and ref == "refs/heads/feature/core-modularization"
    ) or (
        event == "pull_request" and bool(ref and re.fullmatch(r"refs/pull/[0-9]+/merge", ref))
    ) or (
        event == "workflow_dispatch" and ref == "refs/heads/feature/core-modularization"
    )
    if not allowed:
        fail("event-ref", f"unsupported GitHub event/ref pair {event!r}/{ref!r}")


def validate_ordering(repo: Path) -> int:
    checker = load_contract_checker()
    head = git_text(repo, "rev-parse", "HEAD")
    validate_event(head)
    git(repo, "cat-file", "-e", f"{SLICE2_ANCHOR}^{{commit}}")
    if not is_ancestor(repo, SLICE2_ANCHOR, head):
        fail("slice2-anchor", "approved Slice 2 anchor is not an ancestor of HEAD")

    pinned, pinned_handoff = canonical_metadata(repo, checker)
    live = checker.load_validated_contract(
        repo, require_status="roundtable-approved", check_git=True
    )
    live_handoff_path = repo / HANDOFF_PATH
    live_handoff = checker.extract_json_fence(live_handoff_path, "slice3-handoff")
    validate_discovery(live, pinned, live_handoff, pinned_handoff)

    cutoff = pinned["historical_cutoff"]["commit"]
    exact_paths, root_claims = path_metadata(pinned)
    signals = scan_history(
        repo,
        cutoff,
        head,
        exact_paths=exact_paths,
        root_claims=root_claims,
    )
    enforce_signal_precedence(repo, SLICE2_ANCHOR, signals)
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
        signal_count = validate_ordering(repo)
    except (OrderingError, OSError, subprocess.SubprocessError, UnicodeError, ValueError) as exc:
        print(f"check_proposal_ordering: error: {exc}", file=sys.stderr)
        return 1
    print(f"check_proposal_ordering: ok ({signal_count} post-cutoff signal commit(s); {repo})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
