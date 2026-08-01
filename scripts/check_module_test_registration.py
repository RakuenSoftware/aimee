#!/usr/bin/env python3
"""Report which build systems register each descriptor-declared module test.

Slices 35 and 36 recorded, in prose, that CMake registered no test target for
module-runtime and plugin-loader. That was wrong: the audit read only the
top-level CMakeLists.txt and missed src/tests/CMakeLists.txt, which
add_subdirectory() pulls in and which registers most CTest cases. Prose alone
cannot keep that straight, so registration is derived from the build files
themselves and pinned to a baseline: any change to what Make and CTest
statically register for a declared test fails until the baseline is regenerated
and re-reviewed.

Registration is bound to the declared source *path*, not to a matching target
name. A CTest case counts only when the executable named by its COMMAND is built
from the descriptor-declared file, so re-pointing a registered target at another
source — even one with the same basename in a different directory — is drift.

This reports registration, not execution. It says what the build files declare,
not that any suite ran the test, and it does not read documentation, so prose can
still describe an unchanged baseline incorrectly.
"""

from __future__ import annotations

import argparse
import json
import posixpath
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
MODULES = Path("src/modules")
TESTS_DIR = "src/tests"
CMAKE_TESTS = Path(f"{TESTS_DIR}/CMakeLists.txt")
MAKE_RULES = Path(f"{TESTS_DIR}/Rules.mk")
BASELINE = Path("tests/baselines/refactor/module-test-registration.json")

# aimee_add_test(<name> <sources...>) wraps add_executable + add_test; the
# hand-written form is add_executable(<name> <sources...>) plus
# add_test(NAME <case> COMMAND <target> ...).
CALL = re.compile(r"\b(aimee_add_test|add_executable|add_test)\s*\(")
# CMake bracket comments (#[[ ... ]], #[=[ ... ]=]) span lines, so strip them
# before ordinary line comments — otherwise their contents tokenize as arguments.
BRACKET_COMMENT = re.compile(r"#\[(=*)\[.*?\]\1\]", re.DOTALL)
LINE_COMMENT = re.compile(r"#[^\n]*")


class RegistrationError(ValueError):
    """A closed, operator-readable registration validation failure."""


def _read(root: Path, relative: Path) -> str:
    path = root / relative
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        raise RegistrationError(f"cannot read {relative.as_posix()}: {exc}") from exc


def _calls(text: str) -> list[tuple[str, list[str]]]:
    """(command, argument tokens) per recognized call, comments stripped."""
    text = LINE_COMMENT.sub("", BRACKET_COMMENT.sub("", text))
    found: list[tuple[str, list[str]]] = []
    for match in CALL.finditer(text):
        depth, index = 1, match.end()
        while index < len(text) and depth:
            if text[index] == "(":
                depth += 1
            elif text[index] == ")":
                depth -= 1
            index += 1
        if depth:
            raise RegistrationError(
                f"unbalanced {match.group(1)}( at offset {match.start()} in "
                f"{CMAKE_TESTS.as_posix()}"
            )
        found.append((match.group(1), text[match.end() : index - 1].split()))
    return found


def _test_relative(token: str) -> str:
    """Normalize a source token, written relative to src/tests, to a repo path."""
    return posixpath.normpath(posixpath.join(TESTS_DIR, token))


def ctest_sources(root: Path) -> set[str]:
    """Repo-relative sources built by an executable that a CTest case runs.

    Keyed by source path rather than target name, so re-pointing a registered
    target at a different file is visible as drift.
    """
    target_sources: dict[str, set[str]] = {}
    registered_targets: set[str] = set()
    for command, args in _calls(_read(root, CMAKE_TESTS)):
        if not args:
            continue
        if command == "add_test":
            # add_test(NAME <case> COMMAND <target> ...) — bind to the target the
            # case actually runs, which need not share the case's name.
            if "COMMAND" in args:
                index = args.index("COMMAND")
                if index + 1 < len(args):
                    registered_targets.add(args[index + 1])
            continue
        target, rest = args[0], args[1:]
        if command == "aimee_add_test":
            registered_targets.add(target)
        target_sources.setdefault(target, set()).update(
            _test_relative(item) for item in rest if item.endswith(".c")
        )
    # The aimee_add_test function body expands ${test_name}/${ARGN}; those
    # placeholder tokens never normalize to a declared source path.
    return {source for target in registered_targets for source in target_sources.get(target, ())}


def make_sources(root: Path) -> set[str]:
    """Repo-relative sources whose object the Make unit-test rules link.

    Make derives tests/<stem>.o from src/tests/<stem>.c by pattern rule, so the
    object path names the source unambiguously.
    """
    text = _read(root, MAKE_RULES)
    return {
        _test_relative(f"{stem}.c")
        for stem in re.findall(r"\$\(OBJDIR\)/tests/([A-Za-z0-9_]+)\.o", text)
    }


def declared_tests(root: Path) -> list[tuple[str, str]]:
    """(module id, declared test path) for every descriptor, in a stable order."""
    found: list[tuple[str, str]] = []
    for descriptor in sorted((root / MODULES).glob("*/module.yaml")):
        identifier = descriptor.parent.name
        try:
            value = json.loads(descriptor.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            raise RegistrationError(f"cannot read {identifier} descriptor: {exc}") from exc
        for relative in value.get("tests", []):
            found.append((identifier, relative))
    return sorted(found)


def report(root: Path) -> dict[str, object]:
    ctest = ctest_sources(root)
    make = make_sources(root)
    entries = []
    for identifier, relative in declared_tests(root):
        normalized = posixpath.normpath(relative)
        entries.append(
            {
                "module": identifier,
                "test": relative,
                "make": normalized in make,
                "ctest": normalized in ctest,
            }
        )
    return {"schema_version": 1, "tests": entries}


def _format(value: dict[str, object]) -> str:
    return json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n"


def check(root: Path) -> None:
    actual = report(root)
    path = root / BASELINE
    try:
        recorded = json.loads(path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise RegistrationError(f"cannot read {BASELINE.as_posix()}: {exc}") from exc
    except ValueError as exc:
        raise RegistrationError(f"{BASELINE.as_posix()} is not valid JSON: {exc}") from exc
    if recorded == actual:
        return
    recorded_rows = {(row.get("module"), row.get("test")): row for row in recorded.get("tests", [])}
    actual_rows = {(row["module"], row["test"]): row for row in actual["tests"]}
    lines = []
    for key in sorted(set(recorded_rows) | set(actual_rows)):
        before, after = recorded_rows.get(key), actual_rows.get(key)
        if before == after:
            continue
        lines.append(f"  {key[0]} {key[1]}: recorded={before} actual={after}")
    raise RegistrationError(
        "module test registration drifted from "
        f"{BASELINE.as_posix()}; regenerate with --write and re-review:\n" + "\n".join(lines)
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=str(ROOT), help="repository root")
    parser.add_argument("--write", action="store_true", help="regenerate the baseline")
    args = parser.parse_args(argv)
    root = Path(args.root).resolve()
    try:
        if args.write:
            (root / BASELINE).write_text(_format(report(root)), encoding="utf-8")
            print(f"module-test-registration: wrote {BASELINE.as_posix()}")
            return 0
        check(root)
    except RegistrationError as exc:
        print(f"module-test-registration: error: {exc}", file=sys.stderr)
        return 1
    print(f"module-test-registration: ok ({BASELINE.as_posix()})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
