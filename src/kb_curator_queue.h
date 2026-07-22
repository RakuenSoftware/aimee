#ifndef DEC_KB_CURATOR_QUEUE_H
#define DEC_KB_CURATOR_QUEUE_H 1

#include <stdint.h>

/* Bulk-enqueue extract_doc jobs for every kb_documents row in the given
 * project that does not already have a pending/running/done extract_doc job.
 * Returns count of rows enqueued, -1 on fatal error. */
int kb_curator_queue_docs_for_project(const char *project);

/* Backfill sweep: enqueue extract_doc jobs for EVERY indexed project's un-jobbed
 * docs. The curator drain calls this so docs ingested via the drain
 * (kb_doc_refresh) -- not only the ingest route -- get curated. No-op when
 * extract_docs_enabled is 0; per-project dedup makes it idempotent. */
void kb_curator_queue_docs_all_projects(int extract_docs_enabled);

/* Queue one extract_code_unit job when the curator code gate is enabled. */
int kb_curator_queue_code_unit(const char *project, const char *file_path, const char *symbol,
                               int line);

/* Bulk-enqueue extract_code_unit jobs for indexed terms in a project. */
int kb_curator_queue_code_units_for_project(const char *project, const char *root_path);

/* Purge fan-out (webchat-project-lifecycle slice 2): delete every queued
 * kb_code_unit_jobs row for `project`, regardless of status. Returns rows
 * deleted (>=0) or -1 on DB / connection error. */
int kb_curator_code_unit_jobs_delete_project(const char *project);

/* Curator queue depth, for the /v1/health curator block (§4 observability). */
typedef struct
{
   int extract_pending; /* kb_async_jobs kind='extract_doc' status='pending' */
   int extract_done;    /* kb_async_jobs kind='extract_doc' status='done' */
   int code_unit_pending;
   int code_unit_done;
} kb_curator_queue_counts_t;

/* Fill *out with current curator queue depths (zeroed on any error / no DB). */
void kb_curator_queue_counts(kb_curator_queue_counts_t *out);

#endif /* DEC_KB_CURATOR_QUEUE_H */
