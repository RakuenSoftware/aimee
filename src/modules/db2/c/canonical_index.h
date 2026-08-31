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

#include "css_analyze.h"
#include "index.h"

#ifdef __cplusplus
extern "C"
{
#endif

   /* Host-owned process capability used only by scan_project's local filesystem
    * and git discovery. DB2 constructs a fixed, NULL-terminated argv for each
    * operation. The callback must execute argv[0] directly without a shell, put
    * at most max_out captured stdout/stderr bytes in one malloc-owned *out_buf,
    * and return the child exit code (negative for host failure). Environment
    * isolation is host policy: the current KB safe_exec_capture implementation
    * inherits the service environment but never places it in argv. Install once
    * at KB service initialization, before HTTP routing or ingest workers start.
    * Passing NULL disables local discovery (scan_files remains available). */
   typedef int (*canonical_index_exec_capture_fn)(const char *const argv[], char **out_buf,
                                                  size_t max_out);
   void canonical_index_set_exec_capture(canonical_index_exec_capture_fn capture);

   typedef css_stylesheet_t *(*db2_css_analyze_fn)(const char *text, size_t len);
   typedef void (*db2_css_stylesheet_free_fn)(css_stylesheet_t *stylesheet);
   typedef int (*db2_css_extract_class_tokens_fn)(const char *text, size_t len,
                                                  char (*out)[CSS_CLASS_TOKEN_MAX], int max);

   /* Internal declaration of the CSS host contract exported publicly through
    * <aimee/db2/host_contracts.h>. */
   void
   aimee_db2_register_css_analysis_providers(db2_css_analyze_fn analyze,
                                             db2_css_stylesheet_free_fn release,
                                             db2_css_extract_class_tokens_fn extract_class_tokens);

   /* Validate provider-owned output before the indexer uses it. Exposed for
    * focused boundary tests; scan paths use these same helpers. */
   css_stylesheet_t *canonical_index_css_analyze(const char *text, size_t len);
   void canonical_index_css_stylesheet_free(css_stylesheet_t *stylesheet);
   int canonical_index_css_extract_class_tokens(const char *text, size_t len,
                                                char (*out)[CSS_CLASS_TOKEN_MAX], int max);

   /* Scan a project rooted at `root`, registering it as `name`. The complete
    * collected manifest is sealed atomically; content hashes avoid rebuilding
    * unchanged file facts and absent indexed paths are retracted.
    * Returns count of files whose facts changed, or -1 on error.
    *
    * If `inspected_out` is non-NULL, on success it receives the count of
    * files visited (after path/extension and binary-content filters).
    * The return value is always <= *inspected_out; the difference is
    * content-hash-identical files. `force` is retained for wire compatibility;
    * correctness never depends on mtime. */
   int canonical_index_scan_project(const char *name, const char *root, int force,
                                    int *inspected_out);

   typedef struct
   {
      const char *rel_path;
      const char *content;
   } canonical_index_file_input_t;

   typedef struct
   {
      long long revision;
      int files_indexed;
      int files_retracted;
   } canonical_index_seal_result_t;

   /* Complete-manifest protocol. Stage writes are private to scan_id; seal
    * atomically publishes changed facts, retracts absent paths, and advances
    * the project's index revision. seal returns -2 for a stale/count-mismatched
    * session and -1 for storage failure. */
   int canonical_index_scan_begin(const char *name, const char *root_label, const char *scan_id,
                                  long long *baseline_revision_out);
   int canonical_index_scan_stage(const char *scan_id,
                                  const canonical_index_file_input_t *files, int file_count,
                                  int *accepted_out);
   int canonical_index_scan_seal(const char *scan_id, int expected_files,
                                 canonical_index_seal_result_t *out);
   int canonical_index_scan_abort(const char *scan_id);

#define CANONICAL_INDEX_VERIFY_EXAMPLES 16
   typedef struct
   {
      long long index_revision;
      int indexed_files;
      int workspace_files;
      int modified_files;
      int missing_files;
      int unindexed_files;
      int unavailable;
      int example_count;
      char examples[CANONICAL_INDEX_VERIFY_EXAMPLES][256];
   } canonical_index_verify_result_t;

   /* Read-only comparison of a current workspace to the published manifest.
    * deep=0 compares ownership paths; deep=1 also compares content hashes. */
   int canonical_index_verify_project(const char *name, const char *root, int deep,
                                      canonical_index_verify_result_t *out);

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

   /* Find an identifier inside one project. The project predicate is applied by the backing query
    * before its LIMIT, so unrelated namespaces cannot crowd out current-project hits. */
   int canonical_index_find_project(const char *project, const char *identifier, term_hit_t *out,
                                    int max);
   /* Search all current projects except |excluded_project|. The exclusion is
    * applied before LIMIT so a local-heavy corpus cannot crowd out the tail. */
   int canonical_index_find_excluding_project(const char *excluded_project, const char *identifier,
                                              term_hit_t *out, int max);

   /* Compute blast radius for a file. Returns 0 on success, -1 on error. */
   int canonical_index_blast_radius(const char *project, const char *file_path,
                                    blast_radius_t *out);

   /* Get definitions for a file (file structure). Returns count. */
   int canonical_index_structure(const char *project, const char *file_path, definition_t *out,
                                 int max);

   /* Find all callers of a symbol within a project. Returns count. */
   int canonical_index_find_callers(const char *project, const char *symbol, caller_hit_t *out,
                                    int max);
   int canonical_index_find_callers_excluding_project(const char *excluded_project,
                                                      const char *symbol, caller_hit_t *out,
                                                      int max);

   /* Full-text code search across file_contents. |project| may be NULL/empty
    * to search all indexed projects. Returns count of hits, or -1 on error.
    * |enrich| non-zero locates each hit's matched line (ingress-compression P1b);
    * 0 keeps the query/cost identical to before. */
   int canonical_index_code_search(const char *query, const char *project, code_search_hit_t *out,
                                   int max, int enrich);
   int canonical_index_code_search_excluding_project(const char *query,
                                                     const char *excluded_project,
                                                     code_search_hit_t *out, int max, int enrich);

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
