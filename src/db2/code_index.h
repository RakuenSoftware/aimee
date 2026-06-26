/* db2/code_index.h: code-index primitives over the projects/files/terms
 * tables. Owns SQL for the indexer pipeline that scans repos and lets
 * `aimee index find` resolve symbols. The pure-domain types
 * (project_info_t, term_hit_t, ...) live in headers/index.h. */
#ifndef DEC_DB2_CODE_INDEX_H
#define DEC_DB2_CODE_INDEX_H 1

#include "../headers/index.h"

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* List rows from the projects table in name-ASC order, capped at
    * |max| (caller's buffer size). On success returns count (>=0). On
    * DB / connection error returns -1. */
   int db2_code_index_project_list(project_info_t *out, int max);

   /* Total row count of the projects table. Returns 0 on miss / DB
    * unavailable. */
   int db2_code_index_project_count(void);

   /* Most recent scanned_at timestamp across all rows in the projects
    * table, written to |out| (NUL-terminated, ISO-8601). Returns 0 on
    * success (out filled, may be empty if no rows), -1 on DB /
    * connection error / bad args. */
   int db2_code_index_project_last_scan(char *out, size_t cap);

   /* Find every (project, file, line, kind) where a term named
    * |identifier| exists. Definition rows sort first, then
    * (project_name, file_path) ASC. Capped at |max|. Returns count
    * (>=0) on success, -1 on DB / connection error. */
   int db2_code_index_term_find(const char *identifier, term_hit_t *out, int max);

   /* Fill out->dependents and out->dependencies for the given
    * (project, file_path) by reading file_exports and file_imports.
    * out->file is left untouched (caller stamps it). The caller
    * is responsible for memset()ing |out| beforehand. Returns 0 on
    * success, -1 on DB / connection / project-not-found error. The
    * file may exist in projects without a row in `files` (never
    * scanned) — in that case dependent_count and dependency_count
    * stay 0 and the call still returns 0. */
   int db2_code_index_blast_radius(const char *project, const char *file_path, blast_radius_t *out);

   /* List terms with kind='definition' for the (project, file_path),
    * ordered by line. Capped at |max|. Returns count (>=0) on
    * success, 0 if the project or file is unknown, -1 on DB /
    * connection error. */
   int db2_code_index_file_definitions(const char *project, const char *file_path,
                                       definition_t *out, int max);

   /* List call sites where |callee| matches |symbol|. If |project|
    * is non-empty, restrict to that project. Capped at |max|.
    * Returns count (>=0) on success, -1 on DB / connection error. */
   int db2_code_index_callers_find(const char *project, const char *symbol, caller_hit_t *out,
                                   int max);

   /* Upsert a row in projects keyed by name; sets root and stamps
    * scanned_at = now. Returns the project's id (>0) on success, -1
    * on DB / connection error. */
   int64_t db2_code_index_project_upsert(const char *name, const char *root);

   /* Upsert a row in files keyed by (project_id, path), stamping
    * scanned_at = |scanned_at|. Returns the file's id (>0) on
    * success, -1 on DB / connection error. */
   int64_t db2_code_index_file_upsert(int64_t project_id, const char *rel_path,
                                      const char *scanned_at);

   /* Returns 1 if the file at (project_id, rel_path) is unscanned or
    * was scanned strictly before |mtime|, 0 if scanned_at >= mtime. On
    * DB / connection error returns 1 (treat as modified, conservative
    * for a scanner). */
   int db2_code_index_file_modified_since(int64_t project_id, const char *rel_path, time_t mtime);

   /* Delete file rows in |project_id| whose path matches the SQL LIKE
    * pattern |path_glob|. CASCADE drops terms / file_exports /
    * file_imports / code_calls / file_contents automatically (FKs in
    * schema.sql). Returns rows deleted (>=0), or -1 on DB / connection
    * error. */
   int db2_code_index_purge_files_matching(int64_t project_id, const char *path_glob);

   /* One-shot cleanup of hidden-path pollution across the whole code
    * index. Drops every files row whose project-relative path has any
    * hidden component (dotfile or dot-directory) and every projects row
    * whose root path traverses a hidden segment. Hidden paths are dotfile
    * state — not source — and are never ingested. The scanner enforces the
    * same rule at scan-time; this purge cleans up rows from projects that
    * registered before that guard. Returns total rows deleted (files +
    * projects), or -1 on DB / connection error. Safe to call repeatedly;
    * a no-op once the index is clean. */
   int db2_code_index_purge_hidden_pollution(void);

   /* Bundle of per-file derived data passed to db2_code_index_file_replace. */
   typedef struct
   {
      const char *content;
      char **exports;
      int export_count;
      char **imports;
      int import_count;
      char **routes;
      int route_count;
      const definition_t *definitions;
      int definition_count;
      const call_ref_t *calls;
      int call_count;
   } code_index_file_data_t;

   /* Atomically replace per-file index data: deletes existing
    * file_exports / file_imports / terms / code_calls rows for the
    * file_id, upserts file_contents, and reinserts the supplied
    * exports / imports / routes (terms with kind='route') /
    * definitions (terms with kind from definition_t::kind) / calls.
    * Returns 0 on success, -1 on DB / connection error. */
   int db2_code_index_file_replace(int64_t file_id, const code_index_file_data_t *data);

   /* Full-text search across file_contents. |query| is plain text
    * (FTS5 / plainto_tsquery semantics); empty / NULL returns 0. If
    * |project| is non-empty, restricts to that project. Capped at
    * |max|. Returns count (>=0) on success, -1 on DB / connection
    * error. When |enrich| is non-zero, also locates each hit's matched line
    * (out[].line) from file content; 0 leaves line=0 and keeps the query/cost
    * identical to before (ingress-compression P1b). */
   int db2_code_index_code_search(const char *query, const char *project, code_search_hit_t *out,
                                  int max, int enrich);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_CODE_INDEX_H */
