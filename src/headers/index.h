#ifndef DEC_INDEX_H
#define DEC_INDEX_H 1

#include "aimee.h" /* MAX_PATH_LEN, MAX_FILE_SIZE, ... */

typedef struct
{
   char name[128];
   char root[MAX_PATH_LEN];
   char scanned_at[32];
} project_info_t;

typedef struct
{
   char path[MAX_PATH_LEN];
   char purpose[512];
} file_info_t;

typedef struct
{
   char file[MAX_PATH_LEN];
   char dependents[64][MAX_PATH_LEN];
   int dependent_count;
   char dependencies[64][MAX_PATH_LEN];
   int dependency_count;
} blast_radius_t;

typedef struct
{
   char project[128];
   char file_path[MAX_PATH_LEN];
   int line;     /* 1-based definition (start) line */
   int line_end; /* 1-based last line of the symbol body (>= line; 0 if unknown) */
   char kind[32];
} term_hit_t;

typedef struct
{
   char name[128];
   char kind[32];
   int line;     /* 1-based definition (start) line */
   int line_end; /* 1-based last line of the symbol body (>= line; 0 if unknown) */
} definition_t;

typedef struct
{
   char caller[128]; /* calling function name (empty if at file scope) */
   char callee[128]; /* called function/method name */
   int line;
} call_ref_t;

typedef struct
{
   char project[128];
   char file_path[MAX_PATH_LEN];
   char caller[128];
   int line;
} caller_hit_t;

/* Scan a project directory. If force is non-zero, re-index all files regardless of mtime. */
int index_scan_project(const char *name, const char *root, int force);

/* List all projects. Returns count. */
int index_list_projects(project_info_t *out, int max);

/* Find an identifier across all projects. Returns count. */
int index_find(const char *identifier, term_hit_t *out, int max);

/* Get blast radius for a file. */
int index_blast_radius(const char *project, const char *file_path, blast_radius_t *out);

/* Blast radius preview for multiple files. Returns JSON (caller frees).
 * Aggregates blast radii, classifies severity, generates warnings. */

/* Get file structure (definitions). Returns count. */
int index_structure(const char *project, const char *file_path, definition_t *out, int max);

/* Check if a file extension has a registered extractor. */
int index_has_extractor(const char *ext);

/* Extract imports from file content. Returns count. Caller frees each string. */
int extract_imports(const char *ext, const char *content, char **out, int max);

/* As extract_imports, but also fills `sys[i]` = 1 when out[i] is a system /
 * angle-bracket include (C/C++ `#include <...>`), else 0. `sys` may be NULL (then
 * behaves exactly like extract_imports). `sys` must hold at least `max` ints. */
int extract_imports_sys(const char *ext, const char *content, char **out, int *sys, int max);

/* Extract exports from file content. Returns count. Caller frees each string. */
int extract_exports(const char *ext, const char *content, char **out, int max);

/* Extract route definitions. Returns count. Caller frees each string. */
int extract_routes(const char *ext, const char *content, char **out, int max);

/* Extract definitions with line numbers. Returns count. Each definition's
 * line_end is filled with the last line of its body (brace-match for C-family,
 * indentation for Python; falls back to the start line when undetermined). */
int extract_definitions(const char *ext, const char *content, definition_t *out, int max);

/* Last line (1-based) of the definition that starts at `start_line` (1-based)
 * in `content`, by language family: brace-depth for C-like languages,
 * indentation for Python. Returns start_line when the span can't be
 * determined. Pure (no I/O); the code index stores only the start line, so the
 * span is computed here at index time. */
int code_def_end_line(const char *content, int start_line, const char *ext);

/* Extract function calls with caller context and line numbers. Returns count. */
int extract_calls(const char *ext, const char *content, call_ref_t *out, int max);

/* Find all callers of a given symbol across a project. Returns count. */
int index_find_callers(const char *project, const char *symbol, caller_hit_t *out, int max);

/* Code search result */
typedef struct
{
   char project[128];
   char file_path[MAX_PATH_LEN];
   char snippet[512];
   double rank;
   /* files.hash of the matched file — auditable-correctness P2 Layer-1
    * (file-level provenance): lets a citation name the exact content version and
    * lets drift detection flag a hit whose live source no longer matches its
    * stored hash. Empty when the row has no recorded hash. */
   char content_hash[80];
   /* 1-based line of the match within the file (ingress-compression P1b span
    * enrichment), or 0 when not computed. Approximate: the first verbatim
    * occurrence of the matched token, which for a common token may precede the
    * true FTS match — treat as a hint. Populated only when the search is asked to
    * enrich (the lossy-fold path); 0 on the default search, where the query plan
    * and result set are unchanged. */
   int line;
} code_search_hit_t;

/* Lexical search across indexed code. Returns count of results. Ranking and
 * snippet extraction stay behind the code-index implementation. */
int index_code_search(const char *query, const char *project, code_search_hit_t *out, int max);

#endif /* DEC_INDEX_H */
