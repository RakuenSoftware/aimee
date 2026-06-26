/* canonical_index.h: project/org-level code index, owned by aimee-kb.
 *
 * Storage: DB2 (Postgres). Schema is applied by db2_init via
 * db2/schema.sql. Tables: projects, files, terms, file_imports,
 * file_exports, code_calls, file_contents.
 *
 * Coordination: only aimee-kb writes these tables. Reads outside kb go
 * through the /v1 code-index API. Branch-local deltas for the current
 * worktree live in DB1 (see src/db1/branch_overlay.h).
 *
 * All functions return -1 if DB2 is not initialized (db2_init has not
 * been called or the Postgres connection is unavailable). Scan
 * functions are not internally serialized — call sites must hold the
 * scan coordinator slot in kb_service.
 */
#ifndef DEC_DB2_CANONICAL_INDEX_H
#define DEC_DB2_CANONICAL_INDEX_H 1

#include "index.h"

#ifdef __cplusplus
extern "C"
{
#endif

   /* Scan a project rooted at `root`, registering it as `name`.
    * Skips files unchanged since their stored scanned_at unless force=1.
    * Returns count of files (re)indexed, or -1 on error.
    *
    * If `inspected_out` is non-NULL, on success it receives the count of
    * files visited (after path/extension and binary-content filters)
    * — i.e. the universe the incremental decision ran over. The return
    * value is always <= *inspected_out; the difference is files
    * unchanged since their stored scanned_at. */
   int canonical_index_scan_project(const char *name, const char *root, int force,
                                    int *inspected_out);

   typedef struct
   {
      const char *rel_path;
      const char *content;
   } canonical_index_file_input_t;

   /* Index files whose content has already been read by the caller.
    * This is the remote/container-safe companion to scan_project: the
    * knowledge service stores code structure without reading root_label from
    * its local filesystem. Returns count indexed, or -1 on error. */
   int canonical_index_scan_files(const char *name, const char *root_label,
                                  const canonical_index_file_input_t *files, int file_count,
                                  int force, int *inspected_out);

   /* List all projects. Returns count, or -1 on error. */
   int canonical_index_list_projects(project_info_t *out, int max);

   /* Find an identifier across all projects. Returns count, or -1 on error. */
   int canonical_index_find(const char *identifier, term_hit_t *out, int max);

   /* Compute blast radius for a file. Returns 0 on success, -1 on error. */
   int canonical_index_blast_radius(const char *project, const char *file_path,
                                    blast_radius_t *out);

   /* Get definitions for a file (file structure). Returns count. */
   int canonical_index_structure(const char *project, const char *file_path, definition_t *out,
                                 int max);

   /* Find all callers of a symbol within a project. Returns count. */
   int canonical_index_find_callers(const char *project, const char *symbol, caller_hit_t *out,
                                    int max);

   /* Full-text code search across file_contents. |project| may be NULL/empty
    * to search all indexed projects. Returns count of hits, or -1 on error.
    * |enrich| non-zero locates each hit's matched line (ingress-compression P1b);
    * 0 keeps the query/cost identical to before. */
   int canonical_index_code_search(const char *query, const char *project, code_search_hit_t *out,
                                   int max, int enrich);

   /* Count indexed files and definition-kind terms for the project
    * named |project|. Either out pointer may be NULL. Both counts
    * default to 0 when the project has no rows. Returns 0 on success,
    * -1 if DB2 is unavailable. */
   int canonical_index_project_stats(const char *project, int *files_out, int *defs_out);

   /* Language breakdown for one project: JSON array of {lang, count} sorted
    * by count descending (up to 8 entries). Writes at most bufsz-1 bytes into
    * buf and NUL-terminates. Returns 0 on success, -1 if DB2 is unavailable. */
   int canonical_index_project_lang_breakdown(const char *project, char *buf, size_t bufsz);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_CANONICAL_INDEX_H */
