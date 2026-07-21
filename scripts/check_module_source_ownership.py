#!/usr/bin/env python3
"""Enforce canonical source ownership for landed modularization slices."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import sys


@dataclass(frozen=True)
class Contract:
    module: str
    legacy_source: str
    legacy_header: str
    canonical_source: str
    canonical_header: str
    canonical_include: str
    make_source: str
    cmake_source: str
    legacy_cmake_source: str
    test_object: str
    legacy_test_object: str
    consumers: tuple[str, ...]
    document: str
    document_markers: tuple[str, ...]


CONTRACTS = (
    Contract(
        module="plugin-loader",
        legacy_source="src/plugin_loader.c",
        legacy_header="src/headers/plugin_loader.h",
        canonical_source="src/modules/plugin-loader/plugin_loader.c",
        canonical_header="src/modules/plugin-loader/include/aimee/plugin-loader/plugin_loader.h",
        canonical_include="aimee/plugin-loader/plugin_loader.h",
        make_source="modules/plugin-loader/plugin_loader.c",
        cmake_source="${AIMEE_SRC_DIR}/modules/plugin-loader/plugin_loader.c",
        legacy_cmake_source="${AIMEE_SRC_DIR}/plugin_loader.c",
        test_object="$(OBJDIR)/modules/plugin-loader/plugin_loader.o",
        legacy_test_object="$(OBJDIR)/plugin_loader.o",
        consumers=(
            "src/modules/plugin-loader/plugin_loader.c",
            "src/server/server_main.c",
            "src/tests/test_plugin_loader.c",
        ),
        document="docs/modules/plugin-loader.md",
        document_markers=("The link profile remains unconditional",),
    ),
    Contract(
        module="module-runtime-pre-llm-hook",
        legacy_source="src/plugin_c_hook.c",
        legacy_header="src/headers/plugin_c_hook.h",
        canonical_source="src/modules/module-runtime/pre_llm_hook.c",
        canonical_header="src/modules/module-runtime/include/aimee/module-runtime/pre_llm_hook.h",
        canonical_include="aimee/module-runtime/pre_llm_hook.h",
        make_source="modules/module-runtime/pre_llm_hook.c",
        cmake_source="${AIMEE_SRC_DIR}/modules/module-runtime/pre_llm_hook.c",
        legacy_cmake_source="${AIMEE_SRC_DIR}/plugin_c_hook.c",
        test_object="$(OBJDIR)/modules/module-runtime/pre_llm_hook.o",
        legacy_test_object="$(OBJDIR)/plugin_c_hook.o",
        consumers=(
            "src/modules/module-runtime/pre_llm_hook.c",
            "src/server/agent_runtime.c",
            "src/tests/test_plugin_c_hook.c",
        ),
        document="docs/modules/module-runtime.md",
        document_markers=("system prompt", "plugin_chook_apply_pre_llm"),
    ),
    Contract(
        module="plugin-loader-contract",
        legacy_source="src/plugin.c",
        legacy_header="src/headers/plugin.h",
        canonical_source="src/modules/plugin-loader/plugin.c",
        canonical_header="src/modules/plugin-loader/include/aimee/plugin-loader/plugin.h",
        canonical_include="aimee/plugin-loader/plugin.h",
        make_source="modules/plugin-loader/plugin.c",
        cmake_source="${AIMEE_SRC_DIR}/modules/plugin-loader/plugin.c",
        legacy_cmake_source="${AIMEE_SRC_DIR}/plugin.c",
        test_object="$(OBJDIR)/modules/plugin-loader/plugin.o",
        legacy_test_object="$(OBJDIR)/plugin.o",
        consumers=(
            "src/modules/plugin-loader/plugin.c",
            "src/modules/plugin-loader/include/aimee/plugin-loader/plugin_loader.h",
            "src/tests/test_plugin.c",
        ),
        document="docs/modules/plugin-loader.md",
        document_markers=("plugin_manifest_parse", "plugin_load_and_register",
                          "plugin_permission_name", "plugin_permission_from_str"),
    ),
    Contract(
        module="module-runtime-extension",
        legacy_source="src/plugin_ctx.c",
        legacy_header="src/headers/plugin_ctx.h",
        canonical_source="src/modules/module-runtime/extension.c",
        canonical_header="src/modules/module-runtime/include/aimee/module-runtime/extension.h",
        canonical_include="aimee/module-runtime/extension.h",
        make_source="modules/module-runtime/extension.c",
        cmake_source="${AIMEE_SRC_DIR}/modules/module-runtime/extension.c",
        legacy_cmake_source="${AIMEE_SRC_DIR}/plugin_ctx.c",
        test_object="$(OBJDIR)/modules/module-runtime/extension.o",
        legacy_test_object="$(OBJDIR)/plugin_ctx.o",
        consumers=(
            "src/modules/module-runtime/extension.c",
            "src/modules/plugin-loader/include/aimee/plugin-loader/plugin.h",
            "src/tests/test_plugin.c",
        ),
        document="docs/modules/module-runtime.md",
        document_markers=("plugin_ctx_create", "plugin_ctx_destroy"),
    ),
)


class CheckError(ValueError):
    pass


def require(condition: bool, rule: str, detail: str) -> None:
    if not condition:
        raise CheckError(f"rule={rule}: {detail}")


def read(root: Path, relative: str) -> str:
    try:
        return (root / relative).read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        raise CheckError(f"rule=input: cannot read {relative}: {exc}") from exc


def source_files(root: Path):
    for path in (root / "src").rglob("*"):
        if path.is_file() and path.suffix in {".c", ".h"}:
            yield path.relative_to(root).as_posix()


def validate_contract(root: Path, contract: Contract, makefile: str, cmake: str, rules: str) -> None:
    for relative in (contract.legacy_source, contract.legacy_header):
        require(not (root / relative).exists(), "legacy-path-removed",
                f"{contract.module}: {relative}")
    for relative in (contract.canonical_source, contract.canonical_header):
        require((root / relative).is_file(), "canonical-path-present",
                f"{contract.module}: {relative}")

    make_tokens = makefile.replace("\\", " ").split()
    require(make_tokens.count(contract.make_source) == 1, "core-source-unique",
            f"{contract.module}: Make canonical source")
    require(Path(contract.legacy_source).name not in make_tokens, "core-source-unique",
            f"{contract.module}: Make legacy source")
    cmake_pattern = re.compile(rf"^\s*{re.escape(contract.cmake_source)}\s*$", re.MULTILINE)
    require(len(cmake_pattern.findall(cmake)) == 1, "core-source-unique",
            f"{contract.module}: CMake canonical source")
    require(contract.legacy_cmake_source not in cmake, "core-source-unique",
            f"{contract.module}: CMake legacy source")

    # One canonical object can appear in several independently linked test targets.
    require(rules.count(contract.test_object) >= 1, "focused-test-object",
            f"{contract.module}: canonical object")
    require(contract.legacy_test_object not in rules, "focused-test-object",
            f"{contract.module}: legacy object")

    include = f'#include "{contract.canonical_include}"'
    for relative in contract.consumers:
        count = read(root, relative).count(include)
        require(count >= 1, "canonical-include-missing", f"{contract.module}: {relative}")
        require(count == 1, "canonical-include-duplicated", f"{contract.module}: {relative}")

    basename = re.escape(Path(contract.canonical_header).name)
    include_line = re.compile(rf'^\s*#\s*include\s*[<"]([^>"]*{basename})[>"]\s*$', re.MULTILINE)
    for relative in source_files(root):
        for match in include_line.finditer(read(root, relative)):
            require(match.group(1) == contract.canonical_include,
                    "non-canonical-module-include",
                    f"{contract.module}: {relative}: {match.group(1)}")

    document = read(root, contract.document)
    for marker in (contract.canonical_source, contract.canonical_header, *contract.consumers,
                   *contract.document_markers):
        require(marker in document, "module-document", f"{contract.module}: {marker}")


def validate(root: Path) -> None:
    require((root / ".git").exists() or (root / ".git").is_file(), "config-root", str(root))
    makefile = read(root, "src/Makefile")
    cmake = read(root, "CMakeLists.txt")
    rules = read(root, "src/tests/Rules.mk")
    for contract in CONTRACTS:
        validate_contract(root, contract, makefile, cmake, rules)

    source = "\n".join(read(root, relative) for relative in source_files(root))
    for symbol in (
        "plugin_ctx_create_ex",
        "plugin_ctx_destroy_ex",
        "plugin_ctx_name",
        "plugin_ctx_source_path",
        "plugin_ctx_kind",
        "plugin_ctx_set_source_path",
        "plugin_ctx_set_kind",
        "plugin_ctx_register_tool",
        "plugin_ctx_register_hook",
        "plugin_ctx_register_slash_command",
        "plugin_ctx_register_cli_subcommand",
        "plugin_ctx_register_memory_provider",
        "plugin_ctx_register_context_engine",
    ):
        require(not re.search(rf"\b{symbol}\b", source), "dead-wrapper-removed", symbol)

    runtime_root = root / "src/modules/module-runtime"
    for path in runtime_root.rglob("*"):
        if path.is_file() and path.suffix in {".c", ".h"}:
            require("aimee/plugin-loader/" not in path.read_text(encoding="utf-8"),
                    "core-to-optional-edge", path.relative_to(root).as_posix())

    for relative in source_files(root):
        content = read(root, relative)
        require('#include "headers/plugin.h"' not in content,
                "legacy-include-removed", relative)
        require('#include "headers/plugin_ctx.h"' not in content,
                "legacy-include-removed", relative)

    require(makefile.count("-Imodules/plugin-loader/include") == 1,
            "module-include-root", "Make plugin-loader include root")
    require(makefile.count("-Imodules/module-runtime/include") == 1,
            "module-include-root", "Make module-runtime include root")
    require(cmake.count("set(AIMEE_PLUGIN_LOADER_INCLUDE_DIR") == 1,
            "module-include-root", "CMake plugin-loader include root")
    require(cmake.count("set(AIMEE_MODULE_RUNTIME_INCLUDE_DIR") == 1,
            "module-include-root", "CMake module-runtime include root")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config-root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    try:
        validate(args.config_root.resolve())
    except CheckError as exc:
        print(f"module-source-ownership: ERROR {exc}", file=sys.stderr)
        return 1
    print(f"module-source-ownership: ok ({len(CONTRACTS)} contracts)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
