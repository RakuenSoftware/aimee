#!/usr/bin/env python3
"""Enforce the first plugin-loader physical-ownership slice."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


LEGACY_SOURCE = "src/plugin_loader.c"
LEGACY_HEADER = "src/headers/plugin_loader.h"
CANONICAL_SOURCE = "src/modules/plugin-loader/plugin_loader.c"
CANONICAL_HEADER = (
    "src/modules/plugin-loader/include/aimee/plugin-loader/plugin_loader.h"
)
CANONICAL_INCLUDE = '#include "aimee/plugin-loader/plugin_loader.h"'
MAKE_SOURCE = "modules/plugin-loader/plugin_loader.c"
CMAKE_SOURCE = "${AIMEE_SRC_DIR}/modules/plugin-loader/plugin_loader.c"
MAKE_TEST_OBJECT = "$(OBJDIR)/modules/plugin-loader/plugin_loader.o"
LEGACY_TEST_OBJECT = "$(OBJDIR)/plugin_loader.o"
CONSUMERS = (
    CANONICAL_SOURCE,
    "src/server/server_main.c",
    "src/tests/test_plugin_loader.c",
)
DOC = "docs/modules/plugin-loader.md"
DOC_MARKERS = (
    CANONICAL_SOURCE,
    CANONICAL_HEADER,
    "The link profile remains unconditional",
    "src/server/server_main.c",
    "src/tests/test_plugin_loader.c",
)
MAKE_INCLUDE = "-Iheaders -Imodules/plugin-loader/include"
CMAKE_INCLUDE = "${AIMEE_SRC_DIR}/headers ${AIMEE_PLUGIN_LOADER_INCLUDE_DIR}"


class CheckError(ValueError):
    pass


def require(condition: bool, rule: str, detail: str) -> None:
    if not condition:
        raise CheckError(f"rule={rule}: {detail}")


def read(root: Path, relative: str) -> str:
    path = root / relative
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        raise CheckError(f"rule=input: cannot read {relative}: {exc}") from exc


def source_files(root: Path):
    for path in (root / "src").rglob("*"):
        if path.is_file() and path.suffix in {".c", ".h"}:
            yield path


def validate(root: Path) -> None:
    require((root / ".git").exists() or (root / ".git").is_file(), "config-root", str(root))

    # (a) legacy-path-removed
    for relative in (LEGACY_SOURCE, LEGACY_HEADER):
        require(not (root / relative).exists(), "legacy-path-removed", relative)

    # (b) canonical-path-present
    for relative in (CANONICAL_SOURCE, CANONICAL_HEADER):
        require((root / relative).is_file(), "canonical-path-present", relative)

    makefile = read(root, "src/Makefile")
    cmake = read(root, "CMakeLists.txt")
    rules = read(root, "src/tests/Rules.mk")

    # (c) core-source-unique
    make_tokens = makefile.replace("\\", " ").split()
    cmake_source_line = re.compile(
        r"^\s*[$][{]AIMEE_SRC_DIR[}]/modules/plugin-loader/plugin_loader[.]c\s*$",
        re.MULTILINE,
    )
    require(make_tokens.count(MAKE_SOURCE) == 1, "core-source-unique", "Make canonical count")
    require(len(cmake_source_line.findall(cmake)) == 1,
            "core-source-unique", "CMake canonical count")
    require("plugin_loader.c" not in make_tokens, "core-source-unique", "Make legacy source")
    require("${AIMEE_SRC_DIR}/plugin_loader.c" not in cmake,
            "core-source-unique", "CMake legacy source")

    # (d) focused-test-object
    require(rules.count(MAKE_TEST_OBJECT) == 1, "focused-test-object", "canonical object")
    require(LEGACY_TEST_OBJECT not in rules, "focused-test-object", "legacy object")

    # (e) canonical-includes
    for relative in CONSUMERS:
        text = read(root, relative)
        count = text.count(CANONICAL_INCLUDE)
        require(count >= 1, "canonical-include-missing", relative)
        require(count == 1, "canonical-include-duplicated", relative)
    include_line = re.compile(
        r'^\s*#\s*include\s*[<"]([^>"]*plugin_loader[.]h)[>"]\s*$', re.MULTILINE
    )
    for path in source_files(root):
        relative = path.relative_to(root).as_posix()
        for match in include_line.finditer(read(root, relative)):
            require(match.group(1) == "aimee/plugin-loader/plugin_loader.h",
                    "non-canonical-plugin-loader-include", f"{relative}: {match.group(1)}")

    # (f) core-build-legacy-reference-absent
    scoped = ("CMakeLists.txt", "src/Makefile", "src/tests/Rules.mk")
    for relative in scoped:
        text = read(root, relative)
        for legacy in (LEGACY_SOURCE, LEGACY_HEADER, LEGACY_TEST_OBJECT):
            require(legacy not in text, "core-build-legacy-reference-absent",
                    f"{relative}: {legacy}")
    require(makefile.count(MAKE_INCLUDE) == 1, "module-include-root", "Make include order")
    require(cmake.count(CMAKE_INCLUDE) == 1, "module-include-root", "CMake include order")
    require(cmake.count("set(AIMEE_PLUGIN_LOADER_INCLUDE_DIR") == 1,
            "module-include-root", "CMake include variable")

    document = read(root, DOC)
    for marker in DOC_MARKERS:
        require(marker in document, "module-document", marker)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config-root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    try:
        validate(args.config_root.resolve())
    except CheckError as exc:
        print(f"plugin-loader-ownership: ERROR {exc}", file=sys.stderr)
        return 1
    print("plugin-loader-ownership: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
