/* db2/kb_releases.h: aimee-kb corpus staging — doc_releases accessors (DB2).
 *
 * See docs/proposals/pending/aimee-kb-ingest-api-and-corpus-staging.md */
#ifndef DEC_DB2_KB_RELEASES_H
#define DEC_DB2_KB_RELEASES_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef struct
   {
      int64_t id;
      char name[128];
      char state[32];
      char promoted_at[32];
      char retired_at[32];
      char created_at[32];
   } db2_kb_release_t;

   /* Create a doc_releases row with state='pending'.
    * Returns release id (>0) on success, -1 on error (e.g. name collision). */
   int64_t db2_kb_release_create(const char *name);

   /* Read release by id. Returns 0 on success, -1 on not found / error. */
   int db2_kb_release_read(int64_t id, db2_kb_release_t *out);

   /* Set release state and optional timestamp field.
    * ts_field is "promoted_at" or "retired_at" (or NULL to skip).
    * Returns 0 on success, -1 on error. */
   int db2_kb_release_set_state(int64_t id, const char *state, const char *ts_field);

   /* Promote release id: atomically retire current active → activate id.
    * Writes active_release_id to kb_runtime_state.
    * Returns 0 on success, -1 on error. */
   int db2_kb_release_promote(int64_t id);

   /* Rollback: if target_id > 0, promote that release; otherwise promote the most
    * recently retired release. Returns 0 on success, -1 on error. */
   int db2_kb_release_rollback(int64_t target_id);

   /* Get active release id from kb_runtime_state.
    * Returns id (>0) if active, 0 if none set, -1 on error. */
   int64_t db2_kb_release_get_active(void);

   /* Add doc to a release (insert into release_docs).
    * Returns 0 on success, -1 on error. Duplicate is silently ignored. */
   int db2_kb_release_add_doc(int64_t release_id, int64_t doc_id);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_KB_RELEASES_H */
