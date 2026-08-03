/* src/kb/kb_service_code_embed.h: KB-side code embedding refresh service. */
#ifndef KB_SERVICE_CODE_EMBED_H
#define KB_SERVICE_CODE_EMBED_H 1

#include <stddef.h>
#include <stdint.h>

/* Result returned by code embedding refresh operations. */
typedef struct
{
   int accepted;              /* 1 if job accepted (may be async) */
   char job_id[128];          /* job identifier (empty for sync) */
   char project[256];         /* project name */
   char scope[64];            /* "changed_files" | "full_project" | "paths" */
   int64_t estimated_points;  /* estimated new/changed embeddings */
   int64_t skipped_unchanged; /* rows skipped due to matching content_hash */
   int64_t embedded;          /* rows actually embedded */
   int dry_run;               /* 1 if dry_run requested */
   char writer[64];           /* "kb_service" */
} kb_code_embed_result_t;

/* Build deterministic fallback text for a file from code-index facts.
 * Includes path, definitions, and bounded (≤5) calls + imports.
 * out must hold at least cap bytes.  Returns bytes written. */
int kb_code_embed_build_fallback_text(const char *project, const char *file_path, int64_t file_id,
                                      char *out, size_t cap);

/* Compute the normalized file body hash used for advisory clone grouping.
 * Empty/whitespace-only normalized bodies leave out empty. */
void kb_code_embed_normalized_file_body_hash(const char *root, const char *rel_path, char out[32]);

/* Run code embedding refresh for a project.
 * scope: "changed_files" | "full_project"
 * paths: optional array of specific file paths (NULL = all in scope)
 * path_count: number of paths (0 = all)
 * batch_size: max embeddings per batch (0 = default 128)
 * max_points: cap per project (0 = default 5000)
 * dry_run: 1 to report without writing
 * Returns 0 on success, -1 on error. */
int kb_code_embed_refresh(const char *project, const char *scope, const char **paths,
                          int path_count, int batch_size, int max_points, int dry_run,
                          kb_code_embed_result_t *out);

/* 1 when every file of |project|'s CURRENT generation has been embedded at the
 * active embedding dimension, 0 otherwise (including an unknown project, an
 * unindexed one, or a store that cannot be read).
 *
 * This is the "embedded" step of indexed -> embedded -> curated, and it is the
 * same signature kb_code_embed_refresh records when a pass completes the whole
 * file set -- deliberately one definition, because a second reader that computed
 * "fully embedded" differently would let curation start on a half-embedded
 * project exactly when the two disagreed. */
int kb_code_embed_project_fully_embedded(const char *project);

#endif /* KB_SERVICE_CODE_EMBED_H */
