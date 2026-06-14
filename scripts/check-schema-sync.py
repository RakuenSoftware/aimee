#!/usr/bin/env python3
"""Verify the DB2 shim schema and the DB2 native schema cover the same set
of tables, and that the DB1 schema only contains DB1-owned tables.

After the 3db split, the DB2 shim schema used by tests lives in
src/db2/schema_sqlite.sql; production DB2 uses src/db2/schema.sql.
Drift between those two files breaks the DB2 shim test path.

Run via `make schema-sync-check` (or directly during CI). Exits non-zero on
drift; prints the set differences so the fix is obvious.

Doesn't check column-level drift: generated search columns, tier-specific
primary-key spellings, and lexical-index tables vs views legitimately differ
between the two files.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DB1_SCHEMA = ROOT / "src" / "db1" / "schema.sql"
DB2_SCHEMA_PG = ROOT / "src" / "db2" / "schema.sql"
DB2_SCHEMA_SQLITE = ROOT / "src" / "db2" / "schema_sqlite.sql"

# CREATE TABLE [IF NOT EXISTS] <name> ( ... ). `name` may be quoted or bare.
TABLE_RE = re.compile(
    r"create\s+table\s+(?:if\s+not\s+exists\s+)?(?:\"([^\"]+)\"|(\w+))",
    re.IGNORECASE,
)

# Lexical-index virtual tables don't have analogues as *tables* in the DB2
# native schema; their content is mirrored through native search projections,
# so skip them from the parity check between the shim and the native schema.
DB2_SHIM_ONLY_LEXICAL_INDEX_TABLES = {
    "memories_fts",
    "memory_chunks_fts",
    "memory_units_fts",
    "memory_negation_fts",
    "prospective_memories_fts",
    "epistemic_directives_fts",
    "memories_code_fts",
    "code_fts",
    "kb_fts",
}

# DB1-local user/session/runtime tables. These belong only in the DB1 schema;
# the DB2 schemas (sqlite shim and postgres) must not create them.
DB1_ONLY_TABLES = {
    "agent_cache",
    "agent_jobs",
    "agent_log",
    "branch_ownership",
    "clarify_qa",
    "clarify_sessions",
    "checkpoints",
    "coord_job_tasks",
    "coord_jobs",
    "context_cache",
    "context_snapshots",
    "cost_fold_log",
    "cron_job_runs",
    "cron_jobs",
    "decisions",
    "delegation_checkpoint",
    "delegation_messages",
    "delegation_spawns",
    "diagnoses",
    "diagnosis_items",
    "env_capabilities",
    "eval_results",
    "execution_plans",
    "execution_trace",
    "file_snapshot_entries",
    "file_snapshots",
    "local_operator",
    "maintenance_state",
    "memory_cognify_jobs",
    "memory_runtime_state",
    "mcp_osv_cache",
    "model_catalog",
    "pipelines",
    "plan_steps",
    "primary_sessions",
    "project_clones",
    "server_sessions",
    "session_state",
    "session_state_ap_hits",
    "session_state_file_hashes",
    "session_state_read_paths",
    "session_state_seen_paths",
    "session_state_write_paths",
    "session_state_tdd_writes",
    "session_state_worktrees",
    "step_evidence",
    "token_audit",
    "tool_local_availability",
    "trigger_runs",
    "version_state",
    "window_files",
    "window_terms",
    "windows",
    "work_queue",
    "work_queue_log",
    "wc_channel_messages",
    "wc_channels",
    "workflow_sessions",
    "working_memory",
    "working_profile_observations_local",
    "working_profile_promotion_progress",
    "working_profile_state_local",
    "conv_tool_events",
    "conv_tool_chains",
    "conv_context_state",
    "payload_rewrite_state",
    "guardrail_events",
    "delegate_learnings",
    "interaction_events",
    "roadmap_dispatch",
    "roadmap_unit_dispatch",
    "roadmap_milestone_lease",
    # roundtable authoring pipeline ledger (per-machine, DB1-owned)
    "roundtable_pipeline_runs",
    "roundtable_pipeline_passes",
    "roundtable_pipeline_attempts",
    "roundtable_pipeline_gates",
    # workflow engine work-item state + audit (per-user, DB1-owned)
    "lifecycle_work_item",
    "lifecycle_event",
    "lifecycle_stage_attempt",
}

# Sole DB1-owned lexical index. Lives only in db1/schema.sql.
DB1_OWNED_LEXICAL_INDEX = {"window_fts"}


def extract_tables(path: Path) -> set[str]:
    text = path.read_text()
    # Strip -- comments (single-line only; good enough for our schemas).
    text = re.sub(r"--[^\n]*", "", text)
    return {(m.group(1) or m.group(2)).lower() for m in TABLE_RE.finditer(text)}


def main() -> int:
    db1_tables = extract_tables(DB1_SCHEMA)
    db2_shim_tables = extract_tables(DB2_SCHEMA_SQLITE)
    db2_native_tables = extract_tables(DB2_SCHEMA_PG)

    shim_shareable = db2_shim_tables - DB2_SHIM_ONLY_LEXICAL_INDEX_TABLES
    missing_in_native = shim_shareable - db2_native_tables
    missing_in_shim = db2_native_tables - shim_shareable

    db1_only_in_db2_native = DB1_ONLY_TABLES & db2_native_tables
    db1_only_in_db2_shim = DB1_ONLY_TABLES & db2_shim_tables

    db1_allowed = DB1_ONLY_TABLES | DB1_OWNED_LEXICAL_INDEX
    db1_unexpected = db1_tables - db1_allowed

    issues = (
        missing_in_native
        or missing_in_shim
        or db1_only_in_db2_native
        or db1_only_in_db2_shim
        or db1_unexpected
    )

    if not issues:
        print(
            f"schema-sync: ok ({len(shim_shareable)} shared tables, "
            f"{len(DB1_ONLY_TABLES)} DB1-only tables)"
        )
        return 0

    if db1_unexpected:
        print("schema drift: non-DB1 tables present in src/db1/schema.sql:")
        for name in sorted(db1_unexpected):
            print(f"  - {name}")
    if db1_only_in_db2_native:
        print("schema drift: DB1-only tables present in src/db2/schema.sql:")
        for name in sorted(db1_only_in_db2_native):
            print(f"  - {name}")
    if db1_only_in_db2_shim:
        print("schema drift: DB1-only tables present in src/db2/schema_sqlite.sql:")
        for name in sorted(db1_only_in_db2_shim):
            print(f"  - {name}")
    if missing_in_native:
        print(
            "schema drift: tables in src/db2/schema_sqlite.sql but missing "
            "in src/db2/schema.sql:"
        )
        for name in sorted(missing_in_native):
            print(f"  - {name}")
    if missing_in_shim:
        print(
            "schema drift: tables in src/db2/schema.sql but missing "
            "in src/db2/schema_sqlite.sql:"
        )
        for name in sorted(missing_in_shim):
            print(f"  - {name}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
