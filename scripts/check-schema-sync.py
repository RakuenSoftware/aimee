#!/usr/bin/env python3
"""Verify the DB2 shim schema and the DB2 native schema cover the same set
of tables, and that the store's schema only contains DB1-owned tables.

The store's schema is server-go/modules/aimee/families/schema_*.sql, one file
per family, since DB1 became a Go module.

After the 3db split, the DB2 shim schema used by tests lives in
src/modules/db2/c/schema_sqlite.sql; production DB2 uses src/modules/db2/c/schema.sql.
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
# The store's schema, one file per family since it became a Go module. Was a
# single src/modules/db1/schema.sql.
STORE_SCHEMA_DIR = ROOT / "server-go" / "modules" / "aimee" / "families"
DB2_SCHEMA_PG = ROOT / "src" / "modules" / "db2" / "c" / "schema.sql"
DB2_SCHEMA_SQLITE = ROOT / "src" / "modules" / "db2" / "c" / "schema_sqlite.sql"

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
    # pki_certs and pki_mtls_ramp were created lazily by pki_store.c with
    # CREATE TABLE IF NOT EXISTS and never appeared in a schema file, so this
    # check could not see them. The Go store declares them, so it can.
    "pki_certs",
    "pki_mtls_ramp",
    # economizer_state was a labelled row in `checkpoints` in the C store and is
    # its own table in the Go one -- only the newest row per session is ever
    # read, which is a table with a primary key, not a log to filter.
    "economizer_state",
    "agent_cache",
    "agent_jobs",
    "agent_log",
    "branch_ownership",
    "session_feature_branch",
    "clarify_qa",
    "clarify_sessions",
    "checkpoints",
    "coord_job_tasks",
    "coord_jobs",
    "context_cache",
    # fetched web pages, stripped to text, keyed by canonical URL. Purely a
    # local runtime cache: never replicated, never shared, safe to drop.
    "web_page_cache",
    "context_snapshots",
    # Persisted retrieval hysteresis. The turn counter and firing events stay
    # with the DB1-owned conversation rather than resetting with a C process.
    "context_activation_turns",
    "context_activation_events",
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
    "approach_failures",
    "eval_candidates",
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
    "model_pricing",
    "pipelines",
    "plan_steps",
    "primary_sessions",
    # setup-wizard first-owner claim and certificate-bound bootstrap grant
    # (per-appliance authorization state, enforced by aimee-server)
    "remote_first_user",
    "remote_client_grants",
    "project_clones",
    "server_sessions",
    "server_mgmt_nonce",
    "server_mgmt_status_hwm",
    "server_management_jti",
    # The data-plane sibling of server_management_jti: per-server single-use
    # rejection for kb-signed identity tokens (proposal
    # per-user-remote-writes-authz.md §9).  A separate table because that one's
    # peer/request columns are NOT NULL and an identity token has neither.
    # Replay state is per-server runtime, never replicated, so it is DB1-local
    # for the same reason the management one is.
    "server_identity_jti",
    "server_management_jwks_cache",
    "session_state",
    "user_memories",
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
    "wc_channel_messages",
    "wc_channels",
    "ensembles",
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
    # The rest of the workflow engine's own tables. These were created by the Go
    # WFE against DB1's file rather than declared here, which is why they are a
    # late addition to a list that is otherwise as old as the tables: DB1 owned
    # the store but not the schema, and the drift did not show up because
    # nothing declared them to drift from.
    "wfe_convergence",
    "wfe_frozen_create",
    "lifecycle_delegate_job",
    # primary-as-manager: interactive session <-> work-item binding (per-user, DB1-owned)
    "workflow_binding",
    # webchat tab -> Claude --resume id binding (per-user, DB1-owned)
    "webchat_claude_sessions",
    # live in-flight webchat turn mirror the browser polls (per-session, DB1-owned)
    "webchat_live",
    # intercepted agent harness-memory, canonical store (per-project, DB1-owned)
    "harness_memory",
}

# Sole DB1-owned lexical index. Lives only in db1/schema.sql.
DB1_OWNED_LEXICAL_INDEX = {"window_fts"}


def extract_tables(path: Path) -> set[str]:
    text = path.read_text()
    # Strip -- comments (single-line only; good enough for our schemas).
    text = re.sub(r"--[^\n]*", "", text)
    return {(m.group(1) or m.group(2)).lower() for m in TABLE_RE.finditer(text)}


def main() -> int:
    store_schemas = sorted(STORE_SCHEMA_DIR.glob("schema_*.sql"))
    if not store_schemas:
        print(f"schema-sync: no store schema files under {STORE_SCHEMA_DIR}")
        return 1
    db1_tables = set()
    for schema in store_schemas:
        db1_tables |= extract_tables(schema)
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
        print(f"schema drift: non-DB1 tables present in {STORE_SCHEMA_DIR}/schema_*.sql:")
        for name in sorted(db1_unexpected):
            print(f"  - {name}")
    if db1_only_in_db2_native:
        print("schema drift: DB1-only tables present in src/modules/db2/c/schema.sql:")
        for name in sorted(db1_only_in_db2_native):
            print(f"  - {name}")
    if db1_only_in_db2_shim:
        print("schema drift: DB1-only tables present in src/modules/db2/c/schema_sqlite.sql:")
        for name in sorted(db1_only_in_db2_shim):
            print(f"  - {name}")
    if missing_in_native:
        print(
            "schema drift: tables in src/modules/db2/c/schema_sqlite.sql but missing "
            "in src/modules/db2/c/schema.sql:"
        )
        for name in sorted(missing_in_native):
            print(f"  - {name}")
    if missing_in_shim:
        print(
            "schema drift: tables in src/modules/db2/c/schema.sql but missing "
            "in src/modules/db2/c/schema_sqlite.sql:"
        )
        for name in sorted(missing_in_shim):
            print(f"  - {name}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
