#ifndef DEC_KB_CURATOR_QUEUE_H
#define DEC_KB_CURATOR_QUEUE_H 1

#include <stdint.h>

/* Bulk-enqueue extract_doc jobs for every kb_documents row in the given
 * project that does not already have a pending/running/done extract_doc job.
 * Returns count of rows enqueued, -1 on fatal error. */
int kb_curator_queue_docs_for_project(const char *project);

/* Queue one extract_code_unit job when the curator code gate is enabled. */
int kb_curator_queue_code_unit(const char *project, const char *file_path, const char *symbol,
                               int line);

/* Bulk-enqueue extract_code_unit jobs for indexed terms in a project. */
int kb_curator_queue_code_units_for_project(const char *project, const char *root_path);

#endif /* DEC_KB_CURATOR_QUEUE_H */
