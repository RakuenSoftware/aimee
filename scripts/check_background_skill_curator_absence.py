#!/usr/bin/env python3
"""Fail closed if the retired background skill curator returns."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys


DELETED = (
    "src/modules/skill/skill_curator.c",
    "src/modules/skill/skill_curator.h",
)
CANONICAL_DELETED = (
    "src/modules/skills/skill_curator.c",
    "src/modules/skills/include/aimee/skills/skill_curator.h",
)
FORBIDDEN = (
    "skill_curator_maybe",
    "skill_curator_metrics",
    "server_compute_skill_curator_async",
    "skills_curator_enabled",
    "skills_curator_interval_hours",
    "skill_curator.c",
    "skill_curator.h",
)
CONFIG_KEYS = ("curator_interval_hours", '"curator"')
CONFIG_FILES = (
    "src/modules/config/config.c",
    "src/modules/config/config.h",
    "src/modules/config/config_save.c",
    "src/modules/config/config_skills.c",
)
BUILD_FILES = ("CMakeLists.txt", "src/Makefile", "src/tests/Rules.mk")
DISPOSITION = "docs/audit/dispositions/background-skill-curator.yaml"
PROPOSAL = "docs/proposals/done/feature-liveness-and-background-curator-removal.md"


class CheckError(ValueError):
    pass


def require(condition: bool, rule: str, detail: str) -> None:
    if not condition:
        raise CheckError(f"rule={rule}: {detail}")


def read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        raise CheckError(f"rule=input: cannot read {path}: {exc}") from exc


def product_files(root: Path):
    for path in (root / "src").rglob("*"):
        if path.is_file() and (path.suffix in {".c", ".h"} or path.name in {"Makefile", "Rules.mk"}):
            yield path
    yield root / "CMakeLists.txt"


def validate(root: Path) -> None:
    require((root / ".git").exists() or (root / ".git").is_file(), "config-root", str(root))
    for rel in (*DELETED, *CANONICAL_DELETED):
        require(not (root / rel).exists(), "deleted-file", f"retired file exists: {rel}")

    for path in product_files(root):
        if not path.exists():
            continue
        text = read(path)
        for token in FORBIDDEN:
            require(token not in text, "retired-reference", f"{token!r} in {path.relative_to(root)}")

    for rel in CONFIG_FILES:
        text = read(root / rel)
        for key in CONFIG_KEYS:
            require(key not in text, "retired-config", f"{key!r} in {rel}")

    for rel in BUILD_FILES:
        text = read(root / rel)
        # The protected build may invoke this checker by name; reject only the
        # retired object/source forms rather than making enforcement self-failing.
        require("skill_curator.o" not in text, "retired-build-object", rel)

    memory = read(root / "src/modules/memory/memory_maintenance.c")
    require("db1_maintenance_state_load" in memory, "memory-maintenance-preserved", "load anchor")
    require("db1_maintenance_state_save" in memory, "memory-maintenance-preserved", "save anchor")
    require((root / "src/db1/maintenance.c").is_file(), "maintenance-state-preserved", "DB1 implementation")

    kb_files = list((root / "src/modules/kb-synthesis").glob("kb_curator_*.c"))
    require(len(kb_files) >= 3, "kb-curator-preserved", "KB curator implementation family")
    require((root / "src/modules/config/config_kb_curator.c").is_file(), "kb-curator-preserved", "KB config")
    build_text = "\n".join(read(root / rel) for rel in BUILD_FILES)
    require("kb_curator_pipeline.c" in build_text, "kb-curator-preserved", "KB build anchor")

    legacy_test = read(root / "src/tests/test_config.c")
    require("Retired background skill-curator keys" in legacy_test, "legacy-config-test", "test marker")
    require("curator_interval_hours" in legacy_test and "curator:" in legacy_test,
            "legacy-config-test", "exact retired nesting")
    require("g_config_strict = 1" in legacy_test, "legacy-config-test", "strict-mode compatibility")

    disposition_path = root / DISPOSITION
    try:
        disposition = json.loads(read(disposition_path))
    except json.JSONDecodeError as exc:
        raise CheckError(f"rule=disposition-json: {exc}") from exc
    require(disposition.get("schema_version") == 1, "disposition", "schema_version")
    require(disposition.get("feature") == "background-skill-curator", "disposition", "feature")
    require(disposition.get("decision") == "remove", "disposition", "decision")
    require(disposition.get("source_proposal") == PROPOSAL, "disposition", "source proposal")
    require(disposition.get("removed_files") == list(DELETED), "disposition", "removed_files")
    require(disposition.get("removed_symbols") == list(FORBIDDEN[:5]), "disposition", "removed_symbols")
    compatibility = disposition.get("compatibility", {})
    require(compatibility.get("legacy_config") == "accepted-and-scrubbed-on-save",
            "disposition", "legacy compatibility")
    require(compatibility.get("database") == "leave-existing-maintenance_state-key-skill_curator-inert",
            "disposition", "inert database row")
    require(compatibility.get("schema_or_data_migration") is False, "disposition", "migration boundary")
    preserved = disposition.get("preserved", {})
    require(preserved.get("generic_memory_maintenance") == "src/modules/memory/memory_maintenance.c",
            "disposition", "memory boundary")
    require(preserved.get("kb_synthesis_curator") == "src/modules/kb-synthesis/kb_curator_pipeline.c",
            "disposition", "KB boundary")
    roundtable = disposition.get("roundtable", {})
    require(roundtable.get("decision") == "approve", "disposition", "roundtable decision")
    require(str(roundtable.get("run_id", "")).startswith("oprun_"), "disposition", "roundtable run")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config-root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    try:
        validate(args.config_root.resolve())
    except CheckError as exc:
        print(f"background-skill-curator-absence: ERROR {exc}", file=sys.stderr)
        return 1
    print("background-skill-curator-absence: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
