#!/usr/bin/env python3
"""Enforce canonical source ownership for landed modularization slices."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
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
    cmake_source: str | None
    legacy_cmake_source: str | None
    test_object: str
    legacy_test_object: str
    consumers: tuple[str, ...]
    document: str
    document_markers: tuple[str, ...]
    test_cmake_source: str | None = None
    legacy_test_cmake_source: str | None = None


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
        document_markers=("AIMEE_WITH_PLUGIN_LOADER", "disabled profile"),
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
    Contract(
        module="gateway-pipeline",
        legacy_source="src/gateway_pipeline.c",
        legacy_header="src/headers/gateway_pipeline.h",
        canonical_source="src/modules/gateway/gateway_pipeline.c",
        canonical_header="src/modules/gateway/include/aimee/gateway/gateway_pipeline.h",
        canonical_include="aimee/gateway/gateway_pipeline.h",
        make_source="modules/gateway/gateway_pipeline.c",
        cmake_source=None,
        legacy_cmake_source=None,
        test_object="$(OBJDIR)/modules/gateway/gateway_pipeline.o",
        legacy_test_object="$(OBJDIR)/gateway_pipeline.o",
        consumers=(
            "src/modules/gateway/gateway_pipeline.c",
            "src/tests/test_gateway_pipeline.c",
        ),
        document="docs/modules/gateway.md",
        document_markers=("gw_pipeline_run_request", "canonical include namespace"),
    ),
    Contract(
        module="gateway-policy",
        legacy_source="src/gateway_policy.c",
        legacy_header="src/headers/gateway_policy.h",
        canonical_source="src/modules/gateway/gateway_policy.c",
        canonical_header="src/modules/gateway/include/aimee/gateway/gateway_policy.h",
        canonical_include="aimee/gateway/gateway_policy.h",
        make_source="modules/gateway/gateway_policy.c",
        cmake_source=None,
        legacy_cmake_source=None,
        test_object="$(OBJDIR)/modules/gateway/gateway_policy.o",
        legacy_test_object="$(OBJDIR)/gateway_policy.o",
        consumers=(
            "src/modules/gateway/gateway_policy.c",
            "src/tests/test_gateway_policy.c",
        ),
        document="docs/modules/gateway.md",
        document_markers=("gateway_policy_apply_request", "canonical include namespace"),
    ),
    Contract(
        module="gateway-delegate",
        legacy_source="src/gateway_delegate.c",
        legacy_header="src/headers/gateway_delegate.h",
        canonical_source="src/modules/gateway/gateway_delegate.c",
        canonical_header="src/modules/gateway/include/aimee/gateway/gateway_delegate.h",
        canonical_include="aimee/gateway/gateway_delegate.h",
        make_source="modules/gateway/gateway_delegate.c",
        cmake_source=None,
        legacy_cmake_source=None,
        test_object="$(OBJDIR)/modules/gateway/gateway_delegate.o",
        legacy_test_object="$(OBJDIR)/gateway_delegate.o",
        consumers=(
            "src/modules/gateway/gateway_delegate.c",
            "src/tests/test_gateway_p4_delegate.c",
        ),
        document="docs/modules/gateway.md",
        document_markers=("gateway_delegate_run_request_pipeline", "canonical include namespace"),
    ),
    Contract(
        module="ir-messaging",
        legacy_source="src/server/aimee_ir.c",
        legacy_header="src/headers/aimee_ir.h",
        canonical_source="src/modules/ir/aimee_ir.c",
        canonical_header="src/modules/ir/include/aimee/ir/aimee_ir.h",
        canonical_include="aimee/ir/aimee_ir.h",
        make_source="modules/ir/aimee_ir.c",
        cmake_source=None,
        legacy_cmake_source=None,
        test_object="$(OBJDIR)/modules/ir/aimee_ir.o",
        legacy_test_object="$(OBJDIR)/server/aimee_ir.o",
        consumers=(
            "src/modules/ir/aimee_ir.c",
            "src/tests/test_aimee_ir.c",
        ),
        document="docs/modules/ir.md",
        document_markers=("provider-neutral message model", "canonical include namespace"),
        test_cmake_source="../modules/ir/aimee_ir.c",
        legacy_test_cmake_source="../server/aimee_ir.c",
    ),
    Contract(
        module="ir-metrics",
        legacy_source="src/server/aimee_ir_metrics.c",
        legacy_header="src/headers/aimee_ir_metrics.h",
        canonical_source="src/modules/ir/aimee_ir_metrics.c",
        canonical_header="src/modules/ir/include/aimee/ir/aimee_ir_metrics.h",
        canonical_include="aimee/ir/aimee_ir_metrics.h",
        make_source="modules/ir/aimee_ir_metrics.c",
        cmake_source=None,
        legacy_cmake_source=None,
        test_object="$(OBJDIR)/modules/ir/aimee_ir_metrics.o",
        legacy_test_object="$(OBJDIR)/server/aimee_ir_metrics.o",
        consumers=(
            "src/modules/ir/aimee_ir_metrics.c",
            "src/tests/test_aimee_ir_metrics.c",
        ),
        document="docs/modules/ir.md",
        document_markers=("IR-local metrics", "canonical include namespace"),
        test_cmake_source="../modules/ir/aimee_ir_metrics.c",
        legacy_test_cmake_source="../server/aimee_ir_metrics.c",
    ),
)

LEGACY_MODULE_ROOTS = (
    (
        "skills",
        "src/modules/skill",
        ("CMakeLists.txt", "src/Makefile", "src/tests/Rules.mk"),
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


def validate_legacy_module_root(
    root: Path,
    module: str,
    legacy_root: str,
    build_files: tuple[str, ...],
) -> None:
    """Reject a retired module directory and its exact build-path spelling."""
    legacy_path = Path(legacy_root)
    require(
        not legacy_path.is_absolute()
        and len(legacy_path.parts) > 1
        and legacy_path.parts[0] == "src"
        and ".." not in legacy_path.parts,
        "legacy-root-format",
        f"{module}: expected a normalized path beneath src/: {legacy_root}",
    )
    require(not (root / legacy_root).exists(), "legacy-module-root",
            f"{module}: {legacy_root}")
    build_root = Path(*legacy_path.parts[1:]).as_posix().rstrip("/") + "/"
    for relative in build_files:
        require(build_root not in read(root, relative), "legacy-build-root",
                f"{module}: {build_root} in {relative}")


def validate_contract(
    root: Path,
    contract: Contract,
    makefile: str,
    cmake: str,
    test_cmake: str,
    rules: str,
) -> None:
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
    if contract.cmake_source is not None:
        cmake_pattern = re.compile(rf"^\s*{re.escape(contract.cmake_source)}\s*$", re.MULTILINE)
        require(len(cmake_pattern.findall(cmake)) == 1, "core-source-unique",
                f"{contract.module}: CMake canonical source")
    else:
        require(Path(contract.canonical_source).name not in cmake, "core-source-unique",
                f"{contract.module}: CMake source must remain absent")
    if contract.legacy_cmake_source is not None:
        require(contract.legacy_cmake_source not in cmake, "core-source-unique",
                f"{contract.module}: CMake legacy source")
    if contract.test_cmake_source is not None:
        require(contract.test_cmake_source in test_cmake, "focused-test-source",
                f"{contract.module}: test CMake canonical source")
    if contract.legacy_test_cmake_source is not None:
        require(contract.legacy_test_cmake_source not in test_cmake, "focused-test-source",
                f"{contract.module}: test CMake legacy source")

    # One canonical object can appear in several independently linked test targets.
    require(rules.count(contract.test_object) >= 1, "focused-test-object",
            f"{contract.module}: canonical object")
    require(contract.legacy_test_object not in rules, "focused-test-object",
            f"{contract.module}: legacy object")

    include = re.compile(
        rf'^\s*#\s*include\s*[<"]{re.escape(contract.canonical_include)}[>"]\s*$',
        re.MULTILINE,
    )
    for relative in contract.consumers:
        count = len(include.findall(read(root, relative)))
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
    test_cmake = read(root, "src/tests/CMakeLists.txt")
    rules = read(root, "src/tests/Rules.mk")
    for contract in CONTRACTS:
        validate_contract(root, contract, makefile, cmake, test_cmake, rules)

    for module, legacy_root, build_files in LEGACY_MODULE_ROOTS:
        validate_legacy_module_root(root, module, legacy_root, build_files)

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
    require(makefile.count("-Imodules/ir/include") == 1,
            "module-include-root", "Make IR include root")
    require(cmake.count("set(AIMEE_PLUGIN_LOADER_INCLUDE_DIR") == 1,
            "module-include-root", "CMake plugin-loader include root")
    require(cmake.count("set(AIMEE_MODULE_RUNTIME_INCLUDE_DIR") == 1,
            "module-include-root", "CMake module-runtime include root")
    require(cmake.count("set(AIMEE_IR_INCLUDE_DIR") == 1,
            "module-include-root", "CMake IR include root")

    descriptor = json.loads(read(root, "src/modules/plugin-loader/module.yaml"))
    features = read(root, "src/headers/aimee_features.h")
    require(descriptor.get("enabled_by_default") is False,
            "plugin-loader-profile-default", "descriptor must default disabled")
    require(re.search(r"^AIMEE_WITH_PLUGIN_LOADER\s*\?=\s*0$", makefile, re.MULTILINE) is not None,
            "plugin-loader-profile-default", "Make must default disabled")
    require('option(AIMEE_WITH_PLUGIN_LOADER "Build the optional plugin manifest loader" OFF)' in cmake,
            "plugin-loader-profile-default", "CMake must default disabled")
    require(re.search(r"^#define AIMEE_WITH_PLUGIN_LOADER 0$", features, re.MULTILINE) is not None,
            "plugin-loader-profile-default", "header fallback must default disabled")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config-root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    try:
        validate(args.config_root.resolve())
    except CheckError as exc:
        print(f"module-source-ownership: ERROR {exc}", file=sys.stderr)
        return 1
    legacy_count = len(LEGACY_MODULE_ROOTS)
    legacy_label = "root" if legacy_count == 1 else "roots"
    print(
        "module-source-ownership: ok "
        f"({len(CONTRACTS)} contracts, {legacy_count} legacy {legacy_label})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
