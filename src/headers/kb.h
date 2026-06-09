/* kb.h: Project knowledge base — document chunking, embedding, and hybrid retrieval. */
#ifndef DEC_KB_H
#define DEC_KB_H 1

#include "aimee.h"

/* Maximum length of a glob pattern list (comma-separated) */
#define KB_GLOB_MAX 512

/* Default include/exclude patterns */
#define KB_DEFAULT_INCLUDE                                                                         \
   "**/*.md,**/*.txt,**/*.rst,**/*.c,**/*.h,**/*.cpp,**/*.hpp,**/*.py,**/*.go,**/*.rs,**/*.js,**/" \
   "*.ts,**/*.tsx,**/*.jsx,**/*.sh,**/*.yaml,**/*.yml,**/*.toml"
#define KB_DEFAULT_EXCLUDE "node_modules/**,vendor/**,.git/**,build/**,dist/**"

/* Chunking defaults */
#define KB_DEFAULT_CHUNK_SIZE    512 /* approximate tokens per chunk */
#define KB_DEFAULT_CHUNK_OVERLAP 64  /* token overlap between adjacent chunks */
#define KB_DEFAULT_MAX_RESULTS   3

/* kb_stats_t: summary returned by build/update operations */
typedef struct
{
   int files_scanned;
   int files_indexed;
   int files_skipped; /* up-to-date */
   int files_removed; /* deleted source files */
   int chunks_added;
   int chunks_removed;
   int embeddings_added;
} kb_stats_t;

typedef struct
{
   int pending;
   int running;
   int done;
   int failed;
   int total;
   int processed;
} kb_async_queue_stats_t;

/* Build (or rebuild) the knowledge base for a project.
 *
 * root_path:      directory to scan (or NULL for cwd)
 * project:        project label stored with each chunk (NULL → basename of root_path)
 * embedding_cmd:  embedding command used to populate the mandatory vector index
 * force_rebuild:  if non-zero, re-index all files regardless of hash
 * stats_out:      optional — receives operation counts
 *
 * Returns 0 on success, -1 on fatal error. */
int kb_build(const char *root_path, const char *project, const char *embedding_cmd,
             int force_rebuild, kb_stats_t *stats_out);

/* Incrementally update the knowledge base: re-index changed files, remove deleted ones. */
int kb_update(const char *root_path, const char *project, const char *embedding_cmd,
              kb_stats_t *stats_out);

/* Search the knowledge base.
 *
 * Returns a heap-allocated formatted string of results (caller must free).
 * Uses hybrid retrieval (DB2 lexical + pgvector dense search) with RRF fusion.
 * max_results is clamped to cfg->kb_search_max_results (default 50).
 * Returns an `error: ...` string when query embedding or the documentation
 * index is unavailable. */
char *kb_search(const char *project, const char *query, const char *embedding_cmd, int max_results);

/* Structured JSON variant of kb_search().
 *
 * Returns a heap-allocated JSON document (caller frees):
 *   {"fusion_mode":"rrf","results":[{"file_path":"...","line_start":N,"line_end":N,
 *                "heading_path":"...","content":"...","score":X.XX}, ...]}
 * score-aware modes also include "fusion_alpha":X.XX in the top-level object.
 * On failure: {"error":"<message>"}.  Empty result set: {"results":[]}.
 *
 * Fusion mode is controlled by config intelligence.kb.fusion_mode
 * ("rrf" | "static_alpha" | "dynamic_alpha"; default "rrf").
 * Same retrieval path and cap semantics as kb_search(). */
char *kb_search_json(const char *project, const char *query, const char *embedding_cmd,
                     int max_results);

/* Like kb_search_json but with an explicit per-call fusion mode override.
 * fusion_mode_override: "rrf" | "static_alpha" | "dynamic_alpha"; NULL = use config. */
char *kb_search_json_ex(const char *project, const char *query, const char *embedding_cmd,
                        int max_results, const char *fusion_mode_override);

/* Resolve the project name: if project is non-NULL/non-empty, use it.
 * Otherwise derive from the basename of root_path (or cwd). */
void kb_resolve_project(const char *project, const char *root_path, char *out, size_t out_len);

int kb_async_enabled(void);

/* Scan kb_documents for convention-source files (CONTRIBUTING.md,
 * AGENTS.md, STYLE/CODING guides, .aimee-rules, ADRs) and emit low-
 * confidence L3 candidate memories from each chunk.  Idempotent; skips
 * keys that already exist at L3 or higher.  Returns number of candidates
 * emitted, or -1 on DB error. */
int kb_extract_convention_candidates(void);

#endif /* DEC_KB_H */
