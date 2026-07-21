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
CONTRACT_PATH = "docs/proposals/pending/git-core-contract.md"
EVIDENCE_PATH = "docs/validation/roundtable/git-core-contract.json"
HANDOFF_PATH = "docs/validation/core-modularization-slice-2.md"
CHECKER_PATH = "scripts/check_git_core_contract.py"
FEATURE_BRANCH = "feature/core-modularization"
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


def validate_handoff(value: dict[str, object], *, label: str) -> None:
    if value != HANDOFF:
        fail("handoff-shape", f"{label} differs from the exact Slice 2 handoff")


def validate_trusted_contract(repo: Path) -> None:
    checker = git(repo, "show", f"{SLICE2_ANCHOR}:{CHECKER_PATH}")
    with tempfile.TemporaryDirectory(prefix="aimee-ordering-") as directory:
        checker_path = Path(directory) / "check_git_core_contract.py"
        checker_path.write_bytes(checker)
        checker_path.chmod(0o400)
        try:
            result = subprocess.run(
                [
                    PYTHON,
                    "-I",
                    "-S",
                    str(checker_path),
                    "--config-root",
                    str(repo),
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
        anchor_text(repo, CONTRACT_PATH, "anchor-contract"),
        "git-core-contract",
        label=f"{SLICE2_ANCHOR}:{CONTRACT_PATH}",
    )
    evidence = git_run(repo, "cat-file", "-e", f"{SLICE2_ANCHOR}:{EVIDENCE_PATH}")
    if evidence.returncode != 0:
        fail("anchor-evidence", f"{EVIDENCE_PATH} is absent from the Slice 2 anchor")
    handoff = extract_json_fence_text(
        anchor_text(repo, HANDOFF_PATH, "anchor-handoff"),
        "slice3-handoff",
        label=f"{SLICE2_ANCHOR}:{HANDOFF_PATH}",
    )
    validate_handoff(handoff, label="pinned handoff")
    return contract, handoff


def live_metadata(repo: Path) -> tuple[dict[str, object], dict[str, object]]:
    contract = extract_json_fence_text(
        read_live_text(repo, CONTRACT_PATH), "git-core-contract", label=CONTRACT_PATH
    )
    handoff = extract_json_fence_text(
        read_live_text(repo, HANDOFF_PATH), "slice3-handoff", label=HANDOFF_PATH
    )
    validate_handoff(handoff, label="live handoff")
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
    if json.dumps(live_handoff, sort_keys=True) != json.dumps(pinned_handoff, sort_keys=True):
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
        try:
            paths = tuple(field.decode("utf-8") for field in fields[index : index + path_count])
        except UnicodeDecodeError as exc:
            fail("name-status", f"non-UTF-8 path: {exc}")
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
    records = parse_name_status(raw)
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
    found: list[tuple[str, str, list[tuple[str, str]]]] = []
    for commit in commits:
        parent = first_parent(repo, commit)
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
    result = git_run(repo, "merge-base", "--is-ancestor", ancestor, descendant)
    if result.returncode not in (0, 1):
        fail("git-ancestry", f"cannot compare {ancestor} to {descendant}")
    return result.returncode == 0


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
    for commit, parent, evidence in signals:
        if parent == anchor or not on_first_parent_chain(repo, anchor, parent):
            rendered = ", ".join(f"{kind}:{value}" for kind, value in evidence)
            fail(
                "git-contract-ordering",
                f"signal at {commit} does not follow approved Slice 2 contract: {rendered}",
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
        if context["GITHUB_BASE_REF"] != FEATURE_BRANCH:
            fail("event-base", f"pull request must target {FEATURE_BRANCH}")
        if not context["GITHUB_HEAD_REF"]:
            fail("event-head-ref", "GITHUB_HEAD_REF is required for pull requests")
        return
    if event in {"push", "workflow_dispatch"} and ref == f"refs/heads/{FEATURE_BRANCH}":
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
