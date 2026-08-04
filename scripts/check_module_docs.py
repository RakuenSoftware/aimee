#!/usr/bin/env python3
"""Validate the descriptor-driven individual module documentation catalog."""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_MODULES = Path("src/modules")
DEFAULT_DOCS = Path("docs/modules")
DEFAULT_STATUS = Path("tests/baselines/modules/documentation-status.yaml")
MODULE_ID = re.compile(r"^[a-z][a-z0-9]*(?:-[a-z0-9]+)*$")
SECTIONS = (
    "Purpose and non-goals",
    "Public contracts",
    "Dependencies and consumers",
    "Providers and readiness",
    "Configuration and activation",
    "Surfaces",
    "Data and migrations",
    "Security and privacy",
    "Supported journeys",
    "Tests and failure behavior",
    "Operational diagnostics",
    "Compatibility",
    "Extension and removal",
)
PLACEHOLDER = re.compile(
    r"\b(?:TODO|TBD|PLACEHOLDER|pending|coming soon|not (?:yet )?documented|see module owner)\b",
    re.IGNORECASE,
)


class DocError(ValueError):
    """A closed module-documentation contract failure."""


def _unique_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise DocError(f"duplicate object key {key!r}")
        result[key] = value
    return result


def _load_json(path: Path) -> object:
    try:
        return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_unique_object)
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise DocError(f"cannot load {path}: {exc}") from exc


def _string_list(value: object, field: str) -> list[str]:
    if not isinstance(value, list) or not all(isinstance(item, str) for item in value):
        raise DocError(f"{field} must be an array of strings")
    if len(value) != len(set(value)):
        raise DocError(f"{field} contains duplicates")
    return value


def _safe_path(root: Path, path: Path, label: str) -> Path:
    absolute = Path(os.path.abspath(path))
    try:
        relative = absolute.relative_to(root)
    except ValueError as exc:
        raise DocError(f"{label} escapes config root: {absolute}") from exc
    current = root
    for part in relative.parts:
        current /= part
        if current.is_symlink():
            raise DocError(f"{label} contains symlink component: {current}")
    return absolute


def _reject_symlink_components(path: Path, label: str) -> None:
    current = Path(path.anchor)
    for part in path.parts[1:]:
        current /= part
        if current.is_symlink():
            raise DocError(f"{label} contains symlink component: {current}")


def load_descriptors(root: Path, modules_dir: Path) -> dict[str, dict[str, object]]:
    descriptors: dict[str, dict[str, object]] = {}
    for path in sorted(modules_dir.glob("*/module.yaml")):
        _safe_path(root, path, "module descriptor")
        raw = _load_json(path)
        if not isinstance(raw, dict):
            raise DocError(f"{path}: descriptor must be an object")
        module_id = raw.get("id")
        if not isinstance(module_id, str) or not MODULE_ID.fullmatch(module_id):
            raise DocError(f"{path}: invalid or missing module id")
        if module_id in descriptors:
            raise DocError(f"duplicate module id {module_id!r}")
        expected_dir = path.parent.name
        if module_id != expected_dir:
            raise DocError(f"{path}: id {module_id!r} must match directory {expected_dir!r}")
        dependencies = _string_list(raw.get("dependencies"), f"{path}: dependencies")
        toggle = raw.get("runtime_toggle")
        if (
            not isinstance(toggle, dict)
            or not toggle
            or not all(isinstance(key, str) and type(value) is bool for key, value in toggle.items())
        ):
            raise DocError(f"{path}: runtime_toggle must map string keys to booleans")
        raw["dependencies"] = dependencies
        descriptors[module_id] = raw
    if not descriptors:
        raise DocError(f"no descriptors found under {modules_dir.relative_to(root)}")
    return descriptors


def load_status(path: Path, descriptor_ids: set[str]) -> tuple[set[str], set[str]]:
    raw = _load_json(path)
    if not isinstance(raw, dict) or set(raw) != {"schema_version", "substantive", "debt"}:
        raise DocError(f"{path}: status keys must be schema_version, substantive, debt")
    if type(raw["schema_version"]) is not int or raw["schema_version"] != 1:
        raise DocError(f"{path}: schema_version must be integer 1")
    substantive = set(_string_list(raw["substantive"], f"{path}: substantive"))
    debt = set(_string_list(raw["debt"], f"{path}: debt"))
    if substantive & debt:
        raise DocError(f"{path}: substantive and debt sets overlap")
    if substantive | debt != descriptor_ids:
        missing = sorted(descriptor_ids - substantive - debt)
        unknown = sorted((substantive | debt) - descriptor_ids)
        raise DocError(f"{path}: status partition differs: missing={missing}, unknown={unknown}")
    return substantive, debt


