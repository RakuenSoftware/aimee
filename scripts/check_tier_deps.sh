#!/usr/bin/env bash
set -euo pipefail

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
cd "$ROOT"

status=0
scan() {
  local pattern=$1
  local description=$2
  local matches
  local file

  matches=""
  while IFS= read -r -d '' file; do
    case "$file" in
      src/db1/*|src/db2/*|src/tests/*) continue ;;
      src/*) ;;
      *) continue ;;
    esac

    current="$(rg -n -e "$pattern" -- "$file" || true)"
    if [[ -n "$current" ]]; then
      matches="${matches}${matches:+$'\n'}${current}"
    fi
  done < <(rg --files -z src -g '*.c' -g '*.h' -g '*.inc')

  if [[ -n "$matches" ]]; then
    echo "tier-dep violation: $description" >&2
    echo "$matches" >&2
    status=1
  fi
}

path_scan() {
  local pattern=$1
  local description=$2
  shift 2
  local matches
  local existing=()
  local path

  if [[ $# -eq 0 ]]; then
    return
  fi

  for path in "$@"; do
    if [[ -e "$path" ]]; then
      existing+=("$path")
    fi
  done
  if [[ ${#existing[@]} -eq 0 ]]; then
    return
  fi

  matches="$(rg -n -e "$pattern" -- "${existing[@]}" || true)"
  if [[ -n "$matches" ]]; then
    echo "tier-dep violation: $description" >&2
    echo "$matches" >&2
    status=1
  fi
}

public_header_scan() {
  local pattern=$1
  local description=$2
  local matches

  matches="$(rg -n -e "$pattern" -- src/db2/db2.h src/db2/lifecycle.h || true)"
  if [[ -n "$matches" ]]; then
    echo "tier-dep violation: $description" >&2
    echo "$matches" >&2
    status=1
  fi
}

scan '^\\s*#\\s*include\\s*<sqlite3\\.h>' "sqlite3 include outside src/db1"
scan '^\\s*#\\s*include\\s*<libpq-fe\\.h>' "libpq include outside src/db2"
scan '"db2/db_postgres\\.h"' "db2 internal postgres header included outside src/db2"
scan 'aimee_stores_' "aimee_stores symbol outside tier ownership"
scan '\\baimee_stores_t\\b' "aimee_stores_t symbol outside tier ownership"
scan 'PGconn' "Postgres connection symbol outside src/db2"
scan '\\bPGresult\\b' "Postgres result symbol outside src/db2"
scan '\\bPQ[A-Z][A-Za-z0-9_]*\\(' "Postgres helper API outside src/db2"
scan 'sqlite3_' "SQLite symbol outside src/db1"
path_scan '"sqlite3"' \
  "agent policy exposes SQLite CLI capability outside DB1" \
  src/agent_policy.c
scan 'project_store_' "legacy project-store lifecycle alias outside src/db2"
scan 'project_store_lifecycle\\.h' "legacy project-store lifecycle header outside src/db2"
scan '\\bdatabase_backend\\b' "legacy DB2 backend selector outside src/db2"
path_scan '\\bdatabase_(url|pool_size)\\b' \
  "legacy vague DB2 config keys outside src/db2" \
  src/modules/config/config.c src/modules/config/config_database.c src/modules/config/config.h src/cmd_data.c src/cmd_doctor.c \
  src/kb/kb_main.c src/tests/test_config.c
path_scan '\\bdb_path\\b' \
  "legacy vague DB1 config key in config surfaces" \
  src/modules/config/config.c src/modules/config/config_save.c src/modules/config/config.h src/cmd_core.c \
  src/headers/commands.h src/headers/cmd_hooks_platform.h src/tests/test_config.c \
  src/tests/fuzz_config_load.c
path_scan '\\bworkspace_root\\b' \
  "legacy workspace_root config shim" \
  src/modules/config/config.c src/modules/config/config_save.c src/modules/config/config.h src/tests/test_config.c \
  src/tests/fuzz_config_load.c
path_scan '\\bdb2_(open|close)_shared_store\\b' \
  "aimee-kb uses legacy DB2 shared-store lifecycle" \
  src/kb/kb_main.c
path_scan '\\bdb2_(init|shutdown)\\b' \
  "aimee-kb request paths manage DB2 lifecycle outside daemon main" \
  src/kb/kb.c src/kb/kb_service.c src/modules/kb_client/kb_client.c src/cmd_kb.c
path_scan 'DB2_FORK_SPEC_SHARED_STORE|DB2_FORK_SPEC_POSTGRES|db2_child_reopen_shared_store|db2_child_close_shared_store|db2_(open|close)_shared_store' \
  "non-DB2 code exposes legacy DB2 shared-store fork lifecycle" \
  src/posix/memory.c src/posix/cmd_hooks.c src/windows/cmd_hooks.c src/db2/db2.h src/db2/lifecycle.h
scan '\\bdb2_(shared_sqlite|register_shared_sqlite|open_shared_sqlite|close_shared_sqlite)\\b' \
  "DB2 transitional SQLite lifecycle outside src/db2"
public_header_scan '\\bdb2_(shared_sqlite|register_shared_sqlite|open_shared_sqlite|close_shared_sqlite)\\b' \
  "DB2 public lifecycle header exposes SQLite shim primitive"
public_header_scan '\\b[Ss][Qq][Ll][Ii][Tt][Ee]3?\\b' \
  "DB2 public lifecycle header exposes SQLite backend knowledge"
scan '\\bdb2_is_ephemeral\\b' \
  "DB2 backend-mode probe outside src/db2"
public_header_scan '\\bdb2_is_ephemeral\\b' \
  "DB2 public lifecycle header exposes backend-mode probe"
path_scan '\\bdb2_(open|close)_ephemeral_store\\b' \
  "legacy DB2 ephemeral-store lifecycle exposed outside DB2" \
  src/modules/agent_eval/agent_eval_memory_support.c src/db2/db2.h src/db2/lifecycle.h
scan '\\b(DB2_FORK_SPEC_SHIM|db2_shim_|db2_is_shim|db2_pg_url)\\b' \
  "DB2 shim lifecycle API outside src/db2"
public_header_scan '\\b(DB2_FORK_SPEC_SHIM|db2_shim_|db2_is_shim|db2_pg_url|[Ss][Hh][Ii][Mm])\\b' \
  "DB2 public lifecycle header exposes shim backend vocabulary"
scan '\\bdb2_(open|close)_legacy_shared_store' "DB2 shared-store lifecycle alias outside src/db2"
path_scan 'legacy_state_path|load_legacy_state_file|session-[^[:space:]]+\\.state' \
  "legacy file-backed session state migration path" \
  src/session_state.c src/modules/guardrails/guardrails.h
path_scan 'legacy db handle|server-side db handle|legacy DB1 tables' \
  "legacy database-handle vocabulary outside tier internals" \
  src/cmd_index.c src/tasks.c src/cmd_memory_core.c src/headers/memory.h
path_scan 'server-side database handle|database handle|db handle|DB handle|DB handles|database handles|DB1 tool registry|Collaborative rules live in DB1|DB1'\''s rules table|Per-connection DB handles|shared DB handle' \
  "caller-owned DB handle vocabulary or wrong tier ownership comments" \
  docs/BENCHMARKS.md src/README.md src/agent_coord.c src/modules/agent_eval/agent_eval.c \
  src/agent_tasks.c src/headers/agent_coord.h \
  src/headers/agent_tasks.h src/headers/aimee.h src/headers/commands.h \
  src/posix/cmd_hooks.c
scan '\\bdb2_tx_' "DB2 transaction primitive outside src/db2"
scan '\\bparse_sqlite_utc\\b|legacy sqlite-FTS5|SQLite-side|SQLite-backed stores|SQLite PRAGMAs|SQLite FTS5|sqlite wallclock' \
  "SQLite-named legacy/helper vocabulary outside src/db1"
scan '\\bdb1_window_fts_(add|search|available)\\b|\\bdb1_window_fts_hit_t\\b' \
  "non-DB1 callers expose DB1 lexical-index implementation names"
path_scan 'pm_build_fts_match|db2_prospective_list_by_fts|pm_match_clause_to_tsquery|memory_negation_fts|FTS5 prefix matching|FTS5 noise|FTS5 reserved-char|FTS5 indexing|expand_terms_for_fts|pre-built FTS5 MATCH|FTS5 index' \
  "prospective/negation helpers expose backend-specific lexical index names" \
  src/modules/memory/memory_prospective.c src/modules/memory/memory_core_search.inc src/modules/config/config.h \
  src/headers/util.h src/text.c src/tests/test_text.c \
  src/db2/prospective_memories.h src/db2/prospective_memories.c
path_scan 'memory_collect_fts_via_vector|MEM_SOURCE_FTS|FTS string|FTS query string|FTS/graph|unit/fts|generic FTS path|memory FTS|FTS5 / semantic|pre-DB3 FTS|db2_memory_collect_fts_matches|memory_units_fts MATCH|memory_negation_fts FTS table' \
  "memory recall helpers expose stale FTS collector names" \
  src/modules/memory/memory_core_search.inc src/db2/memory_query.h src/db2/memory_query.c
path_scan 'fts_search_via_vector|MAX_FTS_RESULTS|fts_res|n_fts|fts_weight|weights: fts|"fts"|ed_build_fts_match|db2_directive_match_by_fts|FTS over question|FTS match on question|alpha\*FTS|FTS and vector|FTS_OR|ED_FTS' \
  "KB/directive/query-plan surfaces expose stale FTS vocabulary" \
  src/kb/kb.c src/modules/config/config.h src/headers/memory.h src/headers/aimee.h \
  src/modules/memory/memory_directives.c src/cmd_memory_core.c src/modules/memory/memory_core_search.inc \
  src/db2/epistemic_directives.h src/db2/epistemic_directives.c src/tests/test_kb.c
path_scan 'use sqlite|sqlite WAL' \
  "SQLite-named compute comments outside src/db1" \
  src/server/server.c
path_scan '\\bsqlite_(memories|units|chunks)\\b' \
  "SQLite-named vector verify row-count fields" \
  src/kb/kb_service.c src/modules/kb_client/kb_client.h
path_scan '\\b([Pp]ostgres|pg_trgm|libpq)\\b' \
  "Postgres-named doctor DB surface outside src/db2" \
  src/cmd_doctor.c
path_scan '\\b(Postgres|libpq|pg_trgm)\\b' \
  "Postgres-named DB2 implementation comments outside src/db2" \
  src/cmd_index.c src/kb/kb_main.c src/dashboard.c src/cmd_work.c src/kb/kb.c \
  src/modules/memory/memory_core_search.inc src/modules/config/config.h src/db1/db_schema.c src/db1/db_schema.h
path_scan 'system-provided SQLite|SQLite database \(all state\)|`db\.c`[[:space:]]*\|[[:space:]]*SQLite|shared Postgres tier|sqlite\.sql and postgres\.sql' \
  "source docs/build text exposes legacy storage ownership" \
  src/README.md src/Makefile
path_scan 'Memory<br/>L0-L3, FTS5|memories table<br/>FTS5|4-tier memory \(L0-L3\), CRUD, FTS5 search|Queries the `memories_fts` FTS5 table|memory_find_facts\(\) \(FTS5 \+ DB3 dense recall|Term Match<br/>exact lowercase match<br/>in window_terms|FTS5 Match<br/>stemmed search<br/>in window_fts|Conversation: windows, decisions, window_terms, window_files, window_fts|Memory:[[:space:]]+memories, memories_fts' \
  "source README exposes stale DB2 memory-as-SQLite documentation" \
  src/README.md
path_scan '`rules\.c`[[:space:]]*\|[[:space:]]*Rule storage|`feedback\.c`[[:space:]]*\|[[:space:]]*Feedback recording|`working_memory\.c`|`tasks\.c`[[:space:]]*\|[[:space:]]*Task graph, decisions, checkpoints|`db\.c` \(1492\)|`memory_promote\.c`|`memory\.c`[[:space:]]*\|[[:space:]]*1276|`memory_context\.c`|`memory_graph\.c`|`db_migrations\.c`|rules_generate\(db\)|memories L2 LIKE prompt|index_find\(\)|in memories table\?|tasks table' \
  "source README exposes stale pre-split source/API names" \
  src/README.md
path_scan 'Single SQLite database|Memory<br/>4-tier, FTS5 search|Memory search \(FTS5\)|SQLite3 \(with FTS5\)|creates the database' \
  "root README exposes legacy single-store ownership" \
  README.md
path_scan 'with FTS5|Qdrant (memory|KB) index|SQLite-backed memory state' \
  "command docs expose backend-specific storage internals" \
  docs/COMMANDS.md
path_scan 'search_memory[^\\n]*(full-text search|Full-text search)|full-text search on stored facts|Full-text search of stored facts' \
  "agent/MCP docs expose memory search as full-text-only" \
  docs/agent.md docs/COMMANDS.md
path_scan 'Full-text search across all memories|FTS5 syntax|FTS5 code search|Uses FTS5 BM25|hybrid retrieval \(FTS5|Search — hybrid FTS5|FTS5 \+ DB3' \
  "non-tier command/header text exposes backend-specific lexical search" \
  src/cmd_memory.c src/agent_tools.c src/kb/kb.c src/headers/index.h src/headers/kb.h
path_scan 'FTS5 is required for memory full-text search|full-text memory search|Memory full-text search is unavailable without FTS5|Memory FTS5 full-text search|db\.c \(migration 28\), memory\.c' \
  "top-level status/compat docs assign DB2 memory search to DB1 SQLite" \
  docs/COMPATIBILITY.md docs/STATUS.md
path_scan 'agent\.c, db\.c|cmd_memory\.c, memory\.c|database-backed state|\\bdb\.c\\b|memory_promote\.c|mcp_server\.c|State and storage\\ndb\.c, memory\.c|statically linked core runtime \+ data' \
  "status docs expose stale pre-split storage source names" \
  docs/STATUS.md
path_scan 'Memory search \(FTS5\)|Full-text search on memories table|memories table via FTS5|FTS5 `MATCH`|reopening the database|database-backed paths|database-backed and context-assembly' \
  "benchmark docs expose stale DB2 memory-as-SQLite latency model" \
  docs/BENCHMARKS.md
path_scan 'memory\.c: 4-tier|^/\* memory\.c: POSIX|^/\* memory\.c: Windows|private declarations for memory\.c platform split|posix/memory\.c \(POSIX\)|windows/memory\.c \(Windows stubs\)|memory_scan_content is implemented in posix/memory\.c|Add error handling to src/memory\.c' \
  "source comments/examples expose stale memory.c source names" \
  src/modules/memory/memory_core.c src/modules/memory/memory_core_crud.inc src/cmd_session_lifecycle.c \
  src/modules/memory/memory_platform.h src/posix/memory.c src/windows/memory.c
path_scan 'sqlite db postgres storage sql|"postgres"' \
  "memory retrieval hints expose backend product names outside tier modules" \
  src/modules/memory/memory_core_search.inc src/modules/memory/memory_core_scope_embed.inc
path_scan 'legacy no-gate behaviour|legacy DBs may|legacy edge|legacy rows|/\* legacy \*/' \
  "memory source comments expose legacy compatibility labels" \
  src/modules/memory/memory_core_tiers.inc src/modules/memory/memory_core_crud.inc src/modules/memory/memory_episodes.c \
  src/modules/memory/memory_ontology.h
path_scan 'Untagged memories \(legacy\)|legacy promote/demote cycle|legacy hybrid|legacy `symbols` table' \
  "source comments expose legacy storage/route labels" \
  src/modules/memory/memory_assemble.c src/modules/memory/memory_maintenance.c src/modules/config/config.h src/cmd_doctor.c
path_scan 'legacy in-repo|legacy behavior|legacy: 1|legacy prospective-only|legacy MCP server|legacy forward path|pre-concurrent legacy behavior' \
  "active source comments expose legacy runtime labels" \
  src/agent_policy.c src/git_verify.c src/cmd_data.c src/agent_runtime.c \
  src/mcp_tools.c
if [[ -e src/headers/memory_curiosity.h ]]; then
  echo "tier-dep violation: legacy DB2 curiosity re-export header is present" >&2
  status=1
fi
path_scan 'DB\[SQLite\]|SQLite-backed local state|SQLite attack surfaces' \
  "security docs expose backend-specific DB1 storage internals" \
  docs/SECURITY.md
if [[ -e scripts/migrate-sqlite-to-postgres.sh ]]; then
  echo "tier-dep violation: legacy SQLite-to-DB2 migration script is present" >&2
  status=1
fi
if [[ -e src/schema/postgres.sql ]]; then
  echo "tier-dep violation: DB2 Postgres schema must live under src/db2" >&2
  status=1
fi
if [[ -e src/schema/sqlite.sql ]]; then
  echo "tier-dep violation: DB1 SQLite schema must live under src/db1" >&2
  status=1
fi
path_scan 'AIMEE_SCHEMA_SQLITE_SQL|AIMEE_SCHEMA_POSTGRES_SQL' \
  "generated schema constants expose backend product names" \
  src/gen_schema.py src/db1/db_schema.c src/db2/db_schema.c
path_scan 'SQLITE_ONLY_FTS_TABLES|\\bSQLITE\\b|\\bPOSTGRES\\b|sqlite-only|SQLite-only|Postgres-only|SQLite|Postgres|FTS-mirror|FTS5 virtual tables' \
  "schema-sync helper exposes backend product vocabulary" \
  scripts/check-schema-sync.py
path_scan 'DATABASE_BACKENDS|Database backends|SQLite → Postgres|SQLite -> Postgres|A single `aimee` binary runs against either SQLite or Postgres|database_backend|database_url.*postgres' \
  "top-level docs expose legacy selectable database-backend architecture" \
  README.md docs/*.md
path_scan 'legacy DB abstraction|Subsequent PRs migrate one DB1 subsystem|Remaining DB1 cleanup|DB2 migration: blocked|Postgres-only bench|today'\''s monolith|no legacy DB abstraction call-site changes|Pin-backends is the current PR chain|current PR chain on main|current PR chain|pin-backends chain|while the chain runs|DB2 migration' \
  "top-level docs expose stale storage migration roadmap" \
  docs/ROADMAP.md docs/PROPOSALS.md
path_scan 'database_backend|backend: sqlite|database_url.*postgres|A single aimee binary can run either backend|revert to.*db_path' \
  "examples expose legacy selectable database-backend architecture" \
  $(find examples -type f 2>/dev/null || true)
path_scan 'Postgres backend \(libpq\)|SQLite continues to work as the default|string constants for the SQLite and Postgres layers' \
  "CMake exposes legacy storage ownership" \
  CMakeLists.txt
path_scan '\$\{AIMEE_SRC_DIR\}/(version_notifier|memory_curiosity|rules|feedback|workflow_session|working_memory|secret_store|notes|collab_rules|agent_diagnose|agent_clarify|file_snapshot)\.c|\$\{AIMEE_SRC_DIR\}/db2/git_ownership\.c' \
  "CMake lists pre-split storage source paths" \
  CMakeLists.txt
path_scan 'SQLite datetime\(\) compatibility shims|written against SQLite|SQLite'\''s julianday|See note in sqlite\.sql|mirrors of SQLite FTS5|sqlite\.sql\).*MATCH|managed Postgres' \
  "DB2 schema comments expose legacy SQLite/Postgres migration framing" \
  src/db2/schema.sql
path_scan 'pre-cutover sqlite|sqlite native|sqlite tolerates|sqlite needed|sqlite_changes|sqlite strftime|sqlite \(shim\)|legacy sqlite path|sqlite-side shim|sqlite-style|sqlite-FTS5|pm_fts5_to_tsquery|sqlite helpers|sqlite is lenient|postgres CREATE FUNCTION shim' \
  "DB2 module comments expose legacy SQLite migration framing" \
  src/db2/memory_query.c src/db2/memory_promotion.c src/db2/kb_service_backend.c \
  src/db2/code_index.c src/db2/memory_entity_graph.c src/db2/memory_lifecycle.c \
  src/db2/kb_runtime_state.c src/db2/prospective_memories.c src/db2/memory_relations.c \
  src/db2/memory_row_mapper_pg.c
path_scan 'runs either SQLite or Postgres|schema/\{sqlite,postgres\}|Translate SQLite|rewrite_sqlite_(upsert|fts)|FTS5|SQLite-flavoured|SQLite positional placeholder|Postgres equivalents|Postgres path|SQLite path|sqlite-backed|real-postgres' \
  "DB2 provider comments expose legacy backend-normalization vocabulary" \
  src/db2/db_postgres.c src/db2/db_postgres.h
scan '/collections/' "Qdrant collection URL path in src/ (pgvector is in-process, no HTTP)"
scan '/points/' "Qdrant points URL path in src/ (pgvector is in-process, no HTTP)"

if [[ "$status" -ne 0 ]]; then
  echo "run: ./scripts/check_tier_deps.sh failed" >&2
  exit 1
fi

echo "scripts/check_tier_deps.sh: PASS"
