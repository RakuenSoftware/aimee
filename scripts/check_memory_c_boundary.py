#!/usr/bin/env python3
"""Enforce that C at the memory-module boundary is transport/integration only."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


ALLOWED_C = {
    "src/modules/memory/gw_stage_memory.c",
    "src/modules/memory/memory_content_gate_bus.c",
    "src/modules/memory/memory_data_bus.c",
    "src/modules/memory/memory_domain_bus.c",
    "src/modules/memory/memory_domain_runtime_bus.c",
    "src/modules/memory/memory_scope_connection.c",
    "src/modules/memory/memory_embed_bus.c",
    "src/modules/memory/memory_extract_patterns.c",
    "src/modules/memory/memory_fact_gate.c",
    "src/modules/memory/memory_pii_gate.c",
}

FORBIDDEN_INCLUDES = (
    "db1_client/",
    "modules/db2/c/",
    "db_postgres.h",
    "sqlite3.h",
    "libpq-fe.h",
)

RETIRED_POLICY_C = (
    "src/posix/memory.c",
    "src/windows/memory.c",
    "src/modules/db2/c/prospective_memories.c",
    "src/kb/memory_rewrite_llm.c",
    "src/modules/memory/memory_rewrite_llm.h",
    "src/modules/memory/memory_legacy_bus.c",
    "src/kb/fact_grounding.c",
    "src/kb/fact_grounding.h",
)

FORBIDDEN_STORE_CALLS = (
    "db2_conn(",
    "aimee_pg_",
)

EXTERNAL_CONNECTION_C = {
    "src/modules/db2/c/fact_recall.c",
}

KB_CONNECTION_C = {
    "src/kb/kb_memory_facts.c",
}

FORBIDDEN_KB_MEMORY_POLICY = (
    "SELECT ",
    "INSERT INTO",
    "UPDATE ",
    "DELETE FROM",
    "SHA256",
    "fact_grounded",
    "fact_norm_text",
    "rel_type_canonicalize",
    "mf_claim_job",
    "mf_mark_done",
    "mf_mark_retry_or_fail",
    "mf_build_system_prompt",
    "mf_commit_facts",
    "mf_subject_kind",
)

# This adapter binds the already-authorized request scope onto a prepared
# PostgreSQL statement. It does not create statements or execute storage work;
# binding is connection plumbing and is the explicit exception to the direct
# store-call ban.
STATEMENT_BINDING_C = {
    "src/modules/memory/memory_scope_connection.c",
}


class BoundaryError(ValueError):
    pass


def validate(root: Path) -> None:
    descriptor_path = root / "src/modules/memory/module.yaml"
    descriptor = json.loads(descriptor_path.read_text(encoding="utf-8"))
    declared = {source for source in descriptor.get("sources", []) if source.endswith(".c")}
    physical = {
        path.relative_to(root).as_posix()
        for path in (root / "src/modules/memory").glob("*.c")
    }
    if declared != ALLOWED_C or physical != ALLOWED_C:
        raise BoundaryError(
            f"rule=memory-c-allowlist declared={sorted(declared)} physical={sorted(physical)} "
            f"expected={sorted(ALLOWED_C)}"
        )

    retired = sorted((root / "src/modules/db2/c").glob("memory_*.c"))
    if retired:
        raise BoundaryError(
            "rule=retired-db2-memory-c files="
            + repr([path.relative_to(root).as_posix() for path in retired])
        )

    returned_policy = [relative for relative in RETIRED_POLICY_C if (root / relative).exists()]
    if returned_policy:
        raise BoundaryError(
            f"rule=retired-platform-memory-policy files={returned_policy}"
        )

    missing_connections = [relative for relative in KB_CONNECTION_C if not (root / relative).exists()]
    if missing_connections:
        raise BoundaryError(f"rule=memory-c-connection-inventory missing={missing_connections}")

    for relative in sorted(KB_CONNECTION_C):
        text = (root / relative).read_text(encoding="utf-8")
        returned = [token for token in FORBIDDEN_KB_MEMORY_POLICY if token in text]
        if returned:
            raise BoundaryError(
                f"rule=kb-memory-c-policy file={relative} tokens={returned}"
            )

    violations: list[str] = []
    for relative in sorted(ALLOWED_C | EXTERNAL_CONNECTION_C):
        text = (root / relative).read_text(encoding="utf-8")
        for include in FORBIDDEN_INCLUDES:
            if include in text:
                violations.append(f"{relative}: {include}")
        for call in FORBIDDEN_STORE_CALLS:
            if call in text and not (relative in STATEMENT_BINDING_C and call == "aimee_pg_"):
                violations.append(f"{relative}: {call}")
    if violations:
        raise BoundaryError(
            f"rule=memory-c-direct-store-access violations={violations}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    try:
        validate(args.root.resolve())
    except (BoundaryError, FileNotFoundError, json.JSONDecodeError) as exc:
        print(f"check_memory_c_boundary: {exc}")
        return 1
    print("check_memory_c_boundary: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
