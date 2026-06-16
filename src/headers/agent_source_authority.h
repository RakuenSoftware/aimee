#ifndef DEC_AGENT_SOURCE_AUTHORITY_H
#define DEC_AGENT_SOURCE_AUTHORITY_H 1

#include "aimee.h"
#include "cJSON.h"

void agent_source_add_index_freshness(cJSON *obj, const char *project, const char *file_path);
int agent_source_append_overlay_code_hits(cJSON *arr, const char *query, const char *project,
                                          int max_results);

/* Thread-local source-authority context for an in-process delegate run. Set by
 * delegate_run_ctx_enter; the code_search overlay reads it (TLS) instead of the
 * process-global AIMEE_DELEGATE_* env vars, which raced across concurrent
 * delegates. capture/restore preserve nesting on one thread. The env vars stay
 * the fallback for cross-process child clients. */
typedef struct
{
   int active;
   int authority;
   char root[MAX_PATH_LEN];
   char *paths; /* heap-owned; restore() frees the live value and adopts this */
} agent_source_authority_snapshot_t;

void agent_source_authority_tls_set(int authority, const char *worktree_root, const char *paths);
void agent_source_authority_tls_capture(agent_source_authority_snapshot_t *snap);
void agent_source_authority_tls_restore(agent_source_authority_snapshot_t *snap);

/* Export this thread's source-authority TLS to the process env. Call ONLY in a
 * post-fork/pre-exec child (single-threaded → race-free) so a re-exec'd child
 * inherits the correct context instead of a concurrently-clobbered global. */
void agent_source_authority_export_env(void);

#endif /* DEC_AGENT_SOURCE_AUTHORITY_H */
