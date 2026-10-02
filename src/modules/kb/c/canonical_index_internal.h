/* canonical_index_internal.h: helpers shared between canonical_index.c and the
 * query surface split out into canonical_index_query.c.
 *
 * Private to those two translation units. The public contract stays in
 * canonical_index.h; these exist only because splitting one 2500-line file left
 * the read paths needing the same connection and id-resolution helpers the
 * write paths use. */
#ifndef DEC_CANONICAL_INDEX_INTERNAL_H
#define DEC_CANONICAL_INDEX_INTERNAL_H 1

#include <stdint.h>

/* Shared with canonical_index.c: the error-buffer size every aimee_pg_* call
 * site uses, and the dependency cap the blast-radius query fills. */
#define CI_ERRBUF   256
#define CI_MAX_DEPS 64

void *ci_conn(void);
int64_t ci_resolve_project_id(void *conn, const char *name);
int64_t ci_resolve_file_id(void *conn, int64_t project_id, const char *rel_path);

#endif /* DEC_CANONICAL_INDEX_INTERNAL_H */