def _sections(path: Path, module_id: str) -> dict[str, str]:
    text = path.read_text(encoding="utf-8")
    first = next((line for line in text.splitlines() if line.strip()), "")
    if first != f"# {module_id} module":
        raise DocError(f"{path}: first non-blank line must be '# {module_id} module'")
    headings = [(match.group(1), match.start(), match.end()) for match in re.finditer(r"^## (.+)$", text, re.MULTILINE)]
    names = [name for name, _, _ in headings]
    if names != list(SECTIONS):
        raise DocError(f"{path}: required H2 sections differ or are out of order: {names}")
    result = {}
    for index, (name, _, end) in enumerate(headings):
        next_start = headings[index + 1][1] if index + 1 < len(headings) else len(text)
        body = text[end:next_start].strip()
        compact = "".join(body.split())
        if len(compact) < 80 or PLACEHOLDER.search(body) or "`" not in body:
            raise DocError(f"{path}: section {name!r} lacks substantive grounded content")
        result[name] = body
    return result


def _bullet_ids(body: str) -> list[str]:
    found = []
    for line in body.splitlines():
        match = re.match(r"^- `([^`]+)`(?:\s|:|$)", line)
        if match:
            found.append(match.group(1))
    return found


def validate_doc(path: Path, descriptor: dict[str, object]) -> None:
    module_id = descriptor["id"]
    sections = _sections(path, module_id)
    expected_deps = set(descriptor["dependencies"])
    dependency_bullets = _bullet_ids(sections["Dependencies and consumers"])
    if len(dependency_bullets) != len(set(dependency_bullets)):
        raise DocError(f"{path}: duplicate dependency bullet")
    actual_deps = set(dependency_bullets)
    if actual_deps != expected_deps:
        raise DocError(
            f"{path}: dependency bullets differ: missing={sorted(expected_deps - actual_deps)}, "
            f"undeclared={sorted(actual_deps - expected_deps)}"
        )
    toggle_body = sections["Configuration and activation"]
    actual_toggles: dict[str, bool] = {}
    toggle_bullets = re.findall(
        r"^- `runtime_toggle\.([^`]+)`(.*)$", toggle_body, re.MULTILINE
    )
    toggle_keys = [key for key, _ in toggle_bullets]
    duplicate_key = next((key for key in toggle_keys if toggle_keys.count(key) > 1), None)
    if duplicate_key is not None:
        raise DocError(f"{path}: duplicate runtime-toggle bullet {duplicate_key!r}")
    for key, remainder in toggle_bullets:
        claim_match = re.match(r"^: (.+)$", remainder)
        if not claim_match:
            raise DocError(f"{path}: malformed runtime-toggle value for {key!r}")
        claim = claim_match.group(1)
        value_match = re.match(r"`?(true|false)`?(?:[;.,:]|\s|$)", claim)
        if not value_match:
            raise DocError(f"{path}: malformed runtime-toggle value for {key!r}")
        value = value_match.group(1)
        actual_toggles[key] = value == "true"
    expected_toggles = descriptor["runtime_toggle"]
    if actual_toggles != expected_toggles:
        raise DocError(
            f"{path}: runtime-toggle bullets differ: expected={expected_toggles}, actual={actual_toggles}"
        )


def run(root: Path, modules: Path, docs: Path, status_path: Path) -> str:
    root = Path(os.path.abspath(root))
    _reject_symlink_components(root, "config root")
    modules = _safe_path(root, modules, "modules path")
    docs = _safe_path(root, docs, "docs path")
    status_path = _safe_path(root, status_path, "status path")
    descriptors = load_descriptors(root, modules)
    known_ids = set(descriptors)
    substantive, debt = load_status(status_path, known_ids)
    doc_paths = sorted(docs.rglob("*.md"))
    for path in doc_paths:
        _safe_path(root, path, "module document")
        if path == docs / "README.md":
            continue
        if path.parent != docs or path.stem not in known_ids:
            raise DocError(f"{path}: orphan or non-canonical module document")
    lines = []
    for module_id in sorted(known_ids):
        path = docs / f"{module_id}.md"
        if module_id in debt:
            if path.exists():
                raise DocError(f"{path}: documented module remains in debt status")
            lines.append(f"DEBT {module_id}")
            continue
        if not path.is_file():
            raise DocError(f"{path}: substantive module document is missing")
        validate_doc(path, descriptors[module_id])
        lines.append(f"PASS {module_id}")
    lines.append("SUMMARY debt=" + ",".join(sorted(debt)))
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config-root", type=Path, default=ROOT)
    parser.add_argument("--modules", type=Path, default=DEFAULT_MODULES)
    parser.add_argument("--docs", type=Path, default=DEFAULT_DOCS)
    parser.add_argument("--status", type=Path, default=DEFAULT_STATUS)
    args = parser.parse_args(sys.argv[1:] if argv is None else argv)
    root = Path(os.path.abspath(args.config_root))
    resolve = lambda value: value if value.is_absolute() else root / value
    try:
        report = run(root, resolve(args.modules), resolve(args.docs), resolve(args.status))
    except (DocError, OSError, UnicodeError) as exc:
        print(f"check_module_docs: error: {exc}", file=sys.stderr)
        return 1
    sys.stdout.write(report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
